#include "orchestrator/TaskOrchestrator.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>

namespace task {

namespace {
// pump() 重入保护：ModuleRunner::launch() 在输入校验失败等情况下会**同步**
// emit stageFinished，从而回调 onStageFinished -> pump()。若没有保护，
// 同一阶段会被二次启动（且仍在 m_pending 中），造成重复执行与状态错乱。
struct PumpGuard {
    bool& flag;
    explicit PumpGuard(bool& f) : flag(f) { flag = true; }
    ~PumpGuard() { flag = false; }
};
} // namespace

bool TaskOrchestrator::startTask(const QString& taskId, const PipelineDef& pipeline,
                                 const ModuleRegistry& registry, const PathResolver& resolver,
                                 const QString& dataRoot, const QJsonObject& overrides)
{
    if (m_busy) return false;   // 单任务实例：同一时刻只跑一条流水线

    m_taskId = taskId;
    m_pipeline = &pipeline;
    m_registry = &registry;
    m_resolver = resolver;
    m_dataRoot = dataRoot;
    m_overrides = overrides;
    m_maxParallel = pipeline.maxParallel < 1 ? 1 : pipeline.maxParallel;
    m_cancelled = false;
    m_startMs = QDateTime::currentMSecsSinceEpoch();
    m_records.clear();
    m_running.clear();
    m_pending = pipeline.stages;
    m_busy = true;

    // 先登记全部阶段为 pending，供 UI/状态查询看到完整 DAG
    for (const PipelineStage& s : pipeline.stages) {
        StageRecord rec;
        rec.id = s.id;
        rec.moduleId = s.moduleId;
        rec.name = s.name;
        rec.status = StageStatus::Pending;
        m_records.insert(s.id, rec);
    }

    emit notify("task.started", QJsonObject{
        {"taskId", taskId}, {"pipeline", pipeline.id},
        {"ts", QDateTime::currentMSecsSinceEpoch()}});
    pump();
    return true;
}

bool TaskOrchestrator::cancel(const QString& taskId)
{
    if (!m_busy || taskId != m_taskId) return false;
    m_cancelled = true;

    // 未启动的阶段直接置为跳过，避免残留状态让后续任务无法启动
    for (const PipelineStage& s : m_pending) {
        auto it = m_records.find(s.id);
        if (it != m_records.end()) {
            it->status = StageStatus::Skipped;
            it->errorCode = "QS-TL-CANCEL";
            it->errorMessage = "cancelled by user";
            it->finishedAt = QDateTime::currentMSecsSinceEpoch();
        }
        emit notify("task.stage.finished", QJsonObject{
            {"taskId", m_taskId}, {"stageId", s.id}, {"status", "skipped"},
            {"errorCode", "QS-TL-CANCEL"}});
    }
    m_pending.clear();

    const QList<ModuleRunner*> runners = m_running.values();
    for (ModuleRunner* r : runners) {
        if (r) r->cancel();
    }

    emit notify("task.cancelled", QJsonObject{{"taskId", taskId}});

    // 没有在跑的进程时直接收尾；否则等在跑阶段回落后由 pump() 收尾
    if (m_running.isEmpty()) pump();
    return true;
}

bool TaskOrchestrator::isReady(const PipelineStage& s) const
{
    for (const QString& d : s.dependsOn) {
        const auto it = m_records.constFind(d);
        if (it == m_records.constEnd()) return false;
        // 只有“真正跑完”的上游才能放行；被跳过/失败的上游会让下游无法满足输入契约
        if (it->status != StageStatus::Done) return false;
    }
    return true;
}

void TaskOrchestrator::setStageStatus(const PipelineStage& s, StageStatus st,
                                      const QString& errorCode, const QString& errorMessage)
{
    auto it = m_records.find(s.id);
    if (it == m_records.end()) {
        StageRecord rec;
        rec.id = s.id;
        rec.moduleId = s.moduleId;
        rec.name = s.name;
        it = m_records.insert(s.id, rec);
    }
    it->status = st;
    it->errorCode = errorCode;
    it->errorMessage = errorMessage;
    it->finishedAt = QDateTime::currentMSecsSinceEpoch();

    QJsonObject p{
        {"taskId", m_taskId}, {"stageId", s.id}, {"name", s.name}, {"module", s.moduleId},
        {"status", stageStatusName(st)},
        {"exitCode", it->exitCode},
        {"startedAt", it->startedAt},
        {"finishedAt", it->finishedAt},
        {"errorCode", it->errorCode},
        {"errorMessage", it->errorMessage},
        {"outputs", QJsonArray::fromStringList(it->outputs)}};
    emit notify("task.stage.finished", p);
}

void TaskOrchestrator::pump()
{
    if (!m_busy) return;
    if (m_pumping) return;       // 已经在调度中，交给外层循环继续推进
    PumpGuard guard(m_pumping);

    // 反复：启动就绪阶段 -> 若还有在跑的则等回调 -> 否则把上游失败/跳过的下游标 blocked
    // -> 直到没有任何阶段可以推进为止。这样既不会悬挂，也不会“误跑”缺输入的阶段。
    for (;;) {
        if (!m_cancelled) {
            while (m_running.size() < m_maxParallel) {
                int idx = -1;
                for (int i = 0; i < m_pending.size(); ++i) {
                    if (isReady(m_pending.at(i))) { idx = i; break; }
                }
                if (idx < 0) break;
                launchStage(m_pending.at(idx));      // 内部会 erase 到 m_running
            }
        }

        if (!m_running.isEmpty()) return;            // 还有阶段在跑：等 onStageFinished

        if (m_cancelled) {
            finishTask("cancelled", "cancelled by user");
            return;
        }

        if (m_pending.isEmpty()) break;              // 全部处理完，走收尾

        if (!propagateBlocked()) {
            // 依赖无法解析（配置错误 / 环）：强制收尾，绝不让任务悬挂
            for (const PipelineStage& s : m_pending) {
                setStageStatus(s, StageStatus::Failed, "QS-TL-CFG-DEADLOCK",
                               "stage cannot be scheduled: unresolved dependsOn");
            }
            m_pending.clear();
            break;
        }
        // propagateBlocked 改变了状态，回到循环开头再启动可能就绪的下游
    }

    QStringList failedStages;
    QStringList blockedStages;
    if (m_pipeline) {
        for (const PipelineStage& s : m_pipeline->stages) {
            const auto it = m_records.constFind(s.id);
            if (it == m_records.constEnd()) continue;
            if (it->status == StageStatus::Failed) failedStages << s.id;
            else if (it->errorCode.startsWith("QS-TL-DEP-")) blockedStages << s.id;
        }
    }

    QString status = "done";
    QString summary = "all stages done";
    if (!failedStages.isEmpty()) {
        status = "failed";
        summary = "failed stages: " + failedStages.join(",");
    }
    if (!blockedStages.isEmpty()) {
        summary += (summary.isEmpty() ? QString() : " | ");
        summary += "blocked: " + blockedStages.join(",");
    }
    finishTask(status, summary);
}

void TaskOrchestrator::launchStage(const PipelineStage& s)
{
    // 先出队、再启动：launch() 可能同步回调 onStageFinished -> pump()
    for (int i = 0; i < m_pending.size(); ++i) {
        if (m_pending.at(i).id == s.id) { m_pending.remove(i); break; }
    }

    const ModuleDef* def = m_registry->module(s.moduleId);
    if (!def) {
        setStageStatus(s, StageStatus::Failed, "QS-TL-CFG-UNKNOWNMODULE",
                       QString("module '%1' not registered").arg(s.moduleId));
        return;
    }

    // 可选阶段：必需输入缺失则跳过，不阻断主链（其下游按 blocked 处理）
    if (s.optional) {
        bool inputMissing = false;
        const QStringList req = m_resolver.resolveList(def->contract.requiredInputs);
        for (const QString& r : req) {
            if (!QFileInfo::exists(r)) { inputMissing = true; break; }
        }
        if (inputMissing) {
            setStageStatus(s, StageStatus::Skipped, "QS-TL-SKIP-NOINPUT",
                           "optional stage skipped: required inputs missing");
            return;
        }
    }

    auto it = m_records.find(s.id);
    if (it != m_records.end()) {
        it->status = StageStatus::Running;
        it->startedAt = QDateTime::currentMSecsSinceEpoch();
    }

    ModuleRunner* runner = new ModuleRunner(this);
    connect(runner, &ModuleRunner::stageOutput,
            this, &TaskOrchestrator::onStageOutput);
    connect(runner, &ModuleRunner::stageFinished,
            this, &TaskOrchestrator::onStageFinished);
    m_running.insert(s.id, runner);

    // 阶段参数覆盖：overrides[stageId] = {key: value}，按 "--key value" 追加
    QStringList extraArgs;
    const QJsonObject ov = m_overrides.value(s.id).toObject();
    for (auto oit = ov.begin(); oit != ov.end(); ++oit) {
        const QString k = oit.key();
        extraArgs << (k.startsWith("--") ? k : QString("--") + k)
                  << oit.value().toVariant().toString();
    }

    emit notify("task.stage.started", QJsonObject{
        {"taskId", m_taskId}, {"stageId", s.id}, {"name", s.name},
        {"module", s.moduleId}});
    runner->launch(*def, extraArgs, m_resolver, m_dataRoot, s.id, m_taskId,
                   def->timeoutSec);
}

bool TaskOrchestrator::propagateBlocked()
{
    bool any = false;
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < m_pending.size();) {
            const PipelineStage s = m_pending.at(i);
            QString blockedBy;
            QString code;
            for (const QString& d : s.dependsOn) {
                const auto it = m_records.constFind(d);
                if (it == m_records.constEnd()) continue;
                if (it->status == StageStatus::Failed) {
                    blockedBy = d;
                    code = "QS-TL-DEP-BLOCKED";
                    break;
                }
                if (it->status == StageStatus::Skipped) {
                    blockedBy = d;
                    code = "QS-TL-DEP-SKIPPED";
                    break;
                }
            }
            if (!blockedBy.isEmpty()) {
                m_pending.remove(i);
                setStageStatus(s, StageStatus::Skipped, code,
                               QString("upstream stage '%1' did not complete; stage skipped")
                                   .arg(blockedBy));
                changed = true;
                any = true;
                continue;
            }
            ++i;
        }
    }
    return any;
}

void TaskOrchestrator::onStageOutput(const QString& stageId, const QString& line)
{
    emit notify("task.stage.log", QJsonObject{
        {"taskId", m_taskId}, {"stageId", stageId}, {"line", line}});
}

void TaskOrchestrator::onStageFinished(const QString& stageId, const RunResult& result)
{
    ModuleRunner* runner = m_running.take(stageId);
    if (runner) runner->deleteLater();

    if (!m_busy) return;   // 任务已收尾（取消后迟到的回调），忽略

    auto it = m_records.find(stageId);
    if (it == m_records.end()) return;
    StageRecord& rec = *it;

    if (m_cancelled) {
        rec.status = StageStatus::Skipped;
        rec.errorCode = "QS-TL-CANCEL";
        rec.errorMessage = "cancelled by user";
    } else {
        rec.status = result.ok ? StageStatus::Done : StageStatus::Failed;
        rec.errorCode = result.errorCode;
        rec.errorMessage = result.errorMessage;
    }
    rec.exitCode = result.exitCode;
    rec.finishedAt = QDateTime::currentMSecsSinceEpoch();
    if (result.ok) rec.outputs = result.outputs;

    emit notify("task.stage.finished", QJsonObject{
        {"taskId", m_taskId}, {"stageId", stageId},
        {"module", rec.moduleId}, {"name", rec.name},
        {"status", stageStatusName(rec.status)},
        {"exitCode", rec.exitCode},
        {"startedAt", rec.startedAt},
        {"finishedAt", rec.finishedAt},
        {"errorCode", rec.errorCode},
        {"errorMessage", rec.errorMessage},
        {"outputs", QJsonArray::fromStringList(rec.outputs)}});

    pump();
}

void TaskOrchestrator::finishTask(const QString& status, const QString& summary)
{
    if (!m_busy) return;
    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_startMs;
    m_busy = false;
    m_pending.clear();

    emit notify("task.finished", QJsonObject{
        {"taskId", m_taskId}, {"status", status}, {"summary", summary},
        {"elapsedMs", elapsed}});

    clearRunners();
    m_pipeline = nullptr;
}

void TaskOrchestrator::clearRunners()
{
    const QList<ModuleRunner*> runners = m_running.values();
    for (ModuleRunner* r : runners) {
        if (!r) continue;
        r->cancel();
        r->deleteLater();
    }
    m_running.clear();
}

} // namespace task

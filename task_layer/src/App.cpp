#include "App.h"

#include <cstdio>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTextStream>
#include <QUuid>

#include "io/InputValidator.h"
#include "modules/ModuleRunner.h"

namespace task {

App::App(const QString& configDir, const QString& moduleRoot, const QString& binRoot,
         QObject* parent)
    : QObject(parent), m_configDir(configDir), m_moduleRoot(moduleRoot),
      m_binRoot(binRoot)
{
    m_orchestrator = new TaskOrchestrator(this);
    connect(m_orchestrator, &TaskOrchestrator::notify, this, &App::onNotify);
}

bool App::loadConfig()
{
    QString err;
    if (!m_registry.loadFromFile(m_configDir + "/modules.json", &err)) return false;
    if (!m_loader.loadFromFile(m_configDir + "/pipelines.json", &err)) return false;
    return true;
}

int App::serve()
{
    QString err;
    if (!m_registry.loadFromFile(m_configDir + "/modules.json", &err) ||
        !m_loader.loadFromFile(m_configDir + "/pipelines.json", &err)) {
        // 配置错误也要走协议回复，让 UI 能拿到错误码（直接写 stdout，不启动传输层）
        const QByteArray line = rpc::makeError(QJsonValue(), rpc::ErrConfig, err);
        std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
        std::fflush(stdout);
        return 2;
    }

    m_transport = new rpc::StdioTransport(this);
    connect(m_transport, &rpc::StdioTransport::requestReceived,
            this, &App::handleRequest);
    connect(m_transport, &rpc::StdioTransport::inputClosed, this, [this] {
        // UI 侧管道关闭：收尾并退出，避免任务层变成僵尸进程
        const QString running = m_orchestrator->currentTaskId();
        if (!running.isEmpty()) m_orchestrator->cancel(running);
        emit exitRequested(0);
    });
    connect(this, &App::exitRequested, this, [](int code) {
        QCoreApplication::exit(code);
    });
    return 0;
}

int App::runOnce(const QString& pipelineId, const QString& dataRoot,
                 const QJsonObject& overrides, const QJsonObject& inputs)
{
    QString err;
    if (!m_registry.loadFromFile(m_configDir + "/modules.json", &err) ||
        !m_loader.loadFromFile(m_configDir + "/pipelines.json", &err)) {
        QTextStream(stdout) << "config error: " << err << "\n";
        return 2;
    }
    const PipelineDef* pipe = m_loader.pipeline(pipelineId);
    if (!pipe) {
        QTextStream(stdout) << "unknown pipeline: " << pipelineId << "\n";
        return 2;
    }
    const QString taskId = QStringLiteral("cli_%1")
                               .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    // 数据根必须绝对化：模块以 dataRoot 为工作目录启动，相对路径会被二次拼接
    const QString root = QDir::cleanPath(QFileInfo(dataRoot).absoluteFilePath());
    m_activeTaskId = taskId;
    m_activeDataRoot = root;
    m_activeTaskDir = root + "/Result/tasks/" + taskId;
    m_activeInputs = inputs;
    m_resolver.setBase(root, m_moduleRoot, m_binRoot, m_activeTaskDir);
    m_resolver.setInputs(inputs);
    m_store.open(m_activeTaskDir, taskId, pipelineId);
    m_store.setInputs(m_resolver.resolvedInputs());

    // CLI 模式下把通知打到 stdout 便于肉眼观察（与 onNotify 的持久化并存）
    connect(m_orchestrator, &TaskOrchestrator::notify, this,
            [](const QString& m, const QJsonObject& p) {
        if (m == "task.stage.log") return;   // 日志量大，CLI 下过滤
        QTextStream(stdout) << "[event] " << m << " "
                            << QString::fromUtf8(QJsonDocument(p).toJson(QJsonDocument::Compact))
                            << "\n";
        QTextStream(stdout).flush();
    });

    if (!m_orchestrator->startTask(taskId, *pipe, m_registry, m_resolver, root, overrides)) {
        QTextStream(stdout) << "task failed to start (another task running?)\n";
        return 1;
    }
    return 0;   // 事件循环由 main 驱动，结束后由 task.finished 通知退出
}

int App::validateOnce(const QString& dataRoot, const QString& pipelineId,
                      const QJsonObject& inputs)
{
    if (!loadConfig()) {
        QTextStream(stdout) << "config error\n";
        return 2;
    }
    const QJsonObject rep = inspectInputs(dataRoot, pipelineId, QString(), inputs);
    QTextStream(stdout) << QString::fromUtf8(QJsonDocument(rep).toJson(QJsonDocument::Indented));
    QTextStream(stdout).flush();
    return rep.value("valid").toBool() ? 0 : 1;
}

QJsonObject App::inspectInputs(const QString& dataRoot, const QString& pipelineId,
                               const QString& moduleId, const QJsonObject& inputs)
{
    const QString root = QDir::cleanPath(QFileInfo(dataRoot).absoluteFilePath());
    PathResolver r;
    r.setBase(root, m_moduleRoot, m_binRoot,
              QDir(root).filePath("Result/tasks/_validate"));
    r.setInputs(inputs);

    // 只校验“入口阶段”（无 dependsOn）的输入契约：这是“这条流水线能否启动”的判据。
    // 下游阶段的输入由上游阶段产出，运行前不可能存在。
    // 指定 module 时（单模块执行）直接校验该模块的契约。
    QStringList entryModuleIds;
    if (!moduleId.isEmpty()) {
        entryModuleIds << moduleId;
    } else if (!pipelineId.isEmpty()) {
        const PipelineDef* p = m_loader.pipeline(pipelineId);
        if (p) {
            for (const PipelineStage& s : p->stages) {
                if (s.dependsOn.isEmpty()) entryModuleIds << s.moduleId;
            }
        }
    } else {
        for (const PipelineDef& p : m_loader.allPipelines()) {
            for (const PipelineStage& s : p.stages) {
                if (s.dependsOn.isEmpty()) entryModuleIds << s.moduleId;
            }
        }
    }

    QVector<InputIssue> issues;
    QStringList warnings;

    // UI 动态指定的 port.json（task.run / inputs.validate 的 inputs.portJson）
    const bool hasPortJson = r.hasVar("{PORT_JSON}");
    const QString portJson = hasPortJson ? r.resolve("{PORT_JSON}") : QString();

    QSet<QString> seen;
    QStringList required;
    for (const QString& mid : entryModuleIds) {
        const ModuleDef* d = m_registry.module(mid);
        if (!d) continue;
        for (const QString& path : r.resolveList(d->contract.requiredInputs)) {
            // 已显式提供 port.json 时，入口契约里的 port.json 被动态输入取代：
            // 把它真正落到 Model 目录是交接阶段的职责，此处只记 warning，不判缺失。
            if (hasPortJson && path.endsWith("port.json", Qt::CaseInsensitive)) {
                warnings << QString("required input '%1' superseded by UI-provided portJson: %2")
                                .arg(path, portJson);
                continue;
            }
            if (seen.contains(path)) continue;
            seen.insert(path);
            required << path;
        }
    }

    bool ok = InputValidator::validate(required, {}, {}, &issues, &warnings);

    if (hasPortJson) {
        QString detail;
        if (!InputValidator::fileExistsNonEmpty(portJson, &detail)) {
            ok = false;
            issues.push_back(InputIssue{portJson, "missing", detail});
        } else if (!InputValidator::validatePortFile(portJson, r.resolve("{Model}"),
                                                     &issues, &warnings)) {
            ok = false;
        }
        required << portJson;
    }

    const QString modelDir = r.resolve("{Model}");
    const QString modelPort = QDir::cleanPath(QDir(modelDir).filePath("port.json"));
    // UI 指定的 port.json 与 Model/port.json 是同一个文件时，跳过重复校验
    if (QFileInfo::exists(modelDir) &&
        (!hasPortJson || QDir::cleanPath(portJson) != modelPort)) {
        if (!InputValidator::validatePortBinding(modelDir, &issues, &warnings)) ok = false;
    }

    QJsonArray issueArr;
    for (const InputIssue& i : issues)
        issueArr.append(QJsonObject{{"path", i.path}, {"reason", i.reason}, {"detail", i.detail}});
    QJsonArray warnArr;
    for (const QString& w : warnings) warnArr.append(w);
    QJsonArray checkedArr;
    for (const QString& p : required) checkedArr.append(p);

    QJsonObject rep{{"valid", ok},
                    {"dataRoot", root},
                    {"checked", checkedArr},
                    {"issues", issueArr},
                    {"warnings", warnArr}};
    const QJsonObject resolved = r.resolvedInputs();
    if (!resolved.isEmpty()) rep["inputs"] = resolved;
    return rep;
}

void App::onNotify(const QString& method, const QJsonObject& params)
{
    // 同步 ResultStore 状态并持久化
    if (method == "task.stage.log") {
        const QString line = params.value("line").toString();
        m_store.appendLog(QString("[%1] %2")
                              .arg(params.value("stageId").toString(), line.left(4000)));
    } else if (method == "task.stage.started") {
        StageRecord rec;
        rec.id = params.value("stageId").toString();
        rec.moduleId = params.value("module").toString();
        rec.name = params.value("name").toString();
        rec.status = StageStatus::Running;
        rec.startedAt = QDateTime::currentMSecsSinceEpoch();
        m_store.setStage(rec);
        m_store.appendLog(QString("stage %1 started (%2)").arg(rec.id, rec.name));
    } else if (method == "task.stage.finished") {
        StageRecord rec;
        rec.id = params.value("stageId").toString();
        rec.moduleId = params.value("module").toString();
        rec.name = params.value("name").toString();
        const QString st = params.value("status").toString();
        rec.status = st == "done" ? StageStatus::Done
                   : st == "skipped" ? StageStatus::Skipped
                   : st == "pending" ? StageStatus::Pending
                   : StageStatus::Failed;
        rec.exitCode = params.value("exitCode").toInt();
        rec.errorCode = params.value("errorCode").toString();
        rec.errorMessage = params.value("errorMessage").toString();
        // 保留启动时间（阶段记录是整体替换的）
        rec.startedAt = params.value("startedAt").toVariant().toLongLong();
        rec.finishedAt = params.value("finishedAt").toVariant().toLongLong();
        if (rec.finishedAt == 0) rec.finishedAt = QDateTime::currentMSecsSinceEpoch();
        for (const QJsonValue& o : params.value("outputs").toArray())
            rec.outputs << o.toString();
        m_store.setStage(rec);
        m_store.appendLog(QString("stage %1 %2 exit=%3 %4")
                              .arg(rec.id, st)
                              .arg(rec.exitCode)
                              .arg(rec.errorCode.isEmpty() ? QString() : rec.errorCode));
    } else if (method == "task.finished") {
        const QString status = params.value("status").toString();
        m_store.finish(status, params.value("summary").toString(),
                       params.value("elapsedMs").toVariant().toLongLong());
        // 保留最近任务的目录，UI 在任务结束后仍可查 task.status / task.result
        m_lastTaskId = params.value("taskId").toString();
        m_lastTaskDir = m_activeTaskDir;
        m_activeTaskId.clear();
        m_activeTaskDir.clear();
        const bool isCli = m_lastTaskId.startsWith("cli_");
        if (isCli) emit exitRequested(status == "done" ? 0 : 1);
    }

    // 转发给 UI（serve 模式）
    if (m_transport) m_transport->notify(method, params);
}

void App::handleRequest(const QJsonValue& reqId, const QString& method,
                        const QJsonObject& params)
{
    if (method == "ping") rpcPing(reqId);
    else if (method == "modules.list") rpcListModules(reqId);
    else if (method == "pipelines.list") rpcListPipelines(reqId);
    else if (method == "inputs.validate") rpcValidateInputs(reqId, params);
    else if (method == "task.run") rpcRunTask(reqId, params);
    else if (method == "module.run") rpcRunModule(reqId, params);
    else if (method == "task.cancel") rpcCancelTask(reqId, params);
    else if (method == "task.status") rpcTaskStatus(reqId, params);
    else if (method == "task.result") rpcTaskResult(reqId, params);
    else if (method == "shutdown") rpcShutdown(reqId);
    else m_transport->sendError(reqId, rpc::MethodNotFound, "unknown method: " + method);
}

void App::rpcPing(const QJsonValue& id)
{
    m_transport->sendResponse(id, QJsonObject{{"pong", true},
                                               {"app", "task_layer"},
                                               {"schemaVersion", 1}});
}

void App::rpcListModules(const QJsonValue& id)
{
    // 可用性探测用的 resolver：模块路径只依赖 {MODULE_ROOT}/{BIN}，
    // 与具体算例无关，因此 dataRoot 用当前值（可能为空）即可。
    PathResolver r;
    r.setBase(m_activeDataRoot.isEmpty() ? QDir::rootPath() : m_activeDataRoot,
              m_moduleRoot, m_binRoot, QString());

    QJsonArray arr;
    for (const ModuleDef& d : m_registry.allModules()) {
        QJsonObject o;
        o["id"] = d.id;
        o["displayName"] = d.displayName;
        const bool isDll = d.kind == ModuleKind::DllWorker;
        const bool isInternal = d.kind == ModuleKind::Internal;
        o["kind"] = isDll ? "dll" : (isInternal ? "internal" : "external");
        o["command"] = d.command;
        // available：当前部署下能否真的执行。UI 据此区分"已注册"与"可执行"。
        // internal 是任务层内建动作，没有实体文件，可执行性与部署无关，恒为 true
        // ——否则 UI 会把 internal.handoff（刷新端口绑定）误判为不可用而过滤掉。
        QString exe;
        if (!isInternal) exe = ModuleRunner::locateCommand(d, r);
        o["available"] = isInternal ? true : !exe.isEmpty();
        o["resolvedPath"] = exe;
        arr.append(o);
    }
    m_transport->sendResponse(id, QJsonObject{{"modules", arr}});
}

void App::rpcListPipelines(const QJsonValue& id)
{
    QJsonArray arr;
    for (const PipelineDef& p : m_loader.allPipelines()) {
        QJsonObject o;
        o["id"] = p.id;
        o["displayName"] = p.displayName;
        o["description"] = p.description;
        o["maxParallel"] = p.maxParallel;
        QJsonArray stages;
        for (const PipelineStage& s : p.stages) {
            QJsonObject so;
            so["id"] = s.id;
            so["module"] = s.moduleId;
            so["name"] = s.name;
            so["optional"] = s.optional;
            QJsonArray deps;
            for (const QString& d : s.dependsOn) deps.append(d);
            so["dependsOn"] = deps;
            stages.append(so);
        }
        o["stages"] = stages;
        arr.append(o);
    }
    m_transport->sendResponse(id, QJsonObject{{"pipelines", arr}});
}

void App::rpcValidateInputs(const QJsonValue& id, const QJsonObject& params)
{
    const QString dataRoot = params.value("dataRoot").toString();
    if (dataRoot.isEmpty()) {
        m_transport->sendError(id, rpc::InvalidParams, "missing dataRoot");
        return;
    }
    m_transport->sendResponse(id, inspectInputs(dataRoot, params.value("pipeline").toString(),
                                                params.value("module").toString(),
                                                params.value("inputs").toObject()));
}

void App::rpcRunTask(const QJsonValue& id, const QJsonObject& params)
{
    const QString pipelineId = params.value("pipeline").toString();
    const PipelineDef* pipe = m_loader.pipeline(pipelineId);
    if (!pipe) {
        m_transport->sendError(id, rpc::InvalidParams,
                               "unknown pipeline: " + pipelineId);
        return;
    }
    startTaskWith(id, *pipe, params.value("dataRoot").toString(), params);
}

void App::rpcRunModule(const QJsonValue& id, const QJsonObject& params)
{
    const QString moduleId = params.value("moduleId").toString();
    const ModuleDef* def = m_registry.module(moduleId);
    if (!def) {
        m_transport->sendError(id, rpc::InvalidParams, "unknown module: " + moduleId);
        return;
    }

    // 单模块执行 = 只含该模块的"单阶段流水线"：
    // 进度通知、状态持久化、task.status / task.result / task.cancel 全部复用流水线那套。
    PipelineDef single;
    single.id = QStringLiteral("single:%1").arg(moduleId);
    single.displayName = def->displayName;
    single.description = QStringLiteral("单模块执行（module.run）");
    single.maxParallel = 1;
    PipelineStage st;
    st.id = def->id;
    st.moduleId = def->id;
    st.name = def->displayName;
    single.stages.append(st);

    startTaskWith(id, single, params.value("dataRoot").toString(), params);
}

bool App::startTaskWith(const QJsonValue& id, const PipelineDef& pipe,
                        const QString& dataRoot, const QJsonObject& params)
{
    if (dataRoot.isEmpty()) {
        m_transport->sendError(id, rpc::InvalidParams, "missing dataRoot");
        return false;
    }
    if (!m_activeTaskId.isEmpty()) {
        m_transport->sendError(id, rpc::ErrTaskRunning,
                               "another task is running: " + m_activeTaskId);
        return false;
    }
    const QString taskId = params.value("taskId").toString();
    const QString tid = taskId.isEmpty()
        ? QStringLiteral("t%1").arg(QDateTime::currentMSecsSinceEpoch()) : taskId;

    m_activeTaskId = tid;
    const QString root = QDir::cleanPath(QFileInfo(dataRoot).absoluteFilePath());
    m_activeDataRoot = root;
    m_activeTaskDir = QDir(root).filePath("Result/tasks/" + tid);
    m_activeInputs = params.value("inputs").toObject();
    m_resolver.setBase(root, m_moduleRoot, m_binRoot, m_activeTaskDir);
    m_resolver.setInputs(m_activeInputs);
    m_store.open(m_activeTaskDir, tid, pipe.id);
    m_store.setInputs(m_resolver.resolvedInputs());

    const QJsonObject overrides = params.value("overrides").toObject();
    if (!m_orchestrator->startTask(tid, pipe, m_registry, m_resolver, root, overrides)) {
        m_activeTaskId.clear();
        m_activeTaskDir.clear();
        m_activeInputs = QJsonObject();
        m_transport->sendError(id, rpc::InternalError, "task failed to start");
        return false;
    }
    m_transport->sendResponse(id, QJsonObject{{"taskId", tid}, {"pipeline", pipe.id}});
    return true;
}

void App::rpcCancelTask(const QJsonValue& id, const QJsonObject& params)
{
    const QString taskId = params.value("taskId").toString();
    if (taskId != m_activeTaskId || !m_orchestrator->cancel(taskId)) {
        m_transport->sendError(id, rpc::ErrTaskNotFound, "task not running: " + taskId);
        return;
    }
    m_transport->sendResponse(id, QJsonObject{{"cancelled", true}});
}

QString App::taskDirOf(const QString& taskId) const
{
    if (!taskId.isEmpty() && taskId == m_activeTaskId && !m_activeTaskDir.isEmpty())
        return m_activeTaskDir;
    if (!taskId.isEmpty() && taskId == m_lastTaskId && !m_lastTaskDir.isEmpty())
        return m_lastTaskDir;
    return QString();
}

void App::rpcTaskStatus(const QJsonValue& id, const QJsonObject& params)
{
    const QString taskId = params.value("taskId").toString();
    QString dir = taskDirOf(taskId);
    if (dir.isEmpty()) {
        const QString dataRoot = params.value("dataRoot").toString();
        if (!dataRoot.isEmpty())
            dir = QDir(dataRoot).filePath("Result/tasks/" + taskId);
    }
    QJsonObject state;
    if (dir.isEmpty() || !ResultStore::readTaskState(dir, &state, nullptr)) {
        m_transport->sendError(id, rpc::ErrTaskNotFound, "no state for task: " + taskId);
        return;
    }
    m_transport->sendResponse(id, state);
}

void App::rpcTaskResult(const QJsonValue& id, const QJsonObject& params)
{
    const QString taskId = params.value("taskId").toString();
    QString dir = taskDirOf(taskId);
    if (dir.isEmpty()) {
        const QString dataRoot = params.value("dataRoot").toString();
        if (!dataRoot.isEmpty())
            dir = QDir(dataRoot).filePath("Result/tasks/" + taskId);
    }
    QJsonObject state;
    if (dir.isEmpty() || !ResultStore::readTaskState(dir, &state, nullptr)) {
        m_transport->sendError(id, rpc::ErrTaskNotFound, "no state for task: " + taskId);
        return;
    }
    // 汇总结果文件清单
    QJsonArray files;
    for (const QJsonValue& sv : state.value("stages").toArray()) {
        for (const QJsonValue& o : sv.toObject().value("outputs").toArray())
            files.append(o);
    }
    QJsonObject out = state;
    out["taskDir"] = dir;
    out["resultFiles"] = files;
    m_transport->sendResponse(id, out);
}

void App::rpcShutdown(const QJsonValue& id)
{
    m_transport->sendResponse(id, QJsonObject{{"bye", true}});
    m_transport->flush();
    const QString running = m_orchestrator->currentTaskId();
    if (!running.isEmpty()) m_orchestrator->cancel(running);
    emit exitRequested(0);
}

} // namespace task

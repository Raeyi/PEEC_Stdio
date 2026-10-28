// TaskOrchestrator.h — 任务编排核心（DAG 调度：就绪优先 + 最大并行）
#pragma once

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>

#include "config/ModuleRegistry.h"
#include "config/PipelineLoader.h"
#include "io/ResultStore.h"
#include "modules/ModuleRunner.h"

namespace task {

class TaskOrchestrator : public QObject
{
    Q_OBJECT
public:
    explicit TaskOrchestrator(QObject* parent = nullptr) : QObject(parent) {}

    // 运行一个任务。dataRoot：案例目录；overrides：阶段参数覆盖（stageId -> {key:value}）
    bool startTask(const QString& taskId, const PipelineDef& pipeline,
                   const ModuleRegistry& registry, const PathResolver& resolver,
                   const QString& dataRoot, const QJsonObject& overrides);

    bool cancel(const QString& taskId);
    bool isBusy() const { return m_busy; }
    QString currentTaskId() const { return m_busy ? m_taskId : QString(); }

signals:
    void notify(const QString& method, const QJsonObject& params);  // 转发给 UI 的通知

private slots:
    void onStageOutput(const QString& stageId, const QString& line);
    void onStageFinished(const QString& stageId, const RunResult& result);

private:
    void pump();                                  // 从就绪队列启动可用阶段
    void launchStage(const PipelineStage& s);      // 启动单个阶段（先出队）
    bool isReady(const PipelineStage& s) const;    // 依赖全部 done
    bool propagateBlocked();                      // 上游失败/跳过 -> 下游标记 blocked
    void finishTask(const QString& status, const QString& summary);
    void clearRunners();
    void setStageStatus(const PipelineStage& s, StageStatus st, const QString& errorCode,
                        const QString& errorMessage);

    QString m_taskId;
    const PipelineDef* m_pipeline = nullptr;
    const ModuleRegistry* m_registry = nullptr;
    PathResolver m_resolver;
    QString m_dataRoot;
    QJsonObject m_overrides;

    QMap<QString, StageRecord> m_records;      // stageId -> 运行记录
    QMap<QString, ModuleRunner*> m_running;    // stageId -> 正在运行的 runner
    QVector<PipelineStage> m_pending;          // 尚未就绪的阶段
    int m_maxParallel = 1;
    bool m_cancelled = false;
    bool m_pumping = false;                    // 防重入：launch 可能同步回调 stageFinished
    bool m_busy = false;
    qint64 m_startMs = 0;
};

} // namespace task

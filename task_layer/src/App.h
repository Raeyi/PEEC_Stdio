// App.h — 任务层主应用：RPC 路由 + 命令路由 + 编排器桥接 + 结果持久化
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include "config/ModuleRegistry.h"
#include "config/PipelineLoader.h"
#include "io/ResultStore.h"
#include "orchestrator/TaskOrchestrator.h"
#include "rpc/StdioTransport.h"

namespace task {

class App : public QObject
{
    Q_OBJECT
public:
    App(const QString& configDir, const QString& moduleRoot, const QString& binRoot,
        QObject* parent = nullptr);

    // serve 模式：stdio RPC 循环（返回值为启动结果，0=已进入服务）
    int serve();
    // run 模式：执行一次 pipeline 后退出（返回 0=已启动，非 0=未启动）
    int runOnce(const QString& pipelineId, const QString& dataRoot,
                const QJsonObject& overrides, const QJsonObject& inputs = QJsonObject());
    // validate 模式：只做输入检查，打印 JSON
    int validateOnce(const QString& dataRoot, const QString& pipelineId,
                     const QJsonObject& inputs = QJsonObject());

signals:
    void exitRequested(int code);

private:
    bool loadConfig();
    void handleRequest(const QJsonValue& reqId, const QString& method,
                       const QJsonObject& params);
    void onNotify(const QString& method, const QJsonObject& params);

    // RPC 方法实现
    void rpcPing(const QJsonValue& id);
    void rpcListModules(const QJsonValue& id);
    void rpcListPipelines(const QJsonValue& id);
    void rpcValidateInputs(const QJsonValue& id, const QJsonObject& params);
    void rpcRunTask(const QJsonValue& id, const QJsonObject& params);
    // 单模块执行（不走流水线）：内部构造"只含该模块的单阶段流水线"，
    // 因此状态管理、通知、task.status/task.result 与流水线完全一致。
    void rpcRunModule(const QJsonValue& id, const QJsonObject& params);
    // 启动公共逻辑：busy 检查 → 建任务目录 → resolver/store → 交给编排器
    bool startTaskWith(const QJsonValue& id, const PipelineDef& pipe,
                       const QString& dataRoot, const QJsonObject& params);
    void rpcCancelTask(const QJsonValue& id, const QJsonObject& params);
    void rpcTaskStatus(const QJsonValue& id, const QJsonObject& params);
    void rpcTaskResult(const QJsonValue& id, const QJsonObject& params);
    void rpcShutdown(const QJsonValue& id);

    // 已结束任务也要能查：优先活动任务，其次最近一次任务
    QString taskDirOf(const QString& taskId) const;
    // 按 pipeline（或单个 module，或全部模块）的契约做输入检查；
    // inputs 为 UI 动态输入（如 portJson 路径）
    QJsonObject inspectInputs(const QString& dataRoot, const QString& pipelineId,
                              const QString& moduleId, const QJsonObject& inputs);

    QString m_configDir;
    QString m_moduleRoot;
    QString m_binRoot;
    ModuleRegistry m_registry;
    PipelineLoader m_loader;
    rpc::StdioTransport* m_transport = nullptr;
    TaskOrchestrator* m_orchestrator = nullptr;
    PathResolver m_resolver;
    QString m_activeTaskId;
    QString m_activeTaskDir;      // Result/tasks/<taskId>/
    QString m_activeDataRoot;
    QJsonObject m_activeInputs;   // UI 动态输入（{PORT_JSON} 等）
    QString m_lastTaskId;
    QString m_lastTaskDir;
    ResultStore m_store;
};

} // namespace task

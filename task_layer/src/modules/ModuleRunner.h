// ModuleRunner.h — 模块执行（external exe / DLL worker 两种形态）
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QProcess>
#include <QStringList>
#include <QVector>

#include "config/ModuleRegistry.h"
#include "config/PipelineLoader.h"

namespace task {

struct RunResult {
    bool ok = false;
    int exitCode = -1;
    QString errorCode;      // 机器错误码（QS-TL-* 或模块自身 QS-<域>-*）
    QString errorMessage;   // 用户可见
    QStringList outputs;    // 输出校验通过的文件
};

// 单阶段执行器：负责拼装命令、解析路径、启动进程、校验输出
class ModuleRunner : public QObject
{
    Q_OBJECT
public:
    explicit ModuleRunner(QObject* parent = nullptr) : QObject(parent) {}
    ~ModuleRunner() override;

    // 启动一个阶段（外部 exe 或 DLL worker）。调用方需连接信号。
    void launch(const ModuleDef& def, const QStringList& args,
                const PathResolver& resolver, const QString& dataRoot,
                const QString& stageId, const QString& taskId, int timeoutSec);

    // 取消：终止正在运行的进程（触发 finished，走取消路径）
    void cancel();

    // 按 searchDirs 定位模块可执行文件（DLL 形态同理定位其 dll）；
    // 返回空表示当前部署下找不到，可用于可用性预检。
    static QString locateCommand(const ModuleDef& def, const PathResolver& resolver);

signals:
    void stageOutput(const QString& stageId, const QString& line);   // 实时 stdout/stderr
    void stageFinished(const QString& stageId, const RunResult& result);

private slots:
    void onProcessOutput();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);

private:
    // 写 DLL 模块的任务定义 JSON（worker_host --job 用）
    QString writeJobJson(const ModuleDef& def, const QStringList& args,
                         const PathResolver& resolver, const QString& dataRoot,
                         const QString& stageId, const QString& taskId);
    void finishFailure(const QString& code, const QString& message);
    void finishSuccess(const QStringList& outputs);

    // 内建阶段（kind: internal）：不启动子进程，在任务层内完成数据交接/格式适配。
    // command 为动作名，目前支持 gds2em_handoff。
    void runInternal(const ModuleDef& def);
    bool handoffGdsToEm(const PathResolver& resolver);   // gds2geo 产物 → EM 输入
    static QString sha256HexOfFile(const QString& path);

    QProcess* m_proc = nullptr;
    ModuleDef m_def;
    PathResolver m_resolver;
    QString m_stageId;
    QString m_taskId;
    QString m_dataRoot;
    RunResult m_result;
    QByteArray m_tail;          // 滚动保留最近一段原始输出（用于失败时提取模块错误码）
    int m_timeoutSec = 0;
    bool m_cancelled = false;
    bool m_timedOut = false;
    bool m_finished = false;
};

} // namespace task

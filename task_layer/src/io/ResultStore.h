// ResultStore.h — 状态与结果管理（写 task 状态 JSON、结果清单、日志）
#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace task {

enum class StageStatus {
    Pending, Ready, Running, Done, Failed, Skipped, NotConfigured
};

QString stageStatusName(StageStatus s);

struct StageRecord {
    QString id;
    QString moduleId;
    QString name;
    StageStatus status = StageStatus::Pending;
    int exitCode = 0;
    qint64 startedAt = 0;
    qint64 finishedAt = 0;
    QStringList outputs;     // 本阶段产出文件（校验通过的 outputChecks）
    QString errorCode;       // 如 QS-TL-MOD-EXEC
    QString errorMessage;
};

class ResultStore
{
public:
    // taskDir = Result/tasks/<taskId>/
    void open(const QString& taskDir, const QString& taskId, const QString& pipelineId);

    void appendLog(const QString& line);      // 追加到 task.log，同时返回给 UI 的通知由调用方发
    void setStage(const StageRecord& rec);    // 更新/插入阶段记录并落盘
    void finish(const QString& status, const QString& summary, qint64 elapsedMs);
    // UI 动态输入（如 port.json 路径），落盘便于任务追溯
    void setInputs(const QJsonObject& inputs) { m_inputs = inputs; persist(); }

    QString taskDir() const { return m_dir; }
    QString taskId() const { return m_taskId; }

    // 读取指定任务状态（供 status/result 查询）
    static bool readTaskState(const QString& taskDir, QJsonObject* out, QString* error);

private:
    void persist();
    QString m_dir;
    QString m_taskId;
    QString m_pipelineId;
    QVector<StageRecord> m_stages;
    QJsonObject m_inputs;
    QString m_status = "queued";
    QString m_summary;
    QDateTime m_started;
    bool m_finished = false;
    qint64 m_elapsedMs = 0;
};

} // namespace task

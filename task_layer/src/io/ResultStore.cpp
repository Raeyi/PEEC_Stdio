#include "io/ResultStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTextStream>

namespace task {

QString stageStatusName(StageStatus s)
{
    switch (s) {
    case StageStatus::Pending: return "pending";
    case StageStatus::Ready: return "ready";
    case StageStatus::Running: return "running";
    case StageStatus::Done: return "done";
    case StageStatus::Failed: return "failed";
    case StageStatus::Skipped: return "skipped";
    case StageStatus::NotConfigured: return "notConfigured";
    }
    return "pending";
}

void ResultStore::open(const QString& taskDir, const QString& taskId, const QString& pipelineId)
{
    m_dir = taskDir;
    m_taskId = taskId;
    m_pipelineId = pipelineId;
    m_status = "queued";
    m_finished = false;
    m_elapsedMs = 0;
    m_stages.clear();
    m_inputs = QJsonObject();
    m_started = QDateTime::currentDateTime();
    QDir().mkpath(m_dir);
    appendLog(QString("task %1 pipeline=%2 started at %3")
                  .arg(taskId, pipelineId, m_started.toString(Qt::ISODate)));
}

void ResultStore::appendLog(const QString& line)
{
    QFile f(m_dir + "/task.log");
    if (f.open(QIODevice::Append | QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream ts(&f);
        ts << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz")
           << "  " << line << "\n";
    }
}

void ResultStore::setStage(const StageRecord& rec)
{
    bool replaced = false;
    for (StageRecord& s : m_stages) {
        if (s.id == rec.id) { s = rec; replaced = true; break; }
    }
    if (!replaced) m_stages << rec;
    persist();
}

void ResultStore::finish(const QString& status, const QString& summary, qint64 elapsedMs)
{
    m_status = status;
    m_summary = summary;
    m_finished = true;
    m_elapsedMs = elapsedMs;
    appendLog(QString("task finished status=%1 elapsedMs=%2 summary=%3")
                  .arg(status).arg(elapsedMs).arg(summary));
    persist();
}

void ResultStore::persist()
{
    QJsonObject root;
    root["taskId"] = m_taskId;
    root["pipelineId"] = m_pipelineId;
    root["status"] = m_status;
    root["summary"] = m_summary;
    root["startedAt"] = m_started.toMSecsSinceEpoch();
    // finishedAt 只在任务真正进入终态时写入，避免 queued/running 阶段谎报“已结束”
    root["finishedAt"] = m_finished ? QDateTime::currentDateTime().toMSecsSinceEpoch() : 0;
    root["elapsedMs"] = m_elapsedMs;
    if (!m_inputs.isEmpty()) root["inputs"] = m_inputs;
    QJsonArray stages;
    for (const StageRecord& s : m_stages) {
        QJsonObject so;
        so["id"] = s.id;
        so["module"] = s.moduleId;
        so["name"] = s.name;
        so["status"] = stageStatusName(s.status);
        so["exitCode"] = s.exitCode;
        so["startedAt"] = s.startedAt;
        so["finishedAt"] = s.finishedAt;
        QJsonArray out;
        for (const QString& o : s.outputs) out.append(o);
        so["outputs"] = out;
        so["errorCode"] = s.errorCode;
        so["errorMessage"] = s.errorMessage;
        stages.append(so);
    }
    root["stages"] = stages;

    QFile f(m_dir + "/state.json");
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    }
}

bool ResultStore::readTaskState(const QString& taskDir, QJsonObject* out, QString* error)
{
    QFile f(taskDir + "/state.json");
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QString("no state.json under %1").arg(taskDir);
        return false;
    }
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QString("state.json parse error: %1").arg(perr.errorString());
        return false;
    }
    *out = doc.object();
    return true;
}

} // namespace task

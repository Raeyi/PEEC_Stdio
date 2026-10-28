#include "config/PipelineLoader.h"

#include <functional>

#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

namespace task {

void PathResolver::setBase(const QString& dataRoot, const QString& moduleRoot,
                           const QString& binRoot, const QString& taskDir)
{
    m_baseVars.clear();
    m_baseVars["{DATA}"] = dataRoot;
    m_baseVars["{Model}"] = dataRoot + "/Model";
    m_baseVars["{Setting}"] = dataRoot + "/Setting";
    m_baseVars["{Result}"] = dataRoot + "/Result";
    m_baseVars["{BIN}"] = binRoot;
    m_baseVars["{MODULE_ROOT}"] = moduleRoot;
    m_baseVars["{CASE_ROOT}"] = dataRoot;
    m_baseVars["{TASK_DIR}"] = taskDir;
    m_baseVars["{TASK_JSON}"] = taskDir + "/task.json";
}

QString PathResolver::varNameFromKey(const QString& key)
{
    // 驼峰转大写下划线：portJson -> PORT_JSON，setFile -> SET_FILE
    QString out;
    for (int i = 0; i < key.size(); ++i) {
        const QChar c = key.at(i);
        if (i > 0 && c.isUpper() && !key.at(i - 1).isUpper()) out += '_';
        out += c.toUpper();
    }
    return out;
}

void PathResolver::setInputs(const QJsonObject& inputs)
{
    m_inputs = inputs;
    m_extraVars.clear();
    for (auto it = inputs.begin(); it != inputs.end(); ++it) {
        const QString raw = it.value().toString().trimmed();
        if (raw.isEmpty()) continue;
        const QString name = varNameFromKey(it.key());
        m_extraVars["{" + name + "}"] = raw;
        m_extraVars["{" + name.toUpper() + "}"] = raw;   // 兼容直接写全大写键
    }
}

bool PathResolver::hasVar(const QString& name) const
{
    return m_baseVars.contains(name) || m_extraVars.contains(name);
}

QJsonObject PathResolver::resolvedInputs() const
{
    QJsonObject out;
    for (auto it = m_inputs.begin(); it != m_inputs.end(); ++it)
        out[it.key()] = resolve(it.value().toString().trimmed());
    return out;
}

QString PathResolver::resolve(const QString& expr) const
{
    QString out = expr;
    // 先展开动态变量（其值可能本身含 {DATA} 等固定变量），再展开固定变量
    for (auto it = m_extraVars.cbegin(); it != m_extraVars.cend(); ++it)
        out.replace(it.key(), it.value());
    for (auto it = m_baseVars.cbegin(); it != m_baseVars.cend(); ++it)
        out.replace(it.key(), it.value());
    out.replace('/', QDir::separator());
    return out;
}

QStringList PathResolver::resolveList(const QStringList& exprs) const
{
    QStringList out;
    for (const QString& e : exprs) out << resolve(e);
    return out;
}

bool PipelineLoader::parsePipeline(const QString& id, const QJsonObject& obj, QString* error)
{
    PipelineDef def;
    def.id = id;
    def.displayName = obj.value("displayName").toString(id);
    def.description = obj.value("description").toString();
    def.maxParallel = obj.value("maxParallel").toInt(1);

    const QJsonArray arr = obj.value("stages").toArray();
    for (const QJsonValue& v : arr) {
        const QJsonObject so = v.toObject();
        PipelineStage s;
        s.id = so.value("id").toString();
        s.moduleId = so.value("module").toString();
        s.name = so.value("name").toString(s.id);
        s.optional = so.value("optional").toBool(false);
        s.params = so.value("params").toObject();
        for (const QJsonValue& d : so.value("dependsOn").toArray())
            s.dependsOn << d.toString();
        if (s.id.isEmpty() || s.moduleId.isEmpty()) {
            *error = QString("pipeline '%1': stage requires 'id' and 'module'").arg(id);
            return false;
        }
        def.stages << s;
    }
    if (def.stages.isEmpty()) {
        *error = QString("pipeline '%1': no stages").arg(id);
        return false;
    }

    // ---- DAG 校验：阶段 id 唯一、依赖必须存在、不能成环 ----
    // 没有这一步校验，错误配置会在调度时表现为“任务永远不结束”。
    QSet<QString> ids;
    for (const PipelineStage& s : def.stages) {
        if (ids.contains(s.id)) {
            *error = QString("pipeline '%1': duplicate stage id '%2'").arg(id, s.id);
            return false;
        }
        ids.insert(s.id);
    }
    QHash<QString, QStringList> deps;
    for (const PipelineStage& s : def.stages) {
        for (const QString& d : s.dependsOn) {
            if (!ids.contains(d)) {
                *error = QString("pipeline '%1': stage '%2' depends on unknown stage '%3'")
                             .arg(id, s.id, d);
                return false;
            }
            if (d == s.id) {
                *error = QString("pipeline '%1': stage '%2' depends on itself").arg(id, s.id);
                return false;
            }
            deps[s.id] << d;
        }
    }
    QHash<QString, int> color;   // 0=未访问 1=访问中 2=完成
    std::function<bool(const QString&)> visit = [&](const QString& n) -> bool {
        color[n] = 1;
        for (const QString& d : deps.value(n)) {
            if (color.value(d) == 1) return false;             // 回边 = 环
            if (color.value(d) == 0 && !visit(d)) return false;
        }
        color[n] = 2;
        return true;
    };
    for (const PipelineStage& s : def.stages) {
        if (color.value(s.id) == 0 && !visit(s.id)) {
            *error = QString("pipeline '%1': dependsOn cycle detected at stage '%2'")
                         .arg(id, s.id);
            return false;
        }
    }

    m_pipelines.insert(id, def);
    return true;
}

bool PipelineLoader::loadFromFile(const QString& jsonPath, QString* error)
{
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = QString("cannot open %1").arg(jsonPath);
        return false;
    }
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        *error = QString("pipelines.json parse error: %1").arg(perr.errorString());
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.value("schemaVersion").toInt() != 1) {
        *error = QString("pipelines.json: unsupported schemaVersion %1")
                     .arg(root.value("schemaVersion").toInt());
        return false;
    }
    const QJsonObject pipes = root.value("pipelines").toObject();
    for (auto it = pipes.begin(); it != pipes.end(); ++it) {
        if (!parsePipeline(it.key(), it.value().toObject(), error)) return false;
    }
    return true;
}

} // namespace task

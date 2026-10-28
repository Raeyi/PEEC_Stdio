#include "config/ModuleRegistry.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

namespace task {

namespace {
ModuleKind kindFromString(const QString& s)
{
    if (s == "dll") return ModuleKind::DllWorker;
    if (s == "internal") return ModuleKind::Internal;
    return ModuleKind::External;
}
} // namespace

bool ModuleRegistry::parseModule(const QString& id, const QJsonObject& obj, QString* error)
{
    ModuleDef def;
    def.id = id;
    def.displayName = obj.value("displayName").toString(id);
    def.kind = kindFromString(obj.value("kind").toString("external"));
    def.command = obj.value("command").toString();
    if (def.command.isEmpty()) {
        *error = QString("module '%1': missing 'command'").arg(id);
        return false;
    }
    for (const QJsonValue& v : obj.value("searchDirs").toArray())
        def.searchDirs << v.toString();
    def.workingDir = obj.value("workingDir").toString("{DATA}");
    for (const QJsonValue& v : obj.value("args").toArray())
        def.args << v.toString();
    def.workerHost = obj.value("workerHost").toString("worker_host.exe");
    def.timeoutSec = obj.value("timeoutSec").toInt(0);
    for (const QJsonValue& v : obj.value("layerRead").toArray())
        def.layerRead << v.toString();
    for (const QJsonValue& v : obj.value("layerWrite").toArray())
        def.layerWrite << v.toString();

    const QJsonObject contract = obj.value("inputContract").toObject();
    for (const QJsonValue& v : contract.value("required").toArray())
        def.contract.requiredInputs << v.toString();
    for (const QJsonValue& v : contract.value("optional").toArray())
        def.contract.optionalInputs << v.toString();
    const QJsonObject oc = obj.value("outputCheck").toObject();
    for (const QJsonValue& v : oc.value("files").toArray())
        def.contract.outputChecks << v.toString();
    def.contract.allOutputsRequired = oc.value("allRequired").toBool(true);

    if (def.kind == ModuleKind::External && def.searchDirs.isEmpty()) {
        *error = QString("module '%1': external kind requires 'searchDirs'").arg(id);
        return false;
    }
    m_modules.insert(id, def);
    return true;
}

bool ModuleRegistry::loadFromFile(const QString& jsonPath, QString* error)
{
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = QString("cannot open %1").arg(jsonPath);
        return false;
    }
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        *error = QString("modules.json parse error: %1").arg(perr.errorString());
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.value("schemaVersion").toInt() != 1) {
        *error = QString("modules.json: unsupported schemaVersion %1")
                     .arg(root.value("schemaVersion").toInt());
        return false;
    }
    const QJsonObject mods = root.value("modules").toObject();
    for (auto it = mods.begin(); it != mods.end(); ++it) {
        if (!parseModule(it.key(), it.value().toObject(), error)) return false;
    }
    return true;
}

} // namespace task

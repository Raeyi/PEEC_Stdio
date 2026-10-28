#include "io/InputValidator.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace task {

// 通配符展开：路径含 * ? 时按目录列出匹配文件（不含子目录）
static QStringList globMatches(const QString& path)
{
    if (!path.contains('*') && !path.contains('?'))
        return QStringList{path};
    const QFileInfo fi(path);
    const QString dirPath = fi.path();
    const QString pattern = fi.fileName();
    QDir dir(dirPath);
    QStringList out;
    for (const QString& name : dir.entryList({pattern}, QDir::Files, QDir::Name))
        out << QDir(dirPath).filePath(name);
    return out;
}

bool InputValidator::fileExistsNonEmpty(const QString& path, QString* detail)
{
    // 通配路径：任一个匹配文件存在且非空即通过
    const QStringList candidates = globMatches(path);
    for (const QString& cand : candidates) {
        const QFileInfo fi(cand);
        if (fi.exists() && !fi.isDir() && fi.size() > 0)
            return true;
    }
    if (detail) *detail = QString("missing or empty: %1").arg(path);
    return false;
}

bool InputValidator::validate(const QStringList& required, const QStringList& optional,
                              const QStringList& allowedExtensions,
                              QVector<InputIssue>* issues, QStringList* warnings)
{
    bool ok = true;
    for (const QString& p : required) {
        QString detail;
        if (!fileExistsNonEmpty(p, &detail)) {
            ok = false;
            if (issues) issues->push_back({p, "missing", detail});
        } else if (!allowedExtensions.isEmpty()) {
            // 通配路径按第一个匹配文件判断扩展名
            const QString sample = globMatches(p).value(0, p);
            const QString ext = QFileInfo(sample).suffix().toLower();
            bool allowed = false;
            for (const QString& a : allowedExtensions) {
                if (a.toLower() == ext) { allowed = true; break; }
            }
            if (!allowed) {
                ok = false;
                if (issues) issues->push_back({p, "badExtension",
                    QString("extension .%1 not in allowed: %2").arg(ext, allowedExtensions.join("/"))});
            }
        }
    }
    for (const QString& p : optional) {
        if (!fileExistsNonEmpty(p, nullptr) && warnings)
            warnings->append(QString("optional input missing: %1").arg(p));
    }
    return ok;
}

QString InputValidator::sha256OfFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f)) return QString();
    return QString::fromLatin1(h.result().toHex());
}

bool InputValidator::validatePortBinding(const QString& modelDir, QVector<InputIssue>* issues,
                                        QStringList* warnings)
{
    const bool ok = validatePortFile(QDir(modelDir).filePath("port.json"), modelDir,
                                     issues, warnings);
    // 主网格唯一性提示（契约要求 Model 下只有一个主网格）
    const QStringList meshes = QDir(modelDir).entryList({"*.msh"}, QDir::Files, QDir::Name);
    if (meshes.size() > 1 && warnings) {
        warnings->append(QString("multiple .msh under Model: %1 (contract expects one main mesh)")
                             .arg(meshes.join(",")));
    }
    return ok;
}

bool InputValidator::validatePortFile(const QString& portPath, const QString& meshSearchDir,
                                      QVector<InputIssue>* issues, QStringList* warnings)
{
    QFile pf(portPath);
    if (!pf.open(QIODevice::ReadOnly)) return true;   // 缺失由 required 列表负责报错

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(pf.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (issues) issues->push_back({portPath, "badJson",
            "port.json parse error: " + perr.errorString()});
        return false;
    }
    const QJsonObject root = doc.object();
    const QString meshFile = root.value("meshFile").toString();
    const QString expectSha = root.value("meshSha256").toString().toLower();

    if (meshFile.isEmpty()) {
        if (issues) issues->push_back({portPath, "missingField",
            "port.json: meshFile is empty"});
        return false;
    }

    // meshFile 是主网格文件名：先按 port.json 同目录找，再退到 meshSearchDir（通常 {Model}）。
    // UI 动态指定的 port.json 往往不在 Model 下，两步查找才能覆盖两种布局。
    QString meshPath = QFileInfo(portPath).dir().filePath(meshFile);
    if (!QFileInfo::exists(meshPath) && !meshSearchDir.isEmpty())
        meshPath = QDir(meshSearchDir).filePath(meshFile);
    if (!QFileInfo::exists(meshPath)) {
        if (issues) issues->push_back({meshPath, "missing",
            QString("port.json references '%1' but it does not exist (looked beside port.json and in %2)")
                .arg(meshFile, meshSearchDir.isEmpty() ? QString("<unset>") : meshSearchDir)});
        return false;
    }

    const QString actualSha = sha256OfFile(meshPath);
    if (expectSha.isEmpty()) {
        if (warnings) warnings->append(
            QString("port.json has no meshSha256; mesh identity not verified: %1").arg(meshFile));
    } else if (actualSha.compare(expectSha, Qt::CaseInsensitive) != 0) {
        if (issues) issues->push_back({meshPath, "sha256Mismatch",
            QString("mesh SHA-256 mismatch: port.json expects %1..., actual %2... "
                    "(mesh changed but port.json was not re-bound)")
                .arg(expectSha.left(16), actualSha.left(16))});
        return false;
    }

    return true;
}

} // namespace task

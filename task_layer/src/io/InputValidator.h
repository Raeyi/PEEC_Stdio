// InputValidator.h — 输入检查（文件存在性/非空/扩展名/端口-网格绑定）
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace task {

struct InputIssue {
    QString path;
    QString reason;   // missing / empty / badExtension / sha256Mismatch ...
    QString detail;
};

class InputValidator
{
public:
    // 检查 required 全部存在且非空；optional 缺失仅记录 warning
    static bool validate(const QStringList& required, const QStringList& optional,
                         const QStringList& allowedExtensions,
                         QVector<InputIssue>* issues, QStringList* warnings);
    static bool fileExistsNonEmpty(const QString& path, QString* detail);

    // 端口-网格绑定检查：port.json.meshFile 必须存在且 meshSha256 与主网格原始字节一致。
    // 这是 Compact 契约里最容易出错的一环（改网格忘记改哈希），故列为输入检查项。
    static bool validatePortBinding(const QString& modelDir, QVector<InputIssue>* issues,
                                    QStringList* warnings);

    // 校验任意位置的 port.json（UI 动态指定的路径）。
    // meshSearchDir：meshFile 为相对名时的备选查找目录（通常是 {Model}）。
    static bool validatePortFile(const QString& portPath, const QString& meshSearchDir,
                                 QVector<InputIssue>* issues, QStringList* warnings);

    // 流式计算文件 SHA-256（小写十六进制）
    static QString sha256OfFile(const QString& path);
};

} // namespace task

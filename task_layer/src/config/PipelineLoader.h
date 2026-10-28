// PipelineLoader.h — 流水线定义加载 + 路径变量解析
#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace task {

struct PipelineStage {
    QString id;
    QString moduleId;
    QString name;
    QStringList dependsOn;   // 依赖的阶段 id（空 = 无依赖）
    bool optional = false;   // 可选阶段：输入缺失或失败时不阻断主链
    QJsonObject params;      // 传给模块的参数（覆盖模块 args 中的变量）
};

struct PipelineDef {
    QString id;
    QString displayName;
    QString description;
    QVector<PipelineStage> stages;
    int maxParallel = 1;     // 最大并行阶段数
};

// 路径变量解析器：
//   固定变量 {DATA} {Model} {Setting} {Result} {BIN} {MODULE_ROOT} {CASE_ROOT} {TASK_DIR}
//   动态变量 {PORT_JSON} 等，由 UI 通过 task.run/inputs.validate 的 inputs 传入
//   （键名驼峰转大写下划线：portJson -> {PORT_JSON}）
class PathResolver
{
public:
    void setBase(const QString& dataRoot, const QString& moduleRoot,
                 const QString& binRoot, const QString& taskDir);

    // UI 动态输入（如 port.json 路径）。值本身可含固定变量，会一并展开。
    void setInputs(const QJsonObject& inputs);
    void clearInputs() { m_inputs = QJsonObject(); m_extraVars.clear(); }
    QJsonObject inputs() const { return m_inputs; }
    // 展开后的 inputs（供 job.json / state.json 记录）
    QJsonObject resolvedInputs() const;

    bool hasVar(const QString& name) const;       // name 含花括号，如 {PORT_JSON}
    QString resolve(const QString& expr) const;   // 展开全部变量
    QStringList resolveList(const QStringList& exprs) const;

private:
    static QString varNameFromKey(const QString& key);

    QMap<QString, QString> m_baseVars;    // setBase 建立
    QMap<QString, QString> m_extraVars;   // setInputs 建立
    QJsonObject m_inputs;
};

class PipelineLoader
{
public:
    bool loadFromFile(const QString& jsonPath, QString* error);
    const PipelineDef* pipeline(const QString& id) const
    {
        const auto it = m_pipelines.find(id);
        return it == m_pipelines.end() ? nullptr : &it.value();
    }
    QVector<PipelineDef> allPipelines() const { return m_pipelines.values().toVector(); }

private:
    bool parsePipeline(const QString& id, const QJsonObject& obj, QString* error);
    QMap<QString, PipelineDef> m_pipelines;
};

} // namespace task

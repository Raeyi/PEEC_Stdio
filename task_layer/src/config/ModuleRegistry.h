// ModuleRegistry.h — 模块注册表加载与查询
#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace task {

// 模块执行形态
enum class ModuleKind {
    External,   // 外部独立 exe（迁移期：现有 PEEC 工具）
    DllWorker,  // 功能 DLL + worker_host 隔离运行（目标形态）
    Internal    // 任务层内建（预留：校验/格式转换等）
};

struct ModuleContract {
    QStringList requiredInputs;   // 执行前必须存在的文件（路径变量）
    QStringList optionalInputs;   // 可选输入
    QStringList outputChecks;     // 执行后应存在的文件
    bool allOutputsRequired = true;
};

struct ModuleDef {
    QString id;
    QString displayName;
    ModuleKind kind = ModuleKind::External;
    // External: command = exe 名（在 searchDirs 中查找）
    // DllWorker: command = dll 名，由 worker_host 加载
    QString command;
    QStringList searchDirs;       // 路径变量，如 {BIN}
    // 子进程工作目录（路径变量）。默认 {DATA}；
    // 按“EXE 所在目录”解析相对路径的后端（如 gds2geo）需显式设为 {MODULE_ROOT}/gds2geo。
    QString workingDir = "{DATA}";
    QStringList args;             // 支持 {DATA} 等变量替换
    QString workerHost;           // DllWorker 时 worker_host 可执行文件
    ModuleContract contract;
    int timeoutSec = 0;           // 0 = 不限制
    QStringList layerRead;        // Working Model 层读写（L1..L4），供审计
    QStringList layerWrite;
};

class ModuleRegistry
{
public:
    // 从 modules.json 加载；失败返回 false 并设置 error
    bool loadFromFile(const QString& jsonPath, QString* error);
    bool hasModule(const QString& id) const { return m_modules.contains(id); }
    const ModuleDef* module(const QString& id) const
    {
        const auto it = m_modules.find(id);
        return it == m_modules.end() ? nullptr : &it.value();
    }
    QVector<ModuleDef> allModules() const { return m_modules.values().toVector(); }

private:
    bool parseModule(const QString& id, const QJsonObject& obj, QString* error);
    QMap<QString, ModuleDef> m_modules;
};

} // namespace task

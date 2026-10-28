// main.cpp — task_layer 任务层入口
// 用法：
//   task_layer.exe serve [--config <dir>] [--bin <dir>] [--module-root <dir>]   # stdio JSON-RPC 服务（供 UI）
//   task_layer.exe run --pipeline <id> --data-root <dir> [--k v ...]             # 单次执行
//   task_layer.exe validate --data-root <dir> [--pipeline <id>]                  # 输入校验
//   task_layer.exe modules / pipelines                                           # 列出注册表
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include "App.h"

using namespace task;

namespace {
// 后端运行包目录名。任务层以“包含任一后端包的父目录”为模块根 {MODULE_ROOT}，
// 各模块再用 searchDirs 定位到具体包：{MODULE_ROOT}/<包名>。
const char* const kBackendDirs[] = {"compact_em", "electromagnetic", "gds2geo",
                                    "Compact_pipeline", "pipeline"};

bool isModuleRoot(const QDir& d)
{
    for (const char* name : kBackendDirs) {
        if (QFileInfo::exists(d.absoluteFilePath(QString::fromLatin1(name)))) return true;
    }
    return false;
}

// 模块根默认推导：从 config 目录/exe 目录向上找“含后端运行包”的那一层，
// 这样无论 exe 放在 build/ 还是 build/Release/ 下都能定位到计算层。
QString guessModuleRoot(const QString& configDir, const QString& exeDir)
{
    const QStringList starts{configDir, configDir + "/..", exeDir, exeDir + "/.."};
    for (const QString& start : starts) {
        QDir d(QDir::cleanPath(QFileInfo(start).absoluteFilePath()));
        for (int i = 0; i < 6; ++i) {
            if (isModuleRoot(d)) return d.absolutePath();
            if (!d.cdUp()) break;
        }
    }
    return QDir::cleanPath(QFileInfo(configDir + "/../..").absoluteFilePath());
}
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("task_layer");
    QCoreApplication::setApplicationVersion("0.1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("QS-PEEC Studio task layer: orchestration, module routing, state management");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("command", "serve | run | validate | modules | pipelines");

    QCommandLineOption configOpt({"c", "config"}, "config directory (modules.json + pipelines.json)", "dir");
    QCommandLineOption binOpt({"b", "bin"}, "backend bin directory (worker_host.exe, modules/)", "dir");
    QCommandLineOption moduleRootOpt("module-root", "module root: dir containing the module projects (default: parent of config dir)", "dir");
    QCommandLineOption dataOpt("data-root", "case data root directory (contains Model/Setting/Result)", "dir");
    QCommandLineOption pipelineOpt("pipeline", "pipeline id", "id");
    parser.addOption(configOpt);
    parser.addOption(binOpt);
    parser.addOption(moduleRootOpt);
    parser.addOption(dataOpt);
    parser.addOption(pipelineOpt);
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.isEmpty()) parser.showHelp(1);

    // 配置目录：默认取可执行文件旁的 config/；可 --config 覆盖
    const QString exeDir = QCoreApplication::applicationDirPath();
    QString configDir = parser.value(configOpt);
    if (configDir.isEmpty()) configDir = exeDir + "/config";
    QString binRoot = parser.value(binOpt);
    if (binRoot.isEmpty()) {
        binRoot = exeDir + "/bin";
        // 单目录布局（产物与 task_layer.exe 同目录，如本地脚本构建）：
        // {BIN}/bin 并不存在，此时 {BIN} 直接取 exe 所在目录，保证
        // {BIN}/worker_host.exe 与 {BIN}/modules/<id>/ 能被解析到。
        if (!QFileInfo::exists(binRoot) && QFileInfo::exists(exeDir + "/worker_host.exe"))
            binRoot = exeDir;
    }
    // 模块根：--module-root > 环境变量 > 自动推导（向上找含后端运行包的目录）
    QString moduleRoot = parser.value(moduleRootOpt);
    if (moduleRoot.isEmpty()) moduleRoot = qEnvironmentVariable("TASK_LAYER_MODULE_ROOT");
    if (moduleRoot.isEmpty()) moduleRoot = guessModuleRoot(configDir, exeDir);

    App taskApp(configDir, moduleRoot, binRoot, &app);
    const QString cmd = args.first();

    if (cmd == "serve") {
        const int rc = taskApp.serve();
        if (rc != 0) return rc;          // 配置错误：直接退出，不进入事件循环
        return app.exec();
    }

    if (cmd == "run") {
        const QString pipelineId = parser.value(pipelineOpt);
        const QString dataRoot = parser.value(dataOpt);
        if (pipelineId.isEmpty() || dataRoot.isEmpty()) {
            QTextStream(stdout) << "run requires --pipeline and --data-root\n";
            return 2;
        }
        QJsonObject overrides;
        const QStringList passthrough = args.mid(1);
        for (const QString& kv : passthrough) {
            const int eq = kv.indexOf('=');
            if (eq > 0) overrides.insert(kv.left(eq), kv.mid(eq + 1));
        }
        QObject::connect(&taskApp, &App::exitRequested, &app, [](int code) {
            QCoreApplication::exit(code);
        });
        const int rc = taskApp.runOnce(pipelineId, dataRoot, overrides);
        if (rc != 0) return rc;          // 未能启动：不要把事件循环跑起来（否则会挂死）
        return app.exec();
    }

    if (cmd == "validate") {
        const QString dataRoot = parser.value(dataOpt);
        if (dataRoot.isEmpty()) {
            QTextStream(stdout) << "validate requires --data-root\n";
            return 2;
        }
        return taskApp.validateOnce(dataRoot, parser.value(pipelineOpt));
    }

    if (cmd == "modules" || cmd == "pipelines") {
        ModuleRegistry reg;
        PipelineLoader loader;
        QString err;
        if (!reg.loadFromFile(configDir + "/modules.json", &err) ||
            !loader.loadFromFile(configDir + "/pipelines.json", &err)) {
            QTextStream(stdout) << "config error: " << err << "\n";
            return 2;
        }
        QTextStream out(stdout);
        if (cmd == "modules") {
            for (const ModuleDef& d : reg.allModules()) {
                const char* k = d.kind == ModuleKind::DllWorker ? "dll"
                              : d.kind == ModuleKind::Internal ? "internal" : "external";
                out << d.id << "\t" << d.displayName << "\t" << k << "\n";
            }
        } else {
            for (const PipelineDef& p : loader.allPipelines()) {
                out << p.id << "\t" << p.displayName << "\tmaxParallel=" << p.maxParallel << "\n";
                for (const PipelineStage& s : p.stages)
                    out << "  - " << s.id << "\t" << s.moduleId << (s.optional ? "\t(optional)" : "") << "\n";
            }
        }
        out.flush();
        return 0;
    }

    parser.showHelp(1);
    return 0;
}

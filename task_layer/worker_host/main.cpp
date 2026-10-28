// worker_host/main.cpp — 隔离 worker 宿主
// 任务层为每个 DllWorker 模块启动一个独立 worker_host 进程，
// 该进程加载模块 DLL 并调用其入口，实现"功能 DLL 在隔离 worker 内运行"。
// 用法：
//   worker_host.exe --module <dllPath> --job <jobJsonPath> --data-root <dir> [--log <path>]
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "modules/module_api.h"

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("worker_host");

    QCommandLineParser parser;
    parser.setApplicationDescription("QS-PEEC isolated module worker host");
    parser.addHelpOption();
    QCommandLineOption moduleOpt("module", "module DLL path", "path");
    QCommandLineOption jobOpt("job", "job definition JSON path", "path");
    QCommandLineOption dataOpt("data-root", "case data root", "dir");
    QCommandLineOption logOpt("log", "log file path (optional)", "path");
    parser.addOption(moduleOpt);
    parser.addOption(jobOpt);
    parser.addOption(dataOpt);
    parser.addOption(logOpt);
    parser.process(app);

    const QString dllPath = parser.value(moduleOpt);
    const QString jobPath = parser.value(jobOpt);
    const QString dataRoot = parser.value(dataOpt);
    if (dllPath.isEmpty() || jobPath.isEmpty() || dataRoot.isEmpty()) {
        QTextStream(stdout) << "worker_host requires --module, --job, --data-root\n";
        return 2;
    }

    // 读取 job JSON
    QFile jf(jobPath);
    if (!jf.open(QIODevice::ReadOnly)) {
        QTextStream(stdout) << "cannot open job: " << jobPath << "\n";
        return 2;
    }
    QJsonParseError perr;
    QJsonDocument jdoc = QJsonDocument::fromJson(jf.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        QTextStream(stdout) << "job JSON error: " << perr.errorString() << "\n";
        return 2;
    }

    // 加载模块 DLL（显式设置 DLL 搜索目录，Windows 下确保依赖解析正确）
#ifdef Q_OS_WIN
    {
        const QString dllDir = QFileInfo(dllPath).absolutePath();
        SetDllDirectoryW(reinterpret_cast<LPCWSTR>(dllDir.utf16()));
    }
#endif
    QLibrary lib(QDir::toNativeSeparators(QFileInfo(dllPath).absoluteFilePath()));
    if (!lib.load()) {
        QTextStream(stdout) << "failed to load module DLL: " << lib.errorString() << "\n";
        return 3;
    }
    auto entry = reinterpret_cast<QsPeecModuleEntry>(
        lib.resolve("qs_peec_module_entry"));
    if (!entry) {
        QTextStream(stdout) << "module DLL missing entry qs_peec_module_entry\n";
        return 3;
    }

    const QString logPath = parser.value(logOpt);
    // 注意：qPrintable() 是**本地 8 位编码**（中文 Windows 上是 GBK），
    // 传给 DLL 的 C 接口必须统一用 UTF-8，否则中文路径会乱码/打开失败。
    const QByteArray jobUtf8 = QDir::toNativeSeparators(jobPath).toUtf8();
    const QByteArray dataUtf8 = QDir::toNativeSeparators(dataRoot).toUtf8();
    const QByteArray logUtf8 = logPath.isEmpty() ? QByteArray()
                                                 : QDir::toNativeSeparators(logPath).toUtf8();
    const int rc = entry(jobUtf8.constData(), dataUtf8.constData(),
                         logUtf8.isEmpty() ? nullptr : logUtf8.constData());
    // 退出码透传：0=成功，非 0=模块失败
    return rc;
}

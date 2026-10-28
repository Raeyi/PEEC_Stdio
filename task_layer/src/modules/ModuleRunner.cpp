#include "modules/ModuleRunner.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>

#include "io/InputValidator.h"

namespace task {

namespace {
// 模块输出的编码是混合的：机器可读部分（stderr JSON）是 UTF-8，
// 人可读进度行多按本地代码页（中文 Windows 为 GBK）输出。
// 先按 UTF-8 解码，出现替换字符再回退到本地代码页，避免中文路径乱码。
QString decodeModuleText(const QByteArray& bytes)
{
    const QString utf8 = QString::fromUtf8(bytes);
    if (utf8.contains(QChar::ReplacementCharacter))
        return QString::fromLocal8Bit(bytes);
    return utf8;
}

// 在模块输出里找最后一条 {"code":"QS-xxx-..."} 片段，用于对齐《错误码与异常处理规范》
QString extractErrorCode(const QByteArray& text)
{
    static const QRegularExpression re(
        QStringLiteral("\"code\"\\s*:\\s*\"(QS-[A-Za-z0-9_\\-]+)\""));
    QString found;
    auto it = re.globalMatch(QString::fromUtf8(text));
    while (it.hasNext()) {
        const auto m = it.next();
        found = m.captured(1);   // 取最后一条（模块往往先 warning 后 error）
    }
    return found;
}

bool copyOrReplace(const QString& src, const QString& dst, QString* err)
{
    if (!QFileInfo::exists(src)) {
        if (err) *err = QString("handoff source missing: %1").arg(src);
        return false;
    }
    if (QFileInfo::exists(dst) && !QFile::remove(dst)) {
        if (err) *err = QString("cannot overwrite %1").arg(dst);
        return false;
    }
    if (!QFile::copy(src, dst)) {
        if (err) *err = QString("copy failed: %1 -> %2").arg(src, dst);
        return false;
    }
    return true;
}

bool writeTextFile(const QString& path, const QString& text, QString* err)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (err) *err = QString("cannot write %1").arg(path);
        return false;
    }
    f.write(text.toUtf8());
    f.close();
    return true;
}

} // namespace

QString ModuleRunner::locateCommand(const ModuleDef& def, const PathResolver& resolver)
{
    // 先按 searchDirs 顺序找，再退化为 {BIN}。
    // 对外暴露是为了让 modules.list 能报告模块可用性（UI 需要区分"已注册"与"可执行"）。
    for (const QString& d : def.searchDirs) {
        const QString dir = resolver.resolve(d);
        const QString cand = QDir(dir).filePath(def.command);
        if (QFileInfo::exists(cand))
            return QDir::toNativeSeparators(QFileInfo(cand).absoluteFilePath());
    }
    const QString binCand = QDir(resolver.resolve("{BIN}")).filePath(def.command);
    if (QFileInfo::exists(binCand)) return QDir::toNativeSeparators(binCand);
    return QString();
}

ModuleRunner::~ModuleRunner()
{
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc = nullptr;
    }
}

QString ModuleRunner::writeJobJson(const ModuleDef& def, const QStringList& args,
                                   const PathResolver& resolver, const QString& dataRoot,
                                   const QString& stageId, const QString& taskId)
{
    const QString taskDir = resolver.resolve("{TASK_DIR}");
    QDir().mkpath(taskDir);
    const QString jobPath = QDir(taskDir).filePath(QString("job.%1.json").arg(stageId));

    QJsonObject paths;
    for (const QString& v : {"DATA", "Model", "Setting", "Result", "BIN", "MODULE_ROOT",
                             "CASE_ROOT", "TASK_DIR"}) {
        paths[v] = resolver.resolve("{" + v + "}");
    }

    QJsonObject job;
    job["schemaVersion"] = 1;
    job["taskId"] = taskId;
    job["stageId"] = stageId;
    job["moduleId"] = def.id;
    job["displayName"] = def.displayName;
    job["dataRoot"] = QDir::toNativeSeparators(dataRoot);
    job["paths"] = paths;
    job["layerRead"] = QJsonArray::fromStringList(def.layerRead);
    job["layerWrite"] = QJsonArray::fromStringList(def.layerWrite);
    job["args"] = QJsonArray::fromStringList(args);
    // UI 动态输入（如 port.json 路径）已展开为绝对路径，随 job 下发给功能 DLL
    job["inputs"] = resolver.resolvedInputs();
    job["createdAt"] = QDateTime::currentDateTime().toString(Qt::ISODate);

    QFile f(jobPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(job).toJson(QJsonDocument::Indented));
        f.close();
        return jobPath;
    }
    return QString();
}

void ModuleRunner::finishFailure(const QString& code, const QString& message)
{
    if (m_finished) return;
    m_finished = true;
    m_result.ok = false;
    m_result.errorCode = code;
    m_result.errorMessage = message;
    emit stageFinished(m_stageId, m_result);
}

void ModuleRunner::finishSuccess(const QStringList& outputs)
{
    if (m_finished) return;
    m_finished = true;
    m_result.ok = true;
    m_result.exitCode = 0;
    m_result.outputs = outputs;
    emit stageFinished(m_stageId, m_result);
}

QString ModuleRunner::sha256HexOfFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    char buf[65536];
    while (!f.atEnd()) {
        const qint64 n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        hash.addData(QByteArrayView(buf, static_cast<qsizetype>(n)));
    }
    return QString::fromLatin1(hash.result().toHex());
}

void ModuleRunner::runInternal(const ModuleDef& def)
{
    const QString action = def.command.trimmed();
    if (action == QStringLiteral("gds2em_handoff")) {
        if (handoffGdsToEm(m_resolver)) finishSuccess(m_result.outputs);
        return;
    }
    finishFailure("QS-TL-MOD-NOTCONFIGURED",
                  QString("unknown internal action '%1'").arg(action));
}

bool ModuleRunner::handoffGdsToEm(const PathResolver& resolver)
{
    // gds2geo 的 Result/em/ → EM（compact_em）的 Model/ + Setting/。
    // 两端 matdb.json 同为 schemaVersion 3 且属性结构一致，可直接交接；
    // gds2geo 网格坐标单位为 mm，故 set.txt 的 DIM 固定 1000。
    const QString emDir      = resolver.resolve("{DATA}/Result/em");
    const QString modelDir   = resolver.resolve("{Model}");
    const QString settingDir = resolver.resolve("{Setting}");
    if (!QDir().mkpath(modelDir) || !QDir().mkpath(settingDir)) {
        finishFailure("QS-TL-IO-ERROR",
                      "cannot create Model/Setting under data root: " + resolver.resolve("{DATA}"));
        return false;
    }

    // 1) 电磁网格：Result/em/*.msh → Model/<原名>
    const QStringList meshes = QDir(emDir).entryList({"*.msh"}, QDir::Files, QDir::Name);
    if (meshes.isEmpty()) {
        finishFailure("QS-TL-INPUT-MISSING", "no .msh produced under " + emDir);
        return false;
    }
    const QString meshName = meshes.first();
    QString err;
    const QString dstMesh = QDir(modelDir).filePath(meshName);
    if (!copyOrReplace(QDir(emDir).filePath(meshName), dstMesh, &err)) {
        finishFailure("QS-TL-IO-ERROR", err);
        return false;
    }

    // EM 要求 Model/ 下有且仅有一份 *.msh（多份直接报 QS-FMT-INPUT-AMBIGUOUS）。
    // 版图迭代会不断产出新网格，所以交接时清掉上一轮的旧网格，
    // 保证"跑完 gds_full 后 Model/ 只有一份主网格"这个不变式由任务层维护，UI 不必关心。
    for (const QString& f : QDir(modelDir).entryList({"*.msh"}, QDir::Files, QDir::Name)) {
        if (f == meshName) continue;
        const QString victim = QDir(modelDir).filePath(f);
        if (QFile::remove(victim))
            emit stageOutput(m_stageId,
                "[handoff] removed stale mesh '" + f + "' (EM requires exactly one *.msh in Model/)");
        else
            emit stageOutput(m_stageId, "[warn] cannot remove stale mesh " + victim);
    }

    // 2) 材料库：schemaVersion 3、properties.conductivity 结构一致，直接交接
    const QString dstMatdb = QDir(modelDir).filePath("matdb.json");
    if (!copyOrReplace(QDir(emDir).filePath("matdb.json"), dstMatdb, &err)) {
        finishFailure("QS-TL-IO-ERROR", err);
        return false;
    }
    QStringList outputs{dstMesh, dstMatdb};

    // 3) 端口定义：UI 通过 inputs.portJson 指定；未指定时沿用 Model/ 下已有的
    const QString dstPort = QDir(modelDir).filePath("port.json");
    const QString srcPort = resolver.hasVar("{PORT_JSON}") ? resolver.resolve("{PORT_JSON}") : QString();
    if (!srcPort.isEmpty()) {
        QFile pf(srcPort);
        if (!pf.open(QIODevice::ReadOnly)) {
            finishFailure("QS-TL-INPUT-MISSING", "cannot read port.json: " + srcPort);
            return false;
        }
        const QByteArray rawPort = pf.readAll();
        pf.close();  // 与 dstPort 可能同路径（UI 直接指向 Model/port.json），必须先释放句柄
        QJsonParseError pe;
        const QJsonDocument pd = QJsonDocument::fromJson(rawPort, &pe);
        if (pe.error != QJsonParseError::NoError || !pd.isObject()) {
            finishFailure("QS-TL-INPUT-INVALID",
                          QString("port.json is not valid JSON (%1): %2").arg(pe.errorString(), srcPort));
            return false;
        }
        QJsonObject port = pd.object();
        // meshFile 必须等于落到 Model 下的网格文件名（字节相同，哈希不变）
        const QString declared = port.value("meshFile").toString();
        if (!declared.isEmpty() && declared != meshName) {
            emit stageOutput(m_stageId, QString("[handoff] port.json meshFile '%1' -> '%2'")
                                            .arg(declared, meshName));
            port["meshFile"] = meshName;
        }
        // 哈希以实际落地的网格为准，避免 UI 侧算错导致 EM 报文件损坏
        const QString sha = sha256HexOfFile(dstMesh);
        if (!sha.isEmpty()) {
            const QString prev = port.value("meshSha256").toString();
            if (!prev.isEmpty() && prev != sha)
                emit stageOutput(m_stageId, "[warn] port.json meshSha256 recomputed for handed-over mesh");
            port["meshSha256"] = sha;
        }
        if (QFileInfo::exists(dstPort)) QFile::remove(dstPort);
        QFile out(dstPort);
        if (!out.open(QIODevice::WriteOnly)) {
            finishFailure("QS-TL-IO-ERROR", "cannot write " + dstPort);
            return false;
        }
        out.write(QJsonDocument(port).toJson(QJsonDocument::Indented));
        out.close();
        outputs << dstPort;
    } else if (QFileInfo::exists(dstPort)) {
        // 已有端口定义：检查它绑定的网格指纹是否还是当前这一份。
        // 版图迭代时网格会重新生成，三角形编号随之改变，旧的 triangleIds 会失效，
        // 这里必须明确告警，否则只有 EM 求解阶段才会报错，定位成本高。
        emit stageOutput(m_stageId, "[handoff] reusing existing " + dstPort);
        QFile pf(dstPort);
        if (pf.open(QIODevice::ReadOnly)) {
            const QJsonObject port = QJsonDocument::fromJson(pf.readAll()).object();
            pf.close();  // Windows 下文件句柄不关，后面 remove/覆写会失败
            const QString prevSha = port.value("meshSha256").toString();
            const QString curSha  = sha256HexOfFile(dstMesh);
            if (!prevSha.isEmpty() && !curSha.isEmpty() && prevSha != curSha) {
                emit stageOutput(m_stageId,
                    QString("[warn] mesh changed since port.json was written "
                            "(sha %1 -> %2): triangleIds may no longer match this mesh. "
                            "Re-pick ports in the UI before running peec_basic.")
                        .arg(prevSha.left(12), curSha.left(12)));
                // 仍然刷新绑定，避免 EM 直接报文件损坏（但端口几何仍需用户确认）
                QJsonObject fixed = port;
                fixed["meshFile"] = meshName;
                fixed["meshSha256"] = curSha;
                QFile out(dstPort);  // Truncate 直接覆写，不依赖 remove 成功
                if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    out.write(QJsonDocument(fixed).toJson(QJsonDocument::Indented));
                    out.close();
                }
            }
        }
        outputs << dstPort;
    } else {
        // 端口定义由 UI 在\"选端口\"这一步写入 Model/port.json，建模阶段还没有是正常的。
        // 这里只告警不失败，真正缺失会在 peec_basic 的输入检查阶段被拦下。
        emit stageOutput(m_stageId,
            "[warn] port.json not present yet - UI must write Model/port.json "
            "(triangleIds picked on the mesh) before running peec_basic");
    }

    // 4) 求解设置：inputs.setFile 指定 > 已有 > 生成默认（单位 mm ⇒ DIM 1000）
    const QString dstSet = QDir(settingDir).filePath("set.txt");
    if (resolver.hasVar("{SET_FILE}")) {
        const QString srcSet = resolver.resolve("{SET_FILE}");
        if (!copyOrReplace(srcSet, dstSet, &err)) {
            finishFailure("QS-TL-IO-ERROR", err);
            return false;
        }
    } else if (!QFileInfo::exists(dstSet)) {
        const QString fs  = resolver.hasVar("{FS}")   ? resolver.resolve("{FS}")   : QStringLiteral("1e9");
        const QString fe  = resolver.hasVar("{FE}")   ? resolver.resolve("{FE}")   : QStringLiteral("10e9");
        const QString nfp = resolver.hasVar("{N_FP}") ? resolver.resolve("{N_FP}") : QStringLiteral("100");
        if (!writeTextFile(dstSet,
                QStringLiteral("Solver_SET       0\n"
                               "MNA_WEIGHT       1\n"
                               "FS               %1\n"
                               "FE               %2\n"
                               "N_FP             %3\n"
                               "DIM              1000\n"
                               "Taylor           0\n"
                               "SAVE_BIN         1\n"
                               "SAVE_TXT         0\n"
                               "SAVE_SP          0\n"
                               "POST_PROCESSING  1\n"
                               "POST_FREQ_INDEX  -1\n"
                               "POST_PORT        1\n"
                               "POST_PLOT_J      1\n"
                               "POST_PLOT_QE     1\n"
                               "POST_PLOT_VE     1\n"
                               "POST_PLOT_M      0\n"
                               "POST_PLOT_QM     0\n"
                               "POST_PLOT_VM     0\n"
                               "POST_FIELD       1\n"
                               "$End_Def\n").arg(fs, fe, nfp), &err)) {
            finishFailure("QS-TL-IO-ERROR", err);
            return false;
        }
        emit stageOutput(m_stageId, QString("[handoff] generated default set.txt (FS=%1 FE=%2 N_FP=%3, DIM=1000 for mm)")
                                        .arg(fs, fe, nfp));
    }
    outputs << dstSet;

    emit stageOutput(m_stageId, "[handoff] ready for EM: " + outputs.join(", "));
    m_result.outputs = outputs;
    return true;
}

void ModuleRunner::launch(const ModuleDef& def, const QStringList& args,
                          const PathResolver& resolver, const QString& dataRoot,
                          const QString& stageId, const QString& taskId, int timeoutSec)
{
    m_def = def;
    m_resolver = resolver;
    m_stageId = stageId;
    m_taskId = taskId;
    m_dataRoot = dataRoot;
    m_timeoutSec = timeoutSec;
    m_result = RunResult();
    m_tail.clear();
    m_cancelled = false;
    m_timedOut = false;
    m_finished = false;

    // 1) 输入校验（模块契约）
    //    注意：optional 与 required 一样要做路径变量替换，否则会拿 "{Model}/xxx" 字面量去判存在性，
    //    每次都产生“可选输入缺失”的假警告。
    const QStringList required = m_resolver.resolveList(def.contract.requiredInputs);
    const QStringList optional = m_resolver.resolveList(def.contract.optionalInputs);
    QVector<InputIssue> issues;
    QStringList warnings;
    const bool inputsOk = InputValidator::validate(required, optional, {}, &issues, &warnings);
    for (const QString& w : warnings) emit stageOutput(stageId, "[warn] " + w);
    if (!inputsOk) {
        QStringList details;
        for (const InputIssue& i : issues)
            details << QString("%1 (%2)").arg(i.path, i.detail);
        finishFailure("QS-TL-INPUT-MISSING",
                      "required inputs missing: " + details.join("; "));
        return;
    }

    // 2) 内建阶段（kind: internal）：不起子进程，直接在任务层内做数据交接
    //    （此前 internal 会掉进 external 分支去找不存在的 exe，必须单独处理）
    if (def.kind == ModuleKind::Internal) {
        runInternal(def);
        return;
    }

    // 3) 组装可执行文件 + 参数
    QString exe;
    QStringList realArgs;

    if (def.kind == ModuleKind::DllWorker) {
        const QString dll = locateCommand(def, resolver);
        if (dll.isEmpty()) {
            finishFailure("QS-TL-MOD-NOTFOUND",
                          QString("module DLL '%1' not found in searchDirs").arg(def.command));
            return;
        }
        // worker host 查找：先按声明值展开；展开后不存在时，按文件名在 {BIN}、
        // {BIN}/modules、{BIN}/bin 下依次回退（兼容 exe 与 worker_host 同目录的单目录布局）。
        QString host = m_resolver.resolve(
            def.workerHost.isEmpty() ? QString("worker_host.exe") : def.workerHost);
        if (!QFileInfo::exists(host)) {
            const QString name = QFileInfo(host).fileName();
            const QString bin = m_resolver.resolve("{BIN}");
            bool found = false;
            for (const QString& dir : QStringList{bin, bin + "/modules", bin + "/bin"}) {
                const QString cand = QDir(dir).filePath(name);
                if (QFileInfo::exists(cand)) { host = cand; found = true; break; }
            }
            if (!found) {
                finishFailure("QS-TL-WORKER-NOTFOUND",
                              QString("worker host not found: %1 (searched {BIN}, {BIN}/modules, {BIN}/bin)")
                                  .arg(name));
                return;
            }
        }
        exe = QDir::toNativeSeparators(QFileInfo(host).absoluteFilePath());
        QStringList dllArgs = def.args;
        dllArgs << args;
        const QString jobPath = writeJobJson(def, dllArgs, m_resolver, dataRoot, stageId, taskId);
        if (jobPath.isEmpty()) {
            finishFailure("QS-TL-IO-JOB",
                          QString("cannot write job json under %1").arg(m_resolver.resolve("{TASK_DIR}")));
            return;
        }
        realArgs << "--module" << dll
                 << "--job" << QDir::toNativeSeparators(jobPath)
                 << "--data-root" << QDir::toNativeSeparators(dataRoot)
                 << "--log" << QDir::toNativeSeparators(
                        QDir(m_resolver.resolve("{TASK_DIR}")).filePath(stageId + ".log"));
    } else {
        exe = locateCommand(def, resolver);
        if (exe.isEmpty()) {
            finishFailure("QS-TL-MOD-NOTFOUND",
                          QString("executable '%1' not found in searchDirs").arg(def.command));
            return;
        }
        for (const QString& a : def.args) realArgs << m_resolver.resolve(a);
    }
    for (const QString& a : args) realArgs << m_resolver.resolve(a);

    // 3) 启动
    m_proc = new QProcess(this);
    // 工作目录按模块声明（默认 {DATA}）。gds2geo 一类后端按“EXE 所在目录”解析相对路径，
    // 必须在 modules.json 里显式声明 workingDir，否则其设置 JSON 会指向错误位置。
    m_proc->setWorkingDirectory(m_resolver.resolve(def.workingDir));
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_proc, &QProcess::readyReadStandardOutput,
            this, &ModuleRunner::onProcessOutput);
    connect(m_proc, &QProcess::finished,
            this, &ModuleRunner::onProcessFinished);
    connect(m_proc, &QProcess::errorOccurred,
            this, &ModuleRunner::onProcessError);

    emit stageOutput(stageId, QString("[launch] %1 %2").arg(exe, realArgs.join(' ')));
    m_proc->start(exe, realArgs);
    if (!m_proc->waitForStarted(10000)) {
        const QString err = m_proc->errorString();
        m_proc->deleteLater();
        m_proc = nullptr;
        finishFailure("QS-TL-MOD-EXEC", QString("failed to start %1: %2").arg(exe, err));
        return;
    }

    if (m_timeoutSec > 0) {
        QPointer<ModuleRunner> self(this);
        QTimer::singleShot(m_timeoutSec * 1000, this, [self, this] {
            if (!self || !m_proc) return;
            if (m_proc->state() != QProcess::NotRunning) {
                m_timedOut = true;
                emit stageOutput(m_stageId, "[timeout] killing process after "
                                                + QString::number(m_timeoutSec) + "s");
                m_proc->kill();
            }
        });
    }
}

void ModuleRunner::onProcessOutput()
{
    if (!m_proc) return;
    const QByteArray chunk = m_proc->readAll();
    // 滚动保留最近 32KB 原始输出：模块的错误码 JSON 往往在失败前就已输出，
    // 不在 finished 时再读一次就能拿到。
    m_tail.append(chunk);
    if (m_tail.size() > 32768) m_tail = m_tail.right(32768);

    const QString text = decodeModuleText(chunk);
    for (const QString& line : text.split('\n')) {
        if (!line.trimmed().isEmpty()) emit stageOutput(m_stageId, line.trimmed());
    }
}

void ModuleRunner::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    if (m_finished || !m_proc) return;

    QProcess* proc = m_proc;
    m_proc = nullptr;
    // 合并：滚动缓冲 + 收尾残余（MergedChannels 下 readAll 读的是 stdout+stderr）
    m_tail.append(proc->readAll());
    const QByteArray tail = m_tail;

    RunResult r;
    r.exitCode = exitCode;
    r.ok = (status == QProcess::NormalExit && exitCode == 0) && !m_cancelled && !m_timedOut;

    if (m_cancelled) {
        r.errorCode = "QS-TL-CANCEL";
        r.errorMessage = "cancelled by user";
    } else if (m_timedOut) {
        r.errorCode = "QS-TL-MOD-TIMEOUT";
        r.errorMessage = QString("module exceeded timeout of %1s").arg(m_timeoutSec);
    } else if (!r.ok) {
        const QString moduleCode = extractErrorCode(tail);
        r.errorCode = moduleCode.isEmpty() ? QStringLiteral("QS-TL-MOD-EXEC") : moduleCode;
        r.errorMessage = QString("exit=%1 %2").arg(exitCode)
                             .arg(decodeModuleText(tail).trimmed().right(1500));
    }

    // 输出校验（仅成功时才要求产物）
    if (r.ok) {
        const QStringList checks = m_resolver.resolveList(m_def.contract.outputChecks);
        QStringList bad;
        for (const QString& c : checks) {
            QString detail;
            if (InputValidator::fileExistsNonEmpty(c, &detail)) {
                r.outputs << c;
            } else {
                bad << detail;
            }
        }
        if (!bad.isEmpty() && (m_def.contract.allOutputsRequired || r.outputs.isEmpty())) {
            r.ok = false;
            r.errorCode = "QS-TL-OUTPUT-MISSING";
            r.errorMessage = "output check failed: " + bad.join("; ");
        } else if (!bad.isEmpty()) {
            emit stageOutput(m_stageId, QString("[outputs] %1 missing but acceptable")
                                            .arg(bad.join("; ")));
        }
    }

    m_finished = true;
    proc->deleteLater();
    emit stageFinished(m_stageId, r);
}

void ModuleRunner::onProcessError(QProcess::ProcessError error)
{
    if (m_finished) return;
    // FailedToStart 走 launch() 的 waitForStarted 分支；这里记录运行时错误
    if (!m_proc) return;
    m_result.ok = false;
    m_result.errorCode = "QS-TL-MOD-EXEC";
    m_result.errorMessage = QString("process error: %1 (%2)")
                                .arg(int(error))
                                .arg(m_proc->errorString());
    emit stageOutput(m_stageId, "[error] " + m_result.errorMessage);
}

void ModuleRunner::cancel()
{
    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        m_cancelled = true;
        emit stageOutput(m_stageId, "[cancel] killing process");
        m_proc->kill();
    }
}

} // namespace task

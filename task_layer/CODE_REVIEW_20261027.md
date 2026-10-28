# task_layer 代码审查与最小 Demo 记录（2026-10-27）

审查对象：`task_layer/`（v0.1.0，CMake + Qt6 Core），目标形态为架构图中的"平台主 EXE"。
审查方式：逐文件静态审查 → 编译验证（Qt 6.8.3 / MinGW 13.1）→ 真实链路跑通（4 个 PEEC exe）。

结论：**分层设计是对的（输入检查 / 任务编排 / 模块路由 / 状态与结果管理），但原代码存在 3 个编译阻断问题、14 个逻辑缺陷**，其中"失败不传播导致任务悬挂""pump 重入导致阶段重复启动""serve 配置错误时进程挂死"属于必现级问题。已全部修复并验证。

---

## 一、编译阻断问题（原代码无法通过编译）

| # | 位置 | 问题 | 修复 |
|---|---|---|---|
| A1 | `orchestrator/TaskOrchestrator.h:51` | `QMap<QString, QProcess*> m_running` 却插入 `ModuleRunner*`，类型不匹配 | 改为 `QMap<QString, ModuleRunner*>`，并合并原 `m_running`/`m_runners` 两个冗余 map |
| A2 | `rpc/StdioTransport.h:25` | `connect(&m_in, &QTextStream::readyRead, ...)`：`QTextStream` 既不是 `QObject` 也没有 `readyRead` 信号，serve 模式完全不可用 | 新增 `StdioTransport.cpp`：`StdinReader`（`QThread`）阻塞读 stdin → 信号排队到主线程；析构时安全收尾 |
| A3 | `rpc/StdioTransport.h:24` | `m_out.setCodec("UTF-8")` 在 Qt6 已被移除 | 改为 `_setmode(stdin/stdout, _O_BINARY)` + UTF-8 字节直写（同时避免 Windows 文本模式把 JSON 行改成 `\r\n`） |

## 二、逻辑缺陷（能编过，但会挂死 / 状态错乱 / 结果失真）

| # | 问题 | 后果 | 修复 |
|---|---|---|---|
| B1 | 阶段失败后不做依赖传播 | 下游永远停在 `pending`，`m_pending` 非空、`m_running` 为空 → `pump()` 永不收尾 → **任务悬挂**（CLI 不退出、UI 永远 running） | 新增 `propagateBlocked()`：下游标 `skipped` + `QS-TL-DEP-BLOCKED` / `QS-TL-DEP-SKIPPED`，然后正常收尾 |
| B2 | `pump()` 重入 | `launch()` 在输入校验失败时**同步** emit `stageFinished` → 回调 `pump()`；此时该阶段仍在 `m_pending`（原代码 erase 在 launch 之后）→ 同阶段被二次启动、状态错乱 | 抽出 `launchStage()`（先出队再启动）+ `m_pumping` 重入保护（RAII `PumpGuard`） |
| B3 | `cancel()` 只杀进程不清状态 | `m_pending`/`m_running` 残留 → 之后 `startTask()` 永远返回 false，任务层事实上"卡死" | 取消时清空 pending、未启动阶段置 `skipped/QS-TL-CANCEL`，进程回落后统一收尾复位 `m_busy` |
| B4 | `task.status` / `task.result` 只认活动任务 | 任务一结束就 `m_activeTaskId.clear()`，UI **拿不到刚跑完的结果**（协议里明明定义了这两个方法） | 保留 `m_lastTaskId/m_lastTaskDir`，并支持 `params.dataRoot` 回退 |
| B5 | `inputs.validate` 硬编码 `{Model}/model_em.msh` | 样例数据是 `em.msh`，配置里用的是 `{Model}/*.msh` → **永远返回 invalid**，且重复实现了契约逻辑 | 改为按注册表"入口阶段（无 dependsOn）"的 `inputContract` 校验；新增 port.json ↔ 网格 **SHA-256 绑定校验** |
| B6 | `validate` 子命令是空实现 | 直接 `exit(0)`，README 却宣称可用 | 真正执行校验并输出 JSON，非法返回退出码 1 |
| B7 | `serve()` 配置错误时 `QCoreApplication::exit(2)` | 在 `exec()` 之前调用**无效** → 进程进入事件循环但没有 transport → 永久挂死 | `serve()` 改为返回错误码，`main` 直接 return；错误按 JSON-RPC 格式回给 UI |
| B8 | DLL 分支不通 | `worker_host` 需要 `--module/--job/--data-root`，原代码只挑了 exe 路径、不写 job.json → 任何 `kind:"dll"` 模块必失败（`em_full` 流水线不可用） | 写 `job.<stageId>.json`（含 paths/layers/args）+ 组装完整参数；DLL/host 缺失给 `QS-TL-MOD-NOTFOUND` / `QS-TL-WORKER-NOTFOUND` |
| B9 | 子进程输出尾部丢失 | `readyRead` 已消费缓冲，失败时 `readAllStandardOutput()` 只剩残余 → 错误信息为空 | 滚动保留最近 32KB 原始输出，failed 时从中提取错误码 |
| B10 | `worker_host` 用 `qPrintable()` 传路径给 DLL | 中文 Windows 下是 GBK，**中文路径必错**（当前工程目录就是中文） | 统一 `toUtf8()`；`SetDllDirectoryW` 去掉临时对象指针 |
| B11 | 阶段日志不落盘 | README 说写 `task.log`，实际只转发给 UI | `task.stage.log` 同步写 `task.log` |
| B12 | `ModuleRunner` 泄漏 | `m_runners.remove()` 只出 map 不删对象，挂在 orchestrator 下累积 | 统一 `take() + deleteLater()`；`clearRunners()` 收尾 |
| B13 | 超时定时器捕获裸 `this` | runner 先销毁 → 悬空指针 UB | 改用 `QPointer` 守卫 |
| B14 | `maxParallel: 2` 让 plot / post 并发 | Compact 用 `.peec-data.lock` 独占句柄限制并发写，两个 exe 同时操作同一 DATA 会互相锁失败 | `peec_basic` 改 `maxParallel: 1`（并行能力保留给未来不共享 DATA 的 DLL 模块） |
| B15 | 模块错误码不解析 | PEEC 模块 stderr/stdout 有标准 JSON（含 `code`），任务层只报 exit code，UI 无法按码分支 | 从输出提取最后一条 `"code":"QS-..."` 填入 `errorCode`（已验证：`QS-FMT-SETTING-UNKNOWN_KEY` 正确透传） |
| B16 | 相对 `--data-root` 被二次拼接 | 模块以 dataRoot 为 cwd 启动，相对路径变成 `DATA/.../DATA/...` → 输入全缺 | App 层统一 `QDir::cleanPath(QFileInfo(dataRoot).absoluteFilePath())` |
| B17 | `state.json` 的 `finishedAt` 每次都写 now | queued/running 状态谎报"已结束"；`startedAt` 在阶段记录替换时丢失 | 仅在终态写 `finishedAt`，并给阶段事件带上 `startedAt/finishedAt` |

## 三、附带增强（不在原需求内，但直接影响可用性）

1. **配置级 DAG 校验**（`PipelineLoader`）：阶段 id 唯一、`dependsOn` 必须存在、DFS 三色判环。原来错误配置只会在运行期表现为"任务不结束"。
2. **输入检查前置化**：`inputs.validate` 支持 `pipeline` 参数，只校验入口阶段（下游输入由上游产出，运行前不可能存在），并新增网格哈希绑定检查——已验证"改网格忘改 SHA"能被准确报出。
3. **模块根自动推导**：从 config/exe 目录向上找含 `Compact_pipeline` 的那一层，也支持 `TASK_LAYER_MODULE_ROOT` 环境变量；exe 放在 `build/` 或 `build/Release/` 下都不必再手写 `--module-root`。
4. **模块输出编码兼容**：UTF-8 优先、出现替换字符回退本地代码页（PEEC 进度行是 GBK），中文路径不再乱码。

## 四、仍建议处理（本次未改，属设计层面）

1. **Working Model 层契约只标注未校验**：`layerRead/layerWrite` 目前仅写进注册表。建议在调度前做"上游写层 ⊇ 下游读层"检查，并把层指纹（版本 + SHA-256）写进 `state.json`，这才是"单一事实源"的落地。
2. **`overrides` 无白名单**：`task.run` 的 `overrides.{stageId}.{key}` 直接拼成 `--key value`，UI 传错会静默变成模块参数。建议模块声明 `allowedArgs` 并在任务层校验。
3. **日志无背压**：阶段日志全量逐行推送，大算例会刷爆管道（本次 10916 面算例单阶段数千行）。建议截断/采样，或改成 UI 主动拉取。
4. **取消是硬杀进程**：PEEC 无信号处理，可能留下 `.partial` 目录与 `IN_PROGRESS` 状态。建议取消后清理半成品并明确标记"可重跑"。
5. **单任务串行**：`m_busy` 限制同一时刻只跑一条流水线。要做"优化循环（多算例并行扫描）"需要任务队列 + 并发任务池。
6. **错误码表待补**：`QS-TL-*`（含新增 `QS-TL-DEP-BLOCKED` / `QS-TL-DEP-SKIPPED` / `QS-TL-CFG-DEADLOCK` / `QS-TL-MOD-TIMEOUT` / `QS-TL-SKIP-NOINPUT`）需登记进《错误码与异常处理规范》。

## 五、验证结果

| 项目 | 结果 |
|---|---|
| 编译 | Qt 6.8.3 (mingw_64) + MinGW 13.1，`-static` 产物 0 警告阻断；`task_layer.exe` 782KB、`worker_host.exe` 261KB |
| CLI 单次执行 | `run --pipeline peec_basic`：extract → solve → plot → post 四段全 done，686 ms |
| serve 模式 | `rpc_smoke.py` 8 项全 PASS（ping / modules.list / pipelines.list / inputs.validate / task.run / task.finished 通知 / task.status / task.result） |
| 失败路径 | 坏 set.txt：extract 失败 → `errorCode=QS-FMT-SETTING-UNKNOWN_KEY`，下游 3 段 blocked，任务正常收尾（不再悬挂） |
| 输入检查 | 篡改网格不改 SHA：`valid=false, reason=sha256Mismatch` |
| 产物 | `Result/` 下 model.peec / extract.state / S_parameter.csv / solve.state / 4 张 S 参数 SVG / Source_Plot/Source.msh；`Result/tasks/<taskId>/` 下 state.json + task.log |

## 六、关于大算例（原 10916 面样例）

- 提取：约 33–165 s，`model.peec` 约 **2.9 GB**（该版本按 float64 稠密存储）；
- 求解：`N_FP=100` 时内存预算不足（shared 6.67 GB + worker 4.45 GB > 10.46 GB 预算，32 GB 机器 / 16.5 GB 可用），报 `QS-SOLV-MEMORY-OUT_OF_MEMORY`；
- 建议：先用 `N_FP ≤ 10` 或减小网格验证链路；正式算例再放开频点。最小 demo 用 `tools/make_mini_case.py` 生成的 32 三角形算例，全链路 <1 s。

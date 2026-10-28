# task_layer — QS-PEEC Studio 任务层

任务层是 UI（交互层）与计算层之间的"平台主 EXE"，承担架构图标注的四项职责：**输入检查、任务编排、模块路由、状态与结果管理**。计算层模块（现在为 Compact PEEC 4 个 exe，目标形态为功能 DLL）由任务层按流水线定义调用，各阶段可并行、可可选跳过。

> **给 UI/前端的对接文档见 [UI_API.md](UI_API.md)**（进程启动方式、完整 API、通知、错误码、Qt 客户端骨架）。本文件是任务层的设计与维护说明。

## 一、分层定位（对应架构图）

```
┌──────────────────────────────────────────────────┐
│ 交互层  UI（Qt）     项目/导入 物理设置 仿真任务 优化 结果 │
│        │  JSON-RPC 2.0 over stdio（QProcess 启动）  │
├──────────────────────────────────────────────────┤
│ 任务层  task_layer.exe  ← 本模块                     │
│        输入检查 · 任务编排 · 模块路由 · 状态与结果管理   │
│        │  按 pipeline 定义调度（依赖 DAG + 最大并行）    │
├──────────────────────────────────────────────────┤
│ 计算层  功能模块（exe / DLL worker）                   │
│        物理模型解析 | 等效电路(PEEC) | 仿真求解 | 优化 | 后处理 │
│        external：直接启动 exe（迁移期）                │
│        dll：worker_host.exe 隔离加载（目标形态）       │
├──────────────────────────────────────────────────┤
│ 数据层  geometry.geo / materials.json / simulation.json │
│        网格与结果映射（DATA 目录约定）                 │
└──────────────────────────────────────────────────┘
```

## 二、与 UI 的交互协议（JSON-RPC 2.0 over stdio）

UI 用 `QProcess::start("task_layer.exe", {"serve", ...})` 启动任务层，
双方以 **stdout 一行一个 JSON 消息**通信（UTF-8）。

### 请求（UI → 任务层）

| method | params | 返回 |
|---|---|---|
| `ping` | — | `{pong, app, schemaVersion}` |
| `modules.list` | — | `{modules:[{id,displayName,kind,command,available,resolvedPath}]}`（`available` 为可用性探测结果） |
| `pipelines.list` | — | `{pipelines:[{id,displayName,stages:[{id,module,optional,dependsOn}]}]}` |
| `inputs.validate` | `{dataRoot, pipeline?, module?, inputs?}` | `{valid, checked, issues:[{path,reason,detail}], warnings, inputs?}` |
| `task.run` | `{pipeline, dataRoot, taskId?, overrides?, inputs?}` | `{taskId, pipeline}` |
| `module.run` | `{moduleId, dataRoot, taskId?, inputs?}` | `{taskId, pipeline}` |
| `task.cancel` | `{taskId}` | `{cancelled}` |
| `task.status` | `{taskId}` | state.json 内容（各阶段状态） |
| `task.result` | `{taskId}` | state + `resultFiles` |
| `shutdown` | — | `{bye}` 后进程退出 |

错误响应：`{jsonrpc:"2.0", id, error:{code,message,data}}`；
错误码：-32000 取消 / -32001 任务不存在 / -32002 任务运行中 /
-32003 输入非法 / -32004 配置错误 / -32005 模块执行失败。

#### `inputs`：UI 动态输入（重点：port.json 由前端配置下发）

UI 侧由用户动态指定的文件（典型是端口定义 `port.json`）通过 `inputs` 下发，不要塞进 `overrides`：

```json
{"jsonrpc":"2.0","id":3,"method":"task.run","params":{
  "pipeline":"gds_to_peec",
  "dataRoot":"D:/cases/demo/DATA",
  "inputs":{"portJson":"D:/cases/demo/config/port.json"}
}}
```

规则：

- 键名驼峰转大写下划线作为路径变量：`portJson` → `{PORT_JSON}`；值可含固定变量（如 `{DATA}/Model/port.json`），会被二次展开；
- `inputs` 会写入 `state.json` 的 `inputs` 字段，并原样进 DLL 模块的 `job.json`（`job.inputs`）；
- 提供了 `{PORT_JSON}` 时，入口契约里名为 `port.json` 的必需项不再判缺失（改为 warning）——把它落到 `{Model}/` 是交接阶段的职责；
- `inputs.validate` 会额外校验该文件存在、JSON 合法、`meshFile` 存在且 `meshSha256` 与主网格一致。

### 单模块执行（module.run）

UI 可以脱离流水线单独启动一个模块（如只重建网格）。实现方式是构造一条
**只含该模块的"单阶段流水线"**（`single:<moduleId>`）交给编排器，
因此进度通知、状态持久化、`task.status` / `task.result` / `task.cancel` 与流水线完全一致，
UI 不需要两套处理逻辑。

`inputs.validate` 传 `module` 时按该模块的契约做输入检查。

### 通知（任务层 → UI，实时状态）

| method | params |
|---|---|
| `task.started` | `{taskId, pipeline, ts}` |
| `task.stage.started` | `{taskId, stageId, name}` |
| `task.stage.log` | `{taskId, stageId, line}` |
| `task.stage.finished` | `{taskId, stageId, status, exitCode, errorCode, errorMessage, outputs[]}` |
| `task.finished` | `{taskId, status, summary, elapsedMs}` |
| `task.cancelled` | `{taskId}` |

### Qt 侧 UI 最小示例

```cpp
// UI 进程内
auto* proc = new QProcess(this);
proc->start("task_layer.exe", {"serve"});
connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]{
    while (proc->canReadLine()) {
        const QJsonDocument d = QJsonDocument::fromJson(proc->readLine());
        // 判断是响应（有 id）还是通知（有 method）
    }
});
// 发请求
proc->write(QJsonDocument(QJsonObject{
    {"jsonrpc","2.0"},{"id",1},{"method","task.run"},
    {"params", QJsonObject{{"pipeline","peec_basic"},{"dataRoot", dataRoot}}
}}).toJson(QJsonDocument::Compact) + '\n');
```

## 三、编排文件

### config/modules.json — 模块注册表

每个模块声明：`kind`（external/dll）、`command`、`searchDirs`、`args`、`workingDir`、
`inputContract.required/optional`（执行前校验）、`outputCheck.files`（执行后校验）、
`layerRead/layerWrite`（Working Model 层契约，供审计与未来一致性检查）。

- `searchDirs`：按顺序查找 `command`，找不到再退到 `{BIN}`。**支持多候选**，后端包改名/搬迁时不要改代码：
  `["{MODULE_ROOT}/compact_em", "{MODULE_ROOT}/electromagnetic", "{MODULE_ROOT}/Compact_pipeline"]`。
- `workingDir`：子进程工作目录，默认 `{DATA}`。按“**EXE 所在目录**”解析相对路径的后端（如 gds2geo）必须显式写
  `"workingDir": "{MODULE_ROOT}/gds2geo"`，否则其设置 JSON 会指向错误位置。
  （gds2geo 无 `--data-root` 选项，须用 `--gds/--tech/--output-dir` 等显式绝对路径调用。）

路径变量：`{DATA} {Model} {Setting} {Result} {BIN} {MODULE_ROOT} {CASE_ROOT} {TASK_DIR}`，
以及 UI 通过 `inputs` 注入的动态变量（如 `{PORT_JSON}`）。

`{MODULE_ROOT}` 自动推导：从 config 目录 / exe 目录逐级向上，找到**含任一后端运行包**
（`Compact_pipeline` / `electromagnetic` / `gds2geo` / `pipeline`）的那一层；
也可用 `--module-root` 或环境变量 `TASK_LAYER_MODULE_ROOT` 显式指定。
`{BIN}` 默认 `<task_layer.exe 目录>/bin`；若该目录不存在而 exe 旁侧有 `worker_host.exe`
（单目录布局），则 `{BIN}` 直接取 exe 目录。

### config/pipelines.json — 流水线定义

```json
{ "id": "peec_basic", "maxParallel": 2, "stages": [
    { "id": "extract", "module": "peec.model_extract", "name": "...", "dependsOn": [] },
    { "id": "solve",   "module": "peec.solver",        "name": "...", "dependsOn": ["extract"] },
    { "id": "plot",    "module": "peec.plotter",       "name": "...", "dependsOn": ["solve"], "optional": true }
]}
```

- `dependsOn` 声明依赖，形成 DAG；就绪阶段按 `maxParallel` 并行启动；
- `optional: true`：输入缺失自动跳过（Skipped），不阻断主链（对应旁路：网格融合/DPEC/绘图）。
- **注意**：`peec_basic` 的 `maxParallel` 为 **1**。Compact 用 `DATA/.peec-data.lock` 独占句柄限制并发写，
  同一 DATA 上并行跑绘图/后处理会互相锁失败；并行能力留给未来不共享 DATA 的 DLL 模块。

## 四、命令行

```powershell
task_layer.exe serve --config .\config --bin .\bin      # stdio RPC 服务（UI 用）
task_layer.exe run --pipeline peec_basic --data-root D:\Cases\demo   # 单次执行
task_layer.exe modules / pipelines                        # 打印注册表
```

## 五、DLL 模块接入（目标形态）

1. 模块负责人按 `src/modules/module_api.h` 实现 DLL，导出
   `qs_peec_module_entry(job_json_path, data_root, log_path)`；
2. DLL 放 `bin/modules/<moduleId>/`；
3. modules.json 中登记 `kind: "dll"` + `workerHost`；
4. 任务层为每个 DLL 阶段启动独立 `worker_host.exe` 进程加载执行（崩溃隔离、内存隔离）。

迁移期：现有 Compact 4 个 exe 以 `kind: "external"` 直接调用，无需改动。

## 六、状态与结果

- 每任务在 `DATA/Result/tasks/<taskId>/` 下生成 `state.json`（任务状态 + 各阶段记录）+ `task.log`；
- 阶段状态：pending → running → done / failed / skipped / notConfigured；
- 机器错误码格式 `QS-TL-<域>-<错误名>`（如 QS-TL-INPUT-MISSING），与《错误码与异常处理规范》对齐；
  **模块自身错误码会原样透传**（如 `QS-FMT-SETTING-UNKNOWN_KEY`、`QS-SOLV-MEMORY-OUT_OF_MEMORY`），UI 按 `errorCode` 分支，不要解析文本；
- 依赖失败/被跳过的下游阶段统一置 `skipped`，用 `errorCode` 区分原因：
  - `QS-TL-DEP-BLOCKED`：上游 failed；
  - `QS-TL-DEP-SKIPPED`：上游被跳过（可选阶段输入缺失等）；
  - `QS-TL-CFG-DEADLOCK`：`dependsOn` 无法解析（加载期已校验 DAG，正常不会出现）；
  - `QS-TL-SKIP-NOINPUT`：可选阶段自身输入缺失；
  - `QS-TL-CANCEL` / `QS-TL-MOD-TIMEOUT`：用户取消 / 超时；
- 任务结束后 `task.status` / `task.result` **仍可查询**（保留最近一次任务），UI 不必在运行中抢答；
- 任务结束后 `task.finished` 通知携带 elapsedMs，UI 可据此展示耗时。

## 七、构建

### 7.1 产线（MSVC，与 UI 同工具链）

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --build build --config Release
```

依赖：Qt6 Core（仅 Core，不依赖 Widgets）、MSVC v143 / CMake 3.16+。
worker_host 在 Windows 上使用 LoadLibrary/SetDllDirectoryW 保证 DLL 路径解析。

### 7.2 本地快速构建（免管理员，Qt6 MinGW）

无 MSVC 的机器可用 Qt 官方 MinGW 版 Qt + MinGW 13.1（均装在用户目录，不动系统）：

```powershell
python -m pip install aqtinstall
# Qt6 Core（只取 qtbase，体积小）
python -m aqt install-qt windows desktop 6.8.3 win64_mingw --archives qtbase -O C:\Qt
# MinGW 13.1 工具链（官方源慢时可从 USTC 镜像直接下 7z 后用 tar 解压到 C:\Qt\Tools）
python -m aqt install-tool windows desktop tools_mingw1310 -O C:\Qt

# 一键构建（有 cmake 走 cmake，没有则 moc+g++ 直编）
powershell -ExecutionPolicy Bypass -File .\build_local.ps1 -Clean
```

产物在 `build\`：`task_layer.exe`、`worker_host.exe`、`config\`、`Qt6Core.dll` 及 MinGW 运行库（已 `-static` 链接 GCC 运行时）。
脚本做了两处必要的规避：中文工程目录下全部用**相对路径**调用 moc/g++，脚本文件保存为 **UTF-8 BOM**（PowerShell 5.1 才能正确解析中文）。

## 八、本地测试运行

目录布局（**模块根 = 后端运行包目录的父目录**）：

```text
PEEC_Stdio\
├── compact_em\        # 计算层：PEEC 四 exe + DATA\ 样例
├── gds2geo\           # 计算层：建模/网格四 exe + DATA\
└── task_layer\        # 本工程（任务层）源码
    ├── config\        # modules.json / pipelines.json（构建时拷到 exe 旁）
    └── build\         # task_layer.exe、bin\worker_host.exe、config\
```

### 8.1 不编译也能先验证 PEEC 链路（Compact 四 exe 是编译好的二进制）

```powershell
$dataRoot = 'C:\...\Compact_pipeline_20261023\Compact_pipeline\DATA'
& '...\Compact_pipeline\PEEC_Model_Extractor.exe'   --data-root $dataRoot
& '...\Compact_pipeline\PEEC_Solver.exe'            --data-root $dataRoot
& '...\Compact_pipeline\PEEC_S_Parameter_Plotter.exe' --data-root $dataRoot
```

成功退出码为 0；DATA\Result 下出现 model.peec / extract.state / S_parameter.csv / solve.state 即链路通。
样例 DATA 的 `.peec-data.lock` 是零字节持久锁，无需删除。

> 注意：`Compact_pipeline\DATA` 的大样例有 10916 个 PEC 面，提取约 33–165 s、`model.peec` 约 2.9 GB；
> `N_FP=100` 时求解会因内存预算不足报 `QS-SOLV-MEMORY-OUT_OF_MEMORY`（32 GB 机器）。冒烟请用 8.2 的小算例。

### 8.2 生成小算例并跑通四段流水线

```powershell
# 1) 生成小算例（平行板 + 两端端口，16~32 个三角形；自动算网格 SHA-256）
python .\tools\make_mini_case.py --out ..\demo\mini_strip

# 2) 注册表自检
.\build\task_layer.exe modules
.\build\task_layer.exe pipelines

# 3) 输入检查（含 port.json 与网格 SHA-256 绑定校验）
.\build\task_layer.exe validate --data-root ..\demo\mini_strip\DATA --pipeline peec_basic

# 4) 跑编排流水线
.\build\task_layer.exe run --pipeline peec_basic --data-root ..\demo\mini_strip\DATA
```

预期：终端按 extract→solve→plot→post 顺序打印事件，最后 `task.finished {"status":"done"}`；
`DATA\Result\tasks\<taskId>\` 下生成 `state.json` + `task.log`；
S 参数 SVG 在 `Result\S_Parameter_Plot\`，源图在 `Result\Source_Plot\`。

exe 放在 `build\` 下运行时，模块根会自动向上找到含后端运行包（`compact_em` / `gds2geo` /
`electromagnetic` / `Compact_pipeline`）的目录；
也可显式指定：`--module-root <父目录>` 或环境变量 `TASK_LAYER_MODULE_ROOT`。

### 8.3 UI 联调（serve 模式）

```powershell
.\build\task_layer.exe serve
```

UI 以 QProcess 启动并走 JSON-RPC（见第二节协议表），`inputs.validate` 可先于 `task.run` 做输入检查。
无 UI 时可用冒烟脚本验证整套协议（8 项断言：ping / 列表 / 校验 / 运行 / 通知 / 状态 / 结果）：

```powershell
python .\tools\rpc_smoke.py --exe .\build\task_layer.exe --data-root ..\demo\mini_strip\DATA
```

## 九、扩展点（下一步）

- 阶段级参数透传：`task.run` 的 `overrides.{stageId}.{key}` 会追加到模块参数（建议加 allowedArgs 白名单校验）；
- 错误码域扩展：`QS-TL-*` 已预留，模块内部错误由模块自身定义 `QS-<模块域>-*`；
- Working Model 层一致性检查：注册表 `layerRead/layerWrite` 已就位，可在调度前做"上游写 ⊇ 下游读"校验并记录层指纹；
- 任务队列：当前 `m_busy` 保证同一时刻只跑一条流水线，做"优化循环/批量扫描"时需要队列 + 并发池。

## 十、变更记录

- 2026-10-27：修复 Qt6 编译阻断（`QTextStream` 传输层重写、`QMap` 类型）、任务悬挂、pump 重入、取消后无法再启动、结果查询、DLL 分支、错误码透传等 17 项问题；新增 DAG 校验、输入哈希校验、小算例生成器与 RPC 冒烟脚本。详见 `CODE_REVIEW_20261027.md`。
- 2026-10-28：修复路径定位——① 模块根推导改为"向上找含后端运行包（`compact_em`/`electromagnetic`/`gds2geo`/`Compact_pipeline`）的目录"；② `searchDirs` 支持多候选，电磁计算层目录更名 `electromagnetic` → `compact_em` 后配置同步（**`pipelines.json` 只引用模块 id、不含路径，无需改动**）；
  ③ `{BIN}` 与 `worker_host` 布局对齐（产物落 `<exe目录>/bin`）并补齐回退查找；④ 新增模块级 `workingDir`（默认 `{DATA}`），供按 EXE 目录解析相对路径的后端使用；⑤ 修复 `optional` 输入未做变量替换导致的假警告。新增 UI 动态输入通道 `inputs`（`portJson` → `{PORT_JSON}`，随 `job.json`/`state.json` 下发与留痕），`inputs.validate` 会校验该文件与网格的 SHA-256 绑定。新增前端对接文档 `UI_API.md`。
- 2026-10-28：`phys.model_parse` 由 DLL 占位改为真实的 GDS2GEO 源模型阶段（external），
  并按"一个 exe 一个模块"的粒度新登记 `gds.prepare` / `gds.mesh_em` / `gds.mesh_thermal`
  （均显式 `workingDir: {MODULE_ROOT}/gds2geo`：gds2geo 按 EXE 目录解析相对路径且无 `--data-root`）；
  新增流水线 `gds_full`。新增 **`module.run`**（单模块独立执行，内部复用"单阶段流水线"），
  `modules.list` 增加 `available` / `resolvedPath`，`inputs.validate` 支持 `module`。
  修复 `build_local.ps1` 拷贝 config 时目标已存在会嵌套成 `build\config\config\` 的问题。
- 2026-10-28：新增内建阶段（`kind: "internal"`）：`ModuleRunner::launch` 此前会把 internal 当 external
  去找不存在的 exe，现单独分支处理；首个内建动作为 `gds2em_handoff`（`Result/em/` → `Model/` +
  `Setting/`，改写 port.json 的 meshFile/meshSha256，按需生成 set.txt）。
  `em_full` 据此串成真实全链路：gds 三段 → handoff → peec 提取/求解 → 绘图/后处理。
- 2026-10-28：按实际业务流程改为**三段式**（中间要插 UI 选端口交互，无法一条流水线跑到底），
  故**移除 `em_full`**，把交接阶段并入 `gds_full` 末尾（`gds_full` 现在产出的
  `Model/` + `Setting/` 可直接被 `peec_basic` 使用）。交接阶段相应调整：
  ① `port.json` 改为**可选**（建模阶段 UI 还没选端口，缺失只告警不失败，由 `peec_basic`
  的输入检查兜底）；② 新增**网格指纹失效检测**——比对 port.json 记录的 `meshSha256`
  与本次生成的网格，不一致时告警 `triangleIds` 可能失效、提示重新选端口，并刷新绑定；
  ③ 交接时清理 `Model/` 下的旧网格（EM 要求有且仅有一份 `*.msh`，否则报
  `QS-FMT-INPUT-AMBIGUOUS`）。实测确认 EM 会校验 `meshSha256` 非空且与实际网格一致
  （留空报 `QS-FMT-JSON-SCHEMA_VIOLATION`，不符报 `QS-IO-FILE-CORRUPTED`），
  故 UI 选完端口后应再跑一次 `module.run internal.handoff` 刷新绑定。
  修复：改写 port.json 前未关闭读句柄，导致 Windows 下覆写静默失败（只出告警、文件没变）。
- 2026-10-28：新增打包脚本 `tools\package.ps1`（产出 `dist\`，对齐 compact_em 的"平铺目录、拷走即用"规则；
  因任务层链接 QtCore，需额外带 4 个 DLL）。修复 `modules.list`：internal 模块此前被报成
  `kind=external` 且 `available=false`（按文件探测，而内建动作没有实体文件），
  会导致 UI 把"刷新端口绑定"的 `internal.handoff` 当成不可用而过滤掉。


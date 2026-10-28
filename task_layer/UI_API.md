# task_layer 对接说明（供 UI 集成）

本文件是**给前端/UI 侧**的对接文档：任务层以 `task_layer.exe` 形式交付，
UI 通过 `QProcess` 启动它，双方用 **stdin/stdout 上的 JSON-RPC 2.0** 通信。
UI 不需要、也不要直接调用计算层的 exe（`compact_em\`、`gds2geo\`）。

任务层职责：接收 UI 的算例目录与流水线名 → 按依赖调度后端程序 → 实时回传进度 → 回传结果文件清单。

---

## 一、交付物与部署

### 1.1 交付目录（UI 直接拿这个）

用打包脚本产出，一条命令：

```powershell
cd PEEC_Stdio\task_layer
powershell -ExecutionPolicy Bypass -File .\tools\package.ps1     # 产出 dist\
```

```text
dist\                     ← 整个目录交给 UI，拷走即用
├── task_layer.exe        817 KB   UI 唯一需要启动的进程
├── Qt6Core.dll           6.8 MB   必须与 exe 同目录
├── libstdc++-6.dll                 MinGW 运行时（MSVC 构建时没有这三项）
├── libgcc_s_seh-1.dll
├── libwinpthread-1.dll
├── config\                         必须：模块注册表 + 流水线定义
│   ├── modules.json
│   └── pipelines.json
├── bin\
│   ├── worker_host.exe             DLL 形态模块的宿主（当前流水线用不到，但建议带上）
│   └── Qt6Core.dll
├── UI_API.md                       本文档
└── README.md
```

重新打包会**清空重建** `dist\`，不会残留旧产物；若 `task_layer.exe` 正被运行中的实例占用导致复制失败，脚本会直接报错中止（不会打出缺主程序的残包）。

**与 compact_em 打包规则的差异**：`compact_em` 的 4 个 exe 是纯静态的（只依赖 `KERNEL32.dll` / `bcrypt.dll`），单文件拷走即可；任务层用了 Qt 核心库，因此需要额外带上 `Qt6Core.dll` 等 4 个 DLL，**不能只拷 `task_layer.exe` 一个文件**。

`config\` 里的两个 JSON 是**可调配置**：后端 exe 换目录、改流水线编排时改它们即可，不用重新编译。

### 1.2 部署布局：程序与数据分离

```text
<部署根>\
├── compact_em\            程序：电磁计算 exe（只读）
├── gds2geo\               程序：建模与网格 exe（只读）
├── task_layer\
│   └── dist\              程序：任务层（UI 启动它）
└── cases\                 数据：dataRoot 只指向这里
    ├── demo\DATA\
    └── <其它项目>\DATA\
```

任务层启动时**从 exe 所在目录逐级向上**查找含 `compact_em` / `gds2geo` 的目录作为模块根。
`dist\` 在 `task_layer\` 下时会自动找到，已实测无需额外参数。

**算例数据不放在程序目录里**，理由：

- 后端程序目录保持"只有程序"，不会被网格、端口、求解结果污染（`model.peec` 单个文件可达 GB 级）；
- 多个版图项目各占一个 `cases\<项目>\DATA`，互不覆盖；
- 不会误删后端自带的示例算例。

已实测：`dataRoot` 指向 `cases\demo\DATA`（在后端目录之外）时，`gds_full` 正常跑完，
产物全部落在该算例目录，两个程序目录没有新增任何算例文件。
如果 UI 侧把 `task_layer\` 单独拷到别处，后端就找不到了，此时必须显式指定：

```powershell
task_layer.exe serve --module-root D:\app\backend
```

`--module-root` 指向**后端包的父目录**（上例中的 `<模块根>`）。

---

## 二、启动与进程管理

```powershell
task_layer.exe serve [--config <目录>] [--bin <目录>] [--module-root <目录>]
```

- `serve` 是固定子命令，缺了会走命令行模式（跑一次就退出），UI 集成时不要漏；
- 三个可选参数都有自动推导，正常部署下不用传；
- 工作目录建议设为 **exe 所在目录**；
- **通道模式必须是 `SeparateChannels`**：stdout 是纯 JSON 消息流，stderr 是任务层自身的告警/崩溃信息，两者不能合并；
- 退出：先发 `shutdown` 请求，再等待进程结束；超时（>10s）才 `kill`。
  任务层也监听了 stdin 关闭，UI 进程崩溃时它会自行退出，不会留僵尸进程。

---

## 三、通信协议

- 传输：stdio，UTF-8，**一行一个 JSON 消息**，以 `\n` 结尾（Compact 契约）；
- 方向：UI 写 stdin 发请求，任务层写 stdout 回响应/通知；
- 三种消息：
  - **请求**：含 `id` + `method` → 必有响应；
  - **响应**：`{jsonrpc, id, result}` 或 `{jsonrpc, id, error}`；
  - **通知**：含 `method` 无 `id` → 任务层主动推送，UI 不应回复；
- 配对：UI 自增 `id`，按 `id` 匹配响应；通知按 `method` 分发。

---

## 四、API 一览

| method | params | 返回 |
|---|---|---|
| `ping` | — | `{pong, app, schemaVersion}` |
| `modules.list` | — | `{modules:[{id,displayName,kind,command,available,resolvedPath}]}` |
| `pipelines.list` | — | `{pipelines:[{id,displayName,stages:[...]}]}` |
| `inputs.validate` | `{dataRoot, pipeline?, module?, inputs?}` | `{valid, checked, issues, warnings}` |
| `task.run` | `{pipeline, dataRoot, taskId?, inputs?}` | `{taskId, pipeline}` |
| `module.run` | `{moduleId, dataRoot, taskId?, inputs?}` | `{taskId, pipeline}` ← **单独启动一个模块** |
| `task.cancel` | `{taskId}` | `{cancelled}` |
| `task.status` | `{taskId}` | `state.json` 内容 |
| `task.result` | `{taskId}` | `state` + `resultFiles` |
| `shutdown` | — | `{bye}` 后退出 |

### 4.1 ping（握手）

```json
{"jsonrpc":"2.0","id":1,"method":"ping","params":{}}
```
```json
{"jsonrpc":"2.0","id":1,"result":{"pong":true,"app":"task_layer","schemaVersion":1}}
```

### 4.2 modules.list（拿到模块与可用性）

```json
{"jsonrpc":"2.0","id":2,"method":"modules.list","params":{}}
```

返回片段：

```json
{"id":2,"result":{"modules":[
  {"id":"phys.model_parse","displayName":"物理模型解析（GDS2GEO 源模型）",
   "kind":"external","command":"gds2geo.exe",
   "available":true,"resolvedPath":"C:\\...\\gds2geo\\gds2geo.exe"},
  {"id":"sim.solver","displayName":"仿真求解（MNA/ALPS，待接入）",
   "kind":"dll","command":"sim_solver.dll","available":false,"resolvedPath":""}
]}}
```

- `available: true` 表示当前部署下能真的找到程序；`false` 是尚未接入的占位模块；
- UI 的"单独运行某个模块"列表请只展示 `available: true` 的项。

### 4.3 pipelines.list（拿到可跑的流水线）

```json
{"jsonrpc":"2.0","id":2,"method":"pipelines.list","params":{}}
```

返回片段：

```json
{"jsonrpc":"2.0","id":2,"result":{"pipelines":[
  {"id":"peec_basic","displayName":"电磁基本链路（PEEC 四段）",
   "stages":[{"id":"extract","module":"peec.model_extract","optional":false,"dependsOn":[]},
             {"id":"solve","module":"peec.solver","optional":false,"dependsOn":["extract"]},
             {"id":"plot","module":"peec.plotter","optional":true,"dependsOn":["solve"]},
             {"id":"post","module":"peec.post","optional":true,"dependsOn":["solve"]}]}
]}}
```

**当前可用状态**（重要）：

| pipeline | 说明 |
|---|---|
| `gds_full` | **建模**：源模型 → 几何准备 → 电磁网格（热网格可选）→ **交接**（把网格/材料/设置落到 `Model/` + `Setting/`） |
| `peec_basic` | **电磁**：PEEC 提取 → 频域求解 → S 参数绘图 + 源/场后处理（后两段可选） |
| `thermal_te` | 预留，未接入 |

UI 侧下拉框请只暴露 `gds_full` / `peec_basic`。想区分"已注册"与"可执行"，
用 `modules.list` 的 `available` 字段（见 4.2）。

### 典型业务流程（四步，中间插着 UI 选端口）

端口只能由人在网格上选，所以**不是一条流水线跑到底**，而是四步：

```text
① UI: task.run gds_full          → Model/model_em.msh + matdb.json，Setting/set.txt
                                   （此时还没有 port.json，建模照样成功；旧网格自动清理）
② UI: 加载 model_em.msh 渲染      → 用户选三角面 → 写 Model/port.json
③ UI: module.run internal.handoff → 刷新 port.json 的 meshFile / meshSha256
④ UI: task.run peec_basic        → 提取 → 求解 → S 参数
```

```json
// ① 建模（不需要 port.json）
{"method":"task.run","params":{"pipeline":"gds_full","dataRoot":"D:/cases/demo/DATA"}}

// ② UI 自己写 D:/cases/demo/DATA/Model/port.json（不经过 task_layer）

// ③ 刷新端口与网格的绑定（7ms，省得前端自己算 SHA-256）
{"method":"module.run","params":{"moduleId":"internal.handoff","dataRoot":"D:/cases/demo/DATA"}}

// ④ 跑电磁
{"method":"task.run","params":{"pipeline":"peec_basic","dataRoot":"D:/cases/demo/DATA"}}
```

几点约定：

- ① ~ ④ 必须用**同一个 `dataRoot`**；
- ① 的交接段负责把 `Result/em/` 搬到 `Model/`，并**保证 `Model/` 下只有一份 `*.msh`**
  （多份 EM 会直接报 `QS-FMT-INPUT-AMBIGUOUS`）；
- ③ 不是必须的，但强烈推荐——否则前端得自己算网格的 SHA-256（见 4.7）。

### 4.4 inputs.validate（跑之前先体检）

```json
{"jsonrpc":"2.0","id":3,"method":"inputs.validate","params":{
  "pipeline":"peec_basic",
  "dataRoot":"D:/cases/demo/DATA"
}}
```

```json
{"jsonrpc":"2.0","id":3,"result":{
  "valid":false,
  "checked":["D:/cases/demo/DATA/Model/model.msh", "..."],
  "issues":[{"path":"D:/cases/demo/DATA/Model/port.json",
             "reason":"missing","detail":"file does not exist"}],
  "warnings":[]
}}
```

UI 应在点击"开始计算"前调用它，把 `issues` 直接展示给用户（路径 + 原因），`warnings` 以次要样式提示。

### 4.5 task.run（启动任务）

```json
{"jsonrpc":"2.0","id":4,"method":"task.run","params":{
  "pipeline":"peec_basic",
  "dataRoot":"D:/cases/demo/DATA"
}}
```
```json
{"jsonrpc":"2.0","id":4,"result":{"taskId":"t-20261028-104233-7f3a1c"}}
```

- `dataRoot` **必须是绝对路径**（任务层会把后端进程的工作目录设为它，传相对路径会被二次拼接）；
- `taskId` 可省略，由任务层生成；UI 若要自己管理任务号，可传入（须唯一）；
- 一个任务层进程**同时只能跑一个任务**，重复调用返回 `-32002`；
- 该请求只是"启动"，真正完成靠 `task.finished` 通知。

### 4.6 模块独立执行（module.run）

不走流水线、单独跑一个模块时用这个（例如只重建网格、只跑一次源模型解析）：

```json
{"jsonrpc":"2.0","id":4,"method":"module.run","params":{
  "moduleId":"phys.model_parse",
  "dataRoot":"D:/cases/demo/DATA"
}}
```
```json
{"jsonrpc":"2.0","id":4,"result":{"taskId":"t1793168978448","pipeline":"single:phys.model_parse"}}
```

要点：

- `moduleId` 取 `modules.list` 里的 `id`；
- **与流水线执行完全一致**：同样收到 `task.started` / `task.stage.*` / `task.finished` 通知，
  同样能用 `task.status` / `task.result` / `task.cancel`；
- 返回的 `pipeline` 字段形如 `single:<moduleId>`，便于 UI 区分"流水线任务"与"单模块任务"；
- 同一进程仍然一次只能跑一个任务（`-32002`）；
- 模块自身的输入契约照常生效，缺输入时报 `QS-TL-INPUT-MISSING`。

同一个模块既可以用 `module.run` 单独启动，也可以作为 `task.run` 流水线里的一个阶段，
两种方式走的是同一套执行与状态逻辑，UI 不需要写两套处理。

### 4.7 端口定义 port.json（UI 负责写进 `Model/`）

端口只能由用户在网格上选取，任务层既生成不了也猜不出来，所以 **port.json 由 UI 直接写到
`{dataRoot}/Model/port.json`**，不经过 task_layer：

```json
{
  "meshFile": "model_em.msh",
  "meshSha256": "<可留空，交接阶段会按实际网格重算>",
  "ports": [
    {"direction": [0,0,1],  "impedanceOhm": 50, "triangleIds": [1, 2]},
    {"direction": [0,0,-1], "impedanceOhm": 50, "triangleIds": [3, 4]}
  ]
}
```

要点：

- `triangleIds` 必须取自**当前这份网格**（UI 加载 `Model/model_em.msh` 渲染后让用户选）；
- `direction` 必须指向从 `triangleIds[0]` 到 `triangleIds[1]` 的方向，否则 EM 报 `QS-BC-PORT-MAPPING_AMBIGUOUS`；
- **`meshSha256` 必须非空且等于实际网格的 SHA-256**，EM 会逐个校验：

  | 填法 | EM 反应 |
  |---|---|
  | 留空 `""` | `QS-FMT-JSON-SCHEMA_VIOLATION`：`meshSha256 must be a non-empty JSON string` |
  | 非空但不匹配 | `QS-IO-FILE-CORRUPTED`：`meshSha256 does not match the selected mesh` |

  **推荐做法**：UI 只写 `ports`，写完后再调一次 `module.run internal.handoff`（7ms），
  由任务层把 `meshFile` / `meshSha256` 刷新成实际值，前端不用碰哈希。
  若前端愿意自己算网格 SHA-256 也可以，两条路都行。

- `Model/` 下只能有一份 `*.msh`：版图迭代会不断产出新网格，交接阶段会**自动删除上一轮的旧网格**，
  UI 不需要手动清理（否则 EM 报 `QS-FMT-INPUT-AMBIGUOUS`）。

**版图迭代时的失效检测**（重要）：交接阶段会比对 port.json 记录的网格指纹与本次生成的网格，
不一致说明网格已重新生成，会通过 `task.stage.log` 通知：

```text
[warn] mesh changed since port.json was written (sha deadbeef1111 -> 08ea3d3b8710):
       triangleIds may no longer match this mesh. Re-pick ports in the UI before running peec_basic.
```

UI 收到这条应提示用户**重新选端口**。此时建模任务仍会成功完成，只是端口需要复核。

### 4.8 inputs：可选的动态输入（set.txt 等）

| 键 | 对应变量 | 用途 |
|---|---|---|
| `portJson` | `{PORT_JSON}` | 端口文件放在算例之外时使用；不传则沿用 `Model/port.json` |
| `setFile` | `{SET_FILE}` | 求解设置（set.txt）。不传时：算例已有则沿用，否则任务层生成默认（`DIM=1000`，即坐标单位 mm） |
| `fs` / `fe` / `nFp` | `{FS}` / `{FE}` / `{N_FP}` | 仅自动生成 set.txt 时生效，默认 `1e9` / `10e9` / `100` |

```json
{"jsonrpc":"2.0","id":4,"method":"task.run","params":{
  "pipeline":"peec_basic",
  "dataRoot":"D:/cases/demo/DATA",
  "inputs":{"setFile":"D:/cases/demo/config/set.txt"}
}}
```

- `inputs` 会写入 `state.json` 的 `inputs` 字段，便于追溯；
- `inputs.validate` 同样接受 `inputs`，会校验文件存在、`meshFile` 存在且哈希与主网格一致。

### 4.9 task.status / task.result

```json
{"jsonrpc":"2.0","id":5,"method":"task.status","params":{"taskId":"t-20261028-104233-7f3a1c"}}
```

返回 `state.json` 内容：

```json
{"taskId":"t-20261028-104233-7f3a1c","pipeline":"peec_basic","status":"running",
 "startedAt":"2026-10-28T10:42:33","elapsedMs":0,
 "inputs":{"portJson":"D:/cases/demo/config/port.json"},
 "stages":[
   {"id":"extract","moduleId":"peec.model_extract","name":"PEEC 模型提取",
    "status":"done","exitCode":0,"startedAt":...,"finishedAt":...,
    "outputs":["D:/cases/demo/DATA/Result/model.peec"],"errorCode":"","errorMessage":""},
   {"id":"solve","moduleId":"peec.solver","name":"频域求解","status":"running",...}
 ]}
```

`task.result` 在其基础上多一个 `resultFiles`（所有阶段产出的并集，均已确认存在）：

```json
{"taskId":"...","status":"done","resultFiles":[
  "D:/cases/demo/DATA/Result/model.peec",
  "D:/cases/demo/DATA/Result/S_parameter.csv",
  "D:/cases/demo/DATA/Result/S_Parameter_Plot/S_para_mag_port1.svg"]}
```

任务结束后仍可查询（任务层保留最近一次任务），UI 不必在运行中抢答。

### 4.10 task.cancel / shutdown

```json
{"jsonrpc":"2.0","id":6,"method":"task.cancel","params":{"taskId":"..."}}
{"jsonrpc":"2.0","id":7,"method":"shutdown","params":{}}
```

---

## 五、通知（任务层 → UI，实时状态）

| method | params |
|---|---|
| `task.started` | `{taskId, pipeline, ts}` |
| `task.stage.started` | `{taskId, stageId, name}` |
| `task.stage.log` | `{taskId, stageId, line}` |
| `task.stage.finished` | `{taskId, stageId, status, exitCode, errorCode, errorMessage, outputs[]}` |
| `task.finished` | `{taskId, status, summary, elapsedMs}` |
| `task.cancelled` | `{taskId}` |

UI 建议映射：`task.stage.started` → 阶段打点；`task.stage.log` → 日志窗（高频，注意限流）；
`task.finished` → 收尾，随后调用 `task.result` 拉清单。

阶段 `status`：`pending` / `running` / `done` / `failed` / `skipped` / `notConfigured`。

---

## 六、错误码

### 6.1 RPC 层（`error.code`）

| code | 含义 | UI 建议 |
|---|---|---|
| -32000 | 已取消 | 提示用户已取消 |
| -32001 | 任务不存在 | 刷新任务列表 |
| -32002 | 已有任务在运行 | 禁用"开始"按钮，等 `task.finished` |
| -32003 | 输入非法 | 展示 `error.data` 里的 `issues` |
| -32004 | 配置错误（modules.json/pipelines.json 加载失败） | 提示部署不完整，看 stderr |
| -32005 | 模块执行失败 | 展示 `errorCode` |

### 6.2 阶段层（通知里的 `errorCode`）

机器按 `errorCode` 分支，**不要解析文本**。格式为 `QS-TL-<域>-<错误名>`，后端模块自身的错误码会原样透传。

| errorCode | 含义 |
|---|---|
| `QS-TL-INPUT-MISSING` | 必需输入缺失（`errorMessage` 含具体路径） |
| `QS-TL-MOD-NOTFOUND` | 后端程序未找到（部署路径不对） |
| `QS-TL-MOD-EXEC` | 后端程序非 0 退出 |
| `QS-TL-OUTPUT-MISSING` | 宣告成功但没有产出文件 |
| `QS-TL-MOD-TIMEOUT` | 阶段超时 |
| `QS-TL-CANCEL` | 用户取消 |
| `QS-TL-DEP-BLOCKED` | 上游失败导致跳过 |
| `QS-TL-DEP-SKIPPED` | 上游被跳过导致跳过 |
| `QS-TL-SKIP-NOINPUT` | 可选阶段自身输入缺失 |
| `QS-FMT-* / QS-SOLV-* / ...` | 计算层模块自身错误码，原样透传 |

---

## 七、算例目录约定（UI 需要准备的 dataRoot）

**`dataRoot` 是唯一的数据根。** 所有后端模块——gds2geo（建模）、compact_em（电磁），
以及后续接入的任何模块——输入与输出都在这个目录里完成；任务层不会往后端程序目录写算例数据。
UI 侧只需要维护这一个目录，新增模块也不会改变这个约定。

```
dataRoot = <部署根>/cases/<项目名>/DATA
```

**`peec_basic` 的算例布局**：

```text
<case>\DATA\
├── Model\      主网格 *.msh、matdb.json、port.json
├── Setting\    set.txt（FS/FE/N_FP/DIM ...）
└── Result\     后端产出；任务层会在 Result\tasks\<taskId>\ 下写 state.json 与 task.log
```

`port.json` 必须与主网格绑定（`meshFile` + `meshSha256`），改动网格后要重新生成端口文件，否则提取阶段会报文件损坏类错误。

**`gds_full` 的算例布局**（输入是版图而不是网格）：

```text
<case>\DATA\
├── Model\      model.gds + model_info.csv（或 model_info.xtf，二选一）
└── Result\
    ├── source\    源模型：model.geo / model.json / matdb.json
    ├── prepared\  派生几何：em.geo / em.json / thermal.geo / thermal.json / matdb.json
    ├── em\        电磁网格：model_em.msh + matdb.json
    └── thermal\   热网格：model_thermal.msh + matdb.json（可选阶段）
```

注意：gds2geo 的日志（`source-attempts` 等）写在它自己目录的 `gds2geo\DATA\runtime\` 下，
不在算例目录里；任务层自己的 `task.log` 和 `state.json` 始终在算例的 `Result\tasks\<taskId>\` 下。

---

## 八、Qt 客户端骨架（可直接抄）

```cpp
// TaskLayerClient.h
class TaskLayerClient : public QObject {
    Q_OBJECT
public:
    void start(const QString& exePath) {
        m_proc.setProgram(exePath);
        m_proc.setArguments({"serve"});
        m_proc.setWorkingDirectory(QFileInfo(exePath).absolutePath());
        m_proc.setProcessChannelMode(QProcess::SeparateChannels);   // 必须
        connect(&m_proc, &QProcess::readyReadStandardOutput, this, &TaskLayerClient::onStdout);
        connect(&m_proc, &QProcess::readyReadStandardError,  this, &TaskLayerClient::onStderr);
        m_proc.start();
    }
    void stop() {
        send("shutdown");
        m_proc.waitForFinished(10000);
        if (m_proc.state() != QProcess::NotRunning) m_proc.kill();
    }

    int runTask(const QString& pipeline, const QString& dataRoot,
                const QJsonObject& inputs = {}) {
        return send("task.run", QJsonObject{{"pipeline", pipeline},
                                            {"dataRoot", dataRoot},
                                            {"inputs", inputs}});
    }
    int query(const QString& method, const QJsonObject& params = {}) {
        return send(method, params);
    }

signals:
    void response(int id, const QJsonValue& result);
    void error(int id, int code, const QString& message, const QJsonObject& data);
    void notified(const QString& method, const QJsonObject& params);

private:
    int send(const QString& method, const QJsonObject& params = {}) {
        const int id = ++m_nextId;
        m_proc.write(QJsonDocument(QJsonObject{{"jsonrpc","2.0"}, {"id",id},
                                               {"method",method}, {"params",params}})
                         .toJson(QJsonDocument::Compact) + '\n');
        return id;
    }
    void onStdout() {
        while (m_proc.canReadLine()) {
            const QJsonObject o = QJsonDocument::fromJson(m_proc.readLine()).object();
            if (o.contains("method")) {           // 通知
                emit notified(o["method"].toString(), o["params"].toObject());
            } else if (o.contains("error")) {     // 错误响应
                const QJsonObject e = o["error"].toObject();
                emit error(o["id"].toInt(), e["code"].toInt(),
                           e["message"].toString(), e["data"].toObject());
            } else {                              // 成功响应
                emit response(o["id"].toInt(), o["result"]);
            }
        }
    }
    void onStderr() { qWarning() << "[task_layer]" << m_proc.readAllStandardError(); }

    QProcess m_proc;
    int m_nextId = 0;
};
```

---

## 九、对接注意事项（容易踩的坑）

1. `dataRoot` 传绝对路径；路径里可以用 `/`，任务层会按平台转换分隔符。
2. 一条 `task.run` 只负责启动；完成判定看 `task.finished`，不要同步等待。
3. 同一进程不支持并发任务，UI 要么串行，要么为每个任务起一个 `task_layer.exe` 进程。
4. `task.stage.log` 频率高，UI 侧要做缓冲/限流，避免日志窗卡死主线程。
5. 退出前发 `shutdown`；直接杀进程后端程序可能残留。
6. 后端程序报的错是**中文文本 + 错误码**，UI 用错误码分支，文本只用于展示。
7. 部署到中文目录没问题，但不要把 `config\` 改名或拆散。

---

## 十、联调自查

```powershell
# 1) 任务层自检（不接 UI）
.\task_layer.exe modules
.\task_layer.exe pipelines
.\task_layer.exe run --pipeline peec_basic --data-root ..\compact_em\DATA
.\task_layer.exe run --pipeline gds_full   --data-root ..\gds2geo\DATA

# 2) 协议自检（模拟 UI，需要 Python）
python .\tools\rpc_smoke.py --exe .\build\task_layer.exe --data-root ..\compact_em\DATA
```

`rpc_smoke.py` 走完 ping / modules.list / pipelines.list / inputs.validate /
task.run / task.finished / task.status / task.result 八项断言并打印 `RESULT: PASS`，
UI 侧可用它先确认部署没问题，再接自己的进程封装。

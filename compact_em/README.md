# Compact 准静态 PEEC 工具

面向纯金属自由空间与无损异质介质模型的准静态 PEEC 计算工具，提供参数提取、频域端口求解及可选源/场后处理。本文为功能、输入输出契约与运行方式的统一发布说明；模型包格式为 V1。

## 功能与适用范围

| 程序 | 功能 | 主要输入 | 默认输出 |
| --- | --- | --- | --- |
| `PEEC_Model_Extractor.exe` | 网格/材料/端口校验，P/L 装配与介质修正 | 主网格、MATDB、端口、设置 | 模型包、提取完成记录 |
| `PEEC_Solver.exe` | 频率扫描与多端口 S 参数求解 | 已签发模型、设置 | S CSV、求解完成记录 |
| `PEEC_S_Parameter_Plotter.exe` | S 参数幅度/相位绘图 | S CSV | SVG |
| `PEEC_Post_Processing.exe` | 源量及空间 E/H 场可视化 | 按需保存的模型/源快照 | 未选择图时无输出 |

导体表面按 PEC 近似计算，电导率和厚度只校验，不形成导体电阻或损耗。实际使用的介质体必须无损。正式求解仅支持频域；`Solver_SET=1` 的时域入口不支持。`Taylor>0` 不会启用全波传播求解；几何/积分仍包含单精度兼容路径，不能视为端到端全双精度模型。

## 快速开始

在本项目目录运行以下命令。建议始终显式指定 `--data-root`；路径相对当前工作目录解析。

```powershell
$dataRoot = 'Cases\Standard_Three_File_Input'
pipeline\PEEC_Model_Extractor.exe --data-root $dataRoot
pipeline\PEEC_Solver.exe --data-root $dataRoot
pipeline\PEEC_S_Parameter_Plotter.exe --data-root $dataRoot
```

可用样例：[标准三文件输入](Cases/Standard_Three_File_Input/README.md)、[双层表面端口](Cases/Two_Layer_Surface_Port/README.md)。样例用于流程与内部一致性验证，不代表实测或外部全波精度基准。

## 输入契约

```text
DATA/
├── Model/
│   ├── model_em.msh      # 可改名，必须唯一
│   ├── matdb.json        # 固定名称
│   ├── port.json         # 固定名称
│   └── Space_Mesh.msh    # 可选，仅空间场后处理需要
├── Setting/
│   └── set.txt
└── Result/               # 程序创建
```

### 网格与单位

主网格和采样网格支持 MSH 2.2、4.1 的 ASCII/二进制及 4.0 ASCII。主模型使用一阶三角形、四面体和点，不接受激励线或高阶单元；场采样网格需要一阶三角形。`Space_Mesh.msh` 不参与主网格唯一性选择。

坐标换算为 `x_m = x_mesh / DIM`，主模型与采样网格使用同一比例；例如毫米网格使用 `DIM=1000`。材料厚度独立由 `thicknessUnit` 换算。三角形物理组编号匹配导体表面材料 ID，四面体物理组编号匹配介质体材料 ID；每个材料体必须唯一归属物理组。原始节点/元素 tag 可不连续，端口仍引用原始元素 ID。

### 材料库

`matdb.json` 的根字段为 `schemaVersion: 3` 和 `materials` 数组。各条目必须包含唯一正整数 `id`、非空 `name`、`dimension`、`type`、`properties`；未使用但符合格式的材料条目可保留。

| 用途 | 条目约束 | 必需属性 |
| --- | --- | --- |
| 导体表面 | `dimension: "surface"`、`type: "C"` | 正电导率、正厚度 |
| 介质体 | `dimension: "volume"`、`type: "D"` | 正实数 εr、电导率 0；μr 缺省为 1，给出时为正实数 |

属性使用 `properties.<属性名> = {"model":"constant","unit":...,"value":...}`。电导率属性名为 `conductivity`，单位 `S/m`；相对介电常数为 `relativePermittivity`、相对磁导率为 `relativePermeability`，单位均为 `1`。实际使用体材料的介电常数、磁导率虚部必须为 0。

表面厚度字段 `thickness`、`thicknessUnit` 位于材料条目顶层，单位支持 `m`、`mm`、`um`，不放入 `properties`。模型外部使用隐式真空。完整示例见[样例 MATDB](Cases/Standard_Three_File_Input/Model/matdb.json)。

### 端口定义

`port.json` 必须绑定同一版本的主网格：

```json
{
  "meshFile": "model_em.msh",
  "meshSha256": "<主网格原始字节的SHA-256，64个十六进制字符>",
  "ports": [
    {"impedanceOhm": 120, "direction": [0, 0, 1], "triangleIds": [5370, 5149]}
  ]
}
```

`meshFile` 必须等于主网格文件名；`triangleIds` 为两个不同的原始 Gmsh 三角形元素 ID，范围 `1..2^53-1`，不能使用物理组编号或内部面下标。端口接入对应三角面电位节点，不做子面插值。`direction` 为单位向量，与第一面心指向第二面心的连线具有正投影。`impedanceOhm` 为有限正参考阻抗，单位 Ω。端口数组顺序决定结果端口顺序。

示例中的 ID、方向和哈希必须按实际网格替换。网格变化后，应同步更新元素 ID 与哈希。`--export ports` 可生成映射审计 CSV。

### 计算设置

`set.txt` 使用大小写敏感的 `KEY value` 格式，允许整行 `#` 或 `//` 注释，可用 `$End_Def` 显式结束；不在数值后追加行内注释。未知键、重复键和非法数值会报错。

```text
Solver_SET 0
FS 1e9
FE 10e9
N_FP 100
DIM 1000
PRECISION 0
MNA_WEIGHT 0.8
$End_Def
```

| 参数 | 默认/要求 | 含义 |
| --- | --- | --- |
| `FS`、`FE` | 必填，`0 < FS <= FE` | 起止频率，Hz；64 位正整数，可用整数科学计数法 |
| `DIM` | 必填，有限正数 | 网格坐标到米的除数 |
| `N_FP` | 计数模式必填，正整数 | 含两端的总点数 |
| `PRECISION` | `0` | 0 按点数；正数按步长 |
| `Swap_Log` | `0` | 0 线性；1 对数 |
| `MNA_WEIGHT` | `0.8`，正数 | MNA 无功项权重；应沿用已确认算例值 |
| `Solver_SET`、`Taylor` | 均为 `0` | 正式频域求解、静态提取设置 |
| `SOLVER_MEMORY_MB` | `0` 自动 | 模型读入后的新增分配预算，MiB；不是进程硬内存上限 |
| `SOLVER_THREADS` | `0` 自动，最大 4096 | 频点 worker 上限，仍受内存预算与 CPU 限制 |

### 频率采样

`PRECISION=0` 时按 `N_FP` 生成包含 FS/FE 的频点；`N_FP=1` 只取 FS，FS=FE 只保留一点。`PRECISION>0` 时由步长决定点数：线性单位为 Hz，对数为 decade；不能整除时追加 FE。序列必须正、有限、严格递增；无法用 double 区分的频点及超预算的点数会被拒绝。上述示例生成 1–10 GHz 共 100 点。

## 输出契约

在新的 DATA 目录完成提取、求解后，默认 `Result/` 包含以下四个文件；成功运行不创建空日志。

| 文件 | 内容与约束 |
| --- | --- |
| `model.peec` | V1 无压缩模型包：完整 float64 P/L、B2N、端口和坐标/假设元数据；L 单位 H，P 单位 1/F |
| `extract.state` | 提取输入及本次产物指纹、完成状态 |
| `S_parameter.csv` | `frequency_Hz` 与各 Sij 的 `_real`、`_imag` 列，S 无量纲 |
| `solve.state` | 求解输入及本次产物指纹、完成状态 |

Sij 的 i 为响应端口，j 为激励端口；CSV 名称如 `S1_2_real` 使用 1-based 端口编号。`--s-columns full` 增加 `_dB`、`_deg`；零幅度允许 dB=-inf。Z/Y CSV 的单位分别为 Ω/S。包内节点编号为 0-based，地节点为 -1，不与 CSV 端口编号混用。包布局见[二进制格式附录](OUTPUT_FORMAT.md#modelpeec-v1-二进制布局)。

## 按需导出与命令行

所有程序支持 `--data-root`、`--help`、`--output-profile minimal|legacy`，默认 minimal。Extractor、Solver、Post 另支持 `--pause`。列表参数使用无空格的逗号分隔值。

| 程序 | 选项 | 结果/行为 |
| --- | --- | --- |
| Extractor | `--matrix-format`，值为 `package` / `bin` / `txt` / `both` | 默认包；其余显式选择 P/L 散文件及 B2N/PORT |
| Extractor | `--export post,ports,report,spice` | 可组合：Post_Model、端口映射、坐标/假设报告、SPICE 网表 |
| Solver | `--export post,z,y,touchstone,legacy-text,diagnostics` | 可组合：源快照、Z/Y、Touchstone、旧文本、逐频点诊断 |
| Solver | `--s-columns`，值为 `ri` / `full` | minimal 默认 RI，legacy 默认 full；完整列另含幅度/相位 |
| Solver | `--post-freq-index n --post-port p` | 1-based 快照选择；频点 -1 表示最后一点，默认最后点/第一端口；minimal 下须同时 `--export post` |
| Plotter | `--format svg,png` | 选择 SVG、PNG 或两者；显式 PNG 需要 GNUplot |
| Plotter | `--export-gnuplot` | 保留 DAT/GP；`--script-only` 保留旧 SVG/DAT/GP 集合，不可与 `--format` 同用 |
| Plotter | `--input path --output-dir path --gnuplot path` | 指定 CSV、图目录与 GNUplot；更多选项见 `--help` |
| Post | `--source-plots j,j_vec,qe,ve,m,qm,vm` | 选择源视图；j_vec 为 j 的别名；磁源接口保留，当前 Solver 的缺省磁源为零 |
| Post | `--field-plots e,h --parts real,imag,mag` | 选择场及数据分量；parts 也用于源图，默认 real,imag |
| Post | `--export field-materials` | 按需保存场材料审计 CSV，须选择场图 |

后处理需在两个上游阶段分别保留模型和源快照：

```powershell
pipeline\PEEC_Model_Extractor.exe --data-root $dataRoot --export post
pipeline\PEEC_Solver.exe --data-root $dataRoot --export post,touchstone
pipeline\PEEC_Post_Processing.exe --data-root $dataRoot --source-plots j,qe,ve --parts mag
# 电流密度矢量（实部、虚部）
pipeline\PEEC_Post_Processing.exe --data-root $dataRoot --source-plots j_vec --parts real,imag
# 空间场另需 Model/Space_Mesh.msh
pipeline\PEEC_Post_Processing.exe --data-root $dataRoot --field-plots e,h --parts real,imag
```

`j` 与 `j_vec` 等价，同时指定只输出一组视图。`--parts real,imag` 输出三分量矢量视图 `J Real`、`J Imaginary`；`--parts mag` 输出标量 `J Magnitude`，即 `sqrt(|Jx|²+|Jy|²+|Jz|²)`，不以分量绝对值代替方向。legacy 下对应文件为 `J_vec.msh_Re`、`J_vec.msh_Im`、`J_vec.msh_Mag`（仅输出所选分量）。

minimal 源图、场图分别为 `Source_Plot/Source.msh`、`Field_Plot/Field.msh`，每份网格只写一次，所选分量为独立视图；再次生成同一源/场文件时按本次选择重写，不追加视图。Gmsh 图坐标保持输入网格单位。`Post_Model.txt` 内部几何记录使用米，`Post_Source.txt` 只保存选定频点/端口，不保存全频源场快照。S 图位于 `S_Parameter_Plot/`，默认仅 SVG。没有请求源/场图时，Post 不创建图目录。

## 状态、兼容与诊断

- 下游按 READY 完成记录及指纹校验结果；IN_PROGRESS、失败或输入变化的结果不能作为新的已完成结果。修改模型或 set.txt 后须重新提取、求解。
- 同一 DATA 通过 `.peec-data.lock` 的运行句柄限制并发写入；零字节锁文件可持久存在，不需手工删除。只登记本次成功产物，禁用输出不删除历史文件。
- minimal 不由旧 `SAVE_*`、`POST_*` 设置隐式开启文件输出；legacy 读取旧设置并恢复兼容输出集合。显式格式、CSV 列和 Post 视图选择优先，`--export` 在所选策略上追加导出。
- 旧 Solver/工具需用 `--matrix-format bin|txt|both` 导出兼容散文件，不能直接读取新包。无完成记录的旧结果仅作结构校验并警告，不补造历史完成证据。
- 成功退出码为 0，失败为非零；stderr 为 UTF-8 JSON，包含 `code`、`module`、`trace_id` 等字段。可定位且可写时保存错误日志至 `Result/Logs/`，机器应按 code 判断，见[错误码表](ERROR_CODES.md)。

## 构建与发布

源码构建要求 Windows x64、Visual Studio 2022/MSVC v143 和 Intel oneMKL；通过 `MKLROOT` 或 `ONEAPI_ROOT` 提供安装位置。运行 `.\build_pipeline.ps1 -Rebuild` 生成 `pipeline/` 四个程序。SVG 无需外部绘图软件，PNG 需要 GNUplot；可用 `--gnuplot`、`GNUPLOT_EXE` 或 PATH 指定。源/场图使用 Gmsh 查看。

源码按 Model_Extractor、Solver、S_Parameter_Plotter、Post_Processing 分工，Common 为输入校验与交接支持；绘图核心见[共享模块](../Shared/S_Parameter_Plotter/README.md)。源码包必须保留仓库相对目录中的 `scripts/` 和 `01_Mainline/Shared/`，仅复制 Compact 目录不能构建。运行包应附四个程序、本说明、格式附录、错误码及可运行样例，并核对运行依赖。

数值适用性仍受准静态近似、几何精度与已知互易性边界限制，内部回归不等同于外部电磁精度验收。源码仓库中的[验证记录](../../docs/COMPACT_OUTPUT_OPTIMIZATION_2026-10-24.md)与[开发历史](../../docs/COMPACT_DEVELOPMENT_NOTES_2026-10-24.md)供维护追溯，不改变本文发布契约。

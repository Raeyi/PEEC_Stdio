# GDS2GEO 建模与网格流水线

本运行包按 `DATA/Model`、`DATA/Setting`、`DATA/Result` 组织输入、JSON 设置和结果，使用 GDS2GEO 自身的源模型、几何准备及热/电磁网格算法。运行顺序为 `gds2geo` → `geo_prepare` → `geo2thermal` / `geo2em`。最后两步可按需选择，均不执行物理场求解。

## 程序与目录

| 程序 | 职责 | 主要输入 | 正式输出目录 |
| --- | --- | --- | --- |
| `gds2geo.exe` | 解释 GDS 和工艺，生成源物理模型 | GDS、CSV 或 XTF | `DATA/Result/source/` |
| `geo_prepare.exe` | 统一近似并派生热/电磁几何 | 完整 source 包 | `DATA/Result/prepared/` |
| `geo2thermal.exe` | 生成并校验热网格 | 热目标 GEO、JSON、MATDB | `DATA/Result/thermal/` |
| `geo2em.exe` | 生成并校验电磁网格 | EM 目标 GEO、JSON、MATDB | `DATA/Result/em/` |

```text
GDS2GEO_pipeline/
├── gds2geo.exe
├── geo_prepare.exe
├── geo2thermal.exe
├── geo2em.exe
├── gmsh.exe                    # 4.11.1
├── <配套运行库 DLL>
├── README.md
├── ERROR_CODES.md
└── DATA/
    ├── Model/
    │   ├── model.gds
    │   └── model_info.csv       # 或 model_info.xtf，两者只能保留一份
    ├── Setting/
    │   └── gds2geo_setting.json
    ├── Result/                 # 程序创建各阶段子目录
    └── runtime/                # 日志、程序锁、数据目录锁与导入诊断
```

四个 EXE、配套 DLL 和 Gmsh 应一同保留。运行包可整目录搬迁，所在目录须允许程序写入结果、日志和暂存文件。

随包输入为已验证的单 HEATER 流程示例：`model.gds` 为 7,874 字节，CSV 映射 `19/0` 为 HEATER，厚度为 0.0001 mm、底面高度为 0.00985 mm；示例 SiO2 厚度为 0.001 mm、底面高度为 0.00935 mm。该 HEATER 低于 1 µm 薄层阈值，准备后以材料片表达；热目标保留介质回填，EM 目标剔除 D/I。这些数值仅描述随包案例，不是其他工艺的默认值，也不作为外部物理精度或求解器验收基准。

## 快速开始

保留随包示例输入及 JSON 设置，在运行包目录打开 PowerShell，逐步运行。每步成功后再运行下一步：

```powershell
.\gds2geo.exe
if ($LASTEXITCODE -ne 0) { throw 'gds2geo failed' }
.\geo_prepare.exe
if ($LASTEXITCODE -ne 0) { throw 'geo_prepare failed' }
.\geo2thermal.exe
if ($LASTEXITCODE -ne 0) { throw 'geo2thermal failed' }
.\geo2em.exe
if ($LASTEXITCODE -ne 0) { throw 'geo2em failed' }
```

也可按上述顺序双击四个 EXE。资源管理器创建的独占控制台窗口在结束后等待回车；已有终端或重定向运行不等待。仅需一种网格时，可省略另一个网格程序。

替换案例前，将原输入及配套结果一同另存。在 `DATA/Model/` 放入新的 `model.gds` 和唯一的 `model_info.csv` 或 `model_info.xtf`，再从第一步重新运行。多个顶层单元或不同输入文件名请使用下方显式命令。

## JSON 设置

`DATA/Setting/gds2geo_setting.json` 内容为：

```json
{
  "input_path": "DATA/Model",
  "output_path": "DATA/Result"
}
```

设置保持 JSON 格式，只接受上述两个非空目录字符串。支持绝对路径；相对路径始终从 EXE 所在目录解析，与当前工作目录和 Setting 文件所在目录无关。Windows 路径可写成 `D:/cases/demo/Model`；使用反斜杠时须遵循 JSON 转义规则。

四个程序的自动路径入口在以下两个位置选择唯一配置：

- EXE 同目录的 `gds2geo_setting.json`，兼容原布局；
- EXE 目录下的 `DATA/Setting/gds2geo_setting.json`，本运行包使用此位置。

两处同时存在会报歧义并返回 `2`，不会按优先级猜选。两处均不存在时兼容 EXE 目录下的 `input/`、`output/`。配置存在但 JSON 无效、字段缺失/重复/未知或路径无效时明确失败，不回退到另一案例。路径配置不接受网格尺寸、求解频率、端口或材料参数。

## 输入与单位

- 无参入口固定读取 `model.gds` 和唯一的 `model_info.csv` / `model_info.xtf`，不会自动挑选其他文件名。
- GDS 使用自身 `UNITS` / DBU。CSV 的 `Height`、`Thickness` 固定按 mm 解释；XTF v2/v3 按其 `LengthUnit` 解释。正式 GEO 和 MSH 坐标统一为 mm。
- CSV 和 XTF 分别进入自身解析器，共用源模型生成核心；XTF 不先转成 CSV。XTF 默认 `strict-slots`，`embedded-top` 仅能显式选择，失败时不自动切换。CSV 不支持 `embedded-top`。
- 无参入口要求 GDS 顶层单元唯一；多顶层用 `source --top-cell` 明确选择。工艺映射、材料和接触信息须满足实际导入校验，不能仅凭层名推断。
- 源阶段建立本案例的 MATDB，不要求用户提供已有材料库。缺少热导率、密度或比热时会提示；这些物性不会被编造，正式热求解前仍须补齐。
- 默认四阶段入口不生成端口；旧 `ports.csv`、`model_ports.csv`、`mesh.json` 不控制本流程。任意手写 GEO 也不属于目标包读取合同。

## 当前算法与适用范围

`gds2geo` 保留源规则的轮廓、孔、原 Z、真实厚度、材料与 Via 关系，构造完整三维源模型；该阶段不做有界轮廓近似、薄层降维或网格剖分。源介质 XY 域以有效映射版图的包围盒为基准，长宽各取 1.4 倍、中心不变，并按工作网格向外取整。无限工艺层不被猜成有限空气盒；当前 source 入口不支持每层显式 `GDS2GEO_Bounds`。

当前 `geo_prepare` 输出策略为 `four-executable-5`：

- 热与 EM 共用一次 500 nm（0.0005 mm）有界轮廓近似，保留孔、组件和材料接触；500 nm 是轮廓误差上限，不是网格边长。
- 原厚度严格小于 1 µm 的 C 导体在两个目标中取原下表面作为材料薄片；MATDB 保留原厚度，整体 Z 坐标不压缩。厚 C 与 Via 保留体表达。
- 先完成 priority 归属、合法 Via 接触处理及热模型的 D/I 介质回填，再仅从最终 EM 目标剔除 MATDB 类型为 D/I 的实体；不按名称筛选、不补空气、不重新分配导体空间。完全空的 EM 模型会使准备失败。
- 热与 EM 各自基于配套的三维体/材料片几何独立生成自由 Tet4 和材料 Tri3；纯片 EM 可只有 Tri3。两套网格的节点和实体编号不要求相同。

只有两个网格阶段调用 Gmsh，版本必须为 4.11.1。查找顺序为 EXE 同目录、`GDS2GEO_GMSH_EXECUTABLE` 指定的绝对路径、`PATH`。生产配置关闭 Gmsh 优化、平滑和形状质量拒绝门槛，保留有限坐标、正体积、材料覆盖和真实接触等结构校验。网格子进程有 4 GiB 内存预算及 600 秒时限，MSH 大小上限为 1 GiB。

输出网格为一阶 MSH 2.2 ASCII。默认只保留材料体/片，不输出全量非材料外壁/界面单元；热上下边界可另行导出。热/电磁模型是当前算法定义的降维与几何准备结果，仍需下游按真实材料、端口、边界和频率进行求解与收敛验证。本包不计算温度、电磁场、S 参数或热源功率。

## 正式输出与配套关系

```text
DATA/Result/
├── source/
│   ├── model.geo
│   ├── model.json
│   └── matdb.json
├── prepared/
│   ├── thermal.geo
│   ├── thermal.json
│   ├── em.geo
│   ├── em.json
│   └── matdb.json
├── thermal/
│   ├── model_thermal.msh
│   ├── model_thermal.msh.json
│   └── matdb.json
└── em/
    ├── model_em.msh
    ├── model_em.msh.json
    └── matdb.json
```

源包、准备包和每个结果包均是整体交接文件组。GEO、规范 JSON 与 MATDB 会核对几何、材料和摘要；不要单独修改或跨代拼接。`*.msh.json` 绑定生成时的目标 GEO、JSON、MATDB 和 MSH 的 SHA-256。不同阶段的材料库可能含派生记录，不能仅凭同名文件或相同 ID 判断配套。

source 三文件搬迁后可在没有原 GDS/工艺的环境准备几何。仅携带 `thermal.geo`、`thermal.json`、准备库 `matdb.json` 即可独立生成热网格；EM 同理，不需要另一目标文件。每个网格结果目录保存独立材料快照。

各阶段分别成组发布，失败保留该阶段上一组可用结果；不会自动重跑下游或删除旧结果。上游重建后，仍存在的旧网格不代表已经更新，应重跑所需下游并检查配套 receipt。旧 v1–v4 目标按原策略读取；对已有 source 重跑 prepare 和网格才会应用当前 v5。

## 显式命令与独立交接

以下命令从运行包目录执行。显式相对路径按当前工作目录解析；指定本阶段完整输入路径时不读取路径设置 JSON：

```powershell
.\gds2geo.exe source --gds DATA/Model/model.gds --tech DATA/Model/model_info.csv --output-dir DATA/Result/source
.\geo_prepare.exe --input-dir DATA/Result/source --output-dir DATA/Result/prepared
.\geo2thermal.exe --input-dir DATA/Result/prepared --output-dir DATA/Result/thermal
.\geo2em.exe --input-dir DATA/Result/prepared --output-dir DATA/Result/em
```

XTF 案例将 `--tech` 改为实际 `.xtf` 路径。`source` 要求 `--gds`、`--tech`、`--output-dir`；可增加 `--top-cell 单元名`，XTF 可显式使用 `--stack-mode strict-slots|embedded-top`。

后三程序的 `--output-dir` 必须配合 `--input-dir`，省略时分别写到输入目录父目录下的 `prepared/`、`thermal/`、`em/`。输入、输出目录须不同。显式 `convert`、`import-tech` 保留兼容用途，不替代上述四阶段交接。各程序的 `--help`、`--version` 返回实际用法和版本，本包没有 `--data-root` 选项。

热边界及源选择可以按需导出：

```powershell
.\geo2thermal.exe export-thermal-entities --mesh DATA/Result/thermal/model_thermal.msh --out DATA/Result/thermal/model_thermal_entities.json
```

该命令列出实际材料体/片的实体 ID，缺省不选择热源。确认本案例的实体后，用重复的 `--source-entity ID` 或 `--source-surface-entity ID` 选择体或材料片；ID 是 MSH 第二 tag，不是材料 ID。输出绑定原网格 SHA；上下边界指整体最低/最高 Z 处的外面。此操作不修改网格，也不设置功率、温度或对流条件。网格重建后应重新导出。

## 日志、故障与重跑

无参运行将完整控制台信息和退出状态覆盖写入 `DATA/runtime/gds2geo.log`、`DATA/runtime/geo_prepare.log`、`DATA/runtime/geo2thermal.log`、`DATA/runtime/geo2em.log`。源导入的独立尝试报告保存在 `DATA/runtime/source-attempts/`。配置 JSON 无效时，日志仍保存在这里。显式命令直接写控制台，不覆盖这些无参日志，可按需在终端重定向。

本便携布局的程序级锁和数据目录共锁统一位于 `DATA/runtime/`。数据锁按相对 DATA 的目录层级存放，例如 source 包使用 `DATA/runtime/locks/Result/source/.gds2geo-model.lock`；不同 EXE 目录访问同一数据包仍共用同一把锁，同名但父目录不同的数据包也能区分。外部数据的共锁仍按数据目录定位；其规范父目录为便携包根时也归入 `DATA/runtime/`。没有 `DATA/Setting` 布局的旧程序目录保持原 runtime 规则。

锁文件退出后可以保留，不能以空锁文件判断仍在运行，也不要在运行中删除。`.pending` 是程序私有暂存目录，正常退出会清理本次暂存。

成功退出码为 `0`，失败为非零；当前诊断为文本，含程序名和原因，不采用 Compact 的 JSON 错误码合同。先核对失败步骤对应日志，再按 [错误与退出码](ERROR_CODES.md) 修正输入或环境后重跑。不要通过改名、手工重算摘要或删除配套元数据绕过校验。

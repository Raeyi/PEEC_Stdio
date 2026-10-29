# GDS2GEO CLI 集成包

本包通过完整命令行参数调用四个程序，依次生成源模型、准备几何及热/电磁网格，不执行物理场求解。路径设置文件 `gds2geo_setting.json` 已移除；下述完整显式流程不读取也不生成该文件。

四个 EXE 与 `gmsh.exe`（4.11.1）放在同一安装目录，可由宿主软件直接启动。当前正式交付将 GCC、C++ 标准库和线程运行库静态链接到四个 EXE，无须附带 `libgcc_s_seh-1.dll`、`libstdc++-6.dll`、`libwinpthread-1.dll`；仍依赖 Windows 系统 DLL，网格阶段仍需 Gmsh 4.11.1。安装目录可以只读；输入、source/prepared 交接包、网格结果和运行记录使用外部案例工作区，正式输出及对应数据共享锁位置须可写。

历史动态版或 `shared` 兼容包仍需上述三份匹配 DLL。升级时一并替换四个 EXE 并保留 Gmsh，不能直接删除旧动态版的 DLL 来代替静态构建。

包中已有的 `DATA/Model/`、`DATA/Result/`、`DATA/runtime/` 保留为示例与历史记录，不是本 CLI 流程自动选择的路径。使用既有示例时，先将所需原始 GDS 和工艺文件复制到自己的案例输入目录；不要混用历史结果与新结果。

安装包内原有的 `DATA/runtime/` 可以继续保留。只要四个 EXE 都传入外部 `--runtime-dir`，且原始输入、source/prepared 交接包和输出也使用外部案例目录，运行就不会使用包内的 `DATA/runtime/`。它不是集成交付的必需目录，可在确认相关程序全部退出后选择不随包交付。

## 四步完整调用

以下示例假定程序安装在 `C:/Tools/GDS2GEO_pipeline_20261028/`，案例输入在 `D:/Cases/demo/Model/`。将这些绝对路径换成实际位置后，可从任意工作目录运行：

```powershell
& "C:/Tools/GDS2GEO_pipeline_20261028/gds2geo.exe" --runtime-dir "D:/Cases/demo/runtime" source --gds "D:/Cases/demo/Model/model.gds" --tech "D:/Cases/demo/Model/model_info.csv" --output-dir "D:/Cases/demo/Result/source"
if ($LASTEXITCODE -ne 0) { throw 'gds2geo failed' }
& "C:/Tools/GDS2GEO_pipeline_20261028/geo_prepare.exe" --runtime-dir "D:/Cases/demo/runtime" --input-dir "D:/Cases/demo/Result/source" --output-dir "D:/Cases/demo/Result/prepared"
if ($LASTEXITCODE -ne 0) { throw 'geo_prepare failed' }
& "C:/Tools/GDS2GEO_pipeline_20261028/geo2thermal.exe" --runtime-dir "D:/Cases/demo/runtime" --input-dir "D:/Cases/demo/Result/prepared" --output-dir "D:/Cases/demo/Result/thermal"
if ($LASTEXITCODE -ne 0) { throw 'geo2thermal failed' }
& "C:/Tools/GDS2GEO_pipeline_20261028/geo2em.exe" --runtime-dir "D:/Cases/demo/runtime" --input-dir "D:/Cases/demo/Result/prepared" --output-dir "D:/Cases/demo/Result/em"
if ($LASTEXITCODE -ne 0) { throw 'geo2em failed' }
```

每步退出码必须为 `0` 后再运行下游。两个网格程序按示例串行运行，可只选择所需的一种。任一步失败时停止后续调用，保留完整输出并修正原因；磁盘上仍存在的旧结果不能证明本次成功。

`--tech` 可改成实际 `.xtf` 文件；CSV 与 XTF 只选择一份。GDS 使用自身单位，CSV 的 Height/Thickness 按 mm 解释，XTF v2/v3 按 LengthUnit 解释，正式 GEO/MSH 坐标为 mm。GDS 多顶层时，给第一步增加 `--top-cell 实际单元名`；XTF 默认 `strict-slots`，需要 `embedded-top` 时须显式传入 `--stack-mode embedded-top`，CSV 不支持该模式。

四个程序均须为支持 `--runtime-dir` 的新版本，升级时一起替换。显式传入的 `--gds`、`--tech`、`--input-dir`、`--output-dir`、`--runtime-dir` 相对路径均按进程当前工作目录解析，宿主集成应统一传入绝对路径；正式输出由各步 `--output-dir` 指定。

## 宿主进程调用

上面每行开头的 `&` 是 PowerShell 调用运算符。通过 `.NET ProcessStartInfo` 集成时，将绝对 EXE 路径放入 `FileName`，设置 `UseShellExecute = false`，将选项名及其值逐项加入 `ArgumentList`；不要把 `&`、PowerShell 代码或路径外层的命令行引号当作 EXE 参数。需要捕获输出时，设置 `RedirectStandardOutput = true` 和 `RedirectStandardError = true`，在运行期间并发异步读取并保存 stdout/stderr，避免管道写满阻塞；等待进程结束、完成两路读取并检查真实退出码后，再启动下一步。

本包不使用无参运行或双击默认模式。当前二进制仍保留原有兼容入口：无参数或仅传 `--runtime-dir` 时仍会进入默认路径流程；缺少设置文件时回退到 EXE 旁的 `input/`、`output/`。集成调用请按示例完整指定输入、输出和运行目录，以固定所有数据位置。后三步虽支持省略 `--output-dir` 后使用默认目标目录，本集成示例始终显式提供它。删除设置文件没有在编译层面禁用自动模式。

## 结果与 JSON 交接文件

| 阶段 | 输出文件组 |
| --- | --- |
| source | `model.geo`、`model.json`、`matdb.json` |
| prepared | `thermal.geo`、`thermal.json`、`em.geo`、`em.json`、`matdb.json` |
| thermal | `model_thermal.msh`、`model_thermal.msh.json`、`matdb.json` |
| em | `model_em.msh`、`model_em.msh.json`、`matdb.json` |

移除的只是路径设置 JSON。模型元数据、MATDB 和网格 receipt 仍是必需的 JSON 文件，必须与所属阶段的 GEO/MSH 成组保留，不改成 TXT，也不跨代拼接。消费端会校验几何、材料和文件摘要；单独改名、编辑或重算普通摘要不能替代重新生成配套结果。

source 三文件足以供第二步使用，无须重新读取原 GDS/工艺。热网格只需要本目标 `thermal.geo`、`thermal.json` 和 prepared 的 `matdb.json`；EM 同理。各阶段分别发布文件组，失败保留上一组可用正式结果。上游重建不会自动重跑下游，应重新生成所需后续阶段。

当前准备策略为 `four-executable-5`：共用 500 nm 有界轮廓近似；原厚度严格小于 1 µm 的 C 导体取原 bottom 面，MATDB 保留厚度，整体 Z 不压缩。热保留介质回填，EM 在完整材料归属后剔除 D/I，不补空气域。网格使用自由 Tet4 与材料 Tri3，纯片 EM 可只有 Tri3；本包不计算温度、场或 S 参数。

## 日志、暂存与锁

四个 EXE 每次调用都应传入本案例的 `--runtime-dir`。指定目录不存在时，程序会自动创建，宿主须有创建目录及写入权限；不必提前建立空文件夹。运行时仍需要这个可写目录保存日志、程序私有锁和暂存文件。该参数不设置输入或输出路径，输入与输出仍由各步的完整参数指定；省略它也不会自动关闭运行记录或暂存功能。

示例中的运行记录写入 `D:/Cases/demo/runtime/`，四份日志分别为 `gds2geo.log`、`geo_prepare.log`、`geo2thermal.log`、`geo2em.log`，每次覆盖本程序上次日志。源导入尝试保存在该目录的 `source-attempts/`；程序私有锁与 `work/.pending/` 暂存也使用此运行目录。正常退出会清理本次私有暂存，日志和诊断供排错保留。`--help`、`--version` 不创建运行目录。

数据包共享锁由数据规范路径确定，不随 `--runtime-dir` 改变。上述普通案例布局会在 `D:/Cases/demo/Result/runtime/` 保存各阶段数据锁，例如 `source.gds2geo-model.lock`；具有 `DATA/Setting/` 的既有布局仍沿用按 DATA 层级定位数据锁的规则。即使两个进程传入不同运行目录，访问同一数据包仍须共享互斥。source/prepared 也应放在外部案例工作区，不能仅靠运行目录参数消费整棵只读安装树内的交接包。

空锁文件退出后可保留，不表示进程仍在运行；不要在运行期间删除锁文件。日志、临时目录或共享锁不可写时，根据实际诊断修正工作区权限，不通过删除元数据绕过校验。失败退出码和常见诊断见 [ERROR_CODES.md](ERROR_CODES.md)。

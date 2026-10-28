# Compact CLI 错误码表 v0.1

登记日期：2026-10-22。依据用户提供的《QS-PEEC 错误码与异常处理规范 v0.1》，仅登记当前 CLI 实际使用的子集及以下增量。本文档是本仓库版本化登记，未改写微信附件或历史交付包。

`Common/QSError.h` 定义 `qs::Error` 与统一顶层序列化。失败退出码为非零；机器判别使用 JSON 的 `code`，不解析英文字符串。stderr 与 `Result/Logs/<module>-<trace_id>.jsonl` 的错误记录相同，均为 UTF-8。输出目录未解析或不可写时至少保留 stderr；日志写失败另发 `QS-INTERNAL-LOG-NOT_WRITABLE` warning。每次程序调用生成一个 trace_id，消息包含 module、severity、message_dev、message_user_zh、user_action、file_refs、recoverable。`user_action` 是动作键，当前 CLI 不实施前端 UI。

已识别业务错误 severity=error；未知 std::exception 与非标准异常兜底 `QS-INTERNAL-UNHANDLED_EXCEPTION`，severity=fatal。bad_alloc 归 `QS-SOLV-MEMORY-OUT_OF_MEMORY`。recoverable 表示修正输入/环境后可重新启动 CLI，并非在半成品上自动续跑。未分类内部不变量保留内部异常，不根据任意文本猜测原因。

## 本地增量及触发条件

| 新登记错误码 | 触发条件 |
| --- | --- |
| `QS-BC-PORT-MAPPING_AMBIGUOUS` | `port.json` 的方向与 `triangleIds` 顺序相反；历史几何端点的多重匹配记录属于旧输入。 |
| `QS-BC-PORT-SHORTED` | PORT记录两端相同，或几何映射后两端是同一实际电位节点。 |
| `QS-COMM-DATA-STALE` | 阶段记录的输入/输出指纹与当前文件不同，或执行期间输入变化。 |
| `QS-FMT-INPUT-AMBIGUOUS` | 输入目录含多个网格/材料候选，无法唯一选择。 |
| `QS-FMT-MSH-INVALID` | 共享网格读取器拒绝格式、元素或标签契约；不冒称所有失败均为版本问题。 |
| `QS-FMT-SETTING-INVALID` | CLI选项缺值、数值格式、范围、重复项或已移除配置项。 |
| `QS-FMT-SETTING-UNKNOWN_KEY` | 共享设置解析器不认识键名，包括无效PORT_Z0_N后缀。 |
| `QS-IO-FILE-OPERATION_FAILED` | filesystem_error没有可可靠细分的文件系统原因，保留系统error_code与路径。 |
| `QS-IO-FILE-WRITE_FAILED` | 输出打开、flush、close、阶段状态或发布失败，但没有可靠系统原因证据；不伪装磁盘满或权限拒绝。 |
| `QS-MAT-MODEL-UNSUPPORTED` | 模型实际使用有损体材料、非导体表面或不满足导体表面属性约束；当前实 P/L 流程不装配这些模型。正电导导体表面按 PEC 近似计算。 |
| `QS-POST-PLOT-FAILED` | 外部GNUplot执行失败或未生成所要求的文件。 |
| `QS-POST-PORT-OUT_OF_RANGE` | POST_PORT不在已加载端口范围。 |
| `QS-SOLV-MODE-UNSUPPORTED` | Solver_SET=1时域实现尚未验收，CLI在计算前拒绝。 |

## 使用的规范码及对应边界

| 规范错误码 | 当前触发边界 |
| --- | --- |
| `QS-FMT-MSH-PHYSICAL_TAG_MISSING` | 无激励线、无 PEC 三角面、Line/Surface tag 不匹配。 |
| `QS-BC-PORT-FLOATING` | `port.json` 引用了不存在或未映射到 PEC 面节点的导体三角形元素 ID。 |
| `QS-SRC-PORT-COUNT_MISMATCH` | PORT 声明数量不正、缺行或多行。 |
| `QS-SRC-REF_IMPEDANCE-MISMATCH` | 端口阻抗非有限或非正。 |
| `QS-SRC-SWEEP-EMPTY` | 求解实际频率列表为空。 |
| `QS-VAL-NUM-NAN_INF` | 修正后P/L非有限、输入矩阵非有限、求解数值非有限、绘图必须有限的单元无效。 |
| `QS-VAL-DIM-MISMATCH` | 方阵、B2N列数/行数或内部装配尺寸契约不匹配。 |
| `QS-VAL-OUT_OF_RANGE` | 支路端点、扫描或数值参数越界。 |
| `QS-VAL-UNIT-MISMATCH` | 材料属性的单位不符。 |
| `QS-IO-FILE-NOT_FOUND` | 必需设置、模型、材料、矩阵、端口、CSV或所要求的GNUplot可执行文件不存在。 |
| `QS-IO-FILE-PERMISSION_DENIED` | 文件存在但无法打开，限相关输入读取边界。 |
| `QS-IO-FILE-CORRUPTED` | 矩阵/PORT/阶段记录/后处理源格式不完整、尾部垃圾或坏读；也包括 `port.json` 的网格 SHA-256 不匹配。 |
| `QS-IO-FILE-MAGIC_BAD` | 二进制矩阵非PEECMAT1。 |
| `QS-IO-DIR-NOT_WRITABLE` | 结果目录创建失败。 |
| `QS-IO-DISK-FULL` | filesystem_error明确报告no_space_on_device；未用无法分辨原因的写失败冒充磁盘满。 |
| `QS-FMT-JSON-INVALID` | JSON解析器语法拒绝。 |
| `QS-FMT-JSON-SCHEMA_VIOLATION` | 材料schema、属性、重复标识或必需字段无效。 |
| `QS-MAT-CODE-NOT_FOUND` | Volume材料编号未在库中登记。 |
| `QS-SOLV-MATRIX-SINGULAR` | LAPACK返回正info；PN_DD或频域系统不能求解。负info是内部参数契约错误，归QS-INTERNAL-ASSERTION_FAILED。 |
| `QS-SOLV-CONVERGENCE-FAILED` | 介质任一RHS列或逐频MNA任一激励列的尺度化后向误差超过现有1e-8工程接受水平。 |
| `QS-POST-FREQ-POINT_NOT_FOUND` | POST_FREQ_INDEX超出实际采样列表。 |
| `QS-POST-RESULT-NOT_FOUND` | 所需CSV或本次未产出的Post_Source不存在。 |
| `QS-COMM-DEPS-NOT_READY` | 上游无READY、仅有半套矩阵，或同DATA已有活动运行/当前进程已有阶段所有者；锁冲突在改写状态前拒绝。 |
| `QS-SOLV-MEMORY-OUT_OF_MEMORY` | 模型读入后规划预算不能容纳共享数据和一名worker，或顶层捕获bad_alloc；报告路径在再次分配失败时仍以固定完整JSON降级。 |
| `QS-INTERNAL-UNHANDLED_EXCEPTION` | 未分类标准异常/未知异常。 |
| `QS-INTERNAL-ASSERTION_FAILED` | 内部数据契约断言（预留）。 |
| `QS-INTERNAL-LOG-NOT_WRITABLE` | 无法追加JSONL日志，stderr降级。 |

## 文件交接与兼容边界

`extract.state` / `solve.state` 使用 `PEEC_STAGE_V1`，启动先写 IN_PROGRESS，仅在全部计算、输出close与指纹核对完成后发布 READY。完整记录有带数量及trace_id的END尾标记，状态写入临时文件close成功后以Windows替换语义发布；READY不是整组结果的文件系统事务。结果集合发布过程中失败，IN_PROGRESS阻止下游读取混合结果。中间输出保留以便排查。

记录FNV-1a 64位变化指纹，覆盖本次输入/输出；它用于检测旧文件与偶发变更，不代表SHA-256完整性或恶意篡改保护。交付审计另计算SHA-256。路径相对于DATA根目录，整个DATA复制到新目录可核验。extract.state存在时明确B2N/PORT为0-based，-1表示地；无状态文件的历史模型保留原有编号推断并明确警告，不能对缺失历史运行证据保证新契约。新结果不允许手工删除状态文件后混用；若需对照原文件，应在隔离目录运行。

更改网格、材料或set.txt后须重跑上游；当前保守地将整个set.txt纳入指纹。旧结果没有状态记录时只能进行结构验证。外部自定义CSV绘图不绑定默认DATA结果记录，仍检查CSV内容与输出失败。

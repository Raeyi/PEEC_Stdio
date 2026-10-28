// module_api.h — 计算层功能 DLL 标准接口（目标形态）
// 各模块负责人按此接口实现 DLL，任务层通过 worker_host.exe 隔离加载运行。
// 接口约定（C ABI，跨编译器稳定）：
//   int qs_peec_module_entry(const char* job_json_path, const char* data_root,
//                            const char* log_path);
//   返回 0 表示成功；非 0 表示失败（错误详情写入 Result/Logs/ 或 stderr JSON）
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 模块入口：所有计算层 DLL 必须导出此函数
   job_json_path : 任务层下发的任务定义 JSON（含参数、路径、层契约）
   data_root     : 案例数据根目录（Model/Setting/Result）
   log_path      : 日志文件路径（可空，为空则写 stderr）
   返回 0=成功；非 0=失败，错误码写入 job JSON 的 output.errorCode 或日志 */
typedef int (*QsPeecModuleEntry)(const char* job_json_path,
                                 const char* data_root,
                                 const char* log_path);

/* 元信息查询（可选）：返回模块能力描述 JSON（名称、版本、输入契约、输出契约）
   buffer 不足返回所需长度；返回 0 表示未实现。 */
typedef int (*QsPeecModuleInfo)(char* buffer, int buffer_size);

#ifdef __cplusplus
} // extern "C"
#endif

/* 推荐导出名（避免重名冲突）：
     extern "C" __declspec(dllexport) int qs_peec_module_entry(...)
     extern "C" __declspec(dllexport) int qs_peec_module_info(char*, int)
   模块 DLL 放置于 bin/modules/<moduleId>/ 目录。 */

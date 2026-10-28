cmake_minimum_required(VERSION 3.16)
project(task_layer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# 依赖 Qt6 Core（任务层独立于 UI 运行，仅用 Core，不依赖 Widgets）
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)

# 若 Qt 不可用可退化为无 Qt 纯标准库构建（见 README 说明），此处按主路径 Qt 构建
add_executable(task_layer
    src/main.cpp
    src/App.cpp
    src/config/ModuleRegistry.cpp
    src/config/PipelineLoader.cpp
    src/io/InputValidator.cpp
    src/io/ResultStore.cpp
    src/modules/ModuleRunner.cpp
    src/orchestrator/TaskOrchestrator.cpp
    src/rpc/StdioTransport.cpp
)

target_include_directories(task_layer PRIVATE src)
target_link_libraries(task_layer PRIVATE Qt${QT_VERSION_MAJOR}::Core)

# 发布时把 config 拷到可执行文件旁
add_custom_command(TARGET task_layer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/config $<TARGET_FILE_DIR:task_layer>/config
)

# MinGW 构建：把 Qt6Core.dll 拷到产物旁，免设 PATH 即可运行
if(MINGW)
    add_custom_command(TARGET task_layer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:Qt${QT_VERSION_MAJOR}::Core> $<TARGET_FILE_DIR:task_layer>
    )
endif()

# DLL worker 宿主（目标形态：隔离加载功能 DLL）
add_executable(worker_host
    worker_host/main.cpp
)
target_include_directories(worker_host PRIVATE src)
target_link_libraries(worker_host PRIVATE Qt${QT_VERSION_MAJOR}::Core)

# worker_host 必须落在 task_layer.exe 的 <exe目录>/bin 下：
# task_layer 默认 {BIN} = <exe目录>/bin，模块注册表里的 workerHost/{BIN}/modules 都按此解析。
# 多配置生成器（VS）会把产物放进 <配置名>/ 子目录，因此逐配置指定输出目录。
if(CMAKE_CONFIGURATION_TYPES)
    foreach(_cfg IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${_cfg}" _cfg_upper)
        set_target_properties(worker_host PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${_cfg_upper} "${CMAKE_BINARY_DIR}/${_cfg}/bin")
    endforeach()
else()
    set_target_properties(worker_host PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# worker_host 在 bin/ 下独立运行，旁侧需要 Qt6Core.dll
add_custom_command(TARGET worker_host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:Qt${QT_VERSION_MAJOR}::Core> $<TARGET_FILE_DIR:worker_host>
)

if(MSVC)
    # 非致命警告提示：MSVC 默认 UTF-8 处理中文注释需 /utf-8
    target_compile_options(task_layer PRIVATE /utf-8)
    target_compile_options(worker_host PRIVATE /utf-8)
endif()

if(MINGW)
    # 本地 MinGW 构建：静态链接 GCC 运行时，产物只依赖 Qt6Core.dll
    foreach(_t task_layer worker_host)
        target_link_options(${_t} PRIVATE -static -static-libgcc -static-libstdc++)
    endforeach()
    # 中文路径/注释：明确按 UTF-8 处理源码
    foreach(_t task_layer worker_host)
        target_compile_options(${_t} PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
    endforeach()
endif()

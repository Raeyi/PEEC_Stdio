# build_local.ps1 — 本地构建 task_layer（免管理员：Qt6 MinGW + MinGW 工具链）
#
# 一次性准备（不需要管理员权限，也不会改动系统）：
#   python -m pip install aqtinstall
#   python -m aqt install-qt  windows desktop 6.8.3 win64_mingw --archives qtbase -O C:\Qt
#   :: MinGW 13.1（官方源慢时，可直接从 USTC 镜像下 7z 后用 tar 解压到 C:\Qt\Tools）
#   python -m aqt install-tool windows desktop tools_mingw1310 -O C:\Qt
#
# 用法：
#   .\build_local.ps1                      # 构建到 .\build（有 cmake 用 cmake，没有则直接 moc+g++）
#   .\build_local.ps1 -Clean               # 清空重建
#   .\build_local.ps1 -QtDir D:\Qt\6.8.3\mingw_64 -MingwBin D:\Qt\Tools\mingw1310_64\bin
[CmdletBinding()]
param(
    [string]$QtDir    = "C:\Qt\6.8.3\mingw_64",
    [string]$MingwBin = "C:\Qt\Tools\mingw1310_64\bin",
    [string]$BuildDir = "build",
    [ValidateSet("Release", "Debug")]
    [string]$Config   = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root  = $PSScriptRoot
$build = Join-Path $root $BuildDir

if ($Clean -and (Test-Path $build)) {
    Write-Host "==> 清理 $build" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $build
}

foreach ($p in @($QtDir, $MingwBin)) {
    if (-not (Test-Path $p)) {
        throw "路径不存在：$p`n请先按脚本头部注释安装 Qt6(MinGW) 与 MinGW 13.1"
    }
}

$env:PATH = "$MingwBin;$QtDir\bin;$env:PATH"
New-Item -ItemType Directory -Force -Path $build | Out-Null

function Copy-Runtime {
    # 运行时依赖：Qt6Core.dll + MinGW 运行库（产物本身用 -static 链接 GCC 运行时）
    Copy-Item "$QtDir\bin\Qt6Core.dll" $build -Force
    foreach ($dll in @("libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll")) {
        $src = Join-Path $MingwBin $dll
        if (Test-Path $src) { Copy-Item $src $build -Force }
    }
    # worker_host 放在 build\bin（对应 task_layer 默认的 {BIN}），旁侧同样需要 Qt6Core.dll
    $binDir = Join-Path $build "bin"
    New-Item -ItemType Directory -Force -Path $binDir | Out-Null
    Copy-Item "$QtDir\bin\Qt6Core.dll" $binDir -Force
    # 注意：用 config\* 拷贝内容。若直接拷目录且目标已存在，会嵌套成 build\config\config\。
    $cfgDir = Join-Path $build "config"
    New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
    Copy-Item -Force (Join-Path $root "config\*") $cfgDir
}

if (Get-Command cmake -ErrorAction SilentlyContinue) {
    Write-Host "==> CMake 构建 (Qt: $QtDir)" -ForegroundColor Cyan
    $gen = @("-G", "Ninja")
    if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) { $gen = @("-G", "MinGW Makefiles") }
    cmake -S $root -B $build @gen -DCMAKE_BUILD_TYPE=$Config -DCMAKE_PREFIX_PATH=$QtDir
    if ($LASTEXITCODE -ne 0) { throw "cmake 配置失败" }
    cmake --build $build
    if ($LASTEXITCODE -ne 0) { throw "编译失败" }
    Copy-Runtime
}
else {
    Write-Host "==> 未找到 cmake，使用 moc + g++ 直接构建" -ForegroundColor Yellow
    # 注意：全部使用**相对路径**调用 moc/g++。工程放在中文目录时，
    # PowerShell 5.1 会把绝对路径按控制台代码页传给子进程，moc/g++ 会因编码不一致找不到文件。
    Push-Location $root
    try {
        $moc = Join-Path $QtDir "bin\moc.exe"
        $inc = @("-Isrc", "-I$QtDir\include", "-I$QtDir\include\QtCore")
        $headers = @("src\App.h", "src\orchestrator\TaskOrchestrator.h",
                     "src\modules\ModuleRunner.h", "src\rpc\StdioTransport.h")
        New-Item -ItemType Directory -Force -Path "build\moc" | Out-Null
        $mocSrc = @()
        foreach ($h in $headers) {
            $out = "build\moc\moc_" + [IO.Path]::GetFileNameWithoutExtension($h) + ".cpp"
            & $moc "-Isrc" $h -o $out
            if ($LASTEXITCODE -ne 0) { throw "moc 失败: $h" }
            $mocSrc += $out
        }
        $src = @(
            "src\main.cpp", "src\App.cpp",
            "src\config\ModuleRegistry.cpp", "src\config\PipelineLoader.cpp",
            "src\io\InputValidator.cpp", "src\io\ResultStore.cpp",
            "src\modules\ModuleRunner.cpp", "src\orchestrator\TaskOrchestrator.cpp",
            "src\rpc\StdioTransport.cpp"
        )

        $opt = if ($Config -eq "Debug") { "-O0 -g" } else { "-O2" }
        $common = @("-std=c++17") + ($opt -split ' ') + @("-static") + $inc

        Write-Host "==> 编译 task_layer.exe" -ForegroundColor Cyan
        & g++ @common @src @mocSrc "-L$QtDir\lib" -lQt6Core -o "build\task_layer.exe"
        if ($LASTEXITCODE -ne 0) { throw "编译 task_layer 失败" }

        Write-Host "==> 编译 worker_host.exe" -ForegroundColor Cyan
        New-Item -ItemType Directory -Force -Path "build\bin" | Out-Null
        & g++ @common "worker_host\main.cpp" "-L$QtDir\lib" -lQt6Core -o "build\bin\worker_host.exe"
        if ($LASTEXITCODE -ne 0) { throw "编译 worker_host 失败" }
    }
    finally {
        Pop-Location
    }
    Copy-Runtime
}

Write-Host "==> 产物：" -ForegroundColor Green
Get-ChildItem $build -Filter *.exe | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize

Write-Host "自检：" -ForegroundColor Yellow
Write-Host "  .\build\task_layer.exe modules"
Write-Host "  .\build\task_layer.exe pipelines"
Write-Host "  python .\tools\make_mini_case.py --out ..\demo\mini_strip"
Write-Host "  python .\tools\rpc_smoke.py --exe .\build\task_layer.exe --data-root ..\demo\mini_strip\DATA"

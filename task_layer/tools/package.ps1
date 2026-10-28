# Produce a deployable directory for the UI (default: dist\).
#
# Layout follows compact_em: a flat directory with the exe, required runtime
# files and docs, ready to be copied away.
# Difference: compact_em exes are fully static (KERNEL32.dll / bcrypt.dll only),
# while the task layer links QtCore, so Qt6Core.dll and the MinGW runtime must
# sit next to task_layer.exe.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-Out dist]
# NOTE: keep this file ASCII-only. PowerShell 5.1 decodes BOM-less UTF-8 using
# the local code page, which corrupts non-ASCII comments and breaks parsing.

param([string]$Out = "dist")

$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $PSScriptRoot          # parent of tools\ = task_layer
$build = Join-Path $root "build"
$dist  = Join-Path $root $Out

if (-not (Test-Path (Join-Path $build "task_layer.exe"))) {
    throw "not built yet: run .\build_local.ps1 first"
}

# rebuild from scratch to avoid stale artifacts
if (Test-Path $dist) {
    Remove-Item -Recurse -Force $dist
}
New-Item -ItemType Directory -Force -Path $dist | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $dist "config") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $dist "bin") | Out-Null

# 1) main program + runtime dependencies (same dir as the exe: Windows looks here first)
$runtime = @("task_layer.exe", "Qt6Core.dll", "libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll")
foreach ($f in $runtime) {
    $src = Join-Path $build $f
    if (-not (Test-Path $src)) {
        throw "missing in build: $f"
    }
    Copy-Item $src $dist -Force
}

# verify after copy: Copy-Item can fail silently when the exe is locked by a
# running instance, leaving the delivery dir without a main program
foreach ($f in $runtime) {
    if (-not (Test-Path (Join-Path $dist $f))) {
        throw "copy failed: $f (target may be locked by a running instance)"
    }
}

# 2) config: module registry + pipeline definitions
Copy-Item -Force (Join-Path $root "config\*") (Join-Path $dist "config")

# 3) worker_host: host process for DLL-shaped modules
$wh = Join-Path $build "bin\worker_host.exe"
if (Test-Path $wh) {
    Copy-Item $wh (Join-Path $dist "bin") -Force
    Copy-Item (Join-Path $build "bin\Qt6Core.dll") (Join-Path $dist "bin") -Force -ErrorAction SilentlyContinue
}

# 4) docs
foreach ($f in @("UI_API.md", "README.md")) {
    if (Test-Path (Join-Path $root $f)) {
        Copy-Item (Join-Path $root $f) $dist
    }
}

Write-Host ""
Write-Host "package -> $dist"
Write-Host ""
Get-ChildItem $dist -Recurse -File | Sort-Object FullName | ForEach-Object {
    "{0,-42} {1,10:N0} B" -f $_.FullName.Substring($dist.Length + 1), $_.Length
}

# 构建内置独立版 esptool.exe，输出到 host/HidBridge.Host/EmbeddedAssets/esptool.exe。
# 之后 dotnet publish 会把它嵌入 HidBridge.Host.exe，两个刷写入口（远端 API 与设置页本地刷写）
# 都优先调用它，目标机无需安装 Python / ESP-IDF 环境。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts/build-embedded-esptool.ps1 [-Force]
# 可选参数：
#   -PythonExe <path>   指定用于创建构建 venv 的 Python（默认优先项目 .esp-idf 自带 Python）
#   -Force              即使目标已存在也重新构建

param(
    [string]$PythonExe = "",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $repoRoot "host/HidBridge.Host/EmbeddedAssets/esptool.exe"
$buildRoot = Join-Path $repoRoot ".build-tools/esptool-build"
$venv = Join-Path $buildRoot "venv"
$venvPython = Join-Path $venv "Scripts/python.exe"

if (!$Force -and (Test-Path $dest)) {
    Write-Host "已存在内置 esptool：$dest（需要重建请加 -Force）"
    exit 0
}

if (!$PythonExe) {
    $projectPython = Join-Path $repoRoot ".esp-idf/environment/idf-tools/python/python.exe"
    if (Test-Path $projectPython) {
        $PythonExe = $projectPython
    } elseif (Get-Command python -ErrorAction SilentlyContinue) {
        $PythonExe = "python"
    } else {
        throw "找不到 Python，请用 -PythonExe 指定（例如 -PythonExe C:/Python312/python.exe）"
    }
}
Write-Host "使用 Python：$PythonExe"

if (!(Test-Path $venvPython)) {
    Write-Host "创建构建虚拟环境：$venv"
    & $PythonExe -m venv $venv
    if ($LASTEXITCODE -ne 0) { throw "创建 venv 失败" }
}

Write-Host "安装 esptool 与 PyInstaller（可能从镜像下载，需要网络）"
& $venvPython -m pip install --quiet --upgrade esptool pyinstaller
if ($LASTEXITCODE -ne 0) { throw "pip 安装失败" }

$wrapper = Join-Path $buildRoot "run_esptool.py"
@"
import esptool

if __name__ == "__main__":
    esptool._main()
"@ | Set-Content -Encoding UTF8 -Path $wrapper

$distPath = Join-Path $buildRoot "dist"
$workPath = Join-Path $buildRoot "work"
Write-Host "使用 PyInstaller 构建单文件 esptool.exe"
& $venvPython -m PyInstaller --noconfirm --clean --onefile --name esptool --collect-all esptool --collect-all rich_click --distpath $distPath --workpath $workPath --specpath $buildRoot $wrapper
if ($LASTEXITCODE -ne 0) { throw "PyInstaller 构建失败" }

New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
Copy-Item (Join-Path $distPath "esptool.exe") $dest -Force
$size = (Get-Item $dest).Length
Write-Host "完成：$dest（$size 字节）"

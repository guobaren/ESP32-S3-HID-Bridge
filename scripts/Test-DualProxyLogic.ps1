# 运行 dual_proxy 纯逻辑回归（clang -Wall -Wextra -Werror），不触碰硬件。
#
# 用法：
#   pwsh -File .\scripts\Test-DualProxyLogic.ps1
#
# 依赖：clang 在 PATH 中；本仓库为便携式 ESP-IDF（.esp-idf\environment）。
# 退出码：0 表示编译与运行都通过，并输出 dual_proxy_logic_test: PASS。

[CmdletBinding()]
param(
    [string]$CheckoutRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'

$project = Join-Path $CheckoutRoot 'firmware\dual_proxy'
$sources = @(
    'main\bridge_protocol.c',
    'main\dual_input_aggregator.c',
    'main\hid_device_profile.c',
    'main\hid_clone_descriptor.c',
    'main\hid_report_layout.c',
    'main\dual_status_led_logic.c',
    'main\usb_cdc_control_logic.c',
    'main\makcu_ascii_logic.c',
    'main\makcu_v4_logic.c',
    'main\uart0_protocol_router.c',
    'main\m_udp_smoothing.c'
)
# 共享的平滑器仍在旧单板工程里，逻辑测试复用同一份源码。
$shared_sources = @('firmware\main\mouse_motion_smoother.c')

$clang = (Get-Command clang -ErrorAction SilentlyContinue).Source
if (-not $clang) {
    throw 'clang 不在 PATH 中，无法编译纯逻辑回归。'
}

$sdkconfigDir = Join-Path $project 'build\config'
if (-not (Test-Path -LiteralPath (Join-Path $sdkconfigDir 'sdkconfig.h'))) {
    throw "缺少 $sdkconfigDir\sdkconfig.h；请先在本工程执行一次 idf.py build。"
}

$espCommon = Join-Path $CheckoutRoot '.esp-idf\environment\v6.0.2\esp-idf\components\esp_common\include'
if (-not (Test-Path -LiteralPath $espCommon)) {
    throw "缺少 ESP-IDF esp_common 头文件目录：$espCommon"
}

$output = Join-Path $project 'tests\dual_proxy_logic_test.exe'
$arguments = @(
    '-std=c11', '-Wall', '-Wextra', '-Werror',
    '-I', (Join-Path $project 'main'),
    '-I', $sdkconfigDir,
    '-I', (Join-Path $CheckoutRoot 'firmware\main'),
    '-I', $espCommon,
    '-o', $output,
    (Join-Path $project 'tests\dual_proxy_logic_test.c')
)
foreach ($source in $sources) {
    $arguments += (Join-Path $project $source)
}
foreach ($source in $shared_sources) {
    $arguments += (Join-Path $CheckoutRoot $source)
}

& $clang @arguments
if ($LASTEXITCODE -ne 0) {
    throw "逻辑测试编译失败（clang 退出码 $LASTEXITCODE）。"
}

& $output
if ($LASTEXITCODE -ne 0) {
    throw "逻辑测试运行失败（退出码 $LASTEXITCODE）。"
}

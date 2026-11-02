# 从生产文件提取角色清理函数，以离线 stub 验证失败短路和清理顺序；不访问板卡。
[CmdletBinding()]
param([string]$CheckoutRoot = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
$source = Get-Content -LiteralPath (Join-Path $CheckoutRoot 'firmware/main/dual_proxy_app.c') -Raw
$start = $source.IndexOf('static esp_err_t restart_role_probe(void)')
$end = $source.IndexOf('void app_main(void)', $start)
if ($start -lt 0 -or $end -lt 0) { throw '找不到生产角色清理函数，停止 fixture 验证。' }
$body = $source.Substring($start, $end - $start)
$template = Get-Content -LiteralPath (Join-Path $CheckoutRoot 'tests/UsbRoleReprobeNative/role_probe_harness.c') -Raw
$artifact = Join-Path $CheckoutRoot 'artifacts/tests/usb-role-reprobe-20261016/native'
New-Item -ItemType Directory -Force -Path $artifact | Out-Null
$fixture = Join-Path $artifact 'role_probe_harness.c'
[IO.File]::WriteAllText($fixture, $template.Replace('/* PRODUCTION_RESTART_ROLE_PROBE */', $body))
$output = Join-Path $artifact 'role_probe_harness.exe'
$espCommon = Join-Path $CheckoutRoot '.esp-idf/environment/v6.0.2/esp-idf/components/esp_common/include'
& clang -std=c11 -Wall -Wextra -Werror -I (Join-Path $CheckoutRoot 'firmware/main') -I (Join-Path $CheckoutRoot 'firmware/build/config') -I $espCommon $fixture -o $output
if ($LASTEXITCODE -ne 0) { throw '角色清理 fixture 编译失败。' }
& $output
if ($LASTEXITCODE -ne 0) { throw '角色清理 fixture 断言失败。' }

$linkSource = Get-Content -LiteralPath (Join-Path $CheckoutRoot 'firmware/main/uart1_link.c') -Raw
$linkStart = $linkSource.IndexOf('esp_err_t dual_uart1_begin_role_probe(void)')
$linkEnd = $linkSource.IndexOf('esp_err_t dual_uart1_lock_role(uint8_t role)', $linkStart)
if ($linkStart -lt 0 -or $linkEnd -lt 0) { throw '找不到生产 UART 会话重探测函数。' }
$linkBody = $linkSource.Substring($linkStart, $linkEnd - $linkStart)
$linkTemplate = Get-Content -LiteralPath (Join-Path $CheckoutRoot 'tests/UsbRoleReprobeNative/link_role_probe_harness.c') -Raw
$linkFixture = Join-Path $artifact 'link_role_probe_harness.c'
[IO.File]::WriteAllText($linkFixture, $linkTemplate.Replace('/* PRODUCTION_BEGIN_ROLE_PROBE */', $linkBody))
$linkOutput = Join-Path $artifact 'link_role_probe_harness.exe'
& clang -std=c11 -Wall -Wextra -Werror -I (Join-Path $CheckoutRoot 'firmware/main') -I (Join-Path $CheckoutRoot 'firmware/build/config') -I $espCommon $linkFixture -o $linkOutput
if ($LASTEXITCODE -ne 0) { throw 'UART 重探测 fixture 编译失败。' }
& $linkOutput
if ($LASTEXITCODE -ne 0) { throw 'UART 重探测 fixture 断言失败。' }

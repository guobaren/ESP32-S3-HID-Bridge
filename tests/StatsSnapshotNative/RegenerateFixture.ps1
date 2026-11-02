$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$tempExe = Join-Path $env:TEMP ("stats-snapshot-harness-{0}.exe" -f [Guid]::NewGuid().ToString('N'))
$diagTestExe = Join-Path $env:TEMP ("usb-host-diag-logic-{0}.exe" -f [Guid]::NewGuid().ToString('N'))
$fixture = Join-Path $root 'tests\fixtures\stats_snapshot_c_frames.txt'
$include = Join-Path $PSScriptRoot 'include'
$main = Join-Path $root 'firmware\main'
$sources = @(
    (Join-Path $PSScriptRoot 'stats_snapshot_harness.c'),
    (Join-Path $main 'stats_snapshot_codec.c'),
    (Join-Path $main 'bridge_protocol.c')
)
try {
    & clang -std=c11 -Wall -Wextra -Werror "-I$include" "-I$main" @sources -o $tempExe
    if ($LASTEXITCODE -ne 0) {
        throw "clang 编译统计快照 C harness 失败：exit=$LASTEXITCODE"
    }
    & $tempExe | Set-Content -Encoding ascii $fixture
    if ($LASTEXITCODE -ne 0) {
        throw "运行统计快照 C harness 失败：exit=$LASTEXITCODE"
    }
    $lineCount = (Get-Content $fixture | Measure-Object -Line).Lines
    if ($lineCount -ne 34) {
        throw "统计快照 fixture 应为 34 页，实际 $lineCount 页。"
    }
    $diagTest = Join-Path $PSScriptRoot 'usb_host_diagnostics_logic_test.c'
    & clang -std=c11 -Wall -Wextra -Werror "-I$include" "-I$main" $diagTest -o $diagTestExe
    if ($LASTEXITCODE -ne 0) {
        throw "clang 编译 USB Host 诊断状态逻辑测试失败：exit=$LASTEXITCODE"
    }
    & $diagTestExe
    if ($LASTEXITCODE -ne 0) {
        throw "USB Host 诊断状态逻辑测试失败：exit=$LASTEXITCODE"
    }
    Write-Output "dual_proxy C 统计编码 fixture 已生成：$fixture（$lineCount 页；M 扩展为 83 个计数器，跨越 64 项分页边界）"
    Write-Output "USB Host 诊断原子状态逻辑测试：PASS；无事件时间有效性、连接 wake 独立时间、失败保留及成功不清 lastFailure。"
}
finally {
    Remove-Item -LiteralPath $tempExe -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $diagTestExe -Force -ErrorAction SilentlyContinue
}

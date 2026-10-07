$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$tempExe = Join-Path $env:TEMP ("stats-snapshot-harness-{0}.exe" -f [Guid]::NewGuid().ToString('N'))
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
    if ($lineCount -ne 28) {
        throw "统计快照 fixture 应为 28 页，实际 $lineCount 页。"
    }
    Write-Output "dual_proxy C 统计编码 fixture 已生成：$fixture（$lineCount 页）"
}
finally {
    Remove-Item -LiteralPath $tempExe -Force -ErrorAction SilentlyContinue
}

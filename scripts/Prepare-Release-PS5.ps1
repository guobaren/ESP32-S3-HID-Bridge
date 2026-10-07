[CmdletBinding()]
param(
    [switch]$SkipHostBuild,
    [switch]$BuildFirmware
)

$ErrorActionPreference = 'Stop'
$prepareScript = Join-Path $PSScriptRoot 'Prepare-Release.ps1'
if (-not (Test-Path -LiteralPath $prepareScript -PathType Leaf)) {
    throw "找不到发布件生成脚本：$prepareScript"
}

& $prepareScript @PSBoundParameters
if (-not $?) {
    throw 'Prepare-Release.ps1 执行失败。'
}

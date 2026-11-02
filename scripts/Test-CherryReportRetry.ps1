[CmdletBinding()]
param(
    [string]$CheckoutRoot,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
if (-not $CheckoutRoot) { $CheckoutRoot = Split-Path -Parent $PSScriptRoot }
$root = (Resolve-Path $CheckoutRoot).Path
$include = Join-Path $root 'tests\CherryReportRetryNative\include'
$test = Join-Path $root 'tests\CherryReportRetryNative\cherry_report_retry_test.c'
$overrides = Join-Path $root 'firmware\cherryusb_overrides'
$clang = (Get-Command clang -ErrorAction SilentlyContinue).Source
if (-not $clang) { throw 'clang is required for CherryUSB report-descriptor retry tests.' }

if (-not $OutputPath) {
    $OutputPath = Join-Path $root 'artifacts\tests\cherry-report-retry-20261014\cherry-report-retry-test.exe'
}
$outputDirectory = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

& $clang -std=c11 -Wall -Wextra -Werror "-I$include" "-I$overrides" $test `
    (Join-Path $overrides 'cherryusb_hid_control.c') `
    (Join-Path $overrides 'cherryusb_report_retry.c') -o $OutputPath
if ($LASTEXITCODE -ne 0) { throw "CherryUSB report retry compile failed: exit=$LASTEXITCODE" }
& $OutputPath
if ($LASTEXITCODE -ne 0) { throw "CherryUSB report retry run failed: exit=$LASTEXITCODE" }

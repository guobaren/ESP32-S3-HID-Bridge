# Compile the real TinyUSB overlay with controlled ESP-IDF API stubs; no hardware access.
[CmdletBinding()]
param(
    [string]$CheckoutRoot,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
if (-not $CheckoutRoot) { $CheckoutRoot = Split-Path -Parent $PSScriptRoot }
$root = (Resolve-Path $CheckoutRoot).Path
$include = Join-Path $root 'tests\TinyUsbLifecycleNative\include'
$test = Join-Path $root 'tests\TinyUsbLifecycleNative\tinyusb_lifecycle_test.c'
$overlay = Join-Path $root 'firmware\tinyusb_overrides\tinyusb.c'
$clang = (Get-Command clang -ErrorAction SilentlyContinue).Source
if (-not $clang) { throw 'clang is required for TinyUSB lifecycle fault injection.' }

if (-not $OutputPath) {
    $OutputPath = Join-Path $root 'artifacts\tests\cherryusb-lifecycle-20261014\tinyusb-lifecycle-test.exe'
}
$outputDirectory = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

& $clang -std=c11 -Wall -Wextra -Werror "-I$include" $test $overlay -o $OutputPath
if ($LASTEXITCODE -ne 0) { throw "TinyUSB overlay fault-injection compile failed: exit=$LASTEXITCODE" }
& $OutputPath
if ($LASTEXITCODE -ne 0) { throw "TinyUSB overlay fault-injection run failed: exit=$LASTEXITCODE" }

param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$EvidenceDirectory = ""
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($EvidenceDirectory)) {
    $EvidenceDirectory = Join-Path $RepositoryRoot "artifacts/tests/usb-role-reprobe-fault-20261017/repair/native"
}
New-Item -ItemType Directory -Force -Path $EvidenceDirectory | Out-Null

$sourcePath = Join-Path $RepositoryRoot "firmware/cherryusb_overrides/usb_hc_dwc2.c"
$harnessPath = Join-Path $RepositoryRoot "tests/Dwc2DeinitNative/dwc2_deinit_harness.c"
$source = Get-Content -LiteralPath $sourcePath -Raw
$harness = Get-Content -LiteralPath $harnessPath -Raw
$beginMarker = "/* DWC2_DEINIT_HALT_HELPER_BEGIN */"
$endMarker = "/* DWC2_DEINIT_HALT_HELPER_END */"
$beginIndex = $source.IndexOf($beginMarker, [StringComparison]::Ordinal)
$endIndex = $source.IndexOf($endMarker, [StringComparison]::Ordinal)
if ($beginIndex -lt 0 -or $endIndex -le $beginIndex) {
    throw "Production helper markers are missing or out of order."
}
$helperStart = $beginIndex + $beginMarker.Length
$productionHelper = $source.Substring($helperStart, $endIndex - $helperStart).Trim()
$placeholder = "/* __PRODUCTION_HELPER__ */"
if (-not $harness.Contains($placeholder)) {
    throw "Native fixture production-helper placeholder is missing."
}
$generatedPath = Join-Path $EvidenceDirectory "dwc2_deinit_harness.generated.c"
[System.IO.File]::WriteAllText($generatedPath, $harness.Replace($placeholder, $productionHelper), [System.Text.UTF8Encoding]::new($false))

$clang = Get-Command clang -ErrorAction SilentlyContinue
if ($null -eq $clang) {
    throw "clang is required for the native DWC2 helper fixture."
}
$executablePath = Join-Path $EvidenceDirectory "dwc2_deinit_harness.exe"
$compileLog = Join-Path $EvidenceDirectory "compile.log"
& $clang.Source -std=c11 -Wall -Wextra -Werror $generatedPath -o $executablePath 2>&1 | Tee-Object -FilePath $compileLog
if ($LASTEXITCODE -ne 0) {
    throw "Native fixture compile failed with exit code $LASTEXITCODE."
}

# Check the production call is in the register-ready branch and returns before semaphore/PHY release.
$deinitStart = $source.IndexOf("int usb_hc_deinit(struct usbh_bus *bus)", [StringComparison]::Ordinal)
$nextFunction = $source.IndexOf("uint16_t usbh_get_frame_number", $deinitStart, [StringComparison]::Ordinal)
if ($deinitStart -lt 0 -or $nextFunction -le $deinitStart) {
    throw "Could not isolate usb_hc_deinit for ownership-order assertions."
}
$deinit = $source.Substring($deinitStart, $nextFunction - $deinitStart)
$haltCall = $deinit.IndexOf("const int halt_result = dwc2_deinit_halt_channels", [StringComparison]::Ordinal)
$haltReturn = $deinit.IndexOf("if (halt_result != 0) {", $haltCall, [StringComparison]::Ordinal)
$haltReturnBody = $deinit.IndexOf("return halt_result;", $haltReturn, [StringComparison]::Ordinal)
$semaphoreRelease = $deinit.IndexOf("dwc2_release_channel_semaphores(bus_id);", [StringComparison]::Ordinal)
$phyRelease = $deinit.IndexOf("dual_cherryusb_host_low_level_deinit(bus);", [StringComparison]::Ordinal)
$readyBranch = $deinit.LastIndexOf("if (s_dwc2_registers_ready[bus_id])", $haltCall, [StringComparison]::Ordinal)
$readyOpenBrace = if ($readyBranch -ge 0) { $deinit.IndexOf("{", $readyBranch) } else { -1 }
$readyCloseBrace = -1
if ($readyOpenBrace -ge 0) {
    $braceDepth = 0
    for ($index = $readyOpenBrace; $index -lt $deinit.Length; $index++) {
        if ($deinit[$index] -eq '{') { $braceDepth++ }
        elseif ($deinit[$index] -eq '}') {
            $braceDepth--
            if ($braceDepth -eq 0) {
                $readyCloseBrace = $index
                break
            }
        }
    }
}
if ($haltCall -lt 0 -or $haltReturn -le $haltCall -or $haltReturnBody -le $haltReturn -or
    $semaphoreRelease -le $haltReturnBody -or $phyRelease -le $haltReturnBody -or
    $readyOpenBrace -lt 0 -or $readyCloseBrace -le $readyOpenBrace -or
    $haltCall -lt $readyOpenBrace -or $haltCall -gt $readyCloseBrace) {
    throw "Source ownership-order assertion failed: halt helper must run in the ready branch and return before semaphore/PHY release."
}
Write-Output "PASS source ownership order: helper failure returns before semaphore and PHY release"

$runLog = Join-Path $EvidenceDirectory "assertions.log"
& $executablePath 2>&1 | Tee-Object -FilePath $runLog
if ($LASTEXITCODE -ne 0) {
    throw "Native fixture assertions failed with exit code $LASTEXITCODE."
}
Write-Output "PASS native DWC2 deinit fixture (production helper extracted from $sourcePath)"

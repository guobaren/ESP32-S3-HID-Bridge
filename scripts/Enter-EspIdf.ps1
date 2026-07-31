$projectRoot = Split-Path -Parent $PSScriptRoot
$idfPath = Join-Path $projectRoot ".esp-idf\environment\v6.0.2\esp-idf"
$idfToolsPath = Join-Path $projectRoot ".esp-idf\environment\idf-tools"
$portablePython = Join-Path $idfToolsPath "python"
$exportScript = Join-Path $idfPath "export.ps1"

if (-not (Test-Path -LiteralPath $exportScript)) {
    throw "Project-local ESP-IDF export script was not found."
}

$env:IDF_TOOLS_PATH = $idfToolsPath
$env:Path = "$portablePython;$portablePython\Scripts;$env:Path"
. $exportScript

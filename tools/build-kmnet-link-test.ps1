param([string]$Python = "python")
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path $PSScriptRoot -Parent
Push-Location $repoRoot
try {
    & $Python -m PyInstaller --noconfirm --clean --onefile --windowed --name KmnetLinkTest `
        --distpath dist/kmnet-link-test --workpath artifacts/kmnet-link-test/build `
        --specpath artifacts/kmnet-link-test tools/kmnet_link_test.py
    if ($LASTEXITCODE -ne 0) { throw "KMNET 测试工具打包失败" }
    Copy-Item -LiteralPath tools/kmnet-link-test-使用说明.md -Destination dist/kmnet-link-test/使用说明.md
    Compress-Archive -Path dist/kmnet-link-test/KmnetLinkTest.exe,dist/kmnet-link-test/使用说明.md `
        -DestinationPath dist/KmnetLinkTest-win-x64.zip -Force
    Get-FileHash -LiteralPath dist/kmnet-link-test/KmnetLinkTest.exe,dist/KmnetLinkTest-win-x64.zip -Algorithm SHA256
} finally {
    Pop-Location
}

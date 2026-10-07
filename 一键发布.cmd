@echo off
setlocal EnableExtensions
cd /d "%~dp0"
rem Capture the script directory BEFORE any shift: after shift, %~dp0
rem expansion becomes unreliable (cmd quirk), so always use %SCRIPT_DIR%.
set "SCRIPT_DIR=%~dp0"

rem ============================================================
rem  ESP32-S3 HID Bridge one-click publish
rem  Calls scripts\Publish-GitHubRelease.ps1 / Prepare-Release.ps1
rem  Default: build latest Host Release + latest DUAL-BOARD firmware
rem  release/ contents, package ZIP into dist/, print checksum summary.
rem  (Chinese messages are printed by the called PowerShell scripts.)
rem
rem  Usage: this script [options]
rem    /SkipHostBuild   skip Host build (reuse root HidBridge.Host.exe)
rem    /NoFirmware      skip dual-board firmware build (reuse firmware\build output)
rem    /Github          also create a GitHub Release (requires gh login,
rem                     clean tracked workspace, tag not existing remote)
rem    /Draft           with /Github: create a draft Release
rem    /Prerelease      with /Github: mark as prerelease
rem    Use /? to show this help
rem ============================================================

set "SKIP_HOST_BUILD="
set "BUILD_FIRMWARE=1"
set "GITHUB="
set "DRAFT="
set "PRERELEASE="

:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="/SkipHostBuild" (
    set "SKIP_HOST_BUILD=1"
) else if /i "%~1"=="/NoFirmware" (
    set "BUILD_FIRMWARE="
) else if /i "%~1"=="/Github" (
    set "GITHUB=1"
) else if /i "%~1"=="/Draft" (
    set "DRAFT=1"
) else if /i "%~1"=="/Prerelease" (
    set "PRERELEASE=1"
) else if /i "%~1"=="/?" (
    goto usage
) else (
    echo [ERROR] Unknown option: %~1
    goto usage
)
shift
goto parse_args
:args_done

rem ---- extract version from csproj (powershell writes a temp file; safe from cmd metacharacters) ----
rem Force the Windows PowerShell 5.1 module path: when launched from a
rem PowerShell 7 session, PSModulePath may contain PS7 module folders and
rem PS 5.1 then fails to load its own cmdlets (e.g. Get-FileHash).
set "PSModulePath=%SystemRoot%\System32\WindowsPowerShell\v1.0\Modules;%ProgramFiles%\WindowsPowerShell\Modules;%USERPROFILE%\Documents\WindowsPowerShell\Modules"
powershell -NoProfile -Command "$m=[regex]::Match((Get-Content -Raw 'host\HidBridge.Host\HidBridge.Host.csproj'),'<Version>([^<]+)</Version>');Set-Content -LiteralPath 'build-version.tmp' -Value $m.Groups[1].Value -NoNewline"
if errorlevel 1 (
    echo [ERROR] Failed to extract Version from host\HidBridge.Host\HidBridge.Host.csproj
    del /q build-version.tmp >nul 2>nul
    exit /b 1
)
set /p VERSION=<build-version.tmp
del /q build-version.tmp >nul 2>nul
if not defined VERSION (
    echo [ERROR] Cannot extract Version from host\HidBridge.Host\HidBridge.Host.csproj
    exit /b 1
)
set "TAG=v%VERSION%"

echo ============================================================
echo   ESP32-S3 HID Bridge one-click publish  (version %TAG%)
echo ============================================================
echo.
if defined SKIP_HOST_BUILD echo   [INFO] /SkipHostBuild: reuse root EXE, skip Host build.
if not defined BUILD_FIRMWARE echo   [INFO] /NoFirmware: reuse dual-board firmware\build output.
echo.

rem ---- assemble Prepare/Package arguments ----
set "EXTRA_ARGS="
if defined SKIP_HOST_BUILD set "EXTRA_ARGS=%EXTRA_ARGS% -SkipHostBuild"
if defined BUILD_FIRMWARE set "EXTRA_ARGS=%EXTRA_ARGS% -BuildFirmware"

echo [1/2] Building latest artifacts, assembling release/ and packaging ZIP into dist/...
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%scripts\Publish-GitHubRelease.ps1" -Tag %TAG% -PackageOnly %EXTRA_ARGS%
if errorlevel 1 (
    echo.
    echo [ERROR] Release assembly or packaging failed. See messages above.
    exit /b 1
)

echo.
echo [2/2] Summary...

rem ---- summary: EXE/ZIP sizes and hashes (certutil is pure cmd, no quoting issues) ----
for %%F in ("release\HidBridge.Host.exe") do set "EXE_SIZE=%%~zF"
for %%F in ("dist\ESP32-S3-HID-Bridge-%TAG%.zip") do set "ZIP_SIZE=%%~zF"
for /f "skip=1 delims=" %%H in ('certutil -hashfile "release\HidBridge.Host.exe" SHA256') do if not defined EXE_HASH set "EXE_HASH=%%H"
for /f "skip=1 delims=" %%H in ('certutil -hashfile "dist\ESP32-S3-HID-Bridge-%TAG%.zip" SHA256') do if not defined ZIP_HASH set "ZIP_HASH=%%H"

echo.
echo   Release dir : release\  (manifest: release\SHA256SUMS.txt)
echo   ZIP         : dist\ESP32-S3-HID-Bridge-%TAG%.zip  (%ZIP_SIZE% bytes)
echo   EXE size    : %EXE_SIZE% bytes
echo   EXE SHA-256 : %EXE_HASH%
echo   ZIP SHA-256 : %ZIP_HASH%

rem ---- optional: GitHub extras (expanded line-by-line, safe without delayed expansion) ----
set "GH_EXTRA="
if defined DRAFT set "GH_EXTRA=%GH_EXTRA% -Draft"
if defined PRERELEASE set "GH_EXTRA=%GH_EXTRA% -Prerelease"

rem ---- optional: create GitHub Release ----
if defined GITHUB (
    echo.
    echo [GitHub] Creating Release %TAG%...
    powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%scripts\Publish-GitHubRelease.ps1" -Tag %TAG% -SkipHostBuild -Title "ESP32-S3 HID Bridge %TAG%" %GH_EXTRA%
    if errorlevel 1 (
        echo.
        echo [ERROR] GitHub Release creation failed. See messages above.
        exit /b 1
    )
    echo [GitHub] Release created: %TAG%
)

echo.
echo ===== Publish complete =====
exit /b 0

:usage
echo Usage: one-click-publish.cmd [options]
echo.
echo   /SkipHostBuild   skip Host build (reuse root HidBridge.Host.exe)
echo   /NoFirmware      skip dual-board firmware build (reuse firmware\build output)
echo   /Github          also create a GitHub Release (requires gh login,
echo                    clean tracked workspace, tag not existing remote)
echo   /Draft           with /Github: create a draft Release
echo   /Prerelease      with /Github: mark as prerelease
echo   Use /? to show this help
echo.
echo Examples:
echo   one-click-publish.cmd                  default: full build + latest release/ + ZIP
echo   one-click-publish.cmd /SkipHostBuild   reuse existing EXE, build dual-board firmware only
echo   one-click-publish.cmd /Github /Draft   package then create GitHub draft Release
exit /b 1

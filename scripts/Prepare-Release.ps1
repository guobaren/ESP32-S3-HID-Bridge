[CmdletBinding()]
param(
    [switch]$SkipHostBuild,
    [switch]$BuildFirmware
)

$ErrorActionPreference = 'Stop'
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$releaseDirectory = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'release'))
$expectedReleaseDirectory = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'release'))
$stagingDirectory = $null

function Copy-RequiredFile {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$RelativeDestination
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "发布输入文件不存在：$Source"
    }

    $destination = Join-Path $stagingDirectory $RelativeDestination
    $destinationParent = Split-Path -Parent $destination
    New-Item -ItemType Directory -Path $destinationParent -Force | Out-Null
    Copy-Item -LiteralPath $Source -Destination $destination -Force
}

function Copy-RequiredDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$RelativeDestination
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
        throw "发布输入目录不存在：$Source"
    }

    $destination = Join-Path $stagingDirectory $RelativeDestination
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Get-ChildItem -LiteralPath $Source -Force | Copy-Item -Destination $destination -Recurse -Force
}

try {
    if ($releaseDirectory -ne $expectedReleaseDirectory) {
        throw "发布输出目录解析异常：$releaseDirectory"
    }

    if (-not $SkipHostBuild) {
        Write-Host '构建 Host Release...' -ForegroundColor Cyan
        & dotnet build (Join-Path $repoRoot 'host\HidBridge.Host\HidBridge.Host.csproj') -c Release
        if ($LASTEXITCODE -ne 0) {
            throw "Host Release 构建失败，退出码：$LASTEXITCODE"
        }
    }

    if ($BuildFirmware) {
        Write-Host '构建 ESP32-S3 双板固件（firmware/，工程名 dual_s3_hid_proxy）...' -ForegroundColor Cyan
        Push-Location (Join-Path $repoRoot 'firmware')
        try {
            . ..\scripts\Enter-EspIdf.ps1
            # set-target 会重置已有 sdkconfig（分区表、TinyUSB 缓冲等），只在尚未配置过时执行一次。
            if (-not (Test-Path -LiteralPath (Join-Path $repoRoot 'firmware\sdkconfig'))) {
                idf.py set-target esp32s3
                if ($LASTEXITCODE -ne 0) {
                    throw "idf.py set-target 失败，退出码：$LASTEXITCODE"
                }
            }
            else {
                Write-Host '已存在 firmware\sdkconfig，跳过 idf.py set-target 以保留既有配置。' -ForegroundColor Yellow
            }
            idf.py build
            if ($LASTEXITCODE -ne 0) {
                throw "idf.py build 失败，退出码：$LASTEXITCODE"
            }
        }
        finally {
            Pop-Location
        }
    }

    $stagingDirectory = Join-Path $repoRoot ('.build-tools\release-staging-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stagingDirectory -Force | Out-Null

    $hostExe = Join-Path $repoRoot 'HidBridge.Host.exe'
    Copy-RequiredFile $hostExe 'HidBridge.Host.exe'
    Copy-RequiredFile (Join-Path $repoRoot 'host\HidBridge.Host\bridge.json') 'bridge.json'
    Copy-RequiredFile (Join-Path $repoRoot 'LICENSE') 'LICENSE'

    $profilesDestination = Join-Path $stagingDirectory 'profiles'
    New-Item -ItemType Directory -Path $profilesDestination -Force | Out-Null
    Copy-RequiredFile (Join-Path $repoRoot 'profiles.example\README.md') 'profiles\README.md'
    Copy-RequiredDirectory (Join-Path $repoRoot 'profiles.example\示例配置') 'profiles\示例配置'
    $globalProfilePath = Join-Path $profilesDestination 'Global\profile.json'
    New-Item -ItemType Directory -Path (Split-Path -Parent $globalProfilePath) -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $profilesDestination 'Global\macros') -Force | Out-Null
    $globalLuaDirectory = Join-Path $profilesDestination 'Global\lua'
    New-Item -ItemType Directory -Path $globalLuaDirectory -Force | Out-Null
    [System.IO.File]::WriteAllText(
        (Join-Path $globalLuaDirectory 'main.txt'),
        '',
        [System.Text.UTF8Encoding]::new($false))
    $globalProfile = @(
        '{',
        '  "apps": [],',
        '  "lua_script_file": "lua/main.txt",',
        '  "macros": {}',
        '}'
    ) -join [Environment]::NewLine
    [System.IO.File]::WriteAllText(
        $globalProfilePath,
        $globalProfile,
        [System.Text.UTF8Encoding]::new($false))

    Copy-RequiredDirectory (Join-Path $repoRoot 'drivers\wch-ch341ser') 'drivers\wch-ch341ser'

    Copy-RequiredFile (Join-Path $repoRoot 'firmware\build\flasher_args.json') 'firmware\flasher_args.json'
    Copy-RequiredFile (Join-Path $repoRoot 'firmware\build\bootloader\bootloader.bin') 'firmware\bootloader\bootloader.bin'
    Copy-RequiredFile (Join-Path $repoRoot 'firmware\build\partition_table\partition-table.bin') 'firmware\partition_table\partition-table.bin'
    Copy-RequiredFile (Join-Path $repoRoot 'firmware\build\dual_s3_hid_proxy.bin') 'firmware\dual_s3_hid_proxy.bin'

    $packageReadme = @(
        '# ESP32-S3 HID Bridge 发布件',
        '',
        '启动 `HidBridge.Host.exe` 即可使用。程序会优先使用同目录的 `bridge.json`，并自动加载 `profiles/` 下的默认配置。每个配置的宏正文位于 `macros/`，Lua 正文位于 `lua/`，`profile.json` 只保存文件关联和运行元数据。鼠标捕获页提供 0.3–3.0 的统一输出灵敏度，只作用于 **Host 发往板端**的 X/Y 相对移动（网络 UDP、Lua/宏及软件输入），1 为原始值，滚轮和按键不变；**双板模式下 M→P 的实体鼠标是硬件直通，不经过该灵敏度**（旧单板模式的实体转发才受它影响）。“始终开启 UDP 输出”默认开启，HOME 关闭时仍可发送网络 UDP。设置页的“UDP 模拟输入测试”默认关闭，仅在旧版单板通路且 HOME 同步开启时生效，用于验证软件输入聚合与旧单板 7 字节串口报告；它不是网络 UDP，也不用于双板 M 软件输入。Lua 输入栏左侧显示行号；Lua 页“检查”会在不改变换行的前提下对齐缩进并规范常见行内空格；`delay(ms)`、`sleep(ms)` 和 `Sleep(ms)` 共用可取消延时实现，`move(x, y)` 支持小数累计移动，Lua 错误会显示行号。Host 不额外生成 `press arg=`/`release arg=` 摘要，脚本主动调用 `DebugLog(...)` 的内容仍会显示。',
        '',
        '## 驱动',
        '',
        '`drivers/wch-ch341ser/` 保存 WCH CH340/CH341 官方驱动。Host 以 serial 模式启动时，只有已插入 `USB\\VID_1A86&PID_7523` 且 Windows Problem Code 为 28，或设备节点暂时不可见且只读 Driver Store 未检测到 `CH341SER.INF` 时，才会显示确认窗口；Working、已安装驱动但未插入设备、Driver Store 探测失败和其他 Problem Code 不会自动安装。只有用户确认后才通过 UAC 执行安装，安装后无设备时只确认 Driver Store，不虚报 COM。',
        '',
        '## 固件',
        '',
        '本发布件携带的是**双板固件**（`firmware/`，工程名 `dual_s3_hid_proxy`）：**两块 ESP32-S3 刷同一个镜像** `firmware/dual_s3_hid_proxy.bin`，上电后由固件按 USB 连接证据与板间 UART1 身份协商 M（鼠标侧）/P（电脑侧）角色，不需要分别编译两份固件。`firmware/flasher_args.json` 与同目录相对路径下的三段镜像（bootloader、partition table、app）组成可刷写清单；可在 Host 设置页选择该 JSON 清单，或使用本机刷写 API，按 P→M 顺序分别刷入两块板。两块板必须刷同一版本：`PROFILE_ACK` 长度（17 bytes）即版本判据，混刷会判失败。',
        '',
        '单板固件不在本发布件内，也不由本分支构建；如需旧单板方案，请在 `single-board` 分支获取源码与构建入口。',
        '',
        '固件按 **4 MB Flash** 目标构建，分区表为 `nvs`（24 KB）、`phy_init`（4 KB）与 `factory` 应用（1 MB），表尾 `0x110000`（约 1.06 MB），因此 **4 MB 及以上的 ESP32-S3 板卡都可用**，8 MB 板卡同样可用（更大的 Flash 不会被使用）。板卡不需要 PSRAM。若此前刷过含 4 MB `storage` 分区的旧版本，本次需要整片重刷（bootloader + partition table + app 三段）。',
        '',
        '## 校验',
        '',
        '`SHA256SUMS.txt` 包含本发布件内每个文件的 SHA-256。发布件由项目根目录的 `scripts/Prepare-Release.ps1` 生成，根目录也会保留同一份最新 `HidBridge.Host.exe`。'
    ) -join [Environment]::NewLine
    [System.IO.File]::WriteAllText(
        (Join-Path $stagingDirectory 'README.md'),
        $packageReadme,
        [System.Text.UTF8Encoding]::new($false))

    $sumPath = Join-Path $stagingDirectory 'SHA256SUMS.txt'
    $sumLines = Get-ChildItem -LiteralPath $stagingDirectory -File -Recurse |
        Sort-Object FullName |
        ForEach-Object {
            $relative = $_.FullName.Substring($stagingDirectory.Length).TrimStart('\').Replace('\', '/')
            $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToUpperInvariant()
            "$hash  $relative"
        }
    [System.IO.File]::WriteAllLines($sumPath, $sumLines, [System.Text.UTF8Encoding]::new($false))

    New-Item -ItemType Directory -Path (Split-Path -Parent $releaseDirectory) -Force | Out-Null
    if (Test-Path -LiteralPath $releaseDirectory) {
        [System.IO.Directory]::Delete($releaseDirectory, $true)
    }
    [System.IO.Directory]::Move($stagingDirectory, $releaseDirectory)
    $stagingDirectory = $null

    $fileCount = @(Get-ChildItem -LiteralPath $releaseDirectory -File -Recurse).Count
    Write-Host "发布件已生成：$releaseDirectory（$fileCount 个文件）" -ForegroundColor Green
    Write-Host "根目录 EXE：$hostExe" -ForegroundColor Green
}
finally {
    if ($null -ne $stagingDirectory -and (Test-Path -LiteralPath $stagingDirectory)) {
        [System.IO.Directory]::Delete($stagingDirectory, $true)
    }
}

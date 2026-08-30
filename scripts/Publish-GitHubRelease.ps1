[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v\d+\.\d+(?:\.\d+)?(?:-[0-9A-Za-z][0-9A-Za-z.-]*)?$')]
    [string]$Tag,

    [string]$Title,
    [string]$NotesFile,

    [switch]$Draft,
    [switch]$Prerelease,
    [switch]$BuildFirmware,
    [switch]$SkipHostBuild,
    [switch]$PackageOnly,
    [switch]$AllowDirty,

    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._/-]*$')]
    [string]$Target,

    [ValidatePattern('^[^/\s]+/[^/\s]+$')]
    [string]$Repo
)

$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$releaseDirectory = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'release'))
$distDirectory = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'dist'))
$prepareScript = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'scripts\Prepare-Release.ps1'))
$packageName = "ESP32-S3-HID-Bridge-$Tag.zip"
$packagePath = Join-Path $distDirectory $packageName
$packageHashPath = "$packagePath.sha256"

if (-not [string]::IsNullOrWhiteSpace($NotesFile)) {
    $notesPath = [System.IO.Path]::GetFullPath($NotesFile)
    if (-not (Test-Path -LiteralPath $notesPath -PathType Leaf)) {
        throw "NotesFile 不存在：$notesPath"
    }
    $NotesFile = $notesPath
}

function Get-CanonicalPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    return [System.IO.Path]::GetFullPath($Path)
}

function Assert-PathUnder {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Candidate,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $rootPath = (Get-CanonicalPath $Root).TrimEnd('\', '/') + '\'
    $candidatePath = Get-CanonicalPath $Candidate
    if (-not $candidatePath.StartsWith($rootPath, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description 路径必须位于 $Root 内：$candidatePath"
    }

}

function Get-ReleaseRelativePath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $relative = (Get-CanonicalPath $Path).Substring($releaseDirectory.Length).TrimStart('\', '/')
    if ([string]::IsNullOrWhiteSpace($relative)) {
        throw "发布件文件不能是 release 目录本身：$Path"
    }

    return $relative.Replace('\', '/')
}

function Invoke-PrepareRelease {
    if (-not (Test-Path -LiteralPath $prepareScript -PathType Leaf)) {
        throw "找不到现有发布件生成脚本：$prepareScript"
    }

    $prepareArguments = @{}
    if ($SkipHostBuild) {
        $prepareArguments['SkipHostBuild'] = $true
    }
    if ($BuildFirmware) {
        $prepareArguments['BuildFirmware'] = $true
    }

    if ($SkipHostBuild) {
        Write-Warning '已显式使用 -SkipHostBuild；脚本仍会校验根目录 EXE 与 release/ EXE 完全一致。'
    }
    else {
        Write-Host '未跳过 Host 构建：先调用 Prepare-Release.ps1 重建最新 Host 发布件。' -ForegroundColor Cyan
    }

    & $prepareScript @prepareArguments
    if (-not $?) {
        throw "Prepare-Release.ps1 执行失败。"
    }
}

function Get-GitOutput {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    $output = @(& git -C $repoRoot @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "git $($Arguments -join ' ') 执行失败（Exit $exitCode）：$($output -join ' ')"
    }

    return ($output | Out-String).Trim()
}

function Assert-GitWorkspace {
    $gitCommand = Get-Command git -ErrorAction SilentlyContinue
    if ($null -eq $gitCommand) {
        throw '找不到 git；正常发布必须在 Git 仓库中执行。'
    }

    $gitRoot = Get-GitOutput @('rev-parse', '--show-toplevel')
    if ((Get-CanonicalPath $gitRoot) -ne (Get-CanonicalPath $repoRoot)) {
        throw "当前脚本目录不是 Git 仓库根目录：$repoRoot（git 根目录=$gitRoot）"
    }

    $trackedDirty = @(& git -C $repoRoot status --porcelain=v1 --untracked-files=no 2>&1)
    $statusExitCode = $LASTEXITCODE
    if ($statusExitCode -ne 0) {
        throw "无法读取 Git 工作区状态（Exit $statusExitCode）：$($trackedDirty -join ' ')"
    }
    if ($trackedDirty.Count -gt 0) {
        if (-not $AllowDirty) {
            throw "正常发布拒绝 tracked dirty 工作区；如确认要发布请显式使用 -AllowDirty。`n$($trackedDirty -join [Environment]::NewLine)"
        }

        Write-Warning '已使用 -AllowDirty：当前 tracked dirty 工作区将继续发布，请在发布记录中明确说明。'
    }

    if ([string]::IsNullOrWhiteSpace($Target)) {
        $script:Target = Get-GitOutput @('rev-parse', 'HEAD')
    }
    if ($Target -notmatch '^[A-Za-z0-9][A-Za-z0-9._/-]*$') {
        throw "Target 只能包含字母、数字、点、下划线、斜杠或连字符，当前值：$Target"
    }
}

function Invoke-GhCheck {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $output = @(& gh @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    if ($exitCode -eq 0) {
        throw "$Description 已存在，停止发布：$($output -join ' ')"
    }

    $message = $output -join ' '
    if ($message -notmatch '(?i)(404|not found|不存在)') {
        throw "$Description 检查失败，不能确认远端不存在（Exit $exitCode）：$message"
    }
}

function Assert-GitHubSafety {
    $ghCommand = Get-Command gh -ErrorAction SilentlyContinue
    if ($null -eq $ghCommand) {
        throw '找不到 gh；正常发布需要 GitHub CLI。PackageOnly 不要求 gh。'
    }

    $authOutput = @(& gh auth status 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "gh 未登录或认证不可用：$($authOutput -join ' ')"
    }

    if ([string]::IsNullOrWhiteSpace($Repo)) {
        $repoOutput = @(& gh repo view --json nameWithOwner --jq .nameWithOwner 2>&1)
        if ($LASTEXITCODE -ne 0) {
            throw "无法从当前 GitHub 仓库解析 owner/repo：$($repoOutput -join ' ')"
        }
        $script:Repo = ($repoOutput | Out-String).Trim()
    }
    if ($Repo -notmatch '^[^/\s]+/[^/\s]+$') {
        throw "Repo 必须是 owner/repo：$Repo"
    }

    $localTagOutput = @(& git -C $repoRoot show-ref --verify --quiet ("refs/tags/{0}" -f $Tag) 2>&1)
    if ($LASTEXITCODE -eq 0) {
        throw "本地 tag 已存在：$Tag；为避免覆盖，请使用新的版本 tag。"
    }

    Invoke-GhCheck @('api', '--silent', ("repos/{0}/git/ref/tags/{1}" -f $Repo, $Tag)) "GitHub tag $Tag"
    Invoke-GhCheck @('api', '--silent', ("repos/{0}/releases/tags/{1}" -f $Repo, $Tag)) "GitHub Release $Tag"
}

function Get-ReleaseFiles {
    if (-not (Test-Path -LiteralPath $releaseDirectory -PathType Container)) {
        throw "Prepare-Release.ps1 未生成 release 目录：$releaseDirectory"
    }

    $files = @(Get-ChildItem -LiteralPath $releaseDirectory -File -Recurse | Sort-Object FullName)
    if ($files.Count -eq 0) {
        throw "release 目录为空：$releaseDirectory"
    }

    return $files
}

function Assert-ReleaseChecksumManifest {
    param([Parameter(Mandatory = $true)][System.IO.FileInfo[]]$Files)

    $sumPath = Join-Path $releaseDirectory 'SHA256SUMS.txt'
    if (-not (Test-Path -LiteralPath $sumPath -PathType Leaf)) {
        throw "发布件缺少 SHA256SUMS.txt：$sumPath"
    }

    $entries = @{}
    foreach ($line in @(Get-Content -LiteralPath $sumPath -Encoding UTF8)) {
        $trimmed = $line.Trim()
        if ([string]::IsNullOrWhiteSpace($trimmed)) {
            continue
        }
        if ($trimmed -notmatch '^(?<hash>[0-9A-Fa-f]{64})\s{2,}(?<path>.+)$') {
            throw "SHA256SUMS.txt 行格式无效：$line"
        }

        $relative = $Matches['path'].Trim().Replace('\', '/')
        if ($relative.StartsWith('/') -or $relative -match '(^|/)\.\.(?:/|$)') {
            throw "SHA256SUMS.txt 含越界路径：$relative"
        }
        if ($entries.ContainsKey($relative)) {
            throw "SHA256SUMS.txt 含重复路径：$relative"
        }
        $entries[$relative] = $Matches['hash'].ToUpperInvariant()
    }

    $expectedFiles = @($Files | Where-Object { (Get-ReleaseRelativePath $_.FullName) -ne 'SHA256SUMS.txt' })
    $expectedNames = @($expectedFiles | ForEach-Object { Get-ReleaseRelativePath $_.FullName })
    if ($entries.Count -ne $expectedNames.Count) {
        throw "SHA256SUMS.txt 条目数不匹配：entries=$($entries.Count)，files=$($expectedNames.Count)"
    }

    foreach ($relative in $expectedNames) {
        if (-not $entries.ContainsKey($relative)) {
            throw "SHA256SUMS.txt 缺少文件：$relative"
        }
        $filePath = Join-Path $releaseDirectory ($relative.Replace('/', '\'))
        $actualHash = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToUpperInvariant()
        if ($entries[$relative] -ne $actualHash) {
            throw "SHA256SUMS.txt 哈希不匹配：$relative；manifest=$($entries[$relative])，actual=$actualHash"
        }
    }

    foreach ($relative in @($entries.Keys)) {
        if ($relative -notin $expectedNames) {
            throw "SHA256SUMS.txt 指向 release 中不存在的文件：$relative"
        }
    }

    Write-Host "release/SHA256SUMS.txt 校验通过：$($entries.Count) 个文件。" -ForegroundColor Green
}

function Remove-ExistingPackageFiles {
    Assert-PathUnder $distDirectory $packagePath 'ZIP 输出'
    Assert-PathUnder $distDirectory $packageHashPath 'ZIP SHA-256 输出'
    New-Item -ItemType Directory -Path $distDirectory -Force | Out-Null

    foreach ($path in @($packagePath, $packageHashPath)) {
        if (Test-Path -LiteralPath $path -PathType Container) {
            throw "目标输出路径是目录，拒绝删除：$path"
        }
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            [System.IO.File]::Delete($path)
            Write-Host "仅删除本 tag 对应旧输出：$path" -ForegroundColor DarkYellow
        }
    }
}

function New-ReleaseZip {
    param([Parameter(Mandatory = $true)][System.IO.FileInfo[]]$Files)

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    $zipStream = $null
    $archive = $null
    try {
        $zipStream = [System.IO.File]::Open(
            $packagePath,
            [System.IO.FileMode]::Create,
            [System.IO.FileAccess]::ReadWrite,
            [System.IO.FileShare]::None)
        $archive = [System.IO.Compression.ZipArchive]::new(
            $zipStream,
            [System.IO.Compression.ZipArchiveMode]::Create,
            $false)

        foreach ($file in $Files) {
            $relative = Get-ReleaseRelativePath $file.FullName
            if ($relative.StartsWith('release/', [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "ZIP 条目不得包含外层 release 目录：$relative"
            }

            $entry = $archive.CreateEntry($relative)
            $inputStream = $null
            $entryStream = $null
            try {
                $inputStream = [System.IO.File]::OpenRead($file.FullName)
                $entryStream = $entry.Open()
                $inputStream.CopyTo($entryStream)
            }
            finally {
                if ($null -ne $entryStream) {
                    $entryStream.Dispose()
                }
                if ($null -ne $inputStream) {
                    $inputStream.Dispose()
                }
            }
        }
    }
    finally {
        if ($null -ne $archive) {
            $archive.Dispose()
        }
        if ($null -ne $zipStream) {
            $zipStream.Dispose()
        }
    }
}

function Assert-ZipMatchesRelease {
    param([Parameter(Mandatory = $true)][System.IO.FileInfo[]]$Files)

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    $expectedNames = @($Files | ForEach-Object { Get-ReleaseRelativePath $_.FullName } | Sort-Object)
    $zipStream = $null
    $archive = $null
    try {
        $zipStream = [System.IO.File]::OpenRead($packagePath)
        $archive = [System.IO.Compression.ZipArchive]::new(
            $zipStream,
            [System.IO.Compression.ZipArchiveMode]::Read,
            $false)
        $actualNames = @($archive.Entries | ForEach-Object { $_.FullName } | Sort-Object)

        if ($actualNames.Count -ne $expectedNames.Count) {
            throw "ZIP 条目数与 release 文件数不一致：zip=$($actualNames.Count)，release=$($expectedNames.Count)"
        }

        $comparison = Compare-Object -ReferenceObject $expectedNames -DifferenceObject $actualNames
        if ($null -ne $comparison) {
            throw "ZIP 条目与 release 文件清单不一致：$($comparison | Out-String)"
        }

        foreach ($entryName in $actualNames) {
            if ($entryName.StartsWith('release/', [System.StringComparison]::OrdinalIgnoreCase) -or
                $entryName.Contains('\')) {
                throw "ZIP 条目不是直接发布根或使用了 Windows 反斜杠：$entryName"
            }
        }
    }
    finally {
        if ($null -ne $archive) {
            $archive.Dispose()
        }
        if ($null -ne $zipStream) {
            $zipStream.Dispose()
        }
    }

    Write-Host "ZIP 条目校验通过：$($expectedNames.Count) 个条目，根目录无外层 release。" -ForegroundColor Green
}

function Write-AndVerifyPackageHash {
    $zipHash = (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash.ToUpperInvariant()
    $hashLine = "$zipHash  $packageName"
    [System.IO.File]::WriteAllText(
        $packageHashPath,
        $hashLine + [Environment]::NewLine,
        [System.Text.UTF8Encoding]::new($false))

    $writtenHashLine = (Get-Content -LiteralPath $packageHashPath -Raw).Trim()
    if ($writtenHashLine -ne $hashLine) {
        throw "外部 ZIP SHA-256 文件校验失败：$packageHashPath"
    }

    Write-Host "ZIP SHA-256：$zipHash" -ForegroundColor Green
    return $zipHash
}

try {
    Assert-PathUnder $repoRoot $releaseDirectory 'release 目录'
    Assert-PathUnder $repoRoot $distDirectory 'dist 目录'

    if (-not $PackageOnly) {
        Assert-GitWorkspace
        Assert-GitHubSafety
    }
    else {
        # PackageOnly 仍确认脚本位于当前仓库，但不要求 gh 登录、远端可达或工作树干净。
        $gitCommand = Get-Command git -ErrorAction SilentlyContinue
        if ($null -eq $gitCommand) {
            throw 'PackageOnly 仍需要 git 以确认当前仓库；找不到 git。'
        }
        $gitRoot = Get-GitOutput @('rev-parse', '--show-toplevel')
        if ((Get-CanonicalPath $gitRoot) -ne (Get-CanonicalPath $repoRoot)) {
            throw "当前脚本目录不是 Git 仓库根目录：$repoRoot（git 根目录=$gitRoot）"
        }
        if ([string]::IsNullOrWhiteSpace($Target)) {
            $Target = Get-GitOutput @('rev-parse', 'HEAD')
        }
        Write-Host 'PackageOnly：跳过 gh 登录、远端 tag/Release 检查和 Git 工作树洁净检查。' -ForegroundColor DarkYellow
    }

    Invoke-PrepareRelease

    $rootExe = Join-Path $repoRoot 'HidBridge.Host.exe'
    $releaseExe = Join-Path $releaseDirectory 'HidBridge.Host.exe'
    if (-not (Test-Path -LiteralPath $rootExe -PathType Leaf) -or
        -not (Test-Path -LiteralPath $releaseExe -PathType Leaf)) {
        throw "根目录或 release/ 缺少 HidBridge.Host.exe；root=$rootExe，release=$releaseExe"
    }
    $rootExeInfo = Get-Item -LiteralPath $rootExe
    $releaseExeInfo = Get-Item -LiteralPath $releaseExe
    $rootExeHash = (Get-FileHash -LiteralPath $rootExe -Algorithm SHA256).Hash.ToUpperInvariant()
    $releaseExeHash = (Get-FileHash -LiteralPath $releaseExe -Algorithm SHA256).Hash.ToUpperInvariant()
    if ($rootExeInfo.Length -ne $releaseExeInfo.Length -or $rootExeHash -ne $releaseExeHash) {
        throw "根目录 EXE 与 release/ EXE 不一致，拒绝打包：root=$($rootExeInfo.Length) bytes/$rootExeHash；release=$($releaseExeInfo.Length) bytes/$releaseExeHash"
    }
    Write-Host "根目录与 release/ EXE 一致：$($rootExeInfo.Length) bytes，SHA-256 $rootExeHash。" -ForegroundColor Green

    $requiredInf = Join-Path $releaseDirectory 'drivers\wch-ch341ser\CH341SER\CH341SER.INF'
    if (-not (Test-Path -LiteralPath $requiredInf -PathType Leaf)) {
        throw "发布件缺少强制 CH340/CH341 INF：$requiredInf"
    }

    $releaseFiles = Get-ReleaseFiles
    Assert-ReleaseChecksumManifest $releaseFiles
    Remove-ExistingPackageFiles
    New-ReleaseZip $releaseFiles
    Assert-ZipMatchesRelease $releaseFiles
    $packageHash = Write-AndVerifyPackageHash

    $packageInfo = Get-Item -LiteralPath $packagePath
    $entryCount = $releaseFiles.Count
    Write-Host "发布 ZIP 已生成：$packagePath" -ForegroundColor Green
    Write-Host "ZIP 大小：$($packageInfo.Length) bytes；条目数：$entryCount；外部哈希：$packageHashPath" -ForegroundColor Green

    if ($PackageOnly) {
        Write-Host 'PackageOnly 完成：未创建 Git tag，未创建或上传 GitHub Release。' -ForegroundColor Cyan
        return
    }

    $ghArguments = @('release', 'create', $Tag, '--target', $Target, '--repo', $Repo)
    if (-not [string]::IsNullOrWhiteSpace($Title)) {
        $ghArguments += @('--title', $Title)
    }
    if (-not [string]::IsNullOrWhiteSpace($NotesFile)) {
        $ghArguments += @('--notes-file', (Get-CanonicalPath $NotesFile))
    }
    else {
        $ghArguments += '--generate-notes'
    }
    if ($Draft) {
        $ghArguments += '--draft'
    }
    if ($Prerelease) {
        $ghArguments += '--prerelease'
    }
    $ghArguments += @($packagePath, $packageHashPath)

    Write-Host "创建 GitHub Release：$Repo/$Tag，target=$Target" -ForegroundColor Cyan
    & gh @ghArguments
    if ($LASTEXITCODE -ne 0) {
        throw "gh release create 失败，退出码：$LASTEXITCODE"
    }
    Write-Host "GitHub Release 已创建并上传：$Repo/$Tag" -ForegroundColor Green
}
catch {
    Write-Error $_
    exit 1
}

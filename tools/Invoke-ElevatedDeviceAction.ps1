# 提权辅助脚本：在管理员 PowerShell 里执行单个动作，并把结果写回文件供主流程读取。
#
# 用途：Windows 上“用软件拔插 USB 口”（Disable-PnpDevice/Enable-PnpDevice）必须要管理员，
# 而主流程不是管理员。主流程用 Start-Process -Verb RunAs 拉起本脚本（会弹 UAC，由用户点“是”），
# 脚本把每一步的真实结果追加写入 -ResultPath，主流程读该文件判断成败。
#
# 示例：
#   pwsh -File tools/Invoke-ElevatedDeviceAction.ps1 -Action self-check -ResultPath out.txt
#   pwsh -File tools/Invoke-ElevatedDeviceAction.ps1 -Action disable -Match 'VID_046D&PID_C092' -ResultPath out.txt
#   pwsh -File tools/Invoke-ElevatedDeviceAction.ps1 -Action enable  -Match 'VID_046D&PID_C092' -ResultPath out.txt
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('self-check', 'list', 'disable', 'enable', 'cycle')]
    [string]$Action,

    [string]$Match = '',

    [int]$HoldSeconds = 6,

    [Parameter(Mandatory = $true)]
    [string]$ResultPath
)

$ErrorActionPreference = 'Continue'

function Write-Result {
    param([string]$Text)
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss.fff'), $Text
    Add-Content -Path $ResultPath -Value $line -Encoding UTF8
    Write-Host $line
}

function Test-Admin {
    return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

Write-Result ("动作={0} 管理员={1}" -f $Action, (Test-Admin))

if (-not (Test-Admin)) {
    Write-Result '失败：未获得管理员权限（UAC 未通过），不执行任何设备操作。'
    exit 3
}

switch ($Action) {
    'self-check' {
        Write-Result '提权通道可用。'
        exit 0
    }
    'list' {
        $devices = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
            Where-Object { $_.InstanceId -match 'VID_' -and $_.InstanceId -like 'USB\*' } |
            Select-Object -ExpandProperty InstanceId
        foreach ($device in $devices) {
            Write-Result ("设备 {0}" -f $device)
        }
        exit 0
    }
    default {
        if ([string]::IsNullOrWhiteSpace($Match)) {
            Write-Result '失败：disable/enable 需要 -Match 选择器。'
            exit 2
        }
        $targets = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
            Where-Object { $_.InstanceId -match 'VID_' -and $_.InstanceId -like 'USB\*' -and
                           $_.InstanceId -match [regex]::Escape($Match) }
        if (-not $targets) {
            Write-Result ("失败：没有匹配 {0} 的 USB 设备。" -f $Match)
            exit 4
        }
        $exitCode = 0
        # cycle = 先断开、保持一段时间、再恢复，等价于“拔掉再插回”（只是 VBUS 不断）。
        $verbs = if ($Action -eq 'cycle') { @('Disable-PnpDevice', 'Enable-PnpDevice') }
                 else { @($(if ($Action -eq 'disable') { 'Disable-PnpDevice' }
                             else { 'Enable-PnpDevice' })) }
        foreach ($verb in $verbs) {
            foreach ($target in $targets) {
                try {
                    & $verb -InstanceId $target.InstanceId -Confirm:$false -ErrorAction Stop
                    Write-Result ("{0} 成功：{1}" -f $verb, $target.InstanceId)
                } catch {
                    Write-Result ("{0} 失败：{1} -> {2}" -f $verb, $target.InstanceId, $_.Exception.Message)
                    $exitCode = 5
                }
            }
            if ($Action -eq 'cycle' -and $verb -eq 'Disable-PnpDevice') {
                Write-Result ("保持断开 {0} 秒（模拟插头拔出期间）" -f $HoldSeconds)
                Start-Sleep -Seconds $HoldSeconds
            }
        }
        exit $exitCode
    }
}

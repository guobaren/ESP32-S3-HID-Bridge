# 需要用户动手前的提示音（用户要求：后续需要操作时先发声提醒；用蜂鸣器单次嘀声）。
# 用法：pwsh -File tools/notify_user.ps1 [-Message "要做什么"]
param(
    [string]$Message = "需要你操作",
    [int]$Frequency = 1000,
    [int]$DurationMs = 180
)

# 用户明确要求“蜂鸣器”的嘀声，而不是 Windows 提示音：优先 Console.Beep
# （主板/系统喇叭），失败时才退回 MessageBeep。
$ok = $false
try {
    [console]::beep($Frequency, $DurationMs)
    $ok = $true
} catch {
    try {
        if (-not ("DshBeep" -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class DshBeep {
    [DllImport("user32.dll")]
    public static extern bool MessageBeep(uint uType);
}
'@ -ErrorAction Stop | Out-Null
        }
        [DshBeep]::MessageBeep(0x00000040) | Out-Null
        $ok = $true
    } catch { }
}

Write-Host ("蜂鸣提示：" + $Message) -ForegroundColor Yellow
if (-not $ok) { Write-Host "（Console.Beep 与 MessageBeep 都不可用）" -ForegroundColor DarkYellow }

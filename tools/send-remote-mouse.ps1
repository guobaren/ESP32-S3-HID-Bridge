param(
    [Parameter(Mandatory = $true)]
    [string]$HostAddress,

    [int]$Port = 24814,

    [Parameter(Mandatory = $true)]
    [string]$PresharedKey,

    [ValidateRange(-32768, 32767)]
    [int]$Dx = 0,

    [ValidateRange(-32768, 32767)]
    [int]$Dy = 0,

    [ValidateRange(-128, 127)]
    [int]$Wheel = 0,

    [ValidateRange(-128, 127)]
    [int]$Pan = 0
)

$ErrorActionPreference = 'Stop'
if ($Dx -eq 0 -and $Dy -eq 0 -and $Wheel -eq 0 -and $Pan -eq 0) {
    throw '至少提供一个非零的 Dx、Dy、Wheel 或 Pan。'
}

$payload = [ordered]@{
    token = $PresharedKey
    dx = $Dx
    dy = $Dy
    wheel = $Wheel
    pan = $Pan
} | ConvertTo-Json -Compress
$bytes = [System.Text.Encoding]::UTF8.GetBytes($payload)
$client = [System.Net.Sockets.UdpClient]::new()
try {
    [void]$client.Send($bytes, $bytes.Length, $HostAddress, $Port)
    Write-Host "已发送 UDP 模拟鼠标命令到 ${HostAddress}:$Port：dx=$Dx dy=$Dy wheel=$Wheel pan=$Pan"
}
finally {
    $client.Dispose()
}

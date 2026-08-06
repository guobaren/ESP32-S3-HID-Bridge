# Command-line BLE HID pairing and connection helper.
# Run with Windows PowerShell 5.1:
# powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\connect-ble-hid.ps1
[CmdletBinding()]
param(
    [string]$Name = 'HidBridge Keyboard Mouse',
    [string]$Address = '',
    [ValidateRange(1, 300)][int]$ScanSeconds = 20,
    [ValidateRange(0, 86400)][int]$DurationSeconds = 0
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Runtime.WindowsRuntime
[Windows.Devices.Enumeration.DeviceInformation, Windows, ContentType = WindowsRuntime] | Out-Null
[Windows.Devices.Enumeration.DeviceInformationCollection, Windows, ContentType = WindowsRuntime] | Out-Null
[Windows.Devices.Enumeration.DevicePairingResult, Windows, ContentType = WindowsRuntime] | Out-Null
[Windows.Devices.Enumeration.DevicePairingProtectionLevel, Windows, ContentType = WindowsRuntime] | Out-Null
[Windows.Devices.Bluetooth.BluetoothLEDevice, Windows, ContentType = WindowsRuntime] | Out-Null
[Windows.Devices.Bluetooth.BluetoothCacheMode, Windows, ContentType = WindowsRuntime] | Out-Null
[Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceServicesResult, Windows, ContentType = WindowsRuntime] | Out-Null
[Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher, Windows, ContentType = WindowsRuntime] | Out-Null
[Windows.Devices.Bluetooth.Advertisement.BluetoothLEScanningMode, Windows, ContentType = WindowsRuntime] | Out-Null

function Wait-WinRtOperation {
    param(
        [Parameter(Mandatory)]$Operation,
        [Parameter(Mandatory)][Type]$ResultType
    )

    $method = [System.WindowsRuntimeSystemExtensions].GetMethods() |
        Where-Object {
            $_.Name -eq 'AsTask' -and
            $_.IsGenericMethodDefinition -and
            $_.GetGenericArguments().Count -eq 1 -and
            $_.GetParameters().Count -eq 1 -and
            $_.GetParameters()[0].ParameterType.ToString() -like 'Windows.Foundation.IAsyncOperation*'
        } |
        Select-Object -First 1
    if ($null -eq $method) {
        throw 'Cannot find the WinRT IAsyncOperation adapter.'
    }

    $task = $method.MakeGenericMethod($ResultType).Invoke($null, @($Operation))
    return $task.GetAwaiter().GetResult()
}

function Format-BluetoothAddress {
    param([Parameter(Mandatory)][UInt64]$Address)

    return ('{0:X12}' -f $Address)
}

function Convert-BluetoothAddress {
    param([Parameter(Mandatory)][string]$Text)

    $normalized = $Text.Replace(':', '').Replace('-', '').Trim()
    if ($normalized -notmatch '^[0-9a-fA-F]{12}$') {
        throw "Bluetooth address '$Text' must contain exactly 12 hexadecimal digits."
    }
    return [Convert]::ToUInt64($normalized, 16)
}

function Find-KnownDevice {
    param([Parameter(Mandatory)][string]$TargetName)

    $selector = [Windows.Devices.Bluetooth.BluetoothLEDevice]::GetDeviceSelector()
    $operation = [Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($selector)
    $devices = Wait-WinRtOperation $operation ([Windows.Devices.Enumeration.DeviceInformationCollection])
    return $devices |
        Where-Object { $_.Name -eq $TargetName -or $_.Name -like "*$TargetName*" } |
        Select-Object -First 1
}

function Find-Advertisement {
    param(
        [Parameter(Mandatory)][string]$TargetName,
        [Parameter(Mandatory)][int]$Seconds
    )

    # DeviceInformation.GetDeviceSelector() only enumerates devices already known
    # to Windows. An unpaired peripheral is discovered through advertisement events.
    $script:ExpectedAdvertisementName = $TargetName
    $script:AdvertisementMatch = $null
    $watcher = [Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher]::new()
    $watcher.ScanningMode = [Windows.Devices.Bluetooth.Advertisement.BluetoothLEScanningMode]::Active
    $token = $watcher.Add_Received({
            param($sender, $args)

            $advertisedName = $args.Advertisement.LocalName
            if ([string]::IsNullOrWhiteSpace($advertisedName) -or
                ($advertisedName -ne $script:ExpectedAdvertisementName -and
                    $advertisedName -notlike "*$($script:ExpectedAdvertisementName)*")) {
                return
            }

            $address = [UInt64]$args.BluetoothAddress
            $serviceUuids = @($args.Advertisement.ServiceUuids | ForEach-Object { $_.ToString() }) -join ','
            $script:AdvertisementMatch = [pscustomobject]@{
                Address = $address
                Name = $advertisedName
                Rssi = $args.RawSignalStrengthInDBm
                AdvertisementType = $args.AdvertisementType
                ServiceUuids = $serviceUuids
            }
            Write-Host (
                'BLE advertisement: Name={0}; Address={1}; RSSI={2}; Type={3}; Services={4}' -f
                $advertisedName,
                (Format-BluetoothAddress $address),
                $args.RawSignalStrengthInDBm,
                $args.AdvertisementType,
                $serviceUuids)
        })

    try {
        $watcher.Start()
        $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
        while ($null -eq $script:AdvertisementMatch -and [DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 200
        }
    }
    finally {
        $watcher.Stop()
        $watcher.Remove_Received($token)
    }

    return $script:AdvertisementMatch
}

Write-Host 'Local Bluetooth adapters:'
Get-PnpDevice -PresentOnly -Class Bluetooth |
    Select-Object Status, FriendlyName, InstanceId |
    Format-Table -AutoSize

$target = $null
$ble = $null
if (-not [string]::IsNullOrWhiteSpace($Address)) {
    $explicitAddress = Convert-BluetoothAddress $Address
    Write-Host "Opening BluetoothLEDevice from the supplied address $(Format-BluetoothAddress $explicitAddress)..."
    $ble = Wait-WinRtOperation (
        [Windows.Devices.Bluetooth.BluetoothLEDevice]::FromBluetoothAddressAsync($explicitAddress)
    ) ([Windows.Devices.Bluetooth.BluetoothLEDevice])
    if ($null -eq $ble) {
        [Console]::Error.WriteLine("Windows could not open BluetoothLEDevice from address '$Address'.")
        exit 4
    }
    $target = $ble.DeviceInformation
}
else {
    $target = Find-KnownDevice $Name
    if ($null -eq $target) {
        Write-Host "No known BLE device. Scanning advertisements for $ScanSeconds seconds: $Name"
        $advertisement = Find-Advertisement -TargetName $Name -Seconds $ScanSeconds
        if ($null -eq $advertisement) {
            [Console]::Error.WriteLine("Device '$Name' was not found in BLE advertisements. No pairing or connection was attempted.")
            exit 2
        }

        Write-Host 'Opening BluetoothLEDevice from the advertised address...'
        $ble = Wait-WinRtOperation (
            [Windows.Devices.Bluetooth.BluetoothLEDevice]::FromBluetoothAddressAsync([UInt64]$advertisement.Address)
        ) ([Windows.Devices.Bluetooth.BluetoothLEDevice])
        if ($null -eq $ble) {
            [Console]::Error.WriteLine('Windows could not open BluetoothLEDevice from the advertised address.')
            exit 4
        }

        $target = $ble.DeviceInformation
    }
}

Write-Host "Target: Name=$($target.Name) Id=$($target.Id) Paired=$($target.Pairing.IsPaired) Connected=$($target.IsConnected)"
if (-not $target.Pairing.IsPaired) {
    Write-Host 'Device is not paired. Calling PairAsync...'
$pairResult = Wait-WinRtOperation ($target.Pairing.PairAsync([Windows.Devices.Enumeration.DevicePairingProtectionLevel]::None)) ([Windows.Devices.Enumeration.DevicePairingResult])
    Write-Host "Pairing result: Status=$($pairResult.Status) ProtectionLevelUsed=$($pairResult.ProtectionLevelUsed)"
    if ($pairResult.Status -notin @('Paired', 'AlreadyPaired')) {
        [Console]::Error.WriteLine("Pairing failed: $($pairResult.Status)")
        exit 3
    }
}

if ($null -eq $ble) {
    Write-Host 'Opening BluetoothLEDevice from the known device ID...'
    $ble = Wait-WinRtOperation (
        [Windows.Devices.Bluetooth.BluetoothLEDevice]::FromIdAsync($target.Id)
    ) ([Windows.Devices.Bluetooth.BluetoothLEDevice])
}
if ($null -eq $ble) {
    [Console]::Error.WriteLine('Windows could not open BluetoothLEDevice from the device ID.')
    exit 4
}

# Constructing BluetoothLEDevice alone does not necessarily cause Windows to
# schedule a GATT connection. Enumerating services uncached asks the local BLE
# stack to contact the peripheral; HOGP may still reserve some services.
Write-Host 'Requesting uncached GATT service enumeration to trigger the local BLE connection...'
$serviceResult = Wait-WinRtOperation (
    $ble.GetGattServicesAsync([Windows.Devices.Bluetooth.BluetoothCacheMode]::Uncached)
) ([Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceServicesResult])
Write-Host "GATT service enumeration: Status=$($serviceResult.Status) ServiceCount=$($serviceResult.Services.Count)"

Start-Sleep -Seconds 2
Write-Host "Connection: Name=$($ble.Name) ConnectionStatus=$($ble.ConnectionStatus) BluetoothAddress=$(Format-BluetoothAddress $ble.BluetoothAddress)"
if ($ble.ConnectionStatus -ne 'Connected') {
    [Console]::Error.WriteLine('BluetoothLEDevice did not become Connected. Pairing may have completed, but Windows has not established a usable GATT link.')
    exit 4
}

Write-Host 'Command-line BLE connection is established. Keep this process alive to retain the BluetoothLEDevice reference. Press Ctrl+C to stop.'
$deadline = if ($DurationSeconds -gt 0) { [DateTime]::UtcNow.AddSeconds($DurationSeconds) } else { [DateTime]::MaxValue }
while ([DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Seconds 5
    Write-Host "ConnectionStatus=$($ble.ConnectionStatus)"
}

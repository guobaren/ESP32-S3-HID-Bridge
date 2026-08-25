using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

namespace HidBridge.Host.Drivers;

internal enum Ch341DeviceState
{
    NoDevice,
    Working,
    MissingDriver,
    OtherProblem,
}

internal sealed record Ch341DeviceInfo(
    string InstanceId,
    string FriendlyName,
    Ch341DeviceState State,
    int? ProblemCode,
    uint DevInst,
    string? Error);

internal sealed record Ch341ProbeResult(
    Ch341DeviceState State,
    Ch341DeviceInfo? Device,
    string? Error)
{
    internal static Ch341ProbeResult NoDevice() => new(Ch341DeviceState.NoDevice, null, null);
}

internal sealed record Ch341DriverPackage(string InfPath, string SetupPath)
{
    internal string RootPath => Path.GetDirectoryName(InfPath) ?? string.Empty;

    internal Ch341DriverPackage(string infPath)
        : this(infPath, Path.Combine(Path.GetDirectoryName(infPath) ?? string.Empty, "SETUP.EXE"))
    {
    }
}

internal enum Ch341StartupAction
{
    None,
    OfferInstall,
    ManualRecovery,
}

internal enum Ch341DriverStoreState
{
    Installed,
    Missing,
    ProbeFailed,
}

internal enum Ch341DriverPromptReason
{
    DeviceProblemCode28,
    DriverStoreMissing,
}

internal enum Ch341PostInstallAction
{
    WorkingCom,
    DriverPackageStagedNoDevice,
    VerificationFailed,
}

internal sealed record Ch341DriverStoreProbeResult(
    Ch341DriverStoreState State,
    string? MatchedOriginalName,
    string? Error)
{
    internal static Ch341DriverStoreProbeResult Installed(string originalName = "CH341SER.INF") =>
        new(Ch341DriverStoreState.Installed, originalName, null);

    internal static Ch341DriverStoreProbeResult Missing() =>
        new(Ch341DriverStoreState.Missing, null, null);

    internal static Ch341DriverStoreProbeResult ProbeFailed(string error) =>
        new(Ch341DriverStoreState.ProbeFailed, null, error);
}

internal enum Ch341DriverInstallState
{
    PackageCommandSucceeded,
    Cancelled,
    Failed,
}

internal sealed record Ch341DriverInstallResult(
    Ch341DriverInstallState State,
    int? ExitCode,
    string? Error)
{
    // 安装器 Exit 0/3010 只证明命令成功，不能证明设备已枚举或 COM 已可用。
    internal bool PackageCommandSucceeded => State == Ch341DriverInstallState.PackageCommandSucceeded;
}

internal static class Ch341DriverPackageLocator
{
    internal const string RelativeDriverRoot = @"drivers\wch-ch341ser";

    internal static Ch341DriverPackage? Locate(string applicationRoot)
    {
        string root = Path.GetFullPath(applicationRoot);
        string[] candidates =
        [
            Path.Combine(root, RelativeDriverRoot, "CH341SER", "CH341SER.INF"),
            Path.Combine(root, RelativeDriverRoot, "CH341SER_v4.0_2026-06-26", "CH341SER", "CH341SER.INF"),
        ];

        foreach (string infPath in candidates)
        {
            string setupPath = Path.Combine(Path.GetDirectoryName(infPath) ?? string.Empty, "SETUP.EXE");
            if (File.Exists(infPath) && File.Exists(setupPath))
            {
                return new Ch341DriverPackage(Path.GetFullPath(infPath), Path.GetFullPath(setupPath));
            }
        }

        return null;
    }
}

internal static class Ch341StartupPolicy
{
    internal static Ch341DeviceState ClassifyProblemCode(int? problemCode) => problemCode switch
    {
        0 => Ch341DeviceState.Working,
        28 => Ch341DeviceState.MissingDriver,
        _ => Ch341DeviceState.OtherProblem,
    };

    internal static Ch341StartupAction Decide(
        Ch341ProbeResult probe,
        bool packageAvailable,
        Ch341DriverStoreProbeResult? driverStore = null)
    {
        if (probe.State == Ch341DeviceState.OtherProblem)
        {
            return Ch341StartupAction.ManualRecovery;
        }

        // Driver Store 探测失败时不能把“未知”当作“缺失”，也不能冒险安装。
        // Code 28 的默认重载（未传入探测结果）仍保留历史策略，生产启动始终传入结果。
        if (driverStore?.State == Ch341DriverStoreState.ProbeFailed)
        {
            return Ch341StartupAction.None;
        }

        if (probe.State == Ch341DeviceState.Working)
        {
            return Ch341StartupAction.None;
        }

        if (!packageAvailable)
        {
            return Ch341StartupAction.None;
        }

        if (probe.State == Ch341DeviceState.MissingDriver)
        {
            return Ch341StartupAction.OfferInstall;
        }

        return driverStore?.State == Ch341DriverStoreState.Missing
            ? Ch341StartupAction.OfferInstall
            : Ch341StartupAction.None;
    }

    internal static Ch341DriverPromptReason? GetPromptReason(
        Ch341ProbeResult probe,
        Ch341DriverStoreProbeResult driverStore)
    {
        if (driverStore.State == Ch341DriverStoreState.ProbeFailed)
        {
            return null;
        }

        if (probe.State == Ch341DeviceState.MissingDriver)
        {
            return Ch341DriverPromptReason.DeviceProblemCode28;
        }

        if (probe.State == Ch341DeviceState.NoDevice &&
            driverStore.State == Ch341DriverStoreState.Missing)
        {
            return Ch341DriverPromptReason.DriverStoreMissing;
        }

        return null;
    }

    internal static Ch341PostInstallAction DecidePostInstall(
        Ch341ProbeResult probe,
        string? port,
        Ch341DriverStoreProbeResult driverStore) =>
        probe.State == Ch341DeviceState.Working && port is not null
            ? Ch341PostInstallAction.WorkingCom
            : probe.State == Ch341DeviceState.NoDevice &&
              driverStore.State == Ch341DriverStoreState.Installed
                ? Ch341PostInstallAction.DriverPackageStagedNoDevice
                : Ch341PostInstallAction.VerificationFailed;

    internal static string? TryGetComPort(Ch341DeviceInfo device, Func<string[]> getPortNames)
    {
        Match match = System.Text.RegularExpressions.Regex.Match(
            device.FriendlyName,
            @"\((COM\d+)\)",
            System.Text.RegularExpressions.RegexOptions.IgnoreCase | System.Text.RegularExpressions.RegexOptions.CultureInvariant);
        if (!match.Success)
        {
            return null;
        }

        string expected = match.Groups[1].Value;
        return getPortNames().FirstOrDefault(
            port => port.Equals(expected, StringComparison.OrdinalIgnoreCase));
    }
}

internal static class Ch341DriverStoreParser
{
    internal static Ch341DriverStoreProbeResult Parse(string output)
    {
        if (string.IsNullOrWhiteSpace(output))
        {
            return Ch341DriverStoreProbeResult.Missing();
        }

        List<List<string>> blocks = [];
        List<string> current = [];
        foreach (string line in output.Split(["\r\n", "\n", "\r"], StringSplitOptions.None))
        {
            string value = ExtractValue(line);
            bool startsNewDriver = Regex.IsMatch(
                value,
                @"^oem\d+\.inf$",
                RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
            if (startsNewDriver && current.Count > 0)
            {
                blocks.Add(current);
                current = [];
            }

            if (string.IsNullOrWhiteSpace(line))
            {
                if (current.Count > 0)
                {
                    blocks.Add(current);
                    current = [];
                }
                continue;
            }

            current.Add(value);
        }
        if (current.Count > 0)
        {
            blocks.Add(current);
        }

        foreach (List<string> block in blocks)
        {
            bool hasCh341Inf = block.Any(value =>
                value.Trim().Trim('"').Equals("CH341SER.INF", StringComparison.OrdinalIgnoreCase));
            bool hasQinhengProvider = block.Any(IsQinhengProvider);
            if (hasCh341Inf && hasQinhengProvider)
            {
                return Ch341DriverStoreProbeResult.Installed();
            }
        }

        return Ch341DriverStoreProbeResult.Missing();
    }

    private static bool IsQinhengProvider(string value) =>
        value.Contains("wch.cn", StringComparison.OrdinalIgnoreCase) ||
        value.Contains("Nanjing Qinheng", StringComparison.OrdinalIgnoreCase) ||
        value.Contains("Qinheng Microelectronics", StringComparison.OrdinalIgnoreCase);

    private static string ExtractValue(string line)
    {
        int asciiColon = line.IndexOf(':');
        int fullWidthColon = line.IndexOf('：');
        int separator = asciiColon < 0
            ? fullWidthColon
            : fullWidthColon < 0
                ? asciiColon
                : Math.Min(asciiColon, fullWidthColon);
        return (separator < 0 ? line : line[(separator + 1)..]).Trim();
    }
}

internal sealed class WindowsCh341DriverStoreProbe
{
    private const int TimeoutMilliseconds = 5000;

    internal Ch341DriverStoreProbeResult Probe()
    {
        if (!OperatingSystem.IsWindows())
        {
            return Ch341DriverStoreProbeResult.ProbeFailed("Driver Store 探测仅支持 Windows。");
        }

        ProcessStartInfo startInfo = new()
        {
            FileName = "pnputil.exe",
            Arguments = "/enum-drivers",
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };

        try
        {
            using Process process = Process.Start(startInfo)
                ?? throw new InvalidOperationException("无法启动 pnputil.exe。");
            Task<string> standardOutput = process.StandardOutput.ReadToEndAsync();
            Task<string> standardError = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit(TimeoutMilliseconds))
            {
                try
                {
                    process.Kill();
                }
                catch (Exception killException) when (killException is InvalidOperationException or Win32Exception)
                {
                    return Ch341DriverStoreProbeResult.ProbeFailed(
                        $"pnputil /enum-drivers 超时，且终止进程失败：{killException.Message}");
                }

                return Ch341DriverStoreProbeResult.ProbeFailed("pnputil /enum-drivers 超时。");
            }

            process.WaitForExit();
            string output = standardOutput.GetAwaiter().GetResult();
            string error = standardError.GetAwaiter().GetResult();
            if (process.ExitCode != 0)
            {
                return Ch341DriverStoreProbeResult.ProbeFailed(
                    $"pnputil /enum-drivers Exit {process.ExitCode}：{error.Trim()}");
            }

            return Ch341DriverStoreParser.Parse(output);
        }
        catch (Exception exception) when (exception is Win32Exception or InvalidOperationException or IOException)
        {
            return Ch341DriverStoreProbeResult.ProbeFailed(exception.Message);
        }
    }
}

internal static class Ch341DriverInstallerPolicy
{
    // WCH SETUP.EXE 内置帮助明确声明 /S 为隐式安装；同一安装器可用 /U 或界面 UNINSTALL 卸载。
    internal const string VendorInstallArguments = "/S";

    internal static Ch341DriverInstallState ClassifyExitCode(int exitCode) =>
        exitCode is 0 or 3010
            ? Ch341DriverInstallState.PackageCommandSucceeded
            : Ch341DriverInstallState.Failed;
}

internal sealed class WindowsCh341DeviceProbe
{
    private const uint DigcfPresent = 0x00000002;
    private const uint DigcfAllClasses = 0x00000004;
    private const uint SpdrpDevicedesc = 0x00000000;
    private const uint SpdrpFriendlyname = 0x0000000C;
    private const int ErrorInsufficientBuffer = 122;
    private const int CrSuccess = 0;
    private static readonly IntPtr InvalidHandleValue = new(-1);
    private const string TargetIdPrefix = "USB\\VID_1A86&PID_7523";

    internal Ch341ProbeResult Probe()
    {
        if (!OperatingSystem.IsWindows())
        {
            return new Ch341ProbeResult(
                Ch341DeviceState.OtherProblem,
                null,
                "CH340/CH341 PnP 检测仅支持 Windows。");
        }

        IntPtr deviceInfoSet = SetupDiGetClassDevs(
            IntPtr.Zero,
            null,
            IntPtr.Zero,
            DigcfPresent | DigcfAllClasses);
        if (deviceInfoSet == InvalidHandleValue)
        {
            return new Ch341ProbeResult(
                Ch341DeviceState.OtherProblem,
                null,
                new Win32Exception(Marshal.GetLastWin32Error()).Message);
        }

        try
        {
            List<Ch341ProbeResult> matches = [];
            uint index = 0;
            while (true)
            {
                SpDevinfoData data = new() { CbSize = Marshal.SizeOf<SpDevinfoData>() };
                if (!SetupDiEnumDeviceInfo(deviceInfoSet, index++, ref data))
                {
                    int error = Marshal.GetLastWin32Error();
                    if (error == 259)
                    {
                        break;
                    }

                    return new Ch341ProbeResult(
                        Ch341DeviceState.OtherProblem,
                        null,
                        new Win32Exception(error).Message);
                }

                string? instanceId = GetInstanceId(deviceInfoSet, ref data);
                if (string.IsNullOrWhiteSpace(instanceId) ||
                    !instanceId.StartsWith(TargetIdPrefix, StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                string friendlyName = GetDeviceProperty(deviceInfoSet, ref data, SpdrpFriendlyname)
                    ?? GetDeviceProperty(deviceInfoSet, ref data, SpdrpDevicedesc)
                    ?? instanceId;
                int? problemCode = null;
                string? errorText = null;
                int configResult = CM_Get_DevNode_Status(
                    out _,
                    out uint rawProblemCode,
                    data.DevInst,
                    0);
                if (configResult == CrSuccess)
                {
                    problemCode = unchecked((int)rawProblemCode);
                }
                else
                {
                    errorText = $"CM_Get_DevNode_Status 返回 0x{configResult:X8}";
                }

                Ch341DeviceState state = Ch341StartupPolicy.ClassifyProblemCode(problemCode);
                matches.Add(new Ch341ProbeResult(
                    state,
                    new Ch341DeviceInfo(
                        instanceId,
                        friendlyName,
                        state,
                        problemCode,
                        data.DevInst,
                        errorText),
                    errorText));
            }

            if (matches.Count == 0)
            {
                return Ch341ProbeResult.NoDevice();
            }

            return matches.FirstOrDefault(match => match.State == Ch341DeviceState.Working)
                ?? matches[0];
        }
        finally
        {
            _ = SetupDiDestroyDeviceInfoList(deviceInfoSet);
        }
    }

    internal static Ch341DeviceState ClassifyProblemCode(int problemCode) =>
        Ch341StartupPolicy.ClassifyProblemCode(problemCode);

    private static string? GetInstanceId(IntPtr deviceInfoSet, ref SpDevinfoData data)
    {
        StringBuilder buffer = new(256);
        if (SetupDiGetDeviceInstanceId(deviceInfoSet, ref data, buffer, buffer.Capacity, out int required))
        {
            return buffer.ToString();
        }

        if (Marshal.GetLastWin32Error() != ErrorInsufficientBuffer || required <= buffer.Capacity)
        {
            return null;
        }

        buffer = new StringBuilder(required + 1);
        return SetupDiGetDeviceInstanceId(deviceInfoSet, ref data, buffer, buffer.Capacity, out _)
            ? buffer.ToString()
            : null;
    }

    private static string? GetDeviceProperty(IntPtr deviceInfoSet, ref SpDevinfoData data, uint property)
    {
        byte[] buffer = new byte[512];
        if (!SetupDiGetDeviceRegistryProperty(
                deviceInfoSet,
                ref data,
                property,
                out _,
                buffer,
                buffer.Length,
                out int required))
        {
            if (Marshal.GetLastWin32Error() != ErrorInsufficientBuffer || required <= buffer.Length)
            {
                return null;
            }

            buffer = new byte[required];
            if (!SetupDiGetDeviceRegistryProperty(
                    deviceInfoSet,
                    ref data,
                    property,
                    out _,
                    buffer,
                    buffer.Length,
                    out _))
            {
                return null;
            }
        }

        return Encoding.Unicode.GetString(buffer).TrimEnd('\0');
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct SpDevinfoData
    {
        internal int CbSize;
        internal Guid ClassGuid;
        internal uint DevInst;
        internal IntPtr Reserved;
    }

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr SetupDiGetClassDevs(
        IntPtr classGuid,
        string? enumerator,
        IntPtr hwndParent,
        uint flags);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiEnumDeviceInfo(
        IntPtr deviceInfoSet,
        uint memberIndex,
        ref SpDevinfoData deviceInfoData);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiGetDeviceInstanceId(
        IntPtr deviceInfoSet,
        ref SpDevinfoData deviceInfoData,
        StringBuilder deviceInstanceId,
        int deviceInstanceIdSize,
        out int requiredSize);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiGetDeviceRegistryProperty(
        IntPtr deviceInfoSet,
        ref SpDevinfoData deviceInfoData,
        uint property,
        out uint propertyRegDataType,
        [Out] byte[] propertyBuffer,
        int propertyBufferSize,
        out int requiredSize);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceInfoSet);

    [DllImport("cfgmgr32.dll")]
    private static extern int CM_Get_DevNode_Status(
        out uint status,
        out uint problemNumber,
        uint devInst,
        uint flags);
}

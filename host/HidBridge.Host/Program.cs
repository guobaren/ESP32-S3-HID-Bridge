using System.IO.Ports;
using System.Net.Sockets;
using HidBridge.Host.Automation;
using HidBridge.Host.Drivers;
using HidBridge.Host.FirmwareUpdate;
using HidBridge.Host.Input;
using HidBridge.Host.RemoteInput;
using HidBridge.Host.Transport;
using HidBridge.Host.Ui;
using HidBridge.Protocol;

namespace HidBridge.Host;

internal static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        UiLogTextWriter logWriter = new();
        UiLogTextWriter automationLogWriter = new();
        Console.SetOut(logWriter);
        Console.SetError(logWriter);

        IBridgeTransport? transport = null;
        InputForwarder? input = null;
        AutomationController? automation = null;
        SimulationLogGenerator? simulation = null;
        RemoteInputServer? remoteInput = null;
        FirmwareFlashService? firmwareFlash = null;
        FirmwareUpdateApiServer? firmwareUpdateApi = null;
        try
        {
            BridgeOptions options = BridgeOptions.Load();
            RuntimeLogSettings logSettings = new(
                options.ShowDeviceLogInUi ? RuntimeLogMode.Full : RuntimeLogMode.Reduced);
            logWriter.EnableFile(options.HostLogPath, options.HostLogRetentionCount);
            automationLogWriter.EnableFile(options.AutomationLogPath, options.AutomationLogRetentionCount);
            SimulationOptions simulationOptions = SimulationOptions.Parse(args);
            if (!simulationOptions.Enabled &&
                options.Transport.Equals("serial", StringComparison.OrdinalIgnoreCase))
            {
                CheckCh341DriverAtStartup();
            }

            SerialBridge? serialBridge = null;
            if (simulationOptions.Enabled)
            {
                transport = new NoopBridgeTransport();
            }
            else if (options.Transport.Equals("wifi", StringComparison.OrdinalIgnoreCase))
            {
                // Wi-Fi 开发板输入实现暂时保留。BridgeOptions.Validate 当前会阻止该分支启用；
                // 后续完成真实链路验收后，只需开放统一功能开关，不需要恢复被删除的代码。
                transport = new NetworkBridge(options);
            }
            else
            {
                serialBridge = new SerialBridge(
                    options,
                    logSettings,
                    () => automation?.Settings.LegacySingleBoardFirmwareCompatibility ?? false);
                transport = serialBridge;
            }
            input = new InputForwarder(
                transport,
                () => automation?.Settings.LegacySingleBoardFirmwareCompatibility ?? false);
            AutomationProfileStore profileStore = new();
            automation = new AutomationController(profileStore, input);
            automation.Log += automationLogWriter.WriteLine;
            automation.DiagnosticLog += automationLogWriter.WriteLine;
            if (serialBridge is not null)
            {
                firmwareFlash = new FirmwareFlashService(options, serialBridge, input);
                firmwareUpdateApi = new FirmwareUpdateApiServer(
                    options.FirmwareUpdateApiPort,
                    firmwareFlash,
                    () => automation.Settings.FirmwareFlashPortName);
                if (automation.Settings.FirmwareUpdateApiEnabled)
                {
                    try
                    {
                        firmwareUpdateApi.SetEnabled(true);
                    }
                    catch (Exception exception) when (exception is SocketException or InvalidOperationException)
                    {
                        automation.Settings.FirmwareUpdateApiEnabled = false;
                        automation.SaveSettings();
                        Console.Error.WriteLine($"局域网固件刷写接口启动失败，已恢复为关闭：{exception.Message}");
                    }
                }
            }
            if (options.RemoteInputEnabled)
            {
                remoteInput = new RemoteInputServer(
                    options.RemoteInputBindAddress,
                    options.RemoteInputPort,
                    input);
            }

            string endpoint = simulationOptions.Enabled
                ? $"日志模拟 @ {simulationOptions.Rate} Hz"
                : options.Transport.Equals("wifi", StringComparison.OrdinalIgnoreCase)
                    ? $"Wi-Fi {options.WiFiHost}:{options.WiFiPort}"
                    : options.PortName.Equals("auto", StringComparison.OrdinalIgnoreCase) ||
                      string.IsNullOrWhiteSpace(options.PortName)
                        ? $"串口 @ {options.BaudRate}"
                        : $"串口 {options.PortName} @ {options.BaudRate}";

            using BridgeMainForm form = new(
                input,
                automation,
                endpoint,
                logSettings,
                firmwareUpdateApi,
                firmwareFlash,
                lanEndpointDescription: remoteInput?.DisplayEndpoint);
            logWriter.Attach(form.AppendLog);
            Console.WriteLine($"目标端点：{endpoint}");
            Console.WriteLine($"本地实时日志：{logWriter.FilePath}");
            Console.WriteLine($"Lua/宏本地诊断日志：{automationLogWriter.FilePath}");
            Console.WriteLine("HOME：开启/关闭同步；END：结束程序。");
            Console.WriteLine("同步开启后，除上述两个快捷键外的其他键鼠输入均只转发到对端。");
            if (simulationOptions.Enabled)
            {
                Console.WriteLine("模拟日志已开启；请向上滚动并复制历史行，观察新日志是否改变视图位置。");
                simulation = new SimulationLogGenerator(simulationOptions.Rate);
                simulation.Start();
            }

            input.Start();
            automation.Start();
            remoteInput?.Start();
            Application.Run(form);
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"主机程序启动或运行失败：{exception}");
            MessageBox.Show(
                exception.Message,
                "ESP32-S3 HID Bridge",
                MessageBoxButtons.OK,
                MessageBoxIcon.Error);
        }
        finally
        {
            remoteInput?.Dispose();
            firmwareUpdateApi?.Dispose();
            firmwareFlash?.Dispose();
            simulation?.Dispose();
            automation?.Dispose();
            automationLogWriter.Dispose();
            input?.Stop();
            input?.Dispose();
            transport?.Dispose();
            logWriter.Dispose();
        }
    }

    private static void CheckCh341DriverAtStartup()
    {
        Ch341ProbeResult probe = new WindowsCh341DeviceProbe().Probe();
        if (probe.State == Ch341DeviceState.Working)
        {
            Ch341DeviceInfo device = probe.Device!;
            string? port = TryGetSerialPort(device);
            if (port is null)
            {
                Console.WriteLine(
                    $"检测到 CH340/CH341：{device.FriendlyName}；PnP 状态正常，当前尚未匹配到 SerialPort；" +
                    "主程序将继续启动并由串口握手自动发现，不显示可能过时的阻塞告警。");
            }
            else
            {
                Console.WriteLine($"检测到 CH340/CH341：{device.FriendlyName}；PnP 状态正常，串口={port}。");
            }

            return;
        }

        if (probe.State == Ch341DeviceState.OtherProblem)
        {
            Console.Error.WriteLine(
                $"检测 CH340/CH341 设备时遇到问题：{probe.Error ?? "未知错误"}；不会自动覆盖现有驱动。");
            return;
        }

        Ch341DriverPackage? package = Ch341DriverPackageLocator.Locate(AppContext.BaseDirectory);
        Ch341DriverStoreProbeResult driverStore = new WindowsCh341DriverStoreProbe().Probe();
        if (driverStore.State == Ch341DriverStoreState.ProbeFailed)
        {
            Console.Error.WriteLine(
                $"只读检查 CH341 Driver Store 失败：{driverStore.Error ?? "未知错误"}；不会自动安装。");
        }

        Ch341StartupAction action = Ch341StartupPolicy.Decide(probe, package is not null, driverStore);
        if (action != Ch341StartupAction.OfferInstall || package is null)
        {
            if (probe.State == Ch341DeviceState.NoDevice)
            {
                Console.WriteLine(
                    driverStore.State == Ch341DriverStoreState.Installed
                        ? "启动瞬间未发现 CH340/CH341 设备；Driver Store 已有 CH341SER.INF。" +
                          "主程序将继续启动并由串口握手自动发现，不要求立即重插 USB。"
                        : "当前未发现已插入的 CH340/CH341 设备，不提示驱动安装。" +
                          (package is null ? "随程序附带的驱动包也不存在。" : string.Empty));
                return;
            }

            Ch341DeviceInfo deviceInfo = probe.Device!;
            Console.Error.WriteLine(
                $"检测到 CH340/CH341：{deviceInfo.FriendlyName}；Windows Problem Code=" +
                $"{deviceInfo.ProblemCode?.ToString() ?? "未知"}。请在设备管理器中手动处理，不会自动覆盖现有驱动。" +
                (package is null
                    ? $"随程序附带的驱动包不存在，预期路径：{Path.Combine(AppContext.BaseDirectory, Ch341DriverPackageLocator.RelativeDriverRoot)}"
                    : string.Empty));
            return;
        }

        Ch341DriverPromptReason reason = Ch341StartupPolicy.GetPromptReason(probe, driverStore)
            ?? throw new InvalidOperationException("驱动安装提示缺少明确原因。");
        Ch341DriverInstallResult? result = DriverInstallPrompt.ShowAndInstall(package, reason);
        if (result is null)
        {
            Console.WriteLine("用户选择稍后处理驱动，主程序将继续启动。");
            return;
        }

        if (!result.PackageCommandSucceeded)
        {
            Console.Error.WriteLine(
                $"驱动安装未完成：状态={result.State}，退出码={result.ExitCode?.ToString() ?? "未知"}，" +
                $"原因={result.Error ?? "未知"}；主程序将继续启动。");
            return;
        }

        Ch341ProbeResult afterInstall = WaitForWorkingCh341Device();
        string? portAfterInstall = afterInstall.Device is null
            ? null
            : TryGetSerialPort(afterInstall.Device);
        Ch341DriverStoreProbeResult afterDriverStore = new WindowsCh341DriverStoreProbe().Probe();
        Ch341PostInstallAction postInstallAction = Ch341StartupPolicy.DecidePostInstall(
            afterInstall,
            portAfterInstall,
            afterDriverStore);
        if (postInstallAction == Ch341PostInstallAction.WorkingCom)
        {
            Console.WriteLine(
                $"驱动安装后复检通过：Problem Code=0，SerialPort={portAfterInstall}，设备={afterInstall.Device!.FriendlyName}。");
        }
        else if (postInstallAction == Ch341PostInstallAction.DriverPackageStagedNoDevice)
        {
            Console.WriteLine(
                "驱动包已放入 Driver Store；安装后瞬时复检尚未发现设备节点或 COM 口。" +
                "主程序将继续启动并由串口握手自动发现，不显示可能过时的重插 USB 告警。");
        }
        else
        {
            Console.Error.WriteLine(
                $"驱动安装命令已完成，但复检未确认 Problem Code=0 和 SerialPort COM；" +
                $"状态={afterInstall.State}，问题码={afterInstall.Device?.ProblemCode?.ToString() ?? "未知"}，" +
                $"Driver Store={afterDriverStore.State}，原因={afterDriverStore.Error ?? "未知"}。主程序将继续启动。");
        }
    }

    private static Ch341ProbeResult WaitForWorkingCh341Device()
    {
        Ch341ProbeResult latest = Ch341ProbeResult.NoDevice();
        for (int attempt = 0; attempt < 6; attempt++)
        {
            latest = new WindowsCh341DeviceProbe().Probe();
            if (latest.State == Ch341DeviceState.Working &&
                latest.Device is not null &&
                TryGetSerialPort(latest.Device) is not null)
            {
                return latest;
            }

            if (attempt < 5)
            {
                Thread.Sleep(500);
            }
        }

        return latest;
    }

    private static string? TryGetSerialPort(Ch341DeviceInfo device)
    {
        try
        {
            return Ch341StartupPolicy.TryGetComPort(device, SerialPort.GetPortNames);
        }
        catch (Exception exception) when (exception is IOException or InvalidOperationException)
        {
            Console.Error.WriteLine($"读取 SerialPort 列表失败：{exception.Message}");
            return null;
        }
    }
}


internal sealed class NoopBridgeTransport : IBridgeTransport
{
    public void Send(MessageType type, ReadOnlySpan<byte> payload)
    {
    }

    public void Dispose()
    {
    }
}

internal sealed class SimulationOptions
{
    internal bool Enabled { get; private init; }
    internal int Rate { get; private init; } = 10;

    internal static SimulationOptions Parse(string[] args)
    {
        bool enabled = args.Any(argument => argument.Equals("--simulate-log", StringComparison.OrdinalIgnoreCase));
        int rate = 10;
        for (int index = 0; index < args.Length; index++)
        {
            string argument = args[index];
            const string prefix = "--simulate-log-rate=";
            if (argument.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            {
                _ = int.TryParse(argument[prefix.Length..], out rate);
            }
            else if (argument.Equals("--simulate-log-rate", StringComparison.OrdinalIgnoreCase) &&
                     index + 1 < args.Length)
            {
                _ = int.TryParse(args[++index], out rate);
            }
        }

        return new SimulationOptions
        {
            Enabled = enabled,
            Rate = Math.Clamp(rate, 1, 100),
        };
    }
}

internal sealed class SimulationLogGenerator : IDisposable
{
    private readonly int _periodMilliseconds;
    private System.Threading.Timer? _timer;
    private int _lineNumber;

    internal SimulationLogGenerator(int rate)
    {
        _periodMilliseconds = Math.Max(1, 1000 / Math.Max(1, rate));
    }

    internal void Start()
    {
        _timer ??= new System.Threading.Timer(
            _ => Console.WriteLine($"[SIM] line={Interlocked.Increment(ref _lineNumber):000000} timestamp={DateTimeOffset.Now:O}"),
            null,
            TimeSpan.Zero,
            TimeSpan.FromMilliseconds(_periodMilliseconds));
    }

    public void Dispose()
    {
        _timer?.Dispose();
        _timer = null;
    }
}

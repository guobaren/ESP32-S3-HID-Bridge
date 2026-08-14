using System.Net.Sockets;
using HidBridge.Host.Automation;
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
            logWriter.EnableFile(options.HostLogPath);
            automationLogWriter.EnableFile(Path.Combine("artifacts", "automation-runtime-{timestamp}.log"));
            SimulationOptions simulationOptions = SimulationOptions.Parse(args);
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
                serialBridge = new SerialBridge(options, logSettings);
                transport = serialBridge;
            }
            input = new InputForwarder(transport);
            AutomationProfileStore profileStore = new();
            if (profileStore.TryImportLegacyMouseHubProfiles(@"F:\Mouse hub\profiles"))
            {
                Console.WriteLine("已从 F:\\Mouse hub\\profiles 导入宏、Lua 和配置名称；其他 Mouse hub 设置未启用。");
            }
            automation = new AutomationController(profileStore, input);
            automation.Log += automationLogWriter.WriteLine;
            automation.DiagnosticLog += automationLogWriter.WriteLine;
            if (serialBridge is not null)
            {
                firmwareFlash = new FirmwareFlashService(options, serialBridge, input);
                firmwareUpdateApi = new FirmwareUpdateApiServer(options.FirmwareUpdateApiPort, firmwareFlash);
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
                        Console.Error.WriteLine($"本机固件刷写接口启动失败，已恢复为关闭：{exception.Message}");
                    }
                }
            }
            if (options.RemoteInputEnabled)
            {
                remoteInput = new RemoteInputServer(
                    options.RemoteInputBindAddress,
                    options.RemoteInputPort,
                    command => input.TryInjectMouseMovement(
                        command.DeltaX,
                        command.DeltaY,
                        command.Wheel,
                        command.Pan));
            }

            string endpoint = simulationOptions.Enabled
                ? $"日志模拟 @ {simulationOptions.Rate} Hz"
                : options.Transport.Equals("wifi", StringComparison.OrdinalIgnoreCase)
                    ? $"Wi-Fi {options.WiFiHost}:{options.WiFiPort}"
                    : options.PortName.Equals("auto", StringComparison.OrdinalIgnoreCase) ||
                      string.IsNullOrWhiteSpace(options.PortName)
                        ? $"串口自动发现 @ {options.BaudRate}"
                        : $"串口 {options.PortName} @ {options.BaudRate}";

            using BridgeMainForm form = new(input, automation, endpoint, logSettings, firmwareUpdateApi);
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

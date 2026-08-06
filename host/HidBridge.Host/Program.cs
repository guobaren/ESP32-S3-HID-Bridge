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
        Console.SetOut(logWriter);
        Console.SetError(logWriter);

        IBridgeTransport? transport = null;
        InputForwarder? input = null;
        SimulationLogGenerator? simulation = null;
        RemoteInputServer? remoteInput = null;
        try
        {
            BridgeOptions options = BridgeOptions.Load();
            RuntimeLogSettings logSettings = new(
                options.ShowDeviceLogInUi ? RuntimeLogMode.Full : RuntimeLogMode.Reduced);
            logWriter.EnableFile(options.HostLogPath);
            SimulationOptions simulationOptions = SimulationOptions.Parse(args);
            transport = simulationOptions.Enabled
                ? new NoopBridgeTransport()
                : options.Transport.Equals("wifi", StringComparison.OrdinalIgnoreCase)
                    ? new NetworkBridge(options)
                    : new SerialBridge(options, logSettings);
            input = new InputForwarder(transport);
            if (options.RemoteInputEnabled)
            {
                remoteInput = new RemoteInputServer(
                    options.RemoteInputBindAddress,
                    options.RemoteInputPort,
                    options.RemoteInputPresharedKey,
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

            using BridgeMainForm form = new(input, endpoint, logSettings);
            logWriter.Attach(form.AppendLog);
            Console.WriteLine($"目标端点：{endpoint}");
            Console.WriteLine($"本地实时日志：{logWriter.FilePath}");
            Console.WriteLine("HOME：开启/关闭同步；END：结束程序。");
            Console.WriteLine("同步开启后，除上述两个快捷键外的其他键鼠输入均只转发到对端。");
            if (simulationOptions.Enabled)
            {
                Console.WriteLine("模拟日志已开启；请向上滚动并复制历史行，观察新日志是否改变视图位置。");
                simulation = new SimulationLogGenerator(simulationOptions.Rate);
                simulation.Start();
            }

            input.Start();
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
            simulation?.Dispose();
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

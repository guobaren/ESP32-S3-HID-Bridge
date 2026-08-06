using System.Text.Json;

namespace HidBridge.Host;

internal sealed class BridgeOptions
{
    // 电脑通过 Wi-Fi 连接开发板的实现暂时保留，但当前阶段不开放运行入口。
    // 后续完成专门的真实链路验收后，可将此开关改为 true 恢复该输入通道。
    internal const bool WiFiBoardTransportEnabled = false;

    public string Transport { get; init; } = "serial";
    public string PortName { get; init; } = "auto";
    public int BaudRate { get; init; } = 921600;
    public string WiFiHost { get; init; } = "192.168.1.50";
    public int WiFiPort { get; init; } = 24813;
    public string NetworkPresharedKey { get; init; } = string.Empty;
    public int ReconnectDelayMilliseconds { get; init; } = 1000;
    public int HeartbeatIntervalMilliseconds { get; init; } = 500;
    public string DeviceLogPath { get; init; } = "artifacts/host-serial-{timestamp}.log";
    public bool ShowDeviceLogInUi { get; init; }
    public string HostLogPath { get; init; } = "artifacts/host-runtime-{timestamp}.log";
    public bool RemoteInputEnabled { get; init; }
    public string RemoteInputBindAddress { get; init; } = "0.0.0.0";
    public int RemoteInputPort { get; init; } = 24814;
    public string RemoteInputPresharedKey { get; init; } = string.Empty;

    public static BridgeOptions Load()
    {
        string localPath = Path.Combine(AppContext.BaseDirectory, "bridge.local.json");
        string defaultPath = Path.Combine(AppContext.BaseDirectory, "bridge.json");
        string path = File.Exists(localPath) ? localPath : defaultPath;

        if (!File.Exists(path))
        {
            return new BridgeOptions();
        }

        BridgeOptions options = JsonSerializer.Deserialize<BridgeOptions>(
                   File.ReadAllText(path),
                   new JsonSerializerOptions { PropertyNameCaseInsensitive = true })
               ?? new BridgeOptions();

        Validate(options);
        return options;
    }

    internal static void Validate(BridgeOptions options)
    {
        if (!options.Transport.Equals("serial", StringComparison.OrdinalIgnoreCase) &&
            !options.Transport.Equals("wifi", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("transport 只能是 serial 或 wifi。");
        }
        if (options.Transport.Equals("wifi", StringComparison.OrdinalIgnoreCase) &&
            !WiFiBoardTransportEnabled)
        {
            throw new InvalidDataException(
                "电脑通过 Wi-Fi 连接开发板的功能当前暂未启用；实现代码已保留，请使用 serial。");
        }
        if (options.HeartbeatIntervalMilliseconds < 100)
        {
            throw new InvalidDataException("heartbeatIntervalMilliseconds 不能小于 100。");
        }
        if (options.Transport.Equals("wifi", StringComparison.OrdinalIgnoreCase) &&
            System.Text.Encoding.UTF8.GetByteCount(options.NetworkPresharedKey) < 16)
        {
            throw new InvalidDataException("使用 Wi-Fi 时，networkPresharedKey 至少需要 16 个 UTF-8 字节。");
        }
        if (options.RemoteInputPort is < 1 or > 65535)
        {
            throw new InvalidDataException("remoteInputPort 必须在 1..65535 范围内。");
        }
        if (options.RemoteInputEnabled &&
            System.Text.Encoding.UTF8.GetByteCount(options.RemoteInputPresharedKey) < 16)
        {
            throw new InvalidDataException(
                "启用局域网模拟输入时，remoteInputPresharedKey 至少需要 16 个 UTF-8 字节。");
        }
    }
}

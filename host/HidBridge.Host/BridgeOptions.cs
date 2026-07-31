using System.Text.Json;

namespace HidBridge.Host;

internal sealed class BridgeOptions
{
    public string Transport { get; init; } = "serial";
    public string PortName { get; init; } = "COM5";
    public int BaudRate { get; init; } = 921600;
    public string WiFiHost { get; init; } = "192.168.1.50";
    public int WiFiPort { get; init; } = 24813;
    public string NetworkPresharedKey { get; init; } = string.Empty;
    public bool SuppressLocalInput { get; init; }
    public int ReconnectDelayMilliseconds { get; init; } = 1000;
    public int HeartbeatIntervalMilliseconds { get; init; } = 500;

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

        if (!options.Transport.Equals("serial", StringComparison.OrdinalIgnoreCase) &&
            !options.Transport.Equals("wifi", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("transport 只能是 serial 或 wifi。");
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

        return options;
    }
}

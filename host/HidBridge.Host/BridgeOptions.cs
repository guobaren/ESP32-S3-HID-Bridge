using System.Text.Json;

namespace HidBridge.Host;

internal sealed class BridgeOptions
{
    public string PortName { get; init; } = "COM5";
    public int BaudRate { get; init; } = 921600;
    public bool SuppressLocalInput { get; init; }
    public int ReconnectDelayMilliseconds { get; init; } = 1000;

    public static BridgeOptions Load()
    {
        string localPath = Path.Combine(AppContext.BaseDirectory, "bridge.local.json");
        string defaultPath = Path.Combine(AppContext.BaseDirectory, "bridge.json");
        string path = File.Exists(localPath) ? localPath : defaultPath;

        if (!File.Exists(path))
        {
            return new BridgeOptions();
        }

        return JsonSerializer.Deserialize<BridgeOptions>(
                   File.ReadAllText(path),
                   new JsonSerializerOptions { PropertyNameCaseInsensitive = true })
               ?? new BridgeOptions();
    }
}

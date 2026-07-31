using System.Net;
using System.Text;
using System.Text.Json;

namespace HidBridge.TargetAgent;

internal sealed class AgentOptions
{
    public string ListenAddress { get; init; } = "0.0.0.0";
    public int Port { get; init; } = 24814;
    public string PresharedKey { get; init; } = string.Empty;
    public int LeaseTimeoutMilliseconds { get; init; } = 1500;

    internal IPAddress GetListenAddress()
    {
        if (!IPAddress.TryParse(ListenAddress, out IPAddress? address))
        {
            throw new InvalidDataException("listenAddress 必须是有效的 IP 地址。");
        }
        return address;
    }

    internal static AgentOptions Load()
    {
        string localPath = Path.Combine(AppContext.BaseDirectory, "agent.local.json");
        string defaultPath = Path.Combine(AppContext.BaseDirectory, "agent.json");
        string path = File.Exists(localPath) ? localPath : defaultPath;
        AgentOptions options = File.Exists(path)
            ? JsonSerializer.Deserialize<AgentOptions>(
                  File.ReadAllText(path),
                  new JsonSerializerOptions { PropertyNameCaseInsensitive = true })
              ?? new AgentOptions()
            : new AgentOptions();

        if (options.Port is <= 0 or > 65535)
        {
            throw new InvalidDataException("port 必须位于 1..65535。");
        }
        if (options.LeaseTimeoutMilliseconds < 500)
        {
            throw new InvalidDataException("leaseTimeoutMilliseconds 不能小于 500。");
        }
        if (Encoding.UTF8.GetByteCount(options.PresharedKey) < 16)
        {
            throw new InvalidDataException("presharedKey 至少需要 16 个 UTF-8 字节。");
        }
        return options;
    }
}

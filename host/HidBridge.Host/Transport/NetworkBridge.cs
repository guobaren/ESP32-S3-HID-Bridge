using System.Net.Sockets;
using System.Security.Cryptography;
using HidBridge.Protocol;

namespace HidBridge.Host.Transport;

internal sealed class NetworkBridge : IBridgeTransport
{
    private readonly BridgeOptions _options;
    private readonly FrameCodec _codec = new();
    private readonly object _sync = new();
    private readonly System.Threading.Timer _heartbeatTimer;
    private TcpClient? _client;
    private SecurePacketChannel? _channel;
    private DateTime _nextConnectAttemptUtc;
    private bool _sessionStarted;
    private bool _disposed;

    internal NetworkBridge(BridgeOptions options)
    {
        _options = options;
        _heartbeatTimer = new System.Threading.Timer(
            _ => Send(MessageType.Ping, ReadOnlySpan<byte>.Empty),
            null,
            TimeSpan.Zero,
            TimeSpan.FromMilliseconds(options.HeartbeatIntervalMilliseconds));
    }

    public void Send(MessageType type, ReadOnlySpan<byte> payload)
    {
        lock (_sync)
        {
            if (_disposed || !EnsureConnected())
            {
                return;
            }

            try
            {
                if (!_sessionStarted)
                {
                    _channel!.Send(_codec.Encode(MessageType.SessionStart, ReadOnlySpan<byte>.Empty));
                    _sessionStarted = true;
                }
                _channel!.Send(_codec.Encode(type, payload));
            }
            catch (Exception exception) when (
                exception is IOException or SocketException or CryptographicException or ObjectDisposedException)
            {
                Console.Error.WriteLine($"Wi-Fi 通道写入失败：{exception.Message}");
                CloseConnection();
            }
        }
    }

    private bool EnsureConnected()
    {
        if (_client?.Connected == true && _channel is not null)
        {
            return true;
        }
        if (DateTime.UtcNow < _nextConnectAttemptUtc)
        {
            return false;
        }

        _nextConnectAttemptUtc = DateTime.UtcNow.AddMilliseconds(
            Math.Max(100, _options.ReconnectDelayMilliseconds));
        try
        {
            TcpClient client = new();
            Task connectTask = client.ConnectAsync(_options.WiFiHost, _options.WiFiPort);
            if (!connectTask.Wait(TimeSpan.FromSeconds(3)))
            {
                client.Dispose();
                throw new TimeoutException("连接开发板超时。");
            }

            client.NoDelay = true;
            client.ReceiveTimeout = 3000;
            client.SendTimeout = 3000;
            SecurePacketChannel channel = SecurePacketChannel.AuthenticateClient(
                client.GetStream(),
                _options.NetworkPresharedKey);
            _client = client;
            _channel = channel;
            _sessionStarted = false;
            Console.WriteLine($"已安全连接开发板 {_options.WiFiHost}:{_options.WiFiPort}。");
            return true;
        }
        catch (Exception exception) when (
            exception is IOException or SocketException or CryptographicException or TimeoutException or AggregateException)
        {
            Console.Error.WriteLine($"暂时无法连接开发板 Wi-Fi 通道：{exception.Message}");
            CloseConnection();
            return false;
        }
    }

    private void CloseConnection()
    {
        try
        {
            _channel?.Dispose();
            _client?.Dispose();
        }
        catch
        {
            // 连接已经失效时继续完成本地状态清理。
        }
        finally
        {
            _channel = null;
            _client = null;
            _sessionStarted = false;
        }
    }

    public void Dispose()
    {
        _heartbeatTimer.Dispose();
        lock (_sync)
        {
            _disposed = true;
            CloseConnection();
        }
    }
}

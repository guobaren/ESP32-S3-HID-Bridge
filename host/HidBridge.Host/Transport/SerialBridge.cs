using System.IO.Ports;
using HidBridge.Protocol;

namespace HidBridge.Host.Transport;

internal sealed class SerialBridge : IBridgeTransport
{
    private readonly BridgeOptions _options;
    private readonly FrameCodec _codec = new();
    private readonly object _sync = new();
    private readonly System.Threading.Timer _heartbeatTimer;
    private SerialPort? _port;
    private DateTime _nextConnectAttemptUtc;
    private bool _sessionStarted;
    private bool _disposed;

    public SerialBridge(BridgeOptions options)
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
                    WriteFrame(_codec.Encode(MessageType.SessionStart, ReadOnlySpan<byte>.Empty));
                    _sessionStarted = true;
                }
                WriteFrame(_codec.Encode(type, payload));
            }
            catch (Exception exception) when (
                exception is IOException or InvalidOperationException or UnauthorizedAccessException)
            {
                Console.Error.WriteLine($"串口写入失败：{exception.Message}");
                ClosePort();
            }
        }
    }

    private void WriteFrame(byte[] frame) => _port!.Write(frame, 0, frame.Length);

    private bool EnsureConnected()
    {
        if (_port?.IsOpen == true)
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
            _port = new SerialPort(_options.PortName, _options.BaudRate, Parity.None, 8, StopBits.One)
            {
                Handshake = Handshake.None,
                DtrEnable = false,
                RtsEnable = false,
                WriteTimeout = 250,
            };
            _port.Open();
            Console.WriteLine($"已连接 {_options.PortName}。");
            return true;
        }
        catch (Exception exception) when (
            exception is IOException or InvalidOperationException or UnauthorizedAccessException)
        {
            Console.Error.WriteLine($"暂时无法连接 {_options.PortName}：{exception.Message}");
            ClosePort();
            return false;
        }
    }

    private void ClosePort()
    {
        try
        {
            _port?.Dispose();
        }
        catch
        {
            // 端口已失效时，释放失败不应阻止后续重连。
        }
        finally
        {
            _port = null;
            _sessionStarted = false;
        }
    }

    public void Dispose()
    {
        _heartbeatTimer.Dispose();
        lock (_sync)
        {
            _disposed = true;
            ClosePort();
        }
    }
}

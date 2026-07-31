using System.IO.Ports;
using HidBridge.Host.Protocol;

namespace HidBridge.Host.Transport;

internal sealed class SerialBridge : IDisposable
{
    private readonly BridgeOptions _options;
    private readonly FrameCodec _codec = new();
    private readonly object _sync = new();
    private SerialPort? _port;
    private DateTime _nextConnectAttemptUtc;

    public SerialBridge(BridgeOptions options)
    {
        _options = options;
    }

    public void Send(MessageType type, ReadOnlySpan<byte> payload)
    {
        byte[] frame = _codec.Encode(type, payload);

        lock (_sync)
        {
            if (!EnsureConnected())
            {
                return;
            }

            try
            {
                _port!.Write(frame, 0, frame.Length);
            }
            catch (Exception exception) when (
                exception is IOException or InvalidOperationException or UnauthorizedAccessException)
            {
                Console.Error.WriteLine($"串口写入失败：{exception.Message}");
                ClosePort();
            }
        }
    }

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
        }
    }

    public void Dispose()
    {
        lock (_sync)
        {
            ClosePort();
        }
    }
}

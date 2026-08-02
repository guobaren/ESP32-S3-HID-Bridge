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

        bool automatic = IsAutomaticPort(_options.PortName);
        string[] candidates = automatic
            ? SerialPort.GetPortNames()
                .OrderBy(GetPortNumber)
                .ThenBy(name => name, StringComparer.OrdinalIgnoreCase)
                .ToArray()
            : [_options.PortName];
        if (candidates.Length == 0)
        {
            Console.Error.WriteLine("暂未发现可用串口，等待设备连接。");
            return false;
        }

        if (automatic)
        {
            Console.WriteLine($"正在探测串口：{string.Join("、", candidates)}");
        }

        foreach (string portName in candidates)
        {
            SerialPort? candidate = null;
            try
            {
                candidate = CreatePort(portName);
                candidate.Open();
                if (automatic && !SerialDeviceProbe.Probe(candidate, _codec))
                {
                    Console.WriteLine($"{portName} 未返回 HID Bridge 握手，已忽略。");
                    candidate.Dispose();
                    continue;
                }

                _port = candidate;
                Console.WriteLine(automatic
                    ? $"已自动发现并连接 {portName}。"
                    : $"已连接 {portName}。");
                return true;
            }
            catch (Exception exception) when (
                exception is IOException or InvalidOperationException or UnauthorizedAccessException or
                    TimeoutException or ArgumentException)
            {
                Console.Error.WriteLine($"暂时无法使用 {portName}：{exception.Message}");
                candidate?.Dispose();
            }
        }

        return false;
    }

    private SerialPort CreatePort(string portName) =>
        new(portName, _options.BaudRate, Parity.None, 8, StopBits.One)
        {
            Handshake = Handshake.None,
            DtrEnable = false,
            RtsEnable = false,
            ReadTimeout = 100,
            WriteTimeout = 250,
        };

    private static bool IsAutomaticPort(string? portName) =>
        string.IsNullOrWhiteSpace(portName) || portName.Equals("auto", StringComparison.OrdinalIgnoreCase);

    private static int GetPortNumber(string portName) =>
        portName.StartsWith("COM", StringComparison.OrdinalIgnoreCase) &&
        int.TryParse(portName.AsSpan(3), out int number)
            ? number
            : int.MaxValue;

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

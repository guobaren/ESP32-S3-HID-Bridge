using System.Net;
using System.Net.Sockets;
using System.Text.Json;

namespace HidBridge.Host.RemoteInput;

internal readonly record struct RemoteMouseCommand(int DeltaX, int DeltaY, int Wheel, int Pan);

internal sealed class RemoteInputServer : IDisposable
{
    private const int MaximumDatagramBytes = 4096;
    private readonly UdpClient _udp;
    private readonly Func<RemoteMouseCommand, bool> _commandHandler;
    private readonly CancellationTokenSource _cancellation = new();
    private Task? _receiveTask;
    private long _received;
    private long _accepted;
    private long _rejected;
    private DateTime _lastStatisticsUtc = DateTime.UtcNow;
    private bool _disposed;

    internal RemoteInputServer(
        string bindAddress,
        int port,
        Func<RemoteMouseCommand, bool> commandHandler)
    {
        IPAddress address = ResolveBindAddress(bindAddress);
        _udp = new UdpClient(new IPEndPoint(address, port));
        _commandHandler = commandHandler;
    }

    internal int Port => ((IPEndPoint)_udp.Client.LocalEndPoint!).Port;

    internal void Start()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_receiveTask is not null)
        {
            return;
        }

        Console.WriteLine(
            $"局域网模拟输入已监听 UDP {GetLocalEndpoint()}；" +
            "仅在 HOME 同步开启时转发，是否启用固定 20 ms 低延迟平滑由主界面开关决定。");
        _receiveTask = Task.Run(ReceiveLoopAsync);
    }

    private async Task ReceiveLoopAsync()
    {
        while (!_cancellation.IsCancellationRequested)
        {
            try
            {
                UdpReceiveResult result = await _udp.ReceiveAsync(_cancellation.Token).ConfigureAwait(false);
                Interlocked.Increment(ref _received);
                if (result.Buffer.Length > MaximumDatagramBytes)
                {
                    Interlocked.Increment(ref _rejected);
                    Console.Error.WriteLine($"拒绝过大的模拟输入数据报：{result.Buffer.Length} 字节，来源 {result.RemoteEndPoint}。");
                    continue;
                }

                if (!RemoteMouseCommandParser.TryParse(
                        result.Buffer,
                        out RemoteMouseCommand command,
                        out string error))
                {
                    Interlocked.Increment(ref _rejected);
                    Console.Error.WriteLine($"拒绝模拟输入命令（{result.RemoteEndPoint}）：{error}");
                    continue;
                }

                if (_commandHandler(command))
                {
                    Interlocked.Increment(ref _accepted);
                }
                else
                {
                    Interlocked.Increment(ref _rejected);
                }

                LogStatisticsIfDue(result.RemoteEndPoint);
            }
            catch (OperationCanceledException) when (_cancellation.IsCancellationRequested)
            {
                return;
            }
            catch (ObjectDisposedException) when (_cancellation.IsCancellationRequested)
            {
                return;
            }
            catch (SocketException exception) when (_cancellation.IsCancellationRequested)
            {
                Console.WriteLine($"局域网模拟输入监听已停止：{exception.SocketErrorCode}。");
                return;
            }
            catch (Exception exception)
            {
                Interlocked.Increment(ref _rejected);
                Console.Error.WriteLine($"处理局域网模拟输入失败：{exception.Message}");
            }
        }
    }

    private void LogStatisticsIfDue(IPEndPoint remoteEndPoint)
    {
        DateTime now = DateTime.UtcNow;
        if (now - _lastStatisticsUtc < TimeSpan.FromSeconds(1))
        {
            return;
        }

        _lastStatisticsUtc = now;
        Console.WriteLine(
            $"局域网模拟输入统计：接收={Interlocked.Read(ref _received)}，" +
            $"接受={Interlocked.Read(ref _accepted)}，拒绝={Interlocked.Read(ref _rejected)}，" +
            $"最近来源={remoteEndPoint}。");
    }

    private string GetLocalEndpoint() => _udp.Client.LocalEndPoint?.ToString() ?? $"?:{Port}";

    private static IPAddress ResolveBindAddress(string value)
    {
        if (string.IsNullOrWhiteSpace(value) ||
            value.Equals("any", StringComparison.OrdinalIgnoreCase) ||
            value == "*")
        {
            return IPAddress.Any;
        }
        if (value.Equals("localhost", StringComparison.OrdinalIgnoreCase))
        {
            return IPAddress.Loopback;
        }
        if (!IPAddress.TryParse(value, out IPAddress? address))
        {
            throw new InvalidDataException($"remoteInputBindAddress 不是有效 IP 地址：{value}");
        }
        return address;
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _cancellation.Cancel();
        _udp.Dispose();
        try
        {
            _receiveTask?.Wait(TimeSpan.FromSeconds(2));
        }
        catch (AggregateException exception) when (
            exception.InnerExceptions.All(inner => inner is OperationCanceledException or ObjectDisposedException))
        {
            // 正常取消。
        }
        _cancellation.Dispose();
    }
}

internal static class RemoteMouseCommandParser
{
    private sealed class CommandDto
    {
        public int Dx { get; init; }
        public int Dy { get; init; }
        public int Wheel { get; init; }
        public int Pan { get; init; }
    }

    internal static bool TryParse(
        ReadOnlySpan<byte> payload,
        out RemoteMouseCommand command,
        out string error)
    {
        command = default;
        error = string.Empty;
        try
        {
            CommandDto? dto = JsonSerializer.Deserialize<CommandDto>(
                payload,
                new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
            if (dto is null)
            {
                error = "JSON 内容为空";
                return false;
            }
            if (dto.Dx is < short.MinValue or > short.MaxValue ||
                dto.Dy is < short.MinValue or > short.MaxValue)
            {
                error = "dx/dy 必须在 -32768..32767 范围内";
                return false;
            }
            if (dto.Wheel is < sbyte.MinValue or > sbyte.MaxValue ||
                dto.Pan is < sbyte.MinValue or > sbyte.MaxValue)
            {
                error = "wheel/pan 必须在 -128..127 范围内";
                return false;
            }
            if (dto.Dx == 0 && dto.Dy == 0 && dto.Wheel == 0 && dto.Pan == 0)
            {
                error = "命令没有任何移动或滚轮增量";
                return false;
            }

            command = new RemoteMouseCommand(dto.Dx, dto.Dy, dto.Wheel, dto.Pan);
            return true;
        }
        catch (JsonException exception)
        {
            error = $"JSON 无效：{exception.Message}";
            return false;
        }
    }
}

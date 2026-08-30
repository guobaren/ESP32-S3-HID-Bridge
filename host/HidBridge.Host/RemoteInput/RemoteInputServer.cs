using System.Buffers.Binary;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text.Json;
using HidBridge.Host.Input;

namespace HidBridge.Host.RemoteInput;

internal readonly record struct RemoteMouseCommand(int DeltaX, int DeltaY, int Wheel, int Pan);

internal readonly record struct DisplayAddressCandidate(
    IPAddress Address,
    bool HasDefaultGateway,
    bool IsVirtual);

internal sealed class RemoteInputServer : IDisposable
{
    private const int MaximumDatagramBytes = 4096;
    private readonly UdpClient _udp;
    private readonly InputForwarder _input;
    private readonly object _kmboxStateLock = new();
    private readonly CancellationTokenSource _cancellation = new();
    private IPEndPoint? _kmboxClient;
    private IPEndPoint? _monitorEndpoint;
    private uint? _kmboxMac;
    private uint _lastKmboxIndex;
    private Task? _receiveTask;
    private long _received;
    private long _accepted;
    private long _rejected;
    private DateTime _lastStatisticsUtc = DateTime.UtcNow;
    private bool _disposed;

    internal RemoteInputServer(
        string bindAddress,
        int port,
        InputForwarder input)
    {
        IPAddress address = ResolveBindAddress(bindAddress);
        _udp = new UdpClient(new IPEndPoint(address, port));
        _input = input;
        _input.KmboxMonitorReportAvailable += PublishKmboxMonitorReport;
    }

    internal int Port => ((IPEndPoint)_udp.Client.LocalEndPoint!).Port;
    internal string DisplayEndpoint => $"UDP {ResolveDisplayAddress()}:{Port}";

    internal void Start()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_receiveTask is not null)
        {
            return;
        }

        Console.WriteLine(
            $"局域网模拟输入已监听 UDP {GetLocalEndpoint()}；" +
            "是否在 HOME 关闭时继续输出由主界面“始终开启 UDP 输出”决定；" +
            "是否启用开发板 5 槽低延迟平滑由主界面开关决定。");
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

                bool accepted;
                string error;
                if (IsJsonPayload(result.Buffer))
                {
                    bool parsed = RemoteMouseCommandParser.TryParse(
                        result.Buffer,
                        out RemoteMouseCommand command,
                        out error);
                    accepted = parsed && _input.TryInjectMouseMovement(
                            command.DeltaX,
                            command.DeltaY,
                            command.Wheel,
                            command.Pan);
                    if (parsed && !accepted)
                    {
                        error = "同步和“始终开启 UDP 输出”均已关闭";
                    }
                }
                else
                {
                    accepted = await HandleKmboxPacketAsync(
                        result.Buffer,
                        result.RemoteEndPoint).ConfigureAwait(false);
                    error = "kmboxNet 协议包无效或尚未 init";
                }

                if (accepted)
                {
                    Interlocked.Increment(ref _accepted);
                }
                else
                {
                    Interlocked.Increment(ref _rejected);
                    Console.Error.WriteLine($"拒绝模拟输入命令（{result.RemoteEndPoint}）：{error}");
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

    private async Task<bool> HandleKmboxPacketAsync(byte[] payload, IPEndPoint remoteEndPoint)
    {
        uint? sessionMac;
        lock (_kmboxStateLock)
        {
            sessionMac = EndpointsEqual(_kmboxClient, remoteEndPoint) ? _kmboxMac : null;
        }
        if (!KmboxNetProtocol.TryParse(payload, sessionMac, out KmboxNetPacket packet, out _))
        {
            return false;
        }

        bool duplicate = false;
        if (packet.Kind == KmboxNetCommandKind.Connect)
        {
            lock (_kmboxStateLock)
            {
                _kmboxClient = remoteEndPoint;
                _kmboxMac = packet.Header.Mac;
                _lastKmboxIndex = packet.Header.Index;
                _monitorEndpoint = null;
            }
            _input.ReleaseRemoteInput();
        }
        else
        {
            lock (_kmboxStateLock)
            {
                if (!EndpointsEqual(_kmboxClient, remoteEndPoint) || _kmboxMac != packet.Header.Mac)
                {
                    return false;
                }
                duplicate = packet.Header.Index == _lastKmboxIndex;
                if (!duplicate)
                {
                    _lastKmboxIndex = packet.Header.Index;
                }
            }
        }

        if (!duplicate)
        {
            ApplyKmboxPacket(packet, remoteEndPoint);
        }
        byte[] acknowledgement = KmboxNetProtocol.EncodeAcknowledgement(packet.Header);
        await _udp.SendAsync(acknowledgement, remoteEndPoint).ConfigureAwait(false);
        return true;
    }

    private void ApplyKmboxPacket(KmboxNetPacket packet, IPEndPoint remoteEndPoint)
    {
        switch (packet.Kind)
        {
            case KmboxNetCommandKind.Connect:
                break;
            case KmboxNetCommandKind.Mouse:
                _input.SetRemoteMouseButtons(unchecked((byte)packet.Buttons));
                if (packet.DeltaX != 0 || packet.DeltaY != 0 || packet.Wheel != 0)
                {
                    _input.TryInjectMouseMovement(packet.DeltaX, packet.DeltaY, packet.Wheel);
                }
                break;
            case KmboxNetCommandKind.Keyboard:
                _input.SetRemoteKeyboardState(packet.Modifiers, packet.Keys ?? []);
                break;
            case KmboxNetCommandKind.Monitor:
                ushort monitorPort = unchecked((ushort)packet.Header.Random);
                lock (_kmboxStateLock)
                {
                    _monitorEndpoint = monitorPort == 0
                        ? null
                        : new IPEndPoint(remoteEndPoint.Address, monitorPort);
                }
                break;
            case KmboxNetCommandKind.Mask:
                byte keyToMask = unchecked((byte)(packet.Header.Random >> 8));
                _input.ConfigureKmboxMask(
                    unchecked((byte)packet.Header.Random),
                    keyToMask == 0 ? null : keyToMask);
                break;
            case KmboxNetCommandKind.Unmask:
                byte keyToUnmask = unchecked((byte)(packet.Header.Random >> 8));
                bool clearAll = packet.Header.Random == 0;
                _input.ConfigureKmboxMask(
                    unchecked((byte)packet.Header.Random),
                    keyToUnmask: keyToUnmask == 0 ? null : keyToUnmask,
                    clearAll: clearAll);
                break;
            case KmboxNetCommandKind.Trace:
                int traceValue = unchecked((int)(packet.Header.Random & 0x00ffffff));
                if ((traceValue & 0x00800000) != 0)
                {
                    traceValue |= unchecked((int)0xff000000);
                }
                _input.ConfigureUdpSmoothingFromKmbox(traceValue > 0);
                break;
        }
    }

    private void PublishKmboxMonitorReport(KmboxMonitorReport report)
    {
        IPEndPoint? endpoint;
        lock (_kmboxStateLock)
        {
            endpoint = _monitorEndpoint;
        }
        if (endpoint is null || _disposed)
        {
            return;
        }

        byte[] payload = new byte[21];
        payload[0] = 1;
        payload[1] = report.MouseButtons;
        BinaryPrimitives.WriteInt16LittleEndian(payload.AsSpan(2), report.DeltaX);
        BinaryPrimitives.WriteInt16LittleEndian(payload.AsSpan(4), report.DeltaY);
        BinaryPrimitives.WriteInt16LittleEndian(payload.AsSpan(6), report.Wheel);
        payload[9] = 2;
        payload[10] = report.Modifiers;
        report.Keys.AsSpan(0, Math.Min(10, report.Keys.Length)).CopyTo(payload.AsSpan(11));
        _ = SendMonitorReportAsync(payload, endpoint);
    }

    private async Task SendMonitorReportAsync(byte[] payload, IPEndPoint endpoint)
    {
        try
        {
            await _udp.SendAsync(payload, endpoint).ConfigureAwait(false);
        }
        catch (Exception exception) when (_disposed || exception is ObjectDisposedException or SocketException)
        {
            if (!_disposed)
            {
                Console.Error.WriteLine($"发送 kmboxNet monitor 状态失败：{exception.Message}");
            }
        }
    }

    private static bool IsJsonPayload(ReadOnlySpan<byte> payload)
    {
        foreach (byte value in payload)
        {
            if (!char.IsWhiteSpace((char)value))
            {
                return value == (byte)'{';
            }
        }
        return false;
    }

    private static bool EndpointsEqual(IPEndPoint? left, IPEndPoint right) =>
        left is not null && left.Port == right.Port && left.Address.Equals(right.Address);

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

    private string ResolveDisplayAddress()
    {
        IPEndPoint localEndpoint = (IPEndPoint)_udp.Client.LocalEndPoint!;
        if (!localEndpoint.Address.Equals(IPAddress.Any))
        {
            return localEndpoint.Address.ToString();
        }

        IPAddress? routeAddress = null;
        try
        {
            using Socket routeProbe = new(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
            routeProbe.Connect(new IPEndPoint(IPAddress.Parse("1.1.1.1"), 65530));
            if (routeProbe.LocalEndPoint is IPEndPoint routeEndpoint &&
                !routeEndpoint.Address.Equals(IPAddress.Any) &&
                !IPAddress.IsLoopback(routeEndpoint.Address))
            {
                routeAddress = routeEndpoint.Address;
            }
        }
        catch (SocketException)
        {
            // 没有默认路由时继续从已启用网卡中选择。
        }

        DisplayAddressCandidate[] candidates = NetworkInterface.GetAllNetworkInterfaces()
            .Where(adapter => adapter.OperationalStatus == OperationalStatus.Up &&
                adapter.NetworkInterfaceType is not NetworkInterfaceType.Loopback and not NetworkInterfaceType.Tunnel)
            .SelectMany(adapter =>
            {
                IPInterfaceProperties properties = adapter.GetIPProperties();
                bool hasDefaultGateway = properties.GatewayAddresses.Any(gateway =>
                    gateway.Address.AddressFamily == AddressFamily.InterNetwork &&
                    !gateway.Address.Equals(IPAddress.Any));
                bool isVirtual = LooksLikeVirtualAdapter(adapter.Name, adapter.Description);
                return properties.UnicastAddresses.Select(address =>
                    new DisplayAddressCandidate(address.Address, hasDefaultGateway, isVirtual));
            })
            .ToArray();
        return SelectDisplayAddress(candidates, routeAddress).ToString();
    }

    internal static IPAddress SelectDisplayAddress(
        IEnumerable<DisplayAddressCandidate> candidates,
        IPAddress? routeAddress)
    {
        DisplayAddressCandidate[] usable = candidates
            .Where(candidate => IsUsableLanAddress(candidate.Address))
            .ToArray();
        DisplayAddressCandidate[] physical = usable
            .Where(candidate => !candidate.IsVirtual)
            .ToArray();

        DisplayAddressCandidate? selected = physical
            .Where(candidate => candidate.HasDefaultGateway && candidate.Address.Equals(routeAddress))
            .Select(candidate => (DisplayAddressCandidate?)candidate)
            .FirstOrDefault();
        selected ??= physical
            .Where(candidate => candidate.HasDefaultGateway)
            .Select(candidate => (DisplayAddressCandidate?)candidate)
            .FirstOrDefault();
        // Hyper-V 外部虚拟交换机承载主机真实 LAN 地址时，网卡名称/描述虽然带
        // Virtual/vEthernet，但它具有 IPv4 默认网关，应视为可供其他局域网设备连接。
        selected ??= usable
            .Where(candidate => candidate.HasDefaultGateway && candidate.Address.Equals(routeAddress))
            .Select(candidate => (DisplayAddressCandidate?)candidate)
            .FirstOrDefault();
        selected ??= usable
            .Where(candidate => candidate.HasDefaultGateway)
            .Select(candidate => (DisplayAddressCandidate?)candidate)
            .FirstOrDefault();
        selected ??= physical
            .Where(candidate => candidate.Address.Equals(routeAddress))
            .Select(candidate => (DisplayAddressCandidate?)candidate)
            .FirstOrDefault();
        selected ??= physical
            .Select(candidate => (DisplayAddressCandidate?)candidate)
            .FirstOrDefault();
        selected ??= usable
            .Where(candidate => candidate.Address.Equals(routeAddress))
            .Select(candidate => (DisplayAddressCandidate?)candidate)
            .FirstOrDefault();
        selected ??= usable
            .Select(candidate => (DisplayAddressCandidate?)candidate)
            .FirstOrDefault();
        return selected?.Address ?? IPAddress.Loopback;
    }

    private static bool IsUsableLanAddress(IPAddress address) =>
        address.AddressFamily == AddressFamily.InterNetwork &&
        !IPAddress.IsLoopback(address) &&
        !address.Equals(IPAddress.Any) &&
        !address.ToString().StartsWith("169.254.", StringComparison.Ordinal);

    private static bool LooksLikeVirtualAdapter(string name, string description)
    {
        string value = $"{name} {description}";
        string[] markers =
        [
            "virtual", "hyper-v", "vmware", "vbox", "virtualbox", "wsl", "docker",
            "vpn", "wireguard", "tailscale", "zerotier", "singbox", "sing-box",
            "sing-tun", " tunnel", " tap", " tun",
        ];
        return markers.Any(marker => value.Contains(marker, StringComparison.OrdinalIgnoreCase));
    }

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
        _input.KmboxMonitorReportAvailable -= PublishKmboxMonitorReport;
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

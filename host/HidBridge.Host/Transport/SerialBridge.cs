using System.Diagnostics;
using System.IO.Ports;
using System.Buffers.Binary;
using System.Text;
using HidBridge.Host.Automation;
using HidBridge.Host.Input;
using HidBridge.Protocol;

namespace HidBridge.Host.Transport;

internal sealed record SerialPortRefreshSnapshot(string[] AvailablePorts, string[] OpenPorts);

internal sealed class SerialBridge : IBridgeTransport, IMakcuModeHandshake
{
    private readonly record struct OutboundFrame(byte[] Bytes, long Epoch);

    private const int A5BaudRate = MakcuModeCoordinator.A5BaudRate;
    private const int MakcuBaudRate = MakcuModeCoordinator.MakcuBaudRate;
    private const int MaxOutboundFrames = 32;
    private const int ModeResponseTimeoutMilliseconds = 1200;
    private const int TraceStopTimeoutMilliseconds = 2000;
    private static readonly string[] ReducedLogMarkers =
    [
        "BLE connection parameters",
        "connection interval",
        "low-latency",
        "CONNECT",
        "DISCONNECT",
        "encryption",
        "BLE HID",
        "BLE鼠标统计",
        "鼠标统计",
        "USB诊断",
        "输入统计",
        "UART0协议统计",
        "UART1统计",
        "HID统计",
        "advertising restarted",
    ];

    private readonly BridgeOptions _options;
    private readonly AutomationSettings _settings;
    private readonly RuntimeLogSettings _logSettings;
    private readonly Func<bool> _legacyFirmwareCompatibility;
    private readonly FrameCodec _codec = new();
    private readonly FrameCodec _diagnosticCodec = new();
    private readonly object _sync = new();
    private readonly object _writeSync = new();
    private readonly SemaphoreSlim _statsQueryGate = new(1, 1);
    private readonly DeviceStatsPendingRequestRegistry _statsRequests = new();
    private readonly Queue<OutboundFrame> _outboundFrames = [];
    private readonly ButtonStateSharedMemory _buttonStatePublisher;
    private readonly System.Threading.Timer _buttonStateRetryTimer;
    private readonly MakcuModeCoordinator _makcuModeCoordinator;
    private readonly AutoResetEvent _outboundSignal = new(false);
    private readonly Thread _writerThread;
    private readonly System.Threading.Timer _heartbeatTimer;
    private SerialPort? _port;
    private string? _connectedPortName;
    private bool _connectedLegacyCompatibility;
    private int _connectedBaudRate = A5BaudRate;
    private int _makcuModeEnabled;
    private int _modeTransitioning;
    private long _routingEpoch;
    private long _connectionEpoch;
    private bool _buttonSubscriptionRequested;
    private bool _buttonSubscriptionHealthy;
    private bool _buttonStatePublishPending;
    private byte _physicalButtons;
    private bool _physicalMouseValid;
    private long _physicalButtonStateTimestampQpc;
    private int _heartbeatActive;
    private DateTime _nextConnectAttemptUtc;
    private bool _sessionStarted;
    private bool _exclusivePortLeaseActive;
    /*
     * 对端板卡日志监听（2026-09-28）：它占用的是**另一个**串口，但固件刷写等独占任务
     * 可能正好要刷那一块板，所以让出端口时必须把它也算上——否则 esptool 会打不开端口
     * （实测 P 板刷写因此失败，报 "esptool 退出码为 2"）。
     */
    private DeviceLogMirror? _peerLogMirror;
    private string? _exclusivePortPurpose;
    private bool _writerStopping;
    private bool _disposed;
    private CancellationTokenSource? _traceCancellation;
    private Task? _traceTask;
    private BufferedDeviceLog? _traceWriter;
    private long _nextBatchStatisticsTimestamp;
    private long _batchStatisticsStartTimestamp;
    private long _batchWriteCount;
    private long _batchedFrameCount;
    private long _mouseFramesQueued;
    private int _maxFramesPerBatch;
    private int _outboundQueuePeak;
    private long _backpressureCount;

    public SerialBridge(
        BridgeOptions options,
        RuntimeLogSettings logSettings,
        Func<bool>? legacyFirmwareCompatibility = null,
        AutomationSettings? settings = null)
    {
        _options = options;
        _settings = settings ?? new AutomationSettings();
        _logSettings = logSettings;
        _legacyFirmwareCompatibility = legacyFirmwareCompatibility ?? (() => false);
        _settings.ButtonStateMappingName = ButtonStateSharedMemory.NormalizeConfiguredMappingName(
            _settings.ButtonStateMappingName);
        _buttonStatePublisher = new ButtonStateSharedMemory(_settings.ButtonStateMappingName);
        _buttonStateRetryTimer = new System.Threading.Timer(
            _ => RetryButtonStatePublish(),
            null,
            Timeout.Infinite,
            Timeout.Infinite);
        _makcuModeCoordinator = new MakcuModeCoordinator(this);
        _batchStatisticsStartTimestamp = Stopwatch.GetTimestamp();
        _nextBatchStatisticsTimestamp = _batchStatisticsStartTimestamp + Stopwatch.Frequency;
        _writerThread = new Thread(WriterLoop)
        {
            IsBackground = true,
            Name = "HidBridge.SerialBatchWriter",
            Priority = ThreadPriority.AboveNormal,
        };
        _writerThread.Start();
        _heartbeatTimer = new System.Threading.Timer(
            _ => HeartbeatTick(),
            null,
            TimeSpan.Zero,
            TimeSpan.FromMilliseconds(options.HeartbeatIntervalMilliseconds));
    }

    private void HeartbeatTick()
    {
        if (!TryEnterHeartbeat(ref _heartbeatActive))
        {
            return;
        }

        try
        {
            Send(MessageType.Ping, ReadOnlySpan<byte>.Empty);
        }
        finally
        {
            ExitHeartbeat(ref _heartbeatActive);
        }
    }

    internal static bool TryEnterHeartbeat(ref int active) => Interlocked.Exchange(ref active, 1) == 0;

    internal static void ExitHeartbeat(ref int active) => Volatile.Write(ref active, 0);

    public void Send(MessageType type, ReadOnlySpan<byte> payload)
    {
        lock (_sync)
        {
            if (_disposed || _exclusivePortLeaseActive || IsInputWriteGated || !EnsureConnected())
            {
                return;
            }

            if (!_sessionStarted)
            {
                if (!EnqueueFrameLocked(
                        _codec.Encode(MessageType.SessionStart, ReadOnlySpan<byte>.Empty)))
                {
                    return;
                }
                _sessionStarted = true;
            }
            if (EnqueueFrameLocked(_codec.Encode(type, payload)) &&
                type == MessageType.MouseReport)
            {
                _mouseFramesQueued++;
            }
        }
    }

    internal async Task<DeviceStatisticsQueryResult> ReadDeviceStatisticsAsync(
        TimeSpan timeout,
        CancellationToken cancellationToken = default)
    {
        if (Volatile.Read(ref _makcuModeEnabled) != 0 ||
            Volatile.Read(ref _modeTransitioning) != 0)
        {
            throw new InvalidOperationException(
                "M 板当前处于 Makcu 模式或正在切换；UART0 统计响应仅支持 A5 模式，请先关闭 Makcu。");
        }
        if (timeout <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(timeout));
        }

        using CancellationTokenSource deadline =
            CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        deadline.CancelAfter(timeout);
        DeviceStatsQueryContext? query = null;
        bool gateEntered = false;
        try
        {
            try
            {
                await _statsQueryGate.WaitAsync(deadline.Token).ConfigureAwait(false);
                gateEntered = true;
            }
            catch (OperationCanceledException) when (
                !cancellationToken.IsCancellationRequested && deadline.IsCancellationRequested)
            {
                throw new TimeoutException($"统计请求在 {timeout.TotalSeconds:F1} 秒内未能取得查询通道。");
            }

            string[] ports = GetOpenPortNames();
            if (ports.Length == 0)
            {
                throw new IOException("没有已打开的主串口或对端镜像串口可查询。");
            }

            byte[] request = _diagnosticCodec.Encode(MessageType.StatsSnapshotRequest, ReadOnlySpan<byte>.Empty);
            ushort sequence = BinaryPrimitives.ReadUInt16LittleEndian(request.AsSpan(4, 2));
            query = _statsRequests.Register(ports, sequence);

            try
            {
                foreach (string portName in ports)
                {
                    deadline.Token.ThrowIfCancellationRequested();
                    int? written = WriteToOpenPort(portName, request, deadline.Token);
                    if (written != request.Length)
                    {
                        throw new IOException($"端口 {portName} 当前不由本程序持有或请求帧未完整写入。");
                    }
                }

                return await query.Completion.Task.WaitAsync(deadline.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (
                !cancellationToken.IsCancellationRequested && deadline.IsCancellationRequested)
            {
                string pending = query?.DescribeIncomplete() ?? "尚未收到板端分页";
                throw new TimeoutException(
                    $"统计请求在 {timeout.TotalSeconds:F1} 秒内未收齐所有分页；{pending}");
            }
            catch
            {
                lock (query.Sync)
                {
                    query.Closed = true;
                    query.Completion.TrySetCanceled(deadline.Token);
                }
                throw;
            }
        }
        finally
        {
            if (query is not null)
            {
                _statsRequests.Clear(query);
            }
            if (gateEntered)
            {
                _statsQueryGate.Release();
            }
        }
    }

    internal void OnDiagnosticFrame(string portName, BridgeFrame frame)
    {
        if (!_statsRequests.TryGet(
                portName,
                frame,
                out DeviceStatsQueryContext? query,
                out DeviceStatsPageCollector? collector) ||
            query is null || collector is null)
        {
            return;
        }

        lock (query.Sync)
        {
            if (query.Closed)
            {
                return;
            }
            try
            {
                collector.Add(frame);
                if (query.Collectors.Values.All(item => item.IsComplete))
                {
                    DeviceStatisticsSnapshot[] boards = query.Collectors.Values
                        .Select(item => item.Build())
                        .OrderBy(item => item.Role)
                        .ToArray();
                    query.Closed = true;
                    query.Completion.TrySetResult(new DeviceStatisticsQueryResult(
                        DateTimeOffset.Now,
                        boards));
                }
            }
            catch (Exception exception) when (
                exception is InvalidDataException or InvalidOperationException)
            {
                query.Closed = true;
                query.Completion.TrySetException(exception);
            }
        }
    }

    internal async Task<MakcuModeChangeResult> SetMakcuModeAsync(
        bool enabled,
        CancellationToken cancellationToken = default)
    {
        if (_settings.LegacySingleBoardFirmwareCompatibility)
        {
            return new MakcuModeChangeResult(
                false,
                Volatile.Read(ref _makcuModeEnabled) != 0,
                Volatile.Read(ref _makcuModeEnabled) != 0 ? MakcuBaudRate : A5BaudRate,
                "旧版单板通路与双板 Makcu 模式不能同时启用。");
        }

        bool connected = await WaitForConnectionAsync(
            TimeSpan.FromSeconds(5),
            cancellationToken).ConfigureAwait(false);
        if (!connected)
        {
            return new MakcuModeChangeResult(
                false,
                Volatile.Read(ref _makcuModeEnabled) != 0,
                Volatile.Read(ref _connectedBaudRate),
                "尚未验证 M 板角色，无法切换 Makcu 模式。");
        }

        bool currentEnabled = Volatile.Read(ref _makcuModeEnabled) != 0;
        int currentBaud = Volatile.Read(ref _connectedBaudRate);
        return await Task.Run(
            () => _makcuModeCoordinator.SetEnabledAsync(
                enabled,
                currentEnabled,
                currentBaud,
                compiledCapabilityAvailable: true,
                cancellationToken: cancellationToken),
            cancellationToken).ConfigureAwait(false);
    }

    internal async Task<bool> SetButtonStatePublishingEnabledAsync(
        bool enabled,
        CancellationToken cancellationToken = default)
    {
        return await Task.Run(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            lock (_sync)
            {
                _settings.ButtonStatePublishingEnabled = enabled;
                bool available = !_disposed && !_exclusivePortLeaseActive &&
                    !IsInputWriteGated && !_connectedLegacyCompatibility &&
                    _port?.IsOpen == true;
                bool sent = true;
                if (available)
                {
                    sent = WriteButtonSubscriptionLocked(enabled);
                }
                _buttonSubscriptionRequested = enabled && available && sent;
                _buttonSubscriptionHealthy = false;
                _physicalButtons = 0;
                _physicalMouseValid = false;
                _physicalButtonStateTimestampQpc = 0;
                PublishButtonStateLocked(_port?.IsOpen == true && !_exclusivePortLeaseActive
                    ? (byte)1
                    : (byte)0);
                Console.WriteLine($"M 板物理按键状态发布已{(enabled ? "启用" : "停用")}。");
                return sent;
            }
        }, cancellationToken).ConfigureAwait(false);
    }

    private bool IsInputWriteGated =>
        Volatile.Read(ref _modeTransitioning) != 0 ||
        Volatile.Read(ref _makcuModeEnabled) != 0;

    internal static bool IsOutboundBatchEligible(
        long batchEpoch,
        long currentRoutingEpoch,
        bool modeGated) => !modeGated && batchEpoch == currentRoutingEpoch;

    private void OnMainPortFrame(string portName, long connectionEpoch, BridgeFrame frame)
    {
        if (frame.Type == MessageType.StatsSnapshotResponse)
        {
            OnDiagnosticFrame(portName, frame);
            return;
        }
        if (frame.Type != MessageType.ButtonState || frame.Payload.Length != 1)
        {
            return;
        }

        lock (_sync)
        {
            if (_disposed || _exclusivePortLeaseActive || IsInputWriteGated ||
                connectionEpoch != Volatile.Read(ref _connectionEpoch) ||
                !string.Equals(_connectedPortName, portName, StringComparison.OrdinalIgnoreCase) ||
                !_buttonSubscriptionRequested || _connectedLegacyCompatibility)
            {
                return;
            }

            byte value = frame.Payload[0];
            bool healthyChanged = !_buttonSubscriptionHealthy;
            _buttonSubscriptionHealthy = true;
            _physicalMouseValid = (value & 0x80) != 0;
            _physicalButtons = _physicalMouseValid ? (byte)(value & 0x1F) : (byte)0;
            _physicalButtonStateTimestampQpc = Stopwatch.GetTimestamp();
            PublishButtonStateLocked(2);
            if (healthyChanged)
            {
                Console.WriteLine("M 板物理按键状态订阅已通过当前连接的真实状态帧确认。");
            }
        }
    }

    private void PublishButtonStateLocked(byte connectionState)
    {
        bool published = _buttonStatePublisher.TryPublish(new ButtonStateSnapshot(
            _physicalButtons,
            _physicalMouseValid,
            _buttonSubscriptionHealthy,
            connectionState,
            _physicalButtonStateTimestampQpc));
        _buttonStatePublishPending = !published;
        _buttonStateRetryTimer.Change(
            published ? Timeout.Infinite : 5,
            Timeout.Infinite);
    }

    private void RetryButtonStatePublish()
    {
        lock (_sync)
        {
            if (_disposed || !_buttonStatePublishPending)
            {
                return;
            }

            PublishButtonStateLocked(_port?.IsOpen == true && !_exclusivePortLeaseActive
                ? (byte)(_buttonSubscriptionHealthy ? 2 : 1)
                : (byte)0);
        }
    }

    private bool WriteButtonSubscriptionLocked(bool enabled, bool allowDuringTransition = false)
    {
        SerialPort? port = _port;
        if (port?.IsOpen != true)
        {
            return false;
        }

        byte[] payload = [enabled ? (byte)1 : (byte)0];
        byte[] frame = _codec.Encode(MessageType.ButtonState, payload);
        try
        {
            lock (_writeSync)
            {
                if (!ReferenceEquals(_port, port) || !port.IsOpen ||
                    (IsInputWriteGated && !allowDuringTransition))
                {
                    return false;
                }
                port.Write(frame, 0, frame.Length);
            }
            return true;
        }
        catch (Exception exception) when (
            exception is IOException or InvalidOperationException or UnauthorizedAccessException or TimeoutException)
        {
            Console.Error.WriteLine($"M 板物理按键订阅命令写入失败：{exception.Message}");
            return false;
        }
    }

    async Task<bool> IMakcuModeHandshake.BeginTransitionAsync(CancellationToken cancellationToken)
    {
        SerialPort? port;
        bool unsubscribe;
        lock (_sync)
        {
            if (_disposed || _exclusivePortLeaseActive || _port?.IsOpen != true ||
                Interlocked.CompareExchange(ref _modeTransitioning, 1, 0) != 0)
            {
                return false;
            }

            Interlocked.Increment(ref _routingEpoch);
            _outboundFrames.Clear();
            Monitor.PulseAll(_sync);
            _sessionStarted = false;
            Interlocked.Increment(ref _connectionEpoch);
            unsubscribe = _buttonSubscriptionRequested;
            _buttonSubscriptionRequested = false;
            _buttonSubscriptionHealthy = false;
            _physicalButtons = 0;
            _physicalMouseValid = false;
            _physicalButtonStateTimestampQpc = 0;
            PublishButtonStateLocked(1);
            port = _port;
        }

        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            // 获取写门构成 fence：已出队的旧 batch 只能在此之前完成，或因 epoch 不匹配而被丢弃。
            lock (_writeSync)
            {
                if (!ReferenceEquals(_port, port) || port?.IsOpen != true)
                {
                    return false;
                }
            }
            if (unsubscribe)
            {
                _ = WriteButtonSubscriptionLocked(enabled: false, allowDuringTransition: true);
            }
            return await StopDeviceTraceAndConfirmAsync(cancellationToken).ConfigureAwait(false);
        }
        catch
        {
            // 不能确认唯一 reader 停止时保留 gate；调用方会显示失败且不会开第二 reader。
            throw;
        }
    }

    Task<MakcuModeSnapshot?> IMakcuModeHandshake.SendRequestAsync(
        bool enabled,
        CancellationToken cancellationToken) =>
        Task.Run(() => SendMakcuModeRequestBlocking(enabled, cancellationToken), cancellationToken);

    Task IMakcuModeHandshake.SetBaudRateAsync(int baudRate, CancellationToken cancellationToken) =>
        Task.Run(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            SerialPort port = GetPortForModeHandshake();
            lock (_writeSync)
            {
                if (!ReferenceEquals(_port, port) || !port.IsOpen)
                {
                    throw new IOException("模式切换时 M 板串口已断开。");
                }
                port.BaudRate = baudRate;
                Volatile.Write(ref _connectedBaudRate, baudRate);
            }
        }, cancellationToken);

    Task<MakcuModeSnapshot?> IMakcuModeHandshake.QueryAsync(CancellationToken cancellationToken) =>
        Task.Run(() => QueryMakcuModeBlocking(GetPortForModeHandshake(), cancellationToken), cancellationToken);

    Task<bool> IMakcuModeHandshake.ProbeMouseHostAsync(CancellationToken cancellationToken) =>
        Task.Run(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            SerialPort port = GetPortForModeHandshake();
            return SerialDeviceProbe.Probe(port, _codec, SerialDeviceProbe.MouseHostRole);
        }, cancellationToken);

    Task<MakcuModeSnapshot?> IMakcuModeHandshake.QueryAtBaudAsync(
        int baudRate,
        CancellationToken cancellationToken) =>
        Task.Run(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            SerialPort port = GetPortForModeHandshake();
            lock (_writeSync)
            {
                if (!ReferenceEquals(_port, port) || !port.IsOpen)
                {
                    throw new IOException("模式恢复时 M 板串口已断开。");
                }
                port.BaudRate = baudRate;
                Volatile.Write(ref _connectedBaudRate, baudRate);
            }
            return QueryMakcuModeBlocking(port, cancellationToken);
        }, cancellationToken);

    Task<bool> IMakcuModeHandshake.EndTransitionAsync(
        bool enabled,
        int baudRate,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_sync)
        {
            SerialPort? port = _port;
            if (_disposed || _exclusivePortLeaseActive || port?.IsOpen != true ||
                port.BaudRate != baudRate ||
                (enabled && baudRate != MakcuBaudRate) ||
                (!enabled && baudRate != A5BaudRate))
            {
                return Task.FromResult(false);
            }

            _connectedBaudRate = baudRate;
            Volatile.Write(ref _makcuModeEnabled, enabled ? 1 : 0);
            _sessionStarted = false;
            _buttonSubscriptionRequested = !enabled &&
                !_connectedLegacyCompatibility &&
                _settings.ButtonStatePublishingEnabled;
            _buttonSubscriptionHealthy = false;
            _physicalButtons = 0;
            _physicalMouseValid = false;
            _physicalButtonStateTimestampQpc = 0;
            long connectionEpoch = Interlocked.Increment(ref _connectionEpoch);
            PublishButtonStateLocked(1);
            try
            {
                StartDeviceTrace(port, port.PortName, connectionEpoch);
            }
            catch (Exception exception) when (
                exception is IOException or InvalidOperationException or UnauthorizedAccessException or ArgumentException)
            {
                _buttonSubscriptionRequested = false;
                Console.Error.WriteLine($"恢复设备日志读取器失败：{exception.Message}");
                return Task.FromResult(false);
            }

            if (_buttonSubscriptionRequested &&
                !WriteButtonSubscriptionLocked(enabled: true, allowDuringTransition: true))
            {
                _buttonSubscriptionRequested = false;
            }

            Volatile.Write(ref _modeTransitioning, 0);
            Console.WriteLine(
                $"M 板串口输入门已恢复：{(enabled ? "Makcu" : "A5")} / {baudRate} baud。");
            return Task.FromResult(true);
        }
    }

    private SerialPort GetPortForModeHandshake()
    {
        lock (_sync)
        {
            if (_disposed || _exclusivePortLeaseActive || Volatile.Read(ref _modeTransitioning) == 0 ||
                _port?.IsOpen != true)
            {
                throw new IOException("没有处于受控模式切换中的已打开 M 板串口。");
            }
            return _port;
        }
    }

    private MakcuModeSnapshot? SendMakcuModeRequestBlocking(
        bool enabled,
        CancellationToken cancellationToken)
    {
        byte opcode = enabled ? MakcuModeProtocol.EnableOpcode : MakcuModeProtocol.DisableOpcode;
        SerialPort port = GetPortForModeHandshake();
        byte[] frame = MakcuModeProtocol.CreateRequest(_codec, opcode, out ushort sequence);
        lock (_writeSync)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!ReferenceEquals(_port, port) || !port.IsOpen)
            {
                throw new IOException("发送 Makcu 模式请求前 M 板串口已断开。");
            }
            port.Write(frame, 0, frame.Length);
        }
        BridgeFrame response = ReadModeStatusFrame(port, sequence, cancellationToken);
        return MakcuModeProtocol.TryDecodeStatus(response, sequence, out MakcuModeSnapshot snapshot)
            ? snapshot
            : null;
    }

    private MakcuModeSnapshot? QueryMakcuModeBlocking(
        SerialPort port,
        CancellationToken cancellationToken = default)
    {
        byte[] frame = MakcuModeProtocol.CreateRequest(
            _codec,
            MakcuModeProtocol.QueryOpcode,
            out ushort sequence);
        lock (_writeSync)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!ReferenceEquals(_port, port) && _port is not null)
            {
                // 首次发现时 port 尚未写入 _port；运行时查询必须始终沿用同一端口对象。
                throw new IOException("Makcu 状态查询目标串口已变化。");
            }
            if (!port.IsOpen)
            {
                throw new IOException("Makcu 状态查询目标串口已关闭。");
            }
            port.Write(frame, 0, frame.Length);
        }
        BridgeFrame response = ReadModeStatusFrame(port, sequence, cancellationToken);
        return MakcuModeProtocol.TryDecodeStatus(response, sequence, out MakcuModeSnapshot snapshot)
            ? snapshot
            : null;
    }

    private BridgeFrame ReadModeStatusFrame(
        SerialPort port,
        ushort expectedSequence,
        CancellationToken cancellationToken)
    {
        DeviceOutputFrameScanner scanner = new();
        byte[] buffer = new byte[256];
        BridgeFrame? matched = null;
        long deadline = Stopwatch.GetTimestamp() +
            (long)(ModeResponseTimeoutMilliseconds * (double)Stopwatch.Frequency / 1000);
        while (Stopwatch.GetTimestamp() < deadline)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!port.IsOpen)
            {
                throw new IOException("等待 Makcu 模式响应时串口已关闭。");
            }
            int available = port.BytesToRead;
            if (available <= 0)
            {
                Thread.Sleep(2);
                continue;
            }
            int read;
            try
            {
                read = port.Read(buffer, 0, Math.Min(buffer.Length, available));
            }
            catch (TimeoutException)
            {
                continue;
            }
            if (read <= 0)
            {
                continue;
            }
            _ = scanner.Feed(buffer.AsSpan(0, read), frame =>
            {
                if (frame.Type == MessageType.MakcuModeStatus &&
                    frame.Sequence == expectedSequence)
                {
                    matched = frame;
                }
            });
            if (matched is BridgeFrame response)
            {
                return response;
            }
        }
        throw new TimeoutException(
            $"等待 MakcuModeStatus 序号 {expectedSequence} 超时（{ModeResponseTimeoutMilliseconds} ms）。");
    }

    private bool EnqueueFrameLocked(byte[] frame)
    {
        bool backpressured = false;
        while (_outboundFrames.Count >= MaxOutboundFrames &&
               !_disposed && !_writerStopping && !_exclusivePortLeaseActive)
        {
            backpressured = true;
            Monitor.Wait(_sync, 10);
        }
        if (_disposed || _writerStopping || _exclusivePortLeaseActive ||
                IsInputWriteGated ||
            _port?.IsOpen != true)
        {
            return false;
        }

        _outboundFrames.Enqueue(new OutboundFrame(frame, Volatile.Read(ref _routingEpoch)));
        if (backpressured)
        {
            _backpressureCount++;
        }
        _outboundQueuePeak = Math.Max(_outboundQueuePeak, _outboundFrames.Count);
        _outboundSignal.Set();
        return true;
    }

    private void WriterLoop()
    {
        while (true)
        {
            _outboundSignal.WaitOne();
            lock (_sync)
            {
                if (_writerStopping)
                {
                    return;
                }
            }

            // 不主动等待：只合并上一次 CH340 写入阻塞期间自然积累的帧。
            // 这样既不阻塞 500 Hz 采集线程，也不会人为造成多帧突发。
            while (true)
            {
                SerialPort? port;
                byte[] batch;
                int frameCount;
                long batchEpoch;
                lock (_sync)
                {
                    if (_writerStopping)
                    {
                        return;
                    }
                    if (_outboundFrames.Count == 0)
                    {
                        break;
                    }
                    if (_port?.IsOpen != true || _exclusivePortLeaseActive)
                    {
                        _outboundFrames.Clear();
                        Monitor.PulseAll(_sync);
                        break;
                    }

                    port = _port;
                    (batch, frameCount, batchEpoch) = DrainOutboundBatchLocked();
                    Monitor.PulseAll(_sync);
                }

                try
                {
                    lock (_writeSync)
                    {
                        if (!ReferenceEquals(_port, port) || !port.IsOpen ||
                            !IsOutboundBatchEligible(
                                batchEpoch,
                                Volatile.Read(ref _routingEpoch),
                                IsInputWriteGated))
                        {
                            continue;
                        }
                        port.Write(batch, 0, batch.Length);
                    }
                    lock (_sync)
                    {
                        _batchWriteCount++;
                        _batchedFrameCount += frameCount;
                        _maxFramesPerBatch = Math.Max(_maxFramesPerBatch, frameCount);
                        LogBatchStatisticsIfDueLocked();
                    }
                    break;
                }
                catch (Exception exception) when (
                    exception is IOException or InvalidOperationException or UnauthorizedAccessException)
                {
                    Console.Error.WriteLine($"串口批量写入失败：{exception.Message}");
                    lock (_sync)
                    {
                        ClosePort();
                    }
                    break;
                }
            }
        }
    }

    private (byte[] Batch, int FrameCount, long Epoch) DrainOutboundBatchLocked()
    {
        long epoch = _outboundFrames.Peek().Epoch;
        int length = 0;
        foreach (OutboundFrame queued in _outboundFrames)
        {
            if (queued.Epoch == epoch)
            {
                length += queued.Bytes.Length;
            }
        }
        byte[] batch = new byte[length];
        int offset = 0;
        int frameCount = 0;
        while (_outboundFrames.Count > 0 && _outboundFrames.Peek().Epoch == epoch)
        {
            byte[] frame = _outboundFrames.Dequeue().Bytes;
            Buffer.BlockCopy(frame, 0, batch, offset, frame.Length);
            offset += frame.Length;
            frameCount++;
        }
        return (batch, frameCount, epoch);
    }

    /// <summary>列出当前由控制串口或对端日志镜像实际持有的端口，不主动探测或打开端口。</summary>
    internal string[] GetOpenPortNames()
    {
        string? mainPortName;
        DeviceLogMirror? peerLogMirror;
        lock (_sync)
        {
            mainPortName = !_exclusivePortLeaseActive && _port?.IsOpen == true
                ? _port.PortName
                : null;
            peerLogMirror = _peerLogMirror;
        }

        string? mirrorPortName = peerLogMirror?.GetOpenPortName();
        return new[] { mainPortName, mirrorPortName }
            .Where(name => !string.IsNullOrWhiteSpace(name))
            .Select(name => name!)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();
    }

    internal SerialPortRefreshSnapshot RefreshOpenSerialPorts(CancellationToken cancellationToken)
    {
        DeviceLogMirror? peerLogMirror;
        bool mayRefresh;
        lock (_sync)
        {
            mayRefresh = !_disposed && !_exclusivePortLeaseActive;
            peerLogMirror = _peerLogMirror;
        }

        if (mayRefresh)
        {
            peerLogMirror?.Refresh(cancellationToken);
        }

        return new SerialPortRefreshSnapshot(GetAvailablePortNames(), GetOpenPortNames());
    }

    /// <summary>
    /// 向当前已打开的主串口或日志镜像串口同步写入原始字节。主串口与 WriterLoop 共用
    /// _writeSync 写入门，保证两个完整写操作不会交错。
    /// </summary>
    /// <returns>写入完成的字节数；端口当前不由本进程持有时返回 null。</returns>
    internal int? WriteToOpenPort(string portName, byte[] bytes, CancellationToken cancellationToken)
    {
        if (bytes.Length == 0)
        {
            throw new ArgumentException("串口写入数据不能为空。", nameof(bytes));
        }

        cancellationToken.ThrowIfCancellationRequested();
        DeviceLogMirror? peerLogMirror;
        lock (_sync)
        {
            cancellationToken.ThrowIfCancellationRequested();
            peerLogMirror = _peerLogMirror;
            if (string.Equals(_connectedPortName, portName, StringComparison.OrdinalIgnoreCase) &&
                IsInputWriteGated)
            {
                throw new InvalidOperationException(
                    "M 板处于 Makcu 模式或正在切换；普通串口写入已关闭，不能绕过模式门控。");
            }
            if (!_disposed && !_exclusivePortLeaseActive && _port?.IsOpen == true &&
                string.Equals(_port.PortName, portName, StringComparison.OrdinalIgnoreCase))
            {
                SerialPort port = _port!;
                lock (_writeSync)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    if (!ReferenceEquals(_port, port) || !port.IsOpen)
                    {
                        throw new IOException("主串口在写入前已断开。");
                    }
                    port.Write(bytes, 0, bytes.Length);
                }
                return bytes.Length;
            }
        }

        return peerLogMirror?.WriteToOpenPort(portName, bytes, cancellationToken);
    }

    private void LogBatchStatisticsIfDueLocked()
    {
        long now = Stopwatch.GetTimestamp();
        if (now < _nextBatchStatisticsTimestamp)
        {
            return;
        }
        double average = _batchWriteCount == 0
            ? 0
            : (double)_batchedFrameCount / _batchWriteCount;
        double elapsedSeconds = Math.Max(
            0.001,
            (double)(now - _batchStatisticsStartTimestamp) / Stopwatch.Frequency);
        // 本周期没有鼠标帧就不打印（2026-09-28）：空闲时每秒一行没有信息量。
        // 注意下面的计数器重置仍要无条件执行，否则窗口增量会越算越错。
        if (_mouseFramesQueued > 0)
        {
            Console.WriteLine(
                $"EXE输出统计：MouseReport={_mouseFramesQueued} " +
                $"({_mouseFramesQueued / elapsedSeconds:F1} Hz)，串口写入={_batchWriteCount}，" +
                $"总帧={_batchedFrameCount}，" +
                $"平均帧/次={average:F2}，最大帧/次={_maxFramesPerBatch}，" +
                $"队列峰值={_outboundQueuePeak}/{MaxOutboundFrames}，反压={_backpressureCount}。");
        }
        _batchWriteCount = 0;
        _batchedFrameCount = 0;
        _mouseFramesQueued = 0;
        _maxFramesPerBatch = 0;
        _outboundQueuePeak = _outboundFrames.Count;
        _backpressureCount = 0;
        _batchStatisticsStartTimestamp = now;
        _nextBatchStatisticsTimestamp = now + Stopwatch.Frequency;
    }

    private bool EnsureConnected()
    {
        if (_exclusivePortLeaseActive || IsInputWriteGated)
        {
            return false;
        }

        bool requestedLegacyCompatibility = _legacyFirmwareCompatibility();
        if (_port?.IsOpen == true)
        {
            if (_connectedLegacyCompatibility == requestedLegacyCompatibility)
            {
                return true;
            }

            // 设置页模式变化后，不能继续复用旧握手和旧 payload 语义的连接。
            // 旧 Writer 队列可能还留有按下报告；先在旧串口写入 SessionStart + ReleaseAll，
            // 再关闭旧口，避免关闭后新模式的重连把旧 ReleaseAll 丢进新队列。
            Console.WriteLine(
                $"旧版单板兼容模式已{(requestedLegacyCompatibility ? "启用" : "停用")}，正在释放旧输入并重新连接串口。");
            ClosePortForInputModeChange();
            _nextConnectAttemptUtc = DateTime.MinValue;
        }

        if (_traceTask is not null && !TryFinalizeStoppedTrace())
        {
            return false;
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
            return false;
        }

        foreach (string portName in candidates)
        {
            bool legacyCompatibility = requestedLegacyCompatibility;
            int[] baudRates = legacyCompatibility
                ? [_options.BaudRate]
                : _settings.MakcuEnabled
                    ? [A5BaudRate, MakcuBaudRate]
                    : [A5BaudRate];
            foreach (int baudRate in baudRates)
            {
                SerialPort? candidate = null;
                try
                {
                    candidate = CreatePort(portName, baudRate);
                    candidate.Open();
                    bool probeMatched = legacyCompatibility ||
                        SerialDeviceProbe.Probe(candidate, _codec, SerialDeviceProbe.MouseHostRole);
                    bool makcuEnabled = false;
                    if (probeMatched && !legacyCompatibility && baudRate == MakcuBaudRate)
                    {
                        MakcuModeSnapshot? status = QueryMakcuModeBlocking(candidate);
                        probeMatched = status is { Status: 0, Enabled: true, BaudRate: MakcuBaudRate };
                        makcuEnabled = probeMatched;
                    }
                    if (!probeMatched)
                    {
                        candidate.Dispose();
                        candidate = null;
                        continue;
                    }

                    if (legacyCompatibility)
                    {
                        Console.WriteLine(
                            $"{portName} 已按旧版单板通路直接连接；跳过新角色握手，后续使用旧版传输。");
                    }

                    _port = candidate;
                    _connectedLegacyCompatibility = legacyCompatibility;
                    _connectedBaudRate = baudRate;
                    Volatile.Write(ref _makcuModeEnabled, makcuEnabled ? 1 : 0);
                    Volatile.Write(ref _connectedPortName, portName);
                    _sessionStarted = false;
                    long connectionEpoch = Interlocked.Increment(ref _connectionEpoch);
                    _buttonSubscriptionRequested = !legacyCompatibility &&
                        !makcuEnabled &&
                        _settings.ButtonStatePublishingEnabled;
                    _buttonSubscriptionHealthy = false;
                    _physicalButtons = 0;
                    _physicalMouseValid = false;
                    _physicalButtonStateTimestampQpc = 0;
                    PublishButtonStateLocked(1);
                    StartDeviceTrace(candidate, portName, connectionEpoch);
                    if (_buttonSubscriptionRequested && !WriteButtonSubscriptionLocked(enabled: true))
                    {
                        _buttonSubscriptionRequested = false;
                        PublishButtonStateLocked(1);
                    }
                    Console.WriteLine($"已连接 {portName} @ {baudRate} baud。");
                    return true;
                }
                catch (Exception exception) when (
                    exception is IOException or InvalidOperationException or UnauthorizedAccessException or
                        TimeoutException or ArgumentException or InvalidDataException)
                {
                    Console.Error.WriteLine($"暂时无法使用 {portName} @ {baudRate}：{exception.Message}");
                    if (ReferenceEquals(_port, candidate))
                    {
                        _port = null;
                        Volatile.Write(ref _connectedPortName, null);
                        _connectedLegacyCompatibility = false;
                        RequestStopDeviceTrace();
                    }
                    candidate?.Dispose();
                }
            }
        }

        return false;
    }

    private void StartDeviceTrace(SerialPort port, string portName, long connectionEpoch)
    {
        if (_traceTask is not null && !TryFinalizeStoppedTrace())
        {
            throw new InvalidOperationException(
                "上一条设备日志读取线程仍在运行，不能启动第二个串口读取器。");
        }
        BufferedDeviceLog? sink = null;
        try
        {
            if (!string.IsNullOrWhiteSpace(_options.DeviceLogPath))
            {
                string template = _options.DeviceLogPath;
                string relativePath = template.Replace("{timestamp}", DateTime.Now.ToString("yyyyMMdd-HHmmss"),
                    StringComparison.OrdinalIgnoreCase);
                string path = Path.IsPathRooted(relativePath)
                    ? relativePath
                    : Path.Combine(AppContext.BaseDirectory, relativePath);
                string? directory = Path.GetDirectoryName(path);
                if (!string.IsNullOrEmpty(directory))
                {
                    Directory.CreateDirectory(directory);
                }
                LogFileRetention.Enforce(template, path, _options.DeviceLogRetentionCount);

                sink = new BufferedDeviceLog(CreateTraceWriter(path, fullLogging: true));
                Console.WriteLine(
                    $"设备日志已启用（后台完整保存，界面={(_logSettings.FullLoggingEnabled ? "完整" : "精简")}）：{path}");
            }
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or ArgumentException)
        {
            Console.Error.WriteLine($"设备日志创建失败：{exception.Message}");
        }
        _traceWriter = sink;
        _traceCancellation = new CancellationTokenSource();
        CancellationToken cancellation = _traceCancellation.Token;
        _traceTask = Task.Run(
            () => TraceDeviceOutput(port, portName, connectionEpoch, cancellation, sink),
            cancellation);
    }

    internal static StreamWriter CreateTraceWriter(string path, bool fullLogging)
    {
        FileStream stream = new(
            path,
            FileMode.Create,
            FileAccess.Write,
            FileShare.ReadWrite,
            bufferSize: 64 * 1024,
            FileOptions.SequentialScan);
        return new StreamWriter(stream, new UTF8Encoding(false))
        {
            // 显示模式不影响磁盘；后台写入器每500ms刷新。
            AutoFlush = false,
        };
    }

    private void TraceDeviceOutput(
        SerialPort port,
        string portName,
        long connectionEpoch,
        CancellationToken cancellation,
        BufferedDeviceLog? sink)
    {
        byte[] buffer = new byte[1024];
        char[] textBuffer = new char[2048];
        Decoder decoder = Encoding.UTF8.GetDecoder();
        DeviceOutputFrameScanner scanner = new();
        StringBuilder pending = new();
        try
        {
            RunDeviceTraceReadLoop(
                isOpen: () => port.IsOpen,
                bytesToRead: () => port.BytesToRead,
                read: port.Read,
                buffer,
                cancellation,
                (received, read) =>
                {
                    byte[] textBytes = scanner.Feed(
                        received.AsSpan(0, read),
                        frame => OnMainPortFrame(portName, connectionEpoch, frame));
                    if (textBytes.Length == 0)
                    {
                        return;
                    }

                    int charCount = decoder.GetChars(textBytes, 0, textBytes.Length, textBuffer, 0, flush: false);
                    pending.Append(textBuffer, 0, charCount);
                    while (true)
                    {
                        int newline = pending.ToString().IndexOf('\n');
                        if (newline < 0)
                        {
                            break;
                        }

                        string line = pending.ToString(0, newline).TrimEnd('\r');
                        pending.Remove(0, newline + 1);
                        if (!string.IsNullOrWhiteSpace(line))
                        {
                            WriteDeviceTraceLine(sink, portName, line);
                        }
                    }
                    if (pending.Length >= 65536)
                    {
                        WriteDeviceTraceLine(sink, portName, pending.ToString());
                        pending.Clear();
                    }
                });
        }
        catch (Exception exception) when (exception is IOException or InvalidOperationException or TimeoutException)
        {
            if (!cancellation.IsCancellationRequested)
            {
                Console.Error.WriteLine($"设备日志读取停止：{exception.Message}");
            }
        }
        finally
        {
            if (pending.Length > 0)
            {
                string line = pending.ToString().Trim();
                if (!string.IsNullOrWhiteSpace(line))
                {
                    WriteDeviceTraceLine(sink, portName, line);
                }
            }
        }
    }

    internal static void RunDeviceTraceReadLoop(
        Func<bool> isOpen,
        Func<int> bytesToRead,
        Func<byte[], int, int, int> read,
        byte[] buffer,
        CancellationToken cancellation,
        Action<byte[], int> processRead)
    {
        while (!cancellation.IsCancellationRequested)
        {
            if (!isOpen())
            {
                return;
            }

            int available = bytesToRead();
            int requested = available <= 0 ? 1 : Math.Min(buffer.Length, available);
            int readCount;
            try
            {
                readCount = read(buffer, 0, requested);
            }
            catch (TimeoutException)
            {
                // SerialPort.ReadTimeout 为 100 ms；每次超时后回到循环顶部检查取消令牌。
                continue;
            }

            if (readCount > 0)
            {
                processRead(buffer, readCount);
            }
        }
    }

    private void WriteDeviceTraceLine(BufferedDeviceLog? sink, string portName, string line)
    {
        string stamped = $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] [{portName}] {line}";
        sink?.TryWrite(stamped);
        if (ShouldMirrorDeviceLog(_logSettings.FullLoggingEnabled, line))
        {
            Console.WriteLine($"[设备] {line}");
        }
    }

    // 精简模式沿用原有行为：设备原始 ESP-IDF 行只进入文件，不镜像到主界面。
    // 这样启用完整落盘不会改变用户看到的精简日志内容。
    internal static bool ShouldMirrorDeviceLog(bool fullLogging, string line) =>
        fullLogging && IsEspIdfLogLine(line);

    internal static bool ShouldPersistDeviceLog(bool fullLogging, string line) => true;

    internal static bool ShouldDisplayDeviceLog(bool fullLogging, string line)
    {
        if (fullLogging)
        {
            return true;
        }

        if (line.StartsWith("E (", StringComparison.Ordinal) ||
            line.StartsWith("W (", StringComparison.Ordinal))
        {
            return true;
        }

        if (!line.StartsWith("I (", StringComparison.Ordinal))
        {
            return false;
        }

        foreach (string marker in ReducedLogMarkers)
        {
            if (line.Contains(marker, StringComparison.OrdinalIgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    private static bool IsEspIdfLogLine(string line) =>
        line.StartsWith("I (", StringComparison.Ordinal) ||
        line.StartsWith("W (", StringComparison.Ordinal) ||
        line.StartsWith("E (", StringComparison.Ordinal) ||
        line.StartsWith("D (", StringComparison.Ordinal) ||
        line.StartsWith("V (", StringComparison.Ordinal);

    private void RequestStopDeviceTrace()
    {
        _traceCancellation?.Cancel();
        _ = TryFinalizeStoppedTrace();
    }

    private async Task<bool> StopDeviceTraceAndConfirmAsync(CancellationToken cancellationToken)
    {
        CancellationTokenSource? cancellation = _traceCancellation;
        Task? task = _traceTask;
        cancellation?.Cancel();
        if (task is null)
        {
            FinalizeTraceResources(task, cancellation);
            return true;
        }
        try
        {
            await task.WaitAsync(
                TimeSpan.FromMilliseconds(TraceStopTimeoutMilliseconds),
                cancellationToken).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            Console.Error.WriteLine(
                "设备日志读取线程未在限定时间内退出；保留任务引用并阻止启动第二个读取器。");
            return false;
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            // Task.Run 可能在任务体开始前因取消而完成。
        }
        catch (Exception exception) when (exception is IOException or InvalidOperationException)
        {
            // TraceDeviceOutput 已处理并记录读取异常；此处只确认任务已结束。
        }
        FinalizeTraceResources(task, cancellation);
        return true;
    }

    private bool TryFinalizeStoppedTrace()
    {
        Task? task = _traceTask;
        if (task is null)
        {
            return true;
        }
        if (!task.IsCompleted)
        {
            return false;
        }
        _ = task.Exception;
        FinalizeTraceResources(task, _traceCancellation);
        return true;
    }

    private void FinalizeTraceResources(Task? task, CancellationTokenSource? cancellation)
    {
        if (task is not null && !task.IsCompleted)
        {
            return;
        }
        if (!ReferenceEquals(_traceTask, task))
        {
            return;
        }

        _traceTask = null;
        _traceCancellation = null;
        cancellation?.Dispose();
        try
        {
            _traceWriter?.Dispose();
        }
        catch (IOException)
        {
            // 设备日志关闭失败不应阻止串口恢复。
        }
        _traceWriter = null;
    }

    private SerialPort CreatePort(string portName, int baudRate) =>
        new(portName, baudRate, Parity.None, 8, StopBits.One)
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
        _outboundFrames.Clear();
        Monitor.PulseAll(_sync);
        Interlocked.Increment(ref _connectionEpoch);
        _buttonSubscriptionRequested = false;
        _buttonSubscriptionHealthy = false;
        _physicalButtons = 0;
        _physicalMouseValid = false;
        _physicalButtonStateTimestampQpc = 0;
        PublishButtonStateLocked(0);
        RequestStopDeviceTrace();
        lock (_writeSync)
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
                Volatile.Write(ref _connectedPortName, null);
                _port = null;
                _connectedLegacyCompatibility = false;
                _connectedBaudRate = A5BaudRate;
                Volatile.Write(ref _makcuModeEnabled, 0);
                _sessionStarted = false;
            }
        }
    }

    private void ClosePortForInputModeChange()
    {
        _outboundFrames.Clear();
        Monitor.PulseAll(_sync);
        Interlocked.Increment(ref _connectionEpoch);
        _buttonSubscriptionRequested = false;
        _buttonSubscriptionHealthy = false;
        _physicalButtons = 0;
        _physicalMouseValid = false;
        _physicalButtonStateTimestampQpc = 0;
        PublishButtonStateLocked(0);
        RequestStopDeviceTrace();
        lock (_writeSync)
        {
            SerialPort? oldPort = _port;
            try
            {
                if (oldPort?.IsOpen == true)
                {
                    WriteInputModeChangeReleaseFrames(
                        _codec,
                        frame => oldPort.Write(frame, 0, frame.Length));
                    Console.WriteLine("已在旧串口同步写入 SessionStart + ReleaseAll。");
                }
            }
            catch (Exception exception) when (
                exception is IOException or InvalidOperationException or UnauthorizedAccessException or
                    TimeoutException)
            {
                Console.Error.WriteLine($"切换单板/双板通路时旧串口 ReleaseAll 写入失败：{exception.Message}");
            }
            finally
            {
                try
                {
                    oldPort?.Dispose();
                }
                catch
                {
                    // 旧端口已失效时，清理状态仍必须继续。
                }
                Volatile.Write(ref _connectedPortName, null);
                _port = null;
                _connectedLegacyCompatibility = false;
                _connectedBaudRate = A5BaudRate;
                Volatile.Write(ref _makcuModeEnabled, 0);
                _sessionStarted = false;
            }
        }
    }

    internal static void WriteInputModeChangeReleaseFrames(FrameCodec codec, Action<byte[]> writeFrame)
    {
        ArgumentNullException.ThrowIfNull(codec);
        ArgumentNullException.ThrowIfNull(writeFrame);
        writeFrame(codec.Encode(MessageType.SessionStart, ReadOnlySpan<byte>.Empty));
        writeFrame(codec.Encode(MessageType.ReleaseAll, ReadOnlySpan<byte>.Empty));
    }

    internal string? GetConnectedPortName() => Volatile.Read(ref _connectedPortName);

    internal bool IsMakcuModeEnabled => Volatile.Read(ref _makcuModeEnabled) != 0;

    internal bool IsModeTransitioning => Volatile.Read(ref _modeTransitioning) != 0;

    internal int ConnectedBaudRate => Volatile.Read(ref _connectedBaudRate);

    internal bool IsA5ModeReadyForAutomation
    {
        get
        {
            lock (_sync)
            {
                return !_disposed && !_writerStopping && !_exclusivePortLeaseActive &&
                    !_legacyFirmwareCompatibility() && !_connectedLegacyCompatibility &&
                    !IsInputWriteGated && _connectedBaudRate == A5BaudRate &&
                    _port?.IsOpen == true && _traceTask is { IsCompleted: false };
            }
        }
    }

    internal bool IsMakcuModeSessionReady
    {
        get
        {
            lock (_sync)
            {
                return IsConfirmedMakcuModeSessionLocked();
            }
        }
    }

    private bool IsConfirmedMakcuModeSessionLocked() =>
        IsValidMakcuModeChangeConnection(
            disposed: _disposed,
            writerStopping: _writerStopping,
            exclusivePortLeaseActive: _exclusivePortLeaseActive,
            legacyFirmwareCompatibility: _legacyFirmwareCompatibility(),
            connectedLegacyCompatibility: _connectedLegacyCompatibility,
            modeTransitioning: Volatile.Read(ref _modeTransitioning) != 0,
            makcuModeEnabled: Volatile.Read(ref _makcuModeEnabled) == 1,
            connectedBaudRate: _connectedBaudRate,
            portOpen: _port?.IsOpen == true,
            traceReaderRunning: _traceTask is { IsCompleted: false });

    internal static bool IsValidMakcuModeChangeConnection(
        bool disposed,
        bool writerStopping,
        bool exclusivePortLeaseActive,
        bool legacyFirmwareCompatibility,
        bool connectedLegacyCompatibility,
        bool modeTransitioning,
        bool makcuModeEnabled,
        int connectedBaudRate,
        bool portOpen,
        bool traceReaderRunning) =>
        !disposed && !writerStopping && !exclusivePortLeaseActive &&
        !legacyFirmwareCompatibility && !connectedLegacyCompatibility &&
        !modeTransitioning && makcuModeEnabled &&
        connectedBaudRate == MakcuBaudRate && portOpen && traceReaderRunning;

    internal string ButtonStateMappingName => _buttonStatePublisher.MappingName;
    internal string ButtonStateMutexName => _buttonStatePublisher.MutexName;

    internal bool IsMouseHostAvailable
    {
        get
        {
            lock (_sync)
            {
                return !_disposed && !_writerStopping && !_exclusivePortLeaseActive &&
                    !_legacyFirmwareCompatibility() && !_connectedLegacyCompatibility &&
                    !IsInputWriteGated &&
                    _port?.IsOpen == true;
            }
        }
    }

    internal static string[] GetAvailablePortNames() =>
        SerialPort.GetPortNames()
            .OrderBy(GetPortNumber)
            .ThenBy(name => name, StringComparer.OrdinalIgnoreCase)
            .ToArray();

    /// <summary>
    /// 暂停控制串口连接，把该串口独占交给外围串口任务（固件刷写、板载日志转存等）。
    /// 调用方必须释放返回的租约，控制软件才会重新接管串口。
    /// </summary>
    /// <summary>
    /// 挂接对端板卡日志监听（2026-09-28），使其在独占任务期间一起让出串口。
    /// </summary>
    internal void AttachPeerLogMirror(DeviceLogMirror? mirror)
    {
        lock (_sync)
        {
            _peerLogMirror = mirror;
        }
    }

    internal ExclusivePortLease AcquireExclusivePort(string portName, string purpose)
    {
        ExclusivePortLease lease;
        DeviceLogMirror? mirrorToSuspend;
        lock (_sync)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (_exclusivePortLeaseActive)
            {
                throw new InvalidOperationException($"串口正被{_exclusivePortPurpose ?? "其它串口任务"}占用。");
            }
            if (Volatile.Read(ref _modeTransitioning) != 0)
            {
                throw new InvalidOperationException("M 板正在切换 Makcu 模式，当前不能让出串口。");
            }
            string selectedPort = NormalizeFirmwarePortName(portName);
            if (!GetAvailablePortNames().Contains(selectedPort, StringComparer.OrdinalIgnoreCase))
            {
                throw new IOException($"所选串口 {selectedPort} 当前不存在。");
            }

            _exclusivePortLeaseActive = true;
            _exclusivePortPurpose = purpose;
            ClosePort();
            Console.WriteLine($"已暂停控制串口连接，将 {selectedPort} 交给{purpose}独占使用。");

            /*
             * 对端日志监听占的是**另一个**串口，而那个端口很可能正是这次要刷的板子；
             * 让它一起让出，否则 esptool 打不开端口（2026-09-28 实测 P 板刷写失败）。
             * Suspend 会关端口并 join 线程、可能阻塞，所以拿到引用后到锁外执行；
             * 引用本身保留，释放租约时好让它 Resume 回来。
             */
            mirrorToSuspend = _peerLogMirror;
            lease = new ExclusivePortLease(this, selectedPort, purpose);
        }

        mirrorToSuspend?.Suspend();
        return lease;
    }

    internal static string NormalizeFirmwarePortName(string? portName)
    {
        string normalized = portName?.Trim().ToUpperInvariant() ?? string.Empty;
        if (normalized.Length <= 3 ||
            !normalized.StartsWith("COM", StringComparison.Ordinal) ||
            !int.TryParse(normalized.AsSpan(3), out int number) ||
            number <= 0)
        {
            throw new ArgumentException("刷写串口必须是 COM 加正整数，例如 COM3。", nameof(portName));
        }
        return $"COM{number}";
    }

    internal async Task<bool> WaitForConnectionAsync(TimeSpan timeout, CancellationToken cancellationToken)
    {
        DateTime deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            cancellationToken.ThrowIfCancellationRequested();
            lock (_sync)
            {
                if (!_disposed && !_exclusivePortLeaseActive &&
                    (IsConfirmedMakcuModeSessionLocked() || EnsureConnected()))
                {
                    return true;
                }
            }
            await Task.Delay(200, cancellationToken).ConfigureAwait(false);
        }
        return false;
    }

    private void ReleaseExclusivePort(string portName, string purpose)
    {
        DeviceLogMirror? mirror;
        lock (_sync)
        {
            if (!_exclusivePortLeaseActive)
            {
                return;
            }
            _exclusivePortLeaseActive = false;
            _exclusivePortPurpose = null;
            _nextConnectAttemptUtc = DateTime.MinValue;
            Console.WriteLine($"{purpose}已释放 {portName}，控制软件开始恢复连接。");
            mirror = _peerLogMirror;
        }

        /*
         * Resume 会重新探测端口、可能阻塞，放在锁外执行（2026-09-28）。
         * 关键：必须让它**重新走让路等待**（内部会先等 4 秒），否则它会抢在控制通道
         * 之前抓走端口——实测 15:52 就因此报"刷写成功，但控制软件未能在 15 秒内重新连接"。
         */
        mirror?.Resume(null);
    }

    public void Dispose()
    {
        _heartbeatTimer.Dispose();
        lock (_sync)
        {
            _disposed = true;
            _writerStopping = true;
            _outboundFrames.Clear();
            Monitor.PulseAll(_sync);
        }
        _outboundSignal.Set();
        _writerThread.Join(500);
        lock (_sync)
        {
            ClosePort();
        }
        _ = StopDeviceTraceAndConfirmAsync(CancellationToken.None).GetAwaiter().GetResult();
        _buttonStateRetryTimer.Dispose();
        _buttonStatePublisher.Dispose();
        _outboundSignal.Dispose();
    }

    /// <summary>串口独占租约：释放后控制软件立刻开始恢复连接。</summary>
    internal sealed class ExclusivePortLease : IDisposable
    {
        private SerialBridge? _owner;

        internal ExclusivePortLease(SerialBridge owner, string portName, string purpose)
        {
            _owner = owner;
            PortName = portName;
            Purpose = purpose;
        }

        internal string PortName { get; }

        internal string Purpose { get; }

        public void Dispose() =>
            Interlocked.Exchange(ref _owner, null)?.ReleaseExclusivePort(PortName, Purpose);
    }
}

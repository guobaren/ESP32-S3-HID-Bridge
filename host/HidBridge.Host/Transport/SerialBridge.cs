using System.Diagnostics;
using System.IO.Ports;
using System.Text;
using HidBridge.Protocol;

namespace HidBridge.Host.Transport;

internal sealed record SerialPortRefreshSnapshot(string[] AvailablePorts, string[] OpenPorts);

internal sealed class SerialBridge : IBridgeTransport
{
    private const int MaxOutboundFrames = 32;
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
    private readonly RuntimeLogSettings _logSettings;
    private readonly Func<bool> _legacyFirmwareCompatibility;
    private readonly FrameCodec _codec = new();
    private readonly object _sync = new();
    private readonly object _writeSync = new();
    private readonly Queue<byte[]> _outboundFrames = [];
    private readonly AutoResetEvent _outboundSignal = new(false);
    private readonly Thread _writerThread;
    private readonly System.Threading.Timer _heartbeatTimer;
    private SerialPort? _port;
    private string? _connectedPortName;
    private bool _connectedLegacyCompatibility;
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
        Func<bool>? legacyFirmwareCompatibility = null)
    {
        _options = options;
        _logSettings = logSettings;
        _legacyFirmwareCompatibility = legacyFirmwareCompatibility ?? (() => false);
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
            if (_disposed || _exclusivePortLeaseActive || !EnsureConnected())
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

    internal void SetDevicePeriodicStats(bool enabled)
    {
        Send(MessageType.LogStatsControlRequest, new[] { (byte)(enabled ? 1 : 0) });
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
            _port?.IsOpen != true)
        {
            return false;
        }

        _outboundFrames.Enqueue(frame);
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
                    (batch, frameCount) = DrainOutboundBatchLocked();
                    Monitor.PulseAll(_sync);
                }

                try
                {
                    lock (_writeSync)
                    {
                        if (!ReferenceEquals(_port, port) || !port.IsOpen)
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

    private (byte[] Batch, int FrameCount) DrainOutboundBatchLocked()
    {
        int frameCount = _outboundFrames.Count;
        int length = 0;
        foreach (byte[] frame in _outboundFrames)
        {
            length += frame.Length;
        }
        byte[] batch = new byte[length];
        int offset = 0;
        while (_outboundFrames.Count > 0)
        {
            byte[] frame = _outboundFrames.Dequeue();
            Buffer.BlockCopy(frame, 0, batch, offset, frame.Length);
            offset += frame.Length;
        }
        return (batch, frameCount);
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
        if (_exclusivePortLeaseActive)
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
            // 关闭并立即重新探测，避免用户必须拔插串口或等待断线。
            Console.WriteLine(
                $"旧版单板兼容模式已{(requestedLegacyCompatibility ? "启用" : "停用")}，正在重新连接串口。");
            ClosePort();
            _nextConnectAttemptUtc = DateTime.MinValue;
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
            SerialPort? candidate = null;
            try
            {
                candidate = CreatePort(portName);
                candidate.Open();
                bool legacyCompatibility = requestedLegacyCompatibility;
                byte? expectedRole = legacyCompatibility ? null : SerialDeviceProbe.MouseHostRole;
                bool probeMatched = legacyCompatibility ||
                    !automatic || SerialDeviceProbe.Probe(candidate, _codec, expectedRole);
                if (!probeMatched)
                {
                    Console.WriteLine($"{portName} 未返回 HID Bridge 握手，已忽略。");
                    candidate.Dispose();
                    continue;
                }

                if (legacyCompatibility)
                {
                    Console.WriteLine(
                        $"{portName} 已按旧版单板通路直接连接；跳过新角色握手，后续使用旧版传输。");
                }

                _port = candidate;
                _connectedLegacyCompatibility = legacyCompatibility;
                Volatile.Write(ref _connectedPortName, portName);
                StartDeviceTrace(candidate, portName);
                Console.WriteLine($"已连接 {portName}。");
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

    private void StartDeviceTrace(SerialPort port, string portName)
    {
        if (string.IsNullOrWhiteSpace(_options.DeviceLogPath))
        {
            return;
        }

        try
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

            BufferedDeviceLog sink = new(CreateTraceWriter(path, fullLogging: true));
            _traceWriter = sink;
            _traceCancellation = new CancellationTokenSource();
            CancellationToken cancellation = _traceCancellation.Token;
            _traceTask = Task.Run(() => TraceDeviceOutput(port, portName, cancellation, sink), cancellation);
            Console.WriteLine(
                $"设备日志已启用（后台完整保存，界面={(_logSettings.FullLoggingEnabled ? "完整" : "精简")}）：{path}");
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or ArgumentException)
        {
            Console.Error.WriteLine($"设备日志创建失败：{exception.Message}");
            _traceWriter = null;
        }
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

    private void TraceDeviceOutput(SerialPort port, string portName, CancellationToken cancellation, BufferedDeviceLog sink)
    {
        byte[] buffer = new byte[1024];
        char[] textBuffer = new char[2048];
        Decoder decoder = Encoding.UTF8.GetDecoder();
        StringBuilder pending = new();
        try
        {
            while (!cancellation.IsCancellationRequested)
            {
                int available;
                try
                {
                    available = port.IsOpen ? port.BytesToRead : 0;
                }
                catch (InvalidOperationException)
                {
                    break;
                }

                if (available <= 0)
                {
                    Thread.Sleep(10);
                    continue;
                }

                int read = port.Read(buffer, 0, Math.Min(buffer.Length, available));
                if (read <= 0)
                {
                    continue;
                }

                int charCount = decoder.GetChars(buffer, 0, read, textBuffer, 0, flush: false);
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
            }
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

    private void WriteDeviceTraceLine(BufferedDeviceLog sink, string portName, string line)
    {
        string stamped = $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] [{portName}] {line}";
        sink.TryWrite(stamped);
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

    private void StopDeviceTrace()
    {
        CancellationTokenSource? cancellation = _traceCancellation;
        _traceCancellation = null;
        if (cancellation is not null)
        {
            cancellation.Cancel();
        }

        try
        {
            _traceTask?.Wait(500);
        }
        catch (AggregateException)
        {
            // 端口关闭期间的读取异常不应阻止重连。
        }
        finally
        {
            _traceTask = null;
            cancellation?.Dispose();
            try
            {
                _traceWriter?.Dispose();
            }
            catch (IOException)
            {
                // 日志关闭失败不应阻止串口重连。
            }
            _traceWriter = null;
        }
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
        _outboundFrames.Clear();
        Monitor.PulseAll(_sync);
        StopDeviceTrace();
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
                _sessionStarted = false;
            }
        }
    }

    internal string? GetConnectedPortName() => Volatile.Read(ref _connectedPortName);

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
                if (!_disposed && !_exclusivePortLeaseActive && EnsureConnected())
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

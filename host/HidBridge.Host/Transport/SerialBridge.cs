using System.Diagnostics;
using System.IO.Ports;
using System.Text;
using HidBridge.Protocol;

namespace HidBridge.Host.Transport;

internal sealed class SerialBridge : IBridgeTransport
{
    private const int TraceFlushIntervalMilliseconds = 500;
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
    private bool _firmwareUpdateLeaseActive;
    private bool _writerStopping;
    private bool _disposed;
    private CancellationTokenSource? _traceCancellation;
    private Task? _traceTask;
    private StreamWriter? _traceWriter;
    private long _nextTraceFlushTimestamp;
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
            if (_disposed || _firmwareUpdateLeaseActive || !EnsureConnected())
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

    private bool EnqueueFrameLocked(byte[] frame)
    {
        bool backpressured = false;
        while (_outboundFrames.Count >= MaxOutboundFrames &&
               !_disposed && !_writerStopping && !_firmwareUpdateLeaseActive)
        {
            backpressured = true;
            Monitor.Wait(_sync, 10);
        }
        if (_disposed || _writerStopping || _firmwareUpdateLeaseActive ||
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
                    if (_port?.IsOpen != true || _firmwareUpdateLeaseActive)
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
        Console.WriteLine(
            $"EXE输出统计：MouseReport={_mouseFramesQueued} " +
            $"({_mouseFramesQueued / elapsedSeconds:F1} Hz)，串口写入={_batchWriteCount}，" +
            $"总帧={_batchedFrameCount}，" +
            $"平均帧/次={average:F2}，最大帧/次={_maxFramesPerBatch}，" +
            $"队列峰值={_outboundQueuePeak}/{MaxOutboundFrames}，反压={_backpressureCount}。");
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
        if (_firmwareUpdateLeaseActive)
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

            _traceWriter = CreateTraceWriter(path, _logSettings.FullLoggingEnabled);
            _nextTraceFlushTimestamp = Stopwatch.GetTimestamp() +
                                       Stopwatch.Frequency * TraceFlushIntervalMilliseconds / 1000;
            _traceCancellation = new CancellationTokenSource();
            CancellationToken cancellation = _traceCancellation.Token;
            _traceTask = Task.Run(() => TraceDeviceOutput(port, portName, cancellation), cancellation);
            Console.WriteLine(
                $"设备日志已启用（异步实时落盘，模式={(_logSettings.FullLoggingEnabled ? "完整诊断" : "精简高性能")}）：{path}");
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
            // 精简模式每秒只有少量关键行，立即刷新便于运行中排障；
            // 完整模式由专用串口日志线程周期刷新，避免每行强制刷新造成高频磁盘 I/O。
            AutoFlush = !fullLogging,
        };
    }

    private void TraceDeviceOutput(SerialPort port, string portName, CancellationToken cancellation)
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
                    FlushDeviceTraceIfDue();
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
                        WriteDeviceTraceLine(portName, line);
                    }
                }
                FlushDeviceTraceIfDue();
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
                    WriteDeviceTraceLine(portName, line);
                }
            }
            FlushDeviceTraceIfDue(force: true);
        }
    }

    private void WriteDeviceTraceLine(string portName, string line)
    {
        StreamWriter? writer = _traceWriter;
        if (writer is null)
        {
            return;
        }

        bool fullLogging = _logSettings.FullLoggingEnabled;
        if (!ShouldPersistDeviceLog(fullLogging, line))
        {
            return;
        }

        string stamped = $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] [{portName}] {line}";
        try
        {
            writer.WriteLine(stamped);
            if (ShouldMirrorDeviceLog(fullLogging, line))
            {
                Console.WriteLine($"[设备] {line}");
            }
        }
        catch (ObjectDisposedException)
        {
            // 关闭串口时允许日志线程退出。
        }
        catch (IOException)
        {
            // 日志文件不可用时不影响 HID 转发。
        }
    }


    internal static bool ShouldMirrorDeviceLog(bool fullLogging, string line) =>
        fullLogging && IsEspIdfLogLine(line);

    internal static bool ShouldPersistDeviceLog(bool fullLogging, string line)
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

    private void FlushDeviceTraceIfDue(bool force = false)
    {
        StreamWriter? writer = _traceWriter;
        if (writer is null)
        {
            return;
        }

        long now = Stopwatch.GetTimestamp();
        if (!force && now < _nextTraceFlushTimestamp)
        {
            return;
        }

        try
        {
            writer.Flush();
        }
        catch (ObjectDisposedException)
        {
            // 关闭串口时允许日志线程退出。
        }
        catch (IOException)
        {
            // 日志文件不可用时不影响 HID 转发。
        }
        finally
        {
            _nextTraceFlushTimestamp = now +
                                       Stopwatch.Frequency * TraceFlushIntervalMilliseconds / 1000;
        }
    }

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
                FlushDeviceTraceIfDue(force: true);
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

    internal FirmwareUpdatePortLease AcquireFirmwareUpdatePort(string portName)
    {
        lock (_sync)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (_firmwareUpdateLeaseActive)
            {
                throw new InvalidOperationException("固件刷写已经占用串口。");
            }
            string selectedPort = NormalizeFirmwarePortName(portName);
            if (!GetAvailablePortNames().Contains(selectedPort, StringComparer.OrdinalIgnoreCase))
            {
                throw new IOException($"所选刷写串口 {selectedPort} 当前不存在。");
            }

            _firmwareUpdateLeaseActive = true;
            ClosePort();
            Console.WriteLine($"已暂停控制串口连接，将 {selectedPort} 交给固件刷写任务独占使用。");
            return new FirmwareUpdatePortLease(this, selectedPort);
        }
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
                if (!_disposed && !_firmwareUpdateLeaseActive && EnsureConnected())
                {
                    return true;
                }
            }
            await Task.Delay(200, cancellationToken).ConfigureAwait(false);
        }
        return false;
    }

    private void ReleaseFirmwareUpdatePort(string portName)
    {
        lock (_sync)
        {
            if (!_firmwareUpdateLeaseActive)
            {
                return;
            }
            _firmwareUpdateLeaseActive = false;
            _nextConnectAttemptUtc = DateTime.MinValue;
            Console.WriteLine($"固件刷写已释放 {portName}，控制软件开始恢复连接。");
        }
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

    internal sealed class FirmwareUpdatePortLease : IDisposable
    {
        private SerialBridge? _owner;

        internal FirmwareUpdatePortLease(SerialBridge owner, string portName)
        {
            _owner = owner;
            PortName = portName;
        }

        internal string PortName { get; }

        public void Dispose() =>
            Interlocked.Exchange(ref _owner, null)?.ReleaseFirmwareUpdatePort(PortName);
    }
}

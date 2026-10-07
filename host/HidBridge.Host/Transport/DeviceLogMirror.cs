using System.IO.Ports;
using System.Text;
using System.Globalization;
using HidBridge.Protocol;

namespace HidBridge.Host.Transport;

/// <summary>
/// 对端板卡实时串口文本镜像与原始字节输入通道。
///
/// 背景：主串口（<see cref="SerialBridge"/> 用的那个）是双向控制通道，只连着一块板；
/// 另一块板的日志此前完全看不到。本类额外持有**另一个**串口，将实时文本写入独立文件，
/// 同时把统计快照帧送到请求方，**从不为单次统计另开端口**。打不开、掉线、读失败都只是少一路镜像。
///
/// 端口选择：排除主串口后逐个发送随机 DeviceProbe 挑战，只有返回有效签名、挑战和 PC 角色的
/// 串口才会被采用；发现过程不依赖周期日志，也不会误抓别的串口设备（例如 GPS、虚拟串口）。
/// </summary>
internal sealed class DeviceLogMirror : IDisposable
{
    private const int DiscoveryRetryMilliseconds = 2000;
    private const int MaximumCandidatesPerAttempt = 3;

    /// <summary>
    /// 启动前先让路：主控制通道（SerialBridge）是"自己扫描串口 + 握手"决定连哪块板的，
    /// 如果本监听抢在前面占掉它想用的端口，就会把主通道挤到另一块板上。这里先等一会儿，
    /// 让主通道把端口挑走，我们只捡剩下的那个。
    /// </summary>
    private const int StartupYieldMilliseconds = 4000;

    private const int ReadBufferBytes = 1024;

    private readonly int _baudRate;
    private readonly string _hostLogPathTemplate;
    private readonly int _retentionCount;
    private readonly object _sync = new();
    private readonly object _stateSync = new();
    private readonly SemaphoreSlim _discoveryGate = new(1, 1);
    private readonly AutoResetEvent _discoveryWake = new(false);
    private readonly CancellationTokenSource _lifetimeCancellation = new();
    private SerialPort? _port;
    private Thread? _thread;
    private Thread? _discoveryThread;
    private BufferedDeviceLog? _logWriter;
    private string? _logPath;
    private volatile bool _stop;
    private string? _portName;
    private Func<string?>? _primaryPortProvider;
    private readonly Action<string, BridgeFrame>? _diagnosticFrameReceived;
    private CancellationTokenSource? _activeDiscoveryCancellation;
    private bool _disposed;
    private bool _suspended;
    private DateTime _discoveryNotBeforeUtc;
    private int _candidateOffset;
    private int _readerFailed;

    /// <summary>
    /// 上次成功监听的端口（2026-09-28）：让出再恢复时优先回到它，避免抢占主控制通道的端口。
    /// </summary>
    private string? _preferredPort;

    internal DeviceLogMirror(
        int baudRate,
        string hostLogPathTemplate,
        int retentionCount,
        Func<string?>? primaryPortProvider = null,
        Action<string, BridgeFrame>? diagnosticFrameReceived = null)
    {
        _baudRate = baudRate;
        _hostLogPathTemplate = hostLogPathTemplate;
        _retentionCount = retentionCount;
        _primaryPortProvider = primaryPortProvider;
        _diagnosticFrameReceived = diagnosticFrameReceived;
    }

    internal string? PortName => GetOpenPortName();

    internal string? GetOpenPortName()
    {
        lock (_sync)
        {
            return !_stop && Volatile.Read(ref _readerFailed) == 0 && _port?.IsOpen == true ? _portName : null;
        }
    }

    /// <summary>
    /// 仅在镜像当前已经持有指定端口时写入；同一把锁与 Suspend/ReleasePort 串行化，
    /// 读线程仍继续从同一个 SerialPort 读取日志。
    /// </summary>
    internal int? WriteToOpenPort(string portName, byte[] bytes, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_sync)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (_stop || _port?.IsOpen != true ||
                !string.Equals(_portName, portName, StringComparison.OrdinalIgnoreCase))
            {
                return null;
            }

            _port.Write(bytes, 0, bytes.Length);
            return bytes.Length;
        }
    }

    internal static string BuildMirrorLogPath(
        string hostLogPathTemplate,
        string portName,
        DateTime timestamp,
        string? baseDirectory = null)
    {
        string hostLogPath = Path.IsPathRooted(hostLogPathTemplate)
            ? hostLogPathTemplate
            : Path.Combine(baseDirectory ?? AppContext.BaseDirectory, hostLogPathTemplate);
        string directory = Path.GetDirectoryName(Path.GetFullPath(hostLogPath)) ??
                           baseDirectory ?? AppContext.BaseDirectory;
        string timestampText = timestamp.ToString("yyyyMMdd-HHmmss-fff", CultureInfo.InvariantCulture);
        return Path.Combine(directory, $"host-serial-{portName}-{timestampText}.log");
    }

    internal static void WriteMirrorLogLine(StreamWriter writer, string portName, string line, DateTime timestamp)
    {
        writer.WriteLine(
            $"[{timestamp.ToString("yyyy-MM-dd HH:mm:ss.fff", CultureInfo.InvariantCulture)}] [{portName}] {line}");
    }

    private void StartMirrorLog(string portName)
    {
        string path = BuildMirrorLogPath(_hostLogPathTemplate, portName, DateTime.Now);
        string pathTemplate = Path.Combine(
            Path.GetDirectoryName(path) ?? AppContext.BaseDirectory,
            $"host-serial-{portName}-{{timestamp}}.log");
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path) ?? AppContext.BaseDirectory);
            LogFileRetention.Enforce(pathTemplate, path, _retentionCount);
            StreamWriter writer = new(
                new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite),
                new UTF8Encoding(encoderShouldEmitUTF8Identifier: false))
            {
                AutoFlush = false,
            };
            lock (_sync)
            {
                _logWriter = new BufferedDeviceLog(writer);
                _logPath = path;
            }
            Console.WriteLine($"对端串口日志独立保存：{path}");
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            Console.Error.WriteLine($"对端串口日志文件无法打开（{path}）：{exception.Message}；串口读取仍继续。");
        }
    }

    private void WriteMirrorLog(string portName, string line)
    {
        BufferedDeviceLog? sink;
        lock (_sync) { sink = _logWriter; }
        sink?.TryWrite($"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] [{portName}] {line}");
    }

    internal void Start()
    {
        lock (_stateSync)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (_discoveryThread is not null)
            {
                return;
            }
            _suspended = false;
            _stop = false;
            _discoveryNotBeforeUtc = DateTime.UtcNow.AddMilliseconds(StartupYieldMilliseconds);
            _discoveryThread = new Thread(DiscoveryLoop)
            {
                IsBackground = true,
                Name = "HidBridge.LogMirrorDiscovery",
                Priority = ThreadPriority.BelowNormal,
            };
            _discoveryThread.Start();
        }
    }

    internal static bool ShouldAttemptDiscovery(bool disposed, bool suspended, bool hasOpenPort, bool readerFailed) =>
        !disposed && !suspended && (!hasOpenPort || readerFailed);

    internal static string[] SelectDiscoveryCandidates(
        IEnumerable<string> candidates,
        string? primaryPort,
        string? preferredPort,
        int retryOffset)
    {
        string[] eligible = candidates
            .Where(name => !string.IsNullOrWhiteSpace(name) &&
                           !string.Equals(name, primaryPort, StringComparison.OrdinalIgnoreCase))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .OrderBy(name => name, StringComparer.OrdinalIgnoreCase)
            .ToArray();
        if (eligible.Length <= 1)
        {
            return eligible;
        }

        List<string> ordered = [];
        string? preferred = eligible.FirstOrDefault(name =>
            string.Equals(name, preferredPort, StringComparison.OrdinalIgnoreCase));
        if (preferred is not null)
        {
            ordered.Add(preferred);
        }
        string[] remaining = eligible
            .Where(name => !string.Equals(name, preferred, StringComparison.OrdinalIgnoreCase))
            .ToArray();
        if (remaining.Length > 0)
        {
            int start = Math.Abs(retryOffset % remaining.Length);
            for (int index = 0; index < remaining.Length; index++)
            {
                ordered.Add(remaining[(start + index) % remaining.Length]);
            }
        }
        return ordered.ToArray();
    }

    private void DiscoveryLoop()
    {
        bool startupDelay = true;
        while (true)
        {
            int waitMilliseconds = startupDelay ? StartupYieldMilliseconds : DiscoveryRetryMilliseconds;
            startupDelay = false;
            _discoveryWake.WaitOne(waitMilliseconds);

            bool disposed;
            bool suspended;
            DateTime notBefore;
            lock (_stateSync)
            {
                disposed = _disposed;
                suspended = _suspended;
                notBefore = _discoveryNotBeforeUtc;
            }
            if (disposed)
            {
                return;
            }
            if (suspended)
            {
                continue;
            }
            TimeSpan defer = notBefore - DateTime.UtcNow;
            if (defer > TimeSpan.Zero)
            {
                _discoveryWake.WaitOne(defer);
                lock (_stateSync)
                {
                    if (_disposed)
                    {
                        return;
                    }
                    if (_suspended || DateTime.UtcNow < _discoveryNotBeforeUtc)
                    {
                        continue;
                    }
                }
            }

            try
            {
                DiscoverOnce(CancellationToken.None);
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or InvalidOperationException)
            {
                Console.Error.WriteLine($"对端日志监听发现失败：{exception.Message}；稍后重试。");
            }
            catch (OperationCanceledException)
            {
                // Suspend、Dispose 或显式请求取消当前发现轮次。
            }
        }
    }

    internal void Refresh(CancellationToken cancellationToken)
    {
        DiscoverOnce(cancellationToken);
    }

    private bool DiscoverOnce(CancellationToken cancellationToken)
    {
        _discoveryGate.Wait(cancellationToken);
        try
        {
            CancellationTokenSource linkedCancellation = CancellationTokenSource.CreateLinkedTokenSource(
                cancellationToken,
                _lifetimeCancellation.Token);
            lock (_stateSync)
            {
                if (!ShouldAttemptDiscovery(_disposed, _suspended, HasOpenPort(), Volatile.Read(ref _readerFailed) != 0))
                {
                    linkedCancellation.Dispose();
                    return false;
                }
                _activeDiscoveryCancellation = linkedCancellation;
            }

            try
            {
                linkedCancellation.Token.ThrowIfCancellationRequested();
                if (Volatile.Read(ref _readerFailed) != 0)
                {
                    _stop = true;
                    ReleasePort();
                    _stop = false;
                    Volatile.Write(ref _readerFailed, 0);
                }

                string? primaryPort = _primaryPortProvider?.Invoke();
                if (string.IsNullOrWhiteSpace(primaryPort))
                {
                    return false;
                }

                string[] candidates;
                try
                {
                    candidates = SerialPort.GetPortNames();
                }
                catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
                {
                    Console.Error.WriteLine($"对端日志监听：枚举串口失败（{exception.Message}），稍后重试。");
                    return false;
                }

                string[] ordered = SelectDiscoveryCandidates(
                    candidates,
                    primaryPort,
                    _preferredPort,
                    _candidateOffset);
                int attempts = Math.Min(ordered.Length, MaximumCandidatesPerAttempt);
                for (int index = 0; index < attempts; index++)
                {
                    linkedCancellation.Token.ThrowIfCancellationRequested();
                    string candidate = ordered[index];
                    if (!TryOpenCandidate(candidate, linkedCancellation.Token, out SerialPort? port))
                    {
                        continue;
                    }

                    lock (_stateSync)
                    {
                        if (_disposed || _suspended || linkedCancellation.IsCancellationRequested)
                        {
                            port?.Dispose();
                            return false;
                        }
                    }
                    _preferredPort = candidate;
                    lock (_sync)
                    {
                        _portName = candidate;
                        _port = port;
                    }
                    Volatile.Write(ref _readerFailed, 0);
                    StartMirrorLog(candidate);
                    _thread = new Thread(ReadLoop)
                    {
                        IsBackground = true,
                        Name = $"HidBridge.LogMirror.{candidate}",
                        Priority = ThreadPriority.BelowNormal,
                    };
                    _thread.Start();
                    Console.WriteLine($"对端板卡日志监听已启动：{candidate}（主串口={primaryPort}）。");
                    return true;
                }

                if (ordered.Length > attempts)
                {
                    _candidateOffset = (_candidateOffset + attempts) % ordered.Length;
                }
                return false;
            }
            finally
            {
                lock (_stateSync)
                {
                    if (ReferenceEquals(_activeDiscoveryCancellation, linkedCancellation))
                    {
                        _activeDiscoveryCancellation = null;
                    }
                }
                linkedCancellation.Dispose();
            }
        }
        finally
        {
            _discoveryGate.Release();
        }
    }

    private bool HasOpenPort()
    {
        lock (_sync)
        {
            return _port?.IsOpen == true;
        }
    }

    /// <summary>通过带随机 nonce 的握手确认 P 角色；不依赖周期日志，也不改变串口控制线。</summary>
    private bool TryOpenCandidate(string name, CancellationToken cancellationToken, out SerialPort? port)
    {
        port = null;
        SerialPort candidate = new(name, _baudRate)
        {
            ReadTimeout = 200,
            WriteTimeout = 200,
            // 明确保持低电平：CH340 的 DTR/RTS 会拽住 ESP32 的复位/BOOT 脚。
            DtrEnable = false,
            RtsEnable = false,
            Handshake = Handshake.None,
        };
        try
        {
            candidate.Open();
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or InvalidOperationException)
        {
            CloseAndDisposeCandidate(candidate);
            return false;
        }

        try
        {
            if (!SerialDeviceProbe.Probe(candidate, new FrameCodec(), expectedRole: SerialDeviceProbe.PcDeviceRole))
            {
                CloseAndDisposeCandidate(candidate);
                return false;
            }

            cancellationToken.ThrowIfCancellationRequested();
            port = candidate;
            return true;
        }
        catch (OperationCanceledException)
        {
            CloseAndDisposeCandidate(candidate);
            throw;
        }
        catch (Exception exception) when (
            exception is IOException or InvalidOperationException or TimeoutException or
                UnauthorizedAccessException or ObjectDisposedException)
        {
            // 探测期间出错：按"不是目标板"处理。
        }

        CloseAndDisposeCandidate(candidate);
        return false;
    }

    private static void CloseAndDisposeCandidate(SerialPort candidate)
    {
        try
        {
            candidate.Close();
        }
        catch (Exception exception) when (exception is IOException or InvalidOperationException)
        {
            // 关闭失败无所谓，下面的 Dispose 兜底。
        }
        candidate.Dispose();
    }

    private void ReadLoop()
    {
        SerialPort? port;
        lock (_sync)
        {
            port = _port;
        }

        if (port is null)
        {
            return;
        }

        byte[] buffer = new byte[ReadBufferBytes];
        char[] text = new char[ReadBufferBytes * 2];
        Decoder decoder = Encoding.UTF8.GetDecoder();
        DeviceOutputFrameScanner scanner = new();
        StringBuilder pending = new();
        try
        {
            while (!_stop)
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

                byte[] textBytes = scanner.Feed(
                    buffer.AsSpan(0, read),
                    frame => _diagnosticFrameReceived?.Invoke(_portName ?? "?", frame));
                if (textBytes.Length == 0)
                {
                    continue;
                }

                int charCount = decoder.GetChars(textBytes, 0, textBytes.Length, text, 0, flush: false);
                pending.Append(text, 0, charCount);
                int newline;
                while ((newline = pending.ToString().IndexOf('\n')) >= 0)
                {
                    string line = pending.ToString(0, newline).TrimEnd('\r');
                    pending.Remove(0, newline + 1);
                    if (line.Length == 0 || line.Trim().Length == 0)
                    {
                        continue;
                    }

                    WriteMirrorLog(_portName ?? "?", line);
                }
                if (pending.Length >= 65536)
                {
                    WriteMirrorLog(_portName ?? "?", pending.ToString());
                    pending.Clear();
                }
            }
        }
        catch (Exception exception) when (
            exception is IOException or InvalidOperationException or TimeoutException or
                UnauthorizedAccessException or ObjectDisposedException)
        {
            if (!_stop)
            {
                Console.WriteLine($"对端板卡日志监听已停止：{_portName}（{exception.Message}）。");
            }
        }
        finally
        {
            if (pending.Length > 0)
            {
                WriteMirrorLog(_portName ?? "?", pending.ToString().TrimEnd('\r'));
            }
            if (!_stop)
            {
                Volatile.Write(ref _readerFailed, 1);
                _discoveryWake.Set();
            }
        }
    }

    /// <summary>
    /// 让出串口但**保持可恢复**（2026-09-28）：固件刷写等独占任务需要端口时调用。
    /// 与 <see cref="Dispose"/> 的区别是不置 `_stop` 语义上的终结态，随后可 Resume 重新接管。
    /// </summary>
    internal void Suspend()
    {
        lock (_stateSync)
        {
            if (_disposed)
            {
                return;
            }
            _suspended = true;
            _activeDiscoveryCancellation?.Cancel();
        }
        _discoveryWake.Set();
        _discoveryGate.Wait();
        try
        {
            ReleasePort();
        }
        finally
        {
            _discoveryGate.Release();
        }
    }

    /// <summary>独占任务结束后重新接管。</summary>
    internal void Resume(string? primaryPort)
    {
        lock (_stateSync)
        {
            if (_disposed)
            {
                return;
            }
            if (!string.IsNullOrWhiteSpace(primaryPort))
            {
                _primaryPortProvider = () => primaryPort;
            }
            _suspended = false;
            _stop = false;
            _discoveryNotBeforeUtc = DateTime.UtcNow.AddMilliseconds(StartupYieldMilliseconds);
        }
        _discoveryWake.Set();
    }

    private void ReleasePort()
    {
        _stop = true;
        Thread? thread = _thread;
        _thread = null;
        if (thread is not null && thread.IsAlive)
        {
            thread.Join(TimeSpan.FromSeconds(2));
        }

        lock (_sync)
        {
            try
            {
                if (_port?.IsOpen == true)
                {
                    _port.Close();
                }
            }
            catch (Exception exception) when (exception is IOException or InvalidOperationException)
            {
                // 关闭失败不影响后续。
            }
            _port?.Dispose();
            _port = null;
            _portName = null;
            try
            {
                _logWriter?.Dispose();
            }
            catch (Exception exception) when (
                exception is IOException or UnauthorizedAccessException or ObjectDisposedException)
            {
                Console.Error.WriteLine($"对端串口日志关闭失败（{_logPath}）：{exception.Message}");
            }
            _logWriter = null;
            _logPath = null;
        }
        // 注意：_preferredPort 故意保留——Resume 时要优先回到这个端口。
    }

    public void Dispose()
    {
        lock (_stateSync)
        {
            if (_disposed)
            {
                return;
            }
            _disposed = true;
            _suspended = true;
            _activeDiscoveryCancellation?.Cancel();
        }
        _lifetimeCancellation.Cancel();
        _discoveryWake.Set();
        _discoveryGate.Wait();
        try
        {
            ReleasePort();
        }
        finally
        {
            _discoveryGate.Release();
        }

        Thread? discoveryThread = _discoveryThread;
        _discoveryThread = null;
        if (discoveryThread is not null && discoveryThread != Thread.CurrentThread && discoveryThread.IsAlive)
        {
            discoveryThread.Join(TimeSpan.FromSeconds(2));
        }
    }
}

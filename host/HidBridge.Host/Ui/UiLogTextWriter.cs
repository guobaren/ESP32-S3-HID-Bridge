using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;

namespace HidBridge.Host.Ui;

internal sealed class UiLogTextWriter : TextWriter
{
    private const int MaximumQueuedLines = 4_096;
    private readonly object _lineSync = new();
    private readonly object _outputSync = new();
    private readonly StringBuilder _line = new();
    private readonly List<string> _pending = [];
    private readonly BlockingCollection<string> _lines =
        new(new ConcurrentQueue<string>(), MaximumQueuedLines);
    private readonly ManualResetEventSlim _idle = new(true);
    private readonly Thread _writerThread;
    private Action<string>? _sink;
    private StreamWriter? _fileWriter;
    private string? _filePath;
    private int _queuedLineCount;
    private long _droppedLineCount;
    private volatile bool _disposed;

    internal UiLogTextWriter()
    {
        _writerThread = new Thread(WriterLoop)
        {
            IsBackground = true,
            Name = "HidBridge.LogWriter",
            Priority = ThreadPriority.BelowNormal,
        };
        _writerThread.Start();
    }

    public override Encoding Encoding => Encoding.UTF8;

    internal string? FilePath
    {
        get
        {
            lock (_outputSync)
            {
                return _filePath;
            }
        }
    }

    internal void EnableFile(string pathTemplate)
    {
        if (string.IsNullOrWhiteSpace(pathTemplate))
        {
            throw new ArgumentException("日志文件路径不能为空。", nameof(pathTemplate));
        }

        string timestamp = DateTime.Now.ToString("yyyyMMdd-HHmmss", System.Globalization.CultureInfo.InvariantCulture);
        string path = pathTemplate.Replace("{timestamp}", timestamp, StringComparison.OrdinalIgnoreCase);
        if (!Path.IsPathRooted(path))
        {
            path = Path.Combine(AppContext.BaseDirectory, path);
        }
        path = Path.GetFullPath(path);
        string? directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        lock (_outputSync)
        {
            _fileWriter?.Dispose();
            _fileWriter = new StreamWriter(
                new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite),
                new UTF8Encoding(encoderShouldEmitUTF8Identifier: false))
            {
                AutoFlush = true,
            };
            _filePath = path;
        }
    }

    internal void Attach(Action<string> sink)
    {
        string[] pending;
        lock (_outputSync)
        {
            _sink = sink;
            pending = _pending.ToArray();
            _pending.Clear();
        }

        foreach (string line in pending)
        {
            sink(line);
        }
    }

    public override void Write(char value)
    {
        string? completed = null;
        lock (_lineSync)
        {
            if (_disposed || value == '\r')
            {
                return;
            }
            if (value == '\n')
            {
                completed = TakeLineLocked();
            }
            else
            {
                _line.Append(value);
            }
        }
        if (completed is not null)
        {
            EnqueueLine(completed);
        }
    }

    public override void Write(string? value)
    {
        if (string.IsNullOrEmpty(value))
        {
            return;
        }

        List<string> completed = [];
        lock (_lineSync)
        {
            if (_disposed)
            {
                return;
            }
            foreach (char character in value)
            {
                if (character == '\r')
                {
                    continue;
                }
                if (character == '\n')
                {
                    completed.Add(TakeLineLocked());
                }
                else
                {
                    _line.Append(character);
                }
            }
        }
        foreach (string line in completed)
        {
            EnqueueLine(line);
        }
    }

    public override void WriteLine(string? value)
    {
        Write(value);
        FlushLine();
    }

    public override void Flush()
    {
        _idle.Wait(TimeSpan.FromSeconds(5));
    }

    private void FlushLine()
    {
        string? completed = null;
        lock (_lineSync)
        {
            if (!_disposed)
            {
                completed = TakeLineLocked();
            }
        }
        if (completed is not null)
        {
            EnqueueLine(completed);
        }
    }

    private string TakeLineLocked()
    {
        string line = _line.ToString();
        _line.Clear();
        return line;
    }

    private void EnqueueLine(string line)
    {
        if (_disposed || _lines.IsAddingCompleted)
        {
            return;
        }

        _idle.Reset();
        Interlocked.Increment(ref _queuedLineCount);
        bool queued;
        try
        {
            queued = _lines.TryAdd(line);
        }
        catch (InvalidOperationException)
        {
            queued = false;
        }
        if (!queued)
        {
            Interlocked.Decrement(ref _queuedLineCount);
            Interlocked.Increment(ref _droppedLineCount);
            if (Volatile.Read(ref _queuedLineCount) == 0)
            {
                _idle.Set();
            }
        }
    }

    private void WriterLoop()
    {
        foreach (string line in _lines.GetConsumingEnumerable())
        {
            try
            {
                long dropped = Interlocked.Exchange(ref _droppedLineCount, 0);
                if (dropped > 0)
                {
                    PersistLine($"日志队列已满，丢弃 {dropped} 行低优先级日志。");
                }
                PersistLine(line);
            }
            catch (Exception exception)
            {
                Debug.WriteLine($"异步日志输出失败：{exception}");
            }
            finally
            {
                if (Interlocked.Decrement(ref _queuedLineCount) == 0)
                {
                    _idle.Set();
                }
            }
        }

        long finalDropped = Interlocked.Exchange(ref _droppedLineCount, 0);
        if (finalDropped > 0)
        {
            try
            {
                PersistLine($"日志队列已满，丢弃 {finalDropped} 行低优先级日志。");
            }
            catch (Exception exception)
            {
                Debug.WriteLine($"异步日志摘要输出失败：{exception}");
            }
        }
        _idle.Set();
    }

    private void PersistLine(string line)
    {
        Action<string>? sink;
        lock (_outputSync)
        {
            _fileWriter?.WriteLine($"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] {line}");
            sink = _sink;
            if (sink is null)
            {
                _pending.Add(line);
                return;
            }
        }
        sink(line);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing && !_disposed)
        {
            FlushLine();
            _disposed = true;
            _lines.CompleteAdding();
            _writerThread.Join(TimeSpan.FromSeconds(5));
            lock (_outputSync)
            {
                _fileWriter?.Dispose();
                _fileWriter = null;
            }
            _lines.Dispose();
            _idle.Dispose();
        }
        base.Dispose(disposing);
    }
}

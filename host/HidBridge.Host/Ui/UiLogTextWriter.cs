using System.Text;

namespace HidBridge.Host.Ui;

internal sealed class UiLogTextWriter : TextWriter
{
    private readonly object _sync = new();
    private readonly StringBuilder _line = new();
    private readonly List<string> _pending = [];
    private Action<string>? _sink;
    private StreamWriter? _fileWriter;
    private string? _filePath;

    public override Encoding Encoding => Encoding.UTF8;

    internal string? FilePath
    {
        get
        {
            lock (_sync)
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

        lock (_sync)
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
        lock (_sync)
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
        if (value == '\r')
        {
            return;
        }

        if (value == '\n')
        {
            FlushLine();
            return;
        }

        lock (_sync)
        {
            _line.Append(value);
        }
    }

    public override void Write(string? value)
    {
        if (string.IsNullOrEmpty(value))
        {
            return;
        }

        foreach (char character in value)
        {
            Write(character);
        }
    }

    public override void WriteLine(string? value)
    {
        Write(value);
        FlushLine();
    }

    private void FlushLine()
    {
        string line;
        Action<string>? sink;
        lock (_sync)
        {
            line = _line.ToString();
            _line.Clear();
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
        if (disposing)
        {
            lock (_sync)
            {
                _fileWriter?.Dispose();
                _fileWriter = null;
            }
        }
        base.Dispose(disposing);
    }
}

using System.Text;

namespace HidBridge.Host.Ui;

internal sealed class UiLogTextWriter : TextWriter
{
    private readonly object _sync = new();
    private readonly StringBuilder _line = new();
    private readonly List<string> _pending = [];
    private Action<string>? _sink;

    public override Encoding Encoding => Encoding.UTF8;

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
            sink = _sink;
            if (sink is null)
            {
                _pending.Add(line);
                return;
            }
        }

        sink(line);
    }
}

using System.Drawing;
using HidBridge.Host.Input;

namespace HidBridge.Host.Ui;

internal sealed class BridgeMainForm : Form
{
    private readonly InputForwarder _input;
    private readonly MouseCursorLock _cursorLock = new();
    private readonly MouseCaptureSurface _captureSurface;
    private readonly Label _statusLabel;
    private readonly TextBox _logTextBox;
    private readonly SplitContainer _split;
    private readonly List<string> _pendingLogs = [];
    private bool _closing;

    internal BridgeMainForm(InputForwarder input, string endpointDescription)
    {
        _input = input;
        Text = "ESP32-S3 HID Bridge - 同步已关闭";
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(760, 560);
        ClientSize = new Size(960, 720);
        Font = new Font("Microsoft YaHei UI", 10, FontStyle.Regular);

        _captureSurface = new MouseCaptureSurface();
        _statusLabel = new Label
        {
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
            AutoEllipsis = true,
            Text = "同步已关闭",
            ForeColor = Color.FromArgb(76, 88, 106),
        };

        Label endpointLabel = new()
        {
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleRight,
            AutoEllipsis = true,
            Text = endpointDescription,
            ForeColor = Color.FromArgb(92, 105, 124),
        };

        TableLayoutPanel shortcutBar = new()
        {
            Dock = DockStyle.Top,
            Height = 46,
            Padding = new Padding(14, 4, 14, 4),
            ColumnCount = 3,
        };
        shortcutBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 28));
        shortcutBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 42));
        shortcutBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 30));
        shortcutBar.Controls.Add(_statusLabel, 0, 0);
        shortcutBar.Controls.Add(new Label
        {
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleCenter,
            Text = "HOME：开启 / 关闭同步",
            ForeColor = Color.FromArgb(58, 72, 92),
        }, 1, 0);
        shortcutBar.Controls.Add(endpointLabel, 2, 0);

        TableLayoutPanel endingBar = new()
        {
            Dock = DockStyle.Bottom,
            Height = 34,
            Padding = new Padding(14, 0, 14, 4),
            ColumnCount = 2,
        };
        endingBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 55));
        endingBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 45));
        endingBar.Controls.Add(new Label
        {
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
            Text = "同步开启后，其他键鼠输入仅发送到对端",
            ForeColor = Color.FromArgb(92, 105, 124),
        }, 0, 0);
        endingBar.Controls.Add(new Label
        {
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleRight,
            Text = "END：结束程序",
            ForeColor = Color.FromArgb(176, 76, 76),
        }, 1, 0);

        Panel capturePanel = new() { Dock = DockStyle.Fill, BackColor = Color.FromArgb(24, 31, 42) };
        capturePanel.Controls.Add(_captureSurface);
        capturePanel.Controls.Add(endingBar);
        capturePanel.Controls.Add(shortcutBar);

        _logTextBox = new TextBox
        {
            Dock = DockStyle.Fill,
            Multiline = true,
            ReadOnly = true,
            ScrollBars = ScrollBars.Both,
            WordWrap = false,
            BackColor = Color.FromArgb(249, 250, 252),
            ForeColor = Color.FromArgb(38, 47, 61),
            Font = new Font("Cascadia Mono", 9, FontStyle.Regular),
            HideSelection = false,
        };
        Label logLabel = new()
        {
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
            Text = "运行日志（可选择并复制）",
            ForeColor = Color.FromArgb(58, 72, 92),
        };
        TableLayoutPanel logPanel = new()
        {
            Dock = DockStyle.Fill,
            RowCount = 2,
            Padding = new Padding(10, 7, 10, 10),
        };
        logPanel.RowStyles.Add(new RowStyle(SizeType.Absolute, 28));
        logPanel.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        logPanel.Controls.Add(logLabel, 0, 0);
        logPanel.Controls.Add(_logTextBox, 0, 1);

        _split = new SplitContainer
        {
            Dock = DockStyle.Fill,
            Orientation = Orientation.Horizontal,
            IsSplitterFixed = true,
            SplitterWidth = 5,
        };
        _split.Panel1.Controls.Add(capturePanel);
        _split.Panel2.Controls.Add(logPanel);
        Controls.Add(_split);
        ApplySplitLayout();

        _input.ForwardingChanged += InputOnForwardingChanged;
        _input.ExitRequested += InputOnExitRequested;
        Resize += (_, _) =>
        {
            ApplySplitLayout();
            if (_input.ForwardingEnabled)
            {
                ApplyCursorLock();
            }
        };
        FormClosing += (_, _) =>
        {
            _closing = true;
            _cursorLock.Release();
            _input.Stop();
        };
    }

    internal TextBox LogTextBox => _logTextBox;
    internal MouseCaptureSurface CaptureSurface => _captureSurface;
    internal SplitContainer MainSplit => _split;

    internal void AppendLog(string message)
    {
        string line = $"[{DateTime.Now:HH:mm:ss.fff}] {message}";
        if (!IsHandleCreated || IsDisposed)
        {
            lock (_pendingLogs)
            {
                _pendingLogs.Add(line);
            }
            return;
        }

        if (InvokeRequired)
        {
            try
            {
                BeginInvoke((Action)(() => AppendLogToTextBox(line)));
            }
            catch (InvalidOperationException)
            {
            }
            return;
        }

        AppendLogToTextBox(line);
    }

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        string[] pending;
        lock (_pendingLogs)
        {
            pending = _pendingLogs.ToArray();
            _pendingLogs.Clear();
        }
        foreach (string line in pending)
        {
            AppendLogToTextBox(line);
        }
    }

    private void AppendLogToTextBox(string line)
    {
        if (_logTextBox.IsDisposed)
        {
            return;
        }
        _logTextBox.AppendText(line + Environment.NewLine);
        _logTextBox.SelectionStart = _logTextBox.TextLength;
        _logTextBox.ScrollToCaret();
    }

    private void ApplySplitLayout()
    {
        int available = _split.ClientSize.Height - _split.SplitterWidth;
        if (available <= 0)
        {
            return;
        }

        _split.SplitterDistance = available / 2;
    }

    private void InputOnForwardingChanged(object? sender, bool enabled)
    {
        if (_closing || IsDisposed)
        {
            return;
        }
        if (InvokeRequired)
        {
            BeginInvoke((Action)(() => InputOnForwardingChanged(sender, enabled)));
            return;
        }

        _captureSurface.Forwarding = enabled;
        _statusLabel.Text = enabled ? "同步已开启" : "同步已关闭";
        _statusLabel.ForeColor = enabled ? Color.FromArgb(27, 139, 91) : Color.FromArgb(76, 88, 106);
        Text = enabled ? "ESP32-S3 HID Bridge - 同步已开启" : "ESP32-S3 HID Bridge - 同步已关闭";
        AppendLog(enabled ? "键鼠同步已开启，鼠标已锁定到上半区中心。" : "键鼠同步已关闭，本机输入已恢复。");

        if (enabled)
        {
            ApplyCursorLock();
        }
        else
        {
            _cursorLock.Release();
        }
    }

    private void InputOnExitRequested(object? sender, EventArgs e)
    {
        if (_closing || IsDisposed)
        {
            return;
        }
        if (InvokeRequired)
        {
            BeginInvoke((Action)(() => InputOnExitRequested(sender, e)));
            return;
        }
        Close();
    }

    private void ApplyCursorLock()
    {
        Rectangle screenBounds = _captureSurface.RectangleToScreen(_captureSurface.ClientRectangle);
        Point center = new(screenBounds.Left + screenBounds.Width / 2, screenBounds.Top + screenBounds.Height / 2);
        try
        {
            _cursorLock.LockAt(center);
        }
        catch (Exception exception)
        {
            AppendLog($"鼠标锁定失败：{exception.Message}");
            _input.DisableForwarding();
        }
    }
}

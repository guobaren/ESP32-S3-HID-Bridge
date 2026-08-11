using System.Drawing;
using System.Runtime.InteropServices;
using HidBridge.Host.Input;

namespace HidBridge.Host.Ui;

internal sealed class BridgeMainForm : Form
{
    private static readonly Color DeepSurfaceColor = Color.FromArgb(24, 31, 42);
    private static readonly Color PrimaryTextOnDeepSurface = Color.FromArgb(245, 248, 252);
    private static readonly Color SecondaryTextOnDeepSurface = Color.FromArgb(190, 202, 218);
    private static readonly Color SuccessTextOnDeepSurface = Color.FromArgb(85, 224, 164);
    private static readonly Color DangerTextOnDeepSurface = Color.FromArgb(255, 154, 154);

    private readonly InputForwarder _input;
    private readonly RuntimeLogSettings _logSettings;
    private readonly MouseCursorLock _cursorLock = new();
    private readonly MouseCaptureSurface _captureSurface;
    private readonly Label _statusLabel;
    private readonly TextBox _logTextBox;
    private readonly ComboBox _logModeComboBox;
    private readonly CheckBox _simulatedUdpCheckBox;
    private readonly ComboBox _simulatedUdpFrequencyComboBox;
    private readonly CheckBox _udpSmoothingCheckBox;
    private readonly System.Windows.Forms.Timer _logFlushTimer;
    private readonly SplitContainer _split;
    private readonly List<string> _pendingLogs = [];
    private const int EmGetFirstVisibleLine = 0x00CE;
    private const int EmLineScroll = 0x00B6;
    private const int MaxVisibleLogCharacters = 500_000;

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern int SendMessage(
        IntPtr hWnd,
        int message,
        IntPtr wParam,
        IntPtr lParam);

    private bool _closing;

    internal BridgeMainForm(
        InputForwarder input,
        string endpointDescription,
        RuntimeLogSettings? logSettings = null)
    {
        _input = input;
        _logSettings = logSettings ?? new RuntimeLogSettings(RuntimeLogMode.Reduced);
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
            ForeColor = SecondaryTextOnDeepSurface,
        };

        Label endpointLabel = new()
        {
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleRight,
            AutoEllipsis = true,
            Text = endpointDescription,
            ForeColor = SecondaryTextOnDeepSurface,
        };

        _simulatedUdpCheckBox = new CheckBox
        {
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleCenter,
            Text = "模拟 UDP",
            AccessibleName = "模拟 UDP 输入开关",
            BackColor = DeepSurfaceColor,
            ForeColor = PrimaryTextOnDeepSurface,
            Checked = false,
            UseVisualStyleBackColor = false,
        };
        _simulatedUdpFrequencyComboBox = new ComboBox
        {
            Dock = DockStyle.Fill,
            DropDownStyle = ComboBoxStyle.DropDownList,
            AccessibleName = "模拟 UDP 输入频率",
            FormattingEnabled = true,
            Enabled = false,
            BackColor = Color.FromArgb(249, 250, 252),
            ForeColor = Color.FromArgb(38, 47, 61),
        };
        _simulatedUdpFrequencyComboBox.Items.AddRange(
            SimulatedUdpMouseInput.SupportedFrequencies.Cast<object>().ToArray());
        _simulatedUdpFrequencyComboBox.Format += (_, eventArgs) =>
        {
            if (eventArgs.ListItem is int frequencyHz)
            {
                eventArgs.Value = FormatSimulatedUdpFrequency(frequencyHz);
            }
        };
        _simulatedUdpFrequencyComboBox.SelectedItem = 100;
        _udpSmoothingCheckBox = new CheckBox
        {
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleCenter,
            Text = "UDP 平滑",
            AccessibleName = "UDP 平滑功能开关",
            AccessibleDescription = "请先按 HOME 关闭同步，再切换 UDP 平滑。",
            Checked = true,
            BackColor = DeepSurfaceColor,
            ForeColor = PrimaryTextOnDeepSurface,
            UseVisualStyleBackColor = false,
        };
        _udpSmoothingCheckBox.CheckedChanged += (_, _) =>
        {
            _input.ConfigureUdpSmoothing(_udpSmoothingCheckBox.Checked);
            AppendLog(_udpSmoothingCheckBox.Checked
                ? "UDP 平滑已开启：真实和模拟 UDP 移动分摊到固定 10 个 2 ms 槽，最大计划尾部 20 ms。"
                : "UDP 平滑已关闭：真实和模拟 UDP 移动跳过低延迟分摊，直接进入 500 Hz 报告聚合。");
        };
        _simulatedUdpCheckBox.CheckedChanged += (_, _) =>
        {
            int frequencyHz = GetSelectedSimulatedUdpFrequency();
            _simulatedUdpFrequencyComboBox.Enabled = _simulatedUdpCheckBox.Checked;
            _input.ConfigureSimulatedUdpInput(_simulatedUdpCheckBox.Checked, frequencyHz);
            AppendLog(_simulatedUdpCheckBox.Checked
                ? $"模拟 UDP 输入已开启：源频率={FormatSimulatedUdpFrequency(frequencyHz)}；移动和滚轮进入 UDP 公共后续链路，按钮保持即时发送。"
                : "模拟 UDP 输入已关闭：实体鼠标恢复直接进入 500 Hz 聚合链路。");
        };
        _simulatedUdpFrequencyComboBox.SelectedIndexChanged += (_, _) =>
        {
            int frequencyHz = GetSelectedSimulatedUdpFrequency();
            _input.ConfigureSimulatedUdpInput(_simulatedUdpCheckBox.Checked, frequencyHz);
            if (_simulatedUdpCheckBox.Checked)
            {
                AppendLog($"模拟 UDP 输入频率已切换为 {FormatSimulatedUdpFrequency(frequencyHz)}。");
            }
        };

        TableLayoutPanel shortcutBar = new()
        {
            Dock = DockStyle.Top,
            Height = 46,
            Padding = new Padding(14, 4, 14, 4),
            ColumnCount = 6,
            BackColor = DeepSurfaceColor,
            ForeColor = PrimaryTextOnDeepSurface,
        };
        shortcutBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 18));
        shortcutBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 24));
        shortcutBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 14));
        shortcutBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 11));
        shortcutBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 13));
        shortcutBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 20));
        shortcutBar.Controls.Add(_statusLabel, 0, 0);
        shortcutBar.Controls.Add(new Label
        {
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleCenter,
            Text = "HOME：开启 / 关闭同步",
            ForeColor = PrimaryTextOnDeepSurface,
        }, 1, 0);
        shortcutBar.Controls.Add(_simulatedUdpCheckBox, 2, 0);
        shortcutBar.Controls.Add(_simulatedUdpFrequencyComboBox, 3, 0);
        shortcutBar.Controls.Add(_udpSmoothingCheckBox, 4, 0);
        shortcutBar.Controls.Add(endpointLabel, 5, 0);

        TableLayoutPanel endingBar = new()
        {
            Dock = DockStyle.Bottom,
            Height = 34,
            Padding = new Padding(14, 0, 14, 4),
            ColumnCount = 2,
            BackColor = DeepSurfaceColor,
            ForeColor = SecondaryTextOnDeepSurface,
        };
        endingBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 55));
        endingBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 45));
        endingBar.Controls.Add(new Label
        {
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
            Text = "左右键同按开始记录；全部松开 3 秒后生成分析图",
            ForeColor = SecondaryTextOnDeepSurface,
        }, 0, 0);
        endingBar.Controls.Add(new Label
        {
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleRight,
            Text = "END：结束程序",
            ForeColor = DangerTextOnDeepSurface,
        }, 1, 0);

        Panel capturePanel = new()
        {
            Dock = DockStyle.Fill,
            BackColor = DeepSurfaceColor,
            ForeColor = PrimaryTextOnDeepSurface,
        };
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
            Text = "运行日志（可选择、复制；滚动到上方后暂停自动跟随）",
            ForeColor = Color.FromArgb(58, 72, 92),
        };
        _logModeComboBox = new ComboBox
        {
            Dock = DockStyle.Fill,
            DropDownStyle = ComboBoxStyle.DropDownList,
            AccessibleName = "日志输出模式",
        };
        _logModeComboBox.Items.AddRange(["精简日志（高性能）", "完整日志（排障）"]);
        _logModeComboBox.SelectedIndex = _logSettings.FullLoggingEnabled ? 1 : 0;
        _logModeComboBox.SelectedIndexChanged += (_, _) =>
        {
            RuntimeLogMode mode = _logModeComboBox.SelectedIndex == 1
                ? RuntimeLogMode.Full
                : RuntimeLogMode.Reduced;
            if (_logSettings.SetMode(mode))
            {
                AppendLog(mode == RuntimeLogMode.Full
                    ? "日志模式已切换为完整诊断；设备原始日志将写入文件并镜像到窗口，可能降低高频输入性能。"
                    : "日志模式已切换为精简高性能；仅保留连接参数、统计、警告和错误等关键设备日志。");
            }
        };
        TableLayoutPanel logPanel = new()
        {
            Dock = DockStyle.Fill,
            RowCount = 2,
            ColumnCount = 2,
            Padding = new Padding(10, 7, 10, 10),
        };
        logPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 68));
        logPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 32));
        logPanel.RowStyles.Add(new RowStyle(SizeType.Absolute, 30));
        logPanel.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        logPanel.Controls.Add(logLabel, 0, 0);
        logPanel.Controls.Add(_logModeComboBox, 1, 0);
        logPanel.Controls.Add(_logTextBox, 0, 1);
        logPanel.SetColumnSpan(_logTextBox, 2);

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

        _logFlushTimer = new System.Windows.Forms.Timer { Interval = 50 };
        _logFlushTimer.Tick += (_, _) => FlushPendingLogs();
        _logFlushTimer.Start();

        _input.ForwardingChanged += InputOnForwardingChanged;
        _input.ExitRequested += InputOnExitRequested;
        _input.MovementRecordingStarted += InputOnMovementRecordingStarted;
        _input.MovementRecordingCompleted += InputOnMovementRecordingCompleted;
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
            _logFlushTimer.Stop();
            FlushPendingLogs();
            _cursorLock.Release();
            _input.MovementRecordingStarted -= InputOnMovementRecordingStarted;
            _input.MovementRecordingCompleted -= InputOnMovementRecordingCompleted;
            _input.Stop();
        };
    }

    internal TextBox LogTextBox => _logTextBox;
    internal ComboBox LogModeComboBox => _logModeComboBox;
    internal CheckBox SimulatedUdpCheckBox => _simulatedUdpCheckBox;
    internal ComboBox SimulatedUdpFrequencyComboBox => _simulatedUdpFrequencyComboBox;
    internal CheckBox UdpSmoothingCheckBox => _udpSmoothingCheckBox;
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
            lock (_pendingLogs)
            {
                _pendingLogs.Add(line);
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
        AppendLogBatch(pending);
    }

    private void FlushPendingLogs()
    {
        if (IsDisposed || !IsHandleCreated)
        {
            return;
        }

        string[] pending;
        lock (_pendingLogs)
        {
            if (_pendingLogs.Count == 0)
            {
                return;
            }
            pending = _pendingLogs.ToArray();
            _pendingLogs.Clear();
        }
        AppendLogBatch(pending);
    }

    private void AppendLogToTextBox(string line) => AppendLogBatch([line]);

    private void AppendLogBatch(IReadOnlyList<string> lines)
    {
        if (_logTextBox.IsDisposed || lines.Count == 0)
        {
            return;
        }

        bool followLatest = ShouldFollowLatestLog();
        int selectionStart = _logTextBox.SelectionStart;
        int selectionLength = _logTextBox.SelectionLength;
        int firstVisibleLine = GetFirstVisibleLine();
        _logTextBox.AppendText(string.Join(Environment.NewLine, lines) + Environment.NewLine);
        TrimVisibleLogIfNeeded(followLatest);

        if (followLatest)
        {
            _logTextBox.SelectionStart = _logTextBox.TextLength;
            _logTextBox.SelectionLength = 0;
            _logTextBox.ScrollToCaret();
            return;
        }

        // AppendText 可能自行把原生 TextBox 滚动到末尾；仅恢复选区仍会让用户
        // 看到的历史位置跳变，因此同时恢复 EM_GETFIRSTVISIBLELINE 对应的首行。
        int restoredStart = Math.Min(selectionStart, _logTextBox.TextLength);
        _logTextBox.SelectionStart = restoredStart;
        _logTextBox.SelectionLength = Math.Min(
            selectionLength,
            _logTextBox.TextLength - restoredStart);
        RestoreFirstVisibleLine(firstVisibleLine);
    }


    private void TrimVisibleLogIfNeeded(bool followLatest)
    {
        int excess = _logTextBox.TextLength - MaxVisibleLogCharacters;
        if (!followLatest || excess <= 0)
        {
            return;
        }

        int cutAt = _logTextBox.Text.IndexOf('\n', excess);
        if (cutAt < 0)
        {
            return;
        }

        _logTextBox.Select(0, cutAt + 1);
        _logTextBox.SelectedText = string.Empty;
    }

    private int GetFirstVisibleLine()
    {
        return SendMessage(
            _logTextBox.Handle,
            EmGetFirstVisibleLine,
            IntPtr.Zero,
            IntPtr.Zero);
    }

    private void RestoreFirstVisibleLine(int firstVisibleLine)
    {
        if (firstVisibleLine < 0)
        {
            return;
        }

        int currentFirstVisibleLine = GetFirstVisibleLine();
        int lineDelta = firstVisibleLine - currentFirstVisibleLine;
        if (lineDelta != 0)
        {
            SendMessage(
                _logTextBox.Handle,
                EmLineScroll,
                IntPtr.Zero,
                new IntPtr(lineDelta));
        }
    }

    private bool ShouldFollowLatestLog()
    {
        if (_logTextBox.SelectionLength > 0 || _logTextBox.TextLength == 0)
        {
            return _logTextBox.SelectionLength == 0;
        }

        int lastCharacterIndex = _logTextBox.TextLength - 1;
        Point lastCharacterPosition = _logTextBox.GetPositionFromCharIndex(lastCharacterIndex);
        int visibleBottom = _logTextBox.ClientSize.Height - _logTextBox.Font.Height + 4;
        return lastCharacterPosition.Y <= visibleBottom;
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
        _udpSmoothingCheckBox.Enabled = !enabled;
        _statusLabel.Text = enabled ? "同步已开启" : "同步已关闭";
        _statusLabel.ForeColor = enabled ? SuccessTextOnDeepSurface : SecondaryTextOnDeepSurface;
        Text = enabled ? "ESP32-S3 HID Bridge - 同步已开启" : "ESP32-S3 HID Bridge - 同步已关闭";
        AppendLog(enabled
            ? $"键鼠同步已开启，鼠标已锁定到上半区中心；UDP 平滑={(_input.UdpSmoothingEnabled ? "开启" : "关闭")}，切换前请先按 HOME 关闭同步。"
            : "键鼠同步已关闭，本机输入已恢复；现在可以切换 UDP 平滑。");

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

    private int GetSelectedSimulatedUdpFrequency() =>
        _simulatedUdpFrequencyComboBox.SelectedItem is int frequencyHz ? frequencyHz : 100;

    private static string FormatSimulatedUdpFrequency(int frequencyHz) =>
        frequencyHz == SimulatedUdpMouseInput.UnlimitedFrequencyHz
            ? "无上限"
            : $"{frequencyHz} Hz";

    private void InputOnMovementRecordingStarted()
    {
        if (_closing || IsDisposed)
        {
            return;
        }
        if (InvokeRequired)
        {
            BeginInvoke((Action)InputOnMovementRecordingStarted);
            return;
        }

        AppendLog("检测到鼠标左右键同时按下，开始记录实际发出的有符号 X/Y 移动命令。");
    }

    private void InputOnMovementRecordingCompleted(MouseMovementRecording recording)
    {
        if (_closing || IsDisposed)
        {
            return;
        }
        if (InvokeRequired)
        {
            BeginInvoke((Action)(() => InputOnMovementRecordingCompleted(recording)));
            return;
        }

        MouseMovementAnalysisForm analysisForm = new(recording);
        analysisForm.Show(this);
        AppendLog(
            $"左右键已松开超过 3 秒，鼠标移动记录完成：样本={recording.SampleCount}，" +
            (analysisForm.SavedImagePath is null
                ? "分析图已显示但保存失败。"
                : $"分析图={analysisForm.SavedImagePath}"));
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

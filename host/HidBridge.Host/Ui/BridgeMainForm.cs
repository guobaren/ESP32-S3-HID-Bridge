using System.Drawing;
using HidBridge.Host.Automation;
using HidBridge.Host.FirmwareUpdate;
using HidBridge.Host.Input;

namespace HidBridge.Host.Ui;

internal sealed class BridgeMainForm : Form
{
    private static readonly Color PageBackgroundColor = Color.FromArgb(242, 246, 251);
    private static readonly Color CardSurfaceColor = Color.White;
    private static readonly Color CardBorderColor = Color.FromArgb(211, 221, 234);
    private static readonly Color AccentColor = Color.FromArgb(48, 105, 232);
    private static readonly Color DeepSurfaceColor = CardSurfaceColor;
    private static readonly Color PrimaryTextOnDeepSurface = Color.FromArgb(23, 35, 58);
    private static readonly Color SecondaryTextOnDeepSurface = Color.FromArgb(82, 106, 139);
    private static readonly Color SuccessTextOnDeepSurface = Color.FromArgb(19, 128, 88);
    private static readonly Color DangerTextOnDeepSurface = Color.FromArgb(157, 91, 35);

    private readonly InputForwarder _input;
    private readonly AutomationController _automation;
    private readonly RuntimeLogSettings _logSettings;
    private readonly FirmwareUpdateApiServer? _firmwareUpdateApi;
    private readonly FirmwareFlashService? _firmwareFlash;
    private readonly MouseCursorLock _cursorLock = new();
    private readonly bool _enableCursorLock;
    private readonly MouseCaptureSurface _captureSurface;
    private readonly Label _statusLabel;
    private readonly Label _lanEndpointLabel;
    private readonly TextBox _logTextBox;
    private readonly ComboBox _logModeComboBox;
    private readonly CheckBox _udpSmoothingCheckBox;
    private readonly CheckBox _alwaysOutputUdpCheckBox;
    private readonly TrackBar _outputSensitivityTrackBar;
    private readonly TextBox _outputSensitivityTextBox;
    private readonly Label _outputSensitivityDescriptionLabel;
    private readonly System.Windows.Forms.Timer _logFlushTimer;
    private readonly SplitContainer _split;
    private readonly TabControl _tabs;
    private readonly MacroPageControl _macroPage;
    private readonly LuaPageControl _luaPage;
    private readonly SettingsPageControl _settingsPage;
    private readonly NotifyIcon _notifyIcon;
    private readonly List<string> _pendingLogs = [];
    private bool _updatingOutputSensitivity;
    private const int MaxVisibleLogCharacters = 500_000;

    private bool _closing;
    private bool _forceClose;

    internal BridgeMainForm(
        InputForwarder input,
        AutomationController automation,
        string endpointDescription,
        RuntimeLogSettings? logSettings = null,
        FirmwareUpdateApiServer? firmwareUpdateApi = null,
        FirmwareFlashService? firmwareFlash = null,
        bool enableCursorLock = true,
        string? lanEndpointDescription = null)
    {
        _input = input;
        _automation = automation;
        _enableCursorLock = enableCursorLock;
        double configuredOutputSensitivity = MouseOutputSensitivity.Clamp(automation.Settings.OutputSensitivity);
        _automation.Settings.OutputSensitivity = configuredOutputSensitivity;
        _input.ConfigureOutputSensitivity(configuredOutputSensitivity);
        _input.ConfigureAlwaysOutputUdp(automation.Settings.AlwaysOutputUdpEnabled);
        _logSettings = logSettings ?? new RuntimeLogSettings(RuntimeLogMode.Reduced);
        _firmwareUpdateApi = firmwareUpdateApi;
        _firmwareFlash = firmwareFlash;
        Text = "ESP32-S3 HID Bridge - 同步已关闭";
        Icon applicationIcon = LoadApplicationIcon();
        Icon = applicationIcon;
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(900, 640);
        ClientSize = new Size(
            Math.Max(1024, automation.Settings.WindowWidth),
            Math.Max(700, automation.Settings.WindowHeight));
        Font = new Font("Microsoft YaHei UI", 10, FontStyle.Regular);
        BackColor = PageBackgroundColor;

        _captureSurface = new MouseCaptureSurface();
        _statusLabel = new Label
        {
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
            AutoEllipsis = true,
            AutoSize = false,
            Margin = new Padding(0),
            Text = "同步已关闭",
            ForeColor = SecondaryTextOnDeepSurface,
        };

        _lanEndpointLabel = new Label
        {
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
            AutoEllipsis = true,
            AutoSize = false,
            Margin = new Padding(12, 0, 0, 0),
            Text = lanEndpointDescription ?? "局域网 UDP 未启用",
            ForeColor = SecondaryTextOnDeepSurface,
            AccessibleName = "局域网 UDP 监听地址",
        };

        Label endpointLabel = new()
        {
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleRight,
            AutoEllipsis = true,
            AutoSize = false,
            Margin = new Padding(0),
            Text = endpointDescription,
            ForeColor = SecondaryTextOnDeepSurface,
        };

        _udpSmoothingCheckBox = new CheckBox
        {
            AutoSize = true,
            Height = 34,
            Margin = new Padding(0, 0, 8, 0),
            TextAlign = ContentAlignment.MiddleLeft,
            Text = "UDP 平滑",
            AccessibleName = "UDP 平滑功能开关",
            AccessibleDescription = "请先按 HOME 关闭同步，再切换 UDP 平滑。",
            Checked = true,
            BackColor = CardSurfaceColor,
            ForeColor = PrimaryTextOnDeepSurface,
            UseVisualStyleBackColor = true,
        };
        _alwaysOutputUdpCheckBox = new CheckBox
        {
            AutoSize = true,
            Height = 34,
            Margin = new Padding(0),
            TextAlign = ContentAlignment.MiddleLeft,
            Text = "始终开启 UDP 输出",
            AccessibleName = "始终开启 UDP 输出",
            AccessibleDescription = "开启后，即使 HOME 关闭实体鼠标捕获，局域网 UDP 输入仍可发送到 ESP32；不会开启本机鼠标或键盘捕获。",
            Checked = automation.Settings.AlwaysOutputUdpEnabled,
            BackColor = CardSurfaceColor,
            ForeColor = PrimaryTextOnDeepSurface,
            UseVisualStyleBackColor = true,
        };
        _udpSmoothingCheckBox.CheckedChanged += (_, _) =>
        {
            _input.ConfigureUdpSmoothing(_udpSmoothingCheckBox.Checked);
            AppendLog(_udpSmoothingCheckBox.Checked
                ? "UDP 平滑已开启：真实和模拟 UDP 移动分摊到固定 20 个 1 ms 槽，最大计划尾部 20 ms。"
                : "UDP 平滑已关闭：真实和模拟 UDP 移动跳过低延迟分摊，直接进入 1000 Hz 报告聚合。");
        };
        _alwaysOutputUdpCheckBox.CheckedChanged += (_, _) =>
        {
            _automation.Settings.AlwaysOutputUdpEnabled = _alwaysOutputUdpCheckBox.Checked;
            _input.ConfigureAlwaysOutputUdp(_alwaysOutputUdpCheckBox.Checked);
            try
            {
                _automation.SaveSettings();
                AppendLog(_alwaysOutputUdpCheckBox.Checked
                    ? "始终开启 UDP 输出已开启：HOME 关闭时仍允许局域网 UDP 输入发送到 ESP32。"
                    : "始终开启 UDP 输出已关闭：局域网 UDP 输入仅在 HOME 同步开启时发送。");
            }
            catch (Exception exception)
            {
                AppendLog($"始终开启 UDP 输出设置保存失败：{exception.Message}");
            }
        };

        _outputSensitivityTrackBar = new TrackBar
        {
            Dock = DockStyle.Fill,
            AutoSize = false,
            Minimum = (int)(MouseOutputSensitivity.Minimum * MouseOutputSensitivity.TrackBarScale),
            Maximum = (int)(MouseOutputSensitivity.Maximum * MouseOutputSensitivity.TrackBarScale),
            TickFrequency = 30,
            TickStyle = TickStyle.BottomRight,
            Value = MouseOutputSensitivity.ToTrackBarValue(configuredOutputSensitivity),
            AccessibleName = "统一输出灵敏度",
            AccessibleDescription = "在发送到固件前按比例处理最终 X/Y 相对移动，范围 0.3 到 3.0。",
        };
        _outputSensitivityTextBox = new TextBox
        {
            Dock = DockStyle.Fill,
            TextAlign = HorizontalAlignment.Center,
            Text = MouseOutputSensitivity.Format(configuredOutputSensitivity),
            AccessibleName = "统一输出灵敏度数值",
            AccessibleDescription = "可直接输入 0.3 到 3.0 的灵敏度数值。",
            Margin = new Padding(0, 3, 0, 3),
        };
        _outputSensitivityTrackBar.ValueChanged += (_, _) =>
            ApplyOutputSensitivity(
                MouseOutputSensitivity.FromTrackBarValue(_outputSensitivityTrackBar.Value),
                persist: false);
        _outputSensitivityTrackBar.MouseUp += (_, _) => PersistOutputSensitivity();
        _outputSensitivityTrackBar.KeyUp += (_, _) => PersistOutputSensitivity();
        _outputSensitivityTextBox.KeyDown += (_, eventArgs) =>
        {
            if (eventArgs.KeyCode != Keys.Enter)
            {
                return;
            }
            CommitOutputSensitivityText();
            eventArgs.SuppressKeyPress = true;
            eventArgs.Handled = true;
        };
        _outputSensitivityTextBox.Leave += (_, _) => CommitOutputSensitivityText();

        TableLayoutPanel sensitivityControls = new()
        {
            Dock = DockStyle.Fill,
            ColumnCount = 3,
            RowCount = 1,
            Margin = new Padding(0),
            Padding = new Padding(0),
        };
        sensitivityControls.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 88));
        sensitivityControls.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        sensitivityControls.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 58));
        sensitivityControls.Controls.Add(new Label
        {
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.TopLeft,
            Padding = new Padding(0, 2, 0, 0),
            Text = "输出灵敏度",
            ForeColor = PrimaryTextOnDeepSurface,
        }, 0, 0);
        sensitivityControls.Controls.Add(_outputSensitivityTrackBar, 1, 0);
        sensitivityControls.Controls.Add(_outputSensitivityTextBox, 2, 0);
        _outputSensitivityDescriptionLabel = new Label
        {
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
            Text = "发送到固件前处理最终 X/Y 移动；覆盖实体鼠标、UDP、Lua、宏，滚轮和按键不变。",
            ForeColor = SecondaryTextOnDeepSurface,
            AutoEllipsis = true,
            AccessibleName = "统一输出灵敏度说明",
        };

        TableLayoutPanel shortcutBar = new()
        {
            Dock = DockStyle.Top,
            Height = 48,
            Padding = new Padding(14, 5, 14, 5),
            ColumnCount = 3,
            RowCount = 1,
            AutoSize = false,
            BackColor = CardSurfaceColor,
            ForeColor = PrimaryTextOnDeepSurface,
        };
        shortcutBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 30));
        shortcutBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 46));
        shortcutBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 24));
        shortcutBar.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        TableLayoutPanel statusBar = new()
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = 1,
            Margin = new Padding(0),
            Padding = new Padding(0),
        };
        statusBar.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 90));
        statusBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        statusBar.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        statusBar.Controls.Add(_statusLabel, 0, 0);
        statusBar.Controls.Add(_lanEndpointLabel, 1, 0);
        shortcutBar.Controls.Add(statusBar, 0, 0);
        // FlowDirection=RightToLeft 让两个开关在中间列整体靠右，同时保留平滑在左、始终输出在右。
        // 与原先“百分比空白列 + AutoSize 列”的嵌套 TableLayoutPanel 不同，FlowLayoutPanel 会按
        // 控件首选宽度布局，避免 DPI 或端点文本变化时把 CheckBox 的文字挤成不可见区域。
        FlowLayoutPanel udpOptionsBar = new()
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.RightToLeft,
            WrapContents = false,
            AutoSize = false,
            Margin = new Padding(0),
            Padding = new Padding(0),
        };
        _udpSmoothingCheckBox.Anchor = AnchorStyles.None;
        _alwaysOutputUdpCheckBox.Anchor = AnchorStyles.None;
        udpOptionsBar.Controls.Add(_alwaysOutputUdpCheckBox);
        udpOptionsBar.Controls.Add(_udpSmoothingCheckBox);
        shortcutBar.Controls.Add(udpOptionsBar, 1, 0);
        shortcutBar.Controls.Add(endpointLabel, 2, 0);

        TableLayoutPanel endingBar = new()
        {
            Dock = DockStyle.Bottom,
            Height = 70,
            Padding = new Padding(14, 7, 14, 5),
            RowCount = 2,
            ColumnCount = 2,
            BackColor = CardSurfaceColor,
            ForeColor = SecondaryTextOnDeepSurface,
        };
        endingBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 62));
        endingBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 38));
        endingBar.RowStyles.Add(new RowStyle(SizeType.Absolute, 30));
        endingBar.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        endingBar.Controls.Add(sensitivityControls, 0, 0);
        endingBar.Controls.Add(_outputSensitivityDescriptionLabel, 0, 1);
        Label endingShortcutLabel = new()
        {
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleRight,
            Text = "HOME：开启 / 关闭同步    END：结束程序",
            ForeColor = DangerTextOnDeepSurface,
        };
        endingBar.Controls.Add(endingShortcutLabel, 1, 0);
        endingBar.SetRowSpan(endingShortcutLabel, 2);

        Panel capturePanel = new()
        {
            Dock = DockStyle.Fill,
            BackColor = PageBackgroundColor,
            ForeColor = PrimaryTextOnDeepSurface,
            Padding = new Padding(14, 12, 14, 14),
        };
        Panel captureCard = new()
        {
            Dock = DockStyle.Fill,
            BackColor = CardSurfaceColor,
            Padding = new Padding(16),
        };
        captureCard.Paint += (_, eventArgs) =>
        {
            using Pen border = new(CardBorderColor);
            eventArgs.Graphics.DrawRectangle(border, 0, 0, Math.Max(0, captureCard.Width - 1), Math.Max(0, captureCard.Height - 1));
        };
        captureCard.Controls.Add(_captureSurface);
        captureCard.Controls.Add(endingBar);
        capturePanel.Controls.Add(captureCard);
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
            Padding = new Padding(14, 10, 14, 14),
            BackColor = PageBackgroundColor,
        };
        logPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 68));
        logPanel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 32));
        logPanel.RowStyles.Add(new RowStyle(SizeType.Absolute, 30));
        logPanel.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        logPanel.Controls.Add(logLabel, 0, 0);
        logPanel.Controls.Add(_logModeComboBox, 1, 0);
        logPanel.Controls.Add(_logTextBox, 0, 1);
        logPanel.SetColumnSpan(_logTextBox, 2);
        logPanel.Paint += (_, eventArgs) =>
        {
            using Pen border = new(CardBorderColor);
            eventArgs.Graphics.DrawRectangle(border, 0, 0, Math.Max(0, logPanel.Width - 1), Math.Max(0, logPanel.Height - 1));
        };

        _split = new SplitContainer
        {
            Dock = DockStyle.Fill,
            Orientation = Orientation.Horizontal,
            IsSplitterFixed = true,
            SplitterWidth = 5,
            BackColor = PageBackgroundColor,
        };
        _split.Panel1.BackColor = PageBackgroundColor;
        _split.Panel2.BackColor = PageBackgroundColor;
        _split.Panel1.Controls.Add(capturePanel);
        _split.Panel2.Controls.Add(logPanel);

        _macroPage = new MacroPageControl(_automation);
        _luaPage = new LuaPageControl(_automation);
        _settingsPage = new SettingsPageControl(
            _automation,
            firmwareUpdateApi,
            firmwareFlash,
            (enabled, frequencyHz) =>
            {
                _input.ConfigureSimulatedUdpInput(enabled, frequencyHz);
                AppendLog(enabled
                    ? $"模拟 UDP 输入已开启：源频率={FormatSimulatedUdpFrequency(frequencyHz)}；仅用于测试，移动和滚轮进入 UDP 公共后续链路。"
                    : "模拟 UDP 输入已关闭：测试源停止，实体鼠标恢复直接进入 1000 Hz 聚合链路。");
            });
        _tabs = new TabControl
        {
            Dock = DockStyle.Fill,
            Padding = Point.Empty,
            BackColor = CardSurfaceColor,
            ForeColor = PrimaryTextOnDeepSurface,
            DrawMode = TabDrawMode.OwnerDrawFixed,
            SizeMode = TabSizeMode.Fixed,
            ItemSize = new Size(96, 42),
        };
        TabPage captureTab = new("鼠标捕获");
        captureTab.BackColor = PageBackgroundColor;
        captureTab.UseVisualStyleBackColor = false;
        captureTab.Controls.Add(_split);
        TabPage macroTab = new("宏");
        macroTab.BackColor = PageBackgroundColor;
        macroTab.UseVisualStyleBackColor = false;
        macroTab.Controls.Add(_macroPage);
        TabPage luaTab = new("Lua");
        luaTab.BackColor = PageBackgroundColor;
        luaTab.UseVisualStyleBackColor = false;
        luaTab.Controls.Add(_luaPage);
        TabPage settingsTab = new("设置");
        settingsTab.BackColor = PageBackgroundColor;
        settingsTab.UseVisualStyleBackColor = false;
        settingsTab.Controls.Add(_settingsPage);
        _tabs.TabPages.AddRange([captureTab, macroTab, luaTab, settingsTab]);
        _tabs.DrawItem += (_, eventArgs) =>
        {
            bool selected = eventArgs.Index == _tabs.SelectedIndex;
            using Brush background = new SolidBrush(selected ? CardSurfaceColor : PageBackgroundColor);
            Color foregroundColor = selected ? AccentColor : SecondaryTextOnDeepSurface;
            eventArgs.Graphics.FillRectangle(background, eventArgs.Bounds);
            TextRenderer.DrawText(
                eventArgs.Graphics,
                _tabs.TabPages[eventArgs.Index].Text,
                Font,
                eventArgs.Bounds,
                foregroundColor,
                TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPadding);
            if (selected)
            {
                using Pen underline = new(AccentColor, 2);
                eventArgs.Graphics.DrawLine(
                    underline,
                    eventArgs.Bounds.Left + 12,
                    eventArgs.Bounds.Bottom - 2,
                    eventArgs.Bounds.Right - 12,
                    eventArgs.Bounds.Bottom - 2);
            }
        };
        _tabs.SelectedIndexChanged += (_, _) =>
        {
            if (_tabs.SelectedIndex == 3)
            {
                _settingsPage.ResetScrollPosition();
            }
            _tabs.Invalidate();
        };
        Controls.Add(_tabs);
        ApplySplitLayout();

        ContextMenuStrip trayMenu = new();
        trayMenu.Items.Add("显示主窗口", null, (_, _) => RestoreFromTray());
        trayMenu.Items.Add(new ToolStripSeparator());
        trayMenu.Items.Add("退出", null, (_, _) => ForceClose());
        _notifyIcon = new NotifyIcon
        {
            Icon = (Icon)applicationIcon.Clone(),
            Text = "ESP32-S3 HID Bridge",
            ContextMenuStrip = trayMenu,
            Visible = true,
        };
        _notifyIcon.DoubleClick += (_, _) => RestoreFromTray();

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
            if (_enableCursorLock && _input.ForwardingEnabled)
            {
                ApplyCursorLock();
            }
            if (WindowState == FormWindowState.Minimized && _automation.Settings.MinimizeToTray)
            {
                Hide();
            }
        };
        ResizeEnd += (_, _) => SaveWindowSize();
        FormClosing += OnFormClosing;
    }

    internal TextBox LogTextBox => _logTextBox;
    internal ComboBox LogModeComboBox => _logModeComboBox;
    internal CheckBox SimulatedUdpCheckBox => _settingsPage.SimulatedUdpCheckBox;
    internal ComboBox SimulatedUdpFrequencyComboBox => _settingsPage.SimulatedUdpFrequencyComboBox;
    internal CheckBox UdpSmoothingCheckBox => _udpSmoothingCheckBox;
    internal CheckBox AlwaysOutputUdpCheckBox => _alwaysOutputUdpCheckBox;
    internal Label SyncStatusLabel => _statusLabel;
    internal Label LanEndpointLabel => _lanEndpointLabel;
    internal TrackBar OutputSensitivityTrackBar => _outputSensitivityTrackBar;
    internal TextBox OutputSensitivityTextBox => _outputSensitivityTextBox;
    internal Label OutputSensitivityDescriptionLabel => _outputSensitivityDescriptionLabel;
    internal MouseCaptureSurface CaptureSurface => _captureSurface;
    internal SplitContainer MainSplit => _split;
    internal TabControl MainTabs => _tabs;
    internal MacroPageControl MacroPage => _macroPage;
    internal LuaPageControl LuaPage => _luaPage;
    internal SettingsPageControl SettingsPage => _settingsPage;
    internal NotifyIcon TrayIcon => _notifyIcon;
    internal void ProcessMovementRecordingForChecks(MouseMovementRecording recording) =>
        InputOnMovementRecordingCompleted(recording);

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
        LogTextBoxAppender.Append(_logTextBox, lines, MaxVisibleLogCharacters);
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
            ? $"键鼠同步已开启，鼠标已锁定到上半区中心；UDP 平滑={(_input.UdpSmoothingEnabled ? "开启" : "关闭")}，始终 UDP 输出={(_input.AlwaysOutputUdpEnabled ? "开启" : "关闭")}。"
            : $"键鼠同步已关闭，本机输入已恢复；UDP 平滑可切换，始终 UDP 输出={(_input.AlwaysOutputUdpEnabled ? "开启" : "关闭")}。");

        if (enabled && _enableCursorLock)
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
        ForceClose();
    }

    private void OnFormClosing(object? sender, FormClosingEventArgs eventArgs)
    {
        if (_firmwareUpdateApi?.FlashInProgress == true &&
            eventArgs.CloseReason is CloseReason.UserClosing or CloseReason.ApplicationExitCall)
        {
            eventArgs.Cancel = true;
            _forceClose = false;
            MessageBox.Show(
                this,
                "固件正在刷写，完成前不能退出控制软件。请通过状态接口确认任务结束。",
                "固件刷写进行中",
                MessageBoxButtons.OK,
                MessageBoxIcon.Warning);
            return;
        }
        if (!_forceClose && eventArgs.CloseReason == CloseReason.UserClosing && _automation.Settings.CloseToTray)
        {
            SaveWindowSize();
            eventArgs.Cancel = true;
            Hide();
            return;
        }

        SaveWindowSize();
        _closing = true;
        _notifyIcon.Visible = false;
        _logFlushTimer.Stop();
        FlushPendingLogs();
        _cursorLock.Release();
        _input.MovementRecordingStarted -= InputOnMovementRecordingStarted;
        _input.MovementRecordingCompleted -= InputOnMovementRecordingCompleted;
        _input.Stop();
    }

    private void SaveWindowSize()
    {
        if (IsDisposed || _closing)
        {
            return;
        }

        int nonClientWidth = Math.Max(0, Width - ClientSize.Width);
        int nonClientHeight = Math.Max(0, Height - ClientSize.Height);
        int width;
        int height;
        if (WindowState == FormWindowState.Normal)
        {
            width = ClientSize.Width;
            height = ClientSize.Height;
        }
        else
        {
            width = RestoreBounds.Width - nonClientWidth;
            height = RestoreBounds.Height - nonClientHeight;
        }

        if (width <= 0 || height <= 0)
        {
            return;
        }
        _automation.Settings.WindowWidth = Math.Max(MinimumSize.Width, width);
        _automation.Settings.WindowHeight = Math.Max(MinimumSize.Height, height);
        _automation.SaveSettings();
    }

    private void RestoreFromTray()
    {
        Show();
        WindowState = FormWindowState.Normal;
        Activate();
        BringToFront();
    }

    private void ForceClose()
    {
        _forceClose = true;
        Close();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _notifyIcon.Dispose();
        }
        base.Dispose(disposing);
    }

    private static Icon LoadApplicationIcon()
    {
        try
        {
            return Icon.ExtractAssociatedIcon(Application.ExecutablePath) ?? SystemIcons.Application;
        }
        catch
        {
            return SystemIcons.Application;
        }
    }

    private void CommitOutputSensitivityText()
    {
        if (!MouseOutputSensitivity.TryParse(_outputSensitivityTextBox.Text, out double value))
        {
            _outputSensitivityTextBox.Text = MouseOutputSensitivity.Format(_automation.Settings.OutputSensitivity);
            return;
        }

        ApplyOutputSensitivity(value, persist: true);
    }

    private void ApplyOutputSensitivity(double value, bool persist)
    {
        if (_closing || _updatingOutputSensitivity)
        {
            return;
        }

        double normalized = MouseOutputSensitivity.Clamp(value);
        _automation.Settings.OutputSensitivity = normalized;
        _input.ConfigureOutputSensitivity(normalized);
        _updatingOutputSensitivity = true;
        try
        {
            _outputSensitivityTrackBar.Value = MouseOutputSensitivity.ToTrackBarValue(normalized);
            _outputSensitivityTextBox.Text = MouseOutputSensitivity.Format(normalized);
        }
        finally
        {
            _updatingOutputSensitivity = false;
        }

        if (persist)
        {
            PersistOutputSensitivity();
        }
    }

    private void PersistOutputSensitivity()
    {
        if (_closing)
        {
            return;
        }

        try
        {
            _automation.SaveSettings();
        }
        catch (Exception exception)
        {
            AppendLog($"输出灵敏度保存失败：{exception.Message}");
        }
    }

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

        if (!_automation.Settings.GenerateMovementAnalysisImage)
        {
            AppendLog($"左右键移动记录完成：样本={recording.SampleCount}；设置已关闭，不生成按键情况分析图片。");
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
        if (!_enableCursorLock)
        {
            return;
        }

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

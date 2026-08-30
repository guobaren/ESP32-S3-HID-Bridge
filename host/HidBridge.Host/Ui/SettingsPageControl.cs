using HidBridge.Host.Automation;
using HidBridge.Host.FirmwareUpdate;
using HidBridge.Host.Input;
using HidBridge.Host.Transport;

namespace HidBridge.Host.Ui;

internal sealed class SettingsPageControl : UserControl
{
    private static readonly Color PageBackgroundColor = Color.FromArgb(242, 246, 251);
    private static readonly Color CardSurfaceColor = Color.White;
    private static readonly Color CardBorderColor = Color.FromArgb(211, 221, 234);
    private static readonly Color PrimaryTextColor = Color.FromArgb(23, 35, 58);
    private static readonly Color MutedTextColor = Color.FromArgb(82, 106, 139);
    private static readonly Color SuccessColor = Color.FromArgb(19, 128, 88);
    private static readonly Color ErrorColor = Color.FromArgb(170, 42, 42);
    private readonly AutomationController _controller;
    private readonly FirmwareUpdateApiServer? _firmwareUpdateApi;
    private readonly FirmwareFlashService? _firmwareFlash;
    private readonly CheckBox _startOnBootCheckBox;
    private readonly CheckBox _minimizeToTrayCheckBox;
    private readonly CheckBox _closeToTrayCheckBox;
    private readonly CheckBox _generateMovementAnalysisImageCheckBox;
    private readonly CheckBox _firmwareUpdateApiCheckBox;
    private readonly CheckBox _simulatedUdpCheckBox;
    private readonly ComboBox _simulatedUdpFrequencyComboBox;
    private readonly Action<bool, int>? _configureSimulatedUdp;
    private readonly Panel _cards;
    private TextBox _firmwareFileTextBox = null!;
    private ComboBox _firmwarePortComboBox = null!;
    private Button _refreshFirmwarePortsButton = null!;
    private Button _browseFirmwareButton = null!;
    private Button _confirmFlashButton = null!;
    private readonly Label _statusLabel;
    private bool _loading;

    internal SettingsPageControl(
        AutomationController controller,
        FirmwareUpdateApiServer? firmwareUpdateApi = null,
        FirmwareFlashService? firmwareFlash = null,
        Action<bool, int>? configureSimulatedUdp = null)
    {
        _controller = controller;
        _firmwareUpdateApi = firmwareUpdateApi;
        _firmwareFlash = firmwareFlash;
        _configureSimulatedUdp = configureSimulatedUdp;
        Dock = DockStyle.Fill;
        Padding = new Padding(18, 14, 18, 18);
        AutoScroll = false;
        BackColor = PageBackgroundColor;

        Label title = new()
        {
            Text = "程序行为",
            Dock = DockStyle.Fill,
            Font = new Font(Font.FontFamily, 15, FontStyle.Bold),
            ForeColor = PrimaryTextColor,
            TextAlign = ContentAlignment.MiddleLeft,
        };
        Label description = new()
        {
            Text = "设置会立即保存，并在下次启动时继续生效。托盘菜单中的“退出”始终会真正结束程序。",
            Dock = DockStyle.Fill,
            ForeColor = MutedTextColor,
            TextAlign = ContentAlignment.MiddleLeft,
        };

        _startOnBootCheckBox = CreateOption("开机启动", "登录当前 Windows 用户后自动启动本程序。");
        _minimizeToTrayCheckBox = CreateOption("最小化到托盘", "点击最小化后隐藏主窗口，双击托盘图标可恢复。");
        _closeToTrayCheckBox = CreateOption("关闭到托盘", "点击窗口关闭按钮时隐藏主窗口，不结束键鼠捕获和脚本运行。");
        _generateMovementAnalysisImageCheckBox = CreateOption(
            "生成按键情况分析图片",
            "默认关闭。左右键同时按下开始记录，全部松开 3 秒后完成；启用后弹出 X/Y 实际固件报告分析窗口，并将 PNG 保存到程序目录 log。仅用于观察输出，不改变转发逻辑。");
        _firmwareUpdateApiCheckBox = CreateOption(
            "启用局域网固件刷写接口",
            firmwareUpdateApi is null
                ? "当前不是串口模式，固件刷写接口不可用。"
                : $"监听局域网 TCP {firmwareUpdateApi.Port}；默认关闭，刷写时自动使用下方已选择的串口。仅限受信任局域网。");
        _firmwareUpdateApiCheckBox.Enabled = firmwareUpdateApi is not null;

        _simulatedUdpCheckBox = new CheckBox
        {
            Text = "启用模拟 UDP 输入（测试）",
            AutoSize = false,
            Width = 320,
            Height = 32,
            TextAlign = ContentAlignment.MiddleLeft,
            Padding = new Padding(4, 0, 0, 0),
            AccessibleName = "模拟 UDP 输入测试开关",
        };
        _simulatedUdpFrequencyComboBox = new ComboBox
        {
            DropDownStyle = ComboBoxStyle.DropDownList,
            Width = 150,
            Height = 30,
            FormattingEnabled = true,
            AccessibleName = "模拟 UDP 输入测试频率",
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

        FlowLayoutPanel startupOptions = new()
        {
            Dock = DockStyle.Fill,
            Height = 174,
            FlowDirection = FlowDirection.TopDown,
            WrapContents = false,
            Padding = new Padding(0, 8, 0, 0),
        };
        startupOptions.Controls.AddRange([
            WrapOption(_startOnBootCheckBox),
            WrapOption(_minimizeToTrayCheckBox),
            WrapOption(_closeToTrayCheckBox),
        ]);

        Panel startupPanel = CreateCardPanel(238);
        Label startupTitle = CreateSectionTitle("启动与托盘");
        startupTitle.Dock = DockStyle.None;
        startupTitle.Location = new Point(16, 10);
        startupTitle.Size = new Size(900, 30);
        startupTitle.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        startupOptions.Dock = DockStyle.None;
        startupOptions.Location = new Point(16, 44);
        startupOptions.Size = new Size(900, 178);
        startupOptions.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        startupPanel.Controls.Add(startupOptions);
        startupPanel.Controls.Add(startupTitle);
        startupPanel.Controls.SetChildIndex(startupTitle, 0);
        Panel flashPanel = BuildFlashPanel();
        Panel simulatedUdpPanel = BuildSimulatedUdpPanel();

        _statusLabel = new Label
        {
            Dock = DockStyle.Top,
            Height = 38,
            ForeColor = SuccessColor,
            TextAlign = ContentAlignment.MiddleLeft,
        };
        Panel saveBar = new()
        {
            Dock = DockStyle.Top,
            Height = 38,
            BackColor = CardSurfaceColor,
            Padding = new Padding(12, 4, 12, 4),
        };
        StyleCard(saveBar);
        saveBar.Controls.Add(_statusLabel);
        saveBar.Controls.Add(new Label
        {
            Text = "修改开关后立即写入配置",
            Dock = DockStyle.Right,
            Width = 190,
            TextAlign = ContentAlignment.MiddleRight,
            ForeColor = MutedTextColor,
        });

        TableLayoutPanel fixedHeader = new()
        {
            Dock = DockStyle.Top,
            Height = 116,
            ColumnCount = 1,
            RowCount = 3,
            Padding = new Padding(0),
            BackColor = PageBackgroundColor,
        };
        fixedHeader.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
        fixedHeader.RowStyles.Add(new RowStyle(SizeType.Absolute, 36));
        fixedHeader.RowStyles.Add(new RowStyle(SizeType.Absolute, 38));
        fixedHeader.Controls.Add(title, 0, 0);
        fixedHeader.Controls.Add(description, 0, 1);
        fixedHeader.Controls.Add(saveBar, 0, 2);

        _cards = new Panel
        {
            Dock = DockStyle.Fill,
            AutoScroll = true,
            BackColor = PageBackgroundColor,
            Padding = new Padding(0, 0, 0, 12),
        };
        _cards.Controls.Add(flashPanel);
        _cards.Controls.Add(simulatedUdpPanel);
        _cards.Controls.Add(startupPanel);
        Controls.Add(_cards);
        Controls.Add(fixedHeader);
        VisibleChanged += (_, _) =>
        {
            if (Visible)
            {
                ResetScrollPosition();
            }
        };

        _startOnBootCheckBox.CheckedChanged += (_, _) => SaveSettings();
        _minimizeToTrayCheckBox.CheckedChanged += (_, _) => SaveSettings();
        _closeToTrayCheckBox.CheckedChanged += (_, _) => SaveSettings();
        _generateMovementAnalysisImageCheckBox.CheckedChanged += (_, _) => SaveSettings();
        _firmwareUpdateApiCheckBox.CheckedChanged += (_, _) => SaveSettings();
        _simulatedUdpCheckBox.CheckedChanged += (_, _) => SaveSettings();
        _simulatedUdpFrequencyComboBox.SelectedIndexChanged += (_, _) => SaveSettings();
        LoadSettings();
    }

    internal CheckBox StartOnBootCheckBox => _startOnBootCheckBox;
    internal CheckBox MinimizeToTrayCheckBox => _minimizeToTrayCheckBox;
    internal CheckBox CloseToTrayCheckBox => _closeToTrayCheckBox;
    internal CheckBox GenerateMovementAnalysisImageCheckBox => _generateMovementAnalysisImageCheckBox;
    internal CheckBox FirmwareUpdateApiCheckBox => _firmwareUpdateApiCheckBox;
    internal CheckBox SimulatedUdpCheckBox => _simulatedUdpCheckBox;
    internal ComboBox SimulatedUdpFrequencyComboBox => _simulatedUdpFrequencyComboBox;
    internal ComboBox FirmwarePortComboBox => _firmwarePortComboBox;
    internal Button RefreshFirmwarePortsButton => _refreshFirmwarePortsButton;
    internal void ResetScrollPosition() => _cards.AutoScrollPosition = Point.Empty;

    private Panel BuildSimulatedUdpPanel()
    {
        Panel panel = new()
        {
            Dock = DockStyle.Top,
            Height = 212,
            Padding = new Padding(16),
            BackColor = CardSurfaceColor,
        };
        StyleCard(panel);

        Label title = CreateSectionTitle("分析与诊断");
        FlowLayoutPanel row = new()
        {
            Dock = DockStyle.Top,
            Width = 900,
            Height = 88,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = true,
            Padding = new Padding(0, 3, 0, 0),
        };
        row.Controls.Add(_simulatedUdpCheckBox);
        row.Controls.Add(new Label
        {
            Text = "源频率",
            AutoSize = true,
            Padding = new Padding(8, 7, 4, 0),
            ForeColor = MutedTextColor,
        });
        row.Controls.Add(_simulatedUdpFrequencyComboBox);
        Label simulatedUdpDescription = new()
        {
            Text = "仅用于测试输入聚合、UDP 平滑和输出链路，不代表真实网络性能。",
            AutoSize = false,
            Width = 300,
            Height = 56,
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = MutedTextColor,
            Margin = new Padding(8, 0, 0, 0),
        };
        row.Controls.Add(simulatedUdpDescription);
        row.Resize += (_, _) =>
        {
            int reservedWidth = _simulatedUdpCheckBox.Width + _simulatedUdpFrequencyComboBox.Width + 112;
            simulatedUdpDescription.Width = Math.Clamp(row.ClientSize.Width - reservedWidth, 180, 360);
        };
        FlowLayoutPanel rows = new()
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.TopDown,
            WrapContents = false,
            Padding = new Padding(0, 4, 0, 0),
        };
        rows.Controls.Add(WrapOption(_generateMovementAnalysisImageCheckBox));
        rows.Controls.Add(row);
        panel.Controls.Add(rows);
        panel.Controls.Add(title);
        return panel;
    }

    private Panel BuildFlashPanel()
    {
        Panel panel = new()
        {
            Dock = DockStyle.Top,
            Height = 357,
            Padding = new Padding(16),
            BackColor = CardSurfaceColor,
        };
        StyleCard(panel);

        Label sectionTitle = CreateSectionTitle("本地固件刷写");
        Label sectionDescription = new()
        {
            Text = "在本模块选择刷写串口和本机 JSON 清单；刷写直接使用所选 COM，不要求应用层握手。远程 API 自动使用这里保存的串口。",
            Dock = DockStyle.Top,
            Height = 44,
            ForeColor = MutedTextColor,
        };
        Panel apiOption = WrapOption(_firmwareUpdateApiCheckBox);
        _firmwarePortComboBox = new ComboBox
        {
            Dock = DockStyle.Fill,
            Height = 30,
            DropDownStyle = ComboBoxStyle.DropDown,
            FormattingEnabled = true,
            Enabled = _firmwareFlash is not null,
            AccessibleName = "固件刷写串口",
        };
        _firmwarePortComboBox.TextChanged += (_, _) =>
            _confirmFlashButton.Enabled = CanStartFlash();
        _refreshFirmwarePortsButton = new Button
        {
            Text = "刷新端口",
            AutoSize = true,
            Enabled = _firmwareFlash is not null,
            BackColor = Color.FromArgb(237, 243, 250),
            ForeColor = PrimaryTextColor,
            UseVisualStyleBackColor = false,
        };
        _refreshFirmwarePortsButton.Click += (_, _) => RefreshFirmwarePorts(showStatus: true);
        TableLayoutPanel portRow = new()
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = 1,
            Padding = new Padding(0),
        };
        portRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        portRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        portRow.Controls.Add(_firmwarePortComboBox, 0, 0);
        portRow.Controls.Add(_refreshFirmwarePortsButton, 1, 0);
        _firmwareFileTextBox = new TextBox
        {
            Dock = DockStyle.Top,
            Height = 30,
            ReadOnly = true,
            PlaceholderText = "尚未选择 JSON 刷写清单",
            Text = string.Empty,
            BackColor = Color.FromArgb(249, 251, 254),
            ForeColor = PrimaryTextColor,
        };
        _browseFirmwareButton = new Button
        {
            Text = "选择 JSON",
            AutoSize = true,
            Enabled = _firmwareFlash is not null,
            BackColor = Color.FromArgb(237, 243, 250),
            ForeColor = PrimaryTextColor,
            UseVisualStyleBackColor = false,
        };
        _browseFirmwareButton.Click += (_, _) => BrowseFirmwareFile();
        _confirmFlashButton = new Button
        {
            Text = "确定",
            AutoSize = true,
            Enabled = false,
            BackColor = Color.FromArgb(237, 243, 250),
            ForeColor = PrimaryTextColor,
            UseVisualStyleBackColor = false,
        };
        _confirmFlashButton.Click += (_, _) => ConfirmAndStartFlash();

        FlowLayoutPanel buttonRow = new()
        {
            Dock = DockStyle.Top,
            Height = 40,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            Padding = new Padding(0, 6, 0, 0),
        };
        buttonRow.Controls.Add(_browseFirmwareButton);
        buttonRow.Controls.Add(_confirmFlashButton);

        Label? unavailable = null;
        if (_firmwareFlash is null)
        {
            unavailable = new Label
            {
                Text = "当前不是串口模式，本地固件刷写不可用。",
                Dock = DockStyle.Fill,
                Height = 24,
                ForeColor = ErrorColor,
            };
        }

        Label manifestLabel = new()
        {
            Text = "选择本机 JSON 刷写清单",
            Dock = DockStyle.Fill,
            Height = 22,
            ForeColor = MutedTextColor,
        };
        Label portLabel = new()
        {
            Text = "刷写串口（可选择或输入，例如 COM3）",
            Dock = DockStyle.Fill,
            Height = 22,
            ForeColor = MutedTextColor,
        };
        TableLayoutPanel content = new()
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = unavailable is null ? 8 : 9,
            Padding = new Padding(0),
        };
        content.RowStyles.Add(new RowStyle(SizeType.Absolute, 30));
        content.RowStyles.Add(new RowStyle(SizeType.Absolute, 44));
        content.RowStyles.Add(new RowStyle(SizeType.Absolute, 56));
        content.RowStyles.Add(new RowStyle(SizeType.Absolute, 22));
        content.RowStyles.Add(new RowStyle(SizeType.Absolute, 30));
        content.RowStyles.Add(new RowStyle(SizeType.Absolute, 22));
        content.RowStyles.Add(new RowStyle(SizeType.Absolute, 30));
        content.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
        if (unavailable is not null)
        {
            content.RowStyles.Add(new RowStyle(SizeType.Absolute, 24));
        }
        content.Controls.Add(sectionTitle, 0, 0);
        content.Controls.Add(sectionDescription, 0, 1);
        content.Controls.Add(apiOption, 0, 2);
        content.Controls.Add(portLabel, 0, 3);
        content.Controls.Add(portRow, 0, 4);
        content.Controls.Add(manifestLabel, 0, 5);
        content.Controls.Add(_firmwareFileTextBox, 0, 6);
        content.Controls.Add(buttonRow, 0, 7);
        if (unavailable is not null)
        {
            content.Controls.Add(unavailable, 0, 8);
        }
        panel.Controls.Add(content);
        return panel;
    }

    private void BrowseFirmwareFile()
    {
        using OpenFileDialog dialog = new()
        {
            Title = "选择 JSON 刷写清单",
            Filter = "JSON 刷写清单 (*.json)|*.json",
            CheckFileExists = true,
        };
        if (dialog.ShowDialog(this) != DialogResult.OK)
        {
            return;
        }
        try
        {
            _ = FirmwareFlashPlan.LoadFromManifest(dialog.FileName);
            _controller.Settings.FirmwareManifestPath = Path.GetFullPath(dialog.FileName);
            _controller.SaveSettings();
            _firmwareFileTextBox.Text = _controller.Settings.FirmwareManifestPath;
            _confirmFlashButton.Enabled = CanStartFlash();
            _statusLabel.ForeColor = Color.FromArgb(34, 125, 70);
            _statusLabel.Text = "已保存固件 JSON 清单";
        }
        catch (Exception exception)
        {
            MessageBox.Show(this, $"JSON 刷写清单无效：{exception.Message}", "固件刷写", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private void ConfirmAndStartFlash()
    {
        if (_firmwareFlash is null)
        {
            return;
        }
        string file = _firmwareFileTextBox.Text.Trim();
        if (string.IsNullOrWhiteSpace(file) || !File.Exists(file))
        {
            MessageBox.Show(this, "请先选择有效的 JSON 刷写清单。", "固件刷写", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        string portName;
        try
        {
            portName = SerialBridge.NormalizeFirmwarePortName(_firmwarePortComboBox.Text);
        }
        catch (ArgumentException exception)
        {
            MessageBox.Show(this, exception.Message, "固件刷写", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        string[] availablePorts = _firmwareFlash.GetAvailablePortNames();
        if (!availablePorts.Contains(portName, StringComparer.OrdinalIgnoreCase))
        {
            MessageBox.Show(this, $"所选串口 {portName} 当前不存在，请刷新端口后重试。", "固件刷写", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        if (_firmwareFlash.GetSnapshot().State == "running")
        {
            MessageBox.Show(this, "已有刷写任务正在进行，请等待其结束后再试。", "固件刷写", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        string message = BuildFirmwareFlashConfirmation(file, portName, availablePorts);
        DialogResult confirm = MessageBox.Show(
            this,
            message,
            "确认刷写固件",
            MessageBoxButtons.YesNo,
            MessageBoxIcon.Warning);
        if (confirm != DialogResult.Yes)
        {
            return;
        }

        _controller.Settings.FirmwareFlashPortName = portName;
        _controller.SaveSettings();
        if (!_firmwareFlash.TryStartFromManifest(file, portName, out FirmwareFlashSnapshot snapshot))
        {
            MessageBox.Show(this, snapshot.Message, "固件刷写", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        new FirmwareFlashDialog(_firmwareFlash).Show(this);
    }

    private bool CanStartFlash() =>
        _firmwareFlash is not null &&
        File.Exists(_firmwareFileTextBox.Text.Trim()) &&
        !string.IsNullOrWhiteSpace(_firmwarePortComboBox.Text);

    private void RefreshFirmwarePorts(bool showStatus)
    {
        if (_firmwareFlash is null)
        {
            return;
        }
        string previous = _firmwarePortComboBox.Text.Trim();
        string[] ports = _firmwareFlash.GetAvailablePortNames();
        string? connected = _firmwareFlash.GetConnectedPortName();
        _firmwarePortComboBox.BeginUpdate();
        try
        {
            _firmwarePortComboBox.Items.Clear();
            _firmwarePortComboBox.Items.AddRange(ports.Cast<object>().ToArray());
            _firmwarePortComboBox.Text = SelectFirmwarePort(previous, connected, ports);
        }
        finally
        {
            _firmwarePortComboBox.EndUpdate();
        }
        _confirmFlashButton.Enabled = CanStartFlash();
        if (showStatus)
        {
            _statusLabel.ForeColor = ports.Length == 0 ? ErrorColor : SuccessColor;
            _statusLabel.Text = ports.Length switch
            {
                0 => "未检测到可用串口",
                1 => $"检测到串口 {ports[0]}",
                _ => $"检测到 {ports.Length} 个串口，请确认当前选择",
            };
        }
    }

    internal static string SelectFirmwarePort(
        string? previous,
        string? connected,
        IReadOnlyList<string> availablePorts)
    {
        string? connectedAvailable = availablePorts.FirstOrDefault(
            port => port.Equals(connected, StringComparison.OrdinalIgnoreCase));
        if (connectedAvailable is not null)
        {
            return connectedAvailable;
        }
        string? previousAvailable = availablePorts.FirstOrDefault(
            port => port.Equals(previous, StringComparison.OrdinalIgnoreCase));
        return previousAvailable ?? availablePorts.FirstOrDefault() ?? string.Empty;
    }

    internal static string BuildFirmwareFlashConfirmation(
        string manifestPath,
        string portName,
        IReadOnlyList<string> availablePorts)
    {
        string multiplePortsWarning = availablePorts.Count > 1
            ? "检测到多个串口：" + string.Join("、", availablePorts) + Environment.NewLine
              + $"本次只会刷写已选择的 {portName}，请确认没有选错设备。" + Environment.NewLine + Environment.NewLine
            : string.Empty;
        return "即将刷写固件到开发板：" + Environment.NewLine
            + $"串口：{portName}" + Environment.NewLine
            + $"清单：{manifestPath}" + Environment.NewLine + Environment.NewLine
            + multiplePortsWarning
            + "请确认开发板已连接；刷写期间将暂停键鼠同步转发。" + Environment.NewLine
            + "确定开始刷写吗？";
    }

    private static CheckBox CreateOption(string title, string description, int height = 50) => new()
    {
        Text = title,
        Tag = description,
        AutoSize = false,
        Width = 260,
        Height = height,
        TextAlign = ContentAlignment.MiddleLeft,
        Padding = new Padding(0),
        ForeColor = PrimaryTextColor,
    };

    private static Panel WrapOption(CheckBox option)
    {
        Panel panel = new()
        {
            Width = 900,
            Height = option.Height + 6,
            Margin = new Padding(0, 0, 0, 0),
            BackColor = CardSurfaceColor,
        };
        TableLayoutPanel row = new()
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = 1,
            Padding = new Padding(0),
        };
        row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 32));
        row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 68));
        option.Dock = DockStyle.Fill;
        row.Controls.Add(option, 0, 0);
        row.Controls.Add(new Label
        {
            Text = option.Tag as string ?? string.Empty,
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = MutedTextColor,
            AutoEllipsis = true,
        }, 1, 0);
        panel.Controls.Add(row);
        return panel;
    }

    private static Label CreateSectionTitle(string text) => new()
    {
        Text = text,
        Dock = DockStyle.Top,
        Height = 30,
        AutoSize = false,
        Font = new Font("Microsoft YaHei UI", 12, FontStyle.Bold),
        ForeColor = PrimaryTextColor,
    };

    private static Panel CreateCardPanel(int height)
    {
        Panel panel = new()
        {
            Dock = DockStyle.Top,
            Height = height,
            Padding = new Padding(16),
            BackColor = CardSurfaceColor,
        };
        StyleCard(panel);
        return panel;
    }

    private static void StyleCard(Control control)
    {
        control.Paint += (_, eventArgs) =>
        {
            using Pen border = new(CardBorderColor);
            eventArgs.Graphics.DrawRectangle(border, 0, 0, Math.Max(0, control.Width - 1), Math.Max(0, control.Height - 1));
        };
    }

    private int GetSelectedSimulatedUdpFrequency() =>
        _simulatedUdpFrequencyComboBox.SelectedItem is int frequencyHz
            ? frequencyHz
            : 100;

    private void ApplySimulatedUdpSettings()
    {
        int frequencyHz = GetSelectedSimulatedUdpFrequency();
        bool enabled = _simulatedUdpCheckBox.Checked;
        _simulatedUdpFrequencyComboBox.Enabled = enabled;
        _controller.Settings.SimulatedUdpInputEnabled = enabled;
        _controller.Settings.SimulatedUdpInputFrequencyHz = frequencyHz;
        _configureSimulatedUdp?.Invoke(enabled, frequencyHz);
    }

    private static string FormatSimulatedUdpFrequency(int frequencyHz) =>
        frequencyHz == SimulatedUdpMouseInput.UnlimitedFrequencyHz
            ? "无上限"
            : $"{frequencyHz} Hz";

    private void LoadSettings()
    {
        _loading = true;
        _startOnBootCheckBox.Checked = StartupRegistration.IsEnabled();
        _controller.Settings.StartOnBoot = _startOnBootCheckBox.Checked;
        _minimizeToTrayCheckBox.Checked = _controller.Settings.MinimizeToTray;
        _closeToTrayCheckBox.Checked = _controller.Settings.CloseToTray;
        _generateMovementAnalysisImageCheckBox.Checked = _controller.Settings.GenerateMovementAnalysisImage;
        int simulatedFrequency = SimulatedUdpMouseInput.SupportedFrequencies.Contains(
            _controller.Settings.SimulatedUdpInputFrequencyHz)
            ? _controller.Settings.SimulatedUdpInputFrequencyHz
            : 100;
        _simulatedUdpCheckBox.Checked = _controller.Settings.SimulatedUdpInputEnabled;
        _simulatedUdpFrequencyComboBox.SelectedItem = simulatedFrequency;
        _firmwareUpdateApiCheckBox.Checked =
            _firmwareUpdateApi is not null && _controller.Settings.FirmwareUpdateApiEnabled;
        _firmwareFileTextBox.Text = _controller.Settings.FirmwareManifestPath;
        _firmwarePortComboBox.Text = _controller.Settings.FirmwareFlashPortName;
        RefreshFirmwarePorts(showStatus: false);
        _confirmFlashButton.Enabled = CanStartFlash();
        _loading = false;
        ApplySimulatedUdpSettings();
    }

    private void SaveSettings()
    {
        if (_loading)
        {
            return;
        }
        _controller.Settings.StartOnBoot = _startOnBootCheckBox.Checked;
        _controller.Settings.MinimizeToTray = _minimizeToTrayCheckBox.Checked;
        _controller.Settings.CloseToTray = _closeToTrayCheckBox.Checked;
        _controller.Settings.GenerateMovementAnalysisImage = _generateMovementAnalysisImageCheckBox.Checked;
        _controller.Settings.FirmwareUpdateApiEnabled =
            _firmwareUpdateApi is not null && _firmwareUpdateApiCheckBox.Checked;
        _controller.Settings.FirmwareFlashPortName = _firmwarePortComboBox.Text.Trim();
        ApplySimulatedUdpSettings();
        try
        {
            _firmwareUpdateApi?.SetEnabled(_controller.Settings.FirmwareUpdateApiEnabled);
            _controller.SaveSettings();
            _statusLabel.ForeColor = Color.FromArgb(34, 125, 70);
            _statusLabel.Text = $"已保存 · {DateTime.Now:HH:mm:ss}";
        }
        catch (Exception exception)
        {
            _loading = true;
            _firmwareUpdateApiCheckBox.Checked = _firmwareUpdateApi?.Enabled == true;
            _controller.Settings.FirmwareUpdateApiEnabled = _firmwareUpdateApiCheckBox.Checked;
            _loading = false;
            _statusLabel.ForeColor = Color.FromArgb(170, 42, 42);
            _statusLabel.Text = $"保存失败：{exception.Message}";
        }
    }
}

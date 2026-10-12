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
    private readonly SerialBridge? _serialBridge;
    private readonly FirmwareUpdateApiServer? _firmwareUpdateApi;
    private readonly FirmwareFlashService? _firmwareFlash;
    private readonly CheckBox _startOnBootCheckBox;
    private readonly CheckBox _minimizeToTrayCheckBox;
    private readonly CheckBox _closeToTrayCheckBox;
    private readonly CheckBox _legacySingleBoardFirmwareCheckBox;
    private readonly Panel _legacySingleBoardOptionRow;
    private readonly CheckBox _makcuModeCheckBox;
    private readonly CheckBox _buttonStatePublishingCheckBox;
    private readonly Panel _makcuModeOptionRow;
    private readonly Panel _buttonStatePublishingOptionRow;
    private readonly TextBox _buttonStateMappingNameTextBox;
    private readonly Button _saveButtonStateMappingNameButton;
    private readonly Panel _buttonStateMappingNameOptionRow;
    private readonly CheckBox _simulatedUdpInputCheckBox;
    private readonly ComboBox _simulatedUdpInputFrequencyBox;
    private readonly Panel _simulatedUdpInputOptionRow;
    private readonly CheckBox _alwaysOutputUdpCheckBox;
    private readonly Panel _alwaysOutputUdpOptionRow;
    private readonly CheckBox _firmwareUpdateApiCheckBox;
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
        SerialBridge? serialBridge = null)
    {
        _controller = controller;
        _serialBridge = serialBridge;
        _firmwareUpdateApi = firmwareUpdateApi;
        _firmwareFlash = firmwareFlash;
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
            Text = "开关会立即保存；共享内存名称需单独保存并重启 EXE 后生效。托盘菜单中的“退出”始终会真正结束程序。",
            Dock = DockStyle.Fill,
            ForeColor = MutedTextColor,
            TextAlign = ContentAlignment.MiddleLeft,
        };

        _startOnBootCheckBox = CreateOption("开机启动", "登录当前 Windows 用户后自动启动本程序。");
        _minimizeToTrayCheckBox = CreateOption("最小化到托盘", "点击最小化后隐藏主窗口，双击托盘图标可恢复。");
        _closeToTrayCheckBox = CreateOption("关闭到托盘", "点击窗口关闭按钮时隐藏主窗口，不结束键鼠捕获和脚本运行。");
        _legacySingleBoardFirmwareCheckBox = CreateOption(
            "使用旧版单板通路",
            "开启后完全使用旧版单板串口通路：跳过新双板角色握手，发送旧版 7 字节鼠标报告。仅在连接旧版单板转发固件时开启。默认关闭。\n开启后无法确认串口设备身份。 ");
        _makcuModeCheckBox = CreateOption(
            "启用 Makcu 运行模式",
            "先验证 M 板再切至 115200；关闭 EXE 才释放串口。返回 A5 请退出其他客户端、重开 EXE 并手动关闭。",
            height: 58);
        _makcuModeCheckBox.Enabled = serialBridge is not null;
        _buttonStatePublishingCheckBox = CreateOption(
            "发布物理鼠标按键状态",
            "订阅 M 板真实鼠标按键并写入下方配置的共享内存；Makcu 模式期间暂停。",
            height: 58);
        _buttonStatePublishingCheckBox.Enabled = serialBridge is not null;
        _buttonStateMappingNameTextBox = new TextBox
        {
            Width = 420,
            Height = 28,
            AccessibleName = "ButtonState 共享内存名称",
        };
        _saveButtonStateMappingNameButton = new Button
        {
            Text = "保存名称",
            AutoSize = true,
            BackColor = Color.FromArgb(237, 243, 250),
            ForeColor = PrimaryTextColor,
            UseVisualStyleBackColor = false,
        };
        _buttonStateMappingNameOptionRow = WrapButtonStateMappingNameOption(
            _buttonStateMappingNameTextBox,
            _saveButtonStateMappingNameButton);
        _simulatedUdpInputCheckBox = CreateOption(
            "UDP 模拟输入测试",
            "仅旧版单板通路且 HOME 同步开启时生效：将实体鼠标移动按指定频率聚合并写入旧版单板 7 字节鼠标报告；此测试不发送网络 UDP 数据报。");
        _simulatedUdpInputFrequencyBox = new ComboBox
        {
            DropDownStyle = ComboBoxStyle.DropDownList,
            Width = 92,
            Height = 28,
            FormattingEnabled = true,
            AccessibleName = "UDP 模拟输入测试频率",
        };
        _simulatedUdpInputFrequencyBox.Items.AddRange([30, 60, 100, 140, 200, 500, 0]);
        _simulatedUdpInputFrequencyBox.Format += (_, eventArgs) =>
        {
            if (eventArgs.ListItem is int frequencyHz)
            {
                eventArgs.Value = frequencyHz == 0 ? "无上限" : $"{frequencyHz} Hz";
            }
        };
        _alwaysOutputUdpCheckBox = CreateOption(
            "始终开启 UDP 输出",
            "仅旧版单板通路使用。开启后 HOME 关闭时仍允许 UDP 输入发送到开发板；关闭后 UDP 输入只在 HOME 同步开启时发送。");
        _firmwareUpdateApiCheckBox = CreateOption(
            "启用局域网固件刷写接口",
            firmwareUpdateApi is null
                ? "当前不是串口模式，固件刷写接口不可用。"
                : $"监听局域网 TCP {firmwareUpdateApi.Port}；默认关闭，刷写时自动使用下方已选择的串口。仅限受信任局域网。");
        _firmwareUpdateApiCheckBox.Enabled = firmwareUpdateApi is not null;

        FlowLayoutPanel startupOptions = new()
        {
            Dock = DockStyle.Fill,
            Height = 600,
            FlowDirection = FlowDirection.TopDown,
            WrapContents = false,
            Padding = new Padding(0, 8, 0, 0),
        };
        _legacySingleBoardOptionRow = WrapOption(_legacySingleBoardFirmwareCheckBox);
        _makcuModeOptionRow = WrapOption(_makcuModeCheckBox);
        _buttonStatePublishingOptionRow = WrapOption(_buttonStatePublishingCheckBox);
        _simulatedUdpInputOptionRow = WrapSimulatedUdpInputOption(
            _simulatedUdpInputCheckBox,
            _simulatedUdpInputFrequencyBox);
        _alwaysOutputUdpOptionRow = WrapOption(_alwaysOutputUdpCheckBox);
        startupOptions.Controls.AddRange([
            WrapOption(_startOnBootCheckBox),
            WrapOption(_minimizeToTrayCheckBox),
            WrapOption(_closeToTrayCheckBox),
            _legacySingleBoardOptionRow,
            _makcuModeOptionRow,
            _buttonStatePublishingOptionRow,
            _buttonStateMappingNameOptionRow,
            _simulatedUdpInputOptionRow,
            _alwaysOutputUdpOptionRow,
        ]);

        Panel startupPanel = CreateCardPanel(700);
        Label startupTitle = CreateSectionTitle("启动与托盘");
        startupTitle.Dock = DockStyle.None;
        startupTitle.Location = new Point(16, 10);
        startupTitle.Size = new Size(900, 30);
        startupTitle.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        startupOptions.Dock = DockStyle.None;
        startupOptions.Location = new Point(16, 44);
        startupOptions.Size = new Size(900, 600);
        startupOptions.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        startupPanel.Controls.Add(startupOptions);
        startupPanel.Controls.Add(startupTitle);
        startupPanel.Controls.SetChildIndex(startupTitle, 0);
        Panel flashPanel = BuildFlashPanel();

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
        _legacySingleBoardFirmwareCheckBox.CheckedChanged += (_, _) => OnLegacyModeChanged();
        _makcuModeCheckBox.CheckedChanged += async (_, _) => await OnMakcuModeChangedAsync();
        _buttonStatePublishingCheckBox.CheckedChanged += async (_, _) =>
            await OnButtonStatePublishingChangedAsync();
        _saveButtonStateMappingNameButton.Click += (_, _) => SaveButtonStateMappingName();
        _simulatedUdpInputCheckBox.CheckedChanged += (_, _) => SaveSettings();
        _simulatedUdpInputFrequencyBox.SelectedIndexChanged += (_, _) => SaveSettings();
        _alwaysOutputUdpCheckBox.CheckedChanged += (_, _) => SaveSettings();
        _firmwareUpdateApiCheckBox.CheckedChanged += (_, _) => SaveSettings();
        LoadSettings();
    }

    internal CheckBox StartOnBootCheckBox => _startOnBootCheckBox;
    internal CheckBox MinimizeToTrayCheckBox => _minimizeToTrayCheckBox;
    internal CheckBox CloseToTrayCheckBox => _closeToTrayCheckBox;
    internal CheckBox LegacySingleBoardFirmwareCheckBox => _legacySingleBoardFirmwareCheckBox;
    internal Panel LegacySingleBoardOptionRow => _legacySingleBoardOptionRow;
    internal CheckBox MakcuModeCheckBox => _makcuModeCheckBox;
    internal CheckBox ButtonStatePublishingCheckBox => _buttonStatePublishingCheckBox;
    internal Panel MakcuModeOptionRow => _makcuModeOptionRow;
    internal Panel ButtonStatePublishingOptionRow => _buttonStatePublishingOptionRow;
    internal TextBox ButtonStateMappingNameTextBox => _buttonStateMappingNameTextBox;
    internal Button SaveButtonStateMappingNameButton => _saveButtonStateMappingNameButton;
    internal Panel ButtonStateMappingNameOptionRow => _buttonStateMappingNameOptionRow;
    internal string StatusText => _statusLabel.Text;
    internal CheckBox SimulatedUdpInputCheckBox => _simulatedUdpInputCheckBox;
    internal ComboBox SimulatedUdpInputFrequencyBox => _simulatedUdpInputFrequencyBox;
    internal Panel SimulatedUdpInputOptionRow => _simulatedUdpInputOptionRow;
    internal CheckBox AlwaysOutputUdpCheckBox => _alwaysOutputUdpCheckBox;
    internal Panel AlwaysOutputUdpOptionRow => _alwaysOutputUdpOptionRow;
    internal CheckBox FirmwareUpdateApiCheckBox => _firmwareUpdateApiCheckBox;
    internal ComboBox FirmwarePortComboBox => _firmwarePortComboBox;
    internal Button RefreshFirmwarePortsButton => _refreshFirmwarePortsButton;
    internal void ResetScrollPosition() => _cards.AutoScrollPosition = Point.Empty;

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

    private static Panel WrapSimulatedUdpInputOption(CheckBox option, ComboBox frequencyBox)
    {
        Panel panel = new()
        {
            Width = 900,
            Height = 66,
            Margin = new Padding(0),
            BackColor = CardSurfaceColor,
        };
        TableLayoutPanel row = new()
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = 2,
            Padding = new Padding(0),
        };
        row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 32));
        row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 68));
        row.RowStyles.Add(new RowStyle(SizeType.Percent, 58));
        row.RowStyles.Add(new RowStyle(SizeType.Percent, 42));
        option.Dock = DockStyle.Fill;
        row.Controls.Add(option, 0, 0);
        row.SetRowSpan(option, 2);
        row.Controls.Add(new Label
        {
            Text = option.Tag as string ?? string.Empty,
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = MutedTextColor,
            AutoEllipsis = true,
            AccessibleName = "UDP 模拟输入测试说明",
        }, 1, 0);
        FlowLayoutPanel frequencyOptions = new()
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            Margin = new Padding(0),
            Padding = new Padding(0),
        };
        frequencyOptions.Controls.Add(new Label
        {
            Text = "频率：",
            AutoSize = true,
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = MutedTextColor,
            Margin = new Padding(0, 5, 5, 0),
        });
        frequencyOptions.Controls.Add(frequencyBox);
        row.Controls.Add(frequencyOptions, 1, 1);
        panel.Controls.Add(row);
        return panel;
    }

    private static Panel WrapButtonStateMappingNameOption(TextBox mappingNameTextBox, Button saveButton)
    {
        Panel panel = new()
        {
            Width = 900,
            Height = 86,
            Margin = new Padding(0),
            BackColor = CardSurfaceColor,
        };
        TableLayoutPanel row = new()
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            RowCount = 2,
            Padding = new Padding(0),
        };
        row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 32));
        row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 68));
        row.RowStyles.Add(new RowStyle(SizeType.Percent, 54));
        row.RowStyles.Add(new RowStyle(SizeType.Percent, 46));
        row.Controls.Add(new Label
        {
            Text = "共享内存名称",
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = PrimaryTextColor,
        }, 0, 0);
        FlowLayoutPanel editor = new()
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            Margin = new Padding(0),
            Padding = new Padding(0),
        };
        editor.Controls.Add(mappingNameTextBox);
        editor.Controls.Add(saveButton);
        row.Controls.Add(editor, 1, 0);
        row.Controls.Add(new Label
        {
            Text = "与 Sender config.ini [aim] third_party_buttons_map 一致；保存后重启 EXE 生效。",
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = MutedTextColor,
            AutoEllipsis = true,
            AccessibleName = "共享内存名称说明",
        }, 1, 1);
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

    private void ApplyAlwaysOutputUdpSettings()
    {
        bool legacySingleBoard = _legacySingleBoardFirmwareCheckBox.Checked;
        _alwaysOutputUdpOptionRow.Visible = legacySingleBoard;
        _simulatedUdpInputOptionRow.Visible = legacySingleBoard;
        _simulatedUdpInputCheckBox.Enabled = legacySingleBoard;
        _simulatedUdpInputFrequencyBox.Enabled = legacySingleBoard && _simulatedUdpInputCheckBox.Checked;
        _controller.ApplyOutputRoutingSettings(
            legacySingleBoard,
            _alwaysOutputUdpCheckBox.Checked,
            _simulatedUdpInputCheckBox.Checked,
            _simulatedUdpInputFrequencyBox.SelectedItem is int frequencyHz ? frequencyHz : 100);
    }

    internal async Task RestoreMakcuModeOnStartupAsync()
    {
        if (_serialBridge is null || !_controller.Settings.MakcuEnabled)
        {
            return;
        }

        _makcuModeCheckBox.Enabled = false;
        _controller.PrepareMakcuModeTransition();
        try
        {
            MakcuModeChangeResult result = await _serialBridge.SetMakcuModeAsync(true);
            bool settingsSaved = false;
            bool automationResumed = false;
            bool actualSessionReady = IsVerifiedModeSessionReady(result);
            if (actualSessionReady)
            {
                _controller.Settings.MakcuEnabled = result.ActualEnabled;
                SetMakcuModeCheckBox(result.ActualEnabled);
                settingsSaved = SaveSettings();
                automationResumed = !result.ActualEnabled && ResumeAutomationOnlyAfterA5Restore();
            }
            _statusLabel.ForeColor = result.Success && result.ActualStateVerified &&
                (result.ActualEnabled || automationResumed) ? SuccessColor : ErrorColor;
            _statusLabel.Text = DescribeModeResult(
                result,
                settingsSaved,
                automationResumed,
                actualSessionReady,
                startupRestore: true);
        }
        catch (Exception exception)
        {
            _statusLabel.ForeColor = ErrorColor;
            SetMakcuModeCheckBox(_controller.Settings.MakcuEnabled);
            _statusLabel.Text = $"启动时恢复 Makcu 模式失败：{exception.Message} 当前模式未确认；宏/Lua保持停止，设置未改。";
        }
        finally
        {
            _makcuModeCheckBox.Enabled = true;
        }
    }

    private void OnLegacyModeChanged()
    {
        if (_loading)
        {
            return;
        }
        if (_legacySingleBoardFirmwareCheckBox.Checked &&
            (_controller.Settings.MakcuEnabled || _serialBridge?.IsMakcuModeEnabled == true ||
             _serialBridge?.IsModeTransitioning == true))
        {
            _loading = true;
            _legacySingleBoardFirmwareCheckBox.Checked = false;
            _loading = false;
            _statusLabel.ForeColor = ErrorColor;
            _statusLabel.Text = "请先在设置页验证并关闭 Makcu，再启用旧版单板通路。";
            return;
        }
        _ = SaveSettings();
    }

    private async Task OnMakcuModeChangedAsync()
    {
        if (_loading)
        {
            return;
        }
        bool requestedEnabled = _makcuModeCheckBox.Checked;
        if (_serialBridge is null ||
            (requestedEnabled && _legacySingleBoardFirmwareCheckBox.Checked))
        {
            _loading = true;
            _makcuModeCheckBox.Checked = _controller.Settings.MakcuEnabled;
            _loading = false;
            _statusLabel.ForeColor = ErrorColor;
            _statusLabel.Text = _serialBridge is null
                ? "当前没有主串口，Makcu 模式不可用。"
                : "旧版单板通路与 Makcu 不能同时启用。";
            return;
        }

        _makcuModeCheckBox.Enabled = false;
        _buttonStatePublishingCheckBox.Enabled = false;
        _controller.PrepareMakcuModeTransition();
        try
        {
            MakcuModeChangeResult result = await _serialBridge.SetMakcuModeAsync(requestedEnabled);
            bool settingsSaved = false;
            bool automationResumed = false;
            bool actualSessionReady = IsVerifiedModeSessionReady(result);
            if (result.Success && actualSessionReady)
            {
                _controller.Settings.MakcuEnabled = result.ActualEnabled;
                SetMakcuModeCheckBox(result.ActualEnabled);
                settingsSaved = SaveSettings();
                automationResumed = !result.ActualEnabled && ResumeAutomationOnlyAfterA5Restore();
            }
            else
            {
                if (actualSessionReady)
                {
                    _controller.Settings.MakcuEnabled = result.ActualEnabled;
                    SetMakcuModeCheckBox(result.ActualEnabled);
                    settingsSaved = SaveSettings();
                    automationResumed = !result.ActualEnabled && ResumeAutomationOnlyAfterA5Restore();
                }
                else
                {
                    SetMakcuModeCheckBox(_controller.Settings.MakcuEnabled);
                }
            }
            _statusLabel.ForeColor = result.Success && result.ActualStateVerified &&
                (result.ActualEnabled || automationResumed) ? SuccessColor : ErrorColor;
            _statusLabel.Text = DescribeModeResult(
                result,
                settingsSaved,
                automationResumed,
                actualSessionReady,
                startupRestore: false);
        }
        catch (Exception exception)
        {
            SetMakcuModeCheckBox(_controller.Settings.MakcuEnabled);
            _statusLabel.ForeColor = ErrorColor;
            _statusLabel.Text = $"Makcu 模式切换失败：{exception.Message} 当前模式未确认；宏/Lua保持停止，设置未改。";
        }
        finally
        {
            _makcuModeCheckBox.Enabled = true;
            _buttonStatePublishingCheckBox.Enabled = _serialBridge is not null;
        }
    }

    private bool ResumeAutomationOnlyAfterA5Restore() =>
        _serialBridge?.IsA5ModeReadyForAutomation == true &&
        _controller.CompleteMakcuModeTransition(a5ModeReady: true);

    private bool IsVerifiedModeSessionReady(MakcuModeChangeResult result) =>
        result.ActualStateVerified && _serialBridge is not null &&
        (result.ActualEnabled
            ? _serialBridge.IsMakcuModeSessionReady
            : _serialBridge.IsA5ModeReadyForAutomation);

    private void SetMakcuModeCheckBox(bool enabled)
    {
        bool previousLoading = _loading;
        _loading = true;
        _makcuModeCheckBox.Checked = enabled;
        _loading = previousLoading;
    }

    private string DescribeModeResult(
        MakcuModeChangeResult result,
        bool settingsSaved,
        bool automationResumed,
        bool actualSessionReady,
        bool startupRestore)
    {
        string text = result.Message;
        if (!result.ActualStateVerified)
        {
            return $"{text} 当前模式未确认；宏/Lua保持停止，设置未改。";
        }
        if (!actualSessionReady)
        {
            return $"{text} 实际模式查询已通过，但读取器或串口会话未保持健康；宏/Lua保持停止，设置未改。";
        }
        if (!settingsSaved)
        {
            return $"{text} 实际模式已验证，但设置保存失败；请检查本地配置权限。";
        }
        if (result.ActualEnabled)
        {
            return $"{text} 实际模式已核实并已保存。宏/Lua保持停止；关闭 Makcu 并确认恢复 A5 后才会重载。";
        }
        if (!automationResumed)
        {
            return $"{text} 实际模式已核实并已保存为 A5，但读取器或输入门未恢复；宏/Lua继续停止。";
        }
        if (!result.Success)
        {
            return $"{text} 实际状态已同步，宏/Lua仅在 A5/921600 与读取器/输入门均恢复后重载。";
        }
        return startupRestore ? $"{text} A5/921600 与读取器已恢复。" : text;
    }

    private async Task OnButtonStatePublishingChangedAsync()
    {
        if (_loading)
        {
            return;
        }
        bool enabled = _buttonStatePublishingCheckBox.Checked;
        _controller.Settings.ButtonStatePublishingEnabled = enabled;
        if (!SaveSettings())
        {
            _loading = true;
            _buttonStatePublishingCheckBox.Checked =
                _controller.Settings.ButtonStatePublishingEnabled;
            _loading = false;
            return;
        }
        if (_serialBridge is null)
        {
            return;
        }

        try
        {
            bool applied = await _serialBridge.SetButtonStatePublishingEnabledAsync(enabled);
            _statusLabel.ForeColor = applied ? SuccessColor : ErrorColor;
            _statusLabel.Text = applied
                ? _serialBridge.IsMakcuModeEnabled
                    ? "已保存；Makcu 关闭后会恢复物理按键订阅。"
                    : "物理按键状态发布设置已保存。"
                : "设置已保存，但当前串口未能接收订阅命令；重连后会重试。";
        }
        catch (Exception exception)
        {
            _statusLabel.ForeColor = ErrorColor;
            _statusLabel.Text = $"物理按键发布设置已保存，应用失败：{exception.Message}";
        }
    }

    private void SaveButtonStateMappingName()
    {
        if (_loading)
        {
            return;
        }

        if (!ButtonStateSharedMemory.TryNormalizeMappingName(
                _buttonStateMappingNameTextBox.Text,
                out string mappingName,
                out string? error))
        {
            _statusLabel.ForeColor = ErrorColor;
            _statusLabel.Text = error ?? "共享内存名称无效。";
            return;
        }

        string previousName = _controller.Settings.ButtonStateMappingName;
        _controller.Settings.ButtonStateMappingName = mappingName;
        try
        {
            _controller.SaveSettings(updateStartupRegistration: false);
            _buttonStateMappingNameTextBox.Text = mappingName;
            _statusLabel.ForeColor = SuccessColor;
            _statusLabel.Text = "共享内存名称已保存；需重启 EXE 生效，并确保 Sender 名称一致。";
        }
        catch (Exception exception)
        {
            _controller.Settings.ButtonStateMappingName = previousName;
            _statusLabel.ForeColor = ErrorColor;
            _statusLabel.Text = $"共享内存名称保存失败，原设置已保留：{exception.Message}";
        }
    }

    private void LoadSettings()
    {
        _loading = true;
        _startOnBootCheckBox.Checked = StartupRegistration.IsEnabled();
        _controller.Settings.StartOnBoot = _startOnBootCheckBox.Checked;
        _minimizeToTrayCheckBox.Checked = _controller.Settings.MinimizeToTray;
        _closeToTrayCheckBox.Checked = _controller.Settings.CloseToTray;
        _legacySingleBoardFirmwareCheckBox.Checked =
            _controller.Settings.LegacySingleBoardFirmwareCompatibility;
        _makcuModeCheckBox.Checked = _controller.Settings.MakcuEnabled;
        _buttonStatePublishingCheckBox.Checked =
            _controller.Settings.ButtonStatePublishingEnabled;
        _buttonStateMappingNameTextBox.Text = ButtonStateSharedMemory.NormalizeConfiguredMappingName(
            _controller.Settings.ButtonStateMappingName);
        _simulatedUdpInputCheckBox.Checked = _controller.Settings.SimulatedUdpInputEnabled;
        int frequencyIndex = _simulatedUdpInputFrequencyBox.Items.IndexOf(
            _controller.Settings.SimulatedUdpInputFrequencyHz);
        _simulatedUdpInputFrequencyBox.SelectedIndex = frequencyIndex >= 0
            ? frequencyIndex
            : _simulatedUdpInputFrequencyBox.Items.IndexOf(100);
        _alwaysOutputUdpCheckBox.Checked = _controller.Settings.AlwaysOutputUdpEnabled;
        _firmwareUpdateApiCheckBox.Checked =
            _firmwareUpdateApi is not null && _controller.Settings.FirmwareUpdateApiEnabled;
        _firmwareFileTextBox.Text = _controller.Settings.FirmwareManifestPath;
        _firmwarePortComboBox.Text = _controller.Settings.FirmwareFlashPortName;
        RefreshFirmwarePorts(showStatus: false);
        _confirmFlashButton.Enabled = CanStartFlash();
        _loading = false;
        ApplyAlwaysOutputUdpSettings();
    }

    private bool SaveSettings()
    {
        if (_loading)
        {
            return true;
        }
        _controller.Settings.StartOnBoot = _startOnBootCheckBox.Checked;
        _controller.Settings.MinimizeToTray = _minimizeToTrayCheckBox.Checked;
        _controller.Settings.CloseToTray = _closeToTrayCheckBox.Checked;
        _controller.Settings.MakcuEnabled = _makcuModeCheckBox.Checked;
        _controller.Settings.ButtonStatePublishingEnabled =
            _buttonStatePublishingCheckBox.Checked;
        ApplyAlwaysOutputUdpSettings();
        _controller.Settings.FirmwareUpdateApiEnabled =
            _firmwareUpdateApi is not null && _firmwareUpdateApiCheckBox.Checked;
        _controller.Settings.FirmwareFlashPortName = _firmwarePortComboBox.Text.Trim();
        try
        {
            _firmwareUpdateApi?.SetEnabled(_controller.Settings.FirmwareUpdateApiEnabled);
            _controller.SaveSettings();
            _statusLabel.ForeColor = Color.FromArgb(34, 125, 70);
            _statusLabel.Text = $"已保存 · {DateTime.Now:HH:mm:ss}";
            return true;
        }
        catch (Exception exception)
        {
            _loading = true;
            _firmwareUpdateApiCheckBox.Checked = _firmwareUpdateApi?.Enabled == true;
            _controller.Settings.FirmwareUpdateApiEnabled = _firmwareUpdateApiCheckBox.Checked;
            _loading = false;
            _statusLabel.ForeColor = Color.FromArgb(170, 42, 42);
            _statusLabel.Text = $"保存失败：{exception.Message}";
            return false;
        }
    }
}

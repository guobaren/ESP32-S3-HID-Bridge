using HidBridge.Host.Automation;

namespace HidBridge.Host.Ui;

internal sealed class SettingsPageControl : UserControl
{
    private readonly AutomationController _controller;
    private readonly CheckBox _startOnBootCheckBox;
    private readonly CheckBox _minimizeToTrayCheckBox;
    private readonly CheckBox _closeToTrayCheckBox;
    private readonly CheckBox _generateMovementAnalysisImageCheckBox;
    private readonly Label _statusLabel;
    private bool _loading;

    internal SettingsPageControl(AutomationController controller)
    {
        _controller = controller;
        Dock = DockStyle.Fill;
        Padding = new Padding(24);

        Label title = new()
        {
            Text = "程序行为",
            Dock = DockStyle.Top,
            Height = 42,
            Font = new Font(Font.FontFamily, 15, FontStyle.Bold),
        };
        Label description = new()
        {
            Text = "设置会立即保存，并在下次启动时继续生效。托盘菜单中的“退出”始终会真正结束程序。",
            Dock = DockStyle.Top,
            Height = 46,
            ForeColor = Color.FromArgb(74, 88, 108),
        };

        _startOnBootCheckBox = CreateOption("开机启动", "登录当前 Windows 用户后自动启动本程序。");
        _minimizeToTrayCheckBox = CreateOption("最小化到托盘", "点击最小化后隐藏主窗口，双击托盘图标可恢复。");
        _closeToTrayCheckBox = CreateOption("关闭到托盘", "点击窗口关闭按钮时隐藏主窗口，不结束键鼠捕获和脚本运行。");
        _generateMovementAnalysisImageCheckBox = CreateOption("生成按键情况分析图片", "左右键记录完成后打开分析窗口，并在 artifacts 目录生成 PNG 图片。");

        FlowLayoutPanel options = new()
        {
            Dock = DockStyle.Top,
            Height = 274,
            FlowDirection = FlowDirection.TopDown,
            WrapContents = false,
            Padding = new Padding(0, 8, 0, 0),
        };
        options.Controls.AddRange([
            WrapOption(_startOnBootCheckBox),
            WrapOption(_minimizeToTrayCheckBox),
            WrapOption(_closeToTrayCheckBox),
            WrapOption(_generateMovementAnalysisImageCheckBox),
        ]);

        _statusLabel = new Label
        {
            Dock = DockStyle.Top,
            Height = 34,
            ForeColor = Color.FromArgb(34, 125, 70),
            TextAlign = ContentAlignment.MiddleLeft,
        };

        Controls.Add(_statusLabel);
        Controls.Add(options);
        Controls.Add(description);
        Controls.Add(title);

        _startOnBootCheckBox.CheckedChanged += (_, _) => SaveSettings();
        _minimizeToTrayCheckBox.CheckedChanged += (_, _) => SaveSettings();
        _closeToTrayCheckBox.CheckedChanged += (_, _) => SaveSettings();
        _generateMovementAnalysisImageCheckBox.CheckedChanged += (_, _) => SaveSettings();
        LoadSettings();
    }

    internal CheckBox StartOnBootCheckBox => _startOnBootCheckBox;
    internal CheckBox MinimizeToTrayCheckBox => _minimizeToTrayCheckBox;
    internal CheckBox CloseToTrayCheckBox => _closeToTrayCheckBox;
    internal CheckBox GenerateMovementAnalysisImageCheckBox => _generateMovementAnalysisImageCheckBox;

    private static CheckBox CreateOption(string title, string description) => new()
    {
        Text = $"{title}\r\n{description}",
        AutoSize = false,
        Width = 720,
        Height = 58,
        TextAlign = ContentAlignment.MiddleLeft,
        Padding = new Padding(4, 0, 0, 0),
    };

    private static Panel WrapOption(CheckBox option)
    {
        Panel panel = new() { Width = 760, Height = 64 };
        panel.Controls.Add(option);
        return panel;
    }

    private void LoadSettings()
    {
        _loading = true;
        _startOnBootCheckBox.Checked = StartupRegistration.IsEnabled();
        _controller.Settings.StartOnBoot = _startOnBootCheckBox.Checked;
        _minimizeToTrayCheckBox.Checked = _controller.Settings.MinimizeToTray;
        _closeToTrayCheckBox.Checked = _controller.Settings.CloseToTray;
        _generateMovementAnalysisImageCheckBox.Checked = _controller.Settings.GenerateMovementAnalysisImage;
        _loading = false;
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
        try
        {
            _controller.SaveSettings();
            _statusLabel.ForeColor = Color.FromArgb(34, 125, 70);
            _statusLabel.Text = $"已保存 · {DateTime.Now:HH:mm:ss}";
        }
        catch (Exception exception)
        {
            _statusLabel.ForeColor = Color.FromArgb(170, 42, 42);
            _statusLabel.Text = $"保存失败：{exception.Message}";
        }
    }
}

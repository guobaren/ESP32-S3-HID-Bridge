using HidBridge.Host.Automation;

namespace HidBridge.Host.Ui;

internal sealed class LuaPageControl : UserControl
{
    private readonly AutomationController _controller;
    private readonly ComboBox _profileComboBox;
    private readonly TextBox _editor;
    private readonly Label _statusLabel;
    private readonly TextBox _logTextBox;
    private readonly Button _startButton;
    private readonly Button _stopButton;
    private readonly Button _addProfileButton;
    private readonly Button _deleteProfileButton;
    private AutomationProfile _profile;
    private bool _loading;

    internal LuaPageControl(AutomationController controller)
    {
        _controller = controller;
        _profile = controller.ActiveProfile;
        Dock = DockStyle.Fill;
        Padding = new Padding(10);

        _profileComboBox = new ComboBox
        {
            DropDownStyle = ComboBoxStyle.DropDownList,
            Width = 260,
            AccessibleName = "当前 Lua 配置",
        };
        _addProfileButton = new Button { Text = "新增配置", AutoSize = true };
        _deleteProfileButton = new Button { Text = "删除配置", AutoSize = true };
        FlowLayoutPanel profileBar = new()
        {
            Dock = DockStyle.Top,
            Height = 40,
            WrapContents = false,
        };
        profileBar.Controls.Add(new Label { Text = "配置：", AutoSize = true, Padding = new Padding(0, 7, 0, 0) });
        profileBar.Controls.Add(_profileComboBox);
        profileBar.Controls.Add(_addProfileButton);
        profileBar.Controls.Add(_deleteProfileButton);
        profileBar.Controls.Add(new Label
        {
            Text = "切换配置后，非空 Lua 脚本自动启动",
            AutoSize = true,
            Padding = new Padding(12, 7, 0, 0),
            ForeColor = Color.FromArgb(74, 88, 108),
        });

        _editor = new TextBox
        {
            Dock = DockStyle.Fill,
            Multiline = true,
            AcceptsTab = true,
            ScrollBars = ScrollBars.Both,
            WordWrap = false,
            Font = new Font("Cascadia Mono", 10),
        };
        _statusLabel = new Label
        {
            Dock = DockStyle.Bottom,
            Height = 34,
            TextAlign = ContentAlignment.MiddleLeft,
        };
        Button docsButton = new() { Text = "接口说明", Dock = DockStyle.Fill };
        Button checkButton = new() { Text = "检查", Dock = DockStyle.Fill };
        Button saveButton = new() { Text = "保存并重载", Dock = DockStyle.Fill };
        _startButton = new Button { Text = "启动", Dock = DockStyle.Fill };
        _stopButton = new Button { Text = "停止", Dock = DockStyle.Fill };
        Button clearButton = new() { Text = "清空日志", Dock = DockStyle.Fill };
        TableLayoutPanel buttons = new()
        {
            Dock = DockStyle.Bottom,
            Height = 40,
            ColumnCount = 6,
        };
        for (int index = 0; index < 6; index++)
        {
            buttons.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100f / 6));
        }
        buttons.Controls.Add(docsButton, 0, 0);
        buttons.Controls.Add(checkButton, 1, 0);
        buttons.Controls.Add(saveButton, 2, 0);
        buttons.Controls.Add(_startButton, 3, 0);
        buttons.Controls.Add(_stopButton, 4, 0);
        buttons.Controls.Add(clearButton, 5, 0);

        Panel editorPanel = new() { Dock = DockStyle.Fill };
        editorPanel.Controls.Add(_editor);
        editorPanel.Controls.Add(_statusLabel);
        editorPanel.Controls.Add(buttons);

        _logTextBox = new TextBox
        {
            Dock = DockStyle.Fill,
            Multiline = true,
            ReadOnly = true,
            ScrollBars = ScrollBars.Vertical,
            BackColor = Color.FromArgb(249, 250, 252),
            Font = new Font("Cascadia Mono", 9),
        };
        SplitContainer split = new()
        {
            Dock = DockStyle.Fill,
            Orientation = Orientation.Horizontal,
            SplitterDistance = 480,
        };
        split.Panel1.Controls.Add(editorPanel);
        split.Panel2.Controls.Add(_logTextBox);

        Controls.Add(split);
        Controls.Add(profileBar);

        _profileComboBox.SelectedIndexChanged += (_, _) => SelectProfileFromUi();
        _addProfileButton.Click += (_, _) => AddProfile();
        _deleteProfileButton.Click += (_, _) => DeleteProfile();
        docsButton.Click += (_, _) => ShowApiDocumentation();
        checkButton.Click += (_, _) => CheckScript();
        saveButton.Click += (_, _) => SaveScript();
        _startButton.Click += (_, _) => StartScript();
        _stopButton.Click += (_, _) => _controller.StopLua();
        clearButton.Click += (_, _) => _logTextBox.Clear();
        _controller.ActiveProfileChanged += HandleActiveProfileChanged;
        _controller.Log += AppendLog;
        _controller.LuaLogCleared += ClearLog;
        _controller.LuaStateChanged += UpdateLuaState;

        RefreshProfiles(_profile.Name);
        UpdateLuaState(_controller.LuaActive);
    }

    internal ComboBox ProfileComboBox => _profileComboBox;
    internal TextBox Editor => _editor;
    internal Button AddProfileButton => _addProfileButton;
    internal Button DeleteProfileButton => _deleteProfileButton;

    private void RefreshProfiles(string selected)
    {
        _loading = true;
        _profileComboBox.Items.Clear();
        _profileComboBox.Items.AddRange(_controller.ListProfiles().Cast<object>().ToArray());
        _profileComboBox.SelectedItem = selected;
        if (_profileComboBox.SelectedIndex < 0 && _profileComboBox.Items.Count > 0)
        {
            _profileComboBox.SelectedIndex = 0;
        }
        _loading = false;
        LoadProfile(_controller.ActiveProfile.Name);
    }

    private void SelectProfileFromUi()
    {
        if (_loading || _profileComboBox.SelectedItem is not string name)
        {
            return;
        }
        _controller.SetActiveProfile(name);
    }

    private void HandleActiveProfileChanged(string name) => RunOnUiThread(() =>
    {
        _loading = true;
        if (!_profileComboBox.Items.Contains(name))
        {
            RefreshProfiles(name);
        }
        else
        {
            _profileComboBox.SelectedItem = name;
            LoadProfile(name);
        }
        _loading = false;
    });

    private void LoadProfile(string name)
    {
        _profile = _controller.LoadProfile(name);
        _editor.Text = NormalizeEditorNewlines(_profile.LuaScriptText);
        _editor.Select(0, 0);
        _statusLabel.Text = _controller.LuaActive ? "运行中" : "未运行";
    }

    private void AddProfile()
    {
        string? name = PromptDialog.Show(this, "新增配置", "配置名称：");
        if (string.IsNullOrWhiteSpace(name))
        {
            return;
        }
        try
        {
            _controller.CreateProfile(name);
            RefreshProfiles(name);
            _controller.SetActiveProfile(name);
        }
        catch (Exception exception)
        {
            MessageBox.Show(this, exception.Message, "新增配置失败", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private void DeleteProfile()
    {
        string name = _profile.Name;
        if (name.Equals(AutomationProfileStore.GlobalProfile, StringComparison.OrdinalIgnoreCase))
        {
            MessageBox.Show(this, "Global 配置不可删除。", "删除配置");
            return;
        }
        if (MessageBox.Show(this, $"确认删除配置 {name}？", "删除配置", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
        {
            return;
        }
        _controller.DeleteProfile(name);
        RefreshProfiles(_controller.ActiveProfile.Name);
    }

    private void CheckScript()
    {
        (bool success, string message) = _controller.CheckLua(_editor.Text);
        _statusLabel.ForeColor = success ? Color.FromArgb(34, 125, 70) : Color.FromArgb(170, 42, 42);
        _statusLabel.Text = message;
    }

    private void SaveScript()
    {
        _profile.LuaScriptText = _editor.Text;
        _controller.SaveProfile(_profile);
        CheckScript();
        AppendLog("Lua 脚本已保存；当前配置已重新加载。脚本非空时自动运行。");
    }

    private void StartScript()
    {
        try
        {
            _controller.StartLua(_editor.Text);
        }
        catch (Exception exception)
        {
            _statusLabel.ForeColor = Color.FromArgb(170, 42, 42);
            _statusLabel.Text = exception.Message;
        }
    }

    private void ShowApiDocumentation()
    {
        const string documentation =
            "入口：function OnEvent(event, arg)\r\n\r\n" +
            "move(dx, dy) / moveto(x, y)\r\n" +
            "mouse(button, state) / wheel(delta)\r\n" +
            "keydown(key) / keyup(key) / keypress(key[, ms])\r\n" +
            "sleep(ms) / Sleep(ms)\r\n" +
            "randdelay(ms[, range]) / randsleep(ms[, range])\r\n" +
            "IsPressed(arg) / DebugLog(fmt, ...) / ClearLog()\r\n\r\n" +
            "鼠标参数：1 左键、2 中键、3 右键、4/5 侧键。\r\n" +
            "键盘参数支持友好键名或 HID Usage；VK_CODES 表提供常见 VK。";
        MessageBox.Show(this, documentation, "Lua 接口说明", MessageBoxButtons.OK, MessageBoxIcon.Information);
    }

    private void UpdateLuaState(bool active) => RunOnUiThread(() =>
    {
        _startButton.Enabled = !active;
        _stopButton.Enabled = active;
        if (active)
        {
            _statusLabel.ForeColor = Color.FromArgb(34, 125, 70);
            _statusLabel.Text = "运行中";
        }
        else if (_statusLabel.Text == "运行中")
        {
            _statusLabel.ForeColor = Color.FromArgb(74, 88, 108);
            _statusLabel.Text = "未运行";
        }
    });

    private void AppendLog(string message) => RunOnUiThread(() =>
    {
        _logTextBox.AppendText($"[{DateTime.Now:HH:mm:ss.fff}] {message}{Environment.NewLine}");
        _logTextBox.SelectionStart = _logTextBox.TextLength;
        _logTextBox.ScrollToCaret();
    });

    private void ClearLog() => RunOnUiThread(_logTextBox.Clear);

    private void RunOnUiThread(Action action)
    {
        if (IsDisposed)
        {
            return;
        }
        if (InvokeRequired)
        {
            BeginInvoke(action);
        }
        else
        {
            action();
        }
    }

    private static string NormalizeEditorNewlines(string text) =>
        text.Replace("\r\n", "\n", StringComparison.Ordinal)
            .Replace('\r', '\n')
            .Replace("\n", Environment.NewLine, StringComparison.Ordinal);

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _controller.ActiveProfileChanged -= HandleActiveProfileChanged;
            _controller.Log -= AppendLog;
            _controller.LuaLogCleared -= ClearLog;
            _controller.LuaStateChanged -= UpdateLuaState;
        }
        base.Dispose(disposing);
    }
}

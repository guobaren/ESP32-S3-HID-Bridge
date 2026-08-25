using System.Collections.Concurrent;
using HidBridge.Host.Automation;

namespace HidBridge.Host.Ui;

internal sealed class LuaPageControl : UserControl
{
    private static readonly Color PageBackgroundColor = Color.FromArgb(242, 246, 251);
    private static readonly Color CardSurfaceColor = Color.White;
    private static readonly Color CardBorderColor = Color.FromArgb(211, 221, 234);
    private static readonly Color PrimaryTextColor = Color.FromArgb(23, 35, 58);
    private static readonly Color MutedTextColor = Color.FromArgb(82, 106, 139);
    private static readonly Color AccentColor = Color.FromArgb(48, 105, 232);
    private const int LuaLogFlushIntervalMilliseconds = 100;
    private const int MaximumVisibleLuaLogCharacters = 64 * 1024;
    private const int MaximumPendingLuaLogLines = 2_000;
    private const int MaximumLuaLogLinesPerFlush = 500;
    private const string LuaDocumentation =
        "入口：function OnEvent(event, arg)\r\n\r\n" +
        "键盘事件 arg 使用小写键名；驱动模拟的 F13-F24 可用 f13..f24 检测。\r\n\r\n" +
        "move(dx, dy) / moveto(x, y)\r\n" +
        "mouse(button, state) / wheel(delta)\r\n" +
        "keydown(key) / keyup(key) / keypress(key[, ms])\r\n" +
        "delay(ms) / sleep(ms) / Sleep(ms)（可取消，单位毫秒）\r\n" +
        "randdelay(ms[, range]) / randsleep(ms[, range])\r\n" +
        "IsPressed(arg) / DebugLog(fmt, ...) / ClearLog()\r\n\r\n" +
        "鼠标参数：1 左键、2 中键、3 右键、4/5 侧键。\r\n" +
        "键盘参数支持友好键名或 HID Usage；VK_CODES 表提供常见 VK。";
    private readonly AutomationController _controller;
    private readonly ComboBox _profileComboBox;
    private readonly LineNumberTextBox _editor;
    private readonly LineNumberEditor _lineNumberEditor;
    private readonly Label _statusLabel;
    private readonly TextBox _logTextBox;
    private readonly Button _checkButton;
    private readonly Button _startButton;
    private readonly Button _stopButton;
    private readonly Button _apiButton;
    private readonly Button _addProfileButton;
    private readonly Button _deleteProfileButton;
    private readonly ConcurrentQueue<string> _pendingLogs = new();
    private readonly System.Windows.Forms.Timer _logFlushTimer;
    private int _pendingLogCount;
    private AutomationProfile _profile;
    private bool _loading;

    internal LuaPageControl(AutomationController controller)
    {
        _controller = controller;
        _profile = controller.ActiveProfile;
        Dock = DockStyle.Fill;
        Padding = new Padding(18, 14, 18, 18);
        BackColor = PageBackgroundColor;

        _profileComboBox = new ComboBox
        {
            DropDownStyle = ComboBoxStyle.DropDownList,
            Width = 190,
            AccessibleName = "当前 Lua 配置",
        };
        _addProfileButton = CreateButton("新增配置");
        _deleteProfileButton = CreateButton("删除配置");
        FlowLayoutPanel profileBar = new()
        {
            Dock = DockStyle.Right,
            Width = 700,
            Height = 44,
            WrapContents = false,
            FlowDirection = FlowDirection.LeftToRight,
            Padding = new Padding(0, 3, 0, 0),
        };
        profileBar.Controls.Add(new Label { Text = "配置", AutoSize = true, Padding = new Padding(0, 7, 8, 0), ForeColor = MutedTextColor });
        profileBar.Controls.Add(_profileComboBox);
        profileBar.Controls.Add(_addProfileButton);
        profileBar.Controls.Add(_deleteProfileButton);
        Label profileHint = new()
        {
            Text = "切换配置后，非空 Lua 脚本自动启动",
            AutoSize = true,
            Padding = new Padding(12, 7, 0, 0),
            ForeColor = MutedTextColor,
        };
        profileBar.Controls.Add(profileHint);
        Label pageTitle = new()
        {
            Text = "Lua 脚本",
            Dock = DockStyle.Left,
            Width = 240,
            TextAlign = ContentAlignment.MiddleLeft,
            Font = new Font(Font.FontFamily, 16, FontStyle.Bold),
            ForeColor = PrimaryTextColor,
        };
        Label pageStatus = new()
        {
            Text = "已保存",
            Dock = DockStyle.Bottom,
            Height = 22,
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = Color.FromArgb(19, 128, 88),
        };
        Panel header = new() { Dock = DockStyle.Top, Height = 60 };
        header.Controls.Add(profileBar);
        header.Controls.Add(pageStatus);
        header.Controls.Add(pageTitle);

        _editor = new LineNumberTextBox
        {
            Multiline = true,
            AcceptsTab = true,
            ScrollBars = ScrollBars.Both,
            WordWrap = false,
            Font = new Font("Cascadia Mono", 10),
            BackColor = Color.FromArgb(249, 251, 254),
            ForeColor = PrimaryTextColor,
        };
        _lineNumberEditor = new LineNumberEditor(_editor);
        _statusLabel = new Label
        {
            Dock = DockStyle.Bottom,
            Height = 34,
            TextAlign = ContentAlignment.MiddleLeft,
        };
        _apiButton = CreateButton("接口说明");
        _checkButton = CreateButton("检查");
        Button saveButton = CreateButton("保存并重载");
        _startButton = CreateButton("启动");
        _stopButton = CreateButton("停止");
        Button clearButton = CreateButton("清空日志");
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
        foreach (Button button in new[] { _apiButton, _checkButton, saveButton, _startButton, _stopButton, clearButton })
        {
            button.Dock = DockStyle.Fill;
            button.Margin = new Padding(2, 4, 2, 0);
        }
        buttons.Controls.Add(_apiButton, 0, 0);
        buttons.Controls.Add(_checkButton, 1, 0);
        buttons.Controls.Add(saveButton, 2, 0);
        buttons.Controls.Add(_startButton, 3, 0);
        buttons.Controls.Add(_stopButton, 4, 0);
        buttons.Controls.Add(clearButton, 5, 0);

        Panel editorPanel = new() { Dock = DockStyle.Fill, Padding = new Padding(16), BackColor = CardSurfaceColor };
        StyleCard(editorPanel);
        Label editorTitle = new()
        {
            Text = "脚本编辑器",
            Dock = DockStyle.Top,
            Height = 32,
            ForeColor = PrimaryTextColor,
            Font = new Font(Font.FontFamily, 11, FontStyle.Bold),
        };
        editorPanel.Controls.Add(_lineNumberEditor);
        editorPanel.Controls.Add(_statusLabel);
        editorPanel.Controls.Add(buttons);
        editorPanel.Controls.Add(editorTitle);

        _logTextBox = new TextBox
        {
            Dock = DockStyle.Fill,
            Multiline = true,
            ReadOnly = true,
            ScrollBars = ScrollBars.Vertical,
            BackColor = Color.FromArgb(249, 250, 252),
            Font = new Font("Cascadia Mono", 9),
            ForeColor = PrimaryTextColor,
        };
        TableLayoutPanel logPanel = new()
        {
            Dock = DockStyle.Fill,
            RowCount = 2,
            ColumnCount = 1,
            Padding = new Padding(16, 10, 16, 14),
            BackColor = PageBackgroundColor,
        };
        logPanel.RowStyles.Add(new RowStyle(SizeType.Absolute, 30));
        logPanel.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        logPanel.Controls.Add(new Label
        {
            Text = "运行日志",
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = PrimaryTextColor,
            Font = new Font(Font.FontFamily, 11, FontStyle.Bold),
        }, 0, 0);
        logPanel.Controls.Add(_logTextBox, 0, 1);
        logPanel.Paint += (_, eventArgs) =>
        {
            using Pen border = new(CardBorderColor);
            eventArgs.Graphics.DrawRectangle(border, 0, 0, Math.Max(0, logPanel.Width - 1), Math.Max(0, logPanel.Height - 1));
        };
        SplitContainer split = new()
        {
            Dock = DockStyle.Fill,
            Orientation = Orientation.Horizontal,
            SplitterDistance = 450,
            BackColor = PageBackgroundColor,
        };
        split.Panel1.Controls.Add(editorPanel);
        split.Panel2.Controls.Add(logPanel);

        Controls.Add(split);
        Controls.Add(header);

        _profileComboBox.SelectedIndexChanged += (_, _) => SelectProfileFromUi();
        _addProfileButton.Click += (_, _) => AddProfile();
        _deleteProfileButton.Click += (_, _) => DeleteProfile();
        _apiButton.Click += (_, _) => ShowApiDocumentation();
        _checkButton.Click += (_, _) => CheckScript();
        saveButton.Click += (_, _) => SaveScript();
        _startButton.Click += (_, _) => StartScript();
        _stopButton.Click += (_, _) => _controller.StopLua();
        clearButton.Click += (_, _) => ClearLog();
        _controller.ActiveProfileChanged += HandleActiveProfileChanged;
        _controller.LuaLog += AppendLog;
        _controller.LuaLogCleared += ClearLog;
        _controller.LuaStateChanged += UpdateLuaState;

        _logFlushTimer = new System.Windows.Forms.Timer
        {
            Interval = LuaLogFlushIntervalMilliseconds,
        };
        _logFlushTimer.Tick += (_, _) => FlushPendingLogs();
        _logFlushTimer.Start();

        RefreshProfiles(_profile.Name);
        UpdateLuaState(_controller.LuaActive);
    }

    internal ComboBox ProfileComboBox => _profileComboBox;
    internal TextBox Editor => _editor;
    internal Control LineNumberGutter => _lineNumberEditor.Gutter;
    internal Label StatusLabel => _statusLabel;
    internal Button CheckButton => _checkButton;
    internal Button ApiButton => _apiButton;
    internal Button StartButton => _startButton;
    internal Button StopButton => _stopButton;
    internal Button AddProfileButton => _addProfileButton;
    internal Button DeleteProfileButton => _deleteProfileButton;

    private static Button CreateButton(string text, bool primary = false) => new()
    {
        Text = text,
        AutoSize = false,
        BackColor = primary ? Color.FromArgb(48, 105, 232) : Color.FromArgb(237, 243, 250),
        ForeColor = primary ? Color.White : PrimaryTextColor,
        FlatStyle = FlatStyle.Standard,
        UseVisualStyleBackColor = false,
        MinimumSize = new Size(0, 34),
    };

    private static void StyleCard(Control control)
    {
        control.Paint += (_, eventArgs) =>
        {
            using Pen border = new(CardBorderColor);
            eventArgs.Graphics.DrawRectangle(border, 0, 0, Math.Max(0, control.Width - 1), Math.Max(0, control.Height - 1));
        };
    }

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
        _editor.Text = LuaScriptIndentation.Align(NormalizeEditorNewlines(_profile.LuaScriptText));
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
        string aligned = LuaScriptIndentation.Align(_editor.Text);
        (bool success, string message) = _controller.CheckLua(aligned);
        if (success && !string.Equals(_editor.Text, aligned, StringComparison.Ordinal))
        {
            int selectionStart = Math.Min(_editor.SelectionStart, aligned.Length);
            _editor.Text = aligned;
            _editor.Select(selectionStart, 0);
            message += "，已自动对齐缩进和空格（不改变换行）";
        }
        _statusLabel.ForeColor = success ? Color.FromArgb(34, 125, 70) : Color.FromArgb(170, 42, 42);
        _statusLabel.Text = message;
    }

    private void SaveScript()
    {
        _editor.Text = LuaScriptIndentation.Align(_editor.Text);
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
            UpdateLuaState(_controller.LuaActive);
            _statusLabel.ForeColor = Color.FromArgb(170, 42, 42);
            _statusLabel.Text = exception.Message;
        }
    }

    private void ShowApiDocumentation()
    {
        using CopyableTextDialog dialog = new("Lua 接口说明", LuaDocumentation);
        dialog.ShowDialog(FindForm() ?? (IWin32Window)this);
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

    private void AppendLog(string message)
    {
        _pendingLogs.Enqueue($"[{DateTime.Now:HH:mm:ss.fff}] {message}");
        int pendingCount = Interlocked.Increment(ref _pendingLogCount);
        while (pendingCount > MaximumPendingLuaLogLines && _pendingLogs.TryDequeue(out _))
        {
            pendingCount = Interlocked.Decrement(ref _pendingLogCount);
        }
    }

    private void FlushPendingLogs()
    {
        if (_logTextBox.IsDisposed || _pendingLogs.IsEmpty)
        {
            return;
        }

        List<string> pending = [];
        while (pending.Count < MaximumLuaLogLinesPerFlush &&
               _pendingLogs.TryDequeue(out string? line))
        {
            Interlocked.Decrement(ref _pendingLogCount);
            pending.Add(line);
        }
        if (pending.Count == 0)
        {
            return;
        }

        LogTextBoxAppender.Append(_logTextBox, pending, MaximumVisibleLuaLogCharacters);
    }

    private void ClearLog() => RunOnUiThread(() =>
    {
        while (_pendingLogs.TryDequeue(out _))
        {
            Interlocked.Decrement(ref _pendingLogCount);
        }
        _logTextBox.Clear();
    });

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

    internal static string NormalizeEditorNewlines(string text) =>
        text.Replace("\r\n", "\n", StringComparison.Ordinal)
            .Replace('\r', '\n')
            .Replace("\n", Environment.NewLine, StringComparison.Ordinal);

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _logFlushTimer.Stop();
            _logFlushTimer.Dispose();
            _controller.ActiveProfileChanged -= HandleActiveProfileChanged;
            _controller.LuaLog -= AppendLog;
            _controller.LuaLogCleared -= ClearLog;
            _controller.LuaStateChanged -= UpdateLuaState;
        }
        base.Dispose(disposing);
    }

}

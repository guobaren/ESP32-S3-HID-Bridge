using System.Collections.Concurrent;
using HidBridge.Host.Automation;

namespace HidBridge.Host.Ui;

internal sealed class MacroPageControl : UserControl
{
    private static readonly Color PageBackgroundColor = Color.FromArgb(242, 246, 251);
    private static readonly Color CardSurfaceColor = Color.White;
    private static readonly Color CardBorderColor = Color.FromArgb(211, 221, 234);
    private static readonly Color PrimaryTextColor = Color.FromArgb(23, 35, 58);
    private static readonly Color MutedTextColor = Color.FromArgb(82, 106, 139);
    private static readonly Color AccentColor = Color.FromArgb(48, 105, 232);
    private static readonly Color SuccessColor = Color.FromArgb(19, 128, 88);
    private static readonly Color ErrorColor = Color.FromArgb(170, 42, 42);
    private const string MacroDocumentation =
        "命令格式：operation(arg1, arg2)，参数按当前解析器要求为整数；行尾可用 # 添加注释。\r\n\r\n" +
        "move(dx, dy)              相对移动鼠标\r\n" +
        "moveto(x, y)              移动到绝对坐标\r\n" +
        "mouse(button, state)      鼠标键：1 左 / 2 中 / 3 右 / 4、5 侧键；state 1 按下、0 松开\r\n" +
        "keydown(key)              按住键盘按键\r\n" +
        "keyup(key)                松开键盘按键\r\n" +
        "keypress(key[, hold_ms])  点按按键，可选按住时长\r\n" +
        "wheel(amount)             输出垂直滚轮增量\r\n" +
        "delay(ms) / sleep(ms)     可取消延时，底层共用同一实现\r\n" +
        "randsleep(base, variance) / randdelay(base, variance)\r\n" +
        "                          在 base ± variance 范围内随机延时\r\n\r\n" +
        "按下 / 按住 / 松开分段模式使用：\r\n" +
        "[on_press]                按下段\r\n" +
        "[while_hold]              按住循环段\r\n" +
        "[on_release]              松开段\r\n\r\n" +
        "key 参数支持友好键名或 HID Usage；按键、鼠标按钮会在宏停止时自动释放。";
    private const int LogFlushIntervalMilliseconds = 100;
    private const int MaximumVisibleLogCharacters = 64 * 1024;
    private const int MaximumPendingLogLines = 2_000;
    private const int MaximumLogLinesPerFlush = 500;
    private readonly AutomationController _controller;
    private readonly ComboBox _profileComboBox;
    private readonly ListBox _macroListBox;
    private readonly HotkeyChooserControl _triggerChooser;
    private readonly ComboBox _modeComboBox;
    private readonly CheckBox _enabledCheckBox;
    private readonly TextBox _editor;
    private readonly Label _validationLabel;
    private readonly TextBox _logTextBox;
    private readonly SplitContainer _editorSplit;
    private readonly Button _interfaceButton;
    private readonly ConcurrentQueue<string> _pendingLogs = new();
    private readonly System.Windows.Forms.Timer _logFlushTimer;
    private int _pendingLogCount;
    private AutomationProfile _profile;
    private bool _loading;

    internal MacroPageControl(AutomationController controller)
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
            AccessibleName = "当前宏配置",
        };
        Button addProfileButton = CreateButton("新增配置");
        Button deleteProfileButton = CreateButton("删除配置");
        FlowLayoutPanel profileBar = new()
        {
            Dock = DockStyle.Right,
            Width = 460,
            Height = 44,
            WrapContents = false,
            FlowDirection = FlowDirection.LeftToRight,
            Padding = new Padding(0, 3, 0, 0),
        };
        profileBar.Controls.AddRange([
            new Label { Text = "配置", AutoSize = true, Padding = new Padding(0, 7, 8, 0), ForeColor = MutedTextColor },
            _profileComboBox,
            addProfileButton,
            deleteProfileButton,
        ]);
        Label pageTitle = new()
        {
            Text = "宏",
            Dock = DockStyle.Left,
            Width = 220,
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

        _macroListBox = new ListBox { Dock = DockStyle.Fill, BackColor = Color.FromArgb(249, 251, 254), BorderStyle = BorderStyle.FixedSingle };
        Button addMacroButton = CreateButton("新增宏");
        Button deleteMacroButton = CreateButton("删除宏");
        Button renameMacroButton = CreateButton("重命名");
        TableLayoutPanel macroButtons = new()
        {
            Dock = DockStyle.Bottom,
            Height = 80,
            ColumnCount = 2,
            RowCount = 2,
            Padding = new Padding(0, 6, 0, 0),
        };
        macroButtons.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        macroButtons.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        macroButtons.RowStyles.Add(new RowStyle(SizeType.Percent, 50));
        macroButtons.RowStyles.Add(new RowStyle(SizeType.Percent, 50));
        addMacroButton.Dock = DockStyle.Fill;
        deleteMacroButton.Dock = DockStyle.Fill;
        renameMacroButton.Dock = DockStyle.Fill;
        addMacroButton.Margin = new Padding(0, 0, 4, 4);
        deleteMacroButton.Margin = new Padding(4, 0, 0, 4);
        renameMacroButton.Margin = new Padding(0, 4, 4, 0);
        macroButtons.Controls.Add(addMacroButton, 0, 0);
        macroButtons.Controls.Add(deleteMacroButton, 1, 0);
        macroButtons.Controls.Add(renameMacroButton, 0, 1);
        Panel macroListPanel = new() { Dock = DockStyle.Fill, Padding = new Padding(16), BackColor = CardSurfaceColor };
        StyleCard(macroListPanel);
        Label macroListTitle = new()
        {
            Text = "宏列表",
            Dock = DockStyle.Top,
            Height = 30,
            ForeColor = PrimaryTextColor,
            Font = new Font(Font.FontFamily, 11, FontStyle.Bold),
        };
        macroListPanel.Controls.Add(_macroListBox);
        macroListPanel.Controls.Add(macroButtons);
        macroListPanel.Controls.Add(macroListTitle);

        _triggerChooser = new HotkeyChooserControl();
        _modeComboBox = new ComboBox
        {
            Dock = DockStyle.Fill,
            DropDownStyle = ComboBoxStyle.DropDownList,
            Margin = new Padding(0, 3, 3, 3),
        };
        _modeComboBox.Items.AddRange([
            new ModeItem("按下执行一次", MacroRunModes.Once),
            new ModeItem("切换重复 / 停止", MacroRunModes.Toggle),
            new ModeItem("按住循环", MacroRunModes.HoldLoop),
            new ModeItem("按下 / 按住 / 松开分段", MacroRunModes.Staged),
        ]);
        _enabledCheckBox = new CheckBox
        {
            Text = "启用宏",
            Dock = DockStyle.Fill,
            AutoSize = true,
            Checked = true,
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = PrimaryTextColor,
            Margin = new Padding(3, 3, 0, 3),
        };
        TableLayoutPanel options = new()
        {
            Dock = DockStyle.Top,
            Height = 82,
            ColumnCount = 4,
            RowCount = 2,
            AutoSize = false,
            Padding = new Padding(0),
        };
        options.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 72));
        options.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 22));
        options.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 82));
        options.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 78));
        options.RowStyles.Add(new RowStyle(SizeType.Absolute, 34));
        options.RowStyles.Add(new RowStyle(SizeType.Absolute, 34));
        options.Controls.Add(new Label
        {
            Text = "触发键：",
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = MutedTextColor,
        }, 0, 0);
        options.Controls.Add(_triggerChooser, 1, 0);
        options.SetColumnSpan(_triggerChooser, 3);
        options.Controls.Add(new Label
        {
            Text = "模式：",
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = MutedTextColor,
        }, 0, 1);
        options.Controls.Add(_modeComboBox, 1, 1);
        options.Controls.Add(_enabledCheckBox, 2, 1);
        options.SetColumnSpan(_enabledCheckBox, 2);

        _editor = new TextBox
        {
            Dock = DockStyle.Fill,
            Multiline = true,
            AcceptsTab = true,
            ScrollBars = ScrollBars.Both,
            WordWrap = false,
            Font = new Font("Cascadia Mono", 10),
            BackColor = Color.FromArgb(249, 251, 254),
            ForeColor = PrimaryTextColor,
        };
        _validationLabel = new Label
        {
            Dock = DockStyle.Bottom,
            Height = 42,
            AutoEllipsis = true,
            ForeColor = ErrorColor,
        };
        _interfaceButton = CreateButton("接口说明");
        Button checkButton = CreateButton("检查语法");
        Button saveButton = CreateButton("保存宏", primary: true);
        _interfaceButton.Dock = DockStyle.Fill;
        checkButton.Dock = DockStyle.Fill;
        saveButton.Dock = DockStyle.Fill;
        _interfaceButton.Margin = new Padding(0, 0, 4, 0);
        checkButton.Margin = new Padding(0, 0, 4, 0);
        saveButton.Margin = new Padding(4, 0, 0, 0);
        TableLayoutPanel editorButtons = new()
        {
            Dock = DockStyle.Bottom,
            Height = 42,
            ColumnCount = 3,
            Padding = new Padding(0, 4, 0, 0),
        };
        editorButtons.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 33.3333f));
        editorButtons.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 33.3333f));
        editorButtons.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 33.3334f));
        editorButtons.Controls.Add(_interfaceButton, 0, 0);
        editorButtons.Controls.Add(checkButton, 1, 0);
        editorButtons.Controls.Add(saveButton, 2, 0);
        Panel editorPanel = new() { Dock = DockStyle.Fill, Padding = new Padding(16), BackColor = CardSurfaceColor };
        StyleCard(editorPanel);
        Label editorTitle = new()
        {
            Text = "宏编辑器",
            Dock = DockStyle.Top,
            Height = 30,
            ForeColor = PrimaryTextColor,
            Font = new Font(Font.FontFamily, 11, FontStyle.Bold),
        };
        editorPanel.Controls.Add(_editor);
        editorPanel.Controls.Add(_validationLabel);
        editorPanel.Controls.Add(editorButtons);
        editorPanel.Controls.Add(options);
        editorPanel.Controls.Add(editorTitle);

        _editorSplit = new SplitContainer
        {
            Dock = DockStyle.Fill,
            SplitterDistance = 340,
            Panel1MinSize = 340,
            FixedPanel = FixedPanel.Panel1,
            BackColor = PageBackgroundColor,
        };
        _editorSplit.SizeChanged += (_, _) => RestoreMacroListWidth();
        _editorSplit.Panel1.Controls.Add(macroListPanel);
        _editorSplit.Panel2.Controls.Add(editorPanel);

        _logTextBox = new TextBox
        {
            Dock = DockStyle.Fill,
            Multiline = true,
            ReadOnly = true,
            ScrollBars = ScrollBars.Vertical,
            BackColor = Color.FromArgb(249, 250, 252),
            ForeColor = PrimaryTextColor,
            Font = new Font("Cascadia Mono", 9),
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
        SplitContainer verticalSplit = new()
        {
            Dock = DockStyle.Fill,
            Orientation = Orientation.Horizontal,
            SplitterDistance = 450,
            BackColor = PageBackgroundColor,
        };
        verticalSplit.Panel1.Controls.Add(_editorSplit);
        verticalSplit.Panel2.Controls.Add(logPanel);

        Controls.Add(verticalSplit);
        Controls.Add(header);

        _profileComboBox.SelectedIndexChanged += (_, _) => SelectProfileFromUi();
        _macroListBox.SelectedIndexChanged += (_, _) => LoadSelectedMacro();
        _modeComboBox.SelectedIndexChanged += (_, _) =>
        {
            if (_loading)
            {
                return;
            }
            ScaffoldStagedMacro();
            SaveEditedMacroImmediately();
        };
        _enabledCheckBox.CheckedChanged += (_, _) =>
        {
            if (!_loading)
            {
                SaveEditedMacroImmediately();
            }
        };
        addProfileButton.Click += (_, _) => AddProfile();
        deleteProfileButton.Click += (_, _) => DeleteProfile();
        addMacroButton.Click += (_, _) => AddMacro();
        deleteMacroButton.Click += (_, _) => DeleteMacro();
        renameMacroButton.Click += (_, _) => RenameMacro();
        _interfaceButton.Click += (_, _) => ShowInterfaceDocumentation();
        checkButton.Click += (_, _) => CheckMacro();
        saveButton.Click += (_, _) => SaveMacro();
        _controller.ActiveProfileChanged += HandleActiveProfileChanged;
        _controller.MacroLog += AppendLog;

        _logFlushTimer = new System.Windows.Forms.Timer
        {
            Interval = LogFlushIntervalMilliseconds,
        };
        _logFlushTimer.Tick += (_, _) => FlushPendingLogs();
        _logFlushTimer.Start();

        RefreshProfiles(_profile.Name);
    }

    internal ComboBox ProfileComboBox => _profileComboBox;
    internal ListBox MacroListBox => _macroListBox;
    internal HotkeyChooserControl TriggerChooser => _triggerChooser;
    internal ComboBox ModeComboBox => _modeComboBox;
    internal CheckBox EnabledCheckBox => _enabledCheckBox;
    internal Button InterfaceButton => _interfaceButton;

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

    private void RestoreMacroListWidth()
    {
        const int desiredWidth = 340;
        int maximumWidth = _editorSplit.ClientSize.Width - _editorSplit.Panel2MinSize - _editorSplit.SplitterWidth;
        if (maximumWidth >= desiredWidth && _editorSplit.SplitterDistance != desiredWidth)
        {
            _editorSplit.SplitterDistance = desiredWidth;
        }
    }

    private void SelectProfileFromUi()
    {
        if (_loading || _profileComboBox.SelectedItem is not string name)
        {
            return;
        }
        _controller.SetActiveProfile(name);
    }

    private void HandleActiveProfileChanged(string name)
    {
        RunOnUiThread(() =>
        {
            if (!_profileComboBox.Items.Contains(name))
            {
                RefreshProfiles(name);
                return;
            }
            _loading = true;
            _profileComboBox.SelectedItem = name;
            _loading = false;
            LoadProfile(name);
        });
    }

    private void LoadProfile(string name)
    {
        _profile = _controller.LoadProfile(name);
        string? previousMacro = _macroListBox.SelectedItem as string;
        _loading = true;
        _macroListBox.Items.Clear();
        _macroListBox.Items.AddRange(_profile.Macros.Keys.OrderBy(value => value).Cast<object>().ToArray());
        if (previousMacro is not null && _macroListBox.Items.Contains(previousMacro))
        {
            _macroListBox.SelectedItem = previousMacro;
        }
        else if (_macroListBox.Items.Count > 0)
        {
            _macroListBox.SelectedIndex = 0;
        }
        else
        {
            ClearEditor();
        }
        _loading = false;
        LoadSelectedMacro();
    }

    private void LoadSelectedMacro()
    {
        if (_loading || _macroListBox.SelectedItem is not string name ||
            !_profile.Macros.TryGetValue(name, out MacroDefinition? macro))
        {
            return;
        }
        _loading = true;
        _triggerChooser.SetHotkey(macro.Trigger);
        _enabledCheckBox.Checked = macro.Enabled;
        _modeComboBox.SelectedItem = _modeComboBox.Items.Cast<ModeItem>()
            .FirstOrDefault(item => item.Value == macro.Mode) ?? _modeComboBox.Items[0];
        _editor.Text = NormalizeEditorNewlines(macro.Text);
        _validationLabel.Text = string.Empty;
        _loading = false;
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

    private void AddMacro()
    {
        string? name = PromptDialog.Show(this, "新增宏", "宏名称：");
        if (string.IsNullOrWhiteSpace(name))
        {
            return;
        }
        if (_profile.Macros.ContainsKey(name))
        {
            MessageBox.Show(this, "同名宏已存在。", "新增宏");
            return;
        }
        _profile.Macros[name] = new MacroDefinition { Name = name };
        _controller.SaveProfile(_profile);
        LoadProfile(_profile.Name);
        _macroListBox.SelectedItem = name;
    }

    private void DeleteMacro()
    {
        if (_macroListBox.SelectedItem is not string name)
        {
            return;
        }
        if (MessageBox.Show(this, $"确认删除宏 {name}？", "删除宏", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
        {
            return;
        }
        string? associatedFile = _profile.Macros[name].ScriptFile;
        _profile.Macros.Remove(name);
        _controller.DeleteMacro(_profile.Name, name, associatedFile);
        _controller.SaveProfile(_profile);
        LoadProfile(_profile.Name);
    }

    private void RenameMacro()
    {
        if (_macroListBox.SelectedItem is not string oldName)
        {
            return;
        }
        string? newName = PromptDialog.Show(this, "重命名宏", "新的宏名称：");
        if (string.IsNullOrWhiteSpace(newName) ||
            newName.Equals(oldName, StringComparison.OrdinalIgnoreCase))
        {
            return;
        }
        if (_profile.Macros.ContainsKey(newName))
        {
            MessageBox.Show(this, "同名宏已存在。", "重命名宏");
            return;
        }

        MacroDefinition macro = _profile.Macros[oldName];
        string? associatedFile = macro.ScriptFile;
        _profile.Macros.Remove(oldName);
        macro.Name = newName;
        _profile.Macros[newName] = macro;
        _controller.DeleteMacro(_profile.Name, oldName, associatedFile);
        _controller.SaveProfile(_profile);
        LoadProfile(_profile.Name);
        _macroListBox.SelectedItem = newName;
        AppendLog($"宏 {oldName} 已重命名为 {newName}。");
    }

    private void ShowInterfaceDocumentation()
    {
        using CopyableTextDialog dialog = new("宏接口说明", MacroDocumentation);
        dialog.ShowDialog(FindForm() ?? (IWin32Window)this);
    }

    private void CheckMacro()
    {
        MacroDefinition? macro = BuildEditedMacro();
        if (macro is null)
        {
            return;
        }
        IReadOnlyList<MacroParseError> errors = _controller.CheckMacro(macro);
        _validationLabel.ForeColor = errors.Count == 0 ? Color.FromArgb(34, 125, 70) : Color.FromArgb(170, 42, 42);
        _validationLabel.Text = errors.Count == 0 ? "语法正确" : string.Join(" | ", errors.Take(3));
    }

    private void SaveMacro(bool appendLog = true)
    {
        if (_macroListBox.SelectedItem is not string name)
        {
            return;
        }
        MacroDefinition? macro = BuildEditedMacro();
        if (macro is null)
        {
            return;
        }
        macro.Name = name;
        _profile.Macros[name] = macro;
        _controller.SaveProfile(_profile);
        CheckMacro();
        if (appendLog)
        {
            AppendLog($"宏 {name} 已保存并重新加载。");
        }
    }

    private void SaveEditedMacroImmediately() => SaveMacro(appendLog: false);

    private MacroDefinition? BuildEditedMacro()
    {
        if (_macroListBox.SelectedItem is not string name)
        {
            return null;
        }
        return new MacroDefinition
        {
            Name = name,
            Trigger = _triggerChooser.Value.Trim(),
            Mode = (_modeComboBox.SelectedItem as ModeItem)?.Value ?? MacroRunModes.Once,
            Enabled = _enabledCheckBox.Checked,
            Text = _editor.Text,
        };
    }

    private void ScaffoldStagedMacro()
    {
        if (_loading || (_modeComboBox.SelectedItem as ModeItem)?.Value != MacroRunModes.Staged ||
            !string.IsNullOrWhiteSpace(_editor.Text))
        {
            return;
        }
        _editor.Text = "[on_press]\r\n\r\n[while_hold]\r\n\r\n[on_release]\r\n";
    }

    private void ClearEditor()
    {
        _triggerChooser.ClearHotkey();
        _editor.Clear();
        _enabledCheckBox.Checked = true;
        _modeComboBox.SelectedIndex = 0;
        _validationLabel.Text = string.Empty;
    }

    private void AppendLog(string message)
    {
        _pendingLogs.Enqueue($"[{DateTime.Now:HH:mm:ss.fff}] {message}");
        int pendingCount = Interlocked.Increment(ref _pendingLogCount);
        while (pendingCount > MaximumPendingLogLines && _pendingLogs.TryDequeue(out _))
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
        while (pending.Count < MaximumLogLinesPerFlush &&
               _pendingLogs.TryDequeue(out string? line))
        {
            Interlocked.Decrement(ref _pendingLogCount);
            pending.Add(line);
        }
        if (pending.Count == 0)
        {
            return;
        }

        LogTextBoxAppender.Append(_logTextBox, pending, MaximumVisibleLogCharacters);
    }

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
            _logFlushTimer.Stop();
            _logFlushTimer.Dispose();
            _controller.ActiveProfileChanged -= HandleActiveProfileChanged;
            _controller.MacroLog -= AppendLog;
        }
        base.Dispose(disposing);
    }

    private sealed record ModeItem(string Label, string Value)
    {
        public override string ToString() => Label;
    }
}

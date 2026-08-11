using HidBridge.Host.Automation;

namespace HidBridge.Host.Ui;

internal sealed class MacroPageControl : UserControl
{
    private readonly AutomationController _controller;
    private readonly ComboBox _profileComboBox;
    private readonly ListBox _macroListBox;
    private readonly TextBox _triggerTextBox;
    private readonly ComboBox _modeComboBox;
    private readonly CheckBox _enabledCheckBox;
    private readonly TextBox _editor;
    private readonly Label _validationLabel;
    private readonly TextBox _logTextBox;
    private AutomationProfile _profile;
    private bool _loading;

    internal MacroPageControl(AutomationController controller)
    {
        _controller = controller;
        _profile = controller.ActiveProfile;
        Dock = DockStyle.Fill;
        Padding = new Padding(10);

        _profileComboBox = new ComboBox
        {
            DropDownStyle = ComboBoxStyle.DropDownList,
            Width = 210,
            AccessibleName = "当前宏配置",
        };
        Button addProfileButton = new() { Text = "新增配置", AutoSize = true };
        Button deleteProfileButton = new() { Text = "删除配置", AutoSize = true };
        FlowLayoutPanel profileBar = new()
        {
            Dock = DockStyle.Top,
            Height = 40,
            WrapContents = false,
            AutoScroll = true,
        };
        profileBar.Controls.AddRange([
            new Label { Text = "配置：", AutoSize = true, Padding = new Padding(0, 7, 0, 0) },
            _profileComboBox,
            addProfileButton,
            deleteProfileButton,
        ]);

        _macroListBox = new ListBox { Dock = DockStyle.Fill };
        Button addMacroButton = new() { Text = "新增宏", Dock = DockStyle.Fill };
        Button deleteMacroButton = new() { Text = "删除宏", Dock = DockStyle.Fill };
        TableLayoutPanel macroButtons = new()
        {
            Dock = DockStyle.Bottom,
            Height = 36,
            ColumnCount = 2,
        };
        macroButtons.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        macroButtons.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        macroButtons.Controls.Add(addMacroButton, 0, 0);
        macroButtons.Controls.Add(deleteMacroButton, 1, 0);
        Panel macroListPanel = new() { Dock = DockStyle.Fill, Padding = new Padding(0, 0, 6, 0) };
        macroListPanel.Controls.Add(_macroListBox);
        macroListPanel.Controls.Add(macroButtons);

        _triggerTextBox = new TextBox { Dock = DockStyle.Fill, PlaceholderText = "例如 mouse_side1 或 ctrl+f1" };
        _modeComboBox = new ComboBox { Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList };
        _modeComboBox.Items.AddRange([
            new ModeItem("按下执行一次", MacroRunModes.Once),
            new ModeItem("切换重复 / 停止", MacroRunModes.Toggle),
            new ModeItem("按住循环", MacroRunModes.HoldLoop),
            new ModeItem("按下 / 按住 / 松开分段", MacroRunModes.Staged),
        ]);
        _enabledCheckBox = new CheckBox { Text = "启用", Dock = DockStyle.Fill, Checked = true };
        TableLayoutPanel options = new()
        {
            Dock = DockStyle.Top,
            Height = 68,
            ColumnCount = 4,
            RowCount = 2,
        };
        options.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 72));
        options.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 55));
        options.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 82));
        options.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 45));
        options.Controls.Add(new Label { Text = "触发键：", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, 0);
        options.Controls.Add(_triggerTextBox, 1, 0);
        options.SetColumnSpan(_triggerTextBox, 3);
        options.Controls.Add(new Label { Text = "模式：", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, 1);
        options.Controls.Add(_modeComboBox, 1, 1);
        options.Controls.Add(_enabledCheckBox, 2, 1);

        _editor = new TextBox
        {
            Dock = DockStyle.Fill,
            Multiline = true,
            AcceptsTab = true,
            ScrollBars = ScrollBars.Both,
            WordWrap = false,
            Font = new Font("Cascadia Mono", 10),
        };
        _validationLabel = new Label
        {
            Dock = DockStyle.Bottom,
            Height = 42,
            AutoEllipsis = true,
            ForeColor = Color.FromArgb(170, 42, 42),
        };
        Button checkButton = new() { Text = "检查语法", Dock = DockStyle.Fill };
        Button saveButton = new() { Text = "保存宏", Dock = DockStyle.Fill };
        TableLayoutPanel editorButtons = new()
        {
            Dock = DockStyle.Bottom,
            Height = 38,
            ColumnCount = 2,
        };
        editorButtons.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        editorButtons.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        editorButtons.Controls.Add(checkButton, 0, 0);
        editorButtons.Controls.Add(saveButton, 1, 0);
        Panel editorPanel = new() { Dock = DockStyle.Fill };
        editorPanel.Controls.Add(_editor);
        editorPanel.Controls.Add(_validationLabel);
        editorPanel.Controls.Add(editorButtons);
        editorPanel.Controls.Add(options);

        SplitContainer editorSplit = new()
        {
            Dock = DockStyle.Fill,
            SplitterDistance = 250,
            FixedPanel = FixedPanel.Panel1,
        };
        editorSplit.Panel1.Controls.Add(macroListPanel);
        editorSplit.Panel2.Controls.Add(editorPanel);

        _logTextBox = new TextBox
        {
            Dock = DockStyle.Fill,
            Multiline = true,
            ReadOnly = true,
            ScrollBars = ScrollBars.Vertical,
            BackColor = Color.FromArgb(249, 250, 252),
        };
        SplitContainer verticalSplit = new()
        {
            Dock = DockStyle.Fill,
            Orientation = Orientation.Horizontal,
            SplitterDistance = 430,
        };
        verticalSplit.Panel1.Controls.Add(editorSplit);
        verticalSplit.Panel2.Controls.Add(_logTextBox);

        Controls.Add(verticalSplit);
        Controls.Add(profileBar);

        _profileComboBox.SelectedIndexChanged += (_, _) => SelectProfileFromUi();
        _macroListBox.SelectedIndexChanged += (_, _) => LoadSelectedMacro();
        _modeComboBox.SelectedIndexChanged += (_, _) => ScaffoldStagedMacro();
        addProfileButton.Click += (_, _) => AddProfile();
        deleteProfileButton.Click += (_, _) => DeleteProfile();
        addMacroButton.Click += (_, _) => AddMacro();
        deleteMacroButton.Click += (_, _) => DeleteMacro();
        checkButton.Click += (_, _) => CheckMacro();
        saveButton.Click += (_, _) => SaveMacro();
        _controller.ActiveProfileChanged += HandleActiveProfileChanged;
        _controller.Log += AppendLog;

        RefreshProfiles(_profile.Name);
    }

    internal ComboBox ProfileComboBox => _profileComboBox;
    internal ListBox MacroListBox => _macroListBox;

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
        _triggerTextBox.Text = macro.Trigger;
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
        _profile.Macros.Remove(name);
        _controller.DeleteMacro(_profile.Name, name);
        _controller.SaveProfile(_profile);
        LoadProfile(_profile.Name);
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

    private void SaveMacro()
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
        AppendLog($"宏 {name} 已保存并重新加载。");
    }

    private MacroDefinition? BuildEditedMacro()
    {
        if (_macroListBox.SelectedItem is not string name)
        {
            return null;
        }
        return new MacroDefinition
        {
            Name = name,
            Trigger = _triggerTextBox.Text.Trim(),
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
        _triggerTextBox.Clear();
        _editor.Clear();
        _enabledCheckBox.Checked = true;
        _modeComboBox.SelectedIndex = 0;
        _validationLabel.Text = string.Empty;
    }

    private void AppendLog(string message) => RunOnUiThread(() =>
    {
        _logTextBox.AppendText($"[{DateTime.Now:HH:mm:ss.fff}] {message}{Environment.NewLine}");
        _logTextBox.SelectionStart = _logTextBox.TextLength;
        _logTextBox.ScrollToCaret();
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
        }
        base.Dispose(disposing);
    }

    private sealed record ModeItem(string Label, string Value)
    {
        public override string ToString() => Label;
    }
}

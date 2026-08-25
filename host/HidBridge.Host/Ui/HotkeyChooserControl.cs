using HidBridge.Host.Automation;

namespace HidBridge.Host.Ui;

/// <summary>
/// 宏触发键选择器：从按键列表逐个添加按键组成组合键，并支持清空。
/// </summary>
internal sealed class HotkeyChooserControl : UserControl
{
    private const int TriggerControlHeight = 34;
    private readonly TextBox _display;
    private readonly ComboBox _keyComboBox;
    private readonly List<string> _keys = [];

    internal HotkeyChooserControl()
    {
        Dock = DockStyle.Fill;
        Margin = new Padding(0);
        MinimumSize = new Size(0, TriggerControlHeight);

        _display = new TextBox
        {
            ReadOnly = true,
            Dock = DockStyle.Fill,
            AutoSize = false,
            PlaceholderText = "未设置触发键",
            Margin = new Padding(0),
            Height = TriggerControlHeight,
            MinimumSize = new Size(0, TriggerControlHeight),
            TabStop = false,
        };
        _keyComboBox = new ComboBox
        {
            DropDownStyle = ComboBoxStyle.DropDownList,
            Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right,
            AutoSize = false,
            IntegralHeight = false,
            DrawMode = DrawMode.OwnerDrawFixed,
            Margin = new Padding(0),
        };
        _keyComboBox.Items.AddRange(AutomationKeyMap.TriggerKeyNames.Cast<object>().ToArray());
        if (_keyComboBox.Items.Count > 0)
        {
            _keyComboBox.SelectedIndex = 0;
        }
        _keyComboBox.AutoSize = false;
        _keyComboBox.IntegralHeight = false;
        _keyComboBox.ItemHeight = TriggerControlHeight - 6;
        _keyComboBox.Height = TriggerControlHeight;
        _keyComboBox.MinimumSize = new Size(120, TriggerControlHeight);
        _keyComboBox.DrawItem += (_, eventArgs) =>
        {
            eventArgs.DrawBackground();
            if (eventArgs.Index >= 0)
            {
                Brush textBrush = (eventArgs.State & DrawItemState.Selected) != 0
                    ? SystemBrushes.HighlightText
                    : SystemBrushes.ControlText;
                eventArgs.Graphics.DrawString(
                    _keyComboBox.GetItemText(_keyComboBox.Items[eventArgs.Index]),
                    eventArgs.Font ?? _keyComboBox.Font,
                    textBrush,
                    eventArgs.Bounds);
            }
            eventArgs.DrawFocusRectangle();
        };

        Button addButton = new()
        {
            Text = "添加按键",
            Dock = DockStyle.Fill,
            Margin = new Padding(0),
            AutoSize = false,
            Height = TriggerControlHeight,
            MinimumSize = new Size(88, TriggerControlHeight),
            TextAlign = ContentAlignment.MiddleCenter,
            UseVisualStyleBackColor = true,
        };
        Button clearButton = new()
        {
            Text = "清空",
            Dock = DockStyle.Fill,
            Margin = new Padding(0),
            AutoSize = false,
            Height = TriggerControlHeight,
            MinimumSize = new Size(60, TriggerControlHeight),
            TextAlign = ContentAlignment.MiddleCenter,
            UseVisualStyleBackColor = true,
        };
        addButton.Click += (_, _) => AddSelectedKey();
        clearButton.Click += (_, _) => ClearHotkey();

        TableLayoutPanel layout = new()
        {
            Dock = DockStyle.Fill,
            Margin = new Padding(0),
            Padding = new Padding(0),
            ColumnCount = 4,
            RowCount = 1,
            AutoSize = false,
        };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 128));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 92));
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 62));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.Controls.Add(_display, 0, 0);
        layout.Controls.Add(_keyComboBox, 1, 0);
        layout.Controls.Add(addButton, 2, 0);
        layout.Controls.Add(clearButton, 3, 0);
        layout.Layout += (_, _) =>
        {
            int rowHeight = layout.ClientSize.Height;
            if (rowHeight > 0 && _keyComboBox.Height != rowHeight)
            {
                _keyComboBox.SetBounds(_keyComboBox.Left, 0, _keyComboBox.Width, rowHeight);
            }
        };
        Controls.Add(layout);
    }

    internal string Value => string.Join('+', _keys);

    internal void SetHotkey(string? text)
    {
        _keys.Clear();
        foreach (string rawPart in (text ?? string.Empty).Split('+', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            string part = rawPart.ToLowerInvariant();
            if (!_keys.Contains(part, StringComparer.OrdinalIgnoreCase))
            {
                _keys.Add(part);
            }
        }
        RefreshDisplay();
    }

    internal void ClearHotkey()
    {
        _keys.Clear();
        RefreshDisplay();
    }

    private void AddSelectedKey()
    {
        if (_keyComboBox.SelectedItem is not string key ||
            _keys.Contains(key, StringComparer.OrdinalIgnoreCase))
        {
            return;
        }
        _keys.Add(key.ToLowerInvariant());
        RefreshDisplay();
    }

    private void RefreshDisplay()
    {
        _display.Text = Value;
        _display.SelectionStart = _display.TextLength;
    }
}

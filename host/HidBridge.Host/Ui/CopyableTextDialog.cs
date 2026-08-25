namespace HidBridge.Host.Ui;

internal sealed class CopyableTextDialog : Form
{
    private static readonly Color SurfaceColor = Color.White;
    private static readonly Color BorderColor = Color.FromArgb(211, 221, 234);
    private static readonly Color PrimaryTextColor = Color.FromArgb(23, 35, 58);
    private static readonly Color EditorSurfaceColor = Color.FromArgb(249, 251, 254);

    internal CopyableTextDialog(string title, string text)
    {
        Text = title;
        AutoScaleMode = AutoScaleMode.Dpi;
        StartPosition = FormStartPosition.CenterParent;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MinimizeBox = false;
        MaximizeBox = false;
        ShowInTaskbar = false;
        MinimumSize = new Size(520, 340);
        ClientSize = new Size(640, 420);
        BackColor = SurfaceColor;
        Padding = new Padding(14);

        Label heading = new()
        {
            Text = title,
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = PrimaryTextColor,
            Font = new Font(Font.FontFamily, 12, FontStyle.Bold),
        };
        CloseButton = new Button
        {
            Text = "关闭",
            AutoSize = false,
            Width = 84,
            Height = 32,
            DialogResult = DialogResult.OK,
            BackColor = Color.FromArgb(237, 243, 250),
            ForeColor = PrimaryTextColor,
            UseVisualStyleBackColor = false,
            Dock = DockStyle.Right,
        };
        Panel headingPanel = new()
        {
            Dock = DockStyle.Top,
            Height = 38,
            BackColor = SurfaceColor,
        };
        headingPanel.Controls.Add(heading);
        headingPanel.Controls.Add(CloseButton);

        ContentTextBox = new TextBox
        {
            Dock = DockStyle.Fill,
            Multiline = true,
            ReadOnly = true,
            AcceptsTab = true,
            WordWrap = false,
            ScrollBars = ScrollBars.Both,
            ShortcutsEnabled = true,
            Text = text,
            Font = new Font("Cascadia Mono", 10),
            BackColor = EditorSurfaceColor,
            ForeColor = PrimaryTextColor,
            BorderStyle = BorderStyle.FixedSingle,
        };

        Controls.Add(ContentTextBox);
        Controls.Add(headingPanel);
        AcceptButton = CloseButton;
        CancelButton = CloseButton;
        Shown += (_, _) =>
        {
            ContentTextBox.Select(0, 0);
            ContentTextBox.Focus();
        };
    }

    internal TextBox ContentTextBox { get; }
    internal Button CloseButton { get; }
}

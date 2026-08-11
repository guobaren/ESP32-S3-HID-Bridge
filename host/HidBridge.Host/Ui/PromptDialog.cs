namespace HidBridge.Host.Ui;

internal static class PromptDialog
{
    internal static string? Show(IWin32Window owner, string title, string label)
    {
        using Form dialog = new()
        {
            Text = title,
            StartPosition = FormStartPosition.CenterParent,
            ClientSize = new Size(420, 132),
            FormBorderStyle = FormBorderStyle.FixedDialog,
            MinimizeBox = false,
            MaximizeBox = false,
            ShowInTaskbar = false,
            Font = new Font("Microsoft YaHei UI", 10),
        };
        Label prompt = new()
        {
            Dock = DockStyle.Top,
            Height = 38,
            Padding = new Padding(12, 10, 12, 0),
            Text = label,
        };
        TextBox input = new()
        {
            Left = 12,
            Top = 42,
            Width = 396,
        };
        Button cancel = new()
        {
            Text = "取消",
            DialogResult = DialogResult.Cancel,
            Left = 248,
            Top = 84,
            Width = 76,
        };
        Button confirm = new()
        {
            Text = "确定",
            DialogResult = DialogResult.OK,
            Left = 332,
            Top = 84,
            Width = 76,
        };
        dialog.Controls.AddRange([prompt, input, cancel, confirm]);
        dialog.AcceptButton = confirm;
        dialog.CancelButton = cancel;
        dialog.Shown += (_, _) => input.Focus();
        return dialog.ShowDialog(owner) == DialogResult.OK
            ? input.Text.Trim()
            : null;
    }
}

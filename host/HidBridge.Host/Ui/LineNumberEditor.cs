using System.Globalization;
using System.Runtime.InteropServices;

namespace HidBridge.Host.Ui;

/// <summary>
/// 带行号栏的 Lua 编辑器容器。编辑文本本身仍由调用方持有，行号栏只负责绘制视口中的行号。
/// </summary>
internal sealed class LineNumberEditor : UserControl
{
    private readonly LineNumberTextBox _editor;
    private readonly LineNumberGutter _gutter;

    internal LineNumberEditor(LineNumberTextBox editor)
    {
        _editor = editor;
        _gutter = new LineNumberGutter(editor)
        {
            Dock = DockStyle.Left,
        };
        Dock = DockStyle.Fill;
        BackColor = editor.BackColor;
        Controls.Add(editor);
        Controls.Add(_gutter);

        editor.Dock = DockStyle.Fill;
        editor.TextChanged += (_, _) => RefreshGutter();
        editor.FontChanged += (_, _) => RefreshGutter();
        editor.SizeChanged += (_, _) => _gutter.Invalidate();
        editor.ViewportChanged += (_, _) => _gutter.Invalidate();
        RefreshGutter();
    }

    internal Control Gutter => _gutter;

    private void RefreshGutter()
    {
        _gutter.UpdateWidth();
        _gutter.Invalidate();
    }
}

/// <summary>
/// 发送滚动、键盘和尺寸变化通知，使行号栏在编辑器视口变化时重绘。
/// </summary>
internal class LineNumberTextBox : TextBox
{
    private const int WmKeyDown = 0x0100;
    private const int WmLButtonUp = 0x0202;
    private const int WmMouseWheel = 0x020A;
    private const int WmSize = 0x0005;
    private const int WmVScroll = 0x0115;
    private const int WmPaste = 0x0302;

    internal event EventHandler? ViewportChanged;

    protected override void WndProc(ref Message message)
    {
        if (message.Msg == WmPaste && Clipboard.ContainsText())
        {
            SelectedText = LuaPageControl.NormalizeEditorNewlines(Clipboard.GetText());
            ViewportChanged?.Invoke(this, EventArgs.Empty);
            return;
        }

        base.WndProc(ref message);
        if (message.Msg is WmKeyDown or WmLButtonUp or WmMouseWheel or WmSize or WmVScroll)
        {
            ViewportChanged?.Invoke(this, EventArgs.Empty);
        }
    }
}

internal sealed class LineNumberGutter : Panel
{
    private const int EmGetFirstVisibleLine = 0x00CE;

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern int SendMessage(
        IntPtr hWnd,
        int message,
        IntPtr wParam,
        IntPtr lParam);

    private readonly LineNumberTextBox _editor;

    internal LineNumberGutter(LineNumberTextBox editor)
    {
        _editor = editor;
        DoubleBuffered = true;
        TabStop = false;
        BackColor = Color.FromArgb(238, 241, 246);
        ForeColor = Color.FromArgb(105, 115, 130);
        Width = 38;
        editor.BackColorChanged += (_, _) => BackColor = BlendWithEditor(editor.BackColor);
        editor.ForeColorChanged += (_, _) => Invalidate();
    }

    internal void UpdateWidth()
    {
        int lineCount = Math.Max(1, _editor.Lines.Length);
        int digits = lineCount.ToString(CultureInfo.InvariantCulture).Length;
        int textWidth = TextRenderer.MeasureText(
            new string('8', digits),
            _editor.Font,
            Size.Empty,
            TextFormatFlags.NoPadding).Width;
        Width = Math.Max(34, textWidth + 14);
    }

    protected override void OnPaint(PaintEventArgs eventArgs)
    {
        base.OnPaint(eventArgs);
        eventArgs.Graphics.Clear(BackColor);
        if (!_editor.IsHandleCreated)
        {
            return;
        }

        int lineHeight = Math.Max(1, _editor.Font.Height);
        int firstLine = Math.Max(0, SendMessage(
            _editor.Handle,
            EmGetFirstVisibleLine,
            IntPtr.Zero,
            IntPtr.Zero));
        int lineCount = Math.Max(1, _editor.Lines.Length);
        int visibleLineCount = Math.Max(1, Height / lineHeight + 2);
        for (int offset = 0; offset < visibleLineCount; offset++)
        {
            int lineNumber = firstLine + offset + 1;
            if (lineNumber > lineCount)
            {
                break;
            }

            Rectangle bounds = new(2, offset * lineHeight, Width - 6, lineHeight);
            TextRenderer.DrawText(
                eventArgs.Graphics,
                lineNumber.ToString(CultureInfo.InvariantCulture),
                _editor.Font,
                bounds,
                ForeColor,
                TextFormatFlags.Right | TextFormatFlags.NoPadding | TextFormatFlags.VerticalCenter);
        }
    }

    private static Color BlendWithEditor(Color editorBackColor) =>
        editorBackColor == Color.Empty
            ? Color.FromArgb(238, 241, 246)
            : Color.FromArgb(
                Math.Max(0, editorBackColor.R - 11),
                Math.Max(0, editorBackColor.G - 9),
                Math.Max(0, editorBackColor.B - 4));
}

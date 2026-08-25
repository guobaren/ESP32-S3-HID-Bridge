using System.Runtime.InteropServices;

namespace HidBridge.Host.Ui;

/// <summary>
/// 向只读日志框追加内容，同时保留用户当前的选区和查看位置。
/// </summary>
internal static class LogTextBoxAppender
{
    private const int EmGetFirstVisibleLine = 0x00CE;
    private const int EmLineScroll = 0x00B6;
    private const int SbVert = 1;
    private const uint SifRange = 0x0001;
    private const uint SifPage = 0x0002;
    private const uint SifPos = 0x0004;

    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern int SendMessage(
        IntPtr hWnd,
        int message,
        IntPtr wParam,
        IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetScrollInfo(
        IntPtr hWnd,
        int nBar,
        ref ScrollInfo scrollInfo);

    internal static void Append(
        TextBox textBox,
        IReadOnlyList<string> lines,
        int maximumVisibleCharacters)
    {
        if (textBox.IsDisposed || lines.Count == 0)
        {
            return;
        }

        bool followLatest = ShouldFollowLatest(textBox);
        int selectionStart = textBox.SelectionStart;
        int selectionLength = textBox.SelectionLength;
        int firstVisibleLine = GetFirstVisibleLine(textBox);

        textBox.AppendText(string.Join(Environment.NewLine, lines) + Environment.NewLine);
        (int removedCharacters, int removedLines) = TrimIfNeeded(textBox, maximumVisibleCharacters);

        if (followLatest)
        {
            textBox.SelectionStart = textBox.TextLength;
            textBox.SelectionLength = 0;
            textBox.ScrollToCaret();
            return;
        }

        RestoreSelection(textBox, selectionStart, selectionLength, removedCharacters);
        RestoreFirstVisibleLine(
            textBox,
            Math.Max(0, firstVisibleLine - removedLines));
    }

    private static bool ShouldFollowLatest(TextBox textBox)
    {
        if (textBox.TextLength == 0)
        {
            return true;
        }

        // 活动选区必须保持不动，避免用户拖选部分日志时被追加操作打断。
        // 零长度插入点不影响判断：只要视图仍在底部，就应继续跟随最新日志。
        if (textBox.SelectionLength > 0)
        {
            return false;
        }

        if (TryGetVerticalScrollInfo(textBox, out ScrollInfo scrollInfo))
        {
            // nPos 到达可滚动范围末尾时才自动跟随，避免用户停在倒数几行历史记录时被跳走。
            return (long)scrollInfo.nPos + Math.Max(1u, scrollInfo.nPage) >= (long)scrollInfo.nMax + 1;
        }

        // 没有垂直滚动条信息时，退回到 Win32 的当前首可见行判断；该路径与插入点无关。
        int firstVisibleLine = GetFirstVisibleLine(textBox);
        int lastTextLine = textBox.GetLineFromCharIndex(textBox.TextLength);
        int visibleLineCount = Math.Max(1, textBox.ClientSize.Height / Math.Max(1, textBox.Font.Height));
        return firstVisibleLine + visibleLineCount > lastTextLine;
    }

    private static (int RemovedCharacters, int RemovedLines) TrimIfNeeded(
        TextBox textBox,
        int maximumVisibleCharacters)
    {
        int excess = textBox.TextLength - maximumVisibleCharacters;
        if (excess <= 0)
        {
            return (0, 0);
        }

        string text = textBox.Text;
        int cutAt = text.IndexOf('\n', excess);
        if (cutAt < 0)
        {
            return (0, 0);
        }

        int removeCount = cutAt + 1;
        int removedLines = 0;
        for (int index = 0; index < removeCount; index++)
        {
            if (text[index] == '\n')
            {
                removedLines++;
            }
        }

        textBox.Select(0, removeCount);
        textBox.SelectedText = string.Empty;
        return (removeCount, removedLines);
    }

    private static void RestoreSelection(
        TextBox textBox,
        int selectionStart,
        int selectionLength,
        int removedCharacters)
    {
        int originalEnd = selectionStart + selectionLength;
        int restoredStart = Math.Max(0, selectionStart - removedCharacters);
        int restoredEnd = Math.Max(0, originalEnd - removedCharacters);
        restoredStart = Math.Min(restoredStart, textBox.TextLength);
        restoredEnd = Math.Clamp(restoredEnd, restoredStart, textBox.TextLength);
        textBox.SelectionStart = restoredStart;
        textBox.SelectionLength = restoredEnd - restoredStart;
    }

    private static int GetFirstVisibleLine(TextBox textBox) =>
        SendMessage(
            textBox.Handle,
            EmGetFirstVisibleLine,
            IntPtr.Zero,
            IntPtr.Zero);

    private static bool TryGetVerticalScrollInfo(TextBox textBox, out ScrollInfo scrollInfo)
    {
        scrollInfo = new ScrollInfo
        {
            cbSize = (uint)Marshal.SizeOf<ScrollInfo>(),
            fMask = SifRange | SifPage | SifPos,
        };
        return GetScrollInfo(textBox.Handle, SbVert, ref scrollInfo) && scrollInfo.nPage > 0;
    }

    private static void RestoreFirstVisibleLine(TextBox textBox, int firstVisibleLine)
    {
        if (firstVisibleLine < 0)
        {
            return;
        }

        int currentFirstVisibleLine = GetFirstVisibleLine(textBox);
        int lineDelta = firstVisibleLine - currentFirstVisibleLine;
        if (lineDelta != 0)
        {
            SendMessage(
                textBox.Handle,
                EmLineScroll,
                IntPtr.Zero,
                new IntPtr(lineDelta));
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ScrollInfo
    {
        public uint cbSize;
        public uint fMask;
        public int nMin;
        public int nMax;
        public uint nPage;
        public int nPos;
        public int nTrackPos;
    }
}

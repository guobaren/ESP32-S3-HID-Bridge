using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace HidBridge.Host.Ui;

/// <summary>
/// 向只读日志框追加内容，同时保留用户当前的选区和查看位置。
///
/// 跟随策略（2026-09-28 重写）：
///   默认**持续跟随最新行**；只有"用户自己滚动"才会把跟随关掉；用户一旦滚回最底部，
///   立刻恢复跟随。程序自身的追加与截断**永不改变**该状态。
///
/// 为什么不能再用滚动条位置去推断意图：截断会同时改动 nMax 与 nPos，判定结果会在
/// "在底部 / 不在底部"之间反复翻转，表现成视图在首行与尾行之间来回跳。
/// </summary>
internal static class LogTextBoxAppender
{
    private const int EmGetFirstVisibleLine = 0x00CE;
    private const int EmLineScroll = 0x00B6;
    private const int SbVert = 1;
    private const uint SifRange = 0x0001;
    private const uint SifPage = 0x0002;
    private const uint SifPos = 0x0004;

    /// <summary>每个日志框一份跟随状态；用弱表挂靠，随控件一起回收。</summary>
    private sealed class FollowState
    {
        public bool FollowLatest = true;

        /// <summary>程序上一次操作结束时留下的滚动位置；与当前值不符即说明用户滚动过。</summary>
        public int LastKnownPosition = -1;

        /// <summary>事件钩子只挂一次。</summary>
        public bool HookInstalled;
    }

    private static readonly ConditionalWeakTable<TextBox, FollowState> States = new();

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

        FollowState state = GetState(textBox);
        int selectionStart = textBox.SelectionStart;
        int selectionLength = textBox.SelectionLength;
        int firstVisibleLine = GetFirstVisibleLine(textBox);

        // 首次追加时没有程序位置基线。活动选区代表用户正在查看历史内容，
        // 即使控件当前没有可滚动范围也必须保留选区。
        if (state.LastKnownPosition < 0)
        {
            state.FollowLatest = selectionLength == 0 && IsAtBottom(textBox);
        }
        else if (selectionLength > 0)
        {
            // 活动选区优先级最高，即使控件位置恰好与上次程序位置相同。
            state.FollowLatest = false;
        }

        // 追加之前先看：滚动位置是否被"我们不在场时"的某次用户操作改掉了。
        // 这样拖动滚动条、键盘翻页、滚轮都能覆盖到，无需依赖具体事件。
        if (state.LastKnownPosition >= 0 &&
            TryGetVerticalScrollInfo(textBox, out ScrollInfo before) &&
            before.nPos != state.LastKnownPosition)
        {
            state.FollowLatest = IsAtBottom(textBox);
        }

        textBox.AppendText(string.Join(Environment.NewLine, lines) + Environment.NewLine);
        (int removedCharacters, int removedLines) = TrimIfNeeded(textBox, maximumVisibleCharacters);

        if (state.FollowLatest)
        {
            textBox.SelectionStart = textBox.TextLength;
            textBox.SelectionLength = 0;
            textBox.ScrollToCaret();
        }
        else
        {
            RestoreSelection(textBox, selectionStart, selectionLength, removedCharacters);
            RestoreFirstVisibleLine(
                textBox,
                Math.Max(0, firstVisibleLine - removedLines));
        }

        // 记录本次程序操作后留下的位置，作为下一次"用户是否滚动过"的基准。
        // 必须在截断与滚动之后记录，否则截断引起的位移会被误判成用户操作。
        if (TryGetVerticalScrollInfo(textBox, out ScrollInfo after))
        {
            state.LastKnownPosition = after.nPos;
        }
    }

    private static FollowState GetState(TextBox textBox)
    {
        FollowState state = States.GetOrCreateValue(textBox);
        if (state.HookInstalled)
        {
            return state;
        }

        state.HookInstalled = true;
        // 滚轮与键盘即时更新跟随状态（拖动滚动条由 Append 时的位置比较兜底）。
        textBox.MouseWheel += (_, _) => OnUserScroll(textBox);
        textBox.KeyDown += (_, _) => OnUserScroll(textBox);
        return state;
    }

    private static void OnUserScroll(TextBox textBox)
    {
        if (textBox.IsDisposed)
        {
            return;
        }

        FollowState state = States.GetOrCreateValue(textBox);
        // 用户滚回最底部就恢复跟随，停在别处则暂停跟随。
        state.FollowLatest = IsAtBottom(textBox);
    }

    private static bool IsAtBottom(TextBox textBox)
    {
        if (textBox.TextLength == 0)
        {
            return true;
        }

        if (TryGetVerticalScrollInfo(textBox, out ScrollInfo scrollInfo))
        {
            // nPos 到达可滚动范围末尾时视为停在最新行。
            return (long)scrollInfo.nPos + Math.Max(1u, scrollInfo.nPage) >= (long)scrollInfo.nMax + 1;
        }

        // 没有滚动条信息时退回 Win32 首可见行判断。
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

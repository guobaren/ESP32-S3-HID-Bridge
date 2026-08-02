using System.ComponentModel;
using System.Runtime.InteropServices;
using HidBridge.Host.Transport;
using HidBridge.Protocol;

namespace HidBridge.Host.Input;

internal sealed class InputForwarder : IDisposable
{
    private const uint VkEnd = 0x23;
    private const uint VkHome = 0x24;
    private const byte LeftButton = 1 << 0;
    private const byte RightButton = 1 << 1;
    private const byte MiddleButton = 1 << 2;
    private const byte BackButton = 1 << 3;
    private const byte ForwardButton = 1 << 4;

    private readonly NativeMethods.HookProc _keyboardProc;
    private readonly NativeMethods.HookProc _mouseProc;
    private readonly MouseReportPump _mouseReportPump;
    private readonly List<byte> _pressedKeys = [];
    private IntPtr _keyboardHook;
    private IntPtr _mouseHook;
    private RawMouseInputWindow? _rawMouseInput;
    private byte _modifiers;
    private byte _mouseButtons;
    private int _verticalWheelRemainder;
    private int _horizontalWheelRemainder;
    private bool _ignoreHotkeyChord;
    private uint _ignoredHotkeyKey;
    private bool _started;

    internal InputForwarder(IBridgeTransport transport)
    {
        _keyboardProc = KeyboardCallback;
        _mouseProc = MouseCallback;
        _mouseReportPump = new MouseReportPump(transport);
    }

    internal bool ForwardingEnabled => _mouseReportPump.Enabled;

    internal event EventHandler<bool>? ForwardingChanged;
    internal event EventHandler? ExitRequested;

    internal void DisableForwarding() => SetForwarding(false, true);

    internal void Start()
    {
        if (_started)
        {
            return;
        }

        _rawMouseInput = new RawMouseInputWindow((_, input) => HandleRawMouseInput(input));

        IntPtr module = NativeMethods.GetModuleHandle(null);
        _keyboardHook = NativeMethods.SetWindowsHookEx(
            NativeMethods.WhKeyboardLl,
            _keyboardProc,
            module,
            0);
        _mouseHook = NativeMethods.SetWindowsHookEx(
            NativeMethods.WhMouseLl,
            _mouseProc,
            module,
            0);

        if (_keyboardHook == IntPtr.Zero || _mouseHook == IntPtr.Zero)
        {
            Stop();
            throw new Win32Exception(Marshal.GetLastWin32Error(), "无法安装全局输入钩子。");
        }

        _started = true;
    }

    internal void Stop()
    {
        if (!_started && _keyboardHook == IntPtr.Zero && _mouseHook == IntPtr.Zero && _rawMouseInput is null)
        {
            return;
        }

        SetForwarding(false, true);

        if (_keyboardHook != IntPtr.Zero)
        {
            NativeMethods.UnhookWindowsHookEx(_keyboardHook);
            _keyboardHook = IntPtr.Zero;
        }

        if (_mouseHook != IntPtr.Zero)
        {
            NativeMethods.UnhookWindowsHookEx(_mouseHook);
            _mouseHook = IntPtr.Zero;
        }

        _rawMouseInput?.Dispose();
        _rawMouseInput = null;
        _started = false;
    }

    private IntPtr KeyboardCallback(int code, IntPtr wParam, IntPtr lParam)
    {
        if (code != NativeMethods.HcAction)
        {
            return NativeMethods.CallNextHookEx(_keyboardHook, code, wParam, lParam);
        }

        int message = unchecked((int)wParam);
        bool isDown = message is NativeMethods.WmKeyDown or NativeMethods.WmSysKeyDown;
        bool isUp = message is NativeMethods.WmKeyUp or NativeMethods.WmSysKeyUp;
        if (!isDown && !isUp)
        {
            return NativeMethods.CallNextHookEx(_keyboardHook, code, wParam, lParam);
        }

        NativeMethods.KeyboardHookData data =
            Marshal.PtrToStructure<NativeMethods.KeyboardHookData>(lParam);
        uint virtualKey = data.VirtualKey;

        bool isToggleHotkey = isDown && virtualKey == VkHome;
        bool isExitHotkey = isDown && virtualKey == VkEnd;

        if (isToggleHotkey || isExitHotkey)
        {
            _ignoreHotkeyChord = true;
            _ignoredHotkeyKey = virtualKey;
            if (isToggleHotkey)
            {
                SetForwarding(!ForwardingEnabled);
            }
            else
            {
                SetForwarding(false, true);
                ExitRequested?.Invoke(this, EventArgs.Empty);
            }

            return (IntPtr)1;
        }

        if (_ignoreHotkeyChord)
        {
            if (isUp && virtualKey == _ignoredHotkeyKey)
            {
                _ignoreHotkeyChord = false;
                _ignoredHotkeyKey = 0;
            }

            return (IntPtr)1;
        }

        UpdateKeyboardState(
            virtualKey,
            (data.Flags & NativeMethods.LlkhfExtended) != 0,
            isDown);

        if (ForwardingEnabled)
        {
            // 修饰键在按下事件到达时立即发送，避免 Ctrl/Alt 组合出现首个按键延迟。
            SendKeyboardReport();
        }

        if (ShouldSuppressKeyboard(ForwardingEnabled, false, false))
        {
            return (IntPtr)1;
        }

        return NativeMethods.CallNextHookEx(_keyboardHook, code, wParam, lParam);
    }

    private IntPtr MouseCallback(int code, IntPtr wParam, IntPtr lParam)
    {
        if (code != NativeMethods.HcAction)
        {
            return NativeMethods.CallNextHookEx(_mouseHook, code, wParam, lParam);
        }

        // 低级钩子只负责可选的本地抑制。移动、按钮和滚轮统一由
        // Raw Input 按设备事件顺序处理，避免两条输入路径之间状态错序。
        if (ShouldSuppressMouse(ForwardingEnabled))
        {
            return (IntPtr)1;
        }

        return NativeMethods.CallNextHookEx(_mouseHook, code, wParam, lParam);
    }

    private void UpdateKeyboardState(uint virtualKey, bool extended, bool isDown)
    {
        if (VirtualKeyToHid.TryGetModifier(virtualKey, out byte modifier))
        {
            if (isDown)
            {
                _modifiers |= modifier;
            }
            else
            {
                _modifiers &= unchecked((byte)~modifier);
            }

            return;
        }

        if (!VirtualKeyToHid.TryGetUsage(virtualKey, extended, out byte usage))
        {
            return;
        }

        if (isDown)
        {
            if (!_pressedKeys.Contains(usage))
            {
                _pressedKeys.Add(usage);
            }
        }
        else
        {
            _pressedKeys.Remove(usage);
        }
    }

    private void SetForwarding(bool enabled, bool forceRelease = false)
    {
        if (!forceRelease && ForwardingEnabled == enabled)
        {
            return;
        }

        ClearInputState();
        _mouseReportPump.ResetAndSendRelease(enabled);
        ForwardingChanged?.Invoke(this, enabled);
    }

    private void ClearInputState()
    {
        _pressedKeys.Clear();
        _modifiers = 0;
        _mouseButtons = 0;
        _verticalWheelRemainder = 0;
        _horizontalWheelRemainder = 0;
    }

    private void HandleRawMouseInput(NativeMethods.RawMouse input)
    {
        if (!ForwardingEnabled)
        {
            return;
        }


        ushort flags = input.ButtonFlags;
        byte previousButtons = _mouseButtons;
        UpdateMouseButton(flags, NativeMethods.RawMouseLeftButtonDown, NativeMethods.RawMouseLeftButtonUp, LeftButton);
        UpdateMouseButton(flags, NativeMethods.RawMouseRightButtonDown, NativeMethods.RawMouseRightButtonUp, RightButton);
        UpdateMouseButton(flags, NativeMethods.RawMouseMiddleButtonDown, NativeMethods.RawMouseMiddleButtonUp, MiddleButton);
        UpdateMouseButton(flags, NativeMethods.RawMouseButton4Down, NativeMethods.RawMouseButton4Up, BackButton);
        UpdateMouseButton(flags, NativeMethods.RawMouseButton5Down, NativeMethods.RawMouseButton5Up, ForwardButton);

        int wheel = 0;
        int pan = 0;
        if ((flags & NativeMethods.RawMouseWheel) != 0)
        {
            wheel = ConsumeWheelDelta(input.ButtonData, ref _verticalWheelRemainder);
        }
        else if ((flags & NativeMethods.RawMouseHorizontalWheel) != 0)
        {
            pan = ConsumeWheelDelta(input.ButtonData, ref _horizontalWheelRemainder);
        }

        bool hasButtonChange = previousButtons != _mouseButtons;
        if (input.LastX != 0 || input.LastY != 0 || wheel != 0 || pan != 0 || hasButtonChange)
        {
            _mouseReportPump.Accumulate(
                _mouseButtons,
                hasButtonChange,
                input.LastX,
                input.LastY,
                wheel,
                pan);
        }
    }

    private void UpdateMouseButton(ushort flags, ushort downFlag, ushort upFlag, byte button)
    {
        if ((flags & downFlag) != 0)
        {
            _mouseButtons |= button;
        }
        if ((flags & upFlag) != 0)
        {
            _mouseButtons &= unchecked((byte)~button);
        }
    }

    private static int ConsumeWheelDelta(short delta, ref int remainder)
    {
        int total = remainder + delta;
        int steps = total / 120;
        remainder = total % 120;
        return steps;
    }

    internal static bool ShouldSuppressKeyboard(
        bool forwardingEnabled,
        bool handlingControlHotkey,
        bool hotkeyStartedWhileForwarding) =>
        handlingControlHotkey ? hotkeyStartedWhileForwarding : forwardingEnabled;

    internal static bool ShouldSuppressMouse(bool forwardingEnabled) => forwardingEnabled;


    private void SendKeyboardReport()
    {
        Span<byte> report = stackalloc byte[8];
        report[0] = _modifiers;
        int count = Math.Min(6, _pressedKeys.Count);
        for (int index = 0; index < count; index++)
        {
            report[index + 2] = _pressedKeys[index];
        }

        _mouseReportPump.SendKeyboard(report);
    }

    public void Dispose()
    {
        Stop();
        _mouseReportPump.Dispose();
    }
}

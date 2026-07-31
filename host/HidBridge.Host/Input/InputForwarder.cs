using System.ComponentModel;
using System.Runtime.InteropServices;
using HidBridge.Host.Protocol;
using HidBridge.Host.Transport;

namespace HidBridge.Host.Input;

internal sealed class InputForwarder : IDisposable
{
    private const uint VkF11 = 0x7A;
    private const uint VkF12 = 0x7B;
    private const byte LeftButton = 1 << 0;
    private const byte RightButton = 1 << 1;
    private const byte MiddleButton = 1 << 2;
    private const byte BackButton = 1 << 3;
    private const byte ForwardButton = 1 << 4;

    private readonly SerialBridge _transport;
    private readonly bool _suppressLocalInput;
    private readonly NativeMethods.HookProc _keyboardProc;
    private readonly NativeMethods.HookProc _mouseProc;
    private readonly List<byte> _pressedKeys = [];
    private IntPtr _keyboardHook;
    private IntPtr _mouseHook;
    private RawMouseInputWindow? _rawMouseInput;
    private byte _modifiers;
    private byte _mouseButtons;
    private int _verticalWheelRemainder;
    private int _horizontalWheelRemainder;
    private bool _ignoreHotkeyChord;
    private bool _started;

    internal InputForwarder(SerialBridge transport, bool suppressLocalInput)
    {
        _transport = transport;
        _suppressLocalInput = suppressLocalInput;
        _keyboardProc = KeyboardCallback;
        _mouseProc = MouseCallback;
    }

    internal bool ForwardingEnabled { get; private set; }

    internal event EventHandler<bool>? ForwardingChanged;
    internal event EventHandler? ExitRequested;

    internal void Start()
    {
        if (_started)
        {
            return;
        }

        _rawMouseInput = new RawMouseInputWindow(HandleRawMouseInput);

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
        if (ForwardingEnabled)
        {
            SetForwarding(false);
        }
        else
        {
            ReleaseAll();
        }

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

        bool ctrlDown = (_modifiers & ((1 << 0) | (1 << 4))) != 0;
        bool altDown = (_modifiers & ((1 << 2) | (1 << 6))) != 0;
        bool isToggleHotkey = isDown && virtualKey == VkF12 && ctrlDown && altDown;
        bool isExitHotkey = isDown && virtualKey == VkF11 && ctrlDown && altDown;

        if (isToggleHotkey || isExitHotkey)
        {
            _ignoreHotkeyChord = true;
            ReleaseAll();

            if (isToggleHotkey)
            {
                SetForwarding(!ForwardingEnabled);
            }
            else
            {
                ExitRequested?.Invoke(this, EventArgs.Empty);
            }

            return IntPtr.Zero;
        }

        if (_ignoreHotkeyChord)
        {
            if (isUp && virtualKey is 0xA2 or 0xA3 or 0xA4 or 0xA5)
            {
                bool releasingCtrl = virtualKey is 0xA2 or 0xA3;
                bool releasingAlt = virtualKey is 0xA4 or 0xA5;
                if (releasingCtrl || releasingAlt)
                {
                    _ignoreHotkeyChord = false;
                }
            }

            return IntPtr.Zero;
        }

        UpdateKeyboardState(
            virtualKey,
            (data.Flags & NativeMethods.LlkhfExtended) != 0,
            isDown);

        if (ForwardingEnabled)
        {
            SendKeyboardReport();
            if (_suppressLocalInput)
            {
                return (IntPtr)1;
            }
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
        if (ForwardingEnabled && _suppressLocalInput)
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

    private void SetForwarding(bool enabled)
    {
        if (ForwardingEnabled == enabled)
        {
            return;
        }

        ReleaseAll();
        ForwardingEnabled = enabled;
        ForwardingChanged?.Invoke(this, enabled);
    }

    private void ReleaseAll()
    {
        _pressedKeys.Clear();
        _modifiers = 0;
        _mouseButtons = 0;
        _verticalWheelRemainder = 0;
        _horizontalWheelRemainder = 0;
        _transport.Send(MessageType.ReleaseAll, ReadOnlySpan<byte>.Empty);
    }

    private void HandleRawMouseInput(NativeMethods.RawMouse input)
    {
        if (!ForwardingEnabled)
        {
            return;
        }

        ushort flags = input.ButtonFlags;
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

        bool hasButtonChange = (flags & 0x03FF) != 0;
        if (input.LastX != 0 || input.LastY != 0 || wheel != 0 || pan != 0 || hasButtonChange)
        {
            SendMouseReports(input.LastX, input.LastY, wheel, pan);
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

    private void SendKeyboardReport()
    {
        Span<byte> report = stackalloc byte[8];
        report[0] = _modifiers;
        int count = Math.Min(6, _pressedKeys.Count);
        for (int index = 0; index < count; index++)
        {
            report[index + 2] = _pressedKeys[index];
        }

        _transport.Send(MessageType.KeyboardReport, report);
    }

    private void SendMouseReports(int deltaX, int deltaY, int wheel, int pan)
    {
        Span<byte> report = stackalloc byte[5];
        do
        {
            int stepX = Math.Clamp(deltaX, -127, 127);
            int stepY = Math.Clamp(deltaY, -127, 127);
            int stepWheel = Math.Clamp(wheel, -127, 127);
            int stepPan = Math.Clamp(pan, -127, 127);
            report[0] = _mouseButtons;
            report[1] = unchecked((byte)(sbyte)stepX);
            report[2] = unchecked((byte)(sbyte)stepY);
            report[3] = unchecked((byte)(sbyte)stepWheel);
            report[4] = unchecked((byte)(sbyte)stepPan);
            _transport.Send(MessageType.MouseReport, report);
            deltaX -= stepX;
            deltaY -= stepY;
            wheel -= stepWheel;
            pan -= stepPan;
        }
        while (deltaX != 0 || deltaY != 0 || wheel != 0 || pan != 0);
    }

    public void Dispose()
    {
        Stop();
    }
}

using System.ComponentModel;
using System.Runtime.InteropServices;

namespace HidBridge.Host.Automation;

internal sealed class Win32AutomationOutput : IAutomationOutput
{
    private const uint InputMouse = 0;
    private const uint InputKeyboard = 1;
    private const uint KeyEventExtendedKey = 0x0001;
    private const uint KeyEventKeyUp = 0x0002;
    private const uint MouseMove = 0x0001;
    private const uint MouseLeftDown = 0x0002;
    private const uint MouseLeftUp = 0x0004;
    private const uint MouseRightDown = 0x0008;
    private const uint MouseRightUp = 0x0010;
    private const uint MouseMiddleDown = 0x0020;
    private const uint MouseMiddleUp = 0x0040;
    private const uint MouseWheel = 0x0800;
    private const uint MouseXDown = 0x0080;
    private const uint MouseXUp = 0x0100;
    private const uint XButton1 = 0x0001;
    private const uint XButton2 = 0x0002;
    private readonly object _stateLock = new();
    private readonly HashSet<int> _pressedButtons = [];
    private readonly HashSet<byte> _pressedKeys = [];
    private long _releaseAllCount;

    public bool IsRemote => false;

    internal long ReleaseAllCount => Interlocked.Read(ref _releaseAllCount);

    public Point GetCursorPosition()
    {
        if (!GetCursorPos(out NativePoint point))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "无法读取鼠标位置。");
        }
        return new Point(point.X, point.Y);
    }

    public void MoveRelative(int deltaX, int deltaY) =>
        SendMouse(MouseMove, deltaX, deltaY, 0);

    public void MoveAbsolute(int x, int y)
    {
        if (!SetCursorPos(x, y))
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Win32 绝对移动失败。");
        }
    }

    public void SetMouseButton(int button, bool pressed)
    {
        (uint flags, uint data) = button switch
        {
            1 => (pressed ? MouseLeftDown : MouseLeftUp, 0u),
            2 => (pressed ? MouseMiddleDown : MouseMiddleUp, 0u),
            3 => (pressed ? MouseRightDown : MouseRightUp, 0u),
            4 => (pressed ? MouseXDown : MouseXUp, XButton1),
            5 => (pressed ? MouseXDown : MouseXUp, XButton2),
            _ => throw new ArgumentOutOfRangeException(nameof(button), "鼠标按钮必须为 1-5。"),
        };
        lock (_stateLock)
        {
            bool wasPressed = _pressedButtons.Contains(button);
            if (pressed)
            {
                _pressedButtons.Add(button);
            }
            else
            {
                _pressedButtons.Remove(button);
            }
            try
            {
                SendMouse(flags, 0, 0, data);
            }
            catch
            {
                RestoreState(_pressedButtons, button, wasPressed);
                throw;
            }
        }
    }

    public void Wheel(int delta) => SendMouse(MouseWheel, 0, 0, unchecked((uint)delta));

    public void KeyDown(byte hidUsage) => SetKey(hidUsage, true);

    public void KeyUp(byte hidUsage) => SetKey(hidUsage, false);

    internal void ReleaseAll()
    {
        lock (_stateLock)
        {
            List<NativeInput> inputs =
            [
                CreateMouseInput(MouseLeftUp, 0, 0, 0),
                CreateMouseInput(MouseMiddleUp, 0, 0, 0),
                CreateMouseInput(MouseRightUp, 0, 0, 0),
                CreateMouseInput(MouseXUp, 0, 0, XButton1),
                CreateMouseInput(MouseXUp, 0, 0, XButton2),
            ];
            // 全局键盘 Hook 在捕获期间可能已经拦截过实体修饰键的 KeyUp；这些键不在
            // 自动化状态表中。每次进入/退出捕获都主动发送八个修饰键的 KeyUp，才能
            // 修复控制端 Windows 已经形成的 Shift/Ctrl/Alt/Win 卡键状态。
            IEnumerable<byte> keysToRelease = _pressedKeys
                .Concat(Enumerable.Range(224, 8).Select(value => unchecked((byte)value)))
                .Distinct();
            inputs.AddRange(keysToRelease.Select(hidUsage => CreateKeyInput(hidUsage, true)));
            Send(inputs.ToArray());
            _pressedButtons.Clear();
            _pressedKeys.Clear();
            Interlocked.Increment(ref _releaseAllCount);
        }
    }

    private void SetKey(byte hidUsage, bool pressed)
    {
        lock (_stateLock)
        {
            bool wasPressed = _pressedKeys.Contains(hidUsage);
            if (pressed)
            {
                _pressedKeys.Add(hidUsage);
            }
            else
            {
                _pressedKeys.Remove(hidUsage);
            }
            try
            {
                Send([CreateKeyInput(hidUsage, !pressed)]);
            }
            catch
            {
                RestoreState(_pressedKeys, hidUsage, wasPressed);
                throw;
            }
        }
    }

    private static NativeInput CreateKeyInput(byte hidUsage, bool keyUp)
    {
        if (!AutomationKeyMap.TryGetVirtualKeyForHid(hidUsage, out ushort virtualKey))
        {
            throw new InvalidOperationException($"HID Usage {hidUsage} 没有可用的 Win32 VK 映射。");
        }
        return new NativeInput
        {
            Type = InputKeyboard,
            Union = new InputUnion
            {
                Keyboard = new KeyboardInput
                {
                    VirtualKey = virtualKey,
                    Flags = (keyUp ? KeyEventKeyUp : 0) |
                            (IsExtendedHidUsage(hidUsage) ? KeyEventExtendedKey : 0),
                },
            },
        };
    }

    private static bool IsExtendedHidUsage(byte usage) =>
        usage is 70 or >= 73 and <= 82 or 84 or 88 or 227 or 228 or 230 or 231;

    private static void SendMouse(uint flags, int x, int y, uint data)
        => Send([CreateMouseInput(flags, x, y, data)]);

    private static NativeInput CreateMouseInput(uint flags, int x, int y, uint data)
    {
        return new NativeInput
        {
            Type = InputMouse,
            Union = new InputUnion
            {
                Mouse = new MouseInput
                {
                    X = x,
                    Y = y,
                    MouseData = data,
                    Flags = flags,
                },
            },
        };
    }

    private static void RestoreState<T>(HashSet<T> state, T value, bool contained)
    {
        if (contained)
        {
            state.Add(value);
        }
        else
        {
            state.Remove(value);
        }
    }

    private static void Send(NativeInput[] inputs)
    {
        uint sent = SendInput(unchecked((uint)inputs.Length), inputs, Marshal.SizeOf<NativeInput>());
        if (sent != inputs.Length)
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Win32 SendInput 调用失败。");
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativePoint
    {
        internal int X;
        internal int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeInput
    {
        internal uint Type;
        internal InputUnion Union;
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct InputUnion
    {
        [FieldOffset(0)]
        internal MouseInput Mouse;

        [FieldOffset(0)]
        internal KeyboardInput Keyboard;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MouseInput
    {
        internal int X;
        internal int Y;
        internal uint MouseData;
        internal uint Flags;
        internal uint Time;
        internal UIntPtr ExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct KeyboardInput
    {
        internal ushort VirtualKey;
        internal ushort ScanCode;
        internal uint Flags;
        internal uint Time;
        internal UIntPtr ExtraInfo;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint inputCount, [In] NativeInput[] inputs, int inputSize);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetCursorPos(out NativePoint point);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetCursorPos(int x, int y);
}

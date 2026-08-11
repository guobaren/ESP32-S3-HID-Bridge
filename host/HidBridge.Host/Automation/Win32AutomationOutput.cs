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

    public bool IsRemote => false;

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
        SendMouse(flags, 0, 0, data);
    }

    public void Wheel(int delta) => SendMouse(MouseWheel, 0, 0, unchecked((uint)delta));

    public void KeyDown(byte hidUsage) => SendKey(hidUsage, false);

    public void KeyUp(byte hidUsage) => SendKey(hidUsage, true);

    private static void SendKey(byte hidUsage, bool keyUp)
    {
        if (!AutomationKeyMap.TryGetVirtualKeyForHid(hidUsage, out ushort virtualKey))
        {
            throw new InvalidOperationException($"HID Usage {hidUsage} 没有可用的 Win32 VK 映射。");
        }
        NativeInput input = new()
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
        Send([input]);
    }

    private static bool IsExtendedHidUsage(byte usage) =>
        usage is 70 or >= 73 and <= 82 or 84 or 88 or 227 or 228 or 230 or 231;

    private static void SendMouse(uint flags, int x, int y, uint data)
    {
        NativeInput input = new()
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
        Send([input]);
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

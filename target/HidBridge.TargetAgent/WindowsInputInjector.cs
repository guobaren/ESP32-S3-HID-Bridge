using System.ComponentModel;
using System.Runtime.InteropServices;

namespace HidBridge.TargetAgent;

internal sealed class WindowsInputInjector
{
    private const uint InputMouse = 0;
    private const uint InputKeyboard = 1;
    private const uint KeyeventfExtendedkey = 0x0001;
    private const uint KeyeventfKeyup = 0x0002;
    private const uint KeyeventfScancode = 0x0008;
    private const uint MouseeventfMove = 0x0001;
    private const uint MouseeventfLeftdown = 0x0002;
    private const uint MouseeventfLeftup = 0x0004;
    private const uint MouseeventfRightdown = 0x0008;
    private const uint MouseeventfRightup = 0x0010;
    private const uint MouseeventfMiddledown = 0x0020;
    private const uint MouseeventfMiddleup = 0x0040;
    private const uint MouseeventfXdown = 0x0080;
    private const uint MouseeventfXup = 0x0100;
    private const uint MouseeventfWheel = 0x0800;
    private const uint MouseeventfHwheel = 0x1000;
    private const uint Xbutton1 = 0x0001;
    private const uint Xbutton2 = 0x0002;

    private readonly HashSet<byte> _pressedKeys = [];
    private byte _modifiers;
    private byte _mouseButtons;

    internal void ApplyKeyboardReport(ReadOnlySpan<byte> report)
    {
        if (report.Length != 8)
        {
            return;
        }

        byte newModifiers = report[0];
        for (int bit = 0; bit < 8; bit++)
        {
            byte mask = (byte)(1 << bit);
            bool wasDown = (_modifiers & mask) != 0;
            bool isDown = (newModifiers & mask) != 0;
            if (wasDown != isDown)
            {
                SendKeyboardUsage((byte)(0xE0 + bit), isDown);
            }
        }

        HashSet<byte> newKeys = report[2..]
            .ToArray()
            .Where(key => key != 0)
            .ToHashSet();
        foreach (byte key in _pressedKeys.Except(newKeys).ToArray())
        {
            SendKeyboardUsage(key, false);
        }
        foreach (byte key in newKeys.Except(_pressedKeys))
        {
            SendKeyboardUsage(key, true);
        }

        _pressedKeys.Clear();
        _pressedKeys.UnionWith(newKeys);
        _modifiers = newModifiers;
    }

    internal void ApplyMouseReport(ReadOnlySpan<byte> report)
    {
        if (report.Length != 5)
        {
            return;
        }

        byte buttons = report[0];
        SendButtonChange(0x01, buttons, MouseeventfLeftdown, MouseeventfLeftup, 0);
        SendButtonChange(0x02, buttons, MouseeventfRightdown, MouseeventfRightup, 0);
        SendButtonChange(0x04, buttons, MouseeventfMiddledown, MouseeventfMiddleup, 0);
        SendButtonChange(0x08, buttons, MouseeventfXdown, MouseeventfXup, Xbutton1);
        SendButtonChange(0x10, buttons, MouseeventfXdown, MouseeventfXup, Xbutton2);
        _mouseButtons = buttons;

        int x = unchecked((sbyte)report[1]);
        int y = unchecked((sbyte)report[2]);
        int wheel = unchecked((sbyte)report[3]);
        int horizontalWheel = unchecked((sbyte)report[4]);
        if (x != 0 || y != 0)
        {
            SendMouse(MouseeventfMove, 0, x, y);
        }
        if (wheel != 0)
        {
            SendMouse(MouseeventfWheel, unchecked((uint)(wheel * 120)), 0, 0);
        }
        if (horizontalWheel != 0)
        {
            SendMouse(MouseeventfHwheel, unchecked((uint)(horizontalWheel * 120)), 0, 0);
        }
    }

    internal void ReleaseAll()
    {
        foreach (byte key in _pressedKeys.ToArray())
        {
            SendKeyboardUsage(key, false);
        }
        for (int bit = 0; bit < 8; bit++)
        {
            if ((_modifiers & (1 << bit)) != 0)
            {
                SendKeyboardUsage((byte)(0xE0 + bit), false);
            }
        }

        SendButtonRelease(0x01, MouseeventfLeftup, 0);
        SendButtonRelease(0x02, MouseeventfRightup, 0);
        SendButtonRelease(0x04, MouseeventfMiddleup, 0);
        SendButtonRelease(0x08, MouseeventfXup, Xbutton1);
        SendButtonRelease(0x10, MouseeventfXup, Xbutton2);
        _pressedKeys.Clear();
        _modifiers = 0;
        _mouseButtons = 0;
    }

    private void SendButtonChange(byte mask, byte buttons, uint downFlag, uint upFlag, uint data)
    {
        bool wasDown = (_mouseButtons & mask) != 0;
        bool isDown = (buttons & mask) != 0;
        if (wasDown != isDown)
        {
            SendMouse(isDown ? downFlag : upFlag, data, 0, 0);
        }
    }

    private void SendButtonRelease(byte mask, uint upFlag, uint data)
    {
        if ((_mouseButtons & mask) != 0)
        {
            SendMouse(upFlag, data, 0, 0);
        }
    }

    private static void SendKeyboardUsage(byte usage, bool isDown)
    {
        if (!TryGetScanCode(usage, out ushort scanCode, out bool extended))
        {
            return;
        }

        uint flags = KeyeventfScancode;
        if (extended)
        {
            flags |= KeyeventfExtendedkey;
        }
        if (!isDown)
        {
            flags |= KeyeventfKeyup;
        }

        Input input = new()
        {
            Type = InputKeyboard,
            Union = new InputUnion
            {
                Keyboard = new KeyboardInput { Scan = scanCode, Flags = flags },
            },
        };
        SendSingleInput(input);
    }

    private static void SendMouse(uint flags, uint data, int x, int y)
    {
        Input input = new()
        {
            Type = InputMouse,
            Union = new InputUnion
            {
                Mouse = new MouseInput { X = x, Y = y, MouseData = data, Flags = flags },
            },
        };
        SendSingleInput(input);
    }

    private static void SendSingleInput(Input input)
    {
        Input[] inputs = [input];
        if (SendInput(1, inputs, Marshal.SizeOf<Input>()) != 1)
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "无法注入目标键鼠事件。");
        }
    }

    private static bool TryGetScanCode(byte usage, out ushort scanCode, out bool extended)
    {
        extended = false;
        scanCode = usage switch
        {
            0x04 => 0x1E,
            0x05 => 0x30,
            0x06 => 0x2E,
            0x07 => 0x20,
            0x08 => 0x12,
            0x09 => 0x21,
            0x0A => 0x22,
            0x0B => 0x23,
            0x0C => 0x17,
            0x0D => 0x24,
            0x0E => 0x25,
            0x0F => 0x26,
            0x10 => 0x32,
            0x11 => 0x31,
            0x12 => 0x18,
            0x13 => 0x19,
            0x14 => 0x10,
            0x15 => 0x13,
            0x16 => 0x1F,
            0x17 => 0x14,
            0x18 => 0x16,
            0x19 => 0x2F,
            0x1A => 0x11,
            0x1B => 0x2D,
            0x1C => 0x15,
            0x1D => 0x2C,
            0x1E => 0x02,
            0x1F => 0x03,
            0x20 => 0x04,
            0x21 => 0x05,
            0x22 => 0x06,
            0x23 => 0x07,
            0x24 => 0x08,
            0x25 => 0x09,
            0x26 => 0x0A,
            0x27 => 0x0B,
            0x28 => 0x1C,
            0x29 => 0x01,
            0x2A => 0x0E,
            0x2B => 0x0F,
            0x2C => 0x39,
            0x2D => 0x0C,
            0x2E => 0x0D,
            0x2F => 0x1A,
            0x30 => 0x1B,
            0x31 => 0x2B,
            0x33 => 0x27,
            0x34 => 0x28,
            0x35 => 0x29,
            0x36 => 0x33,
            0x37 => 0x34,
            0x38 => 0x35,
            0x39 => 0x3A,
            >= 0x3A and <= 0x43 => (ushort)(0x3B + usage - 0x3A),
            0x44 => 0x57,
            0x45 => 0x58,
            0x47 => 0x46,
            0x53 => 0x45,
            0x54 => 0x35,
            0x55 => 0x37,
            0x56 => 0x4A,
            0x57 => 0x4E,
            0x58 => 0x1C,
            0x59 => 0x4F,
            0x5A => 0x50,
            0x5B => 0x51,
            0x5C => 0x4B,
            0x5D => 0x4C,
            0x5E => 0x4D,
            0x5F => 0x47,
            0x60 => 0x48,
            0x61 => 0x49,
            0x62 => 0x52,
            0x63 => 0x53,
            0xE0 => 0x1D,
            0xE1 => 0x2A,
            0xE2 => 0x38,
            0xE3 => 0x5B,
            0xE4 => 0x1D,
            0xE5 => 0x36,
            0xE6 => 0x38,
            0xE7 => 0x5C,
            _ => 0,
        };

        extended = usage is >= 0x49 and <= 0x52 or 0x54 or 0x58 or >= 0xE3 and <= 0xE4 or >= 0xE6 and <= 0xE7;
        return scanCode != 0;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Input
    {
        internal uint Type;
        internal InputUnion Union;
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct InputUnion
    {
        [FieldOffset(0)] internal MouseInput Mouse;
        [FieldOffset(0)] internal KeyboardInput Keyboard;
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
        internal ushort Scan;
        internal uint Flags;
        internal uint Time;
        internal UIntPtr ExtraInfo;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint inputCount, [In] Input[] inputs, int inputSize);
}

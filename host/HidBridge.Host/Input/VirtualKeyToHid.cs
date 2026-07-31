namespace HidBridge.Host.Input;

internal static class VirtualKeyToHid
{
    internal static bool TryGetUsage(uint virtualKey, bool extended, out byte usage)
    {
        if (virtualKey is >= 0x41 and <= 0x5A)
        {
            usage = (byte)(0x04 + virtualKey - 0x41);
            return true;
        }

        if (virtualKey is >= 0x31 and <= 0x39)
        {
            usage = (byte)(0x1E + virtualKey - 0x31);
            return true;
        }

        if (virtualKey is >= 0x70 and <= 0x7B)
        {
            usage = (byte)(0x3A + virtualKey - 0x70);
            return true;
        }

        if (virtualKey is >= 0x61 and <= 0x69)
        {
            usage = (byte)(0x59 + virtualKey - 0x61);
            return true;
        }

        usage = virtualKey switch
        {
            0x30 => 0x27, // 0
            0x0D when extended => 0x58, // 小键盘 Enter
            0x0D => 0x28,
            0x1B => 0x29,
            0x08 => 0x2A,
            0x09 => 0x2B,
            0x20 => 0x2C,
            0xBD => 0x2D,
            0xBB => 0x2E,
            0xDB => 0x2F,
            0xDD => 0x30,
            0xDC => 0x31,
            0xBA => 0x33,
            0xDE => 0x34,
            0xC0 => 0x35,
            0xBC => 0x36,
            0xBE => 0x37,
            0xBF => 0x38,
            0x14 => 0x39,
            0x2C => 0x46,
            0x91 => 0x47,
            0x13 => 0x48,
            0x2D => 0x49,
            0x24 => 0x4A,
            0x21 => 0x4B,
            0x2E => 0x4C,
            0x23 => 0x4D,
            0x22 => 0x4E,
            0x27 => 0x4F,
            0x25 => 0x50,
            0x28 => 0x51,
            0x26 => 0x52,
            0x90 => 0x53,
            0x6F => 0x54,
            0x6A => 0x55,
            0x6D => 0x56,
            0x6B => 0x57,
            0x60 => 0x62,
            0x6E => 0x63,
            _ => 0,
        };

        return usage != 0;
    }

    internal static bool TryGetModifier(uint virtualKey, out byte mask)
    {
        mask = virtualKey switch
        {
            0xA2 => 1 << 0, // Left Ctrl
            0xA0 => 1 << 1, // Left Shift
            0xA4 => 1 << 2, // Left Alt
            0x5B => 1 << 3, // Left GUI
            0xA3 => 1 << 4, // Right Ctrl
            0xA1 => 1 << 5, // Right Shift
            0xA5 => 1 << 6, // Right Alt
            0x5C => 1 << 7, // Right GUI
            _ => 0,
        };
        return mask != 0;
    }
}

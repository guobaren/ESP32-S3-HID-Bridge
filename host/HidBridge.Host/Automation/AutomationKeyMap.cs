namespace HidBridge.Host.Automation;

internal static class AutomationKeyMap
{
    private static readonly Dictionary<string, uint> VirtualKeys = BuildVirtualKeys();
    private static readonly Dictionary<uint, string> VirtualKeyNames = VirtualKeys
        .GroupBy(pair => pair.Value)
        .ToDictionary(group => group.Key, group => group.First().Key);
    private static readonly Dictionary<string, byte> HidUsages = BuildHidUsages();
    private static readonly Dictionary<byte, ushort> HidToVirtualKey = BuildHidToVirtualKey();

    internal static IReadOnlyList<string> TriggerKeyNames { get; } = BuildTriggerKeyNames();

    internal static bool TryGetVirtualKey(string name, out uint virtualKey) =>
        VirtualKeys.TryGetValue(name.Trim(), out virtualKey);

    internal static string GetEventName(uint virtualKey) =>
        VirtualKeyNames.TryGetValue(virtualKey, out string? name)
            ? name
            : $"vk_{virtualKey:#04x}";

    internal static object GetLuaEventArgument(uint virtualKey)
    {
        if (virtualKey == 0x01)
        {
            return 1;
        }
        if (virtualKey == 0x04)
        {
            return 2;
        }
        if (virtualKey == 0x02)
        {
            return 3;
        }
        if (virtualKey == 0x05)
        {
            return 4;
        }
        if (virtualKey == 0x06)
        {
            return 5;
        }
        return VirtualKeyNames.TryGetValue(virtualKey, out string? name) ? name : virtualKey;
    }

    internal static bool TryResolveScriptVirtualKey(object? argument, out uint virtualKey)
    {
        if (argument is string name)
        {
            return TryGetVirtualKey(name, out virtualKey);
        }
        try
        {
            int value = Convert.ToInt32(argument);
            virtualKey = value switch
            {
                1 => 0x01,
                2 => 0x04,
                3 => 0x02,
                4 => 0x05,
                5 => 0x06,
                _ => unchecked((uint)value),
            };
            return true;
        }
        catch (Exception) when (argument is not null)
        {
            virtualKey = 0;
            return false;
        }
    }

    internal static byte ResolveHid(string token)
    {
        string normalized = token.Trim().Trim('"', '\'').ToLowerInvariant();
        if (byte.TryParse(normalized, out byte usage))
        {
            return usage;
        }
        if (HidUsages.TryGetValue(normalized, out usage))
        {
            return usage;
        }
        throw new FormatException($"未知按键：{token}");
    }

    internal static bool TryGetVirtualKeyForHid(byte usage, out ushort virtualKey) =>
        HidToVirtualKey.TryGetValue(usage, out virtualKey);

    internal static IReadOnlyDictionary<string, uint> LuaVirtualKeys => VirtualKeys;

    private static IReadOnlyList<string> BuildTriggerKeyNames()
    {
        string[] mouseButtons = ["mouse_left", "mouse_middle", "mouse_right", "mouse_side1", "mouse_side2"];
        string[] modifiers = [
            "ctrl", "lctrl", "rctrl", "shift", "lshift", "rshift", "alt", "lalt", "ralt", "lwin", "rwin",
        ];
        string[] control = [
            "pause", "scrolllock", "numlock", "capslock", "esc", "tab", "space", "enter", "backspace",
            "insert", "delete", "home", "end", "pageup", "pagedown", "left", "up", "right", "down",
            "printscreen", "apps",
        ];
        string[] functionKeys = Enumerable.Range(1, 24).Select(index => $"f{index}").ToArray();
        string[] numpad = [
            ..Enumerable.Range(0, 10).Select(index => $"num{index}"),
            "nummultiply", "numadd", "numsubtract", "numdecimal", "numdivide",
        ];
        string[] letters = Enumerable.Range('a', 26).Select(value => ((char)value).ToString()).ToArray();
        string[] digits = Enumerable.Range(0, 10).Select(value => value.ToString()).ToArray();

        HashSet<string> seen = new(StringComparer.OrdinalIgnoreCase);
        List<string> ordered = [];
        foreach (string name in mouseButtons.Concat(modifiers).Concat(control).Concat(functionKeys).Concat(numpad).Concat(letters).Concat(digits).Concat(VirtualKeys.Keys))
        {
            if (VirtualKeys.ContainsKey(name) && seen.Add(name))
            {
                ordered.Add(name);
            }
        }
        return ordered;
    }

    private static Dictionary<string, uint> BuildVirtualKeys()
    {
        Dictionary<string, uint> result = new(StringComparer.OrdinalIgnoreCase)
        {
            ["mouse_left"] = 0x01,
            ["mouse_right"] = 0x02,
            ["mouse_middle"] = 0x04,
            ["mouse_side1"] = 0x05,
            ["mouse_side2"] = 0x06,
            ["ctrl"] = 0x11,
            ["lctrl"] = 0xA2,
            ["rctrl"] = 0xA3,
            ["shift"] = 0x10,
            ["lshift"] = 0xA0,
            ["rshift"] = 0xA1,
            ["alt"] = 0x12,
            ["lalt"] = 0xA4,
            ["ralt"] = 0xA5,
            ["lwin"] = 0x5B,
            ["rwin"] = 0x5C,
            ["pause"] = 0x13,
            ["capslock"] = 0x14,
            ["esc"] = 0x1B,
            ["space"] = 0x20,
            ["pageup"] = 0x21,
            ["pagedown"] = 0x22,
            ["end"] = 0x23,
            ["home"] = 0x24,
            ["left"] = 0x25,
            ["up"] = 0x26,
            ["right"] = 0x27,
            ["down"] = 0x28,
            ["printscreen"] = 0x2C,
            ["insert"] = 0x2D,
            ["delete"] = 0x2E,
            ["backspace"] = 0x08,
            ["tab"] = 0x09,
            ["enter"] = 0x0D,
            ["numlock"] = 0x90,
            ["scrolllock"] = 0x91,
            ["apps"] = 0x5D,
        };
        for (char letter = 'a'; letter <= 'z'; letter++)
        {
            result[letter.ToString()] = char.ToUpperInvariant(letter);
        }
        for (int digit = 0; digit <= 9; digit++)
        {
            result[digit.ToString()] = unchecked((uint)('0' + digit));
            result[$"num{digit}"] = unchecked((uint)(0x60 + digit));
        }
        for (int index = 1; index <= 24; index++)
        {
            result[$"f{index}"] = unchecked((uint)(0x70 + index - 1));
        }
        result["nummultiply"] = 0x6A;
        result["numadd"] = 0x6B;
        result["numsubtract"] = 0x6D;
        result["numdecimal"] = 0x6E;
        result["numdivide"] = 0x6F;
        return result;
    }

    private static Dictionary<string, byte> BuildHidUsages()
    {
        Dictionary<string, byte> result = new(StringComparer.OrdinalIgnoreCase);
        for (int index = 0; index < 26; index++)
        {
            result[((char)('a' + index)).ToString()] = unchecked((byte)(4 + index));
        }
        for (int digit = 1; digit <= 9; digit++)
        {
            result[digit.ToString()] = unchecked((byte)(29 + digit));
        }
        result["0"] = 39;
        result["enter"] = 40;
        result["esc"] = 41;
        result["backspace"] = 42;
        result["tab"] = 43;
        result["space"] = 44;
        result["minus"] = 45;
        result["equal"] = 46;
        result["lbracket"] = 47;
        result["rbracket"] = 48;
        result["backslash"] = 49;
        result["semicolon"] = 51;
        result["quote"] = 52;
        result["grave"] = 53;
        result["comma"] = 54;
        result["period"] = 55;
        result["slash"] = 56;
        result["capslock"] = 57;
        for (int index = 1; index <= 12; index++)
        {
            result[$"f{index}"] = unchecked((byte)(57 + index));
        }
        result["printscreen"] = 70;
        result["scrolllock"] = 71;
        result["pause"] = 72;
        result["insert"] = 73;
        result["home"] = 74;
        result["pageup"] = 75;
        result["delete"] = 76;
        result["end"] = 77;
        result["pagedown"] = 78;
        result["right"] = 79;
        result["left"] = 80;
        result["down"] = 81;
        result["up"] = 82;
        result["numlock"] = 83;
        result["numdivide"] = 84;
        result["nummultiply"] = 85;
        result["numsubtract"] = 86;
        result["numadd"] = 87;
        result["numenter"] = 88;
        for (int digit = 1; digit <= 9; digit++)
        {
            result[$"num{digit}"] = unchecked((byte)(88 + digit));
        }
        result["num0"] = 98;
        result["numdecimal"] = 99;
        for (int index = 13; index <= 24; index++)
        {
            result[$"f{index}"] = unchecked((byte)(104 + index - 13));
        }
        result["ctrl"] = result["lctrl"] = 224;
        result["shift"] = result["lshift"] = 225;
        result["alt"] = result["lalt"] = 226;
        result["win"] = result["lwin"] = 227;
        result["rctrl"] = 228;
        result["rshift"] = 229;
        result["ralt"] = 230;
        result["rwin"] = 231;
        return result;
    }

    private static Dictionary<byte, ushort> BuildHidToVirtualKey()
    {
        Dictionary<byte, ushort> result = [];
        foreach ((string name, byte usage) in HidUsages)
        {
            if (VirtualKeys.TryGetValue(name, out uint virtualKey) && virtualKey <= ushort.MaxValue)
            {
                result.TryAdd(usage, unchecked((ushort)virtualKey));
            }
        }
        return result;
    }
}

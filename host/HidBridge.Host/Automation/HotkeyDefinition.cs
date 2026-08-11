namespace HidBridge.Host.Automation;

internal sealed class HotkeyDefinition
{
    private static readonly Dictionary<string, string[]> GenericModifiers =
        new(StringComparer.OrdinalIgnoreCase)
        {
            ["ctrl"] = ["ctrl", "lctrl", "rctrl"],
            ["shift"] = ["shift", "lshift", "rshift"],
            ["alt"] = ["alt", "lalt", "ralt"],
        };

    private readonly IReadOnlyList<HashSet<uint>> _groups;

    private HotkeyDefinition(string display, IReadOnlyList<HashSet<uint>> groups)
    {
        Display = display;
        _groups = groups;
    }

    internal string Display { get; }

    internal bool IsSatisfiedBy(IReadOnlySet<uint> held) =>
        _groups.Count > 0 && _groups.All(group => group.Any(held.Contains));

    internal static HotkeyDefinition Parse(string text)
    {
        List<HashSet<uint>> groups = [];
        List<string> display = [];
        foreach (string rawPart in text.Split('+', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            string part = rawPart.ToLowerInvariant();
            string[] names = GenericModifiers.TryGetValue(part, out string[]? alternatives)
                ? alternatives
                : [part];
            HashSet<uint> group = [];
            foreach (string name in names)
            {
                if (!AutomationKeyMap.TryGetVirtualKey(name, out uint virtualKey))
                {
                    throw new FormatException($"未知触发键：{part}");
                }
                group.Add(virtualKey);
            }
            groups.Add(group);
            display.Add(part);
        }
        if (groups.Count == 0)
        {
            throw new FormatException("触发键不能为空。");
        }
        return new HotkeyDefinition(string.Join('+', display), groups);
    }
}

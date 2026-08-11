using System.Text.Json.Serialization;

namespace HidBridge.Host.Automation;

internal static class MacroRunModes
{
    internal const string Once = "once";
    internal const string Toggle = "toggle";
    internal const string HoldLoop = "hold_loop";
    internal const string Staged = "staged";

    internal static readonly string[] All = [Once, Toggle, HoldLoop, Staged];
}

internal sealed class MacroDefinition
{
    [JsonIgnore]
    internal string Name { get; set; } = string.Empty;

    [JsonPropertyName("trigger")]
    public string Trigger { get; set; } = string.Empty;

    [JsonPropertyName("mode")]
    public string Mode { get; set; } = MacroRunModes.Once;

    [JsonPropertyName("enabled")]
    public bool Enabled { get; set; } = true;

    [JsonIgnore]
    internal string Text { get; set; } = string.Empty;
}

internal sealed class AutomationProfile
{
    [JsonIgnore]
    internal string Name { get; set; } = string.Empty;

    [JsonPropertyName("apps")]
    public List<string> Apps { get; set; } = [];

    [JsonPropertyName("lua_script_text")]
    public string LuaScriptText { get; set; } = string.Empty;

    [JsonPropertyName("macros")]
    public Dictionary<string, MacroDefinition> Macros { get; set; } =
        new(StringComparer.OrdinalIgnoreCase);
}

internal sealed class AutomationSettings
{
    public string ActiveProfile { get; set; } = AutomationProfileStore.GlobalProfile;
    public bool StartOnBoot { get; set; }
    public bool MinimizeToTray { get; set; } = true;
    public bool CloseToTray { get; set; } = true;
    public bool GenerateMovementAnalysisImage { get; set; } = true;
    public bool FirmwareUpdateApiEnabled { get; set; }
    public int WindowWidth { get; set; } = 1080;
    public int WindowHeight { get; set; } = 760;
}

internal readonly record struct PhysicalInputEvent(
    IReadOnlySet<uint> HeldKeys,
    uint VirtualKey,
    bool Pressed);

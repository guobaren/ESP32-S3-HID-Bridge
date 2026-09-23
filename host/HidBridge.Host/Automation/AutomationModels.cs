using System.Text.Json.Serialization;
using HidBridge.Host.Input;

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

    [JsonPropertyName("file")]
    public string ScriptFile { get; set; } = string.Empty;

    [JsonIgnore]
    internal string Text { get; set; } = string.Empty;
}

internal sealed class AutomationProfile
{
    [JsonIgnore]
    internal string Name { get; set; } = string.Empty;

    [JsonPropertyName("apps")]
    public List<string> Apps { get; set; } = [];

    [JsonIgnore]
    public string LuaScriptText { get; set; } = string.Empty;

    [JsonPropertyName("lua_script_file")]
    public string LuaScriptFile { get; set; } = "lua/main.txt";

    // 仅用于读取旧版 profile.json。加载后会迁移到 lua_script_file 指向的 txt，
    // 保存新格式时该字段保持 null，不再把脚本正文写回 JSON。
    [JsonPropertyName("lua_script_text")]
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)]
    public string? LegacyLuaScriptText { get; set; }

    [JsonPropertyName("macros")]
    public Dictionary<string, MacroDefinition> Macros { get; set; } =
        new(StringComparer.OrdinalIgnoreCase);
}

internal sealed class AutomationSettings
{
    public string ActiveProfile { get; set; } = AutomationProfileStore.GlobalProfile;
    public bool StartOnBoot { get; set; }
    public bool MinimizeToTray { get; set; }
    public bool CloseToTray { get; set; } = true;
    public bool GenerateMovementAnalysisImage { get; set; }
    public double OutputSensitivity { get; set; } = MouseOutputSensitivity.Default;
    public bool AlwaysOutputUdpEnabled { get; set; } = true;
    public bool LegacySingleBoardFirmwareCompatibility { get; set; }
    public bool SimulatedUdpInputEnabled { get; set; }
    public int SimulatedUdpInputFrequencyHz { get; set; } = 100;
    public bool FirmwareUpdateApiEnabled { get; set; }
    public string FirmwareManifestPath { get; set; } = string.Empty;
    public string FirmwareFlashPortName { get; set; } = string.Empty;
    public int WindowWidth { get; set; } = 1080;
    public int WindowHeight { get; set; } = 760;
}

internal readonly record struct PhysicalInputEvent(
    IReadOnlySet<uint> HeldKeys,
    uint VirtualKey,
    bool Pressed);

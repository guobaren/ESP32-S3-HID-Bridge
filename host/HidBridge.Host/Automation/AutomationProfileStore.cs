using System.Text.Json;

namespace HidBridge.Host.Automation;

internal sealed class AutomationProfileStore
{
    internal const string GlobalProfile = "Global";
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true,
    };

    private readonly string _profilesDirectory;
    private readonly string _settingsPath;
    private readonly string _legacyImportMarker;

    internal AutomationProfileStore(string? dataDirectory = null)
    {
        string root = dataDirectory ?? AppContext.BaseDirectory;
        _profilesDirectory = Path.Combine(root, "profiles");
        _settingsPath = Path.Combine(root, "automation.settings.json");
        _legacyImportMarker = Path.Combine(_profilesDirectory, ".mousehub-imported");
        Directory.CreateDirectory(_profilesDirectory);
        EnsureProfile(GlobalProfile);
    }

    internal string ProfilesDirectory => _profilesDirectory;

    internal AutomationSettings LoadSettings()
    {
        try
        {
            if (File.Exists(_settingsPath))
            {
                return JsonSerializer.Deserialize<AutomationSettings>(
                           File.ReadAllText(_settingsPath),
                           JsonOptions) ?? new AutomationSettings();
            }
        }
        catch (Exception exception) when (exception is IOException or JsonException)
        {
            Console.Error.WriteLine($"自动化设置读取失败，将使用默认值：{exception.Message}");
        }

        return new AutomationSettings();
    }

    internal void SaveSettings(AutomationSettings settings) =>
        AtomicWrite(_settingsPath, JsonSerializer.Serialize(settings, JsonOptions));

    internal IReadOnlyList<string> ListProfiles()
    {
        EnsureProfile(GlobalProfile);
        List<string> names = Directory.EnumerateDirectories(_profilesDirectory)
            .Select(Path.GetFileName)
            .Where(name => !string.IsNullOrWhiteSpace(name))
            .Cast<string>()
            .OrderBy(name => name, StringComparer.CurrentCultureIgnoreCase)
            .ToList();
        names.RemoveAll(name => name.Equals(GlobalProfile, StringComparison.OrdinalIgnoreCase));
        names.Insert(0, GlobalProfile);
        return names;
    }

    internal AutomationProfile LoadProfile(string name)
    {
        string normalized = NormalizeProfileName(name);
        EnsureProfile(normalized);
        AutomationProfile profile = new() { Name = normalized };
        string metadataPath = GetMetadataPath(normalized);
        try
        {
            if (File.Exists(metadataPath))
            {
                profile = JsonSerializer.Deserialize<AutomationProfile>(
                              File.ReadAllText(metadataPath),
                              JsonOptions) ?? profile;
                profile.Name = normalized;
            }
        }
        catch (Exception exception) when (exception is IOException or JsonException)
        {
            Console.Error.WriteLine($"配置 {normalized} 读取失败：{exception.Message}");
        }

        profile.Apps ??= [];
        profile.Macros = new Dictionary<string, MacroDefinition>(
            profile.Macros ?? new Dictionary<string, MacroDefinition>(),
            StringComparer.OrdinalIgnoreCase);
        foreach (string macroPath in Directory.EnumerateFiles(GetProfileDirectory(normalized), "*.txt"))
        {
            string macroName = Path.GetFileNameWithoutExtension(macroPath);
            if (!profile.Macros.TryGetValue(macroName, out MacroDefinition? macro))
            {
                macro = new MacroDefinition();
                profile.Macros[macroName] = macro;
            }
            macro.Name = macroName;
            macro.Text = File.ReadAllText(macroPath);
            if (!MacroRunModes.All.Contains(macro.Mode, StringComparer.Ordinal))
            {
                macro.Mode = MacroRunModes.Once;
            }
        }

        foreach ((string macroName, MacroDefinition macro) in profile.Macros)
        {
            macro.Name = macroName;
            string macroPath = GetMacroPath(normalized, macroName);
            if (File.Exists(macroPath) && string.IsNullOrEmpty(macro.Text))
            {
                macro.Text = File.ReadAllText(macroPath);
            }
        }
        return profile;
    }

    internal void SaveProfile(AutomationProfile profile)
    {
        string name = NormalizeProfileName(profile.Name);
        profile.Name = name;
        EnsureProfile(name);
        foreach ((string macroName, MacroDefinition macro) in profile.Macros)
        {
            macro.Name = macroName;
            AtomicWrite(GetMacroPath(name, macroName), macro.Text ?? string.Empty);
        }
        AtomicWrite(GetMetadataPath(name), JsonSerializer.Serialize(profile, JsonOptions));
    }

    internal AutomationProfile CreateProfile(string name)
    {
        string normalized = NormalizeProfileName(name);
        if (ListProfiles().Contains(normalized, StringComparer.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException("同名配置已存在。");
        }
        AutomationProfile profile = new() { Name = normalized };
        SaveProfile(profile);
        return profile;
    }

    internal void DeleteProfile(string name)
    {
        string normalized = NormalizeProfileName(name);
        if (normalized.Equals(GlobalProfile, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException("Global 配置不可删除。");
        }
        Directory.Delete(GetProfileDirectory(normalized), true);
    }

    internal void DeleteMacro(string profileName, string macroName)
    {
        string path = GetMacroPath(profileName, macroName);
        if (File.Exists(path))
        {
            File.Delete(path);
        }
    }

    internal bool TryImportLegacyMouseHubProfiles(string legacyProfilesDirectory)
    {
        if (File.Exists(_legacyImportMarker) || !Directory.Exists(legacyProfilesDirectory))
        {
            return false;
        }

        bool imported = false;
        foreach (string sourceDirectory in Directory.EnumerateDirectories(legacyProfilesDirectory))
        {
            string name = Path.GetFileName(sourceDirectory);
            string destinationDirectory = GetProfileDirectory(name);
            Directory.CreateDirectory(destinationDirectory);
            string sourceMetadata = Path.Combine(sourceDirectory, "profile.json");
            string destinationMetadata = Path.Combine(destinationDirectory, "profile.json");
            if (File.Exists(sourceMetadata))
            {
                AutomationProfile sourceProfile = DeserializeFilteredLegacyProfile(name, sourceMetadata);
                AutomationProfile destinationProfile = File.Exists(destinationMetadata)
                    ? DeserializeFilteredLegacyProfile(name, destinationMetadata)
                    : new AutomationProfile { Name = name };
                bool metadataChanged = false;
                if (string.IsNullOrWhiteSpace(destinationProfile.LuaScriptText) &&
                    !string.IsNullOrWhiteSpace(sourceProfile.LuaScriptText))
                {
                    destinationProfile.LuaScriptText = sourceProfile.LuaScriptText;
                    metadataChanged = true;
                }
                if (destinationProfile.Apps.Count == 0 && sourceProfile.Apps.Count > 0)
                {
                    destinationProfile.Apps = sourceProfile.Apps;
                    metadataChanged = true;
                }
                foreach ((string macroName, MacroDefinition macro) in sourceProfile.Macros)
                {
                    if (destinationProfile.Macros.TryAdd(macroName, macro))
                    {
                        metadataChanged = true;
                    }
                }
                if (metadataChanged)
                {
                    AtomicWrite(destinationMetadata, JsonSerializer.Serialize(destinationProfile, JsonOptions));
                    imported = true;
                }
            }
            foreach (string sourceMacro in Directory.EnumerateFiles(sourceDirectory, "*.txt"))
            {
                string destinationMacro = Path.Combine(destinationDirectory, Path.GetFileName(sourceMacro));
                if (!File.Exists(destinationMacro))
                {
                    File.Copy(sourceMacro, destinationMacro);
                    imported = true;
                }
            }
        }
        File.WriteAllText(_legacyImportMarker, DateTimeOffset.Now.ToString("O"));
        return imported;
    }

    private static AutomationProfile DeserializeFilteredLegacyProfile(string name, string path)
    {
        AutomationProfile profile = JsonSerializer.Deserialize<AutomationProfile>(
                                        File.ReadAllText(path),
                                        JsonOptions) ?? new AutomationProfile();
        profile.Name = name;
        profile.Apps ??= [];
        profile.Macros ??= new Dictionary<string, MacroDefinition>(StringComparer.OrdinalIgnoreCase);
        return profile;
    }

    private void EnsureProfile(string name)
    {
        string directory = GetProfileDirectory(name);
        Directory.CreateDirectory(directory);
        string metadata = GetMetadataPath(name);
        if (!File.Exists(metadata))
        {
            AutomationProfile profile = new() { Name = name };
            AtomicWrite(metadata, JsonSerializer.Serialize(profile, JsonOptions));
        }
    }

    private string GetProfileDirectory(string name) =>
        Path.Combine(_profilesDirectory, NormalizeProfileName(name));

    private string GetMetadataPath(string name) => Path.Combine(GetProfileDirectory(name), "profile.json");

    private string GetMacroPath(string profileName, string macroName)
    {
        string normalizedMacro = NormalizeFileName(macroName, "宏名称");
        return Path.Combine(GetProfileDirectory(profileName), normalizedMacro + ".txt");
    }

    private static string NormalizeProfileName(string name) => NormalizeFileName(name, "配置名称");

    private static string NormalizeFileName(string name, string label)
    {
        string normalized = name.Trim();
        if (string.IsNullOrWhiteSpace(normalized) ||
            normalized.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 ||
            normalized is "." or "..")
        {
            throw new ArgumentException($"{label}无效。", nameof(name));
        }
        return normalized;
    }

    private static void AtomicWrite(string path, string content)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        string temporaryPath = path + ".tmp";
        File.WriteAllText(temporaryPath, content);
        File.Move(temporaryPath, path, true);
    }
}

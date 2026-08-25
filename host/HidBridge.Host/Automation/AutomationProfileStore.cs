using System.Text.Json;

namespace HidBridge.Host.Automation;

internal sealed class AutomationProfileStore
{
    internal const string GlobalProfile = "Global";
    internal const string MacroDirectoryName = "macros";
    internal const string LuaDirectoryName = "lua";
    internal const string DefaultLuaScriptFile = "lua/main.txt";
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
        bool needsMigration = !MetadataUsesCurrentLayout(metadataPath);
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

        profile.LuaScriptFile = NormalizeAssociatedFile(
            profile.LuaScriptFile,
            DefaultLuaScriptFile,
            LuaDirectoryName);
        string luaPath = ResolveAssociatedFilePath(normalized, profile.LuaScriptFile, DefaultLuaScriptFile);
        if (File.Exists(luaPath))
        {
            profile.LuaScriptText = File.ReadAllText(luaPath);
        }
        else if (profile.LegacyLuaScriptText is not null)
        {
            profile.LuaScriptText = profile.LegacyLuaScriptText;
            needsMigration = true;
        }
        profile.LegacyLuaScriptText = null;

        foreach ((string macroName, MacroDefinition macro) in profile.Macros)
        {
            macro.Name = macroName;
            string defaultRelativePath = GetDefaultMacroRelativePath(macroName);
            string associatedRelativePath = NormalizeAssociatedFile(
                macro.ScriptFile,
                defaultRelativePath,
                MacroDirectoryName);
            if (!string.Equals(macro.ScriptFile, associatedRelativePath, StringComparison.Ordinal))
            {
                needsMigration = true;
            }
            macro.ScriptFile = associatedRelativePath;
            string macroPath = ResolveAssociatedFilePath(normalized, associatedRelativePath, defaultRelativePath);
            string legacyMacroPath = GetLegacyMacroPath(normalized, macroName);
            if (File.Exists(macroPath))
            {
                macro.Text = File.ReadAllText(macroPath);
            }
            else if (File.Exists(legacyMacroPath))
            {
                macro.Text = File.ReadAllText(legacyMacroPath);
                needsMigration = true;
            }
            if (!MacroRunModes.All.Contains(macro.Mode, StringComparer.Ordinal))
            {
                macro.Mode = MacroRunModes.Once;
            }
        }

        // 旧版宏正文位于配置根目录的 *.txt；读取后加入元数据并在本次打开时迁移。
        foreach (string legacyMacroPath in Directory.EnumerateFiles(GetProfileDirectory(normalized), "*.txt"))
        {
            string macroName = Path.GetFileNameWithoutExtension(legacyMacroPath);
            if (profile.Macros.ContainsKey(macroName))
            {
                continue;
            }
            MacroDefinition macro = new()
            {
                Name = macroName,
                ScriptFile = GetDefaultMacroRelativePath(macroName),
                Text = File.ReadAllText(legacyMacroPath),
            };
            profile.Macros[macroName] = macro;
            needsMigration = true;
        }

        if (needsMigration)
        {
            SaveProfile(profile);
        }
        return profile;
    }

    internal void SaveProfile(AutomationProfile profile)
    {
        string name = NormalizeProfileName(profile.Name);
        profile.Name = name;
        EnsureProfile(name);

        profile.LuaScriptFile = NormalizeAssociatedFile(
            profile.LuaScriptFile,
            DefaultLuaScriptFile,
            LuaDirectoryName);
        string luaPath = ResolveAssociatedFilePath(name, profile.LuaScriptFile, DefaultLuaScriptFile);
        AtomicWrite(luaPath, profile.LuaScriptText ?? string.Empty);

        foreach ((string macroName, MacroDefinition macro) in profile.Macros)
        {
            macro.Name = macroName;
            macro.ScriptFile = NormalizeAssociatedFile(
                macro.ScriptFile,
                GetDefaultMacroRelativePath(macroName),
                MacroDirectoryName);
            string macroPath = ResolveAssociatedFilePath(name, macro.ScriptFile, GetDefaultMacroRelativePath(macroName));
            AtomicWrite(macroPath, macro.Text ?? string.Empty);
        }

        profile.LegacyLuaScriptText = null;
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

    internal void DeleteMacro(string profileName, string macroName, string? associatedFile = null)
    {
        string defaultRelativePath = GetDefaultMacroRelativePath(macroName);
        string path = ResolveAssociatedFilePath(profileName, associatedFile, defaultRelativePath);
        if (File.Exists(path))
        {
            File.Delete(path);
        }
        string defaultPath = ResolveAssociatedFilePath(profileName, defaultRelativePath, defaultRelativePath);
        if (!path.Equals(defaultPath, StringComparison.OrdinalIgnoreCase) && File.Exists(defaultPath))
        {
            File.Delete(defaultPath);
        }
        string legacyPath = GetLegacyMacroPath(profileName, macroName);
        if (File.Exists(legacyPath))
        {
            File.Delete(legacyPath);
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
            string sourceMetadata = Path.Combine(sourceDirectory, "profile.json");
            AutomationProfile destinationProfile = LoadProfile(name);
            bool profileChanged = false;
            if (File.Exists(sourceMetadata))
            {
                AutomationProfile sourceProfile = DeserializeFilteredLegacyProfile(name, sourceMetadata);
                if (string.IsNullOrWhiteSpace(destinationProfile.LuaScriptText) &&
                    !string.IsNullOrWhiteSpace(sourceProfile.LuaScriptText))
                {
                    destinationProfile.LuaScriptText = sourceProfile.LuaScriptText;
                    profileChanged = true;
                }
                if (destinationProfile.Apps.Count == 0 && sourceProfile.Apps.Count > 0)
                {
                    destinationProfile.Apps = sourceProfile.Apps;
                    profileChanged = true;
                }
                foreach ((string macroName, MacroDefinition macro) in sourceProfile.Macros)
                {
                    if (destinationProfile.Macros.TryAdd(macroName, macro))
                    {
                        profileChanged = true;
                    }
                }
            }
            foreach (string sourceMacro in Directory.EnumerateFiles(sourceDirectory, "*.txt"))
            {
                string macroName = Path.GetFileNameWithoutExtension(sourceMacro);
                if (!destinationProfile.Macros.TryGetValue(macroName, out MacroDefinition? macro))
                {
                    macro = new MacroDefinition
                    {
                        Name = macroName,
                        ScriptFile = GetDefaultMacroRelativePath(macroName),
                    };
                    destinationProfile.Macros[macroName] = macro;
                    profileChanged = true;
                }
                if (string.IsNullOrEmpty(macro.Text))
                {
                    macro.Text = File.ReadAllText(sourceMacro);
                    profileChanged = true;
                }
            }
            if (profileChanged)
            {
                SaveProfile(destinationProfile);
                imported = true;
            }
        }
        File.WriteAllText(_legacyImportMarker, DateTimeOffset.Now.ToString("O"));
        return imported;
    }

    private static AutomationProfile DeserializeFilteredLegacyProfile(string name, string path)
    {
        string json = File.ReadAllText(path);
        AutomationProfile profile = JsonSerializer.Deserialize<AutomationProfile>(json, JsonOptions)
            ?? new AutomationProfile();
        try
        {
            using JsonDocument document = JsonDocument.Parse(json);
            if (document.RootElement.TryGetProperty("lua_script_text", out JsonElement luaText) &&
                luaText.ValueKind == JsonValueKind.String)
            {
                profile.LuaScriptText = luaText.GetString() ?? string.Empty;
            }
        }
        catch (JsonException)
        {
            // 保持原有反序列化错误处理边界，由调用方继续使用可读取的元数据。
        }
        profile.Name = name;
        profile.Apps ??= [];
        profile.Macros ??= new Dictionary<string, MacroDefinition>(StringComparer.OrdinalIgnoreCase);
        return profile;
    }

    private void EnsureProfile(string name)
    {
        string directory = GetProfileDirectory(name);
        Directory.CreateDirectory(directory);
        Directory.CreateDirectory(Path.Combine(directory, MacroDirectoryName));
        Directory.CreateDirectory(Path.Combine(directory, LuaDirectoryName));
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
        return ResolveAssociatedFilePath(
            profileName,
            GetDefaultMacroRelativePath(macroName),
            GetDefaultMacroRelativePath(macroName));
    }

    private string GetLegacyMacroPath(string profileName, string macroName)
    {
        string normalizedMacro = NormalizeFileName(macroName, "宏名称");
        return Path.Combine(GetProfileDirectory(profileName), normalizedMacro + ".txt");
    }

    private static string GetDefaultMacroRelativePath(string macroName)
    {
        string normalizedMacro = NormalizeFileName(macroName, "宏名称");
        return $"{MacroDirectoryName}/{normalizedMacro}.txt";
    }

    private string ResolveAssociatedFilePath(
        string profileName,
        string? associatedFile,
        string fallbackRelativePath)
    {
        string requiredDirectory = fallbackRelativePath.Split('/')[0];
        string relativePath = NormalizeAssociatedFile(associatedFile, fallbackRelativePath, requiredDirectory);
        return Path.Combine(
            GetProfileDirectory(profileName),
            relativePath.Replace('/', Path.DirectorySeparatorChar));
    }

    private static string NormalizeAssociatedFile(
        string? associatedFile,
        string fallbackRelativePath,
        string requiredDirectory)
    {
        string normalized = (associatedFile ?? string.Empty).Trim().Replace('\\', '/');
        string prefix = requiredDirectory.Trim().Trim('/') + "/";
        bool valid = !string.IsNullOrWhiteSpace(normalized) &&
            !Path.IsPathRooted(normalized) &&
            normalized.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) &&
            normalized.EndsWith(".txt", StringComparison.OrdinalIgnoreCase) &&
            normalized.Split('/').All(part => part.Length > 0 && part is not "." and not "..") &&
            normalized.IndexOfAny(Path.GetInvalidPathChars()) < 0;
        return valid ? normalized : fallbackRelativePath;
    }

    private static bool MetadataUsesCurrentLayout(string metadataPath)
    {
        if (!File.Exists(metadataPath))
        {
            return false;
        }

        try
        {
            using JsonDocument document = JsonDocument.Parse(File.ReadAllText(metadataPath));
            JsonElement root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object ||
                !root.TryGetProperty("lua_script_file", out _) ||
                root.TryGetProperty("lua_script_text", out _))
            {
                return false;
            }
            if (!root.TryGetProperty("macros", out JsonElement macros) ||
                macros.ValueKind != JsonValueKind.Object)
            {
                return false;
            }
            foreach (JsonProperty macro in macros.EnumerateObject())
            {
                if (macro.Value.ValueKind != JsonValueKind.Object ||
                    !macro.Value.TryGetProperty("file", out _))
                {
                    return false;
                }
            }
            return true;
        }
        catch (JsonException)
        {
            return false;
        }
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

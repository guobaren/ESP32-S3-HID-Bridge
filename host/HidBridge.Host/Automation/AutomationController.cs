namespace HidBridge.Host.Automation;

internal sealed class AutomationController : IDisposable
{
    private sealed class Binding : IDisposable
    {
        internal required HotkeyDefinition Hotkey { get; init; }
        internal required MacroJob Job { get; init; }
        internal bool Satisfied { get; set; }

        public void Dispose() => Job.Dispose();
    }

    private readonly object _stateLock = new();
    private readonly AutomationProfileStore _store;
    private readonly Input.InputForwarder _input;
    private readonly RoutedAutomationOutput _output;
    private readonly List<Binding> _bindings = [];
    private readonly LuaScriptRunner _lua;
    private AutomationProfile _activeProfile;
    private bool _started;
    private bool _disposed;

    internal AutomationController(
        AutomationProfileStore store,
        Input.InputForwarder input,
        IAutomationOutput? localOutput = null)
    {
        _store = store;
        _input = input;
        _output = new RoutedAutomationOutput(input, localOutput);
        Settings = store.LoadSettings();
        string activeProfile = store.ListProfiles().Contains(Settings.ActiveProfile, StringComparer.OrdinalIgnoreCase)
            ? Settings.ActiveProfile
            : AutomationProfileStore.GlobalProfile;
        _activeProfile = store.LoadProfile(activeProfile);
        _lua = new LuaScriptRunner(
            _output,
            PublishLuaLog,
            PublishLuaDiagnostic,
            () => LuaLogCleared?.Invoke());
    }

    internal AutomationSettings Settings { get; }
    internal AutomationProfile ActiveProfile => _activeProfile;
    internal bool LuaActive => _lua.Active;
    internal long LocalReleaseAllCount => _output.LocalReleaseAllCount;

    internal event Action<string>? Log;
    internal event Action<string>? MacroLog;
    internal event Action<string>? LuaLog;
    internal event Action<string>? DiagnosticLog;
    internal event Action? LuaLogCleared;
    internal event Action<string>? ActiveProfileChanged;
    internal event Action<bool>? LuaStateChanged;

    internal void Start()
    {
        if (_started)
        {
            return;
        }
        _input.PhysicalInputChanged += HandlePhysicalInput;
        _input.ForwardingTransitioning += HandleForwardingTransitioning;
        _input.ForwardingChanged += HandleForwardingChanged;
        _started = true;
        ReloadActiveProfileRuntime();
    }

    internal IReadOnlyList<string> ListProfiles() => _store.ListProfiles();

    internal AutomationProfile LoadProfile(string name) => _store.LoadProfile(name);

    internal void SaveProfile(AutomationProfile profile)
    {
        _store.SaveProfile(profile);
        if (profile.Name.Equals(_activeProfile.Name, StringComparison.OrdinalIgnoreCase))
        {
            SetActiveProfile(profile.Name, forceReload: true);
        }
    }

    internal AutomationProfile CreateProfile(string name) => _store.CreateProfile(name);

    internal void DeleteProfile(string name)
    {
        bool active = name.Equals(_activeProfile.Name, StringComparison.OrdinalIgnoreCase);
        _store.DeleteProfile(name);
        if (active)
        {
            SetActiveProfile(AutomationProfileStore.GlobalProfile);
        }
    }

    internal void DeleteMacro(string profileName, string macroName, string? associatedFile = null) =>
        _store.DeleteMacro(profileName, macroName, associatedFile);

    internal void SetActiveProfile(string name, bool forceReload = false)
    {
        if (!forceReload && name.Equals(_activeProfile.Name, StringComparison.OrdinalIgnoreCase))
        {
            return;
        }
        StopRuntime();
        _activeProfile = _store.LoadProfile(name);
        Settings.ActiveProfile = _activeProfile.Name;
        _store.SaveSettings(Settings);
        ReloadActiveProfileRuntime();
        ActiveProfileChanged?.Invoke(_activeProfile.Name);
        PublishMacroLog($"已手动切换配置：{_activeProfile.Name}");
    }

    internal IReadOnlyList<MacroParseError> CheckMacro(MacroDefinition macro) =>
        MacroParser.Parse(macro.Text, macro.Mode).Errors;

    internal (bool Success, string Message) CheckLua(string text) => _lua.Check(text);

    internal void StartLua(string text)
    {
        _lua.Start(text);
        LuaStateChanged?.Invoke(_lua.Active);
    }

    internal void StopLua()
    {
        _lua.Stop();
        LuaStateChanged?.Invoke(false);
        PublishLuaLog("Lua 脚本已停止。");
    }

    internal void SaveSettings()
    {
        _store.SaveSettings(Settings);
        StartupRegistration.SetEnabled(Settings.StartOnBoot);
    }

    private void ReloadActiveProfileRuntime()
    {
        lock (_stateLock)
        {
            foreach ((string name, MacroDefinition macro) in _activeProfile.Macros)
            {
                if (!macro.Enabled || string.IsNullOrWhiteSpace(macro.Trigger))
                {
                    continue;
                }
                try
                {
                    ParsedMacro parsed = MacroParser.Parse(macro.Text, macro.Mode);
                    if (parsed.Errors.Count > 0)
                    {
                        PublishMacroLog($"宏 {name} 存在解析错误，已跳过：{parsed.Errors[0]}");
                        continue;
                    }
                    HotkeyDefinition hotkey = HotkeyDefinition.Parse(macro.Trigger);
                    _bindings.Add(new Binding
                    {
                        Hotkey = hotkey,
                        Job = new MacroJob(name, macro.Mode, parsed, _output, PublishMacroLog),
                    });
                }
                catch (Exception exception)
                {
                    PublishMacroLog($"宏 {name} 加载失败：{exception.Message}");
                }
            }
        }
        if (!string.IsNullOrWhiteSpace(_activeProfile.LuaScriptText))
        {
            try
            {
                _lua.Start(_activeProfile.LuaScriptText);
            }
            catch (Exception exception)
            {
                PublishLuaLog($"Lua 自动启动失败：{exception.Message}");
            }
        }
        LuaStateChanged?.Invoke(_lua.Active);
    }

    private void HandlePhysicalInput(PhysicalInputEvent input)
    {
        // 按键事件只送入 Lua 和诊断日志，不再由 Host 额外生成
        // press arg=/release arg= 行；脚本中的 DebugLog 仍照常显示。
        _lua.HandlePhysicalInput(input);
        lock (_stateLock)
        {
            foreach (Binding binding in _bindings)
            {
                bool satisfied = binding.Hotkey.IsSatisfiedBy(input.HeldKeys);
                if (satisfied == binding.Satisfied)
                {
                    continue;
                }
                binding.Satisfied = satisfied;
                if (satisfied)
                {
                    bool started = binding.Job.OnPressed();
                    PublishMacroLog(started
                        ? $"触发宏：{binding.Hotkey.Display}"
                        : $"停止宏：{binding.Hotkey.Display}");
                }
                else if (binding.Job.OnReleased())
                {
                    PublishMacroLog($"停止宏：{binding.Hotkey.Display}");
                }
            }
        }
    }

    private void HandleForwardingTransitioning()
    {
        try
        {
            StopRuntime();
        }
        finally
        {
            TryReleaseLocalInputs();
        }
    }

    private void HandleForwardingChanged(object? sender, bool enabled)
    {
        ReloadActiveProfileRuntime();
        PublishMacroLog(enabled
            ? "自动化输出已切换到对端 HID。"
            : "自动化输出已切换到本机 Win32 API。");
    }

    private void StopRuntime()
    {
        _lua.Stop();
        lock (_stateLock)
        {
            foreach (Binding binding in _bindings)
            {
                binding.Dispose();
            }
            _bindings.Clear();
        }
        LuaStateChanged?.Invoke(false);
    }

    private void TryReleaseLocalInputs()
    {
        try
        {
            _output.ReleaseLocalInputs();
            DiagnosticLog?.Invoke("[LocalOutput] 已发送本机 Win32 ReleaseAll");
        }
        catch (Exception exception)
        {
            PublishMacroLog($"本机 Win32 ReleaseAll 失败：{exception.Message}");
        }
    }

    private void PublishMacroLog(string message)
    {
        Log?.Invoke(message);
        MacroLog?.Invoke(message);
    }

    private void PublishLuaLog(string message)
    {
        Log?.Invoke(message);
        LuaLog?.Invoke(message);
    }

    private void PublishLuaDiagnostic(string message)
    {
        DiagnosticLog?.Invoke(message);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }
        _disposed = true;
        if (_started)
        {
            _input.PhysicalInputChanged -= HandlePhysicalInput;
            _input.ForwardingTransitioning -= HandleForwardingTransitioning;
            _input.ForwardingChanged -= HandleForwardingChanged;
        }
        StopRuntime();
        TryReleaseLocalInputs();
        _lua.Dispose();
    }
}

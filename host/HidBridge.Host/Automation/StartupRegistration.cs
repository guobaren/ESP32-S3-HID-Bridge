using Microsoft.Win32;

namespace HidBridge.Host.Automation;

internal static class StartupRegistration
{
    private const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "HidBridge.Host";

    internal static bool IsEnabled()
    {
        using RegistryKey? key = Registry.CurrentUser.OpenSubKey(RunKeyPath, false);
        return key?.GetValue(ValueName) is string value && !string.IsNullOrWhiteSpace(value);
    }

    internal static void SetEnabled(bool enabled)
    {
        using RegistryKey key = Registry.CurrentUser.CreateSubKey(RunKeyPath, true);
        if (!enabled)
        {
            key.DeleteValue(ValueName, false);
            return;
        }
        string executable = Environment.ProcessPath ?? Application.ExecutablePath;
        key.SetValue(ValueName, $"\"{executable}\"");
    }
}

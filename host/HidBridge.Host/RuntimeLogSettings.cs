namespace HidBridge.Host;

internal enum RuntimeLogMode
{
    Reduced,
    Full,
}

internal sealed class RuntimeLogSettings
{
    private int _mode;

    internal RuntimeLogSettings(RuntimeLogMode initialMode)
    {
        _mode = (int)initialMode;
    }

    internal RuntimeLogMode Mode => (RuntimeLogMode)Volatile.Read(ref _mode);

    internal bool FullLoggingEnabled => Mode == RuntimeLogMode.Full;

    internal bool SetMode(RuntimeLogMode mode) =>
        Interlocked.Exchange(ref _mode, (int)mode) != (int)mode;
}

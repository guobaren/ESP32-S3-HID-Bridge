using HidBridge.Host.Input;

namespace HidBridge.Host.Automation;

internal interface IAutomationOutput
{
    bool IsRemote { get; }
    Point GetCursorPosition();
    void MoveRelative(int deltaX, int deltaY);
    void MoveAbsolute(int x, int y);
    void SetMouseButton(int button, bool pressed);
    void Wheel(int delta);
    void KeyDown(byte hidUsage);
    void KeyUp(byte hidUsage);
    void ReleaseAll();
}

internal sealed class RoutedAutomationOutput : IAutomationOutput
{
    private readonly object _stateLock = new();
    private readonly InputForwarder _input;
    private readonly IAutomationOutput _local;
    private readonly HashSet<int> _localMouseButtons = [];
    private readonly HashSet<int> _remoteMouseButtons = [];
    private readonly HashSet<byte> _localKeys = [];
    private readonly HashSet<byte> _remoteKeys = [];
    private long _localReleaseAllCount;

    internal RoutedAutomationOutput(InputForwarder input, IAutomationOutput? localOutput = null)
    {
        _input = input;
        _local = localOutput ?? new Win32AutomationOutput();
    }

    public bool IsRemote => _input.AutomationMouseRemoteOutputEnabled ||
        _input.AutomationKeyboardRemoteOutputEnabled;

    internal long LocalReleaseAllCount => Interlocked.Read(ref _localReleaseAllCount);

    internal void ReleaseLocalInputs()
    {
        int[] buttons;
        byte[] keys;
        lock (_stateLock)
        {
            buttons = _localMouseButtons.ToArray();
            keys = _localKeys.ToArray();
            _localMouseButtons.Clear();
            _localKeys.Clear();
        }
        foreach (int button in buttons)
        {
            _local.SetMouseButton(button, false);
        }
        foreach (byte key in keys)
        {
            _local.KeyUp(key);
        }
        _local.ReleaseAll();
        Interlocked.Increment(ref _localReleaseAllCount);
    }

    public void ReleaseAll()
    {
        int[] remoteButtons;
        byte[] remoteKeys;
        lock (_stateLock)
        {
            remoteButtons = _remoteMouseButtons.ToArray();
            remoteKeys = _remoteKeys.ToArray();
            _remoteMouseButtons.Clear();
            _remoteKeys.Clear();
        }
        foreach (int button in remoteButtons)
        {
            _input.SetAutomationMouseButton(button, false, forceRemote: true);
        }
        foreach (byte key in remoteKeys)
        {
            _input.SetAutomationKey(key, false, forceRemote: true);
        }
        ReleaseLocalInputs();
    }

    public Point GetCursorPosition() => _local.GetCursorPosition();

    public void MoveRelative(int deltaX, int deltaY)
    {
        if (_input.AutomationMouseRemoteOutputEnabled)
        {
            _input.SendAutomationMouseMove(deltaX, deltaY);
        }
        else
        {
            _local.MoveRelative(deltaX, deltaY);
        }
    }

    public void MoveAbsolute(int x, int y)
    {
        if (_input.AutomationMouseRemoteOutputEnabled)
        {
            Point current = GetCursorPosition();
            _input.SendAutomationMouseMove(x - current.X, y - current.Y);
        }
        else
        {
            _local.MoveAbsolute(x, y);
        }
    }

    public void SetMouseButton(int button, bool pressed)
    {
        bool remote;
        lock (_stateLock)
        {
            if (_remoteMouseButtons.Contains(button))
            {
                remote = true;
            }
            else if (_localMouseButtons.Contains(button))
            {
                remote = false;
            }
            else
            {
                remote = _input.AutomationMouseRemoteOutputEnabled;
            }

            if (pressed)
            {
                (remote ? _remoteMouseButtons : _localMouseButtons).Add(button);
            }
            else
            {
                _remoteMouseButtons.Remove(button);
                _localMouseButtons.Remove(button);
            }
        }
        if (remote)
        {
            _input.SetAutomationMouseButton(button, pressed, forceRemote: true);
        }
        else
        {
            _local.SetMouseButton(button, pressed);
        }
    }

    public void Wheel(int delta)
    {
        if (_input.AutomationMouseRemoteOutputEnabled)
        {
            _input.SendAutomationWheel(delta);
        }
        else
        {
            _local.Wheel(delta);
        }
    }

    public void KeyDown(byte hidUsage)
    {
        bool remote;
        lock (_stateLock)
        {
            remote = _remoteKeys.Contains(hidUsage)
                ? true
                : _localKeys.Contains(hidUsage)
                    ? false
                    : _input.AutomationKeyboardRemoteOutputEnabled;
            (remote ? _remoteKeys : _localKeys).Add(hidUsage);
        }
        if (remote)
        {
            _input.SetAutomationKey(hidUsage, true, forceRemote: true);
        }
        else
        {
            _local.KeyDown(hidUsage);
        }
    }

    public void KeyUp(byte hidUsage)
    {
        bool remote;
        lock (_stateLock)
        {
            remote = _remoteKeys.Remove(hidUsage);
            if (!remote)
            {
                remote = !_localKeys.Remove(hidUsage) &&
                    _input.AutomationKeyboardRemoteOutputEnabled;
            }
        }
        if (remote)
        {
            _input.SetAutomationKey(hidUsage, false, forceRemote: true);
        }
        else
        {
            _local.KeyUp(hidUsage);
        }
    }
}

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
}

internal sealed class RoutedAutomationOutput : IAutomationOutput
{
    private readonly InputForwarder _input;
    private readonly Win32AutomationOutput _local = new();

    internal RoutedAutomationOutput(InputForwarder input)
    {
        _input = input;
    }

    public bool IsRemote => _input.ForwardingEnabled;

    public Point GetCursorPosition() => _local.GetCursorPosition();

    public void MoveRelative(int deltaX, int deltaY)
    {
        if (IsRemote)
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
        if (IsRemote)
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
        if (IsRemote)
        {
            _input.SetAutomationMouseButton(button, pressed);
        }
        else
        {
            _local.SetMouseButton(button, pressed);
        }
    }

    public void Wheel(int delta)
    {
        if (IsRemote)
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
        if (IsRemote)
        {
            _input.SetAutomationKey(hidUsage, true);
        }
        else
        {
            _local.KeyDown(hidUsage);
        }
    }

    public void KeyUp(byte hidUsage)
    {
        if (IsRemote)
        {
            _input.SetAutomationKey(hidUsage, false);
        }
        else
        {
            _local.KeyUp(hidUsage);
        }
    }
}

namespace HidBridge.Host.Automation;

internal sealed class MacroJob : IDisposable
{
    private readonly object _stateLock = new();
    private readonly IAutomationOutput _output;
    private readonly ParsedMacro _macro;
    private readonly string _name;
    private readonly string _mode;
    private readonly Action<string> _log;
    private CancellationTokenSource? _cancellation;
    private Task? _task;
    private bool _toggleActive;
    private bool _disposed;

    internal MacroJob(
        string name,
        string mode,
        ParsedMacro macro,
        IAutomationOutput output,
        Action<string> log)
    {
        _name = name;
        _mode = MacroRunModes.All.Contains(mode, StringComparer.Ordinal) ? mode : MacroRunModes.Once;
        _macro = macro;
        _output = output;
        _log = log;
    }

    internal bool OnPressed()
    {
        if (_disposed)
        {
            return false;
        }
        switch (_mode)
        {
            case MacroRunModes.Toggle:
                lock (_stateLock)
                {
                    if (_toggleActive)
                    {
                        _toggleActive = false;
                        CancelLocked();
                        return false;
                    }
                    _toggleActive = true;
                    StartLocked(token => RunLoopAsync(_macro.Body, token));
                }
                break;
            case MacroRunModes.HoldLoop:
                Start(token => RunLoopAsync(_macro.Body, token));
                break;
            case MacroRunModes.Staged:
                Start(RunStagedAsync);
                break;
            default:
                Start(token => RunBodyAsync(_macro.Body, token));
                break;
        }
        return true;
    }

    internal bool OnReleased()
    {
        if (_mode == MacroRunModes.HoldLoop)
        {
            Stop();
            return true;
        }
        if (_mode == MacroRunModes.Staged)
        {
            Stop(runReleaseSection: true);
            return true;
        }
        return false;
    }

    internal void Stop(bool runReleaseSection = false)
    {
        CancellationTokenSource? cancellation;
        Task? task;
        lock (_stateLock)
        {
            _toggleActive = false;
            cancellation = _cancellation;
            task = _task;
            cancellation?.Cancel();
        }
        if (task is not null && !task.IsCompleted)
        {
            try
            {
                task.Wait(TimeSpan.FromMilliseconds(750));
            }
            catch (AggregateException)
            {
            }
        }
        if (runReleaseSection && _macro.OnRelease.Count > 0)
        {
            _ = Task.Run(async () =>
            {
                using CancellationTokenSource releaseCancellation = new();
                await RunBodyAsync(_macro.OnRelease, releaseCancellation.Token).ConfigureAwait(false);
            });
        }
    }

    private void Start(Func<CancellationToken, Task> action)
    {
        lock (_stateLock)
        {
            StartLocked(action);
        }
    }

    private void StartLocked(Func<CancellationToken, Task> action)
    {
        if (_task is { IsCompleted: false })
        {
            return;
        }
        _cancellation?.Dispose();
        _cancellation = new CancellationTokenSource();
        CancellationToken token = _cancellation.Token;
        _task = Task.Run(async () =>
        {
            try
            {
                await action(token).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (token.IsCancellationRequested)
            {
            }
            catch (Exception exception)
            {
                _log($"宏 {_name} 执行失败：{exception.Message}");
            }
            finally
            {
                ReleaseTrackedInputs();
            }
        }, token);
    }

    private async Task RunLoopAsync(IReadOnlyList<MacroCommand> commands, CancellationToken token)
    {
        while (!token.IsCancellationRequested)
        {
            await RunBodyAsync(commands, token).ConfigureAwait(false);
            if (commands.Count == 0)
            {
                await Task.Delay(50, token).ConfigureAwait(false);
            }
        }
    }

    private async Task RunStagedAsync(CancellationToken token)
    {
        await RunBodyAsync(_macro.OnPress, token).ConfigureAwait(false);
        await RunLoopAsync(_macro.WhileHold, token).ConfigureAwait(false);
    }

    private readonly HashSet<int> _pressedButtons = [];
    private readonly HashSet<byte> _pressedKeys = [];

    private async Task RunBodyAsync(IReadOnlyList<MacroCommand> commands, CancellationToken token)
    {
        foreach (MacroCommand command in commands)
        {
            token.ThrowIfCancellationRequested();
            await RunCommandAsync(command, token).ConfigureAwait(false);
        }
    }

    private async Task RunCommandAsync(MacroCommand command, CancellationToken token)
    {
        int Integer(int index) => (int)command.Arguments[index];
        switch (command.Operation)
        {
            case "move":
                _output.MoveRelative(Integer(0), Integer(1));
                break;
            case "moveto":
                _output.MoveAbsolute(Integer(0), Integer(1));
                break;
            case "mouse":
                int button = Integer(0);
                bool pressed = Integer(1) != 0;
                _output.SetMouseButton(button, pressed);
                lock (_stateLock)
                {
                    if (pressed)
                    {
                        _pressedButtons.Add(button);
                    }
                    else
                    {
                        _pressedButtons.Remove(button);
                    }
                }
                break;
            case "wheel":
                _output.Wheel(Integer(0));
                break;
            case "keydown":
                SetKey((byte)command.Arguments[0], true);
                break;
            case "keyup":
                SetKey((byte)command.Arguments[0], false);
                break;
            case "keypress":
                byte usage = (byte)command.Arguments[0];
                int duration = command.Arguments.Length > 1 ? Integer(1) : 10;
                SetKey(usage, true);
                await Task.Delay(Math.Max(0, duration), token).ConfigureAwait(false);
                SetKey(usage, false);
                break;
            case "delay":
            case "sleep":
                await Task.Delay(Math.Max(0, Integer(0)), token).ConfigureAwait(false);
                break;
            case "randsleep":
            case "randdelay":
                int baseDelay = Integer(0);
                int variance = Math.Abs(Integer(1));
                int minimum = Math.Max(0, baseDelay - variance);
                int maximum = Math.Max(minimum, baseDelay + variance);
                int randomDelay = Random.Shared.Next(minimum, maximum + 1);
                await Task.Delay(randomDelay, token).ConfigureAwait(false);
                break;
        }
    }

    private void SetKey(byte usage, bool pressed)
    {
        if (pressed)
        {
            _output.KeyDown(usage);
            lock (_stateLock)
            {
                _pressedKeys.Add(usage);
            }
        }
        else
        {
            _output.KeyUp(usage);
            lock (_stateLock)
            {
                _pressedKeys.Remove(usage);
            }
        }
    }

    private void ReleaseTrackedInputs()
    {
        int[] buttons;
        byte[] keys;
        lock (_stateLock)
        {
            buttons = _pressedButtons.ToArray();
            keys = _pressedKeys.ToArray();
            _pressedButtons.Clear();
            _pressedKeys.Clear();
        }
        foreach (int button in buttons)
        {
            TryRelease(() => _output.SetMouseButton(button, false));
        }
        foreach (byte key in keys)
        {
            TryRelease(() => _output.KeyUp(key));
        }
    }

    private void TryRelease(Action release)
    {
        try
        {
            release();
        }
        catch (Exception exception)
        {
            _log($"宏 {_name} 释放输入失败：{exception.Message}");
        }
    }

    private void CancelLocked() => _cancellation?.Cancel();

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }
        _disposed = true;
        Stop();
        _cancellation?.Dispose();
    }
}

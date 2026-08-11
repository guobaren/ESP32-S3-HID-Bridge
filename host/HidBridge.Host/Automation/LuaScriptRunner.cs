using System.Collections.Concurrent;
using System.Globalization;
using System.Text.RegularExpressions;
using MoonSharp.Interpreter;

namespace HidBridge.Host.Automation;

internal sealed class LuaScriptRunner : IDisposable
{
    private readonly object _stateLock = new();
    private readonly IAutomationOutput _output;
    private readonly Action<string> _log;
    private readonly Action _clearLog;
    private readonly HashSet<uint> _heldPhysicalKeys = [];
    private readonly HashSet<int> _pressedButtons = [];
    private readonly HashSet<byte> _pressedKeys = [];
    private BlockingCollection<(string Event, object Argument)>? _events;
    private CancellationTokenSource? _cancellation;
    private Task? _worker;
    private Script? _script;
    private bool _disposed;

    internal LuaScriptRunner(IAutomationOutput output, Action<string> log, Action clearLog)
    {
        _output = output;
        _log = log;
        _clearLog = clearLog;
    }

    internal bool Active
    {
        get
        {
            lock (_stateLock)
            {
                return _worker is { IsCompleted: false };
            }
        }
    }

    internal (bool Success, string Message) Check(string text)
    {
        try
        {
            Script script = CreateScript(deviceEnabled: false, CancellationToken.None);
            script.DoString(text ?? string.Empty);
            return (true, "语法正确");
        }
        catch (Exception exception)
        {
            return (false, exception.Message);
        }
    }

    internal void Start(string text)
    {
        Stop();
        if (string.IsNullOrWhiteSpace(text))
        {
            return;
        }
        CancellationTokenSource cancellation = new();
        BlockingCollection<(string Event, object Argument)> events = new();
        Script script = CreateScript(deviceEnabled: true, cancellation.Token);
        script.DoString(text);
        lock (_stateLock)
        {
            _cancellation = cancellation;
            _events = events;
            _script = script;
            _worker = Task.Run(() => RunLoop(script, events, cancellation.Token));
        }
        _log("Lua 脚本已自动启动。");
    }

    internal void HandlePhysicalInput(PhysicalInputEvent input)
    {
        BlockingCollection<(string Event, object Argument)>? events;
        lock (_stateLock)
        {
            _heldPhysicalKeys.Clear();
            _heldPhysicalKeys.UnionWith(input.HeldKeys);
            events = _events;
        }
        if (events is null || events.IsAddingCompleted)
        {
            return;
        }
        try
        {
            events.Add((
                input.Pressed ? "pressed" : "released",
                AutomationKeyMap.GetLuaEventArgument(input.VirtualKey)));
        }
        catch (InvalidOperationException)
        {
        }
    }

    internal void Stop()
    {
        CancellationTokenSource? cancellation;
        BlockingCollection<(string Event, object Argument)>? events;
        Task? worker;
        lock (_stateLock)
        {
            cancellation = _cancellation;
            events = _events;
            worker = _worker;
            _cancellation = null;
            _events = null;
            _worker = null;
            _script = null;
        }
        cancellation?.Cancel();
        events?.CompleteAdding();
        if (worker is not null && !worker.IsCompleted)
        {
            try
            {
                worker.Wait(TimeSpan.FromSeconds(1));
            }
            catch (AggregateException)
            {
            }
        }
        ReleaseTrackedInputs();
        events?.Dispose();
        cancellation?.Dispose();
    }

    private void RunLoop(
        Script script,
        BlockingCollection<(string Event, object Argument)> events,
        CancellationToken token)
    {
        try
        {
            foreach ((string eventName, object argument) in events.GetConsumingEnumerable(token))
            {
                DynValue onEvent = script.Globals.Get("OnEvent");
                if (onEvent.Type is DataType.Function or DataType.ClrFunction)
                {
                    script.Call(onEvent, eventName, DynValue.FromObject(script, argument));
                }
            }
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested)
        {
        }
        catch (ScriptRuntimeException exception) when (token.IsCancellationRequested)
        {
            _log($"Lua 已停止：{exception.DecoratedMessage}");
        }
        catch (Exception exception)
        {
            _log($"Lua 执行错误：{exception.Message}");
        }
        finally
        {
            ReleaseTrackedInputs();
        }
    }

    private Script CreateScript(bool deviceEnabled, CancellationToken token)
    {
        Script script = new(CoreModules.Preset_Complete);
        script.Options.DebugPrint = text => _log(text);
        script.Globals["move"] = deviceEnabled
            ? (Action<double, double>)((x, y) => _output.MoveRelative((int)x, (int)y))
            : (_, _) => { };
        script.Globals["moveto"] = deviceEnabled
            ? (Action<double, double>)((x, y) => _output.MoveAbsolute((int)x, (int)y))
            : (_, _) => { };
        script.Globals["mouse"] = deviceEnabled
            ? (Action<double, double>)((button, state) => SetMouseButton((int)button, (int)state != 0))
            : (_, _) => { };
        script.Globals["wheel"] = deviceEnabled
            ? (Action<double>)(delta => _output.Wheel((int)delta))
            : _ => { };
        script.Globals["keydown"] = deviceEnabled
            ? (Action<DynValue>)(value => SetKey(ResolveLuaHid(value), true))
            : _ => { };
        script.Globals["keyup"] = deviceEnabled
            ? (Action<DynValue>)(value => SetKey(ResolveLuaHid(value), false))
            : _ => { };
        script.Globals["keypress"] = new CallbackFunction((_, arguments) =>
        {
            if (!deviceEnabled)
            {
                return DynValue.Nil;
            }
            if (arguments.Count == 0)
            {
                throw new ScriptRuntimeException("keypress 至少需要一个按键参数。");
            }
            byte usage = ResolveLuaHid(arguments[0]);
            SetKey(usage, true);
            InterruptibleSleep(arguments.Count > 1 ? (int)arguments[1].Number : 10, token);
            SetKey(usage, false);
            return DynValue.Nil;
        });
        Action<double> sleep = milliseconds => InterruptibleSleep((int)milliseconds, token);
        script.Globals["sleep"] = sleep;
        script.Globals["Sleep"] = sleep;
        script.Globals["randdelay"] = new CallbackFunction((_, arguments) =>
        {
            if (arguments.Count == 0)
            {
                throw new ScriptRuntimeException("randdelay 至少需要一个延时参数。");
            }
            int variance = arguments.Count > 1 ? Math.Abs((int)arguments[1].Number) : 0;
            int baseline = (int)arguments[0].Number;
            InterruptibleSleep(Random.Shared.Next(
                Math.Max(0, baseline - variance),
                Math.Max(0, baseline + variance) + 1), token);
            return DynValue.Nil;
        });
        script.Globals["randsleep"] = script.Globals["randdelay"];
        script.Globals["IsPressed"] = (Func<DynValue, bool>)(value => IsPressed(value));
        script.Globals["DebugLog"] = new CallbackFunction((_, arguments) =>
        {
            _log(FormatLuaLog(arguments));
            return DynValue.Nil;
        });
        script.Globals["ClearLog"] = (Action)(() => _clearLog());
        Table virtualKeys = new(script);
        foreach ((string name, uint virtualKey) in AutomationKeyMap.LuaVirtualKeys)
        {
            virtualKeys[name] = virtualKey;
        }
        script.Globals["VK_CODES"] = virtualKeys;
        return script;
    }

    private bool IsPressed(DynValue value)
    {
        object? argument = value.Type switch
        {
            DataType.String => value.String,
            DataType.Number => (int)value.Number,
            _ => null,
        };
        if (!AutomationKeyMap.TryResolveScriptVirtualKey(argument, out uint virtualKey))
        {
            return false;
        }
        lock (_stateLock)
        {
            return _heldPhysicalKeys.Contains(virtualKey);
        }
    }

    private static byte ResolveLuaHid(DynValue value) => value.Type switch
    {
        DataType.String => AutomationKeyMap.ResolveHid(value.String),
        DataType.Number => checked((byte)value.Number),
        _ => throw new ScriptRuntimeException("按键参数必须是键名或 HID 数值。"),
    };

    private void SetMouseButton(int button, bool pressed)
    {
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
    }

    private void SetKey(byte usage, bool pressed)
    {
        if (pressed)
        {
            _output.KeyDown(usage);
        }
        else
        {
            _output.KeyUp(usage);
        }
        lock (_stateLock)
        {
            if (pressed)
            {
                _pressedKeys.Add(usage);
            }
            else
            {
                _pressedKeys.Remove(usage);
            }
        }
    }

    private static void InterruptibleSleep(int milliseconds, CancellationToken token)
    {
        int remaining = Math.Max(0, milliseconds);
        while (remaining > 0)
        {
            token.ThrowIfCancellationRequested();
            int slice = Math.Min(remaining, 10);
            Thread.Sleep(slice);
            remaining -= slice;
        }
    }

    private static string FormatLuaLog(CallbackArguments arguments)
    {
        if (arguments.Count == 0)
        {
            return string.Empty;
        }
        string format = arguments[0].CastToString() ?? arguments[0].ToPrintString();
        int argumentIndex = 1;
        string result = Regex.Replace(format, "%[diufs]", match =>
        {
            if (argumentIndex >= arguments.Count)
            {
                return match.Value;
            }
            DynValue value = arguments[argumentIndex++];
            return value.Type == DataType.Number
                ? value.Number.ToString(CultureInfo.InvariantCulture)
                : value.ToPrintString();
        });
        return result.TrimEnd('\r', '\n');
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

    private void TryRelease(Action action)
    {
        try
        {
            action();
        }
        catch (Exception exception)
        {
            _log($"Lua 释放输入失败：{exception.Message}");
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }
        _disposed = true;
        Stop();
    }
}

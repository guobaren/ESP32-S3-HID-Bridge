using System.ComponentModel;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Runtime.InteropServices;
using HidBridge.Host.Automation;
using HidBridge.Host.Transport;
using HidBridge.Protocol;

namespace HidBridge.Host.Input;

internal readonly record struct KmboxMonitorReport(
    byte MouseButtons,
    short DeltaX,
    short DeltaY,
    short Wheel,
    byte Modifiers,
    byte[] Keys);

internal sealed class InputForwarder : IDisposable
{
    private readonly record struct CapturedInputEvent(
        bool IsKeyboard,
        bool IsCoalescedMouseMovement,
        uint VirtualKey,
        bool Extended,
        bool IsDown,
        NativeMethods.RawMouse RawMouse);

    private readonly record struct CapturedKeyboardKey(uint VirtualKey, bool Extended);

    private const uint VkEnd = 0x23;
    private const uint VkHome = 0x24;
    private const byte LeftButton = 1 << 0;
    private const byte RightButton = 1 << 1;
    private const byte MiddleButton = 1 << 2;
    private const byte BackButton = 1 << 3;
    private const byte ForwardButton = 1 << 4;

    private readonly NativeMethods.HookProc _keyboardProc;
    private readonly NativeMethods.HookProc _mouseProc;
    private readonly MouseReportPump _mouseReportPump;
    private readonly object _inputStateLock = new();
    private readonly object _captureLifecycleLock = new();
    private readonly object _keyboardEdgeLock = new();
    private readonly object _rawMouseMovementLock = new();
    private readonly List<byte> _pressedKeys = [];
    private readonly HashSet<byte> _automationPressedKeys = [];
    private readonly HashSet<byte> _remotePressedKeys = [];
    private readonly HashSet<byte> _kmboxMaskedKeys = [];
    private readonly HashSet<uint> _triggerHeldKeys = [];
    private readonly HashSet<uint> _controlHotkeysDown = [];
    private readonly HashSet<CapturedKeyboardKey> _capturedKeyboardKeysDown = [];
    private IntPtr _keyboardHook;
    private IntPtr _mouseHook;
    private RawMouseInputWindow? _rawMouseInput;
    private Thread? _captureThread;
    private Thread? _inputDispatchThread;
    private BlockingCollection<CapturedInputEvent>? _inputEvents;
    private ManualResetEventSlim? _captureReady;
    private Exception? _captureStartupException;
    private uint _captureThreadId;
    private bool _captureStopRequested;
    private long _pendingRawMouseX;
    private long _pendingRawMouseY;
    private bool _rawMouseMovementQueued;
    private byte _modifiers;
    private byte _automationModifiers;
    private byte _remoteModifiers;
    private byte _mouseButtons;
    private byte _automationMouseButtons;
    private byte _remoteMouseButtons;
    private byte _kmboxMouseMask;
    private int _verticalWheelRemainder;
    private int _horizontalWheelRemainder;
    private bool _started;

    internal InputForwarder(IBridgeTransport transport)
    {
        _keyboardProc = KeyboardCallback;
        _mouseProc = MouseCallback;
        _mouseReportPump = new MouseReportPump(transport);
    }

    internal bool ForwardingEnabled => _mouseReportPump.Enabled;
    internal bool AlwaysOutputUdpEnabled => _mouseReportPump.AlwaysOutputUdpEnabled;
    internal bool SimulatedUdpInputEnabled => _mouseReportPump.SimulatedUdpInputEnabled;
    internal int SimulatedUdpInputFrequencyHz => _mouseReportPump.SimulatedUdpInputFrequencyHz;
    internal bool UdpSmoothingEnabled => _mouseReportPump.UdpSmoothingEnabled;
    internal byte KmboxMouseMaskForChecks
    {
        get
        {
            lock (_inputStateLock)
            {
                return _kmboxMouseMask;
            }
        }
    }
    internal uint CaptureThreadId => Volatile.Read(ref _captureThreadId);
    internal int PendingInputEventCountForChecks => _inputEvents?.Count ?? 0;

    internal event EventHandler<bool>? ForwardingChanged;
    internal event Action? ForwardingTransitioning;
    internal event EventHandler? ExitRequested;
    internal event Action<PhysicalInputEvent>? PhysicalInputChanged;
    internal event Action<KmboxMonitorReport>? KmboxMonitorReportAvailable;
    internal event Action? MovementRecordingStarted
    {
        add => _mouseReportPump.MovementRecordingStarted += value;
        remove => _mouseReportPump.MovementRecordingStarted -= value;
    }
    internal event Action<MouseMovementRecording>? MovementRecordingCompleted
    {
        add => _mouseReportPump.MovementRecordingCompleted += value;
        remove => _mouseReportPump.MovementRecordingCompleted -= value;
    }

    internal void DisableForwarding() => SetForwarding(false, true);

    internal void SetForwardingEnabled(bool enabled) => SetForwarding(enabled);

    internal void ConfigureSimulatedUdpInput(bool enabled, int frequencyHz) =>
        _mouseReportPump.ConfigureSimulatedUdpInput(enabled, frequencyHz);

    internal void ConfigureAlwaysOutputUdp(bool enabled) =>
        _mouseReportPump.ConfigureAlwaysOutputUdp(enabled);

    internal void ConfigureUdpSmoothing(bool enabled) =>
        _mouseReportPump.ConfigureUdpSmoothing(enabled);

    internal void ConfigureUdpSmoothingFromKmbox(bool enabled) =>
        _mouseReportPump.ConfigureUdpSmoothingFromRemote(enabled);

    internal void ConfigureOutputSensitivity(double sensitivity) =>
        _mouseReportPump.ConfigureOutputSensitivity(sensitivity);

    internal double OutputSensitivity => _mouseReportPump.OutputSensitivity;

    internal bool IsKmboxKeyMaskedForChecks(byte usage)
    {
        lock (_inputStateLock)
        {
            return _kmboxMaskedKeys.Contains(usage);
        }
    }

    internal bool TryInjectMouseMovement(int deltaX, int deltaY, int wheel = 0, int pan = 0)
    {
        if (!ForwardingEnabled && !AlwaysOutputUdpEnabled)
        {
            return false;
        }

        _mouseReportPump.AccumulateRemote(deltaX, deltaY, wheel, pan);
        return true;
    }

    internal void SetRemoteMouseButtons(byte buttons)
    {
        bool forwarding = ForwardingEnabled;
        byte combined;
        bool changed;
        lock (_inputStateLock)
        {
            byte previous = GetCombinedMouseButtonsLocked(forwarding);
            _remoteMouseButtons = unchecked((byte)(buttons & 0x1f));
            combined = GetCombinedMouseButtonsLocked(forwarding);
            changed = previous != combined;
        }
        if (changed)
        {
            _mouseReportPump.SetRemoteButtons(combined);
        }
    }

    internal void SetRemoteKeyboardState(byte modifiers, ReadOnlySpan<byte> keys)
    {
        byte[] report;
        lock (_inputStateLock)
        {
            _remoteModifiers = modifiers;
            _remotePressedKeys.Clear();
            foreach (byte key in keys)
            {
                if (key != 0)
                {
                    _remotePressedKeys.Add(key);
                }
            }
            report = BuildKeyboardReportLocked(ForwardingEnabled);
        }
        _mouseReportPump.SendRemoteKeyboard(report);
    }

    internal void ReleaseRemoteInput()
    {
        byte mouseButtons;
        byte[] keyboardReport;
        lock (_inputStateLock)
        {
            _remoteMouseButtons = 0;
            _remoteModifiers = 0;
            _remotePressedKeys.Clear();
            mouseButtons = GetCombinedMouseButtonsLocked(ForwardingEnabled);
            keyboardReport = BuildKeyboardReportLocked(ForwardingEnabled);
        }
        _mouseReportPump.SetRemoteButtons(mouseButtons);
        _mouseReportPump.SendRemoteKeyboard(keyboardReport);
    }

    internal void ConfigureKmboxMask(
        byte mouseMask,
        byte? keyToMask = null,
        byte? keyToUnmask = null,
        bool clearAll = false)
    {
        bool forwarding = ForwardingEnabled;
        byte previousButtons;
        byte currentButtons;
        byte[]? keyboardReport = null;
        lock (_inputStateLock)
        {
            previousButtons = GetCombinedMouseButtonsLocked(forwarding);
            _kmboxMouseMask = clearAll ? (byte)0 : mouseMask;
            if (clearAll)
            {
                _kmboxMaskedKeys.Clear();
            }
            else
            {
                if (keyToMask is not null)
                {
                    _kmboxMaskedKeys.Add(keyToMask.Value);
                }
                if (keyToUnmask is not null)
                {
                    _kmboxMaskedKeys.Remove(keyToUnmask.Value);
                }
            }
            currentButtons = GetCombinedMouseButtonsLocked(forwarding);
            if (forwarding)
            {
                keyboardReport = BuildKeyboardReportLocked(true);
            }
        }

        if (previousButtons != currentButtons)
        {
            _mouseReportPump.SetRemoteButtons(currentButtons);
        }
        if (keyboardReport is not null)
        {
            _mouseReportPump.SendKeyboard(keyboardReport);
        }
    }

    internal void ProcessRawMouseInputForChecks(NativeMethods.RawMouse input) =>
        HandleRawMouseInputSafely(input);

    internal bool EnqueueKeyboardInputForChecks(uint virtualKey, bool extended, bool isDown) =>
        TryEnqueueKeyboardTransition(virtualKey, extended, isDown);

    internal bool EnqueueRawMouseInputForChecks(NativeMethods.RawMouse input) =>
        EnqueueRawMouseInputCore(input);

    internal void SendAutomationMouseMove(int deltaX, int deltaY)
    {
        if (!ForwardingEnabled)
        {
            return;
        }
        byte buttons;
        lock (_inputStateLock)
        {
            buttons = GetCombinedMouseButtonsLocked(true);
        }
        _mouseReportPump.Accumulate(buttons, false, deltaX, deltaY, 0, 0);
    }

    internal void SendAutomationWheel(int delta)
    {
        if (!ForwardingEnabled)
        {
            return;
        }
        byte buttons;
        lock (_inputStateLock)
        {
            buttons = GetCombinedMouseButtonsLocked(true);
        }
        _mouseReportPump.Accumulate(buttons, false, 0, 0, delta, 0);
    }

    internal void SetAutomationMouseButton(int button, bool pressed)
    {
        byte mask = button switch
        {
            1 => LeftButton,
            2 => MiddleButton,
            3 => RightButton,
            4 => BackButton,
            5 => ForwardButton,
            _ => throw new ArgumentOutOfRangeException(nameof(button), "鼠标按钮必须为 1-5。"),
        };
        byte combined;
        bool changed;
        lock (_inputStateLock)
        {
            byte previous = GetCombinedMouseButtonsLocked(true);
            if (pressed)
            {
                _automationMouseButtons |= mask;
            }
            else
            {
                _automationMouseButtons &= unchecked((byte)~mask);
            }
            combined = GetCombinedMouseButtonsLocked(true);
            changed = previous != combined;
        }
        if (ForwardingEnabled && changed)
        {
            _mouseReportPump.Accumulate(combined, true, 0, 0, 0, 0);
        }
    }

    internal void SetAutomationKey(byte hidUsage, bool pressed)
    {
        lock (_inputStateLock)
        {
            if (hidUsage is >= 224 and <= 231)
            {
                byte mask = unchecked((byte)(1 << (hidUsage - 224)));
                if (pressed)
                {
                    _automationModifiers |= mask;
                }
                else
                {
                    _automationModifiers &= unchecked((byte)~mask);
                }
            }
            else if (pressed)
            {
                _automationPressedKeys.Add(hidUsage);
            }
            else
            {
                _automationPressedKeys.Remove(hidUsage);
            }
            SendKeyboardReportLocked();
        }
    }

    internal void Start()
    {
        lock (_captureLifecycleLock)
        {
            if (_started)
            {
                return;
            }

            _captureStartupException = null;
            _captureStopRequested = false;
            lock (_keyboardEdgeLock)
            {
                _capturedKeyboardKeysDown.Clear();
            }
            ResetCoalescedRawMouseMovement();
            _inputEvents?.Dispose();
            _inputEvents = new BlockingCollection<CapturedInputEvent>();
            _inputDispatchThread = new Thread(InputDispatchLoop)
            {
                IsBackground = true,
                Name = "HidBridge.InputDispatch",
                Priority = ThreadPriority.AboveNormal,
            };
            _inputDispatchThread.Start();
            _captureReady?.Dispose();
            _captureReady = new ManualResetEventSlim(false);
            _captureThread = new Thread(CaptureThreadMain)
            {
                IsBackground = true,
                Name = "HidBridge.InputCapture",
                Priority = ThreadPriority.AboveNormal,
            };
            _captureThread.SetApartmentState(ApartmentState.STA);
            _captureThread.Start();
        }

        if (!_captureReady.Wait(TimeSpan.FromSeconds(5)))
        {
            Stop();
            throw new TimeoutException("输入捕获线程启动超时。");
        }

        if (_captureStartupException is not null)
        {
            Exception exception = _captureStartupException;
            Stop();
            throw new InvalidOperationException("无法启动独立输入捕获线程。", exception);
        }
    }

    internal void Stop()
    {
        Thread? captureThread;
        uint captureThreadId;
        lock (_captureLifecycleLock)
        {
            captureThread = _captureThread;
            captureThreadId = _captureThreadId;
            if (!_started && captureThread is null && _inputDispatchThread is null)
            {
                return;
            }
        }

        lock (_captureLifecycleLock)
        {
            _captureStopRequested = true;
        }
        SetForwarding(false, true);
        if (captureThreadId != 0)
        {
            NativeMethods.PostThreadMessage(
                captureThreadId,
                NativeMethods.WmQuit,
                IntPtr.Zero,
                IntPtr.Zero);
        }
        if (captureThread is not null && captureThread != Thread.CurrentThread)
        {
            if (!captureThread.Join(TimeSpan.FromSeconds(5)))
            {
                throw new TimeoutException("输入捕获线程未在 5 秒内安全退出。");
            }
        }
        StopInputDispatch();
        lock (_keyboardEdgeLock)
        {
            _capturedKeyboardKeysDown.Clear();
        }
    }

    private void CaptureThreadMain()
    {
        bool captureStarted = false;
        lock (_captureLifecycleLock)
        {
            _captureThreadId = NativeMethods.GetCurrentThreadId();
        }
        try
        {
            _rawMouseInput = new RawMouseInputWindow((_, input) => EnqueueRawMouseInput(input));
            IntPtr module = NativeMethods.GetModuleHandle(null);
            _keyboardHook = NativeMethods.SetWindowsHookEx(
                NativeMethods.WhKeyboardLl,
                _keyboardProc,
                module,
                0);
            _mouseHook = NativeMethods.SetWindowsHookEx(
                NativeMethods.WhMouseLl,
                _mouseProc,
                module,
                0);
            if (_keyboardHook == IntPtr.Zero || _mouseHook == IntPtr.Zero)
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "无法安装全局输入钩子。");
            }

            lock (_captureLifecycleLock)
            {
                _started = true;
                captureStarted = true;
            }
            _captureReady?.Set();
            Application.Run();
        }
        catch (Exception exception)
        {
            if (!captureStarted)
            {
                _captureStartupException = exception;
            }
            else
            {
                Console.Error.WriteLine($"输入捕获线程异常退出：{exception.Message}");
            }
            _captureReady?.Set();
        }
        finally
        {
            bool stopRequested;
            lock (_captureLifecycleLock)
            {
                stopRequested = _captureStopRequested;
            }
            if (captureStarted && !stopRequested)
            {
                Console.Error.WriteLine("输入捕获线程意外停止，已禁用转发并强制释放全部按键和鼠标按钮。");
                lock (_captureLifecycleLock)
                {
                    _captureStopRequested = true;
                }
                FailSafeReleaseAll();
            }
            if (_keyboardHook != IntPtr.Zero)
            {
                NativeMethods.UnhookWindowsHookEx(_keyboardHook);
                _keyboardHook = IntPtr.Zero;
            }
            if (_mouseHook != IntPtr.Zero)
            {
                NativeMethods.UnhookWindowsHookEx(_mouseHook);
                _mouseHook = IntPtr.Zero;
            }
            _rawMouseInput?.Dispose();
            _rawMouseInput = null;
            lock (_captureLifecycleLock)
            {
                _started = false;
                _captureThreadId = 0;
                _captureThread = null;
            }
            _inputEvents?.CompleteAdding();
        }
    }

    private void InputDispatchLoop()
    {
        BlockingCollection<CapturedInputEvent>? events = _inputEvents;
        if (events is null)
        {
            return;
        }
        try
        {
            foreach (CapturedInputEvent input in events.GetConsumingEnumerable())
            {
                if (Volatile.Read(ref _captureStopRequested))
                {
                    continue;
                }
                if (input.IsKeyboard)
                {
                    ProcessKeyboardInput(input.VirtualKey, input.Extended, input.IsDown);
                }
                else if (input.IsCoalescedMouseMovement)
                {
                    DrainCoalescedRawMouseMovement();
                }
                else
                {
                    HandleRawMouseInputSafely(input.RawMouse);
                }
            }
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"输入分发线程异常退出，已强制 ReleaseAll：{exception.Message}");
            lock (_captureLifecycleLock)
            {
                _captureStopRequested = true;
            }
            FailSafeReleaseAll();
        }
    }

    private void StopInputDispatch()
    {
        BlockingCollection<CapturedInputEvent>? events = _inputEvents;
        Thread? dispatchThread = _inputDispatchThread;
        events?.CompleteAdding();
        if (dispatchThread is not null && dispatchThread != Thread.CurrentThread &&
            !dispatchThread.Join(TimeSpan.FromSeconds(5)))
        {
            throw new TimeoutException("输入分发线程未在 5 秒内安全退出。");
        }
        _inputDispatchThread = null;
    }

    private IntPtr KeyboardCallback(int code, IntPtr wParam, IntPtr lParam)
    {
        try
        {
            return KeyboardCallbackCore(code, wParam, lParam);
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"键盘输入回调异常，已强制 ReleaseAll：{exception.Message}");
            FailSafeReleaseAll();
            return NativeMethods.CallNextHookEx(_keyboardHook, code, wParam, lParam);
        }
    }

    private IntPtr KeyboardCallbackCore(int code, IntPtr wParam, IntPtr lParam)
    {
        if (code != NativeMethods.HcAction)
        {
            return NativeMethods.CallNextHookEx(_keyboardHook, code, wParam, lParam);
        }

        int message = unchecked((int)wParam);
        bool isDown = message is NativeMethods.WmKeyDown or NativeMethods.WmSysKeyDown;
        bool isUp = message is NativeMethods.WmKeyUp or NativeMethods.WmSysKeyUp;
        if (!isDown && !isUp)
        {
            return NativeMethods.CallNextHookEx(_keyboardHook, code, wParam, lParam);
        }

        NativeMethods.KeyboardHookData data =
            Marshal.PtrToStructure<NativeMethods.KeyboardHookData>(lParam);
        uint virtualKey = data.VirtualKey;

        if (ShouldIgnoreKeyboardHookEvent(data.Flags, virtualKey))
        {
            return NativeMethods.CallNextHookEx(_keyboardHook, code, wParam, lParam);
        }

        bool isControlHotkey = virtualKey is VkHome or VkEnd;
        TryEnqueueKeyboardTransition(
            virtualKey,
            (data.Flags & NativeMethods.LlkhfExtended) != 0,
            isDown);
        if (ShouldSuppressKeyboard(ForwardingEnabled, isControlHotkey, isUp))
        {
            return (IntPtr)1;
        }

        return NativeMethods.CallNextHookEx(_keyboardHook, code, wParam, lParam);
    }

    private bool TryEnqueueKeyboardTransition(
        uint virtualKey,
        bool extended,
        bool isDown)
    {
        lock (_keyboardEdgeLock)
        {
            CapturedKeyboardKey key = new(virtualKey, extended);
            bool changed = isDown
                ? _capturedKeyboardKeysDown.Add(key)
                : _capturedKeyboardKeysDown.Remove(key);
            if (!changed)
            {
                // 低级键盘 Hook 会收到 Windows/外设驱动的按键自动重复。Lua/宏需要的是
                // 物理边沿，同一次按住期间只能发布一次 pressed 和一次 released。
                return false;
            }

            if (TryEnqueueInput(new CapturedInputEvent(
                    true,
                    false,
                    virtualKey,
                    extended,
                    isDown,
                    default)))
            {
                return true;
            }

            // 入队失败时回滚边沿状态，避免恢复后永久吞掉下一次真实事件。
            if (isDown)
            {
                _capturedKeyboardKeysDown.Remove(key);
            }
            else
            {
                _capturedKeyboardKeysDown.Add(key);
            }
            return false;
        }
    }

    private void ProcessKeyboardInput(uint virtualKey, bool extended, bool isDown)
    {
        KmboxMonitorReport monitorReport;
        lock (_inputStateLock)
        {
            UpdateKeyboardState(virtualKey, extended, isDown);
            monitorReport = BuildKmboxMonitorReportLocked(0, 0, 0);
        }
        KmboxMonitorReportAvailable?.Invoke(monitorReport);

        bool isToggleHotkey = isDown && virtualKey == VkHome;
        bool isExitHotkey = isDown && virtualKey == VkEnd;
        if (isToggleHotkey || isExitHotkey)
        {
            _controlHotkeysDown.Add(virtualKey);
            if (ForwardingEnabled)
            {
                SendKeyboardReport();
            }
            if (isToggleHotkey)
            {
                SetForwarding(!ForwardingEnabled);
            }
            else
            {
                SetForwarding(false, true);
                ExitRequested?.Invoke(this, EventArgs.Empty);
            }
            NotifyPhysicalInput(virtualKey, true);
            return;
        }

        if (!isDown && _controlHotkeysDown.Remove(virtualKey))
        {
            NotifyPhysicalInput(virtualKey, false);
            return;
        }
        if (ForwardingEnabled)
        {
            // 目标端安全状态优先于 Lua/宏回调；released 回调即使阻塞或失败，
            // 对端也必须先收到不再包含该实体键的键盘报告。
            SendKeyboardReport();
        }
        NotifyPhysicalInput(virtualKey, isDown);
    }

    private bool TryEnqueueInput(CapturedInputEvent input)
    {
        BlockingCollection<CapturedInputEvent>? events = _inputEvents;
        if (events is null || events.IsAddingCompleted || Volatile.Read(ref _captureStopRequested))
        {
            return false;
        }
        try
        {
            return events.TryAdd(input);
        }
        catch (InvalidOperationException)
        {
            return false;
        }
    }

    private bool TryEnqueueCoalescedRawMouseMovement(
        int deltaX,
        int deltaY)
    {
        lock (_rawMouseMovementLock)
        {
            _pendingRawMouseX += deltaX;
            _pendingRawMouseY += deltaY;
            if (_rawMouseMovementQueued)
            {
                return true;
            }

            _rawMouseMovementQueued = true;
            if (TryEnqueueInput(new CapturedInputEvent(
                    false,
                    true,
                    0,
                    false,
                    false,
                    default)))
            {
                return true;
            }

            _pendingRawMouseX -= deltaX;
            _pendingRawMouseY -= deltaY;
            _rawMouseMovementQueued = false;
            return false;
        }
    }

    private void DrainCoalescedRawMouseMovement()
    {
        long deltaX;
        long deltaY;
        lock (_rawMouseMovementLock)
        {
            deltaX = _pendingRawMouseX;
            deltaY = _pendingRawMouseY;
            _pendingRawMouseX = 0;
            _pendingRawMouseY = 0;
            _rawMouseMovementQueued = false;
        }

        while (deltaX != 0 || deltaY != 0)
        {
            int chunkX = unchecked((int)Math.Clamp(deltaX, int.MinValue, int.MaxValue));
            int chunkY = unchecked((int)Math.Clamp(deltaY, int.MinValue, int.MaxValue));
            HandleRawMouseInputSafely(new NativeMethods.RawMouse { LastX = chunkX, LastY = chunkY });
            deltaX -= chunkX;
            deltaY -= chunkY;
        }
    }

    private void ResetCoalescedRawMouseMovement()
    {
        lock (_rawMouseMovementLock)
        {
            _pendingRawMouseX = 0;
            _pendingRawMouseY = 0;
            _rawMouseMovementQueued = false;
        }
    }

    private static bool IsCoalescibleRawMouseMovement(NativeMethods.RawMouse input) =>
        input.Buttons == 0 &&
        input.RawButtons == 0 &&
        (input.Flags & NativeMethods.MouseMoveAbsolute) == 0 &&
        (input.LastX != 0 || input.LastY != 0);

    private void EnqueueRawMouseInput(NativeMethods.RawMouse input) =>
        EnqueueRawMouseInputForChecks(input);

    private bool EnqueueRawMouseInputCore(NativeMethods.RawMouse input)
    {
        if (IsCoalescibleRawMouseMovement(input))
        {
            return TryEnqueueCoalescedRawMouseMovement(input.LastX, input.LastY);
        }

        return TryEnqueueInput(new CapturedInputEvent(
                false,
                false,
                0,
                false,
                false,
                input));
    }

    private IntPtr MouseCallback(int code, IntPtr wParam, IntPtr lParam)
    {
        if (code != NativeMethods.HcAction)
        {
            return NativeMethods.CallNextHookEx(_mouseHook, code, wParam, lParam);
        }

        // 低级钩子只负责可选的本地抑制。移动、按钮和滚轮统一由
        // Raw Input 按设备事件顺序处理，避免两条输入路径之间状态错序。
        if (ShouldSuppressMouse(ForwardingEnabled))
        {
            return (IntPtr)1;
        }

        return NativeMethods.CallNextHookEx(_mouseHook, code, wParam, lParam);
    }

    private void UpdateKeyboardState(uint virtualKey, bool extended, bool isDown)
    {
        if (VirtualKeyToHid.TryGetModifier(virtualKey, out byte modifier))
        {
            if (isDown)
            {
                _modifiers |= modifier;
            }
            else
            {
                _modifiers &= unchecked((byte)~modifier);
            }

            return;
        }

        if (!VirtualKeyToHid.TryGetUsage(virtualKey, extended, out byte usage))
        {
            return;
        }

        if (isDown)
        {
            if (!_pressedKeys.Contains(usage))
            {
                _pressedKeys.Add(usage);
            }
        }
        else
        {
            _pressedKeys.Remove(usage);
        }
    }

    private void SetForwarding(bool enabled, bool forceRelease = false)
    {
        if (!forceRelease && ForwardingEnabled == enabled)
        {
            return;
        }

        if (enabled)
        {
            TryNotifyForwardingTransitioning();
        }
        ClearInputState();
        _mouseReportPump.ResetAndSendRelease(enabled);
        if (!enabled)
        {
            // 关闭时安全释放必须先于任何外部事件处理器，避免事件阻塞或异常造成卡键。
            TryNotifyForwardingTransitioning();
        }
        TryNotifyForwardingChanged(enabled);
    }

    private void FailSafeReleaseAll()
    {
        try
        {
            ClearInputState();
            _mouseReportPump.ResetAndSendRelease(false);
            TryNotifyForwardingTransitioning();
            TryNotifyForwardingChanged(false);
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"输入故障保护 ReleaseAll 发送失败：{exception.Message}");
        }
    }

    private void TryNotifyForwardingTransitioning()
    {
        try
        {
            ForwardingTransitioning?.Invoke();
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"停止自动化运行时失败，仍将继续执行 ReleaseAll：{exception.Message}");
        }
    }

    private void TryNotifyForwardingChanged(bool enabled)
    {
        try
        {
            ForwardingChanged?.Invoke(this, enabled);
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"通知转发状态变化失败：{exception.Message}");
        }
    }

    private void ClearInputState()
    {
        lock (_inputStateLock)
        {
            _pressedKeys.Clear();
            _automationPressedKeys.Clear();
            _remotePressedKeys.Clear();
            _triggerHeldKeys.Clear();
            _controlHotkeysDown.Clear();
            _modifiers = 0;
            _automationModifiers = 0;
            _remoteModifiers = 0;
            _mouseButtons = 0;
            _automationMouseButtons = 0;
            _remoteMouseButtons = 0;
            _verticalWheelRemainder = 0;
            _horizontalWheelRemainder = 0;
        }
    }

    private void HandleRawMouseInput(NativeMethods.RawMouse input)
    {
        // 自动化触发监听与 HID 转发是两条独立链路。本机模式虽然不向对端
        // 累计报告，仍必须把实体鼠标按键送给宏和 Lua 的 OnEvent/IsPressed。
        ushort flags = input.ButtonFlags;

        bool forwarding = ForwardingEnabled;
        byte combinedButtons;
        bool hasButtonChange;
        int wheel = 0;
        int pan = 0;
        byte mouseMask;
        KmboxMonitorReport monitorReport;
        lock (_inputStateLock)
        {
            byte previousButtons = GetCombinedMouseButtonsLocked(forwarding);
            UpdateMouseButton(flags, NativeMethods.RawMouseLeftButtonDown, NativeMethods.RawMouseLeftButtonUp, LeftButton);
            UpdateMouseButton(flags, NativeMethods.RawMouseRightButtonDown, NativeMethods.RawMouseRightButtonUp, RightButton);
            UpdateMouseButton(flags, NativeMethods.RawMouseMiddleButtonDown, NativeMethods.RawMouseMiddleButtonUp, MiddleButton);
            UpdateMouseButton(flags, NativeMethods.RawMouseButton4Down, NativeMethods.RawMouseButton4Up, BackButton);
            UpdateMouseButton(flags, NativeMethods.RawMouseButton5Down, NativeMethods.RawMouseButton5Up, ForwardButton);

            if ((flags & NativeMethods.RawMouseWheel) != 0)
            {
                wheel = ConsumeWheelDelta(input.ButtonData, ref _verticalWheelRemainder);
            }
            else if ((flags & NativeMethods.RawMouseHorizontalWheel) != 0)
            {
                pan = ConsumeWheelDelta(input.ButtonData, ref _horizontalWheelRemainder);
            }
            combinedButtons = GetCombinedMouseButtonsLocked(forwarding);
            hasButtonChange = previousButtons != combinedButtons;
            mouseMask = _kmboxMouseMask;
            monitorReport = BuildKmboxMonitorReportLocked(input.LastX, input.LastY, wheel);
        }

        KmboxMonitorReportAvailable?.Invoke(monitorReport);
        if (forwarding &&
            (input.LastX != 0 || input.LastY != 0 || wheel != 0 || pan != 0 || hasButtonChange))
        {
            int forwardedX = (mouseMask & (1 << 5)) == 0 ? input.LastX : 0;
            int forwardedY = (mouseMask & (1 << 6)) == 0 ? input.LastY : 0;
            int forwardedWheel = (mouseMask & (1 << 7)) == 0 ? wheel : 0;
            int forwardedPan = (mouseMask & (1 << 7)) == 0 ? pan : 0;
            _mouseReportPump.Accumulate(
                combinedButtons,
                hasButtonChange,
                forwardedX,
                forwardedY,
                forwardedWheel,
                forwardedPan);
        }
        // 与键盘相同，先把实体按钮的新状态交给 500 Hz 报告泵，再执行 Lua/宏回调。
        NotifyMouseButtonTransitions(flags);
    }

    private void HandleRawMouseInputSafely(NativeMethods.RawMouse input)
    {
        try
        {
            HandleRawMouseInput(input);
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"鼠标输入回调异常，已强制 ReleaseAll：{exception.Message}");
            FailSafeReleaseAll();
        }
    }

    private void UpdateMouseButton(ushort flags, ushort downFlag, ushort upFlag, byte button)
    {
        if ((flags & downFlag) != 0)
        {
            _mouseButtons |= button;
        }
        if ((flags & upFlag) != 0)
        {
            _mouseButtons &= unchecked((byte)~button);
        }
    }

    private static int ConsumeWheelDelta(short delta, ref int remainder)
    {
        int total = remainder + delta;
        int steps = total / 120;
        remainder = total % 120;
        return steps;
    }

    internal static bool ShouldSuppressKeyboard(
        bool forwardingEnabled,
        bool handlingControlHotkey,
        bool isKeyUp) =>
        !isKeyUp && (handlingControlHotkey || forwardingEnabled);

    internal static bool ShouldIgnoreKeyboardHookEvent(uint flags, uint virtualKey) =>
        (flags & NativeMethods.LlkhfInjected) != 0 &&
        virtualKey is not (>= 0x7C and <= 0x87); // 允许驱动模拟的 F13-F24 作为专用宏/Lua 触发键。

    internal static bool ShouldSuppressMouse(bool forwardingEnabled) => forwardingEnabled;


    private void SendKeyboardReport()
    {
        lock (_inputStateLock)
        {
            SendKeyboardReportLocked();
        }
    }

    private void SendKeyboardReportLocked()
    {
        byte[] report = BuildKeyboardReportLocked(true);
        _mouseReportPump.SendKeyboard(report);
    }

    private byte[] BuildKeyboardReportLocked(bool includeForwardedInput)
    {
        byte[] report = new byte[8];
        byte physicalModifiers = 0;
        IEnumerable<byte> physicalKeys = [];
        if (includeForwardedInput)
        {
            for (byte usage = 224; usage <= 231; usage++)
            {
                if (!_kmboxMaskedKeys.Contains(usage) && (_modifiers & (1 << (usage - 224))) != 0)
                {
                    physicalModifiers |= unchecked((byte)(1 << (usage - 224)));
                }
            }
            physicalKeys = _pressedKeys.Where(key => !_kmboxMaskedKeys.Contains(key));
        }
        report[0] = unchecked((byte)(physicalModifiers |
            (includeForwardedInput ? _automationModifiers : 0) |
            _remoteModifiers));
        IEnumerable<byte> usages = physicalKeys
            .Concat(includeForwardedInput ? _automationPressedKeys : [])
            .Concat(_remotePressedKeys)
            .Distinct();
        int index = 0;
        foreach (byte usage in usages.Take(6))
        {
            report[index++ + 2] = usage;
        }
        return report;
    }

    private byte GetCombinedMouseButtonsLocked(bool includeForwardedInput)
    {
        byte physicalAndAutomation = includeForwardedInput
            ? unchecked((byte)((_mouseButtons & ~_kmboxMouseMask) | _automationMouseButtons))
            : (byte)0;
        return unchecked((byte)(physicalAndAutomation | _remoteMouseButtons));
    }

    private KmboxMonitorReport BuildKmboxMonitorReportLocked(int deltaX, int deltaY, int wheel) => new(
        _mouseButtons,
        unchecked((short)Math.Clamp(deltaX, short.MinValue, short.MaxValue)),
        unchecked((short)Math.Clamp(deltaY, short.MinValue, short.MaxValue)),
        unchecked((short)Math.Clamp(wheel, short.MinValue, short.MaxValue)),
        _modifiers,
        _pressedKeys.Take(10).ToArray());

    private void NotifyMouseButtonTransitions(ushort flags)
    {
        NotifyMouseButton(flags, NativeMethods.RawMouseLeftButtonDown, NativeMethods.RawMouseLeftButtonUp, 0x01);
        NotifyMouseButton(flags, NativeMethods.RawMouseRightButtonDown, NativeMethods.RawMouseRightButtonUp, 0x02);
        NotifyMouseButton(flags, NativeMethods.RawMouseMiddleButtonDown, NativeMethods.RawMouseMiddleButtonUp, 0x04);
        NotifyMouseButton(flags, NativeMethods.RawMouseButton4Down, NativeMethods.RawMouseButton4Up, 0x05);
        NotifyMouseButton(flags, NativeMethods.RawMouseButton5Down, NativeMethods.RawMouseButton5Up, 0x06);
    }

    private void NotifyMouseButton(ushort flags, ushort downFlag, ushort upFlag, uint virtualKey)
    {
        if ((flags & downFlag) != 0)
        {
            NotifyPhysicalInput(virtualKey, true);
        }
        if ((flags & upFlag) != 0)
        {
            NotifyPhysicalInput(virtualKey, false);
        }
    }

    private void NotifyPhysicalInput(uint virtualKey, bool pressed)
    {
        IReadOnlySet<uint> snapshot;
        lock (_inputStateLock)
        {
            if (pressed)
            {
                _triggerHeldKeys.Add(virtualKey);
            }
            else
            {
                _triggerHeldKeys.Remove(virtualKey);
            }
            snapshot = new HashSet<uint>(_triggerHeldKeys);
        }
        PhysicalInputChanged?.Invoke(new PhysicalInputEvent(snapshot, virtualKey, pressed));
    }

    public void Dispose()
    {
        Stop();
        _mouseReportPump.Dispose();
    }
}

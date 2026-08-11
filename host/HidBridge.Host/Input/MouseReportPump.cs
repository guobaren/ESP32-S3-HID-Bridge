using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using HidBridge.Host.Transport;
using HidBridge.Protocol;

namespace HidBridge.Host.Input;

internal sealed class MouseReportPump : IDisposable
{
    private const int OutputFrequencyHz = 500;
    private const int OutputIntervalMilliseconds = 1000 / OutputFrequencyHz;
    private const uint CreateWaitableTimerHighResolution = 0x00000002;
    private const uint TimerAllAccess = 0x001F0003;
    private const uint WaitObject0 = 0;
    private const uint Infinite = 0xFFFFFFFF;
    private static readonly TimeSpan StatisticsInterval = TimeSpan.FromSeconds(5);

    private readonly IBridgeTransport _transport;
    private readonly object _stateLock = new();
    private readonly object _sendLock = new();
    private readonly MouseMovementRecorder _movementRecorder;
    private readonly UdpMouseSmoother _udpMouseSmoother = new();
    private readonly SimulatedUdpMouseInput _simulatedUdpInput = new();
    private readonly Queue<byte> _buttonStates = [];
    private readonly Thread _senderThread;
    private readonly IntPtr _waitableTimer;
    private long _pendingX;
    private long _pendingY;
    private long _pendingWheel;
    private long _pendingPan;
    private long _capturedX;
    private long _capturedY;
    private long _submittedX;
    private long _submittedY;
    private long _rawEventCount;
    private long _submittedReportCount;
    private long _buttonTransitionCount;
    private long _maxPendingX;
    private long _maxPendingY;
    private long _lastSubmittedTimestamp;
    private long _minSubmittedIntervalUs;
    private long _maxSubmittedIntervalUs;
    private byte _lastSubmittedButtons;
    private DateTime _lastStatisticsUtc = DateTime.UtcNow;
    private bool _udpSmoothingEnabled = true;
    private bool _enabled;
    private bool _disposed;

    internal event Action? MovementRecordingStarted;
    internal event Action<MouseMovementRecording>? MovementRecordingCompleted;

    internal MouseReportPump(
        IBridgeTransport transport,
        MouseMovementRecorder? movementRecorder = null)
    {
        _transport = transport;
        _movementRecorder = movementRecorder ?? new MouseMovementRecorder();
        _waitableTimer = CreateWaitableTimerEx(
            IntPtr.Zero,
            null,
            CreateWaitableTimerHighResolution,
            TimerAllAccess);
        if (_waitableTimer == IntPtr.Zero)
        {
            _waitableTimer = CreateWaitableTimerEx(IntPtr.Zero, null, 0, TimerAllAccess);
        }
        if (_waitableTimer == IntPtr.Zero)
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), "无法创建鼠标高精度定时器。");
        }

        long dueTime = -OutputIntervalMilliseconds * 10_000L;
        if (!SetWaitableTimer(
                _waitableTimer,
                ref dueTime,
                OutputIntervalMilliseconds,
                IntPtr.Zero,
                IntPtr.Zero,
                false))
        {
            int error = Marshal.GetLastWin32Error();
            CloseHandle(_waitableTimer);
            throw new Win32Exception(error, "无法启动鼠标高精度定时器。");
        }

        _senderThread = new Thread(SenderLoop)
        {
            IsBackground = true,
            Name = "HidBridge.Mouse500Hz",
            Priority = ThreadPriority.AboveNormal,
        };
        _senderThread.Start();
    }

    internal bool Enabled
    {
        get
        {
            lock (_stateLock)
            {
                return _enabled;
            }
        }
    }

    internal void Accumulate(
        byte buttons,
        bool buttonsChanged,
        int deltaX,
        int deltaY,
        int wheel,
        int pan)
    {
        lock (_stateLock)
        {
            if (!_enabled || _disposed)
            {
                return;
            }

            _rawEventCount++;
            _capturedX += deltaX;
            _capturedY += deltaY;
            if (_simulatedUdpInput.Enabled)
            {
                if (_simulatedUdpInput.Accumulate(
                        deltaX,
                        deltaY,
                        wheel,
                        pan,
                        out MouseDelta immediateDelta))
                {
                    RouteUdpDeltaLocked(immediateDelta);
                }
            }
            else
            {
                _pendingX += deltaX;
                _pendingY += deltaY;
                _pendingWheel += wheel;
                _pendingPan += pan;
                _maxPendingX = Math.Max(_maxPendingX, Math.Abs(_pendingX));
                _maxPendingY = Math.Max(_maxPendingY, Math.Abs(_pendingY));
            }
            if (buttonsChanged)
            {
                _buttonStates.Enqueue(buttons);
                _buttonTransitionCount++;
            }
        }
    }

    internal void SendKeyboard(ReadOnlySpan<byte> report)
    {
        lock (_sendLock)
        {
            if (Enabled)
            {
                _transport.Send(MessageType.KeyboardReport, report);
            }
        }
    }

    internal void ResetAndSendRelease(bool enabledAfterRelease)
    {
        lock (_sendLock)
        {
            lock (_stateLock)
            {
                _enabled = false;
                LogDiscardedPendingLocked();
                ResetStateLocked();
            }

            _transport.Send(MessageType.ReleaseAll, ReadOnlySpan<byte>.Empty);
            _movementRecorder.ObserveReleaseAll(DateTime.UtcNow);

            lock (_stateLock)
            {
                _enabled = enabledAfterRelease && !_disposed;
                _lastStatisticsUtc = DateTime.UtcNow;
            }
        }
    }

    private void SenderLoop()
    {
        while (WaitForSingleObject(_waitableTimer, Infinite) == WaitObject0)
        {
            lock (_stateLock)
            {
                if (_disposed)
                {
                    return;
                }
            }
            MouseMovementRecording? completed = _movementRecorder.TryComplete(DateTime.UtcNow);
            if (completed is not null)
            {
                NotifyRecordingCompleted(completed);
            }
            SendPendingReport();
        }
    }

    internal bool SimulatedUdpInputEnabled
    {
        get
        {
            lock (_stateLock)
            {
                return _simulatedUdpInput.Enabled;
            }
        }
    }

    internal int SimulatedUdpInputFrequencyHz
    {
        get
        {
            lock (_stateLock)
            {
                return _simulatedUdpInput.FrequencyHz;
            }
        }
    }

    internal bool UdpSmoothingEnabled
    {
        get
        {
            lock (_stateLock)
            {
                return _udpSmoothingEnabled;
            }
        }
    }

    internal void ConfigureUdpSmoothing(bool enabled)
    {
        lock (_sendLock)
        {
            lock (_stateLock)
            {
                if (_disposed || _udpSmoothingEnabled == enabled)
                {
                    return;
                }

                if (_enabled)
                {
                    Console.WriteLine("同步开启时不能切换 UDP 平滑；请先按 HOME 关闭同步。");
                    return;
                }

                _udpMouseSmoother.Reset();
                _udpSmoothingEnabled = enabled;
            }
        }
    }

    internal void ConfigureSimulatedUdpInput(bool enabled, int frequencyHz)
    {
        if (!SimulatedUdpMouseInput.SupportedFrequencies.Contains(frequencyHz))
        {
            throw new ArgumentOutOfRangeException(nameof(frequencyHz));
        }

        lock (_stateLock)
        {
            if (_disposed)
            {
                return;
            }

            bool changed = _simulatedUdpInput.Enabled != enabled ||
                _simulatedUdpInput.FrequencyHz != frequencyHz;
            if (!changed)
            {
                return;
            }

            if (_simulatedUdpInput.TryDrain(out MouseDelta pending) && _enabled)
            {
                RouteUdpDeltaLocked(pending);
            }
            _simulatedUdpInput.Configure(enabled, frequencyHz, Stopwatch.GetTimestamp());
        }
    }

    internal void AccumulateRemote(
        int deltaX,
        int deltaY,
        int wheel,
        int pan)
    {
        lock (_stateLock)
        {
            if (!_enabled || _disposed)
            {
                return;
            }

            _rawEventCount++;
            _capturedX += deltaX;
            _capturedY += deltaY;
            RouteUdpDeltaLocked(new MouseDelta(deltaX, deltaY, wheel, pan));
        }
    }

    private void SendPendingReport()
    {
        string? statistics = null;
        bool recordingStarted = false;
        lock (_sendLock)
        {
            byte[]? payload = null;
            MouseReport submittedReport = default;
            lock (_stateLock)
            {
                if (_enabled && !_disposed)
                {
                    long nowTimestamp = Stopwatch.GetTimestamp();
                    if (_simulatedUdpInput.TryFlush(nowTimestamp, out MouseDelta simulatedDelta))
                    {
                        RouteUdpDeltaLocked(simulatedDelta);
                    }

                    if (_udpSmoothingEnabled &&
                        _udpMouseSmoother.TryDequeue(out MouseDelta remoteDelta))
                    {
                        _pendingX += remoteDelta.X;
                        _pendingY += remoteDelta.Y;
                        _pendingWheel += remoteDelta.Wheel;
                        _pendingPan += remoteDelta.Pan;
                        _maxPendingX = Math.Max(_maxPendingX, Math.Abs(_pendingX));
                        _maxPendingY = Math.Max(_maxPendingY, Math.Abs(_pendingY));
                    }

                    bool hasButtonTransition = _buttonStates.Count > 0;
                    byte buttons = hasButtonTransition
                        ? _buttonStates.Dequeue()
                        : _lastSubmittedButtons;
                    short x = (short)Math.Clamp(_pendingX, short.MinValue, short.MaxValue);
                    short y = (short)Math.Clamp(_pendingY, short.MinValue, short.MaxValue);
                    sbyte wheel = (sbyte)Math.Clamp(_pendingWheel, sbyte.MinValue, sbyte.MaxValue);
                    sbyte pan = (sbyte)Math.Clamp(_pendingPan, sbyte.MinValue, sbyte.MaxValue);

                    if (hasButtonTransition || x != 0 || y != 0 || wheel != 0 || pan != 0)
                    {
                        _pendingX -= x;
                        _pendingY -= y;
                        _pendingWheel -= wheel;
                        _pendingPan -= pan;
                        _lastSubmittedButtons = buttons;
                        _submittedX += x;
                        _submittedY += y;
                        _submittedReportCount++;
                        payload = MouseReportCodec.Encode(buttons, x, y, wheel, pan);
                        submittedReport = new MouseReport(buttons, x, y, wheel, pan);
                        RecordSubmittedIntervalLocked(Stopwatch.GetTimestamp());
                    }

                    if (DateTime.UtcNow - _lastStatisticsUtc >= StatisticsInterval)
                    {
                        statistics = BuildStatisticsLocked();
                        _lastStatisticsUtc = DateTime.UtcNow;
                    }
                }
            }

            if (payload is not null)
            {
                _transport.Send(MessageType.MouseReport, payload);
                _movementRecorder.ObserveReport(
                    submittedReport.Buttons,
                    submittedReport.X,
                    submittedReport.Y,
                    DateTime.UtcNow,
                    out recordingStarted);
            }
        }

        if (recordingStarted)
        {
            NotifyRecordingStarted();
        }

        if (statistics is not null)
        {
            Console.WriteLine(statistics);
        }
    }

    private void NotifyRecordingStarted()
    {
        try
        {
            MovementRecordingStarted?.Invoke();
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"通知鼠标移动记录开始失败：{exception.Message}");
        }
    }

    private void NotifyRecordingCompleted(MouseMovementRecording recording)
    {
        try
        {
            MovementRecordingCompleted?.Invoke(recording);
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"通知鼠标移动记录完成失败：{exception.Message}");
        }
    }


    private void RecordSubmittedIntervalLocked(long now)
    {
        if (_lastSubmittedTimestamp != 0)
        {
            long intervalUs = (now - _lastSubmittedTimestamp) * 1_000_000L / Stopwatch.Frequency;
            if (_minSubmittedIntervalUs == 0 || intervalUs < _minSubmittedIntervalUs)
            {
                _minSubmittedIntervalUs = intervalUs;
            }
            if (intervalUs > _maxSubmittedIntervalUs)
            {
                _maxSubmittedIntervalUs = intervalUs;
            }
        }
        _lastSubmittedTimestamp = now;
    }

    private string BuildStatisticsLocked()
    {
        UdpMouseSmootherStatistics udp = _udpMouseSmoother.GetStatistics();
        SimulatedUdpInputStatistics simulated = _simulatedUdpInput.GetStatistics();
        return $"鼠标统计（500 Hz）：原始事件={_rawEventCount}，采集位移=({_capturedX},{_capturedY})，" +
        $"已提交报告={_submittedReportCount}，已提交位移=({_submittedX},{_submittedY})，" +
        $"待发送=({_pendingX},{_pendingY})，按钮转换={_buttonTransitionCount}，" +
        $"按钮待发送={_buttonStates.Count}，最大积压=({_maxPendingX},{_maxPendingY})，" +
        $"提交间隔us=({_minSubmittedIntervalUs}..{_maxSubmittedIntervalUs})，" +
        $"UDP平滑={(_udpSmoothingEnabled ? "开启" : "关闭")}，" +
        $"UDP平滑窗={udp.SmoothingSlots}槽/{UdpMouseSmoother.MaximumScheduledDelayMilliseconds}ms，" +
        $"平滑待发送槽={udp.PendingSlots}，接收命令={udp.EnqueuedCommands}，" +
        $"重叠分摊={udp.OverlappingCommands}，" +
        $"模拟UDP={(simulated.Enabled ? FormatSimulatedUdpFrequency(simulated.FrequencyHz) : "关闭")}，" +
        $"模拟待整合=({simulated.PendingX},{simulated.PendingY},{simulated.PendingWheel},{simulated.PendingPan})，" +
        $"模拟输出桶={simulated.EmittedBuckets}";
    }

    private void RouteUdpDeltaLocked(MouseDelta delta)
    {
        if (delta.IsZero)
        {
            return;
        }

        if (_udpSmoothingEnabled)
        {
            _udpMouseSmoother.Enqueue(delta);
            return;
        }

        _pendingX += delta.X;
        _pendingY += delta.Y;
        _pendingWheel += delta.Wheel;
        _pendingPan += delta.Pan;
        _maxPendingX = Math.Max(_maxPendingX, Math.Abs(_pendingX));
        _maxPendingY = Math.Max(_maxPendingY, Math.Abs(_pendingY));
    }

    private static string FormatSimulatedUdpFrequency(int frequencyHz) =>
        frequencyHz == SimulatedUdpMouseInput.UnlimitedFrequencyHz
            ? "无上限"
            : $"{frequencyHz}Hz";

    private void LogDiscardedPendingLocked()
    {
        SimulatedUdpInputStatistics simulated = _simulatedUdpInput.GetStatistics();
        if (_pendingX != 0 || _pendingY != 0 || _pendingWheel != 0 ||
            _pendingPan != 0 || _buttonStates.Count != 0 ||
            simulated.PendingX != 0 || simulated.PendingY != 0 ||
            simulated.PendingWheel != 0 || simulated.PendingPan != 0)
        {
            Console.WriteLine(
                $"鼠标会话结束，丢弃未发送状态：位移=({_pendingX},{_pendingY})，" +
                $"滚轮=({_pendingWheel},{_pendingPan})，" +
                $"模拟UDP待整合=({simulated.PendingX},{simulated.PendingY},{simulated.PendingWheel},{simulated.PendingPan})，" +
                $"按钮转换={_buttonStates.Count}");
        }
    }

    private void ResetStateLocked()
    {
        _pendingX = 0;
        _pendingY = 0;
        _pendingWheel = 0;
        _pendingPan = 0;
        _capturedX = 0;
        _capturedY = 0;
        _submittedX = 0;
        _submittedY = 0;
        _rawEventCount = 0;
        _submittedReportCount = 0;
        _buttonTransitionCount = 0;
        _maxPendingX = 0;
        _maxPendingY = 0;
        _lastSubmittedTimestamp = 0;
        _minSubmittedIntervalUs = 0;
        _maxSubmittedIntervalUs = 0;
        _lastSubmittedButtons = 0;
        _buttonStates.Clear();
        _udpMouseSmoother.Reset();
        _simulatedUdpInput.ResetSession(Stopwatch.GetTimestamp());
    }

    public void Dispose()
    {
        lock (_stateLock)
        {
            if (_disposed)
            {
                return;
            }
            _disposed = true;
            _enabled = false;
            ResetStateLocked();
        }
        _senderThread.Join();
        CancelWaitableTimer(_waitableTimer);
        CloseHandle(_waitableTimer);
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateWaitableTimerEx(
        IntPtr timerAttributes,
        string? timerName,
        uint flags,
        uint desiredAccess);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWaitableTimer(
        IntPtr timer,
        ref long dueTime,
        int periodMilliseconds,
        IntPtr completionRoutine,
        IntPtr completionArgument,
        [MarshalAs(UnmanagedType.Bool)] bool resume);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CancelWaitableTimer(IntPtr timer);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);
}

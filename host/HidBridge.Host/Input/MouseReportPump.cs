using System.ComponentModel;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Runtime.InteropServices;
using HidBridge.Host.Transport;
using HidBridge.Protocol;

namespace HidBridge.Host.Input;

internal sealed class MouseReportPump : IDisposable
{
    private const int OutputFrequencyHz = 500;
    private const int OutputIntervalMilliseconds = 1000 / OutputFrequencyHz;
    private const int CoarseWakeIntervalMilliseconds = 1;
    private const uint CreateWaitableTimerHighResolution = 0x00000002;
    private const uint TimerAllAccess = 0x001F0003;
    private const uint WaitObject0 = 0;
    private const uint Infinite = 0xFFFFFFFF;
    private static readonly TimeSpan DefaultStatisticsInterval = TimeSpan.FromSeconds(5);

    private readonly IBridgeTransport _transport;
    private readonly object _stateLock = new();
    private readonly object _sendLock = new();
    private readonly MouseMovementRecorder _movementRecorder;
    private readonly UdpMouseSmoother _udpMouseSmoother = new();
    private readonly SimulatedUdpMouseInput _simulatedUdpInput = new();
    private readonly Queue<byte> _buttonStates = [];
    private readonly TimeSpan _statisticsInterval;
    private readonly Action<string> _statisticsSink;
    private readonly Func<bool> _legacyFirmwareCompatibility;
    private readonly BlockingCollection<MouseStatisticsSnapshot> _statisticsQueue =
        new(new ConcurrentQueue<MouseStatisticsSnapshot>());
    private readonly Thread _statisticsThread;
    private readonly Thread _senderThread;
    private readonly IntPtr _waitableTimer;
    private long _pendingX;
    private long _pendingY;
    private double _scaledPendingX;
    private double _scaledPendingY;
    private long _pendingWheel;
    private long _pendingPan;
    private long _capturedX;
    private long _capturedY;
    private long _submittedX;
    private long _submittedY;
    private long _rawEventCount;
    private long _senderTickCount;
    private long _submittedReportCount;
    private long _buttonTransitionCount;
    private long _maxPendingX;
    private long _maxPendingY;
    private long _lastSubmittedTimestamp;
    private long _minSubmittedIntervalUs;
    private long _maxSubmittedIntervalUs;
    private byte _lastSubmittedButtons;
    private readonly StatisticsActivityGate _statisticsActivityGate = new(DateTime.UtcNow);
    private bool _udpSmoothingEnabled = true;
    private double _outputSensitivity = MouseOutputSensitivity.Default;
    private bool _alwaysOutputUdp = true;
    private bool _enabled;
    private bool _disposed;

    internal event Action? MovementRecordingStarted;
    internal event Action<MouseMovementRecording>? MovementRecordingCompleted;

    internal MouseReportPump(
        IBridgeTransport transport,
        MouseMovementRecorder? movementRecorder = null,
        TimeSpan? statisticsInterval = null,
        Action<string>? statisticsSink = null,
        Func<bool>? legacyFirmwareCompatibility = null)
    {
        _transport = transport;
        _legacyFirmwareCompatibility = legacyFirmwareCompatibility ?? (() => false);
        _movementRecorder = movementRecorder ?? new MouseMovementRecorder();
        _statisticsInterval = statisticsInterval ?? DefaultStatisticsInterval;
        if (_statisticsInterval <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(statisticsInterval));
        }
        _statisticsSink = statisticsSink ?? Console.WriteLine;
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

        // 当前部分 Windows 主机即便成功创建 HIGH_RESOLUTION timer，2 ms 周期仍会
        // 被实际调度为约 2.9 ms。用 1 ms timer 只负责低开销粗唤醒，再按绝对
        // Stopwatch 截止时间等待，可避免周期漂移，同时不影响固件中的实体鼠标队列。
        long dueTime = -CoarseWakeIntervalMilliseconds * 10_000L;
        if (!SetWaitableTimer(
                _waitableTimer,
                ref dueTime,
                CoarseWakeIntervalMilliseconds,
                IntPtr.Zero,
                IntPtr.Zero,
                false))
        {
            int error = Marshal.GetLastWin32Error();
            CloseHandle(_waitableTimer);
            throw new Win32Exception(error, "无法启动鼠标高精度定时器。");
        }

        _statisticsThread = new Thread(StatisticsLoop)
        {
            IsBackground = true,
            Name = "HidBridge.MouseStatistics",
            Priority = ThreadPriority.BelowNormal,
        };
        _statisticsThread.Start();

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

    internal double OutputSensitivity
    {
        get
        {
            lock (_stateLock)
            {
                return _outputSensitivity;
            }
        }
    }

    internal bool AlwaysOutputUdpEnabled
    {
        get
        {
            lock (_stateLock)
            {
                return _alwaysOutputUdp;
            }
        }
    }

    internal void ConfigureAlwaysOutputUdp(bool enabled)
    {
        lock (_sendLock)
        {
            lock (_stateLock)
            {
                if (_disposed || _alwaysOutputUdp == enabled)
                {
                    return;
                }

                // 捕获关闭时，切换为关闭状态应丢弃尚未发出的 UDP 尾部，
                // 避免下次重新开启时把旧输入误认为新输入。
                if (!enabled && !_enabled)
                {
                    _pendingX = 0;
                    _pendingY = 0;
                    _scaledPendingX = 0;
                    _scaledPendingY = 0;
                    _pendingWheel = 0;
                    _pendingPan = 0;
                    _udpMouseSmoother.Reset();
                }

                _alwaysOutputUdp = enabled;
            }
        }
    }

    internal void ConfigureOutputSensitivity(double sensitivity)
    {
        lock (_stateLock)
        {
            if (_disposed)
            {
                return;
            }

            _outputSensitivity = MouseOutputSensitivity.Clamp(sensitivity);
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
            _statisticsActivityGate.RecordMovement(deltaX, deltaY);
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

    internal void SendRemoteKeyboard(ReadOnlySpan<byte> report)
    {
        lock (_sendLock)
        {
            if (Enabled || AlwaysOutputUdpEnabled)
            {
                _transport.Send(MessageType.KeyboardReport, report);
            }
        }
    }

    internal void SetRemoteButtons(byte buttons)
    {
        lock (_stateLock)
        {
            if ((!_enabled && !_alwaysOutputUdp) || _disposed)
            {
                return;
            }
            byte latestButtons = _buttonStates.Count == 0 ? _lastSubmittedButtons : _buttonStates.Last();
            if (latestButtons != buttons)
            {
                _buttonStates.Enqueue(buttons);
                _buttonTransitionCount++;
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
            Console.WriteLine($"已发送 ReleaseAll；释放后转发={(enabledAfterRelease ? "开启" : "关闭")}。");

            lock (_stateLock)
            {
                _enabled = enabledAfterRelease && !_disposed;
                _statisticsActivityGate.Reset(DateTime.UtcNow);
            }
        }
    }

    private void SenderLoop()
    {
        long outputIntervalTicks = Math.Max(1, Stopwatch.Frequency / OutputFrequencyHz);
        long nextOutputTimestamp = Stopwatch.GetTimestamp() + outputIntervalTicks;
        while (WaitForSingleObject(_waitableTimer, Infinite) == WaitObject0)
        {
            long nowTimestamp;
            while ((nowTimestamp = Stopwatch.GetTimestamp()) < nextOutputTimestamp)
            {
                Thread.SpinWait(20);
            }

            if (nowTimestamp - nextOutputTimestamp >= outputIntervalTicks)
            {
                nextOutputTimestamp = nowTimestamp + outputIntervalTicks;
            }
            else
            {
                nextOutputTimestamp += outputIntervalTicks;
            }

            lock (_stateLock)
            {
                if (_disposed)
                {
                    return;
                }
                _senderTickCount++;
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

    internal void ConfigureUdpSmoothingFromRemote(bool enabled)
    {
        lock (_sendLock)
        {
            lock (_stateLock)
            {
                if (_disposed || _udpSmoothingEnabled == enabled)
                {
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
            if ((!_enabled && !_alwaysOutputUdp) || _disposed)
            {
                return;
            }

            _rawEventCount++;
            _capturedX += deltaX;
            _capturedY += deltaY;
            _statisticsActivityGate.RecordMovement(deltaX, deltaY);
            RouteUdpDeltaLocked(new MouseDelta(deltaX, deltaY, wheel, pan));
        }
    }

    private void SendPendingReport()
    {
        MouseStatisticsSnapshot? statistics = null;
        bool recordingStarted = false;
        lock (_sendLock)
        {
            byte[]? payload = null;
            MouseReport submittedReport = default;
            lock (_stateLock)
            {
                if ((_enabled || _alwaysOutputUdp) && !_disposed)
                {
                    long nowTimestamp = Stopwatch.GetTimestamp();
                    if (_simulatedUdpInput.TryFlush(nowTimestamp, out MouseDelta simulatedDelta))
                    {
                        RouteUdpDeltaLocked(simulatedDelta);
                    }

                    ApplyOutputSensitivityLocked();

                    bool hasButtonTransition = _buttonStates.Count > 0;
                    byte buttons = hasButtonTransition
                        ? _buttonStates.Dequeue()
                        : _lastSubmittedButtons;
                    short x = TakeScaledMovementLocked(ref _scaledPendingX);
                    short y = TakeScaledMovementLocked(ref _scaledPendingY);
                    sbyte wheel = (sbyte)Math.Clamp(_pendingWheel, sbyte.MinValue, sbyte.MaxValue);
                    sbyte pan = (sbyte)Math.Clamp(_pendingPan, sbyte.MinValue, sbyte.MaxValue);

                    if (hasButtonTransition || x != 0 || y != 0 || wheel != 0 || pan != 0)
                    {
                        _pendingWheel -= wheel;
                        _pendingPan -= pan;
                        _lastSubmittedButtons = buttons;
                        _submittedX += x;
                        _submittedY += y;
                        _submittedReportCount++;
                        payload = _legacyFirmwareCompatibility()
                            ? MouseReportCodec.Encode(buttons, x, y, wheel, pan)
                            : MouseReportCodec.EncodeBridge(
                                buttons,
                                x,
                                y,
                                wheel,
                                pan,
                                _udpSmoothingEnabled
                                    ? MouseReportCodec.FirmwareSmoothingSlots
                                    : MouseReportCodec.FirmwareSmoothingDisabled);
                        submittedReport = new MouseReport(buttons, x, y, wheel, pan);
                        RecordSubmittedIntervalLocked(Stopwatch.GetTimestamp());
                    }

                    DateTime nowUtc = DateTime.UtcNow;
                    if (_statisticsActivityGate.TryConsume(nowUtc, _statisticsInterval))
                    {
                        statistics = CaptureStatisticsLocked();
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
            _statisticsQueue.TryAdd(statistics.Value);
        }
    }

    private void StatisticsLoop()
    {
        foreach (MouseStatisticsSnapshot statistics in _statisticsQueue.GetConsumingEnumerable())
        {
            try
            {
                _statisticsSink(FormatStatistics(statistics));
            }
            catch (Exception exception)
            {
                Debug.WriteLine($"鼠标统计日志输出失败：{exception}");
            }
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

    private MouseStatisticsSnapshot CaptureStatisticsLocked()
    {
        UdpMouseSmootherStatistics udp = _udpMouseSmoother.GetStatistics();
        SimulatedUdpInputStatistics simulated = _simulatedUdpInput.GetStatistics();
        long minSubmittedIntervalUs = _minSubmittedIntervalUs;
        long maxSubmittedIntervalUs = _maxSubmittedIntervalUs;
        (long pendingX, long pendingY) = GetPendingOutputForStatisticsLocked();
        _minSubmittedIntervalUs = 0;
        _maxSubmittedIntervalUs = 0;
        return new MouseStatisticsSnapshot(
            _rawEventCount,
            _capturedX,
            _capturedY,
            _senderTickCount,
            _submittedReportCount,
            _submittedX,
            _submittedY,
            pendingX,
            pendingY,
            _buttonTransitionCount,
            _buttonStates.Count,
            _maxPendingX,
            _maxPendingY,
            minSubmittedIntervalUs,
            maxSubmittedIntervalUs,
            _udpSmoothingEnabled,
            udp,
            simulated);
    }

    private static string FormatStatistics(MouseStatisticsSnapshot statistics) =>
        $"鼠标统计（500 Hz）：原始事件={statistics.RawEventCount}，采集位移=({statistics.CapturedX},{statistics.CapturedY})，" +
        $"定时tick={statistics.SenderTickCount}，" +
        $"已提交报告={statistics.SubmittedReportCount}，已提交位移=({statistics.SubmittedX},{statistics.SubmittedY})，" +
        $"待发送=({statistics.PendingX},{statistics.PendingY})，按钮转换={statistics.ButtonTransitionCount}，" +
        $"按钮待发送={statistics.PendingButtonTransitions}，最大积压=({statistics.MaxPendingX},{statistics.MaxPendingY})，" +
        $"提交间隔us=({statistics.MinSubmittedIntervalUs}..{statistics.MaxSubmittedIntervalUs})，" +
        $"UDP平滑={(statistics.UdpSmoothingEnabled ? "开启" : "关闭")}，" +
        $"UDP平滑窗={statistics.Udp.SmoothingSlots}槽/{UdpMouseSmoother.MaximumScheduledDelayMilliseconds}ms，" +
        $"平滑待发送槽={statistics.Udp.PendingSlots}，接收命令={statistics.Udp.EnqueuedCommands}，" +
        $"重叠分摊={statistics.Udp.OverlappingCommands}，" +
        $"模拟UDP={(statistics.Simulated.Enabled ? FormatSimulatedUdpFrequency(statistics.Simulated.FrequencyHz) : "关闭")}，" +
        $"模拟待整合=({statistics.Simulated.PendingX},{statistics.Simulated.PendingY},{statistics.Simulated.PendingWheel},{statistics.Simulated.PendingPan})，" +
        $"模拟输出桶={statistics.Simulated.EmittedBuckets}";

    private void RouteUdpDeltaLocked(MouseDelta delta)
    {
        if (delta.IsZero)
        {
            return;
        }

        if (_udpSmoothingEnabled)
        {
            _udpMouseSmoother.Record(delta);
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

    private void ApplyOutputSensitivityLocked()
    {
        if (_pendingX != 0)
        {
            _scaledPendingX += _pendingX * _outputSensitivity;
            _pendingX = 0;
        }
        if (_pendingY != 0)
        {
            _scaledPendingY += _pendingY * _outputSensitivity;
            _pendingY = 0;
        }
        _maxPendingX = Math.Max(_maxPendingX, SaturatingAbsToLong(_scaledPendingX));
        _maxPendingY = Math.Max(_maxPendingY, SaturatingAbsToLong(_scaledPendingY));
    }

    private static short TakeScaledMovementLocked(ref double pending)
    {
        double integral = pending >= 0
            ? Math.Floor(pending + 1e-9)
            : Math.Ceiling(pending - 1e-9);
        double clamped = Math.Clamp(integral, short.MinValue, short.MaxValue);
        short output = (short)clamped;
        pending -= output;
        return output;
    }

    private (long X, long Y) GetPendingOutputForStatisticsLocked()
    {
        double pendingX = _scaledPendingX + _pendingX * _outputSensitivity;
        double pendingY = _scaledPendingY + _pendingY * _outputSensitivity;
        return (SaturatingToLong(pendingX), SaturatingToLong(pendingY));
    }

    private static long SaturatingAbsToLong(double value)
    {
        double absolute = Math.Abs(value);
        return absolute >= long.MaxValue ? long.MaxValue : (long)absolute;
    }

    private static long SaturatingToLong(double value) =>
        value >= long.MaxValue
            ? long.MaxValue
            : value <= long.MinValue
                ? long.MinValue
                : (long)Math.Truncate(value);

    private void LogDiscardedPendingLocked()
    {
        SimulatedUdpInputStatistics simulated = _simulatedUdpInput.GetStatistics();
        if (_pendingX != 0 || _pendingY != 0 ||
            Math.Abs(_scaledPendingX) > 1e-9 || Math.Abs(_scaledPendingY) > 1e-9 ||
            _pendingWheel != 0 ||
            _pendingPan != 0 || _buttonStates.Count != 0 ||
            simulated.PendingX != 0 || simulated.PendingY != 0 ||
            simulated.PendingWheel != 0 || simulated.PendingPan != 0)
        {
            Console.WriteLine(
                $"鼠标会话结束，丢弃未发送状态：原始位移=({_pendingX},{_pendingY})，" +
                $"缩放后积压=({_scaledPendingX:0.###},{_scaledPendingY:0.###})，" +
                $"滚轮=({_pendingWheel},{_pendingPan})，" +
                $"模拟UDP待整合=({simulated.PendingX},{simulated.PendingY},{simulated.PendingWheel},{simulated.PendingPan})，" +
                $"按钮转换={_buttonStates.Count}");
        }
    }

    private void ResetStateLocked()
    {
        _pendingX = 0;
        _pendingY = 0;
        _scaledPendingX = 0;
        _scaledPendingY = 0;
        _pendingWheel = 0;
        _pendingPan = 0;
        _capturedX = 0;
        _capturedY = 0;
        _submittedX = 0;
        _submittedY = 0;
        _rawEventCount = 0;
        _senderTickCount = 0;
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
        _statisticsQueue.CompleteAdding();
        _statisticsThread.Join();
        _statisticsQueue.Dispose();
    }

    private readonly record struct MouseStatisticsSnapshot(
        long RawEventCount,
        long CapturedX,
        long CapturedY,
        long SenderTickCount,
        long SubmittedReportCount,
        long SubmittedX,
        long SubmittedY,
        long PendingX,
        long PendingY,
        long ButtonTransitionCount,
        int PendingButtonTransitions,
        long MaxPendingX,
        long MaxPendingY,
        long MinSubmittedIntervalUs,
        long MaxSubmittedIntervalUs,
        bool UdpSmoothingEnabled,
        UdpMouseSmootherStatistics Udp,
        SimulatedUdpInputStatistics Simulated);

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

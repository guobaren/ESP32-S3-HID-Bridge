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
    private bool _enabled;
    private bool _disposed;

    internal MouseReportPump(IBridgeTransport transport)
    {
        _transport = transport;
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
            _pendingX += deltaX;
            _pendingY += deltaY;
            _pendingWheel += wheel;
            _pendingPan += pan;
            _capturedX += deltaX;
            _capturedY += deltaY;
            _maxPendingX = Math.Max(_maxPendingX, Math.Abs(_pendingX));
            _maxPendingY = Math.Max(_maxPendingY, Math.Abs(_pendingY));
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
            SendPendingReport();
        }
    }

    private void SendPendingReport()
    {
        string? statistics = null;
        lock (_sendLock)
        {
            byte[]? payload = null;
            lock (_stateLock)
            {
                if (_enabled && !_disposed)
                {
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
            }
        }

        if (statistics is not null)
        {
            Console.WriteLine(statistics);
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

    private string BuildStatisticsLocked() =>
        $"鼠标统计（500 Hz）：原始事件={_rawEventCount}，采集位移=({_capturedX},{_capturedY})，" +
        $"已提交报告={_submittedReportCount}，已提交位移=({_submittedX},{_submittedY})，" +
        $"待发送=({_pendingX},{_pendingY})，按钮转换={_buttonTransitionCount}，" +
        $"按钮待发送={_buttonStates.Count}，最大积压=({_maxPendingX},{_maxPendingY})，" +
        $"提交间隔us=({_minSubmittedIntervalUs}..{_maxSubmittedIntervalUs})";

    private void LogDiscardedPendingLocked()
    {
        if (_pendingX != 0 || _pendingY != 0 || _pendingWheel != 0 ||
            _pendingPan != 0 || _buttonStates.Count != 0)
        {
            Console.WriteLine(
                $"鼠标会话结束，丢弃未发送状态：位移=({_pendingX},{_pendingY})，" +
                $"滚轮=({_pendingWheel},{_pendingPan})，按钮转换={_buttonStates.Count}");
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

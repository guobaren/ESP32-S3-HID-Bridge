using HidBridge.Protocol;

namespace HidBridge.Host.Input;

internal readonly record struct MouseDelta(long X, long Y, long Wheel, long Pan)
{
    internal bool IsZero => X == 0 && Y == 0 && Wheel == 0 && Pan == 0;
}

internal readonly record struct UdpMouseSmootherStatistics(
    int SmoothingSlots,
    int PendingSlots,
    long EnqueuedCommands,
    long OverlappingCommands);

internal sealed class UdpMouseSmoother
{
    // UDP 不在 Host 展开多个报告；选择的槽数由桥接报告传到固件侧执行。
    internal const int DefaultFirmwareSmoothingSlots = MouseReportCodec.FirmwareSmoothingSlots;
    internal const int MaximumScheduledDelayMilliseconds = 0;

    private long _enqueuedCommands;
    private int _smoothingSlots = DefaultFirmwareSmoothingSlots;

    internal int SmoothingSlots => _smoothingSlots;

    internal void Configure(int smoothingSlots)
    {
        if (smoothingSlots < 0 || smoothingSlots > byte.MaxValue ||
            !MouseReportCodec.IsValidFirmwareSmoothingSlots((byte)smoothingSlots))
        {
            throw new ArgumentOutOfRangeException(nameof(smoothingSlots));
        }
        _smoothingSlots = smoothingSlots;
    }

    internal void Record(MouseDelta delta)
    {
        if (delta.IsZero)
        {
            return;
        }

        _enqueuedCommands++;
    }

    internal UdpMouseSmootherStatistics GetStatistics() => new(
        _smoothingSlots,
        0,
        _enqueuedCommands,
        0);

    internal void Reset()
    {
        _enqueuedCommands = 0;
    }
}

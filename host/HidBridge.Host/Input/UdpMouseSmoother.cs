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
    // UDP 不再在 Host 侧展开为多个发送槽；这里仅保留诊断计数，
    // 实际 5 槽分摊由 ESP32 USB HID 输出任务完成。
    internal const int FirmwareSmoothingSlots = 5;
    internal const int MaximumScheduledDelayMilliseconds = 0;

    private long _enqueuedCommands;

    internal void Record(MouseDelta delta)
    {
        if (delta.IsZero)
        {
            return;
        }

        _enqueuedCommands++;
    }

    internal UdpMouseSmootherStatistics GetStatistics() => new(
        FirmwareSmoothingSlots,
        0,
        _enqueuedCommands,
        0);

    internal void Reset()
    {
        _enqueuedCommands = 0;
    }
}

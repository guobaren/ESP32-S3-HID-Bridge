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
    internal const int OutputIntervalMilliseconds = 2;
    internal const int SmoothingSlots = 10;
    internal const int MaximumScheduledDelayMilliseconds =
        OutputIntervalMilliseconds * SmoothingSlots;

    private readonly MouseDelta[] _scheduled = new MouseDelta[SmoothingSlots];
    private int _nextSlot;
    private int _distributionPhase;
    private long _enqueuedCommands;
    private long _overlappingCommands;

    internal void Enqueue(MouseDelta delta)
    {
        if (delta.IsZero)
        {
            return;
        }

        bool overlapsExistingPlan = _scheduled.Any(slot => !slot.IsZero);
        _enqueuedCommands++;
        if (overlapsExistingPlan)
        {
            _overlappingCommands++;
        }
        else
        {
            _distributionPhase = 0;
        }

        for (int offset = 0; offset < SmoothingSlots; offset++)
        {
            int slotIndex = (_nextSlot + offset) % SmoothingSlots;
            MouseDelta current = _scheduled[slotIndex];
            _scheduled[slotIndex] = new MouseDelta(
                current.X + Split(delta.X, offset, SmoothingSlots, _distributionPhase),
                current.Y + Split(delta.Y, offset, SmoothingSlots, _distributionPhase),
                current.Wheel + Split(delta.Wheel, offset, SmoothingSlots, _distributionPhase),
                current.Pan + Split(delta.Pan, offset, SmoothingSlots, _distributionPhase));
        }
        _distributionPhase = (_distributionPhase + 1) % SmoothingSlots;
    }

    internal bool TryDequeue(out MouseDelta delta)
    {
        delta = _scheduled[_nextSlot];
        _scheduled[_nextSlot] = default;
        _nextSlot = (_nextSlot + 1) % SmoothingSlots;
        return !delta.IsZero;
    }

    internal UdpMouseSmootherStatistics GetStatistics() => new(
        SmoothingSlots,
        _scheduled.Count(slot => !slot.IsZero),
        _enqueuedCommands,
        _overlappingCommands);

    internal void Reset()
    {
        Array.Clear(_scheduled);
        _nextSlot = 0;
        _distributionPhase = 0;
        _enqueuedCommands = 0;
        _overlappingCommands = 0;
    }

    private static long Split(long total, int index, int parts, int phase)
    {
        long quotient = total / parts;
        long remainder = total % parts;
        int phasedIndex = (index - phase + parts) % parts;
        long remainderPart = phasedIndex < Math.Abs(remainder) ? Math.Sign(remainder) : 0;
        return quotient + remainderPart;
    }
}

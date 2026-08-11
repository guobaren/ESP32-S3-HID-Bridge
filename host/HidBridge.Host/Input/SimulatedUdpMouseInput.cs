using System.Diagnostics;

namespace HidBridge.Host.Input;

internal readonly record struct SimulatedUdpInputStatistics(
    bool Enabled,
    int FrequencyHz,
    long PendingX,
    long PendingY,
    long PendingWheel,
    long PendingPan,
    long EmittedBuckets);

internal sealed class SimulatedUdpMouseInput
{
    internal const int UnlimitedFrequencyHz = 0;
    internal static readonly int[] SupportedFrequencies = [30, 60, 100, 140, 200, 500, UnlimitedFrequencyHz];

    private readonly long _timestampFrequency;
    private long _pendingX;
    private long _pendingY;
    private long _pendingWheel;
    private long _pendingPan;
    private long _intervalTicks;
    private long _nextFlushTimestamp;
    private long _emittedBuckets;
    private bool _enabled;
    private int _frequencyHz = 100;

    internal SimulatedUdpMouseInput(long? timestampFrequency = null)
    {
        _timestampFrequency = timestampFrequency ?? Stopwatch.Frequency;
        if (_timestampFrequency <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(timestampFrequency));
        }
        _intervalTicks = CalculateIntervalTicks(_frequencyHz);
    }

    internal bool Enabled => _enabled;
    internal int FrequencyHz => _frequencyHz;
    internal bool Unlimited => _frequencyHz == UnlimitedFrequencyHz;

    internal void Configure(bool enabled, int frequencyHz, long nowTimestamp)
    {
        if (!SupportedFrequencies.Contains(frequencyHz))
        {
            throw new ArgumentOutOfRangeException(
                nameof(frequencyHz),
                "模拟 UDP 输入频率只支持：30、60、100、140、200、500 Hz 或无上限。");
        }

        _enabled = enabled;
        _frequencyHz = frequencyHz;
        _intervalTicks = Unlimited ? 0 : CalculateIntervalTicks(frequencyHz);
        _nextFlushTimestamp = Unlimited ? 0 : checked(nowTimestamp + _intervalTicks);
    }

    internal bool Accumulate(
        int deltaX,
        int deltaY,
        int wheel,
        int pan,
        out MouseDelta immediateDelta)
    {
        immediateDelta = default;
        if (Unlimited)
        {
            immediateDelta = new MouseDelta(deltaX, deltaY, wheel, pan);
            if (immediateDelta.IsZero)
            {
                return false;
            }

            _emittedBuckets++;
            return true;
        }

        _pendingX += deltaX;
        _pendingY += deltaY;
        _pendingWheel += wheel;
        _pendingPan += pan;
        return false;
    }

    internal bool TryFlush(long nowTimestamp, out MouseDelta delta)
    {
        delta = default;
        if (!_enabled || Unlimited || nowTimestamp < _nextFlushTimestamp)
        {
            return false;
        }

        long elapsedIntervals = (nowTimestamp - _nextFlushTimestamp) / _intervalTicks + 1;
        _nextFlushTimestamp = checked(_nextFlushTimestamp + elapsedIntervals * _intervalTicks);
        if (!TryDrain(out delta))
        {
            return false;
        }

        _emittedBuckets++;
        return true;
    }

    internal bool TryDrain(out MouseDelta delta)
    {
        delta = new MouseDelta(_pendingX, _pendingY, _pendingWheel, _pendingPan);
        _pendingX = 0;
        _pendingY = 0;
        _pendingWheel = 0;
        _pendingPan = 0;
        return !delta.IsZero;
    }

    internal void ResetSession(long nowTimestamp)
    {
        _pendingX = 0;
        _pendingY = 0;
        _pendingWheel = 0;
        _pendingPan = 0;
        _emittedBuckets = 0;
        _nextFlushTimestamp = Unlimited ? 0 : checked(nowTimestamp + _intervalTicks);
    }

    internal SimulatedUdpInputStatistics GetStatistics() => new(
        _enabled,
        _frequencyHz,
        _pendingX,
        _pendingY,
        _pendingWheel,
        _pendingPan,
        _emittedBuckets);

    private long CalculateIntervalTicks(int frequencyHz) =>
        checked((_timestampFrequency + frequencyHz - 1) / frequencyHz);
}

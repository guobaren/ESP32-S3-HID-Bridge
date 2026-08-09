namespace HidBridge.Host.Input;

internal readonly record struct MouseDelta(long X, long Y, long Wheel, long Pan)
{
    internal bool IsZero => X == 0 && Y == 0 && Wheel == 0 && Pan == 0;
}

internal readonly record struct UdpMouseSmootherStatistics(
    int LastWindowPackets,
    int ReferencePacketsPerWindow,
    long PendingParts,
    long OverflowMerges);

internal sealed class UdpMouseSmoother
{
    internal const int WindowMilliseconds = 100;
    internal const int OutputSlotsPerWindow = 50;
    private const int MaximumPendingParts = OutputSlotsPerWindow * 2;

    private readonly Queue<MovementSegment> _segments = [];
    private DateTime _windowStartedUtc;
    private MovementSegment? _overflowSegment;
    private int _currentWindowPackets;
    private int _lastWindowPackets;
    private int _referencePacketsPerWindow = 1;
    private int _splitPhase;
    private long _pendingParts;
    private long _overflowMerges;
    private bool _windowInitialized;
    private bool _hasReferenceWindow;

    internal void Enqueue(MouseDelta delta, DateTime utcNow)
    {
        if (delta.IsZero)
        {
            return;
        }

        AdvanceWindow(utcNow);
        _currentWindowPackets++;

        int parts = CalculatePartsForNextCommand();
        if (_overflowSegment is not null)
        {
            _overflowSegment.Add(delta);
            _overflowMerges++;
            return;
        }

        if (_pendingParts + parts > MaximumPendingParts)
        {
            _overflowSegment = new MovementSegment(delta, 1);
            _segments.Enqueue(_overflowSegment);
            _pendingParts++;
            return;
        }

        _segments.Enqueue(new MovementSegment(delta, parts));
        _pendingParts += parts;
    }

    internal bool TryDequeue(out MouseDelta delta)
    {
        if (_segments.Count == 0)
        {
            delta = default;
            return false;
        }

        MovementSegment segment = _segments.Peek();
        delta = segment.TakeNext();
        _pendingParts--;
        if (segment.Completed)
        {
            _segments.Dequeue();
            if (ReferenceEquals(segment, _overflowSegment))
            {
                _overflowSegment = null;
            }
        }
        return true;
    }

    internal UdpMouseSmootherStatistics GetStatistics() => new(
        _lastWindowPackets,
        _referencePacketsPerWindow,
        _pendingParts,
        _overflowMerges);

    internal void Reset()
    {
        _segments.Clear();
        _windowStartedUtc = default;
        _overflowSegment = null;
        _currentWindowPackets = 0;
        _lastWindowPackets = 0;
        _referencePacketsPerWindow = 1;
        _splitPhase = 0;
        _pendingParts = 0;
        _overflowMerges = 0;
        _windowInitialized = false;
        _hasReferenceWindow = false;
    }

    private void AdvanceWindow(DateTime utcNow)
    {
        if (!_windowInitialized)
        {
            _windowStartedUtc = utcNow;
            _windowInitialized = true;
            return;
        }

        long elapsedTicks = utcNow.Ticks - _windowStartedUtc.Ticks;
        long windowTicks = TimeSpan.FromMilliseconds(WindowMilliseconds).Ticks;
        if (elapsedTicks < windowTicks)
        {
            return;
        }

        long elapsedWindows = elapsedTicks / windowTicks;
        _lastWindowPackets = elapsedWindows == 1 ? _currentWindowPackets : 0;
        _referencePacketsPerWindow = Math.Max(1, _lastWindowPackets);
        _currentWindowPackets = 0;
        _splitPhase = 0;
        _hasReferenceWindow = true;
        _windowStartedUtc = _windowStartedUtc.AddTicks(elapsedWindows * windowTicks);
    }

    private int CalculatePartsForNextCommand()
    {
        if (!_hasReferenceWindow)
        {
            return 1;
        }

        _splitPhase += OutputSlotsPerWindow;
        int parts = _splitPhase / _referencePacketsPerWindow;
        _splitPhase %= _referencePacketsPerWindow;
        return Math.Clamp(parts, 1, OutputSlotsPerWindow);
    }

    private sealed class MovementSegment
    {
        private MouseDelta _total;
        private readonly int _parts;
        private int _index;

        internal MovementSegment(MouseDelta total, int parts)
        {
            _total = total;
            _parts = parts;
        }

        internal bool Completed => _index == _parts;

        internal void Add(MouseDelta delta)
        {
            _total = new MouseDelta(
                _total.X + delta.X,
                _total.Y + delta.Y,
                _total.Wheel + delta.Wheel,
                _total.Pan + delta.Pan);
        }

        internal MouseDelta TakeNext()
        {
            int previousIndex = _index;
            _index++;
            return new MouseDelta(
                Split(_total.X, previousIndex, _index, _parts),
                Split(_total.Y, previousIndex, _index, _parts),
                Split(_total.Wheel, previousIndex, _index, _parts),
                Split(_total.Pan, previousIndex, _index, _parts));
        }

        private static long Split(long total, int previousIndex, int currentIndex, int parts) =>
            total * currentIndex / parts - total * previousIndex / parts;
    }
}

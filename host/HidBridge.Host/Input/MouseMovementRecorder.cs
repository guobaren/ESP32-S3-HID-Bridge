namespace HidBridge.Host.Input;

internal sealed record MouseMovementRecording(
    DateTime StartedUtc,
    DateTime CompletedUtc,
    int[] XValues,
    int[] YValues)
{
    internal int SampleCount => XValues.Length;
}

internal sealed class MouseMovementRecorder
{
    private const byte LeftAndRightButtons = (1 << 0) | (1 << 1);
    private readonly object _sync = new();
    private readonly TimeSpan _releaseDelay;
    private readonly List<int> _xValues = [];
    private readonly List<int> _yValues = [];
    private DateTime _startedUtc;
    private DateTime? _releasedSinceUtc;
    private bool _recording;

    internal MouseMovementRecorder(TimeSpan? releaseDelay = null)
    {
        _releaseDelay = releaseDelay ?? TimeSpan.FromSeconds(3);
        if (_releaseDelay < TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(releaseDelay));
        }
    }

    internal void ObserveReport(
        byte buttons,
        short x,
        short y,
        DateTime utcNow,
        out bool started)
    {
        lock (_sync)
        {
            started = false;
            bool bothPressed = (buttons & LeftAndRightButtons) == LeftAndRightButtons;
            if (!_recording)
            {
                if (!bothPressed)
                {
                    return;
                }

                _recording = true;
                _startedUtc = utcNow;
                _releasedSinceUtc = null;
                _xValues.Clear();
                _yValues.Clear();
                started = true;
            }

            if ((buttons & LeftAndRightButtons) == 0)
            {
                _releasedSinceUtc ??= utcNow;
            }
            else
            {
                _releasedSinceUtc = null;
            }

            if (x != 0 || y != 0)
            {
                _xValues.Add(x);
                _yValues.Add(y);
            }
        }
    }

    internal void ObserveReleaseAll(DateTime utcNow)
    {
        lock (_sync)
        {
            if (_recording)
            {
                _releasedSinceUtc ??= utcNow;
            }
        }
    }

    internal MouseMovementRecording? TryComplete(DateTime utcNow)
    {
        lock (_sync)
        {
            if (!_recording || !_releasedSinceUtc.HasValue ||
                utcNow - _releasedSinceUtc.Value < _releaseDelay)
            {
                return null;
            }

            MouseMovementRecording recording = new(
                _startedUtc,
                utcNow,
                _xValues.ToArray(),
                _yValues.ToArray());
            _recording = false;
            _releasedSinceUtc = null;
            _xValues.Clear();
            _yValues.Clear();
            return recording;
        }
    }
}

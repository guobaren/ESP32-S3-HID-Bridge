namespace HidBridge.Host.Input;

internal sealed class StatisticsActivityGate
{
    private DateTime _lastEmittedUtc;
    private bool _movementPending;

    internal StatisticsActivityGate(DateTime initialUtc)
    {
        _lastEmittedUtc = initialUtc;
    }

    internal void RecordMovement(int deltaX, int deltaY)
    {
        if (deltaX != 0 || deltaY != 0)
        {
            _movementPending = true;
        }
    }

    internal bool TryConsume(DateTime nowUtc, TimeSpan interval)
    {
        if (!_movementPending || nowUtc - _lastEmittedUtc < interval)
        {
            return false;
        }
        _lastEmittedUtc = nowUtc;
        _movementPending = false;
        return true;
    }

    internal void Reset(DateTime initialUtc)
    {
        _lastEmittedUtc = initialUtc;
        _movementPending = false;
    }
}

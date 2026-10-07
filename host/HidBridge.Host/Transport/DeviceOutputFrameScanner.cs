using HidBridge.Protocol;

namespace HidBridge.Host.Transport;

/// <summary>从连续 UART0 字节流中拆出完整协议帧，并把其余字节交给实时日志解码器。</summary>
internal sealed class DeviceOutputFrameScanner
{
    private const int HeaderLength = 7;
    private const int FrameOverhead = 9;
    private readonly List<byte> _pending = [];

    internal byte[] Feed(ReadOnlySpan<byte> input, Action<BridgeFrame> onFrame)
    {
        foreach (byte value in input)
        {
            _pending.Add(value);
        }

        List<byte> text = [];
        while (_pending.Count > 0)
        {
            int marker = FindMarker();
            if (marker < 0)
            {
                int flushCount = _pending[^1] == 0xA5 ? _pending.Count - 1 : _pending.Count;
                CopyPrefixToText(text, flushCount);
                break;
            }
            if (marker > 0)
            {
                CopyPrefixToText(text, marker);
                continue;
            }
            if (_pending.Count < HeaderLength)
            {
                break;
            }

            int payloadLength = _pending[6];
            if (_pending[2] != FrameCodec.Version || payloadLength > FrameCodec.MaximumPayloadLength)
            {
                CopyPrefixToText(text, 1);
                continue;
            }
            int frameLength = FrameOverhead + payloadLength;
            if (_pending.Count < frameLength)
            {
                break;
            }

            byte[] candidate = _pending.Take(frameLength).ToArray();
            if (!FrameCodec.TryDecode(candidate, out BridgeFrame frame))
            {
                CopyPrefixToText(text, 1);
                continue;
            }
            if (frame.Type == MessageType.StatsSnapshotResponse)
            {
                onFrame(frame);
                _pending.RemoveRange(0, frameLength);
            }
            else
            {
                CopyPrefixToText(text, frameLength);
            }
        }
        return text.ToArray();
    }

    private int FindMarker()
    {
        for (int index = 0; index + 1 < _pending.Count; index++)
        {
            if (_pending[index] == 0xA5 && _pending[index + 1] == 0x5A)
            {
                return index;
            }
        }
        return -1;
    }

    private void CopyPrefixToText(List<byte> text, int count)
    {
        if (count <= 0)
        {
            return;
        }
        text.AddRange(_pending.Take(count));
        _pending.RemoveRange(0, count);
    }
}

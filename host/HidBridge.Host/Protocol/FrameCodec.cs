using System.Buffers.Binary;

namespace HidBridge.Host.Protocol;

internal sealed class FrameCodec
{
    private const byte Version = 1;
    private ushort _sequence;

    public byte[] Encode(MessageType type, ReadOnlySpan<byte> payload)
    {
        if (payload.Length > byte.MaxValue)
        {
            throw new ArgumentOutOfRangeException(nameof(payload));
        }

        byte[] frame = new byte[9 + payload.Length];
        frame[0] = 0xA5;
        frame[1] = 0x5A;
        frame[2] = Version;
        frame[3] = (byte)type;
        BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(4, 2), _sequence++);
        frame[6] = (byte)payload.Length;
        payload.CopyTo(frame.AsSpan(7));

        ushort crc = ComputeCrc16(frame.AsSpan(2, 5 + payload.Length));
        BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(7 + payload.Length, 2), crc);
        return frame;
    }

    internal static ushort ComputeCrc16(ReadOnlySpan<byte> data)
    {
        ushort crc = 0xFFFF;
        foreach (byte value in data)
        {
            crc ^= (ushort)(value << 8);
            for (int bit = 0; bit < 8; bit++)
            {
                crc = (ushort)((crc & 0x8000) != 0
                    ? (crc << 1) ^ 0x1021
                    : crc << 1);
            }
        }

        return crc;
    }
}

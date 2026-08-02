using System.Buffers.Binary;

namespace HidBridge.Protocol;

public readonly record struct MouseReport(
    byte Buttons,
    short X,
    short Y,
    sbyte Wheel,
    sbyte Pan);

public static class MouseReportCodec
{
    public const int Length = 7;

    public static byte[] Encode(byte buttons, short x, short y, sbyte wheel, sbyte pan)
    {
        byte[] report = new byte[Length];
        report[0] = buttons;
        BinaryPrimitives.WriteInt16LittleEndian(report.AsSpan(1, 2), x);
        BinaryPrimitives.WriteInt16LittleEndian(report.AsSpan(3, 2), y);
        report[5] = unchecked((byte)wheel);
        report[6] = unchecked((byte)pan);
        return report;
    }

    public static bool TryDecode(ReadOnlySpan<byte> payload, out MouseReport report)
    {
        if (payload.Length != Length)
        {
            report = default;
            return false;
        }

        report = new MouseReport(
            payload[0],
            BinaryPrimitives.ReadInt16LittleEndian(payload.Slice(1, 2)),
            BinaryPrimitives.ReadInt16LittleEndian(payload.Slice(3, 2)),
            unchecked((sbyte)payload[5]),
            unchecked((sbyte)payload[6]));
        return true;
    }
}

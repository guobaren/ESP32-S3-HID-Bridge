using System.Buffers.Binary;

namespace HidBridge.Protocol;

public readonly record struct MouseReport(
    byte Buttons,
    short X,
    short Y,
    sbyte Wheel,
    sbyte Pan);

public readonly record struct BridgeMouseReport(
    MouseReport Report,
    byte FirmwareSmoothingSlots);

public static class MouseReportCodec
{
    public const int Length = 7;
    public const int BridgeLength = 8;
    public const byte FirmwareSmoothingDisabled = 0;
    // Default tier remains five slots for existing callers that opt in to smoothing.
    public const byte FirmwareSmoothingSlots = 5;
    public const byte FirmwareSmoothingMaxSlots = 20;
    public const byte FirmwareSmoothingSlotStep = 5;

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

    public static byte[] EncodeBridge(
        byte buttons,
        short x,
        short y,
        sbyte wheel,
        sbyte pan,
        byte firmwareSmoothingSlots)
    {
        ValidateFirmwareSmoothingSlots(firmwareSmoothingSlots);
        byte[] payload = new byte[BridgeLength];
        Encode(buttons, x, y, wheel, pan).CopyTo(payload, 0);
        payload[^1] = firmwareSmoothingSlots;
        return payload;
    }

    public static bool TryDecodeBridge(
        ReadOnlySpan<byte> payload,
        out BridgeMouseReport report)
    {
        report = default;
        if (payload.Length != BridgeLength ||
            !TryDecode(payload[..Length], out MouseReport mouseReport))
        {
            return false;
        }

        byte firmwareSmoothingSlots = payload[^1];
        if (!IsValidFirmwareSmoothingSlots(firmwareSmoothingSlots))
        {
            return false;
        }

        report = new BridgeMouseReport(mouseReport, firmwareSmoothingSlots);
        return true;
    }

    public static bool IsValidFirmwareSmoothingSlots(byte value) =>
        value == FirmwareSmoothingDisabled ||
        (value <= FirmwareSmoothingMaxSlots && value % FirmwareSmoothingSlotStep == 0);

    private static void ValidateFirmwareSmoothingSlots(byte value)
    {
        if (!IsValidFirmwareSmoothingSlots(value))
        {
            throw new ArgumentOutOfRangeException(
                nameof(value),
                value,
                "固件平滑槽数只能是 0、5、10、15 或 20。");
        }
    }
}

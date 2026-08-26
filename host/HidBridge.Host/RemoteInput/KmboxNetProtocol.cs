using System.Buffers.Binary;

namespace HidBridge.Host.RemoteInput;

internal enum KmboxNetCommandKind
{
    Connect,
    Mouse,
    Keyboard,
    Monitor,
    Mask,
    Unmask,
    Trace,
}

internal readonly record struct KmboxNetHeader(uint Mac, uint Random, uint Index, uint Command);

internal readonly record struct KmboxNetPacket(
    KmboxNetHeader Header,
    KmboxNetCommandKind Kind,
    int Buttons = 0,
    int DeltaX = 0,
    int DeltaY = 0,
    int Wheel = 0,
    byte Modifiers = 0,
    byte[]? Keys = null,
    bool Encrypted = false);

internal static class KmboxNetProtocol
{
    internal const uint ConnectCommand = 0xaf3c2828;
    internal const uint MouseMoveCommand = 0xaede7345;
    internal const uint MouseLeftCommand = 0x9823ae8d;
    internal const uint MouseMiddleCommand = 0x97a3ae8d;
    internal const uint MouseRightCommand = 0x238d8212;
    internal const uint MouseWheelCommand = 0xffeead38;
    internal const uint MouseAutoMoveCommand = 0xaede7346;
    internal const uint KeyboardAllCommand = 0x123c2c2f;
    internal const uint BezierMoveCommand = 0xa238455a;
    internal const uint MonitorCommand = 0x27388020;
    internal const uint MaskCommand = 0x23234343;
    internal const uint UnmaskCommand = 0x23344343;
    internal const uint TraceCommand = 0xbbcdddac;

    private const int HeaderLength = 16;
    private const int MousePacketLength = 72;
    private const int KeyboardPacketLength = 28;
    private const int EncryptedPacketLength = 128;

    internal static bool TryParse(
        ReadOnlySpan<byte> payload,
        uint? sessionMac,
        out KmboxNetPacket packet,
        out string error)
    {
        packet = default;
        error = string.Empty;

        byte[]? decrypted = null;
        bool encrypted = payload.Length == EncryptedPacketLength;
        if (encrypted)
        {
            if (sessionMac is null)
            {
                error = "加密包到达前尚未完成明文 init";
                return false;
            }
            decrypted = payload.ToArray();
            Decrypt(decrypted, sessionMac.Value);
            payload = decrypted;
        }

        if (payload.Length < HeaderLength)
        {
            error = $"kmboxNet 数据报不足 {HeaderLength} 字节";
            return false;
        }

        KmboxNetHeader header = new(
            BinaryPrimitives.ReadUInt32LittleEndian(payload),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[4..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[8..]),
            BinaryPrimitives.ReadUInt32LittleEndian(payload[12..]));
        if (encrypted && header.Mac != sessionMac)
        {
            error = "kmboxNet 加密包 MAC 校验失败";
            return false;
        }

        if (header.Command == ConnectCommand)
        {
            if (encrypted || payload.Length != HeaderLength)
            {
                error = "init 必须使用 16 字节明文连接包";
                return false;
            }
            packet = new KmboxNetPacket(header, KmboxNetCommandKind.Connect);
            return true;
        }

        if (IsMouseCommand(header.Command))
        {
            if ((!encrypted && payload.Length != MousePacketLength) || payload.Length < MousePacketLength)
            {
                error = "kmboxNet 鼠标包长度无效";
                return false;
            }
            packet = new KmboxNetPacket(
                header,
                KmboxNetCommandKind.Mouse,
                BinaryPrimitives.ReadInt32LittleEndian(payload[16..]),
                BinaryPrimitives.ReadInt32LittleEndian(payload[20..]),
                BinaryPrimitives.ReadInt32LittleEndian(payload[24..]),
                BinaryPrimitives.ReadInt32LittleEndian(payload[28..]),
                Encrypted: encrypted);
            return true;
        }

        if (header.Command == KeyboardAllCommand)
        {
            if ((!encrypted && payload.Length != KeyboardPacketLength) || payload.Length < KeyboardPacketLength)
            {
                error = "kmboxNet 键盘包长度无效";
                return false;
            }
            packet = new KmboxNetPacket(
                header,
                KmboxNetCommandKind.Keyboard,
                Modifiers: payload[16],
                Keys: payload.Slice(18, 10).ToArray(),
                Encrypted: encrypted);
            return true;
        }

        if (encrypted)
        {
            error = $"不支持的 kmboxNet 加密命令 0x{header.Command:x8}";
            return false;
        }

        KmboxNetCommandKind? kind = header.Command switch
        {
            MonitorCommand => KmboxNetCommandKind.Monitor,
            MaskCommand => KmboxNetCommandKind.Mask,
            UnmaskCommand => KmboxNetCommandKind.Unmask,
            TraceCommand => KmboxNetCommandKind.Trace,
            _ => null,
        };
        if (kind is null || payload.Length != HeaderLength)
        {
            error = $"不支持的 kmboxNet 命令 0x{header.Command:x8}";
            return false;
        }

        packet = new KmboxNetPacket(header, kind.Value);
        return true;
    }

    internal static byte[] EncodeAcknowledgement(KmboxNetHeader header)
    {
        byte[] response = new byte[HeaderLength];
        BinaryPrimitives.WriteUInt32LittleEndian(response, header.Mac);
        BinaryPrimitives.WriteUInt32LittleEndian(response.AsSpan(4), header.Random);
        BinaryPrimitives.WriteUInt32LittleEndian(response.AsSpan(8), header.Index);
        BinaryPrimitives.WriteUInt32LittleEndian(response.AsSpan(12), header.Command);
        return response;
    }

    private static bool IsMouseCommand(uint command) => command is
        MouseMoveCommand or MouseLeftCommand or MouseMiddleCommand or MouseRightCommand or
        MouseWheelCommand or MouseAutoMoveCommand or BezierMoveCommand;

    private static void Decrypt(Span<byte> buffer, uint mac)
    {
        Span<uint> values = stackalloc uint[32];
        for (int index = 0; index < values.Length; index++)
        {
            values[index] = BinaryPrimitives.ReadUInt32LittleEndian(buffer[(index * 4)..]);
        }

        Span<uint> key = stackalloc uint[4];
        key[0] = BinaryPrimitives.ReverseEndianness(mac);
        const uint delta = 2654435769;
        uint sum = unchecked(delta * 6);
        uint y = values[0];
        while (sum != 0)
        {
            uint e = (sum >> 2) & 3;
            for (int position = values.Length - 1; position > 0; position--)
            {
                uint z = values[position - 1];
                y = values[position] = unchecked(values[position] - Mix(sum, y, z, position, e, key));
            }
            uint last = values[^1];
            y = values[0] = unchecked(values[0] - Mix(sum, y, last, 0, e, key));
            sum = unchecked(sum - delta);
        }

        for (int index = 0; index < values.Length; index++)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(buffer[(index * 4)..], values[index]);
        }
    }

    private static uint Mix(uint sum, uint y, uint z, int position, uint e, ReadOnlySpan<uint> key) =>
        unchecked(((z >> 5 ^ y << 2) + (y >> 3 ^ z << 4)) ^
            ((sum ^ y) + (key[(position & 3) ^ (int)e] ^ z)));
}

using System.Buffers.Binary;
using HidBridge.Protocol;

namespace HidBridge.Host.Transport;

internal static class MakcuModeProtocol
{
    internal const int StatusPayloadLength = 6;
    internal const byte DisableOpcode = 0;
    internal const byte EnableOpcode = 1;
    internal const byte QueryOpcode = 2;

    internal static byte[] CreateRequest(FrameCodec codec, byte opcode, out ushort sequence)
    {
        ArgumentNullException.ThrowIfNull(codec);
        if (opcode > QueryOpcode)
        {
            throw new ArgumentOutOfRangeException(nameof(opcode));
        }

        byte[] frame = codec.Encode(MessageType.MakcuModeRequest, [opcode]);
        sequence = BinaryPrimitives.ReadUInt16LittleEndian(frame.AsSpan(4, sizeof(ushort)));
        return frame;
    }

    internal static bool TryDecodeStatus(
        BridgeFrame frame,
        ushort expectedSequence,
        out MakcuModeSnapshot snapshot)
    {
        snapshot = default;
        if (frame.Type != MessageType.MakcuModeStatus ||
            frame.Sequence != expectedSequence ||
            frame.Payload is null ||
            frame.Payload.Length != StatusPayloadLength ||
            frame.Payload[0] > 4 ||
            frame.Payload[1] > 1)
        {
            return false;
        }

        uint baud = BinaryPrimitives.ReadUInt32LittleEndian(frame.Payload.AsSpan(2, 4));
        bool enabled = frame.Payload[1] != 0;
        uint expectedBaud = (uint)(enabled
            ? MakcuModeCoordinator.MakcuBaudRate
            : MakcuModeCoordinator.A5BaudRate);
        if (baud is not (MakcuModeCoordinator.A5BaudRate or MakcuModeCoordinator.MakcuBaudRate) ||
            baud != expectedBaud)
        {
            return false;
        }

        snapshot = new MakcuModeSnapshot(enabled, (int)baud, frame.Payload[0]);
        return true;
    }

    internal static bool IsExpectedState(MakcuModeSnapshot snapshot, bool enabled, int baudRate) =>
        snapshot.Status == 0 &&
        snapshot.Enabled == enabled &&
        snapshot.BaudRate == baudRate;
}

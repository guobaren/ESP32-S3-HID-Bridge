namespace HidBridge.Protocol;

public readonly record struct BridgeFrame(
    MessageType Type,
    ushort Sequence,
    byte[] Payload);

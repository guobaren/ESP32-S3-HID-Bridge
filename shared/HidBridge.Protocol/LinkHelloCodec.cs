using System.Buffers.Binary;

namespace HidBridge.Protocol;

// UART1 板间诊断编码器。UsbRole 只表示本板当前 USB 证据，不能复制推导角色。
public static class LinkHelloCodec
{
    public const int Length = 13;
    public const int UsbRoleOffset = 12;

    public static byte[] Encode(byte role, byte usbState, ReadOnlySpan<byte> nodeId,
        uint generation, byte usbRole)
    {
        if (role > 2 || usbState > 4 || nodeId.Length != 6 ||
            (usbRole != 0 && usbRole != role))
        {
            throw new ArgumentException("LINK_HELLO 身份、USB 状态或节点字段非法。");
        }

        byte[] payload = new byte[Length];
        payload[0] = role;
        payload[1] = usbState;
        nodeId.CopyTo(payload.AsSpan(2, 6));
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(8, 4), generation);
        payload[UsbRoleOffset] = usbRole;
        return payload;
    }
}

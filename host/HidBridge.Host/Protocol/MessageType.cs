namespace HidBridge.Host.Protocol;

internal enum MessageType : byte
{
    KeyboardReport = 0x01,
    MouseReport = 0x02,
    ReleaseAll = 0x03,
    Ping = 0x04,
}

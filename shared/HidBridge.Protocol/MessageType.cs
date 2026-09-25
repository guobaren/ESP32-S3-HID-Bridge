namespace HidBridge.Protocol;

public enum MessageType : byte
{
    KeyboardReport = 0x01,
    MouseReport = 0x02,
    ReleaseAll = 0x03,
    Ping = 0x04,
    SessionStart = 0x05,
    DeviceProbe = 0x06,
    DeviceHello = 0x07,
    LogReadRequest = 0x08,
    LogReadResponse = 0x09,
    LogClearRequest = 0x0A,
    LogDumpRequest = 0x0B,
}

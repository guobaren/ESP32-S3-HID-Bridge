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
    DiagProfileRead = 0x0D,
    DiagProfileData = 0x0E,
    DiagProfileBegin = 0x0F,
    DiagProfileChunk = 0x10,
    DiagProfileCommit = 0x11,
    DiagProfileResult = 0x12,
    DiagProfileMode = 0x13,
    DiagStreamControl = 0x14,
    DiagStreamEvent = 0x15,
    DiagStreamStatus = 0x16,
    DiagInjectRequest = 0x17,
    DiagInjectResult = 0x18,
    StatsSnapshotRequest = 0x1D,
    StatsSnapshotResponse = 0x1E,
    VendorSessionBegin = 0x35,
    VendorSessionAck = 0x36,
}

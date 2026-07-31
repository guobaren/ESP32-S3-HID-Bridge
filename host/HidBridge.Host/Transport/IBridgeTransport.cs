using HidBridge.Protocol;

namespace HidBridge.Host.Transport;

internal interface IBridgeTransport : IDisposable
{
    void Send(MessageType type, ReadOnlySpan<byte> payload);
}

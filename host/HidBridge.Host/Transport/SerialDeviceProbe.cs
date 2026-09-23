using System.Buffers.Binary;
using System.Diagnostics;
using System.IO.Ports;
using System.Security.Cryptography;
using HidBridge.Protocol;

namespace HidBridge.Host.Transport;

internal static class SerialDeviceProbe
{
    internal const int NonceLength = 8;
    internal const int ProbeTimeoutMilliseconds = 1500;
    private const int MaximumResponseBytes = 4096;
    private const int ProbeRetryMilliseconds = 250;
    internal const byte MouseHostRole = 2;

    private static ReadOnlySpan<byte> HelloSignature => "HIDBRDG2"u8;

    internal static byte[] CreateProbe(
        FrameCodec codec,
        ReadOnlySpan<byte> nonce,
        out ushort sequence)
    {
        ArgumentNullException.ThrowIfNull(codec);
        if (nonce.Length != NonceLength)
        {
            throw new ArgumentOutOfRangeException(nameof(nonce));
        }

        byte[] frame = codec.Encode(MessageType.DeviceProbe, nonce);
        sequence = BinaryPrimitives.ReadUInt16LittleEndian(frame.AsSpan(4, 2));
        return frame;
    }

    internal static bool Probe(SerialPort port, FrameCodec codec, byte? expectedRole = null)
    {
        ArgumentNullException.ThrowIfNull(port);
        ArgumentNullException.ThrowIfNull(codec);

        byte[] nonce = RandomNumberGenerator.GetBytes(NonceLength);
        byte[] probe = CreateProbe(codec, nonce, out ushort sequence);
        byte[] response = new byte[MaximumResponseBytes];
        int responseLength = 0;
        long started = Stopwatch.GetTimestamp();
        long deadline = started + MillisecondsToStopwatchTicks(ProbeTimeoutMilliseconds);
        long nextProbe = started;

        port.DiscardInBuffer();
        while (Stopwatch.GetTimestamp() < deadline)
        {
            long now = Stopwatch.GetTimestamp();
            if (now >= nextProbe)
            {
                port.Write(probe, 0, probe.Length);
                nextProbe = now + MillisecondsToStopwatchTicks(ProbeRetryMilliseconds);
            }

            int available = port.BytesToRead;
            if (available <= 0)
            {
                Thread.Sleep(10);
                continue;
            }

            if (responseLength == response.Length)
            {
                int retained = Math.Min(FrameCodec.MaximumPayloadLength + 8, responseLength);
                response.AsSpan(responseLength - retained, retained).CopyTo(response);
                responseLength = retained;
            }

            int readLength = Math.Min(available, response.Length - responseLength);
            int read = port.Read(response, responseLength, readLength);
            if (read <= 0)
            {
                continue;
            }
            responseLength += read;
            if (TryMatchHello(
                    response.AsSpan(0, responseLength), sequence, nonce, expectedRole))
            {
                port.DiscardInBuffer();
                return true;
            }
        }

        return false;
    }

    internal static bool TryMatchHello(
        ReadOnlySpan<byte> data,
        ushort expectedSequence,
        ReadOnlySpan<byte> expectedNonce,
        byte? expectedRole = null)
    {
        if (expectedNonce.Length != NonceLength)
        {
            return false;
        }

        for (int offset = 0; offset + 9 <= data.Length; offset++)
        {
            if (data[offset] != 0xA5 || data[offset + 1] != 0x5A)
            {
                continue;
            }

            int frameLength = 9 + data[offset + 6];
            if (frameLength > 9 + FrameCodec.MaximumPayloadLength || offset + frameLength > data.Length)
            {
                continue;
            }

            int legacyPayloadLength = HelloSignature.Length + NonceLength;
            if (!FrameCodec.TryDecode(data.Slice(offset, frameLength), out BridgeFrame frame) ||
                frame.Type != MessageType.DeviceHello ||
                frame.Sequence != expectedSequence ||
                (frame.Payload.Length != legacyPayloadLength &&
                 frame.Payload.Length != legacyPayloadLength + 1))
            {
                continue;
            }

            ReadOnlySpan<byte> payload = frame.Payload;
            ReadOnlySpan<byte> nonce = payload.Slice(HelloSignature.Length, NonceLength);
            bool roleMatches = expectedRole is null ||
                (payload.Length == HelloSignature.Length + NonceLength + 1 &&
                 payload[^1] == expectedRole.Value);
            if (payload[..HelloSignature.Length].SequenceEqual(HelloSignature) &&
                nonce.SequenceEqual(expectedNonce) && roleMatches)
            {
                return true;
            }
        }

        return false;
    }

    private static long MillisecondsToStopwatchTicks(int milliseconds) =>
        (long)(milliseconds * (double)Stopwatch.Frequency / 1000);
}

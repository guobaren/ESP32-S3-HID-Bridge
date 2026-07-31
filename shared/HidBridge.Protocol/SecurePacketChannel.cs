using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;

namespace HidBridge.Protocol;

/// <summary>
/// 使用预共享密钥完成双向认证，并用 AES-256-GCM 保护每个 TCP 数据包。
/// </summary>
public sealed class SecurePacketChannel : IDisposable
{
    private const int NonceLength = 16;
    private const int TagLength = 16;
    private const int MaximumPacketLength = 4096;
    private static readonly byte[] ServerMagic = "HBS1"u8.ToArray();
    private static readonly byte[] ClientMagic = "HBC1"u8.ToArray();
    private static readonly byte[] AckMagic = "HBA1"u8.ToArray();

    private readonly Stream _stream;
    private readonly byte[] _key;
    private readonly byte[] _noncePrefix;
    private readonly object _sendLock = new();
    private readonly object _receiveLock = new();
    private ulong _sendCounter;
    private ulong _receiveCounter;
    private bool _disposed;

    private SecurePacketChannel(Stream stream, byte[] key, byte[] noncePrefix)
    {
        _stream = stream;
        _key = key;
        _noncePrefix = noncePrefix;
    }

    public static SecurePacketChannel AuthenticateClient(Stream stream, string presharedKey)
    {
        ArgumentNullException.ThrowIfNull(stream);
        byte[] psk = ValidatePresharedKey(presharedKey);
        byte[] serverHello = ReadExactly(stream, ServerMagic.Length + NonceLength);
        VerifyMagic(serverHello, ServerMagic);
        ReadOnlySpan<byte> serverNonce = serverHello.AsSpan(ServerMagic.Length, NonceLength);

        byte[] clientNonce = RandomNumberGenerator.GetBytes(NonceLength);
        byte[] clientProof = ComputeProof(psk, "client-v1"u8, serverNonce, clientNonce);
        byte[] response = new byte[ClientMagic.Length + NonceLength + clientProof.Length];
        ClientMagic.CopyTo(response, 0);
        clientNonce.CopyTo(response, ClientMagic.Length);
        clientProof.CopyTo(response, ClientMagic.Length + NonceLength);
        stream.Write(response);

        byte[] ack = ReadExactly(stream, AckMagic.Length + 32);
        VerifyMagic(ack, AckMagic);
        byte[] expectedServerProof = ComputeProof(psk, "server-v1"u8, serverNonce, clientNonce);
        if (!CryptographicOperations.FixedTimeEquals(ack.AsSpan(AckMagic.Length), expectedServerProof))
        {
            throw new CryptographicException("远端身份认证失败。");
        }

        return Create(stream, psk, serverNonce, clientNonce);
    }

    public static SecurePacketChannel AuthenticateServer(Stream stream, string presharedKey)
    {
        ArgumentNullException.ThrowIfNull(stream);
        byte[] psk = ValidatePresharedKey(presharedKey);
        byte[] serverNonce = RandomNumberGenerator.GetBytes(NonceLength);
        stream.Write(ServerMagic);
        stream.Write(serverNonce);

        byte[] response = ReadExactly(stream, ClientMagic.Length + NonceLength + 32);
        VerifyMagic(response, ClientMagic);
        ReadOnlySpan<byte> clientNonce = response.AsSpan(ClientMagic.Length, NonceLength);
        byte[] expectedClientProof = ComputeProof(psk, "client-v1"u8, serverNonce, clientNonce);
        if (!CryptographicOperations.FixedTimeEquals(
                response.AsSpan(ClientMagic.Length + NonceLength),
                expectedClientProof))
        {
            throw new CryptographicException("客户端身份认证失败。");
        }

        byte[] serverProof = ComputeProof(psk, "server-v1"u8, serverNonce, clientNonce);
        stream.Write(AckMagic);
        stream.Write(serverProof);
        return Create(stream, psk, serverNonce, clientNonce);
    }

    public void Send(ReadOnlySpan<byte> plaintext)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (plaintext.IsEmpty || plaintext.Length > MaximumPacketLength)
        {
            throw new ArgumentOutOfRangeException(nameof(plaintext));
        }

        lock (_sendLock)
        {
            ulong counter = checked(++_sendCounter);
            byte[] header = new byte[10];
            BinaryPrimitives.WriteUInt16BigEndian(header.AsSpan(0, 2), (ushort)plaintext.Length);
            BinaryPrimitives.WriteUInt64BigEndian(header.AsSpan(2, 8), counter);
            byte[] ciphertext = new byte[plaintext.Length];
            byte[] tag = new byte[TagLength];
            byte[] nonce = CreateNonce(counter);

            using AesGcm aes = new(_key, TagLength);
            aes.Encrypt(nonce, plaintext, ciphertext, tag, header);

            _stream.Write(header);
            _stream.Write(ciphertext);
            _stream.Write(tag);
        }
    }

    public byte[] Receive()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        lock (_receiveLock)
        {
            byte[] header = ReadExactly(_stream, 10);
            int length = BinaryPrimitives.ReadUInt16BigEndian(header.AsSpan(0, 2));
            ulong counter = BinaryPrimitives.ReadUInt64BigEndian(header.AsSpan(2, 8));
            if (length <= 0 || length > MaximumPacketLength)
            {
                throw new InvalidDataException("安全数据包长度无效。");
            }
            if (counter != _receiveCounter + 1)
            {
                throw new CryptographicException("安全数据包计数器不连续，连接可能被重放或篡改。");
            }

            byte[] ciphertext = ReadExactly(_stream, length);
            byte[] tag = ReadExactly(_stream, TagLength);
            byte[] plaintext = new byte[length];
            byte[] nonce = CreateNonce(counter);
            using AesGcm aes = new(_key, TagLength);
            aes.Decrypt(nonce, ciphertext, tag, plaintext, header);
            _receiveCounter = counter;
            return plaintext;
        }
    }

    private static SecurePacketChannel Create(
        Stream stream,
        byte[] psk,
        ReadOnlySpan<byte> serverNonce,
        ReadOnlySpan<byte> clientNonce)
    {
        byte[] key = ComputeProof(psk, "key-v1"u8, serverNonce, clientNonce);
        byte[] nonceMaterial = ComputeProof(psk, "nonce-v1"u8, serverNonce, clientNonce);
        return new SecurePacketChannel(stream, key, nonceMaterial[..4]);
    }

    private byte[] CreateNonce(ulong counter)
    {
        byte[] nonce = new byte[12];
        _noncePrefix.CopyTo(nonce, 0);
        BinaryPrimitives.WriteUInt64BigEndian(nonce.AsSpan(4, 8), counter);
        return nonce;
    }

    private static byte[] ValidatePresharedKey(string presharedKey)
    {
        byte[] psk = Encoding.UTF8.GetBytes(presharedKey ?? string.Empty);
        if (psk.Length < 16)
        {
            throw new InvalidOperationException("网络预共享密钥至少需要 16 个 UTF-8 字节。");
        }
        return psk;
    }

    private static byte[] ComputeProof(
        byte[] psk,
        ReadOnlySpan<byte> label,
        ReadOnlySpan<byte> serverNonce,
        ReadOnlySpan<byte> clientNonce)
    {
        byte[] material = new byte[label.Length + serverNonce.Length + clientNonce.Length];
        label.CopyTo(material);
        serverNonce.CopyTo(material.AsSpan(label.Length));
        clientNonce.CopyTo(material.AsSpan(label.Length + serverNonce.Length));
        return HMACSHA256.HashData(psk, material);
    }

    private static void VerifyMagic(ReadOnlySpan<byte> data, ReadOnlySpan<byte> expected)
    {
        if (!data.StartsWith(expected))
        {
            throw new InvalidDataException("安全通道握手标识无效。");
        }
    }

    private static byte[] ReadExactly(Stream stream, int length)
    {
        byte[] buffer = new byte[length];
        stream.ReadExactly(buffer);
        return buffer;
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        CryptographicOperations.ZeroMemory(_key);
        _stream.Dispose();
    }
}

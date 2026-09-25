using System.Buffers.Binary;
using System.Diagnostics;
using System.IO.Ports;
using System.Text;
using HidBridge.Protocol;

namespace HidBridge.Host.Transport;

/// <summary>板载日志转存进度。</summary>
/// <param name="BytesReceived">已收到的日志字节数。</param>
/// <param name="ChunkCount">已收到的 LOG_READ_RESPONSE 分片数（含结束分片）。</param>
/// <param name="DeviceTotalBytes">板端在响应里上报的日志总字节数。</param>
internal sealed record OnboardLogDownloadProgress(int BytesReceived, int ChunkCount, uint DeviceTotalBytes);

/// <summary>板载日志转存结果。</summary>
/// <param name="BytesReceived">实际收到的日志字节数。</param>
/// <param name="ChunkCount">收到的 LOG_READ_RESPONSE 分片数（含结束分片）。</param>
/// <param name="DeviceTotalBytes">板端最后上报的日志总字节数；0 表示没有任何有效上报。</param>
/// <param name="Completed">是否正常结束：收到空 data 结束帧，或已覆盖板端上报的总量。</param>
/// <param name="TimedOut">是否因为长时间收不到后续分片而结束。</param>
/// <param name="Elapsed">从发出 LOG_DUMP_REQUEST 到结束的耗时。</param>
internal sealed record OnboardLogDownloadResult(
    int BytesReceived,
    int ChunkCount,
    uint DeviceTotalBytes,
    bool Completed,
    bool TimedOut,
    TimeSpan Elapsed)
{
    /// <summary>板端完全没有回应：端口选错、板子没跑日志服务，或该串口上只有控制台文本。</summary>
    internal bool NoResponse => !Completed && ChunkCount == 0;

    /// <summary>下载量少于板端上报总量：本次被上限或超时截断，尾部仍在板上，可再转存一次。</summary>
    internal bool Truncated => DeviceTotalBytes > (uint)Math.Max(0, BytesReceived);
}

/// <summary>协议帧回调：messageType、sequence 与 payload（payload 仅在回调期间有效）。</summary>
internal delegate void OnboardLogFrameHandler(byte messageType, ushort sequence, ReadOnlySpan<byte> payload);

/// <summary>转存用的串口读写抽象：真实实现包装 SerialPort，离线验证可用合成流。</summary>
internal interface IOnboardLogChannel : IDisposable
{
    /// <summary>写入一条完整帧。</summary>
    void Write(ReadOnlySpan<byte> frame);

    /// <summary>读取一段字节；返回 0 表示当前没有数据（调用方会继续等待）。</summary>
    int Read(Span<byte> buffer);
}

/// <summary>
/// 在混杂控制台文本的字节流里扫描 <c>A5 5A</c> 协议帧：找不到帧头、长度非法或 CRC 不符时后移
/// 一字节继续找，并允许同一条帧跨多个读取窗口到达。校验规则与 <see cref="FrameCodec.TryDecode"/> 一致。
/// </summary>
internal sealed class OnboardLogFrameScanner
{
    private static readonly byte[] FrameMagic = [0xA5, 0x5A];

    private byte[] _pending = new byte[1024];
    private int _count;

    /// <summary>投喂一段原始字节，并对扫描到的每一条合法帧调用 <paramref name="handler"/>。</summary>
    internal void Feed(ReadOnlySpan<byte> chunk, OnboardLogFrameHandler handler)
    {
        Append(chunk);
        int start = 0;
        while (true)
        {
            ReadOnlySpan<byte> pending = _pending.AsSpan(start, _count - start);
            int magic = pending.IndexOf(FrameMagic);
            if (magic < 0)
            {
                // 没有帧头：只保留末尾 1 字节，它可能是被读取窗口拆开的 A5。
                start = Math.Max(start, _count - 1);
                break;
            }

            start += magic;
            pending = _pending.AsSpan(start, _count - start);
            if (pending.Length < OnboardLogDownloader.FrameHeaderLength)
            {
                break; // 长度字节还没到齐，等下一个读取窗口
            }

            int payloadLength = pending[6];
            if (payloadLength > FrameCodec.MaximumPayloadLength)
            {
                start += 2; // 载荷长度非法：这不是真帧头，从下一个字节继续找
                continue;
            }

            int total = OnboardLogDownloader.FrameOverheadLength + payloadLength;
            if (pending.Length < total)
            {
                break; // 帧体还没收全
            }

            if (!FrameCodec.TryDecode(pending[..total], out BridgeFrame frame))
            {
                start += 2; // 版本或 CRC 不符：后移一字节继续扫描
                continue;
            }

            handler((byte)frame.Type, frame.Sequence, frame.Payload);
            start += total;
        }

        if (start > 0)
        {
            Buffer.BlockCopy(_pending, start, _pending, 0, _count - start);
            _count -= start;
        }
    }

    private void Append(ReadOnlySpan<byte> chunk)
    {
        int required = _count + chunk.Length;
        if (required > _pending.Length)
        {
            Array.Resize(ref _pending, Math.Max(required, _pending.Length * 2));
        }
        chunk.CopyTo(_pending.AsSpan(_count));
        _count = required;
    }
}

/// <summary>
/// 板载日志转存：在指定串口上发一条 <c>LOG_DUMP_REQUEST{offset=0, max_bytes}</c>，流式收下
/// 板端连续回的多条 <c>LOG_READ_RESPONSE</c>，按 UTF-8 追加写入本地文件。
/// 协议见 <c>docs/protocol.md</c>「板载日志下载命令」；打开串口时显式保持 DTR/RTS 为低，
/// 转存动作本身不会复位被测板。
/// </summary>
internal static class OnboardLogDownloader
{
    internal const int DefaultBaudRate = 921600;

    /// <summary>默认请求上限 1 MB（覆盖板端 4×128 KB 轮转日志）。</summary>
    internal const uint DefaultMaxBytes = 1U << 20;

    /// <summary>板端单帧数据上限（<c>data:1..56</c>）。</summary>
    internal const int MaximumChunkBytes = 56;

    /// <summary>每累计多少字节回调一次进度，避免几千条分片把 UI 日志刷爆。</summary>
    internal const int ProgressReportIntervalBytes = 32 * 1024;

    /// <summary>分片之间（以及首片之前）的最长等待时间。</summary>
    internal static readonly TimeSpan DefaultTimeout = TimeSpan.FromSeconds(30);

    /// <summary>首片等待窗口：板子没响应时快速失败，而不是干等 30 秒。</summary>
    internal static readonly TimeSpan DefaultFirstResponseTimeout = TimeSpan.FromSeconds(3);

    /// <summary>
    /// 已覆盖板端上报总量后仍继续收流的宽限期。板端在转存期间仍在写日志时 <c>total_bytes</c>
    /// 会持续增长，立即结束会丢掉仍在途的最新几行，因此给一个很短的宽限窗口等结束帧。
    /// </summary>
    private static readonly TimeSpan ReportedEndGraceTimeout = TimeSpan.FromMilliseconds(500);

    private const int LogChunkHeaderLength = 8; // offset:u32 + total_bytes:u32
    private const int ReadBufferSize = 4096;
    private const int ReadWindowMilliseconds = 100;
    private const int WriteWindowMilliseconds = 500;
    private const int NoDataSleepMilliseconds = 1;
    private const int PortOpenAttempts = 5;
    private const int PortOpenRetryDelayMilliseconds = 200;
    private const ushort DumpSequence = 1;

    /// <summary>帧头长度：magic(2) + version(1) + type(1) + sequence(2) + payload_length(1)。</summary>
    internal const int FrameHeaderLength = 7;

    /// <summary>整帧固定开销：帧头 + CRC16(2)。</summary>
    internal const int FrameOverheadLength = 9;

    /// <summary>
    /// 按协议把 <paramref name="portName"/> 上的板载日志转存到 <paramref name="outputPath"/>。
    /// 串口在本方法返回前一定关闭；结果里的 <see cref="OnboardLogDownloadResult.BytesReceived"/>
    /// 是实际下载字节数。
    /// </summary>
    internal static Task<OnboardLogDownloadResult> DownloadAsync(
        string portName,
        string outputPath,
        IProgress<OnboardLogDownloadProgress>? progress = null,
        int baudRate = DefaultBaudRate,
        uint maxBytes = DefaultMaxBytes,
        TimeSpan? timeout = null,
        TimeSpan? firstResponseTimeout = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(portName);
        ArgumentException.ThrowIfNullOrWhiteSpace(outputPath);
        if (maxBytes == 0)
        {
            throw new ArgumentOutOfRangeException(nameof(maxBytes), maxBytes, "转存上限必须大于 0。");
        }

        TimeSpan effectiveTimeout = RequirePositive(timeout ?? DefaultTimeout, nameof(timeout));
        TimeSpan effectiveFirstResponse = RequirePositive(
            firstResponseTimeout ?? DefaultFirstResponseTimeout,
            nameof(firstResponseTimeout));
        return Task.Run(
            () =>
            {
                using IOnboardLogChannel channel = OpenChannel(portName, baudRate);
                return PumpDump(
                    channel,
                    outputPath,
                    progress,
                    maxBytes,
                    effectiveFirstResponse,
                    effectiveTimeout,
                    cancellationToken);
            },
            cancellationToken);
    }

    /// <summary>
    /// 在给定通道上跑一遍完整下载循环（含请求帧编码、流式收帧、UTF-8 落盘）。
    /// 供离线合成流验证使用；真实串口请用 <see cref="DownloadAsync"/>。
    /// </summary>
    internal static OnboardLogDownloadResult DownloadFromChannel(
        IOnboardLogChannel channel,
        string outputPath,
        IProgress<OnboardLogDownloadProgress>? progress = null,
        uint maxBytes = DefaultMaxBytes,
        TimeSpan? timeout = null,
        TimeSpan? firstResponseTimeout = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(channel);
        ArgumentException.ThrowIfNullOrWhiteSpace(outputPath);
        return PumpDump(
            channel,
            outputPath,
            progress,
            maxBytes,
            RequirePositive(firstResponseTimeout ?? DefaultFirstResponseTimeout, nameof(firstResponseTimeout)),
            RequirePositive(timeout ?? DefaultTimeout, nameof(timeout)),
            cancellationToken);
    }

    /// <summary>
    /// 按共享帧格式编码一条命令帧。序列号由调用方显式给出，因此不复用
    /// <see cref="FrameCodec"/> 的内部自增序号；CRC 直接复用 <see cref="FrameCodec.ComputeCrc16"/>。
    /// </summary>
    internal static byte[] EncodeFrame(MessageType type, ushort sequence, ReadOnlySpan<byte> payload)
    {
        if (payload.Length > FrameCodec.MaximumPayloadLength)
        {
            throw new ArgumentOutOfRangeException(
                nameof(payload),
                payload.Length,
                $"协议帧载荷不得超过 {FrameCodec.MaximumPayloadLength} 字节。");
        }

        byte[] frame = new byte[FrameOverheadLength + payload.Length];
        frame[0] = 0xA5;
        frame[1] = 0x5A;
        frame[2] = FrameCodec.Version;
        frame[3] = (byte)type;
        BinaryPrimitives.WriteUInt16LittleEndian(frame.AsSpan(4, 2), sequence);
        frame[6] = (byte)payload.Length;
        payload.CopyTo(frame.AsSpan(7));
        BinaryPrimitives.WriteUInt16LittleEndian(
            frame.AsSpan(7 + payload.Length, 2),
            FrameCodec.ComputeCrc16(frame.AsSpan(2, 5 + payload.Length)));
        return frame;
    }

    /// <summary>LOG_DUMP_REQUEST 的载荷：<c>offset:u32</c> + <c>max_bytes:u32</c>。</summary>
    internal static byte[] BuildDumpPayload(uint offset, uint maxBytes)
    {
        byte[] payload = new byte[8];
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(0, 4), offset);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(4, 4), maxBytes);
        return payload;
    }

    private static OnboardLogDownloadResult PumpDump(
        IOnboardLogChannel channel,
        string outputPath,
        IProgress<OnboardLogDownloadProgress>? progress,
        uint maxBytes,
        TimeSpan firstResponseTimeout,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        string fullPath = Path.GetFullPath(outputPath);
        string? directory = Path.GetDirectoryName(fullPath);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        OnboardLogFrameScanner scanner = new();
        byte[] readBuffer = new byte[ReadBufferSize];
        UTF8Encoding encoding = new(encoderShouldEmitUTF8Identifier: false);
        Decoder decoder = encoding.GetDecoder();
        char[] charBuffer = new char[encoding.GetMaxCharCount(MaximumChunkBytes)];
        Stopwatch stopwatch = Stopwatch.StartNew();
        int bytesReceived = 0;
        int chunkCount = 0;
        uint deviceTotalBytes = 0;
        long nextProgressBytes = ProgressReportIntervalBytes;
        TimeSpan? reportedEndElapsed = null;
        bool completed = false;
        bool timedOut = false;

        using FileStream fileStream = new(
            fullPath,
            FileMode.Create,
            FileAccess.Write,
            FileShare.Read,
            64 * 1024,
            FileOptions.SequentialScan);
        using StreamWriter writer = new(fileStream, encoding);

        channel.Write(EncodeFrame(
            MessageType.LogDumpRequest,
            DumpSequence,
            BuildDumpPayload(0, maxBytes)));
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (completed)
            {
                break;
            }
            if (reportedEndElapsed is TimeSpan reportedEnd &&
                stopwatch.Elapsed - reportedEnd >= ReportedEndGraceTimeout)
            {
                completed = true; // 已覆盖板端上报总量，且宽限期内没有新分片
                break;
            }
            if (stopwatch.Elapsed >= (chunkCount == 0 ? firstResponseTimeout : timeout))
            {
                timedOut = true;
                break;
            }

            int read;
            try
            {
                read = channel.Read(readBuffer);
            }
            catch (TimeoutException)
            {
                continue; // 本读取窗口没有数据，回到循环顶部判断超时
            }
            if (read <= 0)
            {
                Thread.Sleep(NoDataSleepMilliseconds);
                continue;
            }

            scanner.Feed(readBuffer.AsSpan(0, read), (messageType, frameSequence, payload) =>
            {
                if (completed ||
                    messageType != (byte)MessageType.LogReadResponse ||
                    frameSequence != DumpSequence ||
                    payload.Length < LogChunkHeaderLength)
                {
                    // 控制台日志共用同一串口，其它帧（Ping、HELLO、旧响应）一律忽略。
                    return;
                }

                uint offset = BinaryPrimitives.ReadUInt32LittleEndian(payload[..4]);
                uint total = BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(4, 4));
                ReadOnlySpan<byte> data = payload[LogChunkHeaderLength..];
                deviceTotalBytes = total;
                chunkCount++;
                if (data.Length == 0)
                {
                    completed = true; // 板端以空 data 分片收尾
                    return;
                }

                bytesReceived += data.Length;
                reportedEndElapsed = null; // 有新数据，撤销“疑似结束”
                WriteDecoded(writer, decoder, charBuffer, data);
                if (bytesReceived >= maxBytes)
                {
                    completed = true; // 达到本次请求上限
                    return;
                }
                if (total > 0 && offset + (uint)data.Length >= total)
                {
                    reportedEndElapsed = stopwatch.Elapsed;
                }
                if (bytesReceived >= nextProgressBytes)
                {
                    nextProgressBytes += ProgressReportIntervalBytes;
                    progress?.Report(new OnboardLogDownloadProgress(bytesReceived, chunkCount, deviceTotalBytes));
                }
            });
        }

        WriteDecoded(writer, decoder, charBuffer, ReadOnlySpan<byte>.Empty, flush: true);
        writer.Flush();
        return new OnboardLogDownloadResult(
            bytesReceived,
            chunkCount,
            deviceTotalBytes,
            completed,
            timedOut,
            stopwatch.Elapsed);
    }

    /// <summary>把分片数据按 UTF-8 解码后写入文件；跨分片的多字节字符由同一个 Decoder 拼接。</summary>
    private static void WriteDecoded(
        StreamWriter writer,
        Decoder decoder,
        char[] charBuffer,
        ReadOnlySpan<byte> data,
        bool flush = false)
    {
        int charCount = decoder.GetChars(data, charBuffer, flush);
        if (charCount > 0)
        {
            writer.Write(charBuffer, 0, charCount);
        }
    }

    private static IOnboardLogChannel OpenChannel(string portName, int baudRate)
    {
        try
        {
            return SerialOnboardLogChannel.Open(portName, baudRate);
        }
        catch (UnauthorizedAccessException exception)
        {
            throw new IOException(
                $"串口 {portName} 被其它程序占用或拒绝访问（{exception.Message}）；" +
                "请关闭 idf.py monitor、串口助手等占用该端口的程序后重试。",
                exception);
        }
        catch (IOException exception)
        {
            throw new IOException($"串口 {portName} 打开失败（{exception.Message}）。", exception);
        }
        catch (ArgumentException exception)
        {
            throw new IOException($"串口名 {portName} 无效（{exception.Message}）。", exception);
        }
    }

    private static TimeSpan RequirePositive(TimeSpan value, string name) =>
        value > TimeSpan.Zero
            ? value
            : throw new ArgumentOutOfRangeException(name, value, "超时时间必须大于 0。");

    /// <summary>真实串口通道：显式保持 DTR/RTS 为低，避免转存动作本身复位被测板。</summary>
    private sealed class SerialOnboardLogChannel : IOnboardLogChannel
    {
        private readonly SerialPort _port;
        private readonly byte[] _readBuffer = new byte[ReadBufferSize];

        private SerialOnboardLogChannel(SerialPort port) => _port = port;

        internal static SerialOnboardLogChannel Open(string portName, int baudRate)
        {
            SerialPort port = new(portName, baudRate, Parity.None, 8, StopBits.One)
            {
                Handshake = Handshake.None,
                // CH340 会跟随 DTR/RTS 拉复位脚：转存日志不能复位被测板。
                DtrEnable = false,
                RtsEnable = false,
                ReadTimeout = ReadWindowMilliseconds,
                WriteTimeout = WriteWindowMilliseconds,
            };
            try
            {
                OpenWithRetry(port, portName);
                port.DtrEnable = false;
                port.RtsEnable = false;
                // 丢掉打开前残留在驱动缓冲里的旧字节（可能是上一轮的帧或控制台文本）。
                port.DiscardInBuffer();
                return new SerialOnboardLogChannel(port);
            }
            catch
            {
                port.Dispose();
                throw;
            }
        }

        public void Write(ReadOnlySpan<byte> frame) => _port.Write(frame.ToArray(), 0, frame.Length);

        public int Read(Span<byte> buffer)
        {
            int read = _port.Read(_readBuffer, 0, Math.Min(_readBuffer.Length, buffer.Length));
            _readBuffer.AsSpan(0, read).CopyTo(buffer);
            return read;
        }

        public void Dispose() => _port.Dispose();

        private static void OpenWithRetry(SerialPort port, string portName)
        {
            for (int attempt = 1; ; attempt++)
            {
                try
                {
                    port.Open();
                    return;
                }
                catch (Exception exception) when (
                    attempt < PortOpenAttempts &&
                    exception is IOException or UnauthorizedAccessException)
                {
                    // 控制软件刚释放串口时 Windows 可能还握着句柄一小会儿，退避重试即可。
                    Console.WriteLine(
                        $"打开 {portName} 失败（第 {attempt} 次）：{exception.Message}；" +
                        $"{PortOpenRetryDelayMilliseconds} ms 后重试。");
                    Thread.Sleep(PortOpenRetryDelayMilliseconds);
                }
            }
        }
    }
}

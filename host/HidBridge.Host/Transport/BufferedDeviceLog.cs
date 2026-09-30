using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;

namespace HidBridge.Host.Transport;

// 接收线程只入队；文件写入、定时刷新和关闭由后台线程独占。
internal sealed class BufferedDeviceLog : IDisposable
{
    private readonly ConcurrentQueue<(string Line, int Bytes)> _queue = new();
    private readonly AutoResetEvent _wake = new(false);
    private readonly TextWriter _writer;
    private readonly Thread _worker;
    private readonly object _gate = new();
    private readonly int _capacityBytes;
    private long _queuedBytes, _peakBytes, _accepted, _written, _dropped, _writeErrors;
    private bool _closed;
    private bool _finished;

    internal BufferedDeviceLog(TextWriter writer, int capacityBytes = 8 * 1024 * 1024)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(capacityBytes);
        _writer = writer;
        _capacityBytes = capacityBytes;
        _worker = new Thread(WriteLoop) { IsBackground = true, Name = "设备日志写盘" };
        _worker.Start();
    }

    internal long Dropped => Interlocked.Read(ref _dropped);
    internal long Written => Interlocked.Read(ref _written);
    internal long WriteErrors => Interlocked.Read(ref _writeErrors);
    internal long PeakBytes => Interlocked.Read(ref _peakBytes);

    internal bool TryWrite(string line)
    {
        // 按UTF-16对象占用估算预算，另计对象/队列开销，避免巨大行绕过容量约束。
        long size = (long)line.Length * 2 + 96;
        lock (_gate)
        {
            if (_closed || size > _capacityBytes - _queuedBytes)
            {
                Interlocked.Increment(ref _dropped);
                return false;
            }
            _queuedBytes += size;
            _peakBytes = Math.Max(_peakBytes, _queuedBytes);
            ++_accepted;
            _queue.Enqueue((line, (int)size));
            _wake.Set();
            return true;
        }
    }

    private void WriteLoop()
    {
        long reportedDrops = 0;
        long nextFlush = Stopwatch.GetTimestamp() + Stopwatch.Frequency / 2;
        bool failed = false;
        long nextStats = Stopwatch.GetTimestamp() + 5 * Stopwatch.Frequency;
        try
        {
            while (true)
            {
                // 每批有界，持续高流量下也必须按时刷新和报告缺口。
                int batch = 0;
                while (batch++ < 256 && _queue.TryDequeue(out var item))
                {
                    lock (_gate) { _queuedBytes -= item.Bytes; }
                    if (failed)
                    {
                        Interlocked.Increment(ref _dropped);
                        continue;
                    }
                    try
                    {
                        _writer.WriteLine(item.Line);
                        Interlocked.Increment(ref _written);
                    }
                    catch (Exception error) when (error is IOException or UnauthorizedAccessException or ObjectDisposedException)
                    {
                        failed = true;
                        Interlocked.Increment(ref _writeErrors);
                        Interlocked.Increment(ref _dropped);
                        Console.Error.WriteLine($"设备日志写盘失败：{error.Message}；接收继续，本文件不完整。");
                    }
                }
                long now = Stopwatch.GetTimestamp();
                bool closing;
                lock (_gate) { closing = _closed && _queue.IsEmpty; }
                if (!failed && (closing || now >= nextFlush))
                {
                    long drops = Dropped;
                    if (closing || drops != reportedDrops || now >= nextStats)
                    {
                        _writer.WriteLine($"[日志写盘统计] accepted={_accepted} written={Written} dropped={drops} peak_bytes={PeakBytes} write_errors={WriteErrors}");
                        if (drops != reportedDrops)
                        {
                            Console.Error.WriteLine($"设备日志缺口：累计丢弃 {drops} 行，峰值队列 {PeakBytes} 字节。");
                        }
                        reportedDrops = drops;
                        nextStats = now + 5 * Stopwatch.Frequency;
                    }
                    _writer.Flush();
                    nextFlush = now + Stopwatch.Frequency / 2;
                }
                if (closing) { break; }
                if (_queue.IsEmpty) { _wake.WaitOne(100); }
            }
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or ObjectDisposedException)
        {
            Interlocked.Increment(ref _writeErrors);
            Console.Error.WriteLine($"设备日志刷新失败：{error.Message}；文件完整性未知。");
        }
        finally
        {
            lock (_gate)
            {
                _closed = true;
                while (_queue.TryDequeue(out _)) { Interlocked.Increment(ref _dropped); }
                _queuedBytes = 0;
            }
            try { _writer.Dispose(); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or ObjectDisposedException)
            {
                Interlocked.Increment(ref _writeErrors);
                Console.Error.WriteLine($"设备日志关闭失败：{error.Message}。");
            }
            lock (_gate)
            {
                _finished = true;
                _wake.Dispose();
            }
        }
    }

    public void Dispose()
    {
        lock (_gate)
        {
            _closed = true;
            if (!_finished) { _wake.Set(); }
        }
        if (!_worker.Join(2000))
        {
            Console.Error.WriteLine("设备日志关闭等待超时；后台继续排空，尚不能确认日志已全部落盘。");
        }
    }
}

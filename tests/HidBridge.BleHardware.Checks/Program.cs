using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO.Ports;
using System.Text;
using System.Text.RegularExpressions;
using HidBridge.Host.Input;
using HidBridge.Protocol;

const string TargetAddress = "E88485461D82";
string port = ReadArgument(args, "--port") ?? throw new ArgumentException("必须指定 --port，例如 COM3");
int baud = int.TryParse(ReadArgument(args, "--baud"), out int parsedBaud) ? parsedBaud : 921600;

ConcurrentQueue<RawSample> rawSamples = new();
ConcurrentQueue<MouseStats> mouseStats = new();
ConcurrentQueue<string> logLines = new();
using ManualResetEventSlim rawReady = new();
Control? rawDispatcher = null;
Exception? rawThreadException = null;
Thread rawThread = new(() =>
{
    try
    {
        using Control dispatcher = new();
        dispatcher.CreateControl();
        rawDispatcher = dispatcher;
        using RawMouseInputWindow rawWindow = new((device, mouse) =>
        {
            string name = RawMouseInputWindow.GetDeviceName(device);
            if (name.Contains(TargetAddress, StringComparison.OrdinalIgnoreCase))
            {
                rawSamples.Enqueue(new RawSample(mouse.LastX, mouse.LastY, Stopwatch.GetTimestamp(), name));
            }
        });
        rawReady.Set();
        Application.Run();
    }
    catch (Exception exception)
    {
        rawThreadException = exception;
        rawReady.Set();
    }
})
{
    IsBackground = true,
    Priority = ThreadPriority.Highest,
    Name = "BLE Raw Input capture",
};
rawThread.SetApartmentState(ApartmentState.STA);
rawThread.Start();
rawReady.Wait(TimeSpan.FromSeconds(3));
if (rawThreadException is not null || rawDispatcher is null)
{
    throw new InvalidOperationException("启动 BLE Raw Input 捕获线程失败", rawThreadException);
}
using SerialPort serial = new(port, baud)
{
    DtrEnable = false,
    RtsEnable = false,
    ReadTimeout = 100,
    WriteTimeout = 1000,
    Encoding = Encoding.UTF8,
};

serial.Open();
using CancellationTokenSource readerCancellation = new();
Thread reader = new(() => ReadLogs(serial, readerCancellation.Token, mouseStats, logLines))
{
    IsBackground = true,
    Name = "ble-hardware-log-reader",
};
reader.Start();
FrameCodec codec = new();

try
{
    WaitUntil(() => mouseStats.TryPeek(out _), TimeSpan.FromSeconds(3), "等待固件鼠标统计超时");
    foreach (short dx in new short[] { 20, 5, 1 })
    {
        RunCase(serial, codec, dx, rawSamples, mouseStats);
        PumpFor(TimeSpan.FromSeconds(1.8));
    }
    if (logLines.Any(line => line.Contains("输入帧序号不连续", StringComparison.Ordinal) ||
        line.Contains("丢弃输出帧", StringComparison.Ordinal)))
    {
        throw new InvalidOperationException("固件日志包含序号不连续或输出丢弃");
    }
    Console.WriteLine("真实 BLE HID 20 ms 鼠标移动检查通过");
}
finally
{
    Send(serial, codec, MessageType.ReleaseAll, []);
    readerCancellation.Cancel();
    reader.Join(TimeSpan.FromSeconds(1));
    rawDispatcher.BeginInvoke((Action)Application.ExitThread);
    rawThread.Join(TimeSpan.FromSeconds(1));
}

static void RunCase(
    SerialPort serial,
    FrameCodec codec,
    short dx,
    ConcurrentQueue<RawSample> rawSamples,
    ConcurrentQueue<MouseStats> mouseStats)
{
    MouseStats baseline = mouseStats.Last();
    while (rawSamples.TryDequeue(out _))
    {
    }

    Send(serial, codec, MessageType.SessionStart, []);
    PumpFor(TimeSpan.FromMilliseconds(250));
    List<double> sendIntervals = [];
    Exception? senderException = null;
    Thread sender = new(() =>
    {
        try
        {
            long started = Stopwatch.GetTimestamp();
            long previousSend = 0;
            for (int index = 0; index < 50; index++)
            {
                SpinUntil(started + (long)(index * 0.020 * Stopwatch.Frequency));
                Send(serial, codec, MessageType.MouseReport, MouseReportCodec.Encode(0, dx, 0, 0, 0));
                long sent = Stopwatch.GetTimestamp();
                if (previousSend != 0)
                {
                    sendIntervals.Add((sent - previousSend) * 1000.0 / Stopwatch.Frequency);
                }
                previousSend = sent;
            }
        }
        catch (Exception exception)
        {
            senderException = exception;
        }
    })
    {
        IsBackground = true,
        Priority = ThreadPriority.Highest,
        Name = $"BLE 20ms sender dx={dx}",
    };
    sender.Start();
    sender.Join();
    if (senderException is not null)
    {
        throw new InvalidOperationException("测试发送线程失败", senderException);
    }
    PumpFor(TimeSpan.FromSeconds(1.2));

    RawSample[] allRaw = rawSamples.ToArray();
    RawSample[] actualRaw = allRaw.Where(sample => sample.X != 0 || sample.Y != 0).ToArray();
    MouseStats final = mouseStats.Last();
    int rawX = actualRaw.Sum(sample => sample.X);
    double[] rawGaps = actualRaw.Zip(actualRaw.Skip(1), (left, right) =>
        (right.Timestamp - left.Timestamp) * 1000.0 / Stopwatch.Frequency).ToArray();
    var actual = new
    {
        FirmwareReceived = final.Received - baseline.Received,
        FirmwareReceivedX = final.ReceivedX - baseline.ReceivedX,
        RawReports = actualRaw.Length,
        ReleaseReports = allRaw.Length - actualRaw.Length,
        RawX = rawX,
        RawY = actualRaw.Sum(sample => sample.Y),
        MaxRawGapMs = rawGaps.DefaultIfEmpty(0).Max(),
        MaxSendGapMs = sendIntervals.Max(),
        Device = actualRaw.Select(sample => sample.Device).FirstOrDefault() ?? "(未收到)",
    };
    Console.WriteLine($"用例 dx={dx}：期望 帧=50 位移={dx * 50}；实际 {actual}");

    Require(actual.FirmwareReceived == 50, $"固件接收帧期望 50，实际 {actual.FirmwareReceived}");
    Require(actual.FirmwareReceivedX == dx * 50, $"固件接收位移期望 {dx * 50}，实际 {actual.FirmwareReceivedX}");
    Require(actual.RawReports == 50, $"Windows BLE Raw Input 帧期望 50，实际 {actual.RawReports}");
    Require(actual.RawX == dx * 50, $"Windows BLE Raw Input X 期望 {dx * 50}，实际 {actual.RawX}");
    Require(actual.RawY == 0, $"Windows BLE Raw Input Y 期望 0，实际 {actual.RawY}");
    Require(actual.MaxRawGapMs <= 40, $"Windows BLE Raw Input 最大间隔期望 <=40 ms，实际 {actual.MaxRawGapMs:F3} ms");
    Require(actual.MaxSendGapMs <= 25, $"测试发送最大间隔期望 <=25 ms，实际 {actual.MaxSendGapMs:F3} ms");
}

static void ReadLogs(
    SerialPort serial,
    CancellationToken cancellation,
    ConcurrentQueue<MouseStats> mouseStats,
    ConcurrentQueue<string> lines)
{
    Regex regex = new(
        @"接收=(?<received>\d+) 位移=\((?<receivedX>-?\d+),(?<receivedY>-?\d+)\).*" +
        @"提交=(?<submitted>\d+) 位移=\((?<submittedX>-?\d+),(?<submittedY>-?\d+)\).*" +
        @"完成=(?<completed>\d+) 位移=\((?<completedX>-?\d+),(?<completedY>-?\d+)\)",
        RegexOptions.Compiled);
    while (!cancellation.IsCancellationRequested)
    {
        try
        {
            string line = serial.ReadLine();
            lines.Enqueue(line);
            Match match = regex.Match(line);
            if (match.Success)
            {
                MouseStats stats = new(
                    long.Parse(match.Groups["received"].Value),
                    long.Parse(match.Groups["receivedX"].Value));
                mouseStats.Enqueue(stats);
                Console.WriteLine(line.TrimEnd());
            }
        }
        catch (TimeoutException)
        {
        }
        catch (InvalidOperationException) when (cancellation.IsCancellationRequested)
        {
            return;
        }
    }
}

static void Send(SerialPort serial, FrameCodec codec, MessageType type, ReadOnlySpan<byte> payload)
{
    byte[] frame = codec.Encode(type, payload);
    serial.Write(frame, 0, frame.Length);
    serial.BaseStream.Flush();
}

static void PumpUntil(long targetTimestamp)
{
    while (Stopwatch.GetTimestamp() < targetTimestamp)
    {
        Application.DoEvents();
        Thread.SpinWait(64);
    }
}

static void SpinUntil(long targetTimestamp)
{
    while (Stopwatch.GetTimestamp() < targetTimestamp)
    {
        Thread.SpinWait(64);
    }
}

static void PumpFor(TimeSpan duration) =>
    PumpUntil(Stopwatch.GetTimestamp() + (long)(duration.TotalSeconds * Stopwatch.Frequency));

static void WaitUntil(Func<bool> predicate, TimeSpan timeout, string error)
{
    long deadline = Stopwatch.GetTimestamp() + (long)(timeout.TotalSeconds * Stopwatch.Frequency);
    while (!predicate())
    {
        if (Stopwatch.GetTimestamp() >= deadline)
        {
            throw new TimeoutException(error);
        }
        Application.DoEvents();
        Thread.Sleep(10);
    }
}

static string? ReadArgument(string[] arguments, string name)
{
    int index = Array.IndexOf(arguments, name);
    return index >= 0 && index + 1 < arguments.Length ? arguments[index + 1] : null;
}

static void Require(bool condition, string message)
{
    if (!condition)
    {
        throw new InvalidOperationException(message);
    }
}

internal readonly record struct RawSample(int X, int Y, long Timestamp, string Device);
internal readonly record struct MouseStats(long Received, long ReceivedX);

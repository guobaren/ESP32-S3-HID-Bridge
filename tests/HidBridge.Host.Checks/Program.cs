using System.Runtime.InteropServices;
using System.Collections.Concurrent;
using System.Drawing;
using System.Drawing.Imaging;
using System.Net.Sockets;
using System.Text;
using HidBridge.Host;
using HidBridge.Host.Automation;
using HidBridge.Host.FirmwareUpdate;
using HidBridge.Host.Input;
using HidBridge.Host.RemoteInput;
using HidBridge.Host.Transport;
using HidBridge.Host.Ui;
using HidBridge.Protocol;

CheckMouseReportCodec();
CheckMouseAggregation();
CheckSimulatedUdpInputAggregation();
CheckUdpSmoothingSwitch();
CheckUdpMouseSmoothing();
CheckMouseStatisticsLoggingDoesNotBlockPump();
CheckMouseMovementRecordingAndChart();
CheckSerialDiscoveryProtocol();
CheckWiFiBoardTransportDisabled();
CheckDeviceLogPolicy();
CheckInputSuppressionPolicy();
CheckKeyboardAutoRepeatEdgeFiltering();
CheckInputCaptureThreadIsolation();
CheckUnexpectedInputCaptureExitReleasesAll();
CheckForwardingNotificationFailureStillReleasesAll();
CheckInputCallbackFailureStillReleasesAll();
CheckLuaReleaseCannotBlockInputCapture();
CheckCursorLockGeometry();
CheckUiLogWriter();
CheckRemoteInputUdpPath();
CheckFirmwareUpdateApiPolicy();
CheckFirmwareUpdateApiLoopback();
CheckAutomationProfilesAndRuntime();
CheckAutomationRemoteOutput();
CheckLocalMouseTriggersReachLua();
CheckTriggerForwardingIntegration();
CheckWindowLayout();
Console.WriteLine("全部主机检查通过：鼠标协议、500 Hz 聚合、可选频率模拟 UDP、UDP 平滑开关、左右键移动记录与分析图、串口握手、Wi-Fi 开发板输入禁用闸门、可切换日志策略、输入独占策略、UDP 网络输入、本机固件刷写 API 策略、宏配置导入、宏/Lua 执行、本机侧键 Lua 触发、自动化远端输出、实时日志、Lua 配置管理和四页窗口布局。");

static void CheckMouseReportCodec()
{
    MouseReport expected = new(0x15, short.MinValue, short.MaxValue, sbyte.MinValue, sbyte.MaxValue);
    byte[] payload = MouseReportCodec.Encode(
        expected.Buttons,
        expected.X,
        expected.Y,
        expected.Wheel,
        expected.Pan);
    Require(payload.Length == MouseReportCodec.Length, "鼠标报告长度应为 7 字节");
    Require(MouseReportCodec.TryDecode(payload, out MouseReport actual), "鼠标报告应能解码");
    Require(actual == expected, "16 位鼠标报告往返结果不一致");

    FrameCodec codec = new();
    byte[] frame = codec.Encode(MessageType.MouseReport, payload);
    Require(frame[2] == 2, "协议版本应为 v2");
    Require(FrameCodec.TryDecode(frame, out BridgeFrame decoded), "v2 帧应能解码");
    Require(decoded.Payload.SequenceEqual(payload), "帧内鼠标 Payload 不一致");
}

static void CheckMouseAggregation()
{
    RecordingTransport transport = new();
    using MouseReportPump pump = new(transport);
    pump.ResetAndSendRelease(true);

    for (int index = 0; index < 1000; index++)
    {
        pump.Accumulate(0, false, 1, -2, 0, 0);
    }
    pump.Accumulate(1, true, 0, 0, 0, 0);
    pump.Accumulate(0, true, 0, 0, 0, 0);

    DateTime deadline = DateTime.UtcNow.AddSeconds(2);
    while (DateTime.UtcNow < deadline)
    {
        MouseReport[] reports = transport.MouseReports();
        if (reports.Sum(report => (long)report.X) == 1000 &&
            reports.Sum(report => (long)report.Y) == -2000 &&
            reports.Select(report => report.Buttons).ContainsSequence((byte)1, (byte)0))
        {
            break;
        }
        Thread.Sleep(5);
    }

    MouseReport[] actual = transport.MouseReports();
    Require(actual.Sum(report => (long)report.X) == 1000, "累计 X 位移必须完整送出");
    Require(actual.Sum(report => (long)report.Y) == -2000, "累计 Y 位移必须完整送出");
    Require(
        actual.Select(report => report.Buttons).ContainsSequence((byte)1, (byte)0),
        "鼠标按下和释放必须保序");
    Require(actual.Length <= 10, "1002 个原始事件不应退化为逐事件发送");

    int timingStart = actual.Length;
    for (int index = 0; index < 20; index++)
    {
        pump.Accumulate((byte)(index % 2), true, 0, 0, 0, 0);
    }
    deadline = DateTime.UtcNow.AddSeconds(2);
    while (DateTime.UtcNow < deadline && transport.MouseReports().Length < timingStart + 20)
    {
        Thread.Sleep(5);
    }
    long[] timestamps = transport.MouseTimestamps().Skip(timingStart).Take(20).ToArray();
    Require(timestamps.Length == 20, "500 Hz 发送线程未送完按钮测试序列");
    double elapsedMilliseconds =
        (timestamps[^1] - timestamps[0]) * 1000.0 / System.Diagnostics.Stopwatch.Frequency;
    Require(elapsedMilliseconds >= 30, "报告被集中突发发送，未遵守 500 Hz 上限");
    Require(elapsedMilliseconds < 500, "500 Hz 发送线程出现异常长时间停顿");

    pump.ResetAndSendRelease(false);
    int countAfterStop = transport.MouseReports().Length;
    pump.Accumulate(0, false, 100, 100, 0, 0);
    Thread.Sleep(10);
    Require(transport.MouseReports().Length == countAfterStop, "停止转发后不得继续发送鼠标报告");
}

static void CheckSimulatedUdpInputAggregation()
{
    Require(
        SimulatedUdpMouseInput.SupportedFrequencies.SequenceEqual([30, 60, 100, 140, 200, 500, 0]),
        "模拟 UDP 输入频率列表不正确");

    const long timestampFrequency = 1_000_000;
    foreach (int frequencyHz in SimulatedUdpMouseInput.SupportedFrequencies.Where(value => value > 0))
    {
        SimulatedUdpMouseInput aggregator = new(timestampFrequency);
        aggregator.Configure(true, frequencyHz, nowTimestamp: 0);
        long intervalTicks = (timestampFrequency + frequencyHz - 1) / frequencyHz;
        Require(
            !aggregator.Accumulate(7, -5, 2, -1, out _),
            $"模拟 UDP {frequencyHz} Hz 不应逐事件立即输出");
        Require(
            !aggregator.Accumulate(3, 1, -1, 1, out _),
            $"模拟 UDP {frequencyHz} Hz 不应逐事件立即输出");
        Require(
            !aggregator.TryFlush(intervalTicks - 1, out _),
            $"模拟 UDP {frequencyHz} Hz 在周期到达前不得提前输出");
        Require(
            aggregator.TryFlush(intervalTicks, out MouseDelta bucket),
            $"模拟 UDP {frequencyHz} Hz 到达周期后未输出整合数据");
        Require(
            bucket == new MouseDelta(10, -4, 1, 0),
            $"模拟 UDP {frequencyHz} Hz 整合后各轴总量不正确：{bucket}");
        Require(
            !aggregator.TryFlush(intervalTicks * 2, out _),
            $"模拟 UDP {frequencyHz} Hz 无新输入时不得产生空数据报");
    }

    SimulatedUdpMouseInput hundredHz = new(timestampFrequency);
    hundredHz.Configure(true, 100, nowTimestamp: 0);
    Require(!hundredHz.Accumulate(1, 2, 0, 0, out _), "模拟 UDP 100 Hz 不应立即输出");
    Require(!hundredHz.TryFlush(9_999, out _), "模拟 UDP 100 Hz 必须整合完整 10 ms");
    Require(hundredHz.TryFlush(10_000, out MouseDelta hundredHzBucket), "模拟 UDP 100 Hz 未按 10 ms 输出");
    Require(hundredHzBucket == new MouseDelta(1, 2, 0, 0), "模拟 UDP 100 Hz 输出数据不正确");

    SimulatedUdpMouseInput unlimited = new(timestampFrequency);
    unlimited.Configure(true, SimulatedUdpMouseInput.UnlimitedFrequencyHz, nowTimestamp: 0);
    Require(unlimited.Unlimited, "模拟 UDP 无上限模式状态不正确");
    Require(
        unlimited.Accumulate(7, -5, 2, -1, out MouseDelta firstUnlimited) &&
        firstUnlimited == new MouseDelta(7, -5, 2, -1),
        $"无上限模式第一条原始事件未立即形成独立命令：{firstUnlimited}");
    Require(
        unlimited.Accumulate(3, 1, -1, 1, out MouseDelta secondUnlimited) &&
        secondUnlimited == new MouseDelta(3, 1, -1, 1),
        $"无上限模式第二条原始事件未立即形成独立命令：{secondUnlimited}");
    Require(!unlimited.TryFlush(long.MaxValue, out _), "无上限模式不应等待定时分桶");
    SimulatedUdpInputStatistics unlimitedStatistics = unlimited.GetStatistics();
    Require(unlimitedStatistics.EmittedBuckets == 2, "无上限模式应逐事件生成两条模拟 UDP 命令");
    Require(
        unlimitedStatistics.PendingX == 0 && unlimitedStatistics.PendingY == 0 &&
        unlimitedStatistics.PendingWheel == 0 && unlimitedStatistics.PendingPan == 0,
        "无上限模式不得保留源分桶积压");

    RecordingTransport transport = new();
    using MouseReportPump pump = new(transport);
    pump.ConfigureSimulatedUdpInput(true, 100);
    pump.ResetAndSendRelease(true);
    Require(pump.SimulatedUdpInputEnabled, "500 Hz 泵未保持模拟 UDP 开关状态");
    Require(pump.SimulatedUdpInputFrequencyHz == 100, "500 Hz 泵未保持模拟 UDP 频率");

    pump.Accumulate(1, true, 0, 0, 0, 0);
    DateTime deadline = DateTime.UtcNow.AddSeconds(2);
    while (DateTime.UtcNow < deadline && transport.MouseReports().Length < 1)
    {
        Thread.Sleep(2);
    }
    Require(transport.MouseReports().Length >= 1, "模拟 UDP 模式下按钮报告未送出");
    Require(
        transport.MouseReports().FirstOrDefault().Buttons == 1,
        "模拟 UDP 模式下鼠标按钮必须即时进入报告链路");

    for (int index = 0; index < 10; index++)
    {
        pump.Accumulate(1, false, 2, -1, 0, 0);
    }
    deadline = DateTime.UtcNow.AddSeconds(2);
    while (DateTime.UtcNow < deadline &&
           transport.MouseReports().Sum(report => (long)report.X) < 20)
    {
        Thread.Sleep(2);
    }
    MouseReport[] actual = transport.MouseReports();
    Require(actual.Sum(report => (long)report.X) == 20, "模拟 UDP 实际链路 X 总量不守恒");
    Require(actual.Sum(report => (long)report.Y) == -10, "模拟 UDP 实际链路 Y 总量不守恒");

    pump.ConfigureSimulatedUdpInput(false, 100);
    Require(!pump.SimulatedUdpInputEnabled, "模拟 UDP 开关关闭后状态不正确");
    pump.Accumulate(1, false, 5, 3, 0, 0);
    deadline = DateTime.UtcNow.AddSeconds(2);
    while (DateTime.UtcNow < deadline &&
           transport.MouseReports().Sum(report => (long)report.X) < 25)
    {
        Thread.Sleep(2);
    }
    actual = transport.MouseReports();
    Require(actual.Sum(report => (long)report.X) == 25, "关闭模拟 UDP 后直接输入 X 未送出");
    Require(actual.Sum(report => (long)report.Y) == -7, "关闭模拟 UDP 后直接输入 Y 未送出");

    RecordingTransport switchTransport = new();
    using MouseReportPump switchPump = new(switchTransport);
    switchPump.ConfigureSimulatedUdpInput(true, 30);
    switchPump.ResetAndSendRelease(true);
    switchPump.Accumulate(0, false, 11, -6, 0, 0);
    switchPump.ConfigureSimulatedUdpInput(true, 200);
    deadline = DateTime.UtcNow.AddSeconds(2);
    while (DateTime.UtcNow < deadline &&
           switchTransport.MouseReports().Sum(report => (long)report.X) < 11)
    {
        Thread.Sleep(2);
    }
    MouseReport[] switched = switchTransport.MouseReports();
    Require(switched.Sum(report => (long)report.X) == 11, "切换模拟 UDP 频率时缓冲 X 丢失");
    Require(switched.Sum(report => (long)report.Y) == -6, "切换模拟 UDP 频率时缓冲 Y 丢失");

    int beforeRealUdp = switched.Length;
    switchPump.AccumulateRemote(13, 4, 0, 0);
    deadline = DateTime.UtcNow.AddSeconds(2);
    while (DateTime.UtcNow < deadline &&
           switchTransport.MouseReports().Sum(report => (long)report.X) < 24)
    {
        Thread.Sleep(2);
    }
    switched = switchTransport.MouseReports();
    Require(switched.Length > beforeRealUdp, "模拟频率设置不应阻止真实 UDP 输入进入公共链路");
    Require(switched.Sum(report => (long)report.X) == 24, "真实 UDP 与模拟 UDP 公共链路 X 总量不正确");
    Require(switched.Sum(report => (long)report.Y) == -2, "真实 UDP 与模拟 UDP 公共链路 Y 总量不正确");

    switchPump.ConfigureSimulatedUdpInput(true, 30);
    switchPump.Accumulate(0, false, 9, 9, 0, 0);
    switchPump.ResetAndSendRelease(false);
    long stoppedX = switchTransport.MouseReports().Sum(report => (long)report.X);
    Thread.Sleep(50);
    Require(
        switchTransport.MouseReports().Sum(report => (long)report.X) == stoppedX,
        "停止同步后不得继续发送模拟 UDP 缓冲移动");
    Console.WriteLine(
        "模拟 UDP 输入检查：频率=[30,60,100,140,200,500] Hz 和无上限；" +
        "100 Hz=10 ms、500 Hz=2 ms；无上限逐原始事件生成命令；" +
        "频率只影响模拟源，按钮即时发送，真实 UDP 共用后续链路，停止后无残留移动。");
}

static void CheckUdpSmoothingSwitch()
{
    RecordingTransport directTransport = new();
    using MouseReportPump directPump = new(directTransport);
    Require(directPump.UdpSmoothingEnabled, "UDP 平滑默认必须开启");
    directPump.ConfigureUdpSmoothing(false);
    Require(!directPump.UdpSmoothingEnabled, "UDP 平滑开关关闭后状态不正确");
    directPump.ResetAndSendRelease(true);

    directPump.AccumulateRemote(37, -19, 2, -1);
    DateTime deadline = DateTime.UtcNow.AddSeconds(2);
    while (DateTime.UtcNow < deadline && directTransport.MouseReports().Length == 0)
    {
        Thread.Sleep(2);
    }

    MouseReport[] direct = directTransport.MouseReports();
    Require(direct.Length == 1, $"关闭平滑后单条 UDP 命令应直接产生 1 个报告，实际={direct.Length}");
    Require(direct[0].X == 37, $"关闭平滑后 X 应直接输出 37，实际={direct[0].X}");
    Require(direct[0].Y == -19, $"关闭平滑后 Y 应直接输出 -19，实际={direct[0].Y}");
    Require(direct[0].Wheel == 2, $"关闭平滑后 Wheel 应直接输出 2，实际={direct[0].Wheel}");
    Require(direct[0].Pan == -1, $"关闭平滑后 Pan 应直接输出 -1，实际={direct[0].Pan}");

    directPump.ResetAndSendRelease(false);
    directPump.ConfigureUdpSmoothing(true);
    Require(directPump.UdpSmoothingEnabled, "UDP 平滑开关重新开启后状态不正确");

    RecordingTransport preserveTransport = new();
    using MouseReportPump preservePump = new(preserveTransport);
    preservePump.ResetAndSendRelease(true);
    for (int index = 0; index < 10; index++)
    {
        preservePump.AccumulateRemote(1, 0, 0, 0);
    }
    preservePump.AccumulateRemote(50, 0, 0, 0);
    preservePump.ConfigureUdpSmoothing(false);
    Require(preservePump.UdpSmoothingEnabled, "同步开启时不应允许切换 UDP 平滑");
    deadline = DateTime.UtcNow.AddSeconds(2);
    while (DateTime.UtcNow < deadline &&
           preserveTransport.MouseReports().Sum(report => (long)report.X) < 60)
    {
        Thread.Sleep(2);
    }
    Require(
        preserveTransport.MouseReports().Sum(report => (long)report.X) == 60,
        "同步中拒绝切换后，旧平滑队列总位移必须完整输出");
    preservePump.ResetAndSendRelease(false);
    preservePump.ConfigureUdpSmoothing(false);
    Require(!preservePump.UdpSmoothingEnabled, "HOME 关闭后应允许切换 UDP 平滑");

    Console.WriteLine(
        "UDP 平滑开关检查：默认开启；关闭后单条命令实际直接输出 " +
        $"X={direct[0].X},Y={direct[0].Y},Wheel={direct[0].Wheel},Pan={direct[0].Pan}；" +
        "同步中拒绝切换且旧队列 X=60 完整输出，HOME 关闭后可安全切换。");
}

static void CheckMouseMovementRecordingAndChart()
{
    DateTime start = new(2026, 8, 9, 2, 0, 0, DateTimeKind.Utc);
    MouseMovementRecorder stateMachine = new(TimeSpan.FromSeconds(3));

    stateMachine.ObserveReport(1, 8, -6, start, out bool started);
    Require(!started, "只按左键不得开始鼠标移动记录");
    stateMachine.ObserveReport(3, -12, 7, start.AddMilliseconds(10), out started);
    Require(started, "同时按下左右键必须开始鼠标移动记录");
    stateMachine.ObserveReport(
        3,
        short.MinValue,
        short.MaxValue,
        start.AddMilliseconds(20),
        out started);
    Require(!started, "同一次按键期间不得重复开始记录");
    stateMachine.ObserveReport(0, 2, -3, start.AddMilliseconds(30), out _);
    Require(
        stateMachine.TryComplete(start.AddMilliseconds(3029)) is null,
        "左右键松开未满 3 秒时不得结束记录");

    // 3 秒内重新按下任一按键会取消停止倒计时，并继续同一份记录。
    stateMachine.ObserveReport(3, 1, 1, start.AddMilliseconds(2000), out _);
    stateMachine.ObserveReport(0, -4, 5, start.AddMilliseconds(2100), out _);
    Require(
        stateMachine.TryComplete(start.AddMilliseconds(5099)) is null,
        "第二次松开未满 3 秒时不得结束记录");
    MouseMovementRecording recording = stateMachine.TryComplete(start.AddMilliseconds(5100))
        ?? throw new InvalidOperationException("左右键松开满 3 秒后未生成移动记录");
    Require(
        recording.XValues.SequenceEqual([-12, short.MinValue, 2, 1, -4]),
        $"X 有符号值记录顺序不正确：{string.Join(',', recording.XValues)}");
    Require(
        recording.YValues.SequenceEqual([7, short.MaxValue, -3, 1, 5]),
        $"Y 有符号值记录顺序不正确：{string.Join(',', recording.YValues)}");
    System.Drawing.Rectangle plot = new(100, 50, 400, 300);
    System.Drawing.PointF negativeXPoint = MouseMovementAnalysisRenderer.MapVerticalTimePoint(
        value: -10,
        index: 0,
        count: 3,
        minimum: -20,
        maximum: 20,
        plot);
    System.Drawing.PointF zeroXPoint = MouseMovementAnalysisRenderer.MapVerticalTimePoint(
        value: 0,
        index: 1,
        count: 3,
        minimum: -20,
        maximum: 20,
        plot);
    System.Drawing.PointF positiveXPoint = MouseMovementAnalysisRenderer.MapVerticalTimePoint(
        value: 20,
        index: 2,
        count: 3,
        minimum: -20,
        maximum: 20,
        plot);
    Require(negativeXPoint.Y == plot.Bottom, "X 时间顺序的首个样本必须位于图表底部");
    Require(positiveXPoint.Y == plot.Top, "X 时间顺序的末个样本必须位于图表顶部");
    Require(
        negativeXPoint.X < zeroXPoint.X && zeroXPoint.X < positiveXPoint.X,
        "X 图必须把负值显示在零轴左侧、正值显示在零轴右侧");
    float negativeY = MouseMovementAnalysisRenderer.MapSignedValueY(-10, -20, 20, plot);
    float zeroY = MouseMovementAnalysisRenderer.MapSignedValueY(0, -20, 20, plot);
    float positiveY = MouseMovementAnalysisRenderer.MapSignedValueY(10, -20, 20, plot);
    Require(
        positiveY < zeroY && zeroY < negativeY,
        "Y 图必须把正值显示在零轴上方、负值显示在零轴下方");
    string directory = Path.Combine(Path.GetTempPath(), $"hidbridge-mouse-chart-{Guid.NewGuid():N}");
    try
    {
        string path = MouseMovementAnalysisRenderer.SavePng(recording, directory);
        Require(File.Exists(path), "鼠标移动分析 PNG 未生成");
        using System.Drawing.Bitmap bitmap = new(path);
        Require(bitmap.Width == 1600 && bitmap.Height == 800, "鼠标移动分析 PNG 尺寸不正确");
        int nonWhiteSamples = 0;
        for (int y = 0; y < bitmap.Height; y += 20)
        {
            for (int x = 0; x < bitmap.Width; x += 20)
            {
                if (bitmap.GetPixel(x, y).ToArgb() != System.Drawing.Color.White.ToArgb())
                {
                    nonWhiteSamples++;
                }
            }
        }
        Require(nonWhiteSamples > 20, "鼠标移动分析 PNG 没有实际绘制图表内容");
    }
    finally
    {
        if (Directory.Exists(directory))
        {
            Directory.Delete(directory, recursive: true);
        }
    }

    RecordingTransport transport = new();
    MouseMovementRecorder fastRecorder = new(TimeSpan.FromMilliseconds(50));
    using MouseReportPump pump = new(transport, fastRecorder);
    using ManualResetEventSlim completed = new(false);
    MouseMovementRecording? emittedRecording = null;
    int startedCount = 0;
    pump.MovementRecordingStarted += () => Interlocked.Increment(ref startedCount);
    pump.MovementRecordingCompleted += result =>
    {
        emittedRecording = result;
        completed.Set();
    };
    pump.ResetAndSendRelease(true);
    pump.Accumulate(3, true, -9, 4, 0, 0);
    pump.Accumulate(0, true, 0, 0, 0, 0);
    Require(completed.Wait(TimeSpan.FromSeconds(2)), "实际 500 Hz 发送链路未触发移动记录完成事件");
    Require(startedCount == 1, "实际发送链路的移动记录开始事件次数不正确");
    MouseMovementRecording actualEmitted = emittedRecording
        ?? throw new InvalidOperationException("实际发送链路未返回移动记录");
    Require(
        actualEmitted.XValues.SequenceEqual([-9]) && actualEmitted.YValues.SequenceEqual([4]),
        "实际发送报告没有保留正负号进入移动记录");
    Console.WriteLine(
        $"鼠标移动记录检查：状态机 X=[{string.Join(',', recording.XValues)}]，" +
        $"Y=[{string.Join(',', recording.YValues)}]；分析图=1600x800，X 时间顺序=底部到顶部并显示正负零轴；" +
        $"500 Hz 实际发送链路 X=[{string.Join(',', actualEmitted.XValues)}]，" +
        $"Y=[{string.Join(',', actualEmitted.YValues)}]。");
}

static void CheckUdpMouseSmoothing()
{
    UdpMouseSmoother smoother = new();
    smoother.Enqueue(new MouseDelta(50, -7, 5, -5));
    UdpMouseSmootherStatistics statistics = smoother.GetStatistics();
    Require(statistics.SmoothingSlots == 10, "UDP 低延迟平滑窗必须为 10 个 2 ms 槽");
    Require(statistics.PendingSlots == 10, "首条 UDP 命令必须立即分摊到 10 个发送槽");

    List<MouseDelta> parts = [];
    for (int index = 0; index < UdpMouseSmoother.SmoothingSlots; index++)
    {
        Require(smoother.TryDequeue(out MouseDelta part), $"UDP 平滑第 {index + 1} 槽没有输出");
        parts.Add(part);
    }
    Require(parts[0].X == 5, "首条快速移动不应整包跳变，首槽应仅输出 X=5");
    Require(parts.All(part => part.X == 5), "X=50 拆成 10 份时每份必须为 5");
    Require(parts.Sum(part => part.X) == 50, "UDP 平滑后的 X 总位移不守恒");
    Require(parts.Sum(part => part.Y) == -7, "UDP 平滑后的负 Y 总位移不守恒");
    Require(parts.Sum(part => part.Wheel) == 5, "UDP 平滑后的滚轮总量不守恒");
    Require(parts.Sum(part => part.Pan) == -5, "UDP 平滑后的负横向滚轮总量不守恒");
    Require(smoother.GetStatistics().PendingSlots == 0, "10 槽输出后不应残留平滑积压");

    smoother.Reset();
    Require(!smoother.TryDequeue(out _), "重置 UDP 平滑器后不得继续发送旧移动");
    smoother.Reset();
    for (int index = 0; index < 10; index++)
    {
        smoother.Enqueue(new MouseDelta(1, -1, 0, 0));
    }
    List<MouseDelta> smallBurst = [];
    for (int index = 0; index < UdpMouseSmoother.SmoothingSlots; index++)
    {
        Require(smoother.TryDequeue(out MouseDelta smallPart), "10 条小步输入未覆盖全部平滑槽");
        smallBurst.Add(smallPart);
    }
    Require(
        smallBurst.All(delta => delta.X == 1 && delta.Y == -1),
        $"10 条小步输入应均匀分散为每槽 (1,-1)，实际={string.Join(";", smallBurst)}");

    smoother.Reset();
    List<MouseDelta> continuous = [];
    for (int index = 0; index < 100; index++)
    {
        smoother.Enqueue(new MouseDelta(50, -25, 0, 0));
        Require(smoother.TryDequeue(out MouseDelta output), "连续输入的当前 2 ms 槽没有输出");
        continuous.Add(output);
    }
    for (int index = 0; index < UdpMouseSmoother.SmoothingSlots - 1; index++)
    {
        Require(smoother.TryDequeue(out MouseDelta tail), "连续输入停止后的平滑尾部提前中断");
        continuous.Add(tail);
    }
    Require(
        continuous.Take(10).Select(delta => delta.X).SequenceEqual(
            [5L, 10L, 15L, 20L, 25L, 30L, 35L, 40L, 45L, 50L]),
        $"连续移动启动斜坡不正确：{string.Join(',', continuous.Take(10).Select(delta => delta.X))}");
    Require(
        continuous.TakeLast(9).Select(delta => delta.X).SequenceEqual(
            [45L, 40L, 35L, 30L, 25L, 20L, 15L, 10L, 5L]),
        $"连续移动停止尾部不正确：{string.Join(',', continuous.TakeLast(9).Select(delta => delta.X))}");
    Require(continuous.Sum(delta => delta.X) == 5_000, "连续输入平滑后的 X 总位移不守恒");
    Require(continuous.Sum(delta => delta.Y) == -2_500, "连续输入平滑后的 Y 总位移不守恒");
    Require(smoother.GetStatistics().PendingSlots == 0, "连续输入停止 9 槽后仍有平滑积压");

    smoother.Reset();
    for (int index = 0; index < 1_000; index++)
    {
        smoother.Enqueue(new MouseDelta(10, -10, 0, 0));
    }
    statistics = smoother.GetStatistics();
    Require(statistics.PendingSlots == 10, "突发输入只能占用固定 10 个未来槽");
    Require(statistics.OverlappingCommands == 999, "突发输入重叠分摊计数不正确");
    long burstX = 0;
    long burstY = 0;
    for (int index = 0; index < UdpMouseSmoother.SmoothingSlots; index++)
    {
        Require(smoother.TryDequeue(out MouseDelta burstPart), "突发输入固定槽提前中断");
        burstX += burstPart.X;
        burstY += burstPart.Y;
    }
    Require(burstX == 10_000 && burstY == -10_000, "突发输入固定槽合并后总位移不守恒");

    RecordingTransport transport = new();
    using MouseReportPump pump = new(transport);
    pump.ResetAndSendRelease(true);
    long injectedTimestamp = System.Diagnostics.Stopwatch.GetTimestamp();
    pump.AccumulateRemote(50, -10, 5, -5);
    DateTime deadline = DateTime.UtcNow.AddSeconds(2);
    while (DateTime.UtcNow < deadline && transport.MouseReports().Length < 10)
    {
        Thread.Sleep(2);
    }
    MouseReport[] actual = transport.MouseReports();
    Require(actual.Length == 10, $"500 Hz 实际低延迟平滑链路报告数不正确：{actual.Length}");
    Require(actual.All(report => report.X == 5), "实际链路未从首条命令开始按 X=5 平滑输出");
    Require(actual.Sum(report => (long)report.X) == 50, "实际平滑链路 X 总位移不守恒");
    Require(actual.Sum(report => (long)report.Y) == -10, "实际平滑链路 Y 总位移不守恒");
    Require(actual.Sum(report => (long)report.Wheel) == 5, "实际平滑链路滚轮总量不守恒");
    Require(actual.Sum(report => (long)report.Pan) == -5, "实际平滑链路横向滚轮总量不守恒");
    long[] actualTimestamps = transport.MouseTimestamps().TakeLast(10).ToArray();
    double completionMilliseconds =
        (actualTimestamps[^1] - injectedTimestamp) * 1000.0 / System.Diagnostics.Stopwatch.Frequency;
    double maximumReportGapMilliseconds = actualTimestamps
        .Zip(actualTimestamps.Skip(1), (previous, current) =>
            (current - previous) * 1000.0 / System.Diagnostics.Stopwatch.Frequency)
        .Max();
    Require(
        completionMilliseconds < 100,
        $"实际低延迟平滑尾部超过 100 ms：{completionMilliseconds:F1} ms");
    Require(
        maximumReportGapMilliseconds < 50,
        $"实际低延迟平滑报告出现超过 50 ms 的间隔：{maximumReportGapMilliseconds:F1} ms");
    Console.WriteLine(
        "UDP 低延迟平滑检查：首条 (50,-7,5,-5) 立即分摊为 10 槽且首槽 X=5；" +
        "同一泵周期 10 条 (1,-1) 均匀分散为每槽 (1,-1)；" +
        "连续 100 个输入只保留 9 槽尾部，总量 X=5000/Y=-2500；" +
        "1000 条突发输入仍固定 10 槽；500 Hz 实际链路输出 10 份且总量守恒，" +
        $"实际尾部完成={completionMilliseconds:F1} ms，最大报告间隔={maximumReportGapMilliseconds:F1} ms。");
}

static void CheckMouseStatisticsLoggingDoesNotBlockPump()
{
    RecordingTransport transport = new();
    using ManualResetEventSlim statisticsEntered = new(false);
    using ManualResetEventSlim releaseStatistics = new(false);
    using MouseReportPump pump = new(
        transport,
        statisticsInterval: TimeSpan.FromMilliseconds(20),
        statisticsSink: _ =>
        {
            statisticsEntered.Set();
            releaseStatistics.Wait(TimeSpan.FromSeconds(2));
        });

    try
    {
        pump.ResetAndSendRelease(true);
        Require(
            statisticsEntered.Wait(TimeSpan.FromSeconds(1)),
            "鼠标统计专用日志线程未收到统计快照");

        int reportsBefore = transport.MouseReports().Length;
        long injectedTimestamp = System.Diagnostics.Stopwatch.GetTimestamp();
        pump.AccumulateRemote(100, -50, 0, 0);
        DateTime deadline = DateTime.UtcNow.AddSeconds(1);
        while (DateTime.UtcNow < deadline && transport.MouseReports().Length - reportsBefore < 10)
        {
            Thread.Sleep(2);
        }

        MouseReport[] actual = transport.MouseReports().Skip(reportsBefore).Take(10).ToArray();
        Require(actual.Length == 10, $"统计日志阻塞期间报告数不正确：{actual.Length}");
        Require(actual.Sum(report => (long)report.X) == 100, "统计日志阻塞期间 X 位移不守恒");
        Require(actual.Sum(report => (long)report.Y) == -50, "统计日志阻塞期间 Y 位移不守恒");
        long[] timestamps = transport.MouseTimestamps().Skip(reportsBefore).Take(10).ToArray();
        double completionMilliseconds =
            (timestamps[^1] - injectedTimestamp) * 1000.0 / System.Diagnostics.Stopwatch.Frequency;
        double maximumGapMilliseconds = timestamps
            .Zip(timestamps.Skip(1), (previous, current) =>
                (current - previous) * 1000.0 / System.Diagnostics.Stopwatch.Frequency)
            .Max();
        Require(
            completionMilliseconds < 100,
            $"统计日志阻塞拖慢 500 Hz 输出尾部：{completionMilliseconds:F1} ms");
        Require(
            maximumGapMilliseconds < 50,
            $"统计日志阻塞造成报告间隔过大：{maximumGapMilliseconds:F1} ms");
        Console.WriteLine(
            $"鼠标统计异步检查：日志线程阻塞时 10 份报告仍完成，尾部={completionMilliseconds:F1} ms，" +
            $"最大间隔={maximumGapMilliseconds:F1} ms。 ");
    }
    finally
    {
        releaseStatistics.Set();
    }
}

static void CheckSerialDiscoveryProtocol()
{
    byte[] nonce = [0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87];
    FrameCodec probeCodec = new();
    byte[] probe = SerialDeviceProbe.CreateProbe(probeCodec, nonce, out ushort probeSequence);
    Require(FrameCodec.TryDecode(probe, out BridgeFrame decodedProbe), "设备探测帧应能解码");
    Require(decodedProbe.Type == MessageType.DeviceProbe, "设备探测帧类型不正确");
    Require(decodedProbe.Sequence == probeSequence, "设备探测序号不一致");
    Require(decodedProbe.Payload.SequenceEqual(nonce), "设备探测随机数不一致");

    byte[] helloPayload = "HIDBRDG2"u8.ToArray().Concat(nonce).ToArray();
    FrameCodec helloCodec = new();
    byte[] hello = helloCodec.Encode(MessageType.DeviceHello, helloPayload);
    byte[] noisyInput = "ESP-ROM:esp32s3\r\n"u8.ToArray().Concat(hello).ToArray();
    Require(
        SerialDeviceProbe.TryMatchHello(noisyInput, probeSequence, nonce),
        "应能跳过串口日志并识别设备响应");
    Require(
        !SerialDeviceProbe.TryMatchHello(noisyInput.AsSpan(0, noisyInput.Length - 1), probeSequence, nonce),
        "不完整设备响应不得被接受");

    byte[] wrongNonce = nonce.ToArray();
    wrongNonce[0] ^= 0xFF;
    Require(
        !SerialDeviceProbe.TryMatchHello(noisyInput, probeSequence, wrongNonce),
        "随机数不匹配的响应不得被接受");
}

static void CheckWiFiBoardTransportDisabled()
{
    BridgeOptions.Validate(new BridgeOptions { Transport = "serial" });

    bool rejected = false;
    try
    {
        BridgeOptions.Validate(new BridgeOptions
        {
            Transport = "wifi",
            NetworkPresharedKey = "1234567890123456",
        });
    }
    catch (InvalidDataException exception) when (exception.Message.Contains("暂未启用", StringComparison.Ordinal))
    {
        rejected = true;
    }

    if (!rejected)
    {
        throw new InvalidOperationException("Wi-Fi 开发板输入入口没有被明确的临时闸门阻止。");
    }
}

static void CheckDeviceLogPolicy()
{
    BridgeOptions options = new();
    Require(!options.ShowDeviceLogInUi, "设备日志默认不得进入高频 UI 日志链路");
    Require(
        !SerialBridge.ShouldMirrorDeviceLog(false, "I (1) ble_output: test"),
        "关闭窗口镜像后不得转发设备 Info 日志");
    Require(
        SerialBridge.ShouldMirrorDeviceLog(true, "W (1) ble_output: test"),
        "显式开启窗口镜像后应转发 ESP-IDF 日志");
    Require(
        !SerialBridge.ShouldMirrorDeviceLog(true, "boot text"),
        "窗口镜像仅接收 ESP-IDF 分级日志");
    Require(
        SerialBridge.ShouldPersistDeviceLog(false, "W (1) ble_output: warning"),
        "精简模式必须保留警告");
    Require(
        SerialBridge.ShouldPersistDeviceLog(false, "I (1) ESP_HID_GAP: BLE connection parameters: interval=10000 us"),
        "精简模式必须保留最终连接参数");
    Require(
        !SerialBridge.ShouldPersistDeviceLog(false, "I (1) NIMBLE_HIDD: notify report=mouse"),
        "精简模式必须过滤高频普通通知日志");
    Require(
        SerialBridge.ShouldPersistDeviceLog(true, "I (1) NIMBLE_HIDD: notify report=mouse"),
        "完整模式必须保留全部设备日志");
}

static void CheckInputSuppressionPolicy()
{
    Require(!InputForwarder.ShouldSuppressKeyboard(false, false, false), "同步关闭时普通键盘输入不得拦截");
    Require(InputForwarder.ShouldSuppressKeyboard(true, false, false), "同步开启时普通键盘输入必须拦截");
    Require(!InputForwarder.ShouldSuppressKeyboard(true, true, false), "从关闭状态开启同步时，控制快捷键释放必须交还本机");
    Require(InputForwarder.ShouldSuppressKeyboard(false, true, true), "从开启状态关闭同步时，控制快捷键释放必须继续拦截");
    Require(!InputForwarder.ShouldSuppressMouse(false), "同步关闭时鼠标不得拦截");
    Require(InputForwarder.ShouldSuppressMouse(true), "同步开启时鼠标必须拦截");
    Require(
        InputForwarder.ShouldIgnoreKeyboardHookEvent(NativeMethods.LlkhfInjected, 0x41),
        "普通注入键必须继续忽略，避免自动化输入回环");
    Require(
        InputForwarder.ShouldIgnoreKeyboardHookEvent(NativeMethods.LlkhfInjected, 0x7B),
        "注入的 F12 必须继续按普通注入键忽略");
    for (uint virtualKey = 0x7C; virtualKey <= 0x87; virtualKey++)
    {
        int functionNumber = unchecked((int)(virtualKey - 0x70 + 1));
        Require(
            !InputForwarder.ShouldIgnoreKeyboardHookEvent(NativeMethods.LlkhfInjected, virtualKey),
            $"注入的 F{functionNumber} 不应在进入 Lua 前被过滤");
        Require(
            AutomationKeyMap.GetLuaEventArgument(virtualKey) as string == $"f{functionNumber}",
            $"F{functionNumber} 的 Lua 事件参数必须使用小写友好键名");
    }
}

static void CheckKeyboardAutoRepeatEdgeFiltering()
{
    RecordingTransport transport = new();
    using InputForwarder input = new(transport);
    ConcurrentQueue<PhysicalInputEvent> f13Events = new();
    input.PhysicalInputChanged += physicalInput =>
    {
        if (physicalInput.VirtualKey == 0x7C)
        {
            f13Events.Enqueue(physicalInput);
        }
    };

    input.Start();
    try
    {
        Require(input.EnqueueKeyboardInputForChecks(0x7C, false, true), "F13 首次 KeyDown 应成功入队");
        for (int index = 0; index < 2_048; index++)
        {
            Require(
                !input.EnqueueKeyboardInputForChecks(0x7C, false, true),
                "同一次 F13 按住期间的重复 KeyDown 必须被过滤");
        }
        Require(input.EnqueueKeyboardInputForChecks(0x7C, false, false), "F13 首次 KeyUp 应成功入队");
        Require(
            !input.EnqueueKeyboardInputForChecks(0x7C, false, false),
            "没有对应按下的重复 F13 KeyUp 必须被过滤");

        Require(
            SpinWait.SpinUntil(() => f13Events.Count == 2, 1000),
            $"F13 边沿事件未按时完成，实际数量={f13Events.Count}");
        PhysicalInputEvent[] actual = f13Events.ToArray();
        Require(actual.Length == 2, $"一次 F13 按住周期只能产生两个事件，实际={actual.Length}");
        Require(actual[0].Pressed && !actual[1].Pressed, "F13 事件必须严格为一次 pressed、一次 released");

        Require(input.EnqueueKeyboardInputForChecks(0x24, false, true), "HOME 首次 KeyDown 应成功入队");
        Require(
            !input.EnqueueKeyboardInputForChecks(0x24, false, true),
            "HOME 自动重复不得再次切换同步状态");
        Require(SpinWait.SpinUntil(() => input.ForwardingEnabled, 1000), "HOME 首次按下未开启同步");
        Thread.Sleep(20);
        Require(input.ForwardingEnabled, "HOME 自动重复导致同步状态被二次切换");
        Require(input.EnqueueKeyboardInputForChecks(0x24, false, false), "HOME KeyUp 应成功入队");
    }
    finally
    {
        input.Stop();
    }
    Console.WriteLine("键盘边沿检查：2,048 次 F13 重复 KeyDown 仅产生一次 pressed/一次 released，HOME 自动重复仅切换一次。");
}

static void CheckCursorLockGeometry()
{
    NativeMethods.ClipRect rect = MouseCursorLock.CalculateClipRect(new System.Drawing.Point(321, 654));
    Require(rect.Left == 321 && rect.Top == 654, "鼠标锁定矩形左上角不正确");
    Require(rect.Right == 322 && rect.Bottom == 655, "鼠标锁定矩形必须限制为一个像素");
}

static void CheckInputCaptureThreadIsolation()
{
    RecordingTransport transport = new();
    using InputForwarder input = new(transport);
    uint callerThreadId = NativeMethods.GetCurrentThreadId();
    input.Start();
    try
    {
        Require(input.CaptureThreadId != 0, "独立输入捕获线程未发布 Windows 线程 ID");
        Require(
            input.CaptureThreadId != callerThreadId,
            "键盘 Hook 和 Raw Input 不得继续依赖调用方/UI 消息线程");
    }
    finally
    {
        input.Stop();
    }
    Require(input.CaptureThreadId == 0, "输入捕获线程停止后未完成 Hook/Raw Input 清理");
    Console.WriteLine("输入线程隔离检查：键盘 Hook 与 Raw Input 使用独立消息线程，停止后已完成清理。");
}

static void CheckUnexpectedInputCaptureExitReleasesAll()
{
    RecordingTransport transport = new();
    using InputForwarder input = new(transport);
    input.Start();
    input.SetForwardingEnabled(true);
    int releaseCountBeforeExit = transport.FrameCount(MessageType.ReleaseAll);
    uint captureThreadId = input.CaptureThreadId;
    Require(captureThreadId != 0, "故障保护检查未取得输入捕获线程 ID");
    Require(
        NativeMethods.PostThreadMessage(captureThreadId, NativeMethods.WmQuit, IntPtr.Zero, IntPtr.Zero),
        "无法模拟输入捕获消息循环意外退出");

    DateTime deadline = DateTime.UtcNow.AddSeconds(2);
    while (input.CaptureThreadId != 0 && DateTime.UtcNow < deadline)
    {
        Thread.Sleep(10);
    }

    Require(input.CaptureThreadId == 0, "模拟故障后输入捕获线程未退出");
    Require(!input.ForwardingEnabled, "输入捕获线程意外退出后仍保持转发状态");
    Require(
        transport.FrameCount(MessageType.ReleaseAll) > releaseCountBeforeExit,
        "输入捕获线程意外退出时未强制发送 ReleaseAll");
    Console.WriteLine("输入失控保护检查：捕获线程意外退出后已禁用转发并发送 ReleaseAll。");
}

static void CheckForwardingNotificationFailureStillReleasesAll()
{
    RecordingTransport transport = new();
    using InputForwarder input = new(transport);
    input.SetForwardingEnabled(true);
    input.ForwardingTransitioning += () => throw new InvalidOperationException("故意模拟自动化停止失败");
    int releaseCountBeforeStop = transport.FrameCount(MessageType.ReleaseAll);

    input.DisableForwarding();

    Require(!input.ForwardingEnabled, "转发状态通知异常后仍保持转发状态");
    Require(
        transport.FrameCount(MessageType.ReleaseAll) > releaseCountBeforeStop,
        "转发状态通知异常阻止了 ReleaseAll");
    Console.WriteLine("ReleaseAll 顺序检查：外部状态事件异常不能阻止安全释放。");
}

static void CheckInputCallbackFailureStillReleasesAll()
{
    RecordingTransport transport = new();
    using InputForwarder input = new(transport);
    input.SetForwardingEnabled(true);
    input.PhysicalInputChanged += _ => throw new InvalidOperationException("故意模拟 Lua/宏输入回调失败");
    int releaseCountBeforeInput = transport.FrameCount(MessageType.ReleaseAll);

    input.ProcessRawMouseInputForChecks(new NativeMethods.RawMouse
    {
        Buttons = NativeMethods.RawMouseLeftButtonDown,
    });

    Require(!input.ForwardingEnabled, "Lua/宏输入回调异常后仍保持转发状态");
    Require(
        transport.FrameCount(MessageType.ReleaseAll) > releaseCountBeforeInput,
        "Lua/宏输入回调异常后未强制发送 ReleaseAll");
    Console.WriteLine("输入回调故障检查：Lua/宏回调异常后已禁用转发并发送 ReleaseAll。");
}

static void CheckLuaReleaseCannotBlockInputCapture()
{
    RecordingTransport transport = new();
    using InputForwarder input = new(transport);
    using ManualResetEventSlim releaseHandlerEntered = new(false);
    using ManualResetEventSlim allowReleaseHandler = new(false);
    input.PhysicalInputChanged += physicalInput =>
    {
        if (!physicalInput.Pressed && physicalInput.VirtualKey == 0x01)
        {
            releaseHandlerEntered.Set();
            allowReleaseHandler.Wait(TimeSpan.FromSeconds(2));
        }
    };
    input.Start();
    try
    {
        input.SetForwardingEnabled(true);
        Require(
            input.EnqueueRawMouseInputForChecks(new NativeMethods.RawMouse
            {
                Buttons = NativeMethods.RawMouseLeftButtonDown,
            }),
            "无法排入实体左键按下事件");
        Require(
            input.EnqueueRawMouseInputForChecks(new NativeMethods.RawMouse
            {
                Buttons = NativeMethods.RawMouseLeftButtonUp,
            }),
            "无法排入实体左键松开事件");
        Require(releaseHandlerEntered.Wait(TimeSpan.FromSeconds(1)), "左键松开事件未进入 Lua/宏分发");
        Require(
            SpinWait.SpinUntil(() => transport.MouseReports().Length >= 2, 1000),
            "Lua released 回调阻塞期间未送出实体左键按下/松开报告");
        MouseReport[] reportsBeforeUnblock = transport.MouseReports();
        Require(
            reportsBeforeUnblock.Any(report => (report.Buttons & 0x01) != 0),
            "实体左键按下报告未在 Lua 回调前送出");
        Require(
            reportsBeforeUnblock[^1].Buttons == 0,
            "Lua released 回调阻塞时，实体左键松开报告尚未优先送出");

        System.Diagnostics.Stopwatch stopwatch = System.Diagnostics.Stopwatch.StartNew();
        bool accepted = input.EnqueueKeyboardInputForChecks(0x41, false, true);
        stopwatch.Stop();
        Require(accepted, "Lua 松开回调阻塞时输入捕获队列拒绝新事件");
        Require(
            stopwatch.ElapsedMilliseconds < 50,
            $"Lua 松开回调反向阻塞输入捕获：{stopwatch.ElapsedMilliseconds} ms");
    }
    finally
    {
        allowReleaseHandler.Set();
        input.Stop();
    }
    Console.WriteLine("Lua 松开连点隔离检查：阻塞 released 回调时实体左键松开已优先送出，且不会阻塞输入捕获入队。");
}

static void CheckUiLogWriter()
{
    string directory = Path.Combine(Path.GetTempPath(), $"hidbridge-log-{Guid.NewGuid():N}");
    string pathTemplate = Path.Combine(directory, "runtime-{timestamp}.log");
    try
    {
        using UiLogTextWriter writer = new();
        writer.EnableFile(pathTemplate);
        ConcurrentQueue<string> actual = new();
        writer.WriteLine("启动前日志");
        writer.Flush();
        writer.Attach(actual.Enqueue);
        writer.Write("运行中");
        writer.WriteLine("日志");
        writer.Flush();
        Require(actual.SequenceEqual(["启动前日志", "运行中日志"]), "UI 日志缓存或按行输出不符合预期");
        Require(writer.FilePath is not null && File.Exists(writer.FilePath), "实时日志文件未创建");
        string persisted;
        using (FileStream stream = new(writer.FilePath!, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
        using (StreamReader reader = new(stream, Encoding.UTF8))
        {
            persisted = reader.ReadToEnd();
        }
        Require(persisted.Contains("启动前日志", StringComparison.Ordinal), "启动前日志未实时写入本地文件");
        Require(persisted.Contains("运行中日志", StringComparison.Ordinal), "运行中日志未实时写入本地文件");

        using ManualResetEventSlim sinkEntered = new(false);
        using ManualResetEventSlim releaseSink = new(false);
        writer.Attach(_ =>
        {
            sinkEntered.Set();
            releaseSink.Wait(TimeSpan.FromSeconds(2));
        });
        System.Diagnostics.Stopwatch stopwatch = System.Diagnostics.Stopwatch.StartNew();
        writer.WriteLine("阻塞接收器隔离检查");
        stopwatch.Stop();
        Require(sinkEntered.Wait(TimeSpan.FromSeconds(1)), "异步日志线程未进入阻塞接收器");
        Require(stopwatch.ElapsedMilliseconds < 50, $"日志调用方被 UI/文件输出阻塞：{stopwatch.ElapsedMilliseconds} ms");
        releaseSink.Set();
        writer.Flush();
    }
    finally
    {
        if (Directory.Exists(directory))
        {
            Directory.Delete(directory, recursive: true);
        }
    }
}

static void CheckRemoteInputUdpPath()
{
    Require(new BridgeOptions().RemoteInputEnabled, "UDP 模拟输入必须默认开启");
    BridgeOptions.Validate(new BridgeOptions { RemoteInputEnabled = true });
    RecordingTransport transport = new();
    using InputForwarder input = new(transport);
    input.SetForwardingEnabled(true);
    using RemoteInputServer server = new(
        "127.0.0.1",
        0,
        command => input.TryInjectMouseMovement(
            command.DeltaX,
            command.DeltaY,
            command.Wheel,
            command.Pan));
    server.Start();

    using UdpClient client = new();
    byte[] valid = Encoding.UTF8.GetBytes(
        "{\"dx\":37,\"dy\":-19,\"wheel\":2}");
    client.Send(valid, valid.Length, "127.0.0.1", server.Port);

    DateTime deadline = DateTime.UtcNow.AddSeconds(2);
    while (DateTime.UtcNow < deadline)
    {
        MouseReport[] reports = transport.MouseReports();
        if (reports.Sum(report => (long)report.X) == 37 &&
            reports.Sum(report => (long)report.Y) == -19 &&
            reports.Sum(report => (long)report.Wheel) == 2)
        {
            break;
        }
        Thread.Sleep(5);
    }

    MouseReport[] actual = transport.MouseReports();
    Require(actual.Sum(report => (long)report.X) == 37, "UDP 模拟输入 X 位移未完整进入鼠标报告链路");
    Require(actual.Sum(report => (long)report.Y) == -19, "UDP 模拟输入 Y 位移未完整进入鼠标报告链路");
    Require(actual.Sum(report => (long)report.Wheel) == 2, "UDP 模拟输入滚轮增量未完整进入鼠标报告链路");

    int reportCount = actual.Length;
    byte[] oversized = Encoding.UTF8.GetBytes(
        "{\"dx\":999999,\"dy\":999}");
    client.Send(oversized, oversized.Length, "127.0.0.1", server.Port);
    Thread.Sleep(50);
    Require(transport.MouseReports().Length == reportCount, "超范围的 UDP 模拟输入不得进入鼠标报告链路");

    input.SetForwardingEnabled(false);
    byte[] disabled = Encoding.UTF8.GetBytes(
        "{\"dx\":5,\"dy\":5}");
    client.Send(disabled, disabled.Length, "127.0.0.1", server.Port);
    Thread.Sleep(50);
    Require(transport.MouseReports().Length == reportCount, "同步关闭后 UDP 模拟输入不得继续发送鼠标报告");
}

static void CheckFirmwareUpdateApiPolicy()
{
    Require(!new AutomationSettings().FirmwareUpdateApiEnabled, "本机固件刷写接口必须默认关闭");
    BridgeOptions options = new();
    Require(options.FirmwareUpdateApiPort == 24815, "本机固件刷写接口默认端口应为 24815");
    BridgeOptions.Validate(options);
    FirmwareFlashPlan plan = FirmwareFlashPlan.Load(options);
    Require(plan.Images.Count == 3, "固件刷写计划必须包含三段镜像");
    Require(
        plan.Images.Select(image => image.Offset).SequenceEqual([0L, 0x8000L, 0x10000L]),
        "固件刷写计划偏移必须为 0x0、0x8000、0x10000");
    Require(plan.Images.All(image => File.Exists(image.Path) && image.Sha256.Length == 64), "固件镜像路径或 SHA-256 无效");
    Require(File.Exists(plan.EsptoolPath), "固件刷写计划未定位到项目内 esptool.exe");
    Require(plan.Before == "default-reset" && plan.After == "hard-reset", "esptool 5 复位参数未规范化为连字符形式");
    string summary = FirmwareFlashService.FormatVerificationSummary(plan.Images, 3, hardReset: true);
    Require(plan.Images.All(image => summary.Contains($"{image.OffsetArgument}={image.Sha256}", StringComparison.Ordinal)), "刷写最终摘要缺少三段 SHA-256");
    Require(summary.Contains("设备校验=3/3", StringComparison.Ordinal) && summary.EndsWith("RTS复位=完成", StringComparison.Ordinal), "刷写最终摘要缺少校验或复位结果");

    string validRequest =
        "POST /api/v1/firmware/flash HTTP/1.1\r\n" +
        "Host: 127.0.0.1:24815\r\n" +
        $"{FirmwareUpdateApiServer.ConfirmationHeaderName}: {FirmwareUpdateApiServer.ConfirmationHeaderValue}\r\n" +
        "Content-Length: 0\r\n\r\n";
    Require(
        FirmwareUpdateApiServer.TryParseRequest(
            validRequest,
            out string method,
            out string path,
            out Dictionary<string, string> headers),
        "合法固件刷写 HTTP 请求应能解析");
    Require(method == "POST" && path == "/api/v1/firmware/flash", "固件刷写请求方法或路径解析错误");
    Require(
        headers.TryGetValue(FirmwareUpdateApiServer.ConfirmationHeaderName, out string? confirmation) &&
        confirmation == FirmwareUpdateApiServer.ConfirmationHeaderValue,
        "固件刷写确认请求头解析错误");
    Require(
        !FirmwareUpdateApiServer.TryParseRequest(
            "DELETE /api/v1/firmware/flash HTTP/1.1\r\nHost: localhost\r\n\r\n",
            out _,
            out _,
            out _),
        "固件刷写接口必须拒绝 GET/POST 以外的方法");
}

static void CheckFirmwareUpdateApiLoopback()
{
    int started = 0;
    FirmwareFlashSnapshot snapshot = new(
        "loopback-check",
        "idle",
        "test",
        null,
        null,
        null,
        null);
    bool TryStart(out FirmwareFlashSnapshot current)
    {
        Interlocked.Increment(ref started);
        current = snapshot with { State = "running" };
        return true;
    }

    using FirmwareUpdateApiServer server = new(0, () => snapshot, TryStart);
    Require(server.ListenAddress.Equals(System.Net.IPAddress.Loopback), "固件刷写 API 必须固定绑定 IPv4 Loopback");
    server.SetEnabled(true);
    Require(server.Enabled && server.Port > 0, "固件刷写 API 未在临时 Loopback 端口启动");
    using HttpClient client = new() { BaseAddress = new Uri($"http://127.0.0.1:{server.Port}") };

    HttpResponseMessage statusResponse = client.GetAsync("/api/v1/firmware/status").GetAwaiter().GetResult();
    Require(statusResponse.StatusCode == System.Net.HttpStatusCode.OK, "固件刷写状态接口未返回 200");
    string statusJson = statusResponse.Content.ReadAsStringAsync().GetAwaiter().GetResult();
    Require(statusJson.Contains("\"state\":\"idle\"", StringComparison.Ordinal), "固件刷写状态响应内容不正确");

    HttpResponseMessage rejected = client.PostAsync("/api/v1/firmware/flash", null).GetAwaiter().GetResult();
    Require(rejected.StatusCode == System.Net.HttpStatusCode.Forbidden, "缺少确认头的刷写请求必须返回 403");
    Require(started == 0, "缺少确认头时不得调用刷写任务");

    using HttpRequestMessage request = new(HttpMethod.Post, "/api/v1/firmware/flash");
    request.Headers.Add(FirmwareUpdateApiServer.ConfirmationHeaderName, FirmwareUpdateApiServer.ConfirmationHeaderValue);
    HttpResponseMessage accepted = client.Send(request);
    Require(accepted.StatusCode == System.Net.HttpStatusCode.Accepted, "合法刷写请求未返回 202");
    Require(started == 1, "合法刷写请求必须且只能启动一次任务");
    server.SetEnabled(false);
    Require(!server.Enabled, "设置关闭后固件刷写 API 必须停止监听");
}

static void CheckAutomationProfilesAndRuntime()
{
    string root = Path.Combine(Path.GetTempPath(), $"hidbridge-automation-{Guid.NewGuid():N}");
    string legacy = Path.Combine(root, "legacy");
    string legacyGlobal = Path.Combine(legacy, "Global");
    string current = Path.Combine(root, "current");
    Directory.CreateDirectory(legacyGlobal);
    try
    {
        File.WriteAllText(
            Path.Combine(legacyGlobal, "profile.json"),
            """
            {
              "apps": ["ignored.exe"],
              "lua_script_text": "function OnEvent(event, arg) if event == 'pressed' then move(7, -3) end end",
              "macros": {
                "侧键测试": { "trigger": "mouse_side1", "mode": "once", "enabled": true }
              },
              "device_type": "kmboxNet",
              "screen_find_color": "#FFFFFF"
            }
            """);
        File.WriteAllText(Path.Combine(legacyGlobal, "侧键测试.txt"), "move(10,-4)\nmouse(1,1)\nmouse(1,0)");

        AutomationProfileStore store = new(current);
        Require(store.TryImportLegacyMouseHubProfiles(legacy), "首次运行应导入 Mouse hub 的宏和 Lua 配置");
        Require(!store.TryImportLegacyMouseHubProfiles(legacy), "旧配置导入必须只执行一次");
        AutomationProfile profile = store.LoadProfile("Global");
        Require(profile.LuaScriptText.Contains("OnEvent", StringComparison.Ordinal), "旧配置 Lua 文本未导入");
        Require(profile.Macros.TryGetValue("侧键测试", out MacroDefinition? importedMacro), "旧配置宏元数据未导入");
        Require(importedMacro!.Trigger == "mouse_side1" && importedMacro.Text.Contains("move(10,-4)", StringComparison.Ordinal), "旧宏触发键或正文未完整导入");
        AutomationSettings behaviorSettings = store.LoadSettings();
        behaviorSettings.MinimizeToTray = false;
        behaviorSettings.CloseToTray = false;
        behaviorSettings.GenerateMovementAnalysisImage = false;
        store.SaveSettings(behaviorSettings);
        AutomationSettings reloadedBehaviorSettings = store.LoadSettings();
        Require(!reloadedBehaviorSettings.MinimizeToTray && !reloadedBehaviorSettings.CloseToTray && !reloadedBehaviorSettings.GenerateMovementAnalysisImage, "托盘与分析图片设置未持久化");

        ParsedMacro staged = MacroParser.Parse(
            "[on_press]\nmouse(1,1)\n[while_hold]\nmove(1,2)\n[on_release]\nmouse(1,0)",
            MacroRunModes.Staged);
        Require(staged.Errors.Count == 0, "staged 宏解析不应报错");
        Require(staged.OnPress.Count == 1 && staged.WhileHold.Count == 1 && staged.OnRelease.Count == 1, "staged 宏分段结果不正确");
        Require(MacroParser.Parse("keypress(home)\nkeypress(end,20)", MacroRunModes.Once).Errors.Count == 0, "HOME/END 不得被宏按键语法禁止");
        _ = HotkeyDefinition.Parse("home");
        _ = HotkeyDefinition.Parse("end");

        RecordingAutomationOutput macroOutput = new();
        ParsedMacro once = MacroParser.Parse(importedMacro.Text, MacroRunModes.Once);
        using (MacroJob job = new("侧键测试", MacroRunModes.Once, once, macroOutput, _ => { }))
        {
            job.OnPressed();
            Require(SpinWait.SpinUntil(() => macroOutput.Actions.Length >= 3, 1000), "宏未在超时前执行完动作");
        }
        Require(macroOutput.Actions.Contains("move:10,-4"), "宏 move 输出与配置不一致");
        Require(macroOutput.Actions.ContainsSequence("mouse:1:down", "mouse:1:up"), "宏鼠标按下/松开顺序不正确");

        RecordingAutomationOutput luaOutput = new();
        List<string> luaLogs = [];
        using LuaScriptRunner lua = new(luaOutput, luaLogs.Add, _ => { }, () => luaLogs.Clear());
        string luaText = """
            function OnEvent(event, arg)
                if event == "pressed" and arg == 4 and IsPressed(4) then
                    move(7, -3)
                    mouse(1, 1)
                    mouse(1, 0)
                    keypress("a")
                    randdelay(0)
                    DebugLog("arg=%d", arg)
                end
            end
            """;
        (bool luaValid, string luaMessage) = lua.Check(luaText);
        Require(luaValid, $"Lua 语法检查失败：{luaMessage}");
        lua.Start(luaText);
        lua.HandlePhysicalInput(new PhysicalInputEvent(new HashSet<uint> { 0x05 }, 0x05, true));
        Require(SpinWait.SpinUntil(() => luaOutput.Actions.Length >= 5, 1000), "Lua OnEvent 未在超时前执行动作");
        lua.Stop();
        Require(luaOutput.Actions.Contains("move:7,-3"), "Lua move 输出不正确");
        Require(luaOutput.Actions.ContainsSequence("key:4:down", "key:4:up"), "Lua keypress 省略时长参数时输出不正确");
        Require(luaLogs.Any(line => line.Contains("arg=4", StringComparison.Ordinal)), "Lua DebugLog 实际输出不正确");
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, true);
        }
    }
}

static void CheckAutomationRemoteOutput()
{
    RecordingTransport transport = new();
    using InputForwarder input = new(transport);
    input.SetForwardingEnabled(true);
    input.SetAutomationMouseButton(4, true);
    input.SendAutomationMouseMove(17, -9);
    input.SendAutomationWheel(2);
    input.SetAutomationKey(4, true);
    input.SetAutomationKey(4, false);
    input.SetAutomationMouseButton(4, false);
    Require(SpinWait.SpinUntil(() => transport.MouseReports().Length >= 2, 1000), "自动化对端鼠标报告未在超时前发送");

    MouseReport[] mouseReports = transport.MouseReports();
    Require(mouseReports.Sum(report => report.X) == 17, "自动化远端 X 位移不守恒");
    Require(mouseReports.Sum(report => report.Y) == -9, "自动化远端 Y 位移不守恒");
    Require(mouseReports.Sum(report => report.Wheel) == 2, "自动化远端滚轮输出不正确");
    Require(mouseReports.Any(report => (report.Buttons & 0x08) != 0), "自动化远端侧键按下未发送");
    Require(mouseReports.Last().Buttons == 0, "自动化远端侧键松开未发送");
    byte[][] keyboardReports = transport.KeyboardReports();
    Require(keyboardReports.Any(report => report.Skip(2).Contains((byte)4)), "自动化远端键盘按下未发送");
    Require(keyboardReports.Last().All(value => value == 0), "自动化远端键盘松开未发送");
    input.SetForwardingEnabled(false);
}

static void CheckLocalMouseTriggersReachLua()
{
    string directory = Path.Combine(Path.GetTempPath(), $"hidbridge-local-lua-{Guid.NewGuid():N}");
    try
    {
        AutomationProfileStore store = new(directory);
        AutomationProfile profile = store.LoadProfile(AutomationProfileStore.GlobalProfile);
        profile.LuaScriptText =
            "function OnEvent(event, arg) DebugLog(\"event=%s arg=%s\", event, tostring(arg)) end";
        store.SaveProfile(profile);

        RecordingTransport transport = new();
        using InputForwarder input = new(transport);
        using AutomationController automation = new(store, input);
        ConcurrentQueue<string> logs = new();
        ConcurrentQueue<string> diagnosticLogs = new();
        automation.Log += logs.Enqueue;
        automation.DiagnosticLog += diagnosticLogs.Enqueue;
        automation.Start();

        Require(!input.ForwardingEnabled, "本机 Lua 触发检查必须在捕获关闭状态运行");
        input.ProcessRawMouseInputForChecks(new NativeMethods.RawMouse
        {
            Buttons = NativeMethods.RawMouseButton4Down,
        });
        input.ProcessRawMouseInputForChecks(new NativeMethods.RawMouse
        {
            Buttons = NativeMethods.RawMouseButton4Up,
        });

        Require(SpinWait.SpinUntil(
            () => logs.Any(line => line.Contains("event=pressed arg=4", StringComparison.Ordinal)) &&
                  logs.Any(line => line.Contains("event=released arg=4", StringComparison.Ordinal)),
            1000), "捕获关闭时实体鼠标按键未送达 Lua OnEvent/DebugLog");
        Require(
            diagnosticLogs.Any(line => line.StartsWith("[LuaEvent]", StringComparison.Ordinal)),
            "Lua 事件详细诊断未进入独立日志通道");
        Require(
            !logs.Any(line => line.StartsWith("[LuaEvent]", StringComparison.Ordinal) ||
                              line.StartsWith("[LuaOutput]", StringComparison.Ordinal)),
            "Lua UI 简略日志通道不得包含详细事件或输出诊断");
        Require(transport.MouseReports().Length == 0, "本机 Lua 触发不得向对端发送实体鼠标报告");
        Console.WriteLine(
            "本机 Lua 触发检查：捕获关闭；实际日志包含 event=pressed arg=4 和 event=released arg=4；对端鼠标报告=0。");
    }
    finally
    {
        if (Directory.Exists(directory))
        {
            Directory.Delete(directory, true);
        }
    }
}

static void CheckTriggerForwardingIntegration()
{
    string directory = Path.Combine(Path.GetTempPath(), $"hidbridge-trigger-{Guid.NewGuid():N}");
    try
    {
        AutomationProfileStore store = new(directory);
        AutomationProfile profile = store.LoadProfile(AutomationProfileStore.GlobalProfile);
        profile.Macros["侧键透传"] = new MacroDefinition
        {
            Name = "侧键透传",
            Trigger = "mouse_side1",
            Mode = MacroRunModes.Once,
            Enabled = true,
            Text = "move(23,-11)",
        };
        store.SaveProfile(profile);

        RecordingTransport transport = new();
        using InputForwarder input = new(transport);
        using AutomationController automation = new(store, input);
        automation.Start();
        long localReleaseCountBeforeTransitions = automation.LocalReleaseAllCount;
        input.SetForwardingEnabled(true);
        Require(
            automation.LocalReleaseAllCount == localReleaseCountBeforeTransitions + 1,
            "进入捕获模式时必须向主控端发送一次 Win32 ReleaseAll");
        input.ProcessRawMouseInputForChecks(new NativeMethods.RawMouse
        {
            Buttons = NativeMethods.RawMouseButton4Down,
        });
        input.ProcessRawMouseInputForChecks(new NativeMethods.RawMouse
        {
            Buttons = NativeMethods.RawMouseButton4Up,
        });

        Require(SpinWait.SpinUntil(() =>
        {
            MouseReport[] current = transport.MouseReports();
            int pressedIndex = Array.FindIndex(current, report => (report.Buttons & 0x08) != 0);
            int releasedIndex = pressedIndex < 0
                ? -1
                : Array.FindIndex(current, pressedIndex + 1, report => report.Buttons == 0);
            return current.Sum(report => report.X) == 23 && releasedIndex > pressedIndex;
        }, 1000), "未在超时前同时观察到侧键按下、宏位移和侧键松开报告");
        MouseReport[] reports = transport.MouseReports();
        Require(reports.Any(report => (report.Buttons & 0x08) != 0), "宏触发侧键本身未透传到对端");
        Require(reports.Last().Buttons == 0, "宏触发侧键松开未透传到对端");
        Require(reports.Sum(report => report.X) == 23 && reports.Sum(report => report.Y) == -11, "侧键触发宏的对端位移不正确");
        input.SetForwardingEnabled(false);
        Require(
            automation.LocalReleaseAllCount == localReleaseCountBeforeTransitions + 2,
            "退出捕获模式时必须再次向主控端发送一次 Win32 ReleaseAll");
    }
    finally
    {
        if (Directory.Exists(directory))
        {
            Directory.Delete(directory, true);
        }
    }
}

static void CheckWindowLayout()
{
    Exception? failure = null;
    Thread thread = new(() =>
    {
        try
        {
            string automationDirectory = Path.Combine(Path.GetTempPath(), $"hidbridge-ui-{Guid.NewGuid():N}");
            RecordingTransport transport = new();
            using InputForwarder input = new(transport);
            AutomationProfileStore store = new(automationDirectory);
            using AutomationController automation = new(store, input);
            using BridgeMainForm form = new(input, automation, "测试端点");
            _ = form.Handle;
            form.PerformLayout();
            double upperRatio = form.MainSplit.SplitterDistance / (double)form.MainSplit.ClientSize.Height;
            Require(upperRatio is >= 0.45 and <= 0.55, "窗口上半区必须约占客户区一半");
            Require(form.MainTabs.TabPages.Count == 4, "主窗口必须包含鼠标捕获、宏、Lua、设置四个功能页");
            Require(form.MainTabs.TabPages.Cast<TabPage>().Select(page => page.Text).SequenceEqual(["鼠标捕获", "宏", "Lua", "设置"]), "四个功能页顺序或名称不正确");
            Require(form.MacroPage.ProfileComboBox.Items.Count >= 1, "宏页未加载手动配置列表");
            Require(form.LuaPage.ProfileComboBox.Items.Count >= 1, "Lua 页未加载手动配置列表");
            Require(form.LuaPage.AddProfileButton.Text == "新增配置" && form.LuaPage.DeleteProfileButton.Text == "删除配置", "Lua 页缺少新增/删除配置按钮");
            Require(
                form.LuaPage.Editor.Multiline &&
                form.LuaPage.Editor.ScrollBars == ScrollBars.Both &&
                !form.LuaPage.Editor.WordWrap,
                "Lua 编辑器必须保留多行文本和横向滚动能力");
            Require(
                LuaPageControl.NormalizeEditorNewlines("第一行\n第二行\r第三行\r\n第四行") ==
                $"第一行{Environment.NewLine}第二行{Environment.NewLine}第三行{Environment.NewLine}第四行",
                "Lua 编辑器粘贴前必须把不同换行格式统一为 Windows 多行文本");
            string[] macroPageOptions = EnumerateControls(form.MacroPage).Select(control => control.Text).ToArray();
            Require(!macroPageOptions.Contains("开机启动") && !macroPageOptions.Contains("最小化到托盘") && !macroPageOptions.Contains("关闭到托盘"), "程序行为设置不得继续显示在宏页");
            Require(form.SettingsPage.StartOnBootCheckBox.Text.StartsWith("开机启动", StringComparison.Ordinal), "设置页缺少开机启动选项");
            Require(form.SettingsPage.MinimizeToTrayCheckBox.Checked == automation.Settings.MinimizeToTray, "设置页最小化到托盘状态未从配置加载");
            Require(form.SettingsPage.CloseToTrayCheckBox.Checked == automation.Settings.CloseToTray, "设置页关闭到托盘状态未从配置加载");
            Require(form.SettingsPage.GenerateMovementAnalysisImageCheckBox.Checked == automation.Settings.GenerateMovementAnalysisImage, "设置页分析图片开关状态未从配置加载");
            Require(!form.SettingsPage.FirmwareUpdateApiCheckBox.Checked, "本机固件刷写接口默认必须关闭");
            Require(!form.SettingsPage.FirmwareUpdateApiCheckBox.Enabled, "无串口刷写服务时设置页接口开关必须禁用");
            automation.Settings.GenerateMovementAnalysisImage = false;
            form.ProcessMovementRecordingForChecks(new MouseMovementRecording(
                DateTime.UtcNow.AddSeconds(-1), DateTime.UtcNow, [3], [-2]));
            Require(form.OwnedForms.Length == 0, "关闭分析图片后不得打开分析窗口");
            Require(form.LogTextBox.Text.Contains("不生成按键情况分析图片", StringComparison.Ordinal), "关闭分析图片后未输出跳过提示");
            _ = automation.CreateProfile("UI同步检查");
            automation.SetActiveProfile("UI同步检查");
            Require(form.MacroPage.ProfileComboBox.Items.Contains("UI同步检查") && form.LuaPage.ProfileComboBox.Items.Contains("UI同步检查"), "Lua 新增配置后宏/Lua 页列表未同步刷新");
            automation.DeleteProfile("UI同步检查");
            Require(form.MacroPage.ProfileComboBox.SelectedItem as string == AutomationProfileStore.GlobalProfile && form.LuaPage.ProfileComboBox.SelectedItem as string == AutomationProfileStore.GlobalProfile, "删除活动配置后宏/Lua 页未同步回 Global");
            Require(form.LogTextBox.Multiline && form.LogTextBox.ReadOnly, "日志栏必须为只读多行文本框");
            Require(!form.LogTextBox.WordWrap && form.LogTextBox.ScrollBars == ScrollBars.Both, "日志栏必须支持完整选择和双向滚动");
            Require(form.LogModeComboBox.DropDownStyle == ComboBoxStyle.DropDownList, "日志模式必须使用不可编辑下拉框");
            Require(form.LogModeComboBox.Items.Count == 2, "日志模式下拉框必须提供精简和完整两档");
            Require(form.LogModeComboBox.SelectedIndex == 0, "日志模式默认必须为精简高性能");
            form.LogModeComboBox.SelectedIndex = 1;
            Require(form.LogModeComboBox.SelectedIndex == 1, "日志模式必须能切换到完整诊断");
            form.LogModeComboBox.SelectedIndex = 0;
            Require(!form.SimulatedUdpCheckBox.Checked, "模拟 UDP 开关默认必须关闭");
            Require(!input.SimulatedUdpInputEnabled, "界面创建后模拟 UDP 状态应默认关闭");
            Require(!form.SimulatedUdpFrequencyComboBox.Enabled, "模拟 UDP 关闭时频率列表应禁用");
            Require(
                form.SimulatedUdpFrequencyComboBox.Items.Cast<int>().SequenceEqual([30, 60, 100, 140, 200, 500, 0]),
                "模拟 UDP 界面频率列表不正确");
            Require(
                form.SimulatedUdpFrequencyComboBox.SelectedItem is 100,
                "模拟 UDP 默认频率必须为 100 Hz");
            Require(form.UdpSmoothingCheckBox.Checked, "UDP 平滑开关默认必须开启");
            Require(input.UdpSmoothingEnabled, "界面创建后 UDP 平滑状态应默认开启");
            form.UdpSmoothingCheckBox.Checked = false;
            Require(!input.UdpSmoothingEnabled, "界面开关未关闭 UDP 平滑");
            form.UdpSmoothingCheckBox.Checked = true;
            Require(input.UdpSmoothingEnabled, "界面开关未重新开启 UDP 平滑");
            input.SetForwardingEnabled(true);
            Require(!form.UdpSmoothingCheckBox.Enabled, "同步开启时 UDP 平滑开关必须禁用");
            input.SetForwardingEnabled(false);
            Require(form.UdpSmoothingCheckBox.Enabled, "同步关闭后 UDP 平滑开关必须恢复可用");
            form.SimulatedUdpCheckBox.Checked = true;
            Require(input.SimulatedUdpInputEnabled, "界面开关未启用模拟 UDP 输入");
            Require(form.SimulatedUdpFrequencyComboBox.Enabled, "模拟 UDP 开启时频率列表未启用");
            form.SimulatedUdpFrequencyComboBox.SelectedItem = 140;
            Require(input.SimulatedUdpInputFrequencyHz == 140, "界面频率列表未切换到 140 Hz");
            form.SimulatedUdpFrequencyComboBox.SelectedItem = 500;
            Require(input.SimulatedUdpInputFrequencyHz == 500, "界面频率列表未切换到 500 Hz");
            form.SimulatedUdpFrequencyComboBox.SelectedItem = 0;
            Require(input.SimulatedUdpInputFrequencyHz == 0, "界面频率列表未切换到无上限");
            Require(
                form.SimulatedUdpFrequencyComboBox.GetItemText(
                    form.SimulatedUdpFrequencyComboBox.SelectedItem) == "无上限",
                "模拟 UDP 无上限选项显示文案不正确");
            form.SimulatedUdpCheckBox.Checked = false;
            Require(!input.SimulatedUdpInputEnabled, "界面开关未关闭模拟 UDP 输入");
            form.AppendLog("可复制日志检查");
            Require(form.LogTextBox.Text.Contains("可复制日志检查", StringComparison.Ordinal), "日志内容未实际写入窗口文本框");
            for (int index = 0; index < 160; index++)
            {
                form.AppendLog($"历史日志 {index}");
            }

            // 模拟用户向上滚动到首行，验证 AppendText 后原生 TextBox 的首行仍保持不变。
            form.LogTextBox.SelectionStart = 0;
            form.LogTextBox.SelectionLength = 0;
            form.LogTextBox.ScrollToCaret();
            int firstVisibleBeforeAppend = GetFirstVisibleLine(form.LogTextBox);
            Require(firstVisibleBeforeAppend <= 1, "测试未能把日志栏滚动到历史顶部");
            form.AppendLog("最新日志到达");
            int firstVisibleAfterAppend = GetFirstVisibleLine(form.LogTextBox);
            Require(
                firstVisibleAfterAppend == firstVisibleBeforeAppend,
                $"查看历史日志时首行发生跳变：{firstVisibleBeforeAppend}->{firstVisibleAfterAppend}");

            form.LogTextBox.SelectionStart = 0;
            form.LogTextBox.SelectionLength = Math.Min(18, form.LogTextBox.TextLength);
            string selectedBeforeAppend = form.LogTextBox.SelectedText;
            form.AppendLog("最新日志到达");
            Require(
                form.LogTextBox.SelectedText == selectedBeforeAppend,
                "查看或复制旧日志时，新日志不得抢走当前选区");
            string allText = string.Join("\n", EnumerateControls(form).Select(control => control.Text));
            Require(allText.Contains("HOME", StringComparison.Ordinal), "界面未显示同步快捷键");
            Require(allText.Contains("END", StringComparison.Ordinal), "界面未显示结束快捷键");
            Require(allText.Contains("模拟 UDP", StringComparison.Ordinal), "界面未显示模拟 UDP 开关");
            Require(allText.Contains("UDP 平滑", StringComparison.Ordinal), "界面未显示 UDP 平滑开关");
            Require(allText.Contains("左右键同按", StringComparison.Ordinal), "界面未提示鼠标移动记录触发方式");
            Require(allText.Contains("本机固件刷写接口", StringComparison.Ordinal), "设置页未显示本机固件刷写接口开关");
            Require(allText.Contains("暂停自动跟随", StringComparison.Ordinal), "日志栏未提示滚动到上方后暂停自动跟随");
            CheckDarkSurfaceTextContrast(form);
            string artifactDirectory = Path.Combine(Environment.CurrentDirectory, "artifacts");
            Directory.CreateDirectory(artifactDirectory);
            form.StartPosition = FormStartPosition.Manual;
            form.Location = new Point(-32000, -32000);
            form.ShowInTaskbar = false;
            form.Show();
            Application.DoEvents();
            form.MainTabs.SelectedIndex = 2;
            RenderControlToPng(form, Path.Combine(artifactDirectory, "automation-ui-lua-settings-update.png"));
            form.MainTabs.SelectedIndex = 3;
            RenderControlToPng(form, Path.Combine(artifactDirectory, "automation-ui-settings.png"));
            Console.WriteLine("四页 UI 检查：设置已从宏页迁出；Lua 页含新增/删除配置；设置页含启动、托盘、分析图片和本机固件刷写接口开关。");
            Directory.Delete(automationDirectory, true);
        }
        catch (Exception exception)
        {
            failure = exception;
        }
    });
    thread.SetApartmentState(ApartmentState.STA);
    thread.Start();
    thread.Join();
    if (failure is not null)
    {
        throw new InvalidOperationException("窗口布局检查失败", failure);
    }
}

static void RenderControlToPng(Control control, string path)
{
    control.PerformLayout();
    using Bitmap bitmap = new(control.ClientSize.Width, control.ClientSize.Height);
    control.DrawToBitmap(bitmap, control.ClientRectangle);
    bitmap.Save(path, ImageFormat.Png);
}

[DllImport("user32.dll", CharSet = CharSet.Auto)]
static extern int SendMessage(IntPtr hWnd, int message, IntPtr wParam, IntPtr lParam);

static int GetFirstVisibleLine(TextBox textBox)
{
    const int emGetFirstVisibleLine = 0x00CE;
    return SendMessage(textBox.Handle, emGetFirstVisibleLine, IntPtr.Zero, IntPtr.Zero);
}

static IEnumerable<Control> EnumerateControls(Control root)
{
    foreach (Control control in root.Controls)
    {
        yield return control;
        foreach (Control descendant in EnumerateControls(control))
        {
            yield return descendant;
        }
    }
}

static void CheckDarkSurfaceTextContrast(BridgeMainForm form)
{
    List<(Control Control, Color ForeColor, Color BackColor, double Ratio, string Name)> checkedControls = [];
    foreach (Control control in EnumerateControls(form))
    {
        // ComboBox 的文字可能由下拉项绘制；MouseCaptureSurface 的文字由控件自行绘制，
        // 两者都不属于本检查的 WinForms 标准文字渲染范围。
        if (control is ComboBox or MouseCaptureSurface || string.IsNullOrWhiteSpace(control.Text))
        {
            continue;
        }

        Color backColor = GetInheritedColor(control, useBackColor: true);
        if (RelativeLuminance(backColor) >= 0.2)
        {
            continue;
        }

        Color foreColor = GetInheritedColor(control, useBackColor: false);
        double ratio = CalculateContrastRatio(foreColor, backColor);
        string name = GetControlEvidenceName(control);
        Require(
            ratio >= 4.5,
            $"深色背景文字对比度不足：控件={name}，前景={foreColor}，背景={backColor}，对比度={ratio:F2}:1");
        checkedControls.Add((control, foreColor, backColor, ratio, name));
    }

    Require(checkedControls.Count > 0, "未找到需要检查的深色背景文字控件");
    var minimum = checkedControls
        .OrderBy(item => item.Ratio)
        .First();
    string evidence = string.Join(
        "; ",
        checkedControls
            .OrderBy(item => item.Name, StringComparer.Ordinal)
            .Select(item => $"{item.Name}={item.Ratio:F2}:1"));
    Console.WriteLine(
        $"深色背景文字对比度检查：通过；实际最小={minimum.Ratio:F2}:1；控件={minimum.Name}；" +
        $"前景={minimum.ForeColor}；背景={minimum.BackColor}；全部证据={evidence}");
}

static Color GetInheritedColor(Control control, bool useBackColor)
{
    for (Control? current = control; current is not null; current = current.Parent)
    {
        Color color = useBackColor ? current.BackColor : current.ForeColor;
        if (color != Color.Empty && color != Color.Transparent)
        {
            return color;
        }
    }

    return useBackColor ? SystemColors.Control : SystemColors.ControlText;
}

static string GetControlEvidenceName(Control control)
{
    if (!string.IsNullOrWhiteSpace(control.Name))
    {
        return control.Name;
    }

    if (!string.IsNullOrWhiteSpace(control.AccessibleName))
    {
        return control.AccessibleName;
    }

    return $"{control.GetType().Name}({control.Text})";
}

static double CalculateContrastRatio(Color first, Color second)
{
    double firstLuminance = RelativeLuminance(first);
    double secondLuminance = RelativeLuminance(second);
    double lighter = Math.Max(firstLuminance, secondLuminance);
    double darker = Math.Min(firstLuminance, secondLuminance);
    return (lighter + 0.05) / (darker + 0.05);
}

static double RelativeLuminance(Color color)
{
    static double Linearize(byte channel)
    {
        double normalized = channel / 255.0;
        return normalized <= 0.04045
            ? normalized / 12.92
            : Math.Pow((normalized + 0.055) / 1.055, 2.4);
    }

    return 0.2126 * Linearize(color.R) +
           0.7152 * Linearize(color.G) +
           0.0722 * Linearize(color.B);
}

static void Require(bool condition, string message)
{
    if (!condition)
    {
        throw new InvalidOperationException(message);
    }
}

internal sealed class RecordingTransport : IBridgeTransport
{
    private readonly ConcurrentQueue<(MessageType Type, byte[] Payload, long Timestamp)> _frames = new();

    public void Send(MessageType type, ReadOnlySpan<byte> payload) =>
        _frames.Enqueue((type, payload.ToArray(), System.Diagnostics.Stopwatch.GetTimestamp()));

    internal MouseReport[] MouseReports() => _frames
        .Where(frame => frame.Type == MessageType.MouseReport)
        .Select(frame => MouseReportCodec.TryDecode(frame.Payload, out MouseReport report)
            ? report
            : throw new InvalidDataException("记录到无效鼠标报告"))
        .ToArray();

    internal long[] MouseTimestamps() => _frames
        .Where(frame => frame.Type == MessageType.MouseReport)
        .Select(frame => frame.Timestamp)
        .ToArray();

    internal byte[][] KeyboardReports() => _frames
        .Where(frame => frame.Type == MessageType.KeyboardReport)
        .Select(frame => frame.Payload)
        .ToArray();

    internal int FrameCount(MessageType type) => _frames.Count(frame => frame.Type == type);

    public void Dispose()
    {
    }
}

internal sealed class RecordingAutomationOutput : IAutomationOutput
{
    private readonly ConcurrentQueue<string> _actions = new();

    public bool IsRemote => false;
    internal string[] Actions => _actions.ToArray();
    public Point GetCursorPosition() => new(100, 100);
    public void MoveRelative(int deltaX, int deltaY) => _actions.Enqueue($"move:{deltaX},{deltaY}");
    public void MoveAbsolute(int x, int y) => _actions.Enqueue($"moveto:{x},{y}");
    public void SetMouseButton(int button, bool pressed) => _actions.Enqueue($"mouse:{button}:{(pressed ? "down" : "up")}");
    public void Wheel(int delta) => _actions.Enqueue($"wheel:{delta}");
    public void KeyDown(byte hidUsage) => _actions.Enqueue($"key:{hidUsage}:down");
    public void KeyUp(byte hidUsage) => _actions.Enqueue($"key:{hidUsage}:up");
}

internal static class EnumerableExtensions
{
    internal static bool ContainsSequence<T>(this IEnumerable<T> source, T first, T second)
        where T : IEquatable<T>
    {
        bool foundFirst = false;
        foreach (T value in source)
        {
            if (!foundFirst)
            {
                foundFirst = value.Equals(first);
            }
            else if (value.Equals(second))
            {
                return true;
            }
        }
        return false;
    }
}

using System.Buffers.Binary;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Collections.Concurrent;
using System.Drawing;
using System.Drawing.Imaging;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using HidBridge.Host;
using HidBridge.Host.Automation;
using HidBridge.Host.Drivers;
using HidBridge.Host.FirmwareUpdate;
using HidBridge.Host.Input;
using HidBridge.Host.RemoteInput;
using HidBridge.Host.Transport;
using HidBridge.Host.Ui;
using HidBridge.Protocol;

if (args.Contains("--firmware-smoothing-only"))
{
    foreach (byte slots in new byte[] { 0, 5, 10, 15, 20 })
    {
        var encoded = MouseReportCodec.EncodeBridge(0, 100, -100, 2, -2, slots);
        Require(MouseReportCodec.TryDecodeBridge(encoded, out var report) && report.FirmwareSmoothingSlots == slots,
            "槽数编解码丢失");
        var json = Encoding.UTF8.GetBytes($"{{\"dx\":100,\"dy\":-100,\"smoothing_slots\":{slots}}}");
        Require(RemoteMouseCommandParser.TryParse(json, out var command, out _) && command.SmoothingSlots == slots,
            "JSON 档位未传递");
        RecordingTransport transport = new();
        using MouseReportPump pump = new(transport);
        pump.ConfigureUdpSmoothingSlots(slots);
        pump.ResetAndSendRelease(true);
        pump.AccumulateRemote(100, -100, 0, 0);
        DateTime deadline = DateTime.UtcNow.AddSeconds(2);
        while (transport.BridgeMouseReports().Length == 0 && DateTime.UtcNow < deadline) Thread.Sleep(2);
        var actual = transport.BridgeMouseReports();
        Require(actual.Length == 1 && actual[0].FirmwareSmoothingSlots == slots && actual[0].Report.X == 100,
            "实际输出帧未携带选档或改变总位移");
    }
    foreach (var bad in new[] { "1", "25", "-5", "5.5", "\"10\"" })
        Require(!RemoteMouseCommandParser.TryParse(Encoding.UTF8.GetBytes(
            $"{{\"dx\":1,\"smoothing_slots\":{bad}}}"), out _, out _), "接受非法档位");
    CheckUdpSmoothingSwitch();
    CheckUdpMouseSmoothing();
    Console.WriteLine("五档平滑 codec/JSON/实际输出帧/开关：PASS");
    return 0;
}

if (args.Any(argument => argument.Equals("--device-log-only", StringComparison.OrdinalIgnoreCase)))
{
    CheckBufferedFullDeviceLogs();
    CheckDeviceTraceRealtimePersistence();
    CheckDeviceLogMirrorPerPortFile();
    Console.WriteLine("完整设备日志、精简显示与后台写盘专项：PASS");
    return 0;
}

if (args.Any(argument => argument.Equals("--raw-capture-hardware", StringComparison.OrdinalIgnoreCase)))
{
    RunRawCaptureHardwareCheck(args);
    return 0;
}

if (args.Any(argument => argument.Equals("--layout-only", StringComparison.OrdinalIgnoreCase)))
{
    CheckHotkeyChooserControl();
    CheckWindowLayout();
    return 0;
}

if (args.Any(argument => argument.Equals("--serial-api-only", StringComparison.OrdinalIgnoreCase)))
{
    CheckDeviceLogMirrorDiscoveryPolicy();
    CheckDeviceLogMirrorPerPortFile();
    CheckSerialForwardingApi();
    Console.WriteLine("串口 loopback API 与镜像独立日志文件检查通过。");
    return 0;
}

if (args.Any(argument => argument.Equals("--mouse-release-plan-only", StringComparison.OrdinalIgnoreCase)))
{
    CheckWin32MouseReleasePlan();
    return 0;
}

if (args.Any(argument => argument.Equals("--statistics-gate-only", StringComparison.OrdinalIgnoreCase)))
{
    CheckStatisticsActivityGate();
    return 0;
}

if (args.Any(argument => argument.Equals("--driver-support-only", StringComparison.OrdinalIgnoreCase)))
{
    CheckCh341DriverSupport();
    return 0;
}

if (args.Any(argument => argument.Equals("--driver-store-probe-only", StringComparison.OrdinalIgnoreCase)))
{
    CheckCh341DriverStoreProbeReadOnly();
    return 0;
}

if (args.Any(argument => argument.Equals("--automation-routing-only", StringComparison.OrdinalIgnoreCase)))
{
    CheckSerialModeSwitchReleaseSequence();
    CheckAutomationRemoteOutput();
    CheckAutomationOutputRouteAndButtonMerge();
    CheckPhysicalInputForwardingRouting();
    CheckRoutingModeChangesKeepHomeAndReloadOnce();
    CheckLocalMouseTriggersReachLua();
    Console.WriteLine("自动化输出路由与串口模式切换释放专项检查通过。");
    return 0;
}

if (args.Any(argument => argument.Equals("--automation-runtime-only", StringComparison.OrdinalIgnoreCase)))
{
    CheckAutomationProfilesAndRuntime();
    CheckLuaRuntimeFeatures();
    CheckTriggerForwardingIntegration();
    CheckToggleMacroStopsOnSecondPress();
    Console.WriteLine("宏/Lua 运行时与本机触发专项检查通过。");
    return 0;
}

if (args.Any(argument => argument.Equals("--lua-format-only", StringComparison.OrdinalIgnoreCase)))
{
    CheckLuaSpacingFormatting();
    return 0;
}

if (args.Any(argument => argument.Equals("--lua-runtime-only", StringComparison.OrdinalIgnoreCase)))
{
    CheckLuaRuntimeFeatures();
    return 0;
}

if (args.Any(argument => argument.Equals("--kmbox-only", StringComparison.OrdinalIgnoreCase)))
{
    CheckKmboxNetCompatibility();
    return 0;
}

if (args.Any(argument => argument.Equals("--stats-snapshot-only", StringComparison.OrdinalIgnoreCase)))
{
    CheckStatsSnapshotProtocol();
    CheckStatsSnapshotHttpApi();
    return 0;
}

CheckMouseReportCodec();
CheckCh341DriverSupport();
CheckMouseAggregation();
CheckOutputSensitivity();
CheckSimulatedUdpInputAggregation();
CheckUdpSmoothingSwitch();
CheckAlwaysOutputUdp();
CheckUdpMouseSmoothing();
CheckMouseStatisticsLoggingDoesNotBlockPump();
CheckStatisticsActivityGate();
CheckMouseMovementRecordingAndChart();
CheckSerialDiscoveryProtocol();
CheckSerialModeSwitchReleaseSequence();
CheckStatsSnapshotProtocol();
CheckStatsSnapshotHttpApi();
CheckSerialHeartbeatGate();
CheckWiFiBoardTransportDisabled();
CheckDeviceLogPolicy();
CheckDeviceTraceRealtimePersistence();
CheckInputSuppressionPolicy();
CheckKeyboardAutoRepeatEdgeFiltering();
CheckMouseButtonsAreTrackedPerDevice();
CheckInputCaptureThreadIsolation();
CheckUnexpectedInputCaptureExitReleasesAll();
CheckForwardingNotificationFailureStillReleasesAll();
CheckInputCallbackFailureStillReleasesAll();
CheckLuaReleaseCannotBlockInputCapture();
CheckWin32MouseReleasePlan();
CheckCursorLockGeometry();
CheckUiLogWriter();
CheckLogFileRetention();
CheckRemoteInputUdpPath();
CheckKmboxNetCompatibility();
CheckFirmwareUpdateApiPolicy();
CheckFirmwareUpdateApiLan();
CheckDeviceLogMirrorDiscoveryPolicy();
CheckDeviceLogMirrorPerPortFile();
CheckSerialForwardingApi();
CheckAutomationProfilesAndRuntime();
CheckLuaRuntimeFeatures();
CheckExternalProfileStorageAndLuaIndentation();
CheckAutomationRemoteOutput();
CheckAutomationOutputRouteAndButtonMerge();
CheckPhysicalInputForwardingRouting();
CheckRoutingModeChangesKeepHomeAndReloadOnce();
CheckLocalMouseTriggersReachLua();
CheckTriggerForwardingIntegration();
CheckToggleMacroStopsOnSecondPress();
CheckHotkeyChooserControl();
CheckWindowLayout();
Console.WriteLine("全部主机检查通过：鼠标协议与聚合、模拟 UDP 输入核心逻辑、固件平滑标记、单板 UDP 开关、串口握手与切换释放、双板统计分页、输入独占策略、UDP 网络输入、固件刷写 API、宏/Lua 执行、本机输入触发、自动化路由与按钮合并、实时日志、Lua 配置和设置页布局。");
return 0;

static void RunRawCaptureHardwareCheck(string[] arguments)
{
    int seconds = 12;
    for (int index = 0; index + 1 < arguments.Length; index++)
    {
        if (arguments[index].Equals("--seconds", StringComparison.OrdinalIgnoreCase) &&
            int.TryParse(arguments[index + 1], out int parsed))
        {
            seconds = Math.Clamp(parsed, 2, 60);
        }
    }

    RecordingTransport transport = new();
    using InputForwarder input = new(transport);
    input.Start();
    try
    {
        input.SetForwardingEnabled(true);
        Console.WriteLine($"RAW_CAPTURE_READY seconds={seconds}");
        Thread.Sleep(TimeSpan.FromSeconds(seconds));
    }
    finally
    {
        input.Stop();
    }

    MouseReport[] reports = transport.MouseReports();
    long[] timestamps = transport.MouseTimestamps();
    long sumX = reports.Sum(report => (long)report.X);
    long sumY = reports.Sum(report => (long)report.Y);
    double activeSeconds = timestamps.Length >= 2
        ? (double)(timestamps[^1] - timestamps[0]) / Stopwatch.Frequency
        : 0;
    double activeRateHz = timestamps.Length >= 2 && activeSeconds > 0
        ? (timestamps.Length - 1) / activeSeconds
        : 0;
    long[] intervalsUs = timestamps
        .Zip(timestamps.Skip(1), (left, right) =>
            (right - left) * 1_000_000 / Stopwatch.Frequency)
        .Order()
        .ToArray();
    long medianUs = intervalsUs.Length == 0 ? 0 : intervalsUs[(intervalsUs.Length - 1) / 2];
    long p95Us = intervalsUs.Length == 0 ? 0 : intervalsUs[(int)((intervalsUs.Length - 1) * 0.95)];

    Console.WriteLine(
        $"RAW_CAPTURE_RESULT reports={reports.Length} active_seconds={activeSeconds:F3} " +
        $"rate_hz={activeRateHz:F2} median_us={medianUs} p95_us={p95Us} " +
        $"sum=({sumX},{sumY})");
    Require(reports.Length > 0, "真实 Raw Input 窗口未捕获到鼠标移动报告");
}

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

    byte[] bridgePayload = MouseReportCodec.EncodeBridge(
        expected.Buttons,
        expected.X,
        expected.Y,
        expected.Wheel,
        expected.Pan,
        MouseReportCodec.FirmwareSmoothingSlots);
    Require(bridgePayload.Length == MouseReportCodec.BridgeLength, "桥接鼠标报告长度应为 8 字节");
    Require(
        MouseReportCodec.TryDecodeBridge(bridgePayload, out BridgeMouseReport bridgeReport),
        "桥接鼠标报告应能解码");
    Require(bridgeReport.Report == expected, "桥接鼠标报告位移往返结果不一致");
    Require(
        bridgeReport.FirmwareSmoothingSlots == MouseReportCodec.FirmwareSmoothingSlots,
        "桥接鼠标报告未携带固件 5 槽配置");

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

static void CheckAlwaysOutputUdp()
{
    RecordingTransport transport = new();
    using MouseReportPump pump = new(transport);
    Require(pump.AlwaysOutputUdpEnabled, "始终 UDP 输出默认必须开启");

    pump.AccumulateRemote(17, -9, 1, -1);
    DateTime deadline = DateTime.UtcNow.AddSeconds(2);
    while (DateTime.UtcNow < deadline &&
           transport.MouseReports().Sum(report => (long)report.X) != 17)
    {
        Thread.Sleep(2);
    }
    MouseReport[] reports = transport.MouseReports();
    Require(reports.Length > 0, "捕获关闭时始终 UDP 输出未发送远端 UDP 移动");
    Require(reports.Sum(report => (long)report.X) == 17, "捕获关闭时 UDP X 位移未完整发送");
    Require(reports.Sum(report => (long)report.Y) == -9, "捕获关闭时 UDP Y 位移未完整发送");

    pump.ConfigureAlwaysOutputUdp(false);
    int beforeDisabled = transport.MouseReports().Length;
    pump.AccumulateRemote(30, 30, 0, 0);
    Thread.Sleep(40);
    Require(
        transport.MouseReports().Length == beforeDisabled,
        "关闭始终 UDP 输出后，HOME 关闭状态仍不应发送远端 UDP 移动");

    pump.ConfigureAlwaysOutputUdp(true);
    pump.AccumulateRemote(-4, 6, 0, 0);
    long baseX = reports.Sum(report => (long)report.X);
    long baseY = reports.Sum(report => (long)report.Y);
    deadline = DateTime.UtcNow.AddSeconds(2);
    while (DateTime.UtcNow < deadline &&
           transport.MouseReports().Sum(report => (long)report.X) != baseX - 4)
    {
        Thread.Sleep(2);
    }
    reports = transport.MouseReports();
    Require(reports.Sum(report => (long)report.X) == baseX - 4, "重新开启始终 UDP 输出后 X 位移未恢复");
    Require(reports.Sum(report => (long)report.Y) == baseY + 6, "重新开启始终 UDP 输出后 Y 位移未恢复");
    Console.WriteLine("始终 UDP 输出检查：HOME 关闭时默认发送，关闭开关后阻断，重新开启后恢复。");
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

    Console.WriteLine(
        $"鼠标移动记录检查：状态机 X=[{string.Join(',', recording.XValues)}]，" +
        $"Y=[{string.Join(',', recording.YValues)}]；纯分析图逻辑=1600x800，X 时间顺序=底部到顶部并显示正负零轴。");
}

static void CheckUdpMouseSmoothing()
{
    UdpMouseSmoother smoother = new();
    smoother.Record(new MouseDelta(50, -7, 5, -5));
    UdpMouseSmootherStatistics statistics = smoother.GetStatistics();
    Require(statistics.SmoothingSlots == 5, "固件 UDP 平滑窗必须为 5 个 1 ms 槽");
    Require(statistics.PendingSlots == 0, "Host 不得再保留平滑发送槽");
    Require(statistics.EnqueuedCommands == 1, "Host 应记录已转交固件平滑的 UDP 命令");

    RecordingTransport transport = new();
    using MouseReportPump pump = new(transport);
    pump.ResetAndSendRelease(true);
    pump.AccumulateRemote(40, -20, 20, -20);
    DateTime deadline = DateTime.UtcNow.AddSeconds(2);
    while (DateTime.UtcNow < deadline && transport.BridgeMouseReports().Length == 0)
    {
        Thread.Sleep(2);
    }
    BridgeMouseReport[] actual = transport.BridgeMouseReports();
    Require(actual.Length == 1, $"Host 应把单条 UDP 命令聚合为 1 个 500 Hz 桥接报告，实际={actual.Length}");
    Require(actual[0].Report == new MouseReport(0, 40, -20, 20, -20), "桥接报告位移总量不正确");
    Require(actual[0].FirmwareSmoothingSlots == 5, "桥接报告未要求固件执行 5 槽平滑");
    Console.WriteLine(
        "UDP 平滑职责检查：Host 以单个 500 Hz 桥接报告发送完整位移并携带 slots=5；" +
        "实际 1 ms 分摊和连续命令重叠由固件纯 C 测试验证。");
}

static void CheckStatisticsActivityGate()
{
    DateTime initial = new(2026, 8, 25, 0, 0, 0, DateTimeKind.Utc);
    TimeSpan interval = TimeSpan.FromSeconds(1);
    StatisticsActivityGate gate = new(initial);
    gate.RecordMovement(0, 0);
    Require(!gate.TryConsume(initial.AddSeconds(10), interval), "统计门控在纯空闲周期不得输出");

    gate.RecordMovement(4, 0);
    Require(!gate.TryConsume(initial.AddMilliseconds(999), interval), "统计门控不得在周期到达前输出");
    Require(gate.TryConsume(initial.AddSeconds(1), interval), "一次实际位移后应允许输出一条统计");
    Require(!gate.TryConsume(initial.AddSeconds(2), interval), "一次统计后空闲不得重复输出");

    gate.RecordMovement(0, -3);
    Require(gate.TryConsume(initial.AddSeconds(3), interval), "新实际位移后应再次允许输出统计");
    Console.WriteLine("鼠标统计活动门控检查：空闲不输出；一次位移输出一次；再次空闲不重复；新位移后再次输出。");
}

static void CheckMouseStatisticsLoggingDoesNotBlockPump()
{
    RecordingTransport transport = new();
    using ManualResetEventSlim statisticsEntered = new(false);
    using ManualResetEventSlim releaseStatistics = new(false);
    string? statisticsText = null;
    using MouseReportPump pump = new(
        transport,
        statisticsInterval: TimeSpan.FromMilliseconds(20),
        statisticsSink: text =>
        {
            statisticsText = text;
            statisticsEntered.Set();
            releaseStatistics.Wait(TimeSpan.FromSeconds(2));
        });

    try
    {
        pump.ResetAndSendRelease(true);
        pump.AccumulateRemote(1, 1, 0, 0);
        Require(
            statisticsEntered.Wait(TimeSpan.FromSeconds(1)),
            "鼠标统计专用日志线程未收到统计快照");
        Require(
            statisticsText is not null &&
            statisticsText.Contains("原始事件", StringComparison.Ordinal) &&
            !statisticsText.Contains("GC暂停", StringComparison.Ordinal) &&
            !statisticsText.Contains("捕获消息泵心跳", StringComparison.Ordinal),
            "鼠标统计未恢复为精简字段");

        int reportsBefore = transport.MouseReports().Length;
        long injectedTimestamp = System.Diagnostics.Stopwatch.GetTimestamp();
        pump.AccumulateRemote(100, -50, 0, 0);
        DateTime deadline = DateTime.UtcNow.AddSeconds(1);
        while (DateTime.UtcNow < deadline && transport.MouseReports().Length == reportsBefore)
        {
            Thread.Sleep(2);
        }

        MouseReport[] actual = transport.MouseReports().Skip(reportsBefore).ToArray();
        Require(actual.Length == 1, $"统计日志阻塞期间 Host 应发送一个聚合报告，实际={actual.Length}");
        Require(actual.Sum(report => (long)report.X) == 100, "统计日志阻塞期间 X 位移不守恒");
        Require(actual.Sum(report => (long)report.Y) == -50, "统计日志阻塞期间 Y 位移不守恒");
        long[] timestamps = transport.MouseTimestamps().Skip(reportsBefore).ToArray();
        double completionMilliseconds =
            (timestamps[^1] - injectedTimestamp) * 1000.0 / System.Diagnostics.Stopwatch.Frequency;
        Require(
            completionMilliseconds < 50,
            $"统计日志阻塞拖慢 500 Hz 聚合输出：{completionMilliseconds:F1} ms");
        Console.WriteLine(
            $"鼠标统计异步检查：日志线程阻塞时单份 500 Hz 聚合报告仍完成，耗时={completionMilliseconds:F1} ms。 ");
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
    byte[] noisyInput = "ESP-ROM:esp32s3\n"u8.ToArray().Concat(hello).ToArray();
    Require(
        SerialDeviceProbe.TryMatchHello(noisyInput, probeSequence, nonce),
        "应能跳过串口日志并识别设备响应");
    Require(
        !SerialDeviceProbe.TryMatchHello(noisyInput.AsSpan(0, noisyInput.Length - 1), probeSequence, nonce),
        "不完整设备响应不得被接受");

    byte[] rolePayload = "HIDBRDG2"u8.ToArray().Concat(nonce).Concat(new byte[] { 2 }).ToArray();
    byte[] roleHello = new FrameCodec().Encode(MessageType.DeviceHello, rolePayload);
    Require(
        SerialDeviceProbe.TryMatchHello(roleHello, probeSequence, nonce, SerialDeviceProbe.MouseHostRole),
        "双板鼠标侧角色响应应被接受");
    Require(
        !SerialDeviceProbe.TryMatchHello(roleHello, probeSequence, nonce, 1),
        "电脑侧角色响应不得被当作鼠标侧串口接受");

    byte[] wrongNonce = nonce.ToArray();
    wrongNonce[0] ^= 0xFF;
    Require(
        !SerialDeviceProbe.TryMatchHello(noisyInput, probeSequence, wrongNonce),
        "随机数不匹配的响应不得被接受");
}

static void CheckSerialModeSwitchReleaseSequence()
{
    FrameCodec codec = new();
    List<byte[]> writtenFrames = [];
    SerialBridge.WriteInputModeChangeReleaseFrames(codec, writtenFrames.Add);
    Require(writtenFrames.Count == 2, "模式切换释放必须恰好写入两帧");
    Require(
        FrameCodec.TryDecode(writtenFrames[0], out BridgeFrame sessionStart) &&
        sessionStart.Type == MessageType.SessionStart &&
        sessionStart.Payload.Length == 0,
        "旧端口模式切换第一帧必须是空 payload 的 SessionStart");
    Require(
        FrameCodec.TryDecode(writtenFrames[1], out BridgeFrame releaseAll) &&
        releaseAll.Type == MessageType.ReleaseAll &&
        releaseAll.Payload.Length == 0,
        "旧端口模式切换第二帧必须是空 payload 的 ReleaseAll");
    Require(
        releaseAll.Sequence == unchecked((ushort)(sessionStart.Sequence + 1)),
        "模式切换 SessionStart 与 ReleaseAll 必须使用连续序号");
    Console.WriteLine("串口模式切换释放检查：直接写入顺序为 SessionStart → ReleaseAll，序号连续；未访问真实串口。");
}

static void CheckStatsSnapshotProtocol()
{
    string fixturePath = Path.Combine(AppContext.BaseDirectory, "stats_snapshot_c_frames.txt");
    Require(File.Exists(fixturePath), $"缺少由 dual_proxy C 编码器生成的统计帧 fixture：{fixturePath}");
    Dictionary<byte, DeviceStatsPageCollector> collectors = [];
    List<(byte Role, byte Kind, byte Index, byte Count, BridgeFrame Frame)> frames = [];
    foreach (string line in File.ReadLines(fixturePath).Where(line => !string.IsNullOrWhiteSpace(line)))
    {
        string[] fields = line.Split(',', 5, StringSplitOptions.None);
        Require(fields.Length == 5, $"C 帧 fixture 行格式错误：{line}");
        byte role = byte.Parse(fields[0]);
        byte kind = byte.Parse(fields[1]);
        byte index = byte.Parse(fields[2]);
        byte count = byte.Parse(fields[3]);
        byte[] wire = Convert.FromHexString(fields[4]);
        Require(FrameCodec.TryDecode(wire, out BridgeFrame frame), $"C fixture 帧 CRC/长度错误：{line}");
        Require(frame.Type == MessageType.StatsSnapshotResponse, "C fixture 消息类型必须是 StatsSnapshotResponse");
        Require(frame.Sequence == 0xA17C, "C fixture 诊断序号不正确");
        Require(frame.Payload.Length >= 12 && frame.Payload[1] == role && frame.Payload[2] == kind &&
                frame.Payload[3] == index && frame.Payload[4] == count,
            "C fixture 外部索引与 C 编码 payload header 不一致");
        if (!collectors.TryGetValue(role, out DeviceStatsPageCollector? collector))
        {
            collector = new DeviceStatsPageCollector($"COM-role-{role}", frame.Sequence);
            collectors.Add(role, collector);
        }
        collector.Add(frame);
        frames.Add((role, kind, index, count, frame));
    }

    Require(frames.Count == 28, $"C fixture 应包含 P/M 两板 28 个实际编码页，实际={frames.Count}");
    Require(collectors.Count == 2 && collectors[1].IsComplete && collectors[2].IsComplete,
        "实际 C 输出必须完整覆盖 role=1 P 与 role=2 M 的 counters/queues 两组分页");
    DeviceStatisticsSnapshot pc = collectors[1].Build();
    DeviceStatisticsSnapshot mouse = collectors[2].Build();
    Require(pc.RoleName == "P/PC_DEVICE" && mouse.RoleName == "M/MOUSE_HOST",
        "C 角色编号必须是 P=1、M=2");
    Require(pc.State is { } pcState && mouse.State is { } mouseState &&
            (bool)pcState["mounted"] &&
            (string)pcState["lastResult"] == "mounted_acked" &&
            (ulong)pcState["operationEpoch"] == 7UL &&
            (bool)mouseState["firstVendorRequestSeen"] &&
            (ulong)mouseState["vendorSessionEpoch"] == 33UL,
        "C 角色状态计数必须正确解码P挂载/最终ACK与M厂商会话字段");
    foreach ((byte role, byte stateCounterId) in new[] { ((byte)1, (byte)67), ((byte)2, (byte)68) })
    {
        DeviceStatsPageCollector missingStateCollector = new($"COM-missing-state-{role}", 0xA17C);
        foreach (BridgeFrame originalFrame in frames.Where(item => item.Role == role).Select(item => item.Frame))
        {
            byte[] payload = originalFrame.Payload.ToArray();
            if (payload[2] == 1)
            {
                for (int item = 0; item < payload[6]; item++)
                {
                    int recordOffset = 12 + item * 10;
                    if (payload[recordOffset] == stateCounterId)
                    {
                        payload[recordOffset] = 0xFE;
                    }
                }
            }
            missingStateCollector.Add(originalFrame with { Payload = payload });
        }

        bool missingRequiredStateRejected = false;
        try
        {
            _ = missingStateCollector.Build();
        }
        catch (InvalidDataException)
        {
            missingRequiredStateRejected = true;
        }
        Require(missingRequiredStateRejected, $"role={role} 缺少必需状态计数器 id={stateCounterId} 时必须明确失败");
    }
    Require((ulong)pc.Counters["attempt"] == 39_000UL &&
            (ulong)mouse.Counters["recover"] == 11_000UL,
        "C 角色计数器 ID 集合映射错误或被对方角色误用");
    Require((long)mouse.Counters["motion_rx_dx"] == -12_345L &&
            (long)mouse.Counters["motion_rx_dy"] == -6_789L,
        "M 的有符号位移统计必须按 i64 解码，不能变成巨大 unsigned 数值");
    DeviceQueueStatistics rxRing = mouse.Queues.Single(queue => queue.Id == 13);
    Require(rxRing.Unit == 2 && rxRing.Capacity == 16_384 && rxRing.Depth == 23 && rxRing.Peak == 1_024,
        "UART1 RX ring 队列必须以字节单位且保留实际容量/深度/峰值");
    DeviceQueueStatistics eventQueue = pc.Queues.Single(queue => queue.Id == 9);
    Require(eventQueue.Received is null && eventQueue.Rejected is null && eventQueue.Dropped == 17,
        "UART1 event queue 的未知 received/rejected 与独立 reset_dropped 语义必须保持");
    Require(DeviceStatisticsFormatter.Format(new DeviceStatisticsQueryResult(DateTimeOffset.UnixEpoch, [pc, mouse]))
                .Contains("motion_rx_dx = -12345", StringComparison.Ordinal),
        "统计 UI 格式化必须呈现 signed motion 数值");

    DeviceStatsPageCollector duplicateCollector = new("COM-test", 0xA17C);
    BridgeFrame firstCounter = frames.First(item => item.Role == 1 && item.Kind == 1).Frame;
    duplicateCollector.Add(firstCounter);
    bool duplicateRejected = false;
    try
    {
        duplicateCollector.Add(firstCounter);
    }
    catch (InvalidDataException)
    {
        duplicateRejected = true;
    }
    Require(duplicateRejected, "重复统计页必须明确失败");

    DeviceStatsPageCollector missingPageCollector = new("COM-test", 0xA17C);
    missingPageCollector.Add(firstCounter);
    bool missingPageRejected = false;
    try
    {
        _ = missingPageCollector.Build();
    }
    catch (InvalidOperationException)
    {
        missingPageRejected = true;
    }
    Require(missingPageRejected, "只收一部分统计页时不得构造成功快照");

    DeviceStatsPageCollector invalidSchemaCollector = new("COM-test", 0xA17C);
    byte[] invalidSchema = firstCounter.Payload.ToArray();
    invalidSchema[0]++;
    bool invalidSchemaRejected = false;
    try
    {
        invalidSchemaCollector.Add(firstCounter with { Payload = invalidSchema });
    }
    catch (InvalidDataException)
    {
        invalidSchemaRejected = true;
    }
    Require(invalidSchemaRejected, "未知 schema version 必须明确失败");

    DeviceStatsPendingRequestRegistry registry = new();
    DeviceStatsQueryContext expired = registry.Register(["COM12"], 0x1234);
    Require(registry.HasActive, "统计请求注册后必须进入 pending 状态");
    registry.Clear(expired); // 仅模拟 timeout/cancellation finally 清理，不覆盖 deadline 实际触发时序。
    Require(!registry.HasActive && !registry.TryGet("COM12", firstCounter, out _, out _),
        "timeout 清理后迟到页必须因无 pending 请求被忽略");
    DeviceStatsQueryContext next = registry.Register(["COM12"], 0x1235);
    Require(!registry.TryGet("COM12", firstCounter, out _, out _),
        "迟到的旧 sequence 不得污染下一次统计快照");
    BridgeFrame newSequenceFrame = firstCounter with { Sequence = 0x1235 };
    Require(registry.TryGet("COM12", newSequenceFrame, out _, out _),
        "当前 request sequence 与端口匹配时应能找到 pending collector");
    registry.Clear(next);

    DeviceOutputFrameScanner scanner = new();
    List<BridgeFrame> scanned = [];
    byte[] prefix = Encoding.UTF8.GetBytes("I (1) realtime log\r\n");
    byte[] sampleWire = Convert.FromHexString(File.ReadLines(fixturePath).First().Split(',', 5)[4]);
    byte[] stream = [.. prefix, .. sampleWire, .. Encoding.UTF8.GetBytes("\r\nW (2) tail\r\n")];
    List<byte> textBytes = [];
    foreach (byte value in stream)
    {
        textBytes.AddRange(scanner.Feed([value], scanned.Add));
    }
    Require(scanned.Count == 1 && scanned[0].Type == MessageType.StatsSnapshotResponse,
        "实时混合日志扫描器必须识别跨读取边界 stats response");
    Require(Encoding.UTF8.GetString(textBytes.ToArray()) == "I (1) realtime log\r\n\r\nW (2) tail\r\n",
        "剥离 stats 二进制帧时不得丢失前后实时日志文本");
    Console.WriteLine("统计协议回归：PASS；28 页来自 dual_proxy C 序列化器，P/M 角色映射、必需 USB/会话状态及缺项拒绝、signed 位移、20B 队列、未知值、缺页/重复/版本、超时清理后的迟到序号与实时日志混流均符合协议。");
}

static void CheckStatsSnapshotHttpApi()
{
    FirmwareFlashSnapshot flashSnapshot = new("stats-api-check", "idle", "test", null, null, null, null);
    bool NeverStart(string manifestPath, string portName, out FirmwareFlashSnapshot current)
    {
        current = flashSnapshot;
        return false;
    }

    FirmwareUpdateApiServer CreateServer(
        Func<TimeSpan, CancellationToken, Task<DeviceStatisticsQueryResult>>? statsReader)
    {
        return new FirmwareUpdateApiServer(
            0,
            () => flashSnapshot,
            NeverStart,
            () => null,
            () => ["COM12"],
            (string portName, byte[] bytes, CancellationToken cancellationToken) => bytes.Length,
            cancellationToken => new SerialPortRefreshSnapshot(["COM12"], ["COM12"]),
            statsReader);
    }

    HttpClient CreateClient(FirmwareUpdateApiServer server) =>
        new() { BaseAddress = new Uri($"http://127.0.0.1:{server.Port}") };

    DeviceStatisticsQueryResult snapshot = new(
        DateTimeOffset.UnixEpoch,
        [
            new DeviceStatisticsSnapshot(
                "COM12",
                2,
                "M/MOUSE_HOST",
                123_456,
                new Dictionary<string, object> { ["motion_rx_dx"] = -12_345L, ["recover"] = 17UL },
                new Dictionary<string, object>
                {
                    ["vendorSessionActive"] = true,
                    ["firstVendorRequestSeen"] = true,
                    ["peerGenerationCurrent"] = true,
                    ["waitingHost"] = false,
                    ["vendorSessionEpoch"] = 33UL,
                },
                [
                    new DeviceQueueStatistics(
                        13,
                        "UART1.rx_ring",
                        2,
                        16_384,
                        23,
                        1_024,
                        null,
                        null,
                        5),
                ]),
        ]);
    Task<DeviceStatisticsQueryResult> ReadSnapshot(TimeSpan timeout, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        Require(timeout == TimeSpan.FromSeconds(4), "stats API 应为设备读取器传入 4 秒设备超时");
        return Task.FromResult(snapshot);
    }

    using (FirmwareUpdateApiServer server = CreateServer(ReadSnapshot))
    {
        server.SetEnabled(true);
        using HttpClient client = CreateClient(server);
        using HttpResponseMessage response = client.GetAsync("/api/v1/serial/stats").GetAwaiter().GetResult();
        Require(response.StatusCode == System.Net.HttpStatusCode.OK, "GET stats 应返回 200");
        using JsonDocument json = JsonDocument.Parse(response.Content.ReadAsStringAsync().GetAwaiter().GetResult());
        JsonElement board = json.RootElement.GetProperty("boards")[0];
        Require(board.GetProperty("roleName").GetString() == "M/MOUSE_HOST", "stats JSON 必须包含正确的板角色");
        Require(board.GetProperty("counters").GetProperty("motion_rx_dx").GetInt64() == -12_345,
            "stats JSON 必须保留有符号位移计数");
        Require(board.GetProperty("state").GetProperty("vendorSessionActive").GetBoolean() &&
                board.GetProperty("state").GetProperty("vendorSessionEpoch").GetUInt64() == 33UL,
            "stats JSON 必须呈现M厂商会话状态");
        JsonElement queue = board.GetProperty("queues")[0];
        Require(queue.GetProperty("unit").GetByte() == 2 &&
                queue.GetProperty("received").ValueKind == JsonValueKind.Null &&
                queue.GetProperty("rejected").ValueKind == JsonValueKind.Null &&
                queue.GetProperty("depth").GetUInt16() == 23,
            "stats JSON 必须呈现队列单位、深度和未知 null 字段");
    }

    using (FirmwareUpdateApiServer server = CreateServer(
               (_, _) => Task.FromException<DeviceStatisticsQueryResult>(new TimeoutException("fake stats timeout"))))
    {
        server.SetEnabled(true);
        using HttpClient client = CreateClient(server);
        using HttpResponseMessage response = client.GetAsync("/api/v1/serial/stats").GetAwaiter().GetResult();
        Require(response.StatusCode == System.Net.HttpStatusCode.GatewayTimeout, "stats TimeoutException 应映射为 504");
    }

    using (FirmwareUpdateApiServer server = CreateServer(null))
    {
        server.SetEnabled(true);
        using HttpClient client = CreateClient(server);
        using HttpResponseMessage response = client.GetAsync("/api/v1/serial/stats").GetAwaiter().GetResult();
        Require(response.StatusCode == System.Net.HttpStatusCode.ServiceUnavailable, "缺少 stats delegate 应返回 503");
    }

    using (FirmwareUpdateApiServer server = CreateServer(ReadSnapshot))
    {
        server.SetEnabled(true);
        using TcpClient socket = new();
        socket.Connect(System.Net.IPAddress.Loopback, server.Port);
        using NetworkStream stream = socket.GetStream();
        byte[] request = Encoding.ASCII.GetBytes(
            "GET /api/v1/serial/stats HTTP/1.1\r\n" +
            "Host: 127.0.0.1\r\n" +
            "Content-Length: 2\r\n" +
            "Connection: close\r\n\r\n{}");
        stream.Write(request);
        using StreamReader reader = new(stream, Encoding.ASCII, leaveOpen: true);
        string? statusLine = reader.ReadLine();
        Require(statusLine?.StartsWith("HTTP/1.1 400", StringComparison.Ordinal) == true,
            "带 body 的 GET stats 应返回 400");
    }

    Console.WriteLine("统计 loopback API 集成检查：PASS；HTTP 200 JSON 保留 role/signed counter/queue unit/null，Timeout=504、无 delegate=503、请求体=400。");
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
        SerialBridge.ShouldPersistDeviceLog(false, "I (1) NIMBLE_HIDD: notify report=mouse"),
        "精简模式必须过滤高频普通通知日志");
    Require(
        SerialBridge.ShouldPersistDeviceLog(true, "I (1) NIMBLE_HIDD: notify report=mouse"),
        "完整模式必须保留全部设备日志");
}

static void CheckSerialHeartbeatGate()
{
    int active = 0;
    Require(SerialBridge.TryEnterHeartbeat(ref active), "首个串口心跳必须进入执行区");
    Require(!SerialBridge.TryEnterHeartbeat(ref active), "前一个串口心跳未结束时不得重入并积累探测任务");
    SerialBridge.ExitHeartbeat(ref active);
    Require(SerialBridge.TryEnterHeartbeat(ref active), "串口心跳结束后必须允许下一次执行");
    SerialBridge.ExitHeartbeat(ref active);
}

static void CheckInputSuppressionPolicy()
{
    Require(!InputForwarder.ShouldSuppressKeyboard(false, false, false), "同步关闭时普通键盘按下不得拦截");
    Require(InputForwarder.ShouldSuppressKeyboard(true, false, false), "同步开启时普通键盘按下必须拦截");
    Require(InputForwarder.ShouldSuppressKeyboard(false, true, false), "控制快捷键按下必须拦截");
    Require(!InputForwarder.ShouldSuppressKeyboard(true, false, true), "同步开启时普通键盘松开必须交还本机，避免控制端卡键");
    Require(!InputForwarder.ShouldSuppressKeyboard(false, true, true), "控制快捷键松开必须交还本机");
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

static void CheckBufferedFullDeviceLogs()
{
    const string detail = "I (1) dual_hid_host: 物理 SET_REPORT: interface=1 id=11 type=2 length=20";
    Require(SerialBridge.ShouldPersistDeviceLog(false, detail), "精简显示仍须保存SET_REPORT明细");
    Require(!SerialBridge.ShouldMirrorDeviceLog(false, detail), "精简窗口不应显示SET_REPORT洪峰");
    Require(!SerialBridge.ShouldMirrorDeviceLog(false, "W (2) test: timeout"), "精简窗口不得新增设备原始警告");
    Require(SerialBridge.ShouldMirrorDeviceLog(true, detail), "完整窗口须显示明细");
    string directory = Path.Combine(Path.GetTempPath(), $"hidbridge-full-log-{Guid.NewGuid():N}");
    Directory.CreateDirectory(directory);
    try
    {
        string path = Path.Combine(directory, "full.log");
        using (BufferedDeviceLog sink = new(SerialBridge.CreateTraceWriter(path, false)))
        {
            Stopwatch timer = Stopwatch.StartNew();
            for (int i = 0; i < 10000; ++i)
            {
                Require(sink.TryWrite($"{i:D5} {detail}"), "正常批量日志不得丢弃");
            }
            Console.WriteLine($"完整日志入队10000行：{timer.Elapsed.TotalMilliseconds:F2} ms（本机样本，非真机延迟验收）");
            Require(SpinWait.SpinUntil(() => sink.Written == 10000, 3000), "后台未完成10000行写入");
            Require(SpinWait.SpinUntil(() =>
            {
                using FileStream file = new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                using StreamReader reader = new(file, Encoding.UTF8);
                return reader.ReadToEnd().Contains("09999 " + detail);
            }, 3000),
                "日志仍打开时必须周期刷新可读取");
            Require(sink.Dropped == 0 && sink.WriteErrors == 0, "正常写盘出现缺口");
        }
        string[] lines = File.ReadAllLines(path).Where(line => !line.StartsWith("[日志写盘统计]")).ToArray();
        Require(lines.Length == 10000, "磁盘实际行数与输入不符");
        for (int i = 0; i < lines.Length; ++i)
        {
            Require(lines[i] == $"{i:D5} {detail}", "完整日志内容或顺序错误");
        }
        using GatedDeviceLogWriter blocked = new();
        using (BufferedDeviceLog sink = new(blocked, 1024))
        {
            Require(sink.TryWrite("first"), "首行无法入队");
            Require(blocked.Entered.Wait(2000), "慢磁盘测试没有进入写入");
            try
            {
                Stopwatch timer = Stopwatch.StartNew();
                for (int i = 0; i < 1000; ++i) { sink.TryWrite($"slow-{i}"); }
                Require(timer.ElapsedMilliseconds < 1000, "队列满时接收线程被慢磁盘阻塞");
                Require(sink.Dropped > 0 && sink.PeakBytes <= 1024, "有界队列未限流或未记录缺口");
            }
            finally { blocked.Release.Set(); }
        }
        Require(blocked.Content.ToString().Contains("dropped="), "丢弃统计没有实际写入日志");
        Console.WriteLine("完整日志文件逐行比对10000/10000；慢磁盘背压与缺口统计：PASS");
    }
    finally { Directory.Delete(directory, recursive: true); }
}

static void CheckDeviceTraceRealtimePersistence()
{
    string directory = Path.Combine(Path.GetTempPath(), $"hidbridge-device-log-{Guid.NewGuid():N}");
    string path = Path.Combine(directory, "device.log");
    Directory.CreateDirectory(directory);
    try
    {
        using StreamWriter writer = SerialBridge.CreateTraceWriter(path, fullLogging: false);
        writer.WriteLine("实时落盘检查");
        writer.Flush();
        using FileStream stream = new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        using StreamReader reader = new(stream, Encoding.UTF8);
        Require(
            reader.ReadToEnd().Contains("实时落盘检查", StringComparison.Ordinal),
            "精简设备日志在写入器仍打开时不可读取");
        Console.WriteLine("设备日志实时落盘检查：写入器未关闭时已从文件读取到最新日志。");
    }
    finally
    {
        Directory.Delete(directory, recursive: true);
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

static void CheckMouseButtonsAreTrackedPerDevice()
{
    RecordingTransport transport = new();
    using InputForwarder input = new(transport, legacyFirmwareCompatibility: () => true);
    IntPtr firstMouse = new(1);
    IntPtr secondMouse = new(2);
    input.SetForwardingEnabled(true);

    input.ProcessRawMouseInputForChecks(firstMouse, new NativeMethods.RawMouse
    {
        Buttons = NativeMethods.RawMouseRightButtonDown,
    });
    Require(
        SpinWait.SpinUntil(
            () => transport.MouseReports().Any(report => (report.Buttons & 0x02) != 0),
            1000),
        "第一只鼠标右键按下未送达报告泵");
    int reportsAfterPress = transport.MouseReports().Length;

    input.ProcessRawMouseInputForChecks(secondMouse, new NativeMethods.RawMouse
    {
        Buttons = NativeMethods.RawMouseRightButtonUp,
    });
    Thread.Sleep(20);
    MouseReport[] afterUnpairedRelease = transport.MouseReports();
    Require(
        afterUnpairedRelease.Length == reportsAfterPress &&
        (afterUnpairedRelease[^1].Buttons & 0x02) != 0,
        "另一只鼠标的未配对右键松开不应释放第一只鼠标仍按住的右键");

    input.ProcessRawMouseInputForChecks(secondMouse, new NativeMethods.RawMouse
    {
        Buttons = NativeMethods.RawMouseRightButtonDown,
    });
    input.ProcessRawMouseInputForChecks(firstMouse, new NativeMethods.RawMouse
    {
        Buttons = NativeMethods.RawMouseRightButtonUp,
    });
    Thread.Sleep(20);
    Require(
        (transport.MouseReports()[^1].Buttons & 0x02) != 0,
        "第一只鼠标松开时第二只鼠标仍按住右键，不应发送右键松开");

    input.ProcessRawMouseInputForChecks(secondMouse, new NativeMethods.RawMouse
    {
        Buttons = NativeMethods.RawMouseRightButtonUp,
    });
    Require(
        SpinWait.SpinUntil(() => transport.MouseReports().Last().Buttons == 0, 1000),
        "最后一只鼠标松开右键后应发送右键松开报告");
    Console.WriteLine("多鼠标按钮检查：按设备维护状态，非来源设备的松开不会清除仍按住的右键。");
}

static void CheckLuaReleaseCannotBlockInputCapture()
{
    RecordingTransport transport = new();
    using InputForwarder input = new(transport, legacyFirmwareCompatibility: () => true);
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

        for (int index = 0; index < 5000; index++)
        {
            Require(
                input.EnqueueRawMouseInputForChecks(new NativeMethods.RawMouse { LastX = 1, LastY = -1 }),
                "连续纯移动合并期间输入队列拒绝事件");
        }
        Require(
            input.PendingInputEventCountForChecks <= 1,
            $"5000 个连续纯移动不应形成无界 FIFO 积压，当前队列={input.PendingInputEventCountForChecks}");

        System.Diagnostics.Stopwatch stopwatch = System.Diagnostics.Stopwatch.StartNew();
        bool accepted = input.EnqueueKeyboardInputForChecks(0x41, false, true);
        stopwatch.Stop();
        Require(accepted, "Lua 松开回调阻塞时输入捕获队列拒绝新事件");
        Require(
            stopwatch.ElapsedMilliseconds < 50,
            $"Lua 松开回调反向阻塞输入捕获：{stopwatch.ElapsedMilliseconds} ms");
        Require(
            input.PendingInputEventCountForChecks <= 2,
            $"键盘边沿只应排在一份合并移动之后，当前队列={input.PendingInputEventCountForChecks}");

        Thread.Sleep(30);
        allowReleaseHandler.Set();
        bool movementCompleted = SpinWait.SpinUntil(
            () => transport.MouseReports().Sum(report => (long)report.X) == 5000 &&
                  transport.MouseReports().Sum(report => (long)report.Y) == -5000,
            2000);
        MouseReport[] movementReports = transport.MouseReports();
        long movementX = movementReports.Sum(report => (long)report.X);
        long movementY = movementReports.Sum(report => (long)report.Y);
        Require(
            movementCompleted,
            $"连续纯移动合并后累计位移未完整送出：X={movementX}，Y={movementY}，报告数={movementReports.Length}");
    }
    finally
    {
        allowReleaseHandler.Set();
        input.Stop();
    }
    Console.WriteLine("Lua 松开连点隔离检查：实体左键松开优先送出；5000 个纯移动仅保留一份队列标记且累计位移守恒。");
}

static void CheckOutputSensitivity()
{
    RecordingTransport transport = new();
    using MouseReportPump pump = new(transport);
    pump.ConfigureUdpSmoothing(false);
    pump.ConfigureOutputSensitivity(0.3);
    Require(Math.Abs(pump.OutputSensitivity - 0.3) < 0.0001, "输出灵敏度未接受 0.3 下限值");
    Require(Math.Abs(MouseOutputSensitivity.Clamp(1.234) - 1.23) < 0.0001, "输出灵敏度输入精度未与滑块步进同步");
    pump.ResetAndSendRelease(true);

    for (int index = 0; index < 10; index++)
    {
        pump.Accumulate(0, false, 1, -1, index == 0 ? 2 : 0, 0);
    }

    DateTime deadline = DateTime.UtcNow.AddSeconds(2);
    while (DateTime.UtcNow < deadline &&
           transport.MouseReports().Sum(report => (long)report.X) < 3)
    {
        Thread.Sleep(2);
    }
    MouseReport[] lowSensitivityReports = transport.MouseReports();
    Require(lowSensitivityReports.Sum(report => (long)report.X) == 3, "0.3 灵敏度未保留小数余量并输出完整 X");
    Require(lowSensitivityReports.Sum(report => (long)report.Y) == -3, "0.3 灵敏度未保留小数余量并输出完整 Y");
    Require(lowSensitivityReports.Sum(report => (long)report.Wheel) == 2, "输出灵敏度不得改变滚轮值");

    pump.ResetAndSendRelease(false);
    pump.ConfigureOutputSensitivity(2.0);
    Require(Math.Abs(pump.OutputSensitivity - 2.0) < 0.0001, "输出灵敏度未接受 2.0 倍放大");
    pump.ResetAndSendRelease(true);
    int highSensitivityStart = transport.MouseReports().Length;
    pump.AccumulateRemote(3, -2, 1, 0);

    deadline = DateTime.UtcNow.AddSeconds(2);
    while (DateTime.UtcNow < deadline &&
           transport.MouseReports().Skip(highSensitivityStart).Sum(report => (long)report.X) < 6)
    {
        Thread.Sleep(2);
    }
    MouseReport[] highSensitivityReports = transport.MouseReports().Skip(highSensitivityStart).ToArray();
    Require(highSensitivityReports.Sum(report => (long)report.X) == 6, "2.0 灵敏度未覆盖 UDP 公共发送链路");
    Require(highSensitivityReports.Sum(report => (long)report.Y) == -4, "2.0 灵敏度 Y 输出不正确");
    Require(highSensitivityReports.Sum(report => (long)report.Wheel) == 1, "2.0 灵敏度不得改变 UDP 滚轮值");
    Console.WriteLine("统一输出灵敏度检查：0.3 倍小数余量守恒、2.0 倍 UDP 输入放大，滚轮保持原值。");
}

static void CheckCh341DriverSupport()
{
    Require(
        WindowsCh341DeviceProbe.ClassifyProblemCode(0) == Ch341DeviceState.Working,
        "PnP Problem Code 0 应判定为 Working");
    Require(
        WindowsCh341DeviceProbe.ClassifyProblemCode(28) == Ch341DeviceState.MissingDriver,
        "PnP Problem Code 28 应判定为 MissingDriver");
    Require(
        WindowsCh341DeviceProbe.ClassifyProblemCode(10) == Ch341DeviceState.OtherProblem,
        "非 0/28 的 PnP Problem Code 不得触发自动安装");

    Ch341DriverStoreProbeResult englishDriverStore = Ch341DriverStoreParser.Parse(
        "Published Name : oem42.inf\n" +
        "Original Name : CH341SER.INF\n" +
        "Provider Name : wch.cn\n" +
        "Class Name : Ports\n");
    Require(
        englishDriverStore.State == Ch341DriverStoreState.Installed,
        "英文 pnputil Driver Store 输出应识别 CH341SER.INF 与 wch.cn");
    Ch341DriverStoreProbeResult localizedDriverStore = Ch341DriverStoreParser.Parse(
        "发布项：oem43.inf\n" +
        "原始文件：CH341SER.INF\n" +
        "供应商：Nanjing Qinheng Microelectronics Co., Ltd.\n" +
        "类别：端口\n");
    Require(
        localizedDriverStore.State == Ch341DriverStoreState.Installed,
        "中文标签 pnputil Driver Store 输出应只按稳定值识别驱动");
    Require(
        Ch341DriverStoreParser.Parse("Published Name : oem44.inf\nOriginal Name : CH340SER.INF\nProvider Name : wch.cn\n").State ==
        Ch341DriverStoreState.Missing,
        "Driver Store 中没有 CH341SER.INF 时应判定为 Missing");
    Ch341DriverStoreProbeResult driverStoreProbeFailed = Ch341DriverStoreProbeResult.ProbeFailed("test");
    Require(
        driverStoreProbeFailed.State == Ch341DriverStoreState.ProbeFailed,
        "Driver Store 探测错误必须保留为 ProbeFailed，不得当作缺失驱动");

    string root = Path.Combine(Path.GetTempPath(), $"hidbridge-driver-check-{Guid.NewGuid():N}");
    try
    {
        string preferredDirectory = Path.Combine(root, "drivers", "wch-ch341ser", "CH341SER");
        Directory.CreateDirectory(preferredDirectory);
        string preferredInf = Path.Combine(preferredDirectory, "CH341SER.INF");
        string preferredSetup = Path.Combine(preferredDirectory, "SETUP.EXE");
        File.WriteAllText(preferredInf, "; test INF");
        File.WriteAllText(preferredSetup, "test setup");
        Ch341DriverPackage preferred = Ch341DriverPackageLocator.Locate(root)
            ?? throw new InvalidOperationException("未找到整理后的 CH341SER 驱动目录");
        Require(
            preferred.InfPath.Equals(Path.GetFullPath(preferredInf), StringComparison.OrdinalIgnoreCase),
            "驱动包定位必须优先使用 drivers/wch-ch341ser/CH341SER/CH341SER.INF");
        Require(
            preferred.SetupPath.Equals(Path.GetFullPath(preferredSetup), StringComparison.OrdinalIgnoreCase),
            "驱动包必须同时定位同目录的 WCH 官方 SETUP.EXE");

        File.Delete(preferredInf);
        File.Delete(preferredSetup);
        string legacyDirectory = Path.Combine(
            root,
            "drivers",
            "wch-ch341ser",
            "CH341SER_v4.0_2026-06-26",
            "CH341SER");
        Directory.CreateDirectory(legacyDirectory);
        string legacyInf = Path.Combine(legacyDirectory, "CH341SER.INF");
        string legacySetup = Path.Combine(legacyDirectory, "SETUP.EXE");
        File.WriteAllText(legacyInf, "; legacy test INF");
        File.WriteAllText(legacySetup, "legacy test setup");
        Ch341DriverPackage legacy = Ch341DriverPackageLocator.Locate(root)
            ?? throw new InvalidOperationException("未找到旧版嵌套 CH341SER 驱动目录");
        Require(
            legacy.InfPath.Equals(Path.GetFullPath(legacyInf), StringComparison.OrdinalIgnoreCase),
            "驱动包定位必须兼容旧版带版本目录的嵌套路径");
        Require(
            legacy.SetupPath.Equals(Path.GetFullPath(legacySetup), StringComparison.OrdinalIgnoreCase) &&
            Ch341DriverInstallerPolicy.VendorInstallArguments == "/S",
            "自动安装必须调用同目录 WCH SETUP.EXE /S，以便后续由官方安装器卸载");
        Require(
            Ch341DriverInstallerPolicy.ClassifyExitCode(0) == Ch341DriverInstallState.PackageCommandSucceeded &&
            Ch341DriverInstallerPolicy.ClassifyExitCode(3010) == Ch341DriverInstallState.PackageCommandSucceeded &&
            Ch341DriverInstallerPolicy.ClassifyExitCode(1) == Ch341DriverInstallState.Failed,
            "WCH SETUP.EXE 退出码只能分类为安装器命令成功，不能直接分类为设备可用");

        Ch341ProbeResult missing = new(
            Ch341DeviceState.MissingDriver,
            new Ch341DeviceInfo(
                "USB\\VID_1A86&PID_7523\\TEST",
                "USB-SERIAL CH340",
                Ch341DeviceState.MissingDriver,
                28,
                0,
                null),
            null);
        Ch341ProbeResult other = missing with
        {
            State = Ch341DeviceState.OtherProblem,
            Device = missing.Device! with
            {
                State = Ch341DeviceState.OtherProblem,
                ProblemCode = 10,
            },
        };
        Require(
            Ch341StartupPolicy.Decide(missing, packageAvailable: true) == Ch341StartupAction.OfferInstall,
            "Code 28 且驱动包存在时必须提示安装");
        Require(
            Ch341StartupPolicy.Decide(missing, packageAvailable: false) == Ch341StartupAction.None,
            "Code 28 但驱动包缺失时不得调用安装");
        Require(
            Ch341StartupPolicy.Decide(other, packageAvailable: true) == Ch341StartupAction.ManualRecovery,
            "其他 Problem Code 只能提示手动处理");
        Require(
            Ch341StartupPolicy.Decide(Ch341ProbeResult.NoDevice(), packageAvailable: true) == Ch341StartupAction.None,
            "未发现 CH340 设备时不得提示驱动安装");
        Require(
            Ch341StartupPolicy.Decide(
                Ch341ProbeResult.NoDevice(),
                packageAvailable: true,
                englishDriverStore with { State = Ch341DriverStoreState.Missing }) == Ch341StartupAction.OfferInstall,
            "未发现设备但 Driver Store 缺少 CH341SER.INF 且包存在时必须提示安装");
        Require(
            Ch341StartupPolicy.Decide(
                Ch341ProbeResult.NoDevice(),
                packageAvailable: true,
                englishDriverStore) == Ch341StartupAction.None,
            "未发现设备但 Driver Store 已安装驱动时不得提示安装");
        Require(
            Ch341StartupPolicy.Decide(
                Ch341ProbeResult.NoDevice(),
                packageAvailable: true,
                driverStoreProbeFailed) == Ch341StartupAction.None,
            "Driver Store 探测失败时不得冒险提示安装");
        Require(
            Ch341StartupPolicy.Decide(
                missing,
                packageAvailable: true,
                driverStoreProbeFailed) == Ch341StartupAction.None,
            "Driver Store 探测失败时即使设备为 Code 28 也不得冒险提示安装");
        Require(
            Ch341StartupPolicy.GetPromptReason(
                Ch341ProbeResult.NoDevice(),
                englishDriverStore with { State = Ch341DriverStoreState.Missing }) ==
            Ch341DriverPromptReason.DriverStoreMissing,
            "无设备且 Driver Store 缺失时提示原因必须明确为 Driver Store 缺失");
        Require(
            Ch341StartupPolicy.GetPromptReason(missing, driverStoreProbeFailed) ==
            null,
            "Driver Store 探测失败时不得生成安装提示原因");
        Require(
            Ch341StartupPolicy.GetPromptReason(missing, englishDriverStore) ==
            Ch341DriverPromptReason.DeviceProblemCode28,
            "设备 Code 28 且 Driver Store 探测成功时提示原因必须明确为设备 Problem Code 28");
        Require(
            DriverInstallPrompt.BuildMessage(
                    new Ch341DriverPackage("C:\\drivers\\CH341SER.INF"),
                    Ch341DriverPromptReason.DriverStoreMissing)
                .Contains("Driver Store", StringComparison.Ordinal),
            "Driver Store 缺失提示不得复用设备 Code 28 文案");
        string vendorPrompt = DriverInstallPrompt.BuildMessage(
            new Ch341DriverPackage("C:\\drivers\\CH341SER.INF"),
            Ch341DriverPromptReason.DriverStoreMissing);
        Require(
            vendorPrompt.Contains("SETUP.EXE /S", StringComparison.Ordinal) &&
            vendorPrompt.Contains("/U", StringComparison.Ordinal),
            "自动安装提示必须说明使用 WCH 官方安装器并可由同一安装器卸载");

        Ch341ProbeResult working = missing with
        {
            State = Ch341DeviceState.Working,
            Device = missing.Device! with
            {
                State = Ch341DeviceState.Working,
                ProblemCode = 0,
            },
        };
        Require(
            Ch341StartupPolicy.Decide(working, packageAvailable: true, englishDriverStore) == Ch341StartupAction.None,
            "Working 设备即使重新探测 Driver Store 也不得弹安装提示");
        Require(
            Ch341StartupPolicy.DecidePostInstall(
                Ch341ProbeResult.NoDevice(),
                null,
                englishDriverStore) == Ch341PostInstallAction.DriverPackageStagedNoDevice,
            "安装后无设备但 Driver Store 已有包时应报告仅完成驱动包入库而不虚报 COM");
        Require(
            Ch341StartupPolicy.DecidePostInstall(
                Ch341ProbeResult.NoDevice(),
                null,
                driverStoreProbeFailed) == Ch341PostInstallAction.VerificationFailed,
            "安装后无设备且 Driver Store 探测失败时不得宣称安装完成");
        Require(
            Ch341StartupPolicy.DecidePostInstall(working, "COM6", englishDriverStore) ==
            Ch341PostInstallAction.WorkingCom,
            "安装后 Working 且匹配 COM 时应报告完整复检通过");

        string? port = Ch341StartupPolicy.TryGetComPort(
            new Ch341DeviceInfo(
                "USB\\VID_1A86&PID_7523\\TEST",
                "USB-SERIAL CH340 (COM6)",
                Ch341DeviceState.Working,
                0,
                0,
                null),
            () => ["COM6", "COM7"]);
        Require(port == "COM6", "Working CH340 的 friendly name 与 SerialPort 列表应能匹配 COM6");
        Require(
            Ch341StartupPolicy.TryGetComPort(
                new Ch341DeviceInfo(
                    "USB\\VID_1A86&PID_7523\\TEST",
                    "USB-SERIAL CH340 (COM6)",
                    Ch341DeviceState.Working,
                    0,
                    0,
                    null),
                () => ["COM7"]) is null,
            "SerialPort 列表没有 friendly name 中的 COM 时不得宣称已确认");
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }

    Console.WriteLine(
        "CH340/CH341 驱动支持检查：Problem Code 0/28/其他码分类、" +
        "Driver Store 中英文样本解析、新旧驱动包路径、WCH SETUP /S 安装与 /U 卸载语义、" +
        "无设备/Code 28/Working 安装提示策略、驱动包入库语义和安装后继续自动发现策略均通过；未执行真实 PnP、UAC 或驱动安装。");
}

static void CheckCh341DriverStoreProbeReadOnly()
{
    Ch341DriverStoreProbeResult result = new WindowsCh341DriverStoreProbe().Probe();
    Console.WriteLine(
        $"只读 Driver Store 探测：State={result.State}；" +
        $"MatchedOriginalName={result.MatchedOriginalName ?? "<none>"}；" +
        $"Error={result.Error ?? "<none>"}；未执行安装、UAC 或设备修改。");
    Require(
        result.State is Ch341DriverStoreState.Installed or Ch341DriverStoreState.Missing,
        $"只读 Driver Store 探测失败：{result.Error ?? "未知错误"}");
}

static void CheckWin32MouseReleasePlan()
{
    MouseReleasePlanEntry[] empty =
        Win32AutomationOutput.BuildMouseReleasePlan(Array.Empty<int>()).ToArray();
    Require(empty.Length == 0, "没有按下鼠标键时释放计划不得生成任何鼠标 Up");

    MouseReleasePlanEntry[] right =
        Win32AutomationOutput.BuildMouseReleasePlan([3]).ToArray();
    Require(
        right.SequenceEqual([new MouseReleasePlanEntry(3, 0x0010, 0)]),
        "仅右键按下时释放计划必须只生成右键 Up");

    MouseReleasePlanEntry[] side1 =
        Win32AutomationOutput.BuildMouseReleasePlan([4]).ToArray();
    Require(
        side1.SequenceEqual([new MouseReleasePlanEntry(4, 0x0100, 0x0001)]),
        "仅 XButton1 按下时释放计划必须只生成对应侧键 Up");

    MouseReleasePlanEntry[] side2 =
        Win32AutomationOutput.BuildMouseReleasePlan([5]).ToArray();
    Require(
        side2.SequenceEqual([new MouseReleasePlanEntry(5, 0x0100, 0x0002)]),
        "仅 XButton2 按下时释放计划必须只生成对应侧键 Up");

    MouseReleasePlanEntry[] multiple =
        Win32AutomationOutput.BuildMouseReleasePlan([5, 1, 4, 3, 4]).ToArray();
    Require(
        multiple.SequenceEqual([
            new MouseReleasePlanEntry(1, 0x0004, 0),
            new MouseReleasePlanEntry(3, 0x0010, 0),
            new MouseReleasePlanEntry(4, 0x0100, 0x0001),
            new MouseReleasePlanEntry(5, 0x0100, 0x0002),
        ]),
        "多个已按按钮的释放计划必须准确生成、去重且不得多发");

    Console.WriteLine("Win32 鼠标释放计划检查：空集合、右键、XButton1、XButton2及多按钮均只生成已跟踪的对应 Up。");
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

static void CheckLogFileRetention()
{
    BridgeOptions defaults = new();
    Require(defaults.HostLogPath.StartsWith("log/host/", StringComparison.Ordinal), "主机日志默认目录必须位于 EXE 同目录 log/host");
    Require(defaults.DeviceLogPath.StartsWith("log/device/", StringComparison.Ordinal), "设备日志默认目录必须位于 EXE 同目录 log/device");
    Require(defaults.AutomationLogPath.StartsWith("log/automation/", StringComparison.Ordinal), "自动化日志默认目录必须位于 EXE 同目录 log/automation");
    Require(
        defaults.HostLogRetentionCount == LogFileRetention.DefaultMaxFileCount &&
        defaults.DeviceLogRetentionCount == LogFileRetention.DefaultMaxFileCount &&
        defaults.AutomationLogRetentionCount == LogFileRetention.DefaultMaxFileCount,
        "三类日志默认保留数量必须一致且启用限制");

    string directory = Path.Combine(Path.GetTempPath(), $"hidbridge-log-retention-{Guid.NewGuid():N}");
    string template = Path.Combine(directory, "runtime-{timestamp}.log");
    Directory.CreateDirectory(directory);
    try
    {
        string[] oldFiles =
        [
            Path.Combine(directory, "runtime-20200101-000001.log"),
            Path.Combine(directory, "runtime-20200101-000002.log"),
            Path.Combine(directory, "runtime-20200101-000003.log"),
        ];
        for (int index = 0; index < oldFiles.Length; index++)
        {
            File.WriteAllText(oldFiles[index], $"old-{index}");
            File.SetLastWriteTimeUtc(oldFiles[index], DateTime.UtcNow.AddMinutes(-3 + index));
        }

        string current = Path.Combine(directory, "runtime-20200101-000004.log");
        LogFileRetention.Enforce(template, current, maxFileCount: 3);
        File.WriteAllText(current, "current");
        string[] retained = Directory.GetFiles(directory, "runtime-*.log");
        Require(retained.Length == 3, $"日志数量限制未生效：实际保留 {retained.Length} 个文件");
        Require(!File.Exists(oldFiles[0]), "日志数量限制应优先删除最旧文件");
        Console.WriteLine("日志目录与数量限制检查：三类默认目录、默认保留数量和最旧文件清理均符合预期。");
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
        input);
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
    byte[] alwaysOutput = Encoding.UTF8.GetBytes(
        "{\"dx\":5,\"dy\":5}");
    client.Send(alwaysOutput, alwaysOutput.Length, "127.0.0.1", server.Port);
    deadline = DateTime.UtcNow.AddSeconds(2);
    while (DateTime.UtcNow < deadline &&
           transport.MouseReports().Sum(report => (long)report.X) < 42)
    {
        Thread.Sleep(5);
    }
    Require(
        transport.MouseReports().Sum(report => (long)report.X) == 42,
        "默认始终 UDP 输出开启时，同步关闭后仍应发送 UDP 模拟输入");

    input.ConfigureAlwaysOutputUdp(false);
    int disabledReportCount = transport.MouseReports().Length;
    byte[] disabled = Encoding.UTF8.GetBytes(
        "{\"dx\":6,\"dy\":6}");
    client.Send(disabled, disabled.Length, "127.0.0.1", server.Port);
    Thread.Sleep(50);
    Require(
        transport.MouseReports().Length == disabledReportCount,
        "关闭始终 UDP 输出后，同步关闭时不得继续发送 UDP 模拟输入");
}

static void CheckKmboxNetCompatibility()
{
    DisplayAddressCandidate[] addressCandidates =
    [
        new(System.Net.IPAddress.Parse("172.31.255.1"), HasDefaultGateway: false, IsVirtual: true),
        new(System.Net.IPAddress.Parse("192.168.3.47"), HasDefaultGateway: true, IsVirtual: false),
        new(System.Net.IPAddress.Parse("169.254.157.120"), HasDefaultGateway: false, IsVirtual: false),
    ];
    Require(
        RemoteInputServer.SelectDisplayAddress(
            addressCandidates,
            System.Net.IPAddress.Parse("172.31.255.1")).Equals(System.Net.IPAddress.Parse("192.168.3.47")),
        "局域网显示地址必须优先选择带默认网关的实体网卡，而不是 VPN/TUN 的 172 地址");
    DisplayAddressCandidate[] hyperVHostCandidates =
    [
        new(System.Net.IPAddress.Parse("192.168.3.50"), HasDefaultGateway: true, IsVirtual: true),
        new(System.Net.IPAddress.Parse("172.22.112.1"), HasDefaultGateway: false, IsVirtual: true),
    ];
    Require(
        RemoteInputServer.SelectDisplayAddress(
            hyperVHostCandidates,
            System.Net.IPAddress.Parse("192.168.3.50")).Equals(System.Net.IPAddress.Parse("192.168.3.50")),
        "承载默认网关的 Hyper-V vEthernet 必须作为有效局域网地址，不能回退到 127.0.0.1");

    const uint mac = 0xaf425414;
    RecordingTransport transport = new();
    using InputForwarder input = new(transport);
    using (RemoteInputServer displayServer = new("0.0.0.0", 0, input))
    {
        Console.WriteLine($"当前机器通配监听展示地址：{displayServer.DisplayEndpoint}");
    }
    input.SetForwardingEnabled(true);
    using RemoteInputServer server = new("127.0.0.1", 0, input);
    server.Start();
    using UdpClient client = new();
    client.Client.ReceiveTimeout = 2000;

    byte[] connect = BuildKmboxPacket(mac, 0x11223344, 0, KmboxNetProtocol.ConnectCommand, 16);
    SendKmboxAndRequireAck(client, server.Port, connect, 0, KmboxNetProtocol.ConnectCommand);

    byte[] autoMove = BuildKmboxPacket(mac, 20, 1, KmboxNetProtocol.MouseAutoMoveCommand, 72);
    BinaryPrimitives.WriteInt32LittleEndian(autoMove.AsSpan(16), 1);
    BinaryPrimitives.WriteInt32LittleEndian(autoMove.AsSpan(20), 12);
    BinaryPrimitives.WriteInt32LittleEndian(autoMove.AsSpan(24), -7);
    SendKmboxAndRequireAck(client, server.Port, autoMove, 1, KmboxNetProtocol.MouseAutoMoveCommand);

    byte[] bezierMove = BuildKmboxPacket(mac, 30, 2, KmboxNetProtocol.BezierMoveCommand, 72);
    BinaryPrimitives.WriteInt32LittleEndian(bezierMove.AsSpan(16), 1);
    BinaryPrimitives.WriteInt32LittleEndian(bezierMove.AsSpan(20), -2);
    BinaryPrimitives.WriteInt32LittleEndian(bezierMove.AsSpan(24), 3);
    SendKmboxAndRequireAck(client, server.Port, bezierMove, 2, KmboxNetProtocol.BezierMoveCommand);

    DateTime deadline = DateTime.UtcNow.AddSeconds(2);
    while (DateTime.UtcNow < deadline &&
           (transport.MouseReports().Sum(report => (long)report.X) != 10 ||
            transport.MouseReports().Sum(report => (long)report.Y) != -4))
    {
        Thread.Sleep(5);
    }
    MouseReport[] mouseReports = transport.MouseReports();
    Require(mouseReports.Sum(report => (long)report.X) == 10, "kmboxNet move_auto/bezier X 未统一进入现有 move 链路");
    Require(mouseReports.Sum(report => (long)report.Y) == -4, "kmboxNet move_auto/bezier Y 未统一进入现有 move 链路");
    Require(mouseReports.Any(report => (report.Buttons & 1) != 0), "kmboxNet 鼠标按钮状态未进入 HID 报告");

    byte[] keyboard = BuildKmboxPacket(mac, 0x55667788, 3, KmboxNetProtocol.KeyboardAllCommand, 28);
    keyboard[16] = 1;
    keyboard[18] = 4;
    SendKmboxAndRequireAck(client, server.Port, keyboard, 3, KmboxNetProtocol.KeyboardAllCommand);
    deadline = DateTime.UtcNow.AddSeconds(2);
    while (DateTime.UtcNow < deadline && transport.KeyboardReports().Length == 0)
    {
        Thread.Sleep(5);
    }
    byte[] keyboardReport = transport.KeyboardReports().Last();
    Require(keyboardReport[0] == 1 && keyboardReport.Contains((byte)4), "kmboxNet 键盘完整状态未进入现有 HID 接口");

    byte[] traceOff = BuildKmboxPacket(mac, 0, 4, KmboxNetProtocol.TraceCommand, 16);
    SendKmboxAndRequireAck(client, server.Port, traceOff, 4, KmboxNetProtocol.TraceCommand);
    Require(!input.UdpSmoothingEnabled, "kmboxNet trace(0, 0) 未关闭当前 UDP 平滑");
    byte[] traceOn = BuildKmboxPacket(mac, (3u << 24) | 80, 5, KmboxNetProtocol.TraceCommand, 16);
    SendKmboxAndRequireAck(client, server.Port, traceOn, 5, KmboxNetProtocol.TraceCommand);
    Require(input.UdpSmoothingEnabled, "kmboxNet trace(3, 80) 未开启当前 UDP 平滑");

    using UdpClient monitor = new(new System.Net.IPEndPoint(System.Net.IPAddress.Loopback, 0));
    monitor.Client.ReceiveTimeout = 2000;
    int monitorPort = ((System.Net.IPEndPoint)monitor.Client.LocalEndPoint!).Port;
    byte[] monitorCommand = BuildKmboxPacket(
        mac,
        0xaa550000u | unchecked((ushort)monitorPort),
        6,
        KmboxNetProtocol.MonitorCommand,
        16);
    SendKmboxAndRequireAck(client, server.Port, monitorCommand, 6, KmboxNetProtocol.MonitorCommand);
    input.ProcessRawMouseInputForChecks(new NativeMethods.RawMouse
    {
        Buttons = NativeMethods.RawMouseButton4Down,
    });
    System.Net.IPEndPoint monitorSource = new(System.Net.IPAddress.Any, 0);
    byte[] monitorReport = monitor.Receive(ref monitorSource);
    Require(monitorReport.Length == 21, "kmboxNet monitor 回传必须为 21 字节鼠标+键盘状态");
    Require((monitorReport[1] & 0x08) != 0, "kmboxNet isdown_side1 所需的实体侧键状态未回传");

    byte[] mask = BuildKmboxPacket(mac, 0x00000421, 7, KmboxNetProtocol.MaskCommand, 16);
    SendKmboxAndRequireAck(client, server.Port, mask, 7, KmboxNetProtocol.MaskCommand);
    Require(input.KmboxMouseMaskForChecks == 0x21, "kmboxNet 鼠标 mask 位图未应用");
    Require(input.IsKmboxKeyMaskedForChecks(4), "kmboxNet mask_keyboard 未应用");
    byte[] unmaskKey = BuildKmboxPacket(mac, 0x00000421, 8, KmboxNetProtocol.UnmaskCommand, 16);
    SendKmboxAndRequireAck(client, server.Port, unmaskKey, 8, KmboxNetProtocol.UnmaskCommand);
    Require(!input.IsKmboxKeyMaskedForChecks(4), "kmboxNet unmask_keyboard 未应用");
    byte[] unmaskAll = BuildKmboxPacket(mac, 0, 9, KmboxNetProtocol.UnmaskCommand, 16);
    SendKmboxAndRequireAck(client, server.Port, unmaskAll, 9, KmboxNetProtocol.UnmaskCommand);
    Require(input.KmboxMouseMaskForChecks == 0, "kmboxNet unmask_all 未清除鼠标 mask");

    byte[] encryptedMouse = BuildKmboxPacket(mac, 0x12345678, 10, KmboxNetProtocol.MouseMoveCommand, 128);
    BinaryPrimitives.WriteInt32LittleEndian(encryptedMouse.AsSpan(20), 5);
    BinaryPrimitives.WriteInt32LittleEndian(encryptedMouse.AsSpan(24), -5);
    EncryptKmboxPacket(encryptedMouse, mac);
    SendKmboxAndRequireAck(client, server.Port, encryptedMouse, 10, KmboxNetProtocol.MouseMoveCommand);

    Console.WriteLine(
        "kmboxNet 兼容检查：实体网卡优先且支持带网关的 Hyper-V LAN、init/ACK、明文与加密输入、move_auto/bezier 统一移动、" +
        "键盘、trace 平滑映射、monitor 21 字节状态、mask/unmask 均通过纯逻辑/记录传输验证。");
}

static byte[] BuildKmboxPacket(uint mac, uint random, uint index, uint command, int length)
{
    byte[] packet = new byte[length];
    BinaryPrimitives.WriteUInt32LittleEndian(packet, mac);
    BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(4), random);
    BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(8), index);
    BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(12), command);
    return packet;
}

static void SendKmboxAndRequireAck(
    UdpClient client,
    int serverPort,
    byte[] packet,
    uint expectedIndex,
    uint expectedCommand)
{
    client.Send(packet, packet.Length, "127.0.0.1", serverPort);
    System.Net.IPEndPoint source = new(System.Net.IPAddress.Any, 0);
    byte[] acknowledgement = client.Receive(ref source);
    Require(acknowledgement.Length == 16, "kmboxNet ACK 必须为 16 字节");
    Require(BinaryPrimitives.ReadUInt32LittleEndian(acknowledgement.AsSpan(8)) == expectedIndex, "kmboxNet ACK index 不匹配");
    Require(BinaryPrimitives.ReadUInt32LittleEndian(acknowledgement.AsSpan(12)) == expectedCommand, "kmboxNet ACK cmd 不匹配");
}

static void EncryptKmboxPacket(Span<byte> packet, uint mac)
{
    Span<uint> values = stackalloc uint[32];
    for (int index = 0; index < values.Length; index++)
    {
        values[index] = BinaryPrimitives.ReadUInt32LittleEndian(packet[(index * 4)..]);
    }
    Span<uint> key = stackalloc uint[4];
    key[0] = BinaryPrimitives.ReverseEndianness(mac);
    const uint delta = 2654435769;
    uint sum = 0;
    uint z = values[^1];
    for (int round = 0; round < 6; round++)
    {
        sum = unchecked(sum + delta);
        uint e = (sum >> 2) & 3;
        int position;
        for (position = 0; position < values.Length - 1; position++)
        {
            uint y = values[position + 1];
            z = values[position] = unchecked(values[position] + KmboxMix(sum, y, z, position, e, key));
        }
        uint first = values[0];
        z = values[^1] = unchecked(values[^1] + KmboxMix(sum, first, z, position, e, key));
    }
    for (int index = 0; index < values.Length; index++)
    {
        BinaryPrimitives.WriteUInt32LittleEndian(packet[(index * 4)..], values[index]);
    }
}

static uint KmboxMix(uint sum, uint y, uint z, int position, uint e, ReadOnlySpan<uint> key) =>
    unchecked(((z >> 5 ^ y << 2) + (y >> 3 ^ z << 4)) ^
        ((sum ^ y) + (key[(position & 3) ^ (int)e] ^ z)));

static void CheckFirmwareUpdateApiPolicy()
{
    Require(!new AutomationSettings().FirmwareUpdateApiEnabled, "局域网固件刷写接口必须默认关闭");
    Require(!new AutomationSettings().LegacySingleBoardFirmwareCompatibility, "旧版单板固件兼容必须默认关闭");
    Require(string.IsNullOrEmpty(new AutomationSettings().FirmwareManifestPath), "本地固件 JSON 默认不得指向隐式镜像");
    Require(string.IsNullOrEmpty(new AutomationSettings().FirmwareFlashPortName), "本地刷写串口默认不得写死");
    Require(SerialBridge.NormalizeFirmwarePortName(" com03 ") == "COM3", "刷写串口名称未规范化");
    bool invalidPortRejected = false;
    try
    {
        _ = SerialBridge.NormalizeFirmwarePortName("USB0");
    }
    catch (ArgumentException)
    {
        invalidPortRejected = true;
    }
    Require(invalidPortRejected, "刷写串口必须拒绝非 COM 名称");
    Require(
        SettingsPageControl.SelectFirmwarePort("COM7", "COM3", ["COM3", "COM7"]) == "COM3",
        "刷写串口默认选择未优先使用当前连接");
    Require(
        SettingsPageControl.SelectFirmwarePort("COM7", null, ["COM3", "COM7"]) == "COM7",
        "没有当前连接时未复用仍存在的上次选择");
    Require(
        SettingsPageControl.SelectFirmwarePort("COM9", null, ["COM3", "COM7"]) == "COM3",
        "上次选择已消失时未回退到端口列表首项");
    string multiplePortConfirmation = SettingsPageControl.BuildFirmwareFlashConfirmation(
        "D:\\firmware\\flasher_args.json",
        "COM7",
        ["COM3", "COM7"]);
    Require(
        multiplePortConfirmation.Contains("检测到多个串口：COM3、COM7", StringComparison.Ordinal) &&
        multiplePortConfirmation.Contains("只会刷写已选择的 COM7", StringComparison.Ordinal),
        "多串口刷写确认未列出全部端口和当前选择");
    BridgeOptions options = new();
    Require(options.FirmwareUpdateApiPort == 24815, "局域网固件刷写接口默认端口应为 24815");
    BridgeOptions.Validate(options);
    string manifestPath = Path.GetFullPath("firmware/build/flasher_args.json");
    FirmwareFlashPlan plan = FirmwareFlashPlan.LoadFromManifest(manifestPath);
    Require(plan.Images.Count == 3, "固件刷写计划必须包含三段镜像");
    Require(
        plan.Images.Select(image => image.Offset).SequenceEqual([0L, 0x8000L, 0x10000L]),
        "固件刷写计划偏移必须为 0x0、0x8000、0x10000");
    Require(plan.Images.All(image => File.Exists(image.Path) && image.Sha256.Length == 64), "固件镜像路径或 SHA-256 无效");
    Require(File.Exists(plan.EsptoolPath), "固件刷写计划未定位到项目内 esptool.exe");
    Require(plan.Before == "default-reset" && plan.After == "hard-reset", "esptool 5 复位参数未规范化为连字符形式");
    Require(
        !typeof(EmbeddedFlashAssets).Assembly.GetManifestResourceNames()
            .Any(name => name.StartsWith("HidBridge.Host.assets.firmware.", StringComparison.Ordinal)),
        "Host EXE 不得继续内嵌固件 JSON 或镜像");

    string customDirectory = Path.Combine(Path.GetTempPath(), $"hidbridge-flash-plan-{Guid.NewGuid():N}");
    try
    {
        Directory.CreateDirectory(Path.Combine(customDirectory, "bootloader"));
        Directory.CreateDirectory(Path.Combine(customDirectory, "partition_table"));
        File.Copy(manifestPath, Path.Combine(customDirectory, "remote-selected.json"));
        File.Copy(plan.Images[0].Path, Path.Combine(customDirectory, "bootloader", "bootloader.bin"));
        File.Copy(plan.Images[1].Path, Path.Combine(customDirectory, "partition_table", "partition-table.bin"));
        File.Copy(plan.Images[2].Path, Path.Combine(customDirectory, "esp32_s3_hid_bridge.bin"));
        FirmwareFlashPlan customPlan = FirmwareFlashPlan.LoadFromManifest(
            Path.Combine(customDirectory, "remote-selected.json"));
        Require(
            customPlan.Images.All(image => image.Path.StartsWith(customDirectory, StringComparison.OrdinalIgnoreCase)),
            "自定义 JSON 的相对镜像路径未按 JSON 所在目录解析");
    }
    finally
    {
        if (Directory.Exists(customDirectory))
        {
            Directory.Delete(customDirectory, recursive: true);
        }
    }
    string summary = FirmwareFlashService.FormatVerificationSummary(plan.Images, 3, hardReset: true);
    Require(plan.Images.All(image => summary.Contains($"{image.OffsetArgument}={image.Sha256}", StringComparison.Ordinal)), "刷写最终摘要缺少三段 SHA-256");
    Require(summary.Contains("设备校验=3/3", StringComparison.Ordinal) && summary.EndsWith("RTS复位=完成", StringComparison.Ordinal), "刷写最终摘要缺少校验或复位结果");

    string validRequest =
        "POST /api/v1/firmware/flash HTTP/1.1\r\n" +
        "Host: 127.0.0.1:24815\r\n" +
        $"{FirmwareUpdateApiServer.ConfirmationHeaderName}: {FirmwareUpdateApiServer.ConfirmationHeaderValue}\r\n" +
        "Content-Length: 64\r\n\r\n";
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

static void CheckFirmwareUpdateApiLan()
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
    string? requestedManifestPath = null;
    string? requestedPortName = null;
    bool TryStart(string manifestPath, string portName, out FirmwareFlashSnapshot current)
    {
        Interlocked.Increment(ref started);
        requestedManifestPath = manifestPath;
        requestedPortName = portName;
        current = snapshot with { State = "running" };
        return true;
    }

    string selectedPort = string.Empty;
    using FirmwareUpdateApiServer server = new(0, () => snapshot, TryStart, () => selectedPort);
    Require(server.ListenAddress.Equals(System.Net.IPAddress.Any), "固件刷写 API 必须绑定全部 IPv4 接口");
    server.SetEnabled(true);
    Require(server.Enabled && server.Port > 0, "固件刷写 API 未在临时局域网端口启动");
    using HttpClient client = new() { BaseAddress = new Uri($"http://127.0.0.1:{server.Port}") };

    HttpResponseMessage statusResponse = client.GetAsync("/api/v1/firmware/status").GetAwaiter().GetResult();
    Require(statusResponse.StatusCode == System.Net.HttpStatusCode.OK, "固件刷写状态接口未返回 200");
    string statusJson = statusResponse.Content.ReadAsStringAsync().GetAwaiter().GetResult();
    Require(statusJson.Contains("\"state\":\"idle\"", StringComparison.Ordinal), "固件刷写状态响应内容不正确");

    HttpResponseMessage rejected = client.PostAsync("/api/v1/firmware/flash", null).GetAwaiter().GetResult();
    Require(rejected.StatusCode == System.Net.HttpStatusCode.Forbidden, "缺少确认头的刷写请求必须返回 403");
    Require(started == 0, "缺少确认头时不得调用刷写任务");

    using HttpRequestMessage missingManifest = new(HttpMethod.Post, "/api/v1/firmware/flash");
    missingManifest.Headers.Add(
        FirmwareUpdateApiServer.ConfirmationHeaderName,
        FirmwareUpdateApiServer.ConfirmationHeaderValue);
    HttpResponseMessage missingManifestResponse = client.Send(missingManifest);
    Require(
        missingManifestResponse.StatusCode == System.Net.HttpStatusCode.BadRequest,
        "远程刷写未指定 JSON 路径时必须返回 400");
    Require(started == 0, "缺少 JSON 路径时不得调用刷写任务");

    using HttpRequestMessage missingPort = new(HttpMethod.Post, "/api/v1/firmware/flash");
    missingPort.Headers.Add(FirmwareUpdateApiServer.ConfirmationHeaderName, FirmwareUpdateApiServer.ConfirmationHeaderValue);
    string manifestPath = Path.GetFullPath("firmware/build/flasher_args.json");
    missingPort.Content = new StringContent(
        JsonSerializer.Serialize(new { manifestPath }),
        Encoding.UTF8,
        "application/json");
    HttpResponseMessage missingPortResponse = client.Send(missingPort);
    Require(missingPortResponse.StatusCode == System.Net.HttpStatusCode.BadRequest, "设置页未选择串口时远程刷写必须返回 400");
    Require(started == 0, "设置页未选择串口时不得调用刷写任务");

    selectedPort = "COM7";
    using HttpRequestMessage request = new(HttpMethod.Post, "/api/v1/firmware/flash");
    request.Headers.Add(FirmwareUpdateApiServer.ConfirmationHeaderName, FirmwareUpdateApiServer.ConfirmationHeaderValue);
    request.Content = new StringContent(
        JsonSerializer.Serialize(new { manifestPath }),
        Encoding.UTF8,
        "application/json");
    HttpResponseMessage accepted = client.Send(request);
    Require(accepted.StatusCode == System.Net.HttpStatusCode.Accepted, "合法刷写请求未返回 202");
    Require(started == 1, "合法刷写请求必须且只能启动一次任务");
    Require(requestedManifestPath == manifestPath, "远程刷写 API 未将请求指定的 JSON 路径传给底层刷写工具");
    Require(requestedPortName == "COM7", "远程刷写 API 未使用设置页已选择的串口");
    server.SetEnabled(false);
    Require(!server.Enabled, "设置关闭后固件刷写 API 必须停止监听");
}

static void CheckSerialForwardingApi()
{
    string[] heldPorts = ["COM12", "COM3"];
    List<(string PortName, byte[] Bytes)> writes = [];
    bool failWrite = false;
    int refreshCalls = 0;

    int? FakeWrite(string portName, byte[] bytes, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (!heldPorts.Contains(portName, StringComparer.OrdinalIgnoreCase))
        {
            return null;
        }
        if (failWrite)
        {
            throw new IOException("fake serial write failure");
        }
        writes.Add((portName, bytes.ToArray()));
        return bytes.Length;
    }

    FirmwareFlashSnapshot snapshot = new("serial-api-check", "idle", "test", null, null, null, null);
    using FirmwareUpdateApiServer server = new(
        0,
        () => snapshot,
        (string _, string _, out FirmwareFlashSnapshot current) =>
        {
            current = snapshot;
            return false;
        },
        () => null,
        () => heldPorts,
        FakeWrite,
        cancellationToken =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            refreshCalls++;
            return new SerialPortRefreshSnapshot(["COM3", "COM12"], heldPorts);
        });
    server.SetEnabled(false);
    Require(!server.Enabled && server.Port > 0, "关闭固件 API 时仍必须为串口 API 监听");
    using HttpClient client = new() { BaseAddress = new Uri($"http://127.0.0.1:{server.Port}") };

    HttpResponseMessage disabledFirmware = client.GetAsync("/api/v1/firmware/status").GetAwaiter().GetResult();
    Require(disabledFirmware.StatusCode == System.Net.HttpStatusCode.NotFound, "固件 API 关闭时其路由必须不可用");

    HttpResponseMessage portsResponse = client.GetAsync("/api/v1/serial/ports").GetAwaiter().GetResult();
    Require(portsResponse.StatusCode == System.Net.HttpStatusCode.OK, "串口列表路由必须返回 200");
    using (JsonDocument portsJson = JsonDocument.Parse(portsResponse.Content.ReadAsStringAsync().GetAwaiter().GetResult()))
    {
        string[] ports = portsJson.RootElement.GetProperty("ports").EnumerateArray()
            .Select(value => value.GetString()!)
            .ToArray();
        Require(ports.SequenceEqual(heldPorts), "串口列表必须只返回 fake EXE 当前持有的两个端口");
    }

    HttpResponseMessage refreshResponse = client.PostAsync("/api/v1/serial/refresh", null).GetAwaiter().GetResult();
    Require(refreshResponse.StatusCode == System.Net.HttpStatusCode.OK, "串口刷新路由必须返回 200");
    using (JsonDocument refreshJson = JsonDocument.Parse(refreshResponse.Content.ReadAsStringAsync().GetAwaiter().GetResult()))
    {
        string[] available = refreshJson.RootElement.GetProperty("availablePorts").EnumerateArray()
            .Select(value => value.GetString()!)
            .ToArray();
        string[] open = refreshJson.RootElement.GetProperty("openPorts").EnumerateArray()
            .Select(value => value.GetString()!)
            .ToArray();
        Require(available.SequenceEqual(["COM3", "COM12"]) && open.SequenceEqual(heldPorts),
            "刷新响应必须返回刷新后的可用端口与 EXE 持有端口");
    }
    Require(refreshCalls == 1, "POST 串口刷新必须执行一次注入的刷新流程");

    HttpResponseMessage unknownRoute = client.GetAsync("/api/v1/serial/unknown").GetAwaiter().GetResult();
    Require(unknownRoute.StatusCode == System.Net.HttpStatusCode.NotFound, "未注册路由必须返回 404");

    HttpResponseMessage missingBody = client.PostAsync("/api/v1/serial/write", null).GetAwaiter().GetResult();
    Require(missingBody.StatusCode == System.Net.HttpStatusCode.BadRequest, "缺少写入 JSON 时必须返回 400");

    HttpResponseMessage invalidJson = client.PostAsync(
        "/api/v1/serial/write",
        new StringContent("{", Encoding.UTF8, "application/json")).GetAwaiter().GetResult();
    Require(invalidJson.StatusCode == System.Net.HttpStatusCode.BadRequest, "无效 JSON 必须返回 400");

    HttpResponseMessage invalidPort = client.PostAsync(
        "/api/v1/serial/write",
        new StringContent("{\"portName\":\"COMx\",\"hex\":\"00\"}", Encoding.UTF8, "application/json"))
        .GetAwaiter().GetResult();
    Require(invalidPort.StatusCode == System.Net.HttpStatusCode.BadRequest, "非法 COM 名必须返回 400");

    HttpResponseMessage invalidHex = client.PostAsync(
        "/api/v1/serial/write",
        new StringContent("{\"portName\":\"COM12\",\"hex\":\"0G\"}", Encoding.UTF8, "application/json"))
        .GetAwaiter().GetResult();
    Require(invalidHex.StatusCode == System.Net.HttpStatusCode.BadRequest, "非十六进制内容必须返回 400");

    HttpResponseMessage oddHex = client.PostAsync(
        "/api/v1/serial/write",
        new StringContent("{\"portName\":\"COM12\",\"hex\":\"ABC\"}", Encoding.UTF8, "application/json"))
        .GetAwaiter().GetResult();
    Require(oddHex.StatusCode == System.Net.HttpStatusCode.BadRequest, "奇数位十六进制必须返回 400");

    HttpResponseMessage emptyHex = client.PostAsync(
        "/api/v1/serial/write",
        new StringContent("{\"portName\":\"COM12\",\"hex\":\"\"}", Encoding.UTF8, "application/json"))
        .GetAwaiter().GetResult();
    Require(emptyHex.StatusCode == System.Net.HttpStatusCode.BadRequest, "空写入必须返回 400");

    HttpResponseMessage tooLarge = client.PostAsync(
        "/api/v1/serial/write",
        new StringContent(
            JsonSerializer.Serialize(new { portName = "COM12", hex = new string('A', 8194) }),
            Encoding.UTF8,
            "application/json")).GetAwaiter().GetResult();
    Require(tooLarge.StatusCode == System.Net.HttpStatusCode.RequestEntityTooLarge, "超过 4096 字节必须返回 413");

    HttpResponseMessage notOpen = client.PostAsync(
        "/api/v1/serial/write",
        new StringContent("{\"portName\":\"COM44\",\"hex\":\"00\"}", Encoding.UTF8, "application/json"))
        .GetAwaiter().GetResult();
    Require(notOpen.StatusCode == System.Net.HttpStatusCode.Conflict, "EXE 未持有的端口必须返回 409");
    Require(writes.Count == 0, "参数错误或未连接端口不得调用 fake writer");

    byte[] frame = new FrameCodec().Encode(MessageType.Ping, ReadOnlySpan<byte>.Empty);
    string hex = Convert.ToHexString(frame);
    HttpResponseMessage success = client.PostAsync(
        "/api/v1/serial/write",
        new StringContent(
            JsonSerializer.Serialize(new { portName = "COM12", hex }),
            Encoding.UTF8,
            "application/json")).GetAwaiter().GetResult();
    Require(success.StatusCode == System.Net.HttpStatusCode.OK, "已连接端口成功写入必须返回 200");
    using (JsonDocument successJson = JsonDocument.Parse(success.Content.ReadAsStringAsync().GetAwaiter().GetResult()))
    {
        JsonElement root = successJson.RootElement;
        Require(root.GetProperty("portName").GetString() == "COM12", "成功响应的 portName 不正确");
        Require(root.GetProperty("bytesWritten").GetInt32() == frame.Length, "成功响应的 bytesWritten 不正确");
        Require(!root.GetProperty("queued").GetBoolean(), "同步写入响应必须 queued=false");
    }
    Require(
        writes.Count == 1 && writes[0].PortName == "COM12" && writes[0].Bytes.SequenceEqual(frame),
        "fake writer 必须收到完全相同的目标端口和帧字节");

    failWrite = true;
    HttpResponseMessage writeFailure = client.PostAsync(
        "/api/v1/serial/write",
        new StringContent(
            JsonSerializer.Serialize(new { portName = "COM12", hex }),
            Encoding.UTF8,
            "application/json")).GetAwaiter().GetResult();
    Require(writeFailure.StatusCode == System.Net.HttpStatusCode.InternalServerError, "写入异常必须返回 500");
    string failureJson = writeFailure.Content.ReadAsStringAsync().GetAwaiter().GetResult();
    Require(failureJson.Contains("fake serial write failure", StringComparison.Ordinal), "写入失败响应必须保留明确原因");

    Require(
        FirmwareUpdateApiServer.IsLoopbackAddress(System.Net.IPAddress.Loopback) &&
        !FirmwareUpdateApiServer.IsLoopbackAddress(System.Net.IPAddress.Parse("192.0.2.10")) &&
        FirmwareUpdateApiServer.IsLoopbackAddress(System.Net.IPAddress.Parse("::ffff:127.0.0.1")),
        "串口 API loopback 地址判定错误");
    server.SetEnabled(false);
}

static void CheckDeviceLogMirrorPerPortFile()
{
    string root = Path.Combine(Path.GetTempPath(), $"hidbridge-mirror-log-{Guid.NewGuid():N}");
    Directory.CreateDirectory(root);
    try
    {
        string hostLogPath = Path.Combine(root, "host-runtime-{timestamp}.log");
        DateTime timestamp = new(2026, 9, 29, 4, 5, 6, 123);
        string mirrorPath = DeviceLogMirror.BuildMirrorLogPath(hostLogPath, "COM3", timestamp, root);
        Require(
            Path.GetFileName(mirrorPath) == "host-serial-COM3-20260929-040506-123.log",
            "镜像日志文件名必须包含独立 COM 口和毫秒时间戳");
        using (StreamWriter writer = new(
                   new FileStream(mirrorPath, FileMode.CreateNew, FileAccess.Write, FileShare.ReadWrite),
                   new UTF8Encoding(encoderShouldEmitUTF8Identifier: false))
        { AutoFlush = true })
        {
            DeviceLogMirror.WriteMirrorLogLine(writer, "COM3", "I (42) UART1统计 rx=7", timestamp);
            using FileStream readStream = new(mirrorPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            using StreamReader reader = new(readStream, Encoding.UTF8);
            Require(reader.ReadToEnd().Contains("[COM3] I (42) UART1统计 rx=7", StringComparison.Ordinal),
                "镜像日志必须在写入后可实时读取并保留端口标记");
        }
        Require(!File.Exists(Path.Combine(root, "host-runtime-20260929-040506.log")),
            "镜像日志写入不得混入主串口 host-runtime 文件");
    }
    finally
    {
        Directory.Delete(root, recursive: true);
    }
}

static void CheckDeviceLogMirrorDiscoveryPolicy()
{
    string[] startupPorts = DeviceLogMirror.SelectDiscoveryCandidates(["COM12"], "COM12", null, 0);
    Require(startupPorts.Length == 0, "主串口尚未确认之外没有可用候选时不得尝试接管主串口");

    string[] latePeerPorts = DeviceLogMirror.SelectDiscoveryCandidates(
        ["COM12", "COM3"],
        "COM12",
        null,
        0);
    Require(latePeerPorts.SequenceEqual(["COM3"]), "后枚举的 COM3 应在下一轮被识别为对端候选");

    string[] resumedPorts = DeviceLogMirror.SelectDiscoveryCandidates(
        ["COM12", "COM3", "COM8", "COM9"],
        "COM12",
        "COM3",
        1);
    Require(resumedPorts[0] == "COM3" && !resumedPorts.Contains("COM12", StringComparer.OrdinalIgnoreCase),
        "恢复监听时应优先回到历史对端口，并始终排除当前主串口");

    Require(DeviceLogMirror.ShouldAttemptDiscovery(false, false, false, false), "缺少镜像端口时应继续周期重试");
    Require(DeviceLogMirror.ShouldAttemptDiscovery(false, false, true, true), "读线程失效后应释放并重试镜像端口");
    Require(!DeviceLogMirror.ShouldAttemptDiscovery(false, false, true, false), "健康镜像端口不应被周期重开");
    Require(!DeviceLogMirror.ShouldAttemptDiscovery(false, true, false, false), "刷写独占期间必须暂停端口发现");
    Require(!DeviceLogMirror.ShouldAttemptDiscovery(true, false, false, false), "Dispose 后不得继续端口发现");
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
        string migratedProfileDirectory = Path.Combine(current, "profiles", "Global");
        Require(
            File.Exists(Path.Combine(migratedProfileDirectory, "lua", "main.txt")) &&
            File.ReadAllText(Path.Combine(migratedProfileDirectory, "lua", "main.txt")).Contains("OnEvent", StringComparison.Ordinal),
            "旧版 Lua 打开后必须迁移到 lua/main.txt");
        Require(
            File.Exists(Path.Combine(migratedProfileDirectory, "macros", "侧键测试.txt")) &&
            File.ReadAllText(Path.Combine(migratedProfileDirectory, "macros", "侧键测试.txt")).Contains("move(10,-4)", StringComparison.Ordinal),
            "旧版宏打开后必须迁移到 macros/ 下的 txt");
        string migratedMetadata = File.ReadAllText(Path.Combine(migratedProfileDirectory, "profile.json"));
        Require(migratedMetadata.Contains("\"lua_script_file\"", StringComparison.Ordinal), "新版 JSON 必须保存 Lua 文件关联");
        Require(!migratedMetadata.Contains("\"lua_script_text\"", StringComparison.Ordinal), "新版 JSON 不得继续保存 Lua 正文");
        using (JsonDocument migratedDocument = JsonDocument.Parse(migratedMetadata))
        {
            Require(
                migratedDocument.RootElement.GetProperty("macros")
                    .GetProperty("侧键测试")
                    .GetProperty("file")
                    .GetString() == "macros/侧键测试.txt",
                "新版 JSON 必须保存宏文件关联");
        }
        AutomationSettings behaviorSettings = store.LoadSettings();
        behaviorSettings.MinimizeToTray = false;
        behaviorSettings.CloseToTray = false;
        behaviorSettings.AlwaysOutputUdpEnabled = false;
        store.SaveSettings(behaviorSettings);
        AutomationSettings reloadedBehaviorSettings = store.LoadSettings();
        Require(!reloadedBehaviorSettings.MinimizeToTray && !reloadedBehaviorSettings.CloseToTray && !reloadedBehaviorSettings.AlwaysOutputUdpEnabled, "托盘与始终 UDP 输出设置未持久化");

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

static void CheckLuaRuntimeFeatures()
{
    RecordingAutomationOutput output = new();
    List<string> logs = [];
    using LuaScriptRunner lua = new(output, logs.Add, _ => { }, () => logs.Clear());
    string decimalScript = """
        function OnEvent(event, arg)
            if event == "pressed" then
                move(0.25, -0.25)
                move(0.25, -0.25)
                move(0.25, -0.25)
                move(0.25, -0.25)
                delay(0)
                sleep(0)
            end
        end
        """;
    (bool valid, string validationMessage) = lua.Check(decimalScript);
    Require(valid, $"Lua 小数 move/delay/sleep 脚本检查失败：{validationMessage}");
    lua.Start(decimalScript);
    lua.HandlePhysicalInput(new PhysicalInputEvent(new HashSet<uint> { 0x05 }, 0x05, true));
    Require(SpinWait.SpinUntil(() => output.Actions.Length >= 4, 1000), "Lua 小数 move 未在超时前执行完动作");
    lua.Stop();
    Require(
        output.Actions.Count(action => action == "move:0,0") == 2 &&
        output.Actions.Count(action => action == "move:1,-1") == 2,
        $"Lua move 小数累计结果不正确：{string.Join(", ", output.Actions)}");

    (bool syntaxValid, string syntaxMessage) = lua.Check(
        "function OnEvent(event, arg)\n" +
        "    if then\n" +
        "    end\n" +
        "end");
    Require(
        !syntaxValid && syntaxMessage.Contains("第 ", StringComparison.Ordinal) && syntaxMessage.Contains(" 行", StringComparison.Ordinal),
        $"Lua 语法错误应显示错误行号：{syntaxMessage}");
    try
    {
        lua.Start("function OnEvent(event, arg)\n    if then\n    end\nend");
        throw new InvalidOperationException("Lua 非法脚本启动时未抛出语法错误");
    }
    catch (InvalidOperationException exception) when (exception.Message.Contains("第 ", StringComparison.Ordinal))
    {
        // 启动按钮使用同一格式化错误消息，语法错误不会丢失行号。
    }

    string runtimeScript = """
        function OnEvent(event, arg)
            if event == "pressed" then
                missing_function()
            end
        end
        """;
    lua.Start(runtimeScript);
    lua.HandlePhysicalInput(new PhysicalInputEvent(new HashSet<uint> { 0x05 }, 0x05, true));
    Require(
        SpinWait.SpinUntil(
            () => logs.Any(line => line.Contains("Lua 执行错误", StringComparison.Ordinal) &&
                                  line.Contains("第 3 行", StringComparison.Ordinal)),
            1000),
        $"Lua 运行时错误应显示错误行号：{string.Join(" | ", logs)}");
    lua.Stop();
    Console.WriteLine("Lua 运行特性检查：delay/sleep 可执行；move 小数累计输出正确；语法和运行时错误均显示行号。");
}

static void CheckExternalProfileStorageAndLuaIndentation()
{
    string root = Path.Combine(Path.GetTempPath(), $"hidbridge-profile-layout-{Guid.NewGuid():N}");
    try
    {
        AutomationProfileStore store = new(root);
        AutomationProfile profile = store.LoadProfile(AutomationProfileStore.GlobalProfile);
        profile.LuaScriptText = "function OnEvent(event, arg)\nif event == 'pressed' then -- end in comment\nDebugLog('end in string')\nend\nend";
        profile.Macros["布局宏"] = new MacroDefinition
        {
            Name = "布局宏",
            Trigger = "f1",
            Mode = MacroRunModes.Once,
            Enabled = true,
            Text = "move(1, 2)\n",
        };
        store.SaveProfile(profile);

        string profileDirectory = Path.Combine(root, "profiles", AutomationProfileStore.GlobalProfile);
        string metadataPath = Path.Combine(profileDirectory, "profile.json");
        string metadata = File.ReadAllText(metadataPath);
        Require(File.Exists(Path.Combine(profileDirectory, "lua", "main.txt")), "Lua 正文必须位于 lua/ 下的 txt");
        Require(File.Exists(Path.Combine(profileDirectory, "macros", "布局宏.txt")), "宏正文必须位于 macros/ 下的 txt");
        Require(metadata.Contains("\"lua_script_file\"", StringComparison.Ordinal), "配置 JSON 缺少 Lua 文件关联");
        Require(!metadata.Contains("\"lua_script_text\"", StringComparison.Ordinal), "新配置 JSON 不得嵌入 Lua 正文");
        using (JsonDocument metadataDocument = JsonDocument.Parse(metadata))
        {
            Require(
                metadataDocument.RootElement.GetProperty("macros")
                    .GetProperty("布局宏")
                    .GetProperty("file")
                    .GetString() == "macros/布局宏.txt",
                "配置 JSON 缺少宏文件关联");
        }

        AutomationProfile reloaded = store.LoadProfile(AutomationProfileStore.GlobalProfile);
        Require(reloaded.LuaScriptText.Contains("end in string", StringComparison.Ordinal), "外置 Lua 重新加载失败");
        Require(reloaded.Macros["布局宏"].Text.Contains("move(1, 2)", StringComparison.Ordinal), "外置宏重新加载失败");

        string source =
            "function OnEvent(event, arg)\n" +
            "if event == 'pressed' then -- end must remain comment\n" +
            "DebugLog('end must remain string')\n" +
            "end\n" +
            "end";
        string aligned = LuaScriptIndentation.Align(source);
        Require(aligned.Split(Environment.NewLine).Length == source.Split('\n').Length, "Lua 对齐不得改变换行数量");
        Require(
            aligned ==
            string.Join(Environment.NewLine,
            [
                "function OnEvent(event, arg)",
                "    if event == 'pressed' then -- end must remain comment",
                "        DebugLog('end must remain string')",
                "    end",
                "end",
            ]),
            "Lua 对齐缩进结果不符合预期");
        Require(LuaScriptIndentation.Align(aligned) == aligned, "Lua 对齐必须幂等");
        Require(aligned.Contains("end must remain comment", StringComparison.Ordinal) &&
                aligned.Contains("'end must remain string'", StringComparison.Ordinal),
            "Lua 对齐不得修改注释或字符串内容");
        CheckLuaSpacingFormatting();
        Console.WriteLine("外置配置与 Lua 对齐检查：宏/Lua 独立 txt、JSON 关联、重新加载、缩进和行内空格格式化均通过，且不改换行。");
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}

static void CheckLuaSpacingFormatting()
{
    string spacingSource =
        "local a=1+2\n" +
        "local b=a..-3\n" +
        "local c=-1\n" +
        "local d=a- -b\n" +
        "local e=1e-3\n" +
        "local s='a=b .. -c'\n" +
        "local long=[=[a=b .. -c]=] -- a=b .. -c\n" +
        "local vararg=...\n" +
        "local compare=a~=b and c<=d and e>=f\n" +
        "local floor=7//2\n" +
        "local table={a=1,b=2}\n" +
        "foo(a,b)\n" +
        "if(x==1)then";
    string spaced = LuaScriptIndentation.Align(spacingSource);
    string expectedSpacing = string.Join(
        Environment.NewLine,
        [
            "local a = 1 + 2",
            "local b = a .. -3",
            "local c = -1",
            "local d = a - -b",
            "local e = 1e-3",
            "local s = 'a=b .. -c'",
            "local long = [=[a=b .. -c]=] -- a=b .. -c",
            "local vararg = ...",
            "local compare = a ~= b and c <= d and e >= f",
            "local floor = 7 // 2",
            "local table = {a = 1, b = 2}",
            "foo(a, b)",
            "if (x == 1) then",
        ]);
    Require(spaced == expectedSpacing, "Lua 行内空格格式化结果不符合预期");
    Require(
        spaced.Split(Environment.NewLine).Length == spacingSource.Split('\n').Length,
        "Lua 空格格式化不得改变换行数量");
    Require(LuaScriptIndentation.Align(spaced) == spaced, "Lua 空格格式化必须幂等");
    Require(
        spaced.Contains("'a=b .. -c'", StringComparison.Ordinal) &&
        spaced.Contains("[=[a=b .. -c]=] -- a=b .. -c", StringComparison.Ordinal),
        "Lua 空格格式化不得修改字符串或注释内容");
    Console.WriteLine("Lua 空格格式化专项检查通过：运算符、连接符、逗号、括号、一元负号、数字和注释/字符串保护均符合预期。");
}

static void CheckAutomationRemoteOutput()
{
    RecordingTransport transport = new();
    using InputForwarder input = new(transport, legacyFirmwareCompatibility: () => true);
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

static void CheckAutomationOutputRouteAndButtonMerge()
{
    RecordingTransport transport = new();
    RecordingAutomationOutput local = new();
    bool mouseHostAvailable = false;
    using InputForwarder input = new(
        transport,
        legacyFirmwareCompatibility: () => false,
        mouseHostAvailable: () => mouseHostAvailable);
    RoutedAutomationOutput output = new(input, local);

    // 新模式 M 不可用时，按下走 Win32；M 恢复后，原 Win32 按下仍由 Win32 松开。
    output.MoveRelative(3, -2);
    output.SetMouseButton(1, true);
    output.KeyDown(4);
    mouseHostAvailable = true;
    output.SetMouseButton(1, false);
    output.KeyUp(4);
    Require(
        local.Actions.Contains("mouse:1:down") && local.Actions.Contains("mouse:1:up") &&
        local.Actions.Contains("key:4:down") && local.Actions.Contains("key:4:up") &&
        local.Actions.Contains("move:3,-2"),
        "新模式 M 不可用时须使用 Win32，M 恢复后仍须向原 Win32 后端发送松开");
    Require(transport.MouseReports().Length == 0, "M 不可用期间的本机回退不得发送鼠标报告");

    // HOME 保持关闭；M 可用后 Lua 鼠标通过 8 字节桥接报告输出，键盘仍留在 Win32。
    output.MoveRelative(17, -9);
    output.Wheel(2);
    output.SetMouseButton(1, true);
    output.KeyDown(5);
    output.KeyUp(5);
    Require(
        SpinWait.SpinUntil(() =>
        {
            BridgeMouseReport[] reports = transport.BridgeMouseReports();
            return reports.Sum(report => (long)report.Report.X) == 17 &&
                reports.Sum(report => (long)report.Report.Y) == -9 &&
                reports.Sum(report => (long)report.Report.Wheel) == 2;
        }, 1000) && transport.MousePayloadLengths().All(length => length == MouseReportCodec.BridgeLength),
        "HOME 关闭时 M 可用的 Lua 鼠标移动/滚轮未形成完整 8 字节桥接报告");
    Require(
        transport.KeyboardReports().Length == 0 && local.Actions.Contains("key:5:down") &&
        local.Actions.Contains("key:5:up"),
        "新双板模式宏键盘必须继续通过 Win32，不得发送 KeyboardReport 到 M");

    // UDP 右键 held 与 Lua 左键共享报告状态；释放 Lua 左键不能释放 UDP 右键。
    input.SetRemoteMouseButtons(0x02);
    output.SetMouseButton(1, true);
    Require(
        SpinWait.SpinUntil(() => transport.MouseReports().Any(report => report.Buttons == 0x03), 1000),
        "UDP 右键 held 与 Lua 左键 down 未合并为 0x03");
    output.SetMouseButton(1, false);
    Require(
        SpinWait.SpinUntil(() => transport.MouseReports().LastOrDefault().Buttons == 0x02, 1000),
        "Lua 左键 up 未保留仍 held 的 UDP 右键 0x02");
    output.SetMouseButton(1, true);
    Require(
        SpinWait.SpinUntil(() => transport.MouseReports().LastOrDefault().Buttons == 0x03, 1000),
        "停止宏释放检查未先观察到 UDP 右键与宏左键同时按下");
    output.ReleaseAll();
    Require(
        SpinWait.SpinUntil(() => transport.MouseReports().LastOrDefault().Buttons == 0x02, 1000),
        "停止宏的 ReleaseAll 不得清除仍 held 的 UDP 右键 0x02");
    input.SetRemoteMouseButtons(0);
    Require(
        SpinWait.SpinUntil(() => transport.MouseReports().LastOrDefault().Buttons == 0, 1000),
        "UDP 右键释放后 M 软件报告未回到全释放状态");

    // M 失效时移动回退 Win32；先前发往 M 的按钮 up 仍释放 M 上的软件按钮。
    output.SetMouseButton(2, true);
    Require(
        SpinWait.SpinUntil(() => transport.MouseReports().LastOrDefault().Buttons == 0x04, 1000),
        "M 可用时的 Lua 按钮 down 未进入 M 软件报告");
    mouseHostAvailable = false;
    output.MoveRelative(4, 6);
    output.SetMouseButton(2, false);
    Require(
        local.Actions.Contains("move:4,6") &&
        SpinWait.SpinUntil(() => transport.MouseReports().LastOrDefault().Buttons == 0, 1000),
        "M 失效回退 Win32 后，原 M 后端的按钮 up 未释放软件按键");
    mouseHostAvailable = true;
    long recoveredX = transport.MouseReports().Sum(report => (long)report.X);
    long recoveredY = transport.MouseReports().Sum(report => (long)report.Y);
    long recoveredWheel = transport.MouseReports().Sum(report => (long)report.Wheel);
    output.MoveRelative(2, -3);
    output.Wheel(1);
    Require(
        SpinWait.SpinUntil(() =>
        {
            MouseReport[] reports = transport.MouseReports();
            return reports.Sum(report => (long)report.X) == recoveredX + 2 &&
                reports.Sum(report => (long)report.Y) == recoveredY - 3 &&
                reports.Sum(report => (long)report.Wheel) == recoveredWheel + 1;
        }, 1000),
        "M 恢复后新宏移动/滚轮未重新走桥接报告通路");
    output.ReleaseAll();

    RecordingTransport legacyRemoteTransport = new();
    using (InputForwarder legacyRemoteInput = new(
        legacyRemoteTransport,
        legacyFirmwareCompatibility: () => true))
    {
        legacyRemoteInput.SetForwardingEnabled(true);
        RoutedAutomationOutput legacyRemoteOutput = new(legacyRemoteInput, new RecordingAutomationOutput());
        legacyRemoteOutput.MoveRelative(12, -5);
        legacyRemoteOutput.Wheel(-1);
        legacyRemoteOutput.SetMouseButton(1, true);
        legacyRemoteOutput.KeyDown(4);
        legacyRemoteOutput.KeyUp(4);
        legacyRemoteOutput.SetMouseButton(1, false);
        Require(
            SpinWait.SpinUntil(() =>
            {
                MouseReport[] reports = legacyRemoteTransport.MouseReports();
                return reports.Sum(report => (long)report.X) == 12 &&
                    reports.Sum(report => (long)report.Y) == -5 &&
                    reports.Sum(report => (long)report.Wheel) == -1;
            }, 1000) &&
            legacyRemoteTransport.MousePayloadLengths().All(length => length == MouseReportCodec.Length),
            "旧版单板 HOME 开启时 Lua 鼠标移动必须通过 7 字节报告输出");
        byte[][] legacyKeyboardReports = legacyRemoteTransport.KeyboardReports();
        Require(
            legacyKeyboardReports.Any(report => report.Skip(2).Contains((byte)4)) &&
            legacyKeyboardReports.Last().All(value => value == 0),
            "旧版单板 HOME 开启时 Lua 键盘按下/松开必须发送键盘报告");
        legacyRemoteInput.SetForwardingEnabled(false);
    }

    RecordingTransport legacyUdpTransport = new();
    RecordingAutomationOutput legacyLocal = new();
    using (InputForwarder legacyLocalInput = new(
        legacyUdpTransport,
        legacyFirmwareCompatibility: () => true))
    {
        RoutedAutomationOutput legacyLocalOutput = new(legacyLocalInput, legacyLocal);
        legacyLocalOutput.MoveRelative(9, -4);
        legacyLocalOutput.Wheel(3);
        legacyLocalOutput.SetMouseButton(1, true);
        legacyLocalOutput.SetMouseButton(1, false);
        legacyLocalOutput.KeyDown(6);
        legacyLocalOutput.KeyUp(6);
        Require(
            legacyLocal.Actions.Contains("move:9,-4") &&
            legacyLocal.Actions.Contains("wheel:3") &&
            legacyLocal.Actions.ContainsSequence("mouse:1:down", "mouse:1:up") &&
            legacyLocal.Actions.ContainsSequence("key:6:down", "key:6:up") &&
            legacyUdpTransport.FrameCount(MessageType.MouseReport) == 0 &&
            legacyUdpTransport.FrameCount(MessageType.KeyboardReport) == 0,
            "旧版单板 HOME 关闭时 Lua/宏鼠标和键盘必须走 Win32");
        Require(
            legacyLocalInput.TryInjectMouseMovement(8, -3, 1) &&
            SpinWait.SpinUntil(() => legacyUdpTransport.MouseReports().Sum(report => (long)report.X) == 8, 1000) &&
            legacyUdpTransport.MousePayloadLengths().All(length => length == MouseReportCodec.Length),
            "旧版 HOME 关闭且 UDP 开关开启时应继续接收 7 字节 UDP 鼠标报告");
        legacyLocalInput.ConfigureAlwaysOutputUdp(false);
        int reportsBeforeDisabledUdp = legacyUdpTransport.FrameCount(MessageType.MouseReport);
        Require(
            !legacyLocalInput.TryInjectMouseMovement(6, 6) &&
            legacyUdpTransport.FrameCount(MessageType.MouseReport) == reportsBeforeDisabledUdp,
            "旧版 HOME 关闭且 UDP 开关关闭时应阻止 UDP 输入");
    }

    Console.WriteLine("自动化路由检查：新双板 HOME 关闭时 M 优先鼠标+Win32 键盘，失效回退并恢复；旧单板 HOME 开启走 7 字节鼠标与键盘报告，关闭走 Win32；UDP held 在宏 ReleaseAll 后保留。");
}

static void CheckPhysicalInputForwardingRouting()
{
    ConcurrentQueue<PhysicalInputEvent> dualObserved = new();
    RecordingTransport dualTransport = new();
    using (InputForwarder dualInput = new(dualTransport, legacyFirmwareCompatibility: () => false))
    {
        dualInput.PhysicalInputChanged += dualObserved.Enqueue;
        dualInput.ConfigureSimulatedUdpInput(true, 30);
        dualInput.SetForwardingEnabled(true);
        Require(
            dualInput.ForwardingEnabled &&
            !dualInput.PhysicalForwardingEnabled &&
            !dualInput.SimulatedUdpInputEnabled,
            "双板 HOME 开启时只保留 HOME 状态，实体输入与模拟测试不得转发到串口");
        Require(
            !InputForwarder.ShouldSuppressMouse(dualInput.PhysicalForwardingEnabled) &&
            !InputForwarder.ShouldSuppressKeyboard(dualInput.PhysicalForwardingEnabled, false, false),
            "双板 HOME 开启时实体鼠标和普通键盘不得被本机 Hook 抑制");

        dualInput.ProcessKeyboardInputForChecks(0x41, extended: false, isDown: true);
        dualInput.ProcessKeyboardInputForChecks(0x41, extended: false, isDown: false);
        dualInput.ProcessRawMouseInputForChecks(new NativeMethods.RawMouse
        {
            LastX = 11,
            LastY = -8,
            Buttons = NativeMethods.RawMouseLeftButtonDown,
        });
        dualInput.ProcessRawMouseInputForChecks(new NativeMethods.RawMouse
        {
            Buttons = NativeMethods.RawMouseLeftButtonUp,
        });

        Require(
            dualTransport.MouseReports().Length == 0 && dualTransport.KeyboardReports().Length == 0,
            "双板 HOME 开启时 EXE 不得重复转发实体鼠标或键盘报告");
        Require(
            dualObserved.Any(input => input.VirtualKey == 0x41 && input.Pressed) &&
            dualObserved.Any(input => input.VirtualKey == 0x41 && !input.Pressed) &&
            dualObserved.Any(input => input.VirtualKey == 0x01 && input.Pressed) &&
            dualObserved.Any(input => input.VirtualKey == 0x01 && !input.Pressed),
            "双板 HOME 开启时仍须把实体键鼠边沿送给本机 Lua/宏监听");
        dualInput.SetForwardingEnabled(false);
    }

    RecordingTransport legacyHomeOffTransport = new();
    using (InputForwarder legacyHomeOff = new(
        legacyHomeOffTransport,
        legacyFirmwareCompatibility: () => true))
    {
        legacyHomeOff.ConfigureSimulatedUdpInput(true, 30);
        Require(!legacyHomeOff.SimulatedUdpInputEnabled, "旧版 HOME 关闭时不得启用模拟输入测试源");
        legacyHomeOff.ProcessRawMouseInputForChecks(new NativeMethods.RawMouse
        {
            LastX = 7,
            LastY = 3,
        });
        Require(legacyHomeOffTransport.MouseReports().Length == 0, "旧版 HOME 关闭时实体鼠标不得进入串口报告");
        Require(
            legacyHomeOff.TryInjectMouseMovement(4, -2) &&
            SpinWait.SpinUntil(
                () => legacyHomeOffTransport.MouseReports().Sum(report => (long)report.X) == 4,
                1000) &&
            legacyHomeOffTransport.MousePayloadLengths().All(length => length == MouseReportCodec.Length),
            "实体门控不得影响旧版 HOME 关闭时仍启用的网络 UDP 输入");
    }

    ConcurrentQueue<PhysicalInputEvent> legacyObserved = new();
    RecordingTransport legacyHomeOnTransport = new();
    using (InputForwarder legacyHomeOn = new(
        legacyHomeOnTransport,
        legacyFirmwareCompatibility: () => true))
    {
        legacyHomeOn.PhysicalInputChanged += legacyObserved.Enqueue;
        legacyHomeOn.ConfigureSimulatedUdpInput(false, 100);
        legacyHomeOn.SetForwardingEnabled(true);
        Require(legacyHomeOn.PhysicalForwardingEnabled, "旧版 HOME 开启时必须启用实体输入转发");
        Require(
            InputForwarder.ShouldSuppressMouse(legacyHomeOn.PhysicalForwardingEnabled) &&
            InputForwarder.ShouldSuppressKeyboard(legacyHomeOn.PhysicalForwardingEnabled, true, false),
            "旧版 HOME 开启时实体输入和控制快捷键必须按旧通路规则抑制");
        legacyHomeOn.ProcessKeyboardInputForChecks(0x41, extended: false, isDown: true);
        legacyHomeOn.ProcessKeyboardInputForChecks(0x41, extended: false, isDown: false);
        legacyHomeOn.ProcessRawMouseInputForChecks(new NativeMethods.RawMouse
        {
            LastX = 12,
            LastY = -7,
            Buttons = NativeMethods.RawMouseLeftButtonDown,
        });
        legacyHomeOn.ProcessRawMouseInputForChecks(new NativeMethods.RawMouse
        {
            Buttons = NativeMethods.RawMouseLeftButtonUp,
        });
        Require(
            SpinWait.SpinUntil(
                () => legacyHomeOnTransport.MouseReports().Sum(report => (long)report.X) == 12 &&
                    legacyHomeOnTransport.MouseReports().LastOrDefault().Buttons == 0,
                1000),
            "旧版 HOME 开启且测试关闭时实体鼠标未走原始 7 字节报告路径");
        MouseReport[] reports = legacyHomeOnTransport.MouseReports();
        Require(
            reports.Sum(report => (long)report.Y) == -7 &&
            reports.Any(report => report.Buttons == 1) &&
            reports.Last().Buttons == 0 &&
            legacyHomeOnTransport.MousePayloadLengths().All(length => length == MouseReportCodec.Length),
            "旧版 HOME 开启的实体鼠标报告必须保留位移、按钮 down/up 与 7 字节格式");
        byte[][] keyboardReports = legacyHomeOnTransport.KeyboardReports();
        Require(
            keyboardReports.Any(report => report.Skip(2).Contains((byte)4)) &&
            keyboardReports.Last().All(value => value == 0),
            "旧版 HOME 开启时实体键盘必须继续产生按下和释放报告");
        Require(
            legacyObserved.Any(input => input.VirtualKey == 0x41 && input.Pressed) &&
            legacyObserved.Any(input => input.VirtualKey == 0x01 && input.Pressed),
            "旧版实体按键转发期间 Lua/宏本机监听仍须收到键鼠事件");
        legacyHomeOn.SetForwardingEnabled(false);
    }

    RecordingTransport simulatedTransport = new();
    using (InputForwarder simulated = new(
        simulatedTransport,
        legacyFirmwareCompatibility: () => true))
    {
        simulated.ConfigureSimulatedUdpInput(true, 30);
        simulated.SetForwardingEnabled(true);
        Require(simulated.SimulatedUdpInputEnabled, "旧版 HOME 开启且偏好开启时模拟输入测试源必须生效");
        simulated.ProcessRawMouseInputForChecks(new NativeMethods.RawMouse
        {
            LastX = 9,
            LastY = -6,
        });
        simulated.ConfigureSimulatedUdpInput(false, 30);
        Thread.Sleep(70);
        Require(
            !simulated.SimulatedUdpInputEnabled && simulatedTransport.MouseReports().Length == 0,
            "关闭模拟输入测试时必须丢弃旧分桶，不能把测试尾部重放到普通报告路径");
        simulated.SetForwardingEnabled(false);
    }

    Console.WriteLine("实体输入路由检查：实体键鼠仅在原单板固件+HOME 时由 EXE 转发；双板实体鼠标保留 M→P 硬件直通，本机键盘继续监听；UDP 模拟测试仅在原单板+HOME+开关开启时聚合到单板 7 字节报告，不发网络 UDP；网络 UDP 路由独立。");
}

static void CheckRoutingModeChangesKeepHomeAndReloadOnce()
{
    string directory = Path.Combine(Path.GetTempPath(), $"hidbridge-route-switch-{Guid.NewGuid():N}");
    try
    {
        AutomationProfileStore store = new(directory);
        AutomationProfile profile = store.LoadProfile(AutomationProfileStore.GlobalProfile);
        profile.Macros["路由切换单次触发"] = new MacroDefinition
        {
            Trigger = "f1",
            Mode = MacroRunModes.Once,
            Enabled = true,
            Text = "move(10,0)",
        };
        store.SaveProfile(profile);

        AutomationController? controllerState = null;
        RecordingTransport transport = new();
        RecordingAutomationOutput localOutput = new();
        using InputForwarder input = new(
            transport,
            legacyFirmwareCompatibility: () =>
                controllerState?.Settings.LegacySingleBoardFirmwareCompatibility ?? false);
        using AutomationController controller = new(store, input, localOutput);
        controllerState = controller;
        controller.ApplyOutputRoutingSettings(false, true, true, 30);
        input.SetForwardingEnabled(true);
        controller.Start();
        Require(
            input.ForwardingEnabled &&
            !input.PhysicalForwardingEnabled &&
            !input.SimulatedUdpInputEnabled,
            "双板模式切换前 HOME 状态应为开启，但不启用物理/模拟转发");

        controller.ApplyOutputRoutingSettings(true, true, true, 30);
        Require(
            input.ForwardingEnabled &&
            input.PhysicalForwardingEnabled &&
            input.SimulatedUdpInputEnabled,
            "切到旧版后必须保留 HOME 开启状态并按请求激活物理模拟测试源");
        controller.ApplyOutputRoutingSettings(false, true, true, 30);
        Require(
            input.ForwardingEnabled &&
            !input.PhysicalForwardingEnabled &&
            !input.SimulatedUdpInputEnabled,
            "切回双板后必须保留 HOME 开启状态并立即关闭物理/模拟转发");

        input.ProcessKeyboardInputForChecks(0x70, extended: false, isDown: true);
        input.ProcessKeyboardInputForChecks(0x70, extended: false, isDown: false);
        Require(
            SpinWait.SpinUntil(
                () => localOutput.Actions.Count(action => action == "move:10,0") >= 1,
                1000),
            "路由切换后的宏没有重新绑定到 F1");
        Thread.Sleep(80);
        Require(
            localOutput.Actions.Count(action => action == "move:10,0") == 1,
            "一次模式切换后同一 F1 宏不得因重复绑定执行两次");
        input.SetForwardingEnabled(false);
    }
    finally
    {
        if (Directory.Exists(directory))
        {
            Directory.Delete(directory, recursive: true);
        }
    }

    Console.WriteLine("路由模式切换检查：legacy↔dual 保留 HOME，物理/模拟门控和宏重载各生效一次。");
}

static void CheckLocalMouseTriggersReachLua()
{
    string directory = Path.Combine(Path.GetTempPath(), $"hidbridge-local-lua-{Guid.NewGuid():N}");
    try
    {
        AutomationProfileStore store = new(directory);
        AutomationProfile profile = store.LoadProfile(AutomationProfileStore.GlobalProfile);
        profile.LuaScriptText =
            "function OnEvent(event, arg) " +
            "DebugLog(\"event=%s arg=%s\", event, tostring(arg)); " +
            "if event == \"pressed\" then DebugLog(\"press arg=%s\", tostring(arg)) " +
            "else DebugLog(\"release arg=%s\", tostring(arg)) end end";
        store.SaveProfile(profile);

        RecordingTransport transport = new();
        using InputForwarder input = new(transport, legacyFirmwareCompatibility: () => false);
        using AutomationController automation = new(store, input, new RecordingAutomationOutput());
        ConcurrentQueue<string> logs = new();
        ConcurrentQueue<string> diagnosticLogs = new();
        automation.Log += logs.Enqueue;
        automation.DiagnosticLog += diagnosticLogs.Enqueue;
        automation.Start();

        input.SetForwardingEnabled(true);
        Require(
            input.ForwardingEnabled && !input.PhysicalForwardingEnabled,
            "双板 HOME 开启时应保留 Host 状态但不转发实体鼠标");
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
            1000), "双板 HOME 开启时实体鼠标按键未送达 Lua OnEvent/DebugLog");
        Require(
            diagnosticLogs.Any(line => line.StartsWith("[LuaEvent]", StringComparison.Ordinal)),
            "Lua 事件详细诊断未进入独立日志通道");
        Require(
            logs.Count(line => line == "press arg=4") == 1 &&
            logs.Count(line => line == "release arg=4") == 1,
            "脚本主动 DebugLog 的 press/release arg=4 必须保留且不能被 Host 重复生成");
        Require(
            diagnosticLogs.Any(line => line.Contains("event=pressed arg=4", StringComparison.Ordinal)) &&
            diagnosticLogs.Any(line => line.Contains("event=released arg=4", StringComparison.Ordinal)),
            "Lua 事件详细诊断必须保留原始参数");
        Require(
            !logs.Any(line => line.StartsWith("[LuaEvent]", StringComparison.Ordinal) ||
                              line.StartsWith("[LuaOutput]", StringComparison.Ordinal)),
            "Lua UI 简略日志通道不得包含详细事件或输出诊断");
        Require(transport.MouseReports().Length == 0, "本机 Lua 触发不得向对端发送实体鼠标报告");
        Console.WriteLine(
            "本机 Lua 触发检查：双板 HOME 开启时仍收到鼠标事件；脚本 DebugLog 保留；对端实体鼠标报告=0。");
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
        using InputForwarder input = new(transport, legacyFirmwareCompatibility: () => true);
        using AutomationController automation = new(store, input, new RecordingAutomationOutput());
        ConcurrentQueue<string> transitionLogs = new();
        automation.Log += transitionLogs.Enqueue;
        automation.Start();
        long localReleaseCountBeforeTransitions = automation.LocalReleaseAllCount;
        input.SetForwardingEnabled(true);
        Require(
            automation.LocalReleaseAllCount == localReleaseCountBeforeTransitions + 1,
            $"进入捕获模式时必须向主控端发送一次 Win32 ReleaseAll；日志={string.Join(" | ", transitionLogs)}");
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

static void CheckToggleMacroStopsOnSecondPress()
{
    string directory = Path.Combine(Path.GetTempPath(), $"hidbridge-toggle-{Guid.NewGuid():N}");
    try
    {
        AutomationProfileStore store = new(directory);
        AutomationProfile profile = store.LoadProfile(AutomationProfileStore.GlobalProfile);
        profile.Macros["切换宏"] = new MacroDefinition
        {
            Name = "切换宏",
            Trigger = "mouse_side1",
            Mode = MacroRunModes.Toggle,
            Enabled = true,
            Text = "move(1,0)\ndelay(10)",
        };
        store.SaveProfile(profile);

        RecordingTransport transport = new();
        RecordingAutomationOutput output = new();
        ConcurrentQueue<string> logs = new();
        using InputForwarder input = new(transport);
        using AutomationController automation = new(store, input, output);
        automation.Log += logs.Enqueue;
        automation.Start();

        input.ProcessRawMouseInputForChecks(new NativeMethods.RawMouse
        {
            Buttons = NativeMethods.RawMouseButton4Down,
        });
        input.ProcessRawMouseInputForChecks(new NativeMethods.RawMouse
        {
            Buttons = NativeMethods.RawMouseButton4Up,
        });
        Require(
            SpinWait.SpinUntil(() => output.Actions.Count(action => action == "move:1,0") >= 3, 1000),
            "切换重复宏第一次按下未启动循环");
        Require(logs.Any(line => line.StartsWith("触发宏：mouse_side1", StringComparison.Ordinal)), "切换重复宏第一次按下未记录触发宏");

        input.ProcessRawMouseInputForChecks(new NativeMethods.RawMouse
        {
            Buttons = NativeMethods.RawMouseButton4Down,
        });
        input.ProcessRawMouseInputForChecks(new NativeMethods.RawMouse
        {
            Buttons = NativeMethods.RawMouseButton4Up,
        });
        Thread.Sleep(100);
        int moveCountAfterStop = output.Actions.Count(action => action == "move:1,0");
        Thread.Sleep(100);
        Require(
            output.Actions.Count(action => action == "move:1,0") == moveCountAfterStop,
            "切换重复宏第二次按下后仍在循环执行");
        Require(logs.Any(line => line.StartsWith("停止宏：mouse_side1", StringComparison.Ordinal)), "切换重复宏第二次按下未记录停止宏");
        Console.WriteLine($"切换重复宏检查：第一次按下启动，第二次按下停止，停止时累计 move={moveCountAfterStop}，日志已区分触发/停止。");
    }
    finally
    {
        if (Directory.Exists(directory))
        {
            Directory.Delete(directory, true);
        }
    }
}

static void CheckHotkeyChooserControl()
{
    using HotkeyChooserControl chooser = new();
    TableLayoutPanel layout = chooser.Controls.OfType<TableLayoutPanel>().Single();
    ComboBox keyComboBox = layout.Controls.OfType<ComboBox>().Single();
    Button addButton = layout.Controls.OfType<Button>().Single(button => button.Text == "添加按键");
    Button clearButton = layout.Controls.OfType<Button>().Single(button => button.Text == "清空");

    Require(
        AutomationKeyMap.TriggerKeyNames.Contains("mouse_side1", StringComparer.OrdinalIgnoreCase) &&
        AutomationKeyMap.TriggerKeyNames.Contains("ctrl", StringComparer.OrdinalIgnoreCase) &&
        AutomationKeyMap.TriggerKeyNames.Contains("f24", StringComparer.OrdinalIgnoreCase),
        "触发键下拉列表未包含鼠标侧键、修饰键和 F24");

    keyComboBox.SelectedItem = "ctrl";
    addButton.PerformClick();
    keyComboBox.SelectedItem = "f1";
    addButton.PerformClick();
    addButton.PerformClick();
    Require(chooser.Value == "ctrl+f1", $"触发键添加/去重结果不正确：{chooser.Value}");

    chooser.SetHotkey("mouse_side1+ctrl+f1");
    Require(chooser.Value == "mouse_side1+ctrl+f1", $"已有组合键载入结果不正确：{chooser.Value}");
    clearButton.PerformClick();
    Require(string.IsNullOrEmpty(chooser.Value), "清空触发键后仍保留组合键");
    Console.WriteLine("宏触发键选择器检查：下拉键表、组合添加、重复去重、已有组合载入和清空均通过。");
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
            AutomationController? controllerState = null;
            using InputForwarder input = new(
                transport,
                legacyFirmwareCompatibility: () =>
                    controllerState?.Settings.LegacySingleBoardFirmwareCompatibility ?? false);
            AutomationProfileStore store = new(automationDirectory);
            AutomationProfile uiProfile = store.LoadProfile(AutomationProfileStore.GlobalProfile);
            uiProfile.LuaScriptText = "function OnEvent(event, arg)\nif event == \"pressed\" then\nDebugLog(\"aligned\")\nend\nend";
            uiProfile.Macros["立即保存宏"] = new MacroDefinition
            {
                Name = "立即保存宏",
                Trigger = "f1",
                Mode = MacroRunModes.Once,
                Enabled = true,
                Text = "move(1,1)",
            };
            store.SaveProfile(uiProfile);
            using AutomationController automation = new(store, input, new RecordingAutomationOutput());
            controllerState = automation;
            // 该布局检查只验证离屏控件和 RecordingAutomationOutput；禁止把测试光标移到真实桌面。
            using BridgeMainForm form = new(
                input,
                automation,
                "测试端点",
                enableCursorLock: false,
                lanEndpointDescription: "UDP 192.168.1.20:24814");
            _ = form.Handle;
            form.PerformLayout();
            double upperRatio = form.MainSplit.SplitterDistance / (double)form.MainSplit.ClientSize.Height;
            Require(upperRatio is >= 0.45 and <= 0.55, "窗口上半区必须约占客户区一半");
            Require(form.MainTabs.TabPages.Count == 4, "主窗口必须包含鼠标捕获、宏、Lua、设置四个功能页");
            Require(form.MainTabs.TabPages.Cast<TabPage>().Select(page => page.Text).SequenceEqual(["鼠标捕获", "宏", "Lua", "设置"]), "四个功能页顺序或名称不正确");
            form.MainTabs.SelectedTab = form.MainTabs.TabPages[1];
            form.MainTabs.PerformLayout();
            form.MainTabs.SelectedTab.PerformLayout();
            form.MacroPage.PerformLayout();
            Application.DoEvents();
            form.PerformLayout();
            Require(form.MacroPage.ProfileComboBox.Items.Count >= 1, "宏页未加载手动配置列表");
            Require(form.LuaPage.ProfileComboBox.Items.Count >= 1, "Lua 页未加载手动配置列表");
            Require(form.LuaPage.AddProfileButton.Text == "新增配置" && form.LuaPage.DeleteProfileButton.Text == "删除配置", "Lua 页缺少新增/删除配置按钮");
            Require(
                form.LuaPage.Editor.Multiline &&
                form.LuaPage.Editor.ScrollBars == ScrollBars.Both &&
                !form.LuaPage.Editor.WordWrap,
                "Lua 编辑器必须保留多行文本和横向滚动能力");
            Require(
                LuaPageControl.NormalizeEditorNewlines("第一行\n第二行\r第三行\n第四行") ==
                $"第一行{Environment.NewLine}第二行{Environment.NewLine}第三行{Environment.NewLine}第四行",
                "Lua 编辑器粘贴前必须把不同换行格式统一为 Windows 多行文本");
            form.MainTabs.SelectedTab = form.MainTabs.TabPages[2];
            form.LuaPage.Editor.Text = "function OnEvent(event,arg)\nif event==\"pressed\" then\nDebugLog(\"aligned\")\nend\nend";
            int luaLineCountBeforeCheck = form.LuaPage.Editor.Text.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n').Length;
            form.StartPosition = FormStartPosition.Manual;
            form.Location = new Point(-32000, -32000);
            form.Show();
            Application.DoEvents();
            Require(!automation.LuaActive, "Lua 布局检查初始状态必须为已停止");
            Require(form.LuaPage.StartButton.Enabled && !form.LuaPage.StopButton.Enabled, "Lua 初始按钮状态必须为启动可用、停止禁用");
            CheckCopyableInterfaceDialog(form.LuaPage.ApiButton, "Lua 接口说明", "move(dx, dy)");
            form.LuaPage.StartButton.PerformClick();
            Application.DoEvents();
            Require(automation.LuaActive, "点击 Lua 启动后控制器必须进入运行状态");
            Require(!form.LuaPage.StartButton.Enabled && form.LuaPage.StopButton.Enabled, "Lua 运行时必须禁用启动、启用停止");
            form.LuaPage.StopButton.PerformClick();
            Application.DoEvents();
            Require(!automation.LuaActive, "点击 Lua 停止后控制器必须回到停止状态");
            Require(form.LuaPage.StartButton.Enabled && !form.LuaPage.StopButton.Enabled, "Lua 停止后必须恢复启动可用、停止禁用");
            form.LuaPage.Editor.Text = "function OnEvent(event, arg)\nthis is not valid Lua\nend";
            form.LuaPage.StartButton.PerformClick();
            Application.DoEvents();
            Require(!automation.LuaActive, "Lua 启动失败后控制器不得报告运行中");
            Require(form.LuaPage.StartButton.Enabled && !form.LuaPage.StopButton.Enabled, "Lua 启动失败后按钮状态必须仍为启动可用、停止禁用");
            Console.WriteLine("Lua 按钮状态检查：初始、启动、停止和启动失败均按控制器真实状态互斥。");
            form.LuaPage.Editor.Text = "function OnEvent(event,arg)\nif event==\"pressed\" then\nDebugLog(\"aligned\")\nend\nend";
            Require(
                form.LuaPage.LineNumberGutter.Visible && form.LuaPage.LineNumberGutter.Width >= 30,
                "Lua 输入栏左侧必须显示可见的行号栏");
            form.LuaPage.CheckButton.PerformClick();
            Require(
                form.LuaPage.Editor.Text.StartsWith($"function OnEvent(event, arg){Environment.NewLine}    if event == \"pressed\" then", StringComparison.Ordinal) &&
                form.LuaPage.Editor.Text.Contains($"{Environment.NewLine}        DebugLog", StringComparison.Ordinal) &&
                form.LuaPage.Editor.Text.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n').Length == luaLineCountBeforeCheck &&
                form.LuaPage.StatusLabel.Text.Contains("自动对齐缩进和空格", StringComparison.Ordinal),
                "Lua 检查必须自动对齐已有行、规范行内空格且不得改变换行数量");
            form.MainTabs.SelectedTab = form.MainTabs.TabPages[1];
            form.MacroPage.PerformLayout();
            Application.DoEvents();
            CheckCopyableInterfaceDialog(form.MacroPage.InterfaceButton, "宏接口说明", "randsleep(base, variance)");
            SplitContainer macroEditorSplit = EnumerateControls(form.MacroPage)
                .OfType<SplitContainer>()
                .Single(split => split.Orientation == Orientation.Vertical);
            Require(macroEditorSplit.SplitterDistance >= 330, $"宏列表区域应加宽：当前分隔位置={macroEditorSplit.SplitterDistance}，客户区={macroEditorSplit.ClientSize.Width}x{macroEditorSplit.ClientSize.Height}");
            TableLayoutPanel macroButtons = EnumerateControls(form.MacroPage)
                .OfType<TableLayoutPanel>()
                .Single(layout => layout.Controls.OfType<Button>().Any(button => button.Text == "新增宏"));
            Require(
                macroButtons.RowCount == 2 &&
                macroButtons.Controls.OfType<Button>().Select(button => button.Text).ToHashSet().SetEquals(["新增宏", "删除宏", "重命名"]) &&
                macroButtons.GetControlFromPosition(0, 1)?.Text == "重命名" &&
                macroButtons.GetColumnSpan(macroButtons.GetControlFromPosition(0, 1)!) == 1 &&
                macroButtons.GetControlFromPosition(1, 1) is null,
                "宏列表下方按钮必须分成两行并完整显示新增、删除、重命名");
            TableLayoutPanel editorButtons = EnumerateControls(form.MacroPage)
                .OfType<TableLayoutPanel>()
                .Single(layout => layout.Controls.OfType<Button>().Any(button => button.Text == "检查语法"));
            Require(
                editorButtons.GetControlFromPosition(0, 0)?.Text == "接口说明" &&
                editorButtons.GetControlFromPosition(1, 0)?.Text == "检查语法" &&
                editorButtons.GetControlFromPosition(2, 0)?.Text == "保存宏",
                "宏编辑器按钮顺序必须为接口说明、检查语法、保存宏");
            foreach (TextBox logTextBox in new[]
            {
                EnumerateControls(form.MacroPage).OfType<TextBox>().Single(textBox => textBox.ReadOnly && textBox.Multiline),
                EnumerateControls(form.LuaPage).OfType<TextBox>().Single(textBox => textBox.ReadOnly && textBox.Multiline),
            })
            {
                logTextBox.Text = "历史日志第一行\r\n历史日志第二行";
                logTextBox.SelectionStart = 2;
                logTextBox.SelectionLength = 5;
                string selectedLog = logTextBox.SelectedText;
                LogTextBoxAppender.Append(logTextBox, ["最新日志"], 64 * 1024);
                Require(logTextBox.SelectedText == selectedLog, "宏/Lua 日志追加时不得强制把部分选区扩展到日志底部");
            }
            form.MainTabs.SelectedTab = form.MainTabs.TabPages[0];
            form.MainTabs.PerformLayout();
            Application.DoEvents();
            TextBox mainLogTextBox = form.LogTextBox;
            mainLogTextBox.Text = string.Join(
                Environment.NewLine,
                Enumerable.Range(1, 80).Select(index => $"主日志历史 {index}")) + Environment.NewLine;
            mainLogTextBox.Focus();
            mainLogTextBox.SelectionStart = mainLogTextBox.TextLength;
            mainLogTextBox.SelectionLength = 0;
            mainLogTextBox.ScrollToCaret();
            Application.DoEvents();
            int mainLogFirstVisibleLineAtBottom = GetFirstVisibleLine(mainLogTextBox);
            Require(mainLogFirstVisibleLineAtBottom > 0, "主捕获页日志测试未能建立位于底部的视图");
            SetTextBoxInsertionPointWithoutScrolling(mainLogTextBox, 2);
            Application.DoEvents();
            Require(
                GetFirstVisibleLine(mainLogTextBox) == mainLogFirstVisibleLineAtBottom,
                "主捕获页日志测试未能保持历史插入点下的底部视图");
            form.AppendLog("主捕获页底部跟随日志");
            Require(
                mainLogTextBox.SelectionLength == 0 && mainLogTextBox.SelectionStart == mainLogTextBox.TextLength,
                "主捕获页日志在底部时不得因插入点位于历史位置而停止跟随");
            Require(
                GetFirstVisibleLine(mainLogTextBox) >= mainLogFirstVisibleLineAtBottom,
                "主捕获页日志在底部且插入点位于历史位置时必须继续滚动到最新日志");
            CheckLogTextBoxAppenderFollowAtBottom();
            string[] macroPageOptions = EnumerateControls(form.MacroPage).Select(control => control.Text).ToArray();
            Require(!macroPageOptions.Contains("开机启动") && !macroPageOptions.Contains("最小化到托盘") && !macroPageOptions.Contains("关闭到托盘"), "程序行为设置不得继续显示在宏页");
            Require(
                macroPageOptions.Contains("触发键：") &&
                macroPageOptions.Contains("添加按键") &&
                macroPageOptions.Contains("清空") &&
                macroPageOptions.Contains("重命名") &&
                macroPageOptions.Contains("启用宏"),
                "宏页必须显示触发键选择器的按钮、重命名按钮和启用宏勾选框");
            Require(form.MacroPage.EnabledCheckBox.Checked, "新建或空宏默认必须启用");
            Require(form.MacroPage.MacroListBox.SelectedItem as string == "立即保存宏", "宏页未加载用于即时保存检查的宏");
            form.MacroPage.ModeComboBox.SelectedIndex = 1;
            AutomationProfile modeSavedProfile = store.LoadProfile(AutomationProfileStore.GlobalProfile);
            Require(modeSavedProfile.Macros["立即保存宏"].Mode == MacroRunModes.Toggle, "切换宏模式后未立即保存");
            form.MacroPage.EnabledCheckBox.Checked = false;
            AutomationProfile enabledSavedProfile = store.LoadProfile(AutomationProfileStore.GlobalProfile);
            Require(!enabledSavedProfile.Macros["立即保存宏"].Enabled, "启用宏开关变化后未立即保存");
            Require(!automation.Settings.MinimizeToTray, "最小化到托盘默认必须关闭");
            Require(form.Icon is not null && form.TrayIcon.Icon is not null, "主窗口和状态栏必须加载应用图标");
            TableLayoutPanel chooserLayout = form.MacroPage.TriggerChooser.Controls.OfType<TableLayoutPanel>().Single();
            Control displayControl = chooserLayout.GetControlFromPosition(0, 0) ?? throw new InvalidOperationException("触发键显示栏未加载");
            Control keySelectorControl = chooserLayout.GetControlFromPosition(1, 0) ?? throw new InvalidOperationException("触发键选择下拉框未加载");
            Control addKeyControl = chooserLayout.GetControlFromPosition(2, 0) ?? throw new InvalidOperationException("添加按键按钮未加载");
            Control clearKeyControl = chooserLayout.GetControlFromPosition(3, 0) ?? throw new InvalidOperationException("清空按钮未加载");
            Require(
                new[] { displayControl.Height, keySelectorControl.Height, addKeyControl.Height, clearKeyControl.Height }.Distinct().Count() == 1,
                $"触发键显示栏、选择下拉框、添加和清空按钮高度必须一致：显示={displayControl.Height}，选择={keySelectorControl.Height}，添加={addKeyControl.Height}，清空={clearKeyControl.Height}；类型={keySelectorControl.GetType().FullName}，AutoSize={keySelectorControl.AutoSize}，Dock={keySelectorControl.Dock}，最小={keySelectorControl.MinimumSize.Height}，最大={keySelectorControl.MaximumSize.Height}");
            TableLayoutPanel optionsLayout = form.MacroPage.ModeComboBox.Parent as TableLayoutPanel
                ?? throw new InvalidOperationException("模式选项布局未加载");
            Require(
                optionsLayout.ColumnStyles[1].Width < optionsLayout.ColumnStyles[3].Width,
                $"模式选择下拉框必须短于启用宏区域：百分比={optionsLayout.ColumnStyles[1].Width}/{optionsLayout.ColumnStyles[3].Width}");
            Point triggerStart = form.MacroPage.TriggerChooser.PointToScreen(Point.Empty);
            Point modeStart = form.MacroPage.ModeComboBox.PointToScreen(Point.Empty);
            Require(
                Math.Abs(triggerStart.X - modeStart.X) <= 1,
                $"模式选择下拉框左端必须与触发键显示栏对齐：触发={triggerStart.X}，模式={modeStart.X}");
            Require(form.SettingsPage.StartOnBootCheckBox.Text.StartsWith("开机启动", StringComparison.Ordinal), "设置页缺少开机启动选项");
            Require(form.SettingsPage.MinimizeToTrayCheckBox.Checked == automation.Settings.MinimizeToTray, "设置页最小化到托盘状态未从配置加载");
            Require(form.SettingsPage.CloseToTrayCheckBox.Checked == automation.Settings.CloseToTray, "设置页关闭到托盘状态未从配置加载");
            Require(form.SettingsPage.LegacySingleBoardFirmwareCheckBox.Checked == automation.Settings.LegacySingleBoardFirmwareCompatibility, "旧版单板固件兼容开关未从配置加载");
            form.MainTabs.SelectedTab = form.MainTabs.TabPages[3];
            form.SettingsPage.PerformLayout();
            Application.DoEvents();
            Panel legacyUdpRow = form.SettingsPage.AlwaysOutputUdpOptionRow;
            Panel legacyModeRow = form.SettingsPage.LegacySingleBoardOptionRow;
            Panel simulatedUdpRow = form.SettingsPage.SimulatedUdpInputOptionRow;
            CheckBox simulatedUdpCheckBox = form.SettingsPage.SimulatedUdpInputCheckBox;
            ComboBox simulatedUdpFrequency = form.SettingsPage.SimulatedUdpInputFrequencyBox;
            Label simulatedUdpDescription = EnumerateControls(simulatedUdpRow)
                .OfType<Label>()
                .Single(label => label.AccessibleName == "UDP 模拟输入测试说明");
            Label legacyUdpDescription = EnumerateControls(legacyUdpRow)
                .OfType<Label>()
                .Single(label => label.Text.Contains("仅旧版单板通路使用", StringComparison.Ordinal));
            Require(
                legacyUdpRow.Parent is FlowLayoutPanel options &&
                legacyModeRow.Parent == options &&
                simulatedUdpRow.Parent == options &&
                legacyUdpRow.Parent == options &&
                options.Controls.IndexOf(simulatedUdpRow) == options.Controls.IndexOf(legacyModeRow) + 1 &&
                options.Controls.IndexOf(legacyUdpRow) == options.Controls.IndexOf(simulatedUdpRow) + 1 &&
                simulatedUdpRow.Top >= legacyModeRow.Bottom &&
                legacyUdpRow.Top >= simulatedUdpRow.Bottom,
                "UDP 模拟测试行与始终 UDP 输出行必须按顺序紧跟旧版单板选项");
            Require(
                !automation.Settings.LegacySingleBoardFirmwareCompatibility &&
                !legacyUdpRow.Visible &&
                !simulatedUdpRow.Visible &&
                !simulatedUdpCheckBox.Visible &&
                !simulatedUdpFrequency.Visible &&
                !simulatedUdpDescription.Visible &&
                !form.SettingsPage.AlwaysOutputUdpCheckBox.Visible &&
                !legacyUdpDescription.Visible,
                "新双板模式必须隐藏两个旧版选项整行及说明，不能留下可见残余文字");
            Require(
                !automation.Settings.SimulatedUdpInputEnabled &&
                !input.SimulatedUdpInputEnabled &&
                simulatedUdpCheckBox.Text == "UDP 模拟输入测试" &&
                simulatedUdpFrequency.Items.Cast<int>().SequenceEqual([30, 60, 100, 140, 200, 500, 0]) &&
                simulatedUdpFrequency.SelectedItem is 100,
                "模拟输入测试偏好必须默认关闭、频率默认 100 Hz 并保留完整频率列表");
            form.SettingsPage.LegacySingleBoardFirmwareCheckBox.Checked = true;
            Application.DoEvents();
            Require(
                legacyUdpRow.Visible &&
                simulatedUdpRow.Visible &&
                simulatedUdpCheckBox.Visible &&
                simulatedUdpDescription.Visible &&
                !simulatedUdpFrequency.Enabled &&
                form.SettingsPage.AlwaysOutputUdpCheckBox.Visible &&
                legacyUdpDescription.Visible &&
                form.SettingsPage.AlwaysOutputUdpCheckBox.Text == "始终开启 UDP 输出",
                "仅旧版单板模式开启后才显示模拟输入和始终 UDP 输出整行及说明");
            simulatedUdpCheckBox.Checked = true;
            simulatedUdpFrequency.SelectedItem = 140;
            Require(
                automation.Settings.SimulatedUdpInputEnabled &&
                automation.Settings.SimulatedUdpInputFrequencyHz == 140 &&
                input.SimulatedUdpInputFrequencyHz == 140 &&
                !input.SimulatedUdpInputEnabled &&
                simulatedUdpFrequency.Enabled,
                "旧版 HOME 关闭时应保存模拟测试请求和频率，但实际测试源保持关闭");
            input.SetForwardingEnabled(true);
            Require(
                input.ForwardingEnabled && input.PhysicalForwardingEnabled && input.SimulatedUdpInputEnabled &&
                form.CaptureSurface.Forwarding,
                "旧版 HOME 开启后应启用物理路由、模拟测试源并更新捕获面板状态");
            form.SettingsPage.LegacySingleBoardFirmwareCheckBox.Checked = false;
            Require(
                input.ForwardingEnabled && !input.PhysicalForwardingEnabled && !input.SimulatedUdpInputEnabled &&
                !form.CaptureSurface.Forwarding && !simulatedUdpRow.Visible,
                "切换到双板时必须保留 HOME 状态、关闭物理/模拟路由并即时更新 UI");
            form.SettingsPage.LegacySingleBoardFirmwareCheckBox.Checked = true;
            Require(
                input.ForwardingEnabled && input.PhysicalForwardingEnabled && input.SimulatedUdpInputEnabled &&
                form.CaptureSurface.Forwarding && simulatedUdpRow.Visible,
                "切回旧版时必须恢复 HOME 对应的物理/模拟路由及 UI 状态");
            simulatedUdpCheckBox.Checked = false;
            Require(
                !automation.Settings.SimulatedUdpInputEnabled && !input.SimulatedUdpInputEnabled,
                "关闭模拟测试开关必须立即关闭实际测试源");
            form.SettingsPage.AlwaysOutputUdpCheckBox.Checked = false;
            Require(
                !input.AlwaysOutputUdpEnabled &&
                !automation.Settings.AlwaysOutputUdpEnabled,
                "旧版模式下关闭始终 UDP 输出必须同步关闭有效转发和保存偏好");
            form.SettingsPage.LegacySingleBoardFirmwareCheckBox.Checked = false;
            Require(
                !legacyUdpRow.Visible &&
                input.AlwaysOutputUdpEnabled &&
                !automation.Settings.AlwaysOutputUdpEnabled,
                "双板模式隐藏旧版开关时 UDP 有效转发恒开启，并保留关闭偏好");
            form.SettingsPage.LegacySingleBoardFirmwareCheckBox.Checked = true;
            Require(
                legacyUdpRow.Visible &&
                !input.AlwaysOutputUdpEnabled &&
                !automation.Settings.AlwaysOutputUdpEnabled,
                "重新启用旧版模式后应恢复此前保存的 UDP 关闭偏好");
            form.SettingsPage.AlwaysOutputUdpCheckBox.Checked = true;
            form.SettingsPage.LegacySingleBoardFirmwareCheckBox.Checked = false;
            input.SetForwardingEnabled(false);
            Require(
                !input.ForwardingEnabled && !input.PhysicalForwardingEnabled && !form.CaptureSurface.Forwarding,
                "结束布局路由检查前必须关闭 HOME，双板实体鼠标面板保持不锁定");
            Require(form.OutputSensitivityTrackBar.Minimum == 30 && form.OutputSensitivityTrackBar.Maximum == 300 && form.OutputSensitivityTrackBar.Value == 100, "输出灵敏度滑块范围或默认值不正确");
            Require(form.OutputSensitivityTextBox.Text == "1", "输出灵敏度输入框默认值必须为 1");
            Require(form.OutputSensitivityDescriptionLabel.Text.Contains("实体鼠标、UDP、Lua、宏", StringComparison.Ordinal), "输出灵敏度说明未覆盖四类输入来源");
            Label outputSensitivityLabel = EnumerateControls(form)
                .OfType<Label>()
                .Single(label => label.Text == "输出灵敏度");
            Require(
                outputSensitivityLabel.TextAlign == ContentAlignment.TopLeft &&
                outputSensitivityLabel.Padding.Top == 2,
                "输出灵敏度标签应在第一行上方对齐并保留完整字高");
            form.MainTabs.SelectedIndex = 0;
            form.MainTabs.PerformLayout();
            Application.DoEvents();
            Require(
                form.LanEndpointLabel.Visible &&
                form.LanEndpointLabel.Text == "UDP 192.168.1.20:24814" &&
                form.LanEndpointLabel.Height == form.SyncStatusLabel.Height &&
                form.LanEndpointLabel.Top == form.SyncStatusLabel.Top &&
                form.LanEndpointLabel.Width >= 150,
                $"左上角必须显示与同步状态同高且未裁切的局域网 UDP 监听 IP 和端口：" +
                $"status={form.SyncStatusLabel.Bounds},lan={form.LanEndpointLabel.Bounds}");
            FlowLayoutPanel udpOptionsLayout = EnumerateControls(form)
                .OfType<FlowLayoutPanel>()
                .Single(layout => layout.Controls.Contains(form.UdpSmoothingCheckBox));
            Require(
                !udpOptionsLayout.Controls.Contains(form.AlwaysOutputUdpCheckBox) &&
                form.UdpSmoothingCheckBox.Anchor == AnchorStyles.None &&
                form.UdpSmoothingCheckBox.Visible &&
                form.UdpSmoothingCheckBox.Text == "UDP 平滑" &&
                form.UdpSmoothingCheckBox.Width > 0 &&
                form.UdpSmoothingCheckBox.Right <= udpOptionsLayout.ClientSize.Width,
                $"UDP 平滑复选框必须留在输入页选项行并保持完整可见：" +
                $"right={form.UdpSmoothingCheckBox.Right}/{udpOptionsLayout.ClientSize.Width}," +
                $"anchor={form.UdpSmoothingCheckBox.Anchor}," +
                $"flow={udpOptionsLayout.FlowDirection}");
            Require(!form.SettingsPage.FirmwareUpdateApiCheckBox.Checked, "局域网固件刷写接口默认必须关闭");
            Require(!form.SettingsPage.FirmwareUpdateApiCheckBox.Enabled, "无串口刷写服务时设置页接口开关必须禁用");
            Require(
                form.SettingsPage.FirmwarePortComboBox.DropDownStyle == ComboBoxStyle.DropDown &&
                !form.SettingsPage.FirmwarePortComboBox.Enabled &&
                !form.SettingsPage.RefreshFirmwarePortsButton.Enabled,
                "本地刷写模块必须包含可输入串口选择栏和刷新按钮，非串口模式下应禁用");
            Button firmwareConfirmButton = EnumerateControls(form.SettingsPage)
                .OfType<Button>()
                .Single(button => button.Text == "确定");
            Require(
                firmwareConfirmButton.BackColor == Color.FromArgb(237, 243, 250) &&
                firmwareConfirmButton.ForeColor == Color.FromArgb(23, 35, 58),
                "本地固件刷写确定按钮必须使用普通按钮样式");
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
            Require(
                form.StatisticsButton.Text == "读取设备统计" &&
                !form.StatisticsButton.Enabled &&
                form.StatisticsButton.Visible,
                "非串口模式必须显示但禁用「读取设备统计」入口");
            Require(
                form.StatisticsButton.Parent is TableLayoutPanel,
                "「读取设备统计」入口必须与日志模式下拉框同排在日志栏顶部");
            Require(form.UdpSmoothingCheckBox.Checked, "UDP 平滑开关默认必须开启");
            Require(input.UdpSmoothingEnabled, "界面创建后 UDP 平滑状态应默认开启");
            Require(form.AlwaysOutputUdpCheckBox.Checked, "始终 UDP 输出偏好默认必须开启");
            Require(input.AlwaysOutputUdpEnabled, "界面创建后始终 UDP 输出状态应默认开启");
            form.UdpSmoothingCheckBox.Checked = false;
            Require(!input.UdpSmoothingEnabled, "界面开关未关闭 UDP 平滑");
            form.UdpSmoothingCheckBox.Checked = true;
            Require(input.UdpSmoothingEnabled, "界面开关未重新开启 UDP 平滑");
            input.SetForwardingEnabled(true);
            PumpUntilUi(
                () => !form.UdpSmoothingCheckBox.Enabled,
                "同步开启后的 ForwardingChanged UI 更新未完成");
            Require(!form.UdpSmoothingCheckBox.Enabled, "同步开启时 UDP 平滑开关必须禁用");
            input.SetForwardingEnabled(false);
            PumpUntilUi(
                () => form.UdpSmoothingCheckBox.Enabled,
                "同步关闭后的 ForwardingChanged UI 更新未完成");
            Require(form.UdpSmoothingCheckBox.Enabled, "同步关闭后 UDP 平滑开关必须恢复可用");
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
            Require(
                allText.Contains("UDP 模拟输入测试", StringComparison.Ordinal) &&
                !simulatedUdpRow.Visible &&
                !simulatedUdpCheckBox.Visible &&
                !simulatedUdpFrequency.Visible &&
                !simulatedUdpDescription.Visible,
                "新双板模式下模拟输入测试行及说明必须隐藏，但旧版设置仍可保留");
            Require(!allText.Contains("分析与诊断", StringComparison.Ordinal), "设置页仍残留分析与诊断卡片");
            Require(allText.Contains("UDP 平滑", StringComparison.Ordinal), "界面未显示 UDP 平滑开关");
            Require(allText.Contains("输出灵敏度", StringComparison.Ordinal), "界面未显示统一输出灵敏度控件");
            Require(allText.Contains("发送到固件前处理最终", StringComparison.Ordinal), "界面未说明输出灵敏度生效位置");
            Require(allText.Contains("始终开启 UDP 输出", StringComparison.Ordinal), "设置页未显示始终开启 UDP 输出开关");
            Require(allText.Contains("局域网固件刷写接口", StringComparison.Ordinal), "设置页未显示局域网固件刷写接口开关");
            Require(allText.Contains("暂停自动跟随", StringComparison.Ordinal), "日志栏未提示滚动到上方后暂停自动跟随");
            CheckDarkSurfaceTextContrast(form);
            string artifactDirectory = Path.Combine(Environment.CurrentDirectory, "artifacts");
            Directory.CreateDirectory(artifactDirectory);
            form.StartPosition = FormStartPosition.Manual;
            form.Location = new Point(-32000, -32000);
            form.ShowInTaskbar = false;
            form.Show();
            Application.DoEvents();
            form.MainTabs.SelectedIndex = 0;
            RenderControlToPng(form, Path.Combine(artifactDirectory, "automation-ui-capture.png"));
            form.MainTabs.SelectedIndex = 1;
            RenderControlToPng(form, Path.Combine(artifactDirectory, "automation-ui-macro.png"));
            form.MainTabs.SelectedIndex = 2;
            RenderControlToPng(form, Path.Combine(artifactDirectory, "automation-ui-lua-settings-update.png"));
            form.MainTabs.SelectedIndex = 3;
            RenderControlToPng(form, Path.Combine(artifactDirectory, "automation-ui-settings.png"));
            form.WindowState = FormWindowState.Normal;
            form.ClientSize = new Size(1010, 710);
            form.Close();
            AutomationSettings savedWindowSettings = store.LoadSettings();
            Require(
                savedWindowSettings.WindowWidth == 1010 && savedWindowSettings.WindowHeight == 710,
                $"关闭到托盘时窗口尺寸未保存：{savedWindowSettings.WindowWidth}x{savedWindowSettings.WindowHeight}");
            automation.Settings.CloseToTray = false;
            using Form auxiliary = new()
            {
                StartPosition = FormStartPosition.Manual,
                Location = new Point(-32000, -32000),
                ShowInTaskbar = false,
            };
            auxiliary.Show(form);
            form.ForceCloseForChecks();
            Application.DoEvents();
            Require(form.IsDisposed, "托盘明确退出后主窗口未关闭");
            Require(auxiliary.IsDisposed, "托盘明确退出后附属窗口未关闭");
            Console.WriteLine("四页 UI 检查：设置已从宏页迁出；宏/Lua 页含可复制接口说明和状态按钮；刷写模块含可输入串口选择与刷新；托盘退出会关闭主窗口和附属窗口。");
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

static void PumpUntilUi(Func<bool> condition, string timeoutMessage)
{
    long deadline = Environment.TickCount64 + 1000;
    while (!condition())
    {
        Application.DoEvents();
        if (Environment.TickCount64 >= deadline)
        {
            throw new InvalidOperationException(timeoutMessage);
        }
    }
}

static void CheckCopyableInterfaceDialog(Button trigger, string expectedTitle, string expectedText)
{
    Exception? failure = null;
    bool closedByButton = false;
    using System.Windows.Forms.Timer timer = new() { Interval = 20 };
    timer.Tick += (_, _) =>
    {
        CopyableTextDialog? dialog = Application.OpenForms
            .OfType<CopyableTextDialog>()
            .SingleOrDefault();
        if (dialog is null)
        {
            return;
        }

        try
        {
            Require(dialog.Text == expectedTitle, $"接口说明弹窗标题不正确：{dialog.Text}");
            Require(
                dialog.ContentTextBox.Multiline &&
                dialog.ContentTextBox.ReadOnly &&
                dialog.ContentTextBox.ScrollBars == ScrollBars.Both &&
                dialog.ContentTextBox.ShortcutsEnabled,
                "接口说明正文必须是只读、可选中复制、支持滚动的多行文本框");
            Require(dialog.ContentTextBox.Text.Contains(expectedText, StringComparison.Ordinal), "接口说明正文未包含当前实际 API 文案");
            dialog.ContentTextBox.Select(0, Math.Min(16, dialog.ContentTextBox.TextLength));
            Require(dialog.ContentTextBox.SelectedText.Length > 0, "接口说明正文必须支持文本选择");
            dialog.ContentTextBox.Copy();
            Require(dialog.AcceptButton == dialog.CloseButton && dialog.CancelButton == dialog.CloseButton, "接口说明弹窗必须支持确认和 Esc 关闭");
            dialog.CloseButton.PerformClick();
            closedByButton = true;
        }
        catch (Exception exception)
        {
            failure = exception;
            dialog.Close();
        }
        finally
        {
            timer.Stop();
        }
    };

    timer.Start();
    trigger.PerformClick();
    timer.Stop();
    if (failure is not null)
    {
        throw failure;
    }
    Require(closedByButton, $"点击“{expectedTitle}”按钮后未能通过关闭按钮关闭弹窗");
    Console.WriteLine($"{expectedTitle}弹窗检查：实际点击打开；标题、只读多行正文、选择复制属性和关闭按钮均通过。");
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

static void SetTextBoxInsertionPointWithoutScrolling(TextBox textBox, int position)
{
    const int emSetSel = 0x00B1;
    SendMessage(textBox.Handle, emSetSel, new IntPtr(position), new IntPtr(position));
}

static void CheckLogTextBoxAppenderFollowAtBottom()
{
    using Form window = new()
    {
        ClientSize = new Size(360, 120),
        StartPosition = FormStartPosition.Manual,
        Location = new Point(-32000, -32000),
        ShowInTaskbar = false,
    };
    using TextBox logTextBox = new()
    {
        Dock = DockStyle.Fill,
        Multiline = true,
        ReadOnly = true,
        ScrollBars = ScrollBars.Vertical,
        WordWrap = false,
        Font = new Font("Cascadia Mono", 9),
    };
    window.Controls.Add(logTextBox);
    window.Show();
    Application.DoEvents();

    logTextBox.Text = string.Join(
        Environment.NewLine,
        Enumerable.Range(1, 80).Select(index => $"历史日志 {index}")) + Environment.NewLine;
    logTextBox.Focus();
    logTextBox.SelectionStart = logTextBox.TextLength;
    logTextBox.SelectionLength = 0;
    logTextBox.ScrollToCaret();
    Application.DoEvents();
    Require(GetFirstVisibleLine(logTextBox) > 0, "测试未能建立位于底部的日志视图");

    // 插入点回到历史位置后，视图仍保持底部；Append 必须继续跟随而不能依赖插入点。
    int firstVisibleLineAtBottom = GetFirstVisibleLine(logTextBox);
    SetTextBoxInsertionPointWithoutScrolling(logTextBox, 2);
    Application.DoEvents();
    Require(
        GetFirstVisibleLine(logTextBox) == firstVisibleLineAtBottom,
        "测试未能保持历史插入点下的日志底部视图");
    LogTextBoxAppender.Append(logTextBox, ["底部跟随日志"], 64 * 1024);
    Require(
        logTextBox.SelectionLength == 0 && logTextBox.SelectionStart == logTextBox.TextLength,
        "日志视图在底部时不得因插入点位于历史位置而停止跟随");
    Require(
        GetFirstVisibleLine(logTextBox) >= firstVisibleLineAtBottom,
        "日志视图在底部且插入点位于历史位置时必须继续滚动到最新日志");
    window.Close();
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
        .Select(frame => frame.Payload.Length == MouseReportCodec.BridgeLength
            ? MouseReportCodec.TryDecodeBridge(frame.Payload, out BridgeMouseReport bridgeReport)
                ? bridgeReport.Report
                : throw new InvalidDataException("记录到无效桥接鼠标报告")
            : MouseReportCodec.TryDecode(frame.Payload, out MouseReport report)
                ? report
                : throw new InvalidDataException("记录到无效鼠标报告"))
        .ToArray();

    internal BridgeMouseReport[] BridgeMouseReports() => _frames
        .Where(frame => frame.Type == MessageType.MouseReport)
        .Select(frame => MouseReportCodec.TryDecodeBridge(frame.Payload, out BridgeMouseReport report)
            ? report
            : throw new InvalidDataException("记录到非 8 字节桥接鼠标报告"))
        .ToArray();

    internal long[] MouseTimestamps() => _frames
        .Where(frame => frame.Type == MessageType.MouseReport)
        .Select(frame => frame.Timestamp)
        .ToArray();

    internal int[] MousePayloadLengths() => _frames
        .Where(frame => frame.Type == MessageType.MouseReport)
        .Select(frame => frame.Payload.Length)
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
    public void ReleaseAll() => _actions.Enqueue("release-all");
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

internal sealed class GatedDeviceLogWriter : TextWriter
{
    internal readonly ManualResetEventSlim Entered = new(false);
    internal readonly ManualResetEventSlim Release = new(false);
    internal readonly StringBuilder Content = new();
    public override Encoding Encoding => Encoding.UTF8;
    public override void WriteLine(string? value)
    {
        Entered.Set();
        if (!Release.Wait(5000)) { throw new IOException("测试等待释放超时"); }
        Content.AppendLine(value);
    }
}

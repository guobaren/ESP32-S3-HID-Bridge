using System.Runtime.InteropServices;
using System.Collections.Concurrent;
using System.Net.Sockets;
using System.Text;
using HidBridge.Host;
using HidBridge.Host.Input;
using HidBridge.Host.RemoteInput;
using HidBridge.Host.Transport;
using HidBridge.Host.Ui;
using HidBridge.Protocol;

CheckMouseReportCodec();
CheckMouseAggregation();
CheckSerialDiscoveryProtocol();
CheckWiFiBoardTransportDisabled();
CheckDeviceLogPolicy();
CheckInputSuppressionPolicy();
CheckCursorLockGeometry();
CheckUiLogWriter();
CheckRemoteInputUdpPath();
CheckWindowLayout();
Console.WriteLine("全部主机检查通过：鼠标协议、500 Hz 聚合、串口握手、Wi-Fi 开发板输入禁用闸门、可切换日志策略、输入独占策略、UDP 模拟输入、实时日志和窗口布局。");

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
}

static void CheckCursorLockGeometry()
{
    NativeMethods.ClipRect rect = MouseCursorLock.CalculateClipRect(new System.Drawing.Point(321, 654));
    Require(rect.Left == 321 && rect.Top == 654, "鼠标锁定矩形左上角不正确");
    Require(rect.Right == 322 && rect.Bottom == 655, "鼠标锁定矩形必须限制为一个像素");
}

static void CheckUiLogWriter()
{
    string directory = Path.Combine(Path.GetTempPath(), $"hidbridge-log-{Guid.NewGuid():N}");
    string pathTemplate = Path.Combine(directory, "runtime-{timestamp}.log");
    try
    {
        using UiLogTextWriter writer = new();
        writer.EnableFile(pathTemplate);
        List<string> actual = [];
        writer.WriteLine("启动前日志");
        writer.Attach(actual.Add);
        writer.Write("运行中");
        writer.WriteLine("日志");
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
    const string token = "test-remote-input-key-123";
    RecordingTransport transport = new();
    using InputForwarder input = new(transport);
    input.SetForwardingEnabled(true);
    using RemoteInputServer server = new(
        "127.0.0.1",
        0,
        token,
        command => input.TryInjectMouseMovement(
            command.DeltaX,
            command.DeltaY,
            command.Wheel,
            command.Pan));
    server.Start();

    using UdpClient client = new();
    byte[] valid = Encoding.UTF8.GetBytes(
        "{\"token\":\"test-remote-input-key-123\",\"dx\":37,\"dy\":-19,\"wheel\":2}");
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
    byte[] invalid = Encoding.UTF8.GetBytes(
        "{\"token\":\"wrong-remote-input-key\",\"dx\":999,\"dy\":999}");
    client.Send(invalid, invalid.Length, "127.0.0.1", server.Port);
    Thread.Sleep(50);
    Require(transport.MouseReports().Length == reportCount, "错误密钥的 UDP 模拟输入不得进入鼠标报告链路");

    input.SetForwardingEnabled(false);
    byte[] disabled = Encoding.UTF8.GetBytes(
        "{\"token\":\"test-remote-input-key-123\",\"dx\":5,\"dy\":5}");
    client.Send(disabled, disabled.Length, "127.0.0.1", server.Port);
    Thread.Sleep(50);
    Require(transport.MouseReports().Length == reportCount, "同步关闭后 UDP 模拟输入不得继续发送鼠标报告");
}

static void CheckWindowLayout()
{
    Exception? failure = null;
    Thread thread = new(() =>
    {
        try
        {
            RecordingTransport transport = new();
            using InputForwarder input = new(transport);
            using BridgeMainForm form = new(input, "测试端点");
            _ = form.Handle;
            form.PerformLayout();
            double upperRatio = form.MainSplit.SplitterDistance / (double)form.ClientSize.Height;
            Require(upperRatio is >= 0.45 and <= 0.55, "窗口上半区必须约占客户区一半");
            Require(form.LogTextBox.Multiline && form.LogTextBox.ReadOnly, "日志栏必须为只读多行文本框");
            Require(!form.LogTextBox.WordWrap && form.LogTextBox.ScrollBars == ScrollBars.Both, "日志栏必须支持完整选择和双向滚动");
            Require(form.LogModeComboBox.DropDownStyle == ComboBoxStyle.DropDownList, "日志模式必须使用不可编辑下拉框");
            Require(form.LogModeComboBox.Items.Count == 2, "日志模式下拉框必须提供精简和完整两档");
            Require(form.LogModeComboBox.SelectedIndex == 0, "日志模式默认必须为精简高性能");
            form.LogModeComboBox.SelectedIndex = 1;
            Require(form.LogModeComboBox.SelectedIndex == 1, "日志模式必须能切换到完整诊断");
            form.LogModeComboBox.SelectedIndex = 0;
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
            Require(allText.Contains("暂停自动跟随", StringComparison.Ordinal), "日志栏未提示滚动到上方后暂停自动跟随");
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

    public void Dispose()
    {
    }
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

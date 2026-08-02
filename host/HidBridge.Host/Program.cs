using HidBridge.Host.Input;
using HidBridge.Host.Transport;
using HidBridge.Host.Ui;
using HidBridge.Protocol;

namespace HidBridge.Host;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        ApplicationConfiguration.Initialize();
        UiLogTextWriter logWriter = new();
        Console.SetOut(logWriter);
        Console.SetError(logWriter);

        IBridgeTransport? transport = null;
        InputForwarder? input = null;
        try
        {
            BridgeOptions options = BridgeOptions.Load();
            transport = options.Transport.Equals("wifi", StringComparison.OrdinalIgnoreCase)
                ? new NetworkBridge(options)
                : new SerialBridge(options);
            input = new InputForwarder(transport);

            string endpoint = options.Transport.Equals("wifi", StringComparison.OrdinalIgnoreCase)
                ? $"Wi-Fi {options.WiFiHost}:{options.WiFiPort}"
                : options.PortName.Equals("auto", StringComparison.OrdinalIgnoreCase) ||
                  string.IsNullOrWhiteSpace(options.PortName)
                    ? $"串口自动发现 @ {options.BaudRate}"
                    : $"串口 {options.PortName} @ {options.BaudRate}";

            using BridgeMainForm form = new(input, endpoint);
            logWriter.Attach(form.AppendLog);
            Console.WriteLine($"目标端点：{endpoint}");
            Console.WriteLine("HOME：开启/关闭同步；END：结束程序。");
            Console.WriteLine("同步开启后，除上述两个快捷键外的其他键鼠输入均只转发到对端。");

            input.Start();
            Application.Run(form);
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"主机程序启动或运行失败：{exception}");
            MessageBox.Show(
                exception.Message,
                "ESP32-S3 HID Bridge",
                MessageBoxButtons.OK,
                MessageBoxIcon.Error);
        }
        finally
        {
            input?.Stop();
            input?.Dispose();
            transport?.Dispose();
            logWriter.Dispose();
        }
    }
}

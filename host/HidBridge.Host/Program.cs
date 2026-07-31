using HidBridge.Host.Input;
using HidBridge.Host.Protocol;
using HidBridge.Host.Transport;

namespace HidBridge.Host;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        ApplicationConfiguration.Initialize();

        BridgeOptions options = BridgeOptions.Load();
        using SerialBridge transport = new(options);
        using InputForwarder input = new(transport, options.SuppressLocalInput);

        Console.CancelKeyPress += (_, eventArgs) =>
        {
            eventArgs.Cancel = true;
            input.Stop();
            Application.ExitThread();
        };

        input.ExitRequested += (_, _) => Application.ExitThread();
        input.ForwardingChanged += (_, enabled) =>
        {
            Console.WriteLine(enabled
                ? "键鼠转发已启用。"
                : "键鼠转发已停止。");
        };

        Console.WriteLine($"目标串口：{options.PortName} @ {options.BaudRate}");
        Console.WriteLine("Ctrl+Alt+F12：切换转发；Ctrl+Alt+F11：退出。");

        input.Start();
        Application.Run();
        input.Stop();
        transport.Send(MessageType.ReleaseAll, ReadOnlySpan<byte>.Empty);
    }
}

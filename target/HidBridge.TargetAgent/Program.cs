using System.ComponentModel;
using System.Net.Sockets;
using System.Security.Cryptography;
using HidBridge.Protocol;

namespace HidBridge.TargetAgent;

internal static class Program
{
    private static readonly WindowsInputInjector Injector = new();

    private static void Main()
    {
        AgentOptions options = AgentOptions.Load();
        using CancellationTokenSource cancellation = new();
        Console.CancelKeyPress += (_, eventArgs) =>
        {
            eventArgs.Cancel = true;
            cancellation.Cancel();
        };

        TcpListener listener = new(options.GetListenAddress(), options.Port);
        listener.Start(1);
        Console.WriteLine($"HidBridge Target Agent 正在监听 {options.ListenAddress}:{options.Port}。");
        Console.WriteLine("仅允许一个已认证的活动连接；Ctrl+C 安全退出。");

        try
        {
            while (!cancellation.IsCancellationRequested)
            {
                if (!listener.Pending())
                {
                    Thread.Sleep(100);
                    continue;
                }

                using TcpClient client = listener.AcceptTcpClient();
                HandleClient(client, options, cancellation.Token);
            }
        }
        finally
        {
            listener.Stop();
            Injector.ReleaseAll();
        }
    }

    private static void HandleClient(
        TcpClient client,
        AgentOptions options,
        CancellationToken cancellationToken)
    {
        string remote = client.Client.RemoteEndPoint?.ToString() ?? "未知地址";
        bool sessionStarted = false;
        DateTime lastActivityUtc = DateTime.MinValue;
        try
        {
            client.NoDelay = true;
            client.ReceiveTimeout = 3000;
            client.SendTimeout = 3000;
            using SecurePacketChannel channel = SecurePacketChannel.AuthenticateServer(
                client.GetStream(),
                options.PresharedKey);
            Console.WriteLine($"已认证开发板连接：{remote}");

            while (!cancellationToken.IsCancellationRequested)
            {
                if (!client.Client.Poll(100_000, SelectMode.SelectRead))
                {
                    if (sessionStarted &&
                        DateTime.UtcNow - lastActivityUtc >
                        TimeSpan.FromMilliseconds(options.LeaseTimeoutMilliseconds))
                    {
                        Console.Error.WriteLine("开发板心跳超时，已释放全部输入。");
                        break;
                    }
                    continue;
                }
                if (client.Client.Available == 0)
                {
                    break;
                }

                byte[] packet = channel.Receive();
                if (!FrameCodec.TryDecode(packet, out BridgeFrame frame))
                {
                    throw new InvalidDataException("收到无效的桥接帧。");
                }

                if (frame.Type == MessageType.SessionStart)
                {
                    Injector.ReleaseAll();
                    sessionStarted = true;
                    lastActivityUtc = DateTime.UtcNow;
                    continue;
                }
                if (!sessionStarted)
                {
                    continue;
                }

                lastActivityUtc = DateTime.UtcNow;
                switch (frame.Type)
                {
                    case MessageType.KeyboardReport:
                        Injector.ApplyKeyboardReport(frame.Payload);
                        break;
                    case MessageType.MouseReport:
                        Injector.ApplyMouseReport(frame.Payload);
                        break;
                    case MessageType.ReleaseAll:
                        Injector.ReleaseAll();
                        break;
                    case MessageType.Ping:
                        break;
                }
            }
        }
        catch (Exception exception) when (
            exception is IOException or SocketException or CryptographicException or
                InvalidDataException or Win32Exception)
        {
            Console.Error.WriteLine($"目标 Agent 连接已终止：{exception.Message}");
        }
        finally
        {
            Injector.ReleaseAll();
            Console.WriteLine($"开发板连接已关闭：{remote}");
        }
    }
}

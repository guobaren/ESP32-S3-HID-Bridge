using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;

namespace HidBridge.Host.FirmwareUpdate;

internal delegate bool FirmwareFlashStarter(out FirmwareFlashSnapshot snapshot);

internal sealed class FirmwareUpdateApiServer : IDisposable
{
    internal const string ConfirmationHeaderName = "X-HidBridge-Action";
    internal const string ConfirmationHeaderValue = "flash-firmware";
    private const int MaximumHeaderBytes = 8192;
    private readonly int _port;
    private readonly Func<FirmwareFlashSnapshot> _getSnapshot;
    private readonly FirmwareFlashStarter _tryStart;
    private readonly object _sync = new();
    private TcpListener? _listener;
    private CancellationTokenSource? _cancellation;
    private Task? _listenerTask;
    private bool _disposed;

    internal FirmwareUpdateApiServer(int port, FirmwareFlashService service)
        : this(port, service.GetSnapshot, service.TryStart)
    {
    }

    internal FirmwareUpdateApiServer(
        int port,
        Func<FirmwareFlashSnapshot> getSnapshot,
        FirmwareFlashStarter tryStart)
    {
        _port = port;
        _getSnapshot = getSnapshot;
        _tryStart = tryStart;
    }

    internal bool Enabled
    {
        get
        {
            lock (_sync)
            {
                return _listener is not null;
            }
        }
    }

    internal int Port
    {
        get
        {
            lock (_sync)
            {
                return _listener?.LocalEndpoint is IPEndPoint endpoint ? endpoint.Port : _port;
            }
        }
    }

    internal IPAddress ListenAddress => IPAddress.Loopback;
    internal bool FlashInProgress => _getSnapshot().State == "running";

    internal void SetEnabled(bool enabled)
    {
        lock (_sync)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (enabled == (_listener is not null))
            {
                return;
            }
            if (enabled)
            {
                StartLocked();
            }
            else
            {
                StopLocked();
            }
        }
    }

    private void StartLocked()
    {
        TcpListener listener = new(ListenAddress, _port);
        listener.Start();
        CancellationTokenSource cancellation = new();
        _listener = listener;
        _cancellation = cancellation;
        _listenerTask = Task.Run(() => AcceptLoopAsync(listener, cancellation.Token));
        int actualPort = ((IPEndPoint)listener.LocalEndpoint).Port;
        Console.WriteLine(
            $"本机固件刷写接口已启用：http://127.0.0.1:{actualPort}/api/v1/firmware；仅接受 Loopback 请求。");
    }

    private void StopLocked()
    {
        TcpListener? listener = _listener;
        CancellationTokenSource? cancellation = _cancellation;
        Task? listenerTask = _listenerTask;
        _listener = null;
        _cancellation = null;
        _listenerTask = null;
        cancellation?.Cancel();
        listener?.Stop();
        if (listenerTask is not null && Task.CurrentId != listenerTask.Id)
        {
            try
            {
                listenerTask.Wait(TimeSpan.FromSeconds(2));
            }
            catch (AggregateException exception) when (
                exception.InnerExceptions.All(inner => inner is OperationCanceledException or SocketException))
            {
                // 正常停止监听。
            }
        }
        cancellation?.Dispose();
        Console.WriteLine("本机固件刷写接口已关闭。");
    }

    private async Task AcceptLoopAsync(TcpListener listener, CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                TcpClient client = await listener.AcceptTcpClientAsync(cancellationToken).ConfigureAwait(false);
                _ = Task.Run(() => HandleClientAsync(client, cancellationToken), cancellationToken);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return;
            }
            catch (SocketException) when (cancellationToken.IsCancellationRequested)
            {
                return;
            }
            catch (Exception exception)
            {
                Console.Error.WriteLine($"本机固件刷写接口接受连接失败：{exception.Message}");
            }
        }
    }

    private async Task HandleClientAsync(TcpClient client, CancellationToken serverCancellation)
    {
        using (client)
        {
            try
            {
                if (client.Client.RemoteEndPoint is not IPEndPoint remote || !IPAddress.IsLoopback(remote.Address))
                {
                    return;
                }
                using NetworkStream stream = client.GetStream();
                using CancellationTokenSource requestTimeout =
                    CancellationTokenSource.CreateLinkedTokenSource(serverCancellation);
                requestTimeout.CancelAfter(TimeSpan.FromSeconds(5));
                string headerText = await ReadHeadersAsync(stream, requestTimeout.Token).ConfigureAwait(false);
                if (!TryParseRequest(headerText, out string method, out string path, out Dictionary<string, string> headers))
                {
                    await WriteJsonAsync(stream, 400, new { error = "invalid_request" }, requestTimeout.Token)
                        .ConfigureAwait(false);
                    return;
                }
                if (headers.TryGetValue("content-length", out string? contentLengthText) &&
                    (!int.TryParse(contentLengthText, out int contentLength) || contentLength != 0))
                {
                    await WriteJsonAsync(stream, 400, new { error = "request_body_not_allowed" }, requestTimeout.Token)
                        .ConfigureAwait(false);
                    return;
                }

                if (method == "GET" && path == "/api/v1/firmware/status")
                {
                    await WriteJsonAsync(stream, 200, _getSnapshot(), requestTimeout.Token)
                        .ConfigureAwait(false);
                    return;
                }
                if (method == "POST" && path == "/api/v1/firmware/flash")
                {
                    if (!headers.TryGetValue(ConfirmationHeaderName.ToLowerInvariant(), out string? confirmation) ||
                        !confirmation.Equals(ConfirmationHeaderValue, StringComparison.Ordinal))
                    {
                        await WriteJsonAsync(stream, 403, new { error = "confirmation_header_required" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }
                    bool started = _tryStart(out FirmwareFlashSnapshot snapshot);
                    await WriteJsonAsync(stream, started ? 202 : 409, snapshot, requestTimeout.Token)
                        .ConfigureAwait(false);
                    return;
                }

                await WriteJsonAsync(stream, 404, new { error = "not_found" }, requestTimeout.Token)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                // 客户端超时或监听关闭。
            }
            catch (IOException exception)
            {
                Console.Error.WriteLine($"本机固件刷写接口连接失败：{exception.Message}");
            }
        }
    }

    private static async Task<string> ReadHeadersAsync(NetworkStream stream, CancellationToken cancellationToken)
    {
        byte[] buffer = new byte[1024];
        using MemoryStream collected = new();
        while (collected.Length < MaximumHeaderBytes)
        {
            int read = await stream.ReadAsync(buffer, cancellationToken).ConfigureAwait(false);
            if (read <= 0)
            {
                break;
            }
            collected.Write(buffer, 0, read);
            string text = Encoding.ASCII.GetString(collected.GetBuffer(), 0, checked((int)collected.Length));
            int end = text.IndexOf("\r\n\r\n", StringComparison.Ordinal);
            if (end >= 0)
            {
                return text[..(end + 4)];
            }
        }
        throw new InvalidDataException("HTTP 请求头不完整或超过 8192 字节。");
    }

    internal static bool TryParseRequest(
        string headerText,
        out string method,
        out string path,
        out Dictionary<string, string> headers)
    {
        method = string.Empty;
        path = string.Empty;
        headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        string[] lines = headerText.Split("\r\n", StringSplitOptions.None);
        string[] requestLine = lines[0].Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (requestLine.Length != 3 || !requestLine[2].StartsWith("HTTP/1.", StringComparison.Ordinal))
        {
            return false;
        }
        method = requestLine[0].ToUpperInvariant();
        path = requestLine[1].Split('?', 2)[0];
        for (int index = 1; index < lines.Length && lines[index].Length > 0; index++)
        {
            int separator = lines[index].IndexOf(':');
            if (separator <= 0)
            {
                return false;
            }
            string name = lines[index][..separator].Trim();
            string value = lines[index][(separator + 1)..].Trim();
            if (!headers.TryAdd(name, value))
            {
                return false;
            }
        }
        return method is "GET" or "POST";
    }

    private static async Task WriteJsonAsync(
        NetworkStream stream,
        int statusCode,
        object body,
        CancellationToken cancellationToken)
    {
        byte[] payload = JsonSerializer.SerializeToUtf8Bytes(body, new JsonSerializerOptions
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        });
        string reason = statusCode switch
        {
            200 => "OK",
            202 => "Accepted",
            400 => "Bad Request",
            403 => "Forbidden",
            404 => "Not Found",
            409 => "Conflict",
            _ => "Error",
        };
        byte[] headers = Encoding.ASCII.GetBytes(
            $"HTTP/1.1 {statusCode} {reason}\r\n" +
            "Content-Type: application/json; charset=utf-8\r\n" +
            $"Content-Length: {payload.Length}\r\n" +
            "Cache-Control: no-store\r\n" +
            "Connection: close\r\n\r\n");
        await stream.WriteAsync(headers, cancellationToken).ConfigureAwait(false);
        await stream.WriteAsync(payload, cancellationToken).ConfigureAwait(false);
    }

    public void Dispose()
    {
        lock (_sync)
        {
            if (_disposed)
            {
                return;
            }
            if (_listener is not null)
            {
                StopLocked();
            }
            _disposed = true;
        }
    }
}

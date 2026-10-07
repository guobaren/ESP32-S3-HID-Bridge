using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using HidBridge.Host.Transport;

namespace HidBridge.Host.FirmwareUpdate;

internal delegate bool FirmwareFlashStarter(
    string manifestPath,
    string portName,
    out FirmwareFlashSnapshot snapshot);

/// <summary>
/// 刷写请求。<paramref name="PortName"/>（2026-09-28 新增）为**可选**：
/// 远程调用可以显式指定端口；不填则回落到 UI 里选择的刷写端口（默认行为不变）。
/// </summary>
internal sealed record FirmwareFlashRequest(string ManifestPath, string? PortName = null);

internal sealed record SerialCommandWriteRequest(string? PortName, string? Hex);

internal sealed class FirmwareUpdateApiServer : IDisposable
{
    internal const string ConfirmationHeaderName = "X-HidBridge-Action";
    internal const string ConfirmationHeaderValue = "flash-firmware";
    private const int MaximumHeaderBytes = 8192;
    private const int MaximumBodyBytes = 4096;
    private const int MaximumSerialRequestBodyBytes = 16384;
    private const int MaximumSerialWriteBytes = 4096;

    /// <summary>
    /// 校验远程传入的端口名（2026-09-28）：只接受 `COM` + 数字形式的 Windows 串口名，
    /// 避免请求体把任意字符串当作端口转交给刷写器。
    /// </summary>
    private static bool IsValidPortName(string value)
    {
        if (value.Length <= 3 || value.Length > 16 ||
            !value.StartsWith("COM", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        for (int index = 3; index < value.Length; index++)
        {
            if (!char.IsAsciiDigit(value[index]))
            {
                return false;
            }
        }

        return true;
    }
    private readonly int _port;
    private readonly Func<FirmwareFlashSnapshot> _getSnapshot;
    private readonly FirmwareFlashStarter _tryStart;
    private readonly Func<string?> _selectedPortProvider;
    private readonly Func<IReadOnlyList<string>>? _getOpenSerialPorts;
    private readonly Func<string, byte[], CancellationToken, int?>? _writeSerialPort;
    private readonly Func<CancellationToken, SerialPortRefreshSnapshot>? _refreshSerialPorts;
    private readonly Func<TimeSpan, CancellationToken, Task<DeviceStatisticsQueryResult>>? _readDeviceStatistics;
    private readonly bool _serialApiConfigured;
    private readonly object _sync = new();
    private TcpListener? _listener;
    private CancellationTokenSource? _cancellation;
    private Task? _listenerTask;
    private bool _disposed;
    private volatile bool _firmwareApiEnabled;

    internal FirmwareUpdateApiServer(
        int port,
        FirmwareFlashService service,
        Func<string?> selectedPortProvider,
        Func<IReadOnlyList<string>>? getOpenSerialPorts = null,
        Func<string, byte[], CancellationToken, int?>? writeSerialPort = null,
        Func<CancellationToken, SerialPortRefreshSnapshot>? refreshSerialPorts = null,
        Func<TimeSpan, CancellationToken, Task<DeviceStatisticsQueryResult>>? readDeviceStatistics = null)
        : this(
            port,
            service.GetSnapshot,
            service.TryStartFromManifest,
            selectedPortProvider,
            getOpenSerialPorts,
            writeSerialPort,
            refreshSerialPorts,
            readDeviceStatistics)
    {
    }

    internal FirmwareUpdateApiServer(
        int port,
        Func<FirmwareFlashSnapshot> getSnapshot,
        FirmwareFlashStarter tryStart,
        Func<string?> selectedPortProvider,
        Func<IReadOnlyList<string>>? getOpenSerialPorts = null,
        Func<string, byte[], CancellationToken, int?>? writeSerialPort = null,
        Func<CancellationToken, SerialPortRefreshSnapshot>? refreshSerialPorts = null,
        Func<TimeSpan, CancellationToken, Task<DeviceStatisticsQueryResult>>? readDeviceStatistics = null)
    {
        _port = port;
        _getSnapshot = getSnapshot;
        _tryStart = tryStart;
        _selectedPortProvider = selectedPortProvider;
        _getOpenSerialPorts = getOpenSerialPorts;
        _writeSerialPort = writeSerialPort;
        _refreshSerialPorts = refreshSerialPorts;
        _readDeviceStatistics = readDeviceStatistics;
        _serialApiConfigured = getOpenSerialPorts is not null && writeSerialPort is not null &&
                               refreshSerialPorts is not null;
    }

    internal bool Enabled
    {
        get
        {
            lock (_sync)
            {
                return _firmwareApiEnabled;
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

    internal IPAddress ListenAddress => IPAddress.Any;
    internal bool FlashInProgress => _getSnapshot().State == "running";

    internal void SetEnabled(bool enabled)
    {
        lock (_sync)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            bool wasEnabled = _firmwareApiEnabled;
            bool wasListening = _listener is not null;
            _firmwareApiEnabled = enabled;
            bool shouldListen = enabled || _serialApiConfigured;
            if (shouldListen && _listener is null)
            {
                StartLocked();
            }
            else if (!shouldListen && _listener is not null)
            {
                StopLocked();
            }

            if (wasEnabled != enabled && wasListening && _listener is not null)
            {
                Console.WriteLine(enabled
                    ? "局域网固件刷写 API 已启用。"
                    : "局域网固件刷写 API 已关闭；loopback 串口命令 API 继续运行。");
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
        if (_firmwareApiEnabled)
        {
            Console.WriteLine(
                $"局域网固件刷写接口已启用：0.0.0.0:{actualPort}/api/v1/firmware；仅限受信任局域网。");
        }
        if (_serialApiConfigured)
        {
            Console.WriteLine($"loopback 串口命令 API 已启用：127.0.0.1:{actualPort}/api/v1/serial。");
        }
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
        Console.WriteLine("主机 HTTP API 监听已关闭。");
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
                Console.Error.WriteLine($"局域网固件刷写接口接受连接失败：{exception.Message}");
            }
        }
    }

    private async Task HandleClientAsync(TcpClient client, CancellationToken serverCancellation)
    {
        using (client)
        {
            try
            {
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
                if (path == "/api/v1/serial/refresh")
                {
                    requestTimeout.CancelAfter(TimeSpan.FromSeconds(15));
                }
                else if (path == "/api/v1/serial/stats")
                {
                    requestTimeout.CancelAfter(TimeSpan.FromSeconds(10));
                }
                bool serialRoute = path is
                    "/api/v1/serial/ports" or
                    "/api/v1/serial/write" or
                    "/api/v1/serial/refresh" or
                    "/api/v1/serial/stats";
                if (serialRoute && !IsLoopbackClient(client))
                {
                    await WriteJsonAsync(stream, 403, new { error = "loopback_only" }, requestTimeout.Token)
                        .ConfigureAwait(false);
                    return;
                }
                bool firmwareRoute = path is "/api/v1/firmware/status" or "/api/v1/firmware/flash";
                if (firmwareRoute && !_firmwareApiEnabled)
                {
                    await WriteJsonAsync(stream, 404, new { error = "not_found" }, requestTimeout.Token)
                        .ConfigureAwait(false);
                    return;
                }

                int maximumBodyBytes = path == "/api/v1/serial/write"
                    ? MaximumSerialRequestBodyBytes
                    : MaximumBodyBytes;
                int contentLength = 0;
                if (headers.TryGetValue("content-length", out string? contentLengthText) &&
                    (!int.TryParse(contentLengthText, out contentLength) ||
                     contentLength < 0))
                {
                    await WriteJsonAsync(stream, 400, new { error = "invalid_content_length" }, requestTimeout.Token)
                        .ConfigureAwait(false);
                    return;
                }
                if (contentLength > maximumBodyBytes)
                {
                    int statusCode = path == "/api/v1/serial/write" ? 413 : 400;
                    await WriteJsonAsync(
                            stream,
                            statusCode,
                            new { error = statusCode == 413 ? "request_body_too_large" : "invalid_content_length" },
                            requestTimeout.Token)
                        .ConfigureAwait(false);
                    return;
                }

                if (method == "POST" && path == "/api/v1/serial/refresh")
                {
                    if (contentLength != 0)
                    {
                        await WriteJsonAsync(
                                stream,
                                400,
                                new { error = "request_body_not_allowed" },
                                requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }
                    if (_refreshSerialPorts is null)
                    {
                        await WriteJsonAsync(stream, 503, new { error = "serial_api_unavailable" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }
                    try
                    {
                        SerialPortRefreshSnapshot result = _refreshSerialPorts(requestTimeout.Token);
                        await WriteJsonAsync(
                                stream,
                                200,
                                new { availablePorts = result.AvailablePorts, openPorts = result.OpenPorts },
                                requestTimeout.Token)
                            .ConfigureAwait(false);
                    }
                    catch (OperationCanceledException) when (requestTimeout.IsCancellationRequested)
                    {
                        return;
                    }
                    catch (Exception exception)
                    {
                        Console.Error.WriteLine($"串口刷新失败：{exception.Message}");
                        await WriteJsonAsync(
                                stream,
                                500,
                                new { error = "serial_refresh_failed", message = exception.Message },
                                requestTimeout.Token)
                            .ConfigureAwait(false);
                    }
                    return;
                }

                if (method == "GET" && path == "/api/v1/serial/ports")
                {
                    if (contentLength != 0)
                    {
                        await WriteJsonAsync(stream, 400, new { error = "request_body_not_allowed" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }
                    if (_getOpenSerialPorts is null)
                    {
                        await WriteJsonAsync(stream, 503, new { error = "serial_api_unavailable" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }
                    try
                    {
                        string[] ports = _getOpenSerialPorts()
                            .Where(name => !string.IsNullOrWhiteSpace(name) && IsValidPortName(name))
                            .Distinct(StringComparer.OrdinalIgnoreCase)
                            .ToArray();
                        await WriteJsonAsync(stream, 200, new { ports }, requestTimeout.Token)
                            .ConfigureAwait(false);
                    }
                    catch (Exception exception)
                    {
                        await WriteJsonAsync(
                                stream,
                                500,
                                new { error = "serial_port_list_failed", message = exception.Message },
                                requestTimeout.Token)
                            .ConfigureAwait(false);
                    }
                    return;
                }

                if (method == "GET" && path == "/api/v1/serial/stats")
                {
                    if (contentLength != 0)
                    {
                        await WriteJsonAsync(stream, 400, new { error = "request_body_not_allowed" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }
                    if (_readDeviceStatistics is null)
                    {
                        await WriteJsonAsync(stream, 503, new { error = "serial_stats_unavailable" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }
                    try
                    {
                        DeviceStatisticsQueryResult result = await _readDeviceStatistics(
                            TimeSpan.FromSeconds(4),
                            requestTimeout.Token).ConfigureAwait(false);
                        await WriteJsonAsync(stream, 200, result, requestTimeout.Token).ConfigureAwait(false);
                    }
                    catch (OperationCanceledException) when (requestTimeout.IsCancellationRequested)
                    {
                        return;
                    }
                    catch (Exception exception)
                    {
                        Console.Error.WriteLine($"设备统计查询失败：{exception.Message}");
                        await WriteJsonAsync(
                                stream,
                                exception is TimeoutException ? 504 : 500,
                                new { error = "serial_stats_failed", message = exception.Message },
                                requestTimeout.Token)
                            .ConfigureAwait(false);
                    }
                    return;
                }

                if (method == "POST" && path == "/api/v1/serial/write")
                {
                    if (_writeSerialPort is null)
                    {
                        await WriteJsonAsync(stream, 503, new { error = "serial_api_unavailable" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }
                    if (contentLength == 0)
                    {
                        await WriteJsonAsync(stream, 400, new { error = "request_body_required" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }

                    string body = await ReadBodyAsync(stream, contentLength, requestTimeout.Token)
                        .ConfigureAwait(false);
                    SerialCommandWriteRequest? request;
                    try
                    {
                        request = JsonSerializer.Deserialize<SerialCommandWriteRequest>(
                            body,
                            new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
                    }
                    catch (JsonException)
                    {
                        await WriteJsonAsync(stream, 400, new { error = "invalid_json" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }

                    if (request is null || string.IsNullOrWhiteSpace(request.PortName))
                    {
                        await WriteJsonAsync(stream, 400, new { error = "port_name_required" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }
                    if (!IsValidPortName(request.PortName))
                    {
                        await WriteJsonAsync(stream, 400, new { error = "invalid_port_name" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }
                    if (string.IsNullOrEmpty(request.Hex))
                    {
                        await WriteJsonAsync(stream, 400, new { error = "hex_required" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }
                    if (request.Hex.Length > MaximumSerialWriteBytes * 2)
                    {
                        await WriteJsonAsync(stream, 413, new { error = "serial_write_too_large" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }
                    if (request.Hex.Length % 2 != 0 || request.Hex.Any(character => !char.IsAsciiHexDigit(character)))
                    {
                        await WriteJsonAsync(stream, 400, new { error = "invalid_hex" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }

                    byte[] bytes = Convert.FromHexString(request.Hex);
                    int? bytesWritten;
                    try
                    {
                        bytesWritten = _writeSerialPort(request.PortName, bytes, requestTimeout.Token);
                    }
                    catch (OperationCanceledException) when (requestTimeout.IsCancellationRequested)
                    {
                        return;
                    }
                    catch (Exception exception)
                    {
                        Console.Error.WriteLine($"串口命令写入 {request.PortName} 失败：{exception.Message}");
                        await WriteJsonAsync(
                                stream,
                                exception is TimeoutException ? 504 : 500,
                                new { error = "serial_write_failed", message = exception.Message },
                                requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }

                    if (bytesWritten is null)
                    {
                        await WriteJsonAsync(stream, 409, new { error = "serial_port_not_open" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }
                    if (bytesWritten.Value != bytes.Length)
                    {
                        await WriteJsonAsync(
                                stream,
                                500,
                                new
                                {
                                    error = "serial_write_incomplete",
                                    expectedBytes = bytes.Length,
                                    bytesWritten = bytesWritten.Value,
                                },
                                requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }

                    await WriteJsonAsync(
                            stream,
                            200,
                            new { portName = request.PortName, bytesWritten = bytesWritten.Value, queued = false },
                            requestTimeout.Token)
                        .ConfigureAwait(false);
                    return;
                }

                if (method == "GET" && path == "/api/v1/firmware/status")
                {
                    if (contentLength != 0)
                    {
                        await WriteJsonAsync(stream, 400, new { error = "request_body_not_allowed" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }
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
                    if (contentLength == 0)
                    {
                        await WriteJsonAsync(stream, 400, new { error = "manifest_path_required" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }
                    string body = await ReadBodyAsync(stream, contentLength, requestTimeout.Token)
                        .ConfigureAwait(false);
                    FirmwareFlashRequest? request;
                    try
                    {
                        request = JsonSerializer.Deserialize<FirmwareFlashRequest>(
                            body,
                            new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
                    }
                    catch (JsonException)
                    {
                        await WriteJsonAsync(stream, 400, new { error = "invalid_json" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }
                    if (string.IsNullOrWhiteSpace(request?.ManifestPath))
                    {
                        await WriteJsonAsync(stream, 400, new { error = "manifest_path_required" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }
                    /*
                     * 端口选择（2026-09-28）：请求里显式给了 portName 就用它（远程刷写可以
                     * 指定任意一块板），否则回落到 UI 选择的刷写端口——默认行为不变。
                     */
                    string requestedPort = request.PortName?.Trim() ?? string.Empty;
                    if (requestedPort.Length > 0 && !IsValidPortName(requestedPort))
                    {
                        await WriteJsonAsync(stream, 400, new { error = "invalid_port_name" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }
                    string selectedPort = requestedPort.Length > 0
                        ? requestedPort
                        : _selectedPortProvider()?.Trim() ?? string.Empty;
                    if (string.IsNullOrWhiteSpace(selectedPort))
                    {
                        await WriteJsonAsync(stream, 400, new { error = "firmware_flash_port_not_selected" }, requestTimeout.Token)
                            .ConfigureAwait(false);
                        return;
                    }
                    bool started = _tryStart(
                        request.ManifestPath,
                        selectedPort,
                        out FirmwareFlashSnapshot snapshot);
                    int responseCode = started
                        ? 202
                        : snapshot.State == "failed" && string.IsNullOrEmpty(snapshot.JobId)
                            ? 400
                            : 409;
                    await WriteJsonAsync(stream, responseCode, snapshot, requestTimeout.Token)
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
                Console.Error.WriteLine($"局域网固件刷写接口连接失败：{exception.Message}");
            }
        }
    }

    private static async Task<string> ReadHeadersAsync(NetworkStream stream, CancellationToken cancellationToken)
    {
        byte[] buffer = new byte[1];
        using MemoryStream collected = new();
        while (collected.Length < MaximumHeaderBytes)
        {
            int read = await stream.ReadAsync(buffer, cancellationToken).ConfigureAwait(false);
            if (read <= 0)
            {
                break;
            }
            collected.Write(buffer, 0, read);
            if (collected.Length >= 4)
            {
                byte[] bytes = collected.GetBuffer();
                int length = checked((int)collected.Length);
                if (bytes[length - 4] == '\r' && bytes[length - 3] == '\n' &&
                    bytes[length - 2] == '\r' && bytes[length - 1] == '\n')
                {
                    return Encoding.ASCII.GetString(bytes, 0, length);
                }
            }
        }
        throw new InvalidDataException("HTTP 请求头不完整或超过 8192 字节。");
    }

    private static async Task<string> ReadBodyAsync(
        NetworkStream stream,
        int contentLength,
        CancellationToken cancellationToken)
    {
        byte[] body = new byte[contentLength];
        int offset = 0;
        while (offset < body.Length)
        {
            int read = await stream.ReadAsync(body.AsMemory(offset), cancellationToken).ConfigureAwait(false);
            if (read <= 0)
            {
                throw new InvalidDataException("HTTP 请求正文不完整。");
            }
            offset += read;
        }
        return Encoding.UTF8.GetString(body);
    }

    private static bool IsLoopbackClient(TcpClient client)
    {
        if (client.Client.RemoteEndPoint is not IPEndPoint remoteEndpoint)
        {
            return false;
        }

        return IsLoopbackAddress(remoteEndpoint.Address);
    }

    internal static bool IsLoopbackAddress(IPAddress address)
    {
        if (address.IsIPv4MappedToIPv6)
        {
            address = address.MapToIPv4();
        }
        return IPAddress.IsLoopback(address);
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
            413 => "Payload Too Large",
            500 => "Internal Server Error",
            503 => "Service Unavailable",
            504 => "Gateway Timeout",
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

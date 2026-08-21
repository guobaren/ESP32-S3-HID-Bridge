using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using HidBridge.Host.Input;
using HidBridge.Host.Transport;
using HidBridge.Protocol;

namespace HidBridge.Host.FirmwareUpdate;

internal sealed record FirmwareFlashSnapshot(
    string JobId,
    string State,
    string Message,
    DateTimeOffset? StartedAt,
    DateTimeOffset? FinishedAt,
    int? ExitCode,
    string? PortName);

internal sealed class FirmwareFlashService : IDisposable
{
    private readonly BridgeOptions _options;
    private readonly SerialBridge _serialBridge;
    private readonly InputForwarder _input;
    private readonly object _sync = new();
    private FirmwareFlashSnapshot _snapshot = new("", "idle", "尚未启动刷写任务。", null, null, null, null);
    private Task? _activeTask;
    private bool _disposed;

    internal FirmwareFlashService(BridgeOptions options, SerialBridge serialBridge, InputForwarder input)
    {
        _options = options;
        _serialBridge = serialBridge;
        _input = input;
    }

    /// <summary>刷写进度日志（后台线程触发，UI 需自行封送到界面线程）。</summary>
    internal event Action<string>? Log;

    internal FirmwareFlashSnapshot GetSnapshot()
    {
        lock (_sync)
        {
            return _snapshot;
        }
    }

    /// <summary>从调用方指定的本地 JSON 清单创建计划并启动刷写。</summary>
    internal bool TryStartFromManifest(string manifestPath, out FirmwareFlashSnapshot snapshot)
    {
        FirmwareFlashPlan plan;
        try
        {
            if (string.IsNullOrWhiteSpace(manifestPath) || !Path.IsPathFullyQualified(manifestPath))
            {
                throw new ArgumentException("JSON 刷写清单必须使用本机绝对路径。", nameof(manifestPath));
            }
            plan = FirmwareFlashPlan.LoadFromManifest(manifestPath);
        }
        catch (Exception exception) when (
            exception is IOException or UnauthorizedAccessException or ArgumentException or JsonException)
        {
            lock (_sync)
            {
                _snapshot = new(
                    "",
                    "failed",
                    $"无法准备刷写计划：{exception.Message}",
                    null,
                    DateTimeOffset.Now,
                    null,
                    null);
                snapshot = _snapshot;
            }
            return false;
        }
        return TryStart(plan, out snapshot);
    }

    /// <summary>使用已校验的刷写计划启动底层任务。</summary>
    internal bool TryStart(FirmwareFlashPlan plan, out FirmwareFlashSnapshot snapshot)
    {
        lock (_sync)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            if (_activeTask is { IsCompleted: false })
            {
                snapshot = _snapshot;
                return false;
            }

            string jobId = Guid.NewGuid().ToString("N");
            _snapshot = new(
                jobId,
                "running",
                "正在准备固件和串口。",
                DateTimeOffset.Now,
                null,
                null,
                null);
            _activeTask = Task.Run(() => RunAsync(jobId, plan));
            snapshot = _snapshot;
            return true;
        }
    }

    private async Task RunAsync(string jobId, FirmwareFlashPlan plan)
    {
        bool restoreForwarding = _input.ForwardingEnabled;
        SerialBridge.FirmwareUpdatePortLease? lease = null;
        int? exitCode = null;
        string? portName = null;
        try
        {
            _input.DisableForwarding();
            await Task.Delay(100).ConfigureAwait(false);
            lease = _serialBridge.AcquireFirmwareUpdatePort();
            portName = lease.PortName;
            SetRunning(jobId, $"正在通过 {portName} 刷写固件。", portName);
            WriteLog(
                $"固件刷写任务 {jobId}：端口={portName}，esptool={plan.EsptoolPath}，build={plan.BuildDirectory}。");
            foreach (FirmwareFlashImage image in plan.Images)
            {
                WriteLog(
                    $"固件镜像：offset={image.OffsetArgument}，bytes={new FileInfo(image.Path).Length}，SHA-256={image.Sha256}。");
            }

            (exitCode, string output) = await RunEsptoolAsync(plan, portName).ConfigureAwait(false);
            int verifiedImages = Regex.Matches(
                output,
                "Hash of data verified",
                RegexOptions.IgnoreCase | RegexOptions.CultureInvariant).Count;
            bool hardReset = output.Contains("Hard resetting via RTS pin", StringComparison.OrdinalIgnoreCase);
            if (exitCode != 0)
            {
                throw new InvalidOperationException($"esptool 退出码为 {exitCode}。");
            }
            if (verifiedImages < plan.Images.Count)
            {
                throw new InvalidOperationException(
                    $"设备端哈希校验不足：期望 {plan.Images.Count} 段，实际 {verifiedImages} 段。");
            }
            if (!hardReset)
            {
                throw new InvalidOperationException("刷写输出缺少 RTS 硬复位完成标记。");
            }
            WriteLog(FormatVerificationSummary(plan.Images, verifiedImages, hardReset));

            lease.Dispose();
            lease = null;
            bool reconnected = await _serialBridge.WaitForConnectionAsync(
                TimeSpan.FromSeconds(15),
                CancellationToken.None).ConfigureAwait(false);
            if (!reconnected)
            {
                throw new TimeoutException("刷写成功，但控制软件未能在 15 秒内重新连接串口。");
            }
            _serialBridge.Send(MessageType.Ping, ReadOnlySpan<byte>.Empty);
            Complete(jobId, "succeeded", "三段固件哈希校验通过，RTS 复位并恢复串口连接。", exitCode, portName);
        }
        catch (Exception exception)
        {
            WriteLog($"固件刷写任务 {jobId} 失败：{exception.Message}");
            Complete(jobId, "failed", exception.Message, exitCode, portName);
        }
        finally
        {
            lease?.Dispose();
            if (restoreForwarding)
            {
                _input.SetForwardingEnabled(true);
            }
        }
    }

    private async Task<(int ExitCode, string Output)> RunEsptoolAsync(
        FirmwareFlashPlan plan,
        string portName)
    {
        ProcessStartInfo startInfo = new(plan.EsptoolPath)
        {
            WorkingDirectory = plan.BuildDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        foreach (string argument in new[]
                 {
                     "--chip", plan.Chip,
                     "--port", portName,
                     "--baud", _options.FirmwareFlashBaudRate.ToString(System.Globalization.CultureInfo.InvariantCulture),
                     "--before", plan.Before,
                     "--after", plan.After,
                     "write-flash",
                 })
        {
            startInfo.ArgumentList.Add(argument);
        }
        foreach (string argument in plan.WriteFlashArguments)
        {
            startInfo.ArgumentList.Add(argument);
        }
        foreach (FirmwareFlashImage image in plan.Images)
        {
            startInfo.ArgumentList.Add(image.OffsetArgument);
            startInfo.ArgumentList.Add(image.Path);
        }

        using Process process = new() { StartInfo = startInfo };
        if (!process.Start())
        {
            throw new InvalidOperationException("无法启动 esptool。");
        }
        StringBuilder output = new();
        object outputLock = new();
        Task stdout = CaptureOutputAsync(process.StandardOutput, output, outputLock);
        Task stderr = CaptureOutputAsync(process.StandardError, output, outputLock);
        using CancellationTokenSource timeout = new(
            TimeSpan.FromSeconds(_options.FirmwareFlashTimeoutSeconds));
        try
        {
            await process.WaitForExitAsync(timeout.Token).ConfigureAwait(false);
            await Task.WhenAll(stdout, stderr).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            try
            {
                process.Kill(entireProcessTree: true);
            }
            catch (InvalidOperationException)
            {
                // 进程已自行退出。
            }
            throw new TimeoutException($"esptool 超过 {_options.FirmwareFlashTimeoutSeconds} 秒未完成。");
        }
        return (process.ExitCode, output.ToString());
    }

    private async Task CaptureOutputAsync(
        StreamReader reader,
        StringBuilder output,
        object outputLock)
    {
        while (await reader.ReadLineAsync().ConfigureAwait(false) is { } line)
        {
            lock (outputLock)
            {
                output.AppendLine(line);
            }
            WriteLog($"[刷写] {line}");
        }
    }

    internal static string FormatVerificationSummary(
        IReadOnlyList<FirmwareFlashImage> images,
        int verifiedImages,
        bool hardReset)
    {
        string hashes = string.Join(
            "；",
            images.Select(image => $"{image.OffsetArgument}={image.Sha256}"));
        return $"固件刷写最终摘要：{hashes}；设备校验={verifiedImages}/{images.Count}；" +
               $"RTS复位={(hardReset ? "完成" : "未确认")}";
    }

    private void WriteLog(string message)
    {
        Console.WriteLine(message);
        Log?.Invoke(message);
    }

    private void SetRunning(string jobId, string message, string portName)
    {
        lock (_sync)
        {
            if (_snapshot.JobId == jobId)
            {
                _snapshot = _snapshot with { Message = message, PortName = portName };
            }
        }
    }

    private void Complete(string jobId, string state, string message, int? exitCode, string? portName)
    {
        lock (_sync)
        {
            if (_snapshot.JobId == jobId)
            {
                _snapshot = _snapshot with
                {
                    State = state,
                    Message = message,
                    FinishedAt = DateTimeOffset.Now,
                    ExitCode = exitCode,
                    PortName = portName,
                };
            }
        }
        WriteLog($"刷写任务 {jobId} 结束：{state} - {message}");
    }

    public void Dispose()
    {
        Task? activeTask;
        lock (_sync)
        {
            if (_disposed)
            {
                return;
            }
            _disposed = true;
            activeTask = _activeTask;
        }
        if (activeTask is { IsCompleted: false })
        {
            Console.WriteLine("程序正在退出，等待进行中的固件刷写安全结束。");
            activeTask.GetAwaiter().GetResult();
        }
    }
}

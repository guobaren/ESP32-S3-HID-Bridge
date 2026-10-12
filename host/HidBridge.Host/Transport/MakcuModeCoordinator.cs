namespace HidBridge.Host.Transport;

internal readonly record struct MakcuModeSnapshot(bool Enabled, int BaudRate, byte Status);

internal readonly record struct MakcuModeChangeResult(
    bool Success,
    bool ActualEnabled,
    int ActualBaudRate,
    string Message,
    bool ActualStateVerified = false);

/// <summary>串口所有权由调用方持有；此接口只编排单读者握手与状态验证。</summary>
internal interface IMakcuModeHandshake
{
    Task<bool> BeginTransitionAsync(CancellationToken cancellationToken);
    Task<MakcuModeSnapshot?> SendRequestAsync(bool enabled, CancellationToken cancellationToken);
    Task SetBaudRateAsync(int baudRate, CancellationToken cancellationToken);
    Task<MakcuModeSnapshot?> QueryAsync(CancellationToken cancellationToken);
    Task<bool> ProbeMouseHostAsync(CancellationToken cancellationToken);
    Task<MakcuModeSnapshot?> QueryAtBaudAsync(int baudRate, CancellationToken cancellationToken);
    Task<bool> EndTransitionAsync(bool enabled, int baudRate, CancellationToken cancellationToken);
}

/// <summary>
/// 将固件“请求已受理”与“新波特率下状态和 M 角色已验证”分开处理。
/// </summary>
internal sealed class MakcuModeCoordinator(IMakcuModeHandshake handshake)
{
    internal const int A5BaudRate = 921600;
    internal const int MakcuBaudRate = 115200;
    internal static readonly TimeSpan FirmwareSwitchSettleDelay = TimeSpan.FromMilliseconds(80);

    private readonly SemaphoreSlim _gate = new(1, 1);

    internal async Task<MakcuModeChangeResult> SetEnabledAsync(
        bool requestedEnabled,
        bool currentEnabled,
        int currentBaudRate,
        bool compiledCapabilityAvailable,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(handshake);
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (!compiledCapabilityAvailable)
            {
                return new MakcuModeChangeResult(
                    false,
                    currentEnabled,
                    currentBaudRate,
                    "当前固件未编译 Makcu V4 运行时切换能力。可继续使用 921600 的 A5 模式。");
            }
            bool readerStopped = await handshake.BeginTransitionAsync(cancellationToken).ConfigureAwait(false);
            if (!readerStopped)
            {
                return new MakcuModeChangeResult(
                    false,
                    currentEnabled,
                    currentBaudRate,
                    "设备日志读取线程未能确认退出；已保持串口输出关闭，未启动第二个读取器。");
            }

            string failure = "模式切换未完成。";
            MakcuModeSnapshot? accepted = null;
            try
            {
                accepted = await handshake.SendRequestAsync(
                    requestedEnabled,
                    cancellationToken).ConfigureAwait(false);
                int targetBaudRate = requestedEnabled ? MakcuBaudRate : A5BaudRate;
                if (accepted is { Status: 0 } acceptance &&
                    acceptance.Enabled == requestedEnabled && acceptance.BaudRate == targetBaudRate)
                {
                    await Task.Delay(FirmwareSwitchSettleDelay, cancellationToken).ConfigureAwait(false);
                    await handshake.SetBaudRateAsync(targetBaudRate, cancellationToken).ConfigureAwait(false);
                    MakcuModeSnapshot? verified = await handshake.QueryAsync(cancellationToken).ConfigureAwait(false);
                    bool roleVerified = verified is { Status: 0 } state &&
                        state.Enabled == requestedEnabled && state.BaudRate == targetBaudRate &&
                        await handshake.ProbeMouseHostAsync(cancellationToken).ConfigureAwait(false);
                    if (roleVerified && verified is MakcuModeSnapshot verifiedState)
                    {
                        bool readerRestarted = await handshake.EndTransitionAsync(
                            verifiedState.Enabled,
                            verifiedState.BaudRate,
                            cancellationToken).ConfigureAwait(false);
                        if (readerRestarted)
                        {
                            return new MakcuModeChangeResult(
                                true,
                                verifiedState.Enabled,
                                verifiedState.BaudRate,
                                $"模式已验证并切换到 {verifiedState.BaudRate} baud。",
                                ActualStateVerified: true);
                        }
                        failure = "新模式已验证，但设备日志读取器未能恢复；输入保持关闭。";
                    }
                    else
                    {
                        failure = "新波特率下未能同时验证 MakcuModeStatus 与 M 板角色。";
                    }
                }
                else if (accepted is { Status: not 0 } rejected)
                {
                    failure = $"固件拒绝 Makcu 模式请求，status={rejected.Status}。";
                }
                else
                {
                    failure = "未收到有效的 Makcu 模式受理响应。";
                }
            }
            catch (OperationCanceledException)
            {
                throw;
            }
            catch (Exception exception) when (
                exception is IOException or InvalidOperationException or TimeoutException)
            {
                failure = $"{failure} {exception.Message}";
            }

            // 固件明确拒绝时不曾切速；在原 baud 重新验证 M 角色后恢复 A5 输入和 reader。
            if (accepted is { Status: not 0 } &&
                currentBaudRate is A5BaudRate or MakcuBaudRate)
            {
                try
                {
                    await handshake.SetBaudRateAsync(currentBaudRate, cancellationToken).ConfigureAwait(false);
                    if (await handshake.ProbeMouseHostAsync(cancellationToken).ConfigureAwait(false) &&
                        await handshake.EndTransitionAsync(
                            currentEnabled,
                            currentBaudRate,
                            cancellationToken).ConfigureAwait(false))
                    {
                        return new MakcuModeChangeResult(
                            false,
                            currentEnabled,
                            currentBaudRate,
                            $"{failure} 已确认设备仍处于原模式；设置未保存。",
                            ActualStateVerified: !currentEnabled &&
                                currentBaudRate == A5BaudRate);
                    }
                }
                catch (Exception exception) when (
                    exception is IOException or InvalidOperationException or TimeoutException)
                {
                    failure += $" 原速率恢复失败：{exception.Message}";
                }
            }

            foreach (int baudRate in DistinctBaudRates(
                         currentBaudRate,
                         requestedEnabled ? MakcuBaudRate : A5BaudRate))
            {
                cancellationToken.ThrowIfCancellationRequested();
                try
                {
                    MakcuModeSnapshot? recovered = await handshake.QueryAtBaudAsync(
                        baudRate,
                        cancellationToken).ConfigureAwait(false);
                    if (recovered is not { Status: 0 } snapshot ||
                        snapshot.BaudRate != baudRate ||
                        !IsConsistentMode(snapshot) ||
                        !await handshake.ProbeMouseHostAsync(cancellationToken).ConfigureAwait(false))
                    {
                        continue;
                    }

                    bool readerRestarted = await handshake.EndTransitionAsync(
                        snapshot.Enabled,
                        snapshot.BaudRate,
                        cancellationToken).ConfigureAwait(false);
                    if (!readerRestarted)
                    {
                        failure += " 读取器恢复失败，输入保持关闭。";
                        break;
                    }

                    return new MakcuModeChangeResult(
                        false,
                        snapshot.Enabled,
                        snapshot.BaudRate,
                        $"{failure} 已恢复实际模式：{(snapshot.Enabled ? "Makcu" : "A5")} / {snapshot.BaudRate} baud；设置未保存。",
                        ActualStateVerified: true);
                }
                catch (Exception exception) when (
                    exception is IOException or InvalidOperationException or TimeoutException)
                {
                    failure += $" {baudRate} baud 恢复查询失败：{exception.Message}";
                }
            }

            return new MakcuModeChangeResult(
                false,
                currentEnabled,
                currentBaudRate,
                $"{failure} 未能在旧、新速率确认设备状态；串口输入门保持关闭，设置未保存。");
        }
        finally
        {
            _gate.Release();
        }
    }

    private static IEnumerable<int> DistinctBaudRates(int first, int second)
    {
        if (first is A5BaudRate or MakcuBaudRate)
        {
            yield return first;
        }
        if ((second is A5BaudRate or MakcuBaudRate) && second != first)
        {
            yield return second;
        }
        foreach (int fallback in new[] { A5BaudRate, MakcuBaudRate })
        {
            if (fallback != first && fallback != second)
            {
                yield return fallback;
            }
        }
    }

    private static bool IsConsistentMode(MakcuModeSnapshot snapshot) =>
        snapshot.Enabled
            ? snapshot.BaudRate == MakcuBaudRate
            : snapshot.BaudRate == A5BaudRate;
}

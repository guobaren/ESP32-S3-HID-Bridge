using System.Buffers.Binary;
using HidBridge.Protocol;

namespace HidBridge.Host.Transport;

internal sealed record DeviceQueueStatistics(
    byte Id,
    string Name,
    byte Unit,
    ushort? Capacity,
    ushort? Depth,
    ushort? Peak,
    uint? Received,
    uint? Rejected,
    uint? Dropped);

internal sealed record DeviceStatisticsSnapshot(
    string PortName,
    byte Role,
    string RoleName,
    uint UptimeMilliseconds,
    IReadOnlyDictionary<string, object> Counters,
    IReadOnlyDictionary<string, object> State,
    IReadOnlyList<DeviceQueueStatistics> Queues);

internal sealed record DeviceStatisticsQueryResult(
    DateTimeOffset ReadAt,
    IReadOnlyList<DeviceStatisticsSnapshot> Boards);

internal sealed class DeviceStatsQueryContext(string[] ports, ushort sequence)
{
    internal object Sync { get; } = new();
    internal ushort Sequence { get; } = sequence;
    internal bool Closed { get; set; }
    internal TaskCompletionSource<DeviceStatisticsQueryResult> Completion { get; } =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    internal Dictionary<string, DeviceStatsPageCollector> Collectors { get; } = ports.ToDictionary(
        port => port,
        port => new DeviceStatsPageCollector(port, sequence),
        StringComparer.OrdinalIgnoreCase);

    internal string DescribeIncomplete() => string.Join(
        "; ",
        Collectors
            .Where(item => !item.Value.IsComplete)
            .Select(item => $"{item.Key} {item.Value.Progress}"));
}

internal sealed class DeviceStatsPendingRequestRegistry
{
    private readonly object _sync = new();
    private DeviceStatsQueryContext? _active;

    internal bool HasActive
    {
        get
        {
            lock (_sync)
            {
                return _active is not null;
            }
        }
    }

    internal DeviceStatsQueryContext Register(string[] ports, ushort sequence)
    {
        lock (_sync)
        {
            if (_active is not null)
            {
                throw new InvalidOperationException("已有统计请求尚未清理。");
            }
            _active = new DeviceStatsQueryContext(ports, sequence);
            return _active;
        }
    }

    internal bool TryGet(
        string portName,
        BridgeFrame frame,
        out DeviceStatsQueryContext? query,
        out DeviceStatsPageCollector? collector)
    {
        lock (_sync)
        {
            query = _active;
            if (query is null || query.Closed || frame.Type != MessageType.StatsSnapshotResponse ||
                frame.Sequence != query.Sequence ||
                !query.Collectors.TryGetValue(portName, out collector))
            {
                query = null;
                collector = null;
                return false;
            }
            return true;
        }
    }

    internal void Clear(DeviceStatsQueryContext query)
    {
        lock (_sync)
        {
            lock (query.Sync)
            {
                query.Closed = true;
            }
            if (ReferenceEquals(_active, query))
            {
                _active = null;
            }
        }
    }
}

internal sealed class DeviceStatsPageCollector(string portName, ushort sequence)
{
    private const byte SchemaVersion = 1;
    private const byte CounterKind = 1;
    private const byte QueueKind = 2;
    private const byte CounterU64 = 1;
    private const byte CounterI64 = 2;
    private const int HeaderLength = 12;
    private const int CounterRecordLength = 10;
    private const int QueueRecordLength = 20;

    private readonly Dictionary<byte, object> _counters = [];
    private readonly Dictionary<byte, DeviceQueueStatistics> _queues = [];
    private readonly HashSet<byte> _counterPages = [];
    private readonly HashSet<byte> _queuePages = [];
    private byte? _counterPageCount;
    private byte? _queuePageCount;
    private byte? _role;
    private uint? _uptimeMilliseconds;

    internal bool IsComplete => _counterPageCount is > 0 &&
        _counterPages.Count == _counterPageCount &&
        _queuePageCount is > 0 && _queuePages.Count == _queuePageCount;

    internal string Progress =>
        $"counters={_counterPages.Count}/{_counterPageCount?.ToString() ?? "?"}, " +
        $"queues={_queuePages.Count}/{_queuePageCount?.ToString() ?? "?"}";

    internal void Add(BridgeFrame frame)
    {
        if (frame.Type != MessageType.StatsSnapshotResponse || frame.Sequence != sequence)
        {
            throw new InvalidDataException("统计响应类型或请求序号不匹配。");
        }
        ReadOnlySpan<byte> payload = frame.Payload;
        if (payload.Length < HeaderLength)
        {
            throw new InvalidDataException("统计响应头长度不足。");
        }

        byte schema = payload[0];
        byte role = payload[1];
        byte kind = payload[2];
        byte pageIndex = payload[3];
        byte pageCount = payload[4];
        byte status = payload[5];
        byte itemCount = payload[6];
        byte reserved = payload[7];
        uint uptime = BinaryPrimitives.ReadUInt32LittleEndian(payload[8..12]);
        if (schema != SchemaVersion || role is not (1 or 2) || kind is not (CounterKind or QueueKind) ||
            pageCount == 0 || pageIndex >= pageCount || reserved != 0)
        {
            throw new InvalidDataException("统计响应 schema、role、page 或 reserved 字段非法。");
        }
        if (status != 0)
        {
            throw new InvalidDataException($"设备拒绝统计快照，status={status}。");
        }
        int recordLength = kind == CounterKind ? CounterRecordLength : QueueRecordLength;
        int maximumItems = kind == CounterKind ? 5 : 2;
        if (itemCount == 0 || itemCount > maximumItems ||
            payload.Length != HeaderLength + itemCount * recordLength)
        {
            throw new InvalidDataException("统计响应记录数量与载荷长度不匹配。");
        }
        if (_role is null)
        {
            _role = role;
            _uptimeMilliseconds = uptime;
        }
        else if (_role != role || _uptimeMilliseconds != uptime)
        {
            throw new InvalidDataException("统计分页来自不同角色或不同 uptime 快照。");
        }

        HashSet<byte> pages = kind == CounterKind ? _counterPages : _queuePages;
        ref byte? expectedPageCount = ref (kind == CounterKind
            ? ref _counterPageCount
            : ref _queuePageCount);
        if (expectedPageCount is not null && expectedPageCount != pageCount)
        {
            throw new InvalidDataException("同组统计分页的 page_count 不一致。");
        }
        expectedPageCount = pageCount;
        if (!pages.Add(pageIndex))
        {
            throw new InvalidDataException($"重复统计页 kind={kind}, index={pageIndex}。");
        }

        for (int index = 0; index < itemCount; index++)
        {
            ReadOnlySpan<byte> record = payload.Slice(HeaderLength + index * recordLength, recordLength);
            byte id = record[0];
            if (kind == CounterKind)
            {
                byte valueType = record[1];
                object value = valueType switch
                {
                    CounterU64 => BinaryPrimitives.ReadUInt64LittleEndian(record[2..10]),
                    CounterI64 => BinaryPrimitives.ReadInt64LittleEndian(record[2..10]),
                    _ => throw new InvalidDataException($"未知统计值类型 {valueType}。")
                };
                if (!_counters.TryAdd(id, value))
                {
                    throw new InvalidDataException($"重复统计项 id={id}。");
                }
            }
            else
            {
                byte unit = record[1];
                if (unit is not (1 or 2))
                {
                    throw new InvalidDataException($"未知队列单位 {unit}。");
                }
                ushort capacity = BinaryPrimitives.ReadUInt16LittleEndian(record[2..4]);
                ushort depth = BinaryPrimitives.ReadUInt16LittleEndian(record[4..6]);
                ushort peak = BinaryPrimitives.ReadUInt16LittleEndian(record[6..8]);
                uint received = BinaryPrimitives.ReadUInt32LittleEndian(record[8..12]);
                uint rejected = BinaryPrimitives.ReadUInt32LittleEndian(record[12..16]);
                uint dropped = BinaryPrimitives.ReadUInt32LittleEndian(record[16..20]);
                DeviceQueueStatistics queue = new(
                    id,
                    QueueName(id),
                    unit,
                    Unknown(capacity),
                    Unknown(depth),
                    Unknown(peak),
                    Unknown(received),
                    Unknown(rejected),
                    Unknown(dropped));
                if (!_queues.TryAdd(id, queue))
                {
                    throw new InvalidDataException($"重复队列统计 id={id}。");
                }
            }
        }
    }

    internal DeviceStatisticsSnapshot Build()
    {
        if (!IsComplete || _role is null || _uptimeMilliseconds is null)
        {
            throw new InvalidOperationException("统计快照尚未收到所有页。");
        }
        byte[] requiredCounterIds = _role == 2
            ? [.. Enumerable.Range(1, 36).Select(value => (byte)value), .. Enumerable.Range(49, 18).Select(value => (byte)value), 68]
            : [.. Enumerable.Range(37, 12).Select(value => (byte)value), .. Enumerable.Range(49, 18).Select(value => (byte)value), 67];
        byte[] requiredQueueIds = _role == 2
            ? [.. Enumerable.Range(1, 9).Select(value => (byte)value), 13]
            : [.. Enumerable.Range(4, 6).Select(value => (byte)value), 10, 11, 12, 13];
        if (requiredCounterIds.Any(id => !_counters.ContainsKey(id)) ||
            requiredQueueIds.Any(id => !_queues.ContainsKey(id)))
        {
            throw new InvalidDataException("快照缺少必需的统计项或队列记录。");
        }

        Dictionary<string, object> counters = _counters.ToDictionary(
            item => CounterName(item.Key), item => item.Value, StringComparer.Ordinal);
        Dictionary<string, object> state = BuildBoardState(_role.Value);
        return new DeviceStatisticsSnapshot(
            portName,
            _role.Value,
            _role == 1 ? "P/PC_DEVICE" : "M/MOUSE_HOST",
            _uptimeMilliseconds.Value,
            counters,
            state,
            _queues.Values.OrderBy(queue => queue.Id).ToArray());
    }

    private Dictionary<string, object> BuildBoardState(byte role)
    {
        byte stateCounterId = role == 1 ? (byte)67 : (byte)68;
        ulong packed = (ulong)_counters[stateCounterId];
        Dictionary<string, object> state = new(StringComparer.Ordinal);
        if (role == 1)
        {
            byte result = (byte)((packed >> 8) & 0xFF);
            state["attached"] = (packed & (1UL << 0)) != 0;
            state["installed"] = (packed & (1UL << 1)) != 0;
            state["cloneActive"] = (packed & (1UL << 2)) != 0;
            state["mounted"] = (packed & (1UL << 3)) != 0;
            state["reconfigureInProgress"] = (packed & (1UL << 4)) != 0;
            state["waitingHost"] = (packed & (1UL << 5)) != 0;
            state["finalAckFailed"] = (packed & (1UL << 6)) != 0;
            state["disconnectPending"] = (packed & (1UL << 7)) != 0;
            state["waitingFinalAck"] = (packed & (1UL << 16)) != 0;
            state["lastResultId"] = result;
            state["lastResult"] = result switch
            {
                0 => "unknown",
                1 => "wait_host",
                2 => "final_ack_pending",
                3 => "mounted_acked",
                4 => "install_failed",
                5 => "final_ack_failed",
                6 => "canceled",
                _ => $"unknown_{result}",
            };
            state["operationEpoch"] = packed >> 32;
        }
        else
        {
            state["vendorSessionActive"] = (packed & (1UL << 0)) != 0;
            state["firstVendorRequestSeen"] = (packed & (1UL << 1)) != 0;
            state["peerGenerationCurrent"] = (packed & (1UL << 2)) != 0;
            state["waitingHost"] = (packed & (1UL << 3)) != 0;
            state["vendorSessionEpoch"] = packed >> 32;
        }
        return state;
    }

    private static ushort? Unknown(ushort value) => value == ushort.MaxValue ? null : value;
    private static uint? Unknown(uint value) => value == uint.MaxValue ? null : value;

    private static string CounterName(byte id) => id switch
    {
        1 => "reports",
        2 => "vendor_reports",
        3 => "input_fail",
        4 => "control",
        5 => "control_fail",
        6 => "ctrl_retry",
        7 => "urb_sub",
        8 => "urb_ok",
        9 => "urb_to",
        10 => "urb_retry",
        11 => "recover",
        12 => "port_cycle",
        13 => "wheel",
        14 => "ctrl_lat_max_us",
        15 => "slow10",
        16 => "slow100",
        17 => "vmin_gap_us",
        18 => "errors",
        19 => "motion_rx_dx",
        20 => "motion_rx_dy",
        21 => "motion_rx_ok",
        22 => "motion_rx_badlen",
        23 => "motion_rx_badparse",
        24 => "hb_ok",
        25 => "hb_fail",
        26 => "cycle_req",
        27 => "cycle_attempt",
        28 => "cycle_ok",
        29 => "cycle_fail",
        30 => "cycle_off_fail",
        31 => "cycle_on_fail",
        32 => "cycle_suppressed",
        33 => "input_restored",
        34 => "input_missing",
        35 => "late500",
        36 => "detect_max_us",
        37 => "not_mounted",
        38 => "not_ready",
        39 => "attempt",
        40 => "submitted",
        41 => "failed",
        42 => "complete",
        43 => "transfer_fail",
        44 => "physical_rx",
        45 => "vendor_rx",
        46 => "vendor_submitted",
        47 => "vendor_dropped",
        48 => "get_timeouts",
        49 => "uart1_tx",
        50 => "uart1_rx",
        51 => "uart1_rx_bytes",
        52 => "uart1_frame_err",
        53 => "uart1_rx_overflow",
        54 => "uart1_rx_pending_peak_bytes",
        55 => "uart1_heartbeat_gap_peak_ms",
        56 => "uart1_raw_tx_latency_peak_us",
        57 => "uart1_tx_write_fail",
        58 => "uart1_vendor_dropped",
        59 => "uart1_motion_dropped",
        60 => "uart1_profile_fail",
        61 => "uart1_gone_retry",
        62 => "uart1_gone_fail",
        63 => "uart1_budget_exhausted",
        64 => "uart_fifo_ovf_events",
        65 => "uart_buffer_full_events",
        66 => "uart_event_reset_dropped",
        67 => "p_usb_state",
        68 => "m_vendor_session_state",
        _ => $"counter_{id}"
    };

    private static string QueueName(byte id) => id switch
    {
        1 => "M.host_hid_event",
        2 => "M.host_hid_report",
        3 => "M.host_hid_control",
        4 => "UART1.tx",
        5 => "UART1.motion_tx",
        6 => "UART1.safety_tx",
        7 => "UART1.software_tx",
        8 => "UART1.vendor_tx",
        9 => "UART1.event",
        10 => "P.vendor_input",
        11 => "P.motion_input",
        12 => "P.vendor_control",
        13 => "UART1.rx_ring",
        _ => $"queue_{id}"
    };
}

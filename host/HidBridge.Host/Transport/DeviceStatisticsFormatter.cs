using System.Globalization;
using System.Text;

namespace HidBridge.Host.Transport;

internal static class DeviceStatisticsFormatter
{
    internal static string Format(DeviceStatisticsQueryResult result)
    {
        StringBuilder text = new();
        text.AppendLine($"读取时间：{result.ReadAt:yyyy-MM-dd HH:mm:ss.fff zzz}");
        text.AppendLine($"板卡数：{result.Boards.Count}");
        foreach (DeviceStatisticsSnapshot board in result.Boards)
        {
            text.AppendLine();
            text.AppendLine($"[{board.RoleName}] 端口={board.PortName}，uptime_ms={board.UptimeMilliseconds}");
            text.AppendLine("计数：");
            foreach ((string name, object value) in board.Counters)
            {
                text.Append("  ").Append(name).Append(" = ").AppendLine(FormatValue(value));
            }

            text.AppendLine("板卡状态：");
            foreach ((string name, object value) in board.State)
            {
                text.Append("  ").Append(name).Append(" = ").AppendLine(FormatValue(value));
            }

            text.AppendLine("队列（capacity/depth/peak 为条目数或字节，按 unit 标注）：");
            foreach (DeviceQueueStatistics queue in board.Queues)
            {
                string unit = queue.Unit == 1 ? "条" : "字节";
                text.Append("  #").Append(queue.Id).Append(' ').Append(queue.Name)
                    .Append(" [").Append(unit).Append("] capacity=").Append(FormatValue(queue.Capacity))
                    .Append(" depth=").Append(FormatValue(queue.Depth))
                    .Append(" peak=").Append(FormatValue(queue.Peak))
                    .Append(" received=").Append(FormatValue(queue.Received))
                    .Append(" rejected=").Append(FormatValue(queue.Rejected))
                    .Append(" dropped=").AppendLine(FormatValue(queue.Dropped));
            }
        }
        return text.ToString();
    }

    private static string FormatValue(object? value) => value switch
    {
        null => "未采集",
        ulong unsigned => unsigned.ToString(CultureInfo.InvariantCulture),
        long signed => signed.ToString(CultureInfo.InvariantCulture),
        ushort number => number.ToString(CultureInfo.InvariantCulture),
        uint number => number.ToString(CultureInfo.InvariantCulture),
        _ => Convert.ToString(value, CultureInfo.InvariantCulture) ?? "未采集",
    };
}

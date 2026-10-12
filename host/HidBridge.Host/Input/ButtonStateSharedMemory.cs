using System.Buffers.Binary;
using System.Diagnostics;
using System.IO.MemoryMappedFiles;

namespace HidBridge.Host.Input;

internal readonly record struct ButtonStateSnapshot(
    byte Buttons,
    bool MouseValid,
    bool SubscriptionHealthy,
    byte ConnectionState,
    long TimestampQpc = 0);

/// <summary>Host 向 Sender 发布 M 板物理鼠标按键状态的固定 64 字节共享区。</summary>
internal sealed class ButtonStateSharedMemory : IDisposable
{
    internal const string DefaultMappingName = "HidBridge.Buttons.v1";
    internal const string DefaultMutexName = DefaultMappingName + ".Mutex";
    internal const int MappingSize = 64;
    internal const uint Magic = 0x314B4248; // "HBK1" little endian
    internal const uint Version = 1;

    private const int SequenceOffset = 16;
    private const int TimestampOffset = 24;
    private const int ButtonsOffset = 32;
    private const int ValidOffset = 34;
    private const int ConnectionStateOffset = 35;
    private static readonly long MaximumStateAgeQpc = Stopwatch.Frequency / 5;

    private readonly MemoryMappedFile _mapping;
    private readonly MemoryMappedViewAccessor _view;
    private readonly Mutex _mutex;
    private readonly object _lifecycleSync = new();
    private bool _disposed;

    internal ButtonStateSharedMemory(
        string mappingName = DefaultMappingName,
        string? mutexName = null)
    {
        if (!TryNormalizeMappingName(mappingName, out string normalizedName, out string? error))
        {
            throw new ArgumentException(error, nameof(mappingName));
        }

        MappingName = normalizedName;
        MutexName = mutexName ?? GetMutexName(normalizedName);
        _mapping = MemoryMappedFile.CreateOrOpen(MappingName, MappingSize, MemoryMappedFileAccess.ReadWrite);
        _view = _mapping.CreateViewAccessor(0, MappingSize, MemoryMappedFileAccess.ReadWrite);
        _mutex = new Mutex(false, MutexName);
    }

    internal string MappingName { get; }
    internal string MutexName { get; }

    internal static string NormalizeConfiguredMappingName(string? mappingName) =>
        TryNormalizeMappingName(mappingName, out string normalizedName, out _)
            ? normalizedName
            : DefaultMappingName;

    internal static bool TryNormalizeMappingName(
        string? mappingName,
        out string normalizedName,
        out string? error)
    {
        normalizedName = mappingName?.Trim() ?? string.Empty;
        if (normalizedName.Length == 0)
        {
            error = "共享内存名称不能为空。";
            return false;
        }
        if (normalizedName.IndexOf('\0') >= 0)
        {
            error = "共享内存名称不能包含 NUL 字符。";
            return false;
        }
        int namespacePrefixLength = normalizedName.StartsWith("Local\\", StringComparison.Ordinal)
            ? "Local\\".Length
            : normalizedName.StartsWith("Global\\", StringComparison.Ordinal)
                ? "Global\\".Length
                : 0;
        int firstSeparator = normalizedName.IndexOf('\\');
        if (firstSeparator >= 0 &&
            (namespacePrefixLength == 0 ||
             namespacePrefixLength >= normalizedName.Length ||
             normalizedName.IndexOf('\\', namespacePrefixLength) >= 0))
        {
            error = "共享内存名称仅允许在开头使用 Local\\ 或 Global\\ 前缀，其他位置不能包含反斜杠。";
            return false;
        }

        error = null;
        return true;
    }

    internal static string GetMutexName(string mappingName) =>
        mappingName.Equals(DefaultMappingName, StringComparison.Ordinal)
            ? DefaultMutexName
            : $"{mappingName}.Mutex";

    internal bool TryPublish(ButtonStateSnapshot snapshot)
    {
        lock (_lifecycleSync)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            return TryPublishLocked(snapshot);
        }
    }

    private bool TryPublishLocked(ButtonStateSnapshot snapshot)
    {
        bool ownsMutex = false;
        try
        {
            try
            {
                ownsMutex = _mutex.WaitOne(0);
            }
            catch (AbandonedMutexException)
            {
                // .NET 已授予所有权。残留字段不可信；本次仍写入一份完整快照。
                ownsMutex = true;
            }

            if (!ownsMutex)
            {
                return false;
            }

            byte[] prior = new byte[MappingSize];
            _view.ReadArray(0, prior, 0, prior.Length);
            // seq 在完整快照的末尾写入；即使进程在头部/字段写入期间退出，
            // 也必须保留旧 seq，以越过 Sender 弃锁时记录的残留序号下界。
            ulong priorSequence =
                BinaryPrimitives.ReadUInt64LittleEndian(prior.AsSpan(SequenceOffset, 8));
            ulong sequence = unchecked(priorSequence + 1UL);

            byte[] next = new byte[MappingSize];
            BinaryPrimitives.WriteUInt32LittleEndian(next.AsSpan(0, 4), Magic);
            BinaryPrimitives.WriteUInt32LittleEndian(next.AsSpan(4, 4), Version);
            BinaryPrimitives.WriteUInt32LittleEndian(next.AsSpan(8, 4), MappingSize);
            BinaryPrimitives.WriteUInt32LittleEndian(next.AsSpan(12, 4), 0U);
            long nowQpc = Stopwatch.GetTimestamp();
            long stateQpc = snapshot.TimestampQpc > 0 ? snapshot.TimestampQpc : nowQpc;
            bool mouseValid = snapshot.MouseValid;
            if (mouseValid && snapshot.TimestampQpc > 0)
            {
                long ageQpc = nowQpc - snapshot.TimestampQpc;
                mouseValid = ageQpc >= 0 && ageQpc <= MaximumStateAgeQpc;
            }
            byte buttons = mouseValid ? (byte)(snapshot.Buttons & 0x1F) : (byte)0;
            double timestampSeconds = (double)stateQpc / Stopwatch.Frequency;
            BinaryPrimitives.WriteInt64LittleEndian(
                next.AsSpan(TimestampOffset, sizeof(long)),
                BitConverter.DoubleToInt64Bits(timestampSeconds));
            next[ButtonsOffset] = buttons;
            next[33] = 0;
            next[ValidOffset] = (byte)((mouseValid ? 0x01 : 0) |
                (snapshot.SubscriptionHealthy ? 0x02 : 0));
            next[ConnectionStateOffset] = snapshot.ConnectionState;
            // [36..63] 由 new byte[64] 保持为 0。

            _view.WriteArray(0, next, 0, SequenceOffset);
            _view.WriteArray(SequenceOffset + sizeof(ulong), next,
                SequenceOffset + sizeof(ulong), MappingSize - SequenceOffset - sizeof(ulong));
            _view.Write(SequenceOffset, sequence); // 版本号最后提交，且仍在同一把跨进程 mutex 内。
            _view.Flush();
            return true;
        }
        finally
        {
            if (ownsMutex)
            {
                _mutex.ReleaseMutex();
            }
        }
    }

    public void Dispose()
    {
        lock (_lifecycleSync)
        {
            if (_disposed)
            {
                return;
            }

            try
            {
                _ = TryPublishLocked(new ButtonStateSnapshot(0, false, false, 0));
            }
            finally
            {
                _disposed = true;
                _view.Dispose();
                _mapping.Dispose();
                _mutex.Dispose();
            }
        }
    }
}

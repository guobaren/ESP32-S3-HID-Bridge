using System.Reflection;
using System.Security.Cryptography;

namespace HidBridge.Host.FirmwareUpdate;

/// <summary>
/// 程序内置的刷写资源：独立版 esptool.exe 与默认固件镜像。
/// 资源由 csproj 的 EmbeddedResource 项在构建时嵌入（存在才嵌入）；
/// 运行期按需解压到 %LOCALAPPDATA%\HidBridge\embedded 缓存，避免常驻占用磁盘。
/// </summary>
internal static class EmbeddedFlashAssets
{
    private const string ResourcePrefix = "HidBridge.Host.assets.";

    private static readonly string[] FirmwareRelativePaths =
    [
        "firmware/build/flasher_args.json",
        "firmware/build/bootloader/bootloader.bin",
        "firmware/build/partition_table/partition-table.bin",
        "firmware/build/esp32_s3_hid_bridge.bin",
    ];

    private static Assembly Assembly => typeof(EmbeddedFlashAssets).Assembly;

    private static string CacheRoot => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "HidBridge",
        "embedded");

    internal static bool HasEmbeddedEsptool =>
        Assembly.GetManifestResourceNames().Contains(ResourcePrefix + "esptool.exe");

    internal static bool HasEmbeddedFirmware =>
        Assembly.GetManifestResourceNames()
            .Any(name => name.StartsWith(ResourcePrefix + "firmware.build.", StringComparison.Ordinal));

    internal static string EsptoolCachePath => Path.Combine(CacheRoot, "esptool", "esptool.exe");

    /// <summary>
    /// 将内置 esptool.exe 解压到缓存并返回路径；未嵌入时返回 null。
    /// </summary>
    internal static string? ExtractEsptool()
    {
        if (!HasEmbeddedEsptool)
        {
            return null;
        }
        string destination = EsptoolCachePath;
        return ExtractIfChanged("esptool.exe", destination) ? destination : null;
    }

    /// <summary>
    /// 将内置默认固件解压到缓存，返回可作为项目根使用的目录（含 firmware/build）；
    /// 未嵌入完整固件时返回 null。
    /// </summary>
    internal static string? ExtractFirmwareProjectRoot()
    {
        if (!HasEmbeddedFirmware)
        {
            return null;
        }
        foreach (string relative in FirmwareRelativePaths)
        {
            if (!ExtractIfChanged(relative, Path.Combine(CacheRoot, relative)))
            {
                return null;
            }
        }
        return File.Exists(Path.Combine(CacheRoot, "firmware", "build", "flasher_args.json"))
            ? CacheRoot
            : null;
    }

    private static bool ExtractIfChanged(string logicalSuffix, string destinationPath)
    {
        string logicalName = ResourcePrefix + logicalSuffix;
        using Stream? stream = Assembly.GetManifestResourceStream(logicalName);
        if (stream is null)
        {
            return false;
        }
        byte[] bytes;
        using (MemoryStream memory = new())
        {
            stream.CopyTo(memory);
            bytes = memory.ToArray();
        }
        string? directory = Path.GetDirectoryName(destinationPath);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }
        if (File.Exists(destinationPath) && HashesMatch(bytes, destinationPath))
        {
            return true;
        }
        string temporary = destinationPath + ".tmp";
        File.WriteAllBytes(temporary, bytes);
        File.Move(temporary, destinationPath, true);
        return true;
    }

    private static bool HashesMatch(byte[] expected, string path)
    {
        byte[] actual;
        using (FileStream stream = File.OpenRead(path))
        {
            actual = SHA256.HashData(stream);
        }
        return Convert.ToHexString(actual) == Convert.ToHexString(SHA256.HashData(expected));
    }
}

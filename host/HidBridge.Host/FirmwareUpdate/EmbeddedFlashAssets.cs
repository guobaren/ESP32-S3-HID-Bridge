using System.Reflection;
using System.Security.Cryptography;

namespace HidBridge.Host.FirmwareUpdate;

/// <summary>
/// 程序内置的独立版 esptool.exe。
/// 资源由 csproj 的 EmbeddedResource 项在构建时嵌入（存在才嵌入）；
/// 运行期按需解压到 %LOCALAPPDATA%\HidBridge\embedded 缓存，避免常驻占用磁盘。
/// </summary>
internal static class EmbeddedFlashAssets
{
    private const string ResourcePrefix = "HidBridge.Host.assets.";

    private static Assembly Assembly => typeof(EmbeddedFlashAssets).Assembly;

    private static string CacheRoot => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "HidBridge",
        "embedded");

    internal static bool HasEmbeddedEsptool =>
        Assembly.GetManifestResourceNames().Contains(ResourcePrefix + "esptool.exe");

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

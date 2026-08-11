using System.Security.Cryptography;
using System.Text.Json;

namespace HidBridge.Host.FirmwareUpdate;

internal sealed record FirmwareFlashImage(
    long Offset,
    string OffsetArgument,
    string Path,
    string Sha256);

internal sealed class FirmwareFlashPlan
{
    private static readonly long[] RequiredOffsets = [0x0000, 0x8000, 0x10000];

    private FirmwareFlashPlan(
        string projectRoot,
        string buildDirectory,
        string esptoolPath,
        string chip,
        string before,
        string after,
        IReadOnlyList<string> writeFlashArguments,
        IReadOnlyList<FirmwareFlashImage> images)
    {
        ProjectRoot = projectRoot;
        BuildDirectory = buildDirectory;
        EsptoolPath = esptoolPath;
        Chip = chip;
        Before = before;
        After = after;
        WriteFlashArguments = writeFlashArguments;
        Images = images;
    }

    internal string ProjectRoot { get; }
    internal string BuildDirectory { get; }
    internal string EsptoolPath { get; }
    internal string Chip { get; }
    internal string Before { get; }
    internal string After { get; }
    internal IReadOnlyList<string> WriteFlashArguments { get; }
    internal IReadOnlyList<FirmwareFlashImage> Images { get; }

    internal static FirmwareFlashPlan Load(BridgeOptions options)
    {
        string projectRoot = ResolveProjectRoot(options.FirmwareProjectRoot);
        string buildDirectory = Path.Combine(projectRoot, "firmware", "build");
        string manifestPath = Path.Combine(buildDirectory, "flasher_args.json");
        if (!File.Exists(manifestPath))
        {
            throw new FileNotFoundException("找不到固件刷写清单，请先执行 idf.py build。", manifestPath);
        }

        using JsonDocument document = JsonDocument.Parse(File.ReadAllText(manifestPath));
        JsonElement root = document.RootElement;
        JsonElement extra = root.GetProperty("extra_esptool_args");
        string chip = GetRequiredString(extra, "chip");
        string before = NormalizeResetOption(GetRequiredString(extra, "before"));
        string after = NormalizeResetOption(GetRequiredString(extra, "after"));
        string[] writeFlashArguments = root.GetProperty("write_flash_args")
            .EnumerateArray()
            .Select(item => item.GetString() ?? throw new InvalidDataException("write_flash_args 包含空值。"))
            .ToArray();

        string buildRoot = Path.GetFullPath(buildDirectory) + Path.DirectorySeparatorChar;
        List<FirmwareFlashImage> images = [];
        foreach (JsonProperty property in root.GetProperty("flash_files").EnumerateObject())
        {
            long offset = ParseOffset(property.Name);
            string relativePath = property.Value.GetString() ?? string.Empty;
            string imagePath = Path.GetFullPath(Path.Combine(buildDirectory, relativePath));
            if (!imagePath.StartsWith(buildRoot, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException($"刷写清单包含 build 目录外的文件：{relativePath}");
            }
            if (!File.Exists(imagePath))
            {
                throw new FileNotFoundException($"刷写镜像不存在：{relativePath}", imagePath);
            }
            images.Add(new FirmwareFlashImage(
                offset,
                $"0x{offset:x}",
                imagePath,
                Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(imagePath)))));
        }
        images.Sort((left, right) => left.Offset.CompareTo(right.Offset));
        if (!images.Select(image => image.Offset).SequenceEqual(RequiredOffsets))
        {
            throw new InvalidDataException("刷写清单必须且只能包含 0x0、0x8000、0x10000 三段镜像。");
        }

        return new FirmwareFlashPlan(
            projectRoot,
            buildDirectory,
            ResolveEsptoolPath(projectRoot),
            chip,
            before,
            after,
            writeFlashArguments,
            images);
    }

    private static string ResolveProjectRoot(string configuredRoot)
    {
        if (!string.IsNullOrWhiteSpace(configuredRoot))
        {
            string candidate = Path.GetFullPath(configuredRoot, AppContext.BaseDirectory);
            if (!File.Exists(Path.Combine(candidate, "firmware", "build", "flasher_args.json")))
            {
                throw new DirectoryNotFoundException($"firmwareProjectRoot 无有效固件 build：{candidate}");
            }
            return candidate;
        }

        foreach (string start in new[] { AppContext.BaseDirectory, Environment.CurrentDirectory }.Distinct())
        {
            DirectoryInfo? current = new(Path.GetFullPath(start));
            while (current is not null)
            {
                if (File.Exists(Path.Combine(current.FullName, "firmware", "build", "flasher_args.json")))
                {
                    return current.FullName;
                }
                current = current.Parent;
            }
        }
        throw new DirectoryNotFoundException("无法从程序目录或当前目录定位 firmware/build/flasher_args.json。");
    }

    private static string ResolveEsptoolPath(string projectRoot)
    {
        string pythonEnvironmentRoot = Path.Combine(
            projectRoot,
            ".esp-idf",
            "environment",
            "idf-tools",
            "python_env");
        if (!Directory.Exists(pythonEnvironmentRoot))
        {
            throw new DirectoryNotFoundException($"找不到项目 ESP-IDF Python 环境：{pythonEnvironmentRoot}");
        }
        string? path = Directory.EnumerateFiles(
                pythonEnvironmentRoot,
                "esptool.exe",
                SearchOption.AllDirectories)
            .OrderBy(candidate => candidate, StringComparer.OrdinalIgnoreCase)
            .FirstOrDefault();
        return path ?? throw new FileNotFoundException("项目 ESP-IDF 环境中找不到 esptool.exe。");
    }

    private static string GetRequiredString(JsonElement element, string propertyName) =>
        element.GetProperty(propertyName).GetString() is { Length: > 0 } value
            ? value
            : throw new InvalidDataException($"flasher_args.json 缺少 {propertyName}。");

    private static long ParseOffset(string value)
    {
        string normalized = value.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? value[2..] : value;
        return long.TryParse(normalized, System.Globalization.NumberStyles.HexNumber, null, out long offset)
            ? offset
            : throw new InvalidDataException($"无效刷写偏移：{value}");
    }

    private static string NormalizeResetOption(string value) => value.Replace('_', '-');
}

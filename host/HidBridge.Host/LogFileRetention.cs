namespace HidBridge.Host;

internal static class LogFileRetention
{
    internal const int DefaultMaxFileCount = 10;
    internal const int MaxSupportedFileCount = 1000;

    internal static void Validate(string propertyName, int maxFileCount)
    {
        if (maxFileCount is < 1 or > MaxSupportedFileCount)
        {
            throw new InvalidDataException(
                $"{propertyName} 必须在 1..{MaxSupportedFileCount} 范围内。");
        }
    }

    internal static void Enforce(string pathTemplate, string currentPath, int maxFileCount)
    {
        Validate(nameof(maxFileCount), maxFileCount);

        string fileNameTemplate = Path.GetFileName(pathTemplate);
        int timestampIndex = fileNameTemplate.IndexOf("{timestamp}", StringComparison.OrdinalIgnoreCase);
        if (timestampIndex < 0)
        {
            return;
        }

        string directory = Path.GetDirectoryName(currentPath) ?? AppContext.BaseDirectory;
        if (!Directory.Exists(directory))
        {
            return;
        }

        string prefix = fileNameTemplate[..timestampIndex];
        string suffix = fileNameTemplate[(timestampIndex + "{timestamp}".Length)..];
        FileInfo[] files;
        try
        {
            files = Directory.EnumerateFiles(directory)
                .Select(path => new FileInfo(path))
                .Where(file => file.Name.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                .Where(file => file.Name.EndsWith(suffix, StringComparison.OrdinalIgnoreCase))
                .OrderBy(file => file.LastWriteTimeUtc)
                .ThenBy(file => file.Name, StringComparer.OrdinalIgnoreCase)
                .ToArray();
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            return;
        }

        int filesToRemove = files.Length - maxFileCount + (File.Exists(currentPath) ? 0 : 1);
        if (filesToRemove <= 0)
        {
            return;
        }

        string normalizedCurrentPath = Path.GetFullPath(currentPath);
        int removed = 0;
        foreach (FileInfo file in files)
        {
            if (string.Equals(file.FullName, normalizedCurrentPath, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            try
            {
                file.Delete();
                removed++;
                if (removed >= filesToRemove)
                {
                    break;
                }
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
            {
                // 被其他进程占用的旧日志不影响当前日志继续写入。
            }
        }
    }
}

using System.Text;
using System.Text.RegularExpressions;

namespace HidBridge.Host.Automation;

internal readonly record struct MacroCommand(string Operation, object[] Arguments, int LineNumber, string Source);
internal readonly record struct MacroParseError(int LineNumber, string Source, string Message)
{
    public override string ToString() => $"第 {LineNumber} 行：{Message} -> {Source}";
}

internal sealed class ParsedMacro
{
    internal List<MacroCommand> Body { get; } = [];
    internal List<MacroCommand> OnPress { get; } = [];
    internal List<MacroCommand> WhileHold { get; } = [];
    internal List<MacroCommand> OnRelease { get; } = [];
    internal List<MacroParseError> Errors { get; } = [];
}

internal static partial class MacroParser
{
    private static readonly Dictionary<string, (int Minimum, int Maximum)> Specifications =
        new(StringComparer.OrdinalIgnoreCase)
        {
            ["move"] = (2, 2),
            ["moveto"] = (2, 2),
            ["mouse"] = (2, 2),
            ["keydown"] = (1, 1),
            ["keyup"] = (1, 1),
            ["keypress"] = (1, 2),
            ["wheel"] = (1, 1),
            ["delay"] = (1, 1),
            ["sleep"] = (1, 1),
            ["randsleep"] = (2, 2),
            ["randdelay"] = (2, 2),
        };

    internal static ParsedMacro Parse(string text, string mode)
    {
        ParsedMacro parsed = new();
        if (mode.Equals(MacroRunModes.Staged, StringComparison.Ordinal))
        {
            ParseStaged(text, parsed);
        }
        else
        {
            ParseLines(text, parsed.Body, parsed.Errors, 1);
        }
        return parsed;
    }

    private static void ParseStaged(string text, ParsedMacro parsed)
    {
        List<MacroCommand> target = parsed.OnPress;
        string[] lines = text.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n');
        for (int index = 0; index < lines.Length; index++)
        {
            string trimmed = lines[index].Trim();
            if (trimmed.Equals("[on_press]", StringComparison.OrdinalIgnoreCase))
            {
                target = parsed.OnPress;
                continue;
            }
            if (trimmed.Equals("[while_hold]", StringComparison.OrdinalIgnoreCase))
            {
                target = parsed.WhileHold;
                continue;
            }
            if (trimmed.Equals("[on_release]", StringComparison.OrdinalIgnoreCase))
            {
                target = parsed.OnRelease;
                continue;
            }
            ParseLine(lines[index], index + 1, target, parsed.Errors);
        }
    }

    private static void ParseLines(
        string text,
        List<MacroCommand> commands,
        List<MacroParseError> errors,
        int firstLine)
    {
        string[] lines = text.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n');
        for (int index = 0; index < lines.Length; index++)
        {
            ParseLine(lines[index], firstLine + index, commands, errors);
        }
    }

    private static void ParseLine(
        string source,
        int lineNumber,
        List<MacroCommand> commands,
        List<MacroParseError> errors)
    {
        string line = StripComment(source).Trim();
        if (line.Length == 0)
        {
            return;
        }
        Match match = CommandPattern().Match(line);
        if (!match.Success)
        {
            errors.Add(new MacroParseError(lineNumber, source, "命令格式无效"));
            return;
        }
        string operation = match.Groups[1].Value.ToLowerInvariant();
        if (!Specifications.TryGetValue(operation, out (int Minimum, int Maximum) specification))
        {
            errors.Add(new MacroParseError(lineNumber, source, $"不支持的命令 {operation}"));
            return;
        }
        List<string> arguments;
        try
        {
            arguments = SplitArguments(match.Groups[2].Value);
        }
        catch (FormatException exception)
        {
            errors.Add(new MacroParseError(lineNumber, source, exception.Message));
            return;
        }
        if (arguments.Count < specification.Minimum || arguments.Count > specification.Maximum)
        {
            errors.Add(new MacroParseError(
                lineNumber,
                source,
                $"参数数量应为 {specification.Minimum}" +
                (specification.Minimum == specification.Maximum ? string.Empty : $"-{specification.Maximum}")));
            return;
        }
        try
        {
            object[] parsedArguments = arguments
                .Select((argument, index) => ParseArgument(operation, index, argument))
                .ToArray();
            commands.Add(new MacroCommand(operation, parsedArguments, lineNumber, source));
        }
        catch (FormatException exception)
        {
            errors.Add(new MacroParseError(lineNumber, source, exception.Message));
        }
    }

    private static object ParseArgument(string operation, int index, string argument)
    {
        if (operation is "keydown" or "keyup" or "keypress" && index == 0)
        {
            return AutomationKeyMap.ResolveHid(argument);
        }
        if (!int.TryParse(argument.Trim(), out int value))
        {
            throw new FormatException($"参数必须是整数：{argument}");
        }
        return value;
    }

    private static string StripComment(string line)
    {
        char quote = '\0';
        bool escaped = false;
        for (int index = 0; index < line.Length; index++)
        {
            char current = line[index];
            if (escaped)
            {
                escaped = false;
                continue;
            }
            if (current == '\\' && quote != '\0')
            {
                escaped = true;
                continue;
            }
            if (current is '\'' or '"')
            {
                quote = quote == current ? '\0' : quote == '\0' ? current : quote;
            }
            else if (current == '#' && quote == '\0')
            {
                return line[..index];
            }
        }
        return line;
    }

    private static List<string> SplitArguments(string text)
    {
        if (string.IsNullOrWhiteSpace(text))
        {
            return [];
        }
        List<string> result = [];
        StringBuilder current = new();
        char quote = '\0';
        bool escaped = false;
        foreach (char character in text)
        {
            if (escaped)
            {
                current.Append(character);
                escaped = false;
                continue;
            }
            if (character == '\\' && quote != '\0')
            {
                current.Append(character);
                escaped = true;
                continue;
            }
            if (character is '\'' or '"')
            {
                quote = quote == character ? '\0' : quote == '\0' ? character : quote;
                current.Append(character);
                continue;
            }
            if (character == ',' && quote == '\0')
            {
                result.Add(current.ToString().Trim());
                current.Clear();
                continue;
            }
            current.Append(character);
        }
        if (quote != '\0')
        {
            throw new FormatException("字符串引号未闭合");
        }
        result.Add(current.ToString().Trim());
        return result;
    }

    [GeneratedRegex(@"^\s*([a-zA-Z_]\w*)\s*\((.*)\)\s*$")]
    private static partial Regex CommandPattern();
}

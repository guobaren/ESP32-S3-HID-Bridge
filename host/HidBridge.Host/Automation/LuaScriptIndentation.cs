using System.Text;

namespace HidBridge.Host.Automation;

/// <summary>
/// 调整 Lua 每一行的前导缩进和安全的行内空格，不插入、删除或重排换行。
/// </summary>
internal static class LuaScriptIndentation
{
    private enum TokenKind
    {
        Atom,
        Operator,
        Punctuation,
        Comment,
    }

    private readonly record struct Token(TokenKind Kind, string Text);

    private readonly record struct LineShape(
        string? FirstWord,
        int OpenBlocks,
        int CloseBlocks,
        int CloseBlocksBeforeBody);

    private static readonly string[] MultiCharacterOperators =
    [
        "...",
        "==",
        "~=",
        "<=",
        ">=",
        "//",
        "<<",
        ">>",
        "..",
    ];

    internal static string Align(string? text)
    {
        if (string.IsNullOrEmpty(text))
        {
            return text ?? string.Empty;
        }

        string normalized = text.Replace("\r\n", "\n", StringComparison.Ordinal)
            .Replace('\r', '\n');
        string[] lines = normalized.Split('\n');
        StringBuilder result = new();
        int indent = 0;

        for (int index = 0; index < lines.Length; index++)
        {
            string trimmed = lines[index].Trim();
            if (trimmed.Length == 0)
            {
                if (index < lines.Length - 1)
                {
                    result.Append('\n');
                }
                continue;
            }

            string formatted = NormalizeInlineSpacing(trimmed);
            LineShape shape = AnalyzeLine(formatted);
            int lineIndent = Math.Max(0, indent - shape.CloseBlocksBeforeBody);
            result.Append(' ', lineIndent * 4);
            result.Append(formatted);
            if (index < lines.Length - 1)
            {
                result.Append('\n');
            }

            indent = Math.Max(0, indent - shape.CloseBlocks + shape.OpenBlocks);
        }

        string aligned = result.ToString();
        return Environment.NewLine == "\n"
            ? aligned
            : aligned.Replace("\n", Environment.NewLine, StringComparison.Ordinal);
    }

    private static string NormalizeInlineSpacing(string line)
    {
        List<Token> tokens = Tokenize(line);
        if (tokens.Count == 0)
        {
            return string.Empty;
        }

        StringBuilder result = new();
        for (int index = 0; index < tokens.Count; index++)
        {
            Token token = tokens[index];
            if (token.Kind == TokenKind.Comment)
            {
                if (result.Length > 0)
                {
                    result.Append(' ');
                }
                result.Append(token.Text);
                break;
            }

            if (NeedsSpace(tokens, index))
            {
                result.Append(' ');
            }
            result.Append(token.Text);
        }

        return result.ToString().TrimEnd();
    }

    private static List<Token> Tokenize(string line)
    {
        List<Token> tokens = [];
        for (int index = 0; index < line.Length;)
        {
            if (char.IsWhiteSpace(line[index]))
            {
                index++;
                continue;
            }

            if (line[index] is '\'' or '"')
            {
                int end = SkipQuotedString(line, index);
                tokens.Add(new Token(TokenKind.Atom, line[index..end]));
                index = end;
                continue;
            }

            if (line[index] == '[' && TrySkipLongString(line, index, out int longStringEnd))
            {
                tokens.Add(new Token(TokenKind.Atom, line[index..longStringEnd]));
                index = longStringEnd;
                continue;
            }

            if (line[index] == '-' && index + 1 < line.Length && line[index + 1] == '-')
            {
                tokens.Add(new Token(TokenKind.Comment, line[index..]));
                break;
            }

            if (char.IsLetter(line[index]) || line[index] == '_')
            {
                int start = index++;
                while (index < line.Length &&
                       (char.IsLetterOrDigit(line[index]) || line[index] == '_'))
                {
                    index++;
                }
                tokens.Add(new Token(TokenKind.Atom, line[start..index]));
                continue;
            }

            if (char.IsDigit(line[index]))
            {
                int start = index;
                index = SkipNumber(line, index);
                tokens.Add(new Token(TokenKind.Atom, line[start..index]));
                continue;
            }

            string? operatorText = TryReadOperator(line, index);
            if (operatorText is not null)
            {
                tokens.Add(new Token(
                    operatorText == "..." ? TokenKind.Atom : TokenKind.Operator,
                    operatorText));
                index += operatorText.Length;
                continue;
            }

            tokens.Add(new Token(TokenKind.Punctuation, line[index].ToString()));
            index++;
        }

        return tokens;
    }

    private static bool NeedsSpace(IReadOnlyList<Token> tokens, int index)
    {
        if (index == 0)
        {
            return false;
        }

        Token previous = tokens[index - 1];
        Token current = tokens[index];
        if (current.Kind == TokenKind.Comment)
        {
            return false;
        }

        bool currentUnary = IsUnaryOperator(tokens, index);
        bool previousUnary = previous.Kind == TokenKind.Operator &&
            IsUnaryOperator(tokens, index - 1);

        if (current.Kind == TokenKind.Operator)
        {
            if (currentUnary)
            {
                return previous.Kind == TokenKind.Operator ||
                    previous.Kind == TokenKind.Atom ||
                    (previous.Kind == TokenKind.Punctuation &&
                     previous.Text is "," or ";");
            }

            return true;
        }

        if (previous.Kind == TokenKind.Operator)
        {
            return !previousUnary;
        }

        if (current.Kind == TokenKind.Punctuation)
        {
            return current.Text switch
            {
                "," or ";" or ")" or "]" or "}" or "." or ":" => false,
                "(" => previous.Kind == TokenKind.Operator && !previousUnary ||
                       previous.Kind == TokenKind.Punctuation && previous.Text is "," or ";" ||
                       previous.Kind == TokenKind.Atom && IsControlKeyword(previous.Text),
                "[" => previous.Kind == TokenKind.Operator && !previousUnary ||
                       previous.Kind == TokenKind.Punctuation && previous.Text is "," or ";",
                "{" => previous.Kind == TokenKind.Operator && !previousUnary ||
                       previous.Kind == TokenKind.Punctuation && previous.Text is "," or ";" ||
                       previous.Kind == TokenKind.Atom && previous.Text is "return" or "yield",
                _ => false,
            };
        }

        if (previous.Kind == TokenKind.Punctuation)
        {
            return previous.Text is "," or ";" ||
                previous.Text is ")" or "]" or "}";
        }

        return previous.Kind == TokenKind.Atom && current.Kind == TokenKind.Atom;
    }

    private static bool IsUnaryOperator(IReadOnlyList<Token> tokens, int index)
    {
        Token token = tokens[index];
        if (token.Kind != TokenKind.Operator || token.Text is not ("+" or "-" or "#" or "~"))
        {
            return false;
        }

        if (index == 0)
        {
            return true;
        }

        Token previous = tokens[index - 1];
        if (previous.Kind == TokenKind.Operator)
        {
            return true;
        }

        if (previous.Kind == TokenKind.Punctuation)
        {
            return previous.Text is "(" or "[" or "{" or "," or ";" or ":";
        }

        return previous.Kind == TokenKind.Atom &&
            previous.Text is "return" or "then" or "do" or "else" or "elseif" or "and" or "or" or "not";
    }

    private static bool IsControlKeyword(string text) =>
        text is "if" or "for" or "while" or "elseif" or "until";

    private static string? TryReadOperator(string text, int start)
    {
        foreach (string candidate in MultiCharacterOperators)
        {
            if (start + candidate.Length <= text.Length &&
                text.AsSpan(start, candidate.Length).SequenceEqual(candidate))
            {
                return candidate;
            }
        }

        return text[start] switch
        {
            '+' or '-' or '*' or '/' or '%' or '^' or '#' or '=' or '<' or '>' or '&' or '|' or '~'
                => text[start].ToString(),
            _ => null,
        };
    }

    private static int SkipNumber(string text, int start)
    {
        int index = start;
        bool hexadecimal = index + 1 < text.Length &&
            text[index] == '0' && text[index + 1] is 'x' or 'X';
        if (hexadecimal)
        {
            index += 2;
            while (index < text.Length && IsHexDigit(text[index]))
            {
                index++;
            }
            if (index < text.Length && text[index] == '.' &&
                !(index + 1 < text.Length && text[index + 1] == '.'))
            {
                index++;
                while (index < text.Length && IsHexDigit(text[index]))
                {
                    index++;
                }
            }
            if (index < text.Length && text[index] is 'p' or 'P')
            {
                index++;
                if (index < text.Length && text[index] is '+' or '-')
                {
                    index++;
                }
                while (index < text.Length && char.IsDigit(text[index]))
                {
                    index++;
                }
            }
            return index;
        }

        while (index < text.Length && char.IsDigit(text[index]))
        {
            index++;
        }
        if (index < text.Length && text[index] == '.' &&
            !(index + 1 < text.Length && text[index + 1] == '.'))
        {
            index++;
            while (index < text.Length && char.IsDigit(text[index]))
            {
                index++;
            }
        }
        if (index < text.Length && text[index] is 'e' or 'E')
        {
            index++;
            if (index < text.Length && text[index] is '+' or '-')
            {
                index++;
            }
            while (index < text.Length && char.IsDigit(text[index]))
            {
                index++;
            }
        }
        return index;
    }

    private static bool IsHexDigit(char value) =>
        char.IsDigit(value) || value is >= 'a' and <= 'f' or >= 'A' and <= 'F';

    private static LineShape AnalyzeLine(string line)
    {
        List<string> words = [];
        bool loopHeader = false;
        int openBlocks = 0;
        int closeBlocks = 0;
        int closeBlocksBeforeBody = 0;
        for (int index = 0; index < line.Length;)
        {
            if (line[index] is '\'' or '"')
            {
                index = SkipQuotedString(line, index);
                continue;
            }
            if (line[index] == '-' && index + 1 < line.Length && line[index + 1] == '-')
            {
                break;
            }
            if (line[index] == '[' && TrySkipLongString(line, index, out int longStringEnd))
            {
                index = longStringEnd;
                continue;
            }
            if (!char.IsLetter(line[index]) && line[index] != '_')
            {
                index++;
                continue;
            }

            int start = index++;
            while (index < line.Length &&
                   (char.IsLetterOrDigit(line[index]) || line[index] == '_'))
            {
                index++;
            }
            string word = line[start..index];
            words.Add(word);
            switch (word)
            {
                case "end":
                case "until":
                    closeBlocks++;
                    break;
                case "else":
                case "elseif":
                    closeBlocks++;
                    openBlocks++;
                    break;
                case "function":
                case "if":
                case "repeat":
                    openBlocks++;
                    break;
                case "for":
                case "while":
                    openBlocks++;
                    loopHeader = true;
                    break;
                case "do" when !loopHeader:
                    openBlocks++;
                    break;
            }
        }

        if (words.Count > 0 && words[0] is "end" or "until" or "else" or "elseif")
        {
            closeBlocksBeforeBody = 1;
        }

        return new LineShape(words.FirstOrDefault(), openBlocks, closeBlocks, closeBlocksBeforeBody);
    }

    private static int SkipQuotedString(string text, int start)
    {
        char quote = text[start++];
        while (start < text.Length)
        {
            if (text[start] == '\\')
            {
                start = Math.Min(text.Length, start + 2);
                continue;
            }
            if (text[start++] == quote)
            {
                break;
            }
        }
        return start;
    }

    private static bool TrySkipLongString(string text, int start, out int end)
    {
        end = start;
        int equals = 0;
        int cursor = start + 1;
        while (cursor < text.Length && text[cursor] == '=')
        {
            equals++;
            cursor++;
        }
        if (cursor >= text.Length || text[cursor] != '[')
        {
            return false;
        }

        string closing = "]" + new string('=', equals) + "]";
        int closingIndex = text.IndexOf(closing, cursor + 1, StringComparison.Ordinal);
        end = closingIndex >= 0 ? closingIndex + closing.Length : text.Length;
        return true;
    }
}

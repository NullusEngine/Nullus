using System.Text.RegularExpressions;

internal static partial class MetaParserTool
{
    private static List<T> TryExtract<T>(Func<List<T>> extractor)
    {
        try
        {
            return extractor();
        }
        catch
        {
            return [];
        }
    }

    private static string ExtractBraceBody(string text, int openBraceIndex)
    {
        if (openBraceIndex < 0 || openBraceIndex >= text.Length || text[openBraceIndex] != '{')
            return string.Empty;

        var depth = 0;
        for (var i = openBraceIndex; i < text.Length; i++)
        {
            if (text[i] == '{')
                depth++;
            else if (text[i] == '}')
            {
                depth--;
                if (depth == 0)
                    return text[(openBraceIndex + 1)..i];
            }
        }

        return string.Empty;
    }

    private static string ExtractNamespaceFromText(string prefix)
    {
        var namespaceStack = new Stack<string[]>();
        foreach (Match match in Regex.Matches(prefix, @"namespace\s+([A-Za-z_][\w:]*)\s*\{|[{}]", RegexOptions.CultureInvariant).Cast<Match>())
        {
            if (match.Groups[1].Success)
            {
                namespaceStack.Push(match.Groups[1].Value
                    .Split("::", StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries));
            }
            else if (match.Value == "}" && namespaceStack.Count > 0)
            {
                namespaceStack.Pop();
            }
        }

        return string.Join("::", namespaceStack.Reverse().SelectMany(static parts => parts));
    }

    private static List<ReflectBaseInfo> ParseBasesFromText(string basesText, string namespacePrefix)
    {
        if (string.IsNullOrWhiteSpace(basesText))
            return [];

        return basesText
            .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Select(baseType => Regex.Replace(baseType, @"\b(public|protected|private|virtual)\b", string.Empty))
            .Select(NormalizeTypeName)
            .Select(typeName => QualifyTypeName(typeName, namespacePrefix))
            .Where(static typeName => !string.IsNullOrWhiteSpace(typeName))
            .Distinct(StringComparer.Ordinal)
            .Select(static typeName => new ReflectBaseInfo(typeName))
            .ToList();
    }

    private static IEnumerable<ReflectTypeInfo> ParseNestedEnumsFromText(string body, string ownerQualifiedName, string includePath, string sourceFilePath)
    {
        body = StripBlockComments(body);
        foreach (Match match in Regex.Matches(
                     body,
                     @"ENUM\s*\([^)]*\)\s*enum\s+(?:class\s+)?(?:[A-Za-z_]\w*\s+)*(?<name>[A-Za-z_]\w*)\s*(?:\:\s*(?<underlying>[^{]+))?\s*\{(?<items>.*?)\}\s*;",
                     RegexOptions.Singleline | RegexOptions.CultureInvariant).Cast<Match>())
        {
            var enumName = match.Groups["name"].Value;
            if (string.IsNullOrWhiteSpace(enumName))
                continue;

            var items = SplitTopLevel(match.Groups["items"].Value, ',')
                .Select(static item => item.Trim())
                .Where(static item => !string.IsNullOrWhiteSpace(item))
                .Select(item => Regex.Match(item, @"^(?<name>[A-Za-z_]\w*)", RegexOptions.CultureInvariant))
                .Where(static itemMatch => itemMatch.Success)
                .Select(itemMatch => new ReflectEnumValueInfo(itemMatch.Groups["name"].Value))
                .ToList();

            if (items.Count == 0)
                continue;

            yield return new ReflectTypeInfo(
                enumName,
                ownerQualifiedName,
                $"{ownerQualifiedName}::{enumName}",
                includePath,
                sourceFilePath,
                [],
                [],
                [],
                true,
                items);
        }
    }

    private static string StripInlineComments(string line)
    {
        var commentIndex = line.IndexOf("//", StringComparison.Ordinal);
        return commentIndex >= 0 ? line[..commentIndex] : line;
    }

    private static string StripBlockComments(string text)
        => Regex.Replace(text, @"/\*.*?\*/", string.Empty, RegexOptions.Singleline | RegexOptions.CultureInvariant);

    private static bool ContainsUnsupportedReflectionType(string text)
        => !string.IsNullOrWhiteSpace(text)
           && (text.Contains("std::unique_ptr", StringComparison.Ordinal)
               || text.Contains("unique_ptr<", StringComparison.Ordinal));

    private static string NormalizePropertyTypeName(string typeName)
    {
        if (string.IsNullOrWhiteSpace(typeName))
            return string.Empty;

        var normalized = typeName.Trim();
        normalized = normalized.Replace("&&", string.Empty, StringComparison.Ordinal)
            .Replace("&", string.Empty, StringComparison.Ordinal)
            .Trim();

        if (normalized.StartsWith("const ", StringComparison.Ordinal) && !normalized.Contains('*'))
            normalized = normalized["const ".Length..].Trim();

        return normalized;
    }

    private static string BuildPropertyNameFromSuffix(string suffix)
    {
        if (string.IsNullOrWhiteSpace(suffix))
            return string.Empty;
        if (suffix.Length == 1)
            return suffix.ToLowerInvariant();

        return char.ToLowerInvariant(suffix[0]) + suffix[1..];
    }

    private static string BuildPropertyNameFromGetterName(string getterName)
        => BuildPropertyNameFromSuffix(getterName[GetGetterPrefixLength(getterName)..]);

    private static string BuildNullMethodSetterExpression(string fullTypeName, string fieldTypeName)
        => $"static_cast<void ({fullTypeName}::*)({fieldTypeName})>(nullptr)";

    private static string QualifyTypeName(string typeName, string namespacePrefix)
    {
        if (string.IsNullOrWhiteSpace(typeName))
            return typeName;
        if (typeName.Contains("::", StringComparison.Ordinal))
            return QualifyRootNamespaceType(typeName);
        if (string.IsNullOrWhiteSpace(namespacePrefix))
            return typeName;
        if (BuiltinTypeNames.Contains(typeName))
            return typeName;
        if (typeName.IndexOfAny(new[] { ' ', '&', '*', '<', '>', '[', ']', '(', ')', ',' }) >= 0)
            return QualifyRootNamespaceType(typeName);

        return $"{namespacePrefix}::{typeName}";
    }

    private static string QualifyRootNamespaceType(string typeName)
    {
        if (string.IsNullOrWhiteSpace(typeName))
            return typeName;

        var qualified = typeName;
        foreach (var segment in RootQualifiedNamespaceSegments)
        {
            qualified = Regex.Replace(
                qualified,
                $@"(?<![\w:]){Regex.Escape(segment)}::",
                $"NLS::{segment}::",
                RegexOptions.CultureInvariant);
        }

        return qualified;
    }

    private static string GetGeneratedRelativeBasePath(string headerPath)
        => Path.ChangeExtension(headerPath.Replace('/', Path.DirectorySeparatorChar), null) ?? headerPath;
}

using System.Collections.Generic;
using System.Linq;
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

    private static bool ContainsUnsupportedReflectionType(string text)
        => !string.IsNullOrWhiteSpace(text)
           && (text.Contains("std::unique_ptr", StringComparison.Ordinal)
               || text.Contains("unique_ptr<", StringComparison.Ordinal));

    private static string NormalizePropertyTypeName(string typeName)
    {
        if (string.IsNullOrWhiteSpace(typeName))
            return string.Empty;

        var normalized = typeName.Trim();
        normalized = Regex.Replace(normalized, @"\s+const\s*([&*])", "$1", RegexOptions.CultureInvariant);
        normalized = Regex.Replace(normalized, @"([&*])\s+const\b", "$1", RegexOptions.CultureInvariant);
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

    private static string QualifyTypeName(
        string typeName,
        string namespacePrefix,
        IReadOnlyDictionary<string, List<string>>? visibleTypes = null)
    {
        if (string.IsNullOrWhiteSpace(typeName))
            return typeName;
        if (string.IsNullOrWhiteSpace(namespacePrefix))
            return QualifyRootNamespaceType(typeName);
        if (BuiltinTypeNames.Contains(typeName))
            return typeName;
        if (typeName.IndexOfAny(new[] { ' ', '&', '*', '<', '>', '[', ']', '(', ')', ',' }) >= 0)
            return QualifyTypeNameInExpression(typeName, namespacePrefix, visibleTypes);
        if (typeName.Contains("::", StringComparison.Ordinal))
            return QualifyRootNamespaceType(typeName);

        return $"{namespacePrefix}::{typeName}";
    }

    private static string QualifyTypeNameInExpression(
        string typeName,
        string namespacePrefix,
        IReadOnlyDictionary<string, List<string>>? visibleTypes = null)
    {
        if (string.IsNullOrWhiteSpace(typeName))
            return typeName;

        var qualified = QualifyRootNamespaceType(typeName);
        if (string.IsNullOrWhiteSpace(namespacePrefix))
            return qualified;

        return Regex.Replace(
            qualified,
            @"(?<![\w:])([A-Za-z_]\w*)(?![\w:])",
            match =>
            {
                var token = match.Value;
                if (BuiltinTypeNames.Contains(token)
                    || string.Equals(token, "const", StringComparison.Ordinal)
                    || string.Equals(token, "volatile", StringComparison.Ordinal))
                {
                    return token;
                }

                if (IsAlreadyQualifiedAt(qualified, match.Index, match.Length))
                    return token;

                var resolved = ResolveVisibleTypeName(token, namespacePrefix, visibleTypes);
                return resolved ?? token;
            },
            RegexOptions.CultureInvariant);
    }

    private static bool IsAlreadyQualifiedAt(string text, int index, int length)
    {
        return (index >= 2 && text[index - 1] == ':' && text[index - 2] == ':')
               || (index + length + 1 < text.Length && text[index + length] == ':' && text[index + length + 1] == ':');
    }

    private static string? ResolveVisibleTypeName(
        string token,
        string namespacePrefix,
        IReadOnlyDictionary<string, List<string>>? visibleTypes)
    {
        if (visibleTypes is null || !visibleTypes.TryGetValue(token, out var candidates))
            return null;

        foreach (var namespaceCandidate in EnumerateNamespaceSearchOrder(namespacePrefix))
        {
            var expectedName = string.IsNullOrWhiteSpace(namespaceCandidate)
                ? token
                : $"{namespaceCandidate}::{token}";
            var match = candidates.FirstOrDefault(candidate => string.Equals(candidate, expectedName, StringComparison.Ordinal));
            if (!string.IsNullOrWhiteSpace(match))
                return match;
        }

        return candidates.Count == 1 ? candidates[0] : null;
    }

    private static IEnumerable<string> EnumerateNamespaceSearchOrder(string namespacePrefix)
    {
        var current = namespacePrefix;
        while (!string.IsNullOrWhiteSpace(current))
        {
            yield return current;
            var separatorIndex = current.LastIndexOf("::", StringComparison.Ordinal);
            current = separatorIndex < 0 ? string.Empty : current[..separatorIndex];
        }
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

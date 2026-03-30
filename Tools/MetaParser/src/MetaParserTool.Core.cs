using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using CppAst;
internal static partial class MetaParserTool
{
    private static readonly HashSet<string> UnsupportedMethodNames = new(StringComparer.Ordinal)
    {
        "typeof",
        "NLS_TYPEOF",
        "typeidof",
        "NLS_TYPEIDOF",
        "decltypeof",
        "NLS_DECLTYPEOF",
        "GENERATED_BODY"
    };

    private static readonly HashSet<string> BuiltinTypeNames = new(StringComparer.Ordinal)
    {
        "void",
        "bool",
        "char",
        "signed char",
        "unsigned char",
        "short",
        "unsigned short",
        "int",
        "unsigned int",
        "long",
        "unsigned long",
        "long long",
        "unsigned long long",
        "float",
        "double",
        "long double",
        "size_t",
        "std::size_t",
        "uint8_t",
        "uint16_t",
        "uint32_t",
        "uint64_t",
        "int8_t",
        "int16_t",
        "int32_t",
        "int64_t"
    };

    private static readonly string[] RootQualifiedNamespaceSegments =
    {
        "meta",
        "Maths",
        "Render",
        "Core",
        "Engine",
        "Platform",
        "UI",
        "Time",
        "Debug"
    };

    private static string DescribeHeaderParseRoutes(string headerText)
    {
        var routes = new List<string>();

        if (headerText.Contains("ENUM(", StringComparison.Ordinal))
            routes.Add("text-top-level-enum");

        if (ContainsGeneratedBody(headerText))
            routes.Add("text-type-body");

        if (headerText.Contains("MetaExternal", StringComparison.Ordinal)
            || headerText.Contains("REFLECT_EXTERNAL", StringComparison.Ordinal))
        {
            routes.Add("external-declaration");
        }

        if (!ContainsGeneratedBody(headerText)
            && !headerText.Contains("MetaExternal", StringComparison.Ordinal)
            && !headerText.Contains("REFLECT_EXTERNAL", StringComparison.Ordinal))
        {
            routes.Add("cppast");
        }

        if (routes.Count == 0)
            routes.Add("none");

        return string.Join(", ", routes);
    }

    private static IEnumerable<ReflectTypeInfo> ParseHeader(string rootDir, string headerPath, PrecompileParams config)
    {
        var types = new List<ReflectTypeInfo>();
        var headerText = File.ReadAllText(headerPath);
        types.AddRange(ParseTopLevelEnumsFromText(rootDir, headerPath, headerText));
        var hasReflectedTypeBodies = ContainsGeneratedBody(headerText);
        var hasExternalReflectionDeclarations = headerText.Contains("MetaExternal", StringComparison.Ordinal)
            || headerText.Contains("REFLECT_EXTERNAL", StringComparison.Ordinal);
        if (hasReflectedTypeBodies)
        {
            types.AddRange(ParseHeaderFromText(rootDir, headerPath, headerText, emitDiagnostics: true));
        }
        else if (!hasExternalReflectionDeclarations)
        {
            types.AddRange(ParseHeaderWithCppAst(rootDir, headerPath, config));
        }

        types.AddRange(ParseExternalReflectionDeclarations(rootDir, headerPath));
        return MergeReflectTypes(types);
    }

    private static IEnumerable<ReflectTypeInfo> ParseTopLevelEnumsFromText(string rootDir, string headerPath, string headerText)
    {
        var includePath = ToGeneratedIncludePath(rootDir, Path.GetFullPath(headerPath));
        var text = SanitizeTextForMacroParsing(headerText);
        var classBodyRanges = FindClassBodyRanges(text);

        foreach (Match match in Regex.Matches(
                     text,
                     @"ENUM\s*\([^)]*\)\s*enum\s+(?:class\s+)?(?:[A-Za-z_]\w*\s+)*(?<name>[A-Za-z_]\w*)\s*(?:\:\s*(?<underlying>[^{]+))?\s*\{(?<items>.*?)\}\s*;",
                     RegexOptions.Singleline | RegexOptions.CultureInvariant).Cast<Match>())
        {
            if (classBodyRanges.Any(range => match.Index > range.start && match.Index < range.end))
                continue;

            var enumName = match.Groups["name"].Value;
            if (string.IsNullOrWhiteSpace(enumName))
                continue;

            var namespacePrefix = ExtractNamespaceFromText(text[..match.Index]);
            var fullTypeName = string.IsNullOrWhiteSpace(namespacePrefix)
                ? enumName
                : $"{namespacePrefix}::{enumName}";

            var items = SplitTopLevel(match.Groups["items"].Value, ',')
                .Select(item => item.Trim())
                .Where(item => !string.IsNullOrWhiteSpace(item))
                .Select(item => Regex.Match(item, @"^(?<name>[A-Za-z_]\w*)", RegexOptions.CultureInvariant))
                .Where(itemMatch => itemMatch.Success)
                .Select(itemMatch => new ReflectEnumValueInfo(itemMatch.Groups["name"].Value))
                .ToList();

            if (items.Count == 0)
                continue;

            yield return new ReflectTypeInfo(
                enumName,
                namespacePrefix,
                fullTypeName,
                includePath,
                Path.GetFullPath(headerPath),
                [],
                [],
                [],
                true,
                items);
        }
    }

    private static List<(int start, int end)> FindClassBodyRanges(string text)
    {
        var ranges = new List<(int start, int end)>();
        foreach (Match match in Regex.Matches(
                     text,
                     @"\b(?:class|struct)\s+(?:[A-Za-z_]\w*\s+)*(?<name>[A-Za-z_]\w*)\s*(?:\:\s*[^{]+)?\s*\{",
                     RegexOptions.CultureInvariant).Cast<Match>())
        {
            var openBraceIndex = text.IndexOf('{', match.Index);
            if (openBraceIndex < 0)
                continue;

            var body = ExtractBraceBody(text, openBraceIndex);
            if (string.IsNullOrEmpty(body))
                continue;

            ranges.Add((openBraceIndex + 1, openBraceIndex + 1 + body.Length));
        }

        return ranges;
    }

    private static List<string> ExtractMacroInvocations(string text, string macroName)
    {
        var invocations = new List<string>();
        var startIndex = 0;

        while (startIndex < text.Length)
        {
            var match = Regex.Match(text[startIndex..], $@"\b{Regex.Escape(macroName)}\s*\(", RegexOptions.CultureInvariant);
            if (!match.Success)
                break;

            var macroIndex = startIndex + match.Index;
            var openParenIndex = text.IndexOf('(', macroIndex + macroName.Length);
            if (openParenIndex < 0)
                break;

            invocations.Add(ExtractDelimitedBody(text, openParenIndex, '(', ')'));
            startIndex = SkipDelimitedBody(text, openParenIndex, '(', ')');
        }

        return invocations;
    }

    private static string SanitizeTextForMacroParsing(string text)
        => string.Join(
            Environment.NewLine,
            StripBlockComments(text)
                .Split('\n')
                .Select(StripInlineComments));

    private static bool ContainsReflectedDeclaration(string text)
        => ContainsGeneratedBody(text)
           || text.Contains("ENUM(", StringComparison.Ordinal)
           || text.Contains("MetaExternal", StringComparison.Ordinal)
           || text.Contains("REFLECT_EXTERNAL", StringComparison.Ordinal);

    private static bool ContainsGeneratedBody(string text)
        => text.Contains("GENERATED_BODY(", StringComparison.Ordinal);

    private static List<string> SplitTopLevel(string text, char separator)
    {
        var result = new List<string>();
        var current = new StringBuilder();
        var parenDepth = 0;
        var angleDepth = 0;
        var braceDepth = 0;
        var bracketDepth = 0;
        var inString = false;
        char stringDelimiter = '\0';

        foreach (var ch in text)
        {
            if (inString)
            {
                current.Append(ch);
                if (ch == stringDelimiter)
                    inString = false;
                continue;
            }

            switch (ch)
            {
            case '\'':
            case '"':
                inString = true;
                stringDelimiter = ch;
                current.Append(ch);
                break;
            case '(':
                parenDepth++;
                current.Append(ch);
                break;
            case ')':
                parenDepth--;
                current.Append(ch);
                break;
            case '<':
                angleDepth++;
                current.Append(ch);
                break;
            case '>':
                if (angleDepth > 0)
                    angleDepth--;
                current.Append(ch);
                break;
            case '{':
                braceDepth++;
                current.Append(ch);
                break;
            case '}':
                braceDepth--;
                current.Append(ch);
                break;
            case '[':
                bracketDepth++;
                current.Append(ch);
                break;
            case ']':
                bracketDepth--;
                current.Append(ch);
                break;
            default:
                if (ch == separator && parenDepth == 0 && angleDepth == 0 && braceDepth == 0 && bracketDepth == 0)
                {
                    var part = current.ToString().Trim();
                    if (!string.IsNullOrWhiteSpace(part))
                        result.Add(part);
                    current.Clear();
                }
                else
                {
                    current.Append(ch);
                }
                break;
            }
        }

        var trailing = current.ToString().Trim();
        if (!string.IsNullOrWhiteSpace(trailing))
            result.Add(trailing);

        return result;
    }

    private static bool TryParseMacroInvocation(string text, out string macroName, out string macroBody)
    {
        macroName = string.Empty;
        macroBody = string.Empty;

        var trimmed = text.Trim();
        var openParenIndex = trimmed.IndexOf('(');
        if (openParenIndex <= 0 || !trimmed.EndsWith(")", StringComparison.Ordinal))
            return false;

        macroName = trimmed[..openParenIndex].Trim();
        macroBody = trimmed[(openParenIndex + 1)..^1].Trim();
        return !string.IsNullOrWhiteSpace(macroName);
    }

    private static string ExtractDelimitedBody(string text, int openIndex, char openChar, char closeChar)
    {
        if (openIndex < 0 || openIndex >= text.Length || text[openIndex] != openChar)
            return string.Empty;

        var start = openIndex + 1;
        var depth = 1;
        for (var index = start; index < text.Length; index++)
        {
            if (text[index] == openChar)
                depth++;
            else if (text[index] == closeChar)
            {
                depth--;
                if (depth == 0)
                    return text[start..index];
            }
        }

        return string.Empty;
    }

    private static int SkipDelimitedBody(string text, int openIndex, char openChar, char closeChar)
    {
        if (openIndex < 0 || openIndex >= text.Length || text[openIndex] != openChar)
            return text.Length;

        var depth = 1;
        for (var index = openIndex + 1; index < text.Length; index++)
        {
            if (text[index] == openChar)
                depth++;
            else if (text[index] == closeChar)
            {
                depth--;
                if (depth == 0)
                    return index + 1;
            }
        }

        return text.Length;
    }

    private static string ExtractSimpleClassName(string fullTypeName)
    {
        var separatorIndex = fullTypeName.LastIndexOf("::", StringComparison.Ordinal);
        return separatorIndex >= 0 ? fullTypeName[(separatorIndex + 2)..] : fullTypeName;
    }

    private static string NormalizeRegistrationName(string text)
    {
        var trimmed = text.Trim();
        if (trimmed.Length >= 2
            && ((trimmed[0] == '"' && trimmed[^1] == '"')
                || (trimmed[0] == '\'' && trimmed[^1] == '\'')))
        {
            return trimmed[1..^1].Trim();
        }

        return trimmed;
    }

    private static List<ReflectBaseInfo> ExtractBases(CppClass cls)
    {
        var result = new List<ReflectBaseInfo>();
        foreach (var baseType in cls.BaseTypes)
        {
            try
            {
                if (baseType.Visibility != CppVisibility.Public && baseType.Visibility != CppVisibility.Default)
                    continue;

                string typeName;
                if (baseType.Type is CppClass baseClass && !string.IsNullOrWhiteSpace(baseClass.FullName))
                    typeName = NormalizeTypeName(baseClass.FullName);
                else
                    typeName = NormalizeTypeName(baseType.Type.GetDisplayName());

                if (string.IsNullOrWhiteSpace(typeName))
                    continue;

                result.Add(new ReflectBaseInfo(typeName));
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[MetaParser] Warning: skipping base on {cls.FullName}: {ex.Message}");
            }
        }
        return result.DistinctBy(x => x.TypeName).ToList();
    }

    private static List<ReflectFieldInfo> ExtractFields(CppClass cls)
    {
        var result = new List<ReflectFieldInfo>();
        foreach (var field in cls.Fields)
        {
            try
            {
                if (!HasPropertyMarker(field))
                    continue;
                if (field.Visibility != CppVisibility.Public && field.Visibility != CppVisibility.Default)
                    continue;
                if (field.StorageQualifier == CppStorageQualifier.Static)
                    continue;
                if (string.IsNullOrWhiteSpace(field.Name))
                    continue;

                var typeName = NormalizeTypeName(field.Type.GetDisplayName());
                if (string.IsNullOrWhiteSpace(typeName) || ContainsUnsupportedReflectionType(typeName))
                    continue;

                result.Add(new ReflectFieldInfo(
                    field.Name,
                    typeName,
                    $"&{cls.FullName}::{field.Name}",
                    $"&{cls.FullName}::{field.Name}",
                false));
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[MetaParser] Warning: skipping field on {cls.FullName}: {ex.Message}");
            }
        }

        result.AddRange(ExtractAutoProperties(cls));

        return result
            .DistinctBy(f => f.Name)
            .ToList();
    }

    private static List<ReflectMethodInfo> ExtractMethods(CppClass cls)
    {
        var candidateMethods = new List<MethodCandidateInfo>();
        foreach (var method in cls.Functions)
        {
            try
            {
                if (!HasFunctionMarker(method))
                    continue;
                if (method.Visibility != CppVisibility.Public && method.Visibility != CppVisibility.Default)
                    continue;
                if (method.IsStatic || method.IsConstructor || method.IsDestructor)
                    continue;
                if (string.IsNullOrWhiteSpace(method.Name)
                    || method.Name.StartsWith("operator", StringComparison.Ordinal)
                    || !IsBindableMethodName(method.Name))
                    continue;

                var returnTypeName = NormalizeTypeName(method.ReturnType?.GetDisplayName() ?? string.Empty);
                if (ContainsUnsupportedReflectionType(returnTypeName))
                    continue;

                if (method.Parameters.Any(parameter => ContainsUnsupportedReflectionType(NormalizeTypeName(parameter.Type.GetDisplayName()))))
                    continue;

                candidateMethods.Add(new MethodCandidateInfo(
                    method.Name,
                    $"&{cls.FullName}::{method.Name}",
                    returnTypeName,
                    method.Parameters.Select(parameter => NormalizeTypeName(parameter.Type.GetDisplayName())).ToList(),
                    false,
                    false));
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[MetaParser] Warning: skipping method discovery on {cls.FullName}: {ex.Message}");
            }
        }

        var overloadedMethodNames = candidateMethods
            .GroupBy(candidate => candidate.Name, StringComparer.Ordinal)
            .Where(g => g.Count() > 1)
            .Select(g => g.Key)
            .ToHashSet(StringComparer.Ordinal);

        return candidateMethods
            .Where(candidate => !overloadedMethodNames.Contains(candidate.Name))
            .DistinctBy(candidate => candidate.Name)
            .Select(candidate => new ReflectMethodInfo(candidate.Name, candidate.PointerExpression, candidate.IsStatic, candidate.IsPrivate))
            .ToList();
    }

    private static List<ReflectFieldInfo> ExtractAutoProperties(CppClass cls)
    {
        var methods = new List<MethodCandidateInfo>();

        foreach (var method in cls.Functions)
        {
            try
            {
                if (!HasFunctionMarker(method))
                    continue;
                if (method.Visibility != CppVisibility.Public && method.Visibility != CppVisibility.Default)
                    continue;
                if (method.IsStatic || method.IsConstructor || method.IsDestructor)
                    continue;
                if (string.IsNullOrWhiteSpace(method.Name)
                    || method.Name.StartsWith("operator", StringComparison.Ordinal)
                    || !IsBindableMethodName(method.Name))
                {
                    continue;
                }

                var returnTypeName = NormalizeTypeName(method.ReturnType?.GetDisplayName() ?? string.Empty);
                if (ContainsUnsupportedReflectionType(returnTypeName))
                    continue;

                var parameterTypeNames = method.Parameters
                    .Select(parameter => NormalizeTypeName(parameter.Type.GetDisplayName()))
                    .ToList();

                if (parameterTypeNames.Any(ContainsUnsupportedReflectionType))
                    continue;

                methods.Add(new MethodCandidateInfo(
                    method.Name,
                    $"&{cls.FullName}::{method.Name}",
                    returnTypeName,
                    parameterTypeNames,
                    false,
                    false));
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[MetaParser] Warning: skipping auto-property discovery on {cls.FullName}: {ex.Message}");
            }
        }

        return BuildAutoPropertyFields(cls.FullName, methods, new HashSet<string>(StringComparer.Ordinal));
    }

    private static string BuildGeneratedFileId(string headerPath)
        => $"NLS_FID_{SanitizeIdentifier(headerPath.Replace('\\', '_').Replace('/', '_').Replace('.', '_'))}";

    private static string BuildGeneratedBodyMacroName(string fileId, int line)
        => $"{fileId}_{line}_GENERATED_BODY";

    private static List<int> FindGeneratedBodyLines(string sourceFilePath)
    {
        if (string.IsNullOrWhiteSpace(sourceFilePath) || !File.Exists(sourceFilePath))
            return [];

        var text = File.ReadAllText(sourceFilePath);
        return Regex.Matches(text, @"\bGENERATED_BODY\s*\(", RegexOptions.CultureInvariant)
            .Cast<Match>()
            .Select(match => 1 + text.Take(match.Index).Count(ch => ch == '\n'))
            .ToList();
    }

    private static string BuildPrivateAccessStructName(string qualifiedName)
        => $"PrivateAccess_{SanitizeIdentifier(qualifiedName)}";

    private static string BuildPrivateFieldAccessorName(string fieldName, int index)
        => $"Field_{SanitizeIdentifier(fieldName)}_{index}";

    private static string BuildPrivateMethodAccessorName(string methodName, int index)
        => $"Method_{SanitizeIdentifier(methodName)}_{index}";

    private static string BuildRegisterFunctionName(string qualifiedName)
        => $"RegisterType_{SanitizeIdentifier(qualifiedName)}";

    private static string BuildRegistrarClassName(string qualifiedName)
        => $"StaticTypeRegister_{SanitizeIdentifier(qualifiedName)}";

    private static bool HasMemberMarker(ICppAttributeContainer container, params string[] markers)
    {
        if (container is null)
            return false;

        var hasAttribute = container.Attributes.Any(attribute => IsMemberAttribute(attribute, markers));
#pragma warning disable CS0618
        var hasTokenAttribute = container.TokenAttributes.Any(attribute => IsMemberAttribute(attribute, markers));
#pragma warning restore CS0618
        var hasMetaAttribute = container.MetaAttributes.MetaList.Any(attribute => IsMemberMetaAttribute(attribute, markers));
        return hasAttribute || hasTokenAttribute || hasMetaAttribute;
    }

    private static bool IsMemberAttribute(CppAttribute attribute, params string[] markers)
    {
        if (!(attribute.Kind == AttributeKind.AnnotateAttribute
              || string.Equals(attribute.Name, "annotate", StringComparison.OrdinalIgnoreCase)
              || attribute.Name.Contains("annotate", StringComparison.OrdinalIgnoreCase)))
        {
            return false;
        }

        return ContainsAnyMarker(attribute.Arguments, markers)
               || ContainsAnyMarker(attribute.ToString(), markers);
    }

    private static bool IsMemberMetaAttribute(MetaAttribute attribute, params string[] markers)
    {
        if (attribute is null)
            return false;

        if (ContainsAnyMarker(attribute.FeatureName, markers))
            return true;

        return attribute.ArgumentMap.Keys.Any(key => ContainsAnyMarker(key, markers))
               || attribute.ArgumentMap.Values.Any(value => ContainsAnyMarker(Convert.ToString(value), markers));
    }

    private static bool IsReflectionAttribute(CppAttribute attribute)
    {
        if (!(attribute.Kind == AttributeKind.AnnotateAttribute
              || string.Equals(attribute.Name, "annotate", StringComparison.OrdinalIgnoreCase)
              || attribute.Name.Contains("annotate", StringComparison.OrdinalIgnoreCase)))
        {
            return false;
        }

        return string.IsNullOrWhiteSpace(attribute.Arguments)
               || ContainsReflectionMarker(attribute.Arguments)
               || ContainsReflectionMarker(attribute.ToString());
    }

    private static bool IsReflectionMetaAttribute(MetaAttribute attribute)
    {
        if (attribute is null)
            return false;

        if (ContainsReflectionMarker(attribute.FeatureName))
            return true;

        return attribute.ArgumentMap.Keys.Any(ContainsReflectionMarker)
               || attribute.ArgumentMap.Values.Any(value => ContainsReflectionMarker(Convert.ToString(value)));
    }

    private static bool ContainsReflectionMarker(string? text)
        => !string.IsNullOrWhiteSpace(text)
           && text.IndexOf("Reflection", StringComparison.OrdinalIgnoreCase) >= 0;

    private static bool ContainsAnyMarker(string? text, params string[] markers)
        => !string.IsNullOrWhiteSpace(text)
           && markers.Any(marker => text.IndexOf(marker, StringComparison.OrdinalIgnoreCase) >= 0);

    private static string ExtractNamespace(string fullName, string className)
    {
        if (string.IsNullOrWhiteSpace(fullName))
            return string.Empty;

        var suffix = $"::{className}";
        return fullName.EndsWith(suffix, StringComparison.Ordinal)
            ? fullName[..^suffix.Length]
            : string.Empty;
    }

    private static string ToGeneratedIncludePath(string rootDir, string headerPath)
    {
        var runtimeDir = Path.Combine(rootDir, "Runtime");
        var rel = Path.GetRelativePath(runtimeDir, headerPath).Replace('\\', '/');
        var slash = rel.IndexOf('/');
        return slash >= 0 ? rel[(slash + 1)..] : rel;
    }

    private static string NormalizeTypeName(string typeName)
        => string.IsNullOrWhiteSpace(typeName) ? string.Empty : typeName.Replace("class ", string.Empty).Replace("struct ", string.Empty).Trim();

    private static bool IsBindableMethodName(string methodName)
        => !string.IsNullOrWhiteSpace(methodName)
           && Regex.IsMatch(methodName, @"^[A-Za-z_]\w*$", RegexOptions.CultureInvariant)
           && !UnsupportedMethodNames.Contains(methodName);

    private static string SanitizeIdentifier(string value)
        => Regex.Replace(value, @"[^A-Za-z0-9_]", "_", RegexOptions.CultureInvariant);

    private static StringComparison PathComparison => OperatingSystem.IsWindows()
        ? StringComparison.OrdinalIgnoreCase
        : StringComparison.Ordinal;
}

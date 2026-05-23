using System.Linq;
using System.Globalization;
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

        if (ContainsGeneratedBody(headerText)
            || headerText.Contains("CLASS(", StringComparison.Ordinal)
            || headerText.Contains("STRUCT(", StringComparison.Ordinal)
            || headerText.Contains("ENUM(", StringComparison.Ordinal)
            || headerText.Contains("PROPERTY(", StringComparison.Ordinal)
            || headerText.Contains("FUNCTION(", StringComparison.Ordinal))
        {
            routes.Add("cppast");
        }

        if (routes.Count == 0)
            routes.Add("none");

        return string.Join(", ", routes);
    }

    private static IEnumerable<ReflectTypeInfo> ParseHeader(string rootDir, string headerPath, PrecompileParams config)
        => MergeReflectTypes(ParseHeaderWithCppAst(rootDir, headerPath, config));

    private static List<ReflectTypeInfo> MergeReflectTypes(IEnumerable<ReflectTypeInfo> types)
    {
        var merged = new Dictionary<string, ReflectTypeInfo>(StringComparer.Ordinal);

        foreach (var type in types.Where(static type => !string.IsNullOrWhiteSpace(type.QualifiedName)))
        {
            if (!merged.TryGetValue(type.QualifiedName, out var existing))
            {
                merged[type.QualifiedName] = type with
                {
                    Bases = type.Bases.DistinctBy(static baseInfo => baseInfo.TypeName).ToList(),
                    Fields = type.Fields.DistinctBy(static field => field.Name).ToList(),
                    Methods = type.Methods.DistinctBy(static method => $"{method.IsStatic}:{method.Name}:{method.PointerExpression}").ToList(),
                    TypeMetas = (type.TypeMetas ?? []).DistinctBy(static meta => $"{meta.PropertyTypeName}:{meta.InitializerArguments}").ToList(),
                    EnumValues = (type.EnumValues ?? []).DistinctBy(static value => value.Name).ToList()
                };
                continue;
            }

            merged[type.QualifiedName] = existing with
            {
                Bases = existing.Bases.Concat(type.Bases)
                    .DistinctBy(static baseInfo => baseInfo.TypeName)
                    .ToList(),
                Fields = existing.Fields.Concat(type.Fields)
                    .DistinctBy(static field => field.Name)
                    .ToList(),
                Methods = existing.Methods.Concat(type.Methods)
                    .DistinctBy(static method => $"{method.IsStatic}:{method.Name}:{method.PointerExpression}")
                    .ToList(),
                TypeMetas = (existing.TypeMetas ?? []).Concat(type.TypeMetas ?? [])
                    .DistinctBy(static meta => $"{meta.PropertyTypeName}:{meta.InitializerArguments}")
                    .ToList(),
                IsEnum = existing.IsEnum || type.IsEnum,
                EnumValues = (existing.EnumValues ?? []).Concat(type.EnumValues ?? [])
                    .DistinctBy(static value => value.Name)
                    .ToList()
            };
        }

        return merged.Values.ToList();
    }

    private static bool ContainsReflectedDeclaration(string text)
    {
        var reflectedText = StripCommentsAndDisabledPreprocessorBlocks(text);
        return ContainsGeneratedBody(reflectedText)
               || reflectedText.Contains("CLASS(", StringComparison.Ordinal)
               || reflectedText.Contains("STRUCT(", StringComparison.Ordinal)
               || reflectedText.Contains("ENUM(", StringComparison.Ordinal)
               || reflectedText.Contains("PROPERTY(", StringComparison.Ordinal)
               || reflectedText.Contains("FUNCTION(", StringComparison.Ordinal)
               || reflectedText.Contains("NLS_META_EXTERNAL_TYPE_NAME(", StringComparison.Ordinal);
    }

    private static IEnumerable<string> ExtractExternalReflectionTypeNames(string text)
    {
        foreach (Match match in Regex.Matches(
                     StripCommentsAndDisabledPreprocessorBlocks(text),
                     @"\bNLS_META_EXTERNAL_TYPE_NAME\s*\(([^)]*)\)",
                     RegexOptions.CultureInvariant))
        {
            var typeName = NormalizePropertyTypeName(match.Groups[1].Value);
            if (!string.IsNullOrWhiteSpace(typeName))
                yield return typeName;
        }
    }

    private static string StripCommentsAndDisabledPreprocessorBlocks(string text)
    {
        if (string.IsNullOrEmpty(text))
            return string.Empty;

        return StripDisabledIfZeroBlocks(StripCommentsAndStrings(text));
    }

    private static string StripCommentsAndStrings(string text)
    {
        var result = new StringBuilder(text.Length);
        var inLineComment = false;
        var inBlockComment = false;
        var inString = false;
        var inRawString = false;
        string rawStringTerminator = string.Empty;
        char stringDelimiter = '\0';

        for (var index = 0; index < text.Length; ++index)
        {
            var current = text[index];
            var next = index + 1 < text.Length ? text[index + 1] : '\0';

            if (inLineComment)
            {
                if (current == '\r' || current == '\n')
                {
                    inLineComment = false;
                    result.Append(current);
                }
                continue;
            }

            if (inBlockComment)
            {
                if (current == '*' && next == '/')
                {
                    inBlockComment = false;
                    ++index;
                }
                else if (current == '\r' || current == '\n')
                {
                    result.Append(current);
                }
                continue;
            }

            if (inRawString)
            {
                if (current == '\r' || current == '\n')
                    result.Append(current);
                if (!string.IsNullOrEmpty(rawStringTerminator) &&
                    index + rawStringTerminator.Length <= text.Length &&
                    string.Compare(text, index, rawStringTerminator, 0, rawStringTerminator.Length, StringComparison.Ordinal) == 0)
                {
                    index += rawStringTerminator.Length - 1;
                    inRawString = false;
                    rawStringTerminator = string.Empty;
                }
                continue;
            }

            if (inString)
            {
                if (current == '\r' || current == '\n')
                    result.Append(current);
                if (current == '\\' && next != '\0')
                {
                    ++index;
                    continue;
                }
                if (current == stringDelimiter)
                    inString = false;
                continue;
            }

            if (current == 'R' && next == '"')
            {
                var delimiterEnd = text.IndexOf('(', index + 2);
                if (delimiterEnd >= 0)
                {
                    var delimiter = text.Substring(index + 2, delimiterEnd - (index + 2));
                    rawStringTerminator = ")" + delimiter + "\"";
                    inRawString = true;
                    index = delimiterEnd;
                    continue;
                }
            }

            if (current == '/' && next == '/')
            {
                inLineComment = true;
                ++index;
                continue;
            }

            if (current == '/' && next == '*')
            {
                inBlockComment = true;
                ++index;
                continue;
            }

            if (current == '"' || current == '\'')
            {
                inString = true;
                stringDelimiter = current;
            }

            result.Append(current);
        }

        return result.ToString();
    }

    private static string StripDisabledIfZeroBlocks(string text)
    {
        var lines = text.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n');
        var result = new StringBuilder(text.Length);
        var stack = new Stack<(bool ParentActive, bool BranchActive, bool AnyBranchTaken)>();
        var active = true;

        foreach (var line in lines)
        {
            var trimmed = line.TrimStart();
            if (Regex.IsMatch(trimmed, @"^#\s*if\s+0(?:\s|$)", RegexOptions.CultureInvariant))
            {
                stack.Push((active, false, false));
                active = false;
                result.AppendLine();
                continue;
            }

            if (Regex.IsMatch(trimmed, @"^#\s*if(?:\s|$)", RegexOptions.CultureInvariant))
            {
                stack.Push((active, active, active));
                result.AppendLine();
                continue;
            }

            if (stack.Count > 0 &&
                Regex.IsMatch(trimmed, @"^#\s*elif(?:\s|$)", RegexOptions.CultureInvariant))
            {
                var state = stack.Pop();
                var branchActive = state.ParentActive && !state.AnyBranchTaken && !IsDisabledElifDirective(trimmed);
                stack.Push((state.ParentActive, branchActive, state.AnyBranchTaken || branchActive));
                active = branchActive;
                result.AppendLine();
                continue;
            }

            if (stack.Count > 0 &&
                Regex.IsMatch(trimmed, @"^#\s*else(?:\s|$)", RegexOptions.CultureInvariant))
            {
                var state = stack.Pop();
                var branchActive = state.ParentActive && !state.AnyBranchTaken;
                stack.Push((state.ParentActive, branchActive, true));
                active = branchActive;
                result.AppendLine();
                continue;
            }

            if (stack.Count > 0 &&
                Regex.IsMatch(trimmed, @"^#\s*endif(?:\s|$)", RegexOptions.CultureInvariant))
            {
                var state = stack.Pop();
                active = state.ParentActive;
                result.AppendLine();
                continue;
            }

            if (!active)
            {
                result.AppendLine();
                continue;
            }

            result.AppendLine(line);
        }

        return result.ToString();
    }

    private static bool IsDisabledElifDirective(string trimmedDirective)
    {
        var match = Regex.Match(trimmedDirective, @"^#\s*elif\s+(.+)$", RegexOptions.CultureInvariant);
        if (!match.Success)
            return true;

        var expression = match.Groups[1].Value.Trim();
        if (Regex.IsMatch(expression, @"^0(?:\s|$)", RegexOptions.CultureInvariant))
            return true;
        if (Regex.IsMatch(expression, @"^1(?:\s|$)", RegexOptions.CultureInvariant))
            return false;

        return false;
    }

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

    private static List<ReflectFieldInfo> ExtractFields(CppClass cls, IReadOnlyDictionary<string, List<string>> visibleTypes)
    {
        var result = new List<ReflectFieldInfo>();
        var methodCandidates = ExtractMethodCandidates(cls, visibleTypes);
        foreach (var field in cls.Fields)
        {
            try
            {
                var propertyAttributes = GetReflectionAttributes(field, "Property").ToList();
                if (propertyAttributes.Count == 0)
                    continue;
                if (field.Visibility != CppVisibility.Public &&
                    field.Visibility != CppVisibility.Private &&
                    field.Visibility != CppVisibility.Default)
                    continue;
                if (field.StorageQualifier == CppStorageQualifier.Static)
                    continue;
                if (string.IsNullOrWhiteSpace(field.Name))
                    continue;

                var typeName = NormalizeAstMemberTypeName(field.Type, cls, visibleTypes);
                if (string.IsNullOrWhiteSpace(typeName) || ContainsUnsupportedReflectionType(typeName))
                    continue;

                var isPrivateField = field.Visibility == CppVisibility.Private ||
                                     (field.Visibility == CppVisibility.Default && cls.ClassKind == CppClassKind.Class);

                result.Add(new ReflectFieldInfo(
                    field.Name,
                    typeName,
                    $"&{cls.FullName}::{field.Name}",
                    $"&{cls.FullName}::{field.Name}",
                    isPrivateField,
                    ExtractPropertyMetas(propertyAttributes)));
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[MetaParser] Warning: skipping field on {cls.FullName}: {ex.Message}");
                if (ex is FormatException)
                    throw;
            }
        }

        result.AddRange(BuildAutoPropertyFields(cls.FullName, methodCandidates, []));

        return result
            .DistinctBy(f => f.Name)
            .ToList();
    }

    private static List<ReflectMethodInfo> ExtractMethods(CppClass cls, IReadOnlyDictionary<string, List<string>> visibleTypes)
    {
        var candidateMethods = ExtractMethodCandidates(cls, visibleTypes);

        var inlineMethods = candidateMethods
            .Select(candidate => new ReflectMethodInfo(candidate.Name, candidate.PointerExpression, candidate.IsStatic, candidate.IsPrivate))
            .ToList();

        return inlineMethods
            .DistinctBy(static method => $"{method.IsStatic}:{method.Name}:{method.PointerExpression}")
            .ToList();
    }

    private static List<MethodCandidateInfo> ExtractMethodCandidates(CppClass cls, IReadOnlyDictionary<string, List<string>> visibleTypes)
    {
        var methods = new List<MethodCandidateInfo>();

        foreach (var method in cls.Functions)
        {
            try
            {
                if (!HasFunctionMarker(method))
                    continue;
                if (method.Visibility == CppVisibility.Private)
                {
                    throw new NotSupportedException(
                        "Private reflected accessor methods are not supported. " +
                        $"Move '{cls.FullName}::{method.Name}' to public visibility or reflect the backing field directly.");
                }
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

                var returnTypeName = NormalizeAstMemberTypeName(method.ReturnType, cls, visibleTypes);
                if (ContainsUnsupportedReflectionType(returnTypeName))
                    continue;

                var parameterTypeNames = method.Parameters
                    .Select(parameter => NormalizeAstMemberTypeName(parameter.Type, cls, visibleTypes))
                    .ToList();

                if (parameterTypeNames.Any(ContainsUnsupportedReflectionType))
                    continue;

                var signatureReturnTypeName = NormalizeAstSignatureTypeName(method.ReturnType, cls, visibleTypes);
                var signatureParameterTypeNames = method.Parameters
                    .Select(parameter => NormalizeAstSignatureTypeName(parameter.Type, cls, visibleTypes))
                    .ToList();
                var propertyName = ExtractPropertyNameOverride(method);

                methods.Add(new MethodCandidateInfo(
                    method.Name,
                    BuildMethodPointerExpression(cls.FullName, method.Name, signatureReturnTypeName, signatureParameterTypeNames, method.IsConst),
                    returnTypeName,
                    parameterTypeNames,
                    false,
                    false,
                    method.IsConst,
                    propertyName));
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[MetaParser] Warning: skipping auto-property discovery on {cls.FullName}: {ex.Message}");
                if (ex is NotSupportedException)
                    throw;
            }
        }

        return methods;
    }

    private static string NormalizeAstSignatureTypeName(CppType? type, CppClass owner, IReadOnlyDictionary<string, List<string>> visibleTypes)
    {
        var normalized = NormalizeTypeName(GetAstSignatureTypeName(type));
        if (string.IsNullOrWhiteSpace(normalized))
            return string.Empty;

        normalized = NormalizeStdTypeName(normalized);
        normalized = QualifyTypeNameInExpression(normalized, ExtractNamespace(owner.FullName, owner.Name), visibleTypes);

        foreach (var nestedEnum in owner.Enums)
        {
            normalized = Regex.Replace(
                normalized,
                $@"(?<![\w:]){Regex.Escape(nestedEnum.Name)}(?![\w:])",
                $"{owner.FullName}::{nestedEnum.Name}",
                RegexOptions.CultureInvariant);
        }

        normalized = Regex.Replace(normalized, @"\s+", " ", RegexOptions.CultureInvariant).Trim();
        normalized = Regex.Replace(normalized, @"\s*([&*])\s*", "$1", RegexOptions.CultureInvariant);
        return normalized;
    }

    private static string GetAstSignatureTypeName(CppType? type)
    {
        switch (type)
        {
        case null:
            return string.Empty;
        case CppQualifiedType qualified when qualified.Qualifier == CppTypeQualifier.Const:
            return $"const {GetAstSignatureTypeName(qualified.ElementType)}";
        case CppQualifiedType qualified:
            return GetAstSignatureTypeName(qualified.ElementType);
        case CppReferenceType reference:
            return $"{GetAstSignatureTypeName(reference.ElementType)}&";
        case CppPointerType pointer:
            return $"{GetAstSignatureTypeName(pointer.ElementType)}*";
        case CppTypedef typedef:
            return !string.IsNullOrWhiteSpace(typedef.FullName) ? typedef.FullName : typedef.Name;
        case CppClass cls:
            return !string.IsNullOrWhiteSpace(cls.FullName) ? cls.FullName : cls.Name;
        case CppEnum cppEnum:
            return !string.IsNullOrWhiteSpace(cppEnum.FullName) ? cppEnum.FullName : cppEnum.Name;
        case CppUnexposedType unexposed:
            return NormalizeUnexposedAstTypeName(unexposed);
        default:
            return type.GetDisplayName();
        }
    }

    private static string BuildMethodPointerExpression(
        string fullTypeName,
        string methodName,
        string returnTypeName,
        IReadOnlyList<string> parameterTypeNames,
        bool isConst)
    {
        var constSuffix = isConst ? " const" : string.Empty;
        return $"static_cast<{returnTypeName} ({fullTypeName}::*)({string.Join(", ", parameterTypeNames)}){constSuffix}>(&{fullTypeName}::{methodName})";
    }

    private static string? ExtractPropertyNameOverride(CppFunction method)
    {
        foreach (var attribute in GetReflectionAttributes(method, "Property"))
        {
            var body = StripAttributeMarker(attribute, "Property").Trim();
            if (string.IsNullOrWhiteSpace(body))
                continue;

            return NormalizeRegistrationName(body);
        }

        return null;
    }

    private static List<ReflectTypeMetaInfo> ExtractPropertyMetas(IEnumerable<string> propertyAttributes)
    {
        var metas = new List<ReflectTypeMetaInfo>();
        foreach (var attribute in propertyAttributes)
        {
            foreach (var token in SplitTopLevel(StripAttributeMarker(attribute, "Property"), ','))
            {
                var trimmed = token.Trim();
                if (string.IsNullOrWhiteSpace(trimmed))
                    continue;
                if (IsCppAstEmptyReflectionArgumentArtifact(trimmed))
                    continue;

                if (string.Equals(trimmed, "RequiresRestart", StringComparison.Ordinal))
                    metas.Add(new ReflectTypeMetaInfo("NLS::meta::RequiresRestart", ""));
                else if (TryParseRangeMeta(trimmed, out var rangeInitializer))
                    metas.Add(new ReflectTypeMetaInfo("NLS::meta::Range", rangeInitializer));
                else
                    throw new FormatException($"Unsupported PROPERTY metadata token `{trimmed}`.");
            }
        }

        return metas
            .DistinctBy(static meta => $"{meta.PropertyTypeName}:{meta.InitializerArguments}")
            .ToList();
    }

    private static bool IsCppAstEmptyReflectionArgumentArtifact(string token)
        => token.StartsWith("=", StringComparison.Ordinal) &&
           token.Contains("System.Collections.Generic.List", StringComparison.Ordinal);

    private static bool TryParseRangeMeta(string token, out string initializerArguments)
    {
        initializerArguments = string.Empty;
        if (!token.StartsWith("Range(", StringComparison.Ordinal) ||
            !token.EndsWith(")", StringComparison.Ordinal))
            return false;

        var body = token.Substring("Range(".Length, token.Length - "Range(".Length - 1);
        var parts = SplitTopLevel(body, ',');
        if (parts.Count != 2 ||
            !TryParseFiniteFloatLiteral(parts[0], out var min) ||
            !TryParseFiniteFloatLiteral(parts[1], out var max))
        {
            throw new FormatException($"Range metadata must be Range(<finite-number>, <finite-number>), got `{token}`.");
        }

        if (min > max)
            throw new FormatException($"Range metadata min must be less than or equal to max, got `{token}`.");

        initializerArguments =
            $"{FormatFloatInitializer(min)}, {FormatFloatInitializer(max)}";
        return true;
    }

    private static bool TryParseFiniteFloatLiteral(string text, out float value)
    {
        var trimmed = text.Trim();
        if (trimmed.EndsWith("f", StringComparison.OrdinalIgnoreCase))
            trimmed = trimmed[..^1];

        if (!float.TryParse(trimmed, NumberStyles.Float, CultureInfo.InvariantCulture, out value))
            return false;

        return float.IsFinite(value);
    }

    private static string FormatFloatInitializer(float value)
        => value.ToString("R", CultureInfo.InvariantCulture) + "f";

    private static string BuildGeneratedFileId(string headerPath)
        => $"NLS_FID_{SanitizeIdentifier(headerPath.Replace('\\', '_').Replace('/', '_').Replace('.', '_'))}";

    private static string BuildGeneratedBodyMacroName(string fileId, int line)
        => $"{fileId}_{line}_GENERATED_BODY";

    private static List<int> FindGeneratedBodyLines(string sourceFilePath)
    {
        if (string.IsNullOrWhiteSpace(sourceFilePath) || !File.Exists(sourceFilePath))
            return [];

        var text = File.ReadAllText(sourceFilePath);
        return FindGeneratedBodyLineMatches(text)
            .Select(match => 1 + text.Take(match.Index).Count(ch => ch == '\n'))
            .ToList();
    }

    private static int? FindGeneratedBodyLineForType(string sourceFilePath, string className, int startOffset, int endOffset)
    {
        if (string.IsNullOrWhiteSpace(sourceFilePath) || !File.Exists(sourceFilePath))
            return null;

        var text = File.ReadAllText(sourceFilePath);
        return FindGeneratedBodyLineInClassText(text, className)
               ?? FindGeneratedBodyLineInSpan(text, startOffset, endOffset);
    }

    private static int? FindGeneratedBodyLineInClassText(string text, string className)
    {
        if (string.IsNullOrWhiteSpace(text) || string.IsNullOrWhiteSpace(className))
            return null;

        var classPattern = $@"\b(?:CLASS|STRUCT)\s*\([^)]*\b{Regex.Escape(className)}\b[^)]*\)";
        foreach (Match classMatch in Regex.Matches(text, classPattern, RegexOptions.CultureInvariant))
        {
            var openBraceIndex = text.IndexOf('{', classMatch.Index + classMatch.Length);
            if (openBraceIndex < 0)
                continue;

            var closeBraceIndex = FindMatchingBrace(text, openBraceIndex);
            if (!closeBraceIndex.HasValue)
                continue;

            var bodyLine = FindGeneratedBodyLineMatches(text)
                .Where(match => match.Index > openBraceIndex && match.Index < closeBraceIndex.Value)
                .Select(match => (int?)(1 + text.Take(match.Index).Count(ch => ch == '\n')))
                .FirstOrDefault();
            if (bodyLine.HasValue)
                return bodyLine;
        }

        return null;
    }

    private static int? FindGeneratedBodyLineInSpan(string text, int startOffset, int endOffset)
    {
        if (string.IsNullOrWhiteSpace(text) || startOffset < 0 || endOffset <= startOffset || startOffset >= text.Length)
            return null;

        var clampedEnd = Math.Min(endOffset, text.Length);
        return FindGeneratedBodyLineMatches(text)
            .Where(match => match.Index >= startOffset && match.Index < clampedEnd)
            .Select(match => (int?)(1 + text.Take(match.Index).Count(ch => ch == '\n')))
            .FirstOrDefault();
    }

    private static IEnumerable<Match> FindGeneratedBodyLineMatches(string text)
        => Regex.Matches(text, @"\bGENERATED_BODY\s*\(", RegexOptions.CultureInvariant)
            .Cast<Match>();

    private static int? FindMatchingBrace(string text, int openBraceIndex)
    {
        var depth = 0;
        var inString = false;
        var inLineComment = false;
        var inBlockComment = false;
        char stringDelimiter = '\0';

        for (var index = openBraceIndex; index < text.Length; ++index)
        {
            var ch = text[index];
            var next = index + 1 < text.Length ? text[index + 1] : '\0';

            if (inLineComment)
            {
                if (ch == '\n')
                    inLineComment = false;
                continue;
            }

            if (inBlockComment)
            {
                if (ch == '*' && next == '/')
                {
                    inBlockComment = false;
                    ++index;
                }
                continue;
            }

            if (inString)
            {
                if (ch == '\\')
                {
                    ++index;
                    continue;
                }
                if (ch == stringDelimiter)
                    inString = false;
                continue;
            }

            if (ch == '/' && next == '/')
            {
                inLineComment = true;
                ++index;
                continue;
            }

            if (ch == '/' && next == '*')
            {
                inBlockComment = true;
                ++index;
                continue;
            }

            if (ch == '"' || ch == '\'')
            {
                inString = true;
                stringDelimiter = ch;
                continue;
            }

            if (ch == '{')
            {
                ++depth;
                continue;
            }

            if (ch != '}')
                continue;

            --depth;
            if (depth == 0)
                return index;
        }

        return null;
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

    private static List<string> GetReflectionAttributes(ICppAttributeContainer container, params string[] markers)
        => GetReflectionAttributeElements(container, markers)
            .Select(static item => item.Text)
            .ToList();

    private static List<(string Text, CppElement Element)> GetReflectionAttributeElements(ICppAttributeContainer container, params string[] markers)
        => EnumerateAttributeTexts(container)
            .Where(attribute => markers.Length == 0 || markers.Any(marker => AttributeHasMarker(attribute.Text, marker)))
            .ToList();

    private static IEnumerable<(string Text, CppElement Element)> EnumerateAttributeTexts(ICppAttributeContainer? container)
    {
        if (container is null)
            yield break;

        foreach (var attribute in container.Attributes)
            if (TryGetAnnotateAttributeText(attribute, out var text))
                yield return (text, attribute);

#pragma warning disable CS0618
        foreach (var attribute in container.TokenAttributes)
            if (TryGetAnnotateAttributeText(attribute, out var text))
                yield return (text, attribute);
#pragma warning restore CS0618

        foreach (var attribute in container.MetaAttributes.MetaList)
        {
            if (!string.IsNullOrWhiteSpace(attribute.FeatureName))
                yield return (attribute.FeatureName, (CppElement)container);

            foreach (var (key, value) in attribute.ArgumentMap)
            {
                var valueText = Convert.ToString(value);
                if (!string.IsNullOrWhiteSpace(valueText))
                    yield return ($"{key} = {valueText}", (CppElement)container);
            }
        }
    }

    private static bool TryGetAnnotateAttributeText(CppAttribute attribute, out string text)
    {
        text = string.Empty;
        if (!(attribute.Kind == AttributeKind.AnnotateAttribute
              || string.Equals(attribute.Name, "annotate", StringComparison.OrdinalIgnoreCase)
              || attribute.Name.Contains("annotate", StringComparison.OrdinalIgnoreCase)))
        {
            return false;
        }

        text = !string.IsNullOrWhiteSpace(attribute.Arguments)
            ? attribute.Arguments
            : attribute.ToString();
        return !string.IsNullOrWhiteSpace(text);
    }

    private static bool AttributeHasMarker(string attributeText, string marker)
    {
        if (string.IsNullOrWhiteSpace(attributeText) || string.IsNullOrWhiteSpace(marker))
            return false;

        var trimmed = attributeText.Trim();
        return string.Equals(trimmed, marker, StringComparison.OrdinalIgnoreCase)
               || trimmed.StartsWith($"{marker},", StringComparison.OrdinalIgnoreCase)
               || trimmed.StartsWith($"{marker} ", StringComparison.OrdinalIgnoreCase)
               || trimmed.Contains($", {marker}", StringComparison.OrdinalIgnoreCase);
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

    private static Dictionary<string, string> ParseAttributeNamedArguments(string attributeText, string marker)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        var body = StripAttributeMarker(attributeText, marker);
        foreach (var entry in SplitTopLevel(body, ','))
        {
            var trimmed = entry.Trim();
            var separatorIndex = trimmed.IndexOf('=', StringComparison.Ordinal);
            if (separatorIndex <= 0)
                continue;

            var key = trimmed[..separatorIndex].Trim();
            var value = trimmed[(separatorIndex + 1)..].Trim();
            if (string.IsNullOrWhiteSpace(key) || string.IsNullOrWhiteSpace(value))
                continue;

            result[key] = value;
        }

        return result;
    }

    private static bool HasNamedAttributeArgument(string attributeText, string key)
        => ParseAttributeNamedArguments(attributeText, ExtractAttributeMarker(attributeText)).ContainsKey(key);

    private static string StripAttributeMarker(string attributeText, string marker)
    {
        var trimmed = attributeText.Trim();
        if (trimmed.StartsWith(marker, StringComparison.OrdinalIgnoreCase))
            trimmed = trimmed[marker.Length..].TrimStart();
        if (trimmed.StartsWith(",", StringComparison.Ordinal))
            trimmed = trimmed[1..].TrimStart();
        return trimmed;
    }

    private static string ExtractAttributeMarker(string attributeText)
    {
        var trimmed = attributeText.Trim();
        var separatorIndex = trimmed.IndexOfAny(new[] { ',', ' ', '\t' });
        return separatorIndex < 0 ? trimmed : trimmed[..separatorIndex];
    }

    private static List<ReflectTypeMetaInfo> ExtractTypeMetas(CppClass cls)
    {
        var metas = new List<ReflectTypeMetaInfo>();
        foreach (var attribute in GetReflectionAttributes(cls, "Reflection"))
        {
            foreach (var token in SplitTopLevel(StripAttributeMarker(attribute, "Reflection"), ','))
            {
                if (!TryParseMacroInvocation(token, out var macroName, out var macroArgs))
                    continue;

                if (string.Equals(macroName, "ComponentMenu", StringComparison.Ordinal))
                    metas.Add(new ReflectTypeMetaInfo("NLS::meta::ComponentMenu", macroArgs.Trim()));
            }
        }

        return metas;
    }

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

    private static string NormalizeAstMemberTypeName(
        CppType? type,
        CppClass owner,
        IReadOnlyDictionary<string, List<string>> visibleTypes)
        => NormalizeAstMemberTypeName(GetAstTypeName(type), owner, visibleTypes);

    private static string NormalizeAstMemberTypeName(
        string typeName,
        CppClass owner,
        IReadOnlyDictionary<string, List<string>> visibleTypes)
    {
        var normalized = NormalizeTypeName(typeName);
        if (string.IsNullOrWhiteSpace(normalized))
            return string.Empty;

        normalized = NormalizeStdTypeName(normalized);
        normalized = NormalizeTrailingConstReference(normalized);
        normalized = QualifyTypeName(normalized, ExtractNamespace(owner.FullName, owner.Name), visibleTypes);

        foreach (var nestedEnum in owner.Enums)
        {
            if (string.Equals(normalized, nestedEnum.Name, StringComparison.Ordinal))
                return $"{owner.FullName}::{nestedEnum.Name}";
        }

        return normalized;
    }

    private static string GetAstTypeName(CppType? type)
    {
        switch (type)
        {
        case null:
            return string.Empty;
        case CppQualifiedType qualified:
            return GetAstTypeName(qualified.ElementType);
        case CppReferenceType reference:
            return $"{GetAstTypeName(reference.ElementType)}&";
        case CppPointerType pointer:
            return $"{GetAstTypeName(pointer.ElementType)}*";
        case CppTypedef typedef:
            return !string.IsNullOrWhiteSpace(typedef.FullName) ? typedef.FullName : typedef.Name;
        case CppClass cls:
            return !string.IsNullOrWhiteSpace(cls.FullName) ? cls.FullName : cls.Name;
        case CppEnum cppEnum:
            return !string.IsNullOrWhiteSpace(cppEnum.FullName) ? cppEnum.FullName : cppEnum.Name;
        case CppUnexposedType unexposed:
            return NormalizeUnexposedAstTypeName(unexposed);
        default:
            return type.GetDisplayName();
        }
    }

    private static string NormalizeUnexposedAstTypeName(CppUnexposedType type)
    {
        var name = NormalizeStdTypeName(type.Name);
        if (type.TemplateParameters.Count == 0)
            return name;

        var openGenericIndex = name.IndexOf('<');
        var genericName = openGenericIndex >= 0 ? name[..openGenericIndex].Trim() : name;
        return $"{genericName}<{string.Join(", ", type.TemplateParameters.Select(GetAstTypeName))}>";
    }

    private static string NormalizeStdTypeName(string typeName)
    {
        var normalized = Regex.Replace(typeName, @"(?<![\w:])basic_string(?![\w:])", "std::string", RegexOptions.CultureInvariant);
        normalized = Regex.Replace(
            normalized,
            @"std::basic_string\s*<\s*char\s*,\s*std::char_traits\s*<\s*char\s*>\s*,\s*std::allocator\s*<\s*char\s*>\s*>",
            "std::string",
            RegexOptions.CultureInvariant);
        normalized = Regex.Replace(normalized, @"(?<![\w:])string(?![\w:])", "std::string", RegexOptions.CultureInvariant);
        normalized = Regex.Replace(normalized, @"(?<![\w:])vector\s*<", "std::vector<", RegexOptions.CultureInvariant);
        normalized = Regex.Replace(normalized, @"(?<![\w:])Array\s*<", "NLS::Array<", RegexOptions.CultureInvariant);
        normalized = Regex.Replace(normalized, @",\s*std::allocator\s*<\s*([^<>]+?)\s*>\s*(?=>)", string.Empty, RegexOptions.CultureInvariant);
        return normalized;
    }

    private static string NormalizeTrailingConstReference(string typeName)
    {
        var normalized = typeName.Trim();
        normalized = Regex.Replace(normalized, @"\s+const\s*([&*])", "$1", RegexOptions.CultureInvariant);
        normalized = Regex.Replace(normalized, @"([&*])\s+const\b", "$1", RegexOptions.CultureInvariant);
        normalized = Regex.Replace(normalized, @"\s+", " ", RegexOptions.CultureInvariant).Trim();
        return normalized;
    }

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

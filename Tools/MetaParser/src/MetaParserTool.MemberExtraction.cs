using System.Text;
using System.Text.RegularExpressions;

internal static partial class MetaParserTool
{
    private static TextMemberParseResult ExtractMembersFromText(string body, string fullTypeName, bool defaultPublic)
    {
        body = StripBlockComments(body);

        var fields = new List<ReflectFieldInfo>();
        var methods = new List<ReflectMethodInfo>();
        var methodCandidates = new List<MethodCandidateInfo>();
        var explicitProperties = new List<ExplicitPropertyDirectiveInfo>();
        var explicitMethods = new List<ExplicitMethodDirectiveInfo>();
        var className = ExtractSimpleClassName(fullTypeName);
        var namespacePrefix = ExtractNamespace(fullTypeName, ExtractSimpleClassName(fullTypeName));
        var nestedTypeNames = FindNestedTypeNames(body);

        var currentAccess = defaultPublic ? "public" : "private";
        var currentStatement = new StringBuilder();
        var braceDepth = 0;
        var pendingProperty = false;
        var pendingFunction = false;

        foreach (var rawLine in body.Split(new[] { '\r', '\n' }, StringSplitOptions.None))
        {
            var line = StripInlineComments(rawLine).Trim();
            if (string.IsNullOrWhiteSpace(line))
                continue;

            if (Regex.IsMatch(line, @"^GENERATED_BODY\s*\([^)]*\)\s*;?$", RegexOptions.CultureInvariant))
                continue;

            if (TryParseStandalonePropertyDirective(line, namespacePrefix, explicitProperties))
                continue;

            if (TryParseStandaloneFunctionDirective(line, fullTypeName, explicitMethods))
                continue;

            if (Regex.IsMatch(line, @"\bPROPERTY\s*\(", RegexOptions.CultureInvariant))
            {
                pendingProperty = true;
                line = Regex.Replace(line, @"\bPROPERTY\s*\([^)]*\)", string.Empty, RegexOptions.CultureInvariant).Trim();
            }

            if (Regex.IsMatch(line, @"\bFUNCTION\s*\(", RegexOptions.CultureInvariant))
            {
                pendingFunction = true;
                line = Regex.Replace(line, @"\bFUNCTION\s*\([^)]*\)", string.Empty, RegexOptions.CultureInvariant).Trim();
            }

            if (string.IsNullOrWhiteSpace(line))
                continue;

            if (braceDepth == 0)
            {
                if (line == "public:")
                {
                    currentAccess = "public";
                    continue;
                }

                if (line == "protected:")
                {
                    currentAccess = "protected";
                    continue;
                }

                if (line == "private:")
                {
                    currentAccess = "private";
                    continue;
                }
            }

            if (currentAccess != "public")
            {
                braceDepth += line.Count(static c => c == '{');
                braceDepth -= line.Count(static c => c == '}');
                continue;
            }

            currentStatement.Append(' ').Append(line);

            braceDepth += line.Count(static c => c == '{');
            braceDepth -= line.Count(static c => c == '}');

            if (braceDepth == 0 && (line.EndsWith(";", StringComparison.Ordinal) || line.EndsWith("}", StringComparison.Ordinal)))
            {
                var statement = Regex.Replace(currentStatement.ToString(), @"\s+", " ").Trim();
                currentStatement.Clear();

                if (statement.Contains(" enum ", StringComparison.Ordinal)
                    || statement.StartsWith("enum ", StringComparison.Ordinal)
                    || statement.StartsWith("using ", StringComparison.Ordinal)
                    || statement.StartsWith("typedef ", StringComparison.Ordinal)
                    || statement.StartsWith("friend ", StringComparison.Ordinal))
                {
                    pendingProperty = false;
                    pendingFunction = false;
                    continue;
                }

                if (statement.Contains('('))
                {
                    var shouldReflectMethod = pendingFunction;
                    pendingProperty = false;
                    pendingFunction = false;
                    if (!shouldReflectMethod)
                        continue;

                    if (ContainsUnsupportedReflectionType(statement))
                        continue;

                    var nameMatches = Regex.Matches(statement, @"(?<name>[~A-Za-z_]\w*)\s*\([^;{}]*\)", RegexOptions.CultureInvariant);
                    if (nameMatches.Count == 0)
                        continue;

                    var methodName = nameMatches[^1].Groups["name"].Value;
                    if (string.IsNullOrWhiteSpace(methodName)
                        || methodName == className
                        || methodName == $"~{className}"
                        || methodName.StartsWith("operator", StringComparison.Ordinal)
                        || !IsBindableMethodName(methodName)
                        || statement.Contains(" static ", StringComparison.Ordinal)
                        || statement.StartsWith("static ", StringComparison.Ordinal))
                    {
                        continue;
                    }

                    var parameterTypeNames = ExtractParameterTypeNamesFromText(statement, methodName, namespacePrefix, fullTypeName, nestedTypeNames);
                    if (parameterTypeNames.Any(ContainsUnsupportedReflectionType))
                        continue;

                    methodCandidates.Add(new MethodCandidateInfo(
                        methodName,
                        $"&{fullTypeName}::{methodName}",
                        ExtractReturnTypeFromText(statement, methodName, namespacePrefix, fullTypeName, nestedTypeNames),
                        parameterTypeNames,
                        false,
                        false));
                    continue;
                }

                var shouldReflectField = pendingProperty;
                pendingProperty = false;
                pendingFunction = false;
                if (!shouldReflectField)
                    continue;

                var fieldMatch = Regex.Match(
                    statement,
                    @"^(?<type>.+?)\s+(?<name>[A-Za-z_]\w*)(?:\s*=\s*.+)?;$",
                    RegexOptions.CultureInvariant);

                if (!fieldMatch.Success)
                    continue;

                var fieldType = QualifyMemberTypeName(NormalizeTypeName(fieldMatch.Groups["type"].Value), namespacePrefix, fullTypeName, nestedTypeNames);
                var fieldName = fieldMatch.Groups["name"].Value;

                if (string.IsNullOrWhiteSpace(fieldType)
                    || string.IsNullOrWhiteSpace(fieldName)
                    || ContainsUnsupportedReflectionType(fieldType)
                    || fieldType.StartsWith("static ", StringComparison.Ordinal)
                    || fieldType.StartsWith("constexpr ", StringComparison.Ordinal)
                    || fieldType.StartsWith("inline ", StringComparison.Ordinal)
                    || fieldType.Contains(" static ", StringComparison.Ordinal)
                    || fieldType.Contains("typedef", StringComparison.Ordinal))
                {
                    continue;
                }

                fields.Add(new ReflectFieldInfo(
                    fieldName,
                    fieldType,
                    $"&{fullTypeName}::{fieldName}",
                    $"&{fullTypeName}::{fieldName}",
                    false));
            }
        }

        var autoPropertyFields = BuildAutoPropertyFields(fullTypeName, methodCandidates);
        var explicitPropertyFields = ResolveExplicitPropertyDirectives(fullTypeName, explicitProperties, methodCandidates, namespacePrefix);
        fields.AddRange(explicitPropertyFields);
        fields.AddRange(autoPropertyFields);

        var overloaded = methodCandidates
            .GroupBy(static method => method.Name, StringComparer.Ordinal)
            .Where(static group => group.Count() > 1)
            .Select(static group => group.Key)
            .ToHashSet(StringComparer.Ordinal);

        methods.AddRange(methodCandidates
            .Where(method => !overloaded.Contains(method.Name))
            .DistinctBy(static method => method.Name)
            .Select(static method => new ReflectMethodInfo(method.Name, method.PointerExpression, method.IsStatic, method.IsPrivate)));

        methods.AddRange(explicitMethods.Select(static method => new ReflectMethodInfo(method.Name, method.PointerExpression, method.IsStatic, method.IsPrivate)));

        return new TextMemberParseResult(fields.DistinctBy(static field => field.Name).ToList(), methods);
    }

    private static HashSet<string> FindNestedTypeNames(string body)
    {
        var nestedTypeNames = new HashSet<string>(StringComparer.Ordinal);
        foreach (Match match in Regex.Matches(
                     body,
                     @"(?:ENUM\s*\([^)]*\)\s*)?enum\s+(?:class\s+)?(?:[A-Za-z_]\w*\s+)*(?<name>[A-Za-z_]\w*)\b|(?:class|struct)\s+(?<name>[A-Za-z_]\w*)\b",
                     RegexOptions.CultureInvariant))
        {
            var name = match.Groups["name"].Value;
            if (!string.IsNullOrWhiteSpace(name))
                nestedTypeNames.Add(name);
        }

        return nestedTypeNames;
    }

    private static string QualifyMemberTypeName(string typeName, string namespacePrefix, string ownerQualifiedName, IReadOnlyCollection<string> nestedTypeNames)
    {
        if (string.IsNullOrWhiteSpace(typeName))
            return typeName;

        var normalizedTypeName = typeName.Trim();
        if (nestedTypeNames.Count == 0)
            return QualifyTypeName(normalizedTypeName, namespacePrefix);

        foreach (var nestedTypeName in nestedTypeNames)
        {
            normalizedTypeName = Regex.Replace(
                normalizedTypeName,
                $@"(?<![\w:]){Regex.Escape(nestedTypeName)}(?![\w:])",
                $"{ownerQualifiedName}::{nestedTypeName}",
                RegexOptions.CultureInvariant);
        }

        return QualifyTypeName(normalizedTypeName, namespacePrefix);
    }

    private static bool TryParseStandalonePropertyDirective(string line, string namespacePrefix, List<ExplicitPropertyDirectiveInfo> directives)
    {
        if (!TryParseMacroInvocation(line, out var macroName, out var macroBody) || macroName != "PROPERTY")
            return false;

        var arguments = ParseNamedArguments(macroBody);
        if (arguments.Count == 0 || arguments.ContainsKey("name") && !arguments.ContainsKey("getter"))
            return false;

        if (!arguments.TryGetValue("getter", out var getterToken) || !arguments.TryGetValue("setter", out var setterToken))
            return true;

        string? propertyName = null;
        if (arguments.TryGetValue("name", out var explicitName))
            propertyName = NormalizeRegistrationName(explicitName);

        var explicitType = arguments.TryGetValue("type", out var typeToken)
            ? QualifyTypeName(NormalizeTypeName(typeToken), namespacePrefix)
            : null;

        directives.Add(new ExplicitPropertyDirectiveInfo(propertyName ?? string.Empty, getterToken.Trim(), setterToken.Trim(), explicitType));
        return true;
    }

    private static bool TryParseStandaloneFunctionDirective(string line, string fullTypeName, List<ExplicitMethodDirectiveInfo> directives)
    {
        if (!TryParseMacroInvocation(line, out var macroName, out var macroBody) || macroName != "FUNCTION")
            return false;

        var arguments = ParseNamedArguments(macroBody);
        if (arguments.Count == 0 || !arguments.ContainsKey("pointer"))
            return false;

        var pointerExpression = arguments["pointer"].Trim();
        var name = arguments.TryGetValue("name", out var explicitName)
            ? NormalizeRegistrationName(explicitName)
            : ExtractMethodNameFromPointerExpression(pointerExpression, fullTypeName);
        if (string.IsNullOrWhiteSpace(name) || string.IsNullOrWhiteSpace(pointerExpression))
            return true;

        directives.Add(new ExplicitMethodDirectiveInfo(name, pointerExpression, false, false));
        return true;
    }

    private static Dictionary<string, string> ParseNamedArguments(string macroBody)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var entry in SplitTopLevel(macroBody, ','))
        {
            var separatorIndex = entry.IndexOf('=', StringComparison.Ordinal);
            if (separatorIndex <= 0)
                continue;

            var key = entry[..separatorIndex].Trim();
            var value = entry[(separatorIndex + 1)..].Trim();
            if (string.IsNullOrWhiteSpace(key) || string.IsNullOrWhiteSpace(value))
                continue;

            result[key] = value;
        }

        return result;
    }

    private static List<ReflectFieldInfo> ResolveExplicitPropertyDirectives(
        string fullTypeName,
        List<ExplicitPropertyDirectiveInfo> directives,
        List<MethodCandidateInfo> methods,
        string namespacePrefix)
    {
        var fields = new List<ReflectFieldInfo>();
        if (directives.Count == 0)
            return fields;

        var methodMap = methods
            .GroupBy(static method => method.Name, StringComparer.Ordinal)
            .ToDictionary(static group => group.Key, static group => group.ToList(), StringComparer.Ordinal);

        foreach (var directive in directives)
        {
            var getter = ResolvePropertyMethodCandidate(fullTypeName, directive.GetterToken, methodMap, true);
            var setter = ResolvePropertyMethodCandidate(fullTypeName, directive.SetterToken, methodMap, false);

            var propertyName = !string.IsNullOrWhiteSpace(directive.Name)
                ? directive.Name
                : BuildPropertyNameFromGetterName(getter.Name);

            var fieldTypeName = !string.IsNullOrWhiteSpace(directive.TypeName)
                ? directive.TypeName!
                : NormalizePropertyTypeName(getter.ReturnTypeName);

            if (string.IsNullOrWhiteSpace(fieldTypeName))
                throw new InvalidOperationException($"Explicit property '{propertyName}' on '{fullTypeName}' could not infer a reflected type. Add 'type = ...' to the PROPERTY directive.");

            if (!ArePropertyTypesCompatible(fieldTypeName, getter.ReturnTypeName))
                throw new InvalidOperationException($"Explicit property '{propertyName}' on '{fullTypeName}' has getter type '{getter.ReturnTypeName}' incompatible with declared field type '{fieldTypeName}'.");

            if (!ArePropertyTypesCompatible(fieldTypeName, setter.ParameterTypeNames[0]))
                throw new InvalidOperationException($"Explicit property '{propertyName}' on '{fullTypeName}' has setter type '{setter.ParameterTypeNames[0]}' incompatible with declared field type '{fieldTypeName}'.");

            fields.Add(new ReflectFieldInfo(propertyName, fieldTypeName, getter.PointerExpression, setter.PointerExpression, getter.IsPrivate || setter.IsPrivate));
        }

        return fields;
    }

    private static MethodCandidateInfo ResolvePropertyMethodCandidate(
        string fullTypeName,
        string token,
        Dictionary<string, List<MethodCandidateInfo>> methodMap,
        bool getter)
    {
        var methodName = ExtractMethodNameToken(token);
        if (string.IsNullOrWhiteSpace(methodName))
            throw new InvalidOperationException($"Failed to resolve property {(getter ? "getter" : "setter")} '{token}' on '{fullTypeName}'.");

        if (!methodMap.TryGetValue(methodName, out var candidates) || candidates.Count == 0)
            throw new InvalidOperationException($"Explicit property on '{fullTypeName}' references {(getter ? "getter" : "setter")} '{methodName}' but no FUNCTION() method with that name was discovered.");

        if (candidates.Count > 1)
            throw new InvalidOperationException($"Explicit property on '{fullTypeName}' references overloaded {(getter ? "getter" : "setter")} '{methodName}'. Use a non-overloaded adapter method.");

        return candidates[0];
    }

    private static string ExtractMethodNameToken(string token)
    {
        var trimmed = token.Trim();
        if (trimmed.Contains("::", StringComparison.Ordinal))
            return trimmed[(trimmed.LastIndexOf("::", StringComparison.Ordinal) + 2)..];
        return trimmed;
    }

    private static string ExtractMethodNameFromPointerExpression(string pointerExpression, string fullTypeName)
    {
        var trimmed = pointerExpression.Trim();
        var methodName = Regex.Match(trimmed, @"&(?:(?:[A-Za-z_]\w*::)+)?(?<name>[A-Za-z_]\w*)\s*\)?$", RegexOptions.CultureInvariant);
        if (methodName.Success)
            return methodName.Groups["name"].Value;

        if (trimmed.Contains($"&{fullTypeName}::", StringComparison.Ordinal))
            return trimmed[(trimmed.LastIndexOf("::", StringComparison.Ordinal) + 2)..].TrimEnd(')', ' ');

        return string.Empty;
    }

    private static List<ReflectFieldInfo> BuildAutoPropertyFields(string fullTypeName, List<MethodCandidateInfo> methods)
    {
        var getters = methods
            .Where(IsGetterCandidate)
            .GroupBy(GetPropertySuffix, StringComparer.Ordinal)
            .ToDictionary(static group => group.Key, static group => group.First(), StringComparer.Ordinal);

        var setters = methods
            .Where(IsSetterCandidate)
            .GroupBy(GetPropertySuffix, StringComparer.Ordinal)
            .ToDictionary(static group => group.Key, static group => group.First(), StringComparer.Ordinal);

        var fields = new List<ReflectFieldInfo>();
        foreach (var (suffix, getter) in getters)
        {
            setters.TryGetValue(suffix, out var setter);
            if (setter is null)
                continue;

            if (!ArePropertyTypesCompatible(getter.ReturnTypeName, setter.ParameterTypeNames[0]))
            {
                throw new InvalidOperationException(
                    $"Auto property '{BuildPropertyNameFromSuffix(suffix)}' on '{fullTypeName}' has incompatible getter/setter types. " +
                    $"Getter returns '{getter.ReturnTypeName}', setter accepts '{setter.ParameterTypeNames[0]}'.");
            }

            var propertyName = BuildPropertyNameFromSuffix(suffix);
            var fieldTypeName = NormalizePropertyTypeName(getter.ReturnTypeName);
            if (string.IsNullOrWhiteSpace(propertyName) || string.IsNullOrWhiteSpace(fieldTypeName))
                continue;

            fields.Add(new ReflectFieldInfo(propertyName, fieldTypeName, getter.PointerExpression, setter.PointerExpression, getter.IsPrivate || setter.IsPrivate));
        }

        return fields;
    }

    private static bool IsGetterCandidate(MethodCandidateInfo method)
        => GetGetterPrefixLength(method.Name) > 0
           && method.ParameterTypeNames.Count == 0
           && !string.IsNullOrWhiteSpace(method.ReturnTypeName)
           && !string.Equals(method.ReturnTypeName, "void", StringComparison.Ordinal);

    private static bool IsSetterCandidate(MethodCandidateInfo method)
        => method.Name.StartsWith("Set", StringComparison.Ordinal)
           && method.Name.Length > 3
           && method.ParameterTypeNames.Count == 1;

    private static string GetPropertySuffix(MethodCandidateInfo method)
    {
        if (IsSetterCandidate(method))
            return method.Name["Set".Length..];

        var getterPrefixLength = GetGetterPrefixLength(method.Name);
        return getterPrefixLength > 0 ? method.Name[getterPrefixLength..] : method.Name;
    }

    private static int GetGetterPrefixLength(string methodName)
    {
        if (methodName.StartsWith("Get", StringComparison.Ordinal) && methodName.Length > 3)
            return 3;
        if (methodName.StartsWith("Has", StringComparison.Ordinal) && methodName.Length > 3)
            return 3;
        if (methodName.StartsWith("Is", StringComparison.Ordinal) && methodName.Length > 2)
            return 2;

        return 0;
    }

    private static bool ArePropertyTypesCompatible(string getterType, string setterType)
        => string.Equals(NormalizePropertyTypeName(getterType), NormalizePropertyTypeName(setterType), StringComparison.Ordinal);

    private static string ExtractReturnTypeFromText(
        string statement,
        string methodName,
        string namespacePrefix,
        string ownerQualifiedName,
        IReadOnlyCollection<string> nestedTypeNames)
    {
        var methodIndex = statement.IndexOf(methodName, StringComparison.Ordinal);
        if (methodIndex <= 0)
            return string.Empty;

        var prefix = statement[..methodIndex].Trim();
        prefix = Regex.Replace(prefix, @"\b(virtual|static|inline|constexpr|explicit|friend)\b", string.Empty, RegexOptions.CultureInvariant).Trim();
        return QualifyMemberTypeName(NormalizeTypeName(prefix), namespacePrefix, ownerQualifiedName, nestedTypeNames);
    }

    private static List<string> ExtractParameterTypeNamesFromText(
        string statement,
        string methodName,
        string namespacePrefix,
        string ownerQualifiedName,
        IReadOnlyCollection<string> nestedTypeNames)
    {
        var signatureMatch = Regex.Match(statement, $@"\b{Regex.Escape(methodName)}\s*\((?<params>[^)]*)\)", RegexOptions.CultureInvariant);
        if (!signatureMatch.Success)
            return [];

        var paramsText = signatureMatch.Groups["params"].Value.Trim();
        if (string.IsNullOrWhiteSpace(paramsText) || string.Equals(paramsText, "void", StringComparison.Ordinal))
            return [];

        return SplitTopLevel(paramsText, ',')
            .Select(parameter => ExtractParameterTypeFromText(parameter, namespacePrefix, ownerQualifiedName, nestedTypeNames))
            .Where(static typeName => !string.IsNullOrWhiteSpace(typeName))
            .ToList();
    }

    private static string ExtractParameterTypeFromText(
        string parameter,
        string namespacePrefix,
        string ownerQualifiedName,
        IReadOnlyCollection<string> nestedTypeNames)
    {
        var trimmed = parameter.Trim();
        trimmed = Regex.Replace(trimmed, @"\s*=\s*.+$", string.Empty, RegexOptions.CultureInvariant).Trim();
        trimmed = Regex.Replace(trimmed, @"\b(register|constexpr|inline)\b", string.Empty, RegexOptions.CultureInvariant).Trim();

        if (string.IsNullOrWhiteSpace(trimmed))
            return string.Empty;

        var match = Regex.Match(trimmed, @"^(?<type>.+?)(?:\s+[A-Za-z_]\w*)?$", RegexOptions.CultureInvariant);
        if (!match.Success)
            return string.Empty;

        return QualifyMemberTypeName(NormalizeTypeName(match.Groups["type"].Value), namespacePrefix, ownerQualifiedName, nestedTypeNames);
    }
}

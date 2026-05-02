using System.Text.RegularExpressions;

internal static partial class MetaParserTool
{
    private static IEnumerable<ReflectTypeInfo> ParseHeaderFromText(string rootDir, string headerPath, string headerText, bool emitDiagnostics = false)
    {
        var includePath = ToGeneratedIncludePath(rootDir, Path.GetFullPath(headerPath));
        var text = SanitizeTextForMacroParsing(headerText);
        var classMatches = Regex.Matches(
            text,
            @"\b(?<kind>class|struct)\s+(?:[A-Za-z_]\w*\s+)*(?<name>[A-Za-z_]\w*)\s*(?:\:\s*(?<bases>[^{]+))?\s*\{",
            RegexOptions.CultureInvariant);

        foreach (Match match in classMatches.Cast<Match>())
        {
            var kind = match.Groups["kind"].Value;
            var className = match.Groups["name"].Value;
            if (string.IsNullOrWhiteSpace(className))
                continue;

            var openBraceIndex = text.IndexOf('{', match.Index);
            if (openBraceIndex < 0)
                continue;

            var body = ExtractBraceBody(text, openBraceIndex);
            if (!ContainsGeneratedBody(body))
                continue;

            var namespacePrefix = ExtractNamespaceFromText(text[..match.Index]);
            var fullTypeName = string.IsNullOrWhiteSpace(namespacePrefix)
                ? className
                : $"{namespacePrefix}::{className}";
            var bases = ParseBasesFromText(match.Groups["bases"].Value, namespacePrefix);
            var members = ExtractMembersFromText(body, fullTypeName, string.Equals(kind, "struct", StringComparison.Ordinal));
            var typeMetas = ExtractTypeMetasFromDeclarationText(text, match.Index);
            if (emitDiagnostics)
                Console.WriteLine($"[MetaParser] Members {fullTypeName} {FormatMemberDiscoverySummary(members.Summary)}");

            yield return new ReflectTypeInfo(
                className,
                namespacePrefix,
                fullTypeName,
                includePath,
                Path.GetFullPath(headerPath),
                bases,
                members.Fields,
                members.Methods,
                typeMetas);

            foreach (var nestedEnum in ParseNestedEnumsFromText(body, fullTypeName, includePath, Path.GetFullPath(headerPath)))
                yield return nestedEnum;
        }
    }

    private static IEnumerable<ReflectTypeInfo> ParseExternalReflectionDeclarations(string rootDir, string headerPath)
    {
        var text = SanitizeTextForMacroParsing(File.ReadAllText(headerPath));
        if (!text.Contains("MetaExternal", StringComparison.Ordinal)
            && !text.Contains("REFLECT_EXTERNAL", StringComparison.Ordinal))
        {
            return [];
        }

        var includePath = ToGeneratedIncludePath(rootDir, Path.GetFullPath(headerPath));
        var declarations = new List<ReflectTypeInfo>();

        foreach (Match match in Regex.Matches(text, @"\bMetaExternal\s*\(\s*(?<type>[^)]+?)\s*\)", RegexOptions.CultureInvariant).Cast<Match>())
        {
            var fullTypeName = NormalizeTypeName(match.Groups["type"].Value);
            if (string.IsNullOrWhiteSpace(fullTypeName))
                continue;

            declarations.Add(CreateExternalReflectType(fullTypeName, includePath, Path.GetFullPath(headerPath), [], [], []));
        }

        foreach (var invocation in ExtractMacroInvocations(text, "REFLECT_EXTERNAL"))
        {
            var args = SplitTopLevel(invocation, ',');
            if (args.Count == 0)
                continue;

            var fullTypeName = NormalizeTypeName(args[0]);
            if (string.IsNullOrWhiteSpace(fullTypeName))
                continue;

            var namespacePrefix = ExtractNamespace(fullTypeName, ExtractSimpleClassName(fullTypeName));
            var bases = new List<ReflectBaseInfo>();
            var fields = new List<ReflectFieldInfo>();
            var methods = new List<ReflectMethodInfo>();

            foreach (var section in args.Skip(1))
            {
                if (!TryParseMacroInvocation(section, out var sectionName, out var sectionBody))
                    continue;

                switch (sectionName)
                {
                case "Bases":
                    bases.AddRange(ParseExternalBases(sectionBody, namespacePrefix));
                    break;
                case "Fields":
                    fields.AddRange(ParseExternalFields(sectionBody, fullTypeName, namespacePrefix));
                    break;
                case "Methods":
                    methods.AddRange(ParseExternalMethods(sectionBody, fullTypeName));
                    break;
                case "StaticMethods":
                    methods.AddRange(ParseExternalStaticMethods(sectionBody, fullTypeName));
                    break;
                default:
                    Console.Error.WriteLine($"[MetaParser] Warning: unknown external reflection section '{sectionName}' in {headerPath}");
                    break;
                }
            }

            declarations.Add(CreateExternalReflectType(fullTypeName, includePath, Path.GetFullPath(headerPath), bases, fields, methods));
        }

        return declarations;
    }

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
                HeaderPath = type.HeaderPath.Contains("ExternalReflection", StringComparison.Ordinal)
                    ? type.HeaderPath
                    : existing.HeaderPath,
                SourceFilePath = type.SourceFilePath.Contains("ExternalReflection", StringComparison.Ordinal)
                    ? type.SourceFilePath
                    : existing.SourceFilePath,
                Bases = existing.Bases.Concat(type.Bases).DistinctBy(static baseInfo => baseInfo.TypeName).ToList(),
                Fields = (type.SourceFilePath.Contains("ExternalReflection", StringComparison.Ordinal)
                        ? type.Fields.Concat(existing.Fields)
                        : existing.Fields.Concat(type.Fields))
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

    private static ReflectTypeInfo CreateExternalReflectType(
        string fullTypeName,
        string includePath,
        string sourceFilePath,
        List<ReflectBaseInfo> bases,
        List<ReflectFieldInfo> fields,
        List<ReflectMethodInfo> methods)
    {
        var className = ExtractSimpleClassName(fullTypeName);
        return new ReflectTypeInfo(
            className,
            ExtractNamespace(fullTypeName, className),
            fullTypeName,
            includePath,
            sourceFilePath,
            bases.DistinctBy(static baseInfo => baseInfo.TypeName).ToList(),
            fields.DistinctBy(static field => field.Name).ToList(),
            methods.DistinctBy(static method => $"{method.IsStatic}:{method.Name}:{method.PointerExpression}").ToList(),
            []);
    }

    private static List<ReflectTypeMetaInfo> ExtractTypeMetasFromDeclarationText(string text, int classKeywordIndex)
    {
        var lineStart = text.LastIndexOf('\n', Math.Max(0, classKeywordIndex - 1));
        lineStart = lineStart < 0 ? 0 : lineStart + 1;
        var declarationPrefix = text[lineStart..classKeywordIndex];

        var macroMatch = Regex.Match(
            declarationPrefix,
            @"\b(?:CLASS|STRUCT)\s*\(",
            RegexOptions.CultureInvariant | RegexOptions.RightToLeft);

        if (!macroMatch.Success)
            return [];

        var openParenIndex = declarationPrefix.IndexOf('(', macroMatch.Index);
        if (openParenIndex < 0)
            return [];

        var macroBody = ExtractDelimitedBody(declarationPrefix, openParenIndex, '(', ')');
        if (string.IsNullOrWhiteSpace(macroBody))
            return [];

        var metas = new List<ReflectTypeMetaInfo>();
        foreach (var token in SplitTopLevel(macroBody, ','))
        {
            if (!TryParseMacroInvocation(token, out var macroName, out var macroArgs))
                continue;

            if (string.Equals(macroName, "ComponentMenu", StringComparison.Ordinal))
                metas.Add(new ReflectTypeMetaInfo("NLS::meta::ComponentMenu", macroArgs.Trim()));
        }

        return metas;
    }

    private static List<ReflectBaseInfo> ParseExternalBases(string sectionBody, string namespacePrefix)
        => SplitTopLevel(sectionBody, ',')
            .Select(NormalizeTypeName)
            .Select(typeName => QualifyTypeName(typeName, namespacePrefix))
            .Where(static typeName => !string.IsNullOrWhiteSpace(typeName))
            .Distinct(StringComparer.Ordinal)
            .Select(static typeName => new ReflectBaseInfo(typeName))
            .ToList();

    private static List<ReflectFieldInfo> ParseExternalFields(string sectionBody, string fullTypeName, string namespacePrefix)
    {
        var fields = new List<ReflectFieldInfo>();

        foreach (var entry in SplitTopLevel(sectionBody, ','))
        {
            if (!TryParseMacroInvocation(entry, out var macroName, out var macroBody)
                || (macroName != "REFLECT_FIELD" && macroName != "REFLECT_PRIVATE_FIELD" && macroName != "REFLECT_PROPERTY"))
                continue;

            var args = SplitTopLevel(macroBody, ',');
            if (macroName == "REFLECT_PROPERTY")
            {
                if (args.Count != 4)
                    continue;

                var typeName = QualifyTypeName(NormalizeTypeName(args[0]), namespacePrefix);
                var fieldName = NormalizeRegistrationName(args[1]);
                var getterExpression = args[2].Trim();
                var setterExpression = args[3].Trim();
                if (string.IsNullOrWhiteSpace(typeName) || string.IsNullOrWhiteSpace(fieldName) || string.IsNullOrWhiteSpace(getterExpression) || string.IsNullOrWhiteSpace(setterExpression))
                    continue;

                fields.Add(new ReflectFieldInfo(fieldName, typeName, getterExpression, setterExpression, false));
                continue;
            }

            if (args.Count != 2)
                continue;

            var directTypeName = QualifyTypeName(NormalizeTypeName(args[0]), namespacePrefix);
            var directFieldName = NormalizeRegistrationName(args[1]);
            if (string.IsNullOrWhiteSpace(directTypeName) || string.IsNullOrWhiteSpace(directFieldName))
                continue;

            fields.Add(new ReflectFieldInfo(
                directFieldName,
                directTypeName,
                $"&{fullTypeName}::{directFieldName}",
                $"&{fullTypeName}::{directFieldName}",
                macroName == "REFLECT_PRIVATE_FIELD"));
        }

        return fields.DistinctBy(static field => field.Name).ToList();
    }

    private static List<ReflectMethodInfo> ParseExternalMethods(string sectionBody, string fullTypeName)
    {
        var methods = new List<ReflectMethodInfo>();

        foreach (var entry in SplitTopLevel(sectionBody, ','))
        {
            if (!TryParseMacroInvocation(entry, out var macroName, out var macroBody))
                continue;

            switch (macroName)
            {
            case "REFLECT_METHOD":
            case "REFLECT_PRIVATE_METHOD":
            {
                var methodName = NormalizeRegistrationName(macroBody);
                if (string.IsNullOrWhiteSpace(methodName))
                    continue;

                methods.Add(new ReflectMethodInfo(methodName, $"&{fullTypeName}::{methodName}", false, macroName == "REFLECT_PRIVATE_METHOD"));
                break;
            }
            case "REFLECT_METHOD_EX":
            case "REFLECT_PRIVATE_METHOD_EX":
            {
                var args = SplitTopLevel(macroBody, ',');
                if (args.Count != 2)
                    continue;

                var methodName = NormalizeRegistrationName(args[0]);
                var pointerExpression = args[1].Trim();
                if (string.IsNullOrWhiteSpace(methodName) || string.IsNullOrWhiteSpace(pointerExpression))
                    continue;

                methods.Add(new ReflectMethodInfo(methodName, pointerExpression, false, macroName == "REFLECT_PRIVATE_METHOD_EX"));
                break;
            }
            }
        }

        return methods.DistinctBy(static method => $"{method.IsStatic}:{method.Name}:{method.PointerExpression}").ToList();
    }

    private static List<ReflectMethodInfo> ParseExternalStaticMethods(string sectionBody, string fullTypeName)
    {
        var methods = new List<ReflectMethodInfo>();

        foreach (var entry in SplitTopLevel(sectionBody, ','))
        {
            if (!TryParseMacroInvocation(entry, out var macroName, out var macroBody)
                || (macroName != "REFLECT_STATIC_METHOD" && macroName != "REFLECT_PRIVATE_STATIC_METHOD"))
                continue;

            var args = SplitTopLevel(macroBody, ',');
            if (args.Count != 2)
                continue;

            var methodName = NormalizeRegistrationName(args[0]);
            var pointerExpression = args[1].Trim();
            if (string.IsNullOrWhiteSpace(methodName) || string.IsNullOrWhiteSpace(pointerExpression))
                continue;

            methods.Add(new ReflectMethodInfo(methodName, pointerExpression, true, macroName == "REFLECT_PRIVATE_STATIC_METHOD"));
        }

        return methods.DistinctBy(static method => $"{method.IsStatic}:{method.Name}:{method.PointerExpression}").ToList();
    }
}

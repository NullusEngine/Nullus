using System.Text.RegularExpressions;

internal static partial class MetaParserTool
{
    private static void ValidateReflectTypes(IReadOnlyList<ReflectTypeInfo> types, ReflectionTypeCatalog catalog)
    {
        foreach (var type in types)
        {
            foreach (var field in type.Fields)
                ValidateFieldType(type, field, catalog.SupportedTypeNames);
        }
    }

    private static ReflectionTypeCatalog BuildReflectionTypeCatalog(string rootDir, PrecompileParams config, IReadOnlyList<ReflectTypeInfo> types)
    {
        var catalog = new ReflectionTypeCatalog();
        var reflectedTypes = new List<ReflectTypeInfo>();

        foreach (var builtinTypeName in BuiltinTypeNames)
            catalog.SupportedTypeNames.Add(builtinTypeName);

        catalog.SupportedTypeNames.Add("std::string");
        AddSupportedReflectionType(catalog, "NLS::Object");
        AddSupportedReflectionType(catalog, "NLS::NamedObject");
        catalog.ReflectedDirectBaseTypeNames["NLS::NamedObject"] = ["NLS::Object"];

        foreach (var pptrTargetTypeName in BuildSupportedPPtrObjectTargetTypeNames(rootDir))
            AddSupportedReflectionType(catalog, pptrTargetTypeName);

        foreach (var type in types)
        {
            AddSupportedReflectionType(catalog, type.QualifiedName);
            reflectedTypes.Add(type);
        }

        var runtimeRoot = Path.Combine(rootDir, "Runtime");
        if (Directory.Exists(runtimeRoot))
        {
            foreach (var headerPath in Directory.EnumerateFiles(runtimeRoot, "*.*", SearchOption.AllDirectories)
                         .Where(path => string.Equals(Path.GetExtension(path), ".h", StringComparison.OrdinalIgnoreCase)
                                     || string.Equals(Path.GetExtension(path), ".hpp", StringComparison.OrdinalIgnoreCase)))
            {
                var headerText = File.ReadAllText(headerPath);
                if (!ContainsReflectedDeclaration(headerText))
                    continue;

                foreach (var externalTypeName in ExtractExternalReflectionTypeNames(headerText))
                    AddSupportedReflectionType(catalog, externalTypeName);

                foreach (var reflectedType in ParseHeader(rootDir, headerPath, config))
                {
                    AddSupportedReflectionType(catalog, reflectedType.QualifiedName);
                    reflectedTypes.Add(reflectedType);
                }
            }
        }

        foreach (var reflectedType in MergeReflectTypes(reflectedTypes))
            catalog.ReflectedDirectBaseTypeNames[reflectedType.QualifiedName] = BuildReflectedBaseTypeNames(reflectedType, catalog);

        return catalog;
    }

    private static HashSet<string> BuildSupportedPPtrObjectTargetTypeNames(string rootDir)
    {
        var supportedTypeNames = new HashSet<string>(StringComparer.Ordinal);
        var pptrResourceTypesPath = Path.Combine(rootDir, "Runtime", "Engine", "Serialize", "PPtrResourceTypes.h");
        if (!File.Exists(pptrResourceTypesPath))
            return supportedTypeNames;

        var headerText = StripCommentsAndDisabledPreprocessorBlocks(File.ReadAllText(pptrResourceTypesPath));
        var targetsMacro = Regex.Match(
            headerText,
            @"#define\s+NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS\s*\(\s*APPLY\s*\)(?<body>(?:\s*\\\s*\r?\n\s*APPLY\s*\((?:\\\s*\r?\n|[^\r\n)])*\))+)",
            RegexOptions.Multiline);
        if (!targetsMacro.Success)
            throw new InvalidOperationException(
                $"Failed to parse NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS from '{pptrResourceTypesPath}'.");

        foreach (Match target in Regex.Matches(targetsMacro.Groups["body"].Value, @"APPLY\s*\(\s*(?<type>[^,\)]+)\s*,"))
            AddSupportedReflectionType(supportedTypeNames, target.Groups["type"].Value.Trim());

        if (supportedTypeNames.Count == 0)
        {
            throw new InvalidOperationException(
                $"NLS_ENGINE_SERIALIZE_PPTR_RESOURCE_TARGETS in '{pptrResourceTypesPath}' did not contain any APPLY target entries.");
        }

        return supportedTypeNames;
    }

    private static void AddSupportedReflectionType(HashSet<string> supportedTypeNames, string qualifiedName)
    {
        supportedTypeNames.Add(qualifiedName);
        if (qualifiedName.StartsWith("NLS::", StringComparison.Ordinal))
            supportedTypeNames.Add(qualifiedName["NLS::".Length..]);
    }

    private static void AddSupportedReflectionType(ReflectionTypeCatalog catalog, string qualifiedName)
    {
        AddSupportedReflectionType(catalog.SupportedTypeNames, qualifiedName);
        AddReflectedTypeName(catalog.ReflectedBaseTypeNames, qualifiedName);
    }

    private static void ValidateFieldType(ReflectTypeInfo ownerType, ReflectFieldInfo field, HashSet<string> supportedTypeNames)
    {
        if (IsSupportedReflectionFieldType(field.TypeName, supportedTypeNames, out var reason))
            return;

        throw new InvalidOperationException(
            $"Unsupported reflected field type '{field.TypeName}' for '{ownerType.QualifiedName}.{field.Name}'. {reason}");
    }

    private static bool IsSupportedReflectionFieldType(string typeName, HashSet<string> supportedTypeNames, out string reason)
    {
        var normalizedTypeName = NormalizePropertyTypeName(typeName);
        if (string.IsNullOrWhiteSpace(normalizedTypeName))
        {
            reason = "Type name is empty after normalization.";
            return false;
        }

        if (ContainsUnsupportedReflectionType(normalizedTypeName))
        {
            reason = "This type category is not supported by the reflection system.";
            return false;
        }

        if (normalizedTypeName.Contains('*', StringComparison.Ordinal))
        {
            reason = "Pointer properties must stay in explicit external bindings or be adapted through a supported reflected value type.";
            return false;
        }

        if (TryGetArrayElementType(normalizedTypeName, out var elementTypeName))
        {
            if (IsSupportedReflectionFieldType(elementTypeName, supportedTypeNames, out reason))
                return true;

            reason = $"Array element type '{elementTypeName}' is not reflectable. {reason}";
            return false;
        }

        if (TryGetPPtrElementType(normalizedTypeName, out var pptrElementTypeName))
        {
            if (IsSupportedPPtrObjectTargetType(pptrElementTypeName, supportedTypeNames))
            {
                reason = string.Empty;
                return true;
            }

            reason = $"PPtr object references require a supported Unity Object target type; '{pptrElementTypeName}' is not in the registered PPtr target set.";
            return false;
        }

        if (supportedTypeNames.Contains(normalizedTypeName))
        {
            reason = string.Empty;
            return true;
        }

        reason = "The exact type is not registered for reflection. Add reflection support for this type instead of adapting it implicitly.";
        return false;
    }

    private static bool TryGetArrayElementType(string typeName, out string elementTypeName)
    {
        if (!typeName.EndsWith('>'))
        {
            elementTypeName = string.Empty;
            return false;
        }

        string prefix;
        if (typeName.StartsWith("NLS::Array<", StringComparison.Ordinal))
            prefix = "NLS::Array<";
        else if (typeName.StartsWith("std::vector<", StringComparison.Ordinal))
            prefix = "std::vector<";
        else if (typeName.StartsWith("Array<", StringComparison.Ordinal))
            prefix = "Array<";
        else if (typeName.StartsWith("vector<", StringComparison.Ordinal))
            prefix = "vector<";
        else
        {
            elementTypeName = string.Empty;
            return false;
        }

        elementTypeName = typeName[prefix.Length..^1].Trim();
        return !string.IsNullOrWhiteSpace(elementTypeName);
    }

    private static bool TryGetPPtrElementType(string typeName, out string elementTypeName)
    {
        const string PPtrPrefix = "NLS::Engine::Serialize::PPtr<";
        if (!typeName.StartsWith(PPtrPrefix, StringComparison.Ordinal) || !typeName.EndsWith('>'))
        {
            elementTypeName = string.Empty;
            return false;
        }

        elementTypeName = typeName[PPtrPrefix.Length..^1].Trim();
        return !string.IsNullOrWhiteSpace(elementTypeName);
    }

    private static bool IsSupportedPPtrObjectTargetType(string typeName, HashSet<string> supportedTypeNames)
    {
        var normalizedTypeName = NormalizePropertyTypeName(typeName);
        return supportedTypeNames.Contains(normalizedTypeName);
    }
}

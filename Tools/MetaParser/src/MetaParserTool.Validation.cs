internal static partial class MetaParserTool
{
    private static void ValidateReflectTypes(string rootDir, IReadOnlyList<ReflectTypeInfo> types)
    {
        var supportedTypeNames = BuildSupportedReflectionTypeNames(rootDir, types);
        foreach (var type in types)
        {
            foreach (var field in type.Fields)
                ValidateFieldType(type, field, supportedTypeNames);
        }
    }

    private static HashSet<string> BuildSupportedReflectionTypeNames(string rootDir, IReadOnlyList<ReflectTypeInfo> types)
    {
        var supportedTypeNames = new HashSet<string>(BuiltinTypeNames, StringComparer.Ordinal)
        {
            "std::string"
        };

        foreach (var type in types)
        {
            supportedTypeNames.Add(type.QualifiedName);
            if (type.QualifiedName.StartsWith("NLS::", StringComparison.Ordinal))
                supportedTypeNames.Add(type.QualifiedName["NLS::".Length..]);
        }

        var runtimeRoot = Path.Combine(rootDir, "Runtime");
        if (!Directory.Exists(runtimeRoot))
            return supportedTypeNames;

        foreach (var headerPath in Directory.EnumerateFiles(runtimeRoot, "*.*", SearchOption.AllDirectories)
                     .Where(path => string.Equals(Path.GetExtension(path), ".h", StringComparison.OrdinalIgnoreCase)
                                 || string.Equals(Path.GetExtension(path), ".hpp", StringComparison.OrdinalIgnoreCase)))
        {
            var headerText = File.ReadAllText(headerPath);
            foreach (var reflectedType in ParseTopLevelEnumsFromText(rootDir, headerPath, headerText))
            {
                supportedTypeNames.Add(reflectedType.QualifiedName);
                if (reflectedType.QualifiedName.StartsWith("NLS::", StringComparison.Ordinal))
                    supportedTypeNames.Add(reflectedType.QualifiedName["NLS::".Length..]);
            }

            if (ContainsGeneratedBody(headerText))
            {
                foreach (var reflectedType in ParseHeaderFromText(rootDir, headerPath, headerText))
                {
                    supportedTypeNames.Add(reflectedType.QualifiedName);
                    if (reflectedType.QualifiedName.StartsWith("NLS::", StringComparison.Ordinal))
                        supportedTypeNames.Add(reflectedType.QualifiedName["NLS::".Length..]);
                }
            }

            foreach (var reflectedType in ParseExternalReflectionDeclarations(rootDir, headerPath))
            {
                supportedTypeNames.Add(reflectedType.QualifiedName);
                if (reflectedType.QualifiedName.StartsWith("NLS::", StringComparison.Ordinal))
                    supportedTypeNames.Add(reflectedType.QualifiedName["NLS::".Length..]);
            }
        }

        return supportedTypeNames;
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

        if (normalizedTypeName.StartsWith("Array<", StringComparison.Ordinal))
        {
            reason = "Use the fully qualified `NLS::Array<...>` type in reflected declarations so MetaParser can validate and register the container correctly.";
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
        const string ArrayPrefix = "NLS::Array<";
        if (!typeName.StartsWith(ArrayPrefix, StringComparison.Ordinal) || !typeName.EndsWith('>'))
        {
            elementTypeName = string.Empty;
            return false;
        }

        elementTypeName = typeName[ArrayPrefix.Length..^1].Trim();
        return !string.IsNullOrWhiteSpace(elementTypeName);
    }
}

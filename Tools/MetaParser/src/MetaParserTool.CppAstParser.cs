using CppAst;

internal static partial class MetaParserTool
{
    private static IEnumerable<ReflectTypeInfo> ParseHeaderWithCppAst(string rootDir, string headerPath, PrecompileParams config)
    {
        var options = CreateOptions(config);
        options.AutoSquashTypedef = false;
        var compilation = CppParser.ParseFile(headerPath, options);
        if (compilation.HasErrors)
        {
            var errors = string.Join(Environment.NewLine, compilation.Diagnostics.Messages);
            throw new InvalidOperationException(errors);
        }

        var normalizedHeader = Path.GetFullPath(headerPath);
        var visibleTypes = BuildVisibleTypeLookup(compilation);
        foreach (var cls in EnumerateAllClasses(compilation))
        {
            var isSameHeader = IsElementFromHeader(cls, normalizedHeader) || HasAttributeFromHeader(cls, normalizedHeader, "Reflection");
            if (!cls.IsDefinition || !isSameHeader || !HasReflectionMarker(cls))
                continue;

            var fullTypeName = string.IsNullOrWhiteSpace(cls.FullName)
                ? (string.IsNullOrWhiteSpace(cls.Name) ? string.Empty : cls.Name)
                : cls.FullName;
            if (string.IsNullOrWhiteSpace(fullTypeName))
                continue;

            List<ReflectBaseInfo> bases;
            List<ReflectFieldInfo> fields;
            List<ReflectMethodInfo> methods;

            try
            {
                bases = ExtractBases(cls);
                fields = ExtractFields(cls, visibleTypes);
                methods = ExtractMethods(cls, visibleTypes);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[MetaParser] Warning: partial reflection fallback for {fullTypeName}: {ex.Message}");
                bases = TryExtract(() => ExtractBases(cls));
                fields = TryExtract(() => ExtractFields(cls, visibleTypes));
                methods = TryExtract(() => ExtractMethods(cls, visibleTypes));
            }

            yield return new ReflectTypeInfo(
                cls.Name,
                ExtractNamespace(fullTypeName, cls.Name),
                fullTypeName,
                ToGeneratedIncludePath(rootDir, normalizedHeader),
                normalizedHeader,
                bases,
                fields,
                methods,
                ExtractTypeMetas(cls),
                FindGeneratedBodyLineForType(normalizedHeader, cls.Name, cls.Span.Start.Offset, cls.Span.End.Offset));
        }

        foreach (var cppEnum in EnumerateAllEnums(compilation))
        {
            var isSameHeader = IsElementFromHeader(cppEnum, normalizedHeader) || HasAttributeFromHeader(cppEnum, normalizedHeader, "Reflection");
            if (!isSameHeader || !HasReflectionMarker(cppEnum))
                continue;

            var fullTypeName = string.IsNullOrWhiteSpace(cppEnum.FullName)
                ? (string.IsNullOrWhiteSpace(cppEnum.Name) ? string.Empty : cppEnum.Name)
                : cppEnum.FullName;
            if (string.IsNullOrWhiteSpace(fullTypeName))
                continue;

            yield return new ReflectTypeInfo(
                cppEnum.Name,
                ExtractNamespace(fullTypeName, cppEnum.Name),
                fullTypeName,
                ToGeneratedIncludePath(rootDir, normalizedHeader),
                normalizedHeader,
                [],
                [],
                [],
                [],
                null,
                true,
                cppEnum.Items.Select(static item => new ReflectEnumValueInfo(item.Name)).ToList());
        }
    }

    private static CppParserOptions CreateOptions(PrecompileParams config)
    {
        var options = new CppParserOptions
        {
            ParseTokenAttributes = false,
            ParseSystemIncludes = false
        };
        ConfigurePlatformDefaults(options, config);

        foreach (var includeDir in config.IncludeDirs.Where(static value => !string.IsNullOrWhiteSpace(value)).Distinct())
            options.IncludeFolders.Add(Path.GetFullPath(includeDir));

        var normalizedSystemIncludes = config.SystemIncludeDirs.Where(static value => !string.IsNullOrWhiteSpace(value)).Select(Path.GetFullPath).Distinct().ToList();
        if (!OperatingSystem.IsLinux())
        {
            foreach (var systemIncludeDir in normalizedSystemIncludes)
                options.SystemIncludeFolders.Add(systemIncludeDir);
        }

        foreach (var define in config.Defines.Where(static value => !string.IsNullOrWhiteSpace(value)).Distinct())
            options.Defines.Add(define);

        var compilerArgs = new List<string>();
        if (!string.IsNullOrWhiteSpace(config.CompilerTarget))
            compilerArgs.Add($"--target={config.CompilerTarget}");

        if (!string.IsNullOrWhiteSpace(config.ResourceDir))
        {
            compilerArgs.Add("-resource-dir");
            compilerArgs.Add(config.ResourceDir);
        }

        if (!string.IsNullOrWhiteSpace(config.Sysroot))
        {
            if (OperatingSystem.IsMacOS())
            {
                compilerArgs.Add("-isysroot");
                compilerArgs.Add(config.Sysroot);
            }
            else
            {
                compilerArgs.Add("--sysroot");
                compilerArgs.Add(config.Sysroot);
            }
        }

        if (!OperatingSystem.IsLinux())
        {
            foreach (var systemIncludeDir in normalizedSystemIncludes)
            {
                compilerArgs.Add("-isystem");
                compilerArgs.Add(systemIncludeDir);
            }
        }

        compilerArgs.AddRange(config.CompilerOptions.Where(static value => !string.IsNullOrWhiteSpace(value)));
        foreach (var arg in compilerArgs)
            options.AdditionalArguments.Add(arg);

        return options;
    }

    private static void ConfigurePlatformDefaults(CppParserOptions options, PrecompileParams config)
    {
        if (OperatingSystem.IsWindows())
        {
            options.ConfigureForWindowsMsvc(CppTargetCpu.X86_64);
            return;
        }

        options.TargetCpu = CppTargetCpu.X86_64;
        options.TargetVendor = "pc";
        options.TargetSystem = OperatingSystem.IsMacOS() ? "darwin" : "linux";
        options.TargetAbi = OperatingSystem.IsMacOS() ? string.Empty : "gnu";

        if (!string.IsNullOrWhiteSpace(config.CompilerTarget))
        {
            var parts = config.CompilerTarget.Split('-', StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length >= 3)
            {
                options.TargetVendor = parts[1];
                options.TargetSystem = parts[2];
                options.TargetAbi = parts.Length >= 4 ? string.Join('-', parts.Skip(3)) : string.Empty;
            }
        }
    }

    private static IEnumerable<CppClass> EnumerateAllClasses(CppCompilation compilation)
    {
        foreach (var cls in compilation.Classes)
            yield return cls;
        foreach (var ns in compilation.Namespaces)
            foreach (var cls in EnumerateNamespaceClasses(ns))
                yield return cls;
    }

    private static IEnumerable<CppEnum> EnumerateAllEnums(CppCompilation compilation)
    {
        foreach (var cppEnum in compilation.Enums)
            yield return cppEnum;
        foreach (var ns in compilation.Namespaces)
            foreach (var cppEnum in EnumerateNamespaceEnums(ns))
                yield return cppEnum;
        foreach (var cls in EnumerateAllClasses(compilation))
            foreach (var cppEnum in EnumerateClassEnums(cls))
                yield return cppEnum;
    }

    private static IReadOnlyDictionary<string, List<string>> BuildVisibleTypeLookup(CppCompilation compilation)
    {
        var result = new Dictionary<string, List<string>>(StringComparer.Ordinal);

        foreach (var cls in EnumerateAllClasses(compilation))
            AddVisibleType(result, cls.Name, cls.FullName);

        foreach (var cppEnum in EnumerateAllEnums(compilation))
            AddVisibleType(result, cppEnum.Name, cppEnum.FullName);

        return result;
    }

    private static void AddVisibleType(Dictionary<string, List<string>> result, string name, string fullName)
    {
        if (string.IsNullOrWhiteSpace(name) || string.IsNullOrWhiteSpace(fullName))
            return;

        if (!result.TryGetValue(name, out var names))
        {
            names = [];
            result[name] = names;
        }

        if (!names.Contains(fullName, StringComparer.Ordinal))
            names.Add(fullName);
    }

    private static IEnumerable<CppClass> EnumerateNamespaceClasses(CppNamespace ns)
    {
        foreach (var cls in ns.Classes)
            yield return cls;
        foreach (var child in ns.Namespaces)
            foreach (var cls in EnumerateNamespaceClasses(child))
                yield return cls;
    }

    private static IEnumerable<CppEnum> EnumerateNamespaceEnums(CppNamespace ns)
    {
        foreach (var cppEnum in ns.Enums)
            yield return cppEnum;
        foreach (var child in ns.Namespaces)
            foreach (var cppEnum in EnumerateNamespaceEnums(child))
                yield return cppEnum;
    }

    private static IEnumerable<CppEnum> EnumerateClassEnums(CppClass cls)
    {
        foreach (var cppEnum in cls.Enums)
            yield return cppEnum;
        foreach (var child in cls.Classes)
            foreach (var cppEnum in EnumerateClassEnums(child))
                yield return cppEnum;
    }

    private static bool IsElementFromHeader(CppElement element, string normalizedHeader)
    {
        var sourceFileRaw = element.SourceFile ?? string.Empty;
        return !string.IsNullOrWhiteSpace(sourceFileRaw)
               && string.Equals(Path.GetFullPath(sourceFileRaw), normalizedHeader, PathComparison);
    }

    private static bool HasAttributeFromHeader(ICppAttributeContainer container, string normalizedHeader, string marker)
        => GetReflectionAttributeElements(container, marker).Any(attribute => IsElementFromHeader(attribute.Element, normalizedHeader));

    private static bool HasReflectionMarker(CppClass cls)
    {
        var hasAttribute = cls.Attributes.Any(IsReflectionAttribute);
#pragma warning disable CS0618
        var hasTokenAttribute = cls.TokenAttributes.Any(IsReflectionAttribute);
#pragma warning restore CS0618
        var hasMetaAttribute = cls.MetaAttributes.MetaList.Any(IsReflectionMetaAttribute);
        return hasAttribute || hasTokenAttribute || hasMetaAttribute;
    }

    private static bool HasReflectionMarker(CppEnum cppEnum)
    {
        var hasAttribute = cppEnum.Attributes.Any(IsReflectionAttribute);
#pragma warning disable CS0618
        var hasTokenAttribute = cppEnum.TokenAttributes.Any(IsReflectionAttribute);
#pragma warning restore CS0618
        var hasMetaAttribute = cppEnum.MetaAttributes.MetaList.Any(IsReflectionMetaAttribute);
        return hasAttribute || hasTokenAttribute || hasMetaAttribute;
    }

    private static bool HasPropertyMarker(CppField field)
        => GetReflectionAttributes(field, "Property").Count > 0;

    private static bool HasFunctionMarker(CppFunction function)
        => GetReflectionAttributes(function, "Function").Count > 0;

}

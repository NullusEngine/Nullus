using CppAst;

internal static partial class MetaParserTool
{
    private static IEnumerable<ReflectTypeInfo> ParseHeaderWithCppAst(string rootDir, string headerPath, PrecompileParams config)
    {
        var options = CreateOptions(config);
        var compilation = CppParser.ParseFile(headerPath, options);
        if (compilation.HasErrors)
        {
            var errors = string.Join(Environment.NewLine, compilation.Diagnostics.Messages);
            throw new InvalidOperationException(errors);
        }

        var normalizedHeader = Path.GetFullPath(headerPath);
        foreach (var cls in EnumerateAllClasses(compilation))
        {
            var sourceFileRaw = cls.SourceFile ?? string.Empty;
            var sourceFile = string.IsNullOrWhiteSpace(sourceFileRaw) ? string.Empty : Path.GetFullPath(sourceFileRaw);
            var isSameHeader = !string.IsNullOrEmpty(sourceFile) && string.Equals(sourceFile, normalizedHeader, PathComparison);
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
                fields = ExtractFields(cls);
                methods = ExtractMethods(cls);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[MetaParser] Warning: partial reflection fallback for {fullTypeName}: {ex.Message}");
                bases = TryExtract(() => ExtractBases(cls));
                fields = TryExtract(() => ExtractFields(cls));
                methods = TryExtract(() => ExtractMethods(cls));
            }

            yield return new ReflectTypeInfo(
                cls.Name,
                ExtractNamespace(fullTypeName, cls.Name),
                fullTypeName,
                ToGeneratedIncludePath(rootDir, normalizedHeader),
                normalizedHeader,
                bases,
                fields,
                methods);
        }

        foreach (var cppEnum in EnumerateAllEnums(compilation))
        {
            var sourceFileRaw = cppEnum.SourceFile ?? string.Empty;
            var sourceFile = string.IsNullOrWhiteSpace(sourceFileRaw) ? string.Empty : Path.GetFullPath(sourceFileRaw);
            var isSameHeader = !string.IsNullOrEmpty(sourceFile) && string.Equals(sourceFile, normalizedHeader, PathComparison);
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
                true,
                cppEnum.Items.Select(static item => new ReflectEnumValueInfo(item.Name)).ToList());
        }
    }

    private static CppParserOptions CreateOptions(PrecompileParams config)
    {
        var options = new CppParserOptions
        {
            ParseTokenAttributes = true,
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
        => HasMemberMarker(field, "Property");

    private static bool HasFunctionMarker(CppFunction function)
        => HasMemberMarker(function, "Function");
}

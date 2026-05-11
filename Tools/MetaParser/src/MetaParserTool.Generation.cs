using System.Text;

internal static partial class MetaParserTool
{
    internal static void GenerateReflectionOutputs(PrecompileParams config, IReadOnlyList<ReflectTypeInfo> orderedTypes, string outputDir)
    {
        var manifest = MetaParserGeneratorRegistry.ReflectionManifest;
        var templateRoot = Path.Combine(AppContext.BaseDirectory, "Templates");
        var moduleSession = BuildModuleTemplateSession(config.TargetName);
        var generatedSourceIncludes = new List<string>();
        var expectedHeaderOutputs = new HashSet<string>(OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);
        var expectedSourceOutputs = new HashSet<string>(OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);

        foreach (var headerPath in ExpandGeneratedHeaderStubInputs(config.Headers, config.RootDir))
        {
            if (!File.Exists(headerPath))
                continue;

            var headerText = File.ReadAllText(headerPath);
            if (!MatchesHeaderSelection(headerText, manifest.HeaderSelection) || FindGeneratedBodyLines(headerPath).Count == 0)
                continue;

            var includePath = ToGeneratedIncludePath(config.RootDir, headerPath);
            var stubOutputDir = ResolveGeneratedOutputDir(config.RootDir, outputDir, headerPath);
            expectedHeaderOutputs.Add(Path.GetFullPath(Path.Combine(
                stubOutputDir,
                $"{GetGeneratedRelativeBasePath(includePath)}{manifest.Outputs.HeaderGeneratedHeaderSuffix}")));
        }

        foreach (var headerGroup in orderedTypes.GroupBy(static type => type.HeaderPath, StringComparer.Ordinal))
        {
            var generatedBase = GetGeneratedRelativeBasePath(headerGroup.Key);
            var generatedHeaderPath = Path.Combine(outputDir, $"{generatedBase}{manifest.Outputs.HeaderGeneratedHeaderSuffix}");
            var generatedSourcePath = Path.Combine(outputDir, $"{generatedBase}{manifest.Outputs.HeaderGeneratedSourceSuffix}");
            expectedHeaderOutputs.Add(Path.GetFullPath(generatedHeaderPath));
            expectedSourceOutputs.Add(Path.GetFullPath(generatedSourcePath));
            var generatedDirectory = Path.GetDirectoryName(generatedHeaderPath);
            if (!string.IsNullOrWhiteSpace(generatedDirectory))
                Directory.CreateDirectory(generatedDirectory);

            var headerSession = BuildHeaderTemplateSession(
                headerGroup
                    .OrderBy(static type => type.GeneratedBodyLine ?? int.MaxValue)
                    .ThenBy(static type => type.QualifiedName, StringComparer.Ordinal)
                    .ToList(),
                orderedTypes);
            var generatedHeaderText = MetaParserTemplateRenderer.Render(
                MetaParserTemplateCatalog.ResolvePath(templateRoot, manifest.Templates.HeaderRule.HeaderTemplate),
                headerSession);
            var generatedSourceText = MetaParserTemplateRenderer.Render(
                MetaParserTemplateCatalog.ResolvePath(templateRoot, manifest.Templates.HeaderRule.SourceTemplate),
                headerSession);

            File.WriteAllText(generatedHeaderPath, generatedHeaderText, new UTF8Encoding(false));
            File.WriteAllText(generatedSourcePath, generatedSourceText, new UTF8Encoding(false));
            generatedSourceIncludes.Add($"{generatedBase}{manifest.Outputs.HeaderGeneratedSourceSuffix}".Replace('\\', '/'));
        }

        var moduleModel = moduleSession with { GeneratedSourceIncludes = generatedSourceIncludes };

        var moduleHeaderModel = new ModuleHeaderTemplateModel(moduleModel.LinkFunctionName);

        var moduleHeaderText = MetaParserTemplateRenderer.Render(
            MetaParserTemplateCatalog.ResolvePath(templateRoot, manifest.Templates.ModuleRule.HeaderTemplate),
            CreateTemplateSession(moduleHeaderModel));
        var moduleSourceText = MetaParserTemplateRenderer.Render(
            MetaParserTemplateCatalog.ResolvePath(templateRoot, manifest.Templates.ModuleRule.SourceTemplate),
            CreateTemplateSession(moduleModel));

        var sanitizedTargetName = moduleModel.SanitizedTargetName;
        File.WriteAllText(Path.Combine(outputDir, manifest.Outputs.ModuleHeaderFileName), moduleHeaderText, new UTF8Encoding(false));
        File.WriteAllText(
            Path.Combine(outputDir, manifest.Outputs.TargetModuleHeaderFileNamePattern.Replace("{SanitizedTargetName}", sanitizedTargetName, StringComparison.Ordinal)),
            moduleHeaderText,
            new UTF8Encoding(false));
        File.WriteAllText(Path.Combine(outputDir, manifest.Outputs.ModuleSourceFileName), moduleSourceText, new UTF8Encoding(false));

        RemoveStaleGeneratedOutputs(outputDir, manifest, expectedHeaderOutputs, expectedSourceOutputs);
    }

    private static void RemoveStaleGeneratedOutputs(
        string outputDir,
        MetaParserGeneratorManifest manifest,
        IReadOnlySet<string> expectedHeaderOutputs,
        IReadOnlySet<string> expectedSourceOutputs)
    {
        if (!Directory.Exists(outputDir))
            return;

        foreach (var path in Directory.EnumerateFiles(outputDir, "*", SearchOption.AllDirectories))
        {
            var fileName = Path.GetFileName(path);
            var fullPath = Path.GetFullPath(path);
            if (string.Equals(fileName, manifest.Outputs.ModuleHeaderFileName, StringComparison.Ordinal)
                || string.Equals(fileName, manifest.Outputs.ModuleSourceFileName, StringComparison.Ordinal)
                || fileName.EndsWith("_MetaGenerated.h", StringComparison.Ordinal))
            {
                continue;
            }

            if (fileName.EndsWith(manifest.Outputs.HeaderGeneratedHeaderSuffix, StringComparison.Ordinal)
                && !expectedHeaderOutputs.Contains(fullPath))
            {
                File.Delete(fullPath);
                continue;
            }

            if (fileName.EndsWith(manifest.Outputs.HeaderGeneratedSourceSuffix, StringComparison.Ordinal)
                && !expectedSourceOutputs.Contains(fullPath))
            {
                File.Delete(fullPath);
            }
        }
    }

    private static Dictionary<string, object?> CreateTemplateSession<TModel>(TModel model)
        where TModel : class
        => new() { ["Model"] = model };

    private static ModuleTemplateModel BuildModuleTemplateSession(string targetName)
    {
        var sanitizedTargetName = SanitizeIdentifier(string.IsNullOrWhiteSpace(targetName) ? "UnknownTarget" : targetName);
        return new ModuleTemplateModel(
            sanitizedTargetName,
            $"LinkReflectionTypes_{sanitizedTargetName}",
            []);
    }

    private static Dictionary<string, object?> BuildHeaderTemplateSession(IReadOnlyList<ReflectTypeInfo> types, IReadOnlyList<ReflectTypeInfo> allTypes)
    {
        var headerPath = types[0].HeaderPath;
        var fileId = BuildGeneratedFileId(headerPath);
        var headerIncludePath = headerPath.Replace('\\', '/');
        var generatedHeaderIncludePath = $"{GetGeneratedRelativeBasePath(headerPath)}.generated.h".Replace('\\', '/');

        var typeModels = new List<GeneratedTypeTemplateModel>();
        var privateTypeModels = new List<GeneratedPrivateTypeTemplateModel>();

        foreach (var type in types)
        {
            var generatedBodyLine = type.GeneratedBodyLine;

            var fieldModels = type.Fields
                .Select((field, index) => new GeneratedFieldTemplateModel(
                    field.Name,
                    field.TypeName,
                    field.GetterExpression,
                    field.SetterExpression,
                    field.IsPrivate,
                    BuildPrivateFieldAccessorName(field.Name, index),
                    (field.FieldMetas ?? [])
                        .Select(static meta => new GeneratedTypeMetaTemplateModel(
                            meta.PropertyTypeName,
                            meta.InitializerArguments))
                        .ToList()))
                .ToList();

            var methodModels = type.Methods
                .Select((method, index) => new GeneratedMethodTemplateModel(
                    method.Name,
                    method.PointerExpression,
                    method.IsStatic,
                    method.IsPrivate,
                    BuildPrivateMethodAccessorName(method.Name, index)))
                .ToList();

            var typeMetaModels = (type.TypeMetas ?? [])
                .Select(static meta => new GeneratedTypeMetaTemplateModel(
                    meta.PropertyTypeName,
                    meta.InitializerArguments))
                .ToList();

            typeModels.Add(new GeneratedTypeTemplateModel(
                type.ClassName,
                type.QualifiedName,
                ShouldEnableObjectBridge(type, allTypes),
                BuildPrivateAccessStructName(type.QualifiedName),
                BuildRegisterFunctionName(type.QualifiedName),
                BuildRegistrarClassName(type.QualifiedName),
                generatedBodyLine.HasValue ? BuildGeneratedBodyMacroName(fileId, generatedBodyLine.Value) : string.Empty,
                type.IsEnum,
                (type.EnumValues ?? []).Select(static value => value.Name).ToList(),
                type.Bases.Select(static baseInfo => baseInfo.TypeName).Distinct().ToList(),
                typeMetaModels,
                fieldModels,
                methodModels));

            if (type.IsEnum)
                continue;

            var hasPrivateFields = fieldModels.Any(static field => field.IsPrivate);
            var hasPrivateMethods = methodModels.Any(static method => method.IsPrivate);
            if (!hasPrivateFields && !hasPrivateMethods)
                continue;

            privateTypeModels.Add(new GeneratedPrivateTypeTemplateModel(
                type.ClassName,
                type.QualifiedName,
                ShouldEnableObjectBridge(type, allTypes),
                BuildPrivateAccessStructName(type.QualifiedName),
                BuildRegisterFunctionName(type.QualifiedName),
                BuildRegistrarClassName(type.QualifiedName),
                generatedBodyLine.HasValue ? BuildGeneratedBodyMacroName(fileId, generatedBodyLine.Value) : string.Empty,
                fieldModels.Where(static field => field.IsPrivate)
                    .Select(static field => new GeneratedPrivateFieldTemplateModel(field.Name, field.AccessorName))
                    .ToList(),
                methodModels.Where(static method => method.IsPrivate)
                    .Select(static method => new GeneratedPrivateMethodTemplateModel(method.Name, method.AccessorName, method.PointerExpression))
                    .ToList()));
        }

        return CreateTemplateSession(new HeaderTemplateModel(
            headerPath,
            fileId,
            headerIncludePath,
            generatedHeaderIncludePath,
            types.All(static type => !type.GeneratedBodyLine.HasValue),
            typeModels,
            privateTypeModels));
    }

    private static bool ShouldEnableObjectBridge(ReflectTypeInfo type, IReadOnlyList<ReflectTypeInfo> allTypes)
    {
        if (type.IsEnum)
            return false;

        var byName = allTypes.ToDictionary(static item => item.QualifiedName, StringComparer.Ordinal);
        var visited = new HashSet<string>(StringComparer.Ordinal);
        return InheritsFromMetaObject(type, byName, visited);
    }

    private static bool InheritsFromMetaObject(
        ReflectTypeInfo type,
        IReadOnlyDictionary<string, ReflectTypeInfo> byName,
        HashSet<string> visited)
    {
        if (!visited.Add(type.QualifiedName))
            return false;

        foreach (var baseInfo in type.Bases)
        {
            var baseName = NormalizeTypeName(baseInfo.TypeName);
            if (string.Equals(baseName, "NLS::meta::Object", StringComparison.Ordinal)
                || string.Equals(baseName, "meta::Object", StringComparison.Ordinal))
            {
                return true;
            }

            if (byName.TryGetValue(baseName, out var reflectedBase)
                && InheritsFromMetaObject(reflectedBase, byName, visited))
            {
                return true;
            }
        }

        return false;
    }

    private static bool ShouldParseHeader(string headerPath, IReadOnlyList<MetaParserGeneratorDefinition> generators)
    {
        var extension = Path.GetExtension(headerPath);
        if (!string.Equals(extension, ".h", StringComparison.OrdinalIgnoreCase)
            && !string.Equals(extension, ".hpp", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        var headerText = File.ReadAllText(headerPath);
        return generators.Any(generator => MatchesHeaderSelection(headerText, generator.Manifest.HeaderSelection));
    }

    private static bool MatchesHeaderSelection(string headerText, GeneratorHeaderSelection selection)
        => selection.Markers.Any(marker => headerText.Contains(marker, StringComparison.Ordinal));

    private static IReadOnlyList<string> ExpandGeneratedHeaderStubInputs(IEnumerable<string> headers, string rootDir)
    {
        var expanded = headers
            .Where(static header => !string.IsNullOrWhiteSpace(header))
            .Select(Path.GetFullPath)
            .ToList();
        var seen = new HashSet<string>(
            expanded,
            OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);
        var runtimeRoot = Path.Combine(rootDir, "Runtime");

        if (!Directory.Exists(runtimeRoot))
            return expanded;

        foreach (var headerPath in Directory.EnumerateFiles(runtimeRoot, "*.*", SearchOption.AllDirectories)
                     .Where(path => string.Equals(Path.GetExtension(path), ".h", StringComparison.OrdinalIgnoreCase)
                                 || string.Equals(Path.GetExtension(path), ".hpp", StringComparison.OrdinalIgnoreCase)))
        {
            var normalizedHeader = Path.GetFullPath(headerPath);
            if (!seen.Add(normalizedHeader))
                continue;

            if (ShouldParseHeader(normalizedHeader, MetaParserGeneratorRegistry.All))
                expanded.Add(normalizedHeader);
        }

        return expanded;
    }

    private static void EnsureGeneratedHeaderStubs(
        IEnumerable<string> headers,
        string rootDir,
        string outputDir,
        IReadOnlyList<MetaParserGeneratorDefinition> generators)
    {
        var stubGenerators = generators
            .Where(static generator => generator.Manifest.HeaderSelection.RequiresGeneratedHeaderStub)
            .ToList();

        foreach (var headerPath in headers)
        {
            if (!File.Exists(headerPath))
                continue;

            var headerText = File.ReadAllText(headerPath);
            foreach (var generator in stubGenerators)
            {
                if (!MatchesHeaderSelection(headerText, generator.Manifest.HeaderSelection))
                    continue;

                var includePath = ToGeneratedIncludePath(rootDir, headerPath);
                var stubOutputDir = ResolveGeneratedOutputDir(rootDir, outputDir, headerPath);
                var generatedHeaderPath = Path.Combine(
                    stubOutputDir,
                    $"{GetGeneratedRelativeBasePath(includePath)}{generator.Manifest.Outputs.HeaderGeneratedHeaderSuffix}");
                if (File.Exists(generatedHeaderPath) && !IsPathInside(generatedHeaderPath, outputDir))
                    continue;

                var generatedDirectory = Path.GetDirectoryName(generatedHeaderPath);
                if (!string.IsNullOrWhiteSpace(generatedDirectory))
                    Directory.CreateDirectory(generatedDirectory);

                var fileId = BuildGeneratedFileId(includePath);
                var generatedBodyLines = FindGeneratedBodyLines(headerPath);
                var builder = new StringBuilder();
                builder.AppendLine("#pragma once");
                builder.AppendLine();
                builder.AppendLine("#ifdef CURRENT_FILE_ID");
                builder.AppendLine("#undef CURRENT_FILE_ID");
                builder.AppendLine("#endif");
                builder.AppendLine($"#define CURRENT_FILE_ID {fileId}");

                foreach (var line in generatedBodyLines)
                {
                    var macroName = BuildGeneratedBodyMacroName(fileId, line);
                    builder.AppendLine();
                    builder.AppendLine($"#ifdef {macroName}");
                    builder.AppendLine($"#undef {macroName}");
                    builder.AppendLine("#endif");
                    builder.AppendLine($"#define {macroName}");
                }

                File.WriteAllText(generatedHeaderPath, builder.ToString(), new UTF8Encoding(false));
            }
        }
    }

    private static bool IsPathInside(string path, string directory)
    {
        var normalizedPath = Path.GetFullPath(path);
        var normalizedDirectory = Path.GetFullPath(directory)
            .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
            + Path.DirectorySeparatorChar;

        return normalizedPath.StartsWith(
            normalizedDirectory,
            OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal);
    }

    private static string ResolveGeneratedOutputDir(string rootDir, string defaultOutputDir, string headerPath)
    {
        var runtimeRoot = Path.Combine(rootDir, "Runtime");
        var relativeHeader = Path.GetRelativePath(runtimeRoot, headerPath).Replace('\\', '/');
        if (relativeHeader.StartsWith("..", StringComparison.Ordinal))
            return defaultOutputDir;

        var separatorIndex = relativeHeader.IndexOf('/');
        if (separatorIndex <= 0)
            return defaultOutputDir;

        var moduleName = relativeHeader[..separatorIndex];
        var moduleSourceDir = Path.Combine(rootDir, "Runtime", moduleName);
        return Directory.Exists(moduleSourceDir)
            ? Path.Combine(moduleSourceDir, "Gen")
            : defaultOutputDir;
    }
}

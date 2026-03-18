using System.Text;

internal static partial class MetaParserTool
{
    internal static void GenerateReflectionOutputs(PrecompileParams config, IReadOnlyList<ReflectTypeInfo> orderedTypes, string outputDir)
    {
        var manifest = MetaParserGeneratorRegistry.ReflectionManifest;
        var templateRoot = Path.Combine(AppContext.BaseDirectory, "Templates");
        var moduleSession = BuildModuleTemplateSession(config.TargetName);
        var generatedSourceIncludes = new List<string>();

        foreach (var headerGroup in orderedTypes.GroupBy(static type => type.HeaderPath, StringComparer.Ordinal))
        {
            var generatedBase = GetGeneratedRelativeBasePath(headerGroup.Key);
            var generatedHeaderPath = Path.Combine(outputDir, $"{generatedBase}{manifest.Outputs.HeaderGeneratedHeaderSuffix}");
            var generatedSourcePath = Path.Combine(outputDir, $"{generatedBase}{manifest.Outputs.HeaderGeneratedSourceSuffix}");
            var generatedDirectory = Path.GetDirectoryName(generatedHeaderPath);
            if (!string.IsNullOrWhiteSpace(generatedDirectory))
                Directory.CreateDirectory(generatedDirectory);

            var headerSession = BuildHeaderTemplateSession(headerGroup.ToList());
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

    private static Dictionary<string, object?> BuildHeaderTemplateSession(IReadOnlyList<ReflectTypeInfo> types)
    {
        var headerPath = types[0].HeaderPath;
        var fileId = BuildGeneratedFileId(headerPath);
        var generatedBodyLines = FindGeneratedBodyLines(types[0].SourceFilePath);
        var headerIncludePath = headerPath.Replace('\\', '/');
        var generatedHeaderIncludePath = $"{GetGeneratedRelativeBasePath(headerPath)}.generated.h".Replace('\\', '/');

        var typeModels = new List<GeneratedTypeTemplateModel>();
        var privateTypeModels = new List<GeneratedPrivateTypeTemplateModel>();
        var generatedBodyLineIndex = 0;

        foreach (var type in types)
        {
            int? generatedBodyLine = generatedBodyLineIndex < generatedBodyLines.Count
                ? generatedBodyLines[generatedBodyLineIndex++]
                : null;

            var fieldModels = type.Fields
                .Select((field, index) => new GeneratedFieldTemplateModel(
                    field.Name,
                    field.TypeName,
                    field.GetterExpression,
                    field.SetterExpression,
                    field.IsPrivate,
                    BuildPrivateFieldAccessorName(field.Name, index)))
                .ToList();

            var methodModels = type.Methods
                .Select((method, index) => new GeneratedMethodTemplateModel(
                    method.Name,
                    method.PointerExpression,
                    method.IsStatic,
                    method.IsPrivate,
                    BuildPrivateMethodAccessorName(method.Name, index)))
                .ToList();

            typeModels.Add(new GeneratedTypeTemplateModel(
                type.ClassName,
                type.QualifiedName,
                type.QualifiedName.StartsWith("NLS::Engine::Components::", StringComparison.Ordinal),
                BuildPrivateAccessStructName(type.QualifiedName),
                BuildRegisterFunctionName(type.QualifiedName),
                BuildRegistrarClassName(type.QualifiedName),
                generatedBodyLine.HasValue ? BuildGeneratedBodyMacroName(fileId, generatedBodyLine.Value) : string.Empty,
                type.IsEnum,
                (type.EnumValues ?? []).Select(static value => value.Name).ToList(),
                type.Bases.Select(static baseInfo => baseInfo.TypeName).Distinct().ToList(),
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
                type.QualifiedName.StartsWith("NLS::Engine::Components::", StringComparison.Ordinal),
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
            generatedBodyLines.Count == 0,
            typeModels,
            privateTypeModels));
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
                var generatedHeaderPath = Path.Combine(
                    outputDir,
                    $"{GetGeneratedRelativeBasePath(includePath)}{generator.Manifest.Outputs.HeaderGeneratedHeaderSuffix}");
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
}

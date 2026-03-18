public sealed record GeneratorHeaderSelection(
    IReadOnlyList<string> Markers,
    bool RequiresGeneratedHeaderStub);

public sealed record GeneratorOutputLayout(
    string HeaderGeneratedHeaderSuffix,
    string HeaderGeneratedSourceSuffix,
    string ModuleHeaderFileName,
    string TargetModuleHeaderFileNamePattern,
    string ModuleSourceFileName);

public sealed record GeneratorTemplateSet(
    HeaderGenerationRule HeaderRule,
    ModuleGenerationRule ModuleRule);

public sealed record MetaParserGeneratorManifest(
    string Name,
    GeneratorHeaderSelection HeaderSelection,
    GeneratorTemplateSet Templates,
    GeneratorOutputLayout Outputs);

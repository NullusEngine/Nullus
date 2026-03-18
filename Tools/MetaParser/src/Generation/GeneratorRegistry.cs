internal sealed record MetaParserGeneratorDefinition(
    MetaParserGeneratorManifest Manifest,
    Action<PrecompileParams, IReadOnlyList<ReflectTypeInfo>, string> Generate);

internal static class MetaParserGeneratorRegistry
{
    public static readonly MetaParserGeneratorManifest ReflectionManifest = new(
        "reflection",
        new GeneratorHeaderSelection(
            ["GENERATED_BODY(", "ENUM(", "MetaExternal", "REFLECT_EXTERNAL"],
            true),
        new GeneratorTemplateSet(
            MetaParserTemplateCatalog.ReflectedHeaderRule,
            MetaParserTemplateCatalog.ReflectionModuleRule),
        new GeneratorOutputLayout(
            ".generated.h",
            ".generated.cpp",
            "MetaGenerated.h",
            "{SanitizedTargetName}_MetaGenerated.h",
            "MetaGenerated.cpp"));

    public static IReadOnlyList<MetaParserGeneratorDefinition> All { get; } =
    [
        new(ReflectionManifest, MetaParserTool.GenerateReflectionOutputs)
    ];
}

public sealed record TemplateAsset(string Name, string FileName);

public sealed record HeaderGenerationRule(string Name, TemplateAsset HeaderTemplate, TemplateAsset SourceTemplate);

public sealed record ModuleGenerationRule(string Name, TemplateAsset HeaderTemplate, TemplateAsset SourceTemplate);

internal static class MetaParserTemplateCatalog
{
    public static readonly TemplateAsset HeaderGeneratedHeader = new("header-generated-header", "HeaderGenerated.h.tt");
    public static readonly TemplateAsset HeaderGeneratedSource = new("header-generated-source", "HeaderGenerated.cpp.tt");
    public static readonly TemplateAsset MetaGeneratedHeader = new("meta-generated-header", "MetaGenerated.h.tt");
    public static readonly TemplateAsset MetaGeneratedSource = new("meta-generated-source", "MetaGenerated.cpp.tt");

    public static readonly HeaderGenerationRule ReflectedHeaderRule = new(
        "reflected-header",
        HeaderGeneratedHeader,
        HeaderGeneratedSource);

    public static readonly ModuleGenerationRule ReflectionModuleRule = new(
        "reflection-module",
        MetaGeneratedHeader,
        MetaGeneratedSource);

    public static string ResolvePath(string templateRoot, TemplateAsset asset)
        => Path.Combine(templateRoot, asset.FileName);
}

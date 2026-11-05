using System.CodeDom.Compiler;
using Mono.TextTemplating;
using Microsoft.VisualStudio.TextTemplating;
using Nullus.MetaParser.CompiledTemplates;

public interface IMetaParserRuntimeTemplate
{
    IDictionary<string, object> Session { get; set; }
    ITextTemplatingEngineHost Host { get; set; }
    CompilerErrorCollection Diagnostics { get; }
    void Initialize();
    string TransformText();
}

internal sealed class MetaParserTemplateRenderer : IDisposable
{
    private sealed class TemplateHost : TemplateGenerator
    {
        public TemplateHost(string templatePath)
        {
            TemplateFile = templatePath;
            OutputFile = Path.ChangeExtension(templatePath, ".output");
        }
    }

    public string Render(string templatePath, IReadOnlyDictionary<string, object?> sessionValues)
    {
        IMetaParserRuntimeTemplate template = Path.GetFileName(templatePath) switch
        {
            "HeaderGenerated.h.tt" => new HeaderGeneratedHeader(),
            "HeaderGenerated.cpp.tt" => new HeaderGeneratedSource(),
            "MetaGenerated.h.tt" => new ModuleGeneratedHeader(),
            "MetaGenerated.cpp.tt" => new ModuleGeneratedSource(),
            "EditorExtensionDescriptors.cpp.tt" => new EditorExtensionDescriptors(),
            _ => throw new ArgumentException($"Unknown precompiled MetaParser template: {templatePath}")
        };
        template.Host = new TemplateHost(Path.GetFullPath(templatePath));
        template.Session = sessionValues.ToDictionary(pair => pair.Key, pair => pair.Value!);
        template.Initialize();
        var output = template.TransformText();
        if (template.Diagnostics.HasErrors)
            throw RenderError(templatePath, template.Diagnostics);
        return output;
    }

    private static InvalidOperationException RenderError(string templatePath, CompilerErrorCollection errors)
        => new($"T4 render failed for {templatePath}:{Environment.NewLine}" +
               string.Join(Environment.NewLine, errors.Cast<CompilerError>().Select(static error => error.ToString())));

    public void Dispose() { }
}

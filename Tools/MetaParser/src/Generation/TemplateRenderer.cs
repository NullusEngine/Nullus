using System.CodeDom.Compiler;
using Mono.TextTemplating;
using Microsoft.VisualStudio.TextTemplating;

internal static class MetaParserTemplateRenderer
{
    public static string Render(string templatePath, IReadOnlyDictionary<string, object?> sessionValues)
    {
        var generator = new TemplateGenerator();
        var sessionHost = (ITextTemplatingSessionHost)generator;
        sessionHost.Session = sessionHost.CreateSession();
        foreach (var pair in sessionValues)
            sessionHost.Session[pair.Key] = pair.Value;

        var outputPath = Path.GetTempFileName();
        try
        {
            var success = generator.ProcessTemplateAsync(templatePath, outputPath).GetAwaiter().GetResult();
            if (!success || generator.Errors.HasErrors)
            {
                var errors = string.Join(Environment.NewLine, generator.Errors.Cast<CompilerError>().Select(static error => error.ToString()));
                throw new InvalidOperationException($"T4 render failed for {templatePath}:{Environment.NewLine}{errors}");
            }

            return File.ReadAllText(outputPath);
        }
        finally
        {
            if (File.Exists(outputPath))
                File.Delete(outputPath);
        }
    }
}

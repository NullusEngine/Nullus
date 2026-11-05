using System.CodeDom.Compiler;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Mono.TextTemplating;

if (args.Length != 2)
{
    Console.Error.WriteLine("Usage: Nullus.MetaTemplateCompiler <template-directory> <output-directory>");
    return 2;
}

var templateRoot = Path.GetFullPath(args[0]);
var outputRoot = Path.GetFullPath(args[1]);
Directory.CreateDirectory(outputRoot);
var templates = new Dictionary<string, string>
{
    ["HeaderGenerated.h.tt"] = "HeaderGeneratedHeader",
    ["HeaderGenerated.cpp.tt"] = "HeaderGeneratedSource",
    ["MetaGenerated.h.tt"] = "ModuleGeneratedHeader",
    ["MetaGenerated.cpp.tt"] = "ModuleGeneratedSource",
    ["EditorExtensionDescriptors.cpp.tt"] = "EditorExtensionDescriptors"
};

foreach (var (fileName, className) in templates)
{
    var input = Path.Combine(templateRoot, fileName);
    var generator = new RuntimeTemplateGenerator(input);
    var temporary = Path.Combine(outputRoot, className + ".preprocess.tmp");
    bool success;
    string language;
    string code;
    try
    {
        success = generator.PreprocessTemplate(input, className, "Nullus.MetaParser.CompiledTemplates",
            temporary, new UTF8Encoding(false), out language, out _);
        code = success ? File.ReadAllText(temporary) : string.Empty;
    }
    finally
    {
        if (File.Exists(temporary))
            File.Delete(temporary);
    }
    if (!success || generator.Errors.HasErrors || language != "C#")
    {
        Console.Error.WriteLine($"T4 preprocessing failed: {input}");
        foreach (CompilerError error in generator.Errors)
            Console.Error.WriteLine(error);
        return 1;
    }
    var output = Path.Combine(outputRoot, className + ".g.cs");
    // Mono.TextTemplating 3.0 can emit empty filenames for preprocessed line
    // pragmas. Keep compiler diagnostics mapped to the actual source template.
    code = Regex.Replace(code, @"(?m)(#line\s+\d+\s+)\u0022\u0022", match => match.Groups[1].Value + JsonSerializer.Serialize(input));
    var adapter = $$"""

        namespace Nullus.MetaParser.CompiledTemplates
        {
            public partial class {{className}} : global::IMetaParserRuntimeTemplate
            {
                public global::System.CodeDom.Compiler.CompilerErrorCollection Diagnostics => Errors;
            }
        }
        """;
    var content = "#nullable disable" + Environment.NewLine + code + Environment.NewLine + adapter;
    if (!File.Exists(output) || File.ReadAllText(output) != content)
        File.WriteAllText(output, content, new UTF8Encoding(false));
    else
        File.SetLastWriteTimeUtc(output, DateTime.UtcNow);
}
Console.WriteLine($"Preprocessed {templates.Count} MetaParser templates.");
return 0;

sealed class RuntimeTemplateGenerator : TemplateGenerator
{
    public RuntimeTemplateGenerator(string input)
    {
        TemplateFile = input;
        OutputFile = Path.ChangeExtension(input, ".generated.cs");
    }
}

using System.Text;
using System.Text.RegularExpressions;
using CppAst;

internal sealed record ReflectTypeInfo(string ClassName, string NamespacePrefix, string HeaderPath)
{
    public string FullTypeName => string.IsNullOrWhiteSpace(NamespacePrefix) ? ClassName : $"{NamespacePrefix}::{ClassName}";
}

internal static class Program
{
    public static int Main(string[] args)
    {
        var root = GetArg(args, "--root");
        var header = GetArg(args, "--header");
        var output = GetArg(args, "--out");
        var functionName = GetArg(args, "--func");

        if (string.IsNullOrWhiteSpace(root) || string.IsNullOrWhiteSpace(header) || string.IsNullOrWhiteSpace(output) || string.IsNullOrWhiteSpace(functionName))
        {
            Console.Error.WriteLine("Usage: MetaParser --root <repo_root> --header <header_path> --out <generated_cpp_path> --func <register_function_name>");
            return 2;
        }

        var rootDir = Path.GetFullPath(root);
        var runtimeDir = Path.Combine(rootDir, "Runtime");
        var headerPath = Path.GetFullPath(header);
        var outFile = Path.GetFullPath(output);

        if (!File.Exists(headerPath))
        {
            Console.Error.WriteLine($"Header not found: {headerPath}");
            return 3;
        }

        var types = ParseHeader(rootDir, runtimeDir, headerPath)
            .Distinct()
            .OrderBy(t => t.NamespacePrefix, StringComparer.Ordinal)
            .ThenBy(t => t.ClassName, StringComparer.Ordinal)
            .ToList();

        var generated = RenderPerHeader(rootDir, ToIncludePath(rootDir, headerPath), functionName!, types);
        Directory.CreateDirectory(Path.GetDirectoryName(outFile)!);
        File.WriteAllText(outFile, generated, new UTF8Encoding(false));

        Console.WriteLine($"Generated {types.Count} reflection classes from {headerPath}");
        Console.WriteLine($"Output written to: {outFile}");
        return 0;
    }

    private static List<ReflectTypeInfo> ParseHeader(string rootDir, string runtimeDir, string header)
    {
        var sourceText = File.ReadAllText(header);
        if (!MightContainMeta(sourceText))
            return [];

        var options = new CppParserOptions();
        options.ConfigureForWindowsMsvc(CppTargetCpu.X86_64);
        options.ParseTokenAttributes = true;
        options.Defines.Add("__REFLECTION_PARSER__");
        options.AdditionalArguments.Add("-std=c++20");
        options.AdditionalArguments.Add("-x");
        options.AdditionalArguments.Add("c++");
        options.IncludeFolders.Add(runtimeDir);
        options.IncludeFolders.Add(Path.Combine(runtimeDir, "Base"));
        options.IncludeFolders.Add(Path.Combine(runtimeDir, "Base", "Reflection"));

        var compilation = CppParser.ParseFile(header, options);

        var sink = new List<ReflectTypeInfo>();
        foreach (var cls in EnumerateClasses(compilation.Classes))
        {
            if (!cls.IsDefinition)
                continue;

            if (!HasReflectionMarker(sourceText, cls.Name))
                continue;

            if (!InheritsReflectionObject(cls) && !InheritsReflectionObjectFromSource(sourceText, cls.Name))
                continue;

            var nsPrefix = ExtractNamespace(cls.FullName, cls.Name);
            sink.Add(new ReflectTypeInfo(cls.Name, nsPrefix, ToIncludePath(rootDir, header)));
        }

        if (sink.Count == 0)
        {
            var nsFallback = ExtractLikelyNamespace(sourceText);
            var fallbackNames = new HashSet<string>(StringComparer.Ordinal);

            foreach (Match match in Regex.Matches(sourceText, @"\bMeta\s*\([^\)]*\)\s*(?:class|struct)\s+(?<name>[A-Za-z_]\w*)", RegexOptions.Multiline))
                fallbackNames.Add(match.Groups["name"].Value);

            foreach (Match match in Regex.Matches(sourceText, @"\b(?:CLASS|STRUCT)\s*\(\s*(?<name>[A-Za-z_]\w*)\s*,", RegexOptions.Multiline))
                fallbackNames.Add(match.Groups["name"].Value);

            foreach (var className in fallbackNames)
            {
                if (!InheritsReflectionObjectFromSource(sourceText, className))
                    continue;

                sink.Add(new ReflectTypeInfo(className, nsFallback, ToIncludePath(rootDir, header)));
            }
        }

        return sink;
    }

    private static IEnumerable<CppClass> EnumerateClasses(IEnumerable<CppClass> classes)
    {
        foreach (var cls in classes)
        {
            yield return cls;
            foreach (var nested in EnumerateClasses(cls.Classes))
                yield return nested;
        }
    }

    private static bool InheritsReflectionObject(CppClass cls)
    {
        foreach (var baseType in cls.BaseTypes)
        {
            var display = baseType.Type.GetDisplayName();
            if (display == "Object" || display == "NLS::meta::Object" || display.EndsWith("::Object", StringComparison.Ordinal))
                return true;

            if (baseType.Type is CppClass baseClass)
            {
                var baseName = baseClass.FullName;
                if (baseName == "NLS::meta::Object" || baseName.EndsWith("::Object", StringComparison.Ordinal))
                    return true;
            }
        }

        return false;
    }

    private static bool InheritsReflectionObjectFromSource(string sourceText, string className)
    {
        var escapedName = Regex.Escape(className);
        return Regex.IsMatch(sourceText,
                   $@"(?:class|struct)\s+{escapedName}\s*:\s*[^\{{\n;]*\bObject\b",
                   RegexOptions.Multiline)
               || Regex.IsMatch(sourceText,
                   $@"\b(?:CLASS|STRUCT)\s*\(\s*{escapedName}\s*,[^\)]*\)\s*:\s*[^\{{\n;]*\bObject\b",
                   RegexOptions.Multiline);
    }

    private static bool MightContainMeta(string sourceText)
    {
        return sourceText.Contains("Meta(", StringComparison.Ordinal)
            || sourceText.Contains("META(", StringComparison.Ordinal)
            || sourceText.Contains("CLASS(", StringComparison.Ordinal)
            || sourceText.Contains("STRUCT(", StringComparison.Ordinal)
            || sourceText.Contains("annotate(", StringComparison.Ordinal);
    }

    private static bool HasReflectionMarker(string sourceText, string className)
    {
        var escapedName = Regex.Escape(className);

        // 1) 优先识别 annotate（UE 风格宏展开后的形态）
        var annotateClass = new Regex($@"(?:class|struct)\s+__attribute__\s*\(\(\s*annotate\([^\)]*\)\s*\)\)\s+{escapedName}\b", RegexOptions.Multiline);
        if (annotateClass.IsMatch(sourceText))
            return true;

        // 2) CLASS/STRUCT 宏（调用处）
        var macroClass = new Regex($@"\bCLASS\s*\(\s*{escapedName}\s*,", RegexOptions.Multiline);
        var macroStruct = new Regex($@"\bSTRUCT\s*\(\s*{escapedName}\s*,", RegexOptions.Multiline);
        if (macroClass.IsMatch(sourceText) || macroStruct.IsMatch(sourceText))
            return true;

        // 3) 兼容旧 Meta() 形态
        var oldMetaClass = new Regex($@"\bMeta\s*\([^\)]*\)\s*(?:class|struct)\s+{escapedName}\b", RegexOptions.Multiline);
        return oldMetaClass.IsMatch(sourceText);
    }

    private static string ExtractNamespace(string fullName, string className)
    {
        if (string.IsNullOrWhiteSpace(fullName)) return string.Empty;
        var suffix = $"::{className}";
        return fullName.EndsWith(suffix, StringComparison.Ordinal) ? fullName[..^suffix.Length] : string.Empty;
    }

    private static string ExtractLikelyNamespace(string sourceText)
    {
        var names = Regex.Matches(sourceText, @"\bnamespace\s+([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)")
            .Cast<Match>()
            .Select(m => m.Groups[1].Value)
            .ToList();

        var scoped = names.FirstOrDefault(n => n.Contains("::", StringComparison.Ordinal));
        if (!string.IsNullOrWhiteSpace(scoped))
            return scoped;

        return string.Join("::", names.Distinct());
    }

    private static string ToIncludePath(string rootDir, string header)
    {
        var rel = Path.GetRelativePath(Path.Combine(rootDir, "Runtime"), header).Replace('\\', '/');
        return $"\"{rel}\"";
    }

    private static string RenderPerHeader(string rootDir, string includePath, string functionName, IReadOnlyList<ReflectTypeInfo> types)
    {
        var templatePath = Path.Combine(rootDir, "Tools", "MetaParser", "src", "Templates", "MetaGenerated.cpp.tt");
        var template = File.ReadAllText(templatePath);

        var includes = new StringBuilder();
        includes.AppendLine($"#include {includePath}");

        var registerBody = new StringBuilder();
        foreach (var type in types)
        {
            registerBody.AppendLine("    {");
            registerBody.AppendLine($"        auto id = db.AllocateType(\"{type.FullTypeName}\");");
            registerBody.AppendLine("        if (id != NLS::meta::InvalidTypeID) {");
            registerBody.AppendLine("            auto& typeData = db.types[id];");
            registerBody.AppendLine($"            typeData.name = \"{type.FullTypeName}\";");
            registerBody.AppendLine($"            NLS::meta::TypeInfo<{type.FullTypeName}>::Register(id, typeData, true);");
            registerBody.AppendLine("        }");
            registerBody.AppendLine("    }");
        }

        var typeList = types.Count == 0
            ? "// (none)"
            : string.Join('\n', types.Select(t => $"// - {t.FullTypeName}"));

        return template
            .Replace("<#@ template language=\"C#\" hostspecific=\"true\" #>\n", string.Empty, StringComparison.Ordinal)
            .Replace("<#= Includes #>", includes.ToString(), StringComparison.Ordinal)
            .Replace("<#= FunctionName #>", functionName, StringComparison.Ordinal)
            .Replace("<#= RegisterBody #>", registerBody.ToString().TrimEnd(), StringComparison.Ordinal)
            .Replace("<#= TypeList #>", typeList, StringComparison.Ordinal);
    }

    private static string? GetArg(string[] args, string name)
    {
        for (var i = 0; i < args.Length - 1; i++)
        {
            if (args[i] == name) return args[i + 1];
        }
        return null;
    }
}

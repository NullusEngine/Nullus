using System.CodeDom.Compiler;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using CppAst;
using Mono.TextTemplating;
using Microsoft.VisualStudio.TextTemplating;

internal sealed record ReflectFieldInfo(string Name, string TypeName);
internal sealed record ReflectMethodInfo(string Name);
internal sealed record ReflectBaseInfo(string TypeName);

internal sealed record ReflectTypeInfo(
    string ClassName,
    string NamespacePrefix,
    string FullTypeName,
    string HeaderPath,
    List<ReflectBaseInfo> Bases,
    List<ReflectFieldInfo> Fields,
    List<ReflectMethodInfo> Methods)
{
    public string QualifiedName => FullTypeName;
}

internal sealed record TextMemberParseResult(
    List<ReflectFieldInfo> Fields,
    List<ReflectMethodInfo> Methods);

internal sealed class PrecompileParams
{
    public string RootDir { get; set; } = string.Empty;
    public string TargetName { get; set; } = string.Empty;
    public string ModuleName { get; set; } = string.Empty;
    public string OutputDir { get; set; } = string.Empty;
    public string CompilerPath { get; set; } = string.Empty;
    public string CompilerId { get; set; } = string.Empty;
    public string CompilerTarget { get; set; } = string.Empty;
    public string ResourceDir { get; set; } = string.Empty;
    public string Sysroot { get; set; } = string.Empty;
    public List<string> Headers { get; set; } = [];
    public List<string> IncludeDirs { get; set; } = [];
    public List<string> SystemIncludeDirs { get; set; } = [];
    public List<string> Defines { get; set; } = [];
    public List<string> CompilerOptions { get; set; } = [];
}

internal static class Program
{
    private static readonly HashSet<string> UnsupportedMethodNames = new(StringComparer.Ordinal)
    {
        "typeof"
    };

    public static int Main(string[] args)
    {
        if (args.Length != 1)
        {
            Console.Error.WriteLine("Usage: MetaParser <precompile.json>");
            return 2;
        }

        var paramsPath = Path.GetFullPath(args[0]);
        if (!File.Exists(paramsPath))
        {
            Console.Error.WriteLine($"Params file not found: {paramsPath}");
            return 3;
        }

        var config = JsonSerializer.Deserialize<PrecompileParams>(File.ReadAllText(paramsPath), new JsonSerializerOptions
        {
            PropertyNameCaseInsensitive = true
        });

        if (config is null || string.IsNullOrWhiteSpace(config.RootDir) || string.IsNullOrWhiteSpace(config.OutputDir))
        {
            Console.Error.WriteLine("Invalid precompile params file.");
            return 4;
        }

        var rootDir = Path.GetFullPath(config.RootDir);
        var outputDir = Path.GetFullPath(config.OutputDir);
        Directory.CreateDirectory(outputDir);

        var types = new List<ReflectTypeInfo>();
        foreach (var header in config.Headers.Select(Path.GetFullPath).Distinct(StringComparer.OrdinalIgnoreCase))
        {
            if (!File.Exists(header) || !ShouldParseHeader(header))
                continue;

            try
            {
                Console.WriteLine($"[MetaParser] Parsing {header}");
                types.AddRange(ParseHeader(rootDir, header, config));
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[MetaParser] Failed to parse {header}: {ex.Message}");
                return 5;
            }
        }

        var orderedTypes = types
            .DistinctBy(t => t.QualifiedName)
            .OrderBy(t => t.Bases.Count)
            .ThenBy(t => t.HeaderPath, StringComparer.Ordinal)
            .ThenBy(t => t.QualifiedName, StringComparer.Ordinal)
            .ToList();

        var templateData = BuildTemplateData(orderedTypes, config.TargetName);
        var exeDir = AppContext.BaseDirectory;
        var templateDir = Path.Combine(exeDir, "Templates");

        var headerSession = new Dictionary<string, object?>
        {
            ["FunctionName"] = templateData["FunctionName"]
        };

        var headerText = RenderT4(Path.Combine(templateDir, "MetaGenerated.h.tt"), headerSession);
        var sourceText = RenderT4(Path.Combine(templateDir, "MetaGenerated.cpp.tt"), templateData);

        File.WriteAllText(Path.Combine(outputDir, "MetaGenerated.h"), headerText, new UTF8Encoding(false));
        File.WriteAllText(Path.Combine(outputDir, "MetaGenerated.cpp"), sourceText, new UTF8Encoding(false));

        Console.WriteLine($"[MetaParser] Target={config.TargetName}, Types={orderedTypes.Count}, Output={outputDir}");
        return 0;
    }

    private static Dictionary<string, object?> BuildTemplateData(IReadOnlyList<ReflectTypeInfo> types, string targetName)
    {
        var includes = types.Select(t => t.HeaderPath).Distinct(StringComparer.Ordinal).ToList();
        var sanitizedTargetName = SanitizeIdentifier(string.IsNullOrWhiteSpace(targetName) ? "UnknownTarget" : targetName);
        var functionName = $"RegisterReflectionTypes_{sanitizedTargetName}";

        var typeModels = types.Select(type => new Dictionary<string, object>
        {
            ["QualifiedName"] = type.QualifiedName,
            ["Bases"] = type.Bases.Select(b => (object)b.TypeName).Distinct().ToList(),
            ["Fields"] = type.Fields.Select(f => (object)new Dictionary<string, object>
            {
                ["Name"] = f.Name,
                ["TypeName"] = f.TypeName
            }).ToList(),
            ["Methods"] = type.Methods.Select(m => (object)new Dictionary<string, object>
            {
                ["Name"] = m.Name
            }).ToList()
        }).Cast<object?>().ToList();

        return new Dictionary<string, object?>
        {
            ["Includes"] = includes.Cast<object?>().ToList(),
            ["Types"] = typeModels,
            ["FunctionName"] = functionName
        };
    }

    private static string RenderT4(string templatePath, Dictionary<string, object?> sessionValues)
    {
        var generator = new TemplateGenerator();
        var sessionHost = (ITextTemplatingSessionHost)generator;
        sessionHost.Session = sessionHost.CreateSession();
        foreach (var kv in sessionValues)
            sessionHost.Session[kv.Key] = kv.Value;

        var outputPath = Path.GetTempFileName();
        try
        {
            var success = generator.ProcessTemplateAsync(templatePath, outputPath).GetAwaiter().GetResult();
            if (!success || generator.Errors.HasErrors)
            {
                var errors = string.Join(Environment.NewLine, generator.Errors.Cast<CompilerError>().Select(e => e.ToString()));
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

    private static bool ShouldParseHeader(string headerPath)
    {
        var ext = Path.GetExtension(headerPath);
        if (!string.Equals(ext, ".h", StringComparison.OrdinalIgnoreCase) && !string.Equals(ext, ".hpp", StringComparison.OrdinalIgnoreCase))
            return false;

        var text = File.ReadAllText(headerPath);
        return Regex.IsMatch(text, @"\bCLASS\s*\(", RegexOptions.CultureInvariant)
               || Regex.IsMatch(text, @"\bSTRUCT\s*\(", RegexOptions.CultureInvariant)
               || Regex.IsMatch(text, @"\bMeta\s*\(", RegexOptions.CultureInvariant)
               || Regex.IsMatch(text, @"\bMETA\s*\(", RegexOptions.CultureInvariant)
               || text.Contains("__cppast(", StringComparison.Ordinal)
               || text.Contains("__attribute__((annotate(", StringComparison.Ordinal);
    }

    private static IEnumerable<ReflectTypeInfo> ParseHeader(string rootDir, string headerPath, PrecompileParams config)
    {
        if (OperatingSystem.IsLinux())
            return ParseHeaderTextFallback(rootDir, headerPath).ToList();

        try
        {
            return ParseHeaderWithCppAst(rootDir, headerPath, config).ToList();
        }
        catch (Exception)
        {
            return ParseHeaderTextFallback(rootDir, headerPath).ToList();
        }
    }

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
                bases,
                fields,
                methods);
        }
    }

    private static IEnumerable<ReflectTypeInfo> ParseHeaderTextFallback(string rootDir, string headerPath)
    {
        var text = File.ReadAllText(headerPath);
        var matches = Regex.Matches(
            text,
            @"(?:\bMeta\s*\(\s*\)\s+(?<kind>class|struct)\s+(?:[\w_]+\s+)*(?<name>[A-Za-z_]\w*)|\b(?<macro>CLASS|STRUCT)\s*\(\s*(?<macroName>[A-Za-z_]\w*)\s*(?:,\s*[^)]*)?\))\s*(?::\s*(?<bases>[^{]+))?\s*\{",
            RegexOptions.CultureInvariant);

        foreach (Match match in matches.Cast<Match>())
        {
            var className = match.Groups["name"].Success
                ? match.Groups["name"].Value
                : match.Groups["macroName"].Value;
            if (string.IsNullOrWhiteSpace(className))
                continue;

            var bodyStart = match.Index + match.Length - 1;
            var body = ExtractBraceBody(text, bodyStart);
            var namespacePrefix = ExtractNamespaceFromText(text[..match.Index]);
            var fullTypeName = string.IsNullOrWhiteSpace(namespacePrefix)
                ? className
                : $"{namespacePrefix}::{className}";

            var defaultPublic = string.Equals(match.Groups["kind"].Value, "struct", StringComparison.Ordinal)
                || string.Equals(match.Groups["macro"].Value, "STRUCT", StringComparison.Ordinal);
            var members = ExtractMembersFromText(body, className, defaultPublic);

            yield return new ReflectTypeInfo(
                className,
                namespacePrefix,
                fullTypeName,
                ToGeneratedIncludePath(rootDir, Path.GetFullPath(headerPath)),
                ParseBasesFromText(match.Groups["bases"].Value, namespacePrefix),
                members.Fields,
                members.Methods);
        }
    }

    private static List<ReflectBaseInfo> ExtractBases(CppClass cls)
    {
        var result = new List<ReflectBaseInfo>();
        foreach (var baseType in cls.BaseTypes)
        {
            try
            {
                if (baseType.Visibility != CppVisibility.Public && baseType.Visibility != CppVisibility.Default)
                    continue;

                string typeName;
                if (baseType.Type is CppClass baseClass && !string.IsNullOrWhiteSpace(baseClass.FullName))
                    typeName = NormalizeTypeName(baseClass.FullName);
                else
                    typeName = NormalizeTypeName(baseType.Type.GetDisplayName());

                if (string.IsNullOrWhiteSpace(typeName))
                    continue;

                result.Add(new ReflectBaseInfo(typeName));
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[MetaParser] Warning: skipping base on {cls.FullName}: {ex.Message}");
            }
        }
        return result.DistinctBy(x => x.TypeName).ToList();
    }

    private static List<ReflectFieldInfo> ExtractFields(CppClass cls)
    {
        var result = new List<ReflectFieldInfo>();
        foreach (var field in cls.Fields)
        {
            try
            {
                if (field.Visibility != CppVisibility.Public && field.Visibility != CppVisibility.Default)
                    continue;
                if (field.StorageQualifier == CppStorageQualifier.Static)
                    continue;
                if (string.IsNullOrWhiteSpace(field.Name))
                    continue;

                var typeName = NormalizeTypeName(field.Type.GetDisplayName());
                if (string.IsNullOrWhiteSpace(typeName))
                    continue;

                result.Add(new ReflectFieldInfo(field.Name, typeName));
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[MetaParser] Warning: skipping field on {cls.FullName}: {ex.Message}");
            }
        }

        return result
            .DistinctBy(f => f.Name)
            .ToList();
    }

    private static List<ReflectMethodInfo> ExtractMethods(CppClass cls)
    {
        var candidateNames = new List<string>();
        foreach (var method in cls.Functions)
        {
            try
            {
                if (method.Visibility != CppVisibility.Public && method.Visibility != CppVisibility.Default)
                    continue;
                if (method.IsStatic || method.IsConstructor || method.IsDestructor)
                    continue;
                if (string.IsNullOrWhiteSpace(method.Name)
                    || method.Name.StartsWith("operator", StringComparison.Ordinal)
                    || !IsBindableMethodName(method.Name))
                    continue;

                candidateNames.Add(method.Name);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[MetaParser] Warning: skipping method discovery on {cls.FullName}: {ex.Message}");
            }
        }

        var overloadedMethodNames = candidateNames
            .GroupBy(name => name, StringComparer.Ordinal)
            .Where(g => g.Count() > 1)
            .Select(g => g.Key)
            .ToHashSet(StringComparer.Ordinal);

        return candidateNames
            .Where(name => !overloadedMethodNames.Contains(name))
            .Distinct(StringComparer.Ordinal)
            .Select(name => new ReflectMethodInfo(name))
            .ToList();
    }

    private static List<T> TryExtract<T>(Func<List<T>> extractor)
    {
        try
        {
            return extractor();
        }
        catch
        {
            return [];
        }
    }

    private static string ExtractBraceBody(string text, int openBraceIndex)
    {
        if (openBraceIndex < 0 || openBraceIndex >= text.Length || text[openBraceIndex] != '{')
            return string.Empty;

        var depth = 0;
        for (var i = openBraceIndex; i < text.Length; i++)
        {
            if (text[i] == '{')
                depth++;
            else if (text[i] == '}')
            {
                depth--;
                if (depth == 0)
                    return text[(openBraceIndex + 1)..i];
            }
        }

        return string.Empty;
    }

    private static string ExtractNamespaceFromText(string prefix)
    {
        var namespaceStack = new Stack<string[]>();
        foreach (Match match in Regex.Matches(prefix, @"namespace\s+([A-Za-z_][\w:]*)\s*\{|[{}]", RegexOptions.CultureInvariant).Cast<Match>())
        {
            if (match.Groups[1].Success)
            {
                namespaceStack.Push(match.Groups[1].Value
                    .Split("::", StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries));
            }
            else if (match.Value == "}" && namespaceStack.Count > 0)
            {
                namespaceStack.Pop();
            }
        }

        return string.Join("::", namespaceStack.Reverse().SelectMany(static parts => parts));
    }

    private static List<ReflectBaseInfo> ParseBasesFromText(string basesText, string namespacePrefix)
    {
        if (string.IsNullOrWhiteSpace(basesText))
            return [];

        return basesText
            .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Select(b => Regex.Replace(b, @"\b(public|protected|private|virtual)\b", string.Empty))
            .Select(NormalizeTypeName)
            .Select(b => QualifyTypeName(b, namespacePrefix))
            .Where(static b => !string.IsNullOrWhiteSpace(b))
            .Distinct(StringComparer.Ordinal)
            .Select(b => new ReflectBaseInfo(b))
            .ToList();
    }

    private static TextMemberParseResult ExtractMembersFromText(string body, string className, bool defaultPublic)
    {
        body = StripBlockComments(body);

        var fields = new List<ReflectFieldInfo>();
        var methods = new List<ReflectMethodInfo>();
        var methodNames = new List<string>();

        var currentAccess = defaultPublic ? "public" : "private";
        var currentStatement = new StringBuilder();
        var braceDepth = 0;

        foreach (var rawLine in body.Split(new[] { '\r', '\n' }, StringSplitOptions.None))
        {
            var line = StripInlineComments(rawLine).Trim();
            if (string.IsNullOrWhiteSpace(line))
                continue;

            if (braceDepth == 0)
            {
                if (line == "public:")
                {
                    currentAccess = "public";
                    continue;
                }
                if (line == "protected:")
                {
                    currentAccess = "protected";
                    continue;
                }
                if (line == "private:")
                {
                    currentAccess = "private";
                    continue;
                }
            }

            if (currentAccess != "public")
            {
                braceDepth += line.Count(static c => c == '{');
                braceDepth -= line.Count(static c => c == '}');
                continue;
            }

            currentStatement.Append(' ').Append(line);

            braceDepth += line.Count(static c => c == '{');
            braceDepth -= line.Count(static c => c == '}');

            if (braceDepth == 0 && (line.EndsWith(";", StringComparison.Ordinal) || line.EndsWith("}", StringComparison.Ordinal)))
            {
                var statement = Regex.Replace(currentStatement.ToString(), @"\s+", " ").Trim();
                currentStatement.Clear();

                if (statement.Contains(" enum ", StringComparison.Ordinal)
                    || statement.StartsWith("enum ", StringComparison.Ordinal)
                    || statement.StartsWith("using ", StringComparison.Ordinal)
                    || statement.StartsWith("typedef ", StringComparison.Ordinal)
                    || statement.StartsWith("friend ", StringComparison.Ordinal))
                {
                    continue;
                }

                if (statement.Contains('('))
                {
                    var nameMatches = Regex.Matches(statement, @"(?<name>[~A-Za-z_]\w*)\s*\([^;{}]*\)", RegexOptions.CultureInvariant);
                    if (nameMatches.Count == 0)
                        continue;

                    var methodName = nameMatches[^1].Groups["name"].Value;
                    if (string.IsNullOrWhiteSpace(methodName)
                        || methodName == className
                        || methodName == $"~{className}"
                        || methodName.StartsWith("operator", StringComparison.Ordinal)
                        || !IsBindableMethodName(methodName)
                        || statement.Contains(" static ", StringComparison.Ordinal)
                        || statement.StartsWith("static ", StringComparison.Ordinal))
                    {
                        continue;
                    }

                    methodNames.Add(methodName);
                    continue;
                }

                var fieldMatch = Regex.Match(
                    statement,
                    @"^(?<type>.+?)\s+(?<name>[A-Za-z_]\w*)(?:\s*=\s*.+)?;$",
                    RegexOptions.CultureInvariant);

                if (!fieldMatch.Success)
                    continue;

                var fieldType = NormalizeTypeName(fieldMatch.Groups["type"].Value);
                var fieldName = fieldMatch.Groups["name"].Value;

                if (string.IsNullOrWhiteSpace(fieldType)
                    || string.IsNullOrWhiteSpace(fieldName)
                    || fieldType.StartsWith("static ", StringComparison.Ordinal)
                    || fieldType.StartsWith("constexpr ", StringComparison.Ordinal)
                    || fieldType.StartsWith("inline ", StringComparison.Ordinal)
                    || fieldType.Contains(" static ", StringComparison.Ordinal)
                    || fieldType.Contains("typedef", StringComparison.Ordinal))
                {
                    continue;
                }

                fields.Add(new ReflectFieldInfo(fieldName, fieldType));
            }
        }

        var overloaded = methodNames
            .GroupBy(name => name, StringComparer.Ordinal)
            .Where(g => g.Count() > 1)
            .Select(g => g.Key)
            .ToHashSet(StringComparer.Ordinal);

        methods.AddRange(methodNames
            .Where(name => !overloaded.Contains(name))
            .Distinct(StringComparer.Ordinal)
            .Select(name => new ReflectMethodInfo(name)));

        return new TextMemberParseResult(
            fields
                .DistinctBy(f => f.Name)
                .ToList(),
            methods);
    }

    private static string StripInlineComments(string line)
    {
        var commentIndex = line.IndexOf("//", StringComparison.Ordinal);
        return commentIndex >= 0 ? line[..commentIndex] : line;
    }

    private static string StripBlockComments(string text)
        => Regex.Replace(text, @"/\*.*?\*/", string.Empty, RegexOptions.Singleline | RegexOptions.CultureInvariant);

    private static string QualifyTypeName(string typeName, string namespacePrefix)
    {
        if (string.IsNullOrWhiteSpace(typeName) || string.IsNullOrWhiteSpace(namespacePrefix) || typeName.Contains("::", StringComparison.Ordinal))
            return typeName;

        return $"{namespacePrefix}::{typeName}";
    }

    private static CppParserOptions CreateOptions(PrecompileParams config)
    {
        var options = new CppParserOptions
        {
            ParseTokenAttributes = false,
            ParseSystemIncludes = false
        };

        ConfigurePlatformDefaults(options, config);

        foreach (var includeDir in config.IncludeDirs.Where(static x => !string.IsNullOrWhiteSpace(x)).Distinct())
            options.IncludeFolders.Add(Path.GetFullPath(includeDir));

        var normalizedSystemIncludes = config.SystemIncludeDirs
            .Where(static x => !string.IsNullOrWhiteSpace(x))
            .Select(Path.GetFullPath)
            .Distinct()
            .ToList();

        if (!OperatingSystem.IsLinux())
        {
            foreach (var systemIncludeDir in normalizedSystemIncludes)
                options.SystemIncludeFolders.Add(systemIncludeDir);
        }

        foreach (var define in config.Defines.Where(static x => !string.IsNullOrWhiteSpace(x)).Distinct())
            options.Defines.Add(define);

        var compilerArgs = new List<string>();
        if (!string.IsNullOrWhiteSpace(config.CompilerTarget))
        {
            compilerArgs.Add($"--target={config.CompilerTarget}");
        }

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

        compilerArgs.AddRange(config.CompilerOptions.Where(static x => !string.IsNullOrWhiteSpace(x)));
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

    private static IEnumerable<CppClass> EnumerateNamespaceClasses(CppNamespace ns)
    {
        foreach (var cls in ns.Classes)
            yield return cls;
        foreach (var child in ns.Namespaces)
            foreach (var cls in EnumerateNamespaceClasses(child))
                yield return cls;
    }

    private static bool HasReflectionMarker(CppClass cls)
    {
        if (cls.Attributes.Any(IsReflectionAttribute) || cls.MetaAttributes.MetaList.Any())
            return true;

        var sourceFile = cls.SourceFile;
        if (string.IsNullOrWhiteSpace(sourceFile) || !File.Exists(sourceFile))
            return false;

        var text = File.ReadAllText(sourceFile);
        if (string.IsNullOrWhiteSpace(cls.Name))
            return false;

        var escapedName = Regex.Escape(cls.Name);
        return Regex.IsMatch(text, $@"\bMeta\s*\(\s*\)\s+class\b[\s\w:]*\b{escapedName}\b", RegexOptions.CultureInvariant)
               || Regex.IsMatch(text, $@"\bMeta\s*\(\s*\)\s+struct\b[\s\w:]*\b{escapedName}\b", RegexOptions.CultureInvariant)
               || Regex.IsMatch(text, $@"\bMETA\s*\(\s*\)\s+class\b[\s\w:]*\b{escapedName}\b", RegexOptions.CultureInvariant)
               || Regex.IsMatch(text, $@"\bMETA\s*\(\s*\)\s+struct\b[\s\w:]*\b{escapedName}\b", RegexOptions.CultureInvariant)
               || text.Contains($"CLASS({cls.Name}", StringComparison.Ordinal)
               || text.Contains($"STRUCT({cls.Name}", StringComparison.Ordinal);
    }

    private static bool IsReflectionAttribute(CppAttribute attribute)
        => attribute.Kind == AttributeKind.AnnotateAttribute
           || string.Equals(attribute.Name, "annotate", StringComparison.OrdinalIgnoreCase)
           || attribute.Name.Contains("annotate", StringComparison.OrdinalIgnoreCase);

    private static string ExtractNamespace(string fullName, string className)
    {
        if (string.IsNullOrWhiteSpace(fullName)) return string.Empty;
        var suffix = $"::{className}";
        return fullName.EndsWith(suffix, StringComparison.Ordinal) ? fullName[..^suffix.Length] : string.Empty;
    }

    private static string ToGeneratedIncludePath(string rootDir, string headerPath)
    {
        var runtimeDir = Path.Combine(rootDir, "Runtime");
        var rel = Path.GetRelativePath(runtimeDir, headerPath).Replace('\\', '/');
        var slash = rel.IndexOf('/');
        return slash >= 0 ? rel[(slash + 1)..] : rel;
    }

    private static string NormalizeTypeName(string typeName)
        => string.IsNullOrWhiteSpace(typeName) ? string.Empty : typeName.Replace("class ", string.Empty).Replace("struct ", string.Empty).Trim();

    private static bool IsBindableMethodName(string methodName)
        => !string.IsNullOrWhiteSpace(methodName)
           && Regex.IsMatch(methodName, @"^[A-Za-z_]\w*$", RegexOptions.CultureInvariant)
           && !UnsupportedMethodNames.Contains(methodName);

    private static string SanitizeIdentifier(string value)
        => Regex.Replace(value, @"[^A-Za-z0-9_]", "_", RegexOptions.CultureInvariant);

    private static StringComparison PathComparison => OperatingSystem.IsWindows()
        ? StringComparison.OrdinalIgnoreCase
        : StringComparison.Ordinal;
}

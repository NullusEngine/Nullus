using System.CodeDom.Compiler;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using CppAst;
using Mono.TextTemplating;
using Microsoft.VisualStudio.TextTemplating;

internal sealed record ReflectFieldInfo(string Name, string TypeName, string GetterExpression, string SetterExpression, bool IsPrivate);
internal sealed record ReflectMethodInfo(string Name, string PointerExpression, bool IsStatic, bool IsPrivate);
internal sealed record ReflectBaseInfo(string TypeName);

internal sealed record ReflectTypeInfo(
    string ClassName,
    string NamespacePrefix,
    string FullTypeName,
    string HeaderPath,
    string SourceFilePath,
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
    public string SourceDir { get; set; } = string.Empty;
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
        "typeof",
        "GENERATED_BODY",
        "NLS_GENERATED_BODY"
    };

    private static readonly HashSet<string> BuiltinTypeNames = new(StringComparer.Ordinal)
    {
        "void",
        "bool",
        "char",
        "signed char",
        "unsigned char",
        "short",
        "unsigned short",
        "int",
        "unsigned int",
        "long",
        "unsigned long",
        "long long",
        "unsigned long long",
        "float",
        "double",
        "long double",
        "size_t",
        "std::size_t",
        "uint8_t",
        "uint16_t",
        "uint32_t",
        "uint64_t",
        "int8_t",
        "int16_t",
        "int32_t",
        "int64_t"
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

        if (config is null || string.IsNullOrWhiteSpace(config.RootDir) || string.IsNullOrWhiteSpace(config.SourceDir) || string.IsNullOrWhiteSpace(config.OutputDir))
        {
            Console.Error.WriteLine("Invalid precompile params file.");
            return 4;
        }

        var rootDir = Path.GetFullPath(config.RootDir);
        var outputDir = Path.GetFullPath(config.OutputDir);
        Directory.CreateDirectory(outputDir);
        EnsureGeneratedHeaderStubs(config.Headers.Select(Path.GetFullPath).Distinct(StringComparer.OrdinalIgnoreCase), rootDir, outputDir);

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

        var orderedTypes = MergeReflectTypes(types)
            .OrderBy(t => t.HeaderPath, StringComparer.Ordinal)
            .ThenBy(t => t.QualifiedName, StringComparer.Ordinal)
            .ToList();

        var templateData = BuildTemplateData(orderedTypes, config.TargetName);
        var exeDir = AppContext.BaseDirectory;
        var templateDir = Path.Combine(exeDir, "Templates");
        var sanitizedTargetName = (string)(templateData["SanitizedTargetName"] ?? "UnknownTarget");
        var generatedSourceIncludes = new List<object?>();

        foreach (var headerGroup in orderedTypes.GroupBy(type => type.HeaderPath, StringComparer.Ordinal))
        {
            var relativeGeneratedBase = GetGeneratedRelativeBasePath(headerGroup.Key);
            var generatedHeaderPath = Path.Combine(outputDir, $"{relativeGeneratedBase}.generated.h");
            var generatedSourcePath = Path.Combine(outputDir, $"{relativeGeneratedBase}.generated.cpp");
            Directory.CreateDirectory(Path.GetDirectoryName(generatedHeaderPath)!);
            Directory.CreateDirectory(Path.GetDirectoryName(generatedSourcePath)!);

            var headerTemplateData = BuildHeaderTemplateData(headerGroup.ToList());
            var generatedHeaderText = RenderT4(Path.Combine(templateDir, "HeaderGenerated.h.tt"), headerTemplateData);
            var generatedSourceText = RenderT4(Path.Combine(templateDir, "HeaderGenerated.cpp.tt"), headerTemplateData);

            File.WriteAllText(generatedHeaderPath, generatedHeaderText, new UTF8Encoding(false));
            File.WriteAllText(generatedSourcePath, generatedSourceText, new UTF8Encoding(false));
            generatedSourceIncludes.Add($"{relativeGeneratedBase}.generated.cpp".Replace('\\', '/'));
        }

        templateData["GeneratedSourceIncludes"] = generatedSourceIncludes;

        var headerSession = new Dictionary<string, object?>
        {
            ["LinkFunctionName"] = templateData["LinkFunctionName"]
        };

        var headerText = RenderT4(Path.Combine(templateDir, "MetaGenerated.h.tt"), headerSession);
        var sourceText = RenderT4(Path.Combine(templateDir, "MetaGenerated.cpp.tt"), templateData);

        File.WriteAllText(Path.Combine(outputDir, "MetaGenerated.h"), headerText, new UTF8Encoding(false));
        File.WriteAllText(Path.Combine(outputDir, $"{sanitizedTargetName}_MetaGenerated.h"), headerText, new UTF8Encoding(false));
        File.WriteAllText(Path.Combine(outputDir, "MetaGenerated.cpp"), sourceText, new UTF8Encoding(false));

        Console.WriteLine($"[MetaParser] Target={config.TargetName}, Types={orderedTypes.Count}, Output={outputDir}");
        return 0;
    }

    private static Dictionary<string, object?> BuildTemplateData(IReadOnlyList<ReflectTypeInfo> types, string targetName)
    {
        var sanitizedTargetName = SanitizeIdentifier(string.IsNullOrWhiteSpace(targetName) ? "UnknownTarget" : targetName);
        var linkFunctionName = $"LinkReflectionTypes_{sanitizedTargetName}";

        return new Dictionary<string, object?>
        {
            ["LinkFunctionName"] = linkFunctionName,
            ["SanitizedTargetName"] = sanitizedTargetName
        };
    }

    private static Dictionary<string, object?> BuildHeaderTemplateData(IReadOnlyList<ReflectTypeInfo> types)
    {
        var headerPath = types[0].HeaderPath;
        var fileId = BuildGeneratedFileId(headerPath);
        var generatedBodyLines = FindGeneratedBodyLines(types[0].SourceFilePath);
        var headerIncludePath = headerPath.Replace('\\', '/');
        var generatedHeaderIncludePath = $"{GetGeneratedRelativeBasePath(headerPath)}.generated.h".Replace('\\', '/');
        var typeModels = new List<object?>();
        var privateTypeModels = new List<object>();

        var generatedBodyLineIndex = 0;
        foreach (var type in types)
        {
            int? generatedBodyLine = generatedBodyLineIndex < generatedBodyLines.Count
                ? generatedBodyLines[generatedBodyLineIndex++]
                : null;

            var fieldModels = type.Fields
                .Select((field, index) => new Dictionary<string, object>
                {
                    ["Name"] = field.Name,
                    ["TypeName"] = field.TypeName,
                    ["GetterExpression"] = field.GetterExpression,
                    ["SetterExpression"] = field.SetterExpression,
                    ["IsPrivate"] = field.IsPrivate,
                    ["AccessorName"] = BuildPrivateFieldAccessorName(field.Name, index)
                })
                .ToList();

            var methodModels = type.Methods
                .Select((method, index) => new Dictionary<string, object>
                {
                    ["Name"] = method.Name,
                    ["PointerExpression"] = method.PointerExpression,
                    ["IsStatic"] = method.IsStatic,
                    ["IsPrivate"] = method.IsPrivate,
                    ["AccessorName"] = BuildPrivateMethodAccessorName(method.Name, index)
                })
                .ToList();

            typeModels.Add(new Dictionary<string, object>
            {
                ["ClassName"] = type.ClassName,
                ["QualifiedName"] = type.QualifiedName,
                ["AccessStructName"] = BuildPrivateAccessStructName(type.QualifiedName),
                ["RegisterFunctionName"] = BuildRegisterFunctionName(type.QualifiedName),
                ["RegistrarClassName"] = BuildRegistrarClassName(type.QualifiedName),
                ["GeneratedBodyMacroName"] = generatedBodyLine.HasValue
                    ? BuildGeneratedBodyMacroName(fileId, generatedBodyLine.Value)
                    : string.Empty,
                ["Bases"] = type.Bases.Select(b => (object)b.TypeName).Distinct().ToList(),
                ["Fields"] = fieldModels.Cast<object>().ToList(),
                ["Methods"] = methodModels.Cast<object>().ToList()
            });

            if (fieldModels.Any(field => (bool)field["IsPrivate"]) || methodModels.Any(method => (bool)method["IsPrivate"]))
            {
                privateTypeModels.Add(new Dictionary<string, object>
                {
                    ["ClassName"] = type.ClassName,
                    ["QualifiedName"] = type.QualifiedName,
                    ["AccessStructName"] = BuildPrivateAccessStructName(type.QualifiedName),
                    ["RegisterFunctionName"] = BuildRegisterFunctionName(type.QualifiedName),
                    ["RegistrarClassName"] = BuildRegistrarClassName(type.QualifiedName),
                    ["GeneratedBodyMacroName"] = generatedBodyLine.HasValue
                        ? BuildGeneratedBodyMacroName(fileId, generatedBodyLine.Value)
                        : string.Empty,
                    ["PrivateFields"] = fieldModels
                        .Where(field => (bool)field["IsPrivate"])
                        .Select(field => (object)new Dictionary<string, object>
                        {
                            ["Name"] = field["Name"],
                            ["AccessorName"] = field["AccessorName"]
                        }).ToList(),
                    ["PrivateMethods"] = methodModels
                        .Where(method => (bool)method["IsPrivate"])
                        .Select(method => (object)new Dictionary<string, object>
                        {
                            ["Name"] = method["Name"],
                            ["AccessorName"] = method["AccessorName"],
                            ["PointerExpression"] = method["PointerExpression"]
                        }).ToList()
                });
            }
        }

        return new Dictionary<string, object?>
        {
            ["HeaderPath"] = headerPath,
            ["FileId"] = fileId,
            ["HeaderIncludePath"] = headerIncludePath,
            ["GeneratedHeaderIncludePath"] = generatedHeaderIncludePath,
            ["RequiresGeneratedHeaderIncludeInSource"] = generatedBodyLines.Count == 0,
            ["Types"] = typeModels,
            ["PrivateTypes"] = privateTypeModels
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
        return ContainsReflectedDeclaration(text);
    }

    private static void EnsureGeneratedHeaderStubs(IEnumerable<string> headers, string rootDir, string outputDir)
    {
        foreach (var headerPath in headers)
        {
            if (!File.Exists(headerPath) || !ShouldParseHeader(headerPath))
                continue;

            var includePath = ToGeneratedIncludePath(rootDir, headerPath);
            var generatedHeaderPath = Path.Combine(outputDir, $"{GetGeneratedRelativeBasePath(includePath)}.generated.h");
            var generatedHeaderDir = Path.GetDirectoryName(generatedHeaderPath);
            if (!string.IsNullOrWhiteSpace(generatedHeaderDir))
                Directory.CreateDirectory(generatedHeaderDir);

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

    private static IEnumerable<ReflectTypeInfo> ParseHeader(string rootDir, string headerPath, PrecompileParams config)
    {
        var types = new List<ReflectTypeInfo>();
        var headerText = File.ReadAllText(headerPath);
        var hasReflectedTypeBodies = ContainsGeneratedBody(headerText);
        var textFallbackTypes = hasReflectedTypeBodies
            ? ParseHeaderFromText(rootDir, headerPath, headerText).ToList()
            : [];

        try
        {
            types.AddRange(ParseHeaderWithCppAst(rootDir, headerPath, config));
            if (hasReflectedTypeBodies)
            {
                types.AddRange(textFallbackTypes);
            }
        }
        catch (Exception ex) when (hasReflectedTypeBodies)
        {
            types.AddRange(textFallbackTypes);
            if (textFallbackTypes.Count == 0)
            {
                Console.Error.WriteLine($"[MetaParser] Warning: CppAst fallback for {headerPath}: {ex.Message}");
            }
        }

        types.AddRange(ParseExternalReflectionDeclarations(rootDir, headerPath));
        return MergeReflectTypes(types);
    }

    private static IEnumerable<ReflectTypeInfo> ParseHeaderFromText(string rootDir, string headerPath, string headerText)
    {
        var includePath = ToGeneratedIncludePath(rootDir, Path.GetFullPath(headerPath));
        var text = SanitizeTextForMacroParsing(headerText);
        var classMatches = Regex.Matches(
            text,
            @"\b(?<kind>class|struct)\s+(?:[A-Za-z_]\w*\s+)*(?<name>[A-Za-z_]\w*)\s*(?:\:\s*(?<bases>[^{]+))?\s*\{",
            RegexOptions.CultureInvariant);

        foreach (Match match in classMatches.Cast<Match>())
        {
            var kind = match.Groups["kind"].Value;
            var className = match.Groups["name"].Value;
            if (string.IsNullOrWhiteSpace(className))
                continue;

            var openBraceIndex = text.IndexOf('{', match.Index);
            if (openBraceIndex < 0)
                continue;

            var body = ExtractBraceBody(text, openBraceIndex);
            if (!ContainsGeneratedBody(body))
                continue;

            var namespacePrefix = ExtractNamespaceFromText(text[..match.Index]);
            var fullTypeName = string.IsNullOrWhiteSpace(namespacePrefix)
                ? className
                : $"{namespacePrefix}::{className}";
            var bases = ParseBasesFromText(match.Groups["bases"].Value, namespacePrefix);
            var members = ExtractMembersFromText(body, fullTypeName, string.Equals(kind, "struct", StringComparison.Ordinal));

            yield return new ReflectTypeInfo(
                className,
                namespacePrefix,
                fullTypeName,
                includePath,
                Path.GetFullPath(headerPath),
                bases,
                members.Fields,
                members.Methods);
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
                normalizedHeader,
                bases,
                fields,
                methods);
        }
    }

    private static IEnumerable<ReflectTypeInfo> ParseExternalReflectionDeclarations(string rootDir, string headerPath)
    {
        var text = SanitizeTextForMacroParsing(File.ReadAllText(headerPath));
        if (!text.Contains("MetaExternal", StringComparison.Ordinal)
            && !text.Contains("REFLECT_EXTERNAL", StringComparison.Ordinal))
        {
            return [];
        }

        var includePath = ToGeneratedIncludePath(rootDir, Path.GetFullPath(headerPath));
        var declarations = new List<ReflectTypeInfo>();

        foreach (Match match in Regex.Matches(text, @"\bMetaExternal\s*\(\s*(?<type>[^)]+?)\s*\)", RegexOptions.CultureInvariant).Cast<Match>())
        {
            var fullTypeName = NormalizeTypeName(match.Groups["type"].Value);
            if (string.IsNullOrWhiteSpace(fullTypeName))
                continue;

            declarations.Add(CreateExternalReflectType(fullTypeName, includePath, Path.GetFullPath(headerPath), [], [], []));
        }

        foreach (var invocation in ExtractMacroInvocations(text, "REFLECT_EXTERNAL"))
        {
            var args = SplitTopLevel(invocation, ',');
            if (args.Count == 0)
                continue;

            var fullTypeName = NormalizeTypeName(args[0]);
            if (string.IsNullOrWhiteSpace(fullTypeName))
                continue;

            var namespacePrefix = ExtractNamespace(fullTypeName, ExtractSimpleClassName(fullTypeName));
            var bases = new List<ReflectBaseInfo>();
            var fields = new List<ReflectFieldInfo>();
            var methods = new List<ReflectMethodInfo>();

            foreach (var section in args.Skip(1))
            {
                if (!TryParseMacroInvocation(section, out var sectionName, out var sectionBody))
                    continue;

                switch (sectionName)
                {
                case "Bases":
                    bases.AddRange(ParseExternalBases(sectionBody, namespacePrefix));
                    break;
                case "Fields":
                    fields.AddRange(ParseExternalFields(sectionBody, fullTypeName, namespacePrefix));
                    break;
                case "Methods":
                    methods.AddRange(ParseExternalMethods(sectionBody, fullTypeName));
                    break;
                case "StaticMethods":
                    methods.AddRange(ParseExternalStaticMethods(sectionBody, fullTypeName));
                    break;
                default:
                    Console.Error.WriteLine($"[MetaParser] Warning: unknown external reflection section '{sectionName}' in {headerPath}");
                    break;
                }
            }

            declarations.Add(CreateExternalReflectType(fullTypeName, includePath, Path.GetFullPath(headerPath), bases, fields, methods));
        }

        return declarations;
    }

    private static List<ReflectTypeInfo> MergeReflectTypes(IEnumerable<ReflectTypeInfo> types)
    {
        var merged = new Dictionary<string, ReflectTypeInfo>(StringComparer.Ordinal);

        foreach (var type in types.Where(static t => !string.IsNullOrWhiteSpace(t.QualifiedName)))
        {
            if (!merged.TryGetValue(type.QualifiedName, out var existing))
            {
                merged[type.QualifiedName] = type with
                {
                    Bases = type.Bases.DistinctBy(b => b.TypeName).ToList(),
                    Fields = type.Fields.DistinctBy(f => f.Name).ToList(),
                    Methods = type.Methods.DistinctBy(m => $"{m.IsStatic}:{m.Name}:{m.PointerExpression}").ToList()
                };
                continue;
            }

            merged[type.QualifiedName] = existing with
            {
                Bases = existing.Bases
                    .Concat(type.Bases)
                    .DistinctBy(b => b.TypeName)
                    .ToList(),
                Fields = existing.Fields
                    .Concat(type.Fields)
                    .DistinctBy(f => f.Name)
                    .ToList(),
                Methods = existing.Methods
                    .Concat(type.Methods)
                    .DistinctBy(m => $"{m.IsStatic}:{m.Name}:{m.PointerExpression}")
                    .ToList()
            };
        }

        return merged.Values.ToList();
    }

    private static ReflectTypeInfo CreateExternalReflectType(
        string fullTypeName,
        string includePath,
        string sourceFilePath,
        List<ReflectBaseInfo> bases,
        List<ReflectFieldInfo> fields,
        List<ReflectMethodInfo> methods)
    {
        var className = ExtractSimpleClassName(fullTypeName);
        return new ReflectTypeInfo(
            className,
            ExtractNamespace(fullTypeName, className),
            fullTypeName,
            includePath,
            sourceFilePath,
            bases.DistinctBy(b => b.TypeName).ToList(),
            fields.DistinctBy(f => f.Name).ToList(),
            methods.DistinctBy(m => $"{m.IsStatic}:{m.Name}:{m.PointerExpression}").ToList());
    }

    private static List<ReflectBaseInfo> ParseExternalBases(string sectionBody, string namespacePrefix)
        => SplitTopLevel(sectionBody, ',')
            .Select(NormalizeTypeName)
            .Select(typeName => QualifyTypeName(typeName, namespacePrefix))
            .Where(static typeName => !string.IsNullOrWhiteSpace(typeName))
            .Distinct(StringComparer.Ordinal)
            .Select(typeName => new ReflectBaseInfo(typeName))
            .ToList();

    private static List<ReflectFieldInfo> ParseExternalFields(string sectionBody, string fullTypeName, string namespacePrefix)
    {
        var fields = new List<ReflectFieldInfo>();

        foreach (var entry in SplitTopLevel(sectionBody, ','))
        {
            if (!TryParseMacroInvocation(entry, out var macroName, out var macroBody)
                || (macroName != "REFLECT_FIELD" && macroName != "REFLECT_PRIVATE_FIELD"))
                continue;

            var args = SplitTopLevel(macroBody, ',');
            if (args.Count != 2)
                continue;

            var typeName = QualifyTypeName(NormalizeTypeName(args[0]), namespacePrefix);
            var fieldName = NormalizeRegistrationName(args[1]);
            if (string.IsNullOrWhiteSpace(typeName) || string.IsNullOrWhiteSpace(fieldName))
                continue;

            fields.Add(new ReflectFieldInfo(
                fieldName,
                typeName,
                $"&{fullTypeName}::{fieldName}",
                $"&{fullTypeName}::{fieldName}",
                macroName == "REFLECT_PRIVATE_FIELD"));
        }

        return fields
            .DistinctBy(field => field.Name)
            .ToList();
    }

    private static List<ReflectMethodInfo> ParseExternalMethods(string sectionBody, string fullTypeName)
    {
        var methods = new List<ReflectMethodInfo>();

        foreach (var entry in SplitTopLevel(sectionBody, ','))
        {
            if (!TryParseMacroInvocation(entry, out var macroName, out var macroBody))
                continue;

            switch (macroName)
            {
            case "REFLECT_METHOD":
            case "REFLECT_PRIVATE_METHOD":
            {
                var methodName = NormalizeRegistrationName(macroBody);
                if (string.IsNullOrWhiteSpace(methodName))
                    continue;

                methods.Add(new ReflectMethodInfo(
                    methodName,
                    $"&{fullTypeName}::{methodName}",
                    false,
                    macroName == "REFLECT_PRIVATE_METHOD"));
                break;
            }
            case "REFLECT_METHOD_EX":
            case "REFLECT_PRIVATE_METHOD_EX":
            {
                var args = SplitTopLevel(macroBody, ',');
                if (args.Count != 2)
                    continue;

                var methodName = NormalizeRegistrationName(args[0]);
                var pointerExpression = args[1].Trim();
                if (string.IsNullOrWhiteSpace(methodName) || string.IsNullOrWhiteSpace(pointerExpression))
                    continue;

                methods.Add(new ReflectMethodInfo(
                    methodName,
                    pointerExpression,
                    false,
                    macroName == "REFLECT_PRIVATE_METHOD_EX"));
                break;
            }
            }
        }

        return methods
            .DistinctBy(method => $"{method.IsStatic}:{method.Name}:{method.PointerExpression}")
            .ToList();
    }

    private static List<ReflectMethodInfo> ParseExternalStaticMethods(string sectionBody, string fullTypeName)
    {
        var methods = new List<ReflectMethodInfo>();

        foreach (var entry in SplitTopLevel(sectionBody, ','))
        {
            if (!TryParseMacroInvocation(entry, out var macroName, out var macroBody)
                || (macroName != "REFLECT_STATIC_METHOD" && macroName != "REFLECT_PRIVATE_STATIC_METHOD"))
                continue;

            var args = SplitTopLevel(macroBody, ',');
            if (args.Count != 2)
                continue;

            var methodName = NormalizeRegistrationName(args[0]);
            var pointerExpression = args[1].Trim();
            if (string.IsNullOrWhiteSpace(methodName) || string.IsNullOrWhiteSpace(pointerExpression))
                continue;

            methods.Add(new ReflectMethodInfo(
                methodName,
                pointerExpression,
                true,
                macroName == "REFLECT_PRIVATE_STATIC_METHOD"));
        }

        return methods
            .DistinctBy(method => $"{method.IsStatic}:{method.Name}:{method.PointerExpression}")
            .ToList();
    }

    private static List<string> ExtractMacroInvocations(string text, string macroName)
    {
        var invocations = new List<string>();
        var startIndex = 0;

        while (startIndex < text.Length)
        {
            var match = Regex.Match(text[startIndex..], $@"\b{Regex.Escape(macroName)}\s*\(", RegexOptions.CultureInvariant);
            if (!match.Success)
                break;

            var macroIndex = startIndex + match.Index;
            var openParenIndex = text.IndexOf('(', macroIndex + macroName.Length);
            if (openParenIndex < 0)
                break;

            invocations.Add(ExtractDelimitedBody(text, openParenIndex, '(', ')'));
            startIndex = SkipDelimitedBody(text, openParenIndex, '(', ')');
        }

        return invocations;
    }

    private static string SanitizeTextForMacroParsing(string text)
        => string.Join(
            Environment.NewLine,
            StripBlockComments(text)
                .Split('\n')
                .Select(StripInlineComments));

    private static bool ContainsReflectedDeclaration(string text)
        => ContainsGeneratedBody(text)
           || text.Contains("MetaExternal", StringComparison.Ordinal)
           || text.Contains("REFLECT_EXTERNAL", StringComparison.Ordinal);

    private static bool ContainsGeneratedBody(string text)
        => text.Contains("GENERATED_BODY(", StringComparison.Ordinal);

    private static List<string> SplitTopLevel(string text, char separator)
    {
        var result = new List<string>();
        var current = new StringBuilder();
        var parenDepth = 0;
        var angleDepth = 0;
        var braceDepth = 0;
        var bracketDepth = 0;
        var inString = false;
        char stringDelimiter = '\0';

        foreach (var ch in text)
        {
            if (inString)
            {
                current.Append(ch);
                if (ch == stringDelimiter)
                    inString = false;
                continue;
            }

            switch (ch)
            {
            case '\'':
            case '"':
                inString = true;
                stringDelimiter = ch;
                current.Append(ch);
                break;
            case '(':
                parenDepth++;
                current.Append(ch);
                break;
            case ')':
                parenDepth--;
                current.Append(ch);
                break;
            case '<':
                angleDepth++;
                current.Append(ch);
                break;
            case '>':
                if (angleDepth > 0)
                    angleDepth--;
                current.Append(ch);
                break;
            case '{':
                braceDepth++;
                current.Append(ch);
                break;
            case '}':
                braceDepth--;
                current.Append(ch);
                break;
            case '[':
                bracketDepth++;
                current.Append(ch);
                break;
            case ']':
                bracketDepth--;
                current.Append(ch);
                break;
            default:
                if (ch == separator && parenDepth == 0 && angleDepth == 0 && braceDepth == 0 && bracketDepth == 0)
                {
                    var part = current.ToString().Trim();
                    if (!string.IsNullOrWhiteSpace(part))
                        result.Add(part);
                    current.Clear();
                }
                else
                {
                    current.Append(ch);
                }
                break;
            }
        }

        var trailing = current.ToString().Trim();
        if (!string.IsNullOrWhiteSpace(trailing))
            result.Add(trailing);

        return result;
    }

    private static bool TryParseMacroInvocation(string text, out string macroName, out string macroBody)
    {
        macroName = string.Empty;
        macroBody = string.Empty;

        var trimmed = text.Trim();
        var openParenIndex = trimmed.IndexOf('(');
        if (openParenIndex <= 0 || !trimmed.EndsWith(")", StringComparison.Ordinal))
            return false;

        macroName = trimmed[..openParenIndex].Trim();
        macroBody = trimmed[(openParenIndex + 1)..^1].Trim();
        return !string.IsNullOrWhiteSpace(macroName);
    }

    private static string ExtractDelimitedBody(string text, int openIndex, char openChar, char closeChar)
    {
        if (openIndex < 0 || openIndex >= text.Length || text[openIndex] != openChar)
            return string.Empty;

        var start = openIndex + 1;
        var depth = 1;
        for (var index = start; index < text.Length; index++)
        {
            if (text[index] == openChar)
                depth++;
            else if (text[index] == closeChar)
            {
                depth--;
                if (depth == 0)
                    return text[start..index];
            }
        }

        return string.Empty;
    }

    private static int SkipDelimitedBody(string text, int openIndex, char openChar, char closeChar)
    {
        if (openIndex < 0 || openIndex >= text.Length || text[openIndex] != openChar)
            return text.Length;

        var depth = 1;
        for (var index = openIndex + 1; index < text.Length; index++)
        {
            if (text[index] == openChar)
                depth++;
            else if (text[index] == closeChar)
            {
                depth--;
                if (depth == 0)
                    return index + 1;
            }
        }

        return text.Length;
    }

    private static string ExtractSimpleClassName(string fullTypeName)
    {
        var separatorIndex = fullTypeName.LastIndexOf("::", StringComparison.Ordinal);
        return separatorIndex >= 0 ? fullTypeName[(separatorIndex + 2)..] : fullTypeName;
    }

    private static string NormalizeRegistrationName(string text)
    {
        var trimmed = text.Trim();
        if (trimmed.Length >= 2
            && ((trimmed[0] == '"' && trimmed[^1] == '"')
                || (trimmed[0] == '\'' && trimmed[^1] == '\'')))
        {
            return trimmed[1..^1].Trim();
        }

        return trimmed;
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

                result.Add(new ReflectFieldInfo(
                    field.Name,
                    typeName,
                    $"&{cls.FullName}::{field.Name}",
                    $"&{cls.FullName}::{field.Name}",
                    false));
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
            .Select(name => new ReflectMethodInfo(name, $"&{cls.FullName}::{name}", false, false))
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

    private static TextMemberParseResult ExtractMembersFromText(string body, string fullTypeName, bool defaultPublic)
    {
        body = StripBlockComments(body);

        var fields = new List<ReflectFieldInfo>();
        var methods = new List<ReflectMethodInfo>();
        var methodNames = new List<string>();
        var className = ExtractSimpleClassName(fullTypeName);
        var namespacePrefix = ExtractNamespace(fullTypeName, ExtractSimpleClassName(fullTypeName));

        var currentAccess = defaultPublic ? "public" : "private";
        var currentStatement = new StringBuilder();
        var braceDepth = 0;

        foreach (var rawLine in body.Split(new[] { '\r', '\n' }, StringSplitOptions.None))
        {
            var line = StripInlineComments(rawLine).Trim();
            if (string.IsNullOrWhiteSpace(line))
                continue;

            if (Regex.IsMatch(line, @"^(?:GENERATED_BODY|NLS_GENERATED_BODY)\s*\([^)]*\)\s*;?$", RegexOptions.CultureInvariant))
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

                var fieldType = QualifyTypeName(NormalizeTypeName(fieldMatch.Groups["type"].Value), namespacePrefix);
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

                fields.Add(new ReflectFieldInfo(
                    fieldName,
                    fieldType,
                    $"&{fullTypeName}::{fieldName}",
                    $"&{fullTypeName}::{fieldName}",
                    false));
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
            .Select(name => new ReflectMethodInfo(name, $"&{fullTypeName}::{name}", false, false)));

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
        if (BuiltinTypeNames.Contains(typeName))
            return typeName;
        if (typeName.IndexOfAny(new[] { ' ', '&', '*', '<', '>', '[', ']', '(', ')', ',' }) >= 0)
            return typeName;

        return $"{namespacePrefix}::{typeName}";
    }

    private static string GetGeneratedRelativeBasePath(string headerPath)
    {
        return Path.ChangeExtension(headerPath.Replace('/', Path.DirectorySeparatorChar), null) ?? headerPath;
    }

    private static string BuildGeneratedFileId(string headerPath)
        => $"NLS_FID_{SanitizeIdentifier(headerPath.Replace('\\', '_').Replace('/', '_').Replace('.', '_'))}";

    private static string BuildGeneratedBodyMacroName(string fileId, int line)
        => $"{fileId}_{line}_GENERATED_BODY";

    private static List<int> FindGeneratedBodyLines(string sourceFilePath)
    {
        if (string.IsNullOrWhiteSpace(sourceFilePath) || !File.Exists(sourceFilePath))
            return [];

        var text = File.ReadAllText(sourceFilePath);
        return Regex.Matches(text, @"\b(?:GENERATED_BODY|NLS_GENERATED_BODY)\s*\(", RegexOptions.CultureInvariant)
            .Cast<Match>()
            .Select(match => 1 + text.Take(match.Index).Count(ch => ch == '\n'))
            .ToList();
    }

    private static string BuildPrivateAccessStructName(string qualifiedName)
        => $"PrivateAccess_{SanitizeIdentifier(qualifiedName)}";

    private static string BuildPrivateFieldAccessorName(string fieldName, int index)
        => $"Field_{SanitizeIdentifier(fieldName)}_{index}";

    private static string BuildPrivateMethodAccessorName(string methodName, int index)
        => $"Method_{SanitizeIdentifier(methodName)}_{index}";

    private static string BuildRegisterFunctionName(string qualifiedName)
        => $"RegisterType_{SanitizeIdentifier(qualifiedName)}";

    private static string BuildRegistrarClassName(string qualifiedName)
        => $"StaticTypeRegister_{SanitizeIdentifier(qualifiedName)}";

    private static CppParserOptions CreateOptions(PrecompileParams config)
    {
        var options = new CppParserOptions
        {
            ParseTokenAttributes = true,
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
        var hasAttribute = cls.Attributes.Any(IsReflectionAttribute);
#pragma warning disable CS0618
        var hasTokenAttribute = cls.TokenAttributes.Any(IsReflectionAttribute);
#pragma warning restore CS0618
        var hasMetaAttribute = cls.MetaAttributes.MetaList.Any(IsReflectionMetaAttribute);

        if (hasAttribute || hasTokenAttribute || hasMetaAttribute)
        {
            return true;
        }

        return false;
    }

    private static bool IsReflectionAttribute(CppAttribute attribute)
    {
        if (!(attribute.Kind == AttributeKind.AnnotateAttribute
              || string.Equals(attribute.Name, "annotate", StringComparison.OrdinalIgnoreCase)
              || attribute.Name.Contains("annotate", StringComparison.OrdinalIgnoreCase)))
        {
            return false;
        }

        return string.IsNullOrWhiteSpace(attribute.Arguments)
               || ContainsReflectionMarker(attribute.Arguments)
               || ContainsReflectionMarker(attribute.ToString());
    }

    private static bool IsReflectionMetaAttribute(MetaAttribute attribute)
    {
        if (attribute is null)
            return false;

        if (ContainsReflectionMarker(attribute.FeatureName))
            return true;

        return attribute.ArgumentMap.Keys.Any(ContainsReflectionMarker)
               || attribute.ArgumentMap.Values.Any(value => ContainsReflectionMarker(Convert.ToString(value)));
    }

    private static bool ContainsReflectionMarker(string? text)
        => !string.IsNullOrWhiteSpace(text)
           && text.IndexOf("Reflection", StringComparison.OrdinalIgnoreCase) >= 0;

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

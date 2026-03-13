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
            .OrderBy(t => t.HeaderPath, StringComparer.Ordinal)
            .ThenBy(t => t.QualifiedName, StringComparer.Ordinal)
            .ToList();

        var templateData = BuildTemplateData(orderedTypes);
        var exeDir = AppContext.BaseDirectory;
        var templateDir = Path.Combine(exeDir, "Templates");

        var headerText = RenderT4(Path.Combine(templateDir, "MetaGenerated.h.tt"), new Dictionary<string, object?>());
        var sourceText = RenderT4(Path.Combine(templateDir, "MetaGenerated.cpp.tt"), templateData);

        File.WriteAllText(Path.Combine(outputDir, "MetaGenerated.h"), headerText, new UTF8Encoding(false));
        File.WriteAllText(Path.Combine(outputDir, "MetaGenerated.cpp"), sourceText, new UTF8Encoding(false));

        Console.WriteLine($"[MetaParser] Target={config.TargetName}, Types={orderedTypes.Count}, Output={outputDir}");
        return 0;
    }

    private static Dictionary<string, object?> BuildTemplateData(IReadOnlyList<ReflectTypeInfo> types)
    {
        var includes = types.Select(t => t.HeaderPath).Distinct(StringComparer.Ordinal).ToList();

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
            ["FunctionName"] = "RegisterReflectionTypes"
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
            if (!cls.IsDefinition || !isSameHeader || !HasReflectionMarker(cls) || !InheritsReflectionObject(cls))
                continue;

            var fullTypeName = string.IsNullOrWhiteSpace(cls.FullName)
                ? (string.IsNullOrWhiteSpace(cls.Name) ? string.Empty : cls.Name)
                : cls.FullName;
            if (string.IsNullOrWhiteSpace(fullTypeName))
                continue;

            yield return new ReflectTypeInfo(
                cls.Name,
                ExtractNamespace(fullTypeName, cls.Name),
                fullTypeName,
                ToGeneratedIncludePath(rootDir, normalizedHeader),
                ExtractBases(cls),
                ExtractFields(cls),
                ExtractMethods(cls));
        }
    }

    private static List<ReflectBaseInfo> ExtractBases(CppClass cls)
    {
        var result = new List<ReflectBaseInfo>();
        foreach (var baseType in cls.BaseTypes)
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
        return result.DistinctBy(x => x.TypeName).ToList();
    }

    private static List<ReflectFieldInfo> ExtractFields(CppClass cls)
    {
        return cls.Fields
            .Where(f => f.Visibility == CppVisibility.Public || f.Visibility == CppVisibility.Default)
            .Where(f => f.StorageQualifier != CppStorageQualifier.Static)
            .Where(f => !string.IsNullOrWhiteSpace(f.Name))
            .Select(f => new ReflectFieldInfo(f.Name, NormalizeTypeName(f.Type.GetDisplayName())))
            .Where(f => !string.IsNullOrWhiteSpace(f.TypeName))
            .DistinctBy(f => f.Name)
            .ToList();
    }

    private static List<ReflectMethodInfo> ExtractMethods(CppClass cls)
    {
        return cls.Functions
            .Where(m => m.Visibility == CppVisibility.Public || m.Visibility == CppVisibility.Default)
            .Where(m => !m.IsStatic)
            .Where(m => !m.IsConstructor && !m.IsDestructor)
            .Where(m => !m.Name.StartsWith("operator", StringComparison.Ordinal))
            .Where(m => !string.IsNullOrWhiteSpace(m.Name))
            .Select(m => new ReflectMethodInfo(m.Name))
            .DistinctBy(m => m.Name)
            .ToList();
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

        foreach (var systemIncludeDir in normalizedSystemIncludes)
            options.SystemIncludeFolders.Add(systemIncludeDir);

        foreach (var define in config.Defines.Where(static x => !string.IsNullOrWhiteSpace(x)).Distinct())
            options.Defines.Add(define);

        var compilerArgs = new List<string>();
        if (!string.IsNullOrWhiteSpace(config.ResourceDir))
        {
            compilerArgs.Add("-resource-dir");
            compilerArgs.Add(config.ResourceDir);
        }

        if (!string.IsNullOrWhiteSpace(config.Sysroot))
        {
            compilerArgs.Add("--sysroot");
            compilerArgs.Add(config.Sysroot);
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
        foreach (var cls in EnumerateClasses(compilation.Classes))
            yield return cls;
        foreach (var ns in compilation.Namespaces)
            foreach (var cls in EnumerateNamespaceClasses(ns))
                yield return cls;
    }

    private static IEnumerable<CppClass> EnumerateNamespaceClasses(CppNamespace ns)
    {
        foreach (var cls in EnumerateClasses(ns.Classes))
            yield return cls;
        foreach (var child in ns.Namespaces)
            foreach (var cls in EnumerateNamespaceClasses(child))
                yield return cls;
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

        return text.Contains($"Meta() class {cls.Name}", StringComparison.Ordinal)
               || text.Contains($"Meta() struct {cls.Name}", StringComparison.Ordinal)
               || text.Contains($"META() class {cls.Name}", StringComparison.Ordinal)
               || text.Contains($"META() struct {cls.Name}", StringComparison.Ordinal)
               || text.Contains($"CLASS({cls.Name}", StringComparison.Ordinal)
               || text.Contains($"STRUCT({cls.Name}", StringComparison.Ordinal);
    }

    private static bool IsReflectionAttribute(CppAttribute attribute)
        => attribute.Kind == AttributeKind.AnnotateAttribute
           || string.Equals(attribute.Name, "annotate", StringComparison.OrdinalIgnoreCase)
           || attribute.Name.Contains("annotate", StringComparison.OrdinalIgnoreCase);

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

    private static StringComparison PathComparison => OperatingSystem.IsWindows()
        ? StringComparison.OrdinalIgnoreCase
        : StringComparison.Ordinal;
}

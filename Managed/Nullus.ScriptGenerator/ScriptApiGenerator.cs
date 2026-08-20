using System.Collections.Immutable;
using System.Text;
using System.Text.Json;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace Nullus.ScriptGenerator;

[Generator(LanguageNames.CSharp)]
public sealed class ScriptApiGenerator : IIncrementalGenerator
{
    private static readonly DiagnosticDescriptor InvalidManifest = new(
        "NLS001",
        "Invalid Script API manifest",
        "The ScriptApi.json manifest could not be consumed: {0}",
        "Nullus",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private static readonly DiagnosticDescriptor ScriptDiagnostic = new(
        "NLS002",
        "Invalid Nullus script",
        "{0}",
        "Nullus",
        DiagnosticSeverity.Error,
        isEnabledByDefault: true);

    private sealed record Manifest(
        string SchemaHash,
        ImmutableArray<ManifestType> Types,
        ImmutableArray<string> Errors);

    private sealed record ManifestType(
        ulong Id,
        string Name,
        ImmutableArray<ManifestMember> Members,
        ImmutableArray<ManifestMember> Fields);

    private sealed record ManifestMember(ulong Id, string Name, string Signature);

    private sealed record BehaviourInfo(
        string TypeName,
        string SimpleName,
        string FilePath,
        ImmutableArray<string> Diagnostics,
        bool IsPublic,
        bool IsConcrete,
        bool HasNativeObjectHandleConstructor,
        uint CallbackMask,
        ImmutableArray<SerializedFieldInfo> Fields);

    private sealed record SerializedFieldInfo(
        string Name,
        string TypeName,
        string ValueKind,
        ImmutableArray<string> Aliases,
        bool DirectlyAccessible,
        bool Writable);

    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        var manifest = context.AdditionalTextsProvider
            .Where(static file => Path.GetFileName(file.Path).Equals("ScriptApi.json", StringComparison.OrdinalIgnoreCase))
            .Select(static (file, cancellationToken) => ParseManifest(file.GetText(cancellationToken)?.ToString()))
            .Collect()
            .Select(static (manifests, _) => manifests.Length == 0
                ? new Manifest(string.Empty, [], [])
                : manifests.OrderBy(static value => value.SchemaHash, StringComparer.Ordinal).First());

        var behaviours = context.SyntaxProvider
            .CreateSyntaxProvider(
                static (node, _) => node is ClassDeclarationSyntax,
                static (syntaxContext, cancellationToken) => FindBehaviour(syntaxContext, cancellationToken))
            .Where(static value => value is not null)
            .Select(static (value, _) => value!)
            .Collect();

        // MSBuild normally forwards the script-project symbols to Roslyn, but
        // Visual Studio and analyzer-hosted builds can omit project symbols
        // from ParseOptionsProvider.  The assembly name is the stable output
        // contract for the project script DLL, so use it as a second signal.
        var isScriptAssemblyProject = context.ParseOptionsProvider
            .Select(static (parseOptions, _) => parseOptions is CSharpParseOptions csharp
                && (csharp.PreprocessorSymbolNames.Contains("NLS_GAME_SCRIPTS_PROJECT")
                    || csharp.PreprocessorSymbolNames.Contains("NLS_ENGINE_SCRIPTS_PROJECT")))
            .Combine(context.CompilationProvider.Select(static (compilation, _) =>
                string.Equals(compilation.AssemblyName, "GameScripts", StringComparison.OrdinalIgnoreCase)
                || string.Equals(compilation.AssemblyName, "EngineScripts", StringComparison.OrdinalIgnoreCase)))
            .Select(static (values, _) => values.Left || values.Right);

        context.RegisterSourceOutput(
            manifest.Combine(behaviours).Combine(isScriptAssemblyProject),
            static (productionContext, input) => Emit(
                productionContext,
                input.Left.Left,
                input.Left.Right,
                input.Right));
    }

    private static Manifest ParseManifest(string? source)
    {
        if (string.IsNullOrWhiteSpace(source))
            return new Manifest(string.Empty, [], []);

        try
        {
            using var document = JsonDocument.Parse(source);
            var root = document.RootElement;
            var hash = root.TryGetProperty("schemaHash", out var hashElement)
                ? hashElement.GetString() ?? string.Empty
                : string.Empty;
            var types = ImmutableArray.CreateBuilder<ManifestType>();
            var errors = ImmutableArray.CreateBuilder<string>();
            var ids = new Dictionary<ulong, string>();

            if (string.IsNullOrWhiteSpace(hash))
                errors.Add("The manifest must contain a non-empty schemaHash.");

            if (!root.TryGetProperty("types", out var typeArray) || typeArray.ValueKind != JsonValueKind.Array)
                errors.Add("The manifest must contain a types array.");
            else
            {
                foreach (var type in typeArray.EnumerateArray())
                {
                    var typeId = ReadId(type, "id", errors);
                    var typeName = ReadString(type, "name");
                    AddId(ids, typeId, typeName, errors);
                    var members = ReadMembers(type, "methods", typeName, ids, errors);
                    members = members.AddRange(ReadMembers(type, "properties", typeName, ids, errors));
                    var fields = ReadMembers(type, "fields", typeName, ids, errors);
                    types.Add(new ManifestType(typeId, typeName, members, fields));
                }
            }

            return new Manifest(hash, types.ToImmutable(), errors.ToImmutable());
        }
        catch (Exception exception) when (exception is JsonException or InvalidOperationException)
        {
            return new Manifest(string.Empty, [], [exception.Message]);
        }
    }

    private static ImmutableArray<ManifestMember> ReadMembers(
        JsonElement type,
        string propertyName,
        string owner,
        Dictionary<ulong, string> ids,
        ImmutableArray<string>.Builder errors)
    {
        if (!type.TryGetProperty(propertyName, out var array) || array.ValueKind != JsonValueKind.Array)
            return [];

        var result = ImmutableArray.CreateBuilder<ManifestMember>();
        foreach (var member in array.EnumerateArray())
        {
            var id = ReadId(member, "id", errors);
            var name = ReadString(member, "name");
            var signature = member.TryGetProperty("signature", out var signatureElement)
                ? signatureElement.GetString() ?? name
                : name;
            AddId(ids, id, $"{owner}::{signature}", errors);
            result.Add(new ManifestMember(id, name, signature));
        }
        return result.ToImmutable();
    }

    private static ulong ReadId(JsonElement element, string name, ImmutableArray<string>.Builder errors)
    {
        if (!element.TryGetProperty(name, out var value) || value.ValueKind != JsonValueKind.Number || !value.TryGetUInt64(out var id) || id == 0)
        {
            errors.Add($"Manifest member '{name}' has an invalid zero or non-numeric ID.");
            return 0;
        }
        return id;
    }

    private static string ReadString(JsonElement element, string name)
        => element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? string.Empty
            : string.Empty;

    private static void AddId(Dictionary<ulong, string> ids, ulong id, string name, ImmutableArray<string>.Builder errors)
    {
        if (id == 0)
            return;
        if (ids.TryGetValue(id, out var previous) && !string.Equals(previous, name, StringComparison.Ordinal))
            errors.Add($"Script API ID collision: '{previous}' and '{name}' both map to {id}.");
        else
            ids[id] = name;
    }

    private static BehaviourInfo? FindBehaviour(GeneratorSyntaxContext context, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (context.Node is not ClassDeclarationSyntax declaration)
            return null;
        if (context.SemanticModel.GetDeclaredSymbol(declaration, cancellationToken) is not INamedTypeSymbol symbol)
            return null;
        if (!IsBehaviour(symbol))
            return null;

        var diagnostics = ImmutableArray.CreateBuilder<string>();
        var fields = ImmutableArray.CreateBuilder<SerializedFieldInfo>();
        var callbackMask = 0u;
        var isPublic = symbol.DeclaredAccessibility == Accessibility.Public;
        var isConcrete = !symbol.IsAbstract;
        var hasNativeObjectHandleConstructor = symbol.InstanceConstructors.Any(constructor =>
            constructor.DeclaredAccessibility is (Accessibility.Public or Accessibility.Internal) &&
            constructor.Parameters.Length == 1 &&
            constructor.Parameters[0].Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat) == "global::Nullus.NativeObjectHandle");
        if (hasNativeObjectHandleConstructor)
            diagnostics.Add($"Behaviour '{symbol.Name}' must use a parameterless constructor; NativeObjectHandle constructors are not part of the public script API.");
        if (!isPublic)
            diagnostics.Add($"Behaviour '{symbol.Name}' must be public.");
        var sourcePath = declaration.SyntaxTree.FilePath;
        if (isPublic && !string.IsNullOrWhiteSpace(sourcePath))
        {
            var fileName = Path.GetFileNameWithoutExtension(sourcePath);
            if (!string.Equals(fileName, symbol.Name, StringComparison.OrdinalIgnoreCase))
                diagnostics.Add($"Public Behaviour '{symbol.Name}' must be declared in a file named '{symbol.Name}.cs'.");
        }
        // A concrete script may inherit a lifecycle implementation from an
        // abstract/intermediate Behaviour. Include the whole chain in the
        // callback mask; the runtime message dispatcher selects the
        // most-derived method by name.
        for (var current = symbol; current is not null; current = current.BaseType)
        {
            if (current.Name == "Behaviour" && current.ContainingNamespace.ToDisplayString() == "Nullus")
                break;
            foreach (var method in current.GetMembers().OfType<IMethodSymbol>())
            {
                if (!IsLifecycleName(method.Name))
                    continue;
                callbackMask |= CallbackBit(method.Name);
                // Validate methods declared by this type only. Base types are
                // validated by their own manifest entry, avoiding duplicate
                // diagnostics for every derived script.
                if (!SymbolEqualityComparer.Default.Equals(current, symbol))
                    continue;
                if (method.IsAsync || method.ReturnType.Name is "Task" or "ValueTask")
                    diagnostics.Add($"Lifecycle method '{method.Name}' cannot be async.");
                var expectedParameterCount = 0;
                var validParameters = method.Parameters.Length == expectedParameterCount;
                if (method.IsStatic || !method.ReturnsVoid || !validParameters)
                    diagnostics.Add($"Lifecycle method '{method.Name}' must be an instance method returning void and accepting no parameters.");
            }
        }

        foreach (var field in symbol.GetMembers().OfType<IFieldSymbol>())
        {
            var hasSerializeField = HasAttribute(field, "global::Nullus.SerializeFieldAttribute");
            var hasNonSerialized = HasAttribute(field, "global::System.NonSerializedAttribute");
            var isPublicInstanceField = field.DeclaredAccessibility == Accessibility.Public
                && !field.IsStatic
                && !field.IsConst;

            // Match the field eligibility rules: public instance fields are
            // serialized by default, while private/protected fields require
            // [SerializeField]. [NonSerialized] always wins.
            if (hasNonSerialized || (!hasSerializeField && !isPublicInstanceField))
                continue;
            if (field.IsStatic || field.IsConst)
            {
                diagnostics.Add($"Serialized field '{field.Name}' must not be static or const.");
                continue;
            }
            if (field.IsReadOnly)
            {
                diagnostics.Add($"Serialized field '{field.Name}' must not be readonly.");
                continue;
            }
            if (!IsPortableField(field.Type))
                diagnostics.Add($"Serialized field '{field.Name}' uses unsupported type '{field.Type.ToDisplayString()}'.");
            var directlyAccessible = field.DeclaredAccessibility is Accessibility.Public or Accessibility.Internal;
            var aliases = field.GetAttributes()
                .Where(static attribute => attribute.AttributeClass?.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)
                    == "global::Nullus.FormerlySerializedAsAttribute")
                .Select(static attribute => attribute.ConstructorArguments.FirstOrDefault().Value as string)
                .Where(static alias => !string.IsNullOrWhiteSpace(alias))
                .Select(static alias => alias!)
                .Distinct(StringComparer.Ordinal)
                .ToImmutableArray();
            fields.Add(new SerializedFieldInfo(
                field.Name,
                field.Type.ToDisplayString(),
                PortableValueKind(field.Type),
                aliases,
                directlyAccessible,
                !field.IsReadOnly));
        }

        return new BehaviourInfo(
            symbol.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
            symbol.Name,
            sourcePath,
            diagnostics.ToImmutable(),
            isPublic,
            isConcrete,
             false,
            callbackMask,
            fields.ToImmutable());
    }

    private static string CanonicalScriptAssetPath(string sourcePath)
    {
        if (string.IsNullOrWhiteSpace(sourcePath))
            return string.Empty;

        var normalized = sourcePath.Replace('\\', '/');
        var marker = "/Assets/";
        var assetsIndex = normalized.LastIndexOf(marker, StringComparison.OrdinalIgnoreCase);
        if (assetsIndex >= 0)
            return normalized[(assetsIndex + 1)..];

        if (normalized.StartsWith("Assets/", StringComparison.OrdinalIgnoreCase))
            return normalized;

        return Path.GetFileName(normalized);
    }

    private static bool HasAttribute(IFieldSymbol field, string fullyQualifiedMetadataName)
        => field.GetAttributes().Any(attribute =>
            attribute.AttributeClass?.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat)
            == fullyQualifiedMetadataName);

    private static bool IsBehaviour(INamedTypeSymbol symbol)
    {
        for (var current = symbol.BaseType; current is not null; current = current.BaseType)
        {
            if (current.Name == "Behaviour" && current.ContainingNamespace.ToDisplayString() == "Nullus")
                return true;
        }
        return false;
    }

    private static bool IsLifecycleName(string name)
        => name is "Awake" or "Start" or "OnEnable" or "OnDisable" or "Update" or "FixedUpdate" or "LateUpdate" or "OnDestroy";

    private static uint CallbackBit(string name)
        => name switch
        {
            "Awake" => 1u << 0,
            "Start" => 1u << 1,
            "OnEnable" => 1u << 2,
            "OnDisable" => 1u << 3,
            "Update" => 1u << 4,
            "FixedUpdate" => 1u << 5,
            "LateUpdate" => 1u << 6,
            "OnDestroy" => 1u << 7,
            _ => 0u
        };

    private static bool IsPortableField(ITypeSymbol type)
    {
        if (type.TypeKind == TypeKind.Enum || type.SpecialType is SpecialType.System_Boolean or SpecialType.System_Byte
            or SpecialType.System_SByte or SpecialType.System_Int16 or SpecialType.System_UInt16
            or SpecialType.System_Int32 or SpecialType.System_UInt32 or SpecialType.System_Int64
            or SpecialType.System_UInt64 or SpecialType.System_Single or SpecialType.System_Double
            or SpecialType.System_String)
            return true;
        if (type.Name is "Vector2" or "Vector3" or "Vector4" or "Quaternion" or "Color"
            && type.ContainingNamespace.ToDisplayString() == "Nullus")
            return true;
        for (var current = type as INamedTypeSymbol; current is not null; current = current.BaseType)
            if (current.Name == "NativeObject" && current.ContainingNamespace.ToDisplayString() == "Nullus")
                return true;
        return false;
    }

    private static string PortableValueKind(ITypeSymbol type)
    {
        if (type.TypeKind == TypeKind.Enum)
            return "Enum";
        return type.SpecialType switch
        {
            SpecialType.System_Boolean => "Bool",
            SpecialType.System_SByte => "Int8",
            SpecialType.System_Byte => "UInt8",
            SpecialType.System_Int16 => "Int16",
            SpecialType.System_UInt16 => "UInt16",
            SpecialType.System_Int32 => "Int32",
            SpecialType.System_UInt32 => "UInt32",
            SpecialType.System_Int64 => "Int64",
            SpecialType.System_UInt64 => "UInt64",
            SpecialType.System_Single => "Float",
            SpecialType.System_Double => "Double",
            SpecialType.System_String => "String",
            _ when type.Name == "Vector2" && type.ContainingNamespace.ToDisplayString() == "Nullus" => "Vector2",
            _ when type.Name == "Vector3" && type.ContainingNamespace.ToDisplayString() == "Nullus" => "Vector3",
            _ when type.Name == "Vector4" && type.ContainingNamespace.ToDisplayString() == "Nullus" => "Vector4",
            _ when type.Name == "Quaternion" && type.ContainingNamespace.ToDisplayString() == "Nullus" => "Quaternion",
            _ when type.Name == "Color" && type.ContainingNamespace.ToDisplayString() == "Nullus" => "Color",
            _ when type is INamedTypeSymbol named && IsNativeObject(named) => "ObjectReference",
            _ => "Struct"
        };
    }

    private static bool IsNativeObject(INamedTypeSymbol type)
    {
        for (var current = type; current is not null; current = current.BaseType)
        {
            if (current.Name == "NativeObject" && current.ContainingNamespace.ToDisplayString() == "Nullus")
                return true;
        }
        return false;
    }

    private static void Emit(
        SourceProductionContext context,
        Manifest manifest,
        ImmutableArray<BehaviourInfo> behaviours,
        bool isScriptAssemblyProject)
    {
        foreach (var error in manifest.Errors)
            context.ReportDiagnostic(Diagnostic.Create(InvalidManifest, Location.None, error));
        foreach (var behaviour in behaviours)
            foreach (var diagnostic in behaviour.Diagnostics)
                context.ReportDiagnostic(Diagnostic.Create(ScriptDiagnostic, Location.None, diagnostic));

        var duplicateFiles = behaviours
            .Where(static value => value.IsPublic && !string.IsNullOrWhiteSpace(value.FilePath))
            .GroupBy(static value => value.FilePath, StringComparer.OrdinalIgnoreCase)
            .Where(static group => group.Count() > 1);
        foreach (var duplicateFile in duplicateFiles)
            context.ReportDiagnostic(Diagnostic.Create(
                ScriptDiagnostic,
                 Location.None,
                 $"A C# file may contain only one public Behaviour; '{duplicateFile.Key}' declares {duplicateFile.Count()}.") );

        var duplicateScriptNames = behaviours
            .Where(static value => value.IsPublic)
            .GroupBy(static value => value.SimpleName, StringComparer.Ordinal)
            .Where(static group => group.Count() > 1);
        foreach (var duplicateScriptName in duplicateScriptNames)
            context.ReportDiagnostic(Diagnostic.Create(
                ScriptDiagnostic,
                Location.None,
                $"Public Behaviour name '{duplicateScriptName.Key}' is declared {duplicateScriptName.Count()} times; script factory names must be unique."));

        var builder = new StringBuilder("// <auto-generated />\n#nullable enable\n");
        builder.AppendLine("namespace Nullus;");
        builder.AppendLine();
        if (!isScriptAssemblyProject)
        {
            // Native bindings live in Nullus.Managed.  GameScripts references
            // that assembly and must not emit a shadowing manifest type.
            builder.AppendLine("public static partial class ScriptApiManifest");
            builder.AppendLine("{");
            builder.Append("    public const string SchemaHash = ").Append(SymbolDisplay.FormatLiteral(manifest.SchemaHash, quote: true)).AppendLine(";");
            builder.AppendLine("    public static ulong StableId(string value)");
            builder.AppendLine("    {");
            builder.AppendLine("        const ulong offset = 14695981039346656037UL;");
            builder.AppendLine("        const ulong prime = 1099511628211UL;");
            builder.AppendLine("        var hash = offset;");
            builder.AppendLine("        foreach (var b in System.Text.Encoding.UTF8.GetBytes(value)) { hash ^= b; hash *= prime; }");
            builder.AppendLine("        return hash == 0 ? 1UL : hash;");
            builder.AppendLine("    }");
            builder.AppendLine("    public static ulong MemberId(string owner, string signature) => StableId(owner + \"::\" + signature);");
            builder.AppendLine("}");
            builder.AppendLine();
        }

        if (isScriptAssemblyProject)
        {
            // The editor consumes this generated, semantic inventory to decide
            // whether a source asset is a component.  Keep it data-only and
            // deterministic: no reflection or Type handles cross the ABI.
            var behaviourJson = new StringBuilder();
            behaviourJson.Append("{\"schemaHash\":")
                .Append(JsonSerializer.Serialize(manifest.SchemaHash))
                .Append(",\"behaviours\":[");
            var firstBehaviour = true;
            foreach (var behaviour in behaviours.OrderBy(static value => value.TypeName, StringComparer.Ordinal))
            {
                if (!firstBehaviour)
                    behaviourJson.Append(',');
                firstBehaviour = false;
                behaviourJson.Append("{\"scriptAssetPath\":")
                    .Append(JsonSerializer.Serialize(CanonicalScriptAssetPath(behaviour.FilePath)))
                    .Append(",\"fileName\":")
                    .Append(JsonSerializer.Serialize(Path.GetFileName(behaviour.FilePath)))
                    .Append(",\"fullTypeName\":")
                    .Append(JsonSerializer.Serialize(behaviour.TypeName))
                    .Append(",\"simpleName\":")
                    .Append(JsonSerializer.Serialize(behaviour.SimpleName))
                    .Append(",\"scriptTypeId\":")
                    .Append(StableIdentifier($"CSharp:{CanonicalScriptAssetPath(behaviour.FilePath)}").ToString(System.Globalization.CultureInfo.InvariantCulture))
                    .Append(",\"isPublic\":").Append(behaviour.IsPublic ? "true" : "false")
                .Append(",\"isConcrete\":").Append(behaviour.IsConcrete ? "true" : "false")
                .Append(",\"hasNativeObjectHandleConstructor\":")
                .Append(behaviour.HasNativeObjectHandleConstructor ? "true" : "false")
                .Append(",\"isComponent\":")
                    .Append(behaviour.IsPublic && behaviour.IsConcrete && behaviour.Diagnostics.IsEmpty ? "true" : "false")
                    .Append(",\"callbackMask\":").Append(behaviour.CallbackMask.ToString(System.Globalization.CultureInfo.InvariantCulture))
                    .Append(",\"serializedFields\":[");
                var firstField = true;
                foreach (var field in behaviour.Fields.OrderBy(static value => value.Name, StringComparer.Ordinal))
                {
                    if (!firstField)
                        behaviourJson.Append(',');
                    firstField = false;
                     behaviourJson.Append("{\"id\":")
                        .Append(StableIdentifier($"{behaviour.TypeName}::{field.Name}::field").ToString(System.Globalization.CultureInfo.InvariantCulture))
                        .Append(",\"name\":")
                         .Append(JsonSerializer.Serialize(field.Name))
                         .Append(",\"kind\":")
                         .Append(JsonSerializer.Serialize(field.ValueKind))
                         .Append(",\"serialized\":true")
                         .Append(",\"typeName\":")
                        .Append(JsonSerializer.Serialize(field.TypeName))
                        .Append(",\"aliases\":[");
                    for (var aliasIndex = 0; aliasIndex < field.Aliases.Length; ++aliasIndex)
                    {
                        if (aliasIndex != 0)
                            behaviourJson.Append(',');
                        behaviourJson.Append(JsonSerializer.Serialize(field.Aliases[aliasIndex]));
                    }
                    behaviourJson.Append("]}");
                }
                behaviourJson.Append("]}");
            }
            behaviourJson.Append("]}");

            builder.AppendLine("internal static class ScriptBehaviourManifest");
            builder.AppendLine("{");
            builder.Append("    public const string Json = ")
                .Append(SymbolDisplay.FormatLiteral(behaviourJson.ToString(), quote: true))
                .AppendLine(";");
            builder.AppendLine("}");
            builder.AppendLine();
        }
        if (isScriptAssemblyProject)
        {
        builder.AppendLine("internal static class ScriptCallbackMaskRegistry");
        builder.AppendLine("{");
        builder.AppendLine("    public static uint Get(string typeName) => typeName switch");
        builder.AppendLine("    {");
        foreach (var behaviour in behaviours.OrderBy(static value => value.TypeName, StringComparer.Ordinal))
        {
            builder.Append("        ").Append(SymbolDisplay.FormatLiteral(behaviour.TypeName, quote: true))
                .Append(" => ").Append(behaviour.CallbackMask.ToString(System.Globalization.CultureInfo.InvariantCulture)).AppendLine("u,");
        }
        builder.AppendLine("        _ => 0u");
        builder.AppendLine("    };");
        builder.AppendLine("}");
        builder.AppendLine();

        builder.AppendLine("internal static class ScriptFieldRegistry");
        builder.AppendLine("{");
        builder.AppendLine("    public static System.Collections.Generic.IReadOnlyList<Nullus.ManagedScriptFieldDescriptor> Get(string typeName) => typeName switch");
        builder.AppendLine("    {");
        foreach (var behaviour in behaviours.OrderBy(static value => value.TypeName, StringComparer.Ordinal))
        {
            builder.Append("        ").Append(SymbolDisplay.FormatLiteral(behaviour.TypeName, quote: true)).Append(" => new Nullus.ManagedScriptFieldDescriptor[] { ");
            foreach (var field in behaviour.Fields.OrderBy(static value => value.Name, StringComparer.Ordinal))
            {
                builder.Append("new(Nullus.ScriptApiManifest.StableId(")
                    .Append(SymbolDisplay.FormatLiteral($"{behaviour.TypeName}::{field.Name}::field", quote: true))
                    .Append("), ")
                    .Append(SymbolDisplay.FormatLiteral(field.Name, quote: true))
                    .Append(", ")
                    .Append(SymbolDisplay.FormatLiteral(field.TypeName, quote: true))
                    .Append(", new string[] { ");
                foreach (var alias in field.Aliases)
                    builder.Append(SymbolDisplay.FormatLiteral(alias, quote: true)).Append(", ");
                builder.Append(" }), ");
            }
            builder.AppendLine(" },");
        }
        builder.AppendLine("        _ => System.Array.Empty<Nullus.ManagedScriptFieldDescriptor>()");
        builder.AppendLine("    };");
        builder.AppendLine("}");
        builder.AppendLine();
        builder.AppendLine("internal static class ScriptFieldAccess");
        builder.AppendLine("{");
        var reflectionFields = behaviours
            .SelectMany(static value => value.Fields.Where(static field => !field.DirectlyAccessible)
                .Select(field => (Behaviour: value, Field: field)))
            .OrderBy(static value => value.Behaviour.TypeName, StringComparer.Ordinal)
            .ThenBy(static value => value.Field.Name, StringComparer.Ordinal)
            .ToList();
        foreach (var (behaviour, field) in reflectionFields)
        {
            var identifier = $"Field_{StableIdentifier($"{behaviour.TypeName}::{field.Name}::reflection"):X16}";
            builder.Append("    private static readonly System.Reflection.FieldInfo ").Append(identifier)
                .Append(" = typeof(").Append(behaviour.TypeName)
                .Append(").GetField(")
                .Append(SymbolDisplay.FormatLiteral(field.Name, quote: true))
                .AppendLine(", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic)!;");
        }
        builder.AppendLine("    public static bool TryGet(Nullus.Behaviour behaviour, ulong field, out object? value)");
        builder.AppendLine("    {");
        var behaviourIndex = 0;
        foreach (var behaviour in behaviours.OrderBy(static value => value.TypeName, StringComparer.Ordinal))
        {
            var typedName = $"typed{behaviourIndex++}";
            builder.Append("        if (behaviour is ").Append(behaviour.TypeName).Append(" ").Append(typedName).AppendLine(")");
            builder.AppendLine("        {");
            builder.AppendLine("            switch (field)");
            builder.AppendLine("            {");
            foreach (var field in behaviour.Fields.OrderBy(static value => value.Name, StringComparer.Ordinal))
            {
                var fieldId = StableIdentifier($"{behaviour.TypeName}::{field.Name}::field");
                builder.Append("                case ").Append(fieldId.ToString(System.Globalization.CultureInfo.InvariantCulture)).Append("UL: ");
                if (field.DirectlyAccessible)
                    builder.Append("value = ").Append(typedName).Append(".").Append(field.Name);
                else
                {
                    var identifier = $"Field_{StableIdentifier($"{behaviour.TypeName}::{field.Name}::reflection"):X16}";
                    builder.Append("value = ").Append(identifier).Append(".GetValue(").Append(typedName).Append(")");
                }
                builder.AppendLine("; return true;");
            }
            builder.AppendLine("                default: break;");
            builder.AppendLine("            }");
            builder.AppendLine("        }");
        }
        builder.AppendLine("        value = null;");
        builder.AppendLine("        return false;");
        builder.AppendLine("    }");
        builder.AppendLine();
        builder.AppendLine("    public static bool TrySet(Nullus.Behaviour behaviour, ulong field, object? value)");
        builder.AppendLine("    {");
        behaviourIndex = 0;
        foreach (var behaviour in behaviours.OrderBy(static value => value.TypeName, StringComparer.Ordinal))
        {
            var typedName = $"typed{behaviourIndex++}";
            builder.Append("        if (behaviour is ").Append(behaviour.TypeName).Append(" ").Append(typedName).AppendLine(")");
            builder.AppendLine("        {");
            builder.AppendLine("            switch (field)");
            builder.AppendLine("            {");
            foreach (var field in behaviour.Fields.Where(static value => value.Writable).OrderBy(static value => value.Name, StringComparer.Ordinal))
            {
                var fieldId = StableIdentifier($"{behaviour.TypeName}::{field.Name}::field");
                builder.Append("                case ").Append(fieldId.ToString(System.Globalization.CultureInfo.InvariantCulture)).AppendLine("UL:")
                    .AppendLine("                {")
                    .Append("                    if (Nullus.ScriptValueConversion.TryConvert(value, typeof(")
                    .Append(field.TypeName)
                    .Append("), out var converted)) { ");
                if (field.DirectlyAccessible)
                    builder.Append(typedName).Append(".").Append(field.Name).Append(" = (").Append(field.TypeName).Append(")converted!");
                else
                {
                    var identifier = $"Field_{StableIdentifier($"{behaviour.TypeName}::{field.Name}::reflection"):X16}";
                    builder.Append(identifier).Append(".SetValue(").Append(typedName).Append(", converted)");
                }
                builder.AppendLine("; return true; }");
                builder.AppendLine("                    return false;");
                builder.AppendLine("                }");
            }
            builder.AppendLine("                default: break;");
            builder.AppendLine("            }");
            builder.AppendLine("        }");
        }
        builder.AppendLine("        return false;");
        builder.AppendLine("    }");
        builder.AppendLine("}");
        builder.AppendLine();
        builder.AppendLine("internal static class ScriptDispatch");
        builder.AppendLine("{");
        builder.AppendLine("    public static bool Invoke(Nullus.Behaviour behaviour, ushort callback, float deltaTime)");
        builder.AppendLine("    {");
        behaviourIndex = 0;
        foreach (var behaviour in behaviours.OrderBy(static value => value.TypeName, StringComparer.Ordinal))
        {
            var typedName = $"typed{behaviourIndex++}";
            builder.Append("        if (behaviour is ").Append(behaviour.TypeName).Append(" ").Append(typedName).AppendLine(")");
            builder.AppendLine("        {");
            builder.AppendLine("            switch (callback)");
            builder.AppendLine("            {");
            builder.Append("                case 0: return Nullus.LifecycleInvoker.Invoke(").Append(typedName).AppendLine(", callback);");
            builder.Append("                case 1: return Nullus.LifecycleInvoker.Invoke(").Append(typedName).AppendLine(", callback);");
            builder.Append("                case 2: return Nullus.LifecycleInvoker.Invoke(").Append(typedName).AppendLine(", callback);");
            builder.Append("                case 3: return Nullus.LifecycleInvoker.Invoke(").Append(typedName).AppendLine(", callback);");
            builder.Append("                case 4: return Nullus.LifecycleInvoker.Invoke(").Append(typedName).AppendLine(", callback);");
            builder.Append("                case 5: return Nullus.LifecycleInvoker.Invoke(").Append(typedName).AppendLine(", callback);");
            builder.Append("                case 6: return Nullus.LifecycleInvoker.Invoke(").Append(typedName).AppendLine(", callback);");
            builder.Append("                case 7: return Nullus.LifecycleInvoker.Invoke(").Append(typedName).AppendLine(", callback);");
            builder.AppendLine("                default: return false;");
            builder.AppendLine("            }");
            builder.AppendLine("        }");
        }
        builder.AppendLine("        return false;");
        builder.AppendLine("    }");
        builder.AppendLine("}");
        builder.AppendLine();
        builder.AppendLine("internal static class ScriptFactory");
        builder.AppendLine("{");
        builder.AppendLine("    public static bool TryCreate(string assetPath, Nullus.NativeObjectHandle handle, out Nullus.Behaviour? behaviour)");
        builder.AppendLine("    {");
        builder.AppendLine("        var fileName = System.IO.Path.GetFileNameWithoutExtension(assetPath);");
        foreach (var behaviour in behaviours
            .Where(static value => value.IsPublic && value.IsConcrete && value.Diagnostics.IsEmpty)
            .OrderBy(static value => value.TypeName, StringComparer.Ordinal))
        {
            builder.Append("        if (fileName == ").Append(SymbolDisplay.FormatLiteral(behaviour.SimpleName, quote: true))
                .Append(" || assetPath == ").Append(SymbolDisplay.FormatLiteral(behaviour.TypeName, quote: true)).AppendLine(")");
            builder.Append("            { var instance = new ").Append(behaviour.TypeName).Append("(); instance.BindNativeHandle(handle)");
            builder.AppendLine("; behaviour = instance; return true; }");
        }
        builder.AppendLine("        behaviour = null;");
        builder.AppendLine("        return false;");
        builder.AppendLine("    }");
        builder.AppendLine("}");

        builder.AppendLine();
        builder.AppendLine("internal static class NullusScriptAssemblyRegistration");
        builder.AppendLine("{");
        builder.AppendLine("    [System.Runtime.CompilerServices.ModuleInitializer]");
        builder.AppendLine("    public static void Register() => Nullus.ManagedScriptRegistry.Register(typeof(ScriptFactory).Assembly.GetName().Name ?? \"ScriptAssembly\", ScriptFactory.TryCreate, ScriptDispatch.Invoke, ScriptCallbackMaskRegistry.Get, ScriptFieldRegistry.Get, ScriptFieldAccess.TryGet, ScriptFieldAccess.TrySet, static () => ScriptBehaviourManifest.Json);");
        builder.AppendLine("}");
        }

        context.AddSource("ScriptApiManifest.g.cs", SourceText.From(builder.ToString(), Encoding.UTF8));
    }

    private static ulong StableIdentifier(string value)
    {
        const ulong offset = 14695981039346656037UL;
        const ulong prime = 1099511628211UL;
        var hash = offset;
        foreach (var b in Encoding.UTF8.GetBytes(value))
        {
            hash ^= b;
            hash *= prime;
        }
        return hash == 0 ? 1UL : hash;
    }
}

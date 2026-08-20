using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

internal static partial class MetaParserTool
{
    private sealed record ScriptApiManifestDocument(int version, string schemaHash, IReadOnlyList<ScriptApiManifestType> types);
    private sealed record ScriptApiManifestType(ulong id, string name, IReadOnlyList<ScriptApiManifestMethod> methods, IReadOnlyList<ScriptApiManifestProperty> properties, IReadOnlyList<ScriptApiManifestField> fields);
    private sealed record ScriptApiManifestMethod(
        ulong id,
        string name,
        string signature,
        ScriptApiManifestScriptType returnType,
        uint callbacks,
        bool scriptable,
        [property: JsonPropertyName("static")] bool isStatic,
        IReadOnlyList<ScriptApiManifestParameter> parameters);
    private sealed record ScriptApiManifestParameter(string name, ScriptApiManifestScriptType type);
    private sealed record ScriptApiManifestProperty(
        ulong id,
        string name,
        ScriptApiManifestScriptType type,
        bool readable,
        bool writable,
        ulong getterId,
        ulong setterId,
        bool scriptable);
    private sealed record ScriptApiManifestField(ulong id, string name, ScriptApiManifestScriptType type, bool serialized, IReadOnlyList<string> aliases);
    private sealed record ScriptApiManifestScriptType(
        ulong id,
        uint kind,
        string name,
        uint size,
        uint alignment,
        bool portable);

    private static void WriteScriptApiManifest(
        string rootDir,
        string outputDir,
        string targetName,
        IReadOnlyList<ReflectTypeInfo> types)
    {
        var scriptTypes = types
            .Where(static type => type.IsScriptable || type.Methods.Any(static method => method.IsScriptable) || type.Fields.Any(static field => field.IsScriptable))
            .OrderBy(static type => type.QualifiedName, StringComparer.Ordinal)
            .Select(BuildScriptApiType)
            .ToList();
        ValidateScriptApiIds(scriptTypes);

        var jsonOptions = new JsonSerializerOptions { WriteIndented = false, PropertyNamingPolicy = null };
        var schemaHash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(BuildCanonicalSchema(scriptTypes)))).ToLowerInvariant();
        var document = new ScriptApiManifestDocument(2, schemaHash, scriptTypes);
        var json = JsonSerializer.Serialize(document, jsonOptions);
        Directory.CreateDirectory(outputDir);
        File.WriteAllText(Path.Combine(outputDir, "ScriptApi.json"), json, new UTF8Encoding(false));

        var jsonBytes = Encoding.UTF8.GetBytes(json);
        const string binaryMagic = "NLS_SCRIPT_API\0";
        var binary = new byte[binaryMagic.Length + 4 + 32 + 8 + jsonBytes.Length];
        Encoding.ASCII.GetBytes(binaryMagic).CopyTo(binary, 0);
        BinaryPrimitives.WriteUInt32LittleEndian(binary.AsSpan(binaryMagic.Length), 1);
        Convert.FromHexString(schemaHash).CopyTo(binary, binaryMagic.Length + 4);
        BinaryPrimitives.WriteUInt64LittleEndian(binary.AsSpan(binaryMagic.Length + 4 + 32), (ulong)jsonBytes.Length);
        jsonBytes.CopyTo(binary, binaryMagic.Length + 4 + 32 + 8);
        File.WriteAllBytes(Path.Combine(outputDir, "ScriptApi.bin"), binary);

        // Engine owns the portable gameplay API in the first scripting phase.
        // Other runtime targets still receive their per-target manifest above,
        // but must not race to overwrite the project-level schema consumed by
        // the managed generator.
        if (string.Equals(targetName, "NLS_Engine", StringComparison.Ordinal))
        {
            var libraryDir = Path.Combine(rootDir, "Library", "ScriptApi");
            Directory.CreateDirectory(libraryDir);
            File.WriteAllText(Path.Combine(libraryDir, "ScriptApi.json"), json, new UTF8Encoding(false));
            File.WriteAllBytes(Path.Combine(libraryDir, "ScriptApi.bin"), binary);
        }
    }

    private static ScriptApiManifestType BuildScriptApiType(ReflectTypeInfo type)
    {
        foreach (var field in type.Fields.Where(static field => field.IsScriptable))
            ValidatePortableType(field.TypeName, $"{type.QualifiedName}.{field.Name}");
        foreach (var method in type.Methods.Where(static method => method.IsScriptable))
        {
            ValidatePortableType(method.ReturnTypeName ?? "void", $"{type.QualifiedName}.{method.Name} return value");
            foreach (var parameter in method.ParameterTypeNames ?? [])
                ValidatePortableType(parameter, $"{type.QualifiedName}.{method.Name} parameter");
        }
        foreach (var property in type.Fields.Where(static field => field.IsScriptable)
                     .Where(static field => !string.IsNullOrWhiteSpace(field.GetterExpression) || !string.IsNullOrWhiteSpace(field.SetterExpression)))
            ValidatePortableType(property.TypeName, $"{type.QualifiedName}.{property.Name} property");
        var methods = type.Methods
            .Where(static method => method.IsScriptable)
            .OrderBy(static method => method.Name, StringComparer.Ordinal)
            .ThenBy(static method => string.Join(",", method.ParameterTypeNames ?? []), StringComparer.Ordinal)
            .Select(method =>
            {
                var parameters = method.ParameterTypeNames ?? [];
                var signature = $"{method.Name}({string.Join(",", parameters)})";
                return new ScriptApiManifestMethod(
                    StableScriptId($"{type.QualifiedName}::{signature}"),
                    method.Name,
                    signature,
                    ToScriptType(method.ReturnTypeName ?? "void"),
                    CallbackMask(method.Name),
                    method.IsScriptable,
                    method.IsStatic,
                    parameters.Select(parameter => new ScriptApiManifestParameter(string.Empty, ToScriptType(parameter))).ToList());
            })
            .ToList();
        var fields = type.Fields
            .Where(static field => field.IsScriptable)
            .OrderBy(static field => field.Name, StringComparer.Ordinal)
            .Select(field => new ScriptApiManifestField(
                StableScriptId($"{type.QualifiedName}::{field.Name}::field"),
                field.Name,
                ToScriptType(field.TypeName),
                true,
                []))
            .ToList();
        var properties = type.Fields
            .Where(static field => field.IsScriptable && (!string.IsNullOrWhiteSpace(field.GetterExpression) || !string.IsNullOrWhiteSpace(field.SetterExpression)))
            .OrderBy(static field => field.Name, StringComparer.Ordinal)
            .Select(field =>
            {
                var accessorName = string.IsNullOrEmpty(field.Name)
                    ? field.Name
                    : char.ToUpperInvariant(field.Name[0]) + field.Name[1..];
                var getter = methods.FirstOrDefault(method =>
                    string.Equals(method.name, $"Get{accessorName}", StringComparison.Ordinal)
                    && method.parameters.Count == 0);
                var setter = methods.FirstOrDefault(method =>
                    string.Equals(method.name, $"Set{accessorName}", StringComparison.Ordinal)
                    && method.parameters.Count == 1);
                return new ScriptApiManifestProperty(
                    StableScriptId($"{type.QualifiedName}::{field.Name}::property"),
                    field.Name,
                    ToScriptType(field.TypeName),
                    !string.IsNullOrWhiteSpace(field.GetterExpression),
                    !string.IsNullOrWhiteSpace(field.SetterExpression),
                    getter?.id ?? 0,
                    setter?.id ?? 0,
                    true);
            })
            .ToList();
        return new ScriptApiManifestType(
            StableScriptId(type.QualifiedName),
            type.QualifiedName,
            methods,
            properties,
            fields);
    }

    private static void ValidatePortableType(string typeName, string subject)
    {
        var normalized = NormalizePortableTypeName(typeName);
        if (normalized.Contains("unique_ptr", StringComparison.Ordinal)
            || normalized.Contains("shared_ptr", StringComparison.Ordinal)
            || normalized.Contains("map<", StringComparison.Ordinal)
            || normalized.Contains("span<", StringComparison.Ordinal)
            || normalized.Contains("function<", StringComparison.Ordinal)
            || normalized.Contains('<')
            || normalized.Contains('&'))
        {
            throw new InvalidOperationException($"Script API member '{subject}' uses unsupported non-portable type '{typeName}'.");
        }

        var allowed = normalized is "void" or "bool" or "char" or "signed char" or "unsigned char"
            or "short" or "unsigned short" or "int" or "unsigned int"
            or "int8_t" or "uint8_t" or "int16_t" or "uint16_t" or "int32_t" or "uint32_t"
            or "int64_t" or "uint64_t" or "float" or "double" or "std::string"
            or "NLS::Maths::Vector2" or "NLS::Maths::Vector3" or "NLS::Maths::Vector4"
            or "NLS::Maths::Quaternion" or "NLS::Maths::Color"
            or "NLS::Render::Settings::EProjectionMode"
            or "NLS::Render::Settings::ELightType";
        if (normalized.EndsWith('*'))
        {
            var objectName = normalized[..^1].Trim();
            // Pointers are object references at the scripting boundary.  Raw
            // pointers to scalar data would expose an unstable native layout.
            if (objectName is "void" or "char" or "signed char" or "unsigned char"
                or "short" or "unsigned short" or "int" or "unsigned int"
                or "int8_t" or "uint8_t" or "int16_t" or "uint16_t"
                or "int32_t" or "uint32_t" or "int64_t" or "uint64_t"
                or "float" or "double" || objectName.Contains('<'))
            {
                throw new InvalidOperationException($"Script API member '{subject}' uses unsupported pointer type '{typeName}'.");
            }
            return;
        }
        if (!allowed)
            throw new InvalidOperationException($"Script API member '{subject}' uses unsupported type '{typeName}'.");
    }

    private static string NormalizePortableTypeName(string typeName)
    {
        var normalized = typeName.Trim();
        normalized = normalized.Replace("const ", string.Empty, StringComparison.Ordinal);
        normalized = normalized.Replace(" const", string.Empty, StringComparison.Ordinal);
        var whitespace = new StringBuilder(normalized.Length);
        var pendingSpace = false;
        foreach (var character in normalized)
        {
            if (char.IsWhiteSpace(character))
            {
                pendingSpace = whitespace.Length != 0;
                continue;
            }
            if (pendingSpace && whitespace.Length != 0)
                whitespace.Append(' ');
            whitespace.Append(character);
            pendingSpace = false;
        }
        normalized = whitespace.ToString();
        while (normalized.EndsWith('&') && !normalized.EndsWith("&&", StringComparison.Ordinal))
            normalized = normalized[..^1];
        return normalized.Trim();
    }

    private static ulong StableScriptId(string value)
    {
        const ulong offset = 14695981039346656037UL;
        const ulong prime = 1099511628211UL;
        var hash = offset;
        foreach (var character in Encoding.UTF8.GetBytes(value))
        {
            hash ^= character;
            hash *= prime;
        }
        return hash == 0 ? 1UL : hash;
    }

    private static uint CallbackMask(string methodName)
        => methodName switch
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

    private static ScriptApiManifestScriptType ToScriptType(string typeName)
    {
        var normalized = NormalizePortableTypeName(typeName);
        if (normalized.EndsWith("*", StringComparison.Ordinal))
        {
            var objectName = normalized[..^1].Trim();
            return new ScriptApiManifestScriptType(
                StableScriptId(objectName),
                19u,
                objectName,
                8u,
                8u,
                true);
        }

        var (kind, size, alignment) = normalized switch
        {
            "void" => (0u, 0u, 0u),
            "bool" => (1u, 1u, 1u),
            "char" or "signed char" or "int8_t" => (2u, 1u, 1u),
            "unsigned char" or "uint8_t" => (3u, 1u, 1u),
            "short" or "int16_t" => (4u, 2u, 2u),
            "unsigned short" or "uint16_t" => (5u, 2u, 2u),
            "int" or "int32_t" => (6u, 4u, 4u),
            "unsigned int" or "uint32_t" => (7u, 4u, 4u),
            "int64_t" => (8u, 8u, 8u),
            "uint64_t" => (9u, 8u, 8u),
            "float" => (10u, 4u, 4u),
            "double" => (11u, 8u, 8u),
            "std::string" => (12u, 0u, 0u),
            "NLS::Maths::Vector2" => (14u, 8u, 4u),
            "NLS::Maths::Vector3" => (15u, 12u, 4u),
            "NLS::Maths::Vector4" => (16u, 16u, 4u),
            "NLS::Maths::Quaternion" => (17u, 16u, 4u),
            "NLS::Maths::Color" => (18u, 16u, 4u),
            "NLS::Render::Settings::EProjectionMode" or "NLS::Render::Settings::ELightType" => (13u, 1u, 1u),
            _ => (20u, 0u, 1u)
        };
        return new ScriptApiManifestScriptType(
            kind == 0u ? 0u : StableScriptId(normalized),
            kind,
            normalized,
            size,
            alignment,
            true);
    }

    private static string BuildCanonicalSchema(IReadOnlyList<ScriptApiManifestType> types)
    {
        var builder = new StringBuilder();
        static void Append(StringBuilder target, string value)
        {
            target.Append(Encoding.UTF8.GetByteCount(value)).Append(':').Append(value).Append(';');
        }
        static void AppendType(StringBuilder target, ScriptApiManifestScriptType type)
        {
            Append(target, type.name);
            Append(target, type.id.ToString(System.Globalization.CultureInfo.InvariantCulture));
            Append(target, type.kind.ToString(System.Globalization.CultureInfo.InvariantCulture));
            Append(target, type.size.ToString(System.Globalization.CultureInfo.InvariantCulture));
            Append(target, type.alignment.ToString(System.Globalization.CultureInfo.InvariantCulture));
            Append(target, type.portable ? "1" : "0");
        }

        foreach (var type in types.OrderBy(static type => type.name, StringComparer.Ordinal))
        {
            Append(builder, type.name);
            Append(builder, type.id.ToString(System.Globalization.CultureInfo.InvariantCulture));
            foreach (var method in type.methods.OrderBy(static method => method.signature, StringComparer.Ordinal).ThenBy(static method => method.id))
            {
                Append(builder, "method");
                Append(builder, method.name);
                Append(builder, method.signature);
                Append(builder, method.id.ToString(System.Globalization.CultureInfo.InvariantCulture));
                Append(builder, method.callbacks.ToString(System.Globalization.CultureInfo.InvariantCulture));
                Append(builder, method.scriptable ? "1" : "0");
                Append(builder, method.isStatic ? "1" : "0");
                AppendType(builder, method.returnType);
                foreach (var parameter in method.parameters)
                    AppendType(builder, parameter.type);
            }
            foreach (var property in type.properties.OrderBy(static property => property.name, StringComparer.Ordinal).ThenBy(static property => property.id))
            {
                Append(builder, "property");
                Append(builder, property.name);
                Append(builder, property.id.ToString(System.Globalization.CultureInfo.InvariantCulture));
                Append(builder, property.getterId.ToString(System.Globalization.CultureInfo.InvariantCulture));
                Append(builder, property.setterId.ToString(System.Globalization.CultureInfo.InvariantCulture));
                Append(builder, property.readable ? "1" : "0");
                Append(builder, property.writable ? "1" : "0");
                Append(builder, property.scriptable ? "1" : "0");
                AppendType(builder, property.type);
            }
            foreach (var field in type.fields.OrderBy(static field => field.name, StringComparer.Ordinal).ThenBy(static field => field.id))
            {
                Append(builder, "field");
                Append(builder, field.name);
                Append(builder, field.id.ToString(System.Globalization.CultureInfo.InvariantCulture));
                Append(builder, field.serialized ? "1" : "0");
                AppendType(builder, field.type);
                foreach (var alias in field.aliases.OrderBy(static alias => alias, StringComparer.Ordinal))
                    Append(builder, alias);
                // Native canonicalization reserves a slot for field defaults;
                // generated C++ API fields have no default in the reflection
                // manifest, so keep the explicit absent marker in sync.
                Append(builder, "<none>");
            }
        }
        return builder.ToString();
    }

    private static void ValidateScriptApiIds(IReadOnlyList<ScriptApiManifestType> types)
    {
        var ids = new Dictionary<ulong, string>();
        void Add(ulong id, string name)
        {
            if (id == 0)
                throw new InvalidOperationException($"Script API member '{name}' generated an invalid zero ID.");
            if (ids.TryGetValue(id, out var previous) && !string.Equals(previous, name, StringComparison.Ordinal))
                throw new InvalidOperationException($"Script API ID collision: '{previous}' and '{name}' both map to {id}.");
            ids[id] = name;
        }

        foreach (var type in types)
        {
            Add(type.id, type.name);
            foreach (var method in type.methods)
                Add(method.id, $"{type.name}::{method.signature}");
            foreach (var property in type.properties)
                Add(property.id, $"{type.name}::{property.name}::property");
            foreach (var field in type.fields)
                Add(field.id, $"{type.name}::{field.name}::field");
        }
    }
}

internal sealed record ReflectFieldInfo(
    string Name,
    string TypeName,
    string GetterExpression,
    string SetterExpression,
    bool IsPrivate,
    List<ReflectTypeMetaInfo>? FieldMetas = null,
    bool IsScriptable = false);

internal sealed record ReflectMethodInfo(
    string Name,
    string PointerExpression,
    bool IsStatic,
    bool IsPrivate,
    string? ReturnTypeName = null,
    List<string>? ParameterTypeNames = null,
    bool IsScriptable = false,
    string? PropertyName = null);

internal sealed record ReflectBaseInfo(string TypeName);

internal sealed record ReflectEnumValueInfo(string Name);

internal sealed record ReflectTypeMetaInfo(string PropertyTypeName, string InitializerArguments);

internal sealed record MethodCandidateInfo(
    string Name,
    string PointerExpression,
    string ReturnTypeName,
    List<string> ParameterTypeNames,
    bool IsStatic,
    bool IsPrivate,
    bool IsConst,
    string? PropertyName,
    bool IsScriptable = false);

internal sealed record ReflectTypeInfo(
    string ClassName,
    string NamespacePrefix,
    string FullTypeName,
    string HeaderPath,
    string SourceFilePath,
    List<ReflectBaseInfo> Bases,
    List<ReflectFieldInfo> Fields,
    List<ReflectMethodInfo> Methods,
    List<ReflectTypeMetaInfo>? TypeMetas = null,
    int? GeneratedBodyLine = null,
    bool IsEnum = false,
    List<ReflectEnumValueInfo>? EnumValues = null,
    bool IsScriptable = false)
{
    public string QualifiedName => FullTypeName;
}

internal sealed class ReflectionTypeCatalog
{
    public HashSet<string> SupportedTypeNames { get; } = new(StringComparer.Ordinal);
    public Dictionary<string, string> ReflectedBaseTypeNames { get; } = new(StringComparer.Ordinal);
    public Dictionary<string, List<string>> ReflectedDirectBaseTypeNames { get; } = new(StringComparer.Ordinal);
}

internal sealed record ReflectFieldInfo(string Name, string TypeName, string GetterExpression, string SetterExpression, bool IsPrivate);

internal sealed record ReflectMethodInfo(string Name, string PointerExpression, bool IsStatic, bool IsPrivate);

internal sealed record ReflectBaseInfo(string TypeName);

internal sealed record ReflectEnumValueInfo(string Name);

internal sealed record MethodCandidateInfo(
    string Name,
    string PointerExpression,
    string ReturnTypeName,
    List<string> ParameterTypeNames,
    bool IsStatic,
    bool IsPrivate);

internal sealed record ExplicitPropertyDirectiveInfo(
    string Name,
    string GetterToken,
    string SetterToken,
    string? TypeName);

internal sealed record ExplicitMethodDirectiveInfo(
    string Name,
    string PointerExpression,
    bool IsStatic,
    bool IsPrivate);

internal sealed record ReflectTypeInfo(
    string ClassName,
    string NamespacePrefix,
    string FullTypeName,
    string HeaderPath,
    string SourceFilePath,
    List<ReflectBaseInfo> Bases,
    List<ReflectFieldInfo> Fields,
    List<ReflectMethodInfo> Methods,
    bool IsEnum = false,
    List<ReflectEnumValueInfo>? EnumValues = null)
{
    public string QualifiedName => FullTypeName;
}

internal sealed record TextMemberDiscoverySummary(
    int InlineFieldCount,
    int ExplicitPropertyFieldCount,
    int AutoPropertyFieldCount,
    int RejectedFieldCount,
    int TotalFieldCount,
    int InlineMethodCount,
    int ExplicitMethodCount,
    int RejectedMethodCount,
    int OverloadRejectedMethodCount,
    int TotalMethodCount);

internal sealed record TextMemberParseResult(
    List<ReflectFieldInfo> Fields,
    List<ReflectMethodInfo> Methods,
    TextMemberDiscoverySummary Summary);

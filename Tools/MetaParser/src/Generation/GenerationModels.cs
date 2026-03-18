public sealed record GeneratedFieldTemplateModel(
    string Name,
    string TypeName,
    string GetterExpression,
    string SetterExpression,
    bool IsPrivate,
    string AccessorName);

public sealed record GeneratedMethodTemplateModel(
    string Name,
    string PointerExpression,
    bool IsStatic,
    bool IsPrivate,
    string AccessorName);

public sealed record GeneratedPrivateFieldTemplateModel(
    string Name,
    string AccessorName);

public sealed record GeneratedPrivateMethodTemplateModel(
    string Name,
    string AccessorName,
    string PointerExpression);

public sealed record GeneratedTypeTemplateModel(
    string ClassName,
    string QualifiedName,
    bool EnableObjectBridge,
    string AccessStructName,
    string RegisterFunctionName,
    string RegistrarClassName,
    string GeneratedBodyMacroName,
    bool IsEnum,
    List<string> EnumValues,
    List<string> Bases,
    List<GeneratedFieldTemplateModel> Fields,
    List<GeneratedMethodTemplateModel> Methods);

public sealed record GeneratedPrivateTypeTemplateModel(
    string ClassName,
    string QualifiedName,
    bool EnableObjectBridge,
    string AccessStructName,
    string RegisterFunctionName,
    string RegistrarClassName,
    string GeneratedBodyMacroName,
    List<GeneratedPrivateFieldTemplateModel> PrivateFields,
    List<GeneratedPrivateMethodTemplateModel> PrivateMethods);

public sealed record HeaderTemplateModel(
    string HeaderPath,
    string FileId,
    string HeaderIncludePath,
    string GeneratedHeaderIncludePath,
    bool RequiresGeneratedHeaderIncludeInSource,
    List<GeneratedTypeTemplateModel> Types,
    List<GeneratedPrivateTypeTemplateModel> PrivateTypes);

public sealed record ModuleTemplateModel(
    string SanitizedTargetName,
    string LinkFunctionName,
    List<string> GeneratedSourceIncludes);

public sealed record ModuleHeaderTemplateModel(
    string LinkFunctionName);

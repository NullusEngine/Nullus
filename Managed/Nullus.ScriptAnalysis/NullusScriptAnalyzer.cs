using System;
using System.Collections.Immutable;
using System.IO;
using System.Linq;
using System.Collections.Generic;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Diagnostics;

namespace Nullus.ScriptAnalysis;

[DiagnosticAnalyzer(LanguageNames.CSharp)]
public sealed class NullusScriptAnalyzer : DiagnosticAnalyzer
{
    private static readonly ImmutableHashSet<string> LifecycleNames = ImmutableHashSet.Create(
        "Awake", "Start", "OnEnable", "OnDisable", "Update", "FixedUpdate", "LateUpdate", "OnDestroy");

    public override ImmutableArray<DiagnosticDescriptor> SupportedDiagnostics => ImmutableArray.Create(
        ScriptAnalysisRules.PublicBehaviourName,
        ScriptAnalysisRules.InvalidLifecycle,
        ScriptAnalysisRules.AsyncLifecycle,
        ScriptAnalysisRules.UnsupportedField,
        ScriptAnalysisRules.InvalidFieldModifiers,
        ScriptAnalysisRules.InvalidFormerlySerializedAs,
        ScriptAnalysisRules.DuplicateSerializedFieldId);

    public override void Initialize(AnalysisContext context)
    {
        context.ConfigureGeneratedCodeAnalysis(GeneratedCodeAnalysisFlags.None);
        context.EnableConcurrentExecution();
        context.RegisterSymbolAction(AnalyzeType, SymbolKind.NamedType);
    }

    private static void AnalyzeType(SymbolAnalysisContext context)
    {
        if (context.Symbol is not INamedTypeSymbol type || type.TypeKind != TypeKind.Class || type.DeclaredAccessibility != Accessibility.Public)
            return;
        if (type.Name == "Behaviour" && type.ContainingNamespace.ToDisplayString() == "Nullus")
            return;
        var behaviour = context.Compilation.GetTypeByMetadataName("Nullus.Behaviour");
        if (behaviour == null || !DerivesFrom(type, behaviour))
            return;

        var declaration = type.DeclaringSyntaxReferences.FirstOrDefault();
        if (declaration != null)
        {
            var syntax = declaration.GetSyntax(context.CancellationToken);
            var path = syntax.SyntaxTree.FilePath;
            if (!string.IsNullOrWhiteSpace(path) && !string.Equals(Path.GetFileNameWithoutExtension(path), type.Name, StringComparison.Ordinal))
                context.ReportDiagnostic(Diagnostic.Create(ScriptAnalysisRules.PublicBehaviourName, syntax.GetLocation(), type.Name, type.Name));
        }

        var serializedNames = new Dictionary<string, IFieldSymbol>(StringComparer.Ordinal);
        foreach (var field in type.GetMembers().OfType<IFieldSymbol>())
        {
            if (!field.DeclaredAccessibility.Equals(Accessibility.Public) && !HasSerializeField(field))
                continue;
            if (field.IsStatic || field.IsConst || field.IsReadOnly)
            {
                context.ReportDiagnostic(Diagnostic.Create(ScriptAnalysisRules.InvalidFieldModifiers, field.Locations.FirstOrDefault(), field.Name));
            }
            else if (!IsPortableFieldType(field.Type))
                context.ReportDiagnostic(Diagnostic.Create(ScriptAnalysisRules.UnsupportedField, field.Locations.FirstOrDefault(), field.Name, field.Type.ToDisplayString()));

            AddSerializedName(context, serializedNames, field.Name, field);
            foreach (var attribute in field.GetAttributes().Where(IsFormerlySerializedAs))
            {
                var oldName = attribute.ConstructorArguments.Length == 1 &&
                    attribute.ConstructorArguments[0].Value is string value ? value : string.Empty;
                if (string.IsNullOrWhiteSpace(oldName) || string.Equals(oldName, field.Name, StringComparison.Ordinal))
                {
                    context.ReportDiagnostic(Diagnostic.Create(
                        ScriptAnalysisRules.InvalidFormerlySerializedAs,
                        attribute.ApplicationSyntaxReference?.GetSyntax(context.CancellationToken).GetLocation() ?? field.Locations.FirstOrDefault(),
                        field.Name));
                    continue;
                }
                AddSerializedName(context, serializedNames, oldName, field);
            }
        }

        foreach (var method in type.GetMembers().OfType<IMethodSymbol>())
        {
            if (!LifecycleNames.Contains(method.Name))
                continue;
            if (method.IsAsync)
            {
                context.ReportDiagnostic(Diagnostic.Create(ScriptAnalysisRules.AsyncLifecycle, method.Locations.FirstOrDefault(), method.Name));
                continue;
            }
            if (method.IsStatic || !method.IsOverride || method.DeclaredAccessibility != Accessibility.Public ||
                method.Parameters.Length != 0 || !method.ReturnsVoid)
                context.ReportDiagnostic(Diagnostic.Create(ScriptAnalysisRules.InvalidLifecycle, method.Locations.FirstOrDefault(), method.Name));
        }
    }

    private static void AddSerializedName(
        SymbolAnalysisContext context,
        IDictionary<string, IFieldSymbol> names,
        string name,
        IFieldSymbol field)
    {
        if (names.TryGetValue(name, out var existing) && !SymbolEqualityComparer.Default.Equals(existing, field))
        {
            context.ReportDiagnostic(Diagnostic.Create(
                ScriptAnalysisRules.DuplicateSerializedFieldId,
                field.Locations.FirstOrDefault(),
                name));
            return;
        }
        names[name] = field;
    }

    private static bool DerivesFrom(INamedTypeSymbol type, INamedTypeSymbol baseType)
    {
        for (var current = type; current != null; current = current.BaseType)
            if (SymbolEqualityComparer.Default.Equals(current, baseType)) return true;
        return false;
    }

    private static bool HasSerializeField(IFieldSymbol field)
        => field.GetAttributes().Any(attribute => attribute.AttributeClass?.Name is "SerializeFieldAttribute" or "SerializeField");

    private static bool IsFormerlySerializedAs(AttributeData attribute)
        => attribute.AttributeClass?.Name is "FormerlySerializedAsAttribute" or "FormerlySerializedAs";

    private static bool IsPortableFieldType(ITypeSymbol type)
    {
        if (type.TypeKind == TypeKind.Enum || type.SpecialType is SpecialType.System_Boolean or SpecialType.System_Byte or SpecialType.System_SByte or SpecialType.System_Int16 or SpecialType.System_UInt16 or SpecialType.System_Int32 or SpecialType.System_UInt32 or SpecialType.System_Int64 or SpecialType.System_UInt64 or SpecialType.System_Single or SpecialType.System_Double or SpecialType.System_String)
            return true;
        var display = type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
        return display is "global::Nullus.NativeObjectHandle" or "global::Nullus.GameObject" or "global::Nullus.Transform" or "global::Nullus.Vector2" or "global::Nullus.Vector3" or "global::Nullus.Vector4" or "global::Nullus.Quaternion" or "global::Nullus.Color";
    }
}

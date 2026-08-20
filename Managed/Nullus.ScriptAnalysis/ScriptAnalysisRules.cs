using Microsoft.CodeAnalysis;

namespace Nullus.ScriptAnalysis;

internal static class ScriptAnalysisRules
{
    public static readonly DiagnosticDescriptor PublicBehaviourName = new(
        "NLS100", "Behaviour class name must match the file", "Public Behaviour '{0}' must be declared in '{1}.cs'", "Nullus", DiagnosticSeverity.Error, true);
    public static readonly DiagnosticDescriptor InvalidLifecycle = new(
        "NLS101", "Invalid Nullus lifecycle method", "Lifecycle method '{0}' must override the Nullus.Behaviour signature", "Nullus", DiagnosticSeverity.Error, true);
    public static readonly DiagnosticDescriptor AsyncLifecycle = new(
        "NLS102", "Async lifecycle methods are unsupported", "Nullus lifecycle method '{0}' cannot be async", "Nullus", DiagnosticSeverity.Error, true);
    public static readonly DiagnosticDescriptor UnsupportedField = new(
        "NLS103", "Unsupported serialized field", "Serialized field '{0}' uses unsupported type '{1}'", "Nullus", DiagnosticSeverity.Error, true);
    public static readonly DiagnosticDescriptor InvalidFieldModifiers = new(
        "NLS104", "Invalid serialized field modifiers", "Serialized field '{0}' cannot be static, const, or readonly", "Nullus", DiagnosticSeverity.Error, true);
    public static readonly DiagnosticDescriptor InvalidFormerlySerializedAs = new(
        "NLS105", "Invalid FormerlySerializedAs attribute", "FormerlySerializedAs on field '{0}' must contain a non-empty old field name different from the current name", "Nullus", DiagnosticSeverity.Error, true);
    public static readonly DiagnosticDescriptor DuplicateSerializedFieldId = new(
        "NLS106", "Duplicate serialized field identity", "Serialized field alias '{0}' is already used by another field in this Behaviour", "Nullus", DiagnosticSeverity.Error, true);
}

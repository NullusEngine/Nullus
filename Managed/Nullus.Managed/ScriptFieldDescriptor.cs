namespace Nullus;

[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = true)]
public sealed class SerializeFieldAttribute : Attribute;

[AttributeUsage(AttributeTargets.Field, AllowMultiple = true, Inherited = true)]
public sealed class FormerlySerializedAsAttribute(string oldName) : Attribute
{
    public string OldName { get; } = oldName;
}

// Field attributes are declarative metadata only. The native Inspector maps
// them to its shared drawer contract; managed code never supplies UI callbacks.
[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = true)]
public sealed class DisplayNameAttribute(string value) : Attribute
{
    public string Value { get; } = value;
}

[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = true)]
public sealed class TooltipAttribute(string value) : Attribute
{
    public string Value { get; } = value;
}

[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = true)]
public sealed class HeaderAttribute(string value) : Attribute
{
    public string Value { get; } = value;
}

[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = true)]
public sealed class SpaceAttribute(float value) : Attribute
{
    public float Value { get; } = value;
}

[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = true)]
public sealed class RangeAttribute(float minimum, float maximum) : Attribute
{
    public float Minimum { get; } = minimum;
    public float Maximum { get; } = maximum;
}

[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = true)]
public sealed class MinAttribute(float value) : Attribute
{
    public float Value { get; } = value;
}

[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = true)]
public sealed class MaxAttribute(float value) : Attribute
{
    public float Value { get; } = value;
}

[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = true)]
public sealed class MultilineAttribute : Attribute;

[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = true)]
public sealed class ReadOnlyAttribute : Attribute;

[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = true)]
public sealed class HideInInspectorAttribute : Attribute;

[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = true)]
public sealed class FlagsAttribute : Attribute;

[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = true)]
public sealed class ColorUsageAttribute : Attribute;

[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = true)]
public sealed class ReferenceTypeConstraintAttribute(string typeName) : Attribute
{
    public string TypeName { get; } = typeName;
}

public sealed record ManagedScriptFieldMetadata(
    string DisplayName,
    string Tooltip,
    string Header,
    float? SpaceBefore,
    float? RangeMin,
    float? RangeMax,
    float? Min,
    float? Max,
    bool Multiline,
    bool ReadOnly,
    bool HideInInspector,
    bool Flags,
    bool ColorUsage,
    string ReferenceTypeConstraint)
{
    public static ManagedScriptFieldMetadata Empty { get; } = new(
        string.Empty,
        string.Empty,
        string.Empty,
        null,
        null,
        null,
        null,
        null,
        false,
        false,
        false,
        false,
        false,
        string.Empty);
}

public sealed record ManagedScriptFieldDescriptor(
    ulong Id,
    string Name,
    string TypeName,
    IReadOnlyList<string> Aliases,
    ManagedScriptFieldMetadata Metadata)
{
    public ManagedScriptFieldDescriptor(
        ulong id,
        string name,
        string typeName,
        IReadOnlyList<string> aliases)
        : this(id, name, typeName, aliases, ManagedScriptFieldMetadata.Empty)
    {
    }
}

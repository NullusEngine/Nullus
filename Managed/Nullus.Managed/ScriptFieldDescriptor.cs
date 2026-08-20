namespace Nullus;

[AttributeUsage(AttributeTargets.Field, AllowMultiple = false, Inherited = true)]
public sealed class SerializeFieldAttribute : Attribute;

[AttributeUsage(AttributeTargets.Field, AllowMultiple = true, Inherited = true)]
public sealed class FormerlySerializedAsAttribute(string oldName) : Attribute
{
    public string OldName { get; } = oldName;
}

public sealed record ManagedScriptFieldDescriptor(
    ulong Id,
    string Name,
    string TypeName,
    IReadOnlyList<string> Aliases);

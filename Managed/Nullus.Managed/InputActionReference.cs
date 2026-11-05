using System.Buffers.Binary;
using System.Runtime.CompilerServices;

namespace Nullus;

// A persistent reference contains definition/action identities, never player state.
// The field ABI stores the canonical "definition-id/action-id" string so the
// existing scene/prefab serialization, Undo and reload paths remain shared.
public sealed class InputActionReference
{
    public Guid AssetId { get; }
    public Guid ActionId { get; }
    public bool IsAssigned => AssetId != Guid.Empty && ActionId != Guid.Empty;
    private InputAction? primaryAction;
    public InputActionReference() { }
    public InputActionReference(Guid assetId, Guid actionId) { AssetId = assetId; ActionId = actionId; }
    public InputAction? action => IsAssigned ? primaryAction ??= new InputAction(ActionId, AssetId, 0) : null;
    public InputAction? Resolve(InputUser user)
    {
        ArgumentNullException.ThrowIfNull(user);
        return IsAssigned ? new InputAction(ActionId, AssetId, user.Id) : null;
    }
    public override string ToString() => IsAssigned ? AssetId.ToString() + "/" + ActionId.ToString() : "";
    public static bool TryParse(string? text, out InputActionReference reference)
    {
        reference = new();
        if (string.IsNullOrEmpty(text)) return true;
        if (text.Length != 73 || text[36] != '/' || !Guid.TryParse(text.AsSpan(0, 36), out var asset)
            || !Guid.TryParse(text.AsSpan(37), out var action) || asset == Guid.Empty || action == Guid.Empty) return false;
        reference = new(asset, action); return true;
    }
}

public sealed partial class InputUser
{
    // Resolve once to a stable ID. Subsequent queries survive renames and use
    // this user's runtime rather than another player's/default action instance.
    public InputAction? FindAction(string nameOrId, bool throwIfNotFound = false)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(nameOrId);
        var high = InputActionBindings.Command(Id, 20, nameOrId);
        var low = InputActionBindings.Command(Id, 21, nameOrId);
        if (high == 0 && low == 0)
        {
            if (throwIfNotFound) throw new KeyNotFoundException("Input Action is missing or ambiguous: " + nameOrId);
            return null;
        }
        Span<byte> bytes = stackalloc byte[16];
        BinaryPrimitives.WriteUInt64BigEndian(bytes, high); BinaryPrimitives.WriteUInt64BigEndian(bytes[8..], low);
        return new InputAction(new Guid(bytes, bigEndian: true), Guid.Empty, Id);
    }
    public InputAction this[string nameOrId] => FindAction(nameOrId, throwIfNotFound: true)!;
}

public class PlayerInput : Component
{
    public const string NativeTypeName = "NLS::Engine::Components::PlayerInputComponent";
    internal PlayerInput(NativeObjectHandle handle) : base(handle) { }
    [ModuleInitializer] internal static void Register()
    {
        NativeObject.RegisterFactory<PlayerInput>(h => new PlayerInput(h));
        NativeObject.RegisterFactory<PlayerInputComponent>(h => new PlayerInputComponent(h));
    }
    public InputUser user => new(InputActionBindings.Command(0, 22, "", Handle.Value));
    public InputUser actions => user;
}

// Retain the previously generated native component identity for existing scripts.
public sealed class PlayerInputComponent : PlayerInput
{
    public new const string NativeTypeName = PlayerInput.NativeTypeName;
    internal PlayerInputComponent(NativeObjectHandle handle) : base(handle) { }
}

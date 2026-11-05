using System.Buffers.Binary;
using System.Reflection;
using System.Runtime.InteropServices;
using Nullus;

internal static unsafe class Program
{
    private static readonly Guid JumpId = Guid.Parse("172db044-427e-4b65-9042-9c645648c37f");
    private static readonly Guid AssetId = Guid.Parse("33e8ea12-6186-454e-8691-774f4ff9f400");
    private static ulong lastUser;
    private static uint lastOperation;
    private static string lastSelector = "";
    private static int checks;
    private static void Require(bool value, string message)
    { ++checks; if (!value) throw new InvalidOperationException(message); }
    [UnmanagedCallersOnly]
    private static ScriptAbiResult Command(ulong user, uint operation, byte* selector, ulong argument, ulong* result)
    {
        lastUser = user; lastOperation = operation;
        lastSelector = Marshal.PtrToStringUTF8((IntPtr)selector) ?? "";
        *result = 0;
        if (operation == 20 || operation == 21)
        {
            if (lastSelector == "Gameplay/Jump")
            {
                Span<byte> bytes = stackalloc byte[16]; JumpId.TryWriteBytes(bytes, bigEndian: true, out _);
                *result = BinaryPrimitives.ReadUInt64BigEndian(operation == 20 ? bytes : bytes[8..]);
            }
        }
        else if (operation == 22) *result = argument + 100;
        return default;
    }
    [UnmanagedCallersOnly]
    private static ScriptAbiResult Read(ulong user, byte* selector, uint operation, ulong after, ScriptInputValue* result)
    {
        lastUser = user; lastOperation = operation;
        lastSelector = Marshal.PtrToStringUTF8((IntPtr)selector) ?? "";
        *result = new ScriptInputValue { X = user == 2 ? 0 : 1, Y = 0.25f, Flags = user == 2 ? 64u : 67u };
        return default;
    }
    private static void Configure()
    {
        var method = typeof(NativeBinding).GetMethod("Configure", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic)!;
        method.Invoke(null, new object[] { new NativeApiTable { ReadInputAction = &Read, InputCommand = &Command } });
        var guard = typeof(Behaviour).Assembly.GetType("Nullus.PlaySessionGuard")!;
        guard.GetMethod("Configure", BindingFlags.Static | BindingFlags.NonPublic)!.Invoke(null, null);
    }
    public static int Main()
    {
        Configure();
        Require(InputActionReference.TryParse("", out var empty) && !empty.IsAssigned && empty.action is null, "Empty reference");
        Require(!InputActionReference.TryParse("Jump", out _), "Names must not become persistent identities");
        var reference = new InputActionReference(AssetId, JumpId);
        Require(InputActionReference.TryParse(reference.ToString(), out var roundTrip) && roundTrip.ActionId == JumpId && roundTrip.AssetId == AssetId, "Reference round trip");
        Require(ScriptValueConversion.TryConvert(reference.ToString(), typeof(InputActionReference), out var converted)
            && ((InputActionReference)converted!).ActionId == JumpId, "Field decoder");
        Require(!ScriptValueConversion.TryConvert("malformed", typeof(InputActionReference), out _), "Invalid reference rejected");
        var buffers = new List<IntPtr>();
        try
        {
            ScriptAbiValue value = default;
            Require(ScriptAbiCodec.TryEncode(reference, &value, buffers) && value.Kind == 12, "Field uses the existing string ABI");
            Require(ScriptAbiCodec.TryDecode(value, out var decoded) && (string)decoded! == reference.ToString(), "Field encoder");
        }
        finally { foreach (var pointer in buffers) Marshal.FreeCoTaskMem(pointer); }
        var first = new InputUser(1); var second = new InputUser(2);
        var jump = first.FindAction("Gameplay/Jump", throwIfNotFound: true)!;
        Require(jump.Id == JumpId, "Name resolves canonical UUID, including byte order");
        Require(jump.WasPressedThisFrame() && lastUser == 1 && lastSelector == JumpId.ToString(), "Resolved action keeps its user");
        Require(first.FindAction("Missing") is null, "Missing action returns null");
        try { _ = first["Missing"]; throw new Exception("Indexer accepted missing action"); } catch (KeyNotFoundException) { ++checks; }
        Require(!reference.Resolve(second)!.IsPressed() && lastUser == 2, "Reference resolves the selected player's state");
        Require(reference.Resolve(first)!.ReadValue<Vector2>().x == 1 && lastSelector == reference.ToString(), "Typed Vector2 and asset identity");
        Require(reference.Resolve(first)!.ReadValue<float>(InputUpdateMode.Fixed) == 1 && lastOperation == 1, "Fixed snapshot");
        var controls = new PlayerControlsActions(second);
        Require(!controls.Gameplay.Jump.WasPressedThisFrame() && lastUser == 2, "Generated wrapper keeps its user");
        Require(controls.Gameplay.Move.ReadValue<Vector2>().y == .25f, "Generated typed move");
        controls.Gameplay.Enable(); Require(lastOperation == 2 && lastUser == 2, "Generated map enable");
        controls.Gameplay.Disable(); Require(lastOperation == 3 && lastUser == 2, "Generated map disable");
        var registry = typeof(JumpInput).Assembly.GetTypes().Single(type => type.Name == "ScriptFieldRegistry");
        var fields = (IReadOnlyList<ManagedScriptFieldDescriptor>)registry.GetMethod("Get", BindingFlags.Static | BindingFlags.Public)!.Invoke(null, new object[] { "global::JumpInput" })!;
        Require(fields.Any(field => field.Name == "jump" && field.TypeName.Contains("InputActionReference")), "Source generator includes private serialized Action reference");
        var field = fields.Single(field => field.Name == "jump");
        var access = typeof(JumpInput).Assembly.GetTypes().Single(type => type.Name == "ScriptFieldAccess");
        var instance = new JumpInput();
        Require((bool)access.GetMethod("TrySet")!.Invoke(null, new object[] { instance, field.Id, reference.ToString() })!, "Generated setter accepts serialized reference");
        object?[] get = { instance, field.Id, null };
        Require((bool)access.GetMethod("TryGet")!.Invoke(null, get)! && get[2] is InputActionReference restored && restored.AssetId == AssetId && restored.ActionId == JumpId,
            "Generated getter restores the reference into a real Behaviour field");
        Console.WriteLine($"Input usage managed smoke: {checks} checks passed.");
        return 0;
    }
}

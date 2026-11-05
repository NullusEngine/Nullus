using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Nullus;

public enum InputActionPhase : uint { Started, Pressed, Released, Performed, Canceled, ValueChanged }
public enum InputUpdateMode : uint { Dynamic, Fixed }
[StructLayout(LayoutKind.Sequential)]
public struct ScriptInputValue
{
    public float X, Y, Z;
    public uint Flags;
    public ulong Sequence, Tick, Epoch;
    public double Timestamp;
    public InputActionPhase Phase;
    public uint CancelReason;
    public readonly Vector3 Value => new(X, Y, Z);
    public readonly bool IsActuated => (Flags & 1) != 0;
    public readonly bool IsAvailable => (Flags & 64) != 0;
    public readonly bool WasPressed => (Flags & 2) != 0;
    public readonly bool WasReleased => (Flags & 4) != 0;
    public readonly bool WasPerformed => (Flags & 8) != 0;
}

// Immutable Action reference; the UTF-8 identity is cached and pinning does not
// allocate per query. Native resolves the current definition generation by id.
public sealed class InputAction
{
    internal readonly byte[] Identity;
    private readonly ulong ownerUser;
    public Guid Id { get; }
    public Guid AssetId { get; }
    public InputAction(Guid id) : this(id, Guid.Empty, 0) { }
    internal InputAction(Guid id, Guid asset, ulong user)
    {
        Id = id; AssetId = asset; ownerUser = user;
        Identity = Encoding.UTF8.GetBytes((asset == Guid.Empty ? "" : asset.ToString() + "/") + id.ToString() + "\0");
    }
    private ulong User(ulong user) => user == 0 ? ownerUser : user;
    public ScriptInputValue GetState(ulong user = 0, InputUpdateMode mode = InputUpdateMode.Dynamic) => InputActionBindings.Read(User(user), Identity, (uint)mode, 0);
    public Vector3 GetValue(ulong user = 0, InputUpdateMode mode = InputUpdateMode.Dynamic) => GetState(user, mode).Value;
    public bool WasPressed(ulong user = 0, InputUpdateMode mode = InputUpdateMode.Dynamic) => GetState(user, mode).WasPressed;
    public bool WasReleased(ulong user = 0, InputUpdateMode mode = InputUpdateMode.Dynamic) => GetState(user, mode).WasReleased;
    public IDisposable BindAction(Behaviour owner, InputActionPhase phase, Action<ScriptInputValue> callback, ulong user = 0, InputUpdateMode mode = InputUpdateMode.Dynamic)
        => InputActionBindings.Bind(owner, this, phase, callback, User(user), mode);
    public bool IsPressed() => GetState().IsActuated;
    public bool WasPressedThisFrame() => WasPressed();
    public bool WasReleasedThisFrame() => WasReleased();
    public T ReadValue<T>(InputUpdateMode mode = InputUpdateMode.Dynamic) where T : struct
    {
        var state = GetState(mode: mode);
        if (typeof(T) == typeof(Vector3)) { var value = state.Value; return Unsafe.As<Vector3, T>(ref value); }
        if (typeof(T) == typeof(Vector2)) { var value = new Vector2(state.X, state.Y); return Unsafe.As<Vector2, T>(ref value); }
        if (typeof(T) == typeof(float)) { var value = state.X; return Unsafe.As<float, T>(ref value); }
        if (typeof(T) == typeof(bool)) { var value = state.IsActuated; return Unsafe.As<bool, T>(ref value); }
        throw new NotSupportedException("Input values support bool, float, Vector2 and Vector3.");
    }
}
public enum InputDeviceKind : uint { Keyboard, Mouse, Gamepad, Unknown }
public sealed class InputDevice
{
    public ulong Id { get; }
    public InputDeviceKind Kind { get; }
    public bool IsConnected => InputActionBindings.Command(0, 17, "", Id) != 0;
    public bool IsMapped => InputActionBindings.Command(0, 18, "", Id) != 0;
    private InputDevice(ulong id) { Id = id; Kind = (InputDeviceKind)InputActionBindings.Command(0, 16, "", id); }
    public static IReadOnlyList<InputDevice> All
    {
        get
        {
            var devices = new List<InputDevice>();
            for (ulong index = 0;; ++index)
            {
                var id = InputActionBindings.Command(0, 15, "", index);
                if (id == 0) return devices;
                devices.Add(new(id));
            }
        }
    }
}
public sealed partial class InputUser
{
    public static IReadOnlyList<InputUser> All
    {
        get { var users = new List<InputUser>(); for (ulong index = 0;;++index) { var id = InputActionBindings.Command(0,14,"",index); if (id == 0) return users; users.Add(new(id)); } }
    }
    public ulong Id { get; }
    public InputUser(ulong id = 0) { Id = id; }
    public void PushContext(Guid id, int priorityOffset = 0) => InputActionBindings.Command(Id, 0, id.ToString(), unchecked((ulong)priorityOffset));
    public void PopContext(Guid id) => InputActionBindings.Command(Id, 1, id.ToString());
    public void EnableContext(Guid id) => InputActionBindings.Command(Id, 2, id.ToString());
    public void DisableContext(Guid id) => InputActionBindings.Command(Id, 3, id.ToString());
    public void RestoreBindingDefault(Guid id) => InputActionBindings.Command(Id, 4, id.ToString());
    public void ClearBinding(Guid id) => InputActionBindings.Command(Id, 5, id.ToString());
    public void PairDevice(ulong device) => InputActionBindings.Command(Id, 6, "", device);
    public void UnpairDevice(ulong device) => InputActionBindings.Command(Id, 7, "", device);
    public string GetBindingDisplayName(Guid id) => InputActionBindings.BindingName(Id, id.ToString());
    public InputRebindingSession Rebind(Guid binding) => new(Id, InputActionBindings.Command(Id, 8, binding.ToString()));
    public void SaveProfile(string path) => InputActionBindings.Command(Id, 12, path);
    public void LoadProfile(string path) => InputActionBindings.Command(Id, 13, path);
}
public enum InputRebindingStatus : ulong { WaitingForRelease, Listening, Completed, Canceled, TimedOut }
public sealed class InputRebindingSession : IDisposable
{
    private readonly ulong user;
    private readonly long epoch = PlaySessionGuard.Epoch;
    private ulong token;
    internal InputRebindingSession(ulong user, ulong token) { this.user = user; this.token = token; }
    public InputRebindingStatus Update() => token == 0 || !PlaySessionGuard.Owns(epoch) ? InputRebindingStatus.Canceled : (InputRebindingStatus)InputActionBindings.Command(user, 9, "", token);
    public int ConflictCount => token == 0 || !PlaySessionGuard.Owns(epoch) ? 0 : checked((int)InputActionBindings.Command(user, 19, "", token));
    public void Apply()
    {
        if (token == 0 || !PlaySessionGuard.Owns(epoch)) throw new ObjectDisposedException(nameof(InputRebindingSession));
        var current = token; token = 0; InputActionBindings.Command(user, 10, "", current);
    }
    public void Dispose() { if (token != 0) { var current = token; token = 0; if (PlaySessionGuard.Owns(epoch)) InputActionBindings.Command(user, 11, "", current); } }
}
internal static unsafe class InputActionBindings
{
    internal delegate ScriptInputValue ReadDelegate(ulong user, byte[] id, uint operation, ulong after);
    internal delegate ulong CommandDelegate(ulong user, uint operation, string id, ulong argument = 0);
    internal static ReadDelegate Read = (_, _, _, _) => throw new NotSupportedException("The host does not expose Input Actions.");
    internal static CommandDelegate Command = (_, _, _, _) => throw new NotSupportedException("The host does not expose Input Actions.");
    internal static Func<ulong, string, string> BindingName = (_, _) => throw new NotSupportedException("The host does not expose Input Actions.");
    private sealed class Subscription : IDisposable
    {
        internal InputAction Action = null!;
        internal Action<ScriptInputValue>? Callback;
        internal InputActionPhase Phase;
        internal InputUpdateMode Mode;
        internal bool Suspended;
        internal ulong User, Sequence;
        public void Dispose() => Callback = null;
    }
    private static ConditionalWeakTable<Behaviour, List<Subscription>> subscriptions = new();
    internal static IDisposable Bind(Behaviour owner, InputAction action, InputActionPhase phase, Action<ScriptInputValue> callback, ulong user, InputUpdateMode mode)
    {
        ArgumentNullException.ThrowIfNull(owner); ArgumentNullException.ThrowIfNull(callback);
        var slot = new Subscription { Action = action, Phase = phase, Callback = callback, User = user, Mode = mode };
        subscriptions.GetOrCreateValue(owner).Add(slot); return slot;
    }
    internal static void Dispatch(Behaviour owner, ushort callback)
    {
        if (!subscriptions.TryGetValue(owner, out var slots)) return;
        if (callback == 7) { foreach (var slot in slots) slot.Dispose(); subscriptions.Remove(owner); return; }
        if (callback == 2 || callback == 3)
        {
            foreach (var slot in slots)
            {
                if (callback == 2 && slot.Suspended)
                    for (;;) { var value = Read(slot.User,slot.Action.Identity,slot.Mode == InputUpdateMode.Fixed ? 3u : 2u,slot.Sequence); if ((value.Flags&32)==0) break; slot.Sequence=value.Sequence; }
                slot.Suspended = callback == 3;
            }
            return;
        }
        if (callback != 4 && callback != 5) return;
        var count = slots.Count; var dispatchGeneration = generation;
        for (var i = 0; i < count; ++i)
        {
            if (dispatchGeneration != generation) break;
            var slot = slots[i];
            if (slot.Callback == null || slot.Suspended || (callback == 5) != (slot.Mode == InputUpdateMode.Fixed)) continue;
            for (;;)
            {
                var value = Read(slot.User, slot.Action.Identity, slot.Mode == InputUpdateMode.Fixed ? 3u : 2u, slot.Sequence);
                if ((value.Flags & 32) == 0) break;
                slot.Sequence = value.Sequence;
                if (slot.Callback != null && value.Phase == slot.Phase)
                { try { slot.Callback(value); } catch (Exception error) { Debug.LogError(error.ToString()); } }
                if (dispatchGeneration != generation) break;
                if (slot.Callback == null || slot.Suspended) break;
            }
        }
        slots.RemoveAll(static slot => slot.Callback == null);
    }
    internal static void Reset() { ++generation; subscriptions = new(); }
    private static int generation;
    internal static void Install(NativeApiTable table)
    {
        Reset();
        Read = (_, _, _, _) => throw new NotSupportedException("The host does not expose Input Actions.");
        Command = (_, _, _, _) => throw new NotSupportedException("The host does not expose Input Actions.");
        BindingName = (_, _) => throw new NotSupportedException("The host does not expose Input Actions.");
        if (table.ReadInputAction != null) Read = (user, id, op, after) =>
        { PlaySessionGuard.Check(); fixed (byte* text = id) { ScriptInputValue value = default; NativeBindingStore.ThrowOnFailure(table.ReadInputAction(user, text, op, after, &value), "Input Action query failed."); return value; } };
        if (table.InputCommand != null) Command = (user, op, id, argument) =>
        { PlaySessionGuard.Check(); var text = Marshal.StringToCoTaskMemUTF8(id); try { ulong result = 0; NativeBindingStore.ThrowOnFailure(table.InputCommand(user, op, (byte*)text, argument, &result), "Input command failed."); return result; } finally { Marshal.FreeCoTaskMem(text); } };
        if (table.GetInputBindingName != null) BindingName = (user, id) =>
        { PlaySessionGuard.Check(); var text = Marshal.StringToCoTaskMemUTF8(id); try { byte* output = null; uint size = 0; NativeBindingStore.ThrowOnFailure(table.GetInputBindingName(user, (byte*)text, &output, &size), "Binding query failed."); return Marshal.PtrToStringUTF8((IntPtr)output, (int)size) ?? ""; } finally { Marshal.FreeCoTaskMem(text); } };
    }
}

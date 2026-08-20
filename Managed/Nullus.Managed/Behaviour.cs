using System.Collections.Concurrent;
using System.ComponentModel;
using System.Diagnostics;
using System.Linq.Expressions;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Reflection;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Nullus;

[StructLayout(LayoutKind.Sequential)]
[DebuggerDisplay("Handle = {Value}, Valid = {IsValid}")]
public readonly struct NativeObjectHandle : IEquatable<NativeObjectHandle>
{
    public NativeObjectHandle(ulong value) => Value = value;
    public ulong Value { get; }
    public bool IsValid => Value != 0;
    public bool Equals(NativeObjectHandle other) => Value == other.Value;
    public override bool Equals(object? obj) => obj is NativeObjectHandle other && Equals(other);
    public override int GetHashCode() => Value.GetHashCode();
    public static bool operator ==(NativeObjectHandle left, NativeObjectHandle right) => left.Equals(right);
    public static bool operator !=(NativeObjectHandle left, NativeObjectHandle right) => !left.Equals(right);
}

public sealed class MissingReferenceException : InvalidOperationException
{
    public MissingReferenceException(string message) : base(message) { }
}

[DebuggerDisplay("{DebuggerDisplayValue,nq}")]
public abstract class NativeObject
{
    private static readonly ConcurrentDictionary<(ulong Handle, Type Type), WeakReference<NativeObject>> Cache = new();
    private static readonly ConcurrentDictionary<Type, Func<NativeObjectHandle, NativeObject>> Factories = new();
    private bool _destroyed;
    private NativeObjectHandle _handle;

    protected NativeObject() { }
    internal NativeObject(NativeObjectHandle handle) => _handle = handle;
    public NativeObjectHandle Handle => _handle;
    private string DebuggerDisplayValue => IsDestroyed ? "Destroyed (null)" : $"{GetType().Name} ({Handle.Value})";

    // The native host binds a component handle after invoking the user's
    // parameterless constructor.  It is intentionally not a constructor so
    // script authors never need to know about ABI handles.
    [EditorBrowsable(EditorBrowsableState.Never)]
    public void BindNativeHandle(NativeObjectHandle handle)
    {
        _handle = handle;
        _destroyed = false;
        NativeBinding.MarkAlive(handle.Value);
    }

    public static void RegisterFactory<T>(Func<NativeObjectHandle, T> factory)
        where T : NativeObject
    {
        ArgumentNullException.ThrowIfNull(factory);
        Factories[typeof(T)] = handle => factory(handle);
    }

    public static T? FromHandle<T>(NativeObjectHandle handle) where T : NativeObject
    {
        if (!handle.IsValid)
            return null;
        NativeObjectFactories.Ensure();
        var key = (handle.Value, typeof(T));
        if (Cache.TryGetValue(key, out var weak) && weak.TryGetTarget(out var existing))
            return (T)existing;
        T created;
        if (Factories.TryGetValue(typeof(T), out var factory))
            created = (T)factory(handle);
        else
        {
            if (typeof(T).IsAbstract)
                throw new InvalidOperationException($"No generated NativeObject factory is registered for '{typeof(T).FullName}'.");
            created = (T)(Activator.CreateInstance(typeof(T), nonPublic: true)
                ?? throw new InvalidOperationException($"Could not construct NativeObject '{typeof(T).FullName}'."));
            created.BindNativeHandle(handle);
        }
        return RegisterBoundInstance(created);
    }

    internal static T RegisterBoundInstance<T>(T instance) where T : NativeObject
    {
        var key = (instance.Handle.Value, typeof(T));
        if (Cache.TryGetValue(key, out var weak) && weak.TryGetTarget(out var existing))
            return (T)existing;
        Cache[key] = new WeakReference<NativeObject>(instance);
        return instance;
    }

    public bool IsDestroyed => _destroyed || !NativeBinding.IsHandleAlive(Handle.Value);
    protected void ThrowIfDestroyed()
    {
        if (IsDestroyed)
            throw new MissingReferenceException($"Native object {Handle.Value} has been destroyed.");
    }
    internal void MarkDestroyed() => _destroyed = true;
    public static bool operator ==(NativeObject? left, NativeObject? right)
        => ReferenceEquals(left, right) || (left is null ? right?.IsDestroyed == true : right is null ? left.IsDestroyed : left.Handle == right.Handle);
    public static bool operator !=(NativeObject? left, NativeObject? right) => !(left == right);
    public override bool Equals(object? obj) => obj is NativeObject other && Handle == other.Handle;
    public override int GetHashCode() => Handle.GetHashCode();
}

public abstract class Component : Object
{
    protected Component() { }
    internal Component(NativeObjectHandle handle) : base(handle) { }

    public GameObject gameObject
    {
        get
        {
            ThrowIfDestroyed();
            return NativeObject.FromHandle<GameObject>(NativeBindingStore.GetObject(
                Handle,
                ScriptApiManifest.MemberId("NLS::Engine::Components::Component", "GetGameObject()")))!;
        }
    }

    public Transform transform => gameObject.transform;
    public string tag
    {
        get { ThrowIfDestroyed(); return NativeBindingStore.GetString(Handle, ScriptApiManifest.MemberId("NLS::Engine::GameObject", "GetTag()")); }
        set { ThrowIfDestroyed(); NativeBindingStore.SetString(Handle, ScriptApiManifest.MemberId("NLS::Engine::GameObject", "SetTag(std::string&)"), value); }
    }

    public bool CompareTag(string value) => string.Equals(tag, value, StringComparison.Ordinal);

    public T? GetComponent<T>() where T : Component
        => NativeObject.FromHandle<T>(NativeBindingStore.GetComponent(Handle, ScriptApiManifest.StableId(typeof(T).FullName ?? typeof(T).Name)));
}

public abstract class Behaviour : Component
{
    protected Behaviour() { }
    internal Behaviour(NativeObjectHandle handle) : base(handle) { }

    private bool _enabled = true;
    public bool enabled
    {
        get { ThrowIfDestroyed(); return NativeBindingStore.GetBool(Handle, ScriptApiManifest.MemberId("NLS::Engine::Components::Component", "IsSelfEnabled()")); }
        set
        {
            ThrowIfDestroyed();
            _enabled = value;
            NativeBindingStore.SetBool(Handle, ScriptApiManifest.MemberId("NLS::Engine::Components::Component", "SetEnabled(bool)"), value);
        }
    }

    public bool isActiveAndEnabled => !IsDestroyed && NativeBindingStore.GetBool(Handle, ScriptApiManifest.MemberId("NLS::Engine::Components::Component", "IsActiveAndEnabled()"));

}

public static class Time
{
    public static float deltaTime { get; internal set; }
    public static float unscaledDeltaTime { get; internal set; }
    public static float fixedDeltaTime { get; set; } = 0.02f;
    public static float maximumDeltaTime { get; set; } = 0.1f;
    private static float _timeScale = 1.0f;
    public static float timeScale
    {
        get => NativeTime.GetTimeScale();
        set { NativeTime.SetTimeScale(value); _timeScale = value < 0.0f ? 0.0f : value; }
    }
    public static float time { get; internal set; }
    public static float unscaledTime { get; internal set; }
    public static int frameCount { get; internal set; }
    public static int fixedFrameCount { get; internal set; }

    internal static void ApplyFrame(ScriptFrameContext frame)
    {
        deltaTime = frame.DeltaTime;
        unscaledDeltaTime = frame.UnscaledDeltaTime;
        time = (float)frame.Time;
        unscaledTime = (float)frame.UnscaledTime;
        frameCount = unchecked((int)frame.FrameIndex);
        fixedDeltaTime = frame.FixedDeltaTime <= 0 ? fixedDeltaTime : frame.FixedDeltaTime;
        _timeScale = frame.TimeScale < 0 ? 0.0f : frame.TimeScale;
        fixedFrameCount = unchecked((int)frame.FixedFrameIndex);
    }
}

public readonly struct Scene
{
    internal Scene(string scenePath, bool loaded)
    {
        path = scenePath ?? string.Empty;
        name = string.IsNullOrEmpty(path)
            ? string.Empty
            : System.IO.Path.GetFileNameWithoutExtension(path);
        buildIndex = -1;
        isLoaded = loaded;
    }

    public string name { get; }
    public string path { get; }
    public int buildIndex { get; }
    public bool isLoaded { get; }
    public bool IsValid() => isLoaded;
}

public static class SceneManager
{
    public static Scene GetActiveScene() => NativeSceneManager.GetActiveScene();
    public static void LoadScene(string scenePath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(scenePath);
        NativeSceneManager.LoadScene(scenePath);
    }
}

public static class Debug
{
    public static void Log(object? message) => NativeBinding.Log(0, message?.ToString() ?? string.Empty);
    public static void LogWarning(object? message) => NativeBinding.Log(1, message?.ToString() ?? string.Empty);
    public static void LogError(object? message) => NativeBinding.Log(2, message?.ToString() ?? string.Empty);
    public static void LogException(Exception exception) => NativeBinding.Log(2, exception.ToString());
    public static void Assert(bool condition, object? message = null)
    {
        if (!condition) LogError(message ?? "Assertion failed.");
    }
}

public enum KeyCode
{
    None = -1,
    Space = 32,
    Apostrophe = 39, Comma = 44, Minus = 45, Period = 46, Slash = 47,
    Alpha0 = 48, Alpha1 = 49, Alpha2 = 50, Alpha3 = 51, Alpha4 = 52,
    Alpha5 = 53, Alpha6 = 54, Alpha7 = 55, Alpha8 = 56, Alpha9 = 57,
    A = 65, B = 66, C = 67, D = 68, E = 69, F = 70, G = 71, H = 72,
    I = 73, J = 74, K = 75, L = 76, M = 77, N = 78, O = 79, P = 80,
    Q = 81, R = 82, S = 83, T = 84, U = 85, V = 86, W = 87, X = 88,
    Y = 89, Z = 90,
    Escape = 256, Return = 257, Enter = Return, Tab = 258, Backspace = 259,
    Insert = 260, Delete = 261, RightArrow = 262, LeftArrow = 263,
    DownArrow = 264, UpArrow = 265, PageUp = 266, PageDown = 267,
    Home = 268, End = 269, CapsLock = 280, ScrollLock = 281, Numlock = 282,
    F1 = 290, F2 = 291, F3 = 292, F4 = 293, F5 = 294, F6 = 295,
    F7 = 296, F8 = 297, F9 = 298, F10 = 299, F11 = 300, F12 = 301,
    Keypad0 = 320, Keypad1 = 321, Keypad2 = 322, Keypad3 = 323,
    Keypad4 = 324, Keypad5 = 325, Keypad6 = 326, Keypad7 = 327,
    Keypad8 = 328, Keypad9 = 329,
    LeftShift = 340, LeftControl = 341, LeftAlt = 342, LeftMeta = 343,
    RightShift = 344, RightControl = 345, RightAlt = 346, RightMeta = 347
}

public static class Input
{
    public static Vector3 mousePosition => new(NativeInput.GetMousePosition().X, NativeInput.GetMousePosition().Y, 0.0f);
    public static Vector2 mouseScrollDelta => NativeInput.GetMouseScrollDelta();
    public static bool GetKey(KeyCode key) => NativeInput.GetKey((int)key);
    public static bool GetKeyDown(KeyCode key) => NativeInput.GetKeyDown((int)key);
    public static bool GetKeyUp(KeyCode key) => NativeInput.GetKeyUp((int)key);
    public static bool GetMouseButton(int button) => NativeInput.GetMouseButton(button);
    public static bool GetMouseButtonDown(int button) => NativeInput.GetMouseButtonDown(button);
    public static bool GetMouseButtonUp(int button) => NativeInput.GetMouseButtonUp(button);
}

internal static class NativeInput
{
    internal static Func<int, bool> GetKey { get; set; } = _ => false;
    internal static Func<int, bool> GetKeyDown { get; set; } = _ => false;
    internal static Func<int, bool> GetKeyUp { get; set; } = _ => false;
    internal static Func<int, bool> GetMouseButton { get; set; } = _ => false;
    internal static Func<int, bool> GetMouseButtonDown { get; set; } = _ => false;
    internal static Func<int, bool> GetMouseButtonUp { get; set; } = _ => false;
    internal static Func<Vector2> GetMousePosition { get; set; } = () => Vector2.zero;
    internal static Func<Vector2> GetMouseScrollDelta { get; set; } = () => Vector2.zero;
}

internal static class NativeTime
{
    internal static Func<float> GetTimeScale { get; set; } = () => 1.0f;
    internal static Action<float> SetTimeScale { get; set; } = _ => { };
}

internal static class NativeSceneManager
{
    internal static Func<Scene> GetActiveScene { get; set; } = static () => new Scene(string.Empty, false);
    internal static Action<string> LoadScene { get; set; } = static _ => throw new InvalidOperationException("Native SceneManager is unavailable.");
}

[EditorBrowsable(EditorBrowsableState.Never)]
public static class LifecycleInvoker
{
    private static readonly Action<Behaviour> Missing = static _ => { };
    private static readonly ConcurrentDictionary<(Type Type, ushort Callback), Action<Behaviour>> Cache = new();

    public static bool Invoke(Behaviour behaviour, ushort callback)
    {
        var action = Cache.GetOrAdd((behaviour.GetType(), callback), static key =>
        {
            var methodName = key.Callback switch
            {
                0 => "Awake", 1 => "Start", 2 => "OnEnable", 3 => "OnDisable",
                4 => "Update", 5 => "FixedUpdate", 6 => "LateUpdate", 7 => "OnDestroy", _ => string.Empty
            };
            if (methodName.Length == 0) return Missing;
            MethodInfo? method = null;
            for (var type = key.Type; type is not null && method is null; type = type.BaseType)
            {
                method = type.GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly)
                    .FirstOrDefault(candidate => candidate.Name == methodName
                        && !candidate.IsStatic
                        && candidate.GetParameters().Length == 0
                        && candidate.ReturnType == typeof(void));
            }
            if (method is null) return Missing;
            try
            {
                // Compile the non-public method call once.  Lifecycle frames use
                // the delegate directly and never pay MethodInfo.Invoke costs.
                var target = Expression.Parameter(typeof(Behaviour), "target");
                var call = Expression.Call(Expression.Convert(target, key.Type), method);
                return Expression.Lambda<Action<Behaviour>>(call, target).Compile();
            }
            catch (ArgumentException)
            {
                return Missing;
            }
        });
        if (ReferenceEquals(action, Missing)) return false;
        action(behaviour);
        return true;
    }
}

public static unsafe class NativeBinding
{
    private static readonly ConcurrentDictionary<ulong, byte> DestroyedHandles = new();
    public delegate bool IsAliveDelegate(ulong handle);
    public delegate void LogDelegate(int severity, string message);
    public static IsAliveDelegate IsAlive { get; set; } = _ => true;
    public static LogDelegate Log { get; set; } = (_, _) => { };

    internal static void Configure(NativeApiTable table)
    {
        if (table.IsAlive != null)
            IsAlive = handle => table.IsAlive(handle) != 0;

        if (table.Log != null)
        {
            Log = (severity, message) =>
            {
                var pointer = Marshal.StringToCoTaskMemUTF8(message);
                try
                {
                    table.Log((byte)severity, (byte*)pointer);
                }
                finally
                {
                    Marshal.FreeCoTaskMem(pointer);
                }
            };
        }

        NativeBindingStore.Configure(table);
    }

    public static void MarkDestroyed(ulong handle)
    {
        if (handle != 0)
            DestroyedHandles[handle] = 0;
    }

    public static void MarkAlive(ulong handle)
    {
        if (handle != 0)
            DestroyedHandles.TryRemove(handle, out _);
    }

    internal static bool IsHandleAlive(ulong handle)
        => handle != 0 && !DestroyedHandles.ContainsKey(handle) && IsAlive(handle);
}

// Script assemblies are loaded after the portable managed runtime. Their
// generated factory and callback dispatch tables register here through module
// initializers. Registrations are keyed by assembly so EngineScripts and
// GameScripts can coexist and a project hot reload replaces only its own entry.
public static class ManagedScriptRegistry
{
    public delegate bool FactoryDelegate(string assetPath, NativeObjectHandle handle, out Behaviour? behaviour);
    public delegate bool DispatchDelegate(Behaviour behaviour, ushort callback, float deltaTime);
    public delegate uint CallbackMaskDelegate(string typeName);
    public delegate IReadOnlyList<ManagedScriptFieldDescriptor> FieldDescriptorDelegate(string typeName);
    public delegate bool FieldGetterDelegate(Behaviour behaviour, ulong field, out object? value);
    public delegate bool FieldSetterDelegate(Behaviour behaviour, ulong field, object? value);
    public delegate string ManifestProviderDelegate();

    public sealed class Registration
    {
        internal Registration(
            string key,
            FactoryDelegate factory,
            DispatchDelegate dispatch,
            CallbackMaskDelegate callbackMask,
            FieldDescriptorDelegate fields,
            FieldGetterDelegate getter,
            FieldSetterDelegate setter,
            ManifestProviderDelegate manifest)
        {
            Key = key;
            Factory = factory;
            Dispatch = dispatch;
            CallbackMask = callbackMask;
            Fields = fields;
            Getter = getter;
            Setter = setter;
            Manifest = manifest;
        }

        internal string Key { get; }
        internal FactoryDelegate Factory { get; }
        internal DispatchDelegate Dispatch { get; }
        internal CallbackMaskDelegate CallbackMask { get; }
        internal FieldDescriptorDelegate Fields { get; }
        internal FieldGetterDelegate Getter { get; }
        internal FieldSetterDelegate Setter { get; }
        internal ManifestProviderDelegate Manifest { get; }

        internal bool TryCreate(string assetPath, NativeObjectHandle handle, out Behaviour? behaviour)
            => Factory(assetPath, handle, out behaviour);

        internal bool Invoke(Behaviour behaviour, ushort callback, float deltaTime)
        {
            var typeName = "global::" + (behaviour.GetType().FullName ?? string.Empty);
            if (callback >= 8)
                return false;
            if ((CallbackMask(typeName) & (1u << callback)) == 0)
                return true;
            return Dispatch(behaviour, callback, deltaTime);
        }

        internal IReadOnlyList<ManagedScriptFieldDescriptor> GetFields(string typeName)
            => Fields(typeName);

        internal bool TryGetField(Behaviour behaviour, ulong field, out object? value)
            => Getter(behaviour, field, out value);

        internal bool TrySetField(Behaviour behaviour, ulong field, object? value)
            => Setter(behaviour, field, value);

        internal string GetManifest() => Manifest();
    }

    private static readonly object Gate = new();
    private static readonly Registration Default = new(
        "<default>",
        static (string _, NativeObjectHandle __, out Behaviour? behaviour) => { behaviour = null; return false; },
        static (Behaviour _, ushort __, float ___) => false,
        static _ => 0u,
        static _ => Array.Empty<ManagedScriptFieldDescriptor>(),
        static (Behaviour _, ulong __, out object? value) => { value = null; return false; },
        static (Behaviour _, ulong __, object? ___) => false,
        static () => "{\"schemaHash\":\"\",\"behaviours\":[]}");
    private static readonly List<Registration> Registrations = new();
    private static Registration _current = Default;

    public static void Register(
        string key,
        FactoryDelegate factory,
        DispatchDelegate dispatch,
        CallbackMaskDelegate callbackMask,
        FieldDescriptorDelegate fields,
        FieldGetterDelegate getter,
        FieldSetterDelegate setter,
        ManifestProviderDelegate manifest)
    {
        ArgumentNullException.ThrowIfNull(factory);
        ArgumentNullException.ThrowIfNull(dispatch);
        ArgumentNullException.ThrowIfNull(callbackMask);
        ArgumentNullException.ThrowIfNull(fields);
        ArgumentNullException.ThrowIfNull(getter);
        ArgumentNullException.ThrowIfNull(setter);
        ArgumentNullException.ThrowIfNull(manifest);
        lock (Gate)
        {
            var registration = new Registration(
                string.IsNullOrWhiteSpace(key) ? "ScriptAssembly" : key,
                factory,
                dispatch,
                callbackMask,
                fields,
                getter,
                setter,
                manifest);
            var index = Registrations.FindIndex(existing =>
                string.Equals(existing.Key, registration.Key, StringComparison.Ordinal));
            if (index >= 0)
                Registrations[index] = registration;
            else
                Registrations.Add(registration);
            _current = registration;
        }
    }

    // Compatibility overload for older generated assemblies. Their assembly
    // name is still a stable key, so they participate in the same merge.
    public static void Register(
        FactoryDelegate factory,
        DispatchDelegate dispatch,
        CallbackMaskDelegate callbackMask,
        FieldDescriptorDelegate fields,
        FieldGetterDelegate getter,
        FieldSetterDelegate setter,
        ManifestProviderDelegate manifest)
    {
        var key = factory.Method.DeclaringType?.Assembly.GetName().Name ?? "ScriptAssembly";
        Register(key, factory, dispatch, callbackMask, fields, getter, setter, manifest);
    }

    internal static void Restore(Registration registration)
    {
        ArgumentNullException.ThrowIfNull(registration);
        lock (Gate)
        {
            var index = Registrations.FindIndex(existing =>
                string.Equals(existing.Key, registration.Key, StringComparison.Ordinal));
            if (index >= 0)
                Registrations[index] = registration;
            else
                Registrations.Add(registration);
            _current = registration;
        }
    }

    internal static void Reset()
    {
        lock (Gate)
        {
            Registrations.Clear();
            _current = Default;
        }
    }

    internal static Registration Current
    {
        get
        {
            lock (Gate)
                return _current;
        }
    }

    internal static bool TryCreate(string assetPath, NativeObjectHandle handle, out Behaviour? behaviour)
    {
        return TryCreate(assetPath, handle, out behaviour, out _);
    }

    internal static bool TryCreate(
        string assetPath,
        NativeObjectHandle handle,
        out Behaviour? behaviour,
        out Registration registration)
    {
        lock (Gate)
        {
            for (var index = Registrations.Count - 1; index >= 0; --index)
            {
                var candidate = Registrations[index];
                if (candidate.TryCreate(assetPath, handle, out behaviour))
                {
                    registration = candidate;
                    return true;
                }
            }
        }
        behaviour = null;
        registration = Default;
        return false;
    }

    internal static bool Invoke(Behaviour behaviour, ushort callback, float deltaTime)
        => Current.Invoke(behaviour, callback, deltaTime);

    internal static IReadOnlyList<ManagedScriptFieldDescriptor> GetFields(string typeName)
        => Current.GetFields(typeName);

    internal static bool TryGetField(Behaviour behaviour, ulong field, out object? value)
        => Current.TryGetField(behaviour, field, out value);

    internal static bool TrySetField(Behaviour behaviour, ulong field, object? value)
        => Current.TrySetField(behaviour, field, value);

    internal static string GetManifest()
    {
        lock (Gate)
        {
            if (Registrations.Count == 0)
                return Default.GetManifest();

            var schemaHash = string.Empty;
            var behaviours = new JsonArray();
            foreach (var registration in Registrations)
            {
                JsonDocument document;
                try
                {
                    document = JsonDocument.Parse(registration.GetManifest());
                }
                catch (JsonException)
                {
                    continue;
                }

                using (document)
                {
                    var root = document.RootElement;
                    if (!root.TryGetProperty("schemaHash", out var schema)
                        || schema.ValueKind != JsonValueKind.String)
                        continue;
                    var candidateHash = schema.GetString() ?? string.Empty;
                    if (schemaHash.Length == 0)
                        schemaHash = candidateHash;
                    if (!string.Equals(candidateHash, schemaHash, StringComparison.Ordinal))
                        continue;
                    if (!root.TryGetProperty("behaviours", out var entries)
                        || entries.ValueKind != JsonValueKind.Array)
                        continue;
                    foreach (var entry in entries.EnumerateArray())
                    {
                        var node = JsonNode.Parse(entry.GetRawText());
                        if (node is not null)
                            behaviours.Add(node);
                    }
                }
            }

            var merged = new JsonObject
            {
                ["schemaHash"] = schemaHash,
                ["behaviours"] = behaviours
            };
            return merged.ToJsonString();
        }
    }
}

using System;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Nullus;

public class Object : NativeObject
{
    protected Object() { }
    internal Object(NativeObjectHandle handle) : base(handle) { }

    public string name
    {
        get { ThrowIfDestroyed(); return NativeBindingStore.GetString(Handle, ScriptApiManifest.MemberId("NLS::Engine::GameObject", "GetName()")); }
        set { ThrowIfDestroyed(); NativeBindingStore.SetString(Handle, ScriptApiManifest.MemberId("NLS::Engine::GameObject", "SetName(std::string&)"), value); }
    }

    public ulong GetInstanceID() => Handle.Value & 0xffffffffUL;

    public static void Destroy(Object? target, float delay = 0.0f)
    {
        if (target is null || target.IsDestroyed)
            return;
        NativeBindingStore.Destroy(target.Handle, delay);
        target.MarkDestroyed();
    }

    public static GameObject Instantiate(GameObject original)
    {
        ArgumentNullException.ThrowIfNull(original);
        original.ThrowIfDestroyed();
        var handle = NativeBindingStore.Instantiate(original.Handle);
        return NativeObject.FromHandle<GameObject>(handle)!;
    }
}

public class Transform : Component
{
    internal Transform(NativeObjectHandle handle) : base(handle) { }
    internal Transform() { }
    public Vector3 LocalPosition
    {
        get { ThrowIfDestroyed(); return NativeBindingStore.GetVector3(Handle, ScriptApiManifest.MemberId("NLS::Engine::Components::TransformComponent", "GetLocalPosition()")); }
        set { ThrowIfDestroyed(); NativeBindingStore.SetVector3(Handle, ScriptApiManifest.MemberId("NLS::Engine::Components::TransformComponent", "SetLocalPosition(NLS::Maths::Vector3)"), value); }
    }
    public Quaternion LocalRotation
    {
        get { ThrowIfDestroyed(); return NativeBindingStore.GetQuaternion(Handle, ScriptApiManifest.MemberId("NLS::Engine::Components::TransformComponent", "GetLocalRotation()")); }
        set { ThrowIfDestroyed(); NativeBindingStore.SetQuaternion(Handle, ScriptApiManifest.MemberId("NLS::Engine::Components::TransformComponent", "SetLocalRotation(NLS::Maths::Quaternion)"), value); }
    }

    public Vector3 localScale
    {
        get { ThrowIfDestroyed(); return NativeBindingStore.GetVector3(Handle, ScriptApiManifest.MemberId("NLS::Engine::Components::TransformComponent", "GetLocalScale()")); }
        set { ThrowIfDestroyed(); NativeBindingStore.SetVector3(Handle, ScriptApiManifest.MemberId("NLS::Engine::Components::TransformComponent", "SetLocalScale(NLS::Maths::Vector3)"), value); }
    }
    public Vector3 lossyScale
    {
        get { ThrowIfDestroyed(); return NativeBindingStore.GetVector3(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::TransformComponent::GetWorldScale()")); }
    }

    public Vector3 position
    {
        get { ThrowIfDestroyed(); return NativeBindingStore.GetVector3(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::TransformComponent::GetWorldPosition()")); }
        set { ThrowIfDestroyed(); NativeBindingStore.SetVector3(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::TransformComponent::SetWorldPosition(NLS::Maths::Vector3)"), value); }
    }
    public Vector3 localPosition { get => LocalPosition; set => LocalPosition = value; }
    public Quaternion localRotation { get => LocalRotation; set => LocalRotation = value; }
    public Vector3 localEulerAngles { get => localRotation.eulerAngles; set => localRotation = Quaternion.Euler(value); }
    public Quaternion rotation
    {
        get { ThrowIfDestroyed(); return NativeBindingStore.GetQuaternion(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::TransformComponent::GetWorldRotation()")); }
        set { ThrowIfDestroyed(); NativeBindingStore.SetQuaternion(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::TransformComponent::SetWorldRotation(NLS::Maths::Quaternion)"), value); }
    }
    public Vector3 eulerAngles { get => rotation.eulerAngles; set => rotation = Quaternion.Euler(value); }
    public Transform? parent
    {
        get { ThrowIfDestroyed(); return NativeObject.FromHandle<Transform>(NativeBindingStore.GetObject(Handle, ScriptApiManifest.MemberId("NLS::Engine::Components::TransformComponent", "GetParent()"))); }
        set { ThrowIfDestroyed(); NativeBindingStore.SetObject(Handle, ScriptApiManifest.MemberId("NLS::Engine::Components::TransformComponent", "SetParent(NLS::Engine::Components::TransformComponent*)"), value?.Handle ?? default); }
    }
    public Transform root
    {
        get
        {
            var current = this;
            while (current.parent is { } next)
                current = next;
            return current;
        }
    }

    public Vector3 forward => rotation * Vector3.forward;
    public Vector3 right => rotation * Vector3.right;
    public Vector3 up => rotation * Vector3.up;
    public void Translate(Vector3 translation) => Translate(translation, Space.Self);
    public void Translate(Vector3 translation, Space relativeTo)
        => position += relativeTo == Space.World ? translation : rotation * translation;
    public void Rotate(Vector3 eulerAngles) => Rotate(eulerAngles, Space.Self);
    public void Rotate(Vector3 eulerAngles, Space relativeTo)
    {
        var delta = Quaternion.Euler(eulerAngles);
        rotation = relativeTo == Space.World ? delta * rotation : rotation * delta;
    }
    public void LookAt(Vector3 worldPosition) { rotation = Quaternion.LookRotation(worldPosition - position, Vector3.up); }
    public void LookAt(Transform target) { ArgumentNullException.ThrowIfNull(target); LookAt(target.position); }
    public Vector3 TransformPoint(Vector3 point) => position + rotation * Vector3.Scale(lossyScale, point);
    public Vector3 InverseTransformPoint(Vector3 point)
    {
        var local = Quaternion.Inverse(rotation) * (point - position);
        var scale = lossyScale;
        return new Vector3(
            MathF.Abs(scale.x) <= float.Epsilon ? 0.0f : local.x / scale.x,
            MathF.Abs(scale.y) <= float.Epsilon ? 0.0f : local.y / scale.y,
            MathF.Abs(scale.z) <= float.Epsilon ? 0.0f : local.z / scale.z);
    }
    public Vector3 TransformDirection(Vector3 direction) => rotation * direction;
    public Vector3 InverseTransformDirection(Vector3 direction) => Quaternion.Inverse(rotation) * direction;
}

public enum Space
{
    World = 0,
    Self = 1
}

public sealed class Camera : Component
{
    internal Camera(NativeObjectHandle handle) : base(handle) { }
    internal Camera() { }
    public float fieldOfView
    {
        get { ThrowIfDestroyed(); return NativeBindingStore.GetFloat(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::CameraComponent::GetFov()")); }
        set { ThrowIfDestroyed(); NativeBindingStore.SetFloat(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::CameraComponent::SetFov(float)"), value); }
    }
    public float orthographicSize
    {
        get { ThrowIfDestroyed(); return NativeBindingStore.GetFloat(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::CameraComponent::GetSize()")); }
        set { ThrowIfDestroyed(); NativeBindingStore.SetFloat(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::CameraComponent::SetSize(float)"), value); }
    }
    public float nearClipPlane
    {
        get { ThrowIfDestroyed(); return NativeBindingStore.GetFloat(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::CameraComponent::GetNear()")); }
        set { ThrowIfDestroyed(); NativeBindingStore.SetFloat(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::CameraComponent::SetNear(float)"), value); }
    }
    public float farClipPlane
    {
        get { ThrowIfDestroyed(); return NativeBindingStore.GetFloat(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::CameraComponent::GetFar()")); }
        set { ThrowIfDestroyed(); NativeBindingStore.SetFloat(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::CameraComponent::SetFar(float)"), value); }
    }
    public Vector3 clearColor
    {
        get { ThrowIfDestroyed(); return NativeBindingStore.GetVector3(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::CameraComponent::GetClearColor()")); }
        set { ThrowIfDestroyed(); NativeBindingStore.SetVector3(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::CameraComponent::SetClearColor(NLS::Maths::Vector3)"), value); }
    }
    public bool orthographic
    {
        get => NativeBindingStore.GetInt32(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::CameraComponent::GetProjectionMode()")) == 0;
        set => NativeBindingStore.SetInt32(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::CameraComponent::SetProjectionMode(NLS::Render::Settings::EProjectionMode)"), value ? 0 : 1);
    }
}

public sealed class Light : Component
{
    internal Light(NativeObjectHandle handle) : base(handle) { }
    internal Light() { }
    public Vector3 color
    {
        get { ThrowIfDestroyed(); return NativeBindingStore.GetVector3(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::LightComponent::GetColor()")); }
        set { ThrowIfDestroyed(); NativeBindingStore.SetVector3(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::LightComponent::SetColor(NLS::Maths::Vector3)"), value); }
    }
    public float intensity { get => NativeBindingStore.GetFloat(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::LightComponent::GetIntensity()")); set => NativeBindingStore.SetFloat(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::LightComponent::SetIntensity(float)"), value); }
    public float range { get => NativeBindingStore.GetFloat(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::LightComponent::GetRange()")); set => NativeBindingStore.SetFloat(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::LightComponent::SetRange(float)"), value); }
    public float spotAngle { get => NativeBindingStore.GetFloat(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::LightComponent::GetOuterCutoff()")); set => NativeBindingStore.SetFloat(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::LightComponent::SetOuterCutoff(float)"), value); }
    public int type { get => NativeBindingStore.GetInt32(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::LightComponent::GetLightType()")); set => NativeBindingStore.SetInt32(Handle, ScriptApiManifest.StableId("NLS::Engine::Components::LightComponent::SetLightType(NLS::Render::Settings::ELightType)"), value); }
}

public sealed class GameObject : Object
{
    internal GameObject(NativeObjectHandle handle) : base(handle) { }
    public GameObject(string name = "New GameObject", string tag = "")
    {
        var handle = NativeBindingStore.CreateGameObject(name, tag);
        if (!handle.IsValid)
            throw new MissingReferenceException("Native GameObject creation failed.");
        BindNativeHandle(handle);
    }
    public Transform Transform
    {
        get
        {
            ThrowIfDestroyed();
            return NativeObject.FromHandle<Transform>(NativeBindingStore.GetObject(Handle, ScriptApiManifest.MemberId("NLS::Engine::GameObject", "GetTransform()")))!;
        }
    }
    public Transform transform => Transform;
    public bool activeSelf => NativeBindingStore.GetBool(Handle, ScriptApiManifest.MemberId("NLS::Engine::GameObject", "GetActive()"));
    public bool activeInHierarchy => NativeBindingStore.GetBool(Handle, ScriptApiManifest.MemberId("NLS::Engine::GameObject", "IsActive()"));
    public string tag { get => NativeBindingStore.GetString(Handle, ScriptApiManifest.MemberId("NLS::Engine::GameObject", "GetTag()")); set => NativeBindingStore.SetString(Handle, ScriptApiManifest.MemberId("NLS::Engine::GameObject", "SetTag(std::string&)"), value); }
    public int layer { get => NativeBindingStore.GetInt32(Handle, ScriptApiManifest.StableId("NLS::Engine::GameObject::GetLayer()")); set => NativeBindingStore.SetInt32(Handle, ScriptApiManifest.StableId("NLS::Engine::GameObject::SetLayer(int)"), value); }
    public void SetActive(bool active) { ThrowIfDestroyed(); NativeBindingStore.SetBool(Handle, ScriptApiManifest.MemberId("NLS::Engine::GameObject", "SetActive(bool)"), active); }
    public T? AddComponent<T>() where T : Component => NativeObject.FromHandle<T>(NativeBindingStore.AddComponent(Handle, ScriptApiManifest.StableId(typeof(T).FullName ?? typeof(T).Name)));
    public T? GetComponent<T>() where T : Component => NativeObject.FromHandle<T>(NativeBindingStore.GetComponent(Handle, ScriptApiManifest.StableId(typeof(T).FullName ?? typeof(T).Name)));
    public static GameObject? Find(string objectName) => NativeObject.FromHandle<GameObject>(NativeBindingStore.Find(objectName));
    public static GameObject? FindWithTag(string objectTag) => NativeObject.FromHandle<GameObject>(NativeBindingStore.FindWithTag(objectTag));
    public GameObject CreatePrimitive(string primitiveType = "Cube")
    {
        ThrowIfDestroyed();
        var handle = NativeBindingStore.CreatePrimitive(Handle, primitiveType);
        if (!handle.IsValid)
            throw new MissingReferenceException("Native primitive creation failed.");
        return NativeObject.FromHandle<GameObject>(handle)!;
    }
}

[DebuggerNonUserCode]
internal static class NativeObjectFactories
{
    private static bool _initialized;

    internal static void Ensure()
    {
        if (_initialized)
            return;
        _initialized = true;
        NativeObject.RegisterFactory<Transform>(handle => new Transform(handle));
        NativeObject.RegisterFactory<GameObject>(handle => new GameObject(handle));
        NativeObject.RegisterFactory<Camera>(handle => new Camera(handle));
        NativeObject.RegisterFactory<Light>(handle => new Light(handle));
    }
}

[DebuggerNonUserCode]
internal static class NativeBindingStore
{
    public static Func<NativeObjectHandle, ulong, NativeObjectHandle> GetObject { get; set; } = (_, _) => default;
    public static Func<NativeObjectHandle, ulong, Vector3> GetVector3 { get; set; } = (_, _) => default;
    public static Action<NativeObjectHandle, ulong, Vector3> SetVector3 { get; set; } = (_, _, _) => { };
    public static Action<NativeObjectHandle, ulong, bool> SetBool { get; set; } = (_, _, _) => { };
    public static Func<NativeObjectHandle, ulong, Quaternion> GetQuaternion { get; set; } = (_, _) => default;
    public static Action<NativeObjectHandle, ulong, Quaternion> SetQuaternion { get; set; } = (_, _, _) => { };
    public static Func<NativeObjectHandle, string, NativeObjectHandle> CreatePrimitive { get; set; } = (_, _) => default;
    public static Func<NativeObjectHandle, ulong, string> GetString { get; set; } = (_, _) => string.Empty;
    public static Action<NativeObjectHandle, ulong, string> SetString { get; set; } = (_, _, _) => { };
    public static Func<NativeObjectHandle, ulong, bool> GetBool { get; set; } = (_, _) => false;
    public static Func<NativeObjectHandle, ulong, NativeObjectHandle> GetComponent { get; set; } = (_, _) => default;
    public static Func<NativeObjectHandle, ulong, int> GetInt32 { get; set; } = (_, _) => 0;
    public static Action<NativeObjectHandle, ulong, int> SetInt32 { get; set; } = (_, _, _) => { };
    public static Func<NativeObjectHandle, ulong, float> GetFloat { get; set; } = (_, _) => 0.0f;
    public static Action<NativeObjectHandle, ulong, float> SetFloat { get; set; } = (_, _, _) => { };
    public static Action<NativeObjectHandle, ulong, NativeObjectHandle> SetObject { get; set; } = (_, _, _) => { };
    public static Func<NativeObjectHandle, NativeObjectHandle> Instantiate { get; set; } = _ => default;
    public static Action<NativeObjectHandle, float> Destroy { get; set; } = (_, _) => { };
    public static Func<string, NativeObjectHandle> Find { get; set; } = _ => default;
    public static Func<string, NativeObjectHandle> FindWithTag { get; set; } = _ => default;
    public static Func<NativeObjectHandle, ulong, NativeObjectHandle> AddComponent { get; set; } = (_, _) => default;
    public static Func<string, string, NativeObjectHandle> CreateGameObject { get; set; } = (_, _) => default;

    public static unsafe void Configure(NativeApiTable table)
    {
        if (table.GetObject != null)
        {
            GetObject = (owner, member) =>
            {
                ulong output = 0;
                var result = table.GetObject(owner.Value, member, &output);
                ThrowOnFailure(result, "Native object call failed.");
                return new NativeObjectHandle(output);
            };
        }
        if (table.GetVector3 != null)
        {
            GetVector3 = (owner, member) =>
            {
                float x = 0, y = 0, z = 0;
                var result = table.GetVector3(owner.Value, member, &x, &y, &z);
                ThrowOnFailure(result, "Native Vector3 getter failed.");
                return new Vector3(x, y, z);
            };
        }
        if (table.SetVector3 != null)
        {
            SetVector3 = (owner, member, value) =>
            {
                var result = table.SetVector3(owner.Value, member, value.X, value.Y, value.Z);
                ThrowOnFailure(result, "Native Vector3 setter failed.");
            };
        }
        if (table.SetBool != null)
        {
            SetBool = (owner, member, value) =>
            {
                var result = table.SetBool(owner.Value, member, value ? (byte)1 : (byte)0);
                ThrowOnFailure(result, "Native bool setter failed.");
            };
        }
        if (table.GetQuaternion != null)
        {
            GetQuaternion = (owner, member) =>
            {
                float x = 0, y = 0, z = 0, w = 1;
                var result = table.GetQuaternion(owner.Value, member, &x, &y, &z, &w);
                ThrowOnFailure(result, "Native Quaternion getter failed.");
                return new Quaternion(x, y, z, w);
            };
        }
        if (table.SetQuaternion != null)
        {
            SetQuaternion = (owner, member, value) =>
            {
                var result = table.SetQuaternion(owner.Value, member, value.X, value.Y, value.Z, value.W);
                ThrowOnFailure(result, "Native Quaternion setter failed.");
            };
        }
        if (table.CreatePrimitive != null)
        {
            CreatePrimitive = (owner, primitiveType) =>
            {
                ulong output = 0;
                var pointer = Marshal.StringToCoTaskMemUTF8(primitiveType ?? "Cube");
                try
                {
                    var result = table.CreatePrimitive(owner.Value, (byte*)pointer, &output);
                    ThrowOnFailure(result, "Native primitive creation failed.");
                    return new NativeObjectHandle(output);
                }
                finally
                {
                    Marshal.FreeCoTaskMem(pointer);
                }
            };
        }
        if (table.GetString != null)
            GetString = (owner, member) => { byte* output = null; uint size = 0; ThrowOnFailure(table.GetString(owner.Value, member, &output, &size), "Native string getter failed."); return output == null ? string.Empty : Marshal.PtrToStringUTF8((IntPtr)output, (int)size) ?? string.Empty; };
        if (table.SetString != null)
            SetString = (owner, member, value) => { var pointer = Marshal.StringToCoTaskMemUTF8(value ?? string.Empty); try { ThrowOnFailure(table.SetString(owner.Value, member, (byte*)pointer), "Native string setter failed."); } finally { Marshal.FreeCoTaskMem(pointer); } };
        if (table.GetBool != null)
            GetBool = (owner, member) => { byte output = 0; ThrowOnFailure(table.GetBool(owner.Value, member, &output), "Native bool getter failed."); return output != 0; };
        if (table.GetComponent != null)
            GetComponent = (owner, type) => { ulong output = 0; ThrowOnFailure(table.GetComponent(owner.Value, type, &output), "Native component lookup failed."); return new NativeObjectHandle(output); };
        if (table.GetInt32 != null)
            GetInt32 = (owner, member) => { int output = 0; ThrowOnFailure(table.GetInt32(owner.Value, member, &output), "Native integer getter failed."); return output; };
        if (table.SetInt32 != null)
            SetInt32 = (owner, member, value) => ThrowOnFailure(table.SetInt32(owner.Value, member, value), "Native integer setter failed.");
        if (table.GetFloat != null)
            GetFloat = (owner, member) => { float output = 0; ThrowOnFailure(table.GetFloat(owner.Value, member, &output), "Native float getter failed."); return output; };
        if (table.SetFloat != null)
            SetFloat = (owner, member, value) => ThrowOnFailure(table.SetFloat(owner.Value, member, value), "Native float setter failed.");
        if (table.Destroy != null)
            Destroy = (owner, delay) => ThrowOnFailure(table.Destroy(owner.Value, delay), "Native destroy failed.");
        if (table.Instantiate != null)
            Instantiate = owner => { ulong output = 0; ThrowOnFailure(table.Instantiate(owner.Value, &output), "Native instantiate failed."); return new NativeObjectHandle(output); };
        if (table.Find != null)
            Find = name => { ulong output = 0; var pointer = Marshal.StringToCoTaskMemUTF8(name ?? string.Empty); try { ThrowOnFailure(table.Find((byte*)pointer, &output), "Native name lookup failed."); return new NativeObjectHandle(output); } finally { Marshal.FreeCoTaskMem(pointer); } };
        if (table.FindWithTag != null)
            FindWithTag = tag => { ulong output = 0; var pointer = Marshal.StringToCoTaskMemUTF8(tag ?? string.Empty); try { ThrowOnFailure(table.FindWithTag((byte*)pointer, &output), "Native tag lookup failed."); return new NativeObjectHandle(output); } finally { Marshal.FreeCoTaskMem(pointer); } };
        if (table.AddComponent != null)
            AddComponent = (owner, type) => { ulong output = 0; ThrowOnFailure(table.AddComponent(owner.Value, type, &output), "Native AddComponent failed."); return new NativeObjectHandle(output); };
        if (table.CreateGameObject != null)
            CreateGameObject = (name, tag) =>
            {
                ulong output = 0;
                var namePointer = Marshal.StringToCoTaskMemUTF8(name ?? "New GameObject");
                var tagPointer = Marshal.StringToCoTaskMemUTF8(tag ?? string.Empty);
                try
                {
                    ThrowOnFailure(table.CreateGameObject((byte*)namePointer, (byte*)tagPointer, &output), "Native GameObject creation failed.");
                    return new NativeObjectHandle(output);
                }
                finally
                {
                    Marshal.FreeCoTaskMem(namePointer);
                    Marshal.FreeCoTaskMem(tagPointer);
                }
            };
        if (table.SetObject != null)
            SetObject = (owner, member, value) => ThrowOnFailure(table.SetObject(owner.Value, member, value.Value), "Native object setter failed.");
        if (table.GetKey != null)
            NativeInput.GetKey = key => { byte output = 0; ThrowOnFailure(table.GetKey(key, &output), "Native key query failed."); return output != 0; };
        if (table.GetKeyDown != null)
            NativeInput.GetKeyDown = key => { byte output = 0; ThrowOnFailure(table.GetKeyDown(key, &output), "Native key edge query failed."); return output != 0; };
        if (table.GetKeyUp != null)
            NativeInput.GetKeyUp = key => { byte output = 0; ThrowOnFailure(table.GetKeyUp(key, &output), "Native key edge query failed."); return output != 0; };
        if (table.GetMouseButton != null)
            NativeInput.GetMouseButton = button => { byte output = 0; ThrowOnFailure(table.GetMouseButton(button, &output), "Native mouse query failed."); return output != 0; };
        if (table.GetMouseButtonDown != null)
            NativeInput.GetMouseButtonDown = button => { byte output = 0; ThrowOnFailure(table.GetMouseButtonDown(button, &output), "Native mouse edge query failed."); return output != 0; };
        if (table.GetMouseButtonUp != null)
            NativeInput.GetMouseButtonUp = button => { byte output = 0; ThrowOnFailure(table.GetMouseButtonUp(button, &output), "Native mouse edge query failed."); return output != 0; };
        if (table.GetMousePosition != null)
            NativeInput.GetMousePosition = () => { float x = 0, y = 0; ThrowOnFailure(table.GetMousePosition(&x, &y), "Native mouse position query failed."); return new Vector2(x, y); };
        if (table.GetMouseScrollDelta != null)
            NativeInput.GetMouseScrollDelta = () => { float x = 0, y = 0; ThrowOnFailure(table.GetMouseScrollDelta(&x, &y), "Native mouse scroll query failed."); return new Vector2(x, y); };
        if (table.GetTimeScale != null)
            NativeTime.GetTimeScale = () => { float value = 1.0f; ThrowOnFailure(table.GetTimeScale(&value), "Native time scale query failed."); return value; };
        if (table.SetTimeScale != null)
            NativeTime.SetTimeScale = value => ThrowOnFailure(table.SetTimeScale(value), "Native time scale setter failed.");
        if (table.GetActiveScene != null)
            NativeSceneManager.GetActiveScene = () =>
            {
                byte* output = null;
                uint size = 0;
                byte loaded = 0;
                ThrowOnFailure(table.GetActiveScene(&output, &size, &loaded), "Native active scene query failed.");
                var path = output == null ? string.Empty : Marshal.PtrToStringUTF8((IntPtr)output, (int)size) ?? string.Empty;
                return new Scene(path, loaded != 0);
            };
        if (table.LoadScene != null)
            NativeSceneManager.LoadScene = scenePath =>
            {
                var pointer = Marshal.StringToCoTaskMemUTF8(scenePath ?? string.Empty);
                try { ThrowOnFailure(table.LoadScene((byte*)pointer), "Native scene load failed."); }
                finally { Marshal.FreeCoTaskMem(pointer); }
            };
    }

    private static void ThrowOnFailure(ScriptAbiResult result, string fallback)
    {
        if (result.Code == 0)
            return;
        var message = result.Message == IntPtr.Zero ? fallback : Marshal.PtrToStringUTF8(result.Message) ?? fallback;
        throw new MissingReferenceException(message);
    }
}

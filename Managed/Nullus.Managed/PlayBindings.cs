using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Nullus;

internal static unsafe class PlayBindingStore
{
    private static delegate* unmanaged<ulong, ulong, ScriptAbiValue*, uint, ScriptAbiValue*, ScriptAbiResult> _invoke;
    internal static void Configure(NativeApiTable table) => _invoke=table.InvokeMember;
    [ModuleInitializer] internal static void Register()
    {
        NativeObject.RegisterFactory<Rigidbody>(h=>new Rigidbody(h));
        NativeObject.RegisterFactory<Collider>(h=>new Collider(h));
        NativeObject.RegisterFactory<AudioSource>(h=>new AudioSource(h));
        NativeObject.RegisterFactory<AudioClip>(h=>new AudioClip(h));
    }
    internal static ScriptAbiValue Call(NativeObjectHandle owner,string member,params float[] arguments)
    {
        PlaySessionGuard.Check();
        if(_invoke==null)throw new InvalidOperationException("This host does not provide Play bindings.");
        var input=stackalloc ScriptAbiValue[arguments.Length];
        for(int i=0;i<arguments.Length;++i)input[i]=new ScriptAbiValue{Kind=11,FloatingValue=arguments[i]};
        ScriptAbiValue output=default;
        var status=_invoke(owner.Value,ScriptApiManifest.StableId("Nullus.Play::"+member),input,(uint)arguments.Length,&output);
        if(status.Code!=0)throw new InvalidOperationException(Marshal.PtrToStringUTF8(status.Message)??"Play binding failed.");
        return output;
    }
    internal static float Number(NativeObjectHandle h,string member)=> (float)Call(h,member).FloatingValue;
    internal static Vector3 Vector(NativeObjectHandle h,string member)
    {
        var value=Call(h,member);if(value.ByteSize!=12)throw new InvalidOperationException("Invalid native vector.");
        var data=(float*)value.Bytes;return new Vector3(data[0],data[1],data[2]);
    }
    internal static void SetObject(NativeObjectHandle h,string member,NativeObject? target)
    {
        PlaySessionGuard.Check();
        if(_invoke==null)throw new InvalidOperationException("This host does not provide Play bindings.");
        ScriptAbiValue input=new(){ObjectValue=target?.Handle.Value??0},output=default;
        var status=_invoke(h.Value,ScriptApiManifest.StableId("Nullus.Play::"+member),&input,1,&output);
        if(status.Code!=0)throw new InvalidOperationException(Marshal.PtrToStringUTF8(status.Message));
    }
    internal static Collider? Other()=>NativeObject.FromHandle<Collider>(new NativeObjectHandle(Call(default,"Event.other").ObjectValue));
    internal static bool EventBoolean()=>Number(default,"Event.boolean")!=0;
}

public sealed class Rigidbody : Component
{
    public const string NativeTypeName="NLS::Engine::Components::RigidbodyComponent";
    internal Rigidbody(NativeObjectHandle handle):base(handle){}
    public float mass {get=>PlayBindingStore.Number(Handle,"Rigidbody.mass.get");set=>PlayBindingStore.Call(Handle,"Rigidbody.mass.set",value);}
    public bool isKinematic {get=>PlayBindingStore.Number(Handle,"Rigidbody.isKinematic.get")!=0;set=>PlayBindingStore.Call(Handle,"Rigidbody.isKinematic.set",value?1:0);}
    public bool useGravity {get=>PlayBindingStore.Number(Handle,"Rigidbody.useGravity.get")!=0;set=>PlayBindingStore.Call(Handle,"Rigidbody.useGravity.set",value?1:0);}
    public Vector3 velocity {get=>PlayBindingStore.Vector(Handle,"Rigidbody.velocity.get");set=>PlayBindingStore.Call(Handle,"Rigidbody.velocity.set",value.x,value.y,value.z);}
    public void AddForce(Vector3 force)=>PlayBindingStore.Call(Handle,"Rigidbody.AddForce",force.x,force.y,force.z);
}
public sealed class Collider : Component
{
    public const string NativeTypeName="NLS::Engine::Components::PhysicsColliderComponent";
    internal Collider(NativeObjectHandle handle):base(handle){}
    public bool isTrigger {get=>PlayBindingStore.Number(Handle,"Collider.isTrigger.get")!=0;set=>PlayBindingStore.Call(Handle,"Collider.isTrigger.set",value?1:0);}
    public float radius {get=>PlayBindingStore.Number(Handle,"Collider.radius.get");set=>PlayBindingStore.Call(Handle,"Collider.radius.set",value);}
    public Vector3 size {get=>PlayBindingStore.Vector(Handle,"Collider.size.get");set=>PlayBindingStore.Call(Handle,"Collider.size.set",value.x,value.y,value.z);}
}
public readonly struct Collision(Collider? other)
{
    public Collider? collider {get;}=other;
    public GameObject? gameObject=>collider?.gameObject;
    public Rigidbody? rigidbody=>collider?.GetComponent<Rigidbody>();
}
public sealed class AudioClip : Object {public const string NativeTypeName="NLS::Engine::Assets::AudioClip";internal AudioClip(NativeObjectHandle h):base(h){}}
public sealed class AudioSource : Component
{
    public const string NativeTypeName="NLS::Engine::Components::AudioSourceComponent";
    internal AudioSource(NativeObjectHandle h):base(h){}
    public AudioClip? clip {get=>NativeObject.FromHandle<AudioClip>(new NativeObjectHandle(PlayBindingStore.Call(Handle,"AudioSource.clip.get").ObjectValue));set=>PlayBindingStore.SetObject(Handle,"AudioSource.clip.set",value);}
    public bool playOnAwake {get=>PlayBindingStore.Number(Handle,"AudioSource.playOnAwake.get")!=0;set=>PlayBindingStore.Call(Handle,"AudioSource.playOnAwake.set",value?1:0);}
    public bool loop {get=>PlayBindingStore.Number(Handle,"AudioSource.loop.get")!=0;set=>PlayBindingStore.Call(Handle,"AudioSource.loop.set",value?1:0);}
    public float volume {get=>PlayBindingStore.Number(Handle,"AudioSource.volume.get");set=>PlayBindingStore.Call(Handle,"AudioSource.volume.set",value);}
    public bool isPlaying=>PlayBindingStore.Number(Handle,"AudioSource.isPlaying.get")!=0;
    public void Play()=>PlayBindingStore.Call(Handle,"AudioSource.Play");
    public void Pause()=>PlayBindingStore.Call(Handle,"AudioSource.Pause");
    public void Stop()=>PlayBindingStore.Call(Handle,"AudioSource.Stop");
}
public static class Screen
{
    public static int width=>(int)PlayBindingStore.Number(default,"Screen.width");
    public static int height=>(int)PlayBindingStore.Number(default,"Screen.height");
}
public enum CameraClearFlags {Skybox,SolidColor,Depth,Nothing}
public sealed partial class Camera
{
    public int targetDisplay {get=>(int)PlayBindingStore.Number(Handle,"Camera.targetDisplay.get");set=>PlayBindingStore.Call(Handle,"Camera.targetDisplay.set",value);}
    public float depth {get=>PlayBindingStore.Number(Handle,"Camera.depth.get");set=>PlayBindingStore.Call(Handle,"Camera.depth.set",value);}
    public CameraClearFlags clearFlags {get=>(CameraClearFlags)PlayBindingStore.Number(Handle,"Camera.clearFlags.get");set=>PlayBindingStore.Call(Handle,"Camera.clearFlags.set",(int)value);}
    public unsafe Rect rect
    {
        get {var value=PlayBindingStore.Call(Handle,"Camera.rect.get");var data=(float*)value.Bytes;return new Rect(data[0],data[1],data[2],data[3]);}
        set=>PlayBindingStore.Call(Handle,"Camera.rect.set",value.x,value.y,value.width,value.height);
    }
}

using System.Threading;

namespace Nullus;

// ExecutionContext carries this ticket into Task/async continuations. A new
// host epoch invalidates old continuations even when they later reach the host
// thread. Native scene access also always belongs to that thread.
internal static class PlaySessionGuard
{
    private static readonly AsyncLocal<long?> Ticket = new();
    private static long _epoch;
    private static int _thread;
    private static bool _active;
    internal static long Epoch => Volatile.Read(ref _epoch);
    internal static void Configure()
    {
        Interlocked.Increment(ref _epoch);
        _thread = Environment.CurrentManagedThreadId;
        Volatile.Write(ref _active, true);
    }
    internal static void Invalidate() => Volatile.Write(ref _active, false);
    internal static bool Owns(long epoch) => Volatile.Read(ref _active) && epoch == Epoch;
    internal static void Check()
    {
        if (!Volatile.Read(ref _active) || (Ticket.Value is { } epoch && epoch != Epoch))
            throw new MissingReferenceException("The originating Play session has ended.");
        if (Environment.CurrentManagedThreadId != _thread)
            throw new InvalidOperationException("Native game APIs must be called on the game host thread.");
    }
    internal static Scope Enter() => new(Epoch);
    internal readonly struct Scope : IDisposable
    {
        private readonly long? _previous;
        internal Scope(long epoch) { _previous=Ticket.Value; Ticket.Value=epoch; }
        public void Dispose() => Ticket.Value=_previous;
    }

    internal static void WrapBindings()
    {
        var getObject=NativeBindingStore.GetObject; NativeBindingStore.GetObject=(a,b)=>{Check();return getObject(a,b);};
        var getVector=NativeBindingStore.GetVector3; NativeBindingStore.GetVector3=(a,b)=>{Check();return getVector(a,b);};
        var setVector=NativeBindingStore.SetVector3; NativeBindingStore.SetVector3=(a,b,c)=>{Check();setVector(a,b,c);};
        var setBool=NativeBindingStore.SetBool; NativeBindingStore.SetBool=(a,b,c)=>{Check();setBool(a,b,c);};
        var getRotation=NativeBindingStore.GetQuaternion; NativeBindingStore.GetQuaternion=(a,b)=>{Check();return getRotation(a,b);};
        var setRotation=NativeBindingStore.SetQuaternion; NativeBindingStore.SetQuaternion=(a,b,c)=>{Check();setRotation(a,b,c);};
        var primitive=NativeBindingStore.CreatePrimitive; NativeBindingStore.CreatePrimitive=(a,b)=>{Check();return primitive(a,b);};
        var getString=NativeBindingStore.GetString; NativeBindingStore.GetString=(a,b)=>{Check();return getString(a,b);};
        var setString=NativeBindingStore.SetString; NativeBindingStore.SetString=(a,b,c)=>{Check();setString(a,b,c);};
        var getBool=NativeBindingStore.GetBool; NativeBindingStore.GetBool=(a,b)=>{Check();return getBool(a,b);};
        var getComponent=NativeBindingStore.GetComponent; NativeBindingStore.GetComponent=(a,b)=>{Check();return getComponent(a,b);};
        var getInt=NativeBindingStore.GetInt32; NativeBindingStore.GetInt32=(a,b)=>{Check();return getInt(a,b);};
        var setInt=NativeBindingStore.SetInt32; NativeBindingStore.SetInt32=(a,b,c)=>{Check();setInt(a,b,c);};
        var getFloat=NativeBindingStore.GetFloat; NativeBindingStore.GetFloat=(a,b)=>{Check();return getFloat(a,b);};
        var setFloat=NativeBindingStore.SetFloat; NativeBindingStore.SetFloat=(a,b,c)=>{Check();setFloat(a,b,c);};
        var setObject=NativeBindingStore.SetObject; NativeBindingStore.SetObject=(a,b,c)=>{Check();setObject(a,b,c);};
        var instantiate=NativeBindingStore.Instantiate; NativeBindingStore.Instantiate=a=>{Check();return instantiate(a);};
        var destroy=NativeBindingStore.Destroy; NativeBindingStore.Destroy=(a,b)=>{Check();destroy(a,b);};
        var find=NativeBindingStore.Find; NativeBindingStore.Find=a=>{Check();return find(a);};
        var tagged=NativeBindingStore.FindWithTag; NativeBindingStore.FindWithTag=a=>{Check();return tagged(a);};
        var add=NativeBindingStore.AddComponent; NativeBindingStore.AddComponent=(a,b)=>{Check();return add(a,b);};
        var create=NativeBindingStore.CreateGameObject; NativeBindingStore.CreateGameObject=(a,b)=>{Check();return create(a,b);};
        var key=NativeInput.GetKey; NativeInput.GetKey=a=>{Check();return key(a);};
        var keyDown=NativeInput.GetKeyDown; NativeInput.GetKeyDown=a=>{Check();return keyDown(a);};
        var keyUp=NativeInput.GetKeyUp; NativeInput.GetKeyUp=a=>{Check();return keyUp(a);};
        var mouse=NativeInput.GetMouseButton; NativeInput.GetMouseButton=a=>{Check();return mouse(a);};
        var mouseDown=NativeInput.GetMouseButtonDown; NativeInput.GetMouseButtonDown=a=>{Check();return mouseDown(a);};
        var mouseUp=NativeInput.GetMouseButtonUp; NativeInput.GetMouseButtonUp=a=>{Check();return mouseUp(a);};
        var position=NativeInput.GetMousePosition; NativeInput.GetMousePosition=()=>{Check();return position();};
        var scroll=NativeInput.GetMouseScrollDelta; NativeInput.GetMouseScrollDelta=()=>{Check();return scroll();};
        var time=NativeTime.GetTimeScale; NativeTime.GetTimeScale=()=>{Check();return time();};
        var scale=NativeTime.SetTimeScale; NativeTime.SetTimeScale=a=>{Check();scale(a);};
        var scene=NativeSceneManager.GetActiveScene; NativeSceneManager.GetActiveScene=()=>{Check();return scene();};
        var load=NativeSceneManager.LoadScene; NativeSceneManager.LoadScene=a=>{Check();load(a);};
    }
}

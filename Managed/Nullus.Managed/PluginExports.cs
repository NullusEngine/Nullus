using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace Nullus.Plugins;

public interface IPlugin : IDisposable
{
    void Startup(PluginContext context);
    void RequestStop();
}

// Host callbacks are all owned by the native ModuleScope. This object is a
// cooperating lifetime tracker, not a second extension authority.
public sealed unsafe class PluginContext
{
    internal HostApi Host;
    internal volatile bool Active = true;
    internal readonly Dictionary<ulong, Func<uint, double, ulong, ulong>> Callbacks = new();
    private readonly List<Action> _cleanup = new();
    private readonly List<Task> _tasks = new();
    private readonly List<Thread> _threads = new();
    private readonly CancellationTokenSource _cancellation = new();
    private readonly int _lifecycleThread = Environment.CurrentManagedThreadId;
    private void RequireLifecycleThread()
    { if (Environment.CurrentManagedThreadId != _lifecycleThread) throw new InvalidOperationException("Plugin registration and tracking require the lifecycle thread."); }
    public CancellationToken Stopping => _cancellation.Token;
    public void RegisterExtension(string id, string kind, Func<uint, double, ulong, ulong> callback, int priority = 0)
    {
        RequireLifecycleThread();
        if (!Active || Host.Register == null) throw new InvalidOperationException("Plugin session is stopping.");
        var key = (ulong)Callbacks.Count + 1; Callbacks.Add(key, callback);
        var idBytes = Encoding.UTF8.GetBytes(id); var kindBytes = Encoding.UTF8.GetBytes(kind);
        fixed (byte* a = idBytes) fixed (byte* b = kindBytes)
            if (Host.Register(Host.Context, a, (uint)idBytes.Length, b, (uint)kindBytes.Length, priority, key) != 0)
            { Callbacks.Remove(key); throw new InvalidOperationException("Host rejected extension registration."); }
    }
    public void Log(string text)
    {
        if (!Active) throw new ObjectDisposedException(nameof(PluginContext));
        var bytes = Encoding.UTF8.GetBytes(text);
        fixed (byte* data = bytes) Host.Log(Host.Context, data, (uint)bytes.Length);
    }
    public void TrackCleanup(Action unsubscribeOrDispose) { RequireLifecycleThread(); if (!Active) throw new ObjectDisposedException(nameof(PluginContext)); _cleanup.Add(unsubscribeOrDispose); }
    public void TrackObject(IDisposable value) => TrackCleanup(value.Dispose);
    public void TrackHandle(GCHandle handle) => TrackCleanup(() => { if (handle.IsAllocated) handle.Free(); });
    public void TrackTask(Task task) { RequireLifecycleThread(); if (!Active) throw new ObjectDisposedException(nameof(PluginContext)); _tasks.Add(task); }
    public void TrackThread(Thread thread) { RequireLifecycleThread(); if (!Active) throw new ObjectDisposedException(nameof(PluginContext)); _threads.Add(thread); }
    internal void Stop() { Active = false; _cancellation.Cancel(); }
    internal string Blockers => string.Join(", ", _tasks.Where(t => !t.IsCompleted).Select(t => $"task {t.Id}")
        .Concat(_threads.Where(t => t.IsAlive).Select(t => $"thread {t.Name ?? t.ManagedThreadId.ToString()}")));
    internal void Clean()
    {
        for (var i = _cleanup.Count - 1; i >= 0; --i) { _cleanup[i](); _cleanup.RemoveAt(i); }
        Callbacks.Clear(); _tasks.Clear(); _threads.Clear(); Host = default; _cancellation.Dispose();
    }
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct HostApi
{
    internal uint Size, Version;
    internal void* Context;
    internal delegate* unmanaged[Cdecl]<void*, byte*, uint, byte*, uint, int, ulong, int> Register;
    internal delegate* unmanaged[Cdecl]<void*, byte*, uint, void> Log;
}

public static unsafe class PluginExports
{
    [StructLayout(LayoutKind.Sequential)]
    private struct Api
    {
        internal uint Size, Version;
        internal delegate* unmanaged[Cdecl]<byte*, byte*, HostApi*, ulong*, int> Load;
        internal delegate* unmanaged[Cdecl]<ulong, ulong, uint, double, ulong*, int> Invoke;
        internal delegate* unmanaged[Cdecl]<ulong, void> Stop;
        internal delegate* unmanaged[Cdecl]<ulong, int> Poll;
        internal delegate* unmanaged[Cdecl]<ulong, void> Release;
        internal delegate* unmanaged[Cdecl]<ulong, byte*, uint, uint> Diagnostic;
    }
    private sealed class Instance
    {
        internal ScriptAssemblyLoadContext? LoadContext;
        internal IPlugin? Plugin;
        internal PluginContext? Context;
        internal WeakReference? Unloading;
        internal bool Stopping, Failed;
        internal string Error = "";
    }
    private static readonly Dictionary<ulong, Instance> Instances = new();
    private static ulong _next;
    private static readonly Api* Table = CreateTable();
    private static Api* CreateTable()
    {
        var table = (Api*)NativeMemory.AllocZeroed((nuint)sizeof(Api));
        *table = new Api { Size = (uint)sizeof(Api), Version = 1, Load = &Load, Invoke = &Invoke, Stop = &Stop, Poll = &Poll, Release = &Release, Diagnostic = &Diagnostic };
        return table;
    }
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    public static void* GetApiTable() => Table;
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int Load(byte* path, byte* entry, HostApi* host, ulong* output)
    {
        if (output == null || path == null || entry == null || host == null || host->Size < sizeof(HostApi) || host->Version != 1) return -1;
        var instance = new Instance(); var token = ++_next; Instances.Add(token, instance); *output = token;
        try
        {
            instance.Context = new PluginContext { Host = *host };
            instance.LoadContext = new ScriptAssemblyLoadContext("Nullus.Plugin." + token);
            var assembly = instance.LoadContext.LoadPlugin(Marshal.PtrToStringUTF8((nint)path)!);
            var type = assembly.GetType(Marshal.PtrToStringUTF8((nint)entry)!, throwOnError: true)!;
            instance.Plugin = Activator.CreateInstance(type) as IPlugin ?? throw new InvalidOperationException("Entry type must implement Nullus.Plugins.IPlugin.");
            instance.Plugin.Startup(instance.Context); return 0;
        }
        catch (Exception error) { instance.Error = error.ToString(); return -1; }
    }
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int Invoke(ulong token, ulong callback, uint kind, double delta, ulong* value)
    {
        if (!Instances.TryGetValue(token, out var instance) || instance.Stopping || value == null) return -1;
        try { *value = instance.Context!.Callbacks[callback](kind, delta, *value); return 0; }
        catch (Exception error) { instance.Error = error.ToString(); return -1; }
    }
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void Stop(ulong token)
    {
        if (!Instances.TryGetValue(token, out var instance) || instance.Stopping) return;
        instance.Stopping = true;
        try { instance.Context?.Stop(); instance.Plugin?.RequestStop(); }
        catch (Exception error) { instance.Error = "Stop failed; restart required: " + error; instance.Failed = true; }
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void BeginUnload(Instance instance)
    {
        instance.Plugin?.Dispose(); instance.Context?.Clean();
        instance.Plugin = null; instance.Context = null;
        var context = instance.LoadContext; instance.LoadContext = null;
        if (context is not null) { instance.Unloading = new WeakReference(context); context.Unload(); }
    }
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int Poll(ulong token)
    {
        if (!Instances.TryGetValue(token, out var instance)) return 1;
        if (!instance.Stopping) return 0;
        if (instance.Failed) return -1;
        try
        {
            var blockers = instance.Context?.Blockers ?? "";
            if (blockers.Length != 0) { instance.Error = "Cooperative unload waiting for " + blockers; return 0; }
            if (instance.LoadContext is not null) BeginUnload(instance);
            GC.Collect();
            if (instance.Unloading?.IsAlive == true) { instance.Error = "Collectible AssemblyLoadContext remains alive; check static events, delegates, GCHandles and worker stacks."; return 0; }
            return 1;
        }
        catch (Exception error) { instance.Error = "Cleanup failed; restart required: " + error; instance.Failed = true; return -1; }
    }
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void Release(ulong token) { Instances.Remove(token); }
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static uint Diagnostic(ulong token, byte* output, uint capacity)
    {
        var bytes = Encoding.UTF8.GetBytes(Instances.TryGetValue(token, out var instance) ? instance.Error : "Unknown plugin handle");
        if (output != null) bytes.AsSpan(0, (int)Math.Min(capacity, (uint)bytes.Length)).CopyTo(new Span<byte>(output, (int)capacity));
        return (uint)bytes.Length;
    }
}

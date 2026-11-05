using Nullus.Plugins;
using System.Runtime.InteropServices;
namespace Nullus.SamplePlugin;
public sealed class RuntimePlugin : IPlugin
{
    private ulong _ticks;
    public void Startup(PluginContext context)
    {
        context.RegisterExtension("org.nullus.sample.csharp.counter", "runtime.play", (kind, delta, value) =>
        {
            if (kind == 1) context.Log("C# runtime session begin");
            if (kind == 2 && ++_ticks == 3) context.Log("C# received third runtime session tick");
            if (kind == 3) context.Log("C# runtime session end");
            return _ticks;
        });
        context.Log("C# plugin registered with the host ExtensionRegistry");
    }
    public void RequestStop() { }
    public void Dispose() { }
}
// Acceptance fixture for cooperative unload failure. An untracked strong
// handle deliberately pins this collectible context until process exit.
#if NULLUS_PLUGIN_ACCEPTANCE_FIXTURES
public sealed class PinnedPlugin : IPlugin
{
    public void Startup(PluginContext context)
    {
        GCHandle.Alloc(this);
        context.RegisterExtension("org.nullus.sample.csharp.pinned", "runtime.play", (kind, delta, value) => value);
    }
    public void RequestStop() { }
    public void Dispose() { }
}
#endif

using System.Reflection;
using System.Runtime.Loader;

namespace Nullus;

// Project assemblies are isolated from the portable runtime and can be
// collected after a successful replacement. The load context intentionally
// shares framework/Nullus.Managed dependencies with the default context.
internal sealed class ScriptAssemblyLoadContext : AssemblyLoadContext
{
    private AssemblyDependencyResolver? _resolver;

    public ScriptAssemblyLoadContext(string name)
        : base(name, isCollectible: true)
    {
    }

    protected override Assembly? Load(AssemblyName assemblyName)
    {
        if (string.Equals(assemblyName.Name, typeof(Behaviour).Assembly.GetName().Name, StringComparison.Ordinal))
            return typeof(Behaviour).Assembly;
        var path = _resolver?.ResolveAssemblyToPath(assemblyName);
        return path is null ? null : LoadFromAssemblyPath(path);
    }

    public Assembly LoadProject(string assemblyPath)
    {
        var fullPath = Path.GetFullPath(assemblyPath);
        _resolver = new AssemblyDependencyResolver(fullPath);
        return LoadFromAssemblyPath(fullPath);
    }
}

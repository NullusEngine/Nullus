// Design-time fallback for Visual Studio's compiler. The runtime build still
// regenerates this manifest with Nullus.ScriptGenerator using the repository
// SDK; this copy keeps IntelliSense/debug project builds independent of the
// net8 Roslyn analyzer load context used by devenv.exe.
#nullable enable
namespace Nullus;

public static partial class ScriptApiManifest
{
    public const string SchemaHash = "0c5ee312d3213f85dd2917ece42291acc7d788b1e497cf149f6f2d470fc0e855";

    public static ulong StableId(string value)
    {
        const ulong offset = 14695981039346656037UL;
        const ulong prime = 1099511628211UL;
        var hash = offset;
        foreach (var b in System.Text.Encoding.UTF8.GetBytes(value))
        {
            hash ^= b;
            hash *= prime;
        }
        return hash == 0 ? 1UL : hash;
    }

    public static ulong MemberId(string owner, string signature) => StableId(owner + "::" + signature);
}

using System.Runtime.InteropServices;

namespace Nullus.Editor;

/// <summary>
/// Stable entry point the native editor resolves once per domain.
///
/// It lives in the editor host assembly (default load context, never unloaded)
/// and is responsible for loading the project editor-scripts assembly into a
/// <em>collectible</em> load context.  That is what makes a script edit, add, or
/// delete observable without restarting the editor: the next call unloads the
/// previous context and loads the freshly built assembly.
/// </summary>
public static class EditorScriptingHost
{
    /// <summary>
    /// Installs the host table and loads the project editor scripts.
    /// Returns the managed API table pointer, or 0 when the host table is
    /// missing or incompatible. The method never throws across the native
    /// boundary because the native host cannot observe a managed exception.
    /// </summary>
    [UnmanagedCallersOnly]
    public static nint Install(nint hostApi, nint editorScriptsAssemblyPath)
    {
        try
        {
            var path = editorScriptsAssemblyPath == 0
                ? null
                : Marshal.PtrToStringUTF8(editorScriptsAssemblyPath);
            return EditorScriptingRuntime.InstallFromPath(hostApi, path);
        }
        catch
        {
            return 0;
        }
    }

    /// <summary>Drops the loaded editor scripts, their registrations, and any live instances.</summary>
    [UnmanagedCallersOnly]
    public static void Uninstall() => EditorScriptingRuntime.Uninstall();
}

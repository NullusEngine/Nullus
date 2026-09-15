using Nullus.Editor;

namespace Nullus.EditorScripts;

/// <summary>
/// Entry point the editor host invokes after loading this assembly into its
/// collectible editor-scripts load context.  It is a plain managed method on
/// purpose: the host calls it in-process, which is what lets a rebuilt assembly
/// replace this one without restarting the editor.
///
/// The host passes its table of drawing/context callbacks and receives the
/// table of lifecycle callbacks it should use for scripted editors.
/// </summary>
public static class EditorScriptsExports
{
    public static nint GetApiTable(nint hostApi)
    {
        try
        {
            return EditorScriptingRuntime.Install(hostApi, typeof(EditorScriptsExports).Assembly);
        }
        catch
        {
            return 0;
        }
    }
}

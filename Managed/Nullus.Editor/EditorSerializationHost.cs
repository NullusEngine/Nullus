using System.Runtime.InteropServices;

namespace Nullus.Editor;

/// <summary>
/// Holds the native SerializedObject/SerializedProperty ABI table for this
/// editor domain. The native editor host calls <see cref="Install"/> through
/// hostfxr once its CoreCLR runtime is up; every managed SerializedObject then
/// resolves its native handle against that shared table.
/// </summary>
public static class EditorSerializationHost
{
    private static EditorNativeApiTable? s_api;

    /// <summary>True once the editor host has published its ABI table.</summary>
    public static bool IsInstalled => s_api is not null;

    /// <summary>
    /// The installed ABI table. Throws when the host never installed one, so a
    /// managed caller fails loudly instead of observing a silently dead
    /// SerializedObject.
    /// </summary>
    public static EditorNativeApiTable Api => s_api ?? throw new InvalidOperationException(
        "The native editor serialization ABI is not installed. The editor host must call " +
        "EditorSerializationHost.Install before a SerializedObject can be created.");

    /// <summary>
    /// Installs the ABI table. Returns 0 on success, 1 for a null table and 2
    /// when the table header is incompatible. The method never throws across
    /// the native boundary because the host cannot observe managed exceptions.
    /// </summary>
    [UnmanagedCallersOnly]
    public static int Install(nint nativeTable)
    {
        if (nativeTable == 0)
        {
            s_api = null;
            return 1;
        }

        try
        {
            s_api = EditorNativeApiTable.FromNativeTable(nativeTable);
            return 0;
        }
        catch
        {
            s_api = null;
            return 2;
        }
    }

    /// <summary>Drops the table so a domain reload or editor shutdown starts clean.</summary>
    [UnmanagedCallersOnly]
    public static void Uninstall() => s_api = null;
}

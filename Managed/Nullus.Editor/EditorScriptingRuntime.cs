using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;

namespace Nullus.Editor;

/// <summary>
/// Managed half of the scripted editor ABI.  The project editor-scripts
/// assembly calls <see cref="Install"/> once per domain; native then drives the
/// editor and window instances registered here.
///
/// Only declarative metadata is scanned: a script declares which component it
/// draws with <see cref="CustomEditorAttribute"/> and optionally opts into
/// multi-object editing.  Values always travel through
/// <see cref="SerializedObject"/>, never through a component pointer.
/// </summary>
public static unsafe class EditorScriptingRuntime
{
    private const uint SupportedAbiVersion = 1u;
    private const int DefaultEditorPriority = 100;
    private const int TextFieldCapacity = 4096;

    private static readonly Dictionary<ulong, ComponentEditor> s_componentEditors = new();
    private static readonly Dictionary<ulong, EditorWindow> s_windows = new();
    private static readonly List<(string MenuPath, Type WindowType)> s_windowMenuItems = new();
    private static readonly List<(string MenuPath, MethodInfo Method)> s_commandMenuItems = new();

    private static Assembly? s_editorAssembly;
    private static EditorScriptsLoadContext? s_loadContext;
    private static nint s_editorAssemblyPath;
    private static EditorHostApiTable* s_host;
    private static ManagedEditorScriptApiTableMirror* s_managedTable;
    private static string s_lastError = string.Empty;

    internal static ulong CurrentHandle { get; private set; }

    /// <summary>Installs the host table and returns the pointer to our table.</summary>
    public static nint Install(nint hostApi, Assembly editorAssembly)
    {
        if (hostApi == 0 || editorAssembly is null)
            return 0;
        return InstallInternal(hostApi, editorAssembly);
    }

    /// <summary>
    /// Installs the host table and (re)loads the project editor-scripts
    /// assembly from disk into a collectible load context.  Passing a null or
    /// missing path installs the host table with no editor scripts, which is a
    /// valid configuration for a project without an Editor directory.
    /// </summary>
    public static nint InstallFromPath(nint hostApi, string? editorScriptsPath)
    {
        if (hostApi == 0)
            return 0;

        UnloadEditorAssembly();

        Assembly? editorAssembly = null;
        if (!string.IsNullOrEmpty(editorScriptsPath) && File.Exists(editorScriptsPath))
        {
            var fullPath = Path.GetFullPath(editorScriptsPath);
            var context = new EditorScriptsLoadContext(fullPath);
            try
            {
                editorAssembly = context.LoadEditorScripts(fullPath);
                s_loadContext = context;
            }
            catch
            {
                context.Unload();
                s_loadContext = null;
            }
        }

        s_editorAssemblyPath = editorAssembly is null ? 0 : Marshal.StringToCoTaskMemUTF8(Path.GetFullPath(editorScriptsPath!));
        return InstallInternal(hostApi, editorAssembly);
    }

    /// <summary>Drops the loaded editor scripts and every registration they created.</summary>
    public static void Uninstall()
    {
        UnloadEditorAssembly();
        s_host = null;
        s_editorAssembly = null;
    }

    private static void UnloadEditorAssembly()
    {
        s_componentEditors.Clear();
        s_windows.Clear();
        s_windowMenuItems.Clear();
        s_commandMenuItems.Clear();
        s_editorAssembly = null;
        if (s_editorAssemblyPath != 0)
        {
            Marshal.FreeCoTaskMem(s_editorAssemblyPath);
            s_editorAssemblyPath = 0;
        }
        // Release the collectible context so an edited, added, or deleted script
        // is observed on the next scan instead of being served from stale types.
        s_loadContext?.Unload();
        s_loadContext = null;
    }

    private static nint InstallInternal(nint hostApi, Assembly? editorAssembly)
    {

        var host = (EditorHostApiTable*)hostApi;
        if (host->Header.AbiVersion != SupportedAbiVersion)
            return 0;

        s_host = host;
        s_editorAssembly = editorAssembly;
        s_componentEditors.Clear();
        s_windows.Clear();

        if (s_managedTable is null)
        {
            var table = (ManagedEditorScriptApiTableMirror*)Marshal.AllocHGlobal(
                sizeof(ManagedEditorScriptApiTableMirror));
            table->Header.Size = (uint)sizeof(ManagedEditorScriptApiTableMirror);
            table->Header.AbiVersion = SupportedAbiVersion;
            table->Header.SchemaHash = 0ul;
            table->Scan = &ScanEntry;
            table->CreateEditor = &CreateEditorEntry;
            table->DestroyEditor = &DestroyEditorEntry;
            table->OnEnable = &OnEnableEntry;
            table->OnDisable = &OnDisableEntry;
            table->OnInspectorGUI = &OnInspectorGUIEntry;
            table->CreateWindow = &CreateWindowEntry;
            table->DestroyWindow = &DestroyWindowEntry;
            table->OnWindowEnable = &OnWindowEnableEntry;
            table->OnWindowDisable = &OnWindowDisableEntry;
            table->OnWindowGUI = &OnWindowGUIEntry;
            table->InvokeMenuItem = &InvokeMenuItemEntry;
            s_managedTable = table;
        }

        return (nint)s_managedTable;
    }

    /// <summary>
    /// Collectible load context for the project editor-scripts assembly.  Host
    /// assemblies resolve to the copies already loaded in the default context
    /// so <c>SerializedObject</c>, <c>ComponentEditor</c>, and the project
    /// runtime assembly keep a single type identity across a reload.
    /// </summary>
    private sealed class EditorScriptsLoadContext : AssemblyLoadContext
    {
        private readonly AssemblyDependencyResolver _resolver;

        public EditorScriptsLoadContext(string editorScriptsAssemblyPath)
            : base("Nullus.EditorScripts", isCollectible: true)
            => _resolver = new AssemblyDependencyResolver(editorScriptsAssemblyPath);

        public Assembly LoadEditorScripts(string path) => LoadFromAssemblyPath(path);

        protected override Assembly? Load(AssemblyName assemblyName)
        {
            var shared = SharedAssembly(assemblyName);
            if (shared is not null)
                return shared;
            var resolved = _resolver.ResolveAssemblyToPath(assemblyName);
            return resolved is null ? null : LoadFromAssemblyPath(resolved);
        }

        private static Assembly? SharedAssembly(AssemblyName assemblyName)
        {
            if (assemblyName.Name is null)
                return null;
            if (string.Equals(assemblyName.Name, typeof(ComponentEditor).Assembly.GetName().Name, StringComparison.Ordinal)
                || string.Equals(assemblyName.Name, typeof(global::Nullus.Behaviour).Assembly.GetName().Name, StringComparison.Ordinal))
            {
                return assemblyName.Name == typeof(ComponentEditor).Assembly.GetName().Name
                    ? typeof(ComponentEditor).Assembly
                    : typeof(global::Nullus.Behaviour).Assembly;
            }
            // The project runtime assembly (GameScripts) is loaded into the
            // runtime's own collectible context, not the default one. Search
            // every loaded assembly so an editor script binds to the very same
            // Behaviour types the runtime instantiated instead of silently
            // loading a second copy from disk.
            foreach (var loaded in AppDomain.CurrentDomain.GetAssemblies())
            {
                if (string.Equals(loaded.GetName().Name, assemblyName.Name, StringComparison.Ordinal))
                    return loaded;
            }
            return null;
        }
    }

    internal static string LastError => s_lastError;

    // --- host table wrappers -------------------------------------------------

    internal static bool IsHandleValid(ulong handle)
    {
        var host = s_host;
        return host != null && host->IsHandleValid != null && host->IsHandleValid(handle) != 0;
    }

    internal static bool DrawDefaultInspector(ulong handle)
    {
        var host = s_host;
        return host != null && host->DrawDefaultInspector != null && host->DrawDefaultInspector(handle) != 0;
    }

    internal static bool PropertyField(ulong handle, string path, string? label, string? tooltip, bool includeChildren)
    {
        var host = s_host;
        if (host == null || host->PropertyField == null)
            return false;

        var pathPointer = ToUtf8(path);
        var labelPointer = ToUtf8(label);
        var tooltipPointer = ToUtf8(tooltip);
        try
        {
            return host->PropertyField(
                handle,
                (byte*)pathPointer,
                (byte*)labelPointer,
                (byte*)tooltipPointer,
                (byte)(includeChildren ? 1 : 0)) != 0;
        }
        finally
        {
            FreeUtf8(pathPointer, labelPointer, tooltipPointer);
        }
    }

    internal static void Label(ulong handle, string text, string? tooltip)
    {
        var host = s_host;
        if (host == null || host->Label == null)
            return;
        var textPointer = ToUtf8(text);
        var tooltipPointer = ToUtf8(tooltip);
        try
        {
            host->Label(handle, (byte*)textPointer, (byte*)tooltipPointer);
        }
        finally
        {
            FreeUtf8(textPointer, tooltipPointer);
        }
    }

    internal static void Space(ulong handle, float pixels)
    {
        var host = s_host;
        if (host != null && host->Space != null)
            host->Space(handle, pixels);
    }

    internal static bool Toggle(ulong handle, string label, bool value)
    {
        var host = s_host;
        if (host == null || host->Toggle == null)
            return value;
        var labelPointer = ToUtf8(label);
        try
        {
            byte updated = 0;
            var changed = host->Toggle(handle, (byte*)labelPointer, (byte)(value ? 1 : 0), &updated) != 0;
            return changed ? updated != 0 : value;
        }
        finally
        {
            FreeUtf8(labelPointer);
        }
    }

    internal static int IntField(ulong handle, string label, int value)
    {
        var host = s_host;
        if (host == null || host->IntField == null)
            return value;
        var labelPointer = ToUtf8(label);
        try
        {
            var updated = 0;
            var changed = host->IntField(handle, (byte*)labelPointer, value, &updated) != 0;
            return changed ? updated : value;
        }
        finally
        {
            FreeUtf8(labelPointer);
        }
    }

    internal static float FloatField(ulong handle, string label, float value)
    {
        var host = s_host;
        if (host == null || host->FloatField == null)
            return value;
        var labelPointer = ToUtf8(label);
        try
        {
            var updated = 0.0f;
            var changed = host->FloatField(handle, (byte*)labelPointer, value, &updated) != 0;
            return changed ? updated : value;
        }
        finally
        {
            FreeUtf8(labelPointer);
        }
    }

    internal static float Slider(ulong handle, string label, float value, float minimum, float maximum)
    {
        var host = s_host;
        if (host == null || host->Slider == null)
            return value;
        var labelPointer = ToUtf8(label);
        try
        {
            var updated = 0.0f;
            var changed = host->Slider(handle, (byte*)labelPointer, value, minimum, maximum, &updated) != 0;
            return changed ? updated : value;
        }
        finally
        {
            FreeUtf8(labelPointer);
        }
    }

    internal static string TextField(ulong handle, string label, string value)
    {
        var host = s_host;
        if (host == null || host->TextField == null)
            return value;

        var labelPointer = ToUtf8(label);
        var valuePointer = ToUtf8(value);
        var buffer = (byte*)Marshal.AllocHGlobal(TextFieldCapacity);
        try
        {
            new Span<byte>(buffer, TextFieldCapacity).Clear();
            var changed = host->TextField(handle, (byte*)labelPointer, (byte*)valuePointer, buffer, TextFieldCapacity) != 0;
            return changed ? Marshal.PtrToStringUTF8((nint)buffer) ?? value : value;
        }
        finally
        {
            Marshal.FreeHGlobal((nint)buffer);
            FreeUtf8(labelPointer, valuePointer);
        }
    }

    internal static bool Button(ulong handle, string label, string? tooltip, bool enabled)
    {
        var host = s_host;
        if (host == null || host->Button == null)
            return false;
        var labelPointer = ToUtf8(label);
        var tooltipPointer = ToUtf8(tooltip);
        try
        {
            return host->Button(handle, (byte*)labelPointer, (byte*)tooltipPointer, (byte)(enabled ? 1 : 0)) != 0;
        }
        finally
        {
            FreeUtf8(labelPointer, tooltipPointer);
        }
    }

    internal static void HelpBox(ulong handle, string message, MessageType type)
    {
        var host = s_host;
        if (host == null || host->HelpBox == null)
            return;
        var messagePointer = ToUtf8(message);
        try
        {
            host->HelpBox(handle, (byte*)messagePointer, (int)type);
        }
        finally
        {
            FreeUtf8(messagePointer);
        }
    }

    internal static void BeginHorizontal(ulong handle)
    {
        var host = s_host;
        if (host != null && host->BeginHorizontal != null)
            host->BeginHorizontal(handle);
    }

    internal static void EndHorizontal(ulong handle)
    {
        var host = s_host;
        if (host != null && host->EndHorizontal != null)
            host->EndHorizontal(handle);
    }

    internal static void BeginVertical(ulong handle)
    {
        var host = s_host;
        if (host != null && host->BeginVertical != null)
            host->BeginVertical(handle);
    }

    internal static void EndVertical(ulong handle)
    {
        var host = s_host;
        if (host != null && host->EndVertical != null)
            host->EndVertical(handle);
    }

    internal static void OpenWindow(string editorTypeName)
    {
        var host = s_host;
        if (host == null || host->OpenWindow == null)
            return;
        var pointer = ToUtf8(editorTypeName);
        try
        {
            host->OpenWindow((byte*)pointer);
        }
        finally
        {
            FreeUtf8(pointer);
        }
    }

    internal static EditorWindow? GetWindow(Type windowType)
    {
        foreach (var window in s_windows.Values)
        {
            if (window.GetType() == windowType)
                return window;
        }
        return null;
    }

    internal static void CloseWindow(string editorTypeName)
    {
        var host = s_host;
        if (host == null || host->CloseWindow == null)
            return;
        var pointer = ToUtf8(editorTypeName);
        try
        {
            host->CloseWindow((byte*)pointer);
        }
        finally
        {
            FreeUtf8(pointer);
        }
    }

    // --- registry ------------------------------------------------------------

    private static void Scan()
    {
        s_lastError = string.Empty;
        s_windowMenuItems.Clear();
        s_commandMenuItems.Clear();

        var host = s_host;
        var assembly = s_editorAssembly;
        if (host == null || assembly is null)
            return;

        if (host->ClearRegistrations != null)
            host->ClearRegistrations();

        // A project editor-scripts assembly can reference types that are not
        // loadable in this context (a removed script, a renamed runtime type, a
        // missing dependency). Assembly.GetTypes() then throws
        // ReflectionTypeLoadException instead of returning anything, which would
        // otherwise be invisible: report the loader failures and keep scanning
        // the types that did load.
        var types = LoadEditorTypes(assembly, out var loadError);
        if (loadError is not null)
            ReportError(host, loadError);

        foreach (var type in types)
        {
            if (type.IsAbstract)
                continue;

            RegisterComponentEditorFor(host, type);
        }

        foreach (var type in types)
        {
            foreach (var method in type.GetMethods(BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic))
            {
                var attribute = method.GetCustomAttribute<EditorMenuItemAttribute>(false);
                if (attribute is null)
                    continue;
                if (method.GetParameters().Length != 0)
                {
                    ReportError(host, $"Editor menu method '{method.Name}' must be parameterless.");
                    continue;
                }

                // A menu entry declared inside an EditorWindow also names the
                // window, so the native host can reuse the panel title from the
                // menu leaf instead of the raw type name.
                var windowOwner = method.DeclaringType is not null &&
                    typeof(EditorWindow).IsAssignableFrom(method.DeclaringType)
                        ? method.DeclaringType.FullName ?? method.DeclaringType.Name
                        : string.Empty;
                s_commandMenuItems.Add((attribute.MenuPath, method));
                RegisterMenuItem(host, attribute.MenuPath, windowOwner);
            }
        }
    }

    private static void RegisterComponentEditorFor(EditorHostApiTable* host, Type editorType)
    {
        var attribute = editorType.GetCustomAttribute<CustomEditorAttribute>(false);
        if (attribute is null)
            return;
        if (!typeof(ComponentEditor).IsAssignableFrom(editorType))
        {
            ReportError(host, $"'{editorType.FullName}' uses [CustomEditor] but does not derive from ComponentEditor.");
            return;
        }

        var nativeTypeName = ResolveNativeTypeName(attribute.InspectedType);
        if (nativeTypeName is null)
        {
            // A project Behaviour has no NativeTypeName constant: every script
            // component shares one native reflection type, so the editor host
            // matches it by the script's reflected type name instead.
            if (!IsScriptComponent(attribute.InspectedType))
            {
                ReportError(
                    host,
                    $"'{editorType.FullName}' targets '{attribute.InspectedType.FullName}', which is neither a reflected engine component nor a script component.");
                return;
            }
            nativeTypeName = attribute.InspectedType.FullName ?? attribute.InspectedType.Name;
        }

        var supportsMultiObject = editorType.GetCustomAttribute<CanEditMultipleObjectsAttribute>(true) is not null;
        if (host->RegisterComponentEditor == null)
            return;

        var editorPointer = ToUtf8(editorType.FullName ?? editorType.Name);
        var targetPointer = ToUtf8(nativeTypeName);
        try
        {
            if (host->RegisterComponentEditor(
                    (byte*)editorPointer,
                    (byte*)targetPointer,
                    (byte)(attribute.EditorForChildClasses ? 1 : 0),
                    (byte)(supportsMultiObject ? 1 : 0),
                    DefaultEditorPriority) == 0)
            {
                ReportError(host, $"Scripted editor '{editorType.FullName}' was rejected by the editor host.");
            }
        }
        finally
        {
            FreeUtf8(editorPointer, targetPointer);
        }
    }

    private static void RegisterMenuItem(EditorHostApiTable* host, string menuPath, string editorTypeName)
    {
        if (host->RegisterMenuItem == null)
            return;

        var pathPointer = ToUtf8(menuPath);
        var ownerPointer = ToUtf8(editorTypeName);
        try
        {
            if (host->RegisterMenuItem((byte*)pathPointer, (byte*)ownerPointer) == 0)
                ReportError(host, $"Editor menu path '{menuPath}' was rejected by the editor host.");
        }
        finally
        {
            FreeUtf8(pathPointer, ownerPointer);
        }
    }

    private static string? ResolveNativeTypeName(Type targetType)
    {
        var field = targetType.GetField(
            "NativeTypeName",
            BindingFlags.Public | BindingFlags.Static | BindingFlags.FlattenHierarchy);
        if (field is null || !field.IsLiteral)
            return null;
        return field.GetRawConstantValue() as string;
    }

    // Loads every type of the editor-scripts assembly, tolerating types that
    // cannot be resolved. Returns the loadable types plus a diagnostic naming
    // the loader failures so an unusable script is reported instead of silently
    // disabling the whole assembly.
    private static Type[] LoadEditorTypes(Assembly assembly, out string? error)
    {
        error = null;
        try
        {
            return assembly.GetTypes();
        }
        catch (ReflectionTypeLoadException exception)
        {
            var loaded = exception.Types.Where(static type => type is not null).Select(static type => type!).ToArray();
            var failures = exception.LoaderExceptions
                .Where(static failure => failure is not null)
                .Select(static failure => failure!.Message)
                .Distinct(StringComparer.Ordinal)
                .Take(4);
            error = $"Loaded {loaded.Length} of {exception.Types.Length} editor script types; " +
                $"failed types were skipped: {string.Join("; ", failures)}";
            return loaded;
        }
        catch (Exception exception)
        {
            error = $"Editor script types could not be enumerated: {exception.Message}";
            return Array.Empty<Type>();
        }
    }

    private static bool IsScriptComponent(Type targetType)
    {
        for (var current = targetType.BaseType; current is not null; current = current.BaseType)
        {
            if (string.Equals(current.FullName, "Nullus.Behaviour", StringComparison.Ordinal))
                return true;
        }
        return false;
    }

    private static void ReportError(EditorHostApiTable* host, string message)
    {
        s_lastError = message;
        ReportScanFailure(message);
    }

    // Pushes a diagnostic through the host table without needing a live editor
    // handle, so failures raised while the host is still scanning are visible in
    // the editor console.
    private static void ReportScanFailure(string message)
    {
        var host = s_host;
        if (host == null || host->ReportError == null)
            return;
        if (host->ReportError == null)
            return;
        var pointer = ToUtf8(message);
        try
        {
            host->ReportError(0ul, (byte*)pointer);
        }
        finally
        {
            FreeUtf8(pointer);
        }
    }

    private static SerializedObject CreateSerializedObject(ulong handle)
    {
        var host = s_host;
        var api = EditorSerializationHost.Api;
        var count = host != null && host->GetTargetCount != null ? (int)host->GetTargetCount(handle) : 0;
        if (count <= 0 || api.CreateObjectFromInstanceIds is null)
            return SerializedObject.FromNativeHandle(0);

        var instanceIds = new int[count];
        for (var index = 0; index < count; ++index)
            instanceIds[index] = host->GetTargetInstanceId(handle, (uint)index);

        return SerializedObject.FromNativeHandle(api.CreateObjectFromInstanceIds(instanceIds));
    }

    // --- unmanaged entry points ---------------------------------------------

    [UnmanagedCallersOnly]
    private static void ScanEntry()
    {
        // A managed exception must never cross the reverse-P/Invoke boundary:
        // the native caller can only observe it as a foreign exception, which
        // used to surface as "scripted editor scan failed with an unknown
        // exception" with no further detail.
        try
        {
            Scan();
        }
        catch (Exception exception)
        {
            s_lastError = "Editor script scan failed: " + exception.Message;
            ReportScanFailure(s_lastError);
        }
    }

    [UnmanagedCallersOnly]
    private static void CreateEditorEntry(ulong handle, byte* editorTypeName)
    {
        try
        {
            var typeName = Marshal.PtrToStringUTF8((nint)editorTypeName);
            var type = typeName is null ? null : s_editorAssembly?.GetType(typeName);
            if (type is null || Activator.CreateInstance(type) is not ComponentEditor editor)
            {
                s_lastError = $"Scripted editor '{typeName}' could not be created.";
                return;
            }

            editor.Handle = handle;
            editor.serializedObject = CreateSerializedObject(handle);
            s_componentEditors[handle] = editor;
        }
        catch (Exception exception)
        {
            s_lastError = exception.Message;
        }
    }

    [UnmanagedCallersOnly]
    private static void DestroyEditorEntry(ulong handle)
    {
        if (!s_componentEditors.Remove(handle, out var editor))
            return;
        try
        {
            editor.serializedObject?.Dispose();
        }
        catch (Exception exception)
        {
            s_lastError = exception.Message;
        }
    }

    [UnmanagedCallersOnly]
    private static void OnEnableEntry(ulong handle)
    {
        if (s_componentEditors.TryGetValue(handle, out var editor))
        {
            try
            {
                editor.OnEnable();
            }
            catch (Exception exception)
            {
                s_lastError = exception.Message;
            }
        }
    }

    [UnmanagedCallersOnly]
    private static void OnDisableEntry(ulong handle)
    {
        if (s_componentEditors.TryGetValue(handle, out var editor))
        {
            try
            {
                editor.OnDisable();
            }
            catch (Exception exception)
            {
                s_lastError = exception.Message;
            }
        }
    }

    [UnmanagedCallersOnly]
    private static byte OnInspectorGUIEntry(ulong handle)
    {
        if (!s_componentEditors.TryGetValue(handle, out var editor))
            return 0;

        CurrentHandle = handle;
        try
        {
            editor.serializedObject?.UpdateIfRequiredOrScript();
            editor.OnInspectorGUI();
            return 1;
        }
        catch (Exception exception)
        {
            s_lastError = exception.Message;
            return 0;
        }
        finally
        {
            CurrentHandle = 0ul;
        }
    }

    [UnmanagedCallersOnly]
    private static void CreateWindowEntry(ulong handle, byte* editorTypeName)
    {
        try
        {
            var typeName = Marshal.PtrToStringUTF8((nint)editorTypeName);
            var type = typeName is null ? null : s_editorAssembly?.GetType(typeName);
            if (type is null || Activator.CreateInstance(type) is not EditorWindow window)
            {
                s_lastError = $"Scripted editor window '{typeName}' could not be created.";
                return;
            }

            window.Handle = handle;
            s_windows[handle] = window;
        }
        catch (Exception exception)
        {
            s_lastError = exception.Message;
        }
    }

    [UnmanagedCallersOnly]
    private static void DestroyWindowEntry(ulong handle) => s_windows.Remove(handle);

    [UnmanagedCallersOnly]
    private static void OnWindowEnableEntry(ulong handle)
    {
        if (s_windows.TryGetValue(handle, out var window))
        {
            try
            {
                window.OnEnable();
            }
            catch (Exception exception)
            {
                s_lastError = exception.Message;
            }
        }
    }

    [UnmanagedCallersOnly]
    private static void OnWindowDisableEntry(ulong handle)
    {
        if (s_windows.TryGetValue(handle, out var window))
        {
            try
            {
                window.OnDisable();
            }
            catch (Exception exception)
            {
                s_lastError = exception.Message;
            }
        }
    }

    [UnmanagedCallersOnly]
    private static byte OnWindowGUIEntry(ulong handle)
    {
        if (!s_windows.TryGetValue(handle, out var window))
            return 0;

        CurrentHandle = handle;
        try
        {
            window.OnGUI();
            return 1;
        }
        catch (Exception exception)
        {
            s_lastError = exception.Message;
            return 0;
        }
        finally
        {
            CurrentHandle = 0ul;
        }
    }

    [UnmanagedCallersOnly]
    private static byte InvokeMenuItemEntry(byte* menuPath)
    {
        var path = Marshal.PtrToStringUTF8((nint)menuPath);
        if (string.IsNullOrEmpty(path))
            return 0;

        foreach (var (menuPathValue, windowType) in s_windowMenuItems)
        {
            if (!string.Equals(menuPathValue, path, StringComparison.Ordinal))
                continue;
            OpenWindow(windowType.FullName ?? windowType.Name);
            return 1;
        }

        foreach (var (menuPathValue, method) in s_commandMenuItems)
        {
            if (!string.Equals(menuPathValue, path, StringComparison.Ordinal))
                continue;
            try
            {
                method.Invoke(null, null);
                return 1;
            }
            catch (Exception exception)
            {
                s_lastError = exception.Message;
                return 0;
            }
        }
        return 0;
    }

    // --- helpers -------------------------------------------------------------

    private static nint ToUtf8(string? text)
        => string.IsNullOrEmpty(text) ? 0 : Marshal.StringToCoTaskMemUTF8(text);

    private static void FreeUtf8(params nint[] pointers)
    {
        foreach (var pointer in pointers)
        {
            if (pointer != 0)
                Marshal.FreeCoTaskMem(pointer);
        }
    }
}

using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace Nullus.Editor;

/// <summary>Message severity used by <see cref="ComponentEditor.HelpBox"/>.</summary>
public enum MessageType
{
    None = 0,
    Info = 1,
    Warning = 2,
    Error = 3
}

/// <summary>Label text plus optional hover tooltip.</summary>
public sealed class GUIContent
{
    public GUIContent(string text, string? tooltip = null)
    {
        Text = text ?? string.Empty;
        Tooltip = tooltip ?? string.Empty;
    }

    public string Text { get; }
    public string Tooltip { get; }
}

/// <summary>
/// Declares the component type a scripted editor draws.  This mirrors the
/// external editor's attribute of the same name, including child-class
/// matching.
/// </summary>
[AttributeUsage(AttributeTargets.Class, AllowMultiple = false, Inherited = false)]
public sealed class CustomEditorAttribute : Attribute
{
    public CustomEditorAttribute(Type inspectedType, bool editorForChildClasses = false)
    {
        InspectedType = inspectedType ?? throw new ArgumentNullException(nameof(inspectedType));
        EditorForChildClasses = editorForChildClasses;
    }

    public Type InspectedType { get; }
    public bool EditorForChildClasses { get; }
}

/// <summary>Opts a scripted editor into multi-object editing.</summary>
[AttributeUsage(AttributeTargets.Class, AllowMultiple = false, Inherited = true)]
public sealed class CanEditMultipleObjectsAttribute : Attribute
{
}

/// <summary>
/// Adds a static method to the editor menu bar.  Only the Window and Tools
/// top-level menus are honoured so a script cannot displace editor chrome.
/// </summary>
[AttributeUsage(AttributeTargets.Method, AllowMultiple = false, Inherited = false)]
public sealed class EditorMenuItemAttribute : Attribute
{
    public EditorMenuItemAttribute(string menuPath)
    {
        MenuPath = menuPath ?? throw new ArgumentNullException(nameof(menuPath));
    }

    public string MenuPath { get; }
}

/// <summary>
/// Base class for a scripted component inspector.  Values are never written
/// directly: a script stages them through <see cref="serializedObject"/> and
/// commits with <see cref="ApplyModifiedProperties"/>, exactly like the native
/// inspectors, so Undo, Prefab override and multi-object semantics are shared.
/// </summary>
public abstract class ComponentEditor
{
    internal ulong Handle { get; set; }

    public SerializedObject serializedObject { get; internal set; } = null!;

    public global::Nullus.Object? target => serializedObject?.targetObject;

    public global::Nullus.Object[] targets => serializedObject?.targetObjects ?? Array.Empty<global::Nullus.Object>();

    public bool isEditingMultipleObjects => serializedObject?.isEditingMultipleObjects ?? false;

    public virtual void OnEnable()
    {
    }

    public virtual void OnDisable()
    {
    }

    public abstract void OnInspectorGUI();

    public bool DrawDefaultInspector() => EditorScriptingRuntime.DrawDefaultInspector(Handle);

    public bool PropertyField(SerializedProperty property)
        => property is null ? false : EditorScriptingRuntime.PropertyField(Handle, property.propertyPath, null, null, true);

    public bool PropertyField(string propertyPath)
        => EditorScriptingRuntime.PropertyField(Handle, propertyPath, null, null, true);

    public bool PropertyField(SerializedProperty property, GUIContent label)
        => property is null ? false : EditorScriptingRuntime.PropertyField(Handle, property.propertyPath, label.Text, label.Tooltip, true);

    public bool PropertyField(string propertyPath, GUIContent label)
        => EditorScriptingRuntime.PropertyField(Handle, propertyPath, label.Text, label.Tooltip, true);

    public bool PropertyField(string propertyPath, string label, string? tooltip = null, bool includeChildren = true)
        => EditorScriptingRuntime.PropertyField(Handle, propertyPath, label, tooltip, includeChildren);

    public void HelpBox(string message, MessageType type = MessageType.Info)
        => EditorScriptingRuntime.HelpBox(Handle, message, type);

    public bool Button(string label, bool enabled = true, string? tooltip = null)
        => EditorScriptingRuntime.Button(Handle, label, tooltip, enabled);

    public bool ApplyModifiedProperties() => serializedObject?.ApplyModifiedProperties() ?? false;

    public bool ApplyModifiedPropertiesWithoutUndo() => serializedObject?.ApplyModifiedPropertiesWithoutUndo() ?? false;

    public void DiscardModifiedProperties() => serializedObject?.DiscardModifiedProperties();
}

/// <summary>
/// Base class for a scripted tool window.  The native host owns the dockable
/// panel; this object only draws its content.
/// </summary>
public abstract class EditorWindow
{
    internal ulong Handle { get; set; }

    public string title { get; set; } = string.Empty;

    public bool isOpen => EditorScriptingRuntime.IsHandleValid(Handle);

    public virtual void OnEnable()
    {
    }

    public virtual void OnDisable()
    {
    }

    public abstract void OnGUI();

    public void Open() => EditorScriptingRuntime.OpenWindow(GetType().FullName ?? GetType().Name);

    public void Close() => EditorScriptingRuntime.CloseWindow(GetType().FullName ?? GetType().Name);

    public void Repaint()
    {
    }

    public static T? GetWindow<T>() where T : EditorWindow
        => EditorScriptingRuntime.GetWindow(typeof(T)) as T;
}

/// <summary>Immediate-mode widget helpers shared by editors and windows.</summary>
public static class EditorGUILayout
{
    public static void Label(string text, string? tooltip = null)
        => EditorScriptingRuntime.Label(EditorScriptingRuntime.CurrentHandle, text, tooltip);

    public static void Space(float pixels = 6.0f)
        => EditorScriptingRuntime.Space(EditorScriptingRuntime.CurrentHandle, pixels);

    public static bool Toggle(string label, bool value)
        => EditorScriptingRuntime.Toggle(EditorScriptingRuntime.CurrentHandle, label, value);

    public static int IntField(string label, int value)
        => EditorScriptingRuntime.IntField(EditorScriptingRuntime.CurrentHandle, label, value);

    public static float FloatField(string label, float value)
        => EditorScriptingRuntime.FloatField(EditorScriptingRuntime.CurrentHandle, label, value);

    public static float Slider(string label, float value, float minimum, float maximum)
        => EditorScriptingRuntime.Slider(EditorScriptingRuntime.CurrentHandle, label, value, minimum, maximum);

    public static string TextField(string label, string value)
        => EditorScriptingRuntime.TextField(EditorScriptingRuntime.CurrentHandle, label, value);

    public static bool Button(string label, bool enabled = true, string? tooltip = null)
        => EditorScriptingRuntime.Button(EditorScriptingRuntime.CurrentHandle, label, tooltip, enabled);

    public static void HelpBox(string message, MessageType type = MessageType.Info)
        => EditorScriptingRuntime.HelpBox(EditorScriptingRuntime.CurrentHandle, message, type);

    public static void BeginHorizontal() => EditorScriptingRuntime.BeginHorizontal(EditorScriptingRuntime.CurrentHandle);

    public static void EndHorizontal() => EditorScriptingRuntime.EndHorizontal(EditorScriptingRuntime.CurrentHandle);

    public static void BeginVertical() => EditorScriptingRuntime.BeginVertical(EditorScriptingRuntime.CurrentHandle);

    public static void EndVertical() => EditorScriptingRuntime.EndVertical(EditorScriptingRuntime.CurrentHandle);

    public static bool PropertyField(SerializedProperty? property, string? label = null, string? tooltip = null)
        => property is null
            ? false
            : EditorScriptingRuntime.PropertyField(
                EditorScriptingRuntime.CurrentHandle,
                property.propertyPath,
                label,
                tooltip,
                true);
}

/// <summary>Alias kept so scripts can read either name without surprises.</summary>
public static class EditorGUI
{
    public static void Label(string text, string? tooltip = null) => EditorGUILayout.Label(text, tooltip);

    public static bool Toggle(string label, bool value) => EditorGUILayout.Toggle(label, value);

    public static string TextField(string label, string value) => EditorGUILayout.TextField(label, value);

    public static bool Button(string label, bool enabled = true, string? tooltip = null)
        => EditorGUILayout.Button(label, enabled, tooltip);

    public static void HelpBox(string message, MessageType type = MessageType.Info)
        => EditorGUILayout.HelpBox(message, type);
}

[StructLayout(LayoutKind.Sequential)]
internal struct EditorScriptAbiHeader
{
    public uint Size;
    public uint AbiVersion;
    public ulong SchemaHash;
}

/// <summary>
/// Mirror of the native host table.  Field order and types must match
/// Project/Editor/Scripting/EditorScriptAbi.h one for one.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct EditorHostApiTable
{
    public EditorScriptAbiHeader Header;

    public delegate* unmanaged<byte*, byte*, byte, byte, int, byte> RegisterComponentEditor;
    public delegate* unmanaged<byte*, byte*, byte> RegisterMenuItem;
    public delegate* unmanaged<void> ClearRegistrations;

    public delegate* unmanaged<ulong, byte> IsHandleValid;
    public delegate* unmanaged<ulong, uint> GetTargetCount;
    public delegate* unmanaged<ulong, uint, byte*, uint, uint> GetTargetDisplayName;
    public delegate* unmanaged<ulong, uint, int> GetTargetInstanceId;
    public delegate* unmanaged<ulong, byte*, uint, uint> GetSearchText;
    public delegate* unmanaged<ulong, byte> IsMultiObject;
    public delegate* unmanaged<ulong, byte*, void> ReportError;

    public delegate* unmanaged<ulong, byte*, byte*, byte*, byte, byte> PropertyField;
    public delegate* unmanaged<ulong, byte> DrawDefaultInspector;

    public delegate* unmanaged<ulong, byte*, byte*, void> Label;
    public delegate* unmanaged<ulong, float, void> Space;
    public delegate* unmanaged<ulong, byte*, byte, byte*, byte> Toggle;
    public delegate* unmanaged<ulong, byte*, int, int*, byte> IntField;
    public delegate* unmanaged<ulong, byte*, float, float*, byte> FloatField;
    public delegate* unmanaged<ulong, byte*, float, float, float, float*, byte> Slider;
    public delegate* unmanaged<ulong, byte*, byte*, byte*, uint, byte> TextField;
    public delegate* unmanaged<ulong, byte*, byte*, byte, byte> Button;
    public delegate* unmanaged<ulong, byte*, int, void> HelpBox;
    public delegate* unmanaged<ulong, void> BeginHorizontal;
    public delegate* unmanaged<ulong, void> EndHorizontal;
    public delegate* unmanaged<ulong, void> BeginVertical;
    public delegate* unmanaged<ulong, void> EndVertical;

    public delegate* unmanaged<byte*, void> OpenWindow;
    public delegate* unmanaged<byte*, void> CloseWindow;
}

/// <summary>Mirror of the managed table native reads back.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct ManagedEditorScriptApiTableMirror
{
    public EditorScriptAbiHeader Header;
    public delegate* unmanaged<void> Scan;
    public delegate* unmanaged<ulong, byte*, void> CreateEditor;
    public delegate* unmanaged<ulong, void> DestroyEditor;
    public delegate* unmanaged<ulong, void> OnEnable;
    public delegate* unmanaged<ulong, void> OnDisable;
    public delegate* unmanaged<ulong, byte> OnInspectorGUI;
    public delegate* unmanaged<ulong, byte*, void> CreateWindow;
    public delegate* unmanaged<ulong, void> DestroyWindow;
    public delegate* unmanaged<ulong, void> OnWindowEnable;
    public delegate* unmanaged<ulong, void> OnWindowDisable;
    public delegate* unmanaged<ulong, byte> OnWindowGUI;
    public delegate* unmanaged<byte*, byte> InvokeMenuItem;
}

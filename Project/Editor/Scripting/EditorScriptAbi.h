#pragma once

#include <cstdint>

namespace NLS::Editor::Scripting
{
// Project editor scripts (Assets/**/Editor/**/*.cs) live in their own
// assembly and are the only managed code allowed to author editor UI.  The
// two tables below are the complete contract between that assembly and the
// native editor host.
//
//   EditorScriptHostApiTable     : native provides, managed calls
//   ManagedEditorScriptApiTable  : managed provides, native calls
//
// Both follow the same append-only rules as the serialization ABI: the header
// carries the table size and every new entry is appended, so an older peer
// simply observes a smaller table and a null function pointer.

// Slot + generation handle.  Native allocates it and passes it back into the
// managed table, so both sides always agree on identity and a stale handle
// after a domain reload is rejected instead of dereferenced.
using EditorScriptHandle = uint64_t;

struct EditorScriptAbiHeader
{
    uint32_t size = 0u;
    uint32_t abiVersion = 1u;
    uint64_t schemaHash = 0u;
};

enum class EditorScriptMessageKind : int32_t
{
    Info = 0,
    Warning = 1,
    Error = 2
};

// Native -> managed.  Every function is called on the editor main thread
// inside an ImGui frame, so drawing is immediate and ordered exactly like the
// managed OnInspectorGUI that triggered it.  All string inputs are UTF-8 and
// borrowed for the duration of the call; string outputs use the usual
// caller-provided buffer plus required-length return.
struct EditorScriptHostApiTable
{
    EditorScriptAbiHeader header;

    // Descriptor registration.  The managed registry calls these once per
    // domain load, before any editor instance is created.
    uint8_t (*registerComponentEditor)(
        const char* editorTypeName,
        const char* targetTypeName,
        uint8_t editorForChildClasses,
        uint8_t supportsMultiObject,
        int32_t priority) = nullptr;
    uint8_t (*registerMenuItem)(const char* menuPath, const char* editorTypeName) = nullptr;
    void (*clearRegistrations)() = nullptr;

    // Per-editor context.
    uint8_t (*isHandleValid)(EditorScriptHandle handle) = nullptr;
    uint32_t (*getTargetCount)(EditorScriptHandle handle) = nullptr;
    uint32_t (*getTargetDisplayName)(EditorScriptHandle handle, uint32_t index, char* output, uint32_t capacity) = nullptr;
    // Runtime instance id of one target, valid for the frame that is drawing
    // the editor.  Managed SerializedObject instances are created from these
    // ids, so a script never receives a component pointer.
    int32_t (*getTargetInstanceId)(EditorScriptHandle handle, uint32_t index) = nullptr;
    uint32_t (*getSearchText)(EditorScriptHandle handle, char* output, uint32_t capacity) = nullptr;
    uint8_t (*isMultiObject)(EditorScriptHandle handle) = nullptr;
    void (*reportError)(EditorScriptHandle handle, const char* message) = nullptr;

    // Property drawing.  Returns 1 when the widget was drawn.
    uint8_t (*propertyField)(
        EditorScriptHandle handle,
        const char* propertyPath,
        const char* label,
        const char* tooltip,
        uint8_t includeChildren) = nullptr;
    uint8_t (*drawDefaultInspector)(EditorScriptHandle handle) = nullptr;

    // Widgets.  A 1 return means "value changed and the out parameter was
    // written"; a 0 return leaves the out parameter untouched.
    void (*label)(EditorScriptHandle handle, const char* text, const char* tooltip) = nullptr;
    void (*space)(EditorScriptHandle handle, float pixels) = nullptr;
    uint8_t (*toggle)(EditorScriptHandle handle, const char* label, uint8_t value, uint8_t* output) = nullptr;
    uint8_t (*intField)(EditorScriptHandle handle, const char* label, int32_t value, int32_t* output) = nullptr;
    uint8_t (*floatField)(EditorScriptHandle handle, const char* label, float value, float* output) = nullptr;
    uint8_t (*slider)(EditorScriptHandle handle, const char* label, float value, float minimum, float maximum, float* output) = nullptr;
    uint8_t (*textField)(EditorScriptHandle handle, const char* label, const char* value, char* output, uint32_t capacity) = nullptr;
    uint8_t (*button)(EditorScriptHandle handle, const char* label, const char* tooltip, uint8_t enabled) = nullptr;
    void (*helpBox)(EditorScriptHandle handle, const char* text, int32_t kind) = nullptr;
    void (*beginHorizontal)(EditorScriptHandle handle) = nullptr;
    void (*endHorizontal)(EditorScriptHandle handle) = nullptr;
    void (*beginVertical)(EditorScriptHandle handle) = nullptr;
    void (*endVertical)(EditorScriptHandle handle) = nullptr;

    // Window lifetime requested by an editor script (EditorWindow.Open/Close).
    void (*openWindow)(const char* editorTypeName) = nullptr;
    void (*closeWindow)(const char* editorTypeName) = nullptr;
};

// Managed -> native.  Native resolves this table through the editor-scripts
// assembly entry point and stores it for the rest of the domain.
struct ManagedEditorScriptApiTable
{
    EditorScriptAbiHeader header;

    // Called by native whenever the domain (re)loads.  The managed registry
    // re-scans its own assembly and pushes descriptors through the host table.
    void (*scan)() = nullptr;

    // `handle` is allocated by native before the call so both sides share one
    // identity authority.
    void (*createEditor)(EditorScriptHandle handle, const char* editorTypeName) = nullptr;
    void (*destroyEditor)(EditorScriptHandle handle) = nullptr;
    void (*onEnable)(EditorScriptHandle handle) = nullptr;
    void (*onDisable)(EditorScriptHandle handle) = nullptr;
    uint8_t (*onInspectorGUI)(EditorScriptHandle handle) = nullptr;

    void (*createWindow)(EditorScriptHandle handle, const char* editorTypeName) = nullptr;
    void (*destroyWindow)(EditorScriptHandle handle) = nullptr;
    void (*onWindowEnable)(EditorScriptHandle handle) = nullptr;
    void (*onWindowDisable)(EditorScriptHandle handle) = nullptr;
    uint8_t (*onWindowGUI)(EditorScriptHandle handle) = nullptr;

    // Menu activation.  Native resolves the registered menu path back to the
    // managed entry point that opened the window or ran the command.
    uint8_t (*invokeMenuItem)(const char* menuPath) = nullptr;
};

[[nodiscard]] const EditorScriptHostApiTable& GetEditorScriptHostApiTable();
} // namespace NLS::Editor::Scripting

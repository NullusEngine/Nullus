#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Scripting/EditorScriptAbi.h"

namespace NLS::Editor::Inspector
{
struct InspectorTargetSet;
}

namespace NLS::Editor::Scripting
{
// A descriptor pushed by the managed editor-script registry.  targetTypeName is
// the reflected native type name emitted by the script generator, so it always
// resolves through the reflection database instead of a hand-maintained table.
struct EditorScriptDescriptor
{
    std::string editorTypeName;
    std::string targetTypeName;
    bool editorForChildClasses = false;
    bool supportsMultiObject = false;
    int32_t priority = 100;
};

struct EditorScriptMenuItem
{
    std::string menuPath;
    std::string editorTypeName;
};

// Implemented by the native owner of one scripted editor or window instance.
// Both adapters share the same handle space, so a single managed registry keyed
// by EditorScriptHandle is enough.
class IEditorScriptTarget
{
public:
    virtual ~IEditorScriptTarget() = default;
    [[nodiscard]] virtual bool IsInspectorTarget() const = 0;
    [[nodiscard]] virtual const Inspector::InspectorTargetSet* GetScriptTargets() const { return nullptr; }
    [[nodiscard]] virtual std::string_view GetScriptSearchText() const { return {}; }
    [[nodiscard]] virtual bool ScriptPropertyField(
        std::string_view propertyPath,
        const std::string& label,
        const std::string& tooltip,
        bool includeChildren)
    {
        (void)propertyPath;
        (void)label;
        (void)tooltip;
        (void)includeChildren;
        return false;
    }
    [[nodiscard]] virtual bool ScriptDrawDefaultInspector() { return false; }
    virtual void ReportScriptError(std::string_view message) { (void)message; }
};

// Editor-wide registry shared by the Inspector adapter and scripted windows.
// It owns descriptor bookkeeping, the handle table, and the native half of the
// script ABI.  Nothing here keeps a raw managed pointer: the managed side only
// ever sees slot+generation handles and validates them through the host table.
class EditorScriptHost final
{
public:
    static EditorScriptHost& Get();

    // --- descriptor registry (fed by the managed registry through the ABI)

    void ClearRegistrations();
    [[nodiscard]] bool RegisterComponentEditor(const EditorScriptDescriptor& descriptor, std::string& error);
    [[nodiscard]] bool RegisterMenuItem(const EditorScriptMenuItem& item, std::string& error);
    [[nodiscard]] const std::vector<EditorScriptDescriptor>& GetDescriptors() const { return m_descriptors; }
    [[nodiscard]] const std::vector<EditorScriptMenuItem>& GetMenuItems() const { return m_menuItems; }
    // Bumped whenever the loaded editor scripts change.  It is part of every
    // scripted registration identifier so the Inspector drops editors built
    // from the previous assembly instead of keeping their types alive, which
    // would also pin the collectible load context.
    [[nodiscard]] uint64_t GetRegistrationGeneration() const { return m_registrationGeneration; }
    // Removes the scripted component editors from InspectorRegistry so a domain
    // reload can register the new descriptors without a duplicate error.
    void ApplyComponentEditorRegistrations();

    // --- handle registry

    EditorScriptHandle RegisterTarget(IEditorScriptTarget& target);
    void UnregisterTarget(EditorScriptHandle handle);
    [[nodiscard]] IEditorScriptTarget* ResolveTarget(EditorScriptHandle handle) const;
    [[nodiscard]] bool IsHandleValid(EditorScriptHandle handle) const;

    // --- managed table

    void SetManagedApi(const ManagedEditorScriptApiTable* api);
    [[nodiscard]] const ManagedEditorScriptApiTable* GetManagedApi() const { return m_managed; }
    void ResetManagedApi();
    // Re-runs the managed scan so descriptors reflect the current domain.  Safe
    // to call when no managed table is installed.
    void RescanManagedRegistrations();

    // --- window service hook (implemented in EditorScriptWindowService)

    void OpenScriptWindow(std::string_view editorTypeName);
    void CloseScriptWindow(std::string_view editorTypeName);

    // Records a diagnostic for the console and the Inspector error box.
    void LogError(std::string message);
    [[nodiscard]] const std::vector<std::string>& GetErrors() const { return m_errors; }
    void ClearErrors() { m_errors.clear(); }

private:
    struct HandleSlot
    {
        IEditorScriptTarget* target = nullptr;
        uint16_t generation = 0;
        bool alive = false;
    };

    EditorScriptHost() = default;

    std::vector<EditorScriptDescriptor> m_descriptors;
    std::vector<EditorScriptMenuItem> m_menuItems;
    std::vector<HandleSlot> m_slots;
    std::vector<uint32_t> m_freeSlots;
    const ManagedEditorScriptApiTable* m_managed = nullptr;
    std::vector<std::string> m_errors;
    uint64_t m_registrationGeneration = 1u;
};

// Publishes scripted editor menu entries into the editor top bar.  Safe to call
// more than once: entries already added for a menu path are skipped, and a call
// before the top bar exists is a no-op.
void InstallScriptMenuEntries();
} // namespace NLS::Editor::Scripting

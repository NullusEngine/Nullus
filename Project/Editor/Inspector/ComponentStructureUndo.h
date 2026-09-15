#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Inspector/InspectorTypes.h"
#include "Object/Object.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Scripting/ScriptTypes.h"
#include "Undo/EditorUndoService.h"
#include "Undo/ObjectUndoTarget.h"

namespace NLS::Engine
{
class GameObject;
namespace Components { class Component; }
}

namespace NLS::Editor::Inspector
{
// Structural edits must survive a removed component's lifetime. Store the
// ObjectGraph record, persistent component identity and original component
// order instead of retaining UI-time raw pointers or transient InstanceIDs
// inside Undo commands.
struct ComponentSnapshot
{
    Engine::Serialize::ObjectRecord record;
    Engine::Serialize::ObjectIdentifier objectIdentifier;
    size_t originalIndex = 0u;
};

struct ComponentStructureSnapshot
{
    std::vector<ComponentSnapshot> components;
};

// A structural edit is described entirely with stable values.  The transient
// edit session resolves the GameObject and component IDs only while Preview is
// executing under the Undo mutation context; the command recorded at Commit
// retains snapshots instead of this request.
enum class ComponentStructureEditKind : uint8_t
{
    Add,
    Reset,
    PasteValues,
    Move,
    Remove
};

struct ComponentStructureEditOperation
{
    ComponentStructureEditKind kind = ComponentStructureEditKind::Add;
    // Add resolves this reflected type name when Preview starts.
    std::string componentTypeName;
    // Script additions use the same durable asset descriptor as the component
    // picker, including its AssetId and normalized source path.
    std::optional<NLS::Scripting::ScriptAsset> scriptAsset;
    // Existing-component operations resolve this persistent identity from the
    // current owner only while Preview is executing.
    Engine::Serialize::ObjectIdentifier componentIdentifier;
    size_t destinationIndex = 0u;
    ComponentSnapshot pastedValues;
};

// Captures the persistent identity for a component. The PersistentManager
// allocates a process-unique local identity for transient scene components
// when they do not yet have an imported ObjectIdentifier.
[[nodiscard]] std::optional<Engine::Serialize::ObjectIdentifier> CaptureComponentObjectIdentifier(
    const Engine::Components::Component& component);

// Component structure history is scoped to the owning Scene or Prefab Stage.
// The returned target retains only an ObjectIdentifier and resolves the owner
// through PersistentManager at preview/undo/redo time.
[[nodiscard]] std::optional<Undo::ObjectUndoTarget> CaptureComponentStructureOwner(
    Engine::GameObject& gameObject,
    const Undo::UndoCommandInfo& info);

[[nodiscard]] ComponentSnapshot CaptureComponentSnapshot(
    const Engine::Components::Component& component,
    size_t originalIndex = 0u);
[[nodiscard]] ComponentStructureSnapshot CaptureComponentStructure(const Engine::GameObject& gameObject);
[[nodiscard]] bool ApplyComponentSnapshot(
    Engine::Components::Component& component,
    const ComponentSnapshot& snapshot);
[[nodiscard]] bool RestoreComponentStructure(
    Engine::GameObject& gameObject,
    const ComponentStructureSnapshot& snapshot);
[[nodiscard]] bool AreComponentStructureSnapshotsEqual(
    const ComponentStructureSnapshot& left,
    const ComponentStructureSnapshot& right);

// The command resolves its ObjectUndoTarget each time it executes. It
// intentionally owns only durable identifiers and ObjectGraph snapshots, never
// the GameObject or Component pointers observed while the Inspector menu opened.
[[nodiscard]] Undo::UndoCommandPtr MakeComponentStructureUndoCommand(
    Undo::UndoCommandInfo info,
    Undo::ObjectUndoTarget owner,
    ComponentStructureSnapshot before,
    ComponentStructureSnapshot after);

// Produces a one-shot EditSession implementation for a structural Inspector
// action.  It is intentionally separate from the persistent command factory:
// the operation is consumed during Preview and only the resulting before/after
// snapshots are retained in history.
[[nodiscard]] Undo::UndoEditPtr MakeComponentStructureUndoEdit(
    Undo::UndoCommandInfo info,
    Undo::ObjectUndoTarget owner,
    ComponentStructureSnapshot before,
    ComponentStructureEditOperation operation);

// Starts, previews and commits one synchronous structural action.  This is
// shared by Inspector and component-picker entry points so no UI can mutate a
// GameObject's component list and then attempt to record it afterwards.
[[nodiscard]] Undo::UndoRequestResult ExecuteComponentStructureEdit(
    Undo::EditorUndoService& undo,
    Undo::UndoCommandInfo info,
    Undo::ObjectUndoTarget owner,
    ComponentStructureSnapshot before,
    ComponentStructureEditOperation operation);
} // namespace NLS::Editor::Inspector

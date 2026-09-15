#pragma once

#include <optional>
#include <string>

#include <Math/Vector3.h>
#include <Engine/PrimitiveType.h>
#include "Undo/EditorUndoService.h"

namespace NLS::Engine
{
class GameObject;
namespace SceneSystem { class Scene; }
}

namespace NLS::Editor::Assets
{
struct EditorAssetDragPayload;
}

namespace NLS::Scripting
{
struct ScriptAsset;
}

namespace NLS::Editor::Panels::HierarchyUndo
{
// These entry points are deliberately independent from the Hierarchy widget.
// Commands retain ObjectIdentifiers through ObjectUndoTarget, so a rebuilt
// panel cannot leave a history entry with a dangling UI or GameObject pointer.
[[nodiscard]] Undo::ExecuteResult RenameGameObject(
    Undo::EditorUndoService& undoService,
    Engine::GameObject& gameObject,
    std::string newName,
    Undo::UndoScopeId scope);

// A null newParent represents moving the GameObject to the scene root. Root
// ordering is not represented by the engine, while parent child ordering is
// captured and restored by the command.
[[nodiscard]] Undo::ExecuteResult ReparentGameObject(
    Undo::EditorUndoService& undoService,
    Engine::GameObject& gameObject,
    Engine::GameObject* newParent,
    Undo::UndoScopeId scope);

[[nodiscard]] Undo::ExecuteResult AddScriptComponent(
    Undo::EditorUndoService& undoService,
    Engine::GameObject& gameObject,
    const ::NLS::Scripting::ScriptAsset& scriptAsset,
    Undo::UndoScopeId scope);

[[nodiscard]] Undo::ExecuteResult DeleteGameObject(
    Undo::EditorUndoService& undoService,
    Engine::GameObject& gameObject,
    Undo::UndoScopeId scope);

[[nodiscard]] Undo::ExecuteResult DuplicateGameObject(
    Undo::EditorUndoService& undoService,
    Engine::GameObject& gameObject,
    Engine::GameObject* forcedParent,
    Undo::UndoScopeId scope);

[[nodiscard]] Undo::ExecuteResult CreateGameObjectFromAsset(
    Undo::EditorUndoService& undoService,
    const Assets::EditorAssetDragPayload& payload,
    Engine::SceneSystem::Scene& scene,
    Engine::GameObject* parent,
    Undo::UndoScopeId scope,
    std::optional<Maths::Vector3> placementOverride = std::nullopt);

    [[nodiscard]] Undo::ExecuteResult CreateGameObjectFromAsset(
        Undo::EditorUndoService& undoService,
        std::string resourcePath,
    Engine::SceneSystem::Scene& scene,
    Engine::GameObject* parent,
        Undo::UndoScopeId scope,
        std::optional<Maths::Vector3> placementOverride = std::nullopt);

    // Menu-driven creation uses the same subtree command as drag/drop and
    // duplicate. The command captures a durable subtree snapshot immediately
    // after its first successful creation, so later Redo never retains a raw
    // GameObject pointer or invokes a UI callback.
    [[nodiscard]] Undo::ExecuteResult CreateEmptyGameObject(
        Undo::EditorUndoService& undoService,
        Engine::SceneSystem::Scene& scene,
        Engine::GameObject* parent,
        Undo::UndoScopeId scope,
        std::string name = {});

    [[nodiscard]] Undo::ExecuteResult CreatePrimitiveGameObject(
        Undo::EditorUndoService& undoService,
        Engine::SceneSystem::Scene& scene,
        Engine::GameObject* parent,
        Engine::PrimitiveType primitiveType,
        Undo::UndoScopeId scope);

    [[nodiscard]] Undo::ExecuteResult CreateGameObjectWithComponent(
        Undo::EditorUndoService& undoService,
        Engine::SceneSystem::Scene& scene,
        Engine::GameObject* parent,
        std::string componentTypeName,
        Undo::UndoScopeId scope);
} // namespace NLS::Editor::Panels::HierarchyUndo

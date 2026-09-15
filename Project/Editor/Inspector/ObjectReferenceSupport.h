#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Assets/ArtifactManifest.h>

#include "Inspector/InspectorTypes.h"

namespace NLS::Editor::Assets
{
struct EditorAssetDragPayload;
}

namespace NLS::Engine
{
class GameObject;
namespace Components { class Component; }
namespace SceneSystem { class Scene; }
}

namespace NLS::Editor::Inspector
{
struct SceneObjectReferencePickerEntry
{
    Engine::GameObject* gameObject = nullptr;
    Engine::Components::Component* component = nullptr;
    std::string displayName;
};

// These helpers are intentionally independent from InspectorView. They are
// shared by native fields, nested array elements, and script SerializedObject
// fields without preserving any transient object pointer in a PropertyNode.
[[nodiscard]] bool IsSupportedInspectorObjectReferenceType(const meta::Type& type);
[[nodiscard]] bool IsInspectorSceneObjectReferenceType(const meta::Type& type);
[[nodiscard]] bool IsInspectorAssetObjectReferenceType(const meta::Type& type);
[[nodiscard]] bool IsCompatibleInspectorReferenceArtifact(
    const meta::Type& type,
    NLS::Core::Assets::ArtifactType artifactType);
[[nodiscard]] std::string GetInspectorObjectReferenceDisplay(
    const meta::Variant& value,
    const meta::Type& type);

bool ClearInspectorObjectReference(meta::Variant& value, const meta::Type& type);
bool AssignInspectorAssetReference(
    meta::Variant& value,
    const meta::Type& type,
    const Assets::EditorAssetDragPayload& payload);
bool AssignInspectorSceneReference(
    meta::Variant& value,
    const meta::Type& type,
    Engine::GameObject* gameObject,
    Engine::Components::Component* component = nullptr);

[[nodiscard]] std::vector<SceneObjectReferencePickerEntry> BuildInspectorSceneObjectReferencePickerEntries(
    const meta::Type& fieldType,
    const Engine::SceneSystem::Scene* scene,
    std::string_view searchText = {});
[[nodiscard]] std::optional<InspectorTargetHandle> GetInspectorObjectReferencePingTarget(
    const meta::Variant& value,
    const meta::Type& type);
} // namespace NLS::Editor::Inspector

#include "Panels/HierarchyUndo.h"
#include "Undo/EditorUndoMutationContext.h"
#include "Undo/ObjectUndoTarget.h"

#include "Assets/EditorAssetDragPayload.h"
#include "Core/EditorActions.h"
#include "Inspector/EditorSelectionService.h"

#include <Components/Component.h>
#include <GameObject.h>
#include <Object/Object.h>
#include <Reflection/Type.h>
#include <SceneSystem/Scene.h>
#include <Scripting/ScriptComponent.h>
#include <Scripting/ScriptTypes.h>
#include <Serialize/ObjectGraphInstantiator.h>
#include <Serialize/ObjectGraphSerializer.h>
#include <ServiceLocator.h>
#include <Utils/String.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace NLS::Editor::Panels::HierarchyUndo
{
namespace
{
using Undo::IEditorUndoCommand;
using Undo::UndoCommandInfo;
using Undo::UndoCompletion;
using Undo::UndoDirection;
using Undo::UndoExecutionContext;
using Undo::UndoStartResult;
using Undo::UndoValidationResult;
using Undo::ObjectUndoTarget;

constexpr size_t kNoSiblingIndex = std::numeric_limits<size_t>::max();

[[nodiscard]] Undo::UndoRequestResult Failed(std::string message)
{
    return {Undo::UndoRequestState::Failed, std::move(message), 0u};
}

[[nodiscard]] Undo::UndoRequestResult NoChange(std::string message)
{
    return {Undo::UndoRequestState::Completed, std::move(message), 0u};
}

[[nodiscard]] Engine::GameObject* ResolveGameObject(const ObjectUndoTarget& target)
{
    return target.ResolveAs<Engine::GameObject>();
}

[[nodiscard]] Engine::GameObject* ResolveGameObject(const std::optional<ObjectUndoTarget>& target)
{
    return target.has_value() ? ResolveGameObject(*target) : nullptr;
}

[[nodiscard]] Engine::SceneSystem::Scene* ResolveScene(const ObjectUndoTarget& target)
{
    return target.ResolveAs<Engine::SceneSystem::Scene>();
}

[[nodiscard]] std::optional<size_t> FindSiblingIndex(
    const Engine::GameObject& child,
    const Engine::GameObject* parent)
{
    if (parent == nullptr)
        return std::nullopt;

    const auto& children = parent->GetChildren();
    const auto it = std::find(children.begin(), children.end(), &child);
    if (it == children.end())
        return std::nullopt;
    return static_cast<size_t>(std::distance(children.begin(), it));
}

[[nodiscard]] std::optional<std::vector<ObjectUndoTarget>> SnapshotChildren(
    const Engine::GameObject* parent,
    const Undo::UndoScopeId& scope)
{
    std::vector<ObjectUndoTarget> snapshot;
    if (parent == nullptr)
        return snapshot;

    const auto& children = parent->GetChildren();
    snapshot.reserve(children.size());
    for (auto* child : children)
    {
        if (child == nullptr)
            return std::nullopt;
        auto target = ObjectUndoTarget::Capture(*child, scope);
        if (!target.has_value())
            return std::nullopt;
        snapshot.push_back(std::move(*target));
    }
    return snapshot;
}

void EraseChildFromSnapshot(
    std::vector<ObjectUndoTarget>& snapshot,
    const ObjectUndoTarget& child)
{
    const auto found = std::find_if(
        snapshot.begin(),
        snapshot.end(),
        [&child](const ObjectUndoTarget& candidate)
        {
            return candidate.HasSameObjectIdentifier(child);
        });
    if (found != snapshot.end())
        snapshot.erase(found);
}

void InsertChildIntoSnapshot(
    std::vector<ObjectUndoTarget>& snapshot,
    ObjectUndoTarget child,
    const size_t desiredIndex)
{
    snapshot.insert(
        snapshot.begin() + static_cast<std::ptrdiff_t>(std::min(desiredIndex, snapshot.size())),
        std::move(child));
}

[[nodiscard]] UndoValidationResult ValidateChildrenSnapshot(
    const std::optional<ObjectUndoTarget>& parentTarget,
    const std::vector<ObjectUndoTarget>& expectedChildren)
{
    if (!parentTarget.has_value())
        return expectedChildren.empty()
            ? UndoValidationResult::Ready()
            : UndoValidationResult::Failed("A root hierarchy cannot have a child ordering snapshot.");

    auto* parent = ResolveGameObject(*parentTarget);
    if (parent == nullptr)
        return UndoValidationResult::Stale("A hierarchy parent no longer exists.");

    const auto& children = parent->GetChildren();
    if (children.size() != expectedChildren.size())
        return UndoValidationResult::Conflict("The hierarchy child list changed outside the Undo timeline.");

    for (size_t index = 0u; index < children.size(); ++index)
    {
        auto* expectedChild = ResolveGameObject(expectedChildren[index]);
        if (expectedChild == nullptr)
            return UndoValidationResult::Stale("A hierarchy child no longer exists.");
        if (children[index] != expectedChild)
            return UndoValidationResult::Conflict("The hierarchy child order changed outside the Undo timeline.");
    }

    return UndoValidationResult::Ready();
}

[[nodiscard]] bool RestoreSiblingIndex(
    Engine::GameObject& child,
    Engine::GameObject& parent,
    const size_t desiredIndex)
{
    auto& children = parent.GetChildren();
    const auto found = std::find(children.begin(), children.end(), &child);
    if (found == children.end())
        return false;

    auto* childPointer = *found;
    children.erase(found);
    const auto insertionIndex = std::min(desiredIndex, children.size());
    children.insert(children.begin() + static_cast<std::ptrdiff_t>(insertionIndex), childPointer);
    return true;
}

void MarkOwningSceneDirty(Engine::GameObject& gameObject)
{
    if (NLS::Core::ServiceLocator::Contains<Editor::Core::EditorActions>())
        NLS::Core::ServiceLocator::Get<Editor::Core::EditorActions>().MarkOwningSceneDirty(gameObject);
}

[[nodiscard]] UndoValidationResult ValidateHierarchyState(
    const ObjectUndoTarget& childTarget,
    const std::optional<ObjectUndoTarget>& expectedParentTarget,
    const size_t expectedSiblingIndex,
    const std::optional<ObjectUndoTarget>& targetParentTarget,
    const std::vector<ObjectUndoTarget>& expectedOldParentChildren,
    const std::vector<ObjectUndoTarget>& expectedNewParentChildren,
    const std::optional<ObjectUndoTarget>& oldParentTarget,
    const std::optional<ObjectUndoTarget>& newParentTarget)
{
    auto* child = ResolveGameObject(childTarget);
    if (child == nullptr)
        return UndoValidationResult::Stale("The GameObject no longer exists.");

    auto* expectedParent = ResolveGameObject(expectedParentTarget);
    if (expectedParentTarget.has_value() && expectedParent == nullptr)
        return UndoValidationResult::Stale("The previous hierarchy parent no longer exists.");

    auto* targetParent = ResolveGameObject(targetParentTarget);
    if (targetParentTarget.has_value() && targetParent == nullptr)
        return UndoValidationResult::Stale("The destination hierarchy parent no longer exists.");

    if (child->GetParent() != expectedParent)
        return UndoValidationResult::Conflict("The GameObject hierarchy changed outside the Undo timeline.");

    if (expectedParent != nullptr)
    {
        const auto actualIndex = FindSiblingIndex(*child, expectedParent);
        if (!actualIndex.has_value() || *actualIndex != expectedSiblingIndex)
            return UndoValidationResult::Conflict("The GameObject sibling order changed outside the Undo timeline.");
    }

    if (targetParent != nullptr && (targetParent == child || targetParent->IsDescendantOf(child)))
        return UndoValidationResult::Conflict("The target parent would create a hierarchy cycle.");

    const auto oldParentValidation = ValidateChildrenSnapshot(oldParentTarget, expectedOldParentChildren);
    if (oldParentValidation.state != Undo::UndoValidationState::Ready)
        return oldParentValidation;

    const auto newParentValidation = ValidateChildrenSnapshot(newParentTarget, expectedNewParentChildren);
    if (newParentValidation.state != Undo::UndoValidationState::Ready)
        return newParentValidation;

    return UndoValidationResult::Ready();
}

[[nodiscard]] UndoStartResult ApplyHierarchyState(
    const ObjectUndoTarget& childTarget,
    const std::optional<ObjectUndoTarget>& targetParentTarget,
    const size_t targetSiblingIndex)
{
    NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT("changing the GameObject hierarchy");
    auto* child = ResolveGameObject(childTarget);
    if (child == nullptr)
        return UndoStartResult::Failed("The GameObject no longer exists.");

    auto* targetParent = ResolveGameObject(targetParentTarget);
    if (targetParentTarget.has_value() && targetParent == nullptr)
        return UndoStartResult::Failed("The destination hierarchy parent no longer exists.");

    if (targetParent != nullptr)
    {
        child->SetParent(*targetParent);
        if (!RestoreSiblingIndex(*child, *targetParent, targetSiblingIndex))
            return UndoStartResult::Failed("The GameObject could not be inserted into the destination hierarchy.");
    }
    else if (child->HasParent())
    {
        child->DetachFromParent();
    }

    MarkOwningSceneDirty(*child);
    return UndoStartResult::Completed();
}

using Inspector::EditorSelectionService;
using Inspector::InspectorTargetHandle;
using Inspector::InspectorTargetKind;
using Inspector::InspectorTargetSet;
using Engine::Components::Component;
using Engine::SceneSystem::Scene;
using Engine::Serialize::ObjectGraphDocument;
using Engine::Serialize::ObjectGraphInstantiator;
using Engine::Serialize::ObjectId;
using Engine::Serialize::ObjectRecord;
using Engine::Serialize::PrefabDocument;
using Engine::Serialize::PropertyRecord;
using Engine::Serialize::PropertyValue;

struct SubtreeLocation
{
    ObjectUndoTarget scene;
    std::optional<ObjectUndoTarget> parent;
    size_t siblingIndex = kNoSiblingIndex;
    std::vector<ObjectUndoTarget> beforeParentChildren;
    std::vector<ObjectUndoTarget> afterParentChildren;
};

struct SubtreeSnapshot
{
    PrefabDocument prefab;
    std::unordered_map<ObjectId, ObjectUndoTarget> objectTargets;
    ObjectUndoTarget root;
};

[[nodiscard]] const ObjectRecord* FindRecord(const ObjectGraphDocument& document, const ObjectId& id)
{
    const auto found = std::find_if(
        document.objects.begin(),
        document.objects.end(),
        [&id](const ObjectRecord& record) { return record.id == id; });
    return found == document.objects.end() ? nullptr : &*found;
}

[[nodiscard]] ObjectRecord* FindMutableRecord(ObjectGraphDocument& document, const ObjectId& id)
{
    const auto found = std::find_if(
        document.objects.begin(),
        document.objects.end(),
        [&id](const ObjectRecord& record) { return record.id == id; });
    return found == document.objects.end() ? nullptr : &*found;
}

[[nodiscard]] const PropertyRecord* FindProperty(const ObjectRecord& record, const std::string_view name)
{
    const auto found = std::find_if(
        record.properties.begin(),
        record.properties.end(),
        [name](const PropertyRecord& property) { return property.name == name; });
    return found == record.properties.end() ? nullptr : &*found;
}

[[nodiscard]] PropertyRecord* FindMutableProperty(ObjectRecord& record, const std::string_view name)
{
    const auto found = std::find_if(
        record.properties.begin(),
        record.properties.end(),
        [name](const PropertyRecord& property) { return property.name == name; });
    return found == record.properties.end() ? nullptr : &*found;
}

[[nodiscard]] bool NormalizeSnapshotRootParent(PrefabDocument& prefab)
{
    auto* root = FindMutableRecord(prefab.graph, prefab.graph.root);
    if (root == nullptr)
        return false;

    if (auto* parent = FindMutableProperty(*root, "parent"))
    {
        parent->value = PropertyValue::Null();
        return true;
    }

    root->properties.push_back({"parent", PropertyValue::Null()});
    return true;
}

[[nodiscard]] bool CaptureSubtreeObjectTargets(
    const ObjectGraphDocument& document,
    const ObjectId& objectId,
    Engine::GameObject& gameObject,
    const Undo::UndoScopeId& scope,
    std::unordered_map<ObjectId, ObjectUndoTarget>& objectTargets)
{
    const auto* record = FindRecord(document, objectId);
    auto gameObjectTarget = ObjectUndoTarget::Capture(gameObject, scope);
    if (record == nullptr || !gameObjectTarget.has_value() ||
        !objectTargets.emplace(objectId, std::move(*gameObjectTarget)).second)
        return false;

    const auto* componentProperty = FindProperty(*record, "components");
    if (componentProperty == nullptr || componentProperty->value.GetKind() != PropertyValue::Kind::Array)
        return false;

    const auto& components = gameObject.GetComponents();
    size_t componentIndex = 0u;
    for (const auto& reference : componentProperty->value.GetArray())
    {
        while (componentIndex < components.size() && components[componentIndex] == nullptr)
            ++componentIndex;
        if (componentIndex >= components.size() || reference.GetKind() != PropertyValue::Kind::OwnedReference)
            return false;
        auto componentTarget = ObjectUndoTarget::Capture(*components[componentIndex], scope);
        if (!componentTarget.has_value() ||
            !objectTargets.emplace(reference.GetObjectId(), std::move(*componentTarget)).second)
            return false;
        ++componentIndex;
    }
    while (componentIndex < components.size())
    {
        if (components[componentIndex] != nullptr)
            return false;
        ++componentIndex;
    }

    const auto* childrenProperty = FindProperty(*record, "children");
    if (childrenProperty == nullptr || childrenProperty->value.GetKind() != PropertyValue::Kind::Array)
        return false;

    const auto& children = gameObject.GetChildren();
    size_t childIndex = 0u;
    for (const auto& reference : childrenProperty->value.GetArray())
    {
        while (childIndex < children.size() && children[childIndex] == nullptr)
            ++childIndex;
        if (childIndex >= children.size() || reference.GetKind() != PropertyValue::Kind::OwnedReference)
            return false;
        if (!CaptureSubtreeObjectTargets(
                document,
                reference.GetObjectId(),
                *children[childIndex],
                scope,
                objectTargets))
            return false;
        ++childIndex;
    }
    while (childIndex < children.size())
    {
        if (children[childIndex] != nullptr)
            return false;
        ++childIndex;
    }
    return true;
}

[[nodiscard]] std::optional<SubtreeSnapshot> CaptureSubtree(
    Engine::GameObject& root,
    const Undo::UndoScopeId& scope)
{
    if (root.GetScene() == nullptr)
        return std::nullopt;

    SubtreeSnapshot snapshot;
    snapshot.prefab = Engine::Serialize::ObjectGraphSerializer::SerializePrefab(root);
    if (!NormalizeSnapshotRootParent(snapshot.prefab) ||
        !CaptureSubtreeObjectTargets(
            snapshot.prefab.graph,
            snapshot.prefab.graph.root,
            root,
            scope,
            snapshot.objectTargets))
    {
        return std::nullopt;
    }
    const auto rootTarget = snapshot.objectTargets.find(snapshot.prefab.graph.root);
    if (rootTarget == snapshot.objectTargets.end())
        return std::nullopt;
    snapshot.root = rootTarget->second;
    return snapshot;
}

[[nodiscard]] size_t EstimateSnapshotBytes(const SubtreeSnapshot& snapshot)
{
    size_t bytes = sizeof(SubtreeSnapshot) + snapshot.objectTargets.size() *
        (sizeof(ObjectId) + sizeof(ObjectUndoTarget));
    for (const auto& record : snapshot.prefab.graph.objects)
    {
        bytes += sizeof(ObjectRecord) + record.typeName.size() + record.debugName.size() + record.debugPath.size();
        for (const auto& property : record.properties)
            bytes += sizeof(PropertyRecord) + property.name.size();
    }
    return bytes;
}

[[nodiscard]] std::optional<SubtreeLocation> CaptureDeletionLocation(
    Engine::GameObject& root,
    const Undo::UndoScopeId& scope)
{
    SubtreeLocation location;
    auto* scene = root.GetScene();
    auto* parent = root.GetParent();
    if (scene == nullptr)
        return std::nullopt;
    auto sceneTarget = ObjectUndoTarget::Capture(*scene, scope);
    auto rootTarget = ObjectUndoTarget::Capture(root, scope);
    if (!sceneTarget.has_value() || !rootTarget.has_value())
        return std::nullopt;
    location.scene = std::move(*sceneTarget);
    if (parent != nullptr)
    {
        auto parentTarget = ObjectUndoTarget::Capture(*parent, scope);
        if (!parentTarget.has_value())
            return std::nullopt;
        location.parent = std::move(*parentTarget);
    }
    location.siblingIndex = parent != nullptr
        ? FindSiblingIndex(root, parent).value_or(kNoSiblingIndex)
        : kNoSiblingIndex;
    auto beforeChildren = SnapshotChildren(parent, scope);
    if (!beforeChildren.has_value())
        return std::nullopt;
    location.beforeParentChildren = std::move(*beforeChildren);
    location.afterParentChildren = location.beforeParentChildren;
    EraseChildFromSnapshot(location.afterParentChildren, *rootTarget);
    return location;
}

[[nodiscard]] std::optional<SubtreeLocation> CaptureCreationLocation(
    Scene& scene,
    Engine::GameObject* parent,
    const Undo::UndoScopeId& scope)
{
    SubtreeLocation location;
    auto sceneTarget = ObjectUndoTarget::Capture(scene, scope);
    if (!sceneTarget.has_value())
        return std::nullopt;
    location.scene = std::move(*sceneTarget);
    if (parent != nullptr)
    {
        auto parentTarget = ObjectUndoTarget::Capture(*parent, scope);
        if (!parentTarget.has_value())
            return std::nullopt;
        location.parent = std::move(*parentTarget);
    }
    auto beforeChildren = SnapshotChildren(parent, scope);
    if (!beforeChildren.has_value())
        return std::nullopt;
    location.beforeParentChildren = std::move(*beforeChildren);
    return location;
}

[[nodiscard]] bool FinalizeCreationLocation(
    SubtreeLocation& location,
    const Engine::GameObject& root,
    const Undo::UndoScopeId& scope)
{
    auto* parent = ResolveGameObject(location.parent);
    if (root.GetParent() != parent)
        return false;
    location.siblingIndex = parent != nullptr
        ? FindSiblingIndex(root, parent).value_or(kNoSiblingIndex)
        : kNoSiblingIndex;
    if (parent != nullptr && location.siblingIndex == kNoSiblingIndex)
        return false;
    auto afterChildren = SnapshotChildren(parent, scope);
    if (!afterChildren.has_value())
        return false;
    location.afterParentChildren = std::move(*afterChildren);
    return true;
}

[[nodiscard]] UndoValidationResult ValidateLocation(
    const SubtreeLocation& location,
    const std::vector<ObjectUndoTarget>& expectedChildren)
{
    auto* scene = ResolveScene(location.scene);
    if (scene == nullptr)
        return UndoValidationResult::Stale("The owning Scene no longer exists.");
    if (!location.parent.has_value())
        return UndoValidationResult::Ready();

    auto* parent = ResolveGameObject(location.parent);
    if (parent == nullptr || parent->GetScene() != scene)
        return UndoValidationResult::Stale("The hierarchy parent no longer exists.");
    return ValidateChildrenSnapshot(location.parent, expectedChildren);
}

[[nodiscard]] UndoValidationResult ValidateSubtreeAbsent(
    const SubtreeSnapshot& snapshot,
    const SubtreeLocation& location,
    const std::vector<ObjectUndoTarget>& expectedParentChildren)
{
    const auto locationValidation = ValidateLocation(location, expectedParentChildren);
    if (locationValidation.state != Undo::UndoValidationState::Ready)
        return locationValidation;

    for (const auto& [objectId, target] : snapshot.objectTargets)
    {
        static_cast<void>(objectId);
        if (!target.IsValid())
            return UndoValidationResult::Failed("The hierarchy snapshot contains an invalid object target.");
        if (target.Resolve() != nullptr)
            return UndoValidationResult::Conflict("A deleted hierarchy object target is now occupied.");
    }
    return UndoValidationResult::Ready();
}

[[nodiscard]] UndoValidationResult ValidateSubtreePresent(
    const SubtreeSnapshot& snapshot,
    const SubtreeLocation& location,
    const std::vector<ObjectUndoTarget>& expectedParentChildren)
{
    const auto locationValidation = ValidateLocation(location, expectedParentChildren);
    if (locationValidation.state != Undo::UndoValidationState::Ready)
        return locationValidation;

    auto* root = ResolveGameObject(snapshot.root);
    if (root == nullptr)
        return UndoValidationResult::Stale("The hierarchy root no longer exists.");
    auto* scene = ResolveScene(location.scene);
    auto* parent = ResolveGameObject(location.parent);
    if (root->GetScene() != scene || root->GetParent() != parent)
        return UndoValidationResult::Conflict("The hierarchy root moved outside the Undo timeline.");
    if (parent != nullptr)
    {
        const auto actualIndex = FindSiblingIndex(*root, parent);
        if (!actualIndex.has_value() || *actualIndex != location.siblingIndex)
            return UndoValidationResult::Conflict("The hierarchy root sibling order changed outside the Undo timeline.");
    }

    for (const auto& [objectId, target] : snapshot.objectTargets)
    {
        static_cast<void>(objectId);
        if (target.Resolve() == nullptr)
            return UndoValidationResult::Stale("A hierarchy subtree object no longer exists.");
    }
    return UndoValidationResult::Ready();
}

[[nodiscard]] bool AttachAtLocation(
    Engine::GameObject& root,
    const SubtreeLocation& location)
{
    auto* parent = ResolveGameObject(location.parent);
    if (location.parent.has_value() && parent == nullptr)
        return false;
    if (parent == nullptr)
    {
        if (root.HasParent())
            root.DetachFromParent();
        return true;
    }

    root.SetParent(*parent);
    return RestoreSiblingIndex(root, *parent, location.siblingIndex);
}

[[nodiscard]] bool DestroySubtreeImmediately(Scene& scene, Engine::GameObject& root)
{
    // Marking first runs prefab-resource cleanup listeners. Scene destruction
    // is deliberately immediate so the IDs are unavailable only until Undo.
    root.MarkAsDestroy();
    return scene.DestroyGameObject(root);
}

[[nodiscard]] bool MapRestoredSnapshotObjects(
    const SubtreeSnapshot& snapshot,
    const Engine::Serialize::PrefabInstantiationResult& instantiation,
    std::unordered_map<ObjectId, Object*>& liveObjects)
{
    liveObjects.clear();
    liveObjects.reserve(snapshot.objectTargets.size());
    for (const auto& [gameObject, objectId] : instantiation.sourceByInstanceObject)
    {
        if (gameObject == nullptr || !liveObjects.emplace(objectId, gameObject).second)
            return false;

        const auto* record = FindRecord(snapshot.prefab.graph, objectId);
        const auto* componentProperty = record != nullptr ? FindProperty(*record, "components") : nullptr;
        if (componentProperty == nullptr || componentProperty->value.GetKind() != PropertyValue::Kind::Array)
            return false;

        const auto& components = gameObject->GetComponents();
        size_t componentIndex = 0u;
        for (const auto& reference : componentProperty->value.GetArray())
        {
            while (componentIndex < components.size() && components[componentIndex] == nullptr)
                ++componentIndex;
            if (componentIndex >= components.size() || reference.GetKind() != PropertyValue::Kind::OwnedReference ||
                !liveObjects.emplace(reference.GetObjectId(), components[componentIndex].get()).second)
            {
                return false;
            }
            ++componentIndex;
        }
        while (componentIndex < components.size())
        {
            if (components[componentIndex] != nullptr)
                return false;
            ++componentIndex;
        }
    }
    return liveObjects.size() == snapshot.objectTargets.size();
}

struct RestoreSubtreeResult
{
    Engine::GameObject* root = nullptr;
    std::string message;
    bool rollbackFailed = false;
};

[[nodiscard]] RestoreSubtreeResult RestoreSubtree(
    const SubtreeSnapshot& snapshot,
    const SubtreeLocation& location)
{
    NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT("restoring a GameObject hierarchy subtree");
    auto* scene = ResolveScene(location.scene);
    if (scene == nullptr)
        return {nullptr, "The owning Scene no longer exists."};

    auto& persistentManager = Engine::Serialize::PersistentManager::Instance();
    std::vector<const ObjectUndoTarget*> reservations;
    reservations.reserve(snapshot.objectTargets.size());
    for (const auto& [objectId, target] : snapshot.objectTargets)
    {
        static_cast<void>(objectId);
        if (!target.IsValid() ||
            !persistentManager.ReserveObjectIdentifier(target.GetObjectIdentifier()))
        {
            for (const auto* reserved : reservations)
                persistentManager.ReleaseReservedObjectIdentifier(reserved->GetObjectIdentifier());
            return {nullptr, "A restored hierarchy object identifier is already occupied."};
        }
        reservations.push_back(&target);
    }

    auto releaseReservations = [&persistentManager, &reservations]()
    {
        for (const auto* reserved : reservations)
            persistentManager.ReleaseReservedObjectIdentifier(reserved->GetObjectIdentifier());
    };

    Engine::Serialize::LoadPolicy policy;
    policy.missingAssetPolicy = Engine::Serialize::MissingAssetPolicy::Preserve;
    policy.invalidReferencePolicy = Engine::Serialize::InvalidReferencePolicy::Preserve;
    policy.deferAssetReferenceResolution = true;
    policy.skipDeferredAssetReferenceCacheLookup = true;
    const auto instantiation = ObjectGraphInstantiator::InstantiatePrefabGraph(snapshot.prefab.graph, *scene, policy);
    if (instantiation.root == nullptr)
    {
        releaseReservations();
        return {nullptr, "The hierarchy subtree ObjectGraph could not be instantiated."};
    }

    auto cleanupInstantiated = [&scene, root = instantiation.root, &releaseReservations]()
    {
        if (root != nullptr && root->GetScene() == scene)
            static_cast<void>(DestroySubtreeImmediately(*scene, *root));
        releaseReservations();
    };

    std::unordered_map<ObjectId, Object*> liveObjects;
    if (!MapRestoredSnapshotObjects(snapshot, instantiation, liveObjects))
    {
        cleanupInstantiated();
        return {nullptr, "The restored hierarchy subtree does not match its ObjectGraph snapshot."};
    }

    for (const auto& [objectId, target] : snapshot.objectTargets)
    {
        const auto found = liveObjects.find(objectId);
        if (found == liveObjects.end() || found->second == nullptr || !target.IsValid())
        {
            cleanupInstantiated();
            return {nullptr, "The restored hierarchy subtree is missing a stable object mapping."};
        }

        // Bind via the persistent identifier. PersistentManager performs the
        // short-lived InstanceID lookup internally and refuses an occupied
        // target, so the command itself never stores or reuses an InstanceID.
        if (target.Resolve() != nullptr ||
            persistentManager.BindObjectIdentifier(
                *found->second,
                target.GetObjectIdentifier()) == InstanceID_None ||
            !target.Matches(*found->second))
        {
            cleanupInstantiated();
            return {nullptr, "The restored hierarchy subtree could not reclaim its stable object identifiers."};
        }
    }

    if (ResolveGameObject(snapshot.root) != instantiation.root || !AttachAtLocation(*instantiation.root, location))
    {
        cleanupInstantiated();
        return {nullptr, "The restored hierarchy subtree could not be placed at its original location."};
    }

    MarkOwningSceneDirty(*instantiation.root);
    return {instantiation.root, {}};
}

[[nodiscard]] Engine::GameObject* InstantiateSubtreeWithoutStableIds(
    const PrefabDocument& prefab,
    Scene& scene,
    const SubtreeLocation& location)
{
    Engine::Serialize::LoadPolicy policy;
    policy.missingAssetPolicy = Engine::Serialize::MissingAssetPolicy::Preserve;
    policy.invalidReferencePolicy = Engine::Serialize::InvalidReferencePolicy::Preserve;
    policy.deferAssetReferenceResolution = true;
    policy.skipDeferredAssetReferenceCacheLookup = true;
    const auto instantiation = ObjectGraphInstantiator::InstantiatePrefabGraph(prefab.graph, scene, policy);
    if (instantiation.root == nullptr)
        return nullptr;
    if (AttachAtLocation(*instantiation.root, location))
        return instantiation.root;

    static_cast<void>(DestroySubtreeImmediately(scene, *instantiation.root));
    return nullptr;
}

struct UndoSelectionTarget
{
    InspectorTargetKind kind = InspectorTargetKind::None;
    std::optional<ObjectUndoTarget> object;
    std::string assetId;
    std::string resourcePath;
    std::string typeName;
};

struct UndoSelectionSnapshot
{
    std::vector<UndoSelectionTarget> targets;
};

[[nodiscard]] UndoSelectionSnapshot CaptureSelection(const Undo::UndoScopeId& scope)
{
    UndoSelectionSnapshot snapshot;
    const auto& selection = EditorSelectionService::Get().GetSelection();
    snapshot.targets.reserve(selection.targets.size());
    for (const auto& target : selection.targets)
    {
        UndoSelectionTarget captured;
        captured.kind = target.kind;
        captured.assetId = target.assetId;
        captured.resourcePath = target.resourcePath;
        captured.typeName = target.typeName;
        if (target.IsObjectTarget())
        {
            auto* object = Object::IDToPointer(target.instanceId);
            if (object == nullptr)
                continue;
            captured.object = ObjectUndoTarget::Capture(*object, scope);
            if (!captured.object.has_value())
                continue;
        }
        snapshot.targets.push_back(std::move(captured));
    }
    return snapshot;
}

[[nodiscard]] UndoSelectionSnapshot SelectionWithoutSubtree(
    UndoSelectionSnapshot selection,
    const SubtreeSnapshot& snapshot)
{
    selection.targets.erase(
        std::remove_if(
            selection.targets.begin(),
            selection.targets.end(),
            [&snapshot](const UndoSelectionTarget& target)
            {
                if (!target.object.has_value())
                    return false;
                return std::any_of(
                    snapshot.objectTargets.begin(),
                    snapshot.objectTargets.end(),
                    [&target](const auto& entry)
                    {
                        return entry.second.HasSameObjectIdentifier(*target.object);
                    });
            }),
        selection.targets.end());
    return selection;
}

[[nodiscard]] UndoSelectionSnapshot SelectionForGameObject(
    Engine::GameObject& gameObject,
    const Undo::UndoScopeId& scope)
{
    UndoSelectionSnapshot selection;
    auto target = ObjectUndoTarget::Capture(gameObject, scope);
    if (!target.has_value())
        return selection;
    selection.targets.push_back({InspectorTargetKind::GameObject, std::move(*target), {}, {}, {}});
    return selection;
}

void RestoreSelection(const UndoSelectionSnapshot& snapshot)
{
    InspectorTargetSet selection;
    selection.targets.reserve(snapshot.targets.size());
    Engine::GameObject* primaryGameObject = nullptr;
    for (const auto& target : snapshot.targets)
    {
        if (target.object.has_value())
        {
            auto* object = target.object->Resolve();
            if (object == nullptr)
                continue;
            if (target.kind == InspectorTargetKind::GameObject && primaryGameObject == nullptr)
                primaryGameObject = dynamic_cast<Engine::GameObject*>(object);
            selection.targets.push_back(InspectorTargetHandle::FromObject(*object, target.kind));
            continue;
        }

        selection.targets.push_back(InspectorTargetHandle::FromAsset(
            target.kind,
            target.resourcePath,
            target.assetId,
            target.typeName));
    }
    selection.Normalize();

    if (NLS::Core::ServiceLocator::Contains<Editor::Core::EditorActions>())
    {
        auto& actions = NLS::Core::ServiceLocator::Get<Editor::Core::EditorActions>();
        if (selection.IsSingleTarget() && primaryGameObject != nullptr)
        {
            actions.SelectGameObject(*primaryGameObject);
            return;
        }
        if (selection.Empty())
        {
            actions.UnselectGameObject();
            return;
        }
    }
    EditorSelectionService::Get().SetSelection(std::move(selection));
}

[[nodiscard]] std::string MakeDuplicateName(
    const Engine::GameObject& source,
    const Scene& scene,
    const Engine::GameObject* parent)
{
    const auto& adjacent = parent != nullptr ? parent->GetChildren() : scene.GetGameObjects();
    return Utils::String::GenerateUnique(source.GetName(), [&adjacent, parent](std::string candidate)
    {
        return std::none_of(adjacent.begin(), adjacent.end(), [&candidate, parent](const Engine::GameObject* gameObject)
        {
            return gameObject != nullptr && (parent != nullptr || !gameObject->HasParent()) &&
                gameObject->GetName() == candidate;
        });
    });
}

enum class SubtreeCommandKind : uint8_t
{
    Delete,
    Duplicate,
    AssetPayload,
    AssetPath,
    Empty,
    Primitive,
    Component
};

class SubtreeUndoCommand final : public IEditorUndoCommand
{
public:
    SubtreeUndoCommand(
        UndoCommandInfo info,
        SubtreeCommandKind kind,
        SubtreeLocation location,
        UndoSelectionSnapshot selectionBefore,
        std::optional<SubtreeSnapshot> snapshot = std::nullopt,
        std::optional<SubtreeSnapshot> duplicateSource = std::nullopt,
        std::optional<ObjectUndoTarget> duplicateSourceTarget = std::nullopt,
        Assets::EditorAssetDragPayload payload = {},
        std::string resourcePath = {},
        std::optional<Maths::Vector3> placementOverride = std::nullopt,
        std::optional<Engine::PrimitiveType> primitiveType = std::nullopt,
        std::string componentTypeName = {},
        std::string initialName = {})
        : m_info(std::move(info))
        , m_kind(kind)
        , m_location(std::move(location))
        , m_selectionBefore(std::move(selectionBefore))
        , m_snapshot(std::move(snapshot))
        , m_duplicateSource(std::move(duplicateSource))
        , m_duplicateSourceTarget(std::move(duplicateSourceTarget))
        , m_payload(payload)
        , m_resourcePath(std::move(resourcePath))
        , m_placementOverride(std::move(placementOverride))
        , m_primitiveType(primitiveType)
        , m_componentTypeName(std::move(componentTypeName))
        , m_initialName(std::move(initialName))
    {
    }

    [[nodiscard]] UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    [[nodiscard]] UndoValidationResult Validate(
        const UndoDirection direction,
        const UndoExecutionContext&) const override
    {
        if (m_kind == SubtreeCommandKind::Delete)
        {
            if (!m_snapshot.has_value())
                return UndoValidationResult::Failed("The deleted hierarchy subtree has no recovery snapshot.");
            return direction == UndoDirection::Undo
                ? ValidateSubtreeAbsent(*m_snapshot, m_location, m_location.afterParentChildren)
                : ValidateSubtreePresent(*m_snapshot, m_location, m_location.beforeParentChildren);
        }

        if (direction == UndoDirection::Undo)
        {
            if (!m_snapshot.has_value())
                return UndoValidationResult::Failed("The created hierarchy subtree has no recovery snapshot.");
            return ValidateSubtreePresent(*m_snapshot, m_location, m_location.afterParentChildren);
        }

        if (m_snapshot.has_value())
            return ValidateSubtreeAbsent(*m_snapshot, m_location, m_location.beforeParentChildren);

        const auto locationValidation = ValidateLocation(m_location, m_location.beforeParentChildren);
        if (locationValidation.state != Undo::UndoValidationState::Ready)
            return locationValidation;
        if (m_kind == SubtreeCommandKind::Duplicate)
        {
            auto* source = ResolveGameObject(m_duplicateSourceTarget);
            if (source == nullptr || source->GetScene() != ResolveScene(m_location.scene))
                return UndoValidationResult::Stale("The GameObject to duplicate no longer exists.");
            if (!m_duplicateSource.has_value())
                return UndoValidationResult::Failed("The duplicated hierarchy subtree has no source snapshot.");
        }
        else if (!NLS::Core::ServiceLocator::Contains<Editor::Core::EditorActions>())
        {
            return UndoValidationResult::Failed("The EditorActions service is unavailable for hierarchy creation.");
        }

        if (m_kind == SubtreeCommandKind::Primitive && !m_primitiveType.has_value())
            return UndoValidationResult::Failed("The primitive type is unavailable for hierarchy creation.");
        if (m_kind == SubtreeCommandKind::Component)
        {
            const auto componentType = meta::Type::GetFromName(m_componentTypeName);
            if (!componentType.IsValid() || !componentType.DerivesFrom(NLS_TYPEOF(Component)))
                return UndoValidationResult::Failed("The component type is unavailable for hierarchy creation.");
        }
        return UndoValidationResult::Ready();
    }

    UndoStartResult Apply(
        const UndoDirection direction,
        const UndoExecutionContext&,
        UndoCompletion) override
    {
        NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT("changing a hierarchy subtree");
        if (m_kind == SubtreeCommandKind::Delete)
        {
            if (direction == UndoDirection::Undo)
                return RestoreDeletedSubtree();
            return DeleteCreatedSubtree();
        }

        if (direction == UndoDirection::Undo)
            return DeleteCreatedSubtree();
        return m_snapshot.has_value() ? RestoreCreatedSubtree() : CreateInitialSubtree();
    }

    bool TryMergeFrom(const IEditorUndoCommand&) override
    {
        // Hierarchy subtrees are structural changes and must never time-merge.
        return false;
    }

private:
    [[nodiscard]] UndoStartResult DeleteCreatedSubtree()
    {
        if (!m_snapshot.has_value())
            return UndoStartResult::Failed("The hierarchy subtree has no recovery snapshot.");
        auto* scene = ResolveScene(m_location.scene);
        auto* root = ResolveGameObject(m_snapshot->root);
        if (scene == nullptr || root == nullptr || root->GetScene() != scene)
            return UndoStartResult::Failed("The hierarchy subtree no longer exists.");
        MarkOwningSceneDirty(*root);
        if (!DestroySubtreeImmediately(*scene, *root))
            return UndoStartResult::Failed("The hierarchy subtree could not be removed.");

        RestoreSelection(m_selectionBefore);
        return UndoStartResult::Completed();
    }

    [[nodiscard]] UndoStartResult RestoreDeletedSubtree()
    {
        if (!m_snapshot.has_value())
            return UndoStartResult::Failed("The deleted hierarchy subtree has no recovery snapshot.");
        const auto result = RestoreSubtree(*m_snapshot, m_location);
        if (result.root == nullptr)
            return UndoStartResult::Failed(result.message, result.rollbackFailed);
        RestoreSelection(m_selectionBefore);
        return UndoStartResult::Completed();
    }

    [[nodiscard]] UndoStartResult RestoreCreatedSubtree()
    {
        if (!m_snapshot.has_value())
            return UndoStartResult::Failed("The created hierarchy subtree has no recovery snapshot.");
        const auto result = RestoreSubtree(*m_snapshot, m_location);
        if (result.root == nullptr)
            return UndoStartResult::Failed(result.message, result.rollbackFailed);
        RestoreSelection(m_selectionAfter);
        return UndoStartResult::Completed();
    }

    [[nodiscard]] UndoStartResult CreateInitialSubtree()
    {
        auto* scene = ResolveScene(m_location.scene);
        if (scene == nullptr)
            return UndoStartResult::Failed("The owning Scene no longer exists.");

        Engine::GameObject* created = nullptr;
        if (m_kind == SubtreeCommandKind::Duplicate)
        {
            auto* source = ResolveGameObject(m_duplicateSourceTarget);
            if (source == nullptr || !m_duplicateSource.has_value())
                return UndoStartResult::Failed("The GameObject to duplicate no longer exists.");
            created = InstantiateSubtreeWithoutStableIds(m_duplicateSource->prefab, *scene, m_location);
            if (created != nullptr)
            {
                const auto* parent = ResolveGameObject(m_location.parent);
                created->SetName(MakeDuplicateName(*source, *scene, parent));
            }
        }
        else
        {
            if (!NLS::Core::ServiceLocator::Contains<Editor::Core::EditorActions>())
                return UndoStartResult::Failed("The EditorActions service is unavailable for hierarchy creation.");
            auto& actions = NLS::Core::ServiceLocator::Get<Editor::Core::EditorActions>();
            auto* parent = ResolveGameObject(m_location.parent);
            switch (m_kind)
            {
            case SubtreeCommandKind::AssetPayload:
                created = actions.CreateGameObjectFromAssetBlocking(m_payload, true, parent, m_placementOverride);
                break;
            case SubtreeCommandKind::AssetPath:
                created = actions.CreateGameObjectFromAssetBlocking(m_resourcePath, true, parent, m_placementOverride);
                break;
            case SubtreeCommandKind::Empty:
                created = actions.CreateEmptyGameObject(true, parent, m_initialName);
                break;
            case SubtreeCommandKind::Primitive:
                if (!m_primitiveType.has_value())
                    return UndoStartResult::Failed("The primitive type is unavailable for hierarchy creation.");
                created = actions.CreatePrimitive(*m_primitiveType, true, parent);
                break;
            case SubtreeCommandKind::Component:
            {
                const auto componentType = meta::Type::GetFromName(m_componentTypeName);
                if (!componentType.IsValid() || !componentType.DerivesFrom(NLS_TYPEOF(Component)))
                    return UndoStartResult::Failed("The component type is unavailable for hierarchy creation.");

                created = actions.CreateEmptyGameObject(false, parent);
                if (created == nullptr)
                    break;
                if (created->AddComponent(componentType) == nullptr)
                {
                    const bool cleaned = DestroySubtreeImmediately(*scene, *created);
                    return UndoStartResult::Failed(
                        "The component could not be added to the created GameObject.",
                        !cleaned);
                }
                created->SetName(componentType.GetName());
                actions.SelectGameObject(*created);
                break;
            }
            case SubtreeCommandKind::Delete:
            case SubtreeCommandKind::Duplicate:
                return UndoStartResult::Failed("The hierarchy creation command has an invalid kind.");
            }
        }

        if (created == nullptr || created->GetScene() != scene)
            return UndoStartResult::Failed("The hierarchy subtree could not be created.");
        if (m_info.scopes.empty() || !FinalizeCreationLocation(m_location, *created, m_info.scopes.front()))
        {
            const bool cleaned = DestroySubtreeImmediately(*scene, *created);
            return UndoStartResult::Failed(
                "The created hierarchy subtree could not be placed at its requested location.",
                !cleaned);
        }

        auto snapshot = m_info.scopes.empty()
            ? std::optional<SubtreeSnapshot> {}
            : CaptureSubtree(*created, m_info.scopes.front());
        if (!snapshot.has_value())
        {
            const bool cleaned = DestroySubtreeImmediately(*scene, *created);
            return UndoStartResult::Failed(
                "The created hierarchy subtree could not be captured for Undo.",
                !cleaned);
        }

        m_snapshot = std::move(snapshot);
        m_selectionAfter = m_info.scopes.empty()
            ? UndoSelectionSnapshot {}
            : SelectionForGameObject(*created, m_info.scopes.front());
        MarkOwningSceneDirty(*created);
        RestoreSelection(m_selectionAfter);
        return UndoStartResult::Completed();
    }

    UndoCommandInfo m_info;
    SubtreeCommandKind m_kind = SubtreeCommandKind::Delete;
    SubtreeLocation m_location;
    UndoSelectionSnapshot m_selectionBefore;
    UndoSelectionSnapshot m_selectionAfter;
    std::optional<SubtreeSnapshot> m_snapshot;
    std::optional<SubtreeSnapshot> m_duplicateSource;
    std::optional<ObjectUndoTarget> m_duplicateSourceTarget;
    Assets::EditorAssetDragPayload m_payload {};
    std::string m_resourcePath;
    std::optional<Maths::Vector3> m_placementOverride;
    std::optional<Engine::PrimitiveType> m_primitiveType;
    std::string m_componentTypeName;
    std::string m_initialName;
};

[[nodiscard]] ::NLS::Scripting::ScriptComponent* FindScriptComponent(
    Engine::GameObject& gameObject,
    const std::optional<ObjectUndoTarget>& componentTarget)
{
    if (!componentTarget.has_value())
        return nullptr;

    auto* component = componentTarget->ResolveAs<::NLS::Scripting::ScriptComponent>();
    return component != nullptr && component->gameobject() == &gameObject
        ? component
        : nullptr;
}

[[nodiscard]] bool IsSameScriptAsset(
    const ::NLS::Scripting::ScriptAsset& left,
    const ::NLS::Scripting::ScriptAsset& right)
{
    return left.assetId == right.assetId && left.language == right.language &&
        left.sourcePath == right.sourcePath && left.sourceText == right.sourceText &&
        left.scriptType == right.scriptType && left.contentHash == right.contentHash &&
        left.isComponent == right.isComponent;
}

class AddScriptComponentCommand final : public IEditorUndoCommand
{
public:
    AddScriptComponentCommand(
        UndoCommandInfo info,
        ObjectUndoTarget owner,
        ::NLS::Scripting::ScriptAsset asset)
        : m_info(std::move(info))
        , m_owner(std::move(owner))
        , m_asset(std::move(asset))
    {
    }

    [[nodiscard]] UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    [[nodiscard]] UndoValidationResult Validate(
        const UndoDirection direction,
        const UndoExecutionContext&) const override
    {
        auto* owner = ResolveGameObject(m_owner);
        if (owner == nullptr)
            return UndoValidationResult::Stale("The ScriptComponent owner no longer exists.");
        if (direction == UndoDirection::Undo)
        {
            auto* component = FindScriptComponent(*owner, m_component);
            if (component == nullptr)
                return UndoValidationResult::Stale("The ScriptComponent no longer exists.");
            return IsSameScriptAsset(component->GetScriptAsset(), m_asset)
                ? UndoValidationResult::Ready()
                : UndoValidationResult::Conflict("The ScriptComponent changed outside the Undo timeline.");
        }
        if (m_component.has_value() && m_component->Resolve() != nullptr)
            return UndoValidationResult::Conflict("The ScriptComponent identifier is already occupied.");
        return UndoValidationResult::Ready();
    }

    UndoStartResult Apply(
        const UndoDirection direction,
        const UndoExecutionContext&,
        UndoCompletion) override
    {
        NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT("adding or removing a ScriptComponent");
        auto* owner = ResolveGameObject(m_owner);
        if (owner == nullptr)
            return UndoStartResult::Failed("The ScriptComponent owner no longer exists.");

        if (direction == UndoDirection::Undo)
        {
            auto* component = FindScriptComponent(*owner, m_component);
            if (component == nullptr || !owner->RemoveComponent(component))
                return UndoStartResult::Failed("The ScriptComponent could not be removed.");
            MarkOwningSceneDirty(*owner);
            return UndoStartResult::Completed();
        }

        auto& persistentManager = Engine::Serialize::PersistentManager::Instance();
        const bool restoringStableComponent = m_component.has_value();
        if (restoringStableComponent &&
            !persistentManager.ReserveObjectIdentifier(m_component->GetObjectIdentifier()))
        {
            return UndoStartResult::Failed("The ScriptComponent identifier is already occupied.");
        }
        auto* component = owner->AddComponent<::NLS::Scripting::ScriptComponent>();
        if (component == nullptr)
        {
            if (restoringStableComponent)
                persistentManager.ReleaseReservedObjectIdentifier(m_component->GetObjectIdentifier());
            return UndoStartResult::Failed("The ScriptComponent could not be created.");
        }
        if (!m_component.has_value())
        {
            if (m_info.scopes.empty())
            {
                const bool removed = owner->RemoveComponent(component);
                return UndoStartResult::Failed("The ScriptComponent command has no stable Undo scope.", !removed);
            }
            m_component = ObjectUndoTarget::Capture(*component, m_info.scopes.front());
            if (!m_component.has_value())
            {
                const bool removed = owner->RemoveComponent(component);
                return UndoStartResult::Failed("The ScriptComponent could not capture a stable object identifier.", !removed);
            }
        }
        else
        {
            if (persistentManager.BindObjectIdentifier(
                    *component,
                    m_component->GetObjectIdentifier()) == InstanceID_None ||
                !m_component->Matches(*component))
            {
                const bool removed = owner->RemoveComponent(component);
                persistentManager.ReleaseReservedObjectIdentifier(m_component->GetObjectIdentifier());
                return UndoStartResult::Failed("The ScriptComponent could not reclaim its stable object identifier.", !removed);
            }
        }
        component->SetScriptAsset(m_asset);
        MarkOwningSceneDirty(*owner);
        return UndoStartResult::Completed();
    }

    bool TryMergeFrom(const IEditorUndoCommand&) override
    {
        return false;
    }

private:
    UndoCommandInfo m_info;
    ObjectUndoTarget m_owner;
    std::optional<ObjectUndoTarget> m_component;
    ::NLS::Scripting::ScriptAsset m_asset;
};

class RenameGameObjectCommand final : public IEditorUndoCommand
{
public:
    RenameGameObjectCommand(
        UndoCommandInfo info,
        ObjectUndoTarget gameObject,
        std::string before,
        std::string after)
        : m_info(std::move(info))
        , m_gameObject(std::move(gameObject))
        , m_before(std::move(before))
        , m_after(std::move(after))
    {
    }

    [[nodiscard]] UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    [[nodiscard]] UndoValidationResult Validate(
        const UndoDirection direction,
        const UndoExecutionContext&) const override
    {
        auto* gameObject = ResolveGameObject(m_gameObject);
        if (gameObject == nullptr)
            return UndoValidationResult::Stale("The renamed GameObject no longer exists.");

        const auto& expectedName = direction == UndoDirection::Undo ? m_after : m_before;
        return gameObject->GetName() == expectedName
            ? UndoValidationResult::Ready()
            : UndoValidationResult::Conflict("The GameObject name changed outside the Undo timeline.");
    }

    UndoStartResult Apply(
        const UndoDirection direction,
        const UndoExecutionContext&,
        UndoCompletion) override
    {
        NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT("renaming a GameObject");
        auto* gameObject = ResolveGameObject(m_gameObject);
        if (gameObject == nullptr)
            return UndoStartResult::Failed("The renamed GameObject no longer exists.");

        gameObject->SetName(direction == UndoDirection::Undo ? m_before : m_after);
        MarkOwningSceneDirty(*gameObject);
        return UndoStartResult::Completed();
    }

    bool TryMergeFrom(const IEditorUndoCommand& newer) override
    {
        const auto* rename = dynamic_cast<const RenameGameObjectCommand*>(&newer);
        if (rename == nullptr || !rename->m_gameObject.HasSameObjectIdentifier(m_gameObject) ||
            m_after != rename->m_before)
            return false;

        m_after = rename->m_after;
        return true;
    }

private:
    UndoCommandInfo m_info;
    ObjectUndoTarget m_gameObject;
    std::string m_before;
    std::string m_after;
};

class ReparentGameObjectCommand final : public IEditorUndoCommand
{
public:
    ReparentGameObjectCommand(
        UndoCommandInfo info,
        ObjectUndoTarget child,
        std::optional<ObjectUndoTarget> oldParent,
        const size_t oldSiblingIndex,
        std::optional<ObjectUndoTarget> newParent,
        const size_t newSiblingIndex,
        std::vector<ObjectUndoTarget> beforeOldParentChildren,
        std::vector<ObjectUndoTarget> afterOldParentChildren,
        std::vector<ObjectUndoTarget> beforeNewParentChildren,
        std::vector<ObjectUndoTarget> afterNewParentChildren)
        : m_info(std::move(info))
        , m_child(std::move(child))
        , m_oldParent(std::move(oldParent))
        , m_oldSiblingIndex(oldSiblingIndex)
        , m_newParent(std::move(newParent))
        , m_newSiblingIndex(newSiblingIndex)
        , m_beforeOldParentChildren(std::move(beforeOldParentChildren))
        , m_afterOldParentChildren(std::move(afterOldParentChildren))
        , m_beforeNewParentChildren(std::move(beforeNewParentChildren))
        , m_afterNewParentChildren(std::move(afterNewParentChildren))
    {
    }

    [[nodiscard]] UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    [[nodiscard]] UndoValidationResult Validate(
        const UndoDirection direction,
        const UndoExecutionContext&) const override
    {
        if (direction == UndoDirection::Undo)
        {
            return ValidateHierarchyState(
                m_child,
                m_newParent,
                m_newSiblingIndex,
                m_oldParent,
                m_afterOldParentChildren,
                m_afterNewParentChildren,
                m_oldParent,
                m_newParent);
        }

        return ValidateHierarchyState(
            m_child,
            m_oldParent,
            m_oldSiblingIndex,
            m_newParent,
            m_beforeOldParentChildren,
            m_beforeNewParentChildren,
            m_oldParent,
            m_newParent);
    }

    UndoStartResult Apply(
        const UndoDirection direction,
        const UndoExecutionContext&,
        UndoCompletion) override
    {
        if (direction == UndoDirection::Undo)
            return ApplyHierarchyState(m_child, m_oldParent, m_oldSiblingIndex);
        return ApplyHierarchyState(m_child, m_newParent, m_newSiblingIndex);
    }

    bool TryMergeFrom(const IEditorUndoCommand&) override
    {
        // Reparenting is structural and must never time-merge.
        return false;
    }

private:
    UndoCommandInfo m_info;
    ObjectUndoTarget m_child;
    std::optional<ObjectUndoTarget> m_oldParent;
    size_t m_oldSiblingIndex = kNoSiblingIndex;
    std::optional<ObjectUndoTarget> m_newParent;
    size_t m_newSiblingIndex = kNoSiblingIndex;
    std::vector<ObjectUndoTarget> m_beforeOldParentChildren;
    std::vector<ObjectUndoTarget> m_afterOldParentChildren;
    std::vector<ObjectUndoTarget> m_beforeNewParentChildren;
    std::vector<ObjectUndoTarget> m_afterNewParentChildren;
};

[[nodiscard]] UndoCommandInfo BuildInfo(
    std::string label,
    Undo::UndoScopeId scope,
    const ObjectUndoTarget& target,
    const std::string_view property = {})
{
    UndoCommandInfo info;
    info.label = std::move(label);
    info.sourceModule = "Hierarchy";
    info.scopes = {std::move(scope)};
    info.affected = {
        target.GetAffectedKey() + (property.empty() ? std::string {} : "/" + std::string(property))};
    return info;
}
} // namespace

Undo::ExecuteResult RenameGameObject(
    Undo::EditorUndoService& undoService,
    Engine::GameObject& gameObject,
    std::string newName,
    Undo::UndoScopeId scope)
{
    if (!scope.IsValid())
        return Failed("The GameObject has no stable Undo scope.");

    const auto before = gameObject.GetName();
    if (before == newName)
        return NoChange("The GameObject name is unchanged.");

    auto target = ObjectUndoTarget::Capture(gameObject, scope);
    if (!target.has_value())
        return Failed("The GameObject could not capture a stable Undo target.");

    auto info = BuildInfo("Rename GameObject", std::move(scope), *target, "name");
    info.mergeKey = "hierarchy:" + target->GetAffectedKey() + ":name";
    info.allowMerge = true;
    info.estimatedBytes = sizeof(ObjectUndoTarget) + before.size() + newName.size();
    return undoService.Execute(std::make_unique<RenameGameObjectCommand>(
        std::move(info),
        std::move(*target),
        before,
        std::move(newName)));
}

Undo::ExecuteResult ReparentGameObject(
    Undo::EditorUndoService& undoService,
    Engine::GameObject& gameObject,
    Engine::GameObject* newParent,
    Undo::UndoScopeId scope)
{
    if (!scope.IsValid())
        return Failed("The GameObject has no stable Undo scope.");
    if (newParent == &gameObject)
        return {Undo::UndoRequestState::Conflict, "A GameObject cannot be its own parent.", 0u};
    if (newParent != nullptr && newParent->IsDescendantOf(&gameObject))
        return {Undo::UndoRequestState::Conflict, "A GameObject cannot be parented to one of its descendants.", 0u};

    auto* oldParent = gameObject.GetParent();
    if (oldParent == newParent)
        return NoChange("The GameObject is already parented to the selected target.");

    const auto oldSiblingIndex = oldParent != nullptr
        ? FindSiblingIndex(gameObject, oldParent).value_or(kNoSiblingIndex)
        : kNoSiblingIndex;
    if (oldParent != nullptr && oldSiblingIndex == kNoSiblingIndex)
        return Failed("The GameObject is missing from its current parent child list.");

    const auto newSiblingIndex = newParent != nullptr
        ? newParent->GetChildren().size()
        : kNoSiblingIndex;
    auto childTarget = ObjectUndoTarget::Capture(gameObject, scope);
    if (!childTarget.has_value())
        return Failed("The GameObject could not capture a stable Undo target.");
    std::optional<ObjectUndoTarget> oldParentTarget;
    if (oldParent != nullptr)
    {
        oldParentTarget = ObjectUndoTarget::Capture(*oldParent, scope);
        if (!oldParentTarget.has_value())
            return Failed("The previous hierarchy parent could not capture a stable Undo target.");
    }
    std::optional<ObjectUndoTarget> newParentTarget;
    if (newParent != nullptr)
    {
        newParentTarget = ObjectUndoTarget::Capture(*newParent, scope);
        if (!newParentTarget.has_value())
            return Failed("The destination hierarchy parent could not capture a stable Undo target.");
    }
    auto beforeOldParentChildren = SnapshotChildren(oldParent, scope);
    auto beforeNewParentChildren = SnapshotChildren(newParent, scope);
    if (!beforeOldParentChildren.has_value() || !beforeNewParentChildren.has_value())
        return Failed("The hierarchy child order could not capture stable Undo targets.");
    auto afterOldParentChildren = *beforeOldParentChildren;
    EraseChildFromSnapshot(afterOldParentChildren, *childTarget);
    auto afterNewParentChildren = *beforeNewParentChildren;
    if (newParent != nullptr)
        InsertChildIntoSnapshot(afterNewParentChildren, *childTarget, newSiblingIndex);

    auto info = BuildInfo("Reparent GameObject", std::move(scope), *childTarget, "parent");
    info.estimatedBytes =
        sizeof(ObjectUndoTarget) * (3u + beforeOldParentChildren->size() + afterOldParentChildren.size() +
            beforeNewParentChildren->size() + afterNewParentChildren.size()) +
        sizeof(size_t) * 2u;
    return undoService.Execute(std::make_unique<ReparentGameObjectCommand>(
        std::move(info),
        std::move(*childTarget),
        std::move(oldParentTarget),
        oldSiblingIndex,
        std::move(newParentTarget),
        newSiblingIndex,
        std::move(*beforeOldParentChildren),
        std::move(afterOldParentChildren),
        std::move(*beforeNewParentChildren),
        std::move(afterNewParentChildren)));
}

Undo::ExecuteResult AddScriptComponent(
    Undo::EditorUndoService& undoService,
    Engine::GameObject& gameObject,
    const ::NLS::Scripting::ScriptAsset& scriptAsset,
    Undo::UndoScopeId scope)
{
    if (!scope.IsValid())
        return Failed("The GameObject has no stable Undo scope.");
    if (!scriptAsset.isComponent)
        return Failed("Only Behaviour scripts can be added as ScriptComponents.");

    auto ownerTarget = ObjectUndoTarget::Capture(gameObject, scope);
    if (!ownerTarget.has_value())
        return Failed("The ScriptComponent owner could not capture a stable Undo target.");
    auto info = BuildInfo("Add Script Component", std::move(scope), *ownerTarget, "components");
    info.estimatedBytes = sizeof(ObjectUndoTarget) + sizeof(::NLS::Scripting::ScriptAsset) +
        scriptAsset.sourcePath.size() + scriptAsset.sourceText.size();
    return undoService.Execute(std::make_unique<AddScriptComponentCommand>(
        std::move(info), std::move(*ownerTarget), scriptAsset));
}

Undo::ExecuteResult DeleteGameObject(
    Undo::EditorUndoService& undoService,
    Engine::GameObject& gameObject,
    Undo::UndoScopeId scope)
{
    if (!scope.IsValid())
        return Failed("The GameObject has no stable Undo scope.");
    if (gameObject.GetScene() == nullptr)
        return Failed("The GameObject is not owned by a Scene.");

    auto target = ObjectUndoTarget::Capture(gameObject, scope);
    auto snapshot = CaptureSubtree(gameObject, scope);
    if (!target.has_value() || !snapshot.has_value())
        return Failed("The GameObject subtree could not be captured for Undo.");
    auto location = CaptureDeletionLocation(gameObject, scope);
    if (!location.has_value() || !location->scene.IsValid() ||
        (location->parent.has_value() && location->siblingIndex == kNoSiblingIndex))
    {
        return Failed("The GameObject hierarchy location could not be captured for Undo.");
    }

    auto info = BuildInfo("Delete GameObject", std::move(scope), *target);
    info.estimatedBytes = EstimateSnapshotBytes(*snapshot) +
        sizeof(SubtreeLocation) +
        (location->beforeParentChildren.size() + location->afterParentChildren.size()) * sizeof(ObjectUndoTarget);
    return undoService.Execute(std::make_unique<SubtreeUndoCommand>(
        std::move(info),
        SubtreeCommandKind::Delete,
        std::move(*location),
        CaptureSelection(target->GetScope()),
        std::move(snapshot)));
}

Undo::ExecuteResult DuplicateGameObject(
    Undo::EditorUndoService& undoService,
    Engine::GameObject& gameObject,
    Engine::GameObject* forcedParent,
    Undo::UndoScopeId scope)
{
    if (!scope.IsValid())
        return Failed("The GameObject has no stable Undo scope.");
    auto* scene = gameObject.GetScene();
    if (scene == nullptr)
        return Failed("The GameObject is not owned by a Scene.");
    auto* parent = forcedParent != nullptr ? forcedParent : gameObject.GetParent();
    if (parent != nullptr && parent->GetScene() != scene)
        return Failed("The duplicate destination parent belongs to another Scene.");

    auto sourceTarget = ObjectUndoTarget::Capture(gameObject, scope);
    auto sourceSnapshot = CaptureSubtree(gameObject, scope);
    if (!sourceTarget.has_value() || !sourceSnapshot.has_value())
        return Failed("The GameObject subtree could not be captured for duplication.");
    auto location = CaptureCreationLocation(*scene, parent, scope);
    if (!location.has_value())
        return Failed("The duplicate hierarchy location could not be captured for Undo.");
    if (parent != nullptr)
        location->siblingIndex = location->beforeParentChildren.size();

    auto selectionBefore = CaptureSelection(scope);
    auto info = BuildInfo("Duplicate GameObject", std::move(scope), *sourceTarget);
    info.estimatedBytes = EstimateSnapshotBytes(*sourceSnapshot) + sizeof(SubtreeLocation) +
        location->beforeParentChildren.size() * sizeof(ObjectUndoTarget);
    return undoService.Execute(std::make_unique<SubtreeUndoCommand>(
        std::move(info),
        SubtreeCommandKind::Duplicate,
        std::move(*location),
        std::move(selectionBefore),
        std::nullopt,
        std::move(sourceSnapshot),
        std::move(sourceTarget)));
}

Undo::ExecuteResult CreateGameObjectFromAsset(
    Undo::EditorUndoService& undoService,
    const Assets::EditorAssetDragPayload& payload,
    Engine::SceneSystem::Scene& scene,
    Engine::GameObject* parent,
    Undo::UndoScopeId scope,
    std::optional<Maths::Vector3> placementOverride)
{
    if (!scope.IsValid())
        return Failed("The asset drop has no stable Undo scope.");
    if (parent != nullptr && parent->GetScene() != &scene)
        return Failed("The asset drop target belongs to another Scene.");

    auto location = CaptureCreationLocation(scene, parent, scope);
    if (!location.has_value())
        return Failed("The asset drop hierarchy location could not be captured for Undo.");
    if (parent != nullptr)
        location->siblingIndex = location->beforeParentChildren.size();
    auto selectionBefore = CaptureSelection(scope);
    auto info = BuildInfo("Create GameObject From Asset", std::move(scope), location->scene, "asset-drop");
    info.estimatedBytes = sizeof(Assets::EditorAssetDragPayload) + sizeof(SubtreeLocation) +
        location->beforeParentChildren.size() * sizeof(ObjectUndoTarget) +
        (placementOverride.has_value() ? sizeof(Maths::Vector3) : 0u);
    return undoService.Execute(std::make_unique<SubtreeUndoCommand>(
        std::move(info),
        SubtreeCommandKind::AssetPayload,
        std::move(*location),
        std::move(selectionBefore),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        payload,
        std::string {},
        std::move(placementOverride)));
}

Undo::ExecuteResult CreateGameObjectFromAsset(
    Undo::EditorUndoService& undoService,
    std::string resourcePath,
    Engine::SceneSystem::Scene& scene,
    Engine::GameObject* parent,
    Undo::UndoScopeId scope,
    std::optional<Maths::Vector3> placementOverride)
{
    if (!scope.IsValid())
        return Failed("The asset drop has no stable Undo scope.");
    if (resourcePath.empty())
        return Failed("The asset drop path is empty.");
    if (parent != nullptr && parent->GetScene() != &scene)
        return Failed("The asset drop target belongs to another Scene.");

    auto location = CaptureCreationLocation(scene, parent, scope);
    if (!location.has_value())
        return Failed("The asset drop hierarchy location could not be captured for Undo.");
    if (parent != nullptr)
        location->siblingIndex = location->beforeParentChildren.size();
    auto selectionBefore = CaptureSelection(scope);
    auto info = BuildInfo("Create GameObject From Asset", std::move(scope), location->scene, "asset-drop");
    info.estimatedBytes = sizeof(SubtreeLocation) + resourcePath.size() +
        location->beforeParentChildren.size() * sizeof(ObjectUndoTarget) +
        (placementOverride.has_value() ? sizeof(Maths::Vector3) : 0u);
    return undoService.Execute(std::make_unique<SubtreeUndoCommand>(
        std::move(info),
        SubtreeCommandKind::AssetPath,
        std::move(*location),
        std::move(selectionBefore),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        Assets::EditorAssetDragPayload {},
        std::move(resourcePath),
        std::move(placementOverride)));
}

Undo::ExecuteResult CreateEmptyGameObject(
    Undo::EditorUndoService& undoService,
    Engine::SceneSystem::Scene& scene,
    Engine::GameObject* parent,
    Undo::UndoScopeId scope,
    std::string name)
{
    if (!scope.IsValid())
        return Failed("The GameObject creation has no stable Undo scope.");
    if (parent != nullptr && parent->GetScene() != &scene)
        return Failed("The creation parent belongs to another Scene.");

    auto location = CaptureCreationLocation(scene, parent, scope);
    if (!location.has_value())
        return Failed("The GameObject creation location could not be captured for Undo.");
    if (parent != nullptr)
        location->siblingIndex = location->beforeParentChildren.size();
    auto selectionBefore = CaptureSelection(scope);
    auto info = BuildInfo("Create Empty GameObject", std::move(scope), location->scene, "create");
    info.estimatedBytes = sizeof(SubtreeLocation) + name.size() +
        location->beforeParentChildren.size() * sizeof(ObjectUndoTarget);
    return undoService.Execute(std::make_unique<SubtreeUndoCommand>(
        std::move(info),
        SubtreeCommandKind::Empty,
        std::move(*location),
        std::move(selectionBefore),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        Assets::EditorAssetDragPayload {},
        std::string {},
        std::nullopt,
        std::nullopt,
        std::string {},
        std::move(name)));
}

Undo::ExecuteResult CreatePrimitiveGameObject(
    Undo::EditorUndoService& undoService,
    Engine::SceneSystem::Scene& scene,
    Engine::GameObject* parent,
    const Engine::PrimitiveType primitiveType,
    Undo::UndoScopeId scope)
{
    if (!scope.IsValid())
        return Failed("The primitive creation has no stable Undo scope.");
    if (parent != nullptr && parent->GetScene() != &scene)
        return Failed("The creation parent belongs to another Scene.");

    auto location = CaptureCreationLocation(scene, parent, scope);
    if (!location.has_value())
        return Failed("The primitive creation location could not be captured for Undo.");
    if (parent != nullptr)
        location->siblingIndex = location->beforeParentChildren.size();
    auto selectionBefore = CaptureSelection(scope);
    auto info = BuildInfo(
        std::string("Create ") + Engine::GetPrimitiveName(primitiveType),
        std::move(scope),
        location->scene,
        "create");
    info.estimatedBytes = sizeof(SubtreeLocation) + sizeof(primitiveType) +
        location->beforeParentChildren.size() * sizeof(ObjectUndoTarget);
    return undoService.Execute(std::make_unique<SubtreeUndoCommand>(
        std::move(info),
        SubtreeCommandKind::Primitive,
        std::move(*location),
        std::move(selectionBefore),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        Assets::EditorAssetDragPayload {},
        std::string {},
        std::nullopt,
        primitiveType));
}

Undo::ExecuteResult CreateGameObjectWithComponent(
    Undo::EditorUndoService& undoService,
    Engine::SceneSystem::Scene& scene,
    Engine::GameObject* parent,
    std::string componentTypeName,
    Undo::UndoScopeId scope)
{
    if (!scope.IsValid())
        return Failed("The component GameObject creation has no stable Undo scope.");
    if (parent != nullptr && parent->GetScene() != &scene)
        return Failed("The creation parent belongs to another Scene.");
    const auto componentType = meta::Type::GetFromName(componentTypeName);
    if (!componentType.IsValid() || !componentType.DerivesFrom(NLS_TYPEOF(Component)))
        return Failed("The requested component type is unavailable.");

    auto location = CaptureCreationLocation(scene, parent, scope);
    if (!location.has_value())
        return Failed("The component GameObject creation location could not be captured for Undo.");
    if (parent != nullptr)
        location->siblingIndex = location->beforeParentChildren.size();
    auto selectionBefore = CaptureSelection(scope);
    auto info = BuildInfo(
        "Create " + componentType.GetName(),
        std::move(scope),
        location->scene,
        "create");
    info.estimatedBytes = sizeof(SubtreeLocation) + componentTypeName.size() +
        location->beforeParentChildren.size() * sizeof(ObjectUndoTarget);
    return undoService.Execute(std::make_unique<SubtreeUndoCommand>(
        std::move(info),
        SubtreeCommandKind::Component,
        std::move(*location),
        std::move(selectionBefore),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        Assets::EditorAssetDragPayload {},
        std::string {},
        std::nullopt,
        std::nullopt,
        std::move(componentTypeName)));
}
} // namespace NLS::Editor::Panels::HierarchyUndo

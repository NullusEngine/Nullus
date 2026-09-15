#include "Inspector/ComponentStructureUndo.h"
#include "Undo/EditorUndoMutationContext.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <Components/Component.h>
#include <Components/TransformComponent.h>
#include <GameObject.h>
#include <Guid.h>
#include <Object/Object.h>
#include <Reflection/Type.h>
#include <Serialize/ObjectGraphInstantiator.h>
#include <Serialize/ObjectGraphSerializer.h>
#include <Serialize/ObjectGraphWriter.h>
#include <ServiceLocator.h>

#include "Core/EditorActions.h"
#include "Panels/ComponentSearchPanel.h"

namespace NLS::Editor::Inspector
{
namespace
{
using ObjectIdentifier = Engine::Serialize::ObjectIdentifier;

struct ObjectIdentifierHash
{
    size_t operator()(const ObjectIdentifier& identifier) const noexcept
    {
        size_t hash = std::hash<int64_t> {}(identifier.localIdentifierInFile);
        hash ^= std::hash<NLS::Guid> {}(identifier.guid) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        hash ^= std::hash<int32_t> {}(static_cast<int32_t>(identifier.fileType)) +
            0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        return hash;
    }
};

std::string MakeSnapshotIdentityKey(const ObjectIdentifier& identifier)
{
    return identifier.guid.ToString() + ":" +
        std::to_string(identifier.localIdentifierInFile) + ":" +
        std::to_string(static_cast<int32_t>(identifier.fileType));
}

Engine::Serialize::ObjectId MakeSnapshotObjectId(
    const Engine::Components::Component& component,
    const ObjectIdentifier& identifier,
    const size_t index)
{
    const auto type = component.GetType();
    const std::string typeName = type.IsValid() ? type.GetName() : component.GetObjectTypeName();
    return Engine::Serialize::ObjectId(Guid::NewDeterministic(
        "Inspector.ComponentSnapshot:" + typeName + ":" +
        MakeSnapshotIdentityKey(identifier) + ":" + std::to_string(index)));
}

Engine::Serialize::LoadPolicy MakeSnapshotLoadPolicy()
{
    Engine::Serialize::LoadPolicy policy;
    policy.missingAssetPolicy = Engine::Serialize::MissingAssetPolicy::Preserve;
    policy.invalidReferencePolicy = Engine::Serialize::InvalidReferencePolicy::Preserve;
    policy.deferAssetReferenceResolution = true;
    policy.skipDeferredAssetReferenceCacheLookup = true;
    return policy;
}

bool IsCompatibleComponentRecord(
    const Engine::Components::Component& component,
    const Engine::Serialize::ObjectRecord& record)
{
    const auto type = component.GetType();
    return type.IsValid() && !record.typeName.empty() && type.GetName() == record.typeName;
}

std::optional<Undo::UndoScopeId> FindComponentStructureOwnerScope(const Undo::UndoCommandInfo& info)
{
    const auto found = std::find_if(
        info.scopes.begin(),
        info.scopes.end(),
        [](const Undo::UndoScopeId& scope)
        {
            return scope.IsValid() &&
                (scope.kind == Undo::UndoScopeKind::Scene ||
                    scope.kind == Undo::UndoScopeKind::PrefabStage);
        });
    return found != info.scopes.end() ? std::optional<Undo::UndoScopeId>(*found) : std::nullopt;
}

Engine::GameObject* ResolveStructureOwner(const Undo::ObjectUndoTarget& target)
{
    return target.ResolveAs<Engine::GameObject>();
}

void MarkComponentStructureOwnerDirty(Engine::GameObject& owner)
{
    if (NLS::Core::ServiceLocator::Contains<Core::EditorActions>())
        NLS::Core::ServiceLocator::Get<Core::EditorActions>().MarkOwningSceneDirty(owner);
}

Engine::Components::Component* ResolveOwnerComponent(
    Engine::GameObject& gameObject,
    const ObjectIdentifier& componentIdentifier)
{
    if (!componentIdentifier.IsValid())
        return nullptr;

    InstanceID instanceId = InstanceID_None;
    auto& persistentManager = Engine::Serialize::PersistentManager::Instance();
    if (!persistentManager.TryGetInstanceID(componentIdentifier, instanceId))
        return nullptr;

    auto* component = dynamic_cast<Engine::Components::Component*>(NLS::Object::IDToPointer(instanceId));
    return component != nullptr && component->gameobject() == &gameObject
        ? component
        : nullptr;
}

bool ApplyStructureEditOperation(
    Engine::GameObject& owner,
    const ComponentStructureEditOperation& operation)
{
    switch (operation.kind)
    {
    case ComponentStructureEditKind::Add:
    {
        const auto type = meta::Type::GetFromName(operation.componentTypeName);
        if (!type.IsValid())
            return false;

        Panels::ComponentSearchEntry entry;
        entry.componentType = type;
        entry.scriptAsset = operation.scriptAsset;
        entry.isAddable = true;
        return Panels::ComponentSearchPanel::TryAddComponentFromEntry(&owner, entry);
    }
    case ComponentStructureEditKind::Reset:
    {
        auto* component = ResolveOwnerComponent(owner, operation.componentIdentifier);
        if (component == nullptr)
            return false;
        component->Reset();
        return true;
    }
    case ComponentStructureEditKind::PasteValues:
    {
        auto* component = ResolveOwnerComponent(owner, operation.componentIdentifier);
        return component != nullptr && ApplyComponentSnapshot(*component, operation.pastedValues);
    }
    case ComponentStructureEditKind::Move:
    {
        auto* component = ResolveOwnerComponent(owner, operation.componentIdentifier);
        return component != nullptr && component != owner.GetTransform() && operation.destinationIndex > 0u &&
            operation.destinationIndex < owner.GetComponents().size() &&
            owner.MoveComponent(component, operation.destinationIndex);
    }
    case ComponentStructureEditKind::Remove:
    {
        auto* component = ResolveOwnerComponent(owner, operation.componentIdentifier);
        return component != nullptr && component != owner.GetTransform() && owner.RemoveComponent(component);
    }
    }
    return false;
}

struct RestoreResult
{
    bool succeeded = false;
    bool rollbackFailed = false;
};

using ComponentsByObjectIdentifier = std::unordered_map<
    ObjectIdentifier,
    Engine::Components::Component*,
    ObjectIdentifierHash>;

bool BuildComponentsByObjectIdentifier(
    const Engine::GameObject& gameObject,
    ComponentsByObjectIdentifier& componentsByObjectIdentifier)
{
    componentsByObjectIdentifier.clear();
    for (const auto& component : gameObject.GetComponents())
    {
        const auto identifier = component != nullptr
            ? CaptureComponentObjectIdentifier(*component)
            : std::nullopt;
        if (!identifier.has_value())
            return false;
        if (!componentsByObjectIdentifier.emplace(*identifier, component.get()).second)
            return false;
    }
    return true;
}

bool ValidateComponentStructureSnapshot(
    const Engine::GameObject& gameObject,
    const ComponentStructureSnapshot& snapshot)
{
    const auto transformType = NLS_TYPEOF(Engine::Components::TransformComponent);
    const auto* transform = gameObject.GetTransform();
    const auto transformIdentifier = transform != nullptr
        ? CaptureComponentObjectIdentifier(*transform)
        : std::nullopt;
    if (snapshot.components.empty() || transform == nullptr ||
        !transformIdentifier.has_value() ||
        snapshot.components.front().originalIndex != 0u ||
        snapshot.components.front().objectIdentifier != *transformIdentifier ||
        snapshot.components.front().record.typeName != transformType.GetName())
    {
        return false;
    }

    ComponentsByObjectIdentifier currentComponents;
    if (!BuildComponentsByObjectIdentifier(gameObject, currentComponents))
        return false;
    const auto transformEntry = currentComponents.find(*transformIdentifier);
    if (transformEntry == currentComponents.end() || transformEntry->second != transform)
        return false;

    std::unordered_set<ObjectIdentifier, ObjectIdentifierHash> snapshotIdentifiers;
    snapshotIdentifiers.reserve(snapshot.components.size());
    auto& persistentManager = Engine::Serialize::PersistentManager::Instance();
    for (size_t index = 0u; index < snapshot.components.size(); ++index)
    {
        const auto& componentSnapshot = snapshot.components[index];
        if (componentSnapshot.originalIndex != index || !componentSnapshot.objectIdentifier.IsValid() ||
            !snapshotIdentifiers.insert(componentSnapshot.objectIdentifier).second)
        {
            return false;
        }

        const auto type = meta::Type::GetFromName(componentSnapshot.record.typeName);
        if (!type.IsValid() || !type.DerivesFrom(NLS_TYPEOF(Engine::Components::Component)))
            return false;
        if (index == 0u && type != transformType)
            return false;
    }

    for (size_t index = 1u; index < snapshot.components.size(); ++index)
    {
        const auto& componentSnapshot = snapshot.components[index];
        const auto current = currentComponents.find(componentSnapshot.objectIdentifier);
        if (current != currentComponents.end())
        {
            if (!IsCompatibleComponentRecord(*current->second, componentSnapshot.record))
                return false;
        }
        else
        {
            InstanceID instanceId = InstanceID_None;
            if (!persistentManager.TryGetInstanceID(componentSnapshot.objectIdentifier, instanceId) ||
                NLS::Object::IDToPointer(instanceId) != nullptr)
            {
                // Do not claim an identifier whose runtime slot was discarded
                // or is now bound to an unrelated live object.
                return false;
            }
        }
    }

    return true;
}

bool ApplyComponentStructureDelta(
    Engine::GameObject& gameObject,
    const ComponentStructureSnapshot& snapshot,
    bool& mutationStarted)
{
    NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT("changing a GameObject component structure");
    auto* transform = gameObject.GetTransform();
    if (transform == nullptr)
        return false;

    ComponentsByObjectIdentifier currentComponents;
    if (!BuildComponentsByObjectIdentifier(gameObject, currentComponents))
        return false;
    auto& persistentManager = Engine::Serialize::PersistentManager::Instance();

    mutationStarted = true;
    if (!ApplyComponentSnapshot(*transform, snapshot.components.front()))
        return false;

    ComponentsByObjectIdentifier desiredComponents;
    desiredComponents.emplace(snapshot.components.front().objectIdentifier, transform);
    for (size_t index = 1u; index < snapshot.components.size(); ++index)
    {
        const auto& componentSnapshot = snapshot.components[index];
        const auto current = currentComponents.find(componentSnapshot.objectIdentifier);
        if (current != currentComponents.end())
        {
            mutationStarted = true;
            if (!ApplyComponentSnapshot(*current->second, componentSnapshot))
                return false;
            desiredComponents.emplace(componentSnapshot.objectIdentifier, current->second);
            continue;
        }

        if (!persistentManager.ReserveObjectIdentifier(componentSnapshot.objectIdentifier))
            return false;

        const auto type = meta::Type::GetFromName(componentSnapshot.record.typeName);
        auto* component = gameObject.AddComponent(type);
        if (component == nullptr)
        {
            persistentManager.ReleaseReservedObjectIdentifier(componentSnapshot.objectIdentifier);
            return false;
        }

        mutationStarted = true;
        if (persistentManager.BindObjectIdentifier(*component, componentSnapshot.objectIdentifier) == InstanceID_None)
        {
            if (gameObject.RemoveComponent(component))
                persistentManager.ReleaseReservedObjectIdentifier(componentSnapshot.objectIdentifier);
            return false;
        }
        if (!ApplyComponentSnapshot(*component, componentSnapshot))
            return false;
        desiredComponents.emplace(componentSnapshot.objectIdentifier, component);
    }

    // Property state is applied before removing surplus components so a failed
    // snapshot leaves enough live state for the outer rollback to restore.
    std::vector<Engine::Components::Component*> componentsToRemove;
    for (const auto& [identifier, component] : currentComponents)
    {
        if (component != nullptr && component != transform &&
            !desiredComponents.contains(identifier))
        {
            componentsToRemove.push_back(component);
        }
    }
    for (auto* component : componentsToRemove)
    {
        mutationStarted = true;
        if (!gameObject.RemoveComponent(component))
            return false;
    }

    const auto& components = gameObject.GetComponents();
    if (components.empty())
        return false;
    if (components.front().get() != transform)
    {
        mutationStarted = true;
        if (!gameObject.MoveComponent(transform, 0u))
            return false;
    }

    for (size_t index = 1u; index < snapshot.components.size(); ++index)
    {
        const auto desired = desiredComponents.find(snapshot.components[index].objectIdentifier);
        if (desired == desiredComponents.end())
            return false;

        const auto& orderedComponents = gameObject.GetComponents();
        if (index >= orderedComponents.size())
            return false;
        if (orderedComponents[index].get() == desired->second)
            continue;

        mutationStarted = true;
        if (!gameObject.MoveComponent(desired->second, index))
            return false;
    }
    return true;
}

RestoreResult RestoreComponentStructureWithRollback(
    Engine::GameObject& gameObject,
    const ComponentStructureSnapshot& snapshot)
{
    if (!ValidateComponentStructureSnapshot(gameObject, snapshot))
        return {};

    const auto rollbackSnapshot = CaptureComponentStructure(gameObject);
    bool mutationStarted = false;
    if (ApplyComponentStructureDelta(gameObject, snapshot, mutationStarted))
        return {true, false};
    if (!mutationStarted)
        return {};

    bool rollbackMutationStarted = false;
    const bool rollbackSucceeded =
        ValidateComponentStructureSnapshot(gameObject, rollbackSnapshot) &&
        ApplyComponentStructureDelta(gameObject, rollbackSnapshot, rollbackMutationStarted);
    return {false, !rollbackSucceeded};
}

class ComponentStructureUndoCommand final : public Undo::IEditorUndoCommand
{
public:
    ComponentStructureUndoCommand(
        Undo::UndoCommandInfo info,
        Undo::ObjectUndoTarget owner,
        ComponentStructureSnapshot before,
        ComponentStructureSnapshot after)
        : m_info(std::move(info))
        , m_owner(std::move(owner))
        , m_before(std::move(before))
        , m_after(std::move(after))
    {
    }

    [[nodiscard]] Undo::UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    [[nodiscard]] Undo::UndoValidationResult Validate(
        Undo::UndoDirection,
        const Undo::UndoExecutionContext&) const override
    {
        return ResolveStructureOwner(m_owner) != nullptr
            ? Undo::UndoValidationResult::Ready()
            : Undo::UndoValidationResult::Stale("The component owner no longer exists.");
    }

    Undo::UndoStartResult Apply(
        const Undo::UndoDirection direction,
        const Undo::UndoExecutionContext&,
        Undo::UndoCompletion) override
    {
        auto* owner = ResolveStructureOwner(m_owner);
        if (owner == nullptr)
            return Undo::UndoStartResult::Failed("The component owner no longer exists.");

        const auto& snapshot = direction == Undo::UndoDirection::Undo ? m_before : m_after;
        const auto result = RestoreComponentStructureWithRollback(*owner, snapshot);
        if (result.succeeded)
        {
            MarkComponentStructureOwnerDirty(*owner);
            return Undo::UndoStartResult::Completed();
        }
        return Undo::UndoStartResult::Failed(
            "The component structure could not be restored.",
            result.rollbackFailed);
    }

    bool TryMergeFrom(const Undo::IEditorUndoCommand&) override
    {
        return false;
    }

private:
    Undo::UndoCommandInfo m_info;
    Undo::ObjectUndoTarget m_owner;
    ComponentStructureSnapshot m_before;
    ComponentStructureSnapshot m_after;
};

class ComponentStructureUndoEdit final : public Undo::IEditorUndoEdit
{
public:
    ComponentStructureUndoEdit(
        Undo::UndoCommandInfo info,
        Undo::ObjectUndoTarget owner,
        ComponentStructureSnapshot before,
        ComponentStructureEditOperation operation)
        : m_info(std::move(info))
        , m_owner(std::move(owner))
        , m_before(std::move(before))
        , m_operation(std::move(operation))
    {
    }

    [[nodiscard]] Undo::UndoValidationResult Validate(const Undo::UndoExecutionContext&) const override
    {
        auto* owner = ResolveStructureOwner(m_owner);
        if (owner == nullptr)
            return Undo::UndoValidationResult::Stale("The component owner no longer exists.");

        return AreComponentStructureSnapshotsEqual(CaptureComponentStructure(*owner), m_before)
            ? Undo::UndoValidationResult::Ready()
            : Undo::UndoValidationResult::Conflict(
                "The component list changed before this Inspector action could be applied.");
    }

    Undo::UndoStartResult Preview(
        const Undo::UndoExecutionContext&,
        Undo::UndoCompletion) override
    {
        auto* owner = ResolveStructureOwner(m_owner);
        if (owner == nullptr)
            return Undo::UndoStartResult::Failed("The component owner no longer exists.");
        if (!AreComponentStructureSnapshotsEqual(CaptureComponentStructure(*owner), m_before))
        {
            return Undo::UndoStartResult::Failed(
                "The component list changed before this Inspector action could be applied.");
        }

        if (!ApplyStructureEditOperation(*owner, m_operation))
        {
            if (AreComponentStructureSnapshotsEqual(CaptureComponentStructure(*owner), m_before))
                return Undo::UndoStartResult::Failed("The component operation could not be applied.");

            const auto rollback = RestoreComponentStructureWithRollback(*owner, m_before);
            return Undo::UndoStartResult::Failed(
                "The component operation failed and its partial changes could not be restored.",
                !rollback.succeeded || rollback.rollbackFailed);
        }

        m_after = CaptureComponentStructure(*owner);
        if (!AreComponentStructureSnapshotsEqual(m_before, *m_after))
            MarkComponentStructureOwnerDirty(*owner);
        return Undo::UndoStartResult::Completed();
    }

    Undo::UndoStartResult Cancel(
        const Undo::UndoExecutionContext&,
        Undo::UndoCompletion) override
    {
        auto* owner = ResolveStructureOwner(m_owner);
        if (owner == nullptr)
            return Undo::UndoStartResult::Failed("The component owner no longer exists.");
        if (AreComponentStructureSnapshotsEqual(CaptureComponentStructure(*owner), m_before))
            return Undo::UndoStartResult::Completed();

        const auto rollback = RestoreComponentStructureWithRollback(*owner, m_before);
        return rollback.succeeded
            ? Undo::UndoStartResult::Completed()
            : Undo::UndoStartResult::Failed(
                "The component operation could not be cancelled.",
                rollback.rollbackFailed);
    }

    [[nodiscard]] bool HasChanges() const override
    {
        return m_after.has_value() && !AreComponentStructureSnapshotsEqual(m_before, *m_after);
    }

    [[nodiscard]] Undo::UndoCommandPtr BuildCommand() override
    {
        if (!HasChanges())
            return {};

        auto info = m_info;
        info.estimatedBytes = sizeof(ComponentStructureSnapshot) +
            (m_before.components.size() + m_after->components.size()) * sizeof(ComponentSnapshot);
        return MakeComponentStructureUndoCommand(
            std::move(info), m_owner, m_before, std::move(*m_after));
    }

private:
    Undo::UndoCommandInfo m_info;
    Undo::ObjectUndoTarget m_owner;
    ComponentStructureSnapshot m_before;
    ComponentStructureEditOperation m_operation;
    std::optional<ComponentStructureSnapshot> m_after;
};
} // namespace

std::optional<Engine::Serialize::ObjectIdentifier> CaptureComponentObjectIdentifier(
    const Engine::Components::Component& component)
{
    if (component.GetInstanceID() == InstanceID_None)
        return std::nullopt;

    auto& persistentManager = Engine::Serialize::PersistentManager::Instance();
    ObjectIdentifier identifier;
    if (persistentManager.InstanceIDToObjectIdentifier(component.GetInstanceID(), identifier))
        return identifier.IsValid() ? std::optional<ObjectIdentifier>(std::move(identifier)) : std::nullopt;

    identifier = persistentManager.GetOrCreateLocalObjectIdentifier(
        const_cast<Engine::Components::Component&>(component));
    return identifier.IsValid() ? std::optional<ObjectIdentifier>(std::move(identifier)) : std::nullopt;
}

std::optional<Undo::ObjectUndoTarget> CaptureComponentStructureOwner(
    Engine::GameObject& gameObject,
    const Undo::UndoCommandInfo& info)
{
    const auto scope = FindComponentStructureOwnerScope(info);
    if (!scope.has_value())
        return std::nullopt;
    return Undo::ObjectUndoTarget::Capture(gameObject, *scope);
}

ComponentSnapshot CaptureComponentSnapshot(
    const Engine::Components::Component& component,
    const size_t originalIndex)
{
    ComponentSnapshot snapshot;
    snapshot.originalIndex = originalIndex;
    if (const auto identifier = CaptureComponentObjectIdentifier(component); identifier.has_value())
        snapshot.objectIdentifier = *identifier;
    snapshot.record = Engine::Serialize::ObjectGraphSerializer::SerializeObjectRecord(
        component,
        MakeSnapshotObjectId(component, snapshot.objectIdentifier, originalIndex));
    return snapshot;
}

ComponentStructureSnapshot CaptureComponentStructure(const Engine::GameObject& gameObject)
{
    ComponentStructureSnapshot snapshot;
    const auto& components = gameObject.GetComponents();
    snapshot.components.reserve(components.size());
    for (size_t index = 0u; index < components.size(); ++index)
    {
        if (components[index] != nullptr)
            snapshot.components.push_back(CaptureComponentSnapshot(*components[index], snapshot.components.size()));
    }
    return snapshot;
}

bool ApplyComponentSnapshot(
    Engine::Components::Component& component,
    const ComponentSnapshot& snapshot)
{
    if (!IsCompatibleComponentRecord(component, snapshot.record))
        return false;
    return Engine::Serialize::ObjectGraphInstantiator::ApplyComponentSnapshot(
        component,
        snapshot.record,
        MakeSnapshotLoadPolicy());
}

bool RestoreComponentStructure(
    Engine::GameObject& gameObject,
    const ComponentStructureSnapshot& snapshot)
{
    return RestoreComponentStructureWithRollback(gameObject, snapshot).succeeded;
}

bool AreComponentStructureSnapshotsEqual(
    const ComponentStructureSnapshot& left,
    const ComponentStructureSnapshot& right)
{
    if (left.components.size() != right.components.size())
        return false;

    Engine::Serialize::ObjectGraphDocument leftDocument;
    Engine::Serialize::ObjectGraphDocument rightDocument;
    const auto documentId = Guid::NewDeterministic("Inspector.ComponentStructureSnapshot");
    leftDocument.documentId = documentId;
    rightDocument.documentId = documentId;
    for (size_t index = 0u; index < left.components.size(); ++index)
    {
        const auto& leftComponent = left.components[index];
        const auto& rightComponent = right.components[index];
        if (leftComponent.originalIndex != rightComponent.originalIndex ||
            leftComponent.objectIdentifier != rightComponent.objectIdentifier)
        {
            return false;
        }
        leftDocument.objects.push_back(leftComponent.record);
        rightDocument.objects.push_back(rightComponent.record);
    }

    if (!left.components.empty())
    {
        leftDocument.root = left.components.front().record.id;
        rightDocument.root = right.components.front().record.id;
    }
    return Engine::Serialize::ObjectGraphWriter::WriteCompact(leftDocument) ==
        Engine::Serialize::ObjectGraphWriter::WriteCompact(rightDocument);
}

Undo::UndoCommandPtr MakeComponentStructureUndoCommand(
    Undo::UndoCommandInfo info,
    Undo::ObjectUndoTarget owner,
    ComponentStructureSnapshot before,
    ComponentStructureSnapshot after)
{
    return std::make_unique<ComponentStructureUndoCommand>(
        std::move(info),
        std::move(owner),
        std::move(before),
        std::move(after));
}

Undo::UndoEditPtr MakeComponentStructureUndoEdit(
    Undo::UndoCommandInfo info,
    Undo::ObjectUndoTarget owner,
    ComponentStructureSnapshot before,
    ComponentStructureEditOperation operation)
{
    return std::make_unique<ComponentStructureUndoEdit>(
        std::move(info),
        std::move(owner),
        std::move(before),
        std::move(operation));
}

Undo::UndoRequestResult ExecuteComponentStructureEdit(
    Undo::EditorUndoService& undo,
    Undo::UndoCommandInfo info,
    Undo::ObjectUndoTarget owner,
    ComponentStructureSnapshot before,
    ComponentStructureEditOperation operation)
{
    auto commandInfo = info;
    auto begun = undo.BeginEditSession({
        std::move(info),
        MakeComponentStructureUndoEdit(
            std::move(commandInfo),
            std::move(owner),
            std::move(before),
            std::move(operation))});
    if (!begun.result.Succeeded() || !begun.session.IsValid())
        return begun.result;

    auto session = begun.session;
    const auto previewed = undo.PreviewEditSession(session);
    if (!previewed.Succeeded())
    {
        if (session.IsValid())
            (void)undo.CancelEditSession(session);
        return previewed;
    }

    const auto committed = undo.CommitEditSession(session);
    if (!committed.Succeeded())
    {
        if (session.IsValid())
            (void)undo.CancelEditSession(session);
        return committed;
    }
    return committed;
}
} // namespace NLS::Editor::Inspector

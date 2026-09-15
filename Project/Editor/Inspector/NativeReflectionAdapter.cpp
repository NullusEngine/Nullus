#include "Inspector/NativeReflectionAdapter.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iterator>

#include <Components/Component.h>
#include <Components/TransformComponent.h>
#include <GameObject.h>
#include <Reflection/Field.h>
#include <Scripting/ScriptComponent.h>

#include "Inspector/PropertyTypeTraits.h"
#include "Inspector/PrefabInspectorSupport.h"
#include "Inspector/ScriptSerializedAdapter.h"

namespace NLS::Editor::Inspector
{
namespace
{
uint64_t HashCombine(uint64_t seed, const uint64_t value)
{
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U));
}

uint64_t HashString(const std::string& value)
{
    return static_cast<uint64_t>(std::hash<std::string>{}(value));
}

bool HaveMatchingType(const meta::Type& left, const meta::Type& right)
{
    return left.IsValid() && right.IsValid() &&
        (left == right || left.GetName() == right.GetName());
}

std::vector<Engine::Components::Component*> GetComponentsInOrder(Engine::GameObject& gameObject)
{
    std::vector<Engine::Components::Component*> components;
    components.reserve(gameObject.GetComponents().size());
    for (const auto& component : gameObject.GetComponents())
    {
        if (component != nullptr)
            components.push_back(component.get());
    }
    return components;
}

size_t GetComponentTypeOccurrence(
    const std::vector<Engine::Components::Component*>& components,
    const size_t index)
{
    if (index >= components.size() || components[index] == nullptr)
        return 0u;

    size_t occurrence = 0u;
    for (size_t candidateIndex = 0u; candidateIndex < index; ++candidateIndex)
    {
        const auto* candidate = components[candidateIndex];
        if (candidate != nullptr && HaveMatchingType(candidate->GetType(), components[index]->GetType()))
            ++occurrence;
    }
    return occurrence;
}

Engine::Components::Component* FindComponentByTypeOccurrence(
    Engine::GameObject& gameObject,
    const meta::Type& type,
    const size_t occurrence)
{
    size_t matchedOccurrence = 0u;
    for (const auto& component : gameObject.GetComponents())
    {
        if (component == nullptr || !HaveMatchingType(component->GetType(), type))
            continue;
        if (matchedOccurrence == occurrence)
            return component.get();
        ++matchedOccurrence;
    }
    return nullptr;
}
} // namespace

bool NativeReflectionAdapter::CanBuild(const InspectorTargetSet& targets) const
{
    if (targets.Empty())
        return false;

    return std::all_of(targets.targets.begin(), targets.targets.end(), [](const InspectorTargetHandle& target)
    {
        return target.IsObjectTarget() && ResolveObject(target) != nullptr;
    });
}

uint64_t NativeReflectionAdapter::GetStructureVersion(const InspectorTargetSet& targets) const
{
    uint64_t version = 0xcbf29ce484222325ULL;
    for (const auto& handle : targets.targets)
    {
        const auto* object = ResolveObject(handle);
        if (object == nullptr)
            return 0u;

        if (handle.objectIdentifier.IsValid())
        {
            version = HashCombine(version, static_cast<uint64_t>(handle.objectIdentifier.localIdentifierInFile));
            version = HashCombine(
                version,
                static_cast<uint64_t>(static_cast<int32_t>(handle.objectIdentifier.fileType)));
            version = HashCombine(version, HashString(handle.objectIdentifier.guid.ToString()));
        }
        else
        {
            version = HashCombine(version, static_cast<uint64_t>(handle.instanceId));
        }
        version = HashCombine(version, HashString(object->GetType().GetName()));

        if (const auto* scriptComponent = dynamic_cast<const NLS::Scripting::ScriptComponent*>(object))
        {
            version = HashCombine(
                version,
                ScriptSerializedAdapter::GetComponentStructureVersion(*scriptComponent));
        }

        const auto* gameObject = dynamic_cast<const Engine::GameObject*>(object);
        if (gameObject == nullptr)
            continue;

        for (const auto& component : gameObject->GetComponents())
        {
            if (!component)
                continue;
            version = HashCombine(version, static_cast<uint64_t>(component->GetInstanceID()));
            version = HashCombine(version, HashString(component->GetType().GetName()));
            if (const auto* scriptComponent = dynamic_cast<const NLS::Scripting::ScriptComponent*>(component.get()))
                version = HashCombine(
                    version,
                    ScriptSerializedAdapter::GetComponentStructureVersion(*scriptComponent));
        }
    }
    return version;
}

InspectorPresentation NativeReflectionAdapter::BuildPresentation(
    const InspectorTargetSet& targets,
    const InspectorBuildContext&) const
{
    InspectorPresentation presentation;
    presentation.targets = targets;
    presentation.targets.Normalize();
    presentation.structureVersion = GetStructureVersion(presentation.targets);
    if (targets.Empty())
        return presentation;

    const auto& primary = targets.targets.front();
    auto* object = ResolveObject(primary);
    if (object == nullptr)
        return presentation;

    if (auto* gameObject = dynamic_cast<Engine::GameObject*>(object))
    {
        PropertyNode gameObjectNode;
        gameObjectNode.path = PropertyPath("GameObject");
        gameObjectNode.displayName = targets.targets.size() == 1u
            ? gameObject->GetName()
            : std::to_string(targets.targets.size()) + " GameObjects";
        gameObjectNode.kind = InspectorPropertyKind::Struct;
        gameObjectNode.valueType = gameObject->GetType();
        gameObjectNode.target = primary;
        gameObjectNode.structuralVersion = presentation.structureVersion;
        SetPropertyNodeTargets(gameObjectNode, targets.targets);

        std::vector<Engine::GameObject*> selectedObjects;
        selectedObjects.reserve(targets.targets.size());
        for (const auto& handle : targets.targets)
        {
            auto* selected = dynamic_cast<Engine::GameObject*>(ResolveObject(handle));
            if (selected == nullptr)
                return presentation;
            selectedObjects.push_back(selected);
        }

        const auto primaryComponents = GetComponentsInOrder(*gameObject);
        for (size_t componentIndex = 0u; componentIndex < primaryComponents.size(); ++componentIndex)
        {
            auto* primaryComponent = primaryComponents[componentIndex];
            if (primaryComponent == nullptr)
                continue;

            const auto componentOccurrence = GetComponentTypeOccurrence(primaryComponents, componentIndex);
            const auto componentPath = gameObjectNode.path
                .Field("Components")
                .Field(primaryComponent->GetType().GetName())
                .Index(componentOccurrence);
            const bool isTransform = primaryComponent->GetType() == NLS_TYPEOF(Engine::Components::TransformComponent);
            std::vector<InspectorTargetHandle> componentTargets;
            componentTargets.reserve(selectedObjects.size());
            for (auto* selected : selectedObjects)
            {
                auto* matchedComponent = FindComponentByTypeOccurrence(
                    *selected,
                    primaryComponent->GetType(),
                    componentOccurrence);
                if (matchedComponent == nullptr)
                {
                    componentTargets.clear();
                    break;
                }

                componentTargets.push_back(InspectorTargetHandle::FromObject(
                    *matchedComponent,
                    InspectorTargetKind::NativeObject));
            }
            if (componentTargets.size() != selectedObjects.size())
                continue;

            PropertyNode componentNode;
            componentNode.path = componentPath;
            componentNode.fieldName = primaryComponent->GetType().GetName();
            componentNode.displayName = FormatInspectorDisplayName(componentNode.fieldName);
            componentNode.kind = InspectorPropertyKind::Struct;
            componentNode.valueType = primaryComponent->GetType();
            componentNode.target = componentTargets.front();
            componentNode.isComponent = true;
            componentNode.isTransform = isTransform;
            componentNode.structuralVersion = presentation.structureVersion;
            SetPropertyNodeTargets(componentNode, std::move(componentTargets));
            if (const auto* scriptComponent = dynamic_cast<const NLS::Scripting::ScriptComponent*>(primaryComponent))
                componentNode.displayName = ScriptSerializedAdapter::GetComponentDisplayName(*scriptComponent);
            gameObjectNode.children.push_back(std::move(componentNode));
        }

        presentation.roots.push_back(std::move(gameObjectNode));
        RefreshPrefabOverrideStates(presentation);
        return presentation;
    }

    PropertyNode root;
    root.path = PropertyPath(object->GetType().GetName());
    root.displayName = FormatInspectorDisplayName(object->GetType().GetName());
    root.kind = InspectorPropertyKind::Struct;
    root.valueType = object->GetType();
    root.target = primary;
    root.structuralVersion = presentation.structureVersion;
    SetPropertyNodeTargets(root, targets.targets);
    if (const auto* scriptComponent = dynamic_cast<const NLS::Scripting::ScriptComponent*>(object))
        root.displayName = ScriptSerializedAdapter::GetComponentDisplayName(*scriptComponent);
    presentation.roots.push_back(std::move(root));
    RefreshPrefabOverrideStates(presentation);
    return presentation;
}

Object* NativeReflectionAdapter::ResolveObject(const InspectorTargetHandle& handle)
{
    if (!handle.IsObjectTarget())
        return nullptr;

    if (handle.objectIdentifier.IsValid())
    {
        InstanceID resolvedInstanceId = InstanceID_None;
        if (!Engine::Serialize::PersistentManager::Instance().TryGetInstanceID(
                handle.objectIdentifier,
                resolvedInstanceId))
        {
            return nullptr;
        }
        return Object::IDToPointer(resolvedInstanceId);
    }

    // Runtime-only selections may not have a persistent identifier yet. They
    // remain resolvable for the current editor session; history upgrades normal
    // selections through FromObject before retaining them.
    if (handle.instanceId == InstanceID_None)
        return nullptr;
    return Object::IDToPointer(handle.instanceId);
}

} // namespace NLS::Editor::Inspector

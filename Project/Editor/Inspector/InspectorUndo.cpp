#include "Inspector/InspectorUndo.h"

#include <sstream>

#include <Components/Component.h>
#include <GameObject.h>
#include <ServiceLocator.h>

#include "Core/Context.h"
#include "Core/EditorActions.h"
#include "Inspector/NativeReflectionAdapter.h"
#include "Undo/AssetUndoScope.h"

namespace NLS::Editor::Inspector
{
namespace
{
std::string MakeTargetKey(const InspectorTargetHandle& target)
{
    if (target.objectIdentifier.IsValid())
    {
        if (target.objectIdentifier.IsLocalObject())
            return "object:" + std::to_string(target.objectIdentifier.localIdentifierInFile);
        return "asset-object:" + target.objectIdentifier.guid.ToString() + ":" +
            std::to_string(target.objectIdentifier.localIdentifierInFile);
    }
    if (!target.assetId.empty())
        return "asset:" + target.assetId;
    if (!target.resourcePath.empty())
        return "path:" + target.resourcePath;
    return {};
}

std::optional<Undo::UndoScopeId> MakeOwningSceneScope(const InspectorTargetHandle& target)
{
    if (!target.IsObjectTarget() || !NLS::Core::ServiceLocator::Contains<Core::Context>())
        return std::nullopt;

    auto* object = NativeReflectionAdapter::ResolveObject(target);
    auto* gameObject = dynamic_cast<Engine::GameObject*>(object);
    if (gameObject == nullptr)
    {
        if (const auto* component = dynamic_cast<Engine::Components::Component*>(object))
            gameObject = component->gameobject();
    }
    if (gameObject == nullptr || gameObject->GetScene() == nullptr)
        return std::nullopt;

    if (NLS::Core::ServiceLocator::Contains<Core::EditorActions>())
    {
        const auto scope = NLS::Core::ServiceLocator::Get<Core::EditorActions>().GetUndoScopeForScene(
            *gameObject->GetScene());
        if (scope.IsValid())
            return scope;
    }

    const auto& context = NLS::Core::ServiceLocator::Get<Core::Context>();
    if (context.activePrefabStage.has_value() &&
        context.activePrefabStage->stageScene.get() == gameObject->GetScene())
    {
        const auto& stage = *context.activePrefabStage;
        if (!stage.prefabAssetPath.empty())
            return Undo::UndoScopeId {Undo::UndoScopeKind::PrefabStage, stage.prefabAssetPath, 0u};
    }

    if (context.sceneManager.GetCurrentScene() != gameObject->GetScene())
        return std::nullopt;
    const auto scenePath = context.sceneManager.GetCurrentSceneSourcePath();
    return Undo::UndoScopeId {
        Undo::UndoScopeKind::Scene,
        scenePath.empty() ? std::string("untitled-scene") : scenePath,
        0u};
}

std::string MakeAffectedId(const InspectorTargetHandle& target, const PropertyPath& path)
{
    std::ostringstream stream;
    stream << static_cast<unsigned int>(target.kind) << ':' << MakeTargetKey(target);
    if (!path.Empty())
        stream << ':' << path.ToString();
    return stream.str();
}
} // namespace

Undo::UndoScopeId MakeInspectorAssetUndoScope(
    const std::string& resourcePath,
    const std::string& assetId)
{
    return Undo::MakeAssetUndoScope(resourcePath, assetId);
}

Undo::UndoScopeId MakeInspectorUndoScope(const InspectorTargetHandle& target)
{
    if (target.kind == InspectorTargetKind::AssetImporter || target.kind == InspectorTargetKind::Material)
        return MakeInspectorAssetUndoScope(target.resourcePath, target.assetId);
    if (target.kind == InspectorTargetKind::ProjectSettings)
        return {Undo::UndoScopeKind::ProjectSettings, MakeTargetKey(target), 0u};
    if (const auto ownerScope = MakeOwningSceneScope(target); ownerScope.has_value())
        return *ownerScope;
    return {Undo::UndoScopeKind::Scene, MakeTargetKey(target), 0u};
}

Undo::UndoCommandInfo BuildInspectorUndoCommandInfo(
    std::string label,
    const std::vector<InspectorTargetHandle>& targets,
    const PropertyPath& path,
    const bool allowMerge)
{
    Undo::UndoCommandInfo info;
    info.label = std::move(label);
    info.sourceModule = "Inspector";
    info.mergeKey = path.Empty() ? std::string {} : path.ToString();
    info.allowMerge = allowMerge && !info.mergeKey.empty();
    for (const auto& target : targets)
    {
        if (const auto scope = MakeInspectorUndoScope(target); scope.IsValid())
            info.scopes.push_back(scope);
        if (const auto affected = MakeAffectedId(target, path); !affected.empty())
            info.affected.push_back(affected);
    }
    return info;
}
} // namespace NLS::Editor::Inspector

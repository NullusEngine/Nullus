#include <filesystem>
#include <algorithm>
#include "ImGui/imgui.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <deque>
#include <iterator>
#include <mutex>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <Debug/Logger.h>

#include <Components/MeshRenderer.h>
#include <Components/MeshFilter.h>
#include <Rendering/Resources/Material.h>
#include <Rendering/Resources/Shader.h>
#include <Rendering/EditorDefaultResources.h>
#include <Math/Vector4.h>
#include <Math/Vector2.h>


#include <Windowing/Dialogs/OpenFileDialog.h>
#include <Windowing/Dialogs/SaveFileDialog.h>
#include <Windowing/Dialogs/MessageBox.h>

#include <Utils/PathParser.h>
#include <Utils/String.h>
#include <Utils/SystemCalls.h>

#include "Core/EditorActions.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/EditorAssetDatabase.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetPathUtils.h"
#include "Assets/PrefabUtilityFacade.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Engine/PrimitiveFactory.h"
#include "Components/TransformComponent.h"
#include "Panels/Inspector.h"
#include "Panels/SceneView.h"
#include "Panels/AssetBrowser.h"
#include "ResourceManagement/MaterialManager.h"
#include "ResourceManagement/MeshManager.h"
#include "Panels/Console.h"
#include "ServiceLocator.h"
#include "Panels/AssetView.h"
#include "Panels/GameView.h"
#include "Panels/FrameInfo.h"
#include "Panels/Hierarchy.h"
#include "Panels/MaterialEditor.h"
#include "Panels/SceneView.h"
#include "Reflection/Type.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/ObjectGraphReader.h"
#include "Serialize/ObjectGraphSerializer.h"
#include "Serialize/ObjectGraphWriter.h"
#include "ResourceManagement/TextureManager.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Resources/Loaders/MaterialLoader.h"
// #include "Panels/AssetView.h"
// #include "Panels/GameView.h"
// #include "Panels/Inspector.h"
// #include "Panels/ProjectSettings.h"
// #include "Panels/MaterialEditor.h"

using namespace NLS;

namespace
{
std::optional<std::string> ReadTextFileAtPath(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
        return std::nullopt;

    std::ostringstream stream;
    stream << input.rdbuf();
    return stream.str();
}

bool WriteTextFileAtomicallyAtPath(const std::filesystem::path& path, const std::string& text)
{
    std::error_code error;
    const auto parentPath = path.parent_path();
    if (!parentPath.empty())
    {
        std::filesystem::create_directories(parentPath, error);
        if (error)
            return false;
    }

    const auto tempPath = path.string() + ".tmp";
    {
        std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
            return false;
        output << text;
        if (!output.good())
            return false;
    }

    std::filesystem::rename(tempPath, path, error);
    if (!error)
        return true;

    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(tempPath, path, error);
    if (error)
    {
        std::filesystem::remove(tempPath, error);
        return false;
    }
    return true;
}

constexpr size_t kEditorBackgroundTaskQueueCapacity = 256u;
constexpr auto kRendererResourceResolutionFrameBudget = std::chrono::milliseconds(12);
constexpr size_t kRendererResourceResolutionBindTasksPerFrame = 12u;
constexpr size_t kRendererResourceResolutionMeshBindsPerFrame = 4u;
constexpr size_t kRendererResourceResolutionScheduleTasksPerFrame = 24u;
constexpr size_t kRendererResourceResolutionMaxInflightMeshLoads = 16u;
constexpr size_t kRendererResourceResolutionMaterialSlotsPerTask = 8u;
constexpr size_t kRendererResourceResolutionTextureBindsPerFrame = 8u;
enum class RendererResourceResolutionTaskKind
{
    Mesh,
    Material
};

const char* DragDropOperationStatusLabel(const NLS::Editor::Assets::DragDropOperationStatus status)
{
    using NLS::Editor::Assets::DragDropOperationStatus;
    switch (status)
    {
    case DragDropOperationStatus::Committed:
        return "committed";
    case DragDropOperationStatus::Failed:
        return "failed";
    case DragDropOperationStatus::Rejected:
        return "rejected";
    default:
        return "unknown";
    }
}

struct RendererResourceResolutionTask
{
    RendererResourceResolutionTaskKind kind = RendererResourceResolutionTaskKind::Mesh;
    NLS::Engine::Serialize::ObjectId sourceObject;
    std::string modelPath;
    NLS::Array<std::string> materialPaths;
    bool materialHintsApplied = false;
    bool failed = false;
    size_t nextMaterialSlot = 0u;
    size_t nextTextureSlot = 0u;
    std::shared_ptr<struct MeshArtifactLoadState> meshLoad;
};

struct MeshArtifactLoadState
{
    std::mutex mutex;
    bool completed = false;
    bool accepted = true;
    bool failed = false;
    std::shared_ptr<const NLS::Render::Assets::MeshArtifactData> data;
    std::shared_ptr<NLS::Render::Resources::Mesh> transientMesh;
};

struct RendererResourceLiveObjectIndex
{
    const NLS::Editor::Assets::PrefabInstanceRecord* instance = nullptr;
    size_t mappingCount = 0u;
    std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::GameObject*> objectsBySourceId;
};

struct RendererResourceResolutionState
{
    NLS::Editor::Assets::ImportJobId job;
    NLS::Engine::SceneSystem::Scene* scene = nullptr;
    NLS::Core::Assets::AssetId prefabAssetId;
    std::string prefabSubAssetKey;
    std::deque<RendererResourceResolutionTask> remainingTasks;
    std::deque<RendererResourceResolutionTask> inFlightTasks;
    std::unordered_map<std::string, std::shared_ptr<MeshArtifactLoadState>> meshLoadsByPath;
    bool cancelled = false;
    bool failed = false;
    ListenerID destroyedListener = InvalidListenerID;
    NLS::Editor::Core::EditorActions::SceneMutationToken sceneToken;
    const NLS::Editor::Assets::PrefabInstanceRecord* cachedLiveInstance = nullptr;
    RendererResourceLiveObjectIndex liveObjects;
    std::shared_ptr<struct RendererResourceResolutionStats> stats;
    size_t completedTasks = 0u;
    size_t totalTasks = 0u;
    bool rootHiddenUntilRendererResourcesReady = false;
    bool restoreRootSelfActive = true;
};

struct RendererResourceResolutionStats
{
    size_t scheduledMeshTasks = 0u;
    size_t boundMeshTasks = 0u;
    size_t failedMeshTasks = 0u;
    size_t scheduledMaterialTasks = 0u;
    size_t completedMaterialTasks = 0u;
    size_t boundMaterialSlots = 0u;
    size_t loadedTextureSlots = 0u;
    size_t unresolvedMaterialSlots = 0u;
    size_t failedMaterialSlots = 0u;
};

struct PrefabResolvedAssetIndex
{
    std::unordered_map<NLS::Core::Assets::AssetId, std::vector<const NLS::Engine::Assets::PrefabResolvedAsset*>> byAssetId;
    std::unordered_map<std::string, std::vector<const NLS::Engine::Assets::PrefabResolvedAsset*>> bySubAssetKey;
    std::unordered_map<std::string, std::vector<const NLS::Engine::Assets::PrefabResolvedAsset*>> byArtifactPath;
};

struct RendererResourceInstanceStats
{
    size_t meshRenderers = 0u;
    size_t boundMeshes = 0u;
    size_t materialSlotRenderers = 0u;
    size_t boundMaterialSlotRenderers = 0u;
};

std::string ToGenericPath(const std::filesystem::path& path)
{
    return path.lexically_normal().generic_string();
}

std::filesystem::path ProjectRootFromAssetsPath(const std::string& projectAssetsPath)
{
    auto assetsPath = std::filesystem::path(projectAssetsPath).lexically_normal();
    while (!assetsPath.empty() && !assetsPath.has_filename())
        assetsPath = assetsPath.parent_path();
    return assetsPath.parent_path();
}

std::string BuildPrefabResourcePathFromPayload(
    const NLS::Editor::Assets::EditorAssetDragPayload& payload)
{
    auto path = NLS::Editor::Assets::GetEditorAssetDragPayloadPath(payload);
    const auto subAssetKey = NLS::Editor::Assets::GetEditorAssetDragPayloadSubAssetKey(payload);
    if (!path.empty() && !subAssetKey.empty())
        path += "#" + subAssetKey;
    return path;
}

bool ImportCopiedAssetThroughDatabase(
    const std::string& projectAssetsPath,
    const std::string& destination)
{
    const auto projectRoot = ProjectRootFromAssetsPath(projectAssetsPath);
    auto assetPath = std::filesystem::path(destination).lexically_relative(projectRoot);
    if (assetPath.empty() || assetPath.is_absolute())
        return true;

    for (const auto& part : assetPath)
    {
        if (part == "..")
            return true;
    }

    auto& tracker = NLS::Core::ServiceLocator::Get<NLS::Editor::Core::EditorActions>().GetContext().importProgressTracker;
    const auto normalizedAssetPath = ToGenericPath(assetPath);
    auto& actions = NLS::Core::ServiceLocator::Get<NLS::Editor::Core::EditorActions>();
    const bool accepted = actions.TrackBackgroundTask(
        [projectRoot, normalizedAssetPath, &tracker]
        {
            NLS::Editor::Assets::AssetDatabaseFacade database(
                NLS::Editor::Assets::MakeProjectEditorAssetRoots(projectRoot));
            NLS::Editor::Assets::AssetPreimportScheduler scheduler;
            scheduler.Run(
                database,
                tracker,
                {
                    NLS::Editor::Assets::AssetPreimportReason::AssetCopiedOrMoved,
                    {std::filesystem::path(normalizedAssetPath)}
                });
        });
    if (!accepted)
        NLS_LOG_WARNING("Asset database import task was dropped because the editor background queue is saturated or stopping: " + normalizedAssetPath);
    return true;
}

bool RewriteTextAssetPreservingMeta(
    NLS::Editor::Assets::AssetDatabaseFacade& database,
    const std::filesystem::path& projectRoot,
    const std::string& assetPath,
    const std::string& contents)
{
    auto relativePath = std::filesystem::path(assetPath).lexically_normal();
    if (relativePath.empty() || relativePath.is_absolute())
        return false;

    for (const auto& part : relativePath)
    {
        if (part == "..")
            return false;
    }

    auto absolutePath = (projectRoot / relativePath).lexically_normal();
    const auto containedPath = absolutePath.lexically_relative(projectRoot.lexically_normal());
    if (containedPath.empty() || containedPath.is_absolute())
        return false;

    for (const auto& part : containedPath)
    {
        if (part == "..")
            return false;
    }

    std::error_code error;
    std::filesystem::create_directories(absolutePath.parent_path(), error);
    if (error)
        return false;

    std::ofstream output(absolutePath, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;

    output << contents;
    output.close();
    return output && database.ImportAsset(ToGenericPath(relativePath));
}

NLS::Render::Resources::Material* GetOrCreateEditorDefaultMaterial(NLS::Editor::Core::Context& context)
{
    if (!context.editorResources)
        return nullptr;

    auto* shader = context.editorResources->GetLoadedShader("DebugLitColor");
    if (!shader)
        return nullptr;

    static NLS::Render::Resources::Material fallback;
    if (fallback.GetShader() != shader)
    {
        fallback.SetShader(shader);
        fallback.path = ":Editor/GeneratedModelFallbackMaterial";
        fallback.Set<Maths::Vector4>("u_Diffuse", { 0.86f, 0.88f, 0.92f, 1.0f });
        fallback.SetBackfaceCulling(false);
        fallback.SetFrontfaceCulling(false);
        fallback.SetDepthTest(true);
        fallback.SetDepthWriting(true);
        fallback.SetColorWriting(true);
    }
    return &fallback;
}

std::unordered_map<NLS::Engine::Serialize::ObjectId, const NLS::Engine::Serialize::ObjectRecord*> BuildPrefabObjectRecordIndex(
    const NLS::Engine::Serialize::ObjectGraphDocument& graph)
{
    std::unordered_map<NLS::Engine::Serialize::ObjectId, const NLS::Engine::Serialize::ObjectRecord*> recordsById;
    recordsById.reserve(graph.objects.size());
    for (const auto& object : graph.objects)
        recordsById.emplace(object.id, &object);
    return recordsById;
}

std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::GameObject*> BuildPrefabInstanceObjectIndex(
    const NLS::Editor::Assets::PrefabInstanceRecord& instance)
{
    std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::GameObject*> objectsBySourceId;
    objectsBySourceId.reserve(instance.sourceByInstanceObject.size());
    for (const auto& mapping : instance.sourceByInstanceObject)
        objectsBySourceId.emplace(mapping.second, const_cast<NLS::Engine::GameObject*>(mapping.first));
    return objectsBySourceId;
}

RendererResourceLiveObjectIndex BuildRendererResourceLiveObjectIndex(
    const NLS::Editor::Assets::PrefabInstanceRecord& instance)
{
    RendererResourceLiveObjectIndex index;
    index.instance = &instance;
    index.mappingCount = instance.sourceByInstanceObject.size();
    index.objectsBySourceId = BuildPrefabInstanceObjectIndex(instance);
    return index;
}

PrefabResolvedAssetIndex BuildPrefabResolvedAssetIndex(
    const NLS::Engine::Assets::PrefabArtifact& prefab)
{
    PrefabResolvedAssetIndex index;
    index.byAssetId.reserve(prefab.resolvedAssets.size());
    index.bySubAssetKey.reserve(prefab.resolvedAssets.size());
    index.byArtifactPath.reserve(prefab.resolvedAssets.size());
    for (const auto& resolved : prefab.resolvedAssets)
    {
        index.byAssetId[resolved.assetId].push_back(&resolved);
        if (!resolved.subAssetKey.empty())
            index.bySubAssetKey[resolved.subAssetKey].push_back(&resolved);
        if (!resolved.artifactPath.empty())
        {
            index.byArtifactPath[resolved.artifactPath].push_back(&resolved);
            const auto normalizedPath = std::filesystem::path(resolved.artifactPath)
                .lexically_normal()
                .generic_string();
            index.byArtifactPath[normalizedPath].push_back(&resolved);
        }
    }
    return index;
}

const NLS::Engine::Serialize::PropertyRecord* FindPrefabProperty(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const std::string& name)
{
    for (const auto& property : record.properties)
    {
        if (property.name == name)
            return &property;
    }
    return nullptr;
}

std::vector<NLS::Engine::Serialize::ObjectId> ReadOwnedPrefabArray(
    const NLS::Engine::Serialize::ObjectRecord& record,
    const std::string& propertyName)
{
    std::vector<NLS::Engine::Serialize::ObjectId> ids;
    const auto* property = FindPrefabProperty(record, propertyName);
    if (!property || property->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::Array)
        return ids;

    for (const auto& value : property->value.GetArray())
    {
        if (value.GetKind() == NLS::Engine::Serialize::PropertyValue::Kind::OwnedReference)
            ids.push_back(value.GetObjectId());
    }
    return ids;
}

std::optional<std::string> ResolvePrefabAssetPath(
    const PrefabResolvedAssetIndex& resolvedAssets,
    const NLS::Engine::Serialize::PropertyValue& value)
{
    if (value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::ObjectReference ||
        !value.GetObjectReference().guid.IsValid())
        return std::nullopt;

    const auto& reference = value.GetObjectReference();
    const auto assetId = NLS::Core::Assets::AssetId(reference.guid);
    const auto referencePath = std::filesystem::path(reference.filePath)
        .lexically_normal()
        .generic_string();
    const NLS::Engine::Assets::PrefabResolvedAsset* candidate = nullptr;
    auto matchesExpectedType = [](const NLS::Engine::Assets::PrefabResolvedAsset&)
    {
        return true;
    };
    auto tryCandidate = [&](const NLS::Engine::Assets::PrefabResolvedAsset* resolved)
        -> std::optional<std::string>
    {
        if (!resolved || !matchesExpectedType(*resolved))
            return std::nullopt;
        return resolved->artifactPath;
    };

    if (!reference.filePath.empty())
    {
        auto tryCandidates = [&](const auto& candidates) -> std::optional<std::string>
        {
            for (const auto* resolved : candidates)
            {
                if (auto path = tryCandidate(resolved))
                    return path;
            }
            return std::nullopt;
        };

        if (const auto foundBySubAsset = resolvedAssets.bySubAssetKey.find(reference.filePath);
            foundBySubAsset != resolvedAssets.bySubAssetKey.end())
        {
            if (auto path = tryCandidates(foundBySubAsset->second))
                return path;
        }
        auto matchesAssetOrHint = [&](const NLS::Engine::Assets::PrefabResolvedAsset& resolved)
        {
            return resolved.assetId == assetId || resolved.subAssetKey == reference.filePath;
        };
        auto tryPathCandidates = [&](const auto& candidates) -> std::optional<std::string>
        {
            for (const auto* resolved : candidates)
            {
                if (!resolved || !matchesAssetOrHint(*resolved))
                    continue;
                if (auto path = tryCandidate(resolved))
                    return path;
            }
            return std::nullopt;
        };

        if (const auto foundByPath = resolvedAssets.byArtifactPath.find(reference.filePath);
            foundByPath != resolvedAssets.byArtifactPath.end())
        {
            if (auto path = tryPathCandidates(foundByPath->second))
                return path;
        }
        if (!referencePath.empty() && referencePath != reference.filePath)
        {
            if (const auto foundByNormalizedPath = resolvedAssets.byArtifactPath.find(referencePath);
                foundByNormalizedPath != resolvedAssets.byArtifactPath.end())
            {
                if (auto path = tryPathCandidates(foundByNormalizedPath->second))
                    return path;
            }
        }
        if (const auto foundByAssetId = resolvedAssets.byAssetId.find(assetId);
            foundByAssetId != resolvedAssets.byAssetId.end())
        {
            for (const auto* resolved : foundByAssetId->second)
            {
                if (!resolved || !matchesExpectedType(*resolved))
                    continue;
                if (resolved->subAssetKey == reference.filePath ||
                    resolved->artifactPath == reference.filePath ||
                    (!resolved->artifactPath.empty() &&
                        std::filesystem::path(resolved->artifactPath).lexically_normal().generic_string() == referencePath))
                {
                    return resolved->artifactPath;
                }
            }
        }
        return std::nullopt;
    }

    if (const auto foundByAssetId = resolvedAssets.byAssetId.find(assetId);
        foundByAssetId != resolvedAssets.byAssetId.end())
    {
        for (const auto* resolved : foundByAssetId->second)
        {
            if (!resolved || !matchesExpectedType(*resolved))
                continue;
            if (candidate)
                return std::nullopt;
            candidate = resolved;
        }
    }

    if (candidate)
        return candidate->artifactPath;
    return std::nullopt;
}

bool IsObjectInSceneHierarchy(
    const NLS::Engine::SceneSystem::Scene& scene,
    const NLS::Engine::GameObject& object)
{
    for (const auto* root : scene.GetGameObjects())
    {
        if (!root || !root->IsAlive())
            continue;

        if (root == &object || object.IsDescendantOf(root))
            return true;
    }
    return false;
}

NLS::Engine::SceneSystem::Scene* ResolveGameObjectCreationScene(
    NLS::Editor::Core::Context& m_context,
    const NLS::Engine::GameObject* parent)
{
    if (parent)
    {
        if (m_context.activePrefabStage.has_value() &&
            m_context.activePrefabStage->editable &&
            m_context.activePrefabStage->stageScene &&
            IsObjectInSceneHierarchy(*m_context.activePrefabStage->stageScene, *parent))
        {
            return m_context.activePrefabStage->stageScene.get();
        }

        auto* currentScene = m_context.sceneManager.GetCurrentScene();
        if (currentScene && IsObjectInSceneHierarchy(*currentScene, *parent))
            return currentScene;

        return nullptr;
    }

    if (m_context.activePrefabStage.has_value() && m_context.activePrefabStage->stageScene)
    {
        if (!m_context.activePrefabStage->editable)
            return nullptr;
        return m_context.activePrefabStage->stageScene.get();
    }

    return m_context.sceneManager.GetCurrentScene();
}

bool IsGameObjectCreationSceneLive(
    NLS::Editor::Core::Context& m_context,
    const NLS::Engine::SceneSystem::Scene* scene,
    const NLS::Editor::Core::EditorActions::SceneMutationToken& sceneToken,
    const NLS::Editor::Core::EditorActions& actions)
{
    if (!scene)
        return false;

    const auto currentToken = actions.CaptureSceneMutationToken();
    if (currentToken.mainSceneGeneration != sceneToken.mainSceneGeneration ||
        currentToken.prefabStageGeneration != sceneToken.prefabStageGeneration)
    {
        return false;
    }

    if (m_context.activePrefabStage.has_value() &&
        m_context.activePrefabStage->stageScene &&
        m_context.activePrefabStage->stageScene.get() == scene)
    {
        return true;
    }

    return m_context.sceneManager.GetCurrentScene() == scene;
}

bool IsGameObjectCreationSceneLive(
    NLS::Editor::Core::Context& m_context,
    const NLS::Engine::SceneSystem::Scene* scene)
{
    if (!scene)
        return false;

    if (m_context.activePrefabStage.has_value() &&
        m_context.activePrefabStage->stageScene &&
        m_context.activePrefabStage->stageScene.get() == scene)
    {
        return true;
    }

    return m_context.sceneManager.GetCurrentScene() == scene;
}

void MarkGameObjectCreationSceneDirty(
    NLS::Editor::Core::Context& m_context,
    NLS::Engine::SceneSystem::Scene& scene)
{
    if (m_context.activePrefabStage.has_value() &&
        m_context.activePrefabStage->stageScene &&
        m_context.activePrefabStage->stageScene.get() == &scene)
    {
        m_context.activePrefabStage->dirty = true;
        return;
    }

    if (m_context.sceneManager.GetCurrentScene() == &scene)
        m_context.sceneManager.MarkCurrentSceneDirty();
}

NLS::Engine::SceneSystem::Scene* ResolveSceneForLiveObject(
    NLS::Editor::Core::Context& m_context,
    const NLS::Engine::GameObject& object)
{
    if (m_context.activePrefabStage.has_value() &&
        m_context.activePrefabStage->stageScene &&
        IsObjectInSceneHierarchy(*m_context.activePrefabStage->stageScene, object))
    {
        return m_context.activePrefabStage->stageScene.get();
    }

    auto* currentScene = m_context.sceneManager.GetCurrentScene();
    if (currentScene && IsObjectInSceneHierarchy(*currentScene, object))
        return currentScene;

    return nullptr;
}

bool HasResolvedMaterialBindings(NLS::Engine::Components::MeshRenderer& meshRenderer)
{
    const auto paths = meshRenderer.GetMaterialPaths();
    if (paths.empty())
        return true;

    for (size_t index = 0u; index < paths.size() && index < NLS::Engine::Components::MeshRenderer::kMaxMaterialCount; ++index)
    {
        if (paths[index].empty())
            continue;

        const auto* material = meshRenderer.GetMaterialAtIndex(static_cast<uint8_t>(index));
        if (!material || !material->IsValid() || material->path != paths[index])
            return false;
    }
    return true;
}

const NLS::Editor::Assets::PrefabInstanceRecord* ResolveLivePrefabInstance(
    NLS::Editor::Core::EditorActions& actions,
    const RendererResourceResolutionState& state)
{
    if (state.cancelled)
        return nullptr;

    if (!IsGameObjectCreationSceneLive(actions.GetContext(), state.scene, state.sceneToken, actions))
        return nullptr;

    const auto* cached = state.cachedLiveInstance;
    if (!cached)
        return nullptr;

    auto* root = cached->instanceRoot;
    if (!root || !root->IsAlive())
        return nullptr;

    if (!IsObjectInSceneHierarchy(*state.scene, *root))
        return nullptr;

    const auto* registered = actions.GetContext().prefabInstanceRegistry.FindInstance(*root);
    if (registered != cached)
        return nullptr;

    return cached;
}

void RendererResourceResolutionTargetDestroyed(
    RendererResourceResolutionState& state,
    NLS::Engine::GameObject& destroyed)
{
    if (state.cancelled)
        return;

    for (const auto& mapping : state.liveObjects.objectsBySourceId)
    {
        if (mapping.second == &destroyed)
        {
            state.cancelled = true;
            return;
        }
    }
}

NLS::Engine::GameObject* ResolveLiveTaskObject(
    const std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::GameObject*>& liveObjectsBySourceId,
    const RendererResourceResolutionTask& task)
{
    const auto found = liveObjectsBySourceId.find(task.sourceObject);
    auto* object = found != liveObjectsBySourceId.end() ? found->second : nullptr;
    if (!object || !object->IsAlive())
        return nullptr;
    return object;
}

const std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::GameObject*>* EnsureRendererResourceLiveObjectIndex(
    RendererResourceResolutionState& state,
    const NLS::Editor::Assets::PrefabInstanceRecord& liveInstance)
{
    if (state.liveObjects.instance != &liveInstance ||
        state.liveObjects.mappingCount != liveInstance.sourceByInstanceObject.size())
    {
        state.liveObjects = BuildRendererResourceLiveObjectIndex(liveInstance);
    }

    return &state.liveObjects.objectsBySourceId;
}

void CountResolvedRendererResources(
    NLS::Engine::GameObject& object,
    RendererResourceInstanceStats& stats)
{
    if (auto* meshRenderer = object.GetComponent<NLS::Engine::Components::MeshRenderer>())
    {
        ++stats.meshRenderers;
        if (auto* meshFilter = object.GetComponent<NLS::Engine::Components::MeshFilter>();
            meshFilter != nullptr && meshFilter->ResolveMesh() != nullptr)
            ++stats.boundMeshes;

        ++stats.materialSlotRenderers;
        if (HasResolvedMaterialBindings(*meshRenderer))
            ++stats.boundMaterialSlotRenderers;
    }

    for (auto* child : object.GetChildren())
    {
        if (child && child->IsAlive())
            CountResolvedRendererResources(*child, stats);
    }
}

RendererResourceInstanceStats CountResolvedRendererResources(NLS::Engine::GameObject& root)
{
    RendererResourceInstanceStats stats;
    CountResolvedRendererResources(root, stats);
    return stats;
}

NLS::Engine::GameObject* FindLiveGameObjectByAddress(
    NLS::Engine::SceneSystem::Scene& scene,
    const NLS::Engine::GameObject* candidate)
{
    if (!candidate)
        return nullptr;

    std::vector<NLS::Engine::GameObject*> stack;
    for (auto* root : scene.GetGameObjects())
    {
        if (root)
            stack.push_back(root);
    }

    while (!stack.empty())
    {
        auto* object = stack.back();
        stack.pop_back();
        if (!object)
            continue;
        if (object == candidate)
            return object->IsAlive() ? object : nullptr;

        for (auto* child : object->GetChildren())
        {
            if (child)
                stack.push_back(child);
        }
    }

    return nullptr;
}

void RestoreRendererResourceResolutionRootVisibility(RendererResourceResolutionState& state)
{
    if (!state.rootHiddenUntilRendererResourcesReady)
        return;

    auto* root = state.cachedLiveInstance ? state.cachedLiveInstance->instanceRoot : nullptr;
    if (root && root->IsAlive())
        root->SetActive(state.restoreRootSelfActive);
    state.rootHiddenUntilRendererResourcesReady = false;
}

void RollbackHiddenRendererResourceResolutionRoot(
    NLS::Editor::Core::EditorActions& actions,
    RendererResourceResolutionState& state)
{
    if (!state.rootHiddenUntilRendererResourcesReady)
        return;

    auto* root = state.cachedLiveInstance ? state.cachedLiveInstance->instanceRoot : nullptr;
    auto* scene = state.scene;
    if (root && root->IsAlive() && scene)
    {
        if (actions.GetSelectedGameObject() == root)
            actions.UnselectGameObject();
        scene->DestroyGameObject(*root);
    }
    state.rootHiddenUntilRendererResourcesReady = false;
}

template<typename ComponentType>
bool PrefabComponentRecordMatches(const NLS::Engine::Serialize::ObjectRecord& record)
{
    static const std::string typeName = NLS_TYPEOF(ComponentType).GetName();
    return record.typeName == typeName;
}

bool PrefabRecordIsGameObject(const NLS::Engine::Serialize::ObjectRecord& record)
{
    static const std::string typeName = NLS_TYPEOF(NLS::Engine::GameObject).GetName();
    return record.typeName == typeName;
}

void CollectPrefabAssetResolutionTasks(
    NLS::Engine::Assets::PrefabArtifact& prefab,
    const NLS::Editor::Assets::PrefabInstanceRecord& instance,
    std::vector<RendererResourceResolutionTask>& meshTasks,
    std::vector<RendererResourceResolutionTask>& materialTasks,
    const std::shared_ptr<RendererResourceResolutionStats>& stats,
    size_t& visitedGameObjects,
    size_t& visitedMeshReferences,
    size_t& visitedMaterialReferences)
{
    const auto objectRecordsById = BuildPrefabObjectRecordIndex(prefab.graph);
    const auto instanceObjectsBySourceId = BuildPrefabInstanceObjectIndex(instance);
    const auto resolvedAssetIndex = BuildPrefabResolvedAssetIndex(prefab);

    for (const auto& sourceObject : prefab.graph.objects)
    {
        if (sourceObject.state != NLS::Engine::Serialize::ObjectRecordState::Alive ||
            !PrefabRecordIsGameObject(sourceObject))
        {
            continue;
        }

        auto foundInstanceObject = instanceObjectsBySourceId.find(sourceObject.id);
        auto* instanceObject = foundInstanceObject != instanceObjectsBySourceId.end()
            ? foundInstanceObject->second
            : nullptr;
        if (!instanceObject || !instanceObject->IsAlive())
            continue;

        ++visitedGameObjects;
        auto* meshFilter = instanceObject->GetComponent<NLS::Engine::Components::MeshFilter>();
        auto* meshRenderer = instanceObject->GetComponent<NLS::Engine::Components::MeshRenderer>();
        const auto sourceComponents = ReadOwnedPrefabArray(sourceObject, "components");
        for (const auto& componentId : sourceComponents)
        {
            const auto foundComponentRecord = objectRecordsById.find(componentId);
            if (foundComponentRecord == objectRecordsById.end())
                continue;
            const auto* componentRecord = foundComponentRecord->second;

            if (PrefabComponentRecordMatches<NLS::Engine::Components::MeshFilter>(*componentRecord) && meshRenderer && meshFilter)
            {
                if (const auto* model = FindPrefabProperty(*componentRecord, "mesh"))
                {
                    ++visitedMeshReferences;
                    if (auto modelPath = ResolvePrefabAssetPath(resolvedAssetIndex, model->value);
                        modelPath.has_value() && !modelPath->empty())
                    {
                        if (stats)
                            ++stats->scheduledMeshTasks;
                        RendererResourceResolutionTask task;
                        task.kind = RendererResourceResolutionTaskKind::Mesh;
                        task.sourceObject = sourceObject.id;
                        task.modelPath = std::move(*modelPath);
                        meshTasks.push_back(std::move(task));
                    }
                }
            }
            else if (PrefabComponentRecordMatches<NLS::Engine::Components::MeshRenderer>(*componentRecord) && meshRenderer)
            {
                if (const auto* materials = FindPrefabProperty(*componentRecord, "materials");
                    materials && materials->value.GetKind() == NLS::Engine::Serialize::PropertyValue::Kind::Array)
                {
                    NLS::Array<std::string> resolvedPaths;
                    for (const auto& value : materials->value.GetArray())
                    {
                        ++visitedMaterialReferences;
                        auto materialPath = ResolvePrefabAssetPath(resolvedAssetIndex, value);
                        resolvedPaths.push_back(materialPath.value_or(std::string {}));
                    }
                    const auto hasResolvedMaterial = std::any_of(
                        resolvedPaths.begin(),
                        resolvedPaths.end(),
                        [](const std::string& path)
                        {
                            return !path.empty();
                        });
                    if (hasResolvedMaterial)
                    {
                        if (stats)
                            ++stats->scheduledMaterialTasks;
                        RendererResourceResolutionTask task;
                        task.kind = RendererResourceResolutionTaskKind::Material;
                        task.sourceObject = sourceObject.id;
                        task.materialPaths = std::move(resolvedPaths);
                        materialTasks.push_back(std::move(task));
                    }
                }
            }
        }
    }
}

template<typename FrameBudgetExpired>
bool BindDeferredMaterialTextures(
    NLS::Render::Resources::Material& material,
    RendererResourceResolutionTask& task,
    const std::shared_ptr<RendererResourceResolutionStats>& stats,
    const FrameBudgetExpired& frameBudgetExpired)
{
    if (!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::TextureManager>())
        return true;

    const auto& texturePaths = material.GetTextureResourcePaths();
    if (texturePaths.empty())
        return true;

    auto& textureManager = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager);
    size_t visitedTextures = 0u;
    size_t textureIndex = 0u;
    for (const auto& [uniformName, texturePath] : texturePaths)
    {
        if (textureIndex++ < task.nextTextureSlot)
            continue;

        ++visitedTextures;
        auto* texture = textureManager.GetResource(texturePath, false);
        if (!texture)
            texture = textureManager.RequestAsyncArtifact(texturePath);
        if (!texture && textureManager.IsAsyncArtifactLoadPending(texturePath))
            return false;
        if (!texture && textureManager.IsAsyncArtifactLoadFailed(texturePath))
        {
            if (stats)
                ++stats->failedMaterialSlots;
            task.failed = true;
            task.nextTextureSlot = textureIndex;
            return true;
        }
        if (!texture)
        {
            if (stats)
                ++stats->failedMaterialSlots;
            task.failed = true;
            task.nextTextureSlot = textureIndex;
            return true;
        }

        if (texture)
            material.Set<NLS::Render::Resources::Texture2D*>(uniformName, texture);
        if (texture && stats)
            ++stats->loadedTextureSlots;

        task.nextTextureSlot = textureIndex;
        if ((visitedTextures >= kRendererResourceResolutionTextureBindsPerFrame ||
                frameBudgetExpired()) &&
            task.nextTextureSlot < texturePaths.size())
        {
            return false;
        }
    }

    return true;
}

template<typename FrameBudgetExpired>
bool BindDeferredMaterialPaths(
    NLS::Editor::Core::EditorActions& actions,
    NLS::Engine::Components::MeshRenderer& meshRenderer,
    RendererResourceResolutionTask& task,
    const std::shared_ptr<RendererResourceResolutionStats>& stats,
    const FrameBudgetExpired& frameBudgetExpired)
{
    if (!meshRenderer.gameobject() || !meshRenderer.gameobject()->IsAlive())
        return true;

    if (!task.materialHintsApplied)
    {
        meshRenderer.SetMaterialPathHints(task.materialPaths);
        task.materialHintsApplied = true;
    }
    if (!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MaterialManager>())
    {
        if (stats)
            stats->failedMaterialSlots += std::count_if(
                task.materialPaths.begin(),
                task.materialPaths.end(),
                [](const std::string& path)
                {
                    return !path.empty();
                });
        return true;
    }

    auto& materialManager = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager);
    if (task.nextMaterialSlot >= task.materialPaths.size() ||
        task.nextMaterialSlot >= NLS::Engine::Components::MeshRenderer::kMaxMaterialCount)
    {
        task.nextMaterialSlot = 0u;
    }

    size_t visitedSlots = 0u;
    while (task.nextMaterialSlot < task.materialPaths.size() &&
        task.nextMaterialSlot < NLS::Engine::Components::MeshRenderer::kMaxMaterialCount &&
        visitedSlots < kRendererResourceResolutionMaterialSlotsPerTask)
    {
        const auto index = task.nextMaterialSlot;
        ++visitedSlots;
        if (task.materialPaths[index].empty())
        {
            ++task.nextMaterialSlot;
            task.nextTextureSlot = 0u;
            continue;
        }

        auto* material = materialManager.GetResource(task.materialPaths[index], false);
        if (!material || !material->IsValid())
            material = materialManager.LoadArtifactWithoutTextures(task.materialPaths[index]);
        if (!material || !material->IsValid())
        {
            if (stats)
                ++stats->unresolvedMaterialSlots;
            ++task.nextMaterialSlot;
            task.nextTextureSlot = 0u;
            continue;
        }

        const bool alreadyBound =
            meshRenderer.GetMaterialAtIndex(static_cast<uint8_t>(index)) == material;
        if (!alreadyBound)
        {
            meshRenderer.SetResolvedMaterialFromReference(static_cast<uint8_t>(index), *material);
            if (stats)
                ++stats->boundMaterialSlots;
        }

        if (!BindDeferredMaterialTextures(*material, task, stats, frameBudgetExpired))
            return false;

        ++task.nextMaterialSlot;
        task.nextTextureSlot = 0u;

        if (task.nextMaterialSlot < task.materialPaths.size() &&
            task.nextMaterialSlot < NLS::Engine::Components::MeshRenderer::kMaxMaterialCount &&
            frameBudgetExpired())
        {
            return false;
        }
    }

    return task.nextMaterialSlot >= task.materialPaths.size() ||
        task.nextMaterialSlot >= NLS::Engine::Components::MeshRenderer::kMaxMaterialCount;
}

bool BindDeferredMeshPath(
    NLS::Engine::Components::MeshFilter& meshFilter,
    NLS::Engine::Components::MeshRenderer& meshRenderer,
    RendererResourceResolutionTask& task)
{
    if (!meshFilter.gameobject() || !meshFilter.gameobject()->IsAlive())
    {
        return true;
    }

    if (!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MeshManager>())
    {
        meshFilter.SetModelPathHint(task.modelPath);
        meshRenderer.SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL);
        return true;
    }

    auto& meshManager = NLS_SERVICE(NLS::Core::ResourceManagement::MeshManager);
    if (auto* cached = meshManager.GetResource(task.modelPath, false))
    {
        meshFilter.SetResolvedMeshFromReference(cached);
        meshRenderer.SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL);
        return true;
    }

    if (!task.meshLoad)
        return false;

    {
        std::lock_guard lock(task.meshLoad->mutex);
        if (!task.meshLoad->completed)
            return false;
    }

    std::shared_ptr<NLS::Render::Resources::Mesh> transientMesh;
    std::shared_ptr<const NLS::Render::Assets::MeshArtifactData> data;
    bool loadAccepted = true;
    bool loadFailed = false;
    {
        std::lock_guard lock(task.meshLoad->mutex);
        transientMesh = task.meshLoad->transientMesh;
        data = task.meshLoad->data;
        loadAccepted = task.meshLoad->accepted;
        loadFailed = task.meshLoad->failed;
    }

    if (!loadAccepted || loadFailed)
    {
        meshFilter.SetModelPathHint(task.modelPath);
        meshRenderer.SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL);
        task.failed = true;
        return true;
    }

    if (transientMesh)
    {
        meshFilter.SetResolvedTransientMeshFromReference(std::move(transientMesh));
    }
    else if (data)
    {
        if (auto* cached = meshManager.GetResource(task.modelPath, false))
        {
            meshFilter.SetResolvedMeshFromReference(cached);
        }
        else
        {
            auto owner = std::shared_ptr<Render::Resources::Mesh>(
                new Render::Resources::Mesh(
                    data->vertices,
                    data->indices,
                    data->materialIndex,
                    Render::Resources::MeshBufferUploadMode::CpuToGpu,
                    data->boundingSphere));
            {
                std::lock_guard lock(task.meshLoad->mutex);
                if (!task.meshLoad->transientMesh)
                {
                    task.meshLoad->transientMesh = owner;
                    task.meshLoad->data.reset();
                }
                else
                {
                    owner = task.meshLoad->transientMesh;
                }
            }
            if (owner)
            {
                meshFilter.SetResolvedTransientMeshFromReference(std::move(owner));
            }
            else
            {
                meshFilter.SetModelPathHint(task.modelPath);
                task.failed = true;
            }
        }
    }
    else
    {
        meshFilter.SetModelPathHint(task.modelPath);
        task.failed = true;
    }
    meshRenderer.SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_MODEL);
    return true;
}

bool IsMeshTaskAlreadyCached(const RendererResourceResolutionTask& task)
{
    if (task.kind != RendererResourceResolutionTaskKind::Mesh || task.modelPath.empty())
        return false;

    if (!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MeshManager>())
        return false;

    auto& meshManager = NLS_SERVICE(NLS::Core::ResourceManagement::MeshManager);
    return meshManager.GetResource(task.modelPath, false) != nullptr;
}

template<typename FrameBudgetExpired>
bool RunRendererResourceResolutionTask(
    NLS::Editor::Core::EditorActions& actions,
    const NLS::Editor::Assets::PrefabInstanceRecord& instance,
    RendererResourceResolutionTask& task,
    const std::unordered_map<NLS::Engine::Serialize::ObjectId, NLS::Engine::GameObject*>& liveObjectsBySourceId,
    const std::shared_ptr<RendererResourceResolutionStats>& stats,
    const FrameBudgetExpired& frameBudgetExpired)
{
    auto* object = ResolveLiveTaskObject(liveObjectsBySourceId, task);
    if (!object)
        return true;

    switch (task.kind)
    {
    case RendererResourceResolutionTaskKind::Mesh:
    {
        auto* meshFilter = object->GetComponent<NLS::Engine::Components::MeshFilter>();
        auto* meshRenderer = object->GetComponent<NLS::Engine::Components::MeshRenderer>();
        if (!meshFilter || !meshRenderer)
            return true;

        const bool completed = BindDeferredMeshPath(*meshFilter, *meshRenderer, task);
        if (completed && stats)
        {
            if (meshFilter->ResolveMesh())
                ++stats->boundMeshTasks;
            else
                ++stats->failedMeshTasks;
        }
        return completed;
    }
    case RendererResourceResolutionTaskKind::Material:
    {
        auto* meshRenderer = object->GetComponent<NLS::Engine::Components::MeshRenderer>();
        if (!meshRenderer)
            return true;

        const bool completed = BindDeferredMaterialPaths(actions, *meshRenderer, task, stats, frameBudgetExpired);
        if (completed && stats)
            ++stats->completedMaterialTasks;
        return completed;
    }
    default:
        return true;
    }
}

bool StartMeshArtifactLoad(
    NLS::Editor::Core::EditorActions& actions,
    RendererResourceResolutionTask& task,
    std::unordered_map<std::string, std::shared_ptr<MeshArtifactLoadState>>& meshLoadsByPath)
{
    if (task.kind != RendererResourceResolutionTaskKind::Mesh || task.meshLoad)
        return false;

    const auto existingLoad = meshLoadsByPath.find(task.modelPath);
    if (existingLoad != meshLoadsByPath.end())
    {
        task.meshLoad = existingLoad->second;
        return false;
    }

    task.meshLoad = std::make_shared<MeshArtifactLoadState>();
    meshLoadsByPath.emplace(task.modelPath, task.meshLoad);
    auto state = task.meshLoad;
    auto path = task.modelPath;
    const bool accepted = actions.TrackBackgroundTask(
        [state, path = std::move(path)]
        {
            std::optional<NLS::Render::Assets::MeshArtifactData> data;
            try
            {
                std::filesystem::path artifactPath = path;
                if (!artifactPath.is_absolute() &&
                    NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MeshManager>())
                {
                    artifactPath = NLS::Core::ResourceManagement::MeshManager::ResolveResourcePath(path);
                }

                data = NLS::Render::Assets::LoadMeshArtifact(artifactPath);
            }
            catch (const std::exception& exception)
            {
                NLS_LOG_ERROR(std::string("Renderer mesh artifact load failed: ") + path + " error=" + exception.what());
            }
            catch (...)
            {
                NLS_LOG_ERROR("Renderer mesh artifact load failed: " + path + " error=unknown");
            }

            std::lock_guard lock(state->mutex);
            if (data.has_value())
            {
                state->data = std::make_shared<NLS::Render::Assets::MeshArtifactData>(std::move(*data));
                state->failed = false;
            }
            else
            {
                state->data.reset();
                state->failed = true;
            }
            state->completed = true;
        });
    if (!accepted)
    {
        std::lock_guard lock(state->mutex);
        state->accepted = false;
        state->failed = true;
        state->completed = true;
        NLS_LOG_ERROR("Editor background task queue rejected mesh artifact load: " + task.modelPath);
    }
    return true;
}

std::optional<RendererResourceResolutionTask> PopNextRemainingTask(
    std::deque<RendererResourceResolutionTask>& tasks,
    bool preferMaterialTask)
{
    if (tasks.empty())
        return std::nullopt;

    const auto selectAndPop = [&tasks](const auto predicate)
        -> std::optional<RendererResourceResolutionTask>
    {
        auto found = std::find_if(tasks.begin(), tasks.end(), predicate);
        if (found == tasks.end())
            return std::nullopt;

        RendererResourceResolutionTask task = std::move(*found);
        tasks.erase(found);
        return task;
    };

    if (preferMaterialTask)
    {
        if (auto task = selectAndPop(
            [](const RendererResourceResolutionTask& candidate)
            {
                return candidate.kind == RendererResourceResolutionTaskKind::Material;
            }))
        {
            return task;
        }
        return std::nullopt;
    }

    RendererResourceResolutionTask task = std::move(tasks.front());
    tasks.pop_front();
    return task;
}

void RunRendererResourceResolutionStep(
    NLS::Editor::Core::EditorActions& actions,
    const std::shared_ptr<RendererResourceResolutionState>& state)
{
    auto& tracker = actions.GetContext().importProgressTracker;
    auto scheduleNextStep = [&actions, state]
    {
        actions.DelayAction(
            [&actions, state]
            {
                RunRendererResourceResolutionStep(actions, state);
            },
            1);
    };

    const auto frameStart = std::chrono::steady_clock::now();
    size_t boundTasksThisFrame = 0u;
    size_t meshBindsThisFrame = 0u;
    size_t scheduledTasksThisFrame = 0u;

    auto finishCancelled = [&actions, &tracker, &state](const bool restoreRootVisibility)
    {
        if (restoreRootVisibility)
            RestoreRendererResourceResolutionRootVisibility(*state);
        actions.ReleaseGameObjectDestroyedListener(state->destroyedListener);
        state->destroyedListener = InvalidListenerID;
        tracker.FinishJob(
            state->job,
            NLS::Editor::Assets::ImportJobTerminalStatus::Cancelled,
            {});
    };

    auto finishFailed = [&actions, &tracker, &state]
    {
        RollbackHiddenRendererResourceResolutionRoot(actions, *state);
        actions.ReleaseGameObjectDestroyedListener(state->destroyedListener);
        state->destroyedListener = InvalidListenerID;
        tracker.ReportProgress(state->job, NLS::Editor::Assets::ImportPhase::Postprocess, 1.0, "Renderer resource resolution failed");
        tracker.FinishJob(
            state->job,
            NLS::Editor::Assets::ImportJobTerminalStatus::Failed,
            {});
    };

    if (state->cancelled)
    {
        NLS_LOG_INFO("Cancelled renderer resource resolution because the prefab instance was destroyed");
        finishCancelled(false);
        return;
    }

    auto* liveInstance = ResolveLivePrefabInstance(actions, *state);
    if (!liveInstance)
    {
        NLS_LOG_INFO("Cancelled renderer resource resolution because the prefab instance is no longer live");
        finishCancelled(true);
        return;
    }

    auto reportTaskProgress = [&tracker, &state]
    {
        tracker.ReportProgress(
            state->job,
            NLS::Editor::Assets::ImportPhase::Postprocess,
            static_cast<double>(state->completedTasks) / static_cast<double>(state->totalTasks),
            "Resolving renderer resource " +
                std::to_string(state->completedTasks + 1u) +
                "/" +
                std::to_string(state->totalTasks));
    };

    auto frameBudgetExpired = [&frameStart]
    {
        return std::chrono::steady_clock::now() - frameStart >= kRendererResourceResolutionFrameBudget;
    };

    if (NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::TextureManager>())
    {
        auto& textureManager = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager);
        textureManager.PumpAsyncLoads(kRendererResourceResolutionTextureBindsPerFrame);
        if (frameBudgetExpired())
        {
            scheduleNextStep();
            return;
        }
    }

    const auto* liveObjectsBySourceId = EnsureRendererResourceLiveObjectIndex(*state, *liveInstance);

    for (size_t index = 0u;
        index < state->inFlightTasks.size() && boundTasksThisFrame < kRendererResourceResolutionBindTasksPerFrame;)
    {
        reportTaskProgress();

        auto task = std::move(state->inFlightTasks.front());
        state->inFlightTasks.pop_front();
        const bool completed = RunRendererResourceResolutionTask(
            actions,
            *liveInstance,
            task,
            *liveObjectsBySourceId,
            state->stats,
            frameBudgetExpired);
        if (completed)
        {
            if (task.kind == RendererResourceResolutionTaskKind::Mesh)
            {
                ++meshBindsThisFrame;
                if (task.failed || (task.meshLoad && !task.meshLoad->accepted))
                    state->failed = true;
            }
            ++state->completedTasks;
            ++boundTasksThisFrame;
        }
        else
        {
            state->inFlightTasks.push_back(std::move(task));
            ++index;
        }

        if (frameBudgetExpired())
        {
            scheduleNextStep();
            return;
        }
        if (meshBindsThisFrame >= kRendererResourceResolutionMeshBindsPerFrame)
        {
            scheduleNextStep();
            return;
        }
    }

    while (!state->remainingTasks.empty() &&
        boundTasksThisFrame < kRendererResourceResolutionBindTasksPerFrame &&
        meshBindsThisFrame < kRendererResourceResolutionMeshBindsPerFrame &&
        scheduledTasksThisFrame < kRendererResourceResolutionScheduleTasksPerFrame)
    {
        reportTaskProgress();

        auto nextTask = PopNextRemainingTask(
            state->remainingTasks,
            state->inFlightTasks.size() >= kRendererResourceResolutionMaxInflightMeshLoads);
        if (!nextTask.has_value())
            break;

        auto task = std::move(*nextTask);

        if (task.kind == RendererResourceResolutionTaskKind::Mesh)
        {
            if (!IsMeshTaskAlreadyCached(task) && StartMeshArtifactLoad(actions, task, state->meshLoadsByPath))
                ++scheduledTasksThisFrame;
        }

        const bool completed = RunRendererResourceResolutionTask(
            actions,
            *liveInstance,
            task,
            *liveObjectsBySourceId,
            state->stats,
            frameBudgetExpired);
        if (completed)
        {
            if (task.kind == RendererResourceResolutionTaskKind::Mesh)
            {
                ++meshBindsThisFrame;
                if (task.failed || (task.meshLoad && !task.meshLoad->accepted))
                    state->failed = true;
            }
            ++state->completedTasks;
            ++boundTasksThisFrame;
        }
        else
        {
            if (task.kind == RendererResourceResolutionTaskKind::Mesh && task.meshLoad)
            {
                state->inFlightTasks.push_back(std::move(task));
            }
            else
            {
                state->remainingTasks.push_front(std::move(task));
                break;
            }
        }

        if (frameBudgetExpired())
        {
            scheduleNextStep();
            return;
        }
    }

    if (!state->remainingTasks.empty() || !state->inFlightTasks.empty())
    {
        if (state->failed)
        {
            finishFailed();
            return;
        }

        scheduleNextStep();
        return;
    }

    NLS_LOG_INFO(
        "Renderer resources ready for prefab instance: " +
        std::to_string(state->totalTasks) +
        " tasks" +
        (state->stats
            ? " mesh=" +
                std::to_string(state->stats->boundMeshTasks) +
                "/" +
                std::to_string(state->stats->scheduledMeshTasks) +
                " failedMesh=" +
                std::to_string(state->stats->failedMeshTasks) +
                " materialTasks=" +
                std::to_string(state->stats->completedMaterialTasks) +
                "/" +
                std::to_string(state->stats->scheduledMaterialTasks) +
                " materialSlots=" +
                std::to_string(state->stats->boundMaterialSlots) +
                " unresolvedMaterialSlots=" +
                std::to_string(state->stats->unresolvedMaterialSlots) +
                " failedMaterial=" +
                std::to_string(state->stats->failedMaterialSlots)
            : std::string {}));
    if (state->failed ||
        (state->stats &&
            (state->stats->failedMaterialSlots > 0u ||
                state->stats->unresolvedMaterialSlots > 0u)))
    {
        finishFailed();
        return;
    }

    actions.ReleaseGameObjectDestroyedListener(state->destroyedListener);
    state->destroyedListener = InvalidListenerID;
    RestoreRendererResourceResolutionRootVisibility(*state);
    tracker.ReportProgress(state->job, NLS::Editor::Assets::ImportPhase::Postprocess, 1.0, "Renderer resources ready");
    tracker.FinishJob(state->job, NLS::Editor::Assets::ImportJobTerminalStatus::Succeeded, {});
}
}

Editor::Core::EditorActions::EditorActions(Context& p_context, PanelsManager& p_panelsManager)
    : m_context(p_context)
    , m_panelsManager(p_panelsManager)
    , m_backgroundTasks(kEditorBackgroundTaskQueueCapacity)
{
    NLS::Core::ServiceLocator::Provide<Editor::Core::EditorActions>(*this);

    m_sceneSourcePathChangedListener = m_context.sceneManager.CurrentSceneSourcePathChangedEvent += [this](const std::string& p_newPath)
    {
        (void)p_newPath;
        RefreshWindowTitle();
    };

    m_sceneDirtyStateChangedListener = m_context.sceneManager.CurrentSceneDirtyStateChangedEvent += [this](bool p_dirty)
    {
        (void)p_dirty;
        RefreshWindowTitle();
    };

    m_sceneLoadListener = m_context.sceneManager.SceneLoadEvent += [this]
    {
        ++m_mainSceneGeneration;
    };
    m_sceneUnloadListener = m_context.sceneManager.SceneUnloadEvent += [this]
    {
        ++m_mainSceneGeneration;
    };
}

Editor::Core::EditorActions::~EditorActions()
{
    if (m_sceneSourcePathChangedListener != InvalidListenerID)
        m_context.sceneManager.CurrentSceneSourcePathChangedEvent -= m_sceneSourcePathChangedListener;
    if (m_sceneDirtyStateChangedListener != InvalidListenerID)
        m_context.sceneManager.CurrentSceneDirtyStateChangedEvent -= m_sceneDirtyStateChangedListener;
    if (m_sceneLoadListener != InvalidListenerID)
        m_context.sceneManager.SceneLoadEvent -= m_sceneLoadListener;
    if (m_sceneUnloadListener != InvalidListenerID)
        m_context.sceneManager.SceneUnloadEvent -= m_sceneUnloadListener;

    m_backgroundTasks.StopAndComplete();

    {
        std::lock_guard lock(m_delayedActionsMutex);
        m_delayedActions.clear();
    }
    ReleaseTrackedGameObjectDestroyedListeners();
}

void Editor::Core::EditorActions::LoadEmptyScene()
{
    if (GetCurrentEditorMode() != EEditorMode::EDIT)
        StopPlaying();

    if (!PromptSaveCurrentSceneIfDirty())
        return;

    m_context.sceneManager.LoadEmptyLightedScene();
    m_context.sceneManager.MarkCurrentSceneDirty();
    NLS_LOG_INFO("New scene created");
}

bool Editor::Core::EditorActions::SaveCurrentSceneTo(const std::string& p_path)
{
    std::filesystem::path scenePath(p_path);
    if (scenePath.extension() != ".scene")
        scenePath += ".scene";

    const auto currentScene = m_context.sceneManager.GetCurrentScene();
    if (!currentScene)
        return false;

    auto document = NLS::Engine::Serialize::ObjectGraphSerializer::SerializeScene(*currentScene);
    NLS::Editor::Assets::PrefabUtilityFacade().AnnotateSceneDocumentWithPrefabInstances(
        document,
        *currentScene,
        m_context.prefabInstanceRegistry);
    if (!document.Validate().HasErrors() &&
        WriteTextFileAtomicallyAtPath(scenePath, NLS::Engine::Serialize::ObjectGraphWriter::Write(document)))
    {
        m_context.sceneManager.StoreCurrentSceneSourcePath(scenePath.string());
        m_context.sceneManager.MarkCurrentSceneClean();
        DelayAction([this]
        {
            m_panelsManager.GetPanelAs<Panels::AssetBrowser>("Asset Browser").Refresh();
        });
        return true;
    }

    if (!p_path.empty())
        NLS_LOG_ERROR("Failed to save current scene to: " + scenePath.string());
    return false;
}

void Editor::Core::EditorActions::LoadSceneFromDisk(const std::string& p_path, bool p_absolute)
{
    if (GetCurrentEditorMode() != EEditorMode::EDIT)
        StopPlaying();

    if (!PromptSaveCurrentSceneIfDirty())
        return;

    if (!m_context.sceneManager.LoadScene(p_path, p_absolute))
    {
        Dialogs::MessageBox message(
            "Scene loading failed",
            "The scene you are trying to load was not found or could not be read.",
            Dialogs::MessageBox::EMessageType::ERROR,
            Dialogs::MessageBox::EButtonLayout::OK);
        return;
    }

    m_context.prefabInstanceRegistry.Clear();
    const auto sceneText = ReadTextFileAtPath(m_context.sceneManager.GetCurrentSceneSourcePath());
    if (sceneText.has_value())
    {
        const auto document = NLS::Engine::Serialize::ObjectGraphReader::Read(*sceneText);
        if (document.has_value() && m_context.sceneManager.GetCurrentScene())
        {
            NLS::Editor::Assets::AssetDatabaseFacade prefabDatabase({
                std::filesystem::path(m_context.projectPath)
            });
            const auto prefabDatabaseReady = prefabDatabase.Refresh();
            std::unordered_map<std::string, std::optional<NLS::Engine::Assets::PrefabArtifact>> prefabArtifactCache;

            auto restoreResult = NLS::Editor::Assets::PrefabUtilityFacade().RestorePrefabInstancesFromSceneDocument(
                *document,
                *m_context.sceneManager.GetCurrentScene(),
                {},
                m_context.prefabInstanceRegistry,
                [&prefabDatabase, prefabDatabaseReady, &prefabArtifactCache](
                    NLS::Core::Assets::AssetId assetId,
                    const std::string& subAssetKey)
                    -> std::optional<NLS::Engine::Assets::PrefabArtifact>
                {
                    if (!prefabDatabaseReady)
                        return std::nullopt;

                    const auto cacheKey = assetId.ToString() + "\n" + subAssetKey;
                    const auto cached = prefabArtifactCache.find(cacheKey);
                    if (cached != prefabArtifactCache.end())
                        return cached->second;

                    const auto assetPath = prefabDatabase.GUIDToAssetPath(assetId.ToString());
                    if (assetPath.empty())
                    {
                        prefabArtifactCache.emplace(cacheKey, std::nullopt);
                        return std::nullopt;
                    }

                    auto artifact = prefabDatabase.LoadPrefabArtifactAtPath(assetPath, subAssetKey);
                    prefabArtifactCache.emplace(cacheKey, artifact);
                    return artifact;
                });
            if (restoreResult.status != NLS::Editor::Assets::PrefabOperationStatus::Committed)
            {
                NLS_LOG_WARNING(
                    "Scene loaded with missing or unresolved prefab instance metadata: " +
                    m_context.sceneManager.GetCurrentSceneSourcePath());
                for (const auto& diagnostic : restoreResult.diagnostics)
                {
                    NLS_LOG_WARNING(
                        "Scene prefab restore diagnostic code=" +
                        diagnostic.code +
                        " message=" +
                        diagnostic.message);
                }
            }
        }
    }

    NLS_LOG_INFO("Scene loaded from disk: " + m_context.sceneManager.GetCurrentSceneSourcePath());
    m_panelsManager.GetPanelAs<Editor::Panels::SceneView>("Scene View").Focus();
}

bool Editor::Core::EditorActions::IsCurrentSceneLoadedFromDisk() const
{
    return m_context.sceneManager.IsCurrentSceneLoadedFromDisk();
}

void Editor::Core::EditorActions::SaveSceneChanges()
{
    if (SaveActivePrefabStage())
        return;

    if (IsCurrentSceneLoadedFromDisk())
    {
        const auto scenePath = m_context.sceneManager.GetCurrentSceneSourcePath();
        if (SaveCurrentSceneTo(scenePath))
            NLS_LOG_INFO("Current scene saved to: " + scenePath);
    }
    else
    {
        SaveAs();
    }
}

bool Editor::Core::EditorActions::SaveActivePrefabStage()
{
    if (!m_context.activePrefabStage.has_value())
        return false;

    auto& stage = *m_context.activePrefabStage;
    if (!stage.loaded)
    {
        ++m_prefabStageGeneration;
        m_context.activePrefabStage.reset();
        return false;
    }

    const auto projectRoot = ProjectRootFromAssetsPath(m_context.projectAssetsPath);
    Assets::AssetDatabaseFacade database(Assets::MakeProjectEditorAssetRoots(projectRoot));
    if (!database.Refresh())
    {
        NLS_LOG_ERROR("Failed to refresh asset database before saving prefab stage.");
        return true;
    }

    auto assetPath = stage.prefabAssetPath;
    if (assetPath.empty())
        assetPath = database.GUIDToAssetPath(stage.prefabAssetId.ToString());

    auto prefab = assetPath.empty()
        ? std::optional<Engine::Assets::PrefabArtifact>{}
        : database.LoadPrefabArtifactAtPath(assetPath, stage.prefabSubAssetKey);

    if (!prefab.has_value())
    {
        NLS_LOG_ERROR("Failed to load prefab artifact for active prefab stage save.");
        return true;
    }

    auto result = Assets::PrefabUtilityFacade().SavePrefabContents(
        stage,
        *prefab,
        &m_context.prefabInstanceRegistry);
    if (result.status != Assets::PrefabOperationStatus::Committed)
    {
        for (const auto& diagnostic : result.diagnostics)
            NLS_LOG_ERROR(diagnostic.code + ": " + diagnostic.message);
        return true;
    }

    NLS_LOG_INFO("Active prefab stage saved.");
    if (!assetPath.empty() && !result.prefabSourceText.empty())
    {
        if (!RewriteTextAssetPreservingMeta(database, projectRoot, assetPath, result.prefabSourceText))
        {
            stage.dirty = true;
            NLS_LOG_ERROR("Failed to write active prefab stage source asset: " + assetPath);
        }
    }

    m_context.sceneManager.MarkCurrentSceneDirty();
    return true;
}

bool Editor::Core::EditorActions::CloseActivePrefabStage(bool saveBeforeClose)
{
    if (!m_context.activePrefabStage.has_value())
        return true;

    if (saveBeforeClose && !SaveActivePrefabStage())
        return false;

    ++m_prefabStageGeneration;
    m_context.activePrefabStage.reset();
    m_panelsManager.GetPanelAs<Panels::Hierarchy>("Hierarchy").RebuildFromCurrentScene();
    NLS_LOG_INFO("Closed prefab stage.");
    return true;
}

void Editor::Core::EditorActions::SaveAs()
{
    Dialogs::SaveFileDialog dialog("New Scene", m_context.projectAssetsPath + "New Scene", {"Nullus Scene", "*.scene"});

    if (!dialog.Result().empty())
    {
        if (std::filesystem::exists(dialog.Result()))
        {
            Dialogs::MessageBox message("File already exists!", "The file \"" + dialog.Result() + "\" already exists.\n\nUsing this file as the new home for your scene will erase any content stored in this file.\n\nAre you ok with that?", Dialogs::MessageBox::EMessageType::WARNING, Dialogs::MessageBox::EButtonLayout::YES_NO);
            switch (message.GetUserAction())
            {
                case Dialogs::MessageBox::EUserAction::YES:
                    break;
                case Dialogs::MessageBox::EUserAction::NO:
                    return;
            }
        }

        if (SaveCurrentSceneTo(dialog.Result()))
            NLS_LOG_INFO("Current scene saved to: " + dialog.Result());
    }
}

bool Editor::Core::EditorActions::PromptSaveCurrentSceneIfDirty()
{
    if (!m_context.sceneManager.HasUnsavedSceneChanges())
        return true;

    Dialogs::MessageBox message(
        "Unsaved Scene Changes",
        "The current scene has unsaved changes.\n\nDo you want to save them before continuing?",
        Dialogs::MessageBox::EMessageType::QUESTION,
        Dialogs::MessageBox::EButtonLayout::YES_NO_CANCEL);

    switch (message.GetUserAction())
    {
        case Dialogs::MessageBox::EUserAction::YES:
            SaveSceneChanges();
            return !m_context.sceneManager.HasUnsavedSceneChanges();
        case Dialogs::MessageBox::EUserAction::NO:
            return true;
        case Dialogs::MessageBox::EUserAction::CANCEL:
        default:
            return false;
    }
}


std::optional<std::string> Editor::Core::EditorActions::SelectBuildFolder()
{
    Dialogs::SaveFileDialog dialog("Build location", "", {"Game Build", ".."});
    if (!dialog.Result().empty())
    {
        std::string result = dialog.Result();
        result = std::string(result.data(), result.data() + result.size() - std::string("..").size()) + "\\"; // remove auto extension
        if (!std::filesystem::exists(result))
            return result;
        else
        {
            Dialogs::MessageBox message("Folder already exists!", "The folder \"" + result + "\" already exists.\n\nPlease select another location and try again", Dialogs::MessageBox::EMessageType::WARNING, Dialogs::MessageBox::EButtonLayout::OK);
            return {};
        }
    }
    else
    {
        return {};
    }
}

void Editor::Core::EditorActions::Build(bool p_autoRun, bool p_tempFolder)
{
    std::string destinationFolder;

    if (p_tempFolder)
    {
        destinationFolder = "TempBuild";
        try
        {
            std::filesystem::remove_all(destinationFolder);
        }
        catch (std::filesystem::filesystem_error error)
        {
            Dialogs::MessageBox message("Temporary build failed", "The temporary folder is currently being used by another process", Dialogs::MessageBox::EMessageType::ERROR, Dialogs::MessageBox::EButtonLayout::OK);
            return;
        }
    }
    else if (auto res = SelectBuildFolder(); res.has_value())
        destinationFolder = res.value();
    else
        return; // Operation cancelled (No folder selected)

    BuildAtLocation(m_context.projectSettings.Get<bool>("dev_build") ? "Development" : "Shipping", destinationFolder, p_autoRun);
}

void Editor::Core::EditorActions::BuildAtLocation(const std::string& p_configuration, const std::string p_buildPath, bool p_autoRun)
{
    std::string buildPath(p_buildPath);
    std::string executableName = m_context.projectSettings.Get<std::string>("executable_name") + ".exe";

    bool failed = false;

    NLS_LOG_INFO("Preparing to build at location: \"" + buildPath + "\"");

    std::filesystem::remove_all(buildPath);

    if (std::filesystem::create_directory(buildPath))
    {
        NLS_LOG_INFO("Build directory created");

        if (std::filesystem::create_directory(buildPath + "Data/"))
        {
            NLS_LOG_INFO("Data directory created");

            if (std::filesystem::create_directory(buildPath + "Data/User/"))
            {
                NLS_LOG_INFO("Data/User directory created");

                std::error_code err;

                std::filesystem::copy(m_context.projectFilePath, buildPath + "Data/User/Game.ini", err);

                if (!err)
                {
                    NLS_LOG_INFO("Data/User/Game.ini file generated");

                    std::filesystem::copy(m_context.projectAssetsPath, buildPath + "Data/User/Assets/", std::filesystem::copy_options::recursive, err);

                    if (!std::filesystem::exists(buildPath + "Data/User/Assets/" + (m_context.projectSettings.Get<std::string>("start_scene"))))
                    {
                        NLS_LOG_ERROR("Failed to find Start Scene at expected path. Verify your Project Setings.");
                        Dialogs::MessageBox message("Build Failure", "An error occured during the building of your game.\nCheck the console for more information", Dialogs::MessageBox::EMessageType::ERROR, Dialogs::MessageBox::EButtonLayout::OK);
                        std::filesystem::remove_all(buildPath);
                        return;
                    }

                    if (!err)
                    {
                        NLS_LOG_INFO("Data/User/Assets/ directory copied");

                        std::filesystem::copy(m_context.engineAssetsPath, buildPath + "Data/Engine/", std::filesystem::copy_options::recursive, err);

                        if (!err)
                        {
                            NLS_LOG_INFO("Data/Engine/ directory copied");
                        }
                        else
                        {
                            NLS_LOG_INFO("Data/Engine/ directory failed to copy");
                            failed = true;
                        }
                    }
                    else
                    {
                        NLS_LOG_ERROR("Data/User/Assets/ directory failed to copy");
                        failed = true;
                    }
                }
                else
                {
                    NLS_LOG_ERROR("Data/User/Game.ini file failed to generate");
                    failed = true;
                }

                std::string builderFolder = "Builder\\" + p_configuration + "\\";

                if (std::filesystem::exists(builderFolder))
                {
                    std::error_code err;

                    std::filesystem::copy(builderFolder, buildPath, err);

                    if (!err)
                    {
                        NLS_LOG_INFO("Builder data (Dlls and executatble) copied");

                        std::filesystem::rename(buildPath + "OvGame.exe", buildPath + executableName, err);

                        if (!err)
                        {
                            NLS_LOG_INFO("Game executable renamed to " + executableName);

                            if (p_autoRun)
                            {
                                std::string exePath = buildPath + executableName;
                                NLS_LOG_INFO("Launching the game at location: \"" + exePath + "\"");
                                if (std::filesystem::exists(exePath))
                                    Platform::SystemCalls::OpenFile(exePath, buildPath);
                                else
                                {
                                    NLS_LOG_INFO("Failed to start the game: Executable not found");
                                    failed = true;
                                }
                            }
                        }
                        else
                        {
                            NLS_LOG_ERROR("Game executable failed to rename");
                            failed = true;
                        }
                    }
                    else
                    {
                        NLS_LOG_ERROR("Builder data (Dlls and executatble) failed to copy");
                        failed = true;
                    }
                }
                else
                {
                    const std::string buildConfiguration = p_configuration == "Development" ? "Debug" : "Release";
                    NLS_LOG_ERROR("Builder folder for \"" + p_configuration + "\" not found. Verify you have compiled Engine source code in '" + buildConfiguration + "' configuration.");
                    failed = true;
                }
            }
        }
    }
    else
    {
        NLS_LOG_ERROR("Build directory failed to create");
        failed = true;
    }

    if (failed)
    {
        std::filesystem::remove_all(buildPath);
        Dialogs::MessageBox message("Build Failure", "An error occured during the building of your game.\nCheck the console for more information", Dialogs::MessageBox::EMessageType::ERROR, Dialogs::MessageBox::EButtonLayout::OK);
    }
}

void Editor::Core::EditorActions::DelayAction(std::function<void()> p_action, uint32_t p_frames)
{
    std::lock_guard lock(m_delayedActionsMutex);
    m_delayedActions.emplace_back(p_frames + 1, std::move(p_action));
}

Editor::Core::EditorActions::SceneMutationToken Editor::Core::EditorActions::CaptureSceneMutationToken() const
{
    return {m_mainSceneGeneration, m_prefabStageGeneration};
}

void Editor::Core::EditorActions::NotifyPrefabStageOpened()
{
    ++m_prefabStageGeneration;
}

ListenerID Editor::Core::EditorActions::TrackGameObjectDestroyedListener(
    std::function<void(Engine::GameObject&)> callback)
{
    if (!callback)
        return InvalidListenerID;

    const auto listener = Engine::GameObject::DestroyedEvent += std::move(callback);
    {
        std::lock_guard lock(m_gameObjectDestroyedListenersMutex);
        m_gameObjectDestroyedListeners.push_back(listener);
    }
    return listener;
}

void Editor::Core::EditorActions::ReleaseGameObjectDestroyedListener(ListenerID listener)
{
    if (listener == InvalidListenerID)
        return;

    bool tracked = false;
    {
        std::lock_guard lock(m_gameObjectDestroyedListenersMutex);
        const auto found = std::find(
            m_gameObjectDestroyedListeners.begin(),
            m_gameObjectDestroyedListeners.end(),
            listener);
        if (found != m_gameObjectDestroyedListeners.end())
        {
            m_gameObjectDestroyedListeners.erase(found);
            tracked = true;
        }
    }

    if (tracked)
        Engine::GameObject::DestroyedEvent -= listener;
}

void Editor::Core::EditorActions::ReleaseTrackedGameObjectDestroyedListeners()
{
    std::vector<ListenerID> listeners;
    {
        std::lock_guard lock(m_gameObjectDestroyedListenersMutex);
        listeners = std::move(m_gameObjectDestroyedListeners);
        m_gameObjectDestroyedListeners.clear();
    }

    for (const auto listener : listeners)
    {
        if (listener != InvalidListenerID)
            Engine::GameObject::DestroyedEvent -= listener;
    }
}

bool Editor::Core::EditorActions::TrackBackgroundTask(std::function<void()> task)
{
    return m_backgroundTasks.Track(std::move(task));
}

void Editor::Core::EditorActions::ExecuteDelayedActions()
{
    std::vector<std::pair<uint32_t, std::function<void()>>> pendingActions;
    {
        std::lock_guard lock(m_delayedActionsMutex);
        pendingActions = std::move(m_delayedActions);
        m_delayedActions.clear();
    }

    std::vector<std::pair<uint32_t, std::function<void()>>> remainingActions;
    for (auto& action : pendingActions)
    {
        if (action.first > 0u)
            --action.first;

        if (action.first == 0u)
            action.second();
        else
            remainingActions.push_back(std::move(action));
    }

    if (!remainingActions.empty())
    {
        std::lock_guard lock(m_delayedActionsMutex);
        m_delayedActions.insert(
            m_delayedActions.end(),
            std::make_move_iterator(remainingActions.begin()),
            std::make_move_iterator(remainingActions.end()));
    }
}

Editor::Core::Context& Editor::Core::EditorActions::GetContext()
{
    return m_context;
}

Editor::Core::PanelsManager& Editor::Core::EditorActions::GetPanelsManager()
{
    return m_panelsManager;
}

void Editor::Core::EditorActions::SetGameObjectSpawnAtOrigin(bool p_value)
{
    if (p_value)
        m_gameObjectSpawnMode = EGameObjectSpawnMode::ORIGIN;
    else
        m_gameObjectSpawnMode = EGameObjectSpawnMode::FRONT;
}

void Editor::Core::EditorActions::SetGameObjectSpawnMode(EGameObjectSpawnMode p_value)
{
    m_gameObjectSpawnMode = p_value;
}

void Editor::Core::EditorActions::ResetLayout()
{
    DelayAction([this]()
                { m_context.uiManager->ResetLayout(m_context.editorAssetsPath + "/Settings/layout.ini"); });
}

void Editor::Core::EditorActions::ApplyLayoutPreset(const std::string& p_presetId)
{
    DelayAction([this, p_presetId]()
    {
        const std::filesystem::path layoutPath =
            std::filesystem::path(m_context.editorAssetsPath) / "Settings" / ("layout_" + p_presetId + ".ini");
        if (!std::filesystem::exists(layoutPath))
        {
            NLS_LOG_WARNING("Editor layout preset not found: " + layoutPath.string());
            return;
        }

        m_panelsManager.GetPanelAs<Panels::AssetBrowser>("Asset Browser").Open();
        m_panelsManager.GetPanelAs<Panels::AssetView>("Asset View").Close();
        m_panelsManager.GetPanelAs<Panels::Console>("Console").Close();
        m_panelsManager.GetPanelAs<Panels::FrameInfo>("Frame Info").Close();
        m_panelsManager.GetPanelAs<Panels::Hierarchy>("Hierarchy").Open();
        m_panelsManager.GetPanelAs<Panels::Inspector>("Inspector").Open();
        m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View").Open();
        m_panelsManager.GetPanelAs<Panels::GameView>("Game View").Open();

        m_context.uiManager->ResetLayout(layoutPath.string());
        NLS_LOG_INFO("Applied editor layout preset: " + p_presetId);
    });
}

void Editor::Core::EditorActions::SetSceneViewCameraSpeed(int p_speed)
{
    // EDITOR_PANEL(Panels::SceneView, "Scene View").GetCameraController().SetSpeed((float)p_speed);
}

int Editor::Core::EditorActions::GetSceneViewCameraSpeed()
{
    // return (int)EDITOR_PANEL(Panels::SceneView, "Scene View").GetCameraController().GetSpeed();
    return 0;
}

void Editor::Core::EditorActions::SetAssetViewCameraSpeed(int p_speed)
{
    // EDITOR_PANEL(Panels::AssetView, "Asset View").GetCameraController().SetSpeed((float)p_speed);
}

int Editor::Core::EditorActions::GetAssetViewCameraSpeed()
{
    // return (int)EDITOR_PANEL(Panels::AssetView, "Asset View").GetCameraController().GetSpeed();
    return 0;
}

void Editor::Core::EditorActions::ResetSceneViewCameraPosition()
{
    // EDITOR_PANEL(Panels::SceneView, "Scene View").ResetCameraTransform();
}

void Editor::Core::EditorActions::ResetAssetViewCameraPosition()
{
    // EDITOR_PANEL(Panels::AssetView, "Asset View").ResetCameraTransform();
}

Editor::Core::EditorActions::EEditorMode Editor::Core::EditorActions::GetCurrentEditorMode() const
{
    return m_editorMode;
}

void Editor::Core::EditorActions::SetEditorMode(EEditorMode p_newEditorMode)
{
    m_editorMode = p_newEditorMode;
    EditorModeChangedEvent.Invoke(m_editorMode);
}

void Editor::Core::EditorActions::StartPlaying()
{
    if (m_editorMode == EEditorMode::EDIT)
    {
        // 		m_context.scriptInterpreter->RefreshAll();
        // 		EDITOR_PANEL(Panels::Inspector, "Inspector").Refresh();
        //
        // 		if (m_context.scriptInterpreter->IsOk())
        // 		{
        // 			PlayEvent.Invoke();
        // 			m_sceneBackup.Clear();
        // 			tinyxml2::XMLNode* node = m_sceneBackup.NewElement("root");
        // 			m_sceneBackup.InsertFirstChild(node);
        // 			m_context.sceneManager.GetCurrentScene()->OnSerialize(m_sceneBackup, node);
        // 			m_panelsManager.GetPanelAs<Editor::Panels::GameView>("Game View").Focus();
        // 			m_context.sceneManager.GetCurrentScene()->Play();
        // 			SetEditorMode(EEditorMode::PLAY);
        // 		}
    }
    else
    {
        // m_context.audioEngine->Unsuspend();
        SetEditorMode(EEditorMode::PLAY);
    }
}

void Editor::Core::EditorActions::PauseGame()
{
    SetEditorMode(EEditorMode::PAUSE);
    // m_context.audioEngine->Suspend();
}

void Editor::Core::EditorActions::StopPlaying()
{
    // 	if (m_editorMode != EEditorMode::EDIT)
    // 	{
    // 		ImGui::GetIO().DisableMouseUpdate = false;
    // 		m_context.window->SetCursorMode(Windowing::Cursor::ECursorMode::NORMAL);
    // 		SetEditorMode(EEditorMode::EDIT);
    // 		bool loadedFromDisk = m_context.sceneManager.IsCurrentSceneLoadedFromDisk();
    // 		std::string sceneSourcePath = m_context.sceneManager.GetCurrentSceneSourcePath();
    //
    // 		int64_t focusedActorID = -1;
    //
    // 		if (auto targetActor = EDITOR_PANEL(Panels::Inspector, "Inspector").GetTargetGameObject())
    // 			focusedActorID = targetActor->GetID();
    //
    // 		m_context.sceneManager.LoadSceneFromMemory(m_sceneBackup);
    // 		if (loadedFromDisk)
    // 			m_context.sceneManager.StoreCurrentSceneSourcePath(sceneSourcePath); // To bo able to save or reload the scene whereas the scene is loaded from memory (Supposed to have no path)
    // 		m_sceneBackup.Clear();
    // 		EDITOR_PANEL(Panels::SceneView, "Scene View").Focus();
    // 		if (auto actorInstance = m_context.sceneManager.GetCurrentScene()->FindGameObjectByName(focusedActorName))
    // 			EDITOR_PANEL(Panels::Inspector, "Inspector").FocusGameObject(*actorInstance);
    // 	}
}

void Editor::Core::EditorActions::NextFrame()
{
    if (m_editorMode == EEditorMode::PLAY || m_editorMode == EEditorMode::PAUSE)
        SetEditorMode(EEditorMode::FRAME_BY_FRAME);
}

Maths::Vector3 Editor::Core::EditorActions::CalculateGameObjectSpawnPoint(float p_distanceToCamera)
{
    auto& sceneView = m_panelsManager.GetPanelAs<Editor::Panels::SceneView>("Scene View");

    if (auto camera = sceneView.GetCamera(); camera != nullptr && camera->transform != nullptr)
    {
        return camera->GetPosition() + camera->transform->GetWorldForward() * p_distanceToCamera;
    }

    return Maths::Vector3::Zero;
}

Engine::GameObject* Editor::Core::EditorActions::CreateEmptyGameObject(bool p_focusOnCreation, Engine::GameObject* p_parent, const std::string& p_name)
{
    auto* creationScene = ResolveGameObjectCreationScene(m_context, p_parent);
    if (!creationScene)
    {
        NLS_LOG_ERROR("Failed to create GameObject without an editable active scene");
        return nullptr;
    }

    auto& instance = p_name.empty() ? creationScene->CreateGameObject() : creationScene->CreateGameObject(p_name);

    if (p_parent)
        instance.SetParent(*p_parent);

    if (m_gameObjectSpawnMode == EGameObjectSpawnMode::FRONT)
        instance.GetTransform()->SetLocalPosition(CalculateGameObjectSpawnPoint(10.0f));

    if (p_focusOnCreation)
        SelectGameObject(instance);

    NLS_LOG_INFO("GameObject created");
    MarkGameObjectCreationSceneDirty(m_context, *creationScene);

    return &instance;
}

Engine::GameObject* Editor::Core::EditorActions::CreateGameObjectWithModel(const std::string& p_path, bool p_focusOnCreation, Engine::GameObject* p_parent, const std::string& p_name)
{
    if (const auto primitiveType = NLS::Engine::TryGetPrimitiveTypeFromMeshResourcePath(p_path))
        return CreatePrimitive(*primitiveType, p_focusOnCreation, p_parent);

    auto* instance = CreateEmptyGameObject(false, p_parent, p_name);
    if (!instance)
        return nullptr;

    auto meshFilter = instance->AddComponent<Engine::Components::MeshFilter>();
    auto meshRenderer = instance->AddComponent<Engine::Components::MeshRenderer>();

    meshFilter->SetMeshPath(p_path);

    const auto material = GetOrCreateEditorDefaultMaterial(m_context);
    if (material)
        meshRenderer->FillWithMaterial(*material);

    if (p_focusOnCreation)
        SelectGameObject(*instance);

    return instance;
}

Engine::GameObject* Editor::Core::EditorActions::CreatePrimitive(
    const Engine::PrimitiveType p_type,
    const bool p_focusOnCreation,
    Engine::GameObject* p_parent)
{
    auto* instance = CreateEmptyGameObject(false, p_parent, NLS::Engine::GetPrimitiveName(p_type));
    if (!instance)
        return nullptr;

    const auto material = GetOrCreateEditorDefaultMaterial(m_context);
    NLS::Engine::ConfigurePrimitiveGameObject(*instance, p_type, material);

    if (p_focusOnCreation)
        SelectGameObject(*instance);

    return instance;
}

Engine::GameObject* NLS::Editor::Core::EditorActions::CreateGameObjectFromAsset(
    const std::string& path,
    bool focusOnCreation,
    Engine::GameObject* p_parent,
    std::optional<Maths::Vector3> placementOverride)
{
    auto* creationScene = ResolveGameObjectCreationScene(m_context, p_parent);
    if (!creationScene)
    {
        NLS_LOG_ERROR("Failed to create GameObject from asset without an active scene: " + path);
        return nullptr;
    }

    if (NLS::Editor::Assets::IsBuiltInResourcePath(path))
    {
        NLS_LOG_INFO("Creating built-in model GameObject: " + path);
        if (const auto primitiveType = NLS::Engine::TryGetPrimitiveTypeFromMeshResourcePath(path))
            return CreatePrimitive(*primitiveType, focusOnCreation, p_parent);
        return nullptr;
    }

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(m_context.projectAssetsPath);
    const auto sceneToken = CaptureSceneMutationToken();
    PendingAssetDropParentGuard parentGuard;
    parentGuard.parentAddress = static_cast<const Engine::GameObject*>(p_parent);
    if (p_parent)
    {
        parentGuard.parentDestroyed = std::make_shared<bool>(false);
        const auto* watchedParent = parentGuard.parentAddress;
        parentGuard.destroyedListener = TrackGameObjectDestroyedListener(
            [destroyed = parentGuard.parentDestroyed, watchedParent](Engine::GameObject& destroyedObject)
            {
                if (&destroyedObject == watchedParent)
                    *destroyed = true;
            });
    }
    auto result = bridge.DropModelAssetIntoHierarchyAsync(
        path,
        *creationScene,
        {
            {},
            &m_context.prefabInstanceRegistry,
            p_parent,
            &m_context.importProgressTracker,
            [this, path, focusOnCreation, creationScene, sceneToken, parentGuard](NLS::Editor::Assets::EditorAssetDragDropBridgeResult importResult)
            {
                DelayAction(
                    [this, path, focusOnCreation, creationScene, sceneToken, parentGuard, importResult = std::move(importResult)]() mutable
                    {
                        CompletePendingAssetDrop(path, focusOnCreation, creationScene, sceneToken, parentGuard, std::move(importResult));
                    },
                    1);
            },
            [this](std::function<void()> task)
            {
                return TrackBackgroundTask(std::move(task));
            }
        });
    if (result.pendingImport)
    {
        NLS_LOG_INFO("Asset is not imported yet; background preimport must finish before creating GameObject: " + path);
        return nullptr;
    }
    ReleaseGameObjectDestroyedListener(parentGuard.destroyedListener);
    parentGuard.destroyedListener = InvalidListenerID;
    if (!result.handled || result.dragDrop.status != NLS::Editor::Assets::DragDropOperationStatus::Committed ||
        !result.dragDrop.instance.has_value() || !result.dragDrop.instance->instanceRoot)
    {
        NLS_LOG_ERROR("Failed to create GameObject from asset through asset pipeline: " + path);
        return nullptr;
    }

    auto& instance = *result.dragDrop.instance->instanceRoot;
    NLS_LOG_INFO("Created GameObject from asset: " + path + " root=" + instance.GetName());

    if (placementOverride.has_value())
        instance.GetTransform()->SetWorldPosition(*placementOverride);
    else if (m_gameObjectSpawnMode == EGameObjectSpawnMode::FRONT)
        instance.GetTransform()->SetLocalPosition(CalculateGameObjectSpawnPoint(10.0f));

    if (result.dragDrop.deferredAssetReferenceResolutionRequested)
    {
        QueuePrefabInstanceAssetResolution(
            result.dragDrop.instance ? &*result.dragDrop.instance : nullptr,
            result.dragDrop.artifact ? &*result.dragDrop.artifact : nullptr,
            path);
    }
    if (focusOnCreation)
        SelectGameObject(instance);
    MarkGameObjectCreationSceneDirty(m_context, *creationScene);
    return &instance;
}

Engine::GameObject* NLS::Editor::Core::EditorActions::CreateGameObjectFromAsset(
    const NLS::Editor::Assets::EditorAssetDragPayload& payload,
    bool focusOnCreation,
    Engine::GameObject* p_parent,
    std::optional<Maths::Vector3> placementOverride)
{
    auto* creationScene = ResolveGameObjectCreationScene(m_context, p_parent);
    const auto path = NLS::Editor::Assets::GetEditorAssetDragPayloadPath(payload);
    if (!creationScene)
    {
        NLS_LOG_ERROR("Failed to create GameObject from asset without an active scene: " + path);
        return nullptr;
    }

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(m_context.projectAssetsPath);
    auto result = bridge.DropImportedAssetHandleIntoHierarchy(
        payload,
        *creationScene,
        {},
        &m_context.prefabInstanceRegistry,
        p_parent,
        &m_context.importProgressTracker);

    if (result.pendingImport)
    {
        if (payload.imported != 0u)
        {
            NLS_LOG_ERROR(
                "Imported asset drag handle could not resolve a committed prefab artifact without reimport: " +
                path +
                " payloadSubAssetKey=" +
                NLS::Editor::Assets::GetEditorAssetDragPayloadSubAssetKey(payload) +
                " generatedModelPrefab=" +
                std::to_string(payload.generatedModelPrefab) +
                " imported=" +
                std::to_string(payload.imported) +
                " dragDropStatus=" +
                DragDropOperationStatusLabel(result.dragDrop.status));
            for (const auto& diagnostic : result.dragDrop.diagnostics)
            {
                NLS_LOG_ERROR(
                    "Imported asset drag diagnostic code=" +
                    diagnostic.code +
                    " message=" +
                    diagnostic.message);
            }
            return nullptr;
        }

        const auto resourcePath = BuildPrefabResourcePathFromPayload(payload);
        NLS_LOG_INFO("Asset drag handle is not imported yet; scheduling background preimport before scene insertion: " + path);
        return CreateGameObjectFromAsset(resourcePath, focusOnCreation, p_parent, placementOverride);
    }

    if (!result.handled || result.dragDrop.status != NLS::Editor::Assets::DragDropOperationStatus::Committed ||
        !result.dragDrop.instance.has_value() || !result.dragDrop.instance->instanceRoot)
    {
        NLS_LOG_ERROR("Failed to create GameObject from asset drag handle: " + path);
        return nullptr;
    }

    auto& instance = *result.dragDrop.instance->instanceRoot;
    NLS_LOG_INFO("Created GameObject from asset drag handle: " + path + " root=" + instance.GetName());

    if (placementOverride.has_value())
        instance.GetTransform()->SetWorldPosition(*placementOverride);
    else if (m_gameObjectSpawnMode == EGameObjectSpawnMode::FRONT)
        instance.GetTransform()->SetLocalPosition(CalculateGameObjectSpawnPoint(10.0f));

    if (result.dragDrop.deferredAssetReferenceResolutionRequested)
    {
        QueuePrefabInstanceAssetResolution(
            result.dragDrop.instance ? &*result.dragDrop.instance : nullptr,
            result.dragDrop.artifact ? &*result.dragDrop.artifact : nullptr,
            path);
    }
    if (focusOnCreation)
        SelectGameObject(instance);
    MarkGameObjectCreationSceneDirty(m_context, *creationScene);
    return &instance;
}

void NLS::Editor::Core::EditorActions::CompletePendingAssetDrop(
    std::string path,
    bool focusOnCreation,
    Engine::SceneSystem::Scene* scene,
    SceneMutationToken sceneToken,
    PendingAssetDropParentGuard parentGuard,
    NLS::Editor::Assets::EditorAssetDragDropBridgeResult importResult)
{
    auto cleanupParentGuard = [this, &parentGuard]
    {
        ReleaseGameObjectDestroyedListener(parentGuard.destroyedListener);
        parentGuard.destroyedListener = InvalidListenerID;
    };

    if (!IsGameObjectCreationSceneLive(m_context, scene, sceneToken, *this))
    {
        cleanupParentGuard();
        return;
    }

    if (!importResult.importSucceeded)
    {
        cleanupParentGuard();
        NLS_LOG_ERROR("Failed to import asset before creating GameObject: " + path);
        return;
    }

    if (parentGuard.parentDestroyed && *parentGuard.parentDestroyed)
    {
        cleanupParentGuard();
        NLS_LOG_WARNING("Cancelled imported asset drop because the target parent was destroyed: " + path);
        return;
    }

    auto* liveParent = FindLiveGameObjectByAddress(*scene, parentGuard.parentAddress);
    if (parentGuard.parentAddress != nullptr && liveParent == nullptr)
    {
        cleanupParentGuard();
        NLS_LOG_WARNING("Cancelled imported asset drop because the target parent was destroyed: " + path);
        return;
    }

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(m_context.projectAssetsPath);
    auto result = bridge.DropModelAssetIntoHierarchy(
        path,
        *scene,
        {},
        &m_context.prefabInstanceRegistry,
        liveParent,
        &m_context.importProgressTracker);
    if (!result.handled || result.dragDrop.status != NLS::Editor::Assets::DragDropOperationStatus::Committed ||
        !result.dragDrop.instance.has_value() || !result.dragDrop.instance->instanceRoot)
    {
        cleanupParentGuard();
        NLS_LOG_ERROR("Failed to create GameObject from imported asset: " + path);
        return;
    }

    auto& instance = *result.dragDrop.instance->instanceRoot;
    NLS_LOG_INFO("Created GameObject from imported asset: " + path + " root=" + instance.GetName());
    if (m_gameObjectSpawnMode == EGameObjectSpawnMode::FRONT)
        instance.GetTransform()->SetLocalPosition(CalculateGameObjectSpawnPoint(10.0f));

    if (result.dragDrop.deferredAssetReferenceResolutionRequested)
    {
        QueuePrefabInstanceAssetResolution(
            result.dragDrop.instance ? &*result.dragDrop.instance : nullptr,
            result.dragDrop.artifact ? &*result.dragDrop.artifact : nullptr,
            path);
    }
    if (focusOnCreation)
        SelectGameObject(instance);
    MarkGameObjectCreationSceneDirty(m_context, *scene);
    cleanupParentGuard();
}

void NLS::Editor::Core::EditorActions::QueuePrefabInstanceAssetResolution(
    NLS::Editor::Assets::PrefabInstanceRecord* instance,
    const NLS::Engine::Assets::PrefabArtifact* sourcePrefab,
    std::string label)
{
    if (!instance)
    {
        NLS_LOG_WARNING("Skipped renderer resource resolution: missing prefab instance for " + label);
        return;
    }

    if (!instance->instanceRoot)
    {
        NLS_LOG_WARNING("Skipped renderer resource resolution: missing instance root for " + label);
        return;
    }

    auto* registeredInstance = m_context.prefabInstanceRegistry.FindInstance(*instance->instanceRoot);
    if (!registeredInstance || registeredInstance->instanceRoot != instance->instanceRoot)
    {
        NLS_LOG_WARNING("Skipped renderer resource resolution: prefab instance is not registered for " + label);
        return;
    }
    instance = registeredInstance;

    auto* liveScene = ResolveSceneForLiveObject(m_context, *instance->instanceRoot);
    if (!liveScene)
    {
        NLS_LOG_WARNING("Skipped renderer resource resolution: prefab instance is not in a live scene for " + label);
        return;
    }

    const bool generatedModelInstance = instance->generatedReadOnly ||
        (sourcePrefab && sourcePrefab->generatedModelPrefab);
    if (!generatedModelInstance)
    {
        NLS_LOG_INFO("Skipped renderer resource resolution for non-generated prefab instance: " + label);
        return;
    }

    const auto resolvedStats = CountResolvedRendererResources(*instance->instanceRoot);
    if (resolvedStats.meshRenderers > 0u &&
        resolvedStats.boundMeshes == resolvedStats.meshRenderers &&
        resolvedStats.boundMaterialSlotRenderers == resolvedStats.materialSlotRenderers)
    {
        NLS_LOG_INFO(
            "Renderer resources already resolved for prefab instance from cached artifact paths: " +
            label +
            " mesh=" +
            std::to_string(resolvedStats.boundMeshes) +
            "/" +
            std::to_string(resolvedStats.meshRenderers) +
            " materials=" +
            std::to_string(resolvedStats.boundMaterialSlotRenderers) +
            "/" +
            std::to_string(resolvedStats.materialSlotRenderers));
        return;
    }

    NLS::Engine::Assets::PrefabArtifact prefab;
    if (sourcePrefab)
        prefab = *sourcePrefab;
    else
    {
        prefab.assetId = instance->prefabAssetId;
        prefab.graph = instance->sourceGraph;
        prefab.generatedModelPrefab = instance->generatedReadOnly;
    }

    std::vector<RendererResourceResolutionTask> meshTasks;
    std::vector<RendererResourceResolutionTask> materialTasks;
    auto stats = std::make_shared<RendererResourceResolutionStats>();
    size_t visitedGameObjects = 0u;
    size_t visitedMeshReferences = 0u;
    size_t visitedMaterialReferences = 0u;
    CollectPrefabAssetResolutionTasks(
        prefab,
        *instance,
        meshTasks,
        materialTasks,
        stats,
        visitedGameObjects,
        visitedMeshReferences,
        visitedMaterialReferences);

    if (meshTasks.empty() && materialTasks.empty())
    {
        NLS_LOG_WARNING(
            "Skipped renderer resource resolution: no tasks for " +
            label +
            " mappedGameObjects=" +
            std::to_string(visitedGameObjects) +
            " meshRefs=" +
            std::to_string(visitedMeshReferences) +
            " materialRefs=" +
            std::to_string(visitedMaterialReferences) +
            " resolvedAssets=" +
            std::to_string(prefab.resolvedAssets.size()));
        return;
    }

    std::deque<RendererResourceResolutionTask> tasks;
    std::move(
        meshTasks.rbegin(),
        meshTasks.rend(),
        std::back_inserter(tasks));
    std::move(
        materialTasks.begin(),
        materialTasks.end(),
        std::back_inserter(tasks));

    NLS_LOG_INFO(
        "Queued renderer resource resolution for prefab instance: " +
        label +
        " tasks=" +
        std::to_string(tasks.size()));

    auto& tracker = m_context.importProgressTracker;
    const auto job = tracker.BeginJob(
        instance->prefabAssetId,
        label.empty() ? std::string("Resolve model instance") : std::move(label),
        "asset-resolution",
        tasks.size());
    tracker.ReportProgress(job, Assets::ImportPhase::Postprocess, 0.0, "Preparing renderer resources");

    auto state = std::make_shared<RendererResourceResolutionState>();
    state->job = job;
    state->scene = ResolveSceneForLiveObject(m_context, *instance->instanceRoot);
    state->sceneToken = CaptureSceneMutationToken();
    state->prefabAssetId = instance->prefabAssetId;
    state->prefabSubAssetKey = instance->prefabSubAssetKey;
    state->remainingTasks = std::move(tasks);
    state->cachedLiveInstance = instance;
    state->liveObjects = BuildRendererResourceLiveObjectIndex(*instance);
    state->stats = std::move(stats);
    state->totalTasks = state->remainingTasks.size();
    state->restoreRootSelfActive = instance->instanceRoot->IsSelfActive();
    instance->instanceRoot->SetActive(false);
    state->rootHiddenUntilRendererResourcesReady = true;
    state->destroyedListener = TrackGameObjectDestroyedListener(
        [state](NLS::Engine::GameObject& destroyed)
        {
            RendererResourceResolutionTargetDestroyed(*state, destroyed);
        });
    DelayAction(
        [this, state]
        {
            RunRendererResourceResolutionStep(*this, state);
        },
        1);
}

bool Editor::Core::EditorActions::DestroyGameObject(Engine::GameObject& p_actor)
{
    if (GetSelectedGameObject() == &p_actor)
        UnselectGameObject();

    p_actor.MarkAsDestroy();
    NLS_LOG_INFO("GameObject destroyed");
    m_context.sceneManager.MarkCurrentSceneDirty();
    return true;
}

std::string FindDuplicatedGameObjectUniqueName(Engine::GameObject& p_duplicated, Engine::GameObject& p_newActor, Engine::SceneSystem::Scene& p_scene)
{
    const auto parent = p_newActor.GetParent();
    const auto adjacentGameObjects = parent ? parent->GetChildren() : p_scene.GetGameObjects();

    auto availabilityChecker = [&parent, &adjacentGameObjects](std::string target) -> bool
    {
        const auto isGameObjectNameTaken = [&target, parent](auto actor)
        { return (parent || !actor->GetParent()) && actor->GetName() == target; };
        return std::find_if(adjacentGameObjects.begin(), adjacentGameObjects.end(), isGameObjectNameTaken) == adjacentGameObjects.end();
    };

    return Utils::String::GenerateUnique(p_duplicated.GetName(), availabilityChecker);
}

void Editor::Core::EditorActions::DuplicateGameObject(Engine::GameObject& p_toDuplicate, Engine::GameObject* p_forcedParent, bool p_focus)
{
    // 	tinyxml2::XMLDocument doc;
    // 	tinyxml2::XMLNode* actorsRoot = doc.NewElement("actors");
    // 	p_toDuplicate.OnSerialize(doc, actorsRoot);
    // 	auto& newActor = CreateEmptyGameObject(false);
    // 	int64_t idToUse = newActor.GetID();
    // 	tinyxml2::XMLElement* currentActor = actorsRoot->FirstChildElement("GameObject");
    // 	newActor.OnDeserialize(doc, currentActor);
    //
    // 	newActor.SetID(idToUse);
    //
    // 	if (p_forcedParent)
    // 		newActor.SetParent(*p_forcedParent);
    // 	else
    // 	{
    //         auto currentScene = m_context.sceneManager.GetCurrentScene();
    //
    //         if (newActor.HasParent())
    //         {
    //             if (auto found = newActor.GetParent(); found)
    //             {
    //                 newActor.SetParent(*found);
    //             }
    //         }
    //
    //         const auto uniqueName = FindDuplicatedGameObjectUniqueName(p_toDuplicate, newActor, *currentScene);
    //         newActor.SetName(uniqueName);
    // 	}
    //
    // 	if (p_focus)
    // 		SelectGameObject(newActor);
    //
    // 	for (auto& child : p_toDuplicate.GetChildren())
    // 		DuplicateGameObject(*child, &newActor, false);
}

void Editor::Core::EditorActions::SelectGameObject(Engine::GameObject& p_target)
{
    EDITOR_PANEL(Panels::Inspector, "Inspector").FocusGameObject(p_target);
}

void Editor::Core::EditorActions::UnselectGameObject()
{
    EDITOR_PANEL(Panels::Inspector, "Inspector").UnFocus();
}

bool Editor::Core::EditorActions::IsAnyGameObjectSelected() const
{
    return EDITOR_PANEL(Panels::Inspector, "Inspector").GetTargetGameObject();
}

Engine::GameObject* Editor::Core::EditorActions::GetSelectedGameObject() const
{
    return EDITOR_PANEL(Panels::Inspector, "Inspector").GetTargetGameObject();
}

void Editor::Core::EditorActions::MoveToTarget(Engine::GameObject& p_target)
{
    EDITOR_PANEL(Panels::SceneView, "Scene View").GetCameraController().MoveToTarget(p_target);
}

void Editor::Core::EditorActions::CompileShaders()
{
    for (auto shader : m_context.shaderManager.GetResources())
        Render::Resources::Loaders::ShaderLoader::Recompile(
            *shader.second,
            GetRealPath(shader.second->path),
            m_context.projectAssetsPath);
}

void Editor::Core::EditorActions::SaveMaterials()
{
    for (auto& [id, material] : m_context.materialManager.GetResources())
        Render::Resources::Loaders::MaterialLoader::Save(*material, GetRealPath(material->path));
}

bool Editor::Core::EditorActions::ImportAsset(const std::string& p_initialDestinationDirectory)
{
    using namespace Dialogs;

    std::string modelFormats = "*.fbx;*.obj;*.gltf;*.glb;";
    std::string textureFormats = "*.png;*.jpeg;*.jpg;*.tga";
    std::string shaderFormats = "*.hlsl;";
    std::string soundFormats = "*.mp3;*.ogg;*.wav;";

    OpenFileDialog selectAssetDialog("Select an asset to import", "", {"Any supported format", modelFormats + textureFormats + shaderFormats + soundFormats, "Model (.fbx, .obj, .gltf, .glb)", modelFormats, "Texture (.png, .jpeg, .jpg, .tga)", textureFormats, "Shader (.hlsl)", shaderFormats, "Sound (.mp3, .ogg, .wav)", soundFormats});

    if (!selectAssetDialog.Result().empty())
    {
        std::string source = selectAssetDialog.Result()[0];
        std::string extension = '.' + Utils::PathParser::GetExtension(source);
        std::string filename = Utils::PathParser::GetElementName(source);

        SaveFileDialog saveLocationDialog("Where to import?", p_initialDestinationDirectory + filename, {extension, extension});

        if (!saveLocationDialog.Result().empty())
        {
            std::string destination = saveLocationDialog.Result();

            if (!std::filesystem::exists(destination) || MessageBox("File already exists", "The destination you have selected already exists, importing this file will erase the previous file content, are you sure about that?", MessageBox::EMessageType::WARNING, MessageBox::EButtonLayout::OK_CANCEL).GetUserAction() == MessageBox::EUserAction::OK)
            {
                std::filesystem::copy(source, destination, std::filesystem::copy_options::overwrite_existing);
                if (!ImportCopiedAssetThroughDatabase(m_context.projectAssetsPath, destination))
                    return false;
                NLS_LOG_INFO("Asset \"" + destination + "\" imported");
                return true;
            }
        }
    }

    return false;
}

bool Editor::Core::EditorActions::ImportAssetAtLocation(const std::string& p_destination)
{
    using namespace Dialogs;

    std::string modelFormats = "*.fbx;*.obj;*.gltf;*.glb;";
    std::string textureFormats = "*.png;*.jpeg;*.jpg;*.tga;";
    std::string shaderFormats = "*.hlsl;";
    std::string soundFormats = "*.mp3;*.ogg;*.wav;";

    OpenFileDialog selectAssetDialog("Select an asset to import", "", {"Any supported format", modelFormats + textureFormats + shaderFormats + soundFormats, "Model (.fbx, .obj, .gltf, .glb)", modelFormats, "Texture (.png, .jpeg, .jpg, .tga)", textureFormats, "Shader (.hlsl)", shaderFormats, "Sound (.mp3, .ogg, .wav)", soundFormats});

    if (!selectAssetDialog.Result().empty())
    {
        std::string source = selectAssetDialog.Result()[0];
        std::string destination = p_destination + Utils::PathParser::GetElementName(source);

        if (!std::filesystem::exists(destination) || MessageBox("File already exists", "The destination you have selected already exists, importing this file will erase the previous file content, are you sure about that?", MessageBox::EMessageType::WARNING, MessageBox::EButtonLayout::OK_CANCEL).GetUserAction() == MessageBox::EUserAction::OK)
        {
            std::filesystem::copy(source, destination, std::filesystem::copy_options::overwrite_existing);
            if (!ImportCopiedAssetThroughDatabase(m_context.projectAssetsPath, destination))
                return false;
            NLS_LOG_INFO("Asset \"" + destination + "\" imported");
            return true;
        }
    }

    return false;
}

// Duplicate from AResourceManager.h
std::string Editor::Core::EditorActions::GetRealPath(const std::string& p_path)
{
    if (p_path.empty())
        return {};

    if (std::filesystem::path(p_path).is_absolute())
        return p_path;

    std::string result;

    if (p_path[0] == ':') // The path is an engine path
    {
        result = m_context.engineAssetsPath + std::string(p_path.data() + 1, p_path.data() + p_path.size());
    }
    else // The path is a project path
    {
        result = m_context.projectAssetsPath + p_path;
    }

    return result;
}

std::string Editor::Core::EditorActions::GetResourcePath(const std::string& p_path, bool p_isFromEngine)
{
    std::string result = p_path;

    if (Utils::String::Replace(result, p_isFromEngine ? m_context.engineAssetsPath : m_context.projectAssetsPath, ""))
    {
        if (p_isFromEngine)
            result = ':' + result;
    }

    return result;
}

void Editor::Core::EditorActions::PropagateFolderRename(std::string p_previousName, std::string p_newName)
{
    p_previousName = Utils::PathParser::MakeNonWindowsStyle(p_previousName);
    p_newName = Utils::PathParser::MakeNonWindowsStyle(p_newName);

    for (auto& p : std::filesystem::recursive_directory_iterator(p_newName))
    {
        if (!p.is_directory())
        {
            std::string newFileName = Utils::PathParser::MakeNonWindowsStyle(p.path().string());
            std::string previousFileName;

            for (char c : newFileName)
            {
                previousFileName += c;
                if (previousFileName == p_newName)
                    previousFileName = p_previousName;
            }

            PropagateFileRename(Utils::PathParser::MakeNonWindowsStyle(previousFileName), Utils::PathParser::MakeNonWindowsStyle(newFileName));
        }
    }
}

void Editor::Core::EditorActions::PropagateFolderDestruction(std::string p_folderPath)
{
    for (auto& p : std::filesystem::recursive_directory_iterator(p_folderPath))
    {
        if (!p.is_directory())
        {
            PropagateFileRename(Utils::PathParser::MakeNonWindowsStyle(p.path().string()), "?");
        }
    }
}

void Editor::Core::EditorActions::PropagateScriptRename(std::string p_previousName, std::string p_newName)
{

}

void Editor::Core::EditorActions::PropagateFileRename(std::string p_previousName, std::string p_newName)
{
     p_previousName = GetResourcePath(p_previousName);
     p_newName = GetResourcePath(p_newName);
 
     if (p_newName != "?")
     {
         /* If not a real rename is asked (Not delete) */
 
         NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MeshManager>().MoveResource(p_previousName, p_newName);
 
         if (NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::TextureManager>().MoveResource(p_previousName, p_newName))
         {
             Render::Resources::Texture2D* resource = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::TextureManager>()[p_newName];
             resource->path = p_newName;
         }
 
         if (NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::ShaderManager>().MoveResource(p_previousName, p_newName))
         {
             Render::Resources::Shader* resource = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::ShaderManager>()[p_newName];
             resource->path = p_newName;
         }
 
         if (NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MaterialManager>().MoveResource(p_previousName, p_newName))
         {
             NLS::Render::Resources::Material* resource = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MaterialManager>()[p_newName];
             resource->path = p_newName;
         }
     }
     else
     {
         if (auto texture = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::TextureManager>().GetResource(p_previousName, false))
         {
            for (auto [name, instance] : NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MaterialManager>().GetResources())
                 if (instance)
                 {
                     std::vector<std::string> textureUniformsToClear;
                     for (const auto& [uniformName, value] : instance->GetUniformsData())
                         if (value.has_value() && value.type() == typeid(Render::Resources::Texture2D*))
                             if (std::any_cast<Render::Resources::Texture2D*>(value) == texture)
                                 textureUniformsToClear.push_back(uniformName);

                     for (const auto& uniformName : textureUniformsToClear)
                         instance->Set<Render::Resources::Texture2D*>(uniformName, nullptr);
                 }
 
             auto& assetView = EDITOR_PANEL(Panels::AssetView, "Asset View");
             auto assetViewRes = assetView.GetResource();
             if (auto pval = std::get_if<Render::Resources::Texture2D*>(&assetViewRes); pval && *pval)
                 assetView.ClearResource();
 
             NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::TextureManager>().UnloadResource(p_previousName);
         }
 
         if (auto shader = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::ShaderManager>().GetResource(p_previousName, false))
         {
             auto& materialEditor = EDITOR_PANEL(Panels::MaterialEditor, "Material Editor");
 
             for (auto [name, instance] : NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MaterialManager>().GetResources())
                 if (instance && instance->GetShader() == shader)
                     instance->SetShader(nullptr);
 
             NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::ShaderManager>().UnloadResource(p_previousName);
         }
 
         if (auto mesh = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MeshManager>().GetResource(p_previousName, false))
         {
             auto& assetView = EDITOR_PANEL(Panels::AssetView, "Asset View");
             auto assetViewRes = assetView.GetResource();
             if (auto pval = std::get_if<Render::Resources::Mesh*>(&assetViewRes); pval && *pval)
                 assetView.ClearResource();
 
            if (auto currentScene = m_context.sceneManager.GetCurrentScene())
            {
                 for (auto actor : currentScene->GetGameObjects())
                     if (auto meshFilter = actor->GetComponent<Engine::Components::MeshFilter>(); meshFilter && meshFilter->ResolveMesh() == mesh)
                     {
                         meshFilter->SetMesh(nullptr);
                         m_context.sceneManager.MarkCurrentSceneDirty();
                     }
            }
 
             NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MeshManager>().UnloadResource(p_previousName);
         }
 
         if (auto material = NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MaterialManager>().GetResource(p_previousName, false))
         {
             auto& materialEditor = EDITOR_PANEL(Panels::MaterialEditor, "Material Editor");
 
             if (materialEditor.GetTarget() == material)
                 materialEditor.RemoveTarget();
 
             auto& assetView = EDITOR_PANEL(Panels::AssetView, "Asset View");
             auto assetViewRes = assetView.GetResource();
             if (auto pval = std::get_if<NLS::Render::Resources::Material*>(&assetViewRes); pval && *pval)
                 assetView.ClearResource();
 
            if (auto currentScene = m_context.sceneManager.GetCurrentScene())
            {
                 for (auto actor : currentScene->GetGameObjects())
                     if (auto meshRenderer = actor->GetComponent<Engine::Components::MeshRenderer>(); meshRenderer)
                     {
                         meshRenderer->RemoveMaterialByInstance(*material);
                         m_context.sceneManager.MarkCurrentSceneDirty();
                     }
            }
 
             NLS::Core::ServiceLocator::Get<NLS::Core::ResourceManagement::MaterialManager>().UnloadResource(p_previousName);
         }
     }
 
     switch (Utils::PathParser::GetFileType(p_previousName))
     {
         case Utils::PathParser::EFileType::MATERIAL:
             PropagateFileRenameThroughSavedFilesOfType(p_previousName, p_newName, Utils::PathParser::EFileType::SCENE);
             break;
         case Utils::PathParser::EFileType::MODEL:
             PropagateFileRenameThroughSavedFilesOfType(p_previousName, p_newName, Utils::PathParser::EFileType::SCENE);
             break;
         case Utils::PathParser::EFileType::SHADER:
             PropagateFileRenameThroughSavedFilesOfType(p_previousName, p_newName, Utils::PathParser::EFileType::MATERIAL);
             break;
         case Utils::PathParser::EFileType::TEXTURE:
             PropagateFileRenameThroughSavedFilesOfType(p_previousName, p_newName, Utils::PathParser::EFileType::MATERIAL);
             break;
         case Utils::PathParser::EFileType::SOUND:
             PropagateFileRenameThroughSavedFilesOfType(p_previousName, p_newName, Utils::PathParser::EFileType::SCENE);
             break;
     }
 
     EDITOR_PANEL(Panels::Inspector, "Inspector").Refresh();
     EDITOR_PANEL(Panels::MaterialEditor, "Material Editor").Refresh();
}

void Editor::Core::EditorActions::PropagateFileRenameThroughSavedFilesOfType(const std::string& p_previousName, const std::string& p_newName, Utils::PathParser::EFileType p_fileType)
{
    for (auto& entry : std::filesystem::recursive_directory_iterator(m_context.projectAssetsPath))
    {
        if (Utils::PathParser::GetFileType(entry.path().string()) == p_fileType)
        {
            using namespace std;

            {
                ifstream in(entry.path().string().c_str());
                ofstream out("TEMP");
                string wordToReplace(">" + p_previousName + "<");
                string wordToReplaceWith(">" + p_newName + "<");

                string line;
                size_t len = wordToReplace.length();
                while (getline(in, line))
                {
                    if (Utils::String::Replace(line, wordToReplace, wordToReplaceWith))
                        NLS_LOG_INFO("Asset retargeting: \"" + p_previousName + "\" to \"" + p_newName + "\" in \"" + entry.path().string() + "\"");
                    out << line << '\n';
                }

                out.close();
                in.close();
            }

            std::filesystem::copy_file("TEMP", entry.path(), std::filesystem::copy_options::overwrite_existing);
            std::filesystem::remove("TEMP");
        }
    }
}

void Editor::Core::EditorActions::RefreshWindowTitle()
{
    const auto scenePath = m_context.sceneManager.GetCurrentSceneSourcePath();
    const std::string sceneName = scenePath.empty() ? "Untitled Scene" : GetResourcePath(scenePath);
    const std::string dirtyMarker = m_context.sceneManager.HasUnsavedSceneChanges() ? "*" : "";
    m_context.window->SetTitle(m_context.windowSettings.title + " - " + sceneName + dirtyMarker);
}

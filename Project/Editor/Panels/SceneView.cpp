#include "ImGui/imgui.h"

#include "Rendering/DebugSceneRenderer.h"
#include "Rendering/PickingRenderPass.h"
#include "Rendering/SceneRendererFactory.h"
#include "Core/EditorActions.h"
#include "Core/RecentBackgroundWorkGate.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Assets/EditorAssetPathUtils.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Panels/SceneView.h"
#include "Panels/GameView.h"
#include "Panels/SceneViewPickingPolicy.h"
#include "Panels/ViewFrameLifecycle.h"
#include "Settings/EditorSettings.h"
#include "Profiling/Profiler.h"
#include "Core/EditorInteractionBlocker.h"
#include "Core/RendererResourceStreamingBudget.h"
#include "Core/SceneCameraFocus.h"
#include "ImGuizmo.h"
#include "Debug/Logger.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Settings/EPixelDataFormat.h"
#include "Rendering/Settings/EPixelDataType.h"
#include "Rendering/Tooling/MaterialVisualEvidence.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ResourceManagement/ResourceLifetimeRegistry.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Rendering/Resources/Material.h"
#include <ServiceLocator.h>
#include <UI/Plugins/DragDrop.h>
#include <UI/Plugins/IPlugin.h>
#include <UI/UIManager.h>
#include <UI/Widgets/Layout/Group.h>
#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
using namespace NLS;

namespace
{
constexpr float kSceneViewGizmoCameraLength = 8.0f;
constexpr float kSceneViewDefaultFocusDistance = 15.0f;
constexpr float kSceneViewDragPreviewFallbackDistance = 12.0f;
constexpr auto kSceneViewDragPreviewRetryDelay = std::chrono::milliseconds(100);
constexpr size_t kSceneViewDragPreviewResourcePrewarmsPerFrame =
    NLS::Editor::Core::GetDragPreviewPrefabRendererResourceStreamingBudget().resourcePrewarmsPerFrame;
constexpr size_t kSceneViewDragPreviewMeshPrewarmsPerFrame =
    NLS::Editor::Core::GetDragPreviewPrefabRendererResourceStreamingBudget().meshPrewarmsPerFrame;
constexpr size_t kSceneViewDragPreviewMaterialPrewarmsPerFrame =
    NLS::Editor::Core::GetDragPreviewPrefabRendererResourceStreamingBudget().materialPrewarmsPerFrame;
constexpr size_t kSceneViewDragPreviewTextureCompletionsPerFrame =
    NLS::Editor::Core::GetDragPreviewPrefabRendererResourceStreamingBudget().textureCompletionsPerFrame;
constexpr size_t kSceneViewDragPreviewMeshBindsPerFrame =
    NLS::Editor::Core::GetDragPreviewPrefabRendererResourceStreamingBudget().meshBindsPerFrame;
constexpr uint64_t kSceneViewHoverPickingVisibleDrawBudget = 1024u;
constexpr size_t kSceneViewImportedPrefabPreviewPreloadGateCapacity = 256u;
constexpr auto kSceneViewImportedPrefabPreviewPreloadGateTtl = std::chrono::seconds(3);

std::mutex& ImportedPrefabPreviewPreloadMutex()
{
    static std::mutex mutex;
    return mutex;
}

NLS::Editor::Core::RecentBackgroundWorkGate& ImportedPrefabPreviewPreloadGate()
{
    static NLS::Editor::Core::RecentBackgroundWorkGate gate(
        kSceneViewImportedPrefabPreviewPreloadGateCapacity,
        kSceneViewImportedPrefabPreviewPreloadGateTtl);
    return gate;
}

std::string BuildImportedPrefabPreviewPreloadKey(
    const NLS::Editor::Assets::EditorAssetDragPayload& payload)
{
    return NLS::Editor::Assets::GetEditorAssetDragPayloadPath(payload) + "|" +
        NLS::Editor::Assets::GetEditorAssetDragPayloadGuid(payload) + "|" +
        NLS::Editor::Assets::GetEditorAssetDragPayloadSubAssetKey(payload);
}

bool IsImportedPrefabPreviewPreloadInFlight(
    const NLS::Editor::Assets::EditorAssetDragPayload& payload)
{
    const auto key = BuildImportedPrefabPreviewPreloadKey(payload);
    if (key.empty())
        return false;
    std::lock_guard lock(ImportedPrefabPreviewPreloadMutex());
    return ImportedPrefabPreviewPreloadGate().IsInFlight(key);
}

bool ScheduleImportedPrefabPreviewPreloadOnce(
    const NLS::Editor::Assets::EditorAssetDragPayload& payload)
{
    const auto key = BuildImportedPrefabPreviewPreloadKey(payload);
    if (key.empty())
        return false;
    {
        std::lock_guard lock(ImportedPrefabPreviewPreloadMutex());
        if (!ImportedPrefabPreviewPreloadGate().TryBegin(
                key,
                NLS::Editor::Core::RecentBackgroundWorkGate::Clock::now()))
            return false;
    }

    const auto projectAssetsPath = std::filesystem::path(EDITOR_CONTEXT(projectAssetsPath));
    const bool scheduled = NLS::Editor::Assets::SchedulePreviewPrefabHotCachePreload(
        payload,
        projectAssetsPath,
        [key](std::function<void()> task)
        {
            return EDITOR_EXEC(TrackOpportunisticBackgroundTask(
                [task = std::move(task), key]
                {
                    auto completion = ImportedPrefabPreviewPreloadGate().CompleteOnScopeExit(key);
                    if (task)
                        task();
                }));
        });
    if (!scheduled)
    {
        ImportedPrefabPreviewPreloadGate().End(key);
    }
    return scheduled;
}

void HashSceneViewCacheValue(uint64_t& seed, const uint64_t value)
{
    seed ^= value + 0x9E3779B97F4A7C15ull + (seed << 6u) + (seed >> 2u);
}

void HashSceneViewCachePointer(uint64_t& seed, const void* value)
{
    HashSceneViewCacheValue(seed, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value)));
}

void HashSceneViewCacheFloat(uint64_t& seed, const float value)
{
    uint32_t bits = 0u;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(value));
    HashSceneViewCacheValue(seed, static_cast<uint64_t>(bits));
}

void HashSceneViewCacheVector2(uint64_t& seed, const NLS::Maths::Vector2& value)
{
    HashSceneViewCacheFloat(seed, value.x);
    HashSceneViewCacheFloat(seed, value.y);
}

void HashSceneViewCacheVector3(uint64_t& seed, const NLS::Maths::Vector3& value)
{
    HashSceneViewCacheFloat(seed, value.x);
    HashSceneViewCacheFloat(seed, value.y);
    HashSceneViewCacheFloat(seed, value.z);
}

uint64_t BeginSceneViewCacheSegment(const uint64_t salt)
{
    uint64_t seed = 0x51CE71E55EED0001ull;
    HashSceneViewCacheValue(seed, salt);
    return seed;
}

void TraceSceneViewCacheSegmentChange(
    const char* scopeName,
    const uint64_t previousValue,
    const uint64_t currentValue)
{
    if (previousValue == currentValue)
        return;

    NLS_PROFILE_NAMED_SCOPE(scopeName);
}

bool IsImportedAssetPreviewRendererResource(const NLS::Engine::Assets::PrefabResolvedAsset& resolved)
{
    return (resolved.expectedType == "Mesh" || resolved.expectedType == "Material") &&
        !resolved.artifactPath.empty();
}

std::shared_ptr<NLS::Render::Resources::Mesh> CreateImportedAssetDragPreviewMesh(
    const NLS::Render::Assets::MeshArtifactData& source)
{
    if (source.vertices.empty())
        return {};

    return std::shared_ptr<NLS::Render::Resources::Mesh>(
        new NLS::Render::Resources::Mesh(
            source.vertices,
            source.indices,
            source.materialIndex,
            NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
            source.boundingSphere));
}

std::shared_ptr<NLS::Render::Resources::Mesh> TryConsumeImportedAssetDragPreviewMeshLoad(
    const std::string& path,
    std::unordered_map<std::string, std::shared_ptr<NLS::Editor::Core::PrefabInstanceMeshArtifactLoadState>>& loads)
{
    const auto found = loads.find(path);
    if (found == loads.end() || !found->second)
        return {};

    std::shared_ptr<NLS::Render::Resources::Mesh> transientMesh;
    std::shared_ptr<const NLS::Render::Assets::MeshArtifactData> data;
    {
        std::lock_guard lock(found->second->mutex);
        if (!found->second->completed || !found->second->accepted || found->second->failed)
            return {};
        transientMesh = found->second->transientMesh;
        data = found->second->data;
    }

    if (transientMesh)
        return transientMesh;
    if (!data)
        return {};

    auto owner = CreateImportedAssetDragPreviewMesh(*data);
    {
        std::lock_guard lock(found->second->mutex);
        if (!found->second->transientMesh)
        {
            found->second->transientMesh = owner;
        }
        else
        {
            owner = found->second->transientMesh;
        }
    }
    return owner;
}

bool StartImportedAssetDragPreviewMeshLoad(
    const std::string& path,
    std::unordered_map<std::string, std::shared_ptr<NLS::Editor::Core::PrefabInstanceMeshArtifactLoadState>>& loads)
{
    if (path.empty() || loads.find(path) != loads.end())
        return false;

    auto state = std::make_shared<NLS::Editor::Core::PrefabInstanceMeshArtifactLoadState>();
    loads.emplace(path, state);
    const bool accepted = EDITOR_EXEC(TrackOpportunisticBackgroundTask(
        [state, path]
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
                data = NLS::Render::Assets::LoadMeshArtifact(artifactPath, state->cancelled.get());
            }
            catch (const std::exception& exception)
            {
                NLS_LOG_ERROR(std::string("Scene View drag preview mesh load failed: ") + path + " error=" + exception.what());
            }
            catch (...)
            {
                NLS_LOG_ERROR("Scene View drag preview mesh load failed: " + path + " error=unknown");
            }

            std::lock_guard lock(state->mutex);
            if (state->cancelled && state->cancelled->load(std::memory_order_acquire))
            {
                state->data.reset();
                state->transientMesh.reset();
                state->failed = true;
            }
            else if (data.has_value())
            {
                state->data = std::make_shared<NLS::Render::Assets::MeshArtifactData>(std::move(*data));
                state->failed = false;
            }
            else
            {
                state->failed = true;
            }
            state->completed = true;
        }));
    if (!accepted)
    {
        std::lock_guard lock(state->mutex);
        state->accepted = false;
        state->failed = true;
        state->completed = true;
    }
    return accepted;
}

void AcquireImportedAssetDragPreviewResourceOwner(
    const std::string& ownerToken,
    NLS::Core::ResourceManagement::ResourceLifetimeResourceType type,
    const std::string& path)
{
    if (ownerToken.empty() || path.empty() ||
        !NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::ResourceLifetimeRegistry>())
    {
        return;
    }

    NLS_SERVICE(NLS::Core::ResourceManagement::ResourceLifetimeRegistry).Acquire(
        NLS::Core::ResourceManagement::ResourceLifetimeAcquireRequest {
            ownerToken,
            type,
            path,
            0u,
            NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind::Preview });
}

void ReleaseImportedAssetDragPreviewResourceOwner(const std::string& ownerToken)
{
    if (ownerToken.empty() ||
        !NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::ResourceLifetimeRegistry>())
    {
        return;
    }

    NLS_SERVICE(NLS::Core::ResourceManagement::ResourceLifetimeRegistry)
        .ReleaseOwner(ownerToken);
    if (NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::EditorActions>())
        NLS_SERVICE(NLS::Editor::Core::EditorActions).ScheduleImportedResourceTrim();
}

void CancelImportedAssetDragPreviewMeshLoads(
    std::unordered_map<std::string, std::shared_ptr<NLS::Editor::Core::PrefabInstanceMeshArtifactLoadState>>& loads)
{
    for (auto& entry : loads)
    {
        if (entry.second && entry.second->cancelled)
            entry.second->cancelled->store(true, std::memory_order_release);
    }
    loads.clear();
}

NLS::Editor::Core::PrefabInstancePreviewResourceHandoff CollectImportedAssetDragPreviewMeshes(
    NLS::Editor::Core::RendererResourcePrewarmRequest& request)
{
    return NLS::Editor::Core::CollectPrefabInstancePreviewResourceHandoff(request);
}

std::string NormalizeImportedAssetDragPreviewResourcePath(std::string path)
{
    if (path.empty())
        return {};

    std::replace(path.begin(), path.end(), '\\', '/');
    return std::filesystem::path(path).lexically_normal().generic_string();
}

std::string ImportedAssetDragPreviewProjectRelativeResourcePath(
    const std::string& path,
    const std::string& projectAssetsRoot)
{
    if (path.empty() || projectAssetsRoot.empty())
        return {};

    const auto absolutePath = std::filesystem::path(path).lexically_normal();
    if (!absolutePath.is_absolute())
        return {};

    auto assetsRoot = std::filesystem::path(projectAssetsRoot).lexically_normal();
    while (!assetsRoot.empty() && !assetsRoot.has_filename())
        assetsRoot = assetsRoot.parent_path();

    const auto projectRoot = assetsRoot.parent_path();
    if (projectRoot.empty())
        return {};

    const auto relative = absolutePath.lexically_relative(projectRoot.lexically_normal());
    if (relative.empty() || relative.is_absolute())
        return {};

    for (const auto& part : relative)
    {
        if (part == "..")
            return {};
    }

    return relative.generic_string();
}

std::vector<std::string> BuildImportedAssetDragPreviewResourcePathCandidates(
    const std::string& path,
    const std::string& resolvedPath)
{
    std::vector<std::string> candidates;
    auto addCandidate = [&candidates](const std::string& candidate)
    {
        if (candidate.empty() ||
            std::find(candidates.begin(), candidates.end(), candidate) != candidates.end())
        {
            return;
        }
        candidates.push_back(candidate);
    };

    auto addPathVariants = [&addCandidate](const std::string& candidate)
    {
        if (candidate.empty())
            return;

        addCandidate(candidate);
        const auto normalized = std::filesystem::path(candidate).lexically_normal();
        addCandidate(normalized.string());
        addCandidate(normalized.generic_string());
    };

    addPathVariants(path);
    addPathVariants(resolvedPath);
    addPathVariants(ImportedAssetDragPreviewProjectRelativeResourcePath(
        resolvedPath.empty() ? path : resolvedPath,
        NLS::Core::ResourceManagement::MeshManager::ProjectAssetsRoot()));
    return candidates;
}

template <typename ResourceManagerType>
auto FindImportedAssetDragPreviewCachedResource(
    ResourceManagerType& resourceManager,
    const std::vector<std::string>& candidates)
    -> decltype(resourceManager.GetResource(std::declval<std::string>(), false))
{
    for (const auto& candidate : candidates)
    {
        if (auto* cached = resourceManager.GetResource(candidate, false))
            return cached;
    }

    std::vector<std::string> normalizedCandidates;
    normalizedCandidates.reserve(candidates.size());
    for (const auto& candidate : candidates)
    {
        auto normalized = NormalizeImportedAssetDragPreviewResourcePath(candidate);
        if (!normalized.empty() &&
            std::find(normalizedCandidates.begin(), normalizedCandidates.end(), normalized) ==
                normalizedCandidates.end())
        {
            normalizedCandidates.push_back(std::move(normalized));
        }
    }

    const auto resources = resourceManager.GetResources();
    for (const auto& [resourcePath, resource] : resources)
    {
        if (resource == nullptr)
            continue;

        const auto normalizedResourcePath = NormalizeImportedAssetDragPreviewResourcePath(resourcePath);
        if (std::find(normalizedCandidates.begin(), normalizedCandidates.end(), normalizedResourcePath) !=
            normalizedCandidates.end())
        {
            return resource;
        }
    }

    return nullptr;
}

NLS::Core::ResourceManagement::MaterialManager::Material* FindImportedAssetDragPreviewCachedMaterial(
    NLS::Core::ResourceManagement::MaterialManager& materialManager,
    const std::string& path)
{
    return FindImportedAssetDragPreviewCachedResource(
        materialManager,
        BuildImportedAssetDragPreviewResourcePathCandidates(
            path,
            NLS::Core::ResourceManagement::MaterialManager::ResolveResourcePath(path)));
}

NLS::Core::ResourceManagement::TextureManager::Texture2D* FindImportedAssetDragPreviewCachedTexture(
    NLS::Core::ResourceManagement::TextureManager& textureManager,
    const std::string& path)
{
    return FindImportedAssetDragPreviewCachedResource(
        textureManager,
        BuildImportedAssetDragPreviewResourcePathCandidates(
            path,
            NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(path)));
}

NLS::Core::ResourceManagement::MeshManager::Mesh* FindImportedAssetDragPreviewCachedMesh(
    NLS::Core::ResourceManagement::MeshManager& meshManager,
    const std::string& path)
{
    return FindImportedAssetDragPreviewCachedResource(
        meshManager,
        BuildImportedAssetDragPreviewResourcePathCandidates(
            path,
            NLS::Core::ResourceManagement::MeshManager::ResolveResourcePath(path)));
}

bool BindImportedAssetDragPreviewMaterialTextures(
    NLS::Render::Resources::Material& material,
    std::unordered_set<std::string>& textureRequests,
    size_t& textureBindsThisFrame,
    const std::string& ownerToken)
{
    if (!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::TextureManager>())
        return material.GetTextureResourcePaths().empty();

    bool ready = true;
    auto& textureManager = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager);
    auto texturePathMatches = [](const NLS::Render::Resources::Texture2D& texture, const std::string& declaredPath)
    {
        auto normalize = [](const std::string& path)
        {
            auto normalized = NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(path);
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            return std::filesystem::path(normalized).lexically_normal().generic_string();
        };
        return texture.path == declaredPath || normalize(texture.path) == normalize(declaredPath);
    };
    for (const auto& [uniformName, texturePath] : material.GetTextureResourcePaths())
    {
        if (texturePath.empty())
            continue;

        const auto* parameter = material.GetParameterBlock().TryGet(uniformName);
        if (parameter != nullptr &&
            parameter->type() == typeid(NLS::Render::Resources::Texture2D*) &&
            std::any_cast<NLS::Render::Resources::Texture2D*>(*parameter) != nullptr &&
            texturePathMatches(*std::any_cast<NLS::Render::Resources::Texture2D*>(*parameter), texturePath))
        {
            AcquireImportedAssetDragPreviewResourceOwner(
                ownerToken,
                NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Texture,
                texturePath);
            continue;
        }

        if (textureBindsThisFrame >= kSceneViewDragPreviewResourcePrewarmsPerFrame)
        {
            ready = false;
            continue;
        }

        auto* texture = FindImportedAssetDragPreviewCachedTexture(textureManager, texturePath);
        if (!texture &&
            textureRequests.find(texturePath) != textureRequests.end() &&
            !textureManager.IsAsyncArtifactLoadPending(texturePath))
        {
            textureRequests.erase(texturePath);
        }
        if (!texture && textureRequests.find(texturePath) == textureRequests.end())
        {
            AcquireImportedAssetDragPreviewResourceOwner(
                ownerToken,
                NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Texture,
                texturePath);
            texture = textureManager.RequestAsyncArtifact(texturePath, true);
            if (!texture && textureManager.IsAsyncArtifactLoadPending(texturePath))
                textureRequests.insert(texturePath);
        }
        if (!texture)
        {
            ready = false;
            continue;
        }

        AcquireImportedAssetDragPreviewResourceOwner(
            ownerToken,
            NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Texture,
            texturePath);
        material.Set<NLS::Render::Resources::Texture2D*>(uniformName, texture);
        ++textureBindsThisFrame;
    }

    return ready;
}

bool ImportedAssetDragPreviewMaterialsReady(
    NLS::Engine::Components::MeshRenderer& meshRenderer,
    std::unordered_set<std::string>& materialRequests,
    std::unordered_set<std::string>& textureRequests,
    size_t& textureBindsThisFrame,
    const std::string& ownerToken)
{
    const auto paths = meshRenderer.GetMaterialPaths();
    if (paths.empty())
        return true;

    if (!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MaterialManager>())
        return false;

    bool ready = true;
    auto& materialManager = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager);
    for (size_t index = 0;
        index < paths.size() && index < NLS::Engine::Components::MeshRenderer::kMaxMaterialCount;
        ++index)
    {
        auto* material = meshRenderer.GetMaterialAtIndex(static_cast<uint8_t>(index));
        if (material != nullptr)
        {
            if (!paths[index].empty())
            {
                AcquireImportedAssetDragPreviewResourceOwner(
                    ownerToken,
                    NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Material,
                    paths[index]);
            }
            if (!BindImportedAssetDragPreviewMaterialTextures(*material, textureRequests, textureBindsThisFrame, ownerToken))
                ready = false;
            continue;
        }

        if (paths[index].empty())
            continue;

        auto* cached = static_cast<NLS::Render::Resources::Material*>(nullptr);
        const auto materialReferences = meshRenderer.GetMaterialReferences();
        if (index < materialReferences.size())
        {
            if (auto* referencedMaterial = materialReferences[index].Get();
                referencedMaterial != nullptr && referencedMaterial->IsValid())
            {
                cached = referencedMaterial;
            }
        }
        if (cached == nullptr)
            cached = FindImportedAssetDragPreviewCachedMaterial(materialManager, paths[index]);

        if (cached != nullptr)
        {
            AcquireImportedAssetDragPreviewResourceOwner(
                ownerToken,
                NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Material,
                paths[index]);
            meshRenderer.SetResolvedMaterialFromReference(static_cast<uint8_t>(index), *cached);
            if (meshRenderer.GetMaterialAtIndex(static_cast<uint8_t>(index)) != cached)
            {
                ready = false;
                continue;
            }
            if (!BindImportedAssetDragPreviewMaterialTextures(*cached, textureRequests, textureBindsThisFrame, ownerToken))
                ready = false;
            continue;
        }

        if (materialRequests.find(paths[index]) != materialRequests.end() &&
            !materialManager.IsAsyncArtifactLoadPending(paths[index]))
        {
            materialRequests.erase(paths[index]);
        }
        if (materialRequests.find(paths[index]) == materialRequests.end())
        {
            AcquireImportedAssetDragPreviewResourceOwner(
                ownerToken,
                NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Material,
                paths[index]);
            materialManager.RequestAsyncArtifact(paths[index], true);
        }
        if (materialManager.IsAsyncArtifactLoadPending(paths[index]))
            materialRequests.insert(paths[index]);
        ready = false;
    }

    return ready;
}

bool ImportedAssetDragPreviewMeshResourceReady(
    NLS::Engine::Components::MeshFilter& meshFilter,
    std::unordered_map<std::string, std::shared_ptr<NLS::Editor::Core::PrefabInstanceMeshArtifactLoadState>>& meshLoads,
    const std::string& ownerToken)
{
    if (meshFilter.ResolveMesh() != nullptr)
    {
        AcquireImportedAssetDragPreviewResourceOwner(
            ownerToken,
            NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Mesh,
            meshFilter.GetModelPath());
        return true;
    }

    const auto path = meshFilter.GetModelPath();
    if (path.empty())
        return false;

    if (NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MeshManager>())
    {
        auto& meshManager = NLS_SERVICE(NLS::Core::ResourceManagement::MeshManager);
        if (FindImportedAssetDragPreviewCachedMesh(meshManager, path) != nullptr)
        {
            AcquireImportedAssetDragPreviewResourceOwner(
                ownerToken,
                NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Mesh,
                path);
            return true;
        }
    }

    const auto found = meshLoads.find(path);
    if (found == meshLoads.end() || !found->second)
        return false;

    std::lock_guard lock(found->second->mutex);
    return found->second->completed &&
        found->second->accepted &&
        !found->second->failed &&
        (found->second->transientMesh != nullptr || found->second->data != nullptr);
}

bool BindImportedAssetDragPreviewReadyRenderer(
    NLS::Engine::Components::MeshFilter& meshFilter,
    NLS::Engine::Components::MeshRenderer& meshRenderer,
    std::unordered_map<std::string, std::shared_ptr<NLS::Editor::Core::PrefabInstanceMeshArtifactLoadState>>& meshLoads,
    std::unordered_set<std::string>& materialRequests,
    std::unordered_set<std::string>& textureRequests,
    size_t& textureBindsThisFrame,
    bool& foundRenderable,
    size_t& meshBindsThisFrame,
    const std::string& ownerToken)
{
    bool allBound = true;
    foundRenderable = true;
    const bool meshResourceReady = ImportedAssetDragPreviewMeshResourceReady(meshFilter, meshLoads, ownerToken);
    const bool materialsReady = ImportedAssetDragPreviewMaterialsReady(
        meshRenderer,
        materialRequests,
        textureRequests,
        textureBindsThisFrame,
        ownerToken);
    if (!meshResourceReady || !materialsReady)
    {
        allBound = false;
    }
    else if (!meshFilter.ResolveMesh())
    {
        allBound = false;
        const auto path = meshFilter.GetModelPath();
        if (!path.empty() && meshBindsThisFrame < kSceneViewDragPreviewMeshBindsPerFrame)
        {
            if (NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MeshManager>())
            {
                auto& meshManager = NLS_SERVICE(NLS::Core::ResourceManagement::MeshManager);
                if (auto* cached = FindImportedAssetDragPreviewCachedMesh(meshManager, path))
                {
                    AcquireImportedAssetDragPreviewResourceOwner(
                        ownerToken,
                        NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Mesh,
                        path);
                    meshFilter.SetResolvedMeshFromReference(cached);
                    ++meshBindsThisFrame;
                    allBound = true;
                }
            }
            if (!meshFilter.ResolveMesh())
            {
                if (auto transientMesh = TryConsumeImportedAssetDragPreviewMeshLoad(path, meshLoads))
                {
                    meshFilter.SetResolvedTransientMeshFromReference(std::move(transientMesh));
                    ++meshBindsThisFrame;
                    allBound = true;
                }
            }
        }
    }

    return allBound;
}

bool BindImportedAssetDragPreviewReadyRenderers(
    const std::vector<NLS::Editor::Panels::ImportedPrefabDragPreviewRendererEntry>& rendererEntries,
    std::unordered_map<std::string, std::shared_ptr<NLS::Editor::Core::PrefabInstanceMeshArtifactLoadState>>& meshLoads,
    std::unordered_set<std::string>& materialRequests,
    std::unordered_set<std::string>& textureRequests,
    size_t& textureBindsThisFrame,
    bool& foundRenderable,
    size_t& meshBindsThisFrame,
    const std::string& ownerToken)
{
    bool allBound = true;
    for (const auto& entry : rendererEntries)
    {
        if (entry.meshFilter == nullptr || entry.meshRenderer == nullptr)
            continue;

        if (!BindImportedAssetDragPreviewReadyRenderer(
            *entry.meshFilter,
            *entry.meshRenderer,
            meshLoads,
            materialRequests,
            textureRequests,
            textureBindsThisFrame,
            foundRenderable,
            meshBindsThisFrame,
            ownerToken))
        {
            allBound = false;
        }
    }
    return allBound;
}

bool RequestImportedAssetDragPreviewMaterial(
    const NLS::Engine::Assets::PrefabResolvedAsset& resolved,
    std::unordered_set<std::string>& materialRequests,
    std::unordered_set<std::string>& textureRequests,
    size_t& textureBindsThisFrame,
    const std::string& ownerToken)
{
    if (!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MaterialManager>())
        return false;

    auto& materialManager = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager);
    auto* material = FindImportedAssetDragPreviewCachedMaterial(materialManager, resolved.artifactPath);
    if (!material &&
        materialRequests.find(resolved.artifactPath) != materialRequests.end() &&
        !materialManager.IsAsyncArtifactLoadPending(resolved.artifactPath))
    {
        materialRequests.erase(resolved.artifactPath);
    }
    if (!material && materialRequests.find(resolved.artifactPath) == materialRequests.end())
    {
        AcquireImportedAssetDragPreviewResourceOwner(
            ownerToken,
            NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Material,
            resolved.artifactPath);
        material = materialManager.RequestAsyncArtifact(resolved.artifactPath, true);
        if (!material && materialManager.IsAsyncArtifactLoadPending(resolved.artifactPath))
            materialRequests.insert(resolved.artifactPath);
    }
    if (material)
    {
        (void)BindImportedAssetDragPreviewMaterialTextures(*material, textureRequests, textureBindsThisFrame, ownerToken);
        return true;
    }
    return materialRequests.find(resolved.artifactPath) != materialRequests.end();
}

bool RequestImportedAssetDragPreviewMaterialPath(
    const std::string& materialPath,
    std::unordered_set<std::string>& materialRequests,
    std::unordered_set<std::string>& textureRequests,
    size_t& textureBindsThisFrame,
    const std::string& ownerToken)
{
    NLS::Engine::Assets::PrefabResolvedAsset resolved;
    resolved.expectedType = "Material";
    resolved.artifactPath = materialPath;
    return RequestImportedAssetDragPreviewMaterial(
        resolved,
        materialRequests,
        textureRequests,
        textureBindsThisFrame,
        ownerToken);
}

void PumpImportedAssetDragPreviewResourceManagers(
    const std::unordered_set<std::string>& materialRequests,
    const std::unordered_set<std::string>& textureRequests)
{
    if (NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MaterialManager>())
    {
        auto& materialManager = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager);
        materialManager.PumpAsyncLoadsForPaths(
            materialRequests,
            kSceneViewDragPreviewMaterialPrewarmsPerFrame);
    }
    if (NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::TextureManager>())
    {
        auto& textureManager = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager);
        textureManager.PumpAsyncLoadsForPaths(
            textureRequests,
            kSceneViewDragPreviewTextureCompletionsPerFrame);
    }
}

void CancelImportedAssetDragPreviewAsyncResourceRequests(
    std::unordered_set<std::string>& materialRequests,
    std::unordered_set<std::string>& textureRequests)
{
    if (NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MaterialManager>())
    {
        auto& materialManager = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager);
        for (const auto& path : materialRequests)
            materialManager.CancelAsyncArtifact(path);
    }
    materialRequests.clear();
    if (NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::TextureManager>())
    {
        auto& textureManager = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager);
        for (const auto& path : textureRequests)
            textureManager.CancelAsyncArtifact(path);
    }
    textureRequests.clear();
}

ImGuizmo::OPERATION ToNativeImGuizmoOperation(Editor::Core::EGizmoOperation operation)
{
    switch (Editor::Core::ToImGuizmoOperation(operation))
    {
    case Editor::Core::SceneViewGizmoOperation::Rotate:
        return ImGuizmo::ROTATE;
    case Editor::Core::SceneViewGizmoOperation::Scale:
        return ImGuizmo::SCALE;
    case Editor::Core::SceneViewGizmoOperation::Translate:
    default:
        return ImGuizmo::TRANSLATE;
    }
}

ImGuizmo::MODE ToNativeImGuizmoMode(const Editor::Core::SceneViewGizmoSpace space)
{
    return space == Editor::Core::SceneViewGizmoSpace::Local
        ? ImGuizmo::LOCAL
        : ImGuizmo::WORLD;
}

Engine::GameObject* PickGameObjectAtRenderCoordinate(
    Editor::Rendering::PickingRenderPass& pickingPass,
    const float x,
    const float y)
{
    auto pickingResult = pickingPass.PickAtRenderCoordinate(
        static_cast<uint32_t>(x),
        static_cast<uint32_t>(y));

    if (pickingResult.has_value())
    {
        if (const auto pickedGameObject = std::get_if<Engine::GameObject*>(&pickingResult.value()))
            return *pickedGameObject;
    }

    return nullptr;
}

Engine::GameObject* PickGameObjectNearRenderCoordinate(
    Editor::Rendering::PickingRenderPass& pickingPass,
    const Maths::Vector2& mousePos,
    const float maxRenderX,
    const float maxRenderY)
{
    constexpr float kClickPickRadius = 2.0f;
    const std::array<Maths::Vector2, 9> offsets {{
        {0.0f, 0.0f},
        {-kClickPickRadius, 0.0f},
        {kClickPickRadius, 0.0f},
        {0.0f, -kClickPickRadius},
        {0.0f, kClickPickRadius},
        {-kClickPickRadius, -kClickPickRadius},
        {kClickPickRadius, -kClickPickRadius},
        {-kClickPickRadius, kClickPickRadius},
        {kClickPickRadius, kClickPickRadius},
    }};

    for (const auto& offset : offsets)
    {
        const float sampleX = std::clamp(mousePos.x + offset.x, 0.0f, maxRenderX);
        const float sampleY = std::clamp(mousePos.y + offset.y, 0.0f, maxRenderY);
        if (auto* pickedGameObject = PickGameObjectAtRenderCoordinate(pickingPass, sampleX, sampleY))
            return pickedGameObject;
    }

    return nullptr;
}

bool ShouldLogScenePickingDiagnostics()
{
    const auto& diagnostics = NLS::Render::Settings::GetThreadDiagnosticsSettings();
    return diagnostics.dx12LogFrameFlow || diagnostics.editorLogScenePicking;
}

bool ShouldLogSceneCameraInputDiagnostics()
{
    const auto& diagnostics = NLS::Render::Settings::GetThreadDiagnosticsSettings();
    if (diagnostics.dx12LogFrameFlow || diagnostics.editorLogSceneCameraInput)
        return true;

    if (NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::EditorActions>())
    {
        const auto& editorDiagnostics = EDITOR_EXEC(GetContext()).GetDiagnosticsSettings();
        return editorDiagnostics.dx12LogFrameFlow || editorDiagnostics.editorLogSceneCameraInput;
    }

    return false;
}

void LogScenePickingDiagnostics(const std::string& message)
{
    if (ShouldLogScenePickingDiagnostics())
        NLS_LOG_INFO("[SceneViewPicking] " + message);
}

void LogSceneCameraInputDiagnostics(const std::string& message)
{
    if (ShouldLogSceneCameraInputDiagnostics())
        NLS_LOG_INFO("[SceneViewCamera] " + message);
}
}

class Editor::Panels::SceneView::ViewportDragDropTarget final : public UI::IPlugin
{
public:
    explicit ViewportDragDropTarget(SceneView& owner)
        : m_owner(owner)
    {
    }

    void Execute() override
    {
        m_owner.HandleViewportAssetDragDrop();
    }

private:
    SceneView& m_owner;
};

bool Editor::Panels::CanCommitImportedAssetDragPreviewRootOnRelease(
    const bool hasPreviewArtifact,
    const bool hasPreviewRoot)
{
    return hasPreviewArtifact && hasPreviewRoot;
}

Editor::Panels::SceneView::SceneView(
    const std::string& p_title,
    bool p_opened,
    const UI::PanelWindowSettings& p_windowSettings)
    : AViewControllable(p_title, p_opened, p_windowSettings), m_sceneManager(EDITOR_CONTEXT(sceneManager))
{
    // Scene View always renders editor overlays (grid/gizmo/light billboards),
    // so create the editor renderer during startup while the native startup
    // progress window is still visible.
    NLS_LOG_INFO("[Startup] Creating Scene View renderer");
    m_renderer = std::make_unique<Editor::Rendering::DebugSceneRenderer>(*EDITOR_CONTEXT(driver));
    SetRequiresRetiredFrameConsumption(true);
    SetRequiresImmediateRetiredFrameReadback(ShouldSceneViewRequestImmediatePickingReadback());

    m_camera.SetFar(5000.0f);
    m_image->AddPlugin<ViewportDragDropTarget>(*this);

    m_destroyedListener = Engine::GameObject::DestroyedEvent += [this](const Engine::GameObject& actor)
    {
        if (m_highlightedGameObject == &actor)
        {
            m_highlightedGameObject = nullptr;
        }
    };
}

Editor::Panels::SceneView::~SceneView()
{
    ClearImportedAssetDragPreview();
    Engine::GameObject::DestroyedEvent -= m_destroyedListener;
}

void Editor::Panels::SceneView::UpdateImportedAssetDragPreview(
    const NLS::Editor::Assets::EditorAssetDragPayload& payload)
{
    m_importedAssetDragPreviewPayload = payload;
    m_importedAssetDragPreviewMousePos = EDITOR_CONTEXT(inputManager)->GetMousePosition();
    m_importedAssetDragPreviewPlacement =
        ResolveImportedAssetDragPreviewPlacement(m_importedAssetDragPreviewMousePos);
    EnsureImportedAssetDragPreviewMeshGhost(payload);
    PumpImportedAssetDragPreviewResources();
    if (auto* previewRoot = m_importedAssetDragPreviewSession.GetRoot();
        previewRoot != nullptr && m_importedAssetDragPreviewPlacement.has_value())
    {
        m_importedAssetDragPreviewSession.UpdatePlacement(*m_importedAssetDragPreviewPlacement);
    }
}

bool Editor::Panels::SceneView::EnsureImportedAssetDragPreviewMeshGhost(
    const NLS::Editor::Assets::EditorAssetDragPayload& payload)
{
    const auto assetGuid = NLS::Editor::Assets::GetEditorAssetDragPayloadGuid(payload);
    const auto subAssetKey = NLS::Editor::Assets::GetEditorAssetDragPayloadSubAssetKey(payload);
    if (m_importedAssetDragPreviewSession.GetRoot() != nullptr &&
        m_importedAssetDragPreviewSession.GetAssetGuid() == assetGuid &&
        m_importedAssetDragPreviewSession.GetSubAssetKey() == subAssetKey)
    {
        return true;
    }
    if (m_importedAssetDragPreviewMeshGhostUnavailable &&
        m_importedAssetDragPreviewSession.GetAssetGuid() == assetGuid &&
        m_importedAssetDragPreviewSession.GetSubAssetKey() == subAssetKey)
    {
        if (std::chrono::steady_clock::now() < m_importedAssetDragPreviewNextMeshGhostRetryTime)
        {
            return false;
        }

        m_importedAssetDragPreviewMeshGhostUnavailable = false;
    }

    m_importedAssetDragPreviewSession.Clear();
    m_importedAssetDragPreviewArtifact.reset();
    m_importedAssetDragPreviewRenderableReady = false;
    CancelImportedAssetDragPreviewMeshLoads(m_importedAssetDragPreviewPrewarmRequest.meshLoadsByPath);
    ReleaseImportedAssetDragPreviewResourceOwner(m_importedAssetDragPreviewPrewarmRequest.ownerToken);
    CancelImportedAssetDragPreviewAsyncResourceRequests(
        m_importedAssetDragPreviewPrewarmRequest.materialLoadsByPath,
        m_importedAssetDragPreviewPrewarmRequest.textureLoadsByPath);
    m_importedAssetDragPreviewPrewarmRequest = {};
    m_importedAssetDragPreviewPrewarmRequest.ownerToken =
        assetGuid + ":" + subAssetKey + ":" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    m_importedAssetDragPreviewMeshGhostUnavailable = false;
    m_importedAssetDragPreviewRenderableReady = false;
    m_importedAssetDragPreviewNextMeshGhostRetryTime = {};

    const auto assetPath = NLS::Editor::Assets::GetEditorAssetDragPayloadPath(payload);
    if (assetPath.empty())
    {
        m_importedAssetDragPreviewMeshGhostUnavailable = true;
        m_importedAssetDragPreviewNextMeshGhostRetryTime =
            std::chrono::steady_clock::now() + kSceneViewDragPreviewRetryDelay;
        return false;
    }
    NLS::Editor::Assets::EditorAssetDragDropBridge dragDropBridge(
        std::filesystem::path(EDITOR_CONTEXT(projectAssetsPath)));
    std::shared_ptr<const NLS::Engine::Assets::PrefabArtifact> prefab;
    if (!IsImportedPrefabPreviewPreloadInFlight(payload))
        prefab = dragDropBridge.TryGetCachedPreviewPrefabArtifactShared(payload);
    if (!prefab)
    {
        (void)ScheduleImportedPrefabPreviewPreloadOnce(payload);
        m_importedAssetDragPreviewMeshGhostUnavailable = true;
        m_importedAssetDragPreviewNextMeshGhostRetryTime =
            std::chrono::steady_clock::now() + kSceneViewDragPreviewRetryDelay;
        return false;
    }

    auto* scene = GetScene();
    if (scene == nullptr)
    {
        m_importedAssetDragPreviewMeshGhostUnavailable = true;
        m_importedAssetDragPreviewNextMeshGhostRetryTime =
            std::chrono::steady_clock::now() + kSceneViewDragPreviewRetryDelay;
        return false;
    }

    m_importedAssetDragPreviewArtifact = std::move(prefab);

    auto preview = m_importedAssetDragPreviewSession.BeginOrUpdate(
        *m_importedAssetDragPreviewArtifact,
        *scene,
        assetGuid,
        subAssetKey,
        m_importedAssetDragPreviewPlacement.value_or(Maths::Vector3::Zero));
    if (preview.diagnostics.HasErrors() || preview.root == nullptr)
    {
        m_importedAssetDragPreviewArtifact.reset();
        m_importedAssetDragPreviewMeshGhostUnavailable = true;
        m_importedAssetDragPreviewNextMeshGhostRetryTime =
            std::chrono::steady_clock::now() + kSceneViewDragPreviewRetryDelay;
        return false;
    }

    return true;
}

std::optional<Maths::Vector3> Editor::Panels::SceneView::ResolveImportedAssetDragPreviewPlacement(
    const Maths::Vector2& mousePosition) const
{
    if (m_camera.transform == nullptr)
        return std::nullopt;

    const auto localPosition = GetLocalViewPosition(mousePosition);
    const auto [safeWidth, safeHeight] = GetSafeSize();
    if (!localPosition.has_value() || safeWidth == 0u || safeHeight == 0u)
        return m_camera.GetPosition() + m_camera.transform->GetWorldForward() * kSceneViewDragPreviewFallbackDistance;

    const float width = std::max(1.0f, static_cast<float>(safeWidth));
    const float height = std::max(1.0f, static_cast<float>(safeHeight));
    const float ndcX = (localPosition->x / width) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (localPosition->y / height) * 2.0f;
    const float aspect = width / height;
    const float tanHalfFov = std::tan(Maths::DegreesToRadians(m_camera.GetFov()) * 0.5f);
    // Matrix4::CreateView uses eye - look, so this view matrix maps world right to screen-left.
    const float screenRightNdcX = -ndcX;

    auto rayDirection =
        m_camera.transform->GetWorldForward() +
        m_camera.transform->GetWorldRight() * (screenRightNdcX * tanHalfFov * aspect) +
        m_camera.transform->GetWorldUp() * (ndcY * tanHalfFov);
    rayDirection.Normalise();

    const auto cameraPosition = m_camera.GetPosition();
    if (std::fabs(rayDirection.y) > Maths::SMALL_NUMBER)
    {
        const float distanceToGround = -cameraPosition.y / rayDirection.y;
        if (distanceToGround > 0.0f && distanceToGround < m_camera.GetFar())
            return cameraPosition + rayDirection * distanceToGround;
    }

    const float fallbackDistance = m_cameraFocus.hasFocus
        ? std::max(1.0f, m_cameraFocus.focusDistance)
        : kSceneViewDragPreviewFallbackDistance;
    return cameraPosition + rayDirection * fallbackDistance;
}

void Editor::Panels::SceneView::HandleViewportAssetDragDrop()
{
    if (!UI::BeginDragDropTarget())
    {
        const auto mousePosition = EDITOR_CONTEXT(inputManager)->GetMousePosition();
        const UI::DragDropPayloadView activePayload =
            UI::PeekDragDropPayload(NLS::Editor::Assets::kEditorAssetDragPayloadType);
        if (activePayload.data != nullptr &&
            !activePayload.delivered &&
            (IsMouseWithinView(mousePosition) ||
                (m_image != nullptr && m_image->WasHoveredLastDraw())))
        {
            const auto payload =
                *static_cast<const NLS::Editor::Assets::EditorAssetDragPayload*>(activePayload.data);
            UpdateImportedAssetDragPreview(payload);
            return;
        }
        ClearImportedAssetDragPreview();
        return;
    }

    if (const UI::DragDropPayloadView payloadView = UI::AcceptDragDropPayload(
        NLS::Editor::Assets::kEditorAssetDragPayloadType,
        UI::DragDropTargetFlags::AcceptBeforeDelivery);
        payloadView.data != nullptr)
    {
        const auto payload = *static_cast<const NLS::Editor::Assets::EditorAssetDragPayload*>(payloadView.data);
        if (payloadView.delivered)
        {
            if (!CanCommitImportedAssetDragPreviewRootOnRelease(
                    m_importedAssetDragPreviewArtifact != nullptr,
                    m_importedAssetDragPreviewSession.GetRoot() != nullptr))
            {
                auto previewPlacement = m_importedAssetDragPreviewPlacement;
                if (!previewPlacement.has_value())
                    previewPlacement =
                        ResolveImportedAssetDragPreviewPlacement(EDITOR_CONTEXT(inputManager)->GetMousePosition());
                ClearImportedAssetDragPreview();
                EDITOR_EXEC(CreateGameObjectFromAssetNonBlocking(payload, true, nullptr, previewPlacement));
                UI::EndDragDropTarget();
                return;
            }

            auto previewCommitHandoff = m_importedAssetDragPreviewSession.EndForCommit();
            auto previewPlacement = previewCommitHandoff.placement;
            if (!previewPlacement.has_value())
                previewPlacement = m_importedAssetDragPreviewPlacement;
            if (!previewPlacement.has_value())
                previewPlacement =
                    ResolveImportedAssetDragPreviewPlacement(EDITOR_CONTEXT(inputManager)->GetMousePosition());
            auto previewArtifact = std::move(m_importedAssetDragPreviewArtifact);
            auto previewResourceHandoff = CollectImportedAssetDragPreviewResourceHandoff();
            const bool previewRenderableReady = m_importedAssetDragPreviewRenderableReady;
            ClearImportedAssetDragPreview(false);
            if (previewArtifact && previewCommitHandoff.root != nullptr)
            {
                EDITOR_EXEC(CommitGameObjectFromImportedPrefabPreview(
                    payload,
                    std::move(previewArtifact),
                    *previewCommitHandoff.root,
                    true,
                    nullptr,
                    previewPlacement,
                    std::move(previewResourceHandoff),
                    previewRenderableReady));
            }
            else
            {
                if (previewCommitHandoff.root != nullptr)
                {
                    previewCommitHandoff.root->SetEditorTransient(true);
                    if (auto* scene = GetScene())
                    {
                        scene->DestroyGameObject(*previewCommitHandoff.root);
                        scene->CollectGarbages();
                    }
                    else
                    {
                        previewCommitHandoff.root->MarkAsDestroy();
                    }
                }
                EDITOR_EXEC(CreateGameObjectFromAssetNonBlocking(payload, true, nullptr, previewPlacement));
            }
        }
        else
        {
            UpdateImportedAssetDragPreview(payload);
        }

        UI::EndDragDropTarget();
        return;
    }

    if (const UI::DragDropPayloadView payloadView = UI::AcceptDragDropPayload("File", UI::DragDropTargetFlags::None);
        payloadView.data != nullptr)
    {
        const auto payload = *static_cast<const std::pair<std::string, UI::Widgets::Group*>*>(payloadView.data);
        const std::string path = payload.first;

        switch (Utils::PathParser::GetFileType(path))
        {
        case Utils::PathParser::EFileType::SCENE:
            EDITOR_EXEC(LoadSceneFromDisk(path));
            break;
        default:
            if (!NLS::Editor::Assets::IsBuiltInResourcePath(path))
                break;
            EDITOR_EXEC(CreateGameObjectFromAsset(path, true));
            break;
        }
    }

    UI::EndDragDropTarget();
}

void Editor::Panels::SceneView::PumpImportedAssetDragPreviewBeforeRender()
{
    const auto mousePosition = EDITOR_CONTEXT(inputManager)->GetMousePosition();
    if (!IsMouseWithinView(mousePosition) &&
        !(m_image != nullptr && m_image->WasHoveredLastDraw()))
    {
        return;
    }

    const UI::DragDropPayloadView payloadView =
        UI::PeekDragDropPayload(NLS::Editor::Assets::kEditorAssetDragPayloadType);
    if (payloadView.delivered)
        return;

    if (payloadView.data == nullptr)
    {
        if (m_importedAssetDragPreviewPayload.has_value())
        {
            UpdateImportedAssetDragPreview(*m_importedAssetDragPreviewPayload);
        }
        return;
    }

    const auto payload =
        *static_cast<const NLS::Editor::Assets::EditorAssetDragPayload*>(payloadView.data);
    UpdateImportedAssetDragPreview(payload);
}

void Editor::Panels::SceneView::PumpImportedAssetDragPreviewResources()
{
    auto* previewRoot = m_importedAssetDragPreviewSession.GetRoot();
    if (!m_importedAssetDragPreviewArtifact ||
        previewRoot == nullptr)
    {
        return;
    }

    PumpImportedAssetDragPreviewResourceManagers(
        m_importedAssetDragPreviewPrewarmRequest.materialLoadsByPath,
        m_importedAssetDragPreviewPrewarmRequest.textureLoadsByPath);

    auto shouldSkipPreviewResource = [this](const NLS::Engine::Assets::PrefabResolvedAsset& resolved)
    {
        return !IsImportedAssetPreviewRendererResource(resolved) ||
            m_importedAssetDragPreviewPrewarmRequest.prewarmedResources.find(resolved.artifactPath) !=
                m_importedAssetDragPreviewPrewarmRequest.prewarmedResources.end();
    };

    size_t meshPrewarmedThisFrame = 0u;
    size_t materialPrewarmedThisFrame = 0u;
    size_t previewTextureBindsThisFrame = 0u;
    const auto& prioritizedMeshPaths = m_importedAssetDragPreviewSession.GetCachedMeshPaths();
    const auto& prioritizedMaterialPaths = m_importedAssetDragPreviewSession.GetCachedMaterialPaths();
    for (const auto& meshPath : prioritizedMeshPaths)
    {
        if (meshPrewarmedThisFrame >= kSceneViewDragPreviewMeshPrewarmsPerFrame)
            break;
        if (m_importedAssetDragPreviewPrewarmRequest.prewarmedResources.find(meshPath) !=
            m_importedAssetDragPreviewPrewarmRequest.prewarmedResources.end())
        {
            continue;
        }

        AcquireImportedAssetDragPreviewResourceOwner(
            m_importedAssetDragPreviewPrewarmRequest.ownerToken,
            NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Mesh,
            meshPath);
        const bool accepted = StartImportedAssetDragPreviewMeshLoad(
            meshPath,
            m_importedAssetDragPreviewPrewarmRequest.meshLoadsByPath);
        if (accepted)
        {
            m_importedAssetDragPreviewPrewarmRequest.prewarmedResources.insert(meshPath);
            ++meshPrewarmedThisFrame;
        }
    }

    for (const auto& materialPath : prioritizedMaterialPaths)
    {
        if (materialPrewarmedThisFrame >= kSceneViewDragPreviewMaterialPrewarmsPerFrame)
            break;
        if (m_importedAssetDragPreviewPrewarmRequest.prewarmedResources.find(materialPath) !=
            m_importedAssetDragPreviewPrewarmRequest.prewarmedResources.end())
        {
            continue;
        }

        const bool accepted = RequestImportedAssetDragPreviewMaterialPath(
            materialPath,
            m_importedAssetDragPreviewPrewarmRequest.materialLoadsByPath,
            m_importedAssetDragPreviewPrewarmRequest.textureLoadsByPath,
            previewTextureBindsThisFrame,
            m_importedAssetDragPreviewPrewarmRequest.ownerToken);
        if (accepted)
        {
            m_importedAssetDragPreviewPrewarmRequest.prewarmedResources.insert(materialPath);
            ++materialPrewarmedThisFrame;
        }
    }

    for (const auto& resolved : m_importedAssetDragPreviewArtifact->resolvedAssets)
    {
        if (meshPrewarmedThisFrame >= kSceneViewDragPreviewMeshPrewarmsPerFrame)
            break;
        if (resolved.expectedType != "Mesh" || shouldSkipPreviewResource(resolved))
            continue;

        AcquireImportedAssetDragPreviewResourceOwner(
            m_importedAssetDragPreviewPrewarmRequest.ownerToken,
            NLS::Core::ResourceManagement::ResourceLifetimeResourceType::Mesh,
            resolved.artifactPath);
        const bool accepted = StartImportedAssetDragPreviewMeshLoad(
            resolved.artifactPath,
            m_importedAssetDragPreviewPrewarmRequest.meshLoadsByPath);
        if (accepted)
        {
            m_importedAssetDragPreviewPrewarmRequest.prewarmedResources.insert(resolved.artifactPath);
            ++meshPrewarmedThisFrame;
        }
    }

    for (const auto& resolved : m_importedAssetDragPreviewArtifact->resolvedAssets)
    {
        if (materialPrewarmedThisFrame >= kSceneViewDragPreviewMaterialPrewarmsPerFrame)
            break;
        if (resolved.expectedType != "Material" || shouldSkipPreviewResource(resolved))
            continue;

        const bool accepted = RequestImportedAssetDragPreviewMaterial(
            resolved,
            m_importedAssetDragPreviewPrewarmRequest.materialLoadsByPath,
            m_importedAssetDragPreviewPrewarmRequest.textureLoadsByPath,
            previewTextureBindsThisFrame,
            m_importedAssetDragPreviewPrewarmRequest.ownerToken);
        if (accepted)
        {
            m_importedAssetDragPreviewPrewarmRequest.prewarmedResources.insert(resolved.artifactPath);
            ++materialPrewarmedThisFrame;
        }
    }

    size_t textureBindsThisFrame = 0u;
    bool foundRenderable = false;
    size_t meshBindsThisFrame = 0u;
    m_importedAssetDragPreviewRenderableReady = BindImportedAssetDragPreviewReadyRenderers(
        m_importedAssetDragPreviewSession.GetCachedRendererEntries(),
        m_importedAssetDragPreviewPrewarmRequest.meshLoadsByPath,
        m_importedAssetDragPreviewPrewarmRequest.materialLoadsByPath,
        m_importedAssetDragPreviewPrewarmRequest.textureLoadsByPath,
        textureBindsThisFrame,
        foundRenderable,
        meshBindsThisFrame,
        m_importedAssetDragPreviewPrewarmRequest.ownerToken);
    if (!foundRenderable)
        m_importedAssetDragPreviewRenderableReady = false;
}

NLS::Editor::Core::PrefabInstancePreviewResourceHandoff
Editor::Panels::SceneView::CollectImportedAssetDragPreviewResourceHandoff()
{
    return CollectImportedAssetDragPreviewMeshes(m_importedAssetDragPreviewPrewarmRequest);
}

void Editor::Panels::SceneView::DrawImportedAssetDragPreview()
{
    // The additive preview scene is rendered through CreateSceneDescriptor().
    // Pending imported-model drags intentionally draw no proxy so mesh/material/texture never appear out of sync.
}

void Editor::Panels::SceneView::ClearImportedAssetDragPreview(const bool cancelAsyncResourceRequests)
{
    m_importedAssetDragPreviewPayload.reset();
    m_importedAssetDragPreviewArtifact.reset();
    m_importedAssetDragPreviewSession.Clear();
    m_importedAssetDragPreviewMeshGhostUnavailable = false;
    m_importedAssetDragPreviewRenderableReady = false;
    m_importedAssetDragPreviewNextMeshGhostRetryTime = {};
    m_importedAssetDragPreviewPrewarmRequest.prewarmedResources.clear();
    if (cancelAsyncResourceRequests)
    {
        CancelImportedAssetDragPreviewMeshLoads(m_importedAssetDragPreviewPrewarmRequest.meshLoadsByPath);
        CancelImportedAssetDragPreviewAsyncResourceRequests(
            m_importedAssetDragPreviewPrewarmRequest.materialLoadsByPath,
            m_importedAssetDragPreviewPrewarmRequest.textureLoadsByPath);
        ReleaseImportedAssetDragPreviewResourceOwner(m_importedAssetDragPreviewPrewarmRequest.ownerToken);
    }
    else
    {
        m_importedAssetDragPreviewPrewarmRequest.materialLoadsByPath.clear();
        m_importedAssetDragPreviewPrewarmRequest.textureLoadsByPath.clear();
    }
    m_importedAssetDragPreviewPrewarmRequest.ownerToken.clear();
    m_importedAssetDragPreviewPlacement.reset();
}

void Editor::Panels::SceneView::EnsureRenderer()
{
    if (m_renderer != nullptr)
        return;

    NLS_LOG_INFO("[Startup] Creating Scene View renderer");
    m_renderer = std::make_unique<Editor::Rendering::DebugSceneRenderer>(*EDITOR_CONTEXT(driver));
}

void Editor::Panels::SceneView::Update(float p_deltaTime)
{
    using namespace Windowing::Inputs;
    const auto previousCameraPosition = m_camera.GetPosition();
    const auto previousCameraRotation = m_camera.GetRotation();
    const Maths::Vector2 mousePosition = EDITOR_CONTEXT(inputManager)->GetMousePosition();
    const bool shortcutsWindowOpen = Editor::Core::DoesShortcutSettingsWindowBlockSceneInput();
    const bool viewportInputAvailable = HasViewportImageInputBounds();
    const bool mouseOverSceneView = viewportInputAvailable &&
        (IsMouseWithinView(mousePosition) ||
            (m_image != nullptr && m_image->WasHoveredLastDraw()));
    const bool sceneViewActive = !shortcutsWindowOpen && (IsFocused() || IsHovered() || mouseOverSceneView);
    const bool isAnyItemActive = NLS_SERVICE(UI::UIManager).IsAnyItemActive();
    const bool blockCameraInput = ShouldSceneViewBlockCameraInput(
        shortcutsWindowOpen,
        isAnyItemActive,
        mouseOverSceneView,
        ImGui::GetIO().WantTextInput);
    const bool sceneViewInputContextActive = sceneViewActive && !(isAnyItemActive && !mouseOverSceneView);
    m_cameraController.SetInputBlocked(blockCameraInput);
    if (shortcutsWindowOpen)
    {
        m_cameraController.ResetMouseInteractionState();
        m_gizmoInteraction = {};
        m_pendingClickPickRenderPos.reset();
        m_pendingClickPickingSignature.reset();
        m_pendingClickMinReadablePickingFrameSerial = 0u;
        m_highlightedGameObject = nullptr;
        m_hasPickingSample = false;
    }
    EnsureCameraFocus();
    m_cameraController.SetFocusState(&m_cameraFocus);
    m_cameraController.SetInputActive(sceneViewInputContextActive);
    if (HasViewportImageBounds())
    {
        const auto imageMin = GetViewportImageMin();
        const auto imageMax = GetViewportImageMax();
        m_cameraController.SetViewportHeight(std::max(1.0f, imageMax.y - imageMin.y));
    }

    AViewControllable::Update(p_deltaTime);
    m_cameraMovedForPresentation = HasSceneViewCameraMotionForPresentation(
        previousCameraPosition,
        previousCameraRotation,
        m_camera.GetPosition(),
        m_camera.GetRotation());
    if (ShouldLogSceneCameraInputDiagnostics())
    {
        std::ostringstream stream;
        stream << "dt=" << p_deltaTime
            << " focused=" << IsFocused()
            << " hovered=" << IsHovered()
            << " mouseOver=" << mouseOverSceneView
            << " hasBounds=" << HasViewportImageBounds()
            << " active=" << sceneViewActive
            << " blockCamera=" << blockCameraInput
            << " shortcutsBlocked=" << shortcutsWindowOpen
            << " cameraControl=" << m_cameraController.IsCameraControlActive()
            << " moved=" << m_cameraMovedForPresentation
            << " mouse=(" << mousePosition.x << "," << mousePosition.y << ")"
            << " pos=(" << m_camera.GetPosition().x << "," << m_camera.GetPosition().y << "," << m_camera.GetPosition().z << ")"
            << " rot=(" << m_camera.GetRotation().x << "," << m_camera.GetRotation().y << "," << m_camera.GetRotation().z << "," << m_camera.GetRotation().w << ")";
        LogSceneCameraInputDiagnostics(stream.str());
    }
}

Editor::Core::EGizmoOperation Editor::Panels::SceneView::GetCurrentGizmoOperation() const
{
    return m_currentOperation;
}

void Editor::Panels::SceneView::SetCurrentGizmoOperation(const Editor::Core::EGizmoOperation p_operation)
{
    m_currentOperation = p_operation;
}

Editor::Core::SceneViewGizmoPivot Editor::Panels::SceneView::GetCurrentGizmoPivot() const
{
    return m_currentPivot;
}

void Editor::Panels::SceneView::SetCurrentGizmoPivot(const Editor::Core::SceneViewGizmoPivot p_pivot)
{
    m_currentPivot = p_pivot;
}

void Editor::Panels::SceneView::ToggleCurrentGizmoPivot()
{
    m_currentPivot = Core::ToggleGizmoPivot(m_currentPivot);
}

Editor::Core::SceneViewGizmoSpace Editor::Panels::SceneView::GetCurrentGizmoSpace() const
{
    return m_currentSpace;
}

void Editor::Panels::SceneView::SetCurrentGizmoSpace(const Editor::Core::SceneViewGizmoSpace p_space)
{
    m_currentSpace = p_space;
}

void Editor::Panels::SceneView::ToggleCurrentGizmoSpace()
{
    m_currentSpace = Core::ToggleGizmoSpace(m_currentSpace);
}

void Editor::Panels::SceneView::InitFrame()
{
    PumpImportedAssetDragPreviewBeforeRender();
    AViewControllable::InitFrame();

    auto* debugRenderer = dynamic_cast<Editor::Rendering::DebugSceneRenderer*>(m_renderer.get());
    if (debugRenderer == nullptr)
        return;

    Engine::GameObject* selectedGameObject = nullptr;

    if (EDITOR_EXEC(IsAnyGameObjectSelected()))
    {
        selectedGameObject = EDITOR_EXEC(GetSelectedGameObject());
    }

    m_requestPickingFrameForClick = ShouldRequestHitProxyPickingFrameForClick(
        EDITOR_CONTEXT(inputManager)->IsMouseButtonPressed(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_LEFT),
        m_pendingClickPickRenderPos.has_value(),
        m_pendingClickPickingSignature.has_value());
    m_requestPickingFrame = ShouldRequestPickingFrame();
    if (!m_requestPickingFrame)
        m_requestPickingFrameForClick = false;
    debugRenderer->AddDescriptor<Rendering::DebugSceneRenderer::DebugSceneDescriptor>({
        m_highlightedGameObject,
        selectedGameObject,
        m_requestPickingFrame,
        nullptr,
        m_requestPickingFrameForClick,
        kSceneViewHoverPickingVisibleDrawBudget,
        m_pendingClickMinReadablePickingFrameSerial});
}

Engine::SceneSystem::Scene* Editor::Panels::SceneView::GetScene()
{
    if (EDITOR_CONTEXT(activePrefabStage).has_value() && EDITOR_CONTEXT(activePrefabStage)->stageScene)
        return EDITOR_CONTEXT(activePrefabStage)->stageScene.get();

    return m_sceneManager.GetCurrentScene();
}

Engine::Rendering::BaseSceneRenderer::SceneDescriptor Editor::Panels::SceneView::CreateSceneDescriptor()
{
    return AViewControllable::CreateSceneDescriptor();
}

bool Editor::Panels::SceneView::ShouldUseStaticFrameCache() const
{
    return true;
}

uint64_t Editor::Panels::SceneView::BuildStaticFrameCacheKey(
    const Render::Entities::Camera& camera,
    const Engine::SceneSystem::Scene& scene,
    const uint16_t width,
    const uint16_t height) const
{
    const uint64_t baseKey = AViewControllable::BuildStaticFrameCacheKey(camera, scene, width, height);

    uint64_t highlightKey = BeginSceneViewCacheSegment(1u);
    HashSceneViewCachePointer(highlightKey, m_highlightedGameObject);

    uint64_t gizmoKey = BeginSceneViewCacheSegment(2u);
    HashSceneViewCacheValue(gizmoKey, static_cast<uint64_t>(m_currentOperation));
    HashSceneViewCacheValue(gizmoKey, static_cast<uint64_t>(m_currentPivot));
    HashSceneViewCacheValue(gizmoKey, static_cast<uint64_t>(m_currentSpace));
    HashSceneViewCacheValue(gizmoKey, m_gizmoInteraction.isHovered ? 1u : 0u);
    HashSceneViewCacheValue(gizmoKey, m_gizmoInteraction.isUsing ? 1u : 0u);
    HashSceneViewCacheValue(gizmoKey, m_gizmoInteraction.isViewHovered ? 1u : 0u);
    HashSceneViewCacheValue(gizmoKey, m_gizmoInteraction.isViewUsing ? 1u : 0u);

    uint64_t focusKey = BeginSceneViewCacheSegment(3u);
    HashSceneViewCacheValue(focusKey, m_cameraFocus.hasFocus ? 1u : 0u);
    HashSceneViewCacheFloat(focusKey, m_cameraFocus.focusDistance);
    HashSceneViewCacheVector3(focusKey, m_cameraFocus.focusPoint);

    uint64_t selectionKey = BeginSceneViewCacheSegment(4u);
    if (NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::EditorActions>())
    {
        HashSceneViewCacheValue(selectionKey, EDITOR_EXEC(IsAnyGameObjectSelected()) ? 1u : 0u);
        HashSceneViewCachePointer(selectionKey, EDITOR_EXEC(GetSelectedGameObject()));
        HashSceneViewCacheValue(
            selectionKey,
            EDITOR_EXEC(GetContext()).activePrefabStage.has_value() ? 1u : 0u);
    }

    uint64_t dragPreviewKey = BeginSceneViewCacheSegment(5u);
    HashSceneViewCacheValue(dragPreviewKey, m_importedAssetDragPreviewPayload.has_value() ? 1u : 0u);
    HashSceneViewCachePointer(dragPreviewKey, m_importedAssetDragPreviewSession.GetRoot());
    HashSceneViewCacheValue(dragPreviewKey, m_importedAssetDragPreviewRenderableReady ? 1u : 0u);
    HashSceneViewCacheValue(dragPreviewKey, m_importedAssetDragPreviewMeshGhostUnavailable ? 1u : 0u);
    HashSceneViewCacheVector2(dragPreviewKey, m_importedAssetDragPreviewMousePos);
    if (m_importedAssetDragPreviewPlacement.has_value())
    {
        HashSceneViewCacheValue(dragPreviewKey, 1u);
        HashSceneViewCacheVector3(dragPreviewKey, m_importedAssetDragPreviewPlacement.value());
    }
    else
    {
        HashSceneViewCacheValue(dragPreviewKey, 0u);
    }

    m_lastComputedStaticCacheBaseKey = baseKey;
    m_lastComputedStaticCacheHighlightKey = highlightKey;
    m_lastComputedStaticCacheGizmoKey = gizmoKey;
    m_lastComputedStaticCacheFocusKey = focusKey;
    m_lastComputedStaticCacheSelectionKey = selectionKey;
    m_lastComputedStaticCacheDragPreviewKey = dragPreviewKey;

    uint64_t seed = 0x51CE71E55EEDCACEull;
    HashSceneViewCacheValue(seed, baseKey);
    HashSceneViewCacheValue(seed, highlightKey);
    HashSceneViewCacheValue(seed, gizmoKey);
    HashSceneViewCacheValue(seed, focusKey);
    HashSceneViewCacheValue(seed, selectionKey);
    HashSceneViewCacheValue(seed, dragPreviewKey);
    return seed;
}

void Editor::Panels::SceneView::TraceStaticFrameCacheKeyChanged(
    const uint64_t previousKey,
    const uint64_t currentKey) const
{
    (void)previousKey;
    (void)currentKey;

    TraceSceneViewCacheSegmentChange(
        "SceneView::StaticCacheKeyChanged::BaseCameraSceneViewport",
        m_committedStaticCacheBaseKey,
        m_lastComputedStaticCacheBaseKey);
    TraceSceneViewCacheSegmentChange(
        "SceneView::StaticCacheKeyChanged::Highlight",
        m_committedStaticCacheHighlightKey,
        m_lastComputedStaticCacheHighlightKey);
    TraceSceneViewCacheSegmentChange(
        "SceneView::StaticCacheKeyChanged::Gizmo",
        m_committedStaticCacheGizmoKey,
        m_lastComputedStaticCacheGizmoKey);
    TraceSceneViewCacheSegmentChange(
        "SceneView::StaticCacheKeyChanged::Focus",
        m_committedStaticCacheFocusKey,
        m_lastComputedStaticCacheFocusKey);
    TraceSceneViewCacheSegmentChange(
        "SceneView::StaticCacheKeyChanged::Selection",
        m_committedStaticCacheSelectionKey,
        m_lastComputedStaticCacheSelectionKey);
    TraceSceneViewCacheSegmentChange(
        "SceneView::StaticCacheKeyChanged::DragPreview",
        m_committedStaticCacheDragPreviewKey,
        m_lastComputedStaticCacheDragPreviewKey);
}

void Editor::Panels::SceneView::CommitStaticFrameCacheKey(const uint64_t staticFrameCacheKey)
{
    AViewControllable::CommitStaticFrameCacheKey(staticFrameCacheKey);
    m_committedStaticCacheBaseKey = m_lastComputedStaticCacheBaseKey;
    m_committedStaticCacheHighlightKey = m_lastComputedStaticCacheHighlightKey;
    m_committedStaticCacheGizmoKey = m_lastComputedStaticCacheGizmoKey;
    m_committedStaticCacheFocusKey = m_lastComputedStaticCacheFocusKey;
    m_committedStaticCacheSelectionKey = m_lastComputedStaticCacheSelectionKey;
    m_committedStaticCacheDragPreviewKey = m_lastComputedStaticCacheDragPreviewKey;
}

bool Editor::Panels::SceneView::ShouldForceStaticFrameRender() const
{
    if (m_cameraMovedForPresentation || m_cameraController.IsCameraControlActive())
        return true;
    if (m_importedAssetDragPreviewPayload.has_value())
        return true;
    if (ShouldForceSceneViewStaticFrameRenderForPendingClick(m_pendingClickPickRenderPos.has_value()))
        return true;
    if (ShouldForceSceneViewStaticFrameRenderForPicking(
        ShouldRequestPickingFrame(),
        m_hasPickingSample,
        EDITOR_CONTEXT(inputManager)->IsMouseButtonPressed(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_LEFT) ||
            m_pendingClickPickRenderPos.has_value()))
    {
        return true;
    }
    if (NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::EditorActions>())
    {
        const auto& diagnostics = EDITOR_EXEC(GetContext()).GetDiagnosticsSettings();
        if (!m_validationReadbackWritten &&
            (!diagnostics.editorValidationSceneReadbackOutput.empty() ||
                !diagnostics.editorValidationSceneReadbackSummary.empty()))
        {
            return true;
        }
    }
    return false;
}

bool Editor::Panels::SceneView::RequiresSynchronizedRetiredFramePresentation() const
{
    return ShouldSceneViewSynchronizeRetiredFramePresentation();
}

void Editor::Panels::SceneView::DrawPreRenderViewportOverlay()
{
    DrawImportedAssetDragPreview();
    if (ShouldApplySceneMutationFromViewportOverlay(ViewportOverlayLifecyclePhase::BeforeViewRender))
        DrawViewportOverlay();
}

void Editor::Panels::SceneView::OnAfterDrawWidgets()
{
    if (ShouldResolveViewportPicking(ViewportOverlayLifecyclePhase::AfterWidgetDraw))
        HandleGameObjectPicking();
    EndViewportOverlayDrawListChannels();
    MarkViewportImageInputBoundsForLastDraw();
}

void Editor::Panels::SceneView::AfterRenderFrame()
{
    AViewControllable::AfterRenderFrame();
    TryWriteValidationReadback();
}

void Editor::Panels::SceneView::TryWriteValidationReadback()
{
    if (m_validationReadbackWritten)
        return;

    const auto& diagnostics = EDITOR_EXEC(GetContext()).GetDiagnosticsSettings();
    if (diagnostics.editorValidationSceneReadbackOutput.empty() &&
        diagnostics.editorValidationSceneReadbackSummary.empty())
    {
        return;
    }

    constexpr uint32_t kValidationReadbackWarmupFrames = 4u;
    if (m_validationReadbackWarmupFrames < kValidationReadbackWarmupFrames)
    {
        ++m_validationReadbackWarmupFrames;
        return;
    }

    if (!diagnostics.editorValidationCreateAsset.empty())
    {
        auto* selectedGameObject = EDITOR_EXEC(GetSelectedGameObject());
        if (selectedGameObject == nullptr)
        {
            m_validationReadbackReadyFrames = 0u;
            return;
        }

        if (EDITOR_EXEC(GetContext()).importProgressTracker.HasRunningJobs())
        {
            m_validationReadbackReadyFrames = 0u;
            return;
        }

        constexpr uint32_t kStableFramesAfterAssetCreation = 4u;
        if (m_validationReadbackReadyFrames < kStableFramesAfterAssetCreation)
        {
            ++m_validationReadbackReadyFrames;
            return;
        }
    }

    const auto width = static_cast<uint32_t>(m_lastResolvedViewSize.first);
    const auto height = static_cast<uint32_t>(m_lastResolvedViewSize.second);
    if (width == 0u || height == 0u)
        return;

    auto* driver = Render::Context::TryGetLocatedDriver();
    if (driver == nullptr)
        return;

    auto texture = m_fbo.GetExplicitTextureHandle();
    if (texture == nullptr ||
        !Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(*driver, texture))
    {
        m_validationReadbackReadyFrames = 0u;
        return;
    }

    if (Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(*driver) &&
        !Render::Context::DriverRendererAccess::TryDrainThreadedRendering(*driver))
    {
        return;
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0u);
    const auto readback = Render::Context::DriverRendererAccess::ReadPixelsChecked(
        *driver,
        texture,
        0u,
        0u,
        width,
        height,
        Render::Settings::EPixelDataFormat::RGBA,
        Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        pixels.data());

    if (!readback.Succeeded())
    {
        NLS_LOG_ERROR("Scene View validation readback failed: " + readback.message);
        m_validationReadbackWritten = true;
        EDITOR_CONTEXT(window)->SetShouldClose(true);
        return;
    }

    uint64_t nonBlackPixels = 0u;
    uint64_t nonZeroAlphaPixels = 0u;
    uint64_t rgbSum = 0u;
    uint8_t maxChannel = 0u;
    for (size_t index = 0u; index + 3u < pixels.size(); index += 4u)
    {
        const auto r = pixels[index + 0u];
        const auto g = pixels[index + 1u];
        const auto b = pixels[index + 2u];
        const auto a = pixels[index + 3u];
        if (r != 0u || g != 0u || b != 0u)
            ++nonBlackPixels;
        if (a != 0u)
            ++nonZeroAlphaPixels;
        rgbSum += static_cast<uint64_t>(r) + static_cast<uint64_t>(g) + static_cast<uint64_t>(b);
        maxChannel = std::max(maxChannel, std::max(r, std::max(g, b)));
    }

    const auto pixelCount = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    const double averageRgb = pixelCount > 0u
        ? static_cast<double>(rgbSum) / static_cast<double>(pixelCount * 3u)
        : 0.0;

    std::string pngError;
    if (!diagnostics.editorValidationSceneReadbackOutput.empty())
    {
        std::error_code error;
        std::filesystem::create_directories(
            std::filesystem::path(diagnostics.editorValidationSceneReadbackOutput).parent_path(),
            error);
        if (!Render::Tooling::WriteMaterialVisualEvidencePng(
            diagnostics.editorValidationSceneReadbackOutput,
            pixels.data(),
            width,
            height,
            4u,
            width * 4u,
            &pngError))
        {
            NLS_LOG_ERROR("Scene View validation PNG write failed: " + pngError);
        }
    }

    std::ostringstream summary;
    summary << "width=" << width << "\n";
    summary << "height=" << height << "\n";
    summary << "pixels=" << pixelCount << "\n";
    summary << "nonBlackPixels=" << nonBlackPixels << "\n";
    summary << "nonZeroAlphaPixels=" << nonZeroAlphaPixels << "\n";
    summary << "averageRgb=" << averageRgb << "\n";
    summary << "maxChannel=" << static_cast<uint32_t>(maxChannel) << "\n";
    summary << "readbackStatus=success\n";

    NLS_LOG_INFO("Scene View validation readback: " + summary.str());
    if (!diagnostics.editorValidationSceneReadbackSummary.empty())
    {
        std::error_code error;
        std::filesystem::create_directories(
            std::filesystem::path(diagnostics.editorValidationSceneReadbackSummary).parent_path(),
            error);
        std::ofstream output(diagnostics.editorValidationSceneReadbackSummary, std::ios::trunc);
        if (output)
            output << summary.str();
    }

    m_validationReadbackWritten = true;
    EDITOR_CONTEXT(window)->SetShouldClose(true);
}

void Editor::Panels::SceneView::DrawViewportOverlay()
{
    m_gizmoInteraction = {};
    if (Editor::Core::DoesShortcutSettingsWindowBlockSceneInput())
        return;

    const auto imageMin = HasViewportImageBounds()
        ? GetViewportImageMin()
        : GetCurrentViewportImageMin();
    const auto imageMax = HasViewportImageBounds()
        ? GetViewportImageMax()
        : GetCurrentViewportImageMax();
    const auto imageWidth = imageMax.x - imageMin.x;
    const auto imageHeight = imageMax.y - imageMin.y;
    if (imageWidth <= 0.0f || imageHeight <= 0.0f)
        return;

    const auto overlayMatrices = GetViewportOverlayCameraMatrices();
    auto viewMatrix = Core::ToImGuizmoMatrix(overlayMatrices.view);
    auto projectionMatrix = Core::ToImGuizmoMatrix(overlayMatrices.projection);
    const bool cameraControlActive = m_cameraController.IsCameraControlActive();
    EnsureCameraFocus();

    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(imageMin.x, imageMin.y, imageWidth, imageHeight);

    const auto viewGizmoRect = Core::GetSceneViewViewGizmoRect(imageMin, imageMax);
    auto viewGizmoModelMatrix = Core::ToImGuizmoMatrix(Maths::Matrix4::Identity);
    ImGuizmo::ViewManipulate(
        viewMatrix.data(),
        projectionMatrix.data(),
        ToNativeImGuizmoOperation(m_currentOperation),
        ToNativeImGuizmoMode(m_currentSpace),
        viewGizmoModelMatrix.data(),
        kSceneViewGizmoCameraLength,
        ImVec2(viewGizmoRect.position.x, viewGizmoRect.position.y),
        ImVec2(viewGizmoRect.size.x, viewGizmoRect.size.y),
        IM_COL32(0, 0, 0, 0));

    m_gizmoInteraction.isViewHovered = ImGuizmo::IsViewManipulateHovered();
    m_gizmoInteraction.isViewUsing = ImGuizmo::IsUsingViewManipulate();
    if (Core::ShouldCancelViewGizmoCameraTransform(cameraControlActive, m_gizmoInteraction.isViewUsing))
    {
        ImGuizmo::CancelViewManipulate();
        m_gizmoInteraction.isViewUsing = false;
    }

    const ImGuiIO& io = ImGui::GetIO();
    const Maths::Vector2 mouseDelta {io.MouseDelta.x, io.MouseDelta.y};
    if (!cameraControlActive && Core::ShouldApplyViewGizmoCameraTransform(
        m_gizmoInteraction.isViewUsing,
        io.MouseDown[0],
        mouseDelta))
    {
        const auto cameraTransform = Core::GetCameraTransformFromViewMatrix(viewMatrix);
        float viewGizmoTargetDirection[3] {};
        const bool hasViewGizmoTargetDirection =
            ImGuizmo::GetViewManipulateTargetDirection(viewGizmoTargetDirection);
        const Maths::Vector3 targetForward {
            viewGizmoTargetDirection[0],
            viewGizmoTargetDirection[1],
            viewGizmoTargetDirection[2]
        };
        const Maths::Vector3 targetCameraForward =
            Core::GetCameraForwardFromImGuizmoViewTargetDirection(targetForward);
        const auto stableCameraTransform = Core::StabilizeViewGizmoCameraTransform(
            {m_camera.GetPosition(), m_camera.GetRotation()},
            cameraTransform,
            m_cameraFocus.focusDistance,
            m_cameraFocus.focusPoint,
            hasViewGizmoTargetDirection ? &targetCameraForward : nullptr);
        m_camera.SetPosition(stableCameraTransform.position);
        m_camera.SetRotation(stableCameraTransform.rotation);
        m_cameraFocus.focusDistance = Maths::Vector3::Distance(m_camera.GetPosition(), m_cameraFocus.focusPoint);
        m_cameraFocus.hasFocus = true;
        m_camera.CacheViewMatrix();
        m_cameraMovedForPresentation = true;
    }

    if (!EDITOR_EXEC(IsAnyGameObjectSelected()))
        return;

    auto* selectedGameObject = EDITOR_EXEC(GetSelectedGameObject());
    if (selectedGameObject == nullptr || selectedGameObject->GetTransform() == nullptr)
        return;

    auto modelMatrix = Core::GetGameObjectWorldGizmoMatrix(*selectedGameObject, m_currentPivot);

    using namespace Windowing::Inputs;
    const bool snapEnabled = Core::IsSnapModifierActive(
        EDITOR_CONTEXT(inputManager)->GetKeyState(EKey::KEY_LEFT_CONTROL),
        EDITOR_CONTEXT(inputManager)->GetKeyState(EKey::KEY_RIGHT_CONTROL));
    const float snapValue = Core::GetSnapValue(m_currentOperation);
    float snap[3] { snapValue, snapValue, snapValue };

    const bool manipulated = ImGuizmo::Manipulate(
        viewMatrix.data(),
        projectionMatrix.data(),
        ToNativeImGuizmoOperation(m_currentOperation),
        ToNativeImGuizmoMode(m_currentSpace),
        modelMatrix.data(),
        nullptr,
        snapEnabled ? snap : nullptr);

    m_gizmoInteraction.isHovered = !cameraControlActive && ImGuizmo::IsOver();
    m_gizmoInteraction.isUsing = !cameraControlActive && ImGuizmo::IsUsing();

    if (!cameraControlActive && manipulated)
    {
        Core::ApplyGameObjectWorldGizmoMatrix(*selectedGameObject, modelMatrix, m_currentOperation, m_currentPivot);
        EDITOR_CONTEXT(sceneManager).MarkCurrentSceneDirty();
    }
}

void Editor::Panels::SceneView::EnsureCameraFocus()
{
    m_cameraFocus = Core::EnsureSceneCameraFocus(
        m_cameraFocus,
        m_camera.GetPosition(),
        m_camera.GetRotation() * Maths::Vector3::Forward,
        kSceneViewDefaultFocusDistance);
}

bool IsResizing()
{
    auto cursor = NLS_SERVICE(UI::UIManager).GetMouseCursor();

    return cursor == ImGuiMouseCursor_ResizeEW || cursor == ImGuiMouseCursor_ResizeNS || cursor == ImGuiMouseCursor_ResizeNWSE || cursor == ImGuiMouseCursor_ResizeNESW || cursor == ImGuiMouseCursor_ResizeAll;
}

bool Editor::Panels::SceneView::ShouldRequestPickingFrame() const
{
    using namespace Windowing::Inputs;
    if (Editor::Core::DoesShortcutSettingsWindowBlockSceneInput())
        return false;

    const bool pickingSuppressedByGizmo = Core::ShouldSuppressScenePicking(m_gizmoInteraction);
    if (pickingSuppressedByGizmo)
    {
        if (EDITOR_CONTEXT(inputManager)->IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT) ||
            m_pendingClickPickRenderPos.has_value())
        {
            std::ostringstream stream;
            stream << "request blocked by gizmo"
                << " leftPressed=" << EDITOR_CONTEXT(inputManager)->IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT)
                << " pendingClick=" << m_pendingClickPickRenderPos.has_value()
                << " hover=" << m_gizmoInteraction.isHovered
                << " using=" << m_gizmoInteraction.isUsing
                << " viewHover=" << m_gizmoInteraction.isViewHovered
                << " viewUsing=" << m_gizmoInteraction.isViewUsing;
            LogScenePickingDiagnostics(stream.str());
        }
        return false;
    }

    auto& inputManager = *EDITOR_CONTEXT(inputManager);
    const Maths::Vector2 screenMousePos = inputManager.GetMousePosition();
    const bool mouseOverView = IsMouseWithinView(screenMousePos);
    if (!mouseOverView || IsResizing())
    {
        if (inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT) ||
            m_pendingClickPickRenderPos.has_value())
        {
            std::ostringstream stream;
            stream << "request blocked by bounds"
                << " mouseOverView=" << mouseOverView
                << " resizing=" << IsResizing()
                << " mouse=(" << screenMousePos.x << "," << screenMousePos.y << ")"
                << " hasBounds=" << HasViewportImageBounds();
            LogScenePickingDiagnostics(stream.str());
        }
        return false;
    }

    const auto localMousePos = GetLocalViewPosition(screenMousePos);
    if (!localMousePos.has_value())
        return false;

    auto mousePos = localMousePos.value();
    const auto [safeWidth, safeHeight] = GetSafeSize();
    const float maxRenderX = std::max(0.0f, static_cast<float>(safeWidth) - 1.0f);
    const float maxRenderY = std::max(0.0f, static_cast<float>(safeHeight) - 1.0f);

    mousePos.x = std::clamp(mousePos.x, 0.0f, maxRenderX);
    mousePos.y = NLS_SERVICE(UI::UIManager).UsesBottomLeftRenderTargetOrigin()
        ? std::clamp(static_cast<float>(safeHeight) - 1.0f - mousePos.y, 0.0f, maxRenderY)
        : std::clamp(mousePos.y, 0.0f, maxRenderY);

    const auto now = std::chrono::steady_clock::now();
    const bool mouseMoved =
        std::fabs(mousePos.x - m_lastPickingMousePos.x) > 0.5f ||
        std::fabs(mousePos.y - m_lastPickingMousePos.y) > 0.5f;
    constexpr int kHoverPickingIntervalMs = 120;
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastPickingSampleTime).count();
    const bool sampleExpired =
        !m_hasPickingSample ||
        elapsedMs >= kHoverPickingIntervalMs;
    const bool leftClicked = inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT);
    const bool cameraControlActive = m_cameraController.IsCameraControlActive();
    if (!ShouldRequestHitProxyPickingFrameWhileClickReadbackPending(
        leftClicked,
        m_pendingClickPickRenderPos.has_value(),
        m_pendingClickPickingSignature.has_value()))
    {
        return false;
    }
    if (ShouldCancelScenePickingWhileCameraControlIsActive(
        cameraControlActive,
        m_pendingClickPickRenderPos.has_value()))
    {
        LogScenePickingDiagnostics("request cancelled pending click while camera control is active");
        return false;
    }

    const bool request = ShouldRenderScenePickingFrame(
        mouseOverView,
        false,
        false,
        m_pendingClickPickRenderPos.has_value() && !m_pendingClickPickingSignature.has_value(),
        leftClicked,
        cameraControlActive,
        m_cameraMovedForPresentation,
        sampleExpired,
        mouseMoved,
        m_hasPickingSample);
    const bool clickPickingFrame = ShouldRequestHitProxyPickingFrameForClick(
        leftClicked,
        m_pendingClickPickRenderPos.has_value(),
        m_pendingClickPickingSignature.has_value());
    if (request && !clickPickingFrame)
    {
        const auto* sceneRenderer = dynamic_cast<const Engine::Rendering::BaseSceneRenderer*>(m_renderer.get());
        if (sceneRenderer != nullptr &&
            sceneRenderer->HasLastVisiblePickablePrimitiveDrawSources() &&
            ShouldSkipSceneHoverPickingForVisibleDrawBudget(
                false,
                static_cast<uint64_t>(sceneRenderer->GetLastVisiblePickablePrimitiveDrawSources().size()),
                kSceneViewHoverPickingVisibleDrawBudget))
        {
            NLS_PROFILE_NAMED_SCOPE("SceneView::PickingSkipped::HoverVisibleDrawBudget");
            return false;
        }
    }
    if (leftClicked || m_pendingClickPickRenderPos.has_value())
    {
        std::ostringstream stream;
        stream << "request"
            << " result=" << request
            << " leftClicked=" << leftClicked
            << " pendingClick=" << m_pendingClickPickRenderPos.has_value()
            << " cameraControl=" << cameraControlActive
            << " sampleExpired=" << sampleExpired
            << " mouseMoved=" << mouseMoved
            << " hasSample=" << m_hasPickingSample
            << " local=(" << mousePos.x << "," << mousePos.y << ")";
        LogScenePickingDiagnostics(stream.str());
    }
    return request;
}

void Editor::Panels::SceneView::HandleGameObjectPicking()
{
    if (Editor::Core::DoesShortcutSettingsWindowBlockSceneInput())
    {
        m_highlightedGameObject = nullptr;
        m_hasPickingSample = false;
        return;
    }

    auto* debugRenderer = dynamic_cast<Editor::Rendering::DebugSceneRenderer*>(m_renderer.get());
    if (debugRenderer == nullptr)
    {
        m_highlightedGameObject = nullptr;
        m_hasPickingSample = false;
        return;
    }

    using namespace Windowing::Inputs;

    auto& inputManager = *EDITOR_CONTEXT(inputManager);
    const Maths::Vector2 screenMousePos = inputManager.GetMousePosition();
    const bool mouseOverView = IsMouseWithinView(screenMousePos);

    if (inputManager.IsMouseButtonReleased(EMouseButton::MOUSE_BUTTON_LEFT))
    {
        m_gizmoInteraction.isUsing = false;
    }

    if (Core::ShouldSuppressScenePicking(m_gizmoInteraction))
    {
        if (inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT) ||
            m_pendingClickPickRenderPos.has_value())
        {
            std::ostringstream stream;
            stream << "handle blocked by gizmo"
                << " leftPressed=" << inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT)
                << " leftReleased=" << inputManager.IsMouseButtonReleased(EMouseButton::MOUSE_BUTTON_LEFT)
                << " pendingClick=" << m_pendingClickPickRenderPos.has_value()
                << " hover=" << m_gizmoInteraction.isHovered
                << " using=" << m_gizmoInteraction.isUsing
                << " viewHover=" << m_gizmoInteraction.isViewHovered
                << " viewUsing=" << m_gizmoInteraction.isViewUsing;
            LogScenePickingDiagnostics(stream.str());
        }
        return;
    }

    if (mouseOverView && !IsResizing())
    {
        auto& gameObjectPickingFeature = debugRenderer->GetPass<Rendering::PickingRenderPass>("Picking");
        if (!gameObjectPickingFeature.SupportsPickingReadback())
        {
            m_highlightedGameObject = nullptr;
            m_hasPickingSample = false;
            return;
        }

        const auto localMousePos = GetLocalViewPosition(screenMousePos);
        if (!localMousePos.has_value())
        {
            m_highlightedGameObject = nullptr;
            m_hasPickingSample = false;
            return;
        }

        auto mousePos = localMousePos.value();
        const auto [safeWidth, safeHeight] = GetSafeSize();
        const float maxRenderX = std::max(0.0f, static_cast<float>(safeWidth) - 1.0f);
        const float maxRenderY = std::max(0.0f, static_cast<float>(safeHeight) - 1.0f);

        mousePos.x = std::clamp(mousePos.x, 0.0f, maxRenderX);
        mousePos.y = NLS_SERVICE(UI::UIManager).UsesBottomLeftRenderTargetOrigin()
            ? std::clamp(static_cast<float>(safeHeight) - 1.0f - mousePos.y, 0.0f, maxRenderY)
            : std::clamp(mousePos.y, 0.0f, maxRenderY);

        const auto now = std::chrono::steady_clock::now();
        const bool mouseMoved =
            std::fabs(mousePos.x - m_lastPickingMousePos.x) > 0.5f ||
            std::fabs(mousePos.y - m_lastPickingMousePos.y) > 0.5f;
        constexpr int kHoverPickingIntervalMs = 120;
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastPickingSampleTime).count();
        const bool sampleExpired =
            !m_hasPickingSample ||
            elapsedMs >= kHoverPickingIntervalMs;
        const bool leftClicked = inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT);
        const bool cameraControlActive = m_cameraController.IsCameraControlActive();
        if (ShouldCancelScenePickingWhileCameraControlIsActive(
            cameraControlActive,
            m_pendingClickPickRenderPos.has_value()))
        {
            LogScenePickingDiagnostics("handle cancelled pending click while camera control is active");
            m_pendingClickPickRenderPos.reset();
            m_pendingClickPickingSignature.reset();
            m_pendingClickMinReadablePickingFrameSerial = 0u;
        }
        const bool queuedClickPickThisFrame = leftClicked && !cameraControlActive;
        if (queuedClickPickThisFrame)
        {
            m_pendingClickPickRenderPos = mousePos;
            m_pendingClickMinReadablePickingFrameSerial = ComputePendingClickMinimumReadablePickingFrameSerial(
                gameObjectPickingFeature.GetSubmittedPickingFrameSerial(),
                m_requestPickingFrameForClick);
            m_pendingClickPickingSignature = m_requestPickingFrameForClick
                ? gameObjectPickingFeature.GetSubmittedPickingFrameSignature()
                : std::nullopt;
            std::ostringstream stream;
            stream << "queued click"
                << " local=(" << mousePos.x << "," << mousePos.y << ")"
                << " minReadableSerial=" << m_pendingClickMinReadablePickingFrameSerial
                << " currentReadableSerial=" << gameObjectPickingFeature.GetReadablePickingFrameSerial()
                << " requestThisFrame=" << m_requestPickingFrame;
            LogScenePickingDiagnostics(stream.str());
        }
        if (m_pendingClickPickRenderPos.has_value() &&
            !m_pendingClickPickingSignature.has_value() &&
            m_pendingClickMinReadablePickingFrameSerial > 0u &&
            gameObjectPickingFeature.GetSubmittedPickingFrameSerial() >= m_pendingClickMinReadablePickingFrameSerial)
        {
            m_pendingClickPickingSignature = gameObjectPickingFeature.GetSubmittedPickingFrameSignature();
        }

        const bool shouldRepick = ShouldRenderScenePickingFrame(
            mouseOverView,
            false,
            false,
            m_pendingClickPickRenderPos.has_value(),
            leftClicked,
            cameraControlActive,
            m_cameraMovedForPresentation,
            sampleExpired,
            mouseMoved,
            m_hasPickingSample);
        if (shouldRepick)
        {
            m_highlightedGameObject = nullptr;

            const bool pickingFrameReadable = gameObjectPickingFeature.HasReadablePickingFrame();
            const uint64_t readablePickingFrameSerial = gameObjectPickingFeature.GetReadablePickingFrameSerial();
            const bool resolvePendingClickPick =
                m_pendingClickPickRenderPos.has_value() &&
                !cameraControlActive &&
                m_pendingClickPickingSignature.has_value() &&
                gameObjectPickingFeature.CanResolvePickingRequest(
                    HitProxyPickingRequestKind::Click,
                    m_pendingClickMinReadablePickingFrameSerial,
                    m_pendingClickPickingSignature.value());
            if (queuedClickPickThisFrame || m_pendingClickPickRenderPos.has_value())
            {
                std::ostringstream stream;
                stream << "repick"
                    << " shouldRepick=" << shouldRepick
                    << " readable=" << pickingFrameReadable
                    << " readableSerial=" << readablePickingFrameSerial
                    << " minReadableSerial=" << m_pendingClickMinReadablePickingFrameSerial
                    << " hasSignature=" << m_pendingClickPickingSignature.has_value()
                    << " resolvePending=" << resolvePendingClickPick
                    << " requestThisFrame=" << m_requestPickingFrame;
                LogScenePickingDiagnostics(stream.str());
            }
            if (!cameraControlActive && pickingFrameReadable &&
                (!queuedClickPickThisFrame || resolvePendingClickPick))
            {
                const bool resolveClickPick = resolvePendingClickPick;
                if (resolveClickPick)
                {
                    NLS_PROFILE_NAMED_SCOPE("EditorPicking::ResolveClick");
                    const auto samplePos = m_pendingClickPickRenderPos.value();
                    m_highlightedGameObject =
                        PickGameObjectNearRenderCoordinate(gameObjectPickingFeature, samplePos, maxRenderX, maxRenderY);
                }
                else
                {
                    m_highlightedGameObject =
                        PickGameObjectAtRenderCoordinate(gameObjectPickingFeature, mousePos.x, mousePos.y);
                }
                if (m_highlightedGameObject != nullptr &&
                    m_importedAssetDragPreviewSession.ContainsObject(*m_highlightedGameObject))
                {
                    m_highlightedGameObject = nullptr;
                }
                if (resolveClickPick)
                {
                    std::ostringstream stream;
                    stream << "resolved click"
                        << " hit=" << (m_highlightedGameObject != nullptr)
                        << " actor=" << (m_highlightedGameObject != nullptr ? m_highlightedGameObject->GetName() : std::string("<none>"));
                    LogScenePickingDiagnostics(stream.str());
                }
            }
            else if (m_pendingClickPickRenderPos.has_value() && !pickingFrameReadable)
            {
                NLS_PROFILE_NAMED_SCOPE("EditorPicking::WaitReadback");
            }

            if (pickingFrameReadable)
            {
                m_lastPickingMousePos = mousePos;
                m_lastPickingSampleTime = now;
                m_hasPickingSample = true;
            }
        }
        else if (cameraControlActive)
        {
            m_highlightedGameObject = {};
        }

        const uint64_t readablePickingFrameSerial = gameObjectPickingFeature.GetReadablePickingFrameSerial();
        const bool resolvePendingClickPick =
            m_pendingClickPickRenderPos.has_value() &&
            !cameraControlActive &&
            m_pendingClickPickingSignature.has_value() &&
            gameObjectPickingFeature.CanResolvePickingRequest(
                HitProxyPickingRequestKind::Click,
                m_pendingClickMinReadablePickingFrameSerial,
                m_pendingClickPickingSignature.value());
        if (resolvePendingClickPick)
        {
            if (m_highlightedGameObject)
            {
                LogScenePickingDiagnostics("select GameObject=" + m_highlightedGameObject->GetName());
                EDITOR_EXEC(SelectGameObject(*m_highlightedGameObject));
                m_pendingClickPickRenderPos.reset();
                m_pendingClickPickingSignature.reset();
            }
            else if (gameObjectPickingFeature.HasReadablePickingFrame())
            {
                LogScenePickingDiagnostics("unselect GameObject from click");
                EDITOR_EXEC(UnselectGameObject());
                m_pendingClickPickRenderPos.reset();
                m_pendingClickPickingSignature.reset();
            }
            m_pendingClickMinReadablePickingFrameSerial = 0u;
        }
        else if (m_pendingClickPickRenderPos.has_value() &&
            m_pendingClickPickingSignature.has_value() &&
            m_pendingClickMinReadablePickingFrameSerial > 0u &&
            readablePickingFrameSerial >= m_pendingClickMinReadablePickingFrameSerial)
        {
            LogScenePickingDiagnostics("cancel pending click because readable picking signature changed");
            m_pendingClickPickRenderPos.reset();
            m_pendingClickPickingSignature.reset();
            m_pendingClickMinReadablePickingFrameSerial = 0u;
        }
    }
    else
    {
        if (inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT) ||
            m_pendingClickPickRenderPos.has_value())
        {
            std::ostringstream stream;
            stream << "handle blocked by bounds"
                << " mouseOverView=" << mouseOverView
                << " resizing=" << IsResizing()
                << " mouse=(" << screenMousePos.x << "," << screenMousePos.y << ")"
                << " hasBounds=" << HasViewportImageBounds();
            LogScenePickingDiagnostics(stream.str());
        }
        m_highlightedGameObject = nullptr;
        m_hasPickingSample = false;
        m_pendingClickPickRenderPos.reset();
        m_pendingClickPickingSignature.reset();
        m_pendingClickMinReadablePickingFrameSerial = 0u;
    }
}

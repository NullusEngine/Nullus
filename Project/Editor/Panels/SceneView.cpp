#include "ImGui/imgui.h"

#include "Rendering/DebugSceneRenderer.h"
#include "Rendering/PickingRenderPass.h"
#include "Rendering/SceneRendererFactory.h"
#include "Core/EditorActions.h"
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
#include "Core/EditorInteractionBlocker.h"
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
constexpr size_t kSceneViewDragPreviewResourcePrewarmsPerFrame = 64u;
constexpr size_t kSceneViewDragPreviewMeshPrewarmsPerFrame = 32u;
constexpr size_t kSceneViewDragPreviewMaterialPrewarmsPerFrame = 32u;
constexpr size_t kSceneViewDragPreviewTextureCompletionsPerFrame = 64u;
constexpr size_t kSceneViewDragPreviewMeshBindsPerFrame = 32u;
constexpr size_t kSceneViewDragPreviewMaxMeshVertices = 2048u;
constexpr size_t kSceneViewDragPreviewMaxMeshIndices = 6144u;

bool IsImportedAssetPreviewRendererResource(const NLS::Engine::Assets::PrefabResolvedAsset& resolved)
{
    return (resolved.expectedType == "Mesh" || resolved.expectedType == "Material") &&
        !resolved.artifactPath.empty();
}

NLS::Render::Assets::MeshArtifactData BuildImportedAssetDragPreviewMeshData(
    const NLS::Render::Assets::MeshArtifactData& source)
{
    if (source.vertices.size() <= kSceneViewDragPreviewMaxMeshVertices &&
        source.indices.size() <= kSceneViewDragPreviewMaxMeshIndices)
    {
        return source;
    }

    NLS::Render::Assets::MeshArtifactData preview;
    preview.materialIndex = source.materialIndex;
    preview.boundingSphere = source.boundingSphere;
    preview.hasBoundingSphere = source.hasBoundingSphere;

    if (source.indices.empty())
    {
        const auto vertexCount = std::min(source.vertices.size(), kSceneViewDragPreviewMaxMeshVertices);
        preview.vertices.insert(preview.vertices.end(), source.vertices.begin(), source.vertices.begin() + vertexCount);
        return preview;
    }

    std::unordered_map<uint32_t, uint32_t> remappedIndices;
    remappedIndices.reserve(std::min(source.vertices.size(), kSceneViewDragPreviewMaxMeshVertices));
    const size_t indexLimit = std::min(
        source.indices.size() - (source.indices.size() % 3u),
        kSceneViewDragPreviewMaxMeshIndices);

    for (size_t index = 0u; index + 2u < indexLimit; index += 3u)
    {
        const std::array<uint32_t, 3u> triangle {
            source.indices[index],
            source.indices[index + 1u],
            source.indices[index + 2u]
        };

        bool validTriangle = true;
        size_t newVertexCount = 0u;
        for (const auto sourceIndex : triangle)
        {
            if (sourceIndex >= source.vertices.size())
            {
                validTriangle = false;
                break;
            }
            if (remappedIndices.find(sourceIndex) == remappedIndices.end())
                ++newVertexCount;
        }
        if (!validTriangle)
            continue;
        if (preview.vertices.size() + newVertexCount > kSceneViewDragPreviewMaxMeshVertices)
            break;

        for (const auto sourceIndex : triangle)
        {
            auto found = remappedIndices.find(sourceIndex);
            if (found == remappedIndices.end())
            {
                const auto previewIndex = static_cast<uint32_t>(preview.vertices.size());
                preview.vertices.push_back(source.vertices[sourceIndex]);
                found = remappedIndices.emplace(sourceIndex, previewIndex).first;
            }
            preview.indices.push_back(found->second);
        }
    }

    return preview;
}

std::shared_ptr<NLS::Render::Resources::Mesh> CreateImportedAssetDragPreviewMesh(
    const NLS::Render::Assets::MeshArtifactData& source)
{
    auto preview = BuildImportedAssetDragPreviewMeshData(source);
    if (preview.vertices.empty())
        return {};

    return std::shared_ptr<NLS::Render::Resources::Mesh>(
        new NLS::Render::Resources::Mesh(
            preview.vertices,
            preview.indices,
            preview.materialIndex,
            NLS::Render::Resources::MeshBufferUploadMode::CpuToGpu,
            preview.boundingSphere));
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
    const bool accepted = EDITOR_EXEC(TrackBackgroundTask(
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
    std::unordered_map<std::string, std::shared_ptr<NLS::Editor::Core::PrefabInstanceMeshArtifactLoadState>>& loads,
    std::unordered_set<std::string>& materialRequests,
    std::unordered_set<std::string>& textureRequests)
{
    NLS::Editor::Core::PrefabInstancePreviewResourceHandoff handoff;
    for (auto& entry : loads)
    {
        if (!entry.second)
            continue;

        std::lock_guard lock(entry.second->mutex);
        entry.second->transientMesh.reset();
    }
    handoff.meshLoadsByPath = std::move(loads);
    handoff.materialLoadsByPath = std::move(materialRequests);
    handoff.textureLoadsByPath = std::move(textureRequests);
    loads.clear();
    materialRequests.clear();
    textureRequests.clear();
    return handoff;
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
    size_t& textureBindsThisFrame)
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
            texture = textureManager.RequestAsyncArtifact(texturePath, true);
            if (!texture && textureManager.IsAsyncArtifactLoadPending(texturePath))
                textureRequests.insert(texturePath);
        }
        if (!texture)
        {
            ready = false;
            continue;
        }

        material.Set<NLS::Render::Resources::Texture2D*>(uniformName, texture);
        ++textureBindsThisFrame;
    }

    return ready;
}

bool ImportedAssetDragPreviewMaterialsReady(
    NLS::Engine::Components::MeshRenderer& meshRenderer,
    std::unordered_set<std::string>& materialRequests,
    std::unordered_set<std::string>& textureRequests,
    size_t& textureBindsThisFrame)
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
            if (!BindImportedAssetDragPreviewMaterialTextures(*material, textureRequests, textureBindsThisFrame))
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
            meshRenderer.SetResolvedMaterialFromReference(static_cast<uint8_t>(index), *cached);
            if (meshRenderer.GetMaterialAtIndex(static_cast<uint8_t>(index)) != cached)
            {
                ready = false;
                continue;
            }
            if (!BindImportedAssetDragPreviewMaterialTextures(*cached, textureRequests, textureBindsThisFrame))
                ready = false;
            continue;
        }

        if (materialRequests.find(paths[index]) != materialRequests.end() &&
            !materialManager.IsAsyncArtifactLoadPending(paths[index]))
        {
            materialRequests.erase(paths[index]);
        }
        if (materialRequests.find(paths[index]) == materialRequests.end())
            materialManager.RequestAsyncArtifact(paths[index], true);
        if (materialManager.IsAsyncArtifactLoadPending(paths[index]))
            materialRequests.insert(paths[index]);
        ready = false;
    }

    return ready;
}

bool ImportedAssetDragPreviewMeshResourceReady(
    NLS::Engine::Components::MeshFilter& meshFilter,
    std::unordered_map<std::string, std::shared_ptr<NLS::Editor::Core::PrefabInstanceMeshArtifactLoadState>>& meshLoads)
{
    if (meshFilter.ResolveMesh() != nullptr)
        return true;

    const auto path = meshFilter.GetModelPath();
    if (path.empty())
        return false;

    if (NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MeshManager>())
    {
        auto& meshManager = NLS_SERVICE(NLS::Core::ResourceManagement::MeshManager);
        if (FindImportedAssetDragPreviewCachedMesh(meshManager, path) != nullptr)
            return true;
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

bool BindImportedAssetDragPreviewReadyRenderers(
    NLS::Engine::GameObject& object,
    std::unordered_map<std::string, std::shared_ptr<NLS::Editor::Core::PrefabInstanceMeshArtifactLoadState>>& meshLoads,
    std::unordered_set<std::string>& materialRequests,
    std::unordered_set<std::string>& textureRequests,
    size_t& textureBindsThisFrame,
    bool& foundRenderable,
    size_t& meshBindsThisFrame)
{
    bool allBound = true;
    auto* meshFilter = object.GetComponent<NLS::Engine::Components::MeshFilter>();
    auto* meshRenderer = object.GetComponent<NLS::Engine::Components::MeshRenderer>();

    if (meshFilter != nullptr && meshRenderer != nullptr)
    {
        foundRenderable = true;
        const bool meshResourceReady = ImportedAssetDragPreviewMeshResourceReady(*meshFilter, meshLoads);
        const bool materialsReady = ImportedAssetDragPreviewMaterialsReady(
            *meshRenderer,
            materialRequests,
            textureRequests,
            textureBindsThisFrame);
        if (!meshResourceReady || !materialsReady)
        {
            allBound = false;
        }
        else if (!meshFilter->ResolveMesh())
        {
            allBound = false;
            const auto path = meshFilter->GetModelPath();
            if (!path.empty() && meshBindsThisFrame < kSceneViewDragPreviewMeshBindsPerFrame)
            {
                if (NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MeshManager>())
                {
                    auto& meshManager = NLS_SERVICE(NLS::Core::ResourceManagement::MeshManager);
                    if (auto* cached = FindImportedAssetDragPreviewCachedMesh(meshManager, path))
                    {
                        meshFilter->SetResolvedMeshFromReference(cached);
                        ++meshBindsThisFrame;
                        allBound = true;
                    }
                }
                if (!meshFilter->ResolveMesh())
                {
                    if (auto transientMesh = TryConsumeImportedAssetDragPreviewMeshLoad(path, meshLoads))
                    {
                        meshFilter->SetResolvedTransientMeshFromReference(std::move(transientMesh));
                        ++meshBindsThisFrame;
                        allBound = true;
                    }
                }
            }
        }
    }

    for (auto* child : object.GetChildren())
    {
        if (child != nullptr)
        {
            if (!BindImportedAssetDragPreviewReadyRenderers(
                *child,
                meshLoads,
                materialRequests,
                textureRequests,
                textureBindsThisFrame,
                foundRenderable,
                meshBindsThisFrame))
            {
                allBound = false;
            }
        }
    }

    return allBound;
}

bool RequestImportedAssetDragPreviewMaterial(
    const NLS::Engine::Assets::PrefabResolvedAsset& resolved,
    std::unordered_set<std::string>& materialRequests,
    std::unordered_set<std::string>& textureRequests,
    size_t& textureBindsThisFrame)
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
        material = materialManager.RequestAsyncArtifact(resolved.artifactPath, true);
        if (!material && materialManager.IsAsyncArtifactLoadPending(resolved.artifactPath))
            materialRequests.insert(resolved.artifactPath);
    }
    if (material)
    {
        (void)BindImportedAssetDragPreviewMaterialTextures(*material, textureRequests, textureBindsThisFrame);
        return true;
    }
    return materialRequests.find(resolved.artifactPath) != materialRequests.end();
}

void PumpImportedAssetDragPreviewResourceManagers(
    const std::unordered_set<std::string>& textureRequests)
{
    if (NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MaterialManager>())
    {
        auto& materialManager = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager);
        materialManager.PumpAsyncLoads(kSceneViewDragPreviewMaterialPrewarmsPerFrame);
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
    if (m_importedAssetDragPreviewRoot && m_importedAssetDragPreviewPlacement.has_value())
        m_importedAssetDragPreviewRoot->GetTransform()->SetWorldPosition(*m_importedAssetDragPreviewPlacement);
}

bool Editor::Panels::SceneView::EnsureImportedAssetDragPreviewMeshGhost(
    const NLS::Editor::Assets::EditorAssetDragPayload& payload)
{
    const auto assetGuid = NLS::Editor::Assets::GetEditorAssetDragPayloadGuid(payload);
    const auto subAssetKey = NLS::Editor::Assets::GetEditorAssetDragPayloadSubAssetKey(payload);
    if (m_importedAssetDragPreviewRoot &&
        m_importedAssetDragPreviewAssetGuid == assetGuid &&
        m_importedAssetDragPreviewSubAssetKey == subAssetKey)
    {
        return true;
    }
    if (m_importedAssetDragPreviewMeshGhostUnavailable &&
        m_importedAssetDragPreviewAssetGuid == assetGuid &&
        m_importedAssetDragPreviewSubAssetKey == subAssetKey)
    {
        if (std::chrono::steady_clock::now() < m_importedAssetDragPreviewNextMeshGhostRetryTime)
            return false;

        m_importedAssetDragPreviewMeshGhostUnavailable = false;
    }

    if (m_importedAssetDragPreviewScene)
        m_retiredImportedAssetDragPreviewScenes.push_back(std::move(m_importedAssetDragPreviewScene));
    m_importedAssetDragPreviewArtifact.reset();
    m_importedAssetDragPreviewRoot = nullptr;
    m_importedAssetDragPreviewPrewarmedResources.clear();
    m_importedAssetDragPreviewRenderableReady = false;
    CancelImportedAssetDragPreviewMeshLoads(m_importedAssetDragPreviewMeshLoads);
    CancelImportedAssetDragPreviewAsyncResourceRequests(
        m_importedAssetDragPreviewMaterialRequests,
        m_importedAssetDragPreviewTextureRequests);
    m_importedAssetDragPreviewAssetGuid = assetGuid;
    m_importedAssetDragPreviewSubAssetKey = subAssetKey;
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
    auto prefab = dragDropBridge.TryLoadPreviewPrefabArtifact(payload);
    if (!prefab.has_value())
    {
        m_importedAssetDragPreviewMeshGhostUnavailable = true;
        m_importedAssetDragPreviewNextMeshGhostRetryTime =
            std::chrono::steady_clock::now() + kSceneViewDragPreviewRetryDelay;
        return false;
    }

    m_importedAssetDragPreviewArtifact = std::move(*prefab);

    auto previewScene = std::make_unique<NLS::Engine::SceneSystem::Scene>();
    NLS::Engine::Serialize::LoadPolicy previewLoadPolicy;
    previewLoadPolicy.deferAssetReferenceResolution = true;
    auto preview = NLS::Engine::Assets::InstantiatePrefabArtifact(
        *m_importedAssetDragPreviewArtifact,
        *previewScene,
        previewLoadPolicy);
    if (preview.diagnostics.HasErrors() || preview.root == nullptr)
    {
        m_importedAssetDragPreviewArtifact.reset();
        m_importedAssetDragPreviewMeshGhostUnavailable = true;
        m_importedAssetDragPreviewNextMeshGhostRetryTime =
            std::chrono::steady_clock::now() + kSceneViewDragPreviewRetryDelay;
        return false;
    }

    m_importedAssetDragPreviewRoot = preview.root;
    if (m_importedAssetDragPreviewPlacement.has_value())
        m_importedAssetDragPreviewRoot->GetTransform()->SetWorldPosition(*m_importedAssetDragPreviewPlacement);
    m_importedAssetDragPreviewScene = std::move(previewScene);
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
            auto previewPlacement =
                ResolveImportedAssetDragPreviewPlacement(EDITOR_CONTEXT(inputManager)->GetMousePosition());
            if (!previewPlacement.has_value())
                previewPlacement = m_importedAssetDragPreviewPlacement;
            auto previewArtifact = std::move(m_importedAssetDragPreviewArtifact);
            auto previewResourceHandoff = CollectImportedAssetDragPreviewResourceHandoff();
            ClearImportedAssetDragPreview(false);
            if (previewArtifact.has_value())
                EDITOR_EXEC(CreateGameObjectFromImportedPrefabArtifact(
                    payload,
                    *previewArtifact,
                    true,
                    nullptr,
                    previewPlacement,
                    std::move(previewResourceHandoff)));
            else
                EDITOR_EXEC(CreateGameObjectFromAsset(payload, true, nullptr, previewPlacement));
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
    if (payloadView.data == nullptr || payloadView.delivered)
        return;

    const auto payload =
        *static_cast<const NLS::Editor::Assets::EditorAssetDragPayload*>(payloadView.data);
    UpdateImportedAssetDragPreview(payload);
}

void Editor::Panels::SceneView::PumpImportedAssetDragPreviewResources()
{
    if (!m_importedAssetDragPreviewArtifact.has_value() ||
        m_importedAssetDragPreviewRoot == nullptr)
    {
        return;
    }

    PumpImportedAssetDragPreviewResourceManagers(m_importedAssetDragPreviewTextureRequests);

    auto shouldSkipPreviewResource = [this](const NLS::Engine::Assets::PrefabResolvedAsset& resolved)
    {
        return !IsImportedAssetPreviewRendererResource(resolved) ||
            m_importedAssetDragPreviewPrewarmedResources.find(resolved.artifactPath) !=
                m_importedAssetDragPreviewPrewarmedResources.end();
    };

    size_t meshPrewarmedThisFrame = 0u;
    for (const auto& resolved : m_importedAssetDragPreviewArtifact->resolvedAssets)
    {
        if (meshPrewarmedThisFrame >= kSceneViewDragPreviewMeshPrewarmsPerFrame)
            break;
        if (resolved.expectedType != "Mesh" || shouldSkipPreviewResource(resolved))
            continue;

        const bool accepted = StartImportedAssetDragPreviewMeshLoad(
            resolved.artifactPath,
            m_importedAssetDragPreviewMeshLoads);
        if (accepted)
        {
            m_importedAssetDragPreviewPrewarmedResources.insert(resolved.artifactPath);
            ++meshPrewarmedThisFrame;
        }
    }

    size_t materialPrewarmedThisFrame = 0u;
    size_t previewTextureBindsThisFrame = 0u;
    for (const auto& resolved : m_importedAssetDragPreviewArtifact->resolvedAssets)
    {
        if (materialPrewarmedThisFrame >= kSceneViewDragPreviewMaterialPrewarmsPerFrame)
            break;
        if (resolved.expectedType != "Material" || shouldSkipPreviewResource(resolved))
            continue;

        const bool accepted = RequestImportedAssetDragPreviewMaterial(
            resolved,
            m_importedAssetDragPreviewMaterialRequests,
            m_importedAssetDragPreviewTextureRequests,
            previewTextureBindsThisFrame);
        if (accepted)
        {
            m_importedAssetDragPreviewPrewarmedResources.insert(resolved.artifactPath);
            ++materialPrewarmedThisFrame;
        }
    }

    size_t textureBindsThisFrame = 0u;
    bool foundRenderable = false;
    size_t meshBindsThisFrame = 0u;
    m_importedAssetDragPreviewRenderableReady = BindImportedAssetDragPreviewReadyRenderers(
        *m_importedAssetDragPreviewRoot,
        m_importedAssetDragPreviewMeshLoads,
        m_importedAssetDragPreviewMaterialRequests,
        m_importedAssetDragPreviewTextureRequests,
        textureBindsThisFrame,
        foundRenderable,
        meshBindsThisFrame);
    if (!foundRenderable)
        m_importedAssetDragPreviewRenderableReady = false;
}

NLS::Editor::Core::PrefabInstancePreviewResourceHandoff
Editor::Panels::SceneView::CollectImportedAssetDragPreviewResourceHandoff()
{
    return CollectImportedAssetDragPreviewMeshes(
        m_importedAssetDragPreviewMeshLoads,
        m_importedAssetDragPreviewMaterialRequests,
        m_importedAssetDragPreviewTextureRequests);
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
    if (m_importedAssetDragPreviewScene)
        m_retiredImportedAssetDragPreviewScenes.push_back(std::move(m_importedAssetDragPreviewScene));
    m_importedAssetDragPreviewRoot = nullptr;
    m_importedAssetDragPreviewAssetGuid.clear();
    m_importedAssetDragPreviewSubAssetKey.clear();
    m_importedAssetDragPreviewMeshGhostUnavailable = false;
    m_importedAssetDragPreviewRenderableReady = false;
    m_importedAssetDragPreviewNextMeshGhostRetryTime = {};
    m_importedAssetDragPreviewPrewarmedResources.clear();
    CancelImportedAssetDragPreviewMeshLoads(m_importedAssetDragPreviewMeshLoads);
    if (cancelAsyncResourceRequests)
    {
        CancelImportedAssetDragPreviewAsyncResourceRequests(
            m_importedAssetDragPreviewMaterialRequests,
            m_importedAssetDragPreviewTextureRequests);
    }
    else
    {
        m_importedAssetDragPreviewMaterialRequests.clear();
        m_importedAssetDragPreviewTextureRequests.clear();
    }
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
    const bool mouseOverSceneView = IsMouseWithinView(mousePosition) ||
        (m_image != nullptr && m_image->WasHoveredLastDraw());
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

    m_requestPickingFrame = ShouldRequestPickingFrame();
    debugRenderer->AddDescriptor<Rendering::DebugSceneRenderer::DebugSceneDescriptor>({
        m_highlightedGameObject,
        selectedGameObject,
        m_requestPickingFrame,
        m_importedAssetDragPreviewScene.get()});
}

Engine::SceneSystem::Scene* Editor::Panels::SceneView::GetScene()
{
    if (EDITOR_CONTEXT(activePrefabStage).has_value() && EDITOR_CONTEXT(activePrefabStage)->stageScene)
        return EDITOR_CONTEXT(activePrefabStage)->stageScene.get();

    return m_sceneManager.GetCurrentScene();
}

Engine::Rendering::BaseSceneRenderer::SceneDescriptor Editor::Panels::SceneView::CreateSceneDescriptor()
{
    auto descriptor = AViewControllable::CreateSceneDescriptor();
    if (m_importedAssetDragPreviewScene)
        descriptor.additiveScenes.push_back(m_importedAssetDragPreviewScene.get());
    return descriptor;
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
}

void Editor::Panels::SceneView::AfterRenderFrame()
{
    AViewControllable::AfterRenderFrame();
    m_retiredImportedAssetDragPreviewScenes.clear();
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

    if (Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(*driver) &&
        !Render::Context::DriverRendererAccess::TryDrainThreadedRendering(*driver))
    {
        return;
    }

    auto texture = m_fbo.GetExplicitTextureHandle();
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

    const bool request = ShouldRenderScenePickingFrame(
        mouseOverView,
        false,
        false,
        m_pendingClickPickRenderPos.has_value(),
        leftClicked,
        cameraControlActive,
        sampleExpired,
        mouseMoved,
        m_hasPickingSample);
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
        const bool queuedClickPickThisFrame = leftClicked && !cameraControlActive;
        if (queuedClickPickThisFrame)
        {
            m_pendingClickPickRenderPos = mousePos;
            m_pendingClickMinReadablePickingFrameSerial =
                gameObjectPickingFeature.GetSubmittedPickingFrameSerial();
            std::ostringstream stream;
            stream << "queued click"
                << " local=(" << mousePos.x << "," << mousePos.y << ")"
                << " minReadableSerial=" << m_pendingClickMinReadablePickingFrameSerial
                << " currentReadableSerial=" << gameObjectPickingFeature.GetReadablePickingFrameSerial()
                << " requestThisFrame=" << m_requestPickingFrame;
            LogScenePickingDiagnostics(stream.str());
        }

        const bool shouldRepick = ShouldRenderScenePickingFrame(
            mouseOverView,
            false,
            false,
            m_pendingClickPickRenderPos.has_value(),
            leftClicked,
            cameraControlActive,
            sampleExpired,
            mouseMoved,
            m_hasPickingSample);
        if (shouldRepick)
        {
            m_highlightedGameObject = nullptr;

            const bool pickingFrameReadable = gameObjectPickingFeature.HasReadablePickingFrame();
            const bool resolvePendingClickPick = ShouldResolvePendingSceneClickPick(
                m_pendingClickPickRenderPos.has_value(),
                queuedClickPickThisFrame,
                cameraControlActive,
                m_pendingClickMinReadablePickingFrameSerial,
                gameObjectPickingFeature.GetReadablePickingFrameSerial());
            if (queuedClickPickThisFrame || m_pendingClickPickRenderPos.has_value())
            {
                std::ostringstream stream;
                stream << "repick"
                    << " shouldRepick=" << shouldRepick
                    << " readable=" << pickingFrameReadable
                    << " readableSerial=" << gameObjectPickingFeature.GetReadablePickingFrameSerial()
                    << " minReadableSerial=" << m_pendingClickMinReadablePickingFrameSerial
                    << " resolvePending=" << resolvePendingClickPick
                    << " requestThisFrame=" << m_requestPickingFrame;
                LogScenePickingDiagnostics(stream.str());
            }
            if (!cameraControlActive && pickingFrameReadable &&
                (!queuedClickPickThisFrame || resolvePendingClickPick))
            {
                const bool resolveClickPick = resolvePendingClickPick;
                const auto samplePos = resolveClickPick
                    ? m_pendingClickPickRenderPos.value()
                    : mousePos;
                m_highlightedGameObject = resolveClickPick
                    ? PickGameObjectNearRenderCoordinate(gameObjectPickingFeature, samplePos, maxRenderX, maxRenderY)
                    : PickGameObjectAtRenderCoordinate(gameObjectPickingFeature, mousePos.x, mousePos.y);
                if (resolveClickPick)
                {
                    std::ostringstream stream;
                    stream << "resolved click"
                        << " hit=" << (m_highlightedGameObject != nullptr)
                        << " actor=" << (m_highlightedGameObject != nullptr ? m_highlightedGameObject->GetName() : std::string("<none>"));
                    LogScenePickingDiagnostics(stream.str());
                }
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

        const bool resolvePendingClickPick = ShouldResolvePendingSceneClickPick(
            m_pendingClickPickRenderPos.has_value(),
            queuedClickPickThisFrame,
            cameraControlActive,
            m_pendingClickMinReadablePickingFrameSerial,
            gameObjectPickingFeature.GetReadablePickingFrameSerial());
        if (resolvePendingClickPick)
        {
            if (m_highlightedGameObject)
            {
                LogScenePickingDiagnostics("select GameObject=" + m_highlightedGameObject->GetName());
                EDITOR_EXEC(SelectGameObject(*m_highlightedGameObject));
                m_pendingClickPickRenderPos.reset();
            }
            else if (gameObjectPickingFeature.HasReadablePickingFrame())
            {
                LogScenePickingDiagnostics("unselect GameObject from click");
                EDITOR_EXEC(UnselectGameObject());
                m_pendingClickPickRenderPos.reset();
            }
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
    }
}

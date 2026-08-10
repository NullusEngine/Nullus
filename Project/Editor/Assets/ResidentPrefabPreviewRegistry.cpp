#include "Assets/ResidentPrefabPreviewRegistry.h"

#include "Assets/AssetThumbnailService.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "GameObject.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <unordered_set>

namespace
{
constexpr size_t kMaxResidentThumbnailRequestIdentitySamples = 64u;
std::atomic<uint64_t> g_nextImportedThumbnailContinuationRevision {1u};

uint64_t NextImportedThumbnailContinuationRevision()
{
    auto revision = g_nextImportedThumbnailContinuationRevision.fetch_add(
        1u,
        std::memory_order_relaxed);
    if (revision == 0u)
    {
        revision = g_nextImportedThumbnailContinuationRevision.fetch_add(
            1u,
            std::memory_order_relaxed);
    }
    return revision;
}

std::string CompactResidentDiagnosticValue(const std::string& value)
{
    constexpr size_t kMaxLength = 320u;
    if (value.size() <= kMaxLength)
        return value;

    constexpr size_t kPrefixLength = 144u;
    constexpr size_t kSuffixLength = 144u;
    return value.substr(0u, kPrefixLength) +
        "...<" + std::to_string(value.size()) + " bytes>..." +
        value.substr(value.size() - kSuffixLength);
}

uint64_t ResidentDiagnosticHash(const std::string& value)
{
    return static_cast<uint64_t>(std::hash<std::string> {}(value));
}

bool PreviewSnapshotTopologyComplete(
    const std::shared_ptr<const NLS::Editor::Assets::PreviewRenderableSnapshot>& snapshot)
{
    return snapshot != nullptr && !snapshot->drawItems.empty() &&
        snapshot->expectedDrawItemCount != 0u &&
        snapshot->drawItems.size() >= snapshot->expectedDrawItemCount;
}

bool PreviewResourcesComplete(
    const std::shared_ptr<const NLS::Editor::Assets::ResidentPrefabPreviewResources>& resources)
{
    return resources != nullptr && resources->IsCompleteForSource();
}

template<typename T, typename Manager>
std::string FindRegisteredResourcePath(const Manager& manager, const T* resource)
{
    if (resource == nullptr)
        return {};
    for (const auto& [path, candidate] : manager.GetResources())
    {
        if (candidate == resource)
            return path;
    }
    return {};
}

NLS::Render::Resources::Mesh* FindResidentMesh(
    NLS::Core::ResourceManagement::MeshManager& manager,
    const std::string& path)
{
    if (path.empty())
        return nullptr;
    NLS::Render::Resources::Mesh* resource = nullptr;
    if (!manager.TryGetResource(path, resource))
        return nullptr;
    if (resource != nullptr)
        return resource;
    const auto resolvedPath = NLS::Core::ResourceManagement::MeshManager::ResolveArtifactResourcePath(path);
    return resolvedPath.empty()
        ? nullptr
        : manager.FindRegisteredMeshByResolvedArtifactPath(resolvedPath);
}

NLS::Render::Resources::Material* FindResidentMaterial(
    NLS::Core::ResourceManagement::MaterialManager& manager,
    const std::string& path)
{
    if (path.empty())
        return nullptr;
    NLS::Render::Resources::Material* resource = nullptr;
    if (!manager.TryGetResource(path, resource))
        return nullptr;
    if (resource != nullptr)
        return resource;
    if ((resource = manager.FindRegisteredMaterialByResolvedArtifactPath(path)) != nullptr)
        return resource;
    const auto resolvedPath = NLS::Core::ResourceManagement::MaterialManager::ResolveResourcePath(path);
    return resolvedPath.empty()
        ? nullptr
        : manager.FindRegisteredMaterialByResolvedArtifactPath(resolvedPath);
}

NLS::Render::Resources::Texture2D* FindResidentTexture(
    NLS::Core::ResourceManagement::TextureManager& manager,
    const std::string& path)
{
    if (path.empty())
        return nullptr;
    NLS::Render::Resources::Texture2D* resource = nullptr;
    if (!manager.TryGetResource(path, resource))
        return nullptr;
    if (resource != nullptr && resource->GetTextureHandle() != nullptr)
        return resource;
    const auto artifact = manager.TryGetArtifactResource(path);
    if (!artifact.has_value() || *artifact == nullptr ||
        (*artifact)->GetTextureHandle() == nullptr)
    {
        return nullptr;
    }
    return *artifact;
}

NLS::Render::Resources::Texture2D* FindBoundMaterialTexture(
    const NLS::Render::Resources::Material& material,
    const std::string& uniformName)
{
    const auto* parameter = material.GetParameterBlock().TryGet(uniformName);
    if (parameter == nullptr ||
        parameter->type() != typeid(NLS::Render::Resources::Texture2D*))
    {
        return nullptr;
    }

    auto* texture = std::any_cast<NLS::Render::Resources::Texture2D*>(*parameter);
    return texture != nullptr && texture->GetTextureHandle() != nullptr
        ? texture
        : nullptr;
}

NLS::Editor::Assets::ResidentPrefabPreviewResources::TextureHandle AcquireResidentTextureHandle(
    NLS::Core::ResourceManagement::TextureManager& textureManager,
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry& resourceLifetimeRegistry,
    const std::string& ownerToken,
    NLS::Render::Resources::Texture2D* texture,
    const std::string& registeredPath,
    const std::string& requestedPath)
{
    using ResourceType = NLS::Core::ResourceManagement::ResourceLifetimeResourceType;
    using OwnerKind = NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind;

    if (texture == nullptr || texture->GetTextureHandle() == nullptr)
        return {};
    if (!registeredPath.empty())
    {
        return textureManager.AcquireRegisteredTextureHandle(
            resourceLifetimeRegistry,
            ownerToken,
            registeredPath,
            OwnerKind::Preview);
    }

    // Scene restore can bind a ready texture pointer before the manager's
    // artifact index contains its spelling. Track that pointer through the
    // same lifetime registry while resolving it without issuing a load.
    const auto lifetimePath = !texture->path.empty() ? texture->path : requestedPath;
    const auto resourceId = resourceLifetimeRegistry.Acquire({
        ownerToken,
        ResourceType::Texture,
        lifetimePath,
        0u,
        OwnerKind::Preview });
    if (resourceId.normalizedPath.empty())
        return {};

    return NLS::Editor::Assets::ResidentPrefabPreviewResources::TextureHandle(
        resourceLifetimeRegistry,
        resourceId,
        ownerToken,
        [texture](const NLS::Core::ResourceManagement::ResourceId&) ->
            NLS::Render::Resources::Texture2D*
        {
            return texture;
        });
}

bool SamePreviewSnapshot(
    const NLS::Editor::Assets::PreviewRenderableSnapshot& left,
    const NLS::Editor::Assets::PreviewRenderableSnapshot& right)
{
    if (left.expectedDrawItemCount != right.expectedDrawItemCount ||
        left.drawItems.size() != right.drawItems.size())
    {
        return false;
    }
    for (size_t index = 0u; index < left.drawItems.size(); ++index)
    {
        const auto& a = left.drawItems[index];
        const auto& b = right.drawItems[index];
        if (a.sourceObject != b.sourceObject ||
            a.meshPath != b.meshPath ||
            a.materialPaths != b.materialPaths ||
            a.meshAssetId != b.meshAssetId ||
            a.materialAssetIds != b.materialAssetIds)
        {
            return false;
        }
    }
    return true;
}

size_t EstimatePreviewSnapshotBytes(
    const NLS::Editor::Assets::PreviewRenderableSnapshot& snapshot)
{
    size_t bytes = sizeof(snapshot);
    for (const auto& drawItem : snapshot.drawItems)
    {
        bytes += sizeof(drawItem) + drawItem.meshPath.size();
        bytes += drawItem.materialPaths.size() * sizeof(std::string);
        for (const auto& materialPath : drawItem.materialPaths)
            bytes += materialPath.size();
    }
    return bytes;
}

std::string NormalizePreparedPreviewPayloadPath(std::string path)
{
    if (path.empty())
        return {};
    std::replace(path.begin(), path.end(), '\\', '/');
    if (!path.empty() && path.front() == ':')
        path.erase(path.begin());

    const auto normalized = std::filesystem::path(path).lexically_normal();
    std::filesystem::path portable;
    bool foundLibrary = false;
    for (const auto& component : normalized)
    {
        if (!foundLibrary && component.generic_string() == "Library")
            foundLibrary = true;
        if (foundLibrary)
            portable /= component;
    }
    return (foundLibrary ? portable : normalized).generic_string();
}

bool SameResidentResources(
    const std::shared_ptr<const NLS::Editor::Assets::ResidentPrefabPreviewResources>& left,
    const std::shared_ptr<const NLS::Editor::Assets::ResidentPrefabPreviewResources>& right)
{
    if (left == right)
        return true;
    if (left == nullptr || right == nullptr ||
        left->meshManagerInstanceId != right->meshManagerInstanceId ||
        left->materialManagerInstanceId != right->materialManagerInstanceId ||
        left->textureManagerInstanceId != right->textureManagerInstanceId ||
        left->meshes.size() != right->meshes.size() ||
        left->materials.size() != right->materials.size() ||
        left->textures.size() != right->textures.size() ||
        left->hasUnresolvedMaterialBindings != right->hasUnresolvedMaterialBindings ||
        left->hasUnresolvedTextureBindings != right->hasUnresolvedTextureBindings ||
        left->drawItems.size() != right->drawItems.size() ||
        left->meshIndicesByPath != right->meshIndicesByPath ||
        left->materialIndicesByPath != right->materialIndicesByPath ||
        left->textureIndicesByPath != right->textureIndicesByPath ||
        left->textureIndicesByRequestedPath != right->textureIndicesByRequestedPath ||
        left->transientMeshesByIndex.size() != right->transientMeshesByIndex.size() ||
        left->sourceDrawItemCount != right->sourceDrawItemCount ||
        left->sourceExpectedDrawItemCount != right->sourceExpectedDrawItemCount)
    {
        return false;
    }

    for (size_t index = 0u; index < left->meshes.size(); ++index)
    {
        if (left->meshes[index].Get() != right->meshes[index].Get())
            return false;
    }
    for (size_t index = 0u; index < left->materials.size(); ++index)
    {
        if (left->materials[index].Get() != right->materials[index].Get())
            return false;
    }
    for (size_t index = 0u; index < left->textures.size(); ++index)
    {
        if (left->textures[index].Get() != right->textures[index].Get())
            return false;
    }
    for (const auto& [index, mesh] : left->transientMeshesByIndex)
    {
        const auto found = right->transientMeshesByIndex.find(index);
        if (found == right->transientMeshesByIndex.end() ||
            found->second.get() != mesh.get())
        {
            return false;
        }
    }
    for (size_t index = 0u; index < left->drawItems.size(); ++index)
    {
        const auto& leftItem = left->drawItems[index];
        const auto& rightItem = right->drawItems[index];
        if (leftItem.meshIndex != rightItem.meshIndex ||
            leftItem.materialIndices != rightItem.materialIndices)
        {
            return false;
        }
    }
    return true;
}

std::shared_ptr<const NLS::Editor::Assets::ResidentPrefabPreviewResources> BuildResidentResources(
    const NLS::Core::Assets::AssetId& assetId,
    const NLS::Editor::Assets::PreviewRenderableSnapshot& snapshot,
    NLS::Core::ResourceManagement::MeshManager& meshManager,
    NLS::Core::ResourceManagement::MaterialManager& materialManager,
    NLS::Core::ResourceManagement::TextureManager* textureManager,
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry& resourceLifetimeRegistry)
{
    if (!assetId.IsValid() || snapshot.drawItems.empty())
        return {};

    auto resources = std::make_shared<NLS::Editor::Assets::ResidentPrefabPreviewResources>();
    resources->meshManagerInstanceId = meshManager.GetInstanceId();
    resources->materialManagerInstanceId = materialManager.GetInstanceId();
    resources->textureManagerInstanceId = textureManager != nullptr
        ? textureManager->GetInstanceId()
        : 0u;
    resources->sourceDrawItemCount = snapshot.drawItems.size();
    resources->sourceExpectedDrawItemCount = snapshot.expectedDrawItemCount;
    const std::string ownerToken =
        "resident-thumbnail:" + assetId.ToString() + ":" +
        std::to_string(ResidentDiagnosticHash(std::to_string(snapshot.drawItems.size())));

    for (const auto& drawItem : snapshot.drawItems)
    {
        NLS::Editor::Assets::ResidentPrefabPreviewResources::DrawItem residentDrawItem;
        const auto* mesh = FindResidentMesh(meshManager, drawItem.meshPath);
        if (mesh == nullptr)
            return {};
        auto meshPath = FindRegisteredResourcePath(meshManager, mesh);
        if (meshPath.empty())
            return {};
        auto meshIndex = resources->meshIndicesByPath.find(meshPath);
        if (meshIndex == resources->meshIndicesByPath.end())
        {
            const auto index = resources->meshes.size();
            auto handle = meshManager.AcquireRegisteredMeshHandle(
                resourceLifetimeRegistry,
                ownerToken,
                meshPath,
                NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind::Preview);
            if (!handle)
                return {};
            resources->meshes.push_back(std::move(handle));
            resources->meshIndicesByPath.emplace(meshPath, index);
            // Keep the source spelling as well as the manager's registered
            // spelling. The renderer plan is built from the immutable
            // snapshot and must be able to validate both identities without
            // reopening an artifact.
            resources->meshIndicesByPath.emplace(drawItem.meshPath, index);
            residentDrawItem.meshIndex = index;
        }
        else
        {
            residentDrawItem.meshIndex = meshIndex->second;
        }

        residentDrawItem.materialIndices.reserve(drawItem.materialPaths.size());
        for (const auto& materialPath : drawItem.materialPaths)
        {
            if (materialPath.empty())
            {
                residentDrawItem.materialIndices.push_back(SIZE_MAX);
                continue;
            }
            const auto* material = FindResidentMaterial(materialManager, materialPath);
            if (material == nullptr)
                return {};
            auto registeredMaterialPath = FindRegisteredResourcePath(materialManager, material);
            if (registeredMaterialPath.empty())
                return {};
            auto materialIndex = resources->materialIndicesByPath.find(registeredMaterialPath);
            if (materialIndex == resources->materialIndicesByPath.end())
            {
                const auto index = resources->materials.size();
                auto handle = materialManager.AcquireRegisteredMaterialHandle(
                    resourceLifetimeRegistry,
                    ownerToken,
                    registeredMaterialPath,
                    NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind::Preview);
                if (!handle)
                    return {};
                resources->materials.push_back(std::move(handle));
                resources->materialIndicesByPath.emplace(registeredMaterialPath, index);
                resources->materialIndicesByPath.emplace(materialPath, index);
                residentDrawItem.materialIndices.push_back(index);

                const auto& texturePaths = material->GetTextureResourcePaths();
                if (!texturePaths.empty() && textureManager == nullptr)
                    return {};
                if (textureManager != nullptr)
                {
                    for (const auto& [_, texturePath] : texturePaths)
                    {
                        const auto* texture = FindResidentTexture(*textureManager, texturePath);
                        if (texture == nullptr)
                            return {};
                        const auto registeredTexturePath = FindRegisteredResourcePath(*textureManager, texture);
                        if (registeredTexturePath.empty())
                            return {};
                        if (const auto existingTexture = resources->textureIndicesByPath.find(
                                registeredTexturePath);
                            existingTexture != resources->textureIndicesByPath.end())
                        {
                            resources->textureIndicesByRequestedPath.emplace(
                                texturePath,
                                existingTexture->second);
                            continue;
                        }
                        const auto textureIndex = resources->textures.size();
                        auto textureHandle = textureManager->AcquireRegisteredTextureHandle(
                            resourceLifetimeRegistry,
                            ownerToken,
                            registeredTexturePath,
                            NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind::Preview);
                        if (!textureHandle)
                            return {};
                        resources->textures.push_back(std::move(textureHandle));
                        resources->textureIndicesByPath.emplace(registeredTexturePath, textureIndex);
                        resources->textureIndicesByRequestedPath.emplace(
                            texturePath,
                            textureIndex);
                    }
                }
            }
            else
            {
                residentDrawItem.materialIndices.push_back(materialIndex->second);
            }
        }
        resources->drawItems.push_back(std::move(residentDrawItem));
    }

    return resources;
}

std::shared_ptr<const NLS::Editor::Assets::ResidentPrefabPreviewResources>
BuildResidentResourcesFromLiveObjects(
    const NLS::Core::Assets::AssetId& assetId,
    const NLS::Editor::Assets::PreviewRenderableSnapshot& snapshot,
    const std::unordered_map<
        NLS::Engine::Serialize::ObjectId,
        const NLS::Engine::GameObject*>& liveObjectsBySourceId,
    NLS::Core::ResourceManagement::MeshManager& meshManager,
    NLS::Core::ResourceManagement::MaterialManager& materialManager,
    NLS::Core::ResourceManagement::TextureManager* textureManager,
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry& resourceLifetimeRegistry,
    std::shared_ptr<const NLS::Editor::Assets::PreviewRenderableSnapshot>* resolvedSnapshot)
{
    if (!assetId.IsValid() || snapshot.drawItems.empty())
        return {};

    auto resources = std::make_shared<
        NLS::Editor::Assets::ResidentPrefabPreviewResources>();
    resources->meshManagerInstanceId = meshManager.GetInstanceId();
    resources->materialManagerInstanceId = materialManager.GetInstanceId();
    resources->textureManagerInstanceId = textureManager != nullptr
        ? textureManager->GetInstanceId()
        : 0u;
    resources->sourceDrawItemCount = snapshot.drawItems.size();
    resources->sourceExpectedDrawItemCount = snapshot.expectedDrawItemCount;
    const std::string ownerToken =
        "resident-thumbnail-live:" + assetId.ToString() + ":" +
        std::to_string(ResidentDiagnosticHash(std::to_string(snapshot.drawItems.size())));

    auto filteredSnapshot = std::make_shared<
        NLS::Editor::Assets::PreviewRenderableSnapshot>();
    filteredSnapshot->drawItems.reserve(snapshot.drawItems.size());
    size_t missingLiveObjectCount = 0u;
    size_t missingMeshComponentCount = 0u;
    size_t unresolvedMeshCount = 0u;
    size_t unresolvedMaterialCount = 0u;
    size_t unresolvedTextureCount = 0u;
    size_t declaredTextureCount = 0u;
    size_t boundReadyTextureCount = 0u;
    size_t managerReadyTextureCount = 0u;
    size_t boundReadyNotIndexedTextureCount = 0u;
    size_t managerReadyUnboundTextureCount = 0u;
    size_t boundManagerPointerMismatchCount = 0u;
    size_t readyMeshCount = 0u;
    std::string firstUnresolvedModelPath;
    std::string firstUnresolvedSnapshotMeshPath;
    std::string firstUnresolvedTexturePath;
    std::string firstUnresolvedMaterialPath;

    for (const auto& drawItem : snapshot.drawItems)
    {
        const auto liveObject = liveObjectsBySourceId.find(drawItem.sourceObject);
        if (liveObject == liveObjectsBySourceId.end() ||
            liveObject->second == nullptr || !liveObject->second->IsAlive())
        {
            ++missingLiveObjectCount;
            continue;
        }

        auto* meshFilter = liveObject->second->GetComponent<
            NLS::Engine::Components::MeshFilter>();
        auto* meshRenderer = liveObject->second->GetComponent<
            NLS::Engine::Components::MeshRenderer>();
        if (meshFilter == nullptr || meshRenderer == nullptr)
        {
            ++missingMeshComponentCount;
            continue;
        }

        // Prefer the live component's bound pointer. During scene restore the
        // PPtr can lag behind the manager table, so fall back to the immutable
        // snapshot path. FindResidentMesh only inspects already-registered
        // resources and never starts an artifact request.
        const auto transientMesh = meshFilter->GetResolvedTransientMesh();
        auto* mesh = meshFilter->ResolveMesh();
        if (mesh == nullptr)
            mesh = FindResidentMesh(meshManager, meshFilter->GetModelPath());
        if (mesh == nullptr)
            mesh = FindResidentMesh(meshManager, drawItem.meshPath);
        if (mesh == nullptr)
        {
            ++unresolvedMeshCount;
            if (firstUnresolvedModelPath.empty())
            {
                firstUnresolvedModelPath = meshFilter->GetModelPath();
                firstUnresolvedSnapshotMeshPath = drawItem.meshPath;
            }
            continue;
        }
        auto meshPath = FindRegisteredResourcePath(meshManager, mesh);
        if (meshPath.empty() && transientMesh != nullptr && mesh == transientMesh.get())
        {
            // Scene restore may deliberately keep the ready mesh transient so
            // the scene instance and the thumbnail share one allocation.
            meshPath = !meshFilter->GetModelPath().empty()
                ? meshFilter->GetModelPath()
                : drawItem.meshPath;
        }
        if (meshPath.empty())
        {
            ++unresolvedMeshCount;
            continue;
        }
        ++readyMeshCount;

        NLS::Editor::Assets::ResidentPrefabPreviewResources::DrawItem residentDrawItem;
        struct MaterialResolution
        {
            struct TextureResolution
            {
                std::string requestedPath;
                std::string registeredPath;
                NLS::Render::Resources::Texture2D* texture = nullptr;
            };

            std::string requestedPath;
            std::string registeredPath;
            NLS::Render::Resources::Material* material = nullptr;
            std::vector<TextureResolution> textures;
        };
        std::vector<MaterialResolution> materialResolutions;
        materialResolutions.reserve(drawItem.materialPaths.size());

        bool drawItemReady = true;
        for (size_t slot = 0u; slot < drawItem.materialPaths.size(); ++slot)
        {
            const auto& materialPath = drawItem.materialPaths[slot];
            if (materialPath.empty())
            {
                materialResolutions.push_back({});
                continue;
            }

            auto* material = meshRenderer->GetMaterialAtIndex(
                static_cast<uint32_t>(slot));
            if (material == nullptr)
                material = meshRenderer->ResolveMaterialAtIndex(static_cast<uint32_t>(slot));
            if (material == nullptr)
                material = FindResidentMaterial(materialManager, materialPath);
            const auto registeredMaterialPath = FindRegisteredResourcePath(
                materialManager,
                material);
            if (material == nullptr || registeredMaterialPath.empty())
            {
                ++unresolvedMaterialCount;
                resources->hasUnresolvedMaterialBindings = true;
                // Keep the geometry in the provisional package. The renderer
                // fills this slot with its default material and can upgrade it
                // when the scene resolver publishes the real material.
                materialResolutions.push_back({});
                continue;
            }

            MaterialResolution resolution;
            resolution.requestedPath = materialPath;
            resolution.registeredPath = registeredMaterialPath;
            resolution.material = material;
            for (const auto& [uniformName, texturePath] : material->GetTextureResourcePaths())
            {
                if (texturePath.empty())
                    continue;
                ++declaredTextureCount;
                if (textureManager == nullptr)
                {
                    ++unresolvedTextureCount;
                    resources->hasUnresolvedTextureBindings = true;
                    continue;
                }
                auto* boundTexture = FindBoundMaterialTexture(*material, uniformName);
                if (boundTexture != nullptr)
                    ++boundReadyTextureCount;
                auto* managerTexture = FindResidentTexture(*textureManager, texturePath);
                if (managerTexture != nullptr)
                    ++managerReadyTextureCount;
                if (boundTexture != nullptr && managerTexture == nullptr)
                    ++boundReadyNotIndexedTextureCount;
                if (boundTexture == nullptr && managerTexture != nullptr)
                    ++managerReadyUnboundTextureCount;
                if (boundTexture != nullptr && managerTexture != nullptr &&
                    boundTexture != managerTexture)
                {
                    ++boundManagerPointerMismatchCount;
                }
                auto* texture = boundTexture != nullptr ? boundTexture : managerTexture;
                const auto registeredTexturePath = FindRegisteredResourcePath(
                    *textureManager,
                    texture);
                if (texture == nullptr ||
                    (registeredTexturePath.empty() && texture->path.empty() && texturePath.empty()))
                {
                    ++unresolvedTextureCount;
                    resources->hasUnresolvedTextureBindings = true;
                    if (firstUnresolvedTexturePath.empty())
                    {
                        firstUnresolvedTexturePath = texturePath;
                        firstUnresolvedMaterialPath = material->path;
                    }
                    continue;
                }
                resolution.textures.push_back({ texturePath, registeredTexturePath, texture });
            }
            if (!drawItemReady)
                break;
            materialResolutions.push_back(std::move(resolution));
        }
        if (!drawItemReady)
            continue;

        auto meshIndex = resources->meshIndicesByPath.find(meshPath);
        if (meshIndex == resources->meshIndicesByPath.end())
        {
            const auto index = resources->meshes.size();
            if (transientMesh != nullptr && mesh == transientMesh.get())
            {
                resources->meshes.emplace_back();
            }
            else
            {
                auto handle = meshManager.AcquireRegisteredMeshHandle(
                    resourceLifetimeRegistry,
                    ownerToken,
                    meshPath,
                    NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind::Preview);
                if (!handle)
                    continue;
                resources->meshes.push_back(std::move(handle));
            }
            resources->meshIndicesByPath.emplace(meshPath, index);
            resources->meshIndicesByPath.emplace(drawItem.meshPath, index);
            residentDrawItem.meshIndex = index;
        }
        else
        {
            residentDrawItem.meshIndex = meshIndex->second;
        }
        if (transientMesh != nullptr && mesh == transientMesh.get())
            resources->transientMeshesByIndex.emplace(residentDrawItem.meshIndex, transientMesh);

        for (const auto& resolution : materialResolutions)
        {
            if (resolution.requestedPath.empty())
            {
                residentDrawItem.materialIndices.push_back(SIZE_MAX);
                continue;
            }

            for (const auto& textureResolution : resolution.textures)
            {
                const auto& texturePath = textureResolution.requestedPath;
                const auto& registeredTexturePath = textureResolution.registeredPath;
                if (resources->textureIndicesByPath.find(registeredTexturePath) ==
                        resources->textureIndicesByPath.end() &&
                    (registeredTexturePath.empty() ||
                        resources->textureIndicesByRequestedPath.find(texturePath) ==
                            resources->textureIndicesByRequestedPath.end()))
                {
                    const auto textureIndex = resources->textures.size();
                    auto textureHandle = AcquireResidentTextureHandle(
                        *textureManager,
                        resourceLifetimeRegistry,
                        ownerToken,
                        textureResolution.texture,
                        registeredTexturePath,
                        texturePath);
                    if (!textureHandle)
                    {
                        drawItemReady = false;
                        break;
                    }
                    resources->textures.push_back(std::move(textureHandle));
                    if (!registeredTexturePath.empty())
                    {
                        resources->textureIndicesByPath.emplace(
                            registeredTexturePath,
                            textureIndex);
                    }
                    resources->textureIndicesByRequestedPath.emplace(
                        texturePath,
                        textureIndex);
                }
                else if (const auto requested = resources->textureIndicesByRequestedPath.find(texturePath);
                         requested == resources->textureIndicesByRequestedPath.end())
                {
                    const auto textureIndex = resources->textureIndicesByPath.at(registeredTexturePath);
                    resources->textureIndicesByRequestedPath.emplace(
                        texturePath,
                        textureIndex);
                }
            }
            if (!drawItemReady)
                continue;

            auto materialIndex = resources->materialIndicesByPath.find(
                resolution.registeredPath);
            if (materialIndex == resources->materialIndicesByPath.end())
            {
                const auto index = resources->materials.size();
                auto handle = materialManager.AcquireRegisteredMaterialHandle(
                    resourceLifetimeRegistry,
                    ownerToken,
                    resolution.registeredPath,
                    NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind::Preview);
                if (!handle)
                {
                    drawItemReady = false;
                    break;
                }
                resources->materials.push_back(std::move(handle));
                resources->materialIndicesByPath.emplace(
                    resolution.registeredPath,
                    index);
                resources->materialIndicesByPath.emplace(
                    resolution.requestedPath,
                    index);
                residentDrawItem.materialIndices.push_back(index);
            }
            else
            {
                residentDrawItem.materialIndices.push_back(materialIndex->second);
            }
        }
        if (!drawItemReady)
            continue;

        resources->drawItems.push_back(std::move(residentDrawItem));
        filteredSnapshot->drawItems.push_back(drawItem);
    }

    // Scene restore is incremental. A filtered snapshot is intentionally
    // publishable: it lets the thumbnail use the resources already attached to
    // live instances and is replaced as later restore passes resolve more
    // draw items. An empty result remains Pending and never becomes a partial
    // thumbnail.
    if (resources->drawItems.empty())
    {
        std::string registeredMeshSamples;
        for (const auto& [path, resource] : meshManager.GetResources())
        {
            if (resource == nullptr)
                continue;
            if (!registeredMeshSamples.empty())
                registeredMeshSamples += ',';
            registeredMeshSamples += path;
            if (registeredMeshSamples.size() > 640u)
                break;
        }
        NLS_LOG_INFO(
            "resident-prefab-live-probe|asset=" + assetId.ToString() +
            "|sourceDrawItems=" + std::to_string(snapshot.drawItems.size()) +
            "|liveObjects=" + std::to_string(liveObjectsBySourceId.size()) +
            "|missingLiveObjects=" + std::to_string(missingLiveObjectCount) +
            "|missingMeshComponents=" + std::to_string(missingMeshComponentCount) +
            "|unresolvedMeshes=" + std::to_string(unresolvedMeshCount) +
            "|unresolvedMaterials=" + std::to_string(unresolvedMaterialCount) +
             "|unresolvedTextures=" + std::to_string(unresolvedTextureCount) +
             "|declaredTextures=" + std::to_string(declaredTextureCount) +
             "|boundReadyTextures=" + std::to_string(boundReadyTextureCount) +
             "|managerReadyTextures=" + std::to_string(managerReadyTextureCount) +
             "|boundReadyNotIndexed=" + std::to_string(boundReadyNotIndexedTextureCount) +
             "|managerReadyUnbound=" + std::to_string(managerReadyUnboundTextureCount) +
             "|boundManagerPointerMismatch=" + std::to_string(boundManagerPointerMismatchCount) +
             "|readyMeshes=" + std::to_string(readyMeshCount) +
            "|registeredMeshes=" + std::to_string(meshManager.GetResources().size()) +
            "|texturesPartial=" + std::to_string(
                resources->hasUnresolvedTextureBindings ? 1u : 0u) +
            "|firstModelPath=" + firstUnresolvedModelPath +
             "|firstSnapshotMeshPath=" + firstUnresolvedSnapshotMeshPath +
             "|firstUnresolvedMaterialPath=" + firstUnresolvedMaterialPath +
             "|firstUnresolvedTexturePath=" + firstUnresolvedTexturePath +
             "|registeredMeshSamples=" + registeredMeshSamples +
            "|readyDrawItems=0");
        return {};
    }
    filteredSnapshot->expectedDrawItemCount = filteredSnapshot->drawItems.size();
    NLS_LOG_INFO(
        "resident-prefab-live-probe|asset=" + assetId.ToString() +
        "|sourceDrawItems=" + std::to_string(snapshot.drawItems.size()) +
        "|liveObjects=" + std::to_string(liveObjectsBySourceId.size()) +
        "|missingLiveObjects=" + std::to_string(missingLiveObjectCount) +
        "|missingMeshComponents=" + std::to_string(missingMeshComponentCount) +
        "|unresolvedMeshes=" + std::to_string(unresolvedMeshCount) +
        "|unresolvedMaterials=" + std::to_string(unresolvedMaterialCount) +
         "|unresolvedTextures=" + std::to_string(unresolvedTextureCount) +
         "|declaredTextures=" + std::to_string(declaredTextureCount) +
         "|boundReadyTextures=" + std::to_string(boundReadyTextureCount) +
         "|managerReadyTextures=" + std::to_string(managerReadyTextureCount) +
         "|boundReadyNotIndexed=" + std::to_string(boundReadyNotIndexedTextureCount) +
         "|managerReadyUnbound=" + std::to_string(managerReadyUnboundTextureCount) +
         "|boundManagerPointerMismatch=" + std::to_string(boundManagerPointerMismatchCount) +
         "|readyMeshes=" + std::to_string(readyMeshCount) +
        "|registeredMeshes=" + std::to_string(meshManager.GetResources().size()) +
         "|texturesPartial=" + std::to_string(
             resources->hasUnresolvedTextureBindings ? 1u : 0u) +
         "|firstUnresolvedMaterialPath=" + firstUnresolvedMaterialPath +
         "|firstUnresolvedTexturePath=" + firstUnresolvedTexturePath +
         "|readyDrawItems=" + std::to_string(resources->drawItems.size()));
    if (resolvedSnapshot != nullptr)
        *resolvedSnapshot = std::move(filteredSnapshot);
    return resources;
}
}

namespace NLS::Editor::Assets
{
ResidentPrefabPreviewRegistry::Lease::Lease(
    std::shared_ptr<const PreviewRenderableSnapshot> snapshot,
    std::shared_ptr<const ResidentPrefabPreviewResources> resources,
    std::function<std::shared_ptr<const std::vector<uint8_t>>(const std::string&)>
        takePreparedMeshPayload,
    std::function<void()> release) :
    m_snapshot(std::move(snapshot)),
    m_resources(std::move(resources)),
    m_takePreparedMeshPayload(std::move(takePreparedMeshPayload)),
    m_release(std::move(release))
{
}

ResidentPrefabPreviewRegistry::Lease::Lease(Lease&& other) noexcept :
    m_snapshot(std::move(other.m_snapshot)),
    m_resources(std::move(other.m_resources)),
    m_takePreparedMeshPayload(std::move(other.m_takePreparedMeshPayload)),
    m_release(std::move(other.m_release))
{
}

ResidentPrefabPreviewRegistry::Lease& ResidentPrefabPreviewRegistry::Lease::operator=(Lease&& other) noexcept
{
    if (this == &other)
        return *this;
    Reset();
    m_snapshot = std::move(other.m_snapshot);
    m_resources = std::move(other.m_resources);
    m_takePreparedMeshPayload = std::move(other.m_takePreparedMeshPayload);
    m_release = std::move(other.m_release);
    return *this;
}

ResidentPrefabPreviewRegistry::Lease::~Lease()
{
    Reset();
}

void ResidentPrefabPreviewRegistry::Lease::Reset()
{
    if (m_release)
        m_release();
    m_release = {};
    m_takePreparedMeshPayload = {};
    m_resources.reset();
    m_snapshot.reset();
}

bool ResidentPrefabPreviewResources::IsValidFor(
    const NLS::Core::ResourceManagement::MeshManager& meshManager,
    const NLS::Core::ResourceManagement::MaterialManager& materialManager,
    const NLS::Core::ResourceManagement::TextureManager* textureManager) const
{
    if (meshManager.GetInstanceId() != meshManagerInstanceId ||
        materialManager.GetInstanceId() != materialManagerInstanceId ||
        (textureManager != nullptr ? textureManager->GetInstanceId() : 0u) !=
            textureManagerInstanceId)
    {
        return false;
    }
    if (textureManager == nullptr && !textures.empty())
        return false;
    for (size_t index = 0u; index < meshes.size(); ++index)
    {
        if (meshes[index].IsValid())
            continue;
        const auto transient = transientMeshesByIndex.find(index);
        if (transient == transientMeshesByIndex.end() || !transient->second)
            return false;
    }
    if (std::any_of(
            materials.begin(),
            materials.end(),
            [](const auto& handle) { return !handle.IsValid(); }) ||
        std::any_of(
            textures.begin(),
            textures.end(),
            [](const auto& handle) { return !handle.IsValid(); }))
    {
        return false;
    }
    if (drawItems.empty())
        return false;

    for (const auto& material : materials)
    {
        auto* resource = material.Get();
        if (resource == nullptr)
            return false;
        for (const auto& [_, texturePath] : resource->GetTextureResourcePaths())
        {
            if (texturePath.empty())
                continue;
            const auto found = textureIndicesByRequestedPath.find(texturePath);
            if (textureManager == nullptr || found == textureIndicesByRequestedPath.end() ||
                found->second >= textures.size() || !textures[found->second].IsValid())
            {
                if (!hasUnresolvedTextureBindings)
                    return false;
                continue;
            }
        }
    }

    return std::all_of(
            drawItems.begin(),
            drawItems.end(),
            [this](const auto& item)
            {
                return item.meshIndex < meshes.size() &&
                    std::all_of(
                        item.materialIndices.begin(),
                        item.materialIndices.end(),
                        [this](const size_t index)
                        {
                            return index == SIZE_MAX || index < materials.size();
                        });
            });
}

ResidentPrefabPreviewRegistry::ResidentPrefabPreviewRegistry(const size_t inactiveBudgetBytes) :
    m_inactiveBudgetBytes(inactiveBudgetBytes)
{
}

std::shared_ptr<ResidentPrefabPreviewRegistry> ResidentPrefabPreviewRegistry::Create(
    const size_t inactiveBudgetBytes)
{
    return std::make_shared<ResidentPrefabPreviewRegistry>(inactiveBudgetBytes);
}

std::string ResidentPrefabPreviewRegistry::MakeKey(
    const std::string& runtimeCacheIdentity,
    const std::string& freshnessFingerprint)
{
    return runtimeCacheIdentity + "\x1f" + freshnessFingerprint;
}

std::string ResidentPrefabPreviewRegistry::MakeImportedThumbnailContinuationKey(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::AssetId& assetId)
{
    return projectRoot.lexically_normal().generic_string() + "\x1f" + assetId.ToString();
}

void ResidentPrefabPreviewRegistry::RegisterSnapshot(
    std::string runtimeCacheIdentity,
    std::string freshnessFingerprint,
    std::shared_ptr<const PreviewRenderableSnapshot> snapshot,
    const size_t byteSize,
    const bool acquireSceneLease,
    std::string lookupIdentity,
    std::string lookupFreshnessFingerprint,
    std::shared_ptr<const ResidentPrefabPreviewResources> resources,
    const bool allowArtifactResourceLoading,
    std::vector<PreparedPrefabPreviewMeshPayload> preparedMeshPayloads,
    const bool importedThumbnailPending)
{
    if (runtimeCacheIdentity.empty() || freshnessFingerprint.empty() || !snapshot)
        return;

    const auto key = MakeKey(runtimeCacheIdentity, freshnessFingerprint);
    const auto lookupFreshness = lookupFreshnessFingerprint.empty()
        ? freshnessFingerprint
        : lookupFreshnessFingerprint;
    const auto lookupKey = lookupIdentity.empty()
        ? std::string {}
        : MakeKey(lookupIdentity, freshnessFingerprint);
    const auto deferredLookupKey = lookupIdentity.empty() || lookupFreshness == freshnessFingerprint
        ? std::string {}
        : MakeKey(lookupIdentity, lookupFreshness);
    const bool replacePreparedMeshPayloads = !preparedMeshPayloads.empty();
    std::unordered_map<std::string, std::shared_ptr<const std::vector<uint8_t>>>
        preparedMeshPayloadsByPath;
    size_t preparedMeshPayloadBytes = 0u;
    for (auto& preparedPayload : preparedMeshPayloads)
    {
        const auto path = NormalizePreparedPreviewPayloadPath(preparedPayload.artifactPath);
        if (path.empty() || preparedPayload.bytes == nullptr || preparedPayload.bytes->empty())
            continue;
        if (const auto existing = preparedMeshPayloadsByPath.find(path);
            existing != preparedMeshPayloadsByPath.end())
        {
            preparedMeshPayloadBytes -= existing->second->size();
        }
        preparedMeshPayloadBytes += preparedPayload.bytes->size();
        preparedMeshPayloadsByPath.insert_or_assign(path, std::move(preparedPayload.bytes));
    }
    std::lock_guard lock(m_mutex);
    m_lastRegisteredIdentity = CompactResidentDiagnosticValue(runtimeCacheIdentity);
    m_lastRegisteredLookupIdentity = CompactResidentDiagnosticValue(lookupIdentity);
    m_lastRegisteredFreshness = CompactResidentDiagnosticValue(freshnessFingerprint);
    auto found = m_entries.find(key);
    bool thumbnailWakeRequired = found == m_entries.end();
    if (found != m_entries.end())
    {
        const bool topologyWasComplete =
            PreviewSnapshotTopologyComplete(found->second.snapshot);
        const bool resourcesWereComplete =
            PreviewResourcesComplete(found->second.resources);
        const bool artifactResourceLoadingWasAllowed =
            found->second.allowArtifactResourceLoading;
        const bool snapshotChanged = found->second.snapshot == nullptr ||
            !SamePreviewSnapshot(*found->second.snapshot, *snapshot);
        const bool resourcesChanged = snapshotChanged
            ? resources != nullptr
            : resources != nullptr &&
                !SameResidentResources(found->second.resources, resources);

        // Registration is repeated while scene restore publishes resources in
        // stages. Preserve the existing immutable objects when the incoming
        // values are semantically identical; the renderer uses the resource
        // package identity to decide whether its GPU scene plan must restart.
        if (snapshotChanged)
        {
            m_residentBytes -= found->second.TotalByteSize();
            found->second.sourceSnapshot = snapshot;
            found->second.snapshot = std::move(snapshot);
            found->second.snapshotByteSize = byteSize;
            m_residentBytes += found->second.TotalByteSize();
        }
        else if (found->second.sourceSnapshot == nullptr)
        {
            found->second.sourceSnapshot = found->second.snapshot;
        }

        if (resourcesChanged)
            found->second.resources = std::move(resources);
        else if (snapshotChanged && resources == nullptr)
            found->second.resources.reset();

        if (!snapshotChanged && found->second.snapshotByteSize != byteSize)
        {
            m_residentBytes -= found->second.TotalByteSize();
            found->second.snapshotByteSize = byteSize;
            m_residentBytes += found->second.TotalByteSize();
        }
        if (replacePreparedMeshPayloads)
        {
            m_residentBytes -= found->second.TotalByteSize();
            found->second.preparedMeshPayloads = std::move(preparedMeshPayloadsByPath);
            found->second.preparedMeshPayloadBytes = preparedMeshPayloadBytes;
            m_residentBytes += found->second.TotalByteSize();
        }
        if (snapshotChanged || resourcesChanged || replacePreparedMeshPayloads)
            ++found->second.revision;
        found->second.allowArtifactResourceLoading =
            found->second.allowArtifactResourceLoading || allowArtifactResourceLoading;
        found->second.importedThumbnailPending =
            found->second.importedThumbnailPending || importedThumbnailPending;
        const bool topologyIsComplete =
            PreviewSnapshotTopologyComplete(found->second.snapshot);
        const bool resourcesAreComplete =
            PreviewResourcesComplete(found->second.resources);
        thumbnailWakeRequired = thumbnailWakeRequired || replacePreparedMeshPayloads ||
            (!topologyWasComplete && topologyIsComplete) ||
            (snapshotChanged && topologyWasComplete && topologyIsComplete) ||
            (!resourcesWereComplete && resourcesAreComplete) ||
            (!artifactResourceLoadingWasAllowed &&
                found->second.allowArtifactResourceLoading);
        found->second.lastUsedTick = ++m_tick;
        if (acquireSceneLease)
        {
            ++found->second.activeLeaseCount;
            ++found->second.sceneLeaseCount;
        }
    }
    else
    {
        Entry entry;
        entry.runtimeCacheIdentity = std::move(runtimeCacheIdentity);
        entry.freshnessFingerprint = std::move(freshnessFingerprint);
        entry.sourceSnapshot = snapshot;
        entry.snapshot = std::move(snapshot);
        entry.resources = std::move(resources);
        entry.preparedMeshPayloads = std::move(preparedMeshPayloadsByPath);
        entry.snapshotByteSize = byteSize;
        entry.preparedMeshPayloadBytes = preparedMeshPayloadBytes;
        entry.revision = 1u;
        entry.lastUsedTick = ++m_tick;
        entry.activeLeaseCount = acquireSceneLease ? 1u : 0u;
        entry.sceneLeaseCount = acquireSceneLease ? 1u : 0u;
        entry.allowArtifactResourceLoading = allowArtifactResourceLoading;
        entry.importedThumbnailPending = importedThumbnailPending;
        m_residentBytes += entry.TotalByteSize();
        m_entries.emplace(key, std::move(entry));
    }
    // Keep both spellings: a resolved request carries the full artifact-file
    // freshness, while an early resident request carries only metadata
    // freshness until its manifest lookup completes.
    const auto publishAlias = [this, &key](const std::string& alias)
    {
        if (alias.empty() || alias == key)
            return false;
        const auto [foundAlias, inserted] = m_aliases.emplace(alias, key);
        if (inserted)
            return true;
        if (foundAlias->second == key)
            return false;
        foundAlias->second = key;
        return true;
    };
    thumbnailWakeRequired = publishAlias(lookupKey) || thumbnailWakeRequired;
    thumbnailWakeRequired = publishAlias(deferredLookupKey) || thumbnailWakeRequired;
    // Keep the newest registration available for the immediate Acquire that
    // follows scene restore or thumbnail request construction, but trim older
    // inactive entries now. Without this protected-key pass, repeated scene
    // registrations could exceed the inactive budget until an unrelated lease
    // release or settings change happened to trigger maintenance.
    EvictInactiveLocked(&key);
    if (thumbnailWakeRequired)
        m_thumbnailWakeRevision.fetch_add(1u, std::memory_order_release);
}

bool ResidentPrefabPreviewRegistry::RegisterImportedPrefabSnapshot(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::AssetId& assetId,
    const std::string& sourceAssetPath,
    const std::string& prefabSubAssetKey,
    const std::string& artifactPath,
    std::shared_ptr<const PreviewRenderableSnapshot> snapshot,
    std::vector<PreparedPrefabPreviewMeshPayload> preparedMeshPayloads)
{
    if (!assetId.IsValid() || sourceAssetPath.empty() || artifactPath.empty() ||
        !PreviewSnapshotTopologyComplete(snapshot))
    {
        return false;
    }

    const auto canonicalSubAssetKey = BuildCanonicalPrefabPreviewSubAssetKey(
        sourceAssetPath,
        prefabSubAssetKey);
    if (canonicalSubAssetKey.empty())
        return false;

    const auto runtimeCacheIdentity = BuildResidentPrefabRuntimeCacheIdentity(
        assetId.ToString(),
        canonicalSubAssetKey);
    const auto canonicalFreshness = BuildPrefabThumbnailDependencyStamp(
        projectRoot,
        assetId,
        sourceAssetPath,
        canonicalSubAssetKey,
        artifactPath);
    if (runtimeCacheIdentity.empty() || canonicalFreshness.empty())
        return false;

    const auto snapshotBytes = EstimatePreviewSnapshotBytes(*snapshot);
    RegisterSnapshot(
        runtimeCacheIdentity,
        canonicalFreshness,
        snapshot,
        snapshotBytes,
        false,
        {},
        {},
        {},
        true,
        std::move(preparedMeshPayloads),
        true);

    std::vector<std::string> aliasFreshnesses;
    aliasFreshnesses.push_back(BuildPrefabThumbnailDependencyStamp(
        projectRoot,
        assetId,
        sourceAssetPath,
        canonicalSubAssetKey,
        {}));
    if (canonicalSubAssetKey.rfind("prefab:", 0u) == 0u)
    {
        const auto importerSubAssetKey =
            "model:" + canonicalSubAssetKey.substr(std::string("prefab:").size());
        aliasFreshnesses.push_back(BuildPrefabThumbnailDependencyStamp(
            projectRoot,
            assetId,
            sourceAssetPath,
            importerSubAssetKey,
            artifactPath));
        aliasFreshnesses.push_back(BuildPrefabThumbnailDependencyStamp(
            projectRoot,
            assetId,
            sourceAssetPath,
            importerSubAssetKey,
            {}));
    }

    std::sort(aliasFreshnesses.begin(), aliasFreshnesses.end());
    aliasFreshnesses.erase(
        std::unique(aliasFreshnesses.begin(), aliasFreshnesses.end()),
        aliasFreshnesses.end());
    for (const auto& aliasFreshness : aliasFreshnesses)
    {
        if (aliasFreshness.empty() || aliasFreshness == canonicalFreshness)
            continue;
        RegisterSnapshot(
            runtimeCacheIdentity,
            canonicalFreshness,
            snapshot,
            snapshotBytes,
            false,
            runtimeCacheIdentity,
            aliasFreshness,
            {},
            true);
    }

    (void)RegisterImportedPrefabThumbnailContinuation(
        projectRoot,
        assetId,
        sourceAssetPath,
        canonicalSubAssetKey,
        artifactPath);

    NLS_LOG_INFO(
        "resident-prefab-import-snapshot-register|asset=" + assetId.ToString() +
        "|source=" + sourceAssetPath +
        "|subAsset=" + canonicalSubAssetKey +
        "|drawItems=" + std::to_string(snapshot->drawItems.size()) +
        "|expectedDrawItems=" + std::to_string(snapshot->expectedDrawItemCount));
    return true;
}

bool ResidentPrefabPreviewRegistry::RegisterImportedPrefabThumbnailContinuation(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::AssetId& assetId,
    const std::string& sourceAssetPath,
    const std::string& prefabSubAssetKey,
    const std::string& artifactPath)
{
    if (projectRoot.empty() || !assetId.IsValid() || sourceAssetPath.empty() ||
        prefabSubAssetKey.empty() || artifactPath.empty())
    {
        return false;
    }

    const auto canonicalSubAssetKey = BuildCanonicalPrefabPreviewSubAssetKey(
        sourceAssetPath,
        prefabSubAssetKey);
    if (canonicalSubAssetKey.empty())
        return false;

    ImportedPrefabThumbnailContinuation continuation;
    continuation.projectRoot = projectRoot.lexically_normal();
    continuation.assetId = assetId;
    continuation.sourceAssetPath = std::filesystem::path(sourceAssetPath)
        .lexically_normal()
        .generic_string();
    continuation.prefabSubAssetKey = canonicalSubAssetKey;
    continuation.artifactPath = std::filesystem::path(artifactPath)
        .lexically_normal()
        .generic_string();
    continuation.registrationRevision =
        NextImportedThumbnailContinuationRevision();
    {
        std::lock_guard lock(m_mutex);
        m_importedThumbnailContinuations.insert_or_assign(
            MakeImportedThumbnailContinuationKey(projectRoot, assetId),
            continuation);
    }
    m_thumbnailWakeRevision.fetch_add(1u, std::memory_order_release);
    NLS_LOG_INFO(
        "resident-prefab-import-thumbnail-continuation-register|asset=" + assetId.ToString() +
        "|source=" + sourceAssetPath +
        "|subAsset=" + canonicalSubAssetKey +
        "|artifact=" + continuation.artifactPath);
    return true;
}

std::vector<ImportedPrefabThumbnailContinuation>
ResidentPrefabPreviewRegistry::GetImportedPrefabThumbnailContinuations(
    const std::filesystem::path& projectRoot) const
{
    const auto normalizedRoot = projectRoot.lexically_normal();
    std::vector<ImportedPrefabThumbnailContinuation> continuations;
    std::lock_guard lock(m_mutex);
    for (const auto& [_, continuation] : m_importedThumbnailContinuations)
    {
        if (continuation.projectRoot.lexically_normal() == normalizedRoot)
            continuations.push_back(continuation);
    }
    std::sort(
        continuations.begin(),
        continuations.end(),
        [](const ImportedPrefabThumbnailContinuation& left,
           const ImportedPrefabThumbnailContinuation& right)
        {
            if (left.sourceAssetPath != right.sourceAssetPath)
                return left.sourceAssetPath < right.sourceAssetPath;
            return left.prefabSubAssetKey < right.prefabSubAssetKey;
        });
    return continuations;
}

bool ResidentPrefabPreviewRegistry::HasImportedPrefabThumbnailContinuation(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::AssetId& assetId,
    const std::string& sourceAssetPath) const
{
    if (projectRoot.empty() || !assetId.IsValid() || sourceAssetPath.empty())
        return false;

    const auto key = MakeImportedThumbnailContinuationKey(projectRoot, assetId);
    const auto normalizedSource = std::filesystem::path(sourceAssetPath)
        .lexically_normal()
        .generic_string();
    std::lock_guard lock(m_mutex);
    const auto found = m_importedThumbnailContinuations.find(key);
    return found != m_importedThumbnailContinuations.end() &&
        found->second.sourceAssetPath == normalizedSource;
}

uint64_t ResidentPrefabPreviewRegistry::GetImportedPrefabThumbnailContinuationRevision(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::AssetId& assetId,
    const std::string& sourceAssetPath) const
{
    if (projectRoot.empty() || !assetId.IsValid() || sourceAssetPath.empty())
        return 0u;

    const auto key = MakeImportedThumbnailContinuationKey(projectRoot, assetId);
    const auto normalizedSource = std::filesystem::path(sourceAssetPath)
        .lexically_normal()
        .generic_string();
    std::lock_guard lock(m_mutex);
    const auto found = m_importedThumbnailContinuations.find(key);
    return found != m_importedThumbnailContinuations.end() &&
        found->second.sourceAssetPath == normalizedSource
        ? found->second.registrationRevision
        : 0u;
}

void ResidentPrefabPreviewRegistry::CompleteImportedPrefabThumbnailContinuation(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::AssetId& assetId)
{
    if (projectRoot.empty() || !assetId.IsValid())
        return;

    bool removed = false;
    {
        std::lock_guard lock(m_mutex);
        removed = m_importedThumbnailContinuations.erase(
            MakeImportedThumbnailContinuationKey(projectRoot, assetId)) != 0u;
        if (removed)
        {
            const auto identityPrefix = "prefab:" + assetId.ToString() + ":";
            for (auto& [_, entry] : m_entries)
            {
                if (entry.runtimeCacheIdentity.rfind(identityPrefix, 0u) == 0u)
                    entry.importedThumbnailPending = false;
            }
            EvictInactiveLocked();
        }
    }
    if (removed)
        m_thumbnailWakeRevision.fetch_add(1u, std::memory_order_release);
}

std::optional<ResidentPrefabPreviewRegistry::Lease>
ResidentPrefabPreviewRegistry::RegisterPrefabSnapshotForScene(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::AssetId& assetId,
    const std::string& sourceAssetPath,
    const std::string& subAssetKey,
    const std::string& artifactPath,
    const std::string& runtimeCacheIdentity,
    const NLS::Engine::Assets::PrefabArtifact& prefab)
{
    if (runtimeCacheIdentity.empty() || !assetId.IsValid())
        return std::nullopt;

    // Compute freshness before doing any snapshot work. A scene can register
    // the same canonical Prefab more than once; an exact identity/freshness
    // hit already owns the immutable snapshot and only needs another lease.
    const auto canonicalSubAssetKey = BuildCanonicalPrefabPreviewSubAssetKey(
        sourceAssetPath,
        subAssetKey);
    // Scene entries use one canonical sub-resource spelling. Imported model
    // rows may still request model:<name>; aliases below preserve that request
    // freshness without duplicating the resident snapshot itself.
    const auto& freshnessSubAssetKey = canonicalSubAssetKey;
    const auto freshnessFingerprint = BuildPrefabThumbnailDependencyStamp(
        projectRoot,
        assetId,
        sourceAssetPath,
        freshnessSubAssetKey,
        artifactPath);
    if (freshnessFingerprint.empty())
        return std::nullopt;

    if (auto existingLease = Acquire(runtimeCacheIdentity, freshnessFingerprint, false, true);
        existingLease.has_value())
    {
        return existingLease;
    }

    auto snapshot = std::make_shared<const PreviewRenderableSnapshot>(
        BuildPreviewRenderableSnapshot(prefab));
    if (snapshot->drawItems.empty())
        return std::nullopt;

    const size_t snapshotBytes = EstimatePreviewSnapshotBytes(*snapshot);

    const auto lookupIdentity = BuildResidentPrefabRuntimeCacheIdentity(
        assetId.ToString(),
        canonicalSubAssetKey);
    // Asset Browser can enqueue a resident request before its deferred
    // manifest lookup has resolved the artifact path. Keep a second alias for
    // that metadata-only request, while the normal path still requires the
    // complete artifact-file freshness fingerprint.
    const auto unresolvedFreshnessFingerprint = BuildPrefabThumbnailDependencyStamp(
        projectRoot,
        assetId,
        sourceAssetPath,
        freshnessSubAssetKey,
        {});
    const auto importerFreshnessFingerprint = BuildPrefabThumbnailDependencyStamp(
        projectRoot,
        assetId,
        sourceAssetPath,
        subAssetKey.empty() ? canonicalSubAssetKey : subAssetKey,
        artifactPath);
    const auto importerUnresolvedFreshnessFingerprint = BuildPrefabThumbnailDependencyStamp(
        projectRoot,
        assetId,
        sourceAssetPath,
        subAssetKey.empty() ? canonicalSubAssetKey : subAssetKey,
        {});
    NLS_LOG_INFO(
        "resident-prefab-snapshot-register|lookupIdentity=" + lookupIdentity +
        "|freshnessHash=" + std::to_string(ResidentDiagnosticHash(freshnessFingerprint)) +
        "|unresolvedFreshnessHash=" +
        std::to_string(ResidentDiagnosticHash(unresolvedFreshnessFingerprint)) +
        "|drawItems=" + std::to_string(snapshot->drawItems.size()) +
        "|expectedDrawItems=" + std::to_string(snapshot->expectedDrawItemCount));
    const auto snapshotForImporterAlias = snapshot;
    RegisterSnapshot(
        runtimeCacheIdentity,
        freshnessFingerprint,
        std::move(snapshot),
        snapshotBytes,
        false,
        lookupIdentity,
        unresolvedFreshnessFingerprint);
    // Imported model rows retain model:<name> in their request/cache
    // freshness. Keep that spelling as an alias to the canonical scene entry
    // so the request can reuse the resident snapshot without changing the
    // artifact lookup identity. The second registration only adds the
    // no-artifact importer alias; the entry itself remains the canonical one.
    if (snapshotForImporterAlias != nullptr &&
        importerFreshnessFingerprint != freshnessFingerprint)
    {
        RegisterSnapshot(
            runtimeCacheIdentity,
            freshnessFingerprint,
            snapshotForImporterAlias,
            snapshotBytes,
            false,
            lookupIdentity,
            importerFreshnessFingerprint);
    }
    if (snapshotForImporterAlias != nullptr &&
        importerUnresolvedFreshnessFingerprint != importerFreshnessFingerprint &&
        importerUnresolvedFreshnessFingerprint != unresolvedFreshnessFingerprint)
    {
        RegisterSnapshot(
            runtimeCacheIdentity,
            freshnessFingerprint,
            snapshotForImporterAlias,
            snapshotBytes,
            false,
            lookupIdentity,
            importerUnresolvedFreshnessFingerprint);
    }
    // Keep the scene lease distinguishable from short-lived thumbnail leases.
    // A request may carry a newer importer freshness stamp while the loaded
    // scene still owns the exact runtime entry; only that scene-owned entry is
    // eligible for the identity fallback below.
    return Acquire(runtimeCacheIdentity, freshnessFingerprint, false, true);
}

std::optional<ResidentPrefabPreviewRegistry::Lease>
ResidentPrefabPreviewRegistry::EnsureLivePrefabSnapshotForScene(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::AssetId& assetId,
    const std::string& sourceAssetPath,
    const std::string& subAssetKey,
    const std::string& artifactPath,
    const std::string& runtimeCacheIdentity,
    const NLS::Engine::Assets::PrefabArtifact& prefab,
    const std::unordered_map<
        const NLS::Engine::GameObject*,
        NLS::Engine::Serialize::ObjectId>& sourceByInstanceObject,
    NLS::Core::ResourceManagement::MeshManager& meshManager,
    NLS::Core::ResourceManagement::MaterialManager& materialManager,
    NLS::Core::ResourceManagement::TextureManager* textureManager,
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry& resourceLifetimeRegistry)
{
    if (!assetId.IsValid() || sourceByInstanceObject.empty())
        return std::nullopt;

    const auto canonicalSubAssetKey = BuildCanonicalPrefabPreviewSubAssetKey(
        sourceAssetPath,
        subAssetKey);
    if (canonicalSubAssetKey.empty())
        return std::nullopt;

    const auto lookupIdentity = BuildResidentPrefabRuntimeCacheIdentity(
        assetId.ToString(),
        canonicalSubAssetKey);
    std::optional<Lease> sceneLease;
    if (!HasSnapshotForRuntimeCacheIdentity(lookupIdentity))
    {
        // A loaded scene is itself a valid resident source. When no prepared
        // cache identity is available, store the entry under the same stable
        // identity used by Asset Browser requests. Thumbnail Acquire can then
        // tolerate importer-freshness spelling changes while the scene lease
        // remains alive, without reopening the Prefab artifact.
        const auto effectiveRuntimeCacheIdentity = runtimeCacheIdentity.empty()
            ? lookupIdentity
            : runtimeCacheIdentity;
        sceneLease = RegisterPrefabSnapshotForScene(
            projectRoot,
            assetId,
            sourceAssetPath,
            subAssetKey,
            artifactPath,
            effectiveRuntimeCacheIdentity,
            prefab);
    }

    // Always probe again, including when another scene instance registered the
    // snapshot first. Different instances can reach resource readiness at
    // different times, and the registry only promotes richer valid packages.
    (void)AttachRegisteredResourcesForLivePrefab(
        assetId,
        sourceByInstanceObject,
        meshManager,
        materialManager,
        textureManager,
        resourceLifetimeRegistry);
    (void)AttachRegisteredResourcesForPrefab(
        assetId,
        prefab,
        meshManager,
        materialManager,
        textureManager,
        resourceLifetimeRegistry);
    return sceneLease;
}

std::optional<ResidentPrefabPreviewRegistry::Lease> ResidentPrefabPreviewRegistry::Acquire(
    const std::string& runtimeCacheIdentity,
    const std::string& freshnessFingerprint,
    const bool thumbnailRequest,
    const bool sceneLease)
{
    std::lock_guard lock(m_mutex);
    if (thumbnailRequest)
    {
        m_lastThumbnailRequestedIdentity = CompactResidentDiagnosticValue(runtimeCacheIdentity);
        m_lastThumbnailRequestedFreshness = CompactResidentDiagnosticValue(freshnessFingerprint);
        m_lastThumbnailKnownIdentity.clear();
        m_lastThumbnailKnownFreshness.clear();
    }
    const auto key = ResolveKeyLocked(runtimeCacheIdentity, freshnessFingerprint);
    auto found = m_entries.find(key);
    if ((found == m_entries.end() || !found->second.snapshot) && thumbnailRequest)
    {
        // The scene lease is authoritative for the in-memory preview. It is
        // safe to use its entry when only the request freshness spelling drifted
        // during deferred artifact resolution; ordinary inactive entries still
        // require an exact freshness/alias match.
        for (auto iterator = m_entries.begin(); iterator != m_entries.end(); ++iterator)
        {
            if (iterator->second.runtimeCacheIdentity != runtimeCacheIdentity ||
                iterator->second.sceneLeaseCount == 0u ||
                iterator->second.snapshot == nullptr)
            {
                continue;
            }
            if (found == m_entries.end() ||
                iterator->second.lastUsedTick > found->second.lastUsedTick)
            {
                found = iterator;
            }
        }
    }
    if (found == m_entries.end() || !found->second.snapshot)
    {
        const auto identityPrefix = runtimeCacheIdentity + "\x1f";
        const auto rememberKnownEntry = [this, &runtimeCacheIdentity](const Entry& entry)
        {
            if (entry.runtimeCacheIdentity != runtimeCacheIdentity)
                return false;
            m_lastThumbnailKnownIdentity = CompactResidentDiagnosticValue(entry.runtimeCacheIdentity);
            m_lastThumbnailKnownFreshness = entry.freshnessFingerprint;
            return true;
        };
        const bool knownIdentity = std::any_of(
            m_entries.begin(),
            m_entries.end(),
            [&rememberKnownEntry](const auto& entry)
            {
                return rememberKnownEntry(entry.second);
            }) || std::any_of(
            m_aliases.begin(),
            m_aliases.end(),
            [this, &identityPrefix](const auto& alias)
            {
                if (alias.first.rfind(identityPrefix, 0u) != 0u)
                    return false;
                const auto entry = m_entries.find(alias.second);
                if (entry == m_entries.end())
                    return false;
                m_lastThumbnailKnownIdentity = CompactResidentDiagnosticValue(entry->second.runtimeCacheIdentity);
                m_lastThumbnailKnownFreshness = entry->second.freshnessFingerprint;
                return true;
            });
        if (knownIdentity)
        {
            ++m_staleCount;
            m_lastThumbnailMismatchIdentity = CompactResidentDiagnosticValue(runtimeCacheIdentity);
            // The regular request/registration fields are compacted because
            // they are emitted on every benchmark. A mismatch is rare and is
            // specifically used to diagnose identity drift, so retain the
            // complete pair here rather than hiding the differing input in a
            // truncated middle section.
            m_lastThumbnailMismatchRequestedFreshness = freshnessFingerprint;
            m_lastThumbnailMismatchKnownFreshness = m_lastThumbnailKnownFreshness;
            if (thumbnailRequest)
            {
                ++m_thumbnailStaleCount;
                ++m_thumbnailFreshnessMismatchCount;
            }
        }
        ++m_missCount;
        if (thumbnailRequest)
        {
            ++m_thumbnailMissCount;
            if (!knownIdentity)
                ++m_thumbnailIdentityMissCount;
            NLS_LOG_INFO(
                "resident-prefab-thumbnail-miss|identity=" + runtimeCacheIdentity +
                "|freshnessHash=" + std::to_string(ResidentDiagnosticHash(freshnessFingerprint)) +
                "|knownIdentity=" + std::to_string(knownIdentity ? 1u : 0u));
        }
        return std::nullopt;
    }

    ++m_hitCount;
    ++m_zeroArtifactReadHitCount;
    if (thumbnailRequest)
    {
        ++m_thumbnailHitCount;
        ++m_thumbnailZeroArtifactReadHitCount;
    }
    ++found->second.activeLeaseCount;
    if (sceneLease)
        ++found->second.sceneLeaseCount;
    found->second.lastUsedTick = ++m_tick;
    const auto owner = shared_from_this();
    const auto resolvedKey = found->first;
    return Lease(
        found->second.snapshot,
        found->second.resources,
        [owner, resolvedKey](const std::string& artifactPath)
        {
            return owner->TakePreparedMeshPayload(resolvedKey, artifactPath);
        },
        [owner, resolvedKey, sceneLease]
        {
            owner->ReleaseLease(resolvedKey, sceneLease);
        });
}

bool ResidentPrefabPreviewRegistry::AttachRegisteredResourcesForPrefab(
    const NLS::Core::Assets::AssetId& assetId,
    const NLS::Engine::Assets::PrefabArtifact& prefab,
    NLS::Core::ResourceManagement::MeshManager& meshManager,
    NLS::Core::ResourceManagement::MaterialManager& materialManager,
    NLS::Core::ResourceManagement::TextureManager* textureManager,
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry& resourceLifetimeRegistry)
{
    if (!assetId.IsValid())
        return false;
    const auto snapshot = std::make_shared<const PreviewRenderableSnapshot>(
        BuildPreviewRenderableSnapshot(prefab));
    if (snapshot->drawItems.empty() ||
        snapshot->expectedDrawItemCount != snapshot->drawItems.size())
    {
        return false;
    }
    const auto resources = BuildResidentResources(
        assetId,
        *snapshot,
        meshManager,
        materialManager,
        textureManager,
        resourceLifetimeRegistry);
    if (resources == nullptr)
        return false;

    const auto identityPrefix = "prefab:" + assetId.ToString() + ":";
    bool attached = false;
    std::lock_guard lock(m_mutex);
    std::unordered_set<std::string> targetKeys;
    for (const auto& [key, entry] : m_entries)
    {
        if (entry.runtimeCacheIdentity.rfind(identityPrefix, 0u) == 0u)
            targetKeys.insert(key);
    }
    for (const auto& [alias, key] : m_aliases)
    {
        if (alias.rfind(identityPrefix, 0u) == 0u && m_entries.find(key) != m_entries.end())
            targetKeys.insert(key);
    }
    for (const auto& key : targetKeys)
    {
        const auto found = m_entries.find(key);
        const auto sourceSnapshot = found != m_entries.end() &&
            found->second.sourceSnapshot != nullptr
            ? found->second.sourceSnapshot
            : found != m_entries.end() ? found->second.snapshot : nullptr;
        if (found == m_entries.end() || sourceSnapshot == nullptr ||
            !SamePreviewSnapshot(*sourceSnapshot, *snapshot))
        {
            continue;
        }

        // Scene restoration is incremental and can probe an intermediate
        // object/resource view after a later probe has already published a
        // complete package. Never regress a valid manager generation to a
        // smaller package: doing so makes the thumbnail renderer reuse an
        // older partial snapshot and can keep it in WaitingForResources
        // forever. A manager-generation change is the one case where a
        // smaller package is allowed because the old handles are invalid.
        const bool existingResourcesValid = found->second.resources != nullptr &&
            found->second.resources->IsValidFor(
                meshManager,
                materialManager,
                textureManager);
        const bool existingTopologyComplete =
            PreviewSnapshotTopologyComplete(found->second.snapshot);
        const bool existingWakeResourcesComplete =
            PreviewResourcesComplete(found->second.resources);
        const size_t existingReadyDrawItemCount = found->second.snapshot != nullptr
            ? found->second.snapshot->drawItems.size()
            : 0u;
        const bool candidateComplete = resources->IsCompleteForSource();
        const bool existingComplete = existingResourcesValid &&
            found->second.resources->IsCompleteForSource();
        if (existingResourcesValid &&
            (snapshot->drawItems.size() < existingReadyDrawItemCount ||
                (existingComplete && !candidateComplete)))
        {
            found->second.lastUsedTick = ++m_tick;
            attached = true;
            NLS_LOG_INFO(
                "resident-prefab-resources-regression-skip|mode=prefab|existing=" +
                std::to_string(existingReadyDrawItemCount) +
                "|candidate=" + std::to_string(snapshot->drawItems.size()) +
                "|existingComplete=" + std::to_string(existingComplete ? 1u : 0u) +
                "|candidateComplete=" + std::to_string(candidateComplete ? 1u : 0u));
            continue;
        }

        // Scene restore may probe the same fully-resolved managers repeatedly.
        // Keep the original package when its resource identities are unchanged;
        // replacing the shared_ptr would make the renderer treat a no-op probe
        // as a new resource-plan revision and restart scene assembly.
        if (SameResidentResources(found->second.resources, resources))
        {
            found->second.lastUsedTick = ++m_tick;
            attached = true;
            continue;
        }
        m_residentBytes -= found->second.TotalByteSize();
        found->second.snapshot = snapshot;
        found->second.snapshotByteSize = EstimatePreviewSnapshotBytes(*snapshot);
        found->second.resources = resources;
        ++found->second.revision;
        m_residentBytes += found->second.TotalByteSize();
        if ((!existingTopologyComplete && PreviewSnapshotTopologyComplete(found->second.snapshot)) ||
            (!existingWakeResourcesComplete && candidateComplete))
        {
            m_thumbnailWakeRevision.fetch_add(1u, std::memory_order_release);
        }
        attached = true;
    }
    return attached;
}

bool ResidentPrefabPreviewRegistry::AttachRegisteredResourcesForLivePrefab(
    const NLS::Core::Assets::AssetId& assetId,
    const std::unordered_map<
        const NLS::Engine::GameObject*,
        NLS::Engine::Serialize::ObjectId>& sourceByInstanceObject,
    NLS::Core::ResourceManagement::MeshManager& meshManager,
    NLS::Core::ResourceManagement::MaterialManager& materialManager,
    NLS::Core::ResourceManagement::TextureManager* textureManager,
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry& resourceLifetimeRegistry)
{
    if (!assetId.IsValid() || sourceByInstanceObject.empty())
    {
        NLS_LOG_INFO(
            "resident-prefab-live-attach-skip|asset=" + assetId.ToString() +
            "|sourceMap=" + std::to_string(sourceByInstanceObject.size()));
        return false;
    }

    std::unordered_map<
        NLS::Engine::Serialize::ObjectId,
        const NLS::Engine::GameObject*> liveObjectsBySourceId;
    liveObjectsBySourceId.reserve(sourceByInstanceObject.size());
    for (const auto& [object, sourceObject] : sourceByInstanceObject)
    {
        if (object != nullptr)
            liveObjectsBySourceId.emplace(sourceObject, object);
    }

    const auto identityPrefix = "prefab:" + assetId.ToString() + ":";
    struct Target
    {
        std::string key;
        std::shared_ptr<const PreviewRenderableSnapshot> snapshot;
    };
    std::vector<Target> targets;
    {
        std::lock_guard lock(m_mutex);
        std::unordered_set<std::string> targetKeys;
        for (const auto& [key, entry] : m_entries)
        {
            if (entry.runtimeCacheIdentity.rfind(identityPrefix, 0u) == 0u)
                targetKeys.insert(key);
        }
        for (const auto& [alias, key] : m_aliases)
        {
            if (alias.rfind(identityPrefix, 0u) == 0u && m_entries.find(key) != m_entries.end())
                targetKeys.insert(key);
        }
        for (const auto& key : targetKeys)
        {
            const auto found = m_entries.find(key);
            if (found != m_entries.end())
            {
                const auto sourceSnapshot = found->second.sourceSnapshot != nullptr
                    ? found->second.sourceSnapshot
                    : found->second.snapshot;
                if (sourceSnapshot != nullptr)
                    targets.push_back({key, sourceSnapshot});
            }
        }
    }

    NLS_LOG_INFO(
        "resident-prefab-live-attach-targets|asset=" + assetId.ToString() +
        "|sourceMap=" + std::to_string(sourceByInstanceObject.size()) +
        "|targets=" + std::to_string(targets.size()));

    bool attached = false;
    for (const auto& target : targets)
    {
        std::shared_ptr<const PreviewRenderableSnapshot> resolvedSnapshot;
        const auto resources = BuildResidentResourcesFromLiveObjects(
            assetId,
            *target.snapshot,
            liveObjectsBySourceId,
            meshManager,
            materialManager,
            textureManager,
            resourceLifetimeRegistry,
            &resolvedSnapshot);
        if (resources == nullptr)
            continue;

        std::lock_guard lock(m_mutex);
        const auto found = m_entries.find(target.key);
        const auto sourceSnapshot = found != m_entries.end() &&
            found->second.sourceSnapshot != nullptr
            ? found->second.sourceSnapshot
            : found != m_entries.end() ? found->second.snapshot : nullptr;
        if (found == m_entries.end() || sourceSnapshot != target.snapshot ||
            resolvedSnapshot == nullptr)
            continue;

        // The live-object probe is allowed to publish progressively richer
        // packages, but it must not publish a later, smaller view after the
        // scene has already exposed more resources. Preserve the existing
        // package while its manager generation is still valid; if the
        // generation changed, the new probe is authoritative.
        const bool existingResourcesValid = found->second.resources != nullptr &&
            found->second.resources->IsValidFor(
                meshManager,
                materialManager,
                textureManager);
        const bool existingTopologyComplete =
            PreviewSnapshotTopologyComplete(found->second.snapshot);
        const bool existingWakeResourcesComplete =
            PreviewResourcesComplete(found->second.resources);
        const size_t existingReadyDrawItemCount = found->second.snapshot != nullptr
            ? found->second.snapshot->drawItems.size()
            : 0u;
        const bool candidateComplete = resources->IsCompleteForSource();
        const bool existingComplete = existingResourcesValid &&
            found->second.resources->IsCompleteForSource();
        if (existingResourcesValid &&
            (resolvedSnapshot->drawItems.size() < existingReadyDrawItemCount ||
                (existingComplete && !candidateComplete)))
        {
            found->second.lastUsedTick = ++m_tick;
            attached = true;
            NLS_LOG_INFO(
                "resident-prefab-resources-regression-skip|mode=live|existing=" +
                std::to_string(existingReadyDrawItemCount) +
                "|candidate=" + std::to_string(resolvedSnapshot->drawItems.size()) +
                "|existingComplete=" + std::to_string(existingComplete ? 1u : 0u) +
                "|candidateComplete=" + std::to_string(candidateComplete ? 1u : 0u));
            continue;
        }

        if (SamePreviewSnapshot(
                found->second.snapshot != nullptr
                    ? *found->second.snapshot
                    : *resolvedSnapshot,
                *resolvedSnapshot) &&
            SameResidentResources(found->second.resources, resources))
        {
            found->second.lastUsedTick = ++m_tick;
            attached = true;
            continue;
        }
        m_residentBytes -= found->second.TotalByteSize();
        found->second.snapshot = resolvedSnapshot;
        found->second.snapshotByteSize = EstimatePreviewSnapshotBytes(*resolvedSnapshot);
        found->second.resources = resources;
        ++found->second.revision;
        m_residentBytes += found->second.TotalByteSize();
        found->second.lastUsedTick = ++m_tick;
        if ((!existingTopologyComplete && PreviewSnapshotTopologyComplete(found->second.snapshot)) ||
            (!existingWakeResourcesComplete && candidateComplete))
        {
            m_thumbnailWakeRevision.fetch_add(1u, std::memory_order_release);
        }
        attached = true;
        NLS_LOG_INFO(
            "resident-prefab-resources-attached-live|asset=" + assetId.ToString() +
            "|drawItems=" + std::to_string(resources->drawItems.size()) +
            "|meshes=" + std::to_string(resources->meshes.size()) +
            "|materials=" + std::to_string(resources->materials.size()) +
            "|textures=" + std::to_string(resources->textures.size()));
    }
    return attached;
}

bool ResidentPrefabPreviewRegistry::HasSnapshotForRuntimeCacheIdentity(
    const std::string& runtimeCacheIdentity) const
{
    if (runtimeCacheIdentity.empty())
        return false;

    std::lock_guard lock(m_mutex);
    const auto identityPrefix = runtimeCacheIdentity + "\x1f";
    for (const auto& [key, entry] : m_entries)
    {
        if (entry.snapshot && entry.runtimeCacheIdentity == runtimeCacheIdentity)
            return true;

        if (!entry.snapshot)
            continue;
        for (const auto& [alias, target] : m_aliases)
        {
            if (target == key && alias.rfind(identityPrefix, 0u) == 0u)
                return true;
        }
    }
    return false;
}

void ResidentPrefabPreviewRegistry::RecordThumbnailRequest(
    const std::string& runtimeCacheIdentity,
    const std::string& freshnessFingerprint)
{
    std::lock_guard lock(m_mutex);
    ++m_thumbnailRequestCount;
    const auto found = m_thumbnailRequestIdentityCounts.find(runtimeCacheIdentity);
    if (found != m_thumbnailRequestIdentityCounts.end())
        ++found->second;
    else if (m_thumbnailRequestIdentityCounts.size() < kMaxResidentThumbnailRequestIdentitySamples)
        m_thumbnailRequestIdentityCounts.emplace(runtimeCacheIdentity, 1u);
    else
        ++m_thumbnailRequestOtherIdentityCount;
    m_lastThumbnailRequestedIdentity = CompactResidentDiagnosticValue(runtimeCacheIdentity);
    m_lastThumbnailRequestedFreshness = CompactResidentDiagnosticValue(freshnessFingerprint);
    m_lastThumbnailKnownIdentity.clear();
    m_lastThumbnailKnownFreshness.clear();
}

std::weak_ptr<const PreviewRenderableSnapshot> ResidentPrefabPreviewRegistry::FindWeakSnapshot(
    const std::string& runtimeCacheIdentity,
    const std::string& freshnessFingerprint) const
{
    std::lock_guard lock(m_mutex);
    const auto key = ResolveKeyLocked(runtimeCacheIdentity, freshnessFingerprint);
    auto found = m_entries.find(key);
    if ((found == m_entries.end() || !found->second.snapshot))
    {
        for (auto iterator = m_entries.begin(); iterator != m_entries.end(); ++iterator)
        {
            if (iterator->second.runtimeCacheIdentity == runtimeCacheIdentity &&
                iterator->second.sceneLeaseCount != 0u &&
                iterator->second.snapshot != nullptr)
            {
                if (found == m_entries.end() ||
                    iterator->second.lastUsedTick > found->second.lastUsedTick)
                {
                    found = iterator;
                }
            }
        }
    }
    if (found == m_entries.end() || !found->second.snapshot)
        return {};
    return found->second.snapshot;
}

std::optional<ResidentPrefabPreviewRegistry::SnapshotState>
ResidentPrefabPreviewRegistry::GetSnapshotState(
    const std::string& runtimeCacheIdentity,
    const std::string& freshnessFingerprint) const
{
    if (runtimeCacheIdentity.empty() || freshnessFingerprint.empty())
        return std::nullopt;

    std::lock_guard lock(m_mutex);
    const auto key = ResolveKeyLocked(runtimeCacheIdentity, freshnessFingerprint);
    auto found = m_entries.find(key);
    if ((found == m_entries.end() || found->second.snapshot == nullptr))
    {
        for (auto iterator = m_entries.begin(); iterator != m_entries.end(); ++iterator)
        {
            if (iterator->second.runtimeCacheIdentity == runtimeCacheIdentity &&
                iterator->second.sceneLeaseCount != 0u &&
                iterator->second.snapshot != nullptr)
            {
                if (found == m_entries.end() ||
                    iterator->second.lastUsedTick > found->second.lastUsedTick)
                {
                    found = iterator;
                }
            }
        }
    }
    if (found == m_entries.end() || found->second.snapshot == nullptr)
        return std::nullopt;

    SnapshotState state;
    state.revision = found->second.revision;
    state.readyDrawItemCount = found->second.snapshot->drawItems.size();
    state.expectedDrawItemCount = found->second.sourceSnapshot != nullptr
        ? found->second.sourceSnapshot->expectedDrawItemCount
        : found->second.snapshot->expectedDrawItemCount;
    const bool topologyComplete = state.expectedDrawItemCount != 0u &&
        state.readyDrawItemCount >= state.expectedDrawItemCount;
    // A resource package can expose the full draw topology before all texture
    // bindings have finished resolving. Keep that frame provisional so the
    // service may display it in memory but cannot persist it as canonical.
    state.complete = topologyComplete &&
        (found->second.resources == nullptr ||
            found->second.resources->IsCompleteForSource());
    state.allowArtifactResourceLoading = found->second.allowArtifactResourceLoading;
    return state;
}

void ResidentPrefabPreviewRegistry::SetSceneRestoreInProgress(const bool inProgress)
{
    m_sceneRestoreInProgress.store(inProgress, std::memory_order_release);
}

bool ResidentPrefabPreviewRegistry::IsSceneRestoreInProgress() const
{
    return m_sceneRestoreInProgress.load(std::memory_order_acquire);
}

void ResidentPrefabPreviewRegistry::Remove(
    const std::string& runtimeCacheIdentity,
    const std::string& freshnessFingerprint)
{
    std::lock_guard lock(m_mutex);
    const auto key = ResolveKeyLocked(runtimeCacheIdentity, freshnessFingerprint);
    const auto found = m_entries.find(key);
    if (found == m_entries.end() || found->second.activeLeaseCount != 0u)
        return;
    m_residentBytes -= found->second.TotalByteSize();
    RemoveAliasesForKeyLocked(key);
    m_entries.erase(found);
    m_thumbnailWakeRevision.fetch_add(1u, std::memory_order_release);
}

void ResidentPrefabPreviewRegistry::SetInactiveBudgetBytes(const size_t budgetBytes)
{
    std::lock_guard lock(m_mutex);
    m_inactiveBudgetBytes = budgetBytes;
    EvictInactiveLocked();
}

ResidentPrefabPreviewRegistry::Stats ResidentPrefabPreviewRegistry::GetStats() const
{
    std::lock_guard lock(m_mutex);
    Stats stats;
    stats.entryCount = m_entries.size();
    stats.residentBytes = m_residentBytes;
    stats.hitCount = m_hitCount;
    stats.missCount = m_missCount;
    stats.staleCount = m_staleCount;
    stats.zeroArtifactReadHitCount = m_zeroArtifactReadHitCount;
    stats.evictionCount = m_evictionCount;
    stats.thumbnailHitCount = m_thumbnailHitCount;
    stats.thumbnailMissCount = m_thumbnailMissCount;
    stats.thumbnailStaleCount = m_thumbnailStaleCount;
    stats.thumbnailZeroArtifactReadHitCount = m_thumbnailZeroArtifactReadHitCount;
    stats.thumbnailIdentityMissCount = m_thumbnailIdentityMissCount;
    stats.thumbnailFreshnessMismatchCount = m_thumbnailFreshnessMismatchCount;
    stats.thumbnailRequestCount = m_thumbnailRequestCount;
    stats.thumbnailRequestOtherIdentityCount = m_thumbnailRequestOtherIdentityCount;
    stats.thumbnailRequestIdentityCounts.reserve(m_thumbnailRequestIdentityCounts.size());
    for (const auto& [identity, count] : m_thumbnailRequestIdentityCounts)
    {
        stats.thumbnailRequestIdentityCounts.push_back({
            CompactResidentDiagnosticValue(identity),
            count
        });
    }
    std::sort(
        stats.thumbnailRequestIdentityCounts.begin(),
        stats.thumbnailRequestIdentityCounts.end(),
        [](const auto& left, const auto& right)
        {
            if (left.count != right.count)
                return left.count > right.count;
            return left.identity < right.identity;
        });
    stats.lastRegisteredIdentity = m_lastRegisteredIdentity;
    stats.lastRegisteredLookupIdentity = m_lastRegisteredLookupIdentity;
    stats.lastRegisteredFreshness = m_lastRegisteredFreshness;
    stats.lastThumbnailRequestedIdentity = m_lastThumbnailRequestedIdentity;
    stats.lastThumbnailRequestedFreshness = m_lastThumbnailRequestedFreshness;
    stats.lastThumbnailKnownIdentity = m_lastThumbnailKnownIdentity;
    stats.lastThumbnailKnownFreshness = m_lastThumbnailKnownFreshness;
    stats.lastThumbnailMismatchIdentity = m_lastThumbnailMismatchIdentity;
    stats.lastThumbnailMismatchRequestedFreshness = m_lastThumbnailMismatchRequestedFreshness;
    stats.lastThumbnailMismatchKnownFreshness = m_lastThumbnailMismatchKnownFreshness;
    for (const auto& [_, entry] : m_entries)
        stats.activeLeaseCount += entry.activeLeaseCount;
    return stats;
}

void ResidentPrefabPreviewRegistry::ReleaseLease(const std::string& key, const bool sceneLease)
{
    std::lock_guard lock(m_mutex);
    const auto found = m_entries.find(key);
    if (found == m_entries.end())
        return;
    if (found->second.activeLeaseCount > 0u)
        --found->second.activeLeaseCount;
    if (sceneLease && found->second.sceneLeaseCount > 0u)
        --found->second.sceneLeaseCount;
    found->second.lastUsedTick = ++m_tick;
    EvictInactiveLocked();
}

std::shared_ptr<const std::vector<uint8_t>>
ResidentPrefabPreviewRegistry::TakePreparedMeshPayload(
    const std::string& key,
    const std::string& artifactPath)
{
    const auto normalizedPath = NormalizePreparedPreviewPayloadPath(artifactPath);
    if (normalizedPath.empty())
        return {};

    std::lock_guard lock(m_mutex);
    const auto found = m_entries.find(key);
    if (found == m_entries.end())
        return {};
    const auto payload = found->second.preparedMeshPayloads.find(normalizedPath);
    if (payload == found->second.preparedMeshPayloads.end())
        return {};

    auto bytes = std::move(payload->second);
    const auto byteCount = bytes != nullptr ? bytes->size() : 0u;
    found->second.preparedMeshPayloads.erase(payload);
    found->second.preparedMeshPayloadBytes -=
        (std::min)(found->second.preparedMeshPayloadBytes, byteCount);
    m_residentBytes -= (std::min)(m_residentBytes, byteCount);
    return bytes;
}

std::string ResidentPrefabPreviewRegistry::ResolveKeyLocked(
    const std::string& identity,
    const std::string& freshnessFingerprint) const
{
    const auto directKey = MakeKey(identity, freshnessFingerprint);
    if (m_entries.find(directKey) != m_entries.end())
        return directKey;

    const auto alias = m_aliases.find(directKey);
    return alias != m_aliases.end() ? alias->second : directKey;
}

void ResidentPrefabPreviewRegistry::RemoveAliasesForKeyLocked(const std::string& key)
{
    for (auto iterator = m_aliases.begin(); iterator != m_aliases.end();)
    {
        if (iterator->second == key)
            iterator = m_aliases.erase(iterator);
        else
            ++iterator;
    }
}

void ResidentPrefabPreviewRegistry::EvictInactiveLocked(const std::string* protectedKey)
{
    while (m_residentBytes > m_inactiveBudgetBytes)
    {
        // A batch import can publish several multi-gigabyte prepared payload
        // sets before the Asset Browser renderer exists. Retain each complete
        // topology until its continuation finishes, but keep at most the newest
        // protected payload set so memory remains bounded.
        auto payloadCandidate = m_entries.end();
        for (auto iterator = m_entries.begin(); iterator != m_entries.end(); ++iterator)
        {
            if (protectedKey != nullptr && iterator->first == *protectedKey)
                continue;
            if (iterator->second.activeLeaseCount != 0u ||
                !iterator->second.importedThumbnailPending ||
                iterator->second.preparedMeshPayloads.empty())
            {
                continue;
            }
            if (payloadCandidate == m_entries.end() ||
                iterator->second.lastUsedTick < payloadCandidate->second.lastUsedTick)
            {
                payloadCandidate = iterator;
            }
        }
        if (payloadCandidate != m_entries.end())
        {
            m_residentBytes -= (std::min)(
                m_residentBytes,
                payloadCandidate->second.preparedMeshPayloadBytes);
            payloadCandidate->second.preparedMeshPayloads.clear();
            payloadCandidate->second.preparedMeshPayloadBytes = 0u;
            continue;
        }

        auto candidate = m_entries.end();
        for (auto iterator = m_entries.begin(); iterator != m_entries.end(); ++iterator)
        {
            if (protectedKey != nullptr && iterator->first == *protectedKey)
                continue;
            if (iterator->second.activeLeaseCount != 0u)
                continue;
            if (iterator->second.importedThumbnailPending)
                continue;
            if (candidate == m_entries.end() ||
                iterator->second.lastUsedTick < candidate->second.lastUsedTick)
            {
                candidate = iterator;
            }
        }
        if (candidate == m_entries.end())
            break;
        const auto key = candidate->first;
        m_residentBytes -= candidate->second.TotalByteSize();
        m_entries.erase(candidate);
        RemoveAliasesForKeyLocked(key);
        ++m_evictionCount;
        m_thumbnailWakeRevision.fetch_add(1u, std::memory_order_release);
    }
}

std::string BuildResidentPrefabRuntimeCacheIdentity(
    const std::string& assetId,
    const std::string& subAssetKey)
{
    return "prefab:" + assetId + ":" + subAssetKey;
}

std::string BuildCanonicalPrefabPreviewSubAssetKey(
    const std::string& sourceAssetPath,
    const std::string& subAssetKey)
{
    if (!subAssetKey.empty())
    {
        // Imported model scenes historically exposed model:<name> while the
        // canonical prefab artifact is keyed by the source stem. Match the
        // same normalization used by the shared prefab loader.
        if (subAssetKey.rfind("model:", 0u) == 0u)
        {
            const auto stem = std::filesystem::path(sourceAssetPath).stem().generic_string();
            return stem.empty() ? subAssetKey : "prefab:" + stem;
        }
        return subAssetKey;
    }

    const auto stem = std::filesystem::path(sourceAssetPath).stem().generic_string();
    return stem.empty() ? std::string {} : "prefab:" + stem;
}
}

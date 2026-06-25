#include "Assets/EditorThumbnailPreviewRenderer.h"

#include "Assets/AssetThumbnailService.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/ArtifactDatabaseManifestUtils.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/EditorAssetPath.h"
#include "Assets/NativeArtifactContainer.h"
#include "Assets/PreviewRenderableSnapshot.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Core/EditorActions.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Debug/Logger.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Engine/Rendering/SceneRendererFactory.h"
#include "GameObject.h"
#include "Profiling/PerformanceStageStats.h"
#include "Rendering/Buffers/Framebuffer.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "SceneSystem/Scene.h"
#include "ServiceLocator.h"

#include <algorithm>
#include <any>
#include <array>
#include <cmath>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NLS::Editor::Assets
{
EditorThumbnailPreviewReadbackPollResult PollEditorThumbnailPreviewReadback(
    EditorThumbnailPreviewReadbackState& state,
    const std::string& requestKey,
    const NLS::Render::Context::Driver* driver)
{
    EditorThumbnailPreviewReadbackPollResult result;
    if (!state.active || state.completion == nullptr)
        return result;

    NLS::Render::RHI::RHICompletionStatusCode completionCode =
        NLS::Render::RHI::RHICompletionStatusCode::Pending;
    std::string completionMessage;
    if (driver != nullptr)
    {
        const auto polled = NLS::Render::Context::DriverRendererAccess::PollReadbackCompletion(
            *driver,
            NLS::Render::RHI::RHIReadbackResult{
                NLS::Render::RHI::RHIReadbackStatusCode::Success,
                {},
                state.completion
            });
        completionMessage = polled.message;
        if (polled.completion != nullptr && polled.Succeeded())
        {
            completionCode = NLS::Render::RHI::RHICompletionStatusCode::Pending;
        }
        else if (polled.code == NLS::Render::RHI::RHIReadbackStatusCode::Success)
        {
            completionCode = NLS::Render::RHI::RHICompletionStatusCode::Success;
        }
        else if (polled.code == NLS::Render::RHI::RHIReadbackStatusCode::DeviceLost)
        {
            completionCode = NLS::Render::RHI::RHICompletionStatusCode::DeviceLost;
        }
        else
        {
            completionCode = NLS::Render::RHI::RHICompletionStatusCode::Failed;
        }
    }
    else
    {
        const auto status = state.completion->Poll();
        completionCode = status.code;
        completionMessage = status.message;
    }

    if (completionCode == NLS::Render::RHI::RHICompletionStatusCode::Pending)
    {
        result.status = EditorThumbnailPreviewReadbackPollStatus::Pending;
        return result;
    }

    const bool matchesRequest = state.requestKey == requestKey;
    if (matchesRequest)
    {
        result.preview.width = state.width;
        result.preview.height = state.height;
        if (state.rgbaPixels != nullptr)
            result.preview.rgbaPixels = std::move(*state.rgbaPixels);
    }
    result.preview.diagnostic = completionMessage;
    state = {};

    if (!matchesRequest)
    {
        result.status = EditorThumbnailPreviewReadbackPollStatus::Superseded;
        return result;
    }

    if (completionCode == NLS::Render::RHI::RHICompletionStatusCode::Success)
    {
        result.status = EditorThumbnailPreviewReadbackPollStatus::Ready;
        return result;
    }
    if (completionCode == NLS::Render::RHI::RHICompletionStatusCode::DeviceLost)
    {
        result.status = EditorThumbnailPreviewReadbackPollStatus::DeviceLost;
        return result;
    }

    result.status = EditorThumbnailPreviewReadbackPollStatus::Failed;
    return result;
}

NLS::Engine::Serialize::LoadPolicy BuildEditorThumbnailPreviewLoadPolicy()
{
    NLS::Engine::Serialize::LoadPolicy policy;
    policy.deferAssetReferenceResolution = true;
    policy.suppressGameObjectCreatedEvents = true;
    policy.deferActivation = true;
    policy.synchronousAssetReferencePrewarm = false;
    policy.rebuildRuntimeCachesAfterLoad = false;
    return policy;
}

namespace
{
constexpr float kUnityMeshPreviewFieldOfViewDegrees = 30.0f;
constexpr float kUnityMeshPreviewYawDegrees = -120.0f;
constexpr float kUnityMeshPreviewPitchDegrees = 20.0f;
constexpr float kUnityPrefabPreviewYawDegrees = -135.0f;
constexpr float kUnityPrefabPreviewPitchDegrees = -36.0f;
constexpr float kUnityMaterialPreviewDistance = 5.0f;
constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;
const NLS::Maths::Vector3 kThumbnailPreviewKeyLightDirection{0.35f, -0.72f, 0.60f};
constexpr float kThumbnailPreviewKeyLightIntensity = 0.75f;
constexpr float kThumbnailPreviewAmbientIntensity = 0.24f;
constexpr size_t kMaxGpuPreviewNativeArtifactFileBytes = 16u * 1024u * 1024u;
constexpr size_t kMaxGpuPreviewStructurePayloadBytes = 2u * 1024u * 1024u;
constexpr size_t kMaxGpuPreviewPrefabPayloadBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxGpuPreviewPrefabGraphObjects = 24000u;
constexpr size_t kMaxGpuPreviewPrefabGraphProperties = 160000u;
constexpr size_t kMaxGpuPreviewPrefabResolvedAssets = 4096u;
constexpr size_t kMaxGpuPreviewPrefabDrawItems = 64u;
constexpr size_t kMaxGpuPreviewMeshVertices = 240000u;
constexpr size_t kMaxGpuPreviewMeshIndices = 720000u;
constexpr size_t kMaxPreviewRenderableSnapshotCacheEntries = 64u;
constexpr size_t kThumbnailPreviewMeshPumpCompletionsPerFrame = 8u;
constexpr size_t kThumbnailPreviewMaterialPumpCompletionsPerFrame = 8u;
constexpr size_t kThumbnailPreviewTexturePumpCompletionsPerFrame = 8u;
constexpr size_t kMaxRetiredPreviewReadbacks = 32u;
constexpr const char* kGpuPreviewMeshBudgetExceededDiagnostic = "thumbnail-model-preview-budget-exceeded";
constexpr const char* kGpuPreviewMaterialBudgetExceededDiagnostic = "thumbnail-material-preview-budget-exceeded";
constexpr const char* kGpuPreviewPrefabBudgetExceededDiagnostic = "thumbnail-prefab-preview-budget-exceeded";

std::string ToGenericPath(const std::filesystem::path& path)
{
    return path.lexically_normal().generic_string();
}

std::optional<NLS::Core::Assets::ArtifactManifest> LoadManifest(const AssetThumbnailRequest& request)
{
    return LoadArtifactManifestFromProjectArtifactDB(request.projectRoot, request.assetId);
}

std::string BuildPreviewReadbackRequestKey(const AssetThumbnailRequest& request)
{
    return std::to_string(static_cast<int>(request.kind)) + "|" +
        request.assetId.ToString() + "|" +
        request.subAssetKey + "|" +
        std::to_string(request.requestedSize) + "|" +
        request.settingsFingerprint + "|" +
        request.dependencyStamp + "|" +
        request.previewRendererVersion + "|" +
        request.artifactPath + "|" +
        request.sourceAssetPath;
}

std::string BuildPreviewSnapshotCacheKey(const AssetThumbnailRequest& request)
{
    std::string key;
    key.reserve(256u + request.freshnessInputs.size() * 48u);
    key += "preview-snapshot:v1|";
    key += ToGenericPath(request.projectRoot);
    key += '|';
    key += request.assetId.ToString();
    key += '|';
    key += request.subAssetKey;
    key += '|';
    key += request.artifactPath;
    key += '|';
    key += request.sourceAssetPath;
    key += '|';
    key += request.dependencyStamp;
    key += '|';
    key += request.previewRendererVersion;
    key += '|';
    key += request.settingsFingerprint;
    for (const auto& [name, value] : request.freshnessInputs)
    {
        key += "|fresh:";
        key += name;
        key += '=';
        key += value;
    }
    return key;
}

struct PreviewResourcePathSet
{
    std::unordered_set<std::string> meshPaths;
    std::unordered_set<std::string> materialPaths;
    std::unordered_set<std::string> texturePaths;
};

std::optional<std::filesystem::path> ResolveArtifactPath(
    const AssetThumbnailRequest& request,
    const std::string& artifactPath)
{
    if (artifactPath.empty() || !request.assetId.IsValid())
        return std::nullopt;

    const auto rawPath = std::filesystem::path(artifactPath).lexically_normal();
    const auto artifactRoot = NLS::Core::Assets::NormalizeAssetPath(
        request.projectRoot / "Library" / "Artifacts");
    if (artifactRoot.empty())
        return std::nullopt;

    auto resolveCandidate = [&artifactRoot](const std::filesystem::path& candidate)
        -> std::optional<std::filesystem::path>
    {
        const auto normalized = NLS::Core::Assets::NormalizeAssetPath(candidate);
        if (normalized.empty() ||
            !IsPhysicalRegularFileInsideEditorAssetRoot(normalized, artifactRoot))
        {
            return std::nullopt;
        }
        return normalized;
    };

    if (rawPath.is_absolute())
        return resolveCandidate(rawPath);

    if (auto resolved = resolveCandidate(request.projectRoot / rawPath);
        resolved.has_value())
    {
        return resolved;
    }
    return resolveCandidate(artifactRoot / rawPath);
}

std::vector<std::filesystem::path> ResolveMeshArtifactPaths(const AssetThumbnailRequest& request)
{
    std::vector<std::filesystem::path> paths;
    if (request.kind == AssetThumbnailKind::PrefabPreview &&
        (request.subAssetKey.empty() || request.subAssetKey.rfind("mesh:", 0u) != 0u))
    {
        return paths;
    }

    const auto manifest = LoadManifest(request);
    if (!manifest.has_value())
    {
        if (auto resolved = ResolveArtifactPath(request, request.artifactPath);
            resolved.has_value() &&
            NLS::Render::Assets::ReadMeshArtifactHeaderPreview(*resolved, 64u * 1024u).has_value())
        {
            paths.push_back(*resolved);
        }
        return paths;
    }

    const bool requestTargetsSingleMesh =
        !request.subAssetKey.empty() &&
        request.subAssetKey.rfind("mesh:", 0u) == 0u;
    if (requestTargetsSingleMesh)
    {
        for (const auto& artifact : manifest->subAssets)
        {
            if (artifact.artifactType != NLS::Core::Assets::ArtifactType::Mesh ||
                artifact.subAssetKey != request.subAssetKey)
            {
                continue;
            }

            if (auto resolved = ResolveArtifactPath(request, artifact.artifactPath);
                resolved.has_value())
            {
                paths.push_back(*resolved);
            }
            return paths;
        }
    }

    for (const auto& artifact : manifest->subAssets)
    {
        if (artifact.artifactType != NLS::Core::Assets::ArtifactType::Mesh)
            continue;

        if (auto resolved = ResolveArtifactPath(request, artifact.artifactPath);
            resolved.has_value())
        {
            paths.push_back(*resolved);
        }
    }
    return paths;
}

std::vector<std::filesystem::path> ResolveMaterialArtifactPaths(const AssetThumbnailRequest& request)
{
    std::vector<std::filesystem::path> paths;
    const auto manifest = LoadManifest(request);
    if (!manifest.has_value())
    {
        if (auto resolved = ResolveArtifactPath(request, request.artifactPath);
            resolved.has_value())
        {
            const auto prefix = NLS::Core::Assets::ReadNativeArtifactPayloadPrefixFromFile(
                *resolved,
                NLS::Core::Assets::ArtifactType::Material,
                1u,
                0u,
                64u * 1024u);
            if (prefix.has_value())
                paths.push_back(*resolved);
        }
        return paths;
    }

    if (!request.subAssetKey.empty())
    {
        for (const auto& artifact : manifest->subAssets)
        {
            if (artifact.artifactType != NLS::Core::Assets::ArtifactType::Material ||
                artifact.subAssetKey != request.subAssetKey)
            {
                continue;
            }

            if (auto resolved = ResolveArtifactPath(request, artifact.artifactPath);
                resolved.has_value())
            {
                paths.push_back(*resolved);
            }
            return paths;
        }
    }

    for (const auto& artifact : manifest->subAssets)
    {
        if (artifact.artifactType != NLS::Core::Assets::ArtifactType::Material)
            continue;

        if (auto resolved = ResolveArtifactPath(request, artifact.artifactPath);
            resolved.has_value())
        {
            paths.push_back(*resolved);
        }
    }
    return paths;
}

std::optional<std::filesystem::path> ResolvePrefabArtifactPath(const AssetThumbnailRequest& request)
{
    const auto manifest = LoadManifest(request);
    if (manifest.has_value())
    {
        if (!request.subAssetKey.empty())
        {
            for (const auto& artifact : manifest->subAssets)
            {
                if (artifact.artifactType == NLS::Core::Assets::ArtifactType::Prefab &&
                    artifact.subAssetKey == request.subAssetKey)
                {
                    return ResolveArtifactPath(request, artifact.artifactPath);
                }
            }
        }

        for (const auto& artifact : manifest->subAssets)
        {
            if (artifact.artifactType == NLS::Core::Assets::ArtifactType::Prefab)
                return ResolveArtifactPath(request, artifact.artifactPath);
        }
    }

    return ResolveArtifactPath(request, request.artifactPath);
}

std::optional<std::filesystem::path> ResolvePrefabArtifactPathByIdentity(const AssetThumbnailRequest& request)
{
    if (!request.assetId.IsValid() || request.subAssetKey.empty())
        return std::nullopt;

    const auto manifest = LoadManifest(request);
    if (!manifest.has_value() || !manifest->sourceAssetId.IsValid())
        return std::nullopt;

    const auto* prefabArtifact = manifest->FindSubAsset(request.subAssetKey);
    if (prefabArtifact == nullptr ||
        prefabArtifact->artifactType != NLS::Core::Assets::ArtifactType::Prefab)
    {
        return std::nullopt;
    }

    const auto rawPath = NLS::Core::Assets::NormalizeAssetPath(prefabArtifact->artifactPath);
    if (rawPath.empty())
        return std::nullopt;

    for (const auto& root : MakeProjectEditorAssetRoots(request.projectRoot))
    {
        const auto artifactRoot =
            GetEditorAssetRootLibraryPath(root) / "Artifacts" / request.assetId.ToString();

        const auto resolvedInArtifactRoot = NLS::Core::Assets::NormalizeAssetPath(artifactRoot / rawPath);
        if (!resolvedInArtifactRoot.empty() &&
            IsPhysicalRegularFileInsideEditorAssetRoot(resolvedInArtifactRoot, artifactRoot))
        {
            return resolvedInArtifactRoot;
        }

        if (!rawPath.is_absolute())
        {
            const auto resolvedFromProjectRoot = NLS::Core::Assets::NormalizeAssetPath(
                GetEditorAssetRootLibraryPath(root).parent_path() / rawPath);
            if (!resolvedFromProjectRoot.empty() &&
                IsPhysicalRegularFileInsideEditorAssetRoot(resolvedFromProjectRoot, artifactRoot))
            {
                return resolvedFromProjectRoot;
            }
        }
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> ResolvePrefabPreviewBudgetPath(const AssetThumbnailRequest& request)
{
    if (auto resolved = ResolvePrefabArtifactPath(request);
        resolved.has_value())
    {
        return resolved;
    }
    return ResolvePrefabArtifactPathByIdentity(request);
}

uint64_t FileSizeOrMax(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? std::numeric_limits<uint64_t>::max() : size;
}

bool NativeArtifactPayloadExceedsGpuPreviewBudget(
    const std::filesystem::path& path,
    const NLS::Core::Assets::ArtifactType type,
    const uint32_t schemaVersion,
    const size_t maxPayloadBytes = kMaxGpuPreviewStructurePayloadBytes)
{
    const auto fileSize = FileSizeOrMax(path);
    if (fileSize > kMaxGpuPreviewNativeArtifactFileBytes)
        return true;

    const auto prefix = NLS::Core::Assets::ReadNativeArtifactPayloadPrefixFromFile(
        path,
        type,
        schemaVersion,
        1u,
        maxPayloadBytes);
    if (prefix.has_value())
    {
        return prefix->payloadSize > maxPayloadBytes ||
            prefix->payloadOffset > fileSize ||
            prefix->payloadSize != fileSize - prefix->payloadOffset;
    }

    return fileSize > maxPayloadBytes;
}

bool MeshArtifactExceedsGpuPreviewBudget(const std::filesystem::path& path)
{
    if (FileSizeOrMax(path) > kMaxGpuPreviewNativeArtifactFileBytes)
        return true;

    const auto header = NLS::Render::Assets::ReadMeshArtifactHeaderPreview(
        path,
        kMaxGpuPreviewStructurePayloadBytes);
    return !header.has_value() ||
        header->vertexCount > kMaxGpuPreviewMeshVertices ||
        header->indexCount > kMaxGpuPreviewMeshIndices;
}

bool MaterialArtifactExceedsGpuPreviewBudget(const std::filesystem::path& path)
{
    return NativeArtifactPayloadExceedsGpuPreviewBudget(
        path,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
}

bool PrefabArtifactExceedsGpuPreviewBudget(const std::filesystem::path& path)
{
    return NativeArtifactPayloadExceedsGpuPreviewBudget(
        path,
        NLS::Core::Assets::ArtifactType::Prefab,
        1u,
        kMaxGpuPreviewPrefabPayloadBytes);
}

bool IsBuiltInPreviewResourcePath(const std::string& path)
{
    return !path.empty() && path.front() == ':';
}

NLS::Render::Resources::Material* ResolvePreviewMaterial(
    NLS::Core::ResourceManagement::MaterialManager& materialManager,
    const std::string& materialPath)
{
    if (materialPath.empty())
        return nullptr;

    if (auto* cached = materialManager.GetResource(materialPath, false))
        return cached;

    if (NLS::Core::Assets::IsContentStorageArtifactPath(materialPath))
        return materialManager.RequestAsyncArtifact(materialPath, true);

    return materialManager.GetResource(materialPath, true);
}

bool MaterialTextureDependenciesReady(
    NLS::Render::Resources::Material& material,
    const std::unordered_set<std::string>& activeTextureInterests,
    std::unordered_set<std::string>* requestedTexturePaths)
{
    const auto& texturePaths = material.GetTextureResourcePaths();
    if (texturePaths.empty())
        return true;
    if (!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::TextureManager>())
        return false;

    bool ready = true;
    auto& textureManager = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager);
    const auto& uniforms = material.GetUniformsData();
    for (const auto& [name, path] : texturePaths)
    {
        if (path.empty())
            continue;

        const auto uniform = uniforms.find(name);
        NLS::Render::Resources::Texture2D* texture = nullptr;
        if (uniform != uniforms.end())
        {
            if (const auto* value = std::any_cast<NLS::Render::Resources::Texture2D*>(&uniform->second);
                value != nullptr)
            {
                texture = *value;
            }
        }
        if (texture != nullptr && texture->GetTextureHandle() != nullptr)
            continue;

        const auto genericPath = ToGenericPath(path);
        if (requestedTexturePaths != nullptr && !genericPath.empty())
            requestedTexturePaths->insert(genericPath);
        if (activeTextureInterests.find(genericPath) == activeTextureInterests.end())
            (void)textureManager.RequestAsyncArtifact(path, true);
        ready = false;
    }
    return ready;
}

NLS::Render::Resources::Mesh* ResolvePreviewMesh(
    NLS::Core::ResourceManagement::MeshManager& meshManager,
    const std::string& meshPath)
{
    if (meshPath.empty())
        return nullptr;

    if (auto* cached = meshManager.GetResource(meshPath, false))
        return cached;

    const auto extension = std::filesystem::path(meshPath).extension().string();
    if (extension == ".nmesh")
        return meshManager.RequestAsyncArtifact(meshPath, true);

    return meshManager.GetResource(meshPath, true);
}

std::optional<std::filesystem::path> ResolvePrefabPreviewMaterialBudgetPath(
    const AssetThumbnailRequest& request,
    const std::string& materialPath)
{
    if (materialPath.empty() || IsBuiltInPreviewResourcePath(materialPath))
        return std::nullopt;

    if (auto resolved = ResolveArtifactPath(request, materialPath);
        resolved.has_value())
    {
        return resolved;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> ResolvePrefabPreviewMeshBudgetPath(
    const AssetThumbnailRequest& request,
    const std::string& meshPath)
{
    if (meshPath.empty() || IsBuiltInPreviewResourcePath(meshPath))
        return std::nullopt;
    return ResolveArtifactPath(request, meshPath);
}

std::optional<std::filesystem::path> ResolvePrefabDependencyArtifactBudgetPath(
    const AssetThumbnailRequest& request,
    const NLS::Core::Assets::AssetId& assetId,
    const std::string& artifactPath)
{
    if (artifactPath.empty() || !assetId.IsValid() || IsBuiltInPreviewResourcePath(artifactPath))
        return std::nullopt;

    const auto rawPath = std::filesystem::path(artifactPath).lexically_normal();
    for (const auto& root : MakeProjectEditorAssetRoots(request.projectRoot))
    {
        const auto artifactRoot = GetEditorAssetRootLibraryPath(root) / "Artifacts" / assetId.ToString();
        const auto candidate = rawPath.is_absolute()
            ? rawPath
            : artifactRoot / rawPath.filename();
        const auto normalized = NLS::Core::Assets::NormalizeAssetPath(candidate);
        if (!normalized.empty() &&
            IsPhysicalRegularFileInsideEditorAssetRoot(normalized, artifactRoot))
        {
            return normalized;
        }
    }
    return std::nullopt;
}

struct Bounds
{
    Maths::Vector3 min {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()
    };
    Maths::Vector3 max {
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()
    };
    bool valid = false;
};

void IncludeBounds(Bounds& target, const Render::Geometry::Bounds& bounds)
{
    const auto halfSize = bounds.size * 0.5f;
    const auto min = bounds.center - halfSize;
    const auto max = bounds.center + halfSize;
    target.min.x = (std::min)(target.min.x, min.x);
    target.min.y = (std::min)(target.min.y, min.y);
    target.min.z = (std::min)(target.min.z, min.z);
    target.max.x = (std::max)(target.max.x, max.x);
    target.max.y = (std::max)(target.max.y, max.y);
    target.max.z = (std::max)(target.max.z, max.z);
    target.valid = true;
}

std::array<Maths::Vector3, 8> BoundsCorners(const Render::Geometry::Bounds& bounds)
{
    const auto halfSize = bounds.size * 0.5f;
    const auto min = bounds.center - halfSize;
    const auto max = bounds.center + halfSize;
    return {
        Maths::Vector3(min.x, min.y, min.z),
        Maths::Vector3(max.x, min.y, min.z),
        Maths::Vector3(min.x, max.y, min.z),
        Maths::Vector3(max.x, max.y, min.z),
        Maths::Vector3(min.x, min.y, max.z),
        Maths::Vector3(max.x, min.y, max.z),
        Maths::Vector3(min.x, max.y, max.z),
        Maths::Vector3(max.x, max.y, max.z)
    };
}

void IncludePoint(Bounds& target, const Maths::Vector3& point)
{
    target.min.x = (std::min)(target.min.x, point.x);
    target.min.y = (std::min)(target.min.y, point.y);
    target.min.z = (std::min)(target.min.z, point.z);
    target.max.x = (std::max)(target.max.x, point.x);
    target.max.y = (std::max)(target.max.y, point.y);
    target.max.z = (std::max)(target.max.z, point.z);
    target.valid = true;
}

void IncludeWorldBounds(Bounds& target, const Render::Geometry::Bounds& bounds, const Maths::Matrix4& worldMatrix)
{
    for (const auto& corner : BoundsCorners(bounds))
        IncludePoint(target, worldMatrix * Maths::Vector4(corner.x, corner.y, corner.z, 1.0f));
}

Maths::Vector3 PreviewDirection(const float yawDegrees, const float pitchDegrees)
{
    const auto yaw = yawDegrees * kDegreesToRadians;
    const auto pitch = pitchDegrees * kDegreesToRadians;
    const auto cy = std::cos(yaw);
    const auto sy = std::sin(yaw);
    const auto cp = std::cos(pitch);
    const auto sp = std::sin(pitch);
    return Maths::Vector3(
        sy * cp,
        sp,
        cy * cp).Normalised();
}

Maths::Vector3 PreviewDirection()
{
    return PreviewDirection(kUnityMeshPreviewYawDegrees, kUnityMeshPreviewPitchDegrees);
}

struct PreviewCameraPlacement
{
    Maths::Vector3 center;
    Maths::Vector3 direction;
    float radius = 0.0f;
    float distance = 0.0f;
};

PreviewCameraPlacement BuildPrefabPreviewCameraPlacement(
    const Bounds& bounds,
    const uint32_t width,
    const uint32_t height)
{
    PreviewCameraPlacement placement;
    placement.center = (bounds.min + bounds.max) * 0.5f;
    placement.direction = PreviewDirection(kUnityPrefabPreviewYawDegrees, kUnityPrefabPreviewPitchDegrees);
    const Maths::Vector3 extents = (bounds.max - bounds.min) * 0.5f;
    placement.radius = (std::max)(0.001f, extents.Length());
    const auto fovRadians = kUnityMeshPreviewFieldOfViewDegrees * kDegreesToRadians;
    const auto aspect = height == 0u ? 1.0f : static_cast<float>(width) / static_cast<float>(height);
    const auto halfVerticalFov = fovRadians * 0.5f;
    const auto halfHorizontalFov = std::atan(std::tan(halfVerticalFov) * (std::max)(0.001f, aspect));
    const auto distanceVertical = placement.radius / std::tan(halfVerticalFov);
    const auto distanceHorizontal = placement.radius / std::tan(halfHorizontalFov);
    placement.distance = (std::max)(distanceVertical, distanceHorizontal) * 1.18f;
    return placement;
}

void ApplyPreviewMaterialStabilization(NLS::Render::Resources::Material& material)
{
    const auto& uniforms = material.GetUniformsData();
    if (uniforms.find("u_HeightScale") != uniforms.end())
        material.Set("u_HeightScale", 0.0f);
    if (uniforms.find("u_AmbientOcclusion") != uniforms.end())
        material.Set("u_AmbientOcclusion", 1.0f);
    if (uniforms.find("u_Roughness") != uniforms.end())
        material.Set("u_Roughness", 0.58f);
}

std::unique_ptr<NLS::Render::Resources::Material> CreateStablePreviewMaterial(
    NLS::Render::Resources::Material& source)
{
    auto material = std::make_unique<NLS::Render::Resources::Material>(source.GetShader());
    for (const auto& [name, value] : source.GetUniformsData())
        material->SetRawParameter(name, value);
    for (const auto& [name, path] : source.GetTextureResourcePaths())
        material->SetTextureResourcePath(name, path);
    material->SetSurfaceMode(source.GetSurfaceMode());
    material->SetBlendable(source.IsBlendable());
    material->SetBackfaceCulling(source.HasBackfaceCulling());
    material->SetFrontfaceCulling(source.HasFrontfaceCulling());
    material->SetDepthTest(source.HasDepthTest());
    material->SetDepthWriting(source.HasDepthWriting());
    material->SetColorWriting(source.HasColorWriting());
    ApplyPreviewMaterialStabilization(*material);
    return material;
}

bool PrefabArtifactExceedsGpuPreviewComplexityBudget(
    const NLS::Engine::Assets::PrefabArtifact& prefab)
{
    if (prefab.resolvedAssets.size() > kMaxGpuPreviewPrefabResolvedAssets ||
        prefab.graph.objects.size() > kMaxGpuPreviewPrefabGraphObjects)
    {
        return true;
    }

    size_t propertyCount = 0u;
    for (const auto& object : prefab.graph.objects)
    {
        propertyCount += object.properties.size();
        if (propertyCount > kMaxGpuPreviewPrefabGraphProperties)
            return true;
    }
    return false;
}

std::mutex& RetiredPreviewReadbackMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::deque<EditorThumbnailPreviewReadbackState>& RetiredPreviewReadbacks()
{
    static std::deque<EditorThumbnailPreviewReadbackState> readbacks;
    return readbacks;
}

void PruneGlobalRetiredPreviewReadbacks()
{
    std::lock_guard lock(RetiredPreviewReadbackMutex());
    auto& readbacks = RetiredPreviewReadbacks();
    for (auto iterator = readbacks.begin(); iterator != readbacks.end();)
    {
        if (iterator->completion == nullptr || iterator->completion->Poll().IsComplete())
            iterator = readbacks.erase(iterator);
        else
            ++iterator;
    }
}

void RetirePreviewReadback(EditorThumbnailPreviewReadbackState&& readback)
{
    if (!readback.active || readback.completion == nullptr || readback.rgbaPixels == nullptr)
        return;

    std::lock_guard lock(RetiredPreviewReadbackMutex());
    auto& readbacks = RetiredPreviewReadbacks();
    for (auto iterator = readbacks.begin(); iterator != readbacks.end();)
    {
        if (iterator->completion == nullptr || iterator->completion->Poll().IsComplete())
            iterator = readbacks.erase(iterator);
        else
            ++iterator;
    }
    while (readbacks.size() >= kMaxRetiredPreviewReadbacks)
        readbacks.pop_front();
    readbacks.push_back(std::move(readback));
}
}

#if defined(NLS_ENABLE_TEST_HOOKS)
EditorThumbnailPreviewCameraDebugInfo BuildPrefabPreviewCameraDebugInfoForTesting(
    const NLS::Maths::Vector3& boundsMin,
    const NLS::Maths::Vector3& boundsMax,
    const uint32_t width,
    const uint32_t height)
{
    Bounds bounds;
    bounds.min = boundsMin;
    bounds.max = boundsMax;
    bounds.valid = true;

    const auto placement = BuildPrefabPreviewCameraPlacement(bounds, width, height);
    EditorThumbnailPreviewCameraDebugInfo info;
    info.cameraPosition = placement.center - placement.direction * placement.distance;
    info.lookDirection = placement.direction;
    info.distance = placement.distance;
    return info;
}

NLS::Maths::Vector3 GetThumbnailPreviewKeyLightDirectionForTesting()
{
    return kThumbnailPreviewKeyLightDirection.Normalised();
}

float GetThumbnailPreviewKeyLightIntensityForTesting()
{
    return kThumbnailPreviewKeyLightIntensity;
}

float GetThumbnailPreviewAmbientIntensityForTesting()
{
    return kThumbnailPreviewAmbientIntensity;
}

size_t GetThumbnailPreviewMeshPumpBudgetForTesting()
{
    return kThumbnailPreviewMeshPumpCompletionsPerFrame;
}

size_t GetThumbnailPreviewMaterialPumpBudgetForTesting()
{
    return kThumbnailPreviewMaterialPumpCompletionsPerFrame;
}

size_t GetThumbnailPreviewTexturePumpBudgetForTesting()
{
    return kThumbnailPreviewTexturePumpCompletionsPerFrame;
}
#endif

class EditorThumbnailPreviewRenderer::Impl
{
public:
    explicit Impl(NLS::Render::Context::Driver& driver)
        : m_driver(driver)
        , m_renderer(NLS::Engine::Rendering::CreateSceneRenderer(driver))
    {
        auto& cameraObject = m_scene.CreateEditorTransientGameObject("Thumbnail Preview Camera");
        m_camera = cameraObject.AddComponent<NLS::Engine::Components::CameraComponent>();
        m_camera->SetFov(kUnityMeshPreviewFieldOfViewDegrees);
        m_camera->SetClearColor({0.0f, 0.0f, 0.0f});
        m_camera->SetFrustumGeometryCulling(false);
        m_camera->SetFrustumLightCulling(false);

        auto& directional = m_scene.CreateEditorTransientGameObject("Thumbnail Preview Key Light");
        directional.GetTransform()->SetLocalRotation(
            Maths::Quaternion::LookAt(kThumbnailPreviewKeyLightDirection, Maths::Vector3::Up));
        auto* light = directional.AddComponent<NLS::Engine::Components::LightComponent>();
        light->SetLightType(NLS::Render::Settings::ELightType::DIRECTIONAL);
        light->SetIntensity(kThumbnailPreviewKeyLightIntensity);

        auto& ambient = m_scene.CreateEditorTransientGameObject("Thumbnail Preview Ambient");
        auto* ambientLight = ambient.AddComponent<NLS::Engine::Components::LightComponent>();
        ambientLight->SetLightType(NLS::Render::Settings::ELightType::AMBIENT_SPHERE);
        ambientLight->SetRadius(10000.0f);
        ambientLight->SetIntensity(kThumbnailPreviewAmbientIntensity);

        m_framebuffer.SetOptimizedColorClearValue(0.0f, 0.0f, 0.0f, 0.0f);
    }

    ~Impl()
    {
        NLS::Render::Context::DriverRendererAccess::DrainThreadedRendering(m_driver);
        RetirePendingReadback();
        ReleaseTextureInterests();
        ClearPreviewObjects();
        PruneGlobalRetiredPreviewReadbacks();
    }

    bool Supports(const AssetThumbnailRequest& request) const
    {
        return request.kind == AssetThumbnailKind::MaterialSphere ||
            request.kind == AssetThumbnailKind::ModelPreview ||
            request.kind == AssetThumbnailKind::PrefabPreview;
    }

    PreviewResourcePathSet CollectRequestedPreviewResourcePaths(const AssetThumbnailRequest& request)
    {
        PreviewResourcePathSet paths;
        if (request.kind == AssetThumbnailKind::MaterialSphere)
        {
            for (const auto& materialPath : ResolveMaterialArtifactPaths(request))
                paths.materialPaths.insert(ToGenericPath(materialPath));
            return paths;
        }

        if (request.kind == AssetThumbnailKind::ModelPreview)
        {
            for (const auto& meshPath : ResolveMeshArtifactPaths(request))
                paths.meshPaths.insert(ToGenericPath(meshPath));
            return paths;
        }

        if (request.kind != AssetThumbnailKind::PrefabPreview ||
            !request.assetId.IsValid() ||
            request.subAssetKey.empty())
        {
            return paths;
        }

        const auto snapshotCacheKey = BuildPreviewSnapshotCacheKey(request);
        PreviewRenderableSnapshot snapshot;
        if (!TryGetPreviewSnapshotFromCache(snapshotCacheKey, snapshot))
            return paths;

        for (const auto& drawItem : snapshot.drawItems)
        {
            const auto meshPath = ToGenericPath(drawItem.meshPath);
            if (!meshPath.empty())
                paths.meshPaths.insert(meshPath);
            for (const auto& materialPath : drawItem.materialPaths)
            {
                const auto genericMaterialPath = ToGenericPath(materialPath);
                if (!genericMaterialPath.empty())
                    paths.materialPaths.insert(genericMaterialPath);
            }
        }
        return paths;
    }

    EditorThumbnailPreviewResult Render(const AssetThumbnailRequest& request)
    {
        EditorThumbnailPreviewResult result;
        result.width = std::max(1u, request.requestedSize);
        result.height = result.width;
        PruneGlobalRetiredPreviewReadbacks();
        const auto readbackRequestKey = BuildPreviewReadbackRequestKey(request);
        m_activeRequestKey = readbackRequestKey;
        if (!m_textureInterestRequestKey.empty() && m_textureInterestRequestKey != readbackRequestKey)
            ReleaseTextureInterests();
        if (m_pendingReadback.active)
        {
            if (m_pendingReadback.requestKey != readbackRequestKey)
            {
                RetirePendingReadback();
                ReleaseTextureInterests();
                ClearPreviewObjects();
            }
            else
            {
                const auto polled = PollEditorThumbnailPreviewReadback(
                    m_pendingReadback,
                    readbackRequestKey,
                    &m_driver);
                if (polled.status == EditorThumbnailPreviewReadbackPollStatus::Pending)
                {
                    result.diagnostic = "thumbnail-gpu-preview-readback-pending";
                    return result;
                }
                if (polled.status == EditorThumbnailPreviewReadbackPollStatus::Ready)
                {
                    ReleaseTextureInterests();
                    ClearPreviewObjects();
                    return polled.preview;
                }
                if (polled.status == EditorThumbnailPreviewReadbackPollStatus::Failed ||
                    polled.status == EditorThumbnailPreviewReadbackPollStatus::DeviceLost)
                {
                    ReleaseTextureInterests();
                    ClearPreviewObjects();
                    result.diagnostic = "thumbnail-gpu-preview-readback-failed:" + polled.preview.diagnostic;
                    return result;
                }
            }
        }
        if (!Supports(request))
        {
            ReleaseTextureInterests();
            result.diagnostic = "thumbnail-gpu-preview-kind-unsupported";
            return result;
        }
        if (m_renderer == nullptr || m_camera == nullptr || m_camera->GetCamera() == nullptr)
        {
            ReleaseTextureInterests();
            result.diagnostic = "thumbnail-gpu-preview-renderer-unavailable";
            return result;
        }
        if (!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MeshManager>() ||
            !NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MaterialManager>())
        {
            ReleaseTextureInterests();
            result.diagnostic = "thumbnail-gpu-preview-resource-managers-unavailable";
            return result;
        }

        auto& previewMeshManager = NLS_SERVICE(NLS::Core::ResourceManagement::MeshManager);
        auto& previewMaterialManager = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager);
        auto* previewTextureManager = NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::TextureManager>()
            ? &NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager)
            : nullptr;
        const auto requestedResourcePaths = CollectRequestedPreviewResourcePaths(request);
        previewMeshManager.PumpAsyncLoadsForPaths(
            requestedResourcePaths.meshPaths,
            kThumbnailPreviewMeshPumpCompletionsPerFrame);
        previewMaterialManager.PumpAsyncLoadsForPaths(
            requestedResourcePaths.materialPaths,
            kThumbnailPreviewMaterialPumpCompletionsPerFrame);
        if (previewTextureManager != nullptr)
        {
            auto texturePumpPaths = requestedResourcePaths.texturePaths;
            texturePumpPaths.insert(m_textureInterestPaths.begin(), m_textureInterestPaths.end());
            previewTextureManager->PumpAsyncLoadsForPaths(
                texturePumpPaths,
                kThumbnailPreviewTexturePumpCompletionsPerFrame);
        }

        ClearPreviewObjects();

        if (request.kind == AssetThumbnailKind::MaterialSphere)
            return RenderMaterialSphere(request, result);

        if (request.kind == AssetThumbnailKind::PrefabPreview)
        {
            const auto prefabPath = ResolvePrefabPreviewBudgetPath(request);
            if (prefabPath.has_value() && PrefabArtifactExceedsGpuPreviewBudget(*prefabPath))
            {
                result.diagnostic = kGpuPreviewPrefabBudgetExceededDiagnostic;
                return result;
            }
            if (RenderPrefabPreview(request, result) ||
                result.diagnostic == "thumbnail-gpu-preview-readback-pending" ||
                result.diagnostic == "thumbnail-gpu-preview-resources-pending")
            {
                return result;
            }
            ClearPreviewObjects();
            return result;
        }

        const auto meshPaths = ResolveMeshArtifactPaths(request);
        if (meshPaths.empty())
        {
            result.diagnostic = "thumbnail-gpu-preview-mesh-artifact-missing";
            return result;
        }

        std::vector<NLS::Render::Resources::Material*> materials;
        Bounds combinedBounds;
        {
            NLS::Base::Profiling::PerformanceStageScope resourcesScope(
                NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
                "PreparePreviewResources",
                NLS::Base::Profiling::PerformanceStageThread::Main);
            resourcesScope.AddCounter("dependencyResourceCount", meshPaths.size());

            auto& meshManager = NLS_SERVICE(NLS::Core::ResourceManagement::MeshManager);
            if (request.kind == AssetThumbnailKind::PrefabPreview)
            {
                auto& materialManager = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager);
                const auto materialPaths = ResolveMaterialArtifactPaths(request);
                resourcesScope.AddCounter("dependencyResourceCount", materialPaths.size());
                for (const auto& materialPath : materialPaths)
                {
                    if (MaterialArtifactExceedsGpuPreviewBudget(materialPath))
                    {
                        result.diagnostic = kGpuPreviewMaterialBudgetExceededDiagnostic;
                        return result;
                    }
                    materials.push_back(materialManager.GetResource(ToGenericPath(materialPath), false));
                }
            }

            for (const auto& meshPath : meshPaths)
            {
                if (MeshArtifactExceedsGpuPreviewBudget(meshPath))
                {
                    result.diagnostic = kGpuPreviewMeshBudgetExceededDiagnostic;
                    ClearPreviewObjects();
                    return result;
                }
                auto* mesh = meshManager.GetResource(ToGenericPath(meshPath), false);
                if (mesh == nullptr)
                {
                    result.diagnostic = "thumbnail-gpu-preview-resources-pending";
                    ClearPreviewObjects();
                    return result;
                }

                auto& object = m_scene.CreateEditorTransientGameObject("Thumbnail Preview Mesh");
                m_previewObjects.push_back(&object);
                auto* filter = object.AddComponent<NLS::Engine::Components::MeshFilter>();
                auto* renderer = object.AddComponent<NLS::Engine::Components::MeshRenderer>();
                filter->SetMesh(mesh);
                renderer->SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::DISABLED);
                if (materials.empty())
                {
                    renderer->FillEmptySlotsWithMaterial(DefaultMaterial());
                }
                else
                {
                    renderer->FillEmptySlotsWithMaterial(DefaultMaterial());
                    for (size_t slot = 0u; slot < materials.size() && slot < NLS::Engine::Components::MeshRenderer::kMaxMaterialCount; ++slot)
                    {
                        if (materials[slot] != nullptr)
                            renderer->SetMaterialAtIndex(static_cast<uint8_t>(slot), *materials[slot]);
                    }
                }
                IncludeBounds(combinedBounds, mesh->GetBounds());
            }
        }

        if (m_previewObjects.empty() || !combinedBounds.valid)
        {
            result.diagnostic = "thumbnail-gpu-preview-mesh-load-failed";
            ClearPreviewObjects();
            return result;
        }

        ConfigureCamera(combinedBounds, result.width, result.height);
        RenderCurrentPreviewScene(request, result);

        if (result.diagnostic != "thumbnail-gpu-preview-readback-pending")
            ClearPreviewObjects();
        return result;
    }

private:
    struct PreviewRenderInputsKeepAlive
    {
        NLS::Engine::SceneSystem::Scene* scene = nullptr;
        std::vector<NLS::Engine::GameObject*> objects;
        std::unique_ptr<NLS::Render::Resources::Material> material;

        ~PreviewRenderInputsKeepAlive()
        {
            if (scene == nullptr)
                return;
            for (auto* object : objects)
            {
                if (object != nullptr)
                    (void)scene->DestroyGameObject(*object);
            }
            scene->CollectGarbages();
        }
    };

    struct PreviewSnapshotCacheEntry
    {
        std::string key;
        PreviewRenderableSnapshot snapshot;
        uint64_t lastUsed = 0u;
    };

    bool TryGetPreviewSnapshotFromCache(
        const std::string& key,
        PreviewRenderableSnapshot& snapshot)
    {
        ++m_previewSnapshotCacheClock;
        for (auto& entry : m_previewSnapshotCache)
        {
            if (entry.key != key)
                continue;

            entry.lastUsed = m_previewSnapshotCacheClock;
            snapshot = entry.snapshot;
            return true;
        }
        return false;
    }

    void StorePreviewSnapshotInCache(
        std::string key,
        const PreviewRenderableSnapshot& snapshot)
    {
        if (key.empty() || snapshot.drawItems.empty())
            return;

        ++m_previewSnapshotCacheClock;
        for (auto& entry : m_previewSnapshotCache)
        {
            if (entry.key != key)
                continue;

            entry.snapshot = snapshot;
            entry.lastUsed = m_previewSnapshotCacheClock;
            return;
        }

        if (m_previewSnapshotCache.size() >= kMaxPreviewRenderableSnapshotCacheEntries)
        {
            auto oldest = std::min_element(
                m_previewSnapshotCache.begin(),
                m_previewSnapshotCache.end(),
                [](const PreviewSnapshotCacheEntry& left, const PreviewSnapshotCacheEntry& right)
                {
                    return left.lastUsed < right.lastUsed;
                });
            if (oldest != m_previewSnapshotCache.end())
                m_previewSnapshotCache.erase(oldest);
        }

        m_previewSnapshotCache.push_back({
            std::move(key),
            snapshot,
            m_previewSnapshotCacheClock
        });
    }

    EditorThumbnailPreviewResult RenderMaterialSphere(
        const AssetThumbnailRequest& request,
        EditorThumbnailPreviewResult result)
    {
        const auto materialPaths = ResolveMaterialArtifactPaths(request);
        if (materialPaths.empty())
        {
            result.diagnostic = "thumbnail-gpu-preview-material-artifact-missing";
            return result;
        }
        if (MaterialArtifactExceedsGpuPreviewBudget(materialPaths.front()))
        {
            result.diagnostic = kGpuPreviewMaterialBudgetExceededDiagnostic;
            return result;
        }

        auto& materialManager = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager);
        NLS::Render::Resources::Material* material = nullptr;
        {
            NLS::Base::Profiling::PerformanceStageScope resourcesScope(
                NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
                "PreparePreviewResources",
            NLS::Base::Profiling::PerformanceStageThread::Main);
            resourcesScope.AddCounter("dependencyResourceCount", materialPaths.size());
            material = ResolvePreviewMaterial(materialManager, ToGenericPath(materialPaths.front()));
        }
        if (material == nullptr)
        {
            result.diagnostic = "thumbnail-gpu-preview-resources-pending";
            return result;
        }
        std::unordered_set<std::string> requestedTexturePaths;
        if (!MaterialTextureDependenciesReady(*material, m_textureInterestPaths, &requestedTexturePaths))
        {
            TrackRequestedTextureInterests(requestedTexturePaths);
            result.diagnostic = "thumbnail-gpu-preview-resources-pending";
            return result;
        }
        TrackRequestedTextureInterests(requestedTexturePaths);

        if (!EDITOR_CONTEXT(editorResources))
        {
            result.diagnostic = "thumbnail-gpu-preview-editor-resources-unavailable";
            return result;
        }

        auto* sphere = EDITOR_CONTEXT(editorResources)->GetMesh("Sphere");
        if (sphere == nullptr)
        {
            result.diagnostic = "thumbnail-gpu-preview-material-sphere-missing";
            return result;
        }

        m_materialPreviewMaterial = CreateStablePreviewMaterial(*material);

        auto& object = m_scene.CreateEditorTransientGameObject("Thumbnail Preview Material Sphere");
        m_previewObjects.push_back(&object);
        auto* filter = object.AddComponent<NLS::Engine::Components::MeshFilter>();
        auto* renderer = object.AddComponent<NLS::Engine::Components::MeshRenderer>();
        filter->SetMesh(sphere);
        renderer->SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::DISABLED);
        renderer->FillEmptySlotsWithMaterial(DefaultMaterial());
        renderer->SetMaterialAtIndex(0u, *m_materialPreviewMaterial);

        ConfigureMaterialCamera(result.width, result.height);
        RenderCurrentPreviewScene(request, result);
        if (result.diagnostic != "thumbnail-gpu-preview-readback-pending")
            ClearPreviewObjects();
        return result;
    }

    bool RenderPrefabPreview(
        const AssetThumbnailRequest& request,
        EditorThumbnailPreviewResult& result)
    {
        NLS::Base::Profiling::PerformanceStageScope createInstanceScope(
            NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
            "CreatePreviewInstance",
            NLS::Base::Profiling::PerformanceStageThread::Main);

        if (request.assetId.IsValid() == false || request.subAssetKey.empty())
        {
            result.diagnostic = "thumbnail-gpu-preview-prefab-identity-missing";
            return false;
        }

        const auto snapshotCacheKey = BuildPreviewSnapshotCacheKey(request);
        PreviewRenderableSnapshot snapshot;
        const bool snapshotCacheHit = TryGetPreviewSnapshotFromCache(snapshotCacheKey, snapshot);
        if (!snapshotCacheHit)
        {
            AssetDatabaseFacade database(MakeProjectEditorAssetRoots(request.projectRoot));
            auto prefab = database.LoadPrefabArtifactByAssetId(request.assetId, request.subAssetKey);
            if (!prefab.has_value())
            {
                result.diagnostic = "thumbnail-gpu-preview-prefab-load-failed";
                return false;
            }
            if (PrefabArtifactExceedsGpuPreviewComplexityBudget(*prefab))
            {
                result.diagnostic = kGpuPreviewPrefabBudgetExceededDiagnostic;
                return false;
            }

            snapshot = BuildPreviewRenderableSnapshot(*prefab);
        }
        if (snapshot.drawItems.empty())
        {
            result.diagnostic = "thumbnail-gpu-preview-prefab-renderer-missing";
            return false;
        }
        if (snapshot.drawItems.size() > kMaxGpuPreviewPrefabDrawItems)
        {
            result.diagnostic = kGpuPreviewPrefabBudgetExceededDiagnostic;
            return false;
        }
        if (!snapshotCacheHit)
            StorePreviewSnapshotInCache(snapshotCacheKey, snapshot);

        struct ResolvedPreviewDrawItem
        {
            const PreviewDrawItem* drawItem = nullptr;
            NLS::Render::Resources::Mesh* mesh = nullptr;
            std::vector<NLS::Render::Resources::Material*> materials;
        };

        Bounds combinedBounds;
        std::vector<ResolvedPreviewDrawItem> resolvedDrawItems;
        resolvedDrawItems.reserve(snapshot.drawItems.size());
        bool resourcesPending = false;
        size_t pendingResourceCount = 0u;
        {
            NLS::Base::Profiling::PerformanceStageScope resourcesScope(
                NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
                "PreparePreviewResources",
                NLS::Base::Profiling::PerformanceStageThread::Main);
            resourcesScope.AddCounter("dependencyResourceCount", snapshot.drawItems.size());

            auto& meshManager = NLS_SERVICE(NLS::Core::ResourceManagement::MeshManager);
            auto& materialManager = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager);
            for (const auto& drawItem : snapshot.drawItems)
            {
                ResolvedPreviewDrawItem resolved;
                resolved.drawItem = &drawItem;

                const auto genericMeshPath = ToGenericPath(drawItem.meshPath);
                auto meshBudgetPath = ResolvePrefabPreviewMeshBudgetPath(request, genericMeshPath);
                if (!meshBudgetPath.has_value())
                    meshBudgetPath = ResolvePrefabDependencyArtifactBudgetPath(
                        request,
                        drawItem.meshAssetId,
                        genericMeshPath);
                if (meshBudgetPath.has_value() && MeshArtifactExceedsGpuPreviewBudget(*meshBudgetPath))
                {
                    result.diagnostic = kGpuPreviewMeshBudgetExceededDiagnostic;
                    ClearPreviewObjects();
                    return false;
                }

                auto* mesh = ResolvePreviewMesh(meshManager, genericMeshPath);
                if (mesh == nullptr)
                {
                    resourcesPending = true;
                    ++pendingResourceCount;
                }
                resolved.mesh = mesh;

                resolved.materials.reserve(drawItem.materialPaths.size());
                for (size_t materialIndex = 0u; materialIndex < drawItem.materialPaths.size(); ++materialIndex)
                {
                    const auto& materialPath = drawItem.materialPaths[materialIndex];
                    const auto genericMaterialPath = ToGenericPath(materialPath);
                    if (genericMaterialPath.empty())
                    {
                        resolved.materials.push_back(nullptr);
                        continue;
                    }

                    auto materialBudgetPath = ResolvePrefabPreviewMaterialBudgetPath(request, genericMaterialPath);
                    if (!materialBudgetPath.has_value() && materialIndex < drawItem.materialAssetIds.size())
                    {
                        materialBudgetPath = ResolvePrefabDependencyArtifactBudgetPath(
                            request,
                            drawItem.materialAssetIds[materialIndex],
                            genericMaterialPath);
                    }
                    if (materialBudgetPath.has_value() && MaterialArtifactExceedsGpuPreviewBudget(*materialBudgetPath))
                    {
                        result.diagnostic = kGpuPreviewMaterialBudgetExceededDiagnostic;
                        ClearPreviewObjects();
                        return false;
                    }

                    auto* material = ResolvePreviewMaterial(materialManager, genericMaterialPath);
                    if (material == nullptr)
                    {
                        resourcesPending = true;
                        ++pendingResourceCount;
                    }
                    else
                    {
                        std::unordered_set<std::string> requestedTexturePaths;
                        if (!MaterialTextureDependenciesReady(*material, m_textureInterestPaths, &requestedTexturePaths))
                        {
                            TrackRequestedTextureInterests(requestedTexturePaths);
                            resourcesPending = true;
                            ++pendingResourceCount;
                        }
                        else
                        {
                            TrackRequestedTextureInterests(requestedTexturePaths);
                        }
                    }
                    resolved.materials.push_back(material);
                }

                resolvedDrawItems.push_back(std::move(resolved));
            }
            resourcesScope.AddCounter("pendingResourceCount", pendingResourceCount);
        }

        if (resourcesPending)
        {
            result.diagnostic = "thumbnail-gpu-preview-resources-pending";
            ClearPreviewObjects();
            return false;
        }

        for (const auto& resolved : resolvedDrawItems)
        {
            if (resolved.drawItem == nullptr || resolved.mesh == nullptr)
                continue;

            auto& object = m_scene.CreateEditorTransientGameObject("Thumbnail Preview Prefab Draw Item");
            m_previewObjects.push_back(&object);
            object.GetTransform()->SetLocalPosition(resolved.drawItem->localPosition);
            object.GetTransform()->SetLocalRotation(resolved.drawItem->localRotation);
            object.GetTransform()->SetLocalScale(resolved.drawItem->localScale);
            auto* filter = object.AddComponent<NLS::Engine::Components::MeshFilter>();
            auto* renderer = object.AddComponent<NLS::Engine::Components::MeshRenderer>();
            filter->SetMesh(resolved.mesh);
            renderer->SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::DISABLED);
            renderer->FillEmptySlotsWithMaterial(DefaultMaterial());
            for (size_t slot = 0u;
                slot < resolved.materials.size() &&
                slot < NLS::Engine::Components::MeshRenderer::kMaxMaterialCount;
                ++slot)
            {
                if (resolved.materials[slot] != nullptr)
                    renderer->SetMaterialAtIndex(static_cast<uint8_t>(slot), *resolved.materials[slot]);
            }

            IncludeWorldBounds(combinedBounds, resolved.mesh->GetBounds(), object.GetTransform()->GetWorldMatrix());
        }

        if (!combinedBounds.valid)
        {
            result.diagnostic = "thumbnail-gpu-preview-prefab-mesh-load-failed";
            ClearPreviewObjects();
            return false;
        }

        ConfigurePrefabCamera(combinedBounds, result.width, result.height);
        RenderCurrentPreviewScene(request, result);
        if (result.diagnostic != "thumbnail-gpu-preview-readback-pending")
            ClearPreviewObjects();
        return !result.rgbaPixels.empty();
    }

    void RenderCurrentPreviewScene(
        const AssetThumbnailRequest& request,
        EditorThumbnailPreviewResult& result)
    {
        NLS::Base::Profiling::PerformanceStageScope recordScope(
            NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
            "RecordPreviewRender",
            NLS::Base::Profiling::PerformanceStageThread::Main);

        m_framebuffer.Resize(
            static_cast<uint16_t>(result.width),
            static_cast<uint16_t>(result.height));

        m_renderer->AddDescriptor<NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor>({
            m_scene,
            std::nullopt,
            nullptr,
            {},
            false,
            true
        });

        NLS::Render::Data::FrameDescriptor frameDescriptor;
        frameDescriptor.renderWidth = static_cast<uint16_t>(result.width);
        frameDescriptor.renderHeight = static_cast<uint16_t>(result.height);
        frameDescriptor.camera = m_camera->GetCamera();
        NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &m_framebuffer);

        m_renderer->BeginFrame(frameDescriptor);
        m_renderer->DrawFrame();
        {
            NLS::Base::Profiling::PerformanceStageScope submitScope(
                NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
                "SubmitPreviewRender",
                NLS::Base::Profiling::PerformanceStageThread::Main);
            m_renderer->EndFrame();
        }
        if (!NLS::Render::Context::DriverRendererAccess::TryDrainThreadedRendering(m_driver, false))
        {
            result.diagnostic = "thumbnail-gpu-preview-readback-pending";
            return;
        }

        auto readbackTexture = m_framebuffer.GetExplicitTextureHandle();
        if (readbackTexture == nullptr)
            readbackTexture = NLS::Render::Context::DriverRendererAccess::ResolveReadbackTexture(m_driver);
        if (readbackTexture == nullptr)
        {
            result.diagnostic = "thumbnail-gpu-preview-readback-texture-unavailable";
            return;
        }

        const auto readbackRequestKey = BuildPreviewReadbackRequestKey(request);
        auto readbackPixels = std::make_shared<std::vector<uint8_t>>(
            static_cast<size_t>(result.width) * result.height * 4u,
            0u);
        NLS::Render::RHI::RHIReadbackResult readback;
        {
            NLS::Base::Profiling::PerformanceStageScope readbackScope(
                NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
                "ReadbackPreview",
                NLS::Base::Profiling::PerformanceStageThread::Main);
            readback = NLS::Render::Context::DriverRendererAccess::BeginReadPixels(
                m_driver,
                readbackTexture,
                0u,
                0u,
                result.width,
                result.height,
                NLS::Render::Settings::EPixelDataFormat::RGBA,
                NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
                readbackPixels->data());
        }
        if (!readback.Succeeded())
        {
            result.rgbaPixels.clear();
            result.diagnostic = "thumbnail-gpu-preview-readback-failed:" + readback.message;
            return;
        }
        if (readback.completion == nullptr)
        {
            result.rgbaPixels = std::move(*readbackPixels);
            return;
        }

        EditorThumbnailPreviewReadbackState pendingReadback;
        pendingReadback.active = true;
        pendingReadback.requestKey = readbackRequestKey;
        pendingReadback.width = result.width;
        pendingReadback.height = result.height;
        pendingReadback.rgbaPixels = std::move(readbackPixels);
        pendingReadback.completion = readback.completion;
        pendingReadback.renderInputsKeepAlive = CapturePreviewRenderInputsKeepAlive();

        auto polled = PollEditorThumbnailPreviewReadback(pendingReadback, readbackRequestKey, &m_driver);
        if (polled.status == EditorThumbnailPreviewReadbackPollStatus::Ready)
        {
            result = std::move(polled.preview);
            return;
        }
        if (polled.status == EditorThumbnailPreviewReadbackPollStatus::Pending)
        {
            m_pendingReadback = std::move(pendingReadback);
            result.rgbaPixels.clear();
            result.diagnostic = "thumbnail-gpu-preview-readback-pending";
            return;
        }
        result.rgbaPixels.clear();
        result.diagnostic = "thumbnail-gpu-preview-readback-failed:" + polled.preview.diagnostic;
    }

    void ClearPreviewObjects()
    {
        NLS::Render::Context::DriverRendererAccess::DrainThreadedRendering(m_driver);
        for (auto* object : m_previewObjects)
        {
            if (object != nullptr)
                (void)m_scene.DestroyGameObject(*object);
        }
        m_previewObjects.clear();
        m_materialPreviewMaterial.reset();
        m_scene.CollectGarbages();
    }

    void TrackRequestedTextureInterests(const std::unordered_set<std::string>& requestedPaths)
    {
        if (requestedPaths.empty())
            return;
        m_textureInterestPaths.insert(requestedPaths.begin(), requestedPaths.end());
        m_textureInterestRequestKey = m_activeRequestKey;
    }

    void ReleaseTextureInterests()
    {
        if (m_textureInterestPaths.empty() ||
            !NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::TextureManager>())
        {
            m_textureInterestPaths.clear();
            m_textureInterestRequestKey.clear();
            return;
        }

        auto& textureManager = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager);
        for (const auto& path : m_textureInterestPaths)
            textureManager.CancelAsyncArtifact(path);
        m_textureInterestPaths.clear();
        m_textureInterestRequestKey.clear();
    }

    std::shared_ptr<void> CapturePreviewRenderInputsKeepAlive()
    {
        if (m_previewObjects.empty() && m_materialPreviewMaterial == nullptr)
            return nullptr;

        auto keepAlive = std::make_shared<PreviewRenderInputsKeepAlive>();
        keepAlive->scene = &m_scene;
        keepAlive->objects = std::move(m_previewObjects);
        m_previewObjects.clear();
        keepAlive->material = std::move(m_materialPreviewMaterial);
        return keepAlive;
    }

    void RetirePendingReadback()
    {
        if (!m_pendingReadback.active)
            return;

        RetirePreviewReadback(std::move(m_pendingReadback));
        m_pendingReadback = {};
    }

    bool CollectRenderableBounds(
        const AssetThumbnailRequest& request,
        NLS::Engine::GameObject& object,
        Bounds& bounds,
        EditorThumbnailPreviewResult& result)
    {
        if (!object.IsActive())
            return true;

        auto* meshFilter = object.GetComponent<NLS::Engine::Components::MeshFilter>();
        auto* meshRenderer = object.GetComponent<NLS::Engine::Components::MeshRenderer>();
        if (meshFilter != nullptr && meshRenderer != nullptr)
        {
            auto* mesh = meshFilter->ResolveMesh();
            if (mesh == nullptr)
            {
                const auto meshPath = meshFilter->GetModelPath();
                if (!meshPath.empty() &&
                    NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MeshManager>())
                {
                    mesh = NLS_SERVICE(NLS::Core::ResourceManagement::MeshManager).GetResource(meshPath, false);
                    if (mesh == nullptr)
                    {
                        result.diagnostic = "thumbnail-gpu-preview-resources-pending";
                        return false;
                    }
                    meshFilter->SetResolvedMeshFromReference(mesh);
                }
            }

            meshRenderer->SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::DISABLED);
            auto resolvedMaterials = meshRenderer->ResolveMaterials();
            for (auto* material : resolvedMaterials)
            {
                if (material == nullptr)
                    continue;

                std::unordered_set<std::string> requestedTexturePaths;
                if (!MaterialTextureDependenciesReady(*material, m_textureInterestPaths, &requestedTexturePaths))
                {
                    TrackRequestedTextureInterests(requestedTexturePaths);
                    result.diagnostic = "thumbnail-gpu-preview-resources-pending";
                    return false;
                }
                TrackRequestedTextureInterests(requestedTexturePaths);
            }
            if (mesh != nullptr)
                IncludeWorldBounds(bounds, mesh->GetBounds(), object.GetTransform()->GetWorldMatrix());
        }

        for (auto* child : object.GetChildren())
        {
            if (child != nullptr && !CollectRenderableBounds(request, *child, bounds, result))
                return false;
        }
        return true;
    }

    NLS::Render::Resources::Material& DefaultMaterial()
    {
        if (!m_defaultMaterial.HasShader() &&
            NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::ShaderManager>())
        {
            m_defaultMaterial.SetShader(
                NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager).GetResource(":Shaders/Standard.hlsl"));
            m_defaultMaterial.Set("u_Albedo", Maths::Vector4(0.72f, 0.74f, 0.78f, 1.0f));
            m_defaultMaterial.Set("u_Metallic", 0.0f);
            m_defaultMaterial.Set("u_Roughness", 0.72f);
        }
        return m_defaultMaterial;
    }

    void ConfigureCamera(const Bounds& bounds, const uint32_t width, const uint32_t height)
    {
        const Maths::Vector3 center = (bounds.min + bounds.max) * 0.5f;
        const Maths::Vector3 extents = (bounds.max - bounds.min) * 0.5f;
        const auto halfSize = (std::max)(0.001f, extents.Length());
        const auto fovRadians = kUnityMeshPreviewFieldOfViewDegrees * kDegreesToRadians;
        const auto distance = (halfSize / std::tan(fovRadians * 0.5f)) * 1.6f;
        const auto direction = PreviewDirection();
        auto* owner = m_camera->gameobject();
        if (owner == nullptr)
            return;
        auto* transform = owner->GetTransform();
        transform->SetLocalPosition(center - direction * distance);
        transform->SetLocalRotation(Maths::Quaternion::LookAt(direction, Maths::Vector3::Up));
        m_camera->SetFov(kUnityMeshPreviewFieldOfViewDegrees);
        m_camera->SetNear((std::max)(0.001f, distance - halfSize * 3.0f));
        m_camera->SetFar(distance + halfSize * 4.0f);
        m_camera->GetCamera()->CacheMatrices(
            static_cast<uint16_t>(width),
            static_cast<uint16_t>(height));
    }

    void ConfigurePrefabCamera(const Bounds& bounds, const uint32_t width, const uint32_t height)
    {
        const auto placement = BuildPrefabPreviewCameraPlacement(bounds, width, height);
        const auto rotation = Maths::Quaternion::LookAt(placement.direction, Maths::Vector3::Up);
        auto* owner = m_camera->gameobject();
        if (owner == nullptr)
            return;
        auto* transform = owner->GetTransform();
        transform->SetLocalPosition(placement.center - placement.direction * placement.distance);
        transform->SetLocalRotation(rotation);
        m_camera->SetFov(kUnityMeshPreviewFieldOfViewDegrees);
        m_camera->SetNear((std::max)(0.001f, placement.distance - placement.radius * 3.0f));
        m_camera->SetFar(placement.distance + placement.radius * 5.0f);
        m_camera->GetCamera()->CacheMatrices(
            static_cast<uint16_t>(width),
            static_cast<uint16_t>(height));
    }

    void ConfigureMaterialCamera(const uint32_t width, const uint32_t height)
    {
        auto* owner = m_camera->gameobject();
        if (owner == nullptr)
            return;

        auto* transform = owner->GetTransform();
        transform->SetLocalPosition({0.0f, 0.0f, -kUnityMaterialPreviewDistance});
        transform->SetLocalRotation(Maths::Quaternion::Identity);
        m_camera->SetFov(kUnityMeshPreviewFieldOfViewDegrees);
        m_camera->SetNear(0.1f);
        m_camera->SetFar(10.0f);
        m_camera->GetCamera()->CacheMatrices(
            static_cast<uint16_t>(width),
            static_cast<uint16_t>(height));
    }

    NLS::Render::Context::Driver& m_driver;
    std::unique_ptr<NLS::Engine::Rendering::BaseSceneRenderer> m_renderer;
    NLS::Engine::SceneSystem::Scene m_scene;
    NLS::Engine::Components::CameraComponent* m_camera = nullptr;
    NLS::Render::Buffers::Framebuffer m_framebuffer;
    NLS::Render::Resources::Material m_defaultMaterial;
    std::unique_ptr<NLS::Render::Resources::Material> m_materialPreviewMaterial;
    std::vector<NLS::Engine::GameObject*> m_previewObjects;
    EditorThumbnailPreviewReadbackState m_pendingReadback;
    std::string m_activeRequestKey;
    std::unordered_set<std::string> m_textureInterestPaths;
    std::string m_textureInterestRequestKey;
    std::vector<PreviewSnapshotCacheEntry> m_previewSnapshotCache;
    uint64_t m_previewSnapshotCacheClock = 0u;
};

EditorThumbnailPreviewRenderer::EditorThumbnailPreviewRenderer(NLS::Render::Context::Driver& driver)
    : m_impl(std::make_unique<Impl>(driver))
{
}

EditorThumbnailPreviewRenderer::~EditorThumbnailPreviewRenderer() = default;

bool EditorThumbnailPreviewRenderer::Supports(const AssetThumbnailRequest& request) const
{
    return m_impl != nullptr && m_impl->Supports(request);
}

EditorThumbnailPreviewResult EditorThumbnailPreviewRenderer::Render(const AssetThumbnailRequest& request)
{
    return m_impl != nullptr ? m_impl->Render(request) : EditorThumbnailPreviewResult {};
}

EditorThumbnailPreviewRendererAdapter::EditorThumbnailPreviewRendererAdapter(
    EditorThumbnailPreviewRenderer& renderer)
    : m_renderer(renderer)
{
}

bool EditorThumbnailPreviewRendererAdapter::Supports(const AssetThumbnailRequest& request) const
{
    return m_renderer.Supports(request);
}

EditorThumbnailPreviewResult EditorThumbnailPreviewRendererAdapter::Render(
    const AssetThumbnailRequest& request)
{
    return m_renderer.Render(request);
}
}

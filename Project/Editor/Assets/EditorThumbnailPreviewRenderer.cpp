#include "Assets/EditorThumbnailPreviewRenderer.h"

#include "Assets/AssetThumbnailService.h"
#include "Assets/AssetThumbnailPreviewCamera.h"
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
#include "Jobs/BackgroundJobQueue.h"
#include "Jobs/JobSystem.h"
#include "Profiling/PerformanceStageStats.h"
#include "Rendering/Buffers/Framebuffer.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Assets/ArtifactDatabase.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "SceneSystem/Scene.h"
#include "ServiceLocator.h"

#include <algorithm>
#include <any>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
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
    if (!state.active)
        return result;

    NLS::Render::RHI::RHICompletionStatusCode completionCode =
        NLS::Render::RHI::RHICompletionStatusCode::Pending;
    std::string completionMessage;
    bool hasTerminalPostSubmitResult = false;
    if (state.postSubmitTextureReadbackState != nullptr)
    {
        std::lock_guard lock(state.postSubmitTextureReadbackState->mutex);
        if (!state.postSubmitTextureReadbackState->beginAttempted ||
            state.postSubmitTextureReadbackState->beginInProgress)
        {
            result.status = EditorThumbnailPreviewReadbackPollStatus::Pending;
            return result;
        }

        completionMessage = state.postSubmitTextureReadbackState->resultMessage;
        if (!state.postSubmitTextureReadbackState->beginSucceeded)
        {
            hasTerminalPostSubmitResult = true;
            completionCode =
                state.postSubmitTextureReadbackState->resultCode == NLS::Render::RHI::RHIReadbackStatusCode::DeviceLost
                    ? NLS::Render::RHI::RHICompletionStatusCode::DeviceLost
                    : NLS::Render::RHI::RHICompletionStatusCode::Failed;
        }
        else
        {
            state.completion = state.postSubmitTextureReadbackState->completion;
            if (state.completion == nullptr)
            {
                hasTerminalPostSubmitResult = true;
                completionCode = NLS::Render::RHI::RHICompletionStatusCode::Success;
            }
        }
    }

    if (!hasTerminalPostSubmitResult && state.completion == nullptr)
        return result;

    if (!hasTerminalPostSubmitResult && driver != nullptr)
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
    else if (!hasTerminalPostSubmitResult)
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
        result.preview.rawVisibleDrawCount = state.rawVisibleDrawCount;
        result.preview.submittedSceneDrawCount = state.submittedSceneDrawCount;
        result.preview.gpuTexture = state.gpuTexture;
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
const NLS::Maths::Vector3 kThumbnailPreviewKeyLightDirection{0.35f, -0.72f, 0.60f};
constexpr float kThumbnailPreviewKeyLightIntensity = 0.70f;
constexpr float kThumbnailPreviewMainLightIntensity = 0.52f;
constexpr float kThumbnailPreviewFillLightIntensity = 0.18f;
constexpr float kThumbnailPreviewKeyLightAngularRadiusDegrees = 55.0f;
constexpr std::array<const char*, 2> kThumbnailPreviewKeyLightNames {
    "Thumbnail Preview Key Light",
    "Thumbnail Preview Fill Light"
};
constexpr float kThumbnailPreviewAmbientIntensity = 0.10f;
constexpr size_t kMaxGpuPreviewNativeArtifactFileBytes = 16u * 1024u * 1024u;
constexpr size_t kMaxGpuPreviewStructurePayloadBytes = 2u * 1024u * 1024u;
constexpr size_t kMaxGpuPreviewPrefabGraphObjects = 24000u;
constexpr size_t kMaxGpuPreviewPrefabGraphProperties = 160000u;
constexpr size_t kMaxGpuPreviewPrefabResolvedAssets = 4096u;
constexpr size_t kMaxGpuPreviewPrefabProxyDrawItems = 32u;
constexpr size_t kMaxGpuPreviewPrefabProxyCandidateDrawItems = 64u;
constexpr size_t kPersistentPrefabProxyThreshold = 64u;
constexpr uint32_t kPersistentPrefabProxyMaxIndices = 480000u;
constexpr float kPersistentPrefabProxyFormalLODScreenSize = 0.25f;
constexpr size_t kGpuPreviewPrefabProxyBinsX = 2u;
constexpr size_t kGpuPreviewPrefabProxyBinsY = 2u;
constexpr size_t kGpuPreviewPrefabProxyBinsZ = 2u;
constexpr size_t kMaxGpuPreviewMeshVertices = 240000u;
constexpr size_t kMaxGpuPreviewMeshIndices = 720000u;
constexpr size_t kMaxPreviewRenderableSnapshotCacheEntries = 64u;
constexpr size_t kMaxPendingPrefabPreviewPreparations = 8u;
constexpr size_t kThumbnailPreviewMeshPumpCompletionsPerFrame = 8u;
constexpr size_t kThumbnailPreviewPrefabMeshRequestStartsPerFrame = 1u;
constexpr size_t kThumbnailPreviewPrefabMeshPumpCompletionsPerFrame = 1u;
constexpr size_t kThumbnailPreviewMaterialPumpCompletionsPerFrame = 1u;
constexpr size_t kThumbnailPreviewTexturePumpCompletionsPerFrame = 8u;
constexpr size_t kThumbnailPreviewPrefabTexturePumpCompletionsPerFrame = 1u;
constexpr size_t kThumbnailPreviewPrefabResourceInspectionsPerTypePerFrame = 32u;
constexpr auto kThumbnailPreviewPrefabResourcePumpTimeBudget = std::chrono::microseconds(1000);
constexpr size_t kThumbnailPreviewPrefabSceneAssemblyMinimumBatch = 1u;
constexpr size_t kThumbnailPreviewPrefabSceneAssemblyMaximumBatch = 64u;
constexpr auto kThumbnailPreviewPrefabSceneAssemblyTimeBudget = std::chrono::microseconds(1000);
constexpr size_t kMaxRetiredPreviewReadbacks = 32u;
constexpr const char* kGpuPreviewMeshBudgetExceededDiagnostic = "thumbnail-model-preview-budget-exceeded";
constexpr const char* kGpuPreviewMaterialBudgetExceededDiagnostic = "thumbnail-material-preview-budget-exceeded";
constexpr const char* kGpuPreviewPrefabBudgetExceededDiagnostic = "thumbnail-prefab-preview-budget-exceeded";

struct ThumbnailPreviewKeyLightSample
{
    NLS::Maths::Vector3 direction;
    float intensity = 0.0f;
};

const std::array<ThumbnailPreviewKeyLightSample, 2>& ThumbnailPreviewKeyLightSamples()
{
    static const auto samples = []
    {
        const auto center = kThumbnailPreviewKeyLightDirection.Normalised();
        const auto right = NLS::Maths::Vector3::Cross(NLS::Maths::Vector3::Up, center).Normalised();
        const auto radius = kThumbnailPreviewKeyLightAngularRadiusDegrees *
            ThumbnailPreviewCamera::DegreesToRadians;
        return std::array<ThumbnailPreviewKeyLightSample, 2> {
            ThumbnailPreviewKeyLightSample {center, kThumbnailPreviewMainLightIntensity},
            ThumbnailPreviewKeyLightSample {
                (center * std::cos(radius) - right * std::sin(radius)).Normalised(),
                kThumbnailPreviewFillLightIntensity}
        };
    }();
    return samples;
}

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
    std::string key;
    key.reserve(256u + request.freshnessInputs.size() * 48u);
    key += "preview-readback:v2|";
    key += ToGenericPath(request.projectRoot);
    key += '|';
    key += std::to_string(static_cast<int>(request.kind));
    key += '|';
    key += request.assetId.ToString();
    key += '|';
    key += request.subAssetKey;
    key += '|';
    key += std::to_string(request.requestedSize);
    key += '|';
    key += request.settingsFingerprint;
    key += '|';
    key += request.dependencyStamp;
    key += '|';
    key += request.previewRendererVersion;
    key += '|';
    key += request.artifactPath;
    key += '|';
    key += request.sourceAssetPath;
    for (const auto& [name, value] : request.freshnessInputs)
    {
        key += "|fresh:";
        key += name;
        key += '=';
        key += value;
    }
    return key;
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
            NLS::Render::Assets::IsMeshArtifactFile(*resolved))
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
            GetEditorAssetRootLibraryPath(root) / "Artifacts";

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

    auto portableArtifactPath = NLS::Core::Assets::TryMakePortableContentArtifactPath(materialPath);
    if (NLS::Core::Assets::IsContentStorageArtifactPath(materialPath) || !portableArtifactPath.empty())
    {
        return materialManager.RequestAsyncArtifactForPreview(
            portableArtifactPath.empty() ? materialPath : portableArtifactPath,
            true);
    }

    return materialManager.GetResource(materialPath, true);
}

bool ShouldLoadPreviewMeshThroughArtifactLoader(const std::string& meshPath)
{
    if (meshPath.empty())
        return false;

    if (NLS::Core::Assets::IsContentStorageArtifactPath(meshPath))
        return true;

    std::error_code error;
    return std::filesystem::is_regular_file(meshPath, error) &&
        NLS::Render::Assets::IsMeshArtifactFile(meshPath);
}

bool BindReadyMaterialPreviewTextures(
    NLS::Render::Resources::Material& material,
    const std::unordered_set<std::string>& activeTextureInterests = {},
    std::unordered_set<std::string>* requestedTexturePaths = nullptr)
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

        NLS::Render::Resources::Texture2D* texture = nullptr;
        if (const auto uniform = uniforms.find(name); uniform != uniforms.end())
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
        texture = textureManager.GetArtifactResource(path, false);
        if (texture == nullptr && textureManager.IsAsyncArtifactLoadFailed(path))
            continue;
        if (texture == nullptr &&
            (activeTextureInterests.find(genericPath) == activeTextureInterests.end() ||
                !textureManager.IsAsyncArtifactLoadPending(path)))
        {
            texture = textureManager.RequestAsyncArtifact(path, true);
        }
        if (texture != nullptr && texture->GetTextureHandle() != nullptr)
        {
            material.SetRawParameter(name, texture);
            continue;
        }
        ready = false;
    }
    return ready;
}

std::string ToLowerGenericPath(std::string path)
{
    path = std::filesystem::path(path).generic_string();
    std::transform(path.begin(), path.end(), path.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return path;
}

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool IsStandardPbrSourcePath(const std::string& path)
{
    const auto sourcePath = ToLowerGenericPath(path);
    return sourcePath == "app/assets/engine/shaders/shaderlab/standardpbr.shader" ||
        sourcePath.ends_with("/app/assets/engine/shaders/shaderlab/standardpbr.shader") ||
        sourcePath == "assets/engine/shaders/shaderlab/standardpbr.shader" ||
        sourcePath.ends_with("/assets/engine/shaders/shaderlab/standardpbr.shader");
}

bool IsStandardPbrForwardSubAssetKey(const std::string& subAssetKey)
{
    const auto key = ToLowerAscii(subAssetKey);
    return key == "shader:standardpbr" ||
        key == "shader:standardpbr/forward" ||
        key.rfind("shader:standardpbr/forward#", 0u) == 0u;
}

bool IsThumbnailPreviewDefaultShader(const NLS::Render::Resources::Shader& shader)
{
    return shader.GetShaderLabPassState().has_value() &&
        IsStandardPbrSourcePath(shader.GetImportedArtifactSourcePath()) &&
        IsStandardPbrForwardSubAssetKey(shader.GetImportedArtifactSubAssetKey()) &&
        shader.GetShaderLabLightMode() == "Forward";
}

NLS::Render::Resources::Shader* ResolveThumbnailPreviewDefaultShader(
    NLS::Core::ResourceManagement::ShaderManager& shaderManager,
    std::string* resourcePath = nullptr)
{
    for (const auto& [path, shader] : shaderManager.GetResources())
    {
        if (shader != nullptr && IsThumbnailPreviewDefaultShader(*shader))
        {
            if (resourcePath != nullptr)
                *resourcePath = path;
            return shader;
        }
    }

    if (resourcePath != nullptr)
        resourcePath->clear();
    return nullptr;
}

bool PreviewSnapshotIsCompleteForGpuPrefabPreview(const PreviewRenderableSnapshot& snapshot)
{
    return snapshot.expectedDrawItemCount <= snapshot.drawItems.size();
}

bool ShouldDeferPrefabPreviewForResourceReadiness(
    const size_t pendingMeshResourceCount,
    const size_t pendingMaterialResourceCount,
    const size_t pendingMaterialTextureCount,
    const bool resourcePlanTruncated)
{
    return pendingMeshResourceCount != 0u ||
        pendingMaterialResourceCount != 0u ||
        pendingMaterialTextureCount != 0u ||
        resourcePlanTruncated;
}

bool ShouldDeferPrefabPreviewAfterDrawPrewarm(
    const bool prewarmSupported,
    const bool prewarmComplete)
{
    return prewarmSupported && !prewarmComplete;
}

bool ShouldWaitForPersistentPrefabPreviewProxy(
    const bool usesProvisionalPlan,
    const bool persistentProxyReady)
{
    return usesProvisionalPlan && !persistentProxyReady;
}

bool ShouldUseFullSourceBoundsForPrefabCamera(const bool usesProvisionalPlan)
{
    return usesProvisionalPlan;
}

bool ShouldPreservePrefabPreviewSceneAfterRenderAttempt(const std::string& diagnostic)
{
    if (diagnostic == "thumbnail-gpu-preview-readback-pending")
        return true;

    constexpr const char* kDrawPrewarmPendingPrefix =
        "thumbnail-gpu-preview-resources-pending:prefab-draw-prewarm=";
    return diagnostic.rfind(kDrawPrewarmPendingPrefix, 0u) == 0u;
}

NLS::Render::Resources::Mesh* ResolvePreviewMesh(
    NLS::Core::ResourceManagement::MeshManager& meshManager,
    const std::string& meshPath)
{
    if (meshPath.empty())
        return nullptr;

    if (auto* cached = meshManager.GetResource(meshPath, false))
        return cached;

    if (ShouldLoadPreviewMeshThroughArtifactLoader(meshPath))
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
    const std::string& artifactPath);

std::string ResolvePreviewMeshLoadPath(
    const AssetThumbnailRequest& request,
    const std::string& meshPath,
    const NLS::Core::Assets::AssetId meshAssetId)
{
    const auto genericMeshPath = ToGenericPath(meshPath);
    auto meshLoadPath = ResolvePrefabPreviewMeshBudgetPath(request, genericMeshPath);
    if (!meshLoadPath.has_value())
        meshLoadPath = ResolvePrefabDependencyArtifactBudgetPath(request, meshAssetId, genericMeshPath);
    return meshLoadPath.has_value()
        ? ToGenericPath(*meshLoadPath)
        : genericMeshPath;
}

std::string ResolvePreviewMaterialLoadPath(
    const AssetThumbnailRequest& request,
    const std::string& materialPath,
    const NLS::Core::Assets::AssetId materialAssetId)
{
    const auto genericMaterialPath = ToGenericPath(materialPath);
    auto materialLoadPath = ResolvePrefabPreviewMaterialBudgetPath(request, genericMaterialPath);
    if (!materialLoadPath.has_value())
        materialLoadPath = ResolvePrefabDependencyArtifactBudgetPath(request, materialAssetId, genericMaterialPath);
    return materialLoadPath.has_value()
        ? ToGenericPath(*materialLoadPath)
        : genericMaterialPath;
}

struct PrefabPreviewResourcePlanDrawItem
{
    size_t drawItemIndex = SIZE_MAX;
    std::string meshLoadPath;
    std::vector<std::string> materialLoadPaths;
    uint32_t meshVertexCount = 0u;
    uint32_t meshIndexCount = 0u;
    uint32_t meshMaterialIndex = 0u;
    NLS::Maths::Vector3 worldBoundsCenter {};
    float worldBoundsRadius = 0.0f;
};

struct PrefabPreviewResourcePlan
{
    std::vector<PrefabPreviewResourcePlanDrawItem> drawItems;
    std::unordered_set<std::string> meshLoadPaths;
    std::unordered_set<std::string> materialLoadPaths;
    NLS::Maths::Vector3 fullWorldBoundsMin {};
    NLS::Maths::Vector3 fullWorldBoundsMax {};
    std::string diagnostic;
    size_t sourceDrawItemCount = 0u;
    size_t dependencyDrawItemInspectionCount = 0u;
    bool hasFullWorldBounds = false;
    bool truncatedForPendingResources = false;
};

void IncludePrefabPreviewProxyBounds(
    PrefabPreviewResourcePlan& plan,
    PrefabPreviewResourcePlanDrawItem& planned,
    const PreviewDrawItem& drawItem,
    const std::optional<NLS::Render::Geometry::BoundingSphere>& localBounds)
{
    const auto maxScale = (std::max)({
        std::abs(drawItem.localScale.x),
        std::abs(drawItem.localScale.y),
        std::abs(drawItem.localScale.z)
    });
    planned.worldBoundsCenter = drawItem.localPosition;
    planned.worldBoundsRadius = (std::max)(0.5f * maxScale, 0.001f);
    if (localBounds.has_value())
    {
        const NLS::Maths::Vector3 scaledCenter {
            localBounds->position.x * drawItem.localScale.x,
            localBounds->position.y * drawItem.localScale.y,
            localBounds->position.z * drawItem.localScale.z
        };
        planned.worldBoundsCenter = drawItem.localPosition + NLS::Maths::Quaternion::RotatePoint(
            scaledCenter,
            NLS::Maths::Quaternion::Normalize(drawItem.localRotation));
        planned.worldBoundsRadius = (std::max)(localBounds->radius * maxScale, 0.001f);
    }

    const auto radius = NLS::Maths::Vector3(
        planned.worldBoundsRadius,
        planned.worldBoundsRadius,
        planned.worldBoundsRadius);
    const auto itemMin = planned.worldBoundsCenter - radius;
    const auto itemMax = planned.worldBoundsCenter + radius;
    if (!plan.hasFullWorldBounds)
    {
        plan.fullWorldBoundsMin = itemMin;
        plan.fullWorldBoundsMax = itemMax;
        plan.hasFullWorldBounds = true;
        return;
    }

    plan.fullWorldBoundsMin.x = (std::min)(plan.fullWorldBoundsMin.x, itemMin.x);
    plan.fullWorldBoundsMin.y = (std::min)(plan.fullWorldBoundsMin.y, itemMin.y);
    plan.fullWorldBoundsMin.z = (std::min)(plan.fullWorldBoundsMin.z, itemMin.z);
    plan.fullWorldBoundsMax.x = (std::max)(plan.fullWorldBoundsMax.x, itemMax.x);
    plan.fullWorldBoundsMax.y = (std::max)(plan.fullWorldBoundsMax.y, itemMax.y);
    plan.fullWorldBoundsMax.z = (std::max)(plan.fullWorldBoundsMax.z, itemMax.z);
}

void ApplyBoundedPrefabPreviewProxy(
    PrefabPreviewResourcePlan& plan,
    const size_t targetDrawItemCapacity = kMaxGpuPreviewPrefabProxyDrawItems,
    const size_t collapsedTransformDrawItemCapacity = kMaxGpuPreviewPrefabProxyDrawItems)
{
    if (plan.drawItems.size() <= targetDrawItemCapacity || !plan.hasFullWorldBounds)
        return;

    constexpr size_t kBinCount =
        kGpuPreviewPrefabProxyBinsX * kGpuPreviewPrefabProxyBinsY * kGpuPreviewPrefabProxyBinsZ;
    static_assert(kBinCount < kMaxGpuPreviewPrefabProxyDrawItems);
    std::array<size_t, kBinCount> binWinners;
    binWinners.fill(SIZE_MAX);

    const auto extent = plan.fullWorldBoundsMax - plan.fullWorldBoundsMin;
    auto binCoordinate = [](const float value, const float minimum, const float axisExtent, const size_t binCount)
    {
        if (axisExtent <= std::numeric_limits<float>::epsilon())
            return size_t {0u};
        const auto normalized = std::clamp((value - minimum) / axisExtent, 0.0f, 1.0f);
        return (std::min)(static_cast<size_t>(normalized * static_cast<float>(binCount)), binCount - 1u);
    };

    for (size_t index = 0u; index < plan.drawItems.size(); ++index)
    {
        const auto& item = plan.drawItems[index];
        const auto x = binCoordinate(
            item.worldBoundsCenter.x,
            plan.fullWorldBoundsMin.x,
            extent.x,
            kGpuPreviewPrefabProxyBinsX);
        const auto y = binCoordinate(
            item.worldBoundsCenter.y,
            plan.fullWorldBoundsMin.y,
            extent.y,
            kGpuPreviewPrefabProxyBinsY);
        const auto z = binCoordinate(
            item.worldBoundsCenter.z,
            plan.fullWorldBoundsMin.z,
            extent.z,
            kGpuPreviewPrefabProxyBinsZ);
        const auto binIndex = x + kGpuPreviewPrefabProxyBinsX * (y + kGpuPreviewPrefabProxyBinsY * z);
        const auto winnerIndex = binWinners[binIndex];
        if (winnerIndex == SIZE_MAX ||
            item.worldBoundsRadius > plan.drawItems[winnerIndex].worldBoundsRadius ||
            (item.worldBoundsRadius == plan.drawItems[winnerIndex].worldBoundsRadius &&
                item.drawItemIndex < plan.drawItems[winnerIndex].drawItemIndex))
        {
            binWinners[binIndex] = index;
        }
    }

    std::vector<size_t> selectedIndices;
    selectedIndices.reserve((std::max)(
        targetDrawItemCapacity,
        collapsedTransformDrawItemCapacity));
    std::vector<bool> selected(plan.drawItems.size(), false);
    for (const auto index : binWinners)
    {
        if (index == SIZE_MAX || selected[index])
            continue;
        selected[index] = true;
        selectedIndices.push_back(index);
    }

    std::vector<size_t> remainingIndices;
    remainingIndices.reserve(plan.drawItems.size() - selectedIndices.size());
    for (size_t index = 0u; index < plan.drawItems.size(); ++index)
    {
        if (!selected[index])
            remainingIndices.push_back(index);
    }
    std::sort(remainingIndices.begin(), remainingIndices.end(), [&plan](const size_t lhs, const size_t rhs)
    {
        if (plan.drawItems[lhs].worldBoundsRadius != plan.drawItems[rhs].worldBoundsRadius)
            return plan.drawItems[lhs].worldBoundsRadius > plan.drawItems[rhs].worldBoundsRadius;
        return plan.drawItems[lhs].drawItemIndex < plan.drawItems[rhs].drawItemIndex;
    });

    // Imported scenes can bake spatial placement into mesh vertices while every
    // node transform remains at the origin. In that case transform-only bins
    // collapse to one winner. Fill the proxy with stratified source-order
    // samples so the preview still represents the complete imported scene.
    if (selectedIndices.size() <= 1u &&
        remainingIndices.size() + selectedIndices.size() > targetDrawItemCapacity)
    {
        const auto selectionCapacity = (std::min)(
            collapsedTransformDrawItemCapacity,
            plan.drawItems.size());
        selectedIndices.clear();
        selectedIndices.reserve(selectionCapacity);
        const auto sourceCount = plan.drawItems.size();
        for (size_t sample = 0u; sample < selectionCapacity; ++sample)
        {
            const auto index = (sample * sourceCount) / selectionCapacity;
            selectedIndices.push_back((std::min)(index, sourceCount - 1u));
        }
    }
    else
    {
        for (const auto index : remainingIndices)
        {
            if (selectedIndices.size() >= targetDrawItemCapacity)
                break;
            selectedIndices.push_back(index);
        }
    }
    std::sort(selectedIndices.begin(), selectedIndices.end(), [&plan](const size_t lhs, const size_t rhs)
    {
        return plan.drawItems[lhs].drawItemIndex < plan.drawItems[rhs].drawItemIndex;
    });

    std::vector<PrefabPreviewResourcePlanDrawItem> proxyDrawItems;
    proxyDrawItems.reserve(selectedIndices.size());
    plan.meshLoadPaths.clear();
    plan.materialLoadPaths.clear();
    for (const auto index : selectedIndices)
    {
        auto item = std::move(plan.drawItems[index]);
        if (!item.meshLoadPath.empty())
            plan.meshLoadPaths.insert(item.meshLoadPath);
        for (const auto& materialPath : item.materialLoadPaths)
        {
            if (!materialPath.empty())
                plan.materialLoadPaths.insert(materialPath);
        }
        proxyDrawItems.push_back(std::move(item));
    }
    plan.drawItems = std::move(proxyDrawItems);
}

PrefabPreviewResourcePlan BuildPrefabPreviewResourcePlan(
    const AssetThumbnailRequest& request,
    const PreviewRenderableSnapshot& snapshot,
    NLS::Core::ResourceManagement::MeshManager* meshManager = nullptr,
    NLS::Core::ResourceManagement::MaterialManager* materialManager = nullptr,
    size_t maxUnreadyDependencyAttempts = SIZE_MAX)
{
    PrefabPreviewResourcePlan plan;
    plan.sourceDrawItemCount = snapshot.drawItems.size();
    plan.drawItems.reserve(snapshot.drawItems.size());

    // Preserve the complete prefab draw set. Dependency resolution and GPU upload
    // are sliced later, matching thumbnail-pool scheduling without dropping content.
    for (size_t drawItemIndex = 0u; drawItemIndex < snapshot.drawItems.size(); ++drawItemIndex)
    {
        const auto& drawItem = snapshot.drawItems[drawItemIndex];
        PrefabPreviewResourcePlanDrawItem planned;
        planned.drawItemIndex = drawItemIndex;
        IncludePrefabPreviewProxyBounds(plan, planned, drawItem, std::nullopt);
        plan.drawItems.push_back(std::move(planned));
    }

    std::unordered_map<std::string, std::optional<std::filesystem::path>> meshBudgetPathByKey;
    std::unordered_map<
        std::string,
        std::optional<NLS::Render::Assets::MeshArtifactHeaderPreview>> meshHeadersByKey;
    std::unordered_map<std::string, std::optional<std::filesystem::path>> materialBudgetPathByKey;
    std::unordered_set<std::string> checkedMaterialBudgetPaths;

    auto makeDependencyKey = [](const std::string& genericPath, const NLS::Core::Assets::AssetId& assetId)
    {
        return genericPath + "|" + assetId.ToString();
    };

    size_t unreadyDependencyAttempts = 0u;
    auto canIncludeDependency = [&unreadyDependencyAttempts, maxUnreadyDependencyAttempts](const bool ready)
    {
        if (ready)
            return true;
        if (unreadyDependencyAttempts >= maxUnreadyDependencyAttempts)
            return false;
        ++unreadyDependencyAttempts;
        return true;
    };

    for (size_t plannedIndex = 0u; plannedIndex < plan.drawItems.size(); ++plannedIndex)
    {
        auto& planned = plan.drawItems[plannedIndex];
        if (planned.drawItemIndex >= snapshot.drawItems.size())
            continue;
        ++plan.dependencyDrawItemInspectionCount;
        const auto& drawItem = snapshot.drawItems[planned.drawItemIndex];

        const auto genericMeshPath = ToGenericPath(drawItem.meshPath);
        const auto meshKey = makeDependencyKey(genericMeshPath, drawItem.meshAssetId);
        auto meshBudgetPathIterator = meshBudgetPathByKey.find(meshKey);
        if (meshBudgetPathIterator == meshBudgetPathByKey.end())
        {
            auto meshBudgetPath = ResolvePrefabPreviewMeshBudgetPath(request, genericMeshPath);
            if (!meshBudgetPath.has_value())
                meshBudgetPath = ResolvePrefabDependencyArtifactBudgetPath(
                    request,
                    drawItem.meshAssetId,
                    genericMeshPath);
            meshBudgetPathIterator = meshBudgetPathByKey.emplace(meshKey, std::move(meshBudgetPath)).first;
        }
        planned.meshLoadPath = meshBudgetPathIterator->second.has_value()
            ? ToGenericPath(*meshBudgetPathIterator->second)
            : genericMeshPath;
        auto meshHeaderIterator = meshHeadersByKey.find(meshKey);
        if (meshHeaderIterator == meshHeadersByKey.end())
        {
            std::optional<NLS::Render::Assets::MeshArtifactHeaderPreview> meshHeader;
            if (meshBudgetPathIterator->second.has_value())
            {
                meshHeader = NLS::Render::Assets::ReadMeshArtifactHeaderPreview(
                    *meshBudgetPathIterator->second,
                    kMaxGpuPreviewStructurePayloadBytes);
            }
            meshHeaderIterator = meshHeadersByKey.emplace(meshKey, std::move(meshHeader)).first;
        }
        if (meshHeaderIterator->second.has_value())
        {
            planned.meshVertexCount = meshHeaderIterator->second->vertexCount;
            planned.meshIndexCount = meshHeaderIterator->second->indexCount;
            planned.meshMaterialIndex = meshHeaderIterator->second->materialIndex;
        }
        IncludePrefabPreviewProxyBounds(
            plan,
            planned,
            drawItem,
            meshHeaderIterator->second.has_value() && meshHeaderIterator->second->hasBoundingSphere
                ? std::optional<NLS::Render::Geometry::BoundingSphere>(
                    meshHeaderIterator->second->boundingSphere)
                : std::nullopt);
        const bool meshReady = planned.meshLoadPath.empty() ||
            meshManager == nullptr ||
            meshManager->GetResource(planned.meshLoadPath, false) != nullptr;
        if (!canIncludeDependency(meshReady))
        {
            plan.truncatedForPendingResources = true;
            plan.drawItems.resize(plannedIndex);
            return plan;
        }
        if (!planned.meshLoadPath.empty())
            plan.meshLoadPaths.insert(planned.meshLoadPath);

    }

    for (auto& planned : plan.drawItems)
    {
        if (planned.drawItemIndex >= snapshot.drawItems.size())
            continue;
        const auto& drawItem = snapshot.drawItems[planned.drawItemIndex];
        planned.materialLoadPaths.reserve(drawItem.materialPaths.size());
        for (size_t materialIndex = 0u; materialIndex < drawItem.materialPaths.size(); ++materialIndex)
        {
            const auto genericMaterialPath = ToGenericPath(drawItem.materialPaths[materialIndex]);
            if (genericMaterialPath.empty())
            {
                planned.materialLoadPaths.emplace_back();
                continue;
            }

            const auto materialAssetId = materialIndex < drawItem.materialAssetIds.size()
                ? drawItem.materialAssetIds[materialIndex]
                : NLS::Core::Assets::AssetId {};
            const auto materialKey = makeDependencyKey(genericMaterialPath, materialAssetId);
            auto materialBudgetPathIterator = materialBudgetPathByKey.find(materialKey);
            if (materialBudgetPathIterator == materialBudgetPathByKey.end())
            {
                auto materialBudgetPath = ResolvePrefabPreviewMaterialBudgetPath(request, genericMaterialPath);
                if (!materialBudgetPath.has_value())
                {
                    materialBudgetPath = ResolvePrefabDependencyArtifactBudgetPath(
                        request,
                        materialAssetId,
                        genericMaterialPath);
                }
                materialBudgetPathIterator = materialBudgetPathByKey.emplace(
                    materialKey,
                    std::move(materialBudgetPath)).first;
            }
            if (materialBudgetPathIterator->second.has_value())
            {
                const auto budgetPathKey = ToGenericPath(*materialBudgetPathIterator->second);
                if (checkedMaterialBudgetPaths.insert(budgetPathKey).second &&
                    MaterialArtifactExceedsGpuPreviewBudget(*materialBudgetPathIterator->second))
                {
                    plan.diagnostic = kGpuPreviewMaterialBudgetExceededDiagnostic;
                    return plan;
                }
            }
            auto materialLoadPath = materialBudgetPathIterator->second.has_value()
                ? ToGenericPath(*materialBudgetPathIterator->second)
                : genericMaterialPath;
            const bool materialReady = materialLoadPath.empty() ||
                materialManager == nullptr ||
                materialManager->GetResource(materialLoadPath, false) != nullptr;
            (void)materialReady;
            if (!materialLoadPath.empty())
                plan.materialLoadPaths.insert(materialLoadPath);
            planned.materialLoadPaths.push_back(std::move(materialLoadPath));
        }
    }
    return plan;
}

std::filesystem::path BuildPersistentPrefabPreviewProxyPath(
    const AssetThumbnailRequest& request)
{
    const auto key = BuildAssetThumbnailCacheKey(request);
    if (request.projectRoot.empty() || key.size() < 2u)
        return {};
    return (request.projectRoot /
        "Library" /
        "AssetThumbnailProxies" /
        key.substr(0u, 2u) /
        (key + ".nmesh")).lexically_normal();
}

std::filesystem::path BuildPersistentPrefabPreviewProxyChunkPath(
    const std::filesystem::path& basePath,
    const size_t chunkIndex)
{
    if (chunkIndex == 0u)
        return basePath;

    auto chunkPath = basePath;
    const auto extension = chunkPath.extension();
    chunkPath.replace_extension();
    chunkPath += ".material-" + std::to_string(chunkIndex);
    chunkPath += extension;
    return chunkPath;
}

bool WritePersistentPrefabPreviewProxy(
    const std::filesystem::path& path,
    const std::vector<uint8_t>& bytes)
{
    if (path.empty() || bytes.empty())
        return false;

    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;

    auto temporaryPath = path;
    temporaryPath += ".tmp";
    {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!output)
            return false;
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output)
        {
            output.close();
            std::filesystem::remove(temporaryPath, error);
            return false;
        }
    }

    std::filesystem::rename(temporaryPath, path, error);
    if (!error)
        return true;

    error.clear();
    if (NLS::Render::Assets::IsMeshArtifactFile(path))
    {
        std::filesystem::remove(temporaryPath, error);
        return true;
    }

    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporaryPath, path, error);
    if (error)
        std::filesystem::remove(temporaryPath, error);
    return !error;
}

NLS::Maths::Vector3 TransformPrefabProxyPosition(
    const NLS::Maths::Vector3& position,
    const PreviewDrawItem& drawItem)
{
    const NLS::Maths::Vector3 scaled {
        position.x * drawItem.localScale.x,
        position.y * drawItem.localScale.y,
        position.z * drawItem.localScale.z
    };
    return drawItem.localPosition + NLS::Maths::Quaternion::RotatePoint(
        scaled,
        NLS::Maths::Quaternion::Normalize(drawItem.localRotation));
}

NLS::Maths::Vector3 TransformPrefabProxyDirection(
    const NLS::Maths::Vector3& direction,
    const PreviewDrawItem& drawItem)
{
    constexpr float kMinimumScale = 0.000001f;
    NLS::Maths::Vector3 inverseScaled {
        direction.x / ((std::max)(std::abs(drawItem.localScale.x), kMinimumScale)),
        direction.y / ((std::max)(std::abs(drawItem.localScale.y), kMinimumScale)),
        direction.z / ((std::max)(std::abs(drawItem.localScale.z), kMinimumScale))
    };
    inverseScaled = NLS::Maths::Quaternion::RotatePoint(
        inverseScaled,
        NLS::Maths::Quaternion::Normalize(drawItem.localRotation));
    return inverseScaled.Normalised();
}

void TransformPrefabProxyVertex(
    NLS::Render::Geometry::Vertex& vertex,
    const PreviewDrawItem& drawItem)
{
    const auto position = TransformPrefabProxyPosition(
        {vertex.position[0], vertex.position[1], vertex.position[2]},
        drawItem);
    const auto normal = TransformPrefabProxyDirection(
        {vertex.normals[0], vertex.normals[1], vertex.normals[2]},
        drawItem);
    const auto tangent = TransformPrefabProxyDirection(
        {vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]},
        drawItem);
    const auto bitangent = TransformPrefabProxyDirection(
        {vertex.bitangent[0], vertex.bitangent[1], vertex.bitangent[2]},
        drawItem);
    vertex.position[0] = position.x;
    vertex.position[1] = position.y;
    vertex.position[2] = position.z;
    vertex.normals[0] = normal.x;
    vertex.normals[1] = normal.y;
    vertex.normals[2] = normal.z;
    vertex.tangent[0] = tangent.x;
    vertex.tangent[1] = tangent.y;
    vertex.tangent[2] = tangent.z;
    vertex.bitangent[0] = bitangent.x;
    vertex.bitangent[1] = bitangent.y;
    vertex.bitangent[2] = bitangent.z;
}

struct PersistentPrefabPreviewProxyChunk
{
    std::filesystem::path meshPath;
    std::string materialLoadPath;
};

struct PersistentPrefabPreviewProxy
{
    std::vector<PersistentPrefabPreviewProxyChunk> chunks;
};

std::string ResolvePersistentPrefabPreviewProxyMaterialPath(
    const PrefabPreviewResourcePlanDrawItem& planned)
{
    return planned.meshMaterialIndex < planned.materialLoadPaths.size()
        ? planned.materialLoadPaths[planned.meshMaterialIndex]
        : std::string {};
}

std::optional<NLS::Render::Assets::MeshArtifactData> LoadPersistentPrefabPreviewSourceMesh(
    const AssetThumbnailRequest& request,
    const std::filesystem::path& plannedPath)
{
    const auto resolvedPath = ResolveArtifactPath(request, plannedPath.generic_string());
    if (!resolvedPath.has_value())
        return std::nullopt;

    const auto portablePath = NLS::Core::Assets::TryMakePortableContentArtifactPath(
        resolvedPath->generic_string());
    if (!portablePath.empty())
    {
        NLS::Core::Assets::RegisterRuntimeAuthorizedArtifactPath(portablePath);
        if (!NLS::Core::Assets::IsRuntimeArtifactPathAuthorized(portablePath))
            return std::nullopt;
    }
    return NLS::Render::Assets::LoadMeshArtifactLOD(
        *resolvedPath,
        kPersistentPrefabProxyFormalLODScreenSize);
}

std::optional<PersistentPrefabPreviewProxy> BuildOrLoadPersistentPrefabPreviewProxy(
    const AssetThumbnailRequest& request,
    const PreviewRenderableSnapshot& snapshot,
    const PrefabPreviewResourcePlan& sourcePlan)
{
    if (sourcePlan.drawItems.size() <= kPersistentPrefabProxyThreshold)
        return std::nullopt;

    const auto proxyPath = BuildPersistentPrefabPreviewProxyPath(request);
    if (proxyPath.empty())
        return std::nullopt;
    struct ProxySource
    {
        const PrefabPreviewResourcePlanDrawItem* planned = nullptr;
        const PreviewDrawItem* drawItem = nullptr;
        uint32_t indexCount = 0u;
        size_t materialGroupIndex = 0u;
    };

    std::map<std::string, size_t> materialGroupCounts;
    for (const auto& planned : sourcePlan.drawItems)
        ++materialGroupCounts[ResolvePersistentPrefabPreviewProxyMaterialPath(planned)];
    if (materialGroupCounts.empty())
        return std::nullopt;

    PersistentPrefabPreviewProxy result;
    result.chunks.reserve(materialGroupCounts.size());
    std::unordered_map<std::string, size_t> materialGroupIndices;
    materialGroupIndices.reserve(materialGroupCounts.size());
    for (const auto& [materialPath, unusedCount] : materialGroupCounts)
    {
        (void)unusedCount;
        const auto groupIndex = result.chunks.size();
        materialGroupIndices.emplace(materialPath, groupIndex);
        result.chunks.push_back({
            BuildPersistentPrefabPreviewProxyChunkPath(proxyPath, groupIndex),
            materialPath
        });
    }

    std::vector<ProxySource> sources;
    sources.reserve(sourcePlan.drawItems.size());
    uint64_t remainingSourceIndices = 0u;
    for (const auto& planned : sourcePlan.drawItems)
    {
        if (planned.drawItemIndex >= snapshot.drawItems.size() || planned.meshLoadPath.empty())
            continue;
        if (planned.meshIndexCount < 3u || planned.meshVertexCount == 0u)
            continue;
        sources.push_back({
            &planned,
            &snapshot.drawItems[planned.drawItemIndex],
            planned.meshIndexCount,
            materialGroupIndices.at(ResolvePersistentPrefabPreviewProxyMaterialPath(planned))
        });
        remainingSourceIndices += planned.meshIndexCount - (planned.meshIndexCount % 3u);
    }
    if (sources.size() != sourcePlan.drawItems.size() ||
        sources.empty() ||
        remainingSourceIndices < 3u)
    {
        return std::nullopt;
    }

    bool cacheComplete = true;
    for (const auto& chunk : result.chunks)
    {
        const auto existing = NLS::Render::Assets::ReadMeshArtifactHeaderPreview(
            chunk.meshPath,
            kMaxGpuPreviewStructurePayloadBytes);
        if (!existing.has_value() || existing->indexCount < 3u || existing->materialIndex != 0u)
        {
            cacheComplete = false;
            break;
        }
    }
    if (cacheComplete)
        return result;

    std::vector<NLS::Render::Assets::MeshArtifactData> proxyMeshes(result.chunks.size());
    for (auto& proxy : proxyMeshes)
        proxy.materialIndex = 0u;

    uint32_t remainingIndexBudget = kPersistentPrefabProxyMaxIndices;
    std::map<std::string, NLS::Render::Assets::MeshArtifactData> sourceMeshCache;
    size_t processedSourceCount = 0u;
    for (size_t sourceIndex = 0u; sourceIndex < sources.size(); ++sourceIndex)
    {
        const auto& source = sources[sourceIndex];
        auto meshIterator = sourceMeshCache.find(source.planned->meshLoadPath);
        if (meshIterator == sourceMeshCache.end())
        {
            const auto sourceMesh = LoadPersistentPrefabPreviewSourceMesh(
                request,
                source.planned->meshLoadPath);
            if (!sourceMesh.has_value())
            {
                throw std::runtime_error(
                    "persistent-proxy-source-load-failed:path=" +
                    source.planned->meshLoadPath);
            }
            meshIterator = sourceMeshCache.emplace(
                source.planned->meshLoadPath,
                std::move(*sourceMesh)).first;
        }
        const auto& sample = meshIterator->second;
        if (sample.indices.size() > remainingIndexBudget)
            return std::nullopt;

        auto& proxy = proxyMeshes[source.materialGroupIndex];
        if (sample.vertices.size() >
                static_cast<size_t>((std::numeric_limits<uint32_t>::max)()) - proxy.vertices.size() ||
            sample.indices.size() >
                static_cast<size_t>((std::numeric_limits<uint32_t>::max)()) - proxy.indices.size())
        {
            throw std::runtime_error(
                "persistent-proxy-material-group-overflow:path=" +
                source.planned->meshLoadPath);
        }
        const auto vertexOffset = static_cast<uint32_t>(proxy.vertices.size());
        for (auto vertex : sample.vertices)
        {
            TransformPrefabProxyVertex(vertex, *source.drawItem);
            proxy.vertices.push_back(vertex);
        }
        for (const auto index : sample.indices)
            proxy.indices.push_back(vertexOffset + index);
        remainingIndexBudget -= static_cast<uint32_t>(sample.indices.size());
        ++processedSourceCount;
    }

    if (processedSourceCount != sources.size())
        return std::nullopt;

    for (size_t groupIndex = 0u; groupIndex < proxyMeshes.size(); ++groupIndex)
    {
        const auto& proxy = proxyMeshes[groupIndex];
        if (proxy.vertices.empty() || proxy.indices.size() < 3u)
            return std::nullopt;
        const auto bytes = NLS::Render::Assets::SerializeMeshArtifact(proxy);
        if (bytes.empty() ||
            !WritePersistentPrefabPreviewProxy(result.chunks[groupIndex].meshPath, bytes))
        {
            return std::nullopt;
        }
    }
    return result;
}

void ApplyPersistentPrefabPreviewProxy(
    PreviewRenderableSnapshot& snapshot,
    PrefabPreviewResourcePlan& plan,
    const PersistentPrefabPreviewProxy& proxy)
{
    const auto sourceDrawItemCount = plan.sourceDrawItemCount;
    const auto dependencyDrawItemInspectionCount = plan.dependencyDrawItemInspectionCount;
    const auto fullWorldBoundsMin = plan.fullWorldBoundsMin;
    const auto fullWorldBoundsMax = plan.fullWorldBoundsMax;
    const auto hasFullWorldBounds = plan.hasFullWorldBounds;

    snapshot.drawItems.clear();
    snapshot.drawItems.reserve(proxy.chunks.size());
    plan.drawItems.clear();
    plan.drawItems.reserve(proxy.chunks.size());
    plan.meshLoadPaths.clear();
    plan.materialLoadPaths.clear();
    for (const auto& chunk : proxy.chunks)
    {
        PreviewDrawItem proxyDrawItem;
        proxyDrawItem.meshPath = ToGenericPath(chunk.meshPath);
        if (!chunk.materialLoadPath.empty())
            proxyDrawItem.materialPaths.push_back(chunk.materialLoadPath);
        snapshot.drawItems.push_back(std::move(proxyDrawItem));

        PrefabPreviewResourcePlanDrawItem planned;
        planned.drawItemIndex = snapshot.drawItems.size() - 1u;
        planned.meshLoadPath = ToGenericPath(chunk.meshPath);
        if (!chunk.materialLoadPath.empty())
            planned.materialLoadPaths.push_back(chunk.materialLoadPath);
        plan.meshLoadPaths.insert(planned.meshLoadPath);
        if (!chunk.materialLoadPath.empty())
            plan.materialLoadPaths.insert(chunk.materialLoadPath);
        plan.drawItems.push_back(std::move(planned));
    }
    snapshot.expectedDrawItemCount = snapshot.drawItems.size();
    plan.sourceDrawItemCount = sourceDrawItemCount;
    plan.dependencyDrawItemInspectionCount = dependencyDrawItemInspectionCount;
    plan.fullWorldBoundsMin = fullWorldBoundsMin;
    plan.fullWorldBoundsMax = fullWorldBoundsMax;
    plan.hasFullWorldBounds = hasFullWorldBounds;
    plan.truncatedForPendingResources = false;
}

struct PrefabPreviewResourcePumpState
{
    std::deque<std::string> unresolvedMeshPaths;
    std::deque<std::string> unresolvedMaterialPaths;
    std::deque<std::string> materialsAwaitingTextures;
    std::unordered_set<std::string> meshPathsToPump;
    std::unordered_set<std::string> materialPathsToPump;
    std::unordered_set<std::string> requestedTexturePaths;
    std::unordered_map<std::string, NLS::Render::Resources::Mesh*> resolvedMeshes;
    std::unordered_map<std::string, NLS::Render::Resources::Material*> resolvedMaterials;
    std::unordered_map<
        std::string,
        NLS::Core::ResourceManagement::ResourceHandle<NLS::Render::Resources::Mesh>> meshHandles;
    std::unordered_map<
        std::string,
        NLS::Core::ResourceManagement::ResourceHandle<NLS::Render::Resources::Material>> materialHandles;
    const NLS::Core::ResourceManagement::MeshManager* meshManager = nullptr;
    const NLS::Core::ResourceManagement::MaterialManager* materialManager = nullptr;
    uint64_t meshManagerInstanceId = 0u;
    uint64_t materialManagerInstanceId = 0u;
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry* resourceLifetimeRegistry = nullptr;
    std::string ownerToken;
    bool resourcePlanTruncated = false;
    std::string terminalDiagnostic;
};

std::future<std::optional<PersistentPrefabPreviewProxy>> SchedulePersistentPrefabPreviewProxyBuild(
    AssetThumbnailRequest request,
    PreviewRenderableSnapshot snapshot,
    PrefabPreviewResourcePlan sourcePlan)
{
    struct JobState
    {
        std::promise<std::optional<PersistentPrefabPreviewProxy>> promise;
        AssetThumbnailRequest request;
        PreviewRenderableSnapshot snapshot;
        PrefabPreviewResourcePlan sourcePlan;
    };

    auto state = std::make_unique<JobState>();
    state->request = std::move(request);
    state->snapshot = std::move(snapshot);
    state->sourcePlan = std::move(sourcePlan);
    auto future = state->promise.get_future();
    auto* statePtr = state.release();

    NLS::Base::Jobs::BackgroundJobDesc desc {};
    desc.userData = statePtr;
    desc.debugName = "EditorThumbnailPreviewRenderer.BuildPersistentPrefabProxy";
    desc.priority = NLS::Base::Jobs::JobPriority::Normal;
    desc.function = [](void* userData)
    {
        std::unique_ptr<JobState> ownedState(static_cast<JobState*>(userData));
        try
        {
            ownedState->promise.set_value(BuildOrLoadPersistentPrefabPreviewProxy(
                ownedState->request,
                ownedState->snapshot,
                ownedState->sourcePlan));
        }
        catch (...)
        {
            ownedState->promise.set_exception(std::current_exception());
        }
    };
    desc.cancelUserData = statePtr;
    desc.cancelFunction = [](void* userData)
    {
        std::unique_ptr<JobState> ownedState(static_cast<JobState*>(userData));
        ownedState->promise.set_value(std::nullopt);
    };

    const auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
    if (handle.id == 0u)
    {
        std::unique_ptr<JobState> ownedState(statePtr);
        throw std::runtime_error("persistent prefab thumbnail proxy scheduling rejected");
    }
    return future;
}

struct PreparedPrefabPreview
{
    mutable PreviewRenderableSnapshot snapshot;
    mutable PrefabPreviewResourcePlan resourcePlan;
    mutable PrefabPreviewResourcePlan fullResourcePlan;
    mutable PrefabPreviewResourcePumpState resourcePumpState;
    mutable std::future<std::optional<PersistentPrefabPreviewProxy>> persistentProxyFuture;
    mutable bool usesProvisionalPlan = false;
    mutable bool provisionalPreviewPublished = false;
    mutable std::string persistentProxyDiagnostic;
    std::string diagnostic;
};

void ResetPrefabPreviewResourcePumpStateForManagers(
    PrefabPreviewResourcePumpState& state,
    const PrefabPreviewResourcePlan& resourcePlan,
    const NLS::Core::ResourceManagement::MeshManager& meshManager,
    const NLS::Core::ResourceManagement::MaterialManager& materialManager,
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry* resourceLifetimeRegistry,
    std::string ownerToken)
{
    if (state.meshManager == &meshManager &&
        state.materialManager == &materialManager &&
        state.meshManagerInstanceId == meshManager.GetInstanceId() &&
        state.materialManagerInstanceId == materialManager.GetInstanceId() &&
        state.resourceLifetimeRegistry == resourceLifetimeRegistry)
    {
        return;
    }

    state.unresolvedMeshPaths.clear();
    state.unresolvedMaterialPaths.clear();
    state.materialsAwaitingTextures.clear();
    for (const auto& path : resourcePlan.meshLoadPaths)
    {
        if (!path.empty())
            state.unresolvedMeshPaths.push_back(path);
    }
    for (const auto& path : resourcePlan.materialLoadPaths)
    {
        if (!path.empty())
            state.unresolvedMaterialPaths.push_back(path);
    }
    state.meshPathsToPump.clear();
    state.materialPathsToPump.clear();
    state.requestedTexturePaths.clear();
    state.resolvedMeshes.clear();
    state.resolvedMaterials.clear();
    state.meshHandles.clear();
    state.materialHandles.clear();
    state.resourcePlanTruncated = resourcePlan.truncatedForPendingResources;
    state.terminalDiagnostic.clear();
    state.meshManager = &meshManager;
    state.materialManager = &materialManager;
    state.meshManagerInstanceId = meshManager.GetInstanceId();
    state.materialManagerInstanceId = materialManager.GetInstanceId();
    state.resourceLifetimeRegistry = resourceLifetimeRegistry;
    state.ownerToken = std::move(ownerToken);
}

bool PrefabArtifactExceedsGpuPreviewComplexityBudget(
    const NLS::Engine::Assets::PrefabArtifact& prefab);

PreparedPrefabPreview PreparePrefabPreviewInBackground(const AssetThumbnailRequest& request)
{
    PreparedPrefabPreview prepared;
    const auto telemetryBegin = std::chrono::steady_clock::now();
    AssetDatabaseFacade database(MakeProjectEditorAssetRoots(request.projectRoot));
    auto prefab = database.LoadPrefabArtifactByAssetId(request.assetId, request.subAssetKey);
    if (!prefab.has_value())
    {
        prepared.diagnostic = "thumbnail-gpu-preview-prefab-load-failed";
    }
    else if (PrefabArtifactExceedsGpuPreviewComplexityBudget(*prefab))
    {
        prepared.diagnostic = kGpuPreviewPrefabBudgetExceededDiagnostic;
    }
    else
    {
        prepared.snapshot = BuildPreviewRenderableSnapshot(*prefab);
        if (prepared.snapshot.drawItems.empty())
        {
            prepared.diagnostic = "thumbnail-gpu-preview-prefab-renderer-missing";
        }
        else if (!PreviewSnapshotIsCompleteForGpuPrefabPreview(prepared.snapshot))
        {
            prepared.diagnostic = kGpuPreviewPrefabBudgetExceededDiagnostic;
        }
        else
        {
            prepared.resourcePlan = BuildPrefabPreviewResourcePlan(request, prepared.snapshot);
            prepared.diagnostic = prepared.resourcePlan.diagnostic;
            if (prepared.diagnostic.empty() &&
                prepared.resourcePlan.drawItems.size() > kPersistentPrefabProxyThreshold)
            {
                prepared.fullResourcePlan = prepared.resourcePlan;
                try
                {
                    prepared.persistentProxyFuture = SchedulePersistentPrefabPreviewProxyBuild(
                        request,
                        prepared.snapshot,
                        prepared.fullResourcePlan);
                    prepared.usesProvisionalPlan = true;
                    ApplyBoundedPrefabPreviewProxy(prepared.resourcePlan);
                }
                catch (const std::exception& exception)
                {
                    prepared.persistentProxyDiagnostic =
                        std::string("persistent-proxy-schedule-failed:") + exception.what();
                    NLS_LOG_WARNING(prepared.persistentProxyDiagnostic);
                    NLS::Core::Assets::RecordArtifactLoadTelemetry({
                        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects,
                        {},
                        0u,
                        prepared.persistentProxyDiagnostic
                    });
                    prepared.fullResourcePlan = {};
                }
            }
            for (const auto& path : prepared.resourcePlan.meshLoadPaths)
            {
                if (!path.empty())
                    prepared.resourcePumpState.unresolvedMeshPaths.push_back(path);
            }
            for (const auto& path : prepared.resourcePlan.materialLoadPaths)
            {
                if (!path.empty())
                    prepared.resourcePumpState.unresolvedMaterialPaths.push_back(path);
            }
            prepared.resourcePumpState.meshPathsToPump.reserve(
                kThumbnailPreviewPrefabResourceInspectionsPerTypePerFrame);
            prepared.resourcePumpState.materialPathsToPump.reserve(
                kThumbnailPreviewPrefabResourceInspectionsPerTypePerFrame);
            prepared.resourcePumpState.resolvedMeshes.reserve(
                prepared.resourcePlan.meshLoadPaths.size());
            prepared.resourcePumpState.resolvedMaterials.reserve(
                prepared.resourcePlan.materialLoadPaths.size());
            prepared.resourcePumpState.resourcePlanTruncated =
                prepared.resourcePlan.truncatedForPendingResources;
        }
    }

    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects,
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - telemetryBegin),
        prepared.snapshot.drawItems.size(),
        request.sourceAssetPath + "|" + request.subAssetKey + "|background-prepare"
    });
    return prepared;
}

std::future<PreparedPrefabPreview> SchedulePrefabPreviewPreparation(
    const AssetThumbnailRequest& request)
{
    struct JobState
    {
        std::promise<PreparedPrefabPreview> promise;
        AssetThumbnailRequest request;
    };

    auto state = std::make_unique<JobState>();
    state->request = request;
    auto future = state->promise.get_future();
    auto* statePtr = state.release();

    NLS::Base::Jobs::BackgroundJobDesc desc {};
    desc.userData = statePtr;
    desc.debugName = "EditorThumbnailPreviewRenderer.PreparePrefab";
    desc.priority = NLS::Base::Jobs::JobPriority::High;
    desc.function = [](void* userData)
    {
        std::unique_ptr<JobState> ownedState(static_cast<JobState*>(userData));
        try
        {
            ownedState->promise.set_value(PreparePrefabPreviewInBackground(ownedState->request));
        }
        catch (...)
        {
            ownedState->promise.set_exception(std::current_exception());
        }
    };
    desc.cancelUserData = statePtr;
    desc.cancelFunction = [](void* userData)
    {
        std::unique_ptr<JobState> ownedState(static_cast<JobState*>(userData));
        try
        {
            throw std::runtime_error("prefab thumbnail preparation cancelled before execution");
        }
        catch (...)
        {
            ownedState->promise.set_exception(std::current_exception());
        }
    };

    const auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
    if (handle.id == 0u)
    {
        std::unique_ptr<JobState> ownedState(statePtr);
        throw std::runtime_error("prefab thumbnail preparation scheduling rejected");
    }
    return future;
}

template <typename ResourceManager>
std::unordered_set<std::string> CollectPendingPreviewDependencyPaths(
    const std::unordered_set<std::string>& paths,
    const ResourceManager& manager)
{
    std::unordered_set<std::string> pending;
    pending.reserve(paths.size());
    for (const auto& path : paths)
    {
        if (!path.empty() && manager.IsAsyncArtifactLoadPending(path))
            pending.insert(path);
    }
    return pending;
}

template <typename ResourceManager>
size_t CountFailedPreviewDependencyPaths(
    const std::unordered_set<std::string>& paths,
    const ResourceManager& manager)
{
    size_t failedCount = 0u;
    for (const auto& path : paths)
    {
        if (!path.empty() && manager.IsAsyncArtifactLoadFailed(path))
            ++failedCount;
    }
    return failedCount;
}

std::string BuildThumbnailGpuPreviewResourcesPendingDiagnostic(
    const size_t meshCount,
    const size_t materialCount,
    const size_t textureCount,
    const bool truncated,
    const size_t failedMeshCount = 0u,
    const size_t failedMaterialCount = 0u,
    const size_t failedTextureCount = 0u)
{
    return std::string("thumbnail-gpu-preview-resources-pending") +
        "|mesh=" + std::to_string(meshCount) +
        "|material=" + std::to_string(materialCount) +
        "|texture=" + std::to_string(textureCount) +
        "|truncated=" + (truncated ? "1" : "0") +
        "|meshFailed=" + std::to_string(failedMeshCount) +
        "|materialFailed=" + std::to_string(failedMaterialCount) +
        "|textureFailed=" + std::to_string(failedTextureCount);
}

std::string BuildThumbnailGpuPreviewMeshLoadFailedDiagnostic(const size_t failedMeshCount)
{
    return std::string("thumbnail-gpu-preview-mesh-load-failed") +
        "|meshFailed=" + std::to_string(failedMeshCount);
}

std::optional<std::filesystem::path> ResolvePrefabDependencyArtifactBudgetPath(
    const AssetThumbnailRequest& request,
    const NLS::Core::Assets::AssetId& assetId,
    const std::string& artifactPath)
{
    if (artifactPath.empty() || !assetId.IsValid() || IsBuiltInPreviewResourcePath(artifactPath))
        return std::nullopt;

    const auto portableArtifactPath = NLS::Core::Assets::TryMakePortableContentArtifactPath(artifactPath);
    if (portableArtifactPath.empty())
        return std::nullopt;

    const auto rawPath = std::filesystem::path(portableArtifactPath).lexically_normal();
    for (const auto& root : MakeProjectEditorAssetRoots(request.projectRoot))
    {
        const auto artifactRoot = GetEditorAssetRootLibraryPath(root) / "Artifacts";
        const auto candidate = GetEditorAssetRootLibraryPath(root).parent_path() / rawPath;
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
    const auto yaw = yawDegrees * ThumbnailPreviewCamera::DegreesToRadians;
    const auto pitch = pitchDegrees * ThumbnailPreviewCamera::DegreesToRadians;
    const auto cy = std::cos(yaw);
    const auto sy = std::sin(yaw);
    const auto cp = std::cos(pitch);
    const auto sp = std::sin(pitch);
    return Maths::Vector3(
        sy * cp,
        sp,
        cy * cp).Normalised();
}

struct PreviewCameraPlacement
{
    Maths::Vector3 center;
    Maths::Vector3 direction;
    float radius = 0.0f;
    float distance = 0.0f;
};

PreviewCameraPlacement BuildPreviewCameraPlacement(
    const Bounds& bounds,
    const uint32_t width,
    const uint32_t height,
    const float yawDegrees,
    const float pitchDegrees,
    const float framingScale)
{
    PreviewCameraPlacement placement;
    placement.center = (bounds.min + bounds.max) * 0.5f;
    placement.direction = PreviewDirection(yawDegrees, pitchDegrees);
    const Maths::Vector3 extents = (bounds.max - bounds.min) * 0.5f;
    placement.radius = (std::max)(0.001f, extents.Length());
    const auto fovRadians = ThumbnailPreviewCamera::FieldOfViewDegrees *
        ThumbnailPreviewCamera::DegreesToRadians;
    const auto aspect = height == 0u ? 1.0f : static_cast<float>(width) / static_cast<float>(height);
    const auto halfVerticalFov = fovRadians * 0.5f;
    const auto halfHorizontalFov = std::atan(std::tan(halfVerticalFov) * (std::max)(0.001f, aspect));
    const auto distanceVertical = placement.radius / std::tan(halfVerticalFov);
    const auto distanceHorizontal = placement.radius / std::tan(halfHorizontalFov);
    placement.distance = (std::max)(distanceVertical, distanceHorizontal) * framingScale;
    return placement;
}

PreviewCameraPlacement BuildMeshPreviewCameraPlacement(
    const Bounds& bounds,
    const uint32_t width,
    const uint32_t height)
{
    return BuildPreviewCameraPlacement(
        bounds,
        width,
        height,
        ThumbnailPreviewCamera::MeshYawDegrees,
        ThumbnailPreviewCamera::MeshLookPitchDegrees,
        1.6f);
}

PreviewCameraPlacement BuildPrefabPreviewCameraPlacement(
    const Bounds& bounds,
    const uint32_t width,
    const uint32_t height)
{
    return BuildPreviewCameraPlacement(
        bounds,
        width,
        height,
        ThumbnailPreviewCamera::PrefabYawDegrees,
        ThumbnailPreviewCamera::PrefabLookPitchDegrees,
        1.18f);
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
    for (const auto& keyword : source.GetShaderLabKeywordNames())
        material->EnableKeyword(keyword);
    if (source.HasExplicitShaderLabSourcePath())
    {
        material->SetShaderLabSourcePath(source.GetShaderLabSourcePath());
        auto* forwardShader = source.ResolveShaderForLightMode("Forward");
        material->RegisterShaderLabPassShader(forwardShader != nullptr ? forwardShader : source.GetShader());
    }
    material->SetSurfaceMode(source.GetSurfaceMode());
    material->SetBlendable(source.IsBlendable());
    // Thumbnail cameras can observe imported interiors from outside their authored view.
    material->SetBackfaceCulling(false);
    material->SetFrontfaceCulling(false);
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

bool WaitForRetiredPreviewReadbacksBeforeStartingReadback()
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
    return readbacks.empty();
}

bool RetirePreviewReadback(EditorThumbnailPreviewReadbackState&& readback)
{
    if (!readback.active || readback.completion == nullptr || readback.rgbaPixels == nullptr)
        return true;

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
    {
        auto completed = std::find_if(
            readbacks.begin(),
            readbacks.end(),
            [](EditorThumbnailPreviewReadbackState& retired)
            {
                return retired.completion == nullptr || retired.completion->Poll().IsComplete();
            });
        if (completed == readbacks.end())
            return false;
        readbacks.erase(completed);
    }
    readbacks.push_back(std::move(readback));
    return true;
}
}

#if defined(NLS_ENABLE_TEST_HOOKS)
ThumbnailPreviewRenderStatsForTesting g_lastThumbnailPreviewRenderStatsForTesting;

static EditorThumbnailPreviewCameraDebugInfo BuildCameraDebugInfoForTesting(
    const PreviewCameraPlacement& placement)
{
    EditorThumbnailPreviewCameraDebugInfo info;
    info.cameraPosition = placement.center - placement.direction * placement.distance;
    info.lookDirection = placement.direction;
    info.distance = placement.distance;
    return info;
}

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

    return BuildCameraDebugInfoForTesting(BuildPrefabPreviewCameraPlacement(bounds, width, height));
}

EditorThumbnailPreviewCameraDebugInfo BuildMeshPreviewCameraDebugInfoForTesting(
    const NLS::Maths::Vector3& boundsMin,
    const NLS::Maths::Vector3& boundsMax,
    const uint32_t width,
    const uint32_t height)
{
    Bounds bounds;
    bounds.min = boundsMin;
    bounds.max = boundsMax;
    bounds.valid = true;

    return BuildCameraDebugInfoForTesting(BuildMeshPreviewCameraPlacement(bounds, width, height));
}

NLS::Maths::Vector3 GetThumbnailPreviewKeyLightDirectionForTesting()
{
    return kThumbnailPreviewKeyLightDirection.Normalised();
}

float GetThumbnailPreviewKeyLightIntensityForTesting()
{
    return kThumbnailPreviewKeyLightIntensity;
}

size_t GetThumbnailPreviewKeyLightSampleCountForTesting()
{
    return ThumbnailPreviewKeyLightSamples().size();
}

float GetThumbnailPreviewKeyLightAngularRadiusDegreesForTesting()
{
    const auto& samples = ThumbnailPreviewKeyLightSamples();
    return NLS::Maths::Vector3::AngleBetween(samples[0].direction, samples[1].direction) /
        ThumbnailPreviewCamera::DegreesToRadians;
}

float GetThumbnailPreviewKeyLightSampleIntensitySumForTesting()
{
    float intensity = 0.0f;
    for (const auto& sample : ThumbnailPreviewKeyLightSamples())
        intensity += sample.intensity;
    return intensity;
}

float GetThumbnailPreviewAmbientIntensityForTesting()
{
    return kThumbnailPreviewAmbientIntensity;
}

size_t GetThumbnailPreviewMeshPumpBudgetForTesting()
{
    return kThumbnailPreviewMeshPumpCompletionsPerFrame;
}

size_t GetThumbnailPreviewPrefabMeshRequestStartBudgetForTesting()
{
    return kThumbnailPreviewPrefabMeshRequestStartsPerFrame;
}

size_t GetThumbnailPreviewPrefabMeshPumpBudgetForTesting()
{
    return kThumbnailPreviewPrefabMeshPumpCompletionsPerFrame;
}

size_t GetThumbnailPreviewMaterialPumpBudgetForTesting()
{
    return kThumbnailPreviewMaterialPumpCompletionsPerFrame;
}

size_t GetThumbnailPreviewTexturePumpBudgetForTesting()
{
    return kThumbnailPreviewTexturePumpCompletionsPerFrame;
}

size_t GetThumbnailPreviewPrefabTexturePumpBudgetForTesting()
{
    return kThumbnailPreviewPrefabTexturePumpCompletionsPerFrame;
}

size_t GetThumbnailPreviewPrefabResourceInspectionBudgetForTesting()
{
    return kThumbnailPreviewPrefabResourceInspectionsPerTypePerFrame;
}

uint64_t GetThumbnailPreviewPrefabResourcePumpTimeBudgetMicrosForTesting()
{
    return static_cast<uint64_t>(kThumbnailPreviewPrefabResourcePumpTimeBudget.count());
}

size_t GetThumbnailPreviewPrefabSceneAssemblyBudgetForTesting()
{
    return kThumbnailPreviewPrefabSceneAssemblyMaximumBatch;
}

size_t GetThumbnailPreviewPrefabDrawItemCapacityForTesting()
{
    return kMaxGpuPreviewPrefabGraphObjects;
}

size_t GetThumbnailPreviewPrefabProxyDrawItemCapacityForTesting()
{
    return kMaxGpuPreviewPrefabProxyDrawItems;
}

size_t GetThumbnailPreviewPrefabProxyCandidateDrawItemCapacityForTesting()
{
    return kMaxGpuPreviewPrefabProxyCandidateDrawItems;
}

std::filesystem::path BuildThumbnailPreviewPrefabProxyArtifactPathForTesting(
    const AssetThumbnailRequest& request)
{
    return BuildPersistentPrefabPreviewProxyPath(request);
}

std::optional<std::filesystem::path> BuildThumbnailPreviewPrefabProxyForTesting(
    const AssetThumbnailRequest& request,
    const PreviewRenderableSnapshot& snapshot)
{
    const auto plan = BuildPrefabPreviewResourcePlan(request, snapshot);
    const auto proxy = BuildOrLoadPersistentPrefabPreviewProxy(request, snapshot, plan);
    if (!proxy.has_value() || proxy->chunks.empty())
        return std::nullopt;
    return proxy->chunks.front().meshPath;
}

std::optional<ThumbnailPreviewPrefabProxyDetailsForTesting>
BuildThumbnailPreviewPrefabProxyDetailsForTesting(
    const AssetThumbnailRequest& request,
    const PreviewRenderableSnapshot& snapshot)
{
    const auto plan = BuildPrefabPreviewResourcePlan(request, snapshot);
    const auto proxy = BuildOrLoadPersistentPrefabPreviewProxy(request, snapshot, plan);
    if (!proxy.has_value() || proxy->chunks.empty())
        return std::nullopt;

    ThumbnailPreviewPrefabProxyDetailsForTesting details;
    details.meshPaths.reserve(proxy->chunks.size());
    details.materialPaths.reserve(proxy->chunks.size());
    for (const auto& chunk : proxy->chunks)
    {
        details.meshPaths.push_back(chunk.meshPath);
        details.materialPaths.push_back(chunk.materialLoadPath);
    }
    return details;
}

std::string BuildThumbnailPreviewReadbackRequestKeyForTesting(const AssetThumbnailRequest& request)
{
    return BuildPreviewReadbackRequestKey(request);
}

bool ThumbnailPreviewMeshPathUsesArtifactLoaderForTesting(const std::string& meshPath)
{
    return ShouldLoadPreviewMeshThroughArtifactLoader(meshPath);
}

std::string ResolveThumbnailPreviewMeshLoadPathForTesting(
    const AssetThumbnailRequest& request,
    const std::string& meshPath,
    const NLS::Core::Assets::AssetId meshAssetId)
{
    return ResolvePreviewMeshLoadPath(request, meshPath, meshAssetId);
}

ThumbnailPreviewDefaultShaderSelectionForTesting SelectThumbnailPreviewDefaultShaderForTesting(
    NLS::Core::ResourceManagement::ShaderManager& shaderManager)
{
    std::string resourcePath;
    const auto* shader = ResolveThumbnailPreviewDefaultShader(shaderManager, &resourcePath);

    ThumbnailPreviewDefaultShaderSelectionForTesting selection;
    selection.resourcePath = resourcePath;
    selection.usesLegacyBuiltInStandardHlsl = ToLowerGenericPath(resourcePath) == ":shaders/standard.hlsl";
    if (shader == nullptr)
        return selection;

    selection.sourcePath = shader->GetImportedArtifactSourcePath();
    selection.subAssetKey = shader->GetImportedArtifactSubAssetKey();
    selection.lightMode = shader->GetShaderLabLightMode();
    selection.usesShaderLabStandardPbrForward = IsThumbnailPreviewDefaultShader(*shader);
    return selection;
}

bool ThumbnailPreviewSnapshotIsCompleteForGpuPrefabPreviewForTesting(
    const PreviewRenderableSnapshot& snapshot)
{
    return PreviewSnapshotIsCompleteForGpuPrefabPreview(snapshot);
}

bool ShouldDeferPrefabPreviewForResourceReadinessForTesting(
    const size_t pendingMeshResourceCount,
    const size_t pendingMaterialResourceCount,
    const size_t pendingMaterialTextureCount,
    const bool resourcePlanTruncated)
{
    return ShouldDeferPrefabPreviewForResourceReadiness(
        pendingMeshResourceCount,
        pendingMaterialResourceCount,
        pendingMaterialTextureCount,
        resourcePlanTruncated);
}

bool ShouldDeferPrefabPreviewAfterDrawPrewarmForTesting(
    const bool prewarmSupported,
    const bool prewarmComplete)
{
    return ShouldDeferPrefabPreviewAfterDrawPrewarm(
        prewarmSupported,
        prewarmComplete);
}

bool ShouldWaitForPersistentPrefabPreviewProxyForTesting(
    const bool usesProvisionalPlan,
    const bool persistentProxyReady)
{
    return ShouldWaitForPersistentPrefabPreviewProxy(
        usesProvisionalPlan,
        persistentProxyReady);
}

bool ShouldUseFullSourceBoundsForPrefabCameraForTesting(const bool usesProvisionalPlan)
{
    return ShouldUseFullSourceBoundsForPrefabCamera(usesProvisionalPlan);
}

bool ShouldPreservePrefabPreviewSceneAfterRenderAttemptForTesting(
    const std::string& diagnostic)
{
    return ShouldPreservePrefabPreviewSceneAfterRenderAttempt(diagnostic);
}

ThumbnailPreviewRenderStatsForTesting GetLastThumbnailPreviewRenderStatsForTesting()
{
    return g_lastThumbnailPreviewRenderStatsForTesting;
}

ThumbnailPreviewPrefabResourcePlanForTesting BuildThumbnailPreviewPrefabResourcePlanForTesting(
    const AssetThumbnailRequest& request,
    const PreviewRenderableSnapshot& snapshot,
    const size_t maxUnreadyDependencyAttempts)
{
    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    auto* meshManagerForBudget = maxUnreadyDependencyAttempts == SIZE_MAX ? nullptr : &meshManager;
    auto* materialManagerForBudget = maxUnreadyDependencyAttempts == SIZE_MAX ? nullptr : &materialManager;
    const auto plan = BuildPrefabPreviewResourcePlan(
        request,
        snapshot,
        meshManagerForBudget,
        materialManagerForBudget,
        maxUnreadyDependencyAttempts);
    return {
        plan.drawItems.size(),
        plan.meshLoadPaths.size(),
        plan.materialLoadPaths.size(),
        plan.dependencyDrawItemInspectionCount,
        plan.truncatedForPendingResources,
        [&plan]()
        {
            std::vector<size_t> indices;
            indices.reserve(plan.drawItems.size());
            for (const auto& item : plan.drawItems)
                indices.push_back(item.drawItemIndex);
            return indices;
        }(),
        plan.fullWorldBoundsMin,
        plan.fullWorldBoundsMax,
        plan.hasFullWorldBounds
    };
}

ThumbnailPreviewPrefabResourcePlanForTesting BuildThumbnailPreviewPrefabResourcePlanWithManagersForTesting(
    const AssetThumbnailRequest& request,
    const PreviewRenderableSnapshot& snapshot,
    NLS::Core::ResourceManagement::MeshManager& meshManager,
    NLS::Core::ResourceManagement::MaterialManager& materialManager,
    const size_t maxUnreadyDependencyAttempts)
{
    const auto plan = BuildPrefabPreviewResourcePlan(
        request,
        snapshot,
        &meshManager,
        &materialManager,
        maxUnreadyDependencyAttempts);
    return {
        plan.drawItems.size(),
        plan.meshLoadPaths.size(),
        plan.materialLoadPaths.size(),
        plan.dependencyDrawItemInspectionCount,
        plan.truncatedForPendingResources,
        [&plan]()
        {
            std::vector<size_t> indices;
            indices.reserve(plan.drawItems.size());
            for (const auto& item : plan.drawItems)
                indices.push_back(item.drawItemIndex);
            return indices;
        }(),
        plan.fullWorldBoundsMin,
        plan.fullWorldBoundsMax,
        plan.hasFullWorldBounds
    };
}
#endif

class EditorThumbnailPreviewRenderer::Impl
{
public:
    explicit Impl(NLS::Render::Context::Driver& driver)
        : m_driver(driver)
        , m_renderer(NLS::Engine::Rendering::CreateSceneRenderer(
              driver,
              NLS::Engine::Rendering::SceneRendererKind::Forward))
    {
        auto& cameraObject = m_scene.CreateEditorTransientGameObject("Thumbnail Preview Camera");
        m_camera = cameraObject.AddComponent<NLS::Engine::Components::CameraComponent>();
        m_camera->SetFov(ThumbnailPreviewCamera::FieldOfViewDegrees);
        m_camera->SetClearColor({0.0f, 0.0f, 0.0f});
        m_camera->SetFrustumGeometryCulling(false);
        m_camera->SetFrustumLightCulling(false);

        const auto& keyLightSamples = ThumbnailPreviewKeyLightSamples();
        for (size_t index = 0u; index < keyLightSamples.size(); ++index)
        {
            const auto& sample = keyLightSamples[index];
            auto& directional = m_scene.CreateEditorTransientGameObject(kThumbnailPreviewKeyLightNames[index]);
            directional.GetTransform()->SetLocalRotation(
                Maths::Quaternion::LookAt(sample.direction, Maths::Vector3::Up));
            auto* light = directional.AddComponent<NLS::Engine::Components::LightComponent>();
            light->SetLightType(NLS::Render::Settings::ELightType::DIRECTIONAL);
            light->SetIntensity(sample.intensity);
        }

        auto& ambient = m_scene.CreateEditorTransientGameObject("Thumbnail Preview Ambient");
        auto* ambientLight = ambient.AddComponent<NLS::Engine::Components::LightComponent>();
        ambientLight->SetLightType(NLS::Render::Settings::ELightType::AMBIENT_SPHERE);
        ambientLight->SetRange(10000.0f);
        ambientLight->SetIntensity(kThumbnailPreviewAmbientIntensity);

    }

    ~Impl()
    {
        NLS::Render::Context::DriverRendererAccess::CancelBackgroundPreviewPublicationRequest(m_driver);
        NLS::Render::Context::DriverRendererAccess::DrainThreadedRendering(m_driver);
        m_pendingReadback.renderInputsKeepAlive.reset();
        RetirePendingReadback();
        ReleaseTextureInterests();
        ClearPreviewObjects(false);
        PruneGlobalRetiredPreviewReadbacks();
    }

    bool Supports(const AssetThumbnailRequest& request) const
    {
        return request.kind == AssetThumbnailKind::MaterialSphere ||
            request.kind == AssetThumbnailKind::ModelPreview ||
            request.kind == AssetThumbnailKind::PrefabPreview;
    }

    bool PrewarmMaterialPreviewRenderPath(const uint32_t requestedSize)
    {
        PruneGlobalRetiredPreviewReadbacks();
        if (m_pendingReadback.active)
            return false;
        if (m_renderer == nullptr || m_camera == nullptr || m_camera->GetCamera() == nullptr)
            return false;
        const auto threadedTelemetry =
            NLS::Render::Context::DriverRendererAccess::TryGetThreadedFrameTelemetry(m_driver);
        if (threadedTelemetry.has_value() && threadedTelemetry->inFlightFrameCount != 0u)
            return false;
        if (!EDITOR_CONTEXT(editorResources))
            return false;

        auto* sphere = EDITOR_CONTEXT(editorResources)->GetMesh("Sphere");
        if (sphere == nullptr)
            return false;

        auto& defaultMaterial = DefaultMaterial();
        if (!defaultMaterial.HasShader())
            return false;

        EditorThumbnailPreviewResult result;
        result.width = std::max(1u, requestedSize);
        result.height = result.width;

        AssetThumbnailRequest warmupRequest;
        warmupRequest.kind = AssetThumbnailKind::MaterialSphere;
        warmupRequest.requestedSize = result.width;
        warmupRequest.sourceAssetPath = "Assets";
        warmupRequest.subAssetKey = "material-preview-render-warmup";

        m_activeRequestKey = BuildPreviewReadbackRequestKey(warmupRequest);
        ClearPreviewObjects(false);
        m_materialPreviewMaterial = CreateStablePreviewMaterial(defaultMaterial);
        EnsureMaterialPreviewObject(*sphere, *m_materialPreviewMaterial);
        ConfigureMaterialCamera(result.width, result.height);
        RenderCurrentPreviewScene(warmupRequest, result);
        if (result.diagnostic != "thumbnail-gpu-preview-readback-pending")
            ClearPreviewObjects(false);
        return !result.rgbaPixels.empty() ||
            result.diagnostic == "thumbnail-gpu-preview-readback-pending";
    }

    EditorThumbnailPreviewResourcePumpResult PumpResources(const AssetThumbnailRequest& request)
    {
        EditorThumbnailPreviewResourcePumpResult result;
        const auto requestKey = BuildPreviewReadbackRequestKey(request);
        m_activeRequestKey = requestKey;
        if (!m_textureInterestRequestKey.empty() && m_textureInterestRequestKey != requestKey)
            ReleaseTextureInterests();

        if (!Supports(request))
        {
            result.diagnostic = "thumbnail-gpu-preview-kind-unsupported";
            return result;
        }
        result.supported = true;
        if (!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MeshManager>() ||
            !NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MaterialManager>())
        {
            result.diagnostic = "thumbnail-gpu-preview-resource-managers-unavailable";
            return result;
        }

        auto& previewMeshManager = NLS_SERVICE(NLS::Core::ResourceManagement::MeshManager);
        auto& previewMaterialManager = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager);
        auto* previewTextureManager = NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::TextureManager>()
            ? &NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager)
            : nullptr;
        if (request.kind == AssetThumbnailKind::PrefabPreview &&
            request.assetId.IsValid() &&
            !request.subAssetKey.empty())
        {
            std::string preparedDiagnostic;
            const auto prepared = ResolvePreparedPrefabPreview(request, preparedDiagnostic);
            if (prepared == nullptr)
            {
                result.resourcesPending =
                    preparedDiagnostic.rfind("thumbnail-gpu-preview-resources-pending", 0u) == 0u;
                result.diagnostic = preparedDiagnostic.empty()
                    ? std::string("thumbnail-gpu-preview-resources-pending:prefab-prepare=1")
                    : preparedDiagnostic;
                return result;
            }
            const bool persistentProxyReady = TryPromotePersistentPrefabPreviewProxy(prepared);
            if (ShouldWaitForPersistentPrefabPreviewProxy(
                    prepared->usesProvisionalPlan,
                    persistentProxyReady))
            {
                result.resourcesPending = true;
                result.diagnostic = "thumbnail-gpu-preview-resources-pending:prefab-persistent-proxy=1";
                return result;
            }
            auto* pumpState = FindPrefabPreviewResourcePumpState(
                BuildPreviewSnapshotCacheKey(request));
            if (pumpState == nullptr)
            {
                result.diagnostic = "thumbnail-gpu-preview-prefab-pump-state-missing";
                return result;
            }
            auto* resourceLifetimeRegistry =
                NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::EditorActions>()
                    ? &EDITOR_CONTEXT(resourceLifetimeRegistry)
                    : nullptr;
            ResetPrefabPreviewResourcePumpStateForManagers(
                *pumpState,
                prepared->resourcePlan,
                previewMeshManager,
                previewMaterialManager,
                resourceLifetimeRegistry,
                "thumbnail-preview:" + BuildPreviewSnapshotCacheKey(request));
            return PumpPreparedPrefabResources(
                request,
                *pumpState,
                previewMeshManager,
                previewMaterialManager,
                previewTextureManager);
        }
        const auto requestedResourcePaths = CollectRequestedPreviewResourcePaths(request);
        const auto pumpTelemetryBegin = std::chrono::steady_clock::now();
        const auto pendingMeshPaths = CollectPendingPreviewDependencyPaths(
            requestedResourcePaths.meshPaths,
            previewMeshManager);
        const auto pendingMaterialPaths = CollectPendingPreviewDependencyPaths(
            requestedResourcePaths.materialPaths,
            previewMaterialManager);
        const auto failedMeshPathCount = CountFailedPreviewDependencyPaths(
            requestedResourcePaths.meshPaths,
            previewMeshManager);
        const auto failedMaterialPathCount = CountFailedPreviewDependencyPaths(
            requestedResourcePaths.materialPaths,
            previewMaterialManager);
        size_t pendingTexturePathCount = 0u;
        size_t failedTexturePathCount = 0u;
        const auto meshPumpTelemetryBegin = std::chrono::steady_clock::now();
        previewMeshManager.PumpAsyncLoadsForPaths(
            pendingMeshPaths,
            kThumbnailPreviewMeshPumpCompletionsPerFrame);
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMeshDependencies,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - meshPumpTelemetryBegin),
            pendingMeshPaths.size(),
            request.sourceAssetPath + "|" + request.subAssetKey
        });
        const auto materialPumpTelemetryBegin = std::chrono::steady_clock::now();
        previewMaterialManager.PumpAsyncLoadsForPaths(
            pendingMaterialPaths,
            kThumbnailPreviewMaterialPumpCompletionsPerFrame);
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialDependencies,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - materialPumpTelemetryBegin),
            pendingMaterialPaths.size(),
            request.sourceAssetPath + "|" + request.subAssetKey
        });
        if (previewTextureManager != nullptr)
        {
            auto texturePumpPaths = requestedResourcePaths.texturePaths;
            texturePumpPaths.insert(m_textureInterestPaths.begin(), m_textureInterestPaths.end());
            const auto pendingTexturePaths = CollectPendingPreviewDependencyPaths(
                texturePumpPaths,
                *previewTextureManager);
            pendingTexturePathCount = pendingTexturePaths.size();
            failedTexturePathCount = CountFailedPreviewDependencyPaths(
                texturePumpPaths,
                *previewTextureManager);
            const auto texturePumpTelemetryBegin = std::chrono::steady_clock::now();
            previewTextureManager->PumpAsyncLoadsForPaths(
                pendingTexturePaths,
                kThumbnailPreviewTexturePumpCompletionsPerFrame);
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpTextureDependencies,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - texturePumpTelemetryBegin),
                pendingTexturePathCount,
                request.sourceAssetPath + "|" + request.subAssetKey
            });
        }
        if (result.resourcesPending && result.diagnostic.empty())
        {
            result.diagnostic = BuildThumbnailGpuPreviewResourcesPendingDiagnostic(
                pendingMeshPaths.size(),
                pendingMaterialPaths.size(),
                pendingTexturePathCount,
                false,
                failedMeshPathCount,
                failedMaterialPathCount,
                failedTexturePathCount);
        }
        auto pumpTelemetryPath = request.sourceAssetPath + "|" + request.subAssetKey;
        if (result.resourcesPending && !result.diagnostic.empty())
            pumpTelemetryPath += "|diag=" + result.diagnostic;
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpDependencies,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - pumpTelemetryBegin),
            pendingMeshPaths.size() +
                pendingMaterialPaths.size() +
                pendingTexturePathCount +
                failedMeshPathCount +
                failedMaterialPathCount +
                failedTexturePathCount,
            std::move(pumpTelemetryPath)
        });
        return result;
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

        const auto prepared = TryGetPreparedPrefabPreviewFromCache(BuildPreviewSnapshotCacheKey(request));
        if (prepared == nullptr)
            return paths;

        paths.meshPaths.insert(
            prepared->resourcePlan.meshLoadPaths.begin(),
            prepared->resourcePlan.meshLoadPaths.end());
        paths.materialPaths.insert(
            prepared->resourcePlan.materialLoadPaths.begin(),
            prepared->resourcePlan.materialLoadPaths.end());
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
        if (!m_prefabPreviewSceneAssembly.requestKey.empty() &&
            m_prefabPreviewSceneAssembly.requestKey != readbackRequestKey)
        {
            ClearPreviewObjects(false);
        }
        if (!m_textureInterestRequestKey.empty() && m_textureInterestRequestKey != readbackRequestKey)
            ReleaseTextureInterests();
        if (m_pendingReadback.active)
        {
            if (m_pendingReadback.requestKey != readbackRequestKey)
            {
                if (!RetirePendingReadback())
                {
                    result.diagnostic = "thumbnail-gpu-preview-readback-pending";
                    return result;
                }
                ReleaseTextureInterests();
                ClearPreviewObjects(false);
            }
            else
            {
                const auto pollTelemetryBegin = std::chrono::steady_clock::now();
                const auto polled = PollEditorThumbnailPreviewReadback(
                    m_pendingReadback,
                    readbackRequestKey,
                    &m_driver);
                NLS::Core::Assets::RecordArtifactLoadTelemetry({
                    NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPollReadback,
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - pollTelemetryBegin),
                    static_cast<size_t>(m_pendingReadback.width) * static_cast<size_t>(m_pendingReadback.height) * 4u,
                    request.sourceAssetPath + "|" + request.subAssetKey
                });
                if (polled.status == EditorThumbnailPreviewReadbackPollStatus::Pending)
                {
                    result.diagnostic = "thumbnail-gpu-preview-readback-pending";
                    return result;
                }
                if (polled.status == EditorThumbnailPreviewReadbackPollStatus::Ready)
                {
                    ReleaseTextureInterests();
                    ClearPreviewObjects(false);
                    auto readyPreview = polled.preview;
                    readyPreview.completedPendingReadback = true;
                    return readyPreview;
                }
                if (polled.status == EditorThumbnailPreviewReadbackPollStatus::Failed ||
                    polled.status == EditorThumbnailPreviewReadbackPollStatus::DeviceLost)
                {
                    ReleaseTextureInterests();
                    ClearPreviewObjects(false);
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
        if (request.kind != AssetThumbnailKind::MaterialSphere)
            DeactivateMaterialPreviewObject();

        const auto resourcePump = PumpResources(request);
        if (!resourcePump.supported)
        {
            ClearPreviewObjects(false);
            result.diagnostic = resourcePump.diagnostic;
            return result;
        }
        if (resourcePump.resourcesPending)
        {
            result.diagnostic = resourcePump.diagnostic;
            return result;
        }
        if (!resourcePump.diagnostic.empty())
        {
            ClearPreviewObjects(false);
            result.diagnostic = resourcePump.diagnostic;
            return result;
        }

        if (request.kind != AssetThumbnailKind::PrefabPreview)
            ClearPreviewObjects(false);

        if (request.kind == AssetThumbnailKind::MaterialSphere)
            return RenderMaterialSphere(request, result);

        if (request.kind == AssetThumbnailKind::PrefabPreview)
        {
            if (RenderPrefabPreview(request, result) ||
                result.diagnostic == "thumbnail-gpu-preview-readback-pending" ||
                result.diagnostic.rfind("thumbnail-gpu-preview-resources-pending", 0u) == 0u)
            {
                return result;
            }
            ClearPreviewObjects(false);
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
        if (!DefaultMaterialReady(result))
            return result;
        {
            NLS::Base::Profiling::PerformanceStageScope resourcesScope(
                NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
                "PreparePreviewResources",
                NLS::Base::Profiling::PerformanceStageThread::Main);
            resourcesScope.AddCounter("dependencyResourceCount", meshPaths.size());
            const auto telemetryBegin = std::chrono::steady_clock::now();

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
                auto* mesh = meshManager.GetResource(ToGenericPath(meshPath), false);
                if (mesh == nullptr)
                {
                    result.diagnostic = "thumbnail-gpu-preview-resources-pending";
                    ClearPreviewObjects(false);
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
                    for (size_t slot = 0u; slot < materials.size(); ++slot)
                    {
                        if (materials[slot] != nullptr)
                            renderer->SetMaterialAtIndex(static_cast<uint32_t>(slot), *materials[slot]);
                    }
                }
                IncludeBounds(combinedBounds, mesh->GetBounds());
            }
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareResources,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - telemetryBegin),
                meshPaths.size(),
                request.sourceAssetPath + "|" + request.subAssetKey
            });
        }

        if (m_previewObjects.empty() || !combinedBounds.valid)
        {
            result.diagnostic = "thumbnail-gpu-preview-mesh-load-failed";
            ClearPreviewObjects(false);
            return result;
        }

        ConfigureCamera(combinedBounds, result.width, result.height);
        RenderCurrentPreviewScene(request, result);

        if (result.diagnostic != "thumbnail-gpu-preview-readback-pending")
            ClearPreviewObjects(false);
        return result;
    }

private:
    struct PreviewFramebufferEntry
    {
        uint16_t width = 0u;
        uint16_t height = 0u;
        uint64_t lastUsed = 0u;
        std::unique_ptr<NLS::Render::Buffers::Framebuffer> framebuffer;
        std::weak_ptr<void> activeLease;
    };

    struct AcquiredPreviewFramebuffer
    {
        NLS::Render::Buffers::Framebuffer* framebuffer = nullptr;
        std::shared_ptr<void> lease;
    };

    static constexpr size_t kMaxPreviewFramebufferPoolSize = 256u;

    AcquiredPreviewFramebuffer AcquirePreviewFramebuffer(
        const uint16_t width,
        const uint16_t height)
    {
        ++m_previewFramebufferUseClock;
        const auto existing = std::find_if(
            m_previewFramebufferPool.begin(),
            m_previewFramebufferPool.end(),
            [width, height](const PreviewFramebufferEntry& entry)
            {
                return entry.width == width && entry.height == height &&
                    entry.activeLease.expired();
            });
        if (existing != m_previewFramebufferPool.end())
        {
            auto lease = std::make_shared<uint8_t>(0u);
            existing->lastUsed = m_previewFramebufferUseClock;
            existing->activeLease = lease;
            ++m_renderTargetReuseCount;
            return { existing->framebuffer.get(), std::move(lease) };
        }

        auto framebuffer = std::make_unique<NLS::Render::Buffers::Framebuffer>(
            0u,
            0u,
            NLS::Render::RHI::TextureColorSpace::SRGB);
        framebuffer->SetOptimizedColorClearValue(0.0f, 0.0f, 0.0f, 0.0f);
        framebuffer->Resize(width, height);
        if (framebuffer->GetExplicitTextureHandle() == nullptr)
            return {};

        if (m_previewFramebufferPool.size() >= kMaxPreviewFramebufferPoolSize)
        {
            const auto leastRecentlyUsed = std::min_element(
                m_previewFramebufferPool.begin(), m_previewFramebufferPool.end(),
                [](const PreviewFramebufferEntry& left, const PreviewFramebufferEntry& right)
                {
                    if (left.activeLease.expired() != right.activeLease.expired())
                        return left.activeLease.expired();
                    return left.lastUsed < right.lastUsed;
                });
            if (leastRecentlyUsed == m_previewFramebufferPool.end() ||
                !leastRecentlyUsed->activeLease.expired())
            {
                return {};
            }
            m_previewFramebufferPool.erase(leastRecentlyUsed);
        }
        auto lease = std::make_shared<uint8_t>(0u);
        m_previewFramebufferPool.push_back({
            width,
            height,
            m_previewFramebufferUseClock,
            std::move(framebuffer),
            lease
        });
        ++m_renderTargetAllocationCount;
        return { m_previewFramebufferPool.back().framebuffer.get(), std::move(lease) };
    }

public:
    [[nodiscard]] EditorThumbnailPreviewReuseStats GetReuseStats() const
    {
        return {
            m_previewSceneUseCount,
            m_renderTargetAllocationCount,
            m_renderTargetReuseCount,
            m_previewFramebufferPool.size()
        };
    }

private:
    struct PreviewRenderInputsKeepAlive
    {
        NLS::Engine::SceneSystem::Scene* scene = nullptr;
        std::vector<NLS::Engine::GameObject*> objects;
        std::unique_ptr<NLS::Render::Resources::Material> material;
        std::vector<std::unique_ptr<NLS::Render::Resources::Material>> prefabMaterials;

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
        std::shared_ptr<const PreparedPrefabPreview> prepared;
        uint64_t lastUsed = 0u;
    };

    struct PendingPrefabPreviewPreparation
    {
        std::string key;
        std::future<PreparedPrefabPreview> future;
    };

    struct PrefabPreviewSceneAssemblyState
    {
        std::string requestKey;
        std::shared_ptr<const PreparedPrefabPreview> prepared;
        size_t nextDrawItemIndex = 0u;
        size_t nextDrawPrewarmIndex = 0u;
        size_t totalDrawPrewarmCount = 0u;
        bool drawPrewarmComplete = false;
        Bounds combinedBounds;
        std::unordered_map<std::string, std::unique_ptr<NLS::Render::Resources::Material>> stableMaterials;
    };

    bool TryPromotePersistentPrefabPreviewProxy(
        const std::shared_ptr<const PreparedPrefabPreview>& prepared)
    {
        if (prepared == nullptr || !prepared->usesProvisionalPlan)
            return true;
        if (!prepared->persistentProxyFuture.valid())
        {
            prepared->resourcePlan = std::move(prepared->fullResourcePlan);
            prepared->resourcePumpState = {};
            prepared->usesProvisionalPlan = false;
            prepared->persistentProxyDiagnostic = "persistent-proxy-future-invalid";
            NLS_LOG_WARNING(prepared->persistentProxyDiagnostic);
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects,
                {},
                0u,
                prepared->persistentProxyDiagnostic
            });
            ClearPreviewObjects(false);
            ReleaseTextureInterests();
            return true;
        }
        if (prepared->persistentProxyFuture.wait_for(std::chrono::seconds(0)) !=
            std::future_status::ready)
        {
            return false;
        }

        try
        {
            auto proxy = prepared->persistentProxyFuture.get();
            if (proxy.has_value() && !proxy->chunks.empty())
            {
                ApplyPersistentPrefabPreviewProxy(
                    prepared->snapshot,
                    prepared->resourcePlan,
                    *proxy);
                prepared->persistentProxyDiagnostic.clear();
            }
            else
            {
                prepared->resourcePlan = std::move(prepared->fullResourcePlan);
                prepared->persistentProxyDiagnostic = "persistent-proxy-build-failed-full-source-fallback";
                NLS_LOG_WARNING(prepared->persistentProxyDiagnostic);
                NLS::Core::Assets::RecordArtifactLoadTelemetry({
                    NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects,
                    {},
                    0u,
                    prepared->persistentProxyDiagnostic
                });
            }
        }
        catch (const std::exception& exception)
        {
            prepared->resourcePlan = std::move(prepared->fullResourcePlan);
            prepared->persistentProxyDiagnostic =
                std::string("persistent-proxy-build-failed-full-source-fallback:") + exception.what();
            NLS_LOG_WARNING(prepared->persistentProxyDiagnostic);
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects,
                {},
                0u,
                prepared->persistentProxyDiagnostic
            });
        }
        catch (...)
        {
            prepared->resourcePlan = std::move(prepared->fullResourcePlan);
            prepared->persistentProxyDiagnostic = "persistent-proxy-build-failed-full-source-fallback";
            NLS_LOG_WARNING(prepared->persistentProxyDiagnostic);
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects,
                {},
                0u,
                prepared->persistentProxyDiagnostic
            });
        }

        prepared->fullResourcePlan = {};
        prepared->resourcePumpState = {};
        prepared->usesProvisionalPlan = false;
        prepared->provisionalPreviewPublished = false;
        ClearPreviewObjects(false);
        ReleaseTextureInterests();
        return true;
    }

    PrefabPreviewResourcePumpState* FindPrefabPreviewResourcePumpState(const std::string& key)
    {
        for (auto& entry : m_previewSnapshotCache)
        {
            if (entry.key == key)
                return &entry.prepared->resourcePumpState;
        }
        return nullptr;
    }

    EditorThumbnailPreviewResourcePumpResult PumpPreparedPrefabResources(
        const AssetThumbnailRequest& request,
        PrefabPreviewResourcePumpState& state,
        NLS::Core::ResourceManagement::MeshManager& meshManager,
        NLS::Core::ResourceManagement::MaterialManager& materialManager,
        NLS::Core::ResourceManagement::TextureManager* textureManager)
    {
        EditorThumbnailPreviewResourcePumpResult result;
        result.supported = true;
        const auto telemetryBegin = std::chrono::steady_clock::now();
        const auto pumpDeadline = telemetryBegin + kThumbnailPreviewPrefabResourcePumpTimeBudget;
        size_t inspectedResourceCount = 0u;
        auto finalize = [&](EditorThumbnailPreviewResourcePumpResult finalized)
        {
            auto telemetryPath = request.sourceAssetPath + "|" + request.subAssetKey +
                "|bounded-prefab-resource-inspections";
            if (!finalized.diagnostic.empty())
                telemetryPath += "|diag=" + finalized.diagnostic;
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpDependencies,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - telemetryBegin),
                inspectedResourceCount,
                std::move(telemetryPath)
            });
            return finalized;
        };

        if (!state.terminalDiagnostic.empty())
        {
            result.diagnostic = state.terminalDiagnostic;
            return finalize(std::move(result));
        }

        state.meshPathsToPump.clear();
        size_t meshRequestStartCount = 0u;
        const auto meshInspectionCount = (std::min)(
            state.unresolvedMeshPaths.size(),
            kThumbnailPreviewPrefabResourceInspectionsPerTypePerFrame);
        size_t inspectedMeshCount = 0u;
        for (size_t index = 0u; index < meshInspectionCount; ++index)
        {
            if (index != 0u && std::chrono::steady_clock::now() >= pumpDeadline)
                break;
            ++inspectedMeshCount;
            auto path = std::move(state.unresolvedMeshPaths.front());
            state.unresolvedMeshPaths.pop_front();

            auto* mesh = meshManager.GetResource(path, false);
            if (mesh != nullptr)
            {
                state.resolvedMeshes[path] = mesh;
                if (state.resourceLifetimeRegistry != nullptr)
                {
                    state.meshHandles.insert_or_assign(
                        path,
                        meshManager.AcquireMeshHandle(
                            *state.resourceLifetimeRegistry,
                            state.ownerToken,
                            path,
                            NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind::Preview));
                }
                continue;
            }
            if (meshManager.IsAsyncArtifactLoadFailedExactPath(path))
            {
                state.terminalDiagnostic = BuildThumbnailGpuPreviewMeshLoadFailedDiagnostic(1u);
                break;
            }

            const bool pending = meshManager.IsAsyncArtifactLoadPendingExactPath(path);
            if (!pending &&
                meshRequestStartCount < kThumbnailPreviewPrefabMeshRequestStartsPerFrame)
            {
                ++meshRequestStartCount;
                mesh = ResolvePreviewMesh(meshManager, path);
            }
            if (mesh != nullptr)
            {
                state.resolvedMeshes[path] = mesh;
                if (state.resourceLifetimeRegistry != nullptr)
                {
                    state.meshHandles.insert_or_assign(
                        path,
                        meshManager.AcquireMeshHandle(
                            *state.resourceLifetimeRegistry,
                            state.ownerToken,
                            path,
                            NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind::Preview));
                }
                continue;
            }
            if (meshManager.IsAsyncArtifactLoadFailedExactPath(path))
            {
                state.terminalDiagnostic = BuildThumbnailGpuPreviewMeshLoadFailedDiagnostic(1u);
                break;
            }

            state.unresolvedMeshPaths.push_back(path);
            if (meshManager.IsAsyncArtifactLoadPendingExactPath(path))
                state.meshPathsToPump.insert(std::move(path));
        }
        inspectedResourceCount += inspectedMeshCount;

        if (!state.meshPathsToPump.empty())
        {
            meshManager.PumpAsyncLoadsForExactPaths(
                state.meshPathsToPump,
                kThumbnailPreviewPrefabMeshPumpCompletionsPerFrame);
            for (const auto& path : state.meshPathsToPump)
            {
                if (!meshManager.IsAsyncArtifactLoadFailedExactPath(path))
                    continue;
                state.terminalDiagnostic = BuildThumbnailGpuPreviewMeshLoadFailedDiagnostic(1u);
                break;
            }
        }
        if (!state.terminalDiagnostic.empty())
        {
            result.diagnostic = state.terminalDiagnostic;
            return finalize(std::move(result));
        }

        state.materialPathsToPump.clear();
        const auto materialInspectionCount = (std::min)(
            state.unresolvedMaterialPaths.size(),
            kThumbnailPreviewPrefabResourceInspectionsPerTypePerFrame);
        size_t inspectedMaterialCount = 0u;
        for (size_t index = 0u;
            index < materialInspectionCount &&
            (inspectedResourceCount == 0u || std::chrono::steady_clock::now() < pumpDeadline);
            ++index)
        {
            ++inspectedMaterialCount;
            auto path = std::move(state.unresolvedMaterialPaths.front());
            state.unresolvedMaterialPaths.pop_front();

            auto* material = materialManager.GetResource(path, false);
            if (material == nullptr && materialManager.IsAsyncArtifactLoadFailed(path))
            {
                state.resolvedMaterials[path] = nullptr;
                continue;
            }
            const bool pending = material == nullptr && materialManager.IsAsyncArtifactLoadPending(path);
            if (material == nullptr && !pending)
                material = ResolvePreviewMaterial(materialManager, path);
            if (material == nullptr)
            {
                if (!materialManager.IsAsyncArtifactLoadFailed(path))
                {
                    state.unresolvedMaterialPaths.push_back(path);
                    if (materialManager.IsAsyncArtifactLoadPending(path))
                        state.materialPathsToPump.insert(std::move(path));
                }
                else
                {
                    state.resolvedMaterials[path] = nullptr;
                }
                continue;
            }

            state.resolvedMaterials[path] = material;
            if (state.resourceLifetimeRegistry != nullptr)
            {
                state.materialHandles.insert_or_assign(
                    path,
                    materialManager.AcquireMaterialHandle(
                        *state.resourceLifetimeRegistry,
                        state.ownerToken,
                        path,
                        NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind::Preview));
            }

            if (textureManager != nullptr)
                state.materialsAwaitingTextures.push_back(path);
        }
        inspectedResourceCount += inspectedMaterialCount;
        if (!state.materialPathsToPump.empty())
        {
            materialManager.PumpAsyncLoadsForPaths(
                state.materialPathsToPump,
                kThumbnailPreviewMaterialPumpCompletionsPerFrame);
        }

        size_t pendingTexturePathCount = 0u;
        size_t failedTexturePathCount = 0u;
        if (textureManager != nullptr)
        {
            const auto textureMaterialInspectionCount = (std::min)(
                state.materialsAwaitingTextures.size(),
                kThumbnailPreviewPrefabResourceInspectionsPerTypePerFrame);
            size_t inspectedTextureMaterialCount = 0u;
            for (size_t index = 0u;
                index < textureMaterialInspectionCount &&
                (inspectedResourceCount == 0u || std::chrono::steady_clock::now() < pumpDeadline);
                ++index)
            {
                ++inspectedTextureMaterialCount;
                auto path = std::move(state.materialsAwaitingTextures.front());
                state.materialsAwaitingTextures.pop_front();

                NLS::Render::Resources::Material* material = nullptr;
                if (const auto handle = state.materialHandles.find(path);
                    handle != state.materialHandles.end())
                {
                    material = handle->second.Get();
                }
                else if (const auto resolved = state.resolvedMaterials.find(path);
                    resolved != state.resolvedMaterials.end())
                {
                    material = resolved->second;
                }
                if (material == nullptr)
                    continue;

                state.requestedTexturePaths.clear();
                const bool texturesReady = BindReadyMaterialPreviewTextures(
                    *material,
                    m_textureInterestPaths,
                    &state.requestedTexturePaths);
                TrackRequestedTextureInterests(state.requestedTexturePaths);
                if (!texturesReady)
                    state.materialsAwaitingTextures.push_back(std::move(path));
            }
            inspectedResourceCount += inspectedTextureMaterialCount;

            const auto pendingTexturePaths = CollectPendingPreviewDependencyPaths(
                m_textureInterestPaths,
                *textureManager);
            pendingTexturePathCount = pendingTexturePaths.size();
            failedTexturePathCount = CountFailedPreviewDependencyPaths(
                m_textureInterestPaths,
                *textureManager);
            if (!pendingTexturePaths.empty())
            {
                textureManager->PumpAsyncLoadsForPaths(
                    pendingTexturePaths,
                    kThumbnailPreviewPrefabTexturePumpCompletionsPerFrame);
            }
        }

        if (!state.terminalDiagnostic.empty())
        {
            result.diagnostic = state.terminalDiagnostic;
            return finalize(std::move(result));
        }

        result.resourcesPending = ShouldDeferPrefabPreviewForResourceReadiness(
            state.unresolvedMeshPaths.size(),
            state.unresolvedMaterialPaths.size(),
            state.materialsAwaitingTextures.size(),
            state.resourcePlanTruncated);
        if (result.resourcesPending)
        {
            result.diagnostic = BuildThumbnailGpuPreviewResourcesPendingDiagnostic(
                state.unresolvedMeshPaths.size(),
                state.unresolvedMaterialPaths.size(),
                (std::max)(pendingTexturePathCount, state.materialsAwaitingTextures.size()),
                state.resourcePlanTruncated,
                0u,
                0u,
                failedTexturePathCount);
        }

        return finalize(std::move(result));
    }

    std::shared_ptr<const PreparedPrefabPreview> TryGetPreparedPrefabPreviewFromCache(
        const std::string& key)
    {
        ++m_previewSnapshotCacheClock;
        for (auto& entry : m_previewSnapshotCache)
        {
            if (entry.key != key)
                continue;

            entry.lastUsed = m_previewSnapshotCacheClock;
            return entry.prepared;
        }
        return nullptr;
    }

    std::shared_ptr<const PreparedPrefabPreview> StorePreparedPrefabPreviewInCache(
        std::string key,
        PreparedPrefabPreview prepared)
    {
        if (key.empty() || prepared.snapshot.drawItems.empty() || !prepared.diagnostic.empty())
            return nullptr;

        auto sharedPrepared = std::make_shared<const PreparedPrefabPreview>(std::move(prepared));

        ++m_previewSnapshotCacheClock;
        for (auto& entry : m_previewSnapshotCache)
        {
            if (entry.key != key)
                continue;

            entry.prepared = sharedPrepared;
            entry.lastUsed = m_previewSnapshotCacheClock;
            return sharedPrepared;
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
            sharedPrepared,
            m_previewSnapshotCacheClock
        });
        return sharedPrepared;
    }

    size_t PruneCompletedPrefabPreviewPreparations(const std::string& activeKey)
    {
        size_t prunedCount = 0u;
        for (auto iterator = m_pendingPrefabPreviewPreparations.begin();
            iterator != m_pendingPrefabPreviewPreparations.end();)
        {
            if (iterator->key == activeKey ||
                (iterator->future.valid() &&
                    iterator->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready))
            {
                ++iterator;
                continue;
            }

            iterator = m_pendingPrefabPreviewPreparations.erase(iterator);
            ++prunedCount;
        }
        return prunedCount;
    }

    std::shared_ptr<const PreparedPrefabPreview> ResolvePreparedPrefabPreview(
        const AssetThumbnailRequest& request,
        std::string& diagnostic)
    {
        const auto cacheKey = BuildPreviewSnapshotCacheKey(request);
        if (auto cached = TryGetPreparedPrefabPreviewFromCache(cacheKey))
            return cached;

        auto pending = std::find_if(
            m_pendingPrefabPreviewPreparations.begin(),
            m_pendingPrefabPreviewPreparations.end(),
            [&cacheKey](const PendingPrefabPreviewPreparation& entry)
            {
                return entry.key == cacheKey;
            });
        if (pending != m_pendingPrefabPreviewPreparations.end())
        {
            if (pending->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            {
                diagnostic = "thumbnail-gpu-preview-resources-pending:prefab-prepare=1";
                return nullptr;
            }

            auto future = std::move(pending->future);
            m_pendingPrefabPreviewPreparations.erase(pending);
            try
            {
                auto prepared = future.get();
                diagnostic = prepared.diagnostic;
                if (!diagnostic.empty())
                    return nullptr;
                return StorePreparedPrefabPreviewInCache(cacheKey, std::move(prepared));
            }
            catch (const std::exception& exception)
            {
                diagnostic = std::string("thumbnail-gpu-preview-prefab-prepare-failed:") + exception.what();
                return nullptr;
            }
            catch (...)
            {
                diagnostic = "thumbnail-gpu-preview-prefab-prepare-failed";
                return nullptr;
            }
        }

        const auto pruneBegin = std::chrono::steady_clock::now();
        const auto prunedPreparationCount = PruneCompletedPrefabPreviewPreparations(cacheKey);
        if (prunedPreparationCount != 0u)
        {
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - pruneBegin),
                prunedPreparationCount,
                request.sourceAssetPath + "|" + request.subAssetKey + "|pruned-obsolete-preparations"
            });
        }

        if (!NLS::Base::Jobs::IsJobSystemInitialized())
        {
            diagnostic = "thumbnail-gpu-preview-resources-pending:prefab-prepare-job-system=0";
            return nullptr;
        }

        if (m_pendingPrefabPreviewPreparations.size() >= kMaxPendingPrefabPreviewPreparations)
        {
            diagnostic = "thumbnail-gpu-preview-resources-pending:prefab-prepare-capacity=1";
            return nullptr;
        }

        try
        {
            m_pendingPrefabPreviewPreparations.push_back({
                cacheKey,
                SchedulePrefabPreviewPreparation(request)
            });
            diagnostic = "thumbnail-gpu-preview-resources-pending:prefab-prepare=1";
        }
        catch (const std::exception& exception)
        {
            diagnostic = std::string("thumbnail-gpu-preview-prefab-prepare-schedule-failed:") + exception.what();
        }
        return nullptr;
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
        std::unordered_set<std::string> requestedTexturePaths;
        NLS::Render::Resources::Mesh* sphere = nullptr;
        bool texturesReady = false;
        {
            NLS::Base::Profiling::PerformanceStageScope resourcesScope(
                NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
                "PreparePreviewResources",
                NLS::Base::Profiling::PerformanceStageThread::Main);
            resourcesScope.AddCounter("dependencyResourceCount", materialPaths.size());
            const auto telemetryBegin = std::chrono::steady_clock::now();
            {
                const auto materialTelemetryBegin = std::chrono::steady_clock::now();
                material = ResolvePreviewMaterial(materialManager, ToGenericPath(materialPaths.front()));
                if (material != nullptr)
                {
                    texturesReady = BindReadyMaterialPreviewTextures(
                        *material,
                        m_textureInterestPaths,
                        &requestedTexturePaths);
                    TrackRequestedTextureInterests(requestedTexturePaths);
                }
                NLS::Core::Assets::RecordArtifactLoadTelemetry({
                    NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareMaterialResources,
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - materialTelemetryBegin),
                    materialPaths.size(),
                    request.sourceAssetPath + "|" + request.subAssetKey
                });
            }
            if (material != nullptr && texturesReady && EDITOR_CONTEXT(editorResources))
                sphere = EDITOR_CONTEXT(editorResources)->GetMesh("Sphere");
            if (material != nullptr && texturesReady && sphere != nullptr)
            {
                const auto sceneObjectsTelemetryBegin = std::chrono::steady_clock::now();
                m_materialPreviewMaterial = CreateStablePreviewMaterial(*material);
                EnsureMaterialPreviewObject(*sphere, *m_materialPreviewMaterial);
                NLS::Core::Assets::RecordArtifactLoadTelemetry({
                    NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects,
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - sceneObjectsTelemetryBegin),
                    1u,
                    request.sourceAssetPath + "|" + request.subAssetKey
                });
            }
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareResources,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - telemetryBegin),
                materialPaths.size(),
                request.sourceAssetPath + "|" + request.subAssetKey
            });
        }
        if (material == nullptr)
        {
            result.diagnostic = "thumbnail-gpu-preview-resources-pending";
            return result;
        }
        if (!texturesReady)
        {
            result.diagnostic = "thumbnail-gpu-preview-resources-pending";
            return result;
        }

        if (!EDITOR_CONTEXT(editorResources))
        {
            result.diagnostic = "thumbnail-gpu-preview-editor-resources-unavailable";
            return result;
        }

        if (sphere == nullptr)
        {
            result.diagnostic = "thumbnail-gpu-preview-material-sphere-missing";
            return result;
        }

        ConfigureMaterialCamera(result.width, result.height);
        RenderCurrentPreviewScene(request, result);
        if (result.diagnostic != "thumbnail-gpu-preview-readback-pending")
            ClearPreviewObjects(false);
        return result;
    }

    void EnsureMaterialPreviewObject(
        NLS::Render::Resources::Mesh& sphere,
        NLS::Render::Resources::Material& material)
    {
        if (m_materialPreviewObject == nullptr ||
            m_materialPreviewMeshFilter == nullptr ||
            m_materialPreviewMeshRenderer == nullptr)
        {
            auto& object = m_scene.CreateEditorTransientGameObject("Thumbnail Preview Material Sphere");
            m_materialPreviewObject = &object;
            m_materialPreviewMeshFilter = object.AddComponent<NLS::Engine::Components::MeshFilter>();
            m_materialPreviewMeshRenderer = object.AddComponent<NLS::Engine::Components::MeshRenderer>();
            m_materialPreviewMeshRenderer->SetFrustumBehaviour(
                NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::DISABLED);
            m_materialPreviewMeshRenderer->FillEmptySlotsWithMaterial(DefaultMaterial());
            object.SetActive(false);
        }

        m_materialPreviewMeshFilter->SetMesh(&sphere);
        m_materialPreviewMeshRenderer->SetMaterialAtIndex(0u, material);
        m_materialPreviewObject->SetActive(true);
    }

    void DeactivateMaterialPreviewObject()
    {
        if (m_materialPreviewObject != nullptr)
            m_materialPreviewObject->SetActive(false);
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

        const auto prepared = TryGetPreparedPrefabPreviewFromCache(BuildPreviewSnapshotCacheKey(request));
        if (prepared == nullptr)
        {
            result.diagnostic = "thumbnail-gpu-preview-resources-pending:prefab-prepare=1";
            return false;
        }
        const auto& snapshot = prepared->snapshot;
        const auto& resourcePlan = prepared->resourcePlan;
        const auto& resourceState = prepared->resourcePumpState;
        if (!resourceState.terminalDiagnostic.empty())
        {
            result.diagnostic = resourceState.terminalDiagnostic;
            ClearPreviewObjects(false);
            return false;
        }
        if (ShouldDeferPrefabPreviewForResourceReadiness(
                resourceState.unresolvedMeshPaths.size(),
                resourceState.unresolvedMaterialPaths.size(),
                resourceState.materialsAwaitingTextures.size(),
                resourceState.resourcePlanTruncated))
        {
            result.diagnostic = BuildThumbnailGpuPreviewResourcesPendingDiagnostic(
                resourceState.unresolvedMeshPaths.size(),
                resourceState.unresolvedMaterialPaths.size(),
                resourceState.materialsAwaitingTextures.size(),
                resourceState.resourcePlanTruncated);
            ClearPreviewObjects(false);
            return false;
        }
        if (prepared->usesProvisionalPlan && prepared->provisionalPreviewPublished)
        {
            result.diagnostic = "thumbnail-gpu-preview-resources-pending:prefab-persistent-proxy=1";
            return false;
        }
        if (!DefaultMaterialReady(result))
        {
            ClearPreviewObjects(false);
            return false;
        }

        const auto requestKey = BuildPreviewReadbackRequestKey(request);
        if (m_prefabPreviewSceneAssembly.requestKey != requestKey ||
            m_prefabPreviewSceneAssembly.prepared.get() != prepared.get())
        {
            ClearPreviewObjects(false);
            m_prefabPreviewSceneAssembly.requestKey = requestKey;
            m_prefabPreviewSceneAssembly.prepared = prepared;
        }
        auto& assembly = m_prefabPreviewSceneAssembly;

        NLS::Base::Profiling::PerformanceStageScope resourcesScope(
            NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
            "PreparePreviewResources",
            NLS::Base::Profiling::PerformanceStageThread::Main);
        const auto telemetryBegin = std::chrono::steady_clock::now();
        resourcesScope.AddCounter(
            "dependencyResourceCount",
            resourcePlan.meshLoadPaths.size() + resourcePlan.materialLoadPaths.size());
        resourcesScope.AddCounter("sourceDrawItemCount", resourcePlan.sourceDrawItemCount);
        resourcesScope.AddCounter("drawItemCount", resourcePlan.drawItems.size());
        resourcesScope.AddCounter("uniqueMeshLoadPathCount", resourcePlan.meshLoadPaths.size());
        resourcesScope.AddCounter("uniqueMaterialLoadPathCount", resourcePlan.materialLoadPaths.size());

        const size_t batchBegin = assembly.nextDrawItemIndex;
        const auto assemblyDeadline = telemetryBegin + kThumbnailPreviewPrefabSceneAssemblyTimeBudget;
        while (assembly.nextDrawItemIndex < resourcePlan.drawItems.size() &&
            assembly.nextDrawItemIndex - batchBegin < kThumbnailPreviewPrefabSceneAssemblyMaximumBatch)
        {
            if (assembly.nextDrawItemIndex - batchBegin >= kThumbnailPreviewPrefabSceneAssemblyMinimumBatch &&
                std::chrono::steady_clock::now() >= assemblyDeadline)
            {
                break;
            }

            const auto& planned = resourcePlan.drawItems[assembly.nextDrawItemIndex++];
            if (planned.drawItemIndex >= snapshot.drawItems.size())
                continue;

            NLS::Render::Resources::Mesh* mesh = nullptr;
            if (const auto handle = resourceState.meshHandles.find(planned.meshLoadPath);
                handle != resourceState.meshHandles.end())
            {
                mesh = handle->second.Get();
            }
            else if (const auto resolved = resourceState.resolvedMeshes.find(planned.meshLoadPath);
                resolved != resourceState.resolvedMeshes.end())
            {
                mesh = resolved->second;
            }
            if (mesh == nullptr)
            {
                result.diagnostic = "thumbnail-gpu-preview-resources-pending:resolved-mesh-cache=1";
                ClearPreviewObjects(false);
                return false;
            }
            const auto& drawItem = snapshot.drawItems[planned.drawItemIndex];

            auto& object = m_scene.CreateEditorTransientGameObject("Thumbnail Preview Prefab Draw Item");
            m_previewObjects.push_back(&object);
            object.GetTransform()->SetLocalPosition(drawItem.localPosition);
            object.GetTransform()->SetLocalRotation(drawItem.localRotation);
            object.GetTransform()->SetLocalScale(drawItem.localScale);
            auto* filter = object.AddComponent<NLS::Engine::Components::MeshFilter>();
            auto* renderer = object.AddComponent<NLS::Engine::Components::MeshRenderer>();
            filter->SetMesh(mesh);
            renderer->SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::DISABLED);
            renderer->FillEmptySlotsWithMaterial(DefaultMaterial());
            for (size_t slot = 0u; slot < planned.materialLoadPaths.size(); ++slot)
            {
                const auto& materialPath = planned.materialLoadPaths[slot];
                NLS::Render::Resources::Material* material = nullptr;
                if (const auto handle = resourceState.materialHandles.find(materialPath);
                    handle != resourceState.materialHandles.end())
                {
                    material = handle->second.Get();
                }
                else if (const auto resolved = resourceState.resolvedMaterials.find(materialPath);
                    resolved != resourceState.resolvedMaterials.end())
                {
                    material = resolved->second;
                }
                if (material != nullptr)
                {
                    auto& stableMaterial = assembly.stableMaterials[materialPath];
                    if (stableMaterial == nullptr)
                        stableMaterial = CreateStablePreviewMaterial(*material);
                    renderer->SetMaterialAtIndex(static_cast<uint32_t>(slot), *stableMaterial);
                }
            }

            IncludeWorldBounds(
                assembly.combinedBounds,
                mesh->GetBounds(),
                object.GetTransform()->GetWorldMatrix());
        }
        const size_t assembledThisFrame = assembly.nextDrawItemIndex - batchBegin;
        resourcesScope.AddCounter("drawItemsAssembledThisFrame", assembledThisFrame);
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareResources,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - telemetryBegin),
            assembledThisFrame,
            request.sourceAssetPath + "|" + request.subAssetKey + "|resolved-resource-cache|proxy=" +
                std::to_string(resourcePlan.drawItems.size()) + "/" +
                std::to_string(resourcePlan.sourceDrawItemCount) + "|assembled=" +
                std::to_string(assembly.nextDrawItemIndex) + "/" +
                std::to_string(resourcePlan.drawItems.size()) + "|persistentProxy=" +
                (prepared->persistentProxyDiagnostic.empty()
                    ? std::string("ready")
                    : prepared->persistentProxyDiagnostic)
        });

        if (assembly.nextDrawItemIndex < resourcePlan.drawItems.size())
        {
            result.diagnostic = "thumbnail-gpu-preview-resources-pending:prefab-scene-assembly=" +
                std::to_string(assembly.nextDrawItemIndex) + "/" +
                std::to_string(resourcePlan.drawItems.size());
            return false;
        }

        if (!assembly.combinedBounds.valid)
        {
            result.diagnostic = "thumbnail-gpu-preview-prefab-mesh-load-failed";
            ClearPreviewObjects(false);
            return false;
        }

        auto cameraBounds = assembly.combinedBounds;
        if (resourcePlan.hasFullWorldBounds &&
            ShouldUseFullSourceBoundsForPrefabCamera(prepared->usesProvisionalPlan))
        {
            cameraBounds.min = resourcePlan.fullWorldBoundsMin;
            cameraBounds.max = resourcePlan.fullWorldBoundsMax;
            cameraBounds.valid = true;
        }
        ConfigurePrefabCamera(cameraBounds, result.width, result.height);
        const bool publishProvisionalTextureOnly = prepared->usesProvisionalPlan;
        RenderCurrentPreviewScene(request, result, publishProvisionalTextureOnly);
        if (publishProvisionalTextureOnly && result.gpuTexture.IsValid())
            prepared->provisionalPreviewPublished = true;
        if (result.rgbaPixels.empty() &&
            (result.diagnostic == "thumbnail-gpu-preview-render-busy" ||
                result.diagnostic == "thumbnail-gpu-preview-readback-texture-unavailable" ||
                (result.diagnostic == "thumbnail-gpu-preview-readback-pending" &&
                    !m_pendingReadback.active)))
        {
            return false;
        }
        if (!ShouldPreservePrefabPreviewSceneAfterRenderAttempt(result.diagnostic))
            ClearPreviewObjects(false);
        return !result.rgbaPixels.empty();
    }

    void RenderCurrentPreviewScene(
        const AssetThumbnailRequest& request,
        EditorThumbnailPreviewResult& result,
        const bool publishProvisionalTextureOnly = false)
    {
        if (request.kind == AssetThumbnailKind::PrefabPreview &&
            !m_thumbnailRenderDocCaptureQueued &&
            std::getenv("NLS_THUMBNAIL_RENDERDOC_CAPTURE") != nullptr)
        {
            m_thumbnailRenderDocCaptureQueued =
                NLS::Render::Context::DriverUIAccess::QueueRenderDocCaptureForNextExternalOutput(
                    m_driver,
                    "PrefabThumbnail");
        }

        if (!NLS::Render::Core::ABaseRenderer::TryBeginGlobalFrameForBackgroundPreview())
        {
            result.rgbaPixels.clear();
            result.diagnostic = "thumbnail-gpu-preview-render-busy";
            return;
        }
        struct GlobalFrameGuard
        {
            ~GlobalFrameGuard()
            {
                NLS::Render::Core::ABaseRenderer::EndGlobalFrameForBackgroundPreview();
            }
        } globalFrameGuard;

        NLS::Base::Profiling::PerformanceStageScope recordScope(
            NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
            "RecordPreviewRender",
            NLS::Base::Profiling::PerformanceStageThread::Main);
        const auto recordTelemetryBegin = std::chrono::steady_clock::now();

        if (!WaitForRetiredPreviewReadbacksBeforeStartingReadback())
        {
            result.rgbaPixels.clear();
            result.diagnostic = "thumbnail-gpu-preview-readback-pending";
            return;
        }

        auto acquiredFramebuffer = AcquirePreviewFramebuffer(
            static_cast<uint16_t>(result.width),
            static_cast<uint16_t>(result.height));
        auto* framebuffer = acquiredFramebuffer.framebuffer;
        if (framebuffer == nullptr)
        {
            result.rgbaPixels.clear();
            result.diagnostic = "thumbnail-gpu-preview-readback-texture-unavailable";
            return;
        }
        ++m_previewSceneUseCount;

        using PreviewSceneDescriptor =
            NLS::Engine::Rendering::BaseSceneRenderer::SceneDescriptor;
        const auto attachPreviewSceneDescriptor = [&]()
        {
            if (m_renderer->HasDescriptor<PreviewSceneDescriptor>())
                m_renderer->RemoveDescriptor<PreviewSceneDescriptor>();
            m_renderer->AddDescriptor<PreviewSceneDescriptor>({
                m_scene,
                std::nullopt,
                nullptr,
                {},
                false,
                true
            });
        };
        attachPreviewSceneDescriptor();

        NLS::Render::Data::FrameDescriptor frameDescriptor;
        frameDescriptor.renderWidth = static_cast<uint16_t>(result.width);
        frameDescriptor.renderHeight = static_cast<uint16_t>(result.height);
        frameDescriptor.camera = m_camera->GetCamera();
        frameDescriptor.clearColorOverride = NLS::Maths::Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, framebuffer);

        const bool usesThreadedRendering =
            NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver);
        if (usesThreadedRendering &&
            request.kind == AssetThumbnailKind::PrefabPreview &&
            !m_prefabPreviewSceneAssembly.drawPrewarmComplete)
        {
            const auto prewarm = m_renderer->PrewarmBackgroundPreviewDraws(
                frameDescriptor,
                m_prefabPreviewSceneAssembly.nextDrawPrewarmIndex,
                kThumbnailPreviewPrefabSceneAssemblyMaximumBatch);
            if (prewarm.supported)
            {
                m_prefabPreviewSceneAssembly.nextDrawPrewarmIndex = prewarm.nextDrawIndex;
                m_prefabPreviewSceneAssembly.totalDrawPrewarmCount = prewarm.totalDrawCount;
                m_prefabPreviewSceneAssembly.drawPrewarmComplete = prewarm.complete;
                result.rawVisibleDrawCount = prewarm.totalDrawCount;
                if (ShouldDeferPrefabPreviewAfterDrawPrewarm(prewarm.supported, prewarm.complete))
                {
                    result.rgbaPixels.clear();
                    result.diagnostic = "thumbnail-gpu-preview-resources-pending:prefab-draw-prewarm=" +
                        std::to_string(prewarm.nextDrawIndex) + "/" +
                        std::to_string(prewarm.totalDrawCount);
                    return;
                }
            }
            else
            {
                m_prefabPreviewSceneAssembly.drawPrewarmComplete = true;
            }

            // Supported prewarming aborts its temporary renderer frame and clears
            // frame descriptors, so the real preview frame needs a fresh scene descriptor.
            attachPreviewSceneDescriptor();
        }
        EditorThumbnailPreviewReadbackState threadedReadback;
        if (usesThreadedRendering && !publishProvisionalTextureOnly)
        {
            auto readbackTexture = framebuffer->GetExplicitTextureHandle();
            if (readbackTexture == nullptr)
            {
                result.diagnostic = "thumbnail-gpu-preview-readback-texture-unavailable";
                return;
            }
            if (!WaitForRetiredPreviewReadbacksBeforeStartingReadback())
            {
                result.diagnostic = "thumbnail-gpu-preview-readback-pending";
                return;
            }

            threadedReadback.active = true;
            threadedReadback.requestKey = BuildPreviewReadbackRequestKey(request);
            threadedReadback.width = result.width;
            threadedReadback.height = result.height;
            threadedReadback.rgbaPixels = std::make_shared<std::vector<uint8_t>>(
                static_cast<size_t>(result.width) * result.height * 4u,
                0u);
            threadedReadback.postSubmitTextureReadbackState =
                std::make_shared<NLS::Render::Context::PostSubmitTextureReadbackState>();
            NLS::Render::Context::PostSubmitTextureReadbackRequest readbackRequest;
            readbackRequest.texture = std::move(readbackTexture);
            readbackRequest.width = result.width;
            readbackRequest.height = result.height;
            readbackRequest.format = NLS::Render::Settings::EPixelDataFormat::RGBA;
            readbackRequest.type = NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE;
            readbackRequest.destination = threadedReadback.rgbaPixels->data();
            readbackRequest.state = threadedReadback.postSubmitTextureReadbackState;
            readbackRequest.destinationKeepAlive = threadedReadback.rgbaPixels;
            m_renderer->SetNextFramePostSubmitTextureReadback(std::move(readbackRequest));
        }

        m_renderer->BeginFrameForBackgroundPreview(frameDescriptor);
        if (!m_renderer->IsFrameActive())
        {
            m_renderer->EndFrame();
            result.rgbaPixels.clear();
            result.diagnostic = "thumbnail-gpu-preview-render-busy";
            return;
        }
        struct RendererFrameGuard
        {
            NLS::Render::Core::ABaseRenderer* renderer = nullptr;
            ~RendererFrameGuard()
            {
                if (renderer != nullptr && renderer->IsFrameActive())
                    renderer->EndFrame();
            }
        } rendererFrameGuard { m_renderer.get() };
        m_renderer->DrawFrame();
        {
            const auto& drawStats = m_renderer->GetLastDrawCallOptimizationStats();
            result.rawVisibleDrawCount = drawStats.rawVisibleObjectCount;
            result.submittedSceneDrawCount = drawStats.submittedSceneDrawCount;
#if defined(NLS_ENABLE_TEST_HOOKS)
            g_lastThumbnailPreviewRenderStatsForTesting.rawVisibleDrawCount =
                drawStats.rawVisibleObjectCount;
            g_lastThumbnailPreviewRenderStatsForTesting.submittedSceneDrawCount =
                drawStats.submittedSceneDrawCount;
#endif
        }
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRecord,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - recordTelemetryBegin),
            static_cast<size_t>(result.width) * static_cast<size_t>(result.height) * 4u,
            request.sourceAssetPath + "|" + request.subAssetKey
        });
        {
            NLS::Base::Profiling::PerformanceStageScope submitScope(
                NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
                "SubmitPreviewRender",
                NLS::Base::Profiling::PerformanceStageThread::Main);
            const auto submitTelemetryBegin = std::chrono::steady_clock::now();
            m_renderer->EndFrame();
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewSubmit,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - submitTelemetryBegin),
            static_cast<size_t>(result.width) * static_cast<size_t>(result.height) * 4u,
            request.sourceAssetPath + "|" + request.subAssetKey
        });
        }
        if (usesThreadedRendering)
        {
            if (!m_renderer->WasLastThreadedFramePublished())
            {
                result.rgbaPixels.clear();
                result.diagnostic = "thumbnail-gpu-preview-render-busy";
                return;
            }

            result.gpuTexture = {
                framebuffer->GetExplicitTextureHandle(),
                framebuffer->GetOrCreateExplicitColorView("AssetThumbnail.Preview"),
                acquiredFramebuffer.lease,
                result.width,
                result.height
            };
            if (publishProvisionalTextureOnly)
            {
                result.rgbaPixels.clear();
                result.diagnostic =
                    "thumbnail-gpu-preview-resources-pending:prefab-persistent-proxy=1";
                return;
            }
            threadedReadback.rawVisibleDrawCount = result.rawVisibleDrawCount;
            threadedReadback.submittedSceneDrawCount = result.submittedSceneDrawCount;
            threadedReadback.renderInputsKeepAlive = CapturePreviewRenderInputsKeepAlive();
            threadedReadback.gpuTexture = result.gpuTexture;
            m_pendingReadback = std::move(threadedReadback);
            result.diagnostic = "thumbnail-gpu-preview-readback-pending";
            return;
        }

        auto readbackTexture = framebuffer->GetExplicitTextureHandle();
        if (readbackTexture == nullptr)
            readbackTexture = NLS::Render::Context::DriverRendererAccess::ResolveReadbackTexture(m_driver);
        result.gpuTexture = {
            framebuffer->GetExplicitTextureHandle(),
            framebuffer->GetOrCreateExplicitColorView("AssetThumbnail.Preview"),
            acquiredFramebuffer.lease,
            result.width,
            result.height
        };
        if (publishProvisionalTextureOnly)
        {
            result.rgbaPixels.clear();
            result.diagnostic = "thumbnail-gpu-preview-resources-pending:prefab-persistent-proxy=1";
            return;
        }
        BeginPreviewReadback(
            request,
            std::move(readbackTexture),
            CapturePreviewRenderInputsKeepAlive(),
            result);
    }

    void BeginPreviewReadback(
        const AssetThumbnailRequest& request,
        std::shared_ptr<NLS::Render::RHI::RHITexture> readbackTexture,
        std::shared_ptr<void> renderInputsKeepAlive,
        EditorThumbnailPreviewResult& result)
    {
        if (readbackTexture == nullptr)
        {
            result.diagnostic = "thumbnail-gpu-preview-readback-texture-unavailable";
            return;
        }
        if (!WaitForRetiredPreviewReadbacksBeforeStartingReadback())
        {
            result.rgbaPixels.clear();
            result.diagnostic = "thumbnail-gpu-preview-readback-pending";
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
            const auto readbackTelemetryBegin = std::chrono::steady_clock::now();
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
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewReadback,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - readbackTelemetryBegin),
                static_cast<size_t>(result.width) * static_cast<size_t>(result.height) * 4u,
                request.sourceAssetPath + "|" + request.subAssetKey
            });
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
        pendingReadback.rawVisibleDrawCount = result.rawVisibleDrawCount;
        pendingReadback.submittedSceneDrawCount = result.submittedSceneDrawCount;
        pendingReadback.rgbaPixels = std::move(readbackPixels);
        pendingReadback.completion = readback.completion;
        pendingReadback.renderInputsKeepAlive = std::move(renderInputsKeepAlive);
        pendingReadback.gpuTexture = result.gpuTexture;

        auto polled = PollEditorThumbnailPreviewReadback(pendingReadback, readbackRequestKey, &m_driver);
        if (polled.status == EditorThumbnailPreviewReadbackPollStatus::Ready)
        {
            result = std::move(polled.preview);
            result.completedPendingReadback = true;
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

    void ClearPreviewObjects(const bool drainThreadedRendering)
    {
        const auto cleanupTelemetryBegin = std::chrono::steady_clock::now();
        const size_t previewObjectCount = m_previewObjects.size();
        if (drainThreadedRendering)
            NLS::Render::Context::DriverRendererAccess::DrainThreadedRendering(m_driver);
        for (auto* object : m_previewObjects)
        {
            if (object != nullptr)
                (void)m_scene.DestroyGameObject(*object);
        }
        m_previewObjects.clear();
        DeactivateMaterialPreviewObject();
        ClearMaterialPreviewRendererBinding();
        m_materialPreviewMaterial.reset();
        m_scene.CollectGarbages();
        m_prefabPreviewSceneAssembly = {};
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewCleanup,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - cleanupTelemetryBegin),
            previewObjectCount,
            m_activeRequestKey
        });
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
        if (m_previewObjects.empty() &&
            m_materialPreviewObject == nullptr &&
            m_materialPreviewMaterial == nullptr)
            return nullptr;

        auto keepAlive = std::make_shared<PreviewRenderInputsKeepAlive>();
        keepAlive->scene = &m_scene;
        keepAlive->objects = std::move(m_previewObjects);
        m_previewObjects.clear();
        if (m_materialPreviewObject != nullptr)
        {
            keepAlive->objects.push_back(m_materialPreviewObject);
            m_materialPreviewObject = nullptr;
            m_materialPreviewMeshFilter = nullptr;
            m_materialPreviewMeshRenderer = nullptr;
        }
        keepAlive->material = std::move(m_materialPreviewMaterial);
        keepAlive->prefabMaterials.reserve(m_prefabPreviewSceneAssembly.stableMaterials.size());
        for (auto& [_, material] : m_prefabPreviewSceneAssembly.stableMaterials)
        {
            if (material != nullptr)
                keepAlive->prefabMaterials.push_back(std::move(material));
        }
        m_prefabPreviewSceneAssembly.stableMaterials.clear();
        return keepAlive;
    }

    void ClearMaterialPreviewRendererBinding()
    {
        if (m_materialPreviewMeshRenderer != nullptr)
            m_materialPreviewMeshRenderer->RemoveMaterialAtIndex(0u);
    }

    bool RetirePendingReadback()
    {
        if (!m_pendingReadback.active)
            return true;

        if (!RetirePreviewReadback(std::move(m_pendingReadback)))
            return false;
        m_pendingReadback = {};
        return true;
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
                if (!BindReadyMaterialPreviewTextures(*material, m_textureInterestPaths, &requestedTexturePaths))
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
            auto& shaderManager = NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager);
            if (auto* shader = ResolveThumbnailPreviewDefaultShader(shaderManager))
            {
                m_defaultMaterial.SetShaderLabSourcePath(shader->GetImportedArtifactSourcePath());
                m_defaultMaterial.RegisterShaderLabPassShader(shader);
                m_defaultMaterial.SetRawParameter("_BaseColor", Maths::Vector4(0.72f, 0.74f, 0.78f, 1.0f));
                m_defaultMaterial.SetRawParameter("_Metallic", 0.0f);
                m_defaultMaterial.SetRawParameter("_Roughness", 0.72f);
                m_defaultMaterial.SetRawParameter("_AmbientOcclusion", 1.0f);
                m_defaultMaterial.SetRawParameter("u_Albedo", Maths::Vector4(0.72f, 0.74f, 0.78f, 1.0f));
                m_defaultMaterial.SetRawParameter("u_Metallic", 0.0f);
                m_defaultMaterial.SetRawParameter("u_Roughness", 0.72f);
                m_defaultMaterial.SetRawParameter("u_AmbientOcclusion", 1.0f);
                m_defaultMaterial.SetRawParameter("u_EnableNormalMapping", 0.0f);
                m_defaultMaterial.SetRawParameter("u_Emissive", Maths::Vector4(0.0f, 0.0f, 0.0f, 1.0f));
                m_defaultMaterial.SetRawParameter("u_Specular", Maths::Vector4(0.0f, 0.0f, 0.0f, 1.0f));
                m_defaultMaterial.SetBackfaceCulling(false);
                m_defaultMaterial.SetFrontfaceCulling(false);
            }
        }
        return m_defaultMaterial;
    }

    bool DefaultMaterialReady(EditorThumbnailPreviewResult& result)
    {
        if (DefaultMaterial().HasShader())
            return true;
        result.diagnostic = "thumbnail-gpu-preview-resources-pending";
        return false;
    }

    void ConfigureCamera(const Bounds& bounds, const uint32_t width, const uint32_t height)
    {
        const auto placement = BuildMeshPreviewCameraPlacement(bounds, width, height);
        auto* owner = m_camera->gameobject();
        if (owner == nullptr)
            return;
        auto* transform = owner->GetTransform();
        transform->SetLocalPosition(placement.center - placement.direction * placement.distance);
        transform->SetLocalRotation(Maths::Quaternion::LookAt(placement.direction, Maths::Vector3::Up));
        m_camera->SetFov(ThumbnailPreviewCamera::FieldOfViewDegrees);
        m_camera->SetNear((std::max)(0.001f, placement.distance - placement.radius * 3.0f));
        m_camera->SetFar(placement.distance + placement.radius * 4.0f);
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
        m_camera->SetFov(ThumbnailPreviewCamera::FieldOfViewDegrees);
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
        transform->SetLocalPosition({0.0f, 0.0f, -ThumbnailPreviewCamera::MaterialDistance});
        transform->SetLocalRotation(Maths::Quaternion::Identity);
        m_camera->SetFov(ThumbnailPreviewCamera::FieldOfViewDegrees);
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
    std::vector<PreviewFramebufferEntry> m_previewFramebufferPool;
    NLS::Render::Resources::Material m_defaultMaterial;
    std::unique_ptr<NLS::Render::Resources::Material> m_materialPreviewMaterial;
    NLS::Engine::GameObject* m_materialPreviewObject = nullptr;
    NLS::Engine::Components::MeshFilter* m_materialPreviewMeshFilter = nullptr;
    NLS::Engine::Components::MeshRenderer* m_materialPreviewMeshRenderer = nullptr;
    std::vector<NLS::Engine::GameObject*> m_previewObjects;
    EditorThumbnailPreviewReadbackState m_pendingReadback;
    std::string m_activeRequestKey;
    std::unordered_set<std::string> m_textureInterestPaths;
    std::string m_textureInterestRequestKey;
    std::vector<PreviewSnapshotCacheEntry> m_previewSnapshotCache;
    std::vector<PendingPrefabPreviewPreparation> m_pendingPrefabPreviewPreparations;
    PrefabPreviewSceneAssemblyState m_prefabPreviewSceneAssembly;
    uint64_t m_previewSnapshotCacheClock = 0u;
    uint64_t m_previewFramebufferUseClock = 0u;
    uint64_t m_previewSceneUseCount = 0u;
    uint64_t m_renderTargetAllocationCount = 0u;
    uint64_t m_renderTargetReuseCount = 0u;
    bool m_thumbnailRenderDocCaptureQueued = false;
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

bool EditorThumbnailPreviewRenderer::PrewarmMaterialPreviewRenderPath(const uint32_t requestedSize)
{
    return m_impl != nullptr && m_impl->PrewarmMaterialPreviewRenderPath(requestedSize);
}

EditorThumbnailPreviewResourcePumpResult EditorThumbnailPreviewRenderer::PumpResources(
    const AssetThumbnailRequest& request)
{
    return m_impl != nullptr ? m_impl->PumpResources(request) : EditorThumbnailPreviewResourcePumpResult {};
}

EditorThumbnailPreviewResult EditorThumbnailPreviewRenderer::Render(const AssetThumbnailRequest& request)
{
    if (m_impl == nullptr)
    {
        EditorThumbnailPreviewResult result;
        result.status = ThumbnailRenderStatus::Failed;
        result.diagnostic = "thumbnail-gpu-preview-renderer-unavailable";
        return result;
    }

    auto result = m_impl->Render(request);
    if (result.gpuTexture.IsValid() || !result.rgbaPixels.empty())
    {
        result.status = ThumbnailRenderStatus::Ready;
    }
    else if (result.diagnostic == "thumbnail-gpu-preview-kind-unsupported")
    {
        result.status = ThumbnailRenderStatus::Unsupported;
    }
    else if (result.diagnostic.find("pending") == std::string::npos &&
        result.diagnostic.find("busy") == std::string::npos &&
        result.diagnostic.find("budget-exceeded") == std::string::npos)
    {
        result.status = ThumbnailRenderStatus::Failed;
    }
    return result;
}

EditorThumbnailPreviewReuseStats EditorThumbnailPreviewRenderer::GetReuseStats() const
{
    return m_impl != nullptr ? m_impl->GetReuseStats() : EditorThumbnailPreviewReuseStats {};
}

#if defined(NLS_ENABLE_TEST_HOOKS)
bool BindReadyMaterialPreviewTexturesForTesting(NLS::Render::Resources::Material& material)
{
    return BindReadyMaterialPreviewTextures(material);
}

std::unique_ptr<NLS::Render::Resources::Material> CreateStablePreviewMaterialForTesting(
    NLS::Render::Resources::Material& source)
{
    return CreateStablePreviewMaterial(source);
}
#endif

EditorThumbnailPreviewRendererAdapter::EditorThumbnailPreviewRendererAdapter(
    EditorThumbnailPreviewRenderer& renderer)
    : m_renderer(renderer)
{
}

bool EditorThumbnailPreviewRendererAdapter::Supports(const AssetThumbnailRequest& request) const
{
    return m_renderer.Supports(request);
}

EditorThumbnailPreviewResourcePumpResult EditorThumbnailPreviewRendererAdapter::PumpResources(
    const AssetThumbnailRequest& request)
{
    return m_renderer.PumpResources(request);
}

EditorThumbnailPreviewResult EditorThumbnailPreviewRendererAdapter::Render(
    const AssetThumbnailRequest& request)
{
    return m_renderer.Render(request);
}

EditorThumbnailPreviewResourcePumpResult IEditorThumbnailPreviewRenderer::PumpResources(
    const AssetThumbnailRequest&)
{
    return {};
}
}

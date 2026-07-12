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
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
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
constexpr size_t kMaxGpuPreviewPrefabPayloadBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxGpuPreviewPrefabGraphObjects = 24000u;
constexpr size_t kMaxGpuPreviewPrefabGraphProperties = 160000u;
constexpr size_t kMaxGpuPreviewPrefabResolvedAssets = 4096u;
constexpr size_t kMaxGpuPreviewPrefabDrawItems = 1024u;
constexpr size_t kMaxGpuPreviewMeshVertices = 240000u;
constexpr size_t kMaxGpuPreviewMeshIndices = 720000u;
constexpr size_t kMaxPreviewRenderableSnapshotCacheEntries = 64u;
constexpr size_t kMaxPendingPrefabPreviewPreparations = 8u;
constexpr size_t kThumbnailPreviewMeshPumpCompletionsPerFrame = 8u;
constexpr size_t kThumbnailPreviewMaterialPumpCompletionsPerFrame = 1u;
constexpr size_t kThumbnailPreviewTexturePumpCompletionsPerFrame = 8u;
constexpr size_t kThumbnailPreviewPrefabResourceInspectionsPerTypePerFrame = 32u;
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
        const auto radius = kThumbnailPreviewKeyLightAngularRadiusDegrees * kDegreesToRadians;
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
        if (texture == nullptr && activeTextureInterests.find(genericPath) == activeTextureInterests.end())
            texture = textureManager.RequestAsyncArtifact(path, true);
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

bool ShouldDeferPrefabPreviewForMeshReadiness(
    const size_t pendingMeshResourceCount,
    const bool resourcePlanTruncated)
{
    return pendingMeshResourceCount != 0u || resourcePlanTruncated;
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
};

struct PrefabPreviewResourcePlan
{
    std::vector<PrefabPreviewResourcePlanDrawItem> drawItems;
    std::unordered_set<std::string> meshLoadPaths;
    std::unordered_set<std::string> materialLoadPaths;
    std::string diagnostic;
    bool truncatedForPendingResources = false;
};

PrefabPreviewResourcePlan BuildPrefabPreviewResourcePlan(
    const AssetThumbnailRequest& request,
    const PreviewRenderableSnapshot& snapshot,
    NLS::Core::ResourceManagement::MeshManager* meshManager = nullptr,
    NLS::Core::ResourceManagement::MaterialManager* materialManager = nullptr,
    size_t maxUnreadyDependencyAttempts = SIZE_MAX)
{
    PrefabPreviewResourcePlan plan;
    plan.drawItems.reserve(snapshot.drawItems.size());

    std::unordered_map<std::string, std::optional<std::filesystem::path>> meshBudgetPathByKey;
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

    for (size_t drawItemIndex = 0u; drawItemIndex < snapshot.drawItems.size(); ++drawItemIndex)
    {
        const auto& drawItem = snapshot.drawItems[drawItemIndex];
        PrefabPreviewResourcePlanDrawItem planned;
        planned.drawItemIndex = drawItemIndex;

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
        const bool meshReady = planned.meshLoadPath.empty() ||
            meshManager == nullptr ||
            meshManager->GetResource(planned.meshLoadPath, false) != nullptr;
        if (!canIncludeDependency(meshReady))
        {
            plan.truncatedForPendingResources = true;
            return plan;
        }
        if (!planned.meshLoadPath.empty())
            plan.meshLoadPaths.insert(planned.meshLoadPath);

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

        plan.drawItems.push_back(std::move(planned));
    }

    return plan;
}

struct PrefabPreviewResourcePumpState
{
    std::deque<std::string> unresolvedMeshPaths;
    std::deque<std::string> unresolvedMaterialPaths;
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
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry* resourceLifetimeRegistry = nullptr;
    std::string ownerToken;
    bool resourcePlanTruncated = false;
    std::string terminalDiagnostic;
};

struct PreparedPrefabPreview
{
    PreviewRenderableSnapshot snapshot;
    PrefabPreviewResourcePlan resourcePlan;
    mutable PrefabPreviewResourcePumpState resourcePumpState;
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
        state.resourceLifetimeRegistry == resourceLifetimeRegistry)
    {
        return;
    }

    state.unresolvedMeshPaths.clear();
    state.unresolvedMaterialPaths.clear();
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
        else if (!PreviewSnapshotIsCompleteForGpuPrefabPreview(prepared.snapshot) ||
            prepared.snapshot.drawItems.size() > kMaxGpuPreviewPrefabDrawItems)
        {
            prepared.diagnostic = kGpuPreviewPrefabBudgetExceededDiagnostic;
        }
        else
        {
            prepared.resourcePlan = BuildPrefabPreviewResourcePlan(request, prepared.snapshot);
            prepared.diagnostic = prepared.resourcePlan.diagnostic;
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

size_t GetThumbnailPreviewKeyLightSampleCountForTesting()
{
    return ThumbnailPreviewKeyLightSamples().size();
}

float GetThumbnailPreviewKeyLightAngularRadiusDegreesForTesting()
{
    const auto& samples = ThumbnailPreviewKeyLightSamples();
    return NLS::Maths::Vector3::AngleBetween(samples[0].direction, samples[1].direction) /
        kDegreesToRadians;
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

size_t GetThumbnailPreviewMaterialPumpBudgetForTesting()
{
    return kThumbnailPreviewMaterialPumpCompletionsPerFrame;
}

size_t GetThumbnailPreviewTexturePumpBudgetForTesting()
{
    return kThumbnailPreviewTexturePumpCompletionsPerFrame;
}

size_t GetThumbnailPreviewPrefabResourceInspectionBudgetForTesting()
{
    return kThumbnailPreviewPrefabResourceInspectionsPerTypePerFrame;
}

size_t GetThumbnailPreviewPrefabDrawItemCapacityForTesting()
{
    return kMaxGpuPreviewPrefabDrawItems;
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

bool ShouldDeferPrefabPreviewForMeshReadinessForTesting(
    const size_t pendingMeshResourceCount,
    const bool resourcePlanTruncated)
{
    return ShouldDeferPrefabPreviewForMeshReadiness(
        pendingMeshResourceCount,
        resourcePlanTruncated);
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
        plan.truncatedForPendingResources
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
        plan.truncatedForPendingResources
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
        m_camera->SetFov(kUnityMeshPreviewFieldOfViewDegrees);
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
        ambientLight->SetRadius(10000.0f);
        ambientLight->SetIntensity(kThumbnailPreviewAmbientIntensity);

        m_framebuffer.SetOptimizedColorClearValue(0.0f, 0.0f, 0.0f, 0.0f);
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
                previewMaterialManager);
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
            result.diagnostic = resourcePump.diagnostic;
            return result;
        }

        ClearPreviewObjects(false);

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
                if (request.kind != AssetThumbnailKind::PrefabPreview &&
                    MeshArtifactExceedsGpuPreviewBudget(meshPath))
                {
                    result.diagnostic = kGpuPreviewMeshBudgetExceededDiagnostic;
                    ClearPreviewObjects(false);
                    return result;
                }
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
        std::shared_ptr<const PreparedPrefabPreview> prepared;
        uint64_t lastUsed = 0u;
    };

    struct PendingPrefabPreviewPreparation
    {
        std::string key;
        std::future<PreparedPrefabPreview> future;
    };

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
        NLS::Core::ResourceManagement::MaterialManager& materialManager)
    {
        EditorThumbnailPreviewResourcePumpResult result;
        result.supported = true;
        const auto telemetryBegin = std::chrono::steady_clock::now();
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
        const auto meshInspectionCount = (std::min)(
            state.unresolvedMeshPaths.size(),
            kThumbnailPreviewPrefabResourceInspectionsPerTypePerFrame);
        inspectedResourceCount += meshInspectionCount;
        for (size_t index = 0u; index < meshInspectionCount; ++index)
        {
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
            if (meshManager.IsAsyncArtifactLoadFailed(path))
            {
                state.terminalDiagnostic = BuildThumbnailGpuPreviewMeshLoadFailedDiagnostic(1u);
                break;
            }

            const bool pending = meshManager.IsAsyncArtifactLoadPending(path);
            if (!pending)
                mesh = ResolvePreviewMesh(meshManager, path);
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
            if (meshManager.IsAsyncArtifactLoadFailed(path))
            {
                state.terminalDiagnostic = BuildThumbnailGpuPreviewMeshLoadFailedDiagnostic(1u);
                break;
            }

            state.unresolvedMeshPaths.push_back(path);
            if (meshManager.IsAsyncArtifactLoadPending(path))
                state.meshPathsToPump.insert(std::move(path));
        }

        if (!state.meshPathsToPump.empty())
        {
            meshManager.PumpAsyncLoadsForPaths(
                state.meshPathsToPump,
                kThumbnailPreviewMeshPumpCompletionsPerFrame);
            for (const auto& path : state.meshPathsToPump)
            {
                if (!meshManager.IsAsyncArtifactLoadFailed(path))
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
        inspectedResourceCount += materialInspectionCount;
        for (size_t index = 0u; index < materialInspectionCount; ++index)
        {
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

            state.requestedTexturePaths.clear();
            (void)BindReadyMaterialPreviewTextures(
                *material,
                m_textureInterestPaths,
                &state.requestedTexturePaths);
            TrackRequestedTextureInterests(state.requestedTexturePaths);
        }
        if (!state.materialPathsToPump.empty())
        {
            materialManager.PumpAsyncLoadsForPaths(
                state.materialPathsToPump,
                kThumbnailPreviewMaterialPumpCompletionsPerFrame);
        }

        if (!state.terminalDiagnostic.empty())
        {
            result.diagnostic = state.terminalDiagnostic;
            return finalize(std::move(result));
        }

        result.resourcesPending = ShouldDeferPrefabPreviewForMeshReadiness(
            state.unresolvedMeshPaths.size(),
            state.resourcePlanTruncated);
        if (result.resourcesPending)
        {
            result.diagnostic = BuildThumbnailGpuPreviewResourcesPendingDiagnostic(
                state.unresolvedMeshPaths.size(),
                state.unresolvedMaterialPaths.size(),
                0u,
                state.resourcePlanTruncated);
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
        if (ShouldDeferPrefabPreviewForMeshReadiness(
                resourceState.unresolvedMeshPaths.size(),
                resourceState.resourcePlanTruncated))
        {
            result.diagnostic = BuildThumbnailGpuPreviewResourcesPendingDiagnostic(
                resourceState.unresolvedMeshPaths.size(),
                resourceState.unresolvedMaterialPaths.size(),
                0u,
                resourceState.resourcePlanTruncated);
            ClearPreviewObjects(false);
            return false;
        }
        if (!DefaultMaterialReady(result))
        {
            ClearPreviewObjects(false);
            return false;
        }

        Bounds combinedBounds;
        NLS::Base::Profiling::PerformanceStageScope resourcesScope(
            NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
            "PreparePreviewResources",
            NLS::Base::Profiling::PerformanceStageThread::Main);
        const auto telemetryBegin = std::chrono::steady_clock::now();
        resourcesScope.AddCounter(
            "dependencyResourceCount",
            resourcePlan.meshLoadPaths.size() + resourcePlan.materialLoadPaths.size());
        resourcesScope.AddCounter("drawItemCount", resourcePlan.drawItems.size());
        resourcesScope.AddCounter("uniqueMeshLoadPathCount", resourcePlan.meshLoadPaths.size());
        resourcesScope.AddCounter("uniqueMaterialLoadPathCount", resourcePlan.materialLoadPaths.size());

        for (const auto& planned : resourcePlan.drawItems)
        {
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
                    renderer->SetMaterialAtIndex(static_cast<uint32_t>(slot), *material);
            }

            IncludeWorldBounds(combinedBounds, mesh->GetBounds(), object.GetTransform()->GetWorldMatrix());
        }
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareResources,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - telemetryBegin),
            resourcePlan.drawItems.size(),
            request.sourceAssetPath + "|" + request.subAssetKey + "|resolved-resource-cache"
        });

        if (!combinedBounds.valid)
        {
            result.diagnostic = "thumbnail-gpu-preview-prefab-mesh-load-failed";
            ClearPreviewObjects(false);
            return false;
        }

        ConfigurePrefabCamera(combinedBounds, result.width, result.height);
        RenderCurrentPreviewScene(request, result);
        if (result.diagnostic != "thumbnail-gpu-preview-readback-pending")
            ClearPreviewObjects(false);
        return !result.rgbaPixels.empty();
    }

    void RenderCurrentPreviewScene(
        const AssetThumbnailRequest& request,
        EditorThumbnailPreviewResult& result)
    {
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

        if (request.kind == AssetThumbnailKind::PrefabPreview &&
            std::getenv("NLS_THUMBNAIL_RENDERDOC_CAPTURE") != nullptr)
        {
            (void)NLS::Render::Context::DriverUIAccess::QueueRenderDocCaptureForNextExternalOutput(
                m_driver,
                "PrefabThumbnail");
        }

        NLS::Base::Profiling::PerformanceStageScope recordScope(
            NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
            "RecordPreviewRender",
            NLS::Base::Profiling::PerformanceStageThread::Main);
        const auto recordTelemetryBegin = std::chrono::steady_clock::now();

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
        frameDescriptor.clearColorOverride = NLS::Maths::Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &m_framebuffer);

        const bool usesThreadedRendering =
            NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver);
        EditorThumbnailPreviewReadbackState threadedReadback;
        if (usesThreadedRendering)
        {
            auto readbackTexture = m_framebuffer.GetExplicitTextureHandle();
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

            threadedReadback.rawVisibleDrawCount = result.rawVisibleDrawCount;
            threadedReadback.submittedSceneDrawCount = result.submittedSceneDrawCount;
            threadedReadback.renderInputsKeepAlive = CapturePreviewRenderInputsKeepAlive();
            m_pendingReadback = std::move(threadedReadback);
            result.diagnostic = "thumbnail-gpu-preview-readback-pending";
            return;
        }

        auto readbackTexture = m_framebuffer.GetExplicitTextureHandle();
        if (readbackTexture == nullptr)
            readbackTexture = NLS::Render::Context::DriverRendererAccess::ResolveReadbackTexture(m_driver);
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
    NLS::Render::Buffers::Framebuffer m_framebuffer {
        0u,
        0u,
        NLS::Render::RHI::TextureColorSpace::SRGB
    };
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
    return m_impl != nullptr ? m_impl->Render(request) : EditorThumbnailPreviewResult {};
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

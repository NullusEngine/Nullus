#include "Assets/EditorThumbnailPreviewRenderer.h"

#include "Assets/AssetThumbnailService.h"
#include "Assets/AssetThumbnailPreviewCamera.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/ArtifactDatabaseManifestUtils.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetPath.h"
#include "Assets/NativeArtifactContainer.h"
#include "Assets/PreviewRenderableSnapshot.h"
#include "Assets/ResidentPrefabPreviewRegistry.h"
#include "Assets/ThumbnailPreviewProxyPool.h"
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
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Assets/TextureMipGenerator.h"
#include "Assets/ArtifactDatabase.h"
#include "Rendering/Data/FrameDescriptor.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Loaders/TextureLoader.h"
#include "SceneSystem/Scene.h"
#include "ServiceLocator.h"

#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NLS::Editor::Assets
{
namespace
{
constexpr size_t kThumbnailPreviewReadbackRingCapacity = 3u;
constexpr size_t kThumbnailPreviewDeferredReadbackPersistenceCapacity = 8u;
constexpr size_t kMaxStablePreviewMaterialCacheEntries = 256u;
constexpr uint32_t kThumbnailPreviewMaterialTextureMaxDimension = 512u;
constexpr size_t kThumbnailPreviewMaterialTextureMaxInFlight = 4u;
constexpr size_t kThumbnailPreviewMaterialTextureCacheCapacity = 64u;

enum class PreviewReadbackStoreResult
{
    Stored,
    Deferred
};

uint32_t ThumbnailReadbackPriorityRank(const ThumbnailRequestPriority priority)
{
    switch (priority)
    {
    case ThumbnailRequestPriority::Visible:
        return 4u;
    case ThumbnailRequestPriority::Inspector:
        return 3u;
    case ThumbnailRequestPriority::Prefetch:
        return 2u;
    case ThumbnailRequestPriority::Background:
        return 1u;
    }
    return 0u;
}

std::string BuildReadbackTicketIdentity(
    const std::string& requestKey,
    const uint64_t requestRevision)
{
    return requestKey + "\x1f" + std::to_string(requestRevision);
}

void RecordLegacyPollState(
    EditorThumbnailPreviewReadbackState& state,
    const std::string& requestKey,
    const EditorThumbnailPreviewReadbackPollStatus status)
{
    if (state.postSubmitTextureReadbackState != nullptr)
        return;

    const char* phase = "missing";
    switch (status)
    {
    case EditorThumbnailPreviewReadbackPollStatus::Pending:
        phase = "completion-pending";
        break;
    case EditorThumbnailPreviewReadbackPollStatus::Ready:
        phase = "completion-ready";
        break;
    case EditorThumbnailPreviewReadbackPollStatus::Failed:
        phase = "begin-failed";
        break;
    case EditorThumbnailPreviewReadbackPollStatus::DeviceLost:
        phase = "device-lost";
        break;
    case EditorThumbnailPreviewReadbackPollStatus::Superseded:
        phase = "superseded";
        break;
    case EditorThumbnailPreviewReadbackPollStatus::Missing:
        break;
    }
    if (state.lastPostSubmitTelemetryState == phase)
        return;

    state.lastPostSubmitTelemetryState = phase;
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPollReadback,
        {},
        0u,
        requestKey + "|readback-state=" + phase
    });
}

std::optional<NLS::Render::Assets::TextureArtifactData> BuildThumbnailPreviewTextureArtifact(
    NLS::Render::Assets::TextureArtifactData artifact)
{
    if (artifact.dimension != NLS::Render::RHI::TextureDimension::Texture2D ||
        artifact.arrayLayers != 1u ||
        artifact.mips.empty())
    {
        return std::nullopt;
    }

    const NLS::Render::Assets::TextureArtifactMip* selected = nullptr;
    for (const auto& mip : artifact.mips)
    {
        if (!mip.HasPixels() ||
            mip.width == 0u ||
            mip.height == 0u ||
            (std::max)(mip.width, mip.height) > kThumbnailPreviewMaterialTextureMaxDimension)
        {
            continue;
        }
        if (selected == nullptr ||
            (std::max)(mip.width, mip.height) > (std::max)(selected->width, selected->height))
        {
            selected = &mip;
        }
    }
    uint32_t reducedWidth = 0u;
    uint32_t reducedHeight = 0u;
    std::vector<uint8_t> reducedPixels;
    if (selected != nullptr)
    {
        reducedWidth = selected->width;
        reducedHeight = selected->height;
    }
    else
    {
        if (artifact.format != NLS::Render::RHI::TextureFormat::RGBA8 &&
            artifact.format != NLS::Render::RHI::TextureFormat::RGBA16F)
        {
            return std::nullopt;
        }
        const auto& baseMip = artifact.mips.front();
        if (!baseMip.HasPixels() ||
            baseMip.width != artifact.width ||
            baseMip.height != artifact.height)
        {
            return std::nullopt;
        }
        reducedWidth = baseMip.width;
        reducedHeight = baseMip.height;
        reducedPixels.assign(
            baseMip.PixelData(),
            baseMip.PixelData() + baseMip.PixelSize());
        while ((std::max)(reducedWidth, reducedHeight) > kThumbnailPreviewMaterialTextureMaxDimension)
        {
            const auto nextWidth = reducedWidth > 1u ? reducedWidth / 2u : 1u;
            const auto nextHeight = reducedHeight > 1u ? reducedHeight / 2u : 1u;
            const auto nextPixels = NLS::Render::Assets::Detail::GenerateNextTextureMip(
                artifact.format,
                NLS::Render::Assets::TextureMipIntent::Color,
                artifact.colorSpace,
                reducedPixels,
                reducedWidth,
                reducedHeight,
                nextWidth,
                nextHeight);
            if (!nextPixels.has_value())
                return std::nullopt;
            reducedPixels = std::move(*nextPixels);
            reducedWidth = nextWidth;
            reducedHeight = nextHeight;
        }
    }

    NLS::Render::Assets::TextureArtifactData reduced;
    reduced.width = reducedWidth;
    reduced.height = reducedHeight;
    reduced.depth = 1u;
    reduced.dimension = artifact.dimension;
    reduced.arrayLayers = 1u;
    reduced.format = artifact.format;
    reduced.colorSpace = artifact.colorSpace;
    reduced.targetPlatform = std::move(artifact.targetPlatform);
    reduced.buildIdentity = std::move(artifact.buildIdentity);
    reduced.encoderId = std::move(artifact.encoderId);
    reduced.encoderVersion = artifact.encoderVersion;
    reduced.backingBytes = std::move(artifact.backingBytes);
    reduced.backingStorage = std::move(artifact.backingStorage);

    NLS::Render::Assets::TextureArtifactMip mip;
    if (selected != nullptr)
    {
        mip = *selected;
    }
    else
    {
        mip.pixels = std::move(reducedPixels);
        mip.rowPitch = NLS::Render::RHI::CalculateTextureRowPitch(
            reduced.format,
            reducedWidth);
        mip.slicePitch = NLS::Render::RHI::CalculateTextureSlicePitch(
            reduced.format,
            reducedWidth,
            reducedHeight,
            1u);
    }
    mip.level = 0u;
    reduced.mips.push_back(std::move(mip));
    reduced.subresources.push_back({
        0u,
        0u,
        reducedWidth,
        reducedHeight,
        1u,
        NLS::Render::Assets::TextureArtifactCubeFace::None,
        reduced.mips.front().rowPitch,
        reduced.mips.front().slicePitch,
        0u,
        reduced.mips.front().PixelSize()
    });
    return reduced;
}

bool TryBuildThumbnailPreviewRgba8Pixels(
    const NLS::Render::Assets::TextureArtifactData& artifact,
    std::vector<uint8_t>& rgbaPixels)
{
    if (artifact.format != NLS::Render::RHI::TextureFormat::RGBA8 ||
        artifact.mips.empty())
    {
        return false;
    }

    const auto& mip = artifact.mips.front();
    if (!mip.HasPixels() || mip.width == 0u || mip.height == 0u)
        return false;

    const size_t rowBytes = static_cast<size_t>(mip.width) * 4u;
    const size_t sourceRowPitch = mip.rowPitch != 0u ? mip.rowPitch : rowBytes;
    if (sourceRowPitch < rowBytes ||
        static_cast<size_t>(mip.height) > SIZE_MAX / sourceRowPitch ||
        mip.PixelSize() < sourceRowPitch * static_cast<size_t>(mip.height))
    {
        return false;
    }

    rgbaPixels.resize(rowBytes * static_cast<size_t>(mip.height));
    const auto* source = mip.PixelData();
    if (source == nullptr)
    {
        rgbaPixels.clear();
        return false;
    }
    for (uint32_t row = 0u; row < mip.height; ++row)
    {
        std::memcpy(
            rgbaPixels.data() + static_cast<size_t>(row) * rowBytes,
            source + static_cast<size_t>(row) * sourceRowPitch,
            rowBytes);
    }
    return true;
}

bool ShouldRetainThumbnailPreviewTexturePath(
    const bool headerProbeQueued,
    const bool headerProbeInFlight,
    const bool deferredArtifactQueued,
    const bool artifactInFlight,
    const bool uploadInFlight,
    const bool resourceReady)
{
    return headerProbeQueued ||
        headerProbeInFlight ||
        deferredArtifactQueued ||
        artifactInFlight ||
        uploadInFlight ||
        resourceReady;
}

struct ThumbnailPreviewTextureArtifactJobState
{
    std::promise<std::optional<NLS::Render::Assets::TextureArtifactData>> promise;
    std::filesystem::path path;
    std::shared_ptr<std::atomic_bool> cancellationFlag = std::make_shared<std::atomic_bool>(false);
};

struct ThumbnailPreviewTextureHeaderProbeJobState
{
    std::promise<std::optional<NLS::Render::Assets::TextureArtifactHeaderPreview>> promise;
    std::filesystem::path path;
};

void SetThumbnailPreviewTexturePromiseValue(
    ThumbnailPreviewTextureArtifactJobState& state,
    std::optional<NLS::Render::Assets::TextureArtifactData> value)
{
    try
    {
        state.promise.set_value(std::move(value));
    }
    catch (...)
    {
    }
}

void SetThumbnailPreviewTextureHeaderProbePromiseValue(
    ThumbnailPreviewTextureHeaderProbeJobState& state,
    std::optional<NLS::Render::Assets::TextureArtifactHeaderPreview> value)
{
    try
    {
        state.promise.set_value(std::move(value));
    }
    catch (...)
    {
    }
}

std::future<std::optional<NLS::Render::Assets::TextureArtifactHeaderPreview>>
ScheduleThumbnailPreviewTextureHeaderProbe(const std::string& path)
{
    auto state = std::make_unique<ThumbnailPreviewTextureHeaderProbeJobState>();
    state->path = NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(path);
    auto future = state->promise.get_future();
    auto* statePtr = state.release();

    NLS::Base::Jobs::BackgroundJobDesc desc {};
    desc.userData = statePtr;
    desc.cancelUserData = statePtr;
    desc.debugName = "EditorThumbnailPreviewRenderer.ProbeTextureHeader";
    desc.priority = NLS::Base::Jobs::JobPriority::Normal;
    desc.function = [](void* userData)
    {
        std::unique_ptr<ThumbnailPreviewTextureHeaderProbeJobState> ownedState(
            static_cast<ThumbnailPreviewTextureHeaderProbeJobState*>(userData));
        try
        {
            SetThumbnailPreviewTextureHeaderProbePromiseValue(
                *ownedState,
                NLS::Render::Assets::ReadTextureArtifactHeaderPreview(
                    ownedState->path,
                    64u * 1024u));
        }
        catch (...)
        {
            SetThumbnailPreviewTextureHeaderProbePromiseValue(*ownedState, std::nullopt);
        }
    };
    desc.cancelFunction = [](void* userData)
    {
        std::unique_ptr<ThumbnailPreviewTextureHeaderProbeJobState> ownedState(
            static_cast<ThumbnailPreviewTextureHeaderProbeJobState*>(userData));
        SetThumbnailPreviewTextureHeaderProbePromiseValue(*ownedState, std::nullopt);
    };

    const auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
    if (handle.id == 0u)
    {
        std::unique_ptr<ThumbnailPreviewTextureHeaderProbeJobState> ownedState(statePtr);
        throw std::runtime_error("thumbnail texture header probe scheduling rejected");
    }
    return future;
}

std::future<std::optional<NLS::Render::Assets::TextureArtifactData>>
ScheduleThumbnailPreviewTextureArtifactLoad(const std::string& path)
{
    auto state = std::make_unique<ThumbnailPreviewTextureArtifactJobState>();
    state->path = NLS::Core::ResourceManagement::TextureManager::ResolveResourcePath(path);
    auto future = state->promise.get_future();
    auto* statePtr = state.release();

    NLS::Base::Jobs::BackgroundJobDesc desc {};
    desc.userData = statePtr;
    desc.cancelUserData = statePtr;
    desc.debugName = "EditorThumbnailPreviewRenderer.LoadReducedTexture";
    desc.priority = NLS::Base::Jobs::JobPriority::Normal;
    desc.function = [](void* userData)
    {
        std::unique_ptr<ThumbnailPreviewTextureArtifactJobState> ownedState(
            static_cast<ThumbnailPreviewTextureArtifactJobState*>(userData));
        try
        {
            auto artifact = NLS::Render::Assets::LoadTextureArtifact(
                ownedState->path,
                ownedState->cancellationFlag.get());
            if (artifact.has_value())
                SetThumbnailPreviewTexturePromiseValue(
                    *ownedState,
                    BuildThumbnailPreviewTextureArtifact(std::move(*artifact)));
            else
                SetThumbnailPreviewTexturePromiseValue(*ownedState, std::nullopt);
        }
        catch (...)
        {
            SetThumbnailPreviewTexturePromiseValue(*ownedState, std::nullopt);
        }
    };
    desc.cancelFunction = [](void* userData)
    {
        std::unique_ptr<ThumbnailPreviewTextureArtifactJobState> ownedState(
            static_cast<ThumbnailPreviewTextureArtifactJobState*>(userData));
        ownedState->cancellationFlag->store(true, std::memory_order_release);
        SetThumbnailPreviewTexturePromiseValue(*ownedState, std::nullopt);
    };

    const auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
    if (handle.id == 0u)
    {
        std::unique_ptr<ThumbnailPreviewTextureArtifactJobState> ownedState(statePtr);
        throw std::runtime_error("thumbnail reduced texture scheduling rejected");
    }
    return future;
}
}

EditorThumbnailPreviewReadbackPollResult PollEditorThumbnailPreviewReadback(
    EditorThumbnailPreviewReadbackState& state,
    const std::string& requestKey,
    const NLS::Render::Context::Driver* driver,
    const uint64_t requestRevision)
{
    EditorThumbnailPreviewReadbackPollResult result;
    if (!state.active)
        return result;

    const auto recordPostSubmitState = [&state, &requestKey](
        const char* phase,
        const bool carried,
        const bool beginAttempted,
        const bool beginSucceeded,
        const uint64_t frameId)
    {
        if (phase == nullptr || state.lastPostSubmitTelemetryState == phase)
            return;

        state.lastPostSubmitTelemetryState = phase;
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPollReadback,
            {},
            0u,
            requestKey + "|readback-state=" + phase +
                "|carried=" + std::to_string(carried ? 1 : 0) +
                "|beginAttempted=" + std::to_string(beginAttempted ? 1 : 0) +
                "|beginSucceeded=" + std::to_string(beginSucceeded ? 1 : 0) +
                "|frame=" + std::to_string(frameId)
        });
    };

    NLS::Render::RHI::RHICompletionStatusCode completionCode =
        NLS::Render::RHI::RHICompletionStatusCode::Pending;
    std::string completionMessage;
    bool hasTerminalPostSubmitResult = false;
    if (state.postSubmitTextureReadbackState != nullptr)
    {
        std::lock_guard lock(state.postSubmitTextureReadbackState->mutex);
        const bool carried = state.postSubmitTextureReadbackState->carriedIntoRenderScenePackage;
        const bool beginAttempted = state.postSubmitTextureReadbackState->beginAttempted;
        const bool beginSucceeded = state.postSubmitTextureReadbackState->beginSucceeded;
        const uint64_t frameId = state.postSubmitTextureReadbackState->renderScenePackageFrameId;
        if (!carried)
            recordPostSubmitState("request-not-carried", carried, beginAttempted, beginSucceeded, frameId);
        else if (!beginAttempted || state.postSubmitTextureReadbackState->beginInProgress)
            recordPostSubmitState("begin-not-attempted", carried, beginAttempted, beginSucceeded, frameId);
        else if (!beginSucceeded)
            recordPostSubmitState("begin-failed", carried, beginAttempted, beginSucceeded, frameId);
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
                recordPostSubmitState("completion-ready", carried, beginAttempted, beginSucceeded, frameId);
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
        if (state.postSubmitTextureReadbackState != nullptr)
        {
            std::lock_guard lock(state.postSubmitTextureReadbackState->mutex);
            recordPostSubmitState(
                "completion-pending",
                state.postSubmitTextureReadbackState->carriedIntoRenderScenePackage,
                state.postSubmitTextureReadbackState->beginAttempted,
                state.postSubmitTextureReadbackState->beginSucceeded,
                state.postSubmitTextureReadbackState->renderScenePackageFrameId);
        }
        result.status = EditorThumbnailPreviewReadbackPollStatus::Pending;
        return result;
    }

    const bool matchesRequest = state.requestKey == requestKey &&
        (requestRevision == 0u || state.requestRevision == requestRevision);
    if (matchesRequest)
    {
        result.preview.width = state.width;
        result.preview.height = state.height;
        result.preview.expectedSceneDrawCount = state.expectedSceneDrawCount;
        result.preview.rawVisibleDrawCount = state.rawVisibleDrawCount;
        result.preview.submittedSceneDrawCount = state.submittedSceneDrawCount;
        result.preview.objectDataOverflowDroppedObjectCount =
            state.objectDataOverflowDroppedObjectCount;
        result.preview.previewSnapshot = std::move(state.previewSnapshot);
        result.preview.residentPreviewPartial = state.residentPreviewPartial;
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
constexpr size_t kMaxGpuPreviewMeshVertices = 240000u;
constexpr size_t kMaxGpuPreviewMeshIndices = 720000u;
constexpr size_t kMaxPreviewRenderableSnapshotCacheEntries = 64u;
constexpr size_t kMaxPendingPrefabPreviewPreparations = 8u;
constexpr const char* kResidentSnapshotRegistrationPendingDiagnostic =
    "thumbnail-gpu-preview-resources-pending:resident-snapshot-registration";
constexpr const char* kResidentSnapshotResourcesPendingDiagnostic =
    "thumbnail-gpu-preview-resources-pending:resident-snapshot-resources";
constexpr size_t kThumbnailPreviewMeshPumpCompletionsPerFrame = 8u;
// A prefab resource continuation is already bounded by the 1 ms pump budget.
// Allow several small requests per turn so large prefabs do not spend the
// entire resource-pending deadline advancing one dependency at a time.
constexpr size_t kThumbnailPreviewPrefabMeshRequestStartsPerFrame = 4u;
constexpr size_t kThumbnailPreviewPrefabMeshPumpCompletionsPerFrame = 4u;
constexpr size_t kThumbnailPreviewMaterialPumpCompletionsPerFrame = 4u;
constexpr size_t kThumbnailPreviewTexturePumpCompletionsPerFrame = 8u;
constexpr size_t kThumbnailPreviewPrefabTexturePumpCompletionsPerFrame = 4u;
constexpr size_t kThumbnailPreviewPrefabResourceInspectionsPerTypePerFrame = 32u;
constexpr size_t kThumbnailPreviewPrefabMaterialContentionRetryFrameCount = 8u;
constexpr auto kThumbnailPreviewPrefabResourcePumpTimeBudget = std::chrono::microseconds(1000);
// Inspection can consume the frame budget before a worker future or RHI upload
// becomes ready. Retiring an already-ready result never waits and is required
// for a thumbnail continuation to converge.
constexpr bool kAllowReadyThumbnailCompletionAfterDeadline = true;
constexpr auto kThumbnailMaterialFallbackGracePeriod = std::chrono::milliseconds(250);
constexpr size_t kThumbnailPreviewPrefabSceneAssemblyMinimumBatch = 1u;
// Complete resident resources do no I/O here. Amortize the 405-object Sponza
// proxy build across a small number of bounded continuation turns.
constexpr size_t kThumbnailPreviewCompleteResidentSceneAssemblyMinimumBatch = 32u;
constexpr size_t kThumbnailPreviewPrefabSceneAssemblyMaximumBatch = 256u;
constexpr size_t kMaxSuspendedPrefabPreviewSceneAssemblies = 8u;
constexpr size_t kMaxPrefabPreviewDrawPrewarmStates = 32u;
constexpr size_t kMaxRetiredPreviewReadbacks = 32u;
constexpr const char* kGpuPreviewMeshBudgetExceededDiagnostic = "thumbnail-model-preview-budget-exceeded";
constexpr const char* kGpuPreviewMaterialBudgetExceededDiagnostic = "thumbnail-material-preview-budget-exceeded";
constexpr const char* kLargePrefabPreviewAwaitingResidentDiagnostic =
    "thumbnail-prefab-preview-awaiting-resident-load";

auto MakeThumbnailPreviewResourcePumpStopPredicate(
    const std::chrono::steady_clock::time_point deadline)
{
    // Resource inspection and another manager may already have consumed the
    // budget before this manager is reached. Do not grant a completion merely
    // because this is the first poll: manager completion can materialize a
    // large mesh/material/texture synchronously on the caller thread.
    return [deadline]()
    {
        return std::chrono::steady_clock::now() >= deadline;
    };
}

std::chrono::microseconds ThumbnailPreviewResourcePumpBudget(
    const AssetThumbnailRequest& request)
{
    constexpr uint32_t kMinimumBudgetMicroseconds = 1000u;
    constexpr uint32_t kMaximumBudgetMicroseconds = 4000u;
    return std::chrono::microseconds((std::clamp)(
        request.previewResourcePumpBudgetMicroseconds,
        kMinimumBudgetMicroseconds,
        kMaximumBudgetMicroseconds));
}

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
    // Readback slots are keyed by the visual request identity. A UI caller may
    // rebuild the same request while it is pending, which legitimately gives
    // it a newer publication revision. Keep that revision on the ticket for
    // invalidation/orphan checks, but do not create another GPU submission for
    // the same visual work.
    key += "preview-readback:v3|";
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

std::string BuildPrefabPreviewSceneAssemblyKey(const AssetThumbnailRequest& request)
{
    // Readback tickets must change when freshness changes, but the resumable
    // scene assembly belongs to the stable visual identity. The prepared
    // snapshot pointer and resource-plan revision below still invalidate the
    // assembly when the actual canonical geometry changes.
    return "preview-assembly:v1|" + BuildAssetThumbnailPresentationKey(request);
}

std::string BuildPreviewSnapshotCacheKey(const AssetThumbnailRequest& request)
{
    // A resident snapshot is already the canonical, freshness-validated
    // representation of the prefab. Path/artifact fields can legitimately
    // change while the Asset Browser database snapshot is being published;
    // keeping them in this renderer cache key would rebuild the same resource
    // plan more than once for one resident asset.
    if (request.residentPrefabPreviewSource.has_value() &&
        request.residentPrefabPreviewSource->HasIdentity())
    {
        const auto& resident = *request.residentPrefabPreviewSource;
        std::string key;
        key.reserve(
            128u + resident.runtimeCacheIdentity.size() +
            resident.freshnessFingerprint.size() +
            request.previewRendererVersion.size() +
            request.settingsFingerprint.size());
        key += "preview-snapshot:resident-v1|";
        key += ToGenericPath(request.projectRoot);
        key += '|';
        key += resident.runtimeCacheIdentity;
        key += "|fresh=";
        key += resident.freshnessFingerprint;
        key += "|renderer=";
        key += request.previewRendererVersion;
        key += "|settings=";
        key += request.settingsFingerprint;
        return key;
    }

    const bool hasFreshnessEvidence =
        !request.dependencyStamp.empty() || !request.freshnessInputs.empty();
    std::string key;
    key.reserve(192u + request.freshnessInputs.size() * 48u);
    key += "preview-snapshot:canonical-v2|";
    key += ToGenericPath(request.projectRoot);
    key += '|';
    key += request.assetId.ToString();
    key += '|';
    key += request.subAssetKey;
    if (hasFreshnessEvidence)
    {
        key += "|dep=";
        key += request.dependencyStamp;
    }
    else
    {
        // Requests without a freshness contract are only used during early
        // enumeration/diagnostics. Keep their path identity so they cannot
        // accidentally reuse a snapshot for another artifact.
        key += "|artifact=";
        key += request.artifactPath;
        key += "|source=";
        key += request.sourceAssetPath;
    }
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
    const std::string& materialPath,
    const std::string& resolvedMaterialPath = {},
    const bool allowAsyncRequest = true)
{
    if (materialPath.empty())
        return nullptr;

    // Scene loading may register the same canonical material under a source,
    // portable, or resolved artifact path. The caller caches the resolved
    // identity for the lifetime of this resource plan, so retrying a pending
    // preview does not perform another source-path resolution.
    if (auto* cached = materialManager.GetResource(materialPath, false))
        return cached;
    if (auto* cached = materialManager.FindRegisteredMaterialByResolvedArtifactPath(materialPath))
        return cached;
    if (!resolvedMaterialPath.empty())
    {
        if (auto* cached = materialManager.FindRegisteredMaterialByResolvedArtifactPath(resolvedMaterialPath))
            return cached;
    }
    else if (auto* cached = materialManager.FindRegisteredMaterialByEquivalentArtifactPath(materialPath))
        return cached;

    if (!allowAsyncRequest)
        return nullptr;

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

bool ShouldYieldPrefabMeshDependencyInspection(
    const bool meshLoadPending,
    const size_t meshRequestStartCount,
    const size_t meshRequestStartBudget)
{
    return !meshLoadPending && meshRequestStartCount >= meshRequestStartBudget;
}

bool BindReadyMaterialPreviewTextures(
    NLS::Render::Resources::Material& material,
    const std::unordered_set<std::string>& activeTextureInterests = {},
    std::unordered_set<std::string>* requestedTexturePaths = nullptr,
    const std::unordered_set<std::string>* thumbnailTexturePaths = nullptr,
    const std::unordered_map<
        std::string,
        std::unique_ptr<NLS::Render::Resources::Texture2D>>* thumbnailTextureResources = nullptr,
    std::unordered_set<std::string>* pendingThumbnailTexturePaths = nullptr,
    std::unordered_set<std::string>* unavailableTexturePaths = nullptr,
    std::unordered_map<std::string, NLS::Render::Resources::Texture2D*>* readyTextureCache = nullptr,
    std::unordered_set<std::string>* pendingResourceTexturePaths = nullptr,
    const bool allowAsyncRequest = true)
{
    const auto& texturePaths = material.GetTextureResourcePaths();
    if (texturePaths.empty())
        return true;

    bool ready = true;
    auto* textureManager = NLS::Core::ServiceLocator::Contains<
        NLS::Core::ResourceManagement::TextureManager>()
        ? &NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager)
        : nullptr;
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
        // Keep the non-blocking manager probe local to this material binding.
        // Each probe resolves the artifact spelling, checks its timestamp and
        // takes the shared async-request lock. Repeating that sequence for the
        // same texture several times in one pass turns a bounded pump into a
        // visible main-thread tail while the texture is still loading.
        using TextureAsyncProbeResult =
            NLS::Core::ResourceManagement::TextureManager::AsyncArtifactLoadProbeResult;
        std::optional<TextureAsyncProbeResult> cachedTextureProbe;
        const auto probeTextureAsyncState = [&]() -> TextureAsyncProbeResult
        {
            if (!cachedTextureProbe.has_value())
                cachedTextureProbe = textureManager->TryProbeAsyncArtifactLoad(path);
            return *cachedTextureProbe;
        };
        const auto invalidateTextureAsyncProbe = [&]()
        {
            cachedTextureProbe.reset();
        };
        const auto clearPendingResourceTexture = [&]()
        {
            if (pendingResourceTexturePaths != nullptr)
                pendingResourceTexturePaths->erase(genericPath);
        };
        const auto markPendingResourceTexture = [&]()
        {
            if (pendingResourceTexturePaths != nullptr)
                pendingResourceTexturePaths->insert(genericPath);
        };
        if (readyTextureCache != nullptr)
        {
            if (const auto cached = readyTextureCache->find(genericPath);
                cached != readyTextureCache->end() && cached->second != nullptr &&
                cached->second->GetTextureHandle() != nullptr)
            {
                material.SetRawParameter(name, cached->second);
                continue;
            }
        }
        if (thumbnailTextureResources != nullptr)
        {
            if (const auto previewTexture = thumbnailTextureResources->find(genericPath);
                previewTexture != thumbnailTextureResources->end() &&
                previewTexture->second != nullptr &&
                previewTexture->second.get() != nullptr &&
                previewTexture->second.get()->GetTextureHandle() != nullptr)
            {
                material.SetRawParameter(name, previewTexture->second.get());
                if (readyTextureCache != nullptr)
                    (*readyTextureCache)[genericPath] = previewTexture->second.get();
                continue;
            }
        }

        // A scene-owned material may have started this preview while its
        // reduced thumbnail texture was still being prepared. Once the
        // authoritative TextureManager resource becomes ready, prefer it
        // immediately instead of treating the old reduced-path marker as a
        // permanent Pending state. This is the resident-resource fast path.
        std::optional<NLS::Render::Resources::Texture2D*> cachedArtifactTexture;
        if (textureManager != nullptr)
        {
            cachedArtifactTexture = textureManager->TryGetArtifactResource(path);
            if (cachedArtifactTexture.has_value() &&
                *cachedArtifactTexture != nullptr &&
                (*cachedArtifactTexture)->GetTextureHandle() != nullptr)
            {
                material.SetRawParameter(name, *cachedArtifactTexture);
                clearPendingResourceTexture();
                if (readyTextureCache != nullptr)
                    (*readyTextureCache)[genericPath] = *cachedArtifactTexture;
                continue;
            }
        }

        if (thumbnailTexturePaths != nullptr &&
            thumbnailTexturePaths->find(genericPath) != thumbnailTexturePaths->end())
        {
            // The reduced texture is an opportunistic optimization, not a
            // readiness gate. It may still be queued behind the bounded
            // preview texture concurrency, while the authoritative manager
            // resource is already resident, can be loaded independently, or
            // is unavailable and should use the material's default sampler.
            // Keep the path for diagnostics, but continue through the
            // authoritative path below instead of leaving the whole prefab
            // in materialsAwaitingTextures indefinitely.
            if (pendingThumbnailTexturePaths != nullptr && !genericPath.empty())
                pendingThumbnailTexturePaths->insert(genericPath);
        }
        if (textureManager == nullptr)
        {
            // A preview can still be canonical when the runtime texture
            // manager is unavailable: leave the material's default sampler in
            // place and record the explicit fallback instead of retrying a
            // dependency that cannot be submitted.
            if (unavailableTexturePaths != nullptr && !genericPath.empty())
                unavailableTexturePaths->insert(genericPath);
            clearPendingResourceTexture();
            continue;
        }

        // During scene restoration the scene resolver is the owner of a
        // not-yet-visible resource. Probe and join an existing request, but do
        // not create a second thumbnail-owned request when the scene has not
        // published one yet. The caller will retry after the scene gate clears.
        if (!allowAsyncRequest &&
            (texture == nullptr || texture->GetTextureHandle() == nullptr))
        {
            const auto probe = probeTextureAsyncState();
            if (probe == TextureAsyncProbeResult::Pending)
                markPendingResourceTexture();
            else
                clearPendingResourceTexture();
            ready = false;
            continue;
        }
        if (requestedTexturePaths != nullptr && !genericPath.empty())
            requestedTexturePaths->insert(genericPath);
        const auto textureProbe = [&]()
        {
            return probeTextureAsyncState();
        };
        if (!cachedArtifactTexture.has_value())
        {
			// A non-blocking resource probe can yield because a manager lock is
			// busy. Do not confuse a completed Missing/Failed probe with a
			// dependency that can still make progress: unsupported model-local
			// paths must settle to the material's default sampler.
			const auto probe = textureProbe();
			if (probe ==
				NLS::Core::ResourceManagement::TextureManager::AsyncArtifactLoadProbeResult::Busy ||
				probe ==
				NLS::Core::ResourceManagement::TextureManager::AsyncArtifactLoadProbeResult::Pending)
			{
				ready = false;
				if (probe ==
					NLS::Core::ResourceManagement::TextureManager::AsyncArtifactLoadProbeResult::Pending)
					markPendingResourceTexture();
			}
			else if (unavailableTexturePaths != nullptr && !genericPath.empty())
			{
				unavailableTexturePaths->insert(genericPath);
				clearPendingResourceTexture();
			}
			continue;
        }
        texture = *cachedArtifactTexture;
        const bool textureHasGpuHandle =
            texture != nullptr && texture->GetTextureHandle() != nullptr;
        if (!textureHasGpuHandle &&
			textureProbe() ==
                NLS::Core::ResourceManagement::TextureManager::AsyncArtifactLoadProbeResult::Failed)
        {
            clearPendingResourceTexture();
            continue;
        }
        if (!textureHasGpuHandle &&
            (activeTextureInterests.find(genericPath) == activeTextureInterests.end() ||
				textureProbe() !=
                    NLS::Core::ResourceManagement::TextureManager::AsyncArtifactLoadProbeResult::Pending))
        {
            const auto requestResult = textureManager->TryRequestAsyncArtifactForPreview(path, true);
            if (!requestResult.has_value())
            {
                ready = false;
                continue;
            }
            texture = requestResult->resource;
            if (requestResult->pending)
            {
                markPendingResourceTexture();
                cachedTextureProbe =
                    NLS::Core::ResourceManagement::TextureManager::AsyncArtifactLoadProbeResult::Pending;
            }
            else if (requestResult->failed)
            {
                clearPendingResourceTexture();
                cachedTextureProbe =
                    NLS::Core::ResourceManagement::TextureManager::AsyncArtifactLoadProbeResult::Failed;
            }
            else
            {
                // The request may have joined a manager-owned load and returned
                // without a ready resource. Re-probe that transition once below.
                invalidateTextureAsyncProbe();
            }
            if (requestResult->failed)
                continue;
        }
        if (texture != nullptr && texture->GetTextureHandle() != nullptr)
        {
            material.SetRawParameter(name, texture);
            clearPendingResourceTexture();
            if (readyTextureCache != nullptr)
                (*readyTextureCache)[genericPath] = texture;
            continue;
        }

        // A null result is only pending when TextureManager actually owns an
        // async request. Unsupported or model-local paths have no future that
        // can make this material progress; the material loader's established
        // contract is to use its default texture for those samplers.
        const auto textureState = textureProbe();
        if (textureState ==
                NLS::Core::ResourceManagement::TextureManager::AsyncArtifactLoadProbeResult::Busy)
        {
            ready = false;
            continue;
        }
        if (textureState !=
                NLS::Core::ResourceManagement::TextureManager::AsyncArtifactLoadProbeResult::Pending &&
            textureState !=
                NLS::Core::ResourceManagement::TextureManager::AsyncArtifactLoadProbeResult::Failed)
        {
            if (unavailableTexturePaths != nullptr && !genericPath.empty())
                unavailableTexturePaths->insert(genericPath);
            continue;
        }
        if (textureState ==
            NLS::Core::ResourceManagement::TextureManager::AsyncArtifactLoadProbeResult::Pending)
        {
            markPendingResourceTexture();
            ready = false;
        }
        else if (textureState !=
            NLS::Core::ResourceManagement::TextureManager::AsyncArtifactLoadProbeResult::Busy)
        {
            clearPendingResourceTexture();
        }
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
    return snapshot.expectedDrawItemCount != 0u &&
        snapshot.expectedDrawItemCount == snapshot.drawItems.size();
}

bool IsCompletePrefabPreviewSceneDraw(const EditorThumbnailPreviewResult& result)
{
    return result.expectedSceneDrawCount != 0u &&
        result.rawVisibleDrawCount == result.expectedSceneDrawCount &&
        result.submittedSceneDrawCount != 0u &&
        result.objectDataOverflowDroppedObjectCount == 0u;
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

bool ShouldSkipPrefabPreviewDrawPrewarmForResident(
    const bool residentSnapshotUsed,
    const bool residentResourcesComplete)
{
    return residentSnapshotUsed && residentResourcesComplete;
}

bool ShouldRestorePrefabPreviewDrawPrewarmState(
    const bool savedPreparedAlive,
    const uint64_t savedResourcePlanRevision,
    const uint64_t currentResourcePlanRevision,
    const size_t savedNextDrawPrewarmIndex,
    const size_t savedTotalDrawPrewarmCount,
    const bool savedDrawPrewarmComplete)
{
    if (!savedPreparedAlive || savedResourcePlanRevision == 0u ||
        savedResourcePlanRevision != currentResourcePlanRevision)
    {
        return false;
    }
    return savedNextDrawPrewarmIndex <= savedTotalDrawPrewarmCount ||
        savedDrawPrewarmComplete;
}

bool SamePrefabPreviewSceneAssemblyFloat(const float left, const float right)
{
    if (left == right)
        return true;
    if (std::isnan(left) || std::isnan(right))
        return std::isnan(left) && std::isnan(right);
    if (!std::isfinite(left) || !std::isfinite(right))
        return false;
    constexpr float epsilon = 1.0e-5f;
    return std::abs(left - right) <=
        epsilon * std::max({1.0f, std::abs(left), std::abs(right)});
}

bool SamePrefabPreviewSceneAssemblyVector(
    const NLS::Maths::Vector3& left,
    const NLS::Maths::Vector3& right)
{
    return SamePrefabPreviewSceneAssemblyFloat(left.x, right.x) &&
        SamePrefabPreviewSceneAssemblyFloat(left.y, right.y) &&
        SamePrefabPreviewSceneAssemblyFloat(left.z, right.z);
}

bool SamePrefabPreviewSceneAssemblyRotation(
    const NLS::Maths::Quaternion& left,
    const NLS::Maths::Quaternion& right)
{
    const auto sameSign = SamePrefabPreviewSceneAssemblyFloat(left.x, right.x) &&
        SamePrefabPreviewSceneAssemblyFloat(left.y, right.y) &&
        SamePrefabPreviewSceneAssemblyFloat(left.z, right.z) &&
        SamePrefabPreviewSceneAssemblyFloat(left.w, right.w);
    if (sameSign)
        return true;

    // q and -q encode the same orientation. Import preparation and Prefab
    // serialization are allowed to choose either sign.
    return SamePrefabPreviewSceneAssemblyFloat(left.x, -right.x) &&
        SamePrefabPreviewSceneAssemblyFloat(left.y, -right.y) &&
        SamePrefabPreviewSceneAssemblyFloat(left.z, -right.z) &&
        SamePrefabPreviewSceneAssemblyFloat(left.w, -right.w);
}

bool CanReusePrefabPreviewSceneAssembly(
    const PreviewRenderableSnapshot& previous,
    const PreviewRenderableSnapshot& current,
    std::string* mismatchReason = nullptr)
{
    const auto mismatch = [mismatchReason](const char* reason)
    {
        if (mismatchReason != nullptr)
            *mismatchReason = reason;
        return false;
    };
    if (previous.expectedDrawItemCount != current.expectedDrawItemCount ||
        previous.drawItems.size() != current.drawItems.size())
    {
        return mismatch("draw-item-count");
    }

    for (size_t index = 0u; index < previous.drawItems.size(); ++index)
    {
        const auto& left = previous.drawItems[index];
        const auto& right = current.drawItems[index];
        // Object and AssetId identities are registry lookup metadata. They can
        // change when an import-published snapshot is replaced by the loaded
        // scene view, but the assembled preview objects do not consume them.
        if (left.meshPath != right.meshPath)
            return mismatch("mesh-path");
        // Material paths can be filled or normalized as live resources become
        // resident. Existing proxy slots are rebound on resident revisions;
        // only a slot-count change alters the assembled object layout.
        if (left.materialPaths.size() != right.materialPaths.size())
            return mismatch("material-slot-count");
        if (!SamePrefabPreviewSceneAssemblyVector(left.localPosition, right.localPosition))
            return mismatch("position");
        if (!SamePrefabPreviewSceneAssemblyRotation(left.localRotation, right.localRotation))
            return mismatch("rotation");
        if (!SamePrefabPreviewSceneAssemblyVector(left.localScale, right.localScale))
            return mismatch("scale");
    }
    if (mismatchReason != nullptr)
        mismatchReason->clear();
    return true;
}

bool ShouldContinuePrefabPreviewResourceInspection(
    const size_t phaseIndex,
    const size_t inspectedResourceCount,
    const bool deadlineExpired)
{
    (void)phaseIndex;
    (void)inspectedResourceCount;
    // Every phase is best-effort. Once the deadline is reached, defer the
    // whole phase to the next pump; allowing one extra lookup here made a
    // scene import lock turn a nominal 1 ms thumbnail pump into a multi-second
    // UI stall.
    return !deadlineExpired;
}

bool ShouldResetPrefabPreviewPhaseDeadline(
    const size_t unresolvedPathCount,
    const size_t acceptedRequestCount,
    const size_t pumpPathCount)
{
    (void)acceptedRequestCount;
    // The unresolved queue is the source of truth for phase completion. An
    // accepted request marker can briefly outlive the request when the manager
    // publishes the resource through its registered-path index. Letting that
    // bookkeeping marker consume the next phase's budget permanently starves
    // material loading for cold prefabs.
    return unresolvedPathCount == 0u &&
        pumpPathCount == 0u;
}

bool ShouldPumpPrefabRuntimeUploadRetirement(
    const size_t explicitPumpPathCount,
    const size_t acceptedRequestCount)
{
    // Pumping explicit paths already retires matching runtime uploads. The
    // empty window is only needed after the inspection cursor has drained but
    // an accepted request can still have a delayed RHI completion.
    return explicitPumpPathCount == 0u && acceptedRequestCount != 0u;
}

bool ShouldRefreshPrefabPreviewTextureInspectionDeadlineAfterSetup(
    const bool materialPhaseComplete,
    const size_t previouslyPendingTexturePathCount)
{
    // Existing texture requests must stay inside the phase's original budget.
    // With no request to pump, fixed interest-set preparation is not useful
    // progress and must not consume every future inspection window.
    return materialPhaseComplete && previouslyPendingTexturePathCount == 0u;
}

bool ShouldWaitForPrefabPreviewMaterialResourceTable(
    const size_t contentionCount,
    const bool allowNewResourceRequests,
    const bool sceneResourceResolutionBlocking)
{
    return allowNewResourceRequests &&
        !sceneResourceResolutionBlocking &&
        contentionCount >= kThumbnailPreviewPrefabMaterialContentionRetryFrameCount;
}

uint64_t ResolvePrefabPreviewExpectedSceneDrawCount(
    const uint64_t snapshotExpectedDrawItemCount,
    const size_t resourcePlanDrawItemCount,
    const bool residentPreviewPartial)
{
    // Partial resident packages still report the source expectation so they
    // cannot accidentally become durable. Non-resident canonical plans retain
    // every source draw item and are complete only after all are assembled.
    return residentPreviewPartial && snapshotExpectedDrawItemCount != 0u
        ? snapshotExpectedDrawItemCount
        : static_cast<uint64_t>(resourcePlanDrawItemCount);
}

bool ShouldPreservePrefabPreviewSceneAfterRenderAttempt(const std::string& diagnostic)
{
    if (diagnostic == "thumbnail-gpu-preview-readback-pending")
        return true;

    constexpr const char* kDrawPrewarmPendingPrefix =
        "thumbnail-gpu-preview-resources-pending:prefab-draw-prewarm=";
    if (diagnostic.rfind(kDrawPrewarmPendingPrefix, 0u) == 0u)
        return true;

    // Scene assembly is time-sliced independently from dependency pumping.
    // Keep the already-created preview objects and cursor alive when the
    // current batch reaches its budget; clearing here resets the cursor to
    // zero and makes a large resident prefab restart every frame.
    if (diagnostic == "thumbnail-gpu-preview-resident-partial")
        return true;

    constexpr const char* kSceneAssemblyPendingPrefix =
        "thumbnail-gpu-preview-resources-pending:prefab-scene-assembly=";
    return diagnostic.rfind(kSceneAssemblyPendingPrefix, 0u) == 0u;
}

NLS::Render::Resources::Mesh* FindRegisteredPreviewMesh(
    NLS::Core::ResourceManagement::MeshManager& meshManager,
    const std::string& meshPath,
    std::string* registeredPath = nullptr,
    bool* lookupBusy = nullptr)
{
    if (lookupBusy != nullptr)
        *lookupBusy = false;
    if (meshPath.empty())
        return nullptr;

    NLS::Render::Resources::Mesh* cached = nullptr;
    if (!meshManager.TryGetResource(meshPath, cached))
    {
        if (lookupBusy != nullptr)
            *lookupBusy = true;
        return nullptr;
    }
    if (cached != nullptr)
    {
        if (registeredPath != nullptr)
            *registeredPath = meshPath;
        return cached;
    }

    const auto resolvedPath =
        NLS::Core::ResourceManagement::MeshManager::ResolveArtifactResourcePath(meshPath);
    const auto equivalentPath = meshManager.FindRegisteredMeshPathByResolvedArtifactPath(resolvedPath);
    if (!equivalentPath.has_value())
        return nullptr;

    if (registeredPath != nullptr)
        *registeredPath = *equivalentPath;
    return meshManager.GetResource(*equivalentPath, false);
}

NLS::Render::Resources::Material* FindRegisteredPreviewMaterial(
    NLS::Core::ResourceManagement::MaterialManager& materialManager,
    const std::string& materialPath,
    const std::string& resolvedMaterialPath = {})
{
    if (materialPath.empty())
        return nullptr;
    if (auto* cached = materialManager.GetResource(materialPath, false))
        return cached;
    if (auto* cached = materialManager.FindRegisteredMaterialByResolvedArtifactPath(materialPath))
        return cached;
    return !resolvedMaterialPath.empty()
        ? materialManager.FindRegisteredMaterialByResolvedArtifactPath(resolvedMaterialPath)
        : materialManager.FindRegisteredMaterialByEquivalentArtifactPath(materialPath);
}

NLS::Render::Resources::Mesh* ResolvePreviewMesh(
    NLS::Core::ResourceManagement::MeshManager& meshManager,
    const std::string& meshPath,
    std::shared_ptr<const std::vector<uint8_t>> preparedPayload = {})
{
    if (meshPath.empty())
        return nullptr;

    if (auto* cached = FindRegisteredPreviewMesh(meshManager, meshPath))
        return cached;

    if (preparedPayload != nullptr)
    {
        return meshManager.RequestAsyncPreparedArtifactForPreview(
            meshPath,
            std::move(preparedPayload),
            true);
    }

    if (ShouldLoadPreviewMeshThroughArtifactLoader(meshPath))
        return meshManager.RequestAsyncArtifactForPreview(meshPath, true);

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
    // A canonical prefab thumbnail must represent the complete object. Resource
    // resolution and scene assembly are already time-sliced downstream, so
    // dropping draw items here only makes an incomplete image durable.
    for (size_t drawItemIndex = 0u;
         drawItemIndex < snapshot.drawItems.size();
         ++drawItemIndex)
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
            FindRegisteredPreviewMesh(*meshManager, planned.meshLoadPath) != nullptr;
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
                FindRegisteredPreviewMaterial(*materialManager, materialLoadPath) != nullptr;
            (void)materialReady;
            if (!materialLoadPath.empty())
                plan.materialLoadPaths.insert(materialLoadPath);
            planned.materialLoadPaths.push_back(std::move(materialLoadPath));
        }
    }
    return plan;
}

PrefabPreviewResourcePlan BuildResidentPrefabPreviewResourcePlan(
    const PreviewRenderableSnapshot& snapshot)
{
    PrefabPreviewResourcePlan plan;
    plan.sourceDrawItemCount = snapshot.drawItems.size();
    plan.drawItems.reserve(snapshot.drawItems.size());
    for (size_t drawItemIndex = 0u; drawItemIndex < snapshot.drawItems.size(); ++drawItemIndex)
    {
        const auto& drawItem = snapshot.drawItems[drawItemIndex];
        PrefabPreviewResourcePlanDrawItem planned;
        planned.drawItemIndex = drawItemIndex;
        // Resident resources are keyed by the immutable snapshot paths. Do
        // not resolve manifests, inspect headers, or normalize through the
        // artifact filesystem for this branch.
        planned.meshLoadPath = drawItem.meshPath;
        if (!planned.meshLoadPath.empty())
            plan.meshLoadPaths.insert(planned.meshLoadPath);
        planned.materialLoadPaths = drawItem.materialPaths;
        for (const auto& materialPath : planned.materialLoadPaths)
        {
            if (!materialPath.empty())
                plan.materialLoadPaths.insert(materialPath);
        }
        plan.drawItems.push_back(std::move(planned));
    }
    return plan;
}

struct PrefabPreviewResourcePumpState
{
    std::deque<std::string> unresolvedMeshPaths;
    std::deque<std::string> unresolvedMaterialPaths;
    std::deque<std::string> materialsAwaitingTextures;
    std::unordered_set<std::string> unavailableMaterialPaths;
    std::unordered_set<std::string> meshPathsToPump;
    std::unordered_set<std::string> materialPathsToPump;
    // Mesh dependencies can be shared with scene loading under an equivalent
    // artifact spelling. Once a request has been accepted, remember that
    // identity locally so continuation pumps do not rescan every global mesh
    // request just to rediscover the same pending future.
    std::unordered_set<std::string> meshRequestPaths;
    // Once an async material request has been accepted, the path-filtered
    // pump is the source of truth for progress. Re-querying the manager's
    // equivalent-path pending scan on every frame adds avoidable main-thread
    // work while the artifact is still loading.
    std::unordered_set<std::string> materialRequestPaths;
    // Resolve each material source path once per resource plan. The resource
    // manager's equivalent-path lookup otherwise re-parses the path and may
    // stat the artifact on every continuation pump.
    std::unordered_map<std::string, std::string> resolvedMaterialPaths;
    // The scene renderer can hold the primary resource map during every
    // post-draw thumbnail turn. Track repeated contention per dependency so a
    // cold asset eventually performs one blocking, duplicate-safe lookup.
    std::unordered_map<std::string, size_t> materialResourceTableContentionCounts;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        materialUnavailableSince;
    // Texture interests are renderer-wide for compatibility with the
    // non-prefab path, but a prefab continuation must retain its own paths
    // across interleaved requests. Otherwise switching to another preview
    // releases the renderer interest set and strands this prefab's futures.
    std::unordered_set<std::string> texturePathsToPump;
    // Keep only paths for which this plan has observed an authoritative
    // TextureManager request. The umbrella texturePathsToPump set also holds
    // reduced-preview paths, so probing it every frame repeatedly resolves and
    // stats paths that are not owned by the manager.
    std::unordered_set<std::string> pendingTexturePaths;
    std::unordered_set<std::string> requestedTexturePaths;
    std::unordered_set<std::string> pendingThumbnailTexturePaths;
    std::unordered_set<std::string> unavailableTexturePaths;
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
    const NLS::Core::ResourceManagement::TextureManager* textureManager = nullptr;
    uint64_t meshManagerInstanceId = 0u;
    uint64_t materialManagerInstanceId = 0u;
    uint64_t textureManagerInstanceId = 0u;
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry* resourceLifetimeRegistry = nullptr;
    std::string ownerToken;
    // Resource dependencies belong to a particular plan. A resident registry
    // refresh can replace the plan while the prepared snapshot stays alive;
    // do not keep pumping the old dependency set in that case.
    uint64_t resourcePlanRevision = 0u;
    size_t resourcePlanMeshPathCount = 0u;
    size_t resourcePlanMaterialPathCount = 0u;
    bool resourcePlanTruncated = false;
    std::string terminalDiagnostic;
    bool meshIdentityDiagnosticRecorded = false;
};

uint64_t BuildPrefabPreviewResourceProgressToken(
    const PrefabPreviewResourcePumpState& state)
{
    uint64_t token = 1469598103934665603ull;
    const auto combine = [&token](const uint64_t value)
    {
        token ^= value + 0x9e3779b97f4a7c15ull + (token << 6u) + (token >> 2u);
    };

    combine(state.resourcePlanRevision);
    combine(state.unresolvedMeshPaths.size());
    combine(state.unresolvedMaterialPaths.size());
    combine(state.materialsAwaitingTextures.size());
    combine(state.unavailableMaterialPaths.size());
    combine(state.meshRequestPaths.size());
    combine(state.materialRequestPaths.size());
    combine(state.texturePathsToPump.size());
    combine(state.pendingTexturePaths.size());
    combine(state.pendingThumbnailTexturePaths.size());
    combine(state.unavailableTexturePaths.size());
    combine(state.resolvedMeshes.size());
    combine(state.resolvedMaterials.size());
    combine(state.meshHandles.size());
    combine(state.materialHandles.size());
    return token == 0u ? 1u : token;
}

struct PreparedPrefabPreview
{
    mutable PreviewRenderableSnapshot snapshot;
    mutable PrefabPreviewResourcePlan resourcePlan;
    mutable PrefabPreviewResourcePumpState resourcePumpState;
    // Keep the scene-owned snapshot alive until the renderer retires the
    // preview scene. Requests carry only weak resident handles.
    mutable std::optional<ResidentPrefabPreviewRegistry::Lease> residentLease;
    mutable std::shared_ptr<const ResidentPrefabPreviewResources> residentResources;
    // Immutable snapshot shared with the service when a GPU validation frame
    // needs to fall back to CPU PNG persistence.
    mutable std::shared_ptr<const PreviewRenderableSnapshot> canonicalSnapshot;
    mutable bool residentSnapshotUsed = false;
    // Import-time snapshots own complete graph topology but no scene handles.
    // They may load render dependencies while still avoiding a Prefab read.
    mutable bool allowArtifactResourceLoading = false;
    // The registry keeps a stable runtime identity while scene restore publishes
    // progressively richer snapshots. Track its revision separately from the
    // resource package pointer: a package can be updated in place, and the
    // prepared-cache key intentionally remains stable across those updates.
    mutable uint64_t residentSnapshotRevision = 0u;
    // Resource readiness can advance while the scene topology stays identical.
    // Keep assembly lifetime independent so the complete package can reuse the
    // partial frame's already-created preview objects.
    mutable uint64_t sceneAssemblyRevision = 1u;
    mutable uint64_t resourcePlanRevision = 1u;
    mutable std::string diagnostic;
    mutable bool awaitResidentLoad = false;
};

bool ShouldDeferLargePrefabPreviewUntilResident(
    const size_t drawItemCount,
    const bool residentSnapshotUsed)
{
    return !residentSnapshotUsed &&
        drawItemCount > kMaxColdGpuPreviewPrefabDrawItems;
}

bool IsResidentSnapshotRegistrationPendingDiagnostic(const std::string& diagnostic)
{
    return diagnostic == kResidentSnapshotRegistrationPendingDiagnostic;
}

bool IsResidentSnapshotPendingDiagnostic(const std::string& diagnostic)
{
    return IsResidentSnapshotRegistrationPendingDiagnostic(diagnostic) ||
        diagnostic == kResidentSnapshotResourcesPendingDiagnostic;
}

void ResetPrefabPreviewResourcePumpStateForManagers(
    PrefabPreviewResourcePumpState& state,
    const PrefabPreviewResourcePlan& resourcePlan,
    const uint64_t resourcePlanRevision,
    const NLS::Core::ResourceManagement::MeshManager& meshManager,
    const NLS::Core::ResourceManagement::MaterialManager& materialManager,
    const NLS::Core::ResourceManagement::TextureManager* textureManager,
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry* resourceLifetimeRegistry,
    std::string ownerToken)
{
    if (state.meshManager == &meshManager &&
        state.materialManager == &materialManager &&
        state.textureManager == textureManager &&
        state.meshManagerInstanceId == meshManager.GetInstanceId() &&
        state.materialManagerInstanceId == materialManager.GetInstanceId() &&
        state.textureManagerInstanceId ==
            (textureManager != nullptr ? textureManager->GetInstanceId() : 0u) &&
        state.resourceLifetimeRegistry == resourceLifetimeRegistry &&
        state.resourcePlanRevision == resourcePlanRevision)
    {
        return;
    }

    state.unresolvedMeshPaths.clear();
    state.unresolvedMaterialPaths.clear();
    state.materialsAwaitingTextures.clear();
    state.unavailableMaterialPaths.clear();
    state.pendingThumbnailTexturePaths.clear();
    state.unavailableTexturePaths.clear();
    state.texturePathsToPump.clear();
    state.pendingTexturePaths.clear();
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
    state.materialRequestPaths.clear();
    state.resolvedMaterialPaths.clear();
    state.requestedTexturePaths.clear();
    state.materialUnavailableSince.clear();
    state.resolvedMeshes.clear();
    state.resolvedMaterials.clear();
    state.meshHandles.clear();
    state.materialHandles.clear();
    state.resourcePlanTruncated = resourcePlan.truncatedForPendingResources;
    state.resourcePlanMeshPathCount = resourcePlan.meshLoadPaths.size();
    state.resourcePlanMaterialPathCount = resourcePlan.materialLoadPaths.size();
    state.terminalDiagnostic.clear();
    state.meshManager = &meshManager;
    state.materialManager = &materialManager;
    state.textureManager = textureManager;
    state.meshManagerInstanceId = meshManager.GetInstanceId();
    state.materialManagerInstanceId = materialManager.GetInstanceId();
    state.textureManagerInstanceId =
        textureManager != nullptr ? textureManager->GetInstanceId() : 0u;
    state.resourceLifetimeRegistry = resourceLifetimeRegistry;
    state.ownerToken = std::move(ownerToken);
    state.resourcePlanRevision = resourcePlanRevision;
}

bool SeedResidentPrefabPreviewResourceState(
    PrefabPreviewResourcePumpState& state,
    const PrefabPreviewResourcePlan& resourcePlan,
    const ResidentPrefabPreviewResources& resources,
    const NLS::Core::ResourceManagement::MeshManager& meshManager,
    const NLS::Core::ResourceManagement::MaterialManager& materialManager,
    const NLS::Core::ResourceManagement::TextureManager* textureManager)
{
    if (!resources.IsValidFor(meshManager, materialManager, textureManager) ||
        resourcePlan.drawItems.size() != resources.drawItems.size())
    {
        return false;
    }
    state.unresolvedMeshPaths.clear();
    state.unresolvedMaterialPaths.clear();
    state.materialsAwaitingTextures.clear();
    state.materialPathsToPump.clear();
    state.meshPathsToPump.clear();
    state.meshRequestPaths.clear();
    state.materialRequestPaths.clear();
    state.resolvedMaterialPaths.clear();
    state.materialUnavailableSince.clear();
    state.pendingTexturePaths.clear();
    state.requestedTexturePaths.clear();
    state.pendingThumbnailTexturePaths.clear();
    state.unavailableTexturePaths.clear();
    state.resolvedMeshes.clear();
    state.resolvedMaterials.clear();
    state.meshHandles.clear();
    state.materialHandles.clear();
    state.resourcePlanTruncated = false;
    state.resourcePlanMeshPathCount = resourcePlan.meshLoadPaths.size();
    state.resourcePlanMaterialPathCount = resourcePlan.materialLoadPaths.size();

    for (size_t index = 0u; index < resourcePlan.drawItems.size(); ++index)
    {
        const auto& planned = resourcePlan.drawItems[index];
        const auto& resident = resources.drawItems[index];
        if (resident.meshIndex >= resources.meshes.size())
            return false;
        auto* mesh = resources.meshes[resident.meshIndex].Get();
        if (mesh == nullptr)
        {
            const auto transient = resources.transientMeshesByIndex.find(resident.meshIndex);
            if (transient != resources.transientMeshesByIndex.end())
                mesh = transient->second.get();
        }
        if (mesh == nullptr)
            return false;
        const auto meshIndex = resources.meshIndicesByPath.find(planned.meshLoadPath);
        if (meshIndex == resources.meshIndicesByPath.end() ||
            meshIndex->second != resident.meshIndex)
        {
            return false;
        }
        state.resolvedMeshes[planned.meshLoadPath] = mesh;
        for (size_t slot = 0u; slot < planned.materialLoadPaths.size(); ++slot)
        {
            const auto& materialPath = planned.materialLoadPaths[slot];
            if (materialPath.empty())
                continue;
            const auto materialIndex = slot < resident.materialIndices.size()
                ? resident.materialIndices[slot]
                : SIZE_MAX;
            const auto materialPathIndex = resources.materialIndicesByPath.find(materialPath);
            if (materialIndex != SIZE_MAX &&
                (materialPathIndex == resources.materialIndicesByPath.end() ||
                    materialPathIndex->second != materialIndex))
            {
                return false;
            }
            state.resolvedMaterials[materialPath] = materialIndex == SIZE_MAX
                ? nullptr
                : materialIndex < resources.materials.size()
                    ? resources.materials[materialIndex].Get()
                    : nullptr;
            if (materialIndex != SIZE_MAX && state.resolvedMaterials[materialPath] == nullptr)
                return false;
        }
    }
    return true;
}

bool PrefabArtifactExceedsGpuPreviewComplexityBudget(
    const NLS::Engine::Assets::PrefabArtifact& prefab);

PreparedPrefabPreview PreparePrefabPreviewInBackground(const AssetThumbnailRequest& request)
{
    PreparedPrefabPreview prepared;
    const auto telemetryBegin = std::chrono::steady_clock::now();
    const auto preparationCacheKeyHash = std::hash<std::string> {}(
        BuildPreviewSnapshotCacheKey(request));
    const auto freshnessEvidenceHash = std::hash<std::string> {}(
        request.dependencyStamp);
    const auto recordPrepareCheckpoint = [&request](const char* checkpoint, const size_t count = 0u)
    {
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects,
            {},
            count,
            request.sourceAssetPath + "|" + request.subAssetKey + "|background-" + checkpoint
        });
    };
    recordPrepareCheckpoint("start");

    const auto buildResourcePlan = [&]()
    {
        if (prepared.residentSnapshotUsed && prepared.residentResources != nullptr)
        {
            prepared.resourcePlan = BuildResidentPrefabPreviewResourcePlan(prepared.snapshot);
        }
        else
        {
            prepared.resourcePlan = BuildPrefabPreviewResourcePlan(
                request,
                prepared.snapshot,
                nullptr,
                nullptr,
                SIZE_MAX);
            recordPrepareCheckpoint("plan-built", prepared.resourcePlan.drawItems.size());
            prepared.diagnostic = prepared.resourcePlan.diagnostic;
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
    };

    bool residentSnapshotUsed = false;
    if (request.residentPrefabPreviewSource.has_value())
    {
        const auto& residentSource = *request.residentPrefabPreviewSource;
        prepared.allowArtifactResourceLoading =
            residentSource.allowArtifactResourceLoading;
        if (residentSource.HasIdentity())
        {
            std::optional<ResidentPrefabPreviewRegistry::Lease> residentLease;
            std::shared_ptr<const PreviewRenderableSnapshot> snapshot;
            if (const auto registry = residentSource.registry.lock(); registry != nullptr)
            {
                residentLease = registry->Acquire(
                    residentSource.runtimeCacheIdentity,
                    residentSource.freshnessFingerprint,
                    true);
                if (residentLease.has_value())
                    snapshot = residentLease->Snapshot();
            }
            else
            {
                snapshot = residentSource.snapshot.lock();
            }

            // A resident snapshot without its complete resource package is a
            // recoverable scene-restore state. Keep the request Pending so the
            // next pump can acquire the package after live resources attach;
            // falling through here would reopen the artifact and race the
            // scene-owned load.
            if (snapshot != nullptr &&
                !snapshot->drawItems.empty() &&
                residentLease.has_value() &&
                PreviewSnapshotIsCompleteForGpuPrefabPreview(*snapshot))
            {
                if (residentLease->Resources() == nullptr &&
                    !residentSource.allowArtifactResourceLoading)
                {
                    prepared.diagnostic = kResidentSnapshotResourcesPendingDiagnostic;
                    recordPrepareCheckpoint("resident-resources-pending", snapshot->drawItems.size());
                    return prepared;
                }
                prepared.residentLease = std::move(residentLease);
                prepared.residentSnapshotUsed = true;
                prepared.allowArtifactResourceLoading =
                    residentSource.allowArtifactResourceLoading;
                prepared.residentResources = prepared.residentLease.has_value()
                    ? prepared.residentLease->Resources()
                    : nullptr;
                prepared.snapshot = *snapshot;
                prepared.canonicalSnapshot = std::move(snapshot);
                prepared.residentSnapshotRevision = request.residentPreviewRevision;
                if (const auto registry = residentSource.registry.lock();
                    registry != nullptr)
                {
                    if (const auto state = registry->GetSnapshotState(
                            residentSource.runtimeCacheIdentity,
                            residentSource.freshnessFingerprint);
                        state.has_value())
                    {
                        prepared.residentSnapshotRevision = state->revision;
                    }
                }
                recordPrepareCheckpoint("resident-snapshot", prepared.snapshot.drawItems.size());
                buildResourcePlan();
                residentSnapshotUsed = true;
            }
            else if (residentLease.has_value() &&
                snapshot != nullptr &&
                !snapshot->drawItems.empty())
            {
                prepared.diagnostic = kResidentSnapshotResourcesPendingDiagnostic;
                recordPrepareCheckpoint("resident-snapshot-incomplete", snapshot->drawItems.size());
                return prepared;
            }

            // SetupUI can present progress frames before scene prefab restore
            // has registered its immutable snapshots. Do not start the full
            // artifact path in that window: the same asset may become resident
            // moments later, and a negative prepared-cache entry would hide
            // that late registration permanently for this freshness revision.
            if (!residentSnapshotUsed)
            {
                if (const auto registry = residentSource.registry.lock();
                    registry != nullptr && registry->IsSceneRestoreInProgress())
                {
                    prepared.diagnostic = kResidentSnapshotRegistrationPendingDiagnostic;
                    recordPrepareCheckpoint("resident-registration-pending");
                    return prepared;
                }
            }
        }
    }

    std::optional<NLS::Engine::Assets::PrefabArtifact> prefab;
    if (!residentSnapshotUsed)
    {
        // Use the same shared repository as scene restore, drag/drop, and the
        // CPU thumbnail path even when the persistent prepared-cache directory
        // does not exist yet. The repository first checks the hot/in-flight
        // result, then the persistent cache, and only then performs the cold
        // artifact read. Keeping this path unified prevents a cold GPU preview
        // from reopening the same PrefabArtifact that another consumer is
        // already importing.
        EditorAssetDragDropBridge bridge(request.projectRoot / "Assets");
        UnifiedPrefabLoadRequest loadRequest;
        loadRequest.source.sourceAssetPath = request.sourceAssetPath;
        loadRequest.source.prefabSubAssetKey = request.subAssetKey;
        loadRequest.source.sourceAssetId = request.assetId;
        loadRequest.source.assetType = request.generatedSubAsset
            ? NLS::Core::Assets::AssetType::ModelScene
            : NLS::Core::Assets::AssetType::Prefab;
        loadRequest.loadMode = UnifiedPrefabLoadMode::Prewarm;
        loadRequest.ownerKind = UnifiedPrefabOwnerKind::AsyncJob;
        loadRequest.ownerScopeId = request.sourceAssetPath + "|" + request.subAssetKey;
        loadRequest.requiredReadiness = UnifiedPrefabReadiness::PrefabGraphOnly;
        loadRequest.allowPending = false;
        const auto fastLoad = bridge.LoadUnifiedPrefabShared(loadRequest);
        if (fastLoad.prefab != nullptr)
        {
            prefab = *fastLoad.prefab;
            recordPrepareCheckpoint("prefab-shared-repository", prefab->graph.objects.size());
        }
    }
    if (!residentSnapshotUsed && !prefab.has_value() && request.assetDatabaseSnapshot != nullptr)
    {
        prefab = request.assetDatabaseSnapshot->LoadPrefabArtifactByAssetId(
            request.assetId,
            request.subAssetKey);
    }
    else if (!residentSnapshotUsed && !prefab.has_value())
    {
        AssetDatabaseFacade database(MakeProjectEditorAssetRoots(request.projectRoot));
        prefab = database.LoadPrefabArtifactByAssetId(request.assetId, request.subAssetKey);
    }
    recordPrepareCheckpoint("prefab-loaded", prefab.has_value() ? 1u : 0u);
    if (residentSnapshotUsed)
    {
        // The resident branch already built the canonical snapshot and its
        // resource plan. Do not reopen ArtifactDB or deserialize the Prefab.
    }
    else if (!prefab.has_value())
    {
        prepared.diagnostic = "thumbnail-gpu-preview-prefab-load-failed";
    }
    else if (PrefabArtifactExceedsGpuPreviewComplexityBudget(*prefab))
    {
        prepared.diagnostic = kLargePrefabPreviewAwaitingResidentDiagnostic;
    }
    else
    {
        prepared.snapshot = BuildPreviewRenderableSnapshot(*prefab);
        prepared.canonicalSnapshot = std::make_shared<const PreviewRenderableSnapshot>(
            prepared.snapshot);
        recordPrepareCheckpoint("snapshot-built", prepared.snapshot.drawItems.size());
        if (prepared.snapshot.drawItems.empty())
        {
            prepared.diagnostic = "thumbnail-gpu-preview-prefab-renderer-missing";
        }
        else if (!PreviewSnapshotIsCompleteForGpuPrefabPreview(prepared.snapshot))
        {
            prepared.diagnostic = kLargePrefabPreviewAwaitingResidentDiagnostic;
        }
        else
        {
            prepared.awaitResidentLoad = ShouldDeferLargePrefabPreviewUntilResident(
                prepared.snapshot.drawItems.size(),
                prepared.residentSnapshotUsed);
            if (prepared.awaitResidentLoad)
            {
                recordPrepareCheckpoint(
                    "awaiting-resident-load",
                    prepared.snapshot.drawItems.size());
            }
            else
            {
                buildResourcePlan();
            }
        }
    }

    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects,
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - telemetryBegin),
        prepared.snapshot.drawItems.size(),
        request.sourceAssetPath + "|" + request.subAssetKey +
            "|background-prepare|cacheKeyHash=" +
            std::to_string(preparationCacheKeyHash) +
            "|freshnessHash=" + std::to_string(freshnessEvidenceHash)
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

std::shared_ptr<NLS::Render::Resources::Material> CreateSharedStablePreviewMaterial(
    NLS::Render::Resources::Material& source)
{
    return std::shared_ptr<NLS::Render::Resources::Material>(
        CreateStablePreviewMaterial(source));
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

bool ShouldYieldPrefabMeshDependencyInspectionForTesting(
    const bool meshLoadPending,
    const size_t meshRequestStartCount,
    const size_t meshRequestStartBudget)
{
    return ShouldYieldPrefabMeshDependencyInspection(
        meshLoadPending,
        meshRequestStartCount,
        meshRequestStartBudget);
}

size_t GetThumbnailPreviewPrefabSceneAssemblyBudgetForTesting()
{
    return kThumbnailPreviewPrefabSceneAssemblyMaximumBatch;
}

size_t GetThumbnailPreviewPrefabDrawItemCapacityForTesting()
{
    return kMaxGpuPreviewPrefabGraphObjects;
}

std::string BuildThumbnailPreviewReadbackRequestKeyForTesting(const AssetThumbnailRequest& request)
{
    return BuildPreviewReadbackRequestKey(request);
}

std::string BuildThumbnailPreviewSceneAssemblyKeyForTesting(const AssetThumbnailRequest& request)
{
    return BuildPrefabPreviewSceneAssemblyKey(request);
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

bool ThumbnailPrefabPreparationUsesResidentSnapshotForTesting(
    const AssetThumbnailRequest& request)
{
    const auto prepared = PreparePrefabPreviewInBackground(request);
    return prepared.residentLease.has_value() &&
        !prepared.snapshot.drawItems.empty() &&
        prepared.diagnostic.empty();
}

bool ShouldDeferLargePrefabPreviewUntilResidentForTesting(
    const size_t drawItemCount,
    const bool residentSnapshotUsed)
{
    return ShouldDeferLargePrefabPreviewUntilResident(
        drawItemCount,
        residentSnapshotUsed);
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

bool ShouldContinuePrefabPreviewResourceInspectionForTesting(
    const size_t phaseIndex,
    const size_t inspectedResourceCount,
    const bool deadlineExpired)
{
    return ShouldContinuePrefabPreviewResourceInspection(
        phaseIndex,
        inspectedResourceCount,
        deadlineExpired);
}

bool ShouldResetPrefabPreviewPhaseDeadlineForTesting(
    const size_t unresolvedPathCount,
    const size_t acceptedRequestCount,
    const size_t pumpPathCount)
{
    return ShouldResetPrefabPreviewPhaseDeadline(
        unresolvedPathCount,
        acceptedRequestCount,
        pumpPathCount);
}

bool ShouldPumpPrefabRuntimeUploadRetirementForTesting(
    const size_t explicitPumpPathCount,
    const size_t acceptedRequestCount)
{
    return ShouldPumpPrefabRuntimeUploadRetirement(
        explicitPumpPathCount,
        acceptedRequestCount);
}

bool ShouldRefreshPrefabPreviewTextureInspectionDeadlineAfterSetupForTesting(
    const bool materialPhaseComplete,
    const size_t previouslyPendingTexturePathCount)
{
    return ShouldRefreshPrefabPreviewTextureInspectionDeadlineAfterSetup(
        materialPhaseComplete,
        previouslyPendingTexturePathCount);
}

bool ShouldWaitForPrefabPreviewMaterialResourceTableForTesting(
    const size_t contentionCount,
    const bool allowNewResourceRequests,
    const bool sceneResourceResolutionBlocking)
{
    return ShouldWaitForPrefabPreviewMaterialResourceTable(
        contentionCount,
        allowNewResourceRequests,
        sceneResourceResolutionBlocking);
}

bool ShouldRetainThumbnailPreviewTexturePathForTesting(
    const bool headerProbeQueued,
    const bool headerProbeInFlight,
    const bool deferredArtifactQueued,
    const bool artifactInFlight,
    const bool uploadInFlight,
    const bool resourceReady)
{
    return ShouldRetainThumbnailPreviewTexturePath(
        headerProbeQueued,
        headerProbeInFlight,
        deferredArtifactQueued,
        artifactInFlight,
        uploadInFlight,
        resourceReady);
}

bool ShouldDeferPrefabPreviewAfterDrawPrewarmForTesting(
    const bool prewarmSupported,
    const bool prewarmComplete)
{
    return ShouldDeferPrefabPreviewAfterDrawPrewarm(
        prewarmSupported,
        prewarmComplete);
}

bool ShouldSkipPrefabPreviewDrawPrewarmForResidentForTesting(
    const bool residentSnapshotUsed,
    const bool residentResourcesComplete)
{
    return ShouldSkipPrefabPreviewDrawPrewarmForResident(
        residentSnapshotUsed,
        residentResourcesComplete);
}

bool ShouldRestorePrefabPreviewDrawPrewarmStateForTesting(
    const bool savedPreparedAlive,
    const uint64_t savedResourcePlanRevision,
    const uint64_t currentResourcePlanRevision,
    const size_t savedNextDrawPrewarmIndex,
    const size_t savedTotalDrawPrewarmCount,
    const bool savedDrawPrewarmComplete)
{
    return ShouldRestorePrefabPreviewDrawPrewarmState(
        savedPreparedAlive,
        savedResourcePlanRevision,
        currentResourcePlanRevision,
        savedNextDrawPrewarmIndex,
        savedTotalDrawPrewarmCount,
        savedDrawPrewarmComplete);
}

bool CanReusePrefabPreviewSceneAssemblyForTesting(
    const PreviewRenderableSnapshot& previous,
    const PreviewRenderableSnapshot& current)
{
    return CanReusePrefabPreviewSceneAssembly(previous, current);
}

uint64_t ResolvePrefabPreviewExpectedSceneDrawCountForTesting(
    const uint64_t snapshotExpectedDrawItemCount,
    const size_t resourcePlanDrawItemCount,
    const bool residentPreviewPartial)
{
    return ResolvePrefabPreviewExpectedSceneDrawCount(
        snapshotExpectedDrawItemCount,
        resourcePlanDrawItemCount,
        residentPreviewPartial);
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

std::string BuildThumbnailPreviewReadbackRequestKey(const AssetThumbnailRequest& request)
{
    return BuildPreviewReadbackRequestKey(request);
}

class EditorThumbnailPreviewRenderer::Impl
{
public:
    struct StablePreviewMaterialKey
    {
        const NLS::Render::Resources::Material* source = nullptr;
        uint64_t sourceInstanceId = 0u;
        uint64_t parameterRevision = 0u;
        uint64_t renderStateRevision = 0u;
        uint64_t bindingRevision = 0u;
        uint64_t materialManagerInstanceId = 0u;
        std::string colorSpaceMode;
        std::string hdrMode;
        uint64_t visualContractVersion = 1u;

        friend bool operator==(
            const StablePreviewMaterialKey& left,
            const StablePreviewMaterialKey& right)
        {
            return left.source == right.source &&
                left.sourceInstanceId == right.sourceInstanceId &&
                left.parameterRevision == right.parameterRevision &&
                left.renderStateRevision == right.renderStateRevision &&
                left.bindingRevision == right.bindingRevision &&
                left.materialManagerInstanceId == right.materialManagerInstanceId &&
                left.colorSpaceMode == right.colorSpaceMode &&
                left.hdrMode == right.hdrMode &&
                left.visualContractVersion == right.visualContractVersion;
        }
    };

    struct StablePreviewMaterialKeyHash
    {
        size_t operator()(const StablePreviewMaterialKey& key) const
        {
            size_t hash = std::hash<const void*> {}(key.source);
            const auto combine = [&hash](const uint64_t value)
            {
                hash ^= std::hash<uint64_t> {}(value) +
                    static_cast<size_t>(0x9e3779b97f4a7c15ull) +
                    (hash << 6u) + (hash >> 2u);
            };
            combine(key.sourceInstanceId);
            combine(key.parameterRevision);
            combine(key.renderStateRevision);
            combine(key.bindingRevision);
            combine(key.materialManagerInstanceId);
            combine(std::hash<std::string> {}(key.colorSpaceMode));
            combine(std::hash<std::string> {}(key.hdrMode));
            combine(key.visualContractVersion);
            return hash;
        }
    };

    struct StablePreviewMaterialCacheEntry
    {
        std::shared_ptr<NLS::Render::Resources::Material> material;
        uint64_t lastUsed = 0u;
    };

    explicit Impl(NLS::Render::Context::Driver& driver)
        : m_driver(driver)
        , m_renderer(NLS::Engine::Rendering::CreateSceneRenderer(
              driver,
              NLS::Engine::Rendering::SceneRendererKind::Forward))
        , m_previewProxyPool(m_scene)
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
        RetirePendingReadback();
        RetireReadbackRing();
        m_completedReadbackPreviews.clear();
        m_orphanedReadbackRequestKeys.clear();
        ReleaseTextureInterests();
        for (const auto& [_, upload] : m_thumbnailTextureUploadRequests)
            NLS::Render::Context::DriverUIAccess::CancelUiRgba8TextureUpload(
                m_driver,
                upload.requestId);
        m_thumbnailTextureUploadRequests.clear();
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
        EnsureMaterialPreviewObject(
            *sphere,
            *m_materialPreviewMaterial,
            warmupRequest.enablePreviewProxyPool);
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
        if (m_thumbnailTextureRequestKey != requestKey)
        {
            // These are renderer-wide reduced texture resources, not state
            // owned by one prefab request. Clearing them when the visible
            // request changes discarded deferred paths for the previous
            // prefab and forced the next visit to restart its texture queue.
            // Completed failures remove their path in
            // PollThumbnailPreviewTextureLoads().
            m_thumbnailTextureRequestKey = requestKey;
        }
        PollThumbnailPreviewTextureLoads();

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
            if (prepared->awaitResidentLoad)
            {
                result.diagnostic = kLargePrefabPreviewAwaitingResidentDiagnostic;
                return result;
            }
            // A preparation task may have won the race before scene resource
            // resolution published its package. Refresh the same prepared
            // entry here so the next pump can switch to the no-I/O path without
            // rebuilding the request or reopening the artifact.
            uint64_t registryResidentRevision = 0u;
            bool registryResidentPackageComplete = false;
            if (request.residentPrefabPreviewSource.has_value() &&
                request.residentPrefabPreviewSource->HasIdentity())
            {
                if (const auto registry = request.residentPrefabPreviewSource->registry.lock();
                    registry != nullptr)
                {
                    if (const auto state = registry->GetSnapshotState(
                            request.residentPrefabPreviewSource->runtimeCacheIdentity,
                            request.residentPrefabPreviewSource->freshnessFingerprint);
                        state.has_value())
                    {
                        registryResidentRevision = state->revision;
                        registryResidentPackageComplete = state->complete;
                    }
                }
            }
            const bool residentResourcesRefreshed =
                TryRefreshResidentPrefabPreviewResources(request, prepared);
            const bool requiresResidentResourcePackage =
                prepared->residentSnapshotUsed &&
                !prepared->allowArtifactResourceLoading;
            if (requiresResidentResourcePackage && registryResidentPackageComplete &&
                (prepared->residentResources == nullptr ||
                    !prepared->residentResources->IsCompleteForSource() ||
                    registryResidentRevision > prepared->residentSnapshotRevision))
            {
                // The registry can publish completion before the renderer can
                // acquire a valid manager package. Do not render the old
                // partial package in that interval: it would overwrite the
                // stable provisional presentation and keep the request parked
                // on the same partial revision. Wait for the package refresh to
                // become valid, then render the complete resident snapshot.
                NLS::Core::Assets::RecordArtifactLoadTelemetry({
                    NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpDependencies,
                    {},
                    prepared->residentResources != nullptr
                        ? prepared->residentResources->drawItems.size()
                        : 0u,
                    request.sourceAssetPath + "|" + request.subAssetKey +
                        "|resident-package-sync-pending|registryRevision=" +
                        std::to_string(registryResidentRevision) +
                        "|preparedRevision=" +
                        std::to_string(prepared->residentSnapshotRevision) +
                        "|refreshed=" +
                        std::to_string(residentResourcesRefreshed ? 1u : 0u)
                });
                result.resourcesPending = true;
                result.diagnostic =
                    "thumbnail-gpu-preview-resources-pending:resident-package-sync";
                return result;
            }
            if (requiresResidentResourcePackage &&
                (prepared->residentResources == nullptr ||
                    !prepared->residentResources->IsValidFor(
                        previewMeshManager,
                        previewMaterialManager,
                        previewTextureManager)))
            {
                // The scene may have replaced its manager generation or may
                // still be attaching the live package. Keep this resident
                // request pending; starting the generic dependency pump here
                // would create a second artifact load and defeat reuse.
                result.resourcesPending = true;
                result.diagnostic = kResidentSnapshotResourcesPendingDiagnostic;
                return result;
            }
            const bool residentSceneResourceGate =
                requiresResidentResourcePackage &&
                NLS::Editor::Core::HasBlockingSceneLoadRendererResourceResolution();
            if (residentSceneResourceGate)
            {
                // A resident snapshot is usable before scene restoration is
                // complete when its dependencies are already registered. If
                // they are not, PumpPreparedPrefabResources will only join
                // existing scene-owned requests and will not start a second
                // artifact load.
                NLS::Core::Assets::RecordArtifactLoadTelemetry({
                    NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpDependencies,
                    {},
                    0u,
                    request.sourceAssetPath + "|" + request.subAssetKey +
                        "|resident-scene-resource-gate|mode=join-only"
                });
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
                prepared->resourcePlanRevision,
                previewMeshManager,
                previewMaterialManager,
                previewTextureManager,
                resourceLifetimeRegistry,
                "thumbnail-preview:" + BuildPreviewSnapshotCacheKey(request));
            if (prepared->residentResources != nullptr &&
                SeedResidentPrefabPreviewResourceState(
                    *pumpState,
                    prepared->resourcePlan,
                    *prepared->residentResources,
                    previewMeshManager,
                    previewMaterialManager,
                    previewTextureManager))
            {
                NLS::Core::Assets::RecordArtifactLoadTelemetry({
                    NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpDependencies,
                    {},
                    prepared->resourcePlan.drawItems.size(),
                    request.sourceAssetPath + "|" + request.subAssetKey +
                        "|resident-resource-package|no-artifact-load"
                });
                return result;
            }
            return PumpPreparedPrefabResources(
                request,
                *pumpState,
                previewMeshManager,
                previewMaterialManager,
                previewTextureManager,
                prepared->residentLease.has_value()
                    ? &*prepared->residentLease
                    : nullptr,
                // Resource managers de-duplicate requests by equivalent
                // artifact identity. A resident request can therefore join
                // the scene's in-flight load, or start the one shared load if
                // scene registration has not published it yet. Ordinary
                // non-resident requests remain protected by the caller's
                // scene-load scheduler gate.
                prepared->residentSnapshotUsed || !residentSceneResourceGate);
        }
        const auto requestedResourcePaths = CollectRequestedPreviewResourcePaths(request);
        const auto pumpTelemetryBegin = std::chrono::steady_clock::now();
        const auto resourcePumpDeadline = pumpTelemetryBegin + ThumbnailPreviewResourcePumpBudget(request);
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
            kThumbnailPreviewMeshPumpCompletionsPerFrame,
            MakeThumbnailPreviewResourcePumpStopPredicate(resourcePumpDeadline),
            kAllowReadyThumbnailCompletionAfterDeadline);
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
            kThumbnailPreviewMaterialPumpCompletionsPerFrame,
            MakeThumbnailPreviewResourcePumpStopPredicate(resourcePumpDeadline),
            kAllowReadyThumbnailCompletionAfterDeadline);
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
                kThumbnailPreviewTexturePumpCompletionsPerFrame,
                MakeThumbnailPreviewResourcePumpStopPredicate(resourcePumpDeadline),
                kAllowReadyThumbnailCompletionAfterDeadline);
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

    EditorThumbnailPreviewResult Render(
        const AssetThumbnailRequest& request,
        const bool resourcesPrepared = false)
    {
        EditorThumbnailPreviewResult result;
        result.width = std::max(1u, request.requestedSize);
        result.height = result.width;
        m_lastSubmittedReadbackTicket.reset();
        PruneGlobalRetiredPreviewReadbacks();
        const auto readbackRequestKey = BuildPreviewReadbackRequestKey(request);
        const auto sceneAssemblyKey = BuildPrefabPreviewSceneAssemblyKey(request);
        m_activeRequestKey = readbackRequestKey;
        if (!m_prefabPreviewSceneAssembly.requestKey.empty() &&
            m_prefabPreviewSceneAssembly.requestKey != sceneAssemblyKey)
        {
            SuspendPrefabPreviewSceneAssembly();
            ClearPreviewObjects(false);
        }
        if (!m_textureInterestRequestKey.empty() && m_textureInterestRequestKey != readbackRequestKey)
            ReleaseTextureInterests();
        if (request.enableReadbackRing)
        {
            PollReadbackRing();
            const auto completed = m_completedReadbackPreviews.find(readbackRequestKey);
            if (completed != m_completedReadbackPreviews.end())
            {
                auto readyPreview = std::move(completed->second.preview);
                m_completedReadbackPreviews.erase(completed);
                ReleaseTextureInterests();
                ClearPreviewObjects(false);
                readyPreview.completedPendingReadback = true;
                return readyPreview;
            }

            std::optional<uint64_t> pendingRequestRevision;
            const auto findPendingRevision =
                [&readbackRequestKey, &pendingRequestRevision](
                    const std::deque<EditorThumbnailPreviewReadbackState>& queue)
            {
                const auto pending = std::find_if(
                    queue.begin(),
                    queue.end(),
                    [&readbackRequestKey](const EditorThumbnailPreviewReadbackState& state)
                    {
                        return state.active && state.requestKey == readbackRequestKey;
                    });
                if (pending != queue.end())
                    pendingRequestRevision = pending->requestRevision;
            };
            findPendingRevision(m_pendingReadbackRing);
            if (!pendingRequestRevision.has_value())
                findPendingRevision(m_deferredReadbackPersistence);
            if (pendingRequestRevision.has_value())
            {
                m_lastSubmittedReadbackTicket = EditorThumbnailPreviewReadbackTicket {
                    readbackRequestKey,
                    *pendingRequestRevision};
                result.diagnostic = "thumbnail-gpu-preview-readback-pending";
                return result;
            }
            if (m_pendingReadbackRing.size() >= kThumbnailPreviewReadbackRingCapacity &&
                m_deferredReadbackPersistence.size() >=
                    kThumbnailPreviewDeferredReadbackPersistenceCapacity)
            {
                result.diagnostic = "thumbnail-gpu-preview-readback-ring-full";
                return result;
            }
        }
        else if (m_pendingReadback.active)
        {
            if (m_pendingReadback.requestKey != readbackRequestKey)
            {
                if (!RetirePendingReadback())
                {
                    result.diagnostic = "thumbnail-gpu-preview-readback-pending";
                    return result;
                }
                ReleaseTextureInterests();
                SuspendPrefabPreviewSceneAssembly();
                ClearPreviewObjects(false);
            }
            else
            {
                m_lastSubmittedReadbackTicket = EditorThumbnailPreviewReadbackTicket {
                    readbackRequestKey,
                    m_pendingReadback.requestRevision};
                const auto pollTelemetryBegin = std::chrono::steady_clock::now();
                const auto polled = PollEditorThumbnailPreviewReadback(
                    m_pendingReadback,
                    readbackRequestKey,
                    &m_driver,
                    m_pendingReadback.requestRevision);
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

        if (!resourcesPrepared)
        {
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
                result.resourceProgressToken = resourcePump.resourceProgressToken;
                result.resourceWorkActive = resourcePump.resourceWorkActive;
                return result;
            }
            if (!resourcePump.diagnostic.empty())
            {
                ClearPreviewObjects(false);
                result.diagnostic = resourcePump.diagnostic;
                return result;
            }
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

                auto proxy = m_previewProxyPool.Acquire(
                    "Thumbnail Preview Mesh",
                    request.enablePreviewProxyPool);
                if (!proxy.has_value())
                {
                    result.diagnostic = "thumbnail-preview-proxy-pool-exhausted";
                    return result;
                }
                auto* object = proxy->Get();
                m_previewObjects.push_back(std::move(*proxy));
                if (object == nullptr)
                {
                    result.diagnostic = "thumbnail-preview-proxy-pool-invalid-lease";
                    return result;
                }
                auto* filter = object->GetComponent<NLS::Engine::Components::MeshFilter>();
                auto* renderer = object->GetComponent<NLS::Engine::Components::MeshRenderer>();
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

    EditorThumbnailPreviewSubmitResult SubmitPreview(const AssetThumbnailRequest& request)
    {
        // Render() may complete synchronously or produce no new readback at
        // all. Never return the ticket belonging to the previous submission.
        m_lastSubmittedReadbackTicket.reset();
        auto preview = Render(request);
        return {
            std::move(preview),
            m_lastSubmittedReadbackTicket
        };
    }

    EditorThumbnailPreviewSubmitResult SubmitPreparedPreview(
        const AssetThumbnailRequest& request)
    {
        m_lastSubmittedReadbackTicket.reset();
        auto preview = Render(request, true);
        return {
            std::move(preview),
            m_lastSubmittedReadbackTicket
        };
    }

    std::vector<EditorThumbnailPreviewCompletedReadback> PollCompletedReadbacks(
        const size_t maxCount)
    {
        std::vector<EditorThumbnailPreviewCompletedReadback> completed;
        if (maxCount == 0u)
            return completed;

        PruneGlobalRetiredPreviewReadbacks();
        PollReadbackRing();

        for (auto iterator = m_completedReadbackPreviews.begin();
             iterator != m_completedReadbackPreviews.end() && completed.size() < maxCount;)
        {
            const auto completedKey = iterator->first;
            completed.push_back(std::move(iterator->second));
            if (m_activeRequestKey == completedKey)
            {
                ReleaseTextureInterests();
                ClearPreviewObjects(false);
            }
            iterator = m_completedReadbackPreviews.erase(iterator);
        }

        if (completed.size() >= maxCount || !m_pendingReadback.active)
            return completed;

        const auto requestKey = m_pendingReadback.requestKey;
        const auto requestRevision = m_pendingReadback.requestRevision;
        const auto polled = PollEditorThumbnailPreviewReadback(
            m_pendingReadback,
            requestKey,
            &m_driver,
            m_pendingReadback.requestRevision);
        RecordLegacyPollState(m_pendingReadback, requestKey, polled.status);
        if (polled.status == EditorThumbnailPreviewReadbackPollStatus::Pending)
            return completed;

        const bool orphaned = m_orphanedReadbackRequestKeys.erase(
            BuildReadbackTicketIdentity(requestKey, requestRevision)) != 0u;
        m_pendingReadback = {};
        if (!orphaned)
        {
            EditorThumbnailPreviewCompletedReadback item;
            item.ticket.requestKey = requestKey;
            item.ticket.requestRevision = requestRevision;
            item.preview = std::move(polled.preview);
            item.preview.completedPendingReadback =
                polled.status == EditorThumbnailPreviewReadbackPollStatus::Ready;
            completed.push_back(std::move(item));
        }
        if (m_activeRequestKey == requestKey)
        {
            ReleaseTextureInterests();
            ClearPreviewObjects(false);
        }
        return completed;
    }

    void ReleaseCompletedPreviewResources(const AssetThumbnailRequest& request)
    {
        if (!request.importedPrefabThumbnailContinuation ||
            request.kind != AssetThumbnailKind::PrefabPreview)
        {
            return;
        }

        const auto cacheKey = BuildPreviewSnapshotCacheKey(request);
        const auto cacheEntry = std::find_if(
            m_previewSnapshotCache.begin(),
            m_previewSnapshotCache.end(),
            [&cacheKey](const PreviewSnapshotCacheEntry& entry)
            {
                return entry.key == cacheKey;
            });
        if (cacheEntry == m_previewSnapshotCache.end() ||
            cacheEntry->prepared == nullptr ||
            cacheEntry->prepared->residentSnapshotUsed)
        {
            return;
        }

        const auto prepared = cacheEntry->prepared;
        if (m_prefabPreviewSceneAssembly.prepared.get() == prepared.get())
            m_prefabPreviewSceneAssembly = {};
        for (auto iterator = m_suspendedPrefabPreviewAssemblies.begin();
             iterator != m_suspendedPrefabPreviewAssemblies.end();)
        {
            if (iterator->second.prepared.get() == prepared.get())
                iterator = m_suspendedPrefabPreviewAssemblies.erase(iterator);
            else
                ++iterator;
        }
        for (auto iterator = m_prefabPreviewDrawPrewarmStates.begin();
             iterator != m_prefabPreviewDrawPrewarmStates.end();)
        {
            const auto cachedPrepared = iterator->second.prepared.lock();
            if (cachedPrepared == nullptr || cachedPrepared.get() == prepared.get())
                iterator = m_prefabPreviewDrawPrewarmStates.erase(iterator);
            else
                ++iterator;
        }
        m_previewSnapshotCache.erase(cacheEntry);

        // Dropping the final PreparedPrefabPreview releases its preview resource
        // handles. Use the editor's existing bounded trim rather than destroying
        // hundreds of GPU resources synchronously in this polling turn.
        if (NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::EditorActions>())
            NLS_SERVICE(NLS::Editor::Core::EditorActions).ScheduleImportedResourceTrim();
    }

    bool OrphanReadback(const EditorThumbnailPreviewReadbackTicket& ticket)
    {
        if (!ticket.IsValid())
            return false;

        bool found = false;
        bool activeFound = false;
        if (m_pendingReadback.active &&
            m_pendingReadback.requestKey == ticket.requestKey &&
            (ticket.requestRevision == 0u ||
                m_pendingReadback.requestRevision == ticket.requestRevision))
        {
            found = true;
            activeFound = true;
            m_orphanedReadbackRequestKeys.insert(BuildReadbackTicketIdentity(
                m_pendingReadback.requestKey,
                m_pendingReadback.requestRevision));
        }
        const auto markQueue = [this, &ticket, &found, &activeFound](
            const std::deque<EditorThumbnailPreviewReadbackState>& queue)
        {
            for (const auto& state : queue)
            {
                if (!state.active || state.requestKey != ticket.requestKey ||
                    (ticket.requestRevision != 0u &&
                        state.requestRevision != ticket.requestRevision))
                {
                    continue;
                }
                found = true;
                activeFound = true;
                m_orphanedReadbackRequestKeys.insert(BuildReadbackTicketIdentity(
                    state.requestKey,
                    state.requestRevision));
            }
        };
        markQueue(m_pendingReadbackRing);
        markQueue(m_deferredReadbackPersistence);
        bool completedFound = false;
        if (const auto completed = m_completedReadbackPreviews.find(ticket.requestKey);
            completed != m_completedReadbackPreviews.end() &&
            (ticket.requestRevision == 0u ||
                completed->second.ticket.requestRevision == ticket.requestRevision))
        {
            m_completedReadbackPreviews.erase(completed);
            completedFound = true;
        }
        found = completedFound || found;
        if (!activeFound && !completedFound)
        {
            // No renderer-owned state matched this ticket. Do not create a
            // tombstone that could consume a future revision.
            return false;
        }
        return found;
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
        std::vector<ThumbnailPreviewProxyPool::Lease> proxies;
        // The lease owns the proxy lifetime; this list keeps the captured object
        // identities explicit for diagnostics and render-thread ownership audits.
        std::vector<NLS::Engine::GameObject*> objects;
        std::shared_ptr<NLS::Render::Resources::Material> material;
        std::vector<std::shared_ptr<NLS::Render::Resources::Material>> prefabMaterials;
        std::shared_ptr<const ResidentPrefabPreviewResources> residentResources;
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
        bool pendingTelemetryRecorded = false;
    };

    struct ThumbnailPreviewTextureUpload
    {
        uint64_t requestId = 0u;
        uint32_t width = 0u;
        uint32_t height = 0u;
    };

    struct PrefabPreviewSceneAssemblyState
    {
        std::string requestKey;
        std::shared_ptr<const PreparedPrefabPreview> prepared;
        uint64_t sceneAssemblyRevision = 0u;
        uint64_t residentBindingRevision = 0u;
        size_t nextDrawItemIndex = 0u;
        size_t nextDrawPrewarmIndex = 0u;
        size_t totalDrawPrewarmCount = 0u;
        bool drawPrewarmComplete = false;
        // The draw cursor alone is not enough to describe the scene. Proxy
        // leases are moved into the readback keep-alive after submission, so
        // a completed cursor may still have no live objects in the scene.
        bool sceneObjectsReady = false;
        Bounds combinedBounds;
        std::unordered_map<
            std::string,
            std::shared_ptr<NLS::Render::Resources::Material>> stableMaterials;
        uint64_t lastUsed = 0u;
    };

    struct PrefabPreviewDrawPrewarmState
    {
        std::weak_ptr<const PreparedPrefabPreview> prepared;
        uint64_t sceneAssemblyRevision = 0u;
        size_t nextDrawPrewarmIndex = 0u;
        size_t totalDrawPrewarmCount = 0u;
        bool drawPrewarmComplete = false;
        uint64_t lastUsed = 0u;
    };

    void PollThumbnailPreviewTextureUploads()
    {
        for (auto iterator = m_thumbnailTextureUploadRequests.begin();
            iterator != m_thumbnailTextureUploadRequests.end();)
        {
            const auto path = iterator->first;
            const auto upload = iterator->second;
            const auto result = NLS::Render::Context::DriverUIAccess::ConsumeUiRgba8TextureUploadResult(
                m_driver,
                upload.requestId);
            if (!result.ready)
            {
                ++iterator;
                continue;
            }
            iterator = m_thumbnailTextureUploadRequests.erase(iterator);

            if (!result.success || result.texture == nullptr)
            {
                m_thumbnailTexturePaths.erase(path);
                m_thumbnailTextureFallbackPaths.insert(path);
                continue;
            }

            auto texture = NLS::Render::Resources::Texture2D::WrapExternal(
                result.texture,
                result.width != 0u ? result.width : upload.width,
                result.height != 0u ? result.height : upload.height);
            if (texture == nullptr || texture->GetTextureHandle() == nullptr)
            {
                m_thumbnailTexturePaths.erase(path);
                m_thumbnailTextureFallbackPaths.insert(path);
                continue;
            }
            texture->path = path;
            texture->firstFilter = NLS::Render::Settings::ETextureFilteringMode::LINEAR;
            texture->secondFilter = NLS::Render::Settings::ETextureFilteringMode::LINEAR;
            texture->bitsPerPixel = 4u;
            texture->isMimapped = false;
            m_thumbnailTextureResources[path] = std::move(texture);
        }
    }

    void PollThumbnailPreviewTextureLoads()
    {
        // RHI texture creation is completed on the render thread. Polling this
        // queue first keeps the main-thread pump non-blocking while preserving
        // the existing reduced-artifact fallback semantics.
        PollThumbnailPreviewTextureUploads();
        PollThumbnailPreviewTextureHeaderProbes();

        size_t completedTextureCount = 0u;
        for (auto iterator = m_thumbnailTextureFutures.begin();
            iterator != m_thumbnailTextureFutures.end() &&
            completedTextureCount < kThumbnailPreviewMaterialTextureMaxInFlight;)
        {
            if (iterator->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            {
                ++iterator;
                continue;
            }

            const auto path = iterator->first;
            std::optional<NLS::Render::Assets::TextureArtifactData> artifact;
            try
            {
                artifact = iterator->second.get();
            }
            catch (...)
            {
            }
            iterator = m_thumbnailTextureFutures.erase(iterator);
            m_thumbnailTextureDeferredPaths.erase(path);
            ++completedTextureCount;

            if (artifact.has_value())
            {
                std::vector<uint8_t> uploadPixels;
                uint32_t uploadRowPitch = 0u;
                uint32_t uploadSlicePitch = 0u;
                NLS::Render::RHI::TextureFormat uploadFormat = artifact->format;
                const auto& reducedMip = artifact->mips.front();
                if (TryBuildThumbnailPreviewRgba8Pixels(*artifact, uploadPixels))
                {
                    uploadFormat = NLS::Render::RHI::TextureFormat::RGBA8;
                    uploadRowPitch = artifact->width * 4u;
                    uploadSlicePitch = uploadRowPitch * artifact->height;
                }
                else if (reducedMip.HasPixels() &&
                    artifact->width != 0u &&
                    artifact->height != 0u &&
                    artifact->mips.size() == 1u)
                {
                    const auto requiredSlicePitch = NLS::Render::RHI::CalculateTextureSlicePitch(
                        artifact->format,
                        artifact->width,
                        artifact->height,
                        1u);
                    uploadRowPitch = reducedMip.rowPitch != 0u
                        ? reducedMip.rowPitch
                        : NLS::Render::RHI::CalculateTextureRowPitch(
                            artifact->format,
                            artifact->width);
                    uploadSlicePitch = reducedMip.slicePitch != 0u
                        ? reducedMip.slicePitch
                        : requiredSlicePitch;
                    if (uploadRowPitch == 0u ||
                        uploadSlicePitch < requiredSlicePitch ||
                        reducedMip.PixelSize() < uploadSlicePitch)
                    {
                        uploadPixels.clear();
                    }
                    else
                    {
                        uploadPixels.assign(
                            reducedMip.PixelData(),
                            reducedMip.PixelData() + uploadSlicePitch);
                    }
                }
                if (!uploadPixels.empty())
                {
                    NLS::Render::Context::DriverUIAccess::Rgba8TextureUploadRequest uploadRequest;
                    uploadRequest.width = artifact->width;
                    uploadRequest.height = artifact->height;
                    uploadRequest.rgbaPixels = std::move(uploadPixels);
                    uploadRequest.debugName = "ThumbnailPreviewTexture:" + path;
                    uploadRequest.colorSpace = artifact->colorSpace ==
                        NLS::Render::Assets::TextureArtifactColorSpace::Srgb
                        ? NLS::Render::RHI::TextureColorSpace::SRGB
                        : NLS::Render::RHI::TextureColorSpace::Linear;
                    uploadRequest.format = uploadFormat;
                    uploadRequest.rowPitch = uploadRowPitch;
                    uploadRequest.slicePitch = uploadSlicePitch;
                    const auto requestId = NLS::Render::Context::DriverUIAccess::RequestUiRgba8TextureUpload(
                        m_driver,
                        std::move(uploadRequest));
                    if (requestId != 0u)
                    {
                        m_thumbnailTextureUploadRequests[path] = {
                            requestId,
                            artifact->width,
                            artifact->height
                        };
                        NLS::Core::Assets::RecordArtifactLoadTelemetry({
                            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTextureUploadEnqueue,
                            {},
                            static_cast<size_t>(artifact->width) *
                                static_cast<size_t>(artifact->height) * 4u,
                            path + "|reduced-rgba8-rhi-queue"
                        });
                        continue;
                    }
                }

                // The reduced path is an optimization. If the device cannot
                // create or upload the native format, retain the authoritative
                // manager fallback below.
                auto* texture = NLS::Render::Resources::Loaders::TextureLoader::CreateFromArtifact(
                    *artifact,
                    NLS::Render::Settings::ETextureFilteringMode::LINEAR,
                    NLS::Render::Settings::ETextureFilteringMode::LINEAR,
                    false);
                if (texture != nullptr && texture->GetTextureHandle() != nullptr)
                {
                    texture->path = path;
                    m_thumbnailTextureResources[path] =
                        std::unique_ptr<NLS::Render::Resources::Texture2D>(texture);
                    continue;
                }
                if (texture != nullptr)
                    delete texture;
            }

            // The reduced path is an optimization, never a correctness
            // requirement. Let the existing manager load the authoritative
            // texture when the reduced artifact cannot be created.
            m_thumbnailTexturePaths.erase(path);
            m_thumbnailTextureFallbackPaths.insert(path);
        }

        StartDeferredThumbnailPreviewTextureLoads();
        ReconcileThumbnailPreviewTexturePaths();
    }

    void ReconcileThumbnailPreviewTexturePaths()
    {
        for (auto iterator = m_thumbnailTexturePaths.begin();
            iterator != m_thumbnailTexturePaths.end();)
        {
            const auto& path = *iterator;
            const bool resourceReady =
                m_thumbnailTextureResources.find(path) != m_thumbnailTextureResources.end();
            const bool retained = ShouldRetainThumbnailPreviewTexturePath(
                m_thumbnailTextureHeaderProbePaths.find(path) !=
                    m_thumbnailTextureHeaderProbePaths.end(),
                m_thumbnailTextureHeaderProbeFutures.find(path) !=
                    m_thumbnailTextureHeaderProbeFutures.end(),
                m_thumbnailTextureDeferredPaths.find(path) !=
                    m_thumbnailTextureDeferredPaths.end(),
                m_thumbnailTextureFutures.find(path) !=
                    m_thumbnailTextureFutures.end(),
                m_thumbnailTextureUploadRequests.find(path) !=
                    m_thumbnailTextureUploadRequests.end(),
                resourceReady);
            if (retained)
            {
                ++iterator;
                continue;
            }

            // A path in the umbrella set without an owner cannot make progress.
            // Move it to the explicit fallback set so material binding can use
            // the default sampler or the authoritative TextureManager path.
            const auto stalePath = *iterator;
            iterator = m_thumbnailTexturePaths.erase(iterator);
            m_thumbnailTextureFallbackPaths.insert(stalePath);
        }
    }

    void PollThumbnailPreviewTextureHeaderProbes()
    {
        size_t completedProbeCount = 0u;
        for (auto iterator = m_thumbnailTextureHeaderProbeFutures.begin();
            iterator != m_thumbnailTextureHeaderProbeFutures.end() &&
            completedProbeCount < kThumbnailPreviewMaterialTextureMaxInFlight;)
        {
            if (iterator->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            {
                ++iterator;
                continue;
            }

            const auto path = iterator->first;
            std::optional<NLS::Render::Assets::TextureArtifactHeaderPreview> header;
            try
            {
                header = iterator->second.get();
            }
            catch (...)
            {
            }
            iterator = m_thumbnailTextureHeaderProbeFutures.erase(iterator);
            m_thumbnailTextureHeaderProbePaths.erase(path);
            ++completedProbeCount;

            if (header.has_value() &&
                (std::max)(header->width, header->height) >
                    kThumbnailPreviewMaterialTextureMaxDimension)
            {
                m_thumbnailTextureDeferredPaths.insert(path);
                continue;
            }

            m_thumbnailTexturePaths.erase(path);
            m_thumbnailTextureFallbackPaths.insert(path);
        }
    }

    void StartDeferredThumbnailPreviewTextureLoads()
    {
        size_t inFlightCount =
            m_thumbnailTextureHeaderProbeFutures.size() +
            m_thumbnailTextureFutures.size() +
            m_thumbnailTextureUploadRequests.size();
        for (auto iterator = m_thumbnailTextureHeaderProbePaths.begin();
            iterator != m_thumbnailTextureHeaderProbePaths.end() &&
            inFlightCount < kThumbnailPreviewMaterialTextureMaxInFlight;)
        {
            const auto path = *iterator;
            if (m_thumbnailTextureHeaderProbeFutures.find(path) !=
                m_thumbnailTextureHeaderProbeFutures.end() ||
                m_thumbnailTextureFutures.find(path) != m_thumbnailTextureFutures.end() ||
                m_thumbnailTextureUploadRequests.find(path) != m_thumbnailTextureUploadRequests.end() ||
                m_thumbnailTextureResources.find(path) != m_thumbnailTextureResources.end())
            {
                iterator = m_thumbnailTextureHeaderProbePaths.erase(iterator);
                continue;
            }
            try
            {
                m_thumbnailTextureHeaderProbeFutures.emplace(
                    path,
                    ScheduleThumbnailPreviewTextureHeaderProbe(path));
                ++inFlightCount;
                iterator = m_thumbnailTextureHeaderProbePaths.erase(iterator);
            }
            catch (...)
            {
                m_thumbnailTexturePaths.erase(path);
                m_thumbnailTextureFallbackPaths.insert(path);
                iterator = m_thumbnailTextureHeaderProbePaths.erase(iterator);
            }
        }

        for (auto iterator = m_thumbnailTextureDeferredPaths.begin();
            iterator != m_thumbnailTextureDeferredPaths.end() &&
            inFlightCount < kThumbnailPreviewMaterialTextureMaxInFlight;)
        {
            const auto path = *iterator;
            if (m_thumbnailTextureFutures.find(path) != m_thumbnailTextureFutures.end() ||
                m_thumbnailTextureUploadRequests.find(path) != m_thumbnailTextureUploadRequests.end() ||
                m_thumbnailTextureResources.find(path) != m_thumbnailTextureResources.end())
            {
                iterator = m_thumbnailTextureDeferredPaths.erase(iterator);
                continue;
            }
            try
            {
                m_thumbnailTextureFutures.emplace(
                    path,
                    ScheduleThumbnailPreviewTextureArtifactLoad(path));
                ++inFlightCount;
                iterator = m_thumbnailTextureDeferredPaths.erase(iterator);
            }
            catch (...)
            {
                m_thumbnailTexturePaths.erase(path);
                m_thumbnailTextureFallbackPaths.insert(path);
                iterator = m_thumbnailTextureDeferredPaths.erase(iterator);
            }
        }
    }

    void EnsureThumbnailPreviewTexturePaths(
        const NLS::Render::Resources::Material& material)
    {
        if (!NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::TextureManager>())
            return;

        auto& textureManager = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager);
        for (const auto& [unusedName, path] : material.GetTextureResourcePaths())
        {
            (void)unusedName;
            if (path.empty())
                continue;
            const auto genericPath = ToGenericPath(path);
            if (genericPath.empty() ||
                m_thumbnailTexturePaths.find(genericPath) != m_thumbnailTexturePaths.end() ||
                m_thumbnailTextureFallbackPaths.find(genericPath) != m_thumbnailTextureFallbackPaths.end())
            {
                continue;
            }
            const auto cached = textureManager.TryGetArtifactResource(path);
            if (cached.has_value() && *cached != nullptr &&
                (*cached)->GetTextureHandle() != nullptr)
            {
                continue;
            }
            if (m_thumbnailTextureResources.find(genericPath) != m_thumbnailTextureResources.end() ||
                m_thumbnailTextureFutures.find(genericPath) != m_thumbnailTextureFutures.end() ||
                m_thumbnailTextureUploadRequests.find(genericPath) != m_thumbnailTextureUploadRequests.end())
            {
                m_thumbnailTexturePaths.insert(genericPath);
                continue;
            }
            if (m_thumbnailTexturePaths.size() >=
                kThumbnailPreviewMaterialTextureCacheCapacity)
            {
                m_thumbnailTextureFallbackPaths.insert(genericPath);
                continue;
            }

            m_thumbnailTexturePaths.insert(genericPath);
            m_thumbnailTextureHeaderProbePaths.insert(genericPath);
        }
        StartDeferredThumbnailPreviewTextureLoads();
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

    bool TryRefreshResidentPrefabPreviewResources(
        const AssetThumbnailRequest& request,
        const std::shared_ptr<const PreparedPrefabPreview>& prepared)
    {
        if (prepared == nullptr || !prepared->residentSnapshotUsed ||
            !request.residentPrefabPreviewSource.has_value() ||
            !NLS::Core::ServiceLocator::Contains<
                NLS::Core::ResourceManagement::MeshManager>() ||
            !NLS::Core::ServiceLocator::Contains<
                NLS::Core::ResourceManagement::MaterialManager>())
        {
            return false;
        }
        const auto registry = request.residentPrefabPreviewSource->registry.lock();
        if (registry == nullptr)
            return false;
        auto lease = registry->Acquire(
            request.residentPrefabPreviewSource->runtimeCacheIdentity,
            request.residentPrefabPreviewSource->freshnessFingerprint,
            true);
        if (!lease.has_value() || lease->Resources() == nullptr)
            return false;

        const auto snapshot = lease->Snapshot();
        if (snapshot == nullptr || snapshot->drawItems.empty())
            return false;

        uint64_t residentSnapshotRevision = request.residentPreviewRevision;
        if (const auto state = registry->GetSnapshotState(
                request.residentPrefabPreviewSource->runtimeCacheIdentity,
                request.residentPrefabPreviewSource->freshnessFingerprint);
            state.has_value())
        {
            residentSnapshotRevision = state->revision;
        }

        auto& meshManager = NLS_SERVICE(NLS::Core::ResourceManagement::MeshManager);
        auto& materialManager = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager);
        auto* textureManager = NLS::Core::ServiceLocator::Contains<
            NLS::Core::ResourceManagement::TextureManager>()
            ? &NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager)
            : nullptr;
        if (!lease->Resources()->IsValidFor(meshManager, materialManager, textureManager))
            return false;
        if (prepared->residentResources == lease->Resources() &&
            prepared->residentSnapshotRevision == residentSnapshotRevision)
            return true;

        prepared->residentLease = std::move(lease);
        prepared->residentResources = prepared->residentLease->Resources();
        std::string assemblyMismatchReason;
        const bool reuseSceneAssembly = CanReusePrefabPreviewSceneAssembly(
            prepared->snapshot,
            *snapshot,
            &assemblyMismatchReason);
        prepared->snapshot = *snapshot;
        prepared->canonicalSnapshot = snapshot;
        prepared->residentSnapshotRevision = residentSnapshotRevision;
        prepared->resourcePlan = BuildResidentPrefabPreviewResourcePlan(prepared->snapshot);
        prepared->resourcePumpState = {};
        if (!reuseSceneAssembly)
            ++prepared->sceneAssemblyRevision;
        ++prepared->resourcePlanRevision;
        prepared->diagnostic.clear();
        NLS_LOG_INFO(
            "resident-prefab-preview-refresh|asset=" + request.assetId.ToString() +
            "|source=" + request.sourceAssetPath +
            "|subAsset=" + request.subAssetKey +
            "|drawItems=" + std::to_string(snapshot->drawItems.size()) +
            "|resourceDrawItems=" +
            std::to_string(prepared->residentResources->drawItems.size()) +
            "|sourceExpected=" +
            std::to_string(prepared->residentResources->sourceExpectedDrawItemCount) +
            "|complete=" + std::to_string(
                prepared->residentResources->IsCompleteForSource() ? 1u : 0u) +
            "|residentRevision=" + std::to_string(prepared->residentSnapshotRevision) +
            "|revision=" + std::to_string(prepared->resourcePlanRevision) +
            "|assemblyRevision=" + std::to_string(prepared->sceneAssemblyRevision) +
            "|assemblyReused=" + std::to_string(reuseSceneAssembly ? 1u : 0u) +
            "|assemblyMismatch=" + assemblyMismatchReason);
        return true;
    }

    void BindPrefabPreviewDrawItemMaterials(
        const AssetThumbnailRequest& request,
        const std::shared_ptr<const PreparedPrefabPreview>& prepared,
        PrefabPreviewSceneAssemblyState& assembly,
        const PrefabPreviewResourcePlanDrawItem& planned,
        NLS::Engine::Components::MeshRenderer& renderer)
    {
        renderer.FillEmptySlotsWithMaterial(DefaultMaterial());
        const auto& resourceState = prepared->resourcePumpState;
        for (size_t slot = 0u; slot < planned.materialLoadPaths.size(); ++slot)
        {
            renderer.SetMaterialAtIndex(static_cast<uint32_t>(slot), DefaultMaterial());
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
            if (material == nullptr)
                continue;

            auto& stableMaterial = assembly.stableMaterials[materialPath];
            if (stableMaterial == nullptr)
            {
                stableMaterial = GetStablePreviewMaterial(
                    *material,
                    request.colorSpaceMode,
                    request.hdrMode);
            }
            if (prepared->residentResources != nullptr)
            {
                BindResidentPreviewMaterialTextures(
                    *stableMaterial,
                    *prepared->residentResources);
            }
            renderer.SetMaterialAtIndex(static_cast<uint32_t>(slot), *stableMaterial);
        }
    }

    EditorThumbnailPreviewResourcePumpResult PumpPreparedPrefabResources(
        const AssetThumbnailRequest& request,
        PrefabPreviewResourcePumpState& state,
        NLS::Core::ResourceManagement::MeshManager& meshManager,
        NLS::Core::ResourceManagement::MaterialManager& materialManager,
        NLS::Core::ResourceManagement::TextureManager* textureManager,
        const ResidentPrefabPreviewRegistry::Lease* preparedPayloadLease,
        const bool allowNewResourceRequests)
    {
        EditorThumbnailPreviewResourcePumpResult result;
        result.supported = true;
        const auto telemetryBegin = std::chrono::steady_clock::now();
        const auto resourcePumpBudget = ThumbnailPreviewResourcePumpBudget(request);
        const auto pumpDeadline = telemetryBegin + resourcePumpBudget;
        auto materialPhaseDeadline = pumpDeadline;
        auto texturePhaseDeadline = pumpDeadline;
        size_t inspectedResourceCount = 0u;
        size_t meshRequestCallCount = 0u;
        size_t meshPendingAfterRequestCount = 0u;
        size_t meshNotPendingAfterRequestCount = 0u;
        size_t materialRequestCallCount = 0u;
        size_t materialPendingAfterRequestCount = 0u;
        size_t materialNotPendingAfterRequestCount = 0u;
        size_t materialResourceTableBusyCount = 0u;
        size_t materialBlockingLookupCount = 0u;
        std::unordered_map<std::string, NLS::Render::Resources::Texture2D*> readyTextureCache;
        readyTextureCache.reserve(kThumbnailPreviewPrefabResourceInspectionsPerTypePerFrame);
        std::string meshIdentityTelemetry;
        auto finalize = [&](EditorThumbnailPreviewResourcePumpResult finalized)
        {
            finalized.resourceProgressToken =
                BuildPrefabPreviewResourceProgressToken(state);
            finalized.resourceWorkActive =
                !state.meshRequestPaths.empty() ||
                !state.materialRequestPaths.empty() ||
                !state.meshPathsToPump.empty() ||
                !state.materialPathsToPump.empty() ||
                !state.pendingTexturePaths.empty() ||
                !state.pendingThumbnailTexturePaths.empty();
            if (meshIdentityTelemetry.empty() && !state.unresolvedMeshPaths.empty())
            {
                const auto& firstMeshPath = state.unresolvedMeshPaths.front();
                // Keep telemetry construction side-effect free. Resolving the
                // artifact path and probing manager indices here can contend
                // with resource promotion and turn a bounded pump into a
                // hundreds-of-milliseconds main-thread stall.
                meshIdentityTelemetry = "mesh-identity|request=" + firstMeshPath +
                    "|state=deferred";
            }
            auto telemetryPath = request.sourceAssetPath + "|" + request.subAssetKey +
                "|bounded-prefab-resource-inspections";
            if (!meshIdentityTelemetry.empty())
                telemetryPath += "|" + meshIdentityTelemetry;
            if (!finalized.diagnostic.empty())
                telemetryPath += "|diag=" + finalized.diagnostic;
            const auto meshDiagnostics =
                NLS::Core::ResourceManagement::MeshManager::GetAsyncArtifactRequestDiagnostics();
            const auto meshOwnerDiagnostics = meshManager.GetAsyncArtifactRequestDiagnosticsForOwner();
            const auto runtimeUploadDiagnostics =
                NLS::Render::Context::DriverResourceAccess::GetMeshRuntimeUploadDiagnostics(m_driver);
            const auto materialDiagnostics =
                NLS::Core::ResourceManagement::MaterialManager::GetAsyncArtifactRequestDiagnostics();
            const auto textureDiagnostics = textureManager != nullptr
                ? NLS::Core::ResourceManagement::TextureManager::GetAsyncArtifactRequestDiagnostics()
                : NLS::Core::ResourceManagement::AsyncArtifactRequestDiagnostics {};
            telemetryPath += "|meshManagerTotal=" + std::to_string(meshDiagnostics.totalRequests) +
                "|meshManagerActive=" + std::to_string(meshDiagnostics.activeRequests) +
                "|meshManagerQueued=" + std::to_string(meshDiagnostics.queuedRequests) +
                "|meshManagerReady=" + std::to_string(meshDiagnostics.readyRequests) +
                "|meshManagerFailed=" + std::to_string(meshDiagnostics.failedRequests) +
                "|meshManagerPreview=" + std::to_string(meshDiagnostics.previewRequests) +
                "|meshManagerPreviewActive=" + std::to_string(meshDiagnostics.previewActiveRequests) +
                "|meshManagerPreviewQueued=" + std::to_string(meshDiagnostics.previewQueuedRequests) +
                 "|meshRuntimeUploadPending=" +
                 std::to_string(meshDiagnostics.runtimeUploadPendingRequests) +
                 "|rhiUploadPending=" +
                 std::to_string(runtimeUploadDiagnostics.pendingRequestCount) +
                 "|rhiUploadCompleted=" +
                 std::to_string(runtimeUploadDiagnostics.completedResultCount) +
                 "|rhiUploadRequested=" +
                 std::to_string(runtimeUploadDiagnostics.requestedCount) +
                 "|rhiUploadRecordTicks=" +
                 std::to_string(runtimeUploadDiagnostics.recordTickCount) +
                 "|rhiUploadIdleTicks=" +
                 std::to_string(runtimeUploadDiagnostics.rhiIdleTickCount) +
                 "|rhiUploadRecorded=" +
                 std::to_string(runtimeUploadDiagnostics.recordedCount) +
                 "|rhiUploadConsumed=" +
                 std::to_string(runtimeUploadDiagnostics.consumedCount) +
                 "|rhiUploadFailed=" +
                 std::to_string(runtimeUploadDiagnostics.failedCount) +
                  "|rhiUploadCanceled=" +
                  std::to_string(runtimeUploadDiagnostics.canceledCount) +
                  "|rhiUploadDriver=" +
                  std::to_string(runtimeUploadDiagnostics.driverInstanceIdentity) +
                  "|rhiUploadLastRequestedId=" +
                  std::to_string(runtimeUploadDiagnostics.lastRequestedRequestId) +
                  "|rhiUploadLastSwappedBatch=" +
                  std::to_string(runtimeUploadDiagnostics.lastSwappedBatchCount) +
                  "|rhiUploadEmptyRecordTicks=" +
                  std::to_string(runtimeUploadDiagnostics.emptyRecordTickCount) +
                  "|rhiUploadLastRecordedId=" +
                  std::to_string(runtimeUploadDiagnostics.lastRecordedRequestId) +
                  "|rhiUploadLastConsumedId=" +
                  std::to_string(runtimeUploadDiagnostics.lastConsumedRequestId) +
                  "|rhiUploadLastCanceledId=" +
                  std::to_string(runtimeUploadDiagnostics.lastCanceledRequestId) +
                  "|meshManagerMaxActive=" + std::to_string(meshDiagnostics.maxActiveRequests) +
                 "|meshOwnerTotal=" + std::to_string(meshOwnerDiagnostics.totalRequests) +
                 "|meshOwnerActive=" + std::to_string(meshOwnerDiagnostics.activeRequests) +
                 "|meshOwnerQueued=" + std::to_string(meshOwnerDiagnostics.queuedRequests) +
                 "|meshOwnerReady=" + std::to_string(meshOwnerDiagnostics.readyRequests) +
                 "|meshOwnerFailed=" + std::to_string(meshOwnerDiagnostics.failedRequests) +
                 "|meshOwnerPreview=" + std::to_string(meshOwnerDiagnostics.previewRequests) +
                 "|meshOwnerPreviewActive=" +
                 std::to_string(meshOwnerDiagnostics.previewActiveRequests) +
                 "|meshOwnerPreviewQueued=" +
                 std::to_string(meshOwnerDiagnostics.previewQueuedRequests) +
                 "|meshOwnerRuntimeUploadPending=" +
                 std::to_string(meshOwnerDiagnostics.runtimeUploadPendingRequests) +
                 "|meshHasExplicitRHI=" +
                 std::to_string(
                     NLS::Render::Context::DriverRendererAccess::HasExplicitRHI(m_driver) ? 1u : 0u) +
                 "|meshRequestCalls=" + std::to_string(meshRequestCallCount) +
                "|meshPendingAfterRequest=" + std::to_string(meshPendingAfterRequestCount) +
                 "|meshNotPendingAfterRequest=" +
                 std::to_string(meshNotPendingAfterRequestCount) +
                 "|stateUnresolvedMesh=" + std::to_string(state.unresolvedMeshPaths.size()) +
                 "|stateResolvedMesh=" + std::to_string(state.resolvedMeshes.size()) +
                 "|stateMeshRequestPaths=" + std::to_string(state.meshRequestPaths.size()) +
                 "|stateMeshPathsToPump=" + std::to_string(state.meshPathsToPump.size()) +
                "|planMeshPaths=" + std::to_string(state.resourcePlanMeshPathCount) +
                "|materialManagerTotal=" + std::to_string(materialDiagnostics.totalRequests) +
                "|materialManagerActive=" + std::to_string(materialDiagnostics.activeRequests) +
                "|materialManagerQueued=" + std::to_string(materialDiagnostics.queuedRequests) +
                "|materialManagerReady=" + std::to_string(materialDiagnostics.readyRequests) +
                "|materialManagerFailed=" + std::to_string(materialDiagnostics.failedRequests) +
                "|materialManagerPreview=" + std::to_string(materialDiagnostics.previewRequests) +
                "|materialManagerPreviewActive=" +
                std::to_string(materialDiagnostics.previewActiveRequests) +
                "|materialManagerPreviewQueued=" +
                std::to_string(materialDiagnostics.previewQueuedRequests) +
                "|materialManagerMaxActive=" +
                std::to_string(materialDiagnostics.maxActiveRequests) +
                "|materialRequestCalls=" + std::to_string(materialRequestCallCount) +
                "|materialResourceTableBusy=" +
                std::to_string(materialResourceTableBusyCount) +
                "|materialBlockingLookups=" +
                std::to_string(materialBlockingLookupCount) +
                "|materialPendingAfterRequest=" +
                std::to_string(materialPendingAfterRequestCount) +
                 "|stateUnresolvedMaterial=" +
                 std::to_string(state.unresolvedMaterialPaths.size()) +
                 "|stateResolvedMaterial=" +
                 std::to_string(state.resolvedMaterials.size()) +
                 "|stateUnavailableMaterialPaths=" +
                 std::to_string(state.unavailableMaterialPaths.size()) +
                 "|stateMaterialPathsToPump=" +
                std::to_string(state.materialPathsToPump.size()) +
                 "|stateMaterialsAwaitingTextures=" +
                 std::to_string(state.materialsAwaitingTextures.size()) +
                  "|statePendingThumbnailTexturePaths=" +
                  std::to_string(state.pendingThumbnailTexturePaths.size()) +
                  "|stateUnavailableTexturePaths=" +
                  std::to_string(state.unavailableTexturePaths.size()) +
                  "|stateRequestedTexturePaths=" +
                std::to_string(state.requestedTexturePaths.size()) +
                "|planMaterialPaths=" +
                std::to_string(state.resourcePlanMaterialPathCount) +
                "|materialNotPendingAfterRequest=" +
                std::to_string(materialNotPendingAfterRequestCount) +
                "|textureManagerTotal=" + std::to_string(textureDiagnostics.totalRequests) +
                "|textureManagerActive=" + std::to_string(textureDiagnostics.activeRequests) +
                "|textureManagerQueued=" + std::to_string(textureDiagnostics.queuedRequests) +
                "|textureManagerReady=" + std::to_string(textureDiagnostics.readyRequests) +
                "|textureManagerFailed=" + std::to_string(textureDiagnostics.failedRequests) +
                "|textureManagerPreview=" + std::to_string(textureDiagnostics.previewRequests) +
                "|textureManagerPreviewActive=" +
                std::to_string(textureDiagnostics.previewActiveRequests) +
                "|textureManagerPreviewQueued=" +
                std::to_string(textureDiagnostics.previewQueuedRequests) +
                 "|stateTexturePathsToPump=" +
                 std::to_string(state.texturePathsToPump.size());
            telemetryPath +=
                "|thumbnailTexturePaths=" +
                std::to_string(m_thumbnailTexturePaths.size()) +
                "|thumbnailTextureHeaderQueued=" +
                std::to_string(m_thumbnailTextureHeaderProbePaths.size()) +
                "|thumbnailTextureHeaderInFlight=" +
                std::to_string(m_thumbnailTextureHeaderProbeFutures.size()) +
                "|thumbnailTextureDeferred=" +
                std::to_string(m_thumbnailTextureDeferredPaths.size()) +
                "|thumbnailTextureArtifactInFlight=" +
                std::to_string(m_thumbnailTextureFutures.size()) +
                "|thumbnailTextureUploadInFlight=" +
                std::to_string(m_thumbnailTextureUploadRequests.size()) +
                "|thumbnailTextureResources=" +
                std::to_string(m_thumbnailTextureResources.size()) +
                "|thumbnailTextureFallback=" +
                std::to_string(m_thumbnailTextureFallbackPaths.size());
            // Counts alone cannot distinguish a genuinely slow artifact from
            // a path-identity mismatch. Keep a bounded sample of the exact
            // paths selected for the manager pump so a stalled continuation
            // can be diagnosed without dumping the whole prefab plan.
            size_t meshPumpPathIndex = 0u;
            for (const auto& path : state.meshPathsToPump)
            {
                if (meshPumpPathIndex >= 4u)
                    break;
                telemetryPath += "|meshPumpPath" + std::to_string(meshPumpPathIndex) + "=" + path;
                ++meshPumpPathIndex;
            }
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

        if (!state.meshIdentityDiagnosticRecorded && !state.unresolvedMeshPaths.empty())
        {
            const auto& firstMeshPath = state.unresolvedMeshPaths.front();
            meshIdentityTelemetry = "mesh-identity|request=" + firstMeshPath +
                "|state=deferred";
            state.meshIdentityDiagnosticRecorded = true;
        }

        const auto meshInspectionBegin = std::chrono::steady_clock::now();
        state.meshPathsToPump.clear();
        state.pendingThumbnailTexturePaths.clear();
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

            std::string registeredMeshPath = path;
            bool meshLookupBusy = false;
            auto* mesh = FindRegisteredPreviewMesh(
                meshManager,
                path,
                &registeredMeshPath,
                &meshLookupBusy);
            if (meshLookupBusy)
            {
                // Scene registration owns the resource table while it installs
                // a mesh and its artifact-path index. Never wait for that
                // transaction from the thumbnail pump. Keep the cursor fair:
                // one contended path must not prevent other mesh dependencies
                // from starting in this same continuation.
                state.unresolvedMeshPaths.push_back(std::move(path));
                continue;
            }
            if (mesh != nullptr)
            {
                state.resolvedMeshes[path] = mesh;
                state.meshRequestPaths.erase(path);
                if (state.resourceLifetimeRegistry != nullptr)
                {
                    state.meshHandles.insert_or_assign(
                        path,
                        meshManager.AcquireMeshHandle(
                            *state.resourceLifetimeRegistry,
                            state.ownerToken,
                            registeredMeshPath,
                            NLS::Core::ResourceManagement::ResourceLifetimeOwnerKind::Preview));
                }
                continue;
            }
            // The exact-path probe is O(1) and is sufficient for requests
            // started by this continuation. Fall back to the equivalent-path
            // probe only for an already-known request that may have been
            // accepted by scene loading under another spelling.
            const bool requestKnown = state.meshRequestPaths.find(path) !=
                state.meshRequestPaths.end();
            bool pending = meshManager.IsAsyncArtifactLoadPendingExactPath(path);
            if (requestKnown && !pending)
            {
                pending = meshManager.IsAsyncArtifactLoadPending(path);
                if (!pending)
                    state.meshRequestPaths.erase(path);
            }
            if (pending)
                state.meshRequestPaths.insert(path);
            if (ShouldYieldPrefabMeshDependencyInspection(
                    pending,
                    meshRequestStartCount,
                    kThumbnailPreviewPrefabMeshRequestStartsPerFrame))
            {
                // Preserve a round-robin cursor. Without yielding here, the
                // scan requeues every not-yet-started path and begins at the
                // same first pending mesh on every pump.
                state.unresolvedMeshPaths.push_back(std::move(path));
                break;
            }
            if (!pending &&
                meshRequestStartCount < kThumbnailPreviewPrefabMeshRequestStartsPerFrame)
            {
                if (!allowNewResourceRequests)
                {
                    // Scene restoration may own this dependency without
                    // having published it in the manager yet. Keep the
                    // cursor alive and wait for that shared transaction
                    // instead of starting a duplicate preview load.
                    state.unresolvedMeshPaths.push_back(std::move(path));
                    continue;
                }
                ++meshRequestStartCount;
                ++meshRequestCallCount;
                auto preparedPayload = preparedPayloadLease != nullptr
                    ? preparedPayloadLease->TakePreparedMeshPayload(path)
                    : nullptr;
                mesh = ResolvePreviewMesh(
                    meshManager,
                    path,
                    std::move(preparedPayload));
                if (mesh == nullptr)
                {
                    // ResolvePreviewMesh may have joined an equivalent-path
                    // request owned by scene loading. Pay the path scan once
                    // after accepting this new dependency, then keep the
                    // result in meshRequestPaths for later pumps.
                    bool pendingAfterRequest =
                        meshManager.IsAsyncArtifactLoadPendingExactPath(path);
                    if (!pendingAfterRequest)
                        pendingAfterRequest = meshManager.IsAsyncArtifactLoadPending(path);
                    if (pendingAfterRequest)
                    {
                        state.meshRequestPaths.insert(path);
                        pending = true;
                        ++meshPendingAfterRequestCount;
                    }
                    else if (!meshManager.IsAsyncArtifactLoadFailed(path))
                        ++meshNotPendingAfterRequestCount;
                }
            }
            if (mesh != nullptr)
            {
                state.resolvedMeshes[path] = mesh;
                state.meshRequestPaths.erase(path);
                FindRegisteredPreviewMesh(meshManager, path, &registeredMeshPath);
                if (state.resourceLifetimeRegistry != nullptr)
                {
                    state.meshHandles.insert_or_assign(
                        path,
                        meshManager.AcquireMeshHandle(
                            *state.resourceLifetimeRegistry,
                            state.ownerToken,
                            registeredMeshPath,
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
            if (pending)
                state.meshPathsToPump.insert(std::move(path));
        }
        inspectedResourceCount += inspectedMeshCount;
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMeshInspection,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - meshInspectionBegin),
            inspectedMeshCount,
            request.sourceAssetPath + "|" + request.subAssetKey + "|prefab-resource-inspection"
        });

        if (!state.meshPathsToPump.empty())
        {
            const auto meshPumpBegin = std::chrono::steady_clock::now();
            meshManager.PumpAsyncLoadsForPaths(
                state.meshPathsToPump,
                kThumbnailPreviewPrefabMeshPumpCompletionsPerFrame,
                MakeThumbnailPreviewResourcePumpStopPredicate(pumpDeadline),
                kAllowReadyThumbnailCompletionAfterDeadline);
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMeshDependencies,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - meshPumpBegin),
                state.meshPathsToPump.size(),
                request.sourceAssetPath + "|" + request.subAssetKey + "|prefab-resource-pump"
            });
            for (const auto& path : state.meshPathsToPump)
            {
                if (!meshManager.IsAsyncArtifactLoadFailed(path))
                    continue;
                state.terminalDiagnostic = BuildThumbnailGpuPreviewMeshLoadFailedDiagnostic(1u);
                break;
            }
        }
        // A runtime mesh upload can be recorded by the RHI after the path
        // inspection cursor has been drained. Retire that completion with an
        // empty path window so a ready upload cannot keep this preview in
        // WaitingForResources indefinitely. An explicit path pump above
        // already performs the same retirement work; do not follow it with an
        // expensive manager-wide completion scan in the same turn.
        if (ShouldPumpPrefabRuntimeUploadRetirement(
                state.meshPathsToPump.size(),
                state.meshRequestPaths.size()))
        {
            const auto meshCompletionPumpBegin = std::chrono::steady_clock::now();
            meshManager.PumpAsyncLoadsForPaths(
                {},
                kThumbnailPreviewPrefabMeshPumpCompletionsPerFrame,
                MakeThumbnailPreviewResourcePumpStopPredicate(pumpDeadline),
                kAllowReadyThumbnailCompletionAfterDeadline);
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMeshDependencies,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - meshCompletionPumpBegin),
                0u,
                request.sourceAssetPath + "|" + request.subAssetKey +
                    "|prefab-runtime-upload-retire"
            });
        }
        if (!state.terminalDiagnostic.empty())
        {
            result.diagnostic = state.terminalDiagnostic;
            return finalize(std::move(result));
        }

        if (ShouldResetPrefabPreviewPhaseDeadline(
                state.unresolvedMeshPaths.size(),
                state.meshRequestPaths.size(),
                state.meshPathsToPump.size()))
        {
            // Fixed polling and telemetry for a completed phase must not
            // consume the next dependency phase's bounded work window.
            materialPhaseDeadline = std::chrono::steady_clock::now() + resourcePumpBudget;
        }

        // Consume completions from the previous inspection before doing any
        // more path resolution.  FindRegisteredMaterialByEquivalentArtifactPath
        // and the failure checks can touch the artifact-path index and the
        // filesystem; when they consume the small inspection budget first, the
        // old implementation reached PumpAsyncLoadsForPaths only after its
        // deadline and left ready futures stranded indefinitely.
        const auto previouslyPumpedMaterialPaths = state.materialPathsToPump;
        if (!previouslyPumpedMaterialPaths.empty())
        {
            const auto materialPumpBegin = std::chrono::steady_clock::now();
            materialManager.PumpAsyncLoadsForPaths(
                previouslyPumpedMaterialPaths,
                kThumbnailPreviewMaterialPumpCompletionsPerFrame,
                MakeThumbnailPreviewResourcePumpStopPredicate(materialPhaseDeadline),
                kAllowReadyThumbnailCompletionAfterDeadline);
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialDependencies,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - materialPumpBegin),
                previouslyPumpedMaterialPaths.size(),
                request.sourceAssetPath + "|" + request.subAssetKey +
                    "|prefab-resource-pump-previous"
            });
        }

        const auto materialInspectionBegin = std::chrono::steady_clock::now();
        state.materialPathsToPump.clear();
        const auto materialInspectionCount = (std::min)(
            state.unresolvedMaterialPaths.size(),
            kThumbnailPreviewPrefabResourceInspectionsPerTypePerFrame);
        size_t inspectedMaterialCount = 0u;
        for (size_t index = 0u;
            index < materialInspectionCount &&
            ShouldContinuePrefabPreviewResourceInspection(
                index,
                inspectedResourceCount,
                std::chrono::steady_clock::now() >= materialPhaseDeadline);
            ++index)
        {
            ++inspectedMaterialCount;
            auto path = std::move(state.unresolvedMaterialPaths.front());
            state.unresolvedMaterialPaths.pop_front();

            auto resolvedMaterialPath = state.resolvedMaterialPaths.find(path);
            auto deferMaterialPath = [&state, &path]()
            {
                state.unresolvedMaterialPaths.push_front(std::move(path));
            };
            NLS::Render::Resources::Material* material = nullptr;
            const bool resourceTableAvailable =
                materialManager.TryGetResource(path, material);
            if (resourceTableAvailable && material == nullptr)
            {
                const auto cachedResolvedMaterialPath = state.resolvedMaterialPaths.find(path);
                material = ResolvePreviewMaterial(
                    materialManager,
                    path,
                    cachedResolvedMaterialPath != state.resolvedMaterialPaths.end()
                        ? cachedResolvedMaterialPath->second
                        : std::string {},
                    false);
            }
            if (material == nullptr)
            {
                // Resource-plan paths are usually already canonical artifact
                // paths. Probe that identity first; source-path resolution is
                // the fallback only when the direct index lookup misses.
                auto registered = materialManager.TryFindRegisteredMaterialByResolvedArtifactPath(path);
                if (!registered.has_value())
                {
                    deferMaterialPath();
                    break;
                }
                material = *registered;
                if (material == nullptr && resolvedMaterialPath != state.resolvedMaterialPaths.end())
                {
                    registered = materialManager.TryFindRegisteredMaterialByResolvedArtifactPath(
                        resolvedMaterialPath->second);
                    if (!registered.has_value())
                    {
                        deferMaterialPath();
                        break;
                    }
                    material = *registered;
                }
                if (material == nullptr && resolvedMaterialPath == state.resolvedMaterialPaths.end())
                {
                    resolvedMaterialPath = state.resolvedMaterialPaths.emplace(
                        path,
                        NLS::Core::ResourceManagement::MaterialManager::ResolveResourcePath(path)).first;
                    registered = materialManager.TryFindRegisteredMaterialByResolvedArtifactPath(
                        resolvedMaterialPath->second);
                    if (!registered.has_value())
                    {
                        deferMaterialPath();
                        break;
                    }
                    material = *registered;
                }
            }
            if (material != nullptr)
                state.materialResourceTableContentionCounts.erase(path);
            if (material == nullptr)
            {
                if (materialManager.IsAsyncArtifactLoadFailed(path))
                {
                    state.resolvedMaterials[path] = nullptr;
                    state.materialRequestPaths.erase(path);
                    state.materialUnavailableSince.erase(path);
                    state.unavailableMaterialPaths.insert(path);
                    continue;
                }

                bool requestStarted = state.materialRequestPaths.find(path) !=
                    state.materialRequestPaths.end();
                bool pendingAfterRequest = false;
                if (!requestStarted && !allowNewResourceRequests)
                {
                    const auto probe = materialManager.TryProbeAsyncArtifactLoad(path);
                    if (probe == NLS::Core::ResourceManagement::MaterialManager::AsyncArtifactLoadProbeResult::Pending)
                    {
                        requestStarted = true;
                        pendingAfterRequest = true;
                        state.materialRequestPaths.insert(path);
                    }
                    else
                    {
                        // There is no shared scene request to subscribe to
                        // yet. Leave the path pending until the scene resolver
                        // either registers it or releases the gate.
                        deferMaterialPath();
                        continue;
                    }
                }
                if (!requestStarted)
                {
                    bool waitForResourceTable = false;
                    if (!resourceTableAvailable)
                    {
                        ++materialResourceTableBusyCount;
                        auto& contentionCount =
                            state.materialResourceTableContentionCounts[path];
                        if (contentionCount < (std::numeric_limits<size_t>::max)())
                            ++contentionCount;
                        waitForResourceTable =
                            ShouldWaitForPrefabPreviewMaterialResourceTable(
                                contentionCount,
                                allowNewResourceRequests,
                                NLS::Editor::Core::
                                    HasBlockingSceneLoadRendererResourceResolution());
                        if (!waitForResourceTable)
                        {
                            deferMaterialPath();
                            break;
                        }
                        ++materialBlockingLookupCount;
                    }
                    else
                    {
                        state.materialResourceTableContentionCounts.erase(path);
                    }
                    ++materialRequestCallCount;
                    const auto requestResult =
                        materialManager.TryRequestAsyncArtifactForPreview(
                            path,
                            true,
                            waitForResourceTable);
                    if (!requestResult.has_value())
                    {
                        deferMaterialPath();
                        break;
                    }
                    material = requestResult->resource;
                    if (material == nullptr && requestResult->pending)
                    {
                        pendingAfterRequest = true;
                        state.materialRequestPaths.insert(path);
                    }
                    if (material == nullptr && requestResult->failed)
                    {
                        state.resolvedMaterials[path] = nullptr;
                        state.materialRequestPaths.erase(path);
                        state.materialUnavailableSince.erase(path);
                        state.unavailableMaterialPaths.insert(path);
                        continue;
                    }
                    if (material == nullptr && !pendingAfterRequest)
                    {
                        ++materialNotPendingAfterRequestCount;
                        const auto now = std::chrono::steady_clock::now();
                        const auto [since, inserted] = state.materialUnavailableSince.emplace(path, now);
                        (void)inserted;
                        if (now - since->second < kThumbnailMaterialFallbackGracePeriod)
                        {
                            state.unresolvedMaterialPaths.push_back(std::move(path));
                            break;
                        }
                        // No pending request after a bounded grace period
                        // means this path cannot make progress through the
                        // material manager. Keep the draw item alive with the
                        // renderer's default material instead of turning it
                        // into permanent thumbnail Pending.
                        state.resolvedMaterials[path] = nullptr;
                        state.materialRequestPaths.erase(path);
                        state.materialUnavailableSince.erase(path);
                        state.unavailableMaterialPaths.insert(path);
                        continue;
                    }
                }

                if (material == nullptr)
                {
                    if (pendingAfterRequest)
                        ++materialPendingAfterRequestCount;
                    const auto probe = materialManager.TryProbeAsyncArtifactLoad(path);
                    if (probe == NLS::Core::ResourceManagement::MaterialManager::AsyncArtifactLoadProbeResult::Busy)
                    {
                        deferMaterialPath();
                        break;
                    }
                    if (probe == NLS::Core::ResourceManagement::MaterialManager::AsyncArtifactLoadProbeResult::Failed ||
                        probe == NLS::Core::ResourceManagement::MaterialManager::AsyncArtifactLoadProbeResult::Missing)
                    {
                        const auto now = std::chrono::steady_clock::now();
                        const auto [since, inserted] = state.materialUnavailableSince.emplace(path, now);
                        (void)inserted;
                        if (probe != NLS::Core::ResourceManagement::MaterialManager::AsyncArtifactLoadProbeResult::Failed &&
                            now - since->second < kThumbnailMaterialFallbackGracePeriod)
                        {
                            state.unresolvedMaterialPaths.push_back(path);
                            break;
                        }
                        // A previously accepted request can also disappear
                        // without registering a material after cancellation or
                        // an import race. Treat that terminal state exactly as
                        // an unaccepted request and use the default material.
                        state.resolvedMaterials[path] = nullptr;
                        state.materialRequestPaths.erase(path);
                        state.materialUnavailableSince.erase(path);
                        state.unavailableMaterialPaths.insert(path);
                        continue;
                    }
                    state.unresolvedMaterialPaths.push_back(path);
                    state.materialPathsToPump.insert(path);
                    break;
                }
            }

            state.resolvedMaterials[path] = material;
            state.materialUnavailableSince.erase(path);
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
            {
                // Resolve the texture bindings before classifying the material
                // as texture-pending.  A material can be fully usable without
                // any texture resource paths; putting it in this queue makes a
                // budgeted pump report a false permanent Pending state.
                EnsureThumbnailPreviewTexturePaths(*material);
                if (!material->GetTextureResourcePaths().empty())
                    state.materialsAwaitingTextures.push_back(path);
            }
        }
        inspectedResourceCount += inspectedMaterialCount;
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialInspection,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - materialInspectionBegin),
            inspectedMaterialCount,
            request.sourceAssetPath + "|" + request.subAssetKey + "|prefab-resource-inspection"
        });
        if (!state.materialPathsToPump.empty())
        {
            const auto materialPumpBegin = std::chrono::steady_clock::now();
            materialManager.PumpAsyncLoadsForPaths(
                state.materialPathsToPump,
                kThumbnailPreviewMaterialPumpCompletionsPerFrame,
                MakeThumbnailPreviewResourcePumpStopPredicate(materialPhaseDeadline),
                kAllowReadyThumbnailCompletionAfterDeadline);
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpMaterialDependencies,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - materialPumpBegin),
                state.materialPathsToPump.size(),
                request.sourceAssetPath + "|" + request.subAssetKey + "|prefab-resource-pump"
            });
        }

        size_t pendingTexturePathCount = 0u;
        size_t failedTexturePathCount = 0u;
        texturePhaseDeadline = materialPhaseDeadline;
        const bool materialPhaseComplete = ShouldResetPrefabPreviewPhaseDeadline(
                state.unresolvedMaterialPaths.size(),
                state.materialRequestPaths.size(),
                state.materialPathsToPump.size());
        if (materialPhaseComplete)
        {
            texturePhaseDeadline = std::chrono::steady_clock::now() + resourcePumpBudget;
        }
        const auto textureBindingBegin = std::chrono::steady_clock::now();
        const size_t previouslyPendingTexturePathCount = state.pendingTexturePaths.size();
        if (textureManager != nullptr)
        {
            // Texture binding can perform equivalent-path and readiness
            // checks for every material.  Pump interests that were already
            // registered by the previous pass first, otherwise that work can
            // consume the shared deadline before ready texture futures are
            // retired and leave every material waiting for the same paths.
            const auto previouslyPendingTexturePaths = state.pendingTexturePaths;
            if (!previouslyPendingTexturePaths.empty())
            {
                const auto texturePumpBegin = std::chrono::steady_clock::now();
                textureManager->PumpAsyncLoadsForPaths(
                    previouslyPendingTexturePaths,
                    kThumbnailPreviewPrefabTexturePumpCompletionsPerFrame,
                    MakeThumbnailPreviewResourcePumpStopPredicate(texturePhaseDeadline),
                    kAllowReadyThumbnailCompletionAfterDeadline);
                NLS::Core::Assets::RecordArtifactLoadTelemetry({
                    NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpTextureDependencies,
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - texturePumpBegin),
                    previouslyPendingTexturePaths.size(),
                        request.sourceAssetPath + "|" + request.subAssetKey +
                        "|prefab-resource-pump-previous"
                });
            }
        }

        const auto textureMaterialInspectionCount = (std::min)(
            state.materialsAwaitingTextures.size(),
            kThumbnailPreviewPrefabResourceInspectionsPerTypePerFrame);
        std::unordered_set<std::string> activeTextureInterests = m_textureInterestPaths;
        activeTextureInterests.insert(
            state.texturePathsToPump.begin(),
            state.texturePathsToPump.end());
        if (ShouldRefreshPrefabPreviewTextureInspectionDeadlineAfterSetup(
                materialPhaseComplete,
                previouslyPendingTexturePathCount))
        {
            // Copying a large fallback/interest set can exceed the Debug
            // build's 1 ms phase window. Start the actual inspection budget
            // after that fixed setup when there was no request to retire.
            texturePhaseDeadline = std::chrono::steady_clock::now() + resourcePumpBudget;
        }
        size_t inspectedTextureMaterialCount = 0u;
        for (size_t index = 0u;
            index < textureMaterialInspectionCount &&
            ShouldContinuePrefabPreviewResourceInspection(
                index,
                inspectedResourceCount,
                std::chrono::steady_clock::now() >= texturePhaseDeadline);
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

            EnsureThumbnailPreviewTexturePaths(*material);
            for (const auto& [_, texturePath] : material->GetTextureResourcePaths())
            {
                if (!texturePath.empty())
                    state.texturePathsToPump.insert(ToGenericPath(texturePath));
            }
            state.requestedTexturePaths.clear();
            const bool texturesReady = BindReadyMaterialPreviewTextures(
                *material,
                activeTextureInterests,
                &state.requestedTexturePaths,
                &m_thumbnailTexturePaths,
                &m_thumbnailTextureResources,
                &state.pendingThumbnailTexturePaths,
                &state.unavailableTexturePaths,
                &readyTextureCache,
                &state.pendingTexturePaths,
                allowNewResourceRequests);
            TrackRequestedTextureInterests(state.requestedTexturePaths);
            state.texturePathsToPump.insert(
                state.requestedTexturePaths.begin(),
                state.requestedTexturePaths.end());
            if (!texturesReady)
                state.materialsAwaitingTextures.push_back(std::move(path));
        }
        inspectedResourceCount += inspectedTextureMaterialCount;

        if (textureManager != nullptr)
        {
            const auto pendingTexturePaths = state.pendingTexturePaths;
            pendingTexturePathCount = pendingTexturePaths.size();
            failedTexturePathCount = state.unavailableTexturePaths.size();
            if (!pendingTexturePaths.empty())
            {
                const auto texturePumpBegin = std::chrono::steady_clock::now();
                textureManager->PumpAsyncLoadsForPaths(
                    pendingTexturePaths,
                    kThumbnailPreviewPrefabTexturePumpCompletionsPerFrame,
                    MakeThumbnailPreviewResourcePumpStopPredicate(texturePhaseDeadline),
                    kAllowReadyThumbnailCompletionAfterDeadline);
                NLS::Core::Assets::RecordArtifactLoadTelemetry({
                    NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpTextureDependencies,
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - texturePumpBegin),
                    pendingTexturePaths.size(),
                    request.sourceAssetPath + "|" + request.subAssetKey + "|prefab-resource-pump"
                });
            }
        }
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPumpTextureBinding,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - textureBindingBegin),
            state.materialsAwaitingTextures.size(),
            request.sourceAssetPath + "|" + request.subAssetKey + "|prefab-resource-texture-binding"
        });

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
        if (IsResidentSnapshotPendingDiagnostic(prepared.diagnostic))
            return nullptr;

        // Keep terminal preparation diagnostics in the same freshness-scoped
        // cache as successful plans. Without this negative entry, a permanent
        // budget/identity failure starts the same expensive background parse
        // again on every GPU pump and can leave the UI Pending indefinitely.
        if (key.empty() ||
            (prepared.snapshot.drawItems.empty() && prepared.diagnostic.empty()))
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

            try
            {
                auto prepared = iterator->future.get();
                if (IsResidentSnapshotPendingDiagnostic(prepared.diagnostic))
                {
                    iterator = m_pendingPrefabPreviewPreparations.erase(iterator);
                    continue;
                }
                auto stored = StorePreparedPrefabPreviewInCache(
                    iterator->key,
                    std::move(prepared));
                (void)stored;
            }
            catch (...)
            {
                // The active request will retry a failed preparation through
                // its normal scheduling path. Do not retain a broken future.
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
        const auto resolveBegin = std::chrono::steady_clock::now();
        const auto cacheKey = BuildPreviewSnapshotCacheKey(request);
        if (auto cached = TryGetPreparedPrefabPreviewFromCache(cacheKey))
        {
            diagnostic = cached->diagnostic;
            if (!diagnostic.empty())
                return nullptr;
            return cached;
        }

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
                if (!pending->pendingTelemetryRecorded)
                {
                    pending->pendingTelemetryRecorded = true;
                    NLS::Core::Assets::RecordArtifactLoadTelemetry({
                        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects,
                        {},
                        0u,
                        request.sourceAssetPath + "|" + request.subAssetKey +
                            "|prepare-future-pending"
                    });
                }
                diagnostic = "thumbnail-gpu-preview-resources-pending:prefab-prepare=1";
                return nullptr;
            }

            auto future = std::move(pending->future);
            m_pendingPrefabPreviewPreparations.erase(pending);
            try
            {
                auto prepared = future.get();
                diagnostic = prepared.diagnostic;
                NLS::Core::Assets::RecordArtifactLoadTelemetry({
                    NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects,
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - resolveBegin),
                    prepared.snapshot.drawItems.size(),
                    request.sourceAssetPath + "|" + request.subAssetKey +
                        "|prepare-future-ready|planDrawItems=" +
                        std::to_string(prepared.resourcePlan.drawItems.size()) +
                        "|meshPaths=" +
                        std::to_string(prepared.resourcePlan.meshLoadPaths.size()) +
                        "|materialPaths=" +
                        std::to_string(prepared.resourcePlan.materialLoadPaths.size()) +
                        (prepared.diagnostic.empty()
                            ? std::string {}
                            : std::string("|diag=") + prepared.diagnostic)
                });
                if (IsResidentSnapshotPendingDiagnostic(prepared.diagnostic))
                {
                    diagnostic = prepared.diagnostic;
                    return nullptr;
                }
                auto stored = StorePreparedPrefabPreviewInCache(cacheKey, std::move(prepared));
                if (stored == nullptr)
                {
                    diagnostic = "thumbnail-gpu-preview-prefab-prepare-result-rejected";
                    NLS::Core::Assets::RecordArtifactLoadTelemetry({
                        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects,
                        {},
                        0u,
                        request.sourceAssetPath + "|" + request.subAssetKey +
                            "|prepare-result-rejected"
                    });
                }
                if (stored == nullptr)
                    return nullptr;
                NLS::Core::Assets::RecordArtifactLoadTelemetry({
                    NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects,
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - resolveBegin),
                    stored->snapshot.drawItems.size(),
                    request.sourceAssetPath + "|" + request.subAssetKey +
                        "|prepare-cache-store|cacheKeyHash=" +
                        std::to_string(std::hash<std::string> {}(cacheKey)) +
                        (stored->diagnostic.empty()
                            ? std::string {}
                            : std::string("|negative=1|diag=") + stored->diagnostic)
                });
                if (!diagnostic.empty())
                {
                    // Negative preparation results are freshness-scoped and
                    // must not be retried every frame until the asset changes.
                    return nullptr;
                }
                return stored;
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
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareSceneObjects,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - resolveBegin),
                0u,
                request.sourceAssetPath + "|" + request.subAssetKey +
                    "|prepare-cache-miss|cacheKeyHash=" +
                    std::to_string(std::hash<std::string> {}(cacheKey))
            });
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
                m_materialPreviewMaterial = GetStablePreviewMaterial(
                    *material,
                    request.colorSpaceMode,
                    request.hdrMode);
                EnsureMaterialPreviewObject(
                    *sphere,
                    *m_materialPreviewMaterial,
                    request.enablePreviewProxyPool);
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

    std::shared_ptr<NLS::Render::Resources::Material> CreateStablePreviewMaterial(
        NLS::Render::Resources::Material& source,
        std::string colorSpaceMode = "srgb",
        std::string hdrMode = "ldr")
    {
        return GetStablePreviewMaterial(
            source,
            std::move(colorSpaceMode),
            std::move(hdrMode));
    }

    void BindResidentPreviewMaterialTextures(
        NLS::Render::Resources::Material& material,
        const ResidentPrefabPreviewResources& resources)
    {
        for (const auto& [name, path] : material.GetTextureResourcePaths())
        {
            auto index = resources.textureIndicesByRequestedPath.find(path);
            if (index == resources.textureIndicesByRequestedPath.end())
                index = resources.textureIndicesByRequestedPath.find(ToGenericPath(path));
            if (index == resources.textureIndicesByRequestedPath.end() ||
                index->second >= resources.textures.size())
            {
                continue;
            }
            if (auto* texture = resources.textures[index->second].Get(); texture != nullptr)
                material.SetRawParameter(name, texture);
        }
    }

    std::shared_ptr<NLS::Render::Resources::Material> GetStablePreviewMaterial(
        NLS::Render::Resources::Material& source,
        std::string colorSpaceMode = "srgb",
        std::string hdrMode = "ldr")
    {
        StablePreviewMaterialKey key;
        key.source = &source;
        key.sourceInstanceId = source.GetInstanceId();
        key.parameterRevision = source.GetParameterRevision();
        key.renderStateRevision = source.GetRenderStateRevision();
        key.bindingRevision = source.GetBindingRevision();
        key.colorSpaceMode = std::move(colorSpaceMode);
        key.hdrMode = std::move(hdrMode);
        if (NLS::Core::ServiceLocator::Contains<
                NLS::Core::ResourceManagement::MaterialManager>())
        {
            key.materialManagerInstanceId = NLS_SERVICE(
                NLS::Core::ResourceManagement::MaterialManager).GetInstanceId();
        }

        const auto found = m_stablePreviewMaterialCache.find(key);
        if (found != m_stablePreviewMaterialCache.end())
        {
            found->second.lastUsed = ++m_stablePreviewMaterialCacheClock;
            return found->second.material;
        }

        auto material = CreateSharedStablePreviewMaterial(source);
        if (m_stablePreviewMaterialCache.size() >= kMaxStablePreviewMaterialCacheEntries)
        {
            auto eviction = m_stablePreviewMaterialCache.end();
            for (auto iterator = m_stablePreviewMaterialCache.begin();
                 iterator != m_stablePreviewMaterialCache.end();
                 ++iterator)
            {
                if (iterator->second.material.use_count() != 1u)
                    continue;
                if (eviction == m_stablePreviewMaterialCache.end() ||
                    iterator->second.lastUsed < eviction->second.lastUsed)
                {
                    eviction = iterator;
                }
            }
            if (eviction != m_stablePreviewMaterialCache.end())
                m_stablePreviewMaterialCache.erase(eviction);
        }

        m_stablePreviewMaterialCache.emplace(
            std::move(key),
            StablePreviewMaterialCacheEntry {
                material,
                ++m_stablePreviewMaterialCacheClock
            });
        return material;
    }

    void EnsureMaterialPreviewObject(
        NLS::Render::Resources::Mesh& sphere,
        NLS::Render::Resources::Material& material,
        const bool useProxyPool)
    {
        if (m_materialPreviewLease.has_value() &&
            m_materialPreviewUsesProxyPool != useProxyPool)
        {
            m_materialPreviewLease.reset();
            m_materialPreviewObject = nullptr;
            m_materialPreviewMeshFilter = nullptr;
            m_materialPreviewMeshRenderer = nullptr;
        }
        if (!m_materialPreviewLease.has_value() ||
            m_materialPreviewObject == nullptr ||
            m_materialPreviewMeshFilter == nullptr ||
            m_materialPreviewMeshRenderer == nullptr)
        {
            m_materialPreviewLease = m_previewProxyPool.Acquire(
                "Thumbnail Preview Material Sphere",
                useProxyPool);
            if (!m_materialPreviewLease.has_value())
            {
                m_materialPreviewObject = nullptr;
                m_materialPreviewMeshFilter = nullptr;
                m_materialPreviewMeshRenderer = nullptr;
                return;
            }
            auto* object = m_materialPreviewLease->Get();
            m_materialPreviewObject = object;
            m_materialPreviewMeshFilter = object != nullptr
                ? object->GetComponent<NLS::Engine::Components::MeshFilter>()
                : nullptr;
            m_materialPreviewMeshRenderer = object != nullptr
                ? object->GetComponent<NLS::Engine::Components::MeshRenderer>()
                : nullptr;
            if (m_materialPreviewObject == nullptr ||
                m_materialPreviewMeshFilter == nullptr ||
                m_materialPreviewMeshRenderer == nullptr)
            {
                m_materialPreviewLease.reset();
                m_materialPreviewObject = nullptr;
                m_materialPreviewMeshFilter = nullptr;
                m_materialPreviewMeshRenderer = nullptr;
                return;
            }
            m_materialPreviewMeshRenderer->SetFrustumBehaviour(
                NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::DISABLED);
            m_materialPreviewMeshRenderer->FillEmptySlotsWithMaterial(DefaultMaterial());
            object->SetActive(false);
            m_materialPreviewUsesProxyPool = useProxyPool;
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
        result.previewSnapshot = prepared->canonicalSnapshot;
        result.residentPreviewPartial = prepared->residentSnapshotUsed &&
            prepared->residentResources != nullptr &&
            !prepared->residentResources->IsCompleteForSource();
        const auto& resourcePlan = prepared->resourcePlan;
        result.expectedSceneDrawCount = ResolvePrefabPreviewExpectedSceneDrawCount(
            snapshot.expectedDrawItemCount,
            resourcePlan.drawItems.size(),
            result.residentPreviewPartial);
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
        if (!DefaultMaterialReady(result))
        {
            ClearPreviewObjects(false);
            return false;
        }

        const auto requestKey = BuildPrefabPreviewSceneAssemblyKey(request);
        if (m_prefabPreviewSceneAssembly.requestKey != requestKey ||
            m_prefabPreviewSceneAssembly.prepared.get() != prepared.get() ||
            m_prefabPreviewSceneAssembly.sceneAssemblyRevision !=
                prepared->sceneAssemblyRevision)
        {
            SuspendPrefabPreviewSceneAssembly();
            ClearPreviewObjects(false);
            m_prefabPreviewSceneAssembly.requestKey = requestKey;
            m_prefabPreviewSceneAssembly.prepared = prepared;
            m_prefabPreviewSceneAssembly.sceneAssemblyRevision =
                prepared->sceneAssemblyRevision;
            RestorePrefabPreviewSceneAssembly(requestKey, prepared);
            RestorePrefabPreviewDrawPrewarmState(requestKey, prepared);
        }
        auto& assembly = m_prefabPreviewSceneAssembly;

        if (prepared->residentResources != nullptr &&
            assembly.residentBindingRevision != prepared->residentSnapshotRevision)
        {
            for (auto& [_, material] : assembly.stableMaterials)
            {
                if (material != nullptr)
                    BindResidentPreviewMaterialTextures(*material, *prepared->residentResources);
            }
            const size_t existingObjectCount = std::min(
                m_previewObjects.size(),
                resourcePlan.drawItems.size());
            for (size_t index = 0u; index < existingObjectCount; ++index)
            {
                auto* object = m_previewObjects[index].Get();
                auto* renderer = object != nullptr
                    ? object->GetComponent<NLS::Engine::Components::MeshRenderer>()
                    : nullptr;
                if (renderer != nullptr)
                {
                    BindPrefabPreviewDrawItemMaterials(
                        request,
                        prepared,
                        assembly,
                        resourcePlan.drawItems[index],
                        *renderer);
                }
            }
            assembly.residentBindingRevision = prepared->residentSnapshotRevision;
        }

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
        // The service passes the same bounded slice used for dependency
        // continuation here. Interactive work remains at the one millisecond
        // floor, while idle adaptive work may use the four millisecond ceiling
        // to finish large canonical proxy lists in a few turns.
        const auto assemblyBudget = ThumbnailPreviewResourcePumpBudget(request);
        const auto assemblyDeadline = telemetryBegin + assemblyBudget;
        const bool completeResidentAssembly = prepared->residentSnapshotUsed &&
            prepared->residentResources != nullptr &&
            prepared->residentResources->IsCompleteForSource();
        const size_t minimumAssemblyBatch = completeResidentAssembly
            ? kThumbnailPreviewCompleteResidentSceneAssemblyMinimumBatch
            : kThumbnailPreviewPrefabSceneAssemblyMinimumBatch;
        while (assembly.nextDrawItemIndex < resourcePlan.drawItems.size() &&
            assembly.nextDrawItemIndex - batchBegin < kThumbnailPreviewPrefabSceneAssemblyMaximumBatch)
        {
            if (assembly.nextDrawItemIndex - batchBegin >= minimumAssemblyBatch &&
                std::chrono::steady_clock::now() >= assemblyDeadline)
            {
                break;
            }

            // Do not advance the cursor until this item has been completely
            // assembled. A resource continuation can return while a mesh is
            // still resolving; advancing first would permanently skip that
            // item on the next pump and publish an incomplete prefab preview.
            const auto& planned = resourcePlan.drawItems[assembly.nextDrawItemIndex];
            if (planned.drawItemIndex >= snapshot.drawItems.size())
            {
                ++assembly.nextDrawItemIndex;
                continue;
            }

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

            auto proxy = m_previewProxyPool.Acquire(
                "Thumbnail Preview Prefab Draw Item",
                request.enablePreviewProxyPool);
            if (!proxy.has_value())
            {
                result.diagnostic = "thumbnail-preview-proxy-pool-exhausted";
                return false;
            }
            auto* object = proxy->Get();
            m_previewObjects.push_back(std::move(*proxy));
            if (object == nullptr)
            {
                result.diagnostic = "thumbnail-preview-proxy-pool-invalid-lease";
                return false;
            }
            object->GetTransform()->SetLocalPosition(drawItem.localPosition);
            object->GetTransform()->SetLocalRotation(drawItem.localRotation);
            object->GetTransform()->SetLocalScale(drawItem.localScale);
            auto* filter = object->GetComponent<NLS::Engine::Components::MeshFilter>();
            auto* renderer = object->GetComponent<NLS::Engine::Components::MeshRenderer>();
            filter->SetMesh(mesh);
            renderer->SetFrustumBehaviour(NLS::Engine::Components::MeshRenderer::EFrustumBehaviour::DISABLED);
            BindPrefabPreviewDrawItemMaterials(
                request,
                prepared,
                assembly,
                planned,
                *renderer);

            IncludeWorldBounds(
                assembly.combinedBounds,
                mesh->GetBounds(),
                object->GetTransform()->GetWorldMatrix());
            ++assembly.nextDrawItemIndex;
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
                std::to_string(resourcePlan.drawItems.size())
        });

        const bool residentPartialPreview =
            prepared->residentSnapshotUsed &&
            prepared->residentResources != nullptr &&
            !prepared->residentResources->IsCompleteForSource();
        const bool canRenderResidentPartialPreview =
            residentPartialPreview &&
            assembly.nextDrawItemIndex != 0u &&
            assembly.combinedBounds.valid;
        if (assembly.nextDrawItemIndex < resourcePlan.drawItems.size() &&
            !canRenderResidentPartialPreview)
        {
            result.diagnostic = "thumbnail-gpu-preview-resources-pending:prefab-scene-assembly=" +
                std::to_string(assembly.nextDrawItemIndex) + "/" +
                std::to_string(resourcePlan.drawItems.size());
            result.resourceProgressToken =
                0x8000000000000000ull ^
                static_cast<uint64_t>(assembly.nextDrawItemIndex);
            return false;
        }

        if (!assembly.combinedBounds.valid)
        {
            result.diagnostic = "thumbnail-gpu-preview-prefab-mesh-load-failed";
            ClearPreviewObjects(false);
            return false;
        }

        assembly.sceneObjectsReady = true;
        ConfigurePrefabCamera(assembly.combinedBounds, result.width, result.height);
        RenderCurrentPreviewScene(request, result, residentPartialPreview);
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
        return !result.rgbaPixels.empty() || result.publishableGpuTexture;
    }

    void RenderCurrentPreviewScene(
        const AssetThumbnailRequest& request,
        EditorThumbnailPreviewResult& result,
        const bool publishProvisionalTextureOnly = false)
    {
        if (publishProvisionalTextureOnly)
            result.publishableGpuTexture = false;
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

        if (!request.enableReadbackRing &&
            !WaitForRetiredPreviewReadbacksBeforeStartingReadback())
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
            PreviewSceneDescriptor descriptor {
                m_scene,
                std::nullopt,
                nullptr,
                {},
                false
            };
            // Keep canonical prefab geometry while imported material textures
            // are still resolving. Requiring explicit textures here silently
            // removes the entire primitive from the preview scene.
            descriptor.requireExplicitMaterialTextures = false;
            descriptor.allowDefaultMaterialForUnresolvedExplicitMaterials = true;
            m_renderer->AddDescriptor<PreviewSceneDescriptor>(std::move(descriptor));
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
        // Resident packages already contain the scene resources needed for the
        // current frame. Do not hold either a partial resident image or a
        // complete resident image behind the full-scene threaded prewarm gate:
        // that gate is budgeted for canonical cold previews and can otherwise
        // advance only a few draws per frame while the scene continues restoring.
        const bool residentPartialPreview =
            request.kind == AssetThumbnailKind::PrefabPreview &&
            result.residentPreviewPartial;
        const bool completeResidentPreview =
            request.kind == AssetThumbnailKind::PrefabPreview &&
            m_prefabPreviewSceneAssembly.prepared != nullptr &&
            m_prefabPreviewSceneAssembly.prepared->residentSnapshotUsed &&
            m_prefabPreviewSceneAssembly.prepared->residentResources != nullptr &&
            ShouldSkipPrefabPreviewDrawPrewarmForResident(
                m_prefabPreviewSceneAssembly.prepared->residentSnapshotUsed,
                m_prefabPreviewSceneAssembly.prepared->residentResources->IsCompleteForSource());
        if (residentPartialPreview || completeResidentPreview)
            m_prefabPreviewSceneAssembly.drawPrewarmComplete = true;
        if (usesThreadedRendering &&
            request.kind == AssetThumbnailKind::PrefabPreview &&
            !residentPartialPreview &&
            !completeResidentPreview &&
            !m_prefabPreviewSceneAssembly.drawPrewarmComplete)
        {
            const auto prewarm = m_renderer->PrewarmBackgroundPreviewDraws(
                frameDescriptor,
                m_prefabPreviewSceneAssembly.nextDrawPrewarmIndex,
                kThumbnailPreviewPrefabSceneAssemblyMaximumBatch,
                ThumbnailPreviewResourcePumpBudget(request));
            if (prewarm.supported)
            {
                m_prefabPreviewSceneAssembly.nextDrawPrewarmIndex = prewarm.nextDrawIndex;
                m_prefabPreviewSceneAssembly.totalDrawPrewarmCount = prewarm.totalDrawCount;
                m_prefabPreviewSceneAssembly.drawPrewarmComplete = prewarm.complete;
                result.rawVisibleDrawCount = prewarm.totalDrawCount;
                // Persist every cursor advance, including incomplete batches.
                // The preview scene may be suspended immediately after this
                // result when another visible request gets the GPU lane.
                PersistPrefabPreviewDrawPrewarmState(m_prefabPreviewSceneAssembly);
                if (ShouldDeferPrefabPreviewAfterDrawPrewarm(prewarm.supported, prewarm.complete))
                {
                    result.rgbaPixels.clear();
                    result.diagnostic = "thumbnail-gpu-preview-resources-pending:prefab-draw-prewarm=" +
                        std::to_string(prewarm.nextDrawIndex) + "/" +
                        std::to_string(prewarm.totalDrawCount);
                    result.resourceProgressToken =
                        0x4000000000000000ull ^ prewarm.nextDrawIndex;
                    return;
                }
            }
            else
            {
                m_prefabPreviewSceneAssembly.drawPrewarmComplete = true;
            }

            // Draw prewarming is renderer preparation, not scene-object
            // lifetime. Persist its cursor independently so switching between
            // visible requests or clearing proxy objects cannot restart a
            // large canonical prefab from draw zero.
            PersistPrefabPreviewDrawPrewarmState(m_prefabPreviewSceneAssembly);

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
            if (!request.enableReadbackRing &&
                !WaitForRetiredPreviewReadbacksBeforeStartingReadback())
            {
                result.diagnostic = "thumbnail-gpu-preview-readback-pending";
                return;
            }

            threadedReadback.active = true;
            threadedReadback.requestKey = BuildPreviewReadbackRequestKey(request);
            threadedReadback.requestRevision = request.requestRevision;
            threadedReadback.priority = request.priority;
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
            result.objectDataOverflowDroppedObjectCount =
                drawStats.objectDataOverflowDroppedObjectCount;
            result.rawVisibleDrawCount = drawStats.rawVisibleObjectCount;
            result.submittedSceneDrawCount = drawStats.submittedSceneDrawCount;
#if defined(NLS_ENABLE_TEST_HOOKS)
            g_lastThumbnailPreviewRenderStatsForTesting.expectedSceneDrawCount =
                result.expectedSceneDrawCount;
            g_lastThumbnailPreviewRenderStatsForTesting.rawVisibleDrawCount =
                drawStats.rawVisibleObjectCount;
            g_lastThumbnailPreviewRenderStatsForTesting.submittedSceneDrawCount =
                drawStats.submittedSceneDrawCount;
            g_lastThumbnailPreviewRenderStatsForTesting.objectDataOverflowDroppedObjectCount =
                drawStats.objectDataOverflowDroppedObjectCount;
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
            const bool allowResidentPartialGpuTexture =
                request.kind == AssetThumbnailKind::PrefabPreview &&
                publishProvisionalTextureOnly &&
                result.residentPreviewPartial &&
                result.submittedSceneDrawCount != 0u &&
                result.gpuTexture.IsValid();
            // A complete GPU frame is the canonical display result. A resident
            // partial frame is also publishable for immediate in-memory display,
            // but the service keeps it provisional and never persists it as the
            // canonical PNG/JSON until the resident resource package is complete.
            result.publishableGpuTexture =
                (request.kind != AssetThumbnailKind::PrefabPreview ||
                    IsCompletePrefabPreviewSceneDraw(result) ||
                    allowResidentPartialGpuTexture) &&
                result.submittedSceneDrawCount != 0u &&
                result.gpuTexture.IsValid();
            if (request.kind == AssetThumbnailKind::PrefabPreview &&
                !IsCompletePrefabPreviewSceneDraw(result) &&
                !allowResidentPartialGpuTexture)
            {
                result.gpuTexture = {};
                result.rgbaPixels.clear();
                result.diagnostic =
                    "thumbnail-gpu-preview-incomplete-scene-draw|expected=" +
                    std::to_string(result.expectedSceneDrawCount) +
                    "|raw=" + std::to_string(result.rawVisibleDrawCount) +
                    "|submitted=" + std::to_string(result.submittedSceneDrawCount) +
                    "|overflow=" +
                    std::to_string(result.objectDataOverflowDroppedObjectCount) +
                    "|proxies=" + std::to_string(m_previewObjects.size()) +
                    "|assembly=" + std::to_string(m_prefabPreviewSceneAssembly.nextDrawItemIndex) +
                    "/" + std::to_string(m_prefabPreviewSceneAssembly.prepared != nullptr
                        ? m_prefabPreviewSceneAssembly.prepared->resourcePlan.drawItems.size()
                        : 0u) +
                    "|sceneMeshRenderers=" + std::to_string(
                        m_scene.GetFastAccessComponents().modelRenderers.size());
                return;
            }
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRender,
                {},
                result.publishableGpuTexture ? 1u : 0u,
                request.sourceAssetPath + "|" + request.subAssetKey +
                    "|direct-gpu|texture=" +
                    std::to_string(result.gpuTexture.texture != nullptr ? 1u : 0u) +
                    "|view=" +
                    std::to_string(result.gpuTexture.textureView != nullptr ? 1u : 0u) +
                    "|lease=" +
                    std::to_string(result.gpuTexture.renderTargetLease != nullptr ? 1u : 0u) +
                    "|draw=" + std::to_string(result.submittedSceneDrawCount)
            });
            if (publishProvisionalTextureOnly)
            {
                if (allowResidentPartialGpuTexture)
                {
                    // Resident packages are assembled incrementally. Expose
                    // the current GPU frame immediately, but keep it marked
                    // partial so the service stores it only in memory and
                    // replaces it when the registry publishes a new revision.
                    result.publishableGpuTexture = true;
                    result.rgbaPixels.clear();
                    result.diagnostic = "thumbnail-gpu-preview-resident-partial";
                    return;
                }
                result.publishableGpuTexture = false;
                result.rgbaPixels.clear();
                result.diagnostic = "thumbnail-gpu-preview-resident-partial";
                return;
            }
            threadedReadback.rawVisibleDrawCount = result.rawVisibleDrawCount;
            threadedReadback.submittedSceneDrawCount = result.submittedSceneDrawCount;
            threadedReadback.expectedSceneDrawCount = result.expectedSceneDrawCount;
            threadedReadback.objectDataOverflowDroppedObjectCount =
                result.objectDataOverflowDroppedObjectCount;
            threadedReadback.residentPreviewPartial = result.residentPreviewPartial;
            threadedReadback.renderInputsKeepAlive = CapturePreviewRenderInputsKeepAlive();
            threadedReadback.gpuTexture = result.gpuTexture;
            const auto storeResult = StorePendingReadback(
                std::move(threadedReadback),
                request.enableReadbackRing);
            if (storeResult == PreviewReadbackStoreResult::Deferred)
            {
                result.persistenceDeferred = true;
                result.diagnostic = "thumbnail-gpu-preview-persistence-deferred";
                return;
            }
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
        result.publishableGpuTexture =
            (request.kind != AssetThumbnailKind::PrefabPreview ||
                IsCompletePrefabPreviewSceneDraw(result)) &&
            result.submittedSceneDrawCount != 0u &&
            result.gpuTexture.IsValid();
        if (request.kind == AssetThumbnailKind::PrefabPreview &&
            !IsCompletePrefabPreviewSceneDraw(result))
        {
            result.gpuTexture = {};
            result.rgbaPixels.clear();
            result.diagnostic =
                "thumbnail-gpu-preview-incomplete-scene-draw|expected=" +
                std::to_string(result.expectedSceneDrawCount) +
                "|raw=" + std::to_string(result.rawVisibleDrawCount) +
                "|submitted=" + std::to_string(result.submittedSceneDrawCount) +
                "|overflow=" +
                std::to_string(result.objectDataOverflowDroppedObjectCount) +
                "|proxies=" + std::to_string(m_previewObjects.size()) +
                "|assembly=" + std::to_string(m_prefabPreviewSceneAssembly.nextDrawItemIndex) +
                "/" + std::to_string(m_prefabPreviewSceneAssembly.prepared != nullptr
                    ? m_prefabPreviewSceneAssembly.prepared->resourcePlan.drawItems.size()
                    : 0u) +
                "|sceneMeshRenderers=" + std::to_string(
                    m_scene.GetFastAccessComponents().modelRenderers.size());
            return;
        }
        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRender,
            {},
            result.publishableGpuTexture ? 1u : 0u,
            request.sourceAssetPath + "|" + request.subAssetKey +
                "|direct-gpu|texture=" +
                std::to_string(result.gpuTexture.texture != nullptr ? 1u : 0u) +
                "|view=" +
                std::to_string(result.gpuTexture.textureView != nullptr ? 1u : 0u) +
                "|lease=" +
                std::to_string(result.gpuTexture.renderTargetLease != nullptr ? 1u : 0u) +
                "|draw=" + std::to_string(result.submittedSceneDrawCount)
        });
        if (publishProvisionalTextureOnly)
        {
            result.publishableGpuTexture = false;
            result.rgbaPixels.clear();
            result.diagnostic = "thumbnail-gpu-preview-resident-partial";
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
        if (!request.enableReadbackRing &&
            !WaitForRetiredPreviewReadbacksBeforeStartingReadback())
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
        pendingReadback.requestRevision = request.requestRevision;
        pendingReadback.priority = request.priority;
        pendingReadback.width = result.width;
        pendingReadback.height = result.height;
        pendingReadback.rawVisibleDrawCount = result.rawVisibleDrawCount;
        pendingReadback.submittedSceneDrawCount = result.submittedSceneDrawCount;
        pendingReadback.expectedSceneDrawCount = result.expectedSceneDrawCount;
        pendingReadback.objectDataOverflowDroppedObjectCount =
            result.objectDataOverflowDroppedObjectCount;
        pendingReadback.previewSnapshot = result.previewSnapshot;
        pendingReadback.residentPreviewPartial = result.residentPreviewPartial;
        pendingReadback.rgbaPixels = std::move(readbackPixels);
        pendingReadback.completion = readback.completion;
        pendingReadback.renderInputsKeepAlive = std::move(renderInputsKeepAlive);
        pendingReadback.gpuTexture = result.gpuTexture;

        auto polled = PollEditorThumbnailPreviewReadback(
            pendingReadback,
            readbackRequestKey,
            &m_driver,
            pendingReadback.requestRevision);
        if (polled.status == EditorThumbnailPreviewReadbackPollStatus::Ready)
        {
            result = std::move(polled.preview);
            result.completedPendingReadback = true;
            return;
        }
        if (polled.status == EditorThumbnailPreviewReadbackPollStatus::Pending)
        {
            const auto storeResult = StorePendingReadback(
                std::move(pendingReadback),
                request.enableReadbackRing);
            if (storeResult == PreviewReadbackStoreResult::Deferred)
            {
                result.persistenceDeferred = true;
                result.diagnostic = "thumbnail-gpu-preview-persistence-deferred";
                return;
            }
            result.rgbaPixels.clear();
            result.diagnostic = "thumbnail-gpu-preview-readback-pending";
            return;
        }
        result.rgbaPixels.clear();
        result.diagnostic = "thumbnail-gpu-preview-readback-failed:" + polled.preview.diagnostic;
    }

    bool HasIncompletePrefabPreviewSceneAssembly(
        const PrefabPreviewSceneAssemblyState& assembly) const
    {
        if (assembly.requestKey.empty() || assembly.prepared == nullptr)
            return false;

        return !assembly.sceneObjectsReady ||
            assembly.nextDrawItemIndex < assembly.prepared->resourcePlan.drawItems.size() ||
            !assembly.drawPrewarmComplete;
    }

    void PersistPrefabPreviewDrawPrewarmState(
        const PrefabPreviewSceneAssemblyState& assembly)
    {
        if (assembly.requestKey.empty() || assembly.prepared == nullptr ||
            assembly.sceneAssemblyRevision == 0u)
        {
            return;
        }

        auto iterator = m_prefabPreviewDrawPrewarmStates.find(assembly.requestKey);
        if (iterator == m_prefabPreviewDrawPrewarmStates.end())
        {
            if (m_prefabPreviewDrawPrewarmStates.size() >=
                kMaxPrefabPreviewDrawPrewarmStates)
            {
                const auto oldest = std::min_element(
                    m_prefabPreviewDrawPrewarmStates.begin(),
                    m_prefabPreviewDrawPrewarmStates.end(),
                    [](const auto& left, const auto& right)
                    {
                        return left.second.lastUsed < right.second.lastUsed;
                    });
                if (oldest != m_prefabPreviewDrawPrewarmStates.end())
                    m_prefabPreviewDrawPrewarmStates.erase(oldest);
            }
            iterator = m_prefabPreviewDrawPrewarmStates.emplace(
                assembly.requestKey,
                PrefabPreviewDrawPrewarmState {}).first;
        }

        auto& state = iterator->second;
        const auto savedPrepared = state.prepared.lock();
        if (savedPrepared.get() != assembly.prepared.get() ||
            state.sceneAssemblyRevision != assembly.sceneAssemblyRevision)
        {
            state = {};
            state.prepared = assembly.prepared;
            state.sceneAssemblyRevision = assembly.sceneAssemblyRevision;
        }
        state.nextDrawPrewarmIndex = assembly.nextDrawPrewarmIndex;
        state.totalDrawPrewarmCount = assembly.totalDrawPrewarmCount;
        state.drawPrewarmComplete = assembly.drawPrewarmComplete;
        state.lastUsed = ++m_prefabPreviewAssemblyClock;
    }

    void RestorePrefabPreviewDrawPrewarmState(
        const std::string& requestKey,
        const std::shared_ptr<const PreparedPrefabPreview>& prepared)
    {
        auto& assembly = m_prefabPreviewSceneAssembly;
        const auto iterator = m_prefabPreviewDrawPrewarmStates.find(requestKey);
        if (iterator == m_prefabPreviewDrawPrewarmStates.end() || prepared == nullptr)
            return;

        const auto savedPrepared = iterator->second.prepared.lock();
        if (!ShouldRestorePrefabPreviewDrawPrewarmState(
                savedPrepared != nullptr,
                iterator->second.sceneAssemblyRevision,
                assembly.sceneAssemblyRevision,
                iterator->second.nextDrawPrewarmIndex,
                iterator->second.totalDrawPrewarmCount,
                iterator->second.drawPrewarmComplete) ||
            savedPrepared.get() != prepared.get())
        {
            m_prefabPreviewDrawPrewarmStates.erase(iterator);
            return;
        }

        assembly.nextDrawPrewarmIndex = iterator->second.nextDrawPrewarmIndex;
        assembly.totalDrawPrewarmCount = iterator->second.totalDrawPrewarmCount;
        assembly.drawPrewarmComplete = iterator->second.drawPrewarmComplete;
        iterator->second.lastUsed = ++m_prefabPreviewAssemblyClock;
    }

    void SuspendPrefabPreviewSceneAssembly()
    {
        auto& assembly = m_prefabPreviewSceneAssembly;
        PersistPrefabPreviewDrawPrewarmState(assembly);
        if (!HasIncompletePrefabPreviewSceneAssembly(assembly))
            return;

        assembly.lastUsed = ++m_prefabPreviewAssemblyClock;
        // Suspended state does not own the proxy leases. Clear the scene-object
        // cursor before moving the metadata so a later restore rebuilds every
        // draw item instead of resuming past already-released proxies.
        assembly.sceneObjectsReady = false;
        assembly.nextDrawItemIndex = 0u;
        assembly.combinedBounds = {};
        assembly.stableMaterials.clear();
        const auto requestKey = assembly.requestKey;
        if (m_suspendedPrefabPreviewAssemblies.size() >=
                kMaxSuspendedPrefabPreviewSceneAssemblies &&
            m_suspendedPrefabPreviewAssemblies.find(requestKey) ==
                m_suspendedPrefabPreviewAssemblies.end())
        {
            const auto oldest = std::min_element(
                m_suspendedPrefabPreviewAssemblies.begin(),
                m_suspendedPrefabPreviewAssemblies.end(),
                [](const auto& left, const auto& right)
                {
                    return left.second.lastUsed < right.second.lastUsed;
                });
            if (oldest != m_suspendedPrefabPreviewAssemblies.end())
                m_suspendedPrefabPreviewAssemblies.erase(oldest);
        }

        m_suspendedPrefabPreviewAssemblies[requestKey] = std::move(assembly);
        assembly = {};
    }

    void RestorePrefabPreviewSceneAssembly(
        const std::string& requestKey,
        const std::shared_ptr<const PreparedPrefabPreview>& prepared)
    {
        const auto suspended = m_suspendedPrefabPreviewAssemblies.find(requestKey);
        if (suspended == m_suspendedPrefabPreviewAssemblies.end())
            return;

        if (suspended->second.prepared.get() != prepared.get())
        {
            m_suspendedPrefabPreviewAssemblies.erase(suspended);
            return;
        }

        if (suspended->second.sceneAssemblyRevision != prepared->sceneAssemblyRevision)
        {
            // The prepared object is still valid, but its scene inputs changed.
            // Its old proxies, materials and cursor must not be restored.
            m_suspendedPrefabPreviewAssemblies.erase(suspended);
            return;
        }

        m_prefabPreviewSceneAssembly = std::move(suspended->second);
        m_suspendedPrefabPreviewAssemblies.erase(suspended);
        m_prefabPreviewSceneAssembly.lastUsed = ++m_prefabPreviewAssemblyClock;
    }

    void ClearPreviewObjects(const bool drainThreadedRendering)
    {
        // Resource-pending and readback switching paths may clear the transient
        // proxy objects before the caller has a chance to explicitly suspend a
        // partial assembly. Preserve only the resumable cursor and bounds; the
        // proxies themselves are still released below.
        SuspendPrefabPreviewSceneAssembly();
        const auto cleanupTelemetryBegin = std::chrono::steady_clock::now();
        const size_t previewObjectCount = m_previewObjects.size();
        if (drainThreadedRendering)
            NLS::Render::Context::DriverRendererAccess::DrainThreadedRendering(m_driver);
        m_previewObjects.clear();
        DeactivateMaterialPreviewObject();
        ClearMaterialPreviewRendererBinding();
        m_materialPreviewLease.reset();
        m_materialPreviewObject = nullptr;
        m_materialPreviewMeshFilter = nullptr;
        m_materialPreviewMeshRenderer = nullptr;
        m_materialPreviewMaterial.reset();
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
            !m_materialPreviewLease.has_value() &&
            m_materialPreviewMaterial == nullptr &&
            (m_prefabPreviewSceneAssembly.prepared == nullptr ||
                m_prefabPreviewSceneAssembly.prepared->residentResources == nullptr))
            return nullptr;

        auto keepAlive = std::make_shared<PreviewRenderInputsKeepAlive>();
        keepAlive->proxies = std::move(m_previewObjects);
        m_previewObjects.clear();
        if (m_materialPreviewLease.has_value())
        {
            keepAlive->objects.push_back(m_materialPreviewObject);
            keepAlive->proxies.push_back(std::move(*m_materialPreviewLease));
            m_materialPreviewLease.reset();
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
        if (m_prefabPreviewSceneAssembly.prepared != nullptr)
        {
            keepAlive->residentResources =
                m_prefabPreviewSceneAssembly.prepared->residentResources;
        }
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

        m_orphanedReadbackRequestKeys.erase(BuildReadbackTicketIdentity(
            m_pendingReadback.requestKey,
            m_pendingReadback.requestRevision));
        if (!RetirePreviewReadback(std::move(m_pendingReadback)))
            return false;
        m_pendingReadback = {};
        return true;
    }

    PreviewReadbackStoreResult StorePendingReadback(
        EditorThumbnailPreviewReadbackState&& readback,
        const bool useRing)
    {
        const auto clearOrphanedReadbackTombstone = [this](
            const EditorThumbnailPreviewReadbackState& state)
        {
            return m_orphanedReadbackRequestKeys.erase(BuildReadbackTicketIdentity(
                state.requestKey,
                state.requestRevision)) != 0u;
        };
        if (!useRing)
        {
            m_lastSubmittedReadbackTicket = EditorThumbnailPreviewReadbackTicket {
                readback.requestKey,
                readback.requestRevision};
            m_pendingReadback = std::move(readback);
            return PreviewReadbackStoreResult::Stored;
        }
        if (m_pendingReadbackRing.size() >= kThumbnailPreviewReadbackRingCapacity)
        {
            auto victim = m_deferredReadbackPersistence.end();
            uint32_t victimRank = std::numeric_limits<uint32_t>::max();
            for (auto iterator = m_deferredReadbackPersistence.begin();
                 iterator != m_deferredReadbackPersistence.end();
                 ++iterator)
            {
                const auto rank = ThumbnailReadbackPriorityRank(iterator->priority);
                if (victim == m_deferredReadbackPersistence.end() || rank < victimRank)
                {
                    victim = iterator;
                    victimRank = rank;
                }
            }

            const auto incomingRank = ThumbnailReadbackPriorityRank(readback.priority);
            if (victim != m_deferredReadbackPersistence.end() &&
                incomingRank >= victimRank)
            {
                // The active three slots are never evicted. Once those are
                // occupied, replace only a lower/equal priority persistence
                // ticket and keep its GPU/readback inputs alive until the RHI
                // completion is safe to retire.
                auto evicted = std::move(*victim);
                m_deferredReadbackPersistence.erase(victim);
                const bool evictedWasOrphaned = clearOrphanedReadbackTombstone(evicted);
                if (RetirePreviewReadback(std::move(evicted)))
                {
                    m_lastSubmittedReadbackTicket = EditorThumbnailPreviewReadbackTicket {
                        readback.requestKey,
                        readback.requestRevision};
                    m_deferredReadbackPersistence.push_back(std::move(readback));
                    return PreviewReadbackStoreResult::Stored;
                }

                // The retired list is itself under pressure. Preserve the
                // evicted ticket rather than dropping an in-flight RHI readback;
                // the queue can temporarily exceed its normal persistence cap
                // only in this exceptional backpressure case. The incoming
                // ticket must be retained as well: it already owns the
                // render-target/proxy leases that protect the submitted GPU
                // work, so treating it as a dropped persistence request would
                // release those resources before the fence is safe.
                if (evictedWasOrphaned)
                {
                    m_orphanedReadbackRequestKeys.insert(BuildReadbackTicketIdentity(
                        evicted.requestKey,
                        evicted.requestRevision));
                }
                m_deferredReadbackPersistence.push_back(std::move(evicted));

                clearOrphanedReadbackTombstone(readback);
                m_lastSubmittedReadbackTicket = EditorThumbnailPreviewReadbackTicket {
                    readback.requestKey,
                    readback.requestRevision};
                m_deferredReadbackPersistence.push_back(std::move(readback));
                return PreviewReadbackStoreResult::Stored;
            }

            if (victim != m_deferredReadbackPersistence.end())
            {
                // The incoming ticket is lower priority than every retained
                // persistence ticket. It may be discarded only when its RHI
                // completion is already safe; otherwise its leases still have
                // to be retained even though the result will not be persisted
                // ahead of the existing higher-priority work.
                const bool completionSafe =
                    !readback.active ||
                    readback.completion == nullptr ||
                    readback.completion->Poll().IsComplete();
                if (completionSafe)
                {
                    clearOrphanedReadbackTombstone(readback);
                    (void)RetirePreviewReadback(std::move(readback));
                    return PreviewReadbackStoreResult::Deferred;
                }
            }

            // No deferred ticket was available to evict. Keep the incoming
            // readback in the deferred lane instead of destroying its RHI
            // lifetime. The lane is normally capped at eight entries; this
            // exceptional branch can temporarily exceed that cap by one while
            // a lower-priority ticket is still fenced. The submit guard then
            // stops new work until one of the retained tickets completes.
            clearOrphanedReadbackTombstone(readback);
            m_lastSubmittedReadbackTicket = EditorThumbnailPreviewReadbackTicket {
                readback.requestKey,
                readback.requestRevision};
            m_deferredReadbackPersistence.push_back(std::move(readback));
            return PreviewReadbackStoreResult::Stored;
        }
        m_lastSubmittedReadbackTicket = EditorThumbnailPreviewReadbackTicket {
            readback.requestKey,
            readback.requestRevision};
        m_pendingReadbackRing.push_back(std::move(readback));
        return PreviewReadbackStoreResult::Stored;
    }

    void PollReadbackRing()
    {
        const auto pollQueue = [this](std::deque<EditorThumbnailPreviewReadbackState>& queue)
        {
            for (auto iterator = queue.begin(); iterator != queue.end();)
            {
                const auto requestKey = iterator->requestKey;
                const auto polled = PollEditorThumbnailPreviewReadback(
                    *iterator,
                    requestKey,
                    &m_driver,
                    iterator->requestRevision);
                RecordLegacyPollState(*iterator, requestKey, polled.status);
                if (polled.status == EditorThumbnailPreviewReadbackPollStatus::Pending)
                {
                    ++iterator;
                    continue;
                }

                auto preview = polled.preview;
                if (polled.status == EditorThumbnailPreviewReadbackPollStatus::Ready)
                    preview.completedPendingReadback = true;
                else if (polled.status == EditorThumbnailPreviewReadbackPollStatus::Failed ||
                    polled.status == EditorThumbnailPreviewReadbackPollStatus::DeviceLost)
                {
                    preview.rgbaPixels.clear();
                    if (preview.diagnostic.empty())
                        preview.diagnostic = polled.status == EditorThumbnailPreviewReadbackPollStatus::DeviceLost
                            ? "thumbnail-gpu-preview-readback-device-lost"
                            : "thumbnail-gpu-preview-readback-failed";
                }
                if (m_orphanedReadbackRequestKeys.erase(BuildReadbackTicketIdentity(
                        requestKey,
                        iterator->requestRevision)) == 0u)
                {
                    m_completedReadbackPreviews[requestKey] = {
                        {requestKey, iterator->requestRevision},
                        std::move(preview)
                    };
                }
                iterator = queue.erase(iterator);
            }
        };
        pollQueue(m_pendingReadbackRing);
        pollQueue(m_deferredReadbackPersistence);
    }

    void RetireReadbackRing()
    {
        const auto retireQueue = [this](std::deque<EditorThumbnailPreviewReadbackState>& queue)
        {
            for (auto& readback : queue)
            {
                m_orphanedReadbackRequestKeys.erase(BuildReadbackTicketIdentity(
                    readback.requestKey,
                    readback.requestRevision));
                (void)RetirePreviewReadback(std::move(readback));
            }
            queue.clear();
        };
        retireQueue(m_pendingReadbackRing);
        retireQueue(m_deferredReadbackPersistence);
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
                EnsureThumbnailPreviewTexturePaths(*material);
                if (!BindReadyMaterialPreviewTextures(
                        *material,
                        m_textureInterestPaths,
                        &requestedTexturePaths,
                        &m_thumbnailTexturePaths,
                        &m_thumbnailTextureResources))
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
    ThumbnailPreviewProxyPool m_previewProxyPool;
    NLS::Engine::Components::CameraComponent* m_camera = nullptr;
    std::vector<PreviewFramebufferEntry> m_previewFramebufferPool;
    NLS::Render::Resources::Material m_defaultMaterial;
    std::shared_ptr<NLS::Render::Resources::Material> m_materialPreviewMaterial;
    std::optional<ThumbnailPreviewProxyPool::Lease> m_materialPreviewLease;
    NLS::Engine::GameObject* m_materialPreviewObject = nullptr;
    NLS::Engine::Components::MeshFilter* m_materialPreviewMeshFilter = nullptr;
    NLS::Engine::Components::MeshRenderer* m_materialPreviewMeshRenderer = nullptr;
    std::vector<ThumbnailPreviewProxyPool::Lease> m_previewObjects;
    std::unordered_map<
        StablePreviewMaterialKey,
        StablePreviewMaterialCacheEntry,
        StablePreviewMaterialKeyHash> m_stablePreviewMaterialCache;
    uint64_t m_stablePreviewMaterialCacheClock = 0u;
    EditorThumbnailPreviewReadbackState m_pendingReadback;
    std::deque<EditorThumbnailPreviewReadbackState> m_pendingReadbackRing;
    // The three active ring slots are reserved for normal readback progress.
    // When they are full, retain a bounded post-submit readback here so the
    // already-rendered GPU texture can still be presented immediately.
    std::deque<EditorThumbnailPreviewReadbackState> m_deferredReadbackPersistence;
    std::unordered_map<std::string, EditorThumbnailPreviewCompletedReadback> m_completedReadbackPreviews;
    std::string m_activeRequestKey;
    std::unordered_set<std::string> m_textureInterestPaths;
    std::string m_textureInterestRequestKey;
    std::string m_thumbnailTextureRequestKey;
    std::unordered_set<std::string> m_thumbnailTexturePaths;
    std::unordered_set<std::string> m_thumbnailTextureHeaderProbePaths;
    std::unordered_set<std::string> m_thumbnailTextureDeferredPaths;
    std::unordered_set<std::string> m_thumbnailTextureFallbackPaths;
    std::unordered_map<
        std::string,
        std::future<std::optional<NLS::Render::Assets::TextureArtifactHeaderPreview>>>
        m_thumbnailTextureHeaderProbeFutures;
    std::unordered_map<
        std::string,
        std::future<std::optional<NLS::Render::Assets::TextureArtifactData>>> m_thumbnailTextureFutures;
    std::unordered_map<std::string, ThumbnailPreviewTextureUpload>
        m_thumbnailTextureUploadRequests;
    std::unordered_map<
        std::string,
        std::unique_ptr<NLS::Render::Resources::Texture2D>> m_thumbnailTextureResources;
    std::vector<PreviewSnapshotCacheEntry> m_previewSnapshotCache;
    std::vector<PendingPrefabPreviewPreparation> m_pendingPrefabPreviewPreparations;
    PrefabPreviewSceneAssemblyState m_prefabPreviewSceneAssembly;
    std::unordered_map<std::string, PrefabPreviewSceneAssemblyState>
        m_suspendedPrefabPreviewAssemblies;
    std::unordered_map<std::string, PrefabPreviewDrawPrewarmState>
        m_prefabPreviewDrawPrewarmStates;
    uint64_t m_prefabPreviewAssemblyClock = 0u;
    uint64_t m_previewSnapshotCacheClock = 0u;
    uint64_t m_previewFramebufferUseClock = 0u;
    uint64_t m_previewSceneUseCount = 0u;
    uint64_t m_renderTargetAllocationCount = 0u;
    uint64_t m_renderTargetReuseCount = 0u;
    std::optional<EditorThumbnailPreviewReadbackTicket> m_lastSubmittedReadbackTicket;
    std::unordered_set<std::string> m_orphanedReadbackRequestKeys;
    bool m_thumbnailRenderDocCaptureQueued = false;
    bool m_materialPreviewUsesProxyPool = false;
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

EditorThumbnailPreviewSubmitResult EditorThumbnailPreviewRenderer::SubmitPreview(
    const AssetThumbnailRequest& request)
{
    if (m_impl == nullptr)
        return {};
    return m_impl->SubmitPreview(request);
}

EditorThumbnailPreviewSubmitResult EditorThumbnailPreviewRenderer::SubmitPreparedPreview(
    const AssetThumbnailRequest& request)
{
    if (m_impl == nullptr)
        return {};
    return m_impl->SubmitPreparedPreview(request);
}

std::vector<EditorThumbnailPreviewCompletedReadback>
EditorThumbnailPreviewRenderer::PollCompletedReadbacks(const size_t maxCount)
{
    return m_impl != nullptr ? m_impl->PollCompletedReadbacks(maxCount) :
        std::vector<EditorThumbnailPreviewCompletedReadback> {};
}

void EditorThumbnailPreviewRenderer::ReleaseCompletedPreviewResources(
    const AssetThumbnailRequest& request)
{
    if (m_impl != nullptr)
        m_impl->ReleaseCompletedPreviewResources(request);
}

bool EditorThumbnailPreviewRenderer::SupportsAsynchronousReadbackPolling() const
{
    return true;
}

bool EditorThumbnailPreviewRenderer::OrphanReadback(
    const EditorThumbnailPreviewReadbackTicket& ticket)
{
    return m_impl != nullptr && m_impl->OrphanReadback(ticket);
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

EditorThumbnailPreviewSubmitResult EditorThumbnailPreviewRendererAdapter::SubmitPreview(
    const AssetThumbnailRequest& request)
{
    return m_renderer.SubmitPreview(request);
}

EditorThumbnailPreviewSubmitResult EditorThumbnailPreviewRendererAdapter::SubmitPreparedPreview(
    const AssetThumbnailRequest& request)
{
    return m_renderer.SubmitPreparedPreview(request);
}

std::vector<EditorThumbnailPreviewCompletedReadback>
EditorThumbnailPreviewRendererAdapter::PollCompletedReadbacks(const size_t maxCount)
{
    return m_renderer.PollCompletedReadbacks(maxCount);
}

void EditorThumbnailPreviewRendererAdapter::ReleaseCompletedPreviewResources(
    const AssetThumbnailRequest& request)
{
    m_renderer.ReleaseCompletedPreviewResources(request);
}

bool EditorThumbnailPreviewRendererAdapter::SupportsAsynchronousReadbackPolling() const
{
    return m_renderer.SupportsAsynchronousReadbackPolling();
}

bool EditorThumbnailPreviewRendererAdapter::OrphanReadback(
    const EditorThumbnailPreviewReadbackTicket& ticket)
{
    return m_renderer.OrphanReadback(ticket);
}

EditorThumbnailPreviewResourcePumpResult IEditorThumbnailPreviewRenderer::PumpResources(
    const AssetThumbnailRequest&)
{
    return {};
}
}

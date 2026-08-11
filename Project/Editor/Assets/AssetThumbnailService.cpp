#include "Assets/AssetThumbnailService.h"

#include "Assets/AssetBrowserPresentation.h"
#include "Assets/AssetThumbnailPreviewCamera.h"
#include "Assets/AssetMeta.h"
#include "Assets/ArtifactDatabaseManifestUtils.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/EditorThumbnailPreviewRenderer.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetPath.h"
#include "Assets/NativeArtifactContainer.h"
#include "Assets/PreviewRenderableSnapshot.h"
#include "Assets/ResidentPrefabPreviewRegistry.h"
#include "Core/EditorActions.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Guid.h"
#include "Image.h"
#include "Jobs/BackgroundJobQueue.h"
#include "Jobs/JobSystem.h"
#include "Profiling/PerformanceStageStats.h"
#include "Profiling/Profiler.h"
#include "Serialize/ObjectGraphReader.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/RHI/RHITypes.h"

#define STBIWDEF static
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define stbiw__linear_to_rgbe nls_asset_thumbnail_service_stbiw__linear_to_rgbe
#define stbiw__write_run_data nls_asset_thumbnail_service_stbiw__write_run_data
#define stbiw__write_dump_data nls_asset_thumbnail_service_stbiw__write_dump_data
#define stbiw__write_hdr_scanline nls_asset_thumbnail_service_stbiw__write_hdr_scanline
#define stbi_zlib_compress nls_asset_thumbnail_service_stbi_zlib_compress
#define stbi_write_png_to_mem nls_asset_thumbnail_service_stbi_write_png_to_mem
#include <stb/stb_image_write.h>
#undef stbi_write_png_to_mem
#undef stbi_zlib_compress
#undef stbiw__write_hdr_scanline
#undef stbiw__write_dump_data
#undef stbiw__write_run_data
#undef stbiw__linear_to_rgbe

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace NLS::Editor::Assets
{
namespace
{
using NLS::Base::Profiling::PerformanceStageThread;
using AssetThumbnailCancelToken = std::weak_ptr<AssetThumbnailGenerationCancelToken>;
using AssetThumbnailGenerator = AssetThumbnailServiceResult (*)(
    const AssetThumbnailRequest&,
    const AssetThumbnailCacheEvaluation&,
    const AssetThumbnailCancelToken&);

uint64_t MakeAssetThumbnailRequestSessionId()
{
    const auto id = static_cast<uint64_t>(
        std::hash<std::string> {}(NLS::Guid::New().ToString()));
    return id != 0u ? id : 1u;
}

#if defined(NLS_ENABLE_TEST_HOOKS)
std::atomic<size_t> g_freshnessInputCheckCountForTesting {0u};
std::atomic<size_t> g_thumbnailManifestLookupCountForTesting {0u};
std::atomic<size_t> g_thumbnailManifestMainThreadLookupCountForTesting {0u};
std::atomic<size_t> g_thumbnailManifestBackgroundThreadLookupCountForTesting {0u};
#endif

thread_local PerformanceStageThread g_thumbnailGenerationStageThread = PerformanceStageThread::Main;
constexpr uint8_t kGpuPreviewVisibleAlphaThreshold = 8u;
constexpr uint8_t kGpuPreviewLitLumaThreshold = 8u;

std::string BuildThumbnailRequestTelemetryPath(const AssetBrowserItem& item)
{
    const auto& sourcePath = item.sourceAssetPath.empty()
        ? item.projectRelativePath
        : item.sourceAssetPath;
    if (item.subAssetKey.empty())
        return sourcePath;
    return sourcePath + "|" + item.subAssetKey;
}

std::string BuildThumbnailRequestTelemetryPath(const AssetThumbnailRequest& request)
{
    if (request.subAssetKey.empty())
        return request.sourceAssetPath;
    return request.sourceAssetPath + "|" + request.subAssetKey;
}

std::string BuildThumbnailGpuPreviewRenderTelemetryPath(
    const AssetThumbnailRequest& request,
    const EditorThumbnailPreviewResult& preview,
    const std::string_view diagnosticOverride = {})
{
    auto path = BuildThumbnailRequestTelemetryPath(request);
    const auto diagnostic = diagnosticOverride.empty()
        ? std::string_view(preview.diagnostic)
        : diagnosticOverride;
    if (!diagnostic.empty())
    {
        path += "|diag=";
        path += diagnostic;
    }

    const size_t pixelCount = static_cast<size_t>(preview.width) * preview.height;
    if (pixelCount > 0u && preview.rgbaPixels.size() >= pixelCount * 4u)
    {
        size_t visibleAlphaPixelCount = 0u;
        size_t litRgbPixelCount = 0u;
        uint8_t maxAlpha = 0u;
        uint8_t maxLuma = 0u;
        for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
        {
            const auto offset = pixel * 4u;
            const uint8_t r = preview.rgbaPixels[offset + 0u];
            const uint8_t g = preview.rgbaPixels[offset + 1u];
            const uint8_t b = preview.rgbaPixels[offset + 2u];
            const uint8_t a = preview.rgbaPixels[offset + 3u];
            const auto luma = static_cast<uint8_t>(
                (static_cast<uint16_t>(r) * 77u +
                    static_cast<uint16_t>(g) * 150u +
                    static_cast<uint16_t>(b) * 29u) >> 8u);
            maxAlpha = (std::max)(maxAlpha, a);
            maxLuma = (std::max)(maxLuma, luma);
            if (a > kGpuPreviewVisibleAlphaThreshold)
                ++visibleAlphaPixelCount;
            if (luma > kGpuPreviewLitLumaThreshold)
                ++litRgbPixelCount;
        }

        path += "|pixels=" + std::to_string(pixelCount);
        path += "|visibleAlpha=" + std::to_string(visibleAlphaPixelCount);
        path += "|litRgb=" + std::to_string(litRgbPixelCount);
        path += "|maxAlpha=" + std::to_string(maxAlpha);
        path += "|maxLuma=" + std::to_string(maxLuma);
    }
    path += "|rawVisibleDraws=" + std::to_string(preview.rawVisibleDrawCount);
    path += "|submittedDraws=" + std::to_string(preview.submittedSceneDrawCount);
    path += "|expectedDraws=" + std::to_string(preview.expectedSceneDrawCount);
    path += "|objectDataOverflow=" +
        std::to_string(preview.objectDataOverflowDroppedObjectCount);
    return path;
}

void RecordThumbnailGpuPreviewQueueDecisionTelemetry(
    const std::string_view decision,
    const AssetThumbnailRequest* request = nullptr,
    const size_t count = 0u)
{
    if (!NLS::Core::Assets::IsArtifactLoadTelemetryEnabled())
        return;

    std::string path(decision);
    if (request != nullptr)
    {
        path += "|";
        path += BuildThumbnailRequestTelemetryPath(*request);
        path += "|priority=" + std::to_string(
            static_cast<uint32_t>(request->priority));
        path += "|residentSource=" + std::string(
            request->residentPrefabPreviewSource.has_value() ? "1" : "0");
        path += "|residentIdentity=" + std::string(
            request->residentPrefabPreviewSource.has_value() &&
                request->residentPrefabPreviewSource->HasIdentity()
                ? "1"
                : "0");
    }
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailServiceGpuPreviewQueueDecision,
        std::chrono::microseconds(0),
        count,
        std::move(path)
    });
}

void RecordThumbnailJobQueueTelemetry(
    const std::string_view event,
    const AssetThumbnailRequest& request,
    const std::string_view queue,
    const std::chrono::microseconds elapsed)
{
    if (!NLS::Core::Assets::IsArtifactLoadTelemetryEnabled())
        return;

    std::string path("thumbnail-job-");
    path += event;
    path += "|queue=";
    path += queue;
    path += "|";
    path += BuildThumbnailRequestTelemetryPath(request);
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailServiceGpuPreviewQueueDecision,
        elapsed,
        0u,
        std::move(path)
    });
}

const char* ThumbnailServiceStatusName(const AssetThumbnailServiceStatus status)
{
    switch (status)
    {
    case AssetThumbnailServiceStatus::Fresh:
        return "fresh";
    case AssetThumbnailServiceStatus::Pending:
        return "pending";
    case AssetThumbnailServiceStatus::Fallback:
        return "fallback";
    case AssetThumbnailServiceStatus::Failed:
        return "failed";
    }
    return "unknown";
}

void RecordThumbnailJobResultTelemetry(
    const AssetThumbnailRequest& request,
    const std::string_view queue,
    const std::chrono::microseconds elapsed,
    const AssetThumbnailServiceResult& result)
{
    if (!NLS::Core::Assets::IsArtifactLoadTelemetryEnabled())
        return;

    std::string path("thumbnail-job-completed");
    path += "|queue=";
    path += queue;
    path += "|status=";
    path += ThumbnailServiceStatusName(result.status);
    path += "|diagnostic=";
    path += result.diagnostic;
    path += "|";
    path += BuildThumbnailRequestTelemetryPath(request);
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailServiceGpuPreviewQueueDecision,
        elapsed,
        0u,
        std::move(path)
    });
}

void RecordThumbnailRequestBuildTelemetry(
    const NLS::Core::Assets::ArtifactLoadTelemetryStage stage,
    const std::chrono::steady_clock::time_point begin,
    const std::string& path)
{
    if (!NLS::Core::Assets::IsArtifactLoadTelemetryEnabled())
        return;

    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        stage,
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin),
        0u,
        path
    });
}

class ScopedThumbnailRequestBuildTelemetry
{
public:
    ScopedThumbnailRequestBuildTelemetry(
        const NLS::Core::Assets::ArtifactLoadTelemetryStage stage,
        const AssetBrowserItem& item,
        const size_t byteCount = 0u) :
        m_stage(stage),
        m_byteCount(byteCount),
        m_enabled(NLS::Core::Assets::IsArtifactLoadTelemetryEnabled())
    {
        if (!m_enabled)
            return;

        m_path = BuildThumbnailRequestTelemetryPath(item);
        m_begin = std::chrono::steady_clock::now();
    }

    ~ScopedThumbnailRequestBuildTelemetry()
    {
        if (!m_enabled)
            return;

        NLS::Core::Assets::RecordArtifactLoadTelemetry({
            m_stage,
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - m_begin),
            m_byteCount,
            m_path
        });
    }

private:
    NLS::Core::Assets::ArtifactLoadTelemetryStage m_stage;
    size_t m_byteCount = 0u;
    bool m_enabled = false;
    std::chrono::steady_clock::time_point m_begin {};
    std::string m_path;
};

enum class ThumbnailJobQueue
{
    Background,
    Foreground
};

template <typename Function>
auto ScheduleThumbnailJobFuture(
    const char* debugName,
    Function&& function,
    const NLS::Base::Jobs::JobPriority priority = NLS::Base::Jobs::JobPriority::Normal,
    const ThumbnailJobQueue queue = ThumbnailJobQueue::Background)
{
    using Result = std::invoke_result_t<std::decay_t<Function>&>;

    struct JobState
    {
        std::promise<Result> promise;
        std::decay_t<Function> function;
    };

    auto state = std::make_unique<JobState>(JobState {
        std::promise<Result> {},
        std::forward<Function>(function),
    });
    auto future = state->promise.get_future();
    auto* statePtr = state.release();

    const auto runFunction = [](void* userData)
    {
        std::unique_ptr<JobState> ownedState(static_cast<JobState*>(userData));
        try
        {
            if constexpr (std::is_void_v<Result>)
            {
                ownedState->function();
                ownedState->promise.set_value();
            }
            else
            {
                ownedState->promise.set_value(ownedState->function());
            }
        }
        catch (...)
        {
            ownedState->promise.set_exception(std::current_exception());
        }
    };
    const auto cancelFunction = [](void* userData)
    {
        std::unique_ptr<JobState> ownedState(static_cast<JobState*>(userData));
        try
        {
            throw std::runtime_error("thumbnail background job cancelled before execution");
        }
        catch (...)
        {
            ownedState->promise.set_exception(std::current_exception());
        }
    };

    NLS::Base::Jobs::JobHandle handle;
    if (queue == ThumbnailJobQueue::Foreground)
    {
        NLS::Base::Jobs::JobScheduleDesc desc {};
        desc.userData = statePtr;
        desc.debugName = debugName;
        desc.priority = priority;
        desc.function = runFunction;
        desc.cancelUserData = statePtr;
        desc.cancelFunction = cancelFunction;
        handle = NLS::Base::Jobs::ScheduleJob(desc);
    }
    else
    {
        NLS::Base::Jobs::BackgroundJobDesc desc {};
        desc.userData = statePtr;
        desc.debugName = debugName;
        desc.priority = priority;
        desc.function = runFunction;
        desc.cancelUserData = statePtr;
        desc.cancelFunction = cancelFunction;
        handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
    }
    if (handle.id == 0u)
    {
        std::unique_ptr<JobState> ownedState(statePtr);
        throw std::runtime_error(NLS::Base::Jobs::IsJobSystemInitialized()
            ? "thumbnail job scheduling rejected"
            : "thumbnail job scheduling requires initialized JobSystem");
    }

    return future;
}

class ScopedThumbnailGenerationStageThread final
{
public:
    explicit ScopedThumbnailGenerationStageThread(const PerformanceStageThread thread)
        : m_previous(g_thumbnailGenerationStageThread)
    {
        g_thumbnailGenerationStageThread = thread;
    }

    ~ScopedThumbnailGenerationStageThread()
    {
        g_thumbnailGenerationStageThread = m_previous;
    }

    ScopedThumbnailGenerationStageThread(const ScopedThumbnailGenerationStageThread&) = delete;
    ScopedThumbnailGenerationStageThread& operator=(const ScopedThumbnailGenerationStageThread&) = delete;

private:
    PerformanceStageThread m_previous;
};

PerformanceStageThread CurrentThumbnailGenerationStageThread()
{
    return g_thumbnailGenerationStageThread;
}

AssetThumbnailServiceResult GenerateTextureThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation,
    const AssetThumbnailCancelToken& cancelToken);
AssetThumbnailServiceResult GenerateMaterialThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation,
    const AssetThumbnailCancelToken& cancelToken);
AssetThumbnailServiceResult GenerateModelThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation,
    const AssetThumbnailCancelToken& cancelToken);
AssetThumbnailServiceResult GeneratePrefabThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation,
    const AssetThumbnailCancelToken& cancelToken);
bool ShouldRetryLegacyImportedPrefabBudgetFailure(const AssetThumbnailRequest& request);

struct AssetThumbnailKindPolicy
{
    AssetThumbnailKind kind = AssetThumbnailKind::GenericPreview;
    const char* fallbackIcon = "editor.icon.asset.default";
    AssetThumbnailGenerator generator = nullptr;
    const char* unsupportedDiagnostic = "thumbnail-generation-unsupported";
};

constexpr std::array<AssetThumbnailKindPolicy, kAssetThumbnailKindCount> kAssetThumbnailKindPolicies {{
    { AssetThumbnailKind::Icon, "editor.icon.asset.default", nullptr, "thumbnail-generation-unsupported" },
    { AssetThumbnailKind::Texture, "editor.icon.asset.texture", GenerateTextureThumbnail, "thumbnail-generation-unsupported" },
    { AssetThumbnailKind::MaterialSphere, "editor.icon.asset.material", GenerateMaterialThumbnail, "thumbnail-material-preview-generation-failed" },
    { AssetThumbnailKind::ModelPreview, "editor.icon.asset.mesh", GenerateModelThumbnail, "thumbnail-model-preview-generation-failed" },
    { AssetThumbnailKind::PrefabPreview, "editor.icon.asset.prefab", GeneratePrefabThumbnail, "thumbnail-prefab-preview-generation-failed" },
    { AssetThumbnailKind::GenericPreview, "editor.icon.asset.default", nullptr, "thumbnail-generation-unsupported" }
}};

constexpr size_t kMaxMeshPreviewLoadedVertices = 240000u;
constexpr size_t kMaxMeshPreviewLoadedIndices = 720000u;
constexpr size_t kMaxMeshPreviewRenderedTriangles = 12000u;
constexpr float kMeshThumbnailFormalLODScreenSize =
    2.0f / (4.0f * 0.26794919243f); // 30-degree FOV, object framed at four radii.
constexpr size_t kMaxObsoleteThumbnailGenerationInFlightRequests = 2u;
constexpr size_t kMaxCurrentThumbnailGenerationInFlightRequests = 2u;
// Texture thumbnails are CPU-only and cheap to prepare, but they share the
// service with heavy prefab preparation. Keep their visible lane bounded
// independently so a pair of long-lived prefab jobs cannot leave visible PNG
// thumbnails pending indefinitely.
constexpr size_t kMaxVisibleTextureThumbnailGenerationInFlightRequests = 4u;
constexpr size_t kMaxThumbnailGenerationTotalInFlightSlots =
    kMaxObsoleteThumbnailGenerationInFlightRequests +
    kMaxCurrentThumbnailGenerationInFlightRequests +
    kMaxVisibleTextureThumbnailGenerationInFlightRequests;
constexpr size_t kMaxDeferredThumbnailPersistenceTickets = 8u;
// PNG persistence has its own bounded lane. It must not consume the CPU
// preparation slots used by visible non-GPU thumbnails.
constexpr size_t kMaxActiveThumbnailPersistenceRequests =
    kMaxDeferredThumbnailPersistenceTickets;
constexpr size_t kMaxPriorityThumbnailDequeueBurst = 4u;
constexpr auto kThumbnailOffscreenGrace = std::chrono::milliseconds(750);
constexpr size_t kMaxQueuedThumbnailRequests = 512u;
constexpr uint64_t kMaxSourceThumbnailImageBytes = 128ull * 1024ull * 1024ull;
constexpr uint64_t kMaxSourceThumbnailPixels = 4096ull * 4096ull;
constexpr uint32_t kMaxTextureThumbnailGenerationSize = 96u;
constexpr uint64_t kMaxStructurePreviewArtifactPayloadBytes = 1024ull * 1024ull;
constexpr uint64_t kMaxThumbnailPreviewNativeArtifactFileBytes = 128ull * 1024ull * 1024ull;
constexpr size_t kMaxDeferredHeavyGpuPreviewScanPerCall = 8u;
constexpr size_t kMaxDeferredLightGpuPreviewScanPerCall = 96u;
constexpr size_t kMaxResolvedHeavyGpuPreviewManifestLookupsPerCall = 8u;
constexpr size_t kMaxGpuPreviewEmptyFrameRetriesPerGeneration = 3u;
constexpr auto kGpuPreviewEmptyFrameRetryDelay = std::chrono::seconds(1);
// Leave headroom inside the 30-second visible-item terminal deadline for
// startup/import work and for a final result to reach the presentation layer.
// This deadline is intentionally identical in test-hook and production builds:
// test hooks must not turn a stalled resource continuation into a permanent
// WaitingForResources state.
// Large unloaded prefabs can spend tens of seconds building their immutable
// preview plan before bounded resource uploads begin. This is an active-work
// timeout, not a queue-admission timeout, so allow that legitimate path to
// complete while still retiring genuinely stalled continuations.
constexpr auto kGpuPreviewResourcePendingTimeout = std::chrono::seconds(120);
// Once admitted work is no longer waiting on renderer resources, every visible
// request must reach Ready, Failed, or Fallback before the 30-second
// completeness budget. Resource continuations use their longer deadline above.
constexpr auto kVisibleThumbnailRequestTimeout = std::chrono::seconds(20);
#if defined(NLS_ENABLE_TEST_HOOKS)
// Keep the test deadline above the time needed by Debug queue-pressure tests
// to populate the bounded request set; the production deadline remains strict.
constexpr auto kGpuPreviewReadbackPendingTimeout = std::chrono::seconds(5);
#else
constexpr auto kGpuPreviewReadbackPendingTimeout = std::chrono::seconds(20);
#endif
constexpr const char* kSourcePreviewBudgetExceededDiagnostic =
    "thumbnail-source-preview-budget-exceeded";
constexpr const char* kMaterialPreviewBudgetExceededDiagnostic =
    "thumbnail-material-preview-budget-exceeded";
constexpr const char* kPrefabPreviewBudgetExceededDiagnostic =
    "thumbnail-prefab-preview-budget-exceeded";
constexpr const char* kLargePrefabPreviewAwaitingResidentDiagnostic =
    "thumbnail-prefab-preview-awaiting-resident-load";

bool ShouldRefreshGpuPreviewResourceProgress(
    const uint64_t previousProgressToken,
    const uint64_t progressToken,
    const bool resourceWorkActive)
{
    return resourceWorkActive ||
        (progressToken != 0u && progressToken != previousProgressToken);
}

constexpr bool AssetThumbnailKindPoliciesAreExhaustive()
{
    if (kAssetThumbnailKindPolicies.size() != kAssetThumbnailKindCount)
        return false;

    std::array<bool, kAssetThumbnailKindCount> seen {};
    for (const auto& policy : kAssetThumbnailKindPolicies)
    {
        const auto index = static_cast<size_t>(policy.kind);
        if (index >= kAssetThumbnailKindCount || seen[index])
            return false;
        seen[index] = true;
    }

    for (const bool covered : seen)
    {
        if (!covered)
            return false;
    }
    return true;
}

static_assert(AssetThumbnailKindPoliciesAreExhaustive());

const AssetThumbnailKindPolicy* PolicyForKind(const AssetThumbnailKind kind)
{
    const auto index = static_cast<size_t>(kind);
    if (index >= kAssetThumbnailKindCount)
        return nullptr;

    for (const auto& policy : kAssetThumbnailKindPolicies)
    {
        if (policy.kind == kind)
            return &policy;
    }
    return nullptr;
}

AssetThumbnailKind ThumbnailKindForItem(const AssetBrowserItem& item)
{
    switch (item.type)
    {
    case AssetBrowserItemType::Texture:
        return AssetThumbnailKind::Texture;
    case AssetBrowserItemType::Material:
        return AssetThumbnailKind::MaterialSphere;
    case AssetBrowserItemType::Model:
        if (item.kind == AssetBrowserItemKind::SourceAsset)
            return AssetThumbnailKind::PrefabPreview;
        return AssetThumbnailKind::ModelPreview;
    case AssetBrowserItemType::Mesh:
        return AssetThumbnailKind::ModelPreview;
    case AssetBrowserItemType::Prefab:
        return AssetThumbnailKind::PrefabPreview;
    default:
        return AssetThumbnailKind::Icon;
    }
}

constexpr const char* kLegacyThumbnailRendererVersion = "asset-browser-thumbnail-renderer:v8";
constexpr const char* kUpperObliqueCpuThumbnailRendererVersion = "asset-browser-thumbnail-renderer:v9";
constexpr const char* kUpperObliqueGpuThumbnailRendererVersion = "asset-browser-thumbnail-renderer:v13";
constexpr const char* kUpperObliqueGpuPrefabThumbnailRendererVersion = "asset-browser-thumbnail-renderer:v35";
constexpr const char* kPbrMaterialThumbnailRendererVersion = "asset-browser-thumbnail-renderer:v12";

std::string FallbackIconForKind(const AssetThumbnailKind kind)
{
    const auto* policy = PolicyForKind(kind);
    return policy != nullptr ? policy->fallbackIcon : "editor.icon.asset.default";
}

bool CanGenerateThumbnail(const AssetThumbnailKind kind)
{
    const auto* policy = PolicyForKind(kind);
    return policy != nullptr && policy->generator != nullptr;
}

bool IsPendingThumbnailState(const ThumbnailState state)
{
    return state == ThumbnailState::Queued ||
        state == ThumbnailState::Preparing ||
        state == ThumbnailState::WaitingForResources ||
        state == ThumbnailState::Rendering ||
        state == ThumbnailState::WaitingForGpu ||
        state == ThumbnailState::Readback ||
        state == ThumbnailState::Encoding ||
        state == ThumbnailState::Persisting;
}

bool IsActiveThumbnailReadbackOrPersistenceState(const ThumbnailState state)
{
    return state == ThumbnailState::WaitingForGpu ||
        state == ThumbnailState::Readback ||
        state == ThumbnailState::Encoding ||
        state == ThumbnailState::Persisting;
}

const char* ThumbnailStateTelemetryName(const ThumbnailState state)
{
    switch (state)
    {
    case ThumbnailState::Missing:
        return "missing";
    case ThumbnailState::Queued:
        return "queued";
    case ThumbnailState::Preparing:
        return "preparing";
    case ThumbnailState::WaitingForResources:
        return "waiting-resources";
    case ThumbnailState::Rendering:
        return "rendering";
    case ThumbnailState::WaitingForGpu:
        return "waiting-gpu";
    case ThumbnailState::Readback:
        return "readback";
    case ThumbnailState::Encoding:
        return "encoding";
    case ThumbnailState::Persisting:
        return "persisting";
    case ThumbnailState::Ready:
        return "ready";
    case ThumbnailState::Failed:
        return "failed";
    case ThumbnailState::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

bool SupportsGpuThumbnailPreview(const AssetThumbnailKind kind)
{
    return kind == AssetThumbnailKind::ModelPreview ||
        kind == AssetThumbnailKind::MaterialSphere ||
        kind == AssetThumbnailKind::PrefabPreview;
}

bool IsCpuMeshModelPreviewRequest(const AssetThumbnailRequest& request)
{
    if (request.kind != AssetThumbnailKind::ModelPreview ||
        request.subAssetKey.rfind("mesh:", 0u) != 0u)
    {
        return false;
    }

    if (request.generatedSubAsset)
        return true;

    return false;
}

bool SupportsGpuThumbnailPreview(const AssetThumbnailRequest& request)
{
    return SupportsGpuThumbnailPreview(request.kind) &&
        !IsCpuMeshModelPreviewRequest(request);
}

bool IsHeavyGpuThumbnailPreview(const AssetThumbnailKind kind)
{
    return kind == AssetThumbnailKind::PrefabPreview;
}

bool IsUnresolvedSourceModelPreviewRequest(const AssetThumbnailRequest& request)
{
    return request.kind == AssetThumbnailKind::ModelPreview &&
        request.artifactPath.empty();
}

std::filesystem::path ResolveThumbnailSourcePath(const AssetThumbnailRequest& request);

bool ShouldDeferBackgroundCpuThumbnailToPreviewRenderer(const AssetThumbnailKind kind)
{
    return kind == AssetThumbnailKind::MaterialSphere ||
        kind == AssetThumbnailKind::ModelPreview ||
        kind == AssetThumbnailKind::PrefabPreview;
}

bool IsVisibleTextureThumbnailRequest(const AssetThumbnailRequest& request)
{
    return request.kind == AssetThumbnailKind::Texture &&
        request.priority == ThumbnailRequestPriority::Visible;
}

bool IsDirectSourceTextureThumbnailRequest(const AssetThumbnailRequest& request)
{
    if (!IsVisibleTextureThumbnailRequest(request) ||
        !request.directSourceTexture ||
        !request.subAssetKey.empty() ||
        !request.artifactPath.empty() ||
        request.generatedSubAsset)
    {
        return false;
    }

    const auto sourcePath = ResolveThumbnailSourcePath(request);
    std::error_code error;
    const auto sourceSize = std::filesystem::file_size(sourcePath, error);
    // This is still a bounded worker path: IsDirectReadableTextureThumbnailSource
    // already limits the source to the thumbnail byte/pixel budgets. Large
    // source images must remain direct (no artifact read), but visible requests
    // need the high-priority worker lane so a background asset-preparation queue
    // cannot leave the last visible textures pending until the deadline.
    return !sourcePath.empty() &&
        !error &&
        sourceSize <= kMaxSourceThumbnailImageBytes;
}

bool CanRequestThumbnailGeneration(const AssetThumbnailKind kind)
{
    return CanGenerateThumbnail(kind) || SupportsGpuThumbnailPreview(kind);
}

AssetThumbnailGenerator GeneratorForKind(const AssetThumbnailKind kind)
{
    const auto* policy = PolicyForKind(kind);
    return policy != nullptr ? policy->generator : nullptr;
}

std::string UnsupportedDiagnosticForKind(const AssetThumbnailKind kind)
{
    const auto* policy = PolicyForKind(kind);
    return policy != nullptr
        ? policy->unsupportedDiagnostic
        : "thumbnail-generation-unsupported";
}

bool IsThumbnailGenerationCancelled(const AssetThumbnailCancelToken& cancelToken)
{
    const auto token = cancelToken.lock();
    return token == nullptr || token->cancelled.load(std::memory_order_relaxed);
}

bool IsDeferredThumbnailPreviewPersistenceDiagnostic(const std::string& diagnostic);

bool IsRetryableThumbnailFailureDiagnostic(const std::string& diagnostic)
{
    if (diagnostic == "thumbnail-gpu-preview-empty-frame")
        return false;
    if (IsDeferredThumbnailPreviewPersistenceDiagnostic(diagnostic))
        return false;
    if (diagnostic.rfind("thumbnail-gpu-preview-resources-timeout:", 0u) == 0u)
        return false;
    if (diagnostic.rfind("thumbnail-gpu-preview-", 0u) == 0u)
        return true;
    if (diagnostic == kPrefabPreviewBudgetExceededDiagnostic)
        return true;
    return diagnostic == "thumbnail-material-preview-hook-unavailable" ||
        diagnostic == "thumbnail-model-preview-hook-unavailable" ||
        diagnostic == "thumbnail-prefab-preview-hook-unavailable" ||
        diagnostic == "thumbnail-generation-out-of-memory" ||
        diagnostic == "thumbnail-generation-exception" ||
        diagnostic == "thumbnail-material-gpu-preview-required" ||
        diagnostic == "thumbnail-model-gpu-preview-required" ||
        diagnostic == "thumbnail-prefab-gpu-preview-required" ||
        diagnostic == "thumbnail-material-artifact-missing" ||
        diagnostic == "thumbnail-prefab-artifact-missing" ||
        diagnostic == "thumbnail-material-preview-generation-failed" ||
        diagnostic == "thumbnail-model-preview-generation-failed" ||
        diagnostic == "thumbnail-prefab-preview-generation-failed" ||
        diagnostic == "thumbnail-generation-worker-start-failed";
}

bool IsRetryableThumbnailFailureDiagnostic(
    const AssetThumbnailRequest& request,
    const std::string& diagnostic)
{
    if (diagnostic == kPrefabPreviewBudgetExceededDiagnostic)
        return ShouldRetryLegacyImportedPrefabBudgetFailure(request);
    return IsRetryableThumbnailFailureDiagnostic(diagnostic);
}

bool IsImportedPrefabContinuationRecoveryDiagnostic(const std::string& diagnostic)
{
    return diagnostic == kLargePrefabPreviewAwaitingResidentDiagnostic ||
        diagnostic.rfind("thumbnail-gpu-preview-resources-timeout:", 0u) == 0u;
}

bool IsPendingThumbnailPreviewReadbackDiagnostic(const std::string& diagnostic)
{
    return diagnostic == "thumbnail-gpu-preview-readback-pending" ||
        (diagnostic.rfind("thumbnail-gpu-preview-readback-failed:", 0u) == 0u &&
            diagnostic.find("previous async readback has not been completed") != std::string::npos);
}

bool IsPendingThumbnailPreviewResourcesDiagnostic(const std::string& diagnostic)
{
    constexpr std::string_view kResourcesPendingDiagnostic = "thumbnail-gpu-preview-resources-pending";
    if (diagnostic.size() < kResourcesPendingDiagnostic.size() ||
        diagnostic.compare(0u, kResourcesPendingDiagnostic.size(), kResourcesPendingDiagnostic) != 0)
    {
        return false;
    }
    return diagnostic.size() == kResourcesPendingDiagnostic.size() ||
        diagnostic[kResourcesPendingDiagnostic.size()] == '|' ||
        diagnostic[kResourcesPendingDiagnostic.size()] == ':';
}

bool IsPendingPrefabPreviewSceneAssemblyDiagnostic(const std::string& diagnostic)
{
    constexpr std::string_view kSceneAssemblyPendingDiagnostic =
        "thumbnail-gpu-preview-resources-pending:prefab-scene-assembly=";
    return diagnostic.size() > kSceneAssemblyPendingDiagnostic.size() &&
        diagnostic.compare(
            0u,
            kSceneAssemblyPendingDiagnostic.size(),
            kSceneAssemblyPendingDiagnostic) == 0;
}

bool IsDeferredThumbnailPreviewPersistenceDiagnostic(const std::string& diagnostic)
{
    return diagnostic == "thumbnail-gpu-preview-persistence-deferred";
}

bool IsTruncatedThumbnailPreviewResourcesDiagnostic(const std::string& diagnostic)
{
    if (!IsPendingThumbnailPreviewResourcesDiagnostic(diagnostic))
        return false;

    constexpr std::string_view kTruncatedToken = "truncated=1";
    return diagnostic.find(kTruncatedToken) != std::string::npos;
}

bool MeshPreviewHeaderExceedsCpuLoadBudget(
    const NLS::Render::Assets::MeshArtifactHeaderPreview& header)
{
    return header.vertexCount > kMaxMeshPreviewLoadedVertices ||
        header.indexCount > kMaxMeshPreviewLoadedIndices;
}

std::optional<NLS::Render::Assets::MeshArtifactData> LoadMeshArtifactForThumbnailPreview(
    const std::filesystem::path& path,
    const NLS::Render::Assets::MeshArtifactHeaderPreview& header)
{
    if (!header.isLODBundle && MeshPreviewHeaderExceedsCpuLoadBudget(header))
        return std::nullopt;

    auto mesh = NLS::Render::Assets::LoadMeshArtifactLOD(
        path,
        kMeshThumbnailFormalLODScreenSize);
    if (!mesh.has_value() ||
        mesh->vertices.size() > kMaxMeshPreviewLoadedVertices ||
        mesh->indices.size() > kMaxMeshPreviewLoadedIndices)
    {
        return std::nullopt;
    }
    return mesh;
}

bool IsTextureThumbnailSourceExtension(const std::filesystem::path& path)
{
    auto extension = path.extension().generic_string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });

    return extension == ".png" ||
        extension == ".jpg" ||
        extension == ".jpeg" ||
        extension == ".tga" ||
        extension == ".bmp";
}

uint16_t ReadBigEndianUInt16(const uint8_t* data)
{
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8u) |
        static_cast<uint16_t>(data[1]));
}

uint32_t ReadBigEndianUInt32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24u) |
        (static_cast<uint32_t>(data[1]) << 16u) |
        (static_cast<uint32_t>(data[2]) << 8u) |
        static_cast<uint32_t>(data[3]);
}

uint16_t ReadLittleEndianUInt16(const uint8_t* data)
{
    return static_cast<uint16_t>(
        static_cast<uint16_t>(data[0]) |
        (static_cast<uint16_t>(data[1]) << 8u));
}

uint32_t ReadLittleEndianUInt32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8u) |
        (static_cast<uint32_t>(data[2]) << 16u) |
        (static_cast<uint32_t>(data[3]) << 24u);
}

bool ReadFilePrefix(
    const std::filesystem::path& path,
    std::vector<uint8_t>& bytes,
    const size_t maxBytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input || maxBytes == 0u)
        return false;

    bytes.resize(maxBytes);
    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    const auto readCount = input.gcount();
    if (readCount <= 0)
    {
        bytes.clear();
        return false;
    }

    bytes.resize(static_cast<size_t>(readCount));
    return true;
}

struct ImageHeaderDimensions
{
    uint32_t width = 0u;
    uint32_t height = 0u;
};

std::optional<ImageHeaderDimensions> ReadPngHeaderDimensions(const std::filesystem::path& path)
{
    constexpr std::array<uint8_t, 8u> kPngSignature {
        0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au
    };
    std::vector<uint8_t> header;
    if (!ReadFilePrefix(path, header, 33u) || header.size() < 33u)
        return std::nullopt;
    if (!std::equal(kPngSignature.begin(), kPngSignature.end(), header.begin()))
        return std::nullopt;
    if (ReadBigEndianUInt32(header.data() + 8u) != 13u ||
        header[12u] != 'I' ||
        header[13u] != 'H' ||
        header[14u] != 'D' ||
        header[15u] != 'R')
    {
        return std::nullopt;
    }

    ImageHeaderDimensions dimensions;
    dimensions.width = ReadBigEndianUInt32(header.data() + 16u);
    dimensions.height = ReadBigEndianUInt32(header.data() + 20u);
    if (dimensions.width == 0u || dimensions.height == 0u)
        return std::nullopt;
    return dimensions;
}

std::optional<ImageHeaderDimensions> ReadBmpHeaderDimensions(const std::filesystem::path& path)
{
    std::vector<uint8_t> header;
    if (!ReadFilePrefix(path, header, 26u) || header.size() < 26u)
        return std::nullopt;
    if (header[0u] != 'B' || header[1u] != 'M')
        return std::nullopt;

    const auto dibHeaderSize = ReadLittleEndianUInt32(header.data() + 14u);
    ImageHeaderDimensions dimensions;
    if (dibHeaderSize == 12u)
    {
        dimensions.width = ReadLittleEndianUInt16(header.data() + 18u);
        dimensions.height = ReadLittleEndianUInt16(header.data() + 20u);
    }
    else if (dibHeaderSize >= 40u)
    {
        dimensions.width = ReadLittleEndianUInt32(header.data() + 18u);
        const auto signedHeight = static_cast<int64_t>(
            static_cast<int32_t>(ReadLittleEndianUInt32(header.data() + 22u)));
        dimensions.height = static_cast<uint32_t>(signedHeight < 0 ? -signedHeight : signedHeight);
    }
    if (dimensions.width == 0u || dimensions.height == 0u)
        return std::nullopt;
    return dimensions;
}

std::optional<ImageHeaderDimensions> ReadTgaHeaderDimensions(const std::filesystem::path& path)
{
    std::vector<uint8_t> header;
    if (!ReadFilePrefix(path, header, 18u) || header.size() < 18u)
        return std::nullopt;

    ImageHeaderDimensions dimensions;
    dimensions.width = ReadLittleEndianUInt16(header.data() + 12u);
    dimensions.height = ReadLittleEndianUInt16(header.data() + 14u);
    if (dimensions.width == 0u || dimensions.height == 0u)
        return std::nullopt;
    return dimensions;
}

bool IsJpegStartOfFrameMarker(const uint8_t marker)
{
    switch (marker)
    {
    case 0xC0u:
    case 0xC1u:
    case 0xC2u:
    case 0xC3u:
    case 0xC5u:
    case 0xC6u:
    case 0xC7u:
    case 0xC9u:
    case 0xCAu:
    case 0xCBu:
    case 0xCDu:
    case 0xCEu:
    case 0xCFu:
        return true;
    default:
        return false;
    }
}

std::optional<ImageHeaderDimensions> ReadJpegHeaderDimensions(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;

    const std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    if (bytes.size() < 4u)
        return std::nullopt;
    if (bytes[0u] != 0xFFu || bytes[1u] != 0xD8u)
        return std::nullopt;

    size_t offset = 2u;
    while (offset + 3u < bytes.size())
    {
        while (offset < bytes.size() && bytes[offset] != 0xFFu)
            ++offset;
        while (offset < bytes.size() && bytes[offset] == 0xFFu)
            ++offset;
        if (offset >= bytes.size())
            break;

        const auto marker = bytes[offset++];
        if (marker == 0xD9u || marker == 0xDAu)
            break;
        if ((marker >= 0xD0u && marker <= 0xD7u) || marker == 0x01u)
            continue;
        if (offset + 2u > bytes.size())
            break;

        const auto segmentLength = ReadBigEndianUInt16(bytes.data() + offset);
        if (segmentLength < 2u || offset + segmentLength > bytes.size())
            break;

        if (IsJpegStartOfFrameMarker(marker) && segmentLength >= 7u)
        {
            ImageHeaderDimensions dimensions;
            dimensions.height = ReadBigEndianUInt16(bytes.data() + offset + 3u);
            dimensions.width = ReadBigEndianUInt16(bytes.data() + offset + 5u);
            if (dimensions.width == 0u || dimensions.height == 0u)
                return std::nullopt;
            return dimensions;
        }
        offset += segmentLength;
    }
    return std::nullopt;
}

bool IsKnownSourceImageExtension(const std::filesystem::path& path)
{
    auto extension = path.extension().generic_string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return extension == ".png" ||
        extension == ".bmp" ||
        extension == ".tga" ||
        extension == ".jpg" ||
        extension == ".jpeg";
}

std::optional<ImageHeaderDimensions> ReadImageHeaderDimensions(const std::filesystem::path& path)
{
    auto extension = path.extension().generic_string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });

    if (extension == ".png")
        return ReadPngHeaderDimensions(path);
    if (extension == ".bmp")
        return ReadBmpHeaderDimensions(path);
    if (extension == ".tga")
        return ReadTgaHeaderDimensions(path);
    if (extension == ".jpg" || extension == ".jpeg")
        return ReadJpegHeaderDimensions(path);
    return std::nullopt;
}

bool ImageDimensionsExceedPreviewBudget(const ImageHeaderDimensions& dimensions)
{
    const auto pixels =
        static_cast<uint64_t>(dimensions.width) *
        static_cast<uint64_t>(dimensions.height);
    return pixels > kMaxSourceThumbnailPixels;
}

bool IsDirectReadableTextureThumbnailSource(const std::filesystem::path& path)
{
    if (path.empty() || !IsTextureThumbnailSourceExtension(path))
        return false;

    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        return false;

    error.clear();
    if (std::filesystem::file_size(path, error) > kMaxSourceThumbnailImageBytes || error)
        return false;

    const auto dimensions = ReadImageHeaderDimensions(path);
    // A known image extension with an unreadable header is kept on the artifact
    // path so embedded/imported texture data can still provide a fallback.
    return dimensions.has_value() && !ImageDimensionsExceedPreviewBudget(*dimensions);
}

std::string ToLowerAscii(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool ShouldRetryLegacyImportedPrefabBudgetFailure(const AssetThumbnailRequest& request)
{
    return request.kind == AssetThumbnailKind::PrefabPreview &&
        ToLowerAscii(std::filesystem::path(request.sourceAssetPath).extension().generic_string()) != ".prefab";
}

std::vector<uint8_t> ConvertToRgba8(const NLS::Image& image)
{
    const auto* source = image.GetData();
    if (source == nullptr)
        return {};

    const auto width = image.GetWidth();
    const auto height = image.GetHeight();
    const auto channels = image.GetChannels();
    if (width <= 0 || height <= 0 || channels <= 0 || channels > 4)
        return {};

    const auto pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<uint8_t> rgba(pixelCount * 4u, 255u);
    for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
    {
        const auto sourceIndex = pixel * static_cast<size_t>(channels);
        const auto targetIndex = pixel * 4u;
        switch (channels)
        {
        case 1:
            rgba[targetIndex + 0u] = source[sourceIndex + 0u];
            rgba[targetIndex + 1u] = source[sourceIndex + 0u];
            rgba[targetIndex + 2u] = source[sourceIndex + 0u];
            break;
        case 2:
            rgba[targetIndex + 0u] = source[sourceIndex + 0u];
            rgba[targetIndex + 1u] = source[sourceIndex + 0u];
            rgba[targetIndex + 2u] = source[sourceIndex + 0u];
            rgba[targetIndex + 3u] = source[sourceIndex + 1u];
            break;
        case 3:
            rgba[targetIndex + 0u] = source[sourceIndex + 0u];
            rgba[targetIndex + 1u] = source[sourceIndex + 1u];
            rgba[targetIndex + 2u] = source[sourceIndex + 2u];
            break;
        case 4:
            rgba[targetIndex + 0u] = source[sourceIndex + 0u];
            rgba[targetIndex + 1u] = source[sourceIndex + 1u];
            rgba[targetIndex + 2u] = source[sourceIndex + 2u];
            rgba[targetIndex + 3u] = source[sourceIndex + 3u];
            break;
        default:
            return {};
        }
    }
    return rgba;
}

struct DownsampledThumbnail
{
    std::vector<uint8_t> pixels;
    uint32_t width = 0u;
    uint32_t height = 0u;
};

struct ThumbnailTextureSampleData
{
    std::vector<uint8_t> pixels;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t rowPitch = 0u;
    bool flipV = false;
};

struct MaterialTextureReference
{
    std::string resourcePath;
    std::string textureKey;
};

DownsampledThumbnail DownsampleRgba8ToThumbnail(
    const uint8_t* sourcePixels,
    const uint32_t sourceWidth,
    const uint32_t sourceHeight,
    const uint32_t sourceRowPitch,
    const uint32_t requestedSize)
{
    DownsampledThumbnail thumbnail;
    const auto clampedSize = std::max(1u, requestedSize);
    if (sourcePixels == nullptr || sourceWidth == 0u || sourceHeight == 0u || sourceRowPitch < sourceWidth * 4u)
        return thumbnail;

    const auto largestDimension = (std::max)(sourceWidth, sourceHeight);
    const auto targetLargestDimension = (std::min)(largestDimension, clampedSize);
    thumbnail.width = (std::max)(1u, static_cast<uint32_t>(
        (static_cast<uint64_t>(sourceWidth) * targetLargestDimension + largestDimension - 1u) /
        largestDimension));
    thumbnail.height = (std::max)(1u, static_cast<uint32_t>(
        (static_cast<uint64_t>(sourceHeight) * targetLargestDimension + largestDimension - 1u) /
        largestDimension));

    thumbnail.pixels.resize(static_cast<size_t>(thumbnail.width) * thumbnail.height * 4u);
    for (uint32_t y = 0u; y < thumbnail.height; ++y)
    {
        const auto sourceY = (std::min)(
            static_cast<uint32_t>((static_cast<uint64_t>(y) * sourceHeight) / thumbnail.height),
            sourceHeight - 1u);
        for (uint32_t x = 0u; x < thumbnail.width; ++x)
        {
            const auto sourceX = (std::min)(
                static_cast<uint32_t>((static_cast<uint64_t>(x) * sourceWidth) / thumbnail.width),
                sourceWidth - 1u);
            const auto* source = sourcePixels + static_cast<size_t>(sourceY) * sourceRowPitch + sourceX * 4u;
            auto* target = thumbnail.pixels.data() +
                (static_cast<size_t>(y) * thumbnail.width + x) * 4u;
            std::copy_n(source, 4u, target);
        }
    }
    return thumbnail;
}

DownsampledThumbnail DownsampleImageToThumbnail(
    const NLS::Image& image,
    const uint32_t requestedSize)
{
    DownsampledThumbnail thumbnail;
    const auto sourceWidth = image.GetWidth();
    const auto sourceHeight = image.GetHeight();
    if (sourceWidth <= 0 || sourceHeight <= 0)
        return thumbnail;

    const auto sourcePixels = ConvertToRgba8(image);
    if (sourcePixels.empty())
        return {};

    return DownsampleRgba8ToThumbnail(
        sourcePixels.data(),
        static_cast<uint32_t>(sourceWidth),
        static_cast<uint32_t>(sourceHeight),
        static_cast<uint32_t>(sourceWidth) * 4u,
        requestedSize);
}

uint32_t GetTextureThumbnailGenerationSize(const AssetThumbnailRequest& request)
{
    return (std::min)(std::max(1u, request.requestedSize), kMaxTextureThumbnailGenerationSize);
}

bool IsRgba8TextureArtifactMipUsable(const NLS::Render::Assets::TextureArtifactMip& mip)
{
    return mip.width > 0u &&
        mip.height > 0u &&
        mip.rowPitch >= mip.width * 4u &&
        mip.HasPixels() &&
        mip.PixelSize() >= static_cast<size_t>(mip.rowPitch) * mip.height;
}

std::vector<uint8_t> CopyTextureArtifactMipPixels(const NLS::Render::Assets::TextureArtifactMip& mip)
{
    const auto* pixels = mip.PixelData();
    if (pixels == nullptr)
        return {};
    return {pixels, pixels + mip.PixelSize()};
}

const NLS::Render::Assets::TextureArtifactMip* SelectTextureThumbnailMip(
    const NLS::Render::Assets::TextureArtifactData& texture,
    const uint32_t targetSize)
{
    const NLS::Render::Assets::TextureArtifactMip* best = nullptr;
    uint64_t bestPixels = 0u;
    const auto minUsableDimension = std::max(1u, targetSize);

    for (const auto& mip : texture.mips)
    {
        if (!IsRgba8TextureArtifactMipUsable(mip))
            continue;

        const auto pixels = static_cast<uint64_t>(mip.width) * static_cast<uint64_t>(mip.height);
        const auto coversTarget = mip.width >= minUsableDimension || mip.height >= minUsableDimension;
        if (!best)
        {
            best = &mip;
            bestPixels = pixels;
            continue;
        }

        const auto bestCoversTarget = best->width >= minUsableDimension || best->height >= minUsableDimension;
        if (coversTarget != bestCoversTarget)
        {
            if (coversTarget)
            {
                best = &mip;
                bestPixels = pixels;
            }
            continue;
        }

        if ((coversTarget && pixels < bestPixels) || (!coversTarget && pixels > bestPixels))
        {
            best = &mip;
            bestPixels = pixels;
        }
    }

    return best;
}

bool ThumbnailPathHasReparsePoint(const std::filesystem::path& path)
{
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u;
#else
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    return !error && std::filesystem::is_symlink(status);
#endif
}

bool PathStartsWithAssetsMount(const std::filesystem::path& normalizedAssetPath)
{
    const auto first = normalizedAssetPath.begin();
    return first != normalizedAssetPath.end() && *first == "Assets";
}

std::optional<std::filesystem::path> TryResolveProjectRelativeThumbnailSourcePathFast(
    const AssetThumbnailRequest& request)
{
    if (request.projectRoot.empty() || request.sourceAssetPath.empty())
        return std::nullopt;

    const auto normalizedAssetPath = std::filesystem::path(NormalizeEditorAssetPath(request.sourceAssetPath));
    if (normalizedAssetPath.empty() ||
        normalizedAssetPath == "." ||
        normalizedAssetPath == ".." ||
        normalizedAssetPath.is_absolute() ||
        !PathStartsWithAssetsMount(normalizedAssetPath))
    {
        return std::nullopt;
    }

    const auto assetRoot = NLS::Core::Assets::NormalizeAssetPath(request.projectRoot / "Assets");
    const auto candidate = NLS::Core::Assets::NormalizeAssetPath(request.projectRoot / normalizedAssetPath);
    if (assetRoot.empty() ||
        candidate.empty() ||
        !IsPathInsideEditorAssetRoot(candidate, assetRoot))
    {
        return std::nullopt;
    }

    auto ancestor = candidate.parent_path();
    while (!ancestor.empty() && IsPathInsideEditorAssetRoot(ancestor, assetRoot))
    {
        std::error_code error;
        const bool exists = std::filesystem::exists(ancestor, error);
        if (error || !exists)
            return std::nullopt;
        if (ThumbnailPathHasReparsePoint(ancestor))
            return std::nullopt;
        if (ancestor == assetRoot)
            break;
        const auto parent = ancestor.parent_path();
        if (parent == ancestor)
            return std::nullopt;
        ancestor = parent;
    }

    std::error_code statusError;
    (void)std::filesystem::symlink_status(candidate, statusError);
    if (!statusError && ThumbnailPathHasReparsePoint(candidate))
        return std::nullopt;
    if (statusError)
    {
        std::error_code existsError;
        if (std::filesystem::exists(candidate, existsError) || existsError)
            return std::nullopt;
    }

    return candidate;
}

std::filesystem::path ResolveThumbnailSourcePath(const AssetThumbnailRequest& request)
{
    if (const auto fastPath = TryResolveProjectRelativeThumbnailSourcePathFast(request);
        fastPath.has_value())
    {
        return *fastPath;
    }

    return ResolveEditorAssetPath(
        MakeProjectEditorAssetRoots(request.projectRoot),
        request.sourceAssetPath);
}

std::filesystem::path ResolveThumbnailSourcePathCached(
    const AssetThumbnailRequest& request,
    AssetThumbnailRequestBuildContext* context)
{
    if (context == nullptr)
        return ResolveThumbnailSourcePath(request);

    const auto key = request.projectRoot.lexically_normal().generic_string() + "|" +
        NormalizeEditorAssetPath(request.sourceAssetPath);
    auto found = context->sourcePathsByProjectAndAssetPath.find(key);
    if (found != context->sourcePathsByProjectAndAssetPath.end())
        return found->second;

    auto sourcePath = ResolveThumbnailSourcePath(request);
    auto [inserted, insertedNew] =
        context->sourcePathsByProjectAndAssetPath.emplace(key, std::move(sourcePath));
    (void)insertedNew;
    return inserted->second;
}

std::filesystem::path ResolveThumbnailArtifactPath(const AssetThumbnailRequest& request)
{
    if (request.artifactPath.empty() || !request.assetId.IsValid())
        return {};

    const auto rawPath = std::filesystem::path(request.artifactPath).lexically_normal();
    const auto sourceArtifactRoot = NLS::Core::Assets::NormalizeAssetPath(
        request.projectRoot / "Library" / "Artifacts");
    if (sourceArtifactRoot.empty())
        return {};

    auto resolveCandidate = [&sourceArtifactRoot](const std::filesystem::path& candidate)
        -> std::filesystem::path
    {
        const auto normalized = NLS::Core::Assets::NormalizeAssetPath(candidate);
        if (!normalized.empty() &&
            IsPhysicalRegularFileInsideEditorAssetRoot(normalized, sourceArtifactRoot))
        {
            return normalized;
        }
        return {};
    };

    if (rawPath.is_absolute())
        return resolveCandidate(rawPath);

    const auto candidate = resolveCandidate(request.projectRoot / rawPath);
    if (!candidate.empty())
        return candidate;

    const auto artifactRootCandidate = resolveCandidate(sourceArtifactRoot / rawPath);
    if (!artifactRootCandidate.empty())
        return artifactRootCandidate;

    return {};
}

bool IsMissingThumbnailArtifactPath(const AssetThumbnailRequest& request)
{
    if (request.artifactPath.empty() || !request.assetId.IsValid())
        return false;

    const auto sourceArtifactRoot = NLS::Core::Assets::NormalizeAssetPath(
        request.projectRoot / "Library" / "Artifacts");
    if (sourceArtifactRoot.empty())
        return false;

    const auto rawPath = std::filesystem::path(request.artifactPath).lexically_normal();
    std::vector<std::filesystem::path> candidates;
    if (rawPath.is_absolute())
    {
        candidates.push_back(rawPath);
    }
    else
    {
        candidates.push_back(request.projectRoot / rawPath);
        candidates.push_back(sourceArtifactRoot / rawPath);
    }

    for (const auto& candidate : candidates)
    {
        const auto normalized = NLS::Core::Assets::NormalizeAssetPath(candidate);
        if (normalized.empty() || !IsPathInsideEditorAssetRoot(normalized, sourceArtifactRoot))
            continue;

        std::error_code error;
        const bool exists = std::filesystem::exists(normalized, error);
        if (!error && !exists)
            return true;
        if (!error && exists)
            return false;
    }

    return false;
}

std::vector<uint8_t> ReadAllBytes(const std::filesystem::path& path);

std::string FileStamp(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return "missing";

    error.clear();
    const auto writeTime = std::filesystem::last_write_time(path, error);
    if (error)
        return "missing";

    const auto writeTimeTicks = static_cast<std::intmax_t>(writeTime.time_since_epoch().count());
    return std::to_string(size) + ":" + std::to_string(writeTimeTicks);
}

std::string FileStampCached(
    const std::filesystem::path& path,
    AssetThumbnailRequestBuildContext* context)
{
    if (context == nullptr)
        return FileStamp(path);

    const auto key = path.lexically_normal().generic_string();
    auto found = context->fileStampsByPath.find(key);
    if (found != context->fileStampsByPath.end())
        return found->second;

    auto stamp = FileStamp(path);
    auto [inserted, insertedNew] = context->fileStampsByPath.emplace(key, std::move(stamp));
    (void)insertedNew;
    return inserted->second;
}

std::string ArtifactRecordStamp(const std::string& artifactPath)
{
    const auto portable =
        NLS::Core::Assets::TryMakePortableContentArtifactPath(artifactPath);
    return portable.empty()
        ? NormalizeEditorAssetPath(artifactPath)
        : NormalizeEditorAssetPath(portable);
}

void AddSourceFreshnessInputs(
    AssetThumbnailRequest& request,
    AssetThumbnailRequestBuildContext* context)
{
    const auto telemetryPath = BuildThumbnailRequestTelemetryPath(request);
    auto telemetryBegin = std::chrono::steady_clock::now();
    const auto sourcePath = ResolveThumbnailSourcePathCached(request, context);
    RecordThumbnailRequestBuildTelemetry(
        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshnessResolve,
        telemetryBegin,
        telemetryPath);
    if (sourcePath.empty())
    {
        request.freshnessInputs.push_back({"source-file", "missing"});
        request.freshnessInputs.push_back({"source-meta", "missing"});
        return;
    }

    telemetryBegin = std::chrono::steady_clock::now();
    request.freshnessInputs.push_back({"source-file", FileStampCached(sourcePath, context)});
    RecordThumbnailRequestBuildTelemetry(
        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshnessFileStamp,
        telemetryBegin,
        telemetryPath);

    telemetryBegin = std::chrono::steady_clock::now();
    request.freshnessInputs.push_back({
        "source-meta",
        FileStampCached(NLS::Core::Assets::GetAssetMetaPath(sourcePath), context)
    });
    RecordThumbnailRequestBuildTelemetry(
        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshnessMetaStamp,
        telemetryBegin,
        telemetryPath);
}

void AddArtifactFreshnessInputs(
    AssetThumbnailRequest& request,
    const AssetBrowserItem&,
    AssetThumbnailRequestBuildContext* context)
{
    if (request.artifactPath.empty())
        return;

    // The ArtifactDB file is shared by every asset, so its file stamp cannot be
    // part of a durable thumbnail identity. The exact manifest record detects
    // a reimport of this sub-asset, while artifact-file detects payload changes.
    request.freshnessInputs.push_back({
        "artifact-record",
        ArtifactRecordStamp(request.artifactPath)
    });
    request.freshnessInputs.push_back({
        "artifact-file",
        FileStampCached(ResolveThumbnailArtifactPath(request), context)
    });
}

bool IsFileFreshnessInputStillCurrent(
    const AssetThumbnailRequest& request,
    const AssetThumbnailFreshnessInput& input)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    g_freshnessInputCheckCountForTesting.fetch_add(1u, std::memory_order_relaxed);
#endif
    if (input.name == "source-file")
        return input.stamp == FileStamp(ResolveThumbnailSourcePath(request));
    if (input.name == "source-meta")
    {
        const auto sourcePath = ResolveThumbnailSourcePath(request);
        if (sourcePath.empty())
            return input.stamp == "missing";
        return input.stamp == FileStamp(NLS::Core::Assets::GetAssetMetaPath(sourcePath));
    }
    if (input.name == "artifact-file")
        return input.stamp == FileStamp(ResolveThumbnailArtifactPath(request));
    if (input.name == "artifact-record")
    {
        std::optional<NLS::Core::Assets::ArtifactManifest> manifest;
        if (request.assetDatabaseSnapshot != nullptr)
        {
            manifest = request.assetDatabaseSnapshot->GetArtifactManifestForAssetPath(
                request.sourceAssetPath);
        }
        if (!manifest.has_value())
        {
            manifest = LoadArtifactManifestFromProjectArtifactDB(
                request.projectRoot,
                request.assetId);
        }
        if (!manifest.has_value())
        {
            // Import commits can briefly make the shared database unavailable.
            // The request still names a contained, content-addressed artifact;
            // keep its result publishable and let a later readable manifest
            // perform the exact record comparison.
            return input.stamp == ArtifactRecordStamp(request.artifactPath);
        }

        const auto* artifact = request.subAssetKey.empty()
            ? manifest->FindPrimaryArtifact()
            : manifest->FindSubAsset(request.subAssetKey);
        return artifact != nullptr &&
            input.stamp == ArtifactRecordStamp(artifact->artifactPath);
    }
    if (input.name == "artifact-db")
        return input.stamp == FileStamp(GetProjectArtifactDatabasePath(request.projectRoot) / "data.mdb");
    return true;
}

bool IsThumbnailRequestStillFresh(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation* evaluation = nullptr)
{
    if (evaluation != nullptr && evaluation->freshnessCurrent.has_value())
        return *evaluation->freshnessCurrent;

    for (const auto& input : request.freshnessInputs)
    {
        if (!IsFileFreshnessInputStillCurrent(request, input))
            return false;
    }
    return true;
}

void AttachRetainedThumbnailImage(
    const AssetThumbnailRequest& request,
    const std::string& currentCacheKey,
    AssetThumbnailServiceResult& result)
{
    const auto presentation = ReadAssetThumbnailPresentationIndex(request);
    if (!presentation.has_value())
        return;

    const auto TryRetain = [&](const std::optional<AssetThumbnailPresentationIndexEntry>& entry)
    {
        if (!entry.has_value() || entry->cacheKey == currentCacheKey)
            return false;

        // The index reader validates the path, but the file can be removed by
        // cache pruning between that read and this result being consumed.
        // Never publish a retained path that is no longer drawable.
        std::error_code error;
        if (!std::filesystem::is_regular_file(entry->imagePath, error) || error)
            return false;

        result.retainedImage = ThumbnailRetainedImage {
            entry->imagePath,
            entry->cacheKey,
            entry->requestRevision
        };
        result.refreshPending = true;
        return true;
    };

    if (TryRetain(presentation->current) || TryRetain(presentation->previous))
    {
        result.refreshPending = true;
    }
}

void SynchronizeThumbnailResultPresentationState(AssetThumbnailServiceResult& result)
{
    // Service status is mutable while a lookup is being completed. Keep the
    // display state derived from the final status and retained/direct image so
    // callers cannot publish Pending with the default Fallback state.
    switch (result.status)
    {
    case AssetThumbnailServiceStatus::Fresh:
        result.presentationState = ThumbnailPresentationState::Ready;
        result.previewQuality = ThumbnailPreviewQuality::Canonical;
        result.refreshPending = false;
        result.failureRetained = false;
        return;
    case AssetThumbnailServiceStatus::Pending:
        if (result.gpuTexture.IsValid() &&
            result.previewQuality == ThumbnailPreviewQuality::Canonical)
        {
            // GPU presentation is canonical even while its durable PNG is
            // still being encoded or persisted.
            result.presentationState = ThumbnailPresentationState::Ready;
        }
        else if (result.retainedImage.has_value() && result.retainedImage->IsValid())
        {
            result.presentationState = ThumbnailPresentationState::StaleRefreshing;
        }
        else
        {
            result.presentationState = ThumbnailPresentationState::Loading;
        }
        result.failureRetained = false;
        return;
    case AssetThumbnailServiceStatus::Failed:
        if (result.retainedImage.has_value() && result.retainedImage->IsValid())
        {
            result.presentationState = ThumbnailPresentationState::FailedRetained;
            result.failureRetained = true;
        }
        else
        {
            result.presentationState = ThumbnailPresentationState::Fallback;
            result.failureRetained = false;
        }
        result.refreshPending = false;
        return;
    case AssetThumbnailServiceStatus::Fallback:
        result.presentationState = ThumbnailPresentationState::Fallback;
        result.previewQuality = ThumbnailPreviewQuality::None;
        result.refreshPending = false;
        result.failureRetained = false;
        return;
    }
}

bool PromoteAssetThumbnailResultFromPresentationIndexImpl(
    const AssetThumbnailRequest& request,
    AssetThumbnailServiceResult& result)
{
    const auto presentation = ReadAssetThumbnailPresentationIndex(request);
    if (!presentation.has_value() || !presentation->current.has_value())
    {
        return false;
    }

    const auto expectedPresentationKey = BuildAssetThumbnailPresentationKey(request);
    if (!result.presentationKey.empty() &&
        result.presentationKey != expectedPresentationKey)
    {
        return false;
    }

    const auto& current = *presentation->current;
    // Presentation-index parsing can validate filesystem-backed freshness on
    // its own, but model requests also carry an opaque "item" identity input.
    // A resolved artifact can therefore have a different cache key from the
    // Pending UI request. Accept that transition only when every verifiable
    // stamp is current and the canonical revision completes the same UI request
    // (or a newer one). A zero revision has no ordering identity and must not
    // gain the equal-revision exception.
    const bool sameRequestSession =
        current.requestSessionId == result.requestSessionId;
    const bool currentVerifiableCanonical =
        current.verifiableFreshnessCurrent &&
        ((sameRequestSession &&
             current.requestRevision != 0u &&
             current.requestRevision >= result.requestRevision) ||
            (!sameRequestSession && current.hasVerifiableFreshnessInputs));
    if (!current.freshnessCurrent &&
        current.cacheKey != BuildAssetThumbnailCacheKey(request) &&
        !currentVerifiableCanonical)
    {
        return false;
    }
    result.status = AssetThumbnailServiceStatus::Fresh;
    result.presentationState = ThumbnailPresentationState::Ready;
    result.previewQuality = ThumbnailPreviewQuality::Canonical;
    result.presentationKey = expectedPresentationKey;
    if (sameRequestSession)
        result.requestRevision = (std::max)(result.requestRevision, current.requestRevision);
    result.residentPreviewPartial = false;
    result.refreshPending = false;
    result.failureRetained = false;
    result.cacheEntry = AssetThumbnailCacheEntry {
        current.cacheKey,
        current.imagePath,
        current.metadataPath
    };
    result.retainedImage.reset();
    result.retainedGpuPresentation.reset();
    result.imagePath = current.imagePath;
    result.diagnostic.clear();
    result.gpuTexture = {};
    result.gpuTextureGeneration = 0u;
    result.revokeGpuTexture = false;
    return true;
}

bool HasLiveResidentThumbnailSnapshot(const AssetThumbnailRequest& request)
{
    if (request.kind != AssetThumbnailKind::PrefabPreview ||
        !request.residentPrefabPreviewSource.has_value())
    {
        return false;
    }

    const auto& source = *request.residentPrefabPreviewSource;
    if (!source.HasIdentity())
        return false;
    if (!source.snapshot.expired())
        return true;

    const auto registry = source.registry.lock();
    return registry != nullptr &&
        !registry->FindWeakSnapshot(
            source.runtimeCacheIdentity,
            source.freshnessFingerprint).expired();
}

void InitializeThumbnailResultIdentity(
    const AssetThumbnailRequest& request,
    AssetThumbnailServiceResult& result)
{
    result.presentationKey = BuildAssetThumbnailPresentationKey(request);
    result.requestRevision = request.requestRevision;
    result.requestSessionId = request.requestSessionId;
    result.residentPreviewRequest = HasLiveResidentThumbnailSnapshot(request);
    result.residentPreviewRevision = request.residentPreviewRevision;
    result.residentPreviewPartial = request.residentPreviewPartial;
    result.sourceAssetPath = request.sourceAssetPath;
    result.subAssetKey = request.subAssetKey;
    result.artifactPath = request.artifactPath;
    result.requestKind = static_cast<uint8_t>(request.kind);
    SynchronizeThumbnailResultPresentationState(result);
}

void RefreshResidentPreviewRequestState(AssetThumbnailRequest& request)
{
    request.residentPreviewRevision = 0u;
    request.residentPreviewPartial = false;
    if (!request.residentPrefabPreviewSource.has_value())
        return;

    const auto& source = *request.residentPrefabPreviewSource;
    if (!source.HasIdentity())
        return;
    const auto registry = source.registry.lock();
    if (registry == nullptr)
        return;

    const auto state = registry->GetSnapshotState(
        source.runtimeCacheIdentity,
        source.freshnessFingerprint);
    if (!state.has_value())
        return;
    request.residentPreviewRevision = state->revision;
    request.residentPreviewPartial = !state->complete;
}

AssetThumbnailServiceResult BuildStaleThumbnailRequestResult(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation)
{
    AssetThumbnailServiceResult result;
    result.status = AssetThumbnailServiceStatus::Failed;
    InitializeThumbnailResultIdentity(request, result);
    result.cacheEntry = evaluation.entry;
    result.fallbackIcon = FallbackIconForKind(request.kind);
    if (evaluation.entry.has_value())
        result.imagePath = evaluation.entry->imagePath;
    result.diagnostic = "thumbnail-request-stale";
    AttachRetainedThumbnailImage(
        request,
        evaluation.entry.has_value() ? evaluation.entry->cacheKey : std::string {},
        result);
    SynchronizeThumbnailResultPresentationState(result);
    return result;
}

AssetThumbnailServiceResult BuildCancelledThumbnailRequestResult(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation)
{
    AssetThumbnailServiceResult result;
    result.status = AssetThumbnailServiceStatus::Failed;
    InitializeThumbnailResultIdentity(request, result);
    result.cacheEntry = evaluation.entry;
    result.fallbackIcon = FallbackIconForKind(request.kind);
    if (evaluation.entry.has_value())
        result.imagePath = evaluation.entry->imagePath;
    result.diagnostic = "thumbnail-generation-cancelled";
    return result;
}

AssetThumbnailServiceResult BuildResultFromEvaluation(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation,
    const AssetThumbnailServiceStatus status)
{
    AssetThumbnailServiceResult result;
    result.status = status;
    InitializeThumbnailResultIdentity(request, result);
    result.cacheEntry = evaluation.entry;
    result.diagnostic = evaluation.diagnostic;
    result.fallbackIcon = FallbackIconForKind(request.kind);
    if (evaluation.entry.has_value())
        result.imagePath = evaluation.entry->imagePath;
    AttachRetainedThumbnailImage(
        request,
        evaluation.entry.has_value() ? evaluation.entry->cacheKey : std::string {},
        result);
    SynchronizeThumbnailResultPresentationState(result);
    return result;
}

void CompleteImportedPrefabThumbnailContinuation(
    const AssetThumbnailRequest& request)
{
    if (!request.importedPrefabThumbnailContinuation ||
        request.projectRoot.empty() || !request.assetId.IsValid())
    {
        return;
    }

    if (request.residentPrefabPreviewSource.has_value())
    {
        if (const auto registry = request.residentPrefabPreviewSource->registry.lock();
            registry != nullptr)
        {
            registry->CompleteImportedPrefabThumbnailContinuation(
                request.projectRoot,
                request.assetId);
            return;
        }
    }
}

bool PromoteFreshCanonicalThumbnailIfAvailable(
    const AssetThumbnailRequest& request,
    AssetThumbnailCacheEvaluation& evaluation)
{
    if (!evaluation.entry.has_value() ||
        (evaluation.status != AssetThumbnailCacheStatus::Fresh &&
            evaluation.status != AssetThumbnailCacheStatus::Failed))
    {
        return false;
    }

    // A failure marker is deliberately independent from the canonical image.
    // When a timeout races a worker that already committed Fresh, the marker
    // can be newer than the canonical metadata and make the ordinary evaluator
    // report Failed. The presentation index is the authoritative proof that
    // this exact image passed the canonical metadata/PNG validation.
    const auto presentation = ReadAssetThumbnailPresentationIndex(request);
    if (!presentation.has_value() || !presentation->current.has_value() ||
        presentation->current->cacheKey != evaluation.entry->cacheKey ||
        (presentation->current->requestSessionId == request.requestSessionId &&
            request.requestRevision != 0u &&
            presentation->current->requestRevision != 0u &&
            presentation->current->requestRevision < request.requestRevision) ||
        !IsThumbnailRequestStillFresh(request))
    {
        return false;
    }

    if (evaluation.status == AssetThumbnailCacheStatus::Failed)
    {
        // Remove a stale timeout marker so the recovery survives the next
        // lookup or editor restart. This also revalidates the PNG/hash before
        // replacing the marker and atomically recommits the presentation.
        if (!WriteAssetThumbnailCacheMetadata(
                request,
                *evaluation.entry,
                AssetThumbnailCacheStatus::Fresh,
                {}))
        {
            return false;
        }
        evaluation.status = AssetThumbnailCacheStatus::Fresh;
        evaluation.diagnostic.clear();
        evaluation.freshnessCurrent = true;
    }
    return true;
}

std::vector<uint8_t> EncodeThumbnailPng(const DownsampledThumbnail& thumbnail)
{
    if (thumbnail.pixels.empty() || thumbnail.width == 0u || thumbnail.height == 0u)
        return {};

    int encodedLength = 0;
    unsigned char* encoded = nls_asset_thumbnail_service_stbi_write_png_to_mem(
        const_cast<uint8_t*>(thumbnail.pixels.data()),
        static_cast<int>(thumbnail.width * 4u),
        static_cast<int>(thumbnail.width),
        static_cast<int>(thumbnail.height),
        4,
        &encodedLength);
    if (encoded == nullptr || encodedLength <= 0)
        return {};

    std::vector<uint8_t> bytes(
        encoded,
        encoded + static_cast<size_t>(encodedLength));
    std::free(encoded);
    return bytes;
}

AssetThumbnailServiceResult WriteThumbnailPngResult(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation,
    const DownsampledThumbnail& thumbnail,
    const std::string& emptyDiagnostic,
    const AssetThumbnailCancelToken& cancelToken,
    const AssetThumbnailRequest* metadataRequest = nullptr)
{
    const AssetThumbnailRequest& cacheMetadataRequest =
        metadataRequest != nullptr ? *metadataRequest : request;
    const auto presentationRevisionIsSuperseded = [&request]()
    {
        if (request.requestRevision == 0u)
            return false;

        const auto presentation = ReadAssetThumbnailPresentationIndex(request);
        return presentation.has_value() &&
            presentation->committedSessionId == request.requestSessionId &&
            presentation->committedRevision > request.requestRevision;
    };
    const auto buildAbandonedResult = [&](const bool recheckFreshness) -> std::optional<AssetThumbnailServiceResult>
    {
        if (IsThumbnailGenerationCancelled(cancelToken) || presentationRevisionIsSuperseded())
            return BuildCancelledThumbnailRequestResult(request, evaluation);
        // Every generator performs its initial freshness check before doing
        // expensive work. The pre-write gate therefore only needs to reject
        // cancellation or a revision already committed by a newer result;
        // the post-write gate is the freshness recheck that closes the race.
        if (recheckFreshness && !IsThumbnailRequestStillFresh(cacheMetadataRequest))
            return BuildStaleThumbnailRequestResult(request, evaluation);
        return std::nullopt;
    };
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    if (!evaluation.entry.has_value())
    {
        result.diagnostic = evaluation.diagnostic.empty()
            ? "thumbnail-cache-path-invalid"
            : evaluation.diagnostic;
        return result;
    }

    if (const auto abandoned = buildAbandonedResult(false); abandoned.has_value())
        return *abandoned;

    if (thumbnail.pixels.empty() || thumbnail.width == 0u || thumbnail.height == 0u)
    {
        result.diagnostic = emptyDiagnostic;
        if (const auto abandoned = buildAbandonedResult(false); abandoned.has_value())
            return *abandoned;
        WriteAssetThumbnailCacheMetadata(cacheMetadataRequest, *evaluation.entry, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    std::vector<uint8_t> encoded;
    {
        NLS::Base::Profiling::PerformanceStageScope encodeScope(
            NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
            "EncodePreview",
            CurrentThumbnailGenerationStageThread());
        encoded = EncodeThumbnailPng(thumbnail);
        encodeScope.AddCounter("encodedByteCount", encoded.size());
    }
    if (encoded.empty())
    {
        result.diagnostic = "thumbnail-cache-image-encode-failed";
        if (const auto abandoned = buildAbandonedResult(false); abandoned.has_value())
            return *abandoned;
        WriteAssetThumbnailCacheMetadata(cacheMetadataRequest, *evaluation.entry, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

    // Encoding can run long enough for a newer request or source revision to
    // arrive. Check again immediately before the atomic image replace so an
    // obsolete worker does not create an unreferenced canonical file.
    if (const auto abandoned = buildAbandonedResult(false); abandoned.has_value())
        return *abandoned;

    {
        NLS::Base::Profiling::PerformanceStageScope storeScope(
            NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
            "StorePreviewCache",
            CurrentThumbnailGenerationStageThread());
        storeScope.AddCounter("cacheWriteCount");
        storeScope.AddCounter("storedByteCount", encoded.size());
        if (!WriteAssetThumbnailCacheFile(request, evaluation.entry->imagePath, encoded))
        {
            result.diagnostic = "thumbnail-cache-image-write-failed";
            if (const auto abandoned = buildAbandonedResult(false); abandoned.has_value())
                return *abandoned;
            WriteAssetThumbnailCacheMetadata(cacheMetadataRequest, *evaluation.entry, AssetThumbnailCacheStatus::Failed, result.diagnostic);
            return result;
        }
    }

    if (const auto abandoned = buildAbandonedResult(true); abandoned.has_value())
    {
        // The image has already been atomically replaced, but the freshness
        // or revision gate rejected publication. Remove only an unreferenced
        // candidate; current/previous canonical images remain untouched.
        (void)DiscardUnreferencedAssetThumbnailCacheCandidate(
            cacheMetadataRequest,
            *evaluation.entry);
        return *abandoned;
    }

    if (!WriteAssetThumbnailCacheMetadata(cacheMetadataRequest, *evaluation.entry, AssetThumbnailCacheStatus::Fresh, {}))
    {
        result.diagnostic = "thumbnail-cache-metadata-write-failed";
        (void)DiscardUnreferencedAssetThumbnailCacheCandidate(
            cacheMetadataRequest,
            *evaluation.entry);
        return result;
    }

    result.status = AssetThumbnailServiceStatus::Fresh;
    result.presentationState = ThumbnailPresentationState::Ready;
    result.previewQuality = ThumbnailPreviewQuality::Canonical;
    result.refreshPending = false;
    result.failureRetained = false;
    result.imagePath = evaluation.entry->imagePath;
    result.diagnostic.clear();
    CompleteImportedPrefabThumbnailContinuation(request);
    return result;
}

bool WriteThumbnailMetadataForEvaluation(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation,
    const AssetThumbnailCacheStatus status,
    const std::string& diagnostic,
    const AssetThumbnailRequest* metadataRequest = nullptr)
{
    if (evaluation.entry.has_value())
    {
        return WriteAssetThumbnailCacheMetadata(
            metadataRequest != nullptr ? *metadataRequest : request,
            *evaluation.entry,
            status,
            diagnostic);
    }

    return WriteAssetThumbnailCacheMetadata(request, status, diagnostic);
}

AssetThumbnailServiceResult WriteRgbaThumbnailResult(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation,
    const uint8_t* pixels,
    const uint32_t width,
    const uint32_t height,
    const std::string& emptyDiagnostic,
    const AssetThumbnailCancelToken& cancelToken,
    const AssetThumbnailRequest* metadataRequest = nullptr)
{
    DownsampledThumbnail thumbnail;
    if (pixels != nullptr && width > 0u && height > 0u)
    {
        thumbnail.pixels.assign(
            pixels,
            pixels + static_cast<size_t>(width) * height * 4u);
        thumbnail.width = width;
        thumbnail.height = height;
    }
    return WriteThumbnailPngResult(
        request,
        evaluation,
        thumbnail,
        emptyDiagnostic,
        cancelToken,
        metadataRequest);
}

std::vector<uint8_t> ReadAllBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
}

uint64_t FileSizeOrMax(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? std::numeric_limits<uint64_t>::max() : static_cast<uint64_t>(size);
}

bool HasNativeArtifactMagic(const std::filesystem::path& path)
{
    std::array<uint8_t, 4u> bytes {};
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;

    input.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
        return false;

    return bytes[0] == 'N' &&
        bytes[1] == 'L' &&
        bytes[2] == 'S' &&
        bytes[3] == 'A';
}

std::optional<NLS::Core::Assets::NativeArtifactPayloadPrefix> ReadStrictStructurePreviewPrefix(
    const std::filesystem::path& path,
    const NLS::Core::Assets::ArtifactType artifactType,
    const uint32_t schemaVersion)
{
    const auto prefix = NLS::Core::Assets::ReadNativeArtifactPayloadPrefixFromFile(
        path,
        artifactType,
        schemaVersion,
        1u,
        kMaxStructurePreviewArtifactPayloadBytes);
    if (!prefix.has_value())
        return std::nullopt;

    const auto fileSize = FileSizeOrMax(path);
    if (prefix->payloadOffset > fileSize ||
        prefix->payloadSize > fileSize - prefix->payloadOffset ||
        prefix->payloadOffset + prefix->payloadSize != fileSize)
    {
        return std::nullopt;
    }
    return prefix;
}

std::optional<std::string> ReadNativeOrPlainTextArtifact(
    const std::filesystem::path& path,
    const NLS::Core::Assets::ArtifactType artifactType,
    const uint32_t schemaVersion)
{
    if (FileSizeOrMax(path) > kMaxStructurePreviewArtifactPayloadBytes)
        return std::nullopt;

    const auto bytes = ReadAllBytes(path);
    if (bytes.empty())
        return std::nullopt;

    if (NLS::Core::Assets::IsNativeArtifactContainer(bytes))
    {
        const auto container = NLS::Core::Assets::ReadNativeArtifactContainer(
            bytes,
            artifactType,
            schemaVersion);
        if (!container.has_value())
            return std::nullopt;

        return std::string(
            container->payload.begin(),
            container->payload.end());
    }
    return std::string(bytes.begin(), bytes.end());
}

bool StructurePreviewArtifactExceedsBudget(
    const std::filesystem::path& path,
    const NLS::Core::Assets::ArtifactType artifactType,
    const uint32_t schemaVersion)
{
    const auto prefix = ReadStrictStructurePreviewPrefix(path, artifactType, schemaVersion);
    if (prefix.has_value())
        return prefix->payloadSize > kMaxStructurePreviewArtifactPayloadBytes;

    return HasNativeArtifactMagic(path) ||
        FileSizeOrMax(path) > kMaxStructurePreviewArtifactPayloadBytes;
}

bool NativeArtifactFileExceedsThumbnailPreviewBudget(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return !error && size > kMaxThumbnailPreviewNativeArtifactFileBytes;
}

bool MeshArtifactFileExceedsThumbnailPreviewBudget(
    const std::filesystem::path& path,
    const NLS::Render::Assets::MeshArtifactHeaderPreview& header)
{
    (void)header;
    return NativeArtifactFileExceedsThumbnailPreviewBudget(path);
}

std::optional<std::filesystem::path> ResolveArtifactPathForPreview(
    const AssetThumbnailRequest& request,
    const std::string& artifactPath)
{
    if (artifactPath.empty())
        return std::nullopt;

    auto copy = request;
    copy.artifactPath = artifactPath;
    const auto resolved = ResolveThumbnailArtifactPath(copy);
    if (resolved.empty())
        return std::nullopt;
    return resolved;
}

std::optional<std::filesystem::path> ResolveSourceMaterialPathForPreview(
    const AssetThumbnailRequest& request,
    const std::string& artifactPath)
{
    if (artifactPath.empty())
        return std::nullopt;

    const auto rawPath = std::filesystem::path(artifactPath).lexically_normal();
    if (rawPath.extension() != ".mat")
        return std::nullopt;

    const auto assetsRoot = NLS::Core::Assets::NormalizeAssetPath(request.projectRoot / "Assets");
    if (assetsRoot.empty())
        return std::nullopt;

    const auto candidate = rawPath.is_absolute()
        ? rawPath
        : request.projectRoot / rawPath;
    const auto normalized = NLS::Core::Assets::NormalizeAssetPath(candidate);
    if (normalized.empty() ||
        !IsPhysicalRegularFileInsideEditorAssetRoot(normalized, assetsRoot))
    {
        return std::nullopt;
    }
    return normalized;
}

std::optional<std::filesystem::path> ResolveSourceMaterialPathForPreview(
    const AssetThumbnailRequest& request)
{
    if (auto resolved = ResolveSourceMaterialPathForPreview(request, request.artifactPath);
        resolved.has_value())
    {
        return resolved;
    }
    return ResolveSourceMaterialPathForPreview(request, request.sourceAssetPath);
}

bool IsGpuPreviewClearFrame(
    const std::vector<uint8_t>& rgbaPixels,
    const uint32_t width,
    const uint32_t height,
    const bool keepSubmittedDarkFrame = false)
{
    const size_t pixelCount = static_cast<size_t>(width) * height;
    if (pixelCount == 0u || rgbaPixels.size() < pixelCount * 4u)
        return true;

    size_t visiblePixelCount = 0u;
    size_t litPixelCount = 0u;
    uint8_t maxLuma = 0u;
    for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
    {
        const auto offset = pixel * 4u;
        const uint8_t a = rgbaPixels[offset + 3u];
        if (a <= kGpuPreviewVisibleAlphaThreshold)
            continue;

        ++visiblePixelCount;
        const uint8_t r = rgbaPixels[offset + 0u];
        const uint8_t g = rgbaPixels[offset + 1u];
        const uint8_t b = rgbaPixels[offset + 2u];
        const auto luma = static_cast<uint8_t>(
            (static_cast<uint16_t>(r) * 77u +
                static_cast<uint16_t>(g) * 150u +
                static_cast<uint16_t>(b) * 29u) >> 8u);
        maxLuma = (std::max)(maxLuma, luma);
        if (luma > kGpuPreviewLitLumaThreshold)
            ++litPixelCount;
    }
    if (visiblePixelCount == 0u)
        return true;
    if (keepSubmittedDarkFrame)
        return false;

    const auto minimumLitPixels = (std::max<size_t>)(1u, pixelCount / 512u);
    return maxLuma <= kGpuPreviewLitLumaThreshold || litPixelCount < minimumLitPixels;
}

bool IsGpuPreviewFullyTransparentFrame(
    const std::vector<uint8_t>& rgbaPixels,
    const uint32_t width,
    const uint32_t height)
{
    const size_t pixelCount = static_cast<size_t>(width) * height;
    if (pixelCount == 0u || rgbaPixels.size() < pixelCount * 4u)
        return true;

    for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
    {
        if (rgbaPixels[pixel * 4u + 3u] > kGpuPreviewVisibleAlphaThreshold)
            return false;
    }
    return true;
}

enum class GpuPreviewClearFrameDisposition
{
    KeepPreview,
    DeferEmptyFrame,
    FailEmptyFrame
};

GpuPreviewClearFrameDisposition EvaluateGpuPreviewClearFrameDisposition(
    const AssetThumbnailRequest& request,
    const std::vector<uint8_t>& rgbaPixels,
    const uint32_t width,
    const uint32_t height,
    const uint64_t submittedSceneDrawCount = 0u)
{
    const bool keepSubmittedDarkPrefabFrame =
        request.kind == AssetThumbnailKind::PrefabPreview &&
        submittedSceneDrawCount > 0u;
    if (!IsGpuPreviewClearFrame(rgbaPixels, width, height, keepSubmittedDarkPrefabFrame))
        return GpuPreviewClearFrameDisposition::KeepPreview;

    if (IsGpuPreviewFullyTransparentFrame(rgbaPixels, width, height))
        return GpuPreviewClearFrameDisposition::FailEmptyFrame;

    if (request.kind == AssetThumbnailKind::PrefabPreview ||
        request.kind == AssetThumbnailKind::ModelPreview)
        return GpuPreviewClearFrameDisposition::DeferEmptyFrame;

    return GpuPreviewClearFrameDisposition::FailEmptyFrame;
}

bool PreviewArtifactPathResolvesForRequest(
    const AssetThumbnailRequest& request,
    const std::string& artifactPath)
{
    if (ResolveArtifactPathForPreview(request, artifactPath).has_value())
        return true;

    return request.kind == AssetThumbnailKind::MaterialSphere &&
        ResolveSourceMaterialPathForPreview(request, artifactPath).has_value();
}

std::optional<NLS::Core::Assets::ArtifactManifest> LoadThumbnailArtifactManifest(
    const AssetThumbnailRequest& request)
{
    if (!request.assetId.IsValid())
        return std::nullopt;

    // Avoid entering the ArtifactDB loader for projects that have not imported
    // this asset yet. Besides removing a mutex/cache lookup on the resident
    // fast path, this keeps a missing database distinguishable from a real
    // manifest lookup in thumbnail telemetry.
    const auto databasePath = GetProjectArtifactDatabasePath(request.projectRoot);
    if (GetArtifactDatabaseDataFileStamp(databasePath).empty())
        return std::nullopt;

    const auto manifest = LoadArtifactManifestFromProjectArtifactDB(request.projectRoot, request.assetId);
#if defined(NLS_ENABLE_TEST_HOOKS)
    ++g_thumbnailManifestLookupCountForTesting;
    if (CurrentThumbnailGenerationStageThread() == PerformanceStageThread::Main)
        ++g_thumbnailManifestMainThreadLookupCountForTesting;
    else
        ++g_thumbnailManifestBackgroundThreadLookupCountForTesting;
#endif
    return manifest;
}

bool ThumbnailArtifactManifestExceedsPreviewBudget(const AssetThumbnailRequest& request)
{
    (void)request;
    return false;
}

std::string GpuPreviewArtifactPathInvalidDiagnostic(const AssetThumbnailKind kind)
{
    switch (kind)
    {
    case AssetThumbnailKind::MaterialSphere:
        return "thumbnail-material-artifact-path-invalid";
    case AssetThumbnailKind::PrefabPreview:
        return "thumbnail-prefab-artifact-path-invalid";
    case AssetThumbnailKind::ModelPreview:
        return "thumbnail-model-mesh-artifact-path-invalid";
    default:
        return "thumbnail-artifact-path-invalid";
    }
}

std::optional<std::string> ValidateGpuPreviewRequestArtifactPaths(
    const AssetThumbnailRequest& request,
    const bool manifestLookupCompleted)
{
    if (!SupportsGpuThumbnailPreview(request))
        return std::nullopt;

    if (!request.artifactPath.empty())
    {
        if (ResolveArtifactPathForPreview(request, request.artifactPath).has_value() ||
            (request.kind == AssetThumbnailKind::MaterialSphere &&
                ResolveSourceMaterialPathForPreview(request, request.artifactPath).has_value()))
        {
            return std::nullopt;
        }
        return GpuPreviewArtifactPathInvalidDiagnostic(request.kind);
    }

    if (request.kind == AssetThumbnailKind::MaterialSphere &&
        request.artifactPath.empty() &&
        request.generatedSubAsset)
    {
        return "thumbnail-material-artifact-missing";
    }

    if (manifestLookupCompleted)
        return std::nullopt;

    const auto manifest = LoadThumbnailArtifactManifest(request);
    if (!manifest.has_value())
        return std::nullopt;

    auto validateArtifactPath = [&request](const NLS::Core::Assets::ImportedArtifact& artifact)
        -> std::optional<std::string>
    {
        if (artifact.artifactPath.empty() ||
            PreviewArtifactPathResolvesForRequest(request, artifact.artifactPath))
        {
            return std::nullopt;
        }
        return GpuPreviewArtifactPathInvalidDiagnostic(request.kind);
    };

    if (!request.subAssetKey.empty())
    {
        const auto* artifact = manifest->FindSubAsset(request.subAssetKey);
        if (artifact == nullptr)
            return std::nullopt;

        const bool matchesRequest =
            (request.kind == AssetThumbnailKind::MaterialSphere &&
                artifact->artifactType == NLS::Core::Assets::ArtifactType::Material) ||
            (request.kind == AssetThumbnailKind::PrefabPreview &&
                artifact->artifactType == NLS::Core::Assets::ArtifactType::Prefab) ||
            (request.kind == AssetThumbnailKind::ModelPreview &&
                artifact->artifactType == NLS::Core::Assets::ArtifactType::Mesh);
        if (!matchesRequest)
            return std::nullopt;
        return validateArtifactPath(*artifact);
    }

    for (const auto& artifact : manifest->subAssets)
    {
        const bool relevant =
            (request.kind == AssetThumbnailKind::MaterialSphere &&
                artifact.artifactType == NLS::Core::Assets::ArtifactType::Material) ||
            (request.kind == AssetThumbnailKind::PrefabPreview &&
                artifact.artifactType == NLS::Core::Assets::ArtifactType::Prefab) ||
            (request.kind == AssetThumbnailKind::ModelPreview &&
                artifact.artifactType == NLS::Core::Assets::ArtifactType::Mesh);
        if (!relevant || artifact.artifactPath.empty())
            continue;
        if (auto diagnostic = validateArtifactPath(artifact);
            diagnostic.has_value())
        {
            return diagnostic;
        }
    }
    return std::nullopt;
}

bool ShouldPrioritizeThumbnailRequest(const AssetThumbnailRequest& request)
{
    return request.kind == AssetThumbnailKind::PrefabPreview;
}

bool IsVisibleResidentThumbnailRequest(const AssetThumbnailRequest& request)
{
    return request.priority == ThumbnailRequestPriority::Visible &&
        HasLiveResidentThumbnailSnapshot(request);
}

bool IsCompleteResidentThumbnailRequest(const AssetThumbnailRequest& request)
{
    if (!request.residentPrefabPreviewSource.has_value() ||
        !request.residentPrefabPreviewSource->HasIdentity())
    {
        return false;
    }
    const auto registry = request.residentPrefabPreviewSource->registry.lock();
    if (registry == nullptr)
        return false;
    const auto state = registry->GetSnapshotState(
        request.residentPrefabPreviewSource->runtimeCacheIdentity,
        request.residentPrefabPreviewSource->freshnessFingerprint);
    return state.has_value() && state->complete;
}

uint32_t ThumbnailRequestPriorityRank(const ThumbnailRequestPriority priority)
{
    switch (priority)
    {
    case ThumbnailRequestPriority::Visible:
        return 3u;
    case ThumbnailRequestPriority::Inspector:
        return 2u;
    case ThumbnailRequestPriority::Prefetch:
        return 1u;
    case ThumbnailRequestPriority::Background:
        return 0u;
    }
    return 0u;
}

bool ShouldPromoteQueuedThumbnailRequest(
    const AssetThumbnailRequest& current,
    const AssetThumbnailRequest& incoming)
{
    return ThumbnailRequestPriorityRank(incoming.priority) >
        ThumbnailRequestPriorityRank(current.priority);
}

bool ResidentThumbnailLaneEligibilityChanged(
    const AssetThumbnailRequest& current,
    const AssetThumbnailRequest& incoming)
{
    return IsVisibleResidentThumbnailRequest(current) !=
        IsVisibleResidentThumbnailRequest(incoming);
}

std::optional<std::string> PopNextQueuedCacheKeyFrom(
    std::deque<std::string>& queue,
    size_t& priorityThumbnailDequeueStreak,
    const bool countTowardsPriorityBurst)
{
    if (queue.empty())
        return std::nullopt;

    auto cacheKey = queue.front();
    queue.pop_front();
    if (countTowardsPriorityBurst)
        ++priorityThumbnailDequeueStreak;
    return cacheKey;
}

void RestoreDeferredCacheKeyToFront(
    std::deque<std::string>& queue,
    const std::string& cacheKey)
{
    queue.erase(
        std::remove(queue.begin(), queue.end(), cacheKey),
        queue.end());
    queue.push_front(cacheKey);
}

void ConsumeThumbnailCacheWriteBudgetForFreshResult(
    ThumbnailGenerationBudget& budget,
    const bool consumeBudget)
{
    if (!consumeBudget)
        return;
    if (budget.cacheWriteCountBudget > 0u && budget.cacheWriteCountBudget != SIZE_MAX)
        --budget.cacheWriteCountBudget;
}

void ConsumeThumbnailCountBudget(
    size_t& budget,
    const bool consumeBudget)
{
    if (!consumeBudget)
        return;
    if (budget > 0u && budget != SIZE_MAX)
        --budget;
}

size_t EstimateThumbnailCpuPreparationBytes(const AssetThumbnailRequest& request)
{
    size_t bytes = request.sourceAssetPath.size() + request.artifactPath.size() + request.subAssetKey.size();
    bytes += request.settingsFingerprint.size();
    for (const auto& input : request.freshnessInputs)
        bytes += input.name.size() + input.stamp.size();
    return (std::max)(bytes, static_cast<size_t>(1u));
}

size_t EstimateThumbnailGpuUploadBytes(
    const AssetThumbnailRequest& request,
    const uint32_t width = 0u,
    const uint32_t height = 0u)
{
    if (width > 0u && height > 0u)
        return static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    const auto requestedSize = (std::max)(request.requestedSize, 1u);
    return static_cast<size_t>(requestedSize) * static_cast<size_t>(requestedSize) * 4u;
}

bool HasThumbnailBudget(
    const size_t budget,
    const size_t requiredBytes)
{
    return budget == SIZE_MAX || requiredBytes <= budget;
}

void ConsumeThumbnailByteBudget(
    size_t& budget,
    const size_t bytes,
    const bool consumeBudget)
{
    if (!consumeBudget || budget == SIZE_MAX)
        return;
    budget = bytes < budget ? budget - bytes : 0u;
}

std::string BuildThumbnailDependencyStamp(
    const std::vector<AssetThumbnailFreshnessInput>& freshnessInputs)
{
    if (freshnessInputs.empty())
        return {};

    auto sortedInputs = freshnessInputs;
    std::sort(
        sortedInputs.begin(),
        sortedInputs.end(),
        [](const AssetThumbnailFreshnessInput& left, const AssetThumbnailFreshnessInput& right)
        {
            if (left.name != right.name)
                return left.name < right.name;
            return left.stamp < right.stamp;
        });

    std::string stamp;
    for (const auto& input : sortedInputs)
    {
        stamp += input.name;
        stamp += '=';
        stamp += input.stamp;
        stamp += ';';
    }
    return stamp;
}

std::string BuildThumbnailCoalescingFreshnessKey(
    const AssetThumbnailRequest& request)
{
    // Deferred manifest resolution adds artifact-file to the request. That is
    // a cache freshness input, but not a new visual request: the unresolved
    // request and its resolved artifact must share one preparation/readback
    // owner while the source, metadata and artifact database stamps match.
    std::vector<AssetThumbnailFreshnessInput> inputs;
    inputs.reserve(request.freshnessInputs.size());
    for (const auto& input : request.freshnessInputs)
    {
        if (input.name != "artifact-file")
            inputs.push_back(input);
    }
    return BuildThumbnailDependencyStamp(inputs);
}

bool AreThumbnailRequestsCoalescible(
    const AssetThumbnailRequest& left,
    const AssetThumbnailRequest& right)
{
    if (BuildAssetThumbnailPresentationKey(left) !=
        BuildAssetThumbnailPresentationKey(right))
    {
        return false;
    }
    if (BuildThumbnailCoalescingFreshnessKey(left) !=
        BuildThumbnailCoalescingFreshnessKey(right))
    {
        return false;
    }

    // Empty artifact paths are expected before the background manifest lookup
    // completes. If both sides already name an artifact, only merge exact
    // physical identities; equal freshness alone is not enough to merge two
    // different imported assets.
    if (!left.artifactPath.empty() && !right.artifactPath.empty())
    {
        const auto leftResolved = ResolveThumbnailArtifactPath(left);
        const auto rightResolved = ResolveThumbnailArtifactPath(right);
        if (!leftResolved.empty() && !rightResolved.empty())
            return leftResolved == rightResolved;
        return NormalizeEditorAssetPath(left.artifactPath) ==
            NormalizeEditorAssetPath(right.artifactPath);
    }
    return true;
}

bool HasThumbnailFreshnessInput(
    const AssetThumbnailRequest& request,
    const std::string& name)
{
    return std::any_of(
        request.freshnessInputs.begin(),
        request.freshnessInputs.end(),
        [&name](const AssetThumbnailFreshnessInput& input)
        {
            return input.name == name;
        });
}

void RemoveThumbnailFreshnessInputs(
    AssetThumbnailRequest& request,
    const std::string& name)
{
    request.freshnessInputs.erase(
        std::remove_if(
            request.freshnessInputs.begin(),
            request.freshnessInputs.end(),
            [&name](const AssetThumbnailFreshnessInput& input)
            {
                return input.name == name;
            }),
        request.freshnessInputs.end());
}

AssetThumbnailRequest BuildResolvedThumbnailCacheRequest(
    const AssetThumbnailRequest& request,
    const AssetThumbnailRequest& previewRequest)
{
    if (!request.artifactPath.empty() || previewRequest.artifactPath.empty())
        return request;

    auto cacheRequest = previewRequest;
    cacheRequest.priority = request.priority;
    cacheRequest.freshnessInputs = request.freshnessInputs;
    RemoveThumbnailFreshnessInputs(cacheRequest, "artifact-db");
    RemoveThumbnailFreshnessInputs(cacheRequest, "artifact-file");
    RemoveThumbnailFreshnessInputs(cacheRequest, "artifact-record");
    cacheRequest.freshnessInputs.push_back({
        "artifact-record",
        ArtifactRecordStamp(cacheRequest.artifactPath)
    });
    cacheRequest.freshnessInputs.push_back({
        "artifact-file",
        FileStamp(ResolveThumbnailArtifactPath(cacheRequest))
    });
    cacheRequest.dependencyStamp = BuildThumbnailDependencyStamp(cacheRequest.freshnessInputs);
    return cacheRequest;
}

AssetThumbnailServiceResult BuildGpuPreviewEmptyFrameResult(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation,
    const AssetThumbnailRequest& previewRequest)
{
    auto result = BuildResultFromEvaluation(
        request,
        evaluation,
        AssetThumbnailServiceStatus::Failed);
    result.diagnostic = "thumbnail-gpu-preview-empty-frame";
    const auto metadataRequest = BuildResolvedThumbnailCacheRequest(request, previewRequest);
    WriteThumbnailMetadataForEvaluation(
        request,
        evaluation,
        AssetThumbnailCacheStatus::Failed,
        result.diagnostic,
        &metadataRequest);
    return result;
}

std::optional<AssetThumbnailServiceResult> TryGeneratePrefabSnapshotThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailRequest& previewRequest,
    const PreviewRenderableSnapshot& snapshot,
    const AssetThumbnailCacheEvaluation& evaluation,
    const AssetThumbnailCancelToken& cancelToken);

AssetThumbnailServiceResult GenerateCanonicalCpuPrefabFallback(
    const AssetThumbnailRequest& request,
    const AssetThumbnailRequest& previewRequest,
    const std::shared_ptr<const PreviewRenderableSnapshot>& preparedSnapshot,
    const AssetThumbnailCancelToken& cancelToken)
{
    const auto evaluation = EvaluateAssetThumbnailCache(request);
    if (!IsThumbnailRequestStillFresh(request, &evaluation))
        return BuildStaleThumbnailRequestResult(request, evaluation);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);

    // A GPU empty-frame validation failure happens after the renderer has
    // already prepared the canonical Prefab graph. Reuse that immutable
    // snapshot for CPU persistence; reopening the Prefab artifact here can
    // repeat a large deserialize/import even though the GPU path just did it.
    if (request.kind == AssetThumbnailKind::PrefabPreview &&
        preparedSnapshot != nullptr &&
        !preparedSnapshot->drawItems.empty() &&
        preparedSnapshot->expectedDrawItemCount != 0u &&
        preparedSnapshot->expectedDrawItemCount == preparedSnapshot->drawItems.size())
    {
        if (auto snapshotResult = TryGeneratePrefabSnapshotThumbnail(
                request,
                previewRequest,
                *preparedSnapshot,
                evaluation,
                cancelToken);
            snapshotResult.has_value())
        {
            auto result = std::move(*snapshotResult);
            result.previewQuality = ThumbnailPreviewQuality::PreparedCache;
            if (result.status == AssetThumbnailServiceStatus::Fresh)
            {
                result.diagnostic = "thumbnail-gpu-preview-empty-frame-cpu-fallback-prepared";
                return result;
            }

            if (IsThumbnailGenerationCancelled(cancelToken))
                return result;

            if (!IsThumbnailRequestStillFresh(request) || !evaluation.entry.has_value())
                return result;
            (void)ClearAssetThumbnailCacheFailureMetadata(request, *evaluation.entry);
            result.diagnostic = result.diagnostic.empty()
                ? "thumbnail-gpu-preview-empty-frame-cpu-fallback-prepared-failed"
                : "thumbnail-gpu-preview-empty-frame-cpu-fallback-prepared-failed:" +
                    result.diagnostic;
            return result;
        }
    }

    auto result = GeneratePrefabThumbnail(request, evaluation, cancelToken);
    if (result.status == AssetThumbnailServiceStatus::Fresh)
    {
        result.diagnostic = "thumbnail-gpu-preview-empty-frame-cpu-fallback";
        return result;
    }

    // GPU empty-frame failures are transient validation failures. Do not turn
    // a failed CPU fallback into a durable negative cache entry: the retained
    // canonical image remains eligible on the next lookup and a newer request
    // can retry after resources or the renderer recover.
    if (!IsThumbnailGenerationCancelled(cancelToken) &&
        IsThumbnailRequestStillFresh(request) &&
        evaluation.entry.has_value())
    {
        (void)ClearAssetThumbnailCacheFailureMetadata(
            request,
            *evaluation.entry);
    }
    if (!result.diagnostic.empty())
    {
        result.diagnostic =
            "thumbnail-gpu-preview-empty-frame-cpu-fallback-failed:" +
            result.diagnostic;
    }
    else
    {
        result.diagnostic =
            "thumbnail-gpu-preview-empty-frame-cpu-fallback-failed";
    }
    return result;
}

const std::optional<NLS::Core::Assets::ArtifactManifest>* LoadThumbnailArtifactManifestCached(
    const AssetThumbnailRequest& request,
    AssetThumbnailRequestBuildContext* context)
{
    if (context == nullptr)
        return nullptr;

    const auto key = request.assetId.ToString();
    auto [iterator, inserted] = context->artifactManifestsByAssetId.emplace(
        key,
        std::optional<NLS::Core::Assets::ArtifactManifest> {});
    if (inserted)
        iterator->second = LoadThumbnailArtifactManifest(request);
    return &iterator->second;
}

const NLS::Core::Assets::ImportedArtifact* FindThumbnailArtifactForItem(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const AssetBrowserItem& item)
{
    if (!item.subAssetKey.empty())
    {
        if (const auto* subAsset = manifest.FindSubAsset(item.subAssetKey))
            return subAsset;
    }

    const auto wantedType = item.type == AssetBrowserItemType::Prefab ||
            (item.type == AssetBrowserItemType::Model && item.kind == AssetBrowserItemKind::SourceAsset)
        ? NLS::Core::Assets::ArtifactType::Prefab
        : item.type == AssetBrowserItemType::Material
            ? NLS::Core::Assets::ArtifactType::Material
            : item.type == AssetBrowserItemType::Texture
                ? NLS::Core::Assets::ArtifactType::Texture
                : item.type == AssetBrowserItemType::Mesh || item.type == AssetBrowserItemType::Model
                    ? NLS::Core::Assets::ArtifactType::Mesh
                    : NLS::Core::Assets::ArtifactType::Unknown;

    if (const auto* primary = manifest.FindPrimaryArtifact())
    {
        if ((wantedType != NLS::Core::Assets::ArtifactType::Unknown && primary->artifactType == wantedType) ||
            (wantedType == NLS::Core::Assets::ArtifactType::Unknown &&
                (primary->artifactType == item.artifactType ||
                    item.artifactType == NLS::Core::Assets::ArtifactType::Unknown)))
        {
            return primary;
        }
    }

    if (wantedType == NLS::Core::Assets::ArtifactType::Unknown)
        return nullptr;

    for (const auto& artifact : manifest.subAssets)
    {
        if (artifact.artifactType == wantedType)
            return &artifact;
    }
    return nullptr;
}

bool HasExtension(const std::filesystem::path& path, const char* extension)
{
    return ToLowerAscii(path.extension().generic_string()) == extension;
}

bool IsNativeMeshArtifactPath(const std::filesystem::path& path)
{
    return NLS::Render::Assets::IsMeshArtifactFile(path);
}

bool IsNativeTextureArtifactPath(const std::filesystem::path& path)
{
    return NLS::Render::Assets::ReadTextureArtifactHeaderPreview(path, 64u * 1024u).has_value();
}

bool IsRgba8TextureArtifactMipUsable(const NLS::Render::Assets::TextureArtifactData& artifact)
{
    return artifact.format == NLS::Render::RHI::TextureFormat::RGBA8 &&
        SelectTextureThumbnailMip(artifact, kMaxTextureThumbnailGenerationSize) != nullptr;
}

std::string TextureSourceKeyFromSubAssetKey(const std::string& subAssetKey)
{
    constexpr std::string_view kPrefix = "texture:";
    if (subAssetKey.rfind(kPrefix, 0u) != 0u)
        return {};
    return subAssetKey.substr(kPrefix.size());
}

std::optional<std::string> TextureDependencySourcePath(
    const NLS::Core::Assets::AssetDependencyRecord& dependency,
    const std::string& textureSourceKey)
{
    if (dependency.kind != NLS::Core::Assets::AssetDependencyKind::PostprocessorVersion ||
        textureSourceKey.empty())
    {
        return std::nullopt;
    }

    const std::string expectedValue = "texture-build:texture:" + textureSourceKey;
    if (dependency.value != expectedValue)
        return std::nullopt;

    constexpr std::string_view kSourcePathToken = "sourcePath=";
    const auto sourceBegin = dependency.hashOrVersion.find(kSourcePathToken);
    if (sourceBegin == std::string::npos)
        return std::nullopt;

    const auto valueBegin = sourceBegin + kSourcePathToken.size();
    auto valueEnd = dependency.hashOrVersion.find('|', valueBegin);
    if (valueEnd == std::string::npos)
        valueEnd = dependency.hashOrVersion.size();
    if (valueEnd <= valueBegin)
        return std::nullopt;

    auto sourcePath = dependency.hashOrVersion.substr(valueBegin, valueEnd - valueBegin);
    std::replace(sourcePath.begin(), sourcePath.end(), '\\', '/');
    while (sourcePath.rfind("./", 0u) == 0u)
        sourcePath.erase(0u, 2u);
    return sourcePath.empty() ? std::nullopt : std::optional<std::string>(sourcePath);
}

std::optional<std::filesystem::path> ResolveTextureSourceDependencyPath(const AssetThumbnailRequest& request)
{
    const auto textureSourceKey = TextureSourceKeyFromSubAssetKey(request.subAssetKey);
    if (textureSourceKey.empty())
        return std::nullopt;
    const auto manifest = LoadThumbnailArtifactManifest(request);
    if (!manifest.has_value())
        return std::nullopt;

    for (const auto& dependency : manifest->dependencies)
    {
        const auto sourcePathText = TextureDependencySourcePath(dependency, textureSourceKey);
        if (!sourcePathText.has_value())
            continue;

        const auto sourcePath = std::filesystem::path(*sourcePathText).lexically_normal();
        std::vector<std::filesystem::path> candidates;
        if (sourcePath.is_absolute())
        {
            candidates.push_back(sourcePath);
        }
        else
        {
            candidates.push_back(request.projectRoot / sourcePath);
            if (!request.sourceAssetPath.empty())
            {
                candidates.push_back(
                    request.projectRoot /
                    std::filesystem::path(request.sourceAssetPath).parent_path() /
                    sourcePath);
            }
        }

        const auto assetRoots = MakeProjectEditorAssetRoots(request.projectRoot);
        for (const auto& candidate : candidates)
        {
            const auto normalized = NLS::Core::Assets::NormalizeAssetPath(candidate);
            if (normalized.empty() || !IsTextureThumbnailSourceExtension(normalized))
                continue;

            const auto editorAssetPath = ToEditorAssetPath(assetRoots, normalized);
            if (!ResolveEditorAssetPath(assetRoots, editorAssetPath).empty())
                return normalized;
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> ResolveTextureSourceDependencyPathForKey(
    const AssetThumbnailRequest& request,
    const std::string& textureSourceKey)
{
    if (textureSourceKey.empty())
        return std::nullopt;

    AssetThumbnailRequest textureRequest = request;
    textureRequest.subAssetKey = "texture:" + textureSourceKey;
    return ResolveTextureSourceDependencyPath(textureRequest);
}

bool ShouldFlipMaterialSourceTextureVertically(const AssetThumbnailRequest& request)
{
    const auto extension = ToLowerAscii(std::filesystem::path(request.sourceAssetPath).extension().generic_string());
    return extension != ".gltf" && extension != ".glb";
}

std::vector<std::filesystem::path> ResolveMeshArtifactPaths(
    const AssetThumbnailRequest& request)
{
    std::vector<std::filesystem::path> paths;
    const auto directMeshPath = ResolveArtifactPathForPreview(request, request.artifactPath);
    const bool directPathIsMesh =
        directMeshPath.has_value() && IsNativeMeshArtifactPath(*directMeshPath);

    const auto manifest = LoadThumbnailArtifactManifest(request);
    if (!manifest.has_value())
    {
        if (directPathIsMesh)
            paths.push_back(*directMeshPath);
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

            if (auto resolved = ResolveArtifactPathForPreview(request, artifact.artifactPath);
                resolved.has_value())
            {
                paths.push_back(*resolved);
                return paths;
            }
        }

        if (directPathIsMesh)
            paths.push_back(*directMeshPath);
        return paths;
    }

    for (const auto& artifact : manifest->subAssets)
    {
        if (artifact.artifactType != NLS::Core::Assets::ArtifactType::Mesh)
            continue;

        if (auto resolved = ResolveArtifactPathForPreview(request, artifact.artifactPath);
            resolved.has_value())
        {
            paths.push_back(*resolved);
        }
    }
    if (paths.empty() && directPathIsMesh)
        paths.push_back(*directMeshPath);
    return paths;
}

std::optional<std::filesystem::path> ResolveFirstMeshArtifactPath(
    const AssetThumbnailRequest& request)
{
    const auto paths = ResolveMeshArtifactPaths(request);
    if (paths.empty())
        return std::nullopt;
    return paths.front();
}

std::optional<std::filesystem::path> ResolvePreviewArtifactOrSourcePath(
    const AssetThumbnailRequest& request)
{
    if (auto artifactPath = ResolveArtifactPathForPreview(request, request.artifactPath);
        artifactPath.has_value())
    {
        return artifactPath;
    }
    if (request.kind == AssetThumbnailKind::MaterialSphere)
    {
        if (!request.artifactPath.empty())
        {
            if (auto sourceMaterialPath = ResolveSourceMaterialPathForPreview(request, request.artifactPath);
                sourceMaterialPath.has_value())
            {
                return sourceMaterialPath;
            }
            return std::nullopt;
        }
        if (auto sourceMaterialPath = ResolveSourceMaterialPathForPreview(request, request.sourceAssetPath);
            sourceMaterialPath.has_value())
        {
            return sourceMaterialPath;
        }
    }
    if (!request.artifactPath.empty())
        return std::nullopt;

    const auto sourcePath = ResolveThumbnailSourcePath(request);
    if (sourcePath.empty())
        return std::nullopt;
    return sourcePath;
}

NLS::Core::Assets::ArtifactType ExpectedArtifactTypeForThumbnailRequest(
    const AssetThumbnailRequest& request)
{
    using NLS::Core::Assets::ArtifactType;
    switch (request.kind)
    {
    case AssetThumbnailKind::Texture:
        return ArtifactType::Texture;
    case AssetThumbnailKind::MaterialSphere:
        return ArtifactType::Material;
    case AssetThumbnailKind::ModelPreview:
        return ArtifactType::Mesh;
    case AssetThumbnailKind::PrefabPreview:
        return ArtifactType::Prefab;
    default:
        return ArtifactType::Unknown;
    }
}

bool ThumbnailArtifactMatchesRequest(
    const NLS::Core::Assets::ImportedArtifact& artifact,
    const AssetThumbnailRequest& request)
{
    const auto expectedType = ExpectedArtifactTypeForThumbnailRequest(request);
    return expectedType != NLS::Core::Assets::ArtifactType::Unknown &&
        artifact.artifactType == expectedType;
}

const NLS::Core::Assets::ImportedArtifact* FindDeferredThumbnailArtifact(
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const AssetThumbnailRequest& request)
{
    if (!request.subAssetKey.empty())
    {
        const auto* artifact = manifest.FindSubAsset(request.subAssetKey);
        if (artifact != nullptr && ThumbnailArtifactMatchesRequest(*artifact, request))
            return artifact;
        return nullptr;
    }

    const auto* primary = manifest.FindPrimaryArtifact();
    if (primary != nullptr && ThumbnailArtifactMatchesRequest(*primary, request))
        return primary;

    for (const auto& candidate : manifest.subAssets)
    {
        if (ThumbnailArtifactMatchesRequest(candidate, request))
            return &candidate;
    }
    return nullptr;
}

AssetThumbnailRequest ResolveDeferredThumbnailPreviewRequest(const AssetThumbnailRequest& request)
{
    const auto RebindResidentSource = [](AssetThumbnailRequest& resolved)
    {
        if (resolved.kind != AssetThumbnailKind::PrefabPreview ||
            resolved.artifactPath.empty() ||
            !resolved.residentPrefabPreviewSource.has_value())
        {
            return;
        }

        const auto& source = *resolved.residentPrefabPreviewSource;
        const auto registry = source.registry.lock();
        if (registry == nullptr)
            return;

        const auto canonicalSubAssetKey = BuildCanonicalPrefabPreviewSubAssetKey(
            resolved.sourceAssetPath,
            resolved.subAssetKey);
        if (canonicalSubAssetKey.empty())
            return;

        const auto& freshnessSubAssetKey = canonicalSubAssetKey;
        const auto runtimeCacheIdentity = BuildResidentPrefabRuntimeCacheIdentity(
            resolved.assetId.ToString(),
            canonicalSubAssetKey);
        const auto freshnessFingerprint = BuildPrefabThumbnailDependencyStamp(
            resolved.projectRoot,
            resolved.assetId,
            resolved.sourceAssetPath,
            freshnessSubAssetKey,
            resolved.artifactPath);
        if (freshnessFingerprint.empty())
            return;

        const auto residentState = registry->GetSnapshotState(
            runtimeCacheIdentity,
            freshnessFingerprint);

        resolved.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
            runtimeCacheIdentity,
            freshnessFingerprint,
            registry->FindWeakSnapshot(runtimeCacheIdentity, freshnessFingerprint),
            registry,
            (residentState.has_value() && residentState->allowArtifactResourceLoading) ||
                resolved.importedPrefabThumbnailContinuation
        };
    };

    // Deferred requests created before the Asset Browser manifest snapshot was
    // attached may still carry a resident identity but no artifact path. The
    // resident path does not require the Prefab payload, but the resolved path
    // is part of the canonical freshness contract and is needed if the lease
    // turns out to be stale. Prefer the immutable enumeration snapshot, then
    // fall back to the lightweight manifest cache. Neither path reads a Prefab
    // artifact; the latter is only metadata resolution.
    if (request.kind == AssetThumbnailKind::PrefabPreview &&
        request.artifactPath.empty() &&
        request.residentPrefabPreviewSource.has_value() &&
        request.residentPrefabPreviewSource->HasIdentity())
    {
        auto resolved = request;
        std::optional<NLS::Core::Assets::ArtifactManifest> localManifest;
        const NLS::Core::Assets::ArtifactManifest* manifest = nullptr;

        if (resolved.assetDatabaseSnapshot != nullptr)
        {
            localManifest = resolved.assetDatabaseSnapshot->GetArtifactManifestForAssetPath(
                resolved.sourceAssetPath);
            if (localManifest.has_value())
                manifest = &*localManifest;
        }

        if (manifest == nullptr)
        {
            localManifest = LoadThumbnailArtifactManifest(resolved);
            if (localManifest.has_value())
                manifest = &*localManifest;
        }

        if (manifest != nullptr)
        {
            if (const auto* artifact = FindDeferredThumbnailArtifact(*manifest, resolved);
                artifact != nullptr && !artifact->artifactPath.empty())
            {
                if (resolved.subAssetKey.empty())
                    resolved.subAssetKey = artifact->subAssetKey;
                resolved.artifactPath = artifact->artifactPath;
            }
        }

        if (!resolved.artifactPath.empty())
        {
            RebindResidentSource(resolved);
            return resolved;
        }

        // Keep the original resident request intact when metadata is not
        // available. Acquire will reject an incomplete/stale identity and the
        // normal deferred path can report the missing artifact cleanly.
        return request;
    }

    if (!request.artifactPath.empty())
    {
        auto resolved = request;
        RebindResidentSource(resolved);
        return resolved;
    }

    const auto manifest = LoadThumbnailArtifactManifest(request);
    if (!manifest.has_value())
        return request;

    const auto* artifact = FindDeferredThumbnailArtifact(*manifest, request);
    if (artifact == nullptr || artifact->artifactPath.empty())
        return request;

    auto resolved = request;
    if (resolved.subAssetKey.empty())
        resolved.subAssetKey = artifact->subAssetKey;
    resolved.artifactPath = artifact->artifactPath;
    RebindResidentSource(resolved);
    return resolved;
}

std::string PrefabResolvedAssetExpectedType(const NLS::Core::Assets::ArtifactType artifactType)
{
    switch (artifactType)
    {
    case NLS::Core::Assets::ArtifactType::Mesh:
        return "Mesh";
    case NLS::Core::Assets::ArtifactType::Material:
        return "Material";
    case NLS::Core::Assets::ArtifactType::Texture:
        return "Texture";
    case NLS::Core::Assets::ArtifactType::Shader:
        return "Shader";
    case NLS::Core::Assets::ArtifactType::Prefab:
        return "Prefab";
    default:
        return {};
    }
}

std::vector<NLS::Engine::Assets::PrefabResolvedAsset> BuildThumbnailPrefabResolvedAssetsFromManifest(
    const AssetThumbnailRequest& request)
{
    std::vector<NLS::Engine::Assets::PrefabResolvedAsset> resolvedAssets;
    const auto manifest = LoadThumbnailArtifactManifest(request);
    if (!manifest.has_value())
        return resolvedAssets;

    resolvedAssets.reserve(manifest->subAssets.size());
    for (const auto& artifact : manifest->subAssets)
    {
        auto expectedType = PrefabResolvedAssetExpectedType(artifact.artifactType);
        if (expectedType.empty())
            continue;

        resolvedAssets.push_back({
            artifact.sourceAssetId.IsValid() ? artifact.sourceAssetId : request.assetId,
            std::move(expectedType),
            artifact.subAssetKey,
            artifact.artifactPath
        });
    }
    return resolvedAssets;
}

std::optional<NLS::Engine::Assets::PrefabArtifact> ImportPrefabArtifactForThumbnailPreview(
    const AssetThumbnailRequest& request,
    const std::string& payload)
{
    auto imported = NLS::Engine::Assets::ImportPrefabArtifact(
        payload,
        request.assetId,
        BuildThumbnailPrefabResolvedAssetsFromManifest(request));
    if (imported.diagnostics.HasErrors())
        return std::nullopt;
    return std::move(imported.artifact);
}

std::optional<PreviewRenderableSnapshot> BuildPrefabPreviewSnapshotForThumbnail(
    const AssetThumbnailRequest& request,
    const std::string& payload)
{
    auto prefab = ImportPrefabArtifactForThumbnailPreview(request, payload);
    if (!prefab.has_value())
        return std::nullopt;

    return BuildPreviewRenderableSnapshot(*prefab);
}

std::optional<PreviewRenderableSnapshot> TryBuildSharedPrefabPreviewSnapshotForThumbnail(
    const AssetThumbnailRequest& request)
{
    if (request.projectRoot.empty() || request.sourceAssetPath.empty())
        return std::nullopt;

    const auto subAssetKey = request.subAssetKey.empty()
        ? BuildCanonicalPrefabPreviewSubAssetKey(
            request.sourceAssetPath,
            request.subAssetKey)
        : request.subAssetKey;
    if (subAssetKey.empty())
        return std::nullopt;

    // This is the same immutable graph repository used by scene restore and
    // drag/drop. It coalesces a cold load and reuses the hot/prepared result;
    // no scene instance or component pointer crosses into the thumbnail path.
    EditorAssetDragDropBridge bridge(request.projectRoot / "Assets");
    const auto prefab = bridge.TryLoadImportedPrefabArtifactShared(
        request.sourceAssetPath,
        subAssetKey);
    if (!prefab)
        return std::nullopt;

    return BuildPreviewRenderableSnapshot(*prefab);
}

std::optional<std::filesystem::path> ResolvePrefabPreviewDrawItemMeshPath(
    const AssetThumbnailRequest& request,
    const PreviewDrawItem& drawItem)
{
    if (drawItem.meshPath.empty())
        return std::nullopt;

    if (drawItem.meshPath.rfind("builtin:Primitive/", 0) == 0)
    {
        const auto resolved = NLS::Core::ResourceManagement::MeshManager::ResolveArtifactResourcePath(drawItem.meshPath);
        if (!resolved.empty())
            return std::filesystem::path(resolved);
        return std::nullopt;
    }

    if (drawItem.meshAssetId.IsValid() && drawItem.meshAssetId != request.assetId)
    {
        auto meshRequest = request;
        meshRequest.assetId = drawItem.meshAssetId;
        meshRequest.artifactPath = drawItem.meshPath;
        if (auto resolved = ResolveArtifactPathForPreview(meshRequest, drawItem.meshPath);
            resolved.has_value())
        {
            return resolved;
        }

        const auto meshManifest = LoadThumbnailArtifactManifest(meshRequest);
        if (meshManifest.has_value())
        {
            if (const auto* meshArtifact = meshManifest->FindSubAsset(drawItem.meshPath);
                meshArtifact != nullptr && meshArtifact->artifactType == NLS::Core::Assets::ArtifactType::Mesh)
            {
                meshRequest.artifactPath = meshArtifact->artifactPath;
                if (auto resolved = ResolveArtifactPathForPreview(meshRequest, meshArtifact->artifactPath);
                    resolved.has_value())
                {
                    return resolved;
                }
            }
        }
    }

    return ResolveArtifactPathForPreview(request, drawItem.meshPath);
}

struct RgbaCanvas
{
    std::vector<uint8_t> pixels;
    uint32_t width = 0u;
    uint32_t height = 0u;
};

RgbaCanvas MakeCanvas(const uint32_t requestedSize)
{
    const auto size = std::max(1u, requestedSize);
    RgbaCanvas canvas;
    canvas.width = size;
    canvas.height = size;
    canvas.pixels.assign(static_cast<size_t>(size) * size * 4u, 0u);
    return canvas;
}

void PutPixel(
    RgbaCanvas& canvas,
    const int x,
    const int y,
    const uint8_t r,
    const uint8_t g,
    const uint8_t b,
    const uint8_t a = 255u)
{
    if (x < 0 || y < 0 ||
        x >= static_cast<int>(canvas.width) ||
        y >= static_cast<int>(canvas.height))
    {
        return;
    }

    const auto index = (static_cast<size_t>(y) * canvas.width + static_cast<size_t>(x)) * 4u;
    canvas.pixels[index + 0u] = r;
    canvas.pixels[index + 1u] = g;
    canvas.pixels[index + 2u] = b;
    canvas.pixels[index + 3u] = a;
}

void DrawLine(
    RgbaCanvas& canvas,
    int x0,
    int y0,
    const int x1,
    const int y1,
    const uint8_t r,
    const uint8_t g,
    const uint8_t b)
{
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    for (;;)
    {
        PutPixel(canvas, x0, y0, r, g, b);
        PutPixel(canvas, x0 + 1, y0, r, g, b, 210u);
        PutPixel(canvas, x0, y0 + 1, r, g, b, 210u);
        if (x0 == x1 && y0 == y1)
            break;

        const int doubledError = 2 * error;
        if (doubledError >= dy)
        {
            error += dy;
            x0 += sx;
        }
        if (doubledError <= dx)
        {
            error += dx;
            y0 += sy;
        }
    }
}

void FillTriangle(
    RgbaCanvas& canvas,
    const std::array<int, 2u>& p0,
    const std::array<int, 2u>& p1,
    const std::array<int, 2u>& p2,
    const uint8_t r,
    const uint8_t g,
    const uint8_t b,
    const uint8_t a)
{
    const auto edge = [](const std::array<int, 2u>& a, const std::array<int, 2u>& b, const int x, const int y)
    {
        return (x - a[0]) * (b[1] - a[1]) - (y - a[1]) * (b[0] - a[0]);
    };

    const int minX = std::max(0, std::min({p0[0], p1[0], p2[0]}));
    const int maxX = std::min(static_cast<int>(canvas.width) - 1, std::max({p0[0], p1[0], p2[0]}));
    const int minY = std::max(0, std::min({p0[1], p1[1], p2[1]}));
    const int maxY = std::min(static_cast<int>(canvas.height) - 1, std::max({p0[1], p1[1], p2[1]}));
    if (minX > maxX || minY > maxY)
        return;

    const auto area = edge(p0, p1, p2[0], p2[1]);
    if (area == 0)
        return;

    for (int y = minY; y <= maxY; ++y)
    {
        for (int x = minX; x <= maxX; ++x)
        {
            const auto w0 = edge(p1, p2, x, y);
            const auto w1 = edge(p2, p0, x, y);
            const auto w2 = edge(p0, p1, x, y);
            const bool insidePositive = w0 >= 0 && w1 >= 0 && w2 >= 0;
            const bool insideNegative = w0 <= 0 && w1 <= 0 && w2 <= 0;
            if (insidePositive || insideNegative)
                PutPixel(canvas, x, y, r, g, b, a);
        }
    }
}

DownsampledThumbnail CanvasToThumbnail(RgbaCanvas canvas)
{
    DownsampledThumbnail thumbnail;
    thumbnail.width = canvas.width;
    thumbnail.height = canvas.height;
    thumbnail.pixels = std::move(canvas.pixels);
    return thumbnail;
}

std::optional<std::vector<float>> ParseFloatList(const std::string& value)
{
    std::istringstream stream(value);
    std::vector<float> values;
    float number = 0.0f;
    while (stream >> number)
        values.push_back(number);
    if (values.empty())
        return std::nullopt;
    return values;
}

std::optional<std::string> ExtractXmlAttribute(
    const std::string& element,
    const std::string& attribute)
{
    const auto key = attribute + "=\"";
    const auto begin = element.find(key);
    if (begin == std::string::npos)
        return std::nullopt;

    const auto valueBegin = begin + key.size();
    const auto valueEnd = element.find('"', valueBegin);
    if (valueEnd == std::string::npos)
        return std::nullopt;
    return element.substr(valueBegin, valueEnd - valueBegin);
}

std::string UnescapeXmlAttributeValue(std::string value)
{
    auto replaceAll = [&value](const std::string_view from, const std::string_view to)
    {
        size_t position = 0u;
        while ((position = value.find(from, position)) != std::string::npos)
        {
            value.replace(position, from.size(), to);
            position += to.size();
        }
    };

    replaceAll("&quot;", "\"");
    replaceAll("&apos;", "'");
    replaceAll("&lt;", "<");
    replaceAll("&gt;", ">");
    replaceAll("&amp;", "&");
    return value;
}

std::array<float, 4u> ExtractMaterialBaseColor(const std::string& xml)
{
    auto parseNamedValue = [&xml](const std::string& name) -> std::optional<std::array<float, 4u>>
    {
        size_t position = 0u;
        while ((position = xml.find("name=\"" + name + "\"", position)) != std::string::npos)
        {
            const auto elementBegin = xml.rfind('<', position);
            const auto elementEnd = xml.find('>', position);
            if (elementBegin == std::string::npos || elementEnd == std::string::npos)
            {
                position += name.size();
                continue;
            }

            const auto element = xml.substr(elementBegin, elementEnd - elementBegin + 1u);
            const auto value = ExtractXmlAttribute(element, "value");
            const auto values = value.has_value() ? ParseFloatList(*value) : std::nullopt;
            if (values.has_value())
            {
                std::array<float, 4u> color {0.75f, 0.75f, 0.75f, 1.0f};
                for (size_t index = 0u; index < color.size() && index < values->size(); ++index)
                    color[index] = std::clamp((*values)[index], 0.0f, 1.0f);
                return color;
            }
            position = elementEnd + 1u;
        }
        return std::nullopt;
    };

    if (auto uniform = parseNamedValue("u_Albedo");
        uniform.has_value())
    {
        return *uniform;
    }
    if (auto factor = parseNamedValue("BaseColor");
        factor.has_value())
    {
        return *factor;
    }
    return {0.72f, 0.74f, 0.78f, 1.0f};
}

std::optional<MaterialTextureReference> ExtractMaterialTextureReference(const std::string& xml)
{
    constexpr std::array<std::string_view, 6u> kPreferredTextureSlotNames {
        "BaseColor",
        "Albedo",
        "Diffuse",
        "baseColor",
        "albedo",
        "diffuse"
    };
    constexpr std::array<std::string_view, 8u> kTextureUniformNames {
        "u_AlbedoMap",
        "u_DiffuseMap",
        "BaseColorTexture",
        "BaseColorMap",
        "DiffuseTexture",
        "DiffuseMap",
        "AlbedoTexture",
        "AlbedoMap"
    };

    for (const auto slotName : kPreferredTextureSlotNames)
    {
        size_t position = 0u;
        const std::string needle = "name=\"" + std::string(slotName) + "\"";
        while ((position = xml.find(needle, position)) != std::string::npos)
        {
            const auto elementBegin = xml.rfind('<', position);
            const auto elementEnd = xml.find('>', position);
            if (elementBegin == std::string::npos || elementEnd == std::string::npos)
            {
                position += needle.size();
                continue;
            }

            const auto element = xml.substr(elementBegin, elementEnd - elementBegin + 1u);
            if (element.find("<textureSlot") == std::string::npos)
            {
                position = elementEnd + 1u;
                continue;
            }

            MaterialTextureReference reference;
            if (auto key = ExtractXmlAttribute(element, "texture");
                key.has_value() && !key->empty())
            {
                reference.textureKey = UnescapeXmlAttributeValue(*key);
            }
            for (const auto attribute : {"resourcePath", "texture", "value"})
            {
                if (auto value = ExtractXmlAttribute(element, attribute);
                    value.has_value() && !value->empty())
                {
                    reference.resourcePath = UnescapeXmlAttributeValue(*value);
                    return reference;
                }
            }
            position = elementEnd + 1u;
        }
    }

    for (const auto uniformName : kTextureUniformNames)
    {
        size_t position = 0u;
        const std::string needle = "name=\"" + std::string(uniformName) + "\"";
        while ((position = xml.find(needle, position)) != std::string::npos)
        {
            const auto elementBegin = xml.rfind('<', position);
            const auto elementEnd = xml.find('>', position);
            if (elementBegin == std::string::npos || elementEnd == std::string::npos)
            {
                position += needle.size();
                continue;
            }

            const auto element = xml.substr(elementBegin, elementEnd - elementBegin + 1u);
            if (auto value = ExtractXmlAttribute(element, "value");
                value.has_value() && !value->empty())
            {
                return MaterialTextureReference {UnescapeXmlAttributeValue(*value), {}};
            }
            position = elementEnd + 1u;
        }
    }

    size_t textureSlotPosition = 0u;
    while ((textureSlotPosition = xml.find("<textureSlot", textureSlotPosition)) != std::string::npos)
    {
        const auto elementEnd = xml.find('>', textureSlotPosition);
        if (elementEnd == std::string::npos)
            break;

        const auto element = xml.substr(textureSlotPosition, elementEnd - textureSlotPosition + 1u);
        MaterialTextureReference reference;
        if (auto key = ExtractXmlAttribute(element, "texture");
            key.has_value() && !key->empty())
        {
            reference.textureKey = UnescapeXmlAttributeValue(*key);
        }
        for (const auto attribute : {"resourcePath", "texture", "value"})
        {
            if (auto value = ExtractXmlAttribute(element, attribute);
                value.has_value() && !value->empty())
            {
                reference.resourcePath = UnescapeXmlAttributeValue(*value);
                return reference;
            }
        }
        textureSlotPosition = elementEnd + 1u;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> ResolveMaterialSourceTextureDependency(
    const AssetThumbnailRequest& request,
    const std::string& materialPayload,
    const std::string& textureKey)
{
    if (textureKey.empty())
        return std::nullopt;

    if (auto manifestSource = ResolveTextureSourceDependencyPathForKey(request, textureKey);
        manifestSource.has_value())
    {
        return manifestSource;
    }

    const std::string needle = "texture-build:texture:" + textureKey + "\\p";
    const auto position = materialPayload.find(needle);
    if (position == std::string::npos)
        return std::nullopt;

    const auto sourcePathBegin = materialPayload.find("\\psourcePath=", position + needle.size());
    if (sourcePathBegin == std::string::npos)
        return std::nullopt;

    const auto valueBegin = sourcePathBegin + std::string_view("\\psourcePath=").size();
    auto valueEnd = materialPayload.find("\\\\p", valueBegin);
    if (valueEnd == std::string::npos)
        valueEnd = materialPayload.find("\\p", valueBegin);
    if (valueEnd == std::string::npos || valueEnd <= valueBegin)
        return std::nullopt;

    auto sourcePathText = materialPayload.substr(valueBegin, valueEnd - valueBegin);
    std::replace(sourcePathText.begin(), sourcePathText.end(), '\\', '/');
    while (sourcePathText.rfind("./", 0u) == 0u)
        sourcePathText.erase(0u, 2u);
    if (sourcePathText.empty())
        return std::nullopt;

    std::vector<std::filesystem::path> candidates;
    const auto sourcePath = std::filesystem::path(sourcePathText).lexically_normal();
    if (sourcePath.is_absolute())
    {
        candidates.push_back(sourcePath);
    }
    else
    {
        candidates.push_back(request.projectRoot / sourcePath);
        if (!request.sourceAssetPath.empty())
        {
            candidates.push_back(
                request.projectRoot /
                std::filesystem::path(request.sourceAssetPath).parent_path() /
                sourcePath);
        }
    }

    const auto assetRoots = MakeProjectEditorAssetRoots(request.projectRoot);
    for (const auto& candidate : candidates)
    {
        const auto normalized = NLS::Core::Assets::NormalizeAssetPath(candidate);
        if (normalized.empty() || !IsTextureThumbnailSourceExtension(normalized))
            continue;

        const auto editorAssetPath = ToEditorAssetPath(assetRoots, normalized);
        if (!ResolveEditorAssetPath(assetRoots, editorAssetPath).empty())
            return normalized;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> ResolveTexturePathFromMaterialPayload(
    const AssetThumbnailRequest& request,
    const std::string& materialPayload)
{
    const auto textureReference = ExtractMaterialTextureReference(materialPayload);
    if (!textureReference.has_value() || textureReference->resourcePath.empty())
        return std::nullopt;

    auto texturePath = std::filesystem::path(textureReference->resourcePath).lexically_normal();
    if (texturePath.has_extension() &&
        !IsTextureThumbnailSourceExtension(texturePath))
    {
        return ResolveMaterialSourceTextureDependency(
            request,
            materialPayload,
            textureReference->textureKey);
    }

    std::vector<std::filesystem::path> candidates;
    if (texturePath.is_absolute())
    {
        candidates.push_back(texturePath);
    }
    else
    {
        candidates.push_back(request.projectRoot / texturePath);
        if (request.assetId.IsValid())
        {
            candidates.push_back(
                request.projectRoot /
                "Library" /
                "Artifacts" /
                texturePath);
        }
    }

    const auto projectRoot = NLS::Core::Assets::NormalizeAssetPath(request.projectRoot);
    const auto artifactRoot = NLS::Core::Assets::NormalizeAssetPath(projectRoot / "Library" / "Artifacts");
    const auto assetRoots = MakeProjectEditorAssetRoots(projectRoot);
    for (const auto& candidate : candidates)
    {
        const auto normalized = NLS::Core::Assets::NormalizeAssetPath(candidate);
        if (normalized.empty())
            continue;

        const bool libraryArtifact =
            !artifactRoot.empty() &&
            IsPhysicalRegularFileInsideEditorAssetRoot(normalized, artifactRoot) &&
            IsNativeTextureArtifactPath(normalized);
        const auto editorAssetPath = ToEditorAssetPath(assetRoots, normalized);
        const bool sourceTexture =
            IsTextureThumbnailSourceExtension(normalized) &&
            !ResolveEditorAssetPath(assetRoots, editorAssetPath).empty();
        if (libraryArtifact || sourceTexture)
            return normalized;
    }
    return ResolveMaterialSourceTextureDependency(
        request,
        materialPayload,
        textureReference->textureKey);
}

std::optional<std::filesystem::path> ResolveTextureSamplePathFromMaterialPayload(
    const AssetThumbnailRequest& request,
    const std::string& materialPayload)
{
    const auto textureReference = ExtractMaterialTextureReference(materialPayload);
    const auto sourceExtension = ToLowerAscii(std::filesystem::path(request.sourceAssetPath).extension().generic_string());
    if (textureReference.has_value() &&
        (sourceExtension == ".fbx" || sourceExtension == ".obj"))
    {
        if (auto sourceTexture = ResolveMaterialSourceTextureDependency(
                request,
                materialPayload,
                textureReference->textureKey);
            sourceTexture.has_value())
        {
            return sourceTexture;
        }
    }

    const auto texturePath = ResolveTexturePathFromMaterialPayload(request, materialPayload);
    if (!texturePath.has_value())
        return std::nullopt;

    if (!IsNativeTextureArtifactPath(*texturePath))
        return texturePath;

    const auto artifact = NLS::Render::Assets::LoadTextureArtifact(*texturePath);
    if (artifact.has_value() &&
        IsRgba8TextureArtifactMipUsable(*artifact))
    {
        return texturePath;
    }

    if (textureReference.has_value())
    {
        return ResolveMaterialSourceTextureDependency(
            request,
            materialPayload,
            textureReference->textureKey);
    }
    return std::nullopt;
}

DownsampledThumbnail RenderTexturePathThumbnail(
    const std::filesystem::path& texturePath,
    const uint32_t requestedSize)
{
    if (IsNativeTextureArtifactPath(texturePath))
    {
        const auto artifact = NLS::Render::Assets::LoadTextureArtifact(texturePath);
        if (!artifact.has_value() ||
            !IsRgba8TextureArtifactMipUsable(*artifact))
        {
            return {};
        }

        const auto& mip = artifact->mips.front();
        return DownsampleRgba8ToThumbnail(
            mip.PixelData(),
            mip.width,
            mip.height,
            mip.rowPitch,
            requestedSize);
    }

    NLS::Image sourceImage(texturePath.string(), false);
    if (sourceImage.GetData() == nullptr ||
        sourceImage.GetWidth() <= 0 ||
        sourceImage.GetHeight() <= 0 ||
        sourceImage.GetChannels() <= 0)
    {
        return {};
    }
    return DownsampleImageToThumbnail(sourceImage, requestedSize);
}

const NLS::Render::Assets::TextureArtifactMip* SelectTexturePreviewMip(
    const NLS::Render::Assets::TextureArtifactData& artifact,
    const uint32_t requestedSize)
{
    if (artifact.mips.empty())
        return nullptr;

    const auto targetSize = std::max(1u, requestedSize);
    const NLS::Render::Assets::TextureArtifactMip* bestMip = nullptr;
    uint32_t bestScore = std::numeric_limits<uint32_t>::max();
    for (const auto& mip : artifact.mips)
    {
        if (!IsRgba8TextureArtifactMipUsable(mip))
            continue;

        const auto mipLargestDimension = (std::max)(mip.width, mip.height);
        const auto score = mipLargestDimension > targetSize
            ? mipLargestDimension - targetSize
            : (targetSize - mipLargestDimension) * 2u;
        if (bestMip == nullptr || score < bestScore)
        {
            bestMip = &mip;
            bestScore = score;
        }
    }
    return bestMip;
}

std::optional<ThumbnailTextureSampleData> LoadTextureSampleData(
    const std::filesystem::path& texturePath,
    const uint32_t requestedSize)
{
    ThumbnailTextureSampleData data;
    if (IsNativeTextureArtifactPath(texturePath))
    {
        const auto artifact = NLS::Render::Assets::LoadTextureArtifact(texturePath);
        if (!artifact.has_value() ||
            !IsRgba8TextureArtifactMipUsable(*artifact))
        {
            return std::nullopt;
        }

        const auto* mip = SelectTexturePreviewMip(*artifact, requestedSize);
        if (mip == nullptr)
            return std::nullopt;

        data.pixels = CopyTextureArtifactMipPixels(*mip);
        data.width = mip->width;
        data.height = mip->height;
        data.rowPitch = mip->rowPitch;
    }
    else
    {
        NLS::Image sourceImage(texturePath.string(), false);
        if (sourceImage.GetData() == nullptr ||
            sourceImage.GetWidth() <= 0 ||
            sourceImage.GetHeight() <= 0 ||
            sourceImage.GetChannels() <= 0)
        {
            return std::nullopt;
        }

        const auto sourcePixels = ConvertToRgba8(sourceImage);
        if (sourcePixels.empty())
            return std::nullopt;

        const auto downsampled = DownsampleRgba8ToThumbnail(
            sourcePixels.data(),
            static_cast<uint32_t>(sourceImage.GetWidth()),
            static_cast<uint32_t>(sourceImage.GetHeight()),
            static_cast<uint32_t>(sourceImage.GetWidth()) * 4u,
            std::max(1u, requestedSize));
        if (downsampled.pixels.empty() || downsampled.width == 0u || downsampled.height == 0u)
            return std::nullopt;

        data.pixels = downsampled.pixels;
        data.width = downsampled.width;
        data.height = downsampled.height;
        data.rowPitch = data.width * 4u;
    }

    if (data.pixels.empty() || data.width == 0u || data.height == 0u || data.rowPitch < data.width * 4u)
        return std::nullopt;
    return data;
}

std::array<float, 4u> SampleTextureNearest(
    const ThumbnailTextureSampleData& texture,
    float u,
    float v)
{
    if (texture.pixels.empty() || texture.width == 0u || texture.height == 0u || texture.rowPitch < texture.width * 4u)
        return {1.0f, 1.0f, 1.0f, 1.0f};

    u = u - std::floor(u);
    v = v - std::floor(v);
    if (texture.flipV)
        v = 1.0f - v;
    const auto x = std::min(
        texture.width - 1u,
        static_cast<uint32_t>(std::floor(u * static_cast<float>(texture.width))));
    const auto y = std::min(
        texture.height - 1u,
        static_cast<uint32_t>(std::floor((1.0f - v) * static_cast<float>(texture.height))));
    const auto* source = texture.pixels.data() + static_cast<size_t>(y) * texture.rowPitch + x * 4u;
    return {
        static_cast<float>(source[0]) / 255.0f,
        static_cast<float>(source[1]) / 255.0f,
        static_cast<float>(source[2]) / 255.0f,
        static_cast<float>(source[3]) / 255.0f
    };
}

struct MaterialPreviewStyle
{
    std::array<float, 4u> baseColor {0.58f, 0.66f, 0.76f, 1.0f};
    std::optional<ThumbnailTextureSampleData> albedoTexture;
};

MaterialPreviewStyle BuildMaterialPreviewStyle(
    const AssetThumbnailRequest& request,
    const std::string& materialPayload,
    const uint32_t requestedSize)
{
    MaterialPreviewStyle style;
    style.baseColor = ExtractMaterialBaseColor(materialPayload);
    if (const auto texturePath = ResolveTextureSamplePathFromMaterialPayload(request, materialPayload);
        texturePath.has_value())
    {
        style.albedoTexture = LoadTextureSampleData(*texturePath, requestedSize);
        if (style.albedoTexture.has_value() && !IsNativeTextureArtifactPath(*texturePath))
            style.albedoTexture->flipV = ShouldFlipMaterialSourceTextureVertically(request);
    }
    return style;
}

std::optional<size_t> MaterialPreviewIndexForSubAssetKey(const std::string& subAssetKey)
{
    constexpr std::string_view kPrefix = "material:";
    if (subAssetKey.rfind(kPrefix, 0u) != 0u)
        return std::nullopt;

    auto token = subAssetKey.substr(kPrefix.size());
    if (const auto separator = token.find_last_of("/\\:");
        separator != std::string::npos && separator + 1u < token.size())
    {
        token = token.substr(separator + 1u);
    }

    if (token.empty() || !std::all_of(token.begin(), token.end(), [](const unsigned char character)
        {
            return std::isdigit(character) != 0;
        }))
    {
        return std::nullopt;
    }

    try
    {
        return static_cast<size_t>(std::stoull(token));
    }
    catch (...)
    {
        return std::nullopt;
    }
}

struct MaterialPreviewArtifact
{
    std::filesystem::path path;
    std::string subAssetKey;
};

std::vector<MaterialPreviewArtifact> ResolveMaterialArtifactPaths(
    const AssetThumbnailRequest& request)
{
    std::vector<MaterialPreviewArtifact> paths;
    if (!request.subAssetKey.empty() &&
        request.subAssetKey.rfind("material:", 0u) == 0u)
    {
        if (auto resolved = ResolveArtifactPathForPreview(request, request.artifactPath);
            resolved.has_value())
        {
            paths.push_back({*resolved, request.subAssetKey});
            return paths;
        }
    }

    const auto manifest = LoadThumbnailArtifactManifest(request);
    if (!manifest.has_value())
        return paths;

    for (const auto& artifact : manifest->subAssets)
    {
        if (artifact.artifactType != NLS::Core::Assets::ArtifactType::Material)
            continue;

        if (auto resolved = ResolveArtifactPathForPreview(request, artifact.artifactPath);
            resolved.has_value())
        {
            paths.push_back({*resolved, artifact.subAssetKey});
        }
    }
    return paths;
}

std::vector<MaterialPreviewStyle> LoadMaterialPreviewStyles(const AssetThumbnailRequest& request)
{
    std::vector<MaterialPreviewStyle> styles;
    size_t sequentialIndex = 0u;
    for (const auto& materialArtifact : ResolveMaterialArtifactPaths(request))
    {
        const auto& materialPath = materialArtifact.path;
        if (StructurePreviewArtifactExceedsBudget(
                materialPath,
                NLS::Core::Assets::ArtifactType::Material,
                1u))
        {
            const auto materialIndex = MaterialPreviewIndexForSubAssetKey(materialArtifact.subAssetKey)
                .value_or(sequentialIndex);
            if (materialIndex >= styles.size())
                styles.resize(materialIndex + 1u);
            ++sequentialIndex;
            continue;
        }

        const auto payload = ReadNativeOrPlainTextArtifact(
            materialPath,
            NLS::Core::Assets::ArtifactType::Material,
            1u);
        const auto materialIndex = MaterialPreviewIndexForSubAssetKey(materialArtifact.subAssetKey)
            .value_or(sequentialIndex);
        if (materialIndex >= styles.size())
            styles.resize(materialIndex + 1u);
        styles[materialIndex] = payload.has_value()
            ? BuildMaterialPreviewStyle(request, *payload, request.requestedSize)
            : MaterialPreviewStyle {};
        ++sequentialIndex;
    }
    return styles;
}

DownsampledThumbnail RenderMaterialSphereThumbnail(
    const MaterialPreviewStyle& style,
    const uint32_t requestedSize)
{
    auto canvas = MakeCanvas(requestedSize);
    const auto center = (static_cast<float>(canvas.width) - 1.0f) * 0.5f;
    const auto radius = std::max(1.0f, static_cast<float>(canvas.width) * 0.42f);
    constexpr float lightX = -0.35f;
    constexpr float lightY = -0.55f;
    constexpr float lightZ = 0.76f;

    for (uint32_t y = 0u; y < canvas.height; ++y)
    {
        for (uint32_t x = 0u; x < canvas.width; ++x)
        {
            const float nx = (static_cast<float>(x) - center) / radius;
            const float ny = (static_cast<float>(y) - center) / radius;
            const float rr = nx * nx + ny * ny;
            if (rr > 1.0f)
                continue;

            const float nz = std::sqrt(std::max(0.0f, 1.0f - rr));
            auto materialColor = style.baseColor;
            if (style.albedoTexture.has_value())
            {
                const float u = 0.5f + std::atan2(nx, nz) / (2.0f * 3.14159265358979323846f);
                const float v = 0.5f - std::asin(std::clamp(ny, -1.0f, 1.0f)) / 3.14159265358979323846f;
                const auto texel = SampleTextureNearest(*style.albedoTexture, u, v);
                materialColor[0] *= texel[0];
                materialColor[1] *= texel[1];
                materialColor[2] *= texel[2];
                materialColor[3] *= texel[3];
            }

            const float diffuse = std::max(0.0f, nx * lightX + ny * lightY + nz * lightZ);
            const float rim = std::pow(std::max(0.0f, 1.0f - nz), 2.0f) * 0.18f;
            const float shade = std::clamp(0.22f + diffuse * 0.78f + rim, 0.0f, 1.0f);
            PutPixel(
                canvas,
                static_cast<int>(x),
                static_cast<int>(y),
                static_cast<uint8_t>(std::clamp(materialColor[0] * shade * 255.0f, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(materialColor[1] * shade * 255.0f, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(materialColor[2] * shade * 255.0f, 0.0f, 255.0f)),
                static_cast<uint8_t>(std::clamp(materialColor[3] * 255.0f, 0.0f, 255.0f)));
        }
    }
    return CanvasToThumbnail(std::move(canvas));
}

struct MeshPreviewTriangle
{
    struct Vertex
    {
        std::array<float, 3u> screen {};
        std::array<float, 3u> normal {};
        std::array<float, 2u> uv {};
    };
    std::array<Vertex, 3u> vertices {};
    size_t materialIndex = 0u;
};

float Dot3(const std::array<float, 3u>& left, const std::array<float, 3u>& right)
{
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

std::array<float, 3u> Normalize3(std::array<float, 3u> value)
{
    const auto length = std::sqrt(std::max(0.000001f, Dot3(value, value)));
    value[0] /= length;
    value[1] /= length;
    value[2] /= length;
    return value;
}

bool IsNearlyZero3(const std::array<float, 3u>& value)
{
    return Dot3(value, value) < 0.000001f;
}

std::array<float, 3u> Cross3(
    const std::array<float, 3u>& left,
    const std::array<float, 3u>& right)
{
    return {
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0]
    };
}

std::array<float, 3u> TriangleFallbackNormal(
    const NLS::Render::Geometry::Vertex& a,
    const NLS::Render::Geometry::Vertex& b,
    const NLS::Render::Geometry::Vertex& c)
{
    const std::array<float, 3u> ab {
        b.position[0] - a.position[0],
        b.position[1] - a.position[1],
        b.position[2] - a.position[2]
    };
    const std::array<float, 3u> ac {
        c.position[0] - a.position[0],
        c.position[1] - a.position[1],
        c.position[2] - a.position[2]
    };
    auto normal = Cross3(ab, ac);
    if (IsNearlyZero3(normal))
        return {0.0f, 1.0f, 0.0f};
    return Normalize3(normal);
}

std::array<float, 3u> RotateThumbnailPreviewVector(std::array<float, 3u> value)
{
    const auto yaw = ThumbnailPreviewCamera::MeshYawDegrees * ThumbnailPreviewCamera::DegreesToRadians;
    const auto pitch = ThumbnailPreviewCamera::MeshLookPitchDegrees *
        ThumbnailPreviewCamera::DegreesToRadians;

    const auto cy = std::cos(yaw);
    const auto sy = std::sin(yaw);
    std::array<float, 3u> rotated {
        value[0] * cy + value[2] * sy,
        value[1],
        -value[0] * sy + value[2] * cy
    };

    const auto cp = std::cos(pitch);
    const auto sp = std::sin(pitch);
    return {
        rotated[0],
        rotated[1] * cp - rotated[2] * sp,
        rotated[1] * sp + rotated[2] * cp
    };
}

std::array<float, 3u> TransformThumbnailPreviewPoint(
    const NLS::Render::Geometry::Vertex& vertex,
    const std::array<float, 3u>& center,
    const float cameraDistance)
{
    auto rotated = RotateThumbnailPreviewVector({
        vertex.position[0] - center[0],
        vertex.position[1] - center[1],
        vertex.position[2] - center[2]
    });
    rotated[2] += cameraDistance;
    return rotated;
}

NLS::Render::Geometry::Vertex TransformPrefabPreviewVertex(
    NLS::Render::Geometry::Vertex vertex,
    const PreviewDrawItem& drawItem)
{
    const auto rotation = NLS::Maths::Quaternion::Normalize(drawItem.localRotation);
    const NLS::Maths::Vector3 scaledPosition {
        vertex.position[0] * drawItem.localScale.x,
        vertex.position[1] * drawItem.localScale.y,
        vertex.position[2] * drawItem.localScale.z
    };
    const auto transformedPosition =
        NLS::Maths::Quaternion::RotatePoint(scaledPosition, rotation) + drawItem.localPosition;
    vertex.position[0] = transformedPosition.x;
    vertex.position[1] = transformedPosition.y;
    vertex.position[2] = transformedPosition.z;

    const NLS::Maths::Vector3 normal {
        vertex.normals[0],
        vertex.normals[1],
        vertex.normals[2]
    };
    const auto transformedNormal = NLS::Maths::Quaternion::RotatePoint(normal, rotation);
    vertex.normals[0] = transformedNormal.x;
    vertex.normals[1] = transformedNormal.y;
    vertex.normals[2] = transformedNormal.z;
    return vertex;
}

NLS::Render::Assets::MeshArtifactData TransformPrefabPreviewMeshInstance(
    NLS::Render::Assets::MeshArtifactData mesh,
    const PreviewDrawItem& drawItem)
{
    for (auto& vertex : mesh.vertices)
        vertex = TransformPrefabPreviewVertex(vertex, drawItem);
    if (mesh.hasBoundingSphere)
    {
        const auto rotation = NLS::Maths::Quaternion::Normalize(drawItem.localRotation);
        const NLS::Maths::Vector3 scaledCenter {
            mesh.boundingSphere.position.x * drawItem.localScale.x,
            mesh.boundingSphere.position.y * drawItem.localScale.y,
            mesh.boundingSphere.position.z * drawItem.localScale.z
        };
        mesh.boundingSphere.position =
            NLS::Maths::Quaternion::RotatePoint(scaledCenter, rotation) + drawItem.localPosition;
        const auto maxScale = std::max({
            std::abs(drawItem.localScale.x),
            std::abs(drawItem.localScale.y),
            std::abs(drawItem.localScale.z)
        });
        mesh.boundingSphere.radius *= std::max(0.0001f, maxScale);
    }
    return mesh;
}

std::array<float, 4u> ShadeUnityPreviewMaterial(
    const MaterialPreviewStyle& material,
    const std::array<float, 3u>& normal,
    const std::array<float, 2u>& uv)
{
    std::array<float, 4u> color = material.baseColor;
    if (material.albedoTexture.has_value())
    {
        const auto texel = SampleTextureNearest(*material.albedoTexture, uv[0], uv[1]);
        color[0] *= texel[0];
        color[1] *= texel[1];
        color[2] *= texel[2];
        color[3] *= texel[3];
    }

    const auto n = Normalize3(normal);
    const auto light0 = Normalize3({0.58f, 0.64f, 0.50f});
    const auto light1 = Normalize3({-0.35f, 0.25f, 0.90f});
    const auto diffuse =
        std::max(0.0f, Dot3(n, light0)) * 1.15f +
        std::max(0.0f, Dot3(n, light1)) * 0.45f;
    const auto shade = std::clamp(0.18f + diffuse, 0.0f, 1.35f);
    color[0] = std::clamp(color[0] * shade, 0.0f, 1.0f);
    color[1] = std::clamp(color[1] * shade, 0.0f, 1.0f);
    color[2] = std::clamp(color[2] * shade, 0.0f, 1.0f);
    color[3] = std::clamp(color[3], 0.0f, 1.0f);
    return color;
}

DownsampledThumbnail RenderMeshSetThumbnail(
    const std::vector<NLS::Render::Assets::MeshArtifactData>& meshes,
    const std::vector<MaterialPreviewStyle>& materials,
    const uint32_t requestedSize)
{
    auto canvas = MakeCanvas(requestedSize);
    if (meshes.empty())
        return CanvasToThumbnail(std::move(canvas));

    std::array<float, 3u> minBounds {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()
    };
    std::array<float, 3u> maxBounds {
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()
    };
    size_t vertexCount = 0u;
    for (const auto& mesh : meshes)
    {
        for (const auto& vertex : mesh.vertices)
        {
            ++vertexCount;
            for (size_t axis = 0u; axis < 3u; ++axis)
            {
                minBounds[axis] = std::min(minBounds[axis], vertex.position[axis]);
                maxBounds[axis] = std::max(maxBounds[axis], vertex.position[axis]);
            }
        }
    }
    if (vertexCount == 0u)
        return CanvasToThumbnail(std::move(canvas));

    const std::array<float, 3u> center {
        (minBounds[0] + maxBounds[0]) * 0.5f,
        (minBounds[1] + maxBounds[1]) * 0.5f,
        (minBounds[2] + maxBounds[2]) * 0.5f
    };
    const auto extentX = maxBounds[0] - minBounds[0];
    const auto extentY = maxBounds[1] - minBounds[1];
    const auto extentZ = maxBounds[2] - minBounds[2];
    const auto halfSize = std::max(0.0001f, 0.5f * std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ));
    const auto cameraDistance = halfSize * 4.0f;
    const auto focalLength = (static_cast<float>(canvas.height) * 0.5f) /
        std::tan((ThumbnailPreviewCamera::FieldOfViewDegrees * 0.5f) *
            ThumbnailPreviewCamera::DegreesToRadians);
    const auto project = [&](
        const NLS::Render::Geometry::Vertex& vertex,
        const std::array<float, 3u>& fallbackNormal) -> MeshPreviewTriangle::Vertex
    {
        const auto view = TransformThumbnailPreviewPoint(vertex, center, cameraDistance);
        const auto depth = std::max(0.0001f, view[2]);
        const std::array<float, 3u> sourceNormal {
            vertex.normals[0],
            vertex.normals[1],
            vertex.normals[2]
        };
        const bool usesFallbackNormal = IsNearlyZero3(sourceNormal);
        const auto normal = usesFallbackNormal ? fallbackNormal : sourceNormal;
        auto previewNormal = RotateThumbnailPreviewVector(normal);
        if (usesFallbackNormal && previewNormal[2] < 0.0f)
        {
            previewNormal[0] = -previewNormal[0];
            previewNormal[1] = -previewNormal[1];
            previewNormal[2] = -previewNormal[2];
        }
        return {
            {
                view[0] * focalLength / depth + static_cast<float>(canvas.width) * 0.5f,
                static_cast<float>(canvas.height) * 0.5f - view[1] * focalLength / depth,
                depth
            },
            Normalize3(previewNormal),
            {vertex.texCoords[0], vertex.texCoords[1]}
        };
    };

    size_t totalTriangleCount = 0u;
    for (const auto& mesh : meshes)
        totalTriangleCount += mesh.indices.size() / 3u;
    const auto triangleStride = totalTriangleCount > kMaxMeshPreviewRenderedTriangles
        ? (totalTriangleCount + kMaxMeshPreviewRenderedTriangles - 1u) / kMaxMeshPreviewRenderedTriangles
        : 1u;

    std::vector<float> depthBuffer(
        static_cast<size_t>(canvas.width) * canvas.height,
        std::numeric_limits<float>::max());
    std::vector<MeshPreviewTriangle> triangles;
    triangles.reserve(std::min(totalTriangleCount, kMaxMeshPreviewRenderedTriangles));
    size_t globalTriangleIndex = 0u;
    for (const auto& mesh : meshes)
    {
        for (size_t index = 0u; index + 2u < mesh.indices.size(); index += 3u)
        {
            if ((globalTriangleIndex++ % triangleStride) != 0u)
                continue;

            const auto i0 = mesh.indices[index + 0u];
            const auto i1 = mesh.indices[index + 1u];
            const auto i2 = mesh.indices[index + 2u];
            if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size())
                continue;

            const auto& v0 = mesh.vertices[i0];
            const auto& v1 = mesh.vertices[i1];
            const auto& v2 = mesh.vertices[i2];
            const auto fallbackNormal = TriangleFallbackNormal(v0, v1, v2);
            triangles.push_back({{project(v0, fallbackNormal), project(v1, fallbackNormal), project(v2, fallbackNormal)}, mesh.materialIndex});
        }
    }

    bool wroteVisiblePixel = false;
    for (const auto& triangle : triangles)
    {
        const auto& a = triangle.vertices[0];
        const auto& b = triangle.vertices[1];
        const auto& c = triangle.vertices[2];
        const int minX = std::max(0, static_cast<int>(std::floor(std::min({a.screen[0], b.screen[0], c.screen[0]}))));
        const int maxX = std::min(static_cast<int>(canvas.width) - 1, static_cast<int>(std::ceil(std::max({a.screen[0], b.screen[0], c.screen[0]}))));
        const int minY = std::max(0, static_cast<int>(std::floor(std::min({a.screen[1], b.screen[1], c.screen[1]}))));
        const int maxY = std::min(static_cast<int>(canvas.height) - 1, static_cast<int>(std::ceil(std::max({a.screen[1], b.screen[1], c.screen[1]}))));
        if (minX > maxX || minY > maxY)
            continue;

        const auto edge = [](const MeshPreviewTriangle::Vertex& left, const MeshPreviewTriangle::Vertex& right, const float x, const float y)
        {
            return (x - left.screen[0]) * (right.screen[1] - left.screen[1]) -
                (y - left.screen[1]) * (right.screen[0] - left.screen[0]);
        };
        const auto edgeCoverageTolerance = [](const MeshPreviewTriangle::Vertex& left, const MeshPreviewTriangle::Vertex& right)
        {
            const auto dx = right.screen[0] - left.screen[0];
            const auto dy = right.screen[1] - left.screen[1];
            return std::sqrt(dx * dx + dy * dy) * 0.70710677f;
        };
        const auto area = edge(a, b, c.screen[0], c.screen[1]);
        if (std::abs(area) < 0.0001f)
            continue;

        const auto material = triangle.materialIndex < materials.size()
            ? materials[triangle.materialIndex]
            : MaterialPreviewStyle {};
        const auto w0Tolerance = edgeCoverageTolerance(b, c);
        const auto w1Tolerance = edgeCoverageTolerance(c, a);
        const auto w2Tolerance = edgeCoverageTolerance(a, b);
        for (int y = minY; y <= maxY; ++y)
        {
            for (int x = minX; x <= maxX; ++x)
            {
                const auto px = static_cast<float>(x) + 0.5f;
                const auto py = static_cast<float>(y) + 0.5f;
                const auto w0 = edge(b, c, px, py);
                const auto w1 = edge(c, a, px, py);
                const auto w2 = edge(a, b, px, py);
                const bool insidePositive = w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f;
                const bool insideNegative = w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f;
                const bool overlapsPositive =
                    w0 >= -w0Tolerance &&
                    w1 >= -w1Tolerance &&
                    w2 >= -w2Tolerance;
                const bool overlapsNegative =
                    w0 <= w0Tolerance &&
                    w1 <= w1Tolerance &&
                    w2 <= w2Tolerance;
                if (!insidePositive && !insideNegative && !overlapsPositive && !overlapsNegative)
                    continue;

                const auto invArea = 1.0f / area;
                auto b0 = w0 * invArea;
                auto b1 = w1 * invArea;
                auto b2 = w2 * invArea;
                if (!insidePositive && !insideNegative)
                {
                    b0 = std::max(0.0f, b0);
                    b1 = std::max(0.0f, b1);
                    b2 = std::max(0.0f, b2);
                    const auto barycentricSum = b0 + b1 + b2;
                    if (barycentricSum > 0.0001f)
                    {
                        const auto invSum = 1.0f / barycentricSum;
                        b0 *= invSum;
                        b1 *= invSum;
                        b2 *= invSum;
                    }
                    else
                    {
                        b0 = 1.0f / 3.0f;
                        b1 = 1.0f / 3.0f;
                        b2 = 1.0f / 3.0f;
                    }
                }
                const auto depth = a.screen[2] * b0 + b.screen[2] * b1 + c.screen[2] * b2;
                const auto depthIndex = static_cast<size_t>(y) * canvas.width + static_cast<size_t>(x);
                if (depth >= depthBuffer[depthIndex])
                    continue;
                depthBuffer[depthIndex] = depth;

                const std::array<float, 3u> normal {
                    a.normal[0] * b0 + b.normal[0] * b1 + c.normal[0] * b2,
                    a.normal[1] * b0 + b.normal[1] * b1 + c.normal[1] * b2,
                    a.normal[2] * b0 + b.normal[2] * b1 + c.normal[2] * b2
                };
                const std::array<float, 2u> uv {
                    a.uv[0] * b0 + b.uv[0] * b1 + c.uv[0] * b2,
                    a.uv[1] * b0 + b.uv[1] * b1 + c.uv[1] * b2
                };
                const auto shaded = ShadeUnityPreviewMaterial(material, normal, uv);
                const auto alpha = static_cast<uint8_t>(std::clamp(shaded[3] * 255.0f, 0.0f, 255.0f));
                PutPixel(
                    canvas,
                    x,
                    y,
                    static_cast<uint8_t>(std::clamp(shaded[0] * 255.0f, 0.0f, 255.0f)),
                    static_cast<uint8_t>(std::clamp(shaded[1] * 255.0f, 0.0f, 255.0f)),
                    static_cast<uint8_t>(std::clamp(shaded[2] * 255.0f, 0.0f, 255.0f)),
                    alpha);
                wroteVisiblePixel = wroteVisiblePixel || alpha != 0u;
            }
        }
    }

    if (triangles.empty() || !wroteVisiblePixel)
    {
        for (const auto& mesh : meshes)
        {
            for (const auto& vertex : mesh.vertices)
            {
                const auto p = project(vertex, {0.0f, 1.0f, 0.0f});
                PutPixel(
                    canvas,
                    static_cast<int>(std::lround(p.screen[0])),
                    static_cast<int>(std::lround(p.screen[1])),
                    150u,
                    210u,
                    255u);
            }
        }
    }

    return CanvasToThumbnail(std::move(canvas));
}

DownsampledThumbnail RenderMeshThumbnail(
    const NLS::Render::Assets::MeshArtifactData& mesh,
    const uint32_t requestedSize)
{
    return RenderMeshSetThumbnail({mesh}, {}, requestedSize);
}

DownsampledThumbnail RenderMeshSetThumbnail(
    const std::vector<NLS::Render::Assets::MeshArtifactData>& meshes,
    const uint32_t requestedSize)
{
    return RenderMeshSetThumbnail(meshes, {}, requestedSize);
}

DownsampledThumbnail RenderPrefabStructureThumbnail(
    const std::string& prefabPayload,
    const uint32_t requestedSize)
{
    auto canvas = MakeCanvas(requestedSize);
    const auto document = NLS::Engine::Serialize::ObjectGraphReader::Read(prefabPayload);
    if (!document.has_value() || document->objects.empty())
        return CanvasToThumbnail(std::move(canvas));

    const auto size = static_cast<int>(canvas.width);
    const int rootLeft = std::max(2, size / 4);
    const int rootRight = std::min(size - 3, (size * 3) / 4);
    const int rootTop = std::max(2, size / 5);
    const int rootBottom = std::min(size - 3, rootTop + std::max(4, size / 6));
    for (int y = rootTop; y <= rootBottom; ++y)
    {
        for (int x = rootLeft; x <= rootRight; ++x)
            PutPixel(canvas, x, y, 116u, 172u, 232u);
    }

    const auto childCount = std::min<size_t>(document->objects.size() - 1u, 5u);
    const int childTop = std::min(size - 4, rootBottom + std::max(4, size / 7));
    const int slotWidth = std::max(3, size / 7);
    for (size_t child = 0u; child < childCount; ++child)
    {
        const int x = std::max(2, size / 2 - static_cast<int>(childCount) * slotWidth / 2 + static_cast<int>(child) * slotWidth);
        DrawLine(canvas, size / 2, rootBottom, x + slotWidth / 2, childTop, 160u, 170u, 185u);
        for (int yy = childTop; yy < std::min(size - 2, childTop + slotWidth); ++yy)
        {
            for (int xx = x; xx < std::min(size - 2, x + slotWidth); ++xx)
                PutPixel(canvas, xx, yy, 185u, 154u, 90u);
        }
    }

    return CanvasToThumbnail(std::move(canvas));
}

DownsampledThumbnail RenderMaterialPreviewThumbnail(
    const AssetThumbnailRequest& request,
    const std::string& materialPayload,
    const uint32_t requestedSize)
{
    return RenderMaterialSphereThumbnail(
        BuildMaterialPreviewStyle(request, materialPayload, requestedSize),
        requestedSize);
}

AssetThumbnailServiceResult GenerateMaterialThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation,
    const AssetThumbnailCancelToken& cancelToken)
{
    const auto previewRequest = ResolveDeferredThumbnailPreviewRequest(request);
    const auto metadataRequest = BuildResolvedThumbnailCacheRequest(request, previewRequest);
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    if (!IsThumbnailRequestStillFresh(request, &evaluation))
        return BuildStaleThumbnailRequestResult(request, evaluation);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);

    const auto previewPath = ResolvePreviewArtifactOrSourcePath(previewRequest);
    if (!previewPath.has_value())
    {
        result.diagnostic = IsMissingThumbnailArtifactPath(request)
            ? "thumbnail-material-artifact-missing"
            : "thumbnail-material-artifact-path-invalid";
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &metadataRequest);
        return result;
    }

    if (StructurePreviewArtifactExceedsBudget(
            *previewPath,
            NLS::Core::Assets::ArtifactType::Material,
            1u))
    {
        result.status = AssetThumbnailServiceStatus::Fallback;
        result.diagnostic = kMaterialPreviewBudgetExceededDiagnostic;
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &metadataRequest);
        return result;
    }

    const auto payload = ReadNativeOrPlainTextArtifact(
        *previewPath,
        NLS::Core::Assets::ArtifactType::Material,
        1u);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);
    if (!payload.has_value())
    {
        result.diagnostic = "thumbnail-material-artifact-read-failed";
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &metadataRequest);
        return result;
    }

    return WriteThumbnailPngResult(
        request,
        evaluation,
        RenderMaterialPreviewThumbnail(previewRequest, *payload, request.requestedSize),
        "thumbnail-material-preview-generation-failed",
        cancelToken,
        &metadataRequest);
}

AssetThumbnailServiceResult GenerateMeshBackedThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation,
    const std::string& missingDiagnostic,
    const AssetThumbnailCancelToken& cancelToken,
    const AssetThumbnailRequest* metadataRequest = nullptr)
{
    const AssetThumbnailRequest& cacheMetadataRequest =
        metadataRequest != nullptr ? *metadataRequest : request;
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    if (!IsThumbnailRequestStillFresh(request, &evaluation))
        return BuildStaleThumbnailRequestResult(request, evaluation);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);

    const auto meshPath = ResolveFirstMeshArtifactPath(request);
    if (!meshPath.has_value())
    {
        result.diagnostic = missingDiagnostic;
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &cacheMetadataRequest);
        return result;
    }

    const auto meshHeader = NLS::Render::Assets::ReadMeshArtifactHeaderPreview(
        *meshPath,
        kMaxStructurePreviewArtifactPayloadBytes);
    if (!meshHeader.has_value() ||
        MeshArtifactFileExceedsThumbnailPreviewBudget(*meshPath, *meshHeader))
    {
        result.diagnostic = "thumbnail-model-mesh-artifact-read-failed";
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &cacheMetadataRequest);
        return result;
    }
    if (!meshHeader->isLODBundle && MeshPreviewHeaderExceedsCpuLoadBudget(*meshHeader))
    {
        result.status = AssetThumbnailServiceStatus::Fallback;
        result.diagnostic = "thumbnail-model-preview-budget-exceeded";
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &cacheMetadataRequest);
        return result;
    }

    const auto mesh = LoadMeshArtifactForThumbnailPreview(*meshPath, *meshHeader);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);
    if (!mesh.has_value())
    {
        result.diagnostic = "thumbnail-model-mesh-artifact-read-failed";
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &cacheMetadataRequest);
        return result;
    }

    return WriteThumbnailPngResult(
        request,
        evaluation,
        RenderMeshThumbnail(*mesh, request.requestedSize),
        "thumbnail-model-preview-generation-failed",
        cancelToken,
        &cacheMetadataRequest);
}

AssetThumbnailServiceResult GenerateMeshSetThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation,
    const std::vector<std::filesystem::path>& meshPaths,
    const std::string& missingDiagnostic,
    const AssetThumbnailCancelToken& cancelToken,
    const AssetThumbnailRequest* metadataRequest = nullptr)
{
    const AssetThumbnailRequest& cacheMetadataRequest =
        metadataRequest != nullptr ? *metadataRequest : request;
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    if (!IsThumbnailRequestStillFresh(request, &evaluation))
        return BuildStaleThumbnailRequestResult(request, evaluation);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);
    if (meshPaths.empty())
    {
        result.diagnostic = missingDiagnostic;
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &cacheMetadataRequest);
        return result;
    }

    struct MeshPreviewArtifactCandidate
    {
        std::filesystem::path path;
        NLS::Render::Assets::MeshArtifactHeaderPreview header;
        size_t score = 0u;
    };

    std::vector<MeshPreviewArtifactCandidate> candidates;
    candidates.reserve(meshPaths.size());
    for (const auto& meshPath : meshPaths)
    {
        const auto meshHeader = NLS::Render::Assets::ReadMeshArtifactHeaderPreview(
            meshPath,
            kMaxStructurePreviewArtifactPayloadBytes);
        if (!meshHeader.has_value() ||
            MeshArtifactFileExceedsThumbnailPreviewBudget(meshPath, *meshHeader))
        {
            result.diagnostic = "thumbnail-model-mesh-artifact-read-failed";
            WriteThumbnailMetadataForEvaluation(
                request,
                evaluation,
                AssetThumbnailCacheStatus::Failed,
                result.diagnostic,
                &cacheMetadataRequest);
            return result;
        }
        candidates.push_back({
            meshPath,
            *meshHeader,
            static_cast<size_t>(meshHeader->vertexCount) + static_cast<size_t>(meshHeader->indexCount)
        });
    }

    std::stable_sort(
        candidates.begin(),
        candidates.end(),
        [](const MeshPreviewArtifactCandidate& left, const MeshPreviewArtifactCandidate& right)
        {
            return left.score > right.score;
        });

    std::vector<NLS::Render::Assets::MeshArtifactData> meshes;
    meshes.reserve(candidates.size());
    size_t loadedVertices = 0u;
    size_t loadedIndices = 0u;
    bool skippedBudgetedMesh = false;
    for (const auto& candidate : candidates)
    {
        const bool legacyMeshExceedsBudget =
            !candidate.header.isLODBundle && MeshPreviewHeaderExceedsCpuLoadBudget(candidate.header);
        const bool wouldExceedBudget =
            legacyMeshExceedsBudget ||
            (!meshes.empty() &&
                (loadedVertices + candidate.header.vertexCount > kMaxMeshPreviewLoadedVertices ||
                    loadedIndices + candidate.header.indexCount > kMaxMeshPreviewLoadedIndices));
        if (wouldExceedBudget)
        {
            skippedBudgetedMesh = true;
            continue;
        }

        const auto mesh = LoadMeshArtifactForThumbnailPreview(candidate.path, candidate.header);
        if (IsThumbnailGenerationCancelled(cancelToken))
            return BuildCancelledThumbnailRequestResult(request, evaluation);
        if (!mesh.has_value())
        {
            result.diagnostic = "thumbnail-model-mesh-artifact-read-failed";
            WriteThumbnailMetadataForEvaluation(
                request,
                evaluation,
                AssetThumbnailCacheStatus::Failed,
                result.diagnostic,
                &cacheMetadataRequest);
            return result;
        }
        loadedVertices += mesh->vertices.size();
        loadedIndices += mesh->indices.size();
        meshes.push_back(*mesh);
    }

    if (skippedBudgetedMesh &&
        !ShouldRetryLegacyImportedPrefabBudgetFailure(request))
    {
        result.status = AssetThumbnailServiceStatus::Fallback;
        result.diagnostic = "thumbnail-model-preview-budget-exceeded";
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &cacheMetadataRequest);
        return result;
    }

    if (meshes.empty())
    {
        result.diagnostic = "thumbnail-model-mesh-artifact-read-failed";
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &cacheMetadataRequest);
        return result;
    }

    return WriteThumbnailPngResult(
        request,
        evaluation,
        RenderMeshSetThumbnail(
            meshes,
            request.kind == AssetThumbnailKind::PrefabPreview
                ? LoadMaterialPreviewStyles(request)
                : std::vector<MaterialPreviewStyle> {},
            request.requestedSize),
        "thumbnail-model-preview-generation-failed",
        cancelToken,
        &cacheMetadataRequest);
}

std::optional<AssetThumbnailServiceResult> TryGeneratePrefabSnapshotThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailRequest& previewRequest,
    const PreviewRenderableSnapshot& snapshot,
    const AssetThumbnailCacheEvaluation& evaluation,
    const AssetThumbnailCancelToken& cancelToken)
{
    if (snapshot.drawItems.empty())
        return std::nullopt;

    const auto metadataRequest = BuildResolvedThumbnailCacheRequest(request, previewRequest);
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    std::vector<NLS::Render::Assets::MeshArtifactData> meshes;
    meshes.reserve(snapshot.drawItems.size());
    size_t loadedVertices = 0u;
    size_t loadedIndices = 0u;
    bool skippedBudgetedMesh = false;
    bool missingMeshDependency = snapshot.expectedDrawItemCount == 0u ||
        snapshot.expectedDrawItemCount != snapshot.drawItems.size();
    for (const auto& drawItem : snapshot.drawItems)
    {
        const auto meshPath = ResolvePrefabPreviewDrawItemMeshPath(previewRequest, drawItem);
        if (!meshPath.has_value())
        {
            missingMeshDependency = true;
            continue;
        }

        const auto meshHeader = NLS::Render::Assets::ReadMeshArtifactHeaderPreview(
            *meshPath,
            kMaxStructurePreviewArtifactPayloadBytes);
        if (!meshHeader.has_value() ||
            MeshArtifactFileExceedsThumbnailPreviewBudget(*meshPath, *meshHeader))
        {
            result.diagnostic = "thumbnail-model-mesh-artifact-read-failed";
            WriteThumbnailMetadataForEvaluation(
                request,
                evaluation,
                AssetThumbnailCacheStatus::Failed,
                result.diagnostic,
                &metadataRequest);
            return result;
        }

        const bool legacyMeshExceedsBudget =
            !meshHeader->isLODBundle && MeshPreviewHeaderExceedsCpuLoadBudget(*meshHeader);
        const bool wouldExceedBudget =
            legacyMeshExceedsBudget ||
            (!meshes.empty() &&
                (loadedVertices + meshHeader->vertexCount > kMaxMeshPreviewLoadedVertices ||
                    loadedIndices + meshHeader->indexCount > kMaxMeshPreviewLoadedIndices));
        if (wouldExceedBudget)
        {
            skippedBudgetedMesh = true;
            continue;
        }

        const auto mesh = LoadMeshArtifactForThumbnailPreview(*meshPath, *meshHeader);
        if (IsThumbnailGenerationCancelled(cancelToken))
            return BuildCancelledThumbnailRequestResult(request, evaluation);
        if (!mesh.has_value())
        {
            result.diagnostic = "thumbnail-model-mesh-artifact-read-failed";
            WriteThumbnailMetadataForEvaluation(
                request,
                evaluation,
                AssetThumbnailCacheStatus::Failed,
                result.diagnostic,
                &metadataRequest);
            return result;
        }

        loadedVertices += mesh->vertices.size();
        loadedIndices += mesh->indices.size();
        meshes.push_back(TransformPrefabPreviewMeshInstance(*mesh, drawItem));
    }

    if (missingMeshDependency)
    {
        result.diagnostic = "thumbnail-prefab-preview-mesh-artifact-missing";
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &metadataRequest);
        return result;
    }

    if (skippedBudgetedMesh &&
        !ShouldRetryLegacyImportedPrefabBudgetFailure(request))
    {
        result.status = AssetThumbnailServiceStatus::Fallback;
        result.diagnostic = "thumbnail-model-preview-budget-exceeded";
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &metadataRequest);
        return result;
    }

    if (meshes.empty())
    {
        return std::nullopt;
    }

    return WriteThumbnailPngResult(
        request,
        evaluation,
        RenderMeshSetThumbnail(
            meshes,
            LoadMaterialPreviewStyles(previewRequest),
            request.requestedSize),
        "thumbnail-prefab-preview-generation-failed",
        cancelToken,
        &metadataRequest);
}

AssetThumbnailServiceResult GenerateModelThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation,
    const AssetThumbnailCancelToken& cancelToken)
{
    const auto previewRequest = ResolveDeferredThumbnailPreviewRequest(request);
    const auto metadataRequest = BuildResolvedThumbnailCacheRequest(request, previewRequest);
    const auto meshPaths = ResolveMeshArtifactPaths(previewRequest);
    if (meshPaths.empty())
    {
        return GenerateMeshBackedThumbnail(
            request,
            evaluation,
            "thumbnail-model-mesh-artifact-missing",
            cancelToken,
            &metadataRequest);
    }
    return GenerateMeshSetThumbnail(
        request,
        evaluation,
        meshPaths,
        "thumbnail-model-mesh-artifact-missing",
        cancelToken,
        &metadataRequest);
}

AssetThumbnailServiceResult GeneratePrefabThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation,
    const AssetThumbnailCancelToken& cancelToken)
{
    const auto previewRequest = ResolveDeferredThumbnailPreviewRequest(request);
    const auto metadataRequest = BuildResolvedThumbnailCacheRequest(request, previewRequest);
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    if (!IsThumbnailRequestStillFresh(request, &evaluation))
        return BuildStaleThumbnailRequestResult(request, evaluation);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);

    const auto TryResidentSnapshot = [
        &request,
        &previewRequest,
        &evaluation,
        &cancelToken](
        const std::string& runtimeCacheIdentity,
        const std::string& freshnessFingerprint,
        const std::weak_ptr<const PreviewRenderableSnapshot>& weakSnapshot,
        const std::weak_ptr<ResidentPrefabPreviewRegistry>& weakRegistry)
        -> std::optional<AssetThumbnailServiceResult>
    {
        if (runtimeCacheIdentity.empty() || freshnessFingerprint.empty())
            return std::nullopt;

        std::optional<ResidentPrefabPreviewRegistry::Lease> residentLease;
        std::shared_ptr<const PreviewRenderableSnapshot> snapshot;
        if (const auto registry = weakRegistry.lock(); registry != nullptr)
        {
            residentLease = registry->Acquire(
                runtimeCacheIdentity,
                freshnessFingerprint,
                true);
            if (residentLease.has_value())
                snapshot = residentLease->Snapshot();
        }
        else
        {
            snapshot = weakSnapshot.lock();
        }

        if (snapshot == nullptr)
            return std::nullopt;

        if (auto snapshotResult = TryGeneratePrefabSnapshotThumbnail(
                request,
                previewRequest,
                *snapshot,
                evaluation,
                cancelToken);
            snapshotResult.has_value())
        {
            snapshotResult->previewQuality = ThumbnailPreviewQuality::ResidentSnapshot;
            return *snapshotResult;
        }
        return std::nullopt;
    };

    if (request.residentPrefabPreviewSource.has_value())
    {
        const auto& residentSource = *request.residentPrefabPreviewSource;
        if (residentSource.HasIdentity())
        {
            // When the request already carries a resolved artifact, rebuild the
            // resident lookup fingerprint from that canonical path. For the
            // zero-artifact-read path previewRequest may intentionally still
            // have an empty artifactPath; the original source below is then
            // checked with its exact identity/fingerprint contract.
            if (const auto registry = residentSource.registry.lock(); registry != nullptr &&
                !previewRequest.artifactPath.empty())
            {
                const auto canonicalSubAssetKey = BuildCanonicalPrefabPreviewSubAssetKey(
                    previewRequest.sourceAssetPath,
                    previewRequest.subAssetKey);
                const auto resolvedRuntimeCacheIdentity = BuildResidentPrefabRuntimeCacheIdentity(
                    previewRequest.assetId.ToString(),
                    canonicalSubAssetKey);
                const auto freshnessSubAssetKey = previewRequest.subAssetKey.empty()
                    ? canonicalSubAssetKey
                    : previewRequest.subAssetKey;
                const auto resolvedFreshnessFingerprint = BuildPrefabThumbnailDependencyStamp(
                    previewRequest.projectRoot,
                    previewRequest.assetId,
                    previewRequest.sourceAssetPath,
                    freshnessSubAssetKey,
                    previewRequest.artifactPath);
                if (auto snapshotResult = TryResidentSnapshot(
                        resolvedRuntimeCacheIdentity,
                        resolvedFreshnessFingerprint,
                        {},
                        registry);
                    snapshotResult.has_value())
                {
                    return *snapshotResult;
                }
            }

            if (auto snapshotResult = TryResidentSnapshot(
                    residentSource.runtimeCacheIdentity,
                    residentSource.freshnessFingerprint,
                    residentSource.snapshot,
                    residentSource.registry);
                snapshotResult.has_value())
            {
                return *snapshotResult;
            }
        }
    }

    // A scene snapshot is checked first above. For a non-resident request,
    // reuse the shared scene/drag-drop Prefab repository before reading the
    // artifact payload through the legacy thumbnail-only path.
    if (const auto sharedSnapshot = TryBuildSharedPrefabPreviewSnapshotForThumbnail(previewRequest);
        sharedSnapshot.has_value())
    {
        if (auto snapshotResult = TryGeneratePrefabSnapshotThumbnail(
                request,
                previewRequest,
                *sharedSnapshot,
                evaluation,
                cancelToken);
            snapshotResult.has_value())
        {
            return *snapshotResult;
        }
    }

    const auto previewPath = ResolvePreviewArtifactOrSourcePath(previewRequest);
    if (!previewPath.has_value())
    {
        result.diagnostic = IsMissingThumbnailArtifactPath(request)
            ? "thumbnail-prefab-artifact-missing"
            : "thumbnail-prefab-artifact-path-invalid";
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &metadataRequest);
        return result;
    }

    if (StructurePreviewArtifactExceedsBudget(
            *previewPath,
            NLS::Core::Assets::ArtifactType::Prefab,
            1u))
    {
        if (ShouldRetryLegacyImportedPrefabBudgetFailure(previewRequest))
        {
            const auto meshPaths = ResolveMeshArtifactPaths(previewRequest);
            if (!meshPaths.empty())
            {
                return GenerateMeshSetThumbnail(
                    request,
                    evaluation,
                    meshPaths,
                    "thumbnail-prefab-preview-mesh-artifact-missing",
                    cancelToken,
                    &metadataRequest);
            }
        }

        result.status = AssetThumbnailServiceStatus::Fallback;
        result.diagnostic = kPrefabPreviewBudgetExceededDiagnostic;
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &metadataRequest);
        return result;
    }

    const auto payload = ReadNativeOrPlainTextArtifact(
        *previewPath,
        NLS::Core::Assets::ArtifactType::Prefab,
        1u);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);
    if (!payload.has_value())
    {
        result.diagnostic = "thumbnail-prefab-artifact-read-failed";
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &metadataRequest);
        return result;
    }

    if (const auto snapshot = BuildPrefabPreviewSnapshotForThumbnail(previewRequest, *payload);
        snapshot.has_value())
    {
        if (auto snapshotResult = TryGeneratePrefabSnapshotThumbnail(
                request,
                previewRequest,
                *snapshot,
                evaluation,
                cancelToken);
            snapshotResult.has_value())
        {
            return *snapshotResult;
        }

        if (!snapshot->drawItems.empty())
        {
            result.status = AssetThumbnailServiceStatus::Fallback;
            result.diagnostic = "thumbnail-prefab-preview-mesh-artifact-missing";
            WriteThumbnailMetadataForEvaluation(
                request,
                evaluation,
                AssetThumbnailCacheStatus::Failed,
                result.diagnostic,
                &metadataRequest);
            return result;
        }
    }

    return WriteThumbnailPngResult(
        request,
        evaluation,
        RenderPrefabStructureThumbnail(*payload, request.requestedSize),
        "thumbnail-prefab-preview-generation-failed",
        cancelToken,
        &metadataRequest);
}

AssetThumbnailServiceResult GenerateTextureThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation,
    const AssetThumbnailCancelToken& cancelToken)
{
    const auto previewRequest = ResolveDeferredThumbnailPreviewRequest(request);
    const auto metadataRequest = BuildResolvedThumbnailCacheRequest(request, previewRequest);
    const auto generationSize = GetTextureThumbnailGenerationSize(request);
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    if (!IsThumbnailRequestStillFresh(request, &evaluation))
        return BuildStaleThumbnailRequestResult(request, evaluation);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);

    if (!evaluation.entry.has_value())
    {
        result.diagnostic = evaluation.diagnostic.empty()
            ? "thumbnail-cache-path-invalid"
            : evaluation.diagnostic;
        return result;
    }

    // A readable source image has the canonical pixels needed for a thumbnail.
    // Prefer it even when the browser item is marked as a GeneratedSubAsset:
    // importers may expose a generated record for an image that is still
    // directly readable at sourceAssetPath. This avoids an unnecessary
    // artifact read and keeps the source image usable when that artifact is
    // delayed or unavailable. Embedded textures whose source is not an image
    // naturally fall through to the artifact/dependency path below.
    const auto generateFromSource = [&](const std::filesystem::path& candidate)
        -> std::optional<AssetThumbnailServiceResult>
    {
        if (candidate.empty() || !IsTextureThumbnailSourceExtension(candidate))
            return std::nullopt;

        std::error_code sourceError;
        if (!std::filesystem::is_regular_file(candidate, sourceError) || sourceError)
            return std::nullopt;

        sourceError.clear();
        const auto sourceSize = std::filesystem::file_size(candidate, sourceError);
        if (sourceError || sourceSize > kMaxSourceThumbnailImageBytes)
            return std::nullopt;

        const auto dimensions = ReadImageHeaderDimensions(candidate);
        if ((dimensions.has_value() && ImageDimensionsExceedPreviewBudget(*dimensions)) ||
            (!dimensions.has_value() && IsKnownSourceImageExtension(candidate)))
        {
            return std::nullopt;
        }

        NLS::Image sourceImage(candidate.string(), false);
        if (IsThumbnailGenerationCancelled(cancelToken))
            return BuildCancelledThumbnailRequestResult(request, evaluation);
        if (sourceImage.GetData() == nullptr ||
            sourceImage.GetWidth() <= 0 ||
            sourceImage.GetHeight() <= 0 ||
            sourceImage.GetChannels() <= 0)
        {
            return std::nullopt;
        }

        const auto thumbnail = DownsampleImageToThumbnail(sourceImage, generationSize);
        if (thumbnail.pixels.empty() || thumbnail.width == 0u || thumbnail.height == 0u)
            return std::nullopt;

        return WriteThumbnailPngResult(
            request,
            evaluation,
            thumbnail,
            "thumbnail-source-downsample-failed",
            cancelToken,
            &metadataRequest);
    };

    const auto sourcePath = ResolveThumbnailSourcePath(previewRequest);
    if (auto sourceResult = generateFromSource(sourcePath); sourceResult.has_value())
        return *sourceResult;

    std::error_code error;
    if (!previewRequest.artifactPath.empty())
    {
        const auto artifactPath = ResolveThumbnailArtifactPath(previewRequest);
        if (artifactPath.empty())
        {
            result.diagnostic = "thumbnail-texture-artifact-path-invalid";
            WriteThumbnailMetadataForEvaluation(
                request,
                evaluation,
                AssetThumbnailCacheStatus::Failed,
                result.diagnostic,
                &metadataRequest);
            return result;
        }

        const auto textureHeader = NLS::Render::Assets::ReadTextureArtifactHeaderPreview(
            artifactPath,
            kMaxStructurePreviewArtifactPayloadBytes);
        if (!textureHeader.has_value() ||
            NativeArtifactFileExceedsThumbnailPreviewBudget(artifactPath))
        {
            result.diagnostic = "thumbnail-texture-artifact-unsupported";
            result.status = AssetThumbnailServiceStatus::Fallback;
            WriteThumbnailMetadataForEvaluation(
                request,
                evaluation,
                AssetThumbnailCacheStatus::Failed,
                result.diagnostic,
                &metadataRequest);
            return result;
        }
        if (ImageDimensionsExceedPreviewBudget({textureHeader->width, textureHeader->height}))
        {
            result.status = AssetThumbnailServiceStatus::Fallback;
            result.diagnostic = kSourcePreviewBudgetExceededDiagnostic;
            WriteThumbnailMetadataForEvaluation(
                request,
                evaluation,
                AssetThumbnailCacheStatus::Failed,
                result.diagnostic,
                &metadataRequest);
            return result;
        }

        const auto textureArtifact = NLS::Render::Assets::LoadTextureArtifact(artifactPath);
        if (IsThumbnailGenerationCancelled(cancelToken))
            return BuildCancelledThumbnailRequestResult(request, evaluation);
        if (!textureArtifact.has_value() ||
            !IsRgba8TextureArtifactMipUsable(*textureArtifact))
        {
            if (sourcePath.empty() || !IsTextureThumbnailSourceExtension(sourcePath))
            {
                if (auto dependencySourcePath = ResolveTextureSourceDependencyPath(previewRequest);
                    dependencySourcePath.has_value())
                {
                    if (auto sourceResult = generateFromSource(*dependencySourcePath);
                        sourceResult.has_value())
                    {
                        return *sourceResult;
                    }
                }
            }
            else if (auto sourceResult = generateFromSource(sourcePath); sourceResult.has_value())
                return *sourceResult;

            result.diagnostic = "thumbnail-texture-artifact-unsupported";
            result.status = AssetThumbnailServiceStatus::Fallback;
            WriteThumbnailMetadataForEvaluation(
                request,
                evaluation,
                AssetThumbnailCacheStatus::Failed,
                result.diagnostic,
                &metadataRequest);
            return result;
        }

        std::filesystem::create_directories(evaluation.entry->imagePath.parent_path(), error);
        if (error)
        {
            result.diagnostic = "thumbnail-cache-directory-create-failed";
            WriteThumbnailMetadataForEvaluation(
                request,
                evaluation,
                AssetThumbnailCacheStatus::Failed,
                result.diagnostic,
                &metadataRequest);
            return result;
        }

        const auto* mip = SelectTextureThumbnailMip(*textureArtifact, generationSize);
        if (mip == nullptr)
        {
            result.diagnostic = "thumbnail-texture-artifact-unsupported";
            result.status = AssetThumbnailServiceStatus::Fallback;
            WriteThumbnailMetadataForEvaluation(
                request,
                evaluation,
                AssetThumbnailCacheStatus::Failed,
                result.diagnostic,
                &metadataRequest);
            return result;
        }
        return WriteThumbnailPngResult(
            request,
            evaluation,
            DownsampleRgba8ToThumbnail(
                mip->PixelData(),
                mip->width,
                mip->height,
                mip->rowPitch,
                generationSize),
            "thumbnail-texture-artifact-downsample-failed",
            cancelToken,
            &metadataRequest);
    }

    if (sourcePath.empty())
    {
        result.diagnostic = "thumbnail-source-path-invalid";
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &metadataRequest);
        return result;
    }

    if (!IsTextureThumbnailSourceExtension(sourcePath))
    {
        result.diagnostic = "thumbnail-texture-extension-unsupported";
        result.status = AssetThumbnailServiceStatus::Fallback;
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &metadataRequest);
        return result;
    }

    if (!std::filesystem::is_regular_file(sourcePath, error) || error)
    {
        result.diagnostic = "thumbnail-source-missing";
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &metadataRequest);
        return result;
    }

    error.clear();
    const auto sourceSize = std::filesystem::file_size(sourcePath, error);
    if (!error && sourceSize > kMaxSourceThumbnailImageBytes)
    {
        result.status = AssetThumbnailServiceStatus::Fallback;
        result.diagnostic = kSourcePreviewBudgetExceededDiagnostic;
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &metadataRequest);
        return result;
    }

    const auto dimensions = ReadImageHeaderDimensions(sourcePath);
    if ((dimensions.has_value() && ImageDimensionsExceedPreviewBudget(*dimensions)) ||
        (!dimensions.has_value() && IsKnownSourceImageExtension(sourcePath)))
    {
        result.status = AssetThumbnailServiceStatus::Fallback;
        result.diagnostic = kSourcePreviewBudgetExceededDiagnostic;
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &metadataRequest);
        return result;
    }

    NLS::Image sourceImage(sourcePath.string(), false);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);
    if (sourceImage.GetData() == nullptr ||
        sourceImage.GetWidth() <= 0 ||
        sourceImage.GetHeight() <= 0 ||
        sourceImage.GetChannels() <= 0)
    {
        result.diagnostic = "thumbnail-source-decode-failed";
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &metadataRequest);
        return result;
    }

    const auto thumbnail = DownsampleImageToThumbnail(sourceImage, generationSize);
    if (thumbnail.pixels.empty() || thumbnail.width == 0u || thumbnail.height == 0u)
    {
        result.diagnostic = "thumbnail-source-downsample-failed";
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            result.diagnostic,
            &metadataRequest);
        return result;
    }

    return WriteThumbnailPngResult(
        request,
        evaluation,
        thumbnail,
        "thumbnail-source-downsample-failed",
        cancelToken,
        &metadataRequest);
}

AssetThumbnailServiceResult GenerateUnsupportedPreviewThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation,
    const AssetThumbnailCancelToken&)
{
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Fallback);

    result.diagnostic = UnsupportedDiagnosticForKind(request.kind);
    return result;
}

AssetThumbnailServiceResult GenerateThumbnailForRequest(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCancelToken& cancelToken)
{
    const auto evaluation = EvaluateAssetThumbnailCache(request);
    if (evaluation.status == AssetThumbnailCacheStatus::Fresh)
        return BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Fresh);

    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);

    if (const auto generator = GeneratorForKind(request.kind);
        generator != nullptr)
    {
        return generator(request, evaluation, cancelToken);
    }

    if (SupportsGpuThumbnailPreview(request))
    {
        auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Pending);
        result.diagnostic = UnsupportedDiagnosticForKind(request.kind);
        return result;
    }

    return GenerateUnsupportedPreviewThumbnail(request, evaluation, cancelToken);
}

AssetThumbnailServiceResult BuildExceptionThumbnailResult(
    const AssetThumbnailRequest& request,
    const std::string& diagnostic)
{
    AssetThumbnailServiceResult result;
    result.status = AssetThumbnailServiceStatus::Failed;
    InitializeThumbnailResultIdentity(request, result);
    result.diagnostic = diagnostic;
    std::string currentCacheKey;
    try
    {
        const auto evaluation = EvaluateAssetThumbnailCache(request);
        result.cacheEntry = evaluation.entry;
        if (evaluation.entry.has_value())
            currentCacheKey = evaluation.entry->cacheKey;
        if (evaluation.entry.has_value())
        {
            try
            {
                (void)WriteAssetThumbnailCacheMetadata(
                    request,
                    AssetThumbnailCacheStatus::Failed,
                    result.diagnostic);
            }
            catch (...)
            {
            }
        }
    }
    catch (...)
    {
    }
    AttachRetainedThumbnailImage(request, currentCacheKey, result);
    SynchronizeThumbnailResultPresentationState(result);
    return result;
}

AssetThumbnailServiceResult TryGenerateThumbnailForRequest(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCancelToken& cancelToken)
{
    try
    {
        return GenerateThumbnailForRequest(request, cancelToken);
    }
    catch (const std::bad_alloc&)
    {
        return BuildExceptionThumbnailResult(request, "thumbnail-generation-out-of-memory");
    }
    catch (...)
    {
        return BuildExceptionThumbnailResult(request, "thumbnail-generation-exception");
    }
}

std::string ItemFreshnessIdentity(
    const AssetBrowserItem& item,
    const NLS::Core::Assets::AssetId assetId,
    const AssetThumbnailRequest& request)
{
    auto appendPart = [](std::string& result, const char* label, const std::string& value)
    {
        result += label;
        result.push_back('=');
        result += std::to_string(value.size());
        result.push_back(':');
        result += value;
        result.push_back('|');
    };

    const bool canonicalPrefabPreview =
        request.kind == AssetThumbnailKind::PrefabPreview &&
        assetId.IsValid() &&
        !request.subAssetKey.empty() &&
        !request.artifactPath.empty();

    std::string result;
    appendPart(result, "source", item.sourceAssetPath);
    appendPart(result, "subAsset", canonicalPrefabPreview ? request.subAssetKey : item.subAssetKey);
    appendPart(result, "assetId", assetId.ToString());
    if (canonicalPrefabPreview)
    {
        appendPart(result, "itemKind", "prefab-artifact");
        appendPart(result, "type", std::to_string(static_cast<int>(AssetBrowserItemType::Prefab)));
        appendPart(result, "artifactType", std::to_string(static_cast<int>(NLS::Core::Assets::ArtifactType::Prefab)));
        // The same artifact can enter the request from the asset database as a
        // project-relative path or from the scene hot cache as an absolute
        // path. Freshness must describe the physical canonical artifact, not
        // the spelling used by the producer.
        const auto resolvedArtifactPath = ResolveThumbnailArtifactPath(request);
        appendPart(
            result,
            "artifactPath",
            resolvedArtifactPath.empty()
                ? std::filesystem::path(request.artifactPath).lexically_normal().generic_string()
                : resolvedArtifactPath.generic_string());
    }
    else
    {
        appendPart(result, "kind", std::to_string(static_cast<int>(item.kind)));
        appendPart(result, "type", std::to_string(static_cast<int>(item.type)));
        appendPart(result, "artifactType", std::to_string(static_cast<int>(item.artifactType)));
    }
    return result;
}

std::optional<NLS::Core::Assets::AssetId> LoadSourceAssetIdFromMeta(
    const std::filesystem::path& projectRoot,
    const std::string& sourceAssetPath)
{
    const auto sourcePath = ResolveEditorAssetPath(
        MakeProjectEditorAssetRoots(projectRoot),
        sourceAssetPath);
    if (sourcePath.empty())
        return std::nullopt;

    const auto meta = NLS::Core::Assets::AssetMeta::Load(
        NLS::Core::Assets::GetAssetMetaPath(sourcePath));
    if (!meta.has_value() || !meta->id.IsValid())
        return std::nullopt;

    return meta->id;
}

std::optional<AssetThumbnailRequest> BuildAssetThumbnailRequestForItemWithContext(
    const std::filesystem::path& projectRoot,
    const AssetBrowserItem& item,
    uint32_t requestedSize,
    AssetThumbnailRequestBuildContext* context);
}

#if defined(NLS_ENABLE_TEST_HOOKS)
void ResetAssetThumbnailManifestLookupStatsForTesting()
{
    g_thumbnailManifestLookupCountForTesting.store(0u, std::memory_order_relaxed);
    g_thumbnailManifestMainThreadLookupCountForTesting.store(0u, std::memory_order_relaxed);
    g_thumbnailManifestBackgroundThreadLookupCountForTesting.store(0u, std::memory_order_relaxed);
}

AssetThumbnailManifestLookupStatsForTesting GetAssetThumbnailManifestLookupStatsForTesting()
{
    return {
        g_thumbnailManifestLookupCountForTesting.load(std::memory_order_relaxed),
        g_thumbnailManifestMainThreadLookupCountForTesting.load(std::memory_order_relaxed),
        g_thumbnailManifestBackgroundThreadLookupCountForTesting.load(std::memory_order_relaxed)
    };
}

void ResetAssetThumbnailFreshnessInputCheckCountForTesting()
{
    g_freshnessInputCheckCountForTesting.store(0u, std::memory_order_relaxed);
}

size_t GetAssetThumbnailFreshnessInputCheckCountForTesting()
{
    return g_freshnessInputCheckCountForTesting.load(std::memory_order_relaxed);
}

bool ShouldRefreshGpuPreviewResourceProgressForTesting(
    const uint64_t previousProgressToken,
    const uint64_t progressToken,
    const bool resourceWorkActive)
{
    return ShouldRefreshGpuPreviewResourceProgress(
        previousProgressToken,
        progressToken,
        resourceWorkActive);
}

ThumbnailFormalLODSelectionForTesting LoadThumbnailFormalLODForTesting(
    const std::filesystem::path& path)
{
    ThumbnailFormalLODSelectionForTesting result;
    const auto header = NLS::Render::Assets::ReadMeshArtifactHeaderPreview(
        path,
        kMaxStructurePreviewArtifactPayloadBytes);
    if (!header.has_value())
        return result;
    const auto mesh = LoadMeshArtifactForThumbnailPreview(path, *header);
    if (!mesh.has_value())
        return result;
    result.loaded = true;
    result.materialIndex = mesh->materialIndex;
    result.vertexCount = mesh->vertices.size();
    result.indexCount = mesh->indices.size();
    return result;
}
#endif

std::optional<AssetThumbnailRequest> BuildAssetThumbnailRequestForItem(
    const std::filesystem::path& projectRoot,
    const AssetBrowserItem& item,
    const uint32_t requestedSize)
{
    return BuildAssetThumbnailRequestForItemWithContext(projectRoot, item, requestedSize, nullptr);
}

std::optional<AssetThumbnailRequest> BuildAssetThumbnailRequestForItem(
    const std::filesystem::path& projectRoot,
    const AssetBrowserItem& item,
    const uint32_t requestedSize,
    AssetThumbnailRequestBuildContext& context)
{
    return BuildAssetThumbnailRequestForItemWithContext(projectRoot, item, requestedSize, &context);
}

std::string BuildPrefabThumbnailDependencyStamp(
    const std::filesystem::path& projectRoot,
    const NLS::Core::Assets::AssetId& assetId,
    const std::string& sourceAssetPath,
    const std::string& subAssetKey,
    const std::string& artifactPath)
{
    AssetThumbnailRequest request;
    request.projectRoot = projectRoot;
    request.assetId = assetId;
    request.sourceAssetPath = sourceAssetPath;
    request.subAssetKey = subAssetKey;
    request.artifactPath = artifactPath;
    request.kind = AssetThumbnailKind::PrefabPreview;

    AssetBrowserItem item;
    item.sourceAssetPath = sourceAssetPath;
    item.subAssetKey = subAssetKey;
    request.freshnessInputs.push_back({
        "item",
        ItemFreshnessIdentity(item, assetId, request)
    });
    AddSourceFreshnessInputs(request, nullptr);
    AddArtifactFreshnessInputs(request, item, nullptr);
    return BuildThumbnailDependencyStamp(request.freshnessInputs);
}

namespace
{
std::optional<AssetThumbnailRequest> BuildAssetThumbnailRequestForItemWithContext(
    const std::filesystem::path& projectRoot,
    const AssetBrowserItem& item,
    const uint32_t requestedSize,
    AssetThumbnailRequestBuildContext* context)
{
    NLS_PROFILE_NAMED_SCOPE("AssetThumbnailService::BuildRequestForItem");
    {
        ScopedThumbnailRequestBuildTelemetry validateTelemetry {
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestValidate,
            item
        };
        if (projectRoot.empty() ||
            item.kind == AssetBrowserItemKind::Folder ||
            item.sourceAssetPath.empty())
        {
            return std::nullopt;
        }
    }

    auto assetId = item.assetId;
    if (!assetId.IsValid())
    {
        ScopedThumbnailRequestBuildTelemetry metaTelemetry {
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestMetaId,
            item
        };
        const auto metaAssetId = LoadSourceAssetIdFromMeta(projectRoot, item.sourceAssetPath);
        if (!metaAssetId.has_value())
            return std::nullopt;
        assetId = *metaAssetId;
    }

    AssetThumbnailRequest request;
    request.projectRoot = projectRoot;
    if (context != nullptr)
    {
        request.cacheRoot = context->featureConfig.cacheRoot;
        request.enableReadbackRing = context->featureConfig.readbackRing;
        request.enablePreviewProxyPool = context->featureConfig.previewProxyPool;
    }
    request.assetId = assetId;
    request.sourceAssetPath = item.sourceAssetPath;
    request.subAssetKey = item.subAssetKey;
    request.artifactPath = item.artifactPath;
    request.generatedSubAsset = item.kind == AssetBrowserItemKind::GeneratedSubAsset;
    if (context != nullptr)
        request.assetDatabaseSnapshot = context->assetDatabaseSnapshot;

    // Keep the imported sub-resource identity available for presentation
    // coalescing even when the canonical generation path later drops the
    // artifact-backed request in favor of the readable source image.
    const std::string originalTexturePresentationSubAssetKey = request.subAssetKey;
    bool normalizedDirectSourceTexture = false;

    // A source image is already the canonical texture payload. Imported image
    // records can still carry texture:main plus an artifact path; retaining
    // that identity makes the browser wait on a second artifact request even
    // though the source can be decoded directly. Normalize only when the
    // source is known to be readable and within the existing decode budget so
    // embedded or oversized textures keep their artifact fallback.
    std::string texturePresentationSubAssetKey = originalTexturePresentationSubAssetKey;
    if (item.type == AssetBrowserItemType::Texture)
    {
        const auto sourcePath = ResolveThumbnailSourcePathCached(request, context);
        if (IsDirectReadableTextureThumbnailSource(sourcePath))
        {
            request.subAssetKey.clear();
            request.artifactPath.clear();
            request.generatedSubAsset = false;
            request.directSourceTexture = true;
            normalizedDirectSourceTexture = true;

            // Source texture rows commonly have no sub-asset key, while the
            // imported row for the same asset uses the manifest primary key.
            // Read that stable identity from the enumeration snapshot only for
            // presentation coalescing. The source request must remain a direct
            // decode request and must not inherit artifact lookup state.
            if (texturePresentationSubAssetKey.empty() &&
                context != nullptr &&
                context->assetDatabaseSnapshot != nullptr)
            {
                const auto manifest = context->assetDatabaseSnapshot->GetArtifactManifestForAssetPath(
                    request.sourceAssetPath);
                if (manifest.has_value() && !manifest->primarySubAssetKey.empty())
                {
                    texturePresentationSubAssetKey = manifest->primarySubAssetKey;
                }
            }
        }
    }

    // A deferred thumbnail scope may still have an immutable database snapshot
    // from the current Asset Browser enumeration. Reuse its in-memory manifest
    // for resident prefab matching so the request carries the same canonical
    // artifact identity and freshness inputs as the scene lease.
    if (context != nullptr &&
        context->featureConfig.residentPrefabPreview &&
        context->assetDatabaseSnapshot != nullptr &&
        (request.subAssetKey.empty() || request.artifactPath.empty()) &&
        (item.type == AssetBrowserItemType::Model ||
            item.type == AssetBrowserItemType::Prefab))
    {
        const auto manifest = context->assetDatabaseSnapshot->GetArtifactManifestForAssetPath(
            request.sourceAssetPath);
        if (manifest.has_value())
        {
            if (const auto* artifact = FindThumbnailArtifactForItem(*manifest, item);
                artifact != nullptr)
            {
                if (request.subAssetKey.empty())
                    request.subAssetKey = artifact->subAssetKey;
                if (request.artifactPath.empty())
                    request.artifactPath = artifact->artifactPath;
            }
        }
    }
    if ((request.subAssetKey.empty() || request.artifactPath.empty()) &&
        (context == nullptr || !context->deferManifestLookups))
    {
        std::optional<NLS::Core::Assets::ArtifactManifest> localManifest;
        const NLS::Core::Assets::ArtifactManifest* manifest = nullptr;
        {
            ScopedThumbnailRequestBuildTelemetry manifestTelemetry {
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestManifestLookup,
                item
            };
            const auto* cachedManifest = LoadThumbnailArtifactManifestCached(request, context);
            if (cachedManifest != nullptr)
            {
                if (cachedManifest->has_value())
                    manifest = &**cachedManifest;
            }
            else
            {
                localManifest = LoadThumbnailArtifactManifest(request);
                if (localManifest.has_value())
                    manifest = &*localManifest;
            }
        }
        if (manifest != nullptr)
        {
            const auto* artifact = FindThumbnailArtifactForItem(*manifest, item);
            if (artifact != nullptr && request.subAssetKey.empty())
                request.subAssetKey = artifact->subAssetKey;
            if (artifact != nullptr && request.artifactPath.empty())
                request.artifactPath = artifact->artifactPath;
        }
    }
    const bool canUseCanonicalSubAssetFallback =
        context == nullptr ||
        !context->deferManifestLookups ||
        context->residentPrefabPreviewRegistry != nullptr;
    if (request.subAssetKey.empty() &&
        canUseCanonicalSubAssetFallback &&
        (item.type == AssetBrowserItemType::Model ||
            item.type == AssetBrowserItemType::Prefab ||
            item.type == AssetBrowserItemType::Material))
    {
        const auto stem = std::filesystem::path(item.sourceAssetPath).stem().generic_string();
        if (!stem.empty())
        {
            request.subAssetKey = item.type == AssetBrowserItemType::Material
                ? "material:" + stem
                : "prefab:" + stem;
        }
    }
    if (item.kind == AssetBrowserItemKind::GeneratedSubAsset ||
        item.type == AssetBrowserItemType::Model ||
        item.type == AssetBrowserItemType::Prefab)
    {
        request.artifactPath = request.artifactPath.empty() ? item.artifactPath : request.artifactPath;
    }
    request.kind = ThumbnailKindForItem(item);
    request.requestedSize = request.kind == AssetThumbnailKind::Texture
        ? (std::min)(std::max(1u, requestedSize), kMaxTextureThumbnailGenerationSize)
        : std::max(1u, requestedSize);
    request.previewRendererVersion = request.kind == AssetThumbnailKind::PrefabPreview
        ? kUpperObliqueGpuPrefabThumbnailRendererVersion
        : request.kind == AssetThumbnailKind::MaterialSphere
        ? kPbrMaterialThumbnailRendererVersion
        : SupportsGpuThumbnailPreview(request)
            ? kUpperObliqueGpuThumbnailRendererVersion
            : request.kind == AssetThumbnailKind::ModelPreview
                ? kUpperObliqueCpuThumbnailRendererVersion
                : kLegacyThumbnailRendererVersion;
    if (request.kind == AssetThumbnailKind::Texture)
    {
        request.settingsFingerprint = "asset-browser-thumbnail:v15-lowres-image-thumbnails";
    }
    else if (request.kind == AssetThumbnailKind::PrefabPreview)
    {
        request.settingsFingerprint = "asset-browser-thumbnail:v37-prefab-complete-material-fallback";
    }
    else
    {
        request.settingsFingerprint = "asset-browser-thumbnail:v20-gpu-black-frame-fallback";
    }

    {
        ScopedThumbnailRequestBuildTelemetry identityTelemetry {
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestItemIdentity,
            item
        };
        request.freshnessInputs.push_back({
            "item",
            ItemFreshnessIdentity(item, assetId, request)
        });
    }
    {
        ScopedThumbnailRequestBuildTelemetry sourceFreshnessTelemetry {
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestSourceFreshness,
            item
        };
        AddSourceFreshnessInputs(request, context);
    }
    {
        ScopedThumbnailRequestBuildTelemetry artifactFreshnessTelemetry {
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestArtifactFreshness,
            item
        };
        AddArtifactFreshnessInputs(request, item, context);
    }
    {
        ScopedThumbnailRequestBuildTelemetry dependencyStampTelemetry {
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequestDependencyStamp,
            item,
            request.freshnessInputs.size()
        };
        request.dependencyStamp = BuildThumbnailDependencyStamp(request.freshnessInputs);
    }
    request.colorSpaceMode = request.kind == AssetThumbnailKind::Texture ? "linear" : "srgb";
    request.hdrMode = "ldr";
    if (normalizedDirectSourceTexture)
    {
        auto presentationRequest = request;
        if (!texturePresentationSubAssetKey.empty())
            presentationRequest.subAssetKey = texturePresentationSubAssetKey;
        request.presentationKey = BuildAssetThumbnailPresentationKey(presentationRequest);
    }
    if (request.kind == AssetThumbnailKind::PrefabPreview &&
        context != nullptr && context->residentPrefabPreviewRegistry != nullptr)
    {
        // Scene restore normalizes importer aliases such as model:<name> to
        // the canonical prefab:<source-stem> identity. Keep the request's
        // original sub-resource key for artifact lookup, but use the same
        // canonical key for resident snapshot discovery.
        const auto residentSubAssetKey = BuildCanonicalPrefabPreviewSubAssetKey(
            request.sourceAssetPath,
            request.subAssetKey);
        const auto runtimeCacheIdentity = BuildResidentPrefabRuntimeCacheIdentity(
            request.assetId.ToString(),
            residentSubAssetKey);
        // Keep the importer row's freshness identity on the request.  The
        // registry stores canonical scene entries and supplies aliases for
        // importer spellings, while the request/cache identity must remain
        // stable with the Asset Browser row that created it.
        const auto residentFreshnessFingerprint = request.dependencyStamp.empty()
            ? BuildPrefabThumbnailDependencyStamp(
                request.projectRoot,
                request.assetId,
                request.sourceAssetPath,
                request.subAssetKey.empty() ? residentSubAssetKey : request.subAssetKey,
                request.artifactPath)
            : request.dependencyStamp;
        const auto snapshot = context->residentPrefabPreviewRegistry->FindWeakSnapshot(
            runtimeCacheIdentity,
            residentFreshnessFingerprint);
        const auto residentState = context->residentPrefabPreviewRegistry->GetSnapshotState(
            runtimeCacheIdentity,
            residentFreshnessFingerprint);
        const bool importedThumbnailContinuation =
            context->residentPrefabPreviewRegistry->HasImportedPrefabThumbnailContinuation(
                request.projectRoot,
                request.assetId,
                request.sourceAssetPath);
        request.importedPrefabThumbnailContinuation = importedThumbnailContinuation;
        request.importedPrefabThumbnailContinuationRevision = importedThumbnailContinuation
            ? context->residentPrefabPreviewRegistry
                ->GetImportedPrefabThumbnailContinuationRevision(
                    request.projectRoot,
                    request.assetId,
                    request.sourceAssetPath)
            : 0u;
        // Keep the weak identity even when the snapshot is not present yet.
        // Scene restore and thumbnail request construction can race; the
        // service acquires the strong lease at dequeue time, which allows a
        // late scene registration to hit without putting a strong snapshot in
        // the request queue.
        request.residentPrefabPreviewSource = ResidentPrefabPreviewSource {
            runtimeCacheIdentity,
            residentFreshnessFingerprint,
            snapshot,
            context->residentPrefabPreviewRegistry,
            (residentState.has_value() && residentState->allowArtifactResourceLoading) ||
                importedThumbnailContinuation
        };
    }
    return request;
}
}

bool PromoteAssetThumbnailResultFromPresentationIndex(
    const AssetThumbnailRequest& request,
    AssetThumbnailServiceResult& result)
{
    return PromoteAssetThumbnailResultFromPresentationIndexImpl(request, result);
}

#if defined(NLS_ENABLE_TEST_HOOKS)
AssetThumbnailRequest ResolveDeferredThumbnailPreviewRequestForTesting(
    const AssetThumbnailRequest& request)
{
    return ResolveDeferredThumbnailPreviewRequest(request);
}
#endif

AssetThumbnailService::AssetThumbnailService(AssetThumbnailFeatureConfig featureConfig)
    : m_requestSessionId(MakeAssetThumbnailRequestSessionId()),
      m_featureConfig(std::move(featureConfig))
{
}

AssetThumbnailService::~AssetThumbnailService()
{
    Shutdown();
}

void AssetThumbnailService::Shutdown()
{
    if (m_shutdown)
        return;
    m_shutdown = true;

    if (m_generationCancelToken)
        m_generationCancelToken->cancelled.store(true, std::memory_order_relaxed);
    for (const auto& request : m_inFlightThumbnails)
    {
        if (request.cancelToken)
            request.cancelToken->cancelled.store(true, std::memory_order_relaxed);
    }
    m_generationCancelToken.reset();
    WaitForInFlightRequests();

    // These results can retain GPU textures, render inputs, preview snapshots,
    // and proxy-pool leases. Release them before the registry/renderer and the
    // rendering driver begin teardown.
    m_completedGpuPreviewResultsByCacheKey.clear();
    m_stableThumbnailResultsByCacheKey.clear();
    m_terminalThumbnailResultsByCacheKey.clear();
    m_gpuPreviewReadbackPendingCacheKeys.clear();
    m_gpuPreviewReadbackPendingRequestsByCacheKey.clear();
    m_gpuPreviewReadbackPendingDeferralsByCacheKey.clear();
    m_gpuPreviewReadbackCacheKeyByRequestKey.clear();
    m_gpuPreviewResourcePendingRequestsByCacheKey.clear();
    m_resolvedPreviewRequestsByCacheKey.clear();
    m_residentPreviewRequestsByPresentationKey.clear();
    m_queuedRequestsByCacheKey.clear();
    m_activeImportedPrefabThumbnailContinuationAssetId.clear();
    m_completedImportedPrefabThumbnailContinuationAssetIds.clear();
    m_importedPrefabThumbnailAttemptRevisionByCacheKey.clear();
}

AssetThumbnailServiceResult AssetThumbnailService::RequestAssetPreview(
    const AssetThumbnailRequest& request)
{
    return GetThumbnail(request);
}

AssetThumbnailServiceResult AssetThumbnailService::GetAssetPreview(
    const AssetThumbnailRequest& request)
{
    return GetThumbnail(request);
}

AssetThumbnailServiceResult AssetThumbnailService::GetMiniThumbnail(
    const AssetThumbnailRequest& request) const
{
    AssetThumbnailServiceResult result;
    result.status = AssetThumbnailServiceStatus::Fallback;
    result.fallbackIcon = FallbackIconForKind(request.kind);
    return result;
}

std::optional<AssetThumbnailService::CoalescibleThumbnailRequest>
AssetThumbnailService::FindCoalescibleActiveThumbnailRequest(
    const AssetThumbnailRequest& request) const
{
    std::optional<CoalescibleThumbnailRequest> best;
    const auto consider = [&](const std::string& cacheKey,
                              const AssetThumbnailRequest& candidate)
    {
        if (candidate.requestRevision == 0u ||
            (request.requestRevision != 0u &&
                candidate.requestRevision != request.requestRevision) ||
            !AreThumbnailRequestsCoalescible(request, candidate))
        {
            return;
        }

        const auto latest = m_latestPresentationRevisions.find(
            BuildAssetThumbnailPresentationKey(request));
        if (latest != m_latestPresentationRevisions.end() &&
            candidate.requestRevision < latest->second)
        {
            return;
        }

        if (!best.has_value() ||
            candidate.requestRevision > best->request.requestRevision)
        {
            best = CoalescibleThumbnailRequest {cacheKey, candidate};
        }
    };

    for (const auto& [cacheKey, candidate] : m_queuedRequestsByCacheKey)
        consider(cacheKey, candidate);
    for (const auto& [cacheKey, candidate] : m_gpuPreviewResourcePendingRequestsByCacheKey)
        consider(cacheKey, candidate);
    for (const auto& [cacheKey, candidate] : m_gpuPreviewReadbackPendingRequestsByCacheKey)
        consider(cacheKey, candidate);
    for (const auto& [cacheKey, candidate] : m_resolvedPreviewRequestsByCacheKey)
        consider(cacheKey, candidate);
    for (const auto& inFlight : m_inFlightThumbnails)
        consider(inFlight.cacheKey, inFlight.request);
    for (const auto& ticket : m_deferredPersistenceTickets)
        consider(ticket.cacheKey, ticket.request);
    return best;
}

bool AssetThumbnailService::IsLoadingAssetPreview(
    const AssetThumbnailRequest& request) const
{
    const auto state = GetThumbnailState(request);
    return state == ThumbnailState::Queued ||
        state == ThumbnailState::Preparing ||
        state == ThumbnailState::WaitingForResources ||
        state == ThumbnailState::Rendering ||
        state == ThumbnailState::WaitingForGpu ||
        IsActiveThumbnailReadbackOrPersistenceState(state);
}

AssetThumbnailServiceResult AssetThumbnailService::GetThumbnail(
    const AssetThumbnailRequest& inputRequest)
{
    AssetThumbnailRequest request = inputRequest;
    if (request.requestSessionId == 0u)
        request.requestSessionId = m_requestSessionId;
    if (!m_featureConfig.residentPrefabPreview)
        request.residentPrefabPreviewSource.reset();
    // The registry revision is scheduling state, not part of the durable
    // thumbnail identity. Refresh it before any cache/stable-result lookup so
    // a partial resident frame cannot outlive the scene resources that made it.
    RefreshResidentPreviewRequestState(request);
    if (request.residentPrefabPreviewSource.has_value())
    {
        const auto& residentSource = *request.residentPrefabPreviewSource;
        if (const auto registry = residentSource.registry.lock();
            registry != nullptr)
        {
            registry->RecordThumbnailRequest(
                residentSource.runtimeCacheIdentity,
                residentSource.freshnessFingerprint);
        }
    }
    request.enableReadbackRing = m_featureConfig.readbackRing;
    request.enablePreviewProxyPool = m_featureConfig.previewProxyPool;
    request.previewResourcePumpBudgetMicroseconds =
        m_thumbnailPreviewResourcePumpBudgetMicroseconds;
    if (request.presentationKey.empty())
        request.presentationKey = BuildAssetThumbnailPresentationKey(request);

    // Callers normally omit the revision. Keep that implicit revision stable
    // while the same freshness cache entry is queued, in flight, or retained;
    // otherwise a repeated UI lookup would supersede its own work instead of
    // coalescing with it.
    auto requestedCacheKey = BuildAssetThumbnailCacheKey(request);
    if (const auto coalesced = FindCoalescibleActiveThumbnailRequest(request);
        coalesced.has_value())
    {
        // Keep the caller's priority and resident weak handle, but route all
        // lookup and scheduling state through the existing cache-key owner.
        // This is the important bridge between an unresolved request and the
        // same request after its artifact manifest has been resolved.
        requestedCacheKey = coalesced->cacheKey;
        if (request.requestRevision == 0u)
            request.requestRevision = coalesced->request.requestRevision;
    }
    const auto findExistingRevision = [this, &requestedCacheKey]() -> std::optional<uint64_t>
    {
        if (const auto queued = m_queuedRequestsByCacheKey.find(requestedCacheKey);
            queued != m_queuedRequestsByCacheKey.end() && queued->second.requestRevision != 0u)
        {
            return queued->second.requestRevision;
        }
        if (const auto resourcePending =
                m_gpuPreviewResourcePendingRequestsByCacheKey.find(requestedCacheKey);
            resourcePending != m_gpuPreviewResourcePendingRequestsByCacheKey.end() &&
            resourcePending->second.requestRevision != 0u)
        {
            return resourcePending->second.requestRevision;
        }
        for (const auto& inFlight : m_inFlightThumbnails)
        {
            if (inFlight.cacheKey == requestedCacheKey && inFlight.request.requestRevision != 0u)
                return inFlight.request.requestRevision;
        }
        for (const auto& ticket : m_deferredPersistenceTickets)
        {
            if (ticket.cacheKey == requestedCacheKey && ticket.request.requestRevision != 0u)
                return ticket.request.requestRevision;
        }
        if (const auto pending = m_gpuPreviewReadbackPendingRequestsByCacheKey.find(requestedCacheKey);
            pending != m_gpuPreviewReadbackPendingRequestsByCacheKey.end() &&
            pending->second.requestRevision != 0u)
        {
            return pending->second.requestRevision;
        }
        if (const auto resolved = m_resolvedPreviewRequestsByCacheKey.find(requestedCacheKey);
            resolved != m_resolvedPreviewRequestsByCacheKey.end() &&
            resolved->second.requestRevision != 0u)
        {
            return resolved->second.requestRevision;
        }
        if (const auto stable = m_stableThumbnailResultsByCacheKey.find(requestedCacheKey);
            stable != m_stableThumbnailResultsByCacheKey.end() &&
            stable->second.requestRevision != 0u)
        {
            return stable->second.requestRevision;
        }
        return std::nullopt;
    };
    if (request.requestRevision == 0u)
    {
        if (const auto existingRevision = findExistingRevision(); existingRevision.has_value())
            request.requestRevision = *existingRevision;
        else
        {
            request.requestRevision = m_nextRequestRevision++;
            if (m_nextRequestRevision == 0u)
                m_nextRequestRevision = 1u;
        }
    }

    // A resident scene package can finish after the provisional request has
    // already reached the visible timeout. Completion is an independent
    // recovery signal: clear that temporary barrier so the same presentation
    // revision can be rendered canonically without waiting for a new asset
    // freshness revision.
    const bool residentPreviewComplete =
        request.residentPrefabPreviewSource.has_value() &&
        request.residentPrefabPreviewSource->HasIdentity() &&
        request.residentPreviewRevision != 0u &&
        !request.residentPreviewPartial;
    const auto presentationKey = BuildAssetThumbnailPresentationKey(request);
    if (request.residentPrefabPreviewSource.has_value() &&
        request.residentPrefabPreviewSource->HasIdentity() &&
        request.residentPreviewRevision != 0u &&
        !presentationKey.empty())
    {
        // This owner survives generation-scope queue rebuilds. It is only a
         // request identity; the actual mesh/material/texture package remains
         // owned by ResidentPrefabPreviewRegistry.
         m_residentPreviewRequestsByPresentationKey[presentationKey] = request;
    }
    else if (!presentationKey.empty())
    {
        // Asset Browser request construction can carry a resident lookup
        // identity for an unloaded parent or child asset. Without a registry
        // snapshot there is nothing to promote, so keeping that request here
        // would make every maintenance frame rescan it forever.
        m_residentPreviewRequestsByPresentationKey.erase(presentationKey);
    }
    if (residentPreviewComplete)
    {
        // Publish the latest owner before promotion. Queue/pending/resolved
        // tables can be rebuilt during a scene transition; in that case this
        // persistent owner is the only request identity available to promote.
        PromoteCompletedResidentPreviewOwners();
    }
    constexpr std::string_view kResourceTimeoutDiagnostic =
        "thumbnail-gpu-preview-resources-timeout:";
    constexpr std::string_view kVisibleTimeoutDiagnostic =
        "thumbnail-visible-request-timeout";
    const auto isResidentRecoveryDiagnostic = [=](const std::string& diagnostic)
    {
        return diagnostic.rfind(kResourceTimeoutDiagnostic, 0u) == 0u ||
            diagnostic == kVisibleTimeoutDiagnostic ||
            diagnostic == kLargePrefabPreviewAwaitingResidentDiagnostic;
    };
    bool residentTimeoutNeedsRecovery = false;
    if (residentPreviewComplete)
    {
        // A timeout result can already have been consumed by the UI, leaving
        // only the negative cache marker behind. Remember that marker before
        // clearing the terminal presentation barrier so the completed resident
        // package can re-enter the continuation lane below. The visible request
        // timeout is also recoverable here: it can be emitted while the initial
        // artifact request is waiting for the same scene-owned package that
        // later becomes resident.
        for (const auto& [_, terminal] : m_terminalThumbnailResultsByCacheKey)
        {
            if (terminal.presentationKey == presentationKey &&
                terminal.requestRevision <= request.requestRevision &&
                isResidentRecoveryDiagnostic(terminal.diagnostic))
            {
                residentTimeoutNeedsRecovery = true;
                break;
            }
        }
        if (const auto terminal = m_terminalPresentationRevisions.find(presentationKey);
            terminal != m_terminalPresentationRevisions.end() &&
            terminal->second <= request.requestRevision)
        {
            m_terminalPresentationRevisions.erase(terminal);
        }
        for (auto terminalIterator = m_terminalThumbnailResultsByCacheKey.begin();
             terminalIterator != m_terminalThumbnailResultsByCacheKey.end();)
        {
            if (terminalIterator->second.presentationKey == presentationKey &&
                terminalIterator->second.requestRevision <= request.requestRevision)
            {
                terminalIterator = m_terminalThumbnailResultsByCacheKey.erase(
                    terminalIterator);
            }
            else
            {
                ++terminalIterator;
            }
        }
    }

    // If the terminal result was consumed before the scene finished resolving,
    // recover from its durable timeout marker as well. This check is restricted
    // to a complete resident request, so ordinary failed GPU previews remain
    // terminal and are not retried on every UI lookup.
    if (residentPreviewComplete && !residentTimeoutNeedsRecovery)
    {
        const auto state = m_thumbnailStatesByCacheKey.find(requestedCacheKey);
        // The timeout result can be consumed and its in-memory state pruned
        // before the scene package completes. In that case the durable failure
        // metadata is the only recovery signal left, so inspect it even when
        // the old owner table no longer contains the cache key.
        if (state == m_thumbnailStatesByCacheKey.end() ||
            state->second == ThumbnailState::Failed ||
            state->second == ThumbnailState::Cancelled)
        {
            const auto evaluation = EvaluateAssetThumbnailCache(
                request,
                AssetThumbnailCacheIntegrityMode::Fast);
            residentTimeoutNeedsRecovery =
                evaluation.status == AssetThumbnailCacheStatus::Failed &&
                isResidentRecoveryDiagnostic(evaluation.diagnostic);
        }
    }

    // A partial result can outlive the ordinary queue while the UI keeps
    // querying the same presentation. Once the refreshed resident request is
    // complete, repair that owner directly so a maintenance-only frame still
    // reaches canonical rendering even if an earlier lane transition dropped
    // the pending-table entry.
    if (residentPreviewComplete)
    {
        const auto state = m_thumbnailStatesByCacheKey.find(requestedCacheKey);
        const bool recoverableResidentFailure = residentTimeoutNeedsRecovery &&
            (state == m_thumbnailStatesByCacheKey.end() ||
                state->second == ThumbnailState::Failed ||
                state->second == ThumbnailState::Cancelled);
        if (state == m_thumbnailStatesByCacheKey.end() ||
            state->second == ThumbnailState::WaitingForResources ||
            state->second == ThumbnailState::Queued ||
            recoverableResidentFailure)
        {
            if (recoverableResidentFailure)
            {
                // The old visible deadline belongs to the provisional
                // artifact request. Reset it before the complete resident
                // package enters the continuation lane, otherwise the next
                // maintenance pass can immediately recreate the same timeout.
                ClearVisibleThumbnailRequestStart(request);
                TrackVisibleThumbnailRequestStart(request);
            }
            m_gpuPreviewResourcePendingRequestsByCacheKey.insert_or_assign(
                requestedCacheKey,
                request);
            TrackGpuPreviewResourceRequestStart(requestedCacheKey, request);
            m_gpuPreviewResourcePendingDeferredCacheKeys.erase(requestedCacheKey);
            const bool readyMarkerInserted =
                TryPublishResidentCompletionPromotion(requestedCacheKey, request);
            if (readyMarkerInserted && !recoverableResidentFailure)
            {
                // Completion can arrive after the original visible deadline
                // without producing a terminal result. Give the canonical
                // resident retry its own bounded presentation window.
                ClearVisibleThumbnailRequestStart(request);
                TrackVisibleThumbnailRequestStart(request);
            }
            if (recoverableResidentFailure)
                m_thumbnailStatesByCacheKey[requestedCacheKey] =
                    ThumbnailState::WaitingForResources;
            if (m_queuedRequestsByCacheKey.find(requestedCacheKey) ==
                    m_queuedRequestsByCacheKey.end() &&
                EnsureQueuedRequestCapacityFor(requestedCacheKey, request))
            {
                m_queuedRequestsByCacheKey.emplace(requestedCacheKey, request);
                EnqueueQueuedCacheKey(requestedCacheKey, request, true);
                RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                    "repair-complete-resident-queue",
                    &request,
                    m_queuedRequestsByCacheKey.size());
            }
        }
    }

    // A delete invalidates only the revisions that existed at that point.
    // Once a newer request for the same stable presentation arrives, it is a
    // new asset incarnation and must be allowed to publish normally.
    if (const auto invalidation = m_invalidatedPresentationRevisions.find(presentationKey);
        invalidation != m_invalidatedPresentationRevisions.end() &&
        request.requestRevision > invalidation->second)
    {
        m_invalidatedPresentationRevisions.erase(invalidation);
    }
    if (const auto terminal = m_terminalPresentationRevisions.find(presentationKey);
        terminal != m_terminalPresentationRevisions.end() &&
        request.requestRevision > terminal->second)
    {
        // A new freshness revision is allowed to retry after an older
        // preparation timed out. The old revision remains fenced below.
        m_terminalPresentationRevisions.erase(terminal);
    }
    const bool isLatestPresentationRevision = ObserveLatestPresentationRevision(request);
    if (isLatestPresentationRevision && request.priority == ThumbnailRequestPriority::Visible)
        TrackVisibleThumbnailRequestStart(request);

    // Non-browser callers may not own a frame-level maintenance pump. Preserve
    // their terminal-timeout behavior without restoring the old per-tile global
    // scan: only an already-started, actually expired current presentation can
    // trigger maintenance from a lookup.
    if (const auto visibleDeadline =
            m_visibleThumbnailRequestDeadlinesByPresentationKey.find(presentationKey);
        visibleDeadline != m_visibleThumbnailRequestDeadlinesByPresentationKey.end() &&
        visibleDeadline->second.workStarted &&
        visibleDeadline->second.startedAt.time_since_epoch().count() != 0 &&
        std::chrono::steady_clock::now() - visibleDeadline->second.startedAt >=
            kVisibleThumbnailRequestTimeout)
    {
        ExpireStalledVisibleThumbnailRequests();
    }

    // Global lifecycle maintenance is pumped once by AssetBrowser. Running it
    // from every visible tile lookup multiplies queue scans and filesystem
    // validation by the number of items in the folder. Request-local terminal
    // and resource-timeout checks remain below for non-browser callers.
    NLS_PROFILE_NAMED_SCOPE("AssetThumbnailService::GetThumbnail");
    NLS::Base::Profiling::PerformanceStageScope lookupScope(
        NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
        "ThumbnailCacheLookup",
        NLS::Base::Profiling::PerformanceStageThread::Main);

    AssetThumbnailServiceResult result;
    result.fallbackIcon = FallbackIconForKind(request.kind);
    InitializeThumbnailResultIdentity(request, result);
    const std::string requestTelemetryPath = BuildThumbnailRequestTelemetryPath(request);

    {
        const auto stableLookupBegin = std::chrono::steady_clock::now();
        if (const auto stableIterator = m_stableThumbnailResultsByCacheKey.find(requestedCacheKey);
            stableIterator != m_stableThumbnailResultsByCacheKey.end())
        {
            std::error_code imageError;
            const bool imageStillExists =
                !stableIterator->second.imagePath.empty() &&
                std::filesystem::is_regular_file(stableIterator->second.imagePath, imageError) &&
                !imageError;
            bool stableResultUsable = imageStillExists && IsThumbnailRequestStillFresh(request);
            if (stableIterator->second.residentPreviewPartial)
            {
                // Partial resident frames are GPU-only and may never have a
                // PNG path. Keep one only while the registry still reports
                // the same provisional revision; a new revision means the
                // scene attached more resources and must be rendered again.
                const bool residentRequest = request.residentPrefabPreviewSource.has_value() &&
                    request.residentPrefabPreviewSource->HasIdentity();
                stableResultUsable = residentRequest &&
                    request.residentPreviewPartial &&
                    stableIterator->second.residentPreviewRevision != 0u &&
                    stableIterator->second.residentPreviewRevision ==
                        request.residentPreviewRevision &&
                    stableIterator->second.gpuTexture.IsValid();
            }
            if (stableResultUsable)
            {
                RecordThumbnailRequestBuildTelemetry(
                    NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailServiceRequestStableLookup,
                    stableLookupBegin,
                    requestTelemetryPath);
                lookupScope.AddCounter("stableThumbnailResultHitCount");
                const bool keepsResidentContinuation =
                    stableIterator->second.residentPreviewPartial &&
                    m_gpuPreviewResourcePendingRequestsByCacheKey.find(
                        stableIterator->first) !=
                        m_gpuPreviewResourcePendingRequestsByCacheKey.end();
                if (!keepsResidentContinuation && stableIterator->second.cacheEntry.has_value())
                    m_thumbnailStatesByCacheKey[stableIterator->second.cacheEntry->cacheKey] = ThumbnailState::Ready;
                ClearVisibleThumbnailRequestStart(request);
                return stableIterator->second;
            }
            m_stableThumbnailResultsByCacheKey.erase(stableIterator);
        }
        RecordThumbnailRequestBuildTelemetry(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailServiceRequestStableLookup,
            stableLookupBegin,
            requestTelemetryPath);

        // A resident request can be queued before deferred artifact
        // resolution finishes. The resolved request may therefore use a
        // different cache key even though it represents the same presentation
        // and resident registry revision. Recover the parked partial result by
        // presentation identity so this lookup cannot turn it back into a
        // normal queued render.
        if (request.residentPreviewPartial &&
            request.residentPrefabPreviewSource.has_value() &&
            request.residentPrefabPreviewSource->HasIdentity() &&
            request.residentPreviewRevision != 0u)
        {
            for (const auto& [stableCacheKey, stableResult] :
                 m_stableThumbnailResultsByCacheKey)
            {
                if (!stableResult.residentPreviewPartial ||
                    stableResult.presentationKey != presentationKey ||
                    stableResult.residentPreviewRevision != request.residentPreviewRevision ||
                    !stableResult.gpuTexture.IsValid())
                {
                    continue;
                }
                const auto partialRevision =
                    m_gpuPreviewResidentPartialRevisionByCacheKey.find(stableCacheKey);
                const auto pendingOwner =
                    m_gpuPreviewResourcePendingRequestsByCacheKey.find(stableCacheKey);
                if (partialRevision == m_gpuPreviewResidentPartialRevisionByCacheKey.end() ||
                    partialRevision->second != request.residentPreviewRevision ||
                    pendingOwner == m_gpuPreviewResourcePendingRequestsByCacheKey.end())
                {
                    continue;
                }
                lookupScope.AddCounter("stableThumbnailResidentPartialPresentationHitCount");
                ClearVisibleThumbnailRequestStart(request);
                return stableResult;
            }
        }
    }

    const auto FinalizeLookupResult = [&](AssetThumbnailServiceResult candidate)
    {
        const bool recoveredCanonical =
            candidate.status != AssetThumbnailServiceStatus::Fresh &&
            PromoteAssetThumbnailResultFromPresentationIndex(request, candidate);
        if (recoveredCanonical && candidate.cacheEntry.has_value())
        {
            RemoveQueuedCacheKeyOccurrences(requestedCacheKey);
            m_queuedRequestsByCacheKey.erase(requestedCacheKey);
            m_queuedThumbnailLaneByCacheKey.erase(requestedCacheKey);
            m_resolvedPreviewRequestsByCacheKey.erase(requestedCacheKey);
            m_gpuDeferredHeavyPreviewCacheKeys.erase(requestedCacheKey);
            m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(requestedCacheKey);
            m_gpuPreviewEmptyFrameDeferralsByCacheKey.erase(requestedCacheKey);
            ClearGpuPreviewResourcePending(requestedCacheKey);
            m_thumbnailStatesByCacheKey[requestedCacheKey] = ThumbnailState::Ready;
            m_thumbnailStatesByCacheKey[candidate.cacheEntry->cacheKey] = ThumbnailState::Ready;
            m_stableThumbnailResultsByCacheKey[requestedCacheKey] = candidate;
            m_stableThumbnailResultsByCacheKey[candidate.cacheEntry->cacheKey] = candidate;
        }
        if (candidate.status != AssetThumbnailServiceStatus::Pending)
            ClearVisibleThumbnailRequestStart(request);
        if ((candidate.status == AssetThumbnailServiceStatus::Pending ||
                candidate.status == AssetThumbnailServiceStatus::Failed) &&
            !candidate.retainedImage.has_value())
        {
            AttachRetainedThumbnailImage(
                request,
                candidate.cacheEntry.has_value() ? candidate.cacheEntry->cacheKey : requestedCacheKey,
                candidate);
        }
        SynchronizeThumbnailResultPresentationState(candidate);
        if (candidate.status == AssetThumbnailServiceStatus::Fresh)
        {
            CompleteImportedPrefabThumbnailContinuation(request);
            ReleaseImportedPrefabThumbnailContinuationOwner(request);
        }
        return candidate;
    };

    if (!isLatestPresentationRevision)
    {
        result.status = AssetThumbnailServiceStatus::Pending;
        result.cacheEntry = ResolveAssetThumbnailCacheEntryPathForRead(request);
        result.diagnostic = "thumbnail-request-superseded";
        return FinalizeLookupResult(result);
    }

    // A maintenance pass can produce a terminal resource-timeout result even
    // when the render scheduler is gated by scene loading. Return that result
    // through the normal lookup path as well, so a tile cannot remain visually
    // Loading merely because ConsumeCompletedThumbnail was not admitted in the
    // same frame.
    const AssetThumbnailServiceResult* terminalResult = nullptr;
    if (const auto terminal = m_terminalThumbnailResultsByCacheKey.find(requestedCacheKey);
        terminal != m_terminalThumbnailResultsByCacheKey.end())
    {
        terminalResult = &terminal->second;
    }
    if (terminalResult == nullptr)
    {
        for (const auto& [_, candidate] : m_terminalThumbnailResultsByCacheKey)
        {
            if (candidate.presentationKey == presentationKey &&
                (candidate.requestRevision == 0u ||
                    candidate.requestRevision == request.requestRevision))
            {
                terminalResult = &candidate;
                break;
            }
        }
    }
    if (terminalResult != nullptr &&
        (terminalResult->requestRevision == 0u ||
            terminalResult->requestRevision == request.requestRevision) &&
        terminalResult->presentationKey == presentationKey)
    {
        return FinalizeLookupResult(*terminalResult);
    }

    auto pendingStateIterator = m_thumbnailStatesByCacheKey.find(requestedCacheKey);
    if (pendingStateIterator != m_thumbnailStatesByCacheKey.end() &&
        pendingStateIterator->second == ThumbnailState::WaitingForResources &&
        SupportsGpuThumbnailPreview(request) &&
        m_gpuPreviewResourcePendingRequestsByCacheKey.find(requestedCacheKey) ==
            m_gpuPreviewResourcePendingRequestsByCacheKey.end())
    {
        // Repair only this queried continuation. The old lookup path called the
        // global restore pass for every tile, which hid this ownership race at
        // the cost of repeatedly scanning the full queue.
        m_gpuPreviewResourcePendingRequestsByCacheKey.emplace(requestedCacheKey, request);
        TrackGpuPreviewResourceRequestStart(requestedCacheKey, request);
        auto queued = m_queuedRequestsByCacheKey.find(requestedCacheKey);
        if (queued == m_queuedRequestsByCacheKey.end() &&
            EnsureQueuedRequestCapacityFor(requestedCacheKey, request))
        {
            m_queuedRequestsByCacheKey.emplace(requestedCacheKey, request);
            EnqueueQueuedCacheKey(requestedCacheKey, request);
        }
        else if (queued != m_queuedRequestsByCacheKey.end() &&
            m_queuedThumbnailLaneByCacheKey.find(requestedCacheKey) ==
                m_queuedThumbnailLaneByCacheKey.end())
        {
            RemoveQueuedCacheKeyOccurrences(requestedCacheKey);
            EnqueueQueuedCacheKey(requestedCacheKey, queued->second);
        }
    }
    if (pendingStateIterator != m_thumbnailStatesByCacheKey.end() &&
        IsGpuPreviewResourceContinuationState(requestedCacheKey, pendingStateIterator->second))
    {
        const auto now = std::chrono::steady_clock::now();
        const bool sceneLoadDeadlineSuspended =
            request.residentPrefabPreviewSource.has_value() &&
            request.residentPrefabPreviewSource->HasIdentity() &&
            SuspendResidentGpuPreviewResourceDeadlineForSceneLoad(
                requestedCacheKey,
                request,
                now);
        // The scene-load transition may refresh the deadline maps. Acquire
        // iterators only after that refresh so this lookup never dereferences
        // an invalidated map iterator.
        const auto deferral = m_gpuPreviewResourcePendingDeferralsByCacheKey.find(requestedCacheKey);
        const auto requestStart = m_gpuPreviewResourceRequestStartedAtByCacheKey.find(requestedCacheKey);
        const bool waitingForImportedContinuationAdmission =
            request.importedPrefabThumbnailContinuation &&
            deferral == m_gpuPreviewResourcePendingDeferralsByCacheKey.end();
        if (deferral != m_gpuPreviewResourcePendingDeferralsByCacheKey.end() &&
            deferral->second.resourceWorkActive)
        {
            deferral->second.lastProgressAt = now;
        }
        auto resourceDeadlineStart = std::chrono::steady_clock::time_point {};
        if (deferral != m_gpuPreviewResourcePendingDeferralsByCacheKey.end())
        {
            resourceDeadlineStart = deferral->second.lastProgressAt.time_since_epoch().count() != 0
                ? deferral->second.lastProgressAt
                : deferral->second.firstDeferredAt;
        }
        else if (requestStart != m_gpuPreviewResourceRequestStartedAtByCacheKey.end())
        {
            resourceDeadlineStart = requestStart->second;
        }
        const bool resourceTimedOut =
            !sceneLoadDeadlineSuspended &&
            !waitingForImportedContinuationAdmission &&
            resourceDeadlineStart.time_since_epoch().count() != 0 &&
            now - resourceDeadlineStart >= kGpuPreviewResourcePendingTimeout;
        if (resourceTimedOut)
        {
            const auto timeoutEvaluation = EvaluateAssetThumbnailCache(
                request,
                AssetThumbnailCacheIntegrityMode::Fast);
            constexpr std::string_view kResourceTimeoutDiagnostic =
                "thumbnail-gpu-preview-resources-timeout:thumbnail-request-query-timeout";
            auto timeoutResult = BuildResultFromEvaluation(
                request,
                timeoutEvaluation,
                AssetThumbnailServiceStatus::Failed);
            timeoutResult.diagnostic = std::string(kResourceTimeoutDiagnostic);
            WriteThumbnailMetadataForEvaluation(
                request,
                timeoutEvaluation,
                AssetThumbnailCacheStatus::Failed,
                timeoutResult.diagnostic);
            RemoveQueuedCacheKeyOccurrences(requestedCacheKey);
            m_queuedRequestsByCacheKey.erase(requestedCacheKey);
            m_queuedThumbnailLaneByCacheKey.erase(requestedCacheKey);
            m_resolvedPreviewRequestsByCacheKey.erase(requestedCacheKey);
            m_gpuDeferredHeavyPreviewCacheKeys.erase(requestedCacheKey);
            m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(requestedCacheKey);
            m_gpuPreviewEmptyFrameDeferralsByCacheKey.erase(requestedCacheKey);
            ClearGpuPreviewResourcePending(requestedCacheKey);
            m_thumbnailStatesByCacheKey[requestedCacheKey] = ThumbnailState::Failed;
            m_terminalPresentationRevisions[presentationKey] =
                (std::max)(
                    m_terminalPresentationRevisions[presentationKey],
                    timeoutResult.requestRevision);
            m_terminalThumbnailResultsByCacheKey.insert_or_assign(
                requestedCacheKey,
                timeoutResult);
            ClearVisibleThumbnailRequestStart(request);
            return timeoutResult;
        }
    }
    const bool hasActiveGpuReadbackState =
        pendingStateIterator != m_thumbnailStatesByCacheKey.end() &&
            IsActiveThumbnailReadbackOrPersistenceState(pendingStateIterator->second);
    if (pendingStateIterator != m_thumbnailStatesByCacheKey.end() &&
        IsPendingThumbnailState(pendingStateIterator->second))
    {
        const auto queueBegin = std::chrono::steady_clock::now();
        auto queuedIterator = m_queuedRequestsByCacheKey.find(requestedCacheKey);
        const bool adoptedInFlightRequest = AdoptMatchingInFlightRequest(requestedCacheKey);
        if (adoptedInFlightRequest || queuedIterator != m_queuedRequestsByCacheKey.end() || hasActiveGpuReadbackState)
        {
            lookupScope.AddCounter("duplicateThumbnailRequestCount");
            if (request.priority == ThumbnailRequestPriority::Visible ||
                request.priority == ThumbnailRequestPriority::Inspector)
            {
                m_offscreenSinceByCacheKey.erase(requestedCacheKey);
            }
            else if (request.priority == ThumbnailRequestPriority::Prefetch)
            {
                m_offscreenSinceByCacheKey.try_emplace(
                    requestedCacheKey,
                    std::chrono::steady_clock::now());
            }
            if (queuedIterator != m_queuedRequestsByCacheKey.end() &&
                (ShouldPromoteQueuedThumbnailRequest(queuedIterator->second, request) ||
                    queuedIterator->second.priority != request.priority ||
                    ResidentThumbnailLaneEligibilityChanged(queuedIterator->second, request)))
            {
                lookupScope.AddCounter("coalescingPressure");
                queuedIterator->second = request;
                EnqueueQueuedCacheKey(requestedCacheKey, queuedIterator->second);
                if (request.priority == ThumbnailRequestPriority::Prefetch)
                    m_offscreenSinceByCacheKey.try_emplace(requestedCacheKey, std::chrono::steady_clock::now());
            }
            else if (queuedIterator != m_queuedRequestsByCacheKey.end())
            {
                lookupScope.AddCounter("coalescingPressure");
            }
            else if (hasActiveGpuReadbackState)
            {
                if (EnsureQueuedRequestCapacityFor(requestedCacheKey, request))
                {
                    m_queuedRequestsByCacheKey[requestedCacheKey] = request;
                    EnqueueQueuedCacheKey(requestedCacheKey, request);
                    RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                        "restore-active-gpu-state-queue",
                        &request,
                        m_queuedRequestsByCacheKey.size());
                }
            }
            if (adoptedInFlightRequest && !hasActiveGpuReadbackState)
                m_thumbnailStatesByCacheKey[requestedCacheKey] = ThumbnailState::Preparing;
            result.cacheEntry = ResolveAssetThumbnailCacheEntryPathForRead(request);
            result.status = AssetThumbnailServiceStatus::Pending;
            RecordThumbnailRequestBuildTelemetry(
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailServiceRequestQueue,
                queueBegin,
                requestTelemetryPath);
            return FinalizeLookupResult(result);
        }
        if (HasDeferredGpuPreviewEmptyFrame(requestedCacheKey))
        {
            lookupScope.AddCounter("duplicateThumbnailRequestCount");
            result.cacheEntry = ResolveAssetThumbnailCacheEntryPathForRead(request);
            result.status = AssetThumbnailServiceStatus::Pending;
            result.diagnostic = "thumbnail-gpu-preview-empty-frame";
            RecordThumbnailRequestBuildTelemetry(
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailServiceRequestQueue,
                queueBegin,
                requestTelemetryPath);
            return FinalizeLookupResult(result);
        }
    }

    auto cacheEvaluateBegin = std::chrono::steady_clock::now();
    const auto evaluation = EvaluateAssetThumbnailCache(request, AssetThumbnailCacheIntegrityMode::Fast);
    RecordThumbnailRequestBuildTelemetry(
        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailServiceRequestCacheEvaluate,
        cacheEvaluateBegin,
        requestTelemetryPath);
    result.cacheEntry = evaluation.entry;
    result.diagnostic = evaluation.diagnostic;

    if (evaluation.status == AssetThumbnailCacheStatus::Fresh &&
        !request.residentPreviewPartial &&
        evaluation.entry.has_value())
    {
        lookupScope.AddCounter("cacheHitCount");
        m_thumbnailStatesByCacheKey[evaluation.entry->cacheKey] = ThumbnailState::Ready;
        result.status = AssetThumbnailServiceStatus::Fresh;
        result.presentationState = ThumbnailPresentationState::Ready;
        result.previewQuality = ThumbnailPreviewQuality::Canonical;
        result.refreshPending = false;
        result.failureRetained = false;
        result.imagePath = evaluation.entry->imagePath;
        m_stableThumbnailResultsByCacheKey[evaluation.entry->cacheKey] = result;
        m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(evaluation.entry->cacheKey);
        m_gpuPreviewEmptyFrameDeferralsByCacheKey.erase(evaluation.entry->cacheKey);
        ClearGpuPreviewResourceRequestStart(evaluation.entry->cacheKey);
        m_residentPreviewRequestsByPresentationKey.erase(presentationKey);
        return FinalizeLookupResult(result);
    }
    lookupScope.AddCounter("cacheMissCount");

    bool importedContinuationMayRecoverCachedFailure = false;
    if (request.importedPrefabThumbnailContinuation)
    {
        const auto registrationRevision =
            request.importedPrefabThumbnailContinuationRevision != 0u
            ? request.importedPrefabThumbnailContinuationRevision
            : 1u;
        auto [attempt, inserted] =
            m_importedPrefabThumbnailAttemptRevisionByCacheKey.try_emplace(
                requestedCacheKey,
                registrationRevision);
        if (!inserted && attempt->second != registrationRevision)
        {
            attempt->second = registrationRevision;
            inserted = true;
        }
        importedContinuationMayRecoverCachedFailure = inserted;
    }

    // A malformed request or an unsafe cache root must never be represented as
    // Pending. There is no cache entry to enqueue in this case, so returning
    // Pending would leave the UI in Loading forever after the GPU start timer
    // has already been registered above.
    if (!evaluation.entry.has_value())
    {
        ClearGpuPreviewResourceRequestStart(requestedCacheKey);
        result.status = AssetThumbnailServiceStatus::Failed;
        result.diagnostic = evaluation.diagnostic.empty()
            ? "thumbnail-cache-entry-unavailable"
            : evaluation.diagnostic;
        RecordThumbnailRequestBuildTelemetry(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailServiceRequestQueue,
            cacheEvaluateBegin,
            requestTelemetryPath);
        return FinalizeLookupResult(result);
    }

    const bool retryableImportedContinuationFailure =
        importedContinuationMayRecoverCachedFailure &&
        IsImportedPrefabContinuationRecoveryDiagnostic(evaluation.diagnostic);
    if (evaluation.status == AssetThumbnailCacheStatus::Failed &&
        !retryableImportedContinuationFailure &&
        !IsRetryableThumbnailFailureDiagnostic(request, evaluation.diagnostic))
    {
        if (evaluation.entry.has_value())
        {
            m_thumbnailStatesByCacheKey[evaluation.entry->cacheKey] = ThumbnailState::Failed;
            ClearGpuPreviewResourceRequestStart(evaluation.entry->cacheKey);
        }
        result.status = evaluation.diagnostic == kLargePrefabPreviewAwaitingResidentDiagnostic
            ? AssetThumbnailServiceStatus::Fallback
            : AssetThumbnailServiceStatus::Failed;
        return FinalizeLookupResult(result);
    }

    if (!CanRequestThumbnailGeneration(request.kind))
    {
        if (evaluation.entry.has_value())
        m_thumbnailStatesByCacheKey[evaluation.entry->cacheKey] = ThumbnailState::Failed;
        result.status = AssetThumbnailServiceStatus::Fallback;
        result.diagnostic = UnsupportedDiagnosticForKind(request.kind);
        return FinalizeLookupResult(result);
    }

    if (evaluation.entry.has_value())
    {
        const auto queueBegin = std::chrono::steady_clock::now();
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(evaluation.entry->cacheKey);
        const bool hasActiveGpuReadbackState =
            stateIterator != m_thumbnailStatesByCacheKey.end() &&
            IsActiveThumbnailReadbackOrPersistenceState(stateIterator->second);
        if (AdoptMatchingInFlightRequest(evaluation.entry->cacheKey))
        {
            lookupScope.AddCounter("duplicateThumbnailRequestCount");
            if (!hasActiveGpuReadbackState)
                m_thumbnailStatesByCacheKey[evaluation.entry->cacheKey] = ThumbnailState::Preparing;
            result.status = AssetThumbnailServiceStatus::Pending;
            RecordThumbnailRequestBuildTelemetry(
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailServiceRequestQueue,
                queueBegin,
                requestTelemetryPath);
            return FinalizeLookupResult(result);
        }

        auto queuedIterator = m_queuedRequestsByCacheKey.find(evaluation.entry->cacheKey);
        if (queuedIterator == m_queuedRequestsByCacheKey.end())
        {
            if (!EnsureQueuedRequestCapacityFor(evaluation.entry->cacheKey, request))
            {
                lookupScope.AddCounter("thumbnailQueueBackpressureCount");
                m_thumbnailStatesByCacheKey[evaluation.entry->cacheKey] = ThumbnailState::Cancelled;
                result.status = AssetThumbnailServiceStatus::Fallback;
                result.diagnostic = "thumbnail-generation-queue-full";
                RecordThumbnailRequestBuildTelemetry(
                    NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailServiceRequestQueue,
                    queueBegin,
                    requestTelemetryPath);
                return FinalizeLookupResult(result);
            }
            queuedIterator = m_queuedRequestsByCacheKey.emplace(evaluation.entry->cacheKey, request).first;
            EnqueueQueuedCacheKey(evaluation.entry->cacheKey, request);
            if (request.priority == ThumbnailRequestPriority::Prefetch)
                m_offscreenSinceByCacheKey.try_emplace(evaluation.entry->cacheKey, std::chrono::steady_clock::now());
        }
        else
        {
            lookupScope.AddCounter("duplicateThumbnailRequestCount");
            lookupScope.AddCounter("coalescingPressure");
            if (ShouldPromoteQueuedThumbnailRequest(queuedIterator->second, request) ||
                ResidentThumbnailLaneEligibilityChanged(queuedIterator->second, request))
            {
                if (!EnsureQueuedRequestCapacityFor(evaluation.entry->cacheKey, request))
                {
                    lookupScope.AddCounter("thumbnailQueueBackpressureCount");
                    result.status = AssetThumbnailServiceStatus::Fallback;
                    result.diagnostic = "thumbnail-generation-queue-full";
                    RecordThumbnailRequestBuildTelemetry(
                        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailServiceRequestQueue,
                        queueBegin,
                        requestTelemetryPath);
                    return FinalizeLookupResult(result);
                }
                queuedIterator->second = request;
                EnqueueQueuedCacheKey(evaluation.entry->cacheKey, queuedIterator->second);
            }
        }
        if (!hasActiveGpuReadbackState)
            m_thumbnailStatesByCacheKey[evaluation.entry->cacheKey] = ThumbnailState::Queued;
        RecordThumbnailRequestBuildTelemetry(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailServiceRequestQueue,
            queueBegin,
            requestTelemetryPath);
    }
    lookupScope.AddCounter("queueDepth", m_queuedRequestsByCacheKey.size());
    result.status = AssetThumbnailServiceStatus::Pending;
    return FinalizeLookupResult(result);
}

std::optional<AssetThumbnailServiceResult> AssetThumbnailService::GenerateNextThumbnail()
{
    CancelExpiredOffscreenRequests();
    ExpireStalledVisibleThumbnailRequests();
    RestoreGpuPreviewResourceContinuationRequests();
    ExpireStalledGpuPreviewResourceContinuations();
    PumpDeferredPersistenceTickets();
    NLS::Base::Profiling::PerformanceStageScope totalScope(
        NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
        "TotalThumbnail",
        NLS::Base::Profiling::PerformanceStageThread::Main);
    totalScope.AddCounter("queueBacklog", m_queuedRequestsByCacheKey.size());
    totalScope.AddCounter("inFlightRequestCount", m_inFlightThumbnails.size());
    totalScope.AddCounter("cacheWriteBudgetRemaining", m_generationBudget.cacheWriteCountBudget);
    totalScope.AddCounter("cpuPreparationByteBudgetRemaining", m_generationBudget.cpuPreparationByteBudget);
    totalScope.AddCounter("gpuUploadByteBudgetRemaining", m_generationBudget.gpuUploadByteBudget);

    RequeueReadyGpuPreviewEmptyFrameRetries();

    if (HasQueuedCacheKeys() &&
        m_generationBudget.cacheWriteCountBudget == 0u)
    {
        return std::nullopt;
    }

    if (!m_generationCancelToken)
        m_generationCancelToken = std::make_shared<AssetThumbnailGenerationCancelToken>();
    m_generationCancelToken->generation = m_generationSerial;

    std::vector<std::string> deferredCacheKeys;

    while (HasQueuedCacheKeys())
    {
        // Background CPU work must not scan through a run of heavy GPU
        // requests just to discover a texture later in the visible lane. The
        // explicit non-GPU selector gives queued textures/material metadata a
        // bounded path even while a Prefab resource continuation is pending.
        const auto cacheKey = HasQueuedNonGpuThumbnailWork()
            ? PopNextNonGpuThumbnailCacheKey()
            : PopNextQueuedCacheKey();
        if (!cacheKey.has_value())
            break;

        const auto requestIterator = m_queuedRequestsByCacheKey.find(*cacheKey);
        if (requestIterator == m_queuedRequestsByCacheKey.end())
            continue;

        const auto request = requestIterator->second;
        const auto estimatedCpuPreparationBytes = EstimateThumbnailCpuPreparationBytes(request);
        const auto estimatedGpuUploadBytes = EstimateThumbnailGpuUploadBytes(request);
        if (!HasThumbnailBudget(m_generationBudget.cpuPreparationByteBudget, estimatedCpuPreparationBytes) ||
            !HasThumbnailBudget(m_generationBudget.gpuUploadByteBudget, estimatedGpuUploadBytes))
        {
            RestoreDeferredCacheKeys(deferredCacheKeys);
            m_queuedRequestsByCacheKey[*cacheKey] = request;
            EnqueueQueuedCacheKey(*cacheKey, request);
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Queued;
            return std::nullopt;
        }
        if (ShouldDeferBackgroundCpuThumbnailToPreviewRenderer(request.kind) &&
            (m_resolvedPreviewRequestsByCacheKey.find(*cacheKey) != m_resolvedPreviewRequestsByCacheKey.end() ||
                m_gpuDeferredHeavyPreviewCacheKeys.find(*cacheKey) != m_gpuDeferredHeavyPreviewCacheKeys.end() ||
                SupportsGpuThumbnailPreview(request)))
        {
            deferredCacheKeys.push_back(*cacheKey);
            continue;
        }

        m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Preparing;
        if (SupportsGpuThumbnailPreview(request) &&
            !CanGenerateThumbnail(request.kind))
        {
            m_queuedThumbnailLaneByCacheKey.erase(requestIterator->first);
            m_queuedRequestsByCacheKey.erase(requestIterator);
            const auto evaluation = EvaluateAssetThumbnailCache(request);
            const auto previewRequest = ResolveDeferredThumbnailPreviewRequest(request);
            if (const auto invalidPathDiagnostic = ValidateGpuPreviewRequestArtifactPaths(previewRequest, true);
                invalidPathDiagnostic.has_value())
            {
                auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
                result.diagnostic = *invalidPathDiagnostic;
                const auto metadataRequest = BuildResolvedThumbnailCacheRequest(request, previewRequest);
                WriteThumbnailMetadataForEvaluation(
                    request,
                    evaluation,
                    AssetThumbnailCacheStatus::Failed,
                    result.diagnostic,
                    &metadataRequest);
                m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Failed;
                RestoreDeferredCacheKeys(deferredCacheKeys);
                return result;
            }
            auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Fallback);
            result.diagnostic = "thumbnail-gpu-preview-renderer-unavailable";
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Failed;
            RestoreDeferredCacheKeys(deferredCacheKeys);
            return result;
        }

        m_queuedThumbnailLaneByCacheKey.erase(requestIterator->first);
        m_queuedRequestsByCacheKey.erase(requestIterator);

        RestoreDeferredCacheKeys(deferredCacheKeys);
        const auto generated = TryGenerateThumbnailForRequest(request, m_generationCancelToken);
        if (generated.status != AssetThumbnailServiceStatus::Pending)
        {
            m_offscreenSinceByCacheKey.erase(*cacheKey);
            ClearVisibleThumbnailRequestStart(request);
        }
        // A Pending result owns a continuation state written by the generation
        // path. Do not turn it into Failed or clear its ownership here: doing
        // so leaves the request in a queue without a scheduler-visible
        // continuation and prevents resource work from making progress.
        if (generated.status == AssetThumbnailServiceStatus::Fresh)
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Ready;
        else if (generated.status != AssetThumbnailServiceStatus::Pending)
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Failed;
        if (generated.status == AssetThumbnailServiceStatus::Fresh)
        {
            ConsumeThumbnailCacheWriteBudgetForFreshResult(
                m_generationBudget,
                m_hasExplicitGenerationBudget);
            totalScope.AddCounter("thumbnailsGeneratedThisFrame");
        }
        if (generated.status != AssetThumbnailServiceStatus::Pending)
        {
            m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
            m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(*cacheKey);
            ClearGpuPreviewResourcePending(*cacheKey);
            m_gpuPreviewEmptyFrameDeferralsByCacheKey.erase(*cacheKey);
        }
        return generated;
    }

    RestoreDeferredCacheKeys(deferredCacheKeys);
    return std::nullopt;
}

void AssetThumbnailService::TrackGpuPreviewReadbackPending(
    const std::string& cacheKey,
    const AssetThumbnailRequest& request)
{
    const auto requestKey = BuildThumbnailPreviewReadbackRequestKey(request);
    auto& deferral = m_gpuPreviewReadbackPendingDeferralsByCacheKey[cacheKey];
    if (deferral.pollCount == 0u)
        deferral.firstPendingAt = std::chrono::steady_clock::now();
    ++deferral.pollCount;
    m_gpuPreviewReadbackPendingCacheKeys.insert(cacheKey);
    m_gpuPreviewReadbackPendingRequestsByCacheKey[cacheKey] = request;
    m_gpuPreviewReadbackCacheKeyByRequestKey[requestKey] = cacheKey;
}

void AssetThumbnailService::ClearGpuPreviewReadbackPending(const std::string& cacheKey)
{
    m_gpuPreviewReadbackPendingCacheKeys.erase(cacheKey);
    const auto pending = m_gpuPreviewReadbackPendingRequestsByCacheKey.find(cacheKey);
    if (pending != m_gpuPreviewReadbackPendingRequestsByCacheKey.end())
    {
        const auto requestKey = BuildThumbnailPreviewReadbackRequestKey(pending->second);
        const auto reverse = m_gpuPreviewReadbackCacheKeyByRequestKey.find(requestKey);
        if (reverse != m_gpuPreviewReadbackCacheKeyByRequestKey.end() &&
            reverse->second == cacheKey)
        {
            m_gpuPreviewReadbackCacheKeyByRequestKey.erase(reverse);
        }
        m_gpuPreviewReadbackPendingRequestsByCacheKey.erase(pending);
    }
    m_gpuPreviewReadbackPendingDeferralsByCacheKey.erase(cacheKey);
}

void AssetThumbnailService::PollCompletedGpuPreviewReadbacks(
    IEditorThumbnailPreviewRenderer& previewRenderer)
{
    auto completed = previewRenderer.PollCompletedReadbacks(3u);
    RecordThumbnailGpuPreviewQueueDecisionTelemetry(
        "poll-readbacks|count=" + std::to_string(completed.size()),
        nullptr,
        m_gpuPreviewReadbackPendingCacheKeys.size());
    for (auto& item : completed)
    {
        const auto reverse = m_gpuPreviewReadbackCacheKeyByRequestKey.find(
            item.ticket.requestKey);
        if (reverse == m_gpuPreviewReadbackCacheKeyByRequestKey.end())
        {
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "poll-readback-drop=unknown-request-key",
                nullptr,
                m_gpuPreviewReadbackPendingCacheKeys.size());
            continue;
        }

        // Clearing a pending readback erases the reverse-map node that owns this
        // string. Keep an independent key for the remaining cleanup and state
        // updates in this polling turn.
        const auto cacheKey = reverse->second;
        const auto pending = m_gpuPreviewReadbackPendingRequestsByCacheKey.find(cacheKey);
        if (pending == m_gpuPreviewReadbackPendingRequestsByCacheKey.end())
        {
            m_gpuPreviewReadbackCacheKeyByRequestKey.erase(reverse);
            continue;
        }

        auto request = pending->second;
        if (const auto resolved = m_resolvedPreviewRequestsByCacheKey.find(cacheKey);
            resolved != m_resolvedPreviewRequestsByCacheKey.end())
        {
            request = resolved->second;
        }
        if (item.ticket.requestRevision != 0u &&
            request.requestRevision != item.ticket.requestRevision)
        {
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "poll-readback-drop=revision-mismatch|ticket=" +
                    std::to_string(item.ticket.requestRevision) +
                    "|request=" + std::to_string(request.requestRevision),
                &request,
                m_gpuPreviewReadbackPendingCacheKeys.size());
            continue;
        }

        if (IsPresentationRevisionSuperseded(request))
        {
            // The renderer has already retired the ticket. Drop the completed
            // pixels without allowing an older freshness revision to enter the
            // service result table or UI.
            m_completedGpuPreviewResultsByCacheKey.erase(cacheKey);
            RemoveQueuedCacheKeyOccurrences(cacheKey);
            m_queuedRequestsByCacheKey.erase(cacheKey);
            m_queuedThumbnailLaneByCacheKey.erase(cacheKey);
            ClearGpuPreviewReadbackPending(cacheKey);
            m_thumbnailStatesByCacheKey[cacheKey] = ThumbnailState::Cancelled;
            continue;
        }

        const auto existing = m_completedGpuPreviewResultsByCacheKey.find(cacheKey);
        if (existing != m_completedGpuPreviewResultsByCacheKey.end() &&
            existing->second.requestRevision > item.ticket.requestRevision)
        {
            continue;
        }
        m_completedGpuPreviewResultsByCacheKey[cacheKey] = {
            item.ticket.requestRevision,
            std::make_shared<EditorThumbnailPreviewResult>(std::move(item.preview))
        };
    }
}

std::optional<AssetThumbnailServiceResult> AssetThumbnailService::GenerateNextThumbnail(
    EditorThumbnailPreviewRenderer& previewRenderer,
    const bool includeHeavyGpuPreviews)
{
    EditorThumbnailPreviewRendererAdapter adapter(previewRenderer);
    return GenerateNextThumbnail(adapter, includeHeavyGpuPreviews);
}

std::optional<AssetThumbnailServiceResult> AssetThumbnailService::GenerateNextThumbnail(
    IEditorThumbnailPreviewRenderer& previewRenderer,
    const bool includeHeavyGpuPreviews)
{
    CancelExpiredOffscreenRequests();
    RestoreGpuPreviewResourceContinuationRequests();
    ExpireStalledGpuPreviewResourceContinuations();
    PumpDeferredPersistenceTickets();
    RequeueReadyGpuPreviewEmptyFrameRetries();
    RepairQueuedRequestLaneMembership();
    // Readback polling is independent of the submit/write budgets. Retire
    // every completed slot before selecting the next visible request so a
    // slow slot cannot hold up another ready thumbnail.
    PollCompletedGpuPreviewReadbacks(previewRenderer);

    NLS::Base::Profiling::PerformanceStageScope totalScope(
        NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
        "TotalThumbnail",
        NLS::Base::Profiling::PerformanceStageThread::Main);
    totalScope.AddCounter("queueBacklog", m_queuedRequestsByCacheKey.size());
    totalScope.AddCounter("inFlightRequestCount", m_inFlightThumbnails.size());
    totalScope.AddCounter("previewRenderBudgetRemaining", m_generationBudget.previewRenderCountBudget);
    totalScope.AddCounter("readbackBudgetRemaining", m_generationBudget.readbackCountBudget);
    totalScope.AddCounter("cacheWriteBudgetRemaining", m_generationBudget.cacheWriteCountBudget);
    totalScope.AddCounter("cpuPreparationByteBudgetRemaining", m_generationBudget.cpuPreparationByteBudget);
    totalScope.AddCounter("gpuUploadByteBudgetRemaining", m_generationBudget.gpuUploadByteBudget);

    if (HasQueuedCacheKeys() &&
        m_generationBudget.cacheWriteCountBudget == 0u &&
        !HasQueuedGpuPreviewReadback() &&
        !HasQueuedGpuPreviewResourceContinuation())
    {
        return std::nullopt;
    }

    if (!m_generationCancelToken)
        m_generationCancelToken = std::make_shared<AssetThumbnailGenerationCancelToken>();
    m_generationCancelToken->generation = m_generationSerial;

    std::vector<std::string> deferredCacheKeys;
    size_t deferredGpuPreviewCount = 0u;
    size_t deferredHeavyGpuPreviewCount = 0u;
    size_t deferredImportedPrefabContinuationCount = 0u;
    const size_t maxDeferredGpuPreviewScanPerCall = includeHeavyGpuPreviews
        ? kMaxDeferredHeavyGpuPreviewScanPerCall
        : kMaxDeferredLightGpuPreviewScanPerCall;
    std::optional<AssetThumbnailRequest> pendingPreviewRequestResolution;
    std::unordered_set<std::string> scheduledPreviewRequestResolutionCacheKeys;
    const auto needsDeferredPreviewRequestResolution = [](const AssetThumbnailRequest& request)
    {
        const bool hasResidentPrefabSource = HasLiveResidentThumbnailSnapshot(request);
        return request.artifactPath.empty() &&
            !hasResidentPrefabSource &&
            (request.kind == AssetThumbnailKind::ModelPreview ||
                request.kind == AssetThumbnailKind::PrefabPreview ||
                (request.kind == AssetThumbnailKind::MaterialSphere && request.generatedSubAsset));
    };
    const auto refreshActiveImportedPrefabContinuation = [this]()
    {
        if (m_activeImportedPrefabThumbnailContinuationAssetId.empty())
            return;

        const auto isLiveOwner = [this](
            const std::string& cacheKey,
            const AssetThumbnailRequest& request)
        {
            if (!request.importedPrefabThumbnailContinuation ||
                request.assetId.ToString() !=
                    m_activeImportedPrefabThumbnailContinuationAssetId)
            {
                return false;
            }

            const auto state = m_thumbnailStatesByCacheKey.find(cacheKey);
            return state == m_thumbnailStatesByCacheKey.end() ||
                (state->second != ThumbnailState::Ready &&
                    state->second != ThumbnailState::Failed &&
                    state->second != ThumbnailState::Cancelled);
        };
        const auto hasLiveOwner = [&isLiveOwner](const auto& owners)
        {
            return std::any_of(
                owners.begin(),
                owners.end(),
                [&isLiveOwner](const auto& entry)
                {
                    return isLiveOwner(entry.first, entry.second);
                });
        };

        const auto hasLiveInFlightOwner = std::any_of(
            m_inFlightThumbnails.begin(),
            m_inFlightThumbnails.end(),
            [this](const InFlightThumbnailRequest& entry)
            {
                if (!entry.request.importedPrefabThumbnailContinuation ||
                    entry.request.assetId.ToString() !=
                        m_activeImportedPrefabThumbnailContinuationAssetId)
                {
                    return false;
                }
                const auto state = m_thumbnailStatesByCacheKey.find(entry.cacheKey);
                return state == m_thumbnailStatesByCacheKey.end() ||
                    (state->second != ThumbnailState::Ready &&
                        state->second != ThumbnailState::Failed &&
                        state->second != ThumbnailState::Cancelled);
            });
        const auto hasLiveDeferredPersistenceOwner = std::any_of(
            m_deferredPersistenceTickets.begin(),
            m_deferredPersistenceTickets.end(),
            [this](const DeferredPersistenceTicket& entry)
            {
                return entry.request.importedPrefabThumbnailContinuation &&
                    entry.request.assetId.ToString() ==
                        m_activeImportedPrefabThumbnailContinuationAssetId;
            });
        if (!hasLiveOwner(m_queuedRequestsByCacheKey) &&
            !hasLiveOwner(m_gpuPreviewResourcePendingRequestsByCacheKey) &&
            !hasLiveOwner(m_gpuPreviewReadbackPendingRequestsByCacheKey) &&
            !hasLiveInFlightOwner &&
            !hasLiveDeferredPersistenceOwner)
        {
            m_activeImportedPrefabThumbnailContinuationAssetId.clear();
        }
    };
    const auto hasIncompleteImportedPrefabContinuationOtherThan =
        [this](const std::string& assetId)
    {
        return std::any_of(
            m_queuedRequestsByCacheKey.begin(),
            m_queuedRequestsByCacheKey.end(),
            [this, &assetId](const auto& entry)
            {
                const auto& [cacheKey, candidate] = entry;
                if (!candidate.importedPrefabThumbnailContinuation)
                    return false;
                const auto candidateAssetId = candidate.assetId.ToString();
                if (candidateAssetId == assetId ||
                    m_completedImportedPrefabThumbnailContinuationAssetIds.find(
                        candidateAssetId) !=
                        m_completedImportedPrefabThumbnailContinuationAssetIds.end())
                {
                    return false;
                }
                const auto state = m_thumbnailStatesByCacheKey.find(cacheKey);
                return state == m_thumbnailStatesByCacheKey.end() ||
                    (state->second != ThumbnailState::Ready &&
                        state->second != ThumbnailState::Failed &&
                        state->second != ThumbnailState::Cancelled);
            });
    };
    refreshActiveImportedPrefabContinuation();
    const auto scheduleCpuPrefabFallback = [this](
        const std::string& cacheKey,
        const AssetThumbnailRequest& request,
        const AssetThumbnailRequest& previewRequest,
        const std::shared_ptr<const PreviewRenderableSnapshot>& preparedSnapshot)
        -> bool
    {
        if (!m_generationCancelToken)
            m_generationCancelToken = std::make_shared<AssetThumbnailGenerationCancelToken>();
        m_generationCancelToken->generation = m_generationSerial;
        const auto cancelToken = m_generationCancelToken;
        const auto performanceCaptureToken =
            NLS::Base::Profiling::PerformanceStageStatsCapture::GetActiveToken();
        const auto scheduledAt = std::chrono::steady_clock::now();
        try
        {
            m_inFlightThumbnails.push_back({
                cacheKey,
                m_generationSerial,
                cancelToken,
                ScheduleThumbnailJobFuture(
                    "AssetThumbnailService.CpuPrefabFallback",
                    [request, previewRequest, preparedSnapshot, cancelToken, performanceCaptureToken, scheduledAt]
                    {
                        NLS::Base::Profiling::PerformanceStageStatsCaptureScope capture(
                            performanceCaptureToken);
                        ScopedThumbnailGenerationStageThread backgroundStageThread(
                            PerformanceStageThread::Background);
                        const auto startedAt = std::chrono::steady_clock::now();
                        RecordThumbnailJobQueueTelemetry(
                            "started",
                            request,
                            "background",
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                startedAt - scheduledAt));
                        auto result = GenerateCanonicalCpuPrefabFallback(
                            request,
                            previewRequest,
                            preparedSnapshot,
                            cancelToken);
                        RecordThumbnailJobResultTelemetry(
                            request,
                            "background",
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - startedAt),
                            result);
                        return result;
                    },
                    NLS::Base::Jobs::JobPriority::High,
                    ThumbnailJobQueue::Background),
                request,
                false,
                false
            });
            return true;
        }
        catch (...)
        {
            return false;
        }
    };

    while (HasQueuedCacheKeys() || HasQueuedGpuPreviewReadback())
    {
        const auto cacheKey = PopNextGpuPreviewCacheKey(
            includeHeavyGpuPreviews,
            previewRenderer.SupportsAsynchronousReadbackPolling());
        if (!cacheKey.has_value())
            break;

        const auto requestIterator = m_queuedRequestsByCacheKey.find(*cacheKey);
        if (requestIterator == m_queuedRequestsByCacheKey.end())
            continue;

        const auto request = requestIterator->second;
        if (request.importedPrefabThumbnailContinuation)
        {
            refreshActiveImportedPrefabContinuation();
            const auto continuationAssetId = request.assetId.ToString();
            const bool completedInCurrentGeneration =
                m_completedImportedPrefabThumbnailContinuationAssetIds.find(
                    continuationAssetId) !=
                m_completedImportedPrefabThumbnailContinuationAssetIds.end();
            const bool completedAliasMustYield =
                m_activeImportedPrefabThumbnailContinuationAssetId.empty() &&
                completedInCurrentGeneration &&
                hasIncompleteImportedPrefabContinuationOtherThan(
                    continuationAssetId);
            if (completedAliasMustYield ||
                (!m_activeImportedPrefabThumbnailContinuationAssetId.empty() &&
                    m_activeImportedPrefabThumbnailContinuationAssetId !=
                        continuationAssetId))
            {
                deferredCacheKeys.push_back(*cacheKey);
                RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                    "defer-imported-prefab-continuation-owner=" +
                        m_activeImportedPrefabThumbnailContinuationAssetId,
                    &request,
                    m_queuedRequestsByCacheKey.size());
                ++deferredImportedPrefabContinuationCount;
                if (deferredImportedPrefabContinuationCount >=
                    maxDeferredGpuPreviewScanPerCall)
                {
                    break;
                }
                continue;
            }
            if (m_activeImportedPrefabThumbnailContinuationAssetId.empty() &&
                !completedInCurrentGeneration)
            {
                m_activeImportedPrefabThumbnailContinuationAssetId =
                    continuationAssetId;
            }
        }
        const auto thumbnailStateIterator = m_thumbnailStatesByCacheKey.find(*cacheKey);
        const auto thumbnailState = thumbnailStateIterator != m_thumbnailStatesByCacheKey.end()
            ? thumbnailStateIterator->second
            : ThumbnailState::Queued;
        const bool pollingPendingReadback = thumbnailState == ThumbnailState::WaitingForGpu;
        const bool pumpingPendingResources = thumbnailState == ThumbnailState::WaitingForResources;
        RecordThumbnailGpuPreviewQueueDecisionTelemetry(
            std::string("dequeue-state=") + ThumbnailStateTelemetryName(thumbnailState),
            &request,
            m_queuedRequestsByCacheKey.size());

        // Legacy Render()-only implementations poll their previous readback
        // from SubmitPreview/Render. New renderers expose an independent poll
        // lane; never call their submit path while the fence is still pending.
        if (pollingPendingReadback &&
            previewRenderer.SupportsAsynchronousReadbackPolling() &&
            m_completedGpuPreviewResultsByCacheKey.find(*cacheKey) ==
                m_completedGpuPreviewResultsByCacheKey.end())
        {
            m_gpuPreviewReadbackPendingCacheKeys.insert(*cacheKey);
            m_queuedRequestsByCacheKey[*cacheKey] = request;
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::WaitingForGpu;
            EnqueueQueuedCacheKey(*cacheKey, request, false);
            RestoreDeferredCacheKeys(deferredCacheKeys);
            return std::nullopt;
        }

        if (IsPresentationRevisionSuperseded(request))
        {
            m_completedGpuPreviewResultsByCacheKey.erase(*cacheKey);
            if (pollingPendingReadback)
            {
                auto readbackRequest = request;
                if (const auto resolved = m_resolvedPreviewRequestsByCacheKey.find(*cacheKey);
                    resolved != m_resolvedPreviewRequestsByCacheKey.end())
                {
                    readbackRequest = resolved->second;
                }

                const bool orphaned = previewRenderer.OrphanReadback({
                    BuildThumbnailPreviewReadbackRequestKey(readbackRequest),
                    readbackRequest.requestRevision});
                if (orphaned)
                {
                    // PopNextGpuPreviewCacheKey removes the pending set entry
                    // while selecting work. Restore it until the renderer polls
                    // the orphaned ticket to a safe retirement.
                    m_gpuPreviewReadbackPendingCacheKeys.insert(*cacheKey);
                    m_queuedRequestsByCacheKey[*cacheKey] = request;
                    m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::WaitingForGpu;
                    EnqueueQueuedCacheKey(*cacheKey, request, false);
                    RestoreDeferredCacheKeys(deferredCacheKeys);
                    return std::nullopt;
                }
                ClearGpuPreviewReadbackPending(*cacheKey);
            }
            else
            {
                m_queuedRequestsByCacheKey.erase(*cacheKey);
                m_queuedThumbnailLaneByCacheKey.erase(*cacheKey);
                ClearGpuPreviewReadbackPending(*cacheKey);
            }
            m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
            m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Cancelled;
            continue;
        }

        // Deletion cancels publication, not GPU lifetime. Keep polling an
        // orphaned readback through the renderer until its ticket is retired;
        // this also releases the render-input and texture-interest leases held
        // by the renderer. No artifact resolution or cache write is needed for
        // an invalidated request.
        if (IsPresentationInvalidated(request))
        {
            m_completedGpuPreviewResultsByCacheKey.erase(*cacheKey);
            if (!pollingPendingReadback)
            {
                m_queuedRequestsByCacheKey.erase(*cacheKey);
                m_queuedThumbnailLaneByCacheKey.erase(*cacheKey);
                ClearGpuPreviewReadbackPending(*cacheKey);
                m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Cancelled;
                PruneInvalidatedPresentationBarrier(request.presentationKey);
                continue;
            }

            m_queuedRequestsByCacheKey.erase(*cacheKey);
            m_queuedThumbnailLaneByCacheKey.erase(*cacheKey);
            auto readbackRequest = request;
            if (const auto resolved = m_resolvedPreviewRequestsByCacheKey.find(*cacheKey);
                resolved != m_resolvedPreviewRequestsByCacheKey.end())
            {
                readbackRequest = resolved->second;
            }
            previewRenderer.OrphanReadback({
                BuildThumbnailPreviewReadbackRequestKey(readbackRequest),
                readbackRequest.requestRevision});
            EditorThumbnailPreviewResult discardedPreview;
            try
            {
                discardedPreview = previewRenderer.Render(readbackRequest);
            }
            catch (...)
            {
                // A renderer failure still must not prevent the request from
                // being discarded. The renderer destructor/rebuild will retire
                // its ticket if the backend cannot poll it here.
                discardedPreview.diagnostic = "thumbnail-invalidated-readback-poll-failed";
            }

            const bool readbackStillPending =
                IsPendingThumbnailPreviewReadbackDiagnostic(discardedPreview.diagnostic);
            if (readbackStillPending)
            {
                TrackGpuPreviewReadbackPending(*cacheKey, readbackRequest);
                m_queuedRequestsByCacheKey[*cacheKey] = request;
                m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::WaitingForGpu;
                EnqueueQueuedCacheKey(*cacheKey, request, false);
                RestoreDeferredCacheKeys(deferredCacheKeys);
                return std::nullopt;
            }

            ClearGpuPreviewReadbackPending(*cacheKey);
            m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Cancelled;
            PruneInvalidatedPresentationBarrier(request.presentationKey);
            continue;
        }
        if (!pollingPendingReadback &&
            !SupportsGpuThumbnailPreview(request))
        {
            deferredCacheKeys.push_back(*cacheKey);
            ++deferredGpuPreviewCount;
            if (deferredGpuPreviewCount >= maxDeferredGpuPreviewScanPerCall)
                break;
            continue;
        }
        const bool resourceContinuation = thumbnailState == ThumbnailState::WaitingForResources;
        if (!pollingPendingReadback &&
            !resourceContinuation &&
            !includeHeavyGpuPreviews &&
            IsHeavyGpuThumbnailPreview(request.kind))
        {
            deferredCacheKeys.push_back(*cacheKey);
            m_gpuDeferredHeavyPreviewCacheKeys.insert(*cacheKey);
            ++deferredHeavyGpuPreviewCount;
            if (deferredHeavyGpuPreviewCount >= maxDeferredGpuPreviewScanPerCall)
                break;
            continue;
        }
        const bool unresolvedHeavyPreview =
            !pollingPendingReadback &&
            IsHeavyGpuThumbnailPreview(request.kind) &&
            request.artifactPath.empty();
        if (unresolvedHeavyPreview && !previewRenderer.Supports(request))
        {
            deferredCacheKeys.push_back(*cacheKey);
            ++deferredGpuPreviewCount;
            if (deferredGpuPreviewCount >= maxDeferredGpuPreviewScanPerCall)
                break;
            continue;
        }

        const auto resolvedPreviewIterator = m_resolvedPreviewRequestsByCacheKey.find(*cacheKey);
        auto previewRequest = request;
        bool previewRequestManifestLookupCompleted = false;
        // This request has now received a real renderer turn. Start its
        // resource/visible deadlines here rather than when the folder first
        // enumerated it, so a large visible queue cannot time out untouched
        // assets before their first manifest or resource operation.
        TrackGpuPreviewResourceRequestStart(*cacheKey, request);
        if (resolvedPreviewIterator != m_resolvedPreviewRequestsByCacheKey.end())
        {
            previewRequest = resolvedPreviewIterator->second;
            previewRequestManifestLookupCompleted = true;
        }
        else if (needsDeferredPreviewRequestResolution(request))
        {
            auto resolutionFutureIterator = m_previewRequestResolutionFuturesByCacheKey.find(*cacheKey);
            if (resolutionFutureIterator == m_previewRequestResolutionFuturesByCacheKey.end())
            {
                try
                {
                    resolutionFutureIterator = m_previewRequestResolutionFuturesByCacheKey.emplace(
                        *cacheKey,
                        ScheduleThumbnailJobFuture(
                            "AssetThumbnailService.ResolvePreviewRequest",
                            [request]
                            {
                                ScopedThumbnailGenerationStageThread backgroundStageThread(
                                    PerformanceStageThread::Background);
                                return ResolveDeferredThumbnailPreviewRequest(request);
                            })).first;
                    scheduledPreviewRequestResolutionCacheKeys.insert(*cacheKey);

                    if (unresolvedHeavyPreview)
                    {
                        size_t previewRequestResolutionCandidateCount = 0u;
                        for (const auto& [queuedCacheKey, queuedRequest] : m_queuedRequestsByCacheKey)
                        {
                            if (previewRequestResolutionCandidateCount >= maxDeferredGpuPreviewScanPerCall)
                                break;
                            if (queuedCacheKey == *cacheKey ||
                                !needsDeferredPreviewRequestResolution(queuedRequest) ||
                                !SupportsGpuThumbnailPreview(queuedRequest) ||
                                (!includeHeavyGpuPreviews && IsHeavyGpuThumbnailPreview(queuedRequest.kind)) ||
                                m_resolvedPreviewRequestsByCacheKey.find(queuedCacheKey) !=
                                    m_resolvedPreviewRequestsByCacheKey.end() ||
                                m_previewRequestResolutionFuturesByCacheKey.find(queuedCacheKey) !=
                                    m_previewRequestResolutionFuturesByCacheKey.end())
                            {
                                continue;
                            }
                            ++previewRequestResolutionCandidateCount;
                            if (!previewRenderer.Supports(queuedRequest))
                                continue;

                            try
                            {
                                m_previewRequestResolutionFuturesByCacheKey.emplace(
                                    queuedCacheKey,
                                    ScheduleThumbnailJobFuture(
                                        "AssetThumbnailService.ResolvePreviewRequest",
                                        [queuedRequest]
                                        {
                                            ScopedThumbnailGenerationStageThread backgroundStageThread(
                                                PerformanceStageThread::Background);
                                            return ResolveDeferredThumbnailPreviewRequest(queuedRequest);
                                        }));
                                scheduledPreviewRequestResolutionCacheKeys.insert(queuedCacheKey);
                            }
                            catch (...)
                            {
                                break;
                            }
                        }
                    }
                }
                catch (...)
                {
                    try
                    {
                        std::promise<AssetThumbnailRequest> fallbackPromise;
                        {
                            ScopedThumbnailGenerationStageThread backgroundStageThread(
                                PerformanceStageThread::Background);
                            fallbackPromise.set_value(ResolveDeferredThumbnailPreviewRequest(request));
                        }
                        resolutionFutureIterator = m_previewRequestResolutionFuturesByCacheKey.emplace(
                            *cacheKey,
                            fallbackPromise.get_future()).first;
                        scheduledPreviewRequestResolutionCacheKeys.insert(*cacheKey);
                    }
                    catch (...)
                    {
                        m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Failed;
                        RestoreDeferredCacheKeys(deferredCacheKeys);
                        return BuildExceptionThumbnailResult(
                            request,
                            "thumbnail-preview-request-resolution-worker-start-failed");
                    }
                }
            }

            const auto resolutionWaitBudget = scheduledPreviewRequestResolutionCacheKeys.find(*cacheKey) !=
                scheduledPreviewRequestResolutionCacheKeys.end()
                ? std::chrono::milliseconds(5)
                : std::chrono::milliseconds(0);
            if (resolutionFutureIterator->second.wait_for(resolutionWaitBudget) != std::future_status::ready)
            {
                pendingPreviewRequestResolution = request;
                deferredCacheKeys.push_back(*cacheKey);
                ++deferredGpuPreviewCount;
                if (deferredGpuPreviewCount >= maxDeferredGpuPreviewScanPerCall)
                    break;
                continue;
            }

            try
            {
                previewRequest = resolutionFutureIterator->second.get();
            }
            catch (...)
            {
                m_previewRequestResolutionFuturesByCacheKey.erase(resolutionFutureIterator);
                m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Failed;
                RestoreDeferredCacheKeys(deferredCacheKeys);
                return BuildExceptionThumbnailResult(
                    request,
                    "thumbnail-preview-request-resolution-failed");
            }
            m_previewRequestResolutionFuturesByCacheKey.erase(resolutionFutureIterator);
            m_resolvedPreviewRequestsByCacheKey[*cacheKey] = previewRequest;
            previewRequestManifestLookupCompleted = true;
            if (previewRequest.artifactPath.empty() &&
                unresolvedHeavyPreview &&
                ThumbnailArtifactManifestExceedsPreviewBudget(request))
            {
                deferredCacheKeys.push_back(*cacheKey);
                ++deferredHeavyGpuPreviewCount;
                if (deferredHeavyGpuPreviewCount >= maxDeferredGpuPreviewScanPerCall)
                    break;
                continue;
            }
        }
        if (!previewRenderer.Supports(previewRequest))
        {
            deferredCacheKeys.push_back(*cacheKey);
            ++deferredGpuPreviewCount;
            if (deferredGpuPreviewCount >= maxDeferredGpuPreviewScanPerCall)
                break;
            continue;
        }

        auto completeTerminalResourcePumpFailure = [&](const auto& pump, const auto& evaluation)
            -> std::optional<AssetThumbnailServiceResult>
        {
            if (!pump.supported || pump.resourcesPending || pump.diagnostic.empty())
                return std::nullopt;

            const bool complexityPending =
                pump.diagnostic == kPrefabPreviewBudgetExceededDiagnostic ||
                pump.diagnostic == "thumbnail-model-preview-budget-exceeded" ||
                pump.diagnostic == "thumbnail-material-preview-budget-exceeded";
            if (complexityPending)
            {
                auto nextPreviewRequest = previewRequest;
                if (pump.diagnostic == kPrefabPreviewBudgetExceededDiagnostic &&
                    ShouldRetryLegacyImportedPrefabBudgetFailure(request))
                {
                    const auto meshPaths = ResolveMeshArtifactPaths(previewRequest);
                    if (!meshPaths.empty())
                    {
                        nextPreviewRequest.kind = AssetThumbnailKind::ModelPreview;
                        nextPreviewRequest.artifactPath = meshPaths.front().generic_string();
                    }
                }
                m_queuedRequestsByCacheKey[*cacheKey] = request;
                m_resolvedPreviewRequestsByCacheKey[*cacheKey] = std::move(nextPreviewRequest);
                m_gpuPreviewResourcePendingRequestsByCacheKey[*cacheKey] = request;
                TrackGpuPreviewResourceRequestStart(*cacheKey, request);
                EnqueueQueuedCacheKey(*cacheKey, request, false);
                m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::WaitingForResources;
                auto result = BuildResultFromEvaluation(
                    request,
                    evaluation,
                    AssetThumbnailServiceStatus::Pending);
                result.diagnostic = "thumbnail-gpu-preview-complexity-pending";
                return result;
            }

            if (pump.diagnostic == kLargePrefabPreviewAwaitingResidentDiagnostic)
            {
                auto result = BuildResultFromEvaluation(
                    request,
                    evaluation,
                    AssetThumbnailServiceStatus::Fallback);
                result.diagnostic = pump.diagnostic;
                const auto metadataRequest = BuildResolvedThumbnailCacheRequest(
                    request,
                    previewRequest);
                WriteThumbnailMetadataForEvaluation(
                    request,
                    evaluation,
                    AssetThumbnailCacheStatus::Failed,
                    result.diagnostic,
                    &metadataRequest);
                RemoveQueuedCacheKeyOccurrences(*cacheKey);
                m_queuedRequestsByCacheKey.erase(*cacheKey);
                m_queuedThumbnailLaneByCacheKey.erase(*cacheKey);
                m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
                m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
                ClearGpuPreviewResourcePending(*cacheKey);
                m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Failed;
                return result;
            }

            auto result = BuildResultFromEvaluation(
                request,
                evaluation,
                AssetThumbnailServiceStatus::Failed);
            result.diagnostic = pump.diagnostic;
            const auto metadataRequest = BuildResolvedThumbnailCacheRequest(request, previewRequest);
            WriteThumbnailMetadataForEvaluation(
                request,
                evaluation,
                AssetThumbnailCacheStatus::Failed,
                result.diagnostic,
                &metadataRequest);
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Failed;
            m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
            m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
            ClearGpuPreviewResourcePending(*cacheKey);
            return result;
        };

        // A resource-pending GPU preview is normally retried as dependencies
        // become ready. Bound that retry path so a missing dependency or a
        // stalled persistent proxy becomes an explicit failure instead of a
        // permanent Loading state. Retained/previous Canonical presentation is
        // handled by BuildResultFromEvaluation and remains available to UI.
        auto buildResourcePendingResultForDiagnostic = [&](const std::string& diagnostic,
                                                            const auto& evaluation,
                                                            const uint64_t progressToken,
                                                            const bool resourceWorkActive)
            -> std::optional<AssetThumbnailServiceResult>
        {
            const auto now = std::chrono::steady_clock::now();
            auto& deferral = m_gpuPreviewResourcePendingDeferralsByCacheKey[*cacheKey];
            if (deferral.retryCount == 0u)
            {
                deferral.firstDeferredAt = now;
                deferral.lastProgressAt = now;
                deferral.progressToken = progressToken;
            }
            else if (ShouldRefreshGpuPreviewResourceProgress(
                         deferral.progressToken,
                         progressToken,
                         resourceWorkActive))
            {
                deferral.progressToken = progressToken;
                deferral.lastProgressAt = now;
            }
            deferral.resourceWorkActive = resourceWorkActive;
            if (deferral.lastProgressAt.time_since_epoch().count() == 0)
                deferral.lastProgressAt = deferral.firstDeferredAt;
            ++deferral.retryCount;

            const bool sceneLoadDeadlineSuspended =
                request.residentPrefabPreviewSource.has_value() &&
                request.residentPrefabPreviewSource->HasIdentity() &&
                SuspendResidentGpuPreviewResourceDeadlineForSceneLoad(
                    *cacheKey,
                    request,
                    now);
            const bool retryBudgetExhausted =
                !sceneLoadDeadlineSuspended &&
                deferral.lastProgressAt.time_since_epoch().count() != 0 &&
                now - deferral.lastProgressAt >= kGpuPreviewResourcePendingTimeout;
            if (retryBudgetExhausted)
            {
                const auto retryCount = deferral.retryCount;
                auto result = BuildResultFromEvaluation(
                    request,
                    evaluation,
                    AssetThumbnailServiceStatus::Failed);
                result.diagnostic = "thumbnail-gpu-preview-resources-timeout:" + diagnostic;
                const auto metadataRequest = BuildResolvedThumbnailCacheRequest(request, previewRequest);
                WriteThumbnailMetadataForEvaluation(
                    request,
                    evaluation,
                    AssetThumbnailCacheStatus::Failed,
                    result.diagnostic,
                    &metadataRequest);
                m_queuedRequestsByCacheKey.erase(*cacheKey);
                m_queuedThumbnailLaneByCacheKey.erase(*cacheKey);
                m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Failed;
                m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
                m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
                ClearGpuPreviewResourcePending(*cacheKey);
                m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(*cacheKey);
                m_gpuPreviewEmptyFrameDeferralsByCacheKey.erase(*cacheKey);
                m_terminalPresentationRevisions[result.presentationKey] =
                    (std::max)(
                        m_terminalPresentationRevisions[result.presentationKey],
                        result.requestRevision);
                m_terminalThumbnailResultsByCacheKey.insert_or_assign(*cacheKey, result);
                ClearVisibleThumbnailRequestStart(request);
                RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                    "resource-pending-timeout|retryCount=" +
                        std::to_string(retryCount) + "|diagnostic=" + diagnostic,
                    &request,
                    m_queuedRequestsByCacheKey.size());
                return result;
            }

            m_queuedRequestsByCacheKey[*cacheKey] = request;
            m_gpuPreviewResourcePendingRequestsByCacheKey[*cacheKey] = request;
            TrackGpuPreviewResourceRequestStart(*cacheKey, request);
            if (!previewRequest.artifactPath.empty())
                m_resolvedPreviewRequestsByCacheKey[*cacheKey] = previewRequest;
            // A visible resident request is cheap to retry, but a resource
            // pending result must yield the resident lane to its siblings.
            // Requeueing it at the front can repeatedly select the same heavy
            // prefab and starve the other visible assets.
            const bool residentRequest = IsVisibleResidentThumbnailRequest(request);
            EnqueueQueuedCacheKey(*cacheKey, request, !residentRequest);
            // A visible resident request is cheap to retry once its snapshot
            // becomes ready, but it must yield one scheduler turn when the
            // resource report is still pending. Resident reports are often
            // complete enough to use (truncated=0), so relying on the
            // truncated diagnostic alone lets one resident key monopolize the
            // visible lane and starve ordinary visible requests.
            if (IsTruncatedThumbnailPreviewResourcesDiagnostic(diagnostic) ||
                IsVisibleResidentThumbnailRequest(request))
                m_gpuPreviewResourcePendingDeferredCacheKeys.insert(*cacheKey);
            else
                m_gpuPreviewResourcePendingDeferredCacheKeys.erase(*cacheKey);
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::WaitingForResources;
            auto result = BuildResultFromEvaluation(
                request,
                evaluation,
                AssetThumbnailServiceStatus::Pending);
            result.diagnostic = diagnostic;
            return result;
        };
        auto buildResourcePendingResult = [&](const auto& pump, const auto& evaluation)
            -> std::optional<AssetThumbnailServiceResult>
        {
            if (!pump.supported || !pump.resourcesPending)
            {
                m_gpuPreviewResourcePendingDeferredCacheKeys.erase(*cacheKey);
                return std::nullopt;
            }

            return buildResourcePendingResultForDiagnostic(
                pump.diagnostic.empty()
                    ? std::string("thumbnail-gpu-preview-resources-pending")
                    : pump.diagnostic,
                evaluation,
                pump.resourceProgressToken,
                pump.resourceWorkActive);
        };

        std::optional<AssetThumbnailCacheEvaluation> pumpedEvaluation;
        bool pumpedFreshnessVerified = false;
        if (pumpingPendingResources)
        {
            pumpedEvaluation = EvaluateAssetThumbnailCache(request);
            const auto& evaluation = *pumpedEvaluation;
            if (!evaluation.entry.has_value())
            {
                m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Failed;
                m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
                m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
                RestoreDeferredCacheKeys(deferredCacheKeys);
                return BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
            }
            if (!IsThumbnailRequestStillFresh(request))
            {
                m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Cancelled;
                m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
                m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
                RestoreDeferredCacheKeys(deferredCacheKeys);
                return BuildStaleThumbnailRequestResult(request, evaluation);
            }
            pumpedFreshnessVerified = true;

            const auto pumpTelemetryBegin = std::chrono::steady_clock::now();
            const auto pump = previewRenderer.PumpResources(previewRequest);
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareResources,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - pumpTelemetryBegin),
                0u,
                BuildThumbnailGpuPreviewRenderTelemetryPath(previewRequest, {}) + "|pump-resources-call"
            });
            if (auto failed = completeTerminalResourcePumpFailure(pump, evaluation);
                failed.has_value())
            {
                RestoreDeferredCacheKeys(deferredCacheKeys);
                return failed;
            }
            if (auto resourcePending = buildResourcePendingResult(pump, evaluation);
                resourcePending.has_value())
            {
                RestoreDeferredCacheKeys(deferredCacheKeys);
                return resourcePending;
            }
        }

        if (!pollingPendingReadback &&
            (m_generationBudget.previewRenderCountBudget == 0u ||
                m_generationBudget.readbackCountBudget == 0u))
        {
            RestoreDeferredCacheKeys(deferredCacheKeys);
            m_queuedRequestsByCacheKey[*cacheKey] = request;
            EnqueueQueuedCacheKey(*cacheKey, request);
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Queued;
            return std::nullopt;
        }
        const auto estimatedCpuPreparationBytes = EstimateThumbnailCpuPreparationBytes(previewRequest);
        const auto estimatedGpuUploadBytes = EstimateThumbnailGpuUploadBytes(previewRequest);
        if (!pollingPendingReadback &&
            (!HasThumbnailBudget(m_generationBudget.cpuPreparationByteBudget, estimatedCpuPreparationBytes) ||
                !HasThumbnailBudget(m_generationBudget.gpuUploadByteBudget, estimatedGpuUploadBytes)))
        {
            RestoreDeferredCacheKeys(deferredCacheKeys);
            m_queuedRequestsByCacheKey[*cacheKey] = request;
            EnqueueQueuedCacheKey(*cacheKey, request);
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Queued;
            return std::nullopt;
        }

        m_thumbnailStatesByCacheKey[*cacheKey] = pollingPendingReadback
            ? ThumbnailState::WaitingForGpu
            : ThumbnailState::Rendering;
        m_queuedThumbnailLaneByCacheKey.erase(requestIterator->first);
        m_queuedRequestsByCacheKey.erase(requestIterator);

        const auto evaluation = pumpedEvaluation.has_value()
            ? std::move(*pumpedEvaluation)
            : EvaluateAssetThumbnailCache(request);
        if (const auto invalidPathDiagnostic = ValidateGpuPreviewRequestArtifactPaths(
                previewRequest,
                previewRequestManifestLookupCompleted);
            invalidPathDiagnostic.has_value())
        {
            auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
            result.diagnostic = *invalidPathDiagnostic;
            const auto metadataRequest = BuildResolvedThumbnailCacheRequest(request, previewRequest);
            WriteThumbnailMetadataForEvaluation(
                request,
                evaluation,
                AssetThumbnailCacheStatus::Failed,
                result.diagnostic,
                &metadataRequest);
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Failed;
            m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
            m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
            RestoreDeferredCacheKeys(deferredCacheKeys);
            return result;
        }
        if (!evaluation.entry.has_value())
        {
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Failed;
            m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
            m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
            RestoreDeferredCacheKeys(deferredCacheKeys);
            return BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
        }
        if (!pumpedFreshnessVerified && !IsThumbnailRequestStillFresh(request))
        {
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Cancelled;
            m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
            m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
            RestoreDeferredCacheKeys(deferredCacheKeys);
            return BuildStaleThumbnailRequestResult(request, evaluation);
        }
        if (IsThumbnailGenerationCancelled(m_generationCancelToken))
        {
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Cancelled;
            m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
            m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
            RestoreDeferredCacheKeys(deferredCacheKeys);
            return BuildCancelledThumbnailRequestResult(request, evaluation);
        }

        if (pollingPendingReadback)
        {
            const auto deferral = m_gpuPreviewReadbackPendingDeferralsByCacheKey.find(*cacheKey);
            if (deferral != m_gpuPreviewReadbackPendingDeferralsByCacheKey.end() &&
                m_completedGpuPreviewResultsByCacheKey.find(*cacheKey) ==
                    m_completedGpuPreviewResultsByCacheKey.end() &&
                deferral->second.firstPendingAt.time_since_epoch().count() != 0 &&
                std::chrono::steady_clock::now() - deferral->second.firstPendingAt >=
                    kGpuPreviewReadbackPendingTimeout)
            {
                auto readbackRequest = request;
                if (const auto pendingReadback =
                        m_gpuPreviewReadbackPendingRequestsByCacheKey.find(*cacheKey);
                    pendingReadback != m_gpuPreviewReadbackPendingRequestsByCacheKey.end())
                {
                    readbackRequest = pendingReadback->second;
                }
                (void)previewRenderer.OrphanReadback({
                    BuildThumbnailPreviewReadbackRequestKey(readbackRequest),
                    readbackRequest.requestRevision});

                auto result = BuildResultFromEvaluation(
                    request,
                    evaluation,
                    AssetThumbnailServiceStatus::Failed);
                result.diagnostic = "thumbnail-gpu-preview-readback-timeout";
                result.revokeGpuTexture = true;
                result.gpuTextureGeneration = m_generationSerial;
                const auto metadataRequest = BuildResolvedThumbnailCacheRequest(request, previewRequest);
                WriteThumbnailMetadataForEvaluation(
                    request,
                    evaluation,
                    AssetThumbnailCacheStatus::Failed,
                    result.diagnostic,
                    &metadataRequest);
                RemoveQueuedCacheKeyOccurrences(*cacheKey);
                m_queuedRequestsByCacheKey.erase(*cacheKey);
                m_queuedThumbnailLaneByCacheKey.erase(*cacheKey);
                m_completedGpuPreviewResultsByCacheKey.erase(*cacheKey);
                m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
                m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
                ClearGpuPreviewResourcePending(*cacheKey);
                m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Failed;
                ClearGpuPreviewReadbackPending(*cacheKey);
                RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                    "readback-timeout",
                    &request,
                    m_gpuPreviewReadbackPendingCacheKeys.size());
                RestoreDeferredCacheKeys(deferredCacheKeys);
                return result;
            }
        }

        if (!pollingPendingReadback &&
            !pumpingPendingResources &&
            request.kind == AssetThumbnailKind::PrefabPreview)
        {
            const auto pumpTelemetryBegin = std::chrono::steady_clock::now();
            const auto pump = previewRenderer.PumpResources(previewRequest);
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewPrepareResources,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - pumpTelemetryBegin),
                0u,
                BuildThumbnailGpuPreviewRenderTelemetryPath(previewRequest, {}) + "|pump-resources-call"
            });
            if (auto failed = completeTerminalResourcePumpFailure(pump, evaluation);
                failed.has_value())
            {
                RestoreDeferredCacheKeys(deferredCacheKeys);
                return failed;
            }
            if (auto resourcePending = buildResourcePendingResult(pump, evaluation);
                resourcePending.has_value())
            {
                RestoreDeferredCacheKeys(deferredCacheKeys);
                return resourcePending;
            }
        }

        NLS::Editor::Assets::EditorThumbnailPreviewResult preview;
        bool consumedCompletedReadback = false;
        bool consumedReadyResidentMarker = false;
        const auto restoreReadyResidentMarker = [&]()
        {
            if (consumedReadyResidentMarker)
                m_gpuPreviewReadyResidentCacheKeys.insert(*cacheKey);
        };
        if (pollingPendingReadback)
        {
            const auto completed = m_completedGpuPreviewResultsByCacheKey.find(*cacheKey);
            if (completed != m_completedGpuPreviewResultsByCacheKey.end() &&
                completed->second.preview != nullptr &&
                (completed->second.requestRevision == 0u ||
                    completed->second.requestRevision <= request.requestRevision))
            {
                preview = std::move(*completed->second.preview);
                m_completedGpuPreviewResultsByCacheKey.erase(completed);
                consumedCompletedReadback = true;
            }
        }
        if (!consumedCompletedReadback)
        {
            NLS::Base::Profiling::PerformanceStageScope previewScope(
                NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
                "PreparePreviewAsset",
            NLS::Base::Profiling::PerformanceStageThread::Main);
            const auto previewTelemetryBegin = std::chrono::steady_clock::now();
            // The marker is consumed only when a render submission actually
            // starts. PumpResources may still report a provisional assembly
            // result, in which case the marker must remain available for the
            // next heavy-lane retry.
            consumedReadyResidentMarker =
                m_gpuPreviewReadyResidentCacheKeys.erase(*cacheKey) != 0u;
            if (consumedReadyResidentMarker)
            {
                RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                    "ready-resident-marker-consumed-submit",
                    &request,
                    m_queuedRequestsByCacheKey.size());
            }
            auto submitted = previewRenderer.SubmitPreparedPreview(previewRequest);
            preview = std::move(submitted.preview);
            if (IsPendingPrefabPreviewSceneAssemblyDiagnostic(preview.diagnostic))
            {
                // The renderer owns one transient prefab scene. Switching to a
                // sibling request releases the proxy objects and restarts its
                // bounded assembly cursor, so keep this continuation selected
                // until assembly reaches render/readback or a terminal result.
                m_gpuPreviewSceneAssemblyContinuationCacheKey = *cacheKey;
            }
            else if (m_gpuPreviewSceneAssemblyContinuationCacheKey == *cacheKey)
            {
                m_gpuPreviewSceneAssemblyContinuationCacheKey.clear();
            }
            if (!preview.completedPendingReadback &&
                !(pollingPendingReadback &&
                    IsPendingThumbnailPreviewReadbackDiagnostic(preview.diagnostic)))
            {
                NLS::Core::Assets::RecordArtifactLoadTelemetry({
                    NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRender,
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - previewTelemetryBegin),
                    preview.rgbaPixels.size(),
                    BuildThumbnailGpuPreviewRenderTelemetryPath(previewRequest, preview)
                });
            }
        }
        const auto publishedGpuTexture = preview.publishableGpuTexture
            ? preview.gpuTexture
            : AssetThumbnailGpuTexture {};
        const auto ApplyPublishedGpuPresentation =
            [&publishedGpuTexture](AssetThumbnailServiceResult& result)
        {
            if (!publishedGpuTexture.IsValid())
                return;

            // PNG readback/persistence is deliberately independent from the
            // display state. The submitted GPU frame is already the current
            // canonical presentation even while its durable copy is pending.
            result.presentationState = ThumbnailPresentationState::Ready;
            result.previewQuality = ThumbnailPreviewQuality::Canonical;
            result.refreshPending = true;
            result.failureRetained = false;
        };
        if (preview.persistenceDeferred && publishedGpuTexture.IsValid())
        {
            // Readback persistence is best effort once the bounded GPU queues
            // are saturated. The submitted frame is still a complete,
            // canonical presentation and must remain visible instead of being
            // routed through the failure/revoke path below.
            auto result = BuildResultFromEvaluation(
                request,
                evaluation,
                AssetThumbnailServiceStatus::Pending);
            result.diagnostic = preview.diagnostic.empty()
                ? "thumbnail-gpu-preview-persistence-deferred"
                : preview.diagnostic;
            result.gpuTexture = publishedGpuTexture;
            result.gpuTextureGeneration = m_generationSerial;
            ApplyPublishedGpuPresentation(result);
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Ready;
            m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
            m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
            m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(*cacheKey);
            m_gpuPreviewEmptyFrameDeferralsByCacheKey.erase(*cacheKey);
            ClearGpuPreviewReadbackPending(*cacheKey);
            ClearGpuPreviewResourcePending(*cacheKey);
            RestoreDeferredCacheKeys(deferredCacheKeys);
            return result;
        }
        if (!pollingPendingReadback)
        {
            ConsumeThumbnailByteBudget(
                m_generationBudget.cpuPreparationByteBudget,
                estimatedCpuPreparationBytes,
                m_hasExplicitGenerationBudget);
        }
        if (!pollingPendingReadback &&
            m_hasExplicitGenerationBudget)
        {
            ConsumeThumbnailCountBudget(m_generationBudget.previewRenderCountBudget, true);
        }
        if (preview.residentPreviewPartial && publishedGpuTexture.IsValid())
        {
            // Threaded resident previews publish their render target directly
            // and intentionally omit CPU pixels. Classify that frame before the
            // generic empty-result branch so it remains visible and parked on
            // the registry revision instead of being revoked as a failed frame.
            auto pending = BuildResultFromEvaluation(
                request,
                evaluation,
                AssetThumbnailServiceStatus::Pending);
            pending.diagnostic = "thumbnail-gpu-preview-resident-partial";
            pending.gpuTexture = publishedGpuTexture;
            pending.gpuTextureGeneration = m_generationSerial;
            ApplyPublishedGpuPresentation(pending);
            restoreReadyResidentMarker();
            m_stableThumbnailResultsByCacheKey[*cacheKey] = pending;
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::WaitingForResources;
            m_gpuPreviewResourcePendingRequestsByCacheKey[*cacheKey] = request;
            if (!previewRequest.artifactPath.empty())
                m_resolvedPreviewRequestsByCacheKey[*cacheKey] = previewRequest;
            m_gpuPreviewResidentPartialRevisionByCacheKey[*cacheKey] =
                request.residentPreviewRevision;
            TrackGpuPreviewResourceRequestStart(*cacheKey, request);
            m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
            m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(*cacheKey);
            m_gpuPreviewEmptyFrameDeferralsByCacheKey.erase(*cacheKey);
            ClearGpuPreviewReadbackPending(*cacheKey);
            m_gpuPreviewResourcePendingDeferredCacheKeys.insert(*cacheKey);
            RestoreDeferredCacheKeys(deferredCacheKeys);
            return pending;
        }
        if (preview.rgbaPixels.empty() || preview.width == 0u || preview.height == 0u)
        {
            const auto diagnostic = preview.diagnostic.empty()
                ? std::string("thumbnail-gpu-preview-generation-failed")
                : preview.diagnostic;
            if (diagnostic == kPrefabPreviewBudgetExceededDiagnostic)
            {
                auto result = BuildResultFromEvaluation(
                    request,
                    evaluation,
                    AssetThumbnailServiceStatus::Pending);
                result.diagnostic = "thumbnail-gpu-preview-complexity-pending";
                m_queuedRequestsByCacheKey[*cacheKey] = request;
                m_resolvedPreviewRequestsByCacheKey[*cacheKey] = previewRequest;
                m_gpuPreviewResourcePendingRequestsByCacheKey[*cacheKey] = request;
                TrackGpuPreviewResourceRequestStart(*cacheKey, request);
                EnqueueQueuedCacheKey(*cacheKey, request, false);
                m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::WaitingForResources;
                RestoreDeferredCacheKeys(deferredCacheKeys);
                return result;
            }
            const bool retryableGpuFailure = IsRetryableThumbnailFailureDiagnostic(diagnostic);
            auto result = BuildResultFromEvaluation(
                request,
                evaluation,
                retryableGpuFailure
                    ? AssetThumbnailServiceStatus::Pending
                    : AssetThumbnailServiceStatus::Failed);
            result.diagnostic = diagnostic;
            const bool readbackPending =
                IsPendingThumbnailPreviewReadbackDiagnostic(diagnostic);
            if (publishedGpuTexture.IsValid() && readbackPending)
            {
                result.gpuTexture = publishedGpuTexture;
                result.gpuTextureGeneration = m_generationSerial;
                ApplyPublishedGpuPresentation(result);
            }
            else if (publishedGpuTexture.IsValid())
            {
                // A failed or empty validation must revoke the direct image
                // that may already have been handed to the UI.
                result.revokeGpuTexture = true;
                result.gpuTextureGeneration = m_generationSerial;
            }
            if (!retryableGpuFailure)
            {
                const auto metadataRequest = BuildResolvedThumbnailCacheRequest(request, previewRequest);
                WriteThumbnailMetadataForEvaluation(
                    request,
                    evaluation,
                    AssetThumbnailCacheStatus::Failed,
                    result.diagnostic,
                    &metadataRequest);
            }
            if (retryableGpuFailure)
            {
                const bool resourcesPending = IsPendingThumbnailPreviewResourcesDiagnostic(diagnostic);
                if (resourcesPending)
                {
                    auto resourcePending = buildResourcePendingResultForDiagnostic(
                        diagnostic,
                        evaluation,
                        preview.resourceProgressToken,
                        preview.resourceWorkActive);
                    if (resourcePending.has_value())
                    {
                        if (publishedGpuTexture.IsValid())
                        {
                            resourcePending->revokeGpuTexture = true;
                            resourcePending->gpuTextureGeneration = m_generationSerial;
                        }
                        RestoreDeferredCacheKeys(deferredCacheKeys);
                        return resourcePending;
                    }
                }
                if (!pollingPendingReadback &&
                    IsPendingThumbnailPreviewReadbackDiagnostic(diagnostic) &&
                    m_hasExplicitGenerationBudget)
                {
                    ConsumeThumbnailCountBudget(m_generationBudget.readbackCountBudget, true);
                }
                m_queuedRequestsByCacheKey[*cacheKey] = request;
                if (!previewRequest.artifactPath.empty())
                    m_resolvedPreviewRequestsByCacheKey[*cacheKey] = previewRequest;
                const bool residentRequest = IsVisibleResidentThumbnailRequest(request);
                EnqueueQueuedCacheKey(
                    *cacheKey,
                    request,
                    resourcesPending && !residentRequest);
                if (IsTruncatedThumbnailPreviewResourcesDiagnostic(diagnostic) ||
                    IsVisibleResidentThumbnailRequest(request))
                {
                    m_gpuPreviewResourcePendingDeferredCacheKeys.insert(*cacheKey);
                }
                else if (!resourcesPending)
                    ClearGpuPreviewResourcePending(*cacheKey);
                RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                    std::string("requeue-retryable=") + diagnostic,
                    &request,
                    m_queuedRequestsByCacheKey.size());
            }
            const bool pendingReadbackFailure = IsPendingThumbnailPreviewReadbackDiagnostic(diagnostic);
            // A promotion marker may have been consumed immediately before
            // SubmitPreview.  The renderer can still report one provisional
            // frame while its render-thread resource bindings catch up, and a
            // second weak registry lookup can transiently observe the old
            // partial snapshot.  Preserve the marker that was actually
            // consumed for this submission; otherwise the request falls back
            // behind the heavy-preview cooldown and can remain partial forever.
            if (!pendingReadbackFailure &&
                (consumedReadyResidentMarker || IsCompleteResidentThumbnailRequest(request)))
            {
                // The renderer can still report a provisional resident frame
                // after the registry has completed its package (for example,
                // while the final resource bindings cross a render-thread
                // boundary). Keep the one-shot promotion marker so this
                // retry returns through the canonical resident lane instead
                // of falling back to the heavy-preview cooldown.
                m_gpuPreviewReadyResidentCacheKeys.insert(*cacheKey);
            }
            m_thumbnailStatesByCacheKey[*cacheKey] = retryableGpuFailure
                ? (pendingReadbackFailure
                    ? ThumbnailState::WaitingForGpu
                    : (IsPendingThumbnailPreviewResourcesDiagnostic(diagnostic)
                        ? ThumbnailState::WaitingForResources
                        : ThumbnailState::Queued))
                : ThumbnailState::Failed;
            if (pendingReadbackFailure)
            {
                TrackGpuPreviewReadbackPending(*cacheKey, previewRequest);
                RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                    "track-readback-pending",
                    &request,
                    m_gpuPreviewReadbackPendingCacheKeys.size());
            }
            else
            {
                ClearGpuPreviewReadbackPending(*cacheKey);
                // A resident partial frame is provisional presentation, not a
                // failed completion. If the completion marker was published by
                // the registry maintenance pass, keep it across this transient
                // retry so the next heavy lane can submit the canonical frame.
                const bool residentRequest = request.residentPrefabPreviewSource.has_value() &&
                    request.residentPrefabPreviewSource->HasIdentity();
                if (!IsPendingThumbnailPreviewResourcesDiagnostic(diagnostic) &&
                    !IsVisibleResidentThumbnailRequest(request) &&
                    !residentRequest)
                    ClearGpuPreviewResourcePending(*cacheKey);
            }
            if (!retryableGpuFailure)
            {
                m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
                m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
            }
            RestoreDeferredCacheKeys(deferredCacheKeys);
            return result;
        }
        const auto clearFrameDisposition = EvaluateGpuPreviewClearFrameDisposition(
            request,
            preview.rgbaPixels,
            preview.width,
            preview.height,
            preview.submittedSceneDrawCount);
        const auto recordGpuPreviewTerminalDiagnostic = [&](const std::string& diagnostic)
        {
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRender,
                std::chrono::microseconds(0),
                preview.rgbaPixels.size(),
                BuildThumbnailGpuPreviewRenderTelemetryPath(
                    previewRequest,
                    preview,
                    diagnostic)
            });
        };
        const bool emptyFrameRetriesExhausted =
            clearFrameDisposition == GpuPreviewClearFrameDisposition::DeferEmptyFrame &&
            [this, &cacheKey]
            {
                const auto deferralIterator = m_gpuPreviewEmptyFrameDeferralsByCacheKey.find(*cacheKey);
                return deferralIterator != m_gpuPreviewEmptyFrameDeferralsByCacheKey.end() &&
                    deferralIterator->second.retryCount >= kMaxGpuPreviewEmptyFrameRetriesPerGeneration;
            }();
        if (clearFrameDisposition == GpuPreviewClearFrameDisposition::DeferEmptyFrame &&
            !emptyFrameRetriesExhausted)
        {
            // Empty-frame deferral owns the next retry. Do not leave a stale
            // resource-continuation entry behind, or a generation supersede
            // can incorrectly preserve WaitingForResources for this request.
            ClearGpuPreviewResourcePending(*cacheKey);
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "completed-readback-disposition=empty-frame",
                &request,
                m_inFlightThumbnails.size());
            recordGpuPreviewTerminalDiagnostic("thumbnail-gpu-preview-empty-frame");
            auto result = BuildResultFromEvaluation(
                request,
                evaluation,
                AssetThumbnailServiceStatus::Pending);
            result.diagnostic = "thumbnail-gpu-preview-empty-frame";
            m_gpuPreviewEmptyFrameDeferredCacheKeys.insert(*cacheKey);
            auto& deferral = m_gpuPreviewEmptyFrameDeferralsByCacheKey[*cacheKey];
            deferral.request = request;
            ++deferral.retryCount;
            deferral.lastDeferredAt = std::chrono::steady_clock::now();
            if (!previewRequest.artifactPath.empty())
                m_resolvedPreviewRequestsByCacheKey[*cacheKey] = previewRequest;
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Queued;
            RestoreDeferredCacheKeys(deferredCacheKeys);
            return result;
        }
        const bool largePrefabPreview =
            preview.expectedSceneDrawCount > kMaxColdGpuPreviewPrefabDrawItems ||
            (preview.previewSnapshot != nullptr &&
                preview.previewSnapshot->drawItems.size() >
                    kMaxColdGpuPreviewPrefabDrawItems);
        const bool shouldUseCpuPrefabFallback =
            request.kind == AssetThumbnailKind::PrefabPreview &&
            !largePrefabPreview &&
            (clearFrameDisposition == GpuPreviewClearFrameDisposition::FailEmptyFrame ||
                emptyFrameRetriesExhausted);
        if (shouldUseCpuPrefabFallback)
        {
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "completed-readback-disposition=empty-frame-cpu-fallback",
                &request,
                m_inFlightThumbnails.size());
            recordGpuPreviewTerminalDiagnostic("thumbnail-gpu-preview-empty-frame");
            m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(*cacheKey);
            m_gpuPreviewEmptyFrameDeferralsByCacheKey.erase(*cacheKey);
            m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
            m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
            ClearGpuPreviewResourcePending(*cacheKey);
            ClearGpuPreviewReadbackPending(*cacheKey);
            auto result = BuildResultFromEvaluation(
                request,
                evaluation,
                AssetThumbnailServiceStatus::Pending);
            result.revokeGpuTexture = publishedGpuTexture.IsValid();
            if (publishedGpuTexture.IsValid())
                result.gpuTextureGeneration = m_generationSerial;
            if (scheduleCpuPrefabFallback(
                    *cacheKey,
                    request,
                    previewRequest,
                    preview.previewSnapshot))
            {
                result.diagnostic =
                    "thumbnail-gpu-preview-empty-frame-cpu-fallback-pending";
                m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Preparing;
            }
            else
            {
                result.status = AssetThumbnailServiceStatus::Failed;
                result.diagnostic =
                    "thumbnail-gpu-preview-empty-frame-cpu-fallback-schedule-failed";
                m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Failed;
                if (evaluation.entry.has_value())
                {
                    (void)ClearAssetThumbnailCacheFailureMetadata(
                        request,
                        *evaluation.entry);
                }
            }
            RestoreDeferredCacheKeys(deferredCacheKeys);
            return result;
        }
        if (clearFrameDisposition == GpuPreviewClearFrameDisposition::FailEmptyFrame ||
            emptyFrameRetriesExhausted)
        {
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "completed-readback-disposition=empty-frame",
                &request,
                m_inFlightThumbnails.size());
            recordGpuPreviewTerminalDiagnostic("thumbnail-gpu-preview-empty-frame");
            auto result = BuildGpuPreviewEmptyFrameResult(request, evaluation, previewRequest);
            result.revokeGpuTexture = publishedGpuTexture.IsValid();
            if (publishedGpuTexture.IsValid())
                result.gpuTextureGeneration = m_generationSerial;
            m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(*cacheKey);
            m_gpuPreviewEmptyFrameDeferralsByCacheKey.erase(*cacheKey);
            m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
            m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Failed;
            ClearGpuPreviewReadbackPending(*cacheKey);
            RestoreDeferredCacheKeys(deferredCacheKeys);
            return result;
        }

        RestoreDeferredCacheKeys(deferredCacheKeys);
        // The completed readback no longer needs one-shot cold-loaded Prefab
        // inputs. Release them now, but keep the imported-continuation scheduler
        // owner until the canonical PNG is durably committed; otherwise the
        // next large asset can overlap this asset's bounded resource trim.
        previewRenderer.ReleaseCompletedPreviewResources(request);
        if (preview.completedPendingReadback)
        {
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "completed-readback-disposition=cache-write",
                &request,
                m_inFlightThumbnails.size());
        }
        const auto metadataRequest = BuildResolvedThumbnailCacheRequest(request, previewRequest);
        if (!m_generationCancelToken)
            m_generationCancelToken = std::make_shared<AssetThumbnailGenerationCancelToken>();
        m_generationCancelToken->generation = m_generationSerial;
        const auto cancelToken = m_generationCancelToken;
        auto pixels = std::make_shared<std::vector<uint8_t>>(
            std::move(preview.rgbaPixels));
        const auto width = preview.width;
        const auto height = preview.height;
        const auto performanceCaptureToken =
            NLS::Base::Profiling::PerformanceStageStatsCapture::GetActiveToken();
        const bool persistenceSlotsExhausted =
            CountActiveThumbnailPersistenceRequests() >=
                kMaxActiveThumbnailPersistenceRequests;
        bool persistenceDeferred =
            CountCurrentThumbnailPreparationRequests() >=
                kMaxCurrentThumbnailGenerationInFlightRequests ||
            persistenceSlotsExhausted ||
            (m_hasExplicitGenerationBudget &&
                m_generationBudget.cacheWriteCountBudget == 0u);
        bool persistenceDropped = false;
        try
        {
            if (!pollingPendingReadback &&
                m_hasExplicitGenerationBudget)
            {
                ConsumeThumbnailCountBudget(m_generationBudget.readbackCountBudget, true);
            }
            if (!pollingPendingReadback)
            {
                ConsumeThumbnailByteBudget(
                    m_generationBudget.gpuUploadByteBudget,
                    EstimateThumbnailGpuUploadBytes(previewRequest, width, height),
                    m_hasExplicitGenerationBudget);
            }
            if (persistenceDeferred &&
                CountActiveThumbnailPersistenceRequests() >=
                    kMaxActiveThumbnailPersistenceRequests)
            {
                auto victim = m_deferredPersistenceTickets.end();
                uint32_t victimRank = ThumbnailRequestPriorityRank(request.priority) + 1u;
                for (auto iterator = m_deferredPersistenceTickets.begin();
                     iterator != m_deferredPersistenceTickets.end();
                     ++iterator)
                {
                    const auto rank = ThumbnailRequestPriorityRank(iterator->request.priority);
                    if (victim == m_deferredPersistenceTickets.end() || rank < victimRank)
                    {
                        victim = iterator;
                        victimRank = rank;
                    }
                }
                if (victim != m_deferredPersistenceTickets.end() &&
                    victimRank <= ThumbnailRequestPriorityRank(request.priority))
                {
                    m_thumbnailStatesByCacheKey[victim->cacheKey] = ThumbnailState::Ready;
                    m_deferredPersistenceTickets.erase(victim);
                }
                else
                {
                    persistenceDropped = true;
                }
            }

            if (persistenceDeferred && !persistenceDropped)
            {
                m_deferredPersistenceTickets.push_back({
                    *cacheKey,
                    m_generationSerial,
                    cancelToken,
                    request,
                    metadataRequest,
                    evaluation,
                    pixels,
                    width,
                    height
                });
                m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Persisting;
                RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                    "cache-write-deferred",
                    &request,
                    m_deferredPersistenceTickets.size());
            }
            else if (persistenceDropped)
            {
                m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Ready;
                RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                    "cache-write-deferred-dropped",
                    &request,
                    m_deferredPersistenceTickets.size());
            }
            else
            {
                m_inFlightThumbnails.push_back({
                    *cacheKey,
                    m_generationSerial,
                    cancelToken,
                    ScheduleThumbnailJobFuture(
                        "AssetThumbnailService.GpuPreviewCacheWrite",
                        [request, metadataRequest, evaluation, pixels, width, height, cancelToken, performanceCaptureToken]() mutable
                        {
                            NLS::Base::Profiling::PerformanceStageStatsCaptureScope capture(performanceCaptureToken);
                            ScopedThumbnailGenerationStageThread backgroundStageThread(
                                PerformanceStageThread::Background);
                            return WriteRgbaThumbnailResult(
                                request,
                                evaluation,
                                pixels->data(),
                                width,
                                height,
                                "thumbnail-gpu-preview-generation-failed",
                                cancelToken,
                                &metadataRequest);
                        }),
                    request,
                    false,
                    true
                });
                m_thumbnailStatesByCacheKey[*cacheKey] = request.enableReadbackRing
                    ? ThumbnailState::Encoding
                    : ThumbnailState::Readback;
            }
            ClearGpuPreviewReadbackPending(*cacheKey);
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                persistenceDeferred
                    ? (persistenceDropped
                        ? "cache-write-not-persisted"
                        : "cache-write-deferred")
                    : "cache-write-start",
                &request,
                m_inFlightThumbnails.size());
            m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
            m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
            m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(*cacheKey);
            m_gpuPreviewEmptyFrameDeferralsByCacheKey.erase(*cacheKey);
            ClearGpuPreviewResourcePending(*cacheKey);
        }
        catch (...)
        {
            auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
            result.diagnostic = "thumbnail-generation-worker-start-failed";
            const auto metadataRequest = BuildResolvedThumbnailCacheRequest(request, previewRequest);
            WriteThumbnailMetadataForEvaluation(
                request,
                evaluation,
                AssetThumbnailCacheStatus::Failed,
                result.diagnostic,
                &metadataRequest);
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Failed;
            m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
            m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
            m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(*cacheKey);
            m_gpuPreviewEmptyFrameDeferralsByCacheKey.erase(*cacheKey);
            ClearGpuPreviewResourcePending(*cacheKey);
            return result;
        }
        AssetThumbnailServiceResult pending = BuildResultFromEvaluation(
            request,
            evaluation,
            AssetThumbnailServiceStatus::Pending);
        pending.diagnostic = "thumbnail-gpu-preview-cache-write-pending";
        if (publishedGpuTexture.IsValid())
        {
            pending.gpuTexture = publishedGpuTexture;
            pending.gpuTextureGeneration = m_generationSerial;
        }
        ApplyPublishedGpuPresentation(pending);
        return pending;
    }

    RestoreDeferredCacheKeys(deferredCacheKeys);
    if (pendingPreviewRequestResolution.has_value())
    {
        // Keep the original cache identity while the deferred artifact
        // lookup is running.  A bare Pending result cannot be associated with
        // the later cache-write completion, which leaves the UI on its
        // fallback result even though the PNG was generated successfully.
        const auto& pendingRequest = *pendingPreviewRequestResolution;
        const auto pendingEvaluation = EvaluateAssetThumbnailCache(
            pendingRequest,
            AssetThumbnailCacheIntegrityMode::Fast);
        AssetThumbnailServiceResult pending = BuildResultFromEvaluation(
            pendingRequest,
            pendingEvaluation,
            AssetThumbnailServiceStatus::Pending);
        pending.diagnostic = "thumbnail-preview-request-resolution-pending";
        return pending;
    }
    return std::nullopt;
}

void AssetThumbnailService::PumpDeferredPersistenceTickets()
{
    // Deferred persistence is deliberately independent from GPU submission, but
    // it still consumes the explicit cache-write budget. Keep the ticket queued
    // until that budget is available so a throttled frame cannot start disk I/O.
    if (m_hasExplicitGenerationBudget &&
        m_generationBudget.cacheWriteCountBudget == 0u)
    {
        return;
    }

    while (!m_deferredPersistenceTickets.empty() &&
        CountActiveThumbnailPersistenceRequests() <
            kMaxActiveThumbnailPersistenceRequests)
    {
        auto ticket = std::move(m_deferredPersistenceTickets.front());
        m_deferredPersistenceTickets.pop_front();

        if (ticket.generation != m_generationSerial ||
            !ticket.cancelToken ||
            ticket.cancelToken->cancelled.load(std::memory_order_relaxed) ||
            ticket.pixels == nullptr)
        {
            m_thumbnailStatesByCacheKey[ticket.cacheKey] = ThumbnailState::Cancelled;
            continue;
        }

        try
        {
            const auto request = ticket.request;
            const auto metadataRequest = ticket.metadataRequest;
            const auto evaluation = ticket.evaluation;
            const auto pixels = ticket.pixels;
            const auto width = ticket.width;
            const auto height = ticket.height;
            const auto cancelToken = ticket.cancelToken;
            m_inFlightThumbnails.push_back({
                ticket.cacheKey,
                ticket.generation,
                cancelToken,
                ScheduleThumbnailJobFuture(
                    "AssetThumbnailService.DeferredGpuPreviewPersistence",
                    [request, metadataRequest, evaluation, pixels, width, height, cancelToken]
                    {
                        ScopedThumbnailGenerationStageThread backgroundStageThread(
                            PerformanceStageThread::Background);
                        return WriteRgbaThumbnailResult(
                            request,
                            evaluation,
                            pixels->data(),
                            width,
                            height,
                            "thumbnail-gpu-preview-generation-failed",
                            cancelToken,
                            &metadataRequest);
                    }),
                ticket.request,
                false,
                true
            });
            m_thumbnailStatesByCacheKey[ticket.cacheKey] = ThumbnailState::Persisting;
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "deferred-persistence-start",
                &ticket.request,
                m_deferredPersistenceTickets.size());
        }
        catch (...)
        {
            m_thumbnailStatesByCacheKey[ticket.cacheKey] = ThumbnailState::Failed;
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "deferred-persistence-start-failed",
                &ticket.request,
                m_deferredPersistenceTickets.size());
        }
    }
}

bool AssetThumbnailService::StartNextThumbnailGeneration()
{
    return StartNextThumbnailGeneration(nullptr);
}

bool AssetThumbnailService::StartNextThumbnailGeneration(EditorThumbnailPreviewRenderer& previewRenderer)
{
    EditorThumbnailPreviewRendererAdapter adapter(previewRenderer);
    return StartNextThumbnailGeneration(adapter);
}

bool AssetThumbnailService::StartNextThumbnailGeneration(IEditorThumbnailPreviewRenderer& previewRenderer)
{
    return StartNextThumbnailGeneration(&previewRenderer);
}

bool AssetThumbnailService::StartNextThumbnailGeneration(IEditorThumbnailPreviewRenderer* previewRenderer)
{
    CancelExpiredOffscreenRequests();
    ExpireStalledVisibleThumbnailRequests();
    PumpDeferredPersistenceTickets();
    RestoreGpuPreviewResourceContinuationRequests();
    RepairQueuedRequestLaneMembership();
    for (auto iterator = m_inFlightThumbnails.begin(); iterator != m_inFlightThumbnails.end();)
    {
        if (!iterator->future.valid())
        {
            iterator = m_inFlightThumbnails.erase(iterator);
            continue;
        }

        if (iterator->generation != m_generationSerial &&
            iterator->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            try
            {
                (void)iterator->future.get();
            }
            catch (...)
            {
            }
            iterator = m_inFlightThumbnails.erase(iterator);
            continue;
        }

        ++iterator;
    }

    const auto currentGenerationInFlightCount = CountCurrentThumbnailPreparationRequests();
    const auto currentVisibleTextureInFlightCount =
        CountCurrentVisibleTextureThumbnailPreparationRequests();
    const auto currentNonTextureInFlightCount =
        currentGenerationInFlightCount >= currentVisibleTextureInFlightCount
            ? currentGenerationInFlightCount - currentVisibleTextureInFlightCount
            : 0u;
    const auto obsoleteGenerationInFlightCount = static_cast<size_t>(std::count_if(
        m_inFlightThumbnails.begin(),
        m_inFlightThumbnails.end(),
        [this](const InFlightThumbnailRequest& request)
        {
            return request.generation != m_generationSerial &&
                request.future.valid() &&
                !request.persistenceOnly;
        }));
    const auto nonPersistenceInFlightCount =
        currentGenerationInFlightCount + obsoleteGenerationInFlightCount;
    const auto maxCurrentGenerationInFlightCount =
        m_hasExplicitGenerationBudget && m_generationBudget.cacheWriteCountBudget != SIZE_MAX
            ? (std::min)(kMaxCurrentThumbnailGenerationInFlightRequests, m_generationBudget.cacheWriteCountBudget)
            : kMaxCurrentThumbnailGenerationInFlightRequests;
    const auto maxVisibleTextureGenerationInFlightCount =
        m_hasExplicitGenerationBudget && m_generationBudget.cacheWriteCountBudget != SIZE_MAX
            ? (std::min)(
                kMaxVisibleTextureThumbnailGenerationInFlightRequests,
                m_generationBudget.cacheWriteCountBudget)
            : kMaxVisibleTextureThumbnailGenerationInFlightRequests;
    const bool hasQueuedVisibleTextureWork = HasQueuedVisibleTextureThumbnailWork();
    const bool visibleTextureLaneHasCapacity =
        hasQueuedVisibleTextureWork &&
        currentVisibleTextureInFlightCount < maxVisibleTextureGenerationInFlightCount;
    const auto recordStartBlocked = [this, currentGenerationInFlightCount,
                                     currentVisibleTextureInFlightCount,
                                     currentNonTextureInFlightCount,
                                     obsoleteGenerationInFlightCount,
                                     nonPersistenceInFlightCount,
                                     maxCurrentGenerationInFlightCount,
                                     maxVisibleTextureGenerationInFlightCount](
                                        const std::string_view reason)
    {
        if (!NLS::Core::Assets::IsArtifactLoadTelemetryEnabled())
            return;
        std::string diagnostic = "start-next-blocked=" + std::string(reason);
        diagnostic += "|currentPrep=" + std::to_string(currentGenerationInFlightCount);
        diagnostic += "|obsoletePrep=" + std::to_string(obsoleteGenerationInFlightCount);
        diagnostic += "|nonPersistence=" + std::to_string(nonPersistenceInFlightCount);
        diagnostic += "|maxCurrent=" + std::to_string(maxCurrentGenerationInFlightCount);
        diagnostic += "|texturePrep=" + std::to_string(currentVisibleTextureInFlightCount);
        diagnostic += "|nonTexturePrep=" + std::to_string(currentNonTextureInFlightCount);
        diagnostic += "|maxTexture=" + std::to_string(maxVisibleTextureGenerationInFlightCount);
        diagnostic += "|persistence=" + std::to_string(CountActiveThumbnailPersistenceRequests());
        diagnostic += "|deferredPersistence=" +
            std::to_string(m_deferredPersistenceTickets.size());
        diagnostic += "|totalEntries=" + std::to_string(m_inFlightThumbnails.size());
        diagnostic += "|queue=" + std::to_string(m_queuedRequestsByCacheKey.size());
        RecordThumbnailGpuPreviewQueueDecisionTelemetry(
            diagnostic,
            nullptr,
            m_queuedRequestsByCacheKey.size());
    };
    // The ordinary preparation lane remains capped at two. A visible
    // texture may use the separate four-entry lane while those two slots are
    // occupied, provided the overall bounded in-flight limit is respected.
    if ((currentNonTextureInFlightCount >= maxCurrentGenerationInFlightCount &&
            !visibleTextureLaneHasCapacity) ||
        currentVisibleTextureInFlightCount >= maxVisibleTextureGenerationInFlightCount &&
            currentNonTextureInFlightCount >= maxCurrentGenerationInFlightCount ||
        obsoleteGenerationInFlightCount > kMaxObsoleteThumbnailGenerationInFlightRequests ||
        nonPersistenceInFlightCount >= kMaxThumbnailGenerationTotalInFlightSlots)
    {
        recordStartBlocked("capacity");
        return false;
    }

    if (m_generationBudget.cacheWriteCountBudget == 0u && HasQueuedCacheKeys())
    {
        recordStartBlocked("cache-write-budget");
        return false;
    }

    std::vector<std::string> deferredCacheKeys;
    size_t deferredGpuPreviewCount = 0u;

    while (HasQueuedCacheKeys())
    {
        auto cacheKey = HasQueuedNonGpuThumbnailWork()
            ? PopNextNonGpuThumbnailCacheKey()
            : PopNextQueuedCacheKey();
        // A full texture lane can leave only capped non-GPU entries in the
        // map. Fall through to the normal selector so a heavy GPU request can
        // still use an available ordinary slot.
        if (!cacheKey.has_value() && HasQueuedCacheKeys())
            cacheKey = PopNextQueuedCacheKey();
        if (!cacheKey.has_value())
            break;

        const auto requestIterator = m_queuedRequestsByCacheKey.find(*cacheKey);
        if (requestIterator == m_queuedRequestsByCacheKey.end())
            continue;

        const auto request = requestIterator->second;
        const auto preparationCount = CountCurrentThumbnailPreparationRequests();
        const auto visibleTexturePreparationCount =
            CountCurrentVisibleTextureThumbnailPreparationRequests();
        const auto nonTexturePreparationCount =
            preparationCount >= visibleTexturePreparationCount
                ? preparationCount - visibleTexturePreparationCount
                : 0u;
        const bool visibleTextureRequest = IsVisibleTextureThumbnailRequest(request);
        const bool textureLaneFull =
            visibleTextureRequest &&
            visibleTexturePreparationCount >= maxVisibleTextureGenerationInFlightCount;
        const bool ordinaryLaneFull =
            !visibleTextureRequest &&
            nonTexturePreparationCount >= maxCurrentGenerationInFlightCount;
        if (textureLaneFull || ordinaryLaneFull)
        {
            deferredCacheKeys.push_back(*cacheKey);
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                visibleTextureRequest
                    ? "texture-lane-blocked-capacity"
                    : "ordinary-lane-blocked-capacity",
                &request,
                m_queuedRequestsByCacheKey.size());
            continue;
        }
        const auto estimatedCpuPreparationBytes = EstimateThumbnailCpuPreparationBytes(request);
        const auto estimatedGpuUploadBytes = EstimateThumbnailGpuUploadBytes(request);
        if (!HasThumbnailBudget(m_generationBudget.cpuPreparationByteBudget, estimatedCpuPreparationBytes) ||
            !HasThumbnailBudget(m_generationBudget.gpuUploadByteBudget, estimatedGpuUploadBytes))
        {
            RestoreDeferredCacheKeys(deferredCacheKeys);
            m_queuedRequestsByCacheKey[*cacheKey] = request;
            EnqueueQueuedCacheKey(*cacheKey, request);
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Queued;
            return false;
        }
        if (ShouldDeferBackgroundCpuThumbnailToPreviewRenderer(request.kind) &&
            SupportsGpuThumbnailPreview(request))
        {
            deferredCacheKeys.push_back(*cacheKey);
            ++deferredGpuPreviewCount;
            if (deferredGpuPreviewCount >= kMaxDeferredHeavyGpuPreviewScanPerCall)
                break;
            continue;
        }

        if (previewRenderer != nullptr &&
            (IsHeavyGpuThumbnailPreview(request.kind) ||
                IsUnresolvedSourceModelPreviewRequest(request)) &&
            request.artifactPath.empty())
        {
            deferredCacheKeys.push_back(*cacheKey);
            ++deferredGpuPreviewCount;
            if (deferredGpuPreviewCount >= kMaxDeferredHeavyGpuPreviewScanPerCall)
                break;
            continue;
        }

        if (SupportsGpuThumbnailPreview(request) &&
            previewRenderer != nullptr &&
            (!CanGenerateThumbnail(request.kind) ||
                ShouldDeferBackgroundCpuThumbnailToPreviewRenderer(request.kind)) &&
            previewRenderer->Supports(request))
        {
            deferredCacheKeys.push_back(*cacheKey);
            ++deferredGpuPreviewCount;
            if (deferredGpuPreviewCount >= kMaxDeferredHeavyGpuPreviewScanPerCall)
                break;
            continue;
        }
        m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Preparing;
        MarkVisibleThumbnailRequestWorkStarted(request);
        m_queuedThumbnailLaneByCacheKey.erase(requestIterator->first);
        m_queuedRequestsByCacheKey.erase(requestIterator);
        if (!m_generationCancelToken)
            m_generationCancelToken = std::make_shared<AssetThumbnailGenerationCancelToken>();
        m_generationCancelToken->generation = m_generationSerial;
        const auto cancelToken = m_generationCancelToken;
        const auto performanceCaptureToken =
            NLS::Base::Profiling::PerformanceStageStatsCapture::GetActiveToken();

        try
        {
            ConsumeThumbnailByteBudget(
                m_generationBudget.cpuPreparationByteBudget,
                estimatedCpuPreparationBytes,
                m_hasExplicitGenerationBudget);
            ConsumeThumbnailByteBudget(
                m_generationBudget.gpuUploadByteBudget,
                estimatedGpuUploadBytes,
                m_hasExplicitGenerationBudget);
            const bool directSourceTextureRequest =
                IsDirectSourceTextureThumbnailRequest(request);
            const auto thumbnailJobQueue = directSourceTextureRequest
                ? ThumbnailJobQueue::Foreground
                : ThumbnailJobQueue::Background;
            const auto scheduledAt = std::chrono::steady_clock::now();
            RecordThumbnailJobQueueTelemetry(
                "scheduled",
                request,
                directSourceTextureRequest ? "foreground" : "background",
                std::chrono::microseconds(0));
            m_inFlightThumbnails.push_back({
                *cacheKey,
                m_generationSerial,
                cancelToken,
                ScheduleThumbnailJobFuture(
                    "AssetThumbnailService.GenerateThumbnail",
                    [request, cancelToken, performanceCaptureToken, scheduledAt, thumbnailJobQueue]
                    {
                        NLS::Base::Profiling::PerformanceStageStatsCaptureScope capture(performanceCaptureToken);
                        ScopedThumbnailGenerationStageThread backgroundStageThread(
                            PerformanceStageThread::Background);
                        const auto startedAt = std::chrono::steady_clock::now();
                        const auto queueName = thumbnailJobQueue == ThumbnailJobQueue::Foreground
                            ? "foreground"
                            : "background";
                        RecordThumbnailJobQueueTelemetry(
                            "started",
                            request,
                            queueName,
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                startedAt - scheduledAt));
                        auto result = TryGenerateThumbnailForRequest(request, cancelToken);
                        RecordThumbnailJobResultTelemetry(
                            request,
                            queueName,
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - startedAt),
                            result);
                        return result;
                },
                    IsVisibleTextureThumbnailRequest(request)
                        ? NLS::Base::Jobs::JobPriority::High
                        : NLS::Base::Jobs::JobPriority::Normal,
                    thumbnailJobQueue),
	                request,
	                directSourceTextureRequest
            });
        }
        catch (...)
        {
            (void)BuildExceptionThumbnailResult(request, "thumbnail-generation-worker-start-failed");
            m_queuedRequestsByCacheKey[*cacheKey] = request;
            EnqueueQueuedCacheKey(*cacheKey, request);
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Queued;
            RestoreDeferredCacheKeys(deferredCacheKeys);
            return false;
        }
        RestoreDeferredCacheKeys(deferredCacheKeys);
        return true;
    }

    RestoreDeferredCacheKeys(deferredCacheKeys);
    recordStartBlocked("no-eligible-request");
    return false;
}

std::optional<AssetThumbnailServiceResult> AssetThumbnailService::ConsumeCompletedThumbnail(
    const bool maintainPendingRequests)
{
    if (maintainPendingRequests)
        MaintainPendingThumbnailRequests();
    PumpDeferredPersistenceTickets();

    if (!m_terminalThumbnailResultsByCacheKey.empty())
    {
        auto terminal = m_terminalThumbnailResultsByCacheKey.begin();
        auto result = std::move(terminal->second);
        m_terminalThumbnailResultsByCacheKey.erase(terminal);
        return result;
    }

    for (auto iterator = m_inFlightThumbnails.begin(); iterator != m_inFlightThumbnails.end();)
    {
        if (!iterator->future.valid())
        {
            iterator = m_inFlightThumbnails.erase(iterator);
            continue;
        }

        if (iterator->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            ++iterator;
            continue;
        }

        AssetThumbnailServiceResult result;
        try
        {
            result = iterator->future.get();
        }
        catch (const std::bad_alloc&)
        {
            result.status = AssetThumbnailServiceStatus::Failed;
            result.fallbackIcon = "editor.icon.asset.default";
            result.diagnostic = "thumbnail-generation-out-of-memory";
        }
        catch (...)
        {
            result.status = AssetThumbnailServiceStatus::Failed;
            result.fallbackIcon = "editor.icon.asset.default";
            result.diagnostic = "thumbnail-generation-exception";
        }
        if (iterator->cancelToken &&
            iterator->cancelToken->cancelled.load(std::memory_order_relaxed))
        {
            // Complete-resident takeover may cancel a provisional worker while
            // it is already finishing. Its result must not overwrite the new
            // canonical queue state or reintroduce the partial presentation.
            m_completedGpuPreviewResultsByCacheKey.erase(iterator->cacheKey);
            if (m_gpuPreviewReadbackPendingRequestsByCacheKey.find(iterator->cacheKey) ==
                m_gpuPreviewReadbackPendingRequestsByCacheKey.end())
            {
                ClearGpuPreviewReadbackPending(iterator->cacheKey);
            }
            iterator = m_inFlightThumbnails.erase(iterator);
            continue;
        }
        const auto presentationKey = BuildAssetThumbnailPresentationKey(iterator->request);
        const auto invalidation = m_invalidatedPresentationRevisions.find(presentationKey);
        if (invalidation != m_invalidatedPresentationRevisions.end() &&
            iterator->request.requestRevision <= invalidation->second)
        {
            // A worker may have passed its cancellation check just before the
            // delete operation. Remove any late disk result before allowing it
            // to reach the UI or the stable-result table.
            (void)RemoveAssetThumbnailPresentation(iterator->request);
            const bool hasPendingReadback =
                m_gpuPreviewReadbackPendingRequestsByCacheKey.find(iterator->cacheKey) !=
                    m_gpuPreviewReadbackPendingRequestsByCacheKey.end();
            m_thumbnailStatesByCacheKey[iterator->cacheKey] = hasPendingReadback
                ? ThumbnailState::WaitingForGpu
                : ThumbnailState::Cancelled;
            if (!hasPendingReadback)
            {
                ClearGpuPreviewReadbackPending(iterator->cacheKey);
            }
            iterator = m_inFlightThumbnails.erase(iterator);
            PruneInvalidatedPresentationBarrier(presentationKey);
            continue;
        }
        if (IsPresentationRevisionSuperseded(iterator->request))
        {
            // Cancellation is cooperative, so a worker may still complete
            // after a newer revision has replaced it. Do not surface that
            // result or requeue it; the canonical disk commit is protected by
            // its revision CAS and the newer request remains authoritative.
            m_completedGpuPreviewResultsByCacheKey.erase(iterator->cacheKey);
            m_thumbnailStatesByCacheKey[iterator->cacheKey] = ThumbnailState::Cancelled;
            ClearGpuPreviewReadbackPending(iterator->cacheKey);
            iterator = m_inFlightThumbnails.erase(iterator);
            continue;
        }
        const auto terminalBarrier = m_terminalPresentationRevisions.find(presentationKey);
        bool recoveredAfterTerminalTimeout = false;
        if (terminalBarrier != m_terminalPresentationRevisions.end() &&
            iterator->request.requestRevision != 0u &&
            iterator->request.requestRevision <= terminalBarrier->second)
        {
            // A timeout is a UI safety net, not a permanent write barrier. A
            // cooperative worker may have passed its cancellation check and
            // committed a fully canonical result just after the timeout. Do a
            // full disk/index validation before allowing that same revision to
            // recover; a newer revision remains authoritative below.
            if (result.status == AssetThumbnailServiceStatus::Fresh &&
                iterator->request.requestRevision == terminalBarrier->second)
            {
                auto canonicalEvaluation = EvaluateAssetThumbnailCache(
                    iterator->request,
                    AssetThumbnailCacheIntegrityMode::Full);
                recoveredAfterTerminalTimeout =
                    PromoteFreshCanonicalThumbnailIfAvailable(
                        iterator->request,
                        canonicalEvaluation);
                if (recoveredAfterTerminalTimeout)
                {
                    result.cacheEntry = canonicalEvaluation.entry;
                    result.imagePath = canonicalEvaluation.entry->imagePath;
                    result.diagnostic.clear();
                    result.refreshPending = false;
                    result.failureRetained = false;
                    SynchronizeThumbnailResultPresentationState(result);
                    m_terminalPresentationRevisions.erase(presentationKey);
                    for (auto terminalIterator = m_terminalThumbnailResultsByCacheKey.begin();
                         terminalIterator != m_terminalThumbnailResultsByCacheKey.end();)
                    {
                        if (terminalIterator->second.presentationKey == presentationKey &&
                            terminalIterator->second.requestRevision <=
                                iterator->request.requestRevision)
                        {
                            terminalIterator = m_terminalThumbnailResultsByCacheKey.erase(
                                terminalIterator);
                        }
                        else
                        {
                            ++terminalIterator;
                        }
                    }
                    m_stableThumbnailResultsByCacheKey[canonicalEvaluation.entry->cacheKey] = result;
                }
            }

            if (!recoveredAfterTerminalTimeout)
            {
                // The maintenance path may have already published a terminal
                // timeout while this cooperative worker was still running. Its
                // result remains fenced unless the exact revision now has a
                // validated canonical presentation.
                m_completedGpuPreviewResultsByCacheKey.erase(iterator->cacheKey);
                m_thumbnailStatesByCacheKey[iterator->cacheKey] = ThumbnailState::Failed;
                ClearGpuPreviewReadbackPending(iterator->cacheKey);
                ClearVisibleThumbnailRequestStart(iterator->request);
                iterator = m_inFlightThumbnails.erase(iterator);
                continue;
            }
        }
        const bool currentGeneration = iterator->generation == m_generationSerial;
        if (result.status == AssetThumbnailServiceStatus::Fresh)
        {
            ReleaseImportedPrefabThumbnailContinuationOwner(iterator->request);
            m_completedGpuPreviewResultsByCacheKey.erase(iterator->cacheKey);
            m_offscreenSinceByCacheKey.erase(iterator->cacheKey);
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "cache-write-complete=fresh",
                &iterator->request,
                m_inFlightThumbnails.size());
            if (currentGeneration)
            {
                ConsumeThumbnailCacheWriteBudgetForFreshResult(
                    m_generationBudget,
                    m_hasExplicitGenerationBudget);
            }
            m_thumbnailStatesByCacheKey[iterator->cacheKey] = ThumbnailState::Ready;
            m_gpuPreviewReadyResidentCacheKeys.erase(iterator->cacheKey);
            ClearGpuPreviewResourceRequestStart(iterator->cacheKey);
            ClearGpuPreviewReadbackPending(iterator->cacheKey);
            ClearVisibleThumbnailRequestStart(iterator->request);
        }
        else if (result.status == AssetThumbnailServiceStatus::Pending)
        {
            m_completedGpuPreviewResultsByCacheKey.erase(iterator->cacheKey);
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                std::string("cache-write-complete=pending|diag=") + result.diagnostic,
                &iterator->request,
                m_inFlightThumbnails.size());
            m_thumbnailStatesByCacheKey[iterator->cacheKey] = ThumbnailState::Queued;
            // A provisional worker can finish after the registry has already
            // published the complete package. Keep the ready marker in that
            // case so the scheduler immediately admits the canonical retry;
            // clearing it here would put the request behind the scene-load
            // cooldown again and leave the partial presentation indefinitely.
            const bool keepReadyResidentMarker =
                result.diagnostic == "thumbnail-gpu-preview-resident-partial" &&
                (IsCompleteResidentThumbnailRequest(iterator->request) ||
                    m_gpuPreviewReadyResidentCacheKeys.find(iterator->cacheKey) !=
                        m_gpuPreviewReadyResidentCacheKeys.end());
            if (keepReadyResidentMarker)
                m_gpuPreviewReadyResidentCacheKeys.insert(iterator->cacheKey);
            else
            {
                if (m_gpuPreviewReadyResidentCacheKeys.find(iterator->cacheKey) !=
                    m_gpuPreviewReadyResidentCacheKeys.end())
                {
                    RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                        "ready-resident-marker-cleared=pending-result",
                        &iterator->request,
                        m_queuedRequestsByCacheKey.size());
                }
                m_gpuPreviewReadyResidentCacheKeys.erase(iterator->cacheKey);
            }
            if (iterator->requeueOnPending && currentGeneration)
            {
                m_queuedRequestsByCacheKey[iterator->cacheKey] = iterator->request;
                EnqueueQueuedCacheKey(iterator->cacheKey, iterator->request);
                if (IsPendingThumbnailPreviewReadbackDiagnostic(result.diagnostic))
                {
                    m_thumbnailStatesByCacheKey[iterator->cacheKey] = ThumbnailState::WaitingForGpu;
                    auto readbackRequest = iterator->request;
                    if (const auto resolved = m_resolvedPreviewRequestsByCacheKey.find(iterator->cacheKey);
                        resolved != m_resolvedPreviewRequestsByCacheKey.end())
                    {
                        readbackRequest = resolved->second;
                    }
                    TrackGpuPreviewReadbackPending(iterator->cacheKey, readbackRequest);
                }
            }
        }
        else if (currentGeneration)
        {
            m_completedGpuPreviewResultsByCacheKey.erase(iterator->cacheKey);
            m_offscreenSinceByCacheKey.erase(iterator->cacheKey);
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                std::string("cache-write-complete=failed|diag=") + result.diagnostic,
                &iterator->request,
                m_inFlightThumbnails.size());
            m_thumbnailStatesByCacheKey[iterator->cacheKey] = ThumbnailState::Failed;
            m_gpuPreviewReadyResidentCacheKeys.erase(iterator->cacheKey);
            ClearGpuPreviewResourceRequestStart(iterator->cacheKey);
            ClearGpuPreviewReadbackPending(iterator->cacheKey);
            ClearVisibleThumbnailRequestStart(iterator->request);
            if (iterator->requeueOnPending)
            {
                m_resolvedPreviewRequestsByCacheKey.erase(iterator->cacheKey);
                m_gpuDeferredHeavyPreviewCacheKeys.erase(iterator->cacheKey);
            }
        }
        else
        {
            m_completedGpuPreviewResultsByCacheKey.erase(iterator->cacheKey);
            m_thumbnailStatesByCacheKey[iterator->cacheKey] = ThumbnailState::Cancelled;
            m_gpuPreviewReadyResidentCacheKeys.erase(iterator->cacheKey);
            ClearGpuPreviewReadbackPending(iterator->cacheKey);
        }
        iterator = m_inFlightThumbnails.erase(iterator);
        // Generation scopes describe the Asset Browser view, not the durable
        // presentation revision. A worker can finish after an async folder
        // rebuild has advanced the scope even though its Fresh result is still
        // the newest canonical image for that presentation. Surface that result
        // so the browser can bind it by presentationKey; the revision checks
        // above reject stale freshness, and the browser's generation-scoped
        // presentation map ignores results for tiles that are no longer shown.
        if (currentGeneration || result.status == AssetThumbnailServiceStatus::Fresh)
            return result;
    }
    return std::nullopt;
}

void AssetThumbnailService::MaintainPendingThumbnailRequests()
{
    // This path is intentionally independent of the render scheduler. A
    // previous-frame budget rejection must not leave a visible request in
    // WaitingForResources/Queued/Preparing forever.
    CancelExpiredOffscreenRequests();
    ExpireStalledVisibleThumbnailRequests();
    // Scene resource resolution can finish between render-budget turns. The
    // partial resident frame owns a parked continuation, so restore it here as
    // well as from GenerateNextThumbnail; otherwise a completed scene package
    // can remain WaitingForResources while the UI keeps returning the partial
    // presentation.
    RestoreGpuPreviewResourceContinuationRequests();
    ExpireStalledGpuPreviewResourceContinuations();
}

void AssetThumbnailService::InvalidateThumbnail(const AssetThumbnailRequest& inputRequest)
{
    AssetThumbnailRequest request = inputRequest;
    if (request.presentationKey.empty())
        request.presentationKey = BuildAssetThumbnailPresentationKey(request);
    if (request.requestRevision == 0u)
    {
        request.requestRevision = m_nextRequestRevision++;
        if (m_nextRequestRevision == 0u)
            m_nextRequestRevision = 1u;
    }

    const auto presentationKey = BuildAssetThumbnailPresentationKey(request);
    m_residentPreviewRequestsByPresentationKey.erase(presentationKey);
    m_terminalPresentationRevisions.erase(presentationKey);
    m_visibleThumbnailRequestDeadlinesByPresentationKey.erase(presentationKey);
    auto barrier = m_invalidatedPresentationRevisions.find(presentationKey);
    if (barrier == m_invalidatedPresentationRevisions.end())
        m_invalidatedPresentationRevisions.emplace(presentationKey, request.requestRevision);
    else
        barrier->second = (std::max)(barrier->second, request.requestRevision);

    std::vector<std::string> matchingCacheKeys;
    for (const auto& [cacheKey, queuedRequest] : m_queuedRequestsByCacheKey)
    {
        if (BuildAssetThumbnailPresentationKey(queuedRequest) == presentationKey)
            matchingCacheKeys.push_back(cacheKey);
    }
    for (const auto& [cacheKey, pendingRequest] : m_gpuPreviewResourcePendingRequestsByCacheKey)
    {
        if (BuildAssetThumbnailPresentationKey(pendingRequest) == presentationKey &&
            std::find(matchingCacheKeys.begin(), matchingCacheKeys.end(), cacheKey) == matchingCacheKeys.end())
        {
            matchingCacheKeys.push_back(cacheKey);
        }
    }
    for (const auto& cacheKey : matchingCacheKeys)
    {
        m_completedGpuPreviewResultsByCacheKey.erase(cacheKey);
        const bool hasPendingReadback =
            m_gpuPreviewReadbackPendingCacheKeys.find(cacheKey) !=
                m_gpuPreviewReadbackPendingCacheKeys.end() &&
            m_gpuPreviewReadbackPendingRequestsByCacheKey.find(cacheKey) !=
                m_gpuPreviewReadbackPendingRequestsByCacheKey.end();
        RemoveQueuedCacheKeyOccurrences(cacheKey);
        m_queuedRequestsByCacheKey.erase(cacheKey);
        m_queuedThumbnailLaneByCacheKey.erase(cacheKey);
        m_resolvedPreviewRequestsByCacheKey.erase(cacheKey);
        m_gpuDeferredHeavyPreviewCacheKeys.erase(cacheKey);
        m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(cacheKey);
        m_gpuPreviewEmptyFrameDeferralsByCacheKey.erase(cacheKey);
        ClearGpuPreviewResourcePending(cacheKey);
        if (!hasPendingReadback)
        {
            ClearGpuPreviewReadbackPending(cacheKey);
        }
        m_offscreenSinceByCacheKey.erase(cacheKey);
        m_thumbnailStatesByCacheKey[cacheKey] = hasPendingReadback
            ? ThumbnailState::WaitingForGpu
            : ThumbnailState::Cancelled;
        // A deferred manifest lookup cannot publish anything after deletion.
        // Drop its handle from the selector; the background task itself is
        // independent and its result is intentionally ignored.
        m_previewRequestResolutionFuturesByCacheKey.erase(cacheKey);
    }

    for (auto& inFlight : m_inFlightThumbnails)
    {
        if (BuildAssetThumbnailPresentationKey(inFlight.request) != presentationKey)
            continue;
        if (inFlight.cancelToken)
            inFlight.cancelToken->cancelled.store(true, std::memory_order_relaxed);
        m_completedGpuPreviewResultsByCacheKey.erase(inFlight.cacheKey);
        m_thumbnailStatesByCacheKey[inFlight.cacheKey] = ThumbnailState::Cancelled;
    }
    for (auto& ticket : m_deferredPersistenceTickets)
    {
        if (BuildAssetThumbnailPresentationKey(ticket.request) != presentationKey)
            continue;
        if (ticket.cancelToken)
            ticket.cancelToken->cancelled.store(true, std::memory_order_relaxed);
        m_completedGpuPreviewResultsByCacheKey.erase(ticket.cacheKey);
        m_thumbnailStatesByCacheKey[ticket.cacheKey] = ThumbnailState::Cancelled;
    }

    for (auto iterator = m_stableThumbnailResultsByCacheKey.begin();
         iterator != m_stableThumbnailResultsByCacheKey.end();)
    {
        if (iterator->second.presentationKey == presentationKey)
            iterator = m_stableThumbnailResultsByCacheKey.erase(iterator);
        else
            ++iterator;
    }

    for (auto iterator = m_terminalThumbnailResultsByCacheKey.begin();
         iterator != m_terminalThumbnailResultsByCacheKey.end();)
    {
        if (iterator->second.presentationKey == presentationKey &&
            iterator->second.requestRevision <= request.requestRevision)
        {
            iterator = m_terminalThumbnailResultsByCacheKey.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }

    (void)RemoveAssetThumbnailPresentation(request);
    PruneInvalidatedPresentationBarrier(presentationKey);
}

bool AssetThumbnailService::IsPresentationInvalidated(
    const AssetThumbnailRequest& request) const
{
    const auto presentationKey = BuildAssetThumbnailPresentationKey(request);
    const auto invalidation = m_invalidatedPresentationRevisions.find(presentationKey);
    return invalidation != m_invalidatedPresentationRevisions.end() &&
        request.requestRevision <= invalidation->second;
}

void AssetThumbnailService::PruneInvalidatedPresentationBarrier(
    const std::string& presentationKey)
{
    if (presentationKey.empty())
        return;

    const auto hasMatchingRequest = [&presentationKey](const AssetThumbnailRequest& request)
    {
        return BuildAssetThumbnailPresentationKey(request) == presentationKey;
    };
    for (const auto& [_, request] : m_queuedRequestsByCacheKey)
    {
        if (hasMatchingRequest(request))
            return;
    }
    for (const auto& request : m_inFlightThumbnails)
    {
        if (hasMatchingRequest(request.request))
            return;
    }
    for (const auto& ticket : m_deferredPersistenceTickets)
    {
        if (hasMatchingRequest(ticket.request))
            return;
    }
    for (const auto& [_, request] : m_gpuPreviewReadbackPendingRequestsByCacheKey)
    {
        if (hasMatchingRequest(request))
            return;
    }

    m_invalidatedPresentationRevisions.erase(presentationKey);
}

bool AssetThumbnailService::HasInFlightRequest() const
{
    return !m_inFlightThumbnails.empty() || !m_deferredPersistenceTickets.empty();
}

void AssetThumbnailService::SetFeatureConfig(AssetThumbnailFeatureConfig featureConfig)
{
    const bool lanesChanged = m_featureConfig.explicitLanes != featureConfig.explicitLanes;
    m_featureConfig = std::move(featureConfig);
    if (!lanesChanged || m_queuedRequestsByCacheKey.empty())
        return;

    // Rebuild the selector queues when a diagnostic flag is changed at runtime.
    // The request map remains the source of truth, so no request is lost.
    for (const auto& [cacheKey, request] : m_queuedRequestsByCacheKey)
        EnqueueQueuedCacheKey(cacheKey, request);
}

const AssetThumbnailFeatureConfig& AssetThumbnailService::GetFeatureConfig() const
{
    return m_featureConfig;
}

size_t AssetThumbnailService::CountCurrentGenerationInFlightRequests() const
{
    return static_cast<size_t>(std::count_if(
        m_inFlightThumbnails.begin(),
        m_inFlightThumbnails.end(),
        [this](const InFlightThumbnailRequest& request)
        {
            return request.generation == m_generationSerial &&
                request.future.valid();
        }));
}

size_t AssetThumbnailService::CountCurrentThumbnailPreparationRequests() const
{
    return static_cast<size_t>(std::count_if(
        m_inFlightThumbnails.begin(),
        m_inFlightThumbnails.end(),
        [this](const InFlightThumbnailRequest& request)
        {
            return request.generation == m_generationSerial &&
                request.future.valid() &&
                (!request.cancelToken ||
                    !request.cancelToken->cancelled.load(std::memory_order_relaxed)) &&
                !request.persistenceOnly;
        }));
}

size_t AssetThumbnailService::CountCurrentVisibleTextureThumbnailPreparationRequests() const
{
    return static_cast<size_t>(std::count_if(
        m_inFlightThumbnails.begin(),
        m_inFlightThumbnails.end(),
        [this](const InFlightThumbnailRequest& request)
        {
            return request.generation == m_generationSerial &&
                request.future.valid() &&
                (!request.cancelToken ||
                    !request.cancelToken->cancelled.load(std::memory_order_relaxed)) &&
                !request.persistenceOnly &&
                IsVisibleTextureThumbnailRequest(request.request);
        }));
}

size_t AssetThumbnailService::CountActiveThumbnailPersistenceRequests() const
{
    return static_cast<size_t>(std::count_if(
        m_inFlightThumbnails.begin(),
        m_inFlightThumbnails.end(),
        [this](const InFlightThumbnailRequest& request)
        {
            return request.future.valid() && request.persistenceOnly;
        })) + m_deferredPersistenceTickets.size();
}

size_t AssetThumbnailService::GetQueuedRequestCount() const
{
    size_t queuedRequestCount =
        m_queuedRequestsByCacheKey.size() +
        m_deferredPersistenceTickets.size() +
        m_terminalThumbnailResultsByCacheKey.size();
    for (const auto& cacheKey : m_gpuPreviewReadbackPendingCacheKeys)
    {
        if (m_queuedRequestsByCacheKey.find(cacheKey) != m_queuedRequestsByCacheKey.end())
            continue;
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(cacheKey);
        if (stateIterator != m_thumbnailStatesByCacheKey.end() &&
            stateIterator->second == ThumbnailState::WaitingForGpu)
        {
            ++queuedRequestCount;
        }
    }
    for (const auto& [cacheKey, deferral] : m_gpuPreviewEmptyFrameDeferralsByCacheKey)
    {
        if (m_queuedRequestsByCacheKey.find(cacheKey) == m_queuedRequestsByCacheKey.end() &&
            IsGpuPreviewEmptyFrameRetryReady(deferral))
        {
            ++queuedRequestCount;
        }
    }
    for (const auto& [cacheKey, request] : m_gpuPreviewResourcePendingRequestsByCacheKey)
    {
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(cacheKey);
        if (m_queuedRequestsByCacheKey.find(cacheKey) == m_queuedRequestsByCacheKey.end() &&
            stateIterator != m_thumbnailStatesByCacheKey.end() &&
            stateIterator->second == ThumbnailState::WaitingForResources)
        {
            // A partial resident frame deliberately parks its continuation
            // until the registry publishes a newer revision. It is retained
            // for lifetime/dependency tracking, but is not queueable work.
            if (m_gpuPreviewResidentPartialRevisionByCacheKey.find(cacheKey) !=
                m_gpuPreviewResidentPartialRevisionByCacheKey.end())
            {
                continue;
            }
            ++queuedRequestCount;
        }
    }
    return queuedRequestCount;
}

ThumbnailState AssetThumbnailService::GetThumbnailState(const AssetThumbnailRequest& request) const
{
    const auto cacheKey = BuildAssetThumbnailCacheKey(request);
    const auto found = m_thumbnailStatesByCacheKey.find(cacheKey);
    if (found != m_thumbnailStatesByCacheKey.end())
        return found->second;

    const auto presentationKey = BuildAssetThumbnailPresentationKey(request);
    for (const auto& [_, terminal] : m_terminalThumbnailResultsByCacheKey)
    {
        if (terminal.presentationKey == presentationKey &&
            (terminal.requestRevision == 0u ||
                request.requestRevision == 0u ||
                terminal.requestRevision == request.requestRevision))
        {
            return terminal.status == AssetThumbnailServiceStatus::Failed
                ? ThumbnailState::Failed
                : ThumbnailState::Ready;
        }
    }

    const auto terminalBarrier = m_terminalPresentationRevisions.find(presentationKey);
    if (terminalBarrier != m_terminalPresentationRevisions.end() &&
        request.requestRevision != 0u &&
        request.requestRevision <= terminalBarrier->second)
    {
        return ThumbnailState::Failed;
    }

    const auto evaluation = EvaluateAssetThumbnailCache(request, AssetThumbnailCacheIntegrityMode::Fast);
    if (evaluation.entry.has_value())
    {
        if (evaluation.status == AssetThumbnailCacheStatus::Fresh)
            return ThumbnailState::Ready;
        if (evaluation.status == AssetThumbnailCacheStatus::Failed)
            return ThumbnailState::Failed;
    }

    return ThumbnailState::Missing;
}

void AssetThumbnailService::SetThumbnailGenerationBudget(ThumbnailGenerationBudget budget)
{
    m_generationBudget = budget;
    m_hasExplicitGenerationBudget = true;
}

ThumbnailGenerationBudget AssetThumbnailService::GetThumbnailGenerationBudget() const
{
    return m_generationBudget;
}

void AssetThumbnailService::SetThumbnailPreviewResourcePumpBudgetMicroseconds(
    const uint32_t budgetMicroseconds)
{
    m_thumbnailPreviewResourcePumpBudgetMicroseconds = (std::clamp)(
        budgetMicroseconds,
        1000u,
        4000u);
}

bool AssetThumbnailService::HasQueuedCacheKeys() const
{
    return !m_queuedVisibleResidentCacheKeys.empty() ||
        !m_queuedVisibleCacheKeys.empty() ||
        !m_queuedInspectorCacheKeys.empty() ||
        !m_queuedPrefetchCacheKeys.empty() ||
        !m_queuedPriorityCacheKeys.empty() ||
        !m_queuedCacheKeys.empty();
}

void AssetThumbnailService::ExpireStalledGpuPreviewResourceContinuations()
{
    const auto now = std::chrono::steady_clock::now();
    std::unordered_set<std::string> trackedCacheKeys;
    trackedCacheKeys.reserve(
        m_gpuPreviewResourcePendingRequestsByCacheKey.size() +
        m_gpuPreviewResourceRequestStartedAtByCacheKey.size() +
        m_gpuPreviewResourceRequestStartedAtByPresentationKey.size());
    const auto synchronizeStableResourceDeadline =
        [this, &trackedCacheKeys](const std::string& cacheKey, const AssetThumbnailRequest& request)
    {
        if (!SupportsGpuThumbnailPreview(request))
            return;
        const auto presentationKey = BuildAssetThumbnailPresentationKey(request);
        const bool cacheTracked =
            m_gpuPreviewResourceRequestStartedAtByCacheKey.find(cacheKey) !=
                m_gpuPreviewResourceRequestStartedAtByCacheKey.end();
        const bool presentationTracked =
            !presentationKey.empty() &&
            m_gpuPreviewResourceRequestStartedAtByPresentationKey.find(presentationKey) !=
                m_gpuPreviewResourceRequestStartedAtByPresentationKey.end();
        if (!cacheTracked && !presentationTracked)
            return;

        TrackGpuPreviewResourceRequestStart(cacheKey, request);
        m_gpuPreviewResourcePresentationKeyByCacheKey[cacheKey] = presentationKey;
        if (!presentationKey.empty())
        {
            const auto stable = m_gpuPreviewResourceRequestStartedAtByPresentationKey.find(
                presentationKey);
            const auto cache = m_gpuPreviewResourceRequestStartedAtByCacheKey.find(cacheKey);
            if (stable != m_gpuPreviewResourceRequestStartedAtByPresentationKey.end() &&
                (cache == m_gpuPreviewResourceRequestStartedAtByCacheKey.end() ||
                    stable->second < cache->second))
            {
                m_gpuPreviewResourceRequestStartedAtByCacheKey[cacheKey] = stable->second;
            }
        }
        trackedCacheKeys.insert(cacheKey);
    };
    for (const auto& [cacheKey, request] : m_gpuPreviewResourcePendingRequestsByCacheKey)
        synchronizeStableResourceDeadline(cacheKey, request);
    for (const auto& [cacheKey, request] : m_queuedRequestsByCacheKey)
        synchronizeStableResourceDeadline(cacheKey, request);
    for (const auto& [cacheKey, request] : m_resolvedPreviewRequestsByCacheKey)
        synchronizeStableResourceDeadline(cacheKey, request);
    for (const auto& [cacheKey, request] : m_gpuPreviewResourcePendingRequestsByCacheKey)
    {
        (void)request;
        trackedCacheKeys.insert(cacheKey);
    }
    for (const auto& [cacheKey, deferral] : m_gpuPreviewResourcePendingDeferralsByCacheKey)
    {
        (void)deferral;
        trackedCacheKeys.insert(cacheKey);
    }
    for (const auto& [cacheKey, requestStart] : m_gpuPreviewResourceRequestStartedAtByCacheKey)
    {
        (void)requestStart;
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(cacheKey);
        const auto queuedRequestIterator = m_queuedRequestsByCacheKey.find(cacheKey);
        const bool hasVisibleQueuedGpuPreview =
            queuedRequestIterator != m_queuedRequestsByCacheKey.end() &&
            queuedRequestIterator->second.priority == ThumbnailRequestPriority::Visible &&
            SupportsGpuThumbnailPreview(queuedRequestIterator->second);
        const bool hasResourceContinuation =
            m_gpuPreviewResourcePendingRequestsByCacheKey.find(cacheKey) !=
                m_gpuPreviewResourcePendingRequestsByCacheKey.end() ||
            m_resolvedPreviewRequestsByCacheKey.find(cacheKey) !=
                m_resolvedPreviewRequestsByCacheKey.end() ||
            m_gpuDeferredHeavyPreviewCacheKeys.find(cacheKey) !=
                m_gpuDeferredHeavyPreviewCacheKeys.end() ||
            hasVisibleQueuedGpuPreview ||
            (stateIterator != m_thumbnailStatesByCacheKey.end() &&
                stateIterator->second == ThumbnailState::WaitingForResources);
        if (hasResourceContinuation)
            trackedCacheKeys.insert(cacheKey);
    }

    // A generation-scope change can remove the queued request after the UI
    // lookup has registered its resource deadline. Do not retain a timer for a
    // cache key that no longer has any owner capable of making progress.
    for (auto iterator = m_gpuPreviewResourceRequestStartedAtByCacheKey.begin();
         iterator != m_gpuPreviewResourceRequestStartedAtByCacheKey.end();)
    {
        const auto& cacheKey = iterator->first;
        if (trackedCacheKeys.find(cacheKey) != trackedCacheKeys.end())
        {
            ++iterator;
            continue;
        }

        const bool hasInFlightOwner = std::any_of(
            m_inFlightThumbnails.begin(),
            m_inFlightThumbnails.end(),
            [&cacheKey](const InFlightThumbnailRequest& request)
            {
                return request.cacheKey == cacheKey && request.future.valid();
            });
        const bool hasPersistenceOwner = std::any_of(
            m_deferredPersistenceTickets.begin(),
            m_deferredPersistenceTickets.end(),
            [&cacheKey](const DeferredPersistenceTicket& ticket)
            {
                return ticket.cacheKey == cacheKey;
            });
        if (!hasInFlightOwner && !hasPersistenceOwner)
        {
            iterator = m_gpuPreviewResourceRequestStartedAtByCacheKey.erase(iterator);
            const auto suspended = std::find(
                m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.begin(),
                m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.end(),
                cacheKey);
            if (suspended != m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.end())
                m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.erase(suspended);
        }
        else
            ++iterator;
    }

    if (NLS::Core::Assets::IsArtifactLoadTelemetryEnabled() &&
        !m_gpuPreviewResourceRequestStartedAtByCacheKey.empty())
    {
        size_t visibleQueuedGpuPreviewCount = 0u;
        size_t waitingForResourceCount = 0u;
        for (const auto& [cacheKey, request] : m_queuedRequestsByCacheKey)
        {
            if (request.priority == ThumbnailRequestPriority::Visible &&
                SupportsGpuThumbnailPreview(request))
            {
                ++visibleQueuedGpuPreviewCount;
            }
            const auto state = m_thumbnailStatesByCacheKey.find(cacheKey);
            if (state != m_thumbnailStatesByCacheKey.end() &&
                state->second == ThumbnailState::WaitingForResources)
            {
                ++waitingForResourceCount;
            }
        }
        std::array<size_t, static_cast<size_t>(ThumbnailState::Cancelled) + 1u> stateCounts {};
        for (const auto& [_, state] : m_thumbnailStatesByCacheKey)
        {
            const auto index = static_cast<size_t>(state);
            if (index < stateCounts.size())
                ++stateCounts[index];
        }
        std::string maintenanceSignature =
            "started=" + std::to_string(m_gpuPreviewResourceRequestStartedAtByCacheKey.size()) +
            "|tracked=" + std::to_string(trackedCacheKeys.size()) +
            "|queued=" + std::to_string(m_queuedRequestsByCacheKey.size()) +
            "|pending=" + std::to_string(m_gpuPreviewResourcePendingRequestsByCacheKey.size()) +
            "|resolved=" + std::to_string(m_resolvedPreviewRequestsByCacheKey.size()) +
            "|visibleQueuedGpu=" + std::to_string(visibleQueuedGpuPreviewCount) +
            "|waitingResources=" + std::to_string(waitingForResourceCount) +
            "|inFlight=" + std::to_string(m_inFlightThumbnails.size()) +
            "|deferredPersistence=" + std::to_string(m_deferredPersistenceTickets.size()) +
            "|readback=" + std::to_string(m_gpuPreviewReadbackPendingCacheKeys.size()) +
            "|completed=" + std::to_string(m_completedGpuPreviewResultsByCacheKey.size()) +
            "|terminal=" + std::to_string(m_terminalThumbnailResultsByCacheKey.size());
        maintenanceSignature += "|states=";
        for (size_t index = 0u; index < stateCounts.size(); ++index)
        {
            if (index != 0u)
                maintenanceSignature += ',';
            maintenanceSignature += std::to_string(stateCounts[index]);
        }
        const bool stateChanged = maintenanceSignature != m_lastMaintenanceTelemetrySignature;
        const bool lowFrequencySample =
            m_lastMaintenanceTelemetryAt.time_since_epoch().count() == 0 ||
            now - m_lastMaintenanceTelemetryAt >= std::chrono::milliseconds(250);
        if (stateChanged || lowFrequencySample)
        {
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "maintenance|" + maintenanceSignature,
                nullptr,
                m_queuedRequestsByCacheKey.size());
            m_lastMaintenanceTelemetrySignature = std::move(maintenanceSignature);
            m_lastMaintenanceTelemetryAt = now;
        }
    }

    std::vector<std::string> expiredCacheKeys;
    expiredCacheKeys.reserve(trackedCacheKeys.size());
    for (const auto& cacheKey : trackedCacheKeys)
    {
        const AssetThumbnailRequest* deadlineRequest = nullptr;
        if (const auto pending = m_gpuPreviewResourcePendingRequestsByCacheKey.find(cacheKey);
            pending != m_gpuPreviewResourcePendingRequestsByCacheKey.end())
        {
            deadlineRequest = &pending->second;
        }
        else if (const auto queued = m_queuedRequestsByCacheKey.find(cacheKey);
            queued != m_queuedRequestsByCacheKey.end())
        {
            deadlineRequest = &queued->second;
        }
        else if (const auto resolved = m_resolvedPreviewRequestsByCacheKey.find(cacheKey);
            resolved != m_resolvedPreviewRequestsByCacheKey.end())
        {
            deadlineRequest = &resolved->second;
        }
        if (deadlineRequest != nullptr &&
            deadlineRequest->residentPrefabPreviewSource.has_value() &&
            deadlineRequest->residentPrefabPreviewSource->HasIdentity() &&
            SuspendResidentGpuPreviewResourceDeadlineForSceneLoad(
                cacheKey,
                *deadlineRequest,
                now))
        {
            continue;
        }

        const auto deferral = m_gpuPreviewResourcePendingDeferralsByCacheKey.find(cacheKey);
        // Imported Prefabs are serialized by asset. A later import can already
        // have a stable request timer through its resident snapshot while it is
        // still waiting for the preceding import to release the renderer lane.
        // Its resource stall window begins only after its first resource pump.
        if (deadlineRequest != nullptr &&
            deadlineRequest->importedPrefabThumbnailContinuation &&
            deferral == m_gpuPreviewResourcePendingDeferralsByCacheKey.end())
        {
            continue;
        }

        if (deferral != m_gpuPreviewResourcePendingDeferralsByCacheKey.end() &&
            deferral->second.resourceWorkActive)
        {
            // The renderer reported queued or in-flight dependency work on the
            // previous pump. Keep the continuation alive until the next pump can
            // observe that work completing or becoming inactive.
            deferral->second.lastProgressAt = now;
        }
        auto deadlineStart = std::chrono::steady_clock::time_point {};
        if (deferral != m_gpuPreviewResourcePendingDeferralsByCacheKey.end())
        {
            deadlineStart = deferral->second.lastProgressAt.time_since_epoch().count() != 0
                ? deferral->second.lastProgressAt
                : deferral->second.firstDeferredAt;
        }
        else if (const auto requestStart = m_gpuPreviewResourceRequestStartedAtByCacheKey.find(cacheKey);
            requestStart != m_gpuPreviewResourceRequestStartedAtByCacheKey.end())
        {
            deadlineStart = requestStart->second;
        }
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(cacheKey);
        const auto queuedRequestIterator = m_queuedRequestsByCacheKey.find(cacheKey);
        const bool hasVisibleQueuedGpuPreview =
            queuedRequestIterator != m_queuedRequestsByCacheKey.end() &&
            queuedRequestIterator->second.priority == ThumbnailRequestPriority::Visible &&
            SupportsGpuThumbnailPreview(queuedRequestIterator->second);
        const bool hasResourceContinuationState =
            (stateIterator != m_thumbnailStatesByCacheKey.end() &&
                IsGpuPreviewResourceContinuationState(cacheKey, stateIterator->second)) ||
            hasVisibleQueuedGpuPreview;
        if (hasResourceContinuationState &&
            deadlineStart.time_since_epoch().count() != 0 &&
            now - deadlineStart >= kGpuPreviewResourcePendingTimeout)
        {
            expiredCacheKeys.push_back(cacheKey);
        }
    }

    for (const auto& cacheKey : expiredCacheKeys)
    {
        auto pendingRequest = m_gpuPreviewResourcePendingRequestsByCacheKey.find(cacheKey);
        auto queuedRequest = m_queuedRequestsByCacheKey.find(cacheKey);
        auto resolvedRequest = m_resolvedPreviewRequestsByCacheKey.find(cacheKey);
        if (pendingRequest == m_gpuPreviewResourcePendingRequestsByCacheKey.end() &&
            queuedRequest == m_queuedRequestsByCacheKey.end() &&
            resolvedRequest == m_resolvedPreviewRequestsByCacheKey.end())
        {
            ClearGpuPreviewResourcePending(cacheKey);
            continue;
        }

        const auto request = pendingRequest != m_gpuPreviewResourcePendingRequestsByCacheKey.end()
            ? pendingRequest->second
            : (queuedRequest != m_queuedRequestsByCacheKey.end()
                ? queuedRequest->second
                : resolvedRequest->second);
        if (IsPresentationInvalidated(request) || IsPresentationRevisionSuperseded(request))
        {
            ClearGpuPreviewResourcePending(cacheKey);
            m_thumbnailStatesByCacheKey[cacheKey] = ThumbnailState::Cancelled;
            continue;
        }

        const auto evaluation = EvaluateAssetThumbnailCache(
            request,
            AssetThumbnailCacheIntegrityMode::Fast);
        constexpr std::string_view kResourceTimeoutDiagnostic =
            "thumbnail-gpu-preview-resources-timeout:thumbnail-resource-continuation-deadline";
        const auto resolved = m_resolvedPreviewRequestsByCacheKey.find(cacheKey);
        const auto metadataRequest = resolved != m_resolvedPreviewRequestsByCacheKey.end()
            ? BuildResolvedThumbnailCacheRequest(request, resolved->second)
            : request;
        WriteThumbnailMetadataForEvaluation(
            request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            std::string(kResourceTimeoutDiagnostic),
            &metadataRequest);

        auto terminalResult = BuildResultFromEvaluation(
            request,
            evaluation,
            AssetThumbnailServiceStatus::Failed);
        terminalResult.diagnostic = std::string(kResourceTimeoutDiagnostic);
        SynchronizeThumbnailResultPresentationState(terminalResult);

        RemoveQueuedCacheKeyOccurrences(cacheKey);
        m_queuedRequestsByCacheKey.erase(cacheKey);
        m_queuedThumbnailLaneByCacheKey.erase(cacheKey);
        m_resolvedPreviewRequestsByCacheKey.erase(cacheKey);
        m_gpuDeferredHeavyPreviewCacheKeys.erase(cacheKey);
        m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(cacheKey);
        m_gpuPreviewEmptyFrameDeferralsByCacheKey.erase(cacheKey);
        m_thumbnailStatesByCacheKey[cacheKey] = ThumbnailState::Failed;
        ClearGpuPreviewResourcePending(cacheKey);
        m_terminalPresentationRevisions[terminalResult.presentationKey] =
            (std::max)(
                m_terminalPresentationRevisions[terminalResult.presentationKey],
                terminalResult.requestRevision);
        ClearVisibleThumbnailRequestStart(request);
        const auto existingTerminal = m_terminalThumbnailResultsByCacheKey.find(cacheKey);
        if (existingTerminal == m_terminalThumbnailResultsByCacheKey.end() ||
            existingTerminal->second.requestRevision <= terminalResult.requestRevision)
        {
            m_terminalThumbnailResultsByCacheKey.insert_or_assign(
                cacheKey,
                std::move(terminalResult));
        }
        RecordThumbnailGpuPreviewQueueDecisionTelemetry(
            "resource-pending-timeout|diagnostic=" + std::string(kResourceTimeoutDiagnostic),
            &request,
            m_queuedRequestsByCacheKey.size());
    }
}

void AssetThumbnailService::TrackVisibleThumbnailRequestStart(
    const AssetThumbnailRequest& request)
{
    if (request.priority != ThumbnailRequestPriority::Visible ||
        request.requestRevision == 0u)
    {
        return;
    }

    const auto presentationKey = BuildAssetThumbnailPresentationKey(request);
    if (presentationKey.empty())
        return;

    auto iterator = m_visibleThumbnailRequestDeadlinesByPresentationKey.find(presentationKey);
    if (iterator == m_visibleThumbnailRequestDeadlinesByPresentationKey.end())
    {
        m_visibleThumbnailRequestDeadlinesByPresentationKey.emplace(
            presentationKey,
            VisibleThumbnailRequestDeadline {
                request,
                std::chrono::steady_clock::now(),
                false,
                false
            });
        return;
    }

    if (request.requestRevision > iterator->second.request.requestRevision)
    {
        iterator->second.request = request;
        iterator->second.startedAt = std::chrono::steady_clock::now();
        iterator->second.workStarted = false;
        iterator->second.suspendedForSceneLoad = false;
    }
    else if (request.requestRevision == iterator->second.request.requestRevision)
    {
        // The resolved artifact path can be filled in after the first queue
        // lookup. Keep the original start time, but retain the latest request
        // metadata for negative-cache writes and diagnostics.
        iterator->second.request = request;
    }
}

void AssetThumbnailService::MarkVisibleThumbnailRequestWorkStarted(
    const AssetThumbnailRequest& request)
{
    if (request.priority != ThumbnailRequestPriority::Visible ||
        request.requestRevision == 0u)
    {
        return;
    }

    const auto presentationKey = BuildAssetThumbnailPresentationKey(request);
    if (presentationKey.empty())
        return;

    auto iterator = m_visibleThumbnailRequestDeadlinesByPresentationKey.find(presentationKey);
    if (iterator == m_visibleThumbnailRequestDeadlinesByPresentationKey.end())
    {
        m_visibleThumbnailRequestDeadlinesByPresentationKey.emplace(
            presentationKey,
            VisibleThumbnailRequestDeadline {
                request,
                std::chrono::steady_clock::now(),
                true,
                false
            });
        return;
    }
    if (iterator->second.request.requestRevision != request.requestRevision)
        return;

    iterator->second.request = request;
    if (!iterator->second.workStarted)
    {
        iterator->second.startedAt = std::chrono::steady_clock::now();
        iterator->second.workStarted = true;
        iterator->second.suspendedForSceneLoad = false;
    }
}

void AssetThumbnailService::ClearVisibleThumbnailRequestStart(
    const AssetThumbnailRequest& request)
{
    const auto presentationKey = BuildAssetThumbnailPresentationKey(request);
    if (presentationKey.empty())
        return;

    const auto iterator = m_visibleThumbnailRequestDeadlinesByPresentationKey.find(presentationKey);
    if (iterator != m_visibleThumbnailRequestDeadlinesByPresentationKey.end() &&
        (request.requestRevision == 0u ||
            iterator->second.request.requestRevision == request.requestRevision))
    {
        m_visibleThumbnailRequestDeadlinesByPresentationKey.erase(iterator);
    }
}

void AssetThumbnailService::ExpireStalledVisibleThumbnailRequests()
{
    if (m_visibleThumbnailRequestDeadlinesByPresentationKey.empty())
        return;

    struct ActiveOwner
    {
        std::string cacheKey;
        AssetThumbnailRequest request;
        bool found = false;
        bool readbackOrPersistenceActive = false;
    };

    const auto now = std::chrono::steady_clock::now();
    for (auto deadlineIterator = m_visibleThumbnailRequestDeadlinesByPresentationKey.begin();
         deadlineIterator != m_visibleThumbnailRequestDeadlinesByPresentationKey.end();)
    {
        const auto& presentationKey = deadlineIterator->first;
        auto& deadline = deadlineIterator->second;

        // Merely appearing in a large folder is not active work. Heavy previews
        // can wait behind earlier resource continuations for many seconds, and
        // must keep their queue position until their first real scheduler turn.
        if (!deadline.workStarted)
        {
            ++deadlineIterator;
            continue;
        }

        // Resource preparation has its own longer wall-clock deadline. Do not
        // let the ordinary presentation timeout preempt it; large unloaded
        // prefabs and resident scene packages can legitimately spend well over
        // 20 seconds here. Refreshing this timestamp also gives the completed
        // continuation a full presentation window after its resource owner is
        // cleared.
        if (m_gpuPreviewResourceRequestStartedAtByPresentationKey.find(presentationKey) !=
            m_gpuPreviewResourceRequestStartedAtByPresentationKey.end())
        {
            deadline.startedAt = now;
            ++deadlineIterator;
            continue;
        }

        // A resident scene can keep making measurable progress after the
        // renderer's broad scene-load gate has closed (for example while the
        // final material bindings are attached). Refresh the visible window
        // when the registry revision advances so a progressing request does
        // not briefly publish a timeout placeholder before its final package.
        if (deadline.request.residentPrefabPreviewSource.has_value() &&
            deadline.request.residentPrefabPreviewSource->HasIdentity())
        {
            auto refreshedRequest = deadline.request;
            RefreshResidentPreviewRequestState(refreshedRequest);
            if (refreshedRequest.residentPreviewRevision != 0u &&
                refreshedRequest.residentPreviewRevision !=
                    deadline.request.residentPreviewRevision)
            {
                deadline.request = std::move(refreshedRequest);
                deadline.startedAt = now;
                deadline.suspendedForSceneLoad = false;
                ++deadlineIterator;
                continue;
            }
        }

        const bool residentSceneLoadActive =
            deadline.request.residentPrefabPreviewSource.has_value() &&
            deadline.request.residentPrefabPreviewSource->HasIdentity() &&
            NLS::Editor::Core::HasActiveSceneLoadRendererResourceResolution();
        if (residentSceneLoadActive)
        {
            // Scene restoration owns the same Mesh/Material work that the
            // resident thumbnail will reuse. Do not turn an in-progress load
            // into a terminal placeholder merely because the UI deadline
            // started before those resources became available.
            if (!deadline.suspendedForSceneLoad)
            {
                deadline.startedAt = now;
                deadline.suspendedForSceneLoad = true;
            }
            ++deadlineIterator;
            continue;
        }
        if (deadline.suspendedForSceneLoad)
        {
            // The scene load just completed. Start a complete presentation
            // window now so the newly attached resident resources can render
            // and publish their thumbnail before expiry is considered again.
            deadline.startedAt = now;
            deadline.suspendedForSceneLoad = false;
            ++deadlineIterator;
            continue;
        }
        if (deadline.startedAt.time_since_epoch().count() == 0 ||
            now - deadline.startedAt < kVisibleThumbnailRequestTimeout)
        {
            ++deadlineIterator;
            continue;
        }

        const auto latest = m_latestPresentationRevisions.find(presentationKey);
        if (latest != m_latestPresentationRevisions.end() &&
            latest->second > deadline.request.requestRevision)
        {
            deadlineIterator = m_visibleThumbnailRequestDeadlinesByPresentationKey.erase(
                deadlineIterator);
            continue;
        }

        // The worker and the timeout maintenance pass can cross at the disk
        // commit boundary. Recover an already committed canonical result
        // before publishing a terminal timeout. Full validation includes the
        // PNG, metadata identity and current presentation index; it is only
        // paid after the visible request has exceeded its bounded deadline.
        auto recoveryEvaluation = EvaluateAssetThumbnailCache(
            deadline.request,
            AssetThumbnailCacheIntegrityMode::Full);
        if (PromoteFreshCanonicalThumbnailIfAvailable(
                deadline.request,
                recoveryEvaluation))
        {
            auto recoveredResult = BuildResultFromEvaluation(
                deadline.request,
                recoveryEvaluation,
                AssetThumbnailServiceStatus::Fresh);
            m_stableThumbnailResultsByCacheKey[recoveryEvaluation.entry->cacheKey] = recoveredResult;
            m_thumbnailStatesByCacheKey[recoveryEvaluation.entry->cacheKey] = ThumbnailState::Ready;
            m_terminalPresentationRevisions.erase(presentationKey);
            for (auto terminalIterator = m_terminalThumbnailResultsByCacheKey.begin();
                 terminalIterator != m_terminalThumbnailResultsByCacheKey.end();)
            {
                if (terminalIterator->second.presentationKey == presentationKey &&
                    terminalIterator->second.requestRevision <= deadline.request.requestRevision)
                {
                    terminalIterator = m_terminalThumbnailResultsByCacheKey.erase(terminalIterator);
                }
                else
                {
                    ++terminalIterator;
                }
            }
            ClearVisibleThumbnailRequestStart(deadline.request);
            deadlineIterator = m_visibleThumbnailRequestDeadlinesByPresentationKey.erase(
                deadlineIterator);
            continue;
        }

        ActiveOwner owner;
        const auto consider = [&](const std::string& cacheKey,
                                  const AssetThumbnailRequest& request,
                                  const bool readbackOrPersistenceActive = false)
        {
            if (BuildAssetThumbnailPresentationKey(request) != presentationKey ||
                request.requestRevision != deadline.request.requestRevision)
            {
                return;
            }
            if (!owner.found)
            {
                owner.cacheKey = cacheKey;
                owner.request = request;
                owner.found = true;
            }
            owner.readbackOrPersistenceActive =
                owner.readbackOrPersistenceActive || readbackOrPersistenceActive;
        };

        for (const auto& [cacheKey, request] : m_queuedRequestsByCacheKey)
            consider(cacheKey, request);
        for (const auto& [cacheKey, request] : m_gpuPreviewResourcePendingRequestsByCacheKey)
            consider(cacheKey, request);
        for (const auto& [cacheKey, request] : m_resolvedPreviewRequestsByCacheKey)
            consider(cacheKey, request);
        for (const auto& [cacheKey, request] : m_gpuPreviewReadbackPendingRequestsByCacheKey)
            consider(cacheKey, request, true);
        for (const auto& [cacheKey, deferral] : m_gpuPreviewEmptyFrameDeferralsByCacheKey)
            consider(cacheKey, deferral.request);
        for (const auto& inFlight : m_inFlightThumbnails)
        {
            consider(inFlight.cacheKey, inFlight.request, inFlight.persistenceOnly);
        }
        for (const auto& ticket : m_deferredPersistenceTickets)
            consider(ticket.cacheKey, ticket.request, true);

        const auto baseCacheKey = BuildAssetThumbnailCacheKey(deadline.request);
        if (!baseCacheKey.empty() &&
            m_previewRequestResolutionFuturesByCacheKey.find(baseCacheKey) !=
                m_previewRequestResolutionFuturesByCacheKey.end())
        {
            owner.cacheKey = baseCacheKey;
            owner.request = deadline.request;
            owner.found = true;
        }

        const auto hasTerminalResult = [&]()
        {
            for (const auto& [_, result] : m_terminalThumbnailResultsByCacheKey)
            {
                if (result.presentationKey == presentationKey &&
                    result.requestRevision == deadline.request.requestRevision)
                {
                    return true;
                }
            }
            return false;
        }();
        if (hasTerminalResult)
        {
            deadlineIterator = m_visibleThumbnailRequestDeadlinesByPresentationKey.erase(
                deadlineIterator);
            continue;
        }

        if (!owner.found)
        {
            const auto stable = std::find_if(
                m_stableThumbnailResultsByCacheKey.begin(),
                m_stableThumbnailResultsByCacheKey.end(),
                [&presentationKey, &deadline](const auto& candidate)
                {
                    return candidate.second.presentationKey == presentationKey &&
                        candidate.second.requestRevision == deadline.request.requestRevision;
                });
            if (stable != m_stableThumbnailResultsByCacheKey.end())
            {
                deadlineIterator = m_visibleThumbnailRequestDeadlinesByPresentationKey.erase(
                    deadlineIterator);
                    continue;
            }
        }

        // The request can briefly leave every normal ownership table while a
        // worker result, deferred manifest lookup, or resource continuation is
        // being transferred between lanes. The visible deadline is still a
        // valid owner for this revision. Without this fallback, the UI can
        // assign a new revision on every lookup and keep a stale Preparing
        // state alive forever.
        if (!owner.found)
        {
            const auto fallbackCacheKey = BuildAssetThumbnailCacheKey(deadline.request);
            if (!fallbackCacheKey.empty())
            {
                owner.cacheKey = fallbackCacheKey;
                owner.request = deadline.request;
                owner.found = true;
            }
        }

        if (!owner.found || owner.readbackOrPersistenceActive)
        {
            ++deadlineIterator;
            continue;
        }

        std::vector<std::string> ownedCacheKeys;
        const auto collect = [&](const std::string& cacheKey,
                                 const AssetThumbnailRequest& request)
        {
            if (BuildAssetThumbnailPresentationKey(request) == presentationKey &&
                request.requestRevision == deadline.request.requestRevision &&
                std::find(ownedCacheKeys.begin(), ownedCacheKeys.end(), cacheKey) ==
                    ownedCacheKeys.end())
            {
                ownedCacheKeys.push_back(cacheKey);
            }
        };
        for (const auto& [cacheKey, request] : m_queuedRequestsByCacheKey)
            collect(cacheKey, request);
        for (const auto& [cacheKey, request] : m_gpuPreviewResourcePendingRequestsByCacheKey)
            collect(cacheKey, request);
        for (const auto& [cacheKey, request] : m_resolvedPreviewRequestsByCacheKey)
            collect(cacheKey, request);
        for (const auto& inFlight : m_inFlightThumbnails)
            collect(inFlight.cacheKey, inFlight.request);
        if (ownedCacheKeys.empty())
            ownedCacheKeys.push_back(owner.cacheKey);

        for (const auto& cacheKey : ownedCacheKeys)
        {
            RemoveQueuedCacheKeyOccurrences(cacheKey);
            m_queuedRequestsByCacheKey.erase(cacheKey);
            m_queuedThumbnailLaneByCacheKey.erase(cacheKey);
            m_resolvedPreviewRequestsByCacheKey.erase(cacheKey);
            m_previewRequestResolutionFuturesByCacheKey.erase(cacheKey);
            m_gpuDeferredHeavyPreviewCacheKeys.erase(cacheKey);
            m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(cacheKey);
            m_gpuPreviewEmptyFrameDeferralsByCacheKey.erase(cacheKey);
            m_completedGpuPreviewResultsByCacheKey.erase(cacheKey);
            ClearGpuPreviewResourcePending(cacheKey);
            ClearGpuPreviewReadbackPending(cacheKey);
            m_thumbnailStatesByCacheKey[cacheKey] = ThumbnailState::Failed;
        }

        for (auto& inFlight : m_inFlightThumbnails)
        {
            if (std::find(ownedCacheKeys.begin(), ownedCacheKeys.end(), inFlight.cacheKey) !=
                ownedCacheKeys.end() && inFlight.cancelToken)
            {
                inFlight.cancelToken->cancelled.store(true, std::memory_order_relaxed);
            }
        }
        for (auto& ticket : m_deferredPersistenceTickets)
        {
            if (std::find(ownedCacheKeys.begin(), ownedCacheKeys.end(), ticket.cacheKey) !=
                ownedCacheKeys.end() && ticket.cancelToken)
            {
                ticket.cancelToken->cancelled.store(true, std::memory_order_relaxed);
            }
        }

        const auto evaluation = EvaluateAssetThumbnailCache(
            deadline.request,
            AssetThumbnailCacheIntegrityMode::Fast);
        constexpr std::string_view kVisibleTimeoutDiagnostic =
            "thumbnail-visible-request-timeout";
        WriteThumbnailMetadataForEvaluation(
            deadline.request,
            evaluation,
            AssetThumbnailCacheStatus::Failed,
            std::string(kVisibleTimeoutDiagnostic));
        auto terminalResult = BuildResultFromEvaluation(
            deadline.request,
            evaluation,
            AssetThumbnailServiceStatus::Failed);
        terminalResult.diagnostic = std::string(kVisibleTimeoutDiagnostic);
        SynchronizeThumbnailResultPresentationState(terminalResult);
        m_terminalPresentationRevisions[presentationKey] = deadline.request.requestRevision;
        const auto terminalCacheKey = ownedCacheKeys.front();
        m_terminalThumbnailResultsByCacheKey.insert_or_assign(
            terminalCacheKey,
            std::move(terminalResult));
        RecordThumbnailGpuPreviewQueueDecisionTelemetry(
            "visible-request-timeout",
            &deadline.request,
            m_queuedRequestsByCacheKey.size());
        deadlineIterator = m_visibleThumbnailRequestDeadlinesByPresentationKey.erase(
            deadlineIterator);
    }
}

#if defined(NLS_ENABLE_TEST_HOOKS)
void AssetThumbnailService::SetGpuPreviewResourcePendingAgeForTesting(
    const AssetThumbnailRequest& request,
    const std::chrono::steady_clock::duration age)
{
    const auto cacheKey = BuildAssetThumbnailCacheKey(request);
    const auto start = std::chrono::steady_clock::now() - age;
    m_gpuPreviewResourceRequestStartedAtByCacheKey[cacheKey] = start;
    const auto presentationKey = BuildAssetThumbnailPresentationKey(request);
    m_gpuPreviewResourcePresentationKeyByCacheKey[cacheKey] = presentationKey;
    if (!presentationKey.empty())
        m_gpuPreviewResourceRequestStartedAtByPresentationKey[presentationKey] = start;
    auto& deferral = m_gpuPreviewResourcePendingDeferralsByCacheKey[cacheKey];
    deferral.firstDeferredAt = start;
    deferral.lastProgressAt = start;
}

void AssetThumbnailService::SetGpuPreviewResourceRequestStartAgeForTesting(
    const AssetThumbnailRequest& request,
    const std::chrono::steady_clock::duration age)
{
    const auto cacheKey = BuildAssetThumbnailCacheKey(request);
    const auto start = std::chrono::steady_clock::now() - age;
    m_gpuPreviewResourceRequestStartedAtByCacheKey[cacheKey] = start;
    const auto presentationKey = BuildAssetThumbnailPresentationKey(request);
    m_gpuPreviewResourcePresentationKeyByCacheKey[cacheKey] = presentationKey;
    if (!presentationKey.empty())
        m_gpuPreviewResourceRequestStartedAtByPresentationKey[presentationKey] = start;
}

void AssetThumbnailService::SetVisibleThumbnailRequestAgeForTesting(
    const AssetThumbnailRequest& request,
    const std::chrono::steady_clock::duration age,
    const bool workStarted)
{
    const auto presentationKey = BuildAssetThumbnailPresentationKey(request);
    auto iterator = m_visibleThumbnailRequestDeadlinesByPresentationKey.find(presentationKey);
    if (iterator == m_visibleThumbnailRequestDeadlinesByPresentationKey.end())
    {
        m_visibleThumbnailRequestDeadlinesByPresentationKey.emplace(
            presentationKey,
            VisibleThumbnailRequestDeadline {
                request,
                std::chrono::steady_clock::now() - age,
                workStarted,
                false
            });
        return;
    }
    iterator->second.startedAt = std::chrono::steady_clock::now() - age;
    iterator->second.workStarted = workStarted;
}

void AssetThumbnailService::DropGpuPreviewResourcePendingOwnershipForTesting(
    const AssetThumbnailRequest& request)
{
    const auto cacheKey = BuildAssetThumbnailCacheKey(request);
    m_gpuPreviewResourcePendingRequestsByCacheKey.erase(cacheKey);
    m_gpuPreviewResourcePendingDeferredCacheKeys.erase(cacheKey);
    m_gpuPreviewResourcePendingDeferralsByCacheKey.erase(cacheKey);
    m_gpuPreviewResidentPartialRevisionByCacheKey.erase(cacheKey);
    m_gpuPreviewReadyResidentCacheKeys.erase(cacheKey);
}

void AssetThumbnailService::SetThumbnailStateForTesting(
    const AssetThumbnailRequest& request,
    const ThumbnailState state)
{
    m_thumbnailStatesByCacheKey[BuildAssetThumbnailCacheKey(request)] = state;
}

void AssetThumbnailService::DropGpuPreviewResourceQueueOwnershipForTesting(
    const AssetThumbnailRequest& request)
{
    const auto cacheKey = BuildAssetThumbnailCacheKey(request);
    RemoveQueuedCacheKeyOccurrences(cacheKey);
    m_queuedRequestsByCacheKey.erase(cacheKey);
    m_queuedThumbnailLaneByCacheKey.erase(cacheKey);
}

void AssetThumbnailService::DropGpuPreviewQueueLaneMembershipForTesting(
    const AssetThumbnailRequest& request)
{
    // Keep the queued-request map intact: this simulates the bookkeeping race
    // that RepairQueuedRequestLaneMembership is required to recover from.
    RemoveQueuedCacheKeyOccurrences(BuildAssetThumbnailCacheKey(request));
}

void AssetThumbnailService::QueueTerminalAndLateFreshResultForTesting(
    const AssetThumbnailRequest& request)
{
    const auto cacheKey = BuildAssetThumbnailCacheKey(request);
    const auto presentationKey = BuildAssetThumbnailPresentationKey(request);
    const auto evaluation = EvaluateAssetThumbnailCache(
        request,
        AssetThumbnailCacheIntegrityMode::Full);
    if (cacheKey.empty() || presentationKey.empty() ||
        request.requestRevision == 0u || !evaluation.entry.has_value() ||
        evaluation.status != AssetThumbnailCacheStatus::Fresh)
    {
        return;
    }

    constexpr std::string_view kDiagnostic = "thumbnail-visible-request-timeout";
    (void)WriteAssetThumbnailCacheMetadata(
        request,
        *evaluation.entry,
        AssetThumbnailCacheStatus::Failed,
        std::string(kDiagnostic));
    auto terminalResult = BuildResultFromEvaluation(
        request,
        evaluation,
        AssetThumbnailServiceStatus::Failed);
    terminalResult.diagnostic = std::string(kDiagnostic);
    SynchronizeThumbnailResultPresentationState(terminalResult);
    m_terminalPresentationRevisions[presentationKey] = request.requestRevision;
    m_terminalThumbnailResultsByCacheKey.insert_or_assign(
        cacheKey,
        terminalResult);
    m_thumbnailStatesByCacheKey[cacheKey] = ThumbnailState::Failed;

    auto freshResult = BuildResultFromEvaluation(
        request,
        evaluation,
        AssetThumbnailServiceStatus::Fresh);
    std::promise<AssetThumbnailServiceResult> promise;
    auto future = promise.get_future();
    promise.set_value(std::move(freshResult));
    auto cancelToken = std::make_shared<AssetThumbnailGenerationCancelToken>();
    m_inFlightThumbnails.push_back({
        cacheKey,
        m_generationSerial,
        std::move(cancelToken),
        std::move(future),
        request,
        false,
        false
    });
}

size_t AssetThumbnailService::GetResidentPreviewOwnerCountForTesting() const
{
    return m_residentPreviewRequestsByPresentationKey.size();
}
#endif

void AssetThumbnailService::RestoreGpuPreviewResourceContinuationRequests()
{
    // GetThumbnail applies the wall-clock query deadline itself. The
    // generation path applies its bounded retry policy when it actually pumps
    // the continuation; keeping expiration out of this restore pass ensures a
    // slow test or editor frame still returns the terminal result from the
    // generation call rather than silently removing its queue entry first.
    const auto isQueuedInAnyLane = [this](const std::string& cacheKey)
    {
        return m_queuedThumbnailLaneByCacheKey.find(cacheKey) !=
            m_queuedThumbnailLaneByCacheKey.end();
    };
    const auto residentRevisionAdvanced =
        [](const AssetThumbnailRequest& request, const uint64_t previousRevision)
    {
        if (!request.residentPrefabPreviewSource.has_value() ||
            !request.residentPrefabPreviewSource->HasIdentity())
        {
            return true;
        }
        const auto registry = request.residentPrefabPreviewSource->registry.lock();
        if (registry == nullptr)
            return true;
        const auto state = registry->GetSnapshotState(
            request.residentPrefabPreviewSource->runtimeCacheIdentity,
            request.residentPrefabPreviewSource->freshnessFingerprint);
        // A disappeared registry entry must be allowed to recover through the
        // normal preparation path; otherwise a stale partial result could
        // strand the request indefinitely. Resource packages can also become
        // complete in place without changing their identity/revision, so the
        // completion bit is an independent promotion signal.
        return !state.has_value() || state->complete || state->revision > previousRevision;
    };

    // WaitingForResources is the state written at every resource-pump
    // deferral. Repair the auxiliary ownership table before scanning it so a
    // queue/generation transition cannot strand a request as ordinary heavy
    // work. Do not recreate ownership for readback or terminal states.
    std::vector<std::pair<std::string, AssetThumbnailRequest>> missingOwnership;
    missingOwnership.reserve(m_queuedRequestsByCacheKey.size());
    for (const auto& [cacheKey, request] : m_queuedRequestsByCacheKey)
    {
        const auto state = m_thumbnailStatesByCacheKey.find(cacheKey);
        if (state == m_thumbnailStatesByCacheKey.end() ||
            state->second != ThumbnailState::WaitingForResources ||
            m_gpuPreviewResourcePendingRequestsByCacheKey.find(cacheKey) !=
                m_gpuPreviewResourcePendingRequestsByCacheKey.end() ||
            !SupportsGpuThumbnailPreview(request))
        {
            continue;
        }
        if (IsPresentationInvalidated(request) || IsPresentationRevisionSuperseded(request))
            continue;
        missingOwnership.emplace_back(cacheKey, request);
    }
    // The resolved request table is also an active owner while a resource
    // continuation is waiting. It can outlive the ordinary queue during a
    // dequeue/validation transition, so repair from it as well instead of
    // allowing a resolved preview to become invisible to the scheduler.
    for (const auto& [cacheKey, request] : m_resolvedPreviewRequestsByCacheKey)
    {
        const auto state = m_thumbnailStatesByCacheKey.find(cacheKey);
        if (state == m_thumbnailStatesByCacheKey.end() ||
            state->second != ThumbnailState::WaitingForResources ||
            m_queuedRequestsByCacheKey.find(cacheKey) != m_queuedRequestsByCacheKey.end() ||
            m_gpuPreviewResourcePendingRequestsByCacheKey.find(cacheKey) !=
                m_gpuPreviewResourcePendingRequestsByCacheKey.end() ||
            !SupportsGpuThumbnailPreview(request))
        {
            continue;
        }
        if (IsPresentationInvalidated(request) || IsPresentationRevisionSuperseded(request))
            continue;
        missingOwnership.emplace_back(cacheKey, request);
    }
    for (const auto& [cacheKey, request] : missingOwnership)
    {
        m_gpuPreviewResourcePendingRequestsByCacheKey.emplace(cacheKey, request);
        TrackGpuPreviewResourceRequestStart(cacheKey, request);
        RecordThumbnailGpuPreviewQueueDecisionTelemetry(
            "repair-resource-pending-ownership",
            &request,
            m_queuedRequestsByCacheKey.size());
    }

    for (auto iterator = m_gpuPreviewResourcePendingRequestsByCacheKey.begin();
         iterator != m_gpuPreviewResourcePendingRequestsByCacheKey.end();)
    {
        const auto cacheKey = iterator->first;
        const auto& request = iterator->second;
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(cacheKey);
        if (stateIterator == m_thumbnailStatesByCacheKey.end() ||
            !IsGpuPreviewResourceContinuationState(cacheKey, stateIterator->second))
        {
            m_gpuPreviewResourcePendingDeferredCacheKeys.erase(cacheKey);
            m_gpuPreviewResourcePendingDeferralsByCacheKey.erase(cacheKey);
            m_gpuPreviewResidentPartialRevisionByCacheKey.erase(cacheKey);
            iterator = m_gpuPreviewResourcePendingRequestsByCacheKey.erase(iterator);
            continue;
        }

        if (IsPresentationInvalidated(request) || IsPresentationRevisionSuperseded(request))
        {
            m_gpuPreviewResourcePendingDeferredCacheKeys.erase(cacheKey);
            m_gpuPreviewResourcePendingDeferralsByCacheKey.erase(cacheKey);
            m_gpuPreviewResidentPartialRevisionByCacheKey.erase(cacheKey);
            iterator = m_gpuPreviewResourcePendingRequestsByCacheKey.erase(iterator);
            continue;
        }

        bool residentRevisionAdvancedForRequest = false;
        bool residentPackageComplete = false;
        uint64_t residentPackageRevision = 0u;
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
                    residentPackageComplete = state->complete;
                    residentPackageRevision = state->revision;
                }
            }
        }
        if (const auto partialRevision =
                m_gpuPreviewResidentPartialRevisionByCacheKey.find(cacheKey);
            partialRevision != m_gpuPreviewResidentPartialRevisionByCacheKey.end())
        {
            if (!residentPackageComplete &&
                !residentRevisionAdvanced(request, partialRevision->second))
            {
                // The partial GPU texture remains the stable presentation for
                // this revision. Wait for the scene registry to publish a new
                // package before spending another render/readback slot.
                ++iterator;
                continue;
            }
            m_gpuPreviewResidentPartialRevisionByCacheKey.erase(partialRevision);
            residentRevisionAdvancedForRequest = true;
        }

        if (residentPackageComplete || residentRevisionAdvancedForRequest)
        {
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "restore-resource-inspect|complete=" +
                    std::to_string(residentPackageComplete ? 1u : 0u) +
                    "|revisionAdvanced=" +
                    std::to_string(residentRevisionAdvancedForRequest ? 1u : 0u) +
                    "|queued=" +
                    std::to_string(m_queuedRequestsByCacheKey.find(cacheKey) !=
                        m_queuedRequestsByCacheKey.end() ? 1u : 0u) +
                    "|state=" +
                    std::to_string(static_cast<unsigned int>(stateIterator->second)),
                &request,
                m_queuedRequestsByCacheKey.size());
        }

        if (residentRevisionAdvancedForRequest)
        {
            // The parked continuation owns the request captured when the
            // partial frame was rendered. Refresh all request owners before
            // requeueing so the renderer sees the newly complete resident
            // snapshot instead of rendering the same provisional revision.
            auto refreshedRequest = iterator->second;
            RefreshResidentPreviewRequestState(refreshedRequest);
            // The completion bit was read from the same exact identity above.
            // If the registry is being updated concurrently, a second weak
            // lookup during Refresh can transiently return the previous
            // provisional state. Preserve the authoritative completion sample
            // for this maintenance pass so the scheduler cannot miss the
            // canonical retry window.
            if (residentPackageComplete)
            {
                if (residentPackageRevision != 0u)
                    refreshedRequest.residentPreviewRevision = residentPackageRevision;
                refreshedRequest.residentPreviewPartial = false;
            }
            iterator->second = refreshedRequest;
            // Keep an explicit promotion marker until canonical submission
            // starts. This survives a transient owner-table/lane rotation and
            // lets the scheduler admit the request immediately.
            if (residentPackageComplete ||
                (refreshedRequest.residentPreviewRevision != 0u &&
                    !refreshedRequest.residentPreviewPartial))
            {
                const bool readyMarkerInserted =
                    TryPublishResidentCompletionPromotion(cacheKey, refreshedRequest);
                if (readyMarkerInserted)
                {
                    // The previous partial continuation may have spent most
                    // of its deadline while scene resources were loading.
                    // Start both bounded clocks when completion is promoted.
                    ClearVisibleThumbnailRequestStart(refreshedRequest);
                    TrackVisibleThumbnailRequestStart(refreshedRequest);
                    TrackGpuPreviewResourceRequestStart(cacheKey, refreshedRequest);
                }
            }
            if (const auto resolved = m_resolvedPreviewRequestsByCacheKey.find(cacheKey);
                resolved != m_resolvedPreviewRequestsByCacheKey.end())
            {
                RefreshResidentPreviewRequestState(resolved->second);
            }
            if (const auto queued = m_queuedRequestsByCacheKey.find(cacheKey);
                queued != m_queuedRequestsByCacheKey.end())
            {
                queued->second = refreshedRequest;
            }
            m_gpuPreviewResourcePendingDeferredCacheKeys.erase(cacheKey);
        }

        const auto& refreshedRequest = iterator->second;
        const bool hasQueuedRequest = m_queuedRequestsByCacheKey.find(cacheKey) !=
            m_queuedRequestsByCacheKey.end();
        if (!hasQueuedRequest && EnsureQueuedRequestCapacityFor(cacheKey, refreshedRequest))
        {
            m_queuedRequestsByCacheKey.emplace(cacheKey, refreshedRequest);
            EnqueueQueuedCacheKey(cacheKey, refreshedRequest);
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "restore-resource-pending-request",
                &refreshedRequest,
                m_queuedRequestsByCacheKey.size());
        }
        else if (hasQueuedRequest && !isQueuedInAnyLane(cacheKey))
        {
            // A continuation can be popped before a later validation or budget
            // branch returns. Repair lane membership from the authoritative
            // request table instead of leaving it invisible to HasQueuedCacheKeys.
            RemoveQueuedCacheKeyOccurrences(cacheKey);
            EnqueueQueuedCacheKey(cacheKey, refreshedRequest);
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "repair-resource-pending-queue",
                &refreshedRequest,
                m_queuedRequestsByCacheKey.size());
        }
        ++iterator;
    }
}

void AssetThumbnailService::PromoteCompletedResidentPreviewOwners()
{
    // Scene restoration may complete a resident package between the regular
    // maintenance pass and scheduler admission. Reconcile every live owner
    // table here so completion cannot be lost at that boundary.
    std::vector<std::pair<std::string, AssetThumbnailRequest>> candidates;
    std::unordered_set<std::string> candidateKeys;
    const auto collect = [&candidates, &candidateKeys](
        const auto& owners)
    {
        for (const auto& [cacheKey, request] : owners)
        {
            if (candidateKeys.insert(cacheKey).second)
                candidates.emplace_back(cacheKey, request);
        }
    };
    collect(m_gpuPreviewResourcePendingRequestsByCacheKey);
    collect(m_queuedRequestsByCacheKey);
    collect(m_resolvedPreviewRequestsByCacheKey);
    for (const auto& [presentationKey, request] : m_residentPreviewRequestsByPresentationKey)
    {
        if (presentationKey.empty())
            continue;
        const auto cacheKey = BuildAssetThumbnailCacheKey(request);
        const auto existing = std::find_if(
            candidates.begin(),
            candidates.end(),
            [&presentationKey](const auto& candidate)
            {
                return BuildAssetThumbnailPresentationKey(candidate.second) == presentationKey;
            });
        if (existing != candidates.end())
        {
            // The queue owner can still contain the provisional request after
            // GetThumbnail has observed the complete registry package. Merge
            // the latest presentation owner into that candidate so promotion
            // never re-checks an older partial snapshot.
            if (request.requestRevision >= existing->second.requestRevision)
                existing->second = request;
            continue;
        }
        if (!cacheKey.empty() && candidateKeys.insert(cacheKey).second)
            candidates.emplace_back(cacheKey, request);
    }

    for (const auto& [cacheKey, owner] : candidates)
    {
        if (!owner.residentPrefabPreviewSource.has_value() ||
            !owner.residentPrefabPreviewSource->HasIdentity() ||
            !SupportsGpuThumbnailPreview(owner))
        {
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "resident-promotion-skip=not-resident-gpu-owner",
                &owner,
                candidates.size());
            continue;
        }

        const auto registry = owner.residentPrefabPreviewSource->registry.lock();
        if (registry == nullptr)
        {
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "resident-promotion-skip=registry-expired",
                &owner,
                candidates.size());
            continue;
        }
        const auto state = registry->GetSnapshotState(
            owner.residentPrefabPreviewSource->runtimeCacheIdentity,
            owner.residentPrefabPreviewSource->freshnessFingerprint);
        if (!state.has_value())
        {
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "resident-promotion-skip=snapshot-missing",
                &owner,
                candidates.size());
            continue;
        }
        // GetThumbnail refreshes and stores the resident owner immediately
        // before promotion. If the registry's weak state sample briefly lags
        // that refresh, the owner still carries the authoritative complete
        // revision for this presentation and must not be demoted back to the
        // provisional lane.
        const bool ownerReportsComplete = owner.residentPreviewRevision != 0u &&
            !owner.residentPreviewPartial;
        if (!state->complete && !ownerReportsComplete)
        {
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "resident-promotion-skip=snapshot-incomplete",
                &owner,
                candidates.size());
            continue;
        }

        auto refreshed = owner;
        RefreshResidentPreviewRequestState(refreshed);
        // The registry snapshot above is the authoritative completion sample for
        // this maintenance pass. A second weak lookup can transiently observe a
        // stale/incomplete state while scene restoration publishes the package;
        // preserve the complete sample instead of demoting the request again.
        if (ownerReportsComplete && !state->complete)
        {
            refreshed.residentPreviewRevision = owner.residentPreviewRevision;
            refreshed.residentPreviewPartial = false;
        }
        else if (state->revision != 0u &&
            (refreshed.residentPreviewRevision == 0u ||
                refreshed.residentPreviewPartial))
        {
            refreshed.residentPreviewRevision = state->revision;
            refreshed.residentPreviewPartial = false;
        }
        if (refreshed.residentPreviewRevision == 0u || refreshed.residentPreviewPartial)
        {
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "resident-promotion-skip=refresh-partial",
                &owner,
                candidates.size());
            continue;
        }

        // Completion promotion is a one-shot transition. Keep polling calls
        // idempotent, but still repair a request whose physical lane entry was
        // lost by a bookkeeping race.
        const auto queued = m_queuedRequestsByCacheKey.find(cacheKey);
        const auto promotedRevision =
            m_promotedResidentRevisionByPresentationKey.find(
                BuildAssetThumbnailPresentationKey(refreshed));
        const bool alreadyPromoted =
            promotedRevision != m_promotedResidentRevisionByPresentationKey.end() &&
            promotedRevision->second >= refreshed.residentPreviewRevision;
        if (alreadyPromoted)
            continue;

        const auto thumbnailState = m_thumbnailStatesByCacheKey.find(cacheKey);
        const auto isActiveReadbackOrPersistenceState = [](const ThumbnailState state)
        {
            return state == ThumbnailState::WaitingForGpu ||
                state == ThumbnailState::Readback ||
                state == ThumbnailState::Encoding ||
                state == ThumbnailState::Persisting;
        };

        // A complete resident package is allowed to take over a provisional
        // presentation. The stable result can be indexed by the resolved
        // cache key rather than the request's current key, so inspect the
        // presentation identity as well.
        bool hasPartialStablePresentation = false;
        uint64_t partialStableRevision = 0u;
        for (const auto& [stableCacheKey, stableResult] : m_stableThumbnailResultsByCacheKey)
        {
            if (stableResult.presentationKey != BuildAssetThumbnailPresentationKey(refreshed) ||
                !stableResult.residentPreviewPartial)
            {
                continue;
            }
            hasPartialStablePresentation = true;
            partialStableRevision = stableResult.residentPreviewRevision;
            (void)stableCacheKey;
            break;
        }

        const auto cancelProvisionalPreparation = [&]
        {
            for (auto& inFlight : m_inFlightThumbnails)
            {
                if (inFlight.cacheKey != cacheKey || !inFlight.future.valid() ||
                    !inFlight.cancelToken)
                {
                    continue;
                }
                const bool belongsToPartialRevision =
                    inFlight.request.residentPreviewPartial ||
                    (partialStableRevision != 0u &&
                        inFlight.request.residentPreviewRevision == partialStableRevision);
                if (belongsToPartialRevision)
                    inFlight.cancelToken->cancelled.store(true, std::memory_order_relaxed);
            }
        };
        if (hasPartialStablePresentation)
            cancelProvisionalPreparation();

        const auto isPendingPreparationFuture = [](std::future<AssetThumbnailRequest>& future)
        {
            if (!future.valid())
                return false;
            // A completed resolution future is no longer preparation work. It
            // may still be consumed by the next generation pass, but it must
            // not block promotion of a package the registry already marked
            // complete.
            return future.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
        };
        const bool hasActivePreparation = std::any_of(
            m_inFlightThumbnails.begin(),
            m_inFlightThumbnails.end(),
            [&cacheKey](const InFlightThumbnailRequest& inFlight)
            {
                return inFlight.cacheKey == cacheKey && inFlight.future.valid() &&
                    inFlight.future.wait_for(std::chrono::seconds(0)) !=
                        std::future_status::ready &&
                    (!inFlight.cancelToken ||
                        !inFlight.cancelToken->cancelled.load(std::memory_order_relaxed));
            }) ||
            [&]
            {
                const auto resolution = m_previewRequestResolutionFuturesByCacheKey.find(cacheKey);
                return resolution != m_previewRequestResolutionFuturesByCacheKey.end() &&
                    isPendingPreparationFuture(resolution->second);
            }() ||
            std::any_of(
                m_deferredPersistenceTickets.begin(),
                m_deferredPersistenceTickets.end(),
                [&cacheKey](const DeferredPersistenceTicket& ticket)
                {
                    return ticket.cacheKey == cacheKey &&
                        (!ticket.cancelToken ||
                            !ticket.cancelToken->cancelled.load(std::memory_order_relaxed));
                });
        const bool hasCanonicalPresentation = [&]
        {
            const auto stable = m_stableThumbnailResultsByCacheKey.find(cacheKey);
            if (stable == m_stableThumbnailResultsByCacheKey.end() ||
                stable->second.residentPreviewPartial)
            {
                return false;
            }
            if (stable->second.gpuTexture.IsValid())
                return true;
            if (stable->second.imagePath.empty())
                return false;
            std::error_code imageError;
            return std::filesystem::is_regular_file(stable->second.imagePath, imageError) &&
                !imageError;
        }();
        const bool stateIsActiveCanonical = thumbnailState != m_thumbnailStatesByCacheKey.end() &&
            isActiveReadbackOrPersistenceState(thumbnailState->second) &&
            !hasPartialStablePresentation;
        const bool stateCanBePromoted = thumbnailState == m_thumbnailStatesByCacheKey.end() ||
            thumbnailState->second == ThumbnailState::Missing ||
            thumbnailState->second == ThumbnailState::Queued ||
            thumbnailState->second == ThumbnailState::Preparing ||
            thumbnailState->second == ThumbnailState::WaitingForResources ||
            thumbnailState->second == ThumbnailState::Rendering ||
            thumbnailState->second == ThumbnailState::Ready ||
            thumbnailState->second == ThumbnailState::Failed ||
            thumbnailState->second == ThumbnailState::Cancelled;
        if (hasCanonicalPresentation || stateIsActiveCanonical ||
            (!stateCanBePromoted && !hasPartialStablePresentation) ||
            (hasActivePreparation && !hasPartialStablePresentation))
        {
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "resident-promotion-skip=thumbnail-state|state=" +
                    std::to_string(thumbnailState == m_thumbnailStatesByCacheKey.end()
                        ? static_cast<unsigned int>(ThumbnailState::Missing)
                        : static_cast<unsigned int>(thumbnailState->second)) +
                    "|partial=" + std::to_string(hasPartialStablePresentation ? 1u : 0u) +
                    "|active=" + std::to_string(hasActivePreparation ? 1u : 0u),
                &owner,
                candidates.size());
            continue;
        }

        // A complete package must invalidate the in-memory provisional result;
        // retaining it is what makes the UI jump back to the placeholder or
        // keep presenting the partial frame after the scene is ready.
        if (hasPartialStablePresentation)
        {
            for (auto stable = m_stableThumbnailResultsByCacheKey.begin();
                 stable != m_stableThumbnailResultsByCacheKey.end();)
            {
                if (stable->second.presentationKey == BuildAssetThumbnailPresentationKey(refreshed) &&
                    stable->second.residentPreviewPartial)
                {
                    stable = m_stableThumbnailResultsByCacheKey.erase(stable);
                }
                else
                {
                    ++stable;
                }
            }
        }

        m_gpuPreviewResourcePendingRequestsByCacheKey.insert_or_assign(cacheKey, refreshed);
        TrackGpuPreviewResourceRequestStart(cacheKey, refreshed);
        m_gpuPreviewResidentPartialRevisionByCacheKey.erase(cacheKey);
        m_gpuPreviewResourcePendingDeferredCacheKeys.erase(cacheKey);
        const bool readyMarkerInserted =
            TryPublishResidentCompletionPromotion(cacheKey, refreshed);
        if (readyMarkerInserted)
        {
            ClearVisibleThumbnailRequestStart(refreshed);
            TrackVisibleThumbnailRequestStart(refreshed);
            TrackGpuPreviewResourceRequestStart(cacheKey, refreshed);
        }
        auto& promotedState = m_thumbnailStatesByCacheKey[cacheKey];
        promotedState = ThumbnailState::WaitingForResources;

        if (const auto resolved = m_resolvedPreviewRequestsByCacheKey.find(cacheKey);
            resolved != m_resolvedPreviewRequestsByCacheKey.end())
        {
            resolved->second = refreshed;
        }

        if (queued == m_queuedRequestsByCacheKey.end())
        {
            if (!EnsureQueuedRequestCapacityFor(cacheKey, refreshed))
                continue;
            m_queuedRequestsByCacheKey.emplace(cacheKey, refreshed);
        }
        else
        {
            queued->second = refreshed;
        }
        RemoveQueuedCacheKeyOccurrences(cacheKey);
        EnqueueQueuedCacheKey(cacheKey, refreshed, true);
        RecordThumbnailGpuPreviewQueueDecisionTelemetry(
            "promote-complete-resident-owner",
            &refreshed,
            m_queuedRequestsByCacheKey.size());
    }
}

bool AssetThumbnailService::TryPublishResidentCompletionPromotion(
    const std::string& cacheKey,
    const AssetThumbnailRequest& request)
{
    if (cacheKey.empty() || request.residentPreviewRevision == 0u ||
        request.residentPreviewPartial)
    {
        return false;
    }

    const auto presentationKey = BuildAssetThumbnailPresentationKey(request);
    if (presentationKey.empty())
        return false;

    auto [revision, inserted] =
        m_promotedResidentRevisionByPresentationKey.try_emplace(
            presentationKey,
            request.residentPreviewRevision);
    if (!inserted && revision->second >= request.residentPreviewRevision)
        return false;

    revision->second = request.residentPreviewRevision;
    m_gpuPreviewReadyResidentCacheKeys.insert(cacheKey);
    return true;
}

void AssetThumbnailService::TrackGpuPreviewResourceRequestStart(
    const std::string& cacheKey,
    const AssetThumbnailRequest& request)
{
    if (cacheKey.empty() || !SupportsGpuThumbnailPreview(request))
        return;

    MarkVisibleThumbnailRequestWorkStarted(request);
    const auto presentationKey = BuildAssetThumbnailPresentationKey(request);
    const auto now = std::chrono::steady_clock::now();
    m_gpuPreviewResourceRequestStartedAtByCacheKey.try_emplace(cacheKey, now);
    m_gpuPreviewResourcePresentationKeyByCacheKey[cacheKey] = presentationKey;
    if (!presentationKey.empty())
        m_gpuPreviewResourceRequestStartedAtByPresentationKey.try_emplace(
            presentationKey,
            now);
}

bool AssetThumbnailService::SuspendResidentGpuPreviewResourceDeadlineForSceneLoad(
    const std::string& cacheKey,
    const AssetThumbnailRequest& request,
    const std::chrono::steady_clock::time_point now)
{
    const bool residentRequest = request.residentPrefabPreviewSource.has_value() &&
        request.residentPrefabPreviewSource->HasIdentity();
    if (!residentRequest)
    {
        return false;
    }

    const bool sceneLoadActive =
        NLS::Editor::Core::HasActiveSceneLoadRendererResourceResolution();
    const auto presentationIterator = m_gpuPreviewResourcePresentationKeyByCacheKey.find(cacheKey);
    const std::string presentationKey = presentationIterator !=
        m_gpuPreviewResourcePresentationKeyByCacheKey.end()
        ? presentationIterator->second
        : BuildAssetThumbnailPresentationKey(request);

    if (sceneLoadActive)
    {
        TrackGpuPreviewResourceRequestStart(cacheKey, request);
        if (std::find(
                m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.begin(),
                m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.end(),
                cacheKey) == m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.end())
        {
            m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.push_back(cacheKey);
        }
        return true;
    }

    if (std::find(
            m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.begin(),
            m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.end(),
            cacheKey) == m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.end())
    {
        return false;
    }

    // Restart the full continuation timeout when the scene resolver finishes.
    // Updating existing entries does not invalidate the presentation-key map
    // iterator used to identify aliases.
    for (const auto& [aliasCacheKey, aliasPresentationKey] :
         m_gpuPreviewResourcePresentationKeyByCacheKey)
    {
        if (aliasCacheKey == cacheKey ||
            (!presentationKey.empty() && aliasPresentationKey == presentationKey))
        {
            m_gpuPreviewResourceRequestStartedAtByCacheKey[aliasCacheKey] = now;
            const auto suspended = std::find(
                m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.begin(),
                m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.end(),
                aliasCacheKey);
            if (suspended != m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.end())
                m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.erase(suspended);
            if (const auto deferral = m_gpuPreviewResourcePendingDeferralsByCacheKey.find(aliasCacheKey);
                deferral != m_gpuPreviewResourcePendingDeferralsByCacheKey.end())
            {
                deferral->second.retryCount = 0u;
                deferral->second.firstDeferredAt = now;
                deferral->second.lastProgressAt = now;
            }
        }
    }
    if (m_gpuPreviewResourcePresentationKeyByCacheKey.find(cacheKey) ==
        m_gpuPreviewResourcePresentationKeyByCacheKey.end())
    {
        m_gpuPreviewResourceRequestStartedAtByCacheKey[cacheKey] = now;
        const auto suspended = std::find(
            m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.begin(),
            m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.end(),
            cacheKey);
        if (suspended != m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.end())
            m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.erase(suspended);
    }
    if (!presentationKey.empty())
        m_gpuPreviewResourceRequestStartedAtByPresentationKey[presentationKey] = now;
    return false;
}

void AssetThumbnailService::ClearGpuPreviewResourceRequestStart(
    const std::string& cacheKey)
{
    m_gpuPreviewResourceRequestStartedAtByCacheKey.erase(cacheKey);
    const auto suspended = std::find(
        m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.begin(),
        m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.end(),
        cacheKey);
    if (suspended != m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.end())
        m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.erase(suspended);

    auto presentationKey = m_gpuPreviewResourcePresentationKeyByCacheKey.find(cacheKey);
    if (presentationKey != m_gpuPreviewResourcePresentationKeyByCacheKey.end())
    {
        bool hasReplacementOwner = false;
        for (const auto& [otherCacheKey, otherPresentationKey] :
             m_gpuPreviewResourcePresentationKeyByCacheKey)
        {
            if (otherCacheKey != cacheKey && otherPresentationKey == presentationKey->second &&
                m_gpuPreviewResourceRequestStartedAtByCacheKey.find(otherCacheKey) !=
                    m_gpuPreviewResourceRequestStartedAtByCacheKey.end())
            {
                hasReplacementOwner = true;
                break;
            }
        }
        if (!hasReplacementOwner)
            m_gpuPreviewResourceRequestStartedAtByPresentationKey.erase(presentationKey->second);
        m_gpuPreviewResourcePresentationKeyByCacheKey.erase(presentationKey);
        return;
    }

    // Requests created before the stable index was introduced can still reach
    // this cleanup path. Derive the identity from any live owner before it is
    // removed; this keeps old and new queue entries interoperable.
    const auto findPresentationKey = [&cacheKey](const auto& requests)
        -> std::optional<std::string>
    {
        const auto found = requests.find(cacheKey);
        if (found == requests.end())
            return std::nullopt;
        return BuildAssetThumbnailPresentationKey(found->second);
    };
    std::optional<std::string> derived;
    if (!(derived = findPresentationKey(m_gpuPreviewResourcePendingRequestsByCacheKey)).has_value())
        if (!(derived = findPresentationKey(m_queuedRequestsByCacheKey)).has_value())
            derived = findPresentationKey(m_resolvedPreviewRequestsByCacheKey);
    if (derived.has_value() && !derived->empty())
        m_gpuPreviewResourceRequestStartedAtByPresentationKey.erase(*derived);
}

bool AssetThumbnailService::HasGpuPreviewResourceContinuation(const std::string& cacheKey) const
{
    return m_gpuPreviewResourcePendingRequestsByCacheKey.find(cacheKey) !=
            m_gpuPreviewResourcePendingRequestsByCacheKey.end() ||
        m_gpuPreviewResourcePendingDeferralsByCacheKey.find(cacheKey) !=
            m_gpuPreviewResourcePendingDeferralsByCacheKey.end();
}

bool AssetThumbnailService::IsGpuPreviewResourceContinuationState(
    const std::string& cacheKey,
    const ThumbnailState state) const
{
    // WaitingForResources is itself an ownership invariant. The pending table
    // is normally populated alongside this state, but a request can briefly
    // cross a queue/generation bookkeeping boundary with the table missing.
    // Treating that state as ordinary heavy work makes the scheduler reject
    // the only work that can make the request progress.
    if (state == ThumbnailState::WaitingForResources)
        return true;

    // A visible GPU preview starts its resource deadline when it enters the
    // ordinary queue. It may still be Queued when the scheduler has not yet
    // rebuilt the auxiliary pending-ownership table, so use the authoritative
    // request and start-time tables for this transient bookkeeping state.
    if (state == ThumbnailState::Queued)
    {
        const auto queuedRequest = m_queuedRequestsByCacheKey.find(cacheKey);
        return queuedRequest != m_queuedRequestsByCacheKey.end() &&
            queuedRequest->second.priority == ThumbnailRequestPriority::Visible &&
            SupportsGpuThumbnailPreview(queuedRequest->second) &&
            m_gpuPreviewResourceRequestStartedAtByCacheKey.find(cacheKey) !=
                m_gpuPreviewResourceRequestStartedAtByCacheKey.end();
    }

    if (!HasGpuPreviewResourceContinuation(cacheKey))
        return false;

    // The continuation owns the deadline. Queue bookkeeping can temporarily
    // move the request out of WaitingForResources while it remains a resource
    // continuation, but an active readback must be left to its renderer slot.
    return state == ThumbnailState::Queued ||
        state == ThumbnailState::Preparing ||
        state == ThumbnailState::WaitingForResources ||
        state == ThumbnailState::Rendering;
}

void AssetThumbnailService::ClearGpuPreviewResourcePending(const std::string& cacheKey)
{
    if (m_gpuPreviewSceneAssemblyContinuationCacheKey == cacheKey)
        m_gpuPreviewSceneAssemblyContinuationCacheKey.clear();

    // Resource cleanup is also used by transient pending/error paths. Do not
    // discard a completion promotion marker while the current resident package
    // is complete and the request is still waiting to render it. Canonical
    // Ready/Failed/Cancelled states intentionally clear the marker below.
    bool preserveReadyResidentMarker = false;
    const auto state = m_thumbnailStatesByCacheKey.find(cacheKey);
    const bool terminalOrCanonicalState = state != m_thumbnailStatesByCacheKey.end() &&
        (state->second == ThumbnailState::Ready ||
            state->second == ThumbnailState::Failed ||
            state->second == ThumbnailState::Cancelled);
    if (!terminalOrCanonicalState &&
        m_gpuPreviewReadyResidentCacheKeys.find(cacheKey) !=
            m_gpuPreviewReadyResidentCacheKeys.end())
    {
        const auto isCompleteOwner = [&cacheKey](const AssetThumbnailRequest& request)
        {
            return BuildAssetThumbnailCacheKey(request) == cacheKey &&
                IsCompleteResidentThumbnailRequest(request);
        };
        const auto hasCompleteOwner = [&isCompleteOwner](const auto& owners)
        {
            return std::any_of(
                owners.begin(),
                owners.end(),
                [&isCompleteOwner](const auto& entry)
                {
                    return isCompleteOwner(entry.second);
                });
        };
        preserveReadyResidentMarker =
            hasCompleteOwner(m_gpuPreviewResourcePendingRequestsByCacheKey) ||
            hasCompleteOwner(m_queuedRequestsByCacheKey) ||
            hasCompleteOwner(m_resolvedPreviewRequestsByCacheKey);
        if (!preserveReadyResidentMarker)
        {
            const auto presentation = m_gpuPreviewResourcePresentationKeyByCacheKey.find(cacheKey);
            if (presentation != m_gpuPreviewResourcePresentationKeyByCacheKey.end())
            {
                const auto residentOwner = m_residentPreviewRequestsByPresentationKey.find(
                    presentation->second);
                preserveReadyResidentMarker = residentOwner !=
                    m_residentPreviewRequestsByPresentationKey.end() &&
                    IsCompleteResidentThumbnailRequest(residentOwner->second);
            }
        }
    }
    m_gpuPreviewResourcePendingDeferredCacheKeys.erase(cacheKey);
    m_gpuPreviewResourcePendingDeferralsByCacheKey.erase(cacheKey);
    m_gpuPreviewResourcePendingRequestsByCacheKey.erase(cacheKey);
    m_gpuPreviewResidentPartialRevisionByCacheKey.erase(cacheKey);
    if (!preserveReadyResidentMarker &&
        m_gpuPreviewReadyResidentCacheKeys.find(cacheKey) !=
        m_gpuPreviewReadyResidentCacheKeys.end())
    {
        RecordThumbnailGpuPreviewQueueDecisionTelemetry(
            "ready-resident-marker-cleared=resource-pending",
            nullptr,
            m_queuedRequestsByCacheKey.size());
    }
    if (!preserveReadyResidentMarker)
        m_gpuPreviewReadyResidentCacheKeys.erase(cacheKey);
    ClearGpuPreviewResourceRequestStart(cacheKey);
}

bool AssetThumbnailService::IsGpuPreviewEmptyFrameRetryReady(
    const GpuPreviewEmptyFrameDeferral& deferral) const
{
    return deferral.lastDeferredAt != std::chrono::steady_clock::time_point {} &&
        std::chrono::steady_clock::now() - deferral.lastDeferredAt >=
            kGpuPreviewEmptyFrameRetryDelay;
}

void AssetThumbnailService::RequeueReadyGpuPreviewEmptyFrameRetries()
{
    for (auto iterator = m_gpuPreviewEmptyFrameDeferralsByCacheKey.begin();
        iterator != m_gpuPreviewEmptyFrameDeferralsByCacheKey.end();)
    {
        if (!IsGpuPreviewEmptyFrameRetryReady(iterator->second))
        {
            ++iterator;
            continue;
        }

        const auto cacheKey = iterator->first;
        const auto request = iterator->second.request;
        if (!EnsureQueuedRequestCapacityFor(cacheKey, request))
        {
            ++iterator;
            continue;
        }

        m_queuedRequestsByCacheKey[cacheKey] = request;
        m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(cacheKey);
        m_thumbnailStatesByCacheKey[cacheKey] = ThumbnailState::Queued;
        iterator = m_gpuPreviewEmptyFrameDeferralsByCacheKey.erase(iterator);
        EnqueueQueuedCacheKey(cacheKey, request, true);
        RecordThumbnailGpuPreviewQueueDecisionTelemetry(
            "requeue-empty-frame-retry",
            &request,
            m_queuedRequestsByCacheKey.size());
    }
}

bool AssetThumbnailService::HasQueuedGpuPreviewReadback() const
{
    for (const auto& cacheKey : m_gpuPreviewReadbackPendingCacheKeys)
    {
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(cacheKey);
        if (stateIterator != m_thumbnailStatesByCacheKey.end() &&
            stateIterator->second == ThumbnailState::WaitingForGpu)
        {
            return true;
        }
    }
    for (const auto& [cacheKey, request] : m_queuedRequestsByCacheKey)
    {
        (void)request;
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(cacheKey);
        if (stateIterator != m_thumbnailStatesByCacheKey.end() &&
            stateIterator->second == ThumbnailState::WaitingForGpu)
        {
            return true;
        }
    }
    return false;
}

bool AssetThumbnailService::HasQueuedGpuPreviewResourceContinuation() const
{
    const auto isContinuationState = [this](
        const std::string& cacheKey,
        const ThumbnailState state)
    {
        return IsGpuPreviewResourceContinuationState(cacheKey, state);
    };

    // The pending-resource table is the authoritative ownership record. The
    // ordinary queue can temporarily omit a continuation while it is being
    // restored or rotated between lanes, so scanning only that queue makes a
    // continuation look like a fresh heavy GPU submission. That is especially
    // harmful after an over-target frame, where new heavy work is deliberately
    // rejected but bounded continuation work is allowed to make progress.
    for (const auto& [cacheKey, request] : m_gpuPreviewResourcePendingRequestsByCacheKey)
    {
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(cacheKey);
        if (stateIterator != m_thumbnailStatesByCacheKey.end() &&
            isContinuationState(cacheKey, stateIterator->second))
        {
            return true;
        }
    }

    for (const auto& [cacheKey, request] : m_queuedRequestsByCacheKey)
    {
        if (!SupportsGpuThumbnailPreview(request))
            continue;
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(cacheKey);
        if (stateIterator != m_thumbnailStatesByCacheKey.end() &&
            isContinuationState(cacheKey, stateIterator->second))
        {
            return true;
        }
    }

    // A resolved artifact request may be the only remaining owner while the
    // ordinary queue is being repaired. WaitingForResources is sufficient
    // evidence here; RestoreGpuPreviewResourceContinuationRequests() rebuilds
    // the pending ownership and lane before the next generation pump.
    for (const auto& [cacheKey, request] : m_resolvedPreviewRequestsByCacheKey)
    {
        if (!SupportsGpuThumbnailPreview(request))
            continue;
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(cacheKey);
        if (stateIterator != m_thumbnailStatesByCacheKey.end() &&
            isContinuationState(cacheKey, stateIterator->second))
        {
            return true;
        }
    }
    return false;
}

bool AssetThumbnailService::HasQueuedGpuPreviewSceneAssemblyContinuation() const
{
    if (m_gpuPreviewSceneAssemblyContinuationCacheKey.empty())
        return false;

    const auto& cacheKey = m_gpuPreviewSceneAssemblyContinuationCacheKey;
    const auto request = m_queuedRequestsByCacheKey.find(cacheKey);
    const auto state = m_thumbnailStatesByCacheKey.find(cacheKey);
    return request != m_queuedRequestsByCacheKey.end() &&
        request->second.kind == AssetThumbnailKind::PrefabPreview &&
        state != m_thumbnailStatesByCacheKey.end() &&
        state->second == ThumbnailState::WaitingForResources;
}

std::string AssetThumbnailService::GetQueuedGpuPreviewResourceContinuationSummary() const
{
    size_t count = 0u;
    std::string first;
    const auto inspect = [this, &count, &first](
        const std::string& cacheKey,
        const AssetThumbnailRequest& request)
    {
        const auto state = m_thumbnailStatesByCacheKey.find(cacheKey);
        if (state == m_thumbnailStatesByCacheKey.end() ||
            !IsGpuPreviewResourceContinuationState(cacheKey, state->second))
        {
            return;
        }
        ++count;
        if (first.empty())
            first = request.sourceAssetPath + "|" + request.subAssetKey;
    };

    for (const auto& [cacheKey, request] : m_gpuPreviewResourcePendingRequestsByCacheKey)
        inspect(cacheKey, request);
    for (const auto& [cacheKey, request] : m_queuedRequestsByCacheKey)
    {
        if (m_gpuPreviewResourcePendingRequestsByCacheKey.find(cacheKey) ==
            m_gpuPreviewResourcePendingRequestsByCacheKey.end())
        {
            inspect(cacheKey, request);
        }
    }
    for (const auto& [cacheKey, request] : m_resolvedPreviewRequestsByCacheKey)
    {
        if (m_gpuPreviewResourcePendingRequestsByCacheKey.find(cacheKey) ==
                m_gpuPreviewResourcePendingRequestsByCacheKey.end() &&
            m_queuedRequestsByCacheKey.find(cacheKey) == m_queuedRequestsByCacheKey.end())
        {
            inspect(cacheKey, request);
        }
    }

    std::string summary = "count=" + std::to_string(count);
    if (!first.empty())
        summary += "|first=" + first;
    return summary;
}

bool AssetThumbnailService::HasQueuedVisibleResidentThumbnail() const
{
    const auto isLiveVisibleResidentRequest = [this](const AssetThumbnailRequest& request)
    {
        return IsVisibleResidentThumbnailRequest(request);
    };
    const auto containsVisibleResidentRequest = [this, &isLiveVisibleResidentRequest](const auto& queue)
    {
        for (const auto& cacheKey : queue)
        {
            const auto requestIterator = m_queuedRequestsByCacheKey.find(cacheKey);
            if (requestIterator == m_queuedRequestsByCacheKey.end())
                continue;
            if (isLiveVisibleResidentRequest(requestIterator->second))
                return true;
        }
        return false;
    };

    // The request map is the source of truth. Lane de-duplication and
    // continuation repair can temporarily lose a deque entry while the
    // request itself remains queued; relying on deques here would make the
    // scheduler incorrectly apply the scene-load gate to resident work.
    for (const auto& [cacheKey, request] : m_queuedRequestsByCacheKey)
    {
        (void)cacheKey;
        if (isLiveVisibleResidentRequest(request))
            return true;
    }

    // The explicit lane is the common path and keeps this query proportional
    // to resident work. The legacy selector has no dedicated lane, so retain a
    // narrow fallback for feature-flag A/B runs with lanes disabled.
    if (containsVisibleResidentRequest(m_queuedVisibleResidentCacheKeys))
        return true;
    if (m_featureConfig.explicitLanes)
        return false;
    return containsVisibleResidentRequest(m_queuedCacheKeys);
}

bool AssetThumbnailService::HasQueuedReadyResidentThumbnail()
{
    // Resource packages may become complete while the previous generation
    // call is still returning a partial frame. Promote the parked continuation
    // before the scheduler samples this signal.
    RestoreGpuPreviewResourceContinuationRequests();
    PromoteCompletedResidentPreviewOwners();

    // A completion promotion is authoritative even while the request is
    // moving between pending/resolved/queued owner tables. The marker is only
    // inserted by Restore/Promote after an exact registry snapshot reported
    // complete, so do not repeat the weak-registry lookup here. Repeating it
    // created a race where the scheduler observed a valid promotion marker but
    // the registry handle was temporarily unavailable and kept the request on
    // the heavy cooldown. PopNextGpuPreviewCacheKey performs the final live
    // completion check before consuming the marker.
    for (const auto& cacheKey : m_gpuPreviewReadyResidentCacheKeys)
    {
        if (const auto queued = m_queuedRequestsByCacheKey.find(cacheKey);
            queued != m_queuedRequestsByCacheKey.end() &&
            queued->second.residentPrefabPreviewSource.has_value() &&
            queued->second.residentPrefabPreviewSource->HasIdentity())
        {
            return true;
        }
        if (const auto pending = m_gpuPreviewResourcePendingRequestsByCacheKey.find(cacheKey);
            pending != m_gpuPreviewResourcePendingRequestsByCacheKey.end() &&
            pending->second.residentPrefabPreviewSource.has_value() &&
            pending->second.residentPrefabPreviewSource->HasIdentity())
        {
            return true;
        }
        if (const auto resolved = m_resolvedPreviewRequestsByCacheKey.find(cacheKey);
            resolved != m_resolvedPreviewRequestsByCacheKey.end() &&
            resolved->second.residentPrefabPreviewSource.has_value() &&
            resolved->second.residentPrefabPreviewSource->HasIdentity())
        {
            return true;
        }
    }

    RecordThumbnailGpuPreviewQueueDecisionTelemetry(
        "resident-ready-scan=empty|markers=" +
            std::to_string(m_gpuPreviewReadyResidentCacheKeys.size()) +
            "|queued=" + std::to_string(m_queuedRequestsByCacheKey.size()) +
            "|pending=" +
            std::to_string(m_gpuPreviewResourcePendingRequestsByCacheKey.size()) +
            "|resolved=" +
            std::to_string(m_resolvedPreviewRequestsByCacheKey.size()),
        nullptr,
        m_queuedRequestsByCacheKey.size());
    return false;
}

bool AssetThumbnailService::HasQueuedNonGpuThumbnailWork() const
{
    for (const auto& [cacheKey, request] : m_queuedRequestsByCacheKey)
    {
        if (!SupportsGpuThumbnailPreview(request))
            return true;
    }
    return false;
}

bool AssetThumbnailService::HasQueuedVisibleTextureThumbnailWork() const
{
    return std::any_of(
        m_queuedRequestsByCacheKey.begin(),
        m_queuedRequestsByCacheKey.end(),
        [](const auto& entry)
        {
            return IsVisibleTextureThumbnailRequest(entry.second);
        });
}

std::optional<std::string> AssetThumbnailService::PopNextNonGpuThumbnailCacheKey()
{
    const auto popFrom = [this](std::deque<std::string>& queue)
        -> std::optional<std::string>
    {
        for (auto iterator = queue.begin(); iterator != queue.end(); ++iterator)
        {
            const auto requestIterator = m_queuedRequestsByCacheKey.find(*iterator);
            if (requestIterator == m_queuedRequestsByCacheKey.end() ||
                SupportsGpuThumbnailPreview(requestIterator->second))
            {
                continue;
            }

            const auto cacheKey = *iterator;
            queue.erase(iterator);
            // A priority promotion can leave stale membership in another lane.
            // Remove all copies before handing the request to the CPU path.
            RemoveQueuedCacheKeyOccurrences(cacheKey);
            m_priorityThumbnailDequeueStreak = 0u;
            return cacheKey;
        }
        return std::nullopt;
    };

    if (!m_featureConfig.explicitLanes)
        return popFrom(m_queuedCacheKeys);

    if (auto cacheKey = popFrom(m_queuedVisibleResidentCacheKeys); cacheKey.has_value())
        return cacheKey;
    if (auto cacheKey = popFrom(m_queuedVisibleCacheKeys); cacheKey.has_value())
        return cacheKey;
    if (auto cacheKey = popFrom(m_queuedInspectorCacheKeys); cacheKey.has_value())
        return cacheKey;
    if (auto cacheKey = popFrom(m_queuedPrefetchCacheKeys); cacheKey.has_value())
        return cacheKey;
    if (auto cacheKey = popFrom(m_queuedPriorityCacheKeys); cacheKey.has_value())
        return cacheKey;
    return popFrom(m_queuedCacheKeys);
}

bool AssetThumbnailService::EnsureQueuedRequestCapacityFor(
    const std::string& cacheKey,
    const AssetThumbnailRequest& request)
{
    if (m_queuedRequestsByCacheKey.find(cacheKey) != m_queuedRequestsByCacheKey.end())
        return true;
    if (m_queuedRequestsByCacheKey.size() < kMaxQueuedThumbnailRequests)
        return true;

    const auto rank = ThumbnailRequestPriorityRank(request.priority);
    const uint32_t maxEvictableRank = rank >= ThumbnailRequestPriorityRank(ThumbnailRequestPriority::Inspector)
        ? ThumbnailRequestPriorityRank(ThumbnailRequestPriority::Prefetch)
        : rank;
    return DropQueuedRequestForBackpressure(cacheKey, maxEvictableRank);
}

bool AssetThumbnailService::DropQueuedRequestForBackpressure(
    const std::string& protectedCacheKey,
    const uint32_t maxPriorityRank)
{
    std::optional<std::string> victim;
    uint32_t victimRank = ThumbnailRequestPriorityRank(ThumbnailRequestPriority::Visible) + 1u;
    for (const auto& [cacheKey, request] : m_queuedRequestsByCacheKey)
    {
        if (cacheKey == protectedCacheKey)
            continue;
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(cacheKey);
        if (stateIterator != m_thumbnailStatesByCacheKey.end() &&
            IsActiveThumbnailReadbackOrPersistenceState(stateIterator->second))
        {
            continue;
        }
        const auto rank = ThumbnailRequestPriorityRank(request.priority);
        if (rank > maxPriorityRank)
            continue;
        if (!victim.has_value() || rank < victimRank)
        {
            victim = cacheKey;
            victimRank = rank;
            if (rank == ThumbnailRequestPriorityRank(ThumbnailRequestPriority::Background))
                break;
        }
    }

    if (!victim.has_value())
        return false;

    RemoveQueuedCacheKeyOccurrences(*victim);
    m_queuedRequestsByCacheKey.erase(*victim);
    m_queuedThumbnailLaneByCacheKey.erase(*victim);
    m_resolvedPreviewRequestsByCacheKey.erase(*victim);
    m_gpuDeferredHeavyPreviewCacheKeys.erase(*victim);
    m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(*victim);
    m_gpuPreviewEmptyFrameDeferralsByCacheKey.erase(*victim);
    ClearGpuPreviewResourcePending(*victim);
    ClearGpuPreviewReadbackPending(*victim);
    m_thumbnailStatesByCacheKey[*victim] = ThumbnailState::Cancelled;
    return true;
}

bool AssetThumbnailService::HasDeferredGpuPreviewEmptyFrame(const std::string& cacheKey) const
{
    return m_gpuPreviewEmptyFrameDeferredCacheKeys.find(cacheKey) !=
        m_gpuPreviewEmptyFrameDeferredCacheKeys.end();
}

bool AssetThumbnailService::ObserveLatestPresentationRevision(
    const AssetThumbnailRequest& request)
{
    const auto presentationKey = BuildAssetThumbnailPresentationKey(request);
    if (presentationKey.empty() || request.requestRevision == 0u)
        return true;

    const auto found = m_latestPresentationRevisions.find(presentationKey);
    if (found != m_latestPresentationRevisions.end())
    {
        if (request.requestRevision < found->second)
            return false;
        if (request.requestRevision == found->second)
            return true;
    }

    m_latestPresentationRevisions[presentationKey] = request.requestRevision;
    SupersedeOlderPresentationRequests(presentationKey, request.requestRevision);
    return true;
}

bool AssetThumbnailService::IsPresentationRevisionSuperseded(
    const AssetThumbnailRequest& request) const
{
    const auto presentationKey = BuildAssetThumbnailPresentationKey(request);
    const auto found = m_latestPresentationRevisions.find(presentationKey);
    return found != m_latestPresentationRevisions.end() &&
        request.requestRevision != 0u &&
        request.requestRevision < found->second;
}

void AssetThumbnailService::SupersedeOlderPresentationRequests(
    const std::string& presentationKey,
    const uint64_t requestRevision)
{
    if (presentationKey.empty() || requestRevision == 0u)
        return;

    if (const auto terminal = m_terminalPresentationRevisions.find(presentationKey);
        terminal != m_terminalPresentationRevisions.end() &&
        requestRevision > terminal->second)
    {
        m_terminalPresentationRevisions.erase(terminal);
    }
    if (const auto deadline = m_visibleThumbnailRequestDeadlinesByPresentationKey.find(
            presentationKey);
        deadline != m_visibleThumbnailRequestDeadlinesByPresentationKey.end() &&
        deadline->second.request.requestRevision < requestRevision)
    {
        m_visibleThumbnailRequestDeadlinesByPresentationKey.erase(deadline);
    }

    std::vector<std::string> supersededCacheKeys;
    supersededCacheKeys.reserve(m_queuedRequestsByCacheKey.size());
    for (const auto& [cacheKey, request] : m_queuedRequestsByCacheKey)
    {
        if (request.requestRevision < requestRevision &&
            BuildAssetThumbnailPresentationKey(request) == presentationKey)
        {
            supersededCacheKeys.push_back(cacheKey);
        }
    }
    for (const auto& [cacheKey, request] : m_gpuPreviewResourcePendingRequestsByCacheKey)
    {
        if (request.requestRevision < requestRevision &&
            BuildAssetThumbnailPresentationKey(request) == presentationKey &&
            std::find(supersededCacheKeys.begin(), supersededCacheKeys.end(), cacheKey) ==
                supersededCacheKeys.end())
        {
            supersededCacheKeys.push_back(cacheKey);
        }
    }

    for (const auto& cacheKey : supersededCacheKeys)
    {
        const bool hasPendingReadback =
            m_gpuPreviewReadbackPendingCacheKeys.find(cacheKey) !=
                m_gpuPreviewReadbackPendingCacheKeys.end();
        RemoveQueuedCacheKeyOccurrences(cacheKey);
        m_queuedRequestsByCacheKey.erase(cacheKey);
        m_queuedThumbnailLaneByCacheKey.erase(cacheKey);
        m_resolvedPreviewRequestsByCacheKey.erase(cacheKey);
        m_gpuDeferredHeavyPreviewCacheKeys.erase(cacheKey);
        m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(cacheKey);
        m_gpuPreviewEmptyFrameDeferralsByCacheKey.erase(cacheKey);
        ClearGpuPreviewResourcePending(cacheKey);
        m_completedGpuPreviewResultsByCacheKey.erase(cacheKey);
        if (!hasPendingReadback)
            ClearGpuPreviewReadbackPending(cacheKey);
        m_thumbnailStatesByCacheKey[cacheKey] = hasPendingReadback
            ? ThumbnailState::WaitingForGpu
            : ThumbnailState::Cancelled;
    }

    for (auto iterator = m_terminalThumbnailResultsByCacheKey.begin();
         iterator != m_terminalThumbnailResultsByCacheKey.end();)
    {
        if (iterator->second.presentationKey == presentationKey &&
            iterator->second.requestRevision < requestRevision)
        {
            iterator = m_terminalThumbnailResultsByCacheKey.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }

    for (auto& inFlight : m_inFlightThumbnails)
    {
        if (inFlight.request.requestRevision >= requestRevision ||
            BuildAssetThumbnailPresentationKey(inFlight.request) != presentationKey)
        {
            continue;
        }
        if (inFlight.cancelToken)
            inFlight.cancelToken->cancelled.store(true, std::memory_order_relaxed);
        m_completedGpuPreviewResultsByCacheKey.erase(inFlight.cacheKey);
        m_thumbnailStatesByCacheKey[inFlight.cacheKey] = ThumbnailState::Cancelled;
    }

    for (auto& ticket : m_deferredPersistenceTickets)
    {
        if (ticket.request.requestRevision >= requestRevision ||
            BuildAssetThumbnailPresentationKey(ticket.request) != presentationKey)
        {
            continue;
        }
        if (ticket.cancelToken)
            ticket.cancelToken->cancelled.store(true, std::memory_order_relaxed);
        m_completedGpuPreviewResultsByCacheKey.erase(ticket.cacheKey);
        m_thumbnailStatesByCacheKey[ticket.cacheKey] = ThumbnailState::Cancelled;
    }
}

void AssetThumbnailService::EnqueueQueuedCacheKey(
    const std::string& cacheKey,
    const AssetThumbnailRequest& request,
    const bool atFront)
{
    m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(cacheKey);
    // A request selected by the scheduler has already left its deque. In
    // that normal case the membership index is empty and there is nothing to
    // scan. Only indexed entries need removal before a lane promotion.
    if (m_queuedThumbnailLaneByCacheKey.find(cacheKey) !=
        m_queuedThumbnailLaneByCacheKey.end())
    {
        RemoveQueuedCacheKeyOccurrences(cacheKey);
    }
    const auto enqueue = [this, atFront, &cacheKey](
        auto& queue,
        const QueuedThumbnailLane lane)
    {
        if (atFront)
            queue.push_front(cacheKey);
        else
            queue.push_back(cacheKey);
        m_queuedThumbnailLaneByCacheKey[cacheKey] = lane;
    };
    if (!m_featureConfig.explicitLanes)
    {
        enqueue(m_queuedCacheKeys, QueuedThumbnailLane::Legacy);
        return;
    }
    switch (request.priority)
    {
    case ThumbnailRequestPriority::Visible:
        if (IsVisibleResidentThumbnailRequest(request))
            enqueue(m_queuedVisibleResidentCacheKeys, QueuedThumbnailLane::VisibleResident);
        else
            enqueue(m_queuedVisibleCacheKeys, QueuedThumbnailLane::Visible);
        return;
    case ThumbnailRequestPriority::Inspector:
        enqueue(m_queuedInspectorCacheKeys, QueuedThumbnailLane::Inspector);
        return;
    case ThumbnailRequestPriority::Prefetch:
        enqueue(m_queuedPrefetchCacheKeys, QueuedThumbnailLane::Prefetch);
        return;
    case ThumbnailRequestPriority::Background:
        break;
    }

    if (ShouldPrioritizeThumbnailRequest(request))
        enqueue(m_queuedPriorityCacheKeys, QueuedThumbnailLane::Priority);
    else
        enqueue(m_queuedCacheKeys, QueuedThumbnailLane::Background);
}

void AssetThumbnailService::RepairQueuedRequestLaneMembership()
{
    const auto isQueuedInAnyLane = [this](const std::string& cacheKey)
    {
        return m_queuedThumbnailLaneByCacheKey.find(cacheKey) !=
            m_queuedThumbnailLaneByCacheKey.end();
    };

    std::vector<std::pair<std::string, AssetThumbnailRequest>> missingLaneEntries;
    missingLaneEntries.reserve(m_queuedRequestsByCacheKey.size());
    for (const auto& [cacheKey, request] : m_queuedRequestsByCacheKey)
    {
        if (isQueuedInAnyLane(cacheKey))
            continue;

        const auto state = m_thumbnailStatesByCacheKey.find(cacheKey);
        if (state == m_thumbnailStatesByCacheKey.end() ||
            !IsPendingThumbnailState(state->second))
        {
            continue;
        }
        missingLaneEntries.emplace_back(cacheKey, request);
    }

    for (const auto& [cacheKey, request] : missingLaneEntries)
    {
        // Remove any stale physical copies left by the bookkeeping gap before
        // rebuilding the indexed membership.
        RemoveQueuedCacheKeyOccurrences(cacheKey);
        EnqueueQueuedCacheKey(cacheKey, request);
        RecordThumbnailGpuPreviewQueueDecisionTelemetry(
            "repair-queued-lane-membership",
            &request,
            m_queuedRequestsByCacheKey.size());
    }
}

std::optional<std::string> AssetThumbnailService::PopNextQueuedCacheKey()
{
    if (!m_featureConfig.explicitLanes)
    {
        if (m_queuedCacheKeys.empty())
            return std::nullopt;
        auto cacheKey = m_queuedCacheKeys.front();
        m_queuedCacheKeys.pop_front();
        m_queuedThumbnailLaneByCacheKey.erase(cacheKey);
        return cacheKey;
    }
    const auto pop = [this](std::deque<std::string>& queue, const bool countTowardsPriorityBurst)
        -> std::optional<std::string>
    {
        auto cacheKey = PopNextQueuedCacheKeyFrom(
            queue,
            m_priorityThumbnailDequeueStreak,
            countTowardsPriorityBurst);
        if (cacheKey.has_value())
            m_queuedThumbnailLaneByCacheKey.erase(*cacheKey);
        return cacheKey;
    };
    const auto popNonVisibleLane = [this, &pop]() -> std::optional<std::string>
    {
        if (auto cacheKey = pop(
                m_queuedInspectorCacheKeys,
                true);
            cacheKey.has_value())
        {
            m_priorityThumbnailDequeueStreak = 0u;
            return cacheKey;
        }
        if (auto cacheKey = pop(
                m_queuedPrefetchCacheKeys,
                true);
            cacheKey.has_value())
        {
            m_priorityThumbnailDequeueStreak = 0u;
            return cacheKey;
        }
        if (auto cacheKey = pop(
                m_queuedPriorityCacheKeys,
                true);
            cacheKey.has_value())
        {
            m_priorityThumbnailDequeueStreak = 0u;
            return cacheKey;
        }
        if (auto cacheKey = pop(
                m_queuedCacheKeys,
                false);
            cacheKey.has_value())
        {
            m_priorityThumbnailDequeueStreak = 0u;
            return cacheKey;
        }
        return std::nullopt;
    };

    // Visible work may be continuously replenished while scrolling. After a
    // bounded burst, force one lower-priority lane to advance before taking
    // another visible item. If no lower lane is populated, visible work may
    // continue without an artificial stall.
    if (m_priorityThumbnailDequeueStreak >= kMaxPriorityThumbnailDequeueBurst)
    {
        if (auto cacheKey = popNonVisibleLane(); cacheKey.has_value())
            return cacheKey;
        m_priorityThumbnailDequeueStreak = 0u;
    }

    if (auto cacheKey = pop(
            m_queuedVisibleResidentCacheKeys,
            true);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = pop(
            m_queuedVisibleCacheKeys,
            true);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = pop(
            m_queuedInspectorCacheKeys,
            true);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = pop(
            m_queuedPrefetchCacheKeys,
            true);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = pop(
            m_queuedPriorityCacheKeys,
            true);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = pop(
            m_queuedCacheKeys,
            false);
        cacheKey.has_value())
    {
        m_priorityThumbnailDequeueStreak = 0u;
        return cacheKey;
    }
    return std::nullopt;
}

void AssetThumbnailService::RemoveQueuedCacheKeyOccurrences(const std::string& cacheKey)
{
    auto removeFromQueue = [&cacheKey](std::deque<std::string>& queue)
    {
        queue.erase(
            std::remove(queue.begin(), queue.end(), cacheKey),
            queue.end());
    };

    const auto indexed = m_queuedThumbnailLaneByCacheKey.find(cacheKey);
    if (indexed == m_queuedThumbnailLaneByCacheKey.end())
    {
        // This is the recovery path for a deliberately simulated or otherwise
        // observed bookkeeping gap. Normal queue operations never reach it.
        removeFromQueue(m_queuedVisibleResidentCacheKeys);
        removeFromQueue(m_queuedVisibleCacheKeys);
        removeFromQueue(m_queuedInspectorCacheKeys);
        removeFromQueue(m_queuedPrefetchCacheKeys);
        removeFromQueue(m_queuedPriorityCacheKeys);
        removeFromQueue(m_queuedCacheKeys);
        return;
    }

    switch (indexed->second)
    {
    case QueuedThumbnailLane::Legacy:
    case QueuedThumbnailLane::Background:
        removeFromQueue(m_queuedCacheKeys);
        break;
    case QueuedThumbnailLane::VisibleResident:
        removeFromQueue(m_queuedVisibleResidentCacheKeys);
        break;
    case QueuedThumbnailLane::Visible:
        removeFromQueue(m_queuedVisibleCacheKeys);
        break;
    case QueuedThumbnailLane::Inspector:
        removeFromQueue(m_queuedInspectorCacheKeys);
        break;
    case QueuedThumbnailLane::Prefetch:
        removeFromQueue(m_queuedPrefetchCacheKeys);
        break;
    case QueuedThumbnailLane::Priority:
        removeFromQueue(m_queuedPriorityCacheKeys);
        break;
    }
    m_queuedThumbnailLaneByCacheKey.erase(indexed);
}

void AssetThumbnailService::CancelExpiredOffscreenRequests()
{
    if (m_offscreenSinceByCacheKey.empty())
        return;

    const auto now = std::chrono::steady_clock::now();
    for (auto iterator = m_offscreenSinceByCacheKey.begin();
         iterator != m_offscreenSinceByCacheKey.end();)
    {
        if (now - iterator->second < kThumbnailOffscreenGrace)
        {
            ++iterator;
            continue;
        }

        const auto cacheKey = iterator->first;
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(cacheKey);
        const auto state = stateIterator != m_thumbnailStatesByCacheKey.end()
            ? stateIterator->second
            : ThumbnailState::Missing;
        if (IsActiveThumbnailReadbackOrPersistenceState(state))
        {
            // Submitted GPU work must retire safely; only its later
            // publication/persistence is allowed to be discarded.
            iterator = m_offscreenSinceByCacheKey.erase(iterator);
            continue;
        }

        RemoveQueuedCacheKeyOccurrences(cacheKey);
        m_queuedRequestsByCacheKey.erase(cacheKey);
        m_queuedThumbnailLaneByCacheKey.erase(cacheKey);
        m_resolvedPreviewRequestsByCacheKey.erase(cacheKey);
        m_gpuDeferredHeavyPreviewCacheKeys.erase(cacheKey);
        m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(cacheKey);
        m_gpuPreviewEmptyFrameDeferralsByCacheKey.erase(cacheKey);
        ClearGpuPreviewResourcePending(cacheKey);
        m_thumbnailStatesByCacheKey[cacheKey] = ThumbnailState::Cancelled;

        for (auto& inFlight : m_inFlightThumbnails)
        {
            if (inFlight.cacheKey == cacheKey && inFlight.cancelToken)
                inFlight.cancelToken->cancelled.store(true, std::memory_order_relaxed);
        }
        for (auto& ticket : m_deferredPersistenceTickets)
        {
            if (ticket.cacheKey == cacheKey && ticket.cancelToken)
                ticket.cancelToken->cancelled.store(true, std::memory_order_relaxed);
        }
        iterator = m_offscreenSinceByCacheKey.erase(iterator);
    }
}

std::optional<std::string> AssetThumbnailService::PopNextGpuPreviewCacheKey(
    const bool includeHeavyGpuPreviews,
    const bool supportsAsynchronousReadbackPolling)
{
    PromoteCompletedResidentPreviewOwners();
    auto noteDequeuedCacheKey = [this](const bool countTowardsPriorityBurst)
    {
        if (countTowardsPriorityBurst)
            ++m_priorityThumbnailDequeueStreak;
        else
            m_priorityThumbnailDequeueStreak = 0u;
    };

    if (includeHeavyGpuPreviews &&
        !m_gpuPreviewSceneAssemblyContinuationCacheKey.empty())
    {
        const auto cacheKey = m_gpuPreviewSceneAssemblyContinuationCacheKey;
        const auto request = m_queuedRequestsByCacheKey.find(cacheKey);
        const auto state = m_thumbnailStatesByCacheKey.find(cacheKey);
        if (request != m_queuedRequestsByCacheKey.end() &&
            state != m_thumbnailStatesByCacheKey.end() &&
            state->second == ThumbnailState::WaitingForResources &&
            request->second.kind == AssetThumbnailKind::PrefabPreview)
        {
            // Bypass the ordinary one-turn resident yield only while a real
            // scene assembly is active. CPU/texture work still has its own lane,
            // and this marker is cleared as soon as assembly stops being pending.
            m_gpuPreviewResourcePendingDeferredCacheKeys.erase(cacheKey);
            RemoveQueuedCacheKeyOccurrences(cacheKey);
            noteDequeuedCacheKey(false);
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "dequeue-prefab-scene-assembly-continuation",
                &request->second,
                m_queuedRequestsByCacheKey.size());
            return cacheKey;
        }

        m_gpuPreviewSceneAssemblyContinuationCacheKey.clear();
    }

    auto popPendingReadback = [this, &noteDequeuedCacheKey](
        std::deque<std::string>& queue,
        const bool countTowardsPriorityBurst)
        -> std::optional<std::string>
    {
        for (auto iterator = queue.begin(); iterator != queue.end(); ++iterator)
        {
            const auto requestIterator = m_queuedRequestsByCacheKey.find(*iterator);
            if (requestIterator == m_queuedRequestsByCacheKey.end())
                continue;

            const auto stateIterator = m_thumbnailStatesByCacheKey.find(*iterator);
            if (stateIterator == m_thumbnailStatesByCacheKey.end() ||
                stateIterator->second != ThumbnailState::WaitingForGpu)
            {
                continue;
            }

            // A pending readback is a poll candidate only after the renderer
            // has produced a completed result. If its fence is still pending,
            // let another GPU/resource lane advance instead of repeatedly
            // selecting the same key and creating a head-of-line stall.
            if (m_completedGpuPreviewResultsByCacheKey.find(*iterator) ==
                m_completedGpuPreviewResultsByCacheKey.end())
            {
                continue;
            }

            auto cacheKey = *iterator;
            queue.erase(iterator);
            RemoveQueuedCacheKeyOccurrences(cacheKey);
            noteDequeuedCacheKey(countTowardsPriorityBurst);
            return cacheKey;
        }
        return std::nullopt;
    };

    auto popGpuPreview = [this, includeHeavyGpuPreviews, &noteDequeuedCacheKey](
        std::deque<std::string>& queue,
        const bool countTowardsPriorityBurst)
        -> std::optional<std::string>
    {
        for (auto iterator = queue.begin(); iterator != queue.end(); ++iterator)
        {
            const auto requestIterator = m_queuedRequestsByCacheKey.find(*iterator);
            if (requestIterator == m_queuedRequestsByCacheKey.end())
                continue;

            const auto& request = requestIterator->second;
            const auto stateIterator = m_thumbnailStatesByCacheKey.find(*iterator);
            const bool readyResidentPreview =
                m_gpuPreviewReadyResidentCacheKeys.find(*iterator) !=
                m_gpuPreviewReadyResidentCacheKeys.end();
            if (readyResidentPreview && !includeHeavyGpuPreviews)
            {
                // A completed resident package belongs to the heavy continuation
                // lane. Do not let the light pump consume the one-shot marker and
                // then fall back to the provisional resource path in the same
                // frame.
                RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                    "skip-ready-resident-light-pump",
                    &request,
                    m_queuedRequestsByCacheKey.size());
                continue;
            }
            if (stateIterator != m_thumbnailStatesByCacheKey.end() &&
                stateIterator->second == ThumbnailState::WaitingForResources &&
                m_gpuPreviewResourcePendingDeferredCacheKeys.erase(*iterator) != 0u)
            {
                // The deferred set is a one-shot scheduler-turn yield. It is
                // populated only for truncated resource reports and resident
                // requests, where retrying immediately can monopolize the
                // visible lane. Ordinary resource-pending continuations do
                // not enter this set and continue retrying so their bounded
                // retry policy can reach a terminal result even when alone.
                RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                    "skip-resource-deferred",
                    &request,
                    m_queuedRequestsByCacheKey.size());
                continue;
            }
            if (!SupportsGpuThumbnailPreview(request))
            {
                RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                    "skip-not-gpu-preview",
                    &request,
                    m_queuedRequestsByCacheKey.size());
                continue;
            }
            const bool resourceContinuation = stateIterator != m_thumbnailStatesByCacheKey.end() &&
                stateIterator->second == ThumbnailState::WaitingForResources;
            if (stateIterator != m_thumbnailStatesByCacheKey.end() &&
                stateIterator->second == ThumbnailState::WaitingForGpu &&
                m_completedGpuPreviewResultsByCacheKey.find(*iterator) ==
                    m_completedGpuPreviewResultsByCacheKey.end())
            {
                continue;
            }
            if (!resourceContinuation &&
                !includeHeavyGpuPreviews &&
                IsHeavyGpuThumbnailPreview(request.kind))
            {
                m_gpuDeferredHeavyPreviewCacheKeys.insert(*iterator);
                RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                    "skip-heavy-disabled",
                    &request,
                    m_queuedRequestsByCacheKey.size());
                continue;
            }

            auto cacheKey = *iterator;
            queue.erase(iterator);
            RemoveQueuedCacheKeyOccurrences(cacheKey);
            noteDequeuedCacheKey(countTowardsPriorityBurst);
            return cacheKey;
        }
        return std::nullopt;
    };

    // A resident package that just became complete must be rendered before
    // ordinary heavy work, even when the caller is the light GPU lane. The
    // explicit marker is cleared only when canonical submission starts or the
    // request reaches a terminal state.
    auto popReadyResidentPreview = [this, &noteDequeuedCacheKey]()
        -> std::optional<std::string>
    {
        const auto isResidentOwner = [](const AssetThumbnailRequest& request)
        {
            return request.residentPrefabPreviewSource.has_value() &&
                request.residentPrefabPreviewSource->HasIdentity();
        };

        // Deferred manifest resolution and repeated UI lookups can rotate the
        // physical cache key while retaining one presentation identity. A
        // completion marker belongs to that presentation, not to whichever
        // alias happened to be queued when the registry finished. Resolve the
        // marker's presentation once, then migrate it to the live owner key.
        const auto markerPresentationKey = [this](const std::string& cacheKey)
            -> std::string
        {
            if (const auto found = m_gpuPreviewResourcePresentationKeyByCacheKey.find(cacheKey);
                found != m_gpuPreviewResourcePresentationKeyByCacheKey.end())
            {
                return found->second;
            }
            const auto findExact = [&cacheKey](const auto& owners) -> std::string
            {
                const auto found = owners.find(cacheKey);
                return found == owners.end()
                    ? std::string {}
                    : BuildAssetThumbnailPresentationKey(found->second);
            };
            if (auto presentation = findExact(m_queuedRequestsByCacheKey); !presentation.empty())
                return presentation;
            if (auto presentation = findExact(m_gpuPreviewResourcePendingRequestsByCacheKey);
                !presentation.empty())
            {
                return presentation;
            }
            return findExact(m_resolvedPreviewRequestsByCacheKey);
        };
        const auto findAliasOwner = [this, &isResidentOwner](const std::string& presentationKey)
            -> std::optional<std::string>
        {
            if (presentationKey.empty())
                return std::nullopt;
            const auto find = [&presentationKey, &isResidentOwner](const auto& owners)
                -> std::optional<std::string>
            {
                for (const auto& [cacheKey, request] : owners)
                {
                    if (isResidentOwner(request) &&
                        BuildAssetThumbnailPresentationKey(request) == presentationKey)
                    {
                        return cacheKey;
                    }
                }
                return std::nullopt;
            };
            if (auto alias = find(m_queuedRequestsByCacheKey); alias.has_value())
                return alias;
            if (auto alias = find(m_gpuPreviewResourcePendingRequestsByCacheKey); alias.has_value())
                return alias;
            if (auto alias = find(m_resolvedPreviewRequestsByCacheKey); alias.has_value())
                return alias;
            if (const auto resident = m_residentPreviewRequestsByPresentationKey.find(presentationKey);
                resident != m_residentPreviewRequestsByPresentationKey.end())
            {
                const auto cacheKey = BuildAssetThumbnailCacheKey(resident->second);
                if (!cacheKey.empty())
                    return cacheKey;
            }
            return std::nullopt;
        };

        for (auto iterator = m_gpuPreviewReadyResidentCacheKeys.begin();
             iterator != m_gpuPreviewReadyResidentCacheKeys.end();)
        {
            const auto markerCacheKey = *iterator;
            auto cacheKey = markerCacheKey;
            auto request = m_queuedRequestsByCacheKey.find(cacheKey);
            if (request == m_queuedRequestsByCacheKey.end())
            {
                const auto alias = findAliasOwner(markerPresentationKey(markerCacheKey));
                if (alias.has_value() && *alias != markerCacheKey)
                {
                    m_gpuPreviewReadyResidentCacheKeys.erase(markerCacheKey);
                    m_gpuPreviewReadyResidentCacheKeys.insert(*alias);
                    RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                        "ready-resident-marker-migrated-alias",
                        nullptr,
                        m_queuedRequestsByCacheKey.size());
                    // Insertion may rehash the unordered set. Restart from a
                    // fresh iterator and process the migrated marker normally.
                    iterator = m_gpuPreviewReadyResidentCacheKeys.begin();
                    continue;
                }
            }
            if (request == m_queuedRequestsByCacheKey.end())
            {
                const bool hasOtherOwner =
                    m_gpuPreviewResourcePendingRequestsByCacheKey.find(cacheKey) !=
                        m_gpuPreviewResourcePendingRequestsByCacheKey.end() ||
                    m_resolvedPreviewRequestsByCacheKey.find(cacheKey) !=
                        m_resolvedPreviewRequestsByCacheKey.end();
                if (!hasOtherOwner)
                {
                    RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                        "ready-resident-marker-cleared=no-owner",
                        nullptr,
                        m_queuedRequestsByCacheKey.size());
                    iterator = m_gpuPreviewReadyResidentCacheKeys.erase(iterator);
                }
                else
                    ++iterator;
                continue;
            }
            if (!isResidentOwner(request->second))
            {
                RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                    "ready-resident-marker-cleared=non-resident-owner",
                    &request->second,
                    m_queuedRequestsByCacheKey.size());
                iterator = m_gpuPreviewReadyResidentCacheKeys.erase(iterator);
                continue;
            }

            // Keep the promotion marker until the service is about to submit a
            // real preview frame. Resource preparation can still return a
            // provisional/partial result after this lane selection; consuming
            // the marker here would make the completed resident package fall
            // back behind the ordinary heavy-preview cooldown.
            RemoveQueuedCacheKeyOccurrences(cacheKey);
            noteDequeuedCacheKey(false);
            return cacheKey;
        }
        return std::nullopt;
    };

    for (auto iterator = m_gpuPreviewReadbackPendingCacheKeys.begin();
        iterator != m_gpuPreviewReadbackPendingCacheKeys.end();)
    {
        const auto cacheKey = *iterator;
        auto requestIterator = m_queuedRequestsByCacheKey.find(cacheKey);
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(cacheKey);
        if (stateIterator != m_thumbnailStatesByCacheKey.end() &&
            stateIterator->second == ThumbnailState::WaitingForGpu)
        {
            if (supportsAsynchronousReadbackPolling &&
                m_completedGpuPreviewResultsByCacheKey.find(cacheKey) ==
                    m_completedGpuPreviewResultsByCacheKey.end())
            {
                // Independent pollers already checked this fence at the start
                // of the pump. Keep the key in its ownership set, but do not
                // let an incomplete readback consume the submit lane or block
                // other visible GPU/resource work.
                const auto pendingRequest =
                    m_gpuPreviewReadbackPendingRequestsByCacheKey.find(cacheKey);
                RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                    "skip-readback-fence-pending",
                    pendingRequest != m_gpuPreviewReadbackPendingRequestsByCacheKey.end()
                        ? &pendingRequest->second
                        : nullptr,
                    m_gpuPreviewReadbackPendingCacheKeys.size());
                ++iterator;
                continue;
            }
            if (requestIterator == m_queuedRequestsByCacheKey.end())
            {
                const auto pendingRequestIterator =
                    m_gpuPreviewReadbackPendingRequestsByCacheKey.find(cacheKey);
                if (pendingRequestIterator != m_gpuPreviewReadbackPendingRequestsByCacheKey.end() &&
                    EnsureQueuedRequestCapacityFor(cacheKey, pendingRequestIterator->second))
                {
                    requestIterator = m_queuedRequestsByCacheKey.emplace(
                        cacheKey,
                        pendingRequestIterator->second).first;
                    EnqueueQueuedCacheKey(cacheKey, requestIterator->second);
                    RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                        "restore-readback-pending-request",
                        &requestIterator->second,
                        m_queuedRequestsByCacheKey.size());
                }
            }
            if (requestIterator == m_queuedRequestsByCacheKey.end())
            {
                RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                    "readback-pending-missing-request",
                    nullptr,
                    m_gpuPreviewReadbackPendingCacheKeys.size());
                ++iterator;
                continue;
            }
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "dequeue-readback-pending",
                &requestIterator->second,
                m_gpuPreviewReadbackPendingCacheKeys.size());
            iterator = m_gpuPreviewReadbackPendingCacheKeys.erase(iterator);
            RemoveQueuedCacheKeyOccurrences(cacheKey);
            noteDequeuedCacheKey(false);
            return cacheKey;
        }
        RecordThumbnailGpuPreviewQueueDecisionTelemetry(
            "readback-pending-stale-state",
            nullptr,
            m_gpuPreviewReadbackPendingCacheKeys.size());
        const auto pendingRequestIterator =
            m_gpuPreviewReadbackPendingRequestsByCacheKey.find(cacheKey);
        if (pendingRequestIterator != m_gpuPreviewReadbackPendingRequestsByCacheKey.end())
        {
            const auto requestKey = BuildThumbnailPreviewReadbackRequestKey(
                pendingRequestIterator->second);
            const auto reverse = m_gpuPreviewReadbackCacheKeyByRequestKey.find(requestKey);
            if (reverse != m_gpuPreviewReadbackCacheKeyByRequestKey.end() &&
                reverse->second == cacheKey)
            {
                m_gpuPreviewReadbackCacheKeyByRequestKey.erase(reverse);
            }
            m_gpuPreviewReadbackPendingRequestsByCacheKey.erase(pendingRequestIterator);
        }
        iterator = m_gpuPreviewReadbackPendingCacheKeys.erase(iterator);
    }

    if (includeHeavyGpuPreviews)
    {
        if (auto cacheKey = popReadyResidentPreview(); cacheKey.has_value())
            return cacheKey;
    }

            if (auto cacheKey = popPendingReadback(m_queuedVisibleResidentCacheKeys, false);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = popPendingReadback(m_queuedVisibleCacheKeys, false);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = popPendingReadback(m_queuedInspectorCacheKeys, false);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = popPendingReadback(m_queuedPrefetchCacheKeys, false);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = popPendingReadback(m_queuedPriorityCacheKeys, false);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = popPendingReadback(m_queuedCacheKeys, false);
        cacheKey.has_value())
    {
        return cacheKey;
    }

    const auto popNonVisibleGpuLane = [this, &popGpuPreview]() -> std::optional<std::string>
    {
        if (auto cacheKey = popGpuPreview(m_queuedInspectorCacheKeys, true);
            cacheKey.has_value())
        {
            m_priorityThumbnailDequeueStreak = 0u;
            return cacheKey;
        }
        if (auto cacheKey = popGpuPreview(m_queuedPrefetchCacheKeys, true);
            cacheKey.has_value())
        {
            m_priorityThumbnailDequeueStreak = 0u;
            return cacheKey;
        }
        if (auto cacheKey = popGpuPreview(m_queuedPriorityCacheKeys, true);
            cacheKey.has_value())
        {
            m_priorityThumbnailDequeueStreak = 0u;
            return cacheKey;
        }
        if (auto cacheKey = popGpuPreview(m_queuedCacheKeys, false);
            cacheKey.has_value())
        {
            m_priorityThumbnailDequeueStreak = 0u;
            return cacheKey;
        }
        return std::nullopt;
    };

    if (m_priorityThumbnailDequeueStreak >= kMaxPriorityThumbnailDequeueBurst)
    {
        if (auto cacheKey = popNonVisibleGpuLane(); cacheKey.has_value())
            return cacheKey;
        m_priorityThumbnailDequeueStreak = 0u;
    }

    // The light lane may process ordinary resident continuations, but it must
    // leave a completed-resident promotion marker for the heavy lane. The
    // marker is the handoff that bypasses the normal heavy-preview cooldown.
    if (auto cacheKey = popGpuPreview(
            m_queuedVisibleResidentCacheKeys,
            includeHeavyGpuPreviews);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = popGpuPreview(m_queuedVisibleCacheKeys, true);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = popGpuPreview(m_queuedInspectorCacheKeys, true);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = popGpuPreview(m_queuedPrefetchCacheKeys, true);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = popGpuPreview(m_queuedPriorityCacheKeys, true);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = popGpuPreview(m_queuedCacheKeys, false);
        cacheKey.has_value())
    {
        return cacheKey;
    }

    return std::nullopt;
}

void AssetThumbnailService::RestoreDeferredCacheKeys(std::vector<std::string>& deferredCacheKeys)
{
    for (auto iterator = deferredCacheKeys.rbegin(); iterator != deferredCacheKeys.rend(); ++iterator)
    {
        const auto& deferred = *iterator;
        const auto found = m_queuedRequestsByCacheKey.find(deferred);
        if (found == m_queuedRequestsByCacheKey.end())
            continue;

        EnqueueQueuedCacheKey(deferred, found->second, true);
    }
    deferredCacheKeys.clear();
}

void AssetThumbnailService::ReleaseImportedPrefabThumbnailContinuationOwner(
    const AssetThumbnailRequest& request)
{
    if (!request.importedPrefabThumbnailContinuation)
        return;

    const auto assetId = request.assetId.ToString();
    m_completedImportedPrefabThumbnailContinuationAssetIds.insert(assetId);
    if (assetId == m_activeImportedPrefabThumbnailContinuationAssetId)
        m_activeImportedPrefabThumbnailContinuationAssetId.clear();
}

void AssetThumbnailService::ClearQueuedRequests()
{
    ++m_generationSerial;
    ClearPendingQueuedRequestsWithDiagnostics(false);
}

void AssetThumbnailService::ClearPendingQueuedRequestsWithDiagnostics(
    const bool preserveResourceDeadlines)
{
    const auto queuedRequestCount = m_queuedRequestsByCacheKey.size();
    if (queuedRequestCount == 0u)
    {
        ClearPendingQueuedRequests(preserveResourceDeadlines);
        return;
    }

    NLS::Base::Profiling::PerformanceStageScope totalScope(
        NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
        "TotalThumbnail",
        NLS::Base::Profiling::PerformanceStageThread::Main);
    totalScope.AddCounter("queueBacklog", queuedRequestCount);
    totalScope.AddCounter("cancelledThumbnailRequestCount", queuedRequestCount);
    const auto cancellationBegin = std::chrono::steady_clock::now();
    ClearPendingQueuedRequests(preserveResourceDeadlines);
    const auto cancellationLatency = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - cancellationBegin);
    totalScope.AddCounter("cancellationLatency", static_cast<uint64_t>(cancellationLatency.count()));
}

void AssetThumbnailService::ClearPendingQueuedRequests(
    const bool preserveResourceDeadlines)
{
    m_completedImportedPrefabThumbnailContinuationAssetIds.clear();
    if (!preserveResourceDeadlines)
        m_activeImportedPrefabThumbnailContinuationAssetId.clear();

    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        preservedResourceDeadlines;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        preservedResourcePresentationDeadlines;
    std::unordered_map<std::string, AssetThumbnailRequest> preservedResourceRequests;
    std::unordered_map<std::string, GpuPreviewResourcePendingDeferral>
        preservedResourceDeferrals;
    std::vector<std::string> preservedResourceDeadlinesSuspended;
    std::vector<std::pair<std::string, std::string>> preservedReadyResidentMarkers;
    if (preserveResourceDeadlines)
    {
        for (const auto& [cacheKey, requestStart] :
             m_gpuPreviewResourceRequestStartedAtByCacheKey)
        {
            std::optional<AssetThumbnailRequest> owner;
            if (const auto queued = m_queuedRequestsByCacheKey.find(cacheKey);
                queued != m_queuedRequestsByCacheKey.end())
            {
                owner = queued->second;
            }
            else if (const auto pending = m_gpuPreviewResourcePendingRequestsByCacheKey.find(cacheKey);
                pending != m_gpuPreviewResourcePendingRequestsByCacheKey.end())
            {
                owner = pending->second;
            }
            else if (const auto resolved = m_resolvedPreviewRequestsByCacheKey.find(cacheKey);
                resolved != m_resolvedPreviewRequestsByCacheKey.end())
            {
                owner = resolved->second;
            }

            if (owner.has_value() &&
                owner->priority == ThumbnailRequestPriority::Visible &&
                SupportsGpuThumbnailPreview(*owner))
            {
                preservedResourceDeadlines.emplace(cacheKey, requestStart);
                const auto presentationKey = BuildAssetThumbnailPresentationKey(*owner);
                if (!presentationKey.empty())
                    preservedResourcePresentationDeadlines.emplace(
                        presentationKey,
                        requestStart);
                preservedResourceRequests.emplace(cacheKey, std::move(*owner));
                if (const auto deferral =
                        m_gpuPreviewResourcePendingDeferralsByCacheKey.find(cacheKey);
                    deferral != m_gpuPreviewResourcePendingDeferralsByCacheKey.end())
                {
                    preservedResourceDeferrals.emplace(cacheKey, deferral->second);
                }
                if (std::find(
                        m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.begin(),
                        m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.end(),
                        cacheKey) != m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.end())
                {
                    preservedResourceDeadlinesSuspended.push_back(cacheKey);
                }
            }
        }

        // A completed resident package has a one-shot promotion marker in
        // addition to its resource deadline. Queue rebuilds must preserve that
        // handoff as well; otherwise the next scheduler turn sees only an
        // ordinary WaitingForResources request and applies the heavy-preview
        // cooldown again.
        for (const auto& markerCacheKey : m_gpuPreviewReadyResidentCacheKeys)
        {
            std::optional<AssetThumbnailRequest> owner;
            if (const auto queued = m_queuedRequestsByCacheKey.find(markerCacheKey);
                queued != m_queuedRequestsByCacheKey.end())
            {
                owner = queued->second;
            }
            else if (const auto pending = m_gpuPreviewResourcePendingRequestsByCacheKey.find(markerCacheKey);
                pending != m_gpuPreviewResourcePendingRequestsByCacheKey.end())
            {
                owner = pending->second;
            }
            else if (const auto resolved = m_resolvedPreviewRequestsByCacheKey.find(markerCacheKey);
                resolved != m_resolvedPreviewRequestsByCacheKey.end())
            {
                owner = resolved->second;
            }
            if (!owner.has_value() ||
                !owner->residentPrefabPreviewSource.has_value() ||
                !owner->residentPrefabPreviewSource->HasIdentity())
            {
                continue;
            }
            // A promotion marker can outlive the resource-start index during a
            // queue/lane transition. Keep the owner itself in the preservation
            // set so the marker is not restored to a key with no queue entry.
            if (preservedResourceRequests.find(markerCacheKey) ==
                preservedResourceRequests.end())
            {
                const auto now = std::chrono::steady_clock::now();
                preservedResourceRequests.emplace(markerCacheKey, *owner);
                preservedResourceDeadlines.emplace(markerCacheKey, now);
                const auto presentationKey = BuildAssetThumbnailPresentationKey(*owner);
                if (!presentationKey.empty())
                    preservedResourcePresentationDeadlines.emplace(presentationKey, now);
            }
            preservedReadyResidentMarkers.emplace_back(
                markerCacheKey,
                BuildAssetThumbnailPresentationKey(*owner));
        }
    }

    m_offscreenSinceByCacheKey.clear();
    for (const auto& ticket : m_deferredPersistenceTickets)
        m_thumbnailStatesByCacheKey[ticket.cacheKey] = ThumbnailState::Cancelled;
    m_deferredPersistenceTickets.clear();

    auto isActiveGpuReadbackCacheKey = [this](const std::string& cacheKey)
    {
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(cacheKey);
        return stateIterator != m_thumbnailStatesByCacheKey.end() &&
            IsActiveThumbnailReadbackOrPersistenceState(stateIterator->second);
    };

    std::vector<std::pair<std::string, AssetThumbnailRequest>> preservedGpuReadbackRequests;
    preservedGpuReadbackRequests.reserve(m_queuedRequestsByCacheKey.size());
    for (const auto& [cacheKey, request] : m_queuedRequestsByCacheKey)
    {
        if (isActiveGpuReadbackCacheKey(cacheKey))
            preservedGpuReadbackRequests.emplace_back(cacheKey, request);
    }

    for (const auto& cacheKey : m_gpuPreviewEmptyFrameDeferredCacheKeys)
    {
        if (!isActiveGpuReadbackCacheKey(cacheKey))
            m_thumbnailStatesByCacheKey[cacheKey] = ThumbnailState::Cancelled;
    }
    for (const auto& [cacheKey, request] : m_gpuPreviewResourcePendingRequestsByCacheKey)
    {
        (void)request;
        if (!isActiveGpuReadbackCacheKey(cacheKey))
            m_thumbnailStatesByCacheKey[cacheKey] = ThumbnailState::Cancelled;
    }
    for (const auto& [cacheKey, request] : m_queuedRequestsByCacheKey)
    {
        (void)request;
        if (!isActiveGpuReadbackCacheKey(cacheKey))
            m_thumbnailStatesByCacheKey[cacheKey] = ThumbnailState::Cancelled;
    }
    while (!m_queuedPriorityCacheKeys.empty())
        m_queuedPriorityCacheKeys.pop_front();
    while (!m_queuedVisibleResidentCacheKeys.empty())
        m_queuedVisibleResidentCacheKeys.pop_front();
    while (!m_queuedVisibleCacheKeys.empty())
        m_queuedVisibleCacheKeys.pop_front();
    while (!m_queuedInspectorCacheKeys.empty())
        m_queuedInspectorCacheKeys.pop_front();
    while (!m_queuedPrefetchCacheKeys.empty())
        m_queuedPrefetchCacheKeys.pop_front();
    while (!m_queuedCacheKeys.empty())
        m_queuedCacheKeys.pop_front();
    m_queuedRequestsByCacheKey.clear();
    m_queuedThumbnailLaneByCacheKey.clear();
    m_terminalThumbnailResultsByCacheKey.clear();
    m_terminalPresentationRevisions.clear();
    m_visibleThumbnailRequestDeadlinesByPresentationKey.clear();
    m_previewRequestResolutionFuturesByCacheKey.clear();
    for (auto iterator = m_resolvedPreviewRequestsByCacheKey.begin();
        iterator != m_resolvedPreviewRequestsByCacheKey.end();)
    {
        if (isActiveGpuReadbackCacheKey(iterator->first))
            ++iterator;
        else
            iterator = m_resolvedPreviewRequestsByCacheKey.erase(iterator);
    }
    m_gpuDeferredHeavyPreviewCacheKeys.clear();
    m_gpuPreviewEmptyFrameDeferredCacheKeys.clear();
    m_gpuPreviewResourcePendingDeferredCacheKeys.clear();
    m_gpuPreviewResourcePendingDeferralsByCacheKey.clear();
    m_gpuPreviewResourcePendingRequestsByCacheKey.clear();
    m_gpuPreviewResidentPartialRevisionByCacheKey.clear();
    m_gpuPreviewSceneAssemblyContinuationCacheKey.clear();
    if (!m_gpuPreviewReadyResidentCacheKeys.empty())
    {
        RecordThumbnailGpuPreviewQueueDecisionTelemetry(
            "ready-resident-marker-cleared=queue-rebuild|count=" +
                std::to_string(m_gpuPreviewReadyResidentCacheKeys.size()) +
                "|preserved=" +
                std::to_string(preservedReadyResidentMarkers.size()),
            nullptr,
            m_queuedRequestsByCacheKey.size());
    }
    m_gpuPreviewReadyResidentCacheKeys.clear();
    m_gpuPreviewResourceRequestStartedAtByCacheKey.clear();
    m_gpuPreviewResourceRequestStartedAtByPresentationKey.clear();
    m_gpuPreviewResourcePresentationKeyByCacheKey.clear();
    m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.clear();
    if (preserveResourceDeadlines)
    {
        for (auto& [cacheKey, requestStart] : preservedResourceDeadlines)
        {
            m_gpuPreviewResourceRequestStartedAtByCacheKey.emplace(cacheKey, requestStart);
            if (std::find(
                    preservedResourceDeadlinesSuspended.begin(),
                    preservedResourceDeadlinesSuspended.end(),
                    cacheKey) != preservedResourceDeadlinesSuspended.end())
            {
                m_gpuPreviewResourceDeadlinesSuspendedForSceneLoad.push_back(cacheKey);
            }
            const auto preservedRequest = preservedResourceRequests.find(cacheKey);
            const auto presentationKey = preservedRequest != preservedResourceRequests.end()
                ? BuildAssetThumbnailPresentationKey(preservedRequest->second)
                : std::string {};
            if (!presentationKey.empty())
            {
                m_gpuPreviewResourceRequestStartedAtByPresentationKey.emplace(
                    presentationKey,
                    preservedResourcePresentationDeadlines.count(presentationKey) != 0u
                        ? preservedResourcePresentationDeadlines.at(presentationKey)
                        : requestStart);
                m_gpuPreviewResourcePresentationKeyByCacheKey.emplace(cacheKey, presentationKey);
            }
            // Queue membership is rebuilt by the next UI lookup, but the
            // resource deadline remains active across that gap. Preserve the
            // continuation state so the lookup performs the timeout check
            // before treating the request as a fresh generation.
            m_thumbnailStatesByCacheKey[cacheKey] = ThumbnailState::WaitingForResources;
        }
    }
    m_gpuPreviewEmptyFrameDeferralsByCacheKey.clear();
    for (auto iterator = m_gpuPreviewReadbackPendingRequestsByCacheKey.begin();
        iterator != m_gpuPreviewReadbackPendingRequestsByCacheKey.end();)
    {
        if (isActiveGpuReadbackCacheKey(iterator->first))
            ++iterator;
        else
            iterator = m_gpuPreviewReadbackPendingRequestsByCacheKey.erase(iterator);
    }
    m_gpuPreviewReadbackCacheKeyByRequestKey.clear();
    for (const auto& [cacheKey, request] : m_gpuPreviewReadbackPendingRequestsByCacheKey)
        m_gpuPreviewReadbackCacheKeyByRequestKey[BuildThumbnailPreviewReadbackRequestKey(request)] = cacheKey;
    for (auto iterator = m_completedGpuPreviewResultsByCacheKey.begin();
        iterator != m_completedGpuPreviewResultsByCacheKey.end();)
    {
        const auto state = m_thumbnailStatesByCacheKey.find(iterator->first);
        const bool stillPending = state != m_thumbnailStatesByCacheKey.end() &&
            state->second == ThumbnailState::WaitingForGpu &&
            m_gpuPreviewReadbackPendingRequestsByCacheKey.find(iterator->first) !=
                m_gpuPreviewReadbackPendingRequestsByCacheKey.end();
        if (stillPending)
            ++iterator;
        else
            iterator = m_completedGpuPreviewResultsByCacheKey.erase(iterator);
    }
    m_priorityThumbnailDequeueStreak = 0u;
    for (const auto& [cacheKey, request] : preservedGpuReadbackRequests)
    {
        m_queuedRequestsByCacheKey[cacheKey] = request;
        EnqueueQueuedCacheKey(cacheKey, request);
    }
    for (const auto& [cacheKey, request] : preservedResourceRequests)
    {
        // A resource continuation is an active owner, not merely a deadline.
        // Restore the owner directly so generation-scope rebuilds cannot leave
        // a Pending tile with no scheduler-visible work until a later repair.
        m_gpuPreviewResourcePendingRequestsByCacheKey.emplace(cacheKey, request);
        if (const auto deferral = preservedResourceDeferrals.find(cacheKey);
            deferral != preservedResourceDeferrals.end())
        {
            m_gpuPreviewResourcePendingDeferralsByCacheKey.emplace(
                cacheKey,
                deferral->second);
        }
        if (m_queuedRequestsByCacheKey.find(cacheKey) != m_queuedRequestsByCacheKey.end())
            continue;
        m_queuedRequestsByCacheKey.emplace(cacheKey, request);
        m_thumbnailStatesByCacheKey[cacheKey] = ThumbnailState::WaitingForResources;
        EnqueueQueuedCacheKey(cacheKey, request);
        TrackGpuPreviewResourceRequestStart(cacheKey, request);
    }

    for (const auto& [markerCacheKey, presentationKey] : preservedReadyResidentMarkers)
    {
        if (const auto queued = m_queuedRequestsByCacheKey.find(markerCacheKey);
            queued != m_queuedRequestsByCacheKey.end() &&
            queued->second.residentPrefabPreviewSource.has_value() &&
            queued->second.residentPrefabPreviewSource->HasIdentity())
        {
            m_gpuPreviewReadyResidentCacheKeys.insert(markerCacheKey);
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "ready-resident-marker-restored-queue-rebuild",
                &queued->second,
                m_queuedRequestsByCacheKey.size());
            continue;
        }

        // The resource owner may have rotated to a resolved alias while the
        // queue was rebuilt. Bind the marker to that live alias instead of
        // restoring a key with no physical owner.
        const auto findAlias = [&presentationKey](const auto& owners)
            -> std::optional<std::string>
        {
            for (const auto& [cacheKey, request] : owners)
            {
                if (request.residentPrefabPreviewSource.has_value() &&
                    request.residentPrefabPreviewSource->HasIdentity() &&
                    BuildAssetThumbnailPresentationKey(request) == presentationKey)
                {
                    return cacheKey;
                }
            }
            return std::nullopt;
        };
        if (auto alias = findAlias(m_queuedRequestsByCacheKey); alias.has_value())
        {
            m_gpuPreviewReadyResidentCacheKeys.insert(*alias);
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "ready-resident-marker-restored-queue-rebuild-alias",
                nullptr,
                m_queuedRequestsByCacheKey.size());
        }
        else if (auto alias = findAlias(m_gpuPreviewResourcePendingRequestsByCacheKey); alias.has_value())
        {
            m_gpuPreviewReadyResidentCacheKeys.insert(*alias);
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "ready-resident-marker-restored-queue-rebuild-alias",
                nullptr,
                m_queuedRequestsByCacheKey.size());
        }
        else if (auto alias = findAlias(m_resolvedPreviewRequestsByCacheKey); alias.has_value())
        {
            m_gpuPreviewReadyResidentCacheKeys.insert(*alias);
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "ready-resident-marker-restored-queue-rebuild-alias",
                nullptr,
                m_queuedRequestsByCacheKey.size());
        }
    }
}

void AssetThumbnailService::SupersedeQueuedRequestsForGeneration(
    const std::string& generationFingerprint)
{
    if (m_generationFingerprint == generationFingerprint)
        return;

    m_generationFingerprint = generationFingerprint;
    ++m_generationSerial;
    m_generationCancelToken = std::make_shared<AssetThumbnailGenerationCancelToken>();
    m_generationCancelToken->generation = m_generationSerial;
    ClearPendingQueuedRequestsWithDiagnostics(true);
}

void AssetThumbnailService::WaitForInFlightRequests()
{
    for (auto& [cacheKey, future] : m_previewRequestResolutionFuturesByCacheKey)
    {
        (void)cacheKey;
        if (!future.valid())
            continue;

        try
        {
            (void)future.get();
        }
        catch (...)
        {
        }
    }
    m_previewRequestResolutionFuturesByCacheKey.clear();

    for (auto& request : m_inFlightThumbnails)
    {
        if (request.future.valid())
        {
            try
            {
                (void)request.future.get();
            }
            catch (...)
            {
            }
        }
    }
    m_inFlightThumbnails.clear();
    m_deferredPersistenceTickets.clear();
}

bool AssetThumbnailService::AdoptMatchingInFlightRequest(const std::string& cacheKey)
{
    for (auto& request : m_inFlightThumbnails)
    {
        if (request.cacheKey == cacheKey && request.future.valid() &&
            (!request.cancelToken ||
                !request.cancelToken->cancelled.load(std::memory_order_relaxed)))
        {
            request.generation = m_generationSerial;
            return true;
        }
    }
    for (auto& ticket : m_deferredPersistenceTickets)
    {
        if (ticket.cacheKey != cacheKey)
            continue;
        ticket.generation = m_generationSerial;
        ticket.cancelToken = m_generationCancelToken;
            return true;
    }
    const auto resourcePending = m_gpuPreviewResourcePendingRequestsByCacheKey.find(cacheKey);
    if (resourcePending != m_gpuPreviewResourcePendingRequestsByCacheKey.end() &&
        SupportsGpuThumbnailPreview(resourcePending->second))
    {
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(cacheKey);
        if (stateIterator != m_thumbnailStatesByCacheKey.end() &&
            stateIterator->second == ThumbnailState::WaitingForResources)
        {
            return true;
        }
    }
    return false;
}

}

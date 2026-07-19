#include "Assets/AssetThumbnailService.h"

#include "Assets/AssetBrowserPresentation.h"
#include "Assets/AssetThumbnailPreviewCamera.h"
#include "Assets/AssetMeta.h"
#include "Assets/ArtifactDatabaseManifestUtils.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/EditorThumbnailPreviewRenderer.h"
#include "Assets/EditorAssetPath.h"
#include "Assets/NativeArtifactContainer.h"
#include "Assets/PreviewRenderableSnapshot.h"
#include "Core/ResourceManagement/MeshManager.h"
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
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
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
using AssetThumbnailGenerator = AssetThumbnailServiceResult (*)(const AssetThumbnailRequest&, const AssetThumbnailCancelToken&);

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
    }
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailServiceGpuPreviewQueueDecision,
        std::chrono::microseconds(0),
        count,
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

template <typename Function>
auto ScheduleThumbnailJobFuture(const char* debugName, Function&& function)
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

    NLS::Base::Jobs::BackgroundJobDesc desc {};
    desc.userData = statePtr;
    desc.debugName = debugName;
    desc.function = [](void* userData)
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
    desc.cancelUserData = statePtr;
    desc.cancelFunction = [](void* userData)
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

    const auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
    if (handle.id == 0u)
    {
        std::unique_ptr<JobState> ownedState(statePtr);
        throw std::runtime_error(NLS::Base::Jobs::IsJobSystemInitialized()
            ? "thumbnail background job scheduling rejected"
            : "thumbnail background job scheduling requires initialized JobSystem");
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

AssetThumbnailServiceResult GenerateTextureThumbnail(const AssetThumbnailRequest& request, const AssetThumbnailCancelToken& cancelToken);
AssetThumbnailServiceResult GenerateMaterialThumbnail(const AssetThumbnailRequest& request, const AssetThumbnailCancelToken& cancelToken);
AssetThumbnailServiceResult GenerateModelThumbnail(const AssetThumbnailRequest& request, const AssetThumbnailCancelToken& cancelToken);
AssetThumbnailServiceResult GeneratePrefabThumbnail(const AssetThumbnailRequest& request, const AssetThumbnailCancelToken& cancelToken);
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
constexpr size_t kMaxObsoleteThumbnailGenerationInFlightRequests = 2u;
constexpr size_t kMaxCurrentThumbnailGenerationInFlightRequests = 2u;
constexpr size_t kMaxThumbnailGenerationTotalInFlightSlots =
    kMaxObsoleteThumbnailGenerationInFlightRequests + kMaxCurrentThumbnailGenerationInFlightRequests;
constexpr size_t kMaxPriorityThumbnailDequeueBurst = 4u;
constexpr size_t kMaxQueuedThumbnailRequests = 512u;
constexpr uint64_t kMaxSourceThumbnailImageBytes = 128ull * 1024ull * 1024ull;
constexpr uint64_t kMaxSourceThumbnailPixels = 4096ull * 4096ull;
constexpr uint32_t kMaxTextureThumbnailGenerationSize = 96u;
constexpr uint64_t kMaxStructurePreviewArtifactPayloadBytes = 1024ull * 1024ull;
constexpr uint64_t kMaxThumbnailPreviewNativeArtifactFileBytes = 128ull * 1024ull * 1024ull;
constexpr size_t kMaxDeferredHeavyGpuPreviewScanPerCall = 8u;
constexpr size_t kMaxDeferredLightGpuPreviewScanPerCall = 96u;
constexpr size_t kMaxResolvedHeavyGpuPreviewManifestLookupsPerCall = 1u;
constexpr const char* kSourcePreviewBudgetExceededDiagnostic =
    "thumbnail-source-preview-budget-exceeded";
constexpr const char* kMaterialPreviewBudgetExceededDiagnostic =
    "thumbnail-material-preview-budget-exceeded";
constexpr const char* kPrefabPreviewBudgetExceededDiagnostic =
    "thumbnail-prefab-preview-budget-exceeded";

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
constexpr const char* kUpperObliqueGpuPrefabThumbnailRendererVersion = "asset-browser-thumbnail-renderer:v33";
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
        state == ThumbnailState::Readback;
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

bool ShouldDeferBackgroundCpuThumbnailToPreviewRenderer(const AssetThumbnailKind kind)
{
    return kind == AssetThumbnailKind::MaterialSphere ||
        kind == AssetThumbnailKind::ModelPreview ||
        kind == AssetThumbnailKind::PrefabPreview;
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

bool IsRetryableThumbnailFailureDiagnostic(const std::string& diagnostic)
{
    if (diagnostic == "thumbnail-gpu-preview-empty-frame")
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
    if (MeshPreviewHeaderExceedsCpuLoadBudget(header))
    {
        return NLS::Render::Assets::LoadMeshArtifactPreviewSample(
            path,
            static_cast<uint32_t>(kMaxMeshPreviewLoadedVertices),
            static_cast<uint32_t>(kMaxMeshPreviewLoadedIndices),
            kMaxStructurePreviewArtifactPayloadBytes);
    }

    return NLS::Render::Assets::LoadMeshArtifact(path);
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

std::string ArtifactDatabaseStampCached(
    const std::filesystem::path& projectRoot,
    AssetThumbnailRequestBuildContext* context)
{
    if (context == nullptr)
        return FileStamp(GetProjectArtifactDatabasePath(projectRoot) / "data.mdb");

    if (!context->artifactDatabaseStampCached ||
        context->artifactDatabaseProjectRoot != projectRoot)
    {
        context->artifactDatabaseProjectRoot = projectRoot;
        context->artifactDatabasePath = GetProjectArtifactDatabasePath(projectRoot) / "data.mdb";
        context->artifactDatabaseStamp = FileStamp(context->artifactDatabasePath);
        context->artifactDatabaseStampCached = true;
    }
    return context->artifactDatabaseStamp;
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
    const AssetBrowserItem& item,
    AssetThumbnailRequestBuildContext* context)
{
    const bool usesArtifactDatabase =
        item.kind == AssetBrowserItemKind::GeneratedSubAsset ||
        item.type == AssetBrowserItemType::Model ||
        item.type == AssetBrowserItemType::Prefab ||
        !request.artifactPath.empty();
    if (usesArtifactDatabase)
    {
        request.freshnessInputs.push_back({
            "artifact-db",
            ArtifactDatabaseStampCached(request.projectRoot, context)
        });
    }

    if (request.artifactPath.empty())
        return;

    request.freshnessInputs.push_back({
        "artifact-file",
        FileStampCached(ResolveThumbnailArtifactPath(request), context)
    });
}

bool IsFileFreshnessInputStillCurrent(
    const AssetThumbnailRequest& request,
    const AssetThumbnailFreshnessInput& input)
{
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
    if (input.name == "artifact-db")
        return input.stamp == FileStamp(GetProjectArtifactDatabasePath(request.projectRoot) / "data.mdb");
    return true;
}

bool IsThumbnailRequestStillFresh(const AssetThumbnailRequest& request)
{
    for (const auto& input : request.freshnessInputs)
    {
        if (!IsFileFreshnessInputStillCurrent(request, input))
            return false;
    }
    return true;
}

AssetThumbnailServiceResult BuildStaleThumbnailRequestResult(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation)
{
    AssetThumbnailServiceResult result;
    result.status = AssetThumbnailServiceStatus::Failed;
    result.cacheEntry = evaluation.entry;
    result.fallbackIcon = FallbackIconForKind(request.kind);
    if (evaluation.entry.has_value())
        result.imagePath = evaluation.entry->imagePath;
    result.diagnostic = "thumbnail-request-stale";
    return result;
}

AssetThumbnailServiceResult BuildCancelledThumbnailRequestResult(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCacheEvaluation& evaluation)
{
    AssetThumbnailServiceResult result;
    result.status = AssetThumbnailServiceStatus::Failed;
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
    result.cacheEntry = evaluation.entry;
    result.diagnostic = evaluation.diagnostic;
    result.fallbackIcon = FallbackIconForKind(request.kind);
    if (evaluation.entry.has_value())
        result.imagePath = evaluation.entry->imagePath;
    return result;
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
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    if (!evaluation.entry.has_value())
    {
        result.diagnostic = evaluation.diagnostic.empty()
            ? "thumbnail-cache-path-invalid"
            : evaluation.diagnostic;
        return result;
    }

    if (!IsThumbnailRequestStillFresh(request))
        return BuildStaleThumbnailRequestResult(request, evaluation);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);

    if (thumbnail.pixels.empty() || thumbnail.width == 0u || thumbnail.height == 0u)
    {
        result.diagnostic = emptyDiagnostic;
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
        WriteAssetThumbnailCacheMetadata(cacheMetadataRequest, *evaluation.entry, AssetThumbnailCacheStatus::Failed, result.diagnostic);
        return result;
    }

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
            WriteAssetThumbnailCacheMetadata(cacheMetadataRequest, *evaluation.entry, AssetThumbnailCacheStatus::Failed, result.diagnostic);
            return result;
        }
    }

    if (!IsThumbnailRequestStillFresh(request))
        return BuildStaleThumbnailRequestResult(request, evaluation);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);

    if (!WriteAssetThumbnailCacheMetadata(cacheMetadataRequest, *evaluation.entry, AssetThumbnailCacheStatus::Fresh, {}))
    {
        result.diagnostic = "thumbnail-cache-metadata-write-failed";
        return result;
    }

    result.status = AssetThumbnailServiceStatus::Fresh;
    result.imagePath = evaluation.entry->imagePath;
    result.diagnostic.clear();
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
    return !MeshPreviewHeaderExceedsCpuLoadBudget(header) &&
        NativeArtifactFileExceedsThumbnailPreviewBudget(path);
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

    return LoadArtifactManifestFromProjectArtifactDB(request.projectRoot, request.assetId);
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

std::optional<std::string> ValidateGpuPreviewRequestArtifactPaths(const AssetThumbnailRequest& request)
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
    if (!HasThumbnailFreshnessInput(cacheRequest, "artifact-db"))
    {
        cacheRequest.freshnessInputs.push_back({
            "artifact-db",
            FileStamp(GetProjectArtifactDatabasePath(cacheRequest.projectRoot) / "data.mdb")
        });
    }

    RemoveThumbnailFreshnessInputs(cacheRequest, "artifact-file");
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
    if (!request.artifactPath.empty())
        return request;

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
    const AssetThumbnailCancelToken& cancelToken)
{
    const auto previewRequest = ResolveDeferredThumbnailPreviewRequest(request);
    const auto metadataRequest = BuildResolvedThumbnailCacheRequest(request, previewRequest);
    const auto evaluation = EvaluateAssetThumbnailCache(request);
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    if (!IsThumbnailRequestStillFresh(request))
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
    const std::string& missingDiagnostic,
    const AssetThumbnailCancelToken& cancelToken,
    const AssetThumbnailRequest* metadataRequest = nullptr)
{
    const AssetThumbnailRequest& cacheMetadataRequest =
        metadataRequest != nullptr ? *metadataRequest : request;
    const auto evaluation = EvaluateAssetThumbnailCache(request);
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    if (!IsThumbnailRequestStillFresh(request))
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
    const std::vector<std::filesystem::path>& meshPaths,
    const std::string& missingDiagnostic,
    const AssetThumbnailCancelToken& cancelToken,
    const AssetThumbnailRequest* metadataRequest = nullptr)
{
    const AssetThumbnailRequest& cacheMetadataRequest =
        metadataRequest != nullptr ? *metadataRequest : request;
    const auto evaluation = EvaluateAssetThumbnailCache(request);
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    if (!IsThumbnailRequestStillFresh(request))
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
        const bool wouldExceedBudget =
            !meshes.empty() &&
            (loadedVertices + candidate.header.vertexCount > kMaxMeshPreviewLoadedVertices ||
                loadedIndices + candidate.header.indexCount > kMaxMeshPreviewLoadedIndices);
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
    bool missingMeshDependency = snapshot.expectedDrawItemCount > snapshot.drawItems.size();
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

        const bool wouldExceedBudget =
            !meshes.empty() &&
            (loadedVertices + meshHeader->vertexCount > kMaxMeshPreviewLoadedVertices ||
                loadedIndices + meshHeader->indexCount > kMaxMeshPreviewLoadedIndices);
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
    const AssetThumbnailCancelToken& cancelToken)
{
    const auto previewRequest = ResolveDeferredThumbnailPreviewRequest(request);
    const auto metadataRequest = BuildResolvedThumbnailCacheRequest(request, previewRequest);
    const auto meshPaths = ResolveMeshArtifactPaths(previewRequest);
    if (meshPaths.empty())
        return GenerateMeshBackedThumbnail(request, "thumbnail-model-mesh-artifact-missing", cancelToken, &metadataRequest);
    return GenerateMeshSetThumbnail(request, meshPaths, "thumbnail-model-mesh-artifact-missing", cancelToken, &metadataRequest);
}

AssetThumbnailServiceResult GeneratePrefabThumbnail(
    const AssetThumbnailRequest& request,
    const AssetThumbnailCancelToken& cancelToken)
{
    const auto previewRequest = ResolveDeferredThumbnailPreviewRequest(request);
    const auto metadataRequest = BuildResolvedThumbnailCacheRequest(request, previewRequest);
    const auto evaluation = EvaluateAssetThumbnailCache(request);
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    if (!IsThumbnailRequestStillFresh(request))
        return BuildStaleThumbnailRequestResult(request, evaluation);
    if (IsThumbnailGenerationCancelled(cancelToken))
        return BuildCancelledThumbnailRequestResult(request, evaluation);

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
    const AssetThumbnailCancelToken& cancelToken)
{
    const auto previewRequest = ResolveDeferredThumbnailPreviewRequest(request);
    const auto metadataRequest = BuildResolvedThumbnailCacheRequest(request, previewRequest);
    const auto generationSize = GetTextureThumbnailGenerationSize(request);
    auto evaluation = EvaluateAssetThumbnailCache(request);
    auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Failed);
    if (!IsThumbnailRequestStillFresh(request))
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
            auto sourcePath = ResolveThumbnailSourcePath(previewRequest);
            if (sourcePath.empty() || !IsTextureThumbnailSourceExtension(sourcePath))
            {
                if (auto dependencySourcePath = ResolveTextureSourceDependencyPath(previewRequest);
                    dependencySourcePath.has_value())
                {
                    sourcePath = *dependencySourcePath;
                }
            }
            if (!sourcePath.empty() && IsTextureThumbnailSourceExtension(sourcePath))
            {
                const auto dimensions = ReadImageHeaderDimensions(sourcePath);
                if (FileSizeOrMax(sourcePath) <= kMaxSourceThumbnailImageBytes &&
                    !(dimensions.has_value() && ImageDimensionsExceedPreviewBudget(*dimensions)) &&
                    !(!dimensions.has_value() && IsKnownSourceImageExtension(sourcePath)))
                {
                    NLS::Image sourceImage(sourcePath.string(), false);
                    if (sourceImage.GetData() != nullptr &&
                        sourceImage.GetWidth() > 0 &&
                        sourceImage.GetHeight() > 0 &&
                        sourceImage.GetChannels() > 0)
                    {
                        return WriteThumbnailPngResult(
                            request,
                            evaluation,
                            DownsampleImageToThumbnail(sourceImage, generationSize),
                            "thumbnail-source-downsample-failed",
                            cancelToken,
                            &metadataRequest);
                    }
                }
            }

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

    const auto sourcePath = ResolveThumbnailSourcePath(previewRequest);
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
    const AssetThumbnailCancelToken&)
{
    auto evaluation = EvaluateAssetThumbnailCache(request);
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
        return generator(request, cancelToken);
    }

    if (SupportsGpuThumbnailPreview(request))
    {
        auto result = BuildResultFromEvaluation(request, evaluation, AssetThumbnailServiceStatus::Pending);
        result.diagnostic = UnsupportedDiagnosticForKind(request.kind);
        return result;
    }

    return GenerateUnsupportedPreviewThumbnail(request, cancelToken);
}

AssetThumbnailServiceResult BuildExceptionThumbnailResult(
    const AssetThumbnailRequest& request,
    const std::string& diagnostic)
{
    AssetThumbnailServiceResult result;
    result.status = AssetThumbnailServiceStatus::Failed;
    result.fallbackIcon = FallbackIconForKind(request.kind);
    result.diagnostic = diagnostic;
    try
    {
        const auto evaluation = EvaluateAssetThumbnailCache(request);
        result.cacheEntry = evaluation.entry;
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
        appendPart(
            result,
            "artifactPath",
            std::filesystem::path(request.artifactPath).lexically_normal().generic_string());
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
    request.assetId = assetId;
    request.sourceAssetPath = item.sourceAssetPath;
    request.subAssetKey = item.subAssetKey;
    request.artifactPath = item.artifactPath;
    request.generatedSubAsset = item.kind == AssetBrowserItemKind::GeneratedSubAsset;
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
    if (request.subAssetKey.empty() &&
        (item.type == AssetBrowserItemType::Prefab ||
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
        request.settingsFingerprint = "asset-browser-thumbnail:v35-prefab-qem-material-proxy";
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
    return request;
}
}

AssetThumbnailService::~AssetThumbnailService()
{
    if (m_generationCancelToken)
        m_generationCancelToken->cancelled.store(true, std::memory_order_relaxed);
    for (const auto& request : m_inFlightThumbnails)
    {
        if (request.cancelToken)
            request.cancelToken->cancelled.store(true, std::memory_order_relaxed);
    }
    m_generationCancelToken.reset();
    WaitForInFlightRequests();
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

bool AssetThumbnailService::IsLoadingAssetPreview(
    const AssetThumbnailRequest& request) const
{
    const auto state = GetThumbnailState(request);
    return state == ThumbnailState::Queued ||
        state == ThumbnailState::Preparing ||
        state == ThumbnailState::WaitingForResources ||
        state == ThumbnailState::Rendering ||
        state == ThumbnailState::WaitingForGpu ||
        state == ThumbnailState::Readback;
}

AssetThumbnailServiceResult AssetThumbnailService::GetThumbnail(
    const AssetThumbnailRequest& request)
{
    NLS_PROFILE_NAMED_SCOPE("AssetThumbnailService::GetThumbnail");
    NLS::Base::Profiling::PerformanceStageScope lookupScope(
        NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
        "ThumbnailCacheLookup",
        NLS::Base::Profiling::PerformanceStageThread::Main);

    AssetThumbnailServiceResult result;
    result.fallbackIcon = FallbackIconForKind(request.kind);
    const std::string requestTelemetryPath = BuildThumbnailRequestTelemetryPath(request);

    std::string requestedCacheKey;
    {
        const auto stableLookupBegin = std::chrono::steady_clock::now();
        requestedCacheKey = BuildAssetThumbnailCacheKey(request);
        if (const auto stableIterator = m_stableThumbnailResultsByCacheKey.find(requestedCacheKey);
            stableIterator != m_stableThumbnailResultsByCacheKey.end())
        {
            std::error_code imageError;
            const bool imageStillExists =
                !stableIterator->second.imagePath.empty() &&
                std::filesystem::is_regular_file(stableIterator->second.imagePath, imageError) &&
                !imageError;
            if (imageStillExists && IsThumbnailRequestStillFresh(request))
            {
                RecordThumbnailRequestBuildTelemetry(
                    NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailServiceRequestStableLookup,
                    stableLookupBegin,
                    requestTelemetryPath);
                lookupScope.AddCounter("stableThumbnailResultHitCount");
                if (stableIterator->second.cacheEntry.has_value())
                    m_thumbnailStatesByCacheKey[stableIterator->second.cacheEntry->cacheKey] = ThumbnailState::Ready;
                return stableIterator->second;
            }
            m_stableThumbnailResultsByCacheKey.erase(stableIterator);
        }
        RecordThumbnailRequestBuildTelemetry(
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailServiceRequestStableLookup,
            stableLookupBegin,
            requestTelemetryPath);
    }

    auto pendingStateIterator = m_thumbnailStatesByCacheKey.find(requestedCacheKey);
    const bool hasActiveGpuReadbackState =
        pendingStateIterator != m_thumbnailStatesByCacheKey.end() &&
        (pendingStateIterator->second == ThumbnailState::WaitingForGpu ||
            pendingStateIterator->second == ThumbnailState::Readback);
    if (pendingStateIterator != m_thumbnailStatesByCacheKey.end() &&
        IsPendingThumbnailState(pendingStateIterator->second))
    {
        const auto queueBegin = std::chrono::steady_clock::now();
        auto queuedIterator = m_queuedRequestsByCacheKey.find(requestedCacheKey);
        const bool adoptedInFlightRequest = AdoptMatchingInFlightRequest(requestedCacheKey);
        if (adoptedInFlightRequest || queuedIterator != m_queuedRequestsByCacheKey.end() || hasActiveGpuReadbackState)
        {
            lookupScope.AddCounter("duplicateThumbnailRequestCount");
            if (queuedIterator != m_queuedRequestsByCacheKey.end() &&
                ShouldPromoteQueuedThumbnailRequest(queuedIterator->second, request))
            {
                lookupScope.AddCounter("coalescingPressure");
                queuedIterator->second = request;
                EnqueueQueuedCacheKey(requestedCacheKey, queuedIterator->second);
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
            return result;
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
            return result;
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
        evaluation.entry.has_value())
    {
        lookupScope.AddCounter("cacheHitCount");
        m_thumbnailStatesByCacheKey[evaluation.entry->cacheKey] = ThumbnailState::Ready;
        result.status = AssetThumbnailServiceStatus::Fresh;
        result.imagePath = evaluation.entry->imagePath;
        m_stableThumbnailResultsByCacheKey[evaluation.entry->cacheKey] = result;
        m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(evaluation.entry->cacheKey);
        return result;
    }
    lookupScope.AddCounter("cacheMissCount");

    if (evaluation.status == AssetThumbnailCacheStatus::Failed &&
        !IsRetryableThumbnailFailureDiagnostic(request, evaluation.diagnostic))
    {
        if (evaluation.entry.has_value())
            m_thumbnailStatesByCacheKey[evaluation.entry->cacheKey] = ThumbnailState::Failed;
        result.status = AssetThumbnailServiceStatus::Failed;
        return result;
    }

    if (!CanRequestThumbnailGeneration(request.kind))
    {
        if (evaluation.entry.has_value())
            m_thumbnailStatesByCacheKey[evaluation.entry->cacheKey] = ThumbnailState::Failed;
        result.status = AssetThumbnailServiceStatus::Fallback;
        result.diagnostic = UnsupportedDiagnosticForKind(request.kind);
        return result;
    }

    if (evaluation.entry.has_value())
    {
        const auto queueBegin = std::chrono::steady_clock::now();
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(evaluation.entry->cacheKey);
        const bool hasActiveGpuReadbackState =
            stateIterator != m_thumbnailStatesByCacheKey.end() &&
            (stateIterator->second == ThumbnailState::WaitingForGpu ||
                stateIterator->second == ThumbnailState::Readback);
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
            return result;
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
                return result;
            }
            queuedIterator = m_queuedRequestsByCacheKey.emplace(evaluation.entry->cacheKey, request).first;
            EnqueueQueuedCacheKey(evaluation.entry->cacheKey, request);
        }
        else
        {
            lookupScope.AddCounter("duplicateThumbnailRequestCount");
            lookupScope.AddCounter("coalescingPressure");
            if (ShouldPromoteQueuedThumbnailRequest(queuedIterator->second, request))
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
                    return result;
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
    return result;
}

std::optional<AssetThumbnailServiceResult> AssetThumbnailService::GenerateNextThumbnail()
{
    NLS::Base::Profiling::PerformanceStageScope totalScope(
        NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
        "TotalThumbnail",
        NLS::Base::Profiling::PerformanceStageThread::Main);
    totalScope.AddCounter("queueBacklog", m_queuedRequestsByCacheKey.size());
    totalScope.AddCounter("inFlightRequestCount", m_inFlightThumbnails.size());
    totalScope.AddCounter("cacheWriteBudgetRemaining", m_generationBudget.cacheWriteCountBudget);
    totalScope.AddCounter("cpuPreparationByteBudgetRemaining", m_generationBudget.cpuPreparationByteBudget);
    totalScope.AddCounter("gpuUploadByteBudgetRemaining", m_generationBudget.gpuUploadByteBudget);

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
        const auto cacheKey = PopNextQueuedCacheKey();
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
            m_queuedRequestsByCacheKey.erase(requestIterator);
            const auto evaluation = EvaluateAssetThumbnailCache(request);
            const auto previewRequest = ResolveDeferredThumbnailPreviewRequest(request);
            if (const auto invalidPathDiagnostic = ValidateGpuPreviewRequestArtifactPaths(previewRequest);
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

        m_queuedRequestsByCacheKey.erase(requestIterator);

        RestoreDeferredCacheKeys(deferredCacheKeys);
        const auto generated = TryGenerateThumbnailForRequest(request, m_generationCancelToken);
        m_thumbnailStatesByCacheKey[*cacheKey] = generated.status == AssetThumbnailServiceStatus::Fresh
            ? ThumbnailState::Ready
            : ThumbnailState::Failed;
        if (generated.status == AssetThumbnailServiceStatus::Fresh)
        {
            ConsumeThumbnailCacheWriteBudgetForFreshResult(
                m_generationBudget,
                m_hasExplicitGenerationBudget);
            totalScope.AddCounter("thumbnailsGeneratedThisFrame");
        }
        m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
        m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(*cacheKey);
        m_gpuPreviewResourcePendingDeferredCacheKeys.erase(*cacheKey);
        return generated;
    }

    RestoreDeferredCacheKeys(deferredCacheKeys);
    return std::nullopt;
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
        !HasQueuedGpuPreviewReadback())
    {
        return std::nullopt;
    }

    if (!m_generationCancelToken)
        m_generationCancelToken = std::make_shared<AssetThumbnailGenerationCancelToken>();
    m_generationCancelToken->generation = m_generationSerial;

    std::vector<std::string> deferredCacheKeys;
    size_t deferredGpuPreviewCount = 0u;
    size_t deferredHeavyGpuPreviewCount = 0u;
    size_t resolvedHeavyGpuPreviewManifestLookupCount = 0u;
    const size_t maxDeferredGpuPreviewScanPerCall = includeHeavyGpuPreviews
        ? kMaxDeferredHeavyGpuPreviewScanPerCall
        : kMaxDeferredLightGpuPreviewScanPerCall;
    while (HasQueuedCacheKeys() || HasQueuedGpuPreviewReadback())
    {
        const auto cacheKey = PopNextGpuPreviewCacheKey(includeHeavyGpuPreviews);
        if (!cacheKey.has_value())
            break;

        const auto requestIterator = m_queuedRequestsByCacheKey.find(*cacheKey);
        if (requestIterator == m_queuedRequestsByCacheKey.end())
            continue;

        const auto request = requestIterator->second;
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
        if (!pollingPendingReadback &&
            !SupportsGpuThumbnailPreview(request))
        {
            deferredCacheKeys.push_back(*cacheKey);
            ++deferredGpuPreviewCount;
            if (deferredGpuPreviewCount >= maxDeferredGpuPreviewScanPerCall)
                break;
            continue;
        }
        if (!pollingPendingReadback &&
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
        const auto resolvedPreviewIterator = m_resolvedPreviewRequestsByCacheKey.find(*cacheKey);
        auto previewRequest = request;
        if (resolvedPreviewIterator != m_resolvedPreviewRequestsByCacheKey.end())
        {
            previewRequest = resolvedPreviewIterator->second;
        }
        else
        {
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
            if (unresolvedHeavyPreview &&
                resolvedHeavyGpuPreviewManifestLookupCount >= kMaxResolvedHeavyGpuPreviewManifestLookupsPerCall)
            {
                deferredCacheKeys.push_back(*cacheKey);
                ++deferredHeavyGpuPreviewCount;
                if (deferredHeavyGpuPreviewCount >= maxDeferredGpuPreviewScanPerCall)
                    break;
                continue;
            }
            if (!pollingPendingReadback &&
                IsHeavyGpuThumbnailPreview(request.kind) &&
                request.artifactPath.empty())
            {
                ++resolvedHeavyGpuPreviewManifestLookupCount;
            }
            previewRequest = ResolveDeferredThumbnailPreviewRequest(request);
            if (!previewRequest.artifactPath.empty())
                m_resolvedPreviewRequestsByCacheKey[*cacheKey] = previewRequest;
            else if (unresolvedHeavyPreview && ThumbnailArtifactManifestExceedsPreviewBudget(request))
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
                EnqueueQueuedCacheKey(*cacheKey, request, false);
                m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::WaitingForResources;
                auto result = BuildResultFromEvaluation(
                    request,
                    evaluation,
                    AssetThumbnailServiceStatus::Pending);
                result.diagnostic = "thumbnail-gpu-preview-complexity-pending";
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
            m_gpuPreviewResourcePendingDeferredCacheKeys.erase(*cacheKey);
            return result;
        };

        if (pumpingPendingResources)
        {
            const auto evaluation = EvaluateAssetThumbnailCache(request);
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
            if (pump.supported && pump.resourcesPending)
            {
                m_queuedRequestsByCacheKey[*cacheKey] = request;
                if (!previewRequest.artifactPath.empty())
                    m_resolvedPreviewRequestsByCacheKey[*cacheKey] = previewRequest;
                EnqueueQueuedCacheKey(*cacheKey, request, true);
                m_gpuPreviewResourcePendingDeferredCacheKeys.insert(*cacheKey);
                m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::WaitingForResources;
                auto result = BuildResultFromEvaluation(
                    request,
                    evaluation,
                    AssetThumbnailServiceStatus::Pending);
                result.diagnostic = pump.diagnostic.empty()
                    ? std::string("thumbnail-gpu-preview-resources-pending")
                    : pump.diagnostic;
                RestoreDeferredCacheKeys(deferredCacheKeys);
                return result;
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
        m_queuedRequestsByCacheKey.erase(requestIterator);

        const auto evaluation = EvaluateAssetThumbnailCache(request);
        if (const auto invalidPathDiagnostic = ValidateGpuPreviewRequestArtifactPaths(previewRequest);
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
        if (!IsThumbnailRequestStillFresh(request))
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
            if (pump.supported && pump.resourcesPending)
            {
                m_queuedRequestsByCacheKey[*cacheKey] = request;
                if (!previewRequest.artifactPath.empty())
                    m_resolvedPreviewRequestsByCacheKey[*cacheKey] = previewRequest;
                EnqueueQueuedCacheKey(*cacheKey, request, true);
                m_gpuPreviewResourcePendingDeferredCacheKeys.insert(*cacheKey);
                m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::WaitingForResources;
                auto result = BuildResultFromEvaluation(
                    request,
                    evaluation,
                    AssetThumbnailServiceStatus::Pending);
                result.diagnostic = pump.diagnostic.empty()
                    ? std::string("thumbnail-gpu-preview-resources-pending")
                    : pump.diagnostic;
                RestoreDeferredCacheKeys(deferredCacheKeys);
                return result;
            }
        }

        NLS::Editor::Assets::EditorThumbnailPreviewResult preview;
        {
            NLS::Base::Profiling::PerformanceStageScope previewScope(
                NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
                "PreparePreviewAsset",
                NLS::Base::Profiling::PerformanceStageThread::Main);
            const auto previewTelemetryBegin = std::chrono::steady_clock::now();
            preview = previewRenderer.Render(previewRequest);
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
        const auto publishedGpuTexture = preview.gpuTexture;
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
            if (publishedGpuTexture.IsValid())
            {
                result.gpuTexture = publishedGpuTexture;
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
                if (!pollingPendingReadback &&
                    IsPendingThumbnailPreviewReadbackDiagnostic(diagnostic) &&
                    m_hasExplicitGenerationBudget)
                {
                    ConsumeThumbnailCountBudget(m_generationBudget.readbackCountBudget, true);
                }
                m_queuedRequestsByCacheKey[*cacheKey] = request;
                if (!previewRequest.artifactPath.empty())
                    m_resolvedPreviewRequestsByCacheKey[*cacheKey] = previewRequest;
                const bool resourcesPending = IsPendingThumbnailPreviewResourcesDiagnostic(diagnostic);
                EnqueueQueuedCacheKey(*cacheKey, request, resourcesPending);
                if (resourcesPending)
                    m_gpuPreviewResourcePendingDeferredCacheKeys.insert(*cacheKey);
                RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                    std::string("requeue-retryable=") + diagnostic,
                    &request,
                    m_queuedRequestsByCacheKey.size());
            }
            const bool pendingReadbackFailure = IsPendingThumbnailPreviewReadbackDiagnostic(diagnostic);
            m_thumbnailStatesByCacheKey[*cacheKey] = retryableGpuFailure
                ? (pendingReadbackFailure
                    ? ThumbnailState::WaitingForGpu
                    : (IsPendingThumbnailPreviewResourcesDiagnostic(diagnostic)
                        ? ThumbnailState::WaitingForResources
                        : ThumbnailState::Queued))
                : ThumbnailState::Failed;
            if (pendingReadbackFailure)
            {
                m_gpuPreviewReadbackPendingCacheKeys.insert(*cacheKey);
                m_gpuPreviewReadbackPendingRequestsByCacheKey[*cacheKey] = request;
                RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                    "track-readback-pending",
                    &request,
                    m_gpuPreviewReadbackPendingCacheKeys.size());
            }
            else
            {
                m_gpuPreviewReadbackPendingCacheKeys.erase(*cacheKey);
                m_gpuPreviewReadbackPendingRequestsByCacheKey.erase(*cacheKey);
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
        if (clearFrameDisposition == GpuPreviewClearFrameDisposition::DeferEmptyFrame)
        {
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
            if (!previewRequest.artifactPath.empty())
                m_resolvedPreviewRequestsByCacheKey[*cacheKey] = previewRequest;
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Queued;
            RestoreDeferredCacheKeys(deferredCacheKeys);
            return result;
        }
        if (clearFrameDisposition == GpuPreviewClearFrameDisposition::FailEmptyFrame)
        {
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "completed-readback-disposition=empty-frame",
                &request,
                m_inFlightThumbnails.size());
            recordGpuPreviewTerminalDiagnostic("thumbnail-gpu-preview-empty-frame");
            auto result = BuildGpuPreviewEmptyFrameResult(request, evaluation, previewRequest);
            m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(*cacheKey);
            m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
            m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Failed;
            RestoreDeferredCacheKeys(deferredCacheKeys);
            return result;
        }

        RestoreDeferredCacheKeys(deferredCacheKeys);
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
        auto pixels = std::move(preview.rgbaPixels);
        const auto width = preview.width;
        const auto height = preview.height;
        const auto performanceCaptureToken =
            NLS::Base::Profiling::PerformanceStageStatsCapture::GetActiveToken();
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
            m_inFlightThumbnails.push_back({
                *cacheKey,
                m_generationSerial,
                cancelToken,
                ScheduleThumbnailJobFuture(
                    "AssetThumbnailService.GpuPreviewCacheWrite",
                    [request, metadataRequest, evaluation, pixels = std::move(pixels), width, height, cancelToken, performanceCaptureToken]() mutable
                    {
                        NLS::Base::Profiling::PerformanceStageStatsCaptureScope capture(performanceCaptureToken);
                        ScopedThumbnailGenerationStageThread backgroundStageThread(
                            PerformanceStageThread::Background);
                        return WriteRgbaThumbnailResult(
                            request,
                            evaluation,
                            pixels.data(),
                            width,
                            height,
                            "thumbnail-gpu-preview-generation-failed",
                            cancelToken,
                            &metadataRequest);
                    }),
		                request,
		                false
		            });
            m_thumbnailStatesByCacheKey[*cacheKey] = ThumbnailState::Readback;
            m_gpuPreviewReadbackPendingCacheKeys.erase(*cacheKey);
            m_gpuPreviewReadbackPendingRequestsByCacheKey.erase(*cacheKey);
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                "cache-write-start",
                &request,
                m_inFlightThumbnails.size());
            m_resolvedPreviewRequestsByCacheKey.erase(*cacheKey);
            m_gpuDeferredHeavyPreviewCacheKeys.erase(*cacheKey);
            m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(*cacheKey);
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
        return pending;
    }

    RestoreDeferredCacheKeys(deferredCacheKeys);
    return std::nullopt;
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

    const auto currentGenerationInFlightCount = CountCurrentGenerationInFlightRequests();
    const auto obsoleteGenerationInFlightCount =
        m_inFlightThumbnails.size() - currentGenerationInFlightCount;
    const auto maxCurrentGenerationInFlightCount =
        m_hasExplicitGenerationBudget && m_generationBudget.cacheWriteCountBudget != SIZE_MAX
            ? (std::min)(kMaxCurrentThumbnailGenerationInFlightRequests, m_generationBudget.cacheWriteCountBudget)
            : kMaxCurrentThumbnailGenerationInFlightRequests;
    if (currentGenerationInFlightCount >= maxCurrentGenerationInFlightCount ||
        obsoleteGenerationInFlightCount > kMaxObsoleteThumbnailGenerationInFlightRequests ||
        m_inFlightThumbnails.size() >= kMaxThumbnailGenerationTotalInFlightSlots)
    {
        return false;
    }

    if (m_generationBudget.cacheWriteCountBudget == 0u && HasQueuedCacheKeys())
        return false;

    std::vector<std::string> deferredCacheKeys;
    size_t deferredGpuPreviewCount = 0u;

    while (HasQueuedCacheKeys())
    {
        const auto cacheKey = PopNextQueuedCacheKey();
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
            m_inFlightThumbnails.push_back({
                *cacheKey,
                m_generationSerial,
                cancelToken,
                ScheduleThumbnailJobFuture(
                    "AssetThumbnailService.GenerateThumbnail",
                    [request, cancelToken, performanceCaptureToken]
                    {
                        NLS::Base::Profiling::PerformanceStageStatsCaptureScope capture(performanceCaptureToken);
                        ScopedThumbnailGenerationStageThread backgroundStageThread(
                            PerformanceStageThread::Background);
                        return TryGenerateThumbnailForRequest(request, cancelToken);
                }),
	                request,
	                false
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
    return false;
}

std::optional<AssetThumbnailServiceResult> AssetThumbnailService::ConsumeCompletedThumbnail()
{
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
        const bool currentGeneration = iterator->generation == m_generationSerial;
        if (result.status == AssetThumbnailServiceStatus::Fresh)
        {
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
            m_gpuPreviewReadbackPendingCacheKeys.erase(iterator->cacheKey);
            m_gpuPreviewReadbackPendingRequestsByCacheKey.erase(iterator->cacheKey);
        }
        else if (result.status == AssetThumbnailServiceStatus::Pending)
        {
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                std::string("cache-write-complete=pending|diag=") + result.diagnostic,
                &iterator->request,
                m_inFlightThumbnails.size());
            m_thumbnailStatesByCacheKey[iterator->cacheKey] = ThumbnailState::Queued;
            if (iterator->requeueOnPending && currentGeneration)
            {
                m_queuedRequestsByCacheKey[iterator->cacheKey] = iterator->request;
                EnqueueQueuedCacheKey(iterator->cacheKey, iterator->request);
                if (IsPendingThumbnailPreviewReadbackDiagnostic(result.diagnostic))
                {
                    m_thumbnailStatesByCacheKey[iterator->cacheKey] = ThumbnailState::WaitingForGpu;
                    m_gpuPreviewReadbackPendingCacheKeys.insert(iterator->cacheKey);
                    m_gpuPreviewReadbackPendingRequestsByCacheKey[iterator->cacheKey] = iterator->request;
                }
            }
        }
        else if (currentGeneration)
        {
            RecordThumbnailGpuPreviewQueueDecisionTelemetry(
                std::string("cache-write-complete=failed|diag=") + result.diagnostic,
                &iterator->request,
                m_inFlightThumbnails.size());
            m_thumbnailStatesByCacheKey[iterator->cacheKey] = ThumbnailState::Failed;
            m_gpuPreviewReadbackPendingCacheKeys.erase(iterator->cacheKey);
            m_gpuPreviewReadbackPendingRequestsByCacheKey.erase(iterator->cacheKey);
            if (iterator->requeueOnPending)
            {
                m_resolvedPreviewRequestsByCacheKey.erase(iterator->cacheKey);
                m_gpuDeferredHeavyPreviewCacheKeys.erase(iterator->cacheKey);
            }
        }
        else
        {
            m_thumbnailStatesByCacheKey[iterator->cacheKey] = ThumbnailState::Cancelled;
            m_gpuPreviewReadbackPendingCacheKeys.erase(iterator->cacheKey);
            m_gpuPreviewReadbackPendingRequestsByCacheKey.erase(iterator->cacheKey);
        }
        iterator = m_inFlightThumbnails.erase(iterator);
        if (currentGeneration)
            return result;
    }
    return std::nullopt;
}

bool AssetThumbnailService::HasInFlightRequest() const
{
    return !m_inFlightThumbnails.empty();
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

size_t AssetThumbnailService::GetQueuedRequestCount() const
{
    size_t queuedRequestCount = m_queuedRequestsByCacheKey.size();
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
    return queuedRequestCount;
}

ThumbnailState AssetThumbnailService::GetThumbnailState(const AssetThumbnailRequest& request) const
{
    const auto cacheKey = BuildAssetThumbnailCacheKey(request);
    const auto found = m_thumbnailStatesByCacheKey.find(cacheKey);
    if (found != m_thumbnailStatesByCacheKey.end())
        return found->second;

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

bool AssetThumbnailService::HasQueuedCacheKeys() const
{
    return !m_queuedVisibleCacheKeys.empty() ||
        !m_queuedInspectorCacheKeys.empty() ||
        !m_queuedPrefetchCacheKeys.empty() ||
        !m_queuedPriorityCacheKeys.empty() ||
        !m_queuedCacheKeys.empty();
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
    for (const auto& [cacheKey, request] : m_queuedRequestsByCacheKey)
    {
        if (!SupportsGpuThumbnailPreview(request))
            continue;
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(cacheKey);
        if (stateIterator != m_thumbnailStatesByCacheKey.end() &&
            stateIterator->second == ThumbnailState::WaitingForResources)
        {
            return true;
        }
    }
    return false;
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
            (stateIterator->second == ThumbnailState::WaitingForGpu ||
                stateIterator->second == ThumbnailState::Readback))
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
    m_resolvedPreviewRequestsByCacheKey.erase(*victim);
    m_gpuDeferredHeavyPreviewCacheKeys.erase(*victim);
    m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(*victim);
    m_gpuPreviewReadbackPendingCacheKeys.erase(*victim);
    m_gpuPreviewReadbackPendingRequestsByCacheKey.erase(*victim);
    m_thumbnailStatesByCacheKey[*victim] = ThumbnailState::Cancelled;
    m_gpuPreviewResourcePendingDeferredCacheKeys.erase(*victim);
    return true;
}

bool AssetThumbnailService::HasDeferredGpuPreviewEmptyFrame(const std::string& cacheKey) const
{
    return m_gpuPreviewEmptyFrameDeferredCacheKeys.find(cacheKey) !=
        m_gpuPreviewEmptyFrameDeferredCacheKeys.end();
}

void AssetThumbnailService::EnqueueQueuedCacheKey(
    const std::string& cacheKey,
    const AssetThumbnailRequest& request,
    const bool atFront)
{
    m_gpuPreviewEmptyFrameDeferredCacheKeys.erase(cacheKey);
    m_gpuPreviewResourcePendingDeferredCacheKeys.erase(cacheKey);
    RemoveQueuedCacheKeyOccurrences(cacheKey);
    const auto enqueue = [atFront, &cacheKey](auto& queue)
    {
        if (atFront)
            queue.push_front(cacheKey);
        else
            queue.push_back(cacheKey);
    };
    switch (request.priority)
    {
    case ThumbnailRequestPriority::Visible:
        enqueue(m_queuedVisibleCacheKeys);
        return;
    case ThumbnailRequestPriority::Inspector:
        enqueue(m_queuedInspectorCacheKeys);
        return;
    case ThumbnailRequestPriority::Prefetch:
        enqueue(m_queuedPrefetchCacheKeys);
        return;
    case ThumbnailRequestPriority::Background:
        break;
    }

    if (ShouldPrioritizeThumbnailRequest(request))
        enqueue(m_queuedPriorityCacheKeys);
    else
        enqueue(m_queuedCacheKeys);
}

std::optional<std::string> AssetThumbnailService::PopNextQueuedCacheKey()
{
    const bool hasExplicitPriority =
        !m_queuedVisibleCacheKeys.empty() ||
        !m_queuedInspectorCacheKeys.empty() ||
        !m_queuedPrefetchCacheKeys.empty();
    if (hasExplicitPriority &&
        (m_queuedCacheKeys.empty() ||
            m_priorityThumbnailDequeueStreak < kMaxPriorityThumbnailDequeueBurst))
    {
        if (auto cacheKey = PopNextQueuedCacheKeyFrom(m_queuedVisibleCacheKeys, m_priorityThumbnailDequeueStreak, true);
            cacheKey.has_value())
        {
            return cacheKey;
        }
        if (auto cacheKey = PopNextQueuedCacheKeyFrom(m_queuedInspectorCacheKeys, m_priorityThumbnailDequeueStreak, true);
            cacheKey.has_value())
        {
            return cacheKey;
        }
        if (auto cacheKey = PopNextQueuedCacheKeyFrom(m_queuedPrefetchCacheKeys, m_priorityThumbnailDequeueStreak, true);
            cacheKey.has_value())
        {
            return cacheKey;
        }
    }

    if (!m_queuedPriorityCacheKeys.empty() &&
        (m_queuedCacheKeys.empty() ||
            m_priorityThumbnailDequeueStreak < kMaxPriorityThumbnailDequeueBurst))
    {
        auto cacheKey = m_queuedPriorityCacheKeys.front();
        m_queuedPriorityCacheKeys.pop_front();
        ++m_priorityThumbnailDequeueStreak;
        return cacheKey;
    }

    if (!m_queuedCacheKeys.empty())
    {
        auto cacheKey = PopNextQueuedCacheKeyFrom(m_queuedCacheKeys, m_priorityThumbnailDequeueStreak, false);
        m_priorityThumbnailDequeueStreak = 0u;
        return cacheKey;
    }

    if (auto cacheKey = PopNextQueuedCacheKeyFrom(m_queuedVisibleCacheKeys, m_priorityThumbnailDequeueStreak, true);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = PopNextQueuedCacheKeyFrom(m_queuedInspectorCacheKeys, m_priorityThumbnailDequeueStreak, true);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = PopNextQueuedCacheKeyFrom(m_queuedPrefetchCacheKeys, m_priorityThumbnailDequeueStreak, true);
        cacheKey.has_value())
    {
        return cacheKey;
    }

    if (!m_queuedPriorityCacheKeys.empty())
    {
        return PopNextQueuedCacheKeyFrom(m_queuedPriorityCacheKeys, m_priorityThumbnailDequeueStreak, true);
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

    removeFromQueue(m_queuedVisibleCacheKeys);
    removeFromQueue(m_queuedInspectorCacheKeys);
    removeFromQueue(m_queuedPrefetchCacheKeys);
    removeFromQueue(m_queuedPriorityCacheKeys);
    removeFromQueue(m_queuedCacheKeys);
}

std::optional<std::string> AssetThumbnailService::PopNextGpuPreviewCacheKey(
    const bool includeHeavyGpuPreviews)
{
    auto noteDequeuedCacheKey = [this](const bool countTowardsPriorityBurst)
    {
        if (countTowardsPriorityBurst)
            ++m_priorityThumbnailDequeueStreak;
        else
            m_priorityThumbnailDequeueStreak = 0u;
    };

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
            if (!SupportsGpuThumbnailPreview(request))
                continue;
            const auto stateIterator = m_thumbnailStatesByCacheKey.find(*iterator);
            if (stateIterator != m_thumbnailStatesByCacheKey.end() &&
                stateIterator->second == ThumbnailState::WaitingForResources)
            {
                if (m_gpuPreviewResourcePendingDeferredCacheKeys.erase(*iterator) != 0u)
                {
                    const auto cacheKey = *iterator;
                    queue.erase(iterator);
                    queue.push_front(cacheKey);
                    return std::nullopt;
                }
            }
            if (!includeHeavyGpuPreviews &&
                IsHeavyGpuThumbnailPreview(request.kind))
            {
                m_gpuDeferredHeavyPreviewCacheKeys.insert(*iterator);
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

    for (auto iterator = m_gpuPreviewReadbackPendingCacheKeys.begin();
        iterator != m_gpuPreviewReadbackPendingCacheKeys.end();)
    {
        const auto cacheKey = *iterator;
        auto requestIterator = m_queuedRequestsByCacheKey.find(cacheKey);
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(cacheKey);
        if (stateIterator != m_thumbnailStatesByCacheKey.end() &&
            stateIterator->second == ThumbnailState::WaitingForGpu)
        {
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
            noteDequeuedCacheKey(true);
            return cacheKey;
        }
        RecordThumbnailGpuPreviewQueueDecisionTelemetry(
            "readback-pending-stale-state",
            nullptr,
            m_gpuPreviewReadbackPendingCacheKeys.size());
        iterator = m_gpuPreviewReadbackPendingCacheKeys.erase(iterator);
        m_gpuPreviewReadbackPendingRequestsByCacheKey.erase(cacheKey);
    }

    if (auto cacheKey = popPendingReadback(m_queuedVisibleCacheKeys, true);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = popPendingReadback(m_queuedInspectorCacheKeys, true);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = popPendingReadback(m_queuedPrefetchCacheKeys, true);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = popPendingReadback(m_queuedPriorityCacheKeys, true);
        cacheKey.has_value())
    {
        return cacheKey;
    }
    if (auto cacheKey = popPendingReadback(m_queuedCacheKeys, false);
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

        switch (found->second.priority)
        {
        case ThumbnailRequestPriority::Visible:
            RestoreDeferredCacheKeyToFront(m_queuedVisibleCacheKeys, deferred);
            break;
        case ThumbnailRequestPriority::Inspector:
            RestoreDeferredCacheKeyToFront(m_queuedInspectorCacheKeys, deferred);
            break;
        case ThumbnailRequestPriority::Prefetch:
            RestoreDeferredCacheKeyToFront(m_queuedPrefetchCacheKeys, deferred);
            break;
        case ThumbnailRequestPriority::Background:
            if (ShouldPrioritizeThumbnailRequest(found->second))
                RestoreDeferredCacheKeyToFront(m_queuedPriorityCacheKeys, deferred);
            else
                RestoreDeferredCacheKeyToFront(m_queuedCacheKeys, deferred);
            break;
        }
    }
    deferredCacheKeys.clear();
}

void AssetThumbnailService::ClearQueuedRequests()
{
    ++m_generationSerial;
    ClearPendingQueuedRequestsWithDiagnostics();
}

void AssetThumbnailService::ClearPendingQueuedRequestsWithDiagnostics()
{
    const auto queuedRequestCount = m_queuedRequestsByCacheKey.size();
    if (queuedRequestCount == 0u)
    {
        ClearPendingQueuedRequests();
        return;
    }

    NLS::Base::Profiling::PerformanceStageScope totalScope(
        NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
        "TotalThumbnail",
        NLS::Base::Profiling::PerformanceStageThread::Main);
    totalScope.AddCounter("queueBacklog", queuedRequestCount);
    totalScope.AddCounter("cancelledThumbnailRequestCount", queuedRequestCount);
    const auto cancellationBegin = std::chrono::steady_clock::now();
    ClearPendingQueuedRequests();
    const auto cancellationLatency = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - cancellationBegin);
    totalScope.AddCounter("cancellationLatency", static_cast<uint64_t>(cancellationLatency.count()));
}

void AssetThumbnailService::ClearPendingQueuedRequests()
{
    auto isActiveGpuReadbackCacheKey = [this](const std::string& cacheKey)
    {
        const auto stateIterator = m_thumbnailStatesByCacheKey.find(cacheKey);
        return stateIterator != m_thumbnailStatesByCacheKey.end() &&
            (stateIterator->second == ThumbnailState::WaitingForGpu ||
                stateIterator->second == ThumbnailState::Readback);
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
    for (const auto& cacheKey : m_gpuPreviewResourcePendingDeferredCacheKeys)
    {
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
    while (!m_queuedVisibleCacheKeys.empty())
        m_queuedVisibleCacheKeys.pop_front();
    while (!m_queuedInspectorCacheKeys.empty())
        m_queuedInspectorCacheKeys.pop_front();
    while (!m_queuedPrefetchCacheKeys.empty())
        m_queuedPrefetchCacheKeys.pop_front();
    while (!m_queuedCacheKeys.empty())
        m_queuedCacheKeys.pop_front();
    m_queuedRequestsByCacheKey.clear();
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
    for (auto iterator = m_gpuPreviewReadbackPendingRequestsByCacheKey.begin();
        iterator != m_gpuPreviewReadbackPendingRequestsByCacheKey.end();)
    {
        if (isActiveGpuReadbackCacheKey(iterator->first))
            ++iterator;
        else
            iterator = m_gpuPreviewReadbackPendingRequestsByCacheKey.erase(iterator);
    }
    m_priorityThumbnailDequeueStreak = 0u;
    for (const auto& [cacheKey, request] : preservedGpuReadbackRequests)
    {
        m_queuedRequestsByCacheKey[cacheKey] = request;
        EnqueueQueuedCacheKey(cacheKey, request);
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
    ClearPendingQueuedRequestsWithDiagnostics();
}

void AssetThumbnailService::WaitForInFlightRequests()
{
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
}

bool AssetThumbnailService::AdoptMatchingInFlightRequest(const std::string& cacheKey)
{
    for (auto& request : m_inFlightThumbnails)
    {
        if (request.cacheKey == cacheKey && request.future.valid())
        {
            request.generation = m_generationSerial;
            return true;
        }
    }
    return false;
}
}

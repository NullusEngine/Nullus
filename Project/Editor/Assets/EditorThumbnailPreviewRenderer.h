#pragma once

#include "Assets/AssetThumbnail.h"
#include "Assets/AssetThumbnailCache.h"
#include "Serialize/ObjectGraphInstantiator.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <Math/Vector3.h>

namespace NLS::Render::Context
{
class Driver;
struct PostSubmitTextureReadbackState;
}

namespace NLS::Render::RHI
{
class RHICompletionToken;
}

namespace NLS::Core::ResourceManagement
{
class MaterialManager;
class MeshManager;
class ShaderManager;
}

namespace NLS::Render::Resources
{
class Material;
}

namespace NLS::Editor::Assets
{
struct PreviewRenderableSnapshot;

// Cold Prefabs above this topology size wait for a resident scene package.
// Keep the same boundary in the service so GPU validation failures cannot
// reopen a large artifact through the CPU fallback path.
inline constexpr size_t kMaxColdGpuPreviewPrefabDrawItems = 64u;

struct EditorThumbnailPreviewResult
{
    ThumbnailRenderStatus status = ThumbnailRenderStatus::NotReady;
    std::vector<uint8_t> rgbaPixels;
    AssetThumbnailGpuTexture gpuTexture;
    uint32_t width = 0u;
    uint32_t height = 0u;
    std::string diagnostic;
    // A provisional render may allocate and submit a GPU target for internal
    // warmup, but it is never a UI/presentation result.
    bool publishableGpuTexture = true;
    bool completedPendingReadback = false;
    // The GPU frame is still canonical and can be handed to the UI, but the
    // bounded persistence queues could not retain the readback. This is a
    // deliberate durability downgrade, not a render failure.
    bool persistenceDeferred = false;
    // Prefab previews carry the number of canonical MeshRenderer draw items
    // that must survive scene synchronization. Other preview kinds leave this
    // at zero and retain their existing validation behavior.
    uint64_t expectedSceneDrawCount = 0u;
    uint64_t rawVisibleDrawCount = 0u;
    uint64_t submittedSceneDrawCount = 0u;
    uint64_t objectDataOverflowDroppedObjectCount = 0u;
    // Stable within one resource state and changes only when dependency or
    // scene-assembly work advances. The service uses this to distinguish a
    // slow large preview from a genuinely stalled continuation.
    uint64_t resourceProgressToken = 0u;
    // Accepted asynchronous dependency work can remain active while the
    // aggregate resource counts above are unchanged. Keep that state separate
    // from the token so a large artifact is not mistaken for a stalled load.
    bool resourceWorkActive = false;
    // The immutable canonical prefab snapshot used for this GPU submission.
    // Keeping it on the result lets a validation failure use the already
    // prepared graph for CPU persistence instead of reopening the Prefab
    // artifact. It is never a scene-instance or component pointer.
    std::shared_ptr<const PreviewRenderableSnapshot> previewSnapshot;
    // Resident scene resources can be rendered before the entire source
    // topology has finished restoring. Such a frame is displayable but must
    // remain provisional until a later registry revision completes it.
    bool residentPreviewPartial = false;
};

struct EditorThumbnailPreviewReadbackTicket
{
    std::string requestKey;
    // Distinguishes submissions for the same visual/cache identity.
    uint64_t requestRevision = 0u;

    [[nodiscard]] bool IsValid() const
    {
        return !requestKey.empty();
    }
};

struct EditorThumbnailPreviewSubmitResult
{
    EditorThumbnailPreviewResult preview;
    std::optional<EditorThumbnailPreviewReadbackTicket> readbackTicket;
};

struct EditorThumbnailPreviewCompletedReadback
{
    EditorThumbnailPreviewReadbackTicket ticket;
    EditorThumbnailPreviewResult preview;
};

struct EditorThumbnailPreviewResourcePumpResult
{
    bool supported = false;
    bool resourcesPending = false;
    std::string diagnostic;
    uint64_t resourceProgressToken = 0u;
    bool resourceWorkActive = false;
};

struct EditorThumbnailPreviewReuseStats
{
    uint64_t previewSceneUseCount = 0u;
    uint64_t renderTargetAllocationCount = 0u;
    uint64_t renderTargetReuseCount = 0u;
    size_t renderTargetPoolSize = 0u;
};

enum class EditorThumbnailPreviewReadbackPollStatus
{
    Missing,
    Pending,
    Ready,
    Superseded,
    Failed,
    DeviceLost
};

struct EditorThumbnailPreviewReadbackState
{
    bool active = false;
    std::string requestKey;
    uint64_t requestRevision = 0u;
    std::shared_ptr<std::vector<uint8_t>> rgbaPixels;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint64_t expectedSceneDrawCount = 0u;
    uint64_t rawVisibleDrawCount = 0u;
    uint64_t submittedSceneDrawCount = 0u;
    uint64_t objectDataOverflowDroppedObjectCount = 0u;
    std::shared_ptr<const PreviewRenderableSnapshot> previewSnapshot;
    bool residentPreviewPartial = false;
    std::shared_ptr<NLS::Render::RHI::RHICompletionToken> completion;
    std::shared_ptr<NLS::Render::Context::PostSubmitTextureReadbackState> postSubmitTextureReadbackState;
    std::shared_ptr<void> renderInputsKeepAlive;
    AssetThumbnailGpuTexture gpuTexture;
    ThumbnailRequestPriority priority = ThumbnailRequestPriority::Background;
    std::string lastPostSubmitTelemetryState;
};

struct EditorThumbnailPreviewReadbackPollResult
{
    EditorThumbnailPreviewReadbackPollStatus status = EditorThumbnailPreviewReadbackPollStatus::Missing;
    EditorThumbnailPreviewResult preview;
};

EditorThumbnailPreviewReadbackPollResult PollEditorThumbnailPreviewReadback(
    EditorThumbnailPreviewReadbackState& state,
    const std::string& requestKey,
    const NLS::Render::Context::Driver* driver = nullptr,
    uint64_t requestRevision = 0u);

std::string BuildThumbnailPreviewReadbackRequestKey(const AssetThumbnailRequest& request);

NLS::Engine::Serialize::LoadPolicy BuildEditorThumbnailPreviewLoadPolicy();

#if defined(NLS_ENABLE_TEST_HOOKS)
struct EditorThumbnailPreviewCameraDebugInfo
{
    NLS::Maths::Vector3 cameraPosition;
    NLS::Maths::Vector3 lookDirection;
    float distance = 0.0f;
};

EditorThumbnailPreviewCameraDebugInfo BuildPrefabPreviewCameraDebugInfoForTesting(
    const NLS::Maths::Vector3& boundsMin,
    const NLS::Maths::Vector3& boundsMax,
    uint32_t width,
    uint32_t height);
EditorThumbnailPreviewCameraDebugInfo BuildMeshPreviewCameraDebugInfoForTesting(
    const NLS::Maths::Vector3& boundsMin,
    const NLS::Maths::Vector3& boundsMax,
    uint32_t width,
    uint32_t height);

NLS::Maths::Vector3 GetThumbnailPreviewKeyLightDirectionForTesting();
float GetThumbnailPreviewKeyLightIntensityForTesting();
size_t GetThumbnailPreviewKeyLightSampleCountForTesting();
float GetThumbnailPreviewKeyLightAngularRadiusDegreesForTesting();
float GetThumbnailPreviewKeyLightSampleIntensitySumForTesting();
float GetThumbnailPreviewAmbientIntensityForTesting();
size_t GetThumbnailPreviewMeshPumpBudgetForTesting();
size_t GetThumbnailPreviewPrefabMeshRequestStartBudgetForTesting();
size_t GetThumbnailPreviewPrefabMeshPumpBudgetForTesting();
size_t GetThumbnailPreviewMaterialPumpBudgetForTesting();
size_t GetThumbnailPreviewTexturePumpBudgetForTesting();
size_t GetThumbnailPreviewPrefabTexturePumpBudgetForTesting();
size_t GetThumbnailPreviewPrefabResourceInspectionBudgetForTesting();
uint64_t GetThumbnailPreviewPrefabResourcePumpTimeBudgetMicrosForTesting();
bool ShouldYieldPrefabMeshDependencyInspectionForTesting(
    bool meshLoadPending,
    size_t meshRequestStartCount,
    size_t meshRequestStartBudget);
size_t GetThumbnailPreviewPrefabSceneAssemblyBudgetForTesting();
size_t GetThumbnailPreviewPrefabDrawItemCapacityForTesting();
size_t GetThumbnailPreviewPrefabProxyDrawItemCapacityForTesting();
size_t GetThumbnailPreviewPrefabProxyCandidateDrawItemCapacityForTesting();
std::string BuildThumbnailPreviewReadbackRequestKeyForTesting(const AssetThumbnailRequest& request);
std::string BuildThumbnailPreviewSceneAssemblyKeyForTesting(const AssetThumbnailRequest& request);
bool ThumbnailPreviewMeshPathUsesArtifactLoaderForTesting(const std::string& meshPath);
std::string ResolveThumbnailPreviewMeshLoadPathForTesting(
    const AssetThumbnailRequest& request,
    const std::string& meshPath,
    NLS::Core::Assets::AssetId meshAssetId);
struct ThumbnailPreviewDefaultShaderSelectionForTesting
{
    std::string resourcePath;
    std::string sourcePath;
    std::string subAssetKey;
    std::string lightMode;
    bool usesShaderLabStandardPbrForward = false;
    bool usesLegacyBuiltInStandardHlsl = false;
};
ThumbnailPreviewDefaultShaderSelectionForTesting SelectThumbnailPreviewDefaultShaderForTesting(
    NLS::Core::ResourceManagement::ShaderManager& shaderManager);
bool ThumbnailPreviewSnapshotIsCompleteForGpuPrefabPreviewForTesting(
    const PreviewRenderableSnapshot& snapshot);
bool ThumbnailPrefabPreparationUsesResidentSnapshotForTesting(
    const AssetThumbnailRequest& request);
bool ShouldDeferLargePrefabPreviewUntilResidentForTesting(
    size_t drawItemCount,
    bool residentSnapshotUsed);
bool ShouldDeferPrefabPreviewForResourceReadinessForTesting(
    size_t pendingMeshResourceCount,
    size_t pendingMaterialResourceCount,
    size_t pendingMaterialTextureCount,
    bool resourcePlanTruncated);
bool ShouldContinuePrefabPreviewResourceInspectionForTesting(
    size_t phaseIndex,
    size_t inspectedResourceCount,
    bool deadlineExpired);
bool ShouldResetPrefabPreviewPhaseDeadlineForTesting(
    size_t unresolvedPathCount,
    size_t acceptedRequestCount,
    size_t pumpPathCount);
bool ShouldPumpPrefabRuntimeUploadRetirementForTesting(
    size_t explicitPumpPathCount,
    size_t acceptedRequestCount);
bool ShouldRefreshPrefabPreviewTextureInspectionDeadlineAfterSetupForTesting(
    bool materialPhaseComplete,
    size_t previouslyPendingTexturePathCount);
bool ShouldWaitForPrefabPreviewMaterialResourceTableForTesting(
    size_t contentionCount,
    bool allowNewResourceRequests,
    bool sceneResourceResolutionBlocking);
bool ShouldRetainThumbnailPreviewTexturePathForTesting(
    bool headerProbeQueued,
    bool headerProbeInFlight,
    bool deferredArtifactQueued,
    bool artifactInFlight,
    bool uploadInFlight,
    bool resourceReady);
bool ShouldDeferPrefabPreviewAfterDrawPrewarmForTesting(
    bool prewarmSupported,
    bool prewarmComplete);
bool ShouldSkipPrefabPreviewDrawPrewarmForResidentForTesting(
    bool residentSnapshotUsed,
    bool residentResourcesComplete);
bool ShouldRestorePrefabPreviewDrawPrewarmStateForTesting(
    bool savedPreparedAlive,
    uint64_t savedResourcePlanRevision,
    uint64_t currentResourcePlanRevision,
    size_t savedNextDrawPrewarmIndex,
    size_t savedTotalDrawPrewarmCount,
    bool savedDrawPrewarmComplete);
bool CanReusePrefabPreviewSceneAssemblyForTesting(
    const PreviewRenderableSnapshot& previous,
    const PreviewRenderableSnapshot& current);
uint64_t ResolvePrefabPreviewExpectedSceneDrawCountForTesting(
    uint64_t snapshotExpectedDrawItemCount,
    size_t resourcePlanDrawItemCount,
    bool residentPreviewPartial);
bool ShouldPreservePrefabPreviewSceneAfterRenderAttemptForTesting(
    const std::string& diagnostic);
bool BindReadyMaterialPreviewTexturesForTesting(NLS::Render::Resources::Material& material);
std::unique_ptr<NLS::Render::Resources::Material> CreateStablePreviewMaterialForTesting(
    NLS::Render::Resources::Material& source);
struct ThumbnailPreviewRenderStatsForTesting
{
    uint64_t expectedSceneDrawCount = 0u;
    uint64_t rawVisibleDrawCount = 0u;
    uint64_t submittedSceneDrawCount = 0u;
    uint64_t objectDataOverflowDroppedObjectCount = 0u;
};
ThumbnailPreviewRenderStatsForTesting GetLastThumbnailPreviewRenderStatsForTesting();
struct ThumbnailPreviewPrefabResourcePlanForTesting
{
    size_t drawItemCount = 0u;
    size_t uniqueMeshLoadPathCount = 0u;
	size_t uniqueMaterialLoadPathCount = 0u;
	size_t dependencyDrawItemInspectionCount = 0u;
	bool truncatedForPendingResources = false;
	std::vector<size_t> selectedDrawItemIndices;
	NLS::Maths::Vector3 fullWorldBoundsMin {};
	NLS::Maths::Vector3 fullWorldBoundsMax {};
	bool hasFullWorldBounds = false;
};
	ThumbnailPreviewPrefabResourcePlanForTesting BuildThumbnailPreviewPrefabResourcePlanForTesting(
	    const AssetThumbnailRequest& request,
	    const PreviewRenderableSnapshot& snapshot,
	    size_t maxUnreadyDependencyAttempts = SIZE_MAX);
	ThumbnailPreviewPrefabResourcePlanForTesting BuildThumbnailPreviewPrefabResourcePlanWithManagersForTesting(
	    const AssetThumbnailRequest& request,
	    const PreviewRenderableSnapshot& snapshot,
	    NLS::Core::ResourceManagement::MeshManager& meshManager,
	    NLS::Core::ResourceManagement::MaterialManager& materialManager,
	    size_t maxUnreadyDependencyAttempts = SIZE_MAX);
	#endif

class IEditorThumbnailPreviewRenderer
{
public:
    virtual ~IEditorThumbnailPreviewRenderer() = default;

    virtual bool Supports(const AssetThumbnailRequest& request) const = 0;
    virtual EditorThumbnailPreviewResourcePumpResult PumpResources(const AssetThumbnailRequest& request);
    virtual EditorThumbnailPreviewResult Render(const AssetThumbnailRequest& request) = 0;
    virtual EditorThumbnailPreviewSubmitResult SubmitPreview(const AssetThumbnailRequest& request)
    {
        return {Render(request), std::nullopt};
    }
    // The caller completed PumpResources for this request in the same scheduler
    // turn. Renderers that otherwise pump from Render can override this entry
    // point to avoid repeating dependency inspection on the main thread.
    virtual EditorThumbnailPreviewSubmitResult SubmitPreparedPreview(
        const AssetThumbnailRequest& request)
    {
        return SubmitPreview(request);
    }
    virtual std::vector<EditorThumbnailPreviewCompletedReadback> PollCompletedReadbacks(
        size_t maxCount)
    {
        (void)maxCount;
        return {};
    }
    // A completed readback no longer needs one-shot cold-loaded Prefab inputs.
    // Resident scene packages remain registry-owned and are intentionally kept.
    virtual void ReleaseCompletedPreviewResources(const AssetThumbnailRequest& request)
    {
        (void)request;
    }
    // Render()-only implementations use the legacy retry path. Renderers that
    // retire fences independently opt in so the service can leave a pending
    // ticket untouched until PollCompletedReadbacks returns pixels.
    virtual bool SupportsAsynchronousReadbackPolling() const
    {
        return false;
    }
    virtual bool OrphanReadback(const EditorThumbnailPreviewReadbackTicket& ticket)
    {
        (void)ticket;
        return false;
    }
};

class EditorThumbnailPreviewRenderer final : public IEditorThumbnailPreviewRenderer
{
public:
    explicit EditorThumbnailPreviewRenderer(NLS::Render::Context::Driver& driver);
    ~EditorThumbnailPreviewRenderer();

    EditorThumbnailPreviewRenderer(const EditorThumbnailPreviewRenderer&) = delete;
    EditorThumbnailPreviewRenderer& operator=(const EditorThumbnailPreviewRenderer&) = delete;

    bool Supports(const AssetThumbnailRequest& request) const override;
    bool PrewarmMaterialPreviewRenderPath(uint32_t requestedSize);
    EditorThumbnailPreviewResourcePumpResult PumpResources(const AssetThumbnailRequest& request) override;
    EditorThumbnailPreviewResult Render(const AssetThumbnailRequest& request) override;
    EditorThumbnailPreviewSubmitResult SubmitPreview(const AssetThumbnailRequest& request) override;
    EditorThumbnailPreviewSubmitResult SubmitPreparedPreview(
        const AssetThumbnailRequest& request) override;
    std::vector<EditorThumbnailPreviewCompletedReadback> PollCompletedReadbacks(
        size_t maxCount) override;
    void ReleaseCompletedPreviewResources(const AssetThumbnailRequest& request) override;
    bool SupportsAsynchronousReadbackPolling() const override;
    bool OrphanReadback(const EditorThumbnailPreviewReadbackTicket& ticket) override;
    /// Returns lifetime reuse counters for the persistent preview scene and render-target pool.
    [[nodiscard]] EditorThumbnailPreviewReuseStats GetReuseStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

class EditorThumbnailPreviewRendererAdapter final : public IEditorThumbnailPreviewRenderer
{
public:
    explicit EditorThumbnailPreviewRendererAdapter(EditorThumbnailPreviewRenderer& renderer);

    bool Supports(const AssetThumbnailRequest& request) const override;
    EditorThumbnailPreviewResourcePumpResult PumpResources(const AssetThumbnailRequest& request) override;
    EditorThumbnailPreviewResult Render(const AssetThumbnailRequest& request) override;
    EditorThumbnailPreviewSubmitResult SubmitPreview(const AssetThumbnailRequest& request) override;
    EditorThumbnailPreviewSubmitResult SubmitPreparedPreview(
        const AssetThumbnailRequest& request) override;
    std::vector<EditorThumbnailPreviewCompletedReadback> PollCompletedReadbacks(
        size_t maxCount) override;
    void ReleaseCompletedPreviewResources(const AssetThumbnailRequest& request) override;
    bool SupportsAsynchronousReadbackPolling() const override;
    bool OrphanReadback(const EditorThumbnailPreviewReadbackTicket& ticket) override;

private:
    EditorThumbnailPreviewRenderer& m_renderer;
};
}

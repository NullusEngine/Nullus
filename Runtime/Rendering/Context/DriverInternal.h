#pragma once

#include <atomic>
#include <cstddef>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Context/AsyncMeshRuntimeUpload.h"
#include "Rendering/Data/PipelineState.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Tooling/RenderDocCaptureController.h"
#include "Rendering/UI/RHIImGuiOverlayRenderer.h"
#include "Rendering/UI/RHIImGuiTextureRegistry.h"

namespace NLS::Render::RHI
{
    class RHIDevice;
    class RHICompletionToken;
    class PipelineCache;
    struct DescriptorAllocatorStats;
    struct ResourceStateTrackerStats;
}

namespace NLS::Render::Context
{
    struct CompletedReadbackTextureRecord
    {
        std::weak_ptr<Render::RHI::RHITexture> texture;
        uint64_t generation = 0u;
    };

    class DriverImpl
    {
    public:
        struct PendingUiRgba8TextureUpload
        {
            uint64_t requestId = 0u;
            uint32_t width = 0u;
            uint32_t height = 0u;
            std::vector<uint8_t> rgbaPixels;
            std::string debugName;
        };

        struct CompletedUiRgba8TextureUpload
        {
            bool success = false;
            std::shared_ptr<Render::RHI::RHITexture> texture;
            std::shared_ptr<Render::RHI::RHITextureView> textureView;
            uint32_t width = 0u;
            uint32_t height = 0u;
            std::string diagnostic;
        };

        struct RecordedUiRgba8TextureUpload
        {
            uint64_t requestId = 0u;
            std::shared_ptr<Render::RHI::RHITexture> texture;
            std::shared_ptr<Render::RHI::RHITextureView> textureView;
            std::shared_ptr<Render::RHI::RHICompletionToken> completion;
            uint32_t width = 0u;
            uint32_t height = 0u;
            size_t byteCount = 0u;
            std::string debugName;
        };

        struct PendingMeshRuntimeUpload
        {
            uint64_t requestId = 0u;
            MeshRuntimeUploadRequest request;
        };

        struct CompletedMeshRuntimeUpload
        {
            bool success = false;
            std::unique_ptr<Render::Resources::Mesh> mesh;
            std::string diagnostic;
        };

        Render::Settings::EGraphicsBackend requestedGraphicsBackend = Render::Settings::EGraphicsBackend::NONE;
        Render::Data::PipelineState defaultPipelineState;
        Render::Data::PipelineState pipelineState;
        std::shared_ptr<Render::RHI::RHIDevice> explicitDevice;
        std::shared_ptr<Render::RHI::RHISwapchain> explicitSwapchain;
        std::shared_ptr<Render::RHI::PipelineCache> pipelineCache;
        std::vector<Render::RHI::RHIFrameContext> frameContexts;
        std::unique_ptr<Render::Tooling::RenderDocCaptureController> renderDocCaptureController;
        size_t currentFrameIndex = 0u;
        std::vector<std::vector<std::shared_ptr<void>>> retainedThreadedSubmitResourceKeepAliveByFrameContext;
        std::unordered_set<size_t> deferredThreadedFrameScopedRetirementFrameContexts;
        std::unordered_map<size_t, uint64_t> deferredUiTextureRetirementFrameIdsByFrameContext;
        bool explicitFrameActive = false;
        bool uiStandaloneFrameActive = false;
        mutable std::mutex completedReadbackTextureMutex;
        std::shared_ptr<Render::RHI::RHITexture> completedReadbackTexture;
        uint64_t completedReadbackTextureGeneration = 0u;
        uint64_t nextReadbackTextureGeneration = 1u;
        std::vector<CompletedReadbackTextureRecord> submittedReadbackTextureGenerations;
        std::vector<CompletedReadbackTextureRecord> completedReadbackTextureHistory;
        std::vector<PostSubmitBufferReadbackRequest> standalonePostSubmitBufferReadbacks;
        bool hasPendingSwapchainResize = false;
        uint32_t pendingSwapchainWidth = 0u;
        uint32_t pendingSwapchainHeight = 0u;
        std::chrono::steady_clock::time_point lastSwapchainResizeRequestTime{};
        Render::RHI::NativeHandle uiRenderFinishedSemaphore;
        uint64_t uiRenderFinishedValue = 0u;
        mutable std::mutex sceneToUiWaitMutex;
        Render::RHI::NativeHandle sceneToUiWaitSemaphore;
        mutable std::mutex pendingUiOverlaySnapshotMutex;
        std::shared_ptr<const Render::UI::UiDrawDataSnapshot> pendingUiOverlaySnapshot;
        uint64_t pendingUiOverlaySnapshotFrameId = 0u;
        uint64_t latestUiOverlaySnapshotFrameId = 0u;
        uint64_t latestUiOverlayContentSignature = 0u;
        uint64_t pendingUiOverlaySnapshotGeneration = 0u;
        uint64_t uiOverlayPresentationInvalidationGeneration = 1u;
        uint64_t lastPublishedSwapchainUiContentSignature = 0u;
        uint64_t lastPublishedSwapchainUiInvalidationGeneration = 0u;
        uint64_t skippedUnchangedUiOnlyFrameCount = 0u;
        Render::UI::RHIImGuiTextureRegistry uiTextureRegistry;
        Render::UI::RHIImGuiOverlayRenderer uiOverlayRenderer { &uiTextureRegistry };
        mutable std::mutex pendingUiRgba8TextureUploadMutex;
        uint64_t nextUiRgba8TextureUploadRequestId = 1u;
        std::vector<PendingUiRgba8TextureUpload> pendingUiRgba8TextureUploads;
        std::vector<RecordedUiRgba8TextureUpload> recordedUiRgba8TextureUploads;
        std::unordered_map<uint64_t, CompletedUiRgba8TextureUpload> completedUiRgba8TextureUploads;
        std::unordered_set<uint64_t> canceledUiRgba8TextureUploadRequestIds;
        mutable std::mutex pendingMeshRuntimeUploadMutex;
        uint64_t nextMeshRuntimeUploadRequestId = 1u;
        std::vector<PendingMeshRuntimeUpload> pendingMeshRuntimeUploads;
        std::unordered_map<uint64_t, CompletedMeshRuntimeUpload> completedMeshRuntimeUploads;
        std::unordered_set<uint64_t> canceledMeshRuntimeUploadRequestIds;
        std::function<void()> swapchainWillResizeCallback;
        std::unique_ptr<ThreadedRenderingLifecycle> threadedLifecycle;
        bool threadedWorkersRunning = false;
        std::atomic_bool threadedStopRequested{ false };
        std::thread renderSceneWorker;
        std::thread rhiWorker;
        std::mutex threadedWorkerWakeMutex;
        std::condition_variable threadedWorkerWake;
        std::atomic_uint64_t threadedWorkerWakeGeneration{ 0u };
        std::atomic_bool backgroundPreviewPublicationRequested{ false };
        std::timed_mutex threadedRhiSubmissionMutex;
        std::unique_lock<std::timed_mutex> uiStandaloneFrameSubmissionLock;
        std::unique_lock<std::timed_mutex> standaloneFrameSubmissionLock;
        std::atomic_bool uiStandaloneFramePending{ false };
        std::atomic_uint64_t uiStandaloneFramePendingUntilTickNs{ 0u };
        uint64_t nextThreadedFrameId = 1u;
        uint32_t threadedPublishRetirementWaitMs = 0u;
        bool lightGridEnabled = true;
        Render::Settings::EngineDiagnosticsSettings diagnostics;
        mutable std::mutex driverTelemetryMutex;
        uint64_t queueOperationFailureCount = 0u;
        std::string lastQueueOperationFailure;
        uint64_t currentFrameQueueOperationFailureCount = 0u;
        std::string currentFrameLastQueueOperationFailure;
        std::atomic_bool deviceLostDetected{ false };
        std::string deviceLostReason;
        std::atomic_bool unsafeGpuWorkQuarantined{ false };
        std::string unsafeGpuWorkQuarantineReason;
        std::atomic_bool unsafeGpuWorkQuarantineResourcesPreserved{ false };
    };

    namespace Detail
    {
        struct ThreadedQueueSubmissionBatch
        {
            Render::RHI::QueueType queueType = Render::RHI::QueueType::Graphics;
            QueueDependencyPolicy queueDependencyPolicy = QueueDependencyPolicy::Previous;
            std::optional<uint64_t> dependencySourceSubmissionOrder;
            std::vector<uint64_t> queueDependencySourceSubmissionOrders;
            std::vector<uint64_t> crossQueueDependencySourceSubmissionOrders;
            bool requiresDependencyVisibility = false;
            std::vector<std::shared_ptr<Render::RHI::RHICommandPool>> commandPools;
            std::vector<std::shared_ptr<Render::RHI::RHICommandBuffer>> commandBuffers;
            std::vector<std::shared_ptr<Render::RHI::RHICommandBuffer>> childCommandBuffers;
            std::vector<std::shared_ptr<Render::RHI::RHISemaphore>> waitSemaphores;
            std::vector<std::shared_ptr<Render::RHI::RHISemaphore>> signalSemaphores;
        };

        struct AsyncComputeSubmitPlan
        {
            std::vector<ThreadedQueueSubmissionBatch> batches;
            std::vector<std::shared_ptr<Render::RHI::RHISemaphore>> temporarySemaphores;
            std::shared_ptr<Render::RHI::RHISemaphore> lastComputeQueueCompletionSemaphore;
            bool usedDedicatedComputeQueueSubmission = false;
        };

        struct ThreadedCommandSubmissionUnit
        {
            uint64_t submissionOrder = 0u;
            Render::RHI::QueueType queueType = Render::RHI::QueueType::Graphics;
            QueueDependencyPolicy queueDependencyPolicy = QueueDependencyPolicy::Previous;
            std::optional<uint64_t> dependencySourceSubmissionOrder;
            std::vector<uint64_t> queueDependencySourceSubmissionOrders;
            std::vector<WorkUnitDependencyEdge> incomingDependencyEdges;
            bool requiresDependencyVisibility = false;
            std::shared_ptr<Render::RHI::RHICommandPool> commandPool;
            std::shared_ptr<Render::RHI::RHICommandBuffer> commandBuffer;
        };

        inline Render::RHI::RHIFrameContext* GetActiveFrameContext(DriverImpl& impl)
        {
            if (impl.frameContexts.empty() || !impl.explicitFrameActive)
                return nullptr;

            return &impl.frameContexts[impl.currentFrameIndex % impl.frameContexts.size()];
        }

        inline const Render::RHI::RHIFrameContext* GetActiveFrameContext(const DriverImpl& impl)
        {
            if (impl.frameContexts.empty() || !impl.explicitFrameActive)
                return nullptr;

            return &impl.frameContexts[impl.currentFrameIndex % impl.frameContexts.size()];
        }
        
        NLS_RENDER_API bool SupportsThreadedFoundationExecution(const DriverImpl& impl);
        NLS_RENDER_API bool AllowsThreadedHarnessPublish(const DriverImpl& impl);
        NLS_RENDER_API RhiSubmissionFrame SubmitThreadedRhiFrame(
            DriverImpl& impl,
            const RenderScenePackage& renderScenePackage,
            size_t frameContextIndex);
        NLS_RENDER_API uint64_t ResolveAsyncComputeCandidateWorkloadCount(
            const RenderScenePackage& renderScenePackage);
        NLS_RENDER_API bool SupportsOrderedParallelCommandSubmission(const DriverImpl& impl);
        NLS_RENDER_API void NotifyThreadedWorkers(DriverImpl& impl);
        NLS_RENDER_API bool WaitForThreadedWorkerWake(DriverImpl& impl, uint64_t observedGeneration);
        NLS_RENDER_API Render::RHI::RHIFrameContext* BeginThreadedRhiFrame(
            DriverImpl& impl,
            const RenderScenePackage& renderScenePackage,
            size_t frameContextIndex,
            RhiSubmissionFrame* submissionFrame,
            Render::RHI::ResourceStateTrackerStats* preResetTrackerStats,
            Render::RHI::DescriptorAllocatorStats* descriptorStats);
        NLS_RENDER_API void RecordThreadedRhiWork(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
            size_t frameContextIndex,
            AsyncComputeSubmitPlan* submitPlan,
            RhiSubmissionFrame* submissionFrame);
        NLS_RENDER_API bool CompleteThreadedRhiSubmissionTelemetry(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
            const Render::RHI::ResourceStateTrackerStats& preResetTrackerStats,
            Render::RHI::DescriptorAllocatorStats* descriptorStats,
            const AsyncComputeSubmitPlan& submitPlan,
            RhiSubmissionFrame* submissionFrame);
        NLS_RENDER_API void LogCompletedThreadedRhiSubmission(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RhiSubmissionFrame& submissionFrame);
        NLS_RENDER_API void ExecuteThreadedSubmitPlan(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
            AsyncComputeSubmitPlan& submitPlan,
            RhiSubmissionFrame* submissionFrame,
            size_t frameContextIndex);
        NLS_RENDER_API std::shared_ptr<Render::RHI::RHISemaphore> EnsureLastComputeQueueCompletionSemaphore(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            AsyncComputeSubmitPlan& submitPlan);
        NLS_RENDER_API bool ReleaseDeferredThreadedFrameScopedResourcesAfterFence(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            size_t frameContextIndex);
        NLS_RENDER_API void ReleaseDeferredThreadedFrameScopedResourcesAfterFence(DriverImpl& impl);
        NLS_RENDER_API void ReleaseRetiredUiTextureViewsForCompletedFrame(
            DriverImpl& impl,
            uint64_t completedFrameId);
        NLS_RENDER_API void ReleaseRetiredUiTextureViewsForCompletedUiFrame(
            DriverImpl& impl,
            uint64_t fallbackCompletedFrameId);
        NLS_RENDER_API size_t RecordPendingUiRgba8TextureUploads(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            Render::RHI::RHICommandBuffer& commandBuffer);
        NLS_RENDER_API size_t RecordPendingMeshRuntimeUploads(DriverImpl& impl);
        NLS_RENDER_API std::vector<ParallelCommandWorkUnit> BuildParallelCommandWorkUnits(
            const RenderScenePackage& renderScenePackage,
            bool parallelRecordingReady,
            bool parallelTranslationReady,
            bool allowOrderedSlicedSubmission = true,
            bool allowInRenderPassChildCommandRecording = true);
        NLS_RENDER_API std::vector<WorkUnitDependencyEdge> FilterWorkUnitDependencyEdges(
            const ParallelCommandWorkUnit& workUnit,
            const std::function<bool(const WorkUnitDependencyEdge&)>& predicate);
        NLS_RENDER_API std::vector<uint64_t> BuildQueueDependencySourceOrders(
            const std::vector<WorkUnitDependencyEdge>& dependencyEdges);
        NLS_RENDER_API std::optional<uint64_t> ResolveImplicitDependencySourceWorkUnitIndex(
            const ParallelCommandWorkUnit& workUnit);
        NLS_RENDER_API std::optional<uint64_t> ResolveImplicitDependencySourceSubmissionOrder(
            const ParallelCommandWorkUnit& workUnit,
            const std::vector<WorkUnitDependencyEdge>& dependencyEdges);
        NLS_RENDER_API void RememberCompletedReadbackTexture(
            DriverImpl& impl,
            const std::shared_ptr<Render::RHI::RHITexture>& texture,
            uint64_t generation = 0u);
        NLS_RENDER_API bool ResolveImplicitRequiresDependencyVisibility(
            const ParallelCommandWorkUnit& workUnit);
        NLS_RENDER_API RenderPassCommandInput BuildDependencyVisibilityPassInput(
            const ParallelCommandWorkUnit& workUnit,
            const std::vector<WorkUnitDependencyEdge>& dependencyEdges);
        NLS_RENDER_API const char* ToPassDebugName(RenderPassCommandKind kind);
        NLS_RENDER_API const char* ResolvePassProfileScopeName(const RenderPassCommandInput& input);
        NLS_RENDER_API bool IsPassRecordable(
            const RenderScenePackage& package,
            const RenderPassCommandInput& input);
        NLS_RENDER_API void LogSkippedPass(
            const RenderScenePackage& package,
            const RenderPassCommandInput& passInput,
            const char* reason);
        NLS_RENDER_API bool BeginPassCommandPlan(
            Render::RHI::RHICommandBuffer& commandBuffer,
            const std::shared_ptr<Render::RHI::RHITextureView>& swapchainBackbufferView,
            const std::shared_ptr<Render::RHI::RHITextureView>& swapchainDepthStencilView,
            const RenderPassCommandInput& input,
            Render::RHI::RHIFrameContext* frameContext = nullptr);
        NLS_RENDER_API void EndPassCommandPlan(
            Render::RHI::RHICommandBuffer& commandBuffer,
            const RenderPassCommandInput* input = nullptr,
            Render::RHI::RHIFrameContext* frameContext = nullptr);
        NLS_RENDER_API bool HasResourceVisibilityTransitions(const RenderPassCommandInput& input);
        NLS_RENDER_API bool RecordResourceVisibilityTransitions(
            Render::RHI::RHICommandBuffer& commandBuffer,
            const RenderPassCommandInput& input,
            Render::RHI::RHIFrameContext* frameContext = nullptr);
        NLS_RENDER_API uint64_t RecordPreparedDrawCommandsForPass(
            Render::RHI::RHICommandBuffer* commandBuffer,
            const RenderPassCommandInput& input);
        NLS_RENDER_API uint64_t RecordPreparedDrawCommandsForPassRange(
            Render::RHI::RHICommandBuffer* commandBuffer,
            const RenderPassCommandInput& input,
            const std::vector<RecordedDrawCommandInput>& recordedDrawCommands,
            uint64_t recordedDrawBegin,
            uint64_t recordedDrawCount);
        NLS_RENDER_API uint64_t RecordComputeDispatches(
            Render::RHI::RHICommandBuffer& commandBuffer,
            const std::vector<RecordedComputeDispatchInput>& dispatchInputs,
            bool recordShaderReadBarriers = true);
        NLS_RENDER_API void PopulateVisibilityTransitionsFromResourceUsage(
            std::vector<ParallelCommandWorkUnit>& workUnits);
        NLS_RENDER_API RenderScenePreparingResolutionDesc BuildRenderScenePreparingResolutionDesc();
        NLS_RENDER_API AsyncComputeDisposition ResolveThreadedAsyncComputeDisposition(
            DriverImpl& impl,
            const RenderScenePackage& renderScenePackage);
        NLS_RENDER_API void LogThreadedFrameResourceDiagnostics(
            const DriverImpl& impl,
            const Render::RHI::RHIFrameContext& frameContext,
            const RhiSubmissionFrame& submissionFrame);
        NLS_RENDER_API AsyncComputeSubmitPlan BuildAsyncComputeSubmitPlan(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const std::vector<Detail::ThreadedCommandSubmissionUnit>& submissionUnits);
        NLS_RENDER_API bool TryBeginNextRhiFrameExecution(
            ThreadedRenderingLifecycle& lifecycle,
            size_t* slotIndex,
            RenderScenePackage* renderScenePackage);
    }
}

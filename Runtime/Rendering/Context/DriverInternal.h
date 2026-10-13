#pragma once

#include <atomic>
#include <cstddef>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Rendering/Context/FrameSubmissionTelemetry.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/RenderingThreadSystem.h"
#include "Rendering/Context/FrameResourceRing.h"
#include "Rendering/Context/FrameLatencyController.h"
#include "Rendering/Context/SwapchainSceneOutput.h"
#include "Rendering/Context/AsyncMeshRuntimeUpload.h"
#include "Rendering/Context/AsyncTextureRuntimeUpload.h"
#include "Rendering/Data/PipelineState.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Tooling/RenderDocCaptureController.h"
#include "Rendering/UI/RHIImGuiOverlayRenderer.h"
#include "Rendering/UI/RHIImGuiTextureRegistry.h"
#include "TaskGraph/TaskGraph.h"

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
    namespace Detail
    {
        /**
         * Shared state of one asynchronous RHI submission. Defined in
         * RhiCommandListExecutorFrame.cpp; DriverImpl keeps it alive until CPU completion.
         */
        struct ThreadedRhiSubmissionExecution;

        /**
         * One frame's swapchain image acquisition. Defined in
         * RhiCommandListExecutorFrame.cpp; the frame execution keeps it alive from frame
         * begin until the backbuffer is bound or the frame is aborted.
         */
        struct ThreadedSwapchainAcquisition;

        /**
         * Binds the frame's swapchain backbuffer exactly once, at the first point
         * a pass needs it. Returns false when the acquisition failed; the failure
         * is already recorded on the execution so the frame retires as failed.
         */
        NLS_RENDER_API bool ResolveThreadedSwapchainAcquisition(
            const std::shared_ptr<ThreadedRhiSubmissionExecution>& execution);

        /**
         * Queue-operation diagnostics owned by one frame submission.
         *
         * Cumulative driver counters stay on DriverImpl, but the "current frame"
         * counters cannot: once frame N+1 is prepared while frame N is still being
         * submitted, a shared current-frame slot would attribute frame N's present
         * failures to frame N+1. Each execution carries its own telemetry instead.
         */
        struct RhiFrameQueueOperationTelemetry final
        {
            mutable std::mutex mutex;
            uint64_t failureCount = 0u;
            std::string lastFailure;

            void Reset()
            {
                std::lock_guard lock(mutex);
                failureCount = 0u;
                lastFailure.clear();
            }

            void Record(const std::string& message)
            {
                std::lock_guard lock(mutex);
                ++failureCount;
                lastFailure = message;
            }

            void CopyTo(uint64_t& failureCountOut, std::string& lastFailureOut) const
            {
                std::lock_guard lock(mutex);
                failureCountOut = failureCount;
                lastFailureOut = lastFailure;
            }
        };
    }

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
            // Keep debugName before newly added atlas fields so existing
            // aggregate initializers retain standalone-upload semantics.
            std::string debugName;
            Render::RHI::TextureColorSpace colorSpace = Render::RHI::TextureColorSpace::Linear;
            Render::RHI::TextureFormat format = Render::RHI::TextureFormat::RGBA8;
            uint32_t rowPitch = 0u;
            uint32_t slicePitch = 0u;
            std::string atlasPageKey;
            uint32_t atlasPageSize = 0u;
            uint32_t atlasX = 0u;
            uint32_t atlasY = 0u;
        };

        struct UiRgba8TextureAtlasPage
        {
            std::shared_ptr<Render::RHI::RHITexture> texture;
            std::shared_ptr<Render::RHI::RHITextureView> textureView;
            uint32_t pageSize = 0u;
            uint64_t deviceIdentity = 0u;
            Render::RHI::TextureColorSpace colorSpace = Render::RHI::TextureColorSpace::Linear;
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
            std::optional<MeshRuntimeUploadRequest> uploadRequest;
            std::string diagnostic;
        };

        struct PendingTextureRuntimeUpload
        {
            uint64_t requestId = 0u;
            TextureRuntimeUploadRequest request;
        };

        struct RecordedTextureRuntimeUpload
        {
            uint64_t requestId = 0u;
            std::shared_ptr<Render::RHI::RHITexture> texture;
            std::shared_ptr<Render::RHI::RHICompletionToken> completion;
            size_t byteCount = 0u;
            std::string debugName;
        };

        struct CompletedTextureRuntimeUpload
        {
            bool success = false;
            std::shared_ptr<Render::RHI::RHITexture> texture;
            std::string diagnostic;
        };

        Render::Settings::EGraphicsBackend requestedGraphicsBackend = Render::Settings::EGraphicsBackend::NONE;
        Render::Data::PipelineState defaultPipelineState;
        Render::Data::PipelineState pipelineState;
        std::shared_ptr<Render::RHI::RHIDevice> explicitDevice;
        std::shared_ptr<Render::RHI::RHISwapchain> explicitSwapchain;
        std::shared_ptr<Render::RHI::PipelineCache> pipelineCache;
        std::unique_ptr<Render::Tooling::RenderDocCaptureController> renderDocCaptureController;
        std::atomic_uint64_t nextRhiFrameIndex{ 0u };
        // Admission, resize and the RHI lane can retire different slots concurrently.
        mutable std::mutex frameResourceRetirementMutex;
        struct ReservedFrameResourceLease
        {
            size_t slotIndex = 0u;
            uint64_t generation = 0u;
            uint64_t latencyFrameId = 0u;
        };
        std::optional<ReservedFrameResourceLease> reservedFrameResourceLease;
        // Readbacks that threw after possibly queueing work have no retire token.
        std::vector<std::shared_ptr<void>> unfencedReadbackResources;
        mutable std::mutex completedReadbackTextureMutex;
        std::shared_ptr<Render::RHI::RHITexture> completedReadbackTexture;
        uint64_t completedReadbackTextureGeneration = 0u;
        uint64_t nextReadbackTextureGeneration = 1u;
        std::vector<CompletedReadbackTextureRecord> submittedReadbackTextureGenerations;
        std::vector<CompletedReadbackTextureRecord> completedReadbackTextureHistory;
        // Only the render pipe mutates the request; publication polls this flag.
        std::atomic_bool hasPendingSwapchainResize{ false };
        uint32_t pendingSwapchainWidth = 0u;
        uint32_t pendingSwapchainHeight = 0u;
        /**
         * Set when the surface no longer matches the swapchain (Vulkan present
         * returned VK_ERROR_OUT_OF_DATE_KHR) instead of a client resize. The submit
         * lane can only publish this as an atomic flag, because the pending size
         * above is owned by the render thread; the render thread then rebuilds the
         * swapchain at the size the swapchain itself already reports.
         */
        std::atomic_bool pendingSwapchainResizeKeepsCurrentSize{ false };
        std::chrono::steady_clock::time_point lastSwapchainResizeRequestTime{};
        mutable std::mutex uiCompositionSignalMutex;
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
        uint64_t lastQueuedSwapchainUiContentSignature = 0u;
        uint64_t lastQueuedSwapchainUiInvalidationGeneration = 0u;
        uint64_t skippedUnchangedUiOnlyFrameCount = 0u;
        Render::UI::RHIImGuiTextureRegistry uiTextureRegistry;
        Render::UI::RHIImGuiOverlayRenderer uiOverlayRenderer { &uiTextureRegistry };
        SwapchainSceneOutput swapchainSceneOutput;
        mutable std::mutex pendingUiRgba8TextureUploadMutex;
        uint64_t nextUiRgba8TextureUploadRequestId = 1u;
        std::vector<PendingUiRgba8TextureUpload> pendingUiRgba8TextureUploads;
        std::vector<RecordedUiRgba8TextureUpload> recordedUiRgba8TextureUploads;
        std::unordered_map<uint64_t, CompletedUiRgba8TextureUpload> completedUiRgba8TextureUploads;
        std::unordered_set<uint64_t> canceledUiRgba8TextureUploadRequestIds;
        std::unordered_map<std::string, UiRgba8TextureAtlasPage> uiRgba8TextureAtlasPages;
        mutable std::mutex pendingMeshRuntimeUploadMutex;
        uint64_t nextMeshRuntimeUploadRequestId = 1u;
        std::vector<PendingMeshRuntimeUpload> pendingMeshRuntimeUploads;
        std::unordered_map<uint64_t, CompletedMeshRuntimeUpload> completedMeshRuntimeUploads;
        std::unordered_set<uint64_t> canceledMeshRuntimeUploadRequestIds;
        std::atomic_uint64_t meshRuntimeUploadRequestedCount{ 0u };
        std::atomic_uint64_t meshRuntimeUploadRecordTickCount{ 0u };
        std::atomic_uint64_t meshRuntimeUploadRhiIdleTickCount{ 0u };
        std::atomic_uint64_t meshRuntimeUploadRecordedCount{ 0u };
        std::atomic_uint64_t meshRuntimeUploadConsumedCount{ 0u };
        std::atomic_uint64_t meshRuntimeUploadFailedCount{ 0u };
        std::atomic_uint64_t meshRuntimeUploadCanceledCount{ 0u };
        // These values are protected by pendingMeshRuntimeUploadMutex. They
        // make request/record/consume lifetime mismatches diagnosable without
        // logging the mesh payload or taking the upload path out of budget.
        uint64_t meshRuntimeUploadLastRequestedId = 0u;
        uint64_t meshRuntimeUploadLastSwappedBatchCount = 0u;
        uint64_t meshRuntimeUploadEmptyRecordTickCount = 0u;
        uint64_t meshRuntimeUploadLastRecordedId = 0u;
        uint64_t meshRuntimeUploadLastConsumedId = 0u;
        uint64_t meshRuntimeUploadLastCanceledId = 0u;
        mutable std::mutex pendingTextureRuntimeUploadMutex;
        uint64_t nextTextureRuntimeUploadRequestId = 1u;
        std::vector<PendingTextureRuntimeUpload> pendingTextureRuntimeUploads;
        std::vector<RecordedTextureRuntimeUpload> recordedTextureRuntimeUploads;
        std::unordered_map<uint64_t, CompletedTextureRuntimeUpload> completedTextureRuntimeUploads;
        std::unordered_set<uint64_t> canceledTextureRuntimeUploadRequestIds;
        std::atomic_uint64_t textureRuntimeUploadRequestedCount{ 0u };
        std::atomic_uint64_t textureRuntimeUploadRecordedCount{ 0u };
        std::atomic_uint64_t textureRuntimeUploadConsumedCount{ 0u };
        std::atomic_uint64_t textureRuntimeUploadFailedCount{ 0u };
        std::atomic_uint64_t textureRuntimeUploadCanceledCount{ 0u };
        // Clearing a callback must also join an invocation during teardown.
        std::recursive_mutex swapchainResizeCallbackMutex;
        std::function<void()> swapchainWillResizeCallback;
#if defined(NLS_ENABLE_TEST_HOOKS)
        std::function<void(const RenderFramePacket&, const RenderScenePackage&)> renderSceneObserver;
        std::function<void(const RhiSubmissionFrame&)> rhiFrameObserver;
#endif
        std::atomic_uint64_t pendingRenderFrameCommands{ 0u };
        std::atomic_uint64_t pendingSwapchainRenderCommands{ 0u };
        std::atomic_uint64_t pendingUiSnapshotConsumerCommands{ 0u };
        mutable std::mutex frameTelemetryMutex;
        ThreadedFrameTelemetry frameTelemetry;
        std::atomic_uint64_t frameResourceReservationWaitMaxNs{ 0u };
        /**
         * Authoritative GPU-liveness gate for frame slots. A frame context's
         * command pool, allocator and transient resources may only be reset once
         * its fence has signalled, regardless of the CPU-side frame stage.
         */
        std::unique_ptr<FrameResourceRing> frameResourceSlots;
        size_t GetFrameContextCount() const noexcept
        {
            return frameResourceSlots != nullptr ? frameResourceSlots->GetFrameContextCount() : 0u;
        }
        Render::RHI::RHIFrameContext& GetFrameContext(const size_t slotIndex)
        {
            return frameResourceSlots->GetFrameContext(slotIndex);
        }
        const Render::RHI::RHIFrameContext& GetFrameContext(const size_t slotIndex) const
        {
            return frameResourceSlots->GetFrameContext(slotIndex);
        }
        void EnsureFrameContexts(const size_t frameContextCount)
        {
            if (frameResourceSlots == nullptr)
                frameResourceSlots = std::make_unique<FrameResourceRing>(frameContextCount);
            frameResourceSlots->EnsureFrameContexts(frameContextCount);
        }
        /**
         * Game-thread frame-latency budget. Publication waits on this instead of
         * on a wall-clock retirement deadline, so a slow frame no longer decides
         * whether the next frame is dropped.
         */
        std::unique_ptr<FrameLatencyController> frameLatency;
        uint32_t cpuFrameLatencyLimit = 0u;
        /**
         * Lifetime token for the process-wide render-command flush hook.
         *
         * The hook is global while the driver is not, so the hook captures a weak
         * reference to this token and becomes a no-op once the driver is gone,
         * instead of dereferencing a destroyed driver from a later flush.
         */
        struct RenderCommandFlushLifetime final
        {
        };
        std::shared_ptr<RenderCommandFlushLifetime> renderCommandFlushLifetime =
            std::make_shared<RenderCommandFlushLifetime>();
        NLS::Base::TaskGraph::TaskGraph* taskGraph = nullptr;
        std::chrono::milliseconds renderDrainTimeout{ std::chrono::seconds(5) };
        bool threadedWorkersRunning = false;
        std::atomic_bool threadedStopRequested{ false };
        // Requested before capability clamping; kept for logging and for the
        // restart-scoped Launcher setting.
        Render::Settings::RhiThreadModePreference requestedRhiThreadMode =
            Render::Settings::RhiThreadModePreference::BackendDefault;
        // Owns the thread bodies; the OS threads live in RenderingThreadSystem.
        // Held through the interface so the concrete tick can stay private to
        // Driver.cpp while the deleter is still captured with a complete type.
        std::shared_ptr<IRenderingThreadTick> renderingTick;
        // Declared after renderingTick so it is destroyed first: the threads must
        // stop before the tick they call into is released.
        RenderingThreadSystem renderingThreads;
        Render::Settings::RhiThreadMode rhiThreadMode =
            Render::Settings::RhiThreadMode::DedicatedThread;
        std::string rhiThreadModeSummary;
        NLS::Base::TaskGraph::TaskEvent threadedWorkerWake;
        std::atomic_bool backgroundPreviewPublicationRequested{ false };
        /**
         * One-shot guard for the "no threaded pipeline" diagnostic. The condition is
         * a persistent driver state, so reporting it per frame would drown the log.
         */
        bool reportedMissingThreadedPipeline = false;
        std::timed_mutex threadedRhiSubmissionMutex;
        // Serializes one frame's claim/prepare/hand-off window against a swapchain
        // resize, an explicit-device replacement and the synchronous drain. Every
        // producer owns a local scoped lock for the duration of that window; no lock
        // object is stored in the driver, so frames still overlap and no thread can
        // release another thread's ownership. Slot reuse is gated by the frame
        // resource ring's GPU fence, not by this mutex.
        NLS::Base::TaskGraph::TaskHandle pendingIdleMeshUploadTask;
        NLS::Base::TaskGraph::TaskCompletion pendingIdleMeshUploadCompletion;
        uint64_t nextThreadedFrameId = 1u;
        bool lightGridEnabled = true;
        Render::Settings::EngineDiagnosticsSettings diagnostics;
        mutable std::mutex driverTelemetryMutex;
        std::shared_ptr<std::atomic_uint64_t> descriptorAllocationFailureCount =
            std::make_shared<std::atomic_uint64_t>(0u);
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
            /**
             * True when this batch contains work that reads or writes the acquired
             * swapchain image. The image-acquired semaphore has to be waited on by
             * the first such batch: whoever transitions the backbuffer layout is
             * the submit that must not run while the presentation engine still
             * owns the image. Attaching the wait to the first graphics batch was
             * only correct while the offscreen and swapchain work were merged into
             * one submit; once the plan splits them, the split-off swapchain batch
             * submitted without any wait and Vulkan reported
             * SYNC-HAZARD-WRITE-AFTER-PRESENT.
             */
            bool consumesSwapchainImage = false;
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
            std::string failureDiagnostic;
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
            bool targetsSwapchain = false;
            std::shared_ptr<Render::RHI::RHICommandPool> commandPool;
            std::shared_ptr<Render::RHI::RHICommandBuffer> commandBuffer;
        };

        inline Render::RHI::RHIFrameContext* GetActiveFrameContext(DriverImpl& impl)
        {
            const auto slot = ScopedFrameResourceContext::GetSlotIndex(impl);
            if (!slot.has_value() || *slot >= impl.GetFrameContextCount())
                return nullptr;
            return &impl.GetFrameContext(*slot);
        }

        NLS_RENDER_API size_t GetPendingCpuFrameCount(const DriverImpl& impl);
        NLS_RENDER_API bool HasPendingSwapchainFrame(const DriverImpl& impl);
        NLS_RENDER_API std::vector<uint64_t> CollectPendingStreamingDependencyPins(const DriverImpl& impl);

        inline const Render::RHI::RHIFrameContext* GetActiveFrameContext(const DriverImpl& impl)
        {
            const auto slot = ScopedFrameResourceContext::GetSlotIndex(impl);
            if (!slot.has_value() || *slot >= impl.GetFrameContextCount())
                return nullptr;
            return &impl.GetFrameContext(*slot);
        }
        
        NLS_RENDER_API bool SupportsThreadedFoundationExecution(const DriverImpl& impl);
        NLS_RENDER_API RhiSubmissionFrame SubmitThreadedRhiFrame(
            DriverImpl& impl,
            const RenderScenePackage& renderScenePackage,
            size_t frameContextIndex);
        NLS_RENDER_API uint64_t ResolveAsyncComputeCandidateWorkloadCount(
            const RenderScenePackage& renderScenePackage);
        NLS_RENDER_API bool SupportsOrderedParallelCommandSubmission(const DriverImpl& impl);
        NLS_RENDER_API void NotifyThreadedWorkers(DriverImpl& impl);
        NLS_RENDER_API void ReleaseUnsubmittedFrameResourceLease(
            DriverImpl& impl, size_t slotIndex, uint64_t leaseGeneration, uint64_t latencyFrameId = 0u);
        NLS_RENDER_API bool WaitForThreadedWorkerWake(DriverImpl& impl, uint64_t observedGeneration);
        NLS_RENDER_API Render::RHI::RHIFrameContext* BeginThreadedRhiFrame(
            DriverImpl& impl,
            const RenderScenePackage& renderScenePackage,
            size_t frameContextIndex,
            RhiSubmissionFrame* submissionFrame,
            Render::RHI::ResourceStateTrackerStats* preResetTrackerStats,
            Render::RHI::DescriptorAllocatorStats* descriptorStats,
            uint64_t* resourceLeaseGeneration = nullptr,
            const std::shared_ptr<RhiFrameQueueOperationTelemetry>& queueTelemetry = nullptr,
            const std::shared_ptr<ThreadedSwapchainAcquisition>& swapchainAcquisition = nullptr);
        NLS_RENDER_API bool CompleteThreadedRhiSubmissionTelemetry(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
            const Render::RHI::ResourceStateTrackerStats& preResetTrackerStats,
            Render::RHI::DescriptorAllocatorStats* descriptorStats,
            const AsyncComputeSubmitPlan& submitPlan,
            RhiSubmissionFrame* submissionFrame,
            const std::shared_ptr<RhiFrameQueueOperationTelemetry>& queueTelemetry = nullptr);
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
        NLS_RENDER_API size_t RecordPendingUiRgba8TextureUploads(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            Render::RHI::RHICommandBuffer& commandBuffer);
        NLS_RENDER_API size_t RecordPendingMeshRuntimeUploads(
            DriverImpl& impl,
            bool createGpuResources = true);
        NLS_RENDER_API size_t RecordPendingTextureRuntimeUploads(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            Render::RHI::RHICommandBuffer& commandBuffer);
        /**
         * Position of the first work unit in submission order that consumes the
         * acquired swapchain image. `submissionOrderedIndices` indexes `workUnits`
         * in recording order. Everything before the returned position is
         * view-independent and can record while the ordered lane is still
         * draining the previous frame; the acquisition is resolved at the
         * boundary and the trailing swapchain batch records afterwards, so the
         * barrier commit order stays identical to submission order.
         */
        NLS_RENDER_API size_t FindFirstSwapchainDependentWorkUnitPosition(
            const std::vector<ParallelCommandWorkUnit>& workUnits,
            const std::vector<size_t>& submissionOrderedIndices);
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
    }
}

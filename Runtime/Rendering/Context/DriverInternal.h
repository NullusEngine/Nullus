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
#include <vector>

#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Data/PipelineState.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Tooling/RenderDocCaptureController.h"

namespace NLS::Render::RHI
{
    class RHIDevice;
    class PipelineCache;
    struct DescriptorAllocatorStats;
    struct ResourceStateTrackerStats;
}

namespace NLS::Render::Context
{
    class DriverImpl
    {
    public:
        Render::Settings::EGraphicsBackend requestedGraphicsBackend = Render::Settings::EGraphicsBackend::NONE;
        Render::Data::PipelineState defaultPipelineState;
        Render::Data::PipelineState pipelineState;
        std::shared_ptr<Render::RHI::RHIDevice> explicitDevice;
        std::shared_ptr<Render::RHI::RHISwapchain> explicitSwapchain;
        std::shared_ptr<Render::RHI::PipelineCache> pipelineCache;
        std::vector<Render::RHI::RHIFrameContext> frameContexts;
        std::unique_ptr<Render::Tooling::RenderDocCaptureController> renderDocCaptureController;
        size_t currentFrameIndex = 0u;
        bool explicitFrameActive = false;
        bool uiStandaloneFrameActive = false;
        mutable std::mutex completedReadbackTextureMutex;
        std::shared_ptr<Render::RHI::RHITexture> completedReadbackTexture;
        std::vector<std::weak_ptr<Render::RHI::RHITexture>> completedReadbackTextureHistory;
        bool hasPendingSwapchainResize = false;
        uint32_t pendingSwapchainWidth = 0u;
        uint32_t pendingSwapchainHeight = 0u;
        std::chrono::steady_clock::time_point lastSwapchainResizeRequestTime{};
        Render::RHI::NativeHandle uiRenderFinishedSemaphore;
        uint64_t uiRenderFinishedValue = 0u;
        mutable std::mutex sceneToUiWaitMutex;
        Render::RHI::NativeHandle sceneToUiWaitSemaphore;
        std::function<void()> swapchainWillResizeCallback;
        std::unique_ptr<ThreadedRenderingLifecycle> threadedLifecycle;
        bool threadedWorkersRunning = false;
        std::atomic_bool threadedStopRequested{ false };
        std::thread renderSceneWorker;
        std::thread rhiWorker;
        std::mutex threadedWorkerWakeMutex;
        std::condition_variable threadedWorkerWake;
        std::atomic_uint64_t threadedWorkerWakeGeneration{ 0u };
        std::timed_mutex threadedRhiSubmissionMutex;
        std::unique_lock<std::timed_mutex> uiStandaloneFrameSubmissionLock;
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
        bool deviceLostDetected = false;
        std::string deviceLostReason;
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
            std::vector<std::shared_ptr<Render::RHI::RHISemaphore>> waitSemaphores;
            std::vector<std::shared_ptr<Render::RHI::RHISemaphore>> signalSemaphores;
        };

        struct AsyncComputeSubmitPlan
        {
            std::vector<ThreadedQueueSubmissionBatch> batches;
            std::vector<std::shared_ptr<Render::RHI::RHISemaphore>> temporarySemaphores;
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
            const RenderScenePackage& renderScenePackage);
        NLS_RENDER_API uint64_t ResolveAsyncComputeCandidateWorkloadCount(
            const RenderScenePackage& renderScenePackage);
        NLS_RENDER_API bool SupportsOrderedParallelCommandSubmission(const DriverImpl& impl);
        NLS_RENDER_API void NotifyThreadedWorkers(DriverImpl& impl);
        NLS_RENDER_API void WaitForThreadedWorkerWake(DriverImpl& impl, uint64_t observedGeneration);
        NLS_RENDER_API Render::RHI::RHIFrameContext* BeginThreadedRhiFrame(
            DriverImpl& impl,
            const RenderScenePackage& renderScenePackage,
            RhiSubmissionFrame* submissionFrame,
            Render::RHI::ResourceStateTrackerStats* preResetTrackerStats,
            Render::RHI::DescriptorAllocatorStats* descriptorStats);
        NLS_RENDER_API void RecordThreadedRhiWork(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
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
            RhiSubmissionFrame* submissionFrame);
        NLS_RENDER_API std::vector<ParallelCommandWorkUnit> BuildParallelCommandWorkUnits(
            const RenderScenePackage& renderScenePackage,
            bool parallelRecordingReady,
            bool parallelTranslationReady);
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

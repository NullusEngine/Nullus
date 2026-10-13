#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <Debug/Assertion.h>
#include <Debug/Logger.h>

#include "Assets/ArtifactLoadTelemetry.h"
#include "Profiling/PerformanceStageStats.h"
#include "Profiling/Profiler.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverInternal.h"
#include "Rendering/Context/RenderThreadCoordinator.h"
#include "Rendering/Context/RhiCommandListExecutor.h"
#include "Rendering/Context/RhiCommandListRecorder.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHIPipeline.h"
#include "Rendering/RHI/Core/RHISubresourceRangeUtils.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Rendering/RHI/Utils/DescriptorAllocator/DescriptorAllocator.h"
#include "Rendering/RHI/Utils/PipelineCache/PipelineCache.h"
#include "Rendering/RHI/Utils/ResourceStateTracker/ResourceStateTracker.h"
#include "Rendering/RHI/Utils/UploadContext/UploadContext.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Rendering/UI/RHIImGuiOverlayRenderer.h"

namespace NLS::Render::Context
{
    namespace
    {
        constexpr uint64_t kFrameFenceWaitTimeoutNanoseconds = 5'000'000'000ull;
        constexpr uint32_t kMinimumInRenderPassChildSlicesForParallelWorkers = 4u;
        constexpr uint32_t kDx12TextureRowPitchAlignment = 256u;

        class RetainedTextureReadbackCompletion final : public RHI::RHICompletionToken
        {
        public:
            RetainedTextureReadbackCompletion(std::shared_ptr<RHI::RHICompletionToken> completion,
                std::shared_ptr<RHI::RHITexture> texture, std::shared_ptr<void> destination)
                : m_completion(std::move(completion)), m_texture(std::move(texture)),
                  m_destination(std::move(destination))
            {
            }

            std::string_view GetDebugName() const override { return "FrameTextureReadbackCompletion"; }

            RHI::RHICompletionStatus Poll() override
            {
                std::lock_guard lock(m_mutex);
                return Finish(m_completion->Poll());
            }

            RHI::RHICompletionStatus Wait(uint64_t timeoutNanoseconds = 0u) override
            {
                std::lock_guard lock(m_mutex);
                return Finish(m_completion->Wait(timeoutNanoseconds));
            }

        private:
            RHI::RHICompletionStatus Finish(RHI::RHICompletionStatus status)
            {
                // GPU completion alone does not perform a deferred CPU pixel copy.
                if (status.Succeeded())
                {
                    m_destination.reset();
                    m_texture.reset();
                }
                return status;
            }

            std::mutex m_mutex;
            std::shared_ptr<RHI::RHICompletionToken> m_completion;
            std::shared_ptr<RHI::RHITexture> m_texture;
            std::shared_ptr<void> m_destination;
        };

        RHI::RHIReadbackResult RunRenderOrderedReadback(
            DriverImpl& impl, std::function<RHI::RHIReadbackResult()> beginReadback)
        {
            // A render command already owns its FIFO position. The RHI lane must
            // not wait for a render command that may itself be waiting on that lane.
            if (RenderingThreadSystem::IsInRenderingThread() || RenderingThreadSystem::IsInRhiThread())
                return beginReadback();

            RHI::RHIReadbackResult result {
                RHI::RHIReadbackStatusCode::BackendFailure, "readback render command was not executed"
            };
            RenderCommandEvent completion;
            EnqueueRenderCommand<struct BeginTextureReadbackCommand>(
                [beginReadback = std::move(beginReadback), &result] { result = beginReadback(); },
                completion);
            Detail::NotifyThreadedWorkers(impl);
            while (!completion.WaitFor(std::chrono::milliseconds(1)))
            {
                if (!RenderingThreadSystem::HasActiveRenderThread())
                    (void)RenderCommandDispatcher::Dispatch();
                if (impl.taskGraph != nullptr && impl.taskGraph->IsWorkerThread())
                    (void)impl.taskGraph->PumpCurrentThread(1u);
            }
            if (!completion.Succeeded())
                return { RHI::RHIReadbackStatusCode::BackendFailure, completion.GetDiagnostic() };
            return result;
        }

        uint32_t AlignTextureUploadPitch(const uint32_t value)
        {
            return (value + kDx12TextureRowPitchAlignment - 1u) &
                ~(kDx12TextureRowPitchAlignment - 1u);
        }

        struct ScopedArtifactTelemetry
        {
            ::NLS::Core::Assets::ArtifactLoadTelemetryStage stage;
            std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
            size_t byteCount = 0u;
            std::string path;

            ~ScopedArtifactTelemetry()
            {
                ::NLS::Core::Assets::RecordArtifactLoadTelemetry({
                    stage,
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - begin),
                    byteCount,
                    path
                });
            }
        };


        bool IsSwapchainDepthStencilCompatible(
            const Render::RHI::RHIFrameContext& frameContext,
            const uint32_t width,
            const uint32_t height)
        {
            if (frameContext.swapchainDepthStencilTexture == nullptr ||
                frameContext.swapchainDepthStencilView == nullptr)
            {
                return false;
            }

            const auto& desc = frameContext.swapchainDepthStencilTexture->GetDesc();
            return desc.extent.width == width &&
                desc.extent.height == height &&
                desc.extent.depth == 1u &&
                desc.format == Render::RHI::TextureFormat::Depth24Stencil8;
        }

        void EnsureSwapchainDepthStencilAttachment(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const uint32_t width,
            const uint32_t height)
        {
            if (impl.explicitDevice == nullptr || width == 0u || height == 0u)
                return;

            if (IsSwapchainDepthStencilCompatible(frameContext, width, height))
                return;

            Render::RHI::RHITextureDesc textureDesc{};
            textureDesc.extent = { width, height, 1u };
            textureDesc.dimension = Render::RHI::TextureDimension::Texture2D;
            textureDesc.format = Render::RHI::TextureFormat::Depth24Stencil8;
            textureDesc.usage = Render::RHI::TextureUsageFlags::DepthStencilAttachment;
            textureDesc.memoryUsage = Render::RHI::MemoryUsage::GPUOnly;
            textureDesc.optimizedClearValue.enabled = true;
            textureDesc.optimizedClearValue.depth = 1.0f;
            textureDesc.optimizedClearValue.stencil = 0u;
            textureDesc.debugName = "SwapchainSceneDepth";

            frameContext.swapchainDepthStencilTexture = impl.explicitDevice->CreateTexture(textureDesc);

            Render::RHI::RHITextureViewDesc viewDesc{};
            viewDesc.viewType = Render::RHI::TextureViewType::Texture2D;
            viewDesc.format = Render::RHI::TextureFormat::Depth24Stencil8;
            viewDesc.subresourceRange.baseMipLevel = 0u;
            viewDesc.subresourceRange.mipLevelCount = 1u;
            viewDesc.subresourceRange.baseArrayLayer = 0u;
            viewDesc.subresourceRange.arrayLayerCount = 1u;
            viewDesc.debugName = "SwapchainSceneDepthView";
            frameContext.swapchainDepthStencilView =
                impl.explicitDevice->CreateTextureView(frameContext.swapchainDepthStencilTexture, viewDesc);
        }

        RenderPassCommandInput ResolveSwapchainDepthPassInput(
            const RenderPassCommandInput& passInput,
            const Render::RHI::RHIFrameContext& frameContext)
        {
            if (!passInput.targetsSwapchain ||
                !passInput.usesDepthStencilAttachment ||
                passInput.depthStencilAttachmentView != nullptr ||
                frameContext.swapchainDepthStencilView == nullptr)
            {
                return passInput;
            }

            auto resolvedInput = passInput;
            resolvedInput.depthStencilAttachmentView = frameContext.swapchainDepthStencilView;
            return resolvedInput;
        }

        bool AreRenderTargetLayoutsCompatible(
            const Render::RHI::RHIRenderTargetLayoutDesc& lhs,
            const Render::RHI::RHIRenderTargetLayoutDesc& rhs)
        {
            if (lhs.colorFormats != rhs.colorFormats)
                return false;
            for (size_t index = 0u; index < lhs.colorFormats.size(); ++index)
            {
                if (lhs.GetColorSpace(index) != rhs.GetColorSpace(index))
                    return false;
            }
            return
                lhs.hasDepth == rhs.hasDepth &&
                (!lhs.hasDepth || lhs.depthFormat == rhs.depthFormat) &&
                lhs.sampleCount == rhs.sampleCount;
        }

        std::optional<Render::RHI::RHIRenderTargetLayoutDesc> ResolveAttachmentRenderTargetLayout(
            const RenderPassCommandInput& passInput)
        {
            if (passInput.colorAttachmentViews.empty() &&
                passInput.depthStencilAttachmentView == nullptr)
            {
                return std::nullopt;
            }

            Render::RHI::RHIRenderTargetLayoutDesc layout;
            uint32_t sampleCount = 0u;
            layout.colorFormats.reserve(passInput.colorAttachmentViews.size());
            layout.colorSpaces.reserve(passInput.colorAttachmentViews.size());
            for (const auto& colorView : passInput.colorAttachmentViews)
            {
                if (colorView == nullptr)
                    return std::nullopt;

                layout.colorFormats.push_back(colorView->GetDesc().format);
                layout.colorSpaces.push_back(colorView->GetDesc().colorSpace);
                if (colorView->GetTexture() != nullptr)
                    sampleCount = std::max(sampleCount, colorView->GetTexture()->GetDesc().sampleCount);
            }

            if (passInput.depthStencilAttachmentView != nullptr)
            {
                layout.hasDepth = true;
                layout.depthFormat = passInput.depthStencilAttachmentView->GetDesc().format;
                if (passInput.depthStencilAttachmentView->GetTexture() != nullptr)
                {
                    sampleCount = std::max(
                        sampleCount,
                        passInput.depthStencilAttachmentView->GetTexture()->GetDesc().sampleCount);
                }
            }

            if (sampleCount != 0u)
                layout.sampleCount = sampleCount;
            return layout;
        }

        bool AreDrawPipelinesCompatibleWithParentRenderPass(
            const RenderPassCommandInput& parentPassInput,
            const std::vector<RecordedDrawCommandInput>& recordedDrawCommands,
            const uint64_t recordedDrawBegin,
            const uint64_t recordedDrawCount)
        {
            const auto parentLayout = ResolveAttachmentRenderTargetLayout(parentPassInput);
            if (!parentLayout.has_value())
                return true;

            const auto recordedDrawCommandCount = static_cast<uint64_t>(recordedDrawCommands.size());
            if (recordedDrawBegin > recordedDrawCommandCount ||
                recordedDrawCount > recordedDrawCommandCount - recordedDrawBegin)
            {
                return false;
            }

            const uint64_t recordedDrawEnd = recordedDrawBegin + recordedDrawCount;
            for (uint64_t drawIndex = recordedDrawBegin; drawIndex < recordedDrawEnd; ++drawIndex)
            {
                const auto& drawCommand = recordedDrawCommands[static_cast<size_t>(drawIndex)];
                if (drawCommand.pipeline == nullptr)
                    continue;

                if (!AreRenderTargetLayoutsCompatible(
                        *parentLayout,
                        drawCommand.pipeline->GetDesc().renderTargetLayout))
                {
                    return false;
                }
            }
            return true;
        }

        void SeedAcquiredSwapchainBackbufferState(Render::RHI::RHIFrameContext& frameContext)
        {
            if (frameContext.resourceStateTracker == nullptr ||
                frameContext.swapchainBackbufferView == nullptr ||
                frameContext.swapchainBackbufferView->GetTexture() == nullptr)
            {
                return;
            }

            Render::RHI::RHIBarrierDesc barrierDesc;
            barrierDesc.textureBarriers.push_back({
                frameContext.swapchainBackbufferView->GetTexture(),
                Render::RHI::ResourceState::Unknown,
                Render::RHI::ResourceState::Present,
                frameContext.swapchainBackbufferView->GetDesc().subresourceRange,
                Render::RHI::PipelineStageMask::Present,
                Render::RHI::PipelineStageMask::Present,
                Render::RHI::AccessMask::Present,
                Render::RHI::AccessMask::Present
            });
            frameContext.resourceStateTracker->Commit(barrierDesc);
        }


        bool ShouldDeferFrameScopedRetirementAfterSubmit(
            const Render::RHI::RHIQueueOperationResult& submitResult,
            bool submitAttempted);

        bool SubmitMayHaveQueuedGpuWorkWithoutRetirementFence(
            const Render::RHI::RHIQueueOperationResult& submitResult,
            bool submitAttempted);

        void MarkQueuedGpuWorkWithoutRetirementFence(
            DriverImpl& impl,
            const std::string& context,
            const Render::RHI::RHIQueueOperationResult& submitResult,
            const std::shared_ptr<Detail::RhiFrameQueueOperationTelemetry>& queueTelemetry = nullptr);


        uint64_t ResolveSubmittedUiOverlaySnapshotFrameId(
            const RenderScenePackage& renderScenePackage)
        {
            if (!renderScenePackage.hasUIOverlayPass ||
                renderScenePackage.uiDrawDataSnapshot == nullptr)
            {
                return 0u;
            }

            return renderScenePackage.uiDrawDataSnapshot->frameId;
        }


        void PublishSceneToUiWaitSemaphore(DriverImpl& impl, const Render::RHI::RHIFrameContext& frameContext)
        {
            std::lock_guard lock(impl.sceneToUiWaitMutex);
            impl.sceneToUiWaitSemaphore = frameContext.renderFinishedSemaphore != nullptr
                ? frameContext.renderFinishedSemaphore->GetNativeSemaphoreHandle()
                : Render::RHI::NativeHandle{};
        }

        void ClearSceneToUiWaitSemaphoreForFrame(
            DriverImpl& impl,
            const Render::RHI::RHIFrameContext& frameContext)
        {
            if (frameContext.renderFinishedSemaphore == nullptr)
                return;

            // Only the frame slot that has just passed its reusable-frame fence can retire
            // its pending scene->UI wait. Other slots may still need the UI queue wait.
            const auto frameSemaphore = frameContext.renderFinishedSemaphore->GetNativeSemaphoreHandle();
            std::lock_guard lock(impl.sceneToUiWaitMutex);
            if (impl.sceneToUiWaitSemaphore.backend == frameSemaphore.backend &&
                impl.sceneToUiWaitSemaphore.handle == frameSemaphore.handle)
            {
                impl.sceneToUiWaitSemaphore = {};
            }
        }

        void ResetFrameQueueOperationTelemetry(
            DriverImpl& impl,
            const std::shared_ptr<Detail::RhiFrameQueueOperationTelemetry>& queueTelemetry)
        {
            {
                std::lock_guard lock(impl.driverTelemetryMutex);
                impl.currentFrameQueueOperationFailureCount = 0u;
                impl.currentFrameLastQueueOperationFailure.clear();
            }
            if (queueTelemetry != nullptr)
                queueTelemetry->Reset();
        }

        void CopyDriverQueueTelemetryToSubmissionFrame(
            DriverImpl& impl,
            RhiSubmissionFrame& submissionFrame,
            const std::shared_ptr<Detail::RhiFrameQueueOperationTelemetry>& queueTelemetry = nullptr)
        {
            std::lock_guard lock(impl.driverTelemetryMutex);
            submissionFrame.queueOperationFailureCount = impl.queueOperationFailureCount;
            submissionFrame.lastQueueOperationFailure = impl.lastQueueOperationFailure;
            if (queueTelemetry != nullptr)
            {
                queueTelemetry->CopyTo(
                    submissionFrame.currentFrameQueueOperationFailureCount,
                    submissionFrame.currentFrameLastQueueOperationFailure);
            }
            else
            {
                submissionFrame.currentFrameQueueOperationFailureCount =
                    impl.currentFrameQueueOperationFailureCount;
                submissionFrame.currentFrameLastQueueOperationFailure =
                    impl.currentFrameLastQueueOperationFailure;
            }
            submissionFrame.deviceLostDetected = impl.deviceLostDetected.load(std::memory_order_acquire);
            submissionFrame.deviceLostReason = impl.deviceLostReason;
            submissionFrame.unsafeGpuWorkQuarantined =
                impl.unsafeGpuWorkQuarantined.load(std::memory_order_acquire);
            submissionFrame.unsafeGpuWorkQuarantineReason =
                impl.unsafeGpuWorkQuarantineReason;
        }

        void MarkCommandRecordingFailure(
            RhiSubmissionFrame* submissionFrame,
            const char* reason)
        {
            if (submissionFrame == nullptr)
                return;

            ++submissionFrame->commandRecordingFailureCount;
            submissionFrame->lastCommandRecordingFailure = reason != nullptr
                ? reason
                : "command recording failed";
        }

        bool HasCommandRecordingFailure(const RhiSubmissionFrame* submissionFrame)
        {
            return submissionFrame != nullptr &&
                submissionFrame->commandRecordingFailureCount > 0u;
        }

        bool WaitFrameFenceWithDriverPolicy(
            DriverImpl& impl,
            const std::shared_ptr<Render::RHI::RHIFence>& frameFence,
            const std::string& context,
            const std::shared_ptr<Detail::RhiFrameQueueOperationTelemetry>& queueTelemetry = nullptr)
        {
            if (frameFence == nullptr)
                return false;

            {
                NLS_PROFILE_NAMED_SCOPE("RhiCommandListExecutor::CheckFrameFenceSignaled");
                if (frameFence->IsSignaled())
                    return true;
            }

            NLS_PROFILE_NAMED_SCOPE("RhiCommandListExecutor::WaitFrameFenceWithDriverPolicy");
            const bool waited = frameFence->Wait(kFrameFenceWaitTimeoutNanoseconds);
            if (!waited)
            {
                const std::string message =
                    context +
                    " timed out waiting for frame fence after " +
                    std::to_string(kFrameFenceWaitTimeoutNanoseconds) +
                    " ns";
                {
                    std::lock_guard lock(impl.driverTelemetryMutex);
                    ++impl.queueOperationFailureCount;
                    ++impl.currentFrameQueueOperationFailureCount;
                    impl.lastQueueOperationFailure = message;
                    impl.currentFrameLastQueueOperationFailure = message;
                    if (queueTelemetry != nullptr)
                        queueTelemetry->Record(message);
                }
                NLS_LOG_ERROR(message);
            }
            return waited;
        }

        bool RecordQueueOperationResult(
            DriverImpl& impl,
            const Render::RHI::RHIQueueOperationResult& result,
            const std::string& context,
            const std::shared_ptr<Detail::RhiFrameQueueOperationTelemetry>& queueTelemetry = nullptr,
            const bool recoverableSwapchainOutOfDate = false)
        {
            if (result.Succeeded())
                return true;

            // A present whose surface went out of date is not a queue failure. The
            // submission stays valid, the frame retires as presented, and the
            // swapchain is rebuilt at its current size before the next present. Only
            // atomics are touched here: this runs on the submit lane while the render
            // thread owns the pending swapchain size.
            //
            // The same code from an image acquisition is *not* recoverable for that
            // frame - it has no backbuffer to render into - so the caller opts in.
            if (recoverableSwapchainOutOfDate && result.RequiresSwapchainRecreation())
            {
                // A resize that is already queued (a client resize, for example) is
                // going to rebuild the swapchain anyway. Requesting a second rebuild
                // from here would destroy the swapchain again while the next frame is
                // already acquiring from it, so leave the pending request alone.
                if (!impl.hasPendingSwapchainResize.load(std::memory_order_acquire))
                {
                    impl.pendingSwapchainResizeKeepsCurrentSize.store(true, std::memory_order_release);
                    impl.hasPendingSwapchainResize.store(true, std::memory_order_release);
                }
                NLS_LOG_WARNING(
                    context +
                    ": " +
                    (result.message.empty()
                        ? std::string{ "swapchain is out of date" }
                        : result.message) +
                    "; recreating the swapchain at its current size");
                return true;
            }

            const std::string message =
                context +
                " failed: " +
                (result.message.empty()
                    ? std::string{ "queue operation returned failure" }
                    : result.message);

            {
                std::lock_guard lock(impl.driverTelemetryMutex);
                ++impl.queueOperationFailureCount;
                impl.lastQueueOperationFailure = message;
                ++impl.currentFrameQueueOperationFailureCount;
                impl.currentFrameLastQueueOperationFailure = message;
                if (queueTelemetry != nullptr)
                    queueTelemetry->Record(message);
                if (result.code == Render::RHI::RHIQueueOperationStatusCode::DeviceLost)
                {
                    impl.deviceLostDetected.store(true, std::memory_order_release);
                    impl.deviceLostReason = message;
                }
            }

            if (result.code == Render::RHI::RHIQueueOperationStatusCode::DeviceLost && impl.frameLatency != nullptr)
                impl.frameLatency->Stop();
            NLS_LOG_ERROR(message);
            return false;
        }

        bool IsDeviceLostReadbackFailure(const Render::RHI::RHIReadbackResult& result)
        {
            if (result.Succeeded())
                return false;

            return result.code == Render::RHI::RHIReadbackStatusCode::DeviceLost ||
                result.message.find("device removed") != std::string::npos ||
                result.message.find("device removed/lost") != std::string::npos ||
                result.message.find("device is lost") != std::string::npos;
        }

        void MarkReadbackDeviceLostIfNeeded(
            DriverImpl& impl,
            const Render::RHI::RHIReadbackResult& result,
            const std::string& context,
            const std::shared_ptr<Detail::RhiFrameQueueOperationTelemetry>& queueTelemetry = nullptr);

        void BeginPostSubmitBufferReadbacks(
            DriverImpl& impl,
            const RenderScenePackage& renderScenePackage,
            const bool submittedSuccessfully,
            const std::shared_ptr<Render::RHI::RHISemaphore>& lastComputeQueueCompletionSemaphore,
            const bool dedicatedComputeSubmissionUsed,
            const std::shared_ptr<Detail::RhiFrameQueueOperationTelemetry>& queueTelemetry = nullptr)
        {
            if (renderScenePackage.postSubmitBufferReadbacks.empty())
                return;

            for (const auto& request : renderScenePackage.postSubmitBufferReadbacks)
            {
                if (request.state == nullptr)
                    continue;

                {
                    std::lock_guard lock(request.state->mutex);
                    if (request.state->beginAttempted || request.state->beginInProgress)
                        continue;

                    if (!submittedSuccessfully)
                    {
                        request.state->beginAttempted = true;
                        request.state->beginSucceeded = false;
                        request.state->resultCode = Render::RHI::RHIReadbackStatusCode::BackendFailure;
                        request.state->resultMessage = "threaded frame was not submitted successfully";
                        continue;
                    }

					if (impl.explicitDevice == nullptr)
					{
						request.state->beginAttempted = true;
						request.state->beginSucceeded = false;
						request.state->resultCode = Render::RHI::RHIReadbackStatusCode::BackendFailure;
						request.state->resultMessage = "explicit device is unavailable";
						continue;
					}

					if (impl.deviceLostDetected.load(std::memory_order_acquire) ||
						impl.unsafeGpuWorkQuarantined.load(std::memory_order_acquire))
					{
						request.state->beginAttempted = true;
						request.state->beginSucceeded = false;
						request.state->resultCode = Render::RHI::RHIReadbackStatusCode::BackendFailure;
						request.state->resultMessage =
							"post-submit buffer readback rejected because RHI device is lost or GPU work is quarantined";
						continue;
					}

					if (dedicatedComputeSubmissionUsed &&
						request.waitForLastComputeQueueCompletion &&
						lastComputeQueueCompletionSemaphore == nullptr)
                    {
                        request.state->beginAttempted = true;
                        request.state->beginSucceeded = false;
                        request.state->resultCode = Render::RHI::RHIReadbackStatusCode::BackendFailure;
                        request.state->resultMessage = "post-submit readback requires a compute queue completion semaphore";
                        continue;
                    }

                    request.state->beginInProgress = true;
                }

                auto readbackDesc = request.desc;
                if (dedicatedComputeSubmissionUsed &&
                    request.waitForLastComputeQueueCompletion &&
                    lastComputeQueueCompletionSemaphore != nullptr &&
                    std::find(
                        readbackDesc.waitSemaphores.begin(),
                        readbackDesc.waitSemaphores.end(),
                        lastComputeQueueCompletionSemaphore) == readbackDesc.waitSemaphores.end())
                {
                    readbackDesc.waitSemaphores.push_back(lastComputeQueueCompletionSemaphore);
                }
                Render::RHI::RHIReadbackResult result;
                std::string exceptionDiagnostic;
                const auto laneResult = RhiCommandListExecutor::Get().RunOnSubmitLane(
                    "RenderFrame::BeginBufferReadback",
                    [&impl, &readbackDesc, &result, &exceptionDiagnostic]
                    {
                        try
                        {
                            result = impl.explicitDevice->BeginReadBuffer(readbackDesc);
                        }
                        catch (const std::exception& error)
                        {
                            exceptionDiagnostic = error.what();
                            throw;
                        }
                        // Readback errors belong to their request; device loss
                        // is surfaced below and does not strand other waiters.
                        return Render::RHI::RHIQueueOperationResult {};
                    });
                if (!laneResult.Succeeded())
                    result = {Render::RHI::RHIReadbackStatusCode::BackendFailure,
                        exceptionDiagnostic.empty() ? laneResult.message : exceptionDiagnostic};
                {
                    std::lock_guard lock(request.state->mutex);
                    request.state->beginAttempted = true;
                    request.state->beginInProgress = false;
                    request.state->beginSucceeded = result.Succeeded() && result.completion != nullptr;
                    request.state->resultCode = result.code;
                    request.state->resultMessage = result.message;
                    request.state->completion = result.completion;
                }
                MarkReadbackDeviceLostIfNeeded(
                    impl,
                    result,
                    "RhiCommandListExecutor::BeginPostSubmitBufferReadbacks",
                    queueTelemetry);
                if (!laneResult.Succeeded())
                    throw std::runtime_error(result.message);
            }
        }

        void BeginPostSubmitTextureReadbacks(
            DriverImpl& impl,
            const RenderScenePackage& renderScenePackage,
            const Render::RHI::RHIFrameContext& frameContext,
            const bool submittedSuccessfully,
            const std::shared_ptr<Detail::RhiFrameQueueOperationTelemetry>& queueTelemetry = nullptr)
        {
            for (const auto& request : renderScenePackage.postSubmitTextureReadbacks)
            {
                if (request.state == nullptr)
                    continue;

                const auto texture = request.source == TextureReadbackSource::FrameSwapchain
                    ? (renderScenePackage.targetsSwapchain && frameContext.hasAcquiredSwapchainImage &&
                        frameContext.swapchainBackbufferView != nullptr
                        ? frameContext.swapchainBackbufferView->GetTexture() : nullptr)
                    : (request.source == TextureReadbackSource::FrameSceneColor
                        ? frameContext.sceneColorTexture
                        : request.texture);
                {
                    std::lock_guard lock(request.state->mutex);
                    if (request.state->beginAttempted || request.state->beginInProgress)
                        continue;
                    if (!submittedSuccessfully || impl.explicitDevice == nullptr ||
                        texture == nullptr || request.destination == nullptr)
                    {
                        request.state->beginAttempted = true;
                        request.state->beginSucceeded = false;
                        request.state->resultCode = Render::RHI::RHIReadbackStatusCode::BackendFailure;
                        request.state->resultMessage = !submittedSuccessfully
                            ? "threaded frame was not submitted successfully"
                            : "post-submit texture readback input is unavailable";
                        continue;
                    }
                    if (impl.deviceLostDetected.load(std::memory_order_acquire) ||
                        impl.unsafeGpuWorkQuarantined.load(std::memory_order_acquire))
                    {
                        request.state->beginAttempted = true;
                        request.state->beginSucceeded = false;
                        request.state->resultCode = Render::RHI::RHIReadbackStatusCode::BackendFailure;
                        request.state->resultMessage =
                            "post-submit texture readback rejected because RHI device is lost or GPU work is quarantined";
                        continue;
                    }
                    request.state->beginInProgress = true;
                }

                Render::RHI::RHIReadbackResult result;
                std::string exceptionDiagnostic;
                const auto laneResult = RhiCommandListExecutor::Get().RunBackendOperation(
                    "RenderFrame::BeginTextureReadback",
                    [&impl, &request, &texture, &result, &exceptionDiagnostic]
                    {
                        try
                        {
                            result = impl.explicitDevice->BeginReadPixels(
                                texture, request.x, request.y, request.width, request.height,
                                request.format, request.type, request.destination);
                        }
                        catch (const std::exception& error)
                        {
                            exceptionDiagnostic = error.what();
                            throw;
                        }
                        return Render::RHI::RHIQueueOperationResult {};
                    });
                if (!laneResult.Succeeded())
                    result = {Render::RHI::RHIReadbackStatusCode::BackendFailure,
                        exceptionDiagnostic.empty() ? laneResult.message : exceptionDiagnostic};
                {
                    std::lock_guard lock(request.state->mutex);
                    request.state->beginAttempted = true;
                    request.state->beginInProgress = false;
                    if (result.Succeeded() && result.completion != nullptr && request.destinationKeepAlive != nullptr)
                    {
                        result.completion = std::make_shared<RetainedTextureReadbackCompletion>(
                            std::move(result.completion), texture, request.destinationKeepAlive);
                    }
                    request.state->beginSucceeded = result.Succeeded();
                    request.state->resultCode = result.code;
                    request.state->resultMessage = result.message;
                    request.state->completion = result.completion;
                }
                MarkReadbackDeviceLostIfNeeded(
                    impl,
                    result,
                    "RhiCommandListExecutor::BeginPostSubmitTextureReadbacks",
                    queueTelemetry);
                if (!laneResult.Succeeded())
                    throw std::runtime_error(result.message);
            }
        }

        void FailPostSubmitReadbacks(
            const RenderScenePackage& renderScenePackage,
            const char* reason)
        {
            const std::string message = reason != nullptr
                ? reason
                : "threaded frame terminated before post-submit readback";
            const auto failRequests = [&message](const auto& requests)
            {
                for (const auto& request : requests)
                {
                    if (request.state == nullptr)
                        continue;

                    std::lock_guard lock(request.state->mutex);
                    if (request.state->beginAttempted)
                        continue;
                    request.state->beginAttempted = true;
                    request.state->beginInProgress = false;
                    request.state->beginSucceeded = false;
                    request.state->resultCode = Render::RHI::RHIReadbackStatusCode::BackendFailure;
                    request.state->resultMessage = message;
                    request.state->completion.reset();
                }
            };
            failRequests(renderScenePackage.postSubmitBufferReadbacks);
            failRequests(renderScenePackage.postSubmitTextureReadbacks);
        }

        void MarkReadbackDeviceLostIfNeeded(
            DriverImpl& impl,
            const Render::RHI::RHIReadbackResult& result,
            const std::string& context,
            const std::shared_ptr<Detail::RhiFrameQueueOperationTelemetry>& queueTelemetry)
        {
            if (!IsDeviceLostReadbackFailure(result))
                return;

            const std::string message =
                context +
                " failed because the RHI device is lost: " +
                (result.message.empty()
                    ? std::string{ "readback returned device-lost failure" }
                    : result.message);
            {
                std::lock_guard lock(impl.driverTelemetryMutex);
                ++impl.queueOperationFailureCount;
                ++impl.currentFrameQueueOperationFailureCount;
                impl.lastQueueOperationFailure = message;
                impl.currentFrameLastQueueOperationFailure = message;
                if (queueTelemetry != nullptr)
                    queueTelemetry->Record(message);
                impl.deviceLostDetected.store(true, std::memory_order_release);
                impl.deviceLostReason = message;
            }
            if (impl.frameLatency != nullptr)
                impl.frameLatency->Stop();
            auto& executor = RhiCommandListExecutor::Get();
            if (!executor.HasFailed())
                executor.NotifyDeviceLost(message);
            NLS_LOG_ERROR(message);
        }


        void FinalizeThreadedRhiFrameContext(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            Detail::AsyncComputeSubmitPlan& submitPlan,
            const bool deferFrameScopedRetirement = false)
        {
            if (frameContext.commandBuffer != nullptr &&
                frameContext.commandBuffer->IsRecording())
            {
                frameContext.commandBuffer->End();
            }

            if (!deferFrameScopedRetirement && frameContext.descriptorAllocator != nullptr)
            {
                NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::EndDescriptorAllocatorFrame");
                frameContext.descriptorAllocator->EndFrame(frameContext.frameIndex);
            }
            if (!deferFrameScopedRetirement && frameContext.uploadContext != nullptr)
            {
                NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::EndUploadContextFrame");
                frameContext.uploadContext->EndFrame(frameContext.frameIndex);
            }

            {
                NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::RetireFrameResources");
                submitPlan.batches.clear();
                submitPlan.temporarySemaphores.clear();
                Detail::RememberCompletedReadbackTexture(
                    impl,
                    frameContext.explicitReadbackTexture,
                    frameContext.explicitReadbackTextureGeneration);
            }
        }

        void RetainThreadedSubmitPlanResourcesUntilNextFence(
            DriverImpl& impl,
            const size_t frameContextIndex,
            Detail::AsyncComputeSubmitPlan& submitPlan)
        {
            std::lock_guard lock(impl.frameResourceRetirementMutex);
            NLS_ASSERT(impl.frameResourceSlots != nullptr, "Frame resource ring was not initialized");
            auto* slot = impl.frameResourceSlots->PeekSlot(frameContextIndex);
            NLS_ASSERT(slot != nullptr, "Submit resource slot is outside the frame ring");
            auto& keepAlive = slot->ownedResources;
            for (auto& batch : submitPlan.batches)
            {
                for (auto& commandPool : batch.commandPools)
                    keepAlive.push_back(std::move(commandPool));
                for (auto& commandBuffer : batch.commandBuffers)
                    keepAlive.push_back(std::move(commandBuffer));
                for (auto& childCommandBuffer : batch.childCommandBuffers)
                    keepAlive.push_back(std::move(childCommandBuffer));
                for (auto& semaphore : batch.waitSemaphores)
                    keepAlive.push_back(std::move(semaphore));
                for (auto& semaphore : batch.signalSemaphores)
                    keepAlive.push_back(std::move(semaphore));
            }

            for (auto& semaphore : submitPlan.temporarySemaphores)
                keepAlive.push_back(std::move(semaphore));
        }

        bool ShouldDeferFrameScopedRetirementAfterSubmit(
            const Render::RHI::RHIQueueOperationResult& submitResult,
            const bool submitAttempted)
        {
            return submitAttempted && submitResult.frameFenceSignalQueued;
        }

        bool SubmitMayHaveQueuedGpuWorkWithoutRetirementFence(
            const Render::RHI::RHIQueueOperationResult& submitResult,
            const bool submitAttempted)
        {
            return submitAttempted &&
                submitResult.mayHaveQueuedGpuWork &&
                !submitResult.frameFenceSignalQueued;
        }

        void MarkQueuedGpuWorkWithoutRetirementFence(
            DriverImpl& impl,
            const std::string& context,
            const Render::RHI::RHIQueueOperationResult& submitResult,
            const std::shared_ptr<Detail::RhiFrameQueueOperationTelemetry>& queueTelemetry)
        {
            const std::string message =
                context +
                " queued GPU work without a frame retirement fence; quarantining RHI resources" +
                (submitResult.message.empty() ? std::string{} : (": " + submitResult.message));
            {
                std::lock_guard lock(impl.driverTelemetryMutex);
                impl.unsafeGpuWorkQuarantined.store(true, std::memory_order_release);
                impl.unsafeGpuWorkQuarantineReason = message;
                if (submitResult.Succeeded())
                {
                    ++impl.queueOperationFailureCount;
                    ++impl.currentFrameQueueOperationFailureCount;
                    impl.lastQueueOperationFailure = message;
                    impl.currentFrameLastQueueOperationFailure = message;
                    if (queueTelemetry != nullptr)
                        queueTelemetry->Record(message);
                }
            }
            {
                std::lock_guard lock(impl.frameResourceRetirementMutex);
                if (impl.frameResourceSlots != nullptr)
                {
                    for (size_t slotIndex = 0u; slotIndex < impl.frameResourceSlots->GetSlotCount(); ++slotIndex)
                        impl.frameResourceSlots->QuarantineSlot(slotIndex);
                }
            }
            if (impl.frameLatency != nullptr)
                impl.frameLatency->Stop();
            NLS_LOG_ERROR(message);
        }


        bool RequiresExplicitQueueWait(const QueueDependencyPolicy policy)
        {
            return policy != QueueDependencyPolicy::None;
        }

        std::optional<Render::RHI::QueueType> ResolveQueueDependencySourceQueue(
            const QueueDependencyPolicy policy,
            const std::optional<Render::RHI::QueueType> previousQueueType)
        {
            switch (policy)
            {
            case QueueDependencyPolicy::None:
                return std::nullopt;
            case QueueDependencyPolicy::Previous:
                return previousQueueType;
            case QueueDependencyPolicy::LastGraphics:
                return Render::RHI::QueueType::Graphics;
            case QueueDependencyPolicy::LastCompute:
                return Render::RHI::QueueType::Compute;
            default:
                return std::nullopt;
            }
        }

        bool HasWriteAccess(const Render::RHI::AccessMask accessMask)
        {
            constexpr uint32_t kWriteAccessMask =
                static_cast<uint32_t>(Render::RHI::AccessMask::CopyWrite) |
                static_cast<uint32_t>(Render::RHI::AccessMask::ShaderWrite) |
                static_cast<uint32_t>(Render::RHI::AccessMask::ColorAttachmentWrite) |
                static_cast<uint32_t>(Render::RHI::AccessMask::DepthStencilWrite) |
                static_cast<uint32_t>(Render::RHI::AccessMask::HostWrite) |
                static_cast<uint32_t>(Render::RHI::AccessMask::MemoryWrite);
            return (static_cast<uint32_t>(accessMask) & kWriteAccessMask) != 0u;
        }

        std::optional<BufferVisibilityTransition> BuildVisibilityTransitionFromDependencyEdge(
            const WorkUnitDependencyEdge& edge)
        {
            if (edge.resourceKind != ThreadedDependencyResourceKind::Buffer ||
                !edge.sourceBufferAccess.has_value() ||
                !edge.targetBufferAccess.has_value() ||
                edge.targetBufferAccess->buffer == nullptr)
            {
                return std::nullopt;
            }

            BufferVisibilityTransition transition;
            transition.buffer = edge.targetBufferAccess->buffer;
            transition.before = edge.sourceBufferAccess->state;
            transition.after = edge.targetBufferAccess->state;
            transition.sourceStages = edge.sourceBufferAccess->stages;
            transition.destinationStages = edge.targetBufferAccess->stages;
            transition.sourceAccess = edge.sourceBufferAccess->access;
            transition.destinationAccess = edge.targetBufferAccess->access;
            if (transition.before == transition.after &&
                !HasWriteAccess(transition.sourceAccess) &&
                !HasWriteAccess(transition.destinationAccess))
            {
                return std::nullopt;
            }
            return transition;
        }

        std::optional<TextureVisibilityTransition> BuildTextureVisibilityTransitionFromDependencyEdge(
            const WorkUnitDependencyEdge& edge)
        {
            if (edge.resourceKind != ThreadedDependencyResourceKind::Texture ||
                !edge.sourceTextureAccess.has_value() ||
                !edge.targetTextureAccess.has_value() ||
                edge.targetTextureAccess->texture == nullptr)
            {
                return std::nullopt;
            }

            TextureVisibilityTransition transition;
            transition.texture = edge.targetTextureAccess->texture;
            transition.subresourceRange = !Render::RHI::IsEmptySubresourceRange(edge.targetTextureAccess->subresourceRange)
                ? edge.targetTextureAccess->subresourceRange
                : edge.sourceTextureAccess->subresourceRange;
            transition.before = edge.sourceTextureAccess->state;
            transition.after = edge.targetTextureAccess->state;
            transition.sourceStages = edge.sourceTextureAccess->stages;
            transition.destinationStages = edge.targetTextureAccess->stages;
            transition.sourceAccess = edge.sourceTextureAccess->access;
            transition.destinationAccess = edge.targetTextureAccess->access;
            if (transition.before == transition.after &&
                !HasWriteAccess(transition.sourceAccess) &&
                !HasWriteAccess(transition.destinationAccess))
            {
                return std::nullopt;
            }
            return transition;
        }

        bool HasBufferVisibilityTransitionForResource(
            const RenderPassCommandInput& input,
            const std::shared_ptr<Render::RHI::RHIBuffer>& buffer)
        {
            return std::any_of(
                input.bufferVisibilityTransitions.begin(),
                input.bufferVisibilityTransitions.end(),
                [&buffer](const BufferVisibilityTransition& transition)
                {
                    return transition.buffer == buffer;
                });
        }

        bool HasTextureVisibilityTransitionForResource(
            const RenderPassCommandInput& input,
            const std::shared_ptr<Render::RHI::RHITexture>& texture,
            const Render::RHI::RHISubresourceRange& subresourceRange)
        {
            return std::any_of(
                input.textureVisibilityTransitions.begin(),
                input.textureVisibilityTransitions.end(),
                [&texture, &subresourceRange](const TextureVisibilityTransition& transition)
                {
                    if (transition.texture != texture || texture == nullptr)
                        return false;

                    const auto transitionRange = Render::RHI::NormalizeTextureSubresourceRange(
                        texture->GetDesc(),
                        transition.subresourceRange);
                    const auto requestedRange = Render::RHI::NormalizeTextureSubresourceRange(
                        texture->GetDesc(),
                        subresourceRange);
                    return transitionRange.has_value() &&
                        requestedRange.has_value() &&
                        Render::RHI::DoesSubresourceRangeCover(*transitionRange, *requestedRange);
                });
        }

        BufferResourceAccess MakeUiOverlayBufferReadAccess(
            const std::shared_ptr<Render::RHI::RHIBuffer>& buffer,
            const Render::RHI::AccessMask accessMask)
        {
            BufferResourceAccess access;
            access.buffer = buffer;
            access.mode = ResourceAccessMode::Read;
            access.state = Render::RHI::ResourceState::GenericRead;
            access.stages = Render::RHI::PipelineStageMask::VertexInput;
            access.access = accessMask;
            return access;
        }

        TextureResourceAccess MakeUiOverlaySampledTextureAccess(
            const std::shared_ptr<Render::RHI::RHITextureView>& textureView)
        {
            TextureResourceAccess access;
            if (textureView == nullptr)
                return access;

            access.texture = textureView->GetTexture();
            access.subresourceRange = textureView->GetDesc().subresourceRange;
            access.mode = ResourceAccessMode::Read;
            access.state = Render::RHI::ResourceState::ShaderRead;
            access.stages = Render::RHI::PipelineStageMask::FragmentShader;
            access.access = Render::RHI::AccessMask::ShaderRead;
            return access;
        }

        TextureVisibilityTransition MakeUiOverlayFontAtlasUploadToReadTransition(
            const std::shared_ptr<Render::RHI::RHITextureView>& textureView)
        {
            TextureVisibilityTransition transition;
            if (textureView == nullptr)
                return transition;

            transition.texture = textureView->GetTexture();
            transition.subresourceRange = textureView->GetDesc().subresourceRange;
            transition.before = Render::RHI::ResourceState::CopyDst;
            transition.after = Render::RHI::ResourceState::ShaderRead;
            transition.sourceStages = Render::RHI::PipelineStageMask::Copy;
            transition.destinationStages = Render::RHI::PipelineStageMask::FragmentShader;
            transition.sourceAccess = Render::RHI::AccessMask::CopyWrite;
            transition.destinationAccess = Render::RHI::AccessMask::ShaderRead;
            return transition;
        }

        TextureVisibilityTransition MakeUiOverlayTextureReadTransition(
            const std::shared_ptr<Render::RHI::RHITextureView>& textureView)
        {
            TextureVisibilityTransition transition;
            if (textureView == nullptr)
                return transition;

            transition.texture = textureView->GetTexture();
            transition.subresourceRange = textureView->GetDesc().subresourceRange;
            transition.before = Render::RHI::ResourceState::Unknown;
            transition.after = Render::RHI::ResourceState::ShaderRead;
            transition.sourceStages = Render::RHI::PipelineStageMask::None;
            transition.destinationStages = Render::RHI::PipelineStageMask::FragmentShader;
            transition.sourceAccess = Render::RHI::AccessMask::None;
            transition.destinationAccess = Render::RHI::AccessMask::ShaderRead;
            return transition;
        }

        bool IsUiOverlaySwapchainRenderTargetAccess(const TextureResourceAccess& access)
        {
            return access.mode == ResourceAccessMode::Write &&
                access.state == Render::RHI::ResourceState::RenderTarget &&
                access.stages == Render::RHI::PipelineStageMask::RenderTarget &&
                access.access == Render::RHI::AccessMask::ColorAttachmentWrite;
        }

        bool IsUiOverlaySwapchainPresentTransition(const TextureVisibilityTransition& transition)
        {
            return transition.before == Render::RHI::ResourceState::RenderTarget &&
                transition.after == Render::RHI::ResourceState::Present &&
                transition.sourceStages == Render::RHI::PipelineStageMask::RenderTarget &&
                transition.destinationStages == Render::RHI::PipelineStageMask::Present &&
                transition.sourceAccess == Render::RHI::AccessMask::ColorAttachmentWrite &&
                transition.destinationAccess == Render::RHI::AccessMask::Present;
        }

        bool MaterializeUiOverlaySwapchainResourceAccesses(
            RenderPassCommandInput& input,
            const std::shared_ptr<Render::RHI::RHITextureView>& swapchainBackbufferView,
            std::string& errorMessage)
        {
            if (input.kind != RenderPassCommandKind::UIOverlay || !input.targetsSwapchain)
                return true;

            if (swapchainBackbufferView == nullptr || swapchainBackbufferView->GetTexture() == nullptr)
            {
                errorMessage = "UI overlay pass cannot resolve the acquired swapchain backbuffer";
                return false;
            }

            const auto backbufferTexture = swapchainBackbufferView->GetTexture();
            const auto backbufferRange = swapchainBackbufferView->GetDesc().subresourceRange;
            bool foundRenderTargetAccess = false;
            for (auto& access : input.textureResourceAccesses)
            {
                if (!IsUiOverlaySwapchainRenderTargetAccess(access))
                    continue;

                access.texture = backbufferTexture;
                access.subresourceRange = backbufferRange;
                foundRenderTargetAccess = true;
            }

            if (!foundRenderTargetAccess)
            {
                TextureResourceAccess access;
                access.texture = backbufferTexture;
                access.subresourceRange = backbufferRange;
                access.mode = ResourceAccessMode::Write;
                access.state = Render::RHI::ResourceState::RenderTarget;
                access.stages = Render::RHI::PipelineStageMask::RenderTarget;
                access.access = Render::RHI::AccessMask::ColorAttachmentWrite;
                input.textureResourceAccesses.push_back(access);
            }

            bool foundPresentTransition = false;
            for (auto& transition : input.exportedTextureVisibilityTransitions)
            {
                if (!IsUiOverlaySwapchainPresentTransition(transition))
                    continue;

                transition.texture = backbufferTexture;
                transition.subresourceRange = backbufferRange;
                foundPresentTransition = true;
            }

            if (!foundPresentTransition)
            {
                TextureVisibilityTransition transition;
                transition.texture = backbufferTexture;
                transition.subresourceRange = backbufferRange;
                transition.before = Render::RHI::ResourceState::RenderTarget;
                transition.after = Render::RHI::ResourceState::Present;
                transition.sourceStages = Render::RHI::PipelineStageMask::RenderTarget;
                transition.destinationStages = Render::RHI::PipelineStageMask::Present;
                transition.sourceAccess = Render::RHI::AccessMask::ColorAttachmentWrite;
                transition.destinationAccess = Render::RHI::AccessMask::Present;
                input.exportedTextureVisibilityTransitions.push_back(transition);
            }

            return true;
        }

        void MaterializeUiOverlayDependencyEdges(
            std::vector<WorkUnitDependencyEdge>& edges,
            const RenderPassCommandInput& materializedUiOverlayInput)
        {
            if (materializedUiOverlayInput.kind != RenderPassCommandKind::UIOverlay)
                return;

            const auto accessIt = std::find_if(
                materializedUiOverlayInput.textureResourceAccesses.begin(),
                materializedUiOverlayInput.textureResourceAccesses.end(),
                [](const TextureResourceAccess& access)
                {
                    return access.texture != nullptr &&
                        IsUiOverlaySwapchainRenderTargetAccess(access);
                });
            if (accessIt == materializedUiOverlayInput.textureResourceAccesses.end())
                return;

            for (auto& edge : edges)
            {
                if (edge.resourceKind != ThreadedDependencyResourceKind::Texture)
                    continue;

                if (edge.targetTextureAccess.has_value() &&
                    edge.targetTextureAccess->texture == nullptr &&
                    IsUiOverlaySwapchainRenderTargetAccess(*edge.targetTextureAccess))
                {
                    edge.targetTextureAccess = *accessIt;
                }

                if (edge.sourceTextureAccess.has_value() &&
                    edge.sourceTextureAccess->texture == nullptr &&
                    IsUiOverlaySwapchainRenderTargetAccess(*edge.sourceTextureAccess))
                {
                    edge.sourceTextureAccess = *accessIt;
                }
            }
        }

        void AppendUiOverlayPreparedResourceAccesses(
            RenderPassCommandInput& input,
            const Render::UI::RHIImGuiOverlayPreparedResourceSnapshot& resources)
        {
            if (resources.vertexBuffer != nullptr)
            {
                input.bufferResourceAccesses.push_back(MakeUiOverlayBufferReadAccess(
                    resources.vertexBuffer,
                    Render::RHI::AccessMask::VertexRead));
            }
            if (resources.indexBuffer != nullptr)
            {
                input.bufferResourceAccesses.push_back(MakeUiOverlayBufferReadAccess(
                    resources.indexBuffer,
                    Render::RHI::AccessMask::IndexRead));
            }

            if (resources.fontAtlasTextureView != nullptr)
            {
                const auto fontAccess = MakeUiOverlaySampledTextureAccess(resources.fontAtlasTextureView);
                if (fontAccess.texture != nullptr)
                {
                    input.textureResourceAccesses.push_back(fontAccess);
                    if (resources.fontAtlasUploadTransitionRequired)
                    {
                        input.textureVisibilityTransitions.push_back(
                            MakeUiOverlayFontAtlasUploadToReadTransition(resources.fontAtlasTextureView));
                    }
                }
            }

            for (const auto& textureView : resources.registeredTextureViews)
            {
                const auto textureAccess = MakeUiOverlaySampledTextureAccess(textureView);
                if (textureAccess.texture == nullptr)
                    continue;

                const auto alreadyDeclared = std::any_of(
                    input.textureResourceAccesses.begin(),
                    input.textureResourceAccesses.end(),
                    [&textureAccess](const TextureResourceAccess& existing)
                    {
                        return existing.texture == textureAccess.texture &&
                            existing.subresourceRange.baseMipLevel == textureAccess.subresourceRange.baseMipLevel &&
                            existing.subresourceRange.mipLevelCount == textureAccess.subresourceRange.mipLevelCount &&
                            existing.subresourceRange.baseArrayLayer == textureAccess.subresourceRange.baseArrayLayer &&
                            existing.subresourceRange.arrayLayerCount == textureAccess.subresourceRange.arrayLayerCount &&
                            existing.mode == textureAccess.mode &&
                            existing.state == textureAccess.state;
                    });
                if (!alreadyDeclared)
                    input.textureResourceAccesses.push_back(textureAccess);

                input.textureVisibilityTransitions.push_back(
                    MakeUiOverlayTextureReadTransition(textureView));
            }
        }

        std::optional<RenderPassCommandInput> PrepareUiOverlayPassInput(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            Render::RHI::RHICommandBuffer& commandBuffer,
            RenderPassCommandInput passInput,
            const size_t frameContextIndex,
            std::string& errorMessage)
        {
            if (passInput.kind != RenderPassCommandKind::UIOverlay)
                return passInput;

            if (passInput.uiDrawDataSnapshot == nullptr)
            {
                errorMessage = "UI overlay pass is missing a draw-data snapshot";
                return std::nullopt;
            }

            if (!MaterializeUiOverlaySwapchainResourceAccesses(
                    passInput,
                    frameContext.swapchainBackbufferView,
                    errorMessage))
            {
                return std::nullopt;
            }

            (void)Detail::RecordPendingUiRgba8TextureUploads(impl, frameContext, commandBuffer);
            (void)Detail::RecordPendingTextureRuntimeUploads(impl, frameContext, commandBuffer);

            impl.uiOverlayRenderer.SetTextureRegistry(&impl.uiTextureRegistry);
            const auto prepareResult = impl.uiOverlayRenderer.PrepareFrameResources(
                *impl.explicitDevice,
                commandBuffer,
                *passInput.uiDrawDataSnapshot,
                frameContextIndex);
            if (!prepareResult.success)
            {
                errorMessage = prepareResult.message.empty()
                    ? "UI overlay resource preparation failed"
                    : prepareResult.message;
                return std::nullopt;
            }

            AppendUiOverlayPreparedResourceAccesses(
                passInput,
                impl.uiOverlayRenderer.GetPreparedResourceSnapshot(
                    *passInput.uiDrawDataSnapshot,
                    frameContextIndex));
            return passInput;
        }

        bool UsesImplicitDependencySourceIndex(const ParallelCommandWorkUnit& workUnit)
        {
            return workUnit.incomingDependencyEdges.empty() &&
                workUnit.commandInput.dependencySourceWorkUnitIndex.has_value();
        }

        const char* ToThreadedPassDebugName(const RenderPassCommandKind kind)
        {
            switch (kind)
            {
            case RenderPassCommandKind::Opaque:
                return "ThreadedOpaquePass";
            case RenderPassCommandKind::Decal:
                return "ThreadedDecalPass";
            case RenderPassCommandKind::Transparent:
                return "ThreadedTransparentPass";
            case RenderPassCommandKind::Skybox:
                return "ThreadedSkyboxPass";
            case RenderPassCommandKind::Helper:
                return "ThreadedHelperPass";
            case RenderPassCommandKind::GBuffer:
                return "ThreadedGBufferPass";
            case RenderPassCommandKind::Lighting:
                return "ThreadedLightingPass";
            case RenderPassCommandKind::Compute:
                return "ThreadedComputePass";
            case RenderPassCommandKind::UIOverlay:
                return kUIOverlayRenderPassDebugName;
            default:
                return "ThreadedUnknownPass";
            }
        }

        Render::RHI::QueueType ResolveEffectiveQueueTypeForPass(const RenderPassCommandInput& input)
        {
            if (input.kind == RenderPassCommandKind::Compute &&
                input.queueType == Render::RHI::QueueType::Graphics)
            {
                return Render::RHI::QueueType::Compute;
            }

            return input.queueType;
        }

    }

    uint64_t Detail::ResolveAsyncComputeCandidateWorkloadCount(
        const RenderScenePackage& renderScenePackage)
    {
        const auto isComputeQueuePass = [](const RenderPassCommandInput& input)
        {
            return input.queueType == Render::RHI::QueueType::Compute ||
                input.kind == RenderPassCommandKind::Compute;
        };

        uint64_t asyncComputeWorkloadCount = renderScenePackage.asyncComputeWorkloadCount;
        if (asyncComputeWorkloadCount == 0u && renderScenePackage.containsParallelCommandWorkUnits)
        {
            asyncComputeWorkloadCount = static_cast<uint64_t>(std::count_if(
                renderScenePackage.parallelCommandWorkUnits.begin(),
                renderScenePackage.parallelCommandWorkUnits.end(),
                [&isComputeQueuePass](const ParallelCommandWorkUnit& workUnit)
                {
                    return isComputeQueuePass(workUnit.commandInput);
                }));
        }

        return asyncComputeWorkloadCount;
    }

    bool Detail::SupportsOrderedParallelCommandSubmission(const DriverImpl& impl)
    {
        if (impl.explicitDevice == nullptr)
            return false;

        const auto nativeDeviceInfo = impl.explicitDevice->GetNativeDeviceInfo();
        return Render::Settings::SupportsOrderedParallelCommandSubmissionPath(
            nativeDeviceInfo.backend,
            impl.explicitDevice->GetCapabilities());
    }

    namespace
    {
        bool ShouldUseOrderedParallelCommandSubmissionForPackage(
            const DriverImpl& impl,
            const RenderScenePackage& renderScenePackage)
        {
            if (!Detail::SupportsOrderedParallelCommandSubmission(impl))
                return false;

            return true;
        }
    }

    AsyncComputeDisposition Detail::ResolveThreadedAsyncComputeDisposition(
        DriverImpl& impl,
        const RenderScenePackage& renderScenePackage)
    {
        const uint64_t asyncComputeWorkloadCount =
            ResolveAsyncComputeCandidateWorkloadCount(renderScenePackage);

        if (!renderScenePackage.hasAsyncComputeWorkload || asyncComputeWorkloadCount == 0u)
            return AsyncComputeDisposition::DisabledNoEligibleWorkload;

        if (impl.explicitDevice == nullptr)
            return AsyncComputeDisposition::DisabledNoCapability;

        const auto capabilities = impl.explicitDevice->GetCapabilities();
        if (!Render::Settings::SupportsAsyncComputeFoundation(capabilities))
            return AsyncComputeDisposition::DisabledNoCapability;

        auto graphicsQueue = impl.explicitDevice->GetQueue(Render::RHI::QueueType::Graphics);
        auto computeQueue = impl.explicitDevice->GetQueue(Render::RHI::QueueType::Compute);
        if (computeQueue == nullptr || graphicsQueue == nullptr || computeQueue.get() == graphicsQueue.get())
            return AsyncComputeDisposition::DisabledNoComputeQueue;

        return AsyncComputeDisposition::ReadyButNotScheduled;
    }

    void Detail::LogThreadedFrameResourceDiagnostics(
        const DriverImpl& impl,
        const Render::RHI::RHIFrameContext& frameContext,
        const RhiSubmissionFrame& submissionFrame)
    {
        if (!Render::Settings::GetThreadDiagnosticsSettings().logRenderDrawPath)
            return;

        std::string message = "[Driver] Frame resource stats frameId=" + std::to_string(submissionFrame.frameId) +
            " workUnits=" + std::to_string(submissionFrame.recordedWorkUnitCount) +
            " parallelWorkers=" + std::to_string(submissionFrame.parallelRecordingWorkerCount) +
            " translatedWorkUnits=" + std::to_string(submissionFrame.translatedWorkUnitCount) +
            " usedParallelPath=" + std::to_string(submissionFrame.usedParallelCommandPath ? 1 : 0) +
            " usedTranslationMerge=" + std::to_string(submissionFrame.usedTranslationMerge ? 1 : 0) +
            " usedSerialPath=" + std::to_string(submissionFrame.usedSerialCommandPath ? 1 : 0) +
            " externalOutput=" + std::to_string(submissionFrame.usedExternalOutputBridge ? 1 : 0) +
            " externalTextures=" + std::to_string(submissionFrame.externalOutputTextureCount) +
            " asyncCandidates=" + std::to_string(submissionFrame.asyncComputeCandidateWorkloadCount) +
            " asyncQueueAvailable=" + std::to_string(submissionFrame.asyncComputeQueueAvailable ? 1 : 0) +
            " asyncQueueSubmitted=" + std::to_string(submissionFrame.usedAsyncComputeQueueSubmission ? 1 : 0);

        if (frameContext.resourceStateTracker != nullptr)
        {
            message += " tracker(registeredTextures=" + std::to_string(submissionFrame.transientTextureRegistrationCount) +
                ", registeredBuffers=" + std::to_string(submissionFrame.transientBufferRegistrationCount) +
                ", retiredTextures=" + std::to_string(submissionFrame.retiredTransientTextureCount) +
                ", retiredBuffers=" + std::to_string(submissionFrame.retiredTransientBufferCount) + ")";
        }

        if (frameContext.descriptorAllocator != nullptr)
        {
            message += " descriptors(transientUsed=" + std::to_string(submissionFrame.descriptorTransientUsed) +
                ", transientPeak=" + std::to_string(submissionFrame.descriptorTransientPeak) +
                ", transientRetired=" + std::to_string(submissionFrame.descriptorTransientRetired) +
                ", persistentUsed=" + std::to_string(submissionFrame.descriptorPersistentUsed) +
                ", persistentReleased=" + std::to_string(submissionFrame.descriptorPersistentReleased) +
                ", allocFailures=" + std::to_string(submissionFrame.descriptorAllocationFailures) + ")";
        }

        if (impl.pipelineCache != nullptr)
        {
            const auto stats = impl.pipelineCache->GetStats();
            message += " pipelineCache(hits=" + std::to_string(stats.graphicsHits) +
                ", misses=" + std::to_string(stats.graphicsMisses) +
                ", stores=" + std::to_string(stats.graphicsStores) +
                ", entries=" + std::to_string(stats.graphicsEntryCount) +
                ", computeHits=" + std::to_string(stats.computeHits) +
                ", computeMisses=" + std::to_string(stats.computeMisses) +
                ", computeStores=" + std::to_string(stats.computeStores) +
                ", computeEntries=" + std::to_string(stats.computeEntryCount) +
                ", prewarmGfx=" + std::to_string(stats.graphicsPrewarmRequests) +
                ", prewarmComp=" + std::to_string(stats.computePrewarmRequests) +
                ", prewarmGfxHits=" + std::to_string(stats.graphicsPrewarmHits) +
                ", prewarmCompHits=" + std::to_string(stats.computePrewarmHits) + ")";
        }

        if (!submissionFrame.parallelFallbackReason.empty())
        {
            message += " parallelFallback=\"" + submissionFrame.parallelFallbackReason + "\"";
        }

        NLS_LOG_INFO(message.c_str());
    }

    Detail::AsyncComputeSubmitPlan Detail::BuildAsyncComputeSubmitPlan(
        DriverImpl& impl,
        Render::RHI::RHIFrameContext& frameContext,
        const std::vector<Detail::ThreadedCommandSubmissionUnit>& submissionUnits)
    {
        AsyncComputeSubmitPlan submitPlan;
        if (impl.explicitDevice == nullptr)
            return submitPlan;

        const auto capabilities = impl.explicitDevice->GetCapabilities();
        auto graphicsQueue = impl.explicitDevice->GetQueue(Render::RHI::QueueType::Graphics);
        auto computeQueue = impl.explicitDevice->GetQueue(Render::RHI::QueueType::Compute);
        const bool canUseDedicatedAsyncCompute =
            Render::Settings::SupportsAsyncComputeFoundation(capabilities) &&
            graphicsQueue != nullptr &&
            computeQueue != nullptr &&
            graphicsQueue.get() != computeQueue.get();

        std::unordered_map<uint64_t, Render::RHI::QueueType> executionQueueTypeBySubmissionOrder;
        executionQueueTypeBySubmissionOrder.reserve(submissionUnits.size());
        for (const auto& submissionUnit : submissionUnits)
        {
            const auto executionQueueType =
                canUseDedicatedAsyncCompute && submissionUnit.queueType == Render::RHI::QueueType::Compute
                    ? Render::RHI::QueueType::Compute
                    : Render::RHI::QueueType::Graphics;
            executionQueueTypeBySubmissionOrder.insert_or_assign(
                submissionUnit.submissionOrder,
                executionQueueType);
        }

        const auto resolveCrossQueueDependencySourceSubmissionOrders =
            [&](const Detail::ThreadedCommandSubmissionUnit& submissionUnit, const Render::RHI::QueueType executionQueueType)
        {
            std::vector<uint64_t> sourceOrders;
            const auto appendIfCrossQueue = [&](const uint64_t sourceSubmissionOrder)
            {
                const auto sourceQueueIt =
                    executionQueueTypeBySubmissionOrder.find(sourceSubmissionOrder);
                if (sourceQueueIt == executionQueueTypeBySubmissionOrder.end() ||
                    sourceQueueIt->second == executionQueueType)
                {
                    return;
                }

                if (std::find(sourceOrders.begin(), sourceOrders.end(), sourceSubmissionOrder) ==
                    sourceOrders.end())
                {
                    sourceOrders.push_back(sourceSubmissionOrder);
                }
            };

            if (!submissionUnit.queueDependencySourceSubmissionOrders.empty())
            {
                for (const auto sourceSubmissionOrder :
                    submissionUnit.queueDependencySourceSubmissionOrders)
                {
                    appendIfCrossQueue(sourceSubmissionOrder);
                }
            }
            else if (submissionUnit.dependencySourceSubmissionOrder.has_value())
            {
                appendIfCrossQueue(*submissionUnit.dependencySourceSubmissionOrder);
            }

            std::sort(sourceOrders.begin(), sourceOrders.end());
            return sourceOrders;
        };

        bool sawComputeBatch = false;
        auto appendBatchIfNeeded = [&](
            const Render::RHI::QueueType queueType,
            const QueueDependencyPolicy queueDependencyPolicy,
            const std::optional<uint64_t>& dependencySourceSubmissionOrder,
            const std::vector<uint64_t>& queueDependencySourceSubmissionOrders,
            const std::vector<uint64_t>& crossQueueDependencySourceSubmissionOrders,
            const bool requiresDependencyVisibility)
        {
            if (!submitPlan.batches.empty() &&
                submitPlan.batches.back().queueType == queueType &&
                submitPlan.batches.back().crossQueueDependencySourceSubmissionOrders ==
                    crossQueueDependencySourceSubmissionOrders)
            {
                return;
            }

            ThreadedQueueSubmissionBatch batch;
            batch.queueType = queueType;
            batch.queueDependencyPolicy = queueDependencyPolicy;
            batch.dependencySourceSubmissionOrder = dependencySourceSubmissionOrder;
            batch.queueDependencySourceSubmissionOrders = queueDependencySourceSubmissionOrders;
            batch.crossQueueDependencySourceSubmissionOrders =
                crossQueueDependencySourceSubmissionOrders;
            batch.requiresDependencyVisibility = requiresDependencyVisibility;
            submitPlan.batches.push_back(std::move(batch));
        };

        std::unordered_map<uint64_t, size_t> batchIndexBySubmissionOrder;

        for (const auto& submissionUnit : submissionUnits)
        {
            if (submissionUnit.commandBuffer == nullptr)
                continue;

            const auto executionQueueType =
                canUseDedicatedAsyncCompute && submissionUnit.queueType == Render::RHI::QueueType::Compute
                    ? Render::RHI::QueueType::Compute
                    : Render::RHI::QueueType::Graphics;
            const auto crossQueueDependencySourceSubmissionOrders =
                resolveCrossQueueDependencySourceSubmissionOrders(
                    submissionUnit,
                    executionQueueType);
            appendBatchIfNeeded(
                executionQueueType,
                submissionUnit.queueDependencyPolicy,
                submissionUnit.dependencySourceSubmissionOrder,
                submissionUnit.queueDependencySourceSubmissionOrders,
                crossQueueDependencySourceSubmissionOrders,
                submissionUnit.requiresDependencyVisibility);
            auto& batch = submitPlan.batches.back();
            batch.consumesSwapchainImage =
                batch.consumesSwapchainImage || submissionUnit.targetsSwapchain;
            if (submissionUnit.commandPool != nullptr)
                batch.commandPools.push_back(submissionUnit.commandPool);
            batch.commandBuffers.push_back(submissionUnit.commandBuffer);
            batchIndexBySubmissionOrder[submissionUnit.submissionOrder] =
                submitPlan.batches.size() - 1u;
            sawComputeBatch = sawComputeBatch || executionQueueType == Render::RHI::QueueType::Compute;
        }

        std::array<std::optional<size_t>, 3> lastBatchByQueueType{};
        size_t handoffSemaphoreIndex = 0u;
        for (size_t batchIndex = 0u; batchIndex < submitPlan.batches.size(); ++batchIndex)
        {
            auto& currentBatch = submitPlan.batches[batchIndex];
            const auto currentQueueIndex = static_cast<size_t>(currentBatch.queueType);
            const std::optional<Render::RHI::QueueType> previousQueueType =
                batchIndex > 0u
                    ? std::optional<Render::RHI::QueueType>(submitPlan.batches[batchIndex - 1u].queueType)
                    : std::nullopt;
            std::vector<size_t> dependencySourceIndices;
            if (!currentBatch.queueDependencySourceSubmissionOrders.empty())
            {
                for (const auto sourceSubmissionOrder : currentBatch.queueDependencySourceSubmissionOrders)
                {
                    const auto sourceBatchIt = batchIndexBySubmissionOrder.find(sourceSubmissionOrder);
                    if (sourceBatchIt == batchIndexBySubmissionOrder.end())
                        continue;

                    if (std::find(
                        dependencySourceIndices.begin(),
                        dependencySourceIndices.end(),
                        sourceBatchIt->second) == dependencySourceIndices.end())
                    {
                        dependencySourceIndices.push_back(sourceBatchIt->second);
                    }
                }
            }
            else
            {
                std::optional<size_t> dependencySourceIndex;
                if (currentBatch.dependencySourceSubmissionOrder.has_value())
                {
                    const auto sourceBatchIt =
                        batchIndexBySubmissionOrder.find(*currentBatch.dependencySourceSubmissionOrder);
                    if (sourceBatchIt != batchIndexBySubmissionOrder.end())
                        dependencySourceIndex = sourceBatchIt->second;
                }
                else
                {
                    const auto dependencySourceQueue = ResolveQueueDependencySourceQueue(
                        currentBatch.queueDependencyPolicy,
                        previousQueueType);
                    if (dependencySourceQueue.has_value())
                        dependencySourceIndex =
                            lastBatchByQueueType[static_cast<size_t>(*dependencySourceQueue)];
                }

                if (dependencySourceIndex.has_value())
                    dependencySourceIndices.push_back(*dependencySourceIndex);
            }

            const bool hasExplicitEdgeDrivenQueueDependencies =
                !currentBatch.queueDependencySourceSubmissionOrders.empty();
            if (dependencySourceIndices.empty() ||
                (!hasExplicitEdgeDrivenQueueDependencies &&
                    !RequiresExplicitQueueWait(currentBatch.queueDependencyPolicy)))
            {
                lastBatchByQueueType[currentQueueIndex] = batchIndex;
                continue;
            }

            for (const auto dependencySourceIndex : dependencySourceIndices)
            {
                auto& sourceBatch = submitPlan.batches[dependencySourceIndex];
                if (sourceBatch.queueType == currentBatch.queueType)
                    continue;

                std::shared_ptr<Render::RHI::RHISemaphore> handoffSemaphore;
                if (handoffSemaphoreIndex == 0u &&
                    sourceBatch.queueType == Render::RHI::QueueType::Compute &&
                    frameContext.computeFinishedSemaphore != nullptr)
                {
                    handoffSemaphore = frameContext.computeFinishedSemaphore;
                }
                else
                {
                    handoffSemaphore = impl.explicitDevice->CreateSemaphore(
                        "ThreadedQueueHandoff" +
                        std::to_string(frameContext.frameIndex) + "_" +
                        std::to_string(handoffSemaphoreIndex));
                    if (handoffSemaphore != nullptr)
                        submitPlan.temporarySemaphores.push_back(handoffSemaphore);
                }

                if (handoffSemaphore == nullptr)
                {
                    submitPlan.failureDiagnostic = "Cross-queue handoff semaphore creation failed";
                    return submitPlan;
                }

                sourceBatch.signalSemaphores.push_back(handoffSemaphore);
                currentBatch.waitSemaphores.push_back(handoffSemaphore);
                ++handoffSemaphoreIndex;
            }
            lastBatchByQueueType[currentQueueIndex] = batchIndex;
        }

        submitPlan.usedDedicatedComputeQueueSubmission =
            canUseDedicatedAsyncCompute &&
            sawComputeBatch;
        return submitPlan;
    }

    std::shared_ptr<Render::RHI::RHISemaphore> Detail::EnsureLastComputeQueueCompletionSemaphore(
        DriverImpl& impl,
        Render::RHI::RHIFrameContext& frameContext,
        AsyncComputeSubmitPlan& submitPlan)
    {
        if (!submitPlan.usedDedicatedComputeQueueSubmission)
            return nullptr;

        for (auto batchIt = submitPlan.batches.rbegin(); batchIt != submitPlan.batches.rend(); ++batchIt)
        {
            if (batchIt->queueType != Render::RHI::QueueType::Compute)
                continue;

            std::shared_ptr<Render::RHI::RHISemaphore> handoffSemaphore =
                frameContext.computeFinishedSemaphore;
            if (handoffSemaphore == nullptr && impl.explicitDevice != nullptr)
            {
                handoffSemaphore = impl.explicitDevice->CreateSemaphore(
                    "ThreadedPostSubmitReadbackComputeHandoff" +
                    std::to_string(frameContext.frameIndex));
                if (handoffSemaphore != nullptr)
                    submitPlan.temporarySemaphores.push_back(handoffSemaphore);
            }

            if (handoffSemaphore == nullptr)
                return nullptr;

            if (std::find(
                    batchIt->signalSemaphores.begin(),
                    batchIt->signalSemaphores.end(),
                    handoffSemaphore) == batchIt->signalSemaphores.end())
            {
                batchIt->signalSemaphores.push_back(handoffSemaphore);
            }

            submitPlan.lastComputeQueueCompletionSemaphore = handoffSemaphore;
            return handoffSemaphore;
        }

        return nullptr;
    }

    size_t Detail::FindFirstSwapchainDependentWorkUnitPosition(
        const std::vector<ParallelCommandWorkUnit>& workUnits,
        const std::vector<size_t>& submissionOrderedIndices)
    {
        for (size_t position = 0u; position < submissionOrderedIndices.size(); ++position)
        {
            const size_t workUnitIndex = submissionOrderedIndices[position];
            if (workUnitIndex >= workUnits.size())
                return position;
            if (workUnits[workUnitIndex].commandInput.targetsSwapchain)
                return position;
        }
        return submissionOrderedIndices.size();
    }

    std::vector<ParallelCommandWorkUnit> Detail::BuildParallelCommandWorkUnits(
        const RenderScenePackage& renderScenePackage,
        const bool parallelRecordingReady,
        const bool parallelTranslationReady,
        const bool allowOrderedSlicedSubmission,
        const bool allowInRenderPassChildCommandRecording)
    {
        std::vector<ParallelCommandWorkUnit> workUnits;
        const auto canKeepOrderedSourceWorkUnit =
            [allowOrderedSlicedSubmission, allowInRenderPassChildCommandRecording](
                const ParallelCommandWorkUnit& sourceWorkUnit)
            {
                if (!sourceWorkUnit.requiresOrderedSlicedSubmission)
                    return true;
                if (!allowOrderedSlicedSubmission)
                    return false;
                if (sourceWorkUnit.usesInRenderPassChildCommandRecording &&
                    !allowInRenderPassChildCommandRecording)
                {
                    return false;
                }
                return true;
            };

        std::unordered_map<uint64_t, uint64_t> fallbackWorkUnitIndexByOriginalIndex;
        const auto appendFallbackSafeAdditionalWorkUnits = [&](std::vector<ParallelCommandWorkUnit>& destination)
        {
            if (!renderScenePackage.containsParallelCommandWorkUnits)
                return;

            for (size_t sourceIndex = 0u; sourceIndex < renderScenePackage.parallelCommandWorkUnits.size(); ++sourceIndex)
            {
                const auto& sourceWorkUnit = renderScenePackage.parallelCommandWorkUnits[sourceIndex];
                if (sourceWorkUnit.sourcePassIndex != kInvalidParallelCommandSourcePassIndex ||
                    sourceWorkUnit.requiresOrderedSlicedSubmission)
                {
                    continue;
                }

                auto workUnit = sourceWorkUnit;
                workUnit.workUnitIndex = static_cast<uint64_t>(destination.size());
                workUnit.submissionOrder = workUnit.workUnitIndex;
                workUnit.commandInput.queueType = ResolveEffectiveQueueTypeForPass(workUnit.commandInput);
                fallbackWorkUnitIndexByOriginalIndex[static_cast<uint64_t>(sourceIndex)] = workUnit.workUnitIndex;
                destination.push_back(std::move(workUnit));
            }
        };

        auto appendDispatchComputeWorkUnits = [&](std::vector<ParallelCommandWorkUnit>& destination)
        {
            for (const auto& dispatchInput : renderScenePackage.computeDispatchInputs)
            {
                ParallelCommandWorkUnit workUnit;
                workUnit.workUnitIndex = static_cast<uint64_t>(destination.size());
                workUnit.submissionOrder = workUnit.workUnitIndex;
                workUnit.commandInput.kind = RenderPassCommandKind::Compute;
                workUnit.commandInput.queueType = Render::RHI::QueueType::Compute;
                workUnit.commandInput.queueDependencyPolicy = QueueDependencyPolicy::None;
                workUnit.commandInput.debugName = !dispatchInput.debugName.empty()
                    ? dispatchInput.debugName
                    : ToThreadedPassDebugName(RenderPassCommandKind::Compute);
                workUnit.commandInput.renderWidth = renderScenePackage.renderWidth;
                workUnit.commandInput.renderHeight = renderScenePackage.renderHeight;
                workUnit.commandInput.targetsSwapchain = false;
                workUnit.commandInput.computeDispatchInputs.push_back(dispatchInput);
                workUnit.debugName = workUnit.commandInput.debugName;
                workUnit.eligibleForParallelRecording = false;
                workUnit.eligibleForParallelTranslation = false;
                destination.push_back(std::move(workUnit));
            }
        };

        if (renderScenePackage.containsParallelCommandWorkUnits &&
            !renderScenePackage.parallelCommandWorkUnits.empty() &&
            std::all_of(
                renderScenePackage.parallelCommandWorkUnits.begin(),
                renderScenePackage.parallelCommandWorkUnits.end(),
                canKeepOrderedSourceWorkUnit))
        {
            workUnits.reserve(renderScenePackage.parallelCommandWorkUnits.size());
            workUnits.insert(
                workUnits.end(),
                renderScenePackage.parallelCommandWorkUnits.begin(),
                renderScenePackage.parallelCommandWorkUnits.end());
            for (size_t index = 0; index < workUnits.size(); ++index)
            {
                auto& workUnit = workUnits[index];
                workUnit.workUnitIndex = static_cast<uint64_t>(index);
                workUnit.submissionOrder = static_cast<uint64_t>(index);
                workUnit.commandInput.queueType =
                    ResolveEffectiveQueueTypeForPass(workUnit.commandInput);
                if (workUnit.debugName.empty())
                {
                    workUnit.debugName = Detail::ResolvePassProfileScopeName(workUnit.commandInput);
                }
                workUnit.eligibleForParallelRecording =
                    (parallelRecordingReady && workUnit.eligibleForParallelRecording) ||
                    (allowInRenderPassChildCommandRecording &&
                        workUnit.usesInRenderPassChildCommandRecording);
                workUnit.eligibleForParallelTranslation =
                    parallelTranslationReady && workUnit.eligibleForParallelTranslation;
            }
            Detail::PopulateVisibilityTransitionsFromResourceUsage(workUnits);
            return workUnits;
        }

        const auto remapFallbackDependencyEdges =
            [&renderScenePackage, &workUnits, &fallbackWorkUnitIndexByOriginalIndex]()
            {
                if (renderScenePackage.parallelCommandWorkUnits.empty() ||
                    renderScenePackage.workUnitDependencyEdges.empty())
                {
                    return;
                }

                const auto resolveFallbackWorkUnitIndex =
                    [&renderScenePackage, &workUnits, &fallbackWorkUnitIndexByOriginalIndex](
                        const uint64_t sourceWorkUnitIndex) -> std::optional<uint64_t>
                    {
                        if (const auto mapped = fallbackWorkUnitIndexByOriginalIndex.find(sourceWorkUnitIndex);
                            mapped != fallbackWorkUnitIndexByOriginalIndex.end())
                        {
                            return mapped->second;
                        }

                        if (sourceWorkUnitIndex >= renderScenePackage.parallelCommandWorkUnits.size())
                            return std::nullopt;

                        const auto sourcePassIndex =
                            renderScenePackage.parallelCommandWorkUnits[static_cast<size_t>(sourceWorkUnitIndex)].sourcePassIndex;
                        if (sourcePassIndex == kInvalidParallelCommandSourcePassIndex)
                            return std::nullopt;

                        for (size_t fallbackIndex = 0u; fallbackIndex < workUnits.size(); ++fallbackIndex)
                        {
                            if (workUnits[fallbackIndex].sourcePassIndex == sourcePassIndex)
                                return static_cast<uint64_t>(fallbackIndex);
                        }
                        return std::nullopt;
                    };

                for (const auto& edge : renderScenePackage.workUnitDependencyEdges)
                {
                    const auto sourceIndex = resolveFallbackWorkUnitIndex(edge.sourceWorkUnitIndex);
                    const auto targetIndex = resolveFallbackWorkUnitIndex(edge.targetWorkUnitIndex);
                    if (!sourceIndex.has_value() || !targetIndex.has_value() || *targetIndex >= workUnits.size())
                        continue;

                    auto fallbackEdge = edge;
                    fallbackEdge.sourceWorkUnitIndex = *sourceIndex;
                    fallbackEdge.targetWorkUnitIndex = *targetIndex;
                    workUnits[static_cast<size_t>(*targetIndex)].incomingDependencyEdges.push_back(std::move(fallbackEdge));
                }
            };

        const auto appendUnslicedPassWorkUnit =
            [&](const size_t passIndex)
            {
                if (passIndex >= renderScenePackage.passCommandInputs.size())
                    return std::optional<uint64_t>{};

                const auto& passInput = renderScenePackage.passCommandInputs[passIndex];
                ParallelCommandWorkUnit workUnit;
                workUnit.workUnitIndex = static_cast<uint64_t>(workUnits.size());
                workUnit.submissionOrder = workUnit.workUnitIndex;
                workUnit.commandInput = passInput;
                workUnit.commandInput.queueType =
                    ResolveEffectiveQueueTypeForPass(workUnit.commandInput);
                workUnit.debugName = Detail::ResolvePassProfileScopeName(passInput);
                workUnit.sourcePassIndex = static_cast<uint64_t>(passIndex);
                workUnit.sliceIndex = 0u;
                workUnit.sliceCount = 1u;
                workUnit.recordedDrawBegin = 0u;
                workUnit.recordedDrawCount = static_cast<uint64_t>(passInput.recordedDrawCommands.size());
                const bool eligible = IsRenderPassEligibleForParallelRecording(passInput);
                workUnit.eligibleForParallelRecording = parallelRecordingReady && eligible;
                workUnit.eligibleForParallelTranslation = parallelTranslationReady && eligible;

                const auto appendedIndex = workUnit.workUnitIndex;
                workUnits.push_back(std::move(workUnit));
                return std::optional<uint64_t>{ appendedIndex };
            };

        if (renderScenePackage.containsParallelCommandWorkUnits &&
            !renderScenePackage.parallelCommandWorkUnits.empty() &&
            allowOrderedSlicedSubmission)
        {
            std::unordered_set<uint64_t> collapsedSourcePassIndices;
            workUnits.reserve(
                renderScenePackage.parallelCommandWorkUnits.size() +
                renderScenePackage.computeDispatchInputs.size() +
                renderScenePackage.passCommandInputs.size());

            for (size_t sourceIndex = 0u; sourceIndex < renderScenePackage.parallelCommandWorkUnits.size(); ++sourceIndex)
            {
                const auto& sourceWorkUnit = renderScenePackage.parallelCommandWorkUnits[sourceIndex];
                if (canKeepOrderedSourceWorkUnit(sourceWorkUnit))
                {
                    auto workUnit = sourceWorkUnit;
                    workUnit.workUnitIndex = static_cast<uint64_t>(workUnits.size());
                    workUnit.submissionOrder = workUnit.workUnitIndex;
                    workUnit.commandInput.queueType = ResolveEffectiveQueueTypeForPass(workUnit.commandInput);
                    if (workUnit.debugName.empty())
                        workUnit.debugName = Detail::ResolvePassProfileScopeName(workUnit.commandInput);
                    workUnit.eligibleForParallelRecording =
                        (parallelRecordingReady && workUnit.eligibleForParallelRecording) ||
                        (allowInRenderPassChildCommandRecording &&
                            workUnit.usesInRenderPassChildCommandRecording);
                    workUnit.eligibleForParallelTranslation =
                        parallelTranslationReady && workUnit.eligibleForParallelTranslation;
                    fallbackWorkUnitIndexByOriginalIndex[static_cast<uint64_t>(sourceIndex)] = workUnit.workUnitIndex;
                    workUnits.push_back(std::move(workUnit));
                    continue;
                }

                if (sourceWorkUnit.sourcePassIndex == kInvalidParallelCommandSourcePassIndex ||
                    !collapsedSourcePassIndices.insert(sourceWorkUnit.sourcePassIndex).second)
                {
                    continue;
                }

                const auto collapsedIndex = appendUnslicedPassWorkUnit(
                    static_cast<size_t>(sourceWorkUnit.sourcePassIndex));
                if (collapsedIndex.has_value())
                    fallbackWorkUnitIndexByOriginalIndex[static_cast<uint64_t>(sourceIndex)] = *collapsedIndex;
            }

            remapFallbackDependencyEdges();
            Detail::PopulateVisibilityTransitionsFromResourceUsage(workUnits);
            return workUnits;
        }

        workUnits.reserve(
            renderScenePackage.parallelCommandWorkUnits.size() +
            renderScenePackage.computeDispatchInputs.size() +
            renderScenePackage.passCommandInputs.size());
        appendFallbackSafeAdditionalWorkUnits(workUnits);
        appendDispatchComputeWorkUnits(workUnits);
        for (size_t index = 0; index < renderScenePackage.passCommandInputs.size(); ++index)
        {
            const auto& passInput = renderScenePackage.passCommandInputs[index];
            ParallelCommandWorkUnit workUnit;
            workUnit.workUnitIndex = static_cast<uint64_t>(workUnits.size());
            workUnit.submissionOrder = workUnit.workUnitIndex;
            workUnit.commandInput = passInput;
            workUnit.commandInput.queueType =
                ResolveEffectiveQueueTypeForPass(workUnit.commandInput);
            workUnit.debugName = Detail::ResolvePassProfileScopeName(passInput);
            workUnit.sourcePassIndex = static_cast<uint64_t>(index);
            workUnit.sliceIndex = 0u;
            workUnit.sliceCount = 1u;
            workUnit.recordedDrawBegin = 0u;
            workUnit.recordedDrawCount = static_cast<uint64_t>(passInput.recordedDrawCommands.size());
            const bool eligible = IsRenderPassEligibleForParallelRecording(passInput);
            workUnit.eligibleForParallelRecording = parallelRecordingReady && eligible;
            workUnit.eligibleForParallelTranslation = parallelTranslationReady && eligible;
            workUnits.push_back(std::move(workUnit));
        }

        remapFallbackDependencyEdges();
        Detail::PopulateVisibilityTransitionsFromResourceUsage(workUnits);
        return workUnits;
    }

    std::vector<WorkUnitDependencyEdge> Detail::FilterWorkUnitDependencyEdges(
        const ParallelCommandWorkUnit& workUnit,
        const std::function<bool(const WorkUnitDependencyEdge&)>& predicate)
    {
        std::vector<WorkUnitDependencyEdge> edges;
        for (const auto& edge : workUnit.incomingDependencyEdges)
        {
            if (predicate(edge))
                edges.push_back(edge);
        }
        return edges;
    }

    std::vector<uint64_t> Detail::BuildQueueDependencySourceOrders(
        const std::vector<WorkUnitDependencyEdge>& dependencyEdges)
    {
        std::vector<uint64_t> sourceOrders;
        sourceOrders.reserve(dependencyEdges.size());
        for (const auto& edge : dependencyEdges)
        {
            if (edge.kind != ThreadedDependencyKind::QueueSynchronization)
                continue;

            if (std::find(sourceOrders.begin(), sourceOrders.end(), edge.sourceWorkUnitIndex) ==
                sourceOrders.end())
            {
                sourceOrders.push_back(edge.sourceWorkUnitIndex);
            }
        }

        std::sort(sourceOrders.begin(), sourceOrders.end());
        return sourceOrders;
    }

    std::optional<uint64_t> Detail::ResolveImplicitDependencySourceWorkUnitIndex(
        const ParallelCommandWorkUnit& workUnit)
    {
        return UsesImplicitDependencySourceIndex(workUnit)
            ? workUnit.commandInput.dependencySourceWorkUnitIndex
            : std::nullopt;
    }

    std::optional<uint64_t> Detail::ResolveImplicitDependencySourceSubmissionOrder(
        const ParallelCommandWorkUnit& workUnit,
        const std::vector<WorkUnitDependencyEdge>& dependencyEdges)
    {
        return dependencyEdges.empty()
            ? ResolveImplicitDependencySourceWorkUnitIndex(workUnit)
            : std::nullopt;
    }

    bool Detail::ResolveImplicitRequiresDependencyVisibility(
        const ParallelCommandWorkUnit& workUnit)
    {
        return workUnit.incomingDependencyEdges.empty() &&
            workUnit.commandInput.requiresDependencyVisibility;
    }

    RenderPassCommandInput Detail::BuildDependencyVisibilityPassInput(
        const ParallelCommandWorkUnit& workUnit,
        const std::vector<WorkUnitDependencyEdge>& dependencyEdges)
    {
        RenderPassCommandInput visibilityInput;
        visibilityInput.kind = RenderPassCommandKind::Helper;
        visibilityInput.queueType = workUnit.commandInput.queueType;
        visibilityInput.queueDependencyPolicy = workUnit.commandInput.queueDependencyPolicy;
        visibilityInput.dependencySourceWorkUnitIndex =
            ResolveImplicitDependencySourceSubmissionOrder(workUnit, dependencyEdges);
        visibilityInput.requiresDependencyVisibility = true;
        visibilityInput.debugName = workUnit.debugName + "Visibility";
        visibilityInput.bufferVisibilityTransitions = workUnit.commandInput.bufferVisibilityTransitions;
        visibilityInput.textureVisibilityTransitions = workUnit.commandInput.textureVisibilityTransitions;

        for (const auto& edge : dependencyEdges)
        {
            if (const auto bufferTransition = BuildVisibilityTransitionFromDependencyEdge(edge);
                bufferTransition.has_value() &&
                !HasBufferVisibilityTransitionForResource(visibilityInput, bufferTransition->buffer))
            {
                visibilityInput.bufferVisibilityTransitions.push_back(*bufferTransition);
            }

            if (const auto textureTransition = BuildTextureVisibilityTransitionFromDependencyEdge(edge);
                textureTransition.has_value() &&
                !HasTextureVisibilityTransitionForResource(
                    visibilityInput,
                    textureTransition->texture,
                    textureTransition->subresourceRange))
            {
                visibilityInput.textureVisibilityTransitions.push_back(*textureTransition);
            }
        }

        return visibilityInput;
    }

    namespace
    {
        struct RecordedParallelCommandWorkUnit
        {
            ParallelCommandWorkUnit workUnit;
            std::shared_ptr<Render::RHI::RHICommandPool> commandPool;
            std::shared_ptr<Render::RHI::RHICommandBuffer> commandBuffer;
            std::shared_ptr<RhiCommandList> commands;
            std::unique_ptr<RecordedParallelCommandWorkUnit> queueRelease;
            std::thread::id translationThreadId;
            uint64_t recordedPassCount = 0u;
            uint64_t recordedDrawCount = 0u;
            uint64_t recordedComputeDispatchCount = 0u;
            bool wasRecorded = false;
            bool recordingFailed = false;
            std::string failureReason;
        };

        struct RecordedParallelCommandBufferBatch
        {
            std::vector<RecordedParallelCommandWorkUnit> recordedWorkUnits;
            std::optional<RecordedParallelCommandWorkUnit> postPassTransitions;
            uint64_t recordedPassCount = 0u;
            uint64_t recordedDrawCount = 0u;
            uint64_t recordedComputeDispatchCount = 0u;
            uint64_t recordedWorkUnitCount = 0u;
            uint32_t parallelWorkerCountUsed = 0u;
            bool usedParallelRecording = false;
        };

        void MarkRecordedParallelCommandWorkUnitFailure(
            RecordedParallelCommandWorkUnit& recordedWorkUnit,
            const char* reason)
        {
            recordedWorkUnit.recordingFailed = true;
            recordedWorkUnit.failureReason = reason != nullptr
                ? reason
                : "Parallel command recording failed";
        }

        bool PrepareParallelCommandResources(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
            const std::vector<ParallelCommandWorkUnit>& workUnits,
            const std::vector<size_t>& parallelEligibleIndices)
        {
            if (impl.explicitDevice == nullptr)
                return false;

            size_t requiredSlotCount = 0u;
            for (const auto workUnitIndex : parallelEligibleIndices)
                requiredSlotCount = std::max(
                    requiredSlotCount,
                    static_cast<size_t>(workUnits[workUnitIndex].workUnitIndex) + 1u);

            frameContext.parallelCommandPools.resize(requiredSlotCount);
            frameContext.parallelCommandBuffers.resize(requiredSlotCount);

            for (const auto workUnitIndex : parallelEligibleIndices)
            {
                const auto& workUnit = workUnits[workUnitIndex];
                const auto resourceIndex = static_cast<size_t>(workUnit.workUnitIndex);
                auto& commandPool = frameContext.parallelCommandPools[resourceIndex];
                auto& commandBuffer = frameContext.parallelCommandBuffers[resourceIndex];
                if (commandPool != nullptr &&
                    commandPool->GetQueueType() != workUnit.commandInput.queueType)
                {
                    commandBuffer.reset();
                    commandPool.reset();
                }

                if (commandPool == nullptr)
                {
                    commandPool = impl.explicitDevice->CreateCommandPool(
                        workUnit.commandInput.queueType,
                        "ThreadedCommandPool" + std::to_string(renderScenePackage.frameId) + "_" +
                            std::to_string(workUnit.workUnitIndex));
                    if (commandPool == nullptr)
                        return false;
                }

                if (commandBuffer == nullptr)
                {
                    const auto* passProfileScopeName =
                        Detail::ResolvePassProfileScopeName(workUnit.commandInput);
                    commandBuffer = commandPool->CreateCommandBuffer(
                        !workUnit.debugName.empty() ? workUnit.debugName : passProfileScopeName);
                    if (commandBuffer == nullptr)
                        return false;
                }
                else
                {
                    commandBuffer->Reset();
                }
            }
            return true;
        }

        bool PrepareInRenderPassChildCommandResources(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
            const std::vector<ParallelCommandWorkUnit>& workUnits)
        {
            if (impl.explicitDevice == nullptr)
                return false;

            size_t requiredSlotCount = 0u;
            for (const auto& workUnit : workUnits)
                requiredSlotCount = std::max(requiredSlotCount, static_cast<size_t>(workUnit.workUnitIndex) + 1u);

            frameContext.childCommandPools.resize(requiredSlotCount);
            frameContext.childCommandBuffers.resize(requiredSlotCount);

            for (const auto& workUnit : workUnits)
            {
                const auto resourceIndex = static_cast<size_t>(workUnit.workUnitIndex);
                auto& commandPool = frameContext.childCommandPools[resourceIndex];
                if (commandPool == nullptr)
                {
                    commandPool = impl.explicitDevice->CreateCommandPool(
                        Render::RHI::QueueType::Graphics,
                        "InRenderPassChildCommandPool" + std::to_string(renderScenePackage.frameId) + "_" +
                            std::to_string(workUnit.workUnitIndex));
                    if (commandPool == nullptr)
                        return false;
                }

                auto& commandBuffer = frameContext.childCommandBuffers[resourceIndex];
                if (commandBuffer == nullptr)
                {
                    commandBuffer = commandPool->CreateChildCommandBuffer(
                        !workUnit.debugName.empty() ? workUnit.debugName : "InRenderPassChildDrawRange");
                    if (commandBuffer == nullptr || !commandBuffer->IsChildCommandBuffer())
                        return false;
                }
                else
                {
                    commandBuffer->Reset();
                }
            }
            return true;
        }

        struct TranslatedParallelCommandBufferBatch
        {
            std::vector<Detail::ThreadedCommandSubmissionUnit> submissions;
            uint64_t recordedPassCount = 0u;
            uint64_t recordedDrawCount = 0u;
            uint64_t recordedComputeDispatchCount = 0u;
            uint64_t recordedWorkUnitCount = 0u;
            uint64_t translatedWorkUnitCount = 0u;
            std::vector<std::string> commandRecordingFailureReasons;
            bool usedTranslationMerge = false;
        };

        /** Stops the remaining operations of one frame dispatch. */
        struct ThreadedRhiSubmissionStageAbort final
        {
        };

    } // namespace

    namespace Detail
    {
        /**
         * Render-thread state for one frame dispatch. Translate workers own their
         * independent command lists; submit operations run through the executor.
         * The producer retains this object until CPU completion is published.
         */
        /**
         * One frame's asynchronous swapchain image acquisition.
         *
         * BeginThreadedRhiFrame enqueues it on the executor's ordered lane so the
         * lane can process it while the frame builds work units and records its
         * earlier passes. The view is bound by ResolveThreadedSwapchainAcquisition
         * at the first point a pass actually needs the backbuffer.
         */
        struct ThreadedSwapchainAcquisition final
        {
            RhiSubmitLaneOperation laneOperation;
            std::shared_ptr<Render::RHI::RHITextureView> backbufferView;
            uint32_t imageIndex = 0u;
            bool hasImage = false;
            bool enqueued = false;
            /** Guarantees the lane operation is waited on at most once. */
            bool resolved = false;
        };

        struct ThreadedRhiSubmissionExecution
        {
            Driver* driver = nullptr;
            DriverImpl* impl = nullptr;
            size_t frameContextIndex = 0u;
            uint64_t resourceLeaseGeneration = 0u;
            RhiSubmissionAttribution attribution = RhiSubmissionAttribution::Unknown;
            std::shared_ptr<ThreadedSwapchainAcquisition> swapchainAcquisition;
            /**
             * Per-frame queue-operation diagnostics. The driver's "current frame"
             * counters cannot be shared once several submissions may be in flight:
             * frame N's Present runs on the RHI lane while frame N+1 is prepared.
             */
            std::shared_ptr<RhiFrameQueueOperationTelemetry> queueTelemetry =
                std::make_shared<RhiFrameQueueOperationTelemetry>();
            RenderScenePackage renderScenePackage;
            RhiSubmissionFrame submissionFrame;
            Render::RHI::RHIFrameContext* frameContext = nullptr;
            Render::RHI::ResourceStateTrackerStats preResetTrackerStats {};
            Render::RHI::DescriptorAllocatorStats descriptorStats {};
            AsyncComputeSubmitPlan submitPlan;
            RecordedParallelCommandBufferBatch recordedParallelBatch;
            NLS::Base::TaskGraph::TaskCompletion completion;
            RhiSubmitEvent rhiCompletion;
            bool rhiDispatched = false;
            bool usesRecordedParallelBatch = false;
            bool supportsOrderedWorkUnitSubmission = false;
            bool parallelRecordingReady = false;
            bool parallelTranslationReady = false;
            bool framePrepared = false;
            bool frameRetired = false;
            // Success and abort can meet at retirement; publish CPU completion once.
            bool cpuRetired = false;
            bool submissionAttempted = false;
            bool submissionSucceeded = true;
            bool presentedAfterSubmitting = false;
            bool submittedGpuWorkMayNeedFenceConfirmation = false;
            bool frameFenceSignalMayHaveBeenQueued = false;
            bool frameFenceResetForSubmission = false;
            bool deviceLostDuringSubmit = false;
            bool mustDeferFrameScopedRetirement = false;
            bool mustQuarantineFrameScopedResources = false;
            bool submitResourcesRetained = false;
            bool presentRequested = false;
            bool presentSignalsFrameFence = false;
            Render::RHI::NativeHandle uiCompositionSignalSemaphore;
            uint64_t uiCompositionSignalValue = 0u;
            size_t presentGraphicsBatchIndex = 0u;
            std::string failureReason;
        };

        using ThreadedRhiSubmissionExecutionPtr = std::shared_ptr<ThreadedRhiSubmissionExecution>;

        struct RhiFrameExecutionQueue final
        {
            static void Enqueue(ThreadedRhiSubmissionExecutionPtr execution)
            {
                auto& executor = RhiCommandListExecutor::Get();
                std::lock_guard lock(executor.m_frameMutex);
                executor.m_pendingFrames.push_back(std::move(execution));
            }

            static ThreadedRhiSubmissionExecutionPtr Peek(const DriverImpl& owner)
            {
                auto& executor = RhiCommandListExecutor::Get();
                std::lock_guard lock(executor.m_frameMutex);
                if (executor.m_pendingFrames.empty() || executor.m_pendingFrames.front()->impl != &owner)
                    return {};
                return executor.m_pendingFrames.front();
            }

            static void Pop(const DriverImpl& owner)
            {
                auto& executor = RhiCommandListExecutor::Get();
                std::lock_guard lock(executor.m_frameMutex);
                if (!executor.m_pendingFrames.empty() && executor.m_pendingFrames.front()->impl == &owner)
                    executor.m_pendingFrames.pop_front();
            }

            static size_t Count(const DriverImpl& owner)
            {
                auto& executor = RhiCommandListExecutor::Get();
                std::lock_guard lock(executor.m_frameMutex);
                return static_cast<size_t>(std::count_if(executor.m_pendingFrames.begin(),
                    executor.m_pendingFrames.end(), [&owner](const auto& frame) { return frame->impl == &owner; }));
            }

            static std::vector<ThreadedRhiSubmissionExecutionPtr> Copy(const DriverImpl& owner)
            {
                auto& executor = RhiCommandListExecutor::Get();
                std::lock_guard lock(executor.m_frameMutex);
                std::vector<ThreadedRhiSubmissionExecutionPtr> frames;
                for (const auto& frame : executor.m_pendingFrames)
                    if (frame->impl == &owner)
                        frames.push_back(frame);
                return frames;
            }

            static std::mutex& RetirementMutex()
            {
                return RhiCommandListExecutor::Get().m_frameRetirementMutex;
            }
        };
    }

    namespace
    {
        using Detail::ThreadedRhiSubmissionExecution;
        using Detail::ThreadedRhiSubmissionExecutionPtr;
        using Detail::ThreadedSwapchainAcquisition;



        void MarkTranslatedParallelCommandFailure(
            TranslatedParallelCommandBufferBatch& batch,
            RecordedParallelCommandWorkUnit* recordedWorkUnit,
            const char* reason)
        {
            if (recordedWorkUnit != nullptr)
            {
                MarkRecordedParallelCommandWorkUnitFailure(*recordedWorkUnit, reason);
            }
            else
            {
                batch.commandRecordingFailureReasons.push_back(
                    reason != nullptr
                        ? reason
                        : "Parallel command translation failed");
            }
        }

        RhiCommandListRecorder::PreviousStateResolver ResolvePreviousFrameResourceStates(
            DriverImpl& impl, const RHI::RHIFrameContext& frame)
        {
            std::vector<std::pair<uint64_t, std::shared_ptr<RHI::ResourceStateTracker>>> previousFrames;
            {
                for (const auto& execution : Detail::RhiFrameExecutionQueue::Copy(impl))
                {
                    const auto* previous = execution->frameContext;
                    if (previous != nullptr && previous != &frame && previous->frameIndex < frame.frameIndex &&
                        previous->resourceStateTracker != nullptr)
                        previousFrames.emplace_back(previous->frameIndex, previous->resourceStateTracker);
                }
            }
            std::sort(previousFrames.begin(), previousFrames.end(),
                [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
            // Native state only describes submitted work. Recording the next frame
            // must also account for earlier lists waiting for ordered submission.
            return [previousFrames = std::move(previousFrames)](RHI::RHIBarrierDesc& barriers)
            {
                for (auto& barrier : barriers.bufferBarriers)
                {
                    if (barrier.before != RHI::ResourceState::Unknown)
                        continue;
                    for (const auto& previous : previousFrames)
                        if (const auto state = previous.second->GetBufferState(barrier.buffer))
                        {
                            barrier.before = state->state;
                            barrier.sourceStageMask = state->stageMask;
                            barrier.sourceAccessMask = state->accessMask;
                            break;
                        }
                }
                for (auto& barrier : barriers.textureBarriers)
                {
                    if (barrier.before != RHI::ResourceState::Unknown)
                        continue;
                    for (const auto& previous : previousFrames)
                        if (const auto state = previous.second->GetTextureState(barrier.texture, barrier.subresourceRange))
                        {
                            barrier.before = state->state;
                            barrier.sourceStageMask = state->stageMask;
                            barrier.sourceAccessMask = state->accessMask;
                            break;
                        }
                }
            };
        }

        RecordedParallelCommandWorkUnit RecordSingleParallelCommandWorkUnit(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
            const ParallelCommandWorkUnit& workUnit,
            const size_t frameContextIndex)
        {
            RecordedParallelCommandWorkUnit recordedWorkUnit;
            recordedWorkUnit.workUnit = workUnit;

            if (impl.explicitDevice == nullptr)
                return recordedWorkUnit;

            const auto& passInput = workUnit.commandInput;
            const bool isUiOverlayPass = passInput.kind == RenderPassCommandKind::UIOverlay;
            if (!isUiOverlayPass && !Detail::IsPassRecordable(renderScenePackage, passInput))
            {
                Detail::LogSkippedPass(renderScenePackage, passInput, "IsPassRecordable returned false");
                return recordedWorkUnit;
            }

            const auto resourceIndex = static_cast<size_t>(workUnit.workUnitIndex);
            if (resourceIndex >= frameContext.parallelCommandPools.size() ||
                resourceIndex >= frameContext.parallelCommandBuffers.size())
            {
                Detail::LogSkippedPass(renderScenePackage, passInput, "Missing reusable parallel command resources");
                MarkRecordedParallelCommandWorkUnitFailure(
                    recordedWorkUnit,
                    "Missing reusable parallel command resources");
                return recordedWorkUnit;
            }

            auto commandPool = frameContext.parallelCommandPools[resourceIndex];
            if (commandPool == nullptr)
            {
                Detail::LogSkippedPass(renderScenePackage, passInput, "Reusable parallel command pool is null");
                MarkRecordedParallelCommandWorkUnitFailure(recordedWorkUnit, "Reusable parallel command pool is null");
                return recordedWorkUnit;
            }

            const auto* passProfileScopeName = Detail::ResolvePassProfileScopeName(passInput);
            auto nativeCommandBuffer = frameContext.parallelCommandBuffers[resourceIndex];
            if (nativeCommandBuffer == nullptr)
            {
                Detail::LogSkippedPass(renderScenePackage, passInput, "Reusable parallel command buffer is null");
                MarkRecordedParallelCommandWorkUnitFailure(recordedWorkUnit, "Reusable parallel command buffer is null");
                return recordedWorkUnit;
            }

            auto commands = std::make_shared<RhiCommandList>();
            commands->SetDebugName(workUnit.debugName);
            commands->SetQueueType(passInput.queueType);
            commands->SetSignalFence(frameContext.frameFence);
            std::unique_ptr<RhiCommandListRecorder> releaseRecorder;
            auto commandBuffer = std::make_shared<RhiCommandListRecorder>(
                *commands, *nativeCommandBuffer, frameContext.resourceStateTracker,
                [&](const RHI::RHIBarrierDesc& barriers) -> RHI::RHICommandRecordingResult
                {
                    if (releaseRecorder == nullptr)
                    {
                        recordedWorkUnit.queueRelease = std::make_unique<RecordedParallelCommandWorkUnit>();
                        auto& release = *recordedWorkUnit.queueRelease;
                        release.workUnit.commandInput.queueType = RHI::QueueType::Graphics;
                        release.commandPool = impl.explicitDevice->CreateCommandPool(
                            RHI::QueueType::Graphics, workUnit.debugName + "::QueueReleasePool");
                        release.commandBuffer = release.commandPool != nullptr
                            ? release.commandPool->CreateCommandBuffer(workUnit.debugName + "::QueueRelease") : nullptr;
                        if (release.commandBuffer == nullptr)
                            return { RHI::RHICommandRecordingStatusCode::BackendFailure,
                                "Graphics queue release command resource creation failed" };
                        release.commands = std::make_shared<RhiCommandList>();
                        release.commands->SetDebugName(workUnit.debugName + "::QueueRelease");
                        release.commands->SetSignalFence(frameContext.frameFence);
                        releaseRecorder = std::make_unique<RhiCommandListRecorder>(
                            *release.commands, *release.commandBuffer, frameContext.resourceStateTracker,
                            RhiCommandListRecorder::QueueReleaseFunction {}, ResolvePreviousFrameResourceStates(impl, frameContext));
                        releaseRecorder->Begin();
                    }
                    return releaseRecorder->BarrierChecked(barriers);
                }, ResolvePreviousFrameResourceStates(impl, frameContext));
            recordedWorkUnit.commandPool = commandPool;
            recordedWorkUnit.commandBuffer = nativeCommandBuffer;
            recordedWorkUnit.commands = commands;
            commandBuffer->Begin();
            if (!isUiOverlayPass)
            {
                const auto visibilityInput = Detail::BuildDependencyVisibilityPassInput(
                    workUnit, workUnit.incomingDependencyEdges);
                if (Detail::HasResourceVisibilityTransitions(visibilityInput) &&
                    !Detail::RecordResourceVisibilityTransitions(*commandBuffer, visibilityInput, &frameContext))
                {
                    MarkRecordedParallelCommandWorkUnitFailure(recordedWorkUnit, "Dependency visibility planning failed");
                    commandBuffer->End();
                    return recordedWorkUnit;
                }
            }
            if (passInput.kind == RenderPassCommandKind::Compute)
            {
                NLS_PROFILE_NAMED_SCOPE(passProfileScopeName);
                commandBuffer->BeginGpuProfileScope(passProfileScopeName, __FUNCTION__);
                const auto recordedDispatchCount = Detail::RecordComputeDispatches(
                    *commandBuffer,
                    passInput.computeDispatchInputs,
                    false);
                commandBuffer->EndGpuProfileScope();
                if (releaseRecorder != nullptr)
                {
                    releaseRecorder->End();
                    recordedWorkUnit.queueRelease->wasRecorded = true;
                }
                if (recordedDispatchCount == 0u)
                {
                    if (!passInput.computeDispatchInputs.empty())
                        MarkRecordedParallelCommandWorkUnitFailure(recordedWorkUnit, "No dispatches recorded but commands were expected");
                    if (commandBuffer->IsRecording())
                        commandBuffer->End();
                    return recordedWorkUnit;
                }

                if (commandBuffer->IsRecording())
                    commandBuffer->End();

                recordedWorkUnit.commandPool = commandPool;
                recordedWorkUnit.commandBuffer = nativeCommandBuffer;
                recordedWorkUnit.recordedPassCount = 1u;
                recordedWorkUnit.recordedComputeDispatchCount = recordedDispatchCount;
                recordedWorkUnit.wasRecorded = true;
                return recordedWorkUnit;
            }

            if (passInput.targetsSwapchain && frameContext.swapchainBackbufferView == nullptr)
            {
                Detail::LogSkippedPass(renderScenePackage, passInput, "swapchainBackbufferView is null");
                MarkRecordedParallelCommandWorkUnitFailure(recordedWorkUnit, "swapchainBackbufferView is null");
                if (commandBuffer->IsRecording())
                    commandBuffer->End();
                return recordedWorkUnit;
            }

            const auto effectivePassInput = ResolveSwapchainDepthPassInput(passInput, frameContext);
            std::optional<RenderPassCommandInput> preparedUiOverlayPassInput;
            if (isUiOverlayPass)
            {
                std::string prepareError;
                preparedUiOverlayPassInput = PrepareUiOverlayPassInput(
                    impl,
                    frameContext,
                    *commandBuffer,
                    effectivePassInput,
                    frameContextIndex,
                    prepareError);
                if (!preparedUiOverlayPassInput.has_value())
                {
                    Detail::LogSkippedPass(
                        renderScenePackage,
                        effectivePassInput,
                        prepareError.empty()
                            ? "UI overlay resource preparation failed"
                            : prepareError.c_str());
                    MarkRecordedParallelCommandWorkUnitFailure(
                        recordedWorkUnit,
                        prepareError.empty()
                            ? "UI overlay resource preparation failed"
                            : prepareError.c_str());
                    if (commandBuffer->IsRecording())
                        commandBuffer->End();
                    return recordedWorkUnit;
                }
            }
            const auto& recordablePassInput = preparedUiOverlayPassInput.has_value()
                ? *preparedUiOverlayPassInput
                : effectivePassInput;
            if (isUiOverlayPass)
            {
                auto visibilityWorkUnit = workUnit;
                visibilityWorkUnit.commandInput = recordablePassInput;
                MaterializeUiOverlayDependencyEdges(
                    visibilityWorkUnit.incomingDependencyEdges,
                    recordablePassInput);
                const auto visibilityInput = Detail::BuildDependencyVisibilityPassInput(
                    visibilityWorkUnit,
                    visibilityWorkUnit.incomingDependencyEdges);
                if (Detail::HasResourceVisibilityTransitions(visibilityInput))
                {
                    const bool recordedVisibilityTransitions = Detail::RecordResourceVisibilityTransitions(
                        *commandBuffer,
                        visibilityInput,
                        &frameContext);
                    if (!recordedVisibilityTransitions)
                    {
                        MarkRecordedParallelCommandWorkUnitFailure(
                            recordedWorkUnit,
                            "Dependency visibility transition recording failed");
                        if (commandBuffer->IsRecording())
                            commandBuffer->End();
                        return recordedWorkUnit;
                    }
                }
            }
            if (!Detail::BeginPassCommandPlan(
                *commandBuffer,
                frameContext.swapchainBackbufferView,
                frameContext.swapchainDepthStencilView,
                recordablePassInput,
                &frameContext))
            {
                Detail::LogSkippedPass(renderScenePackage, recordablePassInput, "BeginPassCommandPlan failed");
                MarkRecordedParallelCommandWorkUnitFailure(recordedWorkUnit, "BeginPassCommandPlan failed");
                if (commandBuffer->IsRecording())
                    commandBuffer->End();
                return recordedWorkUnit;
            }

            const auto* effectivePassProfileScopeName = Detail::ResolvePassProfileScopeName(recordablePassInput);
            NLS_PROFILE_NAMED_SCOPE(effectivePassProfileScopeName);
            commandBuffer->BeginGpuProfileScope(effectivePassProfileScopeName, __FUNCTION__);
            uint64_t recordedDrawCount = 0u;
            const bool recordsSlicedSourceRange =
                !isUiOverlayPass &&
                workUnit.requiresOrderedSlicedSubmission &&
                workUnit.sourcePassIndex < renderScenePackage.passCommandInputs.size();
            if (isUiOverlayPass)
            {
                const auto uiResult = impl.uiOverlayRenderer.RecordPrepared(
                    *commandBuffer,
                    *recordablePassInput.uiDrawDataSnapshot,
                    frameContextIndex);
                if (!uiResult.success)
                {
                    commandBuffer->EndGpuProfileScope();
                    Detail::EndPassCommandPlan(*commandBuffer, &recordablePassInput, &frameContext);
                    Detail::LogSkippedPass(
                        renderScenePackage,
                        recordablePassInput,
                        uiResult.message.empty()
                            ? "UI overlay recording failed"
                            : uiResult.message.c_str());
                    MarkRecordedParallelCommandWorkUnitFailure(
                        recordedWorkUnit,
                        uiResult.message.empty()
                            ? "UI overlay recording failed"
                            : uiResult.message.c_str());
                    if (commandBuffer->IsRecording())
                        commandBuffer->End();
                    return recordedWorkUnit;
                }
                recordedDrawCount = uiResult.recordedDraws ? effectivePassInput.drawCount : 0u;
            }
            else
            {
                recordedDrawCount = recordsSlicedSourceRange
                    ? Detail::RecordPreparedDrawCommandsForPassRange(
                        commandBuffer.get(),
                        effectivePassInput,
                        renderScenePackage.passCommandInputs[static_cast<size_t>(workUnit.sourcePassIndex)].recordedDrawCommands,
                        workUnit.recordedDrawBegin,
                        workUnit.recordedDrawCount)
                    : Detail::RecordPreparedDrawCommandsForPass(commandBuffer.get(), effectivePassInput);
            }
            commandBuffer->EndGpuProfileScope();
            Detail::EndPassCommandPlan(*commandBuffer, &recordablePassInput, &frameContext);
            if (!isUiOverlayPass &&
                recordedDrawCount == 0u &&
                (!recordablePassInput.recordedDrawCommands.empty() ||
                    (recordsSlicedSourceRange && workUnit.recordedDrawCount > 0u)))
            {
                Detail::LogSkippedPass(
                    renderScenePackage,
                    recordablePassInput,
                    "No draws recorded but commands were expected");
                MarkRecordedParallelCommandWorkUnitFailure(
                    recordedWorkUnit,
                    "No draws recorded but commands were expected");
                if (commandBuffer->IsRecording())
                    commandBuffer->End();
                return recordedWorkUnit;
            }

            if (commandBuffer->IsRecording())
                commandBuffer->End();

            recordedWorkUnit.commandPool = commandPool;
            recordedWorkUnit.commandBuffer = nativeCommandBuffer;
            recordedWorkUnit.recordedPassCount = 1u;
            recordedWorkUnit.recordedDrawCount = recordedDrawCount;
            recordedWorkUnit.wasRecorded = true;
            return recordedWorkUnit;
        }

        bool NativeHandlesMatch(
            const Render::RHI::NativeHandle& lhs,
            const Render::RHI::NativeHandle& rhs)
        {
            return lhs.backend == rhs.backend &&
                lhs.handle == rhs.handle &&
                lhs.value == rhs.value;
        }

        struct DescriptorHeapCompatibilitySignature
        {
            std::array<Render::RHI::NativeHandle, 2u> heaps {};
            bool hasKnownHeap = false;
            bool internallyCompatible = true;
        };

        void AccumulateDescriptorHeapCompatibilityHandle(
            DescriptorHeapCompatibilitySignature& signature,
            const std::shared_ptr<Render::RHI::RHIBindingSet>& bindingSet,
            const uint32_t heapClass)
        {
            if (bindingSet == nullptr || heapClass >= signature.heaps.size())
                return;

            const auto heap = bindingSet->GetNativeDescriptorHeapCompatibilityHandle(heapClass);
            if (!heap.IsValid())
                return;

            auto& current = signature.heaps[heapClass];
            if (!current.IsValid())
            {
                current = heap;
                signature.hasKnownHeap = true;
                return;
            }

            if (!NativeHandlesMatch(current, heap))
                signature.internallyCompatible = false;
        }

        DescriptorHeapCompatibilitySignature ResolveDrawDescriptorHeapCompatibilitySignature(
            const RecordedDrawCommandInput& drawCommand)
        {
            DescriptorHeapCompatibilitySignature signature;
            constexpr uint32_t kResourceHeapClass = 0u;
            constexpr uint32_t kSamplerHeapClass = 1u;
            const std::array<std::shared_ptr<Render::RHI::RHIBindingSet>, 4u> bindingSets = {
                drawCommand.frameBindingSet,
                drawCommand.objectBindingSet,
                drawCommand.passBindingSet,
                drawCommand.materialBindingSet
            };
            for (const auto& bindingSet : bindingSets)
            {
                AccumulateDescriptorHeapCompatibilityHandle(signature, bindingSet, kResourceHeapClass);
                AccumulateDescriptorHeapCompatibilityHandle(signature, bindingSet, kSamplerHeapClass);
            }
            return signature;
        }

        bool IsDescriptorHeapSignatureCompatibleWithRange(
            const DescriptorHeapCompatibilitySignature& referenceSignature,
            const DescriptorHeapCompatibilitySignature& candidateSignature)
        {
            if (!referenceSignature.internallyCompatible ||
                !candidateSignature.internallyCompatible)
            {
                return false;
            }

            if (!referenceSignature.hasKnownHeap || !candidateSignature.hasKnownHeap)
                return true;

            for (size_t heapIndex = 0u; heapIndex < referenceSignature.heaps.size(); ++heapIndex)
            {
                const auto& referenceHeap = referenceSignature.heaps[heapIndex];
                const auto& candidateHeap = candidateSignature.heaps[heapIndex];
                if (referenceHeap.IsValid() &&
                    candidateHeap.IsValid() &&
                    !NativeHandlesMatch(referenceHeap, candidateHeap))
                {
                    return false;
                }
            }
            return true;
        }

        bool IsChildWorkUnitDescriptorHeapStable(
            const RenderPassCommandInput& parentPassInput,
            const ParallelCommandWorkUnit& workUnit)
        {
            if (workUnit.recordedDrawCount == 0u ||
                workUnit.recordedDrawBegin >= parentPassInput.recordedDrawCommands.size())
            {
                return true;
            }

            const auto drawEnd = std::min<uint64_t>(
                static_cast<uint64_t>(parentPassInput.recordedDrawCommands.size()),
                workUnit.recordedDrawBegin + workUnit.recordedDrawCount);
            std::optional<DescriptorHeapCompatibilitySignature> referenceSignature;
            for (uint64_t drawIndex = workUnit.recordedDrawBegin; drawIndex < drawEnd; ++drawIndex)
            {
                const auto signature = ResolveDrawDescriptorHeapCompatibilitySignature(
                    parentPassInput.recordedDrawCommands[static_cast<size_t>(drawIndex)]);
                if (!signature.internallyCompatible)
                    return false;

                if (!signature.hasKnownHeap)
                    continue;

                if (!referenceSignature.has_value())
                {
                    referenceSignature = signature;
                    continue;
                }

                if (!IsDescriptorHeapSignatureCompatibleWithRange(*referenceSignature, signature))
                    return false;
            }
            return true;
        }

        RecordedParallelCommandWorkUnit RecordSingleInRenderPassChildWorkUnit(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
            const ParallelCommandWorkUnit& workUnit,
            const RenderPassCommandInput& parentPassInput)
        {
            RecordedParallelCommandWorkUnit recordedWorkUnit;
            recordedWorkUnit.workUnit = workUnit;

            if (impl.explicitDevice == nullptr)
                return recordedWorkUnit;

            const auto resourceIndex = static_cast<size_t>(workUnit.workUnitIndex);
            if (resourceIndex >= frameContext.childCommandPools.size() ||
                resourceIndex >= frameContext.childCommandBuffers.size())
            {
                MarkRecordedParallelCommandWorkUnitFailure(
                    recordedWorkUnit,
                    "Missing reusable child command resources");
                return recordedWorkUnit;
            }

            auto commandPool = frameContext.childCommandPools[resourceIndex];
            auto commandBuffer = frameContext.childCommandBuffers[resourceIndex];
            if (commandPool == nullptr)
            {
                MarkRecordedParallelCommandWorkUnitFailure(
                    recordedWorkUnit,
                    "Reusable child command pool is null");
                return recordedWorkUnit;
            }
            if (commandBuffer == nullptr || !commandBuffer->IsChildCommandBuffer())
            {
                MarkRecordedParallelCommandWorkUnitFailure(
                    recordedWorkUnit,
                    "Reusable child command buffer is invalid");
                return recordedWorkUnit;
            }
            recordedWorkUnit.commandPool = commandPool;

            if (!AreDrawPipelinesCompatibleWithParentRenderPass(
                    parentPassInput,
                    parentPassInput.recordedDrawCommands,
                    workUnit.recordedDrawBegin,
                    workUnit.recordedDrawCount))
            {
                MarkRecordedParallelCommandWorkUnitFailure(
                    recordedWorkUnit,
                    "Child draw pipeline render target layout does not match parent render pass");
                return recordedWorkUnit;
            }
            if (!IsChildWorkUnitDescriptorHeapStable(parentPassInput, workUnit))
            {
                MarkRecordedParallelCommandWorkUnitFailure(
                    recordedWorkUnit,
                    "Child draw descriptor heaps are not stable within bundle range");
                return recordedWorkUnit;
            }

            commandBuffer->Begin();
            const auto recordedDrawCount = Detail::RecordPreparedDrawCommandsForPassRange(
                commandBuffer.get(),
                workUnit.commandInput,
                parentPassInput.recordedDrawCommands,
                workUnit.recordedDrawBegin,
                workUnit.recordedDrawCount);
            if (commandBuffer->IsRecording())
                commandBuffer->End();

            if (recordedDrawCount != workUnit.recordedDrawCount)
            {
                MarkRecordedParallelCommandWorkUnitFailure(
                    recordedWorkUnit,
                    recordedDrawCount == 0u
                        ? "No child draws recorded but commands were expected"
                        : "Not all child draws were recorded");
                return recordedWorkUnit;
            }

            recordedWorkUnit.commandBuffer = commandBuffer;
            recordedWorkUnit.recordedDrawCount = recordedDrawCount;
            recordedWorkUnit.wasRecorded = true;
            return recordedWorkUnit;
        }

        uint32_t ResolveParallelRecordingWorkerCount(
            const DriverImpl& impl,
            const std::vector<ParallelCommandWorkUnit>& workUnits)
        {
            const auto eligibleCount = static_cast<uint32_t>(std::count_if(
                workUnits.begin(),
                workUnits.end(),
                [](const ParallelCommandWorkUnit& workUnit)
                {
                    return workUnit.eligibleForParallelRecording;
                }));
            if (eligibleCount <= 1u)
                return eligibleCount;

            if (impl.taskGraph == nullptr || !impl.taskGraph->IsStarted())
                return 1u;

            const auto workerBudget = std::max(1u, impl.taskGraph->GetWorkerCount());
            return std::min(eligibleCount, workerBudget);
        }

        bool RecordInRenderPassChildWorkUnitsWithTaskGraph(
            RecordedParallelCommandBufferBatch& batch,
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
            const std::vector<ParallelCommandWorkUnit>& workUnits,
            const RenderPassCommandInput& parentPassInput)
        {
            if (workUnits.empty())
                return true;
            if (impl.taskGraph == nullptr || !impl.taskGraph->IsStarted())
                return false;

            std::vector<std::atomic_bool> workUnitCompleted(workUnits.size());
            for (auto& completed : workUnitCompleted)
                completed.store(false, std::memory_order_relaxed);

            std::vector<NLS::Base::TaskGraph::TaskHandle> handles;
            handles.reserve(workUnits.size());
            for (size_t workUnitIndex = 0u; workUnitIndex < workUnits.size(); ++workUnitIndex)
            {
                NLS::Base::TaskGraph::TaskDesc desc;
                desc.function = [&batch,
                                 &impl,
                                 &frameContext,
                                 &renderScenePackage,
                                 &workUnits,
                                 &parentPassInput,
                                 &workUnitCompleted,
                                 workUnitIndex]
                {
                    NLS_PROFILE_NAMED_SCOPE("RhiCommandListExecutor::RecordInRenderPassChildCommandWorkUnit");
                    batch.recordedWorkUnits[workUnitIndex] = RecordSingleInRenderPassChildWorkUnit(
                        impl,
                        frameContext,
                        renderScenePackage,
                        workUnits[workUnitIndex],
                        parentPassInput);
                    workUnitCompleted[workUnitIndex].store(true, std::memory_order_release);
                };
                desc.targetThread = NLS::Base::TaskGraph::NamedThread::Any;
                desc.priority = NLS::Base::TaskGraph::TaskPriority::High;
                desc.debugName = "RhiCommandListExecutor::RecordInRenderPassChildCommandWorkUnits";
                handles.push_back(impl.taskGraph->Submit(std::move(desc)));
                if (!handles.back().IsValid())
                {
                    for (const auto pendingHandle : handles)
                    {
                        if (pendingHandle.IsValid())
                            (void)impl.taskGraph->Wait(pendingHandle);
                    }
                    return false;
                }
            }

            bool allSucceeded = true;
            for (const auto handle : handles)
            {
                const auto status = impl.taskGraph->Wait(handle);
                allSucceeded = allSucceeded &&
                    status == NLS::Base::TaskGraph::TaskStatus::Succeeded;
            }
            const bool allWorkUnitsCompleted = std::all_of(
                workUnitCompleted.begin(),
                workUnitCompleted.end(),
                [](const std::atomic_bool& completed)
                {
                    return completed.load(std::memory_order_acquire);
                });
            return allSucceeded && allWorkUnitsCompleted;
        }

        RecordedParallelCommandBufferBatch RecordParallelCommandWorkUnits(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
            const std::vector<ParallelCommandWorkUnit>& workUnits,
            const size_t frameContextIndex,
            const ThreadedRhiSubmissionExecutionPtr& execution)
        {
            RecordedParallelCommandBufferBatch batch;
            if (impl.explicitDevice == nullptr)
                return batch;

            if (frameContext.resourceStateTracker == nullptr)
                frameContext.resourceStateTracker = Render::RHI::CreateDefaultResourceStateTracker();

            batch.recordedWorkUnits.resize(workUnits.size());
            std::vector<size_t> workUnitResourceIndices;
            workUnitResourceIndices.reserve(workUnits.size());
            for (size_t index = 0; index < workUnits.size(); ++index)
            {
                workUnitResourceIndices.push_back(index);
            }

            if (!workUnitResourceIndices.empty() &&
                !PrepareParallelCommandResources(
                    impl,
                    frameContext,
                    renderScenePackage,
                    workUnits,
                    workUnitResourceIndices))
            {
                for (const auto workUnitIndex : workUnitResourceIndices)
                {
                    MarkRecordedParallelCommandWorkUnitFailure(
                        batch.recordedWorkUnits[workUnitIndex],
                        "PrepareParallelCommandResources failed");
                }
                return batch;
            }

            // Resolve shared state in GPU submission order before any native
            // command buffer is touched by a translate worker.
            std::stable_sort(workUnitResourceIndices.begin(), workUnitResourceIndices.end(),
                [&workUnits](const size_t lhs, const size_t rhs)
                {
                    return workUnits[lhs].submissionOrder < workUnits[rhs].submissionOrder;
                });

            // Split at the first work unit that consumes the acquired backbuffer.
            // Everything before that boundary is view-independent, so it records
            // while the ordered lane is still draining the previous frame's
            // submit/Present. The acquisition is resolved at the boundary and the
            // trailing swapchain batch is recorded afterwards, which keeps the
            // recording order - and therefore the barrier commits - unchanged.
            const size_t swapchainBatchBegin =
                Detail::FindFirstSwapchainDependentWorkUnitPosition(
                    workUnits, workUnitResourceIndices);
            const auto recordParallelWorkUnit = [&](const size_t index)
            {
                batch.recordedWorkUnits[index] = RecordSingleParallelCommandWorkUnit(
                    impl,
                    frameContext,
                    renderScenePackage,
                    workUnits[index],
                    frameContextIndex);
            };
            for (size_t orderedIndex = 0u; orderedIndex < swapchainBatchBegin; ++orderedIndex)
            {
                recordParallelWorkUnit(workUnitResourceIndices[orderedIndex]);
            }

            if (swapchainBatchBegin < workUnitResourceIndices.size() &&
                !Detail::ResolveThreadedSwapchainAcquisition(execution))
            {
                for (size_t orderedIndex = swapchainBatchBegin;
                     orderedIndex < workUnitResourceIndices.size();
                     ++orderedIndex)
                {
                    MarkRecordedParallelCommandWorkUnitFailure(
                        batch.recordedWorkUnits[workUnitResourceIndices[orderedIndex]],
                        "swapchain acquisition failed");
                }
                return batch;
            }

            for (size_t orderedIndex = swapchainBatchBegin;
                 orderedIndex < workUnitResourceIndices.size();
                 ++orderedIndex)
            {
                recordParallelWorkUnit(workUnitResourceIndices[orderedIndex]);
            }

            const auto extraction = Render::FrameGraph::BuildExtractionVisibilityPassInput(renderScenePackage);
            if (Detail::HasResourceVisibilityTransitions(extraction))
            {
                batch.postPassTransitions.emplace();
                auto& post = *batch.postPassTransitions;
                post.commandPool = impl.explicitDevice->CreateCommandPool(
                    Render::RHI::QueueType::Graphics, "FrameExtractionPool");
                post.commandBuffer = post.commandPool != nullptr
                    ? post.commandPool->CreateCommandBuffer("FrameExtraction") : nullptr;
                if (post.commandBuffer == nullptr)
                {
                    MarkRecordedParallelCommandWorkUnitFailure(post, "Post-pass command resource creation failed");
                }
                else
                {
                    post.commands = std::make_shared<RhiCommandList>();
                    post.commands->SetDebugName("FrameExtraction");
                    post.commands->SetSignalFence(frameContext.frameFence);
                    RhiCommandListRecorder recorder(*post.commands, *post.commandBuffer, frameContext.resourceStateTracker,
                        RhiCommandListRecorder::QueueReleaseFunction {}, ResolvePreviousFrameResourceStates(impl, frameContext));
                    recorder.Begin();
                    post.wasRecorded = Detail::RecordResourceVisibilityTransitions(recorder, extraction, &frameContext);
                    recorder.End();
                    if (!post.wasRecorded)
                        MarkRecordedParallelCommandWorkUnitFailure(post, "Post-pass transition planning failed");
                }
            }

            for (const auto& recordedWorkUnit : batch.recordedWorkUnits)
            {
                if (!recordedWorkUnit.wasRecorded)
                    continue;

                batch.recordedPassCount += recordedWorkUnit.recordedPassCount;
                batch.recordedDrawCount += recordedWorkUnit.recordedDrawCount;
                batch.recordedComputeDispatchCount += recordedWorkUnit.recordedComputeDispatchCount;
                batch.recordedWorkUnitCount += 1u;
            }

            return batch;
        }

        RecordedParallelCommandBufferBatch RecordInRenderPassChildCommandWorkUnits(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
            const std::vector<ParallelCommandWorkUnit>& workUnits,
            const RenderPassCommandInput& parentPassInput)
        {
            RecordedParallelCommandBufferBatch batch;
            if (impl.explicitDevice == nullptr)
                return batch;

            batch.recordedWorkUnits.resize(workUnits.size());
            if (!PrepareInRenderPassChildCommandResources(
                impl,
                frameContext,
                renderScenePackage,
                workUnits))
            {
                return batch;
            }

            const auto workerCount = workUnits.size() >= kMinimumInRenderPassChildSlicesForParallelWorkers
                ? ResolveParallelRecordingWorkerCount(impl, workUnits)
                : 1u;
            if (workerCount > 1u && impl.taskGraph != nullptr && impl.taskGraph->IsStarted())
            {
                const bool scheduledChildJobs = RecordInRenderPassChildWorkUnitsWithTaskGraph(
                    batch,
                    impl,
                    frameContext,
                    renderScenePackage,
                    workUnits,
                    parentPassInput);
                if (scheduledChildJobs)
                {
                    batch.parallelWorkerCountUsed = workerCount;
                    batch.usedParallelRecording = true;
                }
                else
                {
                    for (size_t workUnitIndex = 0u; workUnitIndex < workUnits.size(); ++workUnitIndex)
                    {
                        batch.recordedWorkUnits[workUnitIndex] = RecordSingleInRenderPassChildWorkUnit(
                            impl,
                            frameContext,
                            renderScenePackage,
                            workUnits[workUnitIndex],
                            parentPassInput);
                    }
                    batch.parallelWorkerCountUsed = 1u;
                }
            }
            else if (!workUnits.empty())
            {
                for (size_t workUnitIndex = 0u; workUnitIndex < workUnits.size(); ++workUnitIndex)
                {
                    batch.recordedWorkUnits[workUnitIndex] = RecordSingleInRenderPassChildWorkUnit(
                        impl,
                        frameContext,
                        renderScenePackage,
                        workUnits[workUnitIndex],
                        parentPassInput);
                }
                batch.parallelWorkerCountUsed = 1u;
            }

            for (const auto& recordedWorkUnit : batch.recordedWorkUnits)
            {
                if (!recordedWorkUnit.wasRecorded)
                    continue;

                batch.recordedDrawCount += recordedWorkUnit.recordedDrawCount;
                batch.recordedWorkUnitCount += 1u;
            }

            return batch;
        }

        struct InRenderPassChildCommandGroupResult
        {
            bool recorded = false;
            bool failedBeforeParentPassOpened = false;
            bool failedAfterParentPassOpened = false;
            std::vector<std::shared_ptr<Render::RHI::RHICommandPool>> childCommandPools;
            std::vector<std::shared_ptr<Render::RHI::RHICommandBuffer>> childCommandBuffers;
            uint64_t recordedDrawCount = 0u;
            uint64_t recordedWorkUnitCount = 0u;
            uint32_t parallelWorkerCountUsed = 0u;
        };

        bool IsChildRecordingFailureSerialFallbackSafe(const std::string& failureReason)
        {
            return failureReason.find("render target layout does not match parent render pass") ==
                std::string::npos;
        }

        RenderPassCommandInput BuildInRenderPassChildGroupVisibilityInput(
            const std::vector<ParallelCommandWorkUnit>& workUnits)
        {
            RenderPassCommandInput visibilityInput;
            if (workUnits.empty())
                return visibilityInput;

            visibilityInput = Detail::BuildDependencyVisibilityPassInput(
                workUnits.front(),
                workUnits.front().incomingDependencyEdges);
            for (size_t workUnitIndex = 1u; workUnitIndex < workUnits.size(); ++workUnitIndex)
            {
                const auto workUnitVisibilityInput = Detail::BuildDependencyVisibilityPassInput(
                    workUnits[workUnitIndex],
                    workUnits[workUnitIndex].incomingDependencyEdges);
                for (const auto& bufferTransition : workUnitVisibilityInput.bufferVisibilityTransitions)
                {
                    if (!HasBufferVisibilityTransitionForResource(
                            visibilityInput,
                            bufferTransition.buffer))
                    {
                        visibilityInput.bufferVisibilityTransitions.push_back(bufferTransition);
                    }
                }
                for (const auto& textureTransition : workUnitVisibilityInput.textureVisibilityTransitions)
                {
                    if (!HasTextureVisibilityTransitionForResource(
                            visibilityInput,
                            textureTransition.texture,
                            textureTransition.subresourceRange))
                    {
                        visibilityInput.textureVisibilityTransitions.push_back(textureTransition);
                    }
                }
            }
            visibilityInput.debugName =
                workUnits.front().debugName + "InRenderPassChildGroupVisibility";
            return visibilityInput;
        }

        InRenderPassChildCommandGroupResult RecordInRenderPassChildCommandGroup(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
            const std::vector<ParallelCommandWorkUnit>& workUnits,
            RhiSubmissionFrame* submissionFrame)
        {
            InRenderPassChildCommandGroupResult result;
            if (frameContext.commandBuffer == nullptr ||
                frameContext.commandPool == nullptr ||
                workUnits.empty() ||
                submissionFrame == nullptr)
            {
                return result;
            }

            const auto sourcePassIndex = workUnits.front().sourcePassIndex;
            if (sourcePassIndex >= renderScenePackage.passCommandInputs.size())
                return result;

            const bool allWorkUnitsUseSameSourcePass = std::all_of(
                workUnits.begin(),
                workUnits.end(),
                [sourcePassIndex](const ParallelCommandWorkUnit& workUnit)
                {
                    return workUnit.usesInRenderPassChildCommandRecording &&
                        workUnit.sourcePassIndex == sourcePassIndex;
                });
            if (!allWorkUnitsUseSameSourcePass)
                return result;

            const auto& parentPassInput = renderScenePackage.passCommandInputs[static_cast<size_t>(sourcePassIndex)];
            if (!Detail::IsPassRecordable(renderScenePackage, parentPassInput))
                return result;

            if (parentPassInput.targetsSwapchain && frameContext.swapchainBackbufferView == nullptr)
                return result;

            auto recordedBatch = RecordInRenderPassChildCommandWorkUnits(
                impl,
                frameContext,
                renderScenePackage,
                workUnits,
                parentPassInput);
            for (const auto& recordedWorkUnit : recordedBatch.recordedWorkUnits)
            {
                if (recordedWorkUnit.recordingFailed)
                {
                    if (!IsChildRecordingFailureSerialFallbackSafe(recordedWorkUnit.failureReason))
                    {
                        MarkCommandRecordingFailure(
                            submissionFrame,
                            recordedWorkUnit.failureReason.empty()
                                ? "In-render-pass child command recording failed"
                                : recordedWorkUnit.failureReason.c_str());
                    }
                    result.failedBeforeParentPassOpened = true;
                    return result;
                }
            }
            if (recordedBatch.recordedWorkUnitCount != workUnits.size())
                return result;

            const auto visibilityInput = BuildInRenderPassChildGroupVisibilityInput(workUnits);
            if (Detail::HasResourceVisibilityTransitions(visibilityInput))
            {
                if (!frameContext.commandBuffer->IsRecording())
                    frameContext.commandBuffer->Begin();

                const bool recordedVisibilityTransitions = Detail::RecordResourceVisibilityTransitions(
                    *frameContext.commandBuffer,
                    visibilityInput,
                    &frameContext);
                if (!recordedVisibilityTransitions)
                {
                    MarkCommandRecordingFailure(
                        submissionFrame,
                        "Dependency visibility transition recording failed");
                    result.failedBeforeParentPassOpened = true;
                    return result;
                }
            }

            if (!frameContext.commandBuffer->IsRecording())
                frameContext.commandBuffer->Begin();

            const auto effectivePassInput = ResolveSwapchainDepthPassInput(parentPassInput, frameContext);
            if (!Detail::BeginPassCommandPlan(
                *frameContext.commandBuffer,
                frameContext.swapchainBackbufferView,
                frameContext.swapchainDepthStencilView,
                effectivePassInput,
                &frameContext))
            {
                MarkCommandRecordingFailure(submissionFrame, "BeginPassCommandPlan failed");
                return result;
            }
            result.failedAfterParentPassOpened = true;

            std::stable_sort(
                recordedBatch.recordedWorkUnits.begin(),
                recordedBatch.recordedWorkUnits.end(),
                [](const RecordedParallelCommandWorkUnit& lhs, const RecordedParallelCommandWorkUnit& rhs)
                {
                    return lhs.workUnit.sliceIndex < rhs.workUnit.sliceIndex;
                });

            bool executedAllChildren = true;
            for (const auto& recordedWorkUnit : recordedBatch.recordedWorkUnits)
            {
                result.childCommandPools.push_back(recordedWorkUnit.commandPool);
                result.childCommandBuffers.push_back(recordedWorkUnit.commandBuffer);
                const auto executeResult = frameContext.commandBuffer->ExecuteChildCommandBuffer(
                    recordedWorkUnit.commandBuffer);
                if (!executeResult.Succeeded())
                {
                    executedAllChildren = false;
                    MarkCommandRecordingFailure(
                        submissionFrame,
                        executeResult.message.empty()
                            ? "ExecuteChildCommandBuffer failed"
                            : executeResult.message.c_str());
                }
            }

            Detail::EndPassCommandPlan(*frameContext.commandBuffer, &effectivePassInput, &frameContext);

            if (!executedAllChildren)
                return result;

            submissionFrame->recordedPassCount += 1u;
            submissionFrame->recordedDrawCount += recordedBatch.recordedDrawCount;
            submissionFrame->recordedWorkUnitCount += recordedBatch.recordedWorkUnitCount;
            submissionFrame->parallelRecordingWorkerCount = std::max(
                submissionFrame->parallelRecordingWorkerCount,
                recordedBatch.parallelWorkerCountUsed);

            result.recorded = true;
            result.failedAfterParentPassOpened = false;
            result.recordedDrawCount = recordedBatch.recordedDrawCount;
            result.recordedWorkUnitCount = recordedBatch.recordedWorkUnitCount;
            result.parallelWorkerCountUsed = recordedBatch.parallelWorkerCountUsed;
            return result;
        }

        void MarkParallelCommandRecordingFailures(
            RhiSubmissionFrame* submissionFrame,
            const RecordedParallelCommandBufferBatch& recordedBatch)
        {
            if (submissionFrame == nullptr)
                return;

            for (const auto& recordedWorkUnit : recordedBatch.recordedWorkUnits)
            {
                if (!recordedWorkUnit.recordingFailed)
                    continue;

                MarkCommandRecordingFailure(
                    submissionFrame,
                    recordedWorkUnit.failureReason.empty()
                        ? "Parallel command recording failed"
                        : recordedWorkUnit.failureReason.c_str());
            }
        }

        struct MainCommandBufferSerialRecordResult
        {
            bool recordedAnyCommand = false;
            uint64_t recordedPassCount = 0u;
            uint64_t recordedDrawCount = 0u;
            uint64_t recordedWorkUnitCount = 0u;
            bool failed = false;
        };

        bool BeginMainCommandBufferIfNeeded(Render::RHI::RHIFrameContext& frameContext)
        {
            if (frameContext.commandBuffer == nullptr)
                return false;

            if (!frameContext.commandBuffer->IsRecording())
                frameContext.commandBuffer->Begin();
            return true;
        }

        MainCommandBufferSerialRecordResult RecordWorkUnitOnMainCommandBuffer(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
            const ParallelCommandWorkUnit& workUnit,
            RhiSubmissionFrame* submissionFrame,
            const size_t frameContextIndex,
            const std::optional<RenderPassCommandInput>& overridePassInput = std::nullopt)
        {
            MainCommandBufferSerialRecordResult result;
            const auto& passInput = overridePassInput.has_value()
                ? *overridePassInput
                : workUnit.commandInput;
            const bool isUiOverlayPass = passInput.kind == RenderPassCommandKind::UIOverlay;

            if (!isUiOverlayPass && !Detail::IsPassRecordable(renderScenePackage, passInput))
            {
                Detail::LogSkippedPass(
                    renderScenePackage,
                    passInput,
                    "IsPassRecordable returned false");
                return result;
            }

            if (passInput.kind == RenderPassCommandKind::Compute)
            {
                if (!BeginMainCommandBufferIfNeeded(frameContext))
                    return result;

                const auto* passProfileScopeName = Detail::ResolvePassProfileScopeName(passInput);
                NLS_PROFILE_NAMED_SCOPE(passProfileScopeName);
                frameContext.commandBuffer->BeginGpuProfileScope(passProfileScopeName, __FUNCTION__);
                const auto recordedDispatches = Detail::RecordComputeDispatches(
                    *frameContext.commandBuffer,
                    passInput.computeDispatchInputs,
                    false);
                frameContext.commandBuffer->EndGpuProfileScope();
                if (recordedDispatches == 0u)
                {
                    if (!passInput.computeDispatchInputs.empty())
                    {
                        Detail::LogSkippedPass(
                            renderScenePackage,
                            passInput,
                            "No dispatches recorded but commands were expected");
                        MarkCommandRecordingFailure(
                            submissionFrame,
                            "No dispatches recorded but commands were expected");
                        result.failed = true;
                    }
                    return result;
                }

                result.recordedAnyCommand = true;
                result.recordedPassCount = 1u;
                result.recordedWorkUnitCount = 1u;
                return result;
            }

            if (!isUiOverlayPass)
            {
                const auto visibilityInput = Detail::BuildDependencyVisibilityPassInput(
                    workUnit,
                    workUnit.incomingDependencyEdges);
                if (Detail::HasResourceVisibilityTransitions(visibilityInput))
                {
                    if (!BeginMainCommandBufferIfNeeded(frameContext))
                        return result;

                    const bool recordedVisibilityTransitions = Detail::RecordResourceVisibilityTransitions(
                        *frameContext.commandBuffer,
                        visibilityInput,
                        &frameContext);
                    result.recordedAnyCommand |= recordedVisibilityTransitions;
                    if (!recordedVisibilityTransitions)
                    {
                        MarkCommandRecordingFailure(
                            submissionFrame,
                            "Dependency visibility transition recording failed");
                        result.failed = true;
                        return result;
                    }
                }
            }

            if (passInput.targetsSwapchain &&
                frameContext.swapchainBackbufferView == nullptr)
            {
                Detail::LogSkippedPass(
                    renderScenePackage,
                    passInput,
                    "swapchainBackbufferView is null");
                result.recordedPassCount = 1u;
                result.recordedWorkUnitCount = 1u;
                return result;
            }

            if (!BeginMainCommandBufferIfNeeded(frameContext))
                return result;

            const auto effectivePassInput = ResolveSwapchainDepthPassInput(passInput, frameContext);
            std::optional<RenderPassCommandInput> preparedUiOverlayPassInput;
            if (isUiOverlayPass)
            {
                std::string prepareError;
                preparedUiOverlayPassInput = PrepareUiOverlayPassInput(
                    impl,
                    frameContext,
                    *frameContext.commandBuffer,
                    effectivePassInput,
                    frameContextIndex,
                    prepareError);
                if (!preparedUiOverlayPassInput.has_value())
                {
                    Detail::LogSkippedPass(
                        renderScenePackage,
                        effectivePassInput,
                        prepareError.empty()
                            ? "UI overlay resource preparation failed"
                            : prepareError.c_str());
                    MarkCommandRecordingFailure(
                        submissionFrame,
                        prepareError.empty()
                            ? "UI overlay resource preparation failed"
                            : prepareError.c_str());
                    result.failed = true;
                    return result;
                }
            }
            const auto& recordablePassInput = preparedUiOverlayPassInput.has_value()
                ? *preparedUiOverlayPassInput
                : effectivePassInput;
            if (isUiOverlayPass)
            {
                auto visibilityWorkUnit = workUnit;
                visibilityWorkUnit.commandInput = recordablePassInput;
                const auto visibilityInput = Detail::BuildDependencyVisibilityPassInput(
                    visibilityWorkUnit,
                    visibilityWorkUnit.incomingDependencyEdges);
                if (Detail::HasResourceVisibilityTransitions(visibilityInput))
                {
                    const bool recordedVisibilityTransitions = Detail::RecordResourceVisibilityTransitions(
                        *frameContext.commandBuffer,
                        visibilityInput,
                        &frameContext);
                    result.recordedAnyCommand |= recordedVisibilityTransitions;
                    if (!recordedVisibilityTransitions)
                    {
                        MarkCommandRecordingFailure(
                            submissionFrame,
                            "Dependency visibility transition recording failed");
                        result.failed = true;
                        return result;
                    }
                }
            }
            if (!Detail::BeginPassCommandPlan(
                *frameContext.commandBuffer,
                frameContext.swapchainBackbufferView,
                frameContext.swapchainDepthStencilView,
                recordablePassInput,
                &frameContext))
            {
                Detail::LogSkippedPass(
                    renderScenePackage,
                    recordablePassInput,
                    "BeginPassCommandPlan failed");
                MarkCommandRecordingFailure(
                    submissionFrame,
                    "BeginPassCommandPlan failed");
                result.failed = true;
                return result;
            }

            result.recordedAnyCommand = true;
            const auto* effectivePassProfileScopeName =
                Detail::ResolvePassProfileScopeName(recordablePassInput);
            NLS_PROFILE_NAMED_SCOPE(effectivePassProfileScopeName);
            frameContext.commandBuffer->BeginGpuProfileScope(effectivePassProfileScopeName, __FUNCTION__);
            uint64_t recordedDrawCount = 0u;
            const bool recordsUiOverlay =
                recordablePassInput.kind == RenderPassCommandKind::UIOverlay;
            const bool recordsSlicedSourceRange =
                !recordsUiOverlay &&
                !overridePassInput.has_value() &&
                workUnit.requiresOrderedSlicedSubmission &&
                workUnit.sourcePassIndex < renderScenePackage.passCommandInputs.size();
            if (recordsUiOverlay)
            {
                const auto uiResult = impl.uiOverlayRenderer.RecordPrepared(
                    *frameContext.commandBuffer,
                    *recordablePassInput.uiDrawDataSnapshot,
                    frameContextIndex);
                if (!uiResult.success)
                {
                    frameContext.commandBuffer->EndGpuProfileScope();
                    Detail::EndPassCommandPlan(*frameContext.commandBuffer, &recordablePassInput, &frameContext);
                    Detail::LogSkippedPass(
                        renderScenePackage,
                        recordablePassInput,
                        uiResult.message.empty()
                            ? "UI overlay recording failed"
                            : uiResult.message.c_str());
                    MarkCommandRecordingFailure(
                        submissionFrame,
                        uiResult.message.empty()
                            ? "UI overlay recording failed"
                            : uiResult.message.c_str());
                    result.failed = true;
                    return result;
                }
                recordedDrawCount = uiResult.recordedDraws
                    ? effectivePassInput.drawCount
                    : 0u;
            }
            else
            {
                recordedDrawCount = recordsSlicedSourceRange
                    ? Detail::RecordPreparedDrawCommandsForPassRange(
                        frameContext.commandBuffer.get(),
                        recordablePassInput,
                        renderScenePackage.passCommandInputs[static_cast<size_t>(workUnit.sourcePassIndex)]
                            .recordedDrawCommands,
                        workUnit.recordedDrawBegin,
                        workUnit.recordedDrawCount)
                    : Detail::RecordPreparedDrawCommandsForPass(
                        frameContext.commandBuffer.get(),
                        recordablePassInput);
            }
            frameContext.commandBuffer->EndGpuProfileScope();
            Detail::EndPassCommandPlan(*frameContext.commandBuffer, &recordablePassInput, &frameContext);
            if (!recordsUiOverlay &&
                recordedDrawCount == 0u &&
                (!recordablePassInput.recordedDrawCommands.empty() ||
                    (recordsSlicedSourceRange && workUnit.recordedDrawCount > 0u)))
            {
                Detail::LogSkippedPass(
                    renderScenePackage,
                    recordablePassInput,
                    "No draws recorded but commands were expected");
                MarkCommandRecordingFailure(
                    submissionFrame,
                    "No draws recorded but commands were expected");
                result.failed = true;
                return result;
            }

            result.recordedPassCount = 1u;
            result.recordedDrawCount = recordedDrawCount;
            result.recordedWorkUnitCount = 1u;
            return result;
        }

        void AccumulateSerialRecordResult(
            RhiSubmissionFrame* submissionFrame,
            const MainCommandBufferSerialRecordResult& result)
        {
            if (submissionFrame == nullptr)
                return;

            submissionFrame->recordedPassCount += result.recordedPassCount;
            submissionFrame->recordedDrawCount += result.recordedDrawCount;
            submissionFrame->recordedWorkUnitCount += result.recordedWorkUnitCount;
        }

        bool RecordOrderedInRenderPassChildAndSerialWorkUnits(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
            const std::vector<ParallelCommandWorkUnit>& workUnits,
            Detail::AsyncComputeSubmitPlan* submitPlan,
            RhiSubmissionFrame* submissionFrame,
            const size_t frameContextIndex,
            const ThreadedRhiSubmissionExecutionPtr& execution)
        {
            if (submitPlan == nullptr ||
                submissionFrame == nullptr ||
                frameContext.commandBuffer == nullptr)
            {
                return false;
            }

            bool recordedAnyCommand = false;
            bool recordedChildCommandGroup = false;
            bool recordedSerialWorkUnit = false;
            // The acquisition is resolved lazily at the first work unit that
            // consumes the backbuffer. Earlier view-independent work units record
            // while the ordered lane is still draining the previous frame.
            bool swapchainAcquisitionResolved = false;
            const auto ensureSwapchainAcquisition =
                [&execution, &swapchainAcquisitionResolved]()
                {
                    if (swapchainAcquisitionResolved)
                        return true;
                    swapchainAcquisitionResolved = true;
                    return Detail::ResolveThreadedSwapchainAcquisition(execution);
                };
            std::vector<std::shared_ptr<Render::RHI::RHICommandPool>> childCommandPoolKeepAlive;
            std::vector<std::shared_ptr<Render::RHI::RHICommandBuffer>> childCommandBufferKeepAlive;
            for (size_t workUnitIndex = 0u; workUnitIndex < workUnits.size();)
            {
                const auto& workUnit = workUnits[workUnitIndex];
                if (!workUnit.usesInRenderPassChildCommandRecording)
                {
                    if (workUnit.commandInput.targetsSwapchain && !ensureSwapchainAcquisition())
                        return false;
                    const auto result = RecordWorkUnitOnMainCommandBuffer(
                        impl,
                        frameContext,
                        renderScenePackage,
                        workUnit,
                        submissionFrame,
                        frameContextIndex);
                    AccumulateSerialRecordResult(submissionFrame, result);
                    recordedAnyCommand |= result.recordedAnyCommand;
                    recordedSerialWorkUnit |= result.recordedAnyCommand;
                    if (result.failed)
                        return false;
                    ++workUnitIndex;
                    continue;
                }

                std::vector<ParallelCommandWorkUnit> childGroup;
                const auto sourcePassIndex = workUnit.sourcePassIndex;
                while (workUnitIndex < workUnits.size() &&
                    workUnits[workUnitIndex].usesInRenderPassChildCommandRecording &&
                    workUnits[workUnitIndex].sourcePassIndex == sourcePassIndex)
                {
                    childGroup.push_back(workUnits[workUnitIndex]);
                    ++workUnitIndex;
                }

                const bool childGroupTargetsSwapchain =
                    sourcePassIndex < renderScenePackage.passCommandInputs.size() &&
                    renderScenePackage.passCommandInputs[static_cast<size_t>(sourcePassIndex)]
                        .targetsSwapchain;
                if (childGroupTargetsSwapchain && !ensureSwapchainAcquisition())
                    return false;

                auto childGroupResult = RecordInRenderPassChildCommandGroup(
                    impl,
                    frameContext,
                    renderScenePackage,
                    childGroup,
                    submissionFrame);
                if (childGroupResult.recorded)
                {
                    recordedAnyCommand = true;
                    recordedChildCommandGroup = true;
                    childCommandPoolKeepAlive.insert(
                        childCommandPoolKeepAlive.end(),
                        childGroupResult.childCommandPools.begin(),
                        childGroupResult.childCommandPools.end());
                    childCommandBufferKeepAlive.insert(
                        childCommandBufferKeepAlive.end(),
                        childGroupResult.childCommandBuffers.begin(),
                        childGroupResult.childCommandBuffers.end());
                    continue;
                }

                if (childGroupResult.failedAfterParentPassOpened ||
                    (childGroupResult.failedBeforeParentPassOpened &&
                        HasCommandRecordingFailure(submissionFrame)))
                {
                    return false;
                }

                if (sourcePassIndex >= renderScenePackage.passCommandInputs.size())
                    return false;

                ParallelCommandWorkUnit fallbackWorkUnit = childGroup.front();
                fallbackWorkUnit.requiresOrderedSlicedSubmission = false;
                fallbackWorkUnit.usesInRenderPassChildCommandRecording = false;
                fallbackWorkUnit.recordedDrawBegin = 0u;
                fallbackWorkUnit.recordedDrawCount = static_cast<uint64_t>(
                    renderScenePackage.passCommandInputs[static_cast<size_t>(sourcePassIndex)]
                        .recordedDrawCommands.size());

                if (submissionFrame->parallelFallbackReason.empty())
                {
                    submissionFrame->parallelFallbackReason =
                        "in-render-pass child command recording failed; using unsliced passCommandInputs";
                }

                const auto result = RecordWorkUnitOnMainCommandBuffer(
                    impl,
                    frameContext,
                    renderScenePackage,
                    fallbackWorkUnit,
                    submissionFrame,
                    frameContextIndex,
                    renderScenePackage.passCommandInputs[static_cast<size_t>(sourcePassIndex)]);
                AccumulateSerialRecordResult(submissionFrame, result);
                recordedAnyCommand |= result.recordedAnyCommand;
                recordedSerialWorkUnit |= result.recordedAnyCommand;
                if (result.failed)
                    return false;
            }

            const auto extractionVisibilityInput =
                Render::FrameGraph::BuildExtractionVisibilityPassInput(renderScenePackage);
            if (Detail::HasResourceVisibilityTransitions(extractionVisibilityInput))
            {
                if (!BeginMainCommandBufferIfNeeded(frameContext))
                    return false;

                const bool recordedPostPassTransitions = Detail::RecordResourceVisibilityTransitions(
                    *frameContext.commandBuffer,
                    extractionVisibilityInput,
                    &frameContext);
                recordedAnyCommand |= recordedPostPassTransitions;
                if (!recordedPostPassTransitions)
                {
                    MarkCommandRecordingFailure(
                        submissionFrame,
                        "Post-pass transition recording failed");
                    return false;
                }
            }

            submissionFrame->usedParallelCommandPath = recordedChildCommandGroup;
            submissionFrame->usedSerialCommandPath =
                submissionFrame->usedSerialCommandPath ||
                recordedSerialWorkUnit;
            submissionFrame->usedTranslationMerge = false;
            submissionFrame->translatedWorkUnitCount = 0u;
            if (recordedAnyCommand)
            {
                if (frameContext.commandBuffer->IsRecording())
                    frameContext.commandBuffer->End();

                Detail::ThreadedQueueSubmissionBatch batch;
                batch.queueType = Render::RHI::QueueType::Graphics;
                batch.consumesSwapchainImage = renderScenePackage.targetsSwapchain;
                if (frameContext.commandPool != nullptr)
                    batch.commandPools.push_back(frameContext.commandPool);
                batch.commandPools.insert(
                    batch.commandPools.end(),
                    childCommandPoolKeepAlive.begin(),
                    childCommandPoolKeepAlive.end());
                batch.commandBuffers.push_back(frameContext.commandBuffer);
                batch.childCommandBuffers = std::move(childCommandBufferKeepAlive);
                submitPlan->batches.push_back(std::move(batch));
            }

            if (renderScenePackage.targetsSwapchain && !ensureSwapchainAcquisition())
                return false;

            return true;
        }

        Detail::ThreadedCommandSubmissionUnit BuildSubmissionUnitFromRecordedWorkUnit(
            const RecordedParallelCommandWorkUnit& recordedWorkUnit,
            std::vector<WorkUnitDependencyEdge> incomingDependencyEdges,
            const std::optional<uint64_t> dependencySourceSubmissionOrder,
            const bool requiresDependencyVisibility)
        {
            Detail::ThreadedCommandSubmissionUnit submission;
            submission.submissionOrder = recordedWorkUnit.workUnit.submissionOrder;
            submission.queueType = recordedWorkUnit.workUnit.commandInput.queueType;
            submission.queueDependencyPolicy =
                recordedWorkUnit.workUnit.commandInput.queueDependencyPolicy;
            submission.dependencySourceSubmissionOrder = dependencySourceSubmissionOrder;
            submission.queueDependencySourceSubmissionOrders =
                Detail::BuildQueueDependencySourceOrders(incomingDependencyEdges);
            submission.incomingDependencyEdges = std::move(incomingDependencyEdges);
            submission.requiresDependencyVisibility = requiresDependencyVisibility;
            submission.targetsSwapchain =
                recordedWorkUnit.workUnit.commandInput.targetsSwapchain;
            submission.commandPool = recordedWorkUnit.commandPool;
            submission.commandBuffer = recordedWorkUnit.commandBuffer;
            return submission;
        }

        bool AppendRecordedParallelCommandWorkUnit(
            TranslatedParallelCommandBufferBatch& batch,
            const RecordedParallelCommandWorkUnit& recordedWorkUnit,
            std::vector<WorkUnitDependencyEdge> incomingDependencyEdges = {},
            std::optional<uint64_t> dependencySourceSubmissionOrder = std::nullopt,
            std::optional<bool> requiresDependencyVisibility = std::nullopt)
        {
            if (!recordedWorkUnit.wasRecorded ||
                recordedWorkUnit.commandPool == nullptr ||
                recordedWorkUnit.commandBuffer == nullptr)
            {
                return false;
            }

            if (incomingDependencyEdges.empty())
                incomingDependencyEdges = recordedWorkUnit.workUnit.incomingDependencyEdges;
            if (!dependencySourceSubmissionOrder.has_value())
            {
                dependencySourceSubmissionOrder =
                    Detail::ResolveImplicitDependencySourceSubmissionOrder(
                        recordedWorkUnit.workUnit,
                        incomingDependencyEdges);
            }

            auto submission = BuildSubmissionUnitFromRecordedWorkUnit(
                recordedWorkUnit,
                std::move(incomingDependencyEdges),
                dependencySourceSubmissionOrder,
                requiresDependencyVisibility.value_or(
                    recordedWorkUnit.workUnit.commandInput.requiresDependencyVisibility));
            batch.submissions.push_back(std::move(submission));
            batch.recordedPassCount += recordedWorkUnit.recordedPassCount;
            batch.recordedDrawCount += recordedWorkUnit.recordedDrawCount;
            batch.recordedComputeDispatchCount += recordedWorkUnit.recordedComputeDispatchCount;
            batch.recordedWorkUnitCount += 1u;
            return true;
        }


        TranslatedParallelCommandBufferBatch BuildPreparedCommandSubmissionUnits(
            RecordedParallelCommandBufferBatch& recordedBatch)
        {
            TranslatedParallelCommandBufferBatch translatedBatch;
            struct PendingTranslatedUnit
            {
                RecordedParallelCommandWorkUnit* recordedWorkUnit = nullptr;
                std::vector<WorkUnitDependencyEdge> incomingDependencyEdges;
                std::optional<uint64_t> dependencySourceSubmissionOrder;
                bool requiresDependencyVisibility = false;
            };

            std::vector<RecordedParallelCommandWorkUnit*> orderedRecordedWorkUnits;
            orderedRecordedWorkUnits.reserve(recordedBatch.recordedWorkUnits.size());
            for (auto& recordedWorkUnit : recordedBatch.recordedWorkUnits)
            {
                if (recordedWorkUnit.wasRecorded)
                    orderedRecordedWorkUnits.push_back(&recordedWorkUnit);
            }

            std::stable_sort(
                orderedRecordedWorkUnits.begin(),
                orderedRecordedWorkUnits.end(),
                [](const RecordedParallelCommandWorkUnit* lhs, const RecordedParallelCommandWorkUnit* rhs)
                {
                    return lhs->workUnit.submissionOrder < rhs->workUnit.submissionOrder;
                });

            std::vector<PendingTranslatedUnit> pendingTranslatedUnits;
            pendingTranslatedUnits.reserve(orderedRecordedWorkUnits.size());
            const auto flushPendingTranslatedUnits = [&translatedBatch, &pendingTranslatedUnits]()
            {
                if (pendingTranslatedUnits.empty())
                    return;

                for (auto& translatedUnit : pendingTranslatedUnits)
                {
                    if (AppendRecordedParallelCommandWorkUnit(
                        translatedBatch,
                        *translatedUnit.recordedWorkUnit,
                        std::move(translatedUnit.incomingDependencyEdges),
                        translatedUnit.dependencySourceSubmissionOrder,
                        translatedUnit.requiresDependencyVisibility))
                    {
                        translatedBatch.translatedWorkUnitCount += 1u;
                    }
                }
                pendingTranslatedUnits.clear();
                translatedBatch.usedTranslationMerge = true;
            };

            for (auto* recordedWorkUnit : orderedRecordedWorkUnits)
            {
                const auto resourceDependencyEdges = Detail::FilterWorkUnitDependencyEdges(
                    recordedWorkUnit->workUnit,
                    [](const WorkUnitDependencyEdge& edge)
                    {
                        return edge.resourceKind != ThreadedDependencyResourceKind::None;
                    });
                const auto controlDependencyEdges = Detail::FilterWorkUnitDependencyEdges(
                    recordedWorkUnit->workUnit,
                    [](const WorkUnitDependencyEdge& edge)
                    {
                        return edge.resourceKind == ThreadedDependencyResourceKind::None;
                    });
                const auto dependencySourceSubmissionOrder =
                    !controlDependencyEdges.empty()
                        ? std::optional<uint64_t>(controlDependencyEdges.front().sourceWorkUnitIndex)
                        : Detail::ResolveImplicitDependencySourceWorkUnitIndex(
                            recordedWorkUnit->workUnit);
                const bool requiresDependencyVisibility =
                    Detail::ResolveImplicitRequiresDependencyVisibility(
                        recordedWorkUnit->workUnit);
                auto targetDependencyEdges = std::move(controlDependencyEdges);
                targetDependencyEdges.insert(
                    targetDependencyEdges.end(),
                    resourceDependencyEdges.begin(), resourceDependencyEdges.end());

                if (recordedWorkUnit->workUnit.eligibleForParallelTranslation)
                {
                    pendingTranslatedUnits.push_back({
                        recordedWorkUnit,
                        std::move(targetDependencyEdges),
                        dependencySourceSubmissionOrder,
                        requiresDependencyVisibility
                    });
                }
                else
                {
                    flushPendingTranslatedUnits();
                    AppendRecordedParallelCommandWorkUnit(
                        translatedBatch,
                        *recordedWorkUnit,
                        std::move(targetDependencyEdges),
                        dependencySourceSubmissionOrder,
                        requiresDependencyVisibility);
                }
            }

            flushPendingTranslatedUnits();
            if (recordedBatch.postPassTransitions.has_value())
            {
                auto& post = *recordedBatch.postPassTransitions;
                if (post.recordingFailed)
                    translatedBatch.commandRecordingFailureReasons.push_back(post.failureReason);
                else if (post.wasRecorded)
                {
                    Detail::ThreadedCommandSubmissionUnit submission;
                    submission.submissionOrder = std::numeric_limits<uint64_t>::max();
                    submission.queueType = Render::RHI::QueueType::Graphics;
                    submission.queueDependencyPolicy = QueueDependencyPolicy::Previous;
                    submission.commandPool = post.commandPool;
                    submission.commandBuffer = post.commandBuffer;
                    translatedBatch.submissions.push_back(std::move(submission));
                }
            }
            if (std::any_of(orderedRecordedWorkUnits.begin(), orderedRecordedWorkUnits.end(),
                [](const auto* unit) { return unit->queueRelease != nullptr && unit->queueRelease->wasRecorded; }))
            {
                std::vector<Detail::ThreadedCommandSubmissionUnit> submissions;
                std::unordered_map<uint64_t, uint64_t> remappedOrders;
                std::unordered_map<uint64_t, uint64_t> releaseOrders;
                std::unordered_set<RHI::RHICommandBuffer*> releaseBuffers;
                for (auto& submission : translatedBatch.submissions)
                {
                    const auto unit = std::find_if(orderedRecordedWorkUnits.begin(), orderedRecordedWorkUnits.end(),
                        [&](const auto* recorded) { return recorded->workUnit.submissionOrder == submission.submissionOrder; });
                    if (unit != orderedRecordedWorkUnits.end() && (*unit)->queueRelease != nullptr &&
                        (*unit)->queueRelease->wasRecorded)
                    {
                        const auto& release = *(*unit)->queueRelease;
                        Detail::ThreadedCommandSubmissionUnit prerequisite;
                        prerequisite.submissionOrder = submissions.size();
                        prerequisite.queueType = RHI::QueueType::Graphics;
                        prerequisite.queueDependencyPolicy = QueueDependencyPolicy::LastCompute;
                        prerequisite.commandPool = release.commandPool;
                        prerequisite.commandBuffer = release.commandBuffer;
                        releaseBuffers.insert(release.commandBuffer.get());
                        releaseOrders.emplace(submission.submissionOrder, prerequisite.submissionOrder);
                        submissions.push_back(std::move(prerequisite));
                    }
                    remappedOrders.emplace(submission.submissionOrder, submissions.size());
                    submissions.push_back(std::move(submission));
                }
                for (auto& submission : submissions)
                {
                    // Release lists already have their final order and wait for the
                    // previous Compute batch; consumers also wait for that release.
                    if (releaseBuffers.find(submission.commandBuffer.get()) != releaseBuffers.end())
                        continue;
                    const auto oldOrder = submission.submissionOrder;
                    submission.submissionOrder = remappedOrders.at(oldOrder);
                    if (submission.dependencySourceSubmissionOrder.has_value())
                    {
                        const auto source = remappedOrders.find(*submission.dependencySourceSubmissionOrder);
                        submission.dependencySourceSubmissionOrder = source != remappedOrders.end()
                            ? std::optional<uint64_t>(source->second) : std::nullopt;
                    }
                    std::erase_if(submission.queueDependencySourceSubmissionOrders,
                        [&](const auto source) { return remappedOrders.find(source) == remappedOrders.end(); });
                    for (auto& source : submission.queueDependencySourceSubmissionOrders)
                        source = remappedOrders.at(source);
                    if (const auto release = releaseOrders.find(oldOrder); release != releaseOrders.end())
                    {
                        submission.queueDependencySourceSubmissionOrders.push_back(release->second);
                        submission.queueDependencyPolicy = QueueDependencyPolicy::Previous;
                    }
                }
                translatedBatch.submissions = std::move(submissions);
            }
            return translatedBatch;
        }
    }

    Render::RHI::RHIFrameContext* Detail::BeginThreadedRhiFrame(
        DriverImpl& impl,
        const RenderScenePackage& renderScenePackage,
        const size_t frameContextIndex,
        RhiSubmissionFrame* submissionFrame,
        Render::RHI::ResourceStateTrackerStats* preResetTrackerStats,
        Render::RHI::DescriptorAllocatorStats* descriptorStats,
        uint64_t* resourceLeaseGeneration,
        const std::shared_ptr<Detail::RhiFrameQueueOperationTelemetry>& queueTelemetry,
        const std::shared_ptr<Detail::ThreadedSwapchainAcquisition>& swapchainAcquisition)
    {
        NLS_PROFILE_SCOPE();
        if (impl.explicitDevice == nullptr || impl.GetFrameContextCount() == 0u)
        {
            // Silent rejection here makes a blanket "every later frame failed"
            // cascade impossible to attribute, so each precondition names itself.
            NLS_LOG_WARNING(
                std::string("RhiCommandListExecutor::BeginThreadedRhiFrame rejected: ") +
                (impl.explicitDevice == nullptr ? "no explicit device" : "empty frame context ring"));
            return nullptr;
        }
        if (impl.deviceLostDetected.load(std::memory_order_acquire) ||
            impl.unsafeGpuWorkQuarantined.load(std::memory_order_acquire))
        {
            NLS_LOG_WARNING(
                std::string("RhiCommandListExecutor::BeginThreadedRhiFrame rejected: ") +
                (impl.deviceLostDetected.load(std::memory_order_acquire)
                    ? "device lost: " + impl.deviceLostReason
                    : "unsafe GPU work quarantined: " + impl.unsafeGpuWorkQuarantineReason));
            return nullptr;
        }

        if (frameContextIndex >= impl.GetFrameContextCount())
        {
            if (submissionFrame != nullptr)
            {
                submissionFrame->frameContextIndex = static_cast<uint32_t>(frameContextIndex);
                MarkCommandRecordingFailure(submissionFrame, "Threaded RHI frame context index out of range");
            }
            return nullptr;
        }

        auto& frameContext = impl.GetFrameContext(frameContextIndex);
        ResetFrameQueueOperationTelemetry(impl, queueTelemetry);

        if (submissionFrame != nullptr)
        {
            submissionFrame->frameContextIndex = static_cast<uint32_t>(frameContextIndex);
            submissionFrame->asyncComputeCandidateWorkloadCount =
                ResolveAsyncComputeCandidateWorkloadCount(renderScenePackage);

            auto graphicsQueue = impl.explicitDevice->GetQueue(Render::RHI::QueueType::Graphics);
            auto computeQueue = impl.explicitDevice->GetQueue(Render::RHI::QueueType::Compute);
            submissionFrame->asyncComputeQueueAvailable =
                graphicsQueue != nullptr &&
                computeQueue != nullptr &&
                graphicsQueue.get() != computeQueue.get();
        }

        std::shared_ptr<Render::RHI::RHIFence> retirementFence;
        {
            std::lock_guard retirementLock(impl.frameResourceRetirementMutex);
            const auto* slot = impl.frameResourceSlots->PeekSlot(frameContextIndex);
            if (slot == nullptr || slot->quarantined)
                return nullptr;
            retirementFence = slot->gpuFence;
        }
        if (retirementFence != nullptr)
        {
            NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::WaitReusableFrameFence");
            if (!WaitFrameFenceWithDriverPolicy(
                impl,
                retirementFence,
                "RhiCommandListExecutor::BeginThreadedRhiFrame",
                queueTelemetry))
            {
                return nullptr;
            }
            Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(impl, frameContext, frameContextIndex);
        }
        // Admission may already have reclaimed the slot. Release executor owners
        // before its reusable fence object is reset for the next GPU submission.
        (void)RhiCommandListExecutor::Get().ReleaseRetiredBatches();
        {
            std::lock_guard retirementLock(impl.frameResourceRetirementMutex);
            auto* slot = impl.frameResourceSlots->PeekSlot(frameContextIndex);
            if (slot == nullptr || slot->quarantined || slot->retirementPending)
                return nullptr;
            if ((!slot->inFlight || slot->gpuFence != nullptr) &&
                impl.frameResourceSlots->AcquireSlot(frameContextIndex) == nullptr)
                return nullptr;
            if (resourceLeaseGeneration != nullptr)
                *resourceLeaseGeneration = slot->leaseGeneration;
        }
        frameContext.frameIndex = impl.nextRhiFrameIndex.fetch_add(1u, std::memory_order_relaxed);
        if (frameContext.imageAcquiredSemaphore != nullptr)
            frameContext.imageAcquiredSemaphore->Reset();
        if (frameContext.renderFinishedSemaphore != nullptr)
            frameContext.renderFinishedSemaphore->Reset();
        ClearSceneToUiWaitSemaphoreForFrame(impl, frameContext);
        {
            NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::ResetCommandResources");
            if (frameContext.commandPool != nullptr)
            {
                NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::ResetMainCommandPool");
                frameContext.commandPool->Reset();
            }
            if (frameContext.commandBuffer != nullptr)
            {
                NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::ResetMainCommandBuffer");
                frameContext.commandBuffer->Reset();
            }
            // Keep command-pool reset semantics for RHI implementations that use
            // pool-owned allocation state. The command buffers themselves are
            // reset on demand when their work unit is prepared below.
            {
                NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::ResetParallelCommandPools");
                for (const auto& parallelCommandPool : frameContext.parallelCommandPools)
                {
                    if (parallelCommandPool != nullptr)
                        parallelCommandPool->Reset();
                }
            }
            {
                NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::ResetChildCommandPools");
                for (const auto& childCommandPool : frameContext.childCommandPools)
                {
                    if (childCommandPool != nullptr)
                        childCommandPool->Reset();
                }
            }
            // Parallel and in-render-pass child command buffers are reset when their
            // work unit is prepared for this frame. Resetting every retained buffer
            // here repeats that work and can turn camera motion into a long CPU spike
            // when a previous frame created many command buffers.
        }

        if (preResetTrackerStats != nullptr)
            *preResetTrackerStats = Render::RHI::ResourceStateTrackerStats{};
        if (frameContext.resourceStateTracker != nullptr)
        {
            NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::ResetResourceStateTracker");
            if (submissionFrame != nullptr)
                submissionFrame->usedResourceStateTracker = true;
            frameContext.resourceStateTracker->RetireTransientResources(std::numeric_limits<uint64_t>::max());
            if (preResetTrackerStats != nullptr)
                *preResetTrackerStats = frameContext.resourceStateTracker->GetStats();
            frameContext.resourceStateTracker->Reset();
            frameContext.resourceStateTracker->BeginFrame(renderScenePackage.frameId);
        }

        if (descriptorStats != nullptr)
            *descriptorStats = Render::RHI::DescriptorAllocatorStats{};
        if (frameContext.descriptorAllocator != nullptr)
        {
            NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::BeginDescriptorAllocatorFrame");
            if (submissionFrame != nullptr)
                submissionFrame->usedDescriptorAllocator = true;
            frameContext.descriptorAllocator->BeginFrame(frameContext.frameIndex);
            if (descriptorStats != nullptr)
                *descriptorStats = frameContext.descriptorAllocator->GetStats();
        }
        if (frameContext.uploadContext != nullptr)
            frameContext.uploadContext->BeginFrame(frameContext.frameIndex);
        if (frameContext.computeFinishedSemaphore != nullptr)
            frameContext.computeFinishedSemaphore->Reset();

        frameContext.hasAcquiredSwapchainImage = false;
        frameContext.uploadBytesReserved = 0u;
        frameContext.swapchainBackbufferView = nullptr;
        frameContext.explicitReadbackTexture = nullptr;
        frameContext.explicitReadbackTextureGeneration = 0u;
        if (renderScenePackage.targetsSwapchain && impl.explicitSwapchain != nullptr)
        {
            if (swapchainAcquisition != nullptr)
            {
                // Enqueue now and bind the view at the first consumer. The ordered
                // lane can process the acquisition while this frame still builds
                // work units and prepares command resources, instead of stalling in
                // the lane here before any of that work has run.
                auto acquisition = swapchainAcquisition;
                acquisition->laneOperation = RhiCommandListExecutor::Get().EnqueueOnSubmitLane(
                    "RHIFrame::AcquireSwapchainImage",
                    [acquisition, frameContextPtr = &frameContext, implPtr = &impl]
                    {
                        NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::AcquireNextImage");
                        // The swapchain can be rebuilt between enqueue and execution: a
                        // client resize or a surface-driven recreation (present returned
                        // VK_ERROR_OUT_OF_DATE_KHR) destroys the handle this frame was
                        // published against. Acquire from whatever the driver owns now so
                        // the lane never touches a destroyed swapchain.
                        const auto swapchain = implPtr->explicitSwapchain;
                        if (swapchain == nullptr)
                        {
                            RHI::RHIQueueOperationResult result;
                            result.code = RHI::RHIQueueOperationStatusCode::InvalidArgument;
                            result.message = "swapchain is unavailable for image acquisition";
                            return result;
                        }
                        const auto acquiredImage = swapchain->AcquireNextImage(
                            frameContextPtr->imageAcquiredSemaphore,
                            frameContextPtr->frameFence);
                        if (acquiredImage.has_value())
                        {
                            acquisition->imageIndex = acquiredImage->imageIndex;
                            acquisition->backbufferView =
                                swapchain->GetBackbufferView(acquiredImage->imageIndex);
                            acquisition->hasImage = true;
                            return RHI::RHIQueueOperationResult {};
                        }
                        // No image means the swapchain the acquire ran against no longer
                        // matched the surface. Reporting success here silently produced a
                        // null backbuffer view later, so fail the frame with the real
                        // reason and let the resize path rebuild the swapchain.
                        RHI::RHIQueueOperationResult result;
                        result.code = RHI::RHIQueueOperationStatusCode::SwapchainOutOfDate;
                        result.message = "swapchain image acquisition returned no image";
                        return result;
                    });
                acquisition->enqueued = true;
                frameContext.explicitReadbackTexture =
                    Render::FrameGraph::ResolveFrameReadbackTexture(&renderScenePackage, &frameContext);
                frameContext.explicitReadbackTextureGeneration =
                    Render::FrameGraph::ResolveFrameReadbackTextureGeneration(&renderScenePackage, &frameContext);
                if (impl.renderDocCaptureController != nullptr)
                {
                    impl.renderDocCaptureController->OnPreFrame(
                        renderScenePackage.targetsSwapchain,
                        Render::FrameGraph::HasExternalSceneOutput(renderScenePackage));
                }
                return &frameContext;
            }

            std::optional<Render::RHI::RHIAcquiredImage> acquiredImage;
            std::shared_ptr<Render::RHI::RHITextureView> backbufferView;
            // DX12's current image advances at Present. Acquire shares the FIFO
            // with earlier submissions and presents before workers use its view.
            const auto acquireResult = RhiCommandListExecutor::Get().RunOnSubmitLane(
                "RHIFrame::AcquireSwapchainImage",
                [&, swapchain = impl.explicitSwapchain]
                {
                    NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::AcquireNextImage");
                    acquiredImage = swapchain->AcquireNextImage(
                        frameContext.imageAcquiredSemaphore,
                        frameContext.frameFence);
                    if (acquiredImage.has_value())
                        backbufferView = swapchain->GetBackbufferView(acquiredImage->imageIndex);
                    return Render::RHI::RHIQueueOperationResult {};
                });
            if (!RecordQueueOperationResult(
                impl, acquireResult, "RHIFrame::AcquireSwapchainImage", queueTelemetry))
            {
                MarkCommandRecordingFailure(submissionFrame, acquireResult.message.c_str());
                if (acquireResult.mayHaveQueuedGpuWork)
                    MarkQueuedGpuWorkWithoutRetirementFence(
                        impl, "RHIFrame::AcquireSwapchainImage", acquireResult, queueTelemetry);
                else
                {
                    Detail::AsyncComputeSubmitPlan emptyPlan;
                    FinalizeThreadedRhiFrameContext(impl, frameContext, emptyPlan);
                }
                return nullptr;
            }
            frameContext.hasAcquiredSwapchainImage = acquiredImage.has_value();
            frameContext.swapchainImageIndex = acquiredImage.has_value() ? acquiredImage->imageIndex : 0u;
            frameContext.swapchainBackbufferView = std::move(backbufferView);
            if (frameContext.swapchainBackbufferView != nullptr)
            {
                SeedAcquiredSwapchainBackbufferState(frameContext);
                EnsureSwapchainDepthStencilAttachment(
                    impl,
                    frameContext,
                    renderScenePackage.renderWidth,
                    renderScenePackage.renderHeight);
            }
        }
        frameContext.explicitReadbackTexture =
            Render::FrameGraph::ResolveFrameReadbackTexture(&renderScenePackage, &frameContext);
        frameContext.explicitReadbackTextureGeneration =
            Render::FrameGraph::ResolveFrameReadbackTextureGeneration(&renderScenePackage, &frameContext);

        if (impl.renderDocCaptureController != nullptr)
        {
            impl.renderDocCaptureController->OnPreFrame(
                renderScenePackage.targetsSwapchain,
                Render::FrameGraph::HasExternalSceneOutput(renderScenePackage));
        }

        return &frameContext;
    }

    bool Detail::ResolveThreadedSwapchainAcquisition(
        const ThreadedRhiSubmissionExecutionPtr& execution)
    {
        if (execution == nullptr || execution->swapchainAcquisition == nullptr)
            return true;

        ThreadedSwapchainAcquisition& acquisition = *execution->swapchainAcquisition;
        if (!acquisition.enqueued || acquisition.resolved)
            return true;

        // Mark first: this helper must wait on the lane operation at most once,
        // even when the frame is aborted from a different path.
        acquisition.resolved = true;
        if (execution->frameContext == nullptr || execution->impl == nullptr)
            return true;

        DriverImpl& impl = *execution->impl;
        Render::RHI::RHIFrameContext& frameContext = *execution->frameContext;
        NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::ResolveSwapchainAcquisition");
        const auto laneResult = RhiCommandListExecutor::Get().WaitForSubmitLaneOperation(
            acquisition.laneOperation, "RHIFrame::AcquireSwapchainImage");

        // The frame was published against a swapchain that stopped matching the
        // surface - a synchronous window resize, or a rebuild that landed while this
        // acquisition was already queued. There is no backbuffer to render into, so
        // the frame is dropped instead of failed: no queue operation failed, no
        // rendering work was recorded, and the rebuild is requested so the next frame
        // publishes against a swapchain that matches the surface again.
        if (laneResult.RequiresSwapchainRecreation())
        {
            if (!impl.hasPendingSwapchainResize.load(std::memory_order_acquire))
            {
                impl.pendingSwapchainResizeKeepsCurrentSize.store(true, std::memory_order_release);
                impl.hasPendingSwapchainResize.store(true, std::memory_order_release);
            }
            execution->submissionFrame.droppedForSwapchainRecreation = true;
            if (execution->failureReason.empty())
            {
                execution->failureReason = laneResult.message.empty()
                    ? std::string { "swapchain image acquisition lost the swapchain to a resize" }
                    : laneResult.message;
            }
            if (execution->framePrepared)
            {
                Detail::AsyncComputeSubmitPlan emptyPlan;
                FinalizeThreadedRhiFrameContext(impl, frameContext, emptyPlan);
                execution->framePrepared = false;
            }
            NLS_LOG_INFO(
                "[Driver] Dropped threaded frame " +
                std::to_string(execution->renderScenePackage.frameId) +
                " because the swapchain no longer matches the surface; the next frame "
                "publishes against the rebuilt swapchain");
            return false;
        }

        if (!RecordQueueOperationResult(
                impl, laneResult, "RHIFrame::AcquireSwapchainImage", execution->queueTelemetry))
        {
            MarkCommandRecordingFailure(
                &execution->submissionFrame, laneResult.message.c_str());
            if (laneResult.mayHaveQueuedGpuWork)
            {
                MarkQueuedGpuWorkWithoutRetirementFence(
                    impl,
                    "RHIFrame::AcquireSwapchainImage",
                    laneResult,
                    execution->queueTelemetry);
            }
            else if (execution->framePrepared)
            {
                Detail::AsyncComputeSubmitPlan emptyPlan;
                FinalizeThreadedRhiFrameContext(impl, frameContext, emptyPlan);
                execution->framePrepared = false;
            }
            if (execution->failureReason.empty())
            {
                execution->failureReason = laneResult.message.empty()
                    ? std::string { "RHIFrame::AcquireSwapchainImage failed" }
                    : laneResult.message;
            }
            return false;
        }

        frameContext.hasAcquiredSwapchainImage = acquisition.hasImage;
        frameContext.swapchainImageIndex = acquisition.hasImage ? acquisition.imageIndex : 0u;
        frameContext.swapchainBackbufferView = acquisition.backbufferView;
        if (frameContext.swapchainBackbufferView != nullptr)
        {
            SeedAcquiredSwapchainBackbufferState(frameContext);
            EnsureSwapchainDepthStencilAttachment(
                impl,
                frameContext,
                execution->renderScenePackage.renderWidth,
                execution->renderScenePackage.renderHeight);
        }
        frameContext.explicitReadbackTexture = Render::FrameGraph::ResolveFrameReadbackTexture(
            &execution->renderScenePackage, &frameContext);
        frameContext.explicitReadbackTextureGeneration = Render::FrameGraph::ResolveFrameReadbackTextureGeneration(
            &execution->renderScenePackage, &frameContext);
        return true;
    }

    void Detail::ReleaseRetiredUiTextureViewsForCompletedFrame(
        DriverImpl& impl,
        const uint64_t completedFrameId)
    {
        if (completedFrameId == 0u)
            return;

        impl.uiOverlayRenderer.ReleaseRetiredResourcesUpTo(completedFrameId);
        impl.uiTextureRegistry.ReleaseRetiredTextureViewsUpTo(completedFrameId);
    }

    size_t Detail::RecordPendingUiRgba8TextureUploads(
        DriverImpl& impl,
        Render::RHI::RHIFrameContext& frameContext,
        Render::RHI::RHICommandBuffer& commandBuffer)
    {
        auto publishResult = [&impl](
            uint64_t requestId,
            size_t byteCount,
            const std::string& debugName,
            DriverImpl::CompletedUiRgba8TextureUpload result)
        {
            ScopedArtifactTelemetry publishTelemetry {
                ::NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTextureUploadPublish,
                std::chrono::steady_clock::now(),
                byteCount,
                debugName
            };

            std::lock_guard lock(impl.pendingUiRgba8TextureUploadMutex);
            if (impl.canceledUiRgba8TextureUploadRequestIds.erase(requestId) != 0u)
                return;
            impl.completedUiRgba8TextureUploads[requestId] = std::move(result);
        };

        struct ReadyUiRgba8TextureUpload
        {
            uint64_t requestId = 0u;
            size_t byteCount = 0u;
            std::string debugName;
            DriverImpl::CompletedUiRgba8TextureUpload result;
        };

        std::vector<DriverImpl::RecordedUiRgba8TextureUpload> recordedUploads;
        {
            std::lock_guard lock(impl.pendingUiRgba8TextureUploadMutex);
            recordedUploads.swap(impl.recordedUiRgba8TextureUploads);
        }

        std::vector<ReadyUiRgba8TextureUpload> readyUploads;
        std::vector<DriverImpl::RecordedUiRgba8TextureUpload> stillPendingUploads;
        for (auto& recorded : recordedUploads)
        {
            bool canceled = false;
            {
                std::lock_guard lock(impl.pendingUiRgba8TextureUploadMutex);
                canceled = impl.canceledUiRgba8TextureUploadRequestIds.find(recorded.requestId) !=
                    impl.canceledUiRgba8TextureUploadRequestIds.end();
            }

            const auto status = recorded.completion != nullptr
                ? recorded.completion->Poll()
                : Render::RHI::RHICompletionStatus{ Render::RHI::RHICompletionStatusCode::Success, {} };
            if (status.code == Render::RHI::RHICompletionStatusCode::Pending)
            {
                stillPendingUploads.push_back(std::move(recorded));
                continue;
            }
            if (canceled)
            {
                std::lock_guard lock(impl.pendingUiRgba8TextureUploadMutex);
                impl.canceledUiRgba8TextureUploadRequestIds.erase(recorded.requestId);
                continue;
            }

            DriverImpl::CompletedUiRgba8TextureUpload result;
            result.success = status.Succeeded();
            result.texture = status.Succeeded() ? std::move(recorded.texture) : nullptr;
            result.textureView = status.Succeeded() ? std::move(recorded.textureView) : nullptr;
            result.width = recorded.width;
            result.height = recorded.height;
            result.diagnostic = status.Succeeded()
                ? std::string {}
                : (status.message.empty()
                    ? "UI RGBA8 texture upload did not complete successfully"
                    : status.message);
            readyUploads.push_back({
                recorded.requestId,
                recorded.byteCount,
                std::move(recorded.debugName),
                std::move(result)
            });
        }
        if (!stillPendingUploads.empty())
        {
            std::lock_guard lock(impl.pendingUiRgba8TextureUploadMutex);
            for (auto& recorded : stillPendingUploads)
            {
                impl.recordedUiRgba8TextureUploads.push_back(std::move(recorded));
            }
        }
        for (auto& ready : readyUploads)
        {
            publishResult(
                ready.requestId,
                ready.byteCount,
                ready.debugName,
                std::move(ready.result));
        }

        if (impl.explicitDevice == nullptr ||
            frameContext.uploadContext == nullptr ||
            frameContext.frameFence == nullptr)
        {
            return 0u;
        }

        std::vector<DriverImpl::PendingUiRgba8TextureUpload> pendingUploads;
        {
            std::lock_guard lock(impl.pendingUiRgba8TextureUploadMutex);
            pendingUploads.swap(impl.pendingUiRgba8TextureUploads);
        }
        if (pendingUploads.empty())
            return 0u;

        size_t recordedCount = 0u;
        for (auto& request : pendingUploads)
        {
            const auto* formatDescriptor = Render::RHI::GetTextureFormatDescriptor(request.format);
            const uint32_t formatRowPitch = Render::RHI::CalculateTextureRowPitch(
                request.format,
                request.width);
            const uint32_t formatSlicePitch = Render::RHI::CalculateTextureSlicePitch(
                request.format,
                request.width,
                request.height,
                1u);
            const uint32_t sourceRowPitch = request.rowPitch != 0u
                ? request.rowPitch
                : formatRowPitch;
            const uint32_t sourceSlicePitch = request.slicePitch != 0u
                ? request.slicePitch
                : formatSlicePitch;
            const uint32_t sourceRowCount = formatDescriptor != nullptr &&
                formatDescriptor->blockHeight != 0u
                ? (request.height + formatDescriptor->blockHeight - 1u) /
                    formatDescriptor->blockHeight
                : request.height;
            const size_t expectedBytes = static_cast<size_t>(sourceSlicePitch);
            const std::string debugName = request.debugName.empty()
                ? "UiRgba8TextureUpload"
                : request.debugName;
            const bool atlasUpload = !request.atlasPageKey.empty();
            const uint32_t textureWidth = atlasUpload ? request.atlasPageSize : request.width;
            const uint32_t textureHeight = atlasUpload ? request.atlasPageSize : request.height;
            if (formatDescriptor == nullptr ||
                formatRowPitch == 0u ||
                formatSlicePitch == 0u ||
                sourceRowPitch < formatRowPitch ||
                sourceSlicePitch < sourceRowPitch * sourceRowCount ||
                request.rgbaPixels.size() < sourceSlicePitch ||
                expectedBytes == 0u)
            {
                publishResult(request.requestId, expectedBytes, debugName, {
                    false,
                    nullptr,
                    nullptr,
                    request.width,
                    request.height,
                    "UI RGBA8 texture upload request has invalid pixel data"
                });
                continue;
            }
            if (atlasUpload &&
                (request.format != Render::RHI::TextureFormat::RGBA8 ||
                 request.rowPitch != 0u && request.rowPitch < request.width * 4u ||
                 request.slicePitch != 0u && request.slicePitch < request.width * request.height * 4u ||
                 request.atlasPageSize != 2048u ||
                 request.atlasX > request.atlasPageSize ||
                 request.atlasY > request.atlasPageSize ||
                 request.width > request.atlasPageSize - request.atlasX ||
                 request.height > request.atlasPageSize - request.atlasY))
            {
                publishResult(request.requestId, expectedBytes, debugName, {
                    false,
                    nullptr,
                    nullptr,
                    request.width,
                    request.height,
                    "UI RGBA8 atlas upload region is outside the page"
                });
                continue;
            }

            ScopedArtifactTelemetry uploadTelemetry {
                ::NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTextureUpload,
                std::chrono::steady_clock::now(),
                expectedBytes,
                debugName
            };

            std::shared_ptr<Render::RHI::RHITexture> texture;
            std::shared_ptr<Render::RHI::RHITextureView> textureView;
            if (atlasUpload)
            {
                const uint64_t deviceIdentity = impl.explicitDevice->GetCacheIdentity();
                if (const auto stalePage = impl.uiRgba8TextureAtlasPages.find(request.atlasPageKey);
                    stalePage != impl.uiRgba8TextureAtlasPages.end() &&
                    stalePage->second.deviceIdentity != deviceIdentity)
                {
                    impl.uiRgba8TextureAtlasPages.erase(stalePage);
                }
                if (const auto found = impl.uiRgba8TextureAtlasPages.find(request.atlasPageKey);
                    found != impl.uiRgba8TextureAtlasPages.end())
                {
                    if (found->second.pageSize != request.atlasPageSize ||
                        found->second.colorSpace != request.colorSpace)
                    {
                        publishResult(request.requestId, expectedBytes, debugName, {
                            false,
                            nullptr,
                            nullptr,
                            request.width,
                            request.height,
                            "UI RGBA8 atlas page layout changed"
                        });
                        continue;
                    }
                    texture = found->second.texture;
                    textureView = found->second.textureView;
                }
                else
                {
                    Render::RHI::RHITextureDesc textureDesc{};
                    textureDesc.extent.width = textureWidth;
                    textureDesc.extent.height = textureHeight;
                    textureDesc.extent.depth = 1u;
                    textureDesc.dimension = Render::RHI::TextureDimension::Texture2D;
                    textureDesc.format = request.format;
                    textureDesc.colorSpace = request.colorSpace;
                    textureDesc.usage =
                        Render::RHI::TextureUsageFlags::Sampled |
                        Render::RHI::TextureUsageFlags::CopyDst;
                    textureDesc.memoryUsage = Render::RHI::MemoryUsage::GPUOnly;
                    textureDesc.debugName = request.atlasPageKey;
                    texture = impl.explicitDevice->CreateTexture(textureDesc);
                    if (texture != nullptr)
                    {
                        Render::RHI::RHITextureViewDesc viewDesc;
                        viewDesc.viewType = Render::RHI::TextureViewType::Texture2D;
                        viewDesc.format = request.format;
                        viewDesc.colorSpace = request.colorSpace;
                        viewDesc.subresourceRange = { 0u, 1u, 0u, 1u };
                        viewDesc.debugName = request.atlasPageKey + "View";
                        textureView = impl.explicitDevice->CreateTextureView(texture, viewDesc);
                    }
                    if (texture != nullptr && textureView != nullptr)
                    {
                        impl.uiRgba8TextureAtlasPages.emplace(
                            request.atlasPageKey,
                            DriverImpl::UiRgba8TextureAtlasPage {
                                texture,
                                textureView,
                                request.atlasPageSize,
                                deviceIdentity,
                                request.colorSpace
                            });
                    }
                }
            }
            else
            if (texture == nullptr)
            {
                ScopedArtifactTelemetry createTelemetry {
                    ::NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTextureUploadCreate,
                    std::chrono::steady_clock::now(),
                    expectedBytes,
                    debugName
                };

                Render::RHI::RHITextureDesc textureDesc{};
                textureDesc.extent.width = textureWidth;
                textureDesc.extent.height = textureHeight;
                textureDesc.extent.depth = 1u;
                textureDesc.dimension = Render::RHI::TextureDimension::Texture2D;
                textureDesc.format = request.format;
                textureDesc.colorSpace = request.colorSpace;
                textureDesc.usage =
                    Render::RHI::TextureUsageFlags::Sampled |
                    Render::RHI::TextureUsageFlags::CopyDst;
                textureDesc.memoryUsage = Render::RHI::MemoryUsage::GPUOnly;
                textureDesc.debugName = debugName;
                texture = impl.explicitDevice->CreateTexture(textureDesc);
            }
            if (texture == nullptr)
            {
                publishResult(request.requestId, expectedBytes, debugName, {
                    false,
                    nullptr,
                    nullptr,
                    request.width,
                    request.height,
                    "failed to create UI RGBA8 texture"
                });
                continue;
            }
            uint32_t uploadRowPitch = 0u;
            size_t uploadBytes = 0u;
            std::vector<uint8_t> uploadPixels;
            {
                ScopedArtifactTelemetry preparePixelsTelemetry {
                    ::NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTextureUploadPreparePixels,
                    std::chrono::steady_clock::now(),
                    expectedBytes,
                    debugName
                };

                uploadRowPitch = AlignTextureUploadPitch(sourceRowPitch);
                uploadBytes = static_cast<size_t>(uploadRowPitch) * sourceRowCount;
                uploadPixels.resize(uploadBytes, 0u);
                for (uint32_t row = 0u; row < sourceRowCount; ++row)
                {
                    std::memcpy(
                        uploadPixels.data() + static_cast<size_t>(row) * uploadRowPitch,
                        request.rgbaPixels.data() + static_cast<size_t>(row) * sourceRowPitch,
                        sourceRowPitch);
                }
            }

            if (!atlasUpload)
            {
                ScopedArtifactTelemetry viewTelemetry {
                    ::NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTextureUploadCreateView,
                    std::chrono::steady_clock::now(),
                    expectedBytes,
                    debugName
                };

                Render::RHI::RHITextureViewDesc viewDesc;
                viewDesc.viewType = Render::RHI::TextureViewType::Texture2D;
                viewDesc.format = request.format;
                viewDesc.colorSpace = request.colorSpace;
                viewDesc.subresourceRange = { 0u, 1u, 0u, 1u };
                viewDesc.debugName = debugName + "View";
                textureView = impl.explicitDevice->CreateTextureView(texture, viewDesc);
            }
            if (textureView == nullptr)
            {
                publishResult(request.requestId, expectedBytes, debugName, {
                    false,
                    nullptr,
                    nullptr,
                    request.width,
                    request.height,
                    "failed to create UI RGBA8 texture view"
                });
                continue;
            }

            Render::RHI::UploadTextureRequest uploadRequest;
            uploadRequest.destination = texture;
            uploadRequest.data = uploadPixels.data();
            uploadRequest.dataSize = uploadPixels.size();
            uploadRequest.mipLevel = 0u;
            uploadRequest.arrayLayer = 0u;
            uploadRequest.rowPitch = uploadRowPitch;
            uploadRequest.slicePitch = uploadRowPitch * sourceRowCount;
            uploadRequest.textureOffset = atlasUpload
                ? Render::RHI::RHIOffset3D {
                    static_cast<int32_t>(request.atlasX),
                    static_cast<int32_t>(request.atlasY),
                    0 }
                : Render::RHI::RHIOffset3D {};
            uploadRequest.extent = { request.width, request.height, 1u };
            uploadRequest.debugName = debugName;
            Render::RHI::UploadBatchRequest uploadBatchRequest;
            uploadBatchRequest.textureUploads.push_back(uploadRequest);
            uploadBatchRequest.completionFence = frameContext.frameFence;
            uploadBatchRequest.requireRecordedBackendWork = true;
            uploadBatchRequest.debugName = debugName;
            Render::RHI::UploadBatchSubmission uploadResult;
            {
                ScopedArtifactTelemetry submitTelemetry {
                    ::NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTextureUploadSubmit,
                    std::chrono::steady_clock::now(),
                    uploadBytes,
                    debugName
                };
                uploadResult = frameContext.uploadContext->SubmitUploadBatch(
                    commandBuffer,
                    uploadBatchRequest);
            }
            if (!uploadResult.accepted)
            {
                publishResult(request.requestId, expectedBytes, debugName, {
                    false,
                    nullptr,
                    nullptr,
                    textureWidth,
                    textureHeight,
                    uploadResult.diagnostic.empty()
                        ? "failed to record UI RGBA8 texture upload"
                        : uploadResult.diagnostic
                });
                continue;
            }

            {
                std::lock_guard lock(impl.pendingUiRgba8TextureUploadMutex);
                impl.recordedUiRgba8TextureUploads.push_back({
                    request.requestId,
                    std::move(texture),
                    std::move(textureView),
                    std::move(uploadResult.completion),
                    request.width,
                    request.height,
                    expectedBytes,
                    debugName
                });
            }
            ++recordedCount;
        }

        return recordedCount;
    }

    size_t Detail::RecordPendingTextureRuntimeUploads(
        DriverImpl& impl,
        Render::RHI::RHIFrameContext& frameContext,
        Render::RHI::RHICommandBuffer& commandBuffer)
    {
        constexpr size_t kMaxTextureUploadsPerBatch = 8u;
        constexpr size_t kMaxTextureUploadBytesPerBatch = 32u * 1024u * 1024u;

        auto publishResult = [&impl](
            const uint64_t requestId,
            DriverImpl::CompletedTextureRuntimeUpload result)
        {
            std::lock_guard lock(impl.pendingTextureRuntimeUploadMutex);
            if (impl.canceledTextureRuntimeUploadRequestIds.erase(requestId) != 0u)
                return;
            impl.completedTextureRuntimeUploads[requestId] = std::move(result);
        };

        std::vector<DriverImpl::RecordedTextureRuntimeUpload> recordedUploads;
        {
            std::lock_guard lock(impl.pendingTextureRuntimeUploadMutex);
            recordedUploads.swap(impl.recordedTextureRuntimeUploads);
        }

        std::vector<DriverImpl::RecordedTextureRuntimeUpload> stillPending;
        for (auto& recorded : recordedUploads)
        {
            bool canceled = false;
            {
                std::lock_guard lock(impl.pendingTextureRuntimeUploadMutex);
                canceled = impl.canceledTextureRuntimeUploadRequestIds.find(recorded.requestId) !=
                    impl.canceledTextureRuntimeUploadRequestIds.end();
            }

            const auto status = recorded.completion != nullptr
                ? recorded.completion->Poll()
                : Render::RHI::RHICompletionStatus { Render::RHI::RHICompletionStatusCode::Success, {} };
            if (status.code == Render::RHI::RHICompletionStatusCode::Pending)
            {
                stillPending.push_back(std::move(recorded));
                continue;
            }
            if (canceled)
            {
                std::lock_guard lock(impl.pendingTextureRuntimeUploadMutex);
                impl.canceledTextureRuntimeUploadRequestIds.erase(recorded.requestId);
                continue;
            }

            DriverImpl::CompletedTextureRuntimeUpload completed;
            completed.success = status.Succeeded();
            completed.texture = status.Succeeded() ? std::move(recorded.texture) : nullptr;
            completed.diagnostic = status.Succeeded()
                ? std::string {}
                : (status.message.empty()
                    ? "Texture runtime upload did not complete successfully"
                    : status.message);
            publishResult(recorded.requestId, std::move(completed));
        }

        if (impl.explicitDevice == nullptr ||
            frameContext.uploadContext == nullptr ||
            frameContext.frameFence == nullptr)
        {
            std::lock_guard lock(impl.pendingTextureRuntimeUploadMutex);
            for (auto& recorded : stillPending)
                impl.recordedTextureRuntimeUploads.push_back(std::move(recorded));
            return 0u;
        }

        std::vector<DriverImpl::PendingTextureRuntimeUpload> pendingUploads;
        {
            std::lock_guard lock(impl.pendingTextureRuntimeUploadMutex);
            pendingUploads.swap(impl.pendingTextureRuntimeUploads);
            for (auto& recorded : stillPending)
                impl.recordedTextureRuntimeUploads.push_back(std::move(recorded));
        }
        if (pendingUploads.empty())
            return 0u;

        std::vector<DriverImpl::PendingTextureRuntimeUpload> deferredUploads;
        Render::RHI::UploadBatchRequest batchRequest;
        batchRequest.completionFence = frameContext.frameFence;
        batchRequest.requireRecordedBackendWork = true;
        batchRequest.debugName = "TextureRuntimeUploadBatch";
        std::vector<std::shared_ptr<Render::RHI::RHITexture>> batchTextures;
        std::vector<size_t> batchRequestIds;
        std::vector<size_t> batchByteCounts;
        size_t batchBytes = 0u;

        for (auto& pending : pendingUploads)
        {
            const size_t byteCount = pending.request.ByteSize();
            if (batchRequestIds.size() >= kMaxTextureUploadsPerBatch ||
                (!batchRequestIds.empty() &&
                    batchBytes + byteCount > kMaxTextureUploadBytesPerBatch))
            {
                deferredUploads.push_back(std::move(pending));
                continue;
            }

            auto texture = impl.explicitDevice->CreateTexture(pending.request.desc);
            if (texture == nullptr)
            {
                DriverImpl::CompletedTextureRuntimeUpload failed;
                failed.success = false;
                failed.diagnostic = "failed to create runtime texture " + pending.request.debugName;
                publishResult(pending.requestId, std::move(failed));
                continue;
            }

            for (const auto& mip : pending.request.mips)
            {
                Render::RHI::UploadTextureRequest upload;
                upload.destination = texture;
                upload.data = mip.pixels.data();
                upload.dataSize = mip.pixels.size();
                upload.mipLevel = mip.level;
                upload.rowPitch = mip.rowPitch;
                upload.slicePitch = mip.slicePitch;
                upload.extent = { mip.width, mip.height, 1u };
                upload.debugName = pending.request.debugName;
                batchRequest.textureUploads.push_back(std::move(upload));
            }
            batchBytes += byteCount;
            batchRequestIds.push_back(pending.requestId);
            batchByteCounts.push_back(byteCount);
            batchTextures.push_back(std::move(texture));
        }

        if (!batchRequestIds.empty())
        {
            const auto submission = frameContext.uploadContext->SubmitUploadBatch(
                commandBuffer,
                batchRequest);
            if (!submission.accepted)
            {
                const std::string diagnostic = submission.diagnostic.empty()
                    ? "failed to record texture runtime upload batch"
                    : submission.diagnostic;
                for (const size_t requestId : batchRequestIds)
                {
                    DriverImpl::CompletedTextureRuntimeUpload failed;
                    failed.success = false;
                    failed.diagnostic = diagnostic;
                    publishResult(requestId, std::move(failed));
                }
            }
            else
            {
                std::lock_guard lock(impl.pendingTextureRuntimeUploadMutex);
                for (size_t index = 0u; index < batchRequestIds.size(); ++index)
                {
                    impl.recordedTextureRuntimeUploads.push_back({
                        batchRequestIds[index],
                        std::move(batchTextures[index]),
                        submission.completion,
                        batchByteCounts[index],
                        batchRequest.debugName
                    });
                }
                impl.textureRuntimeUploadRecordedCount.fetch_add(
                    batchRequestIds.size(),
                    std::memory_order_relaxed);
            }
        }

        if (!deferredUploads.empty())
        {
            std::lock_guard lock(impl.pendingTextureRuntimeUploadMutex);
            for (auto& deferred : deferredUploads)
                impl.pendingTextureRuntimeUploads.push_back(std::move(deferred));
        }
        return batchRequestIds.size();
    }

    size_t Detail::RecordPendingMeshRuntimeUploads(
        DriverImpl& impl,
        const bool createGpuResources)
    {
        impl.meshRuntimeUploadRecordTickCount.fetch_add(1u, std::memory_order_relaxed);
        constexpr size_t kMaxUploadsPerFrame = 8u;
        constexpr size_t kMaxUploadBytesPerFrame = 16u * 1024u * 1024u;

        std::vector<DriverImpl::PendingMeshRuntimeUpload> pendingUploads;
        {
            std::lock_guard lock(impl.pendingMeshRuntimeUploadMutex);
            pendingUploads.swap(impl.pendingMeshRuntimeUploads);
            impl.meshRuntimeUploadLastSwappedBatchCount = pendingUploads.size();
            if (pendingUploads.empty())
                ++impl.meshRuntimeUploadEmptyRecordTickCount;
        }
        if (pendingUploads.empty())
            return 0u;

        size_t completedCount = 0u;
        size_t consumedBytes = 0u;
        std::vector<DriverImpl::PendingMeshRuntimeUpload> deferredUploads;
        for (auto& pending : pendingUploads)
        {
            bool canceled = false;
            {
                std::lock_guard lock(impl.pendingMeshRuntimeUploadMutex);
                canceled = impl.canceledMeshRuntimeUploadRequestIds.erase(pending.requestId) != 0u;
            }
            if (canceled)
            {
                impl.meshRuntimeUploadLastCanceledId = pending.requestId;
                impl.meshRuntimeUploadCanceledCount.fetch_add(1u, std::memory_order_relaxed);
                continue;
            }

            const size_t requestBytes = pending.request.ByteSize();
            if (completedCount >= kMaxUploadsPerFrame ||
                (completedCount != 0u && consumedBytes + requestBytes > kMaxUploadBytesPerFrame))
            {
                deferredUploads.push_back(std::move(pending));
                continue;
            }

            DriverImpl::CompletedMeshRuntimeUpload completed;
            if (!createGpuResources)
            {
                // Mesh construction calls the backend's synchronous initial
                // buffer upload. Never do that on the RHI worker: it can wait
                // on the same graphics queue that the worker is responsible
                // for advancing. Transfer the already parsed payload and let
                // the owning resource manager materialize it after consume.
                completed.success = true;
                completed.uploadRequest = std::move(pending.request);
            }
            else
            {
                try
                {
                    completed.mesh = std::make_unique<Render::Resources::Mesh>(
                        pending.request.vertices,
                        pending.request.indices,
                        pending.request.materialIndex,
                        Render::Resources::MeshBufferUploadMode::GpuOnly,
                        pending.request.boundingSphere);
                    if (completed.mesh != nullptr && !pending.request.lodResources.empty())
                    {
                        std::vector<std::unique_ptr<Render::Resources::Mesh>> lodMeshes;
                        std::vector<float> screenSizes {1.0f};
                        lodMeshes.reserve(pending.request.lodResources.size());
                        for (const auto& lod : pending.request.lodResources)
                        {
                            lodMeshes.push_back(std::make_unique<Render::Resources::Mesh>(
                                lod.vertices, lod.indices, lod.materialIndex,
                                Render::Resources::MeshBufferUploadMode::GpuOnly, lod.boundingSphere));
                            screenSizes.push_back(lod.screenSize);
                        }
                        completed.mesh->SetLODResources(
                            std::move(lodMeshes),
                            std::move(screenSizes),
                            pending.request.minLOD);
                    }
                    completed.success = completed.mesh != nullptr;
                    if (!completed.success)
                        completed.diagnostic = "mesh runtime upload returned no resource";
                }
                catch (const std::exception& exception)
                {
                    completed.diagnostic = exception.what();
                }
                catch (...)
                {
                    completed.diagnostic = "mesh runtime upload failed with an unknown exception";
                }
            }

            {
                std::lock_guard lock(impl.pendingMeshRuntimeUploadMutex);
                if (impl.canceledMeshRuntimeUploadRequestIds.erase(pending.requestId) == 0u)
                {
                    impl.completedMeshRuntimeUploads[pending.requestId] = std::move(completed);
                    impl.meshRuntimeUploadLastRecordedId = pending.requestId;
                }
            }
            consumedBytes += requestBytes;
            impl.meshRuntimeUploadRecordedCount.fetch_add(1u, std::memory_order_relaxed);
            ++completedCount;
        }

        if (!deferredUploads.empty())
        {
            std::lock_guard lock(impl.pendingMeshRuntimeUploadMutex);
            for (auto& deferred : deferredUploads)
                impl.pendingMeshRuntimeUploads.push_back(std::move(deferred));
        }
        return completedCount;
    }

    void RecordThreadedRhiSubmissionWork(const ThreadedRhiSubmissionExecutionPtr& execution);
    void PrepareThreadedRhiSubmitPlan(const ThreadedRhiSubmissionExecutionPtr& execution);
    void QueueSubmitThreadedRhiSubmission(const ThreadedRhiSubmissionExecutionPtr& execution);
    void PresentThreadedRhiSubmission(const ThreadedRhiSubmissionExecutionPtr& execution);
    void ReadbackThreadedRhiSubmission(const ThreadedRhiSubmissionExecutionPtr& execution);
    void RetireThreadedRhiSubmission(const ThreadedRhiSubmissionExecutionPtr& execution);
    void AbortThreadedRhiSubmission(const ThreadedRhiSubmissionExecutionPtr& execution);
    void FailThreadedRhiSubmissionStage(
        const ThreadedRhiSubmissionExecutionPtr& execution,
        std::string reason);
    void RecordThreadedRhiSubmissionWork(const ThreadedRhiSubmissionExecutionPtr& execution)
    {
        NLS_PROFILE_SCOPE();
        if (execution == nullptr || execution->frameContext == nullptr)
            return;

        DriverImpl& impl = *execution->impl;
        Render::RHI::RHIFrameContext& frameContext = *execution->frameContext;
        const RenderScenePackage& renderScenePackage = execution->renderScenePackage;
        const size_t frameContextIndex = execution->frameContextIndex;
        Detail::AsyncComputeSubmitPlan* submitPlan = &execution->submitPlan;
        RhiSubmissionFrame* submissionFrame = &execution->submissionFrame;

        (void)Detail::RecordPendingMeshRuntimeUploads(impl, false);

        if (frameContext.commandBuffer != nullptr)
        {
            const bool hasExternalSceneOutput =
                Render::FrameGraph::HasExternalSceneOutput(renderScenePackage);
            const bool supportsOrderedWorkUnitSubmission =
                execution->supportsOrderedWorkUnitSubmission;
            const auto noteParallelFallback = [submissionFrame](const char* reason)
            {
                if (submissionFrame->parallelFallbackReason.empty())
                    submissionFrame->parallelFallbackReason = reason;
            };
            const bool parallelRecordingReady = execution->parallelRecordingReady;
            const bool parallelTranslationReady = execution->parallelTranslationReady;
            const bool inRenderPassChildCommandRecordingReady =
                supportsOrderedWorkUnitSubmission &&
                impl.explicitDevice != nullptr &&
                impl.explicitDevice->GetCapabilities()
                    .GetFeature(Render::RHI::RHIDeviceFeature::ParallelCommandRecording)
                    .supported &&
                impl.explicitDevice->GetCapabilities()
                    .GetFeature(Render::RHI::RHIDeviceFeature::InRenderPassChildCommandBuffers)
                    .supported &&
                frameContext.commandBuffer->CanExecuteChildCommandBuffers();
            const bool packageContainsOrderedSlicedWorkUnits = std::any_of(
                renderScenePackage.parallelCommandWorkUnits.begin(),
                renderScenePackage.parallelCommandWorkUnits.end(),
                [](const ParallelCommandWorkUnit& workUnit)
                {
                    return workUnit.requiresOrderedSlicedSubmission;
                });
            const bool packageContainsInRenderPassChildWorkUnits = std::any_of(
                renderScenePackage.parallelCommandWorkUnits.begin(),
                renderScenePackage.parallelCommandWorkUnits.end(),
                [](const ParallelCommandWorkUnit& workUnit)
                {
                    return workUnit.usesInRenderPassChildCommandRecording;
                });
            const bool packageContainsNonChildOrderedSlicedWorkUnits = std::any_of(
                renderScenePackage.parallelCommandWorkUnits.begin(),
                renderScenePackage.parallelCommandWorkUnits.end(),
                [](const ParallelCommandWorkUnit& workUnit)
                {
                    return workUnit.requiresOrderedSlicedSubmission &&
                        !workUnit.usesInRenderPassChildCommandRecording;
                });
            const bool canPreserveOrderedPathWithChildFallback =
                !packageContainsInRenderPassChildWorkUnits ||
                inRenderPassChildCommandRecordingReady ||
                packageContainsNonChildOrderedSlicedWorkUnits;
            const bool allowOrderedSlicedSubmission =
                supportsOrderedWorkUnitSubmission &&
                packageContainsOrderedSlicedWorkUnits &&
                canPreserveOrderedPathWithChildFallback;
            const bool useOrderedWorkUnitPath =
                supportsOrderedWorkUnitSubmission &&
                canPreserveOrderedPathWithChildFallback;
            if (!supportsOrderedWorkUnitSubmission)
            {
                noteParallelFallback(
                    hasExternalSceneOutput
                        ? "external scene output disables parallel command recording"
                        : "backend does not support ordered parallel command submission");
            }
            else if (
                packageContainsOrderedSlicedWorkUnits &&
                (!allowOrderedSlicedSubmission ||
                    (packageContainsInRenderPassChildWorkUnits && !inRenderPassChildCommandRecordingReady)))
            {
                noteParallelFallback(
                    packageContainsInRenderPassChildWorkUnits
                        ? "in-render-pass child command recording unavailable; using unsliced passCommandInputs"
                        : "ordered sliced submission unavailable; using unsliced passCommandInputs");
            }
            auto workUnits = Detail::BuildParallelCommandWorkUnits(
                renderScenePackage,
                useOrderedWorkUnitPath ? parallelRecordingReady : false,
                useOrderedWorkUnitPath ? parallelTranslationReady : false,
                allowOrderedSlicedSubmission,
                inRenderPassChildCommandRecordingReady);
            // Native list types are fixed when pools are created. Resolve the
            // queue once before recording, including when async compute is gated.
            auto graphicsQueue = impl.explicitDevice->GetQueue(RHI::QueueType::Graphics);
            auto computeQueue = impl.explicitDevice->GetQueue(RHI::QueueType::Compute);
            const bool useComputeQueue = Render::Settings::SupportsAsyncComputeFoundation(
                impl.explicitDevice->GetCapabilities()) &&
                graphicsQueue != nullptr && computeQueue != nullptr && graphicsQueue != computeQueue;
            if (!useComputeQueue)
                for (auto& unit : workUnits)
                    if (unit.commandInput.queueType == RHI::QueueType::Compute)
                        unit.commandInput.queueType = RHI::QueueType::Graphics;
            submissionFrame->parallelDrawCommandBatches = BuildParallelDrawCommandBatchMetadata(workUnits);
            const bool workUnitsContainInRenderPassChildRecording =
                std::any_of(
                    workUnits.begin(),
                    workUnits.end(),
                    [](const ParallelCommandWorkUnit& workUnit)
                    {
                        return workUnit.usesInRenderPassChildCommandRecording;
                    });
            if (useOrderedWorkUnitPath &&
                workUnitsContainInRenderPassChildRecording &&
                inRenderPassChildCommandRecordingReady &&
                !workUnits.empty())
            {
                const bool recordedOrderedChildStream = RecordOrderedInRenderPassChildAndSerialWorkUnits(
                    impl,
                    frameContext,
                    renderScenePackage,
                    workUnits,
                    submitPlan,
                    submissionFrame,
                    frameContextIndex,
                    execution);
                if (!recordedOrderedChildStream)
                {
                    if (!HasCommandRecordingFailure(submissionFrame))
                    {
                        MarkCommandRecordingFailure(
                            submissionFrame,
                            "Ordered in-render-pass child command recording failed");
                    }
                }
            }
            else if (useOrderedWorkUnitPath && !workUnits.empty())
            {
                auto recordedBatch = RecordParallelCommandWorkUnits(
                    impl,
                    frameContext,
                    renderScenePackage,
                    workUnits,
                    frameContextIndex,
                    execution);
                // Workers replay the closed lists after ordered state preparation.
                execution->recordedParallelBatch = std::move(recordedBatch);
                execution->usesRecordedParallelBatch = true;
            }
            else
            {
                bool recordedAnyCommand = false;
                // The acquisition stays unresolved until a pass actually binds the
                // backbuffer, so view-independent work units record while the
                // ordered lane is still draining the previous frame. Presentation
                // still needs the image, so a swapchain frame resolves it before
                // the submit plan closes.
                bool swapchainAcquisitionResolved = false;
                const auto ensureSwapchainAcquisition =
                    [&execution, &swapchainAcquisitionResolved]()
                    {
                        if (swapchainAcquisitionResolved)
                            return true;
                        swapchainAcquisitionResolved = true;
                        return Detail::ResolveThreadedSwapchainAcquisition(execution);
                    };
                const auto beginMainCommandBufferIfNeeded = [&frameContext, &recordedAnyCommand]()
                {
                    if (frameContext.commandBuffer == nullptr)
                        return false;

                    if (!frameContext.commandBuffer->IsRecording())
                        frameContext.commandBuffer->Begin();
                    return true;
                };

                for (const auto& workUnit : workUnits)
                {
                    const auto& passInput = workUnit.commandInput;
                    const bool isUiOverlayPass = passInput.kind == RenderPassCommandKind::UIOverlay;
                    if (!isUiOverlayPass &&
                        !Detail::IsPassRecordable(renderScenePackage, passInput))
                    {
                        Detail::LogSkippedPass(
                            renderScenePackage,
                            passInput,
                            "IsPassRecordable returned false");
                        continue;
                    }

                    if (passInput.kind == RenderPassCommandKind::Compute)
                    {
                        if (!beginMainCommandBufferIfNeeded())
                            continue;

                        const auto* passProfileScopeName = Detail::ResolvePassProfileScopeName(passInput);
                        NLS_PROFILE_NAMED_SCOPE(passProfileScopeName);
                        frameContext.commandBuffer->BeginGpuProfileScope(passProfileScopeName, __FUNCTION__);
                        const auto recordedDispatches = Detail::RecordComputeDispatches(
                            *frameContext.commandBuffer,
                            passInput.computeDispatchInputs,
                            false);
                        frameContext.commandBuffer->EndGpuProfileScope();
                        if (recordedDispatches == 0u)
                        {
                            if (!passInput.computeDispatchInputs.empty())
                            {
                                Detail::LogSkippedPass(
                                    renderScenePackage,
                                    passInput,
                                    "No dispatches recorded but commands were expected");
                                MarkCommandRecordingFailure(
                                    submissionFrame,
                                    "No dispatches recorded but commands were expected");
                            }
                            continue;
                        }

                        recordedAnyCommand = true;
                        submissionFrame->recordedPassCount += 1u;
                        submissionFrame->recordedWorkUnitCount += 1u;
                        continue;
                    }

                    if (!isUiOverlayPass)
                    {
                        const auto visibilityInput = Detail::BuildDependencyVisibilityPassInput(
                            workUnit,
                            workUnit.incomingDependencyEdges);
                        if (Detail::HasResourceVisibilityTransitions(visibilityInput))
                        {
                            if (!beginMainCommandBufferIfNeeded())
                                continue;

                            const bool recordedVisibilityTransitions = Detail::RecordResourceVisibilityTransitions(
                                *frameContext.commandBuffer,
                                visibilityInput,
                                &frameContext);
                            recordedAnyCommand |= recordedVisibilityTransitions;
                            if (!recordedVisibilityTransitions)
                            {
                                MarkCommandRecordingFailure(
                                    submissionFrame,
                                    "Dependency visibility transition recording failed");
                                continue;
                            }
                        }
                    }

                    if (passInput.targetsSwapchain && !ensureSwapchainAcquisition())
                        return;

                    if (passInput.targetsSwapchain &&
                        frameContext.swapchainBackbufferView == nullptr)
                    {
                        Detail::LogSkippedPass(
                            renderScenePackage,
                            passInput,
                            "swapchainBackbufferView is null");
                        submissionFrame->recordedPassCount += 1u;
                        submissionFrame->recordedWorkUnitCount += 1u;
                        continue;
                    }

                    if (!beginMainCommandBufferIfNeeded())
                        continue;

                    const auto effectivePassInput = ResolveSwapchainDepthPassInput(passInput, frameContext);
                    std::optional<RenderPassCommandInput> preparedUiOverlayPassInput;
                    if (isUiOverlayPass)
                    {
                        std::string prepareError;
                        preparedUiOverlayPassInput = PrepareUiOverlayPassInput(
                            impl,
                            frameContext,
                            *frameContext.commandBuffer,
                            effectivePassInput,
                            frameContextIndex,
                            prepareError);
                        if (!preparedUiOverlayPassInput.has_value())
                        {
                            Detail::LogSkippedPass(
                                renderScenePackage,
                                effectivePassInput,
                                prepareError.empty()
                                    ? "UI overlay resource preparation failed"
                                    : prepareError.c_str());
                            MarkCommandRecordingFailure(
                                submissionFrame,
                                prepareError.empty()
                                    ? "UI overlay resource preparation failed"
                                    : prepareError.c_str());
                            continue;
                        }
                    }
                    const auto& recordablePassInput = preparedUiOverlayPassInput.has_value()
                        ? *preparedUiOverlayPassInput
                        : effectivePassInput;
                    if (isUiOverlayPass)
                    {
                        auto visibilityWorkUnit = workUnit;
                        visibilityWorkUnit.commandInput = recordablePassInput;
                        MaterializeUiOverlayDependencyEdges(
                            visibilityWorkUnit.incomingDependencyEdges,
                            recordablePassInput);
                        const auto visibilityInput = Detail::BuildDependencyVisibilityPassInput(
                            visibilityWorkUnit,
                            visibilityWorkUnit.incomingDependencyEdges);
                        if (Detail::HasResourceVisibilityTransitions(visibilityInput))
                        {
                            const bool recordedVisibilityTransitions = Detail::RecordResourceVisibilityTransitions(
                                *frameContext.commandBuffer,
                                visibilityInput,
                                &frameContext);
                            recordedAnyCommand |= recordedVisibilityTransitions;
                            if (!recordedVisibilityTransitions)
                            {
                                MarkCommandRecordingFailure(
                                    submissionFrame,
                                    "Dependency visibility transition recording failed");
                                continue;
                            }
                        }
                    }
                    if (!Detail::BeginPassCommandPlan(
                        *frameContext.commandBuffer,
                        frameContext.swapchainBackbufferView,
                        frameContext.swapchainDepthStencilView,
                        recordablePassInput,
                        &frameContext))
                    {
                        Detail::LogSkippedPass(
                            renderScenePackage,
                            recordablePassInput,
                            "BeginPassCommandPlan failed");
                        MarkCommandRecordingFailure(
                            submissionFrame,
                            "BeginPassCommandPlan failed");
                        continue;
                    }

                    recordedAnyCommand = true;
                    const auto* effectivePassProfileScopeName =
                        Detail::ResolvePassProfileScopeName(recordablePassInput);
                    NLS_PROFILE_NAMED_SCOPE(effectivePassProfileScopeName);
                    frameContext.commandBuffer->BeginGpuProfileScope(effectivePassProfileScopeName, __FUNCTION__);
                    uint64_t recordedDrawCount = 0u;
                    if (isUiOverlayPass)
                    {
                        const auto uiResult = impl.uiOverlayRenderer.RecordPrepared(
                            *frameContext.commandBuffer,
                            *recordablePassInput.uiDrawDataSnapshot,
                            frameContextIndex);
                        if (!uiResult.success)
                        {
                            frameContext.commandBuffer->EndGpuProfileScope();
                            Detail::EndPassCommandPlan(*frameContext.commandBuffer, &recordablePassInput, &frameContext);
                            Detail::LogSkippedPass(
                                renderScenePackage,
                                recordablePassInput,
                                uiResult.message.empty()
                                    ? "UI overlay recording failed"
                                    : uiResult.message.c_str());
                            MarkCommandRecordingFailure(
                                submissionFrame,
                                uiResult.message.empty()
                                    ? "UI overlay recording failed"
                                    : uiResult.message.c_str());
                            continue;
                        }
                        recordedDrawCount = uiResult.recordedDraws
                            ? effectivePassInput.drawCount
                            : 0u;
                    }
                    else
                    {
                        recordedDrawCount = Detail::RecordPreparedDrawCommandsForPass(
                            frameContext.commandBuffer.get(),
                            recordablePassInput);
                    }
                    frameContext.commandBuffer->EndGpuProfileScope();
                    Detail::EndPassCommandPlan(*frameContext.commandBuffer, &recordablePassInput, &frameContext);
                    if (!isUiOverlayPass &&
                        recordedDrawCount == 0u &&
                        !recordablePassInput.recordedDrawCommands.empty())
                    {
                        Detail::LogSkippedPass(
                            renderScenePackage,
                            recordablePassInput,
                            "No draws recorded but commands were expected");
                        MarkCommandRecordingFailure(
                            submissionFrame,
                            "No draws recorded but commands were expected");
                        continue;
                    }

                    submissionFrame->recordedPassCount += 1u;
                    submissionFrame->recordedDrawCount += recordedDrawCount;
                    submissionFrame->recordedWorkUnitCount += 1u;
                }

                if (renderScenePackage.targetsSwapchain && !ensureSwapchainAcquisition())
                    return;

                submissionFrame->usedParallelCommandPath = false;
                submissionFrame->parallelRecordingWorkerCount = 0u;
                submissionFrame->translatedWorkUnitCount = 0u;
                submissionFrame->usedTranslationMerge = false;
                submissionFrame->usedSerialCommandPath = !workUnits.empty();
                const auto extractionVisibilityInput =
                    Render::FrameGraph::BuildExtractionVisibilityPassInput(renderScenePackage);
                if (Detail::HasResourceVisibilityTransitions(extractionVisibilityInput) &&
                    beginMainCommandBufferIfNeeded())
                {
                    const bool recordedPostPassTransitions = Detail::RecordResourceVisibilityTransitions(
                        *frameContext.commandBuffer,
                        extractionVisibilityInput,
                        &frameContext);
                    recordedAnyCommand |= recordedPostPassTransitions;
                    if (!recordedPostPassTransitions)
                    {
                        MarkCommandRecordingFailure(
                            submissionFrame,
                            "Post-pass transition recording failed");
                    }
                }

                if (recordedAnyCommand)
                {
                    if (frameContext.commandBuffer->IsRecording())
                        frameContext.commandBuffer->End();
                    Detail::ThreadedQueueSubmissionBatch batch;
                    batch.queueType = Render::RHI::QueueType::Graphics;
                    batch.commandBuffers.push_back(frameContext.commandBuffer);
                    submitPlan->batches.push_back(std::move(batch));
                }

                if (submissionFrame->usedSerialCommandPath &&
                    !submissionFrame->usedParallelCommandPath &&
                    submissionFrame->parallelFallbackReason.empty())
                {
                    if (frameContext.resourceStateTracker != nullptr)
                        noteParallelFallback("resource state tracker active; using serial command recording");
                    else if (impl.explicitDevice == nullptr)
                        noteParallelFallback("no explicit RHI device; using serial command recording");
                    else if (!parallelRecordingReady)
                        noteParallelFallback("ParallelCommandRecording capability unavailable; using serial command recording");
                    else if (!parallelTranslationReady)
                        noteParallelFallback("ParallelCommandTranslation capability unavailable; using serial command recording");
                    else if (workUnits.empty())
                        noteParallelFallback("no parallel command work units; using serial command recording");
                    else
                        noteParallelFallback("parallel command work units fell back to serial command recording");
                }
            }
        }

        if (submitPlan->batches.empty() &&
            renderScenePackage.targetsSwapchain &&
            frameContext.commandBuffer != nullptr &&
            !HasCommandRecordingFailure(submissionFrame))
        {
            frameContext.commandBuffer->Begin();
            if (!frameContext.commandBuffer->IsRecording())
            {
                MarkCommandRecordingFailure(submissionFrame, "Empty swapchain command buffer failed to begin");
                return;
            }
            frameContext.commandBuffer->End();
            Detail::ThreadedQueueSubmissionBatch batch;
            batch.queueType = Render::RHI::QueueType::Graphics;
            batch.consumesSwapchainImage = renderScenePackage.targetsSwapchain;
            batch.commandBuffers.push_back(frameContext.commandBuffer);
            submitPlan->batches.push_back(std::move(batch));
        }
    }

    void PrepareThreadedRhiSubmitPlan(const ThreadedRhiSubmissionExecutionPtr& execution)
    {
        NLS_PROFILE_SCOPE();
        if (execution == nullptr || !execution->usesRecordedParallelBatch)
        {
            // Serial capability combinations record directly into the submission
            // plan, so this node is the explicit serial no-op of the chain.
            return;
        }

        DriverImpl& impl = *execution->impl;
        Render::RHI::RHIFrameContext& frameContext = *execution->frameContext;
        Detail::AsyncComputeSubmitPlan& submitPlan = execution->submitPlan;
        RhiSubmissionFrame& submissionFrame = execution->submissionFrame;

        const auto translatedBatch = BuildPreparedCommandSubmissionUnits(execution->recordedParallelBatch);
        // Record-stage and translation-stage failures are aggregated together after
        // translation, so a failed visibility batch still marks the submission.
        MarkParallelCommandRecordingFailures(&submissionFrame, execution->recordedParallelBatch);
        for (const auto& failureReason : translatedBatch.commandRecordingFailureReasons)
        {
            MarkCommandRecordingFailure(
                &submissionFrame,
                failureReason.c_str());
        }
        submitPlan = Detail::BuildAsyncComputeSubmitPlan(
            impl,
            frameContext,
            translatedBatch.submissions);
        if (!submitPlan.failureDiagnostic.empty())
            MarkCommandRecordingFailure(&submissionFrame, submitPlan.failureDiagnostic.c_str());
        submissionFrame.recordedPassCount = translatedBatch.recordedPassCount;
        submissionFrame.recordedDrawCount = translatedBatch.recordedDrawCount;
        submissionFrame.recordedWorkUnitCount = translatedBatch.recordedWorkUnitCount;
        submissionFrame.parallelRecordingWorkerCount =
            execution->recordedParallelBatch.parallelWorkerCountUsed;
        submissionFrame.translatedWorkUnitCount = translatedBatch.translatedWorkUnitCount;
        submissionFrame.usedParallelCommandPath =
            execution->recordedParallelBatch.usedParallelRecording ||
            translatedBatch.usedTranslationMerge;
        submissionFrame.usedTranslationMerge = translatedBatch.usedTranslationMerge;
        submissionFrame.usedSerialCommandPath =
            !submissionFrame.usedParallelCommandPath &&
            !execution->recordedParallelBatch.recordedWorkUnits.empty();
    }

    void QueueSubmitThreadedRhiSubmission(const ThreadedRhiSubmissionExecutionPtr& execution)
    {
        if (execution == nullptr || execution->frameContext == nullptr)
            return;

        DriverImpl& impl = *execution->impl;
        Render::RHI::RHIFrameContext& frameContext = *execution->frameContext;
        const RenderScenePackage& renderScenePackage = execution->renderScenePackage;
        Detail::AsyncComputeSubmitPlan& submitPlan = execution->submitPlan;
        RhiSubmissionFrame* submissionFrame = &execution->submissionFrame;
        const size_t frameContextIndex = execution->frameContextIndex;

        NLS_PROFILE_SCOPE();
        if (HasCommandRecordingFailure(submissionFrame))
        {
            // Soft failure: this frame retires through the remaining nodes so the
            // lifecycle still observes a failed retirement.
            execution->submissionSucceeded = false;
            execution->failureReason = submissionFrame->parallelFallbackReason.empty()
                ? std::string("RHI command recording failed before queue submission")
                : std::string("RHI command recording failed before queue submission; ") +
                    submissionFrame->parallelFallbackReason;
            return;
        }
        if (!Detail::CompleteThreadedRhiSubmissionTelemetry(
            impl,
            frameContext,
            renderScenePackage,
            execution->preResetTrackerStats,
            &execution->descriptorStats,
            submitPlan,
            submissionFrame,
            execution->queueTelemetry))
        {
            execution->submissionSucceeded = false;
            execution->failureReason =
                "RHI frame contains no recorded offscreen pass; plannedPasses=" +
                std::to_string(renderScenePackage.passCommandInputs.size()) +
                " visibleDraws=" + std::to_string(renderScenePackage.visibleDrawCount);
            MarkCommandRecordingFailure(submissionFrame, execution->failureReason.c_str());
            return;
        }

        execution->submissionAttempted = true;
        auto graphicsQueue = impl.explicitDevice != nullptr
            ? impl.explicitDevice->GetQueue(Render::RHI::QueueType::Graphics)
            : nullptr;
        auto computeQueue = impl.explicitDevice != nullptr
            ? impl.explicitDevice->GetQueue(Render::RHI::QueueType::Compute)
            : nullptr;
        bool submissionSucceeded = true;
        bool submittedGpuWorkMayNeedFenceConfirmation = false;
        bool frameFenceSignalMayHaveBeenQueued = false;
        bool frameFenceResetForSubmission = false;
        bool deviceLostDuringSubmit = false;
        const bool postSubmitReadbackNeedsLastComputeCompletion =
            std::any_of(
                renderScenePackage.postSubmitBufferReadbacks.begin(),
                renderScenePackage.postSubmitBufferReadbacks.end(),
                [](const PostSubmitBufferReadbackRequest& request)
                {
                    return request.waitForLastComputeQueueCompletion;
                });
        if (postSubmitReadbackNeedsLastComputeCompletion)
        {
            Detail::EnsureLastComputeQueueCompletionSemaphore(impl, frameContext, submitPlan);
        }

        size_t firstGraphicsBatchIndex = submitPlan.batches.size();
        size_t lastGraphicsBatchIndex = submitPlan.batches.size();
        size_t firstSwapchainConsumingBatchIndex = submitPlan.batches.size();
        for (size_t batchIndex = 0u; batchIndex < submitPlan.batches.size(); ++batchIndex)
        {
            if (submitPlan.batches[batchIndex].queueType != Render::RHI::QueueType::Graphics)
                continue;

            if (firstGraphicsBatchIndex == submitPlan.batches.size())
                firstGraphicsBatchIndex = batchIndex;
            if (firstSwapchainConsumingBatchIndex == submitPlan.batches.size() &&
                submitPlan.batches[batchIndex].consumesSwapchainImage)
                firstSwapchainConsumingBatchIndex = batchIndex;
            lastGraphicsBatchIndex = batchIndex;
        }
        // The acquired image has to be waited on by the submit that first touches
        // it. If the plan split the offscreen work from the swapchain work, that is
        // the swapchain batch, not the first graphics batch.
        const size_t acquireWaitBatchIndex =
            firstSwapchainConsumingBatchIndex != submitPlan.batches.size()
                ? firstSwapchainConsumingBatchIndex
                : firstGraphicsBatchIndex;

        for (size_t batchIndex = 0u; batchIndex < submitPlan.batches.size(); ++batchIndex)
        {
            NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::SubmitBatch");
            auto& batch = submitPlan.batches[batchIndex];
            auto queue = batch.queueType == Render::RHI::QueueType::Compute
                ? computeQueue
                : graphicsQueue;
            if (queue == nullptr)
                continue;

            Render::RHI::RHISubmitDesc submitDesc;
            submitDesc.commandBuffers = batch.commandBuffers;
            submitDesc.waitSemaphores = batch.waitSemaphores;
            submitDesc.signalSemaphores = batch.signalSemaphores;

            if (batchIndex == acquireWaitBatchIndex &&
                frameContext.hasAcquiredSwapchainImage &&
                frameContext.imageAcquiredSemaphore != nullptr)
            {
                submitDesc.waitSemaphores.push_back(frameContext.imageAcquiredSemaphore);
            }

            const bool hasExternalSceneOutput =
                Render::FrameGraph::HasExternalSceneOutput(renderScenePackage);
            const bool isLastBatch = batchIndex + 1u == submitPlan.batches.size();
            const bool isLastGraphicsBatch = batchIndex == lastGraphicsBatchIndex;
            const bool signalRenderFinished =
                (renderScenePackage.targetsSwapchain || hasExternalSceneOutput) &&
                isLastGraphicsBatch &&
                frameContext.renderFinishedSemaphore != nullptr;
            if (signalRenderFinished)
            {
                submitDesc.signalSemaphores.push_back(frameContext.renderFinishedSemaphore);
            }
            const bool shouldPresentAfterThisBatch =
                renderScenePackage.targetsSwapchain &&
                isLastGraphicsBatch &&
                frameContext.hasAcquiredSwapchainImage &&
                impl.explicitSwapchain != nullptr &&
                batch.queueType == Render::RHI::QueueType::Graphics;
            const bool signalFrameFenceOnPresent =
                shouldPresentAfterThisBatch &&
                isLastBatch;
            if (isLastBatch && !signalFrameFenceOnPresent)
            {
                submitDesc.signalFence = frameContext.frameFence;
            }
            execution->frameFenceResetForSubmission =
                execution->frameFenceResetForSubmission ||
                submitDesc.signalFence != nullptr;
            {
                const auto submitResult = RhiCommandListExecutor::Get().SubmitNative(queue, submitDesc);
                if (submitResult.Succeeded() && execution->usesRecordedParallelBatch)
                {
                    const auto& recorded = execution->recordedParallelBatch;
                    for (const auto& buffer : submitDesc.commandBuffers)
                    {
                        for (const auto& unit : recorded.recordedWorkUnits)
                        {
                            if (unit.commandBuffer == buffer && unit.commands != nullptr)
                                unit.commands->CommitResourceStates(*buffer);
                            if (unit.queueRelease != nullptr && unit.queueRelease->commandBuffer == buffer)
                                unit.queueRelease->commands->CommitResourceStates(*buffer);
                        }
                        if (recorded.postPassTransitions.has_value() &&
                            recorded.postPassTransitions->commandBuffer == buffer)
                            recorded.postPassTransitions->commands->CommitResourceStates(*buffer);
                    }
                }
                if (submitDesc.signalFence != nullptr &&
                    submitResult.frameFenceSignalQueued)
                {
                    execution->frameFenceSignalMayHaveBeenQueued = true;
                }
                execution->deviceLostDuringSubmit |=
                    submitResult.code == Render::RHI::RHIQueueOperationStatusCode::DeviceLost;
                execution->submittedGpuWorkMayNeedFenceConfirmation |= submitResult.Succeeded() ||
                    submitResult.mayHaveQueuedGpuWork;
                const bool submitted = RecordQueueOperationResult(
                    impl,
                    submitResult,
                    "RhiCommandListExecutor::SubmitCompiledExecution",
                    execution->queueTelemetry);
                if (submitted &&
                    hasExternalSceneOutput &&
                    signalRenderFinished)
                {
                    PublishSceneToUiWaitSemaphore(impl, frameContext);
                }
                if (!submitted)
                {
                    execution->submissionSucceeded = false;
                    execution->failureReason = submitResult.message;
                    break;
                }
            }

            // Present follows all queue submissions through the executor's lane.
            if (execution->submissionSucceeded && shouldPresentAfterThisBatch)
            {
                execution->presentRequested = true;
                execution->presentGraphicsBatchIndex = batchIndex;
                execution->presentSignalsFrameFence = signalFrameFenceOnPresent;
            }
        }

        if (submissionFrame != nullptr)
            submissionFrame->retirementFenceWaited = false;
    }

    void PresentThreadedRhiSubmission(const ThreadedRhiSubmissionExecutionPtr& execution)
    {
        if (execution == nullptr || execution->frameContext == nullptr || execution->presentRequested == false)
            return;
        if (execution->submissionSucceeded == false)
            return;

        DriverImpl& impl = *execution->impl;
        Render::RHI::RHIFrameContext& frameContext = *execution->frameContext;

        // The acquired image belongs to this frame until Present transfers ownership.
        BeginPostSubmitTextureReadbacks(impl, execution->renderScenePackage, frameContext,
            execution->submissionSucceeded, execution->queueTelemetry);
        if (impl.deviceLostDetected.load(std::memory_order_acquire) ||
            impl.unsafeGpuWorkQuarantined.load(std::memory_order_acquire))
        {
            execution->submissionSucceeded = false;
            execution->deviceLostDuringSubmit = impl.deviceLostDetected.load(std::memory_order_acquire);
            execution->failureReason = "pre-present texture readback lost the device or quarantined GPU work";
            return;
        }

        NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::Present");
        if (impl.renderDocCaptureController != nullptr)
            impl.renderDocCaptureController->OnPrePresent();

        Render::RHI::RHIPresentDesc presentDesc;
        presentDesc.swapchain = impl.explicitSwapchain;
        presentDesc.imageIndex = frameContext.swapchainImageIndex;
        if (frameContext.renderFinishedSemaphore != nullptr)
            presentDesc.waitSemaphores.push_back(frameContext.renderFinishedSemaphore);
        if (execution->presentSignalsFrameFence)
            presentDesc.signalFence = frameContext.frameFence;
        presentDesc.uiSignalSemaphore = execution->uiCompositionSignalSemaphore;
        presentDesc.uiSignalValue = execution->uiCompositionSignalValue;
        execution->frameFenceResetForSubmission =
            execution->frameFenceResetForSubmission ||
            presentDesc.signalFence != nullptr;

        bool presented = false;
        {
            auto graphicsQueue = impl.explicitDevice != nullptr
                ? impl.explicitDevice->GetQueue(Render::RHI::QueueType::Graphics)
                : nullptr;
            if (graphicsQueue == nullptr)
                return;
            const auto presentResult = RhiCommandListExecutor::Get().PresentNative(graphicsQueue, presentDesc);
            if (presentDesc.signalFence != nullptr && presentResult.frameFenceSignalQueued)
                execution->frameFenceSignalMayHaveBeenQueued = true;
            execution->deviceLostDuringSubmit |=
                presentResult.code == Render::RHI::RHIQueueOperationStatusCode::DeviceLost;
            execution->submittedGpuWorkMayNeedFenceConfirmation |= presentResult.Succeeded() ||
                presentResult.mayHaveQueuedGpuWork;
                presented = RecordQueueOperationResult(
                    impl,
                    presentResult,
                    "RhiCommandListExecutor::Present",
                    execution->queueTelemetry,
                    // A present that reports an out-of-date swapchain still retires the
                    // frame: its GPU work is queued and the swapchain is rebuilt next.
                    true);
            if (!presented)
                execution->failureReason = presentResult.message;
        }

        if (impl.renderDocCaptureController != nullptr)
            impl.renderDocCaptureController->OnPostPresent();

        execution->presentedAfterSubmitting = presented;
        if (presented == false)
        {
            execution->submissionSucceeded = false;
        }
    }

    void PreserveThreadedRhiSubmissionResources(const ThreadedRhiSubmissionExecutionPtr& execution)
    {
        if (execution == nullptr || execution->frameContext == nullptr)
            return;

        DriverImpl& impl = *execution->impl;
        const bool mustDeferRetirement =
            execution->submittedGpuWorkMayNeedFenceConfirmation &&
            execution->frameFenceSignalMayHaveBeenQueued &&
            execution->frameContext->frameFence != nullptr &&
            !execution->deviceLostDuringSubmit;
        const bool mustQuarantineResources =
            execution->submittedGpuWorkMayNeedFenceConfirmation && !mustDeferRetirement;

        if ((mustDeferRetirement || mustQuarantineResources) && !execution->submitResourcesRetained)
        {
            RetainThreadedSubmitPlanResourcesUntilNextFence(
                impl, execution->frameContextIndex, execution->submitPlan);
            {
                std::lock_guard lock(impl.frameResourceRetirementMutex);
                if (mustQuarantineResources)
                    impl.frameResourceSlots->QuarantineSlot(execution->frameContextIndex);
                auto& keepAlive = impl.frameResourceSlots->PeekSlot(execution->frameContextIndex)->ownedResources;
                for (const auto& unit : execution->recordedParallelBatch.recordedWorkUnits)
                {
                    if (unit.wasRecorded && unit.commands != nullptr)
                        keepAlive.push_back(unit.commands);
                    if (unit.queueRelease != nullptr && unit.queueRelease->wasRecorded)
                        keepAlive.push_back(unit.queueRelease->commands);
                }
                const auto& post = execution->recordedParallelBatch.postPassTransitions;
                if (post.has_value() && post->wasRecorded && post->commands != nullptr)
                    keepAlive.push_back(post->commands);
                for (const auto& request : execution->renderScenePackage.postSubmitTextureReadbacks)
                {
                    if (request.texture != nullptr)
                        keepAlive.push_back(request.texture);
                    if (request.destinationKeepAlive != nullptr)
                        keepAlive.push_back(request.destinationKeepAlive);
                    if (request.state != nullptr)
                        keepAlive.push_back(request.state);
                }
            }
            execution->submitResourcesRetained = true;
        }
        if (mustQuarantineResources && !execution->mustQuarantineFrameScopedResources)
        {
            Render::RHI::RHIQueueOperationResult quarantineResult;
            quarantineResult.mayHaveQueuedGpuWork = true;
            quarantineResult.message = "threaded submit has no reliable frame retirement fence";
            MarkQueuedGpuWorkWithoutRetirementFence(
                impl,
                "RhiCommandListExecutor::Readback",
                quarantineResult,
                execution->queueTelemetry);
        }
        execution->mustDeferFrameScopedRetirement = mustDeferRetirement;
        execution->mustQuarantineFrameScopedResources = mustQuarantineResources;
        execution->submissionFrame.deferredFrameScopedRetirement = mustDeferRetirement;
    }

    void ReadbackThreadedRhiSubmission(const ThreadedRhiSubmissionExecutionPtr& execution)
    {
        if (execution == nullptr || execution->frameContext == nullptr)
            return;

        DriverImpl& impl = *execution->impl;
        const RenderScenePackage& renderScenePackage = execution->renderScenePackage;
        RhiSubmissionFrame& submissionFrame = execution->submissionFrame;

        // The retirement fence decision runs after Present, because a swapchain
        // frame signals its retirement fence from the present operation.
        PreserveThreadedRhiSubmissionResources(execution);

        const bool recordingSucceeded = HasCommandRecordingFailure(&submissionFrame) == false;
        CopyDriverQueueTelemetryToSubmissionFrame(impl, submissionFrame, execution->queueTelemetry);
        submissionFrame.deferredFrameScopedRetirement =
            execution->mustDeferFrameScopedRetirement;
        submissionFrame.submittedSuccessfully =
            execution->submissionSucceeded &&
            recordingSucceeded &&
            submissionFrame.currentFrameQueueOperationFailureCount == 0u &&
            submissionFrame.deviceLostDetected == false;
        BeginPostSubmitBufferReadbacks(
            impl,
            renderScenePackage,
            submissionFrame.submittedSuccessfully,
            execution->submitPlan.lastComputeQueueCompletionSemaphore,
            execution->submitPlan.usedDedicatedComputeQueueSubmission,
            execution->queueTelemetry);
        if (!renderScenePackage.targetsSwapchain)
        {
            BeginPostSubmitTextureReadbacks(
                impl,
                renderScenePackage,
                *execution->frameContext,
                submissionFrame.submittedSuccessfully,
                execution->queueTelemetry);
        }
    }

    // CPU completion updates diagnostics; resource reuse still requires the GPU fence.
    void CompleteRhiFrameExecution(
        const ThreadedRhiSubmissionExecutionPtr& execution)
    {
        if (execution == nullptr || execution->impl == nullptr)
            return;
        if (execution->driver == nullptr)
            return;
        if (execution->cpuRetired)
            return;
        execution->cpuRetired = true;

        DriverImpl& impl = *execution->impl;
        const size_t frameContextIndex = execution->frameContextIndex;
        if (!execution->submissionFrame.submittedSuccessfully)
        {
            // A frame dropped because the swapchain moved under it is expected output
            // of a resize, not a defect: report it at info level so a real failure in
            // the same run is still the only warning an operator sees.
            const std::string message =
                std::string(execution->submissionFrame.droppedForSwapchainRecreation
                    ? "[Driver] Dropped threaded frame frameId="
                    : "[Driver] Failed threaded frame frameId=") +
                std::to_string(execution->submissionFrame.frameId) +
                " plannedPasses=" + std::to_string(execution->renderScenePackage.passCommandInputs.size()) +
                " recordedPasses=" + std::to_string(execution->submissionFrame.recordedPassCount) +
                " visibleDraws=" + std::to_string(execution->renderScenePackage.visibleDrawCount) +
                " reason=" + execution->submissionFrame.lastCommandRecordingFailure;
            if (execution->submissionFrame.droppedForSwapchainRecreation)
                NLS_LOG_INFO(message);
            else
                NLS_LOG_WARNING(message);
        }
#if defined(NLS_ENABLE_TEST_HOOKS)
        if (impl.rhiFrameObserver)
            impl.rhiFrameObserver(execution->submissionFrame);
#endif
        {
            std::lock_guard lock(impl.frameTelemetryMutex);
            auto& telemetry = impl.frameTelemetry;
            const auto& frame = execution->submissionFrame;
            if (frame.submittedSuccessfully && frame.currentFrameQueueOperationFailureCount == 0u &&
                !frame.deviceLostDetected)
            {
                telemetry.latestRetiredFrameId = (std::max)(telemetry.latestRetiredFrameId, frame.frameId);
                ++telemetry.successfullyRetiredFrameCount;
            }
            else if (frame.droppedForSwapchainRecreation)
            {
                telemetry.latestDroppedForSwapchainRecreationFrameId =
                    (std::max)(telemetry.latestDroppedForSwapchainRecreationFrameId, frame.frameId);
                ++telemetry.droppedForSwapchainRecreationFrameCount;
            }
            else
            {
                telemetry.latestFailedRetiredFrameId = (std::max)(telemetry.latestFailedRetiredFrameId, frame.frameId);
                ++telemetry.failedRetiredFrameCount;
            }
            telemetry.renderSceneAttribution = RenderSceneAttribution::RendererPrepared;
            telemetry.rhiSubmissionAttribution = execution->attribution;
            telemetry.descriptorMainlineActive = frame.usedDescriptorAllocator;
            telemetry.transientLifetimeMainlineActive = frame.usedResourceStateTracker;
            telemetry.retirementMainlineActive = frame.retirementFenceWaited || frame.deferredFrameScopedRetirement;
            telemetry.pipelineMainlineActive = frame.pipelineMainlineActive;
            telemetry.descriptorBypassCount = frame.usedDescriptorAllocator ? 0u : 1u;
            telemetry.transientLifetimeBypassCount = frame.usedResourceStateTracker ? 0u : 1u;
            telemetry.retirementBypassCount = telemetry.retirementMainlineActive ? 0u : 1u;
            telemetry.pipelineBypassCount = frame.pipelineBypassCount;
            telemetry.transientTextureRegistrationCount = frame.transientTextureRegistrationCount;
            telemetry.transientBufferRegistrationCount = frame.transientBufferRegistrationCount;
            telemetry.retiredTransientTextureCount = frame.retiredTransientTextureCount;
            telemetry.retiredTransientBufferCount = frame.retiredTransientBufferCount;
            telemetry.descriptorTransientPeak = frame.descriptorTransientPeak;
            telemetry.descriptorAllocationFailures = frame.descriptorAllocationFailures;
            telemetry.pipelineCacheGraphicsHits = frame.pipelineCacheGraphicsHits;
            telemetry.pipelineCacheGraphicsMisses = frame.pipelineCacheGraphicsMisses;
            telemetry.pipelineCacheGraphicsStores = frame.pipelineCacheGraphicsStores;
            telemetry.pipelineCacheGraphicsEntries = frame.pipelineCacheGraphicsEntries;
            telemetry.pipelineCacheComputeHits = frame.pipelineCacheComputeHits;
            telemetry.pipelineCacheComputeMisses = frame.pipelineCacheComputeMisses;
            telemetry.pipelineCacheComputeStores = frame.pipelineCacheComputeStores;
            telemetry.pipelineCacheComputeEntries = frame.pipelineCacheComputeEntries;
            telemetry.parallelCommandWorkUnitCount = frame.recordedWorkUnitCount;
            telemetry.parallelRecordingWorkerCount = frame.parallelRecordingWorkerCount;
            telemetry.parallelFallbackReason = frame.parallelFallbackReason;
            telemetry.queueOperationFailureCount = frame.queueOperationFailureCount;
            telemetry.lastQueueOperationFailure = frame.lastQueueOperationFailure;
            telemetry.currentFrameQueueOperationFailureCount = frame.currentFrameQueueOperationFailureCount;
            telemetry.currentFrameLastQueueOperationFailure = frame.currentFrameLastQueueOperationFailure;
            telemetry.deviceLostDetected = frame.deviceLostDetected;
            telemetry.deviceLostReason = frame.deviceLostReason;
            telemetry.unsafeGpuWorkQuarantined = frame.unsafeGpuWorkQuarantined;
            telemetry.unsafeGpuWorkQuarantineReason = frame.unsafeGpuWorkQuarantineReason;
            telemetry.retirementState = Data::FrameRetirementState::Ready;
        }

        if (impl.frameLatency != nullptr)
            (void)impl.frameLatency->PollCompletedFrames();
    }

    void RetireFrameResourceLease(const ThreadedRhiSubmissionExecutionPtr& execution)
    {
        DriverImpl& impl = *execution->impl;
        std::lock_guard retirementLock(impl.frameResourceRetirementMutex);
        if (impl.frameResourceSlots == nullptr || execution->mustQuarantineFrameScopedResources)
            return;
        const auto* resourceSlot = impl.frameResourceSlots->PeekSlot(execution->frameContextIndex);
        if (resourceSlot == nullptr || !resourceSlot->inFlight || resourceSlot->quarantined ||
            resourceSlot->leaseGeneration != execution->resourceLeaseGeneration || resourceSlot->gpuFence != nullptr)
            return;
        if (execution->mustDeferFrameScopedRetirement && execution->frameContext != nullptr)
        {
            auto& slot = *impl.frameResourceSlots->PeekSlot(execution->frameContextIndex);
            slot.retirementPending = true;
            slot.uiTextureRetirementFrameId = execution->submissionFrame.uiOverlaySnapshotFrameId;
            impl.frameResourceSlots->RetireSlot(execution->frameContextIndex, execution->frameContext->frameFence);
        }
        else
            impl.frameResourceSlots->RetireSlot(execution->frameContextIndex, nullptr);
    }

    void RetireThreadedRhiSubmission(const ThreadedRhiSubmissionExecutionPtr& execution)
    {
        if (execution == nullptr)
            return;

        DriverImpl& impl = *execution->impl;
        RhiSubmissionFrame& submissionFrame = execution->submissionFrame;
        const size_t frameContextIndex = execution->frameContextIndex;

        // Frames that never began still retire, they just have no render-side context
        // to finalize. This preserves the pre-graph behaviour for begin-frame failures.
        if (execution->frameContext != nullptr)
        {
            Render::RHI::RHIFrameContext& frameContext = *execution->frameContext;
            if (impl.renderDocCaptureController != nullptr)
            {
                impl.renderDocCaptureController->OnPostFrame(
                    execution->renderScenePackage.targetsSwapchain,
                    Render::FrameGraph::HasExternalSceneOutput(execution->renderScenePackage));
            }

            if (execution->framePrepared)
            {
                FinalizeThreadedRhiFrameContext(
                    impl,
                    frameContext,
                    execution->submitPlan,
                    execution->mustDeferFrameScopedRetirement ||
                        execution->mustQuarantineFrameScopedResources);
            }
            execution->frameRetired = true;
            Detail::LogCompletedThreadedRhiSubmission(impl, frameContext, submissionFrame);
        }

        // CPU completion publishes retirement; frame resources keep their GPU fence.
        CompleteRhiFrameExecution(execution);
        RetireFrameResourceLease(execution);

        if (execution->driver != nullptr &&
            submissionFrame.submittedSuccessfully &&
            execution->renderScenePackage.targetsSwapchain &&
            execution->renderScenePackage.uiDrawDataSnapshot != nullptr)
        {
            DriverUIAccess::RecordUiOverlayPresentationSucceeded(
                *execution->driver,
                execution->renderScenePackage.uiDrawDataSnapshot,
                execution->renderScenePackage.uiOverlayPresentationInvalidationGeneration);
        }

        if (execution->driver != nullptr &&
            submissionFrame.submittedSuccessfully &&
            !execution->renderScenePackage.targetsSwapchain &&
            execution->renderScenePackage.externalSceneOutputTextureCount > 0u)
        {
            auto externalOutputIdentities =
                execution->renderScenePackage.externalSceneOutputIdentities;
            if (externalOutputIdentities.empty() &&
                execution->renderScenePackage.externalSceneOutputIdentity != 0u)
            {
                externalOutputIdentities.push_back(
                    execution->renderScenePackage.externalSceneOutputIdentity);
            }
            DriverUIAccess::NotifyUiTextureContentChanged(
                *execution->driver,
                externalOutputIdentities);
        }

        // Wake the producer so it can observe completion and release the RHI lane.
        Detail::NotifyThreadedWorkers(impl);
    }

    void FailThreadedRhiSubmissionStage(
        const ThreadedRhiSubmissionExecutionPtr& execution,
        std::string reason)
    {
        if (execution != nullptr && execution->failureReason.empty())
            execution->failureReason = std::move(reason);

        // Stop subsequent operations; the producer performs the failed retirement.
        throw ThreadedRhiSubmissionStageAbort {};
    }

    void AbortThreadedRhiSubmission(const ThreadedRhiSubmissionExecutionPtr& execution)
    {
        if (execution == nullptr)
            return;

        // An enqueued acquisition still executes on the ordered lane even when the
        // frame fails earlier. Consume it here so the swapchain semaphore signal and
        // the lane's result are retired with this frame instead of leaking forward.
        (void)Detail::ResolveThreadedSwapchainAcquisition(execution);

        DriverImpl& impl = *execution->impl;
        RhiSubmissionFrame& submissionFrame = execution->submissionFrame;
        const std::string reason = execution->failureReason.empty()
            ? std::string("threaded RHI submission stage failed")
            : execution->failureReason;
        submissionFrame.submittedSuccessfully = false;
        submissionFrame.recordedVisibleWork = false;
        MarkCommandRecordingFailure(&submissionFrame, reason.c_str());
        CopyDriverQueueTelemetryToSubmissionFrame(impl, submissionFrame, execution->queueTelemetry);
        FailPostSubmitReadbacks(execution->renderScenePackage, reason.c_str());
        if (execution->framePrepared &&
            execution->frameContext != nullptr &&
            execution->frameRetired == false)
        {
            PreserveThreadedRhiSubmissionResources(execution);
            FinalizeThreadedRhiFrameContext(
                impl,
                *execution->frameContext,
                execution->submitPlan,
                execution->mustDeferFrameScopedRetirement ||
                    execution->mustQuarantineFrameScopedResources);
            execution->frameRetired = true;
        }

        // A failed or cancelled frame still owns a lifecycle slot. Without
        // this hand-off the slot stays at `RhiSubmitting` forever, so the frame is
        // never reusable and every later drain burns its whole watchdog.
        CompleteRhiFrameExecution(execution);
        RetireFrameResourceLease(execution);
        Detail::NotifyThreadedWorkers(impl);
        NLS_LOG_WARNING(("[Driver] Threaded RHI submission aborted: " + reason).c_str());
    }

    bool DispatchPreparedRhiBatch(const ThreadedRhiSubmissionExecutionPtr& execution)
    {
        DriverImpl& impl = *execution->impl;

        // Normally the recording step already bound the backbuffer. This covers the
        // paths that record no pass at all: without it the frame would silently skip
        // Present while its acquisition had already been consumed from the lane.
        if (execution->renderScenePackage.targetsSwapchain &&
            !Detail::ResolveThreadedSwapchainAcquisition(execution))
        {
            return false;
        }

        if (execution->frameContext == nullptr ||
            !Detail::CompleteThreadedRhiSubmissionTelemetry(
                impl, *execution->frameContext, execution->renderScenePackage,
                execution->preResetTrackerStats, &execution->descriptorStats,
                execution->submitPlan, &execution->submissionFrame,
                execution->queueTelemetry))
        {
            execution->failureReason = "RHI frame contains no recorded offscreen pass";
            return false;
        }
        std::vector<RecordedParallelCommandWorkUnit*> units;
        for (auto& unit : execution->recordedParallelBatch.recordedWorkUnits)
            if (unit.wasRecorded && unit.commands != nullptr)
                units.push_back(&unit);
        std::stable_sort(units.begin(), units.end(), [](const auto* lhs, const auto* rhs)
            { return lhs->workUnit.submissionOrder < rhs->workUnit.submissionOrder; });
        std::vector<RecordedParallelCommandWorkUnit*> orderedUnits;
        for (auto* unit : units)
        {
            if (unit->queueRelease != nullptr && unit->queueRelease->wasRecorded)
                orderedUnits.push_back(unit->queueRelease.get());
            orderedUnits.push_back(unit);
        }
        units = std::move(orderedUnits);
        if (auto& post = execution->recordedParallelBatch.postPassTransitions;
            post.has_value() && post->wasRecorded && post->commands != nullptr)
            units.push_back(&*post);

        RhiCommandListBatch batch;
        batch.debugName = "RenderFrame::" + std::to_string(execution->renderScenePackage.frameId);
        batch.parallelTranslation = execution->parallelTranslationReady || execution->parallelRecordingReady;
        batch.commitResourceStates = false;
        batch.submitSignalsFences = false;
        batch.retirementFence = execution->frameContext->frameFence;
        batch.onComplete = [execution] { Detail::NotifyThreadedWorkers(*execution->impl); };
        batch.requestsPresent = execution->renderScenePackage.targetsSwapchain &&
            execution->frameContext != nullptr && execution->frameContext->hasAcquiredSwapchainImage;
        for (const auto* unit : units)
            batch.commandLists.push_back(unit->commands);
        batch.acquireCommandBuffer = [execution, units](const size_t index, RHI::QueueType, uint64_t)
        {
            auto* unit = units.at(index);
            if ((unit->workUnit.eligibleForParallelRecording || unit->workUnit.eligibleForParallelTranslation) &&
                execution->impl->taskGraph != nullptr && execution->impl->taskGraph->IsWorkerThread())
                unit->translationThreadId = std::this_thread::get_id();
            return unit->commandBuffer;
        };
        batch.submit = [execution, units](const auto&)
        {
            std::unordered_set<std::thread::id> workers;
            for (const auto* unit : units)
                if (unit->translationThreadId != std::thread::id{})
                    workers.insert(unit->translationThreadId);
            auto& frame = execution->submissionFrame;
            if (execution->usesRecordedParallelBatch)
            {
                frame.parallelRecordingWorkerCount = static_cast<uint32_t>(workers.size());
                frame.usedParallelCommandPath = !workers.empty() || frame.usedTranslationMerge;
                frame.usedSerialCommandPath = !frame.usedParallelCommandPath;
            }
            QueueSubmitThreadedRhiSubmission(execution);
            RHI::RHIQueueOperationResult result;
            if (!execution->submissionSucceeded)
            {
                result.code = execution->deviceLostDuringSubmit
                    ? RHI::RHIQueueOperationStatusCode::DeviceLost : RHI::RHIQueueOperationStatusCode::BackendFailure;
                result.message = execution->failureReason.empty()
                    ? execution->submissionFrame.currentFrameLastQueueOperationFailure : execution->failureReason;
            }
            result.mayHaveQueuedGpuWork = execution->submittedGpuWorkMayNeedFenceConfirmation;
            result.frameFenceSignalQueued = execution->frameFenceSignalMayHaveBeenQueued;
            return result;
        };
        batch.present = [execution](const auto&)
        {
            PresentThreadedRhiSubmission(execution);
            RHI::RHIQueueOperationResult result;
            if (!execution->submissionSucceeded)
            {
                result.code = execution->deviceLostDuringSubmit
                    ? RHI::RHIQueueOperationStatusCode::DeviceLost : RHI::RHIQueueOperationStatusCode::BackendFailure;
                result.message = execution->failureReason;
            }
            result.mayHaveQueuedGpuWork = execution->submittedGpuWorkMayNeedFenceConfirmation;
            result.frameFenceSignalQueued = execution->frameFenceSignalMayHaveBeenQueued;
            return result;
        };
        execution->rhiDispatched = RhiCommandListExecutor::Get().DispatchBatch(
            std::move(batch), &execution->rhiCompletion);
        if (!execution->rhiDispatched)
            execution->failureReason = execution->rhiCompletion.GetDiagnostic();
        Detail::NotifyThreadedWorkers(impl);
        return execution->rhiDispatched;
    }

    void WaitForRhiBatch(const ThreadedRhiSubmissionExecutionPtr& execution)
    {
        while (execution->rhiDispatched && !execution->rhiCompletion.IsComplete())
        {
            if (execution->impl->taskGraph != nullptr)
                (void)execution->impl->taskGraph->PumpCurrentThread(1u);
            (void)execution->rhiCompletion.WaitFor(std::chrono::milliseconds(1));
        }
    }

    void CompletePreparedRhiFrame(const ThreadedRhiSubmissionExecutionPtr& execution)
    {
        if (!execution->rhiDispatched || !execution->rhiCompletion.Succeeded())
        {
            if (execution->failureReason.empty() && execution->rhiDispatched)
                execution->failureReason = execution->rhiCompletion.GetDiagnostic();
            AbortThreadedRhiSubmission(execution);
            return;
        }
        const char* phase = "Readback";
        try
        {
            ReadbackThreadedRhiSubmission(execution);
            phase = "Retire";
            RetireThreadedRhiSubmission(execution);
        }
        catch (const std::exception& error)
        {
            execution->failureReason = std::string(phase) + ": " + error.what();
            AbortThreadedRhiSubmission(execution);
        }
        catch (...)
        {
            execution->failureReason = std::string(phase) + ": unknown exception";
            AbortThreadedRhiSubmission(execution);
        }
    }

    void Detail::ExecuteThreadedSubmitPlan(
        DriverImpl& impl,
        Render::RHI::RHIFrameContext& frameContext,
        const RenderScenePackage& renderScenePackage,
        AsyncComputeSubmitPlan& submitPlan,
        RhiSubmissionFrame* submissionFrame,
        const size_t frameContextIndex)
    {
        // Already recorded native plans enter the same executor FIFO.
        auto execution = std::make_shared<ThreadedRhiSubmissionExecution>();
        execution->impl = &impl;
        execution->frameContextIndex = frameContextIndex;
        execution->frameContext = &frameContext;
        execution->renderScenePackage = renderScenePackage;
        execution->submitPlan = submitPlan;
        execution->framePrepared = true;
        {
            std::lock_guard retirementLock(impl.frameResourceRetirementMutex);
            if (impl.frameResourceSlots != nullptr)
                if (const auto* slot = impl.frameResourceSlots->PeekSlot(frameContextIndex))
                    execution->resourceLeaseGeneration = slot->leaseGeneration;
        }
        if (submissionFrame != nullptr)
            execution->submissionFrame = *submissionFrame;

        (void)DispatchPreparedRhiBatch(execution);
        WaitForRhiBatch(execution);
        CompletePreparedRhiFrame(execution);

        submitPlan = execution->submitPlan;
        if (submissionFrame != nullptr)
            *submissionFrame = execution->submissionFrame;
    }
    namespace
    {
        struct ScheduledThreadedRhiSubmission
        {
            bool scheduled = false;
            bool executedInline = false;
            bool inlineSucceeded = false;
        };

        /** Resolves frame metadata and native translation capabilities. */
        void PrepareThreadedRhiSubmissionExecution(
            DriverImpl& impl,
            const ThreadedRhiSubmissionExecutionPtr& execution)
        {
            const RenderScenePackage& renderScenePackage = execution->renderScenePackage;
            {
                std::lock_guard retirementLock(impl.frameResourceRetirementMutex);
                if (impl.frameResourceSlots != nullptr)
                    if (const auto* slot = impl.frameResourceSlots->PeekSlot(execution->frameContextIndex);
                        slot != nullptr && slot->inFlight && slot->gpuFence == nullptr)
                        execution->resourceLeaseGeneration = slot->leaseGeneration;
            }
            // Consume once on the render lane; a later frame cannot overwrite
            // the signal that this submission's Present must wait for.
            {
                std::lock_guard lock(impl.uiCompositionSignalMutex);
                if (renderScenePackage.targetsSwapchain)
                {
                    execution->uiCompositionSignalSemaphore = impl.uiRenderFinishedSemaphore;
                    execution->uiCompositionSignalValue = impl.uiRenderFinishedValue;
                }
                impl.uiRenderFinishedSemaphore = {};
                impl.uiRenderFinishedValue = 0u;
            }
            const auto externalOutputSummary =
                Render::FrameGraph::BuildExternalSceneOutputSummary(renderScenePackage);
            RhiSubmissionFrame& submissionFrame = execution->submissionFrame;
            submissionFrame.frameId = renderScenePackage.frameId;
            submissionFrame.uiOverlaySnapshotFrameId =
                ResolveSubmittedUiOverlaySnapshotFrameId(renderScenePackage);
            submissionFrame.offscreenOnly = renderScenePackage.targetsSwapchain == false;
            submissionFrame.usedExternalOutputBridge = externalOutputSummary.hasExternalOutput;
            submissionFrame.externalOutputTextureCount =
                Render::FrameGraph::CountExternalSceneOutputSampledTextures(renderScenePackage);

            const bool parallelCommandRecordingSupported =
                impl.explicitDevice != nullptr &&
                impl.explicitDevice->GetCapabilities()
                    .GetFeature(Render::RHI::RHIDeviceFeature::ParallelCommandRecording)
                    .supported;
            const bool parallelCommandTranslationSupported =
                impl.explicitDevice != nullptr &&
                impl.explicitDevice->GetCapabilities()
                    .GetFeature(Render::RHI::RHIDeviceFeature::ParallelCommandTranslation)
                    .supported;
            execution->supportsOrderedWorkUnitSubmission =
                ShouldUseOrderedParallelCommandSubmissionForPackage(impl, renderScenePackage);
            execution->parallelRecordingReady =
                execution->supportsOrderedWorkUnitSubmission &&
                parallelCommandRecordingSupported;
            execution->parallelTranslationReady =
                execution->supportsOrderedWorkUnitSubmission &&
                parallelCommandTranslationSupported;
        }

        bool ExecutePreparedRhiFrame(
            const ThreadedRhiSubmissionExecutionPtr& execution)
        {
            const char* phase = "Prepare";
            try
            {
                auto* frameContext = Detail::BeginThreadedRhiFrame(
                    *execution->impl,
                    execution->renderScenePackage,
                    execution->frameContextIndex,
                    &execution->submissionFrame,
                    &execution->preResetTrackerStats,
                    &execution->descriptorStats,
                    &execution->resourceLeaseGeneration,
                    execution->queueTelemetry,
                    execution->swapchainAcquisition);
                if (frameContext == nullptr)
                {
                    execution->submissionSucceeded = false;
                    execution->failureReason =
                        "RHI frame could not begin its frame context";
                    CopyDriverQueueTelemetryToSubmissionFrame(
                        *execution->impl,
                        execution->submissionFrame,
                        execution->queueTelemetry);
                    MarkCommandRecordingFailure(
                        &execution->submissionFrame,
                        "threaded RHI frame could not begin its frame context");
                }
                else
                {
                    execution->frameContext = frameContext;
                    execution->framePrepared = true;
                    RecordThreadedRhiSubmissionWork(execution);
                }
                PrepareThreadedRhiSubmitPlan(execution);
                if (HasCommandRecordingFailure(&execution->submissionFrame))
                {
                    execution->failureReason = execution->submissionFrame.lastCommandRecordingFailure;
                    return false;
                }
                phase = "Dispatch";
                return DispatchPreparedRhiBatch(execution);
            }
            catch (const ThreadedRhiSubmissionStageAbort&)
            {
                return false;
            }
            catch (const std::exception& error)
            {
                execution->failureReason = std::string(phase) + ": " + error.what();
            }
            catch (...)
            {
                execution->failureReason = std::string(phase) + ": unknown exception";
            }
            return false;
        }

        ScheduledThreadedRhiSubmission ScheduleThreadedRhiSubmission(
            DriverImpl& impl,
            const ThreadedRhiSubmissionExecutionPtr& execution)
        {
            ScheduledThreadedRhiSubmission result;
            if (execution == nullptr)
                return result;

            if (RenderingThreadSystem::IsInRenderingThread() ||
                !impl.renderingThreads.IsInitialized() || impl.renderingThreads.IsSuspended())
            {
                result.inlineSucceeded = ExecutePreparedRhiFrame(execution);
                result.executedInline = true;
                return result;
            }
            if (impl.taskGraph == nullptr || !impl.taskGraph->IsStarted())
                return result;

            const auto handle = impl.taskGraph->Submit(
                [execution]
                {
                    if (!ExecutePreparedRhiFrame(execution))
                        throw ThreadedRhiSubmissionStageAbort {};
                },
                NLS::Base::TaskGraph::NamedThread::Render,
                NLS::Base::TaskGraph::TaskPriority::High,
                "RenderThread::DispatchRhiFrame");
            execution->completion = impl.taskGraph->GetCompletion(handle);
            result.scheduled = handle.IsValid() && execution->completion.IsValid();
            impl.taskGraph->WakeNamedThread(NLS::Base::TaskGraph::NamedThread::Render);
            return result;
        }
    }

    RhiSubmissionFrame Detail::SubmitThreadedRhiFrame(
        DriverImpl& impl,
        const RenderScenePackage& renderScenePackage,
        const size_t frameContextIndex)
    {
        NLS_PROFILE_SCOPE();
        auto execution = std::make_shared<ThreadedRhiSubmissionExecution>();
        execution->impl = &impl;
        execution->frameContextIndex = frameContextIndex;
        execution->attribution = RhiSubmissionAttribution::SynchronousDrain;
        execution->renderScenePackage = renderScenePackage;
        PrepareThreadedRhiSubmissionExecution(impl, execution);

        const auto scheduled = ScheduleThreadedRhiSubmission(impl, execution);
        if (scheduled.executedInline)
        {
            if (!scheduled.inlineSucceeded)
                AbortThreadedRhiSubmission(execution);
            else
            {
                WaitForRhiBatch(execution);
                CompletePreparedRhiFrame(execution);
            }
            return std::move(execution->submissionFrame);
        }
        if (!scheduled.scheduled)
        {
            AbortThreadedRhiSubmission(execution);
            return std::move(execution->submissionFrame);
        }

        if (execution->completion.Wait() !=
            NLS::Base::TaskGraph::TaskStatus::Succeeded)
        {
            AbortThreadedRhiSubmission(execution);
        }
        else
        {
            WaitForRhiBatch(execution);
            CompletePreparedRhiFrame(execution);
        }
        return std::move(execution->submissionFrame);
    }

    bool Detail::CompleteThreadedRhiSubmissionTelemetry(
        DriverImpl& impl,
        Render::RHI::RHIFrameContext& frameContext,
        const RenderScenePackage& renderScenePackage,
        const Render::RHI::ResourceStateTrackerStats& preResetTrackerStats,
        Render::RHI::DescriptorAllocatorStats* descriptorStats,
        const AsyncComputeSubmitPlan& submitPlan,
        RhiSubmissionFrame* submissionFrame,
        const std::shared_ptr<Detail::RhiFrameQueueOperationTelemetry>& queueTelemetry)
    {
        if (submissionFrame == nullptr)
            return false;

            submissionFrame->recordedVisibleWork =
            !HasCommandRecordingFailure(submissionFrame) &&
            submissionFrame->recordedPassCount > 0u &&
            submissionFrame->recordedDrawCount > 0u;
        submissionFrame->usedAsyncComputeQueueSubmission = submitPlan.usedDedicatedComputeQueueSubmission;
        submissionFrame->asyncComputeDisposition =
            submissionFrame->usedAsyncComputeQueueSubmission
                ? AsyncComputeDisposition::Submitted
                : ResolveThreadedAsyncComputeDisposition(impl, renderScenePackage);
        CopyDriverQueueTelemetryToSubmissionFrame(impl, *submissionFrame, queueTelemetry);

        if (impl.pipelineCache != nullptr)
        {
            const auto pipelineCacheStats = impl.pipelineCache->GetStats();
            submissionFrame->pipelineMainlineActive = true;
            submissionFrame->pipelineBypassCount = 0u;
            submissionFrame->pipelineCacheGraphicsHits = pipelineCacheStats.graphicsHits;
            submissionFrame->pipelineCacheGraphicsMisses = pipelineCacheStats.graphicsMisses;
            submissionFrame->pipelineCacheGraphicsStores = pipelineCacheStats.graphicsStores;
            submissionFrame->pipelineCacheGraphicsEntries = pipelineCacheStats.graphicsEntryCount;
            submissionFrame->pipelineCacheComputeHits = pipelineCacheStats.computeHits;
            submissionFrame->pipelineCacheComputeMisses = pipelineCacheStats.computeMisses;
            submissionFrame->pipelineCacheComputeStores = pipelineCacheStats.computeStores;
            submissionFrame->pipelineCacheComputeEntries = pipelineCacheStats.computeEntryCount;
        }
        else
        {
            submissionFrame->pipelineMainlineActive = false;
            submissionFrame->pipelineBypassCount = 1u;
        }

        if (frameContext.resourceStateTracker != nullptr)
        {
            const auto trackerStats = frameContext.resourceStateTracker->GetStats();
            submissionFrame->transientBufferRegistrationCount = trackerStats.transientBufferRegistrations;
            submissionFrame->transientTextureRegistrationCount = trackerStats.transientTextureRegistrations;
            submissionFrame->retiredTransientBufferCount = preResetTrackerStats.retiredTransientBuffers;
            submissionFrame->retiredTransientTextureCount = preResetTrackerStats.retiredTransientTextures;
        }

        if (frameContext.descriptorAllocator != nullptr)
        {
            Render::RHI::DescriptorAllocatorStats resolvedDescriptorStats =
                descriptorStats != nullptr
                    ? *descriptorStats
                    : Render::RHI::DescriptorAllocatorStats{};
            resolvedDescriptorStats = frameContext.descriptorAllocator->GetStats();
            if (descriptorStats != nullptr)
                *descriptorStats = resolvedDescriptorStats;
            submissionFrame->descriptorTransientUsed = resolvedDescriptorStats.transientUsed;
            submissionFrame->descriptorTransientPeak = resolvedDescriptorStats.transientPeak;
            submissionFrame->descriptorTransientRetired = resolvedDescriptorStats.transientRetired;
            submissionFrame->descriptorPersistentUsed = resolvedDescriptorStats.persistentUsed;
            submissionFrame->descriptorPersistentReleased = resolvedDescriptorStats.persistentReleased;
            submissionFrame->descriptorAllocationFailures = resolvedDescriptorStats.allocationFailures;
        }

        if (submissionFrame->recordedPassCount == 0u &&
            renderScenePackage.visibleDrawCount > 0u &&
            Render::Settings::GetThreadDiagnosticsSettings().logRenderDrawPath)
        {
            const auto message = std::string("[Driver] Frame ") + std::to_string(renderScenePackage.frameId)
                + " has " + std::to_string(renderScenePackage.visibleDrawCount) + " visible draws but recorded no work";
            NLS_LOG_WARNING(message.c_str());
        }

        const bool expectsOffscreenPass = !renderScenePackage.passCommandInputs.empty() ||
            !renderScenePackage.recordedDrawCommands.empty() || renderScenePackage.visibleDrawCount != 0u;
        return !(submissionFrame->recordedPassCount == 0u && submissionFrame->offscreenOnly &&
            expectsOffscreenPass);
    }

    void Detail::ReleaseUnsubmittedFrameResourceLease(
        DriverImpl& impl, const size_t slotIndex, const uint64_t leaseGeneration, const uint64_t latencyFrameId)
    {
        bool released = false;
        {
            std::lock_guard retirementLock(impl.frameResourceRetirementMutex);
            if (impl.frameResourceSlots != nullptr)
            {
                const auto* slot = impl.frameResourceSlots->PeekSlot(slotIndex);
                if (slot != nullptr && slot->inFlight && slot->leaseGeneration == leaseGeneration &&
                    slot->gpuFence == nullptr && !slot->quarantined)
                {
                    impl.frameResourceSlots->RetireSlot(slotIndex, nullptr);
                    released = true;
                }
            }
        }
        if (released && latencyFrameId != 0u && impl.frameLatency != nullptr)
            (void)impl.frameLatency->CancelFrame(latencyFrameId);
    }

    bool Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(
        DriverImpl& impl,
        Render::RHI::RHIFrameContext& frameContext,
        const size_t frameContextIndex)
    {
        std::lock_guard lock(impl.frameResourceRetirementMutex);
        if (impl.unsafeGpuWorkQuarantined.load(std::memory_order_acquire) ||
            impl.frameResourceSlots == nullptr)
            return false;

        const auto* slot = impl.frameResourceSlots->PeekSlot(frameContextIndex);
        if (slot == nullptr || slot->quarantined || !slot->retirementPending ||
            slot->gpuFence == nullptr || !slot->gpuFence->IsSignaled())
            return false;
        return impl.frameResourceSlots->ReclaimSlot(frameContextIndex,
            [&impl, &frameContext](FrameResourceSlot& slot)
            {
                const uint64_t completedFrameIndex = frameContext.frameIndex;
                if (frameContext.descriptorAllocator != nullptr)
                {
                    NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::ReleaseDeferredDescriptorAllocatorFrame");
                    frameContext.descriptorAllocator->EndFrame(completedFrameIndex);
                }
                if (frameContext.uploadContext != nullptr)
                {
                    NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::ReleaseDeferredUploadContextFrame");
                    frameContext.uploadContext->EndFrame(completedFrameIndex);
                }
                if (frameContext.resourceStateTracker != nullptr)
                {
                    NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::ReleaseDeferredTransientResources");
                    frameContext.resourceStateTracker->RetireTransientResources(std::numeric_limits<uint64_t>::max());
                }
                if (slot.uiTextureRetirementFrameId != 0u)
                    ReleaseRetiredUiTextureViewsForCompletedFrame(impl, slot.uiTextureRetirementFrameId);
            });
    }

    void Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(DriverImpl& impl)
    {
        for (size_t frameContextIndex = 0u; frameContextIndex < impl.GetFrameContextCount(); ++frameContextIndex)
        {
            ReleaseDeferredThreadedFrameScopedResourcesAfterFence(
                impl,
                impl.GetFrameContext(frameContextIndex),
                frameContextIndex);
        }
    }

    void Detail::LogCompletedThreadedRhiSubmission(
        DriverImpl& impl,
        Render::RHI::RHIFrameContext& frameContext,
        const RhiSubmissionFrame& submissionFrame)
    {
        LogThreadedFrameResourceDiagnostics(
            impl,
            frameContext,
            submissionFrame);
        if (submissionFrame.submittedSuccessfully &&
            !submissionFrame.deferredFrameScopedRetirement &&
            !submissionFrame.deviceLostDetected &&
            submissionFrame.currentFrameQueueOperationFailureCount == 0u)
        {
            ReleaseRetiredUiTextureViewsForCompletedFrame(
                impl,
                submissionFrame.uiOverlaySnapshotFrameId);
        }
    }

    namespace
    {
        /**
         * Serializes the claim/prepare/hand-off window of frame production.
         *
         * The lock is a local object owned by the calling thread for exactly one
         * production step. It is never stored back into the driver, so another thread
         * cannot clear this thread's ownership and two producers cannot overwrite each
         * other's lock. Frames still overlap because the gate is released as soon as
         * the executor owns the work: per-slot reuse is gated by the
         * FrameResourceRing's GPU fence inside BeginThreadedRhiFrame, not by this gate.
         *
         * A blocking acquisition keeps the previous dispatch contract for the render
         * thread; the synchronous drain and the swapchain resize use try_lock so they
         * can defer instead of stalling a producer.
         */
        class ScopedThreadedProductionGate final
        {
        public:
            ScopedThreadedProductionGate(DriverImpl& impl, const bool waitForAvailability)
                : m_lock(impl.threadedRhiSubmissionMutex, std::defer_lock)
            {
                if (waitForAvailability)
                {
                    m_lock.lock();
                    m_held = true;
                }
                else
                {
                    m_held = m_lock.try_lock();
                }
            }

            [[nodiscard]] bool IsHeld() const { return m_held; }

        private:
            std::unique_lock<std::timed_mutex> m_lock;
            bool m_held = false;
        };

        bool HasPendingMeshRuntimeUploads(const DriverImpl& impl)
        {
            return impl.meshRuntimeUploadRequestedCount.load(std::memory_order_acquire) >
                impl.meshRuntimeUploadRecordedCount.load(std::memory_order_acquire);
        }

        void EnqueuePendingThreadedSubmission(
            ThreadedRhiSubmissionExecutionPtr execution)
        {
            Detail::RhiFrameExecutionQueue::Enqueue(std::move(execution));
        }

        ThreadedRhiSubmissionExecutionPtr PeekPendingThreadedSubmission(DriverImpl& impl)
        {
            return Detail::RhiFrameExecutionQueue::Peek(impl);
        }

        void PopPendingThreadedSubmission(DriverImpl& impl)
        {
            Detail::RhiFrameExecutionQueue::Pop(impl);
        }

        size_t GetPendingThreadedSubmissionCount(const DriverImpl& impl)
        {
            return Detail::RhiFrameExecutionQueue::Count(impl);
        }

        /**
         * True once the execution's CPU stage and its executor completion are both
         * terminal. Retirement stays FIFO so readback and lifecycle hand-off keep
         * the executor's submission order.
         */
        bool IsThreadedSubmissionReadyToRetire(
            const ThreadedRhiSubmissionExecutionPtr& execution)
        {
            if (execution == nullptr)
                return true;
            if (execution->completion.IsValid() && !execution->completion.IsComplete())
                return false;
            if (execution->rhiDispatched && !execution->rhiCompletion.IsComplete())
                return false;
            return true;
        }

        /** Retires every ready execution from the head of the in-flight queue. */
        size_t RetireCompletedThreadedSubmissions(DriverImpl& impl)
        {
            std::lock_guard retirementLock(Detail::RhiFrameExecutionQueue::RetirementMutex());
            size_t retired = 0u;
            for (;;)
            {
                auto execution = PeekPendingThreadedSubmission(impl);
                if (execution == nullptr || !IsThreadedSubmissionReadyToRetire(execution))
                    break;

                const bool failedPreparation =
                    execution->completion.IsValid() &&
                    execution->completion.GetStatus() != NLS::Base::TaskGraph::TaskStatus::Succeeded;
                if (failedPreparation)
                {
                    // A cancelled dispatch still owns the frame context, so the
                    // abort path performs the slot hand-off before it is released.
                    AbortThreadedRhiSubmission(execution);
                }
                else
                {
                    CompletePreparedRhiFrame(execution);
                }
                (void)PopPendingThreadedSubmission(impl);
                ++retired;
            }

            if (retired != 0u && impl.taskGraph != nullptr)
                (void)impl.taskGraph->ReleaseTerminalTaskPayloads();
            return retired;
        }

        bool ScheduleClaimedRhiFrame(
            Driver& driver, DriverImpl& impl, const size_t slotIndex,
            RenderScenePackage renderScenePackage, const RhiSubmissionAttribution attribution)
        {
            auto execution = std::make_shared<ThreadedRhiSubmissionExecution>();
            execution->driver = &driver;
            execution->impl = &impl;
            execution->frameContextIndex = slotIndex;
            execution->attribution = attribution;
            execution->renderScenePackage = std::move(renderScenePackage);
            {
                std::lock_guard lock(impl.frameResourceRetirementMutex);
                if (impl.frameResourceSlots != nullptr)
                    if (auto* slot = impl.frameResourceSlots->PeekSlot(slotIndex); slot != nullptr && slot->inFlight)
                        slot->streamingDependencyPins = execution->renderScenePackage.streamingDependencyPins;
            }
            // Frames dispatched through the render lane acquire their swapchain
            // image asynchronously; the synchronous helper keeps the old path.
            execution->swapchainAcquisition = std::make_shared<ThreadedSwapchainAcquisition>();
            PrepareThreadedRhiSubmissionExecution(impl, execution);

            const auto scheduled = ScheduleThreadedRhiSubmission(impl, execution);
            if (scheduled.executedInline)
            {
                if (!scheduled.inlineSucceeded)
                {
                    AbortThreadedRhiSubmission(execution);
                    return true;
                }
                EnqueuePendingThreadedSubmission(std::move(execution));
                // Preserve the synchronous contract: a frame whose executor work
                // already completed retires now instead of waiting for a later tick.
                (void)RetireCompletedThreadedSubmissions(impl);
                return true;
            }
            if (!scheduled.scheduled)
            {
                AbortThreadedRhiSubmission(execution);
                return false;
            }

            EnqueuePendingThreadedSubmission(std::move(execution));
            (void)RetireCompletedThreadedSubmissions(impl);
            Detail::NotifyThreadedWorkers(impl);
            return true;
        }
    }

    size_t Detail::GetPendingCpuFrameCount(const DriverImpl& impl)
    {
        size_t count = static_cast<size_t>(impl.pendingRenderFrameCommands.load(std::memory_order_acquire));
        count += Detail::RhiFrameExecutionQueue::Count(impl);
        return count;
    }

    bool Detail::HasPendingSwapchainFrame(const DriverImpl& impl)
    {
        if (impl.pendingSwapchainRenderCommands.load(std::memory_order_acquire) != 0u)
            return true;
        {
            const auto frames = Detail::RhiFrameExecutionQueue::Copy(impl);
            if (std::any_of(frames.begin(), frames.end(),
                [](const auto& execution) { return execution->renderScenePackage.targetsSwapchain; }))
                return true;
        }
        return false;
    }

    std::vector<uint64_t> Detail::CollectPendingStreamingDependencyPins(const DriverImpl& impl)
    {
        std::vector<uint64_t> pins;
        {
            std::lock_guard lock(impl.frameResourceRetirementMutex);
            if (impl.frameResourceSlots != nullptr)
                for (size_t index = 0u; index < impl.frameResourceSlots->GetSlotCount(); ++index)
                {
                    const auto* slot = impl.frameResourceSlots->PeekSlot(index);
                    pins.insert(pins.end(), slot->streamingDependencyPins.begin(), slot->streamingDependencyPins.end());
                }
        }
        {
            for (const auto& execution : Detail::RhiFrameExecutionQueue::Copy(impl))
                pins.insert(pins.end(), execution->renderScenePackage.streamingDependencyPins.begin(),
                    execution->renderScenePackage.streamingDependencyPins.end());
        }
        std::sort(pins.begin(), pins.end());
        pins.erase(std::unique(pins.begin(), pins.end()), pins.end());
        return pins;
    }

    bool RhiCommandListExecutor::DispatchRenderFrame(Driver& driver, const size_t slotIndex,
        RenderScenePackage renderScenePackage)
    {
        DriverImpl& impl = *driver.m_impl;
        // Dispatch must finish inside the main-pipe command, so a following fence
        // captures this frame's sequence even while its translation is pending.
        // Earlier frames are deliberately left in flight: the in-flight queue and
        // the executor's ordered submit lane provide the ordering, not a full wait.
        ScopedThreadedProductionGate productionGate(impl, true);
        const bool scheduled = ScheduleClaimedRhiFrame(driver, impl, slotIndex,
            std::move(renderScenePackage),
            RenderingThreadSystem::IsInRenderingThread()
                ? RhiSubmissionAttribution::Worker : RhiSubmissionAttribution::SynchronousDrain);
        return scheduled;
    }

    bool RhiCommandListExecutor::TryScheduleIdleMeshRuntimeUploads(Driver& driver)
    {
        NLS_PROFILE_SCOPE();
        DriverImpl& impl = *driver.m_impl;
        if (impl.frameResourceSlots == nullptr ||
            impl.explicitDevice == nullptr ||
            impl.deviceLostDetected.load(std::memory_order_acquire) ||
            impl.unsafeGpuWorkQuarantined.load(std::memory_order_acquire) ||
            impl.taskGraph == nullptr ||
            !impl.taskGraph->IsStarted())
        {
            return false;
        }
        if (GetPendingThreadedSubmissionCount(impl) != 0u ||
            impl.pendingIdleMeshUploadCompletion.IsValid())
            return false;
        if (!HasPendingMeshRuntimeUploads(impl))
            return false;

        const auto uploadLane = impl.rhiThreadMode == Render::Settings::RhiThreadMode::DedicatedThread
            ? NLS::Base::TaskGraph::NamedThread::Rhi
            : (impl.rhiThreadMode == Render::Settings::RhiThreadMode::Tasks
                ? NLS::Base::TaskGraph::NamedThread::Any
                : NLS::Base::TaskGraph::NamedThread::Render);
        DriverImpl* const implPtr = &impl;
        // The idle upload only moves parsed CPU payloads between the driver's own
        // queues; it never touches the device or a frame context, so it needs no
        // production gate and must not serialize frame preparation.
        const auto handle = impl.taskGraph->Submit(
            [implPtr]
            {
                RenderingThreadSystem::ScopedRhiLaneScope laneScope;
                NLS_PROFILE_NAMED_SCOPE("RhiCommandListExecutor::RecordIdleMeshRuntimeUploads");
                (void)Detail::RecordPendingMeshRuntimeUploads(*implPtr, false);
            },
            uploadLane,
            NLS::Base::TaskGraph::TaskPriority::High,
            "RhiCommandListExecutor::RecordIdleMeshRuntimeUploads");
        if (!handle.IsValid())
            return false;

        impl.pendingIdleMeshUploadTask = handle;
        impl.pendingIdleMeshUploadCompletion = impl.taskGraph->GetCompletion(handle);
        if (uploadLane != NLS::Base::TaskGraph::NamedThread::Any)
            impl.taskGraph->WakeNamedThread(uploadLane);
        Detail::NotifyThreadedWorkers(impl);
        return true;
    }

    bool RhiCommandListExecutor::FinalizeThreadedSubmissionProduction(
        Driver& driver,
        const bool applyPendingSwapchainResize)
    {
        DriverImpl& impl = *driver.m_impl;

        // Retire every frame whose CPU stage and executor completion are terminal.
        // A frame that is still translating or submitting stays queued, so the next
        // render command can prepare without waiting for this one.
        const size_t retiredSubmissions = RetireCompletedThreadedSubmissions(impl);

        bool idleUploadFinished = false;
        if (impl.pendingIdleMeshUploadCompletion.IsValid() &&
            impl.pendingIdleMeshUploadCompletion.IsComplete())
        {
            impl.pendingIdleMeshUploadTask = {};
            impl.pendingIdleMeshUploadCompletion = {};
            if (impl.taskGraph != nullptr)
                (void)impl.taskGraph->ReleaseTerminalTaskPayloads();
            idleUploadFinished = true;
        }

        // Match the pre-overlap contract: the tick only applies a pending resize
        // when it observed a producer hand-off, instead of retrying every idle tick.
        if (applyPendingSwapchainResize && (retiredSubmissions != 0u || idleUploadFinished))
            driver.ApplyPendingSwapchainResize();
        return retiredSubmissions != 0u || idleUploadFinished;
    }

    void RhiCommandListExecutor::DrainProducerOwnedThreadedSubmission(Driver& driver)
    {
        DriverImpl& impl = *driver.m_impl;

        const auto waitForCompletion = [&impl](NLS::Base::TaskGraph::TaskCompletion completion)
        {
            while (completion.IsValid() && !completion.IsComplete())
            {
                if (impl.taskGraph != nullptr)
                {
                    (void)impl.taskGraph->PumpCurrentThread();
                    (void)impl.taskGraph->Pump(NLS::Base::TaskGraph::NamedThread::Rhi);
                }
                if (completion.IsComplete())
                    break;
                (void)completion.WaitFor(std::chrono::milliseconds(1));
            }
        };

        // Drain in dispatch order. A later frame must never retire before an
        // earlier one, because readback and slot hand-off follow submission order.
        for (;;)
        {
            auto execution = PeekPendingThreadedSubmission(impl);
            if (execution == nullptr)
                break;

            waitForCompletion(execution->completion);
            WaitForRhiBatch(execution);
            if (RetireCompletedThreadedSubmissions(impl) == 0u)
            {
                // The head did not reach a terminal state; leaving it queued is
                // safer than spinning here without a progress guarantee.
                break;
            }
        }

        if (impl.pendingIdleMeshUploadCompletion.IsValid() &&
            !impl.pendingIdleMeshUploadCompletion.IsComplete())
            waitForCompletion(impl.pendingIdleMeshUploadCompletion);

        (void)RetireCompletedThreadedSubmissions(impl);
        if (impl.pendingIdleMeshUploadCompletion.IsValid() &&
            impl.pendingIdleMeshUploadCompletion.IsComplete())
        {
            impl.pendingIdleMeshUploadTask = {};
            impl.pendingIdleMeshUploadCompletion = {};
        }
    }


    void RhiCommandListExecutor::ReadPixels(
        const Driver& driver,
        const uint32_t x,
        const uint32_t y,
        const uint32_t width,
        const uint32_t height,
        const Settings::EPixelDataFormat format,
        const Settings::EPixelDataType type,
        void* data)
    {
        NLS_PROFILE_SCOPE();
        const auto result = ReadPixelsChecked(driver, x, y, width, height, format, type, data);
        if (!result.Succeeded())
            NLS_LOG_WARNING("RhiCommandListExecutor::ReadPixels failed: " + result.message);
    }

    void RhiCommandListExecutor::ReadPixels(
        const Driver& driver,
        const std::shared_ptr<Render::RHI::RHITexture>& texture,
        const uint32_t x,
        const uint32_t y,
        const uint32_t width,
        const uint32_t height,
        const Settings::EPixelDataFormat format,
        const Settings::EPixelDataType type,
        void* data)
    {
        NLS_PROFILE_SCOPE();
        const auto result = ReadPixelsChecked(driver, texture, x, y, width, height, format, type, data);
        if (!result.Succeeded())
            NLS_LOG_WARNING("RhiCommandListExecutor::ReadPixels failed: " + result.message);
    }

    Render::RHI::RHIReadbackResult RhiCommandListExecutor::ReadPixelsChecked(
        const Driver& driver,
        const uint32_t x,
        const uint32_t y,
        const uint32_t width,
        const uint32_t height,
        const Settings::EPixelDataFormat format,
        const Settings::EPixelDataType type,
        void* data)
    {
        NLS_PROFILE_SCOPE();
        const auto result = BeginReadPixels(driver, x, y, width, height, format, type, data);
        if (!result.Succeeded() || result.completion == nullptr)
            return result;

        auto completedResult = result;
        NLS::Base::Profiling::PerformanceStageScope waitScope(
            NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
            "WaitPreviewFence",
            NLS::Base::Profiling::PerformanceStageThread::Main);
        const auto completionStatus = result.completion->Wait();
        completedResult.message = completionStatus.message;
        if (completionStatus.Succeeded())
        {
            completedResult.code = Render::RHI::RHIReadbackStatusCode::Success;
        }
        else if (completionStatus.code == Render::RHI::RHICompletionStatusCode::DeviceLost)
        {
            completedResult.code = Render::RHI::RHIReadbackStatusCode::DeviceLost;
            MarkReadbackDeviceLostIfNeeded(
                *driver.m_impl,
                completedResult,
                "RhiCommandListExecutor::ReadPixelsChecked");
        }
        else
        {
            completedResult.code = Render::RHI::RHIReadbackStatusCode::BackendFailure;
        }
        return completedResult;
    }

    Render::RHI::RHIReadbackResult RhiCommandListExecutor::ReadPixelsChecked(
        const Driver& driver,
        const std::shared_ptr<Render::RHI::RHITexture>& texture,
        const uint32_t x,
        const uint32_t y,
        const uint32_t width,
        const uint32_t height,
        const Settings::EPixelDataFormat format,
        const Settings::EPixelDataType type,
        void* data)
    {
        NLS_PROFILE_SCOPE();
        const auto result = BeginReadPixels(driver, texture, x, y, width, height, format, type, data);
        if (!result.Succeeded() || result.completion == nullptr)
            return result;

        auto completedResult = result;
        NLS::Base::Profiling::PerformanceStageScope waitScope(
            NLS::Base::Profiling::PerformanceStageDomain::Thumbnail,
            "WaitPreviewFence",
            NLS::Base::Profiling::PerformanceStageThread::Main);
        const auto completionStatus = result.completion->Wait();
        completedResult.message = completionStatus.message;
        if (completionStatus.Succeeded())
        {
            completedResult.code = Render::RHI::RHIReadbackStatusCode::Success;
        }
        else if (completionStatus.code == Render::RHI::RHICompletionStatusCode::DeviceLost)
        {
            completedResult.code = Render::RHI::RHIReadbackStatusCode::DeviceLost;
            MarkReadbackDeviceLostIfNeeded(
                *driver.m_impl,
                completedResult,
                "RhiCommandListExecutor::ReadPixelsChecked");
        }
        else
        {
            completedResult.code = Render::RHI::RHIReadbackStatusCode::BackendFailure;
        }
        return completedResult;
    }

    Render::RHI::RHIReadbackResult RhiCommandListExecutor::BeginReadPixels(
        const Driver& driver,
        const uint32_t x,
        const uint32_t y,
        const uint32_t width,
        const uint32_t height,
        const Settings::EPixelDataFormat format,
        const Settings::EPixelDataType type,
        void* data)
    {
        NLS_PROFILE_SCOPE();
        if (driver.m_impl == nullptr || driver.m_impl->explicitDevice == nullptr)
            return { Render::RHI::RHIReadbackStatusCode::BackendFailure, "explicit device is unavailable" };
        if (driver.m_impl->deviceLostDetected.load(std::memory_order_acquire) ||
            driver.m_impl->unsafeGpuWorkQuarantined.load(std::memory_order_acquire))
        {
            return {
                Render::RHI::RHIReadbackStatusCode::BackendFailure,
                "explicit readback rejected because RHI device is lost or GPU work is quarantined"
            };
        }

        return RunRenderOrderedReadback(*driver.m_impl, [&driver, x, y, width, height, format, type, data]
        {
            const auto texture = DriverRendererAccess::ResolveReadbackTexture(driver);
            if (texture == nullptr)
                return RHI::RHIReadbackResult {
                    RHI::RHIReadbackStatusCode::InvalidArgument,
                    "no active explicit readback source is available"
                };
            return BeginReadPixels(driver, texture, x, y, width, height, format, type, data);
        });
    }

    Render::RHI::RHIReadbackResult RhiCommandListExecutor::BeginReadPixels(
        const Driver& driver,
        const std::shared_ptr<Render::RHI::RHITexture>& texture,
        const uint32_t x,
        const uint32_t y,
        const uint32_t width,
        const uint32_t height,
        const Settings::EPixelDataFormat format,
        const Settings::EPixelDataType type,
        void* data)
    {
        NLS_PROFILE_SCOPE();
        if (driver.m_impl == nullptr || driver.m_impl->explicitDevice == nullptr)
            return { Render::RHI::RHIReadbackStatusCode::BackendFailure, "explicit device is unavailable" };
        if (driver.m_impl->deviceLostDetected.load(std::memory_order_acquire) ||
            driver.m_impl->unsafeGpuWorkQuarantined.load(std::memory_order_acquire))
        {
            return {
                Render::RHI::RHIReadbackStatusCode::BackendFailure,
                "explicit readback rejected because RHI device is lost or GPU work is quarantined"
            };
        }

        if (texture == nullptr)
            return { Render::RHI::RHIReadbackStatusCode::InvalidArgument, "explicit readback source is unavailable" };

        if (!RenderingThreadSystem::IsInRenderingThread() && !RenderingThreadSystem::IsInRhiThread())
            return RunRenderOrderedReadback(*driver.m_impl,
                [&driver, texture, x, y, width, height, format, type, data]
                { return BeginReadPixels(driver, texture, x, y, width, height, format, type, data); });

        Render::RHI::RHIReadbackResult result {
            Render::RHI::RHIReadbackStatusCode::BackendFailure,
            "texture readback was not dispatched"
        };
        const auto device = driver.m_impl->explicitDevice;
        const auto laneResult = RhiCommandListExecutor::Get().RunOnSubmitLane(
            "RhiCommandListExecutor::BeginReadPixels",
            [device, texture, x, y, width, height, format, type, data, &result]
            {
                result = device->BeginReadPixels(texture, x, y, width, height, format, type, data);
                // Format/input errors terminate only this request. Device loss
                // also cancels every later submission in the executor.
                if (IsDeviceLostReadbackFailure(result))
                    return Render::RHI::RHIQueueOperationResult {
                        Render::RHI::RHIQueueOperationStatusCode::DeviceLost, result.message
                    };
                return Render::RHI::RHIQueueOperationResult {};
            });
        if (!laneResult.Succeeded())
        {
            if (!IsDeviceLostReadbackFailure(result))
                result = { Render::RHI::RHIReadbackStatusCode::BackendFailure, laneResult.message };
            if (laneResult.mayHaveQueuedGpuWork && !laneResult.frameFenceSignalQueued)
            {
                {
                    std::lock_guard lock(driver.m_impl->frameResourceRetirementMutex);
                    driver.m_impl->unfencedReadbackResources.push_back(texture);
                }
                MarkQueuedGpuWorkWithoutRetirementFence(
                    *driver.m_impl, "RhiCommandListExecutor::BeginReadPixels", laneResult);
            }
        }
        MarkReadbackDeviceLostIfNeeded(
            *driver.m_impl,
            result,
            "RhiCommandListExecutor::BeginReadPixels");
        return result;
    }

    Render::RHI::RHIReadbackResult RhiCommandListExecutor::PollReadbackCompletion(
        const Driver& driver,
        const Render::RHI::RHIReadbackResult& readback)
    {
        NLS_PROFILE_SCOPE();
        if (driver.m_impl == nullptr)
            return {
                Render::RHI::RHIReadbackStatusCode::BackendFailure,
                "driver implementation is unavailable"
            };
        if (!readback.Succeeded() || readback.completion == nullptr)
            return readback;

        auto completedResult = readback;
        const auto completionStatus = readback.completion->Poll();
        completedResult.message = completionStatus.message;
        if (completionStatus.code == Render::RHI::RHICompletionStatusCode::Pending)
            return completedResult;

        completedResult.completion = nullptr;
        if (completionStatus.Succeeded())
        {
            completedResult.code = Render::RHI::RHIReadbackStatusCode::Success;
        }
        else if (completionStatus.code == Render::RHI::RHICompletionStatusCode::DeviceLost)
        {
            completedResult.code = Render::RHI::RHIReadbackStatusCode::DeviceLost;
            MarkReadbackDeviceLostIfNeeded(
                *driver.m_impl,
                completedResult,
                "RhiCommandListExecutor::PollReadbackCompletion");
        }
        else
        {
            completedResult.code = Render::RHI::RHIReadbackStatusCode::BackendFailure;
        }
        return completedResult;
    }


    void RhiCommandListExecutor::PresentSwapchain(Driver& driver)
    {
        NLS_PROFILE_SCOPE();
        auto& impl = *driver.m_impl;

        if (impl.explicitDevice != nullptr && impl.explicitSwapchain != nullptr)
        {
            const auto overlayFeature =
                Render::RHI::GetUIOverlayFrameGraphFeature(impl.explicitDevice.get());
            if (overlayFeature.supported && impl.frameResourceSlots != nullptr)
            {
                const auto& swapchainDesc = impl.explicitSwapchain->GetDesc();
                // A queued scene command still consumes the pending UI snapshot.
                // Earlier captured UI frames use the ordered lane independently.
                if (impl.pendingUiSnapshotConsumerCommands.load(std::memory_order_acquire) == 0u)
                {
                    NLS_PROFILE_NAMED_SCOPE("RhiCommandListExecutor::TryPublishUiOnlyFrameNonBlocking");
                    (void)DriverUIAccess::PublishUiOnlyFrame(
                        driver,
                        swapchainDesc.width,
                        swapchainDesc.height,
                        nullptr,
                        nullptr,
                        false);
                }

                driver.ApplyPendingSwapchainResize();
                return;
            }
        }

        {
            NLS_PROFILE_NAMED_SCOPE("RhiCommandListExecutor::ApplyPendingSwapchainResize");
            driver.ApplyPendingSwapchainResize();
        }
    }
}

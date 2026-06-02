#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <Debug/Assertion.h>
#include <Debug/Logger.h>

#include "Profiling/Profiler.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverInternal.h"
#include "Rendering/Context/RenderThreadCoordinator.h"
#include "Rendering/Context/RhiThreadCoordinator.h"
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
#include "Jobs/JobSystem.h"

namespace NLS::Render::Context
{
    namespace
    {
        constexpr uint64_t kFrameFenceWaitTimeoutNanoseconds = 5'000'000'000ull;
        constexpr auto kUiStandaloneFrameSubmissionLockWait = std::chrono::milliseconds(8);
        constexpr auto kUiStandaloneFramePendingLease = std::chrono::milliseconds(64);
        constexpr uint32_t kMinimumInRenderPassChildSlicesForParallelWorkers = 4u;

        Render::RHI::RHIFrameContext* GetCurrentFrameContext(DriverImpl& impl)
        {
            return !impl.frameContexts.empty()
                ? &impl.frameContexts[impl.currentFrameIndex % impl.frameContexts.size()]
                : nullptr;
        }

        void RememberCompletedReadbackTexture(
            DriverImpl& impl,
            const std::shared_ptr<Render::RHI::RHITexture>& texture)
        {
            std::lock_guard lock(impl.completedReadbackTextureMutex);
            impl.completedReadbackTexture = texture;
            if (texture == nullptr)
                return;

            impl.completedReadbackTextureHistory.erase(
                std::remove_if(
                    impl.completedReadbackTextureHistory.begin(),
                    impl.completedReadbackTextureHistory.end(),
                    [&texture](const std::weak_ptr<Render::RHI::RHITexture>& storedTexture)
                    {
                        const auto lockedTexture = storedTexture.lock();
                        return lockedTexture == nullptr || lockedTexture == texture;
                    }),
                impl.completedReadbackTextureHistory.end());

            impl.completedReadbackTextureHistory.push_back(texture);
            constexpr size_t kMaxCompletedReadbackHistory = 8u;
            while (impl.completedReadbackTextureHistory.size() > kMaxCompletedReadbackHistory)
                impl.completedReadbackTextureHistory.erase(impl.completedReadbackTextureHistory.begin());
        }

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
            return lhs.colorFormats == rhs.colorFormats &&
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
            for (const auto& colorView : passInput.colorAttachmentViews)
            {
                if (colorView == nullptr)
                    return std::nullopt;

                layout.colorFormats.push_back(colorView->GetDesc().format);
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

        Render::RHI::RHISubmitDesc BuildStandaloneGraphicsSubmitDesc(
            const Render::RHI::RHIFrameContext& frameContext,
            const bool signalRenderFinished,
            const bool signalFrameFence)
        {
            Render::RHI::RHISubmitDesc submitDesc;
            if (frameContext.commandBuffer != nullptr)
                submitDesc.commandBuffers.push_back(frameContext.commandBuffer);
            if (frameContext.hasAcquiredSwapchainImage && frameContext.imageAcquiredSemaphore != nullptr)
                submitDesc.waitSemaphores.push_back(frameContext.imageAcquiredSemaphore);
            if (signalRenderFinished && frameContext.renderFinishedSemaphore != nullptr)
                submitDesc.signalSemaphores.push_back(frameContext.renderFinishedSemaphore);
            if (signalFrameFence)
                submitDesc.signalFence = frameContext.frameFence;
            return submitDesc;
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
            const Render::RHI::RHIQueueOperationResult& submitResult);

        void RetainStandaloneSubmitResourcesUntilNextFence(
            DriverImpl& impl,
            size_t frameContextIndex,
            const Render::RHI::RHIFrameContext& frameContext,
            const Render::RHI::RHISubmitDesc& submitDesc);

        void DeferStandaloneFrameScopedRetirementAfterSubmit(
            DriverImpl& impl,
            size_t frameContextIndex,
            const Render::RHI::RHIFrameContext& frameContext,
            const Render::RHI::RHISubmitDesc& submitDesc);

        Render::RHI::RHIPresentDesc BuildSwapchainPresentDesc(
            const DriverImpl& impl,
            const Render::RHI::RHIFrameContext& frameContext,
            const bool includeUiCompositionSignal,
            const bool signalFrameFence)
        {
            Render::RHI::RHIPresentDesc presentDesc;
            presentDesc.swapchain = impl.explicitSwapchain;
            presentDesc.imageIndex = frameContext.swapchainImageIndex;
            if (frameContext.renderFinishedSemaphore != nullptr)
                presentDesc.waitSemaphores.push_back(frameContext.renderFinishedSemaphore);
            if (signalFrameFence)
                presentDesc.signalFence = frameContext.frameFence;

            if (includeUiCompositionSignal && impl.uiRenderFinishedSemaphore.IsValid())
            {
                if (impl.uiRenderFinishedValue != 0u)
                {
                    presentDesc.uiSignalSemaphore = impl.uiRenderFinishedSemaphore;
                    presentDesc.uiSignalValue = impl.uiRenderFinishedValue;
                }
            }
            return presentDesc;
        }

        void ClearUICompositionSignal(DriverImpl& impl)
        {
            impl.uiRenderFinishedSemaphore = {};
            impl.uiRenderFinishedValue = 0u;
        }

        void PublishSceneToUiWaitSemaphore(DriverImpl& impl, const Render::RHI::RHIFrameContext& frameContext)
        {
            std::lock_guard lock(impl.sceneToUiWaitMutex);
            impl.sceneToUiWaitSemaphore = frameContext.renderFinishedSemaphore != nullptr
                ? frameContext.renderFinishedSemaphore->GetNativeSemaphoreHandle()
                : Render::RHI::NativeHandle{};
        }

        void ClearSceneToUiWaitSemaphore(DriverImpl& impl)
        {
            std::lock_guard lock(impl.sceneToUiWaitMutex);
            impl.sceneToUiWaitSemaphore = {};
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

        void ResetCurrentFrameQueueOperationTelemetry(DriverImpl& impl)
        {
            std::lock_guard lock(impl.driverTelemetryMutex);
            impl.currentFrameQueueOperationFailureCount = 0u;
            impl.currentFrameLastQueueOperationFailure.clear();
        }

        void CopyDriverQueueTelemetryToSubmissionFrame(
            DriverImpl& impl,
            RhiSubmissionFrame& submissionFrame)
        {
            std::lock_guard lock(impl.driverTelemetryMutex);
            submissionFrame.queueOperationFailureCount = impl.queueOperationFailureCount;
            submissionFrame.lastQueueOperationFailure = impl.lastQueueOperationFailure;
            submissionFrame.currentFrameQueueOperationFailureCount =
                impl.currentFrameQueueOperationFailureCount;
            submissionFrame.currentFrameLastQueueOperationFailure =
                impl.currentFrameLastQueueOperationFailure;
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
            const std::string& context)
        {
            if (frameFence == nullptr)
                return false;

            {
                NLS_PROFILE_NAMED_SCOPE("RhiThreadCoordinator::CheckFrameFenceSignaled");
                if (frameFence->IsSignaled())
                    return true;
            }

            NLS_PROFILE_NAMED_SCOPE("RhiThreadCoordinator::WaitFrameFenceWithDriverPolicy");
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
                }
                NLS_LOG_ERROR(message);
            }
            return waited;
        }

        bool RecordQueueOperationResult(
            DriverImpl& impl,
            const Render::RHI::RHIQueueOperationResult& result,
            const std::string& context)
        {
            if (result.Succeeded())
                return true;

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
                if (result.code == Render::RHI::RHIQueueOperationStatusCode::DeviceLost)
                {
                    impl.deviceLostDetected.store(true, std::memory_order_release);
                    impl.deviceLostReason = message;
                }
            }

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
            const std::string& context)
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
                impl.deviceLostDetected.store(true, std::memory_order_release);
                impl.deviceLostReason = message;
            }
            NLS_LOG_ERROR(message);
        }

        Render::RHI::RHIQueueOperationResult PresentSwapchainFrame(
            DriverImpl& impl,
            Render::RHI::RHIQueue& queue,
            const Render::RHI::RHIFrameContext& frameContext,
            const bool includeUiCompositionSignal,
            const bool signalFrameFence)
        {
            NLS_PROFILE_SCOPE();
            if (impl.renderDocCaptureController != nullptr)
                impl.renderDocCaptureController->OnPrePresent();

            const auto presentDesc = BuildSwapchainPresentDesc(
                impl,
                frameContext,
                includeUiCompositionSignal,
                signalFrameFence);
            const auto presentResult = queue.PresentChecked(presentDesc);

            if (impl.renderDocCaptureController != nullptr)
                impl.renderDocCaptureController->OnPostPresent();
            return presentResult;
        }

        bool FinalizeStandaloneUiFrame(DriverImpl& impl)
        {
            NLS_PROFILE_SCOPE();
            auto* frameContext = GetCurrentFrameContext(impl);
            if (frameContext == nullptr)
                return false;

            if (frameContext->commandBuffer != nullptr && frameContext->commandBuffer->IsRecording())
                frameContext->commandBuffer->End();

            const auto submitDesc = BuildStandaloneGraphicsSubmitDesc(*frameContext, true, false);

            auto queue = impl.explicitDevice != nullptr
                ? impl.explicitDevice->GetQueue(Render::RHI::QueueType::Graphics)
                : nullptr;
            bool submitted = false;
            bool submitAttempted = false;
            Render::RHI::RHIQueueOperationResult submitResult;
            if (queue != nullptr)
            {
                submitAttempted = true;
                {
                    NLS_PROFILE_NAMED_SCOPE("StandaloneUiFrame::QueueSubmitChecked");
                    submitResult = queue->SubmitChecked(submitDesc);
                    submitted = RecordQueueOperationResult(
                        impl,
                        submitResult,
                        "RhiThreadCoordinator::FinalizeStandaloneUiFrame::Submit");
                    ClearSceneToUiWaitSemaphoreForFrame(impl, *frameContext);
                }
                if (submitted)
                {
                    NLS_PROFILE_NAMED_SCOPE("StandaloneUiFrame::PresentSwapchainFrame");
                    const auto presentResult = PresentSwapchainFrame(impl, *queue, *frameContext, true, true);
                    RecordQueueOperationResult(
                        impl,
                        presentResult,
                        "RhiThreadCoordinator::FinalizeStandaloneUiFrame::Present");
                    submitResult.mayHaveQueuedGpuWork =
                        submitResult.mayHaveQueuedGpuWork ||
                        presentResult.Succeeded() ||
                        presentResult.mayHaveQueuedGpuWork;
                    submitResult.frameFenceSignalQueued =
                        submitResult.frameFenceSignalQueued ||
                        presentResult.frameFenceSignalQueued;
                    if (presentResult.code == Render::RHI::RHIQueueOperationStatusCode::DeviceLost)
                        submitResult.code = Render::RHI::RHIQueueOperationStatusCode::DeviceLost;
                }
            }
            else
            {
                ClearSceneToUiWaitSemaphoreForFrame(impl, *frameContext);
            }

            ClearUICompositionSignal(impl);

            const auto frameContextIndex = impl.frameContexts.empty()
                ? 0u
                : static_cast<size_t>(impl.currentFrameIndex % impl.frameContexts.size());
            const bool deferFrameScopedRetirement =
                ShouldDeferFrameScopedRetirementAfterSubmit(submitResult, submitAttempted);
            bool quarantinedFrameScopedResources = false;
            if (deferFrameScopedRetirement)
            {
                DeferStandaloneFrameScopedRetirementAfterSubmit(
                    impl,
                    frameContextIndex,
                    *frameContext,
                    submitDesc);
            }
            else if (SubmitMayHaveQueuedGpuWorkWithoutRetirementFence(submitResult, submitAttempted))
            {
                RetainStandaloneSubmitResourcesUntilNextFence(
                    impl,
                    frameContextIndex,
                    *frameContext,
                    submitDesc);
                MarkQueuedGpuWorkWithoutRetirementFence(
                    impl,
                    "RhiThreadCoordinator::FinalizeStandaloneUiFrame::Submit",
                    submitResult);
                quarantinedFrameScopedResources = true;
            }

            if (!deferFrameScopedRetirement && !quarantinedFrameScopedResources && frameContext->descriptorAllocator != nullptr)
                frameContext->descriptorAllocator->EndFrame(impl.currentFrameIndex);
            if (!deferFrameScopedRetirement && !quarantinedFrameScopedResources && frameContext->uploadContext != nullptr)
                frameContext->uploadContext->EndFrame(impl.currentFrameIndex);

            RememberCompletedReadbackTexture(impl, frameContext->explicitReadbackTexture);
            impl.explicitFrameActive = false;
            impl.uiStandaloneFrameActive = false;
            ++impl.currentFrameIndex;
            if (impl.uiStandaloneFrameSubmissionLock.owns_lock())
                impl.uiStandaloneFrameSubmissionLock.unlock();

            return submitted;
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
                frameContext.descriptorAllocator->EndFrame(impl.currentFrameIndex);
            }
            if (!deferFrameScopedRetirement && frameContext.uploadContext != nullptr)
            {
                NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::EndUploadContextFrame");
                frameContext.uploadContext->EndFrame(impl.currentFrameIndex);
            }

            {
                NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::RetireFrameResources");
                submitPlan.batches.clear();
                submitPlan.temporarySemaphores.clear();
                RememberCompletedReadbackTexture(impl, frameContext.explicitReadbackTexture);
                ++impl.currentFrameIndex;
            }
        }

        void RetainThreadedSubmitPlanResourcesUntilNextFence(
            DriverImpl& impl,
            const size_t frameContextIndex,
            Detail::AsyncComputeSubmitPlan& submitPlan)
        {
            if (impl.retainedThreadedSubmitResourceKeepAliveByFrameContext.size() < impl.frameContexts.size())
                impl.retainedThreadedSubmitResourceKeepAliveByFrameContext.resize(impl.frameContexts.size());
            if (frameContextIndex >= impl.retainedThreadedSubmitResourceKeepAliveByFrameContext.size())
                return;

            auto& keepAlive = impl.retainedThreadedSubmitResourceKeepAliveByFrameContext[frameContextIndex];
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
            const Render::RHI::RHIQueueOperationResult& submitResult)
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
                }
            }
            NLS_LOG_ERROR(message);
        }

        void RetainStandaloneSubmitResourcesUntilNextFence(
            DriverImpl& impl,
            const size_t frameContextIndex,
            const Render::RHI::RHIFrameContext& frameContext,
            const Render::RHI::RHISubmitDesc& submitDesc)
        {
            if (impl.retainedThreadedSubmitResourceKeepAliveByFrameContext.size() < impl.frameContexts.size())
                impl.retainedThreadedSubmitResourceKeepAliveByFrameContext.resize(impl.frameContexts.size());
            if (frameContextIndex >= impl.retainedThreadedSubmitResourceKeepAliveByFrameContext.size())
                return;

            auto& keepAlive = impl.retainedThreadedSubmitResourceKeepAliveByFrameContext[frameContextIndex];
            keepAlive.push_back(frameContext.commandPool);
            for (const auto& commandBuffer : submitDesc.commandBuffers)
                keepAlive.push_back(commandBuffer);
            for (const auto& semaphore : submitDesc.waitSemaphores)
                keepAlive.push_back(semaphore);
            for (const auto& semaphore : submitDesc.signalSemaphores)
                keepAlive.push_back(semaphore);
        }

        void DeferStandaloneFrameScopedRetirementAfterSubmit(
            DriverImpl& impl,
            const size_t frameContextIndex,
            const Render::RHI::RHIFrameContext& frameContext,
            const Render::RHI::RHISubmitDesc& submitDesc)
        {
            RetainStandaloneSubmitResourcesUntilNextFence(
                impl,
                frameContextIndex,
                frameContext,
                submitDesc);
            impl.deferredThreadedFrameScopedRetirementFrameContexts.insert(frameContextIndex);
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
            transition.before = Render::RHI::ResourceState::Unknown;
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

        bool UsesImplicitDependencySourceIndex(const ParallelCommandWorkUnit& workUnit)
        {
            return workUnit.incomingDependencyEdges.empty() &&
                workUnit.commandInput.dependencySourceWorkUnitIndex.has_value();
        }

        bool InFlightSlotTargetsSwapchain(const InFlightFrameSlot& slot)
        {
            if (slot.stage == ThreadedFrameStage::Available ||
                slot.stage == ThreadedFrameStage::Retired)
            {
                return false;
            }

            if (slot.renderFrameInput.has_value())
                return slot.renderFrameInput->targetsSwapchain;
            if (slot.renderFrameBuild.has_value())
                return slot.renderFrameBuild->targetsSwapchain;
            if (slot.renderScenePackage.has_value())
                return slot.renderScenePackage->targetsSwapchain;
            if (slot.snapshot.has_value())
                return slot.snapshot->targetsSwapchain;

            return false;
        }

        bool HasInFlightThreadedSwapchainFrame(const DriverImpl& impl)
        {
            if (impl.threadedLifecycle == nullptr)
                return false;

            const auto slots = impl.threadedLifecycle->CopySlots();
            return std::any_of(
                slots.begin(),
                slots.end(),
                [](const InFlightFrameSlot& slot)
                {
                    return InFlightSlotTargetsSwapchain(slot);
                });
        }

        const char* ToThreadedPassDebugName(const RenderPassCommandKind kind)
        {
            switch (kind)
            {
            case RenderPassCommandKind::Opaque:
                return "ThreadedOpaquePass";
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

        bool CanBeginStandaloneExplicitFrameForImpl(const DriverImpl& impl)
        {
            return impl.threadedLifecycle == nullptr &&
                !impl.deviceLostDetected.load(std::memory_order_acquire) &&
                !impl.unsafeGpuWorkQuarantined.load(std::memory_order_acquire);
        }

        bool CanBeginStandaloneUiExplicitFrameForImpl(const DriverImpl& impl)
        {
            return !impl.deviceLostDetected.load(std::memory_order_acquire) &&
                !impl.unsafeGpuWorkQuarantined.load(std::memory_order_acquire) &&
                !HasInFlightThreadedSwapchainFrame(impl);
        }

        uint64_t GetSteadyClockTickNs()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        void RefreshUiStandaloneFramePendingLease(DriverImpl& impl)
        {
            const auto expiresAt = std::chrono::steady_clock::now() + kUiStandaloneFramePendingLease;
            impl.uiStandaloneFramePendingUntilTickNs.store(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    expiresAt.time_since_epoch()).count()),
                std::memory_order_release);
            impl.uiStandaloneFramePending.store(true, std::memory_order_release);
        }

        void ClearUiStandaloneFramePending(DriverImpl& impl, const bool notifyWorkers)
        {
            impl.uiStandaloneFramePendingUntilTickNs.store(0u, std::memory_order_release);
            impl.uiStandaloneFramePending.store(false, std::memory_order_release);
            if (notifyWorkers)
                Detail::NotifyThreadedWorkers(impl);
        }

        bool IsUiStandaloneFramePending(const DriverImpl& impl)
        {
            if (!impl.uiStandaloneFramePending.load(std::memory_order_acquire))
                return false;

            const uint64_t pendingUntil = impl.uiStandaloneFramePendingUntilTickNs.load(std::memory_order_acquire);
            return pendingUntil == 0u || GetSteadyClockTickNs() <= pendingUntil;
        }

        bool BeginStandaloneUiExplicitFrame(DriverImpl& impl)
        {
            NLS_PROFILE_SCOPE();
            if (impl.explicitDevice == nullptr || impl.explicitSwapchain == nullptr || impl.frameContexts.empty())
                return false;

            if (impl.explicitFrameActive)
                return true;

            if (!CanBeginStandaloneUiExplicitFrameForImpl(impl))
            {
                ClearUiStandaloneFramePending(impl, true);
                return false;
            }

            impl.uiStandaloneFrameSubmissionLock =
                std::unique_lock<std::timed_mutex>(impl.threadedRhiSubmissionMutex, std::defer_lock);
            if (!impl.uiStandaloneFrameSubmissionLock.try_lock_for(kUiStandaloneFrameSubmissionLockWait))
                return false;
            if (!impl.uiStandaloneFrameSubmissionLock.owns_lock())
            {
                ClearUiStandaloneFramePending(impl, true);
                return false;
            }

            const size_t frameContextIndex = impl.currentFrameIndex % impl.frameContexts.size();
            auto& frameContext = impl.frameContexts[frameContextIndex];
            ResetCurrentFrameQueueOperationTelemetry(impl);
            if (frameContext.frameFence != nullptr)
            {
                if (!WaitFrameFenceWithDriverPolicy(
                    impl,
                    frameContext.frameFence,
                    "RhiThreadCoordinator::BeginStandaloneUiExplicitFrame"))
                {
                    if (impl.uiStandaloneFrameSubmissionLock.owns_lock())
                        impl.uiStandaloneFrameSubmissionLock.unlock();
                    ClearUiStandaloneFramePending(impl, true);
                    return false;
                }
                Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(impl, frameContext, frameContextIndex);
            }
            frameContext.frameIndex = static_cast<uint64_t>(impl.currentFrameIndex);
            if (frameContext.imageAcquiredSemaphore != nullptr)
                frameContext.imageAcquiredSemaphore->Reset();
            if (frameContext.renderFinishedSemaphore != nullptr)
                frameContext.renderFinishedSemaphore->Reset();
            ClearSceneToUiWaitSemaphoreForFrame(impl, frameContext);
            if (frameContext.commandPool != nullptr)
                frameContext.commandPool->Reset();
            if (frameContext.commandBuffer != nullptr)
                frameContext.commandBuffer->Reset();
            for (const auto& parallelCommandPool : frameContext.parallelCommandPools)
            {
                if (parallelCommandPool != nullptr)
                    parallelCommandPool->Reset();
            }
            for (const auto& parallelCommandBuffer : frameContext.parallelCommandBuffers)
            {
                if (parallelCommandBuffer != nullptr)
                    parallelCommandBuffer->Reset();
            }
            for (const auto& childCommandPool : frameContext.childCommandPools)
            {
                if (childCommandPool != nullptr)
                    childCommandPool->Reset();
            }
            for (const auto& childCommandBuffer : frameContext.childCommandBuffers)
            {
                if (childCommandBuffer != nullptr)
                    childCommandBuffer->Reset();
            }
            if (frameContext.resourceStateTracker != nullptr)
            {
                frameContext.resourceStateTracker->RetireTransientResources(std::numeric_limits<uint64_t>::max());
                frameContext.resourceStateTracker->Reset();
                frameContext.resourceStateTracker->BeginFrame(impl.currentFrameIndex);
            }
            if (frameContext.descriptorAllocator != nullptr)
                frameContext.descriptorAllocator->BeginFrame(impl.currentFrameIndex);
            if (frameContext.uploadContext != nullptr)
                frameContext.uploadContext->BeginFrame(impl.currentFrameIndex);

            frameContext.hasAcquiredSwapchainImage = false;
            frameContext.uploadBytesReserved = 0u;
            frameContext.swapchainBackbufferView = nullptr;
            frameContext.explicitReadbackTexture = nullptr;

            std::optional<Render::RHI::RHIAcquiredImage> acquiredImage;
            {
                NLS_PROFILE_NAMED_SCOPE("RhiThreadCoordinator::AcquireStandaloneUiImage");
                acquiredImage = impl.explicitSwapchain->AcquireNextImage(
                    frameContext.imageAcquiredSemaphore,
                    frameContext.frameFence);
            }
            frameContext.hasAcquiredSwapchainImage = acquiredImage.has_value();
            frameContext.swapchainImageIndex = acquiredImage.has_value() ? acquiredImage->imageIndex : 0u;
            if (acquiredImage.has_value())
                frameContext.swapchainBackbufferView = impl.explicitSwapchain->GetBackbufferView(frameContext.swapchainImageIndex);
            SeedAcquiredSwapchainBackbufferState(frameContext);
            frameContext.explicitReadbackTexture =
                Render::FrameGraph::ResolveFrameReadbackTexture(nullptr, &frameContext);

            if (impl.renderDocCaptureController != nullptr)
                impl.renderDocCaptureController->OnPreFrame(true);

            if (frameContext.commandBuffer != nullptr)
                frameContext.commandBuffer->Begin();

            impl.uiStandaloneFrameActive = true;
            impl.explicitFrameActive = true;
            return true;
        }

        bool PresentStandaloneUiFrame(DriverImpl& impl)
        {
            NLS_PROFILE_SCOPE();
            if (!impl.uiStandaloneFrameActive)
                return false;

            NLS_ASSERT(impl.explicitSwapchain != nullptr, "PresentSwapchain requires explicitSwapchain");
            NLS_ASSERT(impl.explicitDevice != nullptr, "PresentSwapchain requires explicitDevice");
            (void)FinalizeStandaloneUiFrame(impl);
            return true;
        }

        class ScopedUiStandaloneFrameRequest final
        {
        public:
            explicit ScopedUiStandaloneFrameRequest(DriverImpl& impl)
                : m_impl(impl)
            {
                RefreshUiStandaloneFramePendingLease(m_impl);
            }

            ~ScopedUiStandaloneFrameRequest()
            {
                if (!m_keepPending)
                    ClearUiStandaloneFramePending(m_impl, true);
            }

            void KeepPending()
            {
                m_keepPending = true;
            }

        private:
            DriverImpl& m_impl;
            bool m_keepPending = false;
        };

    }

    bool Detail::TryBeginNextRhiFrameExecution(
        ThreadedRenderingLifecycle& lifecycle,
        size_t* slotIndex,
        RenderScenePackage* renderScenePackage)
    {
        NLS_PROFILE_SCOPE();
        size_t claimedSlotIndex = 0u;
        RenderFrameBuild claimedRenderFrameBuild;
        if (!lifecycle.TryBeginNextRhiFrameExecution(&claimedSlotIndex, &claimedRenderFrameBuild))
            return false;

        const auto slotState = lifecycle.CopySlot(claimedSlotIndex);
        if (!slotState.has_value() || !slotState->renderScenePackage.has_value())
            return false;

        if (slotIndex != nullptr)
            *slotIndex = claimedSlotIndex;
        if (renderScenePackage != nullptr)
            *renderScenePackage = slotState->renderScenePackage.value();
        return true;
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

            // External scene outputs are reused by the editor UI as sampled textures.
            // Recording their render passes in parallel lets the DX12 backend race on
            // per-texture state during command-list construction, which is especially
            // fragile while the Scene view is being resized.
            return !Render::FrameGraph::HasExternalSceneOutput(renderScenePackage);
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
        const auto routeToGraphicsQueueSubmission = [&submitPlan, &submissionUnits]()
        {
            submitPlan.batches.clear();
            submitPlan.temporarySemaphores.clear();

            ThreadedQueueSubmissionBatch graphicsBatch;
            graphicsBatch.queueType = Render::RHI::QueueType::Graphics;
            graphicsBatch.queueDependencyPolicy = QueueDependencyPolicy::Previous;
            graphicsBatch.requiresDependencyVisibility = false;
            for (const auto& submissionUnit : submissionUnits)
            {
                if (submissionUnit.commandBuffer == nullptr)
                    continue;

                if (submissionUnit.commandPool != nullptr)
                    graphicsBatch.commandPools.push_back(submissionUnit.commandPool);
                graphicsBatch.commandBuffers.push_back(submissionUnit.commandBuffer);
            }

            if (!graphicsBatch.commandBuffers.empty())
                submitPlan.batches.push_back(std::move(graphicsBatch));
            submitPlan.usedDedicatedComputeQueueSubmission = false;
        };
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
                    routeToGraphicsQueueSubmission();
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
                if (commandPool == nullptr)
                {
                    commandPool = impl.explicitDevice->CreateCommandPool(
                        workUnit.commandInput.queueType,
                        "ThreadedCommandPool" + std::to_string(renderScenePackage.frameId) + "_" +
                            std::to_string(workUnit.workUnitIndex));
                    if (commandPool == nullptr)
                        return false;
                }

                auto& commandBuffer = frameContext.parallelCommandBuffers[resourceIndex];
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

        RecordedParallelCommandWorkUnit RecordSingleParallelCommandWorkUnit(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
            const ParallelCommandWorkUnit& workUnit)
        {
            RecordedParallelCommandWorkUnit recordedWorkUnit;
            recordedWorkUnit.workUnit = workUnit;

            if (impl.explicitDevice == nullptr)
                return recordedWorkUnit;

            const auto& passInput = workUnit.commandInput;
            if (!Detail::IsPassRecordable(renderScenePackage, passInput))
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
            auto commandBuffer = frameContext.parallelCommandBuffers[resourceIndex];
            if (commandBuffer == nullptr)
            {
                Detail::LogSkippedPass(renderScenePackage, passInput, "Reusable parallel command buffer is null");
                MarkRecordedParallelCommandWorkUnitFailure(recordedWorkUnit, "Reusable parallel command buffer is null");
                return recordedWorkUnit;
            }

            commandBuffer->Begin();
            if (passInput.kind == RenderPassCommandKind::Compute)
            {
                NLS_PROFILE_NAMED_SCOPE(passProfileScopeName);
                commandBuffer->BeginGpuProfileScope(passProfileScopeName, __FUNCTION__);
                const auto recordedDispatchCount = Detail::RecordComputeDispatches(
                    *commandBuffer,
                    passInput.computeDispatchInputs,
                    false);
                commandBuffer->EndGpuProfileScope();
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
                recordedWorkUnit.commandBuffer = commandBuffer;
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
            if (!Detail::BeginPassCommandPlan(
                *commandBuffer,
                frameContext.swapchainBackbufferView,
                frameContext.swapchainDepthStencilView,
                effectivePassInput,
                &frameContext))
            {
                Detail::LogSkippedPass(renderScenePackage, effectivePassInput, "BeginPassCommandPlan failed");
                MarkRecordedParallelCommandWorkUnitFailure(recordedWorkUnit, "BeginPassCommandPlan failed");
                if (commandBuffer->IsRecording())
                    commandBuffer->End();
                return recordedWorkUnit;
            }

            const auto* effectivePassProfileScopeName = Detail::ResolvePassProfileScopeName(effectivePassInput);
            NLS_PROFILE_NAMED_SCOPE(effectivePassProfileScopeName);
            commandBuffer->BeginGpuProfileScope(effectivePassProfileScopeName, __FUNCTION__);
            const bool recordsSlicedSourceRange =
                workUnit.requiresOrderedSlicedSubmission &&
                workUnit.sourcePassIndex < renderScenePackage.passCommandInputs.size();
            const auto recordedDrawCount = recordsSlicedSourceRange
                ? Detail::RecordPreparedDrawCommandsForPassRange(
                    commandBuffer.get(),
                    effectivePassInput,
                    renderScenePackage.passCommandInputs[static_cast<size_t>(workUnit.sourcePassIndex)].recordedDrawCommands,
                    workUnit.recordedDrawBegin,
                    workUnit.recordedDrawCount)
                : Detail::RecordPreparedDrawCommandsForPass(commandBuffer.get(), effectivePassInput);
            commandBuffer->EndGpuProfileScope();
            Detail::EndPassCommandPlan(*commandBuffer, &effectivePassInput, &frameContext);
            if (recordedDrawCount == 0u &&
                (!effectivePassInput.recordedDrawCommands.empty() ||
                    (recordsSlicedSourceRange && workUnit.recordedDrawCount > 0u)))
            {
                Detail::LogSkippedPass(
                    renderScenePackage,
                    effectivePassInput,
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
            recordedWorkUnit.commandBuffer = commandBuffer;
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

        uint32_t ResolveParallelRecordingWorkerCount(const std::vector<ParallelCommandWorkUnit>& workUnits)
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

            if (!NLS::Base::Jobs::IsJobSystemInitialized())
                return 1u;

            const auto workerBudget = std::max(2u, NLS::Base::Jobs::GetJobWorkerCount() + 1u);
            return std::min(eligibleCount, workerBudget);
        }

        bool RecordParallelEligibleWorkUnitsWithJobSystem(
            RecordedParallelCommandBufferBatch& batch,
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
            const std::vector<ParallelCommandWorkUnit>& workUnits,
            const std::vector<size_t>& parallelEligibleIndices)
        {
            if (parallelEligibleIndices.empty())
                return true;

            std::vector<std::atomic_bool> workUnitCompleted(parallelEligibleIndices.size());
            for (auto& completed : workUnitCompleted)
                completed.store(false, std::memory_order_relaxed);

            struct RecordParallelWorkUnitJob final : public NLS::Base::Jobs::IJobParallelFor
            {
                RecordedParallelCommandBufferBatch* batch = nullptr;
                DriverImpl* impl = nullptr;
                Render::RHI::RHIFrameContext* frameContext = nullptr;
                const RenderScenePackage* renderScenePackage = nullptr;
                const std::vector<ParallelCommandWorkUnit>* workUnits = nullptr;
                const std::vector<size_t>* parallelEligibleIndices = nullptr;
                std::vector<std::atomic_bool>* workUnitCompleted = nullptr;

                void Execute(uint32_t index)
                {
                    const auto workUnitIndex = (*parallelEligibleIndices)[static_cast<size_t>(index)];
                    batch->recordedWorkUnits[workUnitIndex] = RecordSingleParallelCommandWorkUnit(
                        *impl,
                        *frameContext,
                        *renderScenePackage,
                        (*workUnits)[workUnitIndex]);
                    (*workUnitCompleted)[static_cast<size_t>(index)].store(true, std::memory_order_release);
                }
            };

            RecordParallelWorkUnitJob job;
            job.batch = &batch;
            job.impl = &impl;
            job.frameContext = &frameContext;
            job.renderScenePackage = &renderScenePackage;
            job.workUnits = &workUnits;
            job.parallelEligibleIndices = &parallelEligibleIndices;
            job.workUnitCompleted = &workUnitCompleted;

            NLS::Base::Jobs::JobParallelForScheduleOptions options;
            options.batchSize = 1u;
            options.debugName = "RhiThreadCoordinator::RecordParallelCommandWorkUnits";
            auto handle = NLS::Base::Jobs::ScheduleParallelFor(
                job,
                static_cast<uint32_t>(parallelEligibleIndices.size()),
                options);
            if (handle.id == 0u)
                return false;
            NLS::Base::Jobs::CompleteNoClear(handle);
            const auto completionStatus = NLS::Base::Jobs::Internal::GetJobCompletionStatus(handle);
            NLS::Base::Jobs::ClearWithoutSync(handle);
            const bool allWorkUnitsCompleted = std::all_of(
                workUnitCompleted.begin(),
                workUnitCompleted.end(),
                [](const std::atomic_bool& completed)
                {
                    return completed.load(std::memory_order_acquire);
                });
            return completionStatus == NLS::Base::Jobs::JobCompletionStatus::Succeeded &&
                allWorkUnitsCompleted;
        }

        bool RecordInRenderPassChildWorkUnitsWithJobSystem(
            RecordedParallelCommandBufferBatch& batch,
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
            const std::vector<ParallelCommandWorkUnit>& workUnits,
            const RenderPassCommandInput& parentPassInput)
        {
            if (workUnits.empty())
                return true;

            std::vector<std::atomic_bool> workUnitCompleted(workUnits.size());
            for (auto& completed : workUnitCompleted)
                completed.store(false, std::memory_order_relaxed);

            struct RecordChildWorkUnitJob final : public NLS::Base::Jobs::IJobParallelFor
            {
                RecordedParallelCommandBufferBatch* batch = nullptr;
                DriverImpl* impl = nullptr;
                Render::RHI::RHIFrameContext* frameContext = nullptr;
                const RenderScenePackage* renderScenePackage = nullptr;
                const std::vector<ParallelCommandWorkUnit>* workUnits = nullptr;
                const RenderPassCommandInput* parentPassInput = nullptr;
                std::vector<std::atomic_bool>* workUnitCompleted = nullptr;

                void Execute(uint32_t index)
                {
                    const auto workUnitIndex = static_cast<size_t>(index);
                    batch->recordedWorkUnits[workUnitIndex] = RecordSingleInRenderPassChildWorkUnit(
                        *impl,
                        *frameContext,
                        *renderScenePackage,
                        (*workUnits)[workUnitIndex],
                        *parentPassInput);
                    (*workUnitCompleted)[workUnitIndex].store(true, std::memory_order_release);
                }
            };

            RecordChildWorkUnitJob job;
            job.batch = &batch;
            job.impl = &impl;
            job.frameContext = &frameContext;
            job.renderScenePackage = &renderScenePackage;
            job.workUnits = &workUnits;
            job.parentPassInput = &parentPassInput;
            job.workUnitCompleted = &workUnitCompleted;

            NLS::Base::Jobs::JobParallelForScheduleOptions options;
            options.batchSize = 1u;
            options.debugName = "RhiThreadCoordinator::RecordInRenderPassChildCommandWorkUnits";
            auto handle = NLS::Base::Jobs::ScheduleParallelFor(
                job,
                static_cast<uint32_t>(workUnits.size()),
                options);
            if (handle.id == 0u)
                return false;
            NLS::Base::Jobs::CompleteNoClear(handle);
            const auto completionStatus = NLS::Base::Jobs::Internal::GetJobCompletionStatus(handle);
            NLS::Base::Jobs::ClearWithoutSync(handle);
            const bool allWorkUnitsCompleted = std::all_of(
                workUnitCompleted.begin(),
                workUnitCompleted.end(),
                [](const std::atomic_bool& completed)
                {
                    return completed.load(std::memory_order_acquire);
                });
            return completionStatus == NLS::Base::Jobs::JobCompletionStatus::Succeeded &&
                allWorkUnitsCompleted;
        }

        RecordedParallelCommandBufferBatch RecordParallelCommandWorkUnits(
            DriverImpl& impl,
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
            const std::vector<ParallelCommandWorkUnit>& workUnits)
        {
            RecordedParallelCommandBufferBatch batch;
            if (impl.explicitDevice == nullptr)
                return batch;

            batch.recordedWorkUnits.resize(workUnits.size());
            std::vector<size_t> workUnitResourceIndices;
            workUnitResourceIndices.reserve(workUnits.size());
            std::vector<size_t> parallelEligibleIndices;
            parallelEligibleIndices.reserve(workUnits.size());
            for (size_t index = 0; index < workUnits.size(); ++index)
            {
                workUnitResourceIndices.push_back(index);
                if (workUnits[index].eligibleForParallelRecording)
                    parallelEligibleIndices.push_back(index);
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

            const auto workerCount = ResolveParallelRecordingWorkerCount(workUnits);
            if (!parallelEligibleIndices.empty())
            {
                if (workerCount > 1u && NLS::Base::Jobs::IsJobSystemInitialized())
                {
                    const bool scheduledParallelJobs = RecordParallelEligibleWorkUnitsWithJobSystem(
                        batch,
                        impl,
                        frameContext,
                        renderScenePackage,
                        workUnits,
                        parallelEligibleIndices);
                    if (scheduledParallelJobs)
                    {
                        batch.parallelWorkerCountUsed = workerCount;
                        batch.usedParallelRecording = true;
                    }
                    else
                    {
                        for (const auto workUnitIndex : parallelEligibleIndices)
                        {
                            batch.recordedWorkUnits[workUnitIndex] = RecordSingleParallelCommandWorkUnit(
                                impl,
                                frameContext,
                                renderScenePackage,
                                workUnits[workUnitIndex]);
                        }
                        batch.parallelWorkerCountUsed = 1u;
                    }
                }
                else
                {
                    for (const auto workUnitIndex : parallelEligibleIndices)
                    {
                        batch.recordedWorkUnits[workUnitIndex] = RecordSingleParallelCommandWorkUnit(
                            impl,
                            frameContext,
                            renderScenePackage,
                            workUnits[workUnitIndex]);
                    }
                    batch.parallelWorkerCountUsed = 1u;
                }
            }

            for (size_t index = 0; index < workUnits.size(); ++index)
            {
                if (workUnits[index].eligibleForParallelRecording)
                    continue;

                batch.recordedWorkUnits[index] = RecordSingleParallelCommandWorkUnit(
                    impl,
                    frameContext,
                    renderScenePackage,
                    workUnits[index]);
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
                ? ResolveParallelRecordingWorkerCount(workUnits)
                : 1u;
            if (workerCount > 1u && NLS::Base::Jobs::IsJobSystemInitialized())
            {
                const bool scheduledChildJobs = RecordInRenderPassChildWorkUnitsWithJobSystem(
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
            Render::RHI::RHIFrameContext& frameContext,
            const RenderScenePackage& renderScenePackage,
            const ParallelCommandWorkUnit& workUnit,
            RhiSubmissionFrame* submissionFrame,
            const std::optional<RenderPassCommandInput>& overridePassInput = std::nullopt)
        {
            MainCommandBufferSerialRecordResult result;
            const auto& passInput = overridePassInput.has_value()
                ? *overridePassInput
                : workUnit.commandInput;
            const auto visibilityInput = Detail::BuildDependencyVisibilityPassInput(
                workUnit,
                workUnit.incomingDependencyEdges);

            if (!Detail::IsPassRecordable(renderScenePackage, passInput))
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
            if (!Detail::BeginPassCommandPlan(
                *frameContext.commandBuffer,
                frameContext.swapchainBackbufferView,
                frameContext.swapchainDepthStencilView,
                effectivePassInput,
                &frameContext))
            {
                Detail::LogSkippedPass(
                    renderScenePackage,
                    effectivePassInput,
                    "BeginPassCommandPlan failed");
                MarkCommandRecordingFailure(
                    submissionFrame,
                    "BeginPassCommandPlan failed");
                result.failed = true;
                return result;
            }

            result.recordedAnyCommand = true;
            const auto* effectivePassProfileScopeName =
                Detail::ResolvePassProfileScopeName(effectivePassInput);
            NLS_PROFILE_NAMED_SCOPE(effectivePassProfileScopeName);
            frameContext.commandBuffer->BeginGpuProfileScope(effectivePassProfileScopeName, __FUNCTION__);
            const bool recordsSlicedSourceRange =
                !overridePassInput.has_value() &&
                workUnit.requiresOrderedSlicedSubmission &&
                workUnit.sourcePassIndex < renderScenePackage.passCommandInputs.size();
            const auto recordedDrawCount = recordsSlicedSourceRange
                ? Detail::RecordPreparedDrawCommandsForPassRange(
                    frameContext.commandBuffer.get(),
                    effectivePassInput,
                    renderScenePackage.passCommandInputs[static_cast<size_t>(workUnit.sourcePassIndex)]
                        .recordedDrawCommands,
                    workUnit.recordedDrawBegin,
                    workUnit.recordedDrawCount)
                : Detail::RecordPreparedDrawCommandsForPass(
                    frameContext.commandBuffer.get(),
                    effectivePassInput);
            frameContext.commandBuffer->EndGpuProfileScope();
            Detail::EndPassCommandPlan(*frameContext.commandBuffer, &effectivePassInput, &frameContext);
            if (recordedDrawCount == 0u &&
                (!effectivePassInput.recordedDrawCommands.empty() ||
                    (recordsSlicedSourceRange && workUnit.recordedDrawCount > 0u)))
            {
                Detail::LogSkippedPass(
                    renderScenePackage,
                    effectivePassInput,
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
            RhiSubmissionFrame* submissionFrame)
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
            std::vector<std::shared_ptr<Render::RHI::RHICommandPool>> childCommandPoolKeepAlive;
            std::vector<std::shared_ptr<Render::RHI::RHICommandBuffer>> childCommandBufferKeepAlive;
            for (size_t workUnitIndex = 0u; workUnitIndex < workUnits.size();)
            {
                const auto& workUnit = workUnits[workUnitIndex];
                if (!workUnit.usesInRenderPassChildCommandRecording)
                {
                    const auto result = RecordWorkUnitOnMainCommandBuffer(
                        frameContext,
                        renderScenePackage,
                        workUnit,
                        submissionFrame);
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
                    frameContext,
                    renderScenePackage,
                    fallbackWorkUnit,
                    submissionFrame,
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

        bool RecordResourceVisibilityBatch(
            DriverImpl& impl,
            const RenderScenePackage& renderScenePackage,
            const ParallelCommandWorkUnit& workUnit,
            const RenderPassCommandInput& visibilityInput,
            std::vector<WorkUnitDependencyEdge> incomingDependencyEdges,
            TranslatedParallelCommandBufferBatch& batch)
        {
            if (impl.explicitDevice == nullptr ||
                !Detail::HasResourceVisibilityTransitions(visibilityInput))
            {
                return false;
            }

            auto commandPool = impl.explicitDevice->CreateCommandPool(
                visibilityInput.queueType,
                "ThreadedVisibilityTransitionPool" + std::to_string(renderScenePackage.frameId));
            if (commandPool == nullptr)
                return false;

            auto commandBuffer = commandPool->CreateCommandBuffer(
                "ThreadedVisibilityTransitions" + std::to_string(renderScenePackage.frameId));
            if (commandBuffer == nullptr)
                return false;

            commandBuffer->Begin();
            auto* frameContext = GetCurrentFrameContext(impl);
            const bool recordedTransitions = Detail::RecordResourceVisibilityTransitions(
                *commandBuffer,
                visibilityInput,
                frameContext);
            if (!recordedTransitions)
            {
                if (commandBuffer->IsRecording())
                    commandBuffer->End();
                return false;
            }

            if (commandBuffer->IsRecording())
                commandBuffer->End();

            Detail::ThreadedCommandSubmissionUnit submission;
            submission.submissionOrder = workUnit.submissionOrder;
            submission.queueType = visibilityInput.queueType;
            submission.queueDependencyPolicy = visibilityInput.queueDependencyPolicy;
            submission.dependencySourceSubmissionOrder =
                Detail::ResolveImplicitDependencySourceSubmissionOrder(
                    workUnit,
                    incomingDependencyEdges);
            submission.queueDependencySourceSubmissionOrders =
                Detail::BuildQueueDependencySourceOrders(incomingDependencyEdges);
            submission.incomingDependencyEdges = std::move(incomingDependencyEdges);
            submission.requiresDependencyVisibility = visibilityInput.requiresDependencyVisibility;
            submission.commandPool = std::move(commandPool);
            submission.commandBuffer = std::move(commandBuffer);
            batch.submissions.push_back(std::move(submission));
            return true;
        }

        bool RecordPostPassTransitionBatch(
            DriverImpl& impl,
            const RenderPassCommandInput& extractionVisibilityInput,
            TranslatedParallelCommandBufferBatch& batch)
        {
            if (impl.explicitDevice == nullptr ||
                !Detail::HasResourceVisibilityTransitions(extractionVisibilityInput))
            {
                return false;
            }

            auto commandPool = impl.explicitDevice->CreateCommandPool(
                Render::RHI::QueueType::Graphics,
                "ThreadedPostPassPool" + std::string(extractionVisibilityInput.debugName));
            if (commandPool == nullptr)
                return false;

            auto commandBuffer = commandPool->CreateCommandBuffer(
                "ThreadedPostPassTransitions" + extractionVisibilityInput.debugName);
            if (commandBuffer == nullptr)
                return false;

            commandBuffer->Begin();
            auto* frameContext = GetCurrentFrameContext(impl);
            const bool recordedTransitions = Detail::RecordResourceVisibilityTransitions(
                *commandBuffer,
                extractionVisibilityInput,
                frameContext);
            if (!recordedTransitions)
            {
                if (commandBuffer->IsRecording())
                    commandBuffer->End();
                return false;
            }

            if (commandBuffer->IsRecording())
                commandBuffer->End();

            Detail::ThreadedCommandSubmissionUnit submission;
            submission.submissionOrder = std::numeric_limits<uint64_t>::max();
            submission.queueType = Render::RHI::QueueType::Graphics;
            submission.queueDependencyPolicy = QueueDependencyPolicy::Previous;
            submission.commandPool = std::move(commandPool);
            submission.commandBuffer = std::move(commandBuffer);
            batch.submissions.push_back(std::move(submission));
            return true;
        }

        TranslatedParallelCommandBufferBatch TranslateRecordedParallelCommandWorkUnits(
            DriverImpl& impl,
            const RenderScenePackage& renderScenePackage,
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
                const auto visibilityInput = Detail::BuildDependencyVisibilityPassInput(
                    recordedWorkUnit->workUnit,
                    resourceDependencyEdges);
                const bool needsVisibilityBatch =
                    Detail::HasResourceVisibilityTransitions(visibilityInput);
                bool recordedVisibilityBatch = false;
                if (needsVisibilityBatch)
                {
                    flushPendingTranslatedUnits();
                    recordedVisibilityBatch = RecordResourceVisibilityBatch(
                        impl,
                        renderScenePackage,
                        recordedWorkUnit->workUnit,
                        visibilityInput,
                        resourceDependencyEdges,
                        translatedBatch);
                    if (!recordedVisibilityBatch)
                    {
                        MarkTranslatedParallelCommandFailure(
                            translatedBatch,
                            recordedWorkUnit,
                            "Dependency visibility transition recording failed");
                        continue;
                    }
                }

                const auto dependencySourceSubmissionOrder =
                    !controlDependencyEdges.empty()
                        ? std::optional<uint64_t>(controlDependencyEdges.front().sourceWorkUnitIndex)
                        : Detail::ResolveImplicitDependencySourceWorkUnitIndex(
                            recordedWorkUnit->workUnit);
                const bool requiresDependencyVisibility =
                    !recordedVisibilityBatch &&
                    Detail::ResolveImplicitRequiresDependencyVisibility(
                        recordedWorkUnit->workUnit);
                auto targetDependencyEdges = std::move(controlDependencyEdges);
                if (!recordedVisibilityBatch)
                {
                    targetDependencyEdges.insert(
                        targetDependencyEdges.end(),
                        std::make_move_iterator(resourceDependencyEdges.begin()),
                        std::make_move_iterator(resourceDependencyEdges.end()));
                }

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
            const auto extractionVisibilityInput =
                Render::FrameGraph::BuildExtractionVisibilityPassInput(renderScenePackage);
            if (Detail::HasResourceVisibilityTransitions(extractionVisibilityInput) &&
                !RecordPostPassTransitionBatch(
                    impl,
                    extractionVisibilityInput,
                    translatedBatch))
            {
                MarkTranslatedParallelCommandFailure(
                    translatedBatch,
                    nullptr,
                    "Post-pass transition recording failed");
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
        Render::RHI::DescriptorAllocatorStats* descriptorStats)
    {
        NLS_PROFILE_SCOPE();
        if (impl.explicitDevice == nullptr || impl.frameContexts.empty())
            return nullptr;
        if (impl.deviceLostDetected.load(std::memory_order_acquire) ||
            impl.unsafeGpuWorkQuarantined.load(std::memory_order_acquire))
            return nullptr;

        if (frameContextIndex >= impl.frameContexts.size())
        {
            if (submissionFrame != nullptr)
            {
                submissionFrame->frameContextIndex = static_cast<uint32_t>(frameContextIndex);
                MarkCommandRecordingFailure(submissionFrame, "Threaded RHI frame context index out of range");
            }
            return nullptr;
        }

        auto& frameContext = impl.frameContexts[frameContextIndex];
        ResetCurrentFrameQueueOperationTelemetry(impl);

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

        if (frameContext.frameFence != nullptr)
        {
            NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::WaitReusableFrameFence");
            if (!WaitFrameFenceWithDriverPolicy(
                impl,
                frameContext.frameFence,
                "RhiThreadCoordinator::BeginThreadedRhiFrame"))
            {
                return nullptr;
            }
            Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(impl, frameContext, frameContextIndex);
        }
        frameContext.frameIndex = static_cast<uint64_t>(impl.currentFrameIndex);
        if (frameContext.imageAcquiredSemaphore != nullptr)
            frameContext.imageAcquiredSemaphore->Reset();
        if (frameContext.renderFinishedSemaphore != nullptr)
            frameContext.renderFinishedSemaphore->Reset();
        ClearSceneToUiWaitSemaphoreForFrame(impl, frameContext);
        {
            NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::ResetCommandResources");
            if (frameContext.commandPool != nullptr)
                frameContext.commandPool->Reset();
            if (frameContext.commandBuffer != nullptr)
                frameContext.commandBuffer->Reset();
            for (const auto& parallelCommandPool : frameContext.parallelCommandPools)
            {
                if (parallelCommandPool != nullptr)
                    parallelCommandPool->Reset();
            }
            for (const auto& parallelCommandBuffer : frameContext.parallelCommandBuffers)
            {
                if (parallelCommandBuffer != nullptr)
                    parallelCommandBuffer->Reset();
            }
            for (const auto& childCommandPool : frameContext.childCommandPools)
            {
                if (childCommandPool != nullptr)
                    childCommandPool->Reset();
            }
            for (const auto& childCommandBuffer : frameContext.childCommandBuffers)
            {
                if (childCommandBuffer != nullptr)
                    childCommandBuffer->Reset();
            }
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
            frameContext.descriptorAllocator->BeginFrame(impl.currentFrameIndex);
            if (descriptorStats != nullptr)
                *descriptorStats = frameContext.descriptorAllocator->GetStats();
        }
        if (frameContext.uploadContext != nullptr)
            frameContext.uploadContext->BeginFrame(impl.currentFrameIndex);
        if (frameContext.computeFinishedSemaphore != nullptr)
            frameContext.computeFinishedSemaphore->Reset();

        frameContext.hasAcquiredSwapchainImage = false;
        frameContext.uploadBytesReserved = 0u;
        frameContext.swapchainBackbufferView = nullptr;
        frameContext.explicitReadbackTexture = nullptr;
        if (renderScenePackage.targetsSwapchain && impl.explicitSwapchain != nullptr)
        {
            std::optional<Render::RHI::RHIAcquiredImage> acquiredImage;
            {
                NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::AcquireNextImage");
                acquiredImage = impl.explicitSwapchain->AcquireNextImage(
                    frameContext.imageAcquiredSemaphore,
                    frameContext.frameFence);
            }
            frameContext.hasAcquiredSwapchainImage = acquiredImage.has_value();
            frameContext.swapchainImageIndex = acquiredImage.has_value() ? acquiredImage->imageIndex : 0u;
            if (acquiredImage.has_value())
                frameContext.swapchainBackbufferView = impl.explicitSwapchain->GetBackbufferView(frameContext.swapchainImageIndex);
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

        if (impl.renderDocCaptureController != nullptr)
            impl.renderDocCaptureController->OnPreFrame(renderScenePackage.targetsSwapchain);

        return &frameContext;
    }

    void Detail::RecordThreadedRhiWork(
        DriverImpl& impl,
        Render::RHI::RHIFrameContext& frameContext,
        const RenderScenePackage& renderScenePackage,
        AsyncComputeSubmitPlan* submitPlan,
        RhiSubmissionFrame* submissionFrame)
    {
        NLS_PROFILE_SCOPE();
        if (submitPlan == nullptr || submissionFrame == nullptr)
            return;

        if (frameContext.commandBuffer != nullptr)
        {
            const bool supportsOrderedWorkUnitSubmission =
                ShouldUseOrderedParallelCommandSubmissionForPackage(
                    impl,
                    renderScenePackage);
            const bool parallelRecordingReady =
                supportsOrderedWorkUnitSubmission &&
                impl.explicitDevice != nullptr &&
                frameContext.resourceStateTracker == nullptr &&
                impl.explicitDevice->GetCapabilities()
                    .GetFeature(Render::RHI::RHIDeviceFeature::ParallelCommandRecording)
                    .supported;
            const bool parallelTranslationReady =
                supportsOrderedWorkUnitSubmission &&
                impl.explicitDevice != nullptr &&
                impl.explicitDevice->GetCapabilities()
                    .GetFeature(Render::RHI::RHIDeviceFeature::ParallelCommandTranslation)
                    .supported;
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
            if (packageContainsOrderedSlicedWorkUnits &&
                (!allowOrderedSlicedSubmission ||
                    (packageContainsInRenderPassChildWorkUnits && !inRenderPassChildCommandRecordingReady)))
            {
                submissionFrame->parallelFallbackReason =
                    packageContainsInRenderPassChildWorkUnits
                        ? "in-render-pass child command recording unavailable; using unsliced passCommandInputs"
                        : "ordered sliced submission unavailable; using unsliced passCommandInputs";
            }
            const auto workUnits = Detail::BuildParallelCommandWorkUnits(
                renderScenePackage,
                useOrderedWorkUnitPath ? parallelRecordingReady : false,
                useOrderedWorkUnitPath ? parallelTranslationReady : false,
                allowOrderedSlicedSubmission,
                inRenderPassChildCommandRecordingReady);
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
                    submissionFrame);
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
                    workUnits);
                const auto translatedBatch = TranslateRecordedParallelCommandWorkUnits(
                    impl,
                    renderScenePackage,
                    recordedBatch);
                MarkParallelCommandRecordingFailures(
                    submissionFrame,
                    recordedBatch);
                for (const auto& failureReason : translatedBatch.commandRecordingFailureReasons)
                {
                    MarkCommandRecordingFailure(
                        submissionFrame,
                        failureReason.c_str());
                }
                *submitPlan = Detail::BuildAsyncComputeSubmitPlan(
                    impl,
                    frameContext,
                    translatedBatch.submissions);
                submissionFrame->recordedPassCount = translatedBatch.recordedPassCount;
                submissionFrame->recordedDrawCount = translatedBatch.recordedDrawCount;
                submissionFrame->recordedWorkUnitCount = translatedBatch.recordedWorkUnitCount;
                submissionFrame->parallelRecordingWorkerCount = recordedBatch.parallelWorkerCountUsed;
                submissionFrame->translatedWorkUnitCount = translatedBatch.translatedWorkUnitCount;
                submissionFrame->usedParallelCommandPath =
                    recordedBatch.usedParallelRecording ||
                    translatedBatch.usedTranslationMerge;
                submissionFrame->usedTranslationMerge = translatedBatch.usedTranslationMerge;
                submissionFrame->usedSerialCommandPath =
                    !submissionFrame->usedParallelCommandPath && !workUnits.empty();
            }
            else
            {
                bool recordedAnyCommand = false;
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
                    const auto visibilityInput = Detail::BuildDependencyVisibilityPassInput(
                        workUnit,
                        workUnit.incomingDependencyEdges);
                    if (!Detail::IsPassRecordable(renderScenePackage, passInput))
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
                    if (!Detail::BeginPassCommandPlan(
                        *frameContext.commandBuffer,
                        frameContext.swapchainBackbufferView,
                        frameContext.swapchainDepthStencilView,
                        effectivePassInput,
                        &frameContext))
                    {
                        Detail::LogSkippedPass(
                            renderScenePackage,
                            effectivePassInput,
                            "BeginPassCommandPlan failed");
                        MarkCommandRecordingFailure(
                            submissionFrame,
                            "BeginPassCommandPlan failed");
                        continue;
                    }

                    recordedAnyCommand = true;
                    const auto* effectivePassProfileScopeName =
                        Detail::ResolvePassProfileScopeName(effectivePassInput);
                    NLS_PROFILE_NAMED_SCOPE(effectivePassProfileScopeName);
                    frameContext.commandBuffer->BeginGpuProfileScope(effectivePassProfileScopeName, __FUNCTION__);
                    const auto recordedDrawCount = Detail::RecordPreparedDrawCommandsForPass(
                        frameContext.commandBuffer.get(),
                        effectivePassInput);
                    frameContext.commandBuffer->EndGpuProfileScope();
                    Detail::EndPassCommandPlan(*frameContext.commandBuffer, &effectivePassInput, &frameContext);
                    if (recordedDrawCount == 0u && !effectivePassInput.recordedDrawCommands.empty())
                    {
                        Detail::LogSkippedPass(
                            renderScenePackage,
                            effectivePassInput,
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
            }
        }

        if (submitPlan->batches.empty() &&
            renderScenePackage.targetsSwapchain &&
            frameContext.commandBuffer != nullptr &&
            !HasCommandRecordingFailure(submissionFrame))
        {
            frameContext.commandBuffer->Begin();
            if (frameContext.commandBuffer->IsRecording())
                frameContext.commandBuffer->End();
            Detail::ThreadedQueueSubmissionBatch batch;
            batch.queueType = Render::RHI::QueueType::Graphics;
            batch.commandBuffers.push_back(frameContext.commandBuffer);
            submitPlan->batches.push_back(std::move(batch));
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
        NLS_PROFILE_SCOPE();
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

        size_t firstGraphicsBatchIndex = submitPlan.batches.size();
        size_t lastGraphicsBatchIndex = submitPlan.batches.size();
        for (size_t batchIndex = 0u; batchIndex < submitPlan.batches.size(); ++batchIndex)
        {
            if (submitPlan.batches[batchIndex].queueType != Render::RHI::QueueType::Graphics)
                continue;

            if (firstGraphicsBatchIndex == submitPlan.batches.size())
                firstGraphicsBatchIndex = batchIndex;
            lastGraphicsBatchIndex = batchIndex;
        }

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

            if (batchIndex == firstGraphicsBatchIndex &&
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
            frameFenceResetForSubmission =
                frameFenceResetForSubmission ||
                submitDesc.signalFence != nullptr;
            {
                NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::QueueSubmitChecked");
                const auto submitResult = queue->SubmitChecked(submitDesc);
                if (submitDesc.signalFence != nullptr &&
                    submitResult.frameFenceSignalQueued)
                {
                    frameFenceSignalMayHaveBeenQueued = true;
                }
                deviceLostDuringSubmit |=
                    submitResult.code == Render::RHI::RHIQueueOperationStatusCode::DeviceLost;
                submittedGpuWorkMayNeedFenceConfirmation |= submitResult.Succeeded() ||
                    submitResult.mayHaveQueuedGpuWork;
                const bool submitted = RecordQueueOperationResult(
                    impl,
                    submitResult,
                    "RhiThreadCoordinator::SubmitCompiledExecution");
                if (submitted &&
                    hasExternalSceneOutput &&
                    signalRenderFinished)
                {
                    PublishSceneToUiWaitSemaphore(impl, frameContext);
                }
                if (!submitted)
                {
                    submissionSucceeded = false;
                    break;
                }
            }

            if (submissionSucceeded && shouldPresentAfterThisBatch)
            {
                NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::Present");
                if (impl.renderDocCaptureController != nullptr)
                    impl.renderDocCaptureController->OnPrePresent();

                Render::RHI::RHIPresentDesc presentDesc;
                presentDesc.swapchain = impl.explicitSwapchain;
                presentDesc.imageIndex = frameContext.swapchainImageIndex;
                if (frameContext.renderFinishedSemaphore != nullptr)
                    presentDesc.waitSemaphores.push_back(frameContext.renderFinishedSemaphore);
                if (signalFrameFenceOnPresent)
                    presentDesc.signalFence = frameContext.frameFence;
                frameFenceResetForSubmission =
                    frameFenceResetForSubmission ||
                    presentDesc.signalFence != nullptr;
                bool presented = false;
                {
                    NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::QueuePresentChecked");
                    const auto presentResult = graphicsQueue->PresentChecked(presentDesc);
                    if (presentDesc.signalFence != nullptr &&
                        presentResult.frameFenceSignalQueued)
                    {
                        frameFenceSignalMayHaveBeenQueued = true;
                    }
                    deviceLostDuringSubmit |=
                        presentResult.code == Render::RHI::RHIQueueOperationStatusCode::DeviceLost;
                    submittedGpuWorkMayNeedFenceConfirmation |= presentResult.Succeeded() ||
                        presentResult.mayHaveQueuedGpuWork;
                    presented = RecordQueueOperationResult(
                        impl,
                        presentResult,
                        "RhiThreadCoordinator::SubmitCompiledExecutionPresent");
                }

                if (impl.renderDocCaptureController != nullptr)
                    impl.renderDocCaptureController->OnPostPresent();

                if (!presented)
                {
                    submissionSucceeded = false;
                    break;
                }
            }
        }

        if (submissionFrame != nullptr)
            submissionFrame->retirementFenceWaited = false;

        const bool submittedButFenceNotConfirmed =
            submittedGpuWorkMayNeedFenceConfirmation &&
            frameFenceSignalMayHaveBeenQueued &&
            frameContext.frameFence != nullptr &&
            !deviceLostDuringSubmit;
        const bool submittedWithoutRetirementFence =
            submittedGpuWorkMayNeedFenceConfirmation &&
            !frameFenceSignalMayHaveBeenQueued;
        const bool mustDeferFrameScopedRetirement =
            submittedButFenceNotConfirmed;
        const bool mustQuarantineFrameScopedResources =
            submittedWithoutRetirementFence;
        if (mustDeferFrameScopedRetirement || mustQuarantineFrameScopedResources)
        {
            RetainThreadedSubmitPlanResourcesUntilNextFence(impl, frameContextIndex, submitPlan);
        }
        if (mustQuarantineFrameScopedResources)
        {
            Render::RHI::RHIQueueOperationResult quarantineResult;
            quarantineResult.mayHaveQueuedGpuWork = true;
            quarantineResult.message = "threaded submit did not queue a frame retirement fence";
            MarkQueuedGpuWorkWithoutRetirementFence(
                impl,
                "RhiThreadCoordinator::ExecuteThreadedSubmitPlan",
                quarantineResult);
        }

        if (submissionFrame != nullptr)
        {
            const bool recordingSucceeded = !HasCommandRecordingFailure(submissionFrame);
            CopyDriverQueueTelemetryToSubmissionFrame(impl, *submissionFrame);
            submissionFrame->deferredFrameScopedRetirement = mustDeferFrameScopedRetirement;
            submissionFrame->submittedSuccessfully =
                submissionSucceeded &&
                recordingSucceeded &&
                submissionFrame->currentFrameQueueOperationFailureCount == 0u &&
                !submissionFrame->deviceLostDetected;
        }

        FinalizeThreadedRhiFrameContext(
            impl,
            frameContext,
            submitPlan,
            mustDeferFrameScopedRetirement || mustQuarantineFrameScopedResources);
        if (mustDeferFrameScopedRetirement)
        {
            impl.deferredThreadedFrameScopedRetirementFrameContexts.insert(frameContextIndex);
        }
    }

    RhiSubmissionFrame Detail::SubmitThreadedRhiFrame(
        DriverImpl& impl,
        const RenderScenePackage& renderScenePackage,
        const size_t frameContextIndex)
    {
        NLS_PROFILE_SCOPE();
        const auto externalOutputSummary =
            Render::FrameGraph::BuildExternalSceneOutputSummary(renderScenePackage);
        RhiSubmissionFrame submissionFrame;
        submissionFrame.frameId = renderScenePackage.frameId;
        submissionFrame.offscreenOnly = !renderScenePackage.targetsSwapchain;
        submissionFrame.usedExternalOutputBridge = externalOutputSummary.hasExternalOutput;
        submissionFrame.externalOutputTextureCount =
            Render::FrameGraph::CountExternalSceneOutputSampledTextures(renderScenePackage);

        Render::RHI::ResourceStateTrackerStats preResetTrackerStats{};
        Render::RHI::DescriptorAllocatorStats descriptorStats{};
        auto* preparedFrameContext = Detail::BeginThreadedRhiFrame(
            impl,
            renderScenePackage,
            frameContextIndex,
            &submissionFrame,
            &preResetTrackerStats,
            &descriptorStats);
        if (preparedFrameContext == nullptr)
        {
            CopyDriverQueueTelemetryToSubmissionFrame(impl, submissionFrame);
            submissionFrame.submittedSuccessfully = false;
            return submissionFrame;
        }

        auto& frameContext = *preparedFrameContext;
        AsyncComputeSubmitPlan submitPlan;
        {
            NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::RecordThreadedRhiWork");
            Detail::RecordThreadedRhiWork(
                impl,
                frameContext,
                renderScenePackage,
                &submitPlan,
                &submissionFrame);
        }

        if (HasCommandRecordingFailure(&submissionFrame))
        {
            submissionFrame.submittedSuccessfully = false;
            CopyDriverQueueTelemetryToSubmissionFrame(impl, submissionFrame);
            FinalizeThreadedRhiFrameContext(
                impl,
                frameContext,
                submitPlan);
            return submissionFrame;
        }

        {
            NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::CompleteTelemetry");
            if (!Detail::CompleteThreadedRhiSubmissionTelemetry(
                impl,
                frameContext,
                renderScenePackage,
                preResetTrackerStats,
                &descriptorStats,
                submitPlan,
                &submissionFrame))
            {
                submissionFrame.submittedSuccessfully = false;
                FinalizeThreadedRhiFrameContext(
                    impl,
                    frameContext,
                    submitPlan);
                return submissionFrame;
            }
        }

        {
            NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::ExecuteSubmitPlan");
            Detail::ExecuteThreadedSubmitPlan(
                impl,
                frameContext,
                renderScenePackage,
                submitPlan,
                &submissionFrame,
                frameContextIndex);
        }
        {
            NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::LogSubmission");
            Detail::LogCompletedThreadedRhiSubmission(
                impl,
                frameContext,
                submissionFrame);
        }
        return submissionFrame;
    }

    bool Detail::CompleteThreadedRhiSubmissionTelemetry(
        DriverImpl& impl,
        Render::RHI::RHIFrameContext& frameContext,
        const RenderScenePackage& renderScenePackage,
        const Render::RHI::ResourceStateTrackerStats& preResetTrackerStats,
        Render::RHI::DescriptorAllocatorStats* descriptorStats,
        const AsyncComputeSubmitPlan& submitPlan,
        RhiSubmissionFrame* submissionFrame)
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
        CopyDriverQueueTelemetryToSubmissionFrame(impl, *submissionFrame);

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

        return !(submissionFrame->recordedPassCount == 0u && submissionFrame->offscreenOnly);
    }

    bool Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(
        DriverImpl& impl,
        Render::RHI::RHIFrameContext& frameContext,
        const size_t frameContextIndex)
    {
        if (impl.retainedThreadedSubmitResourceKeepAliveByFrameContext.size() < impl.frameContexts.size())
            impl.retainedThreadedSubmitResourceKeepAliveByFrameContext.resize(impl.frameContexts.size());

        const bool hadDeferredFrameScopedRetirement =
            impl.deferredThreadedFrameScopedRetirementFrameContexts.erase(frameContextIndex) != 0u;
        if (!hadDeferredFrameScopedRetirement)
            return false;

        if (frameContextIndex < impl.retainedThreadedSubmitResourceKeepAliveByFrameContext.size())
            impl.retainedThreadedSubmitResourceKeepAliveByFrameContext[frameContextIndex].clear();

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
        return true;
    }

    void Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(DriverImpl& impl)
    {
        for (size_t frameContextIndex = 0u; frameContextIndex < impl.frameContexts.size(); ++frameContextIndex)
        {
            ReleaseDeferredThreadedFrameScopedResourcesAfterFence(
                impl,
                impl.frameContexts[frameContextIndex],
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
    }

    bool RhiThreadCoordinator::CanBeginStandaloneExplicitFrame(const Driver& driver)
    {
        return driver.m_impl != nullptr &&
            CanBeginStandaloneExplicitFrameForImpl(*driver.m_impl);
    }

    bool RhiThreadCoordinator::BeginStandaloneExplicitFrame(Driver& driver, const bool acquireSwapchainImage)
    {
        NLS_PROFILE_SCOPE();
        NLS_ASSERT(driver.m_impl->explicitDevice != nullptr, "Explicit RHI is not enabled for this driver.");
        NLS_ASSERT(!driver.m_impl->frameContexts.empty(), "Explicit RHI frame contexts were not initialized.");
        NLS_ASSERT(!driver.m_impl->explicitFrameActive, "Cannot begin a new explicit frame while another one is still active.");

        if (!CanBeginStandaloneExplicitFrameForImpl(*driver.m_impl))
            return false;

        auto& frameContext = driver.m_impl->frameContexts[driver.m_impl->currentFrameIndex % driver.m_impl->frameContexts.size()];
        ResetCurrentFrameQueueOperationTelemetry(*driver.m_impl);
        if (frameContext.frameFence != nullptr)
        {
            NLS_PROFILE_NAMED_SCOPE("StandaloneRhiFrame::WaitReusableFrameFence");
            if (!WaitFrameFenceWithDriverPolicy(
                *driver.m_impl,
                frameContext.frameFence,
                "RhiThreadCoordinator::BeginStandaloneExplicitFrame"))
            {
                return false;
            }
            Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(
                *driver.m_impl,
                frameContext,
                driver.m_impl->currentFrameIndex % driver.m_impl->frameContexts.size());
        }
        frameContext.frameIndex = static_cast<uint64_t>(driver.m_impl->currentFrameIndex);
        if (frameContext.imageAcquiredSemaphore != nullptr)
            frameContext.imageAcquiredSemaphore->Reset();
        if (frameContext.renderFinishedSemaphore != nullptr)
            frameContext.renderFinishedSemaphore->Reset();
        ClearSceneToUiWaitSemaphoreForFrame(*driver.m_impl, frameContext);
        if (frameContext.commandPool != nullptr)
            frameContext.commandPool->Reset();
        if (frameContext.commandBuffer != nullptr)
            frameContext.commandBuffer->Reset();
        if (frameContext.resourceStateTracker != nullptr)
        {
            frameContext.resourceStateTracker->RetireTransientResources(std::numeric_limits<uint64_t>::max());
            frameContext.resourceStateTracker->Reset();
            frameContext.resourceStateTracker->BeginFrame(driver.m_impl->currentFrameIndex);
        }
        if (frameContext.descriptorAllocator != nullptr)
            frameContext.descriptorAllocator->BeginFrame(driver.m_impl->currentFrameIndex);
        if (frameContext.uploadContext != nullptr)
            frameContext.uploadContext->BeginFrame(driver.m_impl->currentFrameIndex);

        frameContext.hasAcquiredSwapchainImage = false;
        frameContext.uploadBytesReserved = 0u;
        frameContext.swapchainBackbufferView = nullptr;
        frameContext.explicitReadbackTexture = nullptr;
        if (acquireSwapchainImage && driver.m_impl->explicitSwapchain != nullptr)
        {
            std::optional<Render::RHI::RHIAcquiredImage> acquiredImage;
            {
                NLS_PROFILE_NAMED_SCOPE("StandaloneRhiFrame::AcquireNextImage");
                acquiredImage = driver.m_impl->explicitSwapchain->AcquireNextImage(
                    frameContext.imageAcquiredSemaphore,
                    frameContext.frameFence);
            }
            frameContext.hasAcquiredSwapchainImage = acquiredImage.has_value();
            frameContext.swapchainImageIndex = acquiredImage.has_value() ? acquiredImage->imageIndex : 0u;
            if (acquiredImage.has_value())
                frameContext.swapchainBackbufferView = driver.m_impl->explicitSwapchain->GetBackbufferView(frameContext.swapchainImageIndex);
            if (frameContext.swapchainBackbufferView != nullptr)
            {
                SeedAcquiredSwapchainBackbufferState(frameContext);
                const auto& swapchainDesc = driver.m_impl->explicitSwapchain->GetDesc();
                EnsureSwapchainDepthStencilAttachment(
                    *driver.m_impl,
                    frameContext,
                    swapchainDesc.width,
                    swapchainDesc.height);
            }
        }
        frameContext.explicitReadbackTexture =
            Render::FrameGraph::ResolveFrameReadbackTexture(nullptr, &frameContext);

        if (driver.m_impl->renderDocCaptureController != nullptr)
            driver.m_impl->renderDocCaptureController->OnPreFrame(acquireSwapchainImage);

        if (frameContext.commandBuffer != nullptr)
            frameContext.commandBuffer->Begin();

        driver.m_impl->explicitFrameActive = true;
        return true;
    }

    void RhiThreadCoordinator::EndStandaloneExplicitFrame(Driver& driver, const bool presentSwapchain)
    {
        NLS_PROFILE_SCOPE();
        if (driver.m_impl->explicitDevice == nullptr || driver.m_impl->frameContexts.empty() || !driver.m_impl->explicitFrameActive)
            return;

        auto& frameContext = driver.m_impl->frameContexts[driver.m_impl->currentFrameIndex % driver.m_impl->frameContexts.size()];
        if (frameContext.commandBuffer != nullptr && frameContext.commandBuffer->IsRecording())
            frameContext.commandBuffer->End();

        const bool willPresentSwapchain =
            presentSwapchain &&
            frameContext.hasAcquiredSwapchainImage &&
            driver.m_impl->explicitSwapchain != nullptr;
        const auto submitDesc = BuildStandaloneGraphicsSubmitDesc(
            frameContext,
            true,
            !willPresentSwapchain);

        auto queue = driver.m_impl->explicitDevice->GetQueue(Render::RHI::QueueType::Graphics);
        Render::RHI::RHIQueueOperationResult submitResult;
        bool submitAttempted = false;
        if (queue != nullptr)
        {
            submitAttempted = true;
            submitResult = queue->SubmitChecked(submitDesc);
            const bool submitted = RecordQueueOperationResult(
                *driver.m_impl,
                submitResult,
                "RhiThreadCoordinator::EndStandaloneExplicitFrame::Submit");
            if (submitted &&
                !presentSwapchain &&
                frameContext.renderFinishedSemaphore != nullptr)
            {
                PublishSceneToUiWaitSemaphore(*driver.m_impl, frameContext);
            }
            else
            {
                ClearSceneToUiWaitSemaphoreForFrame(*driver.m_impl, frameContext);
            }
            if (submitted && willPresentSwapchain)
            {
                const auto presentResult = PresentSwapchainFrame(
                    *driver.m_impl,
                    *queue,
                    frameContext,
                    true,
                    true);
                RecordQueueOperationResult(
                    *driver.m_impl,
                    presentResult,
                    "RhiThreadCoordinator::EndStandaloneExplicitFrame::Present");
                submitResult.mayHaveQueuedGpuWork =
                    submitResult.mayHaveQueuedGpuWork ||
                    presentResult.Succeeded() ||
                    presentResult.mayHaveQueuedGpuWork;
                submitResult.frameFenceSignalQueued =
                    submitResult.frameFenceSignalQueued ||
                    presentResult.frameFenceSignalQueued;
                if (presentResult.code == Render::RHI::RHIQueueOperationStatusCode::DeviceLost)
                    submitResult.code = Render::RHI::RHIQueueOperationStatusCode::DeviceLost;
                driver.ApplyPendingSwapchainResize();
            }
        }
        else
        {
            ClearSceneToUiWaitSemaphoreForFrame(*driver.m_impl, frameContext);
        }
        ClearUICompositionSignal(*driver.m_impl);
        const auto frameContextIndex = driver.m_impl->frameContexts.empty()
            ? 0u
            : static_cast<size_t>(driver.m_impl->currentFrameIndex % driver.m_impl->frameContexts.size());
        const bool deferFrameScopedRetirement =
            ShouldDeferFrameScopedRetirementAfterSubmit(submitResult, submitAttempted);
        bool quarantinedFrameScopedResources = false;
        if (deferFrameScopedRetirement)
        {
            DeferStandaloneFrameScopedRetirementAfterSubmit(
                *driver.m_impl,
                frameContextIndex,
                frameContext,
                submitDesc);
        }
        else if (SubmitMayHaveQueuedGpuWorkWithoutRetirementFence(submitResult, submitAttempted))
        {
            RetainStandaloneSubmitResourcesUntilNextFence(
                *driver.m_impl,
                frameContextIndex,
                frameContext,
                submitDesc);
            MarkQueuedGpuWorkWithoutRetirementFence(
                *driver.m_impl,
                "RhiThreadCoordinator::EndStandaloneExplicitFrame::Submit",
                submitResult);
            quarantinedFrameScopedResources = true;
        }

        if (!deferFrameScopedRetirement && !quarantinedFrameScopedResources && frameContext.descriptorAllocator != nullptr)
            frameContext.descriptorAllocator->EndFrame(driver.m_impl->currentFrameIndex);
        if (!deferFrameScopedRetirement && !quarantinedFrameScopedResources && frameContext.uploadContext != nullptr)
            frameContext.uploadContext->EndFrame(driver.m_impl->currentFrameIndex);

        RememberCompletedReadbackTexture(*driver.m_impl, frameContext.explicitReadbackTexture);
        ++driver.m_impl->currentFrameIndex;
        driver.m_impl->explicitFrameActive = false;
    }

    bool RhiThreadCoordinator::TryExecuteNextThreadedSubmission(
        Driver& driver,
        const RhiSubmissionAttribution attribution)
    {
        NLS_PROFILE_SCOPE();
        if (driver.m_impl->threadedLifecycle == nullptr)
            return false;

        if (attribution == RhiSubmissionAttribution::Worker &&
            IsUiStandaloneFramePending(*driver.m_impl))
        {
            return false;
        }

        {
            std::unique_lock<std::timed_mutex> submissionLock(
                driver.m_impl->threadedRhiSubmissionMutex,
                std::defer_lock);
            if (attribution == RhiSubmissionAttribution::SynchronousDrain)
            {
                if (!submissionLock.try_lock())
                    return false;
            }
            else
            {
                submissionLock.lock();
            }

            if (attribution == RhiSubmissionAttribution::Worker &&
                IsUiStandaloneFramePending(*driver.m_impl))
            {
                return false;
            }

            size_t slotIndex = 0u;
            RenderScenePackage renderScenePackage;
            if (!Detail::TryBeginNextRhiFrameExecution(
                *driver.m_impl->threadedLifecycle,
                &slotIndex,
                &renderScenePackage))
            {
                return false;
            }

            RhiSubmissionFrame submissionFrame;
            {
                NLS_PROFILE_NAMED_SCOPE("RhiThreadCoordinator::SubmitThreadedRhiFrame");
                submissionFrame = Detail::SubmitThreadedRhiFrame(*driver.m_impl, renderScenePackage, slotIndex);
            }
            {
                NLS_PROFILE_NAMED_SCOPE("RhiThreadCoordinator::CompleteRhiSubmission");
                driver.m_impl->threadedLifecycle->CompleteRhiSubmission(
                    slotIndex,
                    submissionFrame,
                    attribution);
            }
            {
                NLS_PROFILE_NAMED_SCOPE("RhiThreadCoordinator::RetireFrame");
                driver.m_impl->threadedLifecycle->RetireFrame(slotIndex);
            }
            {
                NLS_PROFILE_NAMED_SCOPE("RhiThreadCoordinator::NotifyThreadedWorkers");
                Detail::NotifyThreadedWorkers(*driver.m_impl);
            }
        }

        {
            NLS_PROFILE_NAMED_SCOPE("RhiThreadCoordinator::ApplyPendingSwapchainResize");
            driver.ApplyPendingSwapchainResize();
        }
        return true;
    }

    bool RhiThreadCoordinator::DrainPendingThreadedSubmissions(
        Driver& driver,
        const RhiSubmissionAttribution attribution)
    {
        NLS_PROFILE_SCOPE();
        bool progressed = false;
        while (TryExecuteNextThreadedSubmission(driver, attribution))
            progressed = true;
        return progressed;
    }

    void RhiThreadCoordinator::ReadPixels(
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
            NLS_LOG_WARNING("RhiThreadCoordinator::ReadPixels failed: " + result.message);
    }

    void RhiThreadCoordinator::ReadPixels(
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
            NLS_LOG_WARNING("RhiThreadCoordinator::ReadPixels failed: " + result.message);
    }

    Render::RHI::RHIReadbackResult RhiThreadCoordinator::ReadPixelsChecked(
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
                "RhiThreadCoordinator::ReadPixelsChecked");
        }
        else
        {
            completedResult.code = Render::RHI::RHIReadbackStatusCode::BackendFailure;
        }
        return completedResult;
    }

    Render::RHI::RHIReadbackResult RhiThreadCoordinator::ReadPixelsChecked(
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
                "RhiThreadCoordinator::ReadPixelsChecked");
        }
        else
        {
            completedResult.code = Render::RHI::RHIReadbackStatusCode::BackendFailure;
        }
        return completedResult;
    }

    Render::RHI::RHIReadbackResult RhiThreadCoordinator::BeginReadPixels(
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

        const auto texture = DriverRendererAccess::ResolveReadbackTexture(driver);
        if (texture == nullptr)
            return {
                Render::RHI::RHIReadbackStatusCode::InvalidArgument,
                "no active explicit readback source is available"
            };

        auto result = driver.m_impl->explicitDevice->BeginReadPixels(
            texture,
            x,
            y,
            width,
            height,
            format,
            type,
            data);
        MarkReadbackDeviceLostIfNeeded(
            *driver.m_impl,
            result,
            "RhiThreadCoordinator::BeginReadPixels");
        return result;
    }

    Render::RHI::RHIReadbackResult RhiThreadCoordinator::BeginReadPixels(
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

        auto result = driver.m_impl->explicitDevice->BeginReadPixels(
            texture,
            x,
            y,
            width,
            height,
            format,
            type,
            data);
        MarkReadbackDeviceLostIfNeeded(
            *driver.m_impl,
            result,
            "RhiThreadCoordinator::BeginReadPixels");
        return result;
    }

    bool RhiThreadCoordinator::PrepareUIRender(Driver& driver)
    {
        NLS_PROFILE_SCOPE();
        if (driver.m_impl->explicitDevice == nullptr)
            return false;

        if (driver.m_impl->threadedLifecycle != nullptr)
        {
            if (HasInFlightThreadedSwapchainFrame(*driver.m_impl))
                return false;
        }

        std::optional<ScopedUiStandaloneFrameRequest> uiFrameRequest;
        if (!driver.m_impl->explicitFrameActive && driver.m_impl->explicitSwapchain != nullptr)
        {
            if (driver.m_impl->frameContexts.empty())
                return false;

            uiFrameRequest.emplace(*driver.m_impl);
            uiFrameRequest->KeepPending();
            {
                NLS_PROFILE_NAMED_SCOPE("RhiThreadCoordinator::BeginStandaloneUiExplicitFrame");
                if (!BeginStandaloneUiExplicitFrame(*driver.m_impl))
                    return false;
            }
        }

        auto* uiBridgeDevice = driver.m_impl->explicitDevice->GetUIBridgeDevice();
        if (uiBridgeDevice == nullptr)
            return true;

        {
            NLS_PROFILE_NAMED_SCOPE("RHIUIBridgeDevice::PrepareUIRender");
            const bool prepared = uiBridgeDevice->PrepareUIRender();
            if (!prepared && !driver.m_impl->uiStandaloneFrameActive)
            {
                ClearUiStandaloneFramePending(*driver.m_impl, true);
            }
            return prepared;
        }
    }

    void RhiThreadCoordinator::PresentSwapchain(Driver& driver)
    {
        NLS_PROFILE_SCOPE();
        auto& impl = *driver.m_impl;

        {
            NLS_PROFILE_NAMED_SCOPE("RhiThreadCoordinator::PresentStandaloneUiFrame");
            if (PresentStandaloneUiFrame(impl))
            {
                NLS_PROFILE_NAMED_SCOPE("RhiThreadCoordinator::ApplyResizeAfterUiPresent");
                ClearUiStandaloneFramePending(impl, true);
                driver.ApplyPendingSwapchainResize();
                return;
            }
        }

        {
            NLS_PROFILE_NAMED_SCOPE("RhiThreadCoordinator::ApplyPendingSwapchainResize");
            driver.ApplyPendingSwapchainResize();
        }
    }
}

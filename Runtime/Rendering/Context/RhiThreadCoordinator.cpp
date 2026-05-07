#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <Debug/Assertion.h>
#include <Debug/Logger.h>

#include "Profiling/Profiler.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverInternal.h"
#include "Rendering/Context/RenderThreadCoordinator.h"
#include "Rendering/Context/RhiThreadCoordinator.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Rendering/RHI/Utils/DescriptorAllocator/DescriptorAllocator.h"
#include "Rendering/RHI/Utils/PipelineCache/PipelineCache.h"
#include "Rendering/RHI/Utils/ResourceStateTracker/ResourceStateTracker.h"
#include "Rendering/RHI/Utils/UploadContext/UploadContext.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"

namespace NLS::Render::Context
{
    namespace
    {
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

            frameContext.swapchainDepthStencilTexture = impl.explicitDevice->CreateTexture(textureDesc, nullptr);

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

        void FinalizeStandaloneUiFrame(DriverImpl& impl)
        {
            auto* frameContext = GetCurrentFrameContext(impl);
            if (frameContext == nullptr)
                return;

            if (frameContext->commandBuffer != nullptr && frameContext->commandBuffer->IsRecording())
                frameContext->commandBuffer->End();

            Render::RHI::RHISubmitDesc submitDesc;
            if (frameContext->commandBuffer != nullptr)
                submitDesc.commandBuffers.push_back(frameContext->commandBuffer);
            if (frameContext->hasAcquiredSwapchainImage && frameContext->imageAcquiredSemaphore != nullptr)
                submitDesc.waitSemaphores.push_back(frameContext->imageAcquiredSemaphore);
            if (frameContext->renderFinishedSemaphore != nullptr)
                submitDesc.signalSemaphores.push_back(frameContext->renderFinishedSemaphore);
            submitDesc.signalFence = frameContext->frameFence;

            auto queue = impl.explicitDevice != nullptr
                ? impl.explicitDevice->GetQueue(Render::RHI::QueueType::Graphics)
                : nullptr;
            if (queue != nullptr)
            {
                queue->Submit(submitDesc);

                if (impl.renderDocCaptureController != nullptr)
                    impl.renderDocCaptureController->OnPrePresent();

                Render::RHI::RHIPresentDesc presentDesc;
                presentDesc.swapchain = impl.explicitSwapchain;
                presentDesc.imageIndex = frameContext->swapchainImageIndex;
                if (frameContext->renderFinishedSemaphore != nullptr)
                    presentDesc.waitSemaphores.push_back(frameContext->renderFinishedSemaphore);
                if (impl.uiRenderFinishedSemaphore != nullptr)
                {
                    presentDesc.uiSignalSemaphore = impl.uiRenderFinishedSemaphore;
                    presentDesc.uiSignalValue = impl.uiRenderFinishedValue;
                }
                queue->Present(presentDesc);

                if (impl.renderDocCaptureController != nullptr)
                    impl.renderDocCaptureController->OnPostPresent();
            }

            if (frameContext->descriptorAllocator != nullptr)
                frameContext->descriptorAllocator->EndFrame(impl.currentFrameIndex);
            if (frameContext->uploadContext != nullptr)
                frameContext->uploadContext->EndFrame(impl.currentFrameIndex);

            RememberCompletedReadbackTexture(impl, frameContext->explicitReadbackTexture);
            impl.explicitFrameActive = false;
            impl.uiStandaloneFrameActive = false;
            impl.currentFrameIndex = (impl.currentFrameIndex + 1u) % impl.frameContexts.size();
            if (impl.uiStandaloneFrameSubmissionLock.owns_lock())
                impl.uiStandaloneFrameSubmissionLock.unlock();
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

        bool IsEmptySubresourceRange(const Render::RHI::RHISubresourceRange& range)
        {
            return range.mipLevelCount == 0u && range.arrayLayerCount == 0u;
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

        bool AreEqualSubresourceRanges(
            const Render::RHI::RHISubresourceRange& lhs,
            const Render::RHI::RHISubresourceRange& rhs)
        {
            return lhs.baseMipLevel == rhs.baseMipLevel &&
                lhs.mipLevelCount == rhs.mipLevelCount &&
                lhs.baseArrayLayer == rhs.baseArrayLayer &&
                lhs.arrayLayerCount == rhs.arrayLayerCount;
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
            transition.subresourceRange = !IsEmptySubresourceRange(edge.targetTextureAccess->subresourceRange)
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
                    return transition.texture == texture &&
                        (IsEmptySubresourceRange(subresourceRange) ||
                            IsEmptySubresourceRange(transition.subresourceRange) ||
                            AreEqualSubresourceRanges(transition.subresourceRange, subresourceRange));
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

        bool IsPassEligibleForParallelRecording(const RenderPassCommandInput& input)
        {
            switch (input.kind)
            {
            case RenderPassCommandKind::Compute:
            case RenderPassCommandKind::Lighting:
                return false;
            case RenderPassCommandKind::Opaque:
            case RenderPassCommandKind::Transparent:
            case RenderPassCommandKind::Skybox:
            case RenderPassCommandKind::Helper:
            case RenderPassCommandKind::GBuffer:
            default:
                return input.drawCount > 0u || !input.recordedDrawCommands.empty();
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
            return impl.threadedLifecycle == nullptr;
        }

        bool CanBeginStandaloneUiExplicitFrameForImpl(const DriverImpl& impl)
        {
            return !HasInFlightThreadedSwapchainFrame(impl);
        }

        bool BeginStandaloneUiExplicitFrame(DriverImpl& impl)
        {
            if (impl.explicitDevice == nullptr || impl.explicitSwapchain == nullptr || impl.frameContexts.empty())
                return false;

            if (impl.explicitFrameActive)
                return true;

            if (!CanBeginStandaloneUiExplicitFrameForImpl(impl))
                return false;

            impl.uiStandaloneFrameSubmissionLock =
                std::unique_lock<std::mutex>(impl.threadedRhiSubmissionMutex, std::try_to_lock);
            if (!impl.uiStandaloneFrameSubmissionLock.owns_lock())
                return false;

            auto& frameContext = impl.frameContexts[impl.currentFrameIndex % impl.frameContexts.size()];
            frameContext.frameIndex = static_cast<uint32_t>(impl.currentFrameIndex);
            if (frameContext.frameFence != nullptr)
                frameContext.frameFence->Wait();
            if (frameContext.frameFence != nullptr)
                frameContext.frameFence->Reset();
            if (frameContext.imageAcquiredSemaphore != nullptr)
                frameContext.imageAcquiredSemaphore->Reset();
            if (frameContext.renderFinishedSemaphore != nullptr)
                frameContext.renderFinishedSemaphore->Reset();
            if (frameContext.commandPool != nullptr)
                frameContext.commandPool->Reset();
            if (frameContext.commandBuffer != nullptr)
                frameContext.commandBuffer->Reset();
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

            const auto acquiredImage = impl.explicitSwapchain->AcquireNextImage(
                frameContext.imageAcquiredSemaphore,
                frameContext.frameFence);
            frameContext.hasAcquiredSwapchainImage = acquiredImage.has_value();
            frameContext.swapchainImageIndex = acquiredImage.has_value() ? acquiredImage->imageIndex : 0u;
            if (acquiredImage.has_value())
                frameContext.swapchainBackbufferView = impl.explicitSwapchain->GetBackbufferView(frameContext.swapchainImageIndex);
            frameContext.explicitReadbackTexture =
                Render::FrameGraph::ResolveFrameReadbackTexture(nullptr, &frameContext);

            if (frameContext.commandBuffer != nullptr)
                frameContext.commandBuffer->Begin();

            if (impl.renderDocCaptureController != nullptr)
                impl.renderDocCaptureController->OnPreFrame(true);

            impl.uiStandaloneFrameActive = true;
            impl.explicitFrameActive = true;
            return true;
        }

        bool PresentStandaloneUiFrame(DriverImpl& impl)
        {
            if (!impl.uiStandaloneFrameActive)
                return false;

            NLS_ASSERT(impl.explicitSwapchain != nullptr, "PresentSwapchain requires explicitSwapchain");
            NLS_ASSERT(impl.explicitDevice != nullptr, "PresentSwapchain requires explicitDevice");
            FinalizeStandaloneUiFrame(impl);
            return true;
        }

    }

    bool Detail::TryBeginNextRhiFrameExecution(
        ThreadedRenderingLifecycle& lifecycle,
        size_t* slotIndex,
        RenderScenePackage* renderScenePackage)
    {
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
        const bool parallelTranslationReady)
    {
        std::vector<ParallelCommandWorkUnit> workUnits;
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
            !renderScenePackage.parallelCommandWorkUnits.empty())
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
                    workUnit.debugName = !workUnit.commandInput.debugName.empty()
                        ? workUnit.commandInput.debugName
                        : ToThreadedPassDebugName(workUnit.commandInput.kind);
                }
                workUnit.eligibleForParallelRecording =
                    parallelRecordingReady && workUnit.eligibleForParallelRecording;
                workUnit.eligibleForParallelTranslation =
                    parallelTranslationReady && workUnit.eligibleForParallelTranslation;
            }
            Detail::PopulateVisibilityTransitionsFromResourceUsage(workUnits);
            return workUnits;
        }

        workUnits.reserve(
            renderScenePackage.computeDispatchInputs.size() +
            renderScenePackage.passCommandInputs.size());
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
            workUnit.debugName = !passInput.debugName.empty()
                ? passInput.debugName
                : ToThreadedPassDebugName(passInput.kind);
            const bool eligible = IsPassEligibleForParallelRecording(passInput);
            workUnit.eligibleForParallelRecording = parallelRecordingReady && eligible;
            workUnit.eligibleForParallelTranslation = parallelTranslationReady && eligible;
            workUnits.push_back(std::move(workUnit));
        }

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

        struct TranslatedParallelCommandBufferBatch
        {
            std::vector<Detail::ThreadedCommandSubmissionUnit> submissions;
            uint64_t recordedPassCount = 0u;
            uint64_t recordedDrawCount = 0u;
            uint64_t recordedComputeDispatchCount = 0u;
            uint64_t recordedWorkUnitCount = 0u;
            uint64_t translatedWorkUnitCount = 0u;
            bool usedTranslationMerge = false;
        };

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

            auto commandPool = impl.explicitDevice->CreateCommandPool(
                passInput.queueType,
                "ThreadedCommandPool" + std::to_string(renderScenePackage.frameId) + "_" + std::to_string(workUnit.workUnitIndex));
            if (commandPool == nullptr)
            {
                Detail::LogSkippedPass(renderScenePackage, passInput, "CreateCommandPool failed");
                return recordedWorkUnit;
            }

            auto commandBuffer = commandPool->CreateCommandBuffer(
                !workUnit.debugName.empty() ? workUnit.debugName : Detail::ToPassDebugName(passInput.kind));
            if (commandBuffer == nullptr)
            {
                Detail::LogSkippedPass(renderScenePackage, passInput, "CreateCommandBuffer failed");
                return recordedWorkUnit;
            }

            commandBuffer->Begin();
            if (passInput.kind == RenderPassCommandKind::Compute)
            {
                commandBuffer->BeginGpuProfileScope("ThreadedParallelComputePass", __FUNCTION__);
                const auto recordedDispatchCount = Detail::RecordComputeDispatches(
                    *commandBuffer,
                    passInput.computeDispatchInputs,
                    false);
                commandBuffer->EndGpuProfileScope();
                if (recordedDispatchCount == 0u)
                {
                    if (commandBuffer->IsRecording())
                        commandBuffer->End();
                    return recordedWorkUnit;
                }

                if (commandBuffer->IsRecording())
                    commandBuffer->End();

                recordedWorkUnit.commandPool = std::move(commandPool);
                recordedWorkUnit.commandBuffer = std::move(commandBuffer);
                recordedWorkUnit.recordedPassCount = 1u;
                recordedWorkUnit.recordedComputeDispatchCount = recordedDispatchCount;
                recordedWorkUnit.wasRecorded = true;
                return recordedWorkUnit;
            }

            if (passInput.targetsSwapchain && frameContext.swapchainBackbufferView == nullptr)
            {
                Detail::LogSkippedPass(renderScenePackage, passInput, "swapchainBackbufferView is null");
                if (commandBuffer->IsRecording())
                    commandBuffer->End();
                return recordedWorkUnit;
            }

            const auto effectivePassInput = ResolveSwapchainDepthPassInput(passInput, frameContext);
            if (!Detail::BeginPassCommandPlan(
                *commandBuffer,
                frameContext.swapchainBackbufferView,
                frameContext.swapchainDepthStencilView,
                effectivePassInput))
            {
                Detail::LogSkippedPass(renderScenePackage, effectivePassInput, "BeginPassCommandPlan failed");
                if (commandBuffer->IsRecording())
                    commandBuffer->End();
                return recordedWorkUnit;
            }

            commandBuffer->BeginGpuProfileScope(ToThreadedPassDebugName(effectivePassInput.kind), __FUNCTION__);
            const auto recordedDrawCount =
                Detail::RecordPreparedDrawCommandsForPass(commandBuffer.get(), effectivePassInput);
            commandBuffer->EndGpuProfileScope();
            Detail::EndPassCommandPlan(*commandBuffer);
            if (recordedDrawCount == 0u && !effectivePassInput.recordedDrawCommands.empty())
            {
                Detail::LogSkippedPass(
                    renderScenePackage,
                    effectivePassInput,
                    "No draws recorded but commands were expected");
                if (commandBuffer->IsRecording())
                    commandBuffer->End();
                return recordedWorkUnit;
            }

            if (commandBuffer->IsRecording())
                commandBuffer->End();

            recordedWorkUnit.commandPool = std::move(commandPool);
            recordedWorkUnit.commandBuffer = std::move(commandBuffer);
            recordedWorkUnit.recordedPassCount = 1u;
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

            const auto hardwareThreads = std::max(2u, std::thread::hardware_concurrency());
            const auto workerBudget = std::max(2u, hardwareThreads - 1u);
            return std::min(eligibleCount, workerBudget);
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
            std::vector<size_t> parallelEligibleIndices;
            parallelEligibleIndices.reserve(workUnits.size());
            for (size_t index = 0; index < workUnits.size(); ++index)
            {
                if (workUnits[index].eligibleForParallelRecording)
                    parallelEligibleIndices.push_back(index);
            }

            const auto workerCount = ResolveParallelRecordingWorkerCount(workUnits);
            if (!parallelEligibleIndices.empty())
            {
                if (workerCount > 1u)
                {
                    std::atomic_size_t nextEligibleIndex = 0u;
                    std::vector<std::thread> workers;
                    workers.reserve(workerCount);
                    for (uint32_t workerIndex = 0u; workerIndex < workerCount; ++workerIndex)
                    {
                        workers.emplace_back([&]()
                        {
                            while (true)
                            {
                                const size_t claimIndex = nextEligibleIndex.fetch_add(1u);
                                if (claimIndex >= parallelEligibleIndices.size())
                                    break;

                                const auto workUnitIndex = parallelEligibleIndices[claimIndex];
                                batch.recordedWorkUnits[workUnitIndex] = RecordSingleParallelCommandWorkUnit(
                                    impl,
                                    frameContext,
                                    renderScenePackage,
                                    workUnits[workUnitIndex]);
                            }
                        });
                    }

                    for (auto& worker : workers)
                    {
                        if (worker.joinable())
                            worker.join();
                    }

                    batch.parallelWorkerCountUsed = workerCount;
                    batch.usedParallelRecording = true;
                }
                else
                {
                    const auto workUnitIndex = parallelEligibleIndices.front();
                    batch.recordedWorkUnits[workUnitIndex] = RecordSingleParallelCommandWorkUnit(
                        impl,
                        frameContext,
                        renderScenePackage,
                        workUnits[workUnitIndex]);
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
            const RenderScenePackage& renderScenePackage,
            TranslatedParallelCommandBufferBatch& batch)
        {
            if (impl.explicitDevice == nullptr || renderScenePackage.extractedTextures.empty())
            {
                return false;
            }

            auto commandPool = impl.explicitDevice->CreateCommandPool(
                Render::RHI::QueueType::Graphics,
                "ThreadedPostPassPool" + std::to_string(renderScenePackage.frameId));
            if (commandPool == nullptr)
                return false;

            auto commandBuffer = commandPool->CreateCommandBuffer(
                "ThreadedPostPassTransitions" + std::to_string(renderScenePackage.frameId));
            if (commandBuffer == nullptr)
                return false;

            const auto extractionVisibilityInput =
                Render::FrameGraph::BuildExtractionVisibilityPassInput(renderScenePackage);
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
            const RecordedParallelCommandBufferBatch& recordedBatch)
        {
            TranslatedParallelCommandBufferBatch translatedBatch;
            struct PendingTranslatedUnit
            {
                const RecordedParallelCommandWorkUnit* recordedWorkUnit = nullptr;
                std::vector<WorkUnitDependencyEdge> incomingDependencyEdges;
                std::optional<uint64_t> dependencySourceSubmissionOrder;
                bool requiresDependencyVisibility = false;
            };

            std::vector<const RecordedParallelCommandWorkUnit*> orderedRecordedWorkUnits;
            orderedRecordedWorkUnits.reserve(recordedBatch.recordedWorkUnits.size());
            for (const auto& recordedWorkUnit : recordedBatch.recordedWorkUnits)
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

            for (const auto* recordedWorkUnit : orderedRecordedWorkUnits)
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
            RecordPostPassTransitionBatch(
                impl,
                renderScenePackage,
                translatedBatch);
            return translatedBatch;
        }
    }

    Render::RHI::RHIFrameContext* Detail::BeginThreadedRhiFrame(
        DriverImpl& impl,
        const RenderScenePackage& renderScenePackage,
        RhiSubmissionFrame* submissionFrame,
        Render::RHI::ResourceStateTrackerStats* preResetTrackerStats,
        Render::RHI::DescriptorAllocatorStats* descriptorStats)
    {
        NLS_PROFILE_SCOPE();
        if (impl.explicitDevice == nullptr || impl.frameContexts.empty())
            return nullptr;

        const size_t frameContextIndex = impl.currentFrameIndex % impl.frameContexts.size();
        auto& frameContext = impl.frameContexts[frameContextIndex];
        frameContext.frameIndex = static_cast<uint32_t>(impl.currentFrameIndex);

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
            frameContext.frameFence->Wait();
        if (frameContext.frameFence != nullptr)
            frameContext.frameFence->Reset();
        if (frameContext.imageAcquiredSemaphore != nullptr)
            frameContext.imageAcquiredSemaphore->Reset();
        if (frameContext.renderFinishedSemaphore != nullptr)
            frameContext.renderFinishedSemaphore->Reset();
        if (frameContext.commandPool != nullptr)
            frameContext.commandPool->Reset();
        if (frameContext.commandBuffer != nullptr)
            frameContext.commandBuffer->Reset();

        if (preResetTrackerStats != nullptr)
            *preResetTrackerStats = Render::RHI::ResourceStateTrackerStats{};
        if (frameContext.resourceStateTracker != nullptr)
        {
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
            const auto acquiredImage = impl.explicitSwapchain->AcquireNextImage(
                frameContext.imageAcquiredSemaphore,
                frameContext.frameFence);
            frameContext.hasAcquiredSwapchainImage = acquiredImage.has_value();
            frameContext.swapchainImageIndex = acquiredImage.has_value() ? acquiredImage->imageIndex : 0u;
            if (acquiredImage.has_value())
                frameContext.swapchainBackbufferView = impl.explicitSwapchain->GetBackbufferView(frameContext.swapchainImageIndex);
            if (frameContext.swapchainBackbufferView != nullptr)
            {
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
                impl.explicitDevice->GetCapabilities().supportsParallelCommandRecording;
            const bool parallelTranslationReady =
                supportsOrderedWorkUnitSubmission &&
                impl.explicitDevice != nullptr &&
                impl.explicitDevice->GetCapabilities().supportsParallelCommandTranslation;
            const auto workUnits = Detail::BuildParallelCommandWorkUnits(
                renderScenePackage,
                parallelRecordingReady,
                parallelTranslationReady);
            if (supportsOrderedWorkUnitSubmission && !workUnits.empty())
            {
                const auto recordedBatch = RecordParallelCommandWorkUnits(
                    impl,
                    frameContext,
                    renderScenePackage,
                    workUnits);
                const auto translatedBatch = TranslateRecordedParallelCommandWorkUnits(
                    impl,
                    renderScenePackage,
                    recordedBatch);
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

                        frameContext.commandBuffer->BeginGpuProfileScope("ThreadedComputePass", __FUNCTION__);
                        const auto recordedDispatches = Detail::RecordComputeDispatches(
                            *frameContext.commandBuffer,
                            passInput.computeDispatchInputs,
                            false);
                        frameContext.commandBuffer->EndGpuProfileScope();
                        if (recordedDispatches == 0u)
                            continue;

                        recordedAnyCommand = true;
                        submissionFrame->recordedPassCount += 1u;
                        submissionFrame->recordedWorkUnitCount += 1u;
                        continue;
                    }

                    if (Detail::HasResourceVisibilityTransitions(visibilityInput))
                    {
                        if (!beginMainCommandBufferIfNeeded())
                            continue;

                        recordedAnyCommand |= Detail::RecordResourceVisibilityTransitions(
                            *frameContext.commandBuffer,
                            visibilityInput,
                            &frameContext);
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
                        effectivePassInput))
                    {
                        Detail::LogSkippedPass(
                            renderScenePackage,
                            effectivePassInput,
                            "BeginPassCommandPlan failed");
                        submissionFrame->recordedPassCount += 1u;
                        submissionFrame->recordedWorkUnitCount += 1u;
                        continue;
                    }

                    recordedAnyCommand = true;
                    frameContext.commandBuffer->BeginGpuProfileScope(ToThreadedPassDebugName(effectivePassInput.kind), __FUNCTION__);
                    const auto recordedDrawCount = Detail::RecordPreparedDrawCommandsForPass(
                        frameContext.commandBuffer.get(),
                        effectivePassInput);
                    frameContext.commandBuffer->EndGpuProfileScope();
                    Detail::EndPassCommandPlan(*frameContext.commandBuffer);
                    if (recordedDrawCount == 0u && !effectivePassInput.recordedDrawCommands.empty())
                    {
                        Detail::LogSkippedPass(
                            renderScenePackage,
                            effectivePassInput,
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
                    recordedAnyCommand |= Detail::RecordResourceVisibilityTransitions(
                        *frameContext.commandBuffer,
                        extractionVisibilityInput,
                        &frameContext);
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
            frameContext.commandBuffer != nullptr)
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
        RhiSubmissionFrame* submissionFrame)
    {
        NLS_PROFILE_SCOPE();
        auto graphicsQueue = impl.explicitDevice != nullptr
            ? impl.explicitDevice->GetQueue(Render::RHI::QueueType::Graphics)
            : nullptr;
        auto computeQueue = impl.explicitDevice != nullptr
            ? impl.explicitDevice->GetQueue(Render::RHI::QueueType::Compute)
            : nullptr;

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

            const bool isLastBatch = batchIndex + 1u == submitPlan.batches.size();
            const bool isLastGraphicsBatch = batchIndex == lastGraphicsBatchIndex;
            if (renderScenePackage.targetsSwapchain &&
                isLastGraphicsBatch &&
                frameContext.renderFinishedSemaphore != nullptr)
            {
                submitDesc.signalSemaphores.push_back(frameContext.renderFinishedSemaphore);
            }
            if (isLastBatch)
                submitDesc.signalFence = frameContext.frameFence;
            queue->Submit(submitDesc);

            if (renderScenePackage.targetsSwapchain &&
                isLastGraphicsBatch &&
                frameContext.hasAcquiredSwapchainImage &&
                impl.explicitSwapchain != nullptr &&
                batch.queueType == Render::RHI::QueueType::Graphics)
            {
                if (impl.renderDocCaptureController != nullptr)
                    impl.renderDocCaptureController->OnPrePresent();

                Render::RHI::RHIPresentDesc presentDesc;
                presentDesc.swapchain = impl.explicitSwapchain;
                presentDesc.imageIndex = frameContext.swapchainImageIndex;
                if (frameContext.renderFinishedSemaphore != nullptr)
                    presentDesc.waitSemaphores.push_back(frameContext.renderFinishedSemaphore);
                graphicsQueue->Present(presentDesc);

                if (impl.renderDocCaptureController != nullptr)
                    impl.renderDocCaptureController->OnPostPresent();
            }
        }

        if (submissionFrame != nullptr)
        {
            if (frameContext.frameFence != nullptr)
            {
                NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::WaitFrameFence");
                submissionFrame->retirementFenceWaited = frameContext.frameFence->Wait();
            }
            else
                submissionFrame->retirementFenceWaited = false;
        }
        else if (frameContext.frameFence != nullptr)
        {
            NLS_PROFILE_NAMED_SCOPE("ThreadedRhiFrame::WaitFrameFence");
            frameContext.frameFence->Wait();
        }

        if (frameContext.descriptorAllocator != nullptr)
            frameContext.descriptorAllocator->EndFrame(impl.currentFrameIndex);
        if (frameContext.uploadContext != nullptr)
            frameContext.uploadContext->EndFrame(impl.currentFrameIndex);

        submitPlan.batches.clear();
        submitPlan.temporarySemaphores.clear();
        RememberCompletedReadbackTexture(impl, frameContext.explicitReadbackTexture);
        impl.currentFrameIndex = (impl.currentFrameIndex + 1u) % impl.frameContexts.size();
    }

    RhiSubmissionFrame Detail::SubmitThreadedRhiFrame(
        DriverImpl& impl,
        const RenderScenePackage& renderScenePackage)
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
            &submissionFrame,
            &preResetTrackerStats,
            &descriptorStats);
        if (preparedFrameContext == nullptr)
            return submissionFrame;

        auto& frameContext = *preparedFrameContext;
        AsyncComputeSubmitPlan submitPlan;
        Detail::RecordThreadedRhiWork(
            impl,
            frameContext,
            renderScenePackage,
            &submitPlan,
            &submissionFrame);
        if (!Detail::CompleteThreadedRhiSubmissionTelemetry(
            impl,
            frameContext,
            renderScenePackage,
            preResetTrackerStats,
            &descriptorStats,
            submitPlan,
            &submissionFrame))
        {
            return submissionFrame;
        }

        Detail::ExecuteThreadedSubmitPlan(
            impl,
            frameContext,
            renderScenePackage,
            submitPlan,
            &submissionFrame);
        Detail::LogCompletedThreadedRhiSubmission(
            impl,
            frameContext,
            submissionFrame);
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
            submissionFrame->recordedPassCount > 0u &&
            submissionFrame->recordedDrawCount > 0u;
        submissionFrame->usedAsyncComputeQueueSubmission = submitPlan.usedDedicatedComputeQueueSubmission;
        submissionFrame->asyncComputeDisposition =
            submissionFrame->usedAsyncComputeQueueSubmission
                ? AsyncComputeDisposition::Submitted
                : ResolveThreadedAsyncComputeDisposition(impl, renderScenePackage);

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

    void RhiThreadCoordinator::BeginStandaloneExplicitFrame(Driver& driver, const bool acquireSwapchainImage)
    {
        NLS_PROFILE_SCOPE();
        NLS_ASSERT(driver.m_impl->explicitDevice != nullptr, "Explicit RHI is not enabled for this driver.");
        NLS_ASSERT(!driver.m_impl->frameContexts.empty(), "Explicit RHI frame contexts were not initialized.");
        NLS_ASSERT(!driver.m_impl->explicitFrameActive, "Cannot begin a new explicit frame while another one is still active.");

        if (!CanBeginStandaloneExplicitFrameForImpl(*driver.m_impl))
            return;

        auto& frameContext = driver.m_impl->frameContexts[driver.m_impl->currentFrameIndex % driver.m_impl->frameContexts.size()];
        frameContext.frameIndex = static_cast<uint32_t>(driver.m_impl->currentFrameIndex);
        if (frameContext.frameFence != nullptr)
            frameContext.frameFence->Wait();
        if (frameContext.frameFence != nullptr)
            frameContext.frameFence->Reset();
        if (frameContext.imageAcquiredSemaphore != nullptr)
            frameContext.imageAcquiredSemaphore->Reset();
        if (frameContext.renderFinishedSemaphore != nullptr)
            frameContext.renderFinishedSemaphore->Reset();
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
            const auto acquiredImage = driver.m_impl->explicitSwapchain->AcquireNextImage(
                frameContext.imageAcquiredSemaphore,
                frameContext.frameFence);
            frameContext.hasAcquiredSwapchainImage = acquiredImage.has_value();
            frameContext.swapchainImageIndex = acquiredImage.has_value() ? acquiredImage->imageIndex : 0u;
            if (acquiredImage.has_value())
                frameContext.swapchainBackbufferView = driver.m_impl->explicitSwapchain->GetBackbufferView(frameContext.swapchainImageIndex);
            if (frameContext.swapchainBackbufferView != nullptr)
            {
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

        if (frameContext.commandBuffer != nullptr)
            frameContext.commandBuffer->Begin();

        if (driver.m_impl->renderDocCaptureController != nullptr)
            driver.m_impl->renderDocCaptureController->OnPreFrame(acquireSwapchainImage);

        driver.m_impl->explicitFrameActive = true;
    }

    void RhiThreadCoordinator::EndStandaloneExplicitFrame(Driver& driver, const bool presentSwapchain)
    {
        NLS_PROFILE_SCOPE();
        if (driver.m_impl->explicitDevice == nullptr || driver.m_impl->frameContexts.empty() || !driver.m_impl->explicitFrameActive)
            return;

        auto& frameContext = driver.m_impl->frameContexts[driver.m_impl->currentFrameIndex % driver.m_impl->frameContexts.size()];
        if (frameContext.commandBuffer != nullptr && frameContext.commandBuffer->IsRecording())
            frameContext.commandBuffer->End();

        Render::RHI::RHISubmitDesc submitDesc;
        if (frameContext.commandBuffer != nullptr)
            submitDesc.commandBuffers.push_back(frameContext.commandBuffer);
        if (frameContext.hasAcquiredSwapchainImage && frameContext.imageAcquiredSemaphore != nullptr)
            submitDesc.waitSemaphores.push_back(frameContext.imageAcquiredSemaphore);
        if (presentSwapchain && frameContext.renderFinishedSemaphore != nullptr)
            submitDesc.signalSemaphores.push_back(frameContext.renderFinishedSemaphore);
        submitDesc.signalFence = frameContext.frameFence;

        auto queue = driver.m_impl->explicitDevice->GetQueue(Render::RHI::QueueType::Graphics);
        if (queue != nullptr)
        {
            queue->Submit(submitDesc);
            if (presentSwapchain && frameContext.hasAcquiredSwapchainImage && driver.m_impl->explicitSwapchain != nullptr)
            {
                if (driver.m_impl->renderDocCaptureController != nullptr)
                    driver.m_impl->renderDocCaptureController->OnPrePresent();

                Render::RHI::RHIPresentDesc presentDesc;
                presentDesc.swapchain = driver.m_impl->explicitSwapchain;
                presentDesc.imageIndex = frameContext.swapchainImageIndex;
                if (frameContext.renderFinishedSemaphore != nullptr)
                    presentDesc.waitSemaphores.push_back(frameContext.renderFinishedSemaphore);
                queue->Present(presentDesc);
                if (driver.m_impl->renderDocCaptureController != nullptr)
                    driver.m_impl->renderDocCaptureController->OnPostPresent();
                driver.ApplyPendingSwapchainResize();
            }
        }
        if (frameContext.descriptorAllocator != nullptr)
            frameContext.descriptorAllocator->EndFrame(driver.m_impl->currentFrameIndex);
        if (frameContext.uploadContext != nullptr)
            frameContext.uploadContext->EndFrame(driver.m_impl->currentFrameIndex);

        RememberCompletedReadbackTexture(*driver.m_impl, frameContext.explicitReadbackTexture);
        driver.m_impl->currentFrameIndex = (driver.m_impl->currentFrameIndex + 1u) % driver.m_impl->frameContexts.size();
        driver.m_impl->explicitFrameActive = false;
    }

    bool RhiThreadCoordinator::TryExecuteNextThreadedSubmission(
        Driver& driver,
        const RhiSubmissionAttribution attribution)
    {
        NLS_PROFILE_SCOPE();
        if (driver.m_impl->threadedLifecycle == nullptr)
            return false;

        {
            std::unique_lock<std::mutex> submissionLock(driver.m_impl->threadedRhiSubmissionMutex);

            size_t slotIndex = 0u;
            RenderScenePackage renderScenePackage;
            if (!Detail::TryBeginNextRhiFrameExecution(
                *driver.m_impl->threadedLifecycle,
                &slotIndex,
                &renderScenePackage))
            {
                return false;
            }

            const RhiSubmissionFrame submissionFrame =
                Detail::SubmitThreadedRhiFrame(*driver.m_impl, renderScenePackage);
            driver.m_impl->threadedLifecycle->CompleteRhiSubmission(
                slotIndex,
                submissionFrame,
                attribution);
            driver.m_impl->threadedLifecycle->RetireFrame(slotIndex);
        }

        driver.ApplyPendingSwapchainResize();
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
        if (driver.m_impl == nullptr || driver.m_impl->explicitDevice == nullptr)
        {
            NLS_LOG_WARNING("RhiThreadCoordinator::ReadPixels: explicit device is unavailable.");
            return;
        }

        const auto texture = DriverRendererAccess::ResolveReadbackTexture(driver);
        if (texture == nullptr)
        {
            NLS_LOG_WARNING("RhiThreadCoordinator::ReadPixels: no active explicit readback source is available.");
            return;
        }

        driver.m_impl->explicitDevice->ReadPixels(
            texture,
            x,
            y,
            width,
            height,
            format,
            type,
            data);
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
        if (driver.m_impl == nullptr || driver.m_impl->explicitDevice == nullptr)
        {
            NLS_LOG_WARNING("RhiThreadCoordinator::ReadPixels: explicit device is unavailable.");
            return;
        }

        if (texture == nullptr)
        {
            NLS_LOG_WARNING("RhiThreadCoordinator::ReadPixels: explicit readback source is unavailable.");
            return;
        }

        driver.m_impl->explicitDevice->ReadPixels(
            texture,
            x,
            y,
            width,
            height,
            format,
            type,
            data);
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

        if (!driver.m_impl->explicitFrameActive && driver.m_impl->explicitSwapchain != nullptr)
        {
            if (!BeginStandaloneUiExplicitFrame(*driver.m_impl))
                return false;
        }

        return driver.m_impl->explicitDevice->PrepareUIRender();
    }

    void RhiThreadCoordinator::PresentSwapchain(Driver& driver)
    {
        NLS_PROFILE_SCOPE();
        auto& impl = *driver.m_impl;

        if (PresentStandaloneUiFrame(impl))
        {
            driver.ApplyPendingSwapchainResize();
            return;
        }

        driver.ApplyPendingSwapchainResize();
    }
}

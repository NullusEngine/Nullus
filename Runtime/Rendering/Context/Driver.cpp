#include <Debug/Assertion.h>
#include <Debug/Logger.h>
#include <Core/ServiceLocator.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <Math/Vector4.h>

#include "Profiling/Profiler.h"
#include "Rendering/Data/PipelineState.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/RHI/Backends/RHIDeviceFactory.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Rendering/RHI/Core/RHISubresourceRangeUtils.h"
#include "Rendering/RHI/Core/RHISwapchain.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/RHI/RHITypes.h"
#include "Rendering/RHI/Utils/DescriptorAllocator/DescriptorAllocator.h"
#include "Rendering/RHI/Utils/PipelineCache/PipelineCache.h"
#include "Rendering/RHI/Utils/ResourceStateTracker/ResourceStateTracker.h"
#include "Rendering/RHI/Utils/UploadContext/UploadContext.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "Rendering/FrameGraph/FrameGraphExecutionContext.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Context/DriverInternal.h"
#include "Rendering/Context/RenderScenePackageBuilder.h"
#include "Rendering/Context/RenderThreadCoordinator.h"
#include "Rendering/Context/RhiThreadCoordinator.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Context/SwapchainResizePolicy.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Rendering/Tooling/RenderDocCaptureController.h"

NLS::Render::Context::MeshRuntimeUploadResult::MeshRuntimeUploadResult() = default;
NLS::Render::Context::MeshRuntimeUploadResult::~MeshRuntimeUploadResult() = default;
NLS::Render::Context::MeshRuntimeUploadResult::MeshRuntimeUploadResult(
    MeshRuntimeUploadResult&&) noexcept = default;
NLS::Render::Context::MeshRuntimeUploadResult&
NLS::Render::Context::MeshRuntimeUploadResult::operator=(MeshRuntimeUploadResult&&) noexcept = default;
#include "Rendering/Resources/IMesh.h"
#include "Rendering/Resources/IndexedObjectDataShaderSupport.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Utils/Conversions.h"

namespace NLS::Render::Context
{
namespace
{
    constexpr const char* kDriverTelemetryDelimiter = " | driver: ";

    std::mutex& GetUnsafeGpuWorkQuarantineKeepAliveMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    std::vector<std::shared_ptr<void>>& GetUnsafeGpuWorkQuarantineKeepAlive()
    {
        static std::vector<std::shared_ptr<void>> keepAlive;
        return keepAlive;
    }

    void PreserveUnsafeGpuWorkQuarantineResources(DriverImpl& impl)
    {
        if (!impl.unsafeGpuWorkQuarantined.load(std::memory_order_acquire) ||
            impl.deviceLostDetected.load(std::memory_order_acquire) ||
            impl.unsafeGpuWorkQuarantineResourcesPreserved.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }

        std::lock_guard lock(GetUnsafeGpuWorkQuarantineKeepAliveMutex());
        auto& keepAlive = GetUnsafeGpuWorkQuarantineKeepAlive();
        if (impl.explicitDevice != nullptr)
            keepAlive.push_back(impl.explicitDevice);
        if (impl.explicitSwapchain != nullptr)
            keepAlive.push_back(impl.explicitSwapchain);
        if (impl.pipelineCache != nullptr)
            keepAlive.push_back(impl.pipelineCache);
        for (auto& retainedFrameResources : impl.retainedThreadedSubmitResourceKeepAliveByFrameContext)
        {
            for (auto& resource : retainedFrameResources)
            {
                if (resource != nullptr)
                    keepAlive.push_back(resource);
            }
        }
        for (const auto& frameContext : impl.frameContexts)
        {
            if (frameContext.commandPool != nullptr)
                keepAlive.push_back(frameContext.commandPool);
            if (frameContext.commandBuffer != nullptr)
                keepAlive.push_back(frameContext.commandBuffer);
            for (const auto& commandPool : frameContext.parallelCommandPools)
            {
                if (commandPool != nullptr)
                    keepAlive.push_back(commandPool);
            }
            for (const auto& commandBuffer : frameContext.parallelCommandBuffers)
            {
                if (commandBuffer != nullptr)
                    keepAlive.push_back(commandBuffer);
            }
            for (const auto& commandPool : frameContext.childCommandPools)
            {
                if (commandPool != nullptr)
                    keepAlive.push_back(commandPool);
            }
            for (const auto& commandBuffer : frameContext.childCommandBuffers)
            {
                if (commandBuffer != nullptr)
                    keepAlive.push_back(commandBuffer);
            }
            if (frameContext.frameFence != nullptr)
                keepAlive.push_back(frameContext.frameFence);
            if (frameContext.imageAcquiredSemaphore != nullptr)
                keepAlive.push_back(frameContext.imageAcquiredSemaphore);
            if (frameContext.renderFinishedSemaphore != nullptr)
                keepAlive.push_back(frameContext.renderFinishedSemaphore);
            if (frameContext.computeFinishedSemaphore != nullptr)
                keepAlive.push_back(frameContext.computeFinishedSemaphore);
            if (frameContext.descriptorAllocator != nullptr)
                keepAlive.push_back(frameContext.descriptorAllocator);
            if (frameContext.uploadContext != nullptr)
                keepAlive.push_back(frameContext.uploadContext);
            if (frameContext.resourceStateTracker != nullptr)
                keepAlive.push_back(frameContext.resourceStateTracker);
            if (frameContext.swapchainBackbufferView != nullptr)
                keepAlive.push_back(frameContext.swapchainBackbufferView);
            if (frameContext.swapchainDepthStencilView != nullptr)
                keepAlive.push_back(frameContext.swapchainDepthStencilView);
            if (frameContext.swapchainDepthStencilTexture != nullptr)
                keepAlive.push_back(frameContext.swapchainDepthStencilTexture);
            if (frameContext.explicitReadbackTexture != nullptr)
                keepAlive.push_back(frameContext.explicitReadbackTexture);
        }
        if (impl.threadedLifecycle != nullptr)
        {
            for (auto slot : impl.threadedLifecycle->CopySlots())
                keepAlive.push_back(std::make_shared<InFlightFrameSlot>(std::move(slot)));
        }
    }

    struct DriverQueueTelemetrySnapshot
    {
        uint64_t queueOperationFailureCount = 0u;
        std::string lastQueueOperationFailure;
        uint64_t currentFrameQueueOperationFailureCount = 0u;
        std::string currentFrameLastQueueOperationFailure;
        bool deviceLostDetected = false;
        std::string deviceLostReason;
        bool unsafeGpuWorkQuarantined = false;
        std::string unsafeGpuWorkQuarantineReason;
    };

    void AppendMergedDriverTelemetryMessage(
        std::string& telemetryMessage,
        const std::string& driverMessage)
    {
        if (driverMessage.empty())
            return;

        if (telemetryMessage.empty())
        {
            telemetryMessage = driverMessage;
            return;
        }

        if (telemetryMessage != driverMessage)
            telemetryMessage += std::string(kDriverTelemetryDelimiter) + driverMessage;
    }

    bool CaptureDriverQueueTelemetrySnapshot(
        const DriverImpl* impl,
        DriverQueueTelemetrySnapshot& snapshot,
        const bool waitForSnapshot)
    {
        if (impl == nullptr)
            return true;

        std::unique_lock<std::mutex> lock(impl->driverTelemetryMutex, std::defer_lock);
        if (waitForSnapshot)
        {
            lock.lock();
        }
        else if (!lock.try_lock())
        {
            return false;
        }

        snapshot.queueOperationFailureCount = impl->queueOperationFailureCount;
        snapshot.lastQueueOperationFailure = impl->lastQueueOperationFailure;
        snapshot.currentFrameQueueOperationFailureCount =
            impl->currentFrameQueueOperationFailureCount;
        snapshot.currentFrameLastQueueOperationFailure =
            impl->currentFrameLastQueueOperationFailure;
        snapshot.deviceLostDetected = impl->deviceLostDetected.load(std::memory_order_acquire);
        snapshot.deviceLostReason = impl->deviceLostReason;
        snapshot.unsafeGpuWorkQuarantined =
            impl->unsafeGpuWorkQuarantined.load(std::memory_order_acquire);
        snapshot.unsafeGpuWorkQuarantineReason = impl->unsafeGpuWorkQuarantineReason;
        return true;
    }

    void RememberCompletedReadbackTexture(
        DriverImpl& impl,
        const std::shared_ptr<Render::RHI::RHITexture>& texture,
        uint64_t generation = 0u)
    {
        std::lock_guard lock(impl.completedReadbackTextureMutex);
        if (texture != nullptr)
        {
            impl.submittedReadbackTextureGenerations.erase(
                std::remove_if(
                    impl.submittedReadbackTextureGenerations.begin(),
                    impl.submittedReadbackTextureGenerations.end(),
                    [](const CompletedReadbackTextureRecord& record)
                    {
                        return record.texture.expired();
                    }),
                impl.submittedReadbackTextureGenerations.end());

            const auto submittedIt = generation != 0u
                ? std::find_if(
                    impl.submittedReadbackTextureGenerations.begin(),
                    impl.submittedReadbackTextureGenerations.end(),
                    [&texture, generation](const CompletedReadbackTextureRecord& record)
                    {
                        return record.generation == generation && record.texture.lock() == texture;
                    })
                : std::find_if(
                    impl.submittedReadbackTextureGenerations.begin(),
                    impl.submittedReadbackTextureGenerations.end(),
                    [&texture](const CompletedReadbackTextureRecord& record)
                    {
                        return record.texture.lock() == texture;
                    });
            if (submittedIt != impl.submittedReadbackTextureGenerations.end())
            {
                if (generation == 0u)
                    generation = submittedIt->generation;
                impl.submittedReadbackTextureGenerations.erase(submittedIt);
            }
        }

        impl.completedReadbackTexture = texture;
        impl.completedReadbackTextureGeneration = generation;
        if (texture == nullptr)
            return;

        impl.completedReadbackTextureHistory.erase(
            std::remove_if(
                impl.completedReadbackTextureHistory.begin(),
                impl.completedReadbackTextureHistory.end(),
                [&texture, generation](const CompletedReadbackTextureRecord& record)
                {
                    const auto lockedTexture = record.texture.lock();
                    return lockedTexture == nullptr ||
                        (lockedTexture == texture && record.generation == generation);
                }),
            impl.completedReadbackTextureHistory.end());

        impl.completedReadbackTextureHistory.push_back({ texture, generation });
        constexpr size_t kMaxCompletedReadbackHistory = 8u;
        while (impl.completedReadbackTextureHistory.size() > kMaxCompletedReadbackHistory)
            impl.completedReadbackTextureHistory.erase(impl.completedReadbackTextureHistory.begin());
    }

    bool HasRememberedReadbackTexture(
        const DriverImpl& impl,
        const std::shared_ptr<Render::RHI::RHITexture>& texture)
    {
        std::lock_guard lock(impl.completedReadbackTextureMutex);
        if (impl.completedReadbackTexture == texture)
            return true;

        return std::any_of(
            impl.completedReadbackTextureHistory.begin(),
            impl.completedReadbackTextureHistory.end(),
            [&texture](const CompletedReadbackTextureRecord& record)
            {
                return record.texture.lock() == texture;
            });
    }

    bool HasRememberedReadbackTexture(
        const DriverImpl& impl,
        const std::shared_ptr<Render::RHI::RHITexture>& texture,
        const uint64_t generation)
    {
        if (generation == 0u)
            return HasRememberedReadbackTexture(impl, texture);

        std::lock_guard lock(impl.completedReadbackTextureMutex);
        if (impl.completedReadbackTexture == texture &&
            impl.completedReadbackTextureGeneration == generation)
        {
            return true;
        }

        return std::any_of(
            impl.completedReadbackTextureHistory.begin(),
            impl.completedReadbackTextureHistory.end(),
            [&texture, generation](const CompletedReadbackTextureRecord& record)
            {
                return record.generation == generation && record.texture.lock() == texture;
            });
    }

    void ForgetRememberedReadbackTexture(
        DriverImpl& impl,
        const std::shared_ptr<Render::RHI::RHITexture>& texture)
    {
        if (texture == nullptr)
            return;

        std::lock_guard lock(impl.completedReadbackTextureMutex);
        if (impl.completedReadbackTexture == texture)
            impl.completedReadbackTexture.reset();

        impl.completedReadbackTextureHistory.erase(
            std::remove_if(
                impl.completedReadbackTextureHistory.begin(),
                impl.completedReadbackTextureHistory.end(),
                [&texture](const CompletedReadbackTextureRecord& record)
                {
                    const auto lockedTexture = record.texture.lock();
                    return lockedTexture == nullptr || lockedTexture == texture;
                }),
            impl.completedReadbackTextureHistory.end());
    }

    bool AppendDriverTelemetry(
        const DriverImpl* impl,
        ThreadedFrameTelemetry& telemetry,
        const bool waitForSnapshot)
    {
        DriverQueueTelemetrySnapshot snapshot;
        if (!CaptureDriverQueueTelemetrySnapshot(impl, snapshot, waitForSnapshot))
            return false;

        const auto mergeFailureTelemetry = [](
            const uint64_t driverFailureCount,
            const std::string& driverLastFailure,
            uint64_t& telemetryFailureCount,
            std::string& telemetryLastFailure)
        {
            if (driverFailureCount == 0u && driverLastFailure.empty())
                return;

            if (driverFailureCount > telemetryFailureCount)
            {
                telemetryFailureCount = driverFailureCount;
                telemetryLastFailure = driverLastFailure;
                return;
            }

            if (!driverLastFailure.empty())
                AppendMergedDriverTelemetryMessage(telemetryLastFailure, driverLastFailure);
        };

        mergeFailureTelemetry(
            snapshot.queueOperationFailureCount,
            snapshot.lastQueueOperationFailure,
            telemetry.queueOperationFailureCount,
            telemetry.lastQueueOperationFailure);
        mergeFailureTelemetry(
            snapshot.currentFrameQueueOperationFailureCount,
            snapshot.currentFrameLastQueueOperationFailure,
            telemetry.currentFrameQueueOperationFailureCount,
            telemetry.currentFrameLastQueueOperationFailure);

        if (snapshot.deviceLostDetected)
        {
            telemetry.deviceLostDetected = true;
            if (telemetry.deviceLostReason.empty())
            {
                telemetry.deviceLostReason = snapshot.deviceLostReason;
            }
            else
            {
                AppendMergedDriverTelemetryMessage(telemetry.deviceLostReason, snapshot.deviceLostReason);
            }
        }
        if (snapshot.unsafeGpuWorkQuarantined)
        {
            telemetry.unsafeGpuWorkQuarantined = true;
            if (telemetry.unsafeGpuWorkQuarantineReason.empty())
            {
                telemetry.unsafeGpuWorkQuarantineReason = snapshot.unsafeGpuWorkQuarantineReason;
            }
            else
            {
                AppendMergedDriverTelemetryMessage(
                    telemetry.unsafeGpuWorkQuarantineReason,
                    snapshot.unsafeGpuWorkQuarantineReason);
            }
        }

        return true;
    }

    constexpr auto kThreadedWorkerWakeTimeout = std::chrono::milliseconds(250);
    constexpr auto kThreadedDrainWakeTimeout = std::chrono::milliseconds(1);

    void AppendPassCommandInput(
        RenderScenePackage& package,
        const RenderPassCommandKind kind,
        const uint64_t drawCount,
        size_t& nextRecordedDrawCommandIndex)
    {
        if (drawCount == 0u)
            return;

        RenderPassCommandInput input;
        input.kind = kind;
        input.drawCount = drawCount;
        input.requiresFrameData = true;
        input.requiresObjectData = drawCount > 0u;
        input.requiresLightingData = false;
        input.targetsSwapchain = package.targetsSwapchain;
        input.renderWidth = package.renderWidth;
        input.renderHeight = package.renderHeight;
        input.clearColorValue = package.clearColorValue;
        input.clearColor = kind == RenderPassCommandKind::Opaque && package.clearColorBuffer;
        input.clearDepth = kind == RenderPassCommandKind::Opaque && package.clearDepthBuffer;
        input.clearStencil = kind == RenderPassCommandKind::Opaque && package.clearStencilBuffer;
        input.usesColorAttachment = true;
        input.usesDepthStencilAttachment = kind != RenderPassCommandKind::Compute;
        input.writesDepthStencilAttachment =
            kind == RenderPassCommandKind::Opaque ||
            kind == RenderPassCommandKind::Skybox ||
            input.clearDepth ||
            input.clearStencil;

        if (!package.recordedDrawCommands.empty() &&
            nextRecordedDrawCommandIndex < package.recordedDrawCommands.size())
        {
            const auto availableDrawCount = package.recordedDrawCommands.size() - nextRecordedDrawCommandIndex;
            const auto copiedDrawCount = std::min<size_t>(static_cast<size_t>(drawCount), availableDrawCount);
            input.recordedDrawCommands.insert(
                input.recordedDrawCommands.end(),
                package.recordedDrawCommands.begin() + static_cast<std::ptrdiff_t>(nextRecordedDrawCommandIndex),
                package.recordedDrawCommands.begin() + static_cast<std::ptrdiff_t>(nextRecordedDrawCommandIndex + copiedDrawCount));
            nextRecordedDrawCommandIndex += copiedDrawCount;
        }

        package.passCommandInputs.push_back(input);
    }

    bool IsPassRecordable(const RenderScenePackage& package, const RenderPassCommandInput& input)
    {
        if (input.kind == RenderPassCommandKind::UIOverlay)
            return false;

        if (input.kind == RenderPassCommandKind::Compute)
        {
            if (input.computeDispatchInputs.empty())
                return false;
        }
        else if (input.drawCount == 0u &&
            input.recordedDrawCommands.empty() &&
            !input.usesColorAttachment &&
            !input.usesDepthStencilAttachment)
        {
            return false;
        }

        if (input.requiresFrameData && !package.frameDataReady)
            return false;
        if (input.requiresObjectData && !package.objectDataReady)
            return false;
        if (input.requiresLightingData && !package.lightingDataReady)
            return false;

        return true;
    }

    const char* ToPassDebugName(const RenderPassCommandKind kind)
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

    const char* ResolvePassProfileScopeName(const RenderPassCommandInput& input)
    {
        return !input.debugName.empty()
            ? input.debugName.c_str()
            : ToPassDebugName(input.kind);
    }

    bool ShouldLogThreadedRenderingDiagnostics()
    {
        return Render::Settings::GetThreadDiagnosticsSettings().logRenderDrawPath;
    }

    void AppendTextureVisibilityTransitionBarrier(
        Render::RHI::RHIBarrierDesc& barriers,
        const TextureVisibilityTransition& transition)
    {
        if (transition.texture == nullptr)
            return;

        barriers.textureBarriers.push_back({
            transition.texture,
            transition.before,
            transition.after,
            transition.subresourceRange,
            transition.sourceStages,
            transition.destinationStages,
            transition.sourceAccess,
            transition.destinationAccess
        });
    }

    void LogSkippedPass(const RenderScenePackage& package, const RenderPassCommandInput& passInput, const char* reason)
    {
        if (!ShouldLogThreadedRenderingDiagnostics())
            return;

        const auto passName = ResolvePassProfileScopeName(passInput);
        const auto message = std::string("[Driver] Skipped pass: ") + passName
            + " reason=" + reason
            + " frameId=" + std::to_string(package.frameId)
            + " drawCount=" + std::to_string(passInput.drawCount)
            + " recordedDraws=" + std::to_string(passInput.recordedDrawCommands.size());
        NLS_LOG_WARNING(message.c_str());
    }

    bool BeginPassCommandPlan(
        Render::RHI::RHICommandBuffer& commandBuffer,
        const std::shared_ptr<Render::RHI::RHITextureView>& swapchainBackbufferView,
        const std::shared_ptr<Render::RHI::RHITextureView>& swapchainDepthStencilView,
        const RenderPassCommandInput& input,
        Render::RHI::RHIFrameContext* frameContext = nullptr)
    {
        if (!commandBuffer.IsRecording())
        {
            if (ShouldLogThreadedRenderingDiagnostics())
            {
                const auto passName = std::string(ResolvePassProfileScopeName(input));
                NLS_LOG_WARNING("[Driver] BeginPassCommandPlan failed: command buffer is not recording for pass " + passName);
            }
            return false;
        }

        const bool useResourceStateTracker =
            frameContext != nullptr &&
            frameContext->resourceStateTracker != nullptr;

        Render::RHI::RHIRenderPassDesc renderPassDesc;
        renderPassDesc.renderArea = { 0, 0, input.renderWidth, input.renderHeight };
        renderPassDesc.debugName = ResolvePassProfileScopeName(input);
        renderPassDesc.attachmentsRequireExternalStateTransitions = useResourceStateTracker;

        if (input.usesColorAttachment)
        {
            // Use custom color attachment views if provided (e.g., for Deferred GBuffer pass)
            if (!input.colorAttachmentViews.empty())
            {
                for (const auto& view : input.colorAttachmentViews)
                {
                    Render::RHI::RHIRenderPassColorAttachmentDesc colorAttachment;
                    colorAttachment.loadOp = input.clearColor ? Render::RHI::LoadOp::Clear : Render::RHI::LoadOp::Load;
                    colorAttachment.storeOp = Render::RHI::StoreOp::Store;
                    colorAttachment.clearValue = {
                        input.clearColorValue.x,
                        input.clearColorValue.y,
                        input.clearColorValue.z,
                        input.clearColorValue.w
                    };
                    colorAttachment.view = view;
                    renderPassDesc.colorAttachments.push_back(std::move(colorAttachment));
                }
            }
            else if (input.targetsSwapchain)
            {
                if (swapchainBackbufferView == nullptr)
                {
                    if (ShouldLogThreadedRenderingDiagnostics())
                    {
                        const auto passName = std::string(ResolvePassProfileScopeName(input));
                        NLS_LOG_WARNING("[Driver] BeginPassCommandPlan failed: swapchain backbuffer view is null for pass " + passName);
                    }
                    return false;
                }

                Render::RHI::RHIRenderPassColorAttachmentDesc colorAttachment;
                colorAttachment.loadOp = input.clearColor ? Render::RHI::LoadOp::Clear : Render::RHI::LoadOp::Load;
                colorAttachment.storeOp = Render::RHI::StoreOp::Store;
                colorAttachment.clearValue = {
                    input.clearColorValue.x,
                    input.clearColorValue.y,
                    input.clearColorValue.z,
                    input.clearColorValue.w
                };
                colorAttachment.view = swapchainBackbufferView;
                renderPassDesc.colorAttachments.push_back(std::move(colorAttachment));
            }
        }

        const auto depthStencilAttachmentView =
            input.depthStencilAttachmentView != nullptr
                ? input.depthStencilAttachmentView
                : (input.targetsSwapchain ? swapchainDepthStencilView : nullptr);
        if (input.usesDepthStencilAttachment && depthStencilAttachmentView != nullptr)
        {
            Render::RHI::RHIRenderPassDepthStencilAttachmentDesc depthAttachment;
            depthAttachment.depthLoadOp = input.clearDepth ? Render::RHI::LoadOp::Clear : Render::RHI::LoadOp::Load;
            depthAttachment.depthStoreOp = Render::RHI::StoreOp::Store;
            depthAttachment.stencilLoadOp = input.clearStencil ? Render::RHI::LoadOp::Clear : Render::RHI::LoadOp::DontCare;
            depthAttachment.stencilStoreOp = Render::RHI::StoreOp::Store;
            depthAttachment.clearValue.depth = 1.0f;
            depthAttachment.clearValue.stencil = 0u;
            depthAttachment.view = depthStencilAttachmentView;
            depthAttachment.readOnlyDepthStencil = !input.writesDepthStencilAttachment;
            renderPassDesc.depthStencilAttachment = std::move(depthAttachment);
        }

        if (useResourceStateTracker)
        {
            Render::RHI::RHIBarrierDesc attachmentBarriers;
            attachmentBarriers.textureBarriers.reserve(
                renderPassDesc.colorAttachments.size() +
                (renderPassDesc.depthStencilAttachment.has_value() ? 1u : 0u));

            for (const auto& colorAttachment : renderPassDesc.colorAttachments)
            {
                if (colorAttachment.view == nullptr || colorAttachment.view->GetTexture() == nullptr)
                    continue;

                attachmentBarriers.textureBarriers.push_back({
                    colorAttachment.view->GetTexture(),
                    Render::RHI::ResourceState::Unknown,
                    Render::RHI::ResourceState::RenderTarget,
                    colorAttachment.view->GetDesc().subresourceRange,
                    Render::RHI::PipelineStageMask::AllCommands,
                    Render::RHI::PipelineStageMask::RenderTarget,
                    Render::RHI::AccessMask::MemoryRead | Render::RHI::AccessMask::MemoryWrite,
                    Render::RHI::AccessMask::ColorAttachmentRead | Render::RHI::AccessMask::ColorAttachmentWrite
                });
            }

            if (renderPassDesc.depthStencilAttachment.has_value() &&
                renderPassDesc.depthStencilAttachment->view != nullptr &&
                renderPassDesc.depthStencilAttachment->view->GetTexture() != nullptr)
            {
                const auto depthStencilState = input.writesDepthStencilAttachment
                    ? Render::RHI::ResourceState::DepthWrite
                    : Render::RHI::ResourceState::DepthRead;
                const auto depthStencilDestinationAccess = input.writesDepthStencilAttachment
                    ? (Render::RHI::AccessMask::DepthStencilRead |
                        Render::RHI::AccessMask::DepthStencilWrite)
                    : Render::RHI::AccessMask::DepthStencilRead;
                attachmentBarriers.textureBarriers.push_back({
                    renderPassDesc.depthStencilAttachment->view->GetTexture(),
                    Render::RHI::ResourceState::Unknown,
                    depthStencilState,
                    renderPassDesc.depthStencilAttachment->view->GetDesc().subresourceRange,
                    Render::RHI::PipelineStageMask::AllCommands,
                    Render::RHI::PipelineStageMask::DepthStencil,
                    Render::RHI::AccessMask::MemoryRead | Render::RHI::AccessMask::MemoryWrite,
                    depthStencilDestinationAccess
                });
            }

            if (!attachmentBarriers.textureBarriers.empty())
            {
                FrameGraph::FrameGraphExecutionContext executionContext{
                    Context::RequireLocatedDriver("BeginPassCommandPlan"),
                    nullptr,
                    &commandBuffer,
                    frameContext
                };
                executionContext.RecordResourceBarriers(attachmentBarriers);
            }
        }

        commandBuffer.BeginRenderPass(renderPassDesc);
        commandBuffer.SetViewport({
            0.0f,
            0.0f,
            static_cast<float>(input.renderWidth),
            static_cast<float>(input.renderHeight),
            0.0f,
            1.0f
        });
        return true;
    }

    void EndPassCommandPlan(
        Render::RHI::RHICommandBuffer& commandBuffer,
        const RenderPassCommandInput* input = nullptr,
        Render::RHI::RHIFrameContext* frameContext = nullptr)
    {
        if (commandBuffer.IsRecording())
            commandBuffer.EndRenderPass();

        if (input == nullptr ||
            frameContext == nullptr ||
            frameContext->resourceStateTracker == nullptr)
        {
            return;
        }

        Render::RHI::RHIBarrierDesc attachmentBarriers;

        if (input->targetsSwapchain)
        {
            const auto& view = frameContext->swapchainBackbufferView;
            if (view != nullptr && view->GetTexture() != nullptr)
            {
                attachmentBarriers.textureBarriers.push_back({
                    view->GetTexture(),
                    Render::RHI::ResourceState::Unknown,
                    Render::RHI::ResourceState::Present,
                    view->GetDesc().subresourceRange,
                    Render::RHI::PipelineStageMask::RenderTarget,
                    Render::RHI::PipelineStageMask::Present,
                    Render::RHI::AccessMask::ColorAttachmentRead | Render::RHI::AccessMask::ColorAttachmentWrite,
                    Render::RHI::AccessMask::Present
                });
            }
        }
        else
        {
            for (const auto& colorView : input->colorAttachmentViews)
            {
                const auto transition = FrameGraph::BuildSampledAttachmentEndTransition(colorView, false);
                if (transition.has_value())
                    AppendTextureVisibilityTransitionBarrier(attachmentBarriers, *transition);
            }

            const auto transition = FrameGraph::BuildSampledAttachmentEndTransition(input->depthStencilAttachmentView, true);
            if (transition.has_value())
                AppendTextureVisibilityTransitionBarrier(attachmentBarriers, *transition);
        }
        if (!attachmentBarriers.textureBarriers.empty())
        {
            FrameGraph::FrameGraphExecutionContext executionContext{
                Context::RequireLocatedDriver("EndPassCommandPlan"),
                nullptr,
                &commandBuffer,
                frameContext
            };
            executionContext.RecordResourceBarriers(attachmentBarriers);
        }
    }

    bool HasResourceVisibilityTransitions(const RenderPassCommandInput& input)
    {
        return !input.bufferVisibilityTransitions.empty() ||
            !input.textureVisibilityTransitions.empty();
    }

    bool HasIncomingResourceDependencyEdges(const ParallelCommandWorkUnit& workUnit)
    {
        return std::any_of(
            workUnit.incomingDependencyEdges.begin(),
            workUnit.incomingDependencyEdges.end(),
            [](const WorkUnitDependencyEdge& edge)
            {
                return edge.resourceKind != ThreadedDependencyResourceKind::None;
            });
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

    Render::RHI::ResourceState AddReadStatesImpliedByAccess(
        Render::RHI::ResourceState state,
        const Render::RHI::AccessMask accessMask)
    {
        const auto access = static_cast<uint32_t>(accessMask);
        if ((access & static_cast<uint32_t>(Render::RHI::AccessMask::ShaderRead)) != 0u)
            state = state | Render::RHI::ResourceState::ShaderRead;
        if ((access & static_cast<uint32_t>(Render::RHI::AccessMask::DepthStencilRead)) != 0u)
            state = state | Render::RHI::ResourceState::DepthRead;
        return state;
    }

    const BufferResourceAccess* FindSourceBufferWriteAccess(
        const RenderPassCommandInput& input,
        const std::shared_ptr<Render::RHI::RHIBuffer>& buffer)
    {
        const auto it = std::find_if(
            input.bufferResourceAccesses.rbegin(),
            input.bufferResourceAccesses.rend(),
            [&buffer](const BufferResourceAccess& access)
            {
                return access.mode == ResourceAccessMode::Write &&
                    access.buffer == buffer;
            });
        return it != input.bufferResourceAccesses.rend() ? &(*it) : nullptr;
    }

    const TextureResourceAccess* FindSourceTextureWriteAccess(
        const RenderPassCommandInput& input,
        const std::shared_ptr<Render::RHI::RHITexture>& texture,
        const Render::RHI::RHISubresourceRange& subresourceRange)
    {
        const auto it = std::find_if(
            input.textureResourceAccesses.rbegin(),
            input.textureResourceAccesses.rend(),
            [&texture, &subresourceRange](const TextureResourceAccess& access)
            {
                if (access.mode != ResourceAccessMode::Write ||
                    access.texture != texture ||
                    texture == nullptr)
                {
                    return false;
                }

                const auto sourceRange = Render::RHI::NormalizeTextureSubresourceRange(
                    texture->GetDesc(),
                    access.subresourceRange);
                const auto requestedRange = Render::RHI::NormalizeTextureSubresourceRange(
                    texture->GetDesc(),
                    subresourceRange);
                return sourceRange.has_value() &&
                    requestedRange.has_value() &&
                    Render::RHI::DoesSubresourceRangeCover(*sourceRange, *requestedRange);
            });
        return it != input.textureResourceAccesses.rend() ? &(*it) : nullptr;
    }

    const BufferVisibilityTransition* FindExportedBufferVisibilityTransition(
        const RenderPassCommandInput& input,
        const std::shared_ptr<Render::RHI::RHIBuffer>& buffer)
    {
        const auto it = std::find_if(
            input.exportedBufferVisibilityTransitions.begin(),
            input.exportedBufferVisibilityTransitions.end(),
            [&buffer](const BufferVisibilityTransition& transition)
            {
                return transition.buffer == buffer;
            });
        return it != input.exportedBufferVisibilityTransitions.end() ? &(*it) : nullptr;
    }

    const TextureVisibilityTransition* FindExportedTextureVisibilityTransition(
        const RenderPassCommandInput& input,
        const std::shared_ptr<Render::RHI::RHITexture>& texture,
        const Render::RHI::RHISubresourceRange& subresourceRange)
    {
        const auto it = std::find_if(
            input.exportedTextureVisibilityTransitions.begin(),
            input.exportedTextureVisibilityTransitions.end(),
            [&texture, &subresourceRange](const TextureVisibilityTransition& transition)
            {
                if (transition.texture != texture || texture == nullptr)
                    return false;

                const auto sourceRange = Render::RHI::NormalizeTextureSubresourceRange(
                    texture->GetDesc(),
                    transition.subresourceRange);
                const auto requestedRange = Render::RHI::NormalizeTextureSubresourceRange(
                    texture->GetDesc(),
                    subresourceRange);
                return sourceRange.has_value() &&
                    requestedRange.has_value() &&
                    Render::RHI::DoesSubresourceRangeCover(*sourceRange, *requestedRange);
            });
        return it != input.exportedTextureVisibilityTransitions.end() ? &(*it) : nullptr;
    }

    std::optional<TextureVisibilityTransition> BuildAttachmentEndVisibilityTransition(
        const RenderPassCommandInput& input,
        const std::shared_ptr<Render::RHI::RHITexture>& texture,
        const Render::RHI::RHISubresourceRange& subresourceRange)
    {
        if (texture == nullptr || input.targetsSwapchain)
            return std::nullopt;

        const auto matchesViewTexture =
            [&texture, &subresourceRange](const std::shared_ptr<Render::RHI::RHITextureView>& view)
        {
            return view != nullptr &&
                view->GetTexture() == texture &&
                Render::RHI::DoesSubresourceRangeOverlap(view->GetDesc().subresourceRange, subresourceRange);
        };

        for (const auto& colorView : input.colorAttachmentViews)
        {
            if (!matchesViewTexture(colorView))
                continue;

            auto transition = FrameGraph::BuildSampledAttachmentEndTransition(colorView, false);
            if (!transition.has_value())
                continue;

            transition->subresourceRange = !Render::RHI::IsEmptySubresourceRange(subresourceRange)
                ? subresourceRange
                : colorView->GetDesc().subresourceRange;
            return transition;
        }

        if (matchesViewTexture(input.depthStencilAttachmentView))
        {
            auto transition = FrameGraph::BuildSampledAttachmentEndTransition(input.depthStencilAttachmentView, true);
            if (!transition.has_value())
                return std::nullopt;

            transition->subresourceRange = !Render::RHI::IsEmptySubresourceRange(subresourceRange)
                ? subresourceRange
                : input.depthStencilAttachmentView->GetDesc().subresourceRange;
            return transition;
        }

        return std::nullopt;
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

    std::optional<BufferVisibilityTransition> BuildDerivedBufferVisibilityTransition(
        const RenderPassCommandInput& sourceInput,
        const BufferResourceAccess& targetAccess)
    {
        if (targetAccess.mode != ResourceAccessMode::Read || targetAccess.buffer == nullptr)
            return std::nullopt;

        const auto* sourceWriteAccess = FindSourceBufferWriteAccess(sourceInput, targetAccess.buffer);
        const auto* exportedTransition = FindExportedBufferVisibilityTransition(sourceInput, targetAccess.buffer);
        if (sourceWriteAccess == nullptr && exportedTransition == nullptr)
            return std::nullopt;

        BufferVisibilityTransition transition;
        transition.buffer = targetAccess.buffer;
        transition.before = sourceWriteAccess != nullptr
            ? sourceWriteAccess->state
            : exportedTransition->before;
        transition.after = targetAccess.state != Render::RHI::ResourceState::Unknown
            ? targetAccess.state
            : (exportedTransition != nullptr ? exportedTransition->after : Render::RHI::ResourceState::Unknown);
        transition.sourceStages = sourceWriteAccess != nullptr
            ? sourceWriteAccess->stages
            : exportedTransition->sourceStages;
        transition.destinationStages = targetAccess.stages != Render::RHI::PipelineStageMask::None
            ? targetAccess.stages
            : (exportedTransition != nullptr ? exportedTransition->destinationStages : Render::RHI::PipelineStageMask::None);
        transition.sourceAccess = sourceWriteAccess != nullptr
            ? sourceWriteAccess->access
            : exportedTransition->sourceAccess;
        transition.destinationAccess = targetAccess.access != Render::RHI::AccessMask::None
            ? targetAccess.access
            : (exportedTransition != nullptr ? exportedTransition->destinationAccess : Render::RHI::AccessMask::None);
        return transition;
    }

    std::optional<TextureVisibilityTransition> BuildDerivedTextureVisibilityTransition(
        const RenderPassCommandInput& sourceInput,
        const TextureResourceAccess& targetAccess)
    {
        if (targetAccess.mode != ResourceAccessMode::Read || targetAccess.texture == nullptr)
            return std::nullopt;

        const auto* sourceWriteAccess = FindSourceTextureWriteAccess(
            sourceInput,
            targetAccess.texture,
            targetAccess.subresourceRange);
        const auto* exportedTransition = FindExportedTextureVisibilityTransition(
            sourceInput,
            targetAccess.texture,
            targetAccess.subresourceRange);
        const auto attachmentEndTransition =
            BuildAttachmentEndVisibilityTransition(
                sourceInput,
                targetAccess.texture,
                targetAccess.subresourceRange);
        const auto* completedSourceTransition =
            exportedTransition != nullptr
                ? exportedTransition
                : (attachmentEndTransition.has_value() ? &attachmentEndTransition.value() : nullptr);
        const auto* attachmentCompletedTransition =
            exportedTransition == nullptr && attachmentEndTransition.has_value()
                ? &attachmentEndTransition.value()
                : nullptr;
        if (sourceWriteAccess == nullptr && completedSourceTransition == nullptr)
            return std::nullopt;

        TextureVisibilityTransition transition;
        transition.texture = targetAccess.texture;
        transition.subresourceRange = !Render::RHI::IsEmptySubresourceRange(targetAccess.subresourceRange)
            ? targetAccess.subresourceRange
            : (completedSourceTransition != nullptr ? completedSourceTransition->subresourceRange : Render::RHI::RHISubresourceRange{});
        // Preserve inferred sampled attachment end state when no explicit export exists.
        transition.before = attachmentCompletedTransition != nullptr
            ? attachmentCompletedTransition->after
            : (completedSourceTransition != nullptr
                ? AddReadStatesImpliedByAccess(completedSourceTransition->after, completedSourceTransition->destinationAccess)
                : (sourceWriteAccess != nullptr ? sourceWriteAccess->state : Render::RHI::ResourceState::Unknown));
        transition.after = targetAccess.state != Render::RHI::ResourceState::Unknown
            ? targetAccess.state
            : (completedSourceTransition != nullptr ? completedSourceTransition->after : Render::RHI::ResourceState::Unknown);
        transition.sourceStages = completedSourceTransition != nullptr
            ? completedSourceTransition->destinationStages
            : sourceWriteAccess->stages;
        transition.destinationStages = targetAccess.stages != Render::RHI::PipelineStageMask::None
            ? targetAccess.stages
            : (completedSourceTransition != nullptr ? completedSourceTransition->destinationStages : Render::RHI::PipelineStageMask::None);
        transition.sourceAccess = completedSourceTransition != nullptr
            ? completedSourceTransition->destinationAccess
            : sourceWriteAccess->access;
        transition.destinationAccess = targetAccess.access != Render::RHI::AccessMask::None
            ? targetAccess.access
            : (completedSourceTransition != nullptr ? completedSourceTransition->destinationAccess : Render::RHI::AccessMask::None);
        if (transition.before == transition.after &&
            !HasWriteAccess(transition.sourceAccess) &&
            !HasWriteAccess(transition.destinationAccess))
        {
            return std::nullopt;
        }
        return transition;
    }

    void PopulateVisibilityTransitionsFromResourceUsage(
        std::vector<ParallelCommandWorkUnit>& workUnits)
    {
        for (size_t targetIndex = 0u; targetIndex < workUnits.size(); ++targetIndex)
        {
            auto& workUnit = workUnits[targetIndex];
            if (HasIncomingResourceDependencyEdges(workUnit))
                continue;

            auto& targetInput = workUnit.commandInput;
            const auto implicitSourceIndex =
                Detail::ResolveImplicitDependencySourceWorkUnitIndex(workUnit);
            if (!implicitSourceIndex.has_value())
                continue;

            const auto sourceIndex = static_cast<size_t>(*implicitSourceIndex);
            if (sourceIndex >= workUnits.size() || sourceIndex == targetIndex)
                continue;

            const auto& sourceInput = workUnits[sourceIndex].commandInput;

            for (const auto& targetAccess : targetInput.bufferResourceAccesses)
            {
                if (HasBufferVisibilityTransitionForResource(targetInput, targetAccess.buffer))
                    continue;

                const auto transition = BuildDerivedBufferVisibilityTransition(sourceInput, targetAccess);
                if (!transition.has_value())
                    continue;

                targetInput.bufferVisibilityTransitions.push_back(*transition);
                targetInput.requiresDependencyVisibility = true;
            }

            for (const auto& targetAccess : targetInput.textureResourceAccesses)
            {
                if (HasTextureVisibilityTransitionForResource(
                    targetInput,
                    targetAccess.texture,
                    targetAccess.subresourceRange))
                {
                    continue;
                }

                const auto transition = BuildDerivedTextureVisibilityTransition(sourceInput, targetAccess);
                if (!transition.has_value())
                    continue;

                targetInput.textureVisibilityTransitions.push_back(*transition);
                targetInput.requiresDependencyVisibility = true;
            }
        }
    }

    bool RecordResourceVisibilityTransitions(
        Render::RHI::RHICommandBuffer& commandBuffer,
        const RenderPassCommandInput& input,
        Render::RHI::RHIFrameContext* frameContext = nullptr)
    {
        const bool useResourceStateTracker =
            frameContext != nullptr &&
            frameContext->resourceStateTracker != nullptr;
        Render::RHI::RHIBarrierDesc barriers;
        barriers.bufferBarriers.reserve(input.bufferVisibilityTransitions.size());
        barriers.textureBarriers.reserve(input.textureVisibilityTransitions.size());

        for (const auto& transition : input.bufferVisibilityTransitions)
        {
            if (transition.buffer == nullptr)
                continue;

            auto before = transition.before;
            if (!useResourceStateTracker && before == Render::RHI::ResourceState::Unknown)
                before = transition.buffer->GetState();
            barriers.bufferBarriers.push_back({
                transition.buffer,
                before,
                transition.after,
                transition.sourceStages,
                transition.destinationStages,
                transition.sourceAccess,
                transition.destinationAccess
            });
        }

        for (const auto& transition : input.textureVisibilityTransitions)
        {
            if (transition.texture == nullptr)
                continue;

            auto before = transition.before;
            if (!useResourceStateTracker && before == Render::RHI::ResourceState::Unknown)
                before = transition.texture->GetState();
            barriers.textureBarriers.push_back({
                transition.texture,
                before,
                transition.after,
                transition.subresourceRange,
                transition.sourceStages,
                transition.destinationStages,
                transition.sourceAccess,
                transition.destinationAccess
            });
        }

        if (barriers.bufferBarriers.empty() && barriers.textureBarriers.empty())
            return false;

        if (useResourceStateTracker)
        {
            FrameGraph::FrameGraphExecutionContext executionContext{
                Context::RequireLocatedDriver("RecordResourceVisibilityTransitions"),
                nullptr,
                &commandBuffer,
                frameContext
            };
            return executionContext.RecordResourceBarriers(barriers).Succeeded();
        }

        return commandBuffer.BarrierChecked(barriers).Succeeded();
    }

    bool RecordPreparedDrawCommand(
        Render::RHI::RHICommandBuffer& commandBuffer,
        const RenderPassCommandInput& passInput,
        const RecordedDrawCommandInput& drawCommand)
    {
        if (drawCommand.pipeline == nullptr ||
            drawCommand.materialBindingSet == nullptr ||
            drawCommand.mesh == nullptr)
        {
            return false;
        }

        const auto& pipelineDesc = drawCommand.pipeline->GetDesc();
        if (ShouldLogThreadedRenderingDiagnostics() &&
            pipelineDesc.renderTargetLayout.hasDepth &&
            (!passInput.usesDepthStencilAttachment || passInput.depthStencilAttachmentView == nullptr))
        {
            const auto passName = std::string(ResolvePassProfileScopeName(passInput));
            NLS_LOG_WARNING(
                "[Driver] Depth PSO recorded without DSV: pass=" + passName +
                " pipeline=" + pipelineDesc.debugName +
                " usesDepth=" + std::to_string(passInput.usesDepthStencilAttachment ? 1 : 0) +
                " hasDepthView=" + std::to_string(passInput.depthStencilAttachmentView != nullptr ? 1 : 0) +
                " drawCount=" + std::to_string(passInput.drawCount) +
                " recordedDraws=" + std::to_string(passInput.recordedDrawCommands.size()));
        }

        commandBuffer.BindGraphicsPipeline(drawCommand.pipeline);
        if (drawCommand.frameBindingSet != nullptr)
            commandBuffer.BindBindingSet(::NLS::Render::RHI::BindingPointMap::kFrameDescriptorSet, drawCommand.frameBindingSet);
        if (drawCommand.objectBindingSet != nullptr)
            commandBuffer.BindBindingSet(::NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet, drawCommand.objectBindingSet);
        if (drawCommand.passBindingSet != nullptr)
            commandBuffer.BindBindingSet(::NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, drawCommand.passBindingSet);
        commandBuffer.BindBindingSet(::NLS::Render::RHI::BindingPointMap::kMaterialDescriptorSet, drawCommand.materialBindingSet);
        if (drawCommand.usesObjectIndex)
        {
            commandBuffer.PushConstants(
                ::NLS::Render::Resources::kIndexedObjectDataPushConstantStageMask,
                0u,
                ::NLS::Render::Resources::kIndexedObjectDataPushConstantSize,
                &drawCommand.objectConstants);
        }

        const auto vertexBuffer = drawCommand.mesh->GetVertexBuffer();
        if (vertexBuffer == nullptr)
            return false;

        commandBuffer.BindVertexBuffer(0, { vertexBuffer, 0, drawCommand.mesh->GetVertexStride() });

        const auto indexBuffer = drawCommand.mesh->GetIndexBuffer();
        const auto indexCount = drawCommand.mesh->GetIndexCount();
        if (indexBuffer != nullptr && indexCount > 0u)
        {
            commandBuffer.BindIndexBuffer({ indexBuffer, 0, drawCommand.mesh->GetIndexType() });
            const auto drawResult = commandBuffer.DrawIndexedChecked(indexCount, drawCommand.instanceCount, 0, 0, 0);
            if (!drawResult.Succeeded() && !drawResult.message.empty())
                NLS_LOG_ERROR(drawResult.message);
            return drawResult.Succeeded();
        }

        const auto meshVertexCount = drawCommand.mesh->GetVertexCount();
        const auto vertexStart = std::min(drawCommand.vertexStart, meshVertexCount);
        const auto vertexCount = drawCommand.vertexCount != 0u
            ? std::min(drawCommand.vertexCount, meshVertexCount - vertexStart)
            : meshVertexCount - vertexStart;
        if (vertexCount > 0u)
        {
            const auto drawResult = commandBuffer.DrawChecked(vertexCount, drawCommand.instanceCount, vertexStart, 0);
            if (!drawResult.Succeeded() && !drawResult.message.empty())
                NLS_LOG_ERROR(drawResult.message);
            return drawResult.Succeeded();
        }

        return false;
    }

    uint64_t RecordPreparedDrawCommandsForPass(
        Render::RHI::RHICommandBuffer* commandBuffer,
        const RenderPassCommandInput& input)
    {
        NLS_PROFILE_NAMED_SCOPE(ResolvePassProfileScopeName(input));

        if (input.kind == RenderPassCommandKind::UIOverlay)
            return 0u;

        if (commandBuffer == nullptr)
            return input.recordedDrawCommands.empty() ? input.drawCount : 0u;

        if (input.recordedDrawCommands.empty())
            return input.drawCount;

        uint64_t recordedDrawCount = 0u;
        for (const auto& recordedDrawCommand : input.recordedDrawCommands)
        {
            if (RecordPreparedDrawCommand(*commandBuffer, input, recordedDrawCommand))
            {
                ++recordedDrawCount;
            }
            else
            {
                break;
            }
        }
        return recordedDrawCount;
    }

    uint64_t RecordPreparedDrawCommandsForPassRange(
        Render::RHI::RHICommandBuffer* commandBuffer,
        const RenderPassCommandInput& input,
        const std::vector<RecordedDrawCommandInput>& recordedDrawCommands,
        const uint64_t recordedDrawBegin,
        const uint64_t recordedDrawCount)
    {
        NLS_PROFILE_NAMED_SCOPE(ResolvePassProfileScopeName(input));

        if (input.kind == RenderPassCommandKind::UIOverlay)
            return 0u;

        if (commandBuffer == nullptr)
            return recordedDrawCount;

        if (recordedDrawCommands.empty() || recordedDrawCount == 0u)
            return 0u;

        const auto recordedDrawCommandCount = static_cast<uint64_t>(recordedDrawCommands.size());
        if (recordedDrawBegin > recordedDrawCommandCount ||
            recordedDrawCount > recordedDrawCommandCount - recordedDrawBegin)
        {
            return 0u;
        }
        const auto recordedDrawEnd = recordedDrawBegin + recordedDrawCount;

        uint64_t recordedCount = 0u;
        for (uint64_t drawIndex = recordedDrawBegin; drawIndex < recordedDrawEnd; ++drawIndex)
        {
            if (RecordPreparedDrawCommand(
                *commandBuffer,
                input,
                recordedDrawCommands[static_cast<size_t>(drawIndex)]))
            {
                ++recordedCount;
            }
            else
            {
                break;
            }
        }
        return recordedCount;
    }

    class TrackedBindingSet final : public Render::RHI::RHIBindingSet
    {
    public:
        TrackedBindingSet(
            std::shared_ptr<Render::RHI::RHIBindingSet> innerBindingSet,
            std::shared_ptr<Render::RHI::DescriptorAllocator> descriptorAllocator,
            Render::RHI::DescriptorAllocation allocation)
            : m_innerBindingSet(std::move(innerBindingSet))
            , m_descriptorAllocator(std::move(descriptorAllocator))
            , m_allocation(std::move(allocation))
        {
        }

        ~TrackedBindingSet() override
        {
            if (m_descriptorAllocator != nullptr &&
                m_allocation.IsValid() &&
                m_allocation.lifetime == Render::RHI::DescriptorAllocationLifetime::Persistent)
            {
                m_descriptorAllocator->Release(m_allocation);
            }
        }

        std::string_view GetDebugName() const override
        {
            return m_innerBindingSet != nullptr
                ? m_innerBindingSet->GetDebugName()
                : std::string_view{};
        }

        const Render::RHI::RHIBindingSetDesc& GetDesc() const override
        {
            static const Render::RHI::RHIBindingSetDesc kEmptyDesc{};
            return m_innerBindingSet != nullptr
                ? m_innerBindingSet->GetDesc()
                : kEmptyDesc;
        }

        Render::RHI::NativeHandle GetNativeBindingSetHandle() const override
        {
            return m_innerBindingSet != nullptr
                ? m_innerBindingSet->GetNativeBindingSetHandle()
                : Render::RHI::NativeHandle{};
        }

        std::shared_ptr<Render::RHI::RHIBindingSet> GetWrappedBindingSetShared() override
        {
            return m_innerBindingSet;
        }

        std::shared_ptr<const Render::RHI::RHIBindingSet> GetWrappedBindingSetShared() const override
        {
            return m_innerBindingSet;
        }

    private:
        std::shared_ptr<Render::RHI::RHIBindingSet> m_innerBindingSet;
        std::shared_ptr<Render::RHI::DescriptorAllocator> m_descriptorAllocator;
        Render::RHI::DescriptorAllocation m_allocation{};
    };

    std::shared_ptr<Render::RHI::RHIBindingSet> CreateTrackedBindingSet(
        const std::shared_ptr<Render::RHI::RHIDevice>& explicitDevice,
        const std::shared_ptr<Render::RHI::DescriptorAllocator>& descriptorAllocator,
        const uint64_t frameIndex,
        const Render::RHI::RHIBindingSetDesc& desc,
        const Render::RHI::DescriptorAllocationLifetime allocationLifetime)
    {
        if (explicitDevice == nullptr)
            return nullptr;

        auto innerBindingSet = explicitDevice->CreateBindingSet(desc);
        if (innerBindingSet == nullptr)
            return nullptr;

        if (descriptorAllocator == nullptr)
        {
            NLS_LOG_WARNING(
                "CreateTrackedBindingSet: descriptor allocator missing for \"" +
                (!desc.debugName.empty() ? desc.debugName : std::string("TrackedBindingSet")) +
                "\" on the DX12-aligned mainline.");
            return nullptr;
        }

        Render::RHI::DescriptorAllocationRequest allocationRequest;
        allocationRequest.count = Render::RHI::CountBindingSetDescriptorSlots(desc);
        allocationRequest.lifetime = allocationLifetime;
        allocationRequest.frameIndex = frameIndex;
        allocationRequest.layout = desc.layout;
        allocationRequest.debugName = !desc.debugName.empty()
            ? desc.debugName
            : "TrackedBindingSet";

        const auto allocation = descriptorAllocator->Allocate(allocationRequest);
        if (!allocation.IsValid())
        {
            NLS_LOG_WARNING(
                "CreateTrackedBindingSet: descriptor allocation failed for \"" +
                allocationRequest.debugName + "\"");
            return nullptr;
        }

        return std::make_shared<TrackedBindingSet>(
            std::move(innerBindingSet),
            descriptorAllocator,
            allocation);
    }

    void RecordUavBarriersForBuffers(
        Render::RHI::RHICommandBuffer& commandBuffer,
        const std::vector<std::shared_ptr<Render::RHI::RHIBuffer>>& buffers)
    {
        if (buffers.empty())
            return;

        Render::RHI::RHIBarrierDesc barriers;
        barriers.bufferBarriers.reserve(buffers.size());

        for (const auto& buffer : buffers)
        {
            if (buffer == nullptr)
                continue;

            barriers.bufferBarriers.push_back({
                buffer,
                Render::RHI::ResourceState::ShaderWrite,
                Render::RHI::ResourceState::ShaderWrite,
                Render::RHI::PipelineStageMask::ComputeShader,
                Render::RHI::PipelineStageMask::ComputeShader,
                Render::RHI::AccessMask::ShaderWrite,
                Render::RHI::AccessMask::ShaderRead | Render::RHI::AccessMask::ShaderWrite
            });
        }

        if (!barriers.bufferBarriers.empty())
            commandBuffer.Barrier(barriers);
    }

    void RecordShaderReadBarriersForBuffers(
        Render::RHI::RHICommandBuffer& commandBuffer,
        const std::vector<std::shared_ptr<Render::RHI::RHIBuffer>>& buffers)
    {
        if (buffers.empty())
            return;

        Render::RHI::RHIBarrierDesc barriers;
        barriers.bufferBarriers.reserve(buffers.size());

        for (const auto& buffer : buffers)
        {
            if (buffer == nullptr)
                continue;

            barriers.bufferBarriers.push_back({
                buffer,
                Render::RHI::ResourceState::Unknown,
                Render::RHI::ResourceState::ShaderRead,
                Render::RHI::PipelineStageMask::AllCommands,
                Render::RHI::PipelineStageMask::ComputeShader,
                Render::RHI::AccessMask::MemoryRead | Render::RHI::AccessMask::MemoryWrite,
                Render::RHI::AccessMask::ShaderRead
            });
        }

        if (!barriers.bufferBarriers.empty())
            commandBuffer.Barrier(barriers);
    }

    void RecordShaderWriteBarriersForBuffers(
        Render::RHI::RHICommandBuffer& commandBuffer,
        const std::vector<std::shared_ptr<Render::RHI::RHIBuffer>>& buffers)
    {
        if (buffers.empty())
            return;

        Render::RHI::RHIBarrierDesc barriers;
        barriers.bufferBarriers.reserve(buffers.size());

        for (const auto& buffer : buffers)
        {
            if (buffer == nullptr)
                continue;

            barriers.bufferBarriers.push_back({
                buffer,
                Render::RHI::ResourceState::Unknown,
                Render::RHI::ResourceState::ShaderWrite,
                Render::RHI::PipelineStageMask::AllCommands,
                Render::RHI::PipelineStageMask::ComputeShader,
                Render::RHI::AccessMask::MemoryRead | Render::RHI::AccessMask::MemoryWrite,
                Render::RHI::AccessMask::ShaderWrite
            });
        }

        if (!barriers.bufferBarriers.empty())
            commandBuffer.Barrier(barriers);
    }

    void RecordShaderReadAfterWriteBarriersForBuffers(
        Render::RHI::RHICommandBuffer& commandBuffer,
        const std::vector<std::shared_ptr<Render::RHI::RHIBuffer>>& buffers)
    {
        if (buffers.empty())
            return;

        Render::RHI::RHIBarrierDesc barriers;
        barriers.bufferBarriers.reserve(buffers.size());

        for (const auto& buffer : buffers)
        {
            if (buffer == nullptr)
                continue;

            barriers.bufferBarriers.push_back({
                buffer,
                Render::RHI::ResourceState::ShaderWrite,
                Render::RHI::ResourceState::ShaderRead,
                Render::RHI::PipelineStageMask::ComputeShader,
                Render::RHI::PipelineStageMask::AllGraphics | Render::RHI::PipelineStageMask::ComputeShader,
                Render::RHI::AccessMask::ShaderWrite,
                Render::RHI::AccessMask::ShaderRead
            });
        }

        if (!barriers.bufferBarriers.empty())
            commandBuffer.Barrier(barriers);
    }

    void RecordTextureVisibilityTransitions(
        Render::RHI::RHICommandBuffer& commandBuffer,
        const std::vector<TextureVisibilityTransition>& transitions)
    {
        if (transitions.empty())
            return;

        Render::RHI::RHIBarrierDesc barriers;
        barriers.textureBarriers.reserve(transitions.size());

        for (const auto& transition : transitions)
        {
            if (transition.texture == nullptr)
                continue;

            barriers.textureBarriers.push_back({
                transition.texture,
                transition.before,
                transition.after,
                transition.subresourceRange,
                transition.sourceStages,
                transition.destinationStages,
                transition.sourceAccess,
                transition.destinationAccess
            });
        }

        if (!barriers.textureBarriers.empty())
            commandBuffer.Barrier(barriers);
    }

    uint64_t RecordComputeDispatches(
        Render::RHI::RHICommandBuffer& commandBuffer,
        const std::vector<RecordedComputeDispatchInput>& dispatchInputs,
        const bool recordShaderReadBarriers)
    {
        NLS_PROFILE_SCOPE();

        uint64_t recordedDispatchCount = 0u;
        for (const auto& dispatchInput : dispatchInputs)
        {
            if (dispatchInput.pipeline == nullptr)
                continue;

            if (recordShaderReadBarriers)
                RecordShaderReadBarriersForBuffers(commandBuffer, dispatchInput.shaderReadBuffersBefore);
            RecordShaderWriteBarriersForBuffers(commandBuffer, dispatchInput.shaderWriteBuffersBefore);
            RecordUavBarriersForBuffers(commandBuffer, dispatchInput.uavBarrierBuffersBefore);
            RecordTextureVisibilityTransitions(commandBuffer, dispatchInput.textureVisibilityTransitionsBefore);
            commandBuffer.BindComputePipeline(dispatchInput.pipeline);
            for (const auto& bindingSet : dispatchInput.bindingSets)
            {
                if (bindingSet.bindingSet != nullptr)
                    commandBuffer.BindBindingSet(bindingSet.setIndex, bindingSet.bindingSet);
            }

            commandBuffer.Dispatch(dispatchInput.groupCountX, dispatchInput.groupCountY, dispatchInput.groupCountZ);
            RecordUavBarriersForBuffers(commandBuffer, dispatchInput.uavBarrierBuffersAfter);
            RecordShaderReadAfterWriteBarriersForBuffers(commandBuffer, dispatchInput.shaderReadBuffersAfter);
            RecordTextureVisibilityTransitions(commandBuffer, dispatchInput.exportedTextureVisibilityTransitions);
            ++recordedDispatchCount;
        }

        return recordedDispatchCount;
    }

    bool ShouldLogExplicitDrawDiagnostics()
    {
        return Render::Settings::GetThreadDiagnosticsSettings().logRenderDrawPath;
    }

    bool ShouldBlockThreadedSnapshotPublishForPendingResize(
        const DriverImpl& impl,
        const FrameSnapshot& snapshot)
    {
        return impl.threadedLifecycle != nullptr &&
            impl.hasPendingSwapchainResize &&
            snapshot.targetsSwapchain;
    }

    bool SupportsThreadedFoundationExecution(const DriverImpl& impl)
    {
        if (impl.explicitDevice == nullptr)
            return true;

        const auto nativeDeviceInfo = impl.explicitDevice->GetNativeDeviceInfo();
        return Render::Settings::SupportsThreadedRenderFoundationPath(
            nativeDeviceInfo.backend,
            impl.explicitDevice->GetCapabilities());
    }

    bool AllowsThreadedHarnessPublish(const DriverImpl& impl)
    {
        switch (impl.requestedGraphicsBackend)
        {
        case Render::Settings::EGraphicsBackend::NONE:
            return true;
        case Render::Settings::EGraphicsBackend::DX12:
        case Render::Settings::EGraphicsBackend::VULKAN:
        case Render::Settings::EGraphicsBackend::OPENGL:
        case Render::Settings::EGraphicsBackend::DX11:
        case Render::Settings::EGraphicsBackend::METAL:
        default:
            return false;
        }
    }

    constexpr uint64_t kDriverGpuDrainTimeoutNanoseconds = 5'000'000'000ull;
    constexpr auto kThreadedLifecycleDrainWatchdog = std::chrono::seconds(5);

    void WaitForThreadedWorkerWake(
        DriverImpl& impl,
        uint64_t observedGeneration,
        std::chrono::milliseconds timeout = kThreadedWorkerWakeTimeout);

    bool DrainThreadedLifecycleSynchronously(
        DriverImpl& impl,
        Driver* driver = nullptr,
        const bool applyPendingSwapchainResize = true)
    {
        NLS_PROFILE_SCOPE();
        if (impl.threadedLifecycle == nullptr || driver == nullptr)
            return true;

        const auto drainStartTime = std::chrono::steady_clock::now();
        while (impl.threadedLifecycle->GetInFlightDepth() > 0u)
        {
            bool progressed = false;

            {
                NLS_PROFILE_NAMED_SCOPE("Driver::DrainRenderFrameBuilds");
                if (RenderThreadCoordinator::DrainPendingRenderFrameBuildsSynchronously(*driver))
                {
                    progressed = true;
                }
            }

            {
                NLS_PROFILE_NAMED_SCOPE("Driver::DrainRhiSubmissions");
                if (RhiThreadCoordinator::DrainPendingThreadedSubmissions(
                    *driver,
                    RhiSubmissionAttribution::SynchronousDrain,
                    applyPendingSwapchainResize))
                {
                    progressed = true;
                }
            }

            if (!progressed)
            {
                const uint64_t observedWakeGeneration =
                    impl.threadedWorkerWakeGeneration.load(std::memory_order_acquire);
                if (std::chrono::steady_clock::now() - drainStartTime >= kThreadedLifecycleDrainWatchdog)
                {
                    NLS_LOG_ERROR(
                        "DrainThreadedLifecycleSynchronously: timed out waiting for threaded lifecycle depth=" +
                        std::to_string(impl.threadedLifecycle->GetInFlightDepth()));
                    return false;
                }
                // Another worker may currently own a RenderScenePreparing/RhiSubmitting slot.
                // UI composition and shutdown are synchronization points, so wait for ownership
                // to return instead of presenting stale backbuffers or exposing unfinished readback.
                {
                    NLS_PROFILE_NAMED_SCOPE("Driver::WaitForThreadedDrainWake");
                    WaitForThreadedWorkerWake(impl, observedWakeGeneration, kThreadedDrainWakeTimeout);
                }
            }
        }
        return true;
    }

    void NotifyThreadedWorkers(DriverImpl& impl)
    {
        NLS_PROFILE_SCOPE();
        impl.threadedWorkerWakeGeneration.fetch_add(1u, std::memory_order_release);
        impl.threadedWorkerWake.notify_all();
    }

    void WaitForThreadedWorkerWake(
        DriverImpl& impl,
        const uint64_t observedGeneration,
        const std::chrono::milliseconds timeout)
    {
        NLS_PROFILE_SCOPE();
        std::unique_lock<std::mutex> lock(impl.threadedWorkerWakeMutex);
        impl.threadedWorkerWake.wait_for(
            lock,
            timeout,
            [&impl, observedGeneration]()
            {
                return impl.threadedStopRequested.load(std::memory_order_acquire) ||
                    impl.threadedWorkerWakeGeneration.load(std::memory_order_acquire) != observedGeneration;
            });
    }

    bool DrainFrameFenceForResize(
        Render::RHI::RHIFrameContext& frameContext,
        const DriverImpl* impl,
        const size_t frameContextIndex = std::numeric_limits<size_t>::max())
    {
        if (frameContext.frameFence == nullptr || frameContext.frameFence->IsSignaled())
            return true;

        if (impl != nullptr && impl->deviceLostDetected.load(std::memory_order_acquire))
        {
            NLS_LOG_ERROR(
                "Driver: abandoned submitted GPU work fence wait after device loss fence=" +
                std::string(frameContext.frameFence->GetDebugName()) +
                (frameContextIndex != std::numeric_limits<size_t>::max()
                    ? " frameContextIndex=" + std::to_string(frameContextIndex)
                    : std::string()));
            return false;
        }

        if (frameContext.frameFence->Wait(kDriverGpuDrainTimeoutNanoseconds))
            return true;

        NLS_LOG_ERROR(
            "Driver: timed out waiting for submitted GPU work frame fence=" +
            std::string(frameContext.frameFence->GetDebugName()) +
            (frameContextIndex != std::numeric_limits<size_t>::max()
                ? " frameContextIndex=" + std::to_string(frameContextIndex)
                : std::string()));
        return false;
    }

    bool DrainFrameFencesForResize(
        std::vector<Render::RHI::RHIFrameContext>& frameContexts,
        const DriverImpl* impl)
    {
        for (size_t frameContextIndex = 0u; frameContextIndex < frameContexts.size(); ++frameContextIndex)
        {
            if (!DrainFrameFenceForResize(frameContexts[frameContextIndex], impl, frameContextIndex))
                return false;
        }
        return true;
    }

    bool ReleaseFrameContextResources(
        Render::RHI::RHIFrameContext& frameContext,
        const DriverImpl* impl,
        const bool abandonFenceWait = false)
    {
        if (!abandonFenceWait && !DrainFrameFenceForResize(frameContext, impl))
            return false;

        if (frameContext.commandBuffer != nullptr && frameContext.commandBuffer->IsRecording())
            frameContext.commandBuffer->End();

        frameContext.swapchainBackbufferView = nullptr;
        frameContext.swapchainDepthStencilView = nullptr;
        frameContext.swapchainDepthStencilTexture = nullptr;
        frameContext.explicitReadbackTexture = nullptr;
        frameContext.explicitReadbackTextureGeneration = 0u;

        if (frameContext.resourceStateTracker != nullptr)
        {
            frameContext.resourceStateTracker->RetireTransientResources((std::numeric_limits<uint64_t>::max)());
            frameContext.resourceStateTracker->Reset();
        }

        frameContext.uploadContext.reset();
        frameContext.descriptorAllocator.reset();
        frameContext.resourceStateTracker.reset();
        frameContext.computeFinishedSemaphore.reset();
        frameContext.renderFinishedSemaphore.reset();
        frameContext.imageAcquiredSemaphore.reset();
        frameContext.frameFence.reset();
        frameContext.childCommandBuffers.clear();
        frameContext.childCommandPools.clear();
        frameContext.parallelCommandBuffers.clear();
        frameContext.parallelCommandPools.clear();
        frameContext.commandBuffer.reset();
        frameContext.commandPool.reset();
        frameContext.hasAcquiredSwapchainImage = false;
        frameContext.swapchainImageIndex = 0u;
        frameContext.uploadBytesReserved = 0u;
        return true;
    }

    uint32_t ResolveThreadedLifecycleSlotCount(const Render::Settings::DriverSettings& settings)
    {
        return settings.threadedFrameSlotCount != 0u
            ? settings.threadedFrameSlotCount
            : std::max<uint32_t>(1u, settings.framesInFlight);
    }

    uint32_t ResolveExplicitFrameContextCount(const Render::Settings::DriverSettings& settings)
    {
        if (settings.enableThreadedRendering)
            return ResolveThreadedLifecycleSlotCount(settings);

        return std::max<uint32_t>(1u, settings.framesInFlight);
    }

    void RebuildExplicitFrameContextRing(
        DriverImpl& impl,
        const uint32_t frameCount)
    {
        if (!DrainFrameFencesForResize(impl.frameContexts, &impl))
            return;
        Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(impl);
        for (auto& frameContext : impl.frameContexts)
            (void)ReleaseFrameContextResources(frameContext, &impl);
        impl.retainedThreadedSubmitResourceKeepAliveByFrameContext.clear();
        impl.deferredThreadedFrameScopedRetirementFrameContexts.clear();
        impl.deferredUiTextureRetirementFrameIdsByFrameContext.clear();
        impl.frameContexts.clear();

        if (impl.explicitDevice == nullptr)
            return;

        impl.frameContexts.reserve(frameCount);
        for (uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            Render::RHI::RHIFrameContext frameContext;
            frameContext.frameIndex = frameIndex;
            frameContext.commandPool = impl.explicitDevice->CreateCommandPool(
                Render::RHI::QueueType::Graphics,
                "FrameCommandPool" + std::to_string(frameIndex));
            NLS_ASSERT(frameContext.commandPool != nullptr, "Failed to create command pool for explicit RHI");
            frameContext.commandBuffer = frameContext.commandPool->CreateCommandBuffer(
                "FrameCommandBuffer" + std::to_string(frameIndex));
            NLS_ASSERT(frameContext.commandBuffer != nullptr, "Failed to create command buffer for explicit RHI");
            frameContext.frameFence = impl.explicitDevice->CreateFence("FrameFence" + std::to_string(frameIndex));
            NLS_ASSERT(frameContext.frameFence != nullptr, "Failed to create fence for explicit RHI");
            frameContext.imageAcquiredSemaphore =
                impl.explicitDevice->CreateSemaphore("FrameAcquire" + std::to_string(frameIndex));
            NLS_ASSERT(frameContext.imageAcquiredSemaphore != nullptr, "Failed to create semaphore for explicit RHI");
            frameContext.renderFinishedSemaphore =
                impl.explicitDevice->CreateSemaphore("FramePresent" + std::to_string(frameIndex));
            NLS_ASSERT(frameContext.renderFinishedSemaphore != nullptr, "Failed to create semaphore for explicit RHI");
            frameContext.computeFinishedSemaphore =
                impl.explicitDevice->CreateSemaphore("FrameCompute" + std::to_string(frameIndex));
            NLS_ASSERT(frameContext.computeFinishedSemaphore != nullptr, "Failed to create compute semaphore for explicit RHI");
            frameContext.resourceStateTracker = Render::RHI::CreateDefaultResourceStateTracker();
            frameContext.descriptorAllocator = Render::RHI::CreateDefaultDescriptorAllocator();
            frameContext.uploadContext = Render::Backend::CreateUploadContextForRhiDevice(impl.explicitDevice);
            impl.frameContexts.push_back(std::move(frameContext));
        }
        impl.retainedThreadedSubmitResourceKeepAliveByFrameContext.resize(impl.frameContexts.size());
    }

    bool ReleaseExplicitDeviceResourcesForReplacement(DriverImpl& impl, Driver* driver)
    {
        if (impl.unsafeGpuWorkQuarantined.load(std::memory_order_acquire) &&
            !impl.deviceLostDetected.load(std::memory_order_acquire))
        {
            PreserveUnsafeGpuWorkQuarantineResources(impl);
            NLS_LOG_ERROR(
                "DriverTestAccess::SetExplicitDevice: refusing to release explicit device resources while GPU work is quarantined without a reliable retirement fence");
            return false;
        }

        impl.explicitFrameActive = false;
        impl.uiStandaloneFrameActive = false;
        impl.uiStandaloneFramePending.store(false, std::memory_order_release);
        impl.uiStandaloneFramePendingUntilTickNs.store(0u, std::memory_order_release);
        impl.uiRenderFinishedSemaphore = {};
        impl.uiRenderFinishedValue = 0u;
        {
            std::lock_guard lock(impl.sceneToUiWaitMutex);
            impl.sceneToUiWaitSemaphore = {};
        }
        {
            std::lock_guard lock(impl.completedReadbackTextureMutex);
            impl.completedReadbackTexture = nullptr;
            impl.completedReadbackTextureGeneration = 0u;
            impl.submittedReadbackTextureGenerations.clear();
            impl.completedReadbackTextureHistory.clear();
        }
        if (impl.uiStandaloneFrameSubmissionLock.owns_lock())
            impl.uiStandaloneFrameSubmissionLock.unlock();
        impl.hasPendingSwapchainResize = false;

        std::unique_lock<std::timed_mutex> threadedSubmissionLock;
        if (impl.threadedLifecycle != nullptr)
        {
            if (driver == nullptr || !DrainThreadedLifecycleSynchronously(impl, driver))
            {
                NLS_LOG_ERROR("DriverTestAccess::SetExplicitDevice: timed out draining threaded rendering before replacing explicit device");
                return false;
            }

            threadedSubmissionLock = std::unique_lock<std::timed_mutex>(
                impl.threadedRhiSubmissionMutex,
                std::defer_lock);
            if (!threadedSubmissionLock.try_lock_for(kThreadedLifecycleDrainWatchdog))
            {
                NLS_LOG_ERROR("DriverTestAccess::SetExplicitDevice: timed out waiting for threaded RHI submission before replacing explicit device");
                return false;
            }
            if (impl.threadedLifecycle->GetInFlightDepth() > 0u)
            {
                NLS_LOG_ERROR("DriverTestAccess::SetExplicitDevice: threaded rendering became active while replacing explicit device");
                return false;
            }

            impl.threadedLifecycle->ReleaseRetainedFrameResources();
        }

        const bool abandonFenceWait = impl.deviceLostDetected.load(std::memory_order_acquire);
        const bool drainedFrameFences =
            abandonFenceWait ||
            DrainFrameFencesForResize(impl.frameContexts, &impl);
        if (drainedFrameFences && !abandonFenceWait)
            Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(impl);
        bool releasedAllFrameContexts = drainedFrameFences;
        for (auto& frameContext : impl.frameContexts)
        {
            releasedAllFrameContexts =
                ReleaseFrameContextResources(frameContext, &impl, abandonFenceWait) &&
                releasedAllFrameContexts;
        }
        if (!releasedAllFrameContexts)
        {
            NLS_LOG_ERROR("DriverTestAccess::SetExplicitDevice: timed out before replacing explicit device resources");
            return false;
        }

        impl.retainedThreadedSubmitResourceKeepAliveByFrameContext.clear();
        impl.deferredThreadedFrameScopedRetirementFrameContexts.clear();
        impl.deferredUiTextureRetirementFrameIdsByFrameContext.clear();
        impl.frameContexts.clear();
        impl.explicitSwapchain.reset();
        return true;
    }

}

void Detail::RememberCompletedReadbackTexture(
    DriverImpl& impl,
    const std::shared_ptr<Render::RHI::RHITexture>& texture,
    const uint64_t generation)
{
    NLS::Render::Context::RememberCompletedReadbackTexture(impl, texture, generation);
}

const char* Detail::ToPassDebugName(const RenderPassCommandKind kind)
{
    return NLS::Render::Context::ToPassDebugName(kind);
}

const char* Detail::ResolvePassProfileScopeName(const RenderPassCommandInput& input)
{
    return NLS::Render::Context::ResolvePassProfileScopeName(input);
}

bool Detail::IsPassRecordable(
    const RenderScenePackage& package,
    const RenderPassCommandInput& input)
{
    return NLS::Render::Context::IsPassRecordable(package, input);
}

void Detail::LogSkippedPass(
    const RenderScenePackage& package,
    const RenderPassCommandInput& passInput,
    const char* reason)
{
    NLS::Render::Context::LogSkippedPass(package, passInput, reason);
}

bool Detail::BeginPassCommandPlan(
    Render::RHI::RHICommandBuffer& commandBuffer,
    const std::shared_ptr<Render::RHI::RHITextureView>& swapchainBackbufferView,
    const std::shared_ptr<Render::RHI::RHITextureView>& swapchainDepthStencilView,
    const RenderPassCommandInput& input,
    Render::RHI::RHIFrameContext* frameContext)
{
    return NLS::Render::Context::BeginPassCommandPlan(
        commandBuffer,
        swapchainBackbufferView,
        swapchainDepthStencilView,
        input,
        frameContext);
}

void Detail::EndPassCommandPlan(
    Render::RHI::RHICommandBuffer& commandBuffer,
    const RenderPassCommandInput* input,
    Render::RHI::RHIFrameContext* frameContext)
{
    NLS::Render::Context::EndPassCommandPlan(commandBuffer, input, frameContext);
}

bool Detail::HasResourceVisibilityTransitions(const RenderPassCommandInput& input)
{
    return NLS::Render::Context::HasResourceVisibilityTransitions(input);
}

bool Detail::RecordResourceVisibilityTransitions(
    Render::RHI::RHICommandBuffer& commandBuffer,
    const RenderPassCommandInput& input,
    Render::RHI::RHIFrameContext* frameContext)
{
    return NLS::Render::Context::RecordResourceVisibilityTransitions(
        commandBuffer,
        input,
        frameContext);
}

uint64_t Detail::RecordPreparedDrawCommandsForPass(
    Render::RHI::RHICommandBuffer* commandBuffer,
    const RenderPassCommandInput& input)
{
    return NLS::Render::Context::RecordPreparedDrawCommandsForPass(commandBuffer, input);
}

uint64_t Detail::RecordPreparedDrawCommandsForPassRange(
    Render::RHI::RHICommandBuffer* commandBuffer,
    const RenderPassCommandInput& input,
    const std::vector<RecordedDrawCommandInput>& recordedDrawCommands,
    const uint64_t recordedDrawBegin,
    const uint64_t recordedDrawCount)
{
    return NLS::Render::Context::RecordPreparedDrawCommandsForPassRange(
        commandBuffer,
        input,
        recordedDrawCommands,
        recordedDrawBegin,
        recordedDrawCount);
}

uint64_t Detail::RecordComputeDispatches(
    Render::RHI::RHICommandBuffer& commandBuffer,
    const std::vector<RecordedComputeDispatchInput>& dispatchInputs,
    const bool recordShaderReadBarriers)
{
    return NLS::Render::Context::RecordComputeDispatches(
        commandBuffer,
        dispatchInputs,
        recordShaderReadBarriers);
}

bool Detail::SupportsThreadedFoundationExecution(const DriverImpl& impl)
{
    return NLS::Render::Context::SupportsThreadedFoundationExecution(impl);
}

bool Detail::AllowsThreadedHarnessPublish(const DriverImpl& impl)
{
    return NLS::Render::Context::AllowsThreadedHarnessPublish(impl);
}

void Detail::NotifyThreadedWorkers(DriverImpl& impl)
{
    NLS::Render::Context::NotifyThreadedWorkers(impl);
}

void Detail::WaitForThreadedWorkerWake(DriverImpl& impl, const uint64_t observedGeneration)
{
    NLS::Render::Context::WaitForThreadedWorkerWake(impl, observedGeneration);
}

void Detail::PopulateVisibilityTransitionsFromResourceUsage(
    std::vector<ParallelCommandWorkUnit>& workUnits)
{
    NLS::Render::Context::PopulateVisibilityTransitionsFromResourceUsage(workUnits);
}

Driver* TryGetLocatedDriver()
{
    return NLS::Core::ServiceLocator::Contains<Driver>()
        ? &NLS::Core::ServiceLocator::Get<Driver>()
        : nullptr;
}

void MarkLocatedDriverUnsafeGpuWorkQuarantined(const std::string& reason)
{
    if (auto* driver = TryGetLocatedDriver(); driver != nullptr)
        DriverUIAccess::MarkUnsafeGpuWorkQuarantined(*driver, reason);
}

void MarkLocatedDriverDeviceLost(const std::string& reason)
{
    if (auto* driver = TryGetLocatedDriver(); driver != nullptr)
        DriverUIAccess::MarkDeviceLost(*driver, reason);
}

Driver& RequireLocatedDriver(const std::string_view ownerName)
{
    if (auto* driver = TryGetLocatedDriver(); driver != nullptr)
        return *driver;

    const auto message = ownerName.empty()
        ? std::string("Rendering operation requires an initialized Driver.")
        : std::string(ownerName) + " requires an initialized Driver.";
    NLS_ASSERT(false, message.c_str());
    return *static_cast<Driver*>(nullptr);
}

std::optional<Render::Settings::EGraphicsBackend> TryGetLocatedActiveGraphicsBackend()
{
    if (const auto* driver = TryGetLocatedDriver(); driver != nullptr)
        return driver->GetActiveGraphicsBackend();

    return std::nullopt;
}

bool DriverRendererAccess::HasExplicitRHI(const Driver& driver)
{
	return driver.m_impl->explicitDevice != nullptr;
}

bool DriverRendererAccess::IsThreadedRenderingEnabled(const Driver& driver)
{
    return RenderThreadCoordinator::IsThreadedRenderingEnabled(driver);
}

bool DriverRendererAccess::IsLightGridEnabled(const Driver& driver)
{
    return driver.m_impl != nullptr && driver.m_impl->lightGridEnabled;
}

std::shared_ptr<Render::RHI::RHIDevice> DriverRendererAccess::GetExplicitDevice(const Driver& driver)
{
	return driver.m_impl->explicitDevice;
}

bool DriverRendererAccess::TryPublishPreparedFrameBuilder(
    Driver& driver,
    const FrameSnapshot& snapshot,
    PreparedRenderSceneBuilder renderSceneBuilder,
    size_t* publishedSlotIndex,
    uint64_t* publishedFrameId,
    const bool backgroundPreview)
{
    if (driver.m_impl != nullptr)
    {
        auto originalRenderSceneBuilder = std::move(renderSceneBuilder);
        renderSceneBuilder = [&driver, originalRenderSceneBuilder = std::move(originalRenderSceneBuilder)]() mutable
        {
            auto package = originalRenderSceneBuilder();
            if (package.targetsSwapchain)
            {
                uint64_t consumedUiSnapshotGeneration = 0u;
                auto uiSnapshot = DriverUIAccess::ConsumePendingUiDrawDataSnapshot(
                    driver,
                    &consumedUiSnapshotGeneration);
                if (uiSnapshot != nullptr && uiSnapshot->hasVisibleDraws)
                {
                    if (!AttachUiOverlaySnapshotToRenderScenePackage(package, uiSnapshot))
                    {
                        DriverUIAccess::RestoreConsumedUiDrawDataSnapshotIfUnchanged(
                            driver,
                            uiSnapshot,
                            consumedUiSnapshotGeneration);
                    }
                }
            }
            return package;
        };
    }

    return RenderThreadCoordinator::TryPublishPreparedFrameBuilder(
        driver,
        snapshot,
        std::move(renderSceneBuilder),
        true,
        publishedSlotIndex,
        publishedFrameId,
        backgroundPreview);
}

void DriverRendererAccess::CancelBackgroundPreviewPublicationRequest(Driver& driver)
{
    if (driver.m_impl != nullptr)
        driver.m_impl->backgroundPreviewPublicationRequested.store(false, std::memory_order_release);
}

bool DriverRendererAccess::QueueStandalonePostSubmitBufferReadback(
    Driver& driver,
    PostSubmitBufferReadbackRequest request)
{
    if (driver.m_impl == nullptr ||
        !driver.m_impl->explicitFrameActive ||
        request.state == nullptr)
    {
        return false;
    }

    driver.m_impl->standalonePostSubmitBufferReadbacks.push_back(std::move(request));
    return true;
}

bool DriverRendererAccess::TryDrainThreadedRendering(Driver& driver, const bool applyPendingSwapchainResize)
{
    if (driver.m_impl == nullptr)
        return true;

    const bool drained = DrainThreadedLifecycleSynchronously(
        *driver.m_impl,
        &driver,
        applyPendingSwapchainResize);
    if (drained && applyPendingSwapchainResize)
        driver.ApplyPendingSwapchainResize();
    return drained;
}

bool DriverRendererAccess::TryWaitForSubmittedGpuWork(Driver& driver)
{
    if (driver.m_impl == nullptr)
        return true;
    if (!TryDrainThreadedRendering(driver, false))
        return false;
    return DrainFrameFencesForResize(driver.m_impl->frameContexts, driver.m_impl.get());
}

void DriverRendererAccess::DrainThreadedRendering(Driver& driver)
{
    (void)TryDrainThreadedRendering(driver);
}

ThreadedFrameTelemetry DriverRendererAccess::GetThreadedFrameTelemetry(const Driver& driver)
{
    auto telemetry = RenderThreadCoordinator::GetThreadedFrameTelemetry(driver);
    (void)AppendDriverTelemetry(driver.m_impl.get(), telemetry, true);
    return telemetry;
}

std::optional<ThreadedFrameTelemetry> DriverRendererAccess::TryGetThreadedFrameTelemetry(const Driver& driver)
{
    auto telemetry = RenderThreadCoordinator::TryGetThreadedFrameTelemetry(driver);
    if (!telemetry.has_value())
        return std::nullopt;

    if (!AppendDriverTelemetry(driver.m_impl.get(), telemetry.value(), false))
        return std::nullopt;
    return telemetry;
}

std::vector<uint64_t> DriverRendererAccess::CollectStreamingDependencyPins(const Driver& driver)
{
    if (driver.m_impl == nullptr || driver.m_impl->threadedLifecycle == nullptr)
        return {};

    return driver.m_impl->threadedLifecycle->CollectStreamingDependencyPins();
}

void DriverRendererAccess::SetViewport(
	Driver& driver,
	const uint32_t /*x*/,
	const uint32_t /*y*/,
	const uint32_t /*width*/,
	const uint32_t /*height*/)
{
	// Formal RHI: viewport is set via RHICommandBuffer::SetViewport
	// This is a no-op when using explicit RHI since command buffer handles it
	NLS_ASSERT(driver.m_impl->explicitDevice != nullptr, "Driver requires explicitDevice for all backends");
}

Render::Data::PipelineState DriverRendererAccess::CreatePipelineState(const Driver& driver)
{
	return driver.m_impl->defaultPipelineState;
}

bool DriverRendererAccess::SupportsEditorPickingReadback(const Driver& driver)
{
	NLS_ASSERT(driver.m_impl->explicitDevice != nullptr, "Driver requires explicitDevice for all backends");
	return Render::Settings::SupportsEditorPickingReadback(driver.m_impl->explicitDevice->GetCapabilities());
}

Render::FrameGraph::FrameGraphExecutionContext DriverRendererAccess::CreateFrameGraphExecutionContext(const Driver& driver)
{
	const auto* frameContext = Detail::GetActiveFrameContext(*driver.m_impl);
	return {
		const_cast<Driver&>(driver),
		driver.m_impl->explicitDevice.get(),
		frameContext != nullptr ? frameContext->commandBuffer.get() : nullptr,
		const_cast<Render::RHI::RHIFrameContext*>(frameContext)
	};
}

std::optional<size_t> DriverRendererAccess::GetActiveFrameContextSlotIndex(const Driver& driver)
{
    if (driver.m_impl == nullptr || driver.m_impl->frameContexts.empty())
        return std::nullopt;

    if (Detail::GetActiveFrameContext(*driver.m_impl) == nullptr)
        return std::nullopt;

    return driver.m_impl->currentFrameIndex % driver.m_impl->frameContexts.size();
}

size_t DriverRendererAccess::GetFrameContextSlotCount(const Driver& driver)
{
    if (driver.m_impl == nullptr)
        return 0u;

    if (!driver.m_impl->frameContexts.empty())
        return driver.m_impl->frameContexts.size();

    return 0u;
}

size_t DriverRendererAccess::GetLifecycleFrameSlotCount(const Driver& driver)
{
    if (driver.m_impl == nullptr)
        return 0u;

    const auto* threadedLifecycle = driver.m_impl->threadedLifecycle.get();
    if (threadedLifecycle != nullptr)
        return threadedLifecycle->GetSlotCount();

    return GetFrameContextSlotCount(driver);
}

namespace
{
    std::optional<size_t> ReserveReusableFrameContextSlotIndexForDriver(
        DriverImpl& impl,
        const bool waitForDeferredFrameFence,
        const bool waitForRetirement)
    {
        auto* threadedLifecycle = impl.threadedLifecycle.get();
        if (threadedLifecycle != nullptr)
        {
            if (impl.deviceLostDetected.load(std::memory_order_acquire) ||
                impl.unsafeGpuWorkQuarantined.load(std::memory_order_acquire))
            {
                return std::nullopt;
            }

            if (impl.explicitDevice != nullptr && impl.frameContexts.empty())
            {
                RebuildExplicitFrameContextRing(
                    impl,
                    static_cast<uint32_t>(threadedLifecycle->GetSlotCount()));
            }

            std::vector<size_t> skippedUnsafeSlots;
            while (skippedUnsafeSlots.size() < threadedLifecycle->GetSlotCount())
            {
                const auto reservedSlotIndex = threadedLifecycle->ReserveReusableSlotIndexExcluding(
                    skippedUnsafeSlots,
                    waitForRetirement
                        ? std::chrono::milliseconds(impl.threadedPublishRetirementWaitMs)
                        : std::chrono::milliseconds::zero());
                if (!reservedSlotIndex.has_value())
                    return std::nullopt;

                if (reservedSlotIndex.value() >= impl.frameContexts.size())
                {
                    if (impl.explicitDevice == nullptr)
                        return reservedSlotIndex;

                    (void)threadedLifecycle->ReleaseReservedReusableSlotIndex(reservedSlotIndex.value());
                    skippedUnsafeSlots.push_back(reservedSlotIndex.value());
                    continue;
                }

                const auto reservedSlotState = threadedLifecycle->CopySlot(reservedSlotIndex.value());
                auto& frameContext = impl.frameContexts[reservedSlotIndex.value()];
                const bool retiredDeferredSlot =
                    reservedSlotState.has_value() &&
                    reservedSlotState->stage == ThreadedFrameStage::Retired &&
                    reservedSlotState->submissionFrame.has_value() &&
                    reservedSlotState->submissionFrame->deferredFrameScopedRetirement &&
                    frameContext.frameFence != nullptr;
                bool deferredFenceComplete =
                    !retiredDeferredSlot ||
                    frameContext.frameFence->IsSignaled();
                const bool exhaustedOtherLifecycleSlots =
                    skippedUnsafeSlots.size() + 1u >= threadedLifecycle->GetSlotCount();
                if (!deferredFenceComplete &&
                    waitForDeferredFrameFence &&
                    exhaustedOtherLifecycleSlots &&
                    impl.threadedPublishRetirementWaitMs > 0u)
                {
                    const uint64_t waitTimeoutNs =
                        static_cast<uint64_t>(impl.threadedPublishRetirementWaitMs) * 1'000'000ull;
                    deferredFenceComplete = frameContext.frameFence->Wait(waitTimeoutNs);
                }
                if (!deferredFenceComplete)
                {
                    (void)threadedLifecycle->ReleaseReservedReusableSlotIndex(reservedSlotIndex.value());
                    skippedUnsafeSlots.push_back(reservedSlotIndex.value());
                    continue;
                }

                if (frameContext.frameFence != nullptr)
                {
                    (void)Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(
                        impl,
                        frameContext,
                        reservedSlotIndex.value());
                }
                return reservedSlotIndex;
            }

            return std::nullopt;
        }

        if (!impl.frameContexts.empty())
            return impl.currentFrameIndex % impl.frameContexts.size();

        return 0u;
    }
}

std::optional<size_t> DriverRendererAccess::ReserveReusableFrameContextSlotIndex(Driver& driver)
{
    if (driver.m_impl == nullptr)
        return std::nullopt;

    return ReserveReusableFrameContextSlotIndexForDriver(*driver.m_impl, false, true);
}

std::optional<size_t> DriverRendererAccess::ReserveReusableFrameContextSlotIndexForPreparedPublication(
    Driver& driver,
    const bool waitForRetirement)
{
    if (driver.m_impl == nullptr)
        return std::nullopt;

    return ReserveReusableFrameContextSlotIndexForDriver(
        *driver.m_impl,
        waitForRetirement,
        waitForRetirement);
}

bool DriverRendererAccess::ReleaseReservedFrameContextSlotIndex(Driver& driver, const size_t slotIndex)
{
    if (driver.m_impl == nullptr)
        return false;

    auto* threadedLifecycle = driver.m_impl->threadedLifecycle.get();
    if (threadedLifecycle != nullptr)
        return threadedLifecycle->ReleaseReservedReusableSlotIndex(slotIndex);

    return true;
}

std::optional<size_t> DriverRendererAccess::GetReservedFrameContextSlotIndex(const Driver& driver)
{
    if (driver.m_impl == nullptr)
        return std::nullopt;

    const auto* threadedLifecycle = driver.m_impl->threadedLifecycle.get();
    if (threadedLifecycle == nullptr)
        return std::nullopt;

    return threadedLifecycle->GetReservedReusableSlotIndex();
}

std::shared_ptr<Render::RHI::RHICommandBuffer> DriverRendererAccess::GetActiveExplicitCommandBuffer(const Driver& driver)
{
	const auto* frameContext = Detail::GetActiveFrameContext(*driver.m_impl);
	return frameContext != nullptr ? frameContext->commandBuffer : nullptr;
}

std::shared_ptr<Render::RHI::RHITextureView> DriverRendererAccess::GetSwapchainBackbufferView(const Driver& driver)
{
	const auto* frameContext = Detail::GetActiveFrameContext(*driver.m_impl);
	if (frameContext == nullptr)
		return nullptr;
	return frameContext->swapchainBackbufferView;
}

std::shared_ptr<Render::RHI::RHITextureView> DriverRendererAccess::GetSwapchainDepthStencilView(const Driver& driver)
{
	const auto* frameContext = Detail::GetActiveFrameContext(*driver.m_impl);
	if (frameContext == nullptr)
		return nullptr;
	return frameContext->swapchainDepthStencilView;
}

std::shared_ptr<Render::RHI::RHITexture> DriverRendererAccess::ResolveReadbackTexture(const Driver& driver)
{
    if (const auto* frameContext = Detail::GetActiveFrameContext(*driver.m_impl); frameContext != nullptr)
    {
        if (const auto texture = Render::FrameGraph::ResolveActiveExplicitReadbackTexture(frameContext);
            texture != nullptr)
        {
            return texture;
        }
    }

    std::lock_guard lock(driver.m_impl->completedReadbackTextureMutex);
    return driver.m_impl->completedReadbackTexture;
}

bool DriverRendererAccess::HasCompletedReadbackTexture(
    const Driver& driver,
    const std::shared_ptr<Render::RHI::RHITexture>& texture)
{
    if (driver.m_impl == nullptr || texture == nullptr)
        return false;

    return HasRememberedReadbackTexture(*driver.m_impl, texture);
}

bool DriverRendererAccess::HasCompletedReadbackTexture(
    const Driver& driver,
    const std::shared_ptr<Render::RHI::RHITexture>& texture,
    const uint64_t generation)
{
    if (driver.m_impl == nullptr || texture == nullptr)
        return false;

    return HasRememberedReadbackTexture(*driver.m_impl, texture, generation);
}

uint64_t DriverRendererAccess::BeginReadbackTextureSubmission(
    const Driver& driver,
    const std::shared_ptr<Render::RHI::RHITexture>& texture)
{
    if (driver.m_impl == nullptr || texture == nullptr)
        return 0u;

    std::lock_guard lock(driver.m_impl->completedReadbackTextureMutex);
    const uint64_t generation = driver.m_impl->nextReadbackTextureGeneration++;
    driver.m_impl->submittedReadbackTextureGenerations.push_back({ texture, generation });

    constexpr size_t kMaxSubmittedReadbackGenerations = 32u;
    driver.m_impl->submittedReadbackTextureGenerations.erase(
        std::remove_if(
            driver.m_impl->submittedReadbackTextureGenerations.begin(),
            driver.m_impl->submittedReadbackTextureGenerations.end(),
            [](const CompletedReadbackTextureRecord& record)
            {
                return record.texture.expired();
            }),
        driver.m_impl->submittedReadbackTextureGenerations.end());
    while (driver.m_impl->submittedReadbackTextureGenerations.size() > kMaxSubmittedReadbackGenerations)
        driver.m_impl->submittedReadbackTextureGenerations.erase(
            driver.m_impl->submittedReadbackTextureGenerations.begin());

    return generation;
}

void DriverRendererAccess::InvalidateCompletedReadbackTexture(
    const Driver& driver,
    const std::shared_ptr<Render::RHI::RHITexture>& texture)
{
    if (driver.m_impl == nullptr || texture == nullptr)
        return;

    ForgetRememberedReadbackTexture(*driver.m_impl, texture);
}

std::shared_ptr<Render::RHI::PipelineCache> DriverRendererAccess::GetPipelineCache(const Driver& driver)
{
    return driver.m_impl->pipelineCache;
}

std::shared_ptr<Render::RHI::DescriptorAllocator> DriverRendererAccess::GetActiveDescriptorAllocator(const Driver& driver)
{
    const auto* frameContext = Detail::GetActiveFrameContext(*driver.m_impl);
    if (frameContext == nullptr)
    {
        const auto reservedSlotIndex = GetReservedFrameContextSlotIndex(driver);
        if (reservedSlotIndex.has_value() &&
            reservedSlotIndex.value() < driver.m_impl->frameContexts.size())
        {
            frameContext = &driver.m_impl->frameContexts[reservedSlotIndex.value()];
        }
    }

    if (frameContext == nullptr &&
        driver.m_impl->threadedLifecycle != nullptr &&
        !driver.m_impl->frameContexts.empty())
    {
        frameContext =
            &driver.m_impl->frameContexts[driver.m_impl->currentFrameIndex % driver.m_impl->frameContexts.size()];
    }

    return frameContext != nullptr ? frameContext->descriptorAllocator : nullptr;
}

std::shared_ptr<Render::RHI::RHIBindingLayout> DriverRendererAccess::CreateExplicitBindingLayout(
	const Driver& driver,
	const Render::RHI::RHIBindingLayoutDesc& desc)
{
	return driver.m_impl->explicitDevice != nullptr ? driver.m_impl->explicitDevice->CreateBindingLayout(desc) : nullptr;
}

std::shared_ptr<Render::RHI::RHIBindingSet> DriverRendererAccess::CreateExplicitBindingSet(
	const Driver& driver,
	const Render::RHI::RHIBindingSetDesc& desc,
    const Render::RHI::DescriptorAllocationLifetime allocationLifetime)
{
    const auto descriptorAllocator = GetActiveDescriptorAllocator(driver);
    uint64_t frameIndex = 0u;
    if (const auto activeSlotIndex = GetActiveFrameContextSlotIndex(driver);
        activeSlotIndex.has_value() && activeSlotIndex.value() < driver.m_impl->frameContexts.size())
    {
        frameIndex = driver.m_impl->frameContexts[activeSlotIndex.value()].frameIndex;
    }
    else if (const auto reservedSlotIndex = GetReservedFrameContextSlotIndex(driver);
        reservedSlotIndex.has_value() && reservedSlotIndex.value() < driver.m_impl->frameContexts.size())
    {
        frameIndex = driver.m_impl->frameContexts[reservedSlotIndex.value()].frameIndex;
    }
    else if (!driver.m_impl->frameContexts.empty())
    {
        frameIndex = driver.m_impl->frameContexts[
            driver.m_impl->currentFrameIndex % driver.m_impl->frameContexts.size()].frameIndex;
    }
	return CreateTrackedBindingSet(
        driver.m_impl->explicitDevice,
        descriptorAllocator,
        frameIndex,
        desc,
        allocationLifetime);
}

void DriverRendererAccess::ReadPixels(
	const Driver& driver,
	const uint32_t x,
	const uint32_t y,
	const uint32_t width,
	const uint32_t height,
	const Settings::EPixelDataFormat format,
	const Settings::EPixelDataType type,
	void* data)
{
	const auto result = ReadPixelsChecked(
        driver,
        x,
        y,
        width,
        height,
        format,
        type,
        data);
    if (!result.Succeeded())
        NLS_LOG_WARNING("DriverRendererAccess::ReadPixels failed: " + result.message);
}

void DriverRendererAccess::ReadPixels(
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
    const auto result = ReadPixelsChecked(
        driver,
        texture,
        x,
        y,
        width,
        height,
        format,
        type,
        data);
    if (!result.Succeeded())
        NLS_LOG_WARNING("DriverRendererAccess::ReadPixels failed: " + result.message);
}

Render::RHI::RHIReadbackResult DriverRendererAccess::ReadPixelsChecked(
    const Driver& driver,
    const uint32_t x,
    const uint32_t y,
    const uint32_t width,
    const uint32_t height,
    const Settings::EPixelDataFormat format,
    const Settings::EPixelDataType type,
    void* data)
{
    return RhiThreadCoordinator::ReadPixelsChecked(
        driver,
        x,
        y,
        width,
        height,
        format,
        type,
        data);
}

Render::RHI::RHIReadbackResult DriverRendererAccess::ReadPixelsChecked(
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
    return RhiThreadCoordinator::ReadPixelsChecked(
        driver,
        texture,
        x,
        y,
        width,
        height,
        format,
        type,
        data);
}

Render::RHI::RHIReadbackResult DriverRendererAccess::BeginReadPixels(
    const Driver& driver,
    const uint32_t x,
    const uint32_t y,
    const uint32_t width,
    const uint32_t height,
    const Settings::EPixelDataFormat format,
    const Settings::EPixelDataType type,
    void* data)
{
    return RhiThreadCoordinator::BeginReadPixels(
        driver,
        x,
        y,
        width,
        height,
        format,
        type,
        data);
}

Render::RHI::RHIReadbackResult DriverRendererAccess::BeginReadPixels(
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
    return RhiThreadCoordinator::BeginReadPixels(
        driver,
        texture,
        x,
        y,
        width,
        height,
        format,
        type,
        data);
}

Render::RHI::RHIReadbackResult DriverRendererAccess::PollReadbackCompletion(
    const Driver& driver,
    const Render::RHI::RHIReadbackResult& readback)
{
    return RhiThreadCoordinator::PollReadbackCompletion(driver, readback);
}


void DriverRendererAccess::Clear(
	Driver& /*driver*/,
	const bool /*colorBuffer*/,
	const bool /*depthBuffer*/,
	const bool /*stencilBuffer*/,
	const Maths::Vector4& /*color*/)
{
	// Formal RHI: clear is handled via BeginRenderPass with LoadOp::Clear
	// This is a no-op when using explicit RHI since render pass handles it
	NLS_ASSERT(false, "Clear should not be called with explicitDevice - use BeginRenderPass with LoadOp::Clear");
}

const Settings::EngineDiagnosticsSettings& DriverRendererAccess::GetDiagnosticsSettings(const Driver& driver)
{
	return driver.m_impl->diagnostics;
}

void DriverRendererAccess::SetDiagnosticsSettings(Driver& driver, const Settings::EngineDiagnosticsSettings& settings)
{
	driver.m_impl->diagnostics = settings;
	Settings::SetThreadDiagnosticsSettings(settings);
}

Render::RHI::NativeRenderDeviceInfo DriverUIAccess::GetNativeDeviceInfo(const Driver& driver)
{
	// Use explicit device - it has its own UI resources (renderPass, descriptorPool)
	// created via CreateUIResources() in the constructor
	NLS_ASSERT(driver.m_impl->explicitDevice != nullptr, "Driver requires explicitDevice for all backends");
	return driver.m_impl->explicitDevice->GetNativeDeviceInfo();
}

void DriverUIAccess::MarkUnsafeGpuWorkQuarantined(Driver& driver, const std::string& reason)
{
    if (driver.m_impl == nullptr)
        return;

    const std::string message = reason.empty()
        ? std::string{ "UI backend quarantined unsafe GPU work" }
        : reason;
    {
        std::lock_guard lock(driver.m_impl->driverTelemetryMutex);
        driver.m_impl->unsafeGpuWorkQuarantined.store(true, std::memory_order_release);
        driver.m_impl->unsafeGpuWorkQuarantineReason = message;
        ++driver.m_impl->queueOperationFailureCount;
        ++driver.m_impl->currentFrameQueueOperationFailureCount;
        driver.m_impl->lastQueueOperationFailure = message;
        driver.m_impl->currentFrameLastQueueOperationFailure = message;
    }
}

void DriverUIAccess::MarkDeviceLost(Driver& driver, const std::string& reason)
{
    if (driver.m_impl == nullptr)
        return;

    const std::string message = reason.empty()
        ? std::string{ "UI backend detected device lost" }
        : reason;
    {
        std::lock_guard lock(driver.m_impl->driverTelemetryMutex);
        driver.m_impl->deviceLostDetected.store(true, std::memory_order_release);
        driver.m_impl->deviceLostReason = message;
        ++driver.m_impl->queueOperationFailureCount;
        ++driver.m_impl->currentFrameQueueOperationFailureCount;
        driver.m_impl->lastQueueOperationFailure = message;
        driver.m_impl->currentFrameLastQueueOperationFailure = message;
    }
}

bool DriverUIAccess::PrepareUIRender(Driver& driver)
{
	return RhiThreadCoordinator::PrepareUIRender(driver);
}

void DriverUIAccess::ReleaseUITextureHandles(Driver& driver)
{
	if (driver.m_impl == nullptr || driver.m_impl->explicitDevice == nullptr)
		return;

	auto* uiBridgeDevice = driver.m_impl->explicitDevice->GetUIBridgeDevice();
	if (uiBridgeDevice != nullptr)
		uiBridgeDevice->ReleaseUITextureHandles();
}

void DriverUIAccess::PresentSwapchain(Driver& driver)
{
	RhiThreadCoordinator::PresentSwapchain(driver);
}

void DriverUIAccess::SetPolygonMode(Driver& driver, const Settings::ERasterizationMode mode)
{
	driver.SetPolygonMode(mode);
}

bool DriverUIAccess::IsRenderDocAvailable(const Driver& driver)
{
	return driver.m_impl->renderDocCaptureController != nullptr &&
		driver.m_impl->renderDocCaptureController->IsAvailable();
}

bool DriverUIAccess::ShouldForceRenderDocCaptureFrameRender(const Driver& driver)
{
	return driver.m_impl->renderDocCaptureController != nullptr &&
		driver.m_impl->renderDocCaptureController->ShouldForceCaptureFrameRender();
}

bool DriverUIAccess::QueueRenderDocCapture(Driver& driver, const std::string& label)
{
	return driver.m_impl->renderDocCaptureController != nullptr &&
		driver.m_impl->renderDocCaptureController->QueueCapture(label);
}

bool DriverUIAccess::QueueRenderDocCaptureForNextExternalOutput(Driver& driver, const std::string& label)
{
	return driver.m_impl->renderDocCaptureController != nullptr &&
		driver.m_impl->renderDocCaptureController->QueueCaptureForNextExternalOutput(label);
}

bool DriverUIAccess::StartRenderDocCapture(Driver& driver)
{
	return driver.m_impl->renderDocCaptureController != nullptr &&
		driver.m_impl->renderDocCaptureController->StartCapture();
}

bool DriverUIAccess::EndRenderDocCapture(Driver& driver)
{
	return driver.m_impl->renderDocCaptureController != nullptr &&
		driver.m_impl->renderDocCaptureController->EndCapture();
}

bool DriverUIAccess::OpenLatestRenderDocCapture(const Driver& driver)
{
	return driver.m_impl->renderDocCaptureController != nullptr &&
		driver.m_impl->renderDocCaptureController->OpenLatestCapture();
}

std::string DriverUIAccess::GetRenderDocCaptureDirectory(const Driver& driver)
{
	return driver.m_impl->renderDocCaptureController != nullptr
		? driver.m_impl->renderDocCaptureController->GetCaptureDirectory()
		: std::string{};
}

bool DriverUIAccess::GetRenderDocAutoOpenEnabled(const Driver& driver)
{
	return driver.m_impl->renderDocCaptureController != nullptr &&
		driver.m_impl->renderDocCaptureController->GetAutoOpenReplayUI();
}

void DriverUIAccess::SetRenderDocAutoOpenEnabled(Driver& driver, const bool enabled)
{
	if (driver.m_impl->renderDocCaptureController != nullptr)
		driver.m_impl->renderDocCaptureController->SetAutoOpenReplayUI(enabled);
}

void DriverUIAccess::SetRenderDocEnabled(Driver& driver, const bool enabled)
{
	if (driver.m_impl->renderDocCaptureController != nullptr)
		driver.m_impl->renderDocCaptureController->SetEnabled(enabled);
}

bool DriverUIAccess::IsRenderDocEnabled(const Driver& driver)
{
	return driver.m_impl->renderDocCaptureController != nullptr &&
		driver.m_impl->renderDocCaptureController->IsEnabled();
}

void DriverUIAccess::PublishUiDrawDataSnapshot(
    Driver& driver,
    std::shared_ptr<const UI::UiDrawDataSnapshot> snapshot)
{
    if (driver.m_impl == nullptr)
        return;

    const uint64_t frameId = snapshot != nullptr ? snapshot->frameId : 0u;
    {
        std::lock_guard lock(driver.m_impl->pendingUiOverlaySnapshotMutex);
        driver.m_impl->pendingUiOverlaySnapshot = std::move(snapshot);
        driver.m_impl->pendingUiOverlaySnapshotFrameId = frameId;
        if (frameId != 0u)
            driver.m_impl->latestUiOverlaySnapshotFrameId = frameId;
        ++driver.m_impl->pendingUiOverlaySnapshotGeneration;
    }

    Detail::NotifyThreadedWorkers(*driver.m_impl);
}

bool DriverUIAccess::PublishUiOnlyFrame(
    Driver& driver,
    const uint32_t renderWidth,
    const uint32_t renderHeight,
    size_t* publishedSlotIndex,
    uint64_t* publishedFrameId)
{
    if (publishedFrameId != nullptr)
        *publishedFrameId = 0u;
    if (publishedSlotIndex != nullptr)
        *publishedSlotIndex = 0u;

    if (driver.m_impl == nullptr ||
        driver.m_impl->explicitDevice == nullptr ||
        driver.m_impl->explicitSwapchain == nullptr ||
        driver.m_impl->threadedLifecycle == nullptr ||
        renderWidth == 0u ||
        renderHeight == 0u)
    {
        return false;
    }

    const auto overlayFeature =
        RHI::GetUIOverlayFrameGraphFeature(driver.m_impl->explicitDevice.get());
    if (!overlayFeature.supported)
        return false;

    std::shared_ptr<const UI::UiDrawDataSnapshot> snapshot;
    {
        std::lock_guard lock(driver.m_impl->pendingUiOverlaySnapshotMutex);
        snapshot = driver.m_impl->pendingUiOverlaySnapshot;
    }
    if (snapshot == nullptr || !snapshot->hasVisibleDraws)
        return false;

    FrameSnapshot frameSnapshot;
    frameSnapshot.frameId = snapshot->frameId != 0u
        ? snapshot->frameId
        : driver.m_impl->nextThreadedFrameId++;
    frameSnapshot.targetsSwapchain = true;
    frameSnapshot.renderWidth = renderWidth;
    frameSnapshot.renderHeight = renderHeight;
    frameSnapshot.clearColorBuffer = false;
    frameSnapshot.clearDepthBuffer = false;
    frameSnapshot.clearStencilBuffer = false;

    auto package = BuildSnapshotOwnedRenderScenePackage(
        frameSnapshot,
        SnapshotRenderScenePackageBuildMode::SkipDefaultPassInputs);
    if (!AttachUiOverlaySnapshotToRenderScenePackage(package, snapshot))
        return false;

    const bool published = RenderThreadCoordinator::TryPublishPreparedFrameBuilder(
        driver,
        frameSnapshot,
        [package = std::move(package)]() mutable
        {
            return package;
        },
        false,
        publishedSlotIndex,
        publishedFrameId);
    if (!published)
        return false;

    {
        std::lock_guard lock(driver.m_impl->pendingUiOverlaySnapshotMutex);
        if (driver.m_impl->pendingUiOverlaySnapshot == snapshot)
        {
            driver.m_impl->pendingUiOverlaySnapshot.reset();
            driver.m_impl->pendingUiOverlaySnapshotFrameId = 0u;
        }
    }

    return true;
}

std::shared_ptr<const UI::UiDrawDataSnapshot> DriverUIAccess::ConsumePendingUiDrawDataSnapshot(
    Driver& driver,
    uint64_t* consumedGeneration)
{
    if (consumedGeneration != nullptr)
        *consumedGeneration = 0u;

    if (driver.m_impl == nullptr)
        return nullptr;

    std::lock_guard lock(driver.m_impl->pendingUiOverlaySnapshotMutex);
    auto snapshot = std::move(driver.m_impl->pendingUiOverlaySnapshot);
    driver.m_impl->pendingUiOverlaySnapshotFrameId = 0u;
    if (consumedGeneration != nullptr)
        *consumedGeneration = driver.m_impl->pendingUiOverlaySnapshotGeneration;
    return snapshot;
}

bool DriverUIAccess::RestoreConsumedUiDrawDataSnapshotIfUnchanged(
    Driver& driver,
    std::shared_ptr<const UI::UiDrawDataSnapshot> snapshot,
    const uint64_t consumedGeneration)
{
    if (driver.m_impl == nullptr || snapshot == nullptr)
        return false;

    const uint64_t frameId = snapshot->frameId;
    {
        std::lock_guard lock(driver.m_impl->pendingUiOverlaySnapshotMutex);
        if (driver.m_impl->pendingUiOverlaySnapshot != nullptr ||
            driver.m_impl->pendingUiOverlaySnapshotGeneration != consumedGeneration)
        {
            return false;
        }

        driver.m_impl->pendingUiOverlaySnapshot = std::move(snapshot);
        driver.m_impl->pendingUiOverlaySnapshotFrameId = frameId;
        if (frameId != 0u)
            driver.m_impl->latestUiOverlaySnapshotFrameId = std::max(
                driver.m_impl->latestUiOverlaySnapshotFrameId,
                frameId);
        ++driver.m_impl->pendingUiOverlaySnapshotGeneration;
    }

    Detail::NotifyThreadedWorkers(*driver.m_impl);
    return true;
}

UI::UiTextureId DriverUIAccess::RegisterUiTextureView(
    Driver& driver,
    const std::shared_ptr<RHI::RHITextureView>& textureView,
    const UI::UiTextureSynchronizationScope synchronizationScope)
{
    if (driver.m_impl == nullptr)
        return {};

    return driver.m_impl->uiTextureRegistry.RegisterTextureView(textureView, synchronizationScope);
}

uint64_t DriverUIAccess::RequestUiRgba8TextureUpload(
    Driver& driver,
    Rgba8TextureUploadRequest request)
{
    if (driver.m_impl == nullptr ||
        request.width == 0u ||
        request.height == 0u ||
        request.rgbaPixels.empty())
    {
        return 0u;
    }

    const size_t expectedBytes =
        static_cast<size_t>(request.width) *
        static_cast<size_t>(request.height) *
        4u;
    if (request.rgbaPixels.size() < expectedBytes)
        return 0u;

    std::lock_guard lock(driver.m_impl->pendingUiRgba8TextureUploadMutex);
    const uint64_t requestId = driver.m_impl->nextUiRgba8TextureUploadRequestId++;
    if (driver.m_impl->nextUiRgba8TextureUploadRequestId == 0u)
        driver.m_impl->nextUiRgba8TextureUploadRequestId = 1u;

    driver.m_impl->pendingUiRgba8TextureUploads.push_back({
        requestId,
        request.width,
        request.height,
        std::move(request.rgbaPixels),
        std::move(request.debugName)
    });
    Detail::NotifyThreadedWorkers(*driver.m_impl);
    return requestId;
}

DriverUIAccess::Rgba8TextureUploadResult DriverUIAccess::ConsumeUiRgba8TextureUploadResult(
    Driver& driver,
    const uint64_t requestId)
{
    Rgba8TextureUploadResult result;
    if (driver.m_impl == nullptr || requestId == 0u)
        return result;

    std::lock_guard lock(driver.m_impl->pendingUiRgba8TextureUploadMutex);
    auto found = driver.m_impl->completedUiRgba8TextureUploads.find(requestId);
    if (found == driver.m_impl->completedUiRgba8TextureUploads.end())
        return result;

    result.ready = true;
    result.success = found->second.success;
    result.texture = std::move(found->second.texture);
    result.textureView = std::move(found->second.textureView);
    result.width = found->second.width;
    result.height = found->second.height;
    result.diagnostic = std::move(found->second.diagnostic);
    driver.m_impl->completedUiRgba8TextureUploads.erase(found);
    return result;
}

void DriverUIAccess::CancelUiRgba8TextureUpload(
    Driver& driver,
    const uint64_t requestId)
{
    if (driver.m_impl == nullptr || requestId == 0u)
        return;

    std::lock_guard lock(driver.m_impl->pendingUiRgba8TextureUploadMutex);
    const size_t pendingCountBefore = driver.m_impl->pendingUiRgba8TextureUploads.size();
    driver.m_impl->pendingUiRgba8TextureUploads.erase(
        std::remove_if(
            driver.m_impl->pendingUiRgba8TextureUploads.begin(),
            driver.m_impl->pendingUiRgba8TextureUploads.end(),
            [requestId](const DriverImpl::PendingUiRgba8TextureUpload& upload)
            {
                return upload.requestId == requestId;
            }),
        driver.m_impl->pendingUiRgba8TextureUploads.end());
    const bool removedPending =
        driver.m_impl->pendingUiRgba8TextureUploads.size() != pendingCountBefore;
    const bool removedCompleted =
        driver.m_impl->completedUiRgba8TextureUploads.erase(requestId) != 0u;
    if (!removedPending && !removedCompleted)
        driver.m_impl->canceledUiRgba8TextureUploadRequestIds.insert(requestId);
}

uint64_t DriverResourceAccess::RequestMeshRuntimeUpload(
    Driver& driver,
    MeshRuntimeUploadRequest request)
{
    if (driver.m_impl == nullptr || request.vertices.empty())
        return 0u;

    std::lock_guard lock(driver.m_impl->pendingMeshRuntimeUploadMutex);
    const uint64_t requestId = driver.m_impl->nextMeshRuntimeUploadRequestId++;
    if (driver.m_impl->nextMeshRuntimeUploadRequestId == 0u)
        driver.m_impl->nextMeshRuntimeUploadRequestId = 1u;
    driver.m_impl->pendingMeshRuntimeUploads.push_back({ requestId, std::move(request) });
    Detail::NotifyThreadedWorkers(*driver.m_impl);
    return requestId;
}

MeshRuntimeUploadResult DriverResourceAccess::ConsumeMeshRuntimeUploadResult(
    Driver& driver,
    const uint64_t requestId)
{
    MeshRuntimeUploadResult result;
    if (driver.m_impl == nullptr || requestId == 0u)
        return result;

    std::lock_guard lock(driver.m_impl->pendingMeshRuntimeUploadMutex);
    auto found = driver.m_impl->completedMeshRuntimeUploads.find(requestId);
    if (found == driver.m_impl->completedMeshRuntimeUploads.end())
        return result;

    result.ready = true;
    result.success = found->second.success;
    result.mesh = std::move(found->second.mesh);
    result.diagnostic = std::move(found->second.diagnostic);
    driver.m_impl->completedMeshRuntimeUploads.erase(found);
    return result;
}

void DriverResourceAccess::CancelMeshRuntimeUpload(
    Driver& driver,
    const uint64_t requestId)
{
    if (driver.m_impl == nullptr || requestId == 0u)
        return;

    std::lock_guard lock(driver.m_impl->pendingMeshRuntimeUploadMutex);
    const auto pendingCountBefore = driver.m_impl->pendingMeshRuntimeUploads.size();
    driver.m_impl->pendingMeshRuntimeUploads.erase(
        std::remove_if(
            driver.m_impl->pendingMeshRuntimeUploads.begin(),
            driver.m_impl->pendingMeshRuntimeUploads.end(),
            [requestId](const DriverImpl::PendingMeshRuntimeUpload& upload)
            {
                return upload.requestId == requestId;
            }),
        driver.m_impl->pendingMeshRuntimeUploads.end());
    const bool removedPending =
        driver.m_impl->pendingMeshRuntimeUploads.size() != pendingCountBefore;
    const bool removedCompleted =
        driver.m_impl->completedMeshRuntimeUploads.erase(requestId) != 0u;
    if (!removedPending && !removedCompleted)
        driver.m_impl->canceledMeshRuntimeUploadRequestIds.insert(requestId);
}

void DriverUIAccess::ReleaseUiTextureView(
    Driver& driver,
    const std::shared_ptr<RHI::RHITextureView>& textureView)
{
    if (driver.m_impl == nullptr)
        return;

    uint64_t retireFrameId = 0u;
    {
        std::lock_guard lock(driver.m_impl->pendingUiOverlaySnapshotMutex);
        retireFrameId = driver.m_impl->pendingUiOverlaySnapshotFrameId != 0u
            ? driver.m_impl->pendingUiOverlaySnapshotFrameId
            : driver.m_impl->latestUiOverlaySnapshotFrameId;
    }

    driver.m_impl->uiTextureRegistry.ReleaseTextureView(textureView, retireFrameId);
}

void DriverUIAccess::NotifyUiOverlayFontAtlasChanged(Driver& driver)
{
    if (driver.m_impl == nullptr)
        return;

    uint64_t retireFrameId = 0u;
    {
        std::lock_guard lock(driver.m_impl->pendingUiOverlaySnapshotMutex);
        retireFrameId = driver.m_impl->pendingUiOverlaySnapshotFrameId != 0u
            ? driver.m_impl->pendingUiOverlaySnapshotFrameId
            : driver.m_impl->latestUiOverlaySnapshotFrameId;
    }

    driver.m_impl->uiOverlayRenderer.InvalidateFontAtlas(retireFrameId);
}

void DriverUIAccess::NotifyUiOverlaySwapchainWillResize(Driver& driver)
{
    if (driver.m_impl == nullptr)
        return;
}

Render::RHI::NativeHandle DriverUIAccess::GetRenderFinishedSemaphore(Driver& driver)
{
    std::lock_guard lock(driver.m_impl->sceneToUiWaitMutex);
    auto semaphore = driver.m_impl->sceneToUiWaitSemaphore;
    driver.m_impl->sceneToUiWaitSemaphore = {};
    return semaphore;
}

DriverUIAccess::UICompositionSyncBoundary DriverUIAccess::BuildUICompositionSyncBoundary(Driver& driver)
{
	UICompositionSyncBoundary boundary;
	boundary.sceneToUiWaitSemaphore = GetRenderFinishedSemaphore(driver);
	boundary.uiToPresentSignalSemaphore = driver.m_impl->uiRenderFinishedSemaphore;
	boundary.uiToPresentSignalValue = driver.m_impl->uiRenderFinishedValue;
	return boundary;
}

void DriverUIAccess::SetUICompositionSignal(
	Driver& driver,
	Render::RHI::NativeHandle semaphore,
	const uint64_t value)
{
	if (!semaphore.IsValid() || value == 0u)
	{
		driver.m_impl->uiRenderFinishedSemaphore = {};
		driver.m_impl->uiRenderFinishedValue = 0u;
		return;
	}

	driver.m_impl->uiRenderFinishedSemaphore = semaphore;
	driver.m_impl->uiRenderFinishedValue = value;
}

void DriverUIAccess::SetUISignalSemaphore(
	Driver& driver,
	Render::RHI::NativeHandle semaphore,
	const uint64_t value)
{
	SetUICompositionSignal(driver, semaphore, value);
}

void DriverTestAccess::SetExplicitDevice(Driver& driver, std::shared_ptr<Render::RHI::RHIDevice> explicitDevice)
{
    if (driver.m_impl == nullptr)
        return;

    const bool replacingDevice =
        driver.m_impl->explicitDevice != nullptr &&
        driver.m_impl->explicitDevice != explicitDevice;
    const bool clearingDevice = explicitDevice == nullptr;
    if ((replacingDevice || clearingDevice) &&
        !ReleaseExplicitDeviceResourcesForReplacement(*driver.m_impl, &driver))
    {
        return;
    }

	driver.m_impl->explicitDevice = std::move(explicitDevice);
    if (driver.m_impl->explicitDevice != nullptr)
    {
        if (driver.m_impl->pipelineCache == nullptr || replacingDevice)
            driver.m_impl->pipelineCache = Render::RHI::CreateDefaultPipelineCache();
        const auto threadedFrameContextCount = driver.m_impl->threadedLifecycle != nullptr
            ? static_cast<uint32_t>(driver.m_impl->threadedLifecycle->GetSlotCount())
            : 0u;
        if (!replacingDevice &&
            driver.m_impl->frameContexts.empty() &&
            threadedFrameContextCount > 0u)
        {
            RebuildExplicitFrameContextRing(
                *driver.m_impl,
                threadedFrameContextCount);
        }
    }
    else
    {
        driver.m_impl->pipelineCache.reset();
    }
}

void DriverTestAccess::RebuildExplicitFrameContexts(Driver& driver, const size_t frameContextCount)
{
    if (driver.m_impl == nullptr)
        return;

    RebuildExplicitFrameContextRing(
        *driver.m_impl,
        static_cast<uint32_t>(std::max<size_t>(1u, frameContextCount)));
}

void DriverTestAccess::SetExplicitSwapchain(Driver& driver, std::shared_ptr<Render::RHI::RHISwapchain> explicitSwapchain)
{
	driver.m_impl->explicitSwapchain = std::move(explicitSwapchain);
}

Render::RHI::RHIFrameContext& DriverTestAccess::EnsureFrameContext(Driver& driver, const size_t index)
{
	if (driver.m_impl->frameContexts.size() <= index)
		driver.m_impl->frameContexts.resize(index + 1u);
	return driver.m_impl->frameContexts[index];
}

const Render::RHI::RHIFrameContext* DriverTestAccess::PeekFrameContext(const Driver& driver, const size_t index)
{
	return index < driver.m_impl->frameContexts.size() ? &driver.m_impl->frameContexts[index] : nullptr;
}

void DriverTestAccess::SetCompletedReadbackTexture(
    Driver& driver,
    std::shared_ptr<Render::RHI::RHITexture> texture)
{
    Detail::RememberCompletedReadbackTexture(*driver.m_impl, texture);
}

void DriverTestAccess::SetCompletedReadbackTexture(
    Driver& driver,
    std::shared_ptr<Render::RHI::RHITexture> texture,
    const uint64_t generation)
{
    Detail::RememberCompletedReadbackTexture(*driver.m_impl, texture, generation);
}

size_t DriverTestAccess::GetRetainedThreadedSubmitResourceCount(const Driver& driver)
{
    if (driver.m_impl == nullptr)
        return 0u;

    size_t retainedResourceCount = 0u;
    for (const auto& keepAlive : driver.m_impl->retainedThreadedSubmitResourceKeepAliveByFrameContext)
        retainedResourceCount += keepAlive.size();
    return retainedResourceCount;
}

#if defined(NLS_ENABLE_TEST_HOOKS)
DriverImpl* DriverTestAccess::GetImplForTesting(Driver& driver)
{
    return driver.m_impl.get();
}

void DriverTestAccess::ShutdownRhiResourcesForTesting(Driver& driver)
{
    driver.ShutdownRhiResources();
}

size_t DriverTestAccess::GetPendingMeshRuntimeUploadCount(const Driver& driver)
{
    if (driver.m_impl == nullptr)
        return 0u;
    std::lock_guard lock(driver.m_impl->pendingMeshRuntimeUploadMutex);
    return driver.m_impl->pendingMeshRuntimeUploads.size();
}
#endif

void DriverTestAccess::SetExplicitFrameActive(Driver& driver, const bool active)
{
    driver.m_impl->explicitFrameActive = active;
}

void DriverTestAccess::AgePendingSwapchainResize(
    Driver& driver,
    const std::chrono::steady_clock::duration age)
{
    driver.m_impl->lastSwapchainResizeRequestTime = std::chrono::steady_clock::now() - age;
}

bool DriverTestAccess::TryLockThreadedRhiSubmission(Driver& driver)
{
    return driver.m_impl != nullptr &&
        driver.m_impl->threadedRhiSubmissionMutex.try_lock();
}

void DriverTestAccess::UnlockThreadedRhiSubmission(Driver& driver)
{
    if (driver.m_impl != nullptr)
        driver.m_impl->threadedRhiSubmissionMutex.unlock();
}

void DriverTestAccess::LockThreadedRhiSubmission(Driver& driver)
{
    if (driver.m_impl != nullptr)
        driver.m_impl->threadedRhiSubmissionMutex.lock();
}

void DriverTestAccess::SetUiStandaloneFramePending(Driver& driver, const bool pending)
{
    if (driver.m_impl == nullptr)
        return;

    driver.m_impl->uiStandaloneFramePending.store(pending, std::memory_order_release);
    driver.m_impl->uiStandaloneFramePendingUntilTickNs.store(
        pending ? std::numeric_limits<uint64_t>::max() : 0u,
        std::memory_order_release);
    NotifyThreadedWorkers(*driver.m_impl);
}

#if defined(NLS_ENABLE_TEST_HOOKS)
void DriverTestAccess::ExpireUiStandaloneFramePendingLease(Driver& driver)
{
    if (driver.m_impl == nullptr)
        return;

    const auto expiredAt = std::chrono::steady_clock::now() - std::chrono::nanoseconds(1);
    driver.m_impl->uiStandaloneFramePendingUntilTickNs.store(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            expiredAt.time_since_epoch()).count()),
        std::memory_order_release);
    driver.m_impl->uiStandaloneFramePending.store(true, std::memory_order_release);
    NotifyThreadedWorkers(*driver.m_impl);
}
#endif

bool DriverTestAccess::TryLockDriverTelemetry(Driver& driver)
{
    return driver.m_impl != nullptr &&
        driver.m_impl->driverTelemetryMutex.try_lock();
}

void DriverTestAccess::UnlockDriverTelemetry(Driver& driver)
{
    if (driver.m_impl != nullptr)
        driver.m_impl->driverTelemetryMutex.unlock();
}

const ThreadedRenderingLifecycle* DriverTestAccess::GetThreadedRenderingLifecycle(const Driver& driver)
{
    return driver.m_impl->threadedLifecycle.get();
}

ThreadedRenderingLifecycle* DriverTestAccess::GetThreadedRenderingLifecycle(Driver& driver)
{
    return driver.m_impl->threadedLifecycle.get();
}

bool DriverTestAccess::CanBeginStandaloneExplicitFrame(const Driver& driver)
{
    return RhiThreadCoordinator::CanBeginStandaloneExplicitFrame(driver);
}

bool DriverTestAccess::BeginStandaloneExplicitFrame(Driver& driver, const bool acquireSwapchainImage)
{
    return RhiThreadCoordinator::BeginStandaloneExplicitFrame(driver, acquireSwapchainImage);
}

void DriverTestAccess::EndStandaloneExplicitFrame(Driver& driver, const bool presentSwapchain)
{
    RhiThreadCoordinator::EndStandaloneExplicitFrame(driver, presentSwapchain);
}

void DriverTestAccess::PauseThreadedRenderingWorkers(Driver& driver)
{
    driver.m_impl->threadedStopRequested.store(true);
    NotifyThreadedWorkers(*driver.m_impl);
    if (driver.m_impl->renderSceneWorker.joinable())
        driver.m_impl->renderSceneWorker.join();
    if (driver.m_impl->rhiWorker.joinable())
        driver.m_impl->rhiWorker.join();
    driver.m_impl->threadedStopRequested.store(false);
}

bool DriverTestAccess::TryDrainThreadedRendering(Driver& driver, const bool applyPendingSwapchainResize)
{
    if (driver.m_impl == nullptr)
        return true;

    const bool drained = DrainThreadedLifecycleSynchronously(
        *driver.m_impl,
        &driver,
        applyPendingSwapchainResize);
    if (drained && applyPendingSwapchainResize)
        driver.ApplyPendingSwapchainResize();
    return drained;
}

void DriverTestAccess::DrainThreadedRendering(Driver& driver)
{
    (void)TryDrainThreadedRendering(driver);
}

bool DriverTestAccess::TryPublishHarnessFrameSnapshot(
    Driver& driver,
    const FrameSnapshot& snapshot,
    size_t* publishedSlotIndex)
{
    return RenderThreadCoordinator::TryPublishHarnessFrameSnapshot(
        driver,
        snapshot,
        publishedSlotIndex);
}

bool DriverTestAccess::TryPublishHarnessPreparedFrame(
    Driver& driver,
    const FrameSnapshot& snapshot,
    const RenderScenePackage& renderScenePackage,
    size_t* publishedSlotIndex)
{
    return RenderThreadCoordinator::TryPublishHarnessPreparedFrame(
        driver,
        snapshot,
        renderScenePackage,
        publishedSlotIndex);
}

bool DriverTestAccess::ResolveAndCompleteThreadedRenderScene(
    Driver& driver,
    const size_t slotIndex)
{
    if (driver.m_impl == nullptr || driver.m_impl->threadedLifecycle == nullptr)
        return false;

    const auto resolutionDesc = Detail::BuildRenderScenePreparingResolutionDesc();
    return driver.m_impl->threadedLifecycle->ResolveRenderScenePreparing(
        slotIndex,
        resolutionDesc);
}

Driver::Driver(const Render::Settings::DriverSettings& p_driverSettings)
    : m_impl(std::make_unique<DriverImpl>())
{
    m_impl->requestedGraphicsBackend = p_driverSettings.graphicsBackend;
    m_impl->lightGridEnabled = p_driverSettings.enableLightGrid;
    if (m_impl->requestedGraphicsBackend != Render::Settings::EGraphicsBackend::NONE &&
        !Render::Settings::IsBackendSelectableForPhase1(m_impl->requestedGraphicsBackend))
    {
        const auto phaseGateReport = Render::Settings::EvaluateBackendPhaseGate(
            m_impl->requestedGraphicsBackend,
            Render::Settings::RuntimeConsumer::Editor,
            Render::RHI::RHIDeviceCapabilities{});
        NLS_LOG_WARNING(
            "Driver: rejecting non-DX12 runtime backend during UE5 alignment phase 1: " +
            std::string(Render::Settings::ToString(m_impl->requestedGraphicsBackend)) +
            " phaseGate=" + phaseGateReport.summary);
    }
	m_impl->renderDocCaptureController = std::make_unique<Render::Tooling::RenderDocCaptureController>(p_driverSettings.renderDoc);

	// All backends use CreateRhiDevice for direct Tier A device creation
	// This creates the Formal RHI device without any IRenderDevice dependency
	m_impl->explicitDevice = Render::Backend::CreateRhiDevice(p_driverSettings);
	if (m_impl->explicitDevice == nullptr)
	{
		const auto phaseGateReport = Render::Settings::EvaluateBackendPhaseGate(
			p_driverSettings.graphicsBackend,
			Render::Settings::RuntimeConsumer::Editor,
			Render::RHI::RHIDeviceCapabilities{});
		NLS_LOG_WARNING(
			std::string("Driver: failed to create explicit RHI device for backend: ") +
			Render::Settings::ToString(p_driverSettings.graphicsBackend) +
			", continuing without device. phaseGate=" +
			phaseGateReport.summary);
	}

	// Set up pipeline state
	if (p_driverSettings.defaultPipelineState)
	{
		m_impl->defaultPipelineState = p_driverSettings.defaultPipelineState.value();
	}
	m_impl->pipelineState = m_impl->defaultPipelineState;

	// Setup RenderDoc if available
	if (m_impl->renderDocCaptureController != nullptr)
	{
		const auto nativeInfo = GetNativeDeviceInfo();
		m_impl->renderDocCaptureController->SetResolvedBackendName(
			Render::Settings::ToString(nativeInfo.backend));
		m_impl->renderDocCaptureController->SetCaptureTarget(nativeInfo);
	}

	if (m_impl->explicitDevice != nullptr)
	{
		m_impl->pipelineCache = Render::RHI::CreateDefaultPipelineCache();
		RebuildExplicitFrameContextRing(
            *m_impl,
            ResolveExplicitFrameContextCount(p_driverSettings));
	}

    m_impl->diagnostics = p_driverSettings.diagnostics;
    Render::Settings::SetThreadDiagnosticsSettings(p_driverSettings.diagnostics);

    if (p_driverSettings.enableThreadedRendering)
    {
        const auto slotCount = ResolveThreadedLifecycleSlotCount(p_driverSettings);
        m_impl->threadedLifecycle = std::make_unique<ThreadedRenderingLifecycle>(slotCount);
        m_impl->threadedPublishRetirementWaitMs = p_driverSettings.threadedPublishRetirementWaitMs;
        StartThreadedRenderingWorkers();
    }
}

Driver::~Driver()
{
    if (m_impl != nullptr)
        m_impl->swapchainWillResizeCallback = nullptr;
    ShutdownThreadedRendering();
    ShutdownRhiResources();
}

void Driver::ShutdownThreadedRendering()
{
    StopThreadedRenderingWorkers();
}

void Driver::ShutdownRhiResources()
{
    if (m_impl == nullptr)
        return;

    if (m_impl->unsafeGpuWorkQuarantined.load(std::memory_order_acquire) &&
        !m_impl->deviceLostDetected.load(std::memory_order_acquire))
    {
        PreserveUnsafeGpuWorkQuarantineResources(*m_impl);
        NLS_LOG_ERROR(
            "Driver::ShutdownRhiResources: preserving RHI resources because GPU work is quarantined without a reliable retirement fence");
        m_impl->explicitFrameActive = false;
        m_impl->uiStandaloneFrameActive = false;
        m_impl->uiStandaloneFramePending.store(false, std::memory_order_release);
        m_impl->uiStandaloneFramePendingUntilTickNs.store(0u, std::memory_order_release);
        if (m_impl->uiStandaloneFrameSubmissionLock.owns_lock())
            m_impl->uiStandaloneFrameSubmissionLock.unlock();
        return;
    }

    m_impl->swapchainWillResizeCallback = nullptr;
    m_impl->uiRenderFinishedSemaphore = {};
    m_impl->uiRenderFinishedValue = 0u;
    {
        std::lock_guard lock(m_impl->sceneToUiWaitMutex);
        m_impl->sceneToUiWaitSemaphore = {};
    }
    {
        std::lock_guard lock(m_impl->completedReadbackTextureMutex);
        m_impl->completedReadbackTexture = nullptr;
        m_impl->completedReadbackTextureGeneration = 0u;
        m_impl->submittedReadbackTextureGenerations.clear();
        m_impl->completedReadbackTextureHistory.clear();
    }
    m_impl->explicitFrameActive = false;
    m_impl->uiStandaloneFrameActive = false;
    m_impl->uiStandaloneFramePending.store(false, std::memory_order_release);
    m_impl->uiStandaloneFramePendingUntilTickNs.store(0u, std::memory_order_release);
    if (m_impl->uiStandaloneFrameSubmissionLock.owns_lock())
        m_impl->uiStandaloneFrameSubmissionLock.unlock();
    m_impl->hasPendingSwapchainResize = false;

    if (m_impl->threadedLifecycle != nullptr)
        m_impl->threadedLifecycle->ReleaseRetainedFrameResources();

    const bool abandonFenceWait = m_impl->deviceLostDetected.load(std::memory_order_acquire);
    const bool drainedFrameFences =
        abandonFenceWait ||
        DrainFrameFencesForResize(m_impl->frameContexts, m_impl.get());
    if (drainedFrameFences && !abandonFenceWait)
        Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(*m_impl);
    bool releasedAllFrameContexts = drainedFrameFences;
    for (auto& frameContext : m_impl->frameContexts)
    {
        releasedAllFrameContexts =
            ReleaseFrameContextResources(frameContext, m_impl.get(), abandonFenceWait) &&
            releasedAllFrameContexts;
    }
    if (!releasedAllFrameContexts)
    {
        NLS_LOG_ERROR("Driver::ShutdownRhiResources: timed out before releasing frame-context resources");
        return;
    }
    m_impl->retainedThreadedSubmitResourceKeepAliveByFrameContext.clear();
    m_impl->deferredThreadedFrameScopedRetirementFrameContexts.clear();
    m_impl->deferredUiTextureRetirementFrameIdsByFrameContext.clear();
    m_impl->frameContexts.clear();

    m_impl->pipelineCache.reset();
    m_impl->explicitSwapchain.reset();
    m_impl->renderDocCaptureController.reset();
    m_impl->explicitDevice.reset();
}

void Driver::SetPipelineState(Render::Data::PipelineState p_state)
{
	// Formal RHI path uses RHIGraphicsPipeline bound directly via commandBuffer->BindGraphicsPipeline()
	// This method is deprecated - only updates internal state tracking
	m_impl->pipelineState = p_state;
}

Render::Settings::RuntimeBackendReadinessDecision Driver::EvaluateEditorMainRuntimeReadiness(
	const Render::Settings::EGraphicsBackend requestedBackend) const
{
	const Render::RHI::RHIDeviceCapabilities capabilities = m_impl->explicitDevice != nullptr
		? m_impl->explicitDevice->GetCapabilities()
		: Render::RHI::RHIDeviceCapabilities{};
	return Render::Settings::EvaluateEditorMainRuntimeReadiness(requestedBackend, capabilities);
}

Render::Settings::RuntimeBackendReadinessDecision Driver::EvaluateGameMainRuntimeReadiness(
	const Render::Settings::EGraphicsBackend requestedBackend) const
{
	const Render::RHI::RHIDeviceCapabilities capabilities = m_impl->explicitDevice != nullptr
		? m_impl->explicitDevice->GetCapabilities()
		: Render::RHI::RHIDeviceCapabilities{};
	return Render::Settings::EvaluateGameMainRuntimeReadiness(requestedBackend, capabilities);
}

std::optional<std::string> Driver::GetEditorPickingReadbackWarning() const
{
	const Render::RHI::RHIDeviceCapabilities capabilities = m_impl->explicitDevice != nullptr
		? m_impl->explicitDevice->GetCapabilities()
		: Render::RHI::RHIDeviceCapabilities{};
	return Render::Settings::GetEditorPickingReadbackWarning(capabilities);
}

Render::Settings::EGraphicsBackend Driver::GetActiveGraphicsBackend() const
{
	if (m_impl->explicitDevice == nullptr)
		return Render::Settings::EGraphicsBackend::NONE;

	return Render::Settings::ToGraphicsBackend(
		m_impl->explicitDevice->GetNativeDeviceInfo().backend);
}

Render::RHI::NativeRenderDeviceInfo Driver::GetNativeDeviceInfo() const
{
	if (m_impl->explicitDevice != nullptr)
	{
		return m_impl->explicitDevice->GetNativeDeviceInfo();
	}
	return {};
}

bool Driver::CreatePlatformSwapchain(
	void* platformWindow,
	void* nativeWindowHandle,
	const uint32_t width,
	const uint32_t height,
	const bool vsync,
	const uint32_t imageCount)
{
	Render::RHI::SwapchainDesc desc;
	desc.platformWindow = platformWindow;
	desc.nativeWindowHandle = nativeWindowHandle;
	desc.width = width;
	desc.height = height;
	desc.vsync = vsync;
	desc.imageCount = imageCount;
	return CreateSwapchain(desc);
}

void Driver::ResizePlatformSwapchain(const uint32_t width, const uint32_t height)
{
	if (width == 0u || height == 0u)
		return;
	ResizeSwapchain(width, height);

	// Interactive window-edge resizing should update the swapchain before the next
	// frame is rendered. Otherwise DXGI can stretch the previous backbuffer to the
	// new client rect for one frame, which shows up as UI stretching/ghosting.
	if (!m_impl->explicitFrameActive)
		ApplyPendingSwapchainResize();
}

void Driver::SetSwapchainWillResizeCallback(SwapchainWillResizeCallback callback)
{
	m_impl->swapchainWillResizeCallback = std::move(callback);
}

bool Driver::CreateSwapchain(const Render::RHI::SwapchainDesc& desc)
{
	m_impl->explicitSwapchain = m_impl->explicitDevice->CreateSwapchain(desc);
	NLS_ASSERT(m_impl->explicitSwapchain != nullptr, "Failed to create swapchain for explicit RHI");
	// RenderDoc targets the explicit device's native info
	if (m_impl->renderDocCaptureController != nullptr)
		m_impl->renderDocCaptureController->SetCaptureTarget(m_impl->explicitDevice->GetNativeDeviceInfo());
	return m_impl->explicitSwapchain != nullptr;
}

void Driver::DestroySwapchain()
{
	m_impl->explicitSwapchain.reset();
}

void Driver::ResizeSwapchain(uint32_t width, uint32_t height)
{
	if (width == 0u || height == 0u)
		return;

	if (m_impl->explicitSwapchain != nullptr)
	{
		const auto& swapchainDesc = m_impl->explicitSwapchain->GetDesc();
		if (!m_impl->hasPendingSwapchainResize &&
			swapchainDesc.width == width &&
			swapchainDesc.height == height)
		{
			return;
		}
	}

	if (m_impl->hasPendingSwapchainResize &&
		m_impl->pendingSwapchainWidth == width &&
		m_impl->pendingSwapchainHeight == height)
	{
		return;
	}

	m_impl->pendingSwapchainWidth = width;
	m_impl->pendingSwapchainHeight = height;
	m_impl->hasPendingSwapchainResize = true;
	m_impl->lastSwapchainResizeRequestTime = std::chrono::steady_clock::now();
}

void Driver::PresentSwapchain()
{
	RhiThreadCoordinator::PresentSwapchain(*this);
}

void Driver::ApplyPendingSwapchainResize()
{
	if (!m_impl->hasPendingSwapchainResize)
		return;

	if (!ShouldApplyPendingSwapchainResize(
		std::chrono::steady_clock::now() - m_impl->lastSwapchainResizeRequestTime))
		return;

    if (m_impl->threadedLifecycle != nullptr && m_impl->threadedLifecycle->GetInFlightDepth() > 0u)
        return;

    std::unique_lock<std::timed_mutex> threadedSubmissionLock;
    if (m_impl->threadedLifecycle != nullptr)
    {
        threadedSubmissionLock = std::unique_lock<std::timed_mutex>(
            m_impl->threadedRhiSubmissionMutex,
            std::try_to_lock);
        if (!threadedSubmissionLock.owns_lock())
            return;
    }

    if (m_impl->threadedLifecycle != nullptr && m_impl->threadedLifecycle->GetInFlightDepth() > 0u)
        return;

    if (!m_impl->deferredThreadedFrameScopedRetirementFrameContexts.empty())
    {
        Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(*m_impl);
        if (!m_impl->deferredThreadedFrameScopedRetirementFrameContexts.empty())
            return;
    }

	const uint32_t width = m_impl->pendingSwapchainWidth;
	const uint32_t height = m_impl->pendingSwapchainHeight;

    if (!DrainFrameFencesForResize(m_impl->frameContexts, m_impl.get()))
    {
        m_impl->lastSwapchainResizeRequestTime = std::chrono::steady_clock::now();
        return;
    }
    Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(*m_impl);

	if (m_impl->swapchainWillResizeCallback)
		m_impl->swapchainWillResizeCallback();

	for (auto& frameContext : m_impl->frameContexts)
	{
		if (frameContext.commandBuffer != nullptr)
			frameContext.commandBuffer->Reset();
		if (frameContext.commandPool != nullptr)
			frameContext.commandPool->Reset();
        for (const auto& childCommandBuffer : frameContext.childCommandBuffers)
        {
            if (childCommandBuffer != nullptr)
                childCommandBuffer->Reset();
        }
        for (const auto& childCommandPool : frameContext.childCommandPools)
        {
            if (childCommandPool != nullptr)
                childCommandPool->Reset();
        }
        for (const auto& parallelCommandBuffer : frameContext.parallelCommandBuffers)
        {
            if (parallelCommandBuffer != nullptr)
                parallelCommandBuffer->Reset();
        }
        for (const auto& parallelCommandPool : frameContext.parallelCommandPools)
        {
            if (parallelCommandPool != nullptr)
                parallelCommandPool->Reset();
        }
		if (frameContext.resourceStateTracker != nullptr)
			frameContext.resourceStateTracker->Reset();
	}

    {
        std::lock_guard lock(m_impl->completedReadbackTextureMutex);
	    m_impl->completedReadbackTexture = nullptr;
        m_impl->completedReadbackTextureGeneration = 0u;
        m_impl->submittedReadbackTextureGenerations.clear();
        m_impl->completedReadbackTextureHistory.clear();
    }
	for (auto& frameContext : m_impl->frameContexts)
	{
		frameContext.hasAcquiredSwapchainImage = false;
		frameContext.swapchainImageIndex = 0u;
		frameContext.swapchainBackbufferView = nullptr;
		frameContext.swapchainDepthStencilTexture = nullptr;
		frameContext.swapchainDepthStencilView = nullptr;
		frameContext.explicitReadbackTexture = nullptr;
        frameContext.explicitReadbackTextureGeneration = 0u;
	}
    {
        std::lock_guard lock(m_impl->sceneToUiWaitMutex);
        m_impl->sceneToUiWaitSemaphore = {};
    }

	if (m_impl->explicitSwapchain != nullptr)
	{
		if (!m_impl->explicitSwapchain->Resize(width, height))
		{
			m_impl->lastSwapchainResizeRequestTime = std::chrono::steady_clock::now();
			return;
		}
	}

	m_impl->hasPendingSwapchainResize = false;
}

void Driver::SetPolygonMode(Settings::ERasterizationMode mode)
{
	m_impl->defaultPipelineState.rasterizationMode = mode;
}

void Driver::StartThreadedRenderingWorkers()
{
    NLS_PROFILE_SCOPE();
    if (m_impl->threadedLifecycle == nullptr || m_impl->threadedWorkersRunning)
        return;

    if (!Detail::SupportsThreadedFoundationExecution(*m_impl))
        return;

    m_impl->threadedStopRequested.store(false);
    m_impl->threadedWorkersRunning = true;
    m_impl->renderSceneWorker = std::thread([this]()
    {
        NLS_PROFILE_REGISTER_THREAD("Render Thread");
        Render::Settings::SetThreadDiagnosticsSettings(m_impl->diagnostics);
        while (!m_impl->threadedStopRequested.load())
        {
            NLS_PROFILE_NAMED_SCOPE("Render Thread Tick");
            const uint64_t observedWakeGeneration =
                m_impl->threadedWorkerWakeGeneration.load(std::memory_order_acquire);
            size_t slotIndex = 0u;
            if (m_impl->threadedLifecycle->TryBeginNextRenderFrameBuild(
                &slotIndex,
                nullptr))
            {
                const auto resolutionDesc = Detail::BuildRenderScenePreparingResolutionDesc();
                {
                    NLS_PROFILE_NAMED_SCOPE("RenderThread::ResolveRenderScenePreparing");
                    m_impl->threadedLifecycle->ResolveRenderScenePreparing(
                        slotIndex,
                        resolutionDesc);
                }
                {
                    NLS_PROFILE_NAMED_SCOPE("RenderThread::NotifyThreadedWorkers");
                    NotifyThreadedWorkers(*m_impl);
                }
                continue;
            }

            {
                NLS_PROFILE_NAMED_SCOPE("RenderThread::WaitForWake");
                WaitForThreadedWorkerWake(*m_impl, observedWakeGeneration);
            }
        }
    });
    m_impl->rhiWorker = std::thread([this]()
    {
        NLS_PROFILE_REGISTER_THREAD("RHI Thread");
        Render::Settings::SetThreadDiagnosticsSettings(m_impl->diagnostics);
        while (!m_impl->threadedStopRequested.load())
        {
            NLS_PROFILE_NAMED_SCOPE("RHI Thread Tick");
            const uint64_t observedWakeGeneration =
                m_impl->threadedWorkerWakeGeneration.load(std::memory_order_acquire);
            {
                NLS_PROFILE_NAMED_SCOPE("RhiThread::TryExecuteNextThreadedSubmission");
                if (RhiThreadCoordinator::TryExecuteNextThreadedSubmission(
                    *this,
                    RhiSubmissionAttribution::Worker))
                {
                    continue;
                }
            }

            {
                NLS_PROFILE_NAMED_SCOPE("RhiThread::WaitForWake");
                WaitForThreadedWorkerWake(*m_impl, observedWakeGeneration);
            }
        }
    });
}

void Driver::StopThreadedRenderingWorkers()
{
    NLS_PROFILE_SCOPE();
    if (m_impl == nullptr)
        return;
    m_impl->threadedStopRequested.store(true);
    NotifyThreadedWorkers(*m_impl);
    if (m_impl->renderSceneWorker.joinable())
        m_impl->renderSceneWorker.join();
    if (m_impl->rhiWorker.joinable())
        m_impl->rhiWorker.join();
    const bool drained = DrainThreadedLifecycleSynchronously(*m_impl, this);
    if (drained && m_impl->swapchainWillResizeCallback != nullptr)
        ApplyPendingSwapchainResize();
    m_impl->threadedWorkersRunning = false;
}

bool Driver::IsThreadedRenderingEnabled() const
{
    if (m_impl->threadedLifecycle == nullptr || !m_impl->threadedWorkersRunning)
        return false;

    return Detail::SupportsThreadedFoundationExecution(*m_impl);
}

}

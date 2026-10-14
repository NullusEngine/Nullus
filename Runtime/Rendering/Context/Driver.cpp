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
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <Math/Vector4.h>

#include "Profiling/Profiler.h"
#include "TaskGraph/TaskGraphSystem.h"
#include "Threading/ExecutionThreadBudget.h"
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
#include "Rendering/Context/RenderCommandPipe.h"
#include "Rendering/Context/RenderCommandFence.h"
#include "Rendering/Context/RenderingThreadSystem.h"
#include "Rendering/Context/RhiCommandListExecutor.h"
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

namespace NLS::Render::Settings
{
namespace
{
thread_local EngineDiagnosticsSettings g_threadDiagnosticsSettings;
}

NLS_RENDER_API const EngineDiagnosticsSettings& GetThreadDiagnosticsSettings()
{
    return g_threadDiagnosticsSettings;
}

NLS_RENDER_API void SetThreadDiagnosticsSettings(const EngineDiagnosticsSettings& settings)
{
    g_threadDiagnosticsSettings = settings;
}
}

namespace NLS::Render::Context
{
namespace
{
    constexpr const char* kDriverTelemetryDelimiter = " | driver: ";

    bool PipelineRequiresBindingSet(
        const RHI::RHIGraphicsPipelineDesc& pipelineDesc,
        const uint32_t setIndex)
    {
        if (pipelineDesc.pipelineLayout == nullptr)
            return false;

        for (const auto& bindingLayout : pipelineDesc.pipelineLayout->GetDesc().bindingLayouts)
        {
            if (bindingLayout == nullptr)
                continue;
            for (const auto& entry : bindingLayout->GetDesc().entries)
            {
                if (entry.set == setIndex)
                    return true;
            }
        }

        return false;
    }

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
        {
            std::lock_guard retirementLock(impl.frameResourceRetirementMutex);
            for (const auto& resource : impl.unfencedReadbackResources)
                keepAlive.push_back(resource);
            if (impl.frameResourceSlots != nullptr)
            {
                for (size_t slotIndex = 0u; slotIndex < impl.frameResourceSlots->GetSlotCount(); ++slotIndex)
                {
                    for (const auto& resource : impl.frameResourceSlots->PeekSlot(slotIndex)->ownedResources)
                        if (resource != nullptr)
                            keepAlive.push_back(resource);
                }
            }
        }
        for (size_t frameContextIndex = 0u; frameContextIndex < impl.GetFrameContextCount(); ++frameContextIndex)
        {
            const auto& frameContext = impl.GetFrameContext(frameContextIndex);
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
            if (frameContext.sceneColorView != nullptr)
                keepAlive.push_back(frameContext.sceneColorView);
            if (frameContext.sceneDepthStencilView != nullptr)
                keepAlive.push_back(frameContext.sceneDepthStencilView);
            if (frameContext.sceneOutputBindingSet != nullptr)
                keepAlive.push_back(frameContext.sceneOutputBindingSet);
            if (frameContext.explicitReadbackTexture != nullptr)
                keepAlive.push_back(frameContext.explicitReadbackTexture);
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

        telemetry.descriptorAllocationFailures =
            impl != nullptr && impl->descriptorAllocationFailureCount != nullptr
            ? impl->descriptorAllocationFailureCount->load(std::memory_order_relaxed)
            : 0u;

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
                FrameGraph::FrameGraphExecutionContext::RecordResourceBarriers(
                    commandBuffer, *frameContext, attachmentBarriers);
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
            FrameGraph::FrameGraphExecutionContext::RecordResourceBarriers(
                commandBuffer, *frameContext, attachmentBarriers);
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
            return FrameGraph::FrameGraphExecutionContext::RecordResourceBarriers(
                commandBuffer, *frameContext, barriers).Succeeded();
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
        const bool requiresObjectBindingSet = PipelineRequiresBindingSet(
            pipelineDesc,
            ::NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet);
        if (requiresObjectBindingSet && drawCommand.objectBindingSet == nullptr)
        {
            NLS_LOG_ERROR(
                "[Driver] recorded draw rejected: pipeline requires object descriptor set " +
                std::to_string(::NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet) +
                " but no object binding set was captured; pipeline=" + pipelineDesc.debugName +
                " usesObjectIndex=" + std::to_string(drawCommand.usesObjectIndex ? 1u : 0u));
            return false;
        }

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
        commandBuffer.PushConstants(
            ::NLS::Render::Resources::kIndexedObjectDataPushConstantStageMask,
            0u,
            ::NLS::Render::Resources::kIndexedObjectDataPushConstantSize,
            &drawCommand.objectConstants);

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

        if (input.kind == RenderPassCommandKind::SwapchainOutput)
        {
            if (input.drawCount == 0u)
                return 0u;
            if (input.outputPipeline == nullptr || input.outputBindingSet == nullptr)
                return 0u;
            commandBuffer->BindGraphicsPipeline(input.outputPipeline);
            commandBuffer->BindBindingSet(0u, input.outputBindingSet);
            commandBuffer->SetScissor({ 0, 0, input.renderWidth, input.renderHeight });
            commandBuffer->Draw(3u, 1u, 0u, 0u);
            return 1u;
        }

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

    bool SupportsThreadedFoundationExecution(const DriverImpl& impl)
    {
        if (impl.explicitDevice == nullptr)
            return true;

        const auto nativeDeviceInfo = impl.explicitDevice->GetNativeDeviceInfo();
        return Render::Settings::SupportsThreadedRenderFoundationPath(
            nativeDeviceInfo.backend,
            impl.explicitDevice->GetCapabilities());
    }

    struct PendingUiSnapshotConsumer final
    {
        explicit PendingUiSnapshotConsumer(DriverImpl& owner)
            : impl(owner), lifetime(owner.renderCommandFlushLifetime)
        {
            ++impl.pendingUiSnapshotConsumerCommands;
        }

        ~PendingUiSnapshotConsumer()
        {
            if (!lifetime.expired())
                --impl.pendingUiSnapshotConsumerCommands;
        }

        DriverImpl& impl;
        std::weak_ptr<DriverImpl::RenderCommandFlushLifetime> lifetime;
    };


    void AdvanceUiOverlayPresentationInvalidationLocked(DriverImpl& impl)
    {
        ++impl.uiOverlayPresentationInvalidationGeneration;
        if (impl.uiOverlayPresentationInvalidationGeneration == 0u)
            impl.uiOverlayPresentationInvalidationGeneration = 1u;
    }

    void MarkSwapchainUiContentPublishedLocked(
        DriverImpl& impl,
        const Render::UI::UiDrawDataSnapshot& snapshot,
        const uint64_t observedPresentationInvalidationGeneration)
    {
        impl.lastPublishedSwapchainUiContentSignature =
            Render::UI::ResolveUiDrawDataContentSignature(snapshot);
        impl.lastPublishedSwapchainUiInvalidationGeneration =
            observedPresentationInvalidationGeneration;
    }

    void NotifyThreadedWorkers(DriverImpl& impl);
    bool WaitForThreadedWorkerWake(
        DriverImpl& impl,
        uint64_t observedGeneration,
        std::chrono::milliseconds timeout = kThreadedWorkerWakeTimeout);

    void ExecuteDriverRenderCommand(DriverImpl& impl, std::function<void()> command)
    {
        if (!RenderingThreadSystem::IsInRhiThread() &&
            (!impl.renderingThreads.IsInitialized() || impl.renderingThreads.IsSuspended() ||
             !RenderingThreadSystem::HasActiveRenderThread() || RenderingThreadSystem::IsInRenderingThread()))
        {
            command();
            return;
        }

        const std::weak_ptr<DriverImpl::RenderCommandFlushLifetime> lifetime = impl.renderCommandFlushLifetime;
        const auto executed = std::make_shared<std::atomic_bool>(false);
        const auto completion = EnqueueRenderCommand<struct DriverOrderedCommand>(
            [lifetime, executed, command = std::move(command)]
            {
                if (lifetime.expired())
                    return;
                command();
                executed->store(true, std::memory_order_release);
            });
        NotifyThreadedWorkers(impl);

        // A submit callback cannot wait for render work that flushes its own lane.
        if (RenderingThreadSystem::IsInRhiThread())
            return;
        if (!completion.WaitFor(impl.renderDrainTimeout))
        {
            ReportRenderFatalError("Driver: timed out waiting for an ordered render command");
            throw std::runtime_error("Driver: timed out waiting for an ordered render command");
        }
        if (!completion.Succeeded())
            throw std::runtime_error("Driver: ordered render command failed: " + completion.GetDiagnostic());
        if (!executed->load(std::memory_order_acquire))
            throw std::runtime_error("Driver: ordered render command was abandoned: " + GetRenderFatalError());
    }

    bool DrainRenderFrameCommands(
        DriverImpl& impl,
        Driver* driver = nullptr,
        const bool applyPendingSwapchainResize = true,
        const std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt)
    {
        NLS_PROFILE_SCOPE();
        if (driver == nullptr)
            return true;

        const auto drainDeadline = deadline.value_or(
            std::chrono::steady_clock::now() + impl.renderDrainTimeout);
        while (Detail::GetPendingCpuFrameCount(impl) > 0u)
        {
            const uint64_t observedWakeGeneration =
                impl.threadedWorkerWake.Generation();
            bool progressed = false;

            if (impl.renderingThreads.IsInitialized() && !impl.renderingThreads.IsSuspended() &&
                !RenderingThreadSystem::IsInRenderingThread())
            {
                if (impl.renderingThreads.IsStopRequested())
                    return false;
                if (std::chrono::steady_clock::now() >= drainDeadline)
                {
                    NLS_LOG_ERROR("Driver: timed out waiting for the render thread to drain frames");
                    return false;
                }
                NotifyThreadedWorkers(impl);
                WaitForThreadedWorkerWake(impl, observedWakeGeneration, kThreadedDrainWakeTimeout);
                continue;
            }

            // The watchdog bounds the whole drain, not just the no-progress branch.
            // A worker that keeps reporting progress without retiring a frame would
            // otherwise spin here forever, because "reported progress" is not the
            // same as "will finish".
            if (std::chrono::steady_clock::now() >= drainDeadline)
            {
                NLS_LOG_ERROR(
                    "Driver: timed out waiting for CPU frame work count=" +
                    std::to_string(Detail::GetPendingCpuFrameCount(impl)));
                return false;
            }

            {
                NLS_PROFILE_NAMED_SCOPE("Driver::DrainRenderFrames");
                progressed = RenderCommandDispatcher::Dispatch() > 0u;
            }

            {
                NLS_PROFILE_NAMED_SCOPE("Driver::DrainRhiSubmissions");
                if (RhiCommandListExecutor::FinalizeThreadedSubmissionProduction(
                    *driver, applyPendingSwapchainResize))
                {
                    progressed = true;
                }
            }

            if (!progressed)
            {
                // A worker can retire the final slot after the loop condition or a
                // failed ownership attempt. Avoid turning that completion into a
                // full drain-wake timeout.
                if (Detail::GetPendingCpuFrameCount(impl) == 0u)
                    continue;

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
        impl.threadedWorkerWake.Trigger();
        if (impl.taskGraph != nullptr)
            impl.taskGraph->WakeNamedThread(NLS::Base::TaskGraph::NamedThread::Render);
        if (impl.taskGraph != nullptr)
            impl.taskGraph->WakeNamedThread(NLS::Base::TaskGraph::NamedThread::Rhi);
    }

    bool WaitForThreadedWorkerWake(
        DriverImpl& impl,
        const uint64_t observedGeneration,
        const std::chrono::milliseconds timeout)
    {
        NLS_PROFILE_SCOPE();
        if (impl.threadedStopRequested.load(std::memory_order_acquire))
            return true;
        if (impl.threadedWorkerWake.Generation() != observedGeneration)
            return true;
        return impl.threadedWorkerWake.WaitForGeneration(observedGeneration, timeout) ||
            impl.threadedStopRequested.load(std::memory_order_acquire);
    }

    bool DrainFrameFenceForResize(
        DriverImpl& impl,
        const size_t frameContextIndex)
    {
        std::shared_ptr<Render::RHI::RHIFence> retirementFence;
        {
            std::lock_guard retirementLock(impl.frameResourceRetirementMutex);
            if (impl.frameResourceSlots == nullptr)
                return true;
            const auto* slot = impl.frameResourceSlots->PeekSlot(frameContextIndex);
            if (slot == nullptr)
                return true;
            if (slot->quarantined)
                return false;
            retirementFence = slot->gpuFence;
        }
        if (retirementFence == nullptr || retirementFence->IsSignaled())
            return true;

        if (impl.deviceLostDetected.load(std::memory_order_acquire))
        {
            NLS_LOG_ERROR(
                "Driver: abandoned submitted GPU work fence wait after device loss fence=" +
                std::string(retirementFence->GetDebugName()) +
                " frameContextIndex=" + std::to_string(frameContextIndex));
            return false;
        }

        const auto gpuDrainTimeout = std::chrono::duration_cast<std::chrono::nanoseconds>(impl.renderDrainTimeout);
        if (retirementFence->Wait(static_cast<uint64_t>(gpuDrainTimeout.count())))
            return true;

        NLS_LOG_ERROR(
            "Driver: timed out waiting for submitted GPU work frame fence=" +
            std::string(retirementFence->GetDebugName()) +
            " frameContextIndex=" + std::to_string(frameContextIndex));
        return false;
    }

    bool DrainFrameFencesForResize(
        DriverImpl& impl)
    {
        for (size_t frameContextIndex = 0u; frameContextIndex < impl.GetFrameContextCount(); ++frameContextIndex)
        {
            if (!DrainFrameFenceForResize(impl, frameContextIndex))
                return false;
        }
        return true;
    }

    bool ReleaseFrameContextResources(
        DriverImpl& impl,
        const size_t frameContextIndex,
        const bool abandonFenceWait = false)
    {
        if (!abandonFenceWait && !DrainFrameFenceForResize(impl, frameContextIndex))
            return false;

        auto& frameContext = impl.GetFrameContext(frameContextIndex);
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

    uint32_t ResolveExplicitFrameContextCount(const Render::Settings::DriverSettings& settings)
    {
        return std::max<uint32_t>(1u, settings.framesInFlight);
    }

    void RebuildExplicitFrameContextRing(
        DriverImpl& impl,
        const uint32_t frameCount)
    {
        if (!DrainFrameFencesForResize(impl))
            return;
        Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(impl);
        for (size_t frameContextIndex = 0u; frameContextIndex < impl.GetFrameContextCount(); ++frameContextIndex)
            (void)ReleaseFrameContextResources(impl, frameContextIndex);
        {
            std::lock_guard lock(impl.frameResourceRetirementMutex);
            impl.frameResourceSlots.reset();
            impl.reservedFrameResourceLease.reset();
        }

        if (impl.explicitDevice == nullptr)
            return;

        impl.EnsureFrameContexts(frameCount);
        for (uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            auto& frameContext = impl.GetFrameContext(frameIndex);
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
            frameContext.descriptorAllocator = Render::RHI::CreateDefaultDescriptorAllocator(
                65536u,
                impl.descriptorAllocationFailureCount);
            frameContext.uploadContext = Render::Backend::CreateUploadContextForRhiDevice(impl.explicitDevice);
        }
        // Resource capacity does not determine the permitted CPU queue depth.
        impl.frameLatency = std::make_unique<FrameLatencyController>(
            impl.cpuFrameLatencyLimit == 0u ? frameCount : (std::min)(frameCount, impl.cpuFrameLatencyLimit));
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

        {
            std::lock_guard lock(impl.uiCompositionSignalMutex);
            impl.uiRenderFinishedSemaphore = {};
            impl.uiRenderFinishedValue = 0u;
        }
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
        impl.hasPendingSwapchainResize = false;

        std::unique_lock<std::timed_mutex> threadedSubmissionLock;
        if (impl.frameResourceSlots != nullptr)
        {
            if (driver == nullptr || !DrainRenderFrameCommands(impl, driver))
            {
                NLS_LOG_ERROR("DriverTestAccess::SetExplicitDevice: timed out draining threaded rendering before replacing explicit device");
                return false;
            }

            threadedSubmissionLock = std::unique_lock<std::timed_mutex>(
                impl.threadedRhiSubmissionMutex,
                std::defer_lock);
            if (!threadedSubmissionLock.try_lock_for(impl.renderDrainTimeout))
            {
                NLS_LOG_ERROR("DriverTestAccess::SetExplicitDevice: timed out waiting for threaded RHI submission before replacing explicit device");
                return false;
            }
            if (Detail::GetPendingCpuFrameCount(impl) > 0u)
            {
                NLS_LOG_ERROR("DriverTestAccess::SetExplicitDevice: threaded rendering became active while replacing explicit device");
                return false;
            }

        }

        const bool abandonFenceWait = impl.deviceLostDetected.load(std::memory_order_acquire);
        const bool drainedFrameFences =
            abandonFenceWait ||
            DrainFrameFencesForResize(impl);
        if (drainedFrameFences && !abandonFenceWait)
            Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(impl);
        bool releasedAllFrameContexts = drainedFrameFences;
        for (size_t frameContextIndex = 0u; frameContextIndex < impl.GetFrameContextCount(); ++frameContextIndex)
        {
            releasedAllFrameContexts =
                ReleaseFrameContextResources(impl, frameContextIndex, abandonFenceWait) &&
                releasedAllFrameContexts;
        }
        if (!releasedAllFrameContexts)
        {
            NLS_LOG_ERROR("DriverTestAccess::SetExplicitDevice: timed out before replacing explicit device resources");
            return false;
        }

        // A recorded UI upload owns resources and a completion token from the
        // device being replaced.  Dropping it silently would leave the editor
        // waiting forever for the old request id.  Convert those requests to
        // retryable failures after the old frame resources have retired;
        // unrecorded CPU pixel requests remain queued and can be replayed on
        // the replacement device.
        {
            std::lock_guard lock(impl.pendingUiRgba8TextureUploadMutex);
            for (const auto& recorded : impl.recordedUiRgba8TextureUploads)
            {
                if (impl.canceledUiRgba8TextureUploadRequestIds.erase(recorded.requestId) != 0u)
                    continue;
                impl.completedUiRgba8TextureUploads[recorded.requestId] = {
                    false,
                    nullptr,
                    nullptr,
                    recorded.width,
                    recorded.height,
                    "UI RGBA8 texture upload invalidated by device replacement"
                };
            }
            impl.recordedUiRgba8TextureUploads.clear();
            for (auto& [_, completed] : impl.completedUiRgba8TextureUploads)
            {
                completed.success = false;
                completed.texture.reset();
                completed.textureView.reset();
                if (completed.diagnostic.empty())
                    completed.diagnostic = "UI RGBA8 texture upload invalidated by device replacement";
            }
            impl.canceledUiRgba8TextureUploadRequestIds.clear();
            impl.uiRgba8TextureAtlasPages.clear();
        }

        {
            std::lock_guard lock(impl.frameResourceRetirementMutex);
            impl.frameResourceSlots.reset();
            impl.reservedFrameResourceLease.reset();
        }
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


void Detail::NotifyThreadedWorkers(DriverImpl& impl)
{
    NLS::Render::Context::NotifyThreadedWorkers(impl);
}

bool Detail::WaitForThreadedWorkerWake(DriverImpl& impl, const uint64_t observedGeneration)
{
    return NLS::Render::Context::WaitForThreadedWorkerWake(impl, observedGeneration);
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

void DriverRendererAccess::PrepareSwapchainSceneOutput(Driver& driver, RenderScenePackage& package)
{
    if (!package.targetsSwapchain || !HasExplicitRHI(driver) || driver.m_impl->explicitSwapchain == nullptr)
        return;
    if (!RenderingThreadSystem::IsInRenderingThread())
        throw std::logic_error("Swapchain scene output must be prepared on the render thread");
    const auto slotIndex = GetActiveFrameContextSlotIndex(driver);
    if (!slotIndex.has_value())
        throw std::logic_error("Swapchain scene output requires a frame resource lease");
    {
        std::lock_guard lock(driver.m_impl->frameResourceRetirementMutex);
        const auto* slot = driver.m_impl->frameResourceSlots->PeekSlot(*slotIndex);
        if (slot == nullptr || !slot->inFlight || slot->gpuFence != nullptr || slot->quarantined)
            throw std::logic_error("Swapchain scene output cannot mutate a GPU-owned resource slot");
    }
    driver.m_impl->swapchainSceneOutput.Prepare(driver, driver.m_impl->GetFrameContext(*slotIndex), package,
        driver.m_impl->explicitSwapchain->GetDesc().backbufferFormat);
}

bool DriverRendererAccess::TryEnqueueRenderFrame(
    Driver& driver,
    const RenderFramePacket& snapshot,
    RenderSceneBuildCommand renderSceneBuilder,
    size_t* publishedSlotIndex,
    uint64_t* publishedFrameId,
    const bool backgroundPreview)
{
    if (!renderSceneBuilder)
    {
        if (publishedFrameId != nullptr)
            *publishedFrameId = 0u;
        return false;
    }
    if (driver.m_impl != nullptr)
    {
        auto pendingUiConsumer = snapshot.targetsSwapchain
            ? std::make_shared<PendingUiSnapshotConsumer>(*driver.m_impl) : nullptr;
        auto originalRenderSceneBuilder = std::move(renderSceneBuilder);
        renderSceneBuilder = [&driver, originalRenderSceneBuilder = std::move(originalRenderSceneBuilder),
                                 pendingUiConsumer = std::move(pendingUiConsumer)](
                                 const size_t lifecycleSlotIndex) mutable
        {
            auto package = originalRenderSceneBuilder(lifecycleSlotIndex);
            if (package.targetsSwapchain)
            {
                uint64_t consumedUiSnapshotGeneration = 0u;
                uint64_t observedPresentationInvalidationGeneration = 0u;
                auto uiSnapshot = DriverUIAccess::ConsumePendingUiDrawDataSnapshot(
                    driver,
                    &consumedUiSnapshotGeneration,
                    &observedPresentationInvalidationGeneration);
                if (uiSnapshot != nullptr && uiSnapshot->hasVisibleDraws)
                {
                    if (!AttachUiOverlaySnapshotToRenderScenePackage(package, uiSnapshot))
                    {
                        DriverUIAccess::RestoreConsumedUiDrawDataSnapshotIfUnchanged(
                            driver,
                            uiSnapshot,
                            consumedUiSnapshotGeneration);
                    }
                    else
                    {
                        package.uiReferencedTextureIdentities =
                            driver.m_impl->uiTextureRegistry.CollectReferencedTextureIdentities(
                                *uiSnapshot);
                        package.uiOverlayPresentationInvalidationGeneration =
                            observedPresentationInvalidationGeneration;
                    }
                }
            }
            pendingUiConsumer.reset();
            return package;
        };
    }

    return RenderThreadCoordinator::TryEnqueueRenderFrame(
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

bool DriverRendererAccess::TryDrainThreadedRendering(
    Driver& driver,
    const bool applyPendingSwapchainResize,
    const std::optional<std::chrono::milliseconds> timeout)
{
    if (driver.m_impl == nullptr)
        return true;

    const auto drainDeadline = std::chrono::steady_clock::now() +
        std::max(std::chrono::milliseconds::zero(), timeout.value_or(driver.m_impl->renderDrainTimeout));
    const bool drained = DrainRenderFrameCommands(
        *driver.m_impl,
        &driver,
        applyPendingSwapchainResize,
        drainDeadline);
    if (drained)
    {
        auto& impl = *driver.m_impl;
        if (impl.renderingThreads.IsInitialized() && !impl.renderingThreads.IsSuspended() &&
            !RenderingThreadSystem::IsInRenderingThread())
        {
            // Slot retirement can precede the producer's final handoff and resize.
            // Join that work on its owning render thread before returning to callers.
            const std::weak_ptr<DriverImpl::RenderCommandFlushLifetime> lifetime =
                impl.renderCommandFlushLifetime;
            const auto applied = std::make_shared<std::atomic_bool>(false);
            RenderCommandEvent completion;
            EnqueueRenderCommand<struct DriverDrainRenderingCommand>(
                [lifetime, applied, driverPtr = &driver, applyPendingSwapchainResize]
                {
                    if (lifetime.expired())
                        return;
                    RhiCommandListExecutor::DrainProducerOwnedThreadedSubmission(*driverPtr);
                    if (applyPendingSwapchainResize)
                        driverPtr->ApplyPendingSwapchainResize();
                    applied->store(true, std::memory_order_release);
                }, completion);
            NotifyThreadedWorkers(impl);
            const auto remaining = std::max(std::chrono::steady_clock::duration::zero(),
                drainDeadline - std::chrono::steady_clock::now());
            if (!completion.WaitFor(std::chrono::ceil<std::chrono::milliseconds>(remaining)))
            {
                NLS_LOG_ERROR("Driver: timed out joining render-thread submission finalization and resize");
                return false;
            }
            if (!completion.Succeeded())
            {
                NLS_LOG_ERROR("Driver: render-thread finalization failed: " + completion.GetDiagnostic());
                return false;
            }
            return applied->load(std::memory_order_acquire);
        }
        (void)RenderCommandDispatcher::Dispatch();
        RhiCommandListExecutor::DrainProducerOwnedThreadedSubmission(driver);
        if (applyPendingSwapchainResize)
            driver.ApplyPendingSwapchainResize();
    }
    return drained;
}

DriverRendererAccess::FrameLatencyDebugStats DriverRendererAccess::GetFrameLatencyDebugStats(
    const Driver& driver)
{
    FrameLatencyDebugStats stats;
    if (driver.m_impl == nullptr || driver.m_impl->frameLatency == nullptr)
        return stats;

    const auto& latency = *driver.m_impl->frameLatency;
    stats.openFrameCount = latency.GetInFlightFrameCount();
    stats.openedFrameCount = latency.GetOpenedFrameCount();
    stats.retiredFrameCount = latency.GetRetiredFrameCount();
    stats.stalledFrameCount = latency.GetStalledFrameCount();
    stats.stallTimeoutCount = latency.GetStallTimeoutCount();
    stats.totalStallTimeNanoseconds = latency.GetTotalStallTimeNanoseconds();
    return stats;
}

bool DriverRendererAccess::TryWaitForSubmittedGpuWork(Driver& driver)
{
    if (driver.m_impl == nullptr)
        return true;
    if (!TryDrainThreadedRendering(driver, false))
        return false;
    const bool drained = DrainFrameFencesForResize(*driver.m_impl);
    if (drained)
        (void)RhiCommandListExecutor::Get().ReleaseRetiredBatches();
    return drained;
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

void DriverRendererAccess::ResetThreadedReservedSlotWaitMeasurementWindowMaxNs(Driver& driver)
{
    if (driver.m_impl != nullptr)
        driver.m_impl->frameResourceReservationWaitMaxNs.store(0u, std::memory_order_release);
}

uint64_t DriverRendererAccess::GetThreadedReservedSlotWaitMeasurementWindowMaxNs(const Driver& driver)
{
    const uint64_t resourceWait = driver.m_impl != nullptr
        ? driver.m_impl->frameResourceReservationWaitMaxNs.load(std::memory_order_acquire) : 0u;
    return resourceWait;
}

std::vector<uint64_t> DriverRendererAccess::CollectStreamingDependencyPins(const Driver& driver)
{
    if (driver.m_impl == nullptr)
        return {};

    return Detail::CollectPendingStreamingDependencyPins(*driver.m_impl);
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

const ScopedFrameResourceContext*& ScopedFrameResourceContext::Current()
{
    static thread_local const ScopedFrameResourceContext* current = nullptr;
    return current;
}

ScopedFrameResourceContext::ScopedFrameResourceContext(const Driver& driver, const size_t slotIndex)
    : m_driver(driver.m_impl.get()), m_slotIndex(slotIndex), m_previous(Current())
{
    if (m_driver == nullptr || slotIndex >= m_driver->GetFrameContextCount())
        throw std::out_of_range("Frame resource context has no resource slot");
    Current() = this;
}

ScopedFrameResourceContext::~ScopedFrameResourceContext()
{
    Current() = m_previous;
}

std::optional<size_t> ScopedFrameResourceContext::GetSlotIndex(const DriverImpl& driver)
{
    for (const auto* context = Current(); context != nullptr; context = context->m_previous)
        if (context->m_driver == &driver)
            return context->m_slotIndex;
    return std::nullopt;
}

std::optional<size_t> DriverRendererAccess::GetActiveFrameContextSlotIndex(const Driver& driver)
{
    if (driver.m_impl == nullptr || driver.m_impl->GetFrameContextCount() == 0u)
        return std::nullopt;

    const auto bound = ScopedFrameResourceContext::GetSlotIndex(*driver.m_impl);
    return bound.has_value() && *bound < driver.m_impl->GetFrameContextCount() ? bound : std::nullopt;
}

size_t DriverRendererAccess::GetFrameContextSlotCount(const Driver& driver)
{
    if (driver.m_impl == nullptr)
        return 0u;

    if (driver.m_impl->GetFrameContextCount() != 0u)
        return driver.m_impl->GetFrameContextCount();

    return 0u;
}

size_t DriverRendererAccess::GetFrameResourceSlotCount(const Driver& driver)
{
    if (driver.m_impl == nullptr)
        return 0u;

    if (driver.m_impl->frameResourceSlots != nullptr)
        return driver.m_impl->frameResourceSlots->GetSlotCount();

    return GetFrameContextSlotCount(driver);
}

namespace
{
    std::chrono::steady_clock::time_point ResolveFrameAdmissionDeadline(
        const DriverImpl& impl,
        const bool waitForRetirement)
    {
        if (waitForRetirement && impl.threadedWorkersRunning && !impl.renderingThreads.IsSuspended())
            return std::chrono::steady_clock::time_point::max();
        // A stopped consumer cannot free capacity. Its caller must drain explicitly.
        return std::chrono::steady_clock::now();
    }

    uint64_t ResolveRemainingWaitNanoseconds(
        const std::chrono::steady_clock::time_point retirementDeadline)
    {
        const auto remaining = retirementDeadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero())
            return 0u;
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count());
    }

    std::optional<size_t> ReserveReusableFrameContextSlotIndexForDriver(
        DriverImpl& impl,
        const bool waitForDeferredFrameFence,
        const bool waitForRetirement,
        const std::chrono::steady_clock::time_point retirementDeadline,
        const bool targetsSwapchain = false)
    {
        const auto admissionCancelled = [&impl]
        {
            if (impl.frameLatency != nullptr)
            {
                // CPU fence failures must be observed even while GPU slots remain busy.
                (void)impl.frameLatency->PollCompletedFrames();
                if (!impl.frameLatency->GetFailureDiagnostic().empty())
                    return true;
            }
            return impl.threadedStopRequested.load(std::memory_order_acquire) ||
                impl.deviceLostDetected.load(std::memory_order_acquire) ||
                impl.unsafeGpuWorkQuarantined.load(std::memory_order_acquire) ||
                HasRenderFatalError();
        };
        if (admissionCancelled() || impl.frameResourceSlots == nullptr)
            return std::nullopt;

        {
            std::lock_guard retirementLock(impl.frameResourceRetirementMutex);
            if (impl.reservedFrameResourceLease.has_value())
                return impl.reservedFrameResourceLease->slotIndex;
        }
        if (impl.explicitDevice != nullptr && impl.GetFrameContextCount() == 0u &&
            impl.explicitDevice->GetQueue(Render::RHI::QueueType::Graphics) != nullptr)
            RebuildExplicitFrameContextRing(impl,
                static_cast<uint32_t>(impl.frameResourceSlots->GetSlotCount()));

        struct ReservationWaitMeasurement final
        {
            DriverImpl& impl;
            std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            ~ReservationWaitMeasurement()
            {
                const auto duration = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count());
                auto previous = impl.frameResourceReservationWaitMaxNs.load(std::memory_order_relaxed);
                while (previous < duration && !impl.frameResourceReservationWaitMaxNs.compare_exchange_weak(
                    previous, duration, std::memory_order_release, std::memory_order_relaxed)) {}
            }
        } waitMeasurement { impl };

        struct FrameBudgetGuard final
        {
            FrameLatencyController* controller = nullptr;
            uint64_t frameId = 0u;
            bool committed = false;
            ~FrameBudgetGuard()
            {
                if (!committed && controller != nullptr)
                    (void)controller->CancelFrame(frameId);
            }
        };
        const bool reservePresentationSlot = impl.frameResourceSlots->GetSlotCount() > 1u &&
            impl.explicitSwapchain != nullptr && impl.explicitDevice != nullptr &&
            RHI::GetUIOverlayFrameGraphFeature(impl.explicitDevice.get()).supported;

        for (;;)
        {
            if (admissionCancelled())
                return std::nullopt;
            const auto wakeGeneration = impl.threadedWorkerWake.Generation();
            std::optional<size_t> candidate;
            std::shared_ptr<Render::RHI::RHIFence> retirementFence;
            {
                std::lock_guard retirementLock(impl.frameResourceRetirementMutex);
                const size_t presentationSlot = impl.frameResourceSlots->GetSlotCount() - 1u;
                // Scene producers must leave capacity for the UI frame that presents their output.
                candidate = targetsSwapchain && reservePresentationSlot &&
                        impl.frameResourceSlots->IsSlotAvailable(presentationSlot)
                    ? std::optional<size_t>(presentationSlot)
                    : impl.frameResourceSlots->FindAvailableSlotIndex(
                        reservePresentationSlot && !targetsSwapchain ? 1u : 0u);
                if (!candidate.has_value())
                    for (size_t index = 0u; index < impl.frameResourceSlots->GetSlotCount() -
                            (reservePresentationSlot && !targetsSwapchain ? 1u : 0u); ++index)
                    {
                        const auto* slot = impl.frameResourceSlots->PeekSlot(index);
                        if (slot->gpuFence != nullptr && !slot->quarantined)
                        {
                            retirementFence = slot->gpuFence;
                            break;
                        }
                    }
            }

            if (candidate.has_value())
            {
                // Do not hold a CPU frame budget while waiting for a physical scene slot.
                FrameBudgetGuard budget;
                if (impl.frameLatency != nullptr)
                {
                    const auto wait = !waitForRetirement ? std::chrono::nanoseconds::zero()
                        : retirementDeadline == std::chrono::steady_clock::time_point::max()
                            ? std::chrono::nanoseconds::max()
                            : std::chrono::nanoseconds(ResolveRemainingWaitNanoseconds(retirementDeadline));
                    const uint32_t presentationBudget = reservePresentationSlot && !targetsSwapchain &&
                            impl.frameLatency->GetMaxFramesInFlight() > 1u
                        ? 1u : 0u;
                    const auto frameId = impl.frameLatency->ReserveFrame(wait, presentationBudget);
                    if (!frameId.has_value())
                    {
                        NLS_PROFILE_NAMED_SCOPE("FrameAdmission::CpuBudgetUnavailable");
                        return std::nullopt;
                    }
                    budget.controller = impl.frameLatency.get();
                    budget.frameId = *frameId;
                }
                if (*candidate < impl.GetFrameContextCount())
                    (void)Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(
                        impl, impl.GetFrameContext(*candidate), *candidate);
                if (admissionCancelled())
                    return std::nullopt;
                std::lock_guard retirementLock(impl.frameResourceRetirementMutex);
                const auto* slot = impl.frameResourceSlots->AcquireSlot(*candidate);
                if (slot == nullptr)
                    continue;
                impl.reservedFrameResourceLease = DriverImpl::ReservedFrameResourceLease {
                    *candidate, slot->leaseGeneration, budget.frameId };
                budget.committed = true;
                return candidate;
            }

            if (!waitForRetirement || std::chrono::steady_clock::now() >= retirementDeadline)
            {
                NLS_PROFILE_NAMED_SCOPE("FrameAdmission::GpuSlotUnavailable");
                return std::nullopt;
            }
            (void)RenderingThreadSystem::PumpGameThreadTasks();
            // Pumping game-thread tasks can consume the whole remaining budget on a
            // loaded machine. Re-check it before waiting so the loop never issues a
            // zero-length wait: the head of the loop then reports the expired
            // deadline instead.
            const uint64_t remainingWaitNanoseconds =
                ResolveRemainingWaitNanoseconds(retirementDeadline);
            if (remainingWaitNanoseconds == 0u)
                continue;
            const auto wait = std::chrono::nanoseconds(
                (std::min)(uint64_t { 1'000'000u }, remainingWaitNanoseconds));
            if (waitForDeferredFrameFence && retirementFence != nullptr)
            {
                const auto sliceDeadline = std::chrono::steady_clock::now() + wait;
                (void)retirementFence->Wait(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(wait).count()));
                if (!retirementFence->IsSignaled())
                    (void)WaitForThreadedWorkerWake(impl, wakeGeneration,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            (std::max)(std::chrono::nanoseconds::zero(),
                                std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    sliceDeadline - std::chrono::steady_clock::now()))));
            }
            else if (RenderingThreadSystem::HasActiveRenderThread())
                (void)WaitForThreadedWorkerWake(impl, wakeGeneration,
                    std::chrono::duration_cast<std::chrono::milliseconds>(wait));
            else
                std::this_thread::yield();
        }
    }
}

std::optional<size_t> DriverRendererAccess::ReserveReusableFrameContextSlotIndex(Driver& driver)
{
    if (driver.m_impl == nullptr)
        return std::nullopt;

    return ReserveReusableFrameContextSlotIndexForDriver(
        *driver.m_impl,
        false,
        true,
        ResolveFrameAdmissionDeadline(*driver.m_impl, true));
}

std::optional<size_t> DriverRendererAccess::ReserveReusableFrameContextSlotIndexForPreparedPublication(
    Driver& driver,
    const bool waitForRetirement,
    const bool targetsSwapchain)
{
    if (driver.m_impl == nullptr)
        return std::nullopt;

    return ReserveReusableFrameContextSlotIndexForPreparedPublication(
        driver,
        ResolveFrameAdmissionDeadline(*driver.m_impl, waitForRetirement),
        waitForRetirement,
        targetsSwapchain);
}

std::optional<size_t> DriverRendererAccess::ReserveReusableFrameContextSlotIndexForPreparedPublication(
    Driver& driver,
    const std::chrono::steady_clock::time_point retirementDeadline,
    const bool waitForRetirement,
    const bool targetsSwapchain)
{
    if (driver.m_impl == nullptr)
        return std::nullopt;

    return ReserveReusableFrameContextSlotIndexForDriver(
        *driver.m_impl,
        waitForRetirement,
        waitForRetirement,
        retirementDeadline,
        targetsSwapchain);
}

bool DriverRendererAccess::ReleaseReservedFrameContextSlotIndex(Driver& driver, const size_t slotIndex)
{
    if (driver.m_impl == nullptr)
        return false;
    uint64_t latencyFrameId = 0u;
    bool released = false;
    {
        std::lock_guard retirementLock(driver.m_impl->frameResourceRetirementMutex);
        const auto& lease = driver.m_impl->reservedFrameResourceLease;
        if (lease.has_value() && lease->slotIndex == slotIndex)
        {
            latencyFrameId = lease->latencyFrameId;
            if (driver.m_impl->frameResourceSlots != nullptr)
            {
                const auto* slot = driver.m_impl->frameResourceSlots->PeekSlot(slotIndex);
                if (slot != nullptr && slot->inFlight && !slot->quarantined &&
                    slot->gpuFence == nullptr && slot->leaseGeneration == lease->generation)
                    driver.m_impl->frameResourceSlots->RetireSlot(slotIndex, nullptr);
            }
            driver.m_impl->reservedFrameResourceLease.reset();
            released = true;
        }
    }
    if (released && latencyFrameId != 0u && driver.m_impl->frameLatency != nullptr)
        (void)driver.m_impl->frameLatency->CancelFrame(latencyFrameId);
    return released;
}

std::optional<size_t> DriverRendererAccess::GetReservedFrameContextSlotIndex(const Driver& driver)
{
    if (driver.m_impl == nullptr)
        return std::nullopt;

    std::lock_guard retirementLock(driver.m_impl->frameResourceRetirementMutex);
    const auto& lease = driver.m_impl->reservedFrameResourceLease;
    return lease.has_value() ? std::optional<size_t>(lease->slotIndex) : std::nullopt;
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

bool DriverRendererAccess::SetActiveReadbackTexture(
    const Driver& driver,
    const std::shared_ptr<Render::RHI::RHITexture>& texture,
    const uint64_t generation)
{
    if (driver.m_impl == nullptr || texture == nullptr)
        return false;

    auto* frameContext = Detail::GetActiveFrameContext(*driver.m_impl);
    if (frameContext == nullptr)
        return false;

    frameContext->explicitReadbackTexture = texture;
    frameContext->explicitReadbackTextureGeneration = generation;
    return true;
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
    auto slotIndex = GetActiveFrameContextSlotIndex(driver);
    if (!slotIndex.has_value() && RenderingThreadSystem::IsInGameThread())
        slotIndex = GetReservedFrameContextSlotIndex(driver);
    if (!slotIndex.has_value() || *slotIndex >= driver.m_impl->GetFrameContextCount())
        return nullptr;
    return driver.m_impl->GetFrameContext(*slotIndex).descriptorAllocator;
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
        activeSlotIndex.has_value() && activeSlotIndex.value() < driver.m_impl->GetFrameContextCount())
    {
        frameIndex = driver.m_impl->GetFrameContext(activeSlotIndex.value()).frameIndex;
    }
    else if (const auto reservedSlotIndex = RenderingThreadSystem::IsInGameThread()
        ? GetReservedFrameContextSlotIndex(driver) : std::nullopt;
        reservedSlotIndex.has_value() && reservedSlotIndex.value() < driver.m_impl->GetFrameContextCount())
    {
        frameIndex = driver.m_impl->GetFrameContext(reservedSlotIndex.value()).frameIndex;
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
    return RhiCommandListExecutor::ReadPixelsChecked(
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
    return RhiCommandListExecutor::ReadPixelsChecked(
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
    return RhiCommandListExecutor::BeginReadPixels(
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
    return RhiCommandListExecutor::BeginReadPixels(
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
    return RhiCommandListExecutor::PollReadbackCompletion(driver, readback);
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
    if (driver.m_impl->frameLatency != nullptr)
        driver.m_impl->frameLatency->Stop();
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
    if (driver.m_impl->frameLatency != nullptr)
        driver.m_impl->frameLatency->Stop();
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
	RhiCommandListExecutor::PresentSwapchain(driver);
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
    const uint64_t contentSignature = snapshot != nullptr
        ? UI::ResolveUiDrawDataContentSignature(*snapshot)
        : 0u;
    bool presentationChanged = false;
    {
        std::lock_guard lock(driver.m_impl->pendingUiOverlaySnapshotMutex);
        presentationChanged =
            driver.m_impl->latestUiOverlayContentSignature != contentSignature;
        driver.m_impl->pendingUiOverlaySnapshot = std::move(snapshot);
        driver.m_impl->pendingUiOverlaySnapshotFrameId = frameId;
        if (frameId != 0u)
            driver.m_impl->latestUiOverlaySnapshotFrameId = frameId;
        driver.m_impl->latestUiOverlayContentSignature = contentSignature;
        ++driver.m_impl->pendingUiOverlaySnapshotGeneration;
    }

    if (presentationChanged)
        Detail::NotifyThreadedWorkers(*driver.m_impl);
}

bool DriverUIAccess::PublishUiOnlyFrame(
    Driver& driver,
    const uint32_t renderWidth,
    const uint32_t renderHeight,
    size_t* publishedSlotIndex,
    uint64_t* publishedFrameId,
    const bool waitForRetirement)
{
    if (publishedFrameId != nullptr)
        *publishedFrameId = 0u;
    if (publishedSlotIndex != nullptr)
        *publishedSlotIndex = 0u;

    if (driver.m_impl == nullptr ||
        driver.m_impl->explicitDevice == nullptr ||
        driver.m_impl->explicitSwapchain == nullptr ||
        driver.m_impl->frameResourceSlots == nullptr ||
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
    uint64_t observedPresentationInvalidationGeneration = 0u;
    uint64_t contentSignature = 0u;
    {
        std::lock_guard lock(driver.m_impl->pendingUiOverlaySnapshotMutex);
        snapshot = driver.m_impl->pendingUiOverlaySnapshot;
        observedPresentationInvalidationGeneration =
            driver.m_impl->uiOverlayPresentationInvalidationGeneration;
        if (snapshot != nullptr && snapshot->hasVisibleDraws)
        {
            contentSignature = UI::ResolveUiDrawDataContentSignature(*snapshot);
            const bool alreadyPresented = contentSignature ==
                    driver.m_impl->lastPublishedSwapchainUiContentSignature &&
                observedPresentationInvalidationGeneration ==
                    driver.m_impl->lastPublishedSwapchainUiInvalidationGeneration;
            const bool hasQueuedUi = Detail::HasPendingSwapchainFrame(*driver.m_impl) &&
                driver.m_impl->lastQueuedSwapchainUiInvalidationGeneration != 0u;
            const bool alreadyQueued = contentSignature ==
                    driver.m_impl->lastQueuedSwapchainUiContentSignature &&
                observedPresentationInvalidationGeneration ==
                    driver.m_impl->lastQueuedSwapchainUiInvalidationGeneration;
            if (hasQueuedUi ? alreadyQueued : alreadyPresented)
            {
                NLS_PROFILE_NAMED_SCOPE("UIOnlyFrame::Unchanged");
                ++driver.m_impl->skippedUnchangedUiOnlyFrameCount;
                return false;
            }
        }
    }
    if (snapshot == nullptr || !snapshot->hasVisibleDraws || CountVisibleUiDrawCommands(*snapshot) == 0u)
        return false;

    const auto referencedTextureIdentities =
        driver.m_impl->uiTextureRegistry.CollectReferencedTextureIdentities(*snapshot);

    RenderFramePacket frameSnapshot;
    frameSnapshot.frameId = snapshot->frameId != 0u
        ? snapshot->frameId
        : driver.m_impl->nextThreadedFrameId++;
    frameSnapshot.targetsSwapchain = true;
    frameSnapshot.renderWidth = renderWidth;
    frameSnapshot.renderHeight = renderHeight;
    frameSnapshot.clearColorBuffer = false;
    frameSnapshot.clearDepthBuffer = false;
    frameSnapshot.clearStencilBuffer = false;

    const bool published = RenderThreadCoordinator::TryEnqueueRenderFrame(
        driver,
        frameSnapshot,
        [frameSnapshot, snapshot, referencedTextureIdentities, observedPresentationInvalidationGeneration](const size_t)
        {
            auto package = BuildRenderFramePackage(frameSnapshot);
            if (!AttachUiOverlaySnapshotToRenderScenePackage(package, snapshot))
                throw std::runtime_error("UI-only Render Command could not attach its captured overlay");
            package.frameDataReady = true;
            package.objectDataReady = true;
            package.lightingDataReady = true;
            package.uiReferencedTextureIdentities = referencedTextureIdentities;
            package.uiOverlayPresentationInvalidationGeneration = observedPresentationInvalidationGeneration;
            return package;
        },
        false,
        publishedSlotIndex,
        publishedFrameId,
        false,
        waitForRetirement);
    if (!published)
    {
        NLS_PROFILE_NAMED_SCOPE("UIOnlyFrame::AdmissionUnavailable");
        return false;
    }

    {
        std::lock_guard lock(driver.m_impl->pendingUiOverlaySnapshotMutex);
        driver.m_impl->lastQueuedSwapchainUiContentSignature = contentSignature;
        driver.m_impl->lastQueuedSwapchainUiInvalidationGeneration = observedPresentationInvalidationGeneration;
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
    uint64_t* consumedGeneration,
    uint64_t* observedPresentationInvalidationGeneration)
{
    if (consumedGeneration != nullptr)
        *consumedGeneration = 0u;
    if (observedPresentationInvalidationGeneration != nullptr)
        *observedPresentationInvalidationGeneration = 0u;

    if (driver.m_impl == nullptr)
        return nullptr;

    std::lock_guard lock(driver.m_impl->pendingUiOverlaySnapshotMutex);
    auto snapshot = std::move(driver.m_impl->pendingUiOverlaySnapshot);
    driver.m_impl->pendingUiOverlaySnapshotFrameId = 0u;
    if (consumedGeneration != nullptr)
        *consumedGeneration = driver.m_impl->pendingUiOverlaySnapshotGeneration;
    if (observedPresentationInvalidationGeneration != nullptr)
    {
        *observedPresentationInvalidationGeneration =
            driver.m_impl->uiOverlayPresentationInvalidationGeneration;
    }
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

    uint64_t requestId = 0u;
    {
        std::lock_guard lock(driver.m_impl->pendingUiRgba8TextureUploadMutex);
        requestId = driver.m_impl->nextUiRgba8TextureUploadRequestId++;
        if (driver.m_impl->nextUiRgba8TextureUploadRequestId == 0u)
            driver.m_impl->nextUiRgba8TextureUploadRequestId = 1u;

        driver.m_impl->pendingUiRgba8TextureUploads.push_back({
            requestId,
            request.width,
            request.height,
            std::move(request.rgbaPixels),
            std::move(request.debugName),
            request.colorSpace,
            request.format,
            request.rowPitch,
            request.slicePitch,
            std::move(request.atlasPageKey),
            request.atlasPageSize,
            request.atlasX,
            request.atlasY
        });
    }
    {
        std::lock_guard lock(driver.m_impl->pendingUiOverlaySnapshotMutex);
        AdvanceUiOverlayPresentationInvalidationLocked(*driver.m_impl);
    }
    Detail::NotifyThreadedWorkers(*driver.m_impl);
    return requestId;
}

bool DriverUIAccess::SupportsUiRgba8TextureAtlasRegionUploads(const Driver& driver)
{
    if (driver.m_impl == nullptr || driver.m_impl->explicitDevice == nullptr)
        return false;
    return driver.m_impl->explicitDevice->GetCapabilities()
        .GetFeature(Render::RHI::RHIDeviceFeature::UITextureAtlasRegionUpload)
        .supported;
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
    driver.m_impl->meshRuntimeUploadLastRequestedId = requestId;
    driver.m_impl->meshRuntimeUploadRequestedCount.fetch_add(1u, std::memory_order_relaxed);
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
    result.uploadRequest = std::move(found->second.uploadRequest);
    result.diagnostic = std::move(found->second.diagnostic);
    driver.m_impl->meshRuntimeUploadLastConsumedId = requestId;
    driver.m_impl->meshRuntimeUploadConsumedCount.fetch_add(1u, std::memory_order_relaxed);
    if (!result.success)
        driver.m_impl->meshRuntimeUploadFailedCount.fetch_add(1u, std::memory_order_relaxed);
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
    driver.m_impl->meshRuntimeUploadLastCanceledId = requestId;
    if (removedPending || removedCompleted)
        driver.m_impl->meshRuntimeUploadCanceledCount.fetch_add(1u, std::memory_order_relaxed);
}

DriverResourceAccess::MeshRuntimeUploadDiagnostics
DriverResourceAccess::GetMeshRuntimeUploadDiagnostics(const Driver& driver)
{
    MeshRuntimeUploadDiagnostics diagnostics;
    if (driver.m_impl == nullptr)
        return diagnostics;

    {
        std::lock_guard lock(driver.m_impl->pendingMeshRuntimeUploadMutex);
        diagnostics.pendingRequestCount = driver.m_impl->pendingMeshRuntimeUploads.size();
        diagnostics.completedResultCount = driver.m_impl->completedMeshRuntimeUploads.size();
        diagnostics.driverInstanceIdentity = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(driver.m_impl.get()));
        diagnostics.lastRequestedRequestId = driver.m_impl->meshRuntimeUploadLastRequestedId;
        diagnostics.lastSwappedBatchCount = driver.m_impl->meshRuntimeUploadLastSwappedBatchCount;
        diagnostics.emptyRecordTickCount = driver.m_impl->meshRuntimeUploadEmptyRecordTickCount;
        diagnostics.lastRecordedRequestId = driver.m_impl->meshRuntimeUploadLastRecordedId;
        diagnostics.lastConsumedRequestId = driver.m_impl->meshRuntimeUploadLastConsumedId;
        diagnostics.lastCanceledRequestId = driver.m_impl->meshRuntimeUploadLastCanceledId;
    }
    diagnostics.requestedCount = driver.m_impl->meshRuntimeUploadRequestedCount.load(std::memory_order_relaxed);
    diagnostics.recordTickCount = driver.m_impl->meshRuntimeUploadRecordTickCount.load(std::memory_order_relaxed);
    diagnostics.rhiIdleTickCount = driver.m_impl->meshRuntimeUploadRhiIdleTickCount.load(std::memory_order_relaxed);
    diagnostics.recordedCount = driver.m_impl->meshRuntimeUploadRecordedCount.load(std::memory_order_relaxed);
    diagnostics.consumedCount = driver.m_impl->meshRuntimeUploadConsumedCount.load(std::memory_order_relaxed);
    diagnostics.failedCount = driver.m_impl->meshRuntimeUploadFailedCount.load(std::memory_order_relaxed);
    diagnostics.canceledCount = driver.m_impl->meshRuntimeUploadCanceledCount.load(std::memory_order_relaxed);
    return diagnostics;
}

uint64_t DriverResourceAccess::RequestTextureRuntimeUpload(
    Driver& driver,
    TextureRuntimeUploadRequest request)
{
    if (driver.m_impl == nullptr ||
        request.desc.extent.width == 0u ||
        request.desc.extent.height == 0u ||
        request.mips.empty())
    {
        return 0u;
    }

    std::lock_guard lock(driver.m_impl->pendingTextureRuntimeUploadMutex);
    const uint64_t requestId = driver.m_impl->nextTextureRuntimeUploadRequestId++;
    if (driver.m_impl->nextTextureRuntimeUploadRequestId == 0u)
        driver.m_impl->nextTextureRuntimeUploadRequestId = 1u;
    driver.m_impl->pendingTextureRuntimeUploads.push_back({ requestId, std::move(request) });
    driver.m_impl->textureRuntimeUploadRequestedCount.fetch_add(1u, std::memory_order_relaxed);
    Detail::NotifyThreadedWorkers(*driver.m_impl);
    return requestId;
}

TextureRuntimeUploadResult DriverResourceAccess::ConsumeTextureRuntimeUploadResult(
    Driver& driver,
    const uint64_t requestId)
{
    TextureRuntimeUploadResult result;
    if (driver.m_impl == nullptr || requestId == 0u)
        return result;

    std::lock_guard lock(driver.m_impl->pendingTextureRuntimeUploadMutex);
    const auto found = driver.m_impl->completedTextureRuntimeUploads.find(requestId);
    if (found == driver.m_impl->completedTextureRuntimeUploads.end())
        return result;

    result.ready = true;
    result.success = found->second.success;
    result.texture = std::move(found->second.texture);
    result.diagnostic = std::move(found->second.diagnostic);
    driver.m_impl->completedTextureRuntimeUploads.erase(found);
    driver.m_impl->textureRuntimeUploadConsumedCount.fetch_add(1u, std::memory_order_relaxed);
    if (!result.success)
        driver.m_impl->textureRuntimeUploadFailedCount.fetch_add(1u, std::memory_order_relaxed);
    return result;
}

void DriverResourceAccess::CancelTextureRuntimeUpload(
    Driver& driver,
    const uint64_t requestId)
{
    if (driver.m_impl == nullptr || requestId == 0u)
        return;

    std::lock_guard lock(driver.m_impl->pendingTextureRuntimeUploadMutex);
    const auto pendingCountBefore = driver.m_impl->pendingTextureRuntimeUploads.size();
    driver.m_impl->pendingTextureRuntimeUploads.erase(
        std::remove_if(
            driver.m_impl->pendingTextureRuntimeUploads.begin(),
            driver.m_impl->pendingTextureRuntimeUploads.end(),
            [requestId](const DriverImpl::PendingTextureRuntimeUpload& upload)
            {
                return upload.requestId == requestId;
            }),
        driver.m_impl->pendingTextureRuntimeUploads.end());
    const bool removedPending =
        driver.m_impl->pendingTextureRuntimeUploads.size() != pendingCountBefore;
    const bool removedCompleted =
        driver.m_impl->completedTextureRuntimeUploads.erase(requestId) != 0u;
    if (!removedPending && !removedCompleted)
        driver.m_impl->canceledTextureRuntimeUploadRequestIds.insert(requestId);
    if (removedPending || removedCompleted)
        driver.m_impl->textureRuntimeUploadCanceledCount.fetch_add(1u, std::memory_order_relaxed);
}

DriverResourceAccess::TextureRuntimeUploadDiagnostics
DriverResourceAccess::GetTextureRuntimeUploadDiagnostics(const Driver& driver)
{
    TextureRuntimeUploadDiagnostics diagnostics;
    if (driver.m_impl == nullptr)
        return diagnostics;

    std::lock_guard lock(driver.m_impl->pendingTextureRuntimeUploadMutex);
    diagnostics.pendingRequestCount = driver.m_impl->pendingTextureRuntimeUploads.size();
    diagnostics.completedResultCount = driver.m_impl->completedTextureRuntimeUploads.size();
    for (const auto& upload : driver.m_impl->pendingTextureRuntimeUploads)
        diagnostics.pendingStagingBytes += upload.request.ByteSize();
    for (const auto& upload : driver.m_impl->recordedTextureRuntimeUploads)
        diagnostics.pendingStagingBytes += upload.byteCount;
    diagnostics.requestedCount = driver.m_impl->textureRuntimeUploadRequestedCount.load(std::memory_order_relaxed);
    diagnostics.recordedCount = driver.m_impl->textureRuntimeUploadRecordedCount.load(std::memory_order_relaxed);
    diagnostics.consumedCount = driver.m_impl->textureRuntimeUploadConsumedCount.load(std::memory_order_relaxed);
    diagnostics.failedCount = driver.m_impl->textureRuntimeUploadFailedCount.load(std::memory_order_relaxed);
    diagnostics.canceledCount = driver.m_impl->textureRuntimeUploadCanceledCount.load(std::memory_order_relaxed);
    return diagnostics;
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

void DriverUIAccess::NotifyUiTextureContentChanged(
    Driver& driver,
    const std::shared_ptr<RHI::RHITextureView>& textureView)
{
    if (driver.m_impl == nullptr ||
        textureView == nullptr ||
        textureView->GetTexture() == nullptr)
    {
        return;
    }

    const uint64_t textureIdentity = static_cast<uint64_t>(
        reinterpret_cast<std::uintptr_t>(textureView->GetTexture().get()));
    NotifyUiTextureContentChanged(driver, std::vector<uint64_t> { textureIdentity });
}

void DriverUIAccess::NotifyUiTextureContentChanged(
    Driver& driver,
    const std::vector<uint64_t>& textureIdentities)
{
    if (driver.m_impl == nullptr || textureIdentities.empty())
        return;

    uint64_t sampledFrameId = 0u;
    {
        std::lock_guard lock(driver.m_impl->pendingUiOverlaySnapshotMutex);
        sampledFrameId = driver.m_impl->pendingUiOverlaySnapshotFrameId != 0u
            ? driver.m_impl->pendingUiOverlaySnapshotFrameId
            : driver.m_impl->latestUiOverlaySnapshotFrameId;
    }

    const bool sampledByRegisteredUiTexture = std::any_of(
        textureIdentities.begin(),
        textureIdentities.end(),
        [&driver, sampledFrameId](const uint64_t textureIdentity)
        {
            return driver.m_impl->uiTextureRegistry.ContainsSampledTextureIdentity(
                textureIdentity,
                sampledFrameId);
        });
    if (!sampledByRegisteredUiTexture)
        return;

    {
        std::lock_guard lock(driver.m_impl->pendingUiOverlaySnapshotMutex);
        AdvanceUiOverlayPresentationInvalidationLocked(*driver.m_impl);
    }
    Detail::NotifyThreadedWorkers(*driver.m_impl);
}

void DriverUIAccess::RecordUiOverlayPresentationSucceeded(
    Driver& driver,
    const std::shared_ptr<const UI::UiDrawDataSnapshot>& snapshot,
    const uint64_t observedPresentationInvalidationGeneration)
{
    if (driver.m_impl == nullptr ||
        snapshot == nullptr ||
        !snapshot->hasVisibleDraws ||
        observedPresentationInvalidationGeneration == 0u)
    {
        return;
    }

    std::lock_guard lock(driver.m_impl->pendingUiOverlaySnapshotMutex);
    MarkSwapchainUiContentPublishedLocked(
        *driver.m_impl,
        *snapshot,
        observedPresentationInvalidationGeneration);
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
        AdvanceUiOverlayPresentationInvalidationLocked(*driver.m_impl);
    }

    driver.m_impl->uiOverlayRenderer.InvalidateFontAtlas(retireFrameId);
    Detail::NotifyThreadedWorkers(*driver.m_impl);
}

void DriverUIAccess::NotifyUiOverlaySwapchainWillResize(Driver& driver)
{
    if (driver.m_impl == nullptr)
        return;

    {
        std::lock_guard lock(driver.m_impl->pendingUiOverlaySnapshotMutex);
        AdvanceUiOverlayPresentationInvalidationLocked(*driver.m_impl);
    }
    Detail::NotifyThreadedWorkers(*driver.m_impl);
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
	std::lock_guard lock(driver.m_impl->uiCompositionSignalMutex);
	boundary.uiToPresentSignalSemaphore = driver.m_impl->uiRenderFinishedSemaphore;
	boundary.uiToPresentSignalValue = driver.m_impl->uiRenderFinishedValue;
	return boundary;
}

void DriverUIAccess::SetUICompositionSignal(
	Driver& driver,
	Render::RHI::NativeHandle semaphore,
	const uint64_t value)
{
	std::lock_guard lock(driver.m_impl->uiCompositionSignalMutex);
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

#if defined(NLS_ENABLE_TEST_HOOKS)
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

    if ((replacingDevice || clearingDevice) && driver.m_impl->explicitDevice != nullptr)
        Resources::Material::ReleaseDefaultTexturesForDevice(driver.m_impl->explicitDevice->GetCacheIdentity());

	driver.m_impl->explicitDevice = std::move(explicitDevice);
    if (!driver.m_impl->threadedWorkersRunning || driver.m_impl->renderingThreads.IsSuspended())
    {
        // Synchronous device fixtures still use the executor's ordered FIFO;
        // there is no dedicated lane to service them while the harness is paused.
        RhiCommandListExecutor::Get().Initialize(RhiThreadMode::None,
            RhiCommandListExecutor::CreateDeviceTargets(driver.m_impl->explicitDevice), driver.m_impl->taskGraph, &driver);
    }
    else if (RhiCommandListExecutor::Get().GetOwner() == &driver)
        RhiCommandListExecutor::Get().SetTargets(RhiCommandListExecutor::CreateDeviceTargets(driver.m_impl->explicitDevice));
    if (driver.m_impl->explicitDevice != nullptr)
    {
        if (driver.m_impl->pipelineCache == nullptr || replacingDevice)
            driver.m_impl->pipelineCache = Render::RHI::CreateDefaultPipelineCache();
        const auto threadedFrameContextCount = driver.m_impl->frameResourceSlots != nullptr
            ? static_cast<uint32_t>(driver.m_impl->frameResourceSlots->GetSlotCount())
            : 0u;
        // Partial fixture devices can implement only the resource creation surface
        // they test. Without a Graphics queue they cannot own a frame context ring,
        // so do not force one and assert on a command pool they never intended to
        // provide.
        const bool supportsFrameContexts =
            driver.m_impl->explicitDevice->GetQueue(Render::RHI::QueueType::Graphics) != nullptr;
        if (!replacingDevice &&
            driver.m_impl->GetFrameContextCount() == 0u &&
            threadedFrameContextCount > 0u &&
            supportsFrameContexts)
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
    std::lock_guard lock(driver.m_impl->frameResourceRetirementMutex);
    driver.m_impl->EnsureFrameContexts(index + 1u);
	return driver.m_impl->GetFrameContext(index);
}

void DriverTestAccess::SetFrameContextDescriptorAllocator(
    Driver& driver,
    const size_t index,
    std::shared_ptr<Render::RHI::DescriptorAllocator> descriptorAllocator)
{
    if (descriptorAllocator != nullptr)
    {
        descriptorAllocator->SetSharedAllocationFailureCounter(
            driver.m_impl->descriptorAllocationFailureCount);
    }
    EnsureFrameContext(driver, index).descriptorAllocator = std::move(descriptorAllocator);
}

const Render::RHI::RHIFrameContext* DriverTestAccess::PeekFrameContext(const Driver& driver, const size_t index)
{
	return index < driver.m_impl->GetFrameContextCount() ? &driver.m_impl->GetFrameContext(index) : nullptr;
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

    std::lock_guard lock(driver.m_impl->frameResourceRetirementMutex);
    return driver.m_impl->frameResourceSlots != nullptr
        ? driver.m_impl->frameResourceSlots->GetOwnedResourceCount() : 0u;
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

void DriverTestAccess::AgePendingSwapchainResize(
    Driver& driver,
    const std::chrono::steady_clock::duration age)
{
    ExecuteDriverRenderCommand(*driver.m_impl, [&driver, age]
    {
        driver.m_impl->lastSwapchainResizeRequestTime = std::chrono::steady_clock::now() - age;
    });
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

bool DriverTestAccess::TryLockFrameTelemetry(Driver& driver)
{
    return driver.m_impl != nullptr && driver.m_impl->frameTelemetryMutex.try_lock();
}

void DriverTestAccess::UnlockFrameTelemetry(Driver& driver)
{
    if (driver.m_impl != nullptr)
        driver.m_impl->frameTelemetryMutex.unlock();
}

void DriverTestAccess::SetRenderSceneObserver(
    Driver& driver,
    std::function<void(const RenderFramePacket&, const RenderScenePackage&)> observer)
{
    ExecuteDriverRenderCommand(*driver.m_impl,
        [impl = driver.m_impl.get(), observer = std::move(observer)]() mutable
        {
            impl->renderSceneObserver = std::move(observer);
        });
}

void DriverTestAccess::SetRhiFrameObserver(
    Driver& driver,
    std::function<void(const RhiSubmissionFrame&)> observer)
{
    ExecuteDriverRenderCommand(*driver.m_impl,
        [impl = driver.m_impl.get(), observer = std::move(observer)]() mutable
        {
            impl->rhiFrameObserver = std::move(observer);
        });
}



void DriverTestAccess::PauseThreadedRenderingWorkers(Driver& driver)
{
    driver.m_impl->threadedStopRequested.store(true);
    NotifyThreadedWorkers(*driver.m_impl);
    // Suspends the threads owned by the rendering thread system while keeping its
    // configuration, so a later ResumeRenderingThreads recreates them unchanged.
    driver.m_impl->renderingThreads.SuspendRenderingThreads();
    driver.m_impl->threadedStopRequested.store(false);
    RhiCommandListExecutor::Get().Initialize(RhiThreadMode::None,
        RhiCommandListExecutor::CreateDeviceTargets(driver.m_impl->explicitDevice), driver.m_impl->taskGraph, &driver);
}

bool DriverTestAccess::TryDrainThreadedRendering(Driver& driver, const bool applyPendingSwapchainResize)
{
    std::unique_ptr<ScopedRenderThreadForTesting> joinedLane;
    if (!RenderingThreadSystem::HasActiveRenderThread() && !RenderingThreadSystem::IsInRenderingThread())
        joinedLane = std::make_unique<ScopedRenderThreadForTesting>();
    return DriverRendererAccess::TryDrainThreadedRendering(driver, applyPendingSwapchainResize);
}

void DriverTestAccess::DrainThreadedRendering(Driver& driver)
{
    (void)TryDrainThreadedRendering(driver);
}



#endif

Driver::Driver(const Render::Settings::DriverSettings& p_driverSettings)
    : m_impl(std::make_unique<DriverImpl>())
{
    m_impl->requestedGraphicsBackend = p_driverSettings.graphicsBackend;
    m_impl->cpuFrameLatencyLimit = p_driverSettings.cpuFrameLatencyLimit;
    m_impl->lightGridEnabled = p_driverSettings.enableLightGrid;
    m_impl->requestedRhiThreadMode = p_driverSettings.rhiThreadMode;
    if (m_impl->requestedGraphicsBackend != Render::Settings::EGraphicsBackend::NONE &&
        !Render::Settings::IsBackendSelectableForPhase1(m_impl->requestedGraphicsBackend))
    {
        const auto phaseGateReport = Render::Settings::EvaluateBackendPhaseGate(
            m_impl->requestedGraphicsBackend,
            Render::Settings::RuntimeConsumer::Editor,
            Render::RHI::RHIDeviceCapabilities{});
        NLS_LOG_WARNING(
            "Driver: rejecting runtime backend outside the platform phase-1 selection: " +
            std::string(Render::Settings::ToString(m_impl->requestedGraphicsBackend)) +
            " phaseGate=" + phaseGateReport.summary);
    }
	m_impl->renderDocCaptureController = std::make_unique<Render::Tooling::RenderDocCaptureController>(p_driverSettings.renderDoc);

    // Device initialization uploads precede the worker lanes but still use the
    // executor FIFO. Thread startup later binds this same executor to its mode.
    if (m_impl->requestedGraphicsBackend != Render::Settings::EGraphicsBackend::NONE &&
        RhiCommandListExecutor::Get().GetOwner() == nullptr)
    {
        RhiCommandListExecutor::Get().Initialize(RhiThreadMode::None, {}, nullptr, this);
    }

	// All backends use CreateRhiDevice for direct Tier A device creation
	// This creates the Formal RHI device without any IRenderDevice dependency
	m_impl->explicitDevice = Render::Backend::CreateRhiDevice(p_driverSettings);
        // GPU validation patches shaders at first use and can spend minutes on
        // cold PSOs. Capture its bounded drain budget before starting the lanes.
        if (m_impl->explicitDevice != nullptr &&
            p_driverSettings.graphicsBackend == Render::Settings::EGraphicsBackend::DX12 &&
            Render::Settings::IsEnvironmentFlagEnabled("NLS_DX12_GPU_VALIDATION"))
        {
            m_impl->renderDrainTimeout = std::chrono::minutes(5);
        }
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

    const auto slotCount = ResolveExplicitFrameContextCount(p_driverSettings);
    if (m_impl->frameResourceSlots == nullptr)
        m_impl->frameResourceSlots = std::make_unique<FrameResourceRing>(slotCount);
    if (m_impl->frameLatency == nullptr)
        m_impl->frameLatency = std::make_unique<FrameLatencyController>(
            m_impl->cpuFrameLatencyLimit == 0u ? slotCount : (std::min)(slotCount, m_impl->cpuFrameLatencyLimit));
    StartThreadedRenderingWorkers();
}

Driver::~Driver()
{
    if (m_impl != nullptr)
        SetSwapchainWillResizeCallback(nullptr);
    ShutdownThreadedRendering();
    ShutdownRhiResources();
    if (TryGetLocatedDriver() == this)
        NLS::Core::ServiceLocator::Remove<Driver>();
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
        return;
    }

    SetSwapchainWillResizeCallback(nullptr);
    {
        std::lock_guard lock(m_impl->uiCompositionSignalMutex);
        m_impl->uiRenderFinishedSemaphore = {};
        m_impl->uiRenderFinishedValue = 0u;
    }
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
    m_impl->hasPendingSwapchainResize = false;


    const bool abandonFenceWait = m_impl->deviceLostDetected.load(std::memory_order_acquire);
    const bool drainedFrameFences =
        abandonFenceWait ||
        DrainFrameFencesForResize(*m_impl);
    if (drainedFrameFences && !abandonFenceWait)
        Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(*m_impl);
    bool releasedAllFrameContexts = drainedFrameFences;
    for (size_t frameContextIndex = 0u; frameContextIndex < m_impl->GetFrameContextCount(); ++frameContextIndex)
    {
        releasedAllFrameContexts =
            ReleaseFrameContextResources(*m_impl, frameContextIndex, abandonFenceWait) &&
            releasedAllFrameContexts;
    }
    if (!releasedAllFrameContexts)
    {
        NLS_LOG_ERROR("Driver::ShutdownRhiResources: timed out before releasing frame-context resources");
        return;
    }
    {
        std::lock_guard lock(m_impl->frameResourceRetirementMutex);
        m_impl->frameResourceSlots.reset();
        m_impl->reservedFrameResourceLease.reset();
        m_impl->unfencedReadbackResources.clear();
    }

    m_impl->pipelineCache.reset();
    m_impl->explicitSwapchain.reset();
    m_impl->renderDocCaptureController.reset();
    if (m_impl->explicitDevice != nullptr)
        Resources::Material::ReleaseDefaultTexturesForDevice(m_impl->explicitDevice->GetCacheIdentity());
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
    ExecuteDriverRenderCommand(*m_impl, [this, width, height]
    {
        ResizeSwapchain(width, height);
        if (!ScopedFrameResourceContext::GetSlotIndex(*m_impl).has_value())
            ApplyPendingSwapchainResize();
    });
}

void Driver::SetSwapchainWillResizeCallback(SwapchainWillResizeCallback callback)
{
	std::lock_guard lock(m_impl->swapchainResizeCallbackMutex);
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
	// An explicit resize request is authoritative. A rebuild-at-current-size request
	// published earlier by the submit lane (an out-of-date present) must not survive
	// it: the client supplied a new size, and honoring the older flag would rebuild
	// the swapchain at the size the user just moved away from.
	m_impl->pendingSwapchainResizeKeepsCurrentSize.store(false, std::memory_order_release);
	m_impl->hasPendingSwapchainResize = true;
	m_impl->lastSwapchainResizeRequestTime = std::chrono::steady_clock::now();
}

void Driver::PresentSwapchain()
{
	RhiCommandListExecutor::PresentSwapchain(*this);
}

void Driver::ApplyPendingSwapchainResize()
{
	if (!m_impl->hasPendingSwapchainResize)
		return;

    if (m_impl->deviceLostDetected.load(std::memory_order_acquire) ||
        m_impl->unsafeGpuWorkQuarantined.load(std::memory_order_acquire))
        return;

    if (RenderingThreadSystem::IsInRhiThread() ||
        (m_impl->renderingThreads.IsInitialized() && !m_impl->renderingThreads.IsSuspended() &&
         RenderingThreadSystem::HasActiveRenderThread() && !RenderingThreadSystem::IsInRenderingThread()))
    {
        ExecuteDriverRenderCommand(*m_impl, [this] { ApplyPendingSwapchainResize(); });
        return;
    }

	if (!ShouldApplyPendingSwapchainResize(
		std::chrono::steady_clock::now() - m_impl->lastSwapchainResizeRequestTime))
		return;

    if (Detail::GetPendingCpuFrameCount(*m_impl) > 0u)
        return;

    std::unique_lock<std::timed_mutex> threadedSubmissionLock;
    if (m_impl->frameResourceSlots != nullptr)
    {
        threadedSubmissionLock = std::unique_lock<std::timed_mutex>(
            m_impl->threadedRhiSubmissionMutex,
            std::try_to_lock);
        if (!threadedSubmissionLock.owns_lock())
            return;
    }

    if (Detail::GetPendingCpuFrameCount(*m_impl) > 0u)
        return;

    // A present that reported the surface as out of date recreates the swapchain at
    // the size it already has: there is no new client size to apply, only a stale
    // surface association. The flag is published by the submit lane, so read it here
    // on the render thread together with the size.
    const bool keepsCurrentSize =
        m_impl->pendingSwapchainResizeKeepsCurrentSize.load(std::memory_order_acquire);
	const uint32_t width = keepsCurrentSize && m_impl->explicitSwapchain != nullptr
        ? m_impl->explicitSwapchain->GetDesc().width
        : m_impl->pendingSwapchainWidth;
	const uint32_t height = keepsCurrentSize && m_impl->explicitSwapchain != nullptr
        ? m_impl->explicitSwapchain->GetDesc().height
        : m_impl->pendingSwapchainHeight;

    auto& executor = RhiCommandListExecutor::Get();
    if (executor.GetOwner() == this && executor.IsInitialized() &&
        !executor.Flush(RhiFlushFlags::Swapchain))
    {
        DriverUIAccess::MarkUnsafeGpuWorkQuarantined(*this, "Swapchain resize could not flush RHI Present: " +
            executor.GetFailureDiagnostic());
        throw std::runtime_error("Driver: swapchain resize could not flush RHI Present: " +
            executor.GetFailureDiagnostic());
    }

    if (!DrainFrameFencesForResize(*m_impl))
    {
        m_impl->lastSwapchainResizeRequestTime = std::chrono::steady_clock::now();
        return;
    }
    (void)RhiCommandListExecutor::Get().ReleaseRetiredBatches();
    Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(*m_impl);
    {
        std::lock_guard lock(m_impl->frameResourceRetirementMutex);
        if (m_impl->frameResourceSlots != nullptr && m_impl->frameResourceSlots->HasPendingRetirements())
            return;
    }

    {
        std::lock_guard lock(m_impl->swapchainResizeCallbackMutex);
        // The callback may clear itself; keep its callable alive until it returns.
        const auto callback = m_impl->swapchainWillResizeCallback;
        if (callback)
            callback();
    }

	for (size_t frameContextIndex = 0u; frameContextIndex < m_impl->GetFrameContextCount(); ++frameContextIndex)
	{
		auto& frameContext = m_impl->GetFrameContext(frameContextIndex);
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
	for (size_t frameContextIndex = 0u; frameContextIndex < m_impl->GetFrameContextCount(); ++frameContextIndex)
	{
		auto& frameContext = m_impl->GetFrameContext(frameContextIndex);
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
	m_impl->pendingSwapchainResizeKeepsCurrentSize.store(false, std::memory_order_release);
}

void Driver::SetPolygonMode(Settings::ERasterizationMode mode)
{
	m_impl->defaultPipelineState.rasterizationMode = mode;
}

    /**
     * Render/RHI thread bodies.
     *
     * These were previously inlined as raw std::thread lambdas inside
     * StartThreadedRenderingWorkers. They now implement IRenderingThreadTick so
     * RenderingThreadSystem owns the threads, their TaskGraph attachment and their
     * shutdown, while the driver keeps sole ownership of the device-facing work.
     */
    class DriverRenderingTick final : public IRenderingThreadTick
    {
    public:
        DriverRenderingTick(Driver& driver, DriverImpl& impl)
            : m_driver(driver)
            , m_impl(impl)
        {
        }

        bool TickRenderThread() override
        {
            SyncStopRequest();
            ApplyThreadDiagnostics(m_renderThreadId);
            NLS_PROFILE_NAMED_SCOPE("Render Thread Tick");
            const uint64_t observedWakeGeneration = m_impl.threadedWorkerWake.Generation();

            // Bound each FIFO slice so a busy producer cannot postpone retirement,
            // resource collection and uploads until the pipe becomes empty.
            constexpr size_t renderCommandsPerTick = 8u;
            const bool drainedRenderCommands = RenderCommandDispatcher::Dispatch(renderCommandsPerTick) > 0u;

            // Render owns ordered frame preparation; native queue work is executed
            // by the RHI executor and translation uses independent worker lists.
            const bool retiredCpuFrames =
                RhiCommandListExecutor::FinalizeThreadedSubmissionProduction(m_driver, true);
            if (m_impl.frameLatency != nullptr)
                (void)m_impl.frameLatency->PollCompletedFrames();
            const bool retiredGpuResources = RhiCommandListExecutor::Get().ReleaseRetiredBatches() > 0u;
            const bool scheduledUploads = RhiCommandListExecutor::TryScheduleIdleMeshRuntimeUploads(m_driver);
            if (drainedRenderCommands || retiredCpuFrames || retiredGpuResources || scheduledUploads)
            {
                return true;
            }

            {
                NLS_PROFILE_NAMED_SCOPE("RenderThread::WaitForWake");
                WaitForThreadedWorkerWake(m_impl, observedWakeGeneration);
            }
            return false;
        }

        bool TickRhiLane() override
        {
            SyncStopRequest();
            ApplyThreadDiagnostics(m_rhiThreadId);
            NLS_PROFILE_NAMED_SCOPE("RHI Thread Tick");

            // RenderingThreadSystem pumps the executor's serial submit queue before
            // this tick. Wait for more lane work without spinning.
            if (m_impl.taskGraph == nullptr)
                return false;

            const bool worked = m_impl.taskGraph->WaitForWork(
                NLS::Base::TaskGraph::NamedThread::Rhi,
                kThreadedWorkerWakeTimeout);
            return worked;
        }

        void OnRenderThreadStopping() override
        {
            // Never leave the producer lane locked by an exiting thread.
            RhiCommandListExecutor::DrainProducerOwnedThreadedSubmission(m_driver);
        }

    private:
        /** Mirrors the thread system's stop/fatal state into the driver's flag. */
        void SyncStopRequest()
        {
            if (m_impl.renderingThreads.IsStopRequested())
                m_impl.threadedStopRequested.store(true, std::memory_order_release);
            if (m_impl.renderingThreads.HasFatalError() && m_impl.frameLatency != nullptr)
                m_impl.frameLatency->Stop();
        }

        /**
         * Applies the per-thread diagnostics settings once per thread instance.
         * Comparing thread ids keeps this correct when the system is resumed and
         * creates fresh threads.
         */
        void ApplyThreadDiagnostics(std::thread::id& observedThreadId)
        {
            const auto current = std::this_thread::get_id();
            if (observedThreadId == current)
                return;
            observedThreadId = current;
            Render::Settings::SetThreadDiagnosticsSettings(m_impl.diagnostics);
        }

        Driver& m_driver;
        DriverImpl& m_impl;
        std::thread::id m_renderThreadId {};
        std::thread::id m_rhiThreadId {};
    };

void Driver::StartThreadedRenderingWorkers()
{
    NLS_PROFILE_SCOPE();
    if (m_impl->frameResourceSlots == nullptr || m_impl->threadedWorkersRunning)
        return;

    if (!Detail::SupportsThreadedFoundationExecution(*m_impl))
        return;

    // Resolve the RHI lane mode before starting threads so an unsupported forced
    // mode fails loudly instead of silently degrading to another scheduler.
    const auto capabilities = m_impl->explicitDevice != nullptr
        ? m_impl->explicitDevice->GetCapabilities()
        : Render::RHI::RHIDeviceCapabilities {};
    const auto backendType = m_impl->explicitDevice != nullptr
        ? m_impl->explicitDevice->GetAdapter()->GetBackendType()
        : Render::RHI::NativeBackendType::None;
    const auto requestedMode = m_impl->requestedRhiThreadMode;
    const auto modeResolution = Render::Settings::ResolveRhiThreadMode(
        backendType,
        capabilities,
        requestedMode);
    m_impl->rhiThreadModeSummary = modeResolution.summary;
    if (!modeResolution.accepted)
    {
        throw std::runtime_error("Driver: rejecting RHI thread mode request: " + modeResolution.summary);
    }
    m_impl->rhiThreadMode = modeResolution.mode;

    NLS::Base::TaskGraph::TaskGraphConfig taskGraphConfig;
    // Tasks and None modes run the logical RHI lane on the pool, so the dedicated
    // RHI lane must not be reserved out of the worker budget.
    const bool reserveDedicatedRhiLane =
        modeResolution.mode == Render::Settings::RhiThreadMode::DedicatedThread;
    const auto threadBudget = NLS::Base::Threading::ResolveExecutionPoolBudget(
        std::thread::hardware_concurrency(),
        0u,
        reserveDedicatedRhiLane);
    taskGraphConfig.workerCount = threadBudget.taskGraphWorkerCount;
    taskGraphConfig.enableDiagnostics = true;
    if (!NLS::Base::TaskGraph::InitializeGlobalTaskGraph(taskGraphConfig))
    {
        NLS_LOG_ERROR("Driver: failed to start the rendering TaskGraph");
        return;
    }
    m_impl->taskGraph = &NLS::Base::TaskGraph::GetGlobalTaskGraph();
    // Bind the pool that the async render command pipe executes on so resource
    // preparation overlaps the serial main pipe instead of running inline.
    RenderCommandDispatcher::SetTaskGraph(m_impl->taskGraph);
    NLS_LOG_INFO(
        "Driver: rendering TaskGraph workers=" +
        std::to_string(m_impl->taskGraph->GetWorkerCount()) +
        " jobsForegroundWorkers=" +
        std::to_string(threadBudget.jobsForegroundWorkerCount) +
        " jobsBackgroundWorkers=" +
        std::to_string(threadBudget.jobsBackgroundWorkerCount) +
        " hardwareConcurrency=" +
        std::to_string(std::thread::hardware_concurrency()));
    NLS_LOG_INFO("Driver: RHI thread mode " + m_impl->rhiThreadModeSummary);

    m_impl->threadedStopRequested.store(false);
    m_impl->threadedWorkersRunning = true;
    m_impl->reportedMissingThreadedPipeline = false;

    // The render command fence can only observe submission sequences when
    // something publishes them. Until the RHI executor owns submission, the
    // driver installs its own drain as the flush hook so that asking for a deeper
    // flush depth performs a real wait instead of returning immediately.
    {
        std::weak_ptr<DriverImpl::RenderCommandFlushLifetime> lifetime =
            m_impl->renderCommandFlushLifetime;
        Driver* const flushDriver = this;
        SetRenderCommandFlushHook(
            [lifetime = std::move(lifetime), flushDriver](const RhiFlushFlags flags)
            {
                // A hook left behind by a destroyed driver must not touch it.
                if (lifetime.expired())
                    return;

                // The Render depth was already satisfied by the serial pipe draining
                // its marker, so only the deeper stages need driving here.
                const bool needsReachableDepth =
                    HasFlag(flags, RhiFlushFlags::Dispatch) ||
                    HasFlag(flags, RhiFlushFlags::Translate) ||
                    HasFlag(flags, RhiFlushFlags::Submit) ||
                    HasFlag(flags, RhiFlushFlags::Swapchain);
                const bool needsGpuDepth =
                    HasFlag(flags, RhiFlushFlags::Gpu);
                const bool releasesResources = HasFlag(flags, RhiFlushFlags::ReleaseResources);

                if (needsGpuDepth)
                {
                    // Drains the CPU stages and then the frame fences, which is the
                    // only honest GPU wait this path can offer.
                    if (!DriverRendererAccess::TryWaitForSubmittedGpuWork(*flushDriver))
                        throw std::runtime_error("FlushRenderingCommands could not retire GPU work");
                }
                else if (needsReachableDepth || releasesResources)
                {
                    if (!DriverRendererAccess::TryDrainThreadedRendering(*flushDriver, false))
                        throw std::runtime_error("FlushRenderingCommands could not drain CPU frame work");
                }
                if (releasesResources)
                {
                    (void)RhiCommandListExecutor::Get().ReleaseRetiredBatches();
                    Detail::ReleaseDeferredThreadedFrameScopedResourcesAfterFence(*flushDriver->m_impl);
                }
                auto& executor = RhiCommandListExecutor::Get();
                if (executor.IsInitialized() && !executor.Flush(flags))
                    throw std::runtime_error("FlushRenderingCommands failed: " + executor.GetFailureDiagnostic());
            });
    }

    m_impl->renderingTick = std::make_shared<DriverRenderingTick>(*this, *m_impl);
    RenderingThreadInitParams threadParams;
    threadParams.tick = m_impl->renderingTick.get();
    threadParams.taskGraph = m_impl->taskGraph;
    threadParams.rhiThreadMode = modeResolution.mode;
    // The tick bodies already block on their own wake event, so the wrapper's
    // post-tick idle must stay short: anything longer would add that delay to
    // every frame published right after a tick timeout.
    threadParams.idleWait = std::chrono::milliseconds(1);
    threadParams.registerGameThread = true;
    // Bring the RHI submit lane up before the render/RHI threads start. From this
    // point on every queue submit and present funnels through
    // RhiCommandListExecutor, so no backend queue can be reached from an
    // arbitrary worker or from the render thread directly.
    RhiCommandListExecutor::Get().Initialize(
        modeResolution.mode,
        RhiCommandListExecutor::CreateDeviceTargets(m_impl->explicitDevice),
        m_impl->taskGraph,
        this,
        (std::max)(m_impl->renderDrainTimeout, std::chrono::milliseconds(std::chrono::seconds(10))));
    if (!m_impl->renderingThreads.InitRenderingThreads(threadParams))
    {
        NLS_LOG_ERROR("Driver: failed to start the rendering threads");
        m_impl->renderingTick.reset();
        m_impl->threadedWorkersRunning = false;
        return;
    }
}

void Driver::StopThreadedRenderingWorkers()
{
    NLS_PROFILE_SCOPE();
    if (m_impl == nullptr)
        return;
    if (m_impl->frameLatency != nullptr)
        m_impl->frameLatency->Stop();
    (void)DrainRenderFrameCommands(*m_impl, this, false);
    // Scene/resource release commands are not counted as frames. Join their FIFO
    // marker before the final GPU flush, including commands beyond one tick's slice.
    if (RenderingThreadSystem::HasActiveRenderThread() &&
        !RenderingThreadSystem::IsInRenderingThread() && !HasRenderFatalError())
    {
        try
        {
            ExecuteDriverRenderCommand(*m_impl, [] {});
        }
        catch (const std::exception& exception)
        {
            NLS_LOG_ERROR(std::string("Driver: render command shutdown drain failed: ") + exception.what());
        }
    }
    // Dedicated RHI work must reach the queues while its named lane is alive.
    // Fresh resource-retirement fences also cover native uploads and readbacks.
    if (m_impl->explicitDevice != nullptr && RhiCommandListExecutor::Get().GetOwner() == this &&
        RhiCommandListExecutor::Get().IsInitialized())
    {
        if (!RhiCommandListExecutor::Get().Flush(RhiFlushFlags::Gpu | RhiFlushFlags::ReleaseResources))
            NLS_LOG_ERROR("Driver: GPU resource retirement could not complete during shutdown");
        // Profiler query queues need the submit lane to release their GPU resources.
        const auto nativeDevice = m_impl->explicitDevice->GetNativeDeviceInfo().device;
        if (nativeDevice != nullptr)
            NLS::Base::Profiling::Profiler::ClearGpuContext(nativeDevice);
    }
    m_impl->threadedStopRequested.store(true);
    NotifyThreadedWorkers(*m_impl);
    // Joins the render/RHI threads; the tick's stop hook releases the producer lane
    // before the threads unregister from the TaskGraph.
    m_impl->renderingThreads.ShutdownRenderingThreads();
    // Tear the submit lane down only after the render/RHI threads are joined:
    // outstanding queue operations drain, and later submissions are rejected
    // instead of touching a device that is about to go away.
    RhiCommandListExecutor::Get().Shutdown(RhiFlushFlags::Gpu | RhiFlushFlags::ReleaseResources, this);
    m_impl->renderingTick.reset();
    // The pool is about to stop being usable: unbind it so later async commands
    // fall back to inline execution instead of being rejected.
    RenderCommandDispatcher::SetTaskGraph(nullptr);
    const bool drained = DrainRenderFrameCommands(*m_impl, this);
    if (drained)
        ApplyPendingSwapchainResize();
    m_impl->threadedWorkersRunning = false;

    // No frame will retire from here on, so release every waiter that is still
    // holding a latency budget or it would block the game thread forever.
    if (m_impl->frameLatency != nullptr)
        m_impl->frameLatency->Stop();
}

bool Driver::IsThreadedRenderingEnabled() const
{
    if (!m_impl->threadedWorkersRunning)
        return false;

    return Detail::SupportsThreadedFoundationExecution(*m_impl);
}

std::string Driver::GetRhiThreadModeSummary() const
{
    return m_impl != nullptr ? m_impl->rhiThreadModeSummary : std::string {};
}

}

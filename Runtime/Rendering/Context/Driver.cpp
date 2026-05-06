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
#include <thread>
#include <unordered_map>
#include <vector>
#include <Math/Vector4.h>

#include "Profiling/Profiler.h"
#include "Rendering/Data/PipelineState.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/RHI/Backends/RHIDeviceFactory.h"
#include "Rendering/RHI/Core/RHIDevice.h"
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
#include "Rendering/Context/RenderThreadCoordinator.h"
#include "Rendering/Context/RhiThreadCoordinator.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"
#include "Rendering/Context/SwapchainResizePolicy.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Rendering/Tooling/RenderDocCaptureController.h"
#include "Rendering/Resources/IMesh.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Utils/Conversions.h"

namespace NLS::Render::Context
{
namespace
{
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
            [&texture](const std::weak_ptr<Render::RHI::RHITexture>& storedTexture)
            {
                return storedTexture.lock() == texture;
            });
    }

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
        input.usesDepthStencilAttachment = input.clearDepth || input.clearStencil;

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

    bool ShouldLogThreadedRenderingDiagnostics()
    {
        return Render::Settings::GetThreadDiagnosticsSettings().logRenderDrawPath;
    }

    void LogSkippedPass(const RenderScenePackage& package, const RenderPassCommandInput& passInput, const char* reason)
    {
        if (!ShouldLogThreadedRenderingDiagnostics())
            return;

        const auto passName = !passInput.debugName.empty()
            ? passInput.debugName.c_str()
            : ToPassDebugName(passInput.kind);
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
        const RenderPassCommandInput& input)
    {
        if (!commandBuffer.IsRecording())
        {
            if (ShouldLogThreadedRenderingDiagnostics())
            {
                const auto passName = !input.debugName.empty()
                    ? input.debugName
                    : ToPassDebugName(input.kind);
                NLS_LOG_WARNING("[Driver] BeginPassCommandPlan failed: command buffer is not recording for pass " + passName);
            }
            return false;
        }

        Render::RHI::RHIRenderPassDesc renderPassDesc;
        renderPassDesc.renderArea = { 0, 0, input.renderWidth, input.renderHeight };
        renderPassDesc.debugName = !input.debugName.empty()
            ? input.debugName
            : ToPassDebugName(input.kind);

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
                        const auto passName = !input.debugName.empty()
                            ? input.debugName
                            : ToPassDebugName(input.kind);
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
            renderPassDesc.depthStencilAttachment = std::move(depthAttachment);
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

    void EndPassCommandPlan(Render::RHI::RHICommandBuffer& commandBuffer)
    {
        if (commandBuffer.IsRecording())
            commandBuffer.EndRenderPass();
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

    bool IsEmptySubresourceRange(const Render::RHI::RHISubresourceRange& range)
    {
        return range.mipLevelCount == 0u && range.arrayLayerCount == 0u;
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

    const BufferResourceAccess* FindSourceBufferWriteAccess(
        const RenderPassCommandInput& input,
        const std::shared_ptr<Render::RHI::RHIBuffer>& buffer)
    {
        const auto it = std::find_if(
            input.bufferResourceAccesses.begin(),
            input.bufferResourceAccesses.end(),
            [&buffer](const BufferResourceAccess& access)
            {
                return access.mode == ResourceAccessMode::Write &&
                    access.buffer == buffer;
            });
        return it != input.bufferResourceAccesses.end() ? &(*it) : nullptr;
    }

    const TextureResourceAccess* FindSourceTextureWriteAccess(
        const RenderPassCommandInput& input,
        const std::shared_ptr<Render::RHI::RHITexture>& texture,
        const Render::RHI::RHISubresourceRange& subresourceRange)
    {
        const auto it = std::find_if(
            input.textureResourceAccesses.begin(),
            input.textureResourceAccesses.end(),
            [&texture, &subresourceRange](const TextureResourceAccess& access)
            {
                return access.mode == ResourceAccessMode::Write &&
                    access.texture == texture &&
                    (IsEmptySubresourceRange(subresourceRange) ||
                        IsEmptySubresourceRange(access.subresourceRange) ||
                        AreEqualSubresourceRanges(access.subresourceRange, subresourceRange));
            });
        return it != input.textureResourceAccesses.end() ? &(*it) : nullptr;
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
                return transition.texture == texture &&
                    (IsEmptySubresourceRange(subresourceRange) ||
                        IsEmptySubresourceRange(transition.subresourceRange) ||
                        AreEqualSubresourceRanges(transition.subresourceRange, subresourceRange));
            });
        return it != input.exportedTextureVisibilityTransitions.end() ? &(*it) : nullptr;
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
        if (sourceWriteAccess == nullptr && exportedTransition == nullptr)
            return std::nullopt;

        TextureVisibilityTransition transition;
        transition.texture = targetAccess.texture;
        transition.subresourceRange = !IsEmptySubresourceRange(targetAccess.subresourceRange)
            ? targetAccess.subresourceRange
            : (exportedTransition != nullptr ? exportedTransition->subresourceRange : Render::RHI::RHISubresourceRange{});
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
        Render::RHI::RHIBarrierDesc barriers;
        barriers.bufferBarriers.reserve(input.bufferVisibilityTransitions.size());
        barriers.textureBarriers.reserve(input.textureVisibilityTransitions.size());

        for (const auto& transition : input.bufferVisibilityTransitions)
        {
            if (transition.buffer == nullptr)
                continue;

            auto before = transition.before;
            if (before == Render::RHI::ResourceState::Unknown)
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
            if (before == Render::RHI::ResourceState::Unknown)
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

        if (frameContext != nullptr && frameContext->resourceStateTracker != nullptr)
        {
            FrameGraph::FrameGraphExecutionContext executionContext{
                Context::RequireLocatedDriver("RecordResourceVisibilityTransitions"),
                nullptr,
                &commandBuffer,
                frameContext
            };
            executionContext.RecordResourceBarriers(barriers);
            return true;
        }

        commandBuffer.Barrier(barriers);
        return true;
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
            const auto passName = !passInput.debugName.empty()
                ? passInput.debugName
                : ToPassDebugName(passInput.kind);
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

        const auto vertexBuffer = drawCommand.mesh->GetVertexBuffer();
        if (vertexBuffer == nullptr)
            return false;

        commandBuffer.BindVertexBuffer(0, { vertexBuffer, 0, drawCommand.mesh->GetVertexStride() });

        const auto indexBuffer = drawCommand.mesh->GetIndexBuffer();
        const auto indexCount = drawCommand.mesh->GetIndexCount();
        if (indexBuffer != nullptr && indexCount > 0u)
        {
            commandBuffer.BindIndexBuffer({ indexBuffer, 0, drawCommand.mesh->GetIndexType() });
            commandBuffer.DrawIndexed(indexCount, drawCommand.instanceCount, 0, 0, 0);
            return true;
        }

        const auto vertexCount = drawCommand.mesh->GetVertexCount();
        if (vertexCount > 0u)
        {
            commandBuffer.Draw(vertexCount, drawCommand.instanceCount, 0, 0);
            return true;
        }

        return false;
    }

    uint64_t RecordPreparedDrawCommandsForPass(
        Render::RHI::RHICommandBuffer* commandBuffer,
        const RenderPassCommandInput& input)
    {
        if (commandBuffer == nullptr)
            return input.recordedDrawCommands.empty() ? input.drawCount : 0u;

        if (input.recordedDrawCommands.empty())
            return input.drawCount;

        uint64_t recordedDrawCount = 0u;
        for (const auto& recordedDrawCommand : input.recordedDrawCommands)
        {
            if (RecordPreparedDrawCommand(*commandBuffer, input, recordedDrawCommand))
                ++recordedDrawCount;
        }
        return recordedDrawCount;
    }

    uint32_t CountBindingSetDescriptorSlots(const Render::RHI::RHIBindingSetDesc& desc)
    {
        uint32_t descriptorCount = 0u;
        if (desc.layout != nullptr)
        {
            for (const auto& entry : desc.layout->GetDesc().entries)
                descriptorCount += std::max(1u, entry.count);
        }
        else
        {
            descriptorCount = static_cast<uint32_t>(desc.entries.size());
        }

        return std::max(1u, descriptorCount);
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
        allocationRequest.count = CountBindingSetDescriptorSlots(desc);
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

    uint64_t RecordComputeDispatches(
        Render::RHI::RHICommandBuffer& commandBuffer,
        const std::vector<RecordedComputeDispatchInput>& dispatchInputs)
    {
        uint64_t recordedDispatchCount = 0u;
        for (const auto& dispatchInput : dispatchInputs)
        {
            if (dispatchInput.pipeline == nullptr)
                continue;

            RecordUavBarriersForBuffers(commandBuffer, dispatchInput.uavBarrierBuffersBefore);
            commandBuffer.BindComputePipeline(dispatchInput.pipeline);
            for (const auto& bindingSet : dispatchInput.bindingSets)
            {
                if (bindingSet.bindingSet != nullptr)
                    commandBuffer.BindBindingSet(bindingSet.setIndex, bindingSet.bindingSet);
            }

            commandBuffer.Dispatch(dispatchInput.groupCountX, dispatchInput.groupCountY, dispatchInput.groupCountZ);
            RecordUavBarriersForBuffers(commandBuffer, dispatchInput.uavBarrierBuffersAfter);
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

    void DrainThreadedLifecycleSynchronously(DriverImpl& impl, Driver* driver = nullptr)
    {
        if (impl.threadedLifecycle == nullptr || driver == nullptr)
            return;

        while (impl.threadedLifecycle->GetInFlightDepth() > 0u)
        {
            bool progressed = false;

            if (RenderThreadCoordinator::DrainPendingRenderFrameBuildsSynchronously(*driver))
            {
                progressed = true;
            }

            if (RhiThreadCoordinator::DrainPendingThreadedSubmissions(
                *driver,
                RhiSubmissionAttribution::SynchronousDrain))
            {
                progressed = true;
            }

            if (!progressed)
            {
                // Another worker may currently own a RenderScenePreparing/RhiSubmitting slot.
                // UI composition and shutdown are synchronization points, so wait for ownership
                // to return instead of presenting stale backbuffers or exposing unfinished readback.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    void ReleaseFrameContextResources(Render::RHI::RHIFrameContext& frameContext)
    {
        if (frameContext.frameFence != nullptr && !frameContext.frameFence->IsSignaled())
            frameContext.frameFence->Wait();

        if (frameContext.commandBuffer != nullptr && frameContext.commandBuffer->IsRecording())
            frameContext.commandBuffer->End();

        frameContext.swapchainBackbufferView = nullptr;
        frameContext.swapchainDepthStencilView = nullptr;
        frameContext.swapchainDepthStencilTexture = nullptr;
        frameContext.explicitReadbackTexture = nullptr;

        if (frameContext.resourceStateTracker != nullptr)
        {
            frameContext.resourceStateTracker->RetireTransientResources(std::numeric_limits<uint64_t>::max());
            frameContext.resourceStateTracker->Reset();
        }

        frameContext.uploadContext.reset();
        frameContext.descriptorAllocator.reset();
        frameContext.resourceStateTracker.reset();
        frameContext.computeFinishedSemaphore.reset();
        frameContext.renderFinishedSemaphore.reset();
        frameContext.imageAcquiredSemaphore.reset();
        frameContext.frameFence.reset();
        frameContext.commandBuffer.reset();
        frameContext.commandPool.reset();
        frameContext.hasAcquiredSwapchainImage = false;
        frameContext.swapchainImageIndex = 0u;
        frameContext.uploadBytesReserved = 0u;
    }

}

const char* Detail::ToPassDebugName(const RenderPassCommandKind kind)
{
    return NLS::Render::Context::ToPassDebugName(kind);
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
    const RenderPassCommandInput& input)
{
    return NLS::Render::Context::BeginPassCommandPlan(
        commandBuffer,
        swapchainBackbufferView,
        swapchainDepthStencilView,
        input);
}

void Detail::EndPassCommandPlan(Render::RHI::RHICommandBuffer& commandBuffer)
{
    NLS::Render::Context::EndPassCommandPlan(commandBuffer);
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

uint64_t Detail::RecordComputeDispatches(
    Render::RHI::RHICommandBuffer& commandBuffer,
    const std::vector<RecordedComputeDispatchInput>& dispatchInputs)
{
    return NLS::Render::Context::RecordComputeDispatches(commandBuffer, dispatchInputs);
}

bool Detail::SupportsThreadedFoundationExecution(const DriverImpl& impl)
{
    return NLS::Render::Context::SupportsThreadedFoundationExecution(impl);
}

bool Detail::AllowsThreadedHarnessPublish(const DriverImpl& impl)
{
    return NLS::Render::Context::AllowsThreadedHarnessPublish(impl);
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

std::shared_ptr<Render::RHI::RHIDevice> DriverRendererAccess::GetExplicitDevice(const Driver& driver)
{
	return driver.m_impl->explicitDevice;
}

bool DriverRendererAccess::TryPublishPreparedFrameBuilder(
    Driver& driver,
    const FrameSnapshot& snapshot,
    PreparedRenderSceneBuilder renderSceneBuilder,
    size_t* publishedSlotIndex)
{
    return RenderThreadCoordinator::TryPublishPreparedFrameBuilder(
        driver,
        snapshot,
        std::move(renderSceneBuilder),
        publishedSlotIndex);
}

void DriverRendererAccess::DrainThreadedRendering(Driver& driver)
{
    DrainThreadedLifecycleSynchronously(*driver.m_impl, &driver);
    driver.ApplyPendingSwapchainResize();
}

ThreadedFrameTelemetry DriverRendererAccess::GetThreadedFrameTelemetry(const Driver& driver)
{
    auto telemetry = RenderThreadCoordinator::GetThreadedFrameTelemetry(driver);
    if (driver.m_impl == nullptr)
        return telemetry;

    if (driver.m_impl->pipelineCache != nullptr)
    {
        telemetry.pipelineMainlineActive = true;
        telemetry.pipelineBypassCount = 0u;

        const auto pipelineCacheStats = driver.m_impl->pipelineCache->GetStats();
        telemetry.pipelineCacheGraphicsHits = pipelineCacheStats.graphicsHits;
        telemetry.pipelineCacheGraphicsMisses = pipelineCacheStats.graphicsMisses;
        telemetry.pipelineCacheGraphicsStores = pipelineCacheStats.graphicsStores;
        telemetry.pipelineCacheGraphicsEntries = pipelineCacheStats.graphicsEntryCount;
        telemetry.pipelineCacheComputeHits = pipelineCacheStats.computeHits;
        telemetry.pipelineCacheComputeMisses = pipelineCacheStats.computeMisses;
        telemetry.pipelineCacheComputeStores = pipelineCacheStats.computeStores;
        telemetry.pipelineCacheComputeEntries = pipelineCacheStats.computeEntryCount;
    }
    else
    {
        telemetry.pipelineMainlineActive = false;
        telemetry.pipelineBypassCount = 1u;
    }
    return telemetry;
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

std::shared_ptr<Render::RHI::PipelineCache> DriverRendererAccess::GetPipelineCache(const Driver& driver)
{
    return driver.m_impl->pipelineCache;
}

std::shared_ptr<Render::RHI::DescriptorAllocator> DriverRendererAccess::GetActiveDescriptorAllocator(const Driver& driver)
{
    const auto* frameContext = Detail::GetActiveFrameContext(*driver.m_impl);
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
    const auto frameIndex =
        !driver.m_impl->frameContexts.empty()
            ? driver.m_impl->frameContexts[driver.m_impl->currentFrameIndex % driver.m_impl->frameContexts.size()].frameIndex
            : 0u;
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
	RhiThreadCoordinator::ReadPixels(
        driver,
        x,
        y,
        width,
        height,
        format,
        type,
        data);
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
    RhiThreadCoordinator::ReadPixels(
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

Render::RHI::NativeRenderDeviceInfo DriverUIAccess::GetNativeDeviceInfo(const Driver& driver)
{
	// Use explicit device - it has its own UI resources (renderPass, descriptorPool)
	// created via CreateUIResources() in the constructor
	NLS_ASSERT(driver.m_impl->explicitDevice != nullptr, "Driver requires explicitDevice for all backends");
	return driver.m_impl->explicitDevice->GetNativeDeviceInfo();
}

bool DriverUIAccess::PrepareUIRender(Driver& driver)
{
	return RhiThreadCoordinator::PrepareUIRender(driver);
}

void DriverUIAccess::ReleaseUITextureHandles(Driver& driver)
{
	// Use formal RHI device for UI texture handle release
	driver.m_impl->explicitDevice->ReleaseUITextureHandles();
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

bool DriverUIAccess::QueueRenderDocCapture(Driver& driver, const std::string& label)
{
	return driver.m_impl->renderDocCaptureController != nullptr &&
		driver.m_impl->renderDocCaptureController->QueueCapture(label);
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

void* DriverUIAccess::GetRenderFinishedSemaphore(Driver& driver)
{
	if (driver.m_impl->frameContexts.empty())
		return nullptr;
	auto& frameContext = driver.m_impl->frameContexts[driver.m_impl->currentFrameIndex % driver.m_impl->frameContexts.size()];
	if (frameContext.renderFinishedSemaphore == nullptr)
		return nullptr;
	return frameContext.renderFinishedSemaphore->GetNativeSemaphoreHandle();
}

void DriverUIAccess::SetUISignalSemaphore(Driver& driver, void* semaphore, const uint64_t value)
{
	driver.m_impl->uiRenderFinishedSemaphore = semaphore;
	driver.m_impl->uiRenderFinishedValue = value;
}

void DriverTestAccess::SetExplicitDevice(Driver& driver, std::shared_ptr<Render::RHI::RHIDevice> explicitDevice)
{
	driver.m_impl->explicitDevice = std::move(explicitDevice);
    if (driver.m_impl->explicitDevice != nullptr)
    {
        if (driver.m_impl->pipelineCache == nullptr)
            driver.m_impl->pipelineCache = Render::RHI::CreateDefaultPipelineCache();
    }
    else
    {
        driver.m_impl->pipelineCache.reset();
    }
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
    RememberCompletedReadbackTexture(*driver.m_impl, texture);
}

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

void DriverTestAccess::BeginStandaloneExplicitFrame(Driver& driver, const bool acquireSwapchainImage)
{
    RhiThreadCoordinator::BeginStandaloneExplicitFrame(driver, acquireSwapchainImage);
}

void DriverTestAccess::EndStandaloneExplicitFrame(Driver& driver, const bool presentSwapchain)
{
    RhiThreadCoordinator::EndStandaloneExplicitFrame(driver, presentSwapchain);
}

void DriverTestAccess::PauseThreadedRenderingWorkers(Driver& driver)
{
    driver.m_impl->threadedStopRequested.store(true);
    if (driver.m_impl->renderSceneWorker.joinable())
        driver.m_impl->renderSceneWorker.join();
    if (driver.m_impl->rhiWorker.joinable())
        driver.m_impl->rhiWorker.join();
    driver.m_impl->threadedStopRequested.store(false);
}

void DriverTestAccess::DrainThreadedRendering(Driver& driver)
{
    DrainThreadedLifecycleSynchronously(*driver.m_impl, &driver);
    driver.ApplyPendingSwapchainResize();
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
    if (m_impl->requestedGraphicsBackend != Render::Settings::EGraphicsBackend::NONE &&
        !Render::Settings::IsBackendSelectableForPhase1(m_impl->requestedGraphicsBackend))
    {
        NLS_LOG_WARNING(
            "Driver: rejecting non-DX12 runtime backend during UE5 alignment phase 1: " +
            std::string(Render::Settings::ToString(m_impl->requestedGraphicsBackend)));
    }
	m_impl->renderDocCaptureController = std::make_unique<Render::Tooling::RenderDocCaptureController>(p_driverSettings.renderDoc);

	// All backends use CreateRhiDevice for direct Tier A device creation
	// This creates the Formal RHI device without any IRenderDevice dependency
	m_impl->explicitDevice = Render::Backend::CreateRhiDevice(p_driverSettings);
	if (m_impl->explicitDevice == nullptr)
	{
		NLS_LOG_WARNING(
			std::string("Driver: failed to create explicit RHI device for backend: ") +
			Render::Settings::ToString(p_driverSettings.graphicsBackend) +
			", continuing without device");
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
		const auto frameCount = std::max<uint32_t>(1u, p_driverSettings.framesInFlight);
		m_impl->frameContexts.reserve(frameCount);
		for (uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
		{
			Render::RHI::RHIFrameContext frameContext;
			frameContext.frameIndex = frameIndex;
			frameContext.commandPool = m_impl->explicitDevice->CreateCommandPool(Render::RHI::QueueType::Graphics, "FrameCommandPool" + std::to_string(frameIndex));
			NLS_ASSERT(frameContext.commandPool != nullptr, "Failed to create command pool for explicit RHI");
			frameContext.commandBuffer = frameContext.commandPool->CreateCommandBuffer("FrameCommandBuffer" + std::to_string(frameIndex));
			NLS_ASSERT(frameContext.commandBuffer != nullptr, "Failed to create command buffer for explicit RHI");
			frameContext.frameFence = m_impl->explicitDevice->CreateFence("FrameFence" + std::to_string(frameIndex));
			NLS_ASSERT(frameContext.frameFence != nullptr, "Failed to create fence for explicit RHI");
			frameContext.imageAcquiredSemaphore = m_impl->explicitDevice->CreateSemaphore("FrameAcquire" + std::to_string(frameIndex));
			NLS_ASSERT(frameContext.imageAcquiredSemaphore != nullptr, "Failed to create semaphore for explicit RHI");
			frameContext.renderFinishedSemaphore = m_impl->explicitDevice->CreateSemaphore("FramePresent" + std::to_string(frameIndex));
			NLS_ASSERT(frameContext.renderFinishedSemaphore != nullptr, "Failed to create semaphore for explicit RHI");
			frameContext.computeFinishedSemaphore = m_impl->explicitDevice->CreateSemaphore("FrameCompute" + std::to_string(frameIndex));
			NLS_ASSERT(frameContext.computeFinishedSemaphore != nullptr, "Failed to create compute semaphore for explicit RHI");
			frameContext.resourceStateTracker = Render::RHI::CreateDefaultResourceStateTracker();
			frameContext.descriptorAllocator = Render::RHI::CreateDefaultDescriptorAllocator();
			frameContext.uploadContext = Render::RHI::CreateDefaultUploadContext();
			m_impl->frameContexts.push_back(std::move(frameContext));
		}
	}

    m_impl->diagnostics = p_driverSettings.diagnostics;
    Render::Settings::SetThreadDiagnosticsSettings(p_driverSettings.diagnostics);

    if (p_driverSettings.enableThreadedRendering)
    {
        const auto slotCount = p_driverSettings.threadedFrameSlotCount != 0u
            ? p_driverSettings.threadedFrameSlotCount
            : std::max<uint32_t>(1u, p_driverSettings.framesInFlight);
        m_impl->threadedLifecycle = std::make_unique<ThreadedRenderingLifecycle>(slotCount);
        m_impl->threadedPublishRetirementWaitMs = p_driverSettings.threadedPublishRetirementWaitMs;
        StartThreadedRenderingWorkers();
    }
}

Driver::~Driver()
{
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

    m_impl->swapchainWillResizeCallback = nullptr;
    m_impl->uiRenderFinishedSemaphore = nullptr;
    m_impl->uiRenderFinishedValue = 0u;
    {
        std::lock_guard lock(m_impl->completedReadbackTextureMutex);
        m_impl->completedReadbackTexture = nullptr;
        m_impl->completedReadbackTextureHistory.clear();
    }
    m_impl->explicitFrameActive = false;
    m_impl->uiStandaloneFrameActive = false;
    m_impl->hasPendingSwapchainResize = false;

    if (m_impl->threadedLifecycle != nullptr)
        m_impl->threadedLifecycle->ReleaseRetainedFrameResources();

    for (auto& frameContext : m_impl->frameContexts)
        ReleaseFrameContextResources(frameContext);
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

    std::unique_lock<std::mutex> threadedSubmissionLock;
    if (m_impl->threadedLifecycle != nullptr)
    {
        threadedSubmissionLock = std::unique_lock<std::mutex>(
            m_impl->threadedRhiSubmissionMutex,
            std::try_to_lock);
        if (!threadedSubmissionLock.owns_lock())
            return;
    }

    if (m_impl->threadedLifecycle != nullptr && m_impl->threadedLifecycle->GetInFlightDepth() > 0u)
        return;

	const uint32_t width = m_impl->pendingSwapchainWidth;
	const uint32_t height = m_impl->pendingSwapchainHeight;

	if (m_impl->swapchainWillResizeCallback)
		m_impl->swapchainWillResizeCallback();

	for (auto& frameContext : m_impl->frameContexts)
	{
		if (frameContext.frameFence != nullptr)
			frameContext.frameFence->Wait();
		if (frameContext.commandBuffer != nullptr)
			frameContext.commandBuffer->Reset();
		if (frameContext.commandPool != nullptr)
			frameContext.commandPool->Reset();
		if (frameContext.resourceStateTracker != nullptr)
			frameContext.resourceStateTracker->Reset();
	}

    {
        std::lock_guard lock(m_impl->completedReadbackTextureMutex);
	    m_impl->completedReadbackTexture = nullptr;
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
            size_t slotIndex = 0u;
            if (m_impl->threadedLifecycle->TryBeginNextRenderFrameBuild(
                &slotIndex,
                nullptr))
            {
                const auto resolutionDesc = Detail::BuildRenderScenePreparingResolutionDesc();
                m_impl->threadedLifecycle->ResolveRenderScenePreparing(
                    slotIndex,
                    resolutionDesc);
                continue;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    m_impl->rhiWorker = std::thread([this]()
    {
        NLS_PROFILE_REGISTER_THREAD("RHI Thread");
        Render::Settings::SetThreadDiagnosticsSettings(m_impl->diagnostics);
        while (!m_impl->threadedStopRequested.load())
        {
            NLS_PROFILE_NAMED_SCOPE("RHI Thread Tick");
            if (RhiThreadCoordinator::TryExecuteNextThreadedSubmission(
                *this,
                RhiSubmissionAttribution::Worker))
            {
                continue;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
}

void Driver::StopThreadedRenderingWorkers()
{
    NLS_PROFILE_SCOPE();
    m_impl->threadedStopRequested.store(true);
    if (m_impl->renderSceneWorker.joinable())
        m_impl->renderSceneWorker.join();
    if (m_impl->rhiWorker.joinable())
        m_impl->rhiWorker.join();
    DrainThreadedLifecycleSynchronously(*m_impl, this);
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

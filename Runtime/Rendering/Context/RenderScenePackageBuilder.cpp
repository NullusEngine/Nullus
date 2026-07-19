#include "Rendering/Context/RenderScenePackageBuilder.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>

#include "Rendering/UI/UiDrawDataSnapshot.h"

namespace NLS::Render::Context
{
    namespace
    {
        const char* ToPassDebugName(const RenderPassCommandKind kind)
        {
            switch (kind)
            {
            case RenderPassCommandKind::Opaque: return "ThreadedOpaquePass";
            case RenderPassCommandKind::Decal: return "ThreadedDecalPass";
            case RenderPassCommandKind::Transparent: return "ThreadedTransparentPass";
            case RenderPassCommandKind::Skybox: return "ThreadedSkyboxPass";
            case RenderPassCommandKind::Helper: return "ThreadedHelperPass";
            case RenderPassCommandKind::GBuffer: return "ThreadedGBufferPass";
            case RenderPassCommandKind::Lighting: return "ThreadedLightingPass";
            case RenderPassCommandKind::Compute: return "ThreadedComputePass";
            case RenderPassCommandKind::UIOverlay: return kUIOverlayRenderPassDebugName;
            default: return "ThreadedUnknownPass";
            }
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
            input.requiresLightingData = kind == RenderPassCommandKind::Lighting;
            input.targetsSwapchain = package.targetsSwapchain;
            input.renderWidth = package.renderWidth;
            input.renderHeight = package.renderHeight;
            input.debugName = ToPassDebugName(kind);
            input.clearColor = kind == RenderPassCommandKind::Opaque && package.clearColorBuffer;
            input.clearColorValue = package.clearColorValue;
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

            package.passCommandInputs.push_back(std::move(input));
        }

        void RefreshParallelCommandWorkUnits(RenderScenePackage& package)
        {
            package.parallelCommandWorkUnits.clear();
            package.workUnitDependencyEdges.clear();
            package.parallelDrawCommandBatches.clear();
            package.parallelCommandWorkUnits.reserve(package.passCommandInputs.size());
            for (size_t index = 0; index < package.passCommandInputs.size(); ++index)
            {
                auto workUnits = BuildRecordedDrawCommandWorkUnitsForPass(
                    package.passCommandInputs[index],
                    static_cast<uint64_t>(index),
                    static_cast<uint64_t>(package.parallelCommandWorkUnits.size()));
            package.parallelCommandWorkUnits.insert(
                package.parallelCommandWorkUnits.end(),
                std::make_move_iterator(workUnits.begin()),
                std::make_move_iterator(workUnits.end()));
            }
            package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());
            package.containsParallelCommandWorkUnits = !package.parallelCommandWorkUnits.empty();
            package.parallelDrawCommandBatches = BuildParallelDrawCommandBatchMetadata(package.parallelCommandWorkUnits);
        }

        uint64_t CountVisibleUiDrawCommands(const UI::UiDrawDataSnapshot& snapshot)
        {
            uint64_t drawCount = 0u;
            for (const auto& drawList : snapshot.drawLists)
            {
                for (const auto& command : drawList.commands)
                {
                    if (command.elementCount != 0u &&
                        command.callbackKind != UI::UiDrawCallbackKind::Unsupported &&
                        !command.hasUnsupportedTextureId)
                    {
                        ++drawCount;
                    }
                }
            }
            return drawCount;
        }

        TextureResourceAccess MakeSwapchainOverlayRenderTargetAccess()
        {
            TextureResourceAccess access;
            access.mode = ResourceAccessMode::Write;
            access.state = RHI::ResourceState::RenderTarget;
            access.stages = RHI::PipelineStageMask::RenderTarget;
            access.access = RHI::AccessMask::ColorAttachmentWrite;
            access.subresourceRange = {};
            access.subresourceRange.mipLevelCount = 1u;
            access.subresourceRange.arrayLayerCount = 1u;
            return access;
        }

        TextureVisibilityTransition MakeSwapchainOverlayPresentTransition()
        {
            TextureVisibilityTransition transition;
            transition.subresourceRange = {};
            transition.subresourceRange.mipLevelCount = 1u;
            transition.subresourceRange.arrayLayerCount = 1u;
            transition.before = RHI::ResourceState::RenderTarget;
            transition.after = RHI::ResourceState::Present;
            transition.sourceStages = RHI::PipelineStageMask::RenderTarget;
            transition.destinationStages = RHI::PipelineStageMask::Present;
            transition.sourceAccess = RHI::AccessMask::ColorAttachmentWrite;
            transition.destinationAccess = RHI::AccessMask::Present;
            return transition;
        }

        TextureResourceAccess MakeSceneToUiOverlaySourceAccess(const RenderPassCommandInput& sourcePass)
        {
            const auto accessIt = std::find_if(
                sourcePass.textureResourceAccesses.begin(),
                sourcePass.textureResourceAccesses.end(),
                [](const TextureResourceAccess& access)
                {
                    return access.mode == ResourceAccessMode::Write &&
                        access.state == RHI::ResourceState::RenderTarget &&
                        access.stages == RHI::PipelineStageMask::RenderTarget &&
                        access.access == RHI::AccessMask::ColorAttachmentWrite;
                });

            return accessIt != sourcePass.textureResourceAccesses.end()
                ? *accessIt
                : MakeSwapchainOverlayRenderTargetAccess();
        }
    }

    RenderScenePackage BuildSnapshotOwnedRenderScenePackage(
        const FrameSnapshot& snapshot,
        const SnapshotRenderScenePackageBuildMode buildMode)
    {
        RenderScenePackage package;
        package.frameId = snapshot.frameId;
        package.targetsSwapchain = snapshot.targetsSwapchain;
        package.clearColorValue = snapshot.clearColor;
        package.clearColorBuffer = snapshot.clearColorBuffer;
        package.clearDepthBuffer = snapshot.clearDepthBuffer;
        package.clearStencilBuffer = snapshot.clearStencilBuffer;
        package.renderWidth = snapshot.renderWidth;
        package.renderHeight = snapshot.renderHeight;
        package.sceneGameObjectCount = snapshot.sceneGameObjectCount;
        package.recordedDrawCommands = snapshot.recordedDrawCommands;
        package.postSubmitTextureReadbacks = snapshot.postSubmitTextureReadbacks;
        package.externalSceneOutputIdentity = snapshot.externalOutputIdentity;
        package.externalSceneOutputIdentities = snapshot.externalOutputIdentities;
        package.externalSceneOutputTextureCount = snapshot.externalOutputTextureCount;
        package.externalSceneOutputColorView = snapshot.externalOutputColorView;
        package.streamingDependencyPins = snapshot.streamingDependencyPins;
        package.opaqueDrawCount = snapshot.visibleOpaqueDrawCount;
        package.decalDrawCount = snapshot.visibleDecalDrawCount;
        package.transparentDrawCount = snapshot.visibleTransparentDrawCount;
        package.skyboxDrawCount = snapshot.visibleSkyboxDrawCount;
        package.helperDrawCount = snapshot.visibleHelperDrawCount;
        package.visibleDrawCount =
            package.opaqueDrawCount +
            package.decalDrawCount +
            package.transparentDrawCount +
            package.skyboxDrawCount +
            package.helperDrawCount;
        package.hasVisibleDraws = package.visibleDrawCount > 0u;
        package.hasLightingData = snapshot.sceneLightCount > 0u;
        package.frameDataReady = true;
        package.objectDataReady = true;
        package.lightingDataReady = true;
        package.hasOpaquePass = package.opaqueDrawCount > 0u;
        package.hasDecalPass = package.decalDrawCount > 0u;
        package.hasTransparentPass = package.transparentDrawCount > 0u;
        package.hasSkyboxPass = package.skyboxDrawCount > 0u;
        package.hasHelperPass = package.helperDrawCount > 0u;
        package.drawCommandCount = !package.recordedDrawCommands.empty()
            ? static_cast<uint64_t>(package.recordedDrawCommands.size())
            : package.visibleDrawCount;
        package.materialBatchCount = package.drawCommandCount;
        package.renderTargetUseCount = package.targetsSwapchain ? 1u : 2u;

        if (buildMode == SnapshotRenderScenePackageBuildMode::BuildDefaultPassInputs)
        {
            package.passPlanCount =
                static_cast<uint64_t>(package.hasOpaquePass) +
                static_cast<uint64_t>(package.hasDecalPass) +
                static_cast<uint64_t>(package.hasTransparentPass) +
                static_cast<uint64_t>(package.hasSkyboxPass) +
                static_cast<uint64_t>(package.hasHelperPass);
            package.containsCommandInputs =
                package.drawCommandCount > 0u ||
                package.hasSkyboxPass ||
                package.hasHelperPass;

            size_t nextRecordedDrawCommandIndex = 0u;
            AppendPassCommandInput(package, RenderPassCommandKind::Opaque, package.opaqueDrawCount, nextRecordedDrawCommandIndex);
            AppendPassCommandInput(package, RenderPassCommandKind::Decal, package.decalDrawCount, nextRecordedDrawCommandIndex);
            AppendPassCommandInput(package, RenderPassCommandKind::Skybox, package.skyboxDrawCount, nextRecordedDrawCommandIndex);
            AppendPassCommandInput(package, RenderPassCommandKind::Transparent, package.transparentDrawCount, nextRecordedDrawCommandIndex);
            AppendPassCommandInput(package, RenderPassCommandKind::Helper, package.helperDrawCount, nextRecordedDrawCommandIndex);
            RefreshParallelCommandWorkUnits(package);
        }
        else
        {
            package.passPlanCount = 0u;
            package.containsCommandInputs = false;
            package.parallelCommandWorkUnits.clear();
            package.workUnitDependencyEdges.clear();
            package.parallelDrawCommandBatches.clear();
            package.parallelCommandWorkUnitCount = 0u;
            package.containsParallelCommandWorkUnits = false;
        }

        return package;
    }

    bool AttachUiOverlaySnapshotToRenderScenePackage(
        RenderScenePackage& package,
        std::shared_ptr<const UI::UiDrawDataSnapshot> snapshot)
    {
        if (snapshot == nullptr || !snapshot->hasVisibleDraws)
            return false;

        if (!package.targetsSwapchain)
            return false;

        if (package.hasUIOverlayPass)
            return false;

        const auto uiDrawCount = CountVisibleUiDrawCommands(*snapshot);
        if (uiDrawCount == 0u)
            return false;

        RenderPassCommandInput input;
        input.kind = RenderPassCommandKind::UIOverlay;
        input.queueType = RHI::QueueType::Graphics;
        input.queueDependencyPolicy = QueueDependencyPolicy::Previous;
        input.requiresDependencyVisibility = !package.passCommandInputs.empty();
        input.debugName = kUIOverlayRenderPassDebugName;
        input.drawCount = uiDrawCount;
        input.uiDrawDataSnapshot = snapshot;
        input.requiresFrameData = false;
        input.requiresObjectData = false;
        input.requiresLightingData = false;
        input.targetsSwapchain = true;
        input.renderWidth = package.renderWidth;
        input.renderHeight = package.renderHeight;
        input.clearColor = false;
        input.clearDepth = false;
        input.clearStencil = false;
        input.usesColorAttachment = true;
        input.usesDepthStencilAttachment = false;
        input.writesDepthStencilAttachment = false;
        input.textureResourceAccesses.push_back(MakeSwapchainOverlayRenderTargetAccess());
        input.exportedTextureVisibilityTransitions.push_back(MakeSwapchainOverlayPresentTransition());

        const auto sourcePassIndex = package.passCommandInputs.empty()
            ? kInvalidParallelCommandSourcePassIndex
            : static_cast<uint64_t>(package.passCommandInputs.size() - 1u);
        const auto targetPassIndex = static_cast<uint64_t>(package.passCommandInputs.size());
        package.passCommandInputs.push_back(input);

        auto workUnits = BuildRecordedDrawCommandWorkUnitsForPass(
            package.passCommandInputs.back(),
            targetPassIndex,
            static_cast<uint64_t>(package.parallelCommandWorkUnits.size()));
        package.parallelCommandWorkUnits.insert(
            package.parallelCommandWorkUnits.end(),
            std::make_move_iterator(workUnits.begin()),
            std::make_move_iterator(workUnits.end()));

        if (sourcePassIndex != kInvalidParallelCommandSourcePassIndex &&
            package.parallelCommandWorkUnits.size() >= 2u)
        {
            uint64_t sourceWorkUnitIndex = 0u;
            bool foundSourceWorkUnit = false;
            for (size_t index = package.parallelCommandWorkUnits.size(); index > 0u; --index)
            {
                const auto& workUnit = package.parallelCommandWorkUnits[index - 1u];
                if (workUnit.sourcePassIndex == sourcePassIndex)
                {
                    sourceWorkUnitIndex = workUnit.workUnitIndex;
                    foundSourceWorkUnit = true;
                    break;
                }
            }
            if (foundSourceWorkUnit)
            {
                const uint64_t targetWorkUnitIndex = static_cast<uint64_t>(package.parallelCommandWorkUnits.size() - 1u);
                WorkUnitDependencyEdge edge;
                edge.sourceWorkUnitIndex = sourceWorkUnitIndex;
                edge.targetWorkUnitIndex = targetWorkUnitIndex;
                edge.kind = ThreadedDependencyKind::ResourceVisibility;
                edge.resourceKind = ThreadedDependencyResourceKind::Texture;
                edge.sourceTextureAccess =
                    MakeSceneToUiOverlaySourceAccess(package.passCommandInputs[static_cast<size_t>(sourcePassIndex)]);
                edge.targetTextureAccess = input.textureResourceAccesses.front();
                package.workUnitDependencyEdges.push_back(edge);
                package.parallelCommandWorkUnits[static_cast<size_t>(targetWorkUnitIndex)]
                    .incomingDependencyEdges.push_back(edge);
            }
        }

        package.uiDrawDataSnapshot = std::move(snapshot);
        package.hasUIOverlayPass = true;
        package.uiOverlayDrawCount = uiDrawCount;
        package.visibleDrawCount += uiDrawCount;
        package.hasVisibleDraws = package.visibleDrawCount > 0u;
        package.passPlanCount = static_cast<uint64_t>(package.passCommandInputs.size());
        package.drawCommandCount = !package.recordedDrawCommands.empty()
            ? static_cast<uint64_t>(package.recordedDrawCommands.size()) + package.uiOverlayDrawCount
            : package.visibleDrawCount;
        package.materialBatchCount = package.drawCommandCount;
        package.containsCommandInputs = !package.passCommandInputs.empty();
        package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());
        package.containsParallelCommandWorkUnits = !package.parallelCommandWorkUnits.empty();
        package.parallelDrawCommandBatches = BuildParallelDrawCommandBatchMetadata(package.parallelCommandWorkUnits);
        return true;
    }
}

#include "Rendering/Context/RenderScenePackageBuilder.h"

#include <algorithm>
#include <string>

namespace NLS::Render::Context
{
    namespace
    {
        const char* ToPassDebugName(const RenderPassCommandKind kind)
        {
            switch (kind)
            {
            case RenderPassCommandKind::Opaque: return "ThreadedOpaquePass";
            case RenderPassCommandKind::Transparent: return "ThreadedTransparentPass";
            case RenderPassCommandKind::Skybox: return "ThreadedSkyboxPass";
            case RenderPassCommandKind::Helper: return "ThreadedHelperPass";
            case RenderPassCommandKind::GBuffer: return "ThreadedGBufferPass";
            case RenderPassCommandKind::Lighting: return "ThreadedLightingPass";
            case RenderPassCommandKind::Compute: return "ThreadedComputePass";
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
                ParallelCommandWorkUnit workUnit;
                workUnit.workUnitIndex = static_cast<uint64_t>(index);
                workUnit.submissionOrder = workUnit.workUnitIndex;
                workUnit.commandInput = package.passCommandInputs[index];
                workUnit.debugName = !workUnit.commandInput.debugName.empty()
                    ? workUnit.commandInput.debugName
                    : std::string{};
                package.parallelCommandWorkUnits.push_back(std::move(workUnit));
            }
            package.parallelCommandWorkUnitCount = static_cast<uint64_t>(package.parallelCommandWorkUnits.size());
            package.containsParallelCommandWorkUnits = !package.parallelCommandWorkUnits.empty();
            package.parallelDrawCommandBatches = BuildUE427ParallelDrawCommandBatches(package.parallelCommandWorkUnits);
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
        package.opaqueDrawCount = snapshot.visibleOpaqueDrawCount;
        package.transparentDrawCount = snapshot.visibleTransparentDrawCount;
        package.skyboxDrawCount = snapshot.visibleSkyboxDrawCount;
        package.helperDrawCount = snapshot.visibleHelperDrawCount;
        package.visibleDrawCount =
            package.opaqueDrawCount +
            package.transparentDrawCount +
            package.skyboxDrawCount +
            package.helperDrawCount;
        package.hasVisibleDraws = package.visibleDrawCount > 0u;
        package.hasLightingData = snapshot.sceneLightCount > 0u;
        package.frameDataReady = true;
        package.objectDataReady = true;
        package.lightingDataReady = true;
        package.hasOpaquePass = package.opaqueDrawCount > 0u;
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
                static_cast<uint64_t>(package.hasTransparentPass) +
                static_cast<uint64_t>(package.hasSkyboxPass) +
                static_cast<uint64_t>(package.hasHelperPass);
            package.containsCommandInputs =
                package.drawCommandCount > 0u ||
                package.hasSkyboxPass ||
                package.hasHelperPass;

            size_t nextRecordedDrawCommandIndex = 0u;
            AppendPassCommandInput(package, RenderPassCommandKind::Opaque, package.opaqueDrawCount, nextRecordedDrawCommandIndex);
            AppendPassCommandInput(package, RenderPassCommandKind::Transparent, package.transparentDrawCount, nextRecordedDrawCommandIndex);
            AppendPassCommandInput(package, RenderPassCommandKind::Skybox, package.skyboxDrawCount, nextRecordedDrawCommandIndex);
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
}

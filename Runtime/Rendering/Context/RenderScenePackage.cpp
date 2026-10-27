#include "Rendering/Context/RenderScenePackage.h"

#include <algorithm>
#include <utility>

namespace NLS::Render::Context
{
namespace
{
    ParallelDrawCommandPassRole ResolveParallelDrawCommandPassRole(const RenderPassCommandKind kind)
    {
        switch (kind)
        {
        case RenderPassCommandKind::Decal:
            return ParallelDrawCommandPassRole::Decal;
        case RenderPassCommandKind::Opaque:
        case RenderPassCommandKind::GBuffer:
        case RenderPassCommandKind::Lighting:
            return ParallelDrawCommandPassRole::Opaque;
        case RenderPassCommandKind::Transparent:
            return ParallelDrawCommandPassRole::Transparent;
        case RenderPassCommandKind::Skybox:
            return ParallelDrawCommandPassRole::Skybox;
        case RenderPassCommandKind::Helper:
            return ParallelDrawCommandPassRole::Helper;
        case RenderPassCommandKind::Compute:
            return ParallelDrawCommandPassRole::Compute;
        case RenderPassCommandKind::UIOverlay:
            return ParallelDrawCommandPassRole::UIOverlay;
        default:
            return ParallelDrawCommandPassRole::Auxiliary;
        }
    }

}

bool IsRenderPassEligibleForParallelRecording(const RenderPassCommandInput& passInput)
{
    switch (passInput.kind)
    {
    case RenderPassCommandKind::Compute:
    case RenderPassCommandKind::Lighting:
    case RenderPassCommandKind::UIOverlay:
        return false;
    case RenderPassCommandKind::Opaque:
    case RenderPassCommandKind::Decal:
    case RenderPassCommandKind::Transparent:
    case RenderPassCommandKind::Skybox:
    case RenderPassCommandKind::Helper:
    case RenderPassCommandKind::GBuffer:
    default:
        return passInput.drawCount > 0u || !passInput.recordedDrawCommands.empty();
    }
}

bool CanRenderPassUseRecordedDrawCommandSlices(const RenderPassCommandInput& passInput)
{
    if (!IsRenderPassEligibleForParallelRecording(passInput))
        return false;

    if (!passInput.computeDispatchInputs.empty())
        return false;

    if (passInput.targetsSwapchain)
        return false;

    if (passInput.clearColor ||
        passInput.clearDepth ||
        passInput.usesColorAttachment ||
        passInput.usesDepthStencilAttachment ||
        passInput.writesDepthStencilAttachment ||
        !passInput.colorAttachmentViews.empty() ||
        passInput.depthStencilAttachmentView != nullptr)
    {
        return false;
    }

    if (passInput.clearStencil)
        return false;

    return true;
}

namespace
{
    struct RecordedDrawCommandSliceRange
    {
        uint64_t begin = 0u;
        uint64_t count = 0u;
    };

    bool CanRenderPassUseInRenderPassChildCommandRecording(const RenderPassCommandInput& passInput)
    {
        if (std::any_of(passInput.recordedDrawCommands.begin(),passInput.recordedDrawCommands.end(),
            [](const auto& draw) { return draw.indirectArguments != nullptr; })) return false;
        if (!IsRenderPassEligibleForParallelRecording(passInput))
            return false;

        if (!passInput.computeDispatchInputs.empty())
            return false;

        if (passInput.targetsSwapchain)
            return false;

        if (passInput.clearColor ||
            passInput.clearDepth ||
            passInput.clearStencil ||
            passInput.usesColorAttachment ||
            passInput.usesDepthStencilAttachment ||
            passInput.writesDepthStencilAttachment ||
            !passInput.colorAttachmentViews.empty() ||
            passInput.depthStencilAttachmentView != nullptr)
        {
            return true;
        }

        return false;
    }

    RenderPassCommandInput CopyRenderPassCommandInputWithoutRecordedDrawCommands(
        const RenderPassCommandInput& input)
    {
        RenderPassCommandInput copy;
        copy.kind = input.kind;
        copy.queueType = input.queueType;
        copy.queueDependencyPolicy = input.queueDependencyPolicy;
        copy.dependencySourceWorkUnitIndex = input.dependencySourceWorkUnitIndex;
        copy.requiresDependencyVisibility = input.requiresDependencyVisibility;
        copy.debugName = input.debugName;
        copy.drawCount = input.drawCount;
        copy.uiDrawDataSnapshot = input.uiDrawDataSnapshot;
        copy.outputPipeline = input.outputPipeline;
        copy.outputBindingSet = input.outputBindingSet;
        copy.computeDispatchInputs = input.computeDispatchInputs;
        copy.requiresFrameData = input.requiresFrameData;
        copy.requiresObjectData = input.requiresObjectData;
        copy.requiresLightingData = input.requiresLightingData;
        copy.targetsSwapchain = input.targetsSwapchain;
        copy.renderWidth = input.renderWidth;
        copy.renderHeight = input.renderHeight;
        copy.clearColorValue = input.clearColorValue;
        copy.clearColor = input.clearColor;
        copy.clearDepth = input.clearDepth;
        copy.clearStencil = input.clearStencil;
        copy.usesColorAttachment = input.usesColorAttachment;
        copy.usesDepthStencilAttachment = input.usesDepthStencilAttachment;
        copy.writesDepthStencilAttachment = input.writesDepthStencilAttachment;
        copy.bufferResourceAccesses = input.bufferResourceAccesses;
        copy.textureResourceAccesses = input.textureResourceAccesses;
        copy.bufferVisibilityTransitions = input.bufferVisibilityTransitions;
        copy.textureVisibilityTransitions = input.textureVisibilityTransitions;
        copy.exportedBufferVisibilityTransitions = input.exportedBufferVisibilityTransitions;
        copy.exportedTextureVisibilityTransitions = input.exportedTextureVisibilityTransitions;
        copy.gbufferTextures = input.gbufferTextures;
        copy.colorAttachmentViews = input.colorAttachmentViews;
        copy.depthStencilAttachmentView = input.depthStencilAttachmentView;
        return copy;
    }

    bool AreRenderTargetLayoutsBundleCompatible(
        const RHI::RHIRenderTargetLayoutDesc& lhs,
        const RHI::RHIRenderTargetLayoutDesc& rhs)
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

    const RHI::RHIRenderTargetLayoutDesc* ResolveRecordedDrawRenderTargetLayout(
        const RecordedDrawCommandInput& drawCommand)
    {
        return drawCommand.pipeline != nullptr
            ? &drawCommand.pipeline->GetDesc().renderTargetLayout
            : nullptr;
    }

    std::vector<RecordedDrawCommandSliceRange> BuildFixedRecordedDrawCommandSliceRanges(
        const uint64_t recordedDrawCommandCount)
    {
        std::vector<RecordedDrawCommandSliceRange> ranges;
        if (recordedDrawCommandCount == 0u)
        {
            ranges.push_back({});
            return ranges;
        }

        for (uint64_t begin = 0u; begin < recordedDrawCommandCount;)
        {
            const uint64_t count = std::min(
                kRecordedDrawCommandSliceThreshold,
                recordedDrawCommandCount - begin);
            ranges.push_back({ begin, count });
            begin += count;
        }
        return ranges;
    }

    std::vector<RecordedDrawCommandSliceRange> BuildBundleCompatibleRecordedDrawCommandSliceRanges(
        const std::vector<RecordedDrawCommandInput>& recordedDrawCommands)
    {
        std::vector<RecordedDrawCommandSliceRange> ranges;
        const auto recordedDrawCommandCount = static_cast<uint64_t>(recordedDrawCommands.size());
        if (recordedDrawCommandCount == 0u)
        {
            ranges.push_back({});
            return ranges;
        }

        uint64_t begin = 0u;
        while (begin < recordedDrawCommandCount)
        {
            uint64_t count = 0u;
            const RHI::RHIRenderTargetLayoutDesc* referenceLayout = nullptr;
            while (begin + count < recordedDrawCommandCount &&
                count < kRecordedDrawCommandSliceThreshold)
            {
                const auto& drawCommand = recordedDrawCommands[static_cast<size_t>(begin + count)];
                const auto* drawLayout = ResolveRecordedDrawRenderTargetLayout(drawCommand);
                if (drawLayout != nullptr)
                {
                    if (referenceLayout == nullptr)
                        referenceLayout = drawLayout;
                    else if (!AreRenderTargetLayoutsBundleCompatible(*referenceLayout, *drawLayout))
                        break;
                }
                ++count;
            }

            if (count == 0u)
                count = 1u;

            ranges.push_back({ begin, count });
            begin += count;
        }

        return ranges;
    }
}

std::vector<ParallelDrawCommandBatchMetadata> BuildParallelDrawCommandBatchMetadata(
    const std::vector<ParallelCommandWorkUnit>& workUnits)
{
    std::vector<ParallelDrawCommandBatchMetadata> batches;
    batches.reserve(workUnits.size());

    for (const auto& workUnit : workUnits)
    {
        const auto& commandInput = workUnit.commandInput;
        const uint64_t recordedDrawCommandCount = workUnit.recordedDrawCount != 0u
            ? workUnit.recordedDrawCount
            : static_cast<uint64_t>(commandInput.recordedDrawCommands.size());
        ParallelDrawCommandBatchMetadata batch;
        batch.passRole = ResolveParallelDrawCommandPassRole(commandInput.kind);
        batch.workUnitIndex = workUnit.workUnitIndex;
        batch.submissionOrder = workUnit.submissionOrder;
        batch.debugName = !workUnit.debugName.empty()
            ? workUnit.debugName
            : commandInput.debugName;
        batch.queueType = commandInput.queueType;
        batch.drawCommandCount = commandInput.drawCount > 0u
            ? commandInput.drawCount
            : recordedDrawCommandCount;
        batch.recordedDrawCommandCount = recordedDrawCommandCount;
        batch.eligibleForParallelRecording = workUnit.eligibleForParallelRecording;
        batch.eligibleForParallelTranslation = workUnit.eligibleForParallelTranslation;
        batch.incomingDependencyEdges = workUnit.incomingDependencyEdges;
        batch.sourcePassIndex = workUnit.sourcePassIndex;
        batch.sliceIndex = workUnit.sliceIndex;
        batch.sliceCount = workUnit.sliceCount;
        batch.recordedDrawBegin = workUnit.recordedDrawBegin;
        batch.recordedDrawCount = workUnit.recordedDrawCount;
        batch.requiresOrderedSlicedSubmission = workUnit.requiresOrderedSlicedSubmission;
        batch.usesInRenderPassChildCommandRecording = workUnit.usesInRenderPassChildCommandRecording;
        batches.push_back(std::move(batch));
    }

    return batches;
}

std::vector<ParallelCommandWorkUnit> BuildRecordedDrawCommandWorkUnitsForPass(
    const RenderPassCommandInput& passInput,
    const uint64_t sourcePassIndex,
    const uint64_t workUnitBaseIndex)
{
    const auto recordedDrawCommandCount = static_cast<uint64_t>(passInput.recordedDrawCommands.size());
    const bool useAttachmentFreeSlices =
        CanRenderPassUseRecordedDrawCommandSlices(passInput) &&
        recordedDrawCommandCount > kRecordedDrawCommandSliceThreshold;
    const bool useInRenderPassChildRecording =
        !useAttachmentFreeSlices &&
        CanRenderPassUseInRenderPassChildCommandRecording(passInput) &&
        recordedDrawCommandCount > kRecordedDrawCommandSliceThreshold;
    const bool shouldSlice =
        useAttachmentFreeSlices ||
        useInRenderPassChildRecording;
    std::vector<RecordedDrawCommandSliceRange> sliceRanges;
    if (useAttachmentFreeSlices)
        sliceRanges = BuildFixedRecordedDrawCommandSliceRanges(recordedDrawCommandCount);
    else if (useInRenderPassChildRecording)
        sliceRanges = BuildBundleCompatibleRecordedDrawCommandSliceRanges(passInput.recordedDrawCommands);
    else
        sliceRanges.push_back({ 0u, recordedDrawCommandCount });

    const uint32_t sliceCount = static_cast<uint32_t>(sliceRanges.size());

    std::vector<ParallelCommandWorkUnit> workUnits;
    workUnits.reserve(sliceCount);

    for (uint32_t sliceIndex = 0u; sliceIndex < sliceCount; ++sliceIndex)
    {
        const auto& sliceRange = sliceRanges[static_cast<size_t>(sliceIndex)];
        const uint64_t drawBegin = shouldSlice ? sliceRange.begin : 0u;
        const uint64_t drawCount = shouldSlice ? sliceRange.count : recordedDrawCommandCount;

        RenderPassCommandInput sliceInput = CopyRenderPassCommandInputWithoutRecordedDrawCommands(passInput);
        if (shouldSlice)
        {
            sliceInput.drawCount = drawCount;
            if (sliceIndex > 0u)
            {
                sliceInput.clearColor = false;
                sliceInput.clearDepth = false;
                sliceInput.clearStencil = false;
                sliceInput.bufferVisibilityTransitions.clear();
                sliceInput.textureVisibilityTransitions.clear();
                sliceInput.exportedBufferVisibilityTransitions.clear();
                sliceInput.exportedTextureVisibilityTransitions.clear();
            }
            sliceInput.debugName =
                passInput.debugName +
                ".Slice" +
                std::to_string(sliceIndex + 1u) +
                "of" +
                std::to_string(sliceCount);
        }
        else
        {
            sliceInput.recordedDrawCommands = passInput.recordedDrawCommands;
        }

        ParallelCommandWorkUnit workUnit;
        workUnit.workUnitIndex = workUnitBaseIndex + static_cast<uint64_t>(workUnits.size());
        workUnit.submissionOrder = workUnit.workUnitIndex;
        workUnit.debugName = !sliceInput.debugName.empty()
            ? sliceInput.debugName
            : passInput.debugName;
        workUnit.commandInput = std::move(sliceInput);
        workUnit.sourcePassIndex = sourcePassIndex;
        workUnit.sliceIndex = sliceIndex;
        workUnit.sliceCount = sliceCount;
        workUnit.recordedDrawBegin = shouldSlice ? drawBegin : 0u;
        workUnit.recordedDrawCount = drawCount;
        workUnit.requiresOrderedSlicedSubmission = shouldSlice;
        workUnit.usesInRenderPassChildCommandRecording = useInRenderPassChildRecording;
        workUnit.eligibleForParallelRecording =
            !useInRenderPassChildRecording && IsRenderPassEligibleForParallelRecording(passInput);
        workUnit.eligibleForParallelTranslation = workUnit.eligibleForParallelRecording;
        workUnits.push_back(std::move(workUnit));
    }

    return workUnits;
}

}

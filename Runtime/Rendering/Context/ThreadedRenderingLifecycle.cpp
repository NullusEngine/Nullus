#include "Rendering/Context/ThreadedRenderingLifecycle.h"

#include <algorithm>
#include <exception>
#include <utility>

#include "Profiling/Profiler.h"

namespace NLS::Render::Context
{
namespace
{
    ParallelDrawCommandPassRole ResolveParallelDrawCommandPassRole(const RenderPassCommandKind kind)
    {
        switch (kind)
        {
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
        default:
            return ParallelDrawCommandPassRole::Auxiliary;
        }
    }

    RenderFrameInput BuildRenderFrameInput(const FrameSnapshot& snapshot)
    {
        RenderFrameInput input;
        input.frameId = snapshot.frameId;
        input.sceneRevision = snapshot.sceneRevision;
        input.renderWidth = snapshot.renderWidth;
        input.renderHeight = snapshot.renderHeight;
        input.targetsSwapchain = snapshot.targetsSwapchain;
        input.hasExternalOutput = snapshot.hasExternalOutput;
        input.immutable = true;
        input.hasSceneInput = snapshot.hasSceneInput;
        input.sceneGameObjectCount = snapshot.sceneGameObjectCount;
        input.visibleDrawCount =
            snapshot.visibleOpaqueDrawCount +
            snapshot.visibleTransparentDrawCount +
            snapshot.visibleSkyboxDrawCount +
            snapshot.visibleHelperDrawCount;
        input.externalOutputTextureCount = snapshot.externalOutputTextureCount;
        return input;
    }

    RenderFrameBuild BuildRenderFrameBuild(const RenderScenePackage& renderScenePackage)
    {
        RenderFrameBuild build;
        build.frameId = renderScenePackage.frameId;
        build.renderThreadOwned = true;
        build.targetsSwapchain = renderScenePackage.targetsSwapchain;
        build.hasVisibleDraws = renderScenePackage.hasVisibleDraws;
        build.frameDataReady = renderScenePackage.frameDataReady;
        build.objectDataReady = renderScenePackage.objectDataReady;
        build.lightingDataReady = renderScenePackage.lightingDataReady;
        build.visibleDrawCount = renderScenePackage.visibleDrawCount;
        build.passPlanCount = renderScenePackage.passPlanCount;
        build.drawCommandCount = renderScenePackage.drawCommandCount;
        build.containsParallelCommandWorkUnits = renderScenePackage.containsParallelCommandWorkUnits;
        return build;
    }

    RenderScenePackage BuildFallbackRenderScenePackage(const FrameSnapshot& snapshot)
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
        package.renderTargetUseCount = package.targetsSwapchain ? 1u : 2u;
        return package;
    }

    RenderScenePackage BuildPreparedBuilderMissingRenderScenePackage(
        const FrameSnapshot& snapshot,
        const RenderScenePreparingResolutionDesc& resolutionDesc)
    {
        if (resolutionDesc.buildPreparedBuilderMissingRenderScenePackage)
        {
            try
            {
                return resolutionDesc.buildPreparedBuilderMissingRenderScenePackage(snapshot);
            }
            catch (...)
            {
            }
        }

        return BuildFallbackRenderScenePackage(snapshot);
    }

    RenderScenePackage BuildSnapshotHarnessRenderScenePackage(
        const FrameSnapshot& snapshot,
        const RenderScenePreparingResolutionDesc& resolutionDesc)
    {
        if (resolutionDesc.buildSnapshotHarnessRenderScenePackage)
        {
            try
            {
                return resolutionDesc.buildSnapshotHarnessRenderScenePackage(snapshot);
            }
            catch (...)
            {
            }
        }

        return BuildFallbackRenderScenePackage(snapshot);
    }

    bool RhiSubmissionCompletedSuccessfully(const RhiSubmissionFrame& submissionFrame)
    {
        return submissionFrame.submittedSuccessfully &&
            submissionFrame.currentFrameQueueOperationFailureCount == 0u &&
            !submissionFrame.deviceLostDetected;
    }

    int GetRenderSceneAttributionRank(const InFlightFrameSlot& slot)
    {
        if (slot.renderSceneAttribution == RenderSceneAttribution::Unknown)
            return -1;

        switch (slot.stage)
        {
        case ThreadedFrameStage::Retired:
            return 3;
        case ThreadedFrameStage::RhiSubmitting:
            return 2;
        case ThreadedFrameStage::RenderScenePreparing:
        case ThreadedFrameStage::RenderSceneResolving:
        case ThreadedFrameStage::RenderReady:
            return 1;
        case ThreadedFrameStage::Available:
        case ThreadedFrameStage::Published:
        default:
            return 0;
        }
    }

    int GetRhiSubmissionAttributionRank(const InFlightFrameSlot& slot)
    {
        if (slot.rhiSubmissionAttribution == RhiSubmissionAttribution::Unknown)
            return -1;

        switch (slot.stage)
        {
        case ThreadedFrameStage::Retired:
            return 2;
        case ThreadedFrameStage::RhiSubmitting:
            return 1;
        case ThreadedFrameStage::Available:
        case ThreadedFrameStage::Published:
        case ThreadedFrameStage::RenderScenePreparing:
        case ThreadedFrameStage::RenderSceneResolving:
        case ThreadedFrameStage::RenderReady:
        default:
            return 0;
        }
    }
}

bool IsRenderPassEligibleForParallelRecording(const RenderPassCommandInput& passInput)
{
    switch (passInput.kind)
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
        return lhs.colorFormats == rhs.colorFormats &&
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
        workUnit.eligibleForParallelRecording = false;
        workUnit.eligibleForParallelTranslation = false;
        workUnits.push_back(std::move(workUnit));
    }

    return workUnits;
}

ThreadedRenderingLifecycle::ThreadedRenderingLifecycle(const uint32_t slotCount)
{
    const size_t resolvedSlotCount = std::max<size_t>(1u, slotCount);
    m_slots.reserve(resolvedSlotCount);
    for (size_t slotIndex = 0; slotIndex < resolvedSlotCount; ++slotIndex)
    {
        InFlightFrameSlot slot;
        slot.slotIndex = slotIndex;
        m_slots.push_back(slot);
    }
    RefreshTelemetryLocked();
}

bool ThreadedRenderingLifecycle::TryPublishFrameSnapshot(const FrameSnapshot& snapshot, size_t* publishedSlotIndex)
{
    return PublishFrameSnapshot(snapshot, std::chrono::nanoseconds::zero(), publishedSlotIndex);
}

bool ThreadedRenderingLifecycle::TryPublishPreparedFrame(
    const FrameSnapshot& snapshot,
    const RenderScenePackage& renderScenePackage,
    size_t* publishedSlotIndex)
{
    NLS_PROFILE_SCOPE();
    std::lock_guard<std::mutex> lock(m_mutex);
    return PublishPreparedFrameLocked(snapshot, renderScenePackage, publishedSlotIndex);
}

bool ThreadedRenderingLifecycle::TryPublishPreparedFrameBuilder(
    const FrameSnapshot& snapshot,
    PreparedRenderSceneBuilder renderSceneBuilder,
    size_t* publishedSlotIndex)
{
    NLS_PROFILE_SCOPE();
    std::lock_guard<std::mutex> lock(m_mutex);
    return PublishPreparedFrameBuilderLocked(snapshot, std::move(renderSceneBuilder), publishedSlotIndex);
}

bool ThreadedRenderingLifecycle::PublishPreparedFrameBuilder(
    const FrameSnapshot& snapshot,
    PreparedRenderSceneBuilder renderSceneBuilder,
    const std::chrono::nanoseconds retirementWaitTimeout,
    size_t* publishedSlotIndex)
{
    NLS_PROFILE_SCOPE();
    std::unique_lock<std::mutex> lock(m_mutex);
    const InFlightFrameSlot* slot = FindReusableSlotReadOnlyLocked(true);
    if (slot == nullptr)
    {
        ++m_telemetry.blockedPublishCount;
        m_telemetry.publishState = Data::FramePublishState::BackPressured;
        RefreshTelemetryLocked();

        if (retirementWaitTimeout <= std::chrono::nanoseconds::zero())
            return false;

        const bool hasReusableSlot = m_slotAvailable.wait_for(lock, retirementWaitTimeout, [this]()
        {
            return FindReusableSlotReadOnlyLocked(true) != nullptr;
        });

        if (!hasReusableSlot)
            return false;
    }

    return PublishPreparedFrameBuilderLocked(snapshot, std::move(renderSceneBuilder), publishedSlotIndex);
}

bool ThreadedRenderingLifecycle::PublishFrameSnapshot(
    const FrameSnapshot& snapshot,
    const std::chrono::nanoseconds retirementWaitTimeout,
    size_t* publishedSlotIndex)
{
    NLS_PROFILE_SCOPE();
    std::unique_lock<std::mutex> lock(m_mutex);
    const InFlightFrameSlot* slot = FindReusableSlotReadOnlyLocked();
    if (slot == nullptr)
    {
        ++m_telemetry.blockedPublishCount;
        m_telemetry.publishState = Data::FramePublishState::BackPressured;
        RefreshTelemetryLocked();

        if (retirementWaitTimeout <= std::chrono::nanoseconds::zero())
            return false;

        const bool hasReusableSlot = m_slotAvailable.wait_for(lock, retirementWaitTimeout, [this]()
        {
            return FindReusableSlotReadOnlyLocked() != nullptr;
        });

        if (!hasReusableSlot)
            return false;
    }

    return PublishFrameSnapshotLocked(snapshot, publishedSlotIndex);
}

bool ThreadedRenderingLifecycle::PublishFrameSnapshotLocked(const FrameSnapshot& snapshot, size_t* publishedSlotIndex)
{
    InFlightFrameSlot* slot = FindReusableSlotLocked();
    if (slot == nullptr)
        return false;

    slot->snapshot = snapshot;
    slot->renderFrameInput = BuildRenderFrameInput(snapshot);
    slot->renderFrameBuild.reset();
    slot->publishOrigin = ThreadedFramePublishOrigin::SnapshotHarness;
    slot->renderSceneAttribution = RenderSceneAttribution::Unknown;
    slot->rhiSubmissionAttribution = RhiSubmissionAttribution::Unknown;
    slot->preparedRenderSceneBuilder.reset();
    slot->renderScenePackage.reset();
    slot->submissionFrame.reset();
    slot->stage = ThreadedFrameStage::Published;
    ++m_telemetry.publishedFrameCount;
    m_latestPublishedFrameId = snapshot.frameId;
    m_telemetry.publishState = Data::FramePublishState::Open;
    RefreshTelemetryLocked();

    if (publishedSlotIndex != nullptr)
        *publishedSlotIndex = slot->slotIndex;

    return true;
}

bool ThreadedRenderingLifecycle::PublishPreparedFrameLocked(
    const FrameSnapshot& snapshot,
    const RenderScenePackage& renderScenePackage,
    size_t* publishedSlotIndex)
{
    InFlightFrameSlot* slot = FindReusableSlotLocked(true);
    if (slot == nullptr)
        return false;

    ClearReservedSlotLocked(slot->slotIndex);
    slot->snapshot = snapshot;
    slot->renderFrameInput = BuildRenderFrameInput(snapshot);
    slot->renderFrameBuild = BuildRenderFrameBuild(renderScenePackage);
    slot->publishOrigin = ThreadedFramePublishOrigin::PreparedPackage;
    slot->renderSceneAttribution = RenderSceneAttribution::Unknown;
    slot->rhiSubmissionAttribution = RhiSubmissionAttribution::Unknown;
    slot->preparedRenderSceneBuilder = [renderScenePackage]() mutable
    {
        return renderScenePackage;
    };
    slot->renderScenePackage.reset();
    slot->submissionFrame.reset();
    slot->stage = ThreadedFrameStage::Published;
    ++m_telemetry.publishedFrameCount;
    m_latestPublishedFrameId = snapshot.frameId;
    m_telemetry.publishState = Data::FramePublishState::Open;
    RefreshTelemetryLocked();

    if (publishedSlotIndex != nullptr)
        *publishedSlotIndex = slot->slotIndex;

    return true;
}

bool ThreadedRenderingLifecycle::PublishPreparedFrameBuilderLocked(
    const FrameSnapshot& snapshot,
    PreparedRenderSceneBuilder renderSceneBuilder,
    size_t* publishedSlotIndex)
{
    InFlightFrameSlot* slot = FindReusableSlotLocked(true);
    if (slot == nullptr || !renderSceneBuilder)
        return false;

    ClearReservedSlotLocked(slot->slotIndex);
    slot->snapshot = snapshot;
    slot->renderFrameInput = BuildRenderFrameInput(snapshot);
    slot->renderFrameBuild.reset();
    slot->publishOrigin = ThreadedFramePublishOrigin::PreparedBuilder;
    slot->renderSceneAttribution = RenderSceneAttribution::Unknown;
    slot->rhiSubmissionAttribution = RhiSubmissionAttribution::Unknown;
    slot->preparedRenderSceneBuilder = std::move(renderSceneBuilder);
    slot->renderScenePackage.reset();
    slot->submissionFrame.reset();
    slot->stage = ThreadedFrameStage::Published;
    ++m_telemetry.publishedFrameCount;
    m_latestPublishedFrameId = snapshot.frameId;
    m_telemetry.publishState = Data::FramePublishState::Open;
    RefreshTelemetryLocked();

    if (publishedSlotIndex != nullptr)
        *publishedSlotIndex = slot->slotIndex;

    return true;
}

bool ThreadedRenderingLifecycle::TryBeginNextRenderFrameBuild(size_t* slotIndex, RenderFrameInput* renderFrameInput)
{
    NLS_PROFILE_SCOPE();
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& slot : m_slots)
    {
        if (slot.stage != ThreadedFrameStage::Published || !slot.renderFrameInput.has_value())
            continue;

        slot.stage = ThreadedFrameStage::RenderScenePreparing;
        if (slotIndex != nullptr)
            *slotIndex = slot.slotIndex;
        if (renderFrameInput != nullptr)
            *renderFrameInput = slot.renderFrameInput.value();
        RefreshTelemetryLocked();
        return true;
    }

    return false;
}

bool ThreadedRenderingLifecycle::TryBeginNextRenderScene(size_t* slotIndex, FrameSnapshot* snapshot)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& slot : m_slots)
    {
        if (slot.stage != ThreadedFrameStage::Published || !slot.snapshot.has_value())
            continue;

        slot.stage = ThreadedFrameStage::RenderScenePreparing;
        if (slotIndex != nullptr)
            *slotIndex = slot.slotIndex;
        if (snapshot != nullptr)
            *snapshot = slot.snapshot.value();
        RefreshTelemetryLocked();
        return true;
    }

    return false;
}

bool ThreadedRenderingLifecycle::TryBeginRenderScene(const size_t slotIndex)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto* slot = const_cast<InFlightFrameSlot*>(PeekSlotLocked(slotIndex));
    if (slot == nullptr || slot->stage != ThreadedFrameStage::Published)
        return false;

    slot->stage = ThreadedFrameStage::RenderScenePreparing;
    RefreshTelemetryLocked();
    return true;
}

bool ThreadedRenderingLifecycle::CompleteRenderScene(
    const size_t slotIndex,
    const RenderScenePackage& renderScenePackage,
    const RenderSceneAttribution attribution)
{
    NLS_PROFILE_SCOPE();
    std::lock_guard<std::mutex> lock(m_mutex);
    return CompleteRenderSceneLocked(
        slotIndex,
        renderScenePackage,
        attribution,
        ThreadedFrameStage::RenderScenePreparing);
}

bool ThreadedRenderingLifecycle::CompleteRenderSceneLocked(
    const size_t slotIndex,
    const RenderScenePackage& renderScenePackage,
    const RenderSceneAttribution attribution,
    const ThreadedFrameStage expectedStage)
{
    auto* slot = const_cast<InFlightFrameSlot*>(PeekSlotLocked(slotIndex));
    if (slot == nullptr || slot->stage != expectedStage)
        return false;

    slot->renderScenePackage = renderScenePackage;
    slot->renderFrameBuild = BuildRenderFrameBuild(renderScenePackage);
    slot->preparedRenderSceneBuilder.reset();
    slot->renderSceneAttribution = attribution;
    slot->stage = ThreadedFrameStage::RenderReady;
    RefreshTelemetryLocked();
    return true;
}

bool ThreadedRenderingLifecycle::ResolveRenderScenePreparing(
    const size_t slotIndex,
    const RenderScenePreparingResolutionDesc& resolutionDesc)
{
    NLS_PROFILE_SCOPE();
    FrameSnapshot snapshot;
    ThreadedFramePublishOrigin publishOrigin = ThreadedFramePublishOrigin::Unknown;
    PreparedRenderSceneBuilder renderSceneBuilder;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto slot = std::find_if(
            m_slots.begin(),
            m_slots.end(),
            [slotIndex](const InFlightFrameSlot& candidate)
            {
                return candidate.slotIndex == slotIndex;
            });
        if (slot == m_slots.end() ||
            slot->stage != ThreadedFrameStage::RenderScenePreparing ||
            !slot->snapshot.has_value())
        {
            return false;
        }

        snapshot = slot->snapshot.value();
        publishOrigin = slot->publishOrigin;
        if (slot->preparedRenderSceneBuilder.has_value())
        {
            renderSceneBuilder = std::move(slot->preparedRenderSceneBuilder.value());
            slot->preparedRenderSceneBuilder.reset();
        }
        slot->stage = ThreadedFrameStage::RenderSceneResolving;
        RefreshTelemetryLocked();
    }

    RenderScenePackage renderScenePackage;
    RenderSceneAttribution attribution = RenderSceneAttribution::Unknown;

    if (publishOrigin == ThreadedFramePublishOrigin::SnapshotHarness)
    {
        renderScenePackage = BuildSnapshotHarnessRenderScenePackage(snapshot, resolutionDesc);
        attribution = RenderSceneAttribution::SnapshotHarness;
    }
    else if (renderSceneBuilder)
    {
        try
        {
            renderScenePackage = renderSceneBuilder();
            attribution = RenderSceneAttribution::RendererPrepared;
        }
        catch (const std::exception&)
        {
            renderScenePackage = BuildPreparedBuilderMissingRenderScenePackage(snapshot, resolutionDesc);
            attribution = RenderSceneAttribution::PreparedBuilderMissing;
        }
        catch (...)
        {
            renderScenePackage = BuildPreparedBuilderMissingRenderScenePackage(snapshot, resolutionDesc);
            attribution = RenderSceneAttribution::PreparedBuilderMissing;
        }
    }
    else if (resolutionDesc.buildPreparedBuilderMissingRenderScenePackage)
    {
        renderScenePackage = BuildPreparedBuilderMissingRenderScenePackage(snapshot, resolutionDesc);
        attribution = RenderSceneAttribution::PreparedBuilderMissing;
    }
    else
    {
        renderScenePackage = BuildPreparedBuilderMissingRenderScenePackage(snapshot, resolutionDesc);
        attribution = RenderSceneAttribution::PreparedBuilderMissing;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return CompleteRenderSceneLocked(
            slotIndex,
            renderScenePackage,
            attribution,
            ThreadedFrameStage::RenderSceneResolving);
    }
}

bool ThreadedRenderingLifecycle::TryBeginNextRhiFrameExecution(size_t* slotIndex, RenderFrameBuild* renderFrameBuild)
{
    NLS_PROFILE_SCOPE();
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& slot : m_slots)
    {
        if (slot.stage != ThreadedFrameStage::RenderReady || !slot.renderFrameBuild.has_value())
            continue;

        slot.stage = ThreadedFrameStage::RhiSubmitting;
        if (slotIndex != nullptr)
            *slotIndex = slot.slotIndex;
        if (renderFrameBuild != nullptr)
            *renderFrameBuild = slot.renderFrameBuild.value();
        RefreshTelemetryLocked();
        return true;
    }

    return false;
}

bool ThreadedRenderingLifecycle::TryBeginNextRhiSubmission(size_t* slotIndex, RenderScenePackage* renderScenePackage)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& slot : m_slots)
    {
        if (slot.stage != ThreadedFrameStage::RenderReady || !slot.renderScenePackage.has_value())
            continue;

        slot.stage = ThreadedFrameStage::RhiSubmitting;
        if (slotIndex != nullptr)
            *slotIndex = slot.slotIndex;
        if (renderScenePackage != nullptr)
            *renderScenePackage = slot.renderScenePackage.value();
        RefreshTelemetryLocked();
        return true;
    }

    return false;
}

bool ThreadedRenderingLifecycle::TryBeginRhiSubmission(const size_t slotIndex)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto* slot = const_cast<InFlightFrameSlot*>(PeekSlotLocked(slotIndex));
    if (slot == nullptr || slot->stage != ThreadedFrameStage::RenderReady)
        return false;

    slot->stage = ThreadedFrameStage::RhiSubmitting;
    RefreshTelemetryLocked();
    return true;
}

bool ThreadedRenderingLifecycle::CompleteRhiSubmission(
    const size_t slotIndex,
    const RhiSubmissionFrame& submissionFrame,
    const RhiSubmissionAttribution attribution)
{
    NLS_PROFILE_SCOPE();
    std::lock_guard<std::mutex> lock(m_mutex);
    auto* slot = const_cast<InFlightFrameSlot*>(PeekSlotLocked(slotIndex));
    if (slot == nullptr || slot->stage != ThreadedFrameStage::RhiSubmitting)
        return false;

    slot->submissionFrame = submissionFrame;
    slot->rhiSubmissionAttribution = attribution;
    RefreshTelemetryLocked();
    return true;
}

bool ThreadedRenderingLifecycle::RetireFrame(const size_t slotIndex)
{
    NLS_PROFILE_SCOPE();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto* slot = const_cast<InFlightFrameSlot*>(PeekSlotLocked(slotIndex));
        if (slot == nullptr || slot->stage != ThreadedFrameStage::RhiSubmitting)
            return false;

        if (slot->submissionFrame.has_value() &&
            RhiSubmissionCompletedSuccessfully(slot->submissionFrame.value()))
        {
            m_latestRetiredFrameId = std::max(m_latestRetiredFrameId, slot->submissionFrame->frameId);
        }
        else if (slot->submissionFrame.has_value())
        {
            m_latestFailedRetiredFrameId = std::max(
                m_latestFailedRetiredFrameId,
                slot->submissionFrame->frameId);
        }
        slot->stage = ThreadedFrameStage::Retired;
        RefreshTelemetryLocked();
    }
    m_slotAvailable.notify_one();
    return true;
}

size_t ThreadedRenderingLifecycle::GetSlotCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_slots.size();
}

size_t ThreadedRenderingLifecycle::GetInFlightDepth() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_telemetry.inFlightFrameCount;
}

bool ThreadedRenderingLifecycle::IsBackPressured() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_telemetry.publishState == Data::FramePublishState::BackPressured;
}

uint64_t ThreadedRenderingLifecycle::GetBlockedPublishCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_telemetry.blockedPublishCount;
}

uint64_t ThreadedRenderingLifecycle::GetPublishedFrameCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_telemetry.publishedFrameCount;
}

std::optional<size_t> ThreadedRenderingLifecycle::ReserveReusableSlotIndex(
    const std::chrono::nanoseconds retirementWaitTimeout)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_reservedReusableSlotIndex.has_value())
    {
        const auto* reservedSlot = PeekSlotLocked(m_reservedReusableSlotIndex.value());
        if (reservedSlot != nullptr &&
            (reservedSlot->stage == ThreadedFrameStage::Available || reservedSlot->stage == ThreadedFrameStage::Retired))
        {
            return m_reservedReusableSlotIndex;
        }

        m_reservedReusableSlotIndex.reset();
    }

    if (FindReusableSlotReadOnlyLocked(false) == nullptr)
    {
        if (retirementWaitTimeout <= std::chrono::nanoseconds::zero())
            return std::nullopt;

        const auto waitStart = std::chrono::steady_clock::now();
        ++m_telemetry.reservedSlotWaitCount;
        const bool hasReusableSlot = m_slotAvailable.wait_for(
            lock,
            retirementWaitTimeout,
            [this]()
            {
                return FindReusableSlotReadOnlyLocked(false) != nullptr;
            });
        m_telemetry.reservedSlotWaitTotalNs += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - waitStart).count());
        if (!hasReusableSlot)
        {
            ++m_telemetry.reservedSlotWaitTimeoutCount;
            return std::nullopt;
        }
    }

    const auto* slot = FindReusableSlotReadOnlyLocked(false);
    if (slot == nullptr)
        return std::nullopt;

    m_reservedReusableSlotIndex = slot->slotIndex;
    return m_reservedReusableSlotIndex;
}

bool ThreadedRenderingLifecycle::ReleaseReservedReusableSlotIndex(const size_t slotIndex)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_reservedReusableSlotIndex.has_value() || m_reservedReusableSlotIndex.value() != slotIndex)
        return false;

    m_reservedReusableSlotIndex.reset();
    m_slotAvailable.notify_one();
    return true;
}

std::optional<size_t> ThreadedRenderingLifecycle::GetReservedReusableSlotIndex() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_reservedReusableSlotIndex;
}

const InFlightFrameSlot* ThreadedRenderingLifecycle::PeekSlot(const size_t slotIndex) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return PeekSlotLocked(slotIndex);
}

std::optional<InFlightFrameSlot> ThreadedRenderingLifecycle::CopySlot(const size_t slotIndex) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto* slot = PeekSlotLocked(slotIndex);
    if (slot == nullptr)
        return std::nullopt;

    return *slot;
}

std::vector<InFlightFrameSlot> ThreadedRenderingLifecycle::CopySlots() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_slots;
}

ThreadedFrameTelemetry ThreadedRenderingLifecycle::GetTelemetry() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_telemetry;
}

std::optional<ThreadedFrameTelemetry> ThreadedRenderingLifecycle::TryGetTelemetry() const
{
    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return std::nullopt;

    return m_telemetry;
}

#if defined(NLS_ENABLE_TEST_HOOKS)
bool ThreadedRenderingLifecycleTestAccess::TryLockTelemetry(ThreadedRenderingLifecycle& lifecycle)
{
    return lifecycle.m_mutex.try_lock();
}

void ThreadedRenderingLifecycleTestAccess::UnlockTelemetry(ThreadedRenderingLifecycle& lifecycle)
{
    lifecycle.m_mutex.unlock();
}
#endif

void ThreadedRenderingLifecycle::ReleaseRetainedFrameResources()
{
    std::vector<InFlightFrameSlot> retainedSlots;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        retainedSlots.reserve(m_slots.size());

        for (auto& slot : m_slots)
        {
            InFlightFrameSlot retainedSlot;
            retainedSlot.slotIndex = slot.slotIndex;
            retainedSlot.stage = slot.stage;
            retainedSlot.publishOrigin = slot.publishOrigin;
            retainedSlot.renderSceneAttribution = slot.renderSceneAttribution;
            retainedSlot.rhiSubmissionAttribution = slot.rhiSubmissionAttribution;
            std::swap(retainedSlot.renderFrameInput, slot.renderFrameInput);
            std::swap(retainedSlot.renderFrameBuild, slot.renderFrameBuild);
            std::swap(retainedSlot.snapshot, slot.snapshot);
            std::swap(retainedSlot.preparedRenderSceneBuilder, slot.preparedRenderSceneBuilder);
            std::swap(retainedSlot.renderScenePackage, slot.renderScenePackage);
            std::swap(retainedSlot.submissionFrame, slot.submissionFrame);
            retainedSlots.push_back(std::move(retainedSlot));

            slot.publishOrigin = ThreadedFramePublishOrigin::Unknown;
            slot.renderSceneAttribution = RenderSceneAttribution::Unknown;
            slot.rhiSubmissionAttribution = RhiSubmissionAttribution::Unknown;
            slot.stage = ThreadedFrameStage::Available;
        }

        m_reservedReusableSlotIndex.reset();
        RefreshTelemetryLocked();
    }

    m_slotAvailable.notify_all();
}

InFlightFrameSlot* ThreadedRenderingLifecycle::FindReusableSlotLocked(const bool allowReservedSlot)
{
    if (allowReservedSlot && m_reservedReusableSlotIndex.has_value())
    {
        auto* reservedSlot = const_cast<InFlightFrameSlot*>(PeekSlotLocked(m_reservedReusableSlotIndex.value()));
        if (reservedSlot != nullptr &&
            (reservedSlot->stage == ThreadedFrameStage::Available || reservedSlot->stage == ThreadedFrameStage::Retired))
        {
            reservedSlot->publishOrigin = ThreadedFramePublishOrigin::Unknown;
            reservedSlot->renderSceneAttribution = RenderSceneAttribution::Unknown;
            reservedSlot->rhiSubmissionAttribution = RhiSubmissionAttribution::Unknown;
            reservedSlot->renderFrameInput.reset();
            reservedSlot->renderFrameBuild.reset();
            reservedSlot->snapshot.reset();
            reservedSlot->preparedRenderSceneBuilder.reset();
            reservedSlot->renderScenePackage.reset();
            reservedSlot->submissionFrame.reset();
            return reservedSlot;
        }

        return nullptr;
    }

    for (auto& slot : m_slots)
    {
        if (slot.stage == ThreadedFrameStage::Available || slot.stage == ThreadedFrameStage::Retired)
        {
            if (!allowReservedSlot && IsSlotReservedLocked(slot.slotIndex))
                continue;

            slot.publishOrigin = ThreadedFramePublishOrigin::Unknown;
            slot.renderSceneAttribution = RenderSceneAttribution::Unknown;
            slot.rhiSubmissionAttribution = RhiSubmissionAttribution::Unknown;
            slot.renderFrameInput.reset();
            slot.renderFrameBuild.reset();
            slot.snapshot.reset();
            slot.preparedRenderSceneBuilder.reset();
            slot.renderScenePackage.reset();
            slot.submissionFrame.reset();
            return &slot;
        }
    }

    return nullptr;
}

const InFlightFrameSlot* ThreadedRenderingLifecycle::FindReusableSlotReadOnlyLocked(const bool allowReservedSlot) const
{
    if (allowReservedSlot && m_reservedReusableSlotIndex.has_value())
    {
        const auto* reservedSlot = PeekSlotLocked(m_reservedReusableSlotIndex.value());
        if (reservedSlot != nullptr &&
            (reservedSlot->stage == ThreadedFrameStage::Available || reservedSlot->stage == ThreadedFrameStage::Retired))
        {
            return reservedSlot;
        }

        return nullptr;
    }

    for (const auto& slot : m_slots)
    {
        if (slot.stage == ThreadedFrameStage::Available || slot.stage == ThreadedFrameStage::Retired)
        {
            if (!allowReservedSlot && IsSlotReservedLocked(slot.slotIndex))
                continue;
            return &slot;
        }
    }

    return nullptr;
}

bool ThreadedRenderingLifecycle::IsSlotReservedLocked(const size_t slotIndex) const
{
    return m_reservedReusableSlotIndex.has_value() && m_reservedReusableSlotIndex.value() == slotIndex;
}

void ThreadedRenderingLifecycle::ClearReservedSlotLocked(const size_t slotIndex)
{
    if (IsSlotReservedLocked(slotIndex))
        m_reservedReusableSlotIndex.reset();
}

const InFlightFrameSlot* ThreadedRenderingLifecycle::PeekSlotLocked(const size_t slotIndex) const
{
    return slotIndex < m_slots.size() ? &m_slots[slotIndex] : nullptr;
}

void ThreadedRenderingLifecycle::RefreshTelemetryLocked()
{
    m_telemetry.inFlightFrameCount = 0u;
    m_telemetry.latestPublishedFrameId = m_latestPublishedFrameId;
    m_telemetry.latestRetiredFrameId = m_latestRetiredFrameId;
    m_telemetry.latestFailedRetiredFrameId = m_latestFailedRetiredFrameId;
    m_telemetry.publishOrigin = ThreadedFramePublishOrigin::Unknown;
    m_telemetry.renderSceneAttribution = RenderSceneAttribution::Unknown;
    m_telemetry.rhiSubmissionAttribution = RhiSubmissionAttribution::Unknown;
    m_telemetry.descriptorMainlineActive = false;
    m_telemetry.pipelineMainlineActive = false;
    m_telemetry.transientLifetimeMainlineActive = false;
    m_telemetry.retirementMainlineActive = false;
    m_telemetry.descriptorBypassCount = 0u;
    m_telemetry.pipelineBypassCount = 0u;
    m_telemetry.transientLifetimeBypassCount = 0u;
    m_telemetry.retirementBypassCount = 0u;
    m_telemetry.transientTextureRegistrationCount = 0u;
    m_telemetry.transientBufferRegistrationCount = 0u;
    m_telemetry.retiredTransientTextureCount = 0u;
    m_telemetry.retiredTransientBufferCount = 0u;
    m_telemetry.descriptorTransientPeak = 0u;
    m_telemetry.descriptorAllocationFailures = 0u;
    m_telemetry.pipelineCacheGraphicsHits = 0u;
    m_telemetry.pipelineCacheGraphicsMisses = 0u;
    m_telemetry.pipelineCacheGraphicsStores = 0u;
    m_telemetry.pipelineCacheGraphicsEntries = 0u;
    m_telemetry.pipelineCacheComputeHits = 0u;
    m_telemetry.pipelineCacheComputeMisses = 0u;
    m_telemetry.pipelineCacheComputeStores = 0u;
    m_telemetry.pipelineCacheComputeEntries = 0u;
    m_telemetry.parallelCommandWorkUnitCount = 0u;
    m_telemetry.parallelRecordingWorkerCount = 0u;
    m_telemetry.parallelFallbackReason.clear();
    m_telemetry.queueOperationFailureCount = 0u;
    m_telemetry.lastQueueOperationFailure.clear();
    m_telemetry.currentFrameQueueOperationFailureCount = 0u;
    m_telemetry.currentFrameLastQueueOperationFailure.clear();
    m_telemetry.deviceLostDetected = false;
    m_telemetry.deviceLostReason.clear();
    bool hasPublishedFrames = false;
    bool hasRenderSceneFrames = false;
    bool hasRhiFrames = false;
    bool hasRetiredFrames = false;
    int bestPublishOriginRank = -1;
    int bestRenderSceneAttributionRank = -1;
    int bestRhiSubmissionAttributionRank = -1;
    const RhiSubmissionFrame* latestSubmissionFrame = nullptr;

    for (const auto& slot : m_slots)
    {
        switch (slot.stage)
        {
        case ThreadedFrameStage::Published:
            hasPublishedFrames = true;
            break;
        case ThreadedFrameStage::RenderScenePreparing:
        case ThreadedFrameStage::RenderSceneResolving:
        case ThreadedFrameStage::RenderReady:
            hasRenderSceneFrames = true;
            break;
        case ThreadedFrameStage::RhiSubmitting:
            hasRhiFrames = true;
            break;
        case ThreadedFrameStage::Retired:
            hasRetiredFrames = true;
            break;
        case ThreadedFrameStage::Available:
        default:
            break;
        }

        if (slot.stage != ThreadedFrameStage::Available && slot.stage != ThreadedFrameStage::Retired)
            ++m_telemetry.inFlightFrameCount;

        if (slot.submissionFrame.has_value() &&
            (latestSubmissionFrame == nullptr || slot.submissionFrame->frameId >= latestSubmissionFrame->frameId))
        {
            latestSubmissionFrame = &slot.submissionFrame.value();
        }

        int publishOriginRank = -1;
        switch (slot.publishOrigin)
        {
        case ThreadedFramePublishOrigin::PreparedBuilder:
            publishOriginRank = 2;
            break;
        case ThreadedFramePublishOrigin::PreparedPackage:
            publishOriginRank = 1;
            break;
        case ThreadedFramePublishOrigin::SnapshotHarness:
            publishOriginRank = 0;
            break;
        case ThreadedFramePublishOrigin::Unknown:
        default:
            break;
        }

        if (publishOriginRank >= bestPublishOriginRank)
        {
            bestPublishOriginRank = publishOriginRank;
            m_telemetry.publishOrigin = slot.publishOrigin;
        }

        const int renderSceneAttributionRank = GetRenderSceneAttributionRank(slot);
        if (renderSceneAttributionRank >= bestRenderSceneAttributionRank)
        {
            bestRenderSceneAttributionRank = renderSceneAttributionRank;
            m_telemetry.renderSceneAttribution = slot.renderSceneAttribution;
        }

        const int rhiSubmissionAttributionRank = GetRhiSubmissionAttributionRank(slot);
        if (rhiSubmissionAttributionRank >= bestRhiSubmissionAttributionRank)
        {
            bestRhiSubmissionAttributionRank = rhiSubmissionAttributionRank;
            m_telemetry.rhiSubmissionAttribution = slot.rhiSubmissionAttribution;
        }
    }

    if (latestSubmissionFrame != nullptr)
    {
        m_telemetry.descriptorMainlineActive = latestSubmissionFrame->usedDescriptorAllocator;
        m_telemetry.transientLifetimeMainlineActive = latestSubmissionFrame->usedResourceStateTracker;
        m_telemetry.retirementMainlineActive = latestSubmissionFrame->retirementFenceWaited;
        m_telemetry.descriptorBypassCount = latestSubmissionFrame->usedDescriptorAllocator ? 0u : 1u;
        m_telemetry.transientLifetimeBypassCount = latestSubmissionFrame->usedResourceStateTracker ? 0u : 1u;
        m_telemetry.retirementBypassCount = latestSubmissionFrame->retirementFenceWaited ? 0u : 1u;
        m_telemetry.transientTextureRegistrationCount = latestSubmissionFrame->transientTextureRegistrationCount;
        m_telemetry.transientBufferRegistrationCount = latestSubmissionFrame->transientBufferRegistrationCount;
        m_telemetry.retiredTransientTextureCount = latestSubmissionFrame->retiredTransientTextureCount;
        m_telemetry.retiredTransientBufferCount = latestSubmissionFrame->retiredTransientBufferCount;
        m_telemetry.descriptorTransientPeak = latestSubmissionFrame->descriptorTransientPeak;
        m_telemetry.descriptorAllocationFailures = latestSubmissionFrame->descriptorAllocationFailures;
        m_telemetry.pipelineMainlineActive = latestSubmissionFrame->pipelineMainlineActive;
        m_telemetry.pipelineBypassCount = latestSubmissionFrame->pipelineBypassCount;
        m_telemetry.pipelineCacheGraphicsHits = latestSubmissionFrame->pipelineCacheGraphicsHits;
        m_telemetry.pipelineCacheGraphicsMisses = latestSubmissionFrame->pipelineCacheGraphicsMisses;
        m_telemetry.pipelineCacheGraphicsStores = latestSubmissionFrame->pipelineCacheGraphicsStores;
        m_telemetry.pipelineCacheGraphicsEntries = latestSubmissionFrame->pipelineCacheGraphicsEntries;
        m_telemetry.pipelineCacheComputeHits = latestSubmissionFrame->pipelineCacheComputeHits;
        m_telemetry.pipelineCacheComputeMisses = latestSubmissionFrame->pipelineCacheComputeMisses;
        m_telemetry.pipelineCacheComputeStores = latestSubmissionFrame->pipelineCacheComputeStores;
        m_telemetry.pipelineCacheComputeEntries = latestSubmissionFrame->pipelineCacheComputeEntries;
        m_telemetry.parallelCommandWorkUnitCount = latestSubmissionFrame->recordedWorkUnitCount;
        m_telemetry.parallelRecordingWorkerCount = latestSubmissionFrame->parallelRecordingWorkerCount;
        m_telemetry.parallelFallbackReason = latestSubmissionFrame->parallelFallbackReason;
        m_telemetry.queueOperationFailureCount = latestSubmissionFrame->queueOperationFailureCount;
        m_telemetry.lastQueueOperationFailure = latestSubmissionFrame->lastQueueOperationFailure;
        m_telemetry.currentFrameQueueOperationFailureCount =
            latestSubmissionFrame->currentFrameQueueOperationFailureCount;
        m_telemetry.currentFrameLastQueueOperationFailure =
            latestSubmissionFrame->currentFrameLastQueueOperationFailure;
        m_telemetry.deviceLostDetected = latestSubmissionFrame->deviceLostDetected;
        m_telemetry.deviceLostReason = latestSubmissionFrame->deviceLostReason;
    }

    if (hasRhiFrames)
        m_telemetry.stageSummary = Data::ThreadedFrameStageSummary::Rhi;
    else if (hasRenderSceneFrames)
        m_telemetry.stageSummary = Data::ThreadedFrameStageSummary::RenderScene;
    else if (hasPublishedFrames)
        m_telemetry.stageSummary = Data::ThreadedFrameStageSummary::Logic;
    else if (hasRetiredFrames)
        m_telemetry.stageSummary = Data::ThreadedFrameStageSummary::Retired;
    else
        m_telemetry.stageSummary = Data::ThreadedFrameStageSummary::Direct;

    if (hasRetiredFrames)
        m_telemetry.retirementState = Data::FrameRetirementState::Ready;
    else if (m_telemetry.inFlightFrameCount > 0u)
        m_telemetry.retirementState = Data::FrameRetirementState::Pending;
    else if (m_telemetry.publishedFrameCount > 0u)
        m_telemetry.retirementState = Data::FrameRetirementState::Consumed;
    else
        m_telemetry.retirementState = Data::FrameRetirementState::Direct;

    if (m_telemetry.inFlightFrameCount == 0u && m_telemetry.publishState != Data::FramePublishState::BackPressured)
        m_telemetry.publishState = Data::FramePublishState::Open;
}
}

#include "Rendering/Context/ThreadedRenderingLifecycle.h"

#include <algorithm>
#include <utility>

namespace NLS::Render::Context
{
namespace
{
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
        input.sceneActorCount = snapshot.sceneActorCount;
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
        case ThreadedFrameStage::RenderReady:
        default:
            return 0;
        }
    }
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
    std::lock_guard<std::mutex> lock(m_mutex);
    return PublishPreparedFrameLocked(snapshot, renderScenePackage, publishedSlotIndex);
}

bool ThreadedRenderingLifecycle::TryPublishPreparedFrameBuilder(
    const FrameSnapshot& snapshot,
    PreparedRenderSceneBuilder renderSceneBuilder,
    size_t* publishedSlotIndex)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return PublishPreparedFrameBuilderLocked(snapshot, std::move(renderSceneBuilder), publishedSlotIndex);
}

bool ThreadedRenderingLifecycle::PublishPreparedFrameBuilder(
    const FrameSnapshot& snapshot,
    PreparedRenderSceneBuilder renderSceneBuilder,
    const std::chrono::nanoseconds retirementWaitTimeout,
    size_t* publishedSlotIndex)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    InFlightFrameSlot* slot = FindReusableSlotLocked();
    if (slot == nullptr)
    {
        ++m_telemetry.blockedPublishCount;
        m_telemetry.publishState = Data::FramePublishState::BackPressured;
        RefreshTelemetryLocked();

        if (retirementWaitTimeout <= std::chrono::nanoseconds::zero())
            return false;

        const bool hasReusableSlot = m_slotAvailable.wait_for(lock, retirementWaitTimeout, [this]()
        {
            return FindReusableSlotLocked() != nullptr;
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
    std::unique_lock<std::mutex> lock(m_mutex);
    InFlightFrameSlot* slot = FindReusableSlotLocked();
    if (slot == nullptr)
    {
        ++m_telemetry.blockedPublishCount;
        m_telemetry.publishState = Data::FramePublishState::BackPressured;
        RefreshTelemetryLocked();

        if (retirementWaitTimeout <= std::chrono::nanoseconds::zero())
            return false;

        const bool hasReusableSlot = m_slotAvailable.wait_for(lock, retirementWaitTimeout, [this]()
        {
            return FindReusableSlotLocked() != nullptr;
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
    InFlightFrameSlot* slot = FindReusableSlotLocked();
    if (slot == nullptr)
        return false;

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
    InFlightFrameSlot* slot = FindReusableSlotLocked();
    if (slot == nullptr || !renderSceneBuilder)
        return false;

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
    m_telemetry.publishState = Data::FramePublishState::Open;
    RefreshTelemetryLocked();

    if (publishedSlotIndex != nullptr)
        *publishedSlotIndex = slot->slotIndex;

    return true;
}

bool ThreadedRenderingLifecycle::TryBeginNextRenderFrameBuild(size_t* slotIndex, RenderFrameInput* renderFrameInput)
{
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
    std::lock_guard<std::mutex> lock(m_mutex);
    auto* slot = const_cast<InFlightFrameSlot*>(PeekSlotLocked(slotIndex));
    if (slot == nullptr || slot->stage != ThreadedFrameStage::RenderScenePreparing)
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
    FrameSnapshot snapshot;
    ThreadedFramePublishOrigin publishOrigin = ThreadedFramePublishOrigin::Unknown;
    PreparedRenderSceneBuilder renderSceneBuilder;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto* slot = PeekSlotLocked(slotIndex);
        if (slot == nullptr ||
            slot->stage != ThreadedFrameStage::RenderScenePreparing ||
            !slot->snapshot.has_value())
        {
            return false;
        }

        snapshot = slot->snapshot.value();
        publishOrigin = slot->publishOrigin;
        if (slot->preparedRenderSceneBuilder.has_value())
            renderSceneBuilder = slot->preparedRenderSceneBuilder.value();
    }

    RenderScenePackage renderScenePackage;
    RenderSceneAttribution attribution = RenderSceneAttribution::Unknown;

    if (publishOrigin == ThreadedFramePublishOrigin::SnapshotHarness)
    {
        if (resolutionDesc.buildSnapshotHarnessRenderScenePackage)
            renderScenePackage = resolutionDesc.buildSnapshotHarnessRenderScenePackage(snapshot);

        attribution = RenderSceneAttribution::SnapshotHarness;
    }
    else if (renderSceneBuilder)
    {
        renderScenePackage = renderSceneBuilder();
        attribution = RenderSceneAttribution::RendererPrepared;
    }
    else if (resolutionDesc.buildPreparedBuilderMissingRenderScenePackage)
    {
        renderScenePackage = resolutionDesc.buildPreparedBuilderMissingRenderScenePackage(snapshot);
        attribution = RenderSceneAttribution::PreparedBuilderMissing;
    }
    else
    {
        attribution = RenderSceneAttribution::PreparedBuilderMissing;
    }

    return CompleteRenderScene(slotIndex, renderScenePackage, attribution);
}

bool ThreadedRenderingLifecycle::TryBeginNextRhiFrameExecution(size_t* slotIndex, RenderFrameBuild* renderFrameBuild)
{
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
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto* slot = const_cast<InFlightFrameSlot*>(PeekSlotLocked(slotIndex));
        if (slot == nullptr || slot->stage != ThreadedFrameStage::RhiSubmitting)
            return false;

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

        RefreshTelemetryLocked();
    }

    m_slotAvailable.notify_all();
}

InFlightFrameSlot* ThreadedRenderingLifecycle::FindReusableSlotLocked()
{
    for (auto& slot : m_slots)
    {
        if (slot.stage == ThreadedFrameStage::Available || slot.stage == ThreadedFrameStage::Retired)
        {
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

const InFlightFrameSlot* ThreadedRenderingLifecycle::PeekSlotLocked(const size_t slotIndex) const
{
    return slotIndex < m_slots.size() ? &m_slots[slotIndex] : nullptr;
}

void ThreadedRenderingLifecycle::RefreshTelemetryLocked()
{
    m_telemetry.inFlightFrameCount = 0u;
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

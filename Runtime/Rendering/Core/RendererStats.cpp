#include "Rendering/Core/RendererStats.h"
#include "Rendering/Context/ThreadedRenderingLifecycle.h"

namespace NLS::Render::Core
{
void RendererStats::BeginFrame()
{
    m_frameInfo.batchCount = 0u;
    m_frameInfo.instanceCount = 0u;
    m_frameInfo.polyCount = 0u;
    m_frameInfo.vertexCount = 0u;
    m_frameInfo.inFlightFrameCount = 0u;
    m_frameInfo.blockedFrameCount = 0u;
    m_frameInfo.publishState = Data::FramePublishState::Direct;
    m_frameInfo.stageSummary = Data::ThreadedFrameStageSummary::Direct;
    m_frameInfo.retirementState = Data::FrameRetirementState::Direct;
    m_frameInfo.descriptorMainlineActive = false;
    m_frameInfo.pipelineMainlineActive = false;
    m_frameInfo.transientLifetimeMainlineActive = false;
    m_frameInfo.retirementMainlineActive = false;
    m_frameInfo.descriptorBypassCount = 0u;
    m_frameInfo.pipelineBypassCount = 0u;
    m_frameInfo.transientLifetimeBypassCount = 0u;
    m_frameInfo.retirementBypassCount = 0u;
    m_frameInfo.transientTextureRegistrationCount = 0u;
    m_frameInfo.transientBufferRegistrationCount = 0u;
    m_frameInfo.retiredTransientTextureCount = 0u;
    m_frameInfo.retiredTransientBufferCount = 0u;
    m_frameInfo.descriptorTransientPeak = 0u;
    m_frameInfo.descriptorAllocationFailures = 0u;
    m_frameInfo.pipelineCacheGraphicsHits = 0u;
    m_frameInfo.pipelineCacheGraphicsMisses = 0u;
    m_frameInfo.pipelineCacheGraphicsStores = 0u;
    m_frameInfo.pipelineCacheGraphicsEntries = 0u;
    m_frameInfo.pipelineCacheComputeHits = 0u;
    m_frameInfo.pipelineCacheComputeMisses = 0u;
    m_frameInfo.pipelineCacheComputeStores = 0u;
    m_frameInfo.pipelineCacheComputeEntries = 0u;
    m_isFrameInfoValid = false;
}

void RendererStats::EndFrame()
{
    m_isFrameInfoValid = true;
}

void RendererStats::RecordSubmittedDraw(const Entities::Drawable& drawable, const uint32_t instanceCount)
{
    if (drawable.mesh == nullptr || drawable.material == nullptr || instanceCount == 0u)
        return;

    constexpr uint32_t kVertexCountPerPolygon = 3u;

    ++m_frameInfo.batchCount;
    m_frameInfo.instanceCount += instanceCount;
    m_frameInfo.polyCount += (drawable.mesh->GetIndexCount() / kVertexCountPerPolygon) * instanceCount;
    m_frameInfo.vertexCount += drawable.mesh->GetVertexCount() * instanceCount;
}

void RendererStats::SetThreadedFrameTelemetry(const NLS::Render::Context::ThreadedFrameTelemetry& telemetry)
{
    m_frameInfo.inFlightFrameCount = telemetry.inFlightFrameCount;
    m_frameInfo.blockedFrameCount = telemetry.blockedPublishCount;
    m_frameInfo.publishState = telemetry.publishState;
    m_frameInfo.stageSummary = telemetry.stageSummary;
    m_frameInfo.retirementState = telemetry.retirementState;
    m_frameInfo.descriptorMainlineActive = telemetry.descriptorMainlineActive;
    m_frameInfo.pipelineMainlineActive = telemetry.pipelineMainlineActive;
    m_frameInfo.transientLifetimeMainlineActive = telemetry.transientLifetimeMainlineActive;
    m_frameInfo.retirementMainlineActive = telemetry.retirementMainlineActive;
    m_frameInfo.descriptorBypassCount = telemetry.descriptorBypassCount;
    m_frameInfo.pipelineBypassCount = telemetry.pipelineBypassCount;
    m_frameInfo.transientLifetimeBypassCount = telemetry.transientLifetimeBypassCount;
    m_frameInfo.retirementBypassCount = telemetry.retirementBypassCount;
    m_frameInfo.transientTextureRegistrationCount = telemetry.transientTextureRegistrationCount;
    m_frameInfo.transientBufferRegistrationCount = telemetry.transientBufferRegistrationCount;
    m_frameInfo.retiredTransientTextureCount = telemetry.retiredTransientTextureCount;
    m_frameInfo.retiredTransientBufferCount = telemetry.retiredTransientBufferCount;
    m_frameInfo.descriptorTransientPeak = telemetry.descriptorTransientPeak;
    m_frameInfo.descriptorAllocationFailures = telemetry.descriptorAllocationFailures;
    m_frameInfo.pipelineCacheGraphicsHits = telemetry.pipelineCacheGraphicsHits;
    m_frameInfo.pipelineCacheGraphicsMisses = telemetry.pipelineCacheGraphicsMisses;
    m_frameInfo.pipelineCacheGraphicsStores = telemetry.pipelineCacheGraphicsStores;
    m_frameInfo.pipelineCacheGraphicsEntries = telemetry.pipelineCacheGraphicsEntries;
    m_frameInfo.pipelineCacheComputeHits = telemetry.pipelineCacheComputeHits;
    m_frameInfo.pipelineCacheComputeMisses = telemetry.pipelineCacheComputeMisses;
    m_frameInfo.pipelineCacheComputeStores = telemetry.pipelineCacheComputeStores;
    m_frameInfo.pipelineCacheComputeEntries = telemetry.pipelineCacheComputeEntries;
}

const Data::FrameInfo& RendererStats::GetFrameInfo() const
{
    NLS_ASSERT(m_isFrameInfoValid, "Invalid FrameInfo data! Make sure to retrieve frame info after the frame got fully rendered");
    return m_frameInfo;
}

bool RendererStats::IsFrameInfoValid() const
{
    return m_isFrameInfoValid;
}
}

#include "Rendering/Core/RendererStats.h"

namespace NLS::Render::Core
{
void RendererStats::BeginFrame()
{
    m_frameInfo.batchCount = 0u;
    m_frameInfo.instanceCount = 0u;
    m_frameInfo.polyCount = 0u;
    m_frameInfo.vertexCount = 0u;
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

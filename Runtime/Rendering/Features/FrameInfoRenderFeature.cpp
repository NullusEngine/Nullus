#include "Rendering/Features/FrameInfoRenderFeature.h"
#include "Rendering/Core/CompositeRenderer.h"

namespace NLS::Render::Features
{
FrameInfoRenderFeature::FrameInfoRenderFeature(Core::CompositeRenderer& p_renderer)
    : ARenderFeature(p_renderer), m_isFrameInfoDataValid(true)
{
    m_postDrawListener = m_renderer.postDrawEntityEvent += std::bind(&FrameInfoRenderFeature::OnAfterDraw, this, std::placeholders::_1);
}

FrameInfoRenderFeature::~FrameInfoRenderFeature()
{
    m_renderer.postDrawEntityEvent.RemoveListener(m_postDrawListener);
}

const FrameInfoRenderFeature::FrameInfo& FrameInfoRenderFeature::GetFrameInfo() const
{
    NLS_ASSERT(m_isFrameInfoDataValid, "Invalid FrameInfo data! Make sure to retrieve frame info after the frame got fully rendered");
    return m_frameInfo;
}

void FrameInfoRenderFeature::OnBeginFrame(const Data::FrameDescriptor& p_frameDescriptor)
{
    m_frameInfo.batchCount = 0;
    m_frameInfo.instanceCount = 0;
    m_frameInfo.polyCount = 0;
    m_frameInfo.vertexCount = 0;

    m_isFrameInfoDataValid = false;
}

void FrameInfoRenderFeature::OnEndFrame()
{
    m_isFrameInfoDataValid = true;
}

void FrameInfoRenderFeature::OnAfterDraw(const Drawable& p_drawable)
{
    constexpr uint32_t kVertexCountPerPolygon = 3;

    const int instances = p_drawable.material->GetGPUInstances();

    if (instances > 0)
    {
        ++m_frameInfo.batchCount;
        m_frameInfo.instanceCount += instances;
        m_frameInfo.polyCount += (p_drawable.mesh->GetIndexCount() / kVertexCountPerPolygon) * instances;
        m_frameInfo.vertexCount += p_drawable.mesh->GetVertexCount() * instances;
    }
}
}

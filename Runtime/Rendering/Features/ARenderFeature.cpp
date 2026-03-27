#include "Rendering/Features/ARenderFeature.h"
#include "Rendering/Core/ABaseRenderer.h"
#include "Rendering/Core/CompositeRenderer.h"

namespace NLS::Render::Features
{
ARenderFeature::ARenderFeature(Core::CompositeRenderer& p_renderer)
    : m_renderer(p_renderer)
{
}

void ARenderFeature::SetEnabled(bool p_enabled)
{
    NLS_ASSERT(!m_renderer.IsDrawing(), "Cannot toggle a render feature while rendering is in progress.");
    m_enabled = p_enabled;
}

bool ARenderFeature::IsEnabled() const
{
    return m_enabled;
}

void ARenderFeature::OnBeginFrame(const Data::FrameDescriptor& p_frameDescriptor)
{
}

void ARenderFeature::OnEndFrame()
{
}

void ARenderFeature::OnBeforeDraw(PipelineState& p_pso, const Entities::Drawable& p_drawable)
{
}

void ARenderFeature::OnPrepareExplicitDraw(RHI::RHICommandBuffer&, PipelineState&, const Entities::Drawable&)
{
}

void ARenderFeature::OnAfterDraw(const Entities::Drawable& p_drawable)
{
}
}

#include "Rendering/Features/ARenderFeature.h"
#include "Rendering/Core/ABaseRenderer.h"
#include "Rendering/Core/CompositeRenderer.h"

NLS::Rendering::Features::ARenderFeature::ARenderFeature(Core::CompositeRenderer& p_renderer)
	: m_renderer(p_renderer)
{
}

void NLS::Rendering::Features::ARenderFeature::SetEnabled(bool p_enabled)
{
	NLS_ASSERT(!m_renderer.IsDrawing(), "Cannot toggle a render feature while rendering is in progress.");
	m_enabled = p_enabled;
}

bool NLS::Rendering::Features::ARenderFeature::IsEnabled() const
{
	return m_enabled;
}

void NLS::Rendering::Features::ARenderFeature::OnBeginFrame(const Data::FrameDescriptor& p_frameDescriptor)
{
}

void NLS::Rendering::Features::ARenderFeature::OnEndFrame()
{
}

void NLS::Rendering::Features::ARenderFeature::OnBeforeDraw(Data::PipelineState& p_pso, const Entities::Drawable& p_drawable)
{
}

void NLS::Rendering::Features::ARenderFeature::OnAfterDraw(const Entities::Drawable& p_drawable)
{
}

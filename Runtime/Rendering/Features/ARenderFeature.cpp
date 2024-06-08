#include "Rendering/Features/ARenderFeature.h"
#include "Rendering/Core/ABaseRenderer.h"
#include "Rendering/Core/CompositeRenderer.h"

Rendering::Features::ARenderFeature::ARenderFeature(Core::CompositeRenderer& p_renderer)
	: m_renderer(p_renderer)
{
}

void Rendering::Features::ARenderFeature::SetEnabled(bool p_enabled)
{
	NLS_ASSERT(!m_renderer.IsDrawing(), "Cannot toggle a render feature while rendering is in progress.");
	m_enabled = p_enabled;
}

bool Rendering::Features::ARenderFeature::IsEnabled() const
{
	return m_enabled;
}

void Rendering::Features::ARenderFeature::OnBeginFrame(const Data::FrameDescriptor& p_frameDescriptor)
{
}

void Rendering::Features::ARenderFeature::OnEndFrame()
{
}

void Rendering::Features::ARenderFeature::OnBeforeDraw(Data::PipelineState& p_pso, const Entities::Drawable& p_drawable)
{
}

void Rendering::Features::ARenderFeature::OnAfterDraw(const Entities::Drawable& p_drawable)
{
}

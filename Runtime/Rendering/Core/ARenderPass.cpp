#include "Rendering/Core/ARenderPass.h"
#include "Rendering/Core/CompositeRenderer.h"

#include <Debug/Assertion.h>

NLS::Render::Core::ARenderPass::ARenderPass(Core::CompositeRenderer& p_renderer)
	: m_renderer(p_renderer)
{
}

void NLS::Render::Core::ARenderPass::SetEnabled(bool p_enabled)
{
	NLS_ASSERT(!m_renderer.IsDrawing(), "Cannot toggle a render feature while rendering is in progress.");
	m_enabled = p_enabled;
}

bool NLS::Render::Core::ARenderPass::IsEnabled() const
{
	return m_enabled;
}

void NLS::Render::Core::ARenderPass::OnBeginFrame(const Data::FrameDescriptor& p_frameDescriptor)
{
}

void NLS::Render::Core::ARenderPass::OnEndFrame()
{
}

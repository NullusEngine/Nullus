#include "Rendering/Core/ARenderPass.h"
#include "Rendering/Core/CompositeRenderer.h"

#include <Debug/Assertion.h>

NLS::Rendering::Core::ARenderPass::ARenderPass(Core::CompositeRenderer& p_renderer)
	: m_renderer(p_renderer)
{
}

void NLS::Rendering::Core::ARenderPass::SetEnabled(bool p_enabled)
{
	NLS_ASSERT(!m_renderer.IsDrawing(), "Cannot toggle a render feature while rendering is in progress.");
	m_enabled = p_enabled;
}

bool NLS::Rendering::Core::ARenderPass::IsEnabled() const
{
	return m_enabled;
}

void NLS::Rendering::Core::ARenderPass::OnBeginFrame(const Data::FrameDescriptor& p_frameDescriptor)
{
}

void NLS::Rendering::Core::ARenderPass::OnEndFrame()
{
}

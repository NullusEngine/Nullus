#include "Rendering/Core/ARenderPass.h"
#include "Rendering/Core/CompositeRenderer.h"

#include <Debug/Assertion.h>

Rendering::Core::ARenderPass::ARenderPass(Core::CompositeRenderer& p_renderer)
	: m_renderer(p_renderer)
{
}

void Rendering::Core::ARenderPass::SetEnabled(bool p_enabled)
{
	NLS_ASSERT(!m_renderer.IsDrawing(), "Cannot toggle a render feature while rendering is in progress.");
	m_enabled = p_enabled;
}

bool Rendering::Core::ARenderPass::IsEnabled() const
{
	return m_enabled;
}

void Rendering::Core::ARenderPass::OnBeginFrame(const Data::FrameDescriptor& p_frameDescriptor)
{
}

void Rendering::Core::ARenderPass::OnEndFrame()
{
}

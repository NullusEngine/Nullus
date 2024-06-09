#include <functional>

#include "Rendering/Core/CompositeRenderer.h"

NLS::Rendering::Core::CompositeRenderer::CompositeRenderer(Context::Driver& p_driver)
	: ABaseRenderer(p_driver)
{
}

void NLS::Rendering::Core::CompositeRenderer::BeginFrame(const Data::FrameDescriptor& p_frameDescriptor)
{
	ABaseRenderer::BeginFrame(p_frameDescriptor);

	for (const auto& [_, feature] : m_features)
	{
		if (feature->IsEnabled())
		{
			feature->OnBeginFrame(p_frameDescriptor);
		}
	}

	for (const auto& [_, pass] : m_passes)
	{
		if (pass.second->IsEnabled())
		{
			pass.second->OnBeginFrame(p_frameDescriptor);
		}
	}
}

void NLS::Rendering::Core::CompositeRenderer::DrawFrame()
{
	auto pso = CreatePipelineState();

	for (const auto& [_, pass] : m_passes)
	{
		if (pass.second->IsEnabled())
		{
			pass.second->Draw(pso);
		}
	}
}

void NLS::Rendering::Core::CompositeRenderer::EndFrame()
{
	for (const auto& [_, pass] : m_passes)
	{
		if (pass.second->IsEnabled())
		{
			pass.second->OnEndFrame();
		}
	}

	for (const auto& [_, feature] : m_features)
	{
		if (feature->IsEnabled())
		{
			feature->OnEndFrame();
		}
	}

	ClearDescriptors();
	ABaseRenderer::EndFrame();
}

void NLS::Rendering::Core::CompositeRenderer::DrawEntity(
	NLS::Rendering::Data::PipelineState p_pso,
	const Entities::Drawable& p_drawable
)
{
	for (const auto& [_, feature] : m_features)
	{
		if (feature->IsEnabled())
		{
			feature->OnBeforeDraw(p_pso, p_drawable);
		}
	}

	ABaseRenderer::DrawEntity(p_pso, p_drawable);
	
	for (const auto& [_, feature] : m_features)
	{
		if (feature->IsEnabled())
		{
			feature->OnAfterDraw(p_drawable);
		}
	}
}

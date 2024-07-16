
#include <Rendering/Core/CompositeRenderer.h>

#include "Rendering/EngineBufferRenderFeature.h"
#include "Rendering/EngineDrawableDescriptor.h"
using namespace NLS;
Engine::Rendering::EngineBufferRenderFeature::EngineBufferRenderFeature(NLS::Render::Core::CompositeRenderer& p_renderer)
	: ARenderFeature(p_renderer)
{
	m_engineBuffer = std::make_unique<NLS::Render::Buffers::UniformBuffer>(
		/* UBO Data Layout */
		sizeof(Maths::Matrix4) +
		sizeof(Maths::Matrix4) +
		sizeof(Maths::Matrix4) +
		sizeof(Maths::Vector3) +
		sizeof(float) +
		sizeof(Maths::Matrix4),
		0, 0,
		NLS::Render::Settings::EAccessSpecifier::STREAM_DRAW
	);

	m_startTime = std::chrono::high_resolution_clock::now();
}

void Engine::Rendering::EngineBufferRenderFeature::OnBeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor)
{
	auto currentTime = std::chrono::high_resolution_clock::now();
	auto elapsedTime = std::chrono::duration_cast<std::chrono::duration<float>>(currentTime - m_startTime);

	size_t offset = sizeof(Maths::Matrix4);
	m_engineBuffer->SetSubData(Maths::Matrix4::Transpose(p_frameDescriptor.camera->GetViewMatrix()), std::ref(offset));
	m_engineBuffer->SetSubData(Maths::Matrix4::Transpose(p_frameDescriptor.camera->GetProjectionMatrix()), std::ref(offset));
	m_engineBuffer->SetSubData(p_frameDescriptor.camera->GetPosition(), std::ref(offset));
	m_engineBuffer->SetSubData(elapsedTime.count(), std::ref(offset));
	m_engineBuffer->Bind(0);
}

void Engine::Rendering::EngineBufferRenderFeature::OnEndFrame()
{
	m_engineBuffer->Unbind();
}

void Engine::Rendering::EngineBufferRenderFeature::OnBeforeDraw(NLS::Render::Data::PipelineState& p_pso, const NLS::Render::Entities::Drawable& p_drawable)
{
	EngineDrawableDescriptor descriptor;
	if (p_drawable.TryGetDescriptor<EngineDrawableDescriptor>(descriptor))
	{
		m_engineBuffer->SetSubData(Maths::Matrix4::Transpose(descriptor.modelMatrix), 0);
		m_engineBuffer->SetSubData
		(
			descriptor.userMatrix,

			// UBO layout offset
			sizeof(Maths::Matrix4) +
			sizeof(Maths::Matrix4) +
			sizeof(Maths::Matrix4) +
			sizeof(Maths::Vector3) +
			sizeof(float)
		);
	}
}

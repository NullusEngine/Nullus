#include "Rendering/Features/LightingRenderFeature.h"
#include "Rendering/Core/CompositeRenderer.h"

Rendering::Features::LightingRenderFeature::LightingRenderFeature(Core::CompositeRenderer& p_renderer, uint32_t p_bufferBindingPoint)
	: ARenderFeature(p_renderer), m_bufferBindingPoint(p_bufferBindingPoint)
{
	m_lightBuffer = std::make_unique<Buffers::ShaderStorageBuffer>(Settings::EAccessSpecifier::STREAM_DRAW);
}

bool IsLightInFrustum(const Rendering::Entities::Light& p_light, const Rendering::Data::Frustum& p_frustum)
{
	const auto& position = p_light.transform.GetWorldPosition();
	const auto effectRange = p_light.GetEffectRange();

	// We always consider lights that have an +inf range (Not necessary to test if they are in frustum)
	const bool isOmniscientLight = std::isinf(effectRange);

	return
		isOmniscientLight ||
		p_frustum.SphereInFrustum(position.x, position.y, position.z, p_light.GetEffectRange());
}

void Rendering::Features::LightingRenderFeature::OnBeginFrame(const Data::FrameDescriptor& p_frameDescriptor)
{
	NLS_ASSERT(m_renderer.HasDescriptor<LightingDescriptor>(), "Cannot find LightingDescriptor attached to this renderer");

	auto& lightDescriptor = m_renderer.GetDescriptor<LightingDescriptor>();
	auto& frameDescriptor = m_renderer.GetFrameDescriptor();

	std::vector<Maths::Matrix4> lightMatrices;
	auto frustum = frameDescriptor.camera->GetLightFrustum();

	for (auto light : lightDescriptor.lights)
	{
		if (!frustum || IsLightInFrustum(light.get(), *frustum))
		{
			lightMatrices.push_back(light.get().GenerateMatrix());
		}
	}

	m_lightBuffer->SendBlocks<Maths::Matrix4>(lightMatrices.data(), lightMatrices.size() * sizeof(Maths::Matrix4));

	m_lightBuffer->Bind(m_bufferBindingPoint);
}

void Rendering::Features::LightingRenderFeature::OnEndFrame()
{
	m_lightBuffer->Unbind();
}

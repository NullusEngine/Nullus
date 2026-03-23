#include "Rendering/Features/LightingRenderFeature.h"
#include "Rendering/Core/CompositeRenderer.h"

namespace
{
using Light = NLS::Render::Entities::Light;
using Frustum = NLS::Render::Data::Frustum;

bool IsLightInFrustum(const Light& light, const Frustum& frustum)
{
	const auto& position = light.transform->GetWorldPosition();
	const auto effectRange = light.GetEffectRange();

	// We always consider lights that have an +inf range (Not necessary to test if they are in frustum)
	const bool isOmniscientLight = std::isinf(effectRange);

	return isOmniscientLight || frustum.SphereInFrustum(position.x, position.y, position.z, light.GetEffectRange());
}
}

namespace NLS::Render::Features
{
LightingRenderFeature::LightingRenderFeature(Core::CompositeRenderer& renderer, uint32_t bufferBindingPoint)
	: ARenderFeature(renderer), m_bufferBindingPoint(bufferBindingPoint)
{
	m_lightBuffer = std::make_unique<Buffers::ShaderStorageBuffer>(Settings::EAccessSpecifier::STREAM_DRAW);
}

void LightingRenderFeature::OnBeginFrame(const Data::FrameDescriptor& frameDescriptor)
{
	NLS_ASSERT(m_renderer.HasDescriptor<LightingDescriptor>(), "Cannot find LightingDescriptor attached to this renderer");

	auto& lightDescriptor = m_renderer.GetDescriptor<LightingDescriptor>();
	auto& currentFrameDescriptor = m_renderer.GetFrameDescriptor();

	std::vector<Maths::Matrix4> lightMatrices;
	auto frustum = currentFrameDescriptor.camera->GetLightFrustum();

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

void LightingRenderFeature::OnEndFrame()
{
	m_lightBuffer->Unbind();
}
}

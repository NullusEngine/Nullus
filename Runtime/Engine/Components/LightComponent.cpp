#include "Components/LightComponent.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"
#include "Rendering/Entities/Light.h"

#include <algorithm>
#include <cmath>

namespace NLS::Engine::Components
{
LightComponent::LightComponent()
{
}

void LightComponent::OnCreate()
{
		m_data = new Render::Entities::Light(&m_owner->GetTransform()->GetTransform());
		SetLightType(lightType);
		SetColor(color);
		SetIntensity(intensity);
		SetRange(range);
		SetCutoff(cutoff);
		SetOuterCutoff(outerCutoff);
		SetSize(size);
}

void LightComponent::SetLightType(Render::Settings::ELightType type)
{
		lightType = type;
		if (m_data)
			m_data->type = type;
	MarkRenderStateChanged();
}

const Render::Entities::Light* LightComponent::GetData() const
{
	return m_data;
}

const Maths::Vector3& LightComponent::GetColor() const
{
		return color;
}

float LightComponent::GetIntensity() const
{
		return intensity;
}

void LightComponent::SetColor(const Maths::Vector3& p_color)
{
		color = p_color;
		if (m_data)
			m_data->color = p_color;
	MarkRenderStateChanged();
}

void LightComponent::SetIntensity(float p_intensity)
{
		intensity = p_intensity;
		if (m_data)
			m_data->intensity = p_intensity;
	MarkRenderStateChanged();
}

float LightComponent::GetRange() const
{
		return std::isfinite(range) && range > 0.0f ? range : 0.0f;
}

void LightComponent::SetRange(float p_range)
{
		range = std::isfinite(p_range) ? std::max(p_range, 0.0f) : 0.0f;
		if (m_data)
			m_data->range = range;
	MarkRenderStateChanged();
}

float LightComponent::GetCutoff() const
{
		return cutoff;
}

float LightComponent::GetOuterCutoff() const
{
		return outerCutoff;
}

void LightComponent::SetCutoff(float p_cutoff)
{
		cutoff = p_cutoff;
		if (m_data)
			m_data->cutoff = p_cutoff;
	MarkRenderStateChanged();
}

void LightComponent::SetOuterCutoff(float p_outerCutoff)
{
		outerCutoff = p_outerCutoff;
		if (m_data)
			m_data->outerCutoff = p_outerCutoff;
	MarkRenderStateChanged();
}

Maths::Vector3 LightComponent::GetSize() const
{
		return size;
}

void LightComponent::SetSize(const Maths::Vector3& p_size)
{
		size = p_size;
		if (m_data)
			m_data->size = p_size;
	MarkRenderStateChanged();
}

Render::Settings::ELightType LightComponent::GetLightType() const
{
		return lightType;
}
} // namespace NLS::Engine::Components

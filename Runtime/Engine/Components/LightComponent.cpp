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
}

void LightComponent::SetLightType(Render::Settings::ELightType type)
{
	m_data->type = type;
}

const Render::Entities::Light* LightComponent::GetData() const
{
	return m_data;
}

const Maths::Vector3& LightComponent::GetColor() const
{
	return m_data->color;
}

float LightComponent::GetIntensity() const
{
	return m_data->intensity;
}

void LightComponent::SetColor(const Maths::Vector3& p_color)
{
	m_data->color = p_color;
}

void LightComponent::SetIntensity(float p_intensity)
{
	m_data->intensity = p_intensity;
}

float LightComponent::GetRange() const
{
	return m_data->GetSafeRange();
}

void LightComponent::SetRange(float p_range)
{
	m_data->range = std::isfinite(p_range) ? std::max(p_range, 0.0f) : 0.0f;
}

float LightComponent::GetCutoff() const
{
	return m_data->cutoff;
}

float LightComponent::GetOuterCutoff() const
{
	return m_data->outerCutoff;
}

void LightComponent::SetCutoff(float p_cutoff)
{
	m_data->cutoff = p_cutoff;
}

void LightComponent::SetOuterCutoff(float p_outerCutoff)
{
	m_data->outerCutoff = p_outerCutoff;
}

Maths::Vector3 LightComponent::GetSize() const
{
	return m_data->size;
}

void LightComponent::SetSize(const Maths::Vector3& p_size)
{
	m_data->size = p_size;
}

Render::Settings::ELightType LightComponent::GetLightType() const
{
    return m_data->type;
}
} // namespace NLS::Engine::Components

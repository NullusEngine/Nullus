#include "Components/LightComponent.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"
#include "Rendering/Entities/Light.h"

#include <algorithm>
#include <cmath>

namespace NLS::Engine::Components
{
namespace
{
	constexpr float kPointLightVisibilityThreshold = 1.0f / 255.0f;
	constexpr float kDefaultPointConstant = 1.0f;
	constexpr float kDefaultPointLinear = 0.09f;
	constexpr float kDefaultPointQuadratic = 0.032f;

	void ApplyPointLightDefaults(Render::Entities::Light& light)
	{
		light.constant = kDefaultPointConstant;
		light.linear = kDefaultPointLinear;
		light.quadratic = kDefaultPointQuadratic;
	}

	void SetPointLightEffectRadius(Render::Entities::Light& light, float radius)
	{
		const float safeRadius = std::max(radius, 0.001f);
		const float targetDenominator = std::abs(light.intensity) / kPointLightVisibilityThreshold;
		const float quadratic =
			(targetDenominator - light.constant - light.linear * safeRadius) /
			(safeRadius * safeRadius);
		light.quadratic = std::max(quadratic, 0.0f);
	}
}

LightComponent::LightComponent()
{
}

void LightComponent::OnCreate()
{
	m_data = new Render::Entities::Light(&m_owner->GetTransform()->GetTransform());
	ApplyPointLightDefaults(*m_data);
}

void LightComponent::SetLightType(Render::Settings::ELightType type)
{
	m_data->type = type;
	switch (type)
	{
	case Render::Settings::ELightType::POINT:
		ApplyPointLightDefaults(*m_data);
		break;
	case Render::Settings::ELightType::DIRECTIONAL:
		break;
	case Render::Settings::ELightType::SPOT:
		ApplyPointLightDefaults(*m_data);
		break;
	case Render::Settings::ELightType::AMBIENT_BOX:
		break;
	case Render::Settings::ELightType::AMBIENT_SPHERE:
		m_data->intensity = 0.1f;
		m_data->constant = 1.0f;
		break;
	default:
		break;
	}
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

float LightComponent::GetConstant() const
{
	return m_data->constant;
}

float LightComponent::GetLinear() const
{
	return m_data->linear;
}

float LightComponent::GetQuadratic() const
{
	return m_data->quadratic;
}

void LightComponent::SetConstant(float p_constant)
{
	m_data->constant = p_constant;
}

void LightComponent::SetLinear(float p_linear)
{
	m_data->linear = p_linear;
}

void LightComponent::SetQuadratic(float p_quadratic)
{
	m_data->quadratic = p_quadratic;
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

float LightComponent::GetRadius() const
{
	switch (m_data->type)
	{
	case Render::Settings::ELightType::POINT:
	case Render::Settings::ELightType::SPOT:
		return m_data->GetEffectRange();
	case Render::Settings::ELightType::AMBIENT_SPHERE:
		return m_data->constant;
	default:
		return m_data->constant;
	}
}

void LightComponent::SetRadius(float p_radius)
{
	switch (m_data->type)
	{
	case Render::Settings::ELightType::POINT:
	case Render::Settings::ELightType::SPOT:
		SetPointLightEffectRadius(*m_data, p_radius);
		break;
	case Render::Settings::ELightType::AMBIENT_SPHERE:
	default:
		m_data->constant = p_radius;
		break;
	}
}

Maths::Vector3 LightComponent::GetSize() const
{
	return { m_data->constant, m_data->linear, m_data->quadratic };
}

void LightComponent::SetSize(const Maths::Vector3& p_size)
{
	m_data->constant = p_size.x;
	m_data->linear = p_size.y;
	m_data->quadratic = p_size.z;
}

Render::Settings::ELightType LightComponent::GetLightType() const
{
    return m_data->type;
}
} // namespace NLS::Engine::Components

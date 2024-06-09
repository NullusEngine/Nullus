#include "Components/LightComponent.h"
#include "Components/TransformComponent.h"
#include "GameObject.h""
using namespace NLS::Engine::Components;
NLS::Engine::Components::LightComponent::LightComponent()
{
}

void NLS::Engine::Components::LightComponent::OnCreate()
{
	m_data = new Light(m_owner->GetTransform()->GetTransform());
}

void NLS::Engine::Components::LightComponent::SetLightType(Settings::ELightType type)
{
	m_data->type = type;
	switch (type)
	{
	case NLS::Rendering::Settings::ELightType::POINT:
		break;
	case NLS::Rendering::Settings::ELightType::DIRECTIONAL:
		break;
	case NLS::Rendering::Settings::ELightType::SPOT:
		break;
	case NLS::Rendering::Settings::ELightType::AMBIENT_BOX:
		break;
	case NLS::Rendering::Settings::ELightType::AMBIENT_SPHERE:
		m_data->intensity = 0.1f;
		m_data->constant = 1.0f;
		break;
	default:
		break;
	}
}

const NLS::Rendering::Entities::Light* LightComponent::GetData() const
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

float NLS::Engine::Components::LightComponent::GetConstant() const
{
	return m_data->constant;
}

float NLS::Engine::Components::LightComponent::GetLinear() const
{
	return m_data->linear;
}

float NLS::Engine::Components::LightComponent::GetQuadratic() const
{
	return m_data->quadratic;
}

void NLS::Engine::Components::LightComponent::SetConstant(float p_constant)
{
	m_data->constant = p_constant;
}

void NLS::Engine::Components::LightComponent::SetLinear(float p_linear)
{
	m_data->linear = p_linear;
}

void NLS::Engine::Components::LightComponent::SetQuadratic(float p_quadratic)
{
	m_data->quadratic = p_quadratic;
}

float NLS::Engine::Components::LightComponent::GetCutoff() const
{
	return m_data->cutoff;
}

float NLS::Engine::Components::LightComponent::GetOuterCutoff() const
{
	return m_data->outerCutoff;
}

void NLS::Engine::Components::LightComponent::SetCutoff(float p_cutoff)
{
	m_data->cutoff = p_cutoff;
}

void NLS::Engine::Components::LightComponent::SetOuterCutoff(float p_outerCutoff)
{
	m_data->outerCutoff = p_outerCutoff;
}

float NLS::Engine::Components::LightComponent::GetRadius() const
{
	return m_data->quadratic;
}

void NLS::Engine::Components::LightComponent::SetRadius(float p_radius)
{
	m_data->constant = p_radius;
}

Maths::Vector3 NLS::Engine::Components::LightComponent::GetSize() const
{
	return { m_data->constant, m_data->linear, m_data->quadratic };
}

void NLS::Engine::Components::LightComponent::SetSize(const Maths::Vector3& p_size)
{
	m_data->constant = p_size.x;
	m_data->linear = p_size.y;
	m_data->quadratic = p_size.z;
}

#include "Components/CameraComponent.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"
using namespace NLS;
using namespace NLS::Engine::Components;
using namespace NLS::Render::Entities;

NLS::Engine::Components::CameraComponent::CameraComponent()
{


}

void NLS::Engine::Components::CameraComponent::OnCreate()
{
	m_camera = new Camera(&m_owner->GetTransform()->GetTransform());
	/* Default clear color for the CCamera (Different from Camera default clear color) */
	SetClearColor({ 0.1921569f, 0.3019608f, 0.4745098f });
}
#include "UDRefl/ReflMngr.hpp"
using namespace UDRefl;
void CameraComponent::Bind()
{
    Mngr.RegisterType<CameraComponent>();
    Mngr.AddBases<CameraComponent, Component>();
}

void CameraComponent::SetFov(float p_value)
{
	m_camera->SetFov(p_value);
}

void CameraComponent::SetSize(float p_value)
{
    m_camera->SetSize(p_value);
}

void CameraComponent::SetNear(float p_value)
{
	m_camera->SetNear(p_value);
}

void CameraComponent::SetFar(float p_value)
{
	m_camera->SetFar(p_value);
}

void CameraComponent::SetFrustumGeometryCulling(bool p_enable)
{
	m_camera->SetFrustumGeometryCulling(p_enable);
}

void CameraComponent::SetFrustumLightCulling(bool p_enable)
{
	m_camera->SetFrustumLightCulling(p_enable);
}

void CameraComponent::SetProjectionMode(NLS::Render::Settings::EProjectionMode p_projectionMode)
{
    m_camera->SetProjectionMode(p_projectionMode);
}

float CameraComponent::GetFov() const
{
	return m_camera->GetFov();
}

float CameraComponent::GetSize() const
{
    return m_camera->GetSize();
}

float CameraComponent::GetNear() const
{
	return m_camera->GetNear();
}

float CameraComponent::GetFar() const
{
	return m_camera->GetFar();
}

const Maths::Vector3 & CameraComponent::GetClearColor() const
{
	return m_camera->GetClearColor();
}

bool CameraComponent::HasFrustumGeometryCulling() const
{
	return m_camera->HasFrustumGeometryCulling();
}

void CameraComponent::SetClearColor(const Maths::Vector3 & p_clearColor)
{
	m_camera->SetClearColor(p_clearColor);
}

bool CameraComponent::HasFrustumLightCulling() const
{
	return m_camera->HasFrustumLightCulling();
}

NLS::Render::Settings::EProjectionMode CameraComponent::GetProjectionMode() const
{
    return m_camera->GetProjectionMode();
}

NLS::Render::Entities::Camera* CameraComponent::GetCamera()
{
	return m_camera;
}


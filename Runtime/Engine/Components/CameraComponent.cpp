#include "Components/CameraComponent.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"
#include "Rendering/Entities/Camera.h"
#include "SceneSystem/Scene.h"

namespace NLS::Engine::Components
{
struct CameraComponent::PresentationCamera
{
    Maths::Transform pose;
    Render::Entities::Camera camera{&pose};
};

Render::Entities::Camera* CameraComponent::GetRenderCamera()
{
    if (!m_camera || !m_owner) return m_camera;
    const auto* scene = m_owner->GetScene();
    if (!scene || !scene->IsPlaying() || !scene->GetPhysicsSettings().interpolate) return m_camera;
    const auto presented = scene->CapturePhysicsPresentationTransform(*m_owner);
    if (!presented.usesPhysicsPose) return m_camera;
    if (!m_presentationCamera) m_presentationCamera = std::make_shared<PresentationCamera>();
    auto& snapshot = *m_presentationCamera;
    snapshot.pose.SetLocalPosition(presented.worldPosition);
    snapshot.pose.SetLocalRotation(presented.worldRotation);
    snapshot.pose.SetLocalScale(presented.worldScale);
    snapshot.camera = *m_camera;
    snapshot.camera.transform = &snapshot.pose;
    return &snapshot.camera;
}

CameraComponent::CameraComponent()
{
}

void CameraComponent::OnCreate()
{
		m_camera = new Render::Entities::Camera(&m_owner->GetTransform()->GetTransform());
		SetFov(fov);
		SetSize(size);
		SetNear(nearClip);
		SetFar(farClip);
		SetClearColor(clearColor);
		SetFrustumGeometryCulling(frustumGeometryCulling);
		SetFrustumLightCulling(frustumLightCulling);
		SetProjectionMode(projectionMode);
		SetVisibleLayers(visibleLayers);
}

void CameraComponent::SetFov(float p_value)
{
		fov = p_value;
		if (m_camera)
			m_camera->SetFov(p_value);
	MarkRenderStateChanged();
}

void CameraComponent::SetSize(float p_value)
{
		size = p_value;
		if (m_camera)
            m_camera->SetSize(p_value);
	MarkRenderStateChanged();
}

void CameraComponent::SetNear(float p_value)
{
		nearClip = p_value;
		if (m_camera)
			m_camera->SetNear(p_value);
	MarkRenderStateChanged();
}

void CameraComponent::SetFar(float p_value)
{
		farClip = p_value;
		if (m_camera)
			m_camera->SetFar(p_value);
	MarkRenderStateChanged();
}

void CameraComponent::SetFrustumGeometryCulling(bool p_enable)
{
		frustumGeometryCulling = p_enable;
		if (m_camera)
			m_camera->SetFrustumGeometryCulling(p_enable);
	MarkRenderStateChanged();
}

void CameraComponent::SetFrustumLightCulling(bool p_enable)
{
		frustumLightCulling = p_enable;
		if (m_camera)
			m_camera->SetFrustumLightCulling(p_enable);
	MarkRenderStateChanged();
}

void CameraComponent::SetProjectionMode(Render::Settings::EProjectionMode p_projectionMode)
{
		projectionMode = p_projectionMode;
		if (m_camera)
            m_camera->SetProjectionMode(p_projectionMode);
	MarkRenderStateChanged();
}

void CameraComponent::SetVisibleLayers(const NLS::Engine::LayerMask p_visibleLayers)
{
		visibleLayers = p_visibleLayers;
		if (m_camera)
			m_camera->SetVisibleLayerMask(p_visibleLayers.GetMask());
	MarkRenderStateChanged();
}

float CameraComponent::GetFov() const
{
		return fov;
}

float CameraComponent::GetSize() const
{
		return size;
}

float CameraComponent::GetNear() const
{
		return nearClip;
}

float CameraComponent::GetFar() const
{
		return farClip;
}

const Maths::Vector3 & CameraComponent::GetClearColor() const
{
		return clearColor;
}

bool CameraComponent::HasFrustumGeometryCulling() const
{
		return frustumGeometryCulling;
}

void CameraComponent::SetClearColor(const Maths::Vector3 & p_clearColor)
{
		clearColor = p_clearColor;
		if (m_camera)
			m_camera->SetClearColor(p_clearColor);
	MarkRenderStateChanged();
}

bool CameraComponent::HasFrustumLightCulling() const
{
		return frustumLightCulling;
}

Render::Settings::EProjectionMode CameraComponent::GetProjectionMode() const
{
		return projectionMode;
}

NLS::Engine::LayerMask CameraComponent::GetVisibleLayers() const
{
		return visibleLayers;
}

Render::Entities::Camera* CameraComponent::GetCamera()
{
	return m_camera;
}
} // namespace NLS::Engine::Components


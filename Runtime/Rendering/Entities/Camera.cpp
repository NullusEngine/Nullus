#include <cmath>

#include "Rendering/Entities/Camera.h"
#include "Math/Matrix4.h"

namespace NLS::Render::Entities
{
Camera::Camera() :
	m_projectionMode(Settings::EProjectionMode::PERSPECTIVE),
	m_fov(45.0f),
	m_size(5.0f),
	m_near(0.1f),
	m_far(100.f),
	m_clearColor(0.f, 0.f, 0.f),
	m_clearColorBuffer(true),
	m_clearDepthBuffer(true),
	m_clearStencilBuffer(true),
	m_frustumGeometryCulling(false),
	m_frustumLightCulling(false),
	m_frustum{}
{
}

Camera::Camera(Maths::Transform* p_transform) :
	Entity{ p_transform },
    m_projectionMode(Settings::EProjectionMode::PERSPECTIVE),
	m_fov(45.0f),
    m_size(5.0f),
	m_near(0.1f),
	m_far(100.f),
	m_clearColor(0.f, 0.f, 0.f),
	m_clearColorBuffer(true),
	m_clearDepthBuffer(true),
	m_clearStencilBuffer(true),
	m_frustumGeometryCulling(false),
	m_frustumLightCulling(false),
	m_frustum{}
{
}

void Camera::CacheMatrices(uint16_t p_windowWidth, uint16_t p_windowHeight)
{
	CacheProjectionMatrix(p_windowWidth, p_windowHeight);
	CacheViewMatrix();
	CacheFrustum(m_viewMatrix, m_projectionMatrix);
}

void Camera::CacheProjectionMatrix(uint16_t p_windowWidth, uint16_t p_windowHeight)
{
	m_projectionMatrix = CalculateProjectionMatrix(p_windowWidth, p_windowHeight);
}

void Camera::CacheViewMatrix()
{
	m_viewMatrix = CalculateViewMatrix();
}

void Camera::CacheFrustum(const Maths::Matrix4& p_view, const Maths::Matrix4& p_projection)
{
	m_frustum.CalculateFrustum(p_projection * p_view);
}

const Maths::Vector3& Camera::GetPosition() const
{
	if (transform)
		return transform->GetWorldPosition();

	static const Maths::Vector3 kZeroPosition {};
	return kZeroPosition;
}

const Maths::Quaternion& Camera::GetRotation() const
{
	if (transform)
		return transform->GetWorldRotation();

	static const Maths::Quaternion kIdentityRotation {};
	return kIdentityRotation;
}

float Camera::GetFov() const
{
	return m_fov;
}

float Camera::GetSize() const
{
    return m_size;
}

float Camera::GetNear() const
{
	return m_near;
}

float Camera::GetFar() const
{
	return m_far;
}

const Maths::Vector3 & Camera::GetClearColor() const
{
	return m_clearColor;
}

bool Camera::GetClearColorBuffer() const
{
	return m_clearColorBuffer;
}

bool Camera::GetClearDepthBuffer() const
{
	return m_clearDepthBuffer;
}

bool Camera::GetClearStencilBuffer() const
{
	return m_clearStencilBuffer;
}

const Maths::Matrix4& Camera::GetProjectionMatrix() const
{
	return m_projectionMatrix;
}

const Maths::Matrix4& Camera::GetViewMatrix() const
{
	return m_viewMatrix;
}

const Data::Frustum& Camera::GetFrustum() const
{
	return m_frustum;
}

const Data::Frustum* Camera::GetGeometryFrustum() const
{
	if (m_frustumGeometryCulling)
	{
		return &m_frustum;
	}

	return nullptr;
}

const Data::Frustum* Camera::GetLightFrustum() const
{
	if (m_frustumLightCulling)
	{
		return &m_frustum;
	}

	return nullptr;
}

bool Camera::HasFrustumGeometryCulling() const
{
	return m_frustumGeometryCulling;
}

bool Camera::HasFrustumLightCulling() const
{
	return m_frustumLightCulling;
}

Settings::EProjectionMode Camera::GetProjectionMode() const
{
    return m_projectionMode;
}

void Camera::SetPosition(const Maths::Vector3& p_position)
{
	if (transform)
	{
		transform->SetWorldPosition(p_position);
	}
}

void Camera::SetRotation(const Maths::Quaternion& p_rotation)
{
	if (transform)
	{
		transform->SetWorldRotation(p_rotation);
	}

}

void Camera::SetFov(float p_value)
{
	m_fov = p_value;
}

void Camera::SetSize(float p_value)
{
    m_size = p_value;
}

void Camera::SetNear(float p_value)
{
	m_near = p_value;
}

void Camera::SetFar(float p_value)
{
	m_far = p_value;
}

void Camera::SetClearColor(const Maths::Vector3 & p_clearColor)
{
	m_clearColor = p_clearColor;
}

void Camera::SetClearColorBuffer(bool p_value)
{
	m_clearColorBuffer = p_value;
}

void Camera::SetClearDepthBuffer(bool p_value)
{
	m_clearDepthBuffer = p_value;
}

void Camera::SetClearStencilBuffer(bool p_value)
{
	m_clearStencilBuffer = p_value;
}

void Camera::SetFrustumGeometryCulling(bool p_enable)
{
	m_frustumGeometryCulling = p_enable;
}

void Camera::SetFrustumLightCulling(bool p_enable)
{
	m_frustumLightCulling = p_enable;
}

void Camera::SetProjectionMode(Settings::EProjectionMode p_projectionMode)
{
    m_projectionMode = p_projectionMode;
}

Maths::Matrix4 Camera::CalculateProjectionMatrix(uint16_t p_windowWidth, uint16_t p_windowHeight) const
{
    using namespace Maths;
    using namespace Settings;

    const auto ratio = p_windowWidth / static_cast<float>(p_windowHeight);

    switch (m_projectionMode)
    {
    case EProjectionMode::ORTHOGRAPHIC:
        return Matrix4::CreateOrthographic(m_size, ratio, m_near, m_far);

    case EProjectionMode::PERSPECTIVE: 
        return Matrix4::CreatePerspective(m_fov, ratio, m_near, m_far);

    default:
        return Matrix4::Identity;
    }
}

Maths::Matrix4 Camera::CalculateViewMatrix() const
{
	if (transform)
	{
		const Maths::Vector3& position = transform->GetWorldPosition();
		const Maths::Quaternion& rotation = transform->GetWorldRotation();
		const Maths::Vector3& up = transform->GetWorldUp();
		const Maths::Vector3& forward = transform->GetWorldForward();

		return Maths::Matrix4::CreateView(
			position.x, position.y, position.z,
			position.x + forward.x, position.y + forward.y, position.z + forward.z,
			up.x, up.y, up.z
		);
	}
	else
	{
		return Maths::Matrix4::Identity;
	}
}
} // namespace NLS::Render::Entities

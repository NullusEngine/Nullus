#include <cmath>

#include "Rendering/Entities/Camera.h"
#include "Math/Matrix4.h"

NLS::Rendering::Entities::Camera::Camera() :
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

NLS::Rendering::Entities::Camera::Camera(Maths::Transform p_transform) :
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

void NLS::Rendering::Entities::Camera::CacheMatrices(uint16_t p_windowWidth, uint16_t p_windowHeight)
{
	CacheProjectionMatrix(p_windowWidth, p_windowHeight);
	CacheViewMatrix();
	CacheFrustum(m_viewMatrix, m_projectionMatrix);
}

void NLS::Rendering::Entities::Camera::CacheProjectionMatrix(uint16_t p_windowWidth, uint16_t p_windowHeight)
{
	m_projectionMatrix = CalculateProjectionMatrix(p_windowWidth, p_windowHeight);
}

void NLS::Rendering::Entities::Camera::CacheViewMatrix()
{
	m_viewMatrix = CalculateViewMatrix();
}

void NLS::Rendering::Entities::Camera::CacheFrustum(const Maths::Matrix4& p_view, const Maths::Matrix4& p_projection)
{
	m_frustum.CalculateFrustum(p_projection * p_view);
}

const Maths::Vector3& NLS::Rendering::Entities::Camera::GetPosition() const
{
	return transform.GetWorldPosition();
}

const Maths::Quaternion& NLS::Rendering::Entities::Camera::GetRotation() const
{
	return transform.GetWorldRotation();
}

float NLS::Rendering::Entities::Camera::GetFov() const
{
	return m_fov;
}

float NLS::Rendering::Entities::Camera::GetSize() const
{
    return m_size;
}

float NLS::Rendering::Entities::Camera::GetNear() const
{
	return m_near;
}

float NLS::Rendering::Entities::Camera::GetFar() const
{
	return m_far;
}

const Maths::Vector3 & NLS::Rendering::Entities::Camera::GetClearColor() const
{
	return m_clearColor;
}

bool NLS::Rendering::Entities::Camera::GetClearColorBuffer() const
{
	return m_clearColorBuffer;
}

bool NLS::Rendering::Entities::Camera::GetClearDepthBuffer() const
{
	return m_clearDepthBuffer;
}

bool NLS::Rendering::Entities::Camera::GetClearStencilBuffer() const
{
	return m_clearStencilBuffer;
}

const Maths::Matrix4& NLS::Rendering::Entities::Camera::GetProjectionMatrix() const
{
	return m_projectionMatrix;
}

const Maths::Matrix4& NLS::Rendering::Entities::Camera::GetViewMatrix() const
{
	return m_viewMatrix;
}

const NLS::Rendering::Data::Frustum& NLS::Rendering::Entities::Camera::GetFrustum() const
{
	return m_frustum;
}

const NLS::Rendering::Data::Frustum* NLS::Rendering::Entities::Camera::GetGeometryFrustum() const
{
	if (m_frustumGeometryCulling)
	{
		return &m_frustum;
	}

	return nullptr;
}

const NLS::Rendering::Data::Frustum* NLS::Rendering::Entities::Camera::GetLightFrustum() const
{
	if (m_frustumLightCulling)
	{
		return &m_frustum;
	}

	return nullptr;
}

bool NLS::Rendering::Entities::Camera::HasFrustumGeometryCulling() const
{
	return m_frustumGeometryCulling;
}

bool NLS::Rendering::Entities::Camera::HasFrustumLightCulling() const
{
	return m_frustumLightCulling;
}

NLS::Rendering::Settings::EProjectionMode NLS::Rendering::Entities::Camera::GetProjectionMode() const
{
    return m_projectionMode;
}

void NLS::Rendering::Entities::Camera::SetPosition(const Maths::Vector3& p_position)
{
	transform.SetWorldPosition(p_position);
}

void NLS::Rendering::Entities::Camera::SetRotation(const Maths::Quaternion& p_rotation)
{
	transform.SetWorldRotation(p_rotation);
}

void NLS::Rendering::Entities::Camera::SetFov(float p_value)
{
	m_fov = p_value;
}

void NLS::Rendering::Entities::Camera::SetSize(float p_value)
{
    m_size = p_value;
}

void NLS::Rendering::Entities::Camera::SetNear(float p_value)
{
	m_near = p_value;
}

void NLS::Rendering::Entities::Camera::SetFar(float p_value)
{
	m_far = p_value;
}

void NLS::Rendering::Entities::Camera::SetClearColor(const Maths::Vector3 & p_clearColor)
{
	m_clearColor = p_clearColor;
}

void NLS::Rendering::Entities::Camera::SetClearColorBuffer(bool p_value)
{
	m_clearColorBuffer = p_value;
}

void NLS::Rendering::Entities::Camera::SetClearDepthBuffer(bool p_value)
{
	m_clearDepthBuffer = p_value;
}

void NLS::Rendering::Entities::Camera::SetClearStencilBuffer(bool p_value)
{
	m_clearStencilBuffer = p_value;
}

void NLS::Rendering::Entities::Camera::SetFrustumGeometryCulling(bool p_enable)
{
	m_frustumGeometryCulling = p_enable;
}

void NLS::Rendering::Entities::Camera::SetFrustumLightCulling(bool p_enable)
{
	m_frustumLightCulling = p_enable;
}

void NLS::Rendering::Entities::Camera::SetProjectionMode(NLS::Rendering::Settings::EProjectionMode p_projectionMode)
{
    m_projectionMode = p_projectionMode;
}

Maths::Matrix4 NLS::Rendering::Entities::Camera::CalculateProjectionMatrix(uint16_t p_windowWidth, uint16_t p_windowHeight) const
{
    using namespace Maths;
    using namespace NLS::Rendering::Settings;

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

Maths::Matrix4 NLS::Rendering::Entities::Camera::CalculateViewMatrix() const
{
	const Maths::Vector3& position = transform.GetWorldPosition();
	const Maths::Quaternion& rotation = transform.GetWorldRotation();
	const Maths::Vector3& up = transform.GetWorldUp();
	const Maths::Vector3& forward = transform.GetWorldForward();

	return Maths::Matrix4::CreateView(
		position.x, position.y, position.z,
		position.x + forward.x, position.y + forward.y, position.z + forward.z,
		up.x, up.y, up.z
	);
}
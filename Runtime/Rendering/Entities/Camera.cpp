#include <cmath>

#include "Rendering/Entities/Camera.h"
#include "Math/Matrix4.h"

NLS::Render::Entities::Camera::Camera() :
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

NLS::Render::Entities::Camera::Camera(Maths::Transform* p_transform) :
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

void NLS::Render::Entities::Camera::CacheMatrices(uint16_t p_windowWidth, uint16_t p_windowHeight)
{
	CacheProjectionMatrix(p_windowWidth, p_windowHeight);
	CacheViewMatrix();
	CacheFrustum(m_viewMatrix, m_projectionMatrix);
}

void NLS::Render::Entities::Camera::CacheProjectionMatrix(uint16_t p_windowWidth, uint16_t p_windowHeight)
{
	m_projectionMatrix = CalculateProjectionMatrix(p_windowWidth, p_windowHeight);
}

void NLS::Render::Entities::Camera::CacheViewMatrix()
{
	m_viewMatrix = CalculateViewMatrix();
}

void NLS::Render::Entities::Camera::CacheFrustum(const Maths::Matrix4& p_view, const Maths::Matrix4& p_projection)
{
	m_frustum.CalculateFrustum(p_projection * p_view);
}

const Maths::Vector3& NLS::Render::Entities::Camera::GetPosition() const
{
	return transform->GetWorldPosition();
}

const Maths::Quaternion& NLS::Render::Entities::Camera::GetRotation() const
{
	return transform->GetWorldRotation();
}

float NLS::Render::Entities::Camera::GetFov() const
{
	return m_fov;
}

float NLS::Render::Entities::Camera::GetSize() const
{
    return m_size;
}

float NLS::Render::Entities::Camera::GetNear() const
{
	return m_near;
}

float NLS::Render::Entities::Camera::GetFar() const
{
	return m_far;
}

const Maths::Vector3 & NLS::Render::Entities::Camera::GetClearColor() const
{
	return m_clearColor;
}

bool NLS::Render::Entities::Camera::GetClearColorBuffer() const
{
	return m_clearColorBuffer;
}

bool NLS::Render::Entities::Camera::GetClearDepthBuffer() const
{
	return m_clearDepthBuffer;
}

bool NLS::Render::Entities::Camera::GetClearStencilBuffer() const
{
	return m_clearStencilBuffer;
}

const Maths::Matrix4& NLS::Render::Entities::Camera::GetProjectionMatrix() const
{
	return m_projectionMatrix;
}

const Maths::Matrix4& NLS::Render::Entities::Camera::GetViewMatrix() const
{
	return m_viewMatrix;
}

const NLS::Render::Data::Frustum& NLS::Render::Entities::Camera::GetFrustum() const
{
	return m_frustum;
}

const NLS::Render::Data::Frustum* NLS::Render::Entities::Camera::GetGeometryFrustum() const
{
	if (m_frustumGeometryCulling)
	{
		return &m_frustum;
	}

	return nullptr;
}

const NLS::Render::Data::Frustum* NLS::Render::Entities::Camera::GetLightFrustum() const
{
	if (m_frustumLightCulling)
	{
		return &m_frustum;
	}

	return nullptr;
}

bool NLS::Render::Entities::Camera::HasFrustumGeometryCulling() const
{
	return m_frustumGeometryCulling;
}

bool NLS::Render::Entities::Camera::HasFrustumLightCulling() const
{
	return m_frustumLightCulling;
}

NLS::Render::Settings::EProjectionMode NLS::Render::Entities::Camera::GetProjectionMode() const
{
    return m_projectionMode;
}

void NLS::Render::Entities::Camera::SetPosition(const Maths::Vector3& p_position)
{
	transform->SetWorldPosition(p_position);
}

void NLS::Render::Entities::Camera::SetRotation(const Maths::Quaternion& p_rotation)
{
	transform->SetWorldRotation(p_rotation);
}

void NLS::Render::Entities::Camera::SetFov(float p_value)
{
	m_fov = p_value;
}

void NLS::Render::Entities::Camera::SetSize(float p_value)
{
    m_size = p_value;
}

void NLS::Render::Entities::Camera::SetNear(float p_value)
{
	m_near = p_value;
}

void NLS::Render::Entities::Camera::SetFar(float p_value)
{
	m_far = p_value;
}

void NLS::Render::Entities::Camera::SetClearColor(const Maths::Vector3 & p_clearColor)
{
	m_clearColor = p_clearColor;
}

void NLS::Render::Entities::Camera::SetClearColorBuffer(bool p_value)
{
	m_clearColorBuffer = p_value;
}

void NLS::Render::Entities::Camera::SetClearDepthBuffer(bool p_value)
{
	m_clearDepthBuffer = p_value;
}

void NLS::Render::Entities::Camera::SetClearStencilBuffer(bool p_value)
{
	m_clearStencilBuffer = p_value;
}

void NLS::Render::Entities::Camera::SetFrustumGeometryCulling(bool p_enable)
{
	m_frustumGeometryCulling = p_enable;
}

void NLS::Render::Entities::Camera::SetFrustumLightCulling(bool p_enable)
{
	m_frustumLightCulling = p_enable;
}

void NLS::Render::Entities::Camera::SetProjectionMode(NLS::Render::Settings::EProjectionMode p_projectionMode)
{
    m_projectionMode = p_projectionMode;
}

Maths::Matrix4 NLS::Render::Entities::Camera::CalculateProjectionMatrix(uint16_t p_windowWidth, uint16_t p_windowHeight) const
{
    using namespace Maths;
    using namespace NLS::Render::Settings;

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

Maths::Matrix4 NLS::Render::Entities::Camera::CalculateViewMatrix() const
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
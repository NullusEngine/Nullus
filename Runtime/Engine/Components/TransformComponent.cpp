#include "Components/TransformComponent.h"

using namespace NLS::Engine::Components;
using namespace NLS;


NLS::Engine::Components::TransformComponent::TransformComponent()
{
}

void TransformComponent::SetParent(TransformComponent& p_parent)
{
	m_transform.SetParent(p_parent.GetTransform());
}

bool TransformComponent::RemoveParent()
{
	return m_transform.RemoveParent();
}

bool TransformComponent::HasParent() const
{
	return m_transform.HasParent();
}

void TransformComponent::SetLocalPosition(Maths::Vector3 p_newPosition)
{
	m_transform.SetLocalPosition(p_newPosition);
}

void TransformComponent::SetLocalRotation(Maths::Quaternion p_newRotation)
{
	m_transform.SetLocalRotation(p_newRotation);
}

void TransformComponent::SetLocalScale(Maths::Vector3 p_newScale)
{
	m_transform.SetLocalScale(p_newScale);
}

void TransformComponent::SetWorldPosition(Maths::Vector3 p_newPosition)
{
	m_transform.SetWorldPosition(p_newPosition);
}

void TransformComponent::SetWorldRotation(Maths::Quaternion p_newRotation)
{
	m_transform.SetWorldRotation(p_newRotation);
}

void TransformComponent::SetWorldScale(Maths::Vector3 p_newScale)
{
	m_transform.SetWorldScale(p_newScale);
}

void TransformComponent::TranslateLocal(const Maths::Vector3& p_translation)
{
	m_transform.TranslateLocal(p_translation);
}

void TransformComponent::RotateLocal(const Maths::Quaternion& p_rotation)
{
	m_transform.RotateLocal(p_rotation);
}

void TransformComponent::ScaleLocal(const Maths::Vector3& p_scale)
{
	m_transform.ScaleLocal(p_scale);
}

const Maths::Vector3& TransformComponent::GetLocalPosition() const
{
	return m_transform.GetLocalPosition();
}

const Maths::Quaternion& TransformComponent::GetLocalRotation() const
{
	return m_transform.GetLocalRotation();
}

const Maths::Vector3& TransformComponent::GetLocalScale() const
{
	return m_transform.GetLocalScale();
}

const Maths::Vector3& TransformComponent::GetWorldPosition() const
{
	return m_transform.GetWorldPosition();
}

const Maths::Quaternion& TransformComponent::GetWorldRotation() const
{
	return m_transform.GetWorldRotation();
}

const Maths::Vector3& TransformComponent::GetWorldScale() const
{
	return m_transform.GetWorldScale();
}

const Maths::Matrix4& TransformComponent::GetLocalMatrix() const
{
	return m_transform.GetLocalMatrix();
}

const Maths::Matrix4& TransformComponent::GetWorldMatrix() const
{
	return m_transform.GetWorldMatrix();
}

Maths::Transform& TransformComponent::GetTransform()
{
	return m_transform;
}

Maths::Vector3 TransformComponent::GetWorldForward() const
{
	return m_transform.GetWorldForward();
}

Maths::Vector3 TransformComponent::GetWorldUp() const
{
	return m_transform.GetWorldUp();
}

Maths::Vector3 TransformComponent::GetWorldRight() const
{
	return m_transform.GetWorldRight();
}

Maths::Vector3 TransformComponent::GetLocalForward() const
{
	return m_transform.GetLocalForward();
}

Maths::Vector3 TransformComponent::GetLocalUp() const
{
	return m_transform.GetLocalUp();
}

Maths::Vector3 TransformComponent::GetLocalRight() const
{
	return m_transform.GetLocalRight();
}

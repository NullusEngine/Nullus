#include "Math/Transform.h"
#include "UDRefl/ReflMngr.hpp"
namespace NLS
{
using namespace UDRefl;
void NLS::Maths::Transform::Bind()
{
    Mngr.RegisterType<Maths::Transform>();
    Mngr.AddField<&Maths::Transform::m_localPosition>("m_localPosition");
    Mngr.AddField<&Maths::Transform::m_localRotation>("m_localRotation");
    Mngr.AddField<&Maths::Transform::m_localScale>("m_localScale");
}
} // namespace NLS

namespace NLS
{
Maths::TransformNotifier::NotificationHandlerID Maths::TransformNotifier::AddNotificationHandler(NotificationHandler p_notificationHandler)
{
    NotificationHandlerID handlerID = m_availableHandlerID++;
    m_notificationHandlers.emplace(handlerID, p_notificationHandler);
    return handlerID;
}

void Maths::TransformNotifier::NotifyChildren(ENotification p_notification)
{
    if (!m_notificationHandlers.empty())
        for (auto const& [id, handler] : m_notificationHandlers)
            handler(p_notification);
}

bool Maths::TransformNotifier::RemoveNotificationHandler(const NotificationHandlerID& p_notificationHandlerID)
{
    return m_notificationHandlers.erase(p_notificationHandlerID) != 0;
}

Maths::Transform::Transform(Vector3 p_localPosition, Quaternion p_localRotation, Vector3 p_localScale)
    : m_notificationHandlerID(-1), m_parent(nullptr)
{
    GenerateMatricesLocal(p_localPosition, p_localRotation, p_localScale);
}

Maths::Transform::~Transform()
{
    Notifier.NotifyChildren(TransformNotifier::ENotification::TRANSFORM_DESTROYED);
}

void Maths::Transform::NotificationHandler(TransformNotifier::ENotification p_notification)
{
    switch (p_notification)
    {
        case TransformNotifier::ENotification::TRANSFORM_CHANGED:
            UpdateWorldMatrix();
            break;

        case TransformNotifier::ENotification::TRANSFORM_DESTROYED:
            /*
             * RemoveParent() is not called here because it is unsafe to remove a notification handler
             * while the parent is iterating on his notification handlers (Segfault otherwise)
             */
            GenerateMatricesLocal(m_worldPosition, m_worldRotation, m_worldScale);
            m_parent = nullptr;
            UpdateWorldMatrix();
            break;
    }
}

void Maths::Transform::SetParent(Transform& p_parent)
{
    m_parent = &p_parent;

    m_notificationHandlerID = m_parent->Notifier.AddNotificationHandler(std::bind(&Transform::NotificationHandler, this, std::placeholders::_1));

    UpdateWorldMatrix();
}

bool Maths::Transform::RemoveParent()
{
    if (m_parent != nullptr)
    {
        m_parent->Notifier.RemoveNotificationHandler(m_notificationHandlerID);
        m_parent = nullptr;
        UpdateWorldMatrix();

        return true;
    }

    return false;
}

bool Maths::Transform::HasParent() const
{
    return m_parent != nullptr;
}

void Maths::Transform::GenerateMatricesLocal(Vector3 p_position, Quaternion p_rotation, Vector3 p_scale)
{
    m_localMatrix = Matrix4::Translation(p_position) * Quaternion::ToMatrix4(Quaternion::Normalize(p_rotation)) * Matrix4::Scaling(p_scale);
    m_localPosition = p_position;
    m_localRotation = p_rotation;
    m_localScale = p_scale;

    UpdateWorldMatrix();
}

void Maths::Transform::GenerateMatricesWorld(Vector3 p_position, Quaternion p_rotation, Vector3 p_scale)
{
    m_worldMatrix = Matrix4::Translation(p_position) * Quaternion::ToMatrix4(Quaternion::Normalize(p_rotation)) * Matrix4::Scaling(p_scale);
    m_worldPosition = p_position;
    m_worldRotation = p_rotation;
    m_worldScale = p_scale;

    UpdateLocalMatrix();
}

void Maths::Transform::UpdateWorldMatrix()
{
    m_worldMatrix = HasParent() ? m_parent->m_worldMatrix * m_localMatrix : m_localMatrix;
    PreDecomposeWorldMatrix();

    Notifier.NotifyChildren(TransformNotifier::ENotification::TRANSFORM_CHANGED);
}

void Maths::Transform::UpdateLocalMatrix()
{
    m_localMatrix = HasParent() ? Matrix4::Inverse(m_parent->m_worldMatrix) * m_worldMatrix : m_worldMatrix;
    PreDecomposeLocalMatrix();

    Notifier.NotifyChildren(TransformNotifier::ENotification::TRANSFORM_CHANGED);
}

void Maths::Transform::SetLocalPosition(Vector3 p_newPosition)
{
    GenerateMatricesLocal(p_newPosition, m_localRotation, m_localScale);
}

void Maths::Transform::SetLocalRotation(Quaternion p_newRotation)
{
    GenerateMatricesLocal(m_localPosition, p_newRotation, m_localScale);
}

void Maths::Transform::SetLocalScale(Vector3 p_newScale)
{
    GenerateMatricesLocal(m_localPosition, m_localRotation, p_newScale);
}

void Maths::Transform::SetWorldPosition(Vector3 p_newPosition)
{
    GenerateMatricesWorld(p_newPosition, m_worldRotation, m_worldScale);
}

void Maths::Transform::SetWorldRotation(Quaternion p_newRotation)
{
    GenerateMatricesWorld(m_worldPosition, p_newRotation, m_worldScale);
}

void Maths::Transform::SetWorldScale(Vector3 p_newScale)
{
    GenerateMatricesWorld(m_worldPosition, m_worldRotation, p_newScale);
}

void Maths::Transform::TranslateLocal(const Vector3& p_translation)
{
    SetLocalPosition(m_localPosition + p_translation);
}

void Maths::Transform::RotateLocal(const Quaternion& p_rotation)
{
    SetLocalRotation(m_localRotation * p_rotation);
}

void Maths::Transform::ScaleLocal(const Vector3& p_scale)
{
    SetLocalScale(Vector3(
        m_localScale.x * p_scale.x,
        m_localScale.y * p_scale.y,
        m_localScale.z * p_scale.z));
}

const Maths::Vector3& Maths::Transform::GetLocalPosition() const
{
    return m_localPosition;
}

const Maths::Quaternion& Maths::Transform::GetLocalRotation() const
{
    return m_localRotation;
}

const Maths::Vector3& Maths::Transform::GetLocalScale() const
{
    return m_localScale;
}

const Maths::Vector3& Maths::Transform::GetWorldPosition() const
{
    return m_worldPosition;
}

const Maths::Quaternion& Maths::Transform::GetWorldRotation() const
{
    return m_worldRotation;
}

const Maths::Vector3& Maths::Transform::GetWorldScale() const
{
    return m_worldScale;
}

const Maths::Matrix4& Maths::Transform::GetLocalMatrix() const
{
    return m_localMatrix;
}

const Maths::Matrix4& Maths::Transform::GetWorldMatrix() const
{
    return m_worldMatrix;
}

Maths::Vector3 Maths::Transform::GetWorldForward() const
{
    return m_worldRotation * Vector3::Forward;
}

Maths::Vector3 Maths::Transform::GetWorldUp() const
{
    return m_worldRotation * Vector3::Up;
}

Maths::Vector3 Maths::Transform::GetWorldRight() const
{
    return m_worldRotation * Vector3::Right;
}

Maths::Vector3 Maths::Transform::GetLocalForward() const
{
    return m_localRotation * Vector3::Forward;
}

Maths::Vector3 Maths::Transform::GetLocalUp() const
{
    return m_localRotation * Vector3::Up;
}

Maths::Vector3 Maths::Transform::GetLocalRight() const
{
    return m_localRotation * Vector3::Right;
}

void Maths::Transform::PreDecomposeWorldMatrix()
{
    m_worldPosition.x = m_worldMatrix(0, 3);
    m_worldPosition.y = m_worldMatrix(1, 3);
    m_worldPosition.z = m_worldMatrix(2, 3);

    Vector3 columns[3] = {
        {m_worldMatrix(0, 0), m_worldMatrix(1, 0), m_worldMatrix(2, 0)},
        {m_worldMatrix(0, 1), m_worldMatrix(1, 1), m_worldMatrix(2, 1)},
        {m_worldMatrix(0, 2), m_worldMatrix(1, 2), m_worldMatrix(2, 2)},
    };

    m_worldScale.x = Vector3::Length(columns[0]);
    m_worldScale.y = Vector3::Length(columns[1]);
    m_worldScale.z = Vector3::Length(columns[2]);

    if (m_worldScale.x)
    {
        columns[0] /= m_worldScale.x;
    }
    if (m_worldScale.y)
    {
        columns[1] /= m_worldScale.y;
    }
    if (m_worldScale.z)
    {
        columns[2] /= m_worldScale.z;
    }

    Matrix3 rotationMatrix(
        columns[0].x, columns[1].x, columns[2].x, columns[0].y, columns[1].y, columns[2].y, columns[0].z, columns[1].z, columns[2].z);
    m_worldRotation = Quaternion(rotationMatrix);
}

void Maths::Transform::PreDecomposeLocalMatrix()
{
    m_localPosition.x = m_localMatrix(0, 3);
    m_localPosition.y = m_localMatrix(1, 3);
    m_localPosition.z = m_localMatrix(2, 3);

    Vector3 columns[3] = {
        {m_localMatrix(0, 0), m_localMatrix(1, 0), m_localMatrix(2, 0)},
        {m_localMatrix(0, 1), m_localMatrix(1, 1), m_localMatrix(2, 1)},
        {m_localMatrix(0, 2), m_localMatrix(1, 2), m_localMatrix(2, 2)},
    };

    m_localScale.x = Vector3::Length(columns[0]);
    m_localScale.y = Vector3::Length(columns[1]);
    m_localScale.z = Vector3::Length(columns[2]);

    if (m_localScale.x)
    {
        columns[0] /= m_localScale.x;
    }
    if (m_localScale.y)
    {
        columns[1] /= m_localScale.y;
    }
    if (m_localScale.z)
    {
        columns[2] /= m_localScale.z;
    }
    float elements[9] = {
        columns[0].x, columns[1].x, columns[2].x, columns[0].y, columns[1].y, columns[2].y, columns[0].z, columns[1].z, columns[2].z};
    Matrix3 rotationMatrix(
        columns[0].x, columns[1].x, columns[2].x, columns[0].y, columns[1].y, columns[2].y, columns[0].z, columns[1].z, columns[2].z);

    m_localRotation = Quaternion(rotationMatrix);
}
} // namespace NLS

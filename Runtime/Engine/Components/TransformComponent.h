#pragma once
#include "Matrix4.h"
#include "Matrix3.h"
#include "Vector3.h"
#include "Quaternion.h"
#include "EngineDef.h"
#include <vector>
#include "Components/Component.h"
#include <Math/Transform.h>

using std::vector;

using namespace NLS::Maths;

namespace NLS
{
namespace Engine
{
namespace Components
{
class NLS_ENGINE_API TransformComponent : public Component
{
public:
    static void Bind();

public:
    TransformComponent();

    void OnCreate();
    /**
     * Defines a parent to the transform
     * @param p_parent
     */
    void SetParent(TransformComponent& p_parent);

    /**
     * Set the parent to nullptr and recalculate world matrix
     * Returns true on success
     */
    bool RemoveParent();

    /**
     * Check if the transform has a parent
     */
    bool HasParent() const;

    /**
     * Set the position of the transform in the local space
     * @param p_newPosition
     */
    void SetLocalPosition(struct Maths::Vector3 p_newPosition);

    /**
     * Set the rotation of the transform in the local space
     * @param p_newRotation
     */
    void SetLocalRotation(Maths::Quaternion p_newRotation);

    /**
     * Set the scale of the transform in the local space
     * @param p_newScale
     */
    void SetLocalScale(struct Maths::Vector3 p_newScale);


    /**
     * Set the position of the transform in world space
     * @param p_newPosition
     */
    void SetWorldPosition(struct Maths::Vector3 p_newPosition);

    /**
     * Set the rotation of the transform in world space
     * @param p_newRotation
     */
    void SetWorldRotation(Maths::Quaternion p_newRotation);

    /**
     * Set the scale of the transform in world space
     * @param p_newScale
     */
    void SetWorldScale(struct Maths::Vector3 p_newScale);

    /**
     * Translate in the local space
     * @param p_translation
     */
    void TranslateLocal(const struct Maths::Vector3& p_translation);

    /**
     * Rotate in the local space
     * @param p_rotation
     */
    void RotateLocal(const Maths::Quaternion& p_rotation);

    /**
     * Scale in the local space
     * @param p_scale
     */
    void ScaleLocal(const struct Maths::Vector3& p_scale);

    /**
     * Return the position in local space
     */
    const Maths::Vector3& GetLocalPosition() const;

    /**
     * Return the rotation in local space
     */
    const Maths::Quaternion& GetLocalRotation() const;

    /**
     * Return the scale in local space
     */
    const Maths::Vector3& GetLocalScale() const;

    /**
     * Return the position in world space
     */
    const Maths::Vector3& GetWorldPosition() const;

    /**
     * Return the rotation in world space
     */
    const Maths::Quaternion& GetWorldRotation() const;

    /**
     * Return the scale in world space
     */
    const Maths::Vector3& GetWorldScale() const;

    /**
     * Return the local matrix
     */
    const Maths::Matrix4& GetLocalMatrix() const;

    /**
     * Return the world matrix
     */
    const Maths::Matrix4& GetWorldMatrix() const;

    /**
     * Return the FTransform attached to the CTransform
     */
    Maths::Transform& GetTransform();

    /**
     * Return the transform world forward
     */
    Maths::Vector3 GetWorldForward() const;

    /**
     * Return the transform world up
     */
    Maths::Vector3 GetWorldUp() const;

    /**
     * Return the transform world right
     */
    Maths::Vector3 GetWorldRight() const;

    /**
     * Return the transform local forward
     */
    Maths::Vector3 GetLocalForward() const;

    /**
     * Return the transform local up
     */
    Maths::Vector3 GetLocalUp() const;

    /**
     * Return the transform local right
     */
    Maths::Vector3 GetLocalRight() const;


private:
    Maths::Transform m_transform;
};
} // namespace Components

} // namespace Engine
} // namespace NLS

#pragma once

#include <Math/Matrix4.h>
#include <Math/Quaternion.h>
#include <Math/Vector3.h>

namespace NLS::Engine::SceneSystem
{
// Copied into immutable render proxies. Never serialized or applied to Transform.
struct PhysicsPresentationTransform
{
    Maths::Matrix4 worldMatrix = Maths::Matrix4::Identity;
    Maths::Vector3 worldPosition, worldScale{1, 1, 1};
    Maths::Quaternion worldRotation = Maths::Quaternion::Identity;
    bool usesPhysicsPose = false;
};
}

#include "AssemblyMath.h"
#include "Transform.h"
#include "Vector2.h"
#include "Vector3.h"
#include "Vector4.h"
#include "Color.h"
#include "Quaternion.h"
#include "Matrix3.h"
#include "Matrix4.h"
namespace NLS
{
void AssemblyMath::Initialize()
{
    Maths::Transform::Bind();
    Maths::Vector2::Bind();
    Maths::Vector3::Bind();
    Maths::Vector4::Bind();
    Maths::Color::Bind();
    Maths::Quaternion::Bind();
    Maths::Matrix3::Bind();
    Maths::Matrix4::Bind();
}
}
#include "Vector4.h"
#include "Vector3.h"
#include "Vector2.h"
using namespace NLS;
using namespace Maths;

const Vector4 Vector4::One(1.0f, 1.0f, 1.0f, 1.0f);
const Vector4 Vector4::Zero(0.0f, 0.0f, 0.0f, 0.0f);

Vector4::Vector4(const Vector3& v3, float newW)
    : x(v3.x), y(v3.y), z(v3.z), w(newW)
{
}

Vector4::Vector4(const Vector2& v2, float newZ, float newW)
    : x(v2.x), y(v2.y), z(newZ), w(newW)
{
}
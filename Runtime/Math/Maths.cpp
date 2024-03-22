/*
Part of Newcastle University's Game Engineering source code.

Use as you see fit!

Comments and queries to: richard-gordon.davison AT ncl.ac.uk
https://research.ncl.ac.uk/game/
*/
#include "Maths.h"
#include "Vector2.h"
#include "Vector3.h"

namespace NLS
{
namespace Maths
{
    // predefined specific
    const float PI = (float)(3.14159265358979323846264338327950288419716939937511);
    const float PI_2 = Maths::PI * 2.0f;
    const float PI_DIV2 = Maths::PI / 2.0f;
    const float PI_DIV4 = Maths::PI / 4.0f;
    const float PI_DIV180 = Maths::PI / 180.0f;
    const float PI_OVER_360 = PI / 360.0f;
    const float INV_PI = 1.0f / Maths::PI;
    const float EPSILON = std::numeric_limits<float>::epsilon();
    const float LOWEPSILON = (float)(1e-04);

    static_assert(std::numeric_limits<float>::is_iec559, "IEEE 754 is needed for NEG_INFINITY.");
    const float POS_INFINITY = std::numeric_limits<float>::infinity();
    const float NEG_INFINITY = -std::numeric_limits<float>::infinity();

    const float LN2 = std::log(2.0f);
    const float LN10 = std::log(10.0f);
    const float INV_LN2 = 1.0f / LN2;
    const float INV_LN10 = 1.0f / LN10;
    const float DEG2RAD = PI_DIV180;
    const float RAD2DEG = 180.0f / Maths::PI;
    const float SMALL_NUMBER = (1.e-8f);
    const float KINDA_SMALL_NUMBER = (1.e-4f);
    const float QUAN_SMALL_NUMBER = (1.e-3f);

    const float SQRT_2(1.4142135623730950488016887242097f);
    const float SQRT_3(1.7320508075688772935274463415059f);
    const float INV_SQRT_2(0.70710678118654752440084436210485f);
    const float INV_SQRT_3(0.57735026918962576450914878050196f);
    const float HALF_SQRT_2(0.70710678118654752440084436210485f);
    const float HALF_SQRT_3(0.86602540378443864676372317075294f);

    const uint16_t MIN_HALFFLOAT = 0xfbff; // s: 1 e: 1 1110  m: 11 1111 1111
    const uint16_t MAX_HALFFLOAT = 0x7bff; // s: 0 e: 1 1110  m: 11 1111 1111
    // lowest()得到的是类型可表示的有限值中的最小值，即一般理解下的min
    // min()在绝大部分时候得到的结果与lowest()相同
    // 但对于float、double等类型，min()实际上返回的是类型可表示的最小正值，此时与lowest()不同，也与一般理解下的min不同
    const float MIN_FLOAT = (std::numeric_limits<float>::lowest)();
    const float MAX_FLOAT = (std::numeric_limits<float>::max)();
    const double MIN_DOUBLE = (std::numeric_limits<double>::lowest)();
    const double MAX_DOUBLE = (std::numeric_limits<double>::max)();

    const unsigned char MAX_BYTE = (std::numeric_limits<unsigned char>::max)();
    const short MIN_SHORT = (std::numeric_limits<short>::lowest)();
    const short MAX_SHORT = (std::numeric_limits<short>::max)();
    const int MIN_INT = (std::numeric_limits<int>::lowest)();
    const int MAX_INT = (std::numeric_limits<int>::max)();
    const long MIN_LONG = (std::numeric_limits<long>::lowest)();
    const long MAX_LONG = (std::numeric_limits<long>::max)();
    const unsigned short MAX_WORD = (std::numeric_limits<unsigned short>::max)();
    const unsigned int MAX_DWORD = (std::numeric_limits<unsigned int>::max)();
    const int8_t MIN_I8 = (std::numeric_limits<int8_t>::lowest)();
    const int8_t MAX_I8 = (std::numeric_limits<int8_t>::max)();
    const uint8_t MAX_UI8 = (std::numeric_limits<uint8_t>::max)();
    const int16_t MIN_I16 = (std::numeric_limits<int16_t>::lowest)();
    const int16_t MAX_I16 = (std::numeric_limits<int16_t>::max)();
    const uint16_t MAX_UI16 = (std::numeric_limits<uint16_t>::max)();
    const int32_t MIN_I32 = (std::numeric_limits<int32_t>::lowest)();
    const int32_t MAX_I32 = (std::numeric_limits<int32_t>::max)();
    const uint32_t MAX_UI32 = (std::numeric_limits<uint32_t>::max)();
    const int64_t MIN_I64 = (std::numeric_limits<int64_t>::lowest)();
    const int64_t MAX_I64 = (std::numeric_limits<int64_t>::max)();
    const uint64_t MAX_UI64 = (std::numeric_limits<uint64_t>::max)();
void ScreenBoxOfTri(const Vector3& v0, const Vector3& v1, const Vector3& v2, Vector2& topLeft, Vector2& bottomRight)
{
    topLeft.x = std::min(v0.x, std::min(v1.x, v2.x));
    topLeft.y = std::min(v0.y, std::min(v1.y, v2.y));

    bottomRight.x = std::max(v0.x, std::max(v1.x, v2.x));
    bottomRight.y = std::max(v0.y, std::max(v1.y, v2.y));
}

int ScreenAreaOfTri(const Vector3& a, const Vector3& b, const Vector3& c)
{
    int area = (int)(((a.x * b.y) + (b.x * c.y) + (c.x * a.y)) - ((b.x * a.y) + (c.x * b.y) + (a.x * c.y)));
    return (area >> 1);
}

float FloatAreaOfTri(const Vector3& a, const Vector3& b, const Vector3& c)
{
    float area = ((a.x * b.y) + (b.x * c.y) + (c.x * a.y)) - ((b.x * a.y) + (c.x * b.y) + (a.x * c.y));
    return (area * 0.5f);
}

float CrossAreaOfTri(const Vector3& a, const Vector3& b, const Vector3& c)
{
    Vector3 area = Vector3::Cross(a - b, a - c);
    return area.Length() * 0.5f;
}


Vector3 Clamp(const Vector3& a, const Vector3& mins, const Vector3& maxs)
{
    return Vector3(
        Clamp(a.x, mins.x, maxs.x),
        Clamp(a.y, mins.y, maxs.y),
        Clamp(a.z, mins.z, maxs.z));
}
} // namespace Maths
} // namespace NLS
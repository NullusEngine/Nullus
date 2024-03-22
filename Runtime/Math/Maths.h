#pragma once
#include <algorithm>
#include <cmath>
#include "MathDef.h"
namespace NLS
{
namespace Maths
{
class Vector2;
class Vector3;

extern NLS_MATH_API const float PI;        //!< 3.14159265358979323846264338327950288419716939937511
extern NLS_MATH_API const float PI_2;      //!< Math::PI * 2.0
extern NLS_MATH_API const float PI_DIV2;   //!< Math::PI / 2.0
extern NLS_MATH_API const float PI_DIV4;   //!< Math::PI / 4.0
extern NLS_MATH_API const float PI_DIV180; //!< Math::PI / 180.0
extern NLS_MATH_API const float PI_OVER_360;
extern NLS_MATH_API const float INV_PI;    //!< 0.31830988618379067153776752674502872406891929148091
extern NLS_MATH_API const float EPSILON;      //!< FLT: 1.1920929e-007; DBL: 2.2204460492503131e-016
extern NLS_MATH_API const float LOWEPSILON;   //!< 1e-04
extern NLS_MATH_API const float POS_INFINITY; //!< infinity
extern NLS_MATH_API const float NEG_INFINITY; //!< -infinity
extern NLS_MATH_API const float DEG2RAD;            //!< 0.01745329
extern NLS_MATH_API const float RAD2DEG;            //!< 57.29577
extern NLS_MATH_API const float SMALL_NUMBER;       //!< 1.e-8f
extern NLS_MATH_API const float KINDA_SMALL_NUMBER; //!< 1.e-4f
extern NLS_MATH_API const float QUAN_SMALL_NUMBER;  //!< 1.e-3f，受计算精度限制，四元数建议使用这个
extern NLS_MATH_API const uint16_t MIN_HALFFLOAT; //!<
extern NLS_MATH_API const uint16_t MAX_HALFFLOAT; //!< 65504.0
extern NLS_MATH_API const float MIN_FLOAT;      //!<
extern NLS_MATH_API const float MAX_FLOAT;      //!< 3.402823466e+38F
extern NLS_MATH_API const double MIN_DOUBLE;    //!<
extern NLS_MATH_API const double MAX_DOUBLE;    //!< 1.7976931348623158e+308
extern NLS_MATH_API const float SQRT_2;
extern NLS_MATH_API const float SQRT_3;
extern NLS_MATH_API const float INV_SQRT_2;
extern NLS_MATH_API const float INV_SQRT_3;
extern NLS_MATH_API const float HALF_SQRT_2;
extern NLS_MATH_API const float HALF_SQRT_3;
extern NLS_MATH_API const unsigned char MAX_BYTE;   //!< 0xff
extern NLS_MATH_API const short MIN_SHORT; //!< -32768
extern NLS_MATH_API const short MAX_SHORT; //!< 32767
extern NLS_MATH_API const int MIN_INT;     //!< -2147483648
extern NLS_MATH_API const int MAX_INT;     //!< 2147483647
extern NLS_MATH_API const long MIN_LONG;   //!< -2147483648L
extern NLS_MATH_API const long MAX_LONG;   //!< 2147483647L
extern NLS_MATH_API const unsigned short MAX_WORD;   //!< 0xffff
extern NLS_MATH_API const unsigned int MAX_DWORD; //!< 0xffffffff
extern NLS_MATH_API const int8_t MIN_I8;     //!< -128
extern NLS_MATH_API const int8_t MAX_I8;     //!< 127
extern NLS_MATH_API const uint8_t MAX_UI8;   //!< 0xff
extern NLS_MATH_API const int16_t MIN_I16;   //!< -32768
extern NLS_MATH_API const int16_t MAX_I16;   //!< 32767
extern NLS_MATH_API const uint16_t MAX_UI16; //!< 0xffff
extern NLS_MATH_API const int32_t MIN_I32;   //!< -2147483648
extern NLS_MATH_API const int32_t MAX_I32;   //!< 2147483647
extern NLS_MATH_API const uint32_t MAX_UI32; //!< 0xffffffff
extern NLS_MATH_API const int64_t MIN_I64;   //!< -9223372036854775808
extern NLS_MATH_API const int64_t MAX_I64;   //!< 9223372036854775807
extern NLS_MATH_API const uint64_t MAX_UI64; //!< 0xffffffffffffffff
// Radians to degrees
inline float RadiansToDegrees(float rads)
{
    return rads * 180.0f / PI;
};
/// @brief 返回给定浮点数的绝对值
/// @tparam T 值的类型
/// @param x 给定的值
/// @return 绝对值
template<typename T>
FORCEINLINE T Abs(const T x)
{
    return (x < (T)0) ? -x : x;
}
/// @brief 返回给定浮点数的绝对值
/// @param x 给定的浮点数
/// @return 绝对值
/// @ingroup LumMath
template<>
FORCEINLINE float Abs<float>(const float x)
{
    return std::abs(x);
}
/// @brief 返回给定双精度浮点数的绝对值
/// @param x 给定的双精度浮点数
/// @return 绝对值
template<>
FORCEINLINE double Abs<double>(const double x)
{
    return std::abs(x);
}
// Degrees to radians
inline float DegreesToRadians(float degs)
{
    return degs * PI / 180.0f;
};

template<class T>
inline T Clamp(T value, T min, T max)
{
    if (value < min)
    {
        return min;
    }
    if (value > max)
    {
        return max;
    }
    return value;
}
/// @brief 返回两个值中较小的一个
/// @tparam T 值的类型
/// @param a 第一个值
/// @param b 第二个值
/// @return 较小的值
// 返回值是一个引用，但尽量不要使用引用来接受返回值。
// 在 const int& ret = Clamp(x, 0, 10); 此类的代码，ret可能会指向一个无效的临时变量。
template<typename T>
constexpr FORCEINLINE const T& Min(const T& a, const T& b)
{
    return (std::min)(a, b);
}

/// @brief 返回两个值中较大的一个
/// @tparam T 值的类型
/// @param a 第一个值
/// @param b 第二个值
/// @return 较大的值
// 返回值是一个引用，但尽量不要使用引用来接受返回值。
//  在 const int& ret = Max(x, 0); 此类的代码，ret可能会指向一个无效的临时变量。
template<typename T>
constexpr FORCEINLINE const T& Max(const T& a, const T& b)
{
    return (std::max)(a, b);
}
/// @brief 判断给定值的正负
/// @tparam T 值的类型
/// @param x 给定的值
/// @return 大于0返回1，小于0返回-1，等于0返回0
template<typename T>
constexpr FORCEINLINE T Sign(const T x)
{
    return (x < (T)0) ? (T)-1 : ((x > (T)0) ? (T)1 : (T)0);
}

/// @brief 返回给定值的平方
/// @tparam T 值的类型
/// @param x 给定的值
/// @return 平方值
template<typename T>
constexpr FORCEINLINE T Square(const T x)
{
    return x * x;
}

/// @brief 返回给定浮点数的平方根
/// @param x 给定的浮点数
/// @return 平方根
/// @ingroup LumMath
FORCEINLINE float Sqrt(float x)
{
    return std::sqrt(x);
}

/// @brief 返回e的给定次方
/// @param x 指数
/// @return e的x次方
/// @ingroup LumMath
FORCEINLINE float Exp(float x)
{
    return std::exp(x);
}

/// @brief 返回给定底数的给定指数次方
/// @param base 底数
/// @param exponent 指数
/// @return base的exponent次方
/// @ingroup LumMath
FORCEINLINE float Pow(float base, float exponent)
{
    return std::pow(base, exponent);
}
/// @brief 返回给定浮点数的以2为底的对数
/// @param x 给定的浮点数
/// @return 以2为底的对数
/// @ingroup LumMath
FORCEINLINE float Log2(float x)
{
    return std::log2(x);
}
/// @brief 返回给定值的以2为底的对数
/// @tparam T 值的类型
/// @param x 给定的值
/// @return 以2为底的对数
template<typename T>
FORCEINLINE T Log2Ex(const T& x)
{
    return std::log2(x);
}
/// @brief 判断给定值是否为2的幂次方
/// @tparam T 值的类型
/// @param num 给定的值
/// @return 若是2的幂次方返回true，否则返回false
template<typename T>
FORCEINLINE bool IsPO2(const T& num)
{
    return (num & (num - 1)) == 0;
}
/// @brief 返回给定32位无符号整数的向下取整的以2为底的对数
/// @param Value 给定的32位无符号整数
/// @return 向下取整的以2为底的对数
/// @ingroup LumMath
[[api, FunctionNode]] FORCEINLINE uint32_t FloorLog2(uint32_t Value)
{
    uint32_t pos = 0;
    if (Value >= 1 << 16)
    {
        Value >>= 16;
        pos += 16;
    }
    if (Value >= 1 << 8)
    {
        Value >>= 8;
        pos += 8;
    }
    if (Value >= 1 << 4)
    {
        Value >>= 4;
        pos += 4;
    }
    if (Value >= 1 << 2)
    {
        Value >>= 2;
        pos += 2;
    }
    if (Value >= 1 << 1)
    {
        pos += 1;
    }
    return (Value == 0) ? 0 : pos;
}
/// @brief 返回给定64位无符号整数的向下取整的以2为底的对数
/// @param Value 给定的64位无符号整数
/// @return 向下取整的以2为底的对数
/// @ingroup LumMath
FORCEINLINE uint64_t FloorLog2_64(uint64_t Value)
{
    uint64_t pos = 0;
    if (Value >= 1ull << 32)
    {
        Value >>= 32;
        pos += 32;
    }
    if (Value >= 1ull << 16)
    {
        Value >>= 16;
        pos += 16;
    }
    if (Value >= 1ull << 8)
    {
        Value >>= 8;
        pos += 8;
    }
    if (Value >= 1ull << 4)
    {
        Value >>= 4;
        pos += 4;
    }
    if (Value >= 1ull << 2)
    {
        Value >>= 2;
        pos += 2;
    }
    if (Value >= 1ull << 1)
    {
        pos += 1;
    }
    return (Value == 0) ? 0 : pos;
}

NLS_MATH_API Vector3 Clamp(const Vector3& a, const Vector3& mins, const Vector3& maxs);

template<class T>
inline T Lerp(const T& a, const T& b, float by)
{
    return (a * (1.0f - by) + b * by);
}

NLS_MATH_API void ScreenBoxOfTri(const Vector3& v0, const Vector3& v1, const Vector3& v2, Vector2& topLeft, Vector2& bottomRight);

NLS_MATH_API int ScreenAreaOfTri(const Vector3& a, const Vector3& b, const Vector3& c);
NLS_MATH_API float FloatAreaOfTri(const Vector3& a, const Vector3& b, const Vector3& c);

NLS_MATH_API float CrossAreaOfTri(const Vector3& a, const Vector3& b, const Vector3& c);
} // namespace Maths
} // namespace NLS
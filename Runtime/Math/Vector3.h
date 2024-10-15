#pragma once
#include "Maths.h"
#include <iostream>
#include <algorithm>
#include "MathDef.h"
namespace NLS
{
namespace Maths
{
class Vector2;
class Vector4;

class NLS_MATH_API Vector3
{
public:
    static void Bind();

public:
    static const Vector3 One;
    static const Vector3 Zero;
    static const Vector3 Forward;
    static const Vector3 Right;
    static const Vector3 Up;
    union
    {
        struct
        {
            float x;
            float y;
            float z;
        };
        float array[3];
    };

public:
    constexpr Vector3(float xVal = 0.0f, float yVal = 0.0f, float zVal = 0.0f)
        : x(xVal), y(yVal), z(zVal) {}

    Vector3(const Vector2& v2, float z = 0.0f);
    Vector3(const Vector4& v4);

    ~Vector3(void) {}

    Vector3 Normalised() const
    {
        Vector3 temp(x, y, z);
        temp.Normalise();
        return temp;
    }

    void Normalise()
    {
        float length = Length();

        if (length != 0.0f)
        {
            length = 1.0f / length;
            x = x * length;
            y = y * length;
            z = z * length;
        }
    }

    float Length() const
    {
        return Sqrt((x * x) + (y * y) + (z * z));
    }

    constexpr float LengthSquared() const
    {
        return ((x * x) + (y * y) + (z * z));
    }

    constexpr float GetMaxElement() const
    {
        float v = x;
        if (y > v)
        {
            v = y;
        }
        if (z > v)
        {
            v = z;
        }
        return v;
    }

    float GetAbsMaxElement() const
    {
        float v = abs(x);
        if (abs(y) > v)
        {
            v = abs(y);
        }
        if (abs(z) > v)
        {
            v = abs(z);
        }
        return v;
    }

    /**
     * Calculate the sum of two vectors
     * @param p_left (First vector)
     * @param p_right (Second vector)
     */
    static Vector3 Add(const Vector3& p_left, const Vector3& p_right);

    /**
     * Calculate the substraction of two vectors
     * @param p_left (First vector)
     * @param p_right (Second vector)
     */
    static Vector3 Substract(const Vector3& p_left, const Vector3& p_right);

    /**
     * Calculate the multiplication of a vector with a scalar
     * @param p_target
     * @param p_scalar
     */
    static Vector3 Multiply(const Vector3& p_target, float p_scalar);

    /**
     * Multiple two vectors component-wise
     * @param p_left
     * @param p_right
     */
    static Vector3 Multiply(const Vector3& p_left, const Vector3& p_right);

    /**
     * Divide scalar to vector left
     * @param p_left
     * @param p_scalar
     */
    static Vector3 Divide(const Vector3& p_left, float p_scalar);

    /**
     * Return the length of a vector
     * @param p_target
     */
    static float Length(const Vector3& p_target);

    /**
     * Return the dot product of two vectors
     * @param p_left
     * @param p_right
     */
    static float Dot(const Vector3& p_left, const Vector3& p_right);

    /**
     * Return the distance between two vectors
     * @param p_left
     * @param p_right
     */
    static float Distance(const Vector3& p_left, const Vector3& p_right);

    /**
     * Return the cross product of two vectors
     * @param p_left
     * @param p_right
     */
    static Vector3 Cross(const Vector3& p_left, const Vector3& p_right);

    /**
     * Return the normalize of the given vector
     * @param p_target
     */
    static Vector3 Normalize(const Vector3& p_target);

    /**
     * Calculate the interpolation between two vectors
     * @param p_start
     * @param p_end
     * @param p_alpha
     */
    static Vector3 Lerp(const Vector3& p_start, const Vector3& p_end, float p_alpha);

    /**
     * Calculate the angle between two vectors
     * @param p_from
     * @param p_to
     */
    static float AngleBetween(const Vector3& p_from, const Vector3& p_to);

    inline Vector3 operator+(const Vector3& a) const
    {
        return Vector3(x + a.x, y + a.y, z + a.z);
    }

    inline Vector3 operator-(const Vector3& a) const
    {
        return Vector3(x - a.x, y - a.y, z - a.z);
    }

    inline Vector3 operator-() const
    {
        return Vector3(-x, -y, -z);
    }

    inline Vector3 operator*(float a) const
    {
        return Vector3(x * a, y * a, z * a);
    }

    inline Vector3 operator*(const Vector3& a) const
    {
        return Vector3(x * a.x, y * a.y, z * a.z);
    }

    inline Vector3 operator/(const Vector3& a) const
    {
        return Vector3(x / a.x, y / a.y, z / a.z);
    };

    inline Vector3 operator/(float v) const
    {
        return Vector3(x / v, y / v, z / v);
    };

    inline constexpr void operator+=(const Vector3& a)
    {
        x += a.x;
        y += a.y;
        z += a.z;
    }

    inline void operator-=(const Vector3& a)
    {
        x -= a.x;
        y -= a.y;
        z -= a.z;
    }


    inline void operator*=(const Vector3& a)
    {
        x *= a.x;
        y *= a.y;
        z *= a.z;
    }

    inline void operator/=(const Vector3& a)
    {
        x /= a.x;
        y /= a.y;
        z /= a.z;
    }

    inline void operator*=(float f)
    {
        x *= f;
        y *= f;
        z *= f;
    }

    inline void operator/=(float f)
    {
        x /= f;
        y /= f;
        z /= f;
    }

    inline float operator[](int i) const
    {
        return array[i];
    }

    inline float& operator[](int i)
    {
        return array[i];
    }

    inline bool operator==(const Vector3& A) const { return (A.x == x && A.y == y && A.z == z) ? true : false; };
    inline bool operator!=(const Vector3& A) const { return (A.x == x && A.y == y && A.z == z) ? false : true; };

    inline friend std::ostream& operator<<(std::ostream& o, const Vector3& v)
    {
        o << "Vector3(" << v.x << "," << v.y << "," << v.z << ")" << std::endl;
        return o;
    }
};
} // namespace Maths
} // namespace NLS

#pragma once
#include <iostream>
#include "MathDef.h"
#include "Maths.h"
namespace NLS
{
namespace Maths
{
class Vector3;
class Vector4;
class NLS_MATH_API Vector2
{
public:
    static const Vector2 One;
    static const Vector2 Zero;
    union
    {
        struct
        {
            float x;
            float y;
        };
        float array[2];
    };

public:
    constexpr Vector2(float xVal = 0.0f, float yVal = 0.0f)
        : x(xVal), y(yVal) {}

    Vector2(const Vector3& v3);
    Vector2(const Vector4& v4);
    /**
     * Copy constructor
     * @param p_toCopy
     */
    Vector2(const Vector2& p_toCopy);

    /**
     * Move constructor
     * @param p_toMove
     */
    Vector2(Vector2&& p_toMove) noexcept = default;

    ~Vector2(void) {}

    Vector2 Normalised() const
    {
        Vector2 temp(x, y);
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
        }
    }

    float Length() const
    {
        return Sqrt((x * x) + (y * y));
    }

    constexpr float LengthSquared() const
    {
        return ((x * x) + (y * y));
    }

    constexpr float GetMaxElement() const
    {
        float v = x;
        if (y > v)
        {
            v = y;
        }
        return v;
    }

    float GetAbsMaxElement() const
    {
        float ax = abs(x);
        float ay = abs(y);

        if (ax > ay)
        {
            return ax;
        }
        return ay;
    }

    static constexpr float Dot(const Vector2& a, const Vector2& b)
    {
        return (a.x * b.x) + (a.y * b.y);
    }
    /**
     * Copy assignment
     * @param p_other
     */
    Vector2 operator=(const Vector2& p_other);

    inline Vector2 operator+(const Vector2& a) const
    {
        return Vector2(x + a.x, y + a.y);
    }

    inline Vector2 operator-(const Vector2& a) const
    {
        return Vector2(x - a.x, y - a.y);
    }

    inline Vector2 operator-() const
    {
        return Vector2(-x, -y);
    }

    inline Vector2 operator*(float a) const
    {
        return Vector2(x * a, y * a);
    }

    inline Vector2 operator*(const Vector2& a) const
    {
        return Vector2(x * a.x, y * a.y);
    }

    inline Vector2 operator/(const Vector2& a) const
    {
        return Vector2(x / a.x, y / a.y);
    };

    inline Vector2 operator/(float v) const
    {
        return Vector2(x / v, y / v);
    };

    inline constexpr void operator+=(const Vector2& a)
    {
        x += a.x;
        y += a.y;
    }

    inline void operator-=(const Vector2& a)
    {
        x -= a.x;
        y -= a.y;
    }


    inline void operator*=(const Vector2& a)
    {
        x *= a.x;
        y *= a.y;
    }

    inline void operator/=(const Vector2& a)
    {
        x /= a.x;
        y /= a.y;
    }

    inline void operator*=(float f)
    {
        x *= f;
        y *= f;
    }

    inline void operator/=(float f)
    {
        x /= f;
        y /= f;
    }

    inline float operator[](int i) const
    {
        return array[i];
    }

    inline float& operator[](int i)
    {
        return array[i];
    }

    inline bool operator==(const Vector2& A) const { return (A.x == x && A.y == y) ? true : false; };
    inline bool operator!=(const Vector2& A) const { return (A.x == x && A.y == y) ? false : true; };

    inline friend std::ostream& operator<<(std::ostream& o, const Vector2& v)
    {
        o << "Vector2(" << v.x << "," << v.y << ")" << std::endl;
        return o;
    }
    /**
     * Calculate the sum of two vectors
     * @param p_left (First vector)
     * @param p_right (Second vector)
     */
    static Vector2 Add(const Vector2& p_left, const Vector2& p_right);

    /**
     * Calculate the substraction of two vectors
     * @param p_left (First vector)
     * @param p_right (Second vector)
     */
    static Vector2 Substract(const Vector2& p_left, const Vector2& p_right);

    /**
     * Calculate the multiplication of a vector with a scalar
     * @param p_target
     * @param p_scalar
     */
    static Vector2 Multiply(const Vector2& p_target, float p_scalar);

    /**
     * Divide scalar to vector left
     * @param p_left
     * @param p_scalar
     */
    static Vector2 Divide(const Vector2& p_left, float p_scalar);

    /**
     * Return the length of a vector
     * @param p_target
     */
    static float Length(const Vector2& p_target);

    /**
     * Return the normalize of the given vector
     * @param p_target
     */
    static Vector2 Normalize(const Vector2& p_target);

    /**
     * Calculate the interpolation between two vectors
     * @param p_start
     * @param p_end
     * @param p_alpha
     */
    static Vector2 Lerp(const Vector2& p_start, const Vector2& p_end, float p_alpha);

    /**
     * Calculate the angle between two vectors
     * @param p_from
     * @param p_to
     */
    static float AngleBetween(const Vector2& p_from, const Vector2& p_to);
};
} // namespace Maths
} // namespace NLS

#pragma once
#include <iostream>
#include "MathDef.h"
namespace NLS
{
namespace Maths
{
class Matrix3;
class Matrix4;
class Vector3;

class NLS_MATH_API Quaternion
{
public:
    static const Quaternion Identity;
    union
    {
        struct
        {
            float x;
            float y;
            float z;
            float w;
        };
        float array[4];
    };

public:
    /**
     * Return an identity quaternion
     */
    static const Quaternion Identity;

    /**
     * Default Quaternion constructor (Create an identity quaternion with 1 as w)
     */
    Quaternion();

    /**
     * Create an identity quaternion with a defined real value
     * @param p_real
     */
    Quaternion(float p_real);

    /**
     * Create a quaternion from a set of 4 floats (x, y, z, w)
     * @param p_x Vector part of Quaternion
     * @param p_y Vector part of Quaternion
     * @param p_z Vector part of Quaternion
     * @param p_w Real value of Quaternion
     * @note In pure/applied Maths, we write W (or real), (Xi + Yj + Zk) (or Vector)
     */
    Quaternion(float p_x, float p_y, float p_z, float p_w);

    /**
     * Copy Constructor
     * @param p_other
     */
    Quaternion(const Quaternion& p_other);

    /**
     * Construct from rotation matrix
     * @param p_rotationMatrix
     */
    Quaternion(const Matrix3& p_rotationMatrix);

    /**
     * Construct from rotation matrix
     * @param p_rotationMatrix
     */
    Quaternion(const Matrix4& p_rotationMatrix);

    /**
     * Construct from euler angles
     * @param p_euler
     */
    Quaternion(const Vector3& p_euler);

    /**
     * Create a quaternion from a forward and up vector
     * @param p_forward
     * @param p_up
     */
    static Quaternion LookAt(const Vector3& p_forward, const Vector3& p_up);

    /**
     * Check if the quaternion is Identity
     * if the quaternion has no rotation(meaning x,y,z axis values = 0), it's Identity
     * @param p_target
     */
    static bool IsIdentity(const Quaternion& p_target);

    /*
     * Check if the quaternion is pure
     * if the quaternion has no real value(meaning real part = 0), it's pure
     * @param p_target
     */
    static bool IsPure(const Quaternion& p_target);

    /**
     * Check if the quaternion is normalized
     * @param p_target
     */
    static bool IsNormalized(const Quaternion& p_target);

    /**
     * Calculate the dot product between two quaternions
     * @param p_left
     * @param p_right
     */
    static float DotProduct(const Quaternion& p_left, const Quaternion& p_right);

    /**
     * Calculate the normalized of a quaternion
     * @param p_target
     */
    static Quaternion Normalize(const Quaternion& p_target);

    /**
     * Calculate the length of a quaternion
     * @param p_target
     */
    static float Length(const Quaternion& p_target);

    /**
     * Calculate the length square of a quaternion
     * @param p_target
     */
    static float LengthSquare(const Quaternion& p_target);

    /**
     * Return the angle of a quaternion
     * @param p_target
     */
    static float GetAngle(const Quaternion& p_target);

    /**
     * Return the rotation axis of the given quaternion
     * @param p_target
     */
    static Vector3 GetRotationAxis(const Quaternion& p_target);

    /**
     * Calculate the inverse of a quaternion
     * @param p_target
     */
    static Quaternion Inverse(const Quaternion& p_target);

    /**
     * Calculate the conjugate of a quaternion
     * @param p_target
     */
    static Quaternion Conjugate(const Quaternion& p_target);

    /**
     * Calculate the square of a quaternion
     * @param p_target
     */
    static Quaternion Square(const Quaternion& p_target);

    /**
     * Get the axis and the angle from a quaternion
     * @param p_axis
     * @param p_angle
     */
    static std::pair<Vector3, float> GetAxisAndAngle(const Quaternion& p_target);

    /**
     * Caculate the angle between two quaternions.
     * @param p_left
     * @param p_right
     */
    static float AngularDistance(const Quaternion& p_left, const Quaternion& p_right);

    /**
     * Lerp two quaternions
     * @param p_start
     * @param p_end
     * @param p_alpha
     */
    static Quaternion Lerp(const Quaternion& p_start, const Quaternion& p_end, float p_alpha);

    /**
     * Slerp two quaternions
     * @param p_first
     * @param p_second
     * @param p_alpha
     */
    static Quaternion Slerp(const Quaternion& p_start, const Quaternion& p_end, float p_alpha);

    /**
     * Nlerp two quaternions (= Lerp + normalization)
     * @param p_start
     * @param p_end
     * @param p_alpha
     */
    static Quaternion Nlerp(const Quaternion& p_start, const Quaternion& p_end, float p_alpha);

    /**
     * Rotate a point using a rotation quaternion (qpq^-1)
     * @param p_point
     * @param p_quaternion
     */
    static Vector3 RotatePoint(const Vector3& p_point, const Quaternion& p_quaternion);

    /**
     * Rotate a point around a pivot point using a rotation quaternion
     * @param p_point
     * @param p_pivot
     * @param p_quaternion
     */
    static Vector3 RotatePoint(const Vector3& p_point, const Quaternion& p_quaternion, const Vector3& p_pivot);

    /**
     * Returning Euler axis angles (In degrees) for each axis.
     * @param p_target
     */
    static Vector3 EulerAngles(const Quaternion& p_target);

    /**
     * Return a rotation matrix (3x3) out of the given quaternion
     * @param p_target
     */
    static Matrix3 ToMatrix3(const Quaternion& p_target);

    /**
     * Return a rotation matrix (4x4) out of the given quaternion
     * @param p_target
     */
    static Matrix4 ToMatrix4(const Quaternion& p_target);

    bool operator==(const Quaternion& p_otherQuat) const;
    bool operator!=(const Quaternion& p_otherQuat) const;
    Quaternion operator+(const Quaternion& p_otherQuat) const;
    Quaternion& operator+=(const Quaternion& p_otherQuat);
    Quaternion operator-(const Quaternion& p_otherQuat) const;
    Quaternion& operator-=(const Quaternion& p_otherQuat);
    float operator|(const Quaternion& p_otherQuat) const;
    Quaternion& operator*=(const float p_scale);
    Quaternion operator*(const float p_scale) const;
    Quaternion operator*(const Quaternion& p_otherQuat) const;
    Quaternion& operator*=(const Quaternion& p_otherQuat);
    Vector3 operator*(const Vector3& p_toMultiply) const;
    Matrix3 operator*(const Matrix3& p_multiply) const;
    Quaternion& operator/=(const float p_scale);
    Quaternion operator/(const float p_scale) const;
};
} // namespace Maths;
} // namespace NLS

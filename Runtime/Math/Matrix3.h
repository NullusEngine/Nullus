#pragma once
#include <assert.h>
#include <algorithm>
#include <iostream>
#include "Math/Vector3.h"
#include "Math/Vector2.h"
#include "Math/Quaternion.h"
#include "MathDef.h"
namespace NLS
{
namespace Maths
{

class NLS_MATH_API Matrix3
{
public:
    float data[9];
    static const Matrix3 Identity;

    /**
     * Default constructor
     */
    Matrix3();

    /**
     * Set all elements to value
     * @param p_value
     */
    Matrix3(float p_value);

    /**
     * Constructor
     * @param p_element1
     * @param p_element2
     * @param p_element3
     * @param p_element4
     * @param p_element5
     * @param p_element6
     * @param p_element7
     * @param p_element9
     * @param p_element8
     */
    Matrix3(float p_element1, float p_element2, float p_element3,
             float p_element4, float p_element5, float p_element6,
             float p_element7, float p_element8, float p_element9);

    /**
     * Copy constructor
     * @param p_other
     */
    Matrix3(const Matrix3& p_other);

    /**
     * Copy assignment
     * @param p_other
     */
    Matrix3& operator=(const Matrix3& p_other);

    /**
     * Check if elements are equals
     * @param p_other
     */
    bool operator==(const Matrix3& p_other);

    /**
     * Element-wise addition
     * @param p_other
     */
    Matrix3 operator+(const Matrix3& p_other) const;

    /**
     * Element-wise addition
     * @param p_other
     */
    Matrix3& operator+=(const Matrix3& p_other);

    /**
     * Element-wise subtraction
     * @param p_other
     */
    Matrix3 operator-(const Matrix3& p_other) const;

    /**
     * Element-wise subtraction
     * @param p_other
     */
    Matrix3& operator-=(const Matrix3& p_other);

    /**
     * Scalar Product
     * @param p_scalar
     */
    Matrix3 operator*(float p_scalar) const;

    /**
     * Scalar Product
     * @param p_scalar
     */
    Matrix3& operator*=(float p_scalar);

    /**
     * Vector Product
     * @param p_vector
     */
    Vector3 operator*(const Vector3& p_vector) const;

    /**
     * Matrix Product
     * @param p_other
     */
    Matrix3 operator*(const Matrix3& p_other) const;

    /**
     * Matrix Product
     * @param p_other
     */
    Matrix3& operator*=(const Matrix3& p_other);

    /**
     * Scalar Division
     * @param p_scalar
     */
    Matrix3 operator/(float p_scalar) const;

    /**
     * Scalar Division
     * @param p_scalar
     */
    Matrix3& operator/=(float p_scalar);

    /**
     * Matrix Division
     * @param p_other
     */
    Matrix3 operator/(const Matrix3& p_other) const;

    /**
     * Matrix Division
     * @param p_other
     */
    Matrix3& operator/=(const Matrix3& p_other);

    /**
     * Get element at index (row,column)
     * @param p_row
     * @param p_column
     */
    float& operator()(uint8_t p_row, uint8_t p_column);

    /**
     * Check if elements are equals
     * @param p_left
     * @param p_right
     */
    static bool AreEquals(const Matrix3& p_left, const Matrix3& p_right);

    /**
     * Element-wise addition
     * @param p_left
     * @param p_scalar
     */
    static Matrix3 Add(const Matrix3& p_left, float p_scalar);

    /**
     * Element-wise addition
     * @param p_left
     * @param p_right
     */
    static Matrix3 Add(const Matrix3& p_left, const Matrix3& p_right);

    /**
     * Element-wise subtraction
     * @param p_left
     * @param p_scalar
     */
    static Matrix3 Subtract(const Matrix3& p_left, float p_scalar);

    /**
     * Element-wise subtractions
     * @param p_left
     * @param p_right
     */
    static Matrix3 Subtract(const Matrix3& p_left, const Matrix3& p_right);

    /**
     * Scalar Product
     * @param p_left
     * @param p_scalar
     */
    static Matrix3 Multiply(const Matrix3& p_left, float p_scalar);

    /**
     * Vector Product
     * @param p_matrix
     * @param p_vector
     */
    static Vector3 Multiply(const Matrix3& p_matrix, const Vector3& p_vector);

    /**
     * Matrix Product
     * @param p_left
     * @param p_right
     */
    static Matrix3 Multiply(const Matrix3& p_left, const Matrix3& p_right);

    /**
     * Scalar Division
     * @param p_left
     * @param p_scalar
     */
    static Matrix3 Divide(const Matrix3& p_left, float p_scalar);

    /**
     * Matrix Division
     * @param p_left
     * @param p_right
     */
    static Matrix3 Divide(const Matrix3& p_left, const Matrix3& p_right);

    /**
     * Compare to Identity matrix
     * @param p_matrix
     */
    static bool IsIdentity(const Matrix3& p_matrix);

    /**
     * Compute matrix determinant
     * @param p_matrix
     */
    static float Determinant(const Matrix3& p_matrix);

    /**
     * Return transposed matrix
     * @param p_matrix
     */
    static Matrix3 Transpose(const Matrix3& p_matrix);

    /**
     * Return Cofactor matrix
     * @param p_matrix
     */
    static Matrix3 Cofactor(const Matrix3& p_matrix);

    /**
     * Return Minor matrix
     * @param p_matrix
     */
    static Matrix3 Minor(const Matrix3& p_matrix);

    /**
     * Return Adjoint matrix
     * @param p_other
     */
    static Matrix3 Adjoint(const Matrix3& p_other);

    /**
     * Return inverse matrix
     * @param p_matrix
     */
    static Matrix3 Inverse(const Matrix3& p_matrix);

    /**
     * Return 2D translation matrix
     * @param p_translation
     */
    static Matrix3 Translation(const Vector2& p_translation);

    /**
     * Translate matrix in 2D
     * @param p_matrix
     * @param p_translation
     */
    static Matrix3 Translate(const Matrix3& p_matrix, const Vector2& p_translation);

    /**
     * Return 2D rotation matrix
     * @param p_rotation angle in radians
     */
    static Matrix3 Rotation(float p_rotation);

    /**
     * Rotate matrix in 2D
     * @param p_matrix
     * @param p_rotation angle in radians
     */
    static Matrix3 Rotate(const Matrix3& p_matrix, float p_rotation);

    /**
     * Return 2D scaling matrix
     * @param p_scale
     */
    static Matrix3 Scaling(const Vector2& p_scale);

    /**
     * Scale matrix in 2D
     * @param p_matrix
     * @param p_scale
     */
    static Matrix3 Scale(const Matrix3& p_matrix, const Vector2& p_scale);

    /**
     * Get row
     * @param p_row
     */
    static Vector3 GetRow(const Matrix3& p_matrix, uint8_t p_row);

    /**
     * Get Column
     * @param p_column
     */
    static Vector3 GetColumn(const Matrix3& p_matrix, uint8_t p_column);
};
} // namespace Maths
} // namespace NLS
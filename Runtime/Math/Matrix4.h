
#pragma once

#include <iostream>
#include "MathDef.h"
namespace NLS
{
namespace Maths
{
class Vector3;
class Vector4;
class Matrix3;
class Quaternion;

class NLS_MATH_API Matrix4
{
public:
	float data[16];
	static const Matrix4 Identity;

	/**
	* Default constructor
	*/
	Matrix4();

	/**
	* Constructor
	* @param p_element1 @param p_element2
	* @param p_element3 @param p_element4
	* @param p_element5 @param p_element6
	* @param p_element7 @param p_element8
	* @param p_element9 @param p_element10
	* @param p_element11 @param p_element12
	* @param p_element13 @param p_element14
	* @param p_element15 @param p_element16
	*/
	Matrix4(float p_element1, float p_element2, float p_element3,
		float p_element4, float p_element5, float p_element6,
		float p_element7, float p_element8, float p_element9,
		float p_element10, float p_element11, float p_element12,
		float p_element13, float p_element14, float p_element15, float p_element16);

	/**
	* Copy constructor
	* @param p_other
	*/
	Matrix4(const Matrix4& p_other);

	/**
	* Copy assignment
	* @param p_other
	*/
	Matrix4& operator=(const Matrix4& p_other);

	/**
	* Check if elements are equals
	* @param p_other
	*/
	bool operator==(const Matrix4& p_other);

	/**
	* Element-wise addition
	* @param p_other
	*/
	Matrix4 operator+(const Matrix4& p_other) const;

	/**
	* Element-wise addition
	* @param p_other
	*/
	Matrix4& operator+=(const Matrix4& p_other);

	/**
	* Element-wise subtraction
	* @param p_other
	*/
	Matrix4 operator-(float p_scalar) const;

	/**
	* Element-wise subtraction
	* @param p_other
	*/
	Matrix4 operator-(const Matrix4& p_other) const;

	/**
	* Element-wise subtraction
	* @param p_other
	*/
	Matrix4& operator-=(const Matrix4& p_other);

	/**
	* Scalar Product
	* @param p_scalar
	*/
	Matrix4 operator*(float p_scalar) const;

	/**
	* Scalar Product
	* @param p_scalar
	*/
	Matrix4& operator*=(float p_scalar);

	/**
	* Vector Product
	* @param p_vector
	*/
	Vector4 operator*(const Vector4& p_vector) const;

	/**
	* Matrix Product
	* @param p_other
	*/
	Matrix4 operator*(const Matrix4& p_other) const;

	/**
	* Matrix Product
	* @param p_other
	*/
	Matrix4& operator*=(const Matrix4& p_other);

	/**
	* Scalar Division
	* @param p_scalar
	*/
	Matrix4 operator/(float p_scalar) const;

	/**
	* Scalar Division
	* @param p_scalar
	*/
	Matrix4& operator/=(float p_scalar);

	/**
	* Matrix Division
	* @param p_other
	*/
	Matrix4 operator/(const Matrix4& p_other) const;

	/**
	* Matrix Division
	* @param p_other
	*/
	Matrix4& operator/=(const Matrix4& p_other);

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
	static bool AreEquals(const Matrix4& p_left, const Matrix4& p_right);

	/**
	* Element-wise addition
	* @param p_left
	* @param p_scalar
	*/
	static Matrix4 Add(const Matrix4& p_left, float p_scalar);

	/**
	* Element-wise addition
	* @param p_left
	* @param p_right
	*/
	static Matrix4 Add(const Matrix4& p_left, const Matrix4& p_right);

	/**
	* Element-wise subtraction
	* @param p_left
	* @param p_scalar
	*/
	static Matrix4 Subtract(const Matrix4& p_left, float p_scalar);

	/**
	* Element-wise subtractions
	* @param p_left
	* @param p_right
	*/
	static Matrix4 Subtract(const Matrix4& p_left, const Matrix4& p_right);

	/**
	* Scalar Product
	* @param p_left
	* @param p_scalar
	*/
	static Matrix4 Multiply(const Matrix4& p_left, float p_scalar);

	/**
	* Vector Product
	* @param p_matrix
	* @param p_vector
	*/
	static Vector4 Multiply(const Matrix4& p_matrix, const Vector4& p_vector);

	/**
	* Matrix Product
	* @param p_left
	* @param p_right
	*/
	static Matrix4 Multiply(const Matrix4& p_left, const Matrix4& p_right);

	/**
	* Scalar Division
	* @param p_left
	* @param p_scalar
	*/
	static Matrix4 Divide(const Matrix4& p_left, float p_scalar);

	/**
	* Matrix Division
	* @param p_left
	* @param p_right
	*/
	static Matrix4 Divide(const Matrix4& p_left, const Matrix4& p_right);

	/**
	* Compare to Identity matrix
	* @param p_matrix
	*/
	static bool IsIdentity(const Matrix4& p_matrix);

	/**
	* Return Minor of matrix
	* @param p_minor0 @param p_minor1
	* @param p_minor2 @param p_minor3
	* @param p_minor4 @param p_minor5
	* @param p_minor6 @param p_minor7
	* @param p_minor8
	*/
	static float GetMinor(float p_minor0, float p_minor1, float p_minor2,
		float p_minor3, float p_minor4, float p_minor5,
		float p_minor6, float p_minor7, float p_minor8);

	/**
	* Compute matrix determinant
	* @param p_matrix
	*/
	static float Determinant(const Matrix4& p_matrix);

	/**
	* Return transposed matrix
	* @param p_matrix
	*/
	static Matrix4 Transpose(const Matrix4& p_matrix);

	/**
	* Return inverse matrix
	* @param p_matrix
	*/
	static Matrix4 Inverse(const Matrix4& p_matrix);

	/**
	* Return 3D translation matrix
	* @param p_translation
	*/
	static Matrix4 Translation(const Vector3& p_translation);

	/**
	* Translate matrix in 3D
	* @param p_matrix
	* @param p_translation
	*/
	static Matrix4 Translate(const Matrix4& p_matrix, const Vector3& p_translation);

	/**
	* Return 3D rotation matrix on X axis
	* @param p_rotation
	*/
	static Matrix4 RotationOnAxisX(float p_rotation);

	/**
	* Rotate Matrix in 3D on X axis
	* @param p_matrix
	* @param p_rotation
	*/
	static Matrix4 RotateOnAxisX(const Matrix4& p_matrix, float p_rotation);

	/**
	* Return 3D rotation matrix on Y axis
	* @param p_rotation
	*/
	static Matrix4 RotationOnAxisY(float p_rotation);

	/**
	* Rotate Matrix in 3D on Y axis
	* @param p_matrix
	* @param p_rotation
	*/
	static Matrix4 RotateOnAxisY(const Matrix4& p_matrix, float p_rotation);

	/**
	* Return 3D rotation matrix on Z axis
	* @param p_rotation
	*/
	static Matrix4 RotationOnAxisZ(float p_rotation);

	/**
	* Rotate Matrix in 3D on Z axis
	* @param p_matrix
	* @param p_rotation
	*/
	static Matrix4 RotateOnAxisZ(const Matrix4& p_matrix, float p_rotation);

	/**
	* Return 3D rotation matrix on XYZ axis
	* @param p_rotation
	*/
	static Matrix4 RotationYXZ(float p_rotation);

	/**
	* Rotate Matrix in 3D on XYZ axis
	* @param p_matrix
	* @param p_rotation
	*/
	static Matrix4 RotateYXZ(const Matrix4& p_matrix, float p_rotation);

	/**
	* Return 3D scaling matrix
	* @param p_scale
	*/
	static Matrix4 Scaling(const Vector3& p_scale);

	/**
	* Scale matrix in 3D
	* @param p_matrix
	* @param p_scale
	*/
	static Matrix4 Scale(const Matrix4& p_matrix, const Vector3& p_scale);

	/**
	* Return rotation matrix from quaternion
	* @param p_quaternion
	*/
	static Matrix4 Rotation(const Quaternion& p_quaternion);

	/**
	* Return rotate matrix in 3D on quaternion
	* @param p_matrix
	* @param p_quaternion
	*/
	static Matrix4 Rotate(const Matrix4& p_matrix, const Quaternion& p_quaternion);

	/**
	* Return perspective matrix
	* @param p_fov
	* @param p_aspectRatio
	* @param p_zNear
	* @param p_zFar
	*/
	static Matrix4 CreatePerspective(const float p_fov, const float p_aspectRatio, const float p_zNear, const float p_zFar);

	/**
	* Returns an orthographic matrix
	* @param p_size
	* @param p_aspectRatio
	* @param p_zNear
	* @param p_zFar
	*/
	static Matrix4 CreateOrthographic(const float p_size, const float p_aspectRatio, const float p_zNear, const float p_zFar);

	/**
	* Return view matrix
	* @param p_eyeX
	* @param p_eyeY
	* @param p_eyeZ
	* @param p_lookX
	* @param p_lookY
	* @param p_lookZ
	* @param p_upX
	* @param p_upY
	* @param p_upZ
	*/
	static Matrix4 CreateView(const float p_eyeX, const float p_eyeY, const float p_eyeZ, const float p_lookX, const float p_lookY, const float p_lookZ, const float p_upX, const float p_upY, const float p_upZ);

	/**
	* Return frustum matrix
	* @param p_left
	* @param p_right
	* @param p_bottom
	* @param p_top
	* @param p_zNear
	* @param p_zFar
	*/
	static Matrix4 CreateFrustum(const float p_left, const float p_right, const float p_bottom, const float p_top, const float p_zNear, const float p_zFar);

	/**
	* Get row
	* @param p_row
	*/
	static Vector4 GetRow(const Matrix4& p_matrix, uint8_t p_row);

	/**
	* Get Column
	* @param p_column
	*/
	static Vector4 GetColumn(const Matrix4& p_matrix, uint8_t p_column);
};
} // namespace Maths
} // namespace NLS

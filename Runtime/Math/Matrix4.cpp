#include "Matrix4.h"
#include "Matrix3.h"
#include "Maths.h"
#include "Vector3.h"
#include "Vector4.h"
#include "Quaternion.h"
#include <cstring>
#include <string>
using namespace NLS;
using namespace NLS::Maths;

constexpr float kPI = 3.14159265359f;
constexpr float kEpsilon = 0.00001f;

const Maths::Matrix4 Maths::Matrix4::Identity{
	1.f, 0.f, 0.f, 0.f,
	0.f, 1.f, 0.f, 0.f,
	0.f, 0.f, 1.f, 0.f,
	0.f, 0.f, 0.f, 1.f
};

Maths::Matrix4::Matrix4()
{
	memcpy(this->data, Identity.data, 16 * sizeof(float)); // TODO: memcpy is not great (consider std::data)
}

Maths::Matrix4::Matrix4(float p_element1, float p_element2, float p_element3, float p_element4, float p_element5, float p_element6, float p_element7, float p_element8, float p_element9, float p_element10, float p_element11, float p_element12, float p_element13, float p_element14, float p_element15, float p_element16)
{
	data[0] = p_element1;
	data[1] = p_element2;
	data[2] = p_element3;
	data[3] = p_element4;
	data[4] = p_element5;
	data[5] = p_element6;
	data[6] = p_element7;
	data[7] = p_element8;
	data[8] = p_element9;
	data[9] = p_element10;
	data[10] = p_element11;
	data[11] = p_element12;
	data[12] = p_element13;
	data[13] = p_element14;
	data[14] = p_element15;
	data[15] = p_element16;
}

Maths::Matrix4::Matrix4(const Matrix4& p_other)
{
	*this = p_other;
}

Maths::Matrix4& Maths::Matrix4::operator=(const Matrix4& p_other)
{
	memcpy(this->data, p_other.data, 16 * sizeof(float));
	return *this;
}

bool Maths::Matrix4::operator==(const Matrix4& p_other)
{
	return AreEquals(*this, p_other);
}

Maths::Matrix4 Maths::Matrix4::operator+(const Matrix4& p_other) const
{
	return Add(*this, p_other);
}

Maths::Matrix4& Maths::Matrix4::operator+=(const Matrix4& p_other)
{
	*this = Add(*this, p_other);
	return *this;
}

Maths::Matrix4 Maths::Matrix4::operator-(float p_scalar) const
{
	return Subtract(*this, p_scalar);
}

Maths::Matrix4 Maths::Matrix4::operator-(const Matrix4& p_other) const
{
	return Subtract(*this, p_other);
}

Maths::Matrix4& Maths::Matrix4::operator-=(const Matrix4& p_other)
{
	*this = Subtract(*this, p_other);
	return *this;
}

Maths::Matrix4 Maths::Matrix4::operator*(float p_scalar) const
{
	return Multiply(*this, p_scalar);
}

Maths::Matrix4& Maths::Matrix4::operator*=(float p_scalar)
{
	*this = Multiply(*this, p_scalar);
	return *this;
}

Maths::Vector4 Maths::Matrix4::operator*(const Vector4& p_vector) const
{
	return Multiply(*this, p_vector);
}

Maths::Matrix4 Maths::Matrix4::operator*(const Matrix4& p_other) const
{
	return Multiply(*this, p_other);
}

Maths::Matrix4& Maths::Matrix4::operator*=(const Matrix4& p_other)
{
	*this = Multiply(*this, p_other);
	return *this;
}

Maths::Matrix4 Maths::Matrix4::operator/(float p_scalar) const
{
	return Divide(*this, p_scalar);
}

Maths::Matrix4& Maths::Matrix4::operator/=(float p_scalar)
{
	*this = Divide(*this, p_scalar);
	return *this;
}

Maths::Matrix4 Maths::Matrix4::operator/(const Matrix4& p_other) const
{
	return Divide(*this, p_other);
}

Maths::Matrix4& Maths::Matrix4::operator/=(const Matrix4& p_other)
{
	*this = Divide(*this, p_other);
	return *this;
}

float& Maths::Matrix4::operator()(uint8_t p_row, uint8_t p_column)
{
	if (p_row >= 4 || p_column >= 4)
		throw std::out_of_range(
			"Invalid index : " + std::to_string(p_row) + "," + std::to_string(p_column) + " is out of range");
	return data[4 * p_row + p_column];
}

bool Maths::Matrix4::AreEquals(const Matrix4& p_left, const Matrix4& p_right)
{
	return memcmp(&p_left, &p_right, 16 * sizeof(float)) == 0;
}

Maths::Matrix4 Maths::Matrix4::Add(const Matrix4& p_left, float p_scalar)
{
	Matrix4 result(p_left);
	for (int8_t i = 0; i < 16; i++)
		result.data[i] += p_scalar;
	return result;
}

Maths::Matrix4 Maths::Matrix4::Add(const Matrix4& p_left, const Matrix4& p_right)
{
	Matrix4 result(p_left);
	for (int8_t i = 0; i < 16; i++)
		result.data[i] += p_right.data[i];
	return result;
}

Maths::Matrix4 Maths::Matrix4::Subtract(const Matrix4& p_left, float p_scalar)
{
	Matrix4 result(p_left);
	for (int8_t i = 0; i < 16; ++i)
		result.data[i] -= p_scalar;
	return result;
}

Maths::Matrix4 Maths::Matrix4::Subtract(const Matrix4& p_left, const Matrix4& p_right)
{
	Matrix4 result(p_left);
	for (int8_t i = 0; i < 16; ++i)
		result.data[i] -= p_right.data[i];
	return result;
}

Maths::Matrix4 Maths::Matrix4::Multiply(const Matrix4& p_left, float p_scalar)
{
	Matrix4 result(p_left);
	for (int8_t i = 0; i < 16; ++i)
		result.data[i] *= p_scalar;
	return result;
}

Maths::Vector4 Maths::Matrix4::Multiply(const Matrix4& p_matrix, const Vector4& p_vector)
{
	Vector4 multiply;

	multiply.x = ((p_matrix.data[0] * p_vector.x) + (p_matrix.data[1] * p_vector.y) + (p_matrix.data[2]
		* p_vector.z) + (p_matrix.data[3] * p_vector.w));
	multiply.y = ((p_matrix.data[4] * p_vector.x) + (p_matrix.data[5] * p_vector.y) + (p_matrix.data[6]
		* p_vector.z) + (p_matrix.data[7] * p_vector.w));
	multiply.z = ((p_matrix.data[8] * p_vector.x) + (p_matrix.data[9] * p_vector.y) + (p_matrix.data[10]
		* p_vector.z) + (p_matrix.data[11] * p_vector.w));
	multiply.w = ((p_matrix.data[12] * p_vector.x) + (p_matrix.data[13] * p_vector.y) + (p_matrix.data[
		14] * p_vector.z) + (p_matrix.data[15] * p_vector.w));
	return multiply;
}

Maths::Matrix4 Maths::Matrix4::Multiply(const Matrix4& p_left, const Matrix4& p_right)
{
	return Matrix4(
		((p_left.data[0] * p_right.data[0]) + (p_left.data[1] * p_right.data[4]) + (p_left.data[
			2] * p_right.data[8]) + (p_left.data[3] * p_right.data[12])),
		((p_left.data[0] * p_right.data[1]) + (p_left.data[1] * p_right.data[5]) + (p_left.data[
			2] * p_right.data[9]) + (p_left.data[3] * p_right.data[13])),
		((p_left.data[0] * p_right.data[2]) + (p_left.data[1] * p_right.data[6]) + (p_left.data[
			2] * p_right.data[10]) + (p_left.data[3] * p_right.data[14])),
		((p_left.data[0] * p_right.data[3]) + (p_left.data[1] * p_right.data[7]) + (p_left.data[
			2] * p_right.data[11]) + (p_left.data[3] * p_right.data[15])),

		((p_left.data[4] * p_right.data[0]) + (p_left.data[5] * p_right.data[4]) + (p_left.data[
			6] * p_right.data[8]) + (p_left.data[7] * p_right.data[12])),
		((p_left.data[4] * p_right.data[1]) + (p_left.data[5] * p_right.data[5]) + (p_left.data[
			6] * p_right.data[9]) + (p_left.data[7] * p_right.data[13])),
		((p_left.data[4] * p_right.data[2]) + (p_left.data[5] * p_right.data[6]) + (p_left.data[
			6] * p_right.data[10]) + (p_left.data[7] * p_right.data[14])),
		((p_left.data[4] * p_right.data[3]) + (p_left.data[5] * p_right.data[7]) + (p_left.data[
			6] * p_right.data[11]) + (p_left.data[7] * p_right.data[15])),

		((p_left.data[8] * p_right.data[0]) + (p_left.data[9] * p_right.data[4]) + (p_left.data[
			10] * p_right.data[8]) + (p_left.data[11] * p_right.data[12])),
		((p_left.data[8] * p_right.data[1]) + (p_left.data[9] * p_right.data[5]) + (p_left.data[
			10] * p_right.data[9]) + (p_left.data[11] * p_right.data[13])),
		((p_left.data[8] * p_right.data[2]) + (p_left.data[9] * p_right.data[6]) + (p_left.data[
			10] * p_right.data[10]) + (p_left.data[11] * p_right.data[14])),
		((p_left.data[8] * p_right.data[3]) + (p_left.data[9] * p_right.data[7]) + (p_left.data[
			10] * p_right.data[11]) + (p_left.data[11] * p_right.data[15])),

		((p_left.data[12] * p_right.data[0]) + (p_left.data[13] * p_right.data[4]) + (p_left.
			data[14] * p_right.data[8]) + (p_left.data[15] * p_right.data[12])),
		((p_left.data[12] * p_right.data[1]) + (p_left.data[13] * p_right.data[5]) + (p_left.
			data[14] * p_right.data[9]) + (p_left.data[15] * p_right.data[13])),
		((p_left.data[12] * p_right.data[2]) + (p_left.data[13] * p_right.data[6]) + (p_left.
			data[14] * p_right.data[10]) + (p_left.data[15] * p_right.data[14])),
		((p_left.data[12] * p_right.data[3]) + (p_left.data[13] * p_right.data[7]) + (p_left.
			data[14] * p_right.data[11]) + (p_left.data[15] * p_right.data[15])));
}

Maths::Matrix4 Maths::Matrix4::Divide(const Matrix4& p_left, float p_scalar)
{
	Matrix4 result(p_left);
	for (float& element : result.data)
		element /= p_scalar;

	return result;
}

Maths::Matrix4 Maths::Matrix4::Divide(const Matrix4& p_left, const Matrix4& p_right)
{
	Matrix4 leftCopy(p_left);
	return leftCopy * Inverse(p_right);
}

bool Maths::Matrix4::IsIdentity(const Matrix4& p_matrix)
{
	return memcmp(Identity.data, p_matrix.data, 16 * sizeof(float)) == 0;
}

float Maths::Matrix4::GetMinor(float p_minor0, float p_minor1, float p_minor2, float p_minor3, float p_minor4, float p_minor5, float p_minor6, float p_minor7, float p_minor8)
{
	return p_minor0 * (p_minor4 * p_minor8 - p_minor5 * p_minor7)
		- p_minor1 * (p_minor3 * p_minor8 - p_minor5 * p_minor6)
		+ p_minor2 * (p_minor3 * p_minor7 - p_minor4 * p_minor6);
}

float Maths::Matrix4::Determinant(const Matrix4& p_matrix)
{
	return p_matrix.data[0] * GetMinor(p_matrix.data[5], p_matrix.data[9], p_matrix.data[13], p_matrix.data[6], p_matrix.data[10], p_matrix.data[14],
		p_matrix.data[7], p_matrix.data[11], p_matrix.data[15])
		- p_matrix.data[4] * GetMinor(p_matrix.data[1], p_matrix.data[9], p_matrix.data[13], p_matrix.data[2], p_matrix.data[10], p_matrix.data[14],
			p_matrix.data[3], p_matrix.data[11], p_matrix.data[15])
		+ p_matrix.data[8] * GetMinor(p_matrix.data[1], p_matrix.data[5], p_matrix.data[13], p_matrix.data[2], p_matrix.data[6], p_matrix.data[14],
			p_matrix.data[3], p_matrix.data[7], p_matrix.data[15])
		- p_matrix.data[12] * GetMinor(p_matrix.data[1], p_matrix.data[5], p_matrix.data[9], p_matrix.data[2], p_matrix.data[6], p_matrix.data[10],
			p_matrix.data[3], p_matrix.data[7], p_matrix.data[11]);
}

Maths::Matrix4 Maths::Matrix4::Transpose(const Matrix4& p_matrix)
{
	Matrix4 TransposedMatrix(p_matrix);

	for (int i = 0; i < 4; i++)
	{
		for (int j = 0; j < 4; j++)
		{
			TransposedMatrix.data[4 * j + i] = p_matrix.data[4 * i + j];
		}
	}
	return TransposedMatrix;
}

Maths::Matrix4 Maths::Matrix4::Inverse(const Matrix4& p_matrix)
{
	const float determinant = Determinant(p_matrix);
	if (determinant == 0)
		throw std::logic_error("Division by 0");

	const float cof0 = GetMinor(p_matrix.data[5], p_matrix.data[9], p_matrix.data[13], p_matrix.data[6], p_matrix.data[10], p_matrix.data[14],
		p_matrix.data[7], p_matrix.data[11], p_matrix.data[15]);
	const float cof1 = GetMinor(p_matrix.data[1], p_matrix.data[9], p_matrix.data[13], p_matrix.data[2], p_matrix.data[10], p_matrix.data[14],
		p_matrix.data[3], p_matrix.data[11], p_matrix.data[15]);
	const float cof2 = GetMinor(p_matrix.data[1], p_matrix.data[5], p_matrix.data[13], p_matrix.data[2], p_matrix.data[6], p_matrix.data[14],
		p_matrix.data[3], p_matrix.data[7], p_matrix.data[15]);
	const float cof3 = GetMinor(p_matrix.data[1], p_matrix.data[5], p_matrix.data[9], p_matrix.data[2], p_matrix.data[6], p_matrix.data[10],
		p_matrix.data[3], p_matrix.data[7], p_matrix.data[11]);

	const float det = p_matrix.data[0] * cof0 - p_matrix.data[4] * cof1 + p_matrix.data[8] * cof2 - p_matrix.data[12] * cof3;
	if (fabs(det) <= kEpsilon)
		return Identity;
	const float cof4 = GetMinor(p_matrix.data[4], p_matrix.data[8], p_matrix.data[12], p_matrix.data[6], p_matrix.data[10], p_matrix.data[14],
		p_matrix.data[7], p_matrix.data[11], p_matrix.data[15]);
	const float cof5 = GetMinor(p_matrix.data[0], p_matrix.data[8], p_matrix.data[12], p_matrix.data[2], p_matrix.data[10], p_matrix.data[14],
		p_matrix.data[3], p_matrix.data[11], p_matrix.data[15]);
	const float cof6 = GetMinor(p_matrix.data[0], p_matrix.data[4], p_matrix.data[12], p_matrix.data[2], p_matrix.data[6], p_matrix.data[14],
		p_matrix.data[3], p_matrix.data[7], p_matrix.data[15]);
	const float cof7 = GetMinor(p_matrix.data[0], p_matrix.data[4], p_matrix.data[8], p_matrix.data[2], p_matrix.data[6], p_matrix.data[10],
		p_matrix.data[3], p_matrix.data[7], p_matrix.data[11]);

	const float cof8 = GetMinor(p_matrix.data[4], p_matrix.data[8], p_matrix.data[12], p_matrix.data[5], p_matrix.data[9], p_matrix.data[13],
		p_matrix.data[7], p_matrix.data[11], p_matrix.data[15]);
	const float cof9 = GetMinor(p_matrix.data[0], p_matrix.data[8], p_matrix.data[12], p_matrix.data[1], p_matrix.data[9], p_matrix.data[13],
		p_matrix.data[3], p_matrix.data[11], p_matrix.data[15]);
	const float cof10 = GetMinor(p_matrix.data[0], p_matrix.data[4], p_matrix.data[12], p_matrix.data[1], p_matrix.data[5], p_matrix.data[13],
		p_matrix.data[3], p_matrix.data[7], p_matrix.data[15]);
	const float cof11 = GetMinor(p_matrix.data[0], p_matrix.data[4], p_matrix.data[8], p_matrix.data[1], p_matrix.data[5], p_matrix.data[9],
		p_matrix.data[3], p_matrix.data[7], p_matrix.data[11]);

	const float cof12 = GetMinor(p_matrix.data[4], p_matrix.data[8], p_matrix.data[12], p_matrix.data[5], p_matrix.data[9], p_matrix.data[13],
		p_matrix.data[6], p_matrix.data[10], p_matrix.data[14]);
	const float cof13 = GetMinor(p_matrix.data[0], p_matrix.data[8], p_matrix.data[12], p_matrix.data[1], p_matrix.data[9], p_matrix.data[13],
		p_matrix.data[2], p_matrix.data[10], p_matrix.data[14]);
	const float cof14 = GetMinor(p_matrix.data[0], p_matrix.data[4], p_matrix.data[12], p_matrix.data[1], p_matrix.data[5], p_matrix.data[13],
		p_matrix.data[2], p_matrix.data[6], p_matrix.data[14]);
	const float cof15 = GetMinor(p_matrix.data[0], p_matrix.data[4], p_matrix.data[8], p_matrix.data[1], p_matrix.data[5], p_matrix.data[9],
		p_matrix.data[2], p_matrix.data[6], p_matrix.data[10]);

	const float detInv = 1.0f / det;
	Matrix4 inverse;

	inverse.data[0] = detInv * cof0;
	inverse.data[4] = -detInv * cof4;
	inverse.data[8] = detInv * cof8;
	inverse.data[12] = -detInv * cof12;
	inverse.data[1] = -detInv * cof1;
	inverse.data[5] = detInv * cof5;
	inverse.data[9] = -detInv * cof9;
	inverse.data[13] = detInv * cof13;
	inverse.data[2] = detInv * cof2;
	inverse.data[6] = -detInv * cof6;
	inverse.data[10] = detInv * cof10;
	inverse.data[14] = -detInv * cof14;
	inverse.data[3] = -detInv * cof3;
	inverse.data[7] = detInv * cof7;
	inverse.data[11] = -detInv * cof11;
	inverse.data[15] = detInv * cof15;

	return inverse;
}

Maths::Matrix4 Maths::Matrix4::Translation(const Vector3& p_translation)
{
	return Matrix4(1, 0, 0, p_translation.x,
		0, 1, 0, p_translation.y,
		0, 0, 1, p_translation.z,
		0, 0, 0, 1);
}

Maths::Matrix4 Maths::Matrix4::Translate(const Matrix4& p_matrix, const Vector3& p_translation)
{
	return p_matrix * Translation(p_translation);
}

Maths::Matrix4 Maths::Matrix4::RotationOnAxisX(float p_rotation)
{
	return Matrix4(1, 0, 0, 0,
		0, std::cos(p_rotation), -std::sin(p_rotation), 0,
		0, std::sin(p_rotation), std::cos(p_rotation), 0,
		0, 0, 0, 1);
}

Maths::Matrix4 Maths::Matrix4::RotateOnAxisX(const Matrix4& p_matrix, float p_rotation)
{
	return p_matrix * RotationOnAxisX(p_rotation);
}

Maths::Matrix4 Maths::Matrix4::RotationOnAxisY(float p_rotation)
{
	return Matrix4(std::cos(p_rotation), 0, -std::sin(p_rotation), 0,
		0, 1, 0, 0,
		std::sin(p_rotation), 0, std::cos(p_rotation), 0,
		0, 0, 0, 1);
}

Maths::Matrix4 Maths::Matrix4::RotateOnAxisY(const Matrix4& p_matrix, float p_rotation)
{
	return p_matrix * RotationOnAxisY(p_rotation);
}

Maths::Matrix4 Maths::Matrix4::RotationOnAxisZ(float p_rotation)
{
	return Matrix4(std::cos(p_rotation), -std::sin(p_rotation), 0, 0,
		std::sin(p_rotation), std::cos(p_rotation), 0, 0,
		0, 0, 1, 0,
		0, 0, 0, 1);
}

Maths::Matrix4 Maths::Matrix4::RotateOnAxisZ(const Matrix4& p_matrix, float p_rotation)
{
	return p_matrix * RotationOnAxisZ(p_rotation);
}

Maths::Matrix4 Maths::Matrix4::RotationYXZ(float p_rotation)
{
	Matrix4 Xrot = RotationOnAxisX(p_rotation);
	Matrix4 Yrot = RotationOnAxisY(p_rotation);
	Matrix4 Zrot = RotationOnAxisZ(p_rotation);

	return Yrot * Xrot * Zrot;
}

Maths::Matrix4 Maths::Matrix4::RotateYXZ(const Matrix4& p_matrix, float p_rotation)
{
	return p_matrix * RotationYXZ(p_rotation);
}

Maths::Matrix4 Maths::Matrix4::Scaling(const Vector3& p_scale)
{
	return Matrix4(p_scale.x, 0, 0, 0,
		0, p_scale.y, 0, 0,
		0, 0, p_scale.z, 0,
		0, 0, 0, 1);
}

Maths::Matrix4 Maths::Matrix4::Scale(const Matrix4& p_matrix, const Vector3& p_scale)
{
	return p_matrix * Scaling(p_scale);
}

Maths::Matrix4 Maths::Matrix4::Rotation(const Quaternion& p_quaternion)
{
	return Maths::Quaternion::ToMatrix4(p_quaternion);
}

Maths::Matrix4 Maths::Matrix4::Rotate(const Matrix4& p_matrix, const Quaternion& p_quaternion)
{
	return p_matrix * Rotation(p_quaternion);
}

Maths::Matrix4 Maths::Matrix4::CreatePerspective(const float p_fov, const float p_aspectRatio, const float p_zNear, const float p_zFar)
{
	const float tangent = tanf(p_fov / 2.0f * kPI / 180.0f);
	const float height = p_zNear * tangent;
	const float width = height * p_aspectRatio;

	return CreateFrustum(-width, width, -height, height, p_zNear, p_zFar);
}

Maths::Matrix4 Maths::Matrix4::CreateOrthographic(const float p_size, const float p_aspectRatio, const float p_zNear, const float p_zFar)
{
	auto ortho = Maths::Matrix4::Identity;

	const auto right = p_size * p_aspectRatio;
	const auto left = -right;

	const auto top = p_size;
	const auto bottom = -top;

	ortho(0, 0) = 2.0f / (right - left);
	ortho(1, 1) = 2.0f / (top - bottom);
	ortho(2, 2) = -2.0f / (p_zFar - p_zNear);
	ortho(0, 3) = -(right + left) / (right - left);
	ortho(1, 3) = -(top + bottom) / (top - bottom);
	ortho(2, 3) = -(p_zFar + p_zNear) / (p_zFar - p_zNear);
	ortho(3, 3) = 1.0f;

	return ortho;
}

Maths::Matrix4 Maths::Matrix4::CreateView(const float p_eyeX, const float p_eyeY, const float p_eyeZ, const float p_lookX, const float p_lookY, const float p_lookZ, const float p_upX, const float p_upY, const float p_upZ)
{
	const Maths::Vector3 eye(p_eyeX, p_eyeY, p_eyeZ);
	const Maths::Vector3 look(p_lookX, p_lookY, p_lookZ);
	const Maths::Vector3 up(p_upX, p_upY, p_upZ);

	const Maths::Vector3 forward(eye - look);
	Vector3::Normalize(forward);

	const Maths::Vector3 upXForward(Maths::Vector3::Cross(up, forward));
	Vector3::Normalize(upXForward);

	const Maths::Vector3 v(Maths::Vector3::Cross(forward, upXForward));

	Maths::Matrix4 View;

	View.data[0] = upXForward.x;
	View.data[1] = upXForward.y;
	View.data[2] = upXForward.z;
	View.data[3] = -Maths::Vector3::Dot(eye, upXForward);

	View.data[4] = v.x;
	View.data[5] = v.y;
	View.data[6] = v.z;
	View.data[7] = -Maths::Vector3::Dot(eye, v);

	View.data[8] = forward.x;
	View.data[9] = forward.y;
	View.data[10] = forward.z;
	View.data[11] = -Maths::Vector3::Dot(eye, forward);

	return View;
}

Maths::Matrix4 Maths::Matrix4::CreateFrustum(const float p_left, const float p_right, const float p_bottom, const float p_top, const float p_zNear, const float p_zFar)
{
	const float maxView = 2.0f * p_zNear;
	const float width = p_right - p_left;
	const float height = p_top - p_bottom;
	const float zRange = p_zFar - p_zNear;

	Matrix4 Frustum;

	Frustum.data[0] = maxView / width;
	Frustum.data[5] = maxView / height;
	Frustum.data[2] = (p_right + p_left) / width;
	Frustum.data[6] = (p_top + p_bottom) / height;
	Frustum.data[10] = (-p_zFar - p_zNear) / zRange;
	Frustum.data[14] = -1.0f;
	Frustum.data[11] = (-maxView * p_zFar) / zRange;
	Frustum.data[15] = 0.0f;

	return Frustum;
}

Maths::Vector4 Maths::Matrix4::GetRow(const Matrix4& p_matrix, uint8_t p_row)
{
	if (p_row >= 4)
		throw std::out_of_range("Invalid index : " + std::to_string(p_row) + " is out of range");

	return Vector4(p_matrix.data[p_row * 4], p_matrix.data[p_row * 4 + 1], p_matrix.data[p_row * 4 + 2], p_matrix.data[p_row * 4 + 3]);
}

Maths::Vector4 Maths::Matrix4::GetColumn(const Matrix4& p_matrix, uint8_t p_column)
{
	if (p_column >= 4)
		throw std::out_of_range("Invalid index : " + std::to_string(p_column) + " is out of range");

	return Vector4(p_matrix.data[p_column], p_matrix.data[p_column + 4], p_matrix.data[p_column + 8], p_matrix.data[p_column + 12]);
}

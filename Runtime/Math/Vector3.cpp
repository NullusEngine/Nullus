#include "Vector4.h"
#include "Vector3.h"
#include "Vector2.h"
using namespace NLS;
using namespace Maths;

const Vector3 Vector3::One(1.0f, 1.0f, 1.0f);
const Vector3 Vector3::Zero(0.0f, 0.0f, 0.0f);
const Vector3 Vector3::Forward(0.0f, 0.0f, 1.0f);
const Vector3 Vector3::Right(1.0f, 0.0f, 0.0f);
const Vector3 Vector3::Up(0.0f, 1.0f, 0.0f);

Vector3::Vector3(const Vector2& v2, float newZ)
    : x(v2.x), y(v2.y), z(newZ)
{
}

Vector3::Vector3(const Vector4& v4)
    : x(v4.x), y(v4.y), z(v4.z)
{
}
Maths::Vector3 Maths::Vector3::Add(const Vector3& p_left, const Vector3& p_right)
{
	return Vector3
	(
		p_left.x + p_right.x,
		p_left.y + p_right.y,
		p_left.z + p_right.z
	);
}

Maths::Vector3 Maths::Vector3::Substract(const Vector3& p_left, const Vector3& p_right)
{
	return Vector3
	(
		p_left.x - p_right.x,
		p_left.y - p_right.y,
		p_left.z - p_right.z
	);
}

Maths::Vector3 Maths::Vector3::Multiply(const Vector3& p_target, float p_scalar)
{
	return Vector3
	(
		p_target.x * p_scalar,
		p_target.y * p_scalar,
		p_target.z * p_scalar
	);
}

Maths::Vector3 Maths::Vector3::Multiply(const Vector3& p_left, const Vector3& p_right)
{
	return Vector3
	(
		p_left.x * p_right.x,
		p_left.y * p_right.y,
		p_left.z * p_right.z
	);
}

Maths::Vector3 Maths::Vector3::Divide(const Vector3& p_left, float p_scalar)
{
	Vector3 result(p_left);

	if (p_scalar == 0)
		throw std::logic_error("Division by 0");

	result.x /= p_scalar;
	result.y /= p_scalar;
	result.z /= p_scalar;

	return result;
}

float Maths::Vector3::Length(const Vector3& p_target)
{
	return std::sqrt(p_target.x * p_target.x + p_target.y * p_target.y + p_target.z * p_target.z);
}

float Maths::Vector3::Dot(const Vector3& p_left, const Vector3& p_right)
{
	return p_left.x * p_right.x + p_left.y * p_right.y + p_left.z * p_right.z;
}

float Maths::Vector3::Distance(const Vector3& p_left, const Vector3& p_right)
{
	return std::sqrt
	(
		(p_left.x - p_right.x) * (p_left.x - p_right.x) +
		(p_left.y - p_right.y) * (p_left.y - p_right.y) +
		(p_left.z - p_right.z) * (p_left.z - p_right.z)
	);
}

Maths::Vector3 Maths::Vector3::Cross(const Vector3& p_left, const Vector3& p_right)
{
	return Vector3
	(
		p_left.y * p_right.z - p_left.z * p_right.y,
		p_left.z * p_right.x - p_left.x * p_right.z,
		p_left.x * p_right.y - p_left.y * p_right.x
	);
}

Maths::Vector3 Maths::Vector3::Normalize(const Vector3& p_target)
{
	float length = Length(p_target);

	if (length > 0.0f)
	{
		float targetLength = 1.0f / length;

		return Vector3
		(
			p_target.x * targetLength,
			p_target.y * targetLength,
			p_target.z * targetLength
		);
	}
	else
	{
		return Vector3::Zero;
	}
}

Maths::Vector3 Maths::Vector3::Lerp(const Vector3& p_start, const Vector3& p_end, float p_alpha)
{
	return (p_start + (p_end - p_start) * p_alpha);
}

float Maths::Vector3::AngleBetween(const Vector3& p_from, const Vector3& p_to)
{
	float lengthProduct = Length(p_from) * Length(p_to);

	if (lengthProduct > 0.0f)
	{
		float fractionResult = Dot(p_from, p_to) / lengthProduct;

		if (fractionResult >= -1.0f && fractionResult <= 1.0f)
			return acosf(fractionResult);
	}

	return 0.0f;
}
#include "Rendering/Entities/Light.h"
using namespace NLS;
uint32_t Pack(uint8_t c0, uint8_t c1, uint8_t c2, uint8_t c3)
{
	return (c0 << 24) | (c1 << 16) | (c2 << 8) | c3;
}

uint32_t Pack(const Maths::Vector3& p_toPack)
{
	return Pack(static_cast<uint8_t>(p_toPack.x * 255.f), static_cast<uint8_t>(p_toPack.y * 255.f), static_cast<uint8_t>(p_toPack.z * 255.f), 0);
}

Maths::Matrix4 NLS::Rendering::Entities::Light::GenerateMatrix() const
{
	Maths::Matrix4 result;

	auto position = transform.GetWorldPosition();
	result.array[0] = position.x;
	result.array[1] = position.y;
	result.array[2] = position.z;

	auto forward = transform.GetWorldForward();
	result.array[4] = forward.x;
	result.array[5] = forward.y;
	result.array[6] = forward.z;

	result.array[8] = static_cast<float>(Pack(color));

	result.array[12] = static_cast<float>(type);
	result.array[13] = cutoff;
	result.array[14] = outerCutoff;

	result.array[3] = constant;
	result.array[7] = linear;
	result.array[11] = quadratic;
	result.array[15] = intensity;

	return result;
}

float CalculateLuminosity(float p_constant, float p_linear, float p_quadratic, float p_intensity, float p_distance)
{
	auto attenuation = (p_constant + p_linear * p_distance + p_quadratic * (p_distance * p_distance));
	return (1.0f / attenuation) * std::abs(p_intensity);
}

float CalculatePointLightRadius(float p_constant, float p_linear, float p_quadratic, float p_intensity)
{
	constexpr float threshold = 1 / 255.0f;
	constexpr float step = 1.0f;

	float distance = 0.0f;

	#define TRY_GREATER(value)\
	else if (CalculateLuminosity(p_constant, p_linear, p_quadratic, p_intensity, value) > threshold)\
	{\
		distance = value;\
	}

	#define TRY_LESS(value, newValue)\
	else if (CalculateLuminosity(p_constant, p_linear, p_quadratic, p_intensity, value) < threshold)\
	{\
		distance = newValue;\
	}

	// Prevents infinite while true. If a light has a bigger radius than 10000 we ignore it and make it infinite
	if (CalculateLuminosity(p_constant, p_linear, p_quadratic, p_intensity, 1000.0f) > threshold)
	{
		return std::numeric_limits<float>::infinity();
	}
	TRY_LESS(20.0f, 0.0f)
	TRY_GREATER(750.0f)
	TRY_LESS(50.0f, 20.0f + step)
	TRY_LESS(100.0f, 50.0f + step)
	TRY_GREATER(500.0f)
	TRY_GREATER(250.0f)

	while (true)
	{
		if (CalculateLuminosity(p_constant, p_linear, p_quadratic, p_intensity, distance) < threshold) // If the light has a very low luminosity for the given distance, we consider the current distance as the light radius
		{
			return distance;
		}
		else
		{
			distance += step;
		}
	}
}

float CalculateAmbientBoxLightRadius(const Maths::Vector3& p_position, const Maths::Vector3& p_size)
{
	return Maths::Vector3::Distance(p_position, p_position + p_size);
}

float NLS::Rendering::Entities::Light::GetEffectRange() const
{
	switch (type)
	{
	case Settings::ELightType::POINT:
	case Settings::ELightType::SPOT: return CalculatePointLightRadius(constant, linear, quadratic, intensity);
	case Settings::ELightType::AMBIENT_BOX: return CalculateAmbientBoxLightRadius(transform.GetWorldPosition(), {constant, linear, quadratic});
	case Settings::ELightType::AMBIENT_SPHERE: return constant;
	}

	return std::numeric_limits<float>::infinity();
}

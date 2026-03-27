#pragma once

#include <stdint.h>

namespace NLS::Render::Resources
{
	/**
	* Defines the types that a uniform can take
	*/
	enum class UniformType : uint32_t
	{
		UNIFORM_BOOL = 0,
		UNIFORM_INT,
		UNIFORM_FLOAT,
		UNIFORM_FLOAT_VEC2,
		UNIFORM_FLOAT_VEC3,
		UNIFORM_FLOAT_VEC4,
		UNIFORM_FLOAT_MAT4,
		UNIFORM_DOUBLE_MAT4,
		UNIFORM_SAMPLER_2D,
		UNIFORM_SAMPLER_CUBE
	};
}

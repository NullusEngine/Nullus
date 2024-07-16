#pragma once

#include <string>
#include <any>

#include "Rendering/Resources/UniformType.h"

namespace NLS::Render::Resources
{
	/**
	* Data structure containing information about a uniform
	*/
	struct UniformInfo
	{
		UniformType		type;
		std::string		name;
		uint32_t		location;
		std::any		defaultValue;
	};
}
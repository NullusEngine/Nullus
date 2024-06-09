#pragma once

#include <cstdint>

namespace NLS::Rendering::Settings
{
	/**
	* Enumeration of cullable face
	*/
	enum class ECullFace : uint8_t
	{
		FRONT,
		BACK,
		FRONT_AND_BACK
	};
}
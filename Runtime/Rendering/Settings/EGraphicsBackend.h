#pragma once

#include <cstdint>

namespace NLS::Rendering::Settings
{
	/**
	* Enumerate graphics backend implementations
	*/
	enum class EGraphicsBackend : uint8_t
	{
		NONE,
		OPENGL,
		VULKAN
	};
}
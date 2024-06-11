#pragma once

#include <cstdint>
#include "RenderDef.h"
namespace NLS::Rendering::Settings
{
	/**
	* Represents some culling options
	*/
	struct NLS_RENDER_API ERenderPassOrder
	{
		using PassType = uint32_t;

		static constexpr PassType First				= 00000U;
		static constexpr PassType Opaque			= 10000U;
		static constexpr PassType Transparent		= 20000U;
		static constexpr PassType PostProcessing	= 30000U;
		static constexpr PassType UI				= 40000U;
		static constexpr PassType Last				= ~First;
	};
}
#pragma once

#include <cstdint>

namespace NLS::Render::Backend
{
	constexpr uint32_t ResolveDX12PresentSyncInterval(bool vsync)
	{
		return vsync ? 1u : 0u;
	}
}

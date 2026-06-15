#pragma once

#include <cstdint>

namespace NLS::Render::Backend
{
	constexpr uint32_t ResolveDX12PresentSyncInterval(bool vsync)
	{
		return vsync ? 1u : 0u;
	}

	constexpr uint32_t ResolveDX12PresentFlags(bool vsync, bool allowTearing)
	{
		constexpr uint32_t kDxgiPresentAllowTearing = 0x00000200u;
		return !vsync && allowTearing ? kDxgiPresentAllowTearing : 0u;
	}

	constexpr uint32_t ResolveDX12SwapchainFlags(bool allowTearing)
	{
		constexpr uint32_t kDxgiSwapChainFlagAllowTearing = 0x00000800u;
		return allowTearing ? kDxgiSwapChainFlagAllowTearing : 0u;
	}
}

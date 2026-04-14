#pragma once

#include <chrono>

namespace NLS::Render::Context
{
inline constexpr std::chrono::milliseconds GetInteractiveSwapchainResizeDebounce()
{
    return std::chrono::milliseconds(0);
}

inline bool ShouldApplyPendingSwapchainResize(
    const std::chrono::steady_clock::duration elapsedSinceLastRequest,
    const std::chrono::steady_clock::duration debounce = GetInteractiveSwapchainResizeDebounce())
{
    return elapsedSinceLastRequest >= debounce;
}
} // namespace NLS::Render::Context

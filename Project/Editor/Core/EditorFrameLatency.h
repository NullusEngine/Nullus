#pragma once

#include <cstdint>

namespace NLS::Editor::Core
{
	/**
	 * Resource headroom for Scene View, Game View, UI and preview submissions.
	 * Allocators remain owned until their GPU fences complete. This capacity
	 * absorbs GPU work in flight; it must not increase the CPU command queue depth.
	 * Context sets cpuFrameLatencyLimit separately, and Driver reserves admission
	 * capacity for the swapchain consumer.
	 */
	constexpr uint32_t kEditorConcurrentViewFrameHeadroom = 14u;

	constexpr uint32_t ResolveEditorFrameResourceCount(uint32_t framesInFlight)
	{
		return framesInFlight > 0u
			? framesInFlight + kEditorConcurrentViewFrameHeadroom
			: 1u;
	}
}

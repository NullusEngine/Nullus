#pragma once

#include <cstdint>

namespace NLS::Editor::Core
{
	constexpr uint32_t ResolveEditorThreadedFrameSlotCount(uint32_t framesInFlight)
	{
		return framesInFlight > 0u ? framesInFlight : 1u;
	}
}

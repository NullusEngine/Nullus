#pragma once

#include <cstdint>

namespace NLS::Editor::Core
{
	constexpr uint32_t ResolveEditorThreadedFrameSlotCount(uint32_t framesInFlight)
	{
		(void)framesInFlight;
		return 1u;
	}
}

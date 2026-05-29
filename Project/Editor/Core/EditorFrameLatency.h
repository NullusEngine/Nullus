#pragma once

#include <cstdint>

namespace NLS::Editor::Core
{
	constexpr uint32_t ResolveEditorThreadedFrameSlotCount(uint32_t framesInFlight)
	{
		return framesInFlight > 0u ? framesInFlight + 1u : 1u;
	}

	constexpr uint32_t ResolveEditorThreadedPublishRetirementWaitMs()
	{
		// Keep the editor wait bounded below a 60 Hz frame while allowing a retired slot to be observed.
		return 8u;
	}
}

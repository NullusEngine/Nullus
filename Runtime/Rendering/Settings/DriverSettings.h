#pragma once

#include <optional>

#include "Rendering/Data/PipelineState.h"

namespace Rendering::Settings
{
	/**
	* Settings that are sent to the driver at construction
	*/
	struct DriverSettings
	{
		bool debugMode = false;
		std::optional<Rendering::Data::PipelineState> defaultPipelineState = std::nullopt;
	};
}
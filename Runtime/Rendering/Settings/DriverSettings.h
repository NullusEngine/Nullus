#pragma once

#include <optional>

#include "Rendering/Data/PipelineState.h"
#include "RenderDef.h"
namespace NLS::Render::Settings
{
	/**
	* Settings that are sent to the driver at construction
	*/
	struct NLS_RENDER_API DriverSettings
	{
		bool debugMode = false;
		std::optional<NLS::Render::Data::PipelineState> defaultPipelineState = std::nullopt;
	};
}
#pragma once

#include <cstdint>
#include <optional>

#include "Rendering/Data/PipelineState.h"
#include "Rendering/Settings/EngineDiagnosticsSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "RenderDef.h"
namespace NLS::Render::Settings
{
	/**
	* Settings that are sent to the driver at construction
	*/
	struct NLS_RENDER_API DriverSettings
	{
		EGraphicsBackend graphicsBackend =
#if defined(_WIN32)
			EGraphicsBackend::DX12;
#elif defined(__APPLE__)
			EGraphicsBackend::METAL;
#elif defined(__linux__)
			EGraphicsBackend::VULKAN;
#else
			EGraphicsBackend::OPENGL;
#endif
		bool debugMode = false;
		uint32_t framesInFlight = 2;
		bool enableExplicitRHI = true;
		bool enableThreadedRendering = false;
		bool enableLightGrid = true;
		uint32_t threadedFrameSlotCount = 0;
		uint32_t threadedPublishRetirementWaitMs = 0;
		RenderDocSettings renderDoc{};
		EngineDiagnosticsSettings diagnostics{};
		std::optional<NLS::Render::Data::PipelineState> defaultPipelineState = std::nullopt;
	};
}

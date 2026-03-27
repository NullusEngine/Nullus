#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "Rendering/Data/PipelineState.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "RenderDef.h"
namespace NLS::Render::Settings
{
	struct NLS_RENDER_API RenderDocSettings
	{
		bool enabled = false;
		bool autoOpenReplayUI = false;
		uint32_t startupCaptureAfterFrames = 0;
		std::string captureDirectory;
		std::string captureLabel;
	};

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
		RenderDocSettings renderDoc{};
		std::optional<NLS::Render::Data::PipelineState> defaultPipelineState = std::nullopt;
	};
}

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

	struct NLS_RENDER_API EngineDiagnosticsSettings
	{
		bool logRenderDrawPath = false;
		bool diagSkipSkyboxDraw = false;
		bool logMaterialBindings = false;
		bool dx12LogMessages = false;
		bool dx12LogFrameFlow = false;
		bool editorGridSkipPlane = false;
		bool editorGridSkipAxes = false;
		bool editorDisableGridPass = false;
		bool editorDisableDebugCamerasPass = false;
		bool editorDisableDebugLightsPass = false;
		bool editorDisableDebugActorPass = false;
		bool editorDisableDebugDrawPass = false;
		bool editorDisablePickingPass = false;
		std::string editorValidationFocusView;
		std::string editorValidationExclusiveView;
		std::string editorValidationSelectActor;
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
		bool enableThreadedRendering = false;
		uint32_t threadedFrameSlotCount = 0;
		uint32_t threadedPublishRetirementWaitMs = 0;
		RenderDocSettings renderDoc{};
		EngineDiagnosticsSettings diagnostics{};
		std::optional<NLS::Render::Data::PipelineState> defaultPipelineState = std::nullopt;
	};
}

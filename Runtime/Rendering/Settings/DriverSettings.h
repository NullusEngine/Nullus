#pragma once

#include <cstdint>
#include <optional>

#include "Rendering/Data/PipelineState.h"
#include "Rendering/Settings/EngineDiagnosticsSettings.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Rendering/Settings/RhiThreadMode.h"
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
		bool debugMode =
#if defined(_DEBUG)
			true;
#else
			false;
#endif
		uint32_t framesInFlight = 2;
		// Zero uses the resource-ring size; consumers can cap CPU queue depth independently.
		uint32_t cpuFrameLatencyLimit = 0u;
		bool enableExplicitRHI = true;
		bool enableLightGrid = true;
		// The render thread is always dedicated; this selects the logical RHI lane.
		// Restart-scoped: changing it requires recreating the Driver.
		RhiThreadModePreference rhiThreadMode = RhiThreadModePreference::BackendDefault;
		// Non-owning platform window used by WSI backends (for example Vulkan/GLFW).
		// The application owns the window and keeps it alive for the Driver lifetime.
		void* platformWindow = nullptr;
		RenderDocSettings renderDoc{};
		EngineDiagnosticsSettings diagnostics{};
		std::optional<NLS::Render::Data::PipelineState> defaultPipelineState = std::nullopt;
	};
}

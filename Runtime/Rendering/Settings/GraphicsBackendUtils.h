#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "Rendering/RHI/RHITypes.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Rendering/Settings/DriverSettings.h"

#ifndef NLS_HAS_IMGUI_DX12_BACKEND
#define NLS_HAS_IMGUI_DX12_BACKEND 0
#endif

#ifndef NLS_HAS_IMGUI_DX11_BACKEND
#define NLS_HAS_IMGUI_DX11_BACKEND 0
#endif

#ifndef NLS_HAS_IMGUI_VULKAN_BACKEND
#define NLS_HAS_IMGUI_VULKAN_BACKEND 0
#endif

namespace NLS::Render::Settings
{
	struct RuntimeBackendReadinessDecision
	{
		std::optional<std::string> primaryWarning;
		std::optional<std::string> detailWarning;
	};

	inline std::string NormalizeGraphicsBackendName(std::string_view value)
	{
		std::string normalized(value);
		std::transform(normalized.begin(), normalized.end(), normalized.begin(),
			[](unsigned char ch)
			{
				return static_cast<char>(std::tolower(ch));
			});
		return normalized;
	}

	inline bool IsTruthyEnvironmentValue(const char* value)
	{
		if (value == nullptr || value[0] == '\0')
			return false;

		return std::strcmp(value, "1") == 0 || NormalizeGraphicsBackendName(value) == "true";
	}

	inline bool IsEnvironmentFlagEnabled(const char* variableName)
	{
		if (variableName == nullptr || variableName[0] == '\0')
			return false;

		return IsTruthyEnvironmentValue(std::getenv(variableName));
	}

	inline std::optional<EGraphicsBackend> TryParseGraphicsBackend(std::string_view value)
	{
		const auto normalized = NormalizeGraphicsBackendName(value);
		if (normalized == "opengl")
			return EGraphicsBackend::OPENGL;
		if (normalized == "vulkan")
			return EGraphicsBackend::VULKAN;
		if (normalized == "dx12" || normalized == "directx12" || normalized == "d3d12")
			return EGraphicsBackend::DX12;
		if (normalized == "dx11" || normalized == "directx11" || normalized == "d3d11")
			return EGraphicsBackend::DX11;
		if (normalized == "metal")
			return EGraphicsBackend::METAL;
		if (normalized == "none" || normalized == "null")
			return EGraphicsBackend::NONE;
		return std::nullopt;
	}

	inline EGraphicsBackend GetPlatformDefaultGraphicsBackend()
	{
#if defined(_WIN32)
		return EGraphicsBackend::DX12;
#elif defined(__APPLE__)
		return EGraphicsBackend::METAL;
#elif defined(__linux__)
		return EGraphicsBackend::VULKAN;
#else
		return EGraphicsBackend::OPENGL;
#endif
	}

	inline EGraphicsBackend ParseGraphicsBackendOrDefault(std::string_view value, EGraphicsBackend fallback = GetPlatformDefaultGraphicsBackend())
	{
		if (const auto parsed = TryParseGraphicsBackend(value); parsed.has_value())
			return parsed.value();

		return fallback;
	}

	inline const char* ToString(EGraphicsBackend backend)
	{
		switch (backend)
		{
		case EGraphicsBackend::VULKAN: return "Vulkan";
		case EGraphicsBackend::DX12: return "DX12";
		case EGraphicsBackend::DX11: return "DX11";
		case EGraphicsBackend::METAL: return "Metal";
		case EGraphicsBackend::NONE: return "None";
		case EGraphicsBackend::OPENGL:
		default:
			return "OpenGL";
		}
	}

	inline const char* ToString(const NLS::Render::RHI::NativeBackendType backend)
	{
		switch (backend)
		{
		case NLS::Render::RHI::NativeBackendType::Vulkan: return "Vulkan";
		case NLS::Render::RHI::NativeBackendType::DX12: return "DX12";
		case NLS::Render::RHI::NativeBackendType::DX11: return "DX11";
		case NLS::Render::RHI::NativeBackendType::Metal: return "Metal";
		case NLS::Render::RHI::NativeBackendType::OpenGL: return "OpenGL";
		case NLS::Render::RHI::NativeBackendType::None:
		default:
			return "Unknown";
		}
	}

	inline EGraphicsBackend ToGraphicsBackend(const NLS::Render::RHI::NativeBackendType backend)
	{
		switch (backend)
		{
		case NLS::Render::RHI::NativeBackendType::DX11: return EGraphicsBackend::DX11;
		case NLS::Render::RHI::NativeBackendType::DX12: return EGraphicsBackend::DX12;
		case NLS::Render::RHI::NativeBackendType::Vulkan: return EGraphicsBackend::VULKAN;
		case NLS::Render::RHI::NativeBackendType::OpenGL: return EGraphicsBackend::OPENGL;
		case NLS::Render::RHI::NativeBackendType::Metal: return EGraphicsBackend::METAL;
		case NLS::Render::RHI::NativeBackendType::None:
		default:
			return EGraphicsBackend::NONE;
		}
	}

	inline EGraphicsBackend GetPhase1RequiredRuntimeBackend()
	{
		return EGraphicsBackend::DX12;
	}

	inline bool IsPhase1RuntimeBackend(EGraphicsBackend backend)
	{
		return backend == GetPhase1RequiredRuntimeBackend();
	}

	inline bool IsBackendSelectableForPhase1(EGraphicsBackend backend)
	{
#if defined(_WIN32)
		return IsPhase1RuntimeBackend(backend);
#else
		(void)backend;
		return false;
#endif
	}

	inline std::optional<std::string> GetPhase1BackendRestrictionMessage(
		EGraphicsBackend backend,
		std::string_view consumer)
	{
		if (IsBackendSelectableForPhase1(backend))
			return std::nullopt;

		std::string message;
		if (!consumer.empty())
		{
			message += std::string(consumer);
			message += ' ';
		}
		message += "only supports DX12 during UE5 alignment phase 1.";

		if (backend != EGraphicsBackend::NONE)
		{
			message += " Requested backend: ";
			message += ToString(backend);
			message += '.';
		}

		return message;
	}

	inline bool IsBackendEnabledForCurrentBuild(EGraphicsBackend backend)
	{
		return IsBackendSelectableForPhase1(backend);
	}

	inline bool HasCompiledOfficialImGuiBackend(EGraphicsBackend backend)
	{
		switch (backend)
		{
		case EGraphicsBackend::OPENGL:
			return true;
		case EGraphicsBackend::DX12:
#if defined(_WIN32) && NLS_HAS_IMGUI_DX12_BACKEND
			return true;
#else
			return false;
#endif
		case EGraphicsBackend::DX11:
#if defined(_WIN32) && NLS_HAS_IMGUI_DX11_BACKEND
			return true;
#else
			return false;
#endif
		case EGraphicsBackend::VULKAN:
#if NLS_HAS_IMGUI_VULKAN_BACKEND
			return true;
#else
			return false;
#endif
		case EGraphicsBackend::METAL:
			return false;
		case EGraphicsBackend::NONE:
		default:
			return false;
		}
	}

	inline bool SupportsImGuiRendererBackend(EGraphicsBackend backend)
	{
		if (!IsBackendEnabledForCurrentBuild(backend))
			return false;

		switch (backend)
		{
		case EGraphicsBackend::OPENGL:
			return true;
		case EGraphicsBackend::DX12:
#if defined(_WIN32) && NLS_HAS_IMGUI_DX12_BACKEND
			return true;
#else
			return false;
#endif
		case EGraphicsBackend::DX11:
#if defined(_WIN32) && NLS_HAS_IMGUI_DX11_BACKEND
			return true;
#else
			return false;
#endif
		case EGraphicsBackend::VULKAN:
#if NLS_HAS_IMGUI_VULKAN_BACKEND
			return true;
#else
			return false;
#endif
		case EGraphicsBackend::METAL:
			return false;
		case EGraphicsBackend::NONE:
		default:
			return false;
		}
	}

	inline const char* SceneRendererSupportDescription(EGraphicsBackend backend)
	{
		switch (backend)
		{
		case EGraphicsBackend::OPENGL:
			return "OpenGL is disabled for the UE5 alignment phase-1 runtime because the accepted mainline only permits DX12.";
		case EGraphicsBackend::DX12:
			return "DX12 is the only active runtime backend for the UE5 alignment phase-1 mainline.";
		case EGraphicsBackend::DX11:
			return "DX11 is disabled for the UE5 alignment phase-1 runtime because the accepted mainline only permits DX12.";
		case EGraphicsBackend::VULKAN:
			return "Vulkan architecture boundaries remain in source for future multi-backend work, but the phase-1 runtime path is intentionally DX12-only.";
		case EGraphicsBackend::METAL:
			return "Metal is disabled for the UE5 alignment phase-1 runtime because the accepted mainline only permits DX12.";
		case EGraphicsBackend::NONE:
		default:
			return "This backend does not provide a runnable scene renderer.";
		}
	}

	inline bool SupportsEditorMainRuntime(const NLS::Render::RHI::RHIDeviceCapabilities& capabilities)
	{
		return capabilities.backendReady &&
			capabilities.supportsCurrentSceneRenderer &&
			capabilities.supportsOffscreenFramebuffers &&
			capabilities.supportsUITextureHandles &&
			capabilities.supportsDepthBlit &&
			capabilities.supportsCubemaps;
	}

	inline bool SupportsGameMainRuntime(const NLS::Render::RHI::RHIDeviceCapabilities& capabilities)
	{
		return capabilities.backendReady &&
			capabilities.supportsCurrentSceneRenderer &&
			capabilities.supportsSwapchain;
	}

	inline bool SupportsTierARenderFoundation(const NLS::Render::RHI::RHIDeviceCapabilities& capabilities)
	{
		return capabilities.backendReady &&
			capabilities.supportsGraphics &&
			capabilities.supportsCompute &&
			capabilities.supportsSwapchain &&
			capabilities.supportsCurrentSceneRenderer &&
			capabilities.supportsOffscreenFramebuffers &&
			capabilities.supportsMultiRenderTargets &&
			capabilities.supportsExplicitBarriers &&
			capabilities.supportsCentralizedDescriptorManagement &&
			capabilities.supportsPipelineStateCache;
	}

	inline bool SupportsRenderGraphTransientResources(const NLS::Render::RHI::RHIDeviceCapabilities& capabilities)
	{
		return SupportsTierARenderFoundation(capabilities) &&
			capabilities.supportsTransientResourceAllocator;
	}

	// Thread-local diagnostics settings - set when Driver is created, read by subsystems without driver access
	inline thread_local Render::Settings::EngineDiagnosticsSettings g_threadDiagnosticsSettings;

	inline const Render::Settings::EngineDiagnosticsSettings& GetThreadDiagnosticsSettings()
	{
		return g_threadDiagnosticsSettings;
	}

	inline void SetThreadDiagnosticsSettings(const Render::Settings::EngineDiagnosticsSettings& settings)
	{
		g_threadDiagnosticsSettings = settings;
	}


	inline bool SupportsAsyncComputeFoundation(const NLS::Render::RHI::RHIDeviceCapabilities& capabilities)
	{
		return SupportsTierARenderFoundation(capabilities) &&
			capabilities.supportsAsyncCompute &&
			capabilities.supportsDedicatedComputeQueue;
	}

	inline bool SupportsParallelCommandFoundation(const NLS::Render::RHI::RHIDeviceCapabilities& capabilities)
	{
		return SupportsTierARenderFoundation(capabilities) &&
			capabilities.supportsParallelCommandRecording &&
			capabilities.supportsParallelCommandTranslation;
	}

	inline bool SupportsOrderedParallelCommandSubmissionPath(
		const NLS::Render::RHI::NativeBackendType backend,
		const NLS::Render::RHI::RHIDeviceCapabilities& capabilities)
	{
		switch (backend)
		{
		case NLS::Render::RHI::NativeBackendType::DX12:
			return SupportsTierARenderFoundation(capabilities);
		case NLS::Render::RHI::NativeBackendType::Vulkan:
		case NLS::Render::RHI::NativeBackendType::None:
		case NLS::Render::RHI::NativeBackendType::DX11:
		case NLS::Render::RHI::NativeBackendType::OpenGL:
		case NLS::Render::RHI::NativeBackendType::Metal:
		default:
			return false;
		}
	}

	inline bool SupportsParallelCommandReadyPath(
		const NLS::Render::RHI::NativeBackendType backend,
		const NLS::Render::RHI::RHIDeviceCapabilities& capabilities)
	{
		return SupportsOrderedParallelCommandSubmissionPath(backend, capabilities) &&
			SupportsParallelCommandFoundation(capabilities);
	}

	inline bool SupportsThreadedRenderFoundationPath(
		const NLS::Render::RHI::NativeBackendType backend,
		const NLS::Render::RHI::RHIDeviceCapabilities& capabilities)
	{
		switch (backend)
		{
		case NLS::Render::RHI::NativeBackendType::DX12:
			return SupportsTierARenderFoundation(capabilities);
		case NLS::Render::RHI::NativeBackendType::Vulkan:
		case NLS::Render::RHI::NativeBackendType::None:
		case NLS::Render::RHI::NativeBackendType::DX11:
		case NLS::Render::RHI::NativeBackendType::OpenGL:
		case NLS::Render::RHI::NativeBackendType::Metal:
		default:
			return false;
		}
	}

	inline RuntimeBackendReadinessDecision EvaluateEditorMainRuntimeReadiness(
		const EGraphicsBackend requestedBackend,
		const NLS::Render::RHI::RHIDeviceCapabilities& capabilities)
	{
		if (const auto restriction = GetPhase1BackendRestrictionMessage(requestedBackend, "Editor runtime");
			restriction.has_value())
		{
			RuntimeBackendReadinessDecision decision;
			decision.primaryWarning = restriction;
			decision.detailWarning = SceneRendererSupportDescription(requestedBackend);
			return decision;
		}

		if (!IsBackendEnabledForCurrentBuild(requestedBackend))
		{
			RuntimeBackendReadinessDecision decision;
			decision.primaryWarning =
				"Selected editor backend " +
				std::string(ToString(requestedBackend)) +
				" is unsupported in the current runtime validation matrix.";
			decision.detailWarning = SceneRendererSupportDescription(requestedBackend);
			return decision;
		}

		if (SupportsEditorMainRuntime(capabilities))
			return {};

		RuntimeBackendReadinessDecision decision;
		if (!capabilities.backendReady)
		{
			decision.primaryWarning =
				"Selected editor backend " +
				std::string(ToString(requestedBackend)) +
				" is not ready for the accepted phase-1 runtime startup path.";
			decision.detailWarning = SceneRendererSupportDescription(requestedBackend);
			return decision;
		}

		decision.primaryWarning =
			"Editor runtime still requires native scene rendering, offscreen framebuffer, UI texture, depth blit, and cubemap support before startup can continue on " +
			std::string(ToString(requestedBackend)) +
			".";
		decision.detailWarning = SceneRendererSupportDescription(requestedBackend);
		return decision;
	}

	inline RuntimeBackendReadinessDecision EvaluateGameMainRuntimeReadiness(
		const EGraphicsBackend requestedBackend,
		const NLS::Render::RHI::RHIDeviceCapabilities& capabilities)
	{
		if (const auto restriction = GetPhase1BackendRestrictionMessage(requestedBackend, "Game runtime");
			restriction.has_value())
		{
			RuntimeBackendReadinessDecision decision;
			decision.primaryWarning = restriction;
			decision.detailWarning = SceneRendererSupportDescription(requestedBackend);
			return decision;
		}

		if (!IsBackendEnabledForCurrentBuild(requestedBackend))
		{
			RuntimeBackendReadinessDecision decision;
			decision.primaryWarning =
				"Requested game backend " +
				std::string(ToString(requestedBackend)) +
				" is unsupported in the current runtime validation matrix.";
			decision.detailWarning = SceneRendererSupportDescription(requestedBackend);
			return decision;
		}

		if (SupportsGameMainRuntime(capabilities))
			return {};

		RuntimeBackendReadinessDecision decision;
		if (!capabilities.backendReady)
		{
			decision.primaryWarning =
				"Requested game backend " +
				std::string(ToString(requestedBackend)) +
				" is not ready for the accepted phase-1 runtime startup path.";
			decision.detailWarning = SceneRendererSupportDescription(requestedBackend);
			return decision;
		}

		decision.primaryWarning =
			"Game scene rendering requires a validated backend before startup can continue on " +
			std::string(ToString(requestedBackend)) +
			".";
		decision.detailWarning = SceneRendererSupportDescription(requestedBackend);
		return decision;
	}

	inline bool SupportsEditorPickingReadback(const NLS::Render::RHI::RHIDeviceCapabilities& capabilities)
	{
		return capabilities.supportsEditorPickingReadback;
	}

	inline std::optional<std::string> GetEditorPickingReadbackWarning(const NLS::Render::RHI::RHIDeviceCapabilities& capabilities)
	{
		if (SupportsEditorPickingReadback(capabilities))
			return std::nullopt;

		return std::string(
			"Scene view picking readback is unavailable on this backend. "
			"Scene view hover picking, click selection, and gizmo hit testing will be disabled.");
	}

	inline std::optional<EGraphicsBackend> TryReadGraphicsBackendFromEnvironment(const char* variableName)
	{
		if (variableName == nullptr)
			return std::nullopt;

		if (const char* value = std::getenv(variableName); value != nullptr && value[0] != '\0')
			return TryParseGraphicsBackend(value);

		return std::nullopt;
	}
}

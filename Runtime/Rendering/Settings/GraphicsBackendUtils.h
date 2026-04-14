#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "Rendering/RHI/RHITypes.h"
#include "Rendering/Settings/EGraphicsBackend.h"

#ifndef NLS_HAS_IMGUI_DX12_BACKEND
#define NLS_HAS_IMGUI_DX12_BACKEND 0
#endif

#ifndef NLS_HAS_IMGUI_VULKAN_BACKEND
#define NLS_HAS_IMGUI_VULKAN_BACKEND 0
#endif

namespace NLS::Render::Settings
{
	struct RuntimeBackendFallbackDecision
	{
		bool shouldFallbackToOpenGL = false;
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

	inline bool IsBackendEnabledForCurrentBuild(EGraphicsBackend backend)
	{
#if defined(_WIN32)
		switch (backend)
		{
		case EGraphicsBackend::DX12:
			return true;
		case EGraphicsBackend::VULKAN:
#if NLS_HAS_VULKAN
			return true;
#else
			return false;
#endif
		case EGraphicsBackend::OPENGL:
		case EGraphicsBackend::DX11:
		case EGraphicsBackend::METAL:
		case EGraphicsBackend::NONE:
		default:
			return false;
		}
#else
		switch (backend)
		{
		case EGraphicsBackend::VULKAN:
#if NLS_HAS_VULKAN
			return true;
#else
			return false;
#endif
		case EGraphicsBackend::METAL:
#if NLS_HAS_METAL
			return true;
#else
			return false;
#endif
		case EGraphicsBackend::OPENGL:
			return true;
		case EGraphicsBackend::DX12:
		case EGraphicsBackend::DX11:
		case EGraphicsBackend::NONE:
		default:
			return false;
		}
#endif
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
			return false;
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
			return false;
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
#if defined(_WIN32)
			return "OpenGL is unsupported in the current Windows runtime validation matrix because startup smoke still crashes before the backend reaches a stable first frame.";
#else
			return "OpenGL support requires platform-specific validation before it should be exposed as a supported runtime backend.";
#endif
		case EGraphicsBackend::DX12:
			return "Runtime scene rendering and editor offscreen framebuffers are available via formal RHI mainline, but scene view picking readback still uses an unfinished compatibility path.";
		case EGraphicsBackend::DX11:
#if defined(_WIN32)
			return "DX11 is unsupported in the current Windows runtime validation matrix until sampler, binding layout, binding set, and pipeline layout support are implemented and revalidated.";
#else
			return "DX11 support is only meaningful on Windows and is not exposed as a supported runtime backend here.";
#endif
		case EGraphicsBackend::VULKAN:
			return "Runtime scene rendering and editor offscreen framebuffers are available via formal RHI mainline, but scene view picking readback still uses an unfinished compatibility path.";
		case EGraphicsBackend::METAL:
#if defined(__APPLE__)
			return "Metal requires additional Apple-native presentation and editor validation before it should be exposed as a supported runtime backend.";
#else
			return "Metal is unsupported on this non-Apple build/platform.";
#endif
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

	inline RuntimeBackendFallbackDecision EvaluateEditorMainRuntimeFallback(
		const EGraphicsBackend requestedBackend,
		const NLS::Render::RHI::RHIDeviceCapabilities& capabilities)
	{
		if (!IsBackendEnabledForCurrentBuild(requestedBackend))
		{
			RuntimeBackendFallbackDecision decision;
			decision.primaryWarning =
				"Selected editor backend " +
				std::string(ToString(requestedBackend)) +
				" is unsupported in the current runtime validation matrix.";
			decision.detailWarning = SceneRendererSupportDescription(requestedBackend);
			return decision;
		}

		if (SupportsEditorMainRuntime(capabilities))
			return {};

		RuntimeBackendFallbackDecision decision;
		if (!capabilities.backendReady)
		{
			decision.primaryWarning =
				"Selected editor backend " +
				std::string(ToString(requestedBackend)) +
				" is not ready, and no validated fallback backend is currently available.";
			decision.detailWarning = SceneRendererSupportDescription(requestedBackend);
			return decision;
		}

		decision.primaryWarning =
			"Editor runtime still requires native scene rendering, offscreen framebuffer, UI texture, depth blit, and cubemap support. no validated fallback backend is currently available for " +
			std::string(ToString(requestedBackend)) +
			".";
		decision.detailWarning = SceneRendererSupportDescription(requestedBackend);
		return decision;
	}

	inline RuntimeBackendFallbackDecision EvaluateGameMainRuntimeFallback(
		const EGraphicsBackend requestedBackend,
		const NLS::Render::RHI::RHIDeviceCapabilities& capabilities)
	{
		if (!IsBackendEnabledForCurrentBuild(requestedBackend))
		{
			RuntimeBackendFallbackDecision decision;
			decision.primaryWarning =
				"Requested game backend " +
				std::string(ToString(requestedBackend)) +
				" is unsupported in the current runtime validation matrix.";
			decision.detailWarning = SceneRendererSupportDescription(requestedBackend);
			return decision;
		}

		if (SupportsGameMainRuntime(capabilities))
			return {};

		RuntimeBackendFallbackDecision decision;
		if (!capabilities.backendReady)
		{
			decision.primaryWarning =
				"Requested game backend " +
				std::string(ToString(requestedBackend)) +
				" is not ready, and no validated fallback backend is currently available.";
			decision.detailWarning = SceneRendererSupportDescription(requestedBackend);
			return decision;
		}

		decision.primaryWarning =
			"Game scene rendering requires a validated backend. no validated fallback backend is currently available for " +
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

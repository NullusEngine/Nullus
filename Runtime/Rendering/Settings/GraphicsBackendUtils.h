#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "Rendering/Settings/EGraphicsBackend.h"

#ifndef NLS_HAS_IMGUI_DX12_BACKEND
#define NLS_HAS_IMGUI_DX12_BACKEND 0
#endif

#ifndef NLS_HAS_IMGUI_VULKAN_BACKEND
#define NLS_HAS_IMGUI_VULKAN_BACKEND 0
#endif

namespace NLS::Render::Settings
{
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
		case EGraphicsBackend::METAL: return "Metal";
		case EGraphicsBackend::NONE: return "None";
		case EGraphicsBackend::OPENGL:
		default:
			return "OpenGL";
		}
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
			return "Current scene renderer is implemented on this backend.";
		case EGraphicsBackend::DX12:
			return "Runtime scene submission is available, but editor offscreen framebuffers/readback are not DX12-native yet.";
		case EGraphicsBackend::VULKAN:
			return "Runtime scene submission, editor offscreen framebuffers, and framebuffer readback are available on this backend.";
		case EGraphicsBackend::METAL:
			return "Metal is now a first-class backend target, but Apple-native presentation and editor parity still depend on platform-specific runtime work.";
		case EGraphicsBackend::NONE:
		default:
			return "This backend does not provide a runnable scene renderer.";
		}
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

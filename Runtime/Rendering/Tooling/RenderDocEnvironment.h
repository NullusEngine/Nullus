#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "Rendering/Settings/DriverSettings.h"

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace NLS::Render::Tooling
{
	inline std::optional<std::string> TryReadEnvironmentString(const char* variableName)
	{
		if (variableName == nullptr)
			return std::nullopt;

		if (const char* value = std::getenv(variableName); value != nullptr && value[0] != '\0')
			return std::string(value);

		return std::nullopt;
	}

	inline bool HasRenderDocEnvironmentRequest()
	{
		for (const char* variableName : {
			"NLS_RENDERDOC_ENABLE",
			"NLS_RENDERDOC_CAPTURE",
			"NLS_RENDERDOC_CAPTURE_AFTER_FRAMES",
			"NLS_RENDERDOC_CAPTURE_DIR",
			"NLS_RENDERDOC_CAPTURE_LABEL",
			"NLS_RENDERDOC_AUTO_OPEN" })
		{
			if (TryReadEnvironmentString(variableName).has_value())
				return true;
		}

		return false;
	}

	inline bool IsTruthyEnvironmentValue(std::string_view value)
	{
		std::string normalized(value);
		std::transform(normalized.begin(), normalized.end(), normalized.begin(),
			[](unsigned char character)
			{
				return static_cast<char>(std::tolower(character));
			});

		return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
	}

	inline std::optional<uint32_t> TryReadEnvironmentUInt(const char* variableName)
	{
		const auto value = TryReadEnvironmentString(variableName);
		if (!value.has_value())
			return std::nullopt;

		try
		{
			const auto parsed = std::stoul(value.value());
			return static_cast<uint32_t>(parsed);
		}
		catch (...)
		{
			return std::nullopt;
		}
	}

	inline void ApplyRenderDocEnvironmentOverrides(
		Settings::RenderDocSettings& settings,
		std::string_view defaultCaptureDirectory,
		std::string_view defaultLabel)
	{
		if (const auto enabled = TryReadEnvironmentString("NLS_RENDERDOC_ENABLE"); enabled.has_value())
			settings.enabled = IsTruthyEnvironmentValue(enabled.value());
		else if (HasRenderDocEnvironmentRequest())
			settings.enabled = true;

		if (settings.captureDirectory.empty())
			settings.captureDirectory = std::string(defaultCaptureDirectory);

		if (settings.captureLabel.empty())
			settings.captureLabel = std::string(defaultLabel);

		if (const auto captureDirectory = TryReadEnvironmentString("NLS_RENDERDOC_CAPTURE_DIR"); captureDirectory.has_value())
			settings.captureDirectory = captureDirectory.value();

		if (const auto captureLabel = TryReadEnvironmentString("NLS_RENDERDOC_CAPTURE_LABEL"); captureLabel.has_value())
			settings.captureLabel = captureLabel.value();

		if (const auto autoOpen = TryReadEnvironmentString("NLS_RENDERDOC_AUTO_OPEN"); autoOpen.has_value())
			settings.autoOpenReplayUI = IsTruthyEnvironmentValue(autoOpen.value());

		const auto startupCaptureAfterFrames = TryReadEnvironmentUInt("NLS_RENDERDOC_CAPTURE_AFTER_FRAMES");
		if (startupCaptureAfterFrames.has_value())
			settings.startupCaptureAfterFrames = startupCaptureAfterFrames.value();

		if (const auto captureOnStartup = TryReadEnvironmentString("NLS_RENDERDOC_CAPTURE"); captureOnStartup.has_value())
		{
			if (IsTruthyEnvironmentValue(captureOnStartup.value()))
			{
				settings.enabled = true;
				if (settings.startupCaptureAfterFrames == 0)
					settings.startupCaptureAfterFrames = 120;
			}
			else
			{
				settings.startupCaptureAfterFrames = 0;
			}
		}
	}

	inline bool PreloadRenderDocIfAvailable(const Settings::RenderDocSettings& settings)
	{
#if defined(_WIN32)
		// Check if RenderDoc Vulkan layer is already loaded (implicit layer from registry)
		// This means the layer will handle captures with its own configuration
		HMODULE existingModule = ::GetModuleHandleW(L"renderdoc.dll");
		if (existingModule != nullptr)
		{
			NLS_LOG_INFO("PreloadRenderDocIfAvailable: renderdoc.dll already loaded (Vulkan layer detected)");
			NLS_LOG_INFO("PreloadRenderDocIfAvailable: Note: Vulkan layer uses its own capture keys and directory settings");
			NLS_LOG_INFO("PreloadRenderDocIfAvailable: For programmatic control, either:");
			NLS_LOG_INFO("  1. Configure capture keys and directory in RenderDoc UI before running, OR");
			NLS_LOG_INFO("  2. Use the renderdoc_runner.py script which sets up environment correctly");
			// Return true - we can still use the layer's API for triggering captures
			return true;
		}

		// Do not actively load RenderDoc when capture tooling is not enabled.
		// This prevents default RenderDoc hotkeys/overlay (F12) from appearing in normal editor/game runs.
		if (!settings.enabled)
		{
			NLS_LOG_INFO("PreloadRenderDocIfAvailable: skipped (RenderDoc disabled)");
			return false;
		}

		auto resolveInstallRoot = []() -> std::filesystem::path
		{
			if (const char* configuredPath = std::getenv("RENDERDOC_PATH"); configuredPath != nullptr && configuredPath[0] != '\0')
			{
				const std::filesystem::path configured(configuredPath);
				if (std::filesystem::is_regular_file(configured))
					return configured.parent_path();
				if (std::filesystem::is_directory(configured))
					return configured;
			}

			for (const char* variableName : { "ProgramFiles", "ProgramFiles(x86)" })
			{
				if (const char* root = std::getenv(variableName); root != nullptr && root[0] != '\0')
				{
					const auto candidate = std::filesystem::path(root) / "RenderDoc";
					if (std::filesystem::exists(candidate / "renderdoc.dll"))
						return candidate;
				}
			}

			return {};
		};

		const auto installRoot = resolveInstallRoot();
		if (installRoot.empty())
		{
			NLS_LOG_INFO("PreloadRenderDocIfAvailable: RenderDoc install root not found");
			return false;
		}

		const auto dllPath = installRoot / "renderdoc.dll";
		if (!std::filesystem::exists(dllPath))
		{
			NLS_LOG_INFO("PreloadRenderDocIfAvailable: renderdoc.dll not found at " + dllPath.string());
			return false;
		}

		// Preload the DLL so it can be dynamically enabled/disabled at runtime.
		HMODULE loadedModule = ::LoadLibraryW(dllPath.wstring().c_str());
		if (loadedModule != nullptr)
		{
			NLS_LOG_INFO("PreloadRenderDocIfAvailable: Successfully loaded renderdoc.dll");
			return true;
		}
		else
		{
			NLS_LOG_INFO("PreloadRenderDocIfAvailable: Failed to load renderdoc.dll");
			return false;
		}
#else
		(void)settings;
		return false;
#endif
	}
}

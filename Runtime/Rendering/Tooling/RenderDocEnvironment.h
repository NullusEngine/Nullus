#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "Rendering/Settings/DriverSettings.h"

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

	NLS_RENDER_API bool PreloadRenderDocIfAvailable(const Settings::RenderDocSettings& settings);
}

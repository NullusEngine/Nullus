#include "Rendering/Tooling/RenderDocEnvironment.h"

#include <cstdlib>

#include <Debug/Logger.h>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace NLS::Render::Tooling
{
	bool PreloadRenderDocIfAvailable(const Settings::RenderDocSettings& settings)
	{
#if defined(_WIN32)
		// Check if RenderDoc Vulkan layer is already loaded (implicit layer from registry).
		HMODULE existingModule = ::GetModuleHandleW(L"renderdoc.dll");
		if (existingModule != nullptr)
		{
			NLS_LOG_INFO("PreloadRenderDocIfAvailable: renderdoc.dll already loaded (Vulkan layer detected)");
			NLS_LOG_INFO("PreloadRenderDocIfAvailable: Note: Vulkan layer uses its own capture keys and directory settings");
			NLS_LOG_INFO("PreloadRenderDocIfAvailable: For programmatic control, either:");
			NLS_LOG_INFO("  1. Configure capture keys and directory in RenderDoc UI before running, OR");
			NLS_LOG_INFO("  2. Use the renderdoc_runner.py script which sets up environment correctly");
			return true;
		}

		// Do not actively load RenderDoc when capture tooling is not enabled.
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

		HMODULE loadedModule = ::LoadLibraryW(dllPath.wstring().c_str());
		if (loadedModule != nullptr)
		{
			NLS_LOG_INFO("PreloadRenderDocIfAvailable: Successfully loaded renderdoc.dll");
			return true;
		}

		NLS_LOG_INFO("PreloadRenderDocIfAvailable: Failed to load renderdoc.dll");
		return false;
#else
		(void)settings;
		return false;
#endif
	}
}

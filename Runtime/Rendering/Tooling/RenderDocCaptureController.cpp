#include "Rendering/Tooling/RenderDocCaptureController.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <unordered_set>
#include <string_view>
#include <vector>

#include <Debug/Logger.h>

#include "Platform/Utils/SystemCalls.h"

#if defined(_WIN32)
#include <Windows.h>
#include <shellapi.h>
#include "../../../ThirdParty/RenderDoc/renderdoc_app.h"
#endif

namespace NLS::Render::Tooling
{
	void* ResolveRenderDocCaptureDevice(const ::NLS::Render::RHI::NativeRenderDeviceInfo& nativeInfo)
	{
		switch (nativeInfo.backend)
		{
		case ::NLS::Render::RHI::NativeBackendType::DX12:
			// RenderDoc expects the API root handle for DX12.
			return nativeInfo.device != nullptr ? nativeInfo.device : nativeInfo.graphicsQueue;
		case ::NLS::Render::RHI::NativeBackendType::Vulkan:
#if defined(_WIN32)
			// RenderDoc expects Vulkan's dispatch-table pointer derived from VkInstance.
			if (nativeInfo.instance != nullptr)
				return RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(nativeInfo.instance);
#endif
			return nativeInfo.device != nullptr ? nativeInfo.device : nativeInfo.graphicsQueue;
		case ::NLS::Render::RHI::NativeBackendType::DX11:
		case ::NLS::Render::RHI::NativeBackendType::OpenGL:
		case ::NLS::Render::RHI::NativeBackendType::Metal:
		case ::NLS::Render::RHI::NativeBackendType::None:
		default:
			return nativeInfo.device;
		}
	}

namespace
{
#if defined(_WIN32)
	std::wstring ExpandEnvironmentStringsUtf16(const std::wstring& value)
	{
		if (value.empty())
			return {};

		const auto requiredSize = ::ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
		if (requiredSize == 0)
			return value;

		std::wstring expanded(static_cast<size_t>(requiredSize), L'\0');
		::ExpandEnvironmentStringsW(value.c_str(), expanded.data(), requiredSize);
		if (!expanded.empty() && expanded.back() == L'\0')
			expanded.pop_back();
		return expanded;
	}

	bool LaunchExecutable(const std::filesystem::path& executablePath, const std::filesystem::path& capturePath)
	{
		if (executablePath.empty() || capturePath.empty())
			return false;

		const std::wstring executableWide = executablePath.wstring();
		const std::wstring captureWide = capturePath.wstring();
		std::wstring commandLine = L"\"" + executableWide + L"\" \"" + captureWide + L"\"";

		STARTUPINFOW startupInfo{};
		startupInfo.cb = sizeof(startupInfo);
		PROCESS_INFORMATION processInformation{};
		const auto workingDirectory = executablePath.parent_path().wstring();

		std::vector<wchar_t> commandBuffer(commandLine.begin(), commandLine.end());
		commandBuffer.push_back(L'\0');

		const BOOL created = ::CreateProcessW(
			executableWide.c_str(),
			commandBuffer.data(),
			nullptr,
			nullptr,
			FALSE,
			0,
			nullptr,
			workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
			&startupInfo,
			&processInformation);

		if (!created)
			return false;

		::CloseHandle(processInformation.hThread);
		::CloseHandle(processInformation.hProcess);
		return true;
	}
#endif

	std::string SanitizePathComponent(std::string value)
	{
		if (value.empty())
			return "capture";

		for (char& character : value)
		{
			switch (character)
			{
			case '<':
			case '>':
			case ':':
			case '"':
			case '/':
			case '\\':
			case '|':
			case '?':
			case '*':
				character = '_';
				break;
			default:
				break;
			}
		}

		return value;
	}

	std::string BuildCaptureStem(
		const Settings::RenderDocSettings& settings,
		const std::string& resolvedBackendName,
		const std::string& label)
	{
		std::string stem = label.empty() ? settings.captureLabel : label;
		if (stem.empty())
			stem = "capture";

		if (!resolvedBackendName.empty())
			stem += "_" + resolvedBackendName;

		return SanitizePathComponent(stem);
	}
}

	struct RenderDocCaptureController::Impl
	{
		explicit Impl(Settings::RenderDocSettings inSettings)
			: settings(std::move(inSettings))
		{
			if (settings.captureDirectory.empty())
			{
				settings.captureDirectory =
					(std::filesystem::current_path() / "Logs" / "RenderDoc").string();
			}

#if defined(_WIN32)
			TryLoadRenderDoc();
			RefreshLatestCapturePath();
#endif
		}

		Settings::RenderDocSettings settings;
		std::string resolvedBackendName;
		std::string latestCapturePath;
		std::string pendingCaptureLabel;
		void* captureDevice = nullptr;
		void* captureWindow = nullptr;
		uint32_t presentCountdown = 0;
		bool captureQueued = false;
		bool manualCaptureActive = false;
		bool queuedCaptureActive = false;
		bool waitingForTriggeredCapture = false;
		std::unordered_set<std::string> knownCaptureFiles;

#if defined(_WIN32)
		std::filesystem::path renderDocDllPath;
		std::filesystem::path qrenderdocPath;
		HMODULE renderDocModule = nullptr;
		bool ownsRenderDocModule = false;
		RENDERDOC_API_1_7_0* api = nullptr;
		uint32_t knownCaptureCount = 0;

		~Impl()
		{
			if (ownsRenderDocModule && renderDocModule != nullptr)
				::FreeLibrary(renderDocModule);
		}

		bool IsAvailable() const
		{
			return settings.enabled && api != nullptr;
		}

		void ConfigureCaptureKeysAndPath(const char* context)
		{
			if (api == nullptr)
				return;

			const std::string contextPrefix = std::string(context != nullptr ? context : "RenderDoc");

			// Clear any existing capture keys first, then set only F11.
			api->SetCaptureKeys(nullptr, 0);
			RENDERDOC_InputButton captureKeys[] = { eRENDERDOC_Key_F11 };
			api->SetCaptureKeys(captureKeys, 1);

			if (!settings.captureDirectory.empty())
			{
				api->SetCaptureFilePathTemplate(settings.captureDirectory.c_str());
				NLS_LOG_INFO(contextPrefix + ": SetCaptureFilePathTemplate to: " + settings.captureDirectory);
			}

			NLS_LOG_INFO(contextPrefix + ": captureKey=F11");
		}

		void EnsureApiConnected(const char* context)
		{
			if (!settings.enabled || api != nullptr)
				return;

			NLS_LOG_INFO(std::string(context != nullptr ? context : "RenderDoc") + ": API not connected, attempting reconnect");
			ConnectToRenderDocAPI();
			if (api != nullptr)
				ConfigureCaptureKeysAndPath(context);
		}

		void TryLoadRenderDoc()
		{
			const auto installRoot = ResolveInstallRoot();
			if (!installRoot.empty())
			{
				const auto candidateDll = installRoot / "renderdoc.dll";
				if (std::filesystem::exists(candidateDll))
					renderDocDllPath = candidateDll;

				const auto candidateReplayUi = installRoot / "qrenderdoc.exe";
				if (std::filesystem::exists(candidateReplayUi))
					qrenderdocPath = candidateReplayUi;
			}

			renderDocModule = ::GetModuleHandleW(L"renderdoc.dll");
			NLS_LOG_INFO("TryLoadRenderDoc: GetModuleHandle result: " + std::string(renderDocModule != nullptr ? "found" : "not found"));
			// Do not actively load RenderDoc DLL unless tooling is enabled.
			// If module is already injected externally, we still connect to API so we can override capture keys.
			if (renderDocModule == nullptr && settings.enabled && !renderDocDllPath.empty())
			{
				renderDocModule = ::LoadLibraryW(renderDocDllPath.wstring().c_str());
				ownsRenderDocModule = renderDocModule != nullptr;
				NLS_LOG_INFO("TryLoadRenderDoc: LoadLibrary result: " + std::string(renderDocModule != nullptr ? "loaded" : "failed"));
			}

			if (renderDocModule == nullptr)
			{
				NLS_LOG_INFO("TryLoadRenderDoc: no module handle available");
				return;
			}

			const auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(::GetProcAddress(renderDocModule, "RENDERDOC_GetAPI"));
			if (getApi == nullptr)
			{
				NLS_LOG_INFO("TryLoadRenderDoc: RENDERDOC_GetAPI not found");
				return;
			}

			if (getApi(eRENDERDOC_API_Version_1_7_0, reinterpret_cast<void**>(&api)) != 1 || api == nullptr)
			{
				NLS_LOG_INFO("TryLoadRenderDoc: API version 1.7.0 not supported");
				return;
			}

			ConfigureCaptureKeysAndPath("TryLoadRenderDoc");

			int major = 0;
			int minor = 0;
			int patch = 0;
			api->GetAPIVersion(&major, &minor, &patch);
			knownCaptureCount = api->GetNumCaptures();

			NLS_LOG_INFO(
				"RenderDoc API connected: " +
				std::to_string(major) + "." +
				std::to_string(minor) + "." +
				std::to_string(patch) +
				", captureKey=F11");
		}

		// Connect to RenderDoc API without checking enabled flag
		// Used for dynamic enable/disable when DLL is already preloaded
		void ConnectToRenderDocAPI()
		{
			if (api != nullptr)
			{
				NLS_LOG_INFO("RenderDoc: Already connected, skipping");
				return; // Already connected
			}

			renderDocModule = ::GetModuleHandleW(L"renderdoc.dll");
			if (renderDocModule == nullptr)
			{
				NLS_LOG_ERROR("RenderDoc: DLL not loaded (GetModuleHandle returned null), cannot connect to API");
				return;
			}

			NLS_LOG_INFO("RenderDoc: DLL handle found, attempting to connect to API...");

			const auto installRoot = ResolveInstallRoot();
			if (!installRoot.empty())
			{
				const auto candidateReplayUi = installRoot / "qrenderdoc.exe";
				if (std::filesystem::exists(candidateReplayUi))
					qrenderdocPath = candidateReplayUi;
			}

			const auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(::GetProcAddress(renderDocModule, "RENDERDOC_GetAPI"));
			if (getApi == nullptr)
			{
				NLS_LOG_INFO("RenderDoc: Failed to get RENDERDOC_GetAPI function");
				return;
			}

			if (getApi(eRENDERDOC_API_Version_1_7_0, reinterpret_cast<void**>(&api)) != 1 || api == nullptr)
			{
				NLS_LOG_INFO("RenderDoc: Failed to get API version 1.7.0");
				return;
			}

			ConfigureCaptureKeysAndPath("ConnectToRenderDocAPI");

			int major = 0;
			int minor = 0;
			int patch = 0;
			api->GetAPIVersion(&major, &minor, &patch);
			knownCaptureCount = api->GetNumCaptures();

			NLS_LOG_INFO(
				"RenderDoc API connected (dynamic): " +
				std::to_string(major) + "." +
				std::to_string(minor) + "." +
				std::to_string(patch) +
				", captureKey=F11");
		}

		std::filesystem::path ResolveInstallRoot() const
		{
			if (const auto environmentPath = _wgetenv(L"RENDERDOC_PATH"); environmentPath != nullptr && environmentPath[0] != L'\0')
			{
				const std::filesystem::path configuredPath = ExpandEnvironmentStringsUtf16(environmentPath);
				if (std::filesystem::is_regular_file(configuredPath))
					return configuredPath.parent_path();
				if (std::filesystem::is_directory(configuredPath))
					return configuredPath;
			}

			const wchar_t* programFiles = _wgetenv(L"ProgramFiles");
			if (programFiles != nullptr && programFiles[0] != L'\0')
			{
				const std::filesystem::path candidate = std::filesystem::path(ExpandEnvironmentStringsUtf16(programFiles)) / "RenderDoc";
				if (std::filesystem::exists(candidate))
					return candidate;
			}

			const wchar_t* programFilesX86 = _wgetenv(L"ProgramFiles(x86)");
			if (programFilesX86 != nullptr && programFilesX86[0] != L'\0')
			{
				const std::filesystem::path candidate = std::filesystem::path(ExpandEnvironmentStringsUtf16(programFilesX86)) / "RenderDoc";
				if (std::filesystem::exists(candidate))
					return candidate;
			}

			wchar_t discoveredPath[MAX_PATH] = {};
			if (::SearchPathW(nullptr, L"qrenderdoc.exe", nullptr, MAX_PATH, discoveredPath, nullptr) > 0)
				return std::filesystem::path(discoveredPath).parent_path();

			return {};
		}

		void EnsureCaptureDirectoryExists() const
		{
			if (settings.captureDirectory.empty())
				return;

			std::error_code errorCode;
			std::filesystem::create_directories(settings.captureDirectory, errorCode);
		}

		std::vector<std::filesystem::path> EnumerateCaptureFiles() const
		{
			std::vector<std::filesystem::path> captures;

			auto appendRoot = [&captures](const std::filesystem::path& root)
			{
				if (root.empty() || !std::filesystem::exists(root))
					return;

				std::error_code errorCode;
				for (const auto& entry : std::filesystem::recursive_directory_iterator(root, errorCode))
				{
					if (errorCode)
						break;

					if (!entry.is_regular_file())
						continue;

					if (entry.path().extension() == ".rdc")
						captures.push_back(entry.path());
				}
			};

			appendRoot(std::filesystem::path(settings.captureDirectory));

			std::error_code tempError;
			const auto tempRenderDoc = std::filesystem::temp_directory_path(tempError) / "RenderDoc";
			if (!tempError)
				appendRoot(tempRenderDoc);

			return captures;
		}

		void SnapshotKnownCaptureFiles()
		{
			knownCaptureFiles.clear();
			for (const auto& capture : EnumerateCaptureFiles())
				knownCaptureFiles.insert(capture.string());
		}

		void RefreshLatestCapturePathFallback()
		{
			std::vector<std::filesystem::path> newCaptures;
			for (const auto& capture : EnumerateCaptureFiles())
			{
				if (!knownCaptureFiles.contains(capture.string()))
					newCaptures.push_back(capture);
			}

			if (newCaptures.empty())
				return;

			const auto newestCapture = *std::max_element(
				newCaptures.begin(),
				newCaptures.end(),
				[](const std::filesystem::path& lhs, const std::filesystem::path& rhs)
				{
					std::error_code lhsError;
					std::error_code rhsError;
					const auto lhsTime = std::filesystem::last_write_time(lhs, lhsError);
					const auto rhsTime = std::filesystem::last_write_time(rhs, rhsError);
					if (lhsError || rhsError)
						return lhs.string() < rhs.string();
					return lhsTime < rhsTime;
				});

			std::filesystem::path resolvedCapture = newestCapture;
			const std::filesystem::path configuredDirectory(settings.captureDirectory);
			if (!configuredDirectory.empty())
			{
				EnsureCaptureDirectoryExists();
				std::error_code relativeError;
				const auto relativePath = newestCapture.lexically_relative(configuredDirectory);
				const bool alreadyInCaptureDirectory =
					!relativeError &&
					!relativePath.empty() &&
					relativePath.generic_string().rfind("..", 0) != 0;

				if (!alreadyInCaptureDirectory)
				{
					const auto destination = configuredDirectory / newestCapture.filename();
					if (destination != newestCapture)
					{
						std::error_code copyError;
						std::filesystem::copy_file(newestCapture, destination, std::filesystem::copy_options::overwrite_existing, copyError);
						if (!copyError)
							resolvedCapture = destination;
					}
				}
			}

			latestCapturePath = resolvedCapture.string();
			SnapshotKnownCaptureFiles();
		}

		void PrepareCaptureMetadata(const std::string& label)
		{
			if (!IsAvailable())
				return;

			EnsureCaptureDirectoryExists();
			SnapshotKnownCaptureFiles();

			const auto captureStem = BuildCaptureStem(settings, resolvedBackendName, label);
			const auto captureTemplate = std::filesystem::path(settings.captureDirectory) / captureStem;

			api->SetCaptureFilePathTemplate(captureTemplate.string().c_str());
			if (api->SetCaptureTitle != nullptr)
				api->SetCaptureTitle(captureStem.c_str());
			if (captureDevice != nullptr && captureWindow != nullptr)
				api->SetActiveWindow(captureDevice, captureWindow);
		}

		void RefreshLatestCapturePath()
		{
			if (!IsAvailable())
				return;

			const auto captureCount = api->GetNumCaptures();
			if (captureCount == 0)
			{
				latestCapturePath.clear();
				knownCaptureCount = 0;
				return;
			}

			const auto index = captureCount - 1;
			uint32_t requiredLength = 0;
			uint64_t timestamp = 0;
			api->GetCapture(index, nullptr, &requiredLength, &timestamp);
			if (requiredLength == 0)
			{
				RefreshLatestCapturePathFallback();
				knownCaptureCount = captureCount;
				return;
			}

			std::string filename(requiredLength, '\0');
			if (api->GetCapture(index, filename.data(), &requiredLength, &timestamp) == 1)
			{
				if (!filename.empty() && filename.back() == '\0')
					filename.pop_back();
				latestCapturePath = filename;
			}

			knownCaptureCount = captureCount;
			if (latestCapturePath.empty())
				RefreshLatestCapturePathFallback();
		}
#endif
	};

	RenderDocCaptureController::RenderDocCaptureController(Settings::RenderDocSettings settings)
		: m_impl(std::make_unique<Impl>(std::move(settings)))
	{
		NLS_LOG_INFO(
			"RenderDoc controller settings: enabled=" + std::string(m_impl->settings.enabled ? "true" : "false") +
			", startupCaptureAfterFrames=" + std::to_string(m_impl->settings.startupCaptureAfterFrames) +
			", captureDirectory=\"" + m_impl->settings.captureDirectory + "\"");

		if (m_impl->settings.startupCaptureAfterFrames > 0)
		{
			m_impl->presentCountdown = m_impl->settings.startupCaptureAfterFrames;
			m_impl->pendingCaptureLabel = m_impl->settings.captureLabel;
			m_impl->captureQueued = true;
			NLS_LOG_INFO("RenderDoc startup capture armed.");
		}
	}

	RenderDocCaptureController::~RenderDocCaptureController() = default;

	bool RenderDocCaptureController::IsAvailable() const
	{
#if defined(_WIN32)
		return m_impl->IsAvailable();
#else
		return false;
#endif
	}

	bool RenderDocCaptureController::QueueCapture(const std::string& label)
	{
#if defined(_WIN32)
		m_impl->EnsureApiConnected("QueueCapture");
#endif
		if (!IsAvailable())
			return false;

		m_impl->presentCountdown = 1;
		m_impl->pendingCaptureLabel = label;
		m_impl->captureQueued = true;
		NLS_LOG_INFO("RenderDoc queued next-frame capture: " + (label.empty() ? std::string("capture") : label));
		return true;
	}

	bool RenderDocCaptureController::StartCapture()
	{
#if defined(_WIN32)
		m_impl->EnsureApiConnected("StartCapture");
		if (!IsAvailable())
			return false;

		m_impl->PrepareCaptureMetadata(m_impl->pendingCaptureLabel);
		m_impl->api->StartFrameCapture(m_impl->captureDevice, m_impl->captureWindow);
		m_impl->manualCaptureActive = m_impl->api->IsFrameCapturing() == 1;
		NLS_LOG_INFO(std::string("RenderDoc StartFrameCapture -> ") + (m_impl->manualCaptureActive ? "success" : "failed"));
		return m_impl->manualCaptureActive;
#else
		return false;
#endif
	}

	bool RenderDocCaptureController::EndCapture()
	{
#if defined(_WIN32)
		if (!IsAvailable() || !m_impl->manualCaptureActive)
			return false;

		const bool ended = m_impl->api->EndFrameCapture(m_impl->captureDevice, m_impl->captureWindow) == 1;
		m_impl->manualCaptureActive = false;
		m_impl->RefreshLatestCapturePath();
		NLS_LOG_INFO(std::string("RenderDoc EndFrameCapture -> ") + (ended ? "success" : "failed"));
		if (ended && m_impl->settings.autoOpenReplayUI)
			OpenLatestCapture();
		return ended;
#else
		return false;
#endif
	}

	void RenderDocCaptureController::OnPreFrame()
	{
#if defined(_WIN32)
		if (!IsAvailable() || !m_impl->captureQueued)
			return;

		if (m_impl->presentCountdown > 1)
		{
			--m_impl->presentCountdown;
			return;
		}

		m_impl->PrepareCaptureMetadata(m_impl->pendingCaptureLabel);
		m_impl->captureQueued = false;
		m_impl->presentCountdown = 0;
		m_impl->api->StartFrameCapture(m_impl->captureDevice, m_impl->captureWindow);
		// Some backends may not report IsFrameCapturing() synchronously immediately after StartFrameCapture().
		// Treat this as an active capture request and validate with EndFrameCapture() on post-present.
		m_impl->queuedCaptureActive = true;
		m_impl->waitingForTriggeredCapture = false;
		NLS_LOG_INFO("RenderDoc queued StartFrameCapture before frame -> requested");
#endif
	}

	void RenderDocCaptureController::OnPrePresent()
	{
	}

	void RenderDocCaptureController::OnPostPresent()
	{
#if defined(_WIN32)
		if (IsAvailable() && m_impl->queuedCaptureActive)
		{
			bool ended = m_impl->api->EndFrameCapture(m_impl->captureDevice, m_impl->captureWindow) == 1;
			if (!ended)
				ended = m_impl->api->EndFrameCapture(nullptr, nullptr) == 1;
			m_impl->queuedCaptureActive = false;
			if (ended)
			{
				m_impl->RefreshLatestCapturePath();
				if (!m_impl->latestCapturePath.empty())
				{
					NLS_LOG_INFO(
						std::string("RenderDoc queued EndFrameCapture after present -> success") +
						", latest=\"" + m_impl->latestCapturePath + "\"");
					if (m_impl->settings.autoOpenReplayUI)
						OpenLatestCapture();
				}
				else
				{
					m_impl->api->TriggerCapture();
					m_impl->waitingForTriggeredCapture = true;
					NLS_LOG_INFO("RenderDoc queued EndFrameCapture after present -> success, path unresolved; issued TriggerCapture fallback.");
				}
			}
			else
			{
				m_impl->api->TriggerCapture();
				m_impl->waitingForTriggeredCapture = true;
				NLS_LOG_INFO("RenderDoc queued EndFrameCapture after present -> failed, fell back to TriggerCapture().");
			}
			return;
		}

		if (!IsAvailable() || !m_impl->waitingForTriggeredCapture)
			return;

		const auto previousCaptureCount = m_impl->knownCaptureCount;
		m_impl->RefreshLatestCapturePath();
		if (!m_impl->latestCapturePath.empty())
		{
			m_impl->waitingForTriggeredCapture = false;
			if (m_impl->knownCaptureCount > previousCaptureCount)
				NLS_LOG_INFO("RenderDoc TriggerCapture produced capture: " + m_impl->latestCapturePath);
			else
				NLS_LOG_INFO("RenderDoc capture path resolved asynchronously: " + m_impl->latestCapturePath);
			if (m_impl->settings.autoOpenReplayUI)
				OpenLatestCapture();
		}
#endif
	}

	std::string RenderDocCaptureController::GetLatestCapturePath() const
	{
		return m_impl->latestCapturePath;
	}

	std::string RenderDocCaptureController::GetCaptureDirectory() const
	{
		return m_impl->settings.captureDirectory;
	}

	bool RenderDocCaptureController::OpenLatestCapture() const
	{
		if (m_impl->latestCapturePath.empty())
			return false;

#if defined(_WIN32)
		if (!m_impl->qrenderdocPath.empty())
			return LaunchExecutable(m_impl->qrenderdocPath, m_impl->latestCapturePath);
#endif

		Platform::SystemCalls::OpenFile(m_impl->latestCapturePath);
		return true;
	}

	bool RenderDocCaptureController::GetAutoOpenReplayUI() const
	{
		return m_impl->settings.autoOpenReplayUI;
	}

	void RenderDocCaptureController::SetAutoOpenReplayUI(bool enabled)
	{
		m_impl->settings.autoOpenReplayUI = enabled;
	}

	void RenderDocCaptureController::SetResolvedBackendName(const std::string& backendName)
	{
		m_impl->resolvedBackendName = SanitizePathComponent(backendName);
	}

	void RenderDocCaptureController::SetCaptureTarget(const ::NLS::Render::RHI::NativeRenderDeviceInfo& nativeInfo)
	{
#if defined(_WIN32)
		m_impl->EnsureApiConnected("SetCaptureTarget");
		m_impl->captureDevice = ResolveRenderDocCaptureDevice(nativeInfo);
		m_impl->captureWindow = nativeInfo.nativeWindowHandle != nullptr
			? nativeInfo.nativeWindowHandle
			: nativeInfo.platformWindow;
		if (m_impl->IsAvailable() && m_impl->captureDevice != nullptr && m_impl->captureWindow != nullptr)
		{
			m_impl->api->SetActiveWindow(m_impl->captureDevice, m_impl->captureWindow);
			m_impl->ConfigureCaptureKeysAndPath("SetCaptureTarget");
		}
#else
		(void)nativeInfo;
#endif
	}

	void RenderDocCaptureController::SetEnabled(bool enabled)
	{
#if defined(_WIN32)
		NLS_LOG_INFO("RenderDocCaptureController::SetEnabled: " + std::string(enabled ? "true" : "false"));
		if (m_impl->settings.enabled == enabled)
		{
			NLS_LOG_INFO("RenderDocCaptureController::SetEnabled: already in desired state, skipping");
			return;
		}

		m_impl->settings.enabled = enabled;

		// If enabling, connect to already-loaded RenderDoc DLL
		if (enabled)
		{
			if (m_impl->api == nullptr)
			{
				// DLL was preloaded by PreloadRenderDocIfAvailable, just connect to API
				NLS_LOG_INFO("RenderDocCaptureController::SetEnabled: calling ConnectToRenderDocAPI");
				m_impl->ConnectToRenderDocAPI();
			}
			else
			{
				NLS_LOG_INFO("RenderDocCaptureController::SetEnabled: API already connected");
				m_impl->ConfigureCaptureKeysAndPath("RenderDocCaptureController::SetEnabled");
			}
		}
		else
		{
			// If disabling, close any active capture
			if (m_impl->api != nullptr)
			{
				if (m_impl->manualCaptureActive || m_impl->queuedCaptureActive)
				{
					m_impl->api->EndFrameCapture(m_impl->captureDevice, m_impl->captureWindow);
				}
				m_impl->manualCaptureActive = false;
				m_impl->queuedCaptureActive = false;
				m_impl->captureQueued = false;
				// Note: We keep the DLL loaded and API pointer intact
				// so that disabling and re-enabling works without reloading
			}
		}
#else
		(void)enabled;
#endif
	}

	bool RenderDocCaptureController::IsEnabled() const
	{
		return m_impl->settings.enabled;
	}
}

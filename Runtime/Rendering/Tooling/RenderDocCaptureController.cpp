#include "Rendering/Tooling/RenderDocCaptureController.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
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
	::NLS::Render::RHI::NativeRenderDeviceHandle ResolveRenderDocCaptureDeviceHandle(
		const ::NLS::Render::RHI::NativeRenderDeviceInfo& nativeInfo)
	{
		switch (nativeInfo.backend)
		{
		case ::NLS::Render::RHI::NativeBackendType::DX12:
			// RenderDoc expects the API root handle for DX12.
			return nativeInfo.device != nullptr
				? nativeInfo.GetDeviceHandle()
				: nativeInfo.GetGraphicsQueueHandle();
		case ::NLS::Render::RHI::NativeBackendType::Vulkan:
#if defined(_WIN32)
			// RenderDoc expects Vulkan's dispatch-table pointer derived from VkInstance.
			if (nativeInfo.instance != nullptr)
				return nativeInfo.MakeHandle(RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(nativeInfo.instance));
#endif
			return nativeInfo.device != nullptr
				? nativeInfo.GetDeviceHandle()
				: nativeInfo.GetGraphicsQueueHandle();
		case ::NLS::Render::RHI::NativeBackendType::DX11:
		case ::NLS::Render::RHI::NativeBackendType::OpenGL:
		case ::NLS::Render::RHI::NativeBackendType::Metal:
		case ::NLS::Render::RHI::NativeBackendType::None:
		default:
			return nativeInfo.GetDeviceHandle();
		}
	}

	void* ResolveRenderDocCaptureDevice(const ::NLS::Render::RHI::NativeRenderDeviceInfo& nativeInfo)
	{
		return ResolveRenderDocCaptureDeviceHandle(nativeInfo).handle;
	}

	bool CanQueueRenderDocCapture(
		const bool available,
		const bool captureQueued,
		const bool manualCaptureActive,
		const bool queuedCaptureActive,
		const bool waitingForTriggeredCapture)
	{
		return available &&
			!captureQueued &&
			!manualCaptureActive &&
			!queuedCaptureActive &&
			!waitingForTriggeredCapture;
	}

	bool ShouldForceRenderDocCaptureFrameRender(
		const bool available,
		const bool captureQueued,
		const bool manualCaptureActive,
		const bool queuedCaptureActive,
		const bool waitingForTriggeredCapture,
		const bool presentationAlreadyCovered,
		const bool coveredPresentationFrameCompleted,
		const uint32_t presentCountdown)
	{
		return available &&
			captureQueued &&
			!manualCaptureActive &&
			!queuedCaptureActive &&
			!waitingForTriggeredCapture &&
			(!presentationAlreadyCovered || coveredPresentationFrameCompleted) &&
			presentCountdown <= 1u;
	}

	uint32_t ResolveRenderDocQueuedCaptureInitialCountdown(const bool nextExternalOutput)
	{
		return nextExternalOutput ? 1u : 2u;
	}

	RenderDocQueuedCaptureAction ResolveRenderDocQueuedCapturePreFrameAction(
		const bool available,
		const bool captureQueued,
		const bool frameWillPresent,
		const bool outputMayBePresentedLater,
		const bool presentationAlreadyCovered,
		const bool coveredPresentationFrameCompleted,
		const uint32_t presentCountdown)
	{
		if (!available || !captureQueued)
			return RenderDocQueuedCaptureAction::None;
		if (presentationAlreadyCovered &&
			(!coveredPresentationFrameCompleted || !outputMayBePresentedLater))
		{
			return RenderDocQueuedCaptureAction::None;
		}
		if (presentCountdown > 1)
			return frameWillPresent || outputMayBePresentedLater
				? RenderDocQueuedCaptureAction::WaitForFutureFrame
				: RenderDocQueuedCaptureAction::None;
		return RenderDocQueuedCaptureAction::StartExplicitFrameCapture;
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
		mutable std::mutex stateMutex;
		uint32_t presentCountdown = 0;
		bool captureQueued = false;
		bool manualCaptureActive = false;
		bool queuedCaptureActive = false;
		bool waitingForTriggeredCapture = false;
		bool triggeredCaptureMayResolveBaselinePath = false;
		bool presentationCoveredBySceneOutput = false;
		bool coveredSceneOutputFrameCompleted = false;
		std::unordered_set<std::string> knownCaptureFiles;

		bool IsAvailable() const
		{
#if defined(_WIN32)
			return settings.enabled && api != nullptr;
#else
			return false;
#endif
		}

#if defined(_WIN32)
		std::filesystem::path renderDocDllPath;
		std::filesystem::path qrenderdocPath;
		HMODULE renderDocModule = nullptr;
		bool ownsRenderDocModule = false;
		RENDERDOC_API_1_7_0* api = nullptr;
		uint32_t knownCaptureCount = 0;
		uint32_t triggeredCaptureBaselineCount = 0;
		std::string triggeredCaptureBaselinePath;

		~Impl()
		{
			if (ownsRenderDocModule && renderDocModule != nullptr)
				::FreeLibrary(renderDocModule);
		}

		void ConfigureCaptureKeysAndPath(const char* context)
		{
			if (api == nullptr)
				return;

			const std::string contextPrefix = std::string(context != nullptr ? context : "RenderDoc");

			// Nullus owns F11 and queues captures through the editor/game shortcut layer.
			// Leaving RenderDoc's native hotkey active creates a second, immediate capture
			// path that can race the queued whole-frame capture boundary.
			api->SetCaptureKeys(nullptr, 0);

			if (!settings.captureDirectory.empty())
			{
				api->SetCaptureFilePathTemplate(settings.captureDirectory.c_str());
				NLS_LOG_INFO(contextPrefix + ": SetCaptureFilePathTemplate to: " + settings.captureDirectory);
			}

			NLS_LOG_INFO(contextPrefix + ": native capture keys disabled; Nullus queues captures via shortcuts");
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
				", native capture keys disabled");
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
				", native capture keys disabled");
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

		bool OpenLatestCaptureFromImpl() const
		{
			if (latestCapturePath.empty())
				return false;

			if (!qrenderdocPath.empty())
				return LaunchExecutable(qrenderdocPath, latestCapturePath);

			Platform::SystemCalls::OpenFile(latestCapturePath);
			return true;
		}

		void TriggerCaptureFallback(
			const char* message,
			const bool mayResolveBaselinePath = false)
		{
			if (!IsAvailable())
				return;

			triggeredCaptureBaselineCount = knownCaptureCount;
			triggeredCaptureBaselinePath = latestCapturePath;
			triggeredCaptureMayResolveBaselinePath = mayResolveBaselinePath;
			api->TriggerCapture();
			waitingForTriggeredCapture = true;
			NLS_LOG_INFO(message != nullptr ? message : "RenderDoc TriggerCapture fallback issued.");
		}

		void WaitForEndedCapturePath(const char* message)
		{
			if (!IsAvailable())
				return;

			triggeredCaptureBaselineCount = knownCaptureCount;
			triggeredCaptureBaselinePath = latestCapturePath;
			triggeredCaptureMayResolveBaselinePath = true;
			waitingForTriggeredCapture = true;
			NLS_LOG_INFO(message != nullptr ? message : "RenderDoc EndFrameCapture completed; waiting for capture path.");
		}

		bool ResolveTriggeredCaptureIfAvailable(const char* context)
		{
			if (!IsAvailable() || !waitingForTriggeredCapture)
				return false;

			RefreshLatestCapturePath();
			const bool captureCountAdvanced = knownCaptureCount > triggeredCaptureBaselineCount;
			const bool capturePathChanged =
				!latestCapturePath.empty() &&
				latestCapturePath != triggeredCaptureBaselinePath;
			const bool baselinePathResolved =
				triggeredCaptureMayResolveBaselinePath &&
				triggeredCaptureBaselinePath.empty() &&
				!latestCapturePath.empty() &&
				knownCaptureCount >= triggeredCaptureBaselineCount;
			if (!captureCountAdvanced && !capturePathChanged && !baselinePathResolved)
				return false;

			waitingForTriggeredCapture = false;
			triggeredCaptureMayResolveBaselinePath = false;
			triggeredCaptureBaselinePath.clear();
			triggeredCaptureBaselineCount = knownCaptureCount;
			NLS_LOG_INFO(
				std::string("RenderDoc TriggerCapture resolved ") +
				(context != nullptr ? context : "asynchronously") +
				": " + latestCapturePath);
			if (settings.autoOpenReplayUI)
				OpenLatestCaptureFromImpl();
			return true;
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
		std::lock_guard lock(m_impl->stateMutex);
		return m_impl->IsAvailable();
	}

	bool RenderDocCaptureController::ShouldForceCaptureFrameRender() const
	{
		std::lock_guard lock(m_impl->stateMutex);
		return ShouldForceRenderDocCaptureFrameRender(
			m_impl->IsAvailable(),
			m_impl->captureQueued,
			m_impl->manualCaptureActive,
			m_impl->queuedCaptureActive,
			m_impl->waitingForTriggeredCapture,
			m_impl->presentationCoveredBySceneOutput,
			m_impl->coveredSceneOutputFrameCompleted,
			m_impl->presentCountdown);
	}

	bool RenderDocCaptureController::QueueCapture(const std::string& label)
	{
		return QueueCapture(label, ResolveRenderDocQueuedCaptureInitialCountdown());
	}

	bool RenderDocCaptureController::QueueCaptureForNextExternalOutput(const std::string& label)
	{
		return QueueCapture(label, ResolveRenderDocQueuedCaptureInitialCountdown(true));
	}

	bool RenderDocCaptureController::QueueCapture(
		const std::string& label,
		const uint32_t initialCountdown)
	{
		std::lock_guard lock(m_impl->stateMutex);
#if defined(_WIN32)
		m_impl->EnsureApiConnected("QueueCapture");
#endif
		if (!CanQueueRenderDocCapture(
				m_impl->IsAvailable(),
				m_impl->captureQueued,
				m_impl->manualCaptureActive,
				m_impl->queuedCaptureActive,
				m_impl->waitingForTriggeredCapture))
		{
			NLS_LOG_INFO("RenderDoc capture request ignored because another capture is already pending or active.");
			return false;
		}

		m_impl->presentCountdown = initialCountdown;
		m_impl->pendingCaptureLabel = label;
		m_impl->captureQueued = true;
		m_impl->presentationCoveredBySceneOutput = false;
		m_impl->coveredSceneOutputFrameCompleted = false;
		NLS_LOG_INFO("RenderDoc queued next-frame capture: " + (label.empty() ? std::string("capture") : label));
		return true;
	}

	bool RenderDocCaptureController::StartCapture()
	{
		std::lock_guard lock(m_impl->stateMutex);
#if defined(_WIN32)
		m_impl->EnsureApiConnected("StartCapture");
		if (!m_impl->IsAvailable())
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
		std::lock_guard lock(m_impl->stateMutex);
#if defined(_WIN32)
		if (!m_impl->IsAvailable() || !m_impl->manualCaptureActive)
			return false;

		const bool ended = m_impl->api->EndFrameCapture(m_impl->captureDevice, m_impl->captureWindow) == 1;
		m_impl->manualCaptureActive = false;
		m_impl->RefreshLatestCapturePath();
		NLS_LOG_INFO(std::string("RenderDoc EndFrameCapture -> ") + (ended ? "success" : "failed"));
		if (ended && m_impl->settings.autoOpenReplayUI)
			m_impl->OpenLatestCaptureFromImpl();
		return ended;
#else
		return false;
#endif
	}

	void RenderDocCaptureController::OnPreFrame(
		const bool frameWillPresent,
		const bool outputMayBePresentedLater)
	{
		std::lock_guard lock(m_impl->stateMutex);
#if defined(_WIN32)
		const RenderDocQueuedCaptureAction action = ResolveRenderDocQueuedCapturePreFrameAction(
			m_impl->IsAvailable(),
			m_impl->captureQueued,
			frameWillPresent,
			outputMayBePresentedLater,
			m_impl->presentationCoveredBySceneOutput,
			m_impl->coveredSceneOutputFrameCompleted,
			m_impl->presentCountdown);
		if (action == RenderDocQueuedCaptureAction::None)
			return;
		if (action == RenderDocQueuedCaptureAction::WaitForFutureFrame)
		{
			--m_impl->presentCountdown;
			if (outputMayBePresentedLater)
			{
				m_impl->presentationCoveredBySceneOutput = true;
				m_impl->coveredSceneOutputFrameCompleted = false;
			}
			return;
		}

		m_impl->PrepareCaptureMetadata(m_impl->pendingCaptureLabel);
		m_impl->captureQueued = false;
		m_impl->presentCountdown = 0;
		m_impl->api->StartFrameCapture(m_impl->captureDevice, m_impl->captureWindow);
		m_impl->queuedCaptureActive = m_impl->api->IsFrameCapturing() == 1;
		m_impl->waitingForTriggeredCapture = false;
		if (outputMayBePresentedLater)
		{
			m_impl->presentationCoveredBySceneOutput = true;
			m_impl->coveredSceneOutputFrameCompleted = false;
		}
		if (m_impl->queuedCaptureActive)
		{
			NLS_LOG_INFO(
				std::string("RenderDoc queued StartFrameCapture before ") +
				(outputMayBePresentedLater ? "external scene output" : "presentable frame") +
				" -> success");
			return;
		}

		m_impl->TriggerCaptureFallback(
			"RenderDoc queued StartFrameCapture before presentable frame -> failed, fell back to TriggerCapture().");
#endif
	}

	void RenderDocCaptureController::OnPrePresent()
	{
	}

	void RenderDocCaptureController::OnPostPresent()
	{
		std::lock_guard lock(m_impl->stateMutex);
#if defined(_WIN32)
		if (m_impl->IsAvailable() && m_impl->queuedCaptureActive)
		{
			bool ended = m_impl->api->EndFrameCapture(m_impl->captureDevice, m_impl->captureWindow) == 1;
			if (!ended)
				ended = m_impl->api->EndFrameCapture(nullptr, nullptr) == 1;
			m_impl->queuedCaptureActive = false;
			m_impl->presentationCoveredBySceneOutput = false;
			m_impl->coveredSceneOutputFrameCompleted = false;
			if (ended)
			{
				m_impl->RefreshLatestCapturePath();
				if (!m_impl->latestCapturePath.empty())
				{
						NLS_LOG_INFO(
							std::string("RenderDoc queued EndFrameCapture after present -> success") +
							", latest=\"" + m_impl->latestCapturePath + "\"");
						if (m_impl->settings.autoOpenReplayUI)
							m_impl->OpenLatestCaptureFromImpl();
					}
				else
				{
					m_impl->WaitForEndedCapturePath(
						"RenderDoc queued EndFrameCapture after present -> success, path unresolved; waiting for capture path.");
				}
			}
			else
			{
				m_impl->TriggerCaptureFallback(
					"RenderDoc queued EndFrameCapture after present -> failed, fell back to TriggerCapture().");
			}
			return;
		}

		m_impl->ResolveTriggeredCaptureIfAvailable("after post-present");
		m_impl->presentationCoveredBySceneOutput = false;
		m_impl->coveredSceneOutputFrameCompleted = false;
#endif
	}

	void RenderDocCaptureController::OnPostFrame(
		const bool frameWillPresent,
		const bool outputMayBePresentedLater)
	{
		std::lock_guard lock(m_impl->stateMutex);
#if defined(_WIN32)
		if (!m_impl->IsAvailable())
			return;
		m_impl->ResolveTriggeredCaptureIfAvailable("after post-frame");
		if (outputMayBePresentedLater && m_impl->presentationCoveredBySceneOutput)
			m_impl->coveredSceneOutputFrameCompleted = true;
		if (!m_impl->queuedCaptureActive ||
			frameWillPresent ||
			outputMayBePresentedLater)
		{
			return;
		}

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
						std::string("RenderDoc queued EndFrameCapture after offscreen frame -> success") +
						", latest=\"" + m_impl->latestCapturePath + "\"");
					if (m_impl->settings.autoOpenReplayUI)
						m_impl->OpenLatestCaptureFromImpl();
				}
			else
			{
				m_impl->WaitForEndedCapturePath(
					"RenderDoc queued EndFrameCapture after offscreen frame -> success, path unresolved; waiting for capture path.");
			}
		}
		else
		{
			m_impl->TriggerCaptureFallback(
				"RenderDoc queued EndFrameCapture after offscreen frame -> failed, fell back to TriggerCapture().");
		}
#else
		(void)frameWillPresent;
		(void)outputMayBePresentedLater;
#endif
	}

	std::string RenderDocCaptureController::GetLatestCapturePath() const
	{
		std::lock_guard lock(m_impl->stateMutex);
		return m_impl->latestCapturePath;
	}

	std::string RenderDocCaptureController::GetCaptureDirectory() const
	{
		std::lock_guard lock(m_impl->stateMutex);
		return m_impl->settings.captureDirectory;
	}

	bool RenderDocCaptureController::OpenLatestCapture() const
	{
		std::lock_guard lock(m_impl->stateMutex);
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
		std::lock_guard lock(m_impl->stateMutex);
		return m_impl->settings.autoOpenReplayUI;
	}

	void RenderDocCaptureController::SetAutoOpenReplayUI(bool enabled)
	{
		std::lock_guard lock(m_impl->stateMutex);
		m_impl->settings.autoOpenReplayUI = enabled;
	}

	void RenderDocCaptureController::SetResolvedBackendName(const std::string& backendName)
	{
		std::lock_guard lock(m_impl->stateMutex);
		m_impl->resolvedBackendName = SanitizePathComponent(backendName);
	}

	void RenderDocCaptureController::SetCaptureTarget(const ::NLS::Render::RHI::NativeRenderDeviceInfo& nativeInfo)
	{
		std::lock_guard lock(m_impl->stateMutex);
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
		std::lock_guard lock(m_impl->stateMutex);
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
				m_impl->waitingForTriggeredCapture = false;
				m_impl->triggeredCaptureMayResolveBaselinePath = false;
				m_impl->triggeredCaptureBaselinePath.clear();
				m_impl->triggeredCaptureBaselineCount = 0;
				m_impl->captureQueued = false;
				m_impl->presentationCoveredBySceneOutput = false;
				m_impl->coveredSceneOutputFrameCompleted = false;
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
		std::lock_guard lock(m_impl->stateMutex);
		return m_impl->settings.enabled;
	}
}

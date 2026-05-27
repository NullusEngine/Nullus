#include <filesystem>

#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include <Core/ServiceLocator.h>
#include <Debug/Logger.h>
#include "Windowing/Settings/DeviceSettings.h"
#include "Assets/EditorAssetDatabase.h"
#include "Core/Context.h"
#include "Core/EditorFrameLatency.h"
#include "Debug/FileHandler.h"
#include "Rendering/BaseSceneRenderer.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Rendering/Tooling/RenderDocEnvironment.h"
#include "Settings/EditorSettings.h"
#include "Settings/EditorSettingsPersistence.h"
#include "Utils/PathParser.h"

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <commctrl.h>
    #pragma comment(lib, "Comctl32.lib")
#endif

using namespace NLS::Core::ResourceManagement;
namespace NLS
{
namespace
{
#ifdef _WIN32
    constexpr wchar_t kNativeProgressWindowClassName[] = L"NullusNativeProgressDialog";
    constexpr UINT_PTR kProgressCancelButtonId = 1001u;
    constexpr UINT kProgressApplyStateMessage = WM_APP + 1;
    constexpr UINT kProgressCloseMessage = WM_APP + 2;

    std::wstring ToWideString(const std::string& value)
    {
        if (value.empty())
            return {};

        const int wideLength = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
        if (wideLength <= 0)
            return std::wstring(value.begin(), value.end());

        std::wstring result(static_cast<size_t>(wideLength), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), wideLength);
        if (!result.empty() && result.back() == L'\0')
            result.pop_back();
        return result;
    }

    LRESULT CALLBACK NativeProgressDialogProc(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam);

    RECT ResolveProgressDialogMonitorBounds()
    {
        RECT workArea {};
        HMONITOR monitor = nullptr;
        if (const HWND foregroundWindow = GetForegroundWindow(); foregroundWindow != nullptr)
            monitor = MonitorFromWindow(foregroundWindow, MONITOR_DEFAULTTONEAREST);

        if (monitor == nullptr)
        {
            POINT cursorPosition {};
            if (GetCursorPos(&cursorPosition))
                monitor = MonitorFromPoint(cursorPosition, MONITOR_DEFAULTTONEAREST);
        }

        if (monitor == nullptr)
        {
            monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        }

        MONITORINFO monitorInfo {};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo))
            return monitorInfo.rcMonitor;

        workArea.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        workArea.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        workArea.right = workArea.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        workArea.bottom = workArea.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (workArea.right <= workArea.left || workArea.bottom <= workArea.top)
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

        return workArea;
    }

    void CenterProgressDialogWindow(HWND windowHandle)
    {
        if (windowHandle == nullptr)
            return;

        RECT windowRect {};
        if (!GetWindowRect(windowHandle, &windowRect))
            return;

        const int windowWidth = windowRect.right - windowRect.left;
        const int windowHeight = windowRect.bottom - windowRect.top;
        if (windowWidth <= 0 || windowHeight <= 0)
            return;

        RECT monitorBounds = ResolveProgressDialogMonitorBounds();
        if (monitorBounds.right - monitorBounds.left < windowWidth)
            monitorBounds.right = monitorBounds.left + windowWidth;
        if (monitorBounds.bottom - monitorBounds.top < windowHeight)
            monitorBounds.bottom = monitorBounds.top + windowHeight;

        const int x = monitorBounds.left + ((monitorBounds.right - monitorBounds.left) - windowWidth) / 2;
        const int y = monitorBounds.top + ((monitorBounds.bottom - monitorBounds.top) - windowHeight) / 2;
        SetWindowPos(
            windowHandle,
            HWND_TOPMOST,
            x,
            y,
            0,
            0,
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    using SetThreadDpiAwarenessContextFn = DPI_AWARENESS_CONTEXT (WINAPI*)(DPI_AWARENESS_CONTEXT);

    SetThreadDpiAwarenessContextFn ResolveSetThreadDpiAwarenessContext()
    {
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32 == nullptr)
            return nullptr;

        return reinterpret_cast<SetThreadDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
    }
#endif

    constexpr const char* kRendererResourceResolutionTargetPlatform = "asset-resolution";

    bool ShouldShowNativeTaskProgress(const NLS::Editor::Assets::ImportProgressEvent& event)
    {
        return event.targetPlatform != kRendererResourceResolutionTargetPlatform;
    }

	bool ResolveEditorLightGridEnabled(const std::filesystem::path& projectPath)
	{
		NLS::Editor::Settings::EditorSettingsRegistry registry;
		NLS::Editor::Settings::EditorSettings::RegisterSettingObjects(registry);
		NLS::Editor::Settings::EditorSettingsPersistence::Load(
			projectPath / "UserSettings" / "editor-settings.json",
			registry);
		return NLS::Editor::Settings::EditorSettings::GetRenderingSettingsObject().enableLightGrid;
	}

	bool IsEditorLayoutFileHealthy(const std::string& content)
	{
		return content.find("[Docking][Data]") != std::string::npos &&
			content.find("[Window][Scene View##Scene View]") != std::string::npos &&
			content.find("[Window][Game View##Game View]") != std::string::npos &&
			content.find("[Window][Hierarchy##Hierarchy]") != std::string::npos &&
			content.find("[Window][Inspector##Inspector]") != std::string::npos;
	}

	Render::Settings::EGraphicsBackend ResolveGraphicsBackend(NLS::Filesystem::IniFile& projectSettings)
	{
		if (projectSettings.IsKeyExisting("graphics_backend"))
		{
			return Render::Settings::ParseGraphicsBackendOrDefault(
				projectSettings.Get<std::string>("graphics_backend"),
				Render::Settings::GetPhase1RequiredRuntimeBackend());
		}

		return Render::Settings::GetPhase1RequiredRuntimeBackend();
	}

	Render::Settings::EGraphicsBackend ResolveEditorGraphicsBackend(NLS::Filesystem::IniFile& projectSettings,
        std::optional<Render::Settings::EGraphicsBackend> backendOverride = std::nullopt)
	{
		if (backendOverride.has_value())
		{
			NLS_LOG_INFO("Using command-line backend override: " + std::string(Render::Settings::ToString(backendOverride.value())));
			return backendOverride.value();
		}
		return ResolveGraphicsBackend(projectSettings);
	}

    void MigrateLegacyEditorLayoutFile(const std::filesystem::path& layoutPath)
    {
        if (!std::filesystem::exists(layoutPath))
            return;

        std::ifstream input(layoutPath, std::ios::binary);
        if (!input)
            return;

        const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const std::regex legacyPanelWindowPattern(R"(\[Window\]\[([^\[#].*?)##\d+\])");
        const std::string normalizedContent = std::regex_replace(content, legacyPanelWindowPattern, "[Window][$1##$1]");

        std::istringstream stream(normalizedContent);
        std::ostringstream deduplicated;
        std::string line;
        std::string currentHeader;
        std::ostringstream currentSection;
        std::unordered_set<std::string> seenWindowSections;

        auto flushSection = [&]()
        {
            if (currentHeader.empty())
                return;

            const bool isWindowSection = currentHeader.rfind("[Window][", 0) == 0;
            if (!isWindowSection || seenWindowSections.insert(currentHeader).second)
                deduplicated << currentSection.str();

            currentHeader.clear();
            currentSection.str({});
            currentSection.clear();
        };

        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (!line.empty() && line.front() == '[')
            {
                flushSection();
                currentHeader = line;
            }

            currentSection << line << "\n";
        }
        flushSection();

        const std::string migratedContent = deduplicated.str();
        if (migratedContent == content)
            return;

        std::ofstream output(layoutPath, std::ios::binary | std::ios::trunc);
        if (!output)
            return;

        output.write(migratedContent.data(), static_cast<std::streamsize>(migratedContent.size()));
        NLS_LOG_INFO("Migrated legacy editor layout IDs in " + layoutPath.string());
    }

	void EnsureEditorLayoutFileReady(const std::filesystem::path& layoutPath, const std::filesystem::path& defaultLayoutPath)
	{
		std::error_code error;
		std::filesystem::create_directories(layoutPath.parent_path(), error);

		bool restoredDefaultLayout = false;
		if (!std::filesystem::exists(layoutPath))
		{
			if (std::filesystem::exists(defaultLayoutPath))
			{
				std::filesystem::copy_file(defaultLayoutPath, layoutPath, std::filesystem::copy_options::overwrite_existing, error);
				restoredDefaultLayout = !error;
			}
		}

		if (!std::filesystem::exists(layoutPath))
			return;

		MigrateLegacyEditorLayoutFile(layoutPath);

		std::ifstream input(layoutPath, std::ios::binary);
		if (!input)
			return;

		const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		if (IsEditorLayoutFileHealthy(content))
			return;

		if (!std::filesystem::exists(defaultLayoutPath))
			return;

		std::filesystem::copy_file(defaultLayoutPath, layoutPath, std::filesystem::copy_options::overwrite_existing, error);
		if (error)
			return;

		MigrateLegacyEditorLayoutFile(layoutPath);
		NLS_LOG_WARNING(
			(restoredDefaultLayout
				? "Initialized missing editor layout from default template: "
				: "Reset invalid editor layout to default template: ") +
			layoutPath.string());
	}

}

class Editor::Core::NativeProgressDialog final
{
public:
    NativeProgressDialog()
    {
#ifdef _WIN32
        m_thread = std::thread([this]
        {
            RunDialogThread();
        });

        std::unique_lock lock(m_mutex);
        m_readyCondition.wait(lock, [this]
        {
            return m_ready;
        });
#endif
    }

    ~NativeProgressDialog()
    {
        Close();
    }

    void Update(
        const std::string& label,
        const float normalizedProgress,
        std::function<void()> cancelHandler = {},
        void* ownerWindowHandle = nullptr,
        const bool blockOwnerWindow = false)
    {
#ifdef _WIN32
        {
            std::lock_guard lock(m_mutex);
            m_label = label;
            m_progress = std::clamp(normalizedProgress, 0.0f, 1.0f);
            m_cancelHandler = std::move(cancelHandler);
            if (ownerWindowHandle != nullptr)
                m_ownerWindowHandle = static_cast<HWND>(ownerWindowHandle);
            m_blockOwnerWindow = blockOwnerWindow && m_ownerWindowHandle != nullptr;
            m_dirty = true;
        }

        if (const HWND windowHandle = GetWindowHandle(); windowHandle != nullptr)
            PostMessageW(windowHandle, kProgressApplyStateMessage, 0, 0);
#else
        (void)label;
        (void)normalizedProgress;
        (void)cancelHandler;
        (void)ownerWindowHandle;
        (void)blockOwnerWindow;
#endif
    }

    bool IsAvailable() const
    {
#ifdef _WIN32
        return m_available.load();
#else
        return false;
#endif
    }

    void Close()
    {
#ifdef _WIN32
        RequestClose();
        if (m_thread.joinable())
            m_thread.join();
#endif
    }

    void RequestClose()
    {
#ifdef _WIN32
        bool expected = false;
        if (!m_closeRequested.compare_exchange_strong(expected, true))
            return;

        RestoreOwnerWindow();

        NLS_LOG_INFO("Native progress dialog close requested.");
        const HWND windowHandle = GetWindowHandle();
        if (windowHandle != nullptr)
            PostMessageW(windowHandle, kProgressCloseMessage, 0, 0);

        if (m_threadId != 0)
            PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
#endif
    }

private:
#ifdef _WIN32
public:
    void RequestCancel()
    {
        std::function<void()> cancelHandler;
        {
            std::lock_guard lock(m_mutex);
            cancelHandler = m_cancelHandler;
        }

        if (cancelHandler)
            cancelHandler();

        {
            std::lock_guard lock(m_mutex);
            m_label = "Cancelling...";
            m_cancelHandler = {};
            m_dirty = true;
        }

        if (const HWND windowHandle = GetWindowHandle(); windowHandle != nullptr)
            PostMessageW(windowHandle, kProgressApplyStateMessage, 0, 0);
    }

private:
    void RunDialogThread()
    {
        m_threadId = GetCurrentThreadId();
        DPI_AWARENESS_CONTEXT previousDpiAwarenessContext = nullptr;
        if (const auto setThreadDpiAwarenessContext = ResolveSetThreadDpiAwarenessContext())
            previousDpiAwarenessContext =
                setThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        INITCOMMONCONTROLSEX controls {};
        controls.dwSize = sizeof(controls);
        controls.dwICC = ICC_PROGRESS_CLASS;
        InitCommonControlsEx(&controls);

        WNDCLASSEXW windowClass {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = NativeProgressDialogProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        windowClass.lpszClassName = kNativeProgressWindowClassName;
        RegisterClassExW(&windowClass);

        constexpr int clientWidth = 520;
        constexpr int clientHeight = 156;
        constexpr DWORD windowStyle = WS_CAPTION | WS_POPUP;
        constexpr DWORD windowExStyle = WS_EX_DLGMODALFRAME | WS_EX_TOPMOST;
        RECT windowRect {0, 0, clientWidth, clientHeight};
        AdjustWindowRectEx(&windowRect, windowStyle, FALSE, windowExStyle);
        const int width = windowRect.right - windowRect.left;
        const int height = windowRect.bottom - windowRect.top;

        HWND windowHandle = CreateWindowExW(
            windowExStyle,
            kNativeProgressWindowClassName,
            L"Nullus Editor",
            windowStyle,
            0,
            0,
            width,
            height,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            this);
        if (windowHandle == nullptr)
        {
            NLS_LOG_WARNING("Native progress dialog failed to create native window.");
            MarkReady();
            return;
        }
        SetWindowHandle(windowHandle);
        m_available.store(true);

        m_progressHandle = CreateWindowExW(
            0,
            PROGRESS_CLASSW,
            nullptr,
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            18,
            42,
            clientWidth - 36,
            18,
            windowHandle,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        if (m_progressHandle == nullptr)
            NLS_LOG_WARNING("Native progress dialog failed to create progress control.");

        m_labelHandle = CreateWindowExW(
            0,
            L"STATIC",
            L"Starting editor",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            18,
            72,
            clientWidth - 36,
            56,
            windowHandle,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        if (m_labelHandle == nullptr)
            NLS_LOG_WARNING("Native progress dialog failed to create label control.");

        m_cancelHandle = CreateWindowExW(
            0,
            L"BUTTON",
            L"Cancel",
            WS_CHILD | BS_PUSHBUTTON,
            clientWidth - 104,
            122,
            86,
            26,
            windowHandle,
            reinterpret_cast<HMENU>(kProgressCancelButtonId),
            GetModuleHandleW(nullptr),
            nullptr);
        if (m_cancelHandle == nullptr)
            NLS_LOG_WARNING("Native progress dialog failed to create cancel control.");

        HFONT defaultFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        if (defaultFont != nullptr)
        {
            if (m_labelHandle != nullptr)
                SendMessageW(m_labelHandle, WM_SETFONT, reinterpret_cast<WPARAM>(defaultFont), TRUE);
            if (m_progressHandle != nullptr)
                SendMessageW(m_progressHandle, WM_SETFONT, reinterpret_cast<WPARAM>(defaultFont), TRUE);
            if (m_cancelHandle != nullptr)
                SendMessageW(m_cancelHandle, WM_SETFONT, reinterpret_cast<WPARAM>(defaultFont), TRUE);
        }

        if (m_progressHandle != nullptr)
            SendMessageW(m_progressHandle, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
        CenterProgressDialogWindow(m_windowHandle);
        ShowWindow(m_windowHandle, SW_SHOWNORMAL);
        CenterProgressDialogWindow(m_windowHandle);
        UpdateWindow(m_windowHandle);

        MarkReady();

        MSG message {};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            if (message.hwnd == m_windowHandle && message.message == kProgressApplyStateMessage)
            {
                ApplyPendingState();
                continue;
            }

            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (m_windowHandle != nullptr)
            DestroyWindow(m_windowHandle);
        RestoreOwnerWindow();
        m_labelHandle = nullptr;
        m_progressHandle = nullptr;
        m_cancelHandle = nullptr;
        SetWindowHandle(nullptr);

        if (previousDpiAwarenessContext != nullptr)
        {
            if (const auto setThreadDpiAwarenessContext = ResolveSetThreadDpiAwarenessContext())
                setThreadDpiAwarenessContext(previousDpiAwarenessContext);
        }
    }

    void MarkReady()
    {
        std::lock_guard lock(m_mutex);
        m_ready = true;
        m_readyCondition.notify_all();
    }

    HWND GetWindowHandle()
    {
        std::lock_guard lock(m_mutex);
        return m_windowHandle;
    }

    void SetWindowHandle(HWND windowHandle)
    {
        std::lock_guard lock(m_mutex);
        m_windowHandle = windowHandle;
    }

    void ApplyPendingState()
    {
        std::string label;
        float progress = 0.0f;
        bool cancellable = false;
        bool blockOwnerWindow = false;
        HWND ownerWindowHandle = nullptr;
        {
            std::lock_guard lock(m_mutex);
            if (!m_dirty)
                return;

            label = m_label;
            progress = m_progress;
            cancellable = static_cast<bool>(m_cancelHandler);
            blockOwnerWindow = m_blockOwnerWindow;
            ownerWindowHandle = m_ownerWindowHandle;
            m_dirty = false;
        }

        ApplyOwnerBlocking(ownerWindowHandle, blockOwnerWindow);

        const int progressValue = static_cast<int>(std::clamp(progress, 0.0f, 1.0f) * 1000.0f);
        if (m_labelHandle != nullptr)
            SetWindowTextW(m_labelHandle, ToWideString(label).c_str());
        if (m_progressHandle != nullptr)
            SendMessageW(m_progressHandle, PBM_SETPOS, static_cast<WPARAM>(progressValue), 0);
        if (m_cancelHandle != nullptr)
        {
            ShowWindow(m_cancelHandle, cancellable ? SW_SHOW : SW_HIDE);
            EnableWindow(m_cancelHandle, cancellable ? TRUE : FALSE);
        }
        if (m_windowHandle != nullptr)
            UpdateWindow(m_windowHandle);
    }

    void ApplyOwnerBlocking(HWND ownerWindowHandle, const bool blockOwnerWindow)
    {
        if (ownerWindowHandle == nullptr)
            return;

        const bool alreadyBlocked = m_ownerWindowBlocked.exchange(blockOwnerWindow);
        if (alreadyBlocked == blockOwnerWindow)
            return;

        EnableWindow(ownerWindowHandle, blockOwnerWindow ? FALSE : TRUE);
    }

    void RestoreOwnerWindow()
    {
        HWND ownerWindowHandle = nullptr;
        {
            std::lock_guard lock(m_mutex);
            ownerWindowHandle = m_ownerWindowHandle;
            m_blockOwnerWindow = false;
        }

        if (ownerWindowHandle != nullptr && m_ownerWindowBlocked.exchange(false))
            EnableWindow(ownerWindowHandle, TRUE);
    }

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_readyCondition;
    std::atomic_bool m_closeRequested = false;
    std::atomic<DWORD> m_threadId = 0;
    std::atomic_bool m_available = false;
    std::atomic_bool m_ownerWindowBlocked = false;
    bool m_ready = false;
    bool m_dirty = false;
    bool m_blockOwnerWindow = false;
    std::string m_label = "Starting editor";
    float m_progress = 0.0f;
    std::function<void()> m_cancelHandler;
    HWND m_ownerWindowHandle = nullptr;
    HWND m_windowHandle = nullptr;
    HWND m_progressHandle = nullptr;
    HWND m_labelHandle = nullptr;
    HWND m_cancelHandle = nullptr;
#endif
};

#ifdef _WIN32
namespace
{
    NLS::Editor::Core::NativeProgressDialog* GetDialogFromWindow(HWND windowHandle)
    {
        return reinterpret_cast<NLS::Editor::Core::NativeProgressDialog*>(
            GetWindowLongPtrW(windowHandle, GWLP_USERDATA));
    }

    LRESULT CALLBACK NativeProgressDialogProc(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
        {
            const auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(windowHandle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(createStruct->lpCreateParams));
        }
        else if (message == WM_COMMAND && LOWORD(wParam) == kProgressCancelButtonId)
        {
            if (auto* dialog = GetDialogFromWindow(windowHandle))
                dialog->RequestCancel();
            return 0;
        }
        else if (message == kProgressCloseMessage)
        {
            PostQuitMessage(0);
            return 0;
        }
        else if (message == WM_CLOSE)
        {
            return 0;
        }
        return DefWindowProcW(windowHandle, message, wParam, lParam);
    }
}
#endif

Editor::Core::Context::Context(const std::string& p_projectPath, const std::string& p_projectName,
    std::optional<Render::Settings::EGraphicsBackend> p_backendOverride,
    std::optional<Render::Settings::RenderDocSettings> p_renderDocOverride,
    std::optional<Render::Settings::EngineDiagnosticsSettings> p_diagnosticsOverride)
    : projectPath(p_projectPath),
    projectName(p_projectName),
    projectFilePath(p_projectPath + Utils::PathParser::Separator() + p_projectName + ".nullus"), 
    engineAssetsPath(std::filesystem::canonical(std::filesystem::path("../Assets/Engine")).string() + Utils::PathParser::Separator()), 
    projectAssetsPath(p_projectPath + Utils::PathParser::Separator() + "Assets" + Utils::PathParser::Separator()), 
    editorAssetsPath(std::filesystem::canonical(std::filesystem::path("../Assets/Editor")).string() + Utils::PathParser::Separator()), 
    sceneManager(projectAssetsPath), projectSettings(projectFilePath),
    m_backendOverride(p_backendOverride),
    m_renderDocOverride(std::move(p_renderDocOverride)),
    m_diagnosticsOverride(std::move(p_diagnosticsOverride)),
    m_nativeProgressDialog(std::make_unique<NativeProgressDialog>())
{
    PresentStartupProgressFrame("Reading project settings", 0.02f);

    MeshManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);
    TextureManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);
    ShaderManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);
    MaterialManager::ProvideAssetPaths(projectAssetsPath, engineAssetsPath);

	if (!IsProjectSettingsIntegrityVerified())
	{
		NLS_LOG_WARNING("Project settings file is missing keys or empty. Restoring editor defaults in " + projectFilePath);
		ResetProjectSettings();
		projectSettings.Rewrite();
	}
    else if (!projectSettings.IsKeyExisting("last_opened_scene"))
    {
        projectSettings.Add<std::string>("last_opened_scene", "");
        projectSettings.Rewrite();
    }

    Editor::Settings::EditorSettingsRegistry editorSettingsRegistry;
    Editor::Settings::EditorSettings::RegisterSettingObjects(editorSettingsRegistry);
    Editor::Settings::EditorSettingsPersistence::Load(
        std::filesystem::path(p_projectPath) / "UserSettings" / "editor-settings.json",
        editorSettingsRegistry);
    auto renderDocSettings = Editor::Settings::EditorSettings::BuildRenderDocSettings();
    if (m_renderDocOverride.has_value())
        renderDocSettings = m_renderDocOverride.value();
    m_diagnosticsSettings = Editor::Settings::EditorSettings::BuildDiagnosticsSettings();
    ApplyDiagnosticsOverride(m_diagnosticsSettings);
    const auto& runtimeSettings = Editor::Settings::EditorSettings::GetRuntimeSettingsObject();

    const auto logDirectory = std::filesystem::path(p_projectPath) / "Logs";
    std::error_code logDirectoryError;
    std::filesystem::create_directories(logDirectory, logDirectoryError);
    if (logDirectoryError)
        throw std::runtime_error("Failed to create editor log directory: " + logDirectory.string());
    NLS::Debug::FileHandler::SetLogFilePath(logDirectory.string());
    /* Settings */
    NLS::Windowing::Settings::DeviceSettings deviceSettings;
    deviceSettings.contextMajorVersion = 4;
    deviceSettings.contextMinorVersion = 3;
    windowSettings.title = "Nullus Editor";
    windowSettings.width = 1600;
    windowSettings.height = 900;
    windowSettings.maximized = true;
    windowSettings.visible = false;
    windowSettings.focused = false;
    const auto graphicsBackend = ResolveEditorGraphicsBackend(projectSettings, m_backendOverride);
    if (const auto restriction =
        Render::Settings::GetPhase1BackendRestrictionMessage(graphicsBackend, "Editor runtime");
        restriction.has_value())
    {
        throw std::runtime_error(*restriction);
    }
    windowSettings.clientAPI = Windowing::Settings::WindowClientAPI::NoAPI;

    /* Graphics context creation */
    NLS::Render::Settings::DriverSettings driverSettings;
    driverSettings.graphicsBackend = graphicsBackend;
    driverSettings.enableThreadedRendering = runtimeSettings.enableThreadedRendering;
    driverSettings.enableLightGrid = Editor::Settings::EditorSettings::GetRenderingSettingsObject().enableLightGrid;
    driverSettings.threadedFrameSlotCount = Editor::Core::ResolveEditorThreadedFrameSlotCount(driverSettings.framesInFlight);
    driverSettings.diagnostics = m_diagnosticsSettings;

    if (renderDocSettings.enabled || renderDocSettings.startupCaptureAfterFrames > 0)
    {
        driverSettings.renderDoc = renderDocSettings;
        NLS_LOG_INFO(
            std::string("RenderDoc: applied ") +
            (m_renderDocOverride.has_value() ? "command-line override" : "editor settings") +
            " (enabled=" + std::string(renderDocSettings.enabled ? "true" : "false") +
            ", captureAfterFrames=" + std::to_string(renderDocSettings.startupCaptureAfterFrames) + ")");
    }

	Render::Tooling::ApplyRenderDocEnvironmentOverrides(
		driverSettings.renderDoc,
		(std::filesystem::current_path() / "Build" / "RenderDocCaptures" / "Editor").string(),
		"Editor");
    Render::Tooling::PreloadRenderDocIfAvailable(driverSettings.renderDoc);

    /* Window creation */
    PresentStartupProgressFrame("Creating editor window", 0.10f);
    device = std::make_unique<NLS::Context::Device>(deviceSettings);
    window = std::make_unique<NLS::Windowing::Window>(*device, windowSettings);
    window->SetIcon(engineAssetsPath + "Brand" + Utils::PathParser::Separator() + "NullusLogoMark.png");
    inputManager = std::make_unique<NLS::Windowing::Inputs::InputManager>(*window);

    PresentStartupProgressFrame("Initializing graphics device", 0.18f);
    driver = std::make_unique<NLS::Render::Context::Driver>(driverSettings);

    const auto runtimeReadiness = driver->EvaluateEditorMainRuntimeReadiness(graphicsBackend);
    if (runtimeReadiness.primaryWarning.has_value())
        NLS_LOG_WARNING(runtimeReadiness.primaryWarning.value());
    if (runtimeReadiness.detailWarning.has_value())
        NLS_LOG_WARNING(runtimeReadiness.detailWarning.value());

    if (runtimeReadiness.primaryWarning.has_value())
    {
        throw std::runtime_error(runtimeReadiness.primaryWarning.value());
    }

    if (driver == nullptr || driver->GetActiveGraphicsBackend() == Render::Settings::EGraphicsBackend::NONE)
    {
        const std::string message =
            "Editor startup failed: could not create a usable RHI device for backend " +
            std::string(Render::Settings::ToString(graphicsBackend)) + ".";
        NLS_LOG_ERROR(message);
        throw std::runtime_error(message);
    }

    PresentStartupProgressFrame("Creating editor swapchain", 0.24f);
    const auto initialFramebufferSize = window->GetFramebufferSize();
    if (!driver->CreatePlatformSwapchain(
        window->GetGlfwWindow(),
        window->GetNativeWindowHandle(),
        static_cast<uint32_t>(initialFramebufferSize.x),
        static_cast<uint32_t>(initialFramebufferSize.y),
        projectSettings.GetOrDefault<bool>("vsync", true)))
    {
        const std::string message = "Editor startup failed: CreatePlatformSwapchain returned false.";
        NLS_LOG_ERROR(message);
        throw std::runtime_error(message);
    }
    NLS::Core::ServiceLocator::Provide<NLS::Render::Context::Driver>(*driver);
    NLS::Core::ServiceLocator::Provide<NLS::Windowing::Window>(*window);

    if (const auto pickingReadbackWarning = driver->GetEditorPickingReadbackWarning();
        pickingReadbackWarning.has_value())
    {
        NLS_LOG_WARNING(pickingReadbackWarning.value());
    }

    uiManager = std::make_unique<NLS::UI::UIManager>(
        window->GetGlfwWindow(),
        driverSettings.graphicsBackend,
        UI::EStyle::ALTERNATIVE_DARK);
    uiManager->LoadFont("Ruda_Big", editorAssetsPath + "/Fonts/Ruda-Bold.ttf", 16);
    uiManager->LoadFont("Ruda_Small", editorAssetsPath + "/Fonts/Ruda-Bold.ttf", 12);
    uiManager->LoadFont("Ruda_Medium", editorAssetsPath + "/Fonts/Ruda-Bold.ttf", 14);
    uiManager->UseFont("Ruda_Medium");
    const auto layoutPath = std::filesystem::path(p_projectPath) / "UserSettings" / "layout.ini";
    EnsureEditorLayoutFileReady(layoutPath, std::filesystem::path(editorAssetsPath) / "Settings" / "layout.ini");

    // Set up swapchain resize callback to notify UI before resize.
    driver->SetSwapchainWillResizeCallback([uiManager = uiManager.get()]() {
        if (uiManager != nullptr)
            uiManager->NotifySwapchainWillResize();
    });

    uiManager->SetEditorLayoutSaveFilename(layoutPath.string());
    uiManager->SetEditorLayoutAutosaveFrequency(60.0f);
    uiManager->EnableEditorLayoutSave(true);
    uiManager->EnableDocking(true);

    PresentStartupProgressFrame("Registering resource services", 0.30f);

    NLS::Core::ServiceLocator::Provide<MeshManager>(meshManager);
    NLS::Core::ServiceLocator::Provide<TextureManager>(textureManager);
    NLS::Core::ServiceLocator::Provide<ShaderManager>(shaderManager);
    NLS::Core::ServiceLocator::Provide<MaterialManager>(materialManager);
    NLS::Engine::Rendering::BaseSceneRenderer::PreloadSceneFallbackShader(shaderManager);

    PresentStartupProgressFrame("Preparing editor resources", 0.34f);

    /* Editor resources */
    editorResources = std::make_unique<Editor::Core::EditorResources>(editorAssetsPath, projectAssetsPath);
    PresentStartupProgressFrame("Loading editor helper shaders and models", 0.38f);
    editorResources->PreloadStartupResources();

    PresentStartupProgressFrame("Registering runtime services", 0.50f);

    /* Service Locator providing */
    NLS::Core::ServiceLocator::Provide<NLS::UI::UIManager>(*uiManager);


    NLS::Core::ServiceLocator::Provide<NLS::Windowing::Inputs::InputManager>(*inputManager);
    NLS::Core::ServiceLocator::Provide<NLS::Engine::SceneSystem::SceneManager>(sceneManager);

    importProgressTracker.Subscribe([this](const NLS::Editor::Assets::ImportProgressEvent& event)
    {
        if (!ShouldShowNativeTaskProgress(event))
            return;

        NLS::Editor::Assets::EditorAssetDatabase database;
        const auto status = database.GetImportProgressStatus(event.terminalStatus == Assets::ImportJobTerminalStatus::None
            ? std::optional<NLS::Editor::Assets::ImportProgressEvent>(event)
            : std::nullopt);
        if (event.terminalStatus == Assets::ImportJobTerminalStatus::None)
        {
            std::function<void()> cancelHandler;
            if (status.cancellable && !event.cancellationRequested)
            {
                cancelHandler = [this, jobId = event.jobId]
                {
                    if (auto token = importProgressTracker.GetCancellationToken(jobId); token.has_value())
                        token->get().Cancel();
                };
            }
            PresentTaskProgress(
                event.jobId.value,
                status.visible ? status.label : event.message,
                status.normalizedProgress,
                std::move(cancelHandler));
        }
        else
        {
            if (const auto activeEvent = importProgressTracker.GetActiveEvent(); activeEvent.has_value())
            {
                if (!ShouldShowNativeTaskProgress(*activeEvent))
                {
                    CompleteTaskProgress(event.message);
                    return;
                }

                const auto activeStatus = database.GetImportProgressStatus(activeEvent);
                std::function<void()> cancelHandler;
                if (activeStatus.cancellable && !activeEvent->cancellationRequested)
                {
                    cancelHandler = [this, jobId = activeEvent->jobId]
                    {
                        if (auto token = importProgressTracker.GetCancellationToken(jobId); token.has_value())
                            token->get().Cancel();
                    };
                }
                PresentTaskProgress(
                    activeEvent->jobId.value,
                    activeStatus.visible ? activeStatus.label : activeEvent->message,
                    activeStatus.normalizedProgress,
                    std::move(cancelHandler));
            }
            else
            {
                CompleteTaskProgress(event.message);
            }
        }
    });

    ApplyProjectSettings();
}

Editor::Core::Context::~Context()
{
    if (driver != nullptr)
        driver->SetSwapchainWillResizeCallback(nullptr);
    ShutdownThreadedRendering();
    meshManager.UnloadResources();
    textureManager.UnloadResources();
    shaderManager.UnloadResources();
    materialManager.UnloadResources();
}

void Editor::Core::Context::ShutdownThreadedRendering()
{
    if (driver != nullptr)
    {
        driver->SetSwapchainWillResizeCallback(nullptr);
        driver->ShutdownThreadedRendering();
    }
}

void Editor::Core::Context::ResetProjectSettings()
{
    projectSettings.RemoveAll();
    projectSettings.Add<float>("gravity", -9.81f);
    projectSettings.Add<int>("x_resolution", 1280);
    projectSettings.Add<int>("y_resolution", 720);
    projectSettings.Add<bool>("fullscreen", false);
    projectSettings.Add<std::string>("executable_name", "Game");
    projectSettings.Add<std::string>("start_scene", "");
    projectSettings.Add<std::string>("last_opened_scene", "");
    projectSettings.Add<bool>("vsync", true);
    projectSettings.Add<bool>("multi_sampling", true);
    projectSettings.Add<int>("samples", 4);
    projectSettings.Add<std::string>("graphics_backend", Render::Settings::ToString(Render::Settings::GetPhase1RequiredRuntimeBackend()));
    projectSettings.Add<int>("opengl_major", 4);
    projectSettings.Add<int>("opengl_minor", 3);
    projectSettings.Add<bool>("dev_build", true);
}

bool Editor::Core::Context::IsProjectSettingsIntegrityVerified()
{
    return projectSettings.IsKeyExisting("gravity") && projectSettings.IsKeyExisting("x_resolution") && projectSettings.IsKeyExisting("y_resolution") && projectSettings.IsKeyExisting("fullscreen") && projectSettings.IsKeyExisting("executable_name") && projectSettings.IsKeyExisting("start_scene") && projectSettings.IsKeyExisting("vsync") && projectSettings.IsKeyExisting("multi_sampling") && projectSettings.IsKeyExisting("samples") && projectSettings.IsKeyExisting("graphics_backend") && projectSettings.IsKeyExisting("opengl_major") && projectSettings.IsKeyExisting("opengl_minor") && projectSettings.IsKeyExisting("dev_build");
}

void Editor::Core::Context::ApplyProjectSettings()
{
}

void Editor::Core::Context::PresentStartupProgressFrame(
    const std::string& label,
    const float normalizedProgress)
{
    std::lock_guard lock(m_progressDialogMutex);
    if (m_nativeProgressDialog == nullptr)
        m_nativeProgressDialog = std::make_unique<NativeProgressDialog>();

    m_lastStartupProgress = std::max(m_lastStartupProgress, std::clamp(normalizedProgress, 0.0f, 1.0f));
    m_nativeProgressDialog->Update(label, m_lastStartupProgress);
}

void Editor::Core::Context::CompleteStartupProgress()
{
    std::unique_ptr<NativeProgressDialog> dialog;
    {
        std::lock_guard lock(m_progressDialogMutex);
        if (m_nativeProgressDialog != nullptr)
        {
            m_lastStartupProgress = 1.0f;
            m_nativeProgressDialog->Update("Opening editor", 1.0f);
            dialog = std::move(m_nativeProgressDialog);
        }
    }

    if (dialog != nullptr)
    {
        dialog->Close();
    }

    if (window != nullptr)
    {
        window->Show();
        window->Focus();
    }
}

bool Editor::Core::Context::IsNativeStartupProgressAvailable() const
{
    std::lock_guard lock(m_progressDialogMutex);
    return m_nativeProgressDialog != nullptr && m_nativeProgressDialog->IsAvailable();
}

void Editor::Core::Context::PresentTaskProgress(
    const uint64_t taskKey,
    const std::string& label,
    const float normalizedProgress,
    std::function<void()> cancelHandler)
{
    std::lock_guard lock(m_progressDialogMutex);
    if (m_nativeProgressDialog == nullptr)
        m_nativeProgressDialog = std::make_unique<NativeProgressDialog>();

    if (!m_taskProgressVisible || m_activeTaskProgressKey != taskKey)
    {
        m_lastTaskProgress = 0.0f;
        m_activeTaskProgressKey = taskKey;
        m_taskProgressVisible = true;
    }

    m_lastTaskProgress = std::max(m_lastTaskProgress, std::clamp(normalizedProgress, 0.0f, 1.0f));
    m_nativeProgressDialog->Update(
        label,
        m_lastTaskProgress,
        std::move(cancelHandler),
        window != nullptr ? window->GetNativeWindowHandle() : nullptr,
        true);
}

void Editor::Core::Context::CompleteTaskProgress(const std::string& label)
{
    std::unique_ptr<NativeProgressDialog> dialog;
    {
        std::lock_guard lock(m_progressDialogMutex);
        if (!m_taskProgressVisible)
            return;

        if (m_nativeProgressDialog != nullptr)
        {
            m_nativeProgressDialog->Update(label, 1.0f);
            dialog = std::move(m_nativeProgressDialog);
        }

        m_lastTaskProgress = 0.0f;
        m_activeTaskProgressKey = 0u;
        m_taskProgressVisible = false;
    }

    if (dialog != nullptr)
    {
        dialog->Close();
    }
}

const Render::Settings::EngineDiagnosticsSettings& Editor::Core::Context::GetDiagnosticsSettings() const
{
    return m_diagnosticsSettings;
}
} // namespace NLS

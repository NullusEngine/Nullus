#include <Utils/SystemCalls.h>

#include <UI/Widgets/Visual/Separator.h>

#include "Panels/MenuBar.h"
#include "Panels/ProjectSettings.h"
#include "Core/EditorActions.h"
#include "Debug/TracyProfilerLauncher.h"
#include <Debug/Logger.h>
#include <Profiling/TracyProfiler.h>
#include "Settings/EditorSettings.h"
#include "Utils/GameObjectCreationMenu.h"
#include "Rendering/Context/DriverAccess.h"
#include "Shortcuts/EditorShortcutService.h"
#include "Undo/EditorUndoService.h"
#include "UI/Widgets/Texts/Text.h"
using namespace NLS;
using namespace NLS::UI;
using namespace NLS::Engine::Components;

Editor::Panels::MenuBar::MenuBar()
{
	CreateFileMenu();
    CreateEditMenu();
	CreateBuildMenu();
	CreateWindowMenu();
	createGameObjectsMenu();
	CreateResourcesMenu();
	CreateLayoutMenu();
	CreateToolsMenu();
	CreateDebuggingMenu();
	CreateHelpMenu();
}

void Editor::Panels::MenuBar::DrawMenuEntries()
{
	UpdateShortcutLabels();
    DrawWidgets();
}

void Editor::Panels::MenuBar::DrawDialogs()
{
    if (m_projectSettingsPanel)
        m_projectSettingsPanel->DrawModal();
    m_shortcutSettingsPanel.Draw();
}

void Editor::Panels::MenuBar::CreateFileMenu()
{
	auto& fileMenu = CreateWidget<Widgets::MenuList>("File");
	m_newSceneItem = &fileMenu.CreateWidget<Widgets::MenuItem>("New Scene", "CTRL + N");
	m_newSceneItem->ClickedEvent += EDITOR_BIND(LoadEmptyScene);
	m_saveSceneItem = &fileMenu.CreateWidget<Widgets::MenuItem>("Save Scene", "CTRL + S");
	m_saveSceneItem->ClickedEvent += EDITOR_BIND(SaveSceneChanges);
	m_saveSceneAsItem = &fileMenu.CreateWidget<Widgets::MenuItem>("Save Scene As...", "CTRL + SHIFT + S");
	m_saveSceneAsItem->ClickedEvent += EDITOR_BIND(SaveAs);
	fileMenu.CreateWidget<Widgets::MenuItem>("Exit", "ALT + F4").ClickedEvent						+= [] { EDITOR_CONTEXT(window)->SetShouldClose(true); };
}

void Editor::Panels::MenuBar::CreateEditMenu()
{
    m_editMenu = &CreateWidget<Widgets::MenuList>("Edit");
	m_undoItem = &m_editMenu->CreateWidget<Widgets::MenuItem>("Undo", "");
	m_undoItem->ClickedEvent += []
	{
		if (NLS::Core::ServiceLocator::Contains<Undo::EditorUndoService>())
			NLS::Core::ServiceLocator::Get<Undo::EditorUndoService>().RequestUndo();
	};
	m_redoItem = &m_editMenu->CreateWidget<Widgets::MenuItem>("Redo", "");
	m_redoItem->ClickedEvent += []
	{
		if (NLS::Core::ServiceLocator::Contains<Undo::EditorUndoService>())
			NLS::Core::ServiceLocator::Get<Undo::EditorUndoService>().RequestRedo();
	};
	m_editMenu->CreateWidget<Widgets::Separator>();
    m_editMenu->CreateWidget<Widgets::MenuItem>("Settings...").ClickedEvent += [this]
    {
        OpenProjectSettings();
    };
    m_editMenu->CreateWidget<Widgets::MenuItem>("Shortcuts...").ClickedEvent += [this]
    {
        m_shortcutSettingsPanel.Open();
    };
}

void Editor::Panels::MenuBar::CreateBuildMenu()
{
	auto& buildMenu = CreateWidget<Widgets::MenuList>("Build");
	buildMenu.CreateWidget<Widgets::MenuItem>("Build game").ClickedEvent					+=	EDITOR_BIND(Build, false, false);
	buildMenu.CreateWidget<Widgets::MenuItem>("Build game and run").ClickedEvent			+=	EDITOR_BIND(Build, true, false);
	buildMenu.CreateWidget<Widgets::Separator>();
	buildMenu.CreateWidget<Widgets::MenuItem>("Temporary build").ClickedEvent			+=	EDITOR_BIND(Build, true, true);
}

void Editor::Panels::MenuBar::CreateWindowMenu()
{
	m_windowMenu = &CreateWidget<Widgets::MenuList>("Window");

	/* When the menu is opened, we update which window is marked as "Opened" or "Closed" */
	m_windowMenu->ClickedEvent += std::bind(&MenuBar::UpdateToggleableItems, this);
}

void Editor::Panels::MenuBar::createGameObjectsMenu()
{
	auto& gameObjectsMenu = CreateWidget<Widgets::MenuList>("GameObjects");
    Utils::GameObjectCreationMenu::GenerateGameObjectCreationMenu(gameObjectsMenu);
}

void Editor::Panels::MenuBar::CreateResourcesMenu()
{
	auto& resourcesMenu = CreateWidget<Widgets::MenuList>("Resources");
	resourcesMenu.CreateWidget<Widgets::MenuItem>("Compile shaders").ClickedEvent += EDITOR_BIND(CompileShaders);
	resourcesMenu.CreateWidget<Widgets::MenuItem>("Save materials").ClickedEvent += EDITOR_BIND(SaveMaterials);
}

void Editor::Panels::MenuBar::CreateLayoutMenu() 
{
	auto& layoutMenu = CreateWidget<Widgets::MenuList>("Layout");
	layoutMenu.CreateWidget<Widgets::MenuItem>("Default").ClickedEvent += EDITOR_BIND(ResetLayout);
	layoutMenu.CreateWidget<Widgets::Separator>();
	layoutMenu.CreateWidget<Widgets::MenuItem>("Tall").ClickedEvent += EDITOR_BIND(ApplyLayoutPreset, "tall");
	layoutMenu.CreateWidget<Widgets::MenuItem>("Wide").ClickedEvent += EDITOR_BIND(ApplyLayoutPreset, "wide");
	layoutMenu.CreateWidget<Widgets::MenuItem>("2 by 3").ClickedEvent += EDITOR_BIND(ApplyLayoutPreset, "2_by_3");
	layoutMenu.CreateWidget<Widgets::MenuItem>("4 Split").ClickedEvent += EDITOR_BIND(ApplyLayoutPreset, "4_split");
}

void Editor::Panels::MenuBar::CreateToolsMenu()
{
	m_toolsMenu = &CreateWidget<Widgets::MenuList>("Tools");
	// Scripted editors add entries through RegisterCommand("Tools/..."); seeding
	// the lookup keeps those commands inside this built-in menu.
	m_commandMenus.emplace("Tools", m_toolsMenu);
	m_toolsMenu->ClickedEvent += std::bind(&MenuBar::RefreshTracyProfilerStatus, this);

	auto& tracyMenu = m_toolsMenu->CreateWidget<Widgets::MenuList>("Tracy Profiler");
	m_tracyClientStatusText = &tracyMenu.CreateWidget<Widgets::Text>("Client: Unknown");
	m_tracyToolStatusText = &tracyMenu.CreateWidget<Widgets::Text>("Tool: Unknown");
	tracyMenu.CreateWidget<Widgets::Separator>();
	m_tracyOpenProfilerItem = &tracyMenu.CreateWidget<Widgets::MenuItem>("Open Tracy Profiler");
	m_tracyOpenProfilerItem->ClickedEvent += [this] { OpenTracyProfiler(); };
	m_tracyOpenCaptureFolderItem = &tracyMenu.CreateWidget<Widgets::MenuItem>("Open Capture Folder");
	m_tracyOpenCaptureFolderItem->ClickedEvent += [this] { OpenTracyCaptureFolder(); };

	RefreshTracyProfilerStatus();
}

void Editor::Panels::MenuBar::RefreshTracyProfilerStatus()
{
	const auto resolution = Debug::ResolveTracyProfilerFromEditorProcess();

	if (m_tracyToolStatusText)
	{
		m_tracyToolStatusText->content = resolution.IsValid()
			? "Tool: " + resolution.executablePath.string()
			: std::string("Tool: not found - set NLS_TRACY_PROFILER or run ")
				+ ".agents/skills/nullus-tracy-performance/scripts/Prepare-NullusTracy.ps1";
	}

	if (m_tracyOpenProfilerItem)
	{
		m_tracyOpenProfilerItem->enabled = resolution.IsValid();
		m_tracyOpenProfilerItem->tooltip = resolution.IsValid()
			? resolution.executablePath.string()
			: "Tracy tools were not found. Run Prepare-NullusTracy.ps1 or set NLS_TRACY_PROFILER.";
	}

	if (m_tracyOpenCaptureFolderItem)
	{
		const auto captureDirectory = Debug::ResolveTracyCaptureDirectory(resolution);
		m_tracyOpenCaptureFolderItem->enabled = !captureDirectory.empty();
		m_tracyOpenCaptureFolderItem->tooltip = captureDirectory.empty()
			? "Run the editor from a repository checkout to locate Build/Tracy."
			: captureDirectory.string();
	}

	if (m_tracyClientStatusText)
	{
#if NLS_ENABLE_TRACY
		m_tracyClientStatusText->content = NLS::Base::Profiling::TracyProfiler::IsConnected()
			? "Client: connected"
			: "Client: ready (waiting for a profiler connection)";
#else
		m_tracyClientStatusText->content = "Client: not built (rebuild with NLS_ENABLE_TRACY=ON)";
#endif
	}
}

void Editor::Panels::MenuBar::OpenTracyProfiler()
{
	const auto resolution = Debug::ResolveTracyProfilerFromEditorProcess();
	std::string error;
	if (!Debug::LaunchTracyProfiler(resolution.executablePath, error))
	{
		NLS_LOG_WARNING("Tracy profiler could not be opened: " + error);
		RefreshTracyProfilerStatus();
		return;
	}

	NLS_LOG_INFO("Tracy profiler launched: " + resolution.executablePath.string());
}

void Editor::Panels::MenuBar::OpenTracyCaptureFolder()
{
	const auto resolution = Debug::ResolveTracyProfilerFromEditorProcess();
	const auto captureDirectory = Debug::ResolveTracyCaptureDirectory(resolution);
	if (captureDirectory.empty())
	{
		NLS_LOG_WARNING("Tracy capture directory is unavailable outside a repository checkout.");
		return;
	}

	std::error_code error;
	std::filesystem::create_directories(captureDirectory, error);
	Platform::SystemCalls::ShowInExplorer(captureDirectory.string());
}

void Editor::Panels::MenuBar::CreateDebuggingMenu()
{
	m_debuggingMenu = &CreateWidget<Widgets::MenuList>("Debugging");
	// RenderDoc availability and the toggles can change while the editor runs,
	// so the presentation is refreshed every time the menu is opened.
	m_debuggingMenu->ClickedEvent += std::bind(&MenuBar::RefreshDebuggingMenuStatus, this);

	auto* debugSettings = &Settings::EditorSettings::GetDebugDrawSettingsObject();
	auto& debugDrawMenu = m_debuggingMenu->CreateWidget<Widgets::MenuList>("Debug Draw");
	debugDrawMenu.CreateWidget<Widgets::MenuItem>("Enabled", "", true, debugSettings->debugDrawEnabled).ValueChangedEvent += [debugSettings](bool p_value) { debugSettings->debugDrawEnabled = p_value; };
	debugDrawMenu.CreateWidget<Widgets::MenuItem>("Grid", "", true, debugSettings->debugDrawGrid).ValueChangedEvent += [debugSettings](bool p_value) { debugSettings->debugDrawGrid = p_value; };
	debugDrawMenu.CreateWidget<Widgets::MenuItem>("Bounds", "", true, debugSettings->debugDrawBounds).ValueChangedEvent += [debugSettings](bool p_value) { debugSettings->debugDrawBounds = p_value; };
	debugDrawMenu.CreateWidget<Widgets::MenuItem>("Cameras", "", true, debugSettings->debugDrawCamera).ValueChangedEvent += [debugSettings](bool p_value) { debugSettings->debugDrawCamera = p_value; };
	debugDrawMenu.CreateWidget<Widgets::MenuItem>("Lights", "", true, debugSettings->debugDrawLighting).ValueChangedEvent += [debugSettings](bool p_value) { debugSettings->debugDrawLighting = p_value; };

	m_wireframeModeItem = &m_debuggingMenu->CreateWidget<Widgets::MenuItem>("Wireframe Mode", "", true, false);
	m_wireframeModeItem->tooltip = "Draw scene geometry as wireframe.";
	m_wireframeModeItem->ValueChangedEvent += [this](bool p_value)
	{
		if (auto* driver = EDITOR_CONTEXT(driver).get())
		{
			Render::Context::DriverUIAccess::SetPolygonMode(
				*driver,
				p_value ? Render::Settings::ERasterizationMode::LINE : Render::Settings::ERasterizationMode::FILL);
		}
	};

	auto& renderDocMenu = m_debuggingMenu->CreateWidget<Widgets::MenuList>("RenderDoc");
	m_renderDocStatusText = &renderDocMenu.CreateWidget<Widgets::Text>("Status: Unknown");
	m_renderDocEnabledItem = &renderDocMenu.CreateWidget<Widgets::MenuItem>("Enabled", "", true, false);
	m_renderDocEnabledItem->ValueChangedEvent += [this](bool p_enabled)
	{
		if (auto* driver = EDITOR_CONTEXT(driver).get())
			Render::Context::DriverUIAccess::SetRenderDocEnabled(*driver, p_enabled);
		RefreshDebuggingMenuStatus();
	};
	m_renderDocCaptureItem = &renderDocMenu.CreateWidget<Widgets::MenuItem>("Capture Next Frame", "F11");
	m_renderDocCaptureItem->ClickedEvent += []
	{
		if (auto* driver = EDITOR_CONTEXT(driver).get())
			Render::Context::DriverUIAccess::QueueRenderDocCapture(*driver, "Editor");
	};
	m_renderDocOpenLatestItem = &renderDocMenu.CreateWidget<Widgets::MenuItem>("Open Latest Capture", "CTRL + F11");
	m_renderDocOpenLatestItem->ClickedEvent += []
	{
		if (auto* driver = EDITOR_CONTEXT(driver).get())
			Render::Context::DriverUIAccess::OpenLatestRenderDocCapture(*driver);
	};
	renderDocMenu.CreateWidget<Widgets::MenuItem>("Open Capture Folder").ClickedEvent += []
	{
		if (auto* driver = EDITOR_CONTEXT(driver).get())
		{
			const auto captureDirectory = Render::Context::DriverUIAccess::GetRenderDocCaptureDirectory(*driver);
			if (!captureDirectory.empty())
				Platform::SystemCalls::ShowInExplorer(captureDirectory);
		}
	};
	m_renderDocAutoOpenItem = &renderDocMenu.CreateWidget<Widgets::MenuItem>("Auto Open Replay UI", "", true, false);
	m_renderDocAutoOpenItem->ValueChangedEvent += [this](bool p_enabled)
	{
		if (auto* driver = EDITOR_CONTEXT(driver).get())
			Render::Context::DriverUIAccess::SetRenderDocAutoOpenEnabled(*driver, p_enabled);
	};

	RefreshDebuggingMenuStatus();
}

void Editor::Panels::MenuBar::RefreshDebuggingMenuStatus()
{
	auto* driver = EDITOR_CONTEXT(driver).get();
	if (driver == nullptr)
		return;

	const bool renderDocAvailable = Render::Context::DriverUIAccess::IsRenderDocAvailable(*driver);
	const bool renderDocEnabled = renderDocAvailable && Render::Context::DriverUIAccess::IsRenderDocEnabled(*driver);

	if (m_renderDocStatusText)
	{
		m_renderDocStatusText->content = renderDocAvailable
			? (renderDocEnabled ? "Status: Enabled" : "Status: Available (disabled)")
			: "Status: Not installed or not loaded";
	}

	if (m_renderDocEnabledItem)
	{
		m_renderDocEnabledItem->checked = renderDocEnabled;
		m_renderDocEnabledItem->enabled = renderDocAvailable;
		m_renderDocEnabledItem->tooltip = renderDocAvailable
			? std::string {}
			: "RenderDoc was not detected. Install it or set RENDERDOC_PATH, then restart the editor.";
	}

	if (m_renderDocAutoOpenItem)
		m_renderDocAutoOpenItem->checked = Render::Context::DriverUIAccess::GetRenderDocAutoOpenEnabled(*driver);

	if (m_renderDocCaptureItem)
		m_renderDocCaptureItem->enabled = renderDocAvailable;

	if (m_renderDocOpenLatestItem)
		m_renderDocOpenLatestItem->enabled = renderDocAvailable;
}

void Editor::Panels::MenuBar::CreateHelpMenu()
{
    auto& helpMenu = CreateWidget<Widgets::MenuList>("Help");
    helpMenu.CreateWidget<Widgets::MenuItem>("GitHub").ClickedEvent += [] {Platform::SystemCalls::OpenURL("https://github.com/NullusEngine/Nullus"); };
    helpMenu.CreateWidget<Widgets::Separator>();
    helpMenu.CreateWidget<Widgets::MenuItem>("Bug Report").ClickedEvent += [] {Platform::SystemCalls::OpenURL("https://github.com/NullusEngine/Nullus/issues/new?assignees=&labels=Bug&template=bug_report.md&title="); };
    helpMenu.CreateWidget<Widgets::MenuItem>("Feature Request").ClickedEvent += [] {Platform::SystemCalls::OpenURL("https://github.com/NullusEngine/Nullus/issues/new?assignees=&labels=Feature&template=feature_request.md&title="); };
    helpMenu.CreateWidget<Widgets::Separator>();
    helpMenu.CreateWidget<Widgets::Text>("Version: 1.0.0");
}

void Editor::Panels::MenuBar::RegisterPanel(const std::string& p_name, UI::PanelWindow& p_panel)
{
	auto& menuItem = m_windowMenu->CreateWidget<Widgets::MenuItem>(p_name, "", true, true);
	menuItem.ValueChangedEvent += [&p_panel](const bool opened)
	{
		p_panel.SetOpened(opened);
		if (opened)
			p_panel.Focus();
	};

	m_panels.emplace(p_name, std::make_pair(std::ref(p_panel), std::ref(menuItem)));
}

void Editor::Panels::MenuBar::RegisterProjectSettingsPanel(ProjectSettings& p_panel)
{
    m_projectSettingsPanel = &p_panel;
}

bool Editor::Panels::MenuBar::RegisterCommand(const std::string& p_menuPath, std::function<void()> p_execute)
{
    if (p_menuPath.empty() || !p_execute)
        return false;

    // Only the last separator is structural, so a leaf label may itself contain
    // a slash without silently creating another menu level.
    const auto separator = p_menuPath.find_last_of('/');
    const auto topLevel = separator == std::string::npos ? p_menuPath : p_menuPath.substr(0u, separator);
    const auto leaf = separator == std::string::npos ? p_menuPath : p_menuPath.substr(separator + 1u);
    if (topLevel.empty() || leaf.empty())
        return false;

    UI::Widgets::MenuList* menu = nullptr;
    if (topLevel == "Window")
    {
        menu = m_windowMenu;
    }
    else
    {
        // Scripted editors may only extend the Window menu with their command
        // entries or use the Tools menu; anything else would let a script
        // shadow built-in menus such as File or Build.
        if (topLevel != "Tools")
            return false;

        const auto existing = m_commandMenus.find(topLevel);
        if (existing != m_commandMenus.end())
        {
            menu = existing->second;
        }
        else
        {
            menu = &CreateWidget<UI::Widgets::MenuList>(topLevel);
            m_commandMenus.emplace(topLevel, menu);
        }
    }

    if (menu == nullptr)
        return false;

    auto& item = menu->CreateWidget<UI::Widgets::MenuItem>(leaf);
    item.ClickedEvent += std::move(p_execute);
    return true;
}

void Editor::Panels::MenuBar::UpdateToggleableItems()
{
	for (auto&[name, panel] : m_panels)
		panel.second.get().checked = panel.first.get().IsOpened();
}

void Editor::Panels::MenuBar::UpdateShortcutLabels()
{
	if (!NLS::Core::ServiceLocator::Contains<Shortcuts::EditorShortcutService>())
		return;

	auto& shortcuts = NLS::Core::ServiceLocator::Get<Shortcuts::EditorShortcutService>();
	if (m_newSceneItem)
		m_newSceneItem->shortcut = shortcuts.GetBindingDisplayText("file.new-scene");
	if (m_saveSceneItem)
		m_saveSceneItem->shortcut = shortcuts.GetBindingDisplayText("file.save-scene");
	if (m_saveSceneAsItem)
		m_saveSceneAsItem->shortcut = shortcuts.GetBindingDisplayText("file.save-scene-as");
	if (m_undoItem)
		m_undoItem->shortcut = shortcuts.GetBindingDisplayText("edit.undo");
	if (m_redoItem)
		m_redoItem->shortcut = shortcuts.GetBindingDisplayText("edit.redo");
	if (m_renderDocCaptureItem)
		m_renderDocCaptureItem->shortcut = shortcuts.GetBindingDisplayText("debug.renderdoc.capture-next-frame");
	if (m_renderDocOpenLatestItem)
		m_renderDocOpenLatestItem->shortcut = shortcuts.GetBindingDisplayText("debug.renderdoc.open-latest-capture");

	if (!NLS::Core::ServiceLocator::Contains<Undo::EditorUndoService>())
	{
		if (m_undoItem)
		{
			m_undoItem->enabled = false;
			m_undoItem->tooltip = "The global Undo service is unavailable.";
		}
		if (m_redoItem)
		{
			m_redoItem->enabled = false;
			m_redoItem->tooltip = "The global Undo service is unavailable.";
		}
		return;
	}

	const auto history = NLS::Core::ServiceLocator::Get<Undo::EditorUndoService>().GetHistoryState();
	if (m_undoItem)
	{
		m_undoItem->name = history.undoLabel.empty() ? "Undo" : "Undo " + history.undoLabel;
		m_undoItem->enabled = history.canUndo;
		m_undoItem->tooltip = history.canUndo ? std::string {} :
			NLS::Core::ServiceLocator::Get<Undo::EditorUndoService>().GetUndoUnavailableReason();
	}
	if (m_redoItem)
	{
		m_redoItem->name = history.redoLabel.empty() ? "Redo" : "Redo " + history.redoLabel;
		m_redoItem->enabled = history.canRedo;
		m_redoItem->tooltip = history.canRedo ? std::string {} :
			NLS::Core::ServiceLocator::Get<Undo::EditorUndoService>().GetRedoUnavailableReason();
	}
}

void Editor::Panels::MenuBar::OpenEveryWindows(bool p_state)
{
	for (auto&[name, panel] : m_panels)
		panel.first.get().SetOpened(p_state);
}

void Editor::Panels::MenuBar::OpenProjectSettings()
{
    if (m_projectSettingsPanel)
        m_projectSettingsPanel->Open();
}

#include <Utils/SystemCalls.h>

#include <UI/Widgets/Visual/Separator.h>
#include <UI/Widgets/Sliders/SliderInt.h>
#include <UI/Widgets/Sliders/SliderFloat.h>
#include <UI/Widgets/Drags/DragFloat.h>
#include <UI/Widgets/Selection/ColorEdit.h>

#include "Panels/MenuBar.h"
#include "Panels/SceneView.h"
#include "Panels/AssetView.h"
#include "Panels/ProjectSettings.h"
#include "Core/EditorActions.h"
#include "Settings/EditorSettings.h"
#include "Utils/GameObjectCreationMenu.h"
#include "Rendering/Context/DriverAccess.h"
#include "Shortcuts/EditorShortcutService.h"
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
	CreateHelpMenu();
}

void Editor::Panels::MenuBar::HandleShortcuts(float p_deltaTime)
{
    (void)p_deltaTime;
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

void Editor::Panels::MenuBar::InitializeSettingsMenu()
{
    if (!m_settingsMenu)
        return;

	m_settingsMenu->CreateWidget<Widgets::MenuItem>("Spawn GameObjects at origin", "", true, true).ValueChangedEvent += EDITOR_BIND(SetGameObjectSpawnAtOrigin, std::placeholders::_1);
	m_settingsMenu->CreateWidget<Widgets::MenuItem>("Vertical Synchronization", "", true, true).ValueChangedEvent += [this](bool p_value) { EDITOR_CONTEXT(device)->SetVsync(p_value); };
	auto& cameraSpeedMenu = m_settingsMenu->CreateWidget<Widgets::MenuList>("Camera Speed");
	cameraSpeedMenu.CreateWidget<Widgets::SliderInt>(1, 50, 15, Widgets::ESliderOrientation::HORIZONTAL, "Scene View").ValueChangedEvent += EDITOR_BIND(SetSceneViewCameraSpeed, std::placeholders::_1);
	cameraSpeedMenu.CreateWidget<Widgets::SliderInt>(1, 50, 15, Widgets::ESliderOrientation::HORIZONTAL, "Asset View").ValueChangedEvent += EDITOR_BIND(SetAssetViewCameraSpeed, std::placeholders::_1);
	auto& cameraPositionMenu = m_settingsMenu->CreateWidget<Widgets::MenuList>("Reset Camera");
	cameraPositionMenu.CreateWidget<Widgets::MenuItem>("Scene View").ClickedEvent += EDITOR_BIND(ResetSceneViewCameraPosition);
	cameraPositionMenu.CreateWidget<Widgets::MenuItem>("Asset View").ClickedEvent += EDITOR_BIND(ResetAssetViewCameraPosition);

	auto& sceneView = EDITOR_PANEL(Panels::SceneView, "Scene View");
	auto& assetView = EDITOR_PANEL(Panels::AssetView, "Asset View");

	auto& viewColors = m_settingsMenu->CreateWidget<Widgets::MenuList>("View Colors");
	auto& sceneViewBackground = viewColors.CreateWidget<Widgets::MenuList>("Scene View Background");
	auto& sceneViewBackgroundPicker = sceneViewBackground.CreateWidget<Widgets::ColorEdit>(false, sceneView.GetCamera()->GetClearColor());
	sceneViewBackgroundPicker.ColorChangedEvent += [&](const auto& color)
	{
		sceneView.GetCamera()->SetClearColor({ color.r, color.g, color.b });
	};
	sceneViewBackground.CreateWidget<Widgets::MenuItem>("Reset").ClickedEvent += [&]
	{
		sceneView.ResetClearColor();
		sceneViewBackgroundPicker.color = sceneView.GetCamera()->GetClearColor();
	};
	auto& sceneViewGrid = viewColors.CreateWidget<Widgets::MenuList>("Scene View Grid");

	auto& sceneViewGridPicker = sceneViewGrid.CreateWidget<Widgets::ColorEdit>(false, sceneView.GetGridColor());
	sceneViewGridPicker.ColorChangedEvent += [this](const auto& color)
	{
		EDITOR_PANEL(Panels::SceneView, "Scene View").SetGridColor({ color.r, color.g, color.b });
	};
	sceneViewGrid.CreateWidget<Widgets::MenuItem>("Reset").ClickedEvent += [&]
	{
		sceneView.ResetGridColor();
		sceneViewGridPicker.color = sceneView.GetGridColor();
	};

	auto& assetViewBackground = viewColors.CreateWidget<Widgets::MenuList>("Asset View Background");
	auto& assetViewBackgroundPicker = assetViewBackground.CreateWidget<Widgets::ColorEdit>(false, assetView.GetCamera()->GetClearColor());
	assetViewBackgroundPicker.ColorChangedEvent += [&](const auto& color)
	{
		assetView.GetCamera()->SetClearColor({ color.r, color.g, color.b });
	};
	assetViewBackground.CreateWidget<Widgets::MenuItem>("Reset").ClickedEvent += [&]
	{
		assetView.ResetClearColor();
		assetViewBackgroundPicker.color = assetView.GetCamera()->GetClearColor();
	};

	auto& assetViewGrid = viewColors.CreateWidget<Widgets::MenuList>("Asset View Grid");
	auto& assetViewGridPicker = assetViewGrid.CreateWidget<Widgets::ColorEdit>(false, assetView.GetGridColor());
	assetViewGridPicker.ColorChangedEvent += [&](const auto& color)
	{
		assetView.SetGridColor({ color.r, color.g, color.b });
	};
	assetViewGrid.CreateWidget<Widgets::MenuItem>("Reset").ClickedEvent += [&]
	{
		assetView.ResetGridColor();
		assetViewGridPicker.color = assetView.GetGridColor();
	};

	auto* debugSettings = &Settings::EditorSettings::GetDebugDrawSettingsObject();
	auto* sceneToolSettings = &Settings::EditorSettings::GetSceneToolSettingsObject();
	auto* runtimeSettings = &Settings::EditorSettings::GetRuntimeSettingsObject();

	auto& sceneViewBillboardScaleMenu = m_settingsMenu->CreateWidget<Widgets::MenuList>("3D Icons Scales");
	auto& lightBillboardScaleSlider = sceneViewBillboardScaleMenu.CreateWidget<Widgets::SliderInt>(0, 100, static_cast<int>(debugSettings->lightBillboardScale * 100.0f), Widgets::ESliderOrientation::HORIZONTAL, "Lights");
	lightBillboardScaleSlider.ValueChangedEvent += [debugSettings](int p_value) { debugSettings->lightBillboardScale = p_value / 100.0f; };
	lightBillboardScaleSlider.format = "%d %%";

	auto& snappingMenu = m_settingsMenu->CreateWidget<Widgets::MenuList>("Snapping");
	snappingMenu.CreateWidget<Widgets::DragFloat>(0.001f, 999999.0f, sceneToolSettings->translationSnapUnit, 0.05f, "Translation Unit").ValueChangedEvent += [sceneToolSettings](float p_value) { sceneToolSettings->translationSnapUnit = p_value; };
	snappingMenu.CreateWidget<Widgets::DragFloat>(0.001f, 999999.0f, sceneToolSettings->rotationSnapUnit, 1.0f, "Rotation Unit").ValueChangedEvent += [sceneToolSettings](float p_value) { sceneToolSettings->rotationSnapUnit = p_value; };
	snappingMenu.CreateWidget<Widgets::DragFloat>(0.001f, 999999.0f, sceneToolSettings->scalingSnapUnit, 0.05f, "Scaling Unit").ValueChangedEvent += [sceneToolSettings](float p_value) { sceneToolSettings->scalingSnapUnit = p_value; };

	auto& performanceMenu = m_settingsMenu->CreateWidget<Widgets::MenuList>("Performance");
	performanceMenu.CreateWidget<Widgets::MenuItem>(
		"Power Saving Idle Pacing",
		"",
		true,
		runtimeSettings->enablePowerSavingIdlePacing).ValueChangedEvent += [runtimeSettings](bool p_value)
	{
		runtimeSettings->enablePowerSavingIdlePacing = p_value;
	};

	auto& debuggingMenu = m_settingsMenu->CreateWidget<Widgets::MenuList>("Debugging");
	auto& debugDrawMenu = debuggingMenu.CreateWidget<Widgets::MenuList>("Debug Draw");
	debugDrawMenu.CreateWidget<Widgets::MenuItem>("Enabled", "", true, debugSettings->debugDrawEnabled).ValueChangedEvent += [debugSettings](bool p_value) { debugSettings->debugDrawEnabled = p_value; };
	debugDrawMenu.CreateWidget<Widgets::MenuItem>("Grid", "", true, debugSettings->debugDrawGrid).ValueChangedEvent += [debugSettings](bool p_value) { debugSettings->debugDrawGrid = p_value; };
	debugDrawMenu.CreateWidget<Widgets::MenuItem>("Bounds", "", true, debugSettings->debugDrawBounds).ValueChangedEvent += [debugSettings](bool p_value) { debugSettings->debugDrawBounds = p_value; };
	debugDrawMenu.CreateWidget<Widgets::MenuItem>("Cameras", "", true, debugSettings->debugDrawCamera).ValueChangedEvent += [debugSettings](bool p_value) { debugSettings->debugDrawCamera = p_value; };
	debugDrawMenu.CreateWidget<Widgets::MenuItem>("Lights", "", true, debugSettings->debugDrawLighting).ValueChangedEvent += [debugSettings](bool p_value) { debugSettings->debugDrawLighting = p_value; };
    debuggingMenu.CreateWidget<Widgets::MenuItem>("Wireframe Mode", "", true, false).ValueChangedEvent += [this](bool p_value)
    {
        Render::Context::DriverUIAccess::SetPolygonMode(
            *EDITOR_CONTEXT(driver),
            p_value ? NLS::Render::Settings::ERasterizationMode::LINE : NLS::Render::Settings::ERasterizationMode::FILL);
    };
	auto& renderDocMenu = debuggingMenu.CreateWidget<Widgets::MenuList>("RenderDoc");
	auto& renderDocStatus = renderDocMenu.CreateWidget<Widgets::Text>("Status: Unknown");
	renderDocStatus.content = Render::Context::DriverUIAccess::IsRenderDocAvailable(*EDITOR_CONTEXT(driver))
		? (Render::Context::DriverUIAccess::IsRenderDocEnabled(*EDITOR_CONTEXT(driver)) ? "Status: Enabled" : "Status: Available (disabled)")
		: "Status: Not installed or not loaded";
	renderDocMenu.CreateWidget<Widgets::MenuItem>(
		"Enabled",
		"",
		true,
		Render::Context::DriverUIAccess::IsRenderDocEnabled(*EDITOR_CONTEXT(driver))).ValueChangedEvent += [&renderDocStatus](bool enabled)
	{
		Render::Context::DriverUIAccess::SetRenderDocEnabled(*EDITOR_CONTEXT(driver), enabled);
		renderDocStatus.content = enabled ? "Status: Enabled" : "Status: Available (disabled)";
	};
	m_renderDocCaptureItem = &renderDocMenu.CreateWidget<Widgets::MenuItem>("Capture Next Frame", "F11");
	m_renderDocCaptureItem->ClickedEvent += []
	{
		Render::Context::DriverUIAccess::QueueRenderDocCapture(*EDITOR_CONTEXT(driver), "Editor");
	};
	m_renderDocOpenLatestItem = &renderDocMenu.CreateWidget<Widgets::MenuItem>("Open Latest Capture", "CTRL + F11");
	m_renderDocOpenLatestItem->ClickedEvent += []
	{
		Render::Context::DriverUIAccess::OpenLatestRenderDocCapture(*EDITOR_CONTEXT(driver));
	};
	renderDocMenu.CreateWidget<Widgets::MenuItem>("Open Capture Folder").ClickedEvent += []
	{
		const auto captureDirectory = Render::Context::DriverUIAccess::GetRenderDocCaptureDirectory(*EDITOR_CONTEXT(driver));
		if (!captureDirectory.empty())
			Platform::SystemCalls::ShowInExplorer(captureDirectory);
	};
	renderDocMenu.CreateWidget<Widgets::MenuItem>(
		"Auto Open Replay UI",
		"",
		true,
		Render::Context::DriverUIAccess::GetRenderDocAutoOpenEnabled(*EDITOR_CONTEXT(driver))).ValueChangedEvent += [](bool enabled)
	{
		Render::Context::DriverUIAccess::SetRenderDocAutoOpenEnabled(*EDITOR_CONTEXT(driver), enabled);
	};
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
	menuItem.ValueChangedEvent += std::bind(&UI::PanelWindow::SetOpened, &p_panel, std::placeholders::_1);

	m_panels.emplace(p_name, std::make_pair(std::ref(p_panel), std::ref(menuItem)));
}

void Editor::Panels::MenuBar::RegisterProjectSettingsPanel(ProjectSettings& p_panel)
{
    m_projectSettingsPanel = &p_panel;
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
	if (m_renderDocCaptureItem)
		m_renderDocCaptureItem->shortcut = shortcuts.GetBindingDisplayText("debug.renderdoc.capture-next-frame");
	if (m_renderDocOpenLatestItem)
		m_renderDocOpenLatestItem->shortcut = shortcuts.GetBindingDisplayText("debug.renderdoc.open-latest-capture");
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

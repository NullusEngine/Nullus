#include <Utils/SystemCalls.h>

#include <UI/Widgets/Visual/Separator.h>
#include <UI/Widgets/Sliders/SliderInt.h>
#include <UI/Widgets/Sliders/SliderFloat.h>
#include <UI/Widgets/Drags/DragFloat.h>
#include <UI/Widgets/Selection/ColorEdit.h>

#include "Panels/MenuBar.h"
#include "Panels/SceneView.h"
#include "Panels/AssetView.h"
#include "Core/EditorActions.h"
#include "Settings/EditorSettings.h"
#include "Utils/ActorCreationMenu.h"
#include "UI/Widgets/Texts/Text.h"
using namespace NLS;
using namespace NLS::UI;
using namespace NLS::Engine::Components;

Editor::Panels::MenuBar::MenuBar()
{
	CreateFileMenu();
	CreateBuildMenu();
	CreateWindowMenu();
	CreateActorsMenu();
	CreateResourcesMenu();
	CreateSettingsMenu();
	CreateLayoutMenu();
	CreateHelpMenu();
}

void Editor::Panels::MenuBar::HandleShortcuts(float p_deltaTime)
{
	auto& inputManager = *EDITOR_CONTEXT(inputManager);

	if (inputManager.GetKeyState(Windowing::Inputs::EKey::KEY_LEFT_CONTROL) == Windowing::Inputs::EKeyState::KEY_DOWN)
	{
		if (inputManager.IsKeyPressed(Windowing::Inputs::EKey::KEY_N))
			EDITOR_EXEC(LoadEmptyScene());

		if (inputManager.IsKeyPressed(Windowing::Inputs::EKey::KEY_S))
		{
			if (inputManager.GetKeyState(Windowing::Inputs::EKey::KEY_LEFT_SHIFT) == Windowing::Inputs::EKeyState::KEY_UP)
				EDITOR_EXEC(SaveSceneChanges());
			else
				EDITOR_EXEC(SaveAs());
		}
	}
}

void Editor::Panels::MenuBar::InitializeSettingsMenu()
{
	m_settingsMenu->CreateWidget<Widgets::MenuItem>("Spawn actors at origin", "", true, true).ValueChangedEvent += EDITOR_BIND(SetActorSpawnAtOrigin, std::placeholders::_1);
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

	auto& sceneViewBillboardScaleMenu = m_settingsMenu->CreateWidget<Widgets::MenuList>("3D Icons Scales");
	auto& lightBillboardScaleSlider = sceneViewBillboardScaleMenu.CreateWidget<Widgets::SliderInt>(0, 100, static_cast<int>(Settings::EditorSettings::LightBillboardScale * 100.0f), Widgets::ESliderOrientation::HORIZONTAL, "Lights");
	lightBillboardScaleSlider.ValueChangedEvent += [this](int p_value) { Settings::EditorSettings::LightBillboardScale = p_value / 100.0f; };
	lightBillboardScaleSlider.format = "%d %%";

	auto& snappingMenu = m_settingsMenu->CreateWidget<Widgets::MenuList>("Snapping");
	snappingMenu.CreateWidget<Widgets::DragFloat>(0.001f, 999999.0f, Settings::EditorSettings::TranslationSnapUnit, 0.05f, "Translation Unit").ValueChangedEvent += [this](float p_value) { Settings::EditorSettings::TranslationSnapUnit = p_value; };
	snappingMenu.CreateWidget<Widgets::DragFloat>(0.001f, 999999.0f, Settings::EditorSettings::RotationSnapUnit, 1.0f, "Rotation Unit").ValueChangedEvent += [this](float p_value) { Settings::EditorSettings::RotationSnapUnit = p_value; };
	snappingMenu.CreateWidget<Widgets::DragFloat>(0.001f, 999999.0f, Settings::EditorSettings::ScalingSnapUnit, 0.05f, "Scaling Unit").ValueChangedEvent += [this](float p_value) { Settings::EditorSettings::ScalingSnapUnit = p_value; };

	auto& debuggingMenu = m_settingsMenu->CreateWidget<Widgets::MenuList>("Debugging");
	debuggingMenu.CreateWidget<Widgets::MenuItem>("Show geometry bounds", "", true, Settings::EditorSettings::ShowGeometryBounds).ValueChangedEvent += [this](bool p_value) { Settings::EditorSettings::ShowGeometryBounds = p_value; };
	debuggingMenu.CreateWidget<Widgets::MenuItem>("Show lights bounds", "", true, Settings::EditorSettings::ShowLightBounds).ValueChangedEvent += [this](bool p_value) { Settings::EditorSettings::ShowLightBounds = p_value; };
    debuggingMenu.CreateWidget<Widgets::MenuItem>("Wireframe Mode", "", true, false).ValueChangedEvent += [this](bool p_value)
    { EDITOR_CONTEXT(driver)->SetPolygonMode(p_value ? NLS::Render::Settings::ERasterizationMode::LINE : NLS::Render::Settings::ERasterizationMode::FILL); };
	auto& subMenu = debuggingMenu.CreateWidget<Widgets::MenuList>("Frustum culling visualizer...");
	subMenu.CreateWidget<Widgets::MenuItem>("For geometry", "", true, Settings::EditorSettings::ShowGeometryFrustumCullingInSceneView).ValueChangedEvent += [this](bool p_value) { Settings::EditorSettings::ShowGeometryFrustumCullingInSceneView = p_value; };
	subMenu.CreateWidget<Widgets::MenuItem>("For lights", "", true, Settings::EditorSettings::ShowLightFrustumCullingInSceneView).ValueChangedEvent += [this](bool p_value) { Settings::EditorSettings::ShowLightFrustumCullingInSceneView = p_value; };
}

void Editor::Panels::MenuBar::CreateFileMenu()
{
	auto& fileMenu = CreateWidget<Widgets::MenuList>("File");
	fileMenu.CreateWidget<Widgets::MenuItem>("New Scene", "CTRL + N").ClickedEvent					+= EDITOR_BIND(LoadEmptyScene);
	fileMenu.CreateWidget<Widgets::MenuItem>("Save Scene", "CTRL + S").ClickedEvent					+= EDITOR_BIND(SaveSceneChanges);
	fileMenu.CreateWidget<Widgets::MenuItem>("Save Scene As...", "CTRL + SHIFT + S").ClickedEvent	+= EDITOR_BIND(SaveAs);
	fileMenu.CreateWidget<Widgets::MenuItem>("Exit", "ALT + F4").ClickedEvent						+= [] { EDITOR_CONTEXT(window)->SetShouldClose(true); };
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
	m_windowMenu->CreateWidget<Widgets::MenuItem>("Close all").ClickedEvent	+= std::bind(&MenuBar::OpenEveryWindows, this, false);
	m_windowMenu->CreateWidget<Widgets::MenuItem>("Open all").ClickedEvent		+= std::bind(&MenuBar::OpenEveryWindows, this, true);
	m_windowMenu->CreateWidget<Widgets::Separator>();

	/* When the menu is opened, we update which window is marked as "Opened" or "Closed" */
	m_windowMenu->ClickedEvent += std::bind(&MenuBar::UpdateToggleableItems, this);
}

void Editor::Panels::MenuBar::CreateActorsMenu()
{
	auto& actorsMenu = CreateWidget<Widgets::MenuList>("Actors");
    Utils::ActorCreationMenu::GenerateActorCreationMenu(actorsMenu);
}

void Editor::Panels::MenuBar::CreateResourcesMenu()
{
	auto& resourcesMenu = CreateWidget<Widgets::MenuList>("Resources");
	resourcesMenu.CreateWidget<Widgets::MenuItem>("Compile shaders").ClickedEvent += EDITOR_BIND(CompileShaders);
	resourcesMenu.CreateWidget<Widgets::MenuItem>("Save materials").ClickedEvent += EDITOR_BIND(SaveMaterials);
}

void Editor::Panels::MenuBar::CreateSettingsMenu()
{
	m_settingsMenu = &CreateWidget<Widgets::MenuList>("Settings");
}

void Editor::Panels::MenuBar::CreateLayoutMenu() 
{
	auto& layoutMenu = CreateWidget<Widgets::MenuList>("Layout");
	layoutMenu.CreateWidget<Widgets::MenuItem>("Reset").ClickedEvent += EDITOR_BIND(ResetLayout);
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

void Editor::Panels::MenuBar::UpdateToggleableItems()
{
	for (auto&[name, panel] : m_panels)
		panel.second.get().checked = panel.first.get().IsOpened();
}

void Editor::Panels::MenuBar::OpenEveryWindows(bool p_state)
{
	for (auto&[name, panel] : m_panels)
		panel.first.get().SetOpened(p_state);
}

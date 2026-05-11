#include <gtest/gtest.h>

#include <Reflection/ReflectionDatabase.h>
#include <Reflection/RuntimeMetaProperties.h>
#include <imgui.h>

#include "Core/EditorInteractionBlocker.h"
#include "Panels/ProjectSettings.h"
#include "Settings/EditorSettings.h"
#include "Settings/EditorSettingsRegistry.h"

TEST(ProjectSettingsPanelTests, SelectionFallsBackToFirstVisibleSearchResult)
{
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Settings::EditorSettingsRegistry registry;
    NLS::Editor::Settings::EditorSettings::RegisterSettingObjects(registry);

    const auto selected = NLS::Editor::Panels::ProjectSettings::ChooseSelectionAfterSearch(
        registry,
        "editor.scene-tools",
        "debug");

    EXPECT_EQ(selected, "editor.debug-draw");
}

TEST(ProjectSettingsPanelTests, SelectionIsClearedWhenSearchHasNoResults)
{
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Settings::EditorSettingsRegistry registry;
    NLS::Editor::Settings::EditorSettings::RegisterSettingObjects(registry);

    const auto selected = NLS::Editor::Panels::ProjectSettings::ChooseSelectionAfterSearch(
        registry,
        "editor.scene-tools",
        "not-present");

    EXPECT_TRUE(selected.empty());
}

TEST(ProjectSettingsPanelTests, SettingsModalParticipatesInSceneInputBlocking)
{
    EXPECT_FALSE(NLS::Editor::Core::DoesEditorModalBlockSceneInput());
    NLS::Editor::Core::SetSettingsWindowBlocksSceneInput(true);
    EXPECT_TRUE(NLS::Editor::Core::DoesEditorModalBlockSceneInput());
    NLS::Editor::Core::SetSettingsWindowBlocksSceneInput(false);
    EXPECT_FALSE(NLS::Editor::Core::DoesEditorModalBlockSceneInput());
}

TEST(ProjectSettingsPanelTests, RuntimeRestartFieldsAreMarkedByMetadata)
{
    NLS::meta::ReflectionDatabase::Instance();
    const auto runtimeType = NLS_TYPEOF(NLS::Editor::Settings::EditorRuntimeSettingsObject);

    const auto& threadedRendering = runtimeType.GetField("enableThreadedRendering");
    const auto& renderDocEnabled = runtimeType.GetField("renderDocEnabled");

    ASSERT_TRUE(threadedRendering.IsValid());
    ASSERT_TRUE(renderDocEnabled.IsValid());
    EXPECT_NE(threadedRendering.GetMeta().GetProperty<NLS::meta::RequiresRestart>(), nullptr);
    EXPECT_EQ(renderDocEnabled.GetMeta().GetProperty<NLS::meta::RequiresRestart>(), nullptr);
}

TEST(ProjectSettingsPanelTests, SettingsModalCanDrawWhenPanelIsDisabledForDocking)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    NLS::Editor::Panels::ProjectSettings settings("Project Settings", false, NLS::UI::PanelWindowSettings {});
    settings.enabled = false;

    ImGui::NewFrame();
    settings.Open();
    settings.DrawModal();

    EXPECT_TRUE(settings.IsModalOpen());
    EXPECT_FALSE(settings.enabled);

    ImGui::EndFrame();
    ImGui::DestroyContext();
}

TEST(ProjectSettingsPanelTests, SettingsModalKeepsReflectedWidgetsStableBetweenFrames)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    NLS::Editor::Panels::ProjectSettings settings("Project Settings", false, NLS::UI::PanelWindowSettings {});
    settings.Open();

    ImGui::NewFrame();
    settings.DrawModal();
    const auto firstWidgetCount = settings.GetWidgets().size();
    const auto* firstWidget = firstWidgetCount > 0 ? settings.GetWidgets().front().first : nullptr;
    ImGui::EndFrame();

    ImGui::NewFrame();
    settings.DrawModal();
    const auto secondWidgetCount = settings.GetWidgets().size();
    const auto* secondWidget = secondWidgetCount > 0 ? settings.GetWidgets().front().first : nullptr;
    ImGui::EndFrame();

    EXPECT_GT(firstWidgetCount, 0u);
    EXPECT_EQ(secondWidgetCount, firstWidgetCount);
    EXPECT_EQ(secondWidget, firstWidget);

    ImGui::DestroyContext();
}

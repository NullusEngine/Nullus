#include <gtest/gtest.h>

#include <Reflection/ReflectionDatabase.h>

#include <algorithm>
#include <filesystem>
#include <string>

#include "Settings/EditorSettings.h"
#include "Settings/EditorSettingsPersistence.h"

namespace
{
std::filesystem::path MakeTempSettingsPath(const char* p_name)
{
    const auto root = std::filesystem::temp_directory_path() / "NullusEditorViewSettingsTests";
    std::filesystem::create_directories(root);
    return root / p_name;
}

NLS::Editor::Settings::EditorSettingsRegistry MakeRegistry()
{
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Settings::EditorSettingsRegistry registry;
    NLS::Editor::Settings::EditorSettings::RegisterSettingObjects(registry);
    return registry;
}

// The view settings are singletons shared by the whole editor, so every test
// restores the defaults it touched.
struct ScopedViewSettingsRestore final
{
    NLS::Editor::Settings::EditorViewSettingsObject& settings;
    NLS::Editor::Settings::EditorViewSettingsObject original;
    ~ScopedViewSettingsRestore() { settings = original; }
};
}

TEST(EditorViewSettingsTests, RegistersViewSettingsAsSearchableEditorSettings)
{
    auto registry = MakeRegistry();

    const auto* viewSettings = registry.Find("editor.view");
    ASSERT_NE(viewSettings, nullptr);
    EXPECT_EQ(viewSettings->displayName, "Views");
    EXPECT_EQ(viewSettings->categoryPath, "Editor/Views");
    EXPECT_EQ(viewSettings->scope, NLS::Editor::Settings::EditorSettingPersistenceScope::User);
    EXPECT_EQ(viewSettings->type, NLS_TYPEOF(NLS::Editor::Settings::EditorViewSettingsObject));

    const auto matches = registry.Search("Scene View Camera Speed");
    EXPECT_TRUE(std::any_of(
        matches.begin(),
        matches.end(),
        [](const auto* object)
        {
            return object != nullptr && object->id == "editor.view";
        }));
}

TEST(EditorViewSettingsTests, ExposesSpeedsAndColorsWithPanelMetadata)
{
    NLS::meta::ReflectionDatabase::Instance();
    const auto& type = NLS_TYPEOF(NLS::Editor::Settings::EditorViewSettingsObject);

    const auto findField = [&type](const char* p_name)
    {
        const auto& fields = type.GetFields();
        const auto it = std::find_if(
            fields.begin(),
            fields.end(),
            [p_name](const auto& field) { return field.GetName() == p_name; });
        return it == fields.end() ? nullptr : &*it;
    };

    const auto* sceneSpeed = findField("sceneViewCameraSpeed");
    ASSERT_NE(sceneSpeed, nullptr);
    EXPECT_FALSE(sceneSpeed->GetMeta().GetProperty<NLS::meta::DisplayName>() == nullptr);
    EXPECT_EQ(sceneSpeed->GetMeta().GetProperty<NLS::meta::DisplayName>()->value, "Scene View Camera Speed");
    const auto* sceneSpeedRange = sceneSpeed->GetMeta().GetProperty<NLS::meta::Range>();
    ASSERT_NE(sceneSpeedRange, nullptr);
    EXPECT_FLOAT_EQ(sceneSpeedRange->min, 1.0f);
    EXPECT_FLOAT_EQ(sceneSpeedRange->max, 50.0f);

    EXPECT_NE(findField("assetViewCameraSpeed"), nullptr);
    EXPECT_NE(findField("sceneViewBackground"), nullptr);
    EXPECT_NE(findField("sceneViewGrid"), nullptr);
    EXPECT_NE(findField("assetViewBackground"), nullptr);
    EXPECT_NE(findField("assetViewGrid"), nullptr);
}

TEST(EditorViewSettingsTests, PersistsSpeedsAndColors)
{
    const auto path = MakeTempSettingsPath("view-settings.json");
    std::filesystem::remove(path);
    auto registry = MakeRegistry();

    auto& viewSettings = NLS::Editor::Settings::EditorSettings::GetViewSettingsObject();
    const ScopedViewSettingsRestore restore {viewSettings, viewSettings};

    viewSettings.sceneViewCameraSpeed = 42.0f;
    viewSettings.assetViewCameraSpeed = 8.0f;
    viewSettings.sceneViewBackground = NLS::Maths::Color(0.25f, 0.5f, 0.75f, 1.0f);
    viewSettings.sceneViewGrid = NLS::Maths::Color(0.1f, 0.2f, 0.3f, 1.0f);
    viewSettings.assetViewBackground = NLS::Maths::Color(1.0f, 0.0f, 0.0f, 1.0f);
    viewSettings.assetViewGrid = NLS::Maths::Color(0.0f, 1.0f, 0.0f, 1.0f);
    ASSERT_TRUE(NLS::Editor::Settings::EditorSettingsPersistence::Save(path, registry));

    viewSettings.sceneViewCameraSpeed = 15.0f;
    viewSettings.assetViewCameraSpeed = 15.0f;
    viewSettings.sceneViewBackground = NLS::Maths::Color {};
    viewSettings.sceneViewGrid = NLS::Maths::Color {};
    viewSettings.assetViewBackground = NLS::Maths::Color {};
    viewSettings.assetViewGrid = NLS::Maths::Color {};
    ASSERT_TRUE(NLS::Editor::Settings::EditorSettingsPersistence::Load(path, registry));

    EXPECT_FLOAT_EQ(viewSettings.sceneViewCameraSpeed, 42.0f);
    EXPECT_FLOAT_EQ(viewSettings.assetViewCameraSpeed, 8.0f);
    EXPECT_FLOAT_EQ(viewSettings.sceneViewBackground.r, 0.25f);
    EXPECT_FLOAT_EQ(viewSettings.sceneViewBackground.g, 0.5f);
    EXPECT_FLOAT_EQ(viewSettings.sceneViewBackground.b, 0.75f);
    EXPECT_FLOAT_EQ(viewSettings.sceneViewGrid.r, 0.1f);
    EXPECT_FLOAT_EQ(viewSettings.sceneViewGrid.g, 0.2f);
    EXPECT_FLOAT_EQ(viewSettings.sceneViewGrid.b, 0.3f);
    EXPECT_FLOAT_EQ(viewSettings.assetViewBackground.r, 1.0f);
    EXPECT_FLOAT_EQ(viewSettings.assetViewGrid.g, 1.0f);

    std::error_code error;
    std::filesystem::remove(path, error);
}

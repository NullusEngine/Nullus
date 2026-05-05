#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include <Reflection/ReflectionDatabase.h>

#include "Settings/EditorSettings.h"
#include "Settings/EditorSettingsPersistence.h"

namespace
{
std::filesystem::path MakeTempSettingsPath(const char* p_name)
{
    const auto root = std::filesystem::temp_directory_path() / "NullusEditorSettingsPersistenceTests";
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
}

TEST(EditorSettingsPersistenceTests, MissingFileKeepsDefaultValues)
{
    auto path = MakeTempSettingsPath("missing.json");
    std::filesystem::remove(path);
    auto registry = MakeRegistry();

    NLS::Editor::Settings::EditorSettings::GetSceneToolSettingsObject().translationSnapUnit = 7.0f;
    EXPECT_TRUE(NLS::Editor::Settings::EditorSettingsPersistence::Load(path, registry));
    EXPECT_FLOAT_EQ(NLS::Editor::Settings::EditorSettings::GetSceneToolSettingsObject().translationSnapUnit, 7.0f);
}

TEST(EditorSettingsPersistenceTests, SavesAndLoadsSupportedValues)
{
    auto path = MakeTempSettingsPath("roundtrip.json");
    auto registry = MakeRegistry();

    auto& sceneTools = NLS::Editor::Settings::EditorSettings::GetSceneToolSettingsObject();
    sceneTools.translationSnapUnit = 2.5f;
    sceneTools.rotationSnapUnit = 45.0f;
    ASSERT_TRUE(NLS::Editor::Settings::EditorSettingsPersistence::Save(path, registry));

    sceneTools.translationSnapUnit = 1.0f;
    sceneTools.rotationSnapUnit = 15.0f;
    ASSERT_TRUE(NLS::Editor::Settings::EditorSettingsPersistence::Load(path, registry));

    EXPECT_FLOAT_EQ(sceneTools.translationSnapUnit, 2.5f);
    EXPECT_FLOAT_EQ(sceneTools.rotationSnapUnit, 45.0f);
}

TEST(EditorSettingsPersistenceTests, IgnoresInvalidValueTypes)
{
    auto path = MakeTempSettingsPath("invalid.json");
    {
        std::ofstream file(path, std::ios::trunc);
        file << R"({"version":1,"objects":{"editor.scene-tools":{"fields":{"translationSnapUnit":"bad"}}}})";
    }

    auto registry = MakeRegistry();
    auto& sceneTools = NLS::Editor::Settings::EditorSettings::GetSceneToolSettingsObject();
    sceneTools.translationSnapUnit = 3.0f;

    EXPECT_TRUE(NLS::Editor::Settings::EditorSettingsPersistence::Load(path, registry));
    EXPECT_FLOAT_EQ(sceneTools.translationSnapUnit, 3.0f);
}

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

TEST(EditorSettingsPersistenceTests, SavesAndLoadsLightGridRenderingSetting)
{
    auto path = MakeTempSettingsPath("lightgrid-rendering.json");
    auto registry = MakeRegistry();

    auto& rendering = NLS::Editor::Settings::EditorSettings::GetRenderingSettingsObject();
    EXPECT_TRUE(rendering.enableLightGrid);

    rendering.enableLightGrid = false;
    ASSERT_TRUE(NLS::Editor::Settings::EditorSettingsPersistence::Save(path, registry));

    rendering.enableLightGrid = true;
    ASSERT_TRUE(NLS::Editor::Settings::EditorSettingsPersistence::Load(path, registry));

    EXPECT_FALSE(rendering.enableLightGrid);
    rendering.enableLightGrid = true;
}

TEST(EditorSettingsPersistenceTests, SavesAndLoadsLargeSceneSettings)
{
    auto path = MakeTempSettingsPath("large-scene.json");
    auto registry = MakeRegistry();

    auto& largeScene = NLS::Editor::Settings::EditorSettings::GetLargeSceneSettingsObject();
    largeScene = {};
    largeScene.enableSpatialIndex = false;
    largeScene.enableHLOD = true;
    largeScene.maxVisibilityJobs = 6;
    largeScene.streamingCpuBudgetUs = 2222;
    largeScene.streamingGpuMemoryBudgetBytes = 300 * 1024 * 1024;

    ASSERT_TRUE(NLS::Editor::Settings::EditorSettingsPersistence::Save(path, registry));

    largeScene = {};
    ASSERT_TRUE(NLS::Editor::Settings::EditorSettingsPersistence::Load(path, registry));

    EXPECT_FALSE(largeScene.enableSpatialIndex);
    EXPECT_TRUE(largeScene.enableHLOD);
    EXPECT_EQ(largeScene.maxVisibilityJobs, 6);
    EXPECT_EQ(largeScene.streamingCpuBudgetUs, 2222);
    EXPECT_EQ(largeScene.streamingGpuMemoryBudgetBytes, 300 * 1024 * 1024);
    largeScene = {};
}

TEST(EditorSettingsPersistenceTests, SavesAndLoadsPowerSavingIdlePacingSetting)
{
    auto path = MakeTempSettingsPath("power-saving-idle-pacing.json");
    auto registry = MakeRegistry();

    auto& runtime = NLS::Editor::Settings::EditorSettings::GetRuntimeSettingsObject();
    const auto oldRuntime = runtime;

    runtime.enablePowerSavingIdlePacing = true;
    ASSERT_TRUE(NLS::Editor::Settings::EditorSettingsPersistence::Save(path, registry));

    runtime.enablePowerSavingIdlePacing = false;
    ASSERT_TRUE(NLS::Editor::Settings::EditorSettingsPersistence::Load(path, registry));

    EXPECT_TRUE(runtime.enablePowerSavingIdlePacing);
    runtime = oldRuntime;
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

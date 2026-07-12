#include <gtest/gtest.h>

#include <Reflection/ReflectionDatabase.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "Settings/EditorSettings.h"
#include "Settings/EditorSettingsPersistence.h"
#include "Settings/EditorSettingsRegistry.h"
#include "Guid.h"

namespace
{
NLS::Editor::Settings::EditorSettingObject MakeSetting(
    std::string p_id,
    std::string p_displayName,
    std::string p_category)
{
    using namespace NLS::Editor::Settings;
    auto& object = EditorSettings::GetSceneToolSettingsObject();
    return {
        std::move(p_id),
        std::move(p_displayName),
        std::move(p_category),
        EditorSettingPersistenceScope::User,
        [&object] { return NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}); },
        NLS_TYPEOF(EditorSceneToolSettingsObject)
    };
}
}

TEST(EditorSettingsRegistryTests, RegistersUniqueSettingsAndRejectsDuplicates)
{
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Settings::EditorSettingsRegistry registry;

    EXPECT_TRUE(registry.Register(MakeSetting("editor.scene-tools", "Scene Tools", "Editor/Scene View")));
    EXPECT_TRUE(registry.Contains("editor.scene-tools"));
    EXPECT_FALSE(registry.Register(MakeSetting("editor.scene-tools", "Duplicate", "Editor")));
}

TEST(EditorSettingsRegistryTests, RegistersDefaultSettingsWithoutCallerPrewarmingReflection)
{
    NLS::Editor::Settings::EditorSettingsRegistry registry;

    NLS::Editor::Settings::EditorSettings::RegisterSettingObjects(registry);

    EXPECT_TRUE(registry.Contains("editor.debug-draw"));
    EXPECT_TRUE(registry.Contains("editor.scene-tools"));
    EXPECT_TRUE(registry.Contains("editor.rendering"));
    EXPECT_TRUE(registry.Contains("editor.large-scene"));
}

TEST(EditorSettingsRegistryTests, RegistersLargeSceneSettingsAsSearchableRenderingSettings)
{
    NLS::Editor::Settings::EditorSettingsRegistry registry;

    NLS::Editor::Settings::EditorSettings::RegisterSettingObjects(registry);

    const auto* largeSceneSettings = registry.Find("editor.large-scene");
    ASSERT_NE(largeSceneSettings, nullptr);
    EXPECT_EQ(largeSceneSettings->displayName, "Large Scene");
    EXPECT_EQ(largeSceneSettings->categoryPath, "Editor/Rendering");
    EXPECT_EQ(largeSceneSettings->scope, NLS::Editor::Settings::EditorSettingPersistenceScope::User);
    EXPECT_EQ(largeSceneSettings->type, NLS_TYPEOF(NLS::Editor::Settings::EditorLargeSceneSettingsObject));

    auto matches = registry.Search("large scene");
    EXPECT_TRUE(std::any_of(
        matches.begin(),
        matches.end(),
        [](const auto* object)
        {
            return object != nullptr && object->id == "editor.large-scene";
        }));

    matches = registry.Search("Streaming CPU Budget");
    EXPECT_TRUE(std::any_of(
        matches.begin(),
        matches.end(),
        [](const auto* object)
        {
            return object != nullptr && object->id == "editor.large-scene";
        }));
}

TEST(EditorSettingsRegistryTests, BuildsEngineLargeSceneSettingsFromEditorSettings)
{
    using namespace NLS::Editor::Settings;

    auto& settings = EditorSettings::GetLargeSceneSettingsObject();
    settings = {};
    settings.enableSpatialIndex = false;
    settings.enableParallelVisibility = false;
    settings.enableLOD = false;
    settings.enableHLOD = true;
    settings.enableHZBOcclusion = true;
    settings.maxVisibilityJobs = 7;
    settings.parallelVisibilityPrimitiveThreshold = 2048;
    settings.parallelVisibilityPrimitivesPerTask = 64;
    settings.staticRebuildDirtyRatio = 0.35;
    settings.staticRebuildBudgetUs = 250;
    settings.streamingCpuBudgetUs = 2000;
    settings.streamingGpuUploadBudgetBytes = 4 * 1024 * 1024;
    settings.streamingIoBudgetBytes = 8 * 1024 * 1024;
    settings.streamingCpuMemoryBudgetBytes = 256 * 1024 * 1024;
    settings.streamingGpuMemoryBudgetBytes = 512 * 1024 * 1024;
    settings.maxOcclusionHistoryAge = 5;

    const auto engineSettings = EditorSettings::BuildLargeSceneSettings();

    EXPECT_FALSE(engineSettings.enableSpatialIndex);
    EXPECT_FALSE(engineSettings.enableParallelVisibility);
    EXPECT_FALSE(engineSettings.enableLOD);
    EXPECT_TRUE(engineSettings.enableHLOD);
    EXPECT_TRUE(engineSettings.enableHZBOcclusion);
    EXPECT_EQ(engineSettings.maxVisibilityJobs, 7u);
    EXPECT_EQ(engineSettings.parallelVisibilityPrimitiveThreshold, 2048u);
    EXPECT_EQ(engineSettings.parallelVisibilityPrimitivesPerTask, 64u);
    EXPECT_DOUBLE_EQ(engineSettings.staticRebuildDirtyRatio, 0.35);
    EXPECT_EQ(engineSettings.staticRebuildBudgetUs, 250u);
    EXPECT_EQ(engineSettings.streamingCpuBudgetUs, 2000u);
    EXPECT_EQ(engineSettings.streamingGpuUploadBudgetBytes, 4u * 1024u * 1024u);
    EXPECT_EQ(engineSettings.streamingIoBudgetBytes, 8u * 1024u * 1024u);
    EXPECT_EQ(engineSettings.streamingCpuMemoryBudgetBytes, 256u * 1024u * 1024u);
    EXPECT_EQ(engineSettings.streamingGpuMemoryBudgetBytes, 512u * 1024u * 1024u);
    EXPECT_EQ(engineSettings.maxOcclusionHistoryAge, 5u);

    settings.maxVisibilityJobs = -1;
    settings.parallelVisibilityPrimitiveThreshold = -2;
    settings.parallelVisibilityPrimitivesPerTask = -3;
    settings.staticRebuildBudgetUs = -4;
    settings.streamingCpuBudgetUs = -5;
    settings.streamingGpuUploadBudgetBytes = -6;
    settings.streamingIoBudgetBytes = -7;
    settings.streamingCpuMemoryBudgetBytes = -8;
    settings.streamingGpuMemoryBudgetBytes = -9;
    settings.maxOcclusionHistoryAge = -10;

    const auto clampedSettings = EditorSettings::BuildLargeSceneSettings();
    EXPECT_EQ(clampedSettings.maxVisibilityJobs, 0u);
    EXPECT_EQ(clampedSettings.parallelVisibilityPrimitiveThreshold, 0u);
    EXPECT_EQ(clampedSettings.parallelVisibilityPrimitivesPerTask, 0u);
    EXPECT_EQ(clampedSettings.staticRebuildBudgetUs, 0u);
    EXPECT_EQ(clampedSettings.streamingCpuBudgetUs, 0u);
    EXPECT_EQ(clampedSettings.streamingGpuUploadBudgetBytes, 0u);
    EXPECT_EQ(clampedSettings.streamingIoBudgetBytes, 0u);
    EXPECT_EQ(clampedSettings.streamingCpuMemoryBudgetBytes, 0u);
    EXPECT_EQ(clampedSettings.streamingGpuMemoryBudgetBytes, 0u);
    EXPECT_EQ(clampedSettings.maxOcclusionHistoryAge, 0u);

    settings = {};
}

TEST(EditorSettingsRegistryTests, OrdersSettingsByCategoryDisplayNameAndId)
{
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Settings::EditorSettingsRegistry registry;

    EXPECT_TRUE(registry.Register(MakeSetting("z", "Zeta", "Editor/Z")));
    EXPECT_TRUE(registry.Register(MakeSetting("a", "Alpha", "Editor/A")));
    EXPECT_TRUE(registry.Register(MakeSetting("b", "Beta", "Editor/A")));

    const auto& objects = registry.GetObjects();
    ASSERT_EQ(objects.size(), 3u);
    EXPECT_EQ(objects[0].id, "a");
    EXPECT_EQ(objects[1].id, "b");
    EXPECT_EQ(objects[2].id, "z");
}

TEST(EditorSettingsRegistryTests, SearchMatchesCategoryNameAndReflectedPropertyLabels)
{
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Settings::EditorSettingsRegistry registry;

    EXPECT_TRUE(registry.Register(MakeSetting("scene", "Scene Tools", "Editor/Scene View")));
    EXPECT_TRUE(registry.Register({
        "debug",
        "Debug Draw",
        "Editor/Debugging",
        NLS::Editor::Settings::EditorSettingPersistenceScope::User,
        [] { return NLS::meta::Variant(NLS::Editor::Settings::EditorSettings::GetDebugDrawSettingsObject(), NLS::meta::variant_policy::NoCopy {}); },
        NLS_TYPEOF(NLS::Editor::Settings::EditorDebugDrawSettingsObject)
    }));

    auto categoryMatches = registry.Search("scene view");
    EXPECT_TRUE(std::any_of(
        categoryMatches.begin(),
        categoryMatches.end(),
        [](const auto* p_object)
        {
            return p_object != nullptr && p_object->id == "scene";
        }));

    auto propertyMatches = registry.Search("Light Billboard");
    ASSERT_EQ(propertyMatches.size(), 1u);
    EXPECT_EQ(propertyMatches[0]->id, "debug");

    EXPECT_TRUE(registry.Register({
        "editor.rendering",
        "Rendering",
        "Editor/Rendering",
        NLS::Editor::Settings::EditorSettingPersistenceScope::User,
        [] { return NLS::meta::Variant(NLS::Editor::Settings::EditorSettings::GetRenderingSettingsObject(), NLS::meta::variant_policy::NoCopy {}); },
        NLS_TYPEOF(NLS::Editor::Settings::EditorRenderingSettingsObject)
    }));
    auto lightGridMatches = registry.Search("LightGrid");
    ASSERT_EQ(lightGridMatches.size(), 1u);
    EXPECT_EQ(lightGridMatches[0]->id, "editor.rendering");

    EXPECT_TRUE(registry.Search("does-not-exist").empty());
}

TEST(EditorSettingsRegistryTests, LoadingLegacyDebugDrawSettingsKeepsSceneIconsVisible)
{
    using namespace NLS::Editor::Settings;

    auto& debugSettings = EditorSettings::GetDebugDrawSettingsObject();
    debugSettings.debugDrawEnabled = true;
    debugSettings.debugDrawCamera = true;
    debugSettings.debugDrawLighting = true;

    EditorSettingsRegistry registry;
    EditorSettings::RegisterSettingObjects(registry);

    const auto path = std::filesystem::temp_directory_path() /
        ("nullus_editor_settings_" + NLS::Guid::New().ToString() + ".json");
    {
        std::ofstream file(path, std::ios::trunc);
        file <<
            R"({"version":1,"objects":{"editor.debug-draw":{"fields":{"debugDrawEnabled":true,"debugDrawCamera":false,"debugDrawLighting":false}}}})";
    }

    ASSERT_TRUE(EditorSettingsPersistence::Load(path, registry));
    EXPECT_TRUE(debugSettings.debugDrawCamera);
    EXPECT_TRUE(debugSettings.debugDrawLighting);

    std::filesystem::remove(path);
}

TEST(EditorSettingsRegistryTests, LoadingCurrentDebugDrawSettingsPreservesUserSceneIconToggles)
{
    using namespace NLS::Editor::Settings;

    auto& debugSettings = EditorSettings::GetDebugDrawSettingsObject();
    debugSettings.debugDrawEnabled = true;
    debugSettings.debugDrawCamera = true;
    debugSettings.debugDrawLighting = true;

    EditorSettingsRegistry registry;
    EditorSettings::RegisterSettingObjects(registry);

    const auto path = std::filesystem::temp_directory_path() /
        ("nullus_editor_settings_" + NLS::Guid::New().ToString() + ".json");
    {
        std::ofstream file(path, std::ios::trunc);
        file <<
            R"({"version":2,"objects":{"editor.debug-draw":{"fields":{"debugDrawEnabled":true,"debugDrawCamera":false,"debugDrawLighting":false}}}})";
    }

    ASSERT_TRUE(EditorSettingsPersistence::Load(path, registry));
    EXPECT_FALSE(debugSettings.debugDrawCamera);
    EXPECT_FALSE(debugSettings.debugDrawLighting);

    std::filesystem::remove(path);
}

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

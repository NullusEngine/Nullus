#include <gtest/gtest.h>

#include <Reflection/ReflectionDatabase.h>

#include "GameObject.h"
#include "Settings/TagLayerSettings.h"

TEST(TagLayerSettingsTests, ProvidesUnityStyleDefaultTagsAndLayers)
{
    const auto& tags = NLS::Editor::Settings::TagLayerSettings::GetTags();
    const auto& layers = NLS::Editor::Settings::TagLayerSettings::GetLayers();

    ASSERT_FALSE(tags.empty());
    ASSERT_EQ(layers.size(), NLS::Editor::Settings::TagLayerSettings::LayerCount);
    EXPECT_EQ(tags.front(), "Untagged");
    EXPECT_EQ(layers[0], "Default");
    EXPECT_EQ(layers[2], "Ignore Raycast");
    EXPECT_EQ(layers[5], "UI");

    const auto layerChoices = NLS::Editor::Settings::TagLayerSettings::BuildLayerChoices();
    ASSERT_EQ(layerChoices.size(), NLS::Editor::Settings::TagLayerSettings::LayerCount);
    EXPECT_EQ(layerChoices.at(3), "3: Layer 3");
    EXPECT_EQ(layerChoices.at(5), "5: UI");
}

TEST(TagLayerSettingsTests, GameObjectDefaultsEmptyTagsToUntagged)
{
    NLS::Engine::GameObject object("Default Tag");
    EXPECT_EQ(object.GetTag(), "Untagged");

    object.SetTag("");
    EXPECT_EQ(object.GetTag(), "Untagged");
}

TEST(TagLayerSettingsTests, GameObjectStoresLayerAndReflectsIt)
{
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Engine::GameObject object("Layered");
    object.SetLayer(5);

    EXPECT_EQ(object.GetLayer(), 5);

    const auto type = NLS_TYPEOF(NLS::Engine::GameObject);
    const auto& layer = type.GetField("layer");
    ASSERT_TRUE(layer.IsValid());
    EXPECT_EQ(layer.GetType(), NLS_TYPEOF(int));

    auto instance = NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {});
    EXPECT_EQ(layer.GetValue(instance).ToInt(), 5);

    auto invalidValue = NLS::meta::Variant(99);
    ASSERT_TRUE(layer.SetValue(instance, invalidValue));
    EXPECT_EQ(object.GetLayer(), 31);
}

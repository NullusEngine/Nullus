#include "ReflectionRuntimeTestFixture.h"

#include "Panels/ComponentSearchPanel.h"

#include "Reflection/RuntimeMetaProperties.h"
#include "Components/CameraComponent.h"
#include "Components/Component.h"
#include "Components/LightComponent.h"
#include "Components/MaterialRenderer.h"
#include "Components/MeshRenderer.h"
#include "Components/SkyBoxComponent.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"

#include <algorithm>
#include <string>
#include <vector>

namespace
{
using NLS::Editor::Panels::ComponentSearchEntry;
using NLS::Editor::Panels::ComponentSearchPanel;
using NLS::Editor::Panels::ComponentPickerViewMode;
using NLS::Engine::Components::CameraComponent;
using NLS::Engine::Components::Component;
using NLS::Engine::Components::LightComponent;
using NLS::Engine::Components::MaterialRenderer;
using NLS::Engine::Components::MeshRenderer;
using NLS::Engine::Components::SkyBoxComponent;
using NLS::Engine::Components::TransformComponent;
using NLS::Engine::GameObject;
using NLS::meta::Type;

GameObject MakeActor()
{
    return GameObject("Actor", "Untagged");
}

bool ContainsDisplayName(const std::vector<ComponentSearchEntry>& p_entries, const std::string& p_displayName)
{
    return std::any_of(
        p_entries.begin(),
        p_entries.end(),
        [&p_displayName](const ComponentSearchEntry& p_entry)
        {
            return p_entry.displayName == p_displayName;
        });
}

const ComponentSearchEntry* FindEntry(const std::vector<ComponentSearchEntry>& p_entries, const std::string& p_displayName)
{
    const auto it = std::find_if(
        p_entries.begin(),
        p_entries.end(),
        [&p_displayName](const ComponentSearchEntry& p_entry)
        {
            return p_entry.displayName == p_displayName;
        });

    return it != p_entries.end() ? &(*it) : nullptr;
}
} // namespace

TEST_F(ReflectionRuntimeTestFixture, NormalizeSearchTextRemovesWhitespaceAndIgnoresCase)
{
    EXPECT_EQ(ComponentSearchPanel::NormalizeSearchText(" Mesh Renderer "), "meshrenderer");
    EXPECT_EQ(ComponentSearchPanel::NormalizeSearchText("Sky Box"), "skybox");
}

TEST_F(ReflectionRuntimeTestFixture, MakeDisplayNameRemovesNamespacesAndSplitsCamelCase)
{
    EXPECT_EQ(ComponentSearchPanel::MakeDisplayName(Type::GetFromName("NLS::Engine::Components::MeshRenderer")), "Mesh Renderer");
    EXPECT_EQ(ComponentSearchPanel::MakeDisplayName(Type::GetFromName("NLS::Engine::Components::SkyBoxComponent")), "Sky Box");
    EXPECT_EQ(ComponentSearchPanel::MakeDisplayName(Type::GetFromName("NLS::Engine::Components::Component")), "Component");
}

TEST_F(ReflectionRuntimeTestFixture, ComponentEntriesReadComponentMenuMetadataAndUseRootFallback)
{
    const auto meshRendererType = Type::GetFromName("NLS::Engine::Components::MeshRenderer");
    const auto materialRendererType = Type::GetFromName("NLS::Engine::Components::MaterialRenderer");

    ASSERT_TRUE(meshRendererType.IsValid());
    ASSERT_TRUE(materialRendererType.IsValid());

    EXPECT_EQ(ComponentSearchPanel::GetComponentMenuPath(meshRendererType), "Rendering/Mesh Renderer");
    EXPECT_EQ(ComponentSearchPanel::GetComponentMenuPath(materialRendererType), "Rendering/Material Renderer");
    EXPECT_EQ(
        ComponentSearchPanel::GetComponentMenuPath(Type::GetFromName("NLS::Engine::Components::SkyBoxComponent")),
        "Sky Box");
}

TEST_F(ReflectionRuntimeTestFixture, BuildComponentEntriesUsesReflectionAndSortsReadableNames)
{
    auto actor = MakeActor();

    const std::vector<ComponentSearchEntry> entries = ComponentSearchPanel::BuildComponentEntries(&actor);

    ASSERT_FALSE(entries.empty());
    EXPECT_TRUE(ContainsDisplayName(entries, "Camera"));
    EXPECT_TRUE(ContainsDisplayName(entries, "Light"));
    EXPECT_TRUE(ContainsDisplayName(entries, "Mesh Renderer"));

    ASSERT_GE(entries.size(), 2u);
    for (size_t index = 1; index < entries.size(); ++index)
        EXPECT_LE(entries[index - 1].displayName, entries[index].displayName);
}

TEST_F(ReflectionRuntimeTestFixture, BuildComponentEntriesFiltersBaseAndTransformTypes)
{
    auto actor = MakeActor();

    const std::vector<ComponentSearchEntry> entries = ComponentSearchPanel::BuildComponentEntries(&actor);

    EXPECT_FALSE(ContainsDisplayName(entries, "Component"));
    EXPECT_FALSE(ContainsDisplayName(entries, "Transform"));
}

TEST_F(ReflectionRuntimeTestFixture, SearchFilteringIsCaseInsensitiveAndSupportsCollapsedSpacing)
{
    auto actor = MakeActor();

    const std::vector<ComponentSearchEntry> entries = ComponentSearchPanel::BuildComponentEntries(&actor, "mesh renderer");

    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries.front().displayName, "Mesh Renderer");
}

TEST_F(ReflectionRuntimeTestFixture, PickerViewModeUsesCategoriesForEmptyQueryAndResultsForSearch)
{
    EXPECT_EQ(ComponentSearchPanel::GetViewModeForQuery(""), ComponentPickerViewMode::Categories);
    EXPECT_EQ(ComponentSearchPanel::GetViewModeForQuery("   "), ComponentPickerViewMode::Categories);
    EXPECT_EQ(ComponentSearchPanel::GetViewModeForQuery("mesh"), ComponentPickerViewMode::SearchResults);
}

TEST_F(ReflectionRuntimeTestFixture, BuildComponentMenuTreeUsesMetadataCategories)
{
    auto actor = MakeActor();

    const auto entries = ComponentSearchPanel::BuildComponentEntries(&actor);
    const auto roots = ComponentSearchPanel::BuildCategoryTree(entries);

    const auto renderingIt = std::find_if(
        roots.begin(),
        roots.end(),
        [](const auto& node)
        {
            return node.label == "Rendering";
        });

    ASSERT_NE(renderingIt, roots.end());

    const auto meshIt = std::find_if(
        renderingIt->entries.begin(),
        renderingIt->entries.end(),
        [](const ComponentSearchEntry& entry)
        {
            return entry.displayName == "Mesh Renderer";
        });

    EXPECT_NE(meshIt, renderingIt->entries.end());
}

TEST_F(ReflectionRuntimeTestFixture, BuildComponentMenuTreePlacesComponentsWithoutMenuMetadataAtRoot)
{
    auto actor = MakeActor();

    const auto entries = ComponentSearchPanel::BuildComponentEntries(&actor);
    const auto roots = ComponentSearchPanel::BuildCategoryTree(entries);

    const auto skyBoxIt = std::find_if(
        roots.begin(),
        roots.end(),
        [](const auto& node)
        {
            return node.label == "Sky Box";
        });

    ASSERT_NE(skyBoxIt, roots.end());
    ASSERT_EQ(skyBoxIt->entries.size(), 1u);
    EXPECT_EQ(skyBoxIt->entries.front().displayName, "Sky Box");
}

TEST_F(ReflectionRuntimeTestFixture, AddabilityBlocksDuplicateSingleInstanceComponents)
{
    auto actor = MakeActor();
    actor.AddComponent<CameraComponent>();

    const std::vector<ComponentSearchEntry> entries = ComponentSearchPanel::BuildComponentEntries(&actor);
    const ComponentSearchEntry* cameraEntry = FindEntry(entries, "Camera");

    ASSERT_NE(cameraEntry, nullptr);
    EXPECT_FALSE(cameraEntry->isAddable);
}

TEST_F(ReflectionRuntimeTestFixture, AddabilityAllowsComponentsNotAlreadyPresent)
{
    auto actor = MakeActor();

    const std::vector<ComponentSearchEntry> entries = ComponentSearchPanel::BuildComponentEntries(&actor);
    const ComponentSearchEntry* lightEntry = FindEntry(entries, "Light");

    ASSERT_NE(lightEntry, nullptr);
    EXPECT_TRUE(lightEntry->isAddable);
}

TEST_F(ReflectionRuntimeTestFixture, TryAddComponentFromEntryUsesDynamicAddPath)
{
    auto actor = MakeActor();
    const auto type = Type::GetFromName("NLS::Engine::Components::LightComponent");

    ComponentSearchEntry entry;
    entry.componentType = type;
    entry.displayName = "Light";
    entry.searchKey = ComponentSearchPanel::NormalizeSearchText(entry.displayName);
    entry.isAddable = true;

    ASSERT_TRUE(ComponentSearchPanel::TryAddComponentFromEntry(&actor, entry));
    EXPECT_NE(actor.GetComponent<LightComponent>(), nullptr);
}

TEST_F(ReflectionRuntimeTestFixture, TryAddComponentFromEntryRejectsInvalidOrBlockedEntries)
{
    auto actor = MakeActor();
    actor.AddComponent<CameraComponent>();

    const auto type = Type::GetFromName("NLS::Engine::Components::CameraComponent");

    ComponentSearchEntry blockedEntry;
    blockedEntry.componentType = type;
    blockedEntry.displayName = "Camera";
    blockedEntry.searchKey = ComponentSearchPanel::NormalizeSearchText(blockedEntry.displayName);
    blockedEntry.isAddable = false;

    EXPECT_FALSE(ComponentSearchPanel::TryAddComponentFromEntry(&actor, blockedEntry));
}

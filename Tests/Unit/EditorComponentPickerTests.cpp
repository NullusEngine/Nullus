#include "ReflectionRuntimeTestFixture.h"

#include "Panels/ComponentSearchPanel.h"

#include "Assets/BuiltInScriptRegistry.h"
#include "Reflection/RuntimeMetaProperties.h"
#include "Components/CameraComponent.h"
#include "Components/Component.h"
#include "Components/LightComponent.h"
#include "Components/MeshRenderer.h"
#include "Components/SkyBoxComponent.h"
#include "Components/TransformComponent.h"
#include "GameObject.h"
#include "Scripting/ScriptComponent.h"
#include "Assets/ScriptAssetUtility.h"

#include <algorithm>
#include <filesystem>
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
using NLS::Engine::Components::MeshRenderer;
using NLS::Engine::Components::SkyBoxComponent;
using NLS::Engine::Components::TransformComponent;
using NLS::Scripting::ScriptComponent;
using NLS::Engine::GameObject;
using NLS::meta::Type;

GameObject MakeGameObject()
{
    return GameObject("GameObject", "Untagged");
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

    ASSERT_TRUE(meshRendererType.IsValid());

    EXPECT_EQ(ComponentSearchPanel::GetComponentMenuPath(meshRendererType), "Rendering/Mesh Renderer");
    EXPECT_FALSE(Type::GetFromName("NLS::Engine::Components::MaterialRenderer").IsValid());
    EXPECT_EQ(
        ComponentSearchPanel::GetComponentMenuPath(Type::GetFromName("NLS::Engine::Components::SkyBoxComponent")),
        "Sky Box");
}

TEST_F(ReflectionRuntimeTestFixture, BuildComponentEntriesUsesReflectionAndSortsReadableNames)
{
    auto actor = MakeGameObject();

    const std::vector<ComponentSearchEntry> entries = ComponentSearchPanel::BuildComponentEntries(&actor);

    ASSERT_FALSE(entries.empty());
    EXPECT_TRUE(ContainsDisplayName(entries, "Camera"));
    EXPECT_TRUE(ContainsDisplayName(entries, "Light"));
    EXPECT_TRUE(ContainsDisplayName(entries, "Mesh Renderer"));

    ASSERT_GE(entries.size(), 2u);
    for (size_t index = 1; index < entries.size(); ++index)
        EXPECT_LE(entries[index - 1].displayName, entries[index].displayName);
}

TEST_F(ReflectionRuntimeTestFixture, BuildComponentEntriesHidesEmptyScriptComponent)
{
    auto actor = MakeGameObject();
    const auto scriptType = Type::GetFromName("NLS::Scripting::ScriptComponent");

    ASSERT_TRUE(scriptType.IsValid());
    ASSERT_TRUE(scriptType.DerivesFrom(NLS_TYPEOF(Component)));
    ASSERT_FALSE(scriptType.GetDynamicConstructors().empty());

    const auto entries = ComponentSearchPanel::BuildComponentEntries(&actor);
    EXPECT_FALSE(ContainsDisplayName(entries, "Script"));
    EXPECT_FALSE(ComponentSearchPanel::IsTypeAddableToGameObject(scriptType, &actor));
    EXPECT_EQ(actor.GetComponent<ScriptComponent>(), nullptr);
}

TEST_F(ReflectionRuntimeTestFixture, BuildComponentEntriesIncludesImportedCSharpAndLuaScripts)
{
    auto actor = MakeGameObject();
    const auto entries = ComponentSearchPanel::BuildComponentEntries(
        &actor,
        {},
        std::filesystem::path("TestProject/Assets"));

    const auto* csharp = FindEntry(entries, "NewScript (C#)");
    const auto* lua = FindEntry(entries, "NewScript (Lua)");
    ASSERT_NE(csharp, nullptr);
    ASSERT_NE(lua, nullptr);
    ASSERT_TRUE(csharp->scriptAsset.has_value());
    ASSERT_TRUE(lua->scriptAsset.has_value());
    EXPECT_EQ(csharp->scriptAsset->language, NLS::Scripting::ScriptLanguage::CSharp);
    EXPECT_EQ(lua->scriptAsset->language, NLS::Scripting::ScriptLanguage::Lua);
    EXPECT_EQ(csharp->scriptAsset->sourcePath, "Assets/NewScript.cs");
    EXPECT_EQ(lua->scriptAsset->sourcePath, "Assets/NewScript.lua");
    EXPECT_TRUE(csharp->scriptAsset->isComponent);
    EXPECT_TRUE(lua->scriptAsset->isComponent);
}

TEST_F(ReflectionRuntimeTestFixture, BuildComponentEntriesIncludesRegisteredEngineScripts)
{
    NLS::Editor::Assets::BuiltInScriptRegistry::Refresh(std::filesystem::current_path());
    auto actor = MakeGameObject();
    const auto entries = ComponentSearchPanel::BuildComponentEntries(&actor);
    const auto* transformMover = FindEntry(entries, "TransformMover (C#)");

    ASSERT_NE(transformMover, nullptr);
    ASSERT_TRUE(transformMover->scriptAsset.has_value());
    EXPECT_TRUE(transformMover->scriptAsset->assetId.IsValid());
    EXPECT_EQ(transformMover->scriptAsset->sourcePath, "TransformMover.cs");
    EXPECT_EQ(transformMover->menuPath, "Scripts/Engine/C#");
    EXPECT_TRUE(transformMover->scriptAsset->isComponent);
    EXPECT_TRUE(ComponentSearchPanel::TryAddComponentFromEntry(&actor, *transformMover));
    EXPECT_EQ(actor.GetComponents().size(), 2u);

    const auto* registered = NLS::Editor::Assets::BuiltInScriptRegistry::FindByAssetId(
        transformMover->scriptAsset->assetId);
    ASSERT_NE(registered, nullptr);
    EXPECT_EQ(registered->asset.sourcePath, "TransformMover.cs");
}

TEST_F(ReflectionRuntimeTestFixture, ScriptImporterDistinguishesComponentsFromUtilitySources)
{
    using NLS::Editor::Assets::IsScriptComponentSource;
    using NLS::Scripting::ScriptLanguage;

    EXPECT_TRUE(IsScriptComponentSource(
        ScriptLanguage::CSharp,
        "public sealed class PlayerBehaviour : Nullus.Managed.Behaviour {}"));
    EXPECT_FALSE(IsScriptComponentSource(
        ScriptLanguage::CSharp,
        "public static class MathHelpers { public static int Add(int a, int b) => a + b; }"));
    EXPECT_FALSE(IsScriptComponentSource(
        ScriptLanguage::CSharp,
        "public abstract class BaseBehaviour : Behaviour {}"));
    EXPECT_FALSE(IsScriptComponentSource(
        ScriptLanguage::CSharp,
        "public static class Text { const string Example = \"\"\" class Fake : Behaviour {} \"\"\"; }"));

    EXPECT_TRUE(IsScriptComponentSource(
        ScriptLanguage::Lua,
        "local Module = {}; function Module:Update(dt) end; return Module"));
    EXPECT_TRUE(IsScriptComponentSource(ScriptLanguage::Lua, "return {}"));
    EXPECT_FALSE(IsScriptComponentSource(
        ScriptLanguage::Lua,
        "local function helper() return {} end"));
    EXPECT_FALSE(IsScriptComponentSource(
        ScriptLanguage::Lua,
        "local example = [[return FakeModule]]"));
}

TEST_F(ReflectionRuntimeTestFixture, AddingConcreteScriptBindsAssetAndAllowsAnotherLanguage)
{
    auto actor = MakeGameObject();
    const auto entries = ComponentSearchPanel::BuildComponentEntries(
        &actor,
        {},
        std::filesystem::path("TestProject/Assets"));
    const auto* csharp = FindEntry(entries, "NewScript (C#)");
    const auto* lua = FindEntry(entries, "NewScript (Lua)");
    ASSERT_NE(csharp, nullptr);
    ASSERT_NE(lua, nullptr);

    ASSERT_TRUE(ComponentSearchPanel::TryAddComponentFromEntry(&actor, *csharp));
    ASSERT_TRUE(ComponentSearchPanel::TryAddComponentFromEntry(&actor, *lua));

    size_t scriptCount = 0u;
    for (const auto& component : actor.GetComponents())
    {
        const auto* script = component ? dynamic_cast<const ScriptComponent*>(component.get()) : nullptr;
        if (script == nullptr)
            continue;
        ++scriptCount;
        EXPECT_TRUE(script->GetScriptAsset().assetId.IsValid());
    }
    EXPECT_EQ(scriptCount, 2u);

    EXPECT_FALSE(ComponentSearchPanel::TryAddComponentFromEntry(&actor, *csharp));
}

TEST_F(ReflectionRuntimeTestFixture, BuildComponentEntriesFiltersBaseAndTransformTypes)
{
    auto actor = MakeGameObject();

    const std::vector<ComponentSearchEntry> entries = ComponentSearchPanel::BuildComponentEntries(&actor);

    EXPECT_FALSE(ContainsDisplayName(entries, "Component"));
    EXPECT_FALSE(ContainsDisplayName(entries, "Transform"));
}

TEST_F(ReflectionRuntimeTestFixture, SearchFilteringIsCaseInsensitiveAndSupportsCollapsedSpacing)
{
    auto actor = MakeGameObject();

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
    auto actor = MakeGameObject();

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
    auto actor = MakeGameObject();

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
    auto actor = MakeGameObject();
    actor.AddComponent<CameraComponent>();

    const std::vector<ComponentSearchEntry> entries = ComponentSearchPanel::BuildComponentEntries(&actor);
    const ComponentSearchEntry* cameraEntry = FindEntry(entries, "Camera");

    ASSERT_NE(cameraEntry, nullptr);
    EXPECT_FALSE(cameraEntry->isAddable);
}

TEST_F(ReflectionRuntimeTestFixture, AddabilityAllowsComponentsNotAlreadyPresent)
{
    auto actor = MakeGameObject();

    const std::vector<ComponentSearchEntry> entries = ComponentSearchPanel::BuildComponentEntries(&actor);
    const ComponentSearchEntry* lightEntry = FindEntry(entries, "Light");

    ASSERT_NE(lightEntry, nullptr);
    EXPECT_TRUE(lightEntry->isAddable);
}

TEST_F(ReflectionRuntimeTestFixture, TryAddComponentFromEntryUsesDynamicAddPath)
{
    auto actor = MakeGameObject();
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
    auto actor = MakeGameObject();
    actor.AddComponent<CameraComponent>();

    const auto type = Type::GetFromName("NLS::Engine::Components::CameraComponent");

    ComponentSearchEntry blockedEntry;
    blockedEntry.componentType = type;
    blockedEntry.displayName = "Camera";
    blockedEntry.searchKey = ComponentSearchPanel::NormalizeSearchText(blockedEntry.displayName);
    blockedEntry.isAddable = false;

    EXPECT_FALSE(ComponentSearchPanel::TryAddComponentFromEntry(&actor, blockedEntry));
}

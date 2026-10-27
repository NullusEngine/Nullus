#include <gtest/gtest.h>

#include "Assets/AssetIconResolver.h"
#include "Icons/EditorIconCatalog.h"

#include <string>
#include <string_view>
#include <vector>

namespace
{
using namespace NLS::Editor::Assets;
using NLS::Core::Assets::AssetType;
using NLS::Editor::Icons::IconId;
using NLS::Editor::Icons::IconRegistry;

bool IsRegistered(const IconId p_icon)
{
    static const IconRegistry registry = IconRegistry::CreateDefault();
    return registry.Find(p_icon) != nullptr;
}
} // namespace

TEST(AssetIconResolverTests, EveryAssetTypeResolvesToARegisteredIcon)
{
    const auto& resolver = AssetIconResolver::Default();
    for (int value = 0; value <= static_cast<int>(AssetType::Count); ++value)
    {
        const auto icon = resolver.ResolveAssetType(static_cast<AssetType>(value));
        EXPECT_TRUE(IsRegistered(icon)) << "AssetType " << value << " -> " << icon.value;
    }
}

TEST(AssetIconResolverTests, AssetTypeMappingMatchesEditorSemantics)
{
    const auto& resolver = AssetIconResolver::Default();
    EXPECT_EQ(resolver.ResolveAssetType(AssetType::ModelScene).value, "editor.icon.asset.mesh");
    EXPECT_EQ(resolver.ResolveAssetType(AssetType::Texture).value, "editor.icon.asset.texture");
    EXPECT_EQ(resolver.ResolveAssetType(AssetType::Shader).value, "editor.icon.asset.shader");
    EXPECT_EQ(resolver.ResolveAssetType(AssetType::ShaderGraph).value, "editor.icon.asset.shader-graph");
    EXPECT_EQ(resolver.ResolveAssetType(AssetType::ShaderFunction).value, "editor.icon.asset.shader-function");
    EXPECT_EQ(resolver.ResolveAssetType(AssetType::Material).value, "editor.icon.asset.material");
    EXPECT_EQ(resolver.ResolveAssetType(AssetType::Audio).value, "editor.icon.asset.audio");
    EXPECT_EQ(resolver.ResolveAssetType(AssetType::Scene).value, "editor.icon.asset.scene");
    EXPECT_EQ(resolver.ResolveAssetType(AssetType::Script).value, "editor.icon.asset.script");
    EXPECT_EQ(resolver.ResolveAssetType(AssetType::Prefab).value, "editor.icon.asset.prefab");
    EXPECT_EQ(resolver.ResolveAssetType(AssetType::Font).value, "editor.icon.asset.font");
    EXPECT_EQ(resolver.ResolveAssetType(AssetType::UIDocument).value, "editor.icon.asset.ui-document");
    EXPECT_EQ(resolver.ResolveAssetType(AssetType::UIStyleSheet).value, "editor.icon.asset.ui-style");
    EXPECT_EQ(resolver.ResolveAssetType(AssetType::LocalizationTable).value, "editor.icon.asset.localization");
    EXPECT_EQ(resolver.ResolveAssetType(AssetType::VfxSystem).value, "editor.icon.asset.vfx-system");
    EXPECT_EQ(resolver.ResolveAssetType(AssetType::VfxScript).value, "editor.icon.asset.vfx-script");
    EXPECT_EQ(resolver.ResolveAssetType(AssetType::VfxEmitter).value, "editor.icon.asset.vfx-emitter");
    EXPECT_EQ(resolver.ResolveAssetType(AssetType::Unknown).value, "editor.icon.asset.default");
}

TEST(AssetIconResolverTests, PathsResolveExtensionsAndLanguageOverrides)
{
    const auto& resolver = AssetIconResolver::Default();
    EXPECT_EQ(resolver.ResolvePath("Assets/Player.cs").value, "editor.icon.asset.script.csharp");
    EXPECT_EQ(resolver.ResolvePath("Assets/Player.CS").value, "editor.icon.asset.script.csharp");
    EXPECT_EQ(resolver.ResolvePath("Assets/Enemy.lua").value, "editor.icon.asset.script.lua");
    EXPECT_EQ(resolver.ResolvePath("Assets/Enemy.LUA").value, "editor.icon.asset.script.lua");
    EXPECT_EQ(resolver.ResolvePath("Assets/Hero.prefab").value, "editor.icon.asset.prefab");
    EXPECT_EQ(resolver.ResolvePath("Assets/Hero.scene").value, "editor.icon.asset.scene");
    EXPECT_EQ(resolver.ResolvePath("Assets/Level.objectgraph.json").value, "editor.icon.asset.scene");
    EXPECT_EQ(resolver.ResolvePath("Assets/Tex.png").value, "editor.icon.asset.texture");
    EXPECT_EQ(resolver.ResolvePath("Assets/Mesh.fbx").value, "editor.icon.asset.mesh");
    EXPECT_EQ(resolver.ResolvePath("Assets/Mat.mat").value, "editor.icon.asset.material");
    EXPECT_EQ(resolver.ResolvePath("Assets/S.shader").value, "editor.icon.asset.shader");
    EXPECT_EQ(resolver.ResolvePath("Assets/S.shadergraph").value, "editor.icon.asset.shader-graph");
    EXPECT_EQ(resolver.ResolvePath("Assets/S.shaderfunc").value, "editor.icon.asset.shader-function");
    EXPECT_EQ(resolver.ResolvePath("Assets/Sound.wav").value, "editor.icon.asset.audio");
    EXPECT_EQ(resolver.ResolvePath("Assets/Font.ttf").value, "editor.icon.asset.font");
    EXPECT_EQ(resolver.ResolvePath("Assets/Panel.ui").value, "editor.icon.asset.ui-document");
    EXPECT_EQ(resolver.ResolvePath("Assets/Theme.style").value, "editor.icon.asset.ui-style");
    EXPECT_EQ(resolver.ResolvePath("Assets/Strings.loc").value, "editor.icon.asset.localization");
    EXPECT_EQ(resolver.ResolvePath("Assets/Fire.vfx").value, "editor.icon.asset.vfx-system");
    EXPECT_EQ(resolver.ResolvePath("Assets/Fire.vfxscript").value, "editor.icon.asset.vfx-script");
    EXPECT_EQ(resolver.ResolvePath("Assets/Fire.vfxemitter").value, "editor.icon.asset.vfx-emitter");
    EXPECT_EQ(resolver.ResolvePath("Assets/Notes.txt").value, "editor.icon.asset.default");
    EXPECT_EQ(resolver.ResolvePath("Assets/NoExtension").value, "editor.icon.asset.default");
}

TEST(AssetIconResolverTests, BrowserTypesAndThumbnailKindsResolveToRegisteredIcons)
{
    const auto& resolver = AssetIconResolver::Default();

    EXPECT_EQ(resolver.ResolveBrowserType(AssetBrowserItemType::Folder).value, "editor.icon.asset.folder");
    EXPECT_EQ(resolver.ResolveBrowserType(AssetBrowserItemType::Model).value, "editor.icon.asset.mesh");
    EXPECT_EQ(resolver.ResolveBrowserType(AssetBrowserItemType::Mesh).value, "editor.icon.asset.mesh");
    EXPECT_EQ(resolver.ResolveBrowserType(AssetBrowserItemType::Prefab).value, "editor.icon.asset.prefab");
    EXPECT_EQ(resolver.ResolveBrowserType(AssetBrowserItemType::Material).value, "editor.icon.asset.material");
    EXPECT_EQ(resolver.ResolveBrowserType(AssetBrowserItemType::Texture).value, "editor.icon.asset.texture");
    EXPECT_EQ(resolver.ResolveBrowserType(AssetBrowserItemType::Shader).value, "editor.icon.asset.shader");
    EXPECT_EQ(resolver.ResolveBrowserType(AssetBrowserItemType::Scene).value, "editor.icon.asset.scene");
    EXPECT_EQ(resolver.ResolveBrowserType(AssetBrowserItemType::Script).value, "editor.icon.asset.script");
    EXPECT_EQ(resolver.ResolveBrowserType(AssetBrowserItemType::Other).value, "editor.icon.asset.default");

    EXPECT_EQ(resolver.ResolveThumbnailFallback(AssetThumbnailKind::Icon).value, "editor.icon.asset.default");
    EXPECT_EQ(resolver.ResolveThumbnailFallback(AssetThumbnailKind::Texture).value, "editor.icon.asset.texture");
    EXPECT_EQ(resolver.ResolveThumbnailFallback(AssetThumbnailKind::MaterialSphere).value, "editor.icon.asset.material");
    EXPECT_EQ(resolver.ResolveThumbnailFallback(AssetThumbnailKind::ModelPreview).value, "editor.icon.asset.mesh");
    EXPECT_EQ(resolver.ResolveThumbnailFallback(AssetThumbnailKind::PrefabPreview).value, "editor.icon.asset.prefab");
    EXPECT_EQ(resolver.ResolveThumbnailFallback(AssetThumbnailKind::GenericPreview).value, "editor.icon.asset.default");
}

TEST(AssetIconResolverTests, PluginExtensionMappingOverridesBuiltIns)
{
    auto& resolver = AssetIconResolver::Default();
    resolver.ClearPluginMappings();

    resolver.RegisterExtension(".cs", IconId{ "editor.icon.asset.material" });
    EXPECT_EQ(resolver.ResolvePath("Assets/Player.cs").value, "editor.icon.asset.material");

    resolver.RegisterExtension("custom", IconId{ "editor.icon.asset.font" });
    EXPECT_EQ(resolver.ResolvePath("Assets/Thing.CUSTOM").value, "editor.icon.asset.font");

    resolver.ClearPluginMappings();
    EXPECT_EQ(resolver.ResolvePath("Assets/Player.cs").value, "editor.icon.asset.script.csharp");
}

TEST(AssetIconResolverTests, ImporterMappingIsCaseInsensitive)
{
    auto& resolver = AssetIconResolver::Default();
    resolver.ClearPluginMappings();

    resolver.RegisterImporter("VfxImporter", IconId{ "editor.icon.asset.material" });
    EXPECT_EQ(resolver.ResolveImporter("otherimporter").value, "editor.icon.asset.default");
    EXPECT_EQ(resolver.ResolveImporter("vfximporter").value, "editor.icon.asset.material");

    resolver.ClearPluginMappings();
    EXPECT_EQ(resolver.ResolveImporter("vfximporter").value, "editor.icon.asset.default");
}

TEST(AssetIconResolverTests, ResolvedIdsAreAlwaysRegisteredForEveryBrowserType)
{
    const auto& resolver = AssetIconResolver::Default();
    for (int value = 0; value <= static_cast<int>(AssetBrowserItemType::Count); ++value)
    {
        const auto icon = resolver.ResolveBrowserType(static_cast<AssetBrowserItemType>(value));
        EXPECT_TRUE(IsRegistered(icon)) << "browser type " << value << " -> " << icon.value;
    }
    for (int value = 0; value <= static_cast<int>(AssetThumbnailKind::Count); ++value)
    {
        const auto icon = resolver.ResolveThumbnailFallback(static_cast<AssetThumbnailKind>(value));
        EXPECT_TRUE(IsRegistered(icon)) << "thumbnail kind " << value << " -> " << icon.value;
    }
}

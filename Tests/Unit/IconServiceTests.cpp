#include <gtest/gtest.h>

#include "Icons/EditorIconCatalog.h"
#include "Icons/IconService.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <Rendering/RHI/Core/RHIResource.h>

namespace
{
using namespace NLS::Editor::Icons;

NLS::Render::Resources::Texture2D* FakeTexture(const std::uintptr_t p_id)
{
    return reinterpret_cast<NLS::Render::Resources::Texture2D*>(p_id);
}

std::shared_ptr<NLS::Render::RHI::RHITextureView> FakeView(const std::uintptr_t p_id)
{
    return std::shared_ptr<NLS::Render::RHI::RHITextureView>(
        reinterpret_cast<NLS::Render::RHI::RHITextureView*>(p_id),
        [](NLS::Render::RHI::RHITextureView*) {});
}

struct FakeBackend
{
    std::map<std::string, bool> existing;
    std::vector<std::string> loadedPaths;
    std::vector<NLS::Render::Resources::Texture2D*> destroyed;
    std::vector<void*> releasedViews;
    std::uintptr_t nextId = 0x1000;

    std::optional<IconTexture> Load(const std::string& p_path)
    {
        loadedPaths.push_back(p_path);
        const std::uintptr_t id = nextId;
        nextId += 0x10;
        return IconTexture{ FakeTexture(id), FakeView(id + 0x8) };
    }

    void Destroy(NLS::Render::Resources::Texture2D* p_texture)
    {
        destroyed.push_back(p_texture);
    }

    NLS::Render::UI::UiTextureId Register(const std::shared_ptr<NLS::Render::RHI::RHITextureView>& p_view)
    {
        if (!p_view)
            return {};
        const auto id = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(p_view.get()) >> 4u);
        return NLS::Render::UI::UiTextureId{ id != 0 ? id : 1u, 1u };
    }

    void Release(const std::shared_ptr<NLS::Render::RHI::RHITextureView>& p_view)
    {
        releasedViews.push_back(p_view.get());
    }
};

struct Fixture
{
    IconRegistry registry = IconRegistry::CreateDefault();
    FakeBackend backend;
    std::map<std::string, std::string> paths;
    std::vector<std::string> diagnostics;

    Fixture()
    {
        // Register one synthetic plugin icon so tests do not depend on real files.
        IconDefinition plugin;
        plugin.id = "editor.icon.test.plugin";
        plugin.kind = IconKind::Color;
        plugin.module = "plugin.test";
        plugin.tags = { "test" };
        registry.Register(plugin);
        paths["editor.icon.test.plugin"] = "plugin/test.png";
        backend.existing["plugin/test.png"] = true;

        paths["editor.icon.asset.default"] = "icons/asset-default.png";
        backend.existing["icons/asset-default.png"] = true;
    }

    IconServiceBindings MakeBindings()
    {
        IconServiceBindings bindings;
        bindings.resolvePath = [this](const std::string_view p_id) -> std::optional<std::filesystem::path>
        {
            const auto found = paths.find(std::string(p_id));
            if (found == paths.end())
                return std::nullopt;
            return std::filesystem::path(found->second);
        };
        bindings.fileExists = [this](const std::string& p_path)
        {
            const auto found = backend.existing.find(p_path);
            return found != backend.existing.end() && found->second;
        };
        bindings.loadTexture = [this](const std::string& p_path) { return backend.Load(p_path); };
        bindings.destroyTexture = [this](auto* p_texture) { backend.Destroy(p_texture); };
        bindings.registerTextureView = [this](const auto& p_view) { return backend.Register(p_view); };
        bindings.releaseTextureView = [this](const auto& p_view) { backend.Release(p_view); };
        bindings.logWarning = [this](const std::string_view p_message)
        {
            diagnostics.emplace_back(p_message);
        };
        return bindings;
    }

    std::unique_ptr<IconService> MakeService()
    {
        return std::make_unique<IconService>(registry, MakeBindings());
    }
};
} // namespace

TEST(IconRegistryTests, DefaultsCoverEveryRegisteredEditorIcon)
{
    const auto& registry = IconRegistry::CreateDefault();
    const std::vector<std::string_view> required {
        "editor.icon.asset.folder", "editor.icon.asset.texture", "editor.icon.asset.material",
        "editor.icon.asset.mesh", "editor.icon.asset.prefab", "editor.icon.asset.scene",
        "editor.icon.asset.shader", "editor.icon.asset.script", "editor.icon.asset.script.csharp",
        "editor.icon.asset.script.lua", "editor.icon.asset.font", "editor.icon.asset.audio",
        "editor.icon.asset.default",
        "editor.icon.toolbar.move", "editor.icon.toolbar.rotate", "editor.icon.toolbar.scale",
        "editor.icon.toolbar.pivot", "editor.icon.toolbar.center", "editor.icon.toolbar.global",
        "editor.icon.toolbar.local",
    };

    for (const auto id : required)
        EXPECT_NE(registry.Find(id), nullptr) << id;
}

TEST(IconRegistryTests, ClassifiesAssetToolbarAndConvertedGlyphIcons)
{
    const auto registry = IconRegistry::CreateDefault();
    ASSERT_NE(registry.Find("editor.icon.asset.prefab"), nullptr);
    EXPECT_EQ(registry.Find("editor.icon.asset.prefab")->kind, IconKind::Color);
    EXPECT_FALSE(registry.Find("editor.icon.asset.prefab")->themeable);

    ASSERT_NE(registry.Find("editor.icon.toolbar.move"), nullptr);
    EXPECT_EQ(registry.Find("editor.icon.toolbar.move")->kind, IconKind::Monochrome);
    EXPECT_TRUE(registry.Find("editor.icon.toolbar.move")->themeable);

    ASSERT_NE(registry.Find("editor.icon.glyph.caret-down"), nullptr);
    EXPECT_EQ(registry.Find("editor.icon.glyph.caret-down")->kind, IconKind::Monochrome);
    EXPECT_TRUE(registry.Find("editor.icon.glyph.caret-down")->themeable);
    EXPECT_EQ(registry.Find("editor.icon.glyph.caret-down")->glyph, 0u);
}

TEST(IconRegistryTests, BuiltInGlyphCodepointsAreEmptyAfterSvgMigration)
{
    const auto registry = IconRegistry::CreateDefault();
    EXPECT_TRUE(registry.UsedGlyphCodepoints().empty());
    EXPECT_TRUE(DefaultGlyphCodepoints().empty());
}

TEST(IconRegistryTests, RejectsInvalidDefinitions)
{
    IconRegistry registry;

    IconDefinition wrongPrefix;
    wrongPrefix.id = "not.an.icon";
    EXPECT_FALSE(registry.Register(wrongPrefix));

    IconDefinition glyphWithoutCodepoint;
    glyphWithoutCodepoint.id = "editor.icon.test.glyph";
    glyphWithoutCodepoint.kind = IconKind::Glyph;
    EXPECT_FALSE(registry.Register(glyphWithoutCodepoint));

    IconDefinition bitmapWithGlyph;
    bitmapWithGlyph.id = "editor.icon.test.bitmap";
    bitmapWithGlyph.kind = IconKind::Color;
    bitmapWithGlyph.glyph = 0xf001;
    EXPECT_FALSE(registry.Register(bitmapWithGlyph));

    IconDefinition valid;
    valid.id = "editor.icon.test.valid";
    EXPECT_TRUE(registry.Register(valid));
    EXPECT_FALSE(registry.Register(valid)) << "duplicate id";

    IconDefinition aliasClash;
    aliasClash.id = "editor.icon.test.other";
    aliasClash.aliases = { "editor.icon.test.valid" };
    EXPECT_FALSE(registry.Register(aliasClash));
}

TEST(IconRegistryTests, AliasesResolveToCanonicalDefinition)
{
    IconRegistry registry;
    IconDefinition definition;
    definition.id = "editor.icon.test.canonical";
    definition.aliases = { "editor.icon.test.legacy" };
    ASSERT_TRUE(registry.Register(definition));

    const auto* resolved = registry.Find("editor.icon.test.legacy");
    ASSERT_NE(resolved, nullptr);
    EXPECT_EQ(resolved->id, "editor.icon.test.canonical");
}

TEST(IconRegistryTests, UnregisterModuleRemovesItsIcons)
{
    IconRegistry registry;
    IconDefinition plugin;
    plugin.id = "editor.icon.plugin.one";
    plugin.module = "plugin.one";
    plugin.aliases = { "editor.icon.plugin.legacy" };
    ASSERT_TRUE(registry.Register(plugin));

    const auto generationBefore = registry.Generation();
    EXPECT_TRUE(registry.UnregisterModule("plugin.one"));
    EXPECT_EQ(registry.Find("editor.icon.plugin.one"), nullptr);
    EXPECT_EQ(registry.Find("editor.icon.plugin.legacy"), nullptr);
    EXPECT_GT(registry.Generation(), generationBefore);
    EXPECT_FALSE(registry.UnregisterModule("plugin.one")) << "nothing left to remove";
}

TEST(IconServiceTests, ResolvesRegisteredIconWithEngineTextureId)
{
    Fixture fixture;
    auto service = fixture.MakeService();

    const auto result = service->Resolve(IconId{ "editor.icon.test.plugin" }, 16);

    ASSERT_TRUE(result.icon.valid);
    EXPECT_EQ(result.status, IconResolveStatus::Resolved);
    EXPECT_TRUE(result.icon.texture.IsValid());
    EXPECT_FLOAT_EQ(result.icon.pixelSize, 16.0f);
    EXPECT_FALSE(result.icon.tintable);
    EXPECT_FLOAT_EQ(result.icon.uvMin.x, 0.0f);
    EXPECT_FLOAT_EQ(result.icon.uvMin.y, 1.0f);
    EXPECT_FLOAT_EQ(result.icon.uvMax.x, 1.0f);
    EXPECT_FLOAT_EQ(result.icon.uvMax.y, 0.0f);
    EXPECT_EQ(fixture.backend.loadedPaths.size(), 1u);
}

TEST(IconServiceTests, MonochromeIconsAreTintable)
{
    Fixture fixture;
    fixture.paths["editor.icon.toolbar.move"] = "icons/toolbar-move.png";
    fixture.backend.existing["icons/toolbar-move.png"] = true;
    auto service = fixture.MakeService();

    const auto result = service->Resolve(IconId{ "editor.icon.toolbar.move" }, 16);
    ASSERT_TRUE(result.icon.valid);
    EXPECT_TRUE(result.icon.tintable);
}

TEST(IconServiceTests, DeduplicatesTexturesByPath)
{
    Fixture fixture;
    fixture.paths["editor.icon.test.first"] = "icons/shared.png";
    fixture.paths["editor.icon.test.second"] = "icons/shared.png";
    fixture.backend.existing["icons/shared.png"] = true;
    IconRegistry registry = fixture.registry;
    for (const auto id : { "editor.icon.test.first", "editor.icon.test.second" })
    {
        IconDefinition definition;
        definition.id = id;
        ASSERT_TRUE(registry.Register(definition));
    }

    IconService service(registry, fixture.MakeBindings());
    ASSERT_TRUE(service.Resolve(IconId{ "editor.icon.test.first" }, 16).icon.valid);
    ASSERT_TRUE(service.Resolve(IconId{ "editor.icon.test.second" }, 16).icon.valid);
    EXPECT_EQ(fixture.backend.loadedPaths.size(), 1u);
    EXPECT_EQ(service.LoadedTextureCount(), 1u);
}

TEST(IconServiceTests, UnknownIdFallsBackToAssetDefault)
{
    Fixture fixture;
    auto service = fixture.MakeService();

    const auto result = service->Resolve(IconId{ "editor.icon.missing.icon" }, 16);
    EXPECT_EQ(result.status, IconResolveStatus::Fallback);
    EXPECT_EQ(result.resolvedId, "editor.icon.asset.default");
    EXPECT_TRUE(result.icon.valid);
    ASSERT_EQ(fixture.diagnostics.size(), 1u);

    service->Resolve(IconId{ "editor.icon.missing.icon" }, 16);
    EXPECT_EQ(fixture.diagnostics.size(), 1u) << "diagnostics stay once per id";
}

TEST(IconServiceTests, MissingFileFallsBackAndReportsPath)
{
    Fixture fixture;
    fixture.paths["editor.icon.test.plugin"] = "plugin/missing.png";
    fixture.backend.existing["plugin/missing.png"] = false;
    auto service = fixture.MakeService();

    const auto result = service->Resolve(IconId{ "editor.icon.test.plugin" }, 16);
    EXPECT_EQ(result.status, IconResolveStatus::Fallback);
    ASSERT_FALSE(fixture.diagnostics.empty());
    EXPECT_NE(fixture.diagnostics.front().find("plugin/missing.png"), std::string::npos);
}

TEST(IconServiceTests, LoadFailureFallsBackToDefault)
{
    Fixture fixture;
    auto bindings = fixture.MakeBindings();
    auto originalLoad = bindings.loadTexture;
    bindings.loadTexture = [&originalLoad](const std::string& p_path) -> std::optional<IconTexture>
    {
        if (p_path == "plugin/test.png")
            return std::nullopt;
        return originalLoad(p_path);
    };

    IconService service(fixture.registry, std::move(bindings));
    const auto result = service.Resolve(IconId{ "editor.icon.test.plugin" }, 16);
    EXPECT_EQ(result.status, IconResolveStatus::Fallback);
    EXPECT_TRUE(result.icon.valid);
}

TEST(IconServiceTests, MissingDefaultYieldsMissing)
{
    IconRegistry registry;
    IconDefinition plugin;
    plugin.id = "editor.icon.test.plugin";
    registry.Register(plugin);

    Fixture fixture;
    fixture.registry = registry;
    fixture.backend.existing["plugin/test.png"] = false;
    IconService service(registry, fixture.MakeBindings());
    const auto result = service.Resolve(IconId{ "editor.icon.test.plugin" }, 16);
    EXPECT_EQ(result.status, IconResolveStatus::Missing);
    EXPECT_FALSE(result.icon.valid);
}

TEST(IconServiceTests, TieredIconsSelectTheSmallestSufficientRaster)
{
    Fixture fixture;
    IconDefinition tiered;
    tiered.id = "editor.icon.test.tiered";
    tiered.kind = IconKind::Color;
    tiered.tiers = { 16, 32, 48 };
    ASSERT_TRUE(fixture.registry.Register(tiered));
    fixture.paths["editor.icon.test.tiered"] = "tiered/icon@48.png";
    for (const auto size : { 16, 32, 48 })
        fixture.backend.existing["tiered/icon@" + std::to_string(size) + ".png"] = true;

    auto service = fixture.MakeService();
    const auto result = service->Resolve(IconId{ "editor.icon.test.tiered" }, 20);

    ASSERT_TRUE(result.icon.valid);
    EXPECT_FLOAT_EQ(result.icon.pixelSize, 32.0f);
    ASSERT_EQ(fixture.backend.loadedPaths.size(), 1u);
    EXPECT_EQ(fixture.backend.loadedPaths[0], "tiered/icon@32.png");
}

TEST(IconServiceTests, UsageTelemetryIdlePreloadAndDiagnostics)
{
    Fixture fixture;
    auto service = fixture.MakeService();

    ASSERT_TRUE(service->Resolve(IconId{ "editor.icon.test.plugin" }, 16).icon.valid);
    EXPECT_EQ(service->GetUsageCount("editor.icon.test.plugin"), 1u);
    EXPECT_GT(service->GetDiagnostics().unusedDefinitionCount, 0u);
    EXPECT_GT(service->GetDiagnostics().estimatedTextureBytes, 0u);

    // Idle preload advances a bounded number of definitions per frame.
    bool more = service->PreloadIdleStep(2, 32);
    EXPECT_TRUE(more);
    for (int guard = 0; more && guard < 200; ++guard)
        more = service->PreloadIdleStep(2, 32);
    EXPECT_FALSE(more);
    EXPECT_GT(service->LoadedTextureCount(), 1u);

    service->ResetUsageCounts();
    EXPECT_EQ(service->GetUsageCount("editor.icon.test.plugin"), 0u);
}

TEST(IconServiceTests, AtlasFramesPreserveTopToBottomOrientationOnFlippedPage)
{
    Fixture fixture;
    IconDefinition atlasIcon;
    atlasIcon.id = "editor.icon.test.atlas";
    atlasIcon.kind = IconKind::Color;
    atlasIcon.tiers = { 16, 32, 64 };
    ASSERT_TRUE(fixture.registry.Register(atlasIcon));
    fixture.paths["editor.icon.test.atlas"] = "standalone/icon@64.png";
    fixture.backend.existing["standalone/icon@64.png"] = true;

    const NLS::Editor::Icons::IconAtlasFrame frame {
        "editor.icon.atlas.page0", 512, 0.125f, 0.25f, 0.375f, 0.5f
    };

    auto bindings = fixture.MakeBindings();
    bindings.findAtlasFrame = [&frame](const std::string_view id, const std::uint32_t pixels)
        -> const NLS::Editor::Icons::IconAtlasFrame*
    {
        return id == "editor.icon.test.atlas" && pixels == 32 ? &frame : nullptr;
    };
    bindings.resolveAtlasPagePath = [](const std::string_view pageId)
        -> std::optional<std::filesystem::path>
    {
        if (pageId == "editor.icon.atlas.page0")
            return std::filesystem::path("atlas/page0.png");
        return std::nullopt;
    };
    fixture.backend.existing["atlas/page0.png"] = true;

    IconService service(fixture.registry, std::move(bindings));
    const auto result = service.Resolve(IconId{ "editor.icon.test.atlas" }, 20);

    ASSERT_TRUE(result.icon.valid);
    EXPECT_FLOAT_EQ(result.icon.pixelSize, 32.0f);
    EXPECT_FLOAT_EQ(result.icon.uvMin.x, 0.125f);
    EXPECT_FLOAT_EQ(result.icon.uvMin.y, 0.75f);  // source top on the flipped page
    EXPECT_FLOAT_EQ(result.icon.uvMax.x, 0.375f);
    EXPECT_FLOAT_EQ(result.icon.uvMax.y, 0.5f);   // source bottom on the flipped page
    ASSERT_EQ(fixture.backend.loadedPaths.size(), 1u);
    EXPECT_EQ(fixture.backend.loadedPaths[0], "atlas/page0.png");
    EXPECT_EQ(service.GetDiagnostics().estimatedTextureBytes, 512u * 512u * 4u);
}

TEST(IconServiceTests, GlyphIconsResolveWithoutTextures)
{
    Fixture fixture;

    IconDefinition glyph;
    glyph.id = "editor.icon.test.glyph";
    glyph.kind = IconKind::Glyph;
    glyph.glyph = 0xf001;
    glyph.module = "test";
    ASSERT_TRUE(fixture.registry.Register(glyph));

    auto service = fixture.MakeService();

    const auto resolved = service->ResolveGlyph(IconId{ "editor.icon.test.glyph" }, 16.0f);
    ASSERT_TRUE(resolved.valid);
    EXPECT_STREQ(resolved.utf8, "\xef\x80\x81");
    EXPECT_FLOAT_EQ(resolved.pixelSize, 16.0f);
    EXPECT_TRUE(fixture.backend.loadedPaths.empty());
}

TEST(IconServiceTests, PreloadCountsOnlyDirectResolutions)
{
    Fixture fixture;
    auto service = fixture.MakeService();

    const auto loaded = service->Preload(
        {
            IconId{ "editor.icon.test.plugin" },
            IconId{ "editor.icon.missing.icon" },
        },
        16);
    EXPECT_EQ(loaded, 1u);
}

TEST(IconServiceTests, InvalidateModuleRetiresTexturesAfterTwoFrames)
{
    Fixture fixture;
    auto service = fixture.MakeService();

    ASSERT_TRUE(service->Resolve(IconId{ "editor.icon.test.plugin" }, 16).icon.valid);
    ASSERT_EQ(service->LoadedTextureCount(), 1u);

    service->SetFrameId(10);
    service->InvalidateModule("plugin.test");
    EXPECT_EQ(service->LoadedTextureCount(), 0u);
    EXPECT_TRUE(fixture.backend.destroyed.empty()) << "retirement is deferred";

    service->TickRetirement();
    EXPECT_TRUE(fixture.backend.destroyed.empty());

    service->SetFrameId(12);
    service->TickRetirement();
    EXPECT_EQ(fixture.backend.destroyed.size(), 1u);
    EXPECT_EQ(fixture.backend.releasedViews.size(), 1u);
}

TEST(IconServiceTests, DeviceRecreatedDropsCachesButKeepsIdsStable)
{
    Fixture fixture;
    auto service = fixture.MakeService();

    ASSERT_TRUE(service->Resolve(IconId{ "editor.icon.test.plugin" }, 16).icon.valid);
    service->OnDeviceRecreated();
    EXPECT_EQ(service->LoadedTextureCount(), 0u);

    const auto result = service->Resolve(IconId{ "editor.icon.test.plugin" }, 16);
    EXPECT_TRUE(result.icon.valid);
    EXPECT_EQ(fixture.backend.loadedPaths.size(), 2u);
}

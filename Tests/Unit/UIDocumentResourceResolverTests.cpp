#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include <fstream>
#include <iterator>
#include <memory>

#include <Guid.h>
#include <Assets/AssetId.h>

#include <Core/Assets/ContentSource.h>
#include <Core/Assets/NativeArtifactContainer.h>
#include <Core/ServiceLocator.h>
#include <Rendering/UI/UIFontAtlas.h>

#include "Engine/Assets/RuntimeAssetDatabase.h"
#include "Engine/UI/UIDocument.h"
#include "Engine/UI/UIDocumentResourceResolver.h"
#include "UIDocumentDesignerTestSupport.h"

using namespace NLS;
using namespace NLS::Engine::UI;
using namespace NLS::Engine::Assets;
using namespace NLS::Editor::UIToolsTest;

namespace
{
    constexpr const char* kTextureGuid = "0f3f794e-7578-4bcb-92cb-b3afb43ee861";
    constexpr const char* kFontGuid = "861b35f8-3170-4d0b-9fdb-111b8e089a3d";

    std::string ReferenceDocument()
    {
        return R"json({
            "version": 1,
            "documentId": "11111111-1111-4111-8111-111111111111",
            "root": {
                "nodeGuid": "22222222-2222-4222-8222-222222222222",
                "type": "Image",
                "name": "Root",
                "texture": {
                    "assetId": "0f3f794e-7578-4bcb-92cb-b3afb43ee861",
                    "subAssetKey": "texture:main"
                },
                "font": {
                    "assetId": "861b35f8-3170-4d0b-9fdb-111b8e089a3d",
                    "subAssetKey": "font:main"
                },
                "fontPixelSize": 22
            }
        })json";
    }

    std::string LegacyDocument()
    {
        return R"json({
            "version": 1,
            "documentId": "11111111-1111-4111-8111-111111111111",
            "root": {
                "nodeGuid": "22222222-2222-4222-8222-222222222222",
                "type": "Image",
                "name": "Root",
                "textureId": 77,
                "textureGeneration": 3,
                "fontAssetId": 5,
                "fontPixelSize": 18
            }
        })json";
    }

    std::string NineSliceDocument(
        const int width,
        const int height,
        const float inset)
    {
        const auto value = std::to_string(inset);
        return std::string(R"json({
            "version": 1,
            "documentId": "11111111-1111-4111-8111-111111111111",
            "root": {
                "nodeGuid": "22222222-2222-4222-8222-222222222222",
                "type": "Panel",
                "name": "Root",
                "children": [
                    {
                        "nodeGuid": "33333333-3333-4333-8333-333333333333",
                        "type": "NineSlice",
                        "name": "Frame",
                        "layout": { "size": [)json") +
            std::to_string(width) + ", " + std::to_string(height) + R"json(] },
                        "texture": {
                            "assetId": "0f3f794e-7578-4bcb-92cb-b3afb43ee861",
                            "subAssetKey": "texture:main"
                        },
                        "nineSlice": {
                            "left": )json" + value + R"json(,
                            "top": )json" + value + R"json(,
                            "right": )json" + value + R"json(,
                            "bottom": )json" + value + R"json(
                        }
                    }
                ]
            }
        })json";
    }

    class FakeDocumentResourceResolver final : public IUIDocumentResourceResolver
    {
    public:
        struct TextureValue
        {
            uint64_t id = 0u;
            uint32_t generation = 0u;
            uint32_t width = 0u;
            uint32_t height = 0u;
        };

        std::unordered_map<std::string, TextureValue> textures;
        std::unordered_map<std::string, uint64_t> fonts;
        int pumpCount = 0;

        static std::string Key(const std::string& assetId, const std::string& subAssetKey)
        {
            return assetId + "|" + subAssetKey;
        }

        std::optional<UIResolvedTexture> ResolveTexture(
            const UITextureReference& reference) override
        {
            const auto iterator = textures.find(Key(
                reference.GetAssetIdString(),
                reference.GetSubAssetKey()));
            if (iterator == textures.end())
                return std::nullopt;
            return UIResolvedTexture {
                iterator->second.id,
                iterator->second.generation,
                iterator->second.width,
                iterator->second.height };
        }

        std::optional<uint64_t> ResolveFont(
            const UIFontReference& reference) override
        {
            const auto iterator = fonts.find(Key(
                reference.GetAssetIdString(),
                reference.GetSubAssetKey()));
            if (iterator == fonts.end())
                return std::nullopt;
            return iterator->second;
        }

        void Pump() override { ++pumpCount; }
    };
}

class UIDocumentResourceResolverTests : public ::testing::Test
{
protected:
    void TearDown() override
    {
        ClearUIDocumentResourceResolver();
    }
};

TEST_F(UIDocumentResourceResolverTests, InstanceRefreshesResolvedTextureAndFont)
{
    auto document = UIDocument::LoadFromJson(ReferenceDocument());
    ASSERT_TRUE(document.has_value());
    const auto& definition = document->GetRootDefinition();
    ASSERT_TRUE(definition.texture.has_value());
    ASSERT_TRUE(definition.font.has_value());

    auto fake = std::make_shared<FakeDocumentResourceResolver>();
    fake->textures[FakeDocumentResourceResolver::Key(
        definition.texture->GetAssetIdString(),
        definition.texture->GetSubAssetKey())] = { 42u, 7u, 128u, 64u };
    fake->fonts[FakeDocumentResourceResolver::Key(
        definition.font->GetAssetIdString(),
        definition.font->GetSubAssetKey())] = 9u;
    SetUIDocumentResourceResolver(fake);

    const auto instance = document->Instantiate();
    ASSERT_NE(instance, nullptr);
    ASSERT_NE(instance->GetRoot(), nullptr);
    EXPECT_EQ(instance->GetRoot()->GetTextureId(), 42u);
    EXPECT_EQ(instance->GetRoot()->GetTextureGeneration(), 7u);
    EXPECT_EQ(instance->GetRoot()->GetFontAssetId(), 9u);
    EXPECT_EQ(instance->GetRoot()->GetFontPixelSize(), 22u);
    EXPECT_GE(fake->pumpCount, 1);
}

TEST_F(UIDocumentResourceResolverTests, PendingThenReadyUpdatesWithoutRebuilding)
{
    auto document = UIDocument::LoadFromJson(ReferenceDocument());
    ASSERT_TRUE(document.has_value());
    const auto& definition = document->GetRootDefinition();

    auto fake = std::make_shared<FakeDocumentResourceResolver>();
    SetUIDocumentResourceResolver(fake);

    const auto instance = document->Instantiate();
    ASSERT_NE(instance->GetRoot(), nullptr);
    EXPECT_EQ(instance->GetRoot()->GetTextureId(), 0u);
    EXPECT_EQ(instance->GetRoot()->GetFontAssetId(), 0u);

    fake->textures[FakeDocumentResourceResolver::Key(
        definition.texture->GetAssetIdString(),
        definition.texture->GetSubAssetKey())] = { 43u, 8u, 256u, 128u };
    fake->fonts[FakeDocumentResourceResolver::Key(
        definition.font->GetAssetIdString(),
        definition.font->GetSubAssetKey())] = 10u;

    const auto* rootBefore = instance->GetRoot();
    instance->RefreshResources();
    EXPECT_EQ(instance->GetRoot(), rootBefore) << "resource refresh must not rebuild the tree";
    EXPECT_EQ(instance->GetRoot()->GetTextureId(), 43u);
    EXPECT_EQ(instance->GetRoot()->GetTextureGeneration(), 8u);
    EXPECT_EQ(instance->GetRoot()->GetFontAssetId(), 10u);
    EXPECT_EQ(instance->GetRoot()->GetFontPixelSize(), 22u);
}

TEST_F(UIDocumentResourceResolverTests, GenerationsUpdateWhenTextureReloads)
{
    auto document = UIDocument::LoadFromJson(ReferenceDocument());
    ASSERT_TRUE(document.has_value());
    const auto& definition = document->GetRootDefinition();

    auto fake = std::make_shared<FakeDocumentResourceResolver>();
    const auto key = FakeDocumentResourceResolver::Key(
        definition.texture->GetAssetIdString(),
        definition.texture->GetSubAssetKey());
    fake->textures[key] = { 42u, 7u, 128u, 64u };
    SetUIDocumentResourceResolver(fake);

    const auto instance = document->Instantiate();
    ASSERT_EQ(instance->GetRoot()->GetTextureId(), 42u);
    ASSERT_EQ(instance->GetRoot()->GetTextureGeneration(), 7u);

    fake->textures[key] = { 42u, 11u, 128u, 64u };
    instance->RefreshResources();
    EXPECT_EQ(instance->GetRoot()->GetTextureGeneration(), 11u);
}

TEST_F(UIDocumentResourceResolverTests, LegacyRuntimeIdsAreNotOverwritten)
{
    auto document = UIDocument::LoadFromJson(LegacyDocument());
    ASSERT_TRUE(document.has_value());

    auto fake = std::make_shared<FakeDocumentResourceResolver>();
    SetUIDocumentResourceResolver(fake);

    const auto instance = document->Instantiate();
    ASSERT_NE(instance->GetRoot(), nullptr);
    EXPECT_EQ(instance->GetRoot()->GetTextureId(), 77u);
    EXPECT_EQ(instance->GetRoot()->GetTextureGeneration(), 3u);
    EXPECT_EQ(instance->GetRoot()->GetFontAssetId(), 5u);
    EXPECT_EQ(instance->GetRoot()->GetFontPixelSize(), 18u);
    EXPECT_EQ(fake->pumpCount, 0) << "documents without references must not resolve or pump";
}

TEST_F(UIDocumentResourceResolverTests, ReloadRebuildsResourceSlots)
{
    auto first = UIDocument::LoadFromJson(ReferenceDocument());
    ASSERT_TRUE(first.has_value());
    const auto& firstDefinition = first->GetRootDefinition();

    auto fake = std::make_shared<FakeDocumentResourceResolver>();
    fake->textures[FakeDocumentResourceResolver::Key(
        firstDefinition.texture->GetAssetIdString(),
        firstDefinition.texture->GetSubAssetKey())] = { 42u, 7u, 0u, 0u };
    SetUIDocumentResourceResolver(fake);

    const auto instance = first->Instantiate();
    ASSERT_EQ(instance->GetRoot()->GetTextureId(), 42u);

    auto second = UIDocument::LoadFromJson(ReferenceDocument());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(instance->Reload(*second));
    EXPECT_EQ(instance->GetRoot()->GetTextureId(), 42u);
}

TEST_F(UIDocumentResourceResolverTests, NineSliceEmitsNineTexturedQuadsWithInsets)
{
    auto document = UIDocument::LoadFromJson(NineSliceDocument(100, 50, 8.0f));
    ASSERT_TRUE(document.has_value());
    const auto& definition = document->GetRootDefinition().children.front();
    ASSERT_TRUE(definition.texture.has_value());

    auto fake = std::make_shared<FakeDocumentResourceResolver>();
    fake->textures[FakeDocumentResourceResolver::Key(
        definition.texture->GetAssetIdString(),
        definition.texture->GetSubAssetKey())] = { 42u, 7u, 64u, 32u };
    SetUIDocumentResourceResolver(fake);

    const auto instance = document->Instantiate();
    auto* frame = const_cast<UIDocumentInstance*>(instance.get())->GetElementByName("Frame");
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->GetTextureId(), 42u);
    EXPECT_EQ(frame->GetTextureGeneration(), 7u);
    EXPECT_EQ(frame->GetTextureWidth(), 64u);
    EXPECT_EQ(frame->GetTextureHeight(), 32u);
    EXPECT_FLOAT_EQ(frame->GetNineSlice().left, 8.0f);
    EXPECT_FLOAT_EQ(frame->GetNineSlice().top, 8.0f);
    EXPECT_FLOAT_EQ(frame->GetNineSlice().right, 8.0f);
    EXPECT_FLOAT_EQ(frame->GetNineSlice().bottom, 8.0f);

    const_cast<UIDocumentInstance*>(instance.get())->UpdateLayout({ 100.0f, 50.0f });
    const auto snapshot = instance->BuildDrawSnapshot(1u, 100u, 50u);
    ASSERT_NE(snapshot, nullptr);

    std::vector<const NLS::Render::UI::UIDrawBatch*> textured;
    for (const auto& batch : snapshot->batches)
    {
        if (batch.textureId == 42u)
            textured.push_back(&batch);
    }
    ASSERT_EQ(textured.size(), 9u);
    const auto* center = textured[4];
    ASSERT_GE(snapshot->vertices.size(), center->firstVertex + 4u);
    const auto& topLeft = snapshot->vertices[center->firstVertex];
    const auto& bottomRight = snapshot->vertices[center->firstVertex + 2u];
    EXPECT_FLOAT_EQ(topLeft.position[0], 8.0f);
    EXPECT_FLOAT_EQ(topLeft.position[1], 8.0f);
    EXPECT_FLOAT_EQ(topLeft.uv[0], 8.0f / 64.0f);
    EXPECT_FLOAT_EQ(topLeft.uv[1], 8.0f / 32.0f);
    EXPECT_FLOAT_EQ(bottomRight.position[0], 92.0f);
    EXPECT_FLOAT_EQ(bottomRight.position[1], 42.0f);
    EXPECT_FLOAT_EQ(bottomRight.uv[0], 56.0f / 64.0f);
    EXPECT_FLOAT_EQ(bottomRight.uv[1], 24.0f / 32.0f);
}

TEST_F(UIDocumentResourceResolverTests, NineSliceClampsBordersToTheDestinationRect)
{
    auto document = UIDocument::LoadFromJson(NineSliceDocument(10, 10, 8.0f));
    ASSERT_TRUE(document.has_value());
    const auto& definition = document->GetRootDefinition().children.front();

    auto fake = std::make_shared<FakeDocumentResourceResolver>();
    fake->textures[FakeDocumentResourceResolver::Key(
        definition.texture->GetAssetIdString(),
        definition.texture->GetSubAssetKey())] = { 42u, 7u, 32u, 32u };
    SetUIDocumentResourceResolver(fake);

    const auto instance = document->Instantiate();
    auto* instanceMutable = const_cast<UIDocumentInstance*>(instance.get());
    instanceMutable->UpdateLayout({ 10.0f, 10.0f });
    const auto snapshot = instanceMutable->BuildDrawSnapshot(1u, 10u, 10u);
    ASSERT_NE(snapshot, nullptr);

    std::vector<const NLS::Render::UI::UIDrawBatch*> textured;
    for (const auto& batch : snapshot->batches)
    {
        if (batch.textureId == 42u)
            textured.push_back(&batch);
    }
    // Both destination borders clamp to 5, so the center row/column collapse
    // and only the four corners remain.
    ASSERT_EQ(textured.size(), 4u);
    const auto& topLeft = snapshot->vertices[textured[0]->firstVertex];
    const auto& bottomRight = snapshot->vertices[textured[0]->firstVertex + 2u];
    EXPECT_FLOAT_EQ(topLeft.position[0], 0.0f);
    EXPECT_FLOAT_EQ(topLeft.position[1], 0.0f);
    EXPECT_FLOAT_EQ(bottomRight.position[0], 5.0f);
    EXPECT_FLOAT_EQ(bottomRight.position[1], 5.0f);
    // Source UVs still use the authored border (8 / 32), not the destination
    // clamp; otherwise the sliced image would stretch instead of crop.
    EXPECT_FLOAT_EQ(topLeft.uv[0], 0.0f);
    EXPECT_FLOAT_EQ(bottomRight.uv[0], 0.25f);
    EXPECT_FLOAT_EQ(bottomRight.uv[1], 0.25f);
}

TEST_F(UIDocumentResourceResolverTests, NineSliceWithoutTextureSizeFallsBackToASingleQuad)
{
    auto document = UIDocument::LoadFromJson(NineSliceDocument(100, 50, 8.0f));
    ASSERT_TRUE(document.has_value());
    const auto& definition = document->GetRootDefinition().children.front();

    auto fake = std::make_shared<FakeDocumentResourceResolver>();
    fake->textures[FakeDocumentResourceResolver::Key(
        definition.texture->GetAssetIdString(),
        definition.texture->GetSubAssetKey())] = { 42u, 7u, 0u, 0u };
    SetUIDocumentResourceResolver(fake);

    const auto instance = document->Instantiate();
    auto* instanceMutable = const_cast<UIDocumentInstance*>(instance.get());
    instanceMutable->UpdateLayout({ 100.0f, 50.0f });
    const auto snapshot = instanceMutable->BuildDrawSnapshot(1u, 100u, 50u);
    ASSERT_NE(snapshot, nullptr);

    std::vector<const NLS::Render::UI::UIDrawBatch*> textured;
    for (const auto& batch : snapshot->batches)
    {
        if (batch.textureId == 42u)
            textured.push_back(&batch);
    }
    ASSERT_EQ(textured.size(), 1u);
    const auto& topLeft = snapshot->vertices[textured[0]->firstVertex];
    const auto& bottomRight = snapshot->vertices[textured[0]->firstVertex + 2u];
    EXPECT_FLOAT_EQ(topLeft.uv[0], 0.0f);
    EXPECT_FLOAT_EQ(topLeft.uv[1], 0.0f);
    EXPECT_FLOAT_EQ(bottomRight.uv[0], 1.0f);
    EXPECT_FLOAT_EQ(bottomRight.uv[1], 1.0f);
}

TEST_F(UIDocumentResourceResolverTests, RuntimeResolverWithoutServicesIsSafe)
{
    const auto parsed = Guid::TryParse(kTextureGuid);
    ASSERT_TRUE(parsed.has_value());
    const Core::Assets::AssetId assetId(*parsed);

    const auto resolver = CreateRuntimeUIDocumentResourceResolver();
    ASSERT_NE(resolver, nullptr);
    const UITextureReference texture(assetId, "texture:main", {});
    const UIFontReference font(assetId, "font:main", {});
    EXPECT_FALSE(resolver->ResolveTexture(texture).has_value());
    EXPECT_FALSE(resolver->ResolveFont(font).has_value());
    resolver->Pump();
}

TEST_F(UIDocumentResourceResolverTests, RuntimeManifestFollowsUiTextureAndFontDependencies)
{
    const auto uiId = Core::Assets::AssetId(*Guid::TryParse(
        "11111111-1111-4111-8111-111111111111"));
    const auto textureId = Core::Assets::AssetId(*Guid::TryParse(kTextureGuid));
    const auto fontId = Core::Assets::AssetId(*Guid::TryParse(kFontGuid));

    const auto makeManifest = [](const Core::Assets::AssetId owner,
                                 const std::string& primarySubAssetKey,
                                 const Core::Assets::ArtifactType type,
                                 const std::string& loaderId,
                                 const char hash)
    {
        Core::Assets::ArtifactManifest manifest;
        manifest.sourceAssetId = owner;
        manifest.targetPlatform = "win64";
        manifest.primarySubAssetKey = primarySubAssetKey;

        Core::Assets::ImportedArtifact artifact;
        artifact.sourceAssetId = owner;
        artifact.subAssetKey = primarySubAssetKey;
        artifact.artifactType = type;
        artifact.loaderId = loaderId;
        artifact.targetPlatform = "win64";
        artifact.contentHash = "sha256:" + owner.ToString();
        artifact.artifactPath =
            (std::filesystem::path("Library") /
             "Artifacts" /
             Core::Assets::BuildArtifactStorageRelativePath(
                 std::string(64u, hash))).generic_string();
        manifest.subAssets.push_back(std::move(artifact));
        return manifest;
    };

    auto uiManifest = makeManifest(
        uiId,
        "ui-document:Hud",
        Core::Assets::ArtifactType::UIDocument,
        "ui-document",
        'a');
    uiManifest.dependencies.push_back({
        Core::Assets::AssetDependencyKind::ImportedArtifact,
        textureId.ToString(),
        "texture:main" });
    uiManifest.dependencies.push_back({
        Core::Assets::AssetDependencyKind::ImportedArtifact,
        fontId.ToString(),
        "font:Inter" });

    RuntimeManifestBuilder builder;
    builder.AddArtifactManifest(std::move(uiManifest));
    builder.AddArtifactManifest(makeManifest(
        textureId,
        "texture:main",
        Core::Assets::ArtifactType::Texture,
        "texture",
        'b'));
    builder.AddArtifactManifest(makeManifest(
        fontId,
        "font:Inter",
        Core::Assets::ArtifactType::Font,
        "font",
        'c'));

    const auto result = builder.Build(
        { RuntimeAssetRef { uiId, "ui-document:Hud" } },
        "win64");
    std::string diagnosticText;
    for (const auto& diagnostic : result.diagnostics.GetItems())
    {
        if (diagnostic.IsError())
            diagnosticText += diagnostic.code + " (" + diagnostic.path + "); ";
    }
    ASSERT_FALSE(result.diagnostics.HasErrors()) << diagnosticText;
    ASSERT_EQ(result.manifest.entries.size(), 3u);

    const Engine::Assets::RuntimeAssetDatabase database(result.manifest);
    const auto* texture = database.Resolve(textureId, "texture:main");
    const auto* font = database.Resolve(fontId, "font:Inter");
    ASSERT_NE(texture, nullptr);
    ASSERT_NE(font, nullptr);
    EXPECT_EQ(texture->artifactType, Core::Assets::ArtifactType::Texture);
    EXPECT_EQ(font->artifactType, Core::Assets::ArtifactType::Font);
    EXPECT_EQ(font->loaderId, "font");

    const auto* ui = database.Resolve(uiId, "ui-document:Hud");
    ASSERT_NE(ui, nullptr);
    EXPECT_NE(
        std::find(
            ui->dependencies.begin(),
            ui->dependencies.end(),
            RuntimeAssetRef { textureId, "texture:main" }),
        ui->dependencies.end());
    EXPECT_NE(
        std::find(
            ui->dependencies.begin(),
            ui->dependencies.end(),
            RuntimeAssetRef { fontId, "font:Inter" }),
        ui->dependencies.end());
}

TEST_F(UIDocumentResourceResolverTests, RuntimeResolverLoadsFontArtifactFromContentSource)
{
    ScopedDocumentDirectory directory;
    const auto sourceFont = std::filesystem::path(NLS_ROOT_DIR) /
        "App/Assets/Engine/Fonts/Ruda-Bold.ttf";
    std::ifstream input(sourceFont, std::ios::binary);
    ASSERT_TRUE(input.good());
    const std::vector<uint8_t> fontBytes {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>() };
    ASSERT_FALSE(fontBytes.empty());

    const auto fontAssetId = Core::Assets::AssetId(*Guid::TryParse(kFontGuid));
    Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = Core::Assets::ArtifactType::Font;
    metadata.schemaName = "font";
    metadata.schemaVersion = 1u;
    metadata.sourceAssetId = fontAssetId;
    metadata.subAssetKey = "font:Inter";
    metadata.displayName = "Inter";
    metadata.importerId = "font";
    metadata.importerVersion = 1u;
    metadata.targetPlatform = "editor";
    metadata.payloadHash = Core::Assets::ComputeNativeArtifactPayloadHash(fontBytes);
    const auto container = Core::Assets::WriteNativeArtifactContainer(
        metadata,
        fontBytes);
    ASSERT_FALSE(container.empty());

    const auto artifactPath = directory.Write("Artifacts/Inter.nlsa", {});
    {
        std::ofstream output(artifactPath, std::ios::binary | std::ios::trunc);
        output.write(
            reinterpret_cast<const char*>(container.data()),
            static_cast<std::streamsize>(container.size()));
    }

    RuntimeAssetManifest manifest;
    RuntimeAssetManifestEntry entry;
    entry.assetId = fontAssetId;
    entry.subAssetKey = "font:Inter";
    entry.artifactType = Core::Assets::ArtifactType::Font;
    entry.loaderId = "font";
    entry.artifactPath = artifactPath.generic_string();
    entry.contentHash = "sha256:font";
    manifest.entries.push_back(std::move(entry));

    Engine::Assets::RuntimeAssetDatabase database(std::move(manifest));
    Core::ServiceLocator::Provide<Engine::Assets::RuntimeAssetDatabase>(database);
    auto& contentSources = Core::Assets::ContentSourceTable::Get();
    contentSources.Clear();
    contentSources.Mount(
        directory.GetPath().generic_string(),
        std::make_shared<Core::Assets::DirectoryContentSource>(
            directory.GetPath()));
    struct Cleanup
    {
        ~Cleanup()
        {
            Core::ServiceLocator::Remove<Engine::Assets::RuntimeAssetDatabase>();
            Core::Assets::ContentSourceTable::Get().Clear();
        }
    } cleanup;

    const auto resolver = CreateRuntimeUIDocumentResourceResolver();
    ASSERT_NE(resolver, nullptr);
    const UIFontReference reference(fontAssetId, "font:Inter", {});
    const auto fontId = resolver->ResolveFont(reference);
    ASSERT_TRUE(fontId.has_value());
    EXPECT_TRUE(NLS::Render::UI::GetSharedUIFontAtlas().HasFace(*fontId));

    NLS::Render::UI::UIFontGlyph glyph;
    ASSERT_TRUE(NLS::Render::UI::GetSharedUIFontAtlas().EnsureGlyph(
        { *fontId, 'A', 20u },
        1u,
        glyph));
    EXPECT_FALSE(glyph.fallback);

    resolver->Invalidate(reference);
    EXPECT_FALSE(NLS::Render::UI::GetSharedUIFontAtlas().HasFace(*fontId));
    const auto reloaded = resolver->ResolveFont(reference);
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_TRUE(NLS::Render::UI::GetSharedUIFontAtlas().HasFace(*reloaded));

    NLS::Render::UI::GetSharedUIFontAtlas().InvalidateFont(*reloaded, 0u);
}

TEST_F(UIDocumentResourceResolverTests, PrimaryResolutionPrefersExplicitEmptyKey)
{
    const auto parsed = Guid::TryParse(kTextureGuid);
    ASSERT_TRUE(parsed.has_value());
    const Core::Assets::AssetId assetId(*parsed);

    RuntimeAssetManifest manifest;
    RuntimeAssetManifestEntry keyed;
    keyed.assetId = assetId;
    keyed.subAssetKey = "texture:main";
    keyed.artifactPath = "texture-keyed.nlsa";
    RuntimeAssetManifestEntry primary;
    primary.assetId = assetId;
    primary.subAssetKey = "";
    primary.artifactPath = "texture-primary.nlsa";
    manifest.entries = { keyed, primary };

    const Engine::Assets::RuntimeAssetDatabase database(std::move(manifest));
    const auto* resolved = database.ResolvePrimary(assetId);
    ASSERT_NE(resolved, nullptr);
    EXPECT_EQ(resolved->artifactPath, "texture-primary.nlsa");
    EXPECT_EQ(database.Resolve(assetId, "texture:main")->artifactPath, "texture-keyed.nlsa");
}

TEST_F(UIDocumentResourceResolverTests, PrimaryResolutionFallsBackToFirstSubAsset)
{
    const auto parsed = Guid::TryParse(kFontGuid);
    ASSERT_TRUE(parsed.has_value());
    const Core::Assets::AssetId assetId(*parsed);

    RuntimeAssetManifest manifest;
    RuntimeAssetManifestEntry first;
    first.assetId = assetId;
    first.subAssetKey = "font:main";
    first.artifactPath = "font.nlsa";
    RuntimeAssetManifestEntry second;
    second.assetId = assetId;
    second.subAssetKey = "font:alt";
    second.artifactPath = "font-alt.nlsa";
    manifest.entries = { first, second };

    const Engine::Assets::RuntimeAssetDatabase database(std::move(manifest));
    const auto* resolved = database.ResolvePrimary(assetId);
    ASSERT_NE(resolved, nullptr);
    EXPECT_EQ(resolved->artifactPath, "font.nlsa");
}

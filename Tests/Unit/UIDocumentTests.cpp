#include <gtest/gtest.h>

#include "Assets/AssetDatabaseFacade.h"
#include "Assets/AssetMeta.h"
#include "Engine/UI/UIDataContext.h"
#include "Engine/UI/UIDocument.h"
#include "Engine/UI/UIDocumentSourceResolver.h"
#include "Engine/UI/UIInputRouter.h"
#include "Engine/UI/UIStyleSheet.h"
#include "Engine/UI/UIWorldHitTest.h"
#include "Engine/UI/UILocalizationTable.h"
#include "Engine/UI/UIBinding.h"
#include "Components/UICanvasComponent.h"
#include "Guid.h"
#include "Rendering/UI/UIDrawSnapshotAdapter.h"
#include "Rendering/UI/UITextureRegistry.h"
#include "Rendering/UI/UIFontAtlas.h"
#include "Rendering/UI/UIFontTextShaper.h"
#include "Rendering/UI/UIFontAtlasUploader.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace NLS::Engine::UI;

TEST(UIDocumentTests, ParsesSchemaBuildsStableTreeAndSnapshot)
{
    const std::string source = R"json({
        "version": 1,
        "documentId": "11111111-1111-4111-8111-111111111111",
        "root": {
            "type": "Panel",
            "name": "Root",
            "layout": { "mode": "Overlay", "size": [200, 100], "clipChildren": true },
            "style": { "backgroundColor": [0.1, 0.2, 0.3, 1.0] },
            "children": [
                { "type": "Button", "name": "Play", "layout": { "size": [80, 24] }, "text": "Play" }
            ]
        }
    })json";

    std::vector<UIDocumentDiagnostic> diagnostics;
    auto document = UIDocument::LoadFromJson(source, &diagnostics);
    ASSERT_TRUE(document.has_value());
    EXPECT_TRUE(diagnostics.empty());

    auto instance = document->Instantiate();
    ASSERT_NE(instance, nullptr);
    ASSERT_NE(instance->GetRoot(), nullptr);
    auto* play = instance->GetElementByName("Play");
    ASSERT_NE(play, nullptr);
    EXPECT_EQ(play->GetElementType(), UIElementType::Button);

    instance->UpdateLayout({ 200.0f, 100.0f });
    EXPECT_FLOAT_EQ(play->GetLayoutRect().width, 80.0f);
    EXPECT_FLOAT_EQ(play->GetLayoutRect().height, 24.0f);

    const auto snapshot = instance->BuildDrawSnapshot(7u, 200u, 100u);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->frameId, 7u);
    EXPECT_EQ(snapshot->viewportWidth, 200u);
    EXPECT_GE(snapshot->diagnostics.nodeCount, 2u);
    EXPECT_GE(snapshot->batches.size(), 1u);
}

TEST(UIDocumentTests, RejectsDuplicateNodeGuidAndUnsupportedVersion)
{
    const std::string duplicate = R"json({
        "version": 1,
        "root": { "type": "Panel", "nodeGuid": "11111111-1111-4111-8111-111111111111", "children": [
            { "type": "Text", "nodeGuid": "11111111-1111-4111-8111-111111111111" }
        ] }
    })json";
    std::vector<UIDocumentDiagnostic> diagnostics;
    EXPECT_FALSE(UIDocument::LoadFromJson(duplicate, &diagnostics).has_value());
    ASSERT_FALSE(diagnostics.empty());

    diagnostics.clear();
    EXPECT_FALSE(UIDocument::LoadFromJson(R"({"version":99,"root":{"type":"Panel"}})", &diagnostics).has_value());
    ASSERT_FALSE(diagnostics.empty());
}

TEST(UIDocumentTests, InputRouterDispatchesCaptureTargetBubbleAndClick)
{
    auto root = std::make_unique<UIElement>(UIElementType::Panel, "Root");
    auto child = std::make_unique<UIElement>(UIElementType::Button, "Button");
    UILayoutParams rootLayout;
    rootLayout.size = { 100.0f, 100.0f };
    rootLayout.mode = UILayoutMode::Overlay;
    root->SetLayout(rootLayout);
    UILayoutParams childLayout;
    childLayout.size = { 40.0f, 40.0f };
    child->SetLayout(childLayout);
    auto& childReference = root->AddChild(std::move(child));
    root->Measure({ 100.0f, 100.0f });
    root->Arrange({ 0.0f, 0.0f, 100.0f, 100.0f });

    std::string phases;
    root->AddEventListener(UIEventType::PointerDown, [&phases](UIEvent&) { phases += "C"; }, true);
    root->AddEventListener(UIEventType::PointerDown, [&phases](UIEvent&) { phases += "B"; });
    childReference.AddEventListener(UIEventType::PointerDown, [&phases](UIEvent&) { phases += "T"; });
    bool clicked = false;
    childReference.AddEventListener(UIEventType::Click, [&clicked](UIEvent&) { clicked = true; });

    UIInputRouter router;
    router.SetRoot(root.get());
    EXPECT_TRUE(router.DispatchPointer(UIEventType::PointerDown, { 10.0f, 10.0f }));
    EXPECT_TRUE(router.DispatchPointer(UIEventType::PointerUp, { 10.0f, 10.0f }));
    EXPECT_EQ(phases, "CTB");
    EXPECT_TRUE(clicked);
}

TEST(UIDocumentTests, DataContextNotifiesOnlyChangedPath)
{
    UIDataContext context;
    int notifications = 0;
    const auto subscription = context.Subscribe("player.health", [&notifications](const std::string&, const UIValue&) {
        ++notifications;
    });
    ASSERT_TRUE(subscription.IsValid());
    EXPECT_TRUE(context.Set("player.health", int64_t(100)));
    EXPECT_FALSE(context.Set("player.health", int64_t(100)));
    EXPECT_TRUE(context.Set("player.health", int64_t(75)));
    EXPECT_EQ(notifications, 2);
    EXPECT_TRUE(context.Unsubscribe(subscription));
}

TEST(UIDocumentTests, AssetDatabaseRecognizesGameUiAssets)
{
    using NLS::Core::Assets::AssetType;
    EXPECT_EQ(NLS::Core::Assets::InferAssetType("HUD.ui"), AssetType::UIDocument);
    EXPECT_EQ(NLS::Core::Assets::InferAssetType("Theme.style"), AssetType::UIStyleSheet);
    EXPECT_EQ(NLS::Core::Assets::InferAssetType("Strings.loc"), AssetType::LocalizationTable);
    EXPECT_EQ(NLS::Core::Assets::InferAssetType("NotoSansCJK.otf"), AssetType::Font);
}

TEST(UIDocumentTests, StyleSheetUsesSpecificityAndSourceOrder)
{
    const auto sheet = UIStyleSheet::LoadFromJson(R"json({
        "version": 1,
        "rules": [
            { "selector": { "type": "Button" }, "style": { "opacity": 0.2 } },
            { "selector": { "class": "Primary" }, "style": { "opacity": 0.8 } },
            { "selector": { "id": "Play" }, "style": { "opacity": 0.9 } }
        ]
    })json");
    ASSERT_TRUE(sheet.has_value());

    UIElement element(UIElementType::Button, "Play");
    element.SetClassName("Primary");
    EXPECT_FLOAT_EQ(sheet->Resolve(element).opacity, 0.9f);
}

TEST(UIDocumentTests, InputQueueRunsToggleDefaultActionAndValueChanged)
{
    auto root = std::make_unique<UIElement>(UIElementType::Panel, "Root");
    UILayoutParams rootLayout;
    rootLayout.mode = UILayoutMode::Overlay;
    rootLayout.size = { 100.0f, 100.0f };
    root->SetLayout(rootLayout);
    auto toggle = std::make_unique<UIElement>(UIElementType::Toggle, "Toggle");
    UILayoutParams toggleLayout;
    toggleLayout.size = { 30.0f, 30.0f };
    toggle->SetLayout(toggleLayout);
    auto& toggleRef = root->AddChild(std::move(toggle));
    root->Measure({ 100.0f, 100.0f });
    root->Arrange({ 0.0f, 0.0f, 100.0f, 100.0f });

    int changes = 0;
    toggleRef.AddEventListener(UIEventType::ValueChanged, [&changes](UIEvent&) { ++changes; });
    InputEventQueue queue;
    queue.Push({ UIEventType::PointerDown, UIInputDevice::Mouse, 1u, 0u, 0u, 0u, 0u, { 5.0f, 5.0f } });
    queue.Push({ UIEventType::PointerUp, UIInputDevice::Mouse, 2u, 0u, 0u, 0u, 0u, { 5.0f, 5.0f } });

    UIInputRouter router;
    router.SetRoot(root.get());
    for (const auto& event : queue.Drain())
        router.Dispatch(event);

    EXPECT_TRUE(toggleRef.IsChecked());
    EXPECT_EQ(changes, 1);
}

TEST(UIDocumentTests, WorldUiPlaneAndTriangleReturnNearestUv)
{
    UIWorldHitResult planeHit;
    EXPECT_TRUE(IntersectWorldCanvasPlane(
        { { 0.5f, 0.5f, 1.0f }, { 0.0f, 0.0f, -1.0f } },
        { 0.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f },
        { 1.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f },
        { 1.0f, 1.0f },
        planeHit));
    EXPECT_FLOAT_EQ(planeHit.distance, 1.0f);
    EXPECT_FLOAT_EQ(planeHit.uv.x, 0.5f);
    EXPECT_FLOAT_EQ(planeHit.uv.y, 0.5f);

    const UIWorldTriangle triangles[] = {
        { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
            { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f } },
        { { 0.0f, 0.0f, 0.5f }, { 1.0f, 0.0f, 0.5f }, { 0.0f, 1.0f, 0.5f },
            { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f } }
    };
    UIWorldHitResult triangleHit;
    EXPECT_TRUE(IntersectWorldCanvasTriangles(
        { { 0.25f, 0.25f, 1.0f }, { 0.0f, 0.0f, -1.0f } },
        triangles,
        triangleHit));
    EXPECT_EQ(triangleHit.triangleIndex, 1u);
    EXPECT_FLOAT_EQ(triangleHit.uv.x, 0.25f);
    EXPECT_FLOAT_EQ(triangleHit.uv.y, 0.25f);
}

TEST(UIDocumentTests, RetainedSnapshotAdaptsToRhiOverlaySnapshot)
{
    NLS::Render::UI::UIDrawSnapshot source;
    source.frameId = 42u;
    source.viewportWidth = 1280u;
    source.viewportHeight = 720u;
    source.dpiScale = 1.0f;
    const auto clip = source.AddClip({ 4.0f, 8.0f, 100.0f, 80.0f });
    source.AddQuad(
        10.0f,
        20.0f,
        90.0f,
        60.0f,
        { 0.25f, 0.5f, 0.75f, 1.0f },
        clip,
        17u,
        7u);
    source.textRuns.push_back({
        "Play", 10.0f, 20.0f, 80.0f, 24.0f, 16.0f,
        { 1.0f, 1.0f, 1.0f, 1.0f }, clip, 0u, 8u });

    const auto adapted = NLS::Render::UI::AdaptUIDrawSnapshot(source);
    ASSERT_NE(adapted, nullptr);
    EXPECT_EQ(adapted->frameId, 42u);
    EXPECT_EQ(adapted->displaySize[0], 1280.0f);
    EXPECT_EQ(adapted->displaySize[1], 720.0f);
    ASSERT_EQ(adapted->drawLists.size(), 1u);
    ASSERT_EQ(adapted->drawLists.front().commands.size(), 1u);
    EXPECT_EQ(adapted->drawLists.front().commands.front().elementCount, 6u);
    EXPECT_EQ(adapted->drawLists.front().commands.front().textureId.value, 17u);
    EXPECT_EQ(adapted->drawLists.front().commands.front().textureId.generation, 1u);
    EXPECT_EQ(adapted->textRuns.size(), 1u);
    EXPECT_TRUE(adapted->containsDeferredText);
    EXPECT_TRUE(adapted->hasVisibleDraws);
    EXPECT_NE(adapted->contentSignature, 0u);
}

TEST(UIDocumentTests, TextChangesInvalidateRetainedSnapshotSignature)
{
    const auto document = UIDocument::LoadFromJson(R"json({
        "version": 1,
        "root": { "type": "Text", "name": "Label", "text": "Before" }
    })json");
    ASSERT_TRUE(document.has_value());

    auto instance = document->Instantiate();
    ASSERT_NE(instance, nullptr);
    instance->UpdateLayout({ 320.0f, 80.0f });
    const auto before = instance->BuildDrawSnapshot(1u, 320u, 80u);
    ASSERT_NE(before, nullptr);
    ASSERT_EQ(before->textRuns.size(), 1u);

    instance->GetRoot()->SetText("After");
    const auto after = instance->BuildDrawSnapshot(2u, 320u, 80u);
    ASSERT_NE(after, nullptr);
    ASSERT_EQ(after->textRuns.size(), 1u);
    EXPECT_EQ(after->textRuns.front().text, "After");

    const auto beforeRhi = NLS::Render::UI::AdaptUIDrawSnapshot(*before);
    const auto afterRhi = NLS::Render::UI::AdaptUIDrawSnapshot(*after);
    ASSERT_NE(beforeRhi, nullptr);
    ASSERT_NE(afterRhi, nullptr);
    EXPECT_NE(beforeRhi->contentSignature, afterRhi->contentSignature);
}

TEST(UIDocumentTests, TextRunsProduceGlyphQuadsWithAtlasUvs)
{
    NLS::Render::UI::SetSharedUIFontAtlasFontPath({});
    NLS::Render::UI::ClearUiFontAtlasTextureId();

    const auto document = UIDocument::LoadFromJson(R"json({
        "version": 1,
        "root": {
            "type": "Text",
            "name": "Score",
            "text": "Score: 0",
            "style": { "backgroundColor": [0.0, 0.0, 0.0, 0.0], "color": [1.0, 1.0, 1.0, 1.0] },
            "layout": { "size": [200, 32] }
        }
    })json");
    ASSERT_TRUE(document.has_value());

    auto instance = document->Instantiate();
    ASSERT_NE(instance, nullptr);
    instance->UpdateLayout({ 320.0f, 120.0f });
    const auto withoutAtlas = instance->BuildDrawSnapshot(6u, 320u, 120u);
    ASSERT_NE(withoutAtlas, nullptr);
    EXPECT_EQ(withoutAtlas->vertices.size(), 0u)
        << "text stays invisible until a font atlas texture is registered";

    // Registering the atlas texture is what makes text produce geometry.
    NLS::Render::UI::SetUiFontAtlasTextureId(4242u, 3u);
    instance->GetRoot()->SetText("Score: 1");
    const auto snapshot = instance->BuildDrawSnapshot(7u, 320u, 120u);
    ASSERT_NE(snapshot, nullptr);
    ASSERT_EQ(snapshot->textRuns.size(), 1u);

    // Text must reach the render consumer as geometry, not only as metadata.
    ASSERT_GE(snapshot->vertices.size(), 4u) << "text runs must emit glyph quads";
    EXPECT_EQ(snapshot->vertices.size() % 4u, 0u);
    EXPECT_GE(snapshot->batches.size(), 1u);
    EXPECT_EQ(snapshot->batches.front().textureId, 4242u)
        << "glyph quads sample the registered UI font atlas texture";
    EXPECT_EQ(snapshot->batches.front().textureGeneration, 3u);

    // Consecutive glyphs must reference distinct atlas cells.
    ASSERT_GE(snapshot->vertices.size(), 8u);
    const auto firstU = snapshot->vertices[0].uv[0];
    const auto secondU = snapshot->vertices[4].uv[0];
    EXPECT_TRUE(firstU != secondU || snapshot->vertices[0].uv[1] != snapshot->vertices[4].uv[1])
        << "each glyph needs its own atlas sub-rect";

    // Every quad must sample exactly the rasterized bitmap. Sampling the whole
    // padded atlas cell squeezed the glyph into a corner of its quad, which is
    // why text used to render as unreadable smudges instead of "Score: 1".
    const auto& atlas = NLS::Render::UI::GetSharedUIFontAtlas();
    ASSERT_FALSE(atlas.GetPages().empty());
    const auto pageWidth = static_cast<float>(atlas.GetPages().front().width);
    float stackHeight = 0.0f;
    for (const auto& page : atlas.GetPages())
        stackHeight += static_cast<float>(page.height);
    ASSERT_GT(pageWidth, 0.0f);
    ASSERT_GT(stackHeight, 0.0f);

    const auto quadWidth = snapshot->vertices[2].position[0] - snapshot->vertices[0].position[0];
    const auto quadHeight = snapshot->vertices[2].position[1] - snapshot->vertices[0].position[1];
    const auto sampledWidth =
        (snapshot->vertices[2].uv[0] - snapshot->vertices[0].uv[0]) * pageWidth;
    const auto sampledHeight =
        (snapshot->vertices[2].uv[1] - snapshot->vertices[0].uv[1]) * stackHeight;
    EXPECT_GT(quadWidth, 0.0f);
    EXPECT_GT(quadHeight, 0.0f);
    EXPECT_NEAR(sampledWidth, quadWidth, 0.5f)
        << "glyph quads must sample the bitmap rect, not the padded atlas cell";
    EXPECT_NEAR(sampledHeight, quadHeight, 0.5f)
        << "glyph quads must sample the bitmap rect, not the padded atlas cell";

    NLS::Render::UI::ClearUiFontAtlasTextureId();
}

TEST(UIDocumentTests, TempDumpProjectHudVertexLayout)
{
    const auto hudPath = std::filesystem::path(NLS_ROOT_DIR) /
        "TestProject" / "Assets" / "HUD.ui";
    const auto fontPath = std::filesystem::path(NLS_ROOT_DIR) /
        "App" / "Assets" / "Engine" / "Fonts" / "Ruda-Bold.ttf";
    if (!std::filesystem::is_regular_file(hudPath) || !std::filesystem::is_regular_file(fontPath))
        GTEST_SKIP() << "project HUD or engine font is unavailable";

    auto& atlas = NLS::Render::UI::GetSharedUIFontAtlas();
    ASSERT_TRUE(atlas.LoadFontFromFile(fontPath));
    // Same order as the editor: prewarm printable ASCII, then upload, then shape.
    constexpr std::string_view prewarm =
        " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
    (void)atlas.EnsureText(1u, prewarm, 16u, 1u, true);
    NLS::Render::UI::SetUiFontAtlasTextureId(9u, 1u);

    std::vector<UIDocumentDiagnostic> diagnostics;
    auto document = UIDocument::LoadFromFile(hudPath.generic_string(), &diagnostics);
    for (const auto& diagnostic : diagnostics)
        GTEST_LOG_(INFO) << "temp-hud|diagnostic " << diagnostic.path << ": " << diagnostic.message;
    ASSERT_TRUE(document.has_value());
    auto instance = document->Instantiate();
    ASSERT_NE(instance, nullptr);
    instance->UpdateLayout({ 320.0f, 120.0f });
    const auto snapshot = instance->BuildDrawSnapshot(31u, 320u, 120u);
    ASSERT_NE(snapshot, nullptr);

    GTEST_LOG_(INFO) << "temp-hud| vertices=" << snapshot->vertices.size()
                     << " indices=" << snapshot->indices.size()
                     << " batches=" << snapshot->batches.size()
                     << " clips=" << snapshot->clips.size();
    for (size_t index = 0u; index < snapshot->vertices.size(); ++index)
    {
        const auto& vertex = snapshot->vertices[index];
        GTEST_LOG_(INFO) << "temp-hud| v" << index
                         << " pos=" << vertex.position[0] << "," << vertex.position[1]
                         << " uv=" << vertex.uv[0] << "," << vertex.uv[1];
    }
    for (size_t index = 0u; index < snapshot->batches.size(); ++index)
    {
        const auto& batch = snapshot->batches[index];
        GTEST_LOG_(INFO) << "temp-hud| b" << index
                         << " firstVertex=" << batch.firstVertex
                         << " vertexCount=" << batch.vertexCount
                         << " firstIndex=" << batch.firstIndex
                         << " indexCount=" << batch.indexCount
                         << " texture=" << batch.textureId
                         << " gen=" << batch.textureGeneration
                         << " clip=" << batch.clipIndex;
    }
    GTEST_LOG_(INFO) << "temp-hud| indices:";
    {
        std::string line = "temp-hud| ";
        for (const auto value : snapshot->indices)
            line += std::to_string(value) + " ";
        GTEST_LOG_(INFO) << line;
    }
    NLS::Render::UI::ClearUiFontAtlasTextureId();
}

TEST(UIDocumentTests, EveryGlyphQuadResolvesToItsOwnVerticesThroughTheDrawBase)
{
    const auto fontPath = std::filesystem::path(NLS_ROOT_DIR) /
        "App" / "Assets" / "Engine" / "Fonts" / "Ruda-Bold.ttf";
    if (!std::filesystem::is_regular_file(fontPath))
        GTEST_SKIP() << "engine font sample is unavailable";
    ASSERT_TRUE(NLS::Render::UI::GetSharedUIFontAtlas().LoadFontFromFile(fontPath));
    NLS::Render::UI::SetUiFontAtlasTextureId(4242u, 1u);

    const auto document = UIDocument::LoadFromJson(R"json({
        "version": 1,
        "root": { "type": "Text", "name": "Score", "text": "abcdefgh",
            "style": { "color": [1.0, 1.0, 1.0, 1.0] },
            "layout": { "size": [200, 32] } }
    })json");
    ASSERT_TRUE(document.has_value());
    auto instance = document->Instantiate();
    ASSERT_NE(instance, nullptr);
    instance->UpdateLayout({ 320.0f, 120.0f });
    const auto snapshot = instance->BuildDrawSnapshot(21u, 320u, 120u);
    ASSERT_NE(snapshot, nullptr);
    ASSERT_EQ(snapshot->batches.size(), 8u);

    // Reproduce exactly how the overlay renderer binds a batch: the draw call's
    // vertex offset is the batch base, and the stored indices are added to it.
    // If the indices already contain the base, each quad resolves to the wrong
    // vertices and alternating glyphs disappear.
    for (size_t batchIndex = 0u; batchIndex < snapshot->batches.size(); ++batchIndex)
    {
        const auto& batch = snapshot->batches[batchIndex];
        ASSERT_GE(batch.indexCount, 3u);

        float resolvedMinX = 1.0e9f;
        float resolvedMinY = 1.0e9f;
        for (uint32_t element = 0u; element < batch.indexCount; ++element)
        {
            const auto index = snapshot->indices[batch.firstIndex + element];
            const auto resolvedVertex = batch.firstVertex + index;
            ASSERT_LT(resolvedVertex, snapshot->vertices.size())
                << "batch " << batchIndex << " resolves outside the vertex buffer";
            resolvedMinX = (std::min)(resolvedMinX, snapshot->vertices[resolvedVertex].position[0]);
            resolvedMinY = (std::min)(resolvedMinY, snapshot->vertices[resolvedVertex].position[1]);
        }

        const auto& expected = snapshot->vertices[batch.firstVertex];
        EXPECT_FLOAT_EQ(resolvedMinX, expected.position[0])
            << "batch " << batchIndex << " must draw its own glyph";
        EXPECT_FLOAT_EQ(resolvedMinY, expected.position[1])
            << "batch " << batchIndex << " must draw its own glyph";
    }

    NLS::Render::UI::ClearUiFontAtlasTextureId();
}

TEST(UIDocumentTests, RegisteringTheFontAtlasTextureRebuildsCachedTextGeometry)
{
    const auto fontPath = std::filesystem::path(NLS_ROOT_DIR) /
        "App" / "Assets" / "Engine" / "Fonts" / "Ruda-Bold.ttf";
    if (!std::filesystem::is_regular_file(fontPath))
        GTEST_SKIP() << "engine font sample is unavailable";
    ASSERT_TRUE(NLS::Render::UI::GetSharedUIFontAtlas().LoadFontFromFile(fontPath));
    NLS::Render::UI::ClearUiFontAtlasTextureId();

    const auto document = UIDocument::LoadFromJson(R"json({
        "version": 1,
        "root": { "type": "Text", "name": "Score", "text": "Score: 0",
            "style": { "color": [1.0, 1.0, 1.0, 1.0] },
            "layout": { "size": [200, 32] } }
    })json");
    ASSERT_TRUE(document.has_value());
    auto instance = document->Instantiate();
    ASSERT_NE(instance, nullptr);
    instance->UpdateLayout({ 320.0f, 120.0f });

    // Without a registered atlas texture there is nothing to sample, so the
    // document legitimately keeps its snapshot free of glyph geometry.
    const auto withoutAtlas = instance->BuildDrawSnapshot(20u, 320u, 120u);
    ASSERT_NE(withoutAtlas, nullptr);
    EXPECT_EQ(withoutAtlas->vertices.size(), 0u);

    // Registering the texture must invalidate that cached snapshot. Resolving
    // text lazily and then reusing a text-less snapshot is what left individual
    // glyphs (the ones added to the atlas last) missing on screen.
    NLS::Render::UI::SetUiFontAtlasTextureId(4242u, 1u);
    const auto withAtlas = instance->BuildDrawSnapshot(21u, 320u, 120u);
    ASSERT_NE(withAtlas, nullptr);
    EXPECT_GE(withAtlas->vertices.size(), 4u)
        << "cached text geometry must be rebuilt once the atlas texture exists";
    EXPECT_EQ(withAtlas->batches.size(), 7u);

    NLS::Render::UI::ClearUiFontAtlasTextureId();
}

TEST(UIDocumentTests, PrewarmedAtlasStillRendersEveryDocumentGlyph)
{
    const auto fontPath = std::filesystem::path(NLS_ROOT_DIR) /
        "App" / "Assets" / "Engine" / "Fonts" / "Ruda-Bold.ttf";
    if (!std::filesystem::is_regular_file(fontPath))
        GTEST_SKIP() << "engine font sample is unavailable";
    auto& atlas = NLS::Render::UI::GetSharedUIFontAtlas();
    ASSERT_TRUE(atlas.LoadFontFromFile(fontPath));

    // Same order as the editor: the uploader prewarms printable ASCII first and
    // the document is shaped afterwards into that page.
    constexpr std::string_view prewarm =
        " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
    (void)atlas.EnsureText(1u, prewarm, 16u, 1u, true);

    NLS::Render::UI::SetUiFontAtlasTextureId(7u, 1u);
    const auto document = UIDocument::LoadFromJson(R"json({
        "version": 1,
        "root": { "type": "Text", "name": "Score", "text": "Score: 0",
            "style": { "color": [1.0, 1.0, 1.0, 1.0] },
            "layout": { "size": [200, 32] } }
    })json");
    ASSERT_TRUE(document.has_value());
    auto instance = document->Instantiate();
    ASSERT_NE(instance, nullptr);
    instance->UpdateLayout({ 320.0f, 120.0f });
    const auto snapshot = instance->BuildDrawSnapshot(12u, 320u, 120u);
    ASSERT_NE(snapshot, nullptr);

    const auto image = NLS::Render::UI::BuildUIFontAtlasImage(atlas);
    ASSERT_GT(image.width, 0u);
    ASSERT_GT(image.height, 0u);

    size_t index = 0u;
    std::string report;
    for (size_t quad = 0u; quad + 4u <= snapshot->vertices.size(); quad += 4u)
    {
        const auto& first = snapshot->vertices[quad];
        const auto& opposite = snapshot->vertices[quad + 2u];
        const auto left = static_cast<int32_t>(first.position[0]);
        const auto top = static_cast<int32_t>(first.position[1]);
        const auto right = static_cast<int32_t>(opposite.position[0]);
        const auto bottom = static_cast<int32_t>(opposite.position[1]);
        uint32_t ink = 0u;
        for (int32_t y = top; y < bottom; ++y)
        {
            for (int32_t x = left; x < right; ++x)
            {
                if (x < 0 || y < 0 || x >= 320 || y >= 120)
                    continue;
                const auto fx = (static_cast<float>(x) + 0.5f - first.position[0]) /
                    (opposite.position[0] - first.position[0]);
                const auto fy = (static_cast<float>(y) + 0.5f - first.position[1]) /
                    (opposite.position[1] - first.position[1]);
                const auto u = first.uv[0] + (opposite.uv[0] - first.uv[0]) * fx;
                const auto v = first.uv[1] + (opposite.uv[1] - first.uv[1]) * fy;
                const auto tx = (std::min)(image.width - 1u,
                    static_cast<uint32_t>(u * static_cast<float>(image.width)));
                const auto ty = (std::min)(image.height - 1u,
                    static_cast<uint32_t>(v * static_cast<float>(image.height)));
                if (image.rgba[(static_cast<size_t>(ty) * image.width + tx) * 4u + 3u] != 0u)
                    ++ink;
            }
        }
        report += "quad" + std::to_string(quad / 4u) + " ink=" + std::to_string(ink) + "; ";
        EXPECT_GT(ink, 0u)
            << "glyph quad " << (quad / 4u) << " samples an empty atlas cell after prewarm";
        ++index;
    }
    EXPECT_EQ(index, 7u) << "\"Score: 0\" must shape seven visible glyphs";
    EXPECT_TRUE(report.empty() == false);
    NLS::Render::UI::ClearUiFontAtlasTextureId();
}

TEST(UIDocumentTests, GlyphQuadsSampleExactlyTheirBitmap)
{
    // The test working directory is the repository root, where the engine-relative
    // font path does not resolve. Point the shared atlas at the shipped font so
    // this test covers the real rasterized path instead of the tofu fallback.
    const auto fontPath = std::filesystem::path(NLS_ROOT_DIR) /
        "App" / "Assets" / "Engine" / "Fonts" / "Ruda-Bold.ttf";
    if (!std::filesystem::is_regular_file(fontPath))
        GTEST_SKIP() << "engine font sample is unavailable";
    // LoadFontFromFile also drops glyphs cached by earlier tests, so this stays
    // independent of test order and of the shared atlas' lazy default font.
    ASSERT_TRUE(NLS::Render::UI::GetSharedUIFontAtlas().LoadFontFromFile(fontPath));
    NLS::Render::UI::SetUiFontAtlasTextureId(7u, 1u);
    // Diagnostics counters are process-wide, so this test compares deltas.
    const auto glyphDiagnosticsBefore =
        NLS::Render::UI::GetSharedUIFontAtlas().GetDiagnostics();

    const auto document = UIDocument::LoadFromJson(R"json({
        "version": 1,
        "root": {
            "type": "Text",
            "name": "Score",
            "text": "Score: 0",
            "style": { "backgroundColor": [0.0, 0.0, 0.0, 0.0], "color": [1.0, 1.0, 1.0, 1.0] },
            "layout": { "size": [200, 32] }
        }
    })json");
    ASSERT_TRUE(document.has_value());
    auto instance = document->Instantiate();
    ASSERT_NE(instance, nullptr);
    instance->UpdateLayout({ 320.0f, 120.0f });
    const auto snapshot = instance->BuildDrawSnapshot(11u, 320u, 120u);
    ASSERT_NE(snapshot, nullptr);
    ASSERT_GE(snapshot->vertices.size(), 4u);

    const auto atlasImage = NLS::Render::UI::BuildUIFontAtlasImage(
        NLS::Render::UI::GetSharedUIFontAtlas());
    ASSERT_GT(atlasImage.width, 0u);
    ASSERT_GT(atlasImage.height, 0u);

    const auto& fontAtlas = NLS::Render::UI::GetSharedUIFontAtlas();
    ASSERT_TRUE(fontAtlas.HasFontData()) << "the engine font must back this glyph test";
    const auto& glyphDiagnosticsAfter = fontAtlas.GetDiagnostics();
    EXPECT_GT(
        glyphDiagnosticsAfter.generatedGlyphs - glyphDiagnosticsBefore.generatedGlyphs,
        0u);
    EXPECT_EQ(
        glyphDiagnosticsAfter.fallbackGlyphs - glyphDiagnosticsBefore.fallbackGlyphs,
        0u)
        << "every glyph of \"Score: 0\" must come from the real font";

    constexpr uint32_t canvasWidth = 320u;
    constexpr uint32_t canvasHeight = 120u;
    std::vector<uint8_t> canvas(static_cast<size_t>(canvasWidth) * canvasHeight * 4u, 0u);
    for (size_t index = 0u; index + 4u <= snapshot->vertices.size(); index += 4u)
    {
        const auto& quad = snapshot->vertices[index];
        const auto& opposite = snapshot->vertices[index + 2u];
        const auto left = quad.position[0];
        const auto top = quad.position[1];
        const auto width = opposite.position[0] - left;
        const auto height = opposite.position[1] - top;
        if (width <= 0.0f || height <= 0.0f)
            continue;

        for (int32_t y = 0; y < static_cast<int32_t>(std::ceil(height)); ++y)
        {
            for (int32_t x = 0; x < static_cast<int32_t>(std::ceil(width)); ++x)
            {
                const auto fx = (static_cast<float>(x) + 0.5f) / width;
                const auto fy = (static_cast<float>(y) + 0.5f) / height;
                const auto u = quad.uv[0] + (opposite.uv[0] - quad.uv[0]) * fx;
                const auto v = quad.uv[1] + (opposite.uv[1] - quad.uv[1]) * fy;
                const auto texelX = (std::min)(atlasImage.width - 1u,
                    static_cast<uint32_t>(u * static_cast<float>(atlasImage.width)));
                const auto texelY = (std::min)(atlasImage.height - 1u,
                    static_cast<uint32_t>(v * static_cast<float>(atlasImage.height)));
                const auto alpha = atlasImage.rgba[
                    (static_cast<size_t>(texelY) * atlasImage.width + texelX) * 4u + 3u];
                const auto pixelX = static_cast<int32_t>(left) + x;
                const auto pixelY = static_cast<int32_t>(top) + y;
                if (pixelX < 0 || pixelY < 0 ||
                    pixelX >= static_cast<int32_t>(canvasWidth) ||
                    pixelY >= static_cast<int32_t>(canvasHeight))
                    continue;
                const auto offset =
                    (static_cast<size_t>(pixelY) * canvasWidth + static_cast<uint32_t>(pixelX)) * 4u;
                canvas[offset + 0u] = 255u;
                canvas[offset + 1u] = 255u;
                canvas[offset + 2u] = 255u;
                canvas[offset + 3u] = alpha;
            }
        }
    }

    size_t inkPixels = 0u;
    for (size_t offset = 3u; offset < canvas.size(); offset += 4u)
        if (canvas[offset] != 0u)
            ++inkPixels;
    EXPECT_GT(inkPixels, 40u) << "glyph quads must sample real coverage";

    // A readable glyph covers its quad. Sampling the padded atlas cell instead of
    // the bitmap left roughly two thirds of every quad empty, which is what made
    // text render as smudges.
    size_t measuredQuads = 0u;
    for (size_t index = 0u; index + 4u <= snapshot->vertices.size(); index += 4u)
    {
        const auto& quad = snapshot->vertices[index];
        const auto& opposite = snapshot->vertices[index + 2u];
        const auto left = static_cast<int32_t>(quad.position[0]);
        const auto top = static_cast<int32_t>(quad.position[1]);
        const auto right = static_cast<int32_t>(opposite.position[0]);
        const auto bottom = static_cast<int32_t>(opposite.position[1]);
        if (right - left <= 0 || bottom - top <= 0)
            continue;

        int32_t minimumX = right;
        int32_t maximumX = left;
        int32_t minimumY = bottom;
        int32_t maximumY = top;
        bool hasInk = false;
        for (int32_t y = top; y < bottom; ++y)
        {
            if (y < 0 || y >= static_cast<int32_t>(canvasHeight))
                continue;
            for (int32_t x = left; x < right; ++x)
            {
                if (x < 0 || x >= static_cast<int32_t>(canvasWidth))
                    continue;
                const auto alpha = canvas[
                    (static_cast<size_t>(y) * canvasWidth + static_cast<uint32_t>(x)) * 4u + 3u];
                if (alpha == 0u)
                    continue;
                hasInk = true;
                minimumX = (std::min)(minimumX, x);
                maximumX = (std::max)(maximumX, x);
                minimumY = (std::min)(minimumY, y);
                maximumY = (std::max)(maximumY, y);
            }
        }
        if (!hasInk)
            continue;

        ++measuredQuads;
        EXPECT_GE(
            static_cast<float>(maximumX - minimumX + 1) / static_cast<float>(right - left),
            0.7f)
            << "glyph ink must span its quad horizontally";
        EXPECT_GE(
            static_cast<float>(maximumY - minimumY + 1) / static_cast<float>(bottom - top),
            0.7f)
            << "glyph ink must span its quad vertically";
    }
    EXPECT_GE(measuredQuads, 6u) << "Score: 0 must produce at least six sized glyph quads";

    // Minimal 24-bit BMP writer so the preview can be inspected without pulling a
    // second image dependency into the test target.
    const auto rowBytes = ((canvasWidth * 3u) + 3u) & ~3u;
    const auto imageBytes = rowBytes * canvasHeight;
    std::vector<uint8_t> header;
    const auto push16 = [&header](const uint16_t value)
    {
        header.push_back(static_cast<uint8_t>(value & 0xffu));
        header.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
    };
    const auto push32 = [&header](const uint32_t value)
    {
        for (uint32_t shift = 0u; shift < 32u; shift += 8u)
            header.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
    };
    header.push_back('B');
    header.push_back('M');
    push32(54u + imageBytes);
    push16(0u);
    push16(0u);
    push32(54u);
    push32(40u);
    push32(canvasWidth);
    push32(canvasHeight);
    push16(1u);
    push16(24u);
    push32(0u);
    push32(imageBytes);
    push32(2835u);
    push32(2835u);
    push32(0u);
    push32(0u);

    std::vector<uint8_t> file = header;
    file.resize(54u + imageBytes, 0u);
    for (uint32_t y = 0u; y < canvasHeight; ++y)
    {
        const auto sourceRow = canvasHeight - 1u - y;
        for (uint32_t x = 0u; x < canvasWidth; ++x)
        {
            const auto alpha = canvas[(static_cast<size_t>(sourceRow) * canvasWidth + x) * 4u + 3u];
            const auto destination = 54u + static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x) * 3u;
            file[destination + 0u] = alpha;
            file[destination + 1u] = alpha;
            file[destination + 2u] = alpha;
        }
    }
    const auto previewPath = std::filesystem::temp_directory_path() / "nullus_ui_text_preview.bmp";
    std::ofstream preview(previewPath, std::ios::binary | std::ios::trunc);
    preview.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    ASSERT_TRUE(preview.good());

    NLS::Render::UI::ClearUiFontAtlasTextureId();
}

TEST(UIDocumentTests, CanvasElementHandlesRejectReloadedOrForeignElements)
{
    constexpr const char* source = R"json({
        "version": 1,
        "root": { "type": "Panel", "name": "Root", "children": [
            { "type": "TextField", "name": "Name", "text": "old" }
        ] }
    })json";
    NLS::Engine::Components::UICanvasComponent canvas;
    ASSERT_TRUE(canvas.LoadDocumentJson(source));
    const auto handle = canvas.GetElementHandle("Name");
    ASSERT_NE(handle, 0u);
    EXPECT_TRUE(canvas.SetElementText(handle, "new"));
    EXPECT_EQ(canvas.GetElementText(handle), "new");
    EXPECT_GT(canvas.GetDocumentGeneration(), 0u);

    ASSERT_TRUE(canvas.LoadDocumentJson(source));
    EXPECT_FALSE(canvas.SetElementText(handle, "stale"));
    EXPECT_TRUE(canvas.GetElementText(handle).empty());

    const auto reloadedHandle = canvas.GetElementHandle("Name");
    ASSERT_NE(reloadedHandle, 0u);
    EXPECT_NE(reloadedHandle, handle);
    EXPECT_TRUE(canvas.SetElementText(reloadedHandle, "fresh"));
    EXPECT_EQ(canvas.GetElementText(reloadedHandle), "fresh");
}

TEST(UIDocumentTests, ImageDocumentsCarryGenerationCheckedTextureReferences)
{
    const auto document = UIDocument::LoadFromJson(R"json({
        "version": 1,
        "root": { "type": "Image", "texture": { "id": 77, "generation": 4 },
            "layout": { "size": [32, 32] } }
    })json");
    ASSERT_TRUE(document.has_value());
    auto instance = document->Instantiate();
    instance->UpdateLayout({ 64.0f, 64.0f });
    const auto snapshot = instance->BuildDrawSnapshot(3u, 64u, 64u);
    ASSERT_NE(snapshot, nullptr);
    ASSERT_EQ(snapshot->batches.size(), 1u);
    EXPECT_EQ(snapshot->batches.front().textureId, 77u);
    EXPECT_EQ(snapshot->batches.front().textureGeneration, 4u);

    const auto adapted = NLS::Render::UI::AdaptUIDrawSnapshot(*snapshot);
    ASSERT_NE(adapted, nullptr);
    ASSERT_EQ(adapted->drawLists.front().commands.size(), 1u);
    EXPECT_EQ(adapted->drawLists.front().commands.front().textureId.value, 77u);
    EXPECT_EQ(adapted->drawLists.front().commands.front().textureId.generation, 4u);
}

TEST(UIDocumentTests, ParsesClassAliasesUsesFontSizeAndStableSourceHash)
{
    const std::string source = R"json({
        "version": 1,
        "root": {
            "type": "Text",
            "name": "Root",
            "class": "RootClass",
            "fontAssetId": 9,
            "fontPixelSize": 24,
            "text": "Ready",
            "children": [
                { "type": "Panel", "name": "Child", "className": "ChildClass" }
            ]
        }
    })json";

    const auto first = UIDocument::LoadFromJson(source);
    const auto second = UIDocument::LoadFromJson(source);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(first->GetSourceHash().empty());
    EXPECT_EQ(first->GetSourceHash(), second->GetSourceHash());

    auto instance = first->Instantiate();
    ASSERT_NE(instance, nullptr);
    ASSERT_NE(instance->GetRoot(), nullptr);
    EXPECT_EQ(instance->GetRoot()->GetClassName(), "RootClass");
    ASSERT_NE(instance->GetElementByName("Child"), nullptr);
    EXPECT_EQ(instance->GetElementByName("Child")->GetClassName(), "ChildClass");

    instance->UpdateLayout({ 320.0f, 80.0f });
    const auto snapshot = instance->BuildDrawSnapshot(5u, 320u, 80u);
    ASSERT_NE(snapshot, nullptr);
    ASSERT_EQ(snapshot->textRuns.size(), 1u);
    EXPECT_FLOAT_EQ(snapshot->textRuns.front().fontSize, 24.0f);
    EXPECT_EQ(snapshot->textRuns.front().fontPixelSize, 24u);
    EXPECT_EQ(snapshot->textRuns.front().clipIndex, NLS::Render::UI::UIClipIndexNone);
}

TEST(UIDocumentTests, TextureRegistryRetiresAfterFrameFence)
{
    NLS::Render::UI::UITextureRegistry registry;
    const auto handle = registry.Register(1234u, NLS::Render::UI::UITextureKind::Image);
    ASSERT_TRUE(handle.IsValid());
    EXPECT_TRUE(registry.MarkUsed(handle, 8u));
    EXPECT_TRUE(registry.IsLive(handle, 8u));
    EXPECT_TRUE(registry.Retire(handle, 10u));
    EXPECT_TRUE(registry.IsLive(handle, 9u));
    EXPECT_FALSE(registry.IsLive(handle, 11u));
    registry.CollectRetired(10u);
    EXPECT_FALSE(registry.Resolve(handle).has_value());
}

TEST(UIDocumentTests, FontAtlasRequestsCjkAndReportsFallbackGlyphs)
{
    NLS::Render::UI::UIFontAtlas atlas;
    atlas.Configure(128u, 128u, 2u);
    EXPECT_EQ(atlas.EnsureText(11u, "A\xE4\xB8\xAD", 16u, 1u, true), 2u);
    EXPECT_EQ(atlas.EnsureText(11u, "A", 16u, 2u, true), 1u);
    NLS::Render::UI::UIFontGlyph fallback;
    ASSERT_TRUE(atlas.EnsureGlyph({ 12u, 0x4e2du, 16u }, 3u, fallback, false));
    EXPECT_TRUE(fallback.fallback);
    EXPECT_GT(atlas.GetDiagnostics().fallbackGlyphs, 0u);
}

TEST(UIDocumentTests, FontAtlasRasterizesRealGlyphsFromTrueTypeData)
{
    const std::filesystem::path relativeFont = "Assets/Engine/Fonts/Ruda-Bold.ttf";
    std::filesystem::path fontPath;
    auto searchRoot = std::filesystem::current_path();
    for (int depth = 0; depth < 6 && fontPath.empty(); ++depth)
    {
        const std::filesystem::path candidates[] = {
            searchRoot / "App" / relativeFont,
            searchRoot / relativeFont,
            searchRoot / "build" / "bin" / "Debug" / relativeFont,
            searchRoot / "App" / "Win64_Debug_Runtime_Shared" / relativeFont};
        for (const auto& candidate : candidates)
        {
            if (std::filesystem::is_regular_file(candidate))
            {
                fontPath = candidate;
                break;
            }
        }
        searchRoot = searchRoot.parent_path();
    }

    NLS::Render::UI::UIFontAtlas atlas;
    atlas.Configure(256u, 256u, 2u);
    ASSERT_FALSE(fontPath.empty()) << "engine font not found from cwd " << std::filesystem::current_path().string();
    ASSERT_TRUE(atlas.LoadFontFromFile(fontPath));
    EXPECT_TRUE(atlas.HasFontData());

    NLS::Render::UI::UIFontGlyph glyphA;
    ASSERT_TRUE(atlas.EnsureGlyph({ 21u, 'A', 24u }, 1u, glyphA, true));
    EXPECT_FALSE(glyphA.fallback);
    EXPECT_GT(glyphA.advance, 0.0f);
    EXPECT_GE(glyphA.uv[0], 0.0f);
    EXPECT_LE(glyphA.uv[2], 1.0f);

    NLS::Render::UI::UIFontGlyph glyphB;
    ASSERT_TRUE(atlas.EnsureGlyph({ 21u, 'B', 24u }, 1u, glyphB, true));
    EXPECT_FALSE(glyphB.fallback);
    EXPECT_NE(glyphA.uv[0], glyphB.uv[0]);

    ASSERT_FALSE(atlas.GetPages().empty());
    const auto& pixels = atlas.GetPages().front().pixels;
    ASSERT_EQ(pixels.size(), 256u * 256u * 4u);
    EXPECT_TRUE(std::any_of(pixels.begin(), pixels.end(), [](const uint8_t value) { return value != 0u; }))
        << "rasterized glyph coverage must reach the atlas page pixels";

    // A codepoint the font does not provide keeps an observable tofu cell.
    NLS::Render::UI::UIFontGlyph tofu;
    ASSERT_TRUE(atlas.EnsureGlyph({ 21u, 0x10ffffu, 24u }, 1u, tofu, true));
    EXPECT_TRUE(tofu.fallback);
}

TEST(UIDocumentTests, FontAtlasImageStacksPagesAndMapsPageUvs)
{
    NLS::Render::UI::UIFontAtlas atlas;
    // A large glyph on a tiny page fills the whole page, so the second glyph
    // lands on page 1 and exercises the stacked atlas layout.
    atlas.Configure(64u, 64u, 2u);
    NLS::Render::UI::UIFontGlyph first;
    ASSERT_TRUE(atlas.EnsureGlyph({ 31u, 'A', 48u }, 1u, first, true));
    NLS::Render::UI::UIFontGlyph second;
    ASSERT_TRUE(atlas.EnsureGlyph({ 31u, 'B', 48u }, 1u, second, true));
    ASSERT_EQ(atlas.GetPages().size(), 2u);
    EXPECT_EQ(first.pageIndex, 0u);
    EXPECT_EQ(second.pageIndex, 1u);

    const auto image = NLS::Render::UI::BuildUIFontAtlasImage(atlas);
    EXPECT_EQ(image.width, 64u);
    EXPECT_EQ(image.height, 128u);
    EXPECT_EQ(image.pageCount, 2u);
    ASSERT_EQ(image.rgba.size(), 64u * 128u * 4u);

    const auto halfBytes = image.rgba.begin() + static_cast<std::ptrdiff_t>(64u * 64u * 4u);
    EXPECT_TRUE(std::any_of(image.rgba.begin(), halfBytes, [](const uint8_t value) { return value != 0u; }))
        << "page 0 pixels must be present in the stacked image";
    EXPECT_TRUE(std::any_of(halfBytes, image.rgba.end(), [](const uint8_t value) { return value != 0u; }))
        << "page 1 pixels must be present in the stacked image";

    const auto pageZeroOrigin = NLS::Render::UI::MapUIFontAtlasPageUv(atlas, 0u, 0.0f, 0.0f);
    const auto pageOneOrigin = NLS::Render::UI::MapUIFontAtlasPageUv(atlas, 1u, 0.0f, 0.0f);
    const auto pageOneBottom = NLS::Render::UI::MapUIFontAtlasPageUv(atlas, 1u, 1.0f, 1.0f);
    EXPECT_FLOAT_EQ(pageZeroOrigin[1], 0.0f);
    EXPECT_FLOAT_EQ(pageOneOrigin[1], 0.5f);
    EXPECT_FLOAT_EQ(pageOneBottom[1], 1.0f);
    EXPECT_FLOAT_EQ(pageOneBottom[0], 1.0f);

    // A third glyph only fits through page eviction, and both eviction and the
    // new glyph must advance the generation so uploaders notice changed pixels.
    const auto generationAfterGlyphs = atlas.GetContentGeneration();
    atlas.SetCompletedFrame(1u);
    NLS::Render::UI::UIFontGlyph third;
    ASSERT_TRUE(atlas.EnsureGlyph({ 31u, 'C', 16u }, 2u, third, true));
    EXPECT_GT(atlas.GetContentGeneration(), generationAfterGlyphs);
}

TEST(UIDocumentTests, LocalizationTableFormatsWhitelistedArguments)
{
    const auto table = UILocalizationTable::LoadFromJson(R"json({
        "version": 1,
        "locale": "zh-CN",
        "entries": { "greeting": "你好，{name}!" }
    })json");
    ASSERT_TRUE(table.has_value());
    EXPECT_EQ(table->GetLocale(), "zh-CN");
    EXPECT_EQ(table->Format("greeting", {{ "name", "玩家" }}), "你好，玩家!");
    EXPECT_EQ(table->Format("missing"), "missing");
}

TEST(UIDocumentTests, NamedSignalsUseStableSubscriptionsAndCopyOnEmit)
{
    UISignalBus bus;
    uint32_t calls = 0u;
    const auto handle = bus.Connect("Submit", [&calls, &bus](const std::string&, const std::vector<UIValue>&) {
        ++calls;
        bus.Clear();
    });
    ASSERT_TRUE(handle.IsValid());
    EXPECT_EQ(bus.Emit("Submit"), 1u);
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(bus.Emit("Submit"), 0u);
}

TEST(UIDocumentTests, TextFieldSupportsSelectionImeCommitAndUndo)
{
    UIElement textField(UIElementType::TextField, "Input");
    textField.SetText("abc");
    textField.SetTextSelection(1u, 2u);

    UIEvent input;
    input.type = UIEventType::TextInput;
    input.text = "X";
    textField.PerformDefaultAction(input);
    EXPECT_EQ(textField.GetText(), "aXc");

    UIEvent ime;
    ime.type = UIEventType::ImeCompositionUpdate;
    ime.text = "中";
    textField.PerformDefaultAction(ime);
    EXPECT_EQ(textField.GetImeComposition(), "中");
    ime.type = UIEventType::ImeCompositionCommit;
    textField.PerformDefaultAction(ime);
    EXPECT_EQ(textField.GetText(), "aX中c");

    UIEvent undo;
    undo.type = UIEventType::KeyDown;
    undo.keyCode = 90u;
    undo.modifiers = UIInputModifierControl;
    textField.PerformDefaultAction(undo);
    EXPECT_EQ(textField.GetText(), "aXc");
}

TEST(UIDocumentTests, DataBindingSupportsOneWayAndTwoWayCommit)
{
    UIDataContext context;
    context.Set("label", std::string("initial"));
    UIElement label(UIElementType::TextField, "Label");
    UIBindingEngine bindings;
    const auto handle = bindings.Bind(
        context,
        label,
        "label",
        UIBindingProperty::Text,
        UIBindingMode::TwoWay,
        UIBindingUpdateStrategy::Commit);
    ASSERT_TRUE(handle.IsValid());
    EXPECT_EQ(label.GetText(), "initial");
    context.Set("label", std::string("updated"));
    EXPECT_EQ(label.GetText(), "updated");

    label.SetText("committed");
    UIEvent submit;
    submit.type = UIEventType::KeyDown;
    submit.keyCode = 13u;
    label.PerformDefaultAction(submit);
    ASSERT_NE(context.Get("label"), nullptr);
    EXPECT_EQ(std::get<std::string>(*context.Get("label")), "committed");
}

TEST(UIDocumentTests, AssetDatabaseImportsUiArtifactsAndCanvasReloadsFromArtifact)
{
    using NLS::Core::Assets::ArtifactType;
    using NLS::Editor::Assets::AssetDatabaseFacade;

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_ui_asset_pipeline_" + NLS::Guid::New().ToString());
    const auto documentPath = root / "Assets" / "HUD.ui";
    const auto stylePath = root / "Assets" / "Theme.style";
    const auto localizationPath = root / "Assets" / "Strings.loc";
    std::filesystem::create_directories(documentPath.parent_path());

    const auto write = [](const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << text;
        return output.good();
    };

    const std::string initialDocument = R"json({
        "version": 1,
        "documentId": "22222222-2222-4222-8222-222222222222",
        "root": { "type": "TextField", "name": "Input", "text": "source", "layout": { "size": [160, 24] } }
    })json";
    ASSERT_TRUE(write(documentPath, initialDocument));
    ASSERT_TRUE(write(stylePath, R"json({
        "version": 1,
        "rules": [{ "selector": { "type": "Button" }, "style": { "opacity": 0.75 } }]
    })json"));
    ASSERT_TRUE(write(localizationPath, R"json({
        "version": 1,
        "locale": "en-US",
        "entries": { "ready": "Ready" }
    })json"));

    AssetDatabaseFacade database({root});
    ASSERT_TRUE(database.Refresh());
    ASSERT_TRUE(database.ImportAsset("Assets/HUD.ui"));
    ASSERT_TRUE(database.ImportAsset("Assets/Theme.style"));
    ASSERT_TRUE(database.ImportAsset("Assets/Strings.loc"));

    const auto documentManifest = database.GetArtifactManifestForAssetPath("Assets/HUD.ui");
    ASSERT_TRUE(documentManifest.has_value());
    ASSERT_EQ(documentManifest->subAssets.size(), 1u);
    ASSERT_EQ(documentManifest->primarySubAssetKey, "ui-document:HUD");
    ASSERT_EQ(documentManifest->FindPrimaryArtifact()->artifactType, ArtifactType::UIDocument);
    const auto documentArtifactPath = database.ResolveArtifactPathAtPath(
        "Assets/HUD.ui", documentManifest->primarySubAssetKey);
    ASSERT_TRUE(std::filesystem::is_regular_file(documentArtifactPath));

    std::vector<UIDocumentDiagnostic> documentDiagnostics;
    const auto loadedDocument = UIDocument::LoadFromArtifactFile(documentArtifactPath, &documentDiagnostics);
    ASSERT_TRUE(loadedDocument.has_value());
    EXPECT_TRUE(documentDiagnostics.empty());
    ASSERT_NE(loadedDocument->Instantiate()->GetElementByName("Input"), nullptr);

    const auto styleManifest = database.GetArtifactManifestForAssetPath("Assets/Theme.style");
    ASSERT_TRUE(styleManifest.has_value());
    ASSERT_EQ(styleManifest->FindPrimaryArtifact()->artifactType, ArtifactType::UIStyleSheet);
    const auto styleArtifactPath = database.ResolveArtifactPathAtPath(
        "Assets/Theme.style", styleManifest->primarySubAssetKey);
    ASSERT_TRUE(UIStyleSheet::LoadFromArtifactFile(styleArtifactPath).has_value());

    const auto localizationManifest = database.GetArtifactManifestForAssetPath("Assets/Strings.loc");
    ASSERT_TRUE(localizationManifest.has_value());
    ASSERT_EQ(localizationManifest->FindPrimaryArtifact()->artifactType, ArtifactType::LocalizationTable);
    const auto localizationArtifactPath = database.ResolveArtifactPathAtPath(
        "Assets/Strings.loc", localizationManifest->primarySubAssetKey);
    const auto loadedLocalization = UILocalizationTable::LoadFromArtifactFile(localizationArtifactPath);
    ASSERT_TRUE(loadedLocalization.has_value());
    EXPECT_EQ(loadedLocalization->Format("ready"), "Ready");

    NLS::Engine::Components::UICanvasComponent canvas;
    ASSERT_TRUE(canvas.LoadDocumentArtifact(documentArtifactPath.string()));
    const auto initialHandle = canvas.GetElementHandle("Input");
    ASSERT_NE(initialHandle, 0u);
    ASSERT_TRUE(canvas.SetElementText(initialHandle, "typed"));

    ASSERT_TRUE(write(documentPath, R"json({
        "version": 1,
        "documentId": "22222222-2222-4222-8222-222222222222",
        "root": { "type": "TextField", "name": "Input", "text": "updated", "layout": { "size": [160, 24] } }
    })json"));
    ASSERT_TRUE(database.ReimportAsset("Assets/HUD.ui"));

    const auto updatedManifest = database.GetArtifactManifestForAssetPath("Assets/HUD.ui");
    ASSERT_TRUE(updatedManifest.has_value());
    const auto updatedArtifactPath = database.ResolveArtifactPathAtPath(
        "Assets/HUD.ui", updatedManifest->primarySubAssetKey);
    const auto updatedDocument = UIDocument::LoadFromArtifactFile(updatedArtifactPath);
    ASSERT_TRUE(updatedDocument.has_value());
    ASSERT_EQ(updatedDocument->Instantiate()->GetElementByName("Input")->GetText(), "updated");

    ASSERT_TRUE(canvas.LoadDocumentArtifact(updatedArtifactPath.string()));
    const auto reloadedHandle = canvas.GetElementHandle("Input");
    ASSERT_NE(reloadedHandle, 0u);
    EXPECT_NE(reloadedHandle, initialHandle);
    EXPECT_EQ(canvas.GetElementText(reloadedHandle), "typed");

    AssetDatabaseFacade restarted({root});
    ASSERT_TRUE(restarted.Refresh());
    const auto persistedManifest = restarted.GetArtifactManifestForAssetPath("Assets/HUD.ui");
    ASSERT_TRUE(persistedManifest.has_value());
    EXPECT_EQ(persistedManifest->FindPrimaryArtifact()->artifactType, ArtifactType::UIDocument);
    EXPECT_TRUE(restarted.IsArtifactManifestCurrentForAssetPath("Assets/HUD.ui"));

    std::filesystem::remove_all(root);
}

TEST(UIDocumentTests, CanvasResolvesProjectRelativeDocumentReferencesThroughHostResolver)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_ui_document_reference_" + NLS::Guid::New().ToString());
    const auto documentPath = root / "Assets" / "UI" / "Hud.ui";
    std::filesystem::create_directories(documentPath.parent_path());

    {
        std::ofstream output(documentPath, std::ios::binary | std::ios::trunc);
        output << R"json({
            "version": 1,
            "documentId": "33333333-3333-4333-8333-333333333333",
            "root": { "type": "Text", "name": "Score", "text": "Score: 0", "layout": { "size": [160, 24] } }
        })json";
        ASSERT_TRUE(output.good());
    }

    // A scene stores the logical "Assets/..." reference. The host resolver maps
    // it onto the project without rewriting what the scene serializes.
    SetUIDocumentSourcePathResolver(
        [assetsPath = (root / "Assets").generic_string()](const UIDocumentReference& reference)
        {
            return ResolveProjectUIDocumentPath(assetsPath, reference.GetResourcePath());
        });

    NLS::Engine::Components::UICanvasComponent canvas;
    canvas.SetDocumentReference(UIDocumentReference({}, "Assets/UI/Hud.ui"));

    EXPECT_NE(canvas.GetDocument(), nullptr);
    EXPECT_EQ(canvas.GetDocumentPath(), "Assets/UI/Hud.ui");
    EXPECT_NE(canvas.GetElementHandle("Score"), 0u);

    // An unresolvable reference must keep the stable reference while leaving the
    // canvas without a live document instead of failing the assignment.
    canvas.SetDocumentReference(UIDocumentReference({}, "Assets/UI/Missing.ui"));
    EXPECT_EQ(canvas.GetDocument(), nullptr);
    EXPECT_EQ(canvas.GetDocumentPath(), "Assets/UI/Missing.ui");

    SetUIDocumentSourcePathResolver({});
    std::filesystem::remove_all(root);
}

TEST(UIDocumentTests, ProjectDocumentResolutionHandlesHostAssetsPathsWithTrailingSeparator)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_ui_trailing_separator_" + NLS::Guid::New().ToString());
    const auto documentPath = root / "Assets" / "UI" / "Hud.ui";
    std::filesystem::create_directories(documentPath.parent_path());
    {
        std::ofstream output(documentPath, std::ios::binary | std::ios::trunc);
        output << R"json({"version":1,"root":{"type":"Text","name":"Score"}})json";
        ASSERT_TRUE(output.good());
    }

    // The editor hands the resolver its project assets folder with a trailing
    // separator (Context::projectAssetsPath). A reference stored as
    // "Assets/..." must still map onto the project folder instead of nesting a
    // second Assets directory below it.
    const auto canonical = (root / "Assets").generic_string();
    const auto withSeparator = canonical + "/";
    EXPECT_FALSE(ResolveProjectUIDocumentPath(withSeparator, "Assets/UI/Hud.ui").empty());
    EXPECT_FALSE(ResolveProjectUIDocumentPath(withSeparator, "UI/Hud.ui").empty());
    EXPECT_EQ(
        ResolveProjectUIDocumentPath(withSeparator, "Assets/UI/Hud.ui"),
        ResolveProjectUIDocumentPath(canonical, "Assets/UI/Hud.ui"));
    EXPECT_EQ(
        ResolveProjectUIDocumentPath(withSeparator, "UI/Hud.ui"),
        ResolveProjectUIDocumentPath(canonical, "UI/Hud.ui"));

    std::filesystem::remove_all(root);
}

TEST(UIDocumentTests, NestedPaintChangesInvalidateRetainedSnapshot)
{
    const auto document = UIDocument::LoadFromJson(R"json({
        "version": 1,
        "root": { "type": "Panel", "layout": { "size": [320, 80] }, "children": [
            { "type": "Panel", "children": [
                { "type": "Text", "name": "Label", "text": "Before" }
            ] }
        ] }
    })json");
    ASSERT_TRUE(document.has_value());

    auto instance = document->Instantiate();
    ASSERT_NE(instance, nullptr);
    instance->UpdateLayout({ 320.0f, 80.0f });
    const auto before = instance->BuildDrawSnapshot(1u, 320u, 80u);
    ASSERT_NE(before, nullptr);
    ASSERT_EQ(before->textRuns.size(), 1u);

    auto* label = instance->GetElementByName("Label");
    ASSERT_NE(label, nullptr);
    label->SetText("After");
    const auto after = instance->BuildDrawSnapshot(2u, 320u, 80u);
    ASSERT_NE(after, nullptr);
    ASSERT_EQ(after->textRuns.size(), 1u);
    EXPECT_EQ(after->textRuns.front().text, "After");
    EXPECT_NE(before->textRuns.front().text, after->textRuns.front().text);
}

TEST(UIDocumentTests, StopPropagationDoesNotSkipToggleDefaultAction)
{
    auto root = std::make_unique<UIElement>(UIElementType::Panel, "Root");
    UILayoutParams rootLayout;
    rootLayout.mode = UILayoutMode::Overlay;
    rootLayout.size = { 100.0f, 100.0f };
    root->SetLayout(rootLayout);
    auto toggle = std::make_unique<UIElement>(UIElementType::Toggle, "Toggle");
    UILayoutParams toggleLayout;
    toggleLayout.size = { 30.0f, 30.0f };
    toggle->SetLayout(toggleLayout);
    auto& toggleReference = root->AddChild(std::move(toggle));
    root->Measure({ 100.0f, 100.0f });
    root->Arrange({ 0.0f, 0.0f, 100.0f, 100.0f });

    toggleReference.AddEventListener(UIEventType::Click, [](UIEvent& event) {
        event.StopPropagation();
    });

    UIInputRouter router;
    router.SetRoot(root.get());
    ASSERT_TRUE(router.DispatchPointer(UIEventType::PointerDown, { 5.0f, 5.0f }));
    ASSERT_TRUE(router.DispatchPointer(UIEventType::PointerUp, { 5.0f, 5.0f }));
    EXPECT_TRUE(toggleReference.IsChecked());
}

TEST(UIDocumentTests, SpatialFocusNavigationPrefersRequestedDirection)
{
    auto root = std::make_unique<UIElement>(UIElementType::Panel, "Root");
    UILayoutParams rootLayout;
    rootLayout.mode = UILayoutMode::Overlay;
    rootLayout.size = { 200.0f, 100.0f };
    root->SetLayout(rootLayout);

    auto left = std::make_unique<UIElement>(UIElementType::Button, "Left");
    UILayoutParams leftLayout;
    leftLayout.size = { 20.0f, 20.0f };
    leftLayout.offsetMin = { 10.0f, 10.0f };
    left->SetLayout(leftLayout);
    auto& leftReference = root->AddChild(std::move(left));

    auto right = std::make_unique<UIElement>(UIElementType::Button, "Right");
    UILayoutParams rightLayout;
    rightLayout.size = { 20.0f, 20.0f };
    rightLayout.offsetMin = { 100.0f, 10.0f };
    right->SetLayout(rightLayout);
    auto& rightReference = root->AddChild(std::move(right));

    root->Measure({ 200.0f, 100.0f });
    root->Arrange({ 0.0f, 0.0f, 200.0f, 100.0f });
    UIInputRouter router;
    router.SetRoot(root.get());
    router.Focus(&leftReference);
    EXPECT_TRUE(router.Navigate(UIFocusDirection::Right));
    EXPECT_EQ(router.GetFocusedElement(), &rightReference);
    EXPECT_TRUE(router.Navigate(UIFocusDirection::Left));
    EXPECT_EQ(router.GetFocusedElement(), &leftReference);
}

TEST(UIDocumentTests, ReloadPreservesNodeStateAndRebindsFocus)
{
    constexpr const char* source = R"json({
        "version": 1,
        "root": { "type": "ScrollView", "name": "Scroll", "layout": { "size": [200, 100] }, "children": [
            { "type": "TextField", "name": "Input", "text": "default", "layout": { "size": [100, 24] } },
            { "type": "Slider", "name": "Volume", "layout": { "size": [100, 20] } }
        ] }
    })json";
    NLS::Engine::Components::UICanvasComponent canvas;
    ASSERT_TRUE(canvas.LoadDocumentJson(source));
    auto* instance = canvas.GetInstance();
    ASSERT_NE(instance, nullptr);
    instance->UpdateLayout({ 200.0f, 100.0f });
    auto* scroll = instance->GetElementByName("Scroll");
    auto* input = instance->GetElementByName("Input");
    auto* volume = instance->GetElementByName("Volume");
    ASSERT_NE(scroll, nullptr);
    ASSERT_NE(input, nullptr);
    ASSERT_NE(volume, nullptr);
    scroll->SetScrollOffset({ 0.0f, 17.0f });
    input->SetText("typed");
    volume->SetValue(0.75f);
    ASSERT_TRUE(canvas.FocusElement(canvas.GetElementHandle("Input")));
    const auto oldGeneration = canvas.GetDocumentGeneration();

    ASSERT_TRUE(canvas.LoadDocumentJson(source));
    EXPECT_GT(canvas.GetDocumentGeneration(), oldGeneration);
    const auto newInputHandle = canvas.GetElementHandle("Input");
    ASSERT_NE(newInputHandle, 0u);
    EXPECT_EQ(canvas.GetElementText(newInputHandle), "typed");
    auto* newScroll = canvas.GetInstance()->GetElementByName("Scroll");
    auto* newVolume = canvas.GetInstance()->GetElementByName("Volume");
    ASSERT_NE(newScroll, nullptr);
    ASSERT_NE(newVolume, nullptr);
    EXPECT_FLOAT_EQ(newScroll->GetScrollOffset().y, 17.0f);
    EXPECT_FLOAT_EQ(newVolume->GetValue(), 0.75f);
    EXPECT_EQ(canvas.GetInputRouter().GetFocusedElement(), canvas.GetInstance()->GetElementByName("Input"));
}

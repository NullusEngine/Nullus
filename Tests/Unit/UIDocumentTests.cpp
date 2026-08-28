#include <gtest/gtest.h>

#include "Assets/AssetDatabaseFacade.h"
#include "Assets/AssetMeta.h"
#include "Engine/UI/UIDataContext.h"
#include "Engine/UI/UIDocument.h"
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

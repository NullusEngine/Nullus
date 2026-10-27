#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include <Reflection/Type.h>

#include <Inspector/PropertyTypeTraits.h>

#include "UI/UIDocumentDesignerModel.h"
#include "UI/UIDocumentNodeEdit.h"
#include "UIDocumentDesignerTestSupport.h"

namespace
{
    using namespace NLS::Editor::UIToolsTest;
    using namespace NLS::Editor::UIDocumentTools;

    bool CoversField(
        const std::vector<UIDocumentEditField>& fields,
        const std::string& name)
    {
        return std::any_of(
            fields.begin(),
            fields.end(),
            [&name](const UIDocumentEditField& entry) { return name == entry.field; });
    }
}

TEST(UIDocumentNodeEdit, ReflectionRegistersEverySectionField)
{
    EnsureUIDocumentInspectorReflection();

    EXPECT_EQ(NLS_TYPEOF(UIDocumentElementEdit).GetFields().size(), 2u);
    EXPECT_EQ(NLS_TYPEOF(UIDocumentRectEdit).GetFields().size(), 9u);
    EXPECT_EQ(NLS_TYPEOF(UIDocumentLayoutEdit).GetFields().size(), 9u);
    EXPECT_EQ(NLS_TYPEOF(UIDocumentStyleEdit).GetFields().size(), 6u);
    EXPECT_EQ(NLS_TYPEOF(UIDocumentContentEdit).GetFields().size(), 8u);

    const auto& fields = GetUIDocumentEditFields();
    for (const auto& field : NLS_TYPEOF(UIDocumentElementEdit).GetFields())
        EXPECT_TRUE(CoversField(fields, field.GetName())) << field.GetName();
    for (const auto& field : NLS_TYPEOF(UIDocumentRectEdit).GetFields())
        EXPECT_TRUE(CoversField(fields, field.GetName())) << field.GetName();
    for (const auto& field : NLS_TYPEOF(UIDocumentLayoutEdit).GetFields())
        EXPECT_TRUE(CoversField(fields, field.GetName())) << field.GetName();
    for (const auto& field : NLS_TYPEOF(UIDocumentStyleEdit).GetFields())
        EXPECT_TRUE(CoversField(fields, field.GetName())) << field.GetName();
    for (const auto& field : NLS_TYPEOF(UIDocumentContentEdit).GetFields())
        EXPECT_TRUE(CoversField(fields, field.GetName())) << field.GetName();
}

TEST(UIDocumentNodeEdit, EveryMappedPathIsAnEditableDocumentProperty)
{
    EnsureUIDocumentInspectorReflection();
    for (const auto& field : GetUIDocumentEditFields())
    {
        EXPECT_TRUE(NLS::Editor::UIDocumentTools::UIDocumentDesignerModel::
                IsSupportedPropertyPath(field.jsonPath))
            << field.field << " -> " << field.jsonPath;
    }
}

TEST(UIDocumentNodeEdit, EnumMappingsRoundTrip)
{
    for (const auto mode : {
             UIDocumentLayoutModeEdit::Absolute,
             UIDocumentLayoutModeEdit::Stack,
             UIDocumentLayoutModeEdit::Flex,
             UIDocumentLayoutModeEdit::Grid,
             UIDocumentLayoutModeEdit::Overlay,
             UIDocumentLayoutModeEdit::Scroll })
    {
        EXPECT_EQ(LayoutModeFromJson(ToJson(mode)), mode);
    }
    for (const auto direction : {
             UIDocumentDirectionEdit::Vertical,
             UIDocumentDirectionEdit::Horizontal })
    {
        EXPECT_EQ(DirectionFromJson(ToJson(direction)), direction);
    }
    for (const auto align : {
             UIDocumentAlignEdit::Start,
             UIDocumentAlignEdit::Center,
             UIDocumentAlignEdit::End,
             UIDocumentAlignEdit::Stretch })
    {
        EXPECT_EQ(AlignFromJson(ToJson(align)), align);
    }
}

TEST(UIDocumentNodeEdit, ChangedPathsMatchReflectedPropertySuffixes)
{
    EXPECT_TRUE(UIDocumentEditChanged({ "anchorMin" }, "anchorMin"));
    EXPECT_TRUE(UIDocumentEditChanged({ "Value.layout.anchorMin" }, "anchorMin"));
    EXPECT_FALSE(UIDocumentEditChanged({ "Value.layout.anchorMax" }, "anchorMin"));
    EXPECT_FALSE(UIDocumentEditChanged({}, "anchorMin"));
}

TEST(UIDocumentNodeEdit, AppliesEverySectionToJsonAndUndoRestores)
{
    ScopedDocumentDirectory directory;
    ScopedUndoService undo;

    const auto path = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));
    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(path));
    const auto rootGuid = model.GetRootGuid();
    ASSERT_TRUE(model.AddChild(rootGuid, "Panel"));
    const auto nodeGuid = model.GetSelectedNodeGuid();
    model.SetSelection({ nodeGuid });

    UIDocumentElementEdit element;
    element.name = "Panel A";
    element.className = "hud.panel";
    model.BeginTransaction("Edit Element");
    ApplyElementEdit(model, element, { "name", "className" }, false);
    model.EndTransaction();
    EXPECT_EQ(model.GetNodeProperty(nodeGuid, "name")->get<std::string>(), "Panel A");
    EXPECT_EQ(
        model.GetNodeProperty(nodeGuid, "className")->get<std::string>(),
        "hud.panel");

    UIDocumentRectEdit rect;
    rect.anchorMin = { 0.25f, 0.5f };
    rect.anchorMax = { 0.75f, 0.5f };
    rect.pivot = { 0.5f, 0.5f };
    rect.offsetMin = { 12.0f, 24.0f };
    rect.offsetMax = { -12.0f, 24.0f };
    rect.size = { 320.0f, 64.0f };
    rect.minSize = { 10.0f, 10.0f };
    rect.maxSize = { 800.0f, 200.0f };
    rect.aspectRatio = 2.0f;
    model.BeginTransaction("Edit Rect");
    ApplyRectEdit(
        model,
        rect,
        { "anchorMin", "anchorMax", "pivot", "offsetMin", "offsetMax", "size",
          "minSize", "maxSize", "aspectRatio" },
        false);
    model.EndTransaction();
    EXPECT_FLOAT_EQ(
        model.GetNodeProperty(nodeGuid, "layout.anchorMin")->at(0).get<float>(),
        0.25f);
    EXPECT_FLOAT_EQ(
        model.GetNodeProperty(nodeGuid, "layout.size")->at(1).get<float>(),
        64.0f);
    EXPECT_FLOAT_EQ(
        model.GetNodeProperty(nodeGuid, "layout.aspectRatio")->get<float>(),
        2.0f);

    UIDocumentLayoutEdit layout;
    layout.mode = UIDocumentLayoutModeEdit::Flex;
    layout.direction = UIDocumentDirectionEdit::Horizontal;
    layout.align = UIDocumentAlignEdit::Center;
    layout.spacing = 12.0f;
    layout.flexGrow = 1.5f;
    layout.gridColumns = 3u;
    layout.padding = { 4.0f, 5.0f };
    layout.margin = { 6.0f, 7.0f };
    layout.clipChildren = true;
    model.BeginTransaction("Edit Layout");
    ApplyLayoutEdit(
        model,
        layout,
        { "mode", "direction", "align", "spacing", "flexGrow", "gridColumns",
          "padding", "margin", "clipChildren" },
        false);
    model.EndTransaction();
    EXPECT_EQ(
        model.GetNodeProperty(nodeGuid, "layout.mode")->get<std::string>(),
        "Flex");
    EXPECT_EQ(
        model.GetNodeProperty(nodeGuid, "layout.direction")->get<std::string>(),
        "Horizontal");
    EXPECT_EQ(
        model.GetNodeProperty(nodeGuid, "layout.align")->get<std::string>(),
        "Center");
    EXPECT_FLOAT_EQ(
        model.GetNodeProperty(nodeGuid, "layout.spacing")->get<float>(),
        12.0f);
    EXPECT_EQ(
        model.GetNodeProperty(nodeGuid, "layout.gridColumns")->get<uint32_t>(),
        3u);
    EXPECT_TRUE(
        model.GetNodeProperty(nodeGuid, "layout.clipChildren")->get<bool>());

    UIDocumentStyleEdit style;
    style.backgroundColor = { 1.0f, 0.0f, 0.0f, 1.0f };
    style.color = { 0.0f, 1.0f, 0.0f, 0.5f };
    style.opacity = 0.5f;
    style.visible = false;
    style.enabled = false;
    style.interactive = true;
    model.BeginTransaction("Edit Style");
    ApplyStyleEdit(
        model,
        style,
        { "backgroundColor", "color", "opacity", "visible", "enabled", "interactive" },
        false);
    model.EndTransaction();
    EXPECT_FLOAT_EQ(
        model.GetNodeProperty(nodeGuid, "style.backgroundColor")->at(0).get<float>(),
        1.0f);
    EXPECT_FLOAT_EQ(
        model.GetNodeProperty(nodeGuid, "style.opacity")->get<float>(),
        0.5f);
    EXPECT_FALSE(model.GetNodeProperty(nodeGuid, "style.visible")->get<bool>());
    EXPECT_TRUE(model.GetNodeProperty(nodeGuid, "style.interactive")->get<bool>());

    UIDocumentContentEdit content;
    content.text = "Hello";
    content.password = true;
    content.fontAssetId = 42u;
    content.fontPixelSize = 24u;
    content.requiresRenderTarget = true;
    model.BeginTransaction("Edit Content");
    ApplyContentEdit(
        model,
        content,
        { "text", "password", "fontAssetId", "fontPixelSize", "requiresRenderTarget" },
        false);
    model.EndTransaction();
    EXPECT_EQ(model.GetNodeProperty(nodeGuid, "text")->get<std::string>(), "Hello");
    EXPECT_TRUE(model.GetNodeProperty(nodeGuid, "password")->get<bool>());
    EXPECT_EQ(
        model.GetNodeProperty(nodeGuid, "fontAssetId")->get<uint64_t>(),
        42u);
    EXPECT_EQ(
        model.GetNodeProperty(nodeGuid, "fontPixelSize")->get<uint32_t>(),
        24u);
    EXPECT_TRUE(
        model.GetNodeProperty(nodeGuid, "requiresRenderTarget")->get<bool>());

    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_EQ(model.GetNodeProperty(nodeGuid, "requiresRenderTarget"), nullptr);
}

TEST(UIDocumentNodeEdit, ContentEditAppliesAndReadsTextureAndFontReferences)
{
    ScopedDocumentDirectory directory;
    ScopedUndoService undo;

    const auto path = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));
    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(path));
    const auto nodeGuid = model.GetRootGuid();
    const auto* node = model.FindNode(nodeGuid);
    ASSERT_NE(node, nullptr);

    UIDocumentContentEdit content;
    content.texture.SetAssetIdString("0f3f794e-7578-4bcb-92cb-b3afb43ee861");
    content.texture.SetSubAssetKey("texture:main");
    content.texture.SetResourcePath("Assets/UI/icon.png");
    content.font.SetAssetIdString("861b35f8-3170-4d0b-9fdb-111b8e089a3d");
    content.font.SetSubAssetKey("font:main");
    content.font.SetResourcePath("Assets/Fonts/Inter.ttf");
    content.fontPixelSize = 28u;
    content.nineSlice = { 8.0f, 4.0f, 8.0f, 4.0f };

    model.BeginTransaction("Edit UI Resources");
    ApplyContentEdit(
        model,
        content,
        { "texture", "font", "fontPixelSize", "nineSlice" },
        false);
    model.EndTransaction();

    EXPECT_EQ(
        model.GetNodeProperty(nodeGuid, "texture.assetId")->get<std::string>(),
        "0f3f794e-7578-4bcb-92cb-b3afb43ee861");
    EXPECT_EQ(
        model.GetNodeProperty(nodeGuid, "texture.subAssetKey")->get<std::string>(),
        "texture:main");
    EXPECT_EQ(
        model.GetNodeProperty(nodeGuid, "texture.path")->get<std::string>(),
        "Assets/UI/icon.png");
    EXPECT_EQ(
        model.GetNodeProperty(nodeGuid, "font.assetId")->get<std::string>(),
        "861b35f8-3170-4d0b-9fdb-111b8e089a3d");
    EXPECT_EQ(
        model.GetNodeProperty(nodeGuid, "font.path")->get<std::string>(),
        "Assets/Fonts/Inter.ttf");
    EXPECT_EQ(
        model.GetNodeProperty(nodeGuid, "fontPixelSize")->get<uint32_t>(),
        28u);
    EXPECT_FLOAT_EQ(
        model.GetNodeProperty(nodeGuid, "nineSlice.left")->get<float>(),
        8.0f);
    EXPECT_FLOAT_EQ(
        model.GetNodeProperty(nodeGuid, "nineSlice.bottom")->get<float>(),
        4.0f);

    const auto readBack = ReadContentEdit(*model.FindNode(nodeGuid));
    EXPECT_EQ(
        readBack.texture.GetAssetIdString(),
        "0f3f794e-7578-4bcb-92cb-b3afb43ee861");
    EXPECT_EQ(readBack.texture.GetSubAssetKey(), "texture:main");
    EXPECT_EQ(readBack.texture.GetResourcePath(), "Assets/UI/icon.png");
    EXPECT_EQ(
        readBack.font.GetAssetIdString(),
        "861b35f8-3170-4d0b-9fdb-111b8e089a3d");
    EXPECT_EQ(readBack.font.GetResourcePath(), "Assets/Fonts/Inter.ttf");
    EXPECT_FLOAT_EQ(readBack.nineSlice.left, 8.0f);
    EXPECT_FLOAT_EQ(readBack.nineSlice.top, 4.0f);
    EXPECT_FLOAT_EQ(readBack.nineSlice.right, 8.0f);
    EXPECT_FLOAT_EQ(readBack.nineSlice.bottom, 4.0f);

    // Clearing a reference writes an empty reference object; the runtime parser
    // treats it as null while the JSON structure remains editable.
    UIDocumentContentEdit cleared = content;
    cleared.texture.Reset();
    cleared.font.Reset();
    model.BeginTransaction("Clear UI Resources");
    ApplyContentEdit(model, cleared, { "texture", "font" }, false);
    model.EndTransaction();
    const auto clearedBack = ReadContentEdit(*model.FindNode(nodeGuid));
    EXPECT_TRUE(clearedBack.texture.IsNull());
    EXPECT_TRUE(clearedBack.font.IsNull());

    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_FALSE(ReadContentEdit(*model.FindNode(nodeGuid)).texture.IsNull());
}

TEST(UIDocumentNodeEdit, MultiSelectionEditIsOneUndoableCommand)
{
    ScopedDocumentDirectory directory;
    ScopedUndoService undo;

    const auto path = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));
    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(path));
    const auto rootGuid = model.GetRootGuid();
    ASSERT_TRUE(model.AddChild(rootGuid, "Text"));
    const auto firstGuid = model.GetSelectedNodeGuid();
    ASSERT_TRUE(model.AddChild(rootGuid, "Text"));
    const auto secondGuid = model.GetSelectedNodeGuid();
    model.SetSelection({ firstGuid, secondGuid });

    const auto historyBefore = undo.Get().GetTransactionCount();
    UIDocumentStyleEdit style;
    style.opacity = 0.25f;
    style.backgroundColor = { 0.0f, 0.0f, 1.0f, 1.0f };

    model.BeginTransaction("Edit Style");
    ApplyStyleEdit(
        model,
        style,
        { "opacity", "backgroundColor" },
        false);
    model.EndTransaction();

    EXPECT_EQ(undo.Get().GetTransactionCount(), historyBefore + 1u);
    for (const auto& guid : { firstGuid, secondGuid })
    {
        EXPECT_FLOAT_EQ(
            model.GetNodeProperty(guid, "style.opacity")->get<float>(),
            0.25f);
        EXPECT_FLOAT_EQ(
            model.GetNodeProperty(guid, "style.backgroundColor")->at(2).get<float>(),
            1.0f);
    }

    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    for (const auto& guid : { firstGuid, secondGuid })
    {
        EXPECT_EQ(model.GetNodeProperty(guid, "style.opacity"), nullptr);
        EXPECT_EQ(model.GetNodeProperty(guid, "style.backgroundColor"), nullptr);
    }
}

TEST(UIDocumentNodeEdit, ReadHelpersUseRuntimeDefaults)
{
    EnsureUIDocumentInspectorReflection();
    const nlohmann::json node {
        { "name", "Node" },
        { "type", "Text" },
        { "layout", { { "mode", "Flex" }, { "size", { 10.0f, 20.0f } } } },
        { "style", { { "opacity", 0.5f } } }
    };

    const auto element = ReadElementEdit(node);
    EXPECT_EQ(element.name, "Node");
    EXPECT_TRUE(element.className.empty());

    const auto rect = ReadRectEdit(node);
    EXPECT_FLOAT_EQ(rect.size.x, 10.0f);
    EXPECT_FLOAT_EQ(rect.size.y, 20.0f);
    EXPECT_FLOAT_EQ(rect.anchorMin.x, 0.0f);
    EXPECT_FLOAT_EQ(rect.aspectRatio, 0.0f);

    const auto layout = ReadLayoutEdit(node);
    EXPECT_EQ(layout.mode, UIDocumentLayoutModeEdit::Flex);
    EXPECT_EQ(layout.direction, UIDocumentDirectionEdit::Vertical);
    EXPECT_EQ(layout.gridColumns, 1u);

    const auto style = ReadStyleEdit(node);
    EXPECT_FLOAT_EQ(style.opacity, 0.5f);
    EXPECT_TRUE(style.visible);
    EXPECT_TRUE(style.enabled);

    const auto content = ReadContentEdit(node);
    EXPECT_EQ(content.fontPixelSize, 16u);
    EXPECT_EQ(content.fontAssetId, 0u);
    EXPECT_FALSE(content.requiresRenderTarget);
}

TEST(UIDocumentNodeEdit, LargeAssetIdentifiersAndPixelSizeAreNormalized)
{
    ScopedDocumentDirectory directory;
    ScopedUndoService undo;

    const auto path = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));
    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(path));
    const auto rootGuid = model.GetRootGuid();
    ASSERT_TRUE(model.AddChild(rootGuid, "Text"));
    const auto nodeGuid = model.GetSelectedNodeGuid();

    constexpr uint64_t largeAssetId = 0x100000001ull;
    ASSERT_TRUE(model.SetNodeProperty(
        nodeGuid,
        "fontAssetId",
        largeAssetId,
        true));
    ASSERT_NE(model.GetNodeProperty(nodeGuid, "fontAssetId"), nullptr);
    EXPECT_EQ(
        model.GetNodeProperty(nodeGuid, "fontAssetId")->get<uint64_t>(),
        largeAssetId);

    ASSERT_TRUE(model.SetNodeProperty(
        nodeGuid,
        "fontPixelSize",
        100000u,
        true));
    EXPECT_EQ(
        model.GetNodeProperty(nodeGuid, "fontPixelSize")->get<uint32_t>(),
        65535u);
}

TEST(UIDocumentNodeEdit, DeclarativeMetadataProvidesLabelsRangesAndMultiline)
{
    EnsureUIDocumentInspectorReflection();
    const auto& registry = NLS::Editor::Inspector::InspectorMetadataRegistry::Get();

    const auto opacity = registry.Resolve(NLS_TYPEOF(UIDocumentStyleEdit), "opacity");
    EXPECT_EQ(opacity.displayName, "Opacity");
    ASSERT_TRUE(opacity.min.has_value());
    ASSERT_TRUE(opacity.max.has_value());
    EXPECT_FLOAT_EQ(*opacity.min, 0.0f);
    EXPECT_FLOAT_EQ(*opacity.max, 1.0f);

    const auto text = registry.Resolve(NLS_TYPEOF(UIDocumentContentEdit), "text");
    EXPECT_EQ(text.displayName, "Text");
    EXPECT_TRUE(text.multiline);

    const auto pixelSize = registry.Resolve(NLS_TYPEOF(UIDocumentContentEdit), "fontPixelSize");
    EXPECT_EQ(pixelSize.displayName, "Pixel Size");
    ASSERT_TRUE(pixelSize.min.has_value());
    EXPECT_FLOAT_EQ(*pixelSize.min, 1.0f);
    EXPECT_FLOAT_EQ(*pixelSize.max, 512.0f);
}

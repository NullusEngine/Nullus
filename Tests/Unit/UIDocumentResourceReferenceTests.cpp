#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <Reflection/Type.h>

#include "Assets/ArtifactManifest.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Engine/UI/UIDocument.h"
#include "Inspector/InspectorProjectionBuilder.h"
#include "Inspector/InspectorTypes.h"
#include "Inspector/ObjectReferenceSupport.h"
#include "Inspector/ReflectedPropertyEditor.h"
#include "UI/UIDocumentDesignerModel.h"
#include "UI/UIDocumentNodeEdit.h"
#include "UIDocumentDesignerTestSupport.h"

using namespace NLS;
using namespace NLS::Engine::UI;
using namespace NLS::Editor::UIDocumentTools;
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
                "type": "Panel",
                "name": "Root",
                "children": [
                    {
                        "nodeGuid": "33333333-3333-4333-8333-333333333333",
                        "type": "Image",
                        "name": "Icon",
                        "texture": {
                            "assetId": "0f3f794e-7578-4bcb-92cb-b3afb43ee861",
                            "subAssetKey": "texture:main",
                            "path": "Assets/UI/icon.png"
                        }
                    },
                    {
                        "nodeGuid": "44444444-4444-4444-8444-444444444444",
                        "type": "Text",
                        "name": "Label",
                        "text": "Hello",
                        "font": {
                            "assetId": "861b35f8-3170-4d0b-9fdb-111b8e089a3d",
                            "path": "Assets/Fonts/Inter.ttf"
                        },
                        "fontPixelSize": 20
                    }
                ]
            }
        })json";
    }
}

TEST(UIDocumentResourceReference, ParsesReferencesAndCollectsDependencies)
{
    std::vector<UIDocumentDiagnostic> diagnostics;
    const auto document = UIDocument::LoadFromJson(ReferenceDocument(), &diagnostics);
    ASSERT_TRUE(document.has_value());
    EXPECT_TRUE(diagnostics.empty());

    const auto& children = document->GetRootDefinition().children;
    ASSERT_EQ(children.size(), 2u);
    ASSERT_TRUE(children[0].texture.has_value());
    EXPECT_EQ(children[0].texture->GetAssetIdString(), kTextureGuid);
    EXPECT_EQ(children[0].texture->GetSubAssetKey(), "texture:main");
    EXPECT_EQ(children[0].texture->GetResourcePath(), "Assets/UI/icon.png");

    ASSERT_TRUE(children[1].font.has_value());
    EXPECT_EQ(children[1].font->GetAssetIdString(), kFontGuid);
    EXPECT_EQ(children[1].font->GetResourcePath(), "Assets/Fonts/Inter.ttf");
    EXPECT_EQ(children[1].fontPixelSize, 20u);

    std::vector<UITextureReference> textures;
    std::vector<UIFontReference> fonts;
    CollectUIResourceReferences(*document, textures, fonts);
    ASSERT_EQ(textures.size(), 1u);
    ASSERT_EQ(fonts.size(), 1u);
    EXPECT_EQ(textures.front().GetAssetIdString(), kTextureGuid);
    EXPECT_EQ(fonts.front().GetAssetIdString(), kFontGuid);
}

TEST(UIDocumentResourceReference, LegacyRuntimeIdsStillParseWithoutReferences)
{
    const auto json = R"json({
        "version": 1,
        "documentId": "11111111-1111-4111-8111-111111111111",
        "root": {
            "nodeGuid": "22222222-2222-4222-8222-222222222222",
            "type": "Panel",
            "name": "Root",
            "children": [
                {
                    "nodeGuid": "33333333-3333-4333-8333-333333333333",
                    "type": "Image",
                    "textureId": 42,
                    "textureGeneration": 3,
                    "fontAssetId": 1,
                    "fontPixelSize": 18
                }
            ]
        }
    })json";

    std::vector<UIDocumentDiagnostic> diagnostics;
    const auto document = UIDocument::LoadFromJson(json, &diagnostics);
    ASSERT_TRUE(document.has_value());
    EXPECT_TRUE(diagnostics.empty());
    const auto& child = document->GetRootDefinition().children.front();
    EXPECT_EQ(child.textureId, 42u);
    EXPECT_EQ(child.textureGeneration, 3u);
    EXPECT_EQ(child.fontAssetId, 1u);
    EXPECT_EQ(child.fontPixelSize, 18u);
    EXPECT_FALSE(child.texture.has_value());
    EXPECT_FALSE(child.font.has_value());

    std::vector<UITextureReference> textures;
    std::vector<UIFontReference> fonts;
    CollectUIResourceReferences(*document, textures, fonts);
    EXPECT_TRUE(textures.empty());
    EXPECT_TRUE(fonts.empty());
}

TEST(UIDocumentResourceReference, RejectsMalformedReferences)
{
    const auto reject = [](const std::string& referenceJson, const std::string& expectedPathFragment)
    {
        const auto json = std::string(R"json({
            "version": 1,
            "documentId": "11111111-1111-4111-8111-111111111111",
            "root": {
                "nodeGuid": "22222222-2222-4222-8222-222222222222",
                "type": "Panel",
                "children": [ { "nodeGuid": "33333333-3333-4333-8333-333333333333", "type": "Image", )json") +
            referenceJson +
            R"json( } ]
            }
        })json";

        std::vector<UIDocumentDiagnostic> diagnostics;
        const auto document = UIDocument::LoadFromJson(json, &diagnostics);
        EXPECT_FALSE(document.has_value());
        EXPECT_FALSE(diagnostics.empty());
        if (!diagnostics.empty())
        {
            EXPECT_NE(
                diagnostics.front().path.find(expectedPathFragment),
                std::string::npos)
                << diagnostics.front().path;
        }
    };

    reject(R"("texture": 5)", ".texture");
    reject(R"("texture": { "assetId": 5 })", ".texture.assetId");
    reject(R"("texture": { "assetId": "not-a-guid" })", ".texture.assetId");
    reject(R"("font": { "path": 5 })", ".font.path");
}

TEST(UIDocumentResourceReference, ReferencesUseSharedAssetDrawerContract)
{
    const auto textureType = NLS_TYPEOF(UITextureReference);
    const auto fontType = NLS_TYPEOF(UIFontReference);
    EXPECT_TRUE(Editor::Inspector::IsSupportedInspectorObjectReferenceType(textureType));
    EXPECT_TRUE(Editor::Inspector::IsSupportedInspectorObjectReferenceType(fontType));
    EXPECT_TRUE(Editor::Inspector::IsInspectorAssetObjectReferenceType(textureType));
    EXPECT_TRUE(Editor::Inspector::IsInspectorAssetObjectReferenceType(fontType));
    EXPECT_TRUE(Editor::Inspector::IsCompatibleInspectorReferenceArtifact(
        textureType, Core::Assets::ArtifactType::Texture));
    EXPECT_FALSE(Editor::Inspector::IsCompatibleInspectorReferenceArtifact(
        textureType, Core::Assets::ArtifactType::Font));
    EXPECT_TRUE(Editor::Inspector::IsCompatibleInspectorReferenceArtifact(
        fontType, Core::Assets::ArtifactType::Font));
    EXPECT_FALSE(Editor::Inspector::IsCompatibleInspectorReferenceArtifact(
        fontType, Core::Assets::ArtifactType::Texture));

    const auto textureId = Core::Assets::AssetId(*Guid::TryParse(kTextureGuid));
    const auto fontId = Core::Assets::AssetId(*Guid::TryParse(kFontGuid));
    const auto texturePayload = Editor::Assets::MakeEditorAssetDragPayload(
        "Assets/UI/icon.png",
        textureId,
        "texture:main",
        Core::Assets::ArtifactType::Texture,
        false,
        true);
    const auto fontPayload = Editor::Assets::MakeEditorAssetDragPayload(
        "Assets/Fonts/Inter.ttf",
        fontId,
        "font:main",
        Core::Assets::ArtifactType::Font,
        false,
        true);

    meta::Variant value(UITextureReference {});
    EXPECT_EQ(
        Editor::Inspector::GetInspectorObjectReferenceDisplay(value, textureType),
        "None (Texture)");
    EXPECT_FALSE(Editor::Inspector::AssignInspectorAssetReference(
        value, textureType, fontPayload));
    ASSERT_TRUE(Editor::Inspector::AssignInspectorAssetReference(
        value, textureType, texturePayload));

    const auto& texture = value.GetValue<UITextureReference>();
    EXPECT_EQ(texture.GetAssetId(), textureId);
    EXPECT_EQ(texture.GetSubAssetKey(), "texture:main");
    EXPECT_EQ(texture.GetResourcePath(), "Assets/UI/icon.png");
    EXPECT_EQ(
        Editor::Inspector::GetInspectorObjectReferenceDisplay(value, textureType),
        "icon.png (Texture)");
    const auto ping = Editor::Inspector::GetInspectorObjectReferencePingTarget(
        value, textureType);
    ASSERT_TRUE(ping.has_value());
    EXPECT_EQ(ping->kind, Editor::Inspector::InspectorTargetKind::AssetImporter);
    EXPECT_EQ(ping->assetId, textureId.ToString());
    EXPECT_EQ(ping->resourcePath, "Assets/UI/icon.png");
    EXPECT_TRUE(Editor::Inspector::ClearInspectorObjectReference(value, textureType));
    EXPECT_TRUE(value.GetValue<UITextureReference>().IsNull());

    meta::Variant fontValue(UIFontReference {});
    ASSERT_TRUE(Editor::Inspector::AssignInspectorAssetReference(
        fontValue, fontType, fontPayload));
    const auto& font = fontValue.GetValue<UIFontReference>();
    EXPECT_EQ(font.GetAssetIdString(), kFontGuid);
    EXPECT_EQ(font.GetSubAssetKey(), "font:main");
    EXPECT_EQ(font.GetResourcePath(), "Assets/Fonts/Inter.ttf");
    EXPECT_EQ(
        Editor::Inspector::GetInspectorObjectReferenceDisplay(fontValue, fontType),
        "Inter.ttf (Font)");
}

TEST(UIDocumentResourceReference, ContentViewModelProjectsReferencesAsObjectReference)
{
    EnsureUIDocumentInspectorReflection();
    auto lifetime = std::make_shared<int>(0);
    UIDocumentContentEdit content;
    content.texture.SetAssetIdString(kTextureGuid);
    content.texture.SetResourcePath("Assets/UI/icon.png");
    content.font.SetAssetIdString(kFontGuid);
    content.font.SetResourcePath("Assets/Fonts/Inter.ttf");

    auto backend = std::make_shared<Editor::Inspector::ReflectedValueBackend>(
        meta::Variant(content),
        lifetime);
    Editor::Serialization::SerializedTargetSet targets;
    targets.targets.push_back(Editor::Inspector::InspectorTargetHandle::FromAsset(
        Editor::Inspector::InspectorTargetKind::AssetImporter,
        "content[0]",
        "0",
        "ReflectedProperties"));
    Editor::Serialization::SerializedObject object(std::move(targets), backend);
    ASSERT_TRUE(object.IsValid()) << object.GetLastError();

    auto properties = Editor::Inspector::InspectorProjectionBuilder::BuildPropertyProjection(
        object,
        object.GetTargetSet(),
        {});
    if (properties.size() == 1u && properties.front().path.ToString() == "Value" &&
        properties.front().kind == Editor::Inspector::InspectorPropertyKind::Struct)
    {
        auto children = std::move(properties.front().children);
        properties = std::move(children);
    }

    const auto findProperty = [](const auto& self,
                                 const std::vector<Editor::Inspector::PropertyNode>& nodes,
                                 const std::string_view field) -> const Editor::Inspector::PropertyNode*
    {
        for (const auto& node : nodes)
        {
            if (node.fieldName == field)
                return &node;
            if (const auto* child = self(self, node.children, field))
                return child;
        }
        return nullptr;
    };

    const auto* texture = findProperty(findProperty, properties, "texture");
    ASSERT_NE(texture, nullptr);
    EXPECT_EQ(texture->kind, Editor::Inspector::InspectorPropertyKind::ObjectReference);
    EXPECT_EQ(texture->valueType, NLS_TYPEOF(UITextureReference));

    const auto* font = findProperty(findProperty, properties, "font");
    ASSERT_NE(font, nullptr);
    EXPECT_EQ(font->kind, Editor::Inspector::InspectorPropertyKind::ObjectReference);
    EXPECT_EQ(font->valueType, NLS_TYPEOF(UIFontReference));
}

TEST(UIDocumentResourceReference, ReferenceTypesAreReflected)
{
    EXPECT_EQ(NLS_TYPEOF(UITextureReference).GetFields().size(), 3u);
    EXPECT_EQ(NLS_TYPEOF(UIFontReference).GetFields().size(), 3u);
}

TEST(UIDocumentResourceReference, DesignerModelSupportsNestedReferencePaths)
{
    ScopedDocumentDirectory directory;
    ScopedUndoService undo;
    const auto path = directory.Write(
        "Assets/UI/Hud.ui",
        UIDocumentDesignerModel::CreateDocumentSource("Hud"));
    UIDocumentDesignerModel model;
    ASSERT_TRUE(model.Load(path));
    const auto rootGuid = model.GetRootGuid();

    ASSERT_TRUE(UIDocumentDesignerModel::IsSupportedPropertyPath("texture.assetId"));
    ASSERT_TRUE(UIDocumentDesignerModel::IsSupportedPropertyPath("font.resourcePath") == false);
    ASSERT_TRUE(UIDocumentDesignerModel::IsSupportedPropertyPath("font.path"));
    EXPECT_FALSE(UIDocumentDesignerModel::IsSupportedPropertyPath("texture.bogus"));

    ASSERT_TRUE(model.SetNodeProperty(rootGuid, "texture.assetId", kTextureGuid, true));
    ASSERT_TRUE(model.SetNodeProperty(rootGuid, "texture.path", "Assets/UI/icon.png", true));
    ASSERT_TRUE(model.SetNodeProperty(rootGuid, "font.assetId", kFontGuid, true));
    ASSERT_TRUE(model.SetNodeProperty(rootGuid, "font.path", "Assets/Fonts/Inter.ttf", true));

    const auto& root = model.GetDocument().at("root");
    ASSERT_TRUE(root.contains("texture"));
    ASSERT_TRUE(root.at("texture").is_object());
    EXPECT_EQ(root.at("texture").value("assetId", std::string {}), kTextureGuid);
    EXPECT_EQ(root.at("texture").value("path", std::string {}), "Assets/UI/icon.png");
    ASSERT_TRUE(root.contains("font"));
    EXPECT_EQ(root.at("font").value("assetId", std::string {}), kFontGuid);
    EXPECT_EQ(root.at("font").value("path", std::string {}), "Assets/Fonts/Inter.ttf");

    EXPECT_EQ(
        model.GetNodeProperty(rootGuid, "texture.assetId")->get<std::string>(),
        kTextureGuid);
    EXPECT_EQ(
        model.GetNodeProperty(rootGuid, "font.path")->get<std::string>(),
        "Assets/Fonts/Inter.ttf");

    ASSERT_TRUE(undo.Get().RequestUndo().Succeeded());
    EXPECT_EQ(model.GetNodeProperty(rootGuid, "font.path"), nullptr);
    EXPECT_NE(model.GetNodeProperty(rootGuid, "texture.assetId"), nullptr);
}

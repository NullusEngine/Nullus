#include <gtest/gtest.h>

#include <Reflection/ReflectionDatabase.h>
#include <Reflection/ExternalReflectionRegistration.h>
#include <Reflection/RuntimeMetaProperties.h>
#include <imgui.h>
#include <Math/Color.h>
#include <Math/Rect.h>
#include <Math/Vector2.h>
#include <UI/Plugins/DataDispatcher.h>
#include <UI/Plugins/DDTarget.h>
#include <UI/Widgets/Drags/DragSingleScalar.h>
#include <UI/Widgets/Drags/DragMultipleScalars.h>
#include <UI/Widgets/Buttons/ButtonSmall.h>
#include <UI/Widgets/InputFields/InputText.h>
#include <UI/Widgets/Layout/Columns.h>
#include <UI/Widgets/Layout/Group.h>
#include <UI/Widgets/Layout/TreeNode.h>
#include <UI/Widgets/Selection/CheckBox.h>
#include <UI/Widgets/Selection/ColorEdit.h>
#include <UI/Widgets/Selection/ComboBox.h>
#include <UI/Widgets/Sliders/SliderFloat.h>
#include <UI/Widgets/Sliders/SliderInt.h>
#include <UI/Widgets/Texts/Text.h>

#include "Components/LightComponent.h"
#include "Components/MeshRenderer.h"
#include "Assets/EditorAssetDragPayload.h"
#include "GameObject.h"
#include "LayerMask.h"
#include "Panels/ReflectedPropertyDrawer.h"
#include "Rendering/Geometry/Bounds.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Mesh.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/Texture.h"
#include "Rendering/Resources/Texture2D.h"
#include "SceneSystem/Scene.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/PPtr.h"
#include "Settings/EditorSettings.h"
#include "Settings/TagLayerSettings.h"

#include <vector>

namespace NLS::Editor::Panels::Tests
{
class ScopedImGuiContext
{
public:
    ScopedImGuiContext()
    {
        if (ImGui::GetCurrentContext() == nullptr)
        {
            m_context = ImGui::CreateContext();
            ImGui::SetCurrentContext(m_context);
        }
    }

    ~ScopedImGuiContext()
    {
        if (m_context != nullptr)
            ImGui::DestroyContext(m_context);
    }

private:
    ImGuiContext* m_context = nullptr;
};

template<typename Widget>
Widget* FindFirstWidgetOfType(NLS::UI::Internal::WidgetContainer& p_container)
{
    for (const auto& [widget, _] : p_container.GetWidgets())
    {
        if (auto* typedWidget = dynamic_cast<Widget*>(widget))
            return typedWidget;
    }

    return nullptr;
}

NLS::UI::Widgets::ButtonSmall* FindButtonSmallByLabel(
    NLS::UI::Internal::WidgetContainer& p_container,
    const std::string& p_label)
{
    for (const auto& [widget, _] : p_container.GetWidgets())
    {
        auto* button = dynamic_cast<NLS::UI::Widgets::ButtonSmall*>(widget);
        if (button != nullptr && button->label == p_label)
            return button;
    }

    return nullptr;
}

NLS::UI::Widgets::TreeNode* FindTreeNodeByName(
    NLS::UI::Internal::WidgetContainer& p_container,
    const std::string& p_name)
{
    for (const auto& [widget, _] : p_container.GetWidgets())
    {
        auto* treeNode = dynamic_cast<NLS::UI::Widgets::TreeNode*>(widget);
        if (treeNode != nullptr && treeNode->name == p_name)
            return treeNode;

        auto* nestedContainer = dynamic_cast<NLS::UI::Internal::WidgetContainer*>(widget);
        if (nestedContainer != nullptr)
        {
            if (auto* nested = FindTreeNodeByName(*nestedContainer, p_name))
                return nested;
        }
    }

    return nullptr;
}

NLS::UI::Widgets::Text* FindTextContaining(
    NLS::UI::Internal::WidgetContainer& p_container,
    const std::string& p_text)
{
    for (const auto& [widget, _] : p_container.GetWidgets())
    {
        auto* text = dynamic_cast<NLS::UI::Widgets::Text*>(widget);
        if (text != nullptr && text->content.find(p_text) != std::string::npos)
            return text;

        auto* nestedContainer = dynamic_cast<NLS::UI::Internal::WidgetContainer*>(widget);
        if (nestedContainer != nullptr)
        {
            if (auto* nested = FindTextContaining(*nestedContainer, p_text))
                return nested;
        }
    }

    return nullptr;
}

struct InspectorCoreFieldFixture
{
    Maths::Vector2 uvScale {1.0f, 1.0f};
    Maths::Color tint {0.25f, 0.5f, 0.75f, 1.0f};
    Maths::Rect viewport {1.0f, 2.0f, 128.0f, 64.0f};
    Render::Geometry::Bounds bounds {{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}};
    NLS::Array<std::string> names {"Main", "Detail"};
    NLS::Array<float> weights {1.0f, 2.0f};
    NLS::Array<bool> toggles {true, false};
    NLS::Array<Maths::Vector3> points {{1.0f, 2.0f, 3.0f}};
    NLS::Array<Maths::Color> swatches {{0.25f, 0.5f, 0.75f, 1.0f}};
    Engine::LayerMask visibleLayers {0b11};
    Engine::GameObject* targetGameObject = nullptr;
    Engine::Components::Component* targetComponent = nullptr;
    const Engine::GameObject* constTargetGameObject = nullptr;
    const Engine::Components::Component* constTargetComponent = nullptr;
    Engine::Components::LightComponent* targetLightComponent = nullptr;
    Engine::Components::MeshRenderer* targetMeshRenderer = nullptr;
    Render::Resources::Texture2D* texture = nullptr;
    Render::Resources::Mesh* mesh = nullptr;
    Render::Resources::Shader* shader = nullptr;
    Render::Resources::Material* material = nullptr;
    Engine::Serialize::PPtr<Render::Resources::Texture> textureReference;
    Engine::Serialize::PPtr<Render::Resources::Material> objectReference;
    NLS::Array<Engine::Serialize::PPtr<Render::Resources::Material>> materialReferences;
};

struct UnsupportedPPtrFieldFixture
{
    Engine::Serialize::PPtr<Engine::GameObject> gameObjectReference;
};

struct InspectorRangeFieldFixture
{
    float normalized = 0.5f;
    int priority = 2;
};

struct CountingArrayFieldFixture
{
    NLS::Array<float> values {1.0f, 2.0f, 3.0f};
    int getterCalls = 0;

    NLS::Array<float> GetValues()
    {
        ++getterCalls;
        return values;
    }

    void SetValues(const NLS::Array<float>& p_values)
    {
        values = p_values;
    }
};

struct InspectorNestedValueElementFixture
{
    int count = 1;
    float weight = 2.0f;
    struct Child
    {
        int depthCount = 9;
    } child;
};

struct InspectorNestedArrayFixture
{
    NLS::Array<InspectorNestedValueElementFixture> entries {{3, 4.0f}};
    std::vector<InspectorNestedValueElementFixture> vectorEntries {{5, 6.0f}};
};

struct InspectorRawPointerArrayFixture
{
    NLS::Array<int*> pointers {nullptr};
};

struct InspectorRecursiveValueFixture
{
    int value = 1;
    NLS::Array<InspectorRecursiveValueFixture> children;
};

struct InspectorRecursiveArrayFixture
{
    NLS::Array<InspectorRecursiveValueFixture> roots;
};

NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::Panels::Tests::InspectorRangeFieldFixture)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::Panels::Tests::UnsupportedPPtrFieldFixture)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::Panels::Tests::CountingArrayFieldFixture)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::Panels::Tests::InspectorNestedValueElementFixture)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::Panels::Tests::InspectorNestedValueElementFixture::Child)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::Panels::Tests::InspectorNestedArrayFixture)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::Panels::Tests::InspectorRawPointerArrayFixture)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::Panels::Tests::InspectorRecursiveValueFixture)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Editor::Panels::Tests::InspectorRecursiveArrayFixture)
NLS_META_EXTERNAL_TYPE_NAME(NLS::Engine::Serialize::PPtr<NLS::Engine::GameObject>)

inline void RegisterInspectorDrawerTestReflection(
    meta::ReflectionDatabase& db,
    meta::ReflectionRegistrationPhase phase)
{
    NLS_META_EXTERNAL_BEGIN(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture)
        NLS_META_EXTERNAL_FIELD(NLS::Maths::Vector2, uvScale);
        NLS_META_EXTERNAL_FIELD(NLS::Maths::Color, tint);
        NLS_META_EXTERNAL_FIELD(NLS::Maths::Rect, viewport);
        NLS_META_EXTERNAL_FIELD(NLS::Render::Geometry::Bounds, bounds);
        NLS_META_EXTERNAL_FIELD(NLS::Array<std::string>, names);
        NLS_META_EXTERNAL_FIELD(NLS::Array<float>, weights);
        NLS_META_EXTERNAL_FIELD(NLS::Array<bool>, toggles);
        NLS_META_EXTERNAL_FIELD(NLS::Array<NLS::Maths::Vector3>, points);
        NLS_META_EXTERNAL_FIELD(NLS::Array<NLS::Maths::Color>, swatches);
        NLS_META_EXTERNAL_FIELD(NLS::Engine::LayerMask, visibleLayers);
        NLS_META_EXTERNAL_FIELD(NLS::Engine::GameObject*, targetGameObject);
        NLS_META_EXTERNAL_FIELD(NLS::Engine::Components::Component*, targetComponent);
        NLS_META_EXTERNAL_FIELD(const NLS::Engine::GameObject*, constTargetGameObject);
        NLS_META_EXTERNAL_FIELD(const NLS::Engine::Components::Component*, constTargetComponent);
        NLS_META_EXTERNAL_FIELD(NLS::Engine::Components::LightComponent*, targetLightComponent);
        NLS_META_EXTERNAL_FIELD(NLS::Engine::Components::MeshRenderer*, targetMeshRenderer);
        NLS_META_EXTERNAL_FIELD(NLS::Render::Resources::Texture2D*, texture);
        NLS_META_EXTERNAL_FIELD(NLS::Render::Resources::Mesh*, mesh);
        NLS_META_EXTERNAL_FIELD(NLS::Render::Resources::Shader*, shader);
        NLS_META_EXTERNAL_FIELD(NLS::Render::Resources::Material*, material);
        NLS_META_EXTERNAL_FIELD(NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Texture>, textureReference);
        NLS_META_EXTERNAL_FIELD(NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Material>, objectReference);
        NLS_META_EXTERNAL_FIELD(NLS::Array<NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Material>>, materialReferences);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Editor::Panels::Tests::InspectorRangeFieldFixture)
        metaExternalType.AddField<MetaExternalType, float>(
            "normalized",
            &MetaExternalType::normalized,
            &MetaExternalType::normalized,
            {{NLS_TYPEOF(NLS::meta::Range), NLS::meta::MetaPropertyInitializer<NLS::meta::Range>(0.0f, 1.0f)}});
        metaExternalType.AddField<MetaExternalType, int>(
            "priority",
            &MetaExternalType::priority,
            &MetaExternalType::priority,
            {{NLS_TYPEOF(NLS::meta::Range), NLS::meta::MetaPropertyInitializer<NLS::meta::Range>(0.0f, 5.0f)}});
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Editor::Panels::Tests::CountingArrayFieldFixture)
        metaExternalType.AddField<MetaExternalType, NLS::Array<float>>(
            "values",
            &MetaExternalType::GetValues,
            &MetaExternalType::SetValues,
            {});
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Editor::Panels::Tests::InspectorNestedValueElementFixture)
        NLS_META_EXTERNAL_FIELD(int, count);
        NLS_META_EXTERNAL_FIELD(float, weight);
        NLS_META_EXTERNAL_FIELD(NLS::Editor::Panels::Tests::InspectorNestedValueElementFixture::Child, child);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Editor::Panels::Tests::InspectorNestedValueElementFixture::Child)
        NLS_META_EXTERNAL_FIELD(int, depthCount);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Editor::Panels::Tests::InspectorNestedArrayFixture)
        NLS_META_EXTERNAL_FIELD(NLS::Array<NLS::Editor::Panels::Tests::InspectorNestedValueElementFixture>, entries);
        NLS_META_EXTERNAL_FIELD(std::vector<NLS::Editor::Panels::Tests::InspectorNestedValueElementFixture>, vectorEntries);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Editor::Panels::Tests::InspectorRawPointerArrayFixture)
        NLS_META_EXTERNAL_FIELD(NLS::Array<int*>, pointers);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Editor::Panels::Tests::InspectorRecursiveValueFixture)
        NLS_META_EXTERNAL_FIELD(int, value);
        NLS_META_EXTERNAL_FIELD(NLS::Array<NLS::Editor::Panels::Tests::InspectorRecursiveValueFixture>, children);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Editor::Panels::Tests::InspectorRecursiveArrayFixture)
        NLS_META_EXTERNAL_FIELD(NLS::Array<NLS::Editor::Panels::Tests::InspectorRecursiveValueFixture>, roots);
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Engine::Serialize::PPtr<NLS::Engine::GameObject>)
    NLS_META_EXTERNAL_END();

    NLS_META_EXTERNAL_BEGIN(NLS::Editor::Panels::Tests::UnsupportedPPtrFieldFixture)
        NLS_META_EXTERNAL_FIELD(NLS::Engine::Serialize::PPtr<NLS::Engine::GameObject>, gameObjectReference);
    NLS_META_EXTERNAL_END();
}
}

NLS_META_EXTERNAL_MODULE(NLS::Editor::Panels::Tests::RegisterInspectorDrawerTestReflection)

TEST(ReflectedPropertyDrawerTests, FormatsFieldAndEnumLabels)
{
    EXPECT_EQ(NLS::Editor::Panels::FormatReflectedFieldLabel("lightBillboardScale"), "Light Billboard Scale");
    EXPECT_EQ(NLS::Editor::Panels::FormatEnumChoiceLabel("LESS_EQUAL"), "Less Equal");
    EXPECT_EQ(NLS::Editor::Panels::FormatEnumChoiceLabel("FrustumCulled"), "Frustum Culled");
}

TEST(ReflectedPropertyDrawerTests, NormalizesSearchIgnoringCaseAndSpaces)
{
    EXPECT_EQ(NLS::Editor::Panels::NormalizeReflectedSearchText(" Light Billboard "), "lightbillboard");
}

TEST(ReflectedPropertyDrawerTests, ClassifiesSupportedReflectedFields)
{
    NLS::meta::ReflectionDatabase::Instance();
    const auto type = NLS_TYPEOF(NLS::Editor::Settings::EditorSceneToolSettingsObject);
    const auto& field = type.GetField("translationSnapUnit");

    ASSERT_TRUE(field.IsValid());
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(field),
        NLS::Editor::Panels::ReflectedPropertySupport::Float);
    EXPECT_TRUE(NLS::Editor::Panels::ReflectedFieldMatchesSearch(field, "translation snap"));
}

TEST(ReflectedPropertyDrawerTests, ClassifiesReflectedArrayFieldsBeforeScalarElementTypes)
{
    NLS::meta::ReflectionDatabase::Instance();
    const auto type = NLS_TYPEOF(NLS::Engine::Components::MeshRenderer);
    const auto& materials = type.GetField("materials");
    const auto& userMatrixValues = type.GetField("userMatrixValues");

    ASSERT_TRUE(materials.IsValid());
    ASSERT_TRUE(userMatrixValues.IsValid());
    ASSERT_TRUE(materials.GetType().IsArray());
    ASSERT_TRUE(userMatrixValues.GetType().IsArray());
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(materials),
        NLS::Editor::Panels::ReflectedPropertySupport::Array);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(userMatrixValues),
        NLS::Editor::Panels::ReflectedPropertySupport::Array);
}

TEST(ReflectedPropertyDrawerTests, ClassifiesUnityCoreVector2AndColorFields)
{
    NLS::meta::ReflectionDatabase::Instance();
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& uvScale = type.GetField("uvScale");
    const auto& tint = type.GetField("tint");

    ASSERT_TRUE(uvScale.IsValid());
    ASSERT_TRUE(tint.IsValid());
    EXPECT_NE(
        NLS::Editor::Panels::GetReflectedPropertySupport(uvScale),
        NLS::Editor::Panels::ReflectedPropertySupport::Unsupported);
    EXPECT_NE(
        NLS::Editor::Panels::GetReflectedPropertySupport(tint),
        NLS::Editor::Panels::ReflectedPropertySupport::Unsupported);
}

TEST(ReflectedPropertyDrawerTests, ClassifiesRectBoundsArraysAndLayerMaskFields)
{
    NLS::meta::ReflectionDatabase::Instance();
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& viewport = type.GetField("viewport");
    const auto& bounds = type.GetField("bounds");
    const auto& names = type.GetField("names");
    const auto& weights = type.GetField("weights");
    const auto& toggles = type.GetField("toggles");
    const auto& points = type.GetField("points");
    const auto& swatches = type.GetField("swatches");
    const auto& visibleLayers = type.GetField("visibleLayers");

    ASSERT_TRUE(viewport.IsValid());
    ASSERT_TRUE(bounds.IsValid());
    ASSERT_TRUE(names.IsValid());
    ASSERT_TRUE(weights.IsValid());
    ASSERT_TRUE(toggles.IsValid());
    ASSERT_TRUE(points.IsValid());
    ASSERT_TRUE(swatches.IsValid());
    ASSERT_TRUE(visibleLayers.IsValid());
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(viewport),
        NLS::Editor::Panels::ReflectedPropertySupport::Rect);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(bounds),
        NLS::Editor::Panels::ReflectedPropertySupport::Bounds);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(names),
        NLS::Editor::Panels::ReflectedPropertySupport::Array);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(weights),
        NLS::Editor::Panels::ReflectedPropertySupport::Array);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(toggles),
        NLS::Editor::Panels::ReflectedPropertySupport::Array);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(points),
        NLS::Editor::Panels::ReflectedPropertySupport::Array);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(swatches),
        NLS::Editor::Panels::ReflectedPropertySupport::Array);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(visibleLayers),
        NLS::Editor::Panels::ReflectedPropertySupport::LayerMask);
}

TEST(ReflectedPropertyDrawerTests, ClassifiesObjectReferencesAsUnityObjectReferences)
{
    NLS::meta::ReflectionDatabase::Instance();
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& gameObject = type.GetField("targetGameObject");
    const auto& component = type.GetField("targetComponent");
    const auto& constGameObject = type.GetField("constTargetGameObject");
    const auto& constComponent = type.GetField("constTargetComponent");
    const auto& lightComponent = type.GetField("targetLightComponent");
    const auto& MeshRenderer = type.GetField("targetMeshRenderer");
    const auto& texture = type.GetField("texture");
    const auto& mesh = type.GetField("mesh");
    const auto& shader = type.GetField("shader");
    const auto& material = type.GetField("material");
    const auto& textureReference = type.GetField("textureReference");
    const auto& objectReference = type.GetField("objectReference");
    const auto& materialReferences = type.GetField("materialReferences");

    ASSERT_TRUE(gameObject.IsValid());
    ASSERT_TRUE(component.IsValid());
    ASSERT_TRUE(constGameObject.IsValid());
    ASSERT_TRUE(constComponent.IsValid());
    ASSERT_TRUE(lightComponent.IsValid());
    ASSERT_TRUE(MeshRenderer.IsValid());
    ASSERT_TRUE(texture.IsValid());
    ASSERT_TRUE(mesh.IsValid());
    ASSERT_TRUE(shader.IsValid());
    ASSERT_TRUE(material.IsValid());
    ASSERT_TRUE(textureReference.IsValid());
    ASSERT_TRUE(objectReference.IsValid());
    ASSERT_TRUE(materialReferences.IsValid());
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(gameObject),
        NLS::Editor::Panels::ReflectedPropertySupport::ObjectReference);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(component),
        NLS::Editor::Panels::ReflectedPropertySupport::ObjectReference);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(constGameObject),
        NLS::Editor::Panels::ReflectedPropertySupport::ObjectReference);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(constComponent),
        NLS::Editor::Panels::ReflectedPropertySupport::ObjectReference);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(lightComponent),
        NLS::Editor::Panels::ReflectedPropertySupport::ObjectReference);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(MeshRenderer),
        NLS::Editor::Panels::ReflectedPropertySupport::ObjectReference);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(texture),
        NLS::Editor::Panels::ReflectedPropertySupport::ObjectReference);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(mesh),
        NLS::Editor::Panels::ReflectedPropertySupport::ObjectReference);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(shader),
        NLS::Editor::Panels::ReflectedPropertySupport::ObjectReference);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(material),
        NLS::Editor::Panels::ReflectedPropertySupport::ObjectReference);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(textureReference),
        NLS::Editor::Panels::ReflectedPropertySupport::ObjectReference);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(objectReference),
        NLS::Editor::Panels::ReflectedPropertySupport::ObjectReference);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(materialReferences),
        NLS::Editor::Panels::ReflectedPropertySupport::Array);
}

TEST(ReflectedPropertyDrawerTests, UnsupportedTypedPPtrDoesNotPretendToHaveACompleteObjectReferenceDrawer)
{
    NLS::meta::ReflectionDatabase::Instance();
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::UnsupportedPPtrFieldFixture);
    const auto& field = type.GetField("gameObjectReference");

    ASSERT_TRUE(field.IsValid());
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(field),
        NLS::Editor::Panels::ReflectedPropertySupport::Unsupported);
}

TEST(ReflectedPropertyDrawerTests, UnityObjectIdentifierWidgetDisplaysAndClearsIdentity)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;
    const auto identifier = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("dddddddd-dddd-4ddd-9ddd-dddddddddddd")),
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(
            NLS::Guid::Parse("dddddddd-dddd-4ddd-9ddd-dddddddddddd"),
            "Assets/Materials/Default.mat"),
        "Assets/Materials/Default.mat");
    object.objectReference = NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Material>(
        NLS::Engine::Serialize::PersistentManager::Instance().ObjectIdentifierToInstanceID(identifier));

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& objectReferenceField = type.GetField("objectReference");

    ASSERT_TRUE(objectReferenceField.IsValid());
    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        objectReferenceField));
    ASSERT_EQ(root.GetWidgets().size(), 2u);

    auto* objectGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
    ASSERT_NE(objectGroup, nullptr);
    ASSERT_EQ(objectGroup->GetWidgets().size(), 3u);

    auto* display = dynamic_cast<NLS::UI::Widgets::Text*>(objectGroup->GetWidgets()[0].first);
    ASSERT_NE(display, nullptr);
    EXPECT_NE(display->content.find("Assets/Materials/Default.mat"), std::string::npos);
    EXPECT_NE(display->content.find("Material"), std::string::npos);

    auto* pickerButton = dynamic_cast<NLS::UI::Widgets::ButtonSmall*>(objectGroup->GetWidgets()[1].first);
    ASSERT_NE(pickerButton, nullptr);
    EXPECT_EQ(pickerButton->label, "o");

    auto* clearButton = dynamic_cast<NLS::UI::Widgets::ButtonSmall*>(objectGroup->GetWidgets()[2].first);
    ASSERT_NE(clearButton, nullptr);
    EXPECT_EQ(clearButton->label, "x");
    clearButton->ClickedEvent.Invoke();

    EXPECT_TRUE(object.objectReference.IsNull());
}

TEST(ReflectedPropertyDrawerTests, UnityObjectReferenceDisplayRefreshesAfterClear)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;
    const auto identifier = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("12121212-3434-4567-8899-121212121212")),
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(
            NLS::Guid::Parse("12121212-3434-4567-8899-121212121212"),
            "material:Default"),
        "Assets/Materials/Default.mat");
    object.objectReference = NLS::Engine::Serialize::PPtr<NLS::Render::Resources::Material>(
        NLS::Engine::Serialize::PersistentManager::Instance().ObjectIdentifierToInstanceID(identifier));

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& objectReferenceField = type.GetField("objectReference");
    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        objectReferenceField));

    auto* objectGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
    ASSERT_NE(objectGroup, nullptr);
    auto* display = NLS::Editor::Panels::Tests::FindFirstWidgetOfType<NLS::UI::Widgets::Text>(*objectGroup);
    ASSERT_NE(display, nullptr);
    auto* clearButton = NLS::Editor::Panels::Tests::FindButtonSmallByLabel(*objectGroup, "x");
    ASSERT_NE(clearButton, nullptr);

    clearButton->ClickedEvent.Invoke();
    auto* dispatcher = display->GetPlugin<NLS::UI::DataDispatcher<std::string>>();
    ASSERT_NE(dispatcher, nullptr);

    const auto refreshedDisplay = dispatcher->Gather();
    EXPECT_NE(refreshedDisplay.find("Empty (Material)"), std::string::npos) << refreshedDisplay;
}

#if 0
TEST(ReflectedPropertyDrawerTests, UnityObjectReferenceWidgetAcceptsEditorAssetDragPayloadAsPersistentPPtr)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;

    int changedFields = 0;
    NLS::Editor::Panels::ReflectedPropertyDrawerOptions options;
    options.onFieldChanged = [&changedFields](const NLS::meta::Field& field)
    {
        if (field.GetName() == "objectReference")
            ++changedFields;
    };

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& objectReferenceField = type.GetField("objectReference");

    ASSERT_TRUE(objectReferenceField.IsValid());
    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        objectReferenceField,
        options));

    auto* objectGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
    ASSERT_NE(objectGroup, nullptr);
    auto* display = NLS::Editor::Panels::Tests::FindFirstWidgetOfType<NLS::UI::Widgets::Text>(*objectGroup);
    ASSERT_NE(display, nullptr);
    auto* assetTarget = display->GetPlugin<NLS::UI::DDTarget<NLS::Editor::Assets::EditorAssetDragPayload>>();
    ASSERT_NE(assetTarget, nullptr);

    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"));
    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
        "Assets/Materials/Hero.mat",
        assetId,
        "material:Body",
        NLS::Core::Assets::ArtifactType::Material,
        false,
        true);

    assetTarget->DataReceivedEvent.Invoke(payload);

    ASSERT_FALSE(object.objectReference.IsNull());
    EXPECT_EQ(changedFields, 1);

    NLS::Engine::Serialize::ObjectIdentifier identifier;
    ASSERT_TRUE(NLS::Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(
        object.objectReference.GetInstanceID(),
        identifier));
    EXPECT_EQ(identifier.guid, assetId.GetGuid());
    EXPECT_EQ(
        identifier.localIdentifierInFile,
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(assetId.GetGuid(), "material:Body"));
    EXPECT_EQ(identifier.fileType, NLS::Engine::Serialize::FileType::SerializedAssetType);
    EXPECT_EQ(identifier.filePath, "Assets/Materials/Hero.mat");
}

TEST(ReflectedPropertyDrawerTests, UnityObjectReferenceArrayElementAcceptsEditorAssetDragPayload)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;
    object.materialReferences.emplace_back();

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& materialReferences = type.GetField("materialReferences");

    ASSERT_TRUE(materialReferences.IsValid());
    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        materialReferences));

    auto* arrayGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
    ASSERT_NE(arrayGroup, nullptr);
    auto* foldout = dynamic_cast<NLS::UI::Widgets::TreeNode*>(arrayGroup->GetWidgets()[0].first);
    ASSERT_NE(foldout, nullptr);
    auto* referenceGroup = NLS::Editor::Panels::Tests::FindFirstWidgetOfType<NLS::UI::Widgets::Group>(*foldout);
    ASSERT_NE(referenceGroup, nullptr);
    auto* display = NLS::Editor::Panels::Tests::FindFirstWidgetOfType<NLS::UI::Widgets::Text>(*referenceGroup);
    ASSERT_NE(display, nullptr);
    auto* assetTarget = display->GetPlugin<NLS::UI::DDTarget<NLS::Editor::Assets::EditorAssetDragPayload>>();
    ASSERT_NE(assetTarget, nullptr);

    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("cccccccc-cccc-4ccc-8ccc-cccccccccccc"));
    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
        "Assets/Materials/Array.mat",
        assetId,
        "material:Slot",
        NLS::Core::Assets::ArtifactType::Material,
        false,
        true);

    assetTarget->DataReceivedEvent.Invoke(payload);

    ASSERT_EQ(object.materialReferences.size(), 1u);
    ASSERT_NE(object.materialReferences[0].GetInstanceID(), NLS::Engine::Serialize::InstanceID_None);

    NLS::Engine::Serialize::ObjectIdentifier identifier;
    ASSERT_TRUE(NLS::Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(
        object.materialReferences[0].GetInstanceID(),
        identifier));
    EXPECT_EQ(identifier.guid, assetId.GetGuid());
    EXPECT_EQ(identifier.localIdentifierInFile, NLS::Engine::Serialize::MakeLocalIdentifierInFile(
        assetId.GetGuid(),
        "material:Slot"));
}

TEST(ReflectedPropertyDrawerTests, UnityObjectReferenceWidgetRejectsWrongResourceType)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;

    int changedFields = 0;
    NLS::Editor::Panels::ReflectedPropertyDrawerOptions options;
    options.onFieldChanged = [&changedFields](const NLS::meta::Field&) { ++changedFields; };

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& objectReferenceField = type.GetField("objectReference");

    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        objectReferenceField,
        options));

    auto* objectGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
    ASSERT_NE(objectGroup, nullptr);
    auto* display = NLS::Editor::Panels::Tests::FindFirstWidgetOfType<NLS::UI::Widgets::Text>(*objectGroup);
    ASSERT_NE(display, nullptr);
    auto* assetTarget = display->GetPlugin<NLS::UI::DDTarget<NLS::Editor::Assets::EditorAssetDragPayload>>();
    ASSERT_NE(assetTarget, nullptr);

    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
        "Assets/Textures/Hero.png",
        NLS::Core::Assets::AssetId(NLS::Guid::Parse("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee")),
        "texture:Hero",
        NLS::Core::Assets::ArtifactType::Texture,
        false,
        true);

    assetTarget->DataReceivedEvent.Invoke(payload);

    EXPECT_TRUE(object.objectReference.IsNull());
    EXPECT_EQ(changedFields, 0);
    EXPECT_NE(display->content.find("Empty"), std::string::npos);
}

TEST(ReflectedPropertyDrawerTests, UnityObjectReferencePickerButtonAcceptsCompatiblePayloadThroughSharedSelectionPath)
{
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;

    int changedFields = 0;
    NLS::Editor::Panels::ReflectedPropertyDrawerOptions options;
    options.onFieldChanged = [&changedFields](const NLS::meta::Field& field)
    {
        if (field.GetName() == "objectReference")
            ++changedFields;
    };

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& objectReferenceField = type.GetField("objectReference");
    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        objectReferenceField,
        options));

    auto* objectGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
    ASSERT_NE(objectGroup, nullptr);
    ASSERT_EQ(objectGroup->GetWidgets().size(), 3u);

    auto* pickerButton = dynamic_cast<NLS::UI::Widgets::ButtonSmall*>(objectGroup->GetWidgets()[1].first);
    ASSERT_NE(pickerButton, nullptr);
    EXPECT_EQ(pickerButton->label, "o");
    auto* pickerTarget = pickerButton->GetPlugin<NLS::UI::DDTarget<NLS::Editor::Assets::EditorAssetDragPayload>>();
    ASSERT_NE(pickerTarget, nullptr);
    EXPECT_EQ(pickerTarget->identifier, NLS::Editor::Assets::kEditorAssetDragPayloadType);

    const auto assetId = NLS::Core::Assets::AssetId(
        NLS::Guid::Parse("abababab-abab-4bab-8bab-abababababab"));
    const auto payload = NLS::Editor::Assets::MakeEditorAssetDragPayload(
        "Assets/Materials/Picked.mat",
        assetId,
        "material:Picked",
        NLS::Core::Assets::ArtifactType::Material,
        false,
        true);

    pickerTarget->DataReceivedEvent.Invoke(payload);

    ASSERT_FALSE(object.objectReference.IsNull());
    EXPECT_EQ(changedFields, 1);

    NLS::Engine::Serialize::ObjectIdentifier identifier;
    ASSERT_TRUE(NLS::Engine::Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(
        object.objectReference.GetInstanceID(),
        identifier));
    EXPECT_EQ(identifier.guid, assetId.GetGuid());
    EXPECT_EQ(
        identifier.localIdentifierInFile,
        NLS::Engine::Serialize::MakeLocalIdentifierInFile(assetId.GetGuid(), "material:Picked"));
    EXPECT_EQ(identifier.filePath, "Assets/Materials/Picked.mat");
}

#endif
TEST(ReflectedPropertyDrawerTests, UnityCoreVector2AndColorWidgetsWriteBackToOriginalObject)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;
    NLS::UI::Internal::WidgetContainer root;

    EXPECT_GE(
        NLS::Editor::Panels::DrawReflectedObject(
            root,
            NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {})),
        19);
    ASSERT_EQ(root.GetWidgets().size(), 1u);

    auto* columns = dynamic_cast<NLS::UI::Widgets::Columns*>(root.GetWidgets().front().first);
    ASSERT_NE(columns, nullptr);

    auto& widgets = columns->GetWidgets();
    ASSERT_GE(widgets.size(), 4u);

    auto* vectorWidget = dynamic_cast<NLS::UI::Widgets::DragMultipleScalars<float, 2>*>(widgets[1].first);
    ASSERT_NE(vectorWidget, nullptr);

    auto* vectorDispatcher = vectorWidget->GetPlugin<NLS::UI::DataDispatcher<std::array<float, 2>>>();
    ASSERT_NE(vectorDispatcher, nullptr);
    vectorDispatcher->NotifyChange();
    vectorDispatcher->Provide({2.0f, 3.0f});

    auto* colorWidget = dynamic_cast<NLS::UI::Widgets::ColorEdit*>(widgets[3].first);
    ASSERT_NE(colorWidget, nullptr);

    auto* colorDispatcher = colorWidget->GetPlugin<NLS::UI::DataDispatcher<NLS::Maths::Color>>();
    ASSERT_NE(colorDispatcher, nullptr);
    colorDispatcher->NotifyChange();
    colorDispatcher->Provide({0.1f, 0.2f, 0.3f, 0.4f});

    EXPECT_FLOAT_EQ(object.uvScale.x, 2.0f);
    EXPECT_FLOAT_EQ(object.uvScale.y, 3.0f);
    EXPECT_FLOAT_EQ(object.tint.r, 0.1f);
    EXPECT_FLOAT_EQ(object.tint.g, 0.2f);
    EXPECT_FLOAT_EQ(object.tint.b, 0.3f);
    EXPECT_FLOAT_EQ(object.tint.a, 0.4f);
}

TEST(ReflectedPropertyDrawerTests, OwnedVariantDispatchersOutliveCallerAndStillWriteBack)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;
    NLS::UI::Internal::WidgetContainer root;

    EXPECT_GE(
        NLS::Editor::Panels::DrawReflectedObject(
            root,
            NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {})),
        19);

    auto* columns = dynamic_cast<NLS::UI::Widgets::Columns*>(root.GetWidgets().front().first);
    ASSERT_NE(columns, nullptr);
    ASSERT_GE(columns->GetWidgets().size(), 2u);

    auto* vectorWidget = dynamic_cast<NLS::UI::Widgets::DragMultipleScalars<float, 2>*>(columns->GetWidgets()[1].first);
    ASSERT_NE(vectorWidget, nullptr);

    auto* vectorDispatcher = vectorWidget->GetPlugin<NLS::UI::DataDispatcher<std::array<float, 2>>>();
    ASSERT_NE(vectorDispatcher, nullptr);
    vectorDispatcher->NotifyChange();
    vectorDispatcher->Provide({6.0f, 7.0f});

    EXPECT_FLOAT_EQ(object.uvScale.x, 6.0f);
    EXPECT_FLOAT_EQ(object.uvScale.y, 7.0f);
}

TEST(ReflectedPropertyDrawerTests, RectAndBoundsWidgetsWriteBackToOriginalObject)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;
    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& viewport = type.GetField("viewport");
    const auto& bounds = type.GetField("bounds");

    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        viewport));
    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        bounds));
    ASSERT_EQ(root.GetWidgets().size(), 6u);

    auto* rectWidget = dynamic_cast<NLS::UI::Widgets::DragMultipleScalars<float, 4>*>(root.GetWidgets()[1].first);
    ASSERT_NE(rectWidget, nullptr);
    auto* rectDispatcher = rectWidget->GetPlugin<NLS::UI::DataDispatcher<std::array<float, 4>>>();
    ASSERT_NE(rectDispatcher, nullptr);
    rectDispatcher->NotifyChange();
    rectDispatcher->Provide({10.0f, 20.0f, 30.0f, 40.0f});

    auto* centerWidget = dynamic_cast<NLS::UI::Widgets::DragMultipleScalars<float, 3>*>(root.GetWidgets()[3].first);
    ASSERT_NE(centerWidget, nullptr);
    auto* centerDispatcher = centerWidget->GetPlugin<NLS::UI::DataDispatcher<std::array<float, 3>>>();
    ASSERT_NE(centerDispatcher, nullptr);
    centerDispatcher->NotifyChange();
    centerDispatcher->Provide({7.0f, 8.0f, 9.0f});

    auto* extentsLabel = dynamic_cast<NLS::UI::Widgets::Text*>(root.GetWidgets()[4].first);
    ASSERT_NE(extentsLabel, nullptr);
    EXPECT_NE(extentsLabel->content.find("Extents"), std::string::npos);

    auto* extentsWidget = dynamic_cast<NLS::UI::Widgets::DragMultipleScalars<float, 3>*>(root.GetWidgets()[5].first);
    ASSERT_NE(extentsWidget, nullptr);
    auto* extentsDispatcher = extentsWidget->GetPlugin<NLS::UI::DataDispatcher<std::array<float, 3>>>();
    ASSERT_NE(extentsDispatcher, nullptr);
    EXPECT_EQ(extentsDispatcher->Gather(), (std::array<float, 3> {2.0f, 2.5f, 3.0f}));
    extentsDispatcher->NotifyChange();
    extentsDispatcher->Provide({3.0f, 4.0f, 5.0f});

    EXPECT_FLOAT_EQ(object.viewport.x, 10.0f);
    EXPECT_FLOAT_EQ(object.viewport.y, 20.0f);
    EXPECT_FLOAT_EQ(object.viewport.width, 30.0f);
    EXPECT_FLOAT_EQ(object.viewport.height, 40.0f);
    EXPECT_FLOAT_EQ(object.bounds.center.x, 7.0f);
    EXPECT_FLOAT_EQ(object.bounds.center.y, 8.0f);
    EXPECT_FLOAT_EQ(object.bounds.center.z, 9.0f);
    EXPECT_FLOAT_EQ(object.bounds.size.x, 6.0f);
    EXPECT_FLOAT_EQ(object.bounds.size.y, 8.0f);
    EXPECT_FLOAT_EQ(object.bounds.size.z, 10.0f);
}

TEST(ReflectedPropertyDrawerTests, DragWidgetsEnableClickToInputBeforeDrag)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);

    NLS::UI::Widgets::DragSingleScalar<float> scalarWidget(
        ImGuiDataType_Float,
        -10.0f,
        10.0f,
        0.0f,
        1.0f,
        "",
        "%.3f");
    EXPECT_TRUE(scalarWidget.enableClickToInput);

    NLS::UI::Widgets::DragMultipleScalars<float, 4> vector4Widget(
        ImGuiDataType_Float,
        -10.0f,
        10.0f,
        0.0f,
        1.0f,
        "",
        "%.3f");
    EXPECT_TRUE(vector4Widget.enableClickToInput);

    {
        NLS::UI::Internal::WidgetContainer root;
        const auto& uvScale = type.GetField("uvScale");

        ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
            root,
            NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
            uvScale));
        ASSERT_EQ(root.GetWidgets().size(), 2u);

        auto* vector2Widget = dynamic_cast<NLS::UI::Widgets::DragMultipleScalars<float, 2>*>(root.GetWidgets()[1].first);
        ASSERT_NE(vector2Widget, nullptr);
        EXPECT_TRUE(vector2Widget->enableClickToInput);
    }

    {
        NLS::UI::Internal::WidgetContainer root;
        const auto& bounds = type.GetField("bounds");

        ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
            root,
            NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
            bounds));
        ASSERT_EQ(root.GetWidgets().size(), 4u);

        auto* centerWidget = dynamic_cast<NLS::UI::Widgets::DragMultipleScalars<float, 3>*>(root.GetWidgets()[1].first);
        ASSERT_NE(centerWidget, nullptr);
        EXPECT_TRUE(centerWidget->enableClickToInput);

        auto* extentsWidget = dynamic_cast<NLS::UI::Widgets::DragMultipleScalars<float, 3>*>(root.GetWidgets()[3].first);
        ASSERT_NE(extentsWidget, nullptr);
        EXPECT_TRUE(extentsWidget->enableClickToInput);
    }
}

TEST(ReflectedPropertyDrawerTests, DragTextInputDetectionIgnoresGlobalTextInputCursorState)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    ImGuiContext& context = *ImGui::GetCurrentContext();
    const ImGuiID previousActiveId = context.ActiveId;
    const ImGuiID previousTempInputId = context.TempInputId;
    const ImGuiID previousInputTextId = context.InputTextState.ID;
    const bool previousWantTextInput = context.IO.WantTextInput;
    const ImGuiMouseCursor previousMouseCursor = context.MouseCursor;

    constexpr ImGuiID dragItemId = 101;
    constexpr ImGuiID otherItemId = 202;
    context.ActiveId = otherItemId;
    context.TempInputId = otherItemId;
    context.InputTextState.ID = otherItemId;
    context.IO.WantTextInput = true;
    context.MouseCursor = ImGuiMouseCursor_TextInput;

    EXPECT_FALSE(NLS::UI::Widgets::DragScalarInternal::IsDragScalarTextInputActive(dragItemId));

    context.ActiveId = dragItemId;
    context.TempInputId = 0;
    context.InputTextState.ID = dragItemId;
    EXPECT_FALSE(NLS::UI::Widgets::DragScalarInternal::IsDragScalarTextInputActive(dragItemId));

    context.ActiveId = dragItemId;
    context.TempInputId = dragItemId;
    EXPECT_TRUE(NLS::UI::Widgets::DragScalarInternal::IsDragScalarTextInputActive(dragItemId));

    context.ActiveId = previousActiveId;
    context.TempInputId = previousTempInputId;
    context.InputTextState.ID = previousInputTextId;
    context.IO.WantTextInput = previousWantTextInput;
    context.MouseCursor = previousMouseCursor;
}

TEST(ReflectedPropertyDrawerTests, RangeMetadataUsesSliderWidgetsAndWritesBack)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorRangeFieldFixture object;
    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorRangeFieldFixture);
    const auto& normalized = type.GetField("normalized");
    const auto& priority = type.GetField("priority");

    ASSERT_TRUE(normalized.IsValid());
    ASSERT_TRUE(priority.IsValid());
    ASSERT_NE(normalized.GetMeta().GetProperty<NLS::meta::Range>(), nullptr);
    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        normalized));
    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        priority));
    ASSERT_EQ(root.GetWidgets().size(), 4u);

    auto* floatSlider = dynamic_cast<NLS::UI::Widgets::SliderFloat*>(root.GetWidgets()[1].first);
    ASSERT_NE(floatSlider, nullptr);
    EXPECT_FLOAT_EQ(floatSlider->min, 0.0f);
    EXPECT_FLOAT_EQ(floatSlider->max, 1.0f);
    auto* floatDispatcher = floatSlider->GetPlugin<NLS::UI::DataDispatcher<float>>();
    ASSERT_NE(floatDispatcher, nullptr);
    floatDispatcher->NotifyChange();
    floatDispatcher->Provide(0.75f);

    auto* intSlider = dynamic_cast<NLS::UI::Widgets::SliderInt*>(root.GetWidgets()[3].first);
    ASSERT_NE(intSlider, nullptr);
    EXPECT_EQ(intSlider->min, 0);
    EXPECT_EQ(intSlider->max, 5);
    auto* intDispatcher = intSlider->GetPlugin<NLS::UI::DataDispatcher<int>>();
    ASSERT_NE(intDispatcher, nullptr);
    intDispatcher->NotifyChange();
    intDispatcher->Provide(4);

    EXPECT_FLOAT_EQ(object.normalized, 0.75f);
    EXPECT_EQ(object.priority, 4);
}

TEST(ReflectedPropertyDrawerTests, GenericArrayFieldDrawsSizeAndElementWidgets)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;
    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& names = type.GetField("names");
    int layoutChanges = 0;
    NLS::Editor::Panels::ReflectedPropertyDrawerOptions options;
    options.onFieldLayoutChanged = [&layoutChanges, &names](const NLS::meta::Field& field)
    {
        EXPECT_EQ(field.GetName(), names.GetName());
        ++layoutChanges;
    };

    ASSERT_TRUE(names.IsValid());
    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        names,
        options));
    ASSERT_EQ(root.GetWidgets().size(), 2u);

    auto* arrayGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
    ASSERT_NE(arrayGroup, nullptr);
    ASSERT_EQ(arrayGroup->GetWidgets().size(), 1u);

    auto* foldout = dynamic_cast<NLS::UI::Widgets::TreeNode*>(arrayGroup->GetWidgets()[0].first);
    ASSERT_NE(foldout, nullptr);
    ASSERT_GE(foldout->GetWidgets().size(), 7u);

    auto* sizeWidget = dynamic_cast<NLS::UI::Widgets::DragSingleScalar<int>*>(foldout->GetWidgets()[1].first);
    ASSERT_NE(sizeWidget, nullptr);
    auto* sizeDispatcher = sizeWidget->GetPlugin<NLS::UI::DataDispatcher<int>>();
    ASSERT_NE(sizeDispatcher, nullptr);
    sizeDispatcher->NotifyChange();
    sizeDispatcher->Provide(3);

    EXPECT_EQ(object.names.size(), 3u);
    EXPECT_EQ(layoutChanges, 1);

    auto* firstElement = dynamic_cast<NLS::UI::Widgets::InputText*>(foldout->GetWidgets()[3].first);
    ASSERT_NE(firstElement, nullptr);
    auto* elementDispatcher = firstElement->GetPlugin<NLS::UI::DataDispatcher<std::string>>();
    ASSERT_NE(elementDispatcher, nullptr);
    elementDispatcher->NotifyChange();
    elementDispatcher->Provide("Renamed");

    EXPECT_EQ(object.names[0], "Renamed");

    auto* addButton = dynamic_cast<NLS::UI::Widgets::ButtonSmall*>(foldout->GetWidgets()[foldout->GetWidgets().size() - 2].first);
    ASSERT_NE(addButton, nullptr);
    addButton->ClickedEvent.Invoke();
    EXPECT_EQ(object.names.size(), 4u);
    EXPECT_EQ(layoutChanges, 2);

    auto* removeButton = dynamic_cast<NLS::UI::Widgets::ButtonSmall*>(foldout->GetWidgets()[foldout->GetWidgets().size() - 1].first);
    ASSERT_NE(removeButton, nullptr);
    removeButton->ClickedEvent.Invoke();
    EXPECT_EQ(object.names.size(), 3u);
    EXPECT_EQ(layoutChanges, 3);
}

TEST(ReflectedPropertyDrawerTests, GenericArrayFieldWritesBackByValueGetterSetterArray)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Engine::Components::MeshRenderer renderer;
    renderer.SetUserMatrixElement(0, 0, 1.0f);
    renderer.SetUserMatrixElement(0, 1, 2.0f);

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Engine::Components::MeshRenderer);
    const auto& userMatrixValues = type.GetField("userMatrixValues");

    ASSERT_TRUE(userMatrixValues.IsValid());
    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(renderer, NLS::meta::variant_policy::NoCopy {}),
        userMatrixValues));

    auto* arrayGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
    ASSERT_NE(arrayGroup, nullptr);
    auto* foldout = dynamic_cast<NLS::UI::Widgets::TreeNode*>(arrayGroup->GetWidgets()[0].first);
    ASSERT_NE(foldout, nullptr);
    ASSERT_GE(foldout->GetWidgets().size(), 4u);

    auto* firstElement = NLS::Editor::Panels::Tests::FindFirstWidgetOfType<NLS::UI::Widgets::DragSingleScalar<float>>(*foldout);
    ASSERT_NE(firstElement, nullptr);
    auto* dispatcher = firstElement->GetPlugin<NLS::UI::DataDispatcher<float>>();
    ASSERT_NE(dispatcher, nullptr);
    dispatcher->NotifyChange();
    dispatcher->Provide(42.0f);

    EXPECT_FLOAT_EQ(renderer.GetUserMatrixElement(0, 0), 42.0f);
    EXPECT_FLOAT_EQ(renderer.GetUserMatrixElement(0, 1), 2.0f);
}

TEST(ReflectedPropertyDrawerTests, GenericArrayElementGatherUsesSharedArrayState)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::CountingArrayFieldFixture object;

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::CountingArrayFieldFixture);
    const auto& values = type.GetField("values");

    ASSERT_TRUE(values.IsValid());
    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        values));
    EXPECT_EQ(object.getterCalls, 1);

    auto* arrayGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
    ASSERT_NE(arrayGroup, nullptr);
    auto* foldout = dynamic_cast<NLS::UI::Widgets::TreeNode*>(arrayGroup->GetWidgets()[0].first);
    ASSERT_NE(foldout, nullptr);

    for (const auto& [widget, memoryMode] : foldout->GetWidgets())
    {
        (void)memoryMode;
        if (auto* scalar = dynamic_cast<NLS::UI::Widgets::DragSingleScalar<float>*>(widget))
        {
            auto* dispatcher = scalar->GetPlugin<NLS::UI::DataDispatcher<float>>();
            ASSERT_NE(dispatcher, nullptr);
            (void)dispatcher->Gather();
        }
    }

    EXPECT_EQ(object.getterCalls, 1);
}

TEST(ReflectedPropertyDrawerTests, GenericArraySizeGatherUsesSharedArrayState)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::CountingArrayFieldFixture object;

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::CountingArrayFieldFixture);
    const auto& values = type.GetField("values");

    ASSERT_TRUE(values.IsValid());
    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        values));
    EXPECT_EQ(object.getterCalls, 1);

    auto* arrayGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
    ASSERT_NE(arrayGroup, nullptr);
    auto* foldout = dynamic_cast<NLS::UI::Widgets::TreeNode*>(arrayGroup->GetWidgets()[0].first);
    ASSERT_NE(foldout, nullptr);
    auto* sizeWidget = dynamic_cast<NLS::UI::Widgets::DragSingleScalar<int>*>(foldout->GetWidgets()[1].first);
    ASSERT_NE(sizeWidget, nullptr);
    auto* sizeDispatcher = sizeWidget->GetPlugin<NLS::UI::DataDispatcher<int>>();
    ASSERT_NE(sizeDispatcher, nullptr);

    EXPECT_EQ(sizeDispatcher->Gather(), 3);
    EXPECT_EQ(object.getterCalls, 1);
}

TEST(ReflectedPropertyDrawerTests, GenericArrayFieldCapsAddAtInspectorSizeLimit)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;
    object.names.resize(1024u);

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& names = type.GetField("names");

    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        names));
    auto* arrayGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
    ASSERT_NE(arrayGroup, nullptr);
    auto* foldout = dynamic_cast<NLS::UI::Widgets::TreeNode*>(arrayGroup->GetWidgets()[0].first);
    ASSERT_NE(foldout, nullptr);

    auto* addButton = dynamic_cast<NLS::UI::Widgets::ButtonSmall*>(foldout->GetWidgets()[foldout->GetWidgets().size() - 2].first);
    ASSERT_NE(addButton, nullptr);
    addButton->ClickedEvent.Invoke();

    EXPECT_EQ(object.names.size(), 1024u);
}

TEST(ReflectedPropertyDrawerTests, GenericArrayFieldClampsSizeInputAtInspectorLimit)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& names = type.GetField("names");

    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        names));
    auto* arrayGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
    ASSERT_NE(arrayGroup, nullptr);
    auto* foldout = dynamic_cast<NLS::UI::Widgets::TreeNode*>(arrayGroup->GetWidgets()[0].first);
    ASSERT_NE(foldout, nullptr);

    auto* sizeWidget = dynamic_cast<NLS::UI::Widgets::DragSingleScalar<int>*>(foldout->GetWidgets()[1].first);
    ASSERT_NE(sizeWidget, nullptr);
    auto* sizeDispatcher = sizeWidget->GetPlugin<NLS::UI::DataDispatcher<int>>();
    ASSERT_NE(sizeDispatcher, nullptr);
    sizeDispatcher->NotifyChange();
    sizeDispatcher->Provide(2048);

    EXPECT_EQ(object.names.size(), 1024u);
}

TEST(ReflectedPropertyDrawerTests, ObjectReferenceArrayElementsCanBeClearedAndWriteBack)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Engine::Serialize::PersistentManager::Instance().Clear();

    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;
    NLS::Render::Resources::Material material;
    material.SetName("SkyMaterial");
    const auto identifier = NLS::Engine::Serialize::ObjectIdentifier::Asset(
        NLS::Engine::Serialize::AssetId(NLS::Guid::Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee")),
        12345,
        "Assets/Sky/SkyMaterial.mat");
    NLS::Engine::Serialize::PersistentManager::Instance().BindObjectIdentifier(material, identifier);
    object.materialReferences.emplace_back(&material);

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& materialReferences = type.GetField("materialReferences");

    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        materialReferences));
    auto* arrayGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
    ASSERT_NE(arrayGroup, nullptr);
    auto* foldout = dynamic_cast<NLS::UI::Widgets::TreeNode*>(arrayGroup->GetWidgets()[0].first);
    ASSERT_NE(foldout, nullptr);

    auto* referenceGroup = NLS::Editor::Panels::Tests::FindFirstWidgetOfType<NLS::UI::Widgets::Group>(*foldout);
    ASSERT_NE(referenceGroup, nullptr);
    auto* display = NLS::Editor::Panels::Tests::FindFirstWidgetOfType<NLS::UI::Widgets::Text>(*referenceGroup);
    ASSERT_NE(display, nullptr);
    EXPECT_NE(display->content.find("SkyMaterial.mat"), std::string::npos);
    auto* clearButton = NLS::Editor::Panels::Tests::FindFirstWidgetOfType<NLS::UI::Widgets::ButtonSmall>(*referenceGroup);
    ASSERT_NE(clearButton, nullptr);

    clearButton->ClickedEvent.Invoke();

    ASSERT_EQ(object.materialReferences.size(), 1u);
    EXPECT_EQ(object.materialReferences[0].GetInstanceID(), NLS::Engine::Serialize::InstanceID_None);
    auto* dispatcher = display->GetPlugin<NLS::UI::DataDispatcher<std::string>>();
    ASSERT_NE(dispatcher, nullptr);
    EXPECT_NE(dispatcher->Gather().find("Empty (Object)"), std::string::npos);
}

TEST(ReflectedPropertyDrawerTests, GenericArrayFieldDrawsSupportedElementEditors)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);

    {
        NLS::UI::Internal::WidgetContainer root;
        const auto& toggles = type.GetField("toggles");

        ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
            root,
            NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
            toggles));
        auto* arrayGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
        ASSERT_NE(arrayGroup, nullptr);
        auto* foldout = dynamic_cast<NLS::UI::Widgets::TreeNode*>(arrayGroup->GetWidgets()[0].first);
        ASSERT_NE(foldout, nullptr);

        auto* toggleWidget = dynamic_cast<NLS::UI::Widgets::AWidget*>(foldout->GetWidgets()[3].first);
        ASSERT_NE(toggleWidget, nullptr);
        auto* toggleDispatcher = toggleWidget->GetPlugin<NLS::UI::DataDispatcher<bool>>();
        ASSERT_NE(toggleDispatcher, nullptr);
        toggleDispatcher->NotifyChange();
        toggleDispatcher->Provide(false);

        EXPECT_FALSE(object.toggles[0]);
    }

    {
        NLS::UI::Internal::WidgetContainer root;
        const auto& points = type.GetField("points");

        ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
            root,
            NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
            points));
        auto* arrayGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
        ASSERT_NE(arrayGroup, nullptr);
        auto* foldout = dynamic_cast<NLS::UI::Widgets::TreeNode*>(arrayGroup->GetWidgets()[0].first);
        ASSERT_NE(foldout, nullptr);

        auto* vectorWidget = dynamic_cast<NLS::UI::Widgets::DragMultipleScalars<float, 3>*>(foldout->GetWidgets()[3].first);
        ASSERT_NE(vectorWidget, nullptr);
        auto* vectorDispatcher = vectorWidget->GetPlugin<NLS::UI::DataDispatcher<std::array<float, 3>>>();
        ASSERT_NE(vectorDispatcher, nullptr);
        vectorDispatcher->NotifyChange();
        vectorDispatcher->Provide({4.0f, 5.0f, 6.0f});

        EXPECT_FLOAT_EQ(object.points[0].x, 4.0f);
        EXPECT_FLOAT_EQ(object.points[0].y, 5.0f);
        EXPECT_FLOAT_EQ(object.points[0].z, 6.0f);
    }

    {
        NLS::UI::Internal::WidgetContainer root;
        const auto& swatches = type.GetField("swatches");

        ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
            root,
            NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
            swatches));
        auto* arrayGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
        ASSERT_NE(arrayGroup, nullptr);
        auto* foldout = dynamic_cast<NLS::UI::Widgets::TreeNode*>(arrayGroup->GetWidgets()[0].first);
        ASSERT_NE(foldout, nullptr);

        auto* colorWidget = dynamic_cast<NLS::UI::Widgets::ColorEdit*>(foldout->GetWidgets()[3].first);
        ASSERT_NE(colorWidget, nullptr);
        auto* colorDispatcher = colorWidget->GetPlugin<NLS::UI::DataDispatcher<NLS::Maths::Color>>();
        ASSERT_NE(colorDispatcher, nullptr);
        colorDispatcher->NotifyChange();
        colorDispatcher->Provide({0.9f, 0.8f, 0.7f, 0.6f});

        EXPECT_FLOAT_EQ(object.swatches[0].r, 0.9f);
        EXPECT_FLOAT_EQ(object.swatches[0].g, 0.8f);
        EXPECT_FLOAT_EQ(object.swatches[0].b, 0.7f);
        EXPECT_FLOAT_EQ(object.swatches[0].a, 0.6f);
    }
}

TEST(ReflectedPropertyDrawerTests, GenericArrayFieldClassifiesReflectedValueAndStdVectorElementsAsArrays)
{
    NLS::meta::ReflectionDatabase::Instance();
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorNestedArrayFixture);
    const auto& entries = type.GetField("entries");
    const auto& vectorEntries = type.GetField("vectorEntries");

    ASSERT_TRUE(entries.IsValid());
    ASSERT_TRUE(vectorEntries.IsValid());
    EXPECT_TRUE(entries.GetType().IsArray());
    EXPECT_TRUE(vectorEntries.GetType().IsArray());
    EXPECT_EQ(entries.GetType().GetArrayType(), NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorNestedValueElementFixture));
    EXPECT_EQ(vectorEntries.GetType().GetArrayType(), NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorNestedValueElementFixture));
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(entries),
        NLS::Editor::Panels::ReflectedPropertySupport::Array);
    EXPECT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(vectorEntries),
        NLS::Editor::Panels::ReflectedPropertySupport::Array);
}

TEST(ReflectedPropertyDrawerTests, GenericArrayFieldRecursivelyDrawsReflectedValueElementsAndWritesBack)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorNestedArrayFixture object;

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorNestedArrayFixture);
    const auto& entries = type.GetField("entries");

    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        entries));

    auto* elementFoldout = NLS::Editor::Panels::Tests::FindTreeNodeByName(root, "Element 0");
    ASSERT_NE(elementFoldout, nullptr);
    auto* countWidget =
        NLS::Editor::Panels::Tests::FindFirstWidgetOfType<NLS::UI::Widgets::DragSingleScalar<int>>(*elementFoldout);
    ASSERT_NE(countWidget, nullptr);
    auto* countDispatcher = countWidget->GetPlugin<NLS::UI::DataDispatcher<int>>();
    ASSERT_NE(countDispatcher, nullptr);

    EXPECT_EQ(countDispatcher->Gather(), 3);
    countDispatcher->NotifyChange();
    countDispatcher->Provide(11);

    ASSERT_EQ(object.entries.size(), 1u);
    EXPECT_EQ(object.entries[0].count, 11);
    EXPECT_FLOAT_EQ(object.entries[0].weight, 4.0f);
}

TEST(ReflectedPropertyDrawerTests, GenericStdVectorArrayFieldRecursivelyWritesBackReflectedValueElements)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorNestedArrayFixture object;

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorNestedArrayFixture);
    const auto& vectorEntries = type.GetField("vectorEntries");

    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        vectorEntries));

    auto* elementFoldout = NLS::Editor::Panels::Tests::FindTreeNodeByName(root, "Element 0");
    ASSERT_NE(elementFoldout, nullptr);
    auto* weightWidget =
        NLS::Editor::Panels::Tests::FindFirstWidgetOfType<NLS::UI::Widgets::DragSingleScalar<float>>(*elementFoldout);
    ASSERT_NE(weightWidget, nullptr);
    auto* weightDispatcher = weightWidget->GetPlugin<NLS::UI::DataDispatcher<float>>();
    ASSERT_NE(weightDispatcher, nullptr);

    EXPECT_FLOAT_EQ(weightDispatcher->Gather(), 6.0f);
    weightDispatcher->NotifyChange();
    weightDispatcher->Provide(8.5f);

    ASSERT_EQ(object.vectorEntries.size(), 1u);
    EXPECT_EQ(object.vectorEntries[0].count, 5);
    EXPECT_FLOAT_EQ(object.vectorEntries[0].weight, 8.5f);
}

TEST(ReflectedPropertyDrawerTests, GenericArrayFieldRecursivelyDrawsNestedReflectedValueFields)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorNestedArrayFixture object;

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorNestedArrayFixture);
    const auto& entries = type.GetField("entries");

    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        entries));

    auto* childFoldout = NLS::Editor::Panels::Tests::FindTreeNodeByName(root, "Child");
    ASSERT_NE(childFoldout, nullptr);
    auto* nestedScalar =
        NLS::Editor::Panels::Tests::FindFirstWidgetOfType<NLS::UI::Widgets::DragSingleScalar<int>>(*childFoldout);
    ASSERT_NE(nestedScalar, nullptr);
    auto* nestedDispatcher = nestedScalar->GetPlugin<NLS::UI::DataDispatcher<int>>();
    ASSERT_NE(nestedDispatcher, nullptr);

    EXPECT_EQ(nestedDispatcher->Gather(), 9);
    nestedDispatcher->NotifyChange();
    nestedDispatcher->Provide(13);

    ASSERT_EQ(object.entries.size(), 1u);
    EXPECT_EQ(object.entries[0].child.depthCount, 13);
    EXPECT_EQ(object.entries[0].count, 3);
    EXPECT_FLOAT_EQ(object.entries[0].weight, 4.0f);
}

TEST(ReflectedPropertyDrawerTests, GenericArrayFieldShowsFallbackForUnsupportedPointerElements)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorRawPointerArrayFixture object;

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorRawPointerArrayFixture);
    const auto& pointers = type.GetField("pointers");

    ASSERT_EQ(
        NLS::Editor::Panels::GetReflectedPropertySupport(pointers),
        NLS::Editor::Panels::ReflectedPropertySupport::Array);
    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        pointers));

    EXPECT_NE(NLS::Editor::Panels::Tests::FindTextContaining(root, "unsupported"), nullptr);
}

TEST(ReflectedPropertyDrawerTests, GenericArrayFieldStopsRecursiveValueExpansionAtCycleFallback)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorRecursiveArrayFixture object;
    object.roots.push_back({});
    object.roots[0].children.push_back({});

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorRecursiveArrayFixture);
    const auto& roots = type.GetField("roots");

    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        roots));

    EXPECT_NE(NLS::Editor::Panels::Tests::FindTreeNodeByName(root, "Element 0"), nullptr);
    EXPECT_NE(NLS::Editor::Panels::Tests::FindTextContaining(root, "recursive"), nullptr);
}

TEST(ReflectedPropertyDrawerTests, LayerMaskWidgetWritesBackMask)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;
    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& visibleLayers = type.GetField("visibleLayers");

    ASSERT_TRUE(visibleLayers.IsValid());
    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        visibleLayers));
    ASSERT_EQ(root.GetWidgets().size(), 2u);

    auto* layerGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
    ASSERT_NE(layerGroup, nullptr);
    ASSERT_EQ(layerGroup->GetWidgets().size(), NLS::Editor::Settings::TagLayerSettings::LayerCount);

    NLS::UI::Widgets::AWidget* layer4Toggle = nullptr;
    for (const auto& [widget, memoryMode] : layerGroup->GetWidgets())
    {
        (void)memoryMode;
        auto* checkBox = dynamic_cast<NLS::UI::Widgets::CheckBox*>(widget);
        if (checkBox != nullptr && checkBox->label == "4: Water")
        {
            layer4Toggle = checkBox;
            break;
        }
    }
    ASSERT_NE(layer4Toggle, nullptr);
    auto* dispatcher = layer4Toggle->GetPlugin<NLS::UI::DataDispatcher<bool>>();
    ASSERT_NE(dispatcher, nullptr);
    dispatcher->NotifyChange();
    dispatcher->Provide(true);

    EXPECT_EQ(object.visibleLayers.GetMask(), 0b10011u);
}

TEST(ReflectedPropertyDrawerTests, LayerMaskWidgetExposesEmptySlotsSoHiddenBitsCanBeCleared)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();
    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;
    object.visibleLayers.SetMask(1u << 3u);

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& visibleLayers = type.GetField("visibleLayers");

    ASSERT_TRUE(visibleLayers.IsValid());
    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        visibleLayers));

    auto* layerGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
    ASSERT_NE(layerGroup, nullptr);

    NLS::UI::Widgets::CheckBox* layer3Toggle = nullptr;
    for (const auto& [widget, memoryMode] : layerGroup->GetWidgets())
    {
        (void)memoryMode;
        auto* checkBox = dynamic_cast<NLS::UI::Widgets::CheckBox*>(widget);
        if (checkBox != nullptr && checkBox->label == "3: Layer 3")
        {
            layer3Toggle = checkBox;
            break;
        }
    }

    ASSERT_NE(layer3Toggle, nullptr);
    auto* dispatcher = layer3Toggle->GetPlugin<NLS::UI::DataDispatcher<bool>>();
    ASSERT_NE(dispatcher, nullptr);
    EXPECT_TRUE(dispatcher->Gather());

    dispatcher->NotifyChange();
    dispatcher->Provide(false);

    EXPECT_EQ(object.visibleLayers.GetMask(), 0u);
}

TEST(ReflectedPropertyDrawerTests, ObjectReferenceWidgetDisplaysAndClearsScenePointers)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();

    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("Linked Actor");
    auto* component = actor.AddComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(component, nullptr);

    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;
    object.targetGameObject = &actor;
    object.targetComponent = component;

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& gameObjectField = type.GetField("targetGameObject");

    ASSERT_TRUE(gameObjectField.IsValid());
    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        gameObjectField));
    ASSERT_EQ(root.GetWidgets().size(), 2u);

    auto* objectGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
    ASSERT_NE(objectGroup, nullptr);
    ASSERT_EQ(objectGroup->GetWidgets().size(), 3u);

    auto* display = dynamic_cast<NLS::UI::Widgets::Text*>(objectGroup->GetWidgets()[0].first);
    ASSERT_NE(display, nullptr);
    EXPECT_NE(display->content.find("Assigned (GameObject)"), std::string::npos);

    auto* clearButton = dynamic_cast<NLS::UI::Widgets::ButtonSmall*>(objectGroup->GetWidgets()[2].first);
    ASSERT_NE(clearButton, nullptr);
    clearButton->ClickedEvent.Invoke();

    EXPECT_EQ(object.targetGameObject, nullptr);
    EXPECT_EQ(object.targetComponent, component);
}

TEST(ReflectedPropertyDrawerTests, ObjectReferenceWidgetAcceptsHierarchyGameObjectDropForScenePointers)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();

    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("Dragged Actor");
    auto* component = actor.AddComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(component, nullptr);

    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;
    int changedFields = 0;
    NLS::Editor::Panels::ReflectedPropertyDrawerOptions options;
    options.onFieldChanged = [&changedFields](const NLS::meta::Field&) { ++changedFields; };

    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& gameObjectField = type.GetField("targetGameObject");
    const auto& componentField = type.GetField("targetComponent");

    {
        NLS::UI::Internal::WidgetContainer root;
        ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
            root,
            NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
            gameObjectField,
            options));

        auto* objectGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
        ASSERT_NE(objectGroup, nullptr);
        auto* display = NLS::Editor::Panels::Tests::FindFirstWidgetOfType<NLS::UI::Widgets::Text>(*objectGroup);
        ASSERT_NE(display, nullptr);
        auto* sceneTarget =
            display->GetPlugin<NLS::UI::DDTarget<std::pair<NLS::Engine::GameObject*, NLS::UI::Widgets::TreeNode*>>>();
        ASSERT_NE(sceneTarget, nullptr);

        sceneTarget->DataReceivedEvent.Invoke({&actor, nullptr});
        EXPECT_EQ(object.targetGameObject, &actor);
    }

    {
        NLS::UI::Internal::WidgetContainer root;
        ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
            root,
            NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
            componentField,
            options));

        auto* objectGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
        ASSERT_NE(objectGroup, nullptr);
        auto* display = NLS::Editor::Panels::Tests::FindFirstWidgetOfType<NLS::UI::Widgets::Text>(*objectGroup);
        ASSERT_NE(display, nullptr);
        auto* sceneTarget =
            display->GetPlugin<NLS::UI::DDTarget<std::pair<NLS::Engine::GameObject*, NLS::UI::Widgets::TreeNode*>>>();
        ASSERT_NE(sceneTarget, nullptr);

        sceneTarget->DataReceivedEvent.Invoke({&actor, nullptr});
        EXPECT_EQ(object.targetComponent, component);
    }

    EXPECT_EQ(changedFields, 2);
}

TEST(ReflectedPropertyDrawerTests, ObjectReferenceWidgetResolvesHierarchyDropToConcreteComponentPointer)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();

    NLS::Engine::SceneSystem::Scene scene;
    auto& lightActor = scene.CreateGameObject("Light Actor");
    auto* light = lightActor.AddComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(light, nullptr);
    auto& plainActor = scene.CreateGameObject("Plain Actor");

    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;
    int changedFields = 0;
    NLS::Editor::Panels::ReflectedPropertyDrawerOptions options;
    options.onFieldChanged = [&changedFields](const NLS::meta::Field& field)
    {
        if (field.GetName() == "targetLightComponent")
            ++changedFields;
    };

    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& lightField = type.GetField("targetLightComponent");
    ASSERT_TRUE(lightField.IsValid());
    EXPECT_EQ(lightActor.GetComponent(lightField.GetType().GetDecayedType(), true), light);

    NLS::UI::Internal::WidgetContainer root;
    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        lightField,
        options));

    auto* objectGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
    ASSERT_NE(objectGroup, nullptr);
    auto* display = NLS::Editor::Panels::Tests::FindFirstWidgetOfType<NLS::UI::Widgets::Text>(*objectGroup);
    ASSERT_NE(display, nullptr);
    auto* sceneTarget =
        display->GetPlugin<NLS::UI::DDTarget<std::pair<NLS::Engine::GameObject*, NLS::UI::Widgets::TreeNode*>>>();
    ASSERT_NE(sceneTarget, nullptr);

    sceneTarget->DataReceivedEvent.Invoke({&lightActor, nullptr});
    EXPECT_EQ(object.targetLightComponent, light);
    EXPECT_EQ(changedFields, 1);

    object.targetLightComponent = nullptr;
    sceneTarget->DataReceivedEvent.Invoke({&plainActor, nullptr});
    EXPECT_EQ(object.targetLightComponent, nullptr);
    EXPECT_EQ(changedFields, 1);
}

TEST(ReflectedPropertyDrawerTests, ObjectReferenceWidgetUsesReflectedPointerTypeWithoutInspectorComponentWhitelist)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();

    NLS::Engine::SceneSystem::Scene scene;
    auto& materialActor = scene.CreateGameObject("Material Actor");
    auto* MeshRenderer = materialActor.AddComponent<NLS::Engine::Components::MeshRenderer>();
    ASSERT_NE(MeshRenderer, nullptr);

    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& field = type.GetField("targetMeshRenderer");
    ASSERT_TRUE(field.IsValid());
    EXPECT_EQ(materialActor.GetComponent(field.GetType().GetDecayedType(), true), MeshRenderer);

    NLS::UI::Internal::WidgetContainer root;
    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
        field));

    auto* objectGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
    ASSERT_NE(objectGroup, nullptr);
    auto* display = NLS::Editor::Panels::Tests::FindFirstWidgetOfType<NLS::UI::Widgets::Text>(*objectGroup);
    ASSERT_NE(display, nullptr);
    auto* sceneTarget =
        display->GetPlugin<NLS::UI::DDTarget<std::pair<NLS::Engine::GameObject*, NLS::UI::Widgets::TreeNode*>>>();
    ASSERT_NE(sceneTarget, nullptr);

    sceneTarget->DataReceivedEvent.Invoke({&materialActor, nullptr});
    EXPECT_EQ(object.targetMeshRenderer, MeshRenderer);

    auto* clearButton = NLS::Editor::Panels::Tests::FindButtonSmallByLabel(*objectGroup, "x");
    ASSERT_NE(clearButton, nullptr);
    clearButton->ClickedEvent.Invoke();
    EXPECT_EQ(object.targetMeshRenderer, nullptr);
}

TEST(ReflectedPropertyDrawerTests, ObjectReferenceWidgetAcceptsHierarchyDropForConstScenePointers)
{
    NLS::Editor::Panels::Tests::ScopedImGuiContext imguiContext;
    NLS::meta::ReflectionDatabase::Instance();

    NLS::Engine::SceneSystem::Scene scene;
    auto& actor = scene.CreateGameObject("Const Target");
    auto* component = actor.AddComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(component, nullptr);

    NLS::Editor::Panels::Tests::InspectorCoreFieldFixture object;
    const auto type = NLS_TYPEOF(NLS::Editor::Panels::Tests::InspectorCoreFieldFixture);
    const auto& gameObjectField = type.GetField("constTargetGameObject");
    const auto& componentField = type.GetField("constTargetComponent");
    ASSERT_TRUE(gameObjectField.IsValid());
    ASSERT_TRUE(componentField.IsValid());

    {
        NLS::UI::Internal::WidgetContainer root;
        ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
            root,
            NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
            gameObjectField));

        auto* objectGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
        ASSERT_NE(objectGroup, nullptr);
        auto* display = NLS::Editor::Panels::Tests::FindFirstWidgetOfType<NLS::UI::Widgets::Text>(*objectGroup);
        ASSERT_NE(display, nullptr);
        auto* sceneTarget =
            display->GetPlugin<NLS::UI::DDTarget<std::pair<NLS::Engine::GameObject*, NLS::UI::Widgets::TreeNode*>>>();
        ASSERT_NE(sceneTarget, nullptr);

        sceneTarget->DataReceivedEvent.Invoke({&actor, nullptr});
        EXPECT_EQ(object.constTargetGameObject, &actor);
    }

    {
        NLS::UI::Internal::WidgetContainer root;
        ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
            root,
            NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {}),
            componentField));

        auto* objectGroup = dynamic_cast<NLS::UI::Widgets::Group*>(root.GetWidgets()[1].first);
        ASSERT_NE(objectGroup, nullptr);
        auto* display = NLS::Editor::Panels::Tests::FindFirstWidgetOfType<NLS::UI::Widgets::Text>(*objectGroup);
        ASSERT_NE(display, nullptr);
        auto* sceneTarget =
            display->GetPlugin<NLS::UI::DDTarget<std::pair<NLS::Engine::GameObject*, NLS::UI::Widgets::TreeNode*>>>();
        ASSERT_NE(sceneTarget, nullptr);

        sceneTarget->DataReceivedEvent.Invoke({&actor, nullptr});
        EXPECT_EQ(object.constTargetComponent, component);

        auto* clearButton = NLS::Editor::Panels::Tests::FindButtonSmallByLabel(*objectGroup, "x");
        ASSERT_NE(clearButton, nullptr);
        clearButton->ClickedEvent.Invoke();
        EXPECT_EQ(object.constTargetComponent, nullptr);
    }
}

TEST(ReflectedPropertyDrawerTests, ReflectedSettingFieldWritesBackToOriginalObject)
{
    NLS::meta::ReflectionDatabase::Instance();
    auto& object = NLS::Editor::Settings::EditorSettings::GetSceneToolSettingsObject();
    object.translationSnapUnit = 1.0f;

    auto instance = NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {});
    const auto type = NLS_TYPEOF(NLS::Editor::Settings::EditorSceneToolSettingsObject);
    const auto& field = type.GetField("translationSnapUnit");
    float updatedValue = 2.0f;
    auto updatedVariant = NLS::meta::Variant(updatedValue, NLS::meta::variant_policy::NoCopy {});

    ASSERT_TRUE(field.SetValue(instance, updatedVariant));

    EXPECT_FLOAT_EQ(object.translationSnapUnit, 2.0f);
}

TEST(ReflectedPropertyDrawerTests, ReflectedObjectFieldWidgetWritesBackToOriginalObject)
{
    NLS::meta::ReflectionDatabase::Instance();
    auto& object = NLS::Editor::Settings::EditorSettings::GetSceneToolSettingsObject();
    object.translationSnapUnit = 1.0f;
    object.rotationSnapUnit = 15.0f;
    object.scalingSnapUnit = 1.0f;

    NLS::UI::Internal::WidgetContainer root;

    EXPECT_EQ(
        NLS::Editor::Panels::DrawReflectedObject(
            root,
            NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {})),
        3);
    ASSERT_EQ(root.GetWidgets().size(), 1u);

    auto* columns = dynamic_cast<NLS::UI::Widgets::Columns*>(root.GetWidgets().front().first);
    ASSERT_NE(columns, nullptr);

    auto& widgets = columns->GetWidgets();
    ASSERT_GE(widgets.size(), 4u);

    auto* rotationWidget = dynamic_cast<NLS::UI::Widgets::DragSingleScalar<float>*>(widgets[3].first);
    ASSERT_NE(rotationWidget, nullptr);

    auto* dispatcher = rotationWidget->GetPlugin<NLS::UI::DataDispatcher<float>>();
    ASSERT_NE(dispatcher, nullptr);

    dispatcher->NotifyChange();
    dispatcher->Provide(30.0f);

    EXPECT_FLOAT_EQ(object.translationSnapUnit, 1.0f);
    EXPECT_FLOAT_EQ(object.rotationSnapUnit, 30.0f);
    EXPECT_FLOAT_EQ(object.scalingSnapUnit, 1.0f);
}

TEST(ReflectedPropertyDrawerTests, EnumFieldWidgetReadsUnderlyingUint8EnumValue)
{
    NLS::meta::ReflectionDatabase::Instance();

    NLS::Engine::SceneSystem::Scene scene;
    auto& lightActor = scene.CreateGameObject("Ambient Light");
    auto* light = lightActor.AddComponent<NLS::Engine::Components::LightComponent>();
    ASSERT_NE(light, nullptr);
    light->SetLightType(NLS::Render::Settings::ELightType::AMBIENT_SPHERE);

    NLS::UI::Internal::WidgetContainer root;
    const auto type = NLS_TYPEOF(NLS::Engine::Components::LightComponent);
    const auto& field = type.GetField("lightType");

    ASSERT_TRUE(field.IsValid());
    ASSERT_TRUE(NLS::Editor::Panels::DrawReflectedField(
        root,
        NLS::meta::Variant(*light, NLS::meta::variant_policy::NoCopy {}),
        field));
    ASSERT_EQ(root.GetWidgets().size(), 2u);

    auto* combo = dynamic_cast<NLS::UI::Widgets::ComboBox*>(root.GetWidgets()[1].first);
    ASSERT_NE(combo, nullptr);
    EXPECT_EQ(combo->currentChoice, static_cast<int>(NLS::Render::Settings::ELightType::AMBIENT_SPHERE));
}

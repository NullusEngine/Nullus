#include <gtest/gtest.h>

#include <Reflection/ReflectionDatabase.h>
#include <UI/Plugins/DataDispatcher.h>
#include <UI/Widgets/Drags/DragSingleScalar.h>
#include <UI/Widgets/Layout/Columns.h>

#include "Panels/ReflectedPropertyDrawer.h"
#include "Settings/EditorSettings.h"

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
    auto instance = NLS::meta::Variant(object, NLS::meta::variant_policy::NoCopy {});

    EXPECT_EQ(NLS::Editor::Panels::DrawReflectedObject(root, instance), 3);
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

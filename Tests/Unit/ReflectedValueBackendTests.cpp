#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "Inspector/ReflectedEditHost.h"
#include "Inspector/ReflectedPropertyEditor.h"
#include "UI/UIDocumentNodeEdit.h"
#include "UIDocumentDesignerTestSupport.h"

using namespace NLS;
using namespace NLS::Editor::Inspector;
using namespace NLS::Editor::UIDocumentTools;
using NLS::Editor::Serialization::SerializedObject;
using NLS::Editor::Serialization::SerializedTargetSet;

namespace
{
    SerializedTargetSet MakeTargets(const size_t count)
    {
        SerializedTargetSet targets;
        for (size_t index = 0u; index < count; ++index)
        {
            targets.targets.push_back(InspectorTargetHandle::FromAsset(
                InspectorTargetKind::AssetImporter,
                "reflected-test[" + std::to_string(index) + "]",
                std::to_string(index),
                "ReflectedProperties"));
        }
        return targets;
    }
}

TEST(ReflectedValueBackendTests, ContentPropertyTraversalVisitsEveryPathOnce)
{
    EnsureUIDocumentInspectorReflection();
    const auto lifetime = std::make_shared<int>(0);
    auto backend = std::make_shared<ReflectedValueBackend>(meta::Variant(UIDocumentContentEdit {}), lifetime);
    SerializedObject object(MakeTargets(1u), backend);
    ASSERT_TRUE(object.IsValid()) << object.GetLastError();

    std::vector<std::string> paths;
    auto iterator = object.GetIterator();
    bool enterChildren = true;
    while (iterator.NextVisible(enterChildren))
    {
        const std::string path(iterator.GetPropertyPath());
        ASSERT_EQ(std::find(paths.begin(), paths.end(), path), paths.end()) << path;
        ASSERT_LT(paths.size(), 64u);
        paths.push_back(path);
        enterChildren = iterator.HasChildren();
    }
    EXPECT_NE(std::find(paths.begin(), paths.end(), "Value.texture.subAssetKey"), paths.end());
    EXPECT_NE(std::find(paths.begin(), paths.end(), "Value.font.resourcePath"), paths.end());
    EXPECT_NE(std::find(paths.begin(), paths.end(), "Value.nineSlice.bottom"), paths.end());
    EXPECT_NE(std::find(paths.begin(), paths.end(), "Value.requiresRenderTarget"), paths.end());
}

TEST(ReflectedValueBackendTests, MultipleValuesShowMixedFieldsAndApplyToEveryTarget)
{
    EnsureUIDocumentInspectorReflection();
    const auto lifetime = std::make_shared<int>(0);

    UIDocumentStyleEdit first;
    first.opacity = 0.25f;
    first.visible = true;
    UIDocumentStyleEdit second;
    second.opacity = 1.0f;
    second.visible = true;

    std::vector<meta::Variant> values { meta::Variant(first), meta::Variant(second) };
    auto backend = std::make_shared<ReflectedValueBackend>(values, lifetime);
    SerializedObject object(MakeTargets(2u), backend);
    ASSERT_TRUE(object.IsValid()) << object.GetLastError();

    auto opacity = object.FindProperty("Value.opacity");
    ASSERT_TRUE(opacity.IsValid());
    EXPECT_TRUE(opacity.HasMultipleDifferentValues());

    auto visible = object.FindProperty("Value.visible");
    ASSERT_TRUE(visible.IsValid());
    EXPECT_FALSE(visible.HasMultipleDifferentValues());

    ASSERT_TRUE(opacity.SetFloatValue(0.5f));
    ASSERT_TRUE(object.ApplyModifiedPropertiesWithoutUndo());
    object.Update();

    opacity = object.FindProperty("Value.opacity");
    ASSERT_TRUE(opacity.IsValid());
    EXPECT_FALSE(opacity.HasMultipleDifferentValues());

    const auto& backendValues = backend->Values();
    ASSERT_EQ(backendValues.size(), 2u);
    EXPECT_FLOAT_EQ(
        backendValues[0].GetValue<UIDocumentStyleEdit>().opacity,
        0.5f);
    EXPECT_FLOAT_EQ(
        backendValues[1].GetValue<UIDocumentStyleEdit>().opacity,
        0.5f);
    // Fields that were already equal keep their values on every target.
    EXPECT_TRUE(backendValues[0].GetValue<UIDocumentStyleEdit>().visible);
    EXPECT_TRUE(backendValues[1].GetValue<UIDocumentStyleEdit>().visible);
    EXPECT_FLOAT_EQ(
        backendValues[1].GetValue<UIDocumentStyleEdit>().color.a,
        1.0f);
}

TEST(ReflectedValueBackendTests, SetSourcesRefreshesTheMixedState)
{
    EnsureUIDocumentInspectorReflection();
    const auto lifetime = std::make_shared<int>(0);

    UIDocumentStyleEdit first;
    first.opacity = 0.5f;
    UIDocumentStyleEdit second;
    second.opacity = 0.5f;
    auto backend = std::make_shared<ReflectedValueBackend>(
        std::vector<meta::Variant> { meta::Variant(first), meta::Variant(second) },
        lifetime);
    SerializedObject object(MakeTargets(2u), backend);
    ASSERT_TRUE(object.IsValid());
    EXPECT_FALSE(object.FindProperty("Value.opacity").HasMultipleDifferentValues());

    second.opacity = 0.75f;
    backend->SetSources(
        std::vector<meta::Variant> { meta::Variant(first), meta::Variant(second) });
    object.Update();

    ASSERT_TRUE(object.FindProperty("Value.opacity").IsValid());
    EXPECT_TRUE(object.FindProperty("Value.opacity").HasMultipleDifferentValues());
    EXPECT_EQ(backend->Values().size(), 2u);
    EXPECT_FLOAT_EQ(backend->Value().GetValue<UIDocumentStyleEdit>().opacity, 0.5f);
}

TEST(ReflectedValueBackendTests, SingleValueBackendKeepsTheExistingBehavior)
{
    EnsureUIDocumentInspectorReflection();
    const auto lifetime = std::make_shared<int>(0);
    UIDocumentElementEdit value;
    value.name = "Header";
    auto backend = std::make_shared<ReflectedValueBackend>(meta::Variant(value), lifetime);
    SerializedObject object(MakeTargets(1u), backend);
    ASSERT_TRUE(object.IsValid());
    EXPECT_EQ(backend->Values().size(), 1u);

    auto name = object.FindProperty("Value.name");
    ASSERT_TRUE(name.IsValid());
    EXPECT_FALSE(name.HasMultipleDifferentValues());
    ASSERT_TRUE(name.SetStringValue("Title"));
    ASSERT_TRUE(object.ApplyModifiedPropertiesWithoutUndo());
    object.Update();
    EXPECT_EQ(
        backend->Value().GetValue<UIDocumentElementEdit>().name,
        "Title");
}

namespace
{
    class NullEditHost final : public IReflectedEditHost
    {
    public:
        [[nodiscard]] bool BeginPropertyEdit(std::string_view) override { open = true; ++begins; return true; }
        [[nodiscard]] bool PreviewPropertyEdit() override { return true; }
        [[nodiscard]] bool CommitPropertyEdit() override { open = false; return true; }
        [[nodiscard]] bool CancelPropertyEdit() override { open = false; return true; }
        [[nodiscard]] bool IsPropertyEditOpen() const override { return open; }
        bool open = false;
        int begins = 0;
    };
}

TEST(ReflectedValueBackendTests, EditorDrawsMixedValuesWithoutStartingAnEdit)
{
    using namespace NLS::Editor::UIToolsTest;

    EnsureUIDocumentInspectorReflection();
    ScopedImGuiContext imgui;
    const auto lifetime = std::make_shared<int>(0);

    ReflectedPropertyEditor editor;
    editor.SetLifetime(lifetime);
    NullEditHost host;

    UIDocumentStyleEdit first;
    first.opacity = 0.25f;
    UIDocumentStyleEdit second;
    second.opacity = 1.0f;
    std::vector<UIDocumentStyleEdit> values { first, second };

    const auto draw = [&]()
    {
        (void)DrawReflectedValues(
            editor,
            host,
            "ReflectedStyleDraw",
            "Style",
            values,
            {},
            [](const UIDocumentStyleEdit&, const std::vector<std::string>&) {});
    };
    RenderFrame(draw);
    RenderFrame(draw);

    EXPECT_EQ(host.begins, 0);
    EXPECT_FALSE(host.IsPropertyEditOpen());
    EXPECT_FLOAT_EQ(values[0].opacity, 0.25f);
    EXPECT_FLOAT_EQ(values[1].opacity, 1.0f);
    (void)editor.FinishFrame();
}

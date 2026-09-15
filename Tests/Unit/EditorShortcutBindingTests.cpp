#include <gtest/gtest.h>

#include "Shortcuts/EditorShortcutBinding.h"

using namespace NLS::Editor::Shortcuts;
using namespace NLS::Windowing::Inputs;

TEST(EditorShortcutBindingTests, FormatsModifiersInStableDisplayOrder)
{
    const auto binding = ShortcutBinding::FromKey(
        EKey::KEY_S,
        EShortcutModifier::Shift | EShortcutModifier::Ctrl | EShortcutModifier::Alt);

#if defined(__APPLE__)
    // macOS keeps its native modifier order: Cmd, Shift, Alt, Ctrl.
    EXPECT_EQ(ToDisplayString(binding), "Shift + Alt + Ctrl + S");
#else
    EXPECT_EQ(ToDisplayString(binding), "Ctrl + Shift + Alt + S");
#endif
}

TEST(EditorShortcutBindingTests, FormatsLogicalPrimaryForTheCurrentPlatform)
{
    const auto binding = ShortcutBinding::FromKey(
        EKey::KEY_S,
        EShortcutModifier::Primary | EShortcutModifier::Shift);

#if defined(__APPLE__)
    EXPECT_EQ(ToDisplayString(binding), "Cmd + Shift + S");
#else
    EXPECT_EQ(ToDisplayString(binding), "Ctrl + Shift + S");
#endif
}

TEST(EditorShortcutBindingTests, KeepsPhysicalModifierBindingsDistinctInStorage)
{
    const auto primary = ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Primary);
    const auto ctrl = ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl);
    const auto super = ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Super);

    EXPECT_NE(primary, ctrl);
    EXPECT_NE(primary, super);

#if defined(__APPLE__)
    EXPECT_TRUE(AreShortcutBindingsEquivalentForCurrentPlatform(primary, super));
    EXPECT_FALSE(AreShortcutBindingsEquivalentForCurrentPlatform(primary, ctrl));
#else
    EXPECT_TRUE(AreShortcutBindingsEquivalentForCurrentPlatform(primary, ctrl));
    EXPECT_FALSE(AreShortcutBindingsEquivalentForCurrentPlatform(primary, super));
#endif
}

TEST(EditorShortcutBindingTests, RejectsUnassignedAndModifierOnlyBindings)
{
    EXPECT_FALSE(ShortcutBinding::Unassigned().IsValid());
    EXPECT_FALSE(ShortcutBinding::FromKey(EKey::KEY_LEFT_CONTROL).IsValid());
    EXPECT_FALSE(ShortcutBinding::FromKey(EKey::KEY_RIGHT_SHIFT).IsValid());
}

TEST(EditorShortcutBindingTests, ComparesEquivalentBindings)
{
    const auto first = ShortcutBinding::FromKey(EKey::KEY_N, EShortcutModifier::Ctrl);
    const auto second = ShortcutBinding::FromKey(EKey::KEY_N, EShortcutModifier::Ctrl);
    const auto third = ShortcutBinding::FromKey(EKey::KEY_N, EShortcutModifier::Shift);

    EXPECT_EQ(first, second);
    EXPECT_NE(first, third);
}

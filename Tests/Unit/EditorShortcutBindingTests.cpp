#include <gtest/gtest.h>

#include "Shortcuts/EditorShortcutBinding.h"

using namespace NLS::Editor::Shortcuts;
using namespace NLS::Windowing::Inputs;

TEST(EditorShortcutBindingTests, FormatsModifiersInStableDisplayOrder)
{
    const auto binding = ShortcutBinding::FromKey(
        EKey::KEY_S,
        EShortcutModifier::Shift | EShortcutModifier::Ctrl | EShortcutModifier::Alt);

    EXPECT_EQ(ToDisplayString(binding), "Ctrl + Shift + Alt + S");
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

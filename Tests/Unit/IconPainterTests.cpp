#include <gtest/gtest.h>

#include "Icons/EditorTheme.h"
#include "Icons/IconPainter.h"

namespace
{
using namespace NLS::Editor::Icons;
}

TEST(IconPainterTests, TargetPixelsCombineLayoutAndFramebufferScale)
{
    EXPECT_EQ(ComputeIconTargetPixels(16.0f, IconMetrics{ 1.0f, 1.0f }), 16u);
    EXPECT_EQ(ComputeIconTargetPixels(16.0f, IconMetrics{ 1.5f, 1.0f }), 24u)
        << "Windows 150% content scale";
    EXPECT_EQ(ComputeIconTargetPixels(16.0f, IconMetrics{ 1.0f, 2.0f }), 32u)
        << "Retina framebuffer scale with uiScale fixed to 1";
    EXPECT_EQ(ComputeIconTargetPixels(16.0f, IconMetrics{ 1.5f, 2.0f }), 48u)
        << "the product is the physical target; callers must not multiply again";
    EXPECT_EQ(ComputeIconTargetPixels(16.0f, IconMetrics{ 0.0f, 0.0f }), 16u)
        << "invalid metrics fall back to 1x";
    EXPECT_EQ(ComputeIconTargetPixels(1.0f, IconMetrics{ 0.75f, 1.0f }), 1u)
        << "never returns zero";
}

TEST(IconPainterTests, ThemeColorsCoverAllStates)
{
    const auto& theme = GetEditorIconTheme();
    EXPECT_EQ(IconStateColor(IconState::Normal, true), theme.primary);
    EXPECT_EQ(IconStateColor(IconState::Hovered, true), theme.primary);
    EXPECT_EQ(IconStateColor(IconState::Pressed, true), theme.secondary);
    EXPECT_EQ(IconStateColor(IconState::Selected, true), theme.accent);
    EXPECT_EQ(IconStateColor(IconState::Disabled, true), theme.disabled);
    EXPECT_EQ(IconStateColor(IconState::Danger, true), theme.danger);
}

TEST(IconPainterTests, ColorIconsIgnoreThemeExceptForDisabled)
{
    const auto normal = IconStateColor(IconState::Selected, false);
    const auto disabled = IconStateColor(IconState::Disabled, false);
    EXPECT_EQ(normal, 0xffffffffu);
    EXPECT_EQ(disabled, 0x5affffffu)
        << "disabled color icons use a fixed alpha, not the monochrome token";
}

#include <gtest/gtest.h>

#include "ImGui/imgui.h"
#include "UI/Icons/FontAwesomeIconFont.h"

TEST(FontAwesomeIconFontTests, LoadsSearchGlyphIntoCurrentFontAtlas)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ASSERT_TRUE(NLS::UI::Icons::EnsureFontAwesomeIconFontLoaded());

    ImGuiIO& io = ImGui::GetIO();
    ASSERT_NE(io.Fonts->Fonts.Size, 0);
    ASSERT_TRUE(io.Fonts->Build());
    EXPECT_NE(io.Fonts->Fonts[0]->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomeSearchGlyph)), nullptr);
    EXPECT_NE(io.Fonts->Fonts[0]->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomeTimesGlyph)), nullptr);
    EXPECT_NE(io.Fonts->Fonts[0]->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomeCaretDownGlyph)), nullptr);
    EXPECT_NE(io.Fonts->Fonts[0]->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomeCaretRightGlyph)), nullptr);
    EXPECT_NE(io.Fonts->Fonts[0]->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomePaintBrushGlyph)), nullptr);
    EXPECT_NE(io.Fonts->Fonts[0]->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomePlayGlyph)), nullptr);
    EXPECT_NE(io.Fonts->Fonts[0]->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomePauseGlyph)), nullptr);
    EXPECT_NE(io.Fonts->Fonts[0]->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomeStopGlyph)), nullptr);
    EXPECT_NE(io.Fonts->Fonts[0]->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomeStepForwardGlyph)), nullptr);
    EXPECT_NE(io.Fonts->Fonts[0]->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomeRefreshGlyph)), nullptr);

    ImGui::DestroyContext();
}

TEST(FontAwesomeIconFontTests, LoadsSearchGlyphIntoRequestedFont)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->AddFontDefault();
    ImFont* secondFont = io.Fonts->AddFontDefault();
    ASSERT_NE(secondFont, nullptr);

    ASSERT_TRUE(NLS::UI::Icons::EnsureFontAwesomeIconFontLoaded(13.0f, secondFont));

    ASSERT_TRUE(io.Fonts->Build());
    EXPECT_NE(secondFont->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomeSearchGlyph)), nullptr);
    EXPECT_NE(secondFont->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomeTimesGlyph)), nullptr);
    EXPECT_NE(secondFont->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomeCaretDownGlyph)), nullptr);
    EXPECT_NE(secondFont->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomeCaretRightGlyph)), nullptr);
    EXPECT_NE(secondFont->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomePaintBrushGlyph)), nullptr);
    EXPECT_NE(secondFont->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomePlayGlyph)), nullptr);
    EXPECT_NE(secondFont->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomePauseGlyph)), nullptr);
    EXPECT_NE(secondFont->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomeStopGlyph)), nullptr);
    EXPECT_NE(secondFont->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomeStepForwardGlyph)), nullptr);
    EXPECT_NE(secondFont->FindGlyphNoFallback(static_cast<ImWchar>(NLS::UI::Icons::kFontAwesomeRefreshGlyph)), nullptr);

    ImGui::DestroyContext();
}

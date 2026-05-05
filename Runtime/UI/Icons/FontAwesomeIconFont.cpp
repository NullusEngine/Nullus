#include "UI/Icons/FontAwesomeIconFont.h"

#include "UI/ImGuiExtensions/TimelineProfiler/IconsFontAwesome4_data.h"

namespace NLS::UI::Icons
{
bool EnsureFontAwesomeIconFontLoaded(const float pixelSize, ImFont* targetFont)
{
    if (ImGui::GetCurrentContext() == nullptr)
        return false;

    ImGuiIO& io = ImGui::GetIO();
    if (targetFont == nullptr && !io.Fonts->Fonts.empty())
        targetFont = io.Fonts->Fonts[0];

    if (targetFont != nullptr &&
        targetFont->FindGlyphNoFallback(kFontAwesomeSearchGlyph) != nullptr)
    {
        return true;
    }

    if (io.Fonts->Locked)
        return false;

    if (io.Fonts->Fonts.empty())
        io.Fonts->AddFontDefault();
    if (targetFont == nullptr)
        targetFont = io.Fonts->Fonts[0];

    ImFontConfig iconConfig;
    iconConfig.MergeMode = true;
    iconConfig.PixelSnapH = true;
    iconConfig.GlyphMinAdvanceX = pixelSize;
    iconConfig.DstFont = targetFont;
    static const ImWchar iconRanges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    io.Fonts->AddFontFromMemoryCompressedTTF(
        font_awesome_compressed_data,
        static_cast<int>(font_awesome_compressed_size),
        pixelSize,
        &iconConfig,
        iconRanges);
    io.Fonts->Build();

    return targetFont->FindGlyphNoFallback(kFontAwesomeSearchGlyph) != nullptr;
}
}

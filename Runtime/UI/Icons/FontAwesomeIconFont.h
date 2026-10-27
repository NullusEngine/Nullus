#pragma once

#include <cstdint>
#include <vector>

#include <UI/UIDef.h>

#include "UI/ImGuiExtensions/TimelineProfiler/IconsFontAwesome4.h"

struct ImFont;
using ImWchar = uint16_t;

namespace NLS::UI::Icons
{
constexpr ImWchar kFontAwesomeTimesGlyph = ICON_MIN_FA + 0xd;
constexpr ImWchar kFontAwesomeSearchGlyph = ICON_MIN_FA + 0x2;
constexpr ImWchar kFontAwesomeCaretDownGlyph = ICON_MIN_FA + 0xd7;
constexpr ImWchar kFontAwesomeCaretRightGlyph = ICON_MIN_FA + 0xda;
constexpr ImWchar kFontAwesomePaintBrushGlyph = ICON_MIN_FA + 0x1fc;
constexpr ImWchar kFontAwesomePlayGlyph = ICON_MIN_FA + 0x4b;
constexpr ImWchar kFontAwesomePauseGlyph = ICON_MIN_FA + 0x4c;
constexpr ImWchar kFontAwesomeStopGlyph = ICON_MIN_FA + 0x4d;
constexpr ImWchar kFontAwesomeStepForwardGlyph = ICON_MIN_FA + 0x51;
constexpr ImWchar kFontAwesomeRefreshGlyph = ICON_MIN_FA + 0x21;

/**
* Merges the subset of FontAwesome glyphs requested by the application into the
* target font. An empty codepoint list keeps the legacy full-range behavior.
* The application (editor/launcher) owns the policy; Runtime/UI only consumes it.
*/
NLS_UI_API bool EnsureFontAwesomeIconFontLoaded(float pixelSize = 13.0f, ImFont* targetFont = nullptr);
NLS_UI_API bool EnsureFontAwesomeIconFontLoaded(
    float pixelSize,
    ImFont* targetFont,
    const std::vector<ImWchar>& codepoints);

/**
* Converts a sorted, unique codepoint list into ImGui glyph range pairs
* terminated with 0. Contiguous codepoints are merged into one range.
*/
NLS_UI_API std::vector<ImWchar> BuildFontIconRanges(const std::vector<ImWchar>& codepoints);
}

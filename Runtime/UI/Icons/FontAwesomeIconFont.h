#pragma once

#include <cstdint>

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

NLS_UI_API bool EnsureFontAwesomeIconFontLoaded(float pixelSize = 13.0f, ImFont* targetFont = nullptr);
}

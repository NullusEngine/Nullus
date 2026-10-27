#pragma once
#include "UI/Controls/Control.h"
#include <string>

namespace NLS::UI::Controls
{
struct KeyChord
{
    ImGuiKey key = ImGuiKey_None;
    bool ctrl = false, shift = false, alt = false, super = false;
};
struct KeyChordState
{
    bool recording = false;
};
NLS_UI_API std::string KeyChordLabel(const KeyChord& chord);
NLS_UI_API ControlResult KeyChordField(const char* id, KeyChord& chord, KeyChordState& state,
                                       const ControlOptions& options = {});
} // namespace NLS::UI::Controls

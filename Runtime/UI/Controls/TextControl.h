#pragma once
#include <string>
#include <limits>
#include "UI/Controls/Control.h"

namespace NLS::UI::Controls
{
NLS_UI_API ControlResult Text(const char* id, std::string& value, bool multiline = false,
                              const ControlOptions& options = {}, ImVec2 size = {}, ImGuiInputTextFlags flags = 0,
                              size_t maximumBytes = (std::numeric_limits<size_t>::max)());
NLS_UI_API ControlResult Toggle(const char* id, bool& value, const ControlOptions& options = {});
} // namespace NLS::UI::Controls

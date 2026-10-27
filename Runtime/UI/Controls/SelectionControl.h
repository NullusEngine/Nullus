#pragma once
#include <span>
#include <string>
#include "UI/Controls/Control.h"

namespace NLS::UI::Controls
{
struct SelectionItem
{
    std::string label, tooltip;
    bool enabled = true;
};
struct SelectionState
{
    std::string search;
};
NLS_UI_API ControlResult Selection(const char* id, int& selected, std::span<const SelectionItem> items,
                                   SelectionState& state, const ControlOptions& options = {});
NLS_UI_API ControlResult Flags(const char* id, uint64_t& value, std::span<const uint64_t> masks,
                               std::span<const SelectionItem> items, const ControlOptions& options = {});
} // namespace NLS::UI::Controls

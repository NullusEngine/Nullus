#pragma once
#include "UI/UIDef.h"

#include <cstdint>
#include <algorithm>
#include <imgui.h>

namespace NLS::UI::Controls
{
// Values and interaction only. Controls never own an edited document or Undo.
struct ControlResult
{
    bool started = false, active = false, changed = false, completed = false, canceled = false;
    uint64_t components = 0;

    ControlResult& Merge(const ControlResult& other)
    {
        started |= other.started;
        active |= other.active;
        changed |= other.changed;
        completed |= other.completed;
        canceled |= other.canceled;
        components |= other.components;
        return *this;
    }
};

struct ControlOptions
{
    bool readOnly = false;
    bool mixed = false;
    float scale = 1.f;
};

inline void MixedLabel(const ControlOptions& options)
{
    if (!options.mixed)
        return;
    const float width = ImGui::CalcItemWidth();
    const float marker = ImGui::CalcTextSize("Mixed").x + ImGui::GetStyle().ItemSpacing.x;
    ImGui::TextDisabled("Mixed");
    ImGui::SameLine();
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth((std::max)(1.f, (std::min)(available, width - marker)));
}

inline ControlResult ItemResult(bool changed)
{
    ControlResult result{ImGui::IsItemActivated(),
                         ImGui::IsItemActive(),
                         changed,
                         ImGui::IsItemDeactivatedAfterEdit(),
                         (ImGui::IsItemActive() || ImGui::IsItemDeactivated()) && ImGui::IsKeyPressed(ImGuiKey_Escape),
                         changed ? 1ull : 0ull};
    if (result.canceled)
        result.completed = false;
    return result;
}

// Includes all children, not only the last item. Popup lifetime belongs to the
// control, so callers must not finish a gesture while active is true.
inline void FinishComposite(ControlResult& result)
{
    if (result.active)
        result.completed = false;
    if (result.canceled)
        result.completed = false;
}
} // namespace NLS::UI::Controls

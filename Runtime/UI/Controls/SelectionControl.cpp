#include "UI/Controls/SelectionControl.h"
#include "UI/Controls/TextControl.h"
#include <algorithm>
#include <cctype>

namespace NLS::UI::Controls
{
namespace
{
std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    return text;
}
} // namespace
ControlResult Selection(const char* id, int& selected, std::span<const SelectionItem> items, SelectionState& state,
                        const ControlOptions& options)
{
    ControlResult result;
    ImGui::BeginDisabled(options.readOnly);
    const char* label = options.mixed ? "Mixed" : selected >= 0 && static_cast<size_t>(selected) < items.size() ? items[selected].label.c_str() :
                                                                                                                  "<Unknown>";
    if (ImGui::BeginCombo(id, label))
    {
        result.active = true;
        if (items.size() > 8)
            Text("##Search", state.search);
        const auto search = Lower(state.search);
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (!search.empty() && Lower(items[i].label).find(search) == std::string::npos)
                continue;
            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginDisabled(!items[i].enabled);
            if (ImGui::Selectable(items[i].label.c_str(), selected == static_cast<int>(i)))
            {
                selected = static_cast<int>(i);
                result.changed = result.completed = true;
                result.active = false;
                result.components = 1;
            }
            if (!items[i].tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", items[i].tooltip.c_str());
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        result.canceled = ImGui::IsKeyPressed(ImGuiKey_Escape);
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    return result;
}
ControlResult Flags(const char* id, uint64_t& value, std::span<const uint64_t> masks,
                    std::span<const SelectionItem> items, const ControlOptions& options)
{
    ControlResult result;
    std::string label;
    for (size_t i = 0; i < items.size() && i < masks.size(); ++i)
        if ((masks[i] == 0 && value == 0) || (masks[i] != 0 && (value & masks[i]) == masks[i]))
        {
            if (!label.empty())
                label += ", ";
            label += items[i].label;
        }
    if (label.empty())
        label = value ? "<Unknown flags>" : "None";
    ImGui::PushID(id);
    const auto openKey = ImGui::GetID("##FlagsWasOpen"), dirtyKey = ImGui::GetID("##FlagsWasEdited");
    ImGui::PopID();
    auto* storage = ImGui::GetStateStorage();
    const bool wasOpen = storage->GetInt(openKey) != 0;
    bool dirty = storage->GetInt(dirtyKey) != 0, open = false;
    ImGui::BeginDisabled(options.readOnly);
    if (ImGui::BeginCombo(id, options.mixed ? "Mixed" : label.c_str()))
    {
        open = true;
        result.active = true;
        for (size_t i = 0; i < items.size() && i < masks.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginDisabled(!items[i].enabled);
            const bool selected = masks[i] == 0 ? value == 0 : (value & masks[i]) == masks[i];
            if (ImGui::Selectable(items[i].label.c_str(), selected, ImGuiSelectableFlags_DontClosePopups))
            {
                value = masks[i] == 0 ? 0 : selected ? value & ~masks[i] :
                                                       value | masks[i];
                result.changed = true;
                result.components = 1;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        result.canceled = ImGui::IsKeyPressed(ImGuiKey_Escape);
        ImGui::EndCombo();
    }
    dirty |= result.changed;
    if (wasOpen && !open)
    {
        result.canceled |= ImGui::IsKeyPressed(ImGuiKey_Escape);
        result.completed = dirty && !result.canceled;
        dirty = false;
    }
    if (result.canceled)
        dirty = false;
    storage->SetInt(openKey, open ? 1 : 0);
    storage->SetInt(dirtyKey, dirty ? 1 : 0);
    ImGui::EndDisabled();
    return result;
}
} // namespace NLS::UI::Controls

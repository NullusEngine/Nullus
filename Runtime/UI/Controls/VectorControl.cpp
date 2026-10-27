#include "UI/Controls/VectorControl.h"
#include <algorithm>
#include <array>
#include <string>

namespace NLS::UI::Controls
{
namespace
{
const std::array<ComponentLabel, 4> axes{{{"X", "X axis", {.91f, .31f, .31f, 1}},
                                          {"Y", "Y axis", {.35f, .78f, .38f, 1}},
                                          {"Z", "Z axis", {.37f, .61f, .95f, 1}},
                                          {"W", "W component", {.74f, .67f, .94f, 1}}}};
}
ControlResult FloatComponents(const char* id, std::span<float> values, std::span<const ComponentLabel> labels,
                              float speed, const ControlOptions& options)
{
    MixedLabel(options);
    auto componentOptions = options;
    componentOptions.mixed = false;

    ControlResult result;
    if (values.empty())
        return result;
    if (labels.empty())
        labels = axes;
    const float available = (std::min)(ImGui::GetContentRegionAvail().x, ImGui::CalcItemWidth());
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float labelWidth = (std::max)(20.f * options.scale, ImGui::CalcTextSize("W").x + ImGui::GetStyle().FramePadding.x * 2);
    const bool stacked = available < values.size() * (labelWidth + 42.f * options.scale + spacing);
    const float width = stacked ? available : (available - spacing * (values.size() - 1)) / values.size();
    ImGui::PushID(id);
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (!stacked && i)
            ImGui::SameLine(0, spacing);
        ImGui::BeginGroup();
        ImGui::PushID(static_cast<int>(i));
        const auto& label = labels[i % labels.size()];
        const auto p = ImGui::GetCursorScreenPos();
        const auto text = ImGui::CalcTextSize(label.name);
        ImGui::InvisibleButton("##Label", {labelWidth, ImGui::GetFrameHeight()});
        ImGui::GetWindowDrawList()->AddRectFilled(
            p, ImGui::GetItemRectMax(), ImGui::GetColorU32({label.color.x, label.color.y, label.color.z, .2f}), ImGui::GetStyle().FrameRounding);
        ImGui::GetWindowDrawList()->AddText(
            {p.x + (labelWidth - text.x) * .5f, p.y + (ImGui::GetFrameHeight() - text.y) * .5f},
            ImGui::GetColorU32(label.color),
            label.name);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", label.tooltip);
        ImGui::SameLine(0, 0);
        ImGui::SetNextItemWidth((std::max)(1.f, width - labelWidth));
        ScalarOptions scalar;
        scalar.drag = true;
        scalar.speed = speed;
        auto component = Numeric("##Value", values[i], scalar, componentOptions);
        component.components = component.changed ? 1ull << i : 0;
        result.Merge(component);
        ImGui::PopID();
        ImGui::EndGroup();
    }
    ImGui::PopID();
    FinishComposite(result);
    return result;
}
ControlResult IntComponents(const char* id, std::span<int32_t> values, const ControlOptions& options)
{
    MixedLabel(options);
    auto componentOptions = options;
    componentOptions.mixed = false;

    ControlResult result;
    ImGui::PushID(id);
    for (size_t i = 0; i < values.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        auto item = Numeric(axes[i % 4].name, values[i], {}, componentOptions);
        item.components = item.changed ? 1ull << i : 0;
        result.Merge(item);
        ImGui::PopID();
    }
    ImGui::PopID();
    FinishComposite(result);
    return result;
}
ControlResult Color(const char* id, float* rgba, bool hdr, const ControlOptions& options, ImGuiColorEditFlags flags)
{
    MixedLabel(options);
    const std::array<float, 4> before{rgba[0], rgba[1], rgba[2], rgba[3]};
    const std::array<ComponentLabel, 4> labels{{{"R", "Red channel", {.91f, .31f, .31f, 1}},
                                                {"G", "Green channel", {.35f, .78f, .38f, 1}},
                                                {"B", "Blue channel", {.37f, .61f, .95f, 1}},
                                                {"A", "Alpha channel", {.82f, .84f, .88f, 1}}}};
    auto componentOptions = options;
    componentOptions.mixed = false;
    const float swatch = ImGui::GetFrameHeight(), spacing = ImGui::GetStyle().ItemSpacing.x;
    const float width = (std::min)(ImGui::CalcItemWidth(), ImGui::GetContentRegionAvail().x);
    const bool inlineSwatch = width >= swatch * 2 + spacing;
    const size_t count = (flags & ImGuiColorEditFlags_NoAlpha) ? 3 : 4;
    ImGui::BeginGroup();
    ImGui::PushItemWidth(inlineSwatch ? width - swatch - spacing : width);
    auto result = FloatComponents(id, std::span<float>(rgba, count), std::span<const ComponentLabel>(labels.data(), count), .01f, componentOptions);
    ImGui::PopItemWidth();
    ImGui::EndGroup();
    if (inlineSwatch)
        ImGui::SameLine(0, spacing);
    ImGui::SetNextItemWidth(swatch);
    ImGui::PushID(id);
    ImGui::BeginDisabled(options.readOnly);
    const bool changed = ImGui::ColorEdit4("##Picker", rgba, flags | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | (hdr ? ImGuiColorEditFlags_HDR : 0));
    result.Merge(ItemResult(changed));
    auto* storage = ImGui::GetStateStorage();
    const auto openKey = ImGui::GetID("##WasOpen"), dirtyKey = ImGui::GetID("##WasEdited");
    const bool wasOpen = storage->GetInt(openKey) != 0;
    ImGui::PushID("##Picker");
    const bool open = ImGui::IsPopupOpen("picker");
    ImGui::PopID();
    bool dirty = storage->GetInt(dirtyKey) != 0;
    dirty |= result.changed && (open || wasOpen);
    result.active |= open;
    if (wasOpen && !open)
    {
        result.canceled |= ImGui::IsKeyPressed(ImGuiKey_Escape);
        result.completed |= dirty && !result.canceled;
        dirty = false;
    }
    storage->SetInt(openKey, open ? 1 : 0);
    storage->SetInt(dirtyKey, dirty ? 1 : 0);
    result.components = 0;
    for (size_t i = 0; i < 4; ++i)
        if (before[i] != rgba[i])
            result.components |= 1ull << i;
    FinishComposite(result);
    ImGui::EndDisabled();
    ImGui::PopID();
    return result;
}
ControlResult Matrix(const char* id, std::span<float> values, size_t columns, const ControlOptions& options)
{
    MixedLabel(options);
    auto componentOptions = options;
    componentOptions.mixed = false;

    ControlResult result;
    if (!columns || columns > 4 || values.size() > 64)
        return result;
    // SetNextItemWidth is consumed by the first scalar. All later rows need
    // the same matrix width, including when embedded in a table label column.
    const float width = (std::min)(ImGui::CalcItemWidth(), ImGui::GetContentRegionAvail().x);
    ImGui::PushID(id);
    ImGui::PushItemWidth(width);
    for (size_t i = 0; i < values.size(); i += columns)
    {
        ImGui::PushID(static_cast<int>(i));
        auto row = FloatComponents("##Row", values.subspan(i, (std::min)(columns, values.size() - i)), {}, .01f, componentOptions);
        row.components <<= i;
        result.Merge(row);
        ImGui::PopID();
    }
    ImGui::PopItemWidth();
    ImGui::PopID();
    FinishComposite(result);
    return result;
}
} // namespace NLS::UI::Controls

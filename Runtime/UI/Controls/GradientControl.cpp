#include "UI/Controls/GradientControl.h"
#include "UI/Controls/ScalarControl.h"
#include "UI/Controls/VectorControl.h"
#include <algorithm>
#include <cmath>

namespace NLS::UI::Controls
{
std::array<float, 4> EvaluateGradient(const GradientData& gradient, float time)
{
    std::array<float, 4> color{1, 1, 1, 1};
    if (!gradient.colors.empty())
    {
        color = gradient.colors.front().color;
        for (size_t i = 1; i < gradient.colors.size(); ++i)
        {
            const auto& a = gradient.colors[i - 1];
            const auto& b = gradient.colors[i];
            if (time >= b.time)
            {
                color = b.color;
                continue;
            }
            const float t = gradient.mode == 1 ? 0 : std::clamp((time - a.time) / (std::max)(b.time - a.time, 1e-8f), 0.f, 1.f);
            for (size_t c = 0; c < 3; ++c)
                color[c] = a.color[c] + (b.color[c] - a.color[c]) * t;
            break;
        }
    }
    color[3] = gradient.alphas.empty() ? 1 : gradient.alphas.front().alpha;
    for (size_t i = 1; i < gradient.alphas.size(); ++i)
    {
        const auto& a = gradient.alphas[i - 1];
        const auto& b = gradient.alphas[i];
        if (time >= b.time)
        {
            color[3] = b.alpha;
            continue;
        }
        const float t = gradient.mode == 1 ? 0 : std::clamp((time - a.time) / (std::max)(b.time - a.time, 1e-8f), 0.f, 1.f);
        color[3] = a.alpha + (b.alpha - a.alpha) * t;
        break;
    }
    return color;
}
namespace
{
void Bar(const GradientData& data, ImVec2 p, ImVec2 size)
{
    auto* draw = ImGui::GetWindowDrawList();
    for (int i = 0; i < 128; ++i)
    {
        const auto color = EvaluateGradient(data, i / 127.f);
        const float checker = ((i / 8) % 2) ? .25f : .4f;
        ImVec4 visible{color[0] * color[3] + checker * (1 - color[3]), color[1] * color[3] + checker * (1 - color[3]), color[2] * color[3] + checker * (1 - color[3]), 1};
        draw->AddRectFilled({p.x + size.x * i / 128, p.y}, {p.x + size.x * (i + 1) / 128 + 1, p.y + size.y}, ImGui::GetColorU32(visible));
    }
}
template<class T>
void SortSelection(std::vector<T>& keys, int& selected)
{
    if (selected < 0 || static_cast<size_t>(selected) >= keys.size())
        return;
    const auto key = keys[selected];
    std::stable_sort(keys.begin(), keys.end(), [](const auto& a, const auto& b)
                     { return a.time < b.time; });
    selected = static_cast<int>(std::find(keys.begin(), keys.end(), key) - keys.begin());
}
} // namespace
ControlResult Gradient(const char* id, GradientData& gradient, GradientState& state, const ControlOptions& options)
{
    ControlResult result;
    ImGui::PushID(id);
    auto p = ImGui::GetCursorScreenPos();
    ImVec2 size{(std::max)(60.f, (std::min)(ImGui::CalcItemWidth(), ImGui::GetContentRegionAvail().x)),
                28 * options.scale};
    if (ImGui::InvisibleButton("##Preview", size))
        ImGui::OpenPopup("Gradient");
    Bar(gradient, p, size);
    ImGui::SetNextWindowSize({520 * options.scale, 320 * options.scale}, ImGuiCond_FirstUseEver);
    if (ImGui::BeginPopup("Gradient"))
    {
        result.active = true;
        state.wasOpen = true;
        p = ImGui::GetCursorScreenPos();
        size = {480 * options.scale, 64 * options.scale};
        ImGui::InvisibleButton("##Keys", size, ImGuiButtonFlags_MouseButtonLeft);
        const bool hovered = ImGui::IsItemHovered();
        const auto mouse = ImGui::GetIO().MousePos;
        Bar(gradient, {p.x, p.y + 16 * options.scale}, {size.x, 32 * options.scale});
        const auto marker = [&](float time, bool alpha, bool selected)
        {
            const ImVec2 center{p.x + time * size.x, p.y + (alpha ? 8.f : 56.f) * options.scale};
            ImGui::GetWindowDrawList()->AddCircleFilled(
                center, 6 * options.scale, selected ? IM_COL32(255, 195, 70, 255) : IM_COL32(205, 210, 225, 255));
            return std::hypot(center.x - mouse.x, center.y - mouse.y) < 9 * options.scale;
        };
        for (size_t i = 0; i < gradient.colors.size(); ++i)
            if (marker(gradient.colors[i].time, false, state.color == static_cast<int>(i)) && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                state.color = static_cast<int>(i);
                state.alpha = -1;
                state.dragging = 1;
            }
        for (size_t i = 0; i < gradient.alphas.size(); ++i)
            if (marker(gradient.alphas[i].time, true, state.alpha == static_cast<int>(i)) && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                state.alpha = static_cast<int>(i);
                state.color = -1;
                state.dragging = 2;
            }
        const float time = std::clamp((mouse.x - p.x) / size.x, 0.f, 1.f);
        ImGui::BeginDisabled(options.readOnly);
        if (!options.readOnly && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            if (state.dragging == 1 && state.color >= 0 && static_cast<size_t>(state.color) < gradient.colors.size())
            {
                gradient.colors[state.color].time = time;
                SortSelection(gradient.colors, state.color);
                result.changed = true;
            }
            if (state.dragging == 2 && state.alpha >= 0 && static_cast<size_t>(state.alpha) < gradient.alphas.size())
            {
                gradient.alphas[state.alpha].time = time;
                SortSelection(gradient.alphas, state.alpha);
                result.changed = true;
            }
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            state.dragging = 0;
        if (!options.readOnly && hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            const auto value = EvaluateGradient(gradient, time);
            if (mouse.y < p.y + 32 * options.scale)
            {
                gradient.alphas.push_back({time, value[3]});
                state.alpha = static_cast<int>(gradient.alphas.size() - 1);
                state.color = -1;
                SortSelection(gradient.alphas, state.alpha);
            }
            else
            {
                gradient.colors.push_back({time, value});
                state.color = static_cast<int>(gradient.colors.size() - 1);
                state.alpha = -1;
                SortSelection(gradient.colors, state.color);
            }
            result.changed = true;
        }
        if (ImGui::Button("Add color"))
        {
            gradient.colors.push_back({.5f, EvaluateGradient(gradient, .5f)});
            state.color = static_cast<int>(gradient.colors.size() - 1);
            state.alpha = -1;
            SortSelection(gradient.colors, state.color);
            result.changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Add alpha"))
        {
            gradient.alphas.push_back({.5f, 1});
            state.alpha = static_cast<int>(gradient.alphas.size() - 1);
            state.color = -1;
            SortSelection(gradient.alphas, state.alpha);
            result.changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete selected"))
        {
            if (state.color >= 0 && static_cast<size_t>(state.color) < gradient.colors.size())
            {
                gradient.colors.erase(gradient.colors.begin() + state.color);
                result.changed = true;
            }
            if (state.alpha >= 0 && static_cast<size_t>(state.alpha) < gradient.alphas.size())
            {
                gradient.alphas.erase(gradient.alphas.begin() + state.alpha);
                result.changed = true;
            }
            state.color = state.alpha = -1;
        }
        float minimum = 0, maximum = 1;
        ScalarOptions range;
        range.minimum = &minimum;
        range.maximum = &maximum;
        range.slider = true;
        if (state.color >= 0 && static_cast<size_t>(state.color) < gradient.colors.size())
        {
            result.changed |= Color("Color", gradient.colors[state.color].color.data(), true).changed;
            if (Numeric("Position", gradient.colors[state.color].time, range).changed)
            {
                SortSelection(gradient.colors, state.color);
                result.changed = true;
            }
        }
        if (state.alpha >= 0 && static_cast<size_t>(state.alpha) < gradient.alphas.size())
        {
            result.changed |= Numeric("Alpha", gradient.alphas[state.alpha].alpha, range).changed;
            if (Numeric("Position", gradient.alphas[state.alpha].time, range).changed)
            {
                SortSelection(gradient.alphas, state.alpha);
                result.changed = true;
            }
        }
        result.changed |= ImGui::Combo("Mode", &gradient.mode, "Blend\0Fixed\0");
        ImGui::EndDisabled();
        if (ImGui::Button("Done"))
        {
            result.completed = state.dirty || result.changed;
            result.active = false;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            result.canceled = true;
            result.active = false;
            ImGui::CloseCurrentPopup();
        }
        state.dirty |= result.changed;
        ImGui::EndPopup();
    }
    else if (state.wasOpen)
    {
        result.completed = state.dirty;
        state.wasOpen = state.dirty = false;
        state.dragging = 0;
    }
    if (result.changed)
        result.components = ~0ull;
    if (result.completed || result.canceled)
        state.dirty = false;
    ImGui::PopID();
    return result;
}
} // namespace NLS::UI::Controls

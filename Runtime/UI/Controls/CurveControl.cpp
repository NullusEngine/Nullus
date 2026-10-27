#include "UI/Controls/CurveControl.h"
#include "UI/Controls/ScalarControl.h"
#include "UI/Controls/SelectionControl.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <string>

namespace NLS::UI::Controls
{
float CurveViewport::X(double t, float width) const
{
    return static_cast<float>((t - timeMin) / (timeMax - timeMin)) * width;
}
float CurveViewport::Y(float v, float height) const
{
    return (1 - (v - valueMin) / (valueMax - valueMin)) * height;
}
double CurveViewport::Time(float x, float width) const
{
    return timeMin + x / (std::max)(1.f, width) * (timeMax - timeMin);
}
float CurveViewport::Value(float y, float height) const
{
    return valueMin + (1 - y / (std::max)(1.f, height)) * (valueMax - valueMin);
}
void CurveViewport::Fit(const CurveData& curve, size_t channel)
{
    timeMin = 0;
    timeMax = 1;
    valueMin = 0;
    valueMax = 1;
    for (const auto& key : curve.keys)
    {
        timeMin = (std::min)(timeMin, key.time);
        timeMax = (std::max)(timeMax, key.time);
        valueMin = (std::min)(valueMin, key.value[channel % 4]);
        valueMax = (std::max)(valueMax, key.value[channel % 4]);
    }
    const auto pad = (valueMax - valueMin) * .1f;
    valueMin -= pad;
    valueMax += pad;
}
void CurveViewport::Zoom(double time, float value, float factor)
{
    const double span = (timeMax - timeMin) * factor;
    const float range = (valueMax - valueMin) * factor;
    if (span < 1e-6 || span > 1e12 || range < 1e-6f || range > 1e12f)
        return;
    timeMin = time + (timeMin - time) * factor;
    timeMax = time + (timeMax - time) * factor;
    valueMin = value + (valueMin - value) * factor;
    valueMax = value + (valueMax - value) * factor;
}
float EvaluateCurve(const CurveData& curve, double time, size_t channel)
{
    if (curve.keys.empty())
        return 0;
    if (curve.wrap && curve.keys.size() > 1)
    {
        const double first = curve.keys.front().time, last = curve.keys.back().time, span = last - first;
        const int mode = time < first ? curve.preWrap : curve.postWrap;
        if (span > 0 && (time < first || time > last) && (mode == 2 || mode == 4))
        {
            const double period = span * (mode == 4 ? 2 : 1);
            auto offset = std::fmod(time - first, period);
            if (offset < 0)
                offset += period;
            time = first + (mode == 4 && offset > span ? period - offset : offset);
        }
    }
    channel %= 4;
    if (time <= curve.keys.front().time)
        return curve.keys.front().value[channel];
    for (size_t i = 1; i < curve.keys.size(); ++i)
    {
        const auto& a = curve.keys[i - 1];
        const auto& b = curve.keys[i];
        if (time > b.time)
            continue;
        const auto dt = b.time - a.time;
        if (dt <= 0)
            return b.value[channel];
        const float t = static_cast<float>((time - a.time) / dt);
        if (a.interpolation == 0)
            return a.value[channel] + (b.value[channel] - a.value[channel]) * t;
        if (std::isinf(a.outTangent[channel]) || std::isinf(b.inTangent[channel]))
            return a.value[channel];
        float u = t;
        // Weighted cubic handles use normalized time weights. Invert x(u)
        // monotonically before sampling y(u), matching the authored handles.
        const bool weighted = curve.weights && ((a.weightedMode & 2) || (b.weightedMode & 1));
        const float wa = (a.weightedMode & 2) ? std::clamp(a.outWeight, .0001f, .9999f) : 1.f / 3;
        const float wb = (b.weightedMode & 1) ? std::clamp(b.inWeight, .0001f, .9999f) : 1.f / 3;
        if (weighted)
        {
            float low = 0, high = 1;
            for (int n = 0; n < 24; ++n)
            {
                u = (low + high) * .5f;
                const float x = 3 * (1 - u) * (1 - u) * u * wa + 3 * (1 - u) * u * u * (1 - wb) + u * u * u;
                if (x < t)
                    low = u;
                else
                    high = u;
            }
            return (1 - u) * (1 - u) * (1 - u) * a.value[channel] + 3 * (1 - u) * (1 - u) * u * (a.value[channel] + a.outTangent[channel] * static_cast<float>(dt) * wa) + 3 * (1 - u) * u * u * (b.value[channel] - b.inTangent[channel] * static_cast<float>(dt) * wb) + u * u * u * b.value[channel];
        }
        return (2 * t * t * t - 3 * t * t + 1) * a.value[channel] + (t * t * t - 2 * t * t + t) * static_cast<float>(dt) * a.outTangent[channel] + (-2 * t * t * t + 3 * t * t) * b.value[channel] + (t * t * t - t * t) * static_cast<float>(dt) * b.inTangent[channel];
    }
    return curve.keys.back().value[channel];
}
bool MoveCurveKeys(CurveData& curve, std::span<const size_t> selection, double deltaTime, float deltaValue,
                   size_t channel)
{
    if (selection.empty() || channel >= curve.channels || !std::isfinite(deltaTime) || !std::isfinite(deltaValue))
        return false;
    double minimum = -DBL_MAX, maximum = DBL_MAX;
    const auto selected = [&](size_t i)
    { return std::find(selection.begin(), selection.end(), i) != selection.end(); };
    for (const auto i : selection)
    {
        if (i >= curve.keys.size())
            return false;
        if (i && !selected(i - 1))
            minimum = (std::max)(minimum, curve.keys[i - 1].time - curve.keys[i].time + 1e-6);
        if (i + 1 < curve.keys.size() && !selected(i + 1))
            maximum = (std::min)(maximum, curve.keys[i + 1].time - curve.keys[i].time - 1e-6);
    }
    if (minimum > maximum)
        return false;
    deltaTime = std::clamp(deltaTime, minimum, maximum);
    for (const auto i : selection)
    {
        curve.keys[i].time += deltaTime;
        curve.keys[i].value[channel] += deltaValue;
    }
    return deltaTime != 0 || deltaValue != 0;
}
namespace
{
void Plot(const CurveData& curve, const CurveViewport& view, ImVec2 p, ImVec2 size, size_t channel)
{
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p, {p.x + size.x, p.y + size.y}, IM_COL32(23, 25, 30, 255), 4);
    draw->PushClipRect(p, {p.x + size.x, p.y + size.y}, true);
    for (int line = 1; line < 8; ++line)
    {
        const float x = p.x + size.x * line / 8;
        draw->AddLine({x, p.y}, {x, p.y + size.y}, IM_COL32(55, 58, 65, 180));
    }
    ImVec2 previous{p.x, p.y + view.Y(EvaluateCurve(curve, view.timeMin, channel), size.y)};
    for (int i = 1; i <= 192; ++i)
    {
        const float x = size.x * i / 192;
        ImVec2 next{p.x + x, p.y + view.Y(EvaluateCurve(curve, view.Time(x, size.x), channel), size.y)};
        draw->AddLine(previous, next, IM_COL32(110, 195, 255, 255), 1.5f);
        previous = next;
    }
    draw->PopClipRect();
}
} // namespace
ControlResult Curve(const char* id, CurveData& curve, CurveState& state, const ControlOptions& options)
{
    ControlResult result;
    curve.channels = std::clamp(curve.channels, size_t(1), size_t(4));
    state.channel = (std::min)(state.channel, curve.channels - 1);
    std::erase_if(state.selected, [&](size_t i)
                  { return i >= curve.keys.size(); });
    if (!state.fitted)
    {
        state.view.Fit(curve, state.channel);
        state.fitted = true;
    }
    ImGui::PushID(id);
    const auto preview = ImGui::GetCursorScreenPos();
    const ImVec2 thumbnail{(std::max)(60.f, (std::min)(ImGui::CalcItemWidth(), ImGui::GetContentRegionAvail().x)),
                           28 * options.scale};
    if (ImGui::InvisibleButton("##Preview", thumbnail))
        ImGui::OpenPopup("Curve");
    Plot(curve, state.view, preview, thumbnail, state.channel);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Edit curve (%zu keys)", curve.keys.size());
    ImGui::SetNextWindowSize({580 * options.scale, 450 * options.scale}, ImGuiCond_FirstUseEver);
    if (ImGui::BeginPopup("Curve"))
    {
        result.active = true;
        state.wasOpen = true;
        if (ImGui::Button("Fit"))
            state.view.Fit(curve, state.channel);
        if (curve.channels > 1)
        {
            ImGui::SameLine();
            int channel = static_cast<int>(state.channel);
            ImGui::SetNextItemWidth(100 * options.scale);
            if (ImGui::Combo("Channel", &channel, "X\0Y\0Z\0W\0"))
                state.channel = static_cast<size_t>(channel);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70 * options.scale);
        Numeric("Time snap", state.timeSnap);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70 * options.scale);
        Numeric("Value snap", state.valueSnap);
        const auto p = ImGui::GetCursorScreenPos();
        const ImVec2 size{(std::max)(100.f, ImGui::GetContentRegionAvail().x), 220 * options.scale};
        ImGui::InvisibleButton("##Canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        const bool hover = ImGui::IsItemHovered();
        const auto mouse = ImGui::GetIO().MousePos;
        const double time = state.view.Time(mouse.x - p.x, size.x);
        const float value = state.view.Value(mouse.y - p.y, size.y);
        if (hover && ImGui::GetIO().MouseWheel)
            state.view.Zoom(time, value, std::pow(.85f, ImGui::GetIO().MouseWheel));
        if (hover && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
        {
            const auto delta = ImGui::GetIO().MouseDelta;
            const auto t = delta.x / size.x * (state.view.timeMax - state.view.timeMin);
            const auto v = delta.y / size.y * (state.view.valueMax - state.view.valueMin);
            state.view.timeMin -= t;
            state.view.timeMax -= t;
            state.view.valueMin += v;
            state.view.valueMax += v;
        }
        Plot(curve, state.view, p, size, state.channel);
        int nearest = -1;
        float distance = 100 * options.scale * options.scale;
        for (size_t i = 0; i < curve.keys.size(); ++i)
        {
            const auto& key = curve.keys[i];
            const ImVec2 point{p.x + state.view.X(key.time, size.x),
                               p.y + state.view.Y(key.value[state.channel], size.y)};
            const float d = (point.x - mouse.x) * (point.x - mouse.x) + (point.y - mouse.y) * (point.y - mouse.y);
            if (d < distance)
            {
                distance = d;
                nearest = static_cast<int>(i);
            }
            const bool selected = std::find(state.selected.begin(), state.selected.end(), i) != state.selected.end();
            if (point.x >= p.x && point.x <= p.x + size.x && point.y >= p.y && point.y <= p.y + size.y)
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    point, selected ? 5.f : 3.f, selected ? IM_COL32(255, 195, 70, 255) : IM_COL32(160, 215, 255, 255));
        }
        if (hover && !options.readOnly && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            state.dragging = nearest;
            state.tangent = 0;
            if (nearest >= 0)
            {
                const auto i = static_cast<size_t>(nearest);
                const auto found = std::find(state.selected.begin(), state.selected.end(), i);
                if (ImGui::GetIO().KeyCtrl)
                {
                    if (found == state.selected.end())
                        state.selected.push_back(i);
                    else
                        state.selected.erase(found);
                }
                else if (found == state.selected.end())
                    state.selected = {i};
            }
            else if (!ImGui::GetIO().KeyCtrl)
                state.selected.clear();
        }
        if (state.selected.size() == 1 && state.selected[0] < curve.keys.size())
        {
            auto& key = curve.keys[state.selected[0]];
            const float x = p.x + state.view.X(key.time, size.x),
                        y = p.y + state.view.Y(key.value[state.channel], size.y);
            for (const int direction : {-1, 1})
            {
                const float tangent = direction < 0 ? key.inTangent[state.channel] : key.outTangent[state.channel];
                if (!std::isfinite(tangent))
                    continue;
                const double span = (state.view.timeMax - state.view.timeMin) * .08 * direction;
                const ImVec2 handle{
                    p.x + state.view.X(key.time + span, size.x),
                    p.y + state.view.Y(key.value[state.channel] + static_cast<float>(span) * tangent, size.y)};
                ImGui::GetWindowDrawList()->AddLine({x, y}, handle, IM_COL32(245, 160, 80, 255));
                ImGui::GetWindowDrawList()->AddCircleFilled(handle, 4, IM_COL32(245, 160, 80, 255));
                if (!options.readOnly && hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && std::hypot(handle.x - mouse.x, handle.y - mouse.y) < 8 * options.scale)
                {
                    state.dragging = static_cast<int>(state.selected[0]);
                    state.tangent = direction;
                }
            }
        }
        if (!options.readOnly && state.dragging >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && static_cast<size_t>(state.dragging) < curve.keys.size())
        {
            auto& key = curve.keys[state.dragging];
            if (state.tangent)
            {
                const auto dt = time - key.time;
                if (std::abs(dt) > 1e-8)
                {
                    (state.tangent < 0 ? key.inTangent[state.channel] : key.outTangent[state.channel]) = static_cast<float>((value - key.value[state.channel]) / dt);
                    result.changed = true;
                }
            }
            else
            {
                auto t = time;
                auto v = value;
                if (state.timeSnap > 0)
                    t = std::round(t / state.timeSnap) * state.timeSnap;
                if (state.valueSnap > 0)
                    v = std::round(v / state.valueSnap) * state.valueSnap;
                result.changed |= MoveCurveKeys(curve, state.selected, t - key.time, v - key.value[state.channel], state.channel);
            }
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            state.dragging = -1;
        ImGui::BeginDisabled(options.readOnly);
        if (hover && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && nearest < 0)
        {
            CurveKey key;
            key.time = time;
            for (size_t c = 0; c < curve.channels; ++c)
                key.value[c] = EvaluateCurve(curve, time, c);
            key.value[state.channel] = value;
            curve.keys.push_back(key);
            std::stable_sort(curve.keys.begin(), curve.keys.end(), [](const auto& a, const auto& b)
                             { return a.time < b.time; });
            state.selected.clear();
            result.changed = true;
        }
        if (ImGui::Button("Add key"))
        {
            CurveKey key;
            key.time = curve.keys.empty() ? 0 : curve.keys.back().time + 1;
            curve.keys.push_back(key);
            state.selected = {curve.keys.size() - 1};
            result.changed = true;
        }
        ImGui::SameLine();
        if ((ImGui::Button("Delete selected") || (hover && ImGui::IsKeyPressed(ImGuiKey_Delete))) && !state.selected.empty())
        {
            auto selected = state.selected;
            std::sort(selected.rbegin(), selected.rend());
            for (auto i : selected)
                if (i < curve.keys.size())
                    curve.keys.erase(curve.keys.begin() + i);
            state.selected.clear();
            result.changed = true;
        }
        if (state.selected.size() == 1 && state.selected.front() < curve.keys.size())
        {
            auto& key = curve.keys[state.selected.front()];
            double t = key.time;
            float v = key.value[state.channel];
            const auto a = Numeric("Time", t);
            const auto b = Numeric("Value", v);
            if (a.changed || b.changed)
                result.changed |= MoveCurveKeys(curve, state.selected, t - key.time, v - key.value[state.channel], state.channel);
            result.changed |= Numeric("In tangent", key.inTangent[state.channel]).changed;
            result.changed |= Numeric("Out tangent", key.outTangent[state.channel]).changed;
            result.changed |= ImGui::Combo("Interpolation", &key.interpolation, "Linear\0Cubic\0");
            if (curve.weights)
            {
                result.changed |= Numeric("In weight", key.inWeight).changed;
                result.changed |= Numeric("Out weight", key.outWeight).changed;
                result.changed |= ImGui::Combo("Weighted mode", &key.weightedMode, "None\0In\0Out\0Both\0");
            }
        }
        if (curve.wrap)
        {
            const auto wrapMode = [&](const char* label, int& value)
            {
                const int modes[]{0, 1, 2, 4, 8};
                const SelectionItem items[]{{"Default"}, {"Once"}, {"Loop"}, {"Ping pong"}, {"Clamp forever"}};
                int selected = -1;
                for (int i = 0; i < 5; ++i)
                    if (value == modes[i])
                        selected = i;
                SelectionState selection;
                const auto edit = Selection(label, selected, items, selection);
                if (edit.changed)
                    value = modes[selected];
                return edit.changed;
            };
            result.changed |= wrapMode("Pre wrap", curve.preWrap);
            result.changed |= wrapMode("Post wrap", curve.postWrap);
        }
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
        state.dragging = -1;
    }
    if (result.changed)
        result.components = ~0ull;
    if (result.completed || result.canceled)
        state.dirty = false;
    ImGui::PopID();
    return result;
}
} // namespace NLS::UI::Controls

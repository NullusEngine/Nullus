#pragma once
#include <span>
#include "UI/Controls/ScalarControl.h"

namespace NLS::UI::Controls
{
struct ComponentLabel
{
    const char* name = "";
    const char* tooltip = "";
    ImVec4 color{};
};
NLS_UI_API ControlResult FloatComponents(const char* id, std::span<float> values,
                                         std::span<const ComponentLabel> labels = {}, float speed = .01f,
                                         const ControlOptions& options = {});
NLS_UI_API ControlResult IntComponents(const char* id, std::span<int32_t> values, const ControlOptions& options = {});
NLS_UI_API ControlResult Color(const char* id, float* rgba, bool hdr = true, const ControlOptions& options = {},
                               ImGuiColorEditFlags flags = 0);
NLS_UI_API ControlResult Matrix(const char* id, std::span<float> values, size_t columns = 4,
                                const ControlOptions& options = {});
} // namespace NLS::UI::Controls

#pragma once
#include <array>
#include <vector>
#include "UI/Controls/Control.h"

namespace NLS::UI::Controls
{
struct GradientColorKey
{
    float time = 0;
    std::array<float, 4> color{1, 1, 1, 1};
    bool operator==(const GradientColorKey&) const = default;
};
struct GradientAlphaKey
{
    float time = 0, alpha = 1;
    bool operator==(const GradientAlphaKey&) const = default;
};
struct GradientData
{
    std::vector<GradientColorKey> colors;
    std::vector<GradientAlphaKey> alphas;
    int mode = 0;
    bool operator==(const GradientData&) const = default;
};
struct GradientState
{
    int color = -1, alpha = -1, dragging = 0;
    bool wasOpen = false, dirty = false;
};
NLS_UI_API std::array<float, 4> EvaluateGradient(const GradientData& gradient, float time);
NLS_UI_API ControlResult Gradient(const char* id, GradientData& gradient, GradientState& state,
                                  const ControlOptions& options = {});
} // namespace NLS::UI::Controls

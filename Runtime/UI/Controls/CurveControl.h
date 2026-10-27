#pragma once
#include <array>
#include <span>
#include <vector>
#include "UI/Controls/Control.h"

namespace NLS::UI::Controls
{
struct CurveKey
{
    double time = 0;
    std::array<float, 4> value{}, inTangent{}, outTangent{};
    float inWeight = 1, outWeight = 1;
    int weightedMode = 0, interpolation = 1;
    bool operator==(const CurveKey&) const = default;
};
struct CurveData
{
    std::vector<CurveKey> keys;
    size_t channels = 1;
    int preWrap = 0, postWrap = 0;
    bool weights = false, wrap = false;
    bool operator==(const CurveData&) const = default;
};
struct NLS_UI_API CurveViewport
{
    double timeMin = 0, timeMax = 1;
    float valueMin = 0, valueMax = 1;
    float X(double time, float width) const;
    float Y(float value, float height) const;
    double Time(float x, float width) const;
    float Value(float y, float height) const;
    void Fit(const CurveData& curve, size_t channel);
    void Zoom(double time, float value, float factor);
};
struct CurveState
{
    CurveViewport view;
    std::vector<size_t> selected;
    size_t channel = 0;
    float timeSnap = 0, valueSnap = 0;
    int dragging = -1, tangent = 0;
    bool fitted = false, wasOpen = false, dirty = false;
};
NLS_UI_API float EvaluateCurve(const CurveData& curve, double time, size_t channel = 0);
NLS_UI_API bool MoveCurveKeys(CurveData& curve, std::span<const size_t> selection, double deltaTime, float deltaValue,
                              size_t channel);
NLS_UI_API ControlResult Curve(const char* id, CurveData& curve, CurveState& state, const ControlOptions& options = {});
} // namespace NLS::UI::Controls

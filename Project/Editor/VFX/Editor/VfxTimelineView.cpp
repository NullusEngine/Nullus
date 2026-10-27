#include "VfxTimelineView.h"

#include <algorithm>
#include <cmath>

namespace NLS::Editor::VFXTools
{
namespace
{
constexpr double kMinSpan = 0.01, kMaxSpan = 600.0;
constexpr float kMinValueSpan = 1e-4f, kMaxValueSpan = 1e6f;
double SnapTo(const double value,const double step)
{
    if (!std::isfinite(step) || step <= 0.0) return value;
    return std::round(value/step)*step;
}
}
float VfxTimelineView::XAt(const double time,const float width) const
{
    const auto span = (std::max)(end-start,kMinSpan);
    return static_cast<float>((time-start)/span)*std::max(width,1.0f);
}
double VfxTimelineView::TimeAt(const float x,const float width) const
{
    return start+static_cast<double>(x)/static_cast<double>(std::max(width,1.0f))*(end-start);
}
double VfxTimelineView::ClampTime(const double time) const
{
    return std::clamp(time,start,end);
}
double VfxTimelineView::SnapTime(const double time,const double step)
{
    return SnapTo(time,step);
}
void VfxTimelineView::ZoomAt(const double anchorTime,const float factor)
{
    if (!std::isfinite(factor) || factor <= 0.0f) return;
    const auto span = std::clamp((end-start)*static_cast<double>(factor),kMinSpan,kMaxSpan);
    const auto ratio = (end-start) > 0.0 ? (anchorTime-start)/(end-start) : 0.5;
    start = anchorTime-span*ratio;
    end = start+span;
}
void VfxTimelineView::Pan(const float deltaPixels,const float width)
{
    const auto seconds = static_cast<double>(deltaPixels)/static_cast<double>(std::max(width,1.0f))*(end-start);
    start -= seconds; end -= seconds;
}
void VfxTimelineView::Fit(const double range,const float width)
{
    (void)width;
    start = 0.0;
    end = (std::max)(range,kMinSpan);
}
float VfxCurveView::XAt(const double time,const float width) const
{
    const auto span = (std::max)(timeMax-timeMin,1e-6);
    return static_cast<float>((time-timeMin)/span)*(std::max)(width,1.0f);
}
float VfxCurveView::YAt(const float value,const float height) const
{
    const auto span = (std::max)(valueMax-valueMin,kMinValueSpan);
    return (1.0f-(value-valueMin)/span)*(std::max)(height,1.0f);
}
double VfxCurveView::TimeAt(const float x,const float width) const
{
    const auto span = (std::max)(timeMax-timeMin,1e-6);
    return timeMin+(static_cast<double>(x)/(std::max)(width,1.0f))*span;
}
float VfxCurveView::ValueAt(const float y,const float height) const
{
    const auto span = (std::max)(valueMax-valueMin,kMinValueSpan);
    return valueMin+(1.0f-y/(std::max)(height,1.0f))*span;
}
double VfxCurveView::SnapTime(const double time,const double step)
{
    return SnapTo(time,step);
}
float VfxCurveView::SnapValue(const float value,const float step)
{
    return static_cast<float>(SnapTo(static_cast<double>(value),static_cast<double>(step)));
}
void VfxCurveView::ZoomTime(const double anchorTime,const float factor)
{
    if (!std::isfinite(factor) || factor <= 0.0f) return;
    const auto span = std::clamp((timeMax-timeMin)*static_cast<double>(factor),static_cast<double>(kMinSpan),kMaxSpan);
    const auto ratio = (timeMax-timeMin) > 0.0 ? (anchorTime-timeMin)/(timeMax-timeMin) : 0.5;
    timeMin = anchorTime-span*ratio;
    timeMax = timeMin+span;
}
void VfxCurveView::ZoomValue(const float anchorValue,const float factor)
{
    if (!std::isfinite(factor) || factor <= 0.0f) return;
    const auto span = std::clamp((valueMax-valueMin)*factor,kMinValueSpan,kMaxValueSpan);
    const auto ratio = (valueMax-valueMin) > 0.0f ? (anchorValue-valueMin)/(valueMax-valueMin) : 0.5f;
    valueMin = anchorValue-span*ratio;
    valueMax = valueMin+span;
}
void VfxCurveView::PanTime(const double seconds)
{
    timeMin += seconds; timeMax += seconds;
}
void VfxCurveView::PanValue(const float value)
{
    valueMin += value; valueMax += value;
}
void VfxCurveView::Fit(const std::span<const VFX::Authoring::CurveKey>& keys)
{
    if (keys.empty()) { timeMin = 0; timeMax = 1; valueMin = 0; valueMax = 1; return; }
    auto timeLow = keys.front().time, timeHigh = keys.front().time;
    auto valueLow = keys.front().value[0], valueHigh = keys.front().value[0];
    for (const auto& key : keys)
    {
        timeLow = (std::min)(timeLow,key.time); timeHigh = (std::max)(timeHigh,key.time);
        for (const auto channel : key.value)
        { valueLow = (std::min)(valueLow,channel); valueHigh = (std::max)(valueHigh,channel); }
    }
    if (timeHigh-timeLow < kMinSpan) { timeHigh = timeLow+kMinSpan; }
    timeMin = timeLow; timeMax = timeHigh;
    if (valueHigh-valueLow < kMinValueSpan) { valueHigh = valueLow+kMinValueSpan; }
    const auto margin = (valueHigh-valueLow)*0.1f;
    valueMin = valueLow-margin; valueMax = valueHigh+margin;
}
int VfxCurveView::FindKey(const std::span<const VFX::Authoring::CurveKey>& keys,
    const float x,const float y,const float width,const float height,const float radius) const
{
    auto best = -1; auto bestDistance = radius*radius;
    for (size_t index = 0; index < keys.size(); ++index)
    {
        const auto dx = XAt(keys[index].time,width)-x;
        const auto dy = YAt(keys[index].value[0],height)-y;
        const auto distance = dx*dx+dy*dy;
        if (distance <= bestDistance) { bestDistance = distance; best = static_cast<int>(index); }
    }
    return best;
}
double VfxCurveView::ClampDragTime(const std::span<const VFX::Authoring::CurveKey>& keys,
    const size_t index,const double time)
{
    // Keys keep their ordering and stay inside the non-negative time domain;
    // the editor still snaps the result when snapping is enabled.
    auto low = 0.0;
    if (index > 0) low = keys[index-1].time+1e-3;
    auto high = 1e6;
    if (index+1 < keys.size()) high = keys[index+1].time-1e-3;
    return std::clamp(time,low,(std::max)(low,high));
}
}

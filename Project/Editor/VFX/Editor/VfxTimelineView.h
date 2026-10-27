#pragma once

#include <span>
#include <VFX/Authoring/VfxAuthoring.h>

namespace NLS::Editor::VFXTools
{
// Pure timeline/canvas mapping. Keeping the maths outside the ImGui code makes
// the ruler, scrubbing and key dragging testable without a frame. [start, end]
// is the authoritative visible window; the canvas width provides the mapping.
struct VfxTimelineView
{
    double start = 0.0, end = 5.0;

    [[nodiscard]] float XAt(double time,float width) const;
    [[nodiscard]] double TimeAt(float x,float width) const;
    [[nodiscard]] double ClampTime(double time) const;
    [[nodiscard]] static double SnapTime(double time,double step);
    // Zoom around the time under the cursor; cancels when the span would
    // collapse or exceed the supported playback range.
    void ZoomAt(double anchorTime,float factor);
    void Pan(float deltaPixels,float width);
    // Frames [0, range] into the given width.
    void Fit(double range,float width);
};

// Curve canvas mapping in time and value space, plus hit testing for keys.
struct VfxCurveView
{
    double timeMin = 0.0, timeMax = 1.0;
    float valueMin = 0.0f, valueMax = 1.0f;

    [[nodiscard]] float XAt(double time,float width) const;
    [[nodiscard]] float YAt(float value,float height) const;
    [[nodiscard]] double TimeAt(float x,float width) const;
    [[nodiscard]] float ValueAt(float y,float height) const;
    [[nodiscard]] static double SnapTime(double time,double step);
    [[nodiscard]] static float SnapValue(float value,float step);
    void ZoomTime(double anchorTime,float factor);
    void ZoomValue(float anchorValue,float factor);
    void PanTime(double seconds);
    void PanValue(float value);
    void Fit(const std::span<const VFX::Authoring::CurveKey>& keys);
    // Nearest key within the pick radius in pixels; -1 when nothing is close.
    [[nodiscard]] int FindKey(const std::span<const VFX::Authoring::CurveKey>& keys,
        float x,float y,float width,float height,float radius) const;
    // Clamp a dragged key time so neighbours keep ordering.
    [[nodiscard]] static double ClampDragTime(const std::span<const VFX::Authoring::CurveKey>& keys,
        size_t index,double time);
};
}

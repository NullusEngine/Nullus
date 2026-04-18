#pragma once

#include <array>
#include <cstdint>

#include <Math/Quaternion.h>
#include <Math/Vector3.h>

#include "Rendering/Data/PipelineState.h"
#include "RenderDef.h"

namespace NLS::Render::Debug
{
    enum class DebugDrawCategory : uint8_t
    {
        General = 0,
        Grid,
        Bounds,
        Lighting,
        Camera,
        Selection,
        Count
    };

    enum class DebugDrawPrimitiveType : uint8_t
    {
        Point = 0,
        Line,
        Triangle
    };

    enum class DebugDrawDepthMode : uint8_t
    {
        DepthTest = 0,
        AlwaysOnTop
    };

    enum class DebugDrawFillMode : uint8_t
    {
        Wireframe = 0,
        Solid
    };

    enum class DebugDrawLifetimeMode : uint8_t
    {
        OneFrame = 0,
        FrameCount,
        Persistent
    };

    enum class DebugDrawLimitState : uint8_t
    {
        WithinLimits = 0,
        AtLimit,
        OverflowRejected
    };

    struct NLS_RENDER_API DebugDrawLifetime
    {
        DebugDrawLifetimeMode mode = DebugDrawLifetimeMode::OneFrame;
        uint32_t remainingFrames = 1u;

        static DebugDrawLifetime OneFrame();
        static DebugDrawLifetime Frames(uint32_t frameCount);
        static DebugDrawLifetime Persistent();
    };

    struct NLS_RENDER_API DebugDrawStyle
    {
        Maths::Vector3 color{ 1.0f, 1.0f, 1.0f };
        float lineWidth = 1.0f;
        float pointSize = 4.0f;
        DebugDrawDepthMode depthMode = DebugDrawDepthMode::DepthTest;
        DebugDrawFillMode fillMode = DebugDrawFillMode::Wireframe;
    };

    struct NLS_RENDER_API DebugDrawSubmitOptions
    {
        DebugDrawCategory category = DebugDrawCategory::General;
        DebugDrawLifetime lifetime = DebugDrawLifetime::OneFrame();
        DebugDrawStyle style;
    };

    struct NLS_RENDER_API DebugDrawPrimitive
    {
        DebugDrawPrimitiveType type = DebugDrawPrimitiveType::Line;
        std::array<Maths::Vector3, 3u> points{};
        DebugDrawSubmitOptions options;
    };
}

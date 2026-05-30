#pragma once

#include <algorithm>
#include <optional>

#include "Math/Vector2.h"

namespace NLS::Windowing
{
struct InfiniteCursorWarpRequest
{
    Maths::Vector2 targetPosition{};
    Maths::Vector2 compensation{};
};

class InfiniteCursorWrapState
{
public:
    void SetEnabled(const bool p_enabled)
    {
        if (m_enabled == p_enabled)
            return;

        m_enabled = p_enabled;
        if (!m_enabled)
            Reset();
    }

    bool IsEnabled() const
    {
        return m_enabled;
    }

    void Reset()
    {
        m_frameCompensation = Maths::Vector2::Zero;
        m_hasCursorPosition = false;
        m_latestCursorPosition = Maths::Vector2::Zero;
    }

    void BeginFrame()
    {
        m_frameCompensation = Maths::Vector2::Zero;
    }

    void SeedCursorPosition(const double p_x, const double p_y)
    {
        m_latestCursorPosition = Maths::Vector2(static_cast<float>(p_x), static_cast<float>(p_y));
        m_hasCursorPosition = true;
    }

    std::optional<InfiniteCursorWarpRequest> Evaluate(
        const Maths::Vector2& p_windowSize,
        const bool p_focused)
    {
        if (!m_enabled || !p_focused || !m_hasCursorPosition)
            return std::nullopt;

        const auto request = TryBuildWarpRequest(p_windowSize, m_latestCursorPosition);
        if (!request.has_value())
            return std::nullopt;

        m_frameCompensation = request->compensation;
        m_latestCursorPosition = request->targetPosition;
        return request;
    }

    Maths::Vector2 GetFrameCompensation() const
    {
        return m_frameCompensation;
    }

private:
    static std::optional<InfiniteCursorWarpRequest> TryBuildWarpRequest(
        const Maths::Vector2& p_windowSize,
        const Maths::Vector2& p_cursorPosition)
    {
        if (p_windowSize.x <= 2.0f || p_windowSize.y <= 2.0f)
            return std::nullopt;

        constexpr float edgeTriggerDistance = 4.0f;
        constexpr float warpTargetPadding = edgeTriggerDistance + 1.0f;
        if (p_windowSize.x <= warpTargetPadding * 2.0f || p_windowSize.y <= warpTargetPadding * 2.0f)
            return std::nullopt;

        // Warp outside the trigger zone so the next frame starts from a stable, non-wrapping position.
        const float leftTarget = warpTargetPadding;
        const float topTarget = warpTargetPadding;
        const float rightTarget = p_windowSize.x - warpTargetPadding;
        const float bottomTarget = p_windowSize.y - warpTargetPadding;

        float wrappedX = p_cursorPosition.x;
        float wrappedY = p_cursorPosition.y;

        if (p_cursorPosition.x <= edgeTriggerDistance)
            wrappedX = rightTarget;
        else if (p_cursorPosition.x >= p_windowSize.x - edgeTriggerDistance)
            wrappedX = leftTarget;

        if (p_cursorPosition.y <= edgeTriggerDistance)
            wrappedY = bottomTarget;
        else if (p_cursorPosition.y >= p_windowSize.y - edgeTriggerDistance)
            wrappedY = topTarget;

        if (wrappedX == p_cursorPosition.x && wrappedY == p_cursorPosition.y)
            return std::nullopt;

        InfiniteCursorWarpRequest request;
        request.targetPosition.x = std::clamp(wrappedX, leftTarget, rightTarget);
        request.targetPosition.y = std::clamp(wrappedY, topTarget, bottomTarget);
        request.compensation = request.targetPosition - p_cursorPosition;
        return request;
    }

private:
    bool m_enabled = false;
    bool m_hasCursorPosition = false;
    Maths::Vector2 m_latestCursorPosition = Maths::Vector2::Zero;
    Maths::Vector2 m_frameCompensation = Maths::Vector2::Zero;
};
} // namespace NLS::Windowing

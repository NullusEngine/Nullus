#include "Rendering/Debug/DebugDrawService.h"

#include "Rendering/Debug/DebugDrawGeometry.h"

#include <algorithm>

namespace NLS::Render::Debug
{
DebugDrawService::DebugDrawService(const size_t maxVisiblePrimitives)
    : m_maxVisiblePrimitives(maxVisiblePrimitives)
    , m_categoryVisibility(static_cast<size_t>(DebugDrawCategory::Count), true)
{
}

bool DebugDrawService::SubmitPoint(
    const Maths::Vector3& position,
    const DebugDrawSubmitOptions& options)
{
    DebugDrawPrimitive primitive;
    primitive.type = DebugDrawPrimitiveType::Point;
    primitive.points[0] = position;
    primitive.options = options;
    return SubmitPrimitive(primitive);
}

bool DebugDrawService::SubmitPoint(
    const Maths::Vector3& position,
    const Maths::Vector3& color,
    const float pointSize,
    const DebugDrawSubmitOptions& options)
{
    return SubmitPoint(position, WithColorAndPointSize(options, color, pointSize));
}

bool DebugDrawService::SubmitLine(
    const Maths::Vector3& start,
    const Maths::Vector3& end,
    const DebugDrawSubmitOptions& options)
{
    DebugDrawPrimitive primitive;
    primitive.type = DebugDrawPrimitiveType::Line;
    primitive.points[0] = start;
    primitive.points[1] = end;
    primitive.options = options;
    return SubmitPrimitive(primitive);
}

bool DebugDrawService::SubmitLine(
    const Maths::Vector3& start,
    const Maths::Vector3& end,
    const Maths::Vector3& color,
    const float lineWidth,
    const DebugDrawSubmitOptions& options)
{
    return SubmitLine(start, end, WithColorAndLineWidth(options, color, lineWidth));
}

bool DebugDrawService::SubmitTriangle(
    const Maths::Vector3& a,
    const Maths::Vector3& b,
    const Maths::Vector3& c,
    const DebugDrawSubmitOptions& options)
{
    DebugDrawPrimitive primitive;
    primitive.type = DebugDrawPrimitiveType::Triangle;
    primitive.points[0] = a;
    primitive.points[1] = b;
    primitive.points[2] = c;
    primitive.options = options;
    return SubmitPrimitive(primitive);
}

bool DebugDrawService::SubmitTriangle(
    const Maths::Vector3& a,
    const Maths::Vector3& b,
    const Maths::Vector3& c,
    const Maths::Vector3& color,
    const DebugDrawSubmitOptions& options)
{
    auto styledOptions = options;
    styledOptions.style.color = color;
    return SubmitTriangle(a, b, c, styledOptions);
}

bool DebugDrawService::SubmitLine(
    const Data::PipelineState&,
    const Maths::Vector3& start,
    const Maths::Vector3& end,
    const Maths::Vector3& color,
    const float lineWidth,
    const DebugDrawSubmitOptions& options)
{
    return SubmitLine(start, end, color, lineWidth, options);
}

bool DebugDrawService::SubmitBox(
    const Data::PipelineState&,
    const Maths::Vector3& position,
    const Maths::Quaternion& rotation,
    const Maths::Vector3& size,
    const Maths::Vector3& color,
    const float lineWidth,
    const DebugDrawSubmitOptions& options)
{
    return NLS::Render::Debug::SubmitBox(*this, position, rotation, size, WithColorAndLineWidth(options, color, lineWidth));
}

bool DebugDrawService::SubmitSphere(
    const Data::PipelineState&,
    const Maths::Vector3& position,
    const Maths::Quaternion& rotation,
    const float radius,
    const Maths::Vector3& color,
    const float lineWidth,
    const DebugDrawSubmitOptions& options)
{
    return NLS::Render::Debug::SubmitSphere(*this, position, rotation, radius, WithColorAndLineWidth(options, color, lineWidth));
}

bool DebugDrawService::SubmitCapsule(
    const Data::PipelineState&,
    const Maths::Vector3& position,
    const Maths::Quaternion& rotation,
    const float radius,
    const float height,
    const Maths::Vector3& color,
    const float lineWidth,
    const DebugDrawSubmitOptions& options)
{
    return NLS::Render::Debug::SubmitCapsule(*this, position, rotation, radius, height, WithColorAndLineWidth(options, color, lineWidth));
}

void DebugDrawService::SetCategoryEnabled(const DebugDrawCategory category, const bool enabled)
{
    m_categoryVisibility[CategoryIndex(category)] = enabled;
}

bool DebugDrawService::IsCategoryEnabled(const DebugDrawCategory category) const
{
    return m_categoryVisibility[CategoryIndex(category)];
}

void DebugDrawService::SetEnabled(const bool enabled)
{
    m_enabled = enabled;
}

bool DebugDrawService::IsEnabled() const
{
    return m_enabled;
}

std::vector<std::reference_wrapper<const DebugDrawPrimitive>> DebugDrawService::CollectVisiblePrimitives() const
{
    std::vector<std::reference_wrapper<const DebugDrawPrimitive>> visiblePrimitives;
    visiblePrimitives.reserve(m_primitives.size());

    if (!m_enabled)
        return visiblePrimitives;

    for (const auto& primitive : m_primitives)
    {
        if (IsCategoryEnabled(primitive.options.category))
            visiblePrimitives.push_back(std::cref(primitive));
    }

    return visiblePrimitives;
}

std::vector<std::reference_wrapper<const DebugDrawPrimitive>> DebugDrawService::CollectVisibleLines() const
{
    std::vector<std::reference_wrapper<const DebugDrawPrimitive>> visibleLines;

    for (const auto& primitive : CollectVisiblePrimitives())
    {
        if (primitive.get().type == DebugDrawPrimitiveType::Line)
            visibleLines.push_back(primitive);
    }

    return visibleLines;
}

DebugDrawLimitState DebugDrawService::GetLimitState() const
{
    return m_limitState;
}

size_t DebugDrawService::GetQueuedPrimitiveCount() const
{
    return m_primitives.size();
}

size_t DebugDrawService::GetQueuedLineCount() const
{
    return GetQueuedPrimitiveCount();
}

size_t DebugDrawService::GetMaxVisibleLines() const
{
    return m_maxVisiblePrimitives;
}

void DebugDrawService::EndFrame()
{
    m_primitives.erase(
        std::remove_if(
            m_primitives.begin(),
            m_primitives.end(),
            [](DebugDrawPrimitive& primitive)
            {
                auto& lifetime = primitive.options.lifetime;
                switch (lifetime.mode)
                {
                case DebugDrawLifetimeMode::OneFrame:
                    return true;

                case DebugDrawLifetimeMode::FrameCount:
                    if (lifetime.remainingFrames <= 1u)
                        return true;
                    --lifetime.remainingFrames;
                    return false;

                case DebugDrawLifetimeMode::Persistent:
                    return false;
                }

                return false;
            }),
        m_primitives.end());

    UpdateLimitState();
}

void DebugDrawService::Clear()
{
    m_primitives.clear();
    UpdateLimitState();
}

bool DebugDrawService::CanReserve(const size_t additionalPrimitiveCount)
{
    if (m_primitives.size() + additionalPrimitiveCount > m_maxVisiblePrimitives)
    {
        m_limitState = DebugDrawLimitState::OverflowRejected;
        return false;
    }

    m_limitState =
        m_primitives.size() + additionalPrimitiveCount == m_maxVisiblePrimitives
            ? DebugDrawLimitState::AtLimit
            : DebugDrawLimitState::WithinLimits;
    return true;
}

void DebugDrawService::UpdateLimitState()
{
    m_limitState =
        m_maxVisiblePrimitives > 0u && m_primitives.size() >= m_maxVisiblePrimitives
            ? DebugDrawLimitState::AtLimit
            : DebugDrawLimitState::WithinLimits;
}

bool DebugDrawService::SubmitPrimitive(DebugDrawPrimitive primitive)
{
    if (!CanReserve(1u))
        return false;

    m_primitives.push_back(primitive);
    return true;
}

DebugDrawSubmitOptions DebugDrawService::WithColorAndLineWidth(
    DebugDrawSubmitOptions options,
    const Maths::Vector3& color,
    const float lineWidth)
{
    options.style.color = color;
    options.style.lineWidth = lineWidth;
    return options;
}

DebugDrawSubmitOptions DebugDrawService::WithColorAndPointSize(
    DebugDrawSubmitOptions options,
    const Maths::Vector3& color,
    const float pointSize)
{
    options.style.color = color;
    options.style.pointSize = pointSize;
    return options;
}

size_t DebugDrawService::CategoryIndex(const DebugDrawCategory category)
{
    return static_cast<size_t>(category);
}
}

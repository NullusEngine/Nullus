#include "Rendering/Debug/DebugDrawService.h"

#include "Rendering/Debug/DebugDrawGeometry.h"

#include <algorithm>
#include <utility>

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
    const bool current = m_categoryVisibility[CategoryIndex(category)];
    if (current == enabled)
        return;

    m_categoryVisibility[CategoryIndex(category)] = enabled;
    ++m_contentRevision;
}

bool DebugDrawService::IsCategoryEnabled(const DebugDrawCategory category) const
{
    return m_categoryVisibility[CategoryIndex(category)];
}

void DebugDrawService::SetEnabled(const bool enabled)
{
    if (m_enabled == enabled)
        return;

    m_enabled = enabled;
    ++m_contentRevision;
}

bool DebugDrawService::IsEnabled() const
{
    return m_enabled;
}

bool DebugDrawService::SetPersistentPrimitiveGroup(
    const uint64_t groupId,
    std::vector<DebugDrawPrimitive> primitives)
{
    if (groupId == 0u)
        return false;

    const auto existingCount = static_cast<size_t>(std::count_if(
        m_primitives.begin(),
        m_primitives.end(),
        [groupId](const PrimitiveEntry& entry) { return entry.persistentGroupId == groupId; }));
    if (m_primitives.size() - existingCount + primitives.size() > m_maxVisiblePrimitives)
    {
        m_limitState = DebugDrawLimitState::OverflowRejected;
        return false;
    }

    m_primitives.erase(
        std::remove_if(
            m_primitives.begin(),
            m_primitives.end(),
            [groupId](const PrimitiveEntry& entry) { return entry.persistentGroupId == groupId; }),
        m_primitives.end());

    m_primitives.reserve(m_primitives.size() + primitives.size());
    for (auto& primitive : primitives)
    {
        primitive.options.lifetime = DebugDrawLifetime::Persistent();
        m_primitives.push_back({ std::move(primitive), groupId });
    }

    ++m_contentRevision;
    UpdateLimitState();
    return true;
}

bool DebugDrawService::RemovePersistentPrimitiveGroup(const uint64_t groupId)
{
    if (groupId == 0u)
        return false;

    const auto oldSize = m_primitives.size();
    m_primitives.erase(
        std::remove_if(
            m_primitives.begin(),
            m_primitives.end(),
            [groupId](const PrimitiveEntry& entry) { return entry.persistentGroupId == groupId; }),
        m_primitives.end());
    if (m_primitives.size() == oldSize)
        return false;

    ++m_contentRevision;
    UpdateLimitState();
    return true;
}

uint64_t DebugDrawService::GetContentRevision() const
{
    return m_contentRevision;
}

bool DebugDrawService::HasVisiblePrimitives() const
{
    if (!m_enabled)
        return false;

    return std::any_of(
        m_primitives.begin(),
        m_primitives.end(),
        [this](const PrimitiveEntry& entry)
        {
            return IsCategoryEnabled(entry.primitive.options.category);
        });
}

void DebugDrawService::CollectVisiblePrimitives(
    std::vector<std::reference_wrapper<const DebugDrawPrimitive>>& outPrimitives) const
{
    outPrimitives.clear();
    outPrimitives.reserve(m_primitives.size());

    if (!m_enabled)
        return;

    for (const auto& entry : m_primitives)
    {
        if (IsCategoryEnabled(entry.primitive.options.category))
            outPrimitives.push_back(std::cref(entry.primitive));
    }
}

std::vector<std::reference_wrapper<const DebugDrawPrimitive>> DebugDrawService::CollectVisiblePrimitives() const
{
    std::vector<std::reference_wrapper<const DebugDrawPrimitive>> visiblePrimitives;
    CollectVisiblePrimitives(visiblePrimitives);

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
    const auto oldSize = m_primitives.size();
    m_primitives.erase(
        std::remove_if(
            m_primitives.begin(),
            m_primitives.end(),
            [](PrimitiveEntry& entry)
            {
                auto& primitive = entry.primitive;
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

    if (m_primitives.size() != oldSize)
        ++m_contentRevision;
    UpdateLimitState();
}

void DebugDrawService::Clear()
{
    if (m_primitives.empty())
        return;

    m_primitives.clear();
    ++m_contentRevision;
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

    m_primitives.push_back({ std::move(primitive), 0u });
    ++m_contentRevision;
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

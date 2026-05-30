#pragma once

#include <optional>

#include "Windowing/Cursor/ECursorShape.h"

namespace NLS::UI
{
struct InfiniteDragCursorRelease
{
    NLS::Cursor::ECursorShape activeCursorShape = NLS::Cursor::ECursorShape::ARROW;
    std::optional<NLS::Cursor::ECursorShape> previousCursorShape;
};

class InfiniteDragCursorLease
{
public:
    void BeginFrame()
    {
        m_requestedThisFrame = false;
    }

    bool Request(
        const NLS::Cursor::ECursorShape p_currentCursorShape,
        const NLS::Cursor::ECursorShape p_requestedCursorShape)
    {
        m_requestedThisFrame = true;
        if (m_ownsCursor)
        {
            if (m_activeCursorShape == p_requestedCursorShape)
                return false;

            m_activeCursorShape = p_requestedCursorShape;
            return false;
        }

        m_ownsCursor = true;
        m_activeCursorShape = p_requestedCursorShape;
        m_previousCursorShape = p_currentCursorShape;
        return true;
    }

    std::optional<InfiniteDragCursorRelease> ReleaseIfUnrequested()
    {
        if (!m_ownsCursor || m_requestedThisFrame)
            return std::nullopt;

        InfiniteDragCursorRelease release;
        release.activeCursorShape = m_activeCursorShape;
        release.previousCursorShape = m_previousCursorShape;
        m_ownsCursor = false;
        m_activeCursorShape = NLS::Cursor::ECursorShape::ARROW;
        m_previousCursorShape.reset();
        return release;
    }

    bool OwnsCursor() const
    {
        return m_ownsCursor;
    }

private:
    bool m_ownsCursor = false;
    bool m_requestedThisFrame = false;
    NLS::Cursor::ECursorShape m_activeCursorShape = NLS::Cursor::ECursorShape::ARROW;
    std::optional<NLS::Cursor::ECursorShape> m_previousCursorShape;
};
} // namespace NLS::UI

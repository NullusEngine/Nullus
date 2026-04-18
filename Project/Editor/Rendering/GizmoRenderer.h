#pragma once

#include <optional>

#include <Core/GizmoBehaviour.h>
#include <Math/Quaternion.h>
#include <Math/Vector3.h>
#include <Resources/Material.h>

namespace NLS::Render::Core { class CompositeRenderer; }

namespace NLS::Editor::Rendering
{
class DebugModelRenderer;

/**
 * Draws editor transform gizmos with explicit renderer-owned helpers.
 */
class GizmoRenderer
{
public:
    GizmoRenderer(
        NLS::Render::Core::CompositeRenderer& renderer,
        DebugModelRenderer& debugModelRenderer);

    void DrawGizmo(
        const Maths::Vector3& position,
        const Maths::Quaternion& rotation,
        Editor::Core::EGizmoOperation operation,
        bool pickable,
        std::optional<Editor::Core::GizmoBehaviour::EDirection> highlightedDirection);

private:
    NLS::Render::Core::CompositeRenderer& m_renderer;
    DebugModelRenderer& m_debugModelRenderer;
    NLS::Render::Resources::Material m_gizmoArrowMaterial;
    NLS::Render::Resources::Material m_gizmoBallMaterial;
};
}

#pragma once

#include <optional>
#include <vector>

#include <Core/GizmoBehaviour.h>
#include <Math/Quaternion.h>
#include <Math/Vector3.h>
#include <Resources/Material.h>
#include <Rendering/Context/ThreadedRenderingLifecycle.h>

#include "Rendering/EditorHelperLifecycle.h"

namespace NLS::Engine { class GameObject; }
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

    static bool ShouldIncludeInThreadedFrame(bool actorPassEnabled, const Engine::GameObject* selectedActor)
    {
        ThreadedEditorHelperState helperState;
        helperState.actorPassEnabled = actorPassEnabled;
        helperState.hasSelectedActor = selectedActor != nullptr;
        return HasThreadedGizmoHelperPass(helperState);
    }

    void CaptureGizmoDrawCommands(
        const Maths::Vector3& position,
        const Maths::Quaternion& rotation,
        Editor::Core::EGizmoOperation operation,
        std::optional<Editor::Core::GizmoBehaviour::EDirection> highlightedDirection,
        std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands);
    void DrawGizmo(
        const Maths::Vector3& position,
        const Maths::Quaternion& rotation,
        Editor::Core::EGizmoOperation operation,
        std::optional<Editor::Core::GizmoBehaviour::EDirection> highlightedDirection);

private:
    NLS::Render::Core::CompositeRenderer& m_renderer;
    DebugModelRenderer& m_debugModelRenderer;
    NLS::Render::Resources::Material m_gizmoArrowMaterial;
    NLS::Render::Resources::Material m_gizmoBallMaterial;
};
}

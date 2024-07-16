#pragma once

#include <Rendering/Entities/Camera.h>
#include <Rendering/Features/DebugShapeRenderFeature.h>

#include <GameObject.h>
#include <SceneSystem/SceneManager.h>
#include <Components/MeshRenderer.h>
#include <Resources/Material.h>
#include <Components/LightComponent.h>
#include <Rendering/SceneRenderer.h>

#include "Core/Context.h"

namespace NLS::Editor::Rendering
{
	/**
	* Draw a gizmo
	*/
class GizmoRenderFeature : public NLS::Render::Features::ARenderFeature
	{
	public:
		/**
		* Constructor
		* @param p_renderer
		*/
        GizmoRenderFeature(NLS::Render::Core::CompositeRenderer& p_renderer);

		/**
		* Render a gizmo at position
		* @param p_position
		* @param p_rotation
		* @param p_operation
		* @param p_pickable (Determine the shader to use to render the gizmo)
		* @param p_highlightedDirection
		*/
		void DrawGizmo(
			const Maths::Vector3& p_position,
			const Maths::Quaternion& p_rotation,
			Editor::Core::EGizmoOperation p_operation,
			bool p_pickable,
			std::optional<Editor::Core::GizmoBehaviour::EDirection> p_highlightedDirection
		);

	private:
        NLS::Render::Resources::Material m_gizmoArrowMaterial;
        NLS::Render::Resources::Material m_gizmoBallMaterial;
	};
}
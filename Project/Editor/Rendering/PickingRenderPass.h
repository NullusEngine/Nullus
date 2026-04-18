#pragma once

#include <cstdint>
#include <optional>
#include <variant>

#include <Rendering/Entities/Camera.h>

#include <GameObject.h>
#include <SceneSystem/SceneManager.h>
#include <Components/MeshRenderer.h>
#include <Resources/Material.h>
#include <Components/LightComponent.h>
#include <Rendering/BaseSceneRenderer.h>

#include "Core/Context.h"
#include "Core/GizmoBehaviour.h"
#include "Rendering/DebugModelRenderer.h"

namespace NLS::Editor::Rendering
{
	/**
	* Draw the scene for actor picking
	*/
	class PickingRenderPass : public NLS::Render::Core::ARenderPass
	{
	public:
		using PickingResult =
			std::optional<
            std::variant<Engine::GameObject*,
			Core::GizmoBehaviour::EDirection>
			>;

		/**
		* Constructor
		* @param p_renderer
		*/
		PickingRenderPass(NLS::Render::Core::CompositeRenderer& p_renderer);

		/**
		* Return the picking result at the given position
		* @param p_x
		* @param p_y
		*/
		PickingResult PickAtRenderCoordinate(uint32_t p_x, uint32_t p_y);
		bool SupportsPickingReadback() const;

		bool ManagesOwnRenderPass() const override { return true; }

	private:
		virtual void Draw(NLS::Render::Data::PipelineState p_pso) override;
        PickingResult DecodePickingResult(const Engine::SceneSystem::Scene& p_scene, const uint8_t (&pixel)[3]) const;
        bool RenderPickingScene(NLS::Render::Data::PipelineState p_pso);
        void DrawPickableModels(NLS::Render::Data::PipelineState p_pso, Engine::SceneSystem::Scene& p_scene);
        void DrawPickableCameras(NLS::Render::Data::PipelineState p_pso, Engine::SceneSystem::Scene& p_scene);
        void DrawPickableLights(NLS::Render::Data::PipelineState p_pso, Engine::SceneSystem::Scene& p_scene);
		void DrawPickableGizmo(
			NLS::Render::Data::PipelineState p_pso,
			const Maths::Vector3& p_position,
			const Maths::Quaternion& p_rotation,
			Core::EGizmoOperation p_operation
		);

	private:
        DebugModelRenderer m_debugModelRenderer;
		NLS::Render::Buffers::Framebuffer m_actorPickingFramebuffer;
		NLS::Render::Resources::Material m_actorPickingMaterial;
		NLS::Render::Resources::Material m_lightMaterial;
		NLS::Render::Resources::Material m_gizmoPickingMaterial;
		Engine::SceneSystem::Scene* m_lastRenderedScene = nullptr;
		uint16_t m_lastRenderWidth = 0;
		uint16_t m_lastRenderHeight = 0;
		bool m_hasRenderedPickingFrame = false;
	};
}

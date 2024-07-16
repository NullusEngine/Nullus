#pragma once

#include <Rendering/Entities/Camera.h>
#include <Rendering/Features/DebugShapeRenderFeature.h>
#include <Rendering/Core/CompositeRenderer.h>

#include <GameObject.h>
#include <SceneSystem/SceneManager.h>
#include <Components/MeshRenderer.h>
#include <Resources/Material.h>
#include <Components/LightComponent.h>

#include "Core/Context.h"

namespace NLS::Editor::Rendering
{
	/**
	* Draw the scene for actor picking
	*/
	class OutlineRenderFeature : public NLS::Render::Features::ARenderFeature
	{
	public:
		/**
		* Constructor
		* @param p_renderer
		*/
		OutlineRenderFeature(NLS::Render::Core::CompositeRenderer& p_renderer);

		/**
		* Draw an outline around the given actor
		* @param p_actor
		* @param p_color
		* @param p_thickness
		*/
		virtual void DrawOutline(Engine::GameObject& p_actor, const Maths::Vector4& p_color, float p_thickness);

	private:
		void DrawStencilPass(Engine::GameObject& p_actor);
		void DrawOutlinePass(Engine::GameObject& p_actor, const Maths::Vector4& p_color, float p_thickness);
		
		void DrawActorToStencil(NLS::Render::Data::PipelineState p_pso, Engine::GameObject& p_actor);
		void DrawActorOutline(NLS::Render::Data::PipelineState p_pso, Engine::GameObject& p_actor);
		void DrawModelToStencil(NLS::Render::Data::PipelineState p_pso, const Maths::Matrix4& p_worldMatrix, NLS::Render::Resources::Model& p_model);
		void DrawModelOutline(NLS::Render::Data::PipelineState p_pso, const Maths::Matrix4& p_worldMatrix, NLS::Render::Resources::Model& p_model);

	private:
        NLS::Render::Resources::Material m_stencilFillMaterial;
        NLS::Render::Resources::Material m_outlineMaterial;
	};
}
#pragma once

#include "Rendering/Features/ARenderFeature.h"
#include "RenderDef.h"
namespace NLS::Render::Features
{
	/**
	* The ShapeDrawer handles the drawing of basic shapes
	*/
	class NLS_RENDER_API DebugShapeRenderFeature : public ARenderFeature
	{
	public:
		/**
		* Constructor
		* @param p_renderer
		*/
		DebugShapeRenderFeature(Core::CompositeRenderer& p_renderer);

		/**
		* Destructor
		*/
		virtual ~DebugShapeRenderFeature();

		/**
		* Defines the view projection to use when drawing
		* @param p_viewProjection
		*/
		void SetViewProjection(const Maths::Matrix4& p_viewProjection);

		/**
		* Draw a line in world space
		* @param p_pso
		* @param p_start
		* @param p_end
		* @param p_color
		* @param p_lineWidth
		*/
		void DrawLine(
			NLS::Render::Data::PipelineState p_pso,
			const Maths::Vector3& p_start,
			const Maths::Vector3& p_end,
			const Maths::Vector3& p_color,
			float p_lineWidth = 1.0f
		);

		/**
		* Draw a box in world space
		* @param p_pso
		* @param p_position
		* @param p_rotation
		* @param p_size
		* @param p_color
		* @param p_lineWidth
		*/
		void DrawBox(
			NLS::Render::Data::PipelineState p_pso,
			const Maths::Vector3& p_position,
			const Maths::Quaternion& p_rotation,
			const Maths::Vector3& p_size,
			const Maths::Vector3& p_color,
			float p_lineWidth = 1.0f
		);

		/**
		* Draw a sphere in world space
		* @param p_pso
		* @param p_position
		* @param p_rotation
		* @param p_radius
		* @param p_color
		* @param p_lineWidth
		*/
		void DrawSphere(
			NLS::Render::Data::PipelineState p_pso,
			const Maths::Vector3& p_position,
			const Maths::Quaternion& p_rotation,
			float p_radius,
			const Maths::Vector3& p_color,
			float p_lineWidth = 1.0f
		);

		/**
		* Draw a capsule in world space
		* @param p_pso
		* @param p_position
		* @param p_rotation
		* @param p_radius
		* @param p_height
		* @param p_color
		* @param p_lineWidth
		*/
		void DrawCapsule(
			NLS::Render::Data::PipelineState p_pso,
			const Maths::Vector3& p_position,
			const Maths::Quaternion& p_rotation,
			float p_radius,
			float p_height,
			const Maths::Vector3& p_color,
			float p_lineWidth = 1.0f
		);

	protected:
		virtual void OnBeginFrame(const Data::FrameDescriptor& p_frameDescriptor) override;

	private:
		NLS::Render::Resources::Shader* m_lineShader = nullptr;
		NLS::Render::Resources::Mesh* m_lineMesh = nullptr;

		std::unique_ptr<NLS::Render::Resources::Material> m_lineMaterial;
	};
}
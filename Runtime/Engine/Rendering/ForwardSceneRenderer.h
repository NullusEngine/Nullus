#pragma once

#include <fg/FrameGraph.hpp>

#include "Rendering/BaseSceneRenderer.h"

namespace NLS::Engine::Rendering
{
	class NLS_ENGINE_API ForwardSceneRenderer : public BaseSceneRenderer
	{
	public:
		explicit ForwardSceneRenderer(NLS::Render::Context::Driver& p_driver);

		void BeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor) override;
		void DrawFrame() override;

	protected:
		virtual void DrawOpaques(NLS::Render::Data::PipelineState pso);
		virtual void DrawSkyboxes(NLS::Render::Data::PipelineState pso);
		virtual void DrawTransparents(NLS::Render::Data::PipelineState pso);

	private:
		struct ForwardSceneDescriptor
		{
			AllDrawables drawables;
		};
	};
}

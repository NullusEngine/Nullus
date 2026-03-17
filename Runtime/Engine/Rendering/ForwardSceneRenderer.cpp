#include "Rendering/ForwardSceneRenderer.h"

namespace NLS::Engine::Rendering
{
	ForwardSceneRenderer::ForwardSceneRenderer(NLS::Render::Context::Driver& p_driver)
		: BaseSceneRenderer(p_driver)
	{
	}

	void ForwardSceneRenderer::BeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor)
	{
		BaseSceneRenderer::BeginFrame(p_frameDescriptor);
		AddDescriptor<ForwardSceneDescriptor>({
			ParseScene()
		});
	}

	void ForwardSceneRenderer::DrawFrame()
	{
		auto pso = CreatePipelineState();
		const auto& scene = GetDescriptor<ForwardSceneDescriptor>();
		FrameGraph frameGraph;
		frameGraph.reserve(3, 0);

		frameGraph.addCallbackPass(
			"ForwardOpaque",
			[](FrameGraph::Builder& builder, FrameGraph::NoData&)
			{
				builder.setSideEffect();
			},
			[this, pso](const FrameGraph::NoData&, FrameGraphPassResources&, void*)
			{
				DrawOpaques(pso);
			}
		);

		frameGraph.addCallbackPass(
			"ForwardSkybox",
			[](FrameGraph::Builder& builder, FrameGraph::NoData&)
			{
				builder.setSideEffect();
			},
			[this, pso](const FrameGraph::NoData&, FrameGraphPassResources&, void*)
			{
				DrawSkyboxes(pso);
			}
		);

		frameGraph.addCallbackPass(
			"ForwardTransparent",
			[](FrameGraph::Builder& builder, FrameGraph::NoData&)
			{
				builder.setSideEffect();
			},
			[this, pso](const FrameGraph::NoData&, FrameGraphPassResources&, void*)
			{
				DrawTransparents(pso);
			}
		);

		frameGraph.compile();
		frameGraph.execute();

		DrawRegisteredPasses(CreatePipelineState());
	}

	void ForwardSceneRenderer::DrawOpaques(NLS::Render::Data::PipelineState pso)
	{
		const auto& scene = GetDescriptor<ForwardSceneDescriptor>();
		for (const auto& [_, drawable] : scene.drawables.opaques)
		{
			DrawEntity(pso, drawable);
		}
	}

	void ForwardSceneRenderer::DrawSkyboxes(NLS::Render::Data::PipelineState pso)
	{
		pso.depthFunc = NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL;

		const auto& scene = GetDescriptor<ForwardSceneDescriptor>();
		size_t skyboxCount = 0;
		for (const auto& [_, drawable] : scene.drawables.skyboxes)
		{
			if (skyboxCount > 0)
			{
				NLS_LOG_WARNING("Multiple skyboxes detected, only the first one will be drawn!");
				break;
			}

			DrawEntity(pso, drawable);
			++skyboxCount;
		}
	}

	void ForwardSceneRenderer::DrawTransparents(NLS::Render::Data::PipelineState pso)
	{
		const auto& scene = GetDescriptor<ForwardSceneDescriptor>();
		for (const auto& [_, drawable] : scene.drawables.transparents)
		{
			DrawEntity(pso, drawable);
		}
	}
}

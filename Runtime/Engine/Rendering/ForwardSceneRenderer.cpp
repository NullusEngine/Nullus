#include "Rendering/ForwardSceneRenderer.h"
#include <fg/Blackboard.hpp>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstring>

#include <Rendering/FrameGraph/FrameGraphExecutionContext.h>
#include <Rendering/FrameGraph/FrameGraphTexture.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/Settings/GraphicsBackendUtils.h>

#include "Rendering/FrameGraphSceneTargets.h"
#include "Rendering/ScenePipelineStatePresets.h"

namespace
{
	struct ForwardOutputData
	{
		FrameGraphResource color = -1;
		FrameGraphResource depth = -1;
	};

	bool ShouldLogSceneRendererDiagnostics()
	{
		static const bool enabled = []()
		{
			return NLS::Render::Settings::IsEnvironmentFlagEnabled("NLS_LOG_RENDER_DRAW_PATH");
		}();
		return enabled;
	}

	bool ShouldSkipSkyboxDrawForDiagnostics()
	{
		static const bool enabled = []()
		{
			return NLS::Render::Settings::IsEnvironmentFlagEnabled("NLS_DIAG_SKIP_SKYBOX_DRAW");
		}();
		return enabled;
	}
}

namespace NLS::Engine::Rendering
{
	ForwardSceneRenderer::ForwardSceneRenderer(NLS::Render::Context::Driver& p_driver)
		: BaseSceneRenderer(p_driver)
	{
	}

	void ForwardSceneRenderer::BeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor)
	{
		BaseSceneRenderer::BeginFrame(p_frameDescriptor);
		auto drawables = ParseScene();
		if (ShouldLogSceneRendererDiagnostics())
		{
			NLS_LOG_INFO(
				"[ForwardSceneRenderer] Parsed scene drawables: opaque=" + std::to_string(drawables.opaques.size()) +
				", transparent=" + std::to_string(drawables.transparents.size()) +
				", skybox=" + std::to_string(drawables.skyboxes.size()));
		}
		AddDescriptor<ForwardSceneDescriptor>({ std::move(drawables) });
	}

	void ForwardSceneRenderer::DrawFrame()
	{
		const auto& frame = GetFrameDescriptor();
		FrameGraph frameGraph;
		frameGraph.reserve(3, frame.outputBuffer ? 2 : 0);
		FrameGraphBlackboard blackboard;

		ImportSceneRenderTargets(frameGraph, blackboard, frame, "ForwardOutputColor", "ForwardOutputDepth");
		if (const auto* importedTargets = blackboard.try_get<SceneRenderTargetsData>())
		{
			if (importedTargets->color >= 0 || importedTargets->depth >= 0)
				blackboard.add<ForwardOutputData>(ForwardOutputData{ importedTargets->color, importedTargets->depth });
		}

		const auto& opaquePass = frameGraph.addCallbackPass<ForwardOutputData>(
			"ForwardOpaque",
			[&blackboard](FrameGraph::Builder& builder, ForwardOutputData& data)
			{
				if (const auto* output = blackboard.try_get<ForwardOutputData>())
				{
					if (output->color >= 0)
						data.color = builder.write(output->color);
					if (output->depth >= 0)
						data.depth = builder.write(output->depth);
				}
				else
					builder.setSideEffect();
			},
			[this, &frame](const ForwardOutputData&, FrameGraphPassResources&, void*)
			{
				const auto clearColor = Maths::Vector4{
					frame.camera->GetClearColor().x,
					frame.camera->GetClearColor().y,
					frame.camera->GetClearColor().z,
					1.0f
				};
				const bool startedRenderPass =
					BeginOutputRenderPass(
						frame.renderWidth,
						frame.renderHeight,
						frame.camera->GetClearColorBuffer(),
						frame.camera->GetClearDepthBuffer(),
						frame.camera->GetClearStencilBuffer(),
						clearColor);
				DrawOpaques(CreateSceneDefaultPipelineState(*this));
				EndOutputRenderPass(startedRenderPass);
			}
		);

		const auto& skyboxPass = frameGraph.addCallbackPass<ForwardOutputData>(
			"ForwardSkybox",
			[&blackboard, &opaquePass](FrameGraph::Builder& builder, ForwardOutputData& data)
			{
				if (opaquePass.color >= 0)
					data.color = builder.write(opaquePass.color);
				else if (const auto* output = blackboard.try_get<ForwardOutputData>(); output != nullptr && output->color >= 0)
					data.color = builder.write(output->color);

				if (opaquePass.depth >= 0)
					data.depth = builder.write(opaquePass.depth);
				else if (const auto* output = blackboard.try_get<ForwardOutputData>(); output != nullptr && output->depth >= 0)
					data.depth = builder.write(output->depth);

				if (data.color < 0 && data.depth < 0)
					builder.setSideEffect();
			},
			[this, &frame](const ForwardOutputData&, FrameGraphPassResources&, void*)
			{
				const bool startedRenderPass =
					BeginOutputRenderPass(
						frame.renderWidth,
						frame.renderHeight,
						false,
						false,
						false);
				DrawSkyboxes(CreateSceneSkyboxPipelineState(*this));
				EndOutputRenderPass(startedRenderPass);
			}
		);

		frameGraph.addCallbackPass<ForwardOutputData>(
			"ForwardTransparent",
			[&blackboard, &skyboxPass](FrameGraph::Builder& builder, ForwardOutputData& data)
			{
				if (skyboxPass.color >= 0)
					data.color = builder.write(skyboxPass.color);
				else if (const auto* output = blackboard.try_get<ForwardOutputData>(); output != nullptr && output->color >= 0)
					data.color = builder.write(output->color);

				if (skyboxPass.depth >= 0)
					data.depth = builder.write(skyboxPass.depth);
				else if (const auto* output = blackboard.try_get<ForwardOutputData>(); output != nullptr && output->depth >= 0)
					data.depth = builder.write(output->depth);

				if (data.color < 0 && data.depth < 0)
					builder.setSideEffect();
			},
			[this, &frame](const ForwardOutputData&, FrameGraphPassResources&, void*)
			{
				const bool startedRenderPass =
					BeginOutputRenderPass(
						frame.renderWidth,
						frame.renderHeight,
						false,
						false,
						false);
				DrawTransparents(CreateSceneDefaultPipelineState(*this));
				EndOutputRenderPass(startedRenderPass);
			}
		);

		frameGraph.compile();
		auto executionContext = CreateFrameGraphExecutionContext();
		frameGraph.execute(&executionContext, &executionContext);

		DrawRegisteredPasses();
	}

	void ForwardSceneRenderer::DrawOpaques(NLS::Render::Data::PipelineState pso)
	{
		const auto& scene = GetDescriptor<ForwardSceneDescriptor>();

		for (const auto& [_, drawable] : scene.drawables.opaques)
		{
			if (drawable.material == nullptr || drawable.mesh == nullptr)
				continue;
			DrawEntity(pso, drawable);
		}
	}

	void ForwardSceneRenderer::DrawSkyboxes(NLS::Render::Data::PipelineState pso)
	{
		const auto& scene = GetDescriptor<ForwardSceneDescriptor>();

		if (ShouldSkipSkyboxDrawForDiagnostics())
		{
			static bool loggedSkip = false;
			if (!loggedSkip)
			{
				loggedSkip = true;
				NLS_LOG_WARNING("[ForwardSceneRenderer] Skipping skybox draw because NLS_DIAG_SKIP_SKYBOX_DRAW is enabled");
			}
			return;
		}

		for (const auto& [_, drawable] : scene.drawables.skyboxes)
		{
			if (drawable.mesh == nullptr || drawable.material == nullptr)
				continue;
			DrawEntity(pso, drawable);
		}
	}

	void ForwardSceneRenderer::DrawTransparents(NLS::Render::Data::PipelineState pso)
	{
		const auto& scene = GetDescriptor<ForwardSceneDescriptor>();

		NLS::Render::Resources::MaterialPipelineStateOverrides transparentOverrides;
		transparentOverrides.depthWrite = false;

		for (const auto& [_, drawable] : scene.drawables.transparents)
		{
			if (drawable.material == nullptr || drawable.mesh == nullptr)
				continue;
			DrawEntity(drawable, transparentOverrides, pso.depthFunc);
		}
	}
}

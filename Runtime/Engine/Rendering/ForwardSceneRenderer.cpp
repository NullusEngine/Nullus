#include "Rendering/ForwardSceneRenderer.h"
#include <fg/Blackboard.hpp>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstring>

#include <Rendering/FrameGraph/FrameGraphExecutionContext.h>
#include <Rendering/FrameGraph/FrameGraphTexture.h>

#include "Rendering/FrameGraphSceneTargets.h"

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
			if (const char* value = std::getenv("NLS_LOG_RENDER_DRAW_PATH"); value != nullptr)
				return std::strcmp(value, "1") == 0 || _stricmp(value, "true") == 0;
			return false;
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
		static std::atomic_uint32_t s_loggedDrawables{ 0u };
		BaseSceneRenderer::BeginFrame(p_frameDescriptor);
		auto drawables = ParseScene();
		if (ShouldLogSceneRendererDiagnostics() || s_loggedDrawables.fetch_add(1u) < 4u)
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
		auto pso = CreatePipelineState();
		const auto& frame = GetFrameDescriptor();
		const auto commandBuffer = GetActiveExplicitCommandBuffer();
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
			[this, pso, &frame, commandBuffer](const ForwardOutputData&, FrameGraphPassResources&, void*)
			{
				const auto clearColor = Maths::Vector4{
					frame.camera->GetClearColor().x,
					frame.camera->GetClearColor().y,
					frame.camera->GetClearColor().z,
					1.0f
				};
				const bool startedRenderPass =
					commandBuffer != nullptr &&
					BeginRecordedRenderPass(
						frame.outputBuffer,
						frame.renderWidth,
						frame.renderHeight,
						frame.camera->GetClearColorBuffer(),
						frame.camera->GetClearDepthBuffer(),
						frame.camera->GetClearStencilBuffer(),
						clearColor);
				DrawOpaques(pso);
				if (startedRenderPass)
					EndRecordedRenderPass();
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
			[this, pso, &frame, commandBuffer](const ForwardOutputData&, FrameGraphPassResources&, void*)
			{
				const bool startedRenderPass =
					commandBuffer != nullptr &&
					BeginRecordedRenderPass(
						frame.outputBuffer,
						frame.renderWidth,
						frame.renderHeight,
						false,
						false,
						false);
				DrawSkyboxes(pso);
				if (startedRenderPass)
					EndRecordedRenderPass();
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
			[this, pso, &frame, commandBuffer](const ForwardOutputData&, FrameGraphPassResources&, void*)
			{
				const bool startedRenderPass =
					commandBuffer != nullptr &&
					BeginRecordedRenderPass(
						frame.outputBuffer,
						frame.renderWidth,
						frame.renderHeight,
						false,
						false,
						false);
				DrawTransparents(pso);
				if (startedRenderPass)
					EndRecordedRenderPass();
			}
		);

		frameGraph.compile();
		auto* frameContext = m_driver.GetCurrentExplicitFrameContext();
		NLS::Render::FrameGraph::FrameGraphExecutionContext executionContext{
			m_driver,
			m_driver.GetExplicitDevice().get(),
			frameContext != nullptr ? frameContext->commandBuffer.get() : nullptr,
			frameContext
		};
		frameGraph.execute(&executionContext, &executionContext);

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

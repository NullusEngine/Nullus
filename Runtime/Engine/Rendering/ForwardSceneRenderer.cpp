#include "Rendering/ForwardSceneRenderer.h"
#include <algorithm>
#include <fg/Blackboard.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstring>

#include <Rendering/FrameGraph/FrameGraphExecutionContext.h>
#include <Rendering/FrameGraph/FrameGraphExecutionPlan.h>
#include <Rendering/FrameGraph/SceneRenderGraphBuilder.h>
#include <Rendering/FrameGraph/FrameGraphTexture.h>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/Settings/GraphicsBackendUtils.h>
#include <Rendering/Settings/DriverSettings.h>

#include "Rendering/ScenePipelineStatePresets.h"

namespace
{
	bool ShouldLogSceneRendererDiagnostics(const NLS::Render::Context::Driver& driver)
	{
		return NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(driver).logRenderDrawPath;
	}

	bool ShouldSkipSkyboxDrawForDiagnostics(const NLS::Render::Context::Driver& driver)
	{
		return NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(driver).diagSkipSkyboxDraw;
	}

}

namespace NLS::Engine::Rendering
{
	ForwardSceneRenderer::ForwardSceneRenderer(NLS::Render::Context::Driver& p_driver)
		: BaseSceneRenderer(p_driver)
	{
	}

	void ForwardSceneRenderer::ExecuteCompiledGraphPass(
		NLS::Render::Context::RenderPassCommandKind kind,
		NLS::Render::Data::PipelineState pipelineState)
	{
		switch (NLS::Render::FrameGraph::GetForwardScenePassExecutionKind(kind))
		{
		case NLS::Render::FrameGraph::ForwardScenePassExecutionKind::Opaque:
			DrawOpaques(pipelineState);
			break;
		case NLS::Render::FrameGraph::ForwardScenePassExecutionKind::Skybox:
			DrawSkyboxes(pipelineState);
			break;
		case NLS::Render::FrameGraph::ForwardScenePassExecutionKind::Transparent:
			DrawTransparents(pipelineState);
			break;
		default:
			break;
		}
	}

	void ForwardSceneRenderer::BeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor)
	{
		NLS_ASSERT(HasFrameObjectBindingProvider(), "ForwardSceneRenderer requires a renderer-owned frame/object binding provider.");
		BaseSceneRenderer::BeginFrame(p_frameDescriptor);
		auto drawables = ParseScene();
		const bool hasSkyboxTexture = !drawables.skyboxes.empty();

		const bool usesThreadedRendering = NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver);
		if (usesThreadedRendering)
		{
			SetActivePreparedPassBindingSet(BaseSceneRenderer::GetPreparedPassBindingSetPlaceholder());
			const auto opaquePso = CreateSceneDefaultPipelineState(*this);
			const auto skyboxPso = CreateSceneSkyboxPipelineState(*this);

			for (const auto& [_, drawable] : drawables.opaques)
			{
				PreparedRecordedDraw preparedDraw;
				if (CaptureThreadedPreparedDraw(opaquePso, drawable, preparedDraw))
					QueueThreadedRecordedDraw(preparedDraw);
			}

			for (const auto& [_, drawable] : drawables.skyboxes)
			{
				PreparedRecordedDraw preparedDraw;
				if (CaptureThreadedPreparedDraw(skyboxPso, drawable, preparedDraw))
					QueueThreadedRecordedDraw(preparedDraw);
			}

			auto transparentOverrides = NLS::Render::Resources::MaterialPipelineStateOverrides{};
			transparentOverrides.depthWrite = false;
			for (const auto& [_, drawable] : drawables.transparents)
			{
				PreparedRecordedDraw preparedDraw;
				if (CaptureThreadedPreparedDraw(drawable, transparentOverrides, opaquePso.depthFunc, preparedDraw))
					QueueThreadedRecordedDraw(preparedDraw);
			}

			SetActivePreparedPassBindingSet(nullptr);
		}

		auto pendingFrameSnapshot = BuildFrameSnapshot(p_frameDescriptor);
		if (pendingFrameSnapshot.has_value())
		{
			RefreshFrameSnapshotVisibility(pendingFrameSnapshot.value(), drawables);
			SetPendingFrameSnapshot(pendingFrameSnapshot.value());
		}

		NLS::Render::Context::RenderScenePackage scenePackage;
		if (!usesThreadedRendering && pendingFrameSnapshot.has_value())
			scenePackage = BuildRenderScenePackage(pendingFrameSnapshot.value());

		if (ShouldLogSceneRendererDiagnostics(m_driver))
		{
			NLS_LOG_INFO(
				"[ForwardSceneRenderer] Parsed scene drawables: opaque=" + std::to_string(drawables.opaques.size()) +
				", transparent=" + std::to_string(drawables.transparents.size()) +
				", skybox=" + std::to_string(drawables.skyboxes.size()));
		}
		AddDescriptor<ForwardSceneDescriptor>({ std::move(drawables), scenePackage, hasSkyboxTexture });

		if (usesThreadedRendering && pendingFrameSnapshot.has_value())
		{
			auto snapshot = pendingFrameSnapshot.value();
			auto lightGridContext = BuildLightGridCompileContext(hasSkyboxTexture);
			auto frameDescriptor = lightGridContext.frameDescriptor;
			SetPendingPreparedRenderSceneBuilder(
				[snapshot = std::move(snapshot),
				 frameDescriptor,
				 lightGridContext = std::move(lightGridContext)]() mutable
			{
				auto package = BuildSnapshotOwnedRenderScenePackage(
					snapshot,
					SnapshotRenderScenePackageBuildMode::SkipDefaultPassInputs);
				NLS::Render::FrameGraph::CompileAndApplyPreparedForwardLightGridSceneExecution(
					package,
					lightGridContext);
				NLS::Render::FrameGraph::FinalizePreparedForwardScenePackage(package, frameDescriptor);
				return package;
			});
		}
	}

	void ForwardSceneRenderer::DrawFrame()
	{
		const bool usesThreadedRendering = NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver);

		// In threaded rendering mode, the Game Thread only captured immutable per-draw inputs.
		// Render-scene package assembly now happens later through the prepared builder path.
		if (!usesThreadedRendering)
		{
			const auto& frame = GetFrameDescriptor();
			FrameGraph frameGraph;
			NLS::Render::FrameGraph::ReserveForwardSceneGraph(frameGraph, frame);
			FrameGraphBlackboard blackboard;
			const auto& scene = GetDescriptor<ForwardSceneDescriptor>();
			const auto lightGridContext = BuildLightGridCompileContext(scene.hasSkyboxTexture);
			const auto preparedGraph = NLS::Render::FrameGraph::PrepareForwardSceneGraph(
				frameGraph,
				blackboard,
				lightGridContext);
			SetActivePreparedPassBindingSet(lightGridContext.graphicsPassBindingSet);

			NLS::Render::FrameGraph::ExecutePreparedForwardSceneGraph(
				frameGraph,
				preparedGraph,
				{
					[this](const auto& beginDesc) -> bool
					{
						return BeginOutputRenderPass(
							beginDesc.renderWidth,
							beginDesc.renderHeight,
							beginDesc.clearColor,
							beginDesc.clearDepth,
							beginDesc.clearStencil,
							beginDesc.clearValue);
					},
					[this](const NLS::Render::FrameGraph::CompiledThreadedRenderSceneGraphPass& compiledPass)
					{
						const auto kind = compiledPass.metadata.commandKind;
						const auto pipelineState = NLS::Render::FrameGraph::GetForwardScenePassPipelineKind(kind) ==
								NLS::Render::FrameGraph::ForwardScenePassPipelineKind::Skybox
							? CreateSceneSkyboxPipelineState(*this)
							: CreateSceneDefaultPipelineState(*this);
						ExecuteCompiledGraphPass(kind, pipelineState);
					},
					[this](bool startedRenderPass, const auto& endDesc)
					{
						(void)endDesc;
						EndOutputRenderPass(startedRenderPass);
					}
				});

			frameGraph.compile();
			auto executionContext = CreateFrameGraphExecutionContext();
			frameGraph.execute(&executionContext, &executionContext);
			SetActivePreparedPassBindingSet(nullptr);
		}

		DrawRegisteredPasses();

		if (usesThreadedRendering)
		{
			const auto& scene = GetDescriptor<ForwardSceneDescriptor>();
			auto pendingFrameSnapshot = BuildFrameSnapshot(m_frameDescriptor);
			if (pendingFrameSnapshot.has_value())
			{
				RefreshFrameSnapshotVisibility(pendingFrameSnapshot.value(), scene.drawables);
				SetPendingFrameSnapshot(pendingFrameSnapshot.value());
			}
		}
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

		if (ShouldSkipSkyboxDrawForDiagnostics(m_driver))
		{
			static bool loggedSkip = false;
			if (!loggedSkip)
			{
				loggedSkip = true;
				NLS_LOG_WARNING("[ForwardSceneRenderer] Skipping skybox draw because --diag-skip-skybox is enabled");
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

#include "Rendering/ForwardSceneRenderer.h"
#include <algorithm>
#include <fg/Blackboard.hpp>
#include <array>
#include <chrono>
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

#include <Profiling/Profiler.h>

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

	NLS::Render::Settings::EComparaisonAlgorithm GetForwardDecalDepthCompare()
	{
		return NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL;
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
		NLS_PROFILE_SCOPE();
		switch (NLS::Render::FrameGraph::GetForwardScenePassExecutionKind(kind))
		{
		case NLS::Render::FrameGraph::ForwardScenePassExecutionKind::Opaque:
			DrawOpaques(pipelineState);
			break;
		case NLS::Render::FrameGraph::ForwardScenePassExecutionKind::Decal:
			DrawDecals(pipelineState);
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
		BeginSceneFrame(p_frameDescriptor, false);
	}

	void ForwardSceneRenderer::BeginFrameForBackgroundPreview(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor)
	{
		BeginSceneFrame(p_frameDescriptor, true);
	}

	BaseSceneRenderer::BackgroundPreviewDrawPrewarmResult
	ForwardSceneRenderer::PrewarmBackgroundPreviewDraws(
		const NLS::Render::Data::FrameDescriptor& frameDescriptor,
		const size_t firstDrawIndex,
		const size_t maxDraws)
	{
		BackgroundPreviewDrawPrewarmResult result;
		result.supported = true;
		BaseSceneRenderer::BeginFrameForBackgroundPreview(frameDescriptor);
		if (!IsFrameActive())
			return result;

		auto drawables = ParseScene();
		result.totalDrawCount =
			drawables.opaques.size() +
			drawables.decals.size() +
			drawables.skyboxes.size() +
			drawables.transparents.size();
		const auto boundedFirstDrawIndex = (std::min)(firstDrawIndex, result.totalDrawCount);
		const auto boundedMaxDraws = (std::max)(size_t {1u}, maxDraws);
		const auto prewarmDeadline =
			std::chrono::steady_clock::now() + std::chrono::microseconds(1000);
		size_t drawIndex = 0u;
		size_t processedDrawCount = 0u;
		auto shouldProcessDraw = [&]()
		{
			const bool selected =
				drawIndex >= boundedFirstDrawIndex &&
				processedDrawCount < boundedMaxDraws &&
				(processedDrawCount == 0u || std::chrono::steady_clock::now() < prewarmDeadline);
			++drawIndex;
			if (selected)
				++processedDrawCount;
			return selected;
		};

		SetActivePreparedPassBindingSet(BaseSceneRenderer::GetPreparedPassBindingSetPlaceholder());
		const auto opaquePso = CreateSceneDefaultPipelineState(*this);
		const auto skyboxPso = CreateSceneSkyboxPipelineState(*this);
		for (const auto& entry : drawables.opaques)
		{
			if (!shouldProcessDraw())
				continue;
			PreparedRecordedDraw preparedDraw;
			(void)CaptureThreadedPreparedDraw(opaquePso, entry.second, preparedDraw);
		}

		auto decalOverrides = NLS::Render::Resources::MaterialPipelineStateOverrides {};
		decalOverrides.depthWrite = false;
		for (const auto& entry : drawables.decals)
		{
			if (!shouldProcessDraw())
				continue;
			PreparedRecordedDraw preparedDraw;
			(void)CaptureThreadedPreparedDraw(
				entry.second,
				decalOverrides,
				GetForwardDecalDepthCompare(),
				"Forward",
				preparedDraw);
		}

		for (const auto& entry : drawables.skyboxes)
		{
			if (!shouldProcessDraw())
				continue;
			PreparedRecordedDraw preparedDraw;
			(void)CaptureThreadedPreparedDraw(skyboxPso, entry.second, preparedDraw);
		}

		auto transparentOverrides = NLS::Render::Resources::MaterialPipelineStateOverrides {};
		transparentOverrides.depthWrite = false;
		for (const auto& entry : drawables.transparents)
		{
			if (!shouldProcessDraw())
				continue;
			PreparedRecordedDraw preparedDraw;
			(void)CaptureThreadedPreparedDraw(
				entry.second,
				transparentOverrides,
				opaquePso.depthFunc,
				"Forward",
				preparedDraw);
		}
		SetActivePreparedPassBindingSet(nullptr);

		result.nextDrawIndex = (std::min)(
			boundedFirstDrawIndex + processedDrawCount,
			result.totalDrawCount);
		result.complete = result.nextDrawIndex >= result.totalDrawCount;
		AbortFrameForBackgroundPreview();
		return result;
	}

	void ForwardSceneRenderer::BeginSceneFrame(
		const NLS::Render::Data::FrameDescriptor& p_frameDescriptor,
		bool backgroundPreview)
	{
		NLS_PROFILE_SCOPE();
		NLS_ASSERT(HasFrameObjectBindingProvider(), "ForwardSceneRenderer requires a renderer-owned frame/object binding provider.");
		if (backgroundPreview)
			BaseSceneRenderer::BeginFrameForBackgroundPreview(p_frameDescriptor);
		else
			BaseSceneRenderer::BeginFrame(p_frameDescriptor);
		if (!IsFrameActive())
			return;
		auto drawables = ParseScene();
		const bool hasSkyboxTexture = !drawables.skyboxes.empty();

		const bool usesThreadedRendering =
			NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver);
		if (usesThreadedRendering)
		{
			SetActivePreparedPassBindingSet(BaseSceneRenderer::GetPreparedPassBindingSetPlaceholder());
			const auto opaquePso = CreateSceneDefaultPipelineState(*this);
			const auto skyboxPso = CreateSceneSkyboxPipelineState(*this);

			for (const auto& entry : drawables.opaques)
			{
				const auto& drawable = entry.second;
				PreparedRecordedDraw preparedDraw;
				if (CaptureThreadedPreparedDraw(opaquePso, drawable, preparedDraw))
					QueueThreadedRecordedDraw(std::move(preparedDraw));
			}

			auto decalOverrides = NLS::Render::Resources::MaterialPipelineStateOverrides{};
			decalOverrides.depthWrite = false;
			for (const auto& entry : drawables.decals)
			{
				const auto& drawable = entry.second;
				PreparedRecordedDraw preparedDraw;
				if (CaptureThreadedPreparedDraw(drawable, decalOverrides, GetForwardDecalDepthCompare(), "Forward", preparedDraw))
					QueueThreadedRecordedDraw(std::move(preparedDraw));
			}

			for (const auto& entry : drawables.skyboxes)
			{
				const auto& drawable = entry.second;
				PreparedRecordedDraw preparedDraw;
				if (CaptureThreadedPreparedDraw(skyboxPso, drawable, preparedDraw))
					QueueThreadedRecordedDraw(std::move(preparedDraw));
			}

			auto transparentOverrides = NLS::Render::Resources::MaterialPipelineStateOverrides{};
			transparentOverrides.depthWrite = false;
			for (const auto& entry : drawables.transparents)
			{
				const auto& drawable = entry.second;
				PreparedRecordedDraw preparedDraw;
				if (CaptureThreadedPreparedDraw(drawable, transparentOverrides, opaquePso.depthFunc, "Forward", preparedDraw))
					QueueThreadedRecordedDraw(std::move(preparedDraw));
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
				", decal=" + std::to_string(drawables.decals.size()) +
				", transparent=" + std::to_string(drawables.transparents.size()) +
				", skybox=" + std::to_string(drawables.skyboxes.size()));
		}
		AddDescriptor<ForwardSceneDescriptor>({ std::move(drawables), scenePackage, hasSkyboxTexture });

		if (usesThreadedRendering && pendingFrameSnapshot.has_value())
		{
			auto snapshot = pendingFrameSnapshot.value();
			auto externalOutputFrameDescriptor =
				NLS::Render::FrameGraph::CaptureExternalSceneOutputSnapshot(p_frameDescriptor);
			auto lightGridContext = BuildLightGridCompileContext(hasSkyboxTexture);
			SetPendingPreparedRenderSceneBuilder(
				[snapshot = std::move(snapshot),
				 externalOutputFrameDescriptor = std::move(externalOutputFrameDescriptor),
				 lightGridContext = std::move(lightGridContext)]() mutable
			{
				auto package = BuildSnapshotOwnedRenderScenePackage(
					snapshot,
					SnapshotRenderScenePackageBuildMode::SkipDefaultPassInputs);
				NLS::Render::FrameGraph::CompileAndApplyPreparedForwardLightGridSceneExecution(
					package,
					lightGridContext);
				NLS::Render::FrameGraph::FinalizePreparedForwardScenePackage(
					package,
					externalOutputFrameDescriptor);
				return package;
			});
		}
	}

	void ForwardSceneRenderer::DrawFrame()
	{
		NLS_PROFILE_SCOPE();
		const bool usesThreadedRendering =
			NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver);

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
		NLS_PROFILE_SCOPE();
		const auto& scene = GetDescriptor<ForwardSceneDescriptor>();

		for (const auto& entry : scene.drawables.opaques)
		{
			const auto& drawable = entry.second;
			if (drawable.material == nullptr || drawable.mesh == nullptr)
				continue;
			DrawEntity(pso, drawable);
		}
	}

	void ForwardSceneRenderer::DrawSkyboxes(NLS::Render::Data::PipelineState pso)
	{
		NLS_PROFILE_SCOPE();
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

		for (const auto& entry : scene.drawables.skyboxes)
		{
			const auto& drawable = entry.second;
			if (drawable.mesh == nullptr || drawable.material == nullptr)
				continue;
			DrawEntity(pso, drawable);
		}
	}

	void ForwardSceneRenderer::DrawDecals(NLS::Render::Data::PipelineState pso)
	{
		NLS_PROFILE_SCOPE();
		const auto& scene = GetDescriptor<ForwardSceneDescriptor>();

		NLS::Render::Resources::MaterialPipelineStateOverrides decalOverrides;
		decalOverrides.depthWrite = false;

		for (const auto& entry : scene.drawables.decals)
		{
			const auto& drawable = entry.second;
			if (drawable.material == nullptr || drawable.mesh == nullptr)
				continue;
			DrawEntity(drawable, decalOverrides, GetForwardDecalDepthCompare(), "Forward");
		}
	}

	void ForwardSceneRenderer::DrawTransparents(NLS::Render::Data::PipelineState pso)
	{
		NLS_PROFILE_SCOPE();
		const auto& scene = GetDescriptor<ForwardSceneDescriptor>();

		NLS::Render::Resources::MaterialPipelineStateOverrides transparentOverrides;
		transparentOverrides.depthWrite = false;

		for (const auto& entry : scene.drawables.transparents)
		{
			const auto& drawable = entry.second;
			if (drawable.material == nullptr || drawable.mesh == nullptr)
				continue;
			DrawEntity(drawable, transparentOverrides, pso.depthFunc, "Forward");
		}
	}
}

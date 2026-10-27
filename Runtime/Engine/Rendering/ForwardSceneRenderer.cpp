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
	struct CaptureFrameSlotScope
	{
		NLS::Render::Core::FrameObjectBindingProvider* provider = nullptr;

		CaptureFrameSlotScope(
			NLS::Render::Core::FrameObjectBindingProvider* p_provider,
			const size_t slotIndex, const std::function<void(size_t)>& bind)
			: provider(p_provider)
		{
			if (provider != nullptr)
				bind(slotIndex);
		}

		~CaptureFrameSlotScope()
		{
			if (provider != nullptr)
				provider->UnbindCaptureFrameSlot();
		}

		CaptureFrameSlotScope(const CaptureFrameSlotScope&) = delete;
		CaptureFrameSlotScope& operator=(const CaptureFrameSlotScope&) = delete;
	};

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
		const size_t maxDraws,
		const std::chrono::microseconds timeBudget)
	{
		BackgroundPreviewDrawPrewarmResult result;
		result.supported = true;
		BaseSceneRenderer::BeginFrameForBackgroundPreview(frameDescriptor);
		if (!IsFrameActive())
			return result;

		auto drawables = ParseScene(
			NLS::Render::FrameGraph::FreezeFrameDescriptor(frameDescriptor));
		result.totalDrawCount =
			drawables.opaques.size() +
			drawables.decals.size() +
			drawables.skyboxes.size() +
			drawables.transparents.size();
		const auto boundedFirstDrawIndex = (std::min)(firstDrawIndex, result.totalDrawCount);
		const auto boundedMaxDraws = (std::max)(size_t {1u}, maxDraws);
		const auto prewarmDeadline =
			std::chrono::steady_clock::now() + timeBudget;
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

	bool ForwardSceneRenderer::TryPublishThreadedFrame()
	{
		if (m_skipThreadedFramePublish)
		{
			// No prepared builder was published for this frame, so there is nothing
			// for the render thread to resolve. Reporting the failure keeps the
			// lifecycle from holding a slot for a frame that will never be drawn.
			return false;
		}

		return BaseSceneRenderer::TryPublishThreadedFrame();
	}

		NLS::Render::Context::RenderSceneBuildCommand ForwardSceneRenderer::BuildRenderSceneCommand(
			const NLS::Render::Context::RenderFramePacket& packet)
		{
			if (m_pendingSceneFrameInput != nullptr)
			{
				*m_pendingSceneFrameInput = packet;
				*m_pendingHelperDrawCommands = std::move(m_threadedRecordedDrawCommands);
			}
			return BaseSceneRenderer::BuildRenderSceneCommand(packet);
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
		// Cleared per frame: a refusal on one frame must not suppress the next.
		m_skipThreadedFramePublish = false;
			m_pendingSceneFrameInput.reset();
		m_pendingHelperDrawCommands.reset();
		auto threadedCaptureOutput =
			std::make_shared<std::vector<NLS::Render::Context::RecordedDrawCommandInput>>();

		// Owns its camera. CaptureExternalSceneOutputSnapshot keeps the live camera
		// pointer, which work running on the render thread would then read while the
		// game thread advances the next frame.
		auto frozenFrame =
			NLS::Render::FrameGraph::FreezeFrameDescriptor(p_frameDescriptor);

		// Game-thread half of the parse: it captures which scenes this frame renders
		// and collects each one's proxy update batch. The resolve half - apply,
		// rebuild, gather, sort - is the expensive part, and in threaded mode it runs
		// on the render thread inside the prepared builder so it shares that thread
		// with the frame it produces.
		// Shared, not a local: the dropper below keeps a reference so a dropped frame
		// can still release its claims after this scope ends.
		auto parseInput = std::make_shared<SceneParseInput>(BeginSceneParse(frozenFrame));
		// Built through the renderer so the dropper observes this renderer's
		// liveness token: a builder the driver retains past renderer destruction
		// must not call back through a dangling pointer.
		SceneParseCaptureDropper parseDropper = MakeSceneParseCaptureDropper(parseInput);

		if (parseInput->captureRefused)
		{
			// The game thread is at least one frame ahead of the render thread and
			// the render scene refused the new capture, so this frame describes no
			// scene. Publishing it would present an empty frame; dropping it keeps
			// the last good frame on screen. The dropper below releases the claims
			// this frame did obtain.
			m_skipThreadedFramePublish = true;
			if (ShouldLogSceneRendererDiagnostics(m_driver))
			{
				NLS_LOG_WARNING(
					"[ForwardSceneRenderer] Dropping threaded frame: the render scene "
					"refused a new capture because the previous one is still outstanding.");
			}
			return;
		}

		auto pendingPacket = BuildRenderFramePacket(p_frameDescriptor);
		if (pendingPacket.has_value())
		{
			SetPendingRenderFramePacket(pendingPacket.value());
		}

		AddDescriptor<ForwardSceneDescriptor>({ nullptr, false });

		if (pendingPacket.has_value())
		{
				m_pendingSceneFrameInput = std::make_shared<NLS::Render::Context::RenderFramePacket>(
					pendingPacket.value());
			m_pendingHelperDrawCommands = std::make_shared<std::vector<NLS::Render::Context::RecordedDrawCommandInput>>();
			SetPendingRenderSceneCommand(
				[this,
				 basePipelineState = CreatePipelineState(),
				 bindCaptureFrame = GetFrameObjectBindingProvider()->FreezeCaptureFrame(),
					 frameInput = m_pendingSceneFrameInput,
				 helperCommands = m_pendingHelperDrawCommands,
			 parseInput = std::move(parseInput),
				parseDropper = std::move(parseDropper),
				 recordedDrawCommands = threadedCaptureOutput,
			 frozenFrame = std::move(frozenFrame)](const size_t lifecycleSlotIndex) mutable
				{
					const auto& packet = *frameInput;
					NLS::Render::Context::RenderSceneBuildData snapshot;
					snapshot.visibleHelperDrawCount = static_cast<uint64_t>(helperCommands->size());
					snapshot.recordedDrawCommands = std::move(*helperCommands);
					PreparedFrameScope preparedFrameScope(*this, frozenFrame.descriptor, basePipelineState);
						std::optional<NLS::Render::Context::ScopedFrameResourceContext> frameResources;
						if (NLS::Render::Context::DriverRendererAccess::GetFrameContextSlotCount(m_driver) != 0u)
							frameResources.emplace(m_driver, lifecycleSlotIndex);
					CaptureFrameSlotScope captureFrameSlotScope(GetFrameObjectBindingProvider(), lifecycleSlotIndex, bindCaptureFrame);
					SetActivePreparedPassBindingSet(BaseSceneRenderer::GetPreparedPassBindingSetPlaceholder());

				// Resolve half of the parse, on this thread: apply the captured proxy
				// batches, rebuild the primitive index, gather and sort. Running here
				// means the expensive part of the frame shares a thread with the
				// frame it produces, and the game thread only paid for the capture.
				auto drawables = std::make_shared<AllDrawables>(ResolveSceneParse(*parseInput, frozenFrame));
				// Resolved: the claims are released, so the dropper must not apply the
				// batch a second time when this builder is eventually discarded.
				parseDropper.MarkResolved();
				RefreshFrameBuildMetadata(snapshot);
				const bool hasSkyboxTexture = !drawables->skyboxes.empty();

				// Built here rather than on the game thread: this reads the lighting
				// provider's published snapshot, which the resolve above has just
				// refreshed, so the light grid sees this frame's lighting. It also
				// depends on whether the resolved scene has a skybox, which is only
				// known once the drawables exist.
				const auto lightGridCapture = CaptureLightGridFrameInputs(hasSkyboxTexture);
				const auto lightGridContext =
					BuildLightGridCompileContext(frozenFrame, lightGridCapture);

				if (ShouldLogSceneRendererDiagnostics(m_driver))
				{
					NLS_LOG_INFO(
						"[ForwardSceneRenderer] Resolved scene drawables: opaque=" +
						std::to_string(drawables->opaques.size()) +
						", decal=" + std::to_string(drawables->decals.size()) +
						", transparent=" + std::to_string(drawables->transparents.size()) +
						", skybox=" + std::to_string(drawables->skyboxes.size()));
				}

					const auto opaquePso = CreateSceneDefaultPipelineState(*this);
					const auto skyboxPso = CreateSceneSkyboxPipelineState(*this);
					uint64_t opaqueDrawCount = 0u;
					uint64_t decalDrawCount = 0u;
					uint64_t skyboxDrawCount = 0u;
					uint64_t transparentDrawCount = 0u;

					for (const auto& entry : drawables->opaques)
					{
						PreparedRecordedDraw preparedDraw;
						if (CaptureThreadedPreparedDraw(opaquePso, entry.second, preparedDraw) &&
							QueueThreadedRecordedDraw(std::move(preparedDraw), *recordedDrawCommands))
							++opaqueDrawCount;
					}

					auto decalOverrides = NLS::Render::Resources::MaterialPipelineStateOverrides{};
					decalOverrides.depthWrite = false;
					for (const auto& entry : drawables->decals)
					{
						PreparedRecordedDraw preparedDraw;
						if (CaptureThreadedPreparedDraw(
								entry.second,
								decalOverrides,
								GetForwardDecalDepthCompare(),
								"Forward",
								preparedDraw) &&
							QueueThreadedRecordedDraw(std::move(preparedDraw), *recordedDrawCommands))
							++decalDrawCount;
					}

					for (const auto& entry : drawables->skyboxes)
					{
						PreparedRecordedDraw preparedDraw;
						if (CaptureThreadedPreparedDraw(skyboxPso, entry.second, preparedDraw) &&
							QueueThreadedRecordedDraw(std::move(preparedDraw), *recordedDrawCommands))
							++skyboxDrawCount;
					}

					auto transparentOverrides = NLS::Render::Resources::MaterialPipelineStateOverrides{};
					transparentOverrides.depthWrite = false;
					for (const auto& entry : drawables->transparents)
					{
						PreparedRecordedDraw preparedDraw;
						if (CaptureThreadedPreparedDraw(
								entry.second,
								transparentOverrides,
								opaquePso.depthFunc,
								"Forward",
								preparedDraw) &&
							QueueThreadedRecordedDraw(std::move(preparedDraw), *recordedDrawCommands))
							++transparentDrawCount;
					}
					SetActivePreparedPassBindingSet(nullptr);

					snapshot.visibleOpaqueDrawCount = opaqueDrawCount;
					snapshot.visibleDecalDrawCount = decalDrawCount;
					snapshot.visibleSkyboxDrawCount = skyboxDrawCount;
					snapshot.visibleTransparentDrawCount = transparentDrawCount;
					// The rendered draw list is this frame's largest payload and the
					// builder holds its only remaining owner, so both hops move instead
					// of copying the whole command vector twice.
						auto helperDrawCommands = std::move(snapshot.recordedDrawCommands);
						snapshot.recordedDrawCommands = std::move(*recordedDrawCommands);
						for (auto& command : helperDrawCommands)
							snapshot.recordedDrawCommands.push_back(std::move(command));
					recordedDrawCommands->clear();

					auto package = NLS::Render::Context::BuildRenderScenePackage(
						packet,
						std::move(snapshot),
						NLS::Render::Context::RenderScenePackageBuildMode::SkipDefaultPassInputs);
					NLS::Render::FrameGraph::CompileAndApplyPreparedForwardLightGridSceneExecution(
						package,
						lightGridContext);
					NLS::Render::FrameGraph::FinalizePreparedForwardScenePackage(
						package,
						frozenFrame.descriptor);
						AppendParticleScenePass(package,frozenFrame,drawables->particles);
                        AppendDistanceFieldScenePass(package,frozenFrame,GetDistanceFieldDebugSettings(),packet.frameId);
						NLS::Render::Context::DriverRendererAccess::PrepareSwapchainSceneOutput(m_driver, package);
					return package;
				});
		}
	}

	void ForwardSceneRenderer::DrawFrame()
	{
		NLS_PROFILE_SCOPE();
		DrawRegisteredPasses();
	}

	void ForwardSceneRenderer::DrawOpaques(NLS::Render::Data::PipelineState pso)
	{
		NLS_PROFILE_SCOPE();
		const auto& scene = GetDescriptor<ForwardSceneDescriptor>();

		for (const auto& entry : scene.drawables->opaques)
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

		for (const auto& entry : scene.drawables->skyboxes)
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

		for (const auto& entry : scene.drawables->decals)
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

		for (const auto& entry : scene.drawables->transparents)
		{
			const auto& drawable = entry.second;
			if (drawable.material == nullptr || drawable.mesh == nullptr)
				continue;
			DrawEntity(drawable, transparentOverrides, pso.depthFunc, "Forward");
		}
	}
}

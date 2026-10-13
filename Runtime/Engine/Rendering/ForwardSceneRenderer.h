#pragma once

#include <memory>

#include <fg/FrameGraph.hpp>

#include "Rendering/BaseSceneRenderer.h"

namespace NLS::Engine::Rendering
{
	class NLS_ENGINE_API ForwardSceneRenderer : public BaseSceneRenderer
	{
	public:
		explicit ForwardSceneRenderer(NLS::Render::Context::Driver& p_driver);

		void BeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor) override;
		void BeginFrameForBackgroundPreview(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor) override;
		BackgroundPreviewDrawPrewarmResult PrewarmBackgroundPreviewDraws(
			const NLS::Render::Data::FrameDescriptor& frameDescriptor,
			size_t firstDrawIndex,
			size_t maxDraws,
			std::chrono::microseconds timeBudget = std::chrono::milliseconds(1)) override;
		void DrawFrame() override;

	protected:
		virtual void DrawOpaques(NLS::Render::Data::PipelineState pso);
		virtual void DrawDecals(NLS::Render::Data::PipelineState pso);
		virtual void DrawSkyboxes(NLS::Render::Data::PipelineState pso);
		virtual void DrawTransparents(NLS::Render::Data::PipelineState pso);

	private:
		/**
		 * Drops the threaded publish for this frame.
		 *
		 * A frame whose scene capture was refused carries no scene at all, so
		 * publishing it would present an empty frame instead of the previous one.
		 */
		bool TryPublishThreadedFrame() override;
			NLS::Render::Context::RenderSceneBuildCommand BuildRenderSceneCommand(
                const NLS::Render::Context::RenderFramePacket& packet) override;

		void BeginSceneFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor, bool backgroundPreview);

		void ExecuteCompiledGraphPass(
			NLS::Render::Context::RenderPassCommandKind kind,
			NLS::Render::Data::PipelineState pipelineState);

			struct ForwardSceneDescriptor
			{
				/**
				 * Resolved drawables for the inline path. Null in threaded mode: the
				 * game thread only captures, and the prepared builder resolves and
				 * consumes its own drawables without publishing them here.
				 */
				std::shared_ptr<AllDrawables> drawables;
				/**
				 * Only meaningful when `drawables` is set, because the inline path is
				 * the only one that draws from this descriptor.
				 */
			bool hasSkyboxTexture = false;
		};

		// Set while the current frame's scene capture was refused. Cleared at the
		// start of every BeginFrame so one refusal cannot suppress later frames.
		bool m_skipThreadedFramePublish = false;
            std::shared_ptr<NLS::Render::Context::RenderFramePacket> m_pendingSceneFrameInput;
            std::shared_ptr<std::vector<NLS::Render::Context::RecordedDrawCommandInput>> m_pendingHelperDrawCommands;
	};
}

#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include <Rendering/Buffers/MultiFramebuffer.h>

#include "Rendering/BaseSceneRenderer.h"

namespace NLS::Render::Resources
{
	class Material;
	class Mesh;
	class Shader;
	class Texture2D;
	class TextureCube;
}

namespace NLS::Engine::Rendering
{
	class NLS_ENGINE_API DeferredSceneRenderer : public BaseSceneRenderer
	{
	public:
		explicit DeferredSceneRenderer(NLS::Render::Context::Driver& p_driver);
		~DeferredSceneRenderer() override;

		void BeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor) override;
		void DrawFrame() override;

	protected:
		struct DeferredSceneDescriptor
		{
			AllDrawables drawables;
			NLS::Render::Context::RenderScenePackage scenePackage;
			bool hasSkyboxTexture = false;
		};

		static void SynchronizeThreadedDeferredSnapshot(
			NLS::Render::Context::FrameSnapshot& snapshot,
			uint64_t queuedGBufferDrawCount);
		NLS::Render::FrameGraph::DeferredPreparedSceneResourceRequest BuildDeferredPreparedSceneResourceRequest() const;
		void LogPreparedDrawResult(
			const char* stage,
			bool captured,
			bool queued,
			const PreparedRecordedDraw& preparedDraw) const;
		NLS::Render::Context::PreparedRenderSceneBuilder BuildDeferredPreparedRenderSceneBuilder(
			NLS::Render::Context::FrameSnapshot snapshot,
			bool hasSkyboxTexture,
			const std::vector<NLS::Render::Context::RenderPassCommandInput>& appendedPassInputs = {},
			const std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata>& appendedPassMetadata = {},
			std::shared_ptr<NLS::Render::RHI::RHITexture> preferredReadbackTexture = nullptr,
			uint64_t additionalRenderTargetUseCount = 0u) const;
		NLS::Render::Context::PreparedRenderSceneBuilder BuildPreparedRenderSceneBuilder(
			const NLS::Render::Context::FrameSnapshot& snapshot) const override;

	private:
		void LoadPipelineResources();
		void EnsureGBufferTargets(uint16_t width, uint16_t height);
		std::unique_ptr<NLS::Render::Resources::Material> CreateGBufferMaterial() const;
		NLS::Render::Resources::Material& GetOrCreateGBufferMaterial(NLS::Render::Resources::Material& sourceMaterial);
		void SyncGBufferMaterial(NLS::Render::Resources::Material& target, const NLS::Render::Resources::Material& sourceMaterial) const;
		void DrawGBufferOpaques(NLS::Render::Data::PipelineState pso);
		void DrawLightingPass(NLS::Render::Data::PipelineState pso);

	private:
		NLS::Render::Buffers::MultiFramebuffer m_gBuffer;
		std::unique_ptr<NLS::Render::Resources::Mesh> m_fullscreenQuad;
		std::unique_ptr<NLS::Render::Resources::Material> m_lightingMaterial;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_gBufferAlbedoTexture;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_gBufferNormalTexture;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_gBufferMaterialTexture;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_gBufferDepthTexture;
		std::unordered_map<std::string, std::unique_ptr<NLS::Render::Resources::Material>> m_gBufferMaterialCache;
		NLS::Render::Resources::Shader* m_gBufferShader = nullptr;
		NLS::Render::Resources::Shader* m_lightingShader = nullptr;
		uint64_t m_threadedQueuedGBufferDrawCount = 0u;
		uint64_t m_threadedQueuedLightingDrawCount = 0u;
	};
}

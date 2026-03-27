#pragma once

#include <memory>
#include <unordered_map>

#include <fg/FrameGraph.hpp>

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

namespace NLS::Render::Buffers
{
	class UniformBuffer;
}

namespace NLS::Engine::Rendering
{
	class NLS_ENGINE_API DeferredSceneRenderer final : public BaseSceneRenderer
	{
	public:
		explicit DeferredSceneRenderer(NLS::Render::Context::Driver& p_driver);
		~DeferredSceneRenderer() override;

		void BeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor) override;
		void DrawFrame() override;

	private:
		struct DeferredSceneDescriptor
		{
			AllDrawables drawables;
		};

		void LoadPipelineResources();
		void EnsureGBufferTargets(uint16_t width, uint16_t height);
		NLS::Render::Resources::Material& GetOrCreateGBufferMaterial(NLS::Render::Resources::Material& sourceMaterial);
		void SyncGBufferMaterial(NLS::Render::Resources::Material& target, const NLS::Render::Resources::Material& sourceMaterial) const;
		void DrawGBufferOpaques(NLS::Render::Data::PipelineState pso);
		void DrawLightingPass(NLS::Render::Data::PipelineState pso);

	private:
		NLS::Render::Buffers::MultiFramebuffer m_gBuffer;
		std::unique_ptr<NLS::Render::Buffers::UniformBuffer> m_passBuffer;
		std::unique_ptr<NLS::Render::Resources::Mesh> m_fullscreenQuad;
		std::unique_ptr<NLS::Render::Resources::Material> m_lightingMaterial;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_gBufferAlbedoTexture;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_gBufferNormalTexture;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_gBufferMaterialTexture;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_gBufferDepthTexture;
		std::unordered_map<const NLS::Render::Resources::Material*, std::unique_ptr<NLS::Render::Resources::Material>> m_gBufferMaterialCache;
		NLS::Render::Resources::Shader* m_gBufferShader = nullptr;
		NLS::Render::Resources::Shader* m_lightingShader = nullptr;
	};
}

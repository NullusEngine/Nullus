#pragma once

#include <memory>

#include <fg/FrameGraph.hpp>

#include <Rendering/Buffers/MultiFramebuffer.h>
#include <Rendering/Buffers/ShaderStorageBuffer.h>
#include <Rendering/Features/LightingRenderFeature.h>
#include <Rendering/Resources/Material.h>
#include <Rendering/Resources/Mesh.h>
#include <Rendering/Resources/Shader.h>
#include <Rendering/Resources/Texture2D.h>

#include "Rendering/ClusteredShading.h"
#include "Rendering/SceneRenderer.h"

namespace NLS::Engine::Rendering
{
	class NLS_ENGINE_API DeferredSceneRenderer : public SceneRenderer
	{
	public:
		DeferredSceneRenderer(NLS::Render::Context::Driver& p_driver);

		void BeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor) override;
		void ExecuteDeferredPass(NLS::Render::Data::PipelineState pso);
		void DrawSkyboxes(NLS::Render::Data::PipelineState pso);
		void DrawTransparents(NLS::Render::Data::PipelineState pso);

	private:
		struct DeferredSceneDescriptor
		{
			OpaqueDrawables opaques;
			TransparentDrawables transparents;
			SkyboxDrawables skyboxes;
			NLS::Render::Features::LightingRenderFeature::LightSet lights;
			ClusteredLightGrid clusteredLights;
		};

		void EnsureFrameResources(uint16_t width, uint16_t height);
		void DrawDeferredGeometryPass(const DeferredSceneDescriptor& scene, NLS::Render::Data::PipelineState pso);
		void DrawDeferredLightingPass(const DeferredSceneDescriptor& scene, NLS::Render::Data::PipelineState pso);
		NLS::Render::Resources::Material BuildGeometryMaterial(const NLS::Render::Resources::Material& source) const;

	private:
		NLS::Render::Buffers::MultiFramebuffer m_gbuffer;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_albedoTexture;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_positionTexture;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_normalTexture;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_materialTexture;
		std::unique_ptr<NLS::Render::Resources::Texture2D> m_depthTexture;

		NLS::Render::Resources::Mesh m_fullscreenTriangle;
		NLS::Render::Resources::Shader* m_deferredLightingShader = nullptr;

		std::unique_ptr<NLS::Render::Buffers::ShaderStorageBuffer> m_clusterRecordsBuffer;
		std::unique_ptr<NLS::Render::Buffers::ShaderStorageBuffer> m_clusterIndicesBuffer;

		ClusteredShadingSettings m_clusterSettings;
		uint16_t m_gbufferWidth = 0;
		uint16_t m_gbufferHeight = 0;
	};
}

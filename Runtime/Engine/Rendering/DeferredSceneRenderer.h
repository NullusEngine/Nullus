#pragma once

#include <memory>

#include <fg/FrameGraph.hpp>

#include <Rendering/Buffers/ShaderStorageBuffer.h>
#include <Rendering/Features/LightingRenderFeature.h>
#include <Rendering/Resources/Material.h>
#include <Rendering/Resources/Mesh.h>
#include <Rendering/Resources/Shader.h>
#include <Rendering/Resources/Texture2D.h>

#include "Rendering/ClusteredShading.h"
#include "Rendering/BaseSceneRenderer.h"

namespace NLS::Engine::Rendering
{
	class NLS_ENGINE_API DeferredSceneRenderer : public BaseSceneRenderer
	{
	public:
        using PipelineState = Render::Data::PipelineState;
        using Material = Render::Resources::Material;
        using Mesh = Render::Resources::Mesh;
        using Shader = Render::Resources::Shader;
        using ShaderStorageBuffer = Render::Buffers::ShaderStorageBuffer;
        using LightSet = Render::Features::LightingRenderFeature::LightSet;

		DeferredSceneRenderer(Driver& p_driver);
		~DeferredSceneRenderer() override;

		void BeginFrame(const Render::Data::FrameDescriptor& p_frameDescriptor) override;
		void DrawFrame() override;

	private:
		struct DeferredSceneDescriptor
		{
			OpaqueDrawables opaques;
			TransparentDrawables transparents;
			SkyboxDrawables skyboxes;
			LightSet lights;
			ClusteredLightGrid clusteredLights;
		};

		void EnsureGBufferFramebuffer();
		void DrawOpaques(PipelineState pso);
		void DrawDeferredGeometryPass(const DeferredSceneDescriptor& scene, PipelineState pso);
		void DrawDeferredLightingPass(const DeferredSceneDescriptor& scene, PipelineState pso, uint32_t sourceFramebuffer);
		void DrawSkyboxes(PipelineState pso);
		void DrawTransparents(PipelineState pso);
		Material BuildGeometryMaterial(const Material& source) const;

	private:
		Mesh m_fullscreenTriangle;
		Shader* m_deferredLightingShader = nullptr;

		std::unique_ptr<ShaderStorageBuffer> m_clusterRecordsBuffer;
		std::unique_ptr<ShaderStorageBuffer> m_clusterIndicesBuffer;

		ClusteredShadingSettings m_clusterSettings;
		uint32_t m_gbufferFramebuffer = 0;
	};
}

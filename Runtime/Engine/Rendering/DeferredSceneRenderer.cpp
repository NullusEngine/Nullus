#include "Rendering/DeferredSceneRenderer.h"

#include <glad/glad.h>
#include <fg/Blackboard.hpp>

#include <vector>

#include <Core/ResourceManagement/ShaderManager.h>
#include <Core/ServiceLocator.h>
#include <Rendering/FrameGraph/OpenGLFrameGraphTexture.h>

#include "Components/MaterialRenderer.h"
#include "Components/MeshRenderer.h"
#include "Rendering/EngineDrawableDescriptor.h"

namespace
{
	constexpr GLint kClusterDimensionsLocation = 0;
	constexpr GLint kScreenSizeLocation = 1;
	constexpr GLint kNearFarLocation = 2;
	constexpr GLint kClusterLightIndexCountLocation = 3;
	constexpr GLint kCameraWorldPosLocation = 4;

	NLS::Render::Resources::Mesh BuildFullscreenTriangle()
	{
		using Vertex = NLS::Render::Geometry::Vertex;
		std::vector<Vertex> vertices(3);

		vertices[0].position[0] = -1.0f;
		vertices[0].position[1] = -1.0f;
		vertices[0].position[2] = 0.0f;
		vertices[0].texCoords[0] = 0.0f;
		vertices[0].texCoords[1] = 0.0f;

		vertices[1].position[0] = 3.0f;
		vertices[1].position[1] = -1.0f;
		vertices[1].position[2] = 0.0f;
		vertices[1].texCoords[0] = 2.0f;
		vertices[1].texCoords[1] = 0.0f;

		vertices[2].position[0] = -1.0f;
		vertices[2].position[1] = 3.0f;
		vertices[2].position[2] = 0.0f;
		vertices[2].texCoords[0] = 0.0f;
		vertices[2].texCoords[1] = 2.0f;

		return NLS::Render::Resources::Mesh(vertices, {}, 0);
	}
}

namespace NLS::Engine::Rendering
{
	DeferredSceneRenderer::DeferredSceneRenderer(NLS::Render::Context::Driver& p_driver)
		: BaseSceneRenderer(p_driver)
		, m_fullscreenTriangle(BuildFullscreenTriangle())
	{
		m_deferredLightingShader = NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager)[":Shaders/DeferredLighting.glsl"];

		m_clusterRecordsBuffer = std::make_unique<NLS::Render::Buffers::ShaderStorageBuffer>(NLS::Render::Settings::EAccessSpecifier::STREAM_DRAW);
		m_clusterIndicesBuffer = std::make_unique<NLS::Render::Buffers::ShaderStorageBuffer>(NLS::Render::Settings::EAccessSpecifier::STREAM_DRAW);
	}

	void DeferredSceneRenderer::BeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor)
	{
		BaseSceneRenderer::BeginFrame(p_frameDescriptor);

		EnsureFrameResources(p_frameDescriptor.renderWidth, p_frameDescriptor.renderHeight);

		auto drawables = ParseScene();
		const auto& lighting = GetDescriptor<NLS::Render::Features::LightingRenderFeature::LightingDescriptor>();
		const auto& lights = lighting.lights;
		const auto clusteredLights = BuildClusteredLightGrid(
			m_clusterSettings,
			lights,
			*p_frameDescriptor.camera,
			p_frameDescriptor.camera->GetViewMatrix(),
			p_frameDescriptor.camera->GetProjectionMatrix(),
			p_frameDescriptor.renderWidth,
			p_frameDescriptor.renderHeight,
			p_frameDescriptor.camera->GetNear(),
			p_frameDescriptor.camera->GetFar()
		);

		if (clusteredLights.records.empty())
		{
			static ClusterRecord emptyRecord{};
			m_clusterRecordsBuffer->SendBlocks(&emptyRecord, sizeof(ClusterRecord));
		}
		else
		{
			m_clusterRecordsBuffer->SendBlocks(
				const_cast<ClusterRecord*>(clusteredLights.records.data()),
				clusteredLights.records.size() * sizeof(ClusterRecord)
			);
		}

		if (clusteredLights.lightIndices.empty())
		{
			uint32_t emptyIndex = 0;
			m_clusterIndicesBuffer->SendBlocks(&emptyIndex, sizeof(uint32_t));
		}
		else
		{
			m_clusterIndicesBuffer->SendBlocks(
				clusteredLights.lightIndices.data(),
				clusteredLights.lightIndices.size() * sizeof(uint32_t)
			);
		}

		AddDescriptor<DeferredSceneDescriptor>({
			std::move(drawables.opaques),
			std::move(drawables.transparents),
			std::move(drawables.skyboxes),
			lights,
			clusteredLights
		});
	}

	void DeferredSceneRenderer::DrawFrame()
	{
		auto pso = CreatePipelineState();
		const auto& scene = GetDescriptor<DeferredSceneDescriptor>();

		FrameGraph frameGraph;
		frameGraph.reserve(4, 0);

		frameGraph.addCallbackPass(
			"DeferredOpaque",
			[](FrameGraph::Builder& builder, FrameGraph::NoData&)
			{
				builder.setSideEffect();
			},
			[this, pso](const FrameGraph::NoData&, FrameGraphPassResources&, void*)
			{
				DrawOpaques(pso);
			}
		);

		frameGraph.addCallbackPass(
			"DeferredLighting",
			[](FrameGraph::Builder& builder, FrameGraph::NoData&)
			{
				builder.setSideEffect();
			},
			[this, &scene, pso](const FrameGraph::NoData&, FrameGraphPassResources&, void*)
			{
				DrawDeferredLightingPass(scene, pso);
			}
		);

		frameGraph.addCallbackPass(
			"DeferredSkybox",
			[](FrameGraph::Builder& builder, FrameGraph::NoData&)
			{
				builder.setSideEffect();
			},
			[this, pso](const FrameGraph::NoData&, FrameGraphPassResources&, void*)
			{
				DrawSkyboxes(pso);
			}
		);

		frameGraph.addCallbackPass(
			"DeferredTransparent",
			[](FrameGraph::Builder& builder, FrameGraph::NoData&)
			{
				builder.setSideEffect();
			},
			[this, pso](const FrameGraph::NoData&, FrameGraphPassResources&, void*)
			{
				DrawTransparents(pso);
			}
		);

		frameGraph.compile();
		frameGraph.execute();

		DrawRegisteredPasses(CreatePipelineState());
	}

	void DeferredSceneRenderer::EnsureFrameResources(uint16_t width, uint16_t height)
	{
		if (width == 0 || height == 0)
			return;

		if (m_gbufferWidth != width || m_gbufferHeight != height)
		{
			using AttachmentDesc = NLS::Render::Buffers::MultiFramebuffer::AttachmentDesc;
			m_gbuffer.Init(width, height, {
				AttachmentDesc{ GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE },
				AttachmentDesc{ GL_RGBA16F, GL_RGBA, GL_FLOAT },
				AttachmentDesc{ GL_RGBA16F, GL_RGBA, GL_FLOAT },
				AttachmentDesc{ GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE }
			}, true);

			const auto& textures = m_gbuffer.GetColorTextures();
			m_albedoTexture = NLS::Render::Resources::Texture2D::WrapExternal(textures[0], width, height);
			m_positionTexture = NLS::Render::Resources::Texture2D::WrapExternal(textures[1], width, height);
			m_normalTexture = NLS::Render::Resources::Texture2D::WrapExternal(textures[2], width, height);
			m_materialTexture = NLS::Render::Resources::Texture2D::WrapExternal(textures[3], width, height);
			m_depthTexture = NLS::Render::Resources::Texture2D::WrapExternal(m_gbuffer.GetDepthTexture(), width, height);

			m_gbufferWidth = width;
			m_gbufferHeight = height;
		}
	}

	void DeferredSceneRenderer::DrawSkyboxes(NLS::Render::Data::PipelineState pso)
	{
		pso.depthFunc = NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL;
		pso.culling = false;

		const auto& scene = GetDescriptor<DeferredSceneDescriptor>();
		size_t skyboxCount = 0;
		for (const auto& [_, drawable] : scene.skyboxes)
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

	void DeferredSceneRenderer::DrawTransparents(NLS::Render::Data::PipelineState pso)
	{
		const auto& scene = GetDescriptor<DeferredSceneDescriptor>();
		for (const auto& [_, drawable] : scene.transparents)
		{
			DrawEntity(pso, drawable);
		}
	}

	void DeferredSceneRenderer::DrawOpaques(NLS::Render::Data::PipelineState pso)
	{
		const auto& scene = GetDescriptor<DeferredSceneDescriptor>();
		DrawDeferredGeometryPass(scene, pso);
	}

	void DeferredSceneRenderer::DrawDeferredGeometryPass(const DeferredSceneDescriptor& scene, NLS::Render::Data::PipelineState pso)
	{
		m_gbuffer.Bind();
		m_driver.SetViewport(0, 0, m_gbufferWidth, m_gbufferHeight);
		m_driver.Clear(true, true, false, NLS::Maths::Vector4(0.0f, 0.0f, 0.0f, 0.0f));

		for (const auto& [_, drawable] : scene.opaques)
		{
			auto geometryMaterial = BuildGeometryMaterial(*drawable.material);
			auto geometryDrawable = drawable;
			geometryDrawable.material = &geometryMaterial;
			geometryDrawable.stateMask = geometryMaterial.GenerateStateMask();
			DrawEntity(pso, geometryDrawable);
		}
	}

	void DeferredSceneRenderer::DrawDeferredLightingPass(const DeferredSceneDescriptor& scene, NLS::Render::Data::PipelineState pso)
	{
		const auto& frame = GetFrameDescriptor();
		const auto destinationFramebuffer = frame.outputBuffer ? frame.outputBuffer->GetID() : 0;

		m_driver.BindFramebuffer(destinationFramebuffer);
		m_driver.SetViewport(0, 0, frame.renderWidth, frame.renderHeight);

		m_clusterRecordsBuffer->Bind(1);
		m_clusterIndicesBuffer->Bind(2);

		auto& camera = *frame.camera;
		pso.depthTest = false;
		pso.depthWriting = false;
		pso.culling = false;

		if (m_deferredLightingShader)
		{
			m_deferredLightingShader->Bind();
			m_albedoTexture->Bind(0);
			m_positionTexture->Bind(1);
			m_normalTexture->Bind(2);
			m_materialTexture->Bind(3);
			m_depthTexture->Bind(4);

			glUniform3f(
				kClusterDimensionsLocation,
				static_cast<float>(scene.clusteredLights.settings.gridSizeX),
				static_cast<float>(scene.clusteredLights.settings.gridSizeY),
				static_cast<float>(scene.clusteredLights.settings.gridSizeZ)
			);
			glUniform2f(kScreenSizeLocation, static_cast<float>(frame.renderWidth), static_cast<float>(frame.renderHeight));
			glUniform2f(kNearFarLocation, camera.GetNear(), camera.GetFar());
			glUniform1i(kClusterLightIndexCountLocation, static_cast<int>(scene.clusteredLights.lightIndices.size()));
			glUniform3f(kCameraWorldPosLocation, camera.GetPosition().x, camera.GetPosition().y, camera.GetPosition().z);

			m_driver.Draw(pso, m_fullscreenTriangle);

			m_depthTexture->Unbind();
			m_materialTexture->Unbind();
			m_normalTexture->Unbind();
			m_positionTexture->Unbind();
			m_albedoTexture->Unbind();
			m_deferredLightingShader->Unbind();
		}

		m_clusterIndicesBuffer->Unbind();
		m_clusterRecordsBuffer->Unbind();

		m_driver.BlitDepth(m_gbuffer.GetID(), destinationFramebuffer, frame.renderWidth, frame.renderHeight);
		m_driver.BindFramebuffer(destinationFramebuffer);
	}

	NLS::Render::Resources::Material DeferredSceneRenderer::BuildGeometryMaterial(const NLS::Render::Resources::Material& source) const
	{
		NLS::Render::Resources::Material geometryMaterial(NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager)[":Shaders/DeferredGBuffer.glsl"]);
		geometryMaterial.SetBlendable(false);
		geometryMaterial.SetBackfaceCulling(source.HasBackfaceCulling());
		geometryMaterial.SetFrontfaceCulling(source.HasFrontfaceCulling());
		geometryMaterial.SetDepthTest(source.HasDepthTest());
		geometryMaterial.SetDepthWriting(source.HasDepthWriting());

		geometryMaterial.Set<NLS::Render::Resources::Texture2D*>("u_DiffuseMap", nullptr);
		geometryMaterial.Set<NLS::Render::Resources::Texture2D*>("u_SpecularMap", nullptr);
		geometryMaterial.Set<NLS::Render::Resources::Texture2D*>("u_NormalMap", nullptr);
		geometryMaterial.Set<NLS::Render::Resources::Texture2D*>("u_MaskMap", nullptr);
		geometryMaterial.Set("u_Specular", NLS::Maths::Vector3(1.0f, 1.0f, 1.0f));
		geometryMaterial.Set("u_Shininess", 64.0f);
		geometryMaterial.Set("u_EnableNormalMapping", false);

		auto& targetUniforms = geometryMaterial.GetUniformsData();
		for (const auto& [name, value] : const_cast<NLS::Render::Resources::Material&>(source).GetUniformsData())
		{
			if (targetUniforms.find(name) != targetUniforms.end())
				targetUniforms[name] = value;
		}

		return geometryMaterial;
	}
}

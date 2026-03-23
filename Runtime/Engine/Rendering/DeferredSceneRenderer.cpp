#include "Rendering/DeferredSceneRenderer.h"

#include <fg/Blackboard.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include <Core/ResourceManagement/ShaderManager.h>
#include <Core/ServiceLocator.h>
#include <Rendering/FrameGraph/FrameGraphExecutionContext.h>
#include <Rendering/FrameGraph/FrameGraphBuffer.h>
#include <Rendering/FrameGraph/FrameGraphTexture.h>
#include <Rendering/RHI/RHITypes.h>

#include "Components/MaterialRenderer.h"
#include "Components/MeshRenderer.h"
#include "Rendering/EngineDrawableDescriptor.h"
#include "Rendering/FrameGraphSceneTargets.h"

namespace RHI = NLS::Render::RHI;

namespace
{
    using Driver = NLS::Render::Context::Driver;
    using RenderMesh = NLS::Render::Resources::Mesh;
    using Vertex = NLS::Render::Geometry::Vertex;
	using FrameGraphTexture = NLS::Render::FrameGraph::FrameGraphTexture;
	using FrameGraphBuffer = NLS::Render::FrameGraph::FrameGraphBuffer;
	using DeferredOutputData = NLS::Engine::Rendering::SceneRenderTargetsData;

	struct DeferredGBufferData
	{
		FrameGraphResource albedo = -1;
		FrameGraphResource position = -1;
		FrameGraphResource normal = -1;
		FrameGraphResource material = -1;
		FrameGraphResource depth = -1;
	};

	struct DeferredLightingData
	{
		FrameGraphResource albedo = -1;
		FrameGraphResource position = -1;
		FrameGraphResource normal = -1;
		FrameGraphResource material = -1;
		FrameGraphResource depth = -1;
		FrameGraphResource clusterRecords = -1;
		FrameGraphResource clusterIndices = -1;
		FrameGraphResource target = -1;
	};

	FrameGraphTexture::Desc MakeColorTextureDesc(uint16_t width, uint16_t height, RHI::TextureFormat format)
	{
		FrameGraphTexture::Desc desc;
		desc.width = width;
		desc.height = height;
		desc.format = format;
		desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::ColorAttachment;
		return desc;
	}

	FrameGraphTexture::Desc MakeDepthTextureDesc(uint16_t width, uint16_t height)
	{
		FrameGraphTexture::Desc desc;
		desc.width = width;
		desc.height = height;
		desc.format = RHI::TextureFormat::Depth24Stencil8;
		desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::DepthStencilAttachment;
		return desc;
	}

	void AttachDeferredGBuffer(Driver& driver, uint32_t framebuffer, uint32_t albedo, uint32_t position, uint32_t normal, uint32_t material, uint32_t depth)
	{
		driver.AttachFramebufferColorTexture(framebuffer, albedo, 0);
		driver.AttachFramebufferColorTexture(framebuffer, position, 1);
		driver.AttachFramebufferColorTexture(framebuffer, normal, 2);
		driver.AttachFramebufferColorTexture(framebuffer, material, 3);
		driver.AttachFramebufferDepthStencilTexture(framebuffer, depth);
		driver.SetFramebufferDrawBufferCount(framebuffer, 4);
	}

	float RoughnessFromLegacyShininess(float shininess)
	{
		const float clampedShininess = std::max(shininess, 1.0f);
		return std::clamp(std::sqrt(2.0f / (clampedShininess + 2.0f)), 0.045f, 1.0f);
	}

	RenderMesh BuildFullscreenTriangle()
	{
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

		return RenderMesh(vertices, {}, 0);
	}
}

namespace NLS::Engine::Rendering
{
    using DeferredLightingDescriptor = Render::Features::LightingRenderFeature::LightingDescriptor;
    using RenderMaterial = Render::Resources::Material;
    using RenderTexture2D = Render::Resources::Texture2D;
    using RenderMesh = Render::Resources::Mesh;

	DeferredSceneRenderer::DeferredSceneRenderer(Render::Context::Driver& p_driver)
		: BaseSceneRenderer(p_driver)
		, m_fullscreenTriangle(BuildFullscreenTriangle())
	{
		m_deferredLightingShader = NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager)[":Shaders/DeferredLighting.glsl"];

		m_clusterRecordsBuffer = std::make_unique<Render::Buffers::ShaderStorageBuffer>(Render::Settings::EAccessSpecifier::STREAM_DRAW);
		m_clusterIndicesBuffer = std::make_unique<Render::Buffers::ShaderStorageBuffer>(Render::Settings::EAccessSpecifier::STREAM_DRAW);
	}

	DeferredSceneRenderer::~DeferredSceneRenderer()
	{
		if (m_gbufferFramebuffer != 0)
		{
			m_driver.DestroyFramebuffer(m_gbufferFramebuffer);
			m_gbufferFramebuffer = 0;
		}
	}

	void DeferredSceneRenderer::BeginFrame(const Render::Data::FrameDescriptor& p_frameDescriptor)
	{
		BaseSceneRenderer::BeginFrame(p_frameDescriptor);

		EnsureGBufferFramebuffer();

		auto drawables = ParseScene();
		const auto& lighting = GetDescriptor<DeferredLightingDescriptor>();
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
		const auto& frame = GetFrameDescriptor();

		FrameGraph frameGraph;
		frameGraph.reserve(4, frame.outputBuffer ? 9 : 7);
		FrameGraphBlackboard blackboard;

		ImportSceneRenderTargets(frameGraph, blackboard, frame, "SceneColor", "SceneDepth");

		const auto& gbufferPass = frameGraph.addCallbackPass<DeferredGBufferData>(
			"DeferredOpaque",
			[&frame](FrameGraph::Builder& builder, DeferredGBufferData& data)
			{
				data.albedo = builder.create<FrameGraphTexture>("GBufferAlbedo", MakeColorTextureDesc(frame.renderWidth, frame.renderHeight, RHI::TextureFormat::RGBA8));
				data.position = builder.create<FrameGraphTexture>("GBufferPosition", MakeColorTextureDesc(frame.renderWidth, frame.renderHeight, RHI::TextureFormat::RGBA16F));
				data.normal = builder.create<FrameGraphTexture>("GBufferNormal", MakeColorTextureDesc(frame.renderWidth, frame.renderHeight, RHI::TextureFormat::RGBA16F));
				data.material = builder.create<FrameGraphTexture>("GBufferMaterial", MakeColorTextureDesc(frame.renderWidth, frame.renderHeight, RHI::TextureFormat::RGBA8));
				data.depth = builder.create<FrameGraphTexture>("GBufferDepth", MakeDepthTextureDesc(frame.renderWidth, frame.renderHeight));

				data.albedo = builder.write(data.albedo);
				data.position = builder.write(data.position);
				data.normal = builder.write(data.normal);
				data.material = builder.write(data.material);
				data.depth = builder.write(data.depth);
			},
			[this, &scene, pso](const DeferredGBufferData& data, FrameGraphPassResources& resources, void*)
			{
				AttachDeferredGBuffer(
					m_driver,
					m_gbufferFramebuffer,
					resources.get<FrameGraphTexture>(data.albedo).id,
					resources.get<FrameGraphTexture>(data.position).id,
					resources.get<FrameGraphTexture>(data.normal).id,
					resources.get<FrameGraphTexture>(data.material).id,
					resources.get<FrameGraphTexture>(data.depth).id
				);
				DrawDeferredGeometryPass(scene, pso);
			}
		);

		const auto clusterRecords = frameGraph.import<FrameGraphBuffer>(
			"ClusterRecords",
			[]()
			{
				FrameGraphBuffer::Desc desc;
				desc.type = RHI::BufferType::ShaderStorage;
				desc.usage = RHI::BufferUsage::StreamDraw;
				return desc;
			}(),
			FrameGraphBuffer::WrapExternal(m_clusterRecordsBuffer->GetID())
		);
		const auto clusterIndices = frameGraph.import<FrameGraphBuffer>(
			"ClusterIndices",
			[]()
			{
				FrameGraphBuffer::Desc desc;
				desc.type = RHI::BufferType::ShaderStorage;
				desc.usage = RHI::BufferUsage::StreamDraw;
				return desc;
			}(),
			FrameGraphBuffer::WrapExternal(m_clusterIndicesBuffer->GetID())
		);

		const auto& lightingPass = frameGraph.addCallbackPass<DeferredLightingData>(
			"DeferredLighting",
			[&blackboard, &gbufferPass, clusterRecords, clusterIndices](FrameGraph::Builder& builder, DeferredLightingData& data)
			{
				data.albedo = builder.read(gbufferPass.albedo, 0);
				data.position = builder.read(gbufferPass.position, 1);
				data.normal = builder.read(gbufferPass.normal, 2);
				data.material = builder.read(gbufferPass.material, 3);
				data.depth = builder.read(gbufferPass.depth, 4);
				data.clusterRecords = builder.read(clusterRecords, 1);
				data.clusterIndices = builder.read(clusterIndices, 2);

				if (const auto* output = blackboard.try_get<DeferredOutputData>())
				{
					if (output->color >= 0)
						data.target = builder.write(output->color);
					if (output->depth >= 0)
						data.depth = builder.write(output->depth);
				}
				else
					builder.setSideEffect();
			},
			[this, &scene, pso](const DeferredLightingData&, FrameGraphPassResources&, void*)
			{
				DrawDeferredLightingPass(scene, pso, m_gbufferFramebuffer);
			}
		);

		auto currentTarget = lightingPass.target;

		const auto& skyboxPass = frameGraph.addCallbackPass<DeferredOutputData>(
			"DeferredSkybox",
			[&blackboard, currentTarget](FrameGraph::Builder& builder, DeferredOutputData& data)
			{
				if (currentTarget >= 0)
					data.color = builder.write(currentTarget);
				else if (const auto* output = blackboard.try_get<DeferredOutputData>())
					data.color = builder.write(output->color);

				if (const auto* output = blackboard.try_get<DeferredOutputData>(); output && output->depth >= 0)
					data.depth = builder.write(output->depth);

				if (data.color < 0 && data.depth < 0)
					builder.setSideEffect();
			},
			[this, pso](const DeferredOutputData&, FrameGraphPassResources&, void*)
			{
				DrawSkyboxes(pso);
			}
		);

		currentTarget = skyboxPass.color;

		frameGraph.addCallbackPass<DeferredOutputData>(
			"DeferredTransparent",
			[&blackboard, currentTarget](FrameGraph::Builder& builder, DeferredOutputData& data)
			{
				if (currentTarget >= 0)
					data.color = builder.write(currentTarget);
				else if (const auto* output = blackboard.try_get<DeferredOutputData>())
					data.color = builder.write(output->color);

				if (const auto* output = blackboard.try_get<DeferredOutputData>(); output && output->depth >= 0)
					data.depth = builder.write(output->depth);

				if (data.color < 0 && data.depth < 0)
					builder.setSideEffect();
			},
			[this, pso](const DeferredOutputData&, FrameGraphPassResources&, void*)
			{
				DrawTransparents(pso);
			}
		);

		frameGraph.compile();
		Render::FrameGraph::FrameGraphExecutionContext executionContext{ m_driver.GetRenderDevice() };
		frameGraph.execute(&executionContext, &executionContext);

		DrawRegisteredPasses(CreatePipelineState());
	}

	void DeferredSceneRenderer::EnsureGBufferFramebuffer()
	{
		if (m_gbufferFramebuffer == 0)
			m_gbufferFramebuffer = m_driver.CreateFramebuffer();
	}

	void DeferredSceneRenderer::DrawSkyboxes(Render::Data::PipelineState pso)
	{
		pso.depthFunc = Render::Settings::EComparaisonAlgorithm::LESS_EQUAL;
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

	void DeferredSceneRenderer::DrawTransparents(Render::Data::PipelineState pso)
	{
		const auto& scene = GetDescriptor<DeferredSceneDescriptor>();
		for (const auto& [_, drawable] : scene.transparents)
		{
			DrawEntity(pso, drawable);
		}
	}

	void DeferredSceneRenderer::DrawOpaques(Render::Data::PipelineState pso)
	{
		const auto& scene = GetDescriptor<DeferredSceneDescriptor>();
		DrawDeferredGeometryPass(scene, pso);
	}

	void DeferredSceneRenderer::DrawDeferredGeometryPass(const DeferredSceneDescriptor& scene, Render::Data::PipelineState pso)
	{
		const auto& frame = GetFrameDescriptor();
		m_driver.BindFramebuffer(m_gbufferFramebuffer);
		m_driver.SetViewport(0, 0, frame.renderWidth, frame.renderHeight);
		m_driver.Clear(true, true, false, Maths::Vector4(0.0f, 0.0f, 0.0f, 0.0f));

		for (const auto& [_, drawable] : scene.opaques)
		{
			auto geometryMaterial = BuildGeometryMaterial(*drawable.material);
			auto geometryDrawable = drawable;
			geometryDrawable.material = &geometryMaterial;
			geometryDrawable.stateMask = geometryMaterial.GenerateStateMask();
			DrawEntity(pso, geometryDrawable);
		}
	}

	void DeferredSceneRenderer::DrawDeferredLightingPass(const DeferredSceneDescriptor& scene, Render::Data::PipelineState pso, uint32_t sourceFramebuffer)
	{
		const auto& frame = GetFrameDescriptor();
		const auto destinationFramebuffer = frame.outputBuffer ? frame.outputBuffer->GetID() : 0;

		m_driver.BindFramebuffer(destinationFramebuffer);
		m_driver.SetViewport(0, 0, frame.renderWidth, frame.renderHeight);

		auto& camera = *frame.camera;
		pso.depthTest = false;
		pso.depthWriting = false;
		pso.culling = false;

		if (m_deferredLightingShader)
		{
			m_deferredLightingShader->Bind();

			m_deferredLightingShader->SetUniformVec3(
				"u_ClusterDimensions",
				{
					static_cast<float>(scene.clusteredLights.settings.gridSizeX),
					static_cast<float>(scene.clusteredLights.settings.gridSizeY),
					static_cast<float>(scene.clusteredLights.settings.gridSizeZ)
				}
			);
			m_deferredLightingShader->SetUniformVec2("u_ScreenSize", { static_cast<float>(frame.renderWidth), static_cast<float>(frame.renderHeight) });
			m_deferredLightingShader->SetUniformVec2("u_NearFar", { camera.GetNear(), camera.GetFar() });
			m_deferredLightingShader->SetUniformInt("u_ClusterLightIndexCount", static_cast<int>(scene.clusteredLights.lightIndices.size()));
			m_deferredLightingShader->SetUniformVec3("u_CameraWorldPos", camera.GetPosition());

			m_driver.Draw(pso, m_fullscreenTriangle);
			m_deferredLightingShader->Unbind();
		}

		m_driver.BlitDepth(sourceFramebuffer, destinationFramebuffer, frame.renderWidth, frame.renderHeight);
		m_driver.BindFramebuffer(destinationFramebuffer);
	}

	RenderMaterial DeferredSceneRenderer::BuildGeometryMaterial(const RenderMaterial& source) const
	{
		RenderMaterial geometryMaterial(NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager)[":Shaders/DeferredGBuffer.glsl"]);
		geometryMaterial.SetBlendable(false);
		geometryMaterial.SetBackfaceCulling(source.HasBackfaceCulling());
		geometryMaterial.SetFrontfaceCulling(source.HasFrontfaceCulling());
		geometryMaterial.SetDepthTest(source.HasDepthTest());
		geometryMaterial.SetDepthWriting(source.HasDepthWriting());

		geometryMaterial.Set<RenderTexture2D*>("u_AlbedoMap", nullptr);
		geometryMaterial.Set<RenderTexture2D*>("u_MetallicMap", nullptr);
		geometryMaterial.Set<RenderTexture2D*>("u_RoughnessMap", nullptr);
		geometryMaterial.Set<RenderTexture2D*>("u_AmbientOcclusionMap", nullptr);
		geometryMaterial.Set<RenderTexture2D*>("u_NormalMap", nullptr);
		geometryMaterial.Set<RenderTexture2D*>("u_MaskMap", nullptr);
		geometryMaterial.Set("u_Albedo", Maths::Vector4(1.0f, 1.0f, 1.0f, 1.0f));
		geometryMaterial.Set("u_Metallic", 1.0f);
		geometryMaterial.Set("u_Roughness", 1.0f);
		geometryMaterial.Set("u_EnableNormalMapping", false);

		auto& targetUniforms = geometryMaterial.GetUniformsData();
		const auto& sourceUniforms = const_cast<RenderMaterial&>(source).GetUniformsData();
		for (const auto& [name, value] : sourceUniforms)
		{
			if (targetUniforms.find(name) != targetUniforms.end())
				targetUniforms[name] = value;
		}

		if (targetUniforms.contains("u_AlbedoMap") && sourceUniforms.contains("u_DiffuseMap") && !sourceUniforms.contains("u_AlbedoMap"))
			targetUniforms["u_AlbedoMap"] = sourceUniforms.at("u_DiffuseMap");

		if (targetUniforms.contains("u_Albedo") && sourceUniforms.contains("u_Diffuse") && !sourceUniforms.contains("u_Albedo"))
			targetUniforms["u_Albedo"] = sourceUniforms.at("u_Diffuse");

		if (targetUniforms.contains("u_EnableNormalMapping") && sourceUniforms.contains("u_EnableNormalMapping"))
			targetUniforms["u_EnableNormalMapping"] = sourceUniforms.at("u_EnableNormalMapping");

		if (targetUniforms.contains("u_NormalMap") && sourceUniforms.contains("u_NormalMap"))
			targetUniforms["u_NormalMap"] = sourceUniforms.at("u_NormalMap");

		if (!sourceUniforms.contains("u_Metallic"))
			targetUniforms["u_Metallic"] = 0.0f;

		if (!sourceUniforms.contains("u_Roughness") && sourceUniforms.contains("u_Shininess"))
		{
			if (const auto* legacyShininess = std::any_cast<float>(&sourceUniforms.at("u_Shininess")))
				targetUniforms["u_Roughness"] = RoughnessFromLegacyShininess(*legacyShininess);
		}

		return geometryMaterial;
	}
}

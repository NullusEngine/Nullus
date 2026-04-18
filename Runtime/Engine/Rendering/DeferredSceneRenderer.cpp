#include "Rendering/DeferredSceneRenderer.h"

#include <fg/Blackboard.hpp>

#include <Debug/Logger.h>
#include <Math/Matrix4.h>
#include <Math/Vector3.h>
#include <Rendering/Buffers/UniformBuffer.h>
#include <Rendering/Data/LightingDescriptor.h>
#include <Rendering/FrameGraph/FrameGraphExecutionContext.h>
#include <Rendering/FrameGraph/FrameGraphTexture.h>
#include <Rendering/Geometry/Vertex.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/Resources/Loaders/ShaderLoader.h>
#include <Rendering/Resources/Material.h>
#include <Rendering/Resources/Mesh.h>
#include <Rendering/Resources/Texture2D.h>
#include <Rendering/Resources/TextureCube.h>
#include <Rendering/Settings/EPrimitiveMode.h>

#include "Rendering/FrameGraphSceneTargets.h"
#include "Rendering/ScenePipelineStatePresets.h"

namespace
{
	struct DeferredGBufferPassData
	{
		FrameGraphResource albedo = -1;
		FrameGraphResource normal = -1;
		FrameGraphResource material = -1;
		FrameGraphResource depth = -1;
	};

	struct DeferredLightingPassData
	{
		FrameGraphResource albedo = -1;
		FrameGraphResource normal = -1;
		FrameGraphResource material = -1;
		FrameGraphResource depth = -1;
		FrameGraphResource outputColor = -1;
		FrameGraphResource outputDepth = -1;
	};

	struct DeferredPassConstants
	{
		NLS::Maths::Matrix4 inverseViewProjection;
		NLS::Maths::Vector3 cameraWorldPosition;
		float ambientIntensity = 0.2f;
		NLS::Maths::Vector3 lightDirection{ -0.4f, -1.0f, -0.25f };
		float lightIntensity = 1.0f;
		NLS::Maths::Vector3 lightColor{ 1.0f, 0.98f, 0.92f };
		float hasSkyboxTexture = 0.0f;
		NLS::Maths::Vector3 skyFallbackColor{ 0.55f, 0.70f, 0.92f };
		float depthFogFactor = 0.15f;
	};

	using LightingDescriptor = NLS::Render::Data::LightingDescriptor;

	DeferredPassConstants BuildDeferredPassConstants(
		const NLS::Render::Data::FrameDescriptor& frameDescriptor,
		const LightingDescriptor* lightingDescriptor,
		bool hasSkyboxTexture)
	{
		DeferredPassConstants constants{};
		const auto viewProjection = frameDescriptor.camera->GetProjectionMatrix() * frameDescriptor.camera->GetViewMatrix();
		constants.inverseViewProjection = NLS::Maths::Matrix4::Transpose(NLS::Maths::Matrix4::Inverse(viewProjection));
		constants.cameraWorldPosition = frameDescriptor.camera->GetPosition();
		constants.hasSkyboxTexture = hasSkyboxTexture ? 1.0f : 0.0f;

		if (lightingDescriptor == nullptr)
			return constants;

		for (const auto& lightRef : lightingDescriptor->lights)
		{
			const auto& light = lightRef.get();
			if (light.type != NLS::Render::Settings::ELightType::DIRECTIONAL &&
				light.type != NLS::Render::Settings::ELightType::SPOT)
			{
				continue;
			}

			constants.lightDirection = light.transform->GetWorldForward();
			constants.lightColor = light.color;
			constants.lightIntensity = light.intensity;
			return constants;
		}

		if (!lightingDescriptor->lights.empty())
		{
			const auto& light = lightingDescriptor->lights.front().get();
			constants.lightDirection = light.transform->GetWorldForward();
			constants.lightColor = light.color;
			constants.lightIntensity = light.intensity;
		}

		return constants;
	}

	NLS::Render::FrameGraph::FrameGraphTexture::Desc MakeGBufferColorDesc(uint16_t width, uint16_t height)
	{
		NLS::Render::FrameGraph::FrameGraphTexture::Desc desc;
		desc.extent.width = width;
		desc.extent.height = height;
		desc.extent.depth = 1u;
		desc.format = NLS::Render::RHI::TextureFormat::RGBA8;
		desc.usage = NLS::Render::RHI::TextureUsageFlags::ColorAttachment | NLS::Render::RHI::TextureUsageFlags::Sampled;
		return desc;
	}

	NLS::Render::FrameGraph::FrameGraphTexture::Desc MakeGBufferDepthDesc(uint16_t width, uint16_t height)
	{
		NLS::Render::FrameGraph::FrameGraphTexture::Desc desc;
		desc.extent.width = width;
		desc.extent.height = height;
		desc.extent.depth = 1u;
		desc.format = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
		desc.usage = NLS::Render::RHI::TextureUsageFlags::DepthStencilAttachment | NLS::Render::RHI::TextureUsageFlags::Sampled;
		return desc;
	}

	NLS::Render::Geometry::Vertex MakeFullscreenVertex(float x, float y, float u, float v)
	{
		NLS::Render::Geometry::Vertex vertex{};
		vertex.position[0] = x;
		vertex.position[1] = y;
		vertex.position[2] = 0.0f;
		vertex.texCoords[0] = u;
		vertex.texCoords[1] = v;
		vertex.normals[2] = 1.0f;
		vertex.tangent[0] = 1.0f;
		vertex.bitangent[1] = 1.0f;
		return vertex;
	}

	void SubmitMeshDraw(
		const std::shared_ptr<NLS::Render::RHI::RHICommandBuffer>& commandBuffer,
		const std::shared_ptr<NLS::Render::RHI::RHIMesh>& rhiMesh,
		const uint32_t instanceCount)
	{
		if (commandBuffer == nullptr || rhiMesh == nullptr || rhiMesh->GetVertexBuffer() == nullptr)
			return;

		commandBuffer->BindVertexBuffer(0, { rhiMesh->GetVertexBuffer(), 0, rhiMesh->GetVertexStride() });

		const auto indexBuffer = rhiMesh->GetIndexBuffer();
		const auto indexCount = rhiMesh->GetIndexCount();
		if (indexBuffer != nullptr && indexCount > 0u)
		{
			commandBuffer->BindIndexBuffer({ indexBuffer, 0, rhiMesh->GetIndexType() });
			commandBuffer->DrawIndexed(indexCount, instanceCount, 0, 0, 0);
			return;
		}

		const auto vertexCount = rhiMesh->GetVertexCount();
		if (vertexCount > 0u)
			commandBuffer->Draw(vertexCount, instanceCount, 0, 0);
	}

}

namespace NLS::Engine::Rendering
{
	DeferredSceneRenderer::DeferredSceneRenderer(NLS::Render::Context::Driver& p_driver)
		: BaseSceneRenderer(p_driver)
	{
		LoadPipelineResources();
	}

	DeferredSceneRenderer::~DeferredSceneRenderer()
	{
		m_gBufferMaterialCache.clear();
		m_lightingMaterial.reset();
		m_fullscreenQuad.reset();
		NLS::Render::Resources::Loaders::ShaderLoader::Destroy(m_lightingShader);
		NLS::Render::Resources::Loaders::ShaderLoader::Destroy(m_gBufferShader);
	}

	void DeferredSceneRenderer::BeginFrame(const NLS::Render::Data::FrameDescriptor& p_frameDescriptor)
	{
		NLS_ASSERT(HasFrameObjectBindingProvider(), "DeferredSceneRenderer requires a renderer-owned frame/object binding provider.");
		BaseSceneRenderer::BeginFrame(p_frameDescriptor);
		AddDescriptor<DeferredSceneDescriptor>({
			ParseScene()
		});
	}

	void DeferredSceneRenderer::DrawFrame()
	{
		const auto& frame = GetFrameDescriptor();
		EnsureGBufferTargets(frame.renderWidth, frame.renderHeight);

		if (!m_gBufferShader || !m_lightingMaterial || !m_fullscreenQuad)
		{
			NLS_LOG_WARNING("DeferredSceneRenderer is missing shader or mesh resources; skipping deferred frame.");
			return;
		}

		FrameGraph frameGraph;
		frameGraph.reserve(2, frame.outputBuffer ? 6 : 4);
		FrameGraphBlackboard blackboard;

		ImportSceneRenderTargets(frameGraph, blackboard, frame, "DeferredOutputColor", "DeferredOutputDepth");

		const auto gBufferAlbedo = frameGraph.import<NLS::Render::FrameGraph::FrameGraphTexture>(
			"DeferredGBufferAlbedo",
			MakeGBufferColorDesc(frame.renderWidth, frame.renderHeight),
			NLS::Render::FrameGraph::FrameGraphTexture::WrapExternal(
				m_gBuffer.GetExplicitColorTextureHandles()[0],
				m_gBuffer.GetOrCreateExplicitColorView(0, "DeferredGBufferAlbedoView"))
		);
		const auto gBufferNormal = frameGraph.import<NLS::Render::FrameGraph::FrameGraphTexture>(
			"DeferredGBufferNormal",
			MakeGBufferColorDesc(frame.renderWidth, frame.renderHeight),
			NLS::Render::FrameGraph::FrameGraphTexture::WrapExternal(
				m_gBuffer.GetExplicitColorTextureHandles()[1],
				m_gBuffer.GetOrCreateExplicitColorView(1, "DeferredGBufferNormalView"))
		);
		const auto gBufferMaterial = frameGraph.import<NLS::Render::FrameGraph::FrameGraphTexture>(
			"DeferredGBufferMaterial",
			MakeGBufferColorDesc(frame.renderWidth, frame.renderHeight),
			NLS::Render::FrameGraph::FrameGraphTexture::WrapExternal(
				m_gBuffer.GetExplicitColorTextureHandles()[2],
				m_gBuffer.GetOrCreateExplicitColorView(2, "DeferredGBufferMaterialView"))
		);
		const auto gBufferDepth = frameGraph.import<NLS::Render::FrameGraph::FrameGraphTexture>(
			"DeferredGBufferDepth",
			MakeGBufferDepthDesc(frame.renderWidth, frame.renderHeight),
			NLS::Render::FrameGraph::FrameGraphTexture::WrapExternal(
				m_gBuffer.GetExplicitDepthTextureHandle(),
				m_gBuffer.GetOrCreateExplicitDepthView("DeferredGBufferDepthView"))
		);

		const auto& gBufferPass = frameGraph.addCallbackPass<DeferredGBufferPassData>(
			"DeferredGBuffer",
			[&](FrameGraph::Builder& builder, DeferredGBufferPassData& data)
			{
				data.albedo = builder.write(gBufferAlbedo);
				data.normal = builder.write(gBufferNormal);
				data.material = builder.write(gBufferMaterial);
				data.depth = builder.write(gBufferDepth);
			},
			[this](const DeferredGBufferPassData&, FrameGraphPassResources&, void*)
			{
				const bool recordedPass = BeginRecordedRenderPass(
					&m_gBuffer,
					GetFrameDescriptor().renderWidth,
					GetFrameDescriptor().renderHeight,
					true,
					true,
					true,
					Maths::Vector4{ 0.0f, 0.0f, 0.0f, 1.0f });
				if (!recordedPass)
				{
					ExecuteLegacyFramebufferPass(m_gBuffer, [&]()
					{
						Clear(true, true, true, Maths::Vector4{ 0.0f, 0.0f, 0.0f, 1.0f });
						DrawGBufferOpaques(CreateSceneDefaultPipelineState(*this));
					});
				}
				else
				{
					DrawGBufferOpaques(CreateSceneDefaultPipelineState(*this));
					EndRecordedRenderPass();
				}
			}
		);

		frameGraph.addCallbackPass<DeferredLightingPassData>(
			"DeferredLighting",
			[&](FrameGraph::Builder& builder, DeferredLightingPassData& data)
			{
				data.albedo = builder.read(gBufferPass.albedo);
				data.normal = builder.read(gBufferPass.normal);
				data.material = builder.read(gBufferPass.material);
				data.depth = builder.read(gBufferPass.depth);

				if (const auto* output = blackboard.try_get<SceneRenderTargetsData>())
				{
					if (output->color >= 0)
						data.outputColor = builder.write(output->color);
					if (output->depth >= 0)
						data.outputDepth = builder.write(output->depth);
				}
				else
					builder.setSideEffect();
			},
			[this](const DeferredLightingPassData&, FrameGraphPassResources&, void*)
			{
				const auto& frameDescriptor = GetFrameDescriptor();
				const auto clearColor = Maths::Vector4{
					frameDescriptor.camera->GetClearColor().x,
					frameDescriptor.camera->GetClearColor().y,
					frameDescriptor.camera->GetClearColor().z,
					1.0f
				};
				const bool recordedPass = BeginOutputRenderPass(
					frameDescriptor.renderWidth,
					frameDescriptor.renderHeight,
					frameDescriptor.camera->GetClearColorBuffer(),
					frameDescriptor.camera->GetClearDepthBuffer(),
					frameDescriptor.camera->GetClearStencilBuffer(),
					clearColor,
					true);
				DrawLightingPass(CreateSceneFullscreenCompositePipelineState(*this));
				EndOutputRenderPass(recordedPass, true);
			}
		);

		frameGraph.compile();
		auto executionContext = CreateFrameGraphExecutionContext();
		frameGraph.execute(&executionContext, &executionContext);

		DrawRegisteredPasses();
	}

	void DeferredSceneRenderer::LoadPipelineResources()
	{
		using ShaderLoader = NLS::Render::Resources::Loaders::ShaderLoader;

		m_gBufferShader = ShaderLoader::Create("App/Assets/Engine/Shaders/DeferredGBuffer.hlsl");
		m_lightingShader = ShaderLoader::Create("App/Assets/Engine/Shaders/DeferredLighting.hlsl");

		if (m_lightingShader)
		{
			m_lightingMaterial = std::make_unique<NLS::Render::Resources::Material>(m_lightingShader);
		}

		m_passBuffer = std::make_unique<NLS::Render::Buffers::UniformBuffer>(
			sizeof(DeferredPassConstants),
			NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(NLS::Render::RHI::BindingPointMap::kPassBindingSpace, 0),
			0,
			NLS::Render::Settings::EAccessSpecifier::STREAM_DRAW);

		std::vector<NLS::Render::Geometry::Vertex> vertices{
			MakeFullscreenVertex(-1.0f, -1.0f, 0.0f, 1.0f),
			MakeFullscreenVertex(-1.0f,  1.0f, 0.0f, 0.0f),
			MakeFullscreenVertex( 1.0f,  1.0f, 1.0f, 0.0f),
			MakeFullscreenVertex( 1.0f, -1.0f, 1.0f, 1.0f)
		};
		std::vector<uint32_t> indices{ 0, 1, 2, 0, 2, 3 };
		m_fullscreenQuad = std::make_unique<NLS::Render::Resources::Mesh>(vertices, indices, 0);
	}

	void DeferredSceneRenderer::EnsureGBufferTargets(uint16_t width, uint16_t height)
	{
		static const std::vector<NLS::Render::Buffers::MultiFramebuffer::AttachmentDesc> kAttachments{
			{ NLS::Render::RHI::TextureFormat::RGBA8 },
			{ NLS::Render::RHI::TextureFormat::RGBA8 },
			{ NLS::Render::RHI::TextureFormat::RGBA8 }
		};

		if (m_gBuffer.GetID() == 0)
			m_gBuffer.Init(width, height, kAttachments, true);
		else
			m_gBuffer.Resize(width, height);

		const auto& colorResources = m_gBuffer.GetExplicitColorTextureHandles();
		if (colorResources.size() < 3)
			return;

		auto wrapTexture = [width, height](const std::shared_ptr<NLS::Render::RHI::RHITexture>& textureResource)
		{
			return NLS::Render::Resources::Texture2D::WrapExternal(textureResource, width, height);
		};

		m_gBufferAlbedoTexture = wrapTexture(colorResources[0]);
		m_gBufferNormalTexture = wrapTexture(colorResources[1]);
		m_gBufferMaterialTexture = wrapTexture(colorResources[2]);
		if (m_gBuffer.GetExplicitDepthTextureHandle())
			m_gBufferDepthTexture = wrapTexture(m_gBuffer.GetExplicitDepthTextureHandle());
	}

	std::unique_ptr<NLS::Render::Resources::Material> DeferredSceneRenderer::CreateGBufferMaterial() const
	{
		auto material = std::make_unique<NLS::Render::Resources::Material>(m_gBufferShader);
		material->SetBlendable(false);
		material->SetColorWriting(true);
		return material;
	}

	NLS::Render::Resources::Material& DeferredSceneRenderer::GetOrCreateGBufferMaterial(NLS::Render::Resources::Material& sourceMaterial)
	{
		auto found = m_gBufferMaterialCache.find(&sourceMaterial);
		if (found == m_gBufferMaterialCache.end())
			found = m_gBufferMaterialCache.emplace(&sourceMaterial, CreateGBufferMaterial()).first;

		SyncGBufferMaterial(*found->second, sourceMaterial);
		return *found->second;
	}

	void DeferredSceneRenderer::SyncGBufferMaterial(NLS::Render::Resources::Material& target, const NLS::Render::Resources::Material& sourceMaterial) const
	{
		target.SetGPUInstances(sourceMaterial.GetGPUInstances());

		target.FillUniform();
		for (const auto& [name, value] : sourceMaterial.GetParameterBlock().Data())
		{
			if (target.GetParameterBlock().Contains(name))
				target.GetParameterBlock().Set(name, value);
		}
	}

	void DeferredSceneRenderer::DrawGBufferOpaques(NLS::Render::Data::PipelineState pso)
	{
		if (!m_gBufferShader)
			return;

		const auto& scene = GetDescriptor<DeferredSceneDescriptor>();

		for (const auto& [_, drawable] : scene.drawables.opaques)
		{
			if (!drawable.material)
				continue;

			auto gbufferDrawable = drawable;
			gbufferDrawable.material = &GetOrCreateGBufferMaterial(*drawable.material);

			NLS::Render::Resources::MaterialPipelineStateOverrides gBufferOverrides;
			gBufferOverrides.depthTest = drawable.material->HasDepthTest();
			gBufferOverrides.depthWrite = drawable.material->HasDepthWriting();
			gBufferOverrides.colorWrite = true;
			gBufferOverrides.culling = drawable.material->HasBackfaceCulling() || drawable.material->HasFrontfaceCulling();
			gBufferOverrides.cullFace = drawable.material->HasBackfaceCulling() && drawable.material->HasFrontfaceCulling()
				? NLS::Render::Settings::ECullFace::FRONT_AND_BACK
				: drawable.material->HasFrontfaceCulling()
					? NLS::Render::Settings::ECullFace::FRONT
					: NLS::Render::Settings::ECullFace::BACK;

			DrawEntity(gbufferDrawable, gBufferOverrides, pso.depthFunc);
		}
	}

	void DeferredSceneRenderer::DrawLightingPass(NLS::Render::Data::PipelineState pso)
	{
		if (!m_lightingMaterial || !m_fullscreenQuad || !m_gBufferAlbedoTexture || !m_gBufferNormalTexture || !m_gBufferMaterialTexture || !m_passBuffer)
			return;

		NLS::Render::Resources::TextureCube* skyboxTexture = nullptr;
		const auto& scene = GetDescriptor<DeferredSceneDescriptor>();
		for (const auto& [_, drawable] : scene.drawables.skyboxes)
		{
			if (drawable.material == nullptr)
				continue;

			const auto* skyboxParameter = drawable.material->GetParameterBlock().TryGet("cubeTex");
			if (skyboxParameter != nullptr && skyboxParameter->type() == typeid(NLS::Render::Resources::TextureCube*))
			{
				skyboxTexture = std::any_cast<NLS::Render::Resources::TextureCube*>(*skyboxParameter);
				break;
			}
		}

		const auto* lightingDescriptor = HasDescriptor<LightingDescriptor>()
			? &GetDescriptor<LightingDescriptor>()
			: nullptr;
		const auto passConstants = BuildDeferredPassConstants(GetFrameDescriptor(), lightingDescriptor, skyboxTexture != nullptr);
		m_passBuffer->SetRawData(&passConstants, sizeof(passConstants));
		m_passBuffer->Bind(NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(NLS::Render::RHI::BindingPointMap::kPassBindingSpace, 0));

		m_lightingMaterial->GetParameterBlock().Set("u_GBufferAlbedo", m_gBufferAlbedoTexture.get());
		m_lightingMaterial->GetParameterBlock().Set("u_GBufferNormal", m_gBufferNormalTexture.get());
		m_lightingMaterial->GetParameterBlock().Set("u_GBufferMaterial", m_gBufferMaterialTexture.get());
		if (m_gBufferDepthTexture)
			m_lightingMaterial->GetParameterBlock().Set("u_GBufferDepth", m_gBufferDepthTexture.get());
		if (m_lightingMaterial->GetParameterBlock().Contains("u_SkyboxCube"))
			m_lightingMaterial->GetParameterBlock().Set("u_SkyboxCube", skyboxTexture);

		auto commandBuffer = GetActiveExplicitCommandBuffer();
		auto device = GetExplicitDevice();

		NLS::Render::Entities::Drawable lightingDrawable;
		lightingDrawable.mesh = m_fullscreenQuad.get();
		lightingDrawable.material = m_lightingMaterial.get();
		lightingDrawable.primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;

		NLS::Render::Resources::MaterialPipelineStateOverrides compositeOverrides;
		compositeOverrides.depthTest = false;
		compositeOverrides.depthWrite = false;
		compositeOverrides.culling = false;
		compositeOverrides.colorWrite = true;

		auto material = lightingDrawable.material;
		auto mesh = lightingDrawable.mesh;

		auto pipeline = material->BuildRecordedGraphicsPipeline(
			device, lightingDrawable.primitiveMode, pso, compositeOverrides);
		auto bindingSet = material->GetRecordedBindingSet(device);
		auto rhiMesh = mesh->GetRHIMesh();
		NLS::Render::Core::ABaseRenderer::ExplicitUniformBufferBindingDesc passBindingDesc;
		passBindingDesc.set = NLS::Render::RHI::BindingPointMap::kPassDescriptorSet;
		passBindingDesc.registerSpace = NLS::Render::RHI::BindingPointMap::kPassBindingSpace;
		passBindingDesc.binding = 0u;
		passBindingDesc.range = sizeof(DeferredPassConstants);
		passBindingDesc.entryName = "PassConstants";
		passBindingDesc.layoutDebugName = "DeferredLightingPassBindingLayout";
		passBindingDesc.setDebugName = "DeferredLightingPassBindingSet";
		passBindingDesc.snapshotDebugName = "DeferredLightingPassConstantsSnapshot";
		passBindingDesc.stageMask = NLS::Render::RHI::ShaderStageMask::AllGraphics;
		auto passBindingSet = CreateExplicitUniformBufferBindingSet(*m_passBuffer, passBindingDesc);

		commandBuffer->BindGraphicsPipeline(pipeline);
		if (passBindingSet != nullptr)
			commandBuffer->BindBindingSet(NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, passBindingSet);
		commandBuffer->BindBindingSet(NLS::Render::RHI::BindingPointMap::kMaterialDescriptorSet, bindingSet);
		SubmitMeshDraw(commandBuffer, rhiMesh, material->GetGPUInstances());
	}
}

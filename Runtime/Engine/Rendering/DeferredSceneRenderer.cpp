#include "Rendering/DeferredSceneRenderer.h"

#include <fg/Blackboard.hpp>

#include <Debug/Logger.h>
#include <Math/Matrix4.h>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/Data/LightingDescriptor.h>
#include <Rendering/FrameGraph/FrameGraphExecutionContext.h>
#include <Rendering/FrameGraph/FrameGraphExecutionPlan.h>
#include <Rendering/FrameGraph/SceneRenderGraphBuilder.h>
#include <Rendering/Geometry/Vertex.h>
#include <Rendering/RHI/BindingPointMap.h>
#include <Rendering/Resources/Loaders/ShaderLoader.h>
#include <Rendering/Resources/Material.h>
#include <Rendering/Resources/Mesh.h>
#include <Rendering/Resources/Texture2D.h>
#include <Rendering/Resources/TextureCube.h>
#include <Rendering/Settings/EPrimitiveMode.h>

#include "Rendering/ScenePipelineStatePresets.h"

namespace
{
	using LightingDescriptor = NLS::Render::Data::LightingDescriptor;

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

		const bool usesThreadedRendering = NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver);

		auto drawables = ParseScene();
		const auto& frameDescriptor = GetFrameDescriptor();
		NLS::Render::Resources::TextureCube* skyboxTexture = nullptr;
		for (const auto& [_, drawable] : drawables.skyboxes)
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
		const bool hasSkyboxTexture = skyboxTexture != nullptr;

		if (usesThreadedRendering)
		{
			EnsureGBufferTargets(frameDescriptor.renderWidth, frameDescriptor.renderHeight);

			auto gbufferPso = CreateSceneDefaultPipelineState(*this);
			for (const auto& [_, drawable] : drawables.opaques)
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

				PreparedRecordedDraw preparedDraw;
				if (CaptureThreadedPreparedDraw(gbufferDrawable, gBufferOverrides, gbufferPso.depthFunc, preparedDraw))
					QueueThreadedRecordedDraw(preparedDraw);
			}

			{
				SetActivePreparedPassBindingSet(BaseSceneRenderer::GetPreparedPassBindingSetPlaceholder());

				m_lightingMaterial->GetParameterBlock().Set("u_GBufferAlbedo", m_gBufferAlbedoTexture.get());
				m_lightingMaterial->GetParameterBlock().Set("u_GBufferNormal", m_gBufferNormalTexture.get());
				m_lightingMaterial->GetParameterBlock().Set("u_GBufferMaterial", m_gBufferMaterialTexture.get());
				if (m_gBufferDepthTexture)
					m_lightingMaterial->GetParameterBlock().Set("u_GBufferDepth", m_gBufferDepthTexture.get());
				if (m_lightingMaterial->GetParameterBlock().Contains("u_SkyboxCube"))
					m_lightingMaterial->GetParameterBlock().Set("u_SkyboxCube", skyboxTexture);

				NLS::Render::Entities::Drawable lightingDrawable;
				lightingDrawable.mesh = m_fullscreenQuad.get();
				lightingDrawable.material = m_lightingMaterial.get();
				lightingDrawable.primitiveMode = NLS::Render::Settings::EPrimitiveMode::TRIANGLES;

				NLS::Render::Resources::MaterialPipelineStateOverrides compositeOverrides;
				compositeOverrides.depthTest = false;
				compositeOverrides.depthWrite = false;
				compositeOverrides.culling = false;
				compositeOverrides.colorWrite = true;

				PreparedRecordedDraw preparedDraw;
				if (CaptureThreadedPreparedDraw(lightingDrawable, compositeOverrides, gbufferPso.depthFunc, preparedDraw))
					QueueThreadedRecordedDraw(preparedDraw);
				SetActivePreparedPassBindingSet(nullptr);
			}

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

		AddDescriptor<DeferredSceneDescriptor>({
			std::move(drawables),
			std::move(scenePackage),
			hasSkyboxTexture });

		if (usesThreadedRendering && pendingFrameSnapshot.has_value())
		{
			auto snapshot = pendingFrameSnapshot.value();
			const NLS::Render::FrameGraph::DeferredPreparedSceneResourceRequest deferredResourceRequest{
				&m_gBuffer,
				m_gBufferAlbedoTexture.get(),
				m_gBufferNormalTexture.get(),
				m_gBufferMaterialTexture.get(),
				m_gBufferDepthTexture.get()
			};
			auto lightGridContext = NLS::Render::FrameGraph::BuildLightGridCompileContext(
				GetFrameDescriptor(),
				GetLightGridPrepass(),
				BuildLightGridFrameInputs(hasSkyboxTexture));
			auto frameDescriptorForBuilder = GetFrameDescriptor();
			auto deferredResources = NLS::Render::FrameGraph::CaptureDeferredPreparedSceneResources(deferredResourceRequest);

			SetPendingPreparedRenderSceneBuilder(
				[snapshot = std::move(snapshot),
				 frameDescriptorForBuilder,
				 lightGridContext = std::move(lightGridContext),
				 deferredResources = std::move(deferredResources)]() mutable
			{
				auto package = BuildSnapshotOwnedRenderScenePackage(
					snapshot,
					SnapshotRenderScenePackageBuildMode::SkipDefaultPassInputs);
				NLS::Render::FrameGraph::CompileAndApplyPreparedDeferredLightGridSceneExecution(
					package,
					lightGridContext,
					deferredResources);
				NLS::Render::FrameGraph::FinalizePreparedDeferredScenePackage(package, frameDescriptorForBuilder);
				return package;
			});
		}
	}

	void DeferredSceneRenderer::DrawFrame()
	{
		const bool usesThreadedRendering = NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_driver);

		// NOTE: Deferred rendering with threaded RHI requires proper GBuffer-to-Lighting
		// texture barrier handling in SubmitThreadedRhiFrame, which is not yet implemented.
		// For now, always use FrameGraph path for Deferred to ensure correct rendering.
		if (!usesThreadedRendering)
		{
			const auto& frame = GetFrameDescriptor();
			EnsureGBufferTargets(frame.renderWidth, frame.renderHeight);

			if (!m_gBufferShader || !m_lightingMaterial || !m_fullscreenQuad)
			{
				NLS_LOG_WARNING("DeferredSceneRenderer is missing shader or mesh resources; skipping deferred frame.");
				return;
			}

			FrameGraph frameGraph;
			FrameGraphBlackboard blackboard;
			const NLS::Render::FrameGraph::DeferredPreparedSceneResourceRequest deferredResourceRequest{
				&m_gBuffer,
				m_gBufferAlbedoTexture.get(),
				m_gBufferNormalTexture.get(),
				m_gBufferMaterialTexture.get(),
				m_gBufferDepthTexture.get()
			};
			const auto& scene = GetDescriptor<DeferredSceneDescriptor>();
			const auto lightGridContext = NLS::Render::FrameGraph::BuildLightGridCompileContext(
				frame,
				GetLightGridPrepass(),
				BuildLightGridFrameInputs(scene.hasSkyboxTexture));
			const auto resourceRequest = NLS::Render::FrameGraph::BuildDeferredGraphSceneResourceRequest(
				frameGraph,
				blackboard,
				frame,
				deferredResourceRequest);
			NLS::Render::FrameGraph::ReserveDeferredSceneGraph(frameGraph, resourceRequest);
			const auto preparedGraph = NLS::Render::FrameGraph::PrepareDeferredSceneGraph(
				resourceRequest,
				lightGridContext);
			NLS::Render::FrameGraph::ExecutePreparedDeferredSceneGraph(
				frameGraph,
				preparedGraph,
				{
					[this](const auto& beginDesc) -> bool
					{
						return BeginRecordedRenderPass(
							&m_gBuffer,
							beginDesc.renderWidth,
							beginDesc.renderHeight,
							beginDesc.clearColor,
							beginDesc.clearDepth,
							beginDesc.clearStencil,
							beginDesc.clearValue);
					},
					[this]()
					{
						DrawGBufferOpaques(CreateSceneDefaultPipelineState(*this));
					},
					[this]()
					{
						EndRecordedRenderPass();
					},
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
					[this]()
					{
						DrawLightingPass(CreateSceneFullscreenCompositePipelineState(*this));
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
		}

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

		if (!m_gBuffer.IsInitialized())
			m_gBuffer.Init(width, height, kAttachments, true);
		else
			m_gBuffer.Resize(width, height);

		const auto& colorResources = m_gBuffer.GetExplicitColorTextureHandles();
		if (colorResources.size() < 3)
			return;

		auto wrapTexture = [width, height](const std::shared_ptr<NLS::Render::RHI::RHITexture>& textureResource) -> std::unique_ptr<NLS::Render::Resources::Texture2D>
		{
			if (!textureResource)
				return nullptr;
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
		if (!m_lightingMaterial || !m_fullscreenQuad || !m_gBufferAlbedoTexture || !m_gBufferNormalTexture || !m_gBufferMaterialTexture)
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

		m_lightingMaterial->GetParameterBlock().Set("u_GBufferAlbedo", m_gBufferAlbedoTexture.get());
		m_lightingMaterial->GetParameterBlock().Set("u_GBufferNormal", m_gBufferNormalTexture.get());
		m_lightingMaterial->GetParameterBlock().Set("u_GBufferMaterial", m_gBufferMaterialTexture.get());
		if (m_gBufferDepthTexture)
			m_lightingMaterial->GetParameterBlock().Set("u_GBufferDepth", m_gBufferDepthTexture.get());
		if (m_lightingMaterial->GetParameterBlock().Contains("u_SkyboxCube"))
			m_lightingMaterial->GetParameterBlock().Set("u_SkyboxCube", skyboxTexture);

		auto commandBuffer = GetActiveExplicitCommandBuffer();
		auto device = GetExplicitDevice();
		auto pipelineCache = NLS::Render::Context::DriverRendererAccess::GetPipelineCache(GetDriver());

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
			device, pipelineCache, lightingDrawable.primitiveMode, pso, compositeOverrides);
		auto bindingSet = material->GetRecordedBindingSet(device);
		auto rhiMesh = mesh->GetRHIMesh();
		commandBuffer->BindGraphicsPipeline(pipeline);
		if (GetLightGridGraphicsPassBindingSet() != nullptr)
			commandBuffer->BindBindingSet(NLS::Render::RHI::BindingPointMap::kPassDescriptorSet, GetLightGridGraphicsPassBindingSet());
		commandBuffer->BindBindingSet(NLS::Render::RHI::BindingPointMap::kMaterialDescriptorSet, bindingSet);
		SubmitMeshDraw(commandBuffer, rhiMesh, material->GetGPUInstances());
	}
}

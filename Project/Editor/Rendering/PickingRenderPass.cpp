#include "Rendering/PickingRenderPass.h"
#include "Rendering/EditorDefaultResources.h"
#include "Core/EditorActions.h"
#include "Settings/EditorSettings.h"
#include "Rendering/DebugSceneRenderer.h"
#include "Rendering/EditorPipelineStatePresets.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include <Components/TransformComponent.h>
#include <Components/MaterialRenderer.h>
#include <Rendering/EngineDrawableDescriptor.h>
#include <algorithm>
#include <iterator>
using namespace NLS;
Editor::Rendering::PickingRenderPass::PickingRenderPass(NLS::Render::Core::CompositeRenderer& p_renderer) :
	NLS::Render::Core::ARenderPass(p_renderer),
	m_debugModelRenderer(p_renderer)
{
	/* Light Material */
	m_lightMaterial.SetShader(EDITOR_CONTEXT(editorResources)->GetShader("Billboard"));
	m_lightMaterial.Set("u_TextureTiling", Maths::Vector2(1.0f, 1.0f));
	m_lightMaterial.Set("u_TextureOffset", Maths::Vector2(0.0f, 0.0f));

	/* Picking Material */
	m_actorPickingMaterial.SetShader(EDITOR_CONTEXT(shaderManager)[":Shaders\\Unlit.hlsl"]);
	m_actorPickingMaterial.Set("u_Diffuse", Maths::Vector4(1.f, 1.f, 1.f, 1.0f));
	m_actorPickingMaterial.Set<NLS::Render::Resources::Texture2D*>("u_DiffuseMap", Editor::Rendering::GetEditorDefaultWhiteTexture());
	m_actorPickingMaterial.Set("u_TextureTiling", Maths::Vector2(1.0f, 1.0f));
	m_actorPickingMaterial.Set("u_TextureOffset", Maths::Vector2(0.0f, 0.0f));
}

Editor::Rendering::PickingRenderPass::PickingResult Editor::Rendering::PickingRenderPass::DecodePickingResult(
    const PickingReadbackLifecycle<Engine::SceneSystem::Scene>::Frame& frame,
	const uint8_t (&pixel)[3]) const
{
	const uint32_t pickID = (0u << 24u) | (pixel[2] << 16u) | (pixel[1] << 8u) | (pixel[0] << 0u);
    if (pickID == 0u || pickID > frame.pickRegistry.size())
        return std::nullopt;

	auto* actorUnderMouse = frame.pickRegistry[pickID - 1u];
	if (actorUnderMouse)
	{
        return actorUnderMouse;
	}

	return std::nullopt;
}

Editor::Rendering::PickingRenderPass::PickingResult Editor::Rendering::PickingRenderPass::PickAtRenderCoordinate(
	uint32_t p_x,
	uint32_t p_y)
{
    PromotePendingFrameIfReadbackAvailable();
    const auto* readableFrame = m_readbackLifecycle.GetReadableFrame();
	if (!SupportsPickingReadback() || readableFrame == nullptr)
		return std::nullopt;
    if (readableFrame->readbackTexture == nullptr)
        return std::nullopt;
    if (!NLS::Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(
        m_renderer.GetDriver(),
        readableFrame->readbackTexture))
    {
        return std::nullopt;
    }

	const uint64_t maxX = static_cast<uint64_t>(p_x) + 1u;
	const uint64_t maxY = static_cast<uint64_t>(p_y) + 1u;
	if (maxX > static_cast<uint64_t>(readableFrame->width) ||
		maxY > static_cast<uint64_t>(readableFrame->height))
	{
		return std::nullopt;
	}

	uint8_t pixel[3]{};
	NLS::Render::Context::DriverRendererAccess::ReadPixels(
        m_renderer.GetDriver(),
        readableFrame->readbackTexture,
		p_x,
		p_y,
		1u,
		1u,
		NLS::Render::Settings::EPixelDataFormat::RGB,
		NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
		pixel);

	return DecodePickingResult(*readableFrame, pixel);
}

bool Editor::Rendering::PickingRenderPass::SupportsPickingReadback() const
{
	return m_renderer.SupportsEditorPickingReadback();
}

bool Editor::Rendering::PickingRenderPass::HasReadablePickingFrame() const
{
    PromotePendingFrameIfReadbackAvailable();
    const auto* readableFrame = m_readbackLifecycle.GetReadableFrame();
    return readableFrame != nullptr &&
        NLS::Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(
            m_renderer.GetDriver(),
            readableFrame->readbackTexture);
}

uint64_t Editor::Rendering::PickingRenderPass::GetReadablePickingFrameSerial() const
{
    PromotePendingFrameIfReadbackAvailable();
    const auto* readableFrame = m_readbackLifecycle.GetReadableFrame();
    return readableFrame != nullptr &&
        NLS::Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(
            m_renderer.GetDriver(),
            readableFrame->readbackTexture)
        ? readableFrame->serial
        : 0u;
}

uint64_t Editor::Rendering::PickingRenderPass::GetSubmittedPickingFrameSerial() const
{
    return m_submittedPickingFrameSerial;
}

std::optional<NLS::Render::Context::RenderPassCommandInput> Editor::Rendering::PickingRenderPass::GetPreparedThreadedPassInput() const
{
    return m_preparedThreadedPassInput;
}

void Editor::Rendering::PickingRenderPass::OnBeginFrame(const NLS::Render::Data::FrameDescriptor&)
{
    PromotePendingFrameIfReadbackAvailable();
    m_preparedThreadedPassInput.reset();
}

void Editor::Rendering::PickingRenderPass::PromotePendingFrameIfReadbackAvailable() const
{
    const auto* pendingFrame = m_readbackLifecycle.GetPendingFrame();
    m_readbackLifecycle.PromotePendingFrameIfReadbackAvailable(
        pendingFrame != nullptr &&
        NLS::Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(
            m_renderer.GetDriver(),
            pendingFrame->readbackTexture));
}

void Editor::Rendering::PickingRenderPass::ResetPickingFrameState()
{
    m_readbackLifecycle.ResetSubmittedFrame();
}

Editor::Rendering::PickingReadbackLifecycle<Engine::SceneSystem::Scene>::Frame
Editor::Rendering::PickingRenderPass::BuildSubmittedReadbackFrame(
    Engine::SceneSystem::Scene& scene,
    const uint64_t serial) const
{
    const auto& frameDescriptor = m_renderer.GetFrameDescriptor();
    return {
        &scene,
        frameDescriptor.renderWidth,
        frameDescriptor.renderHeight,
        serial,
        m_actorPickingFramebuffer.GetExplicitTextureHandle(),
        m_submittedPickRegistry
    };
}

void Editor::Rendering::PickingRenderPass::Draw(NLS::Render::Data::PipelineState p_pso)
{
	if (!SupportsPickingReadback())
	{
        ResetPickingFrameState();
		return;
	}

	auto& frameDescriptor = m_renderer.GetFrameDescriptor();
    if (frameDescriptor.renderWidth == 0u || frameDescriptor.renderHeight == 0u)
    {
        ResetPickingFrameState();
        return;
    }

	auto& sceneDescriptor = m_renderer.GetDescriptor<Engine::Rendering::BaseSceneRenderer::SceneDescriptor>();
    auto& debugSceneDescriptor = m_renderer.GetDescriptor<DebugSceneRenderer::DebugSceneDescriptor>();
    if (!debugSceneDescriptor.requestPickingFrame)
        return;

    const uint64_t submittedSerial = ++m_submittedPickingFrameSerial;
    m_submittedPickRegistry.clear();
	m_actorPickingFramebuffer.Resize(frameDescriptor.renderWidth, frameDescriptor.renderHeight);
    if (NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_renderer.GetDriver()))
    {
        m_preparedThreadedPassInput = BuildThreadedPassInput(p_pso);
        if (!m_preparedThreadedPassInput.has_value())
        {
            ResetPickingFrameState();
            return;
        }
        m_readbackLifecycle.QueueSubmittedFrame(BuildSubmittedReadbackFrame(sceneDescriptor.scene, submittedSerial));
    }
    else if (!RenderPickingScene(p_pso))
    {
        ResetPickingFrameState();
        return;
    }
    else
    {
        m_readbackLifecycle.MarkSubmittedFrameImmediatelyReadable(
            BuildSubmittedReadbackFrame(sceneDescriptor.scene, submittedSerial));
    }
}

bool Editor::Rendering::PickingRenderPass::RenderPickingScene(NLS::Render::Data::PipelineState p_pso)
{
	// TODO: Make sure we only render when the view is hovered and not being resized.
	if (!SupportsPickingReadback())
		return false;

	using namespace Engine::Rendering;

	NLS_ASSERT(m_renderer.HasDescriptor<BaseSceneRenderer::SceneDescriptor>(), "Cannot find SceneDescriptor attached to this renderer");
	NLS_ASSERT(m_renderer.HasDescriptor<DebugSceneRenderer::DebugSceneDescriptor>(), "Cannot find DebugSceneDescriptor attached to this renderer");

	auto& sceneDescriptor = m_renderer.GetDescriptor<BaseSceneRenderer::SceneDescriptor>();
	auto& scene = sceneDescriptor.scene;

	const auto& frameDescriptor = m_renderer.GetFrameDescriptor();
	const bool startedPickingPass = m_renderer.BeginRecordedRenderPass(
		&m_actorPickingFramebuffer,
		frameDescriptor.renderWidth,
		frameDescriptor.renderHeight,
		true,
		true,
		true);

	if (!startedPickingPass)
		return false;

	DrawPickableModels(p_pso, scene);
	DrawPickableCameras(p_pso, scene);
	DrawPickableLights(p_pso, scene);

	m_renderer.EndRecordedRenderPass();
	return true;
}

std::optional<NLS::Render::Context::RenderPassCommandInput> Editor::Rendering::PickingRenderPass::BuildThreadedPassInput(
    NLS::Render::Data::PipelineState p_pso)
{
    using namespace Engine::Rendering;

    NLS_ASSERT(m_renderer.HasDescriptor<BaseSceneRenderer::SceneDescriptor>(), "Cannot find SceneDescriptor attached to this renderer");
    NLS_ASSERT(m_renderer.HasDescriptor<DebugSceneRenderer::DebugSceneDescriptor>(), "Cannot find DebugSceneDescriptor attached to this renderer");

    auto& sceneDescriptor = m_renderer.GetDescriptor<BaseSceneRenderer::SceneDescriptor>();
    auto& scene = sceneDescriptor.scene;
    const auto& frameDescriptor = m_renderer.GetFrameDescriptor();

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    passInput.debugName = "EditorPickingPass";
    passInput.queueType = NLS::Render::RHI::QueueType::Graphics;
    passInput.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.targetsSwapchain = false;
    passInput.renderWidth = frameDescriptor.renderWidth;
    passInput.renderHeight = frameDescriptor.renderHeight;
    passInput.clearColor = true;
    passInput.clearDepth = true;
    passInput.clearStencil = true;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;
    passInput.colorAttachmentViews = {
        m_actorPickingFramebuffer.GetOrCreateExplicitColorView("EditorPickingColorView")
    };
    passInput.depthStencilAttachmentView =
        m_actorPickingFramebuffer.GetOrCreateExplicitDepthStencilView("EditorPickingDepthView");

    CapturePickableModels(p_pso, scene, passInput.recordedDrawCommands);
    CapturePickableCameras(p_pso, scene, passInput.recordedDrawCommands);
    CapturePickableLights(p_pso, scene, passInput.recordedDrawCommands);

    passInput.drawCount = static_cast<uint64_t>(passInput.recordedDrawCommands.size());
    return passInput;
}

void PreparePickingMaterial(uint32_t pickID, NLS::Render::Resources::Material& p_material)
{
	auto bytes = reinterpret_cast<uint8_t*>(&pickID);
	auto color = Maths::Vector4{ bytes[0] / 255.0f, bytes[1] / 255.0f, bytes[2] / 255.0f, 1.0f };

	p_material.Set("u_Diffuse", color);
}

uint32_t Editor::Rendering::PickingRenderPass::RegisterPickableActor(Engine::GameObject& actor)
{
    auto found = std::find(m_submittedPickRegistry.begin(), m_submittedPickRegistry.end(), &actor);
    if (found != m_submittedPickRegistry.end())
        return static_cast<uint32_t>(std::distance(m_submittedPickRegistry.begin(), found) + 1);

    m_submittedPickRegistry.push_back(&actor);
    return static_cast<uint32_t>(m_submittedPickRegistry.size());
}

void Editor::Rendering::PickingRenderPass::CapturePickableModels(
    NLS::Render::Data::PipelineState p_pso,
    Engine::SceneSystem::Scene& p_scene,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    auto modelPickingPso = Editor::Rendering::CreateEditorUnculledPipelineState(p_pso);

    for (auto modelRenderer : p_scene.GetFastAccessComponents().modelRenderers)
    {
        auto& actor = *modelRenderer->gameobject();
        if (!actor.IsActive())
            continue;

        auto model = modelRenderer->GetModel();
        auto materialRenderer = modelRenderer->gameobject()->GetComponent<Engine::Components::MaterialRenderer>();
        if (model == nullptr || materialRenderer == nullptr)
            continue;

        const auto& materials = materialRenderer->GetMaterials();
        const auto& modelMatrix = actor.GetTransform()->GetWorldMatrix();
        PreparePickingMaterial(RegisterPickableActor(actor), m_actorPickingMaterial);

        for (auto mesh : model->GetMeshes())
        {
            auto stateMask = m_actorPickingMaterial.GenerateStateMask();
            if (auto material = materials.at(mesh->GetMaterialIndex()); material && material->IsValid())
                stateMask = material->GenerateStateMask();

            NLS::Render::Entities::Drawable drawable;
            drawable.mesh = mesh;
            drawable.material = &m_actorPickingMaterial;
            drawable.stateMask = stateMask;
            drawable.AddDescriptor<Engine::Rendering::EngineDrawableDescriptor>({
                modelMatrix
            });

            NLS::Render::Resources::MaterialPipelineStateOverrides unculledOverrides;
            unculledOverrides.culling = false;

            NLS::Render::Context::RecordedDrawCommandInput drawCommand;
            if (m_renderer.CaptureRecordedDrawCommand(
                drawable,
                unculledOverrides,
                NLS::Render::Settings::EComparaisonAlgorithm::LESS,
                drawCommand))
            {
                outDrawCommands.push_back(std::move(drawCommand));
            }
        }
    }
}

void Editor::Rendering::PickingRenderPass::DrawPickableModels(
	NLS::Render::Data::PipelineState p_pso,
	Engine::SceneSystem::Scene& p_scene
)
{
	auto modelPickingPso = Editor::Rendering::CreateEditorUnculledPipelineState(p_pso);

	for (auto modelRenderer : p_scene.GetFastAccessComponents().modelRenderers)
	{
		auto& actor = *modelRenderer->gameobject();

		if (actor.IsActive())
		{
			if (auto model = modelRenderer->GetModel())
			{
                if (auto materialRenderer = modelRenderer->gameobject()->GetComponent<Engine::Components::MaterialRenderer>())
				{
					const auto& materials = materialRenderer->GetMaterials();
					const auto& modelMatrix = actor.GetTransform()->GetWorldMatrix();

					PreparePickingMaterial(RegisterPickableActor(actor), m_actorPickingMaterial);

					for (auto mesh : model->GetMeshes())
					{
						auto stateMask = m_actorPickingMaterial.GenerateStateMask();

						// Override the state mask to use the material state mask (if this one is valid)
						if (auto material = materials.at(mesh->GetMaterialIndex()); material && material->IsValid())
						{
							stateMask = material->GenerateStateMask();
						}

						NLS::Render::Entities::Drawable drawable;
						drawable.mesh = mesh;
						drawable.material = &m_actorPickingMaterial;
						drawable.stateMask = stateMask;

						drawable.AddDescriptor<Engine::Rendering::EngineDrawableDescriptor>({
							modelMatrix
						});

						NLS::Render::Resources::MaterialPipelineStateOverrides unculledOverrides;
						unculledOverrides.culling = false;
						m_renderer.DrawEntity(drawable, unculledOverrides);
					}
				}
			}
		}
	}
}

void Editor::Rendering::PickingRenderPass::DrawPickableCameras(
	NLS::Render::Data::PipelineState p_pso,
	Engine::SceneSystem::Scene& p_scene
)
{
	auto cameraPickingPso = Editor::Rendering::CreateEditorUnculledPipelineState(p_pso);

	for (auto camera : p_scene.GetFastAccessComponents().cameras)
	{
		auto& actor = *camera->gameobject();

		if (actor.IsActive())
		{
			PreparePickingMaterial(RegisterPickableActor(actor), m_actorPickingMaterial);
			auto& cameraModel = *EDITOR_CONTEXT(editorResources)->GetModel("Camera");
            auto translation = Maths::Matrix4::Translation(actor.GetTransform()->GetWorldPosition());
            auto rotation = Maths::Quaternion::ToMatrix4(actor.GetTransform()->GetWorldRotation());
			auto modelMatrix = translation * rotation;

			m_debugModelRenderer.DrawModelWithSingleMaterial(cameraPickingPso, cameraModel, m_actorPickingMaterial, modelMatrix);
		}
	}
}

void Editor::Rendering::PickingRenderPass::CapturePickableCameras(
    NLS::Render::Data::PipelineState p_pso,
    Engine::SceneSystem::Scene& p_scene,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    auto cameraPickingPso = Editor::Rendering::CreateEditorUnculledPipelineState(p_pso);
    auto* cameraModel = EDITOR_CONTEXT(editorResources)->GetModel("Camera");
    if (cameraModel == nullptr)
        return;

    for (auto camera : p_scene.GetFastAccessComponents().cameras)
    {
        auto& actor = *camera->gameobject();
        if (!actor.IsActive())
            continue;

        PreparePickingMaterial(RegisterPickableActor(actor), m_actorPickingMaterial);
        auto translation = Maths::Matrix4::Translation(actor.GetTransform()->GetWorldPosition());
        auto rotation = Maths::Quaternion::ToMatrix4(actor.GetTransform()->GetWorldRotation());
        auto modelMatrix = translation * rotation;
        m_debugModelRenderer.CaptureModelDrawCommandsWithSingleMaterial(
            cameraPickingPso,
            *cameraModel,
            m_actorPickingMaterial,
            modelMatrix,
            outDrawCommands);
    }
}

void Editor::Rendering::PickingRenderPass::DrawPickableLights(
	NLS::Render::Data::PipelineState p_pso,
	Engine::SceneSystem::Scene& p_scene
)
{
    const auto lightBillboardScale = Settings::EditorSettings::GetDebugDrawSettingsObject().lightBillboardScale;
	if (lightBillboardScale > 0.001f)
	{
		auto lightPickingPso = Editor::Rendering::CreateEditorOverlayPipelineState(p_pso);

		m_lightMaterial.Set<float>("u_Scale", lightBillboardScale * 0.1f);

		for (auto light : p_scene.GetFastAccessComponents().lights)
		{
			auto& actor = *light->gameobject();

			if (actor.IsActive())
			{
				PreparePickingMaterial(RegisterPickableActor(actor), m_lightMaterial);
				auto& lightModel = *EDITOR_CONTEXT(editorResources)->GetModel("Vertical_Plane");
				auto modelMatrix = Maths::Matrix4::Translation(actor.GetTransform()->GetWorldPosition());

				m_debugModelRenderer.DrawModelWithSingleMaterial(lightPickingPso, lightModel, m_lightMaterial, modelMatrix);
			}
		}
	}
}

void Editor::Rendering::PickingRenderPass::CapturePickableLights(
    NLS::Render::Data::PipelineState p_pso,
    Engine::SceneSystem::Scene& p_scene,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    const auto lightBillboardScale = Settings::EditorSettings::GetDebugDrawSettingsObject().lightBillboardScale;
    if (lightBillboardScale <= 0.001f)
        return;

    auto lightPickingPso = Editor::Rendering::CreateEditorOverlayPipelineState(p_pso);
    auto* lightModel = EDITOR_CONTEXT(editorResources)->GetModel("Vertical_Plane");
    if (lightModel == nullptr)
        return;
    m_lightMaterial.Set<float>("u_Scale", lightBillboardScale * 0.1f);

    for (auto light : p_scene.GetFastAccessComponents().lights)
    {
        auto& actor = *light->gameobject();
        if (!actor.IsActive())
            continue;

        PreparePickingMaterial(RegisterPickableActor(actor), m_lightMaterial);
        auto modelMatrix = Maths::Matrix4::Translation(actor.GetTransform()->GetWorldPosition());
        m_debugModelRenderer.CaptureModelDrawCommandsWithSingleMaterial(
            lightPickingPso,
            *lightModel,
            m_lightMaterial,
            modelMatrix,
            outDrawCommands);
    }
}


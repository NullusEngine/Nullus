#include "Rendering/PickingRenderPass.h"
#include "Rendering/EditorDefaultResources.h"
#include "Core/EditorActions.h"
#include "Panels/SceneViewPickingPolicy.h"
#include "Settings/EditorSettings.h"
#include "Rendering/DebugSceneRenderer.h"
#include "Rendering/EditorPipelineStatePresets.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/RHI/Core/RHIResource.h"
#include "Profiling/Profiler.h"
#include <ServiceLocator.h>
#include <Debug/Logger.h>
#include <Components/MeshFilter.h>
#include <Components/MeshRenderer.h>
#include <Components/TransformComponent.h>
#include <Rendering/EngineDrawableDescriptor.h>
#include <iterator>
#include <cstring>
using namespace NLS;

namespace
{
    void HashCombine(uint64_t& seed, const uint64_t value)
    {
        seed ^= value + 0x9E3779B97F4A7C15ull + (seed << 6u) + (seed >> 2u);
    }

    void HashPointer(uint64_t& seed, const void* value)
    {
        HashCombine(seed, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value)));
    }

    void HashFloat(uint64_t& seed, const float value)
    {
        uint32_t bits = 0u;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(value));
        HashCombine(seed, bits);
    }

    void HashMatrix(uint64_t& seed, const Maths::Matrix4& matrix)
    {
        for (const float value : matrix.data)
            HashFloat(seed, value);
    }

    void HashPickableSource(
        uint64_t& seed,
        const Engine::Rendering::ScenePickablePrimitiveDrawSource& source)
    {
        HashPointer(seed, source.owner);
        HashCombine(seed, source.owner != nullptr ? source.owner->GetInstanceID() : 0u);
        HashPointer(seed, source.mesh);
        HashCombine(seed, source.mesh != nullptr ? source.mesh->GetContentRevision() : 0u);
        HashCombine(seed, source.stateMask.mask);
        HashMatrix(seed, source.worldMatrix);
    }

    void HashPickableActor(
        uint64_t& seed,
        const Engine::GameObject& actor)
    {
        HashPointer(seed, &actor);
        HashCombine(seed, actor.GetInstanceID());
        HashCombine(seed, actor.IsAlive() ? 1u : 0u);
        HashCombine(seed, actor.IsActive() ? 1u : 0u);
        if (actor.GetTransform() != nullptr)
            HashMatrix(seed, actor.GetTransform()->GetWorldMatrix());
    }

    template<typename Lifecycle>
    class ScopedPickingPixelReadback
    {
    public:
        explicit ScopedPickingPixelReadback(Lifecycle& lifecycle)
            : m_lifecycle(&lifecycle)
        {
        }

        ~ScopedPickingPixelReadback()
        {
            if (m_lifecycle != nullptr)
                m_lifecycle->EndPixelReadback();
        }

        ScopedPickingPixelReadback(const ScopedPickingPixelReadback&) = delete;
        ScopedPickingPixelReadback& operator=(const ScopedPickingPixelReadback&) = delete;

    private:
        Lifecycle* m_lifecycle = nullptr;
    };

    bool HasCompletedPickingReadback(
        const NLS::Render::Context::Driver& driver,
        const Editor::Rendering::PickingReadbackLifecycle<Engine::SceneSystem::Scene>::Frame& frame)
    {
        if (frame.readbackTexture == nullptr)
            return false;
        if (frame.readbackGeneration != 0u)
        {
            return NLS::Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(
                driver,
                frame.readbackTexture,
                frame.readbackGeneration);
        }

        return NLS::Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(
            driver,
            frame.readbackTexture);
    }
}

Editor::Rendering::PickingRenderPass::PickingRenderPass(NLS::Render::Core::CompositeRenderer& p_renderer) :
	NLS::Render::Core::ARenderPass(p_renderer),
	m_debugModelRenderer(p_renderer)
{
	/* Light Material */
	m_lightMaterial.SetShader(EDITOR_CONTEXT(editorResources)->GetShader("Billboard"));
	m_lightMaterial.Set("u_TextureTiling", Maths::Vector2(1.0f, 1.0f));
	m_lightMaterial.Set("u_TextureOffset", Maths::Vector2(0.0f, 0.0f));

	/* Picking Material */
	m_gameObjectPickingMaterial.SetShader(EDITOR_CONTEXT(shaderManager)[":Shaders\\Unlit.hlsl"]);
	m_gameObjectPickingMaterial.Set("u_Diffuse", Maths::Vector4(1.f, 1.f, 1.f, 1.0f));
	m_gameObjectPickingMaterial.Set<NLS::Render::Resources::Texture2D*>("u_DiffuseMap", Editor::Rendering::GetEditorDefaultWhiteTexture());
	m_gameObjectPickingMaterial.Set("u_TextureTiling", Maths::Vector2(1.0f, 1.0f));
	m_gameObjectPickingMaterial.Set("u_TextureOffset", Maths::Vector2(0.0f, 0.0f));
}

Editor::Rendering::PickingRenderPass::PickingResult Editor::Rendering::PickingRenderPass::DecodePickingResult(
    const PickingReadbackLifecycle<Engine::SceneSystem::Scene>::Frame& frame,
	const uint8_t (&pixel)[3]) const
{
	const uint32_t pickID = (0u << 24u) | (pixel[2] << 16u) | (pixel[1] << 8u) | (pixel[0] << 0u);
    if (pickID == 0u || pickID > frame.pickRegistry.size())
        return std::nullopt;

	auto* gameObjectUnderMouse = frame.pickRegistry[pickID - 1u];
	if (gameObjectUnderMouse && gameObjectUnderMouse->IsAlive())
	{
        return gameObjectUnderMouse;
	}

	return std::nullopt;
}

Editor::Rendering::PickingRenderPass::PickingResult Editor::Rendering::PickingRenderPass::PickAtRenderCoordinate(
	uint32_t p_x,
	uint32_t p_y)
{
    NLS_PROFILE_NAMED_SCOPE("PickingRenderPass::PickAtRenderCoordinate");
    PromotePendingFrameIfReadbackAvailable();
    const auto* readableFrame = m_readbackLifecycle.GetReadableFrame();
	if (!SupportsPickingReadback() || readableFrame == nullptr)
		return std::nullopt;
    if (!HasCompletedPickingReadback(m_renderer.GetDriver(), *readableFrame))
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

    if (!m_readbackLifecycle.TryBeginPixelReadback())
        return std::nullopt;
    ScopedPickingPixelReadback pixelReadback(m_readbackLifecycle);

	uint8_t pixel[3]{};
    NLS::Render::RHI::RHIReadbackResult readbackResult;
    {
        NLS_PROFILE_NAMED_SCOPE("PickingRenderPass::PickAtRenderCoordinate::ReadPixelsChecked");
	    readbackResult = NLS::Render::Context::DriverRendererAccess::ReadPixelsChecked(
            m_renderer.GetDriver(),
            readableFrame->readbackTexture,
		    p_x,
		    p_y,
		    1u,
		    1u,
		    NLS::Render::Settings::EPixelDataFormat::RGB,
		    NLS::Render::Settings::EPixelDataType::UNSIGNED_BYTE,
		    pixel);
    }
	if (!readbackResult.Succeeded())
	{
        if (readbackResult.message.find("previous async readback has not been completed") == std::string::npos)
        {
		    NLS_LOG_WARNING("PickingRenderPass::PickAtRenderCoordinate readback failed: " + readbackResult.message);
        }
		return std::nullopt;
	}

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
        HasCompletedPickingReadback(m_renderer.GetDriver(), *readableFrame);
}

uint64_t Editor::Rendering::PickingRenderPass::GetReadablePickingFrameSerial() const
{
    PromotePendingFrameIfReadbackAvailable();
    const auto* readableFrame = m_readbackLifecycle.GetReadableFrame();
    return readableFrame != nullptr &&
        HasCompletedPickingReadback(m_renderer.GetDriver(), *readableFrame)
        ? readableFrame->serial
        : 0u;
}

uint64_t Editor::Rendering::PickingRenderPass::GetSubmittedPickingFrameSerial() const
{
    return m_submittedPickingFrameSerial;
}

std::optional<NLS::Editor::Panels::HitProxyPickingSignature>
Editor::Rendering::PickingRenderPass::GetReadablePickingFrameSignature() const
{
    PromotePendingFrameIfReadbackAvailable();
    const auto* readableFrame = m_readbackLifecycle.GetReadableFrame();
    if (readableFrame == nullptr ||
        !HasCompletedPickingReadback(m_renderer.GetDriver(), *readableFrame))
    {
        return std::nullopt;
    }

    return readableFrame->signature;
}

std::optional<NLS::Editor::Panels::HitProxyPickingSignature>
Editor::Rendering::PickingRenderPass::GetSubmittedPickingFrameSignature() const
{
    if (const auto* pendingFrame = m_readbackLifecycle.GetPendingFrame(); pendingFrame != nullptr)
        return pendingFrame->signature;
    if (const auto* readableFrame = m_readbackLifecycle.GetReadableFrame(); readableFrame != nullptr)
        return readableFrame->signature;
    return std::nullopt;
}

bool Editor::Rendering::PickingRenderPass::CanResolvePickingRequest(
    const NLS::Editor::Panels::HitProxyPickingRequestKind requestKind,
    const uint64_t minimumReadablePickingFrameSerial,
    const NLS::Editor::Panels::HitProxyPickingSignature& requestSignature) const
{
    PromotePendingFrameIfReadbackAvailable();
    const auto* readableFrame = m_readbackLifecycle.GetReadableFrame();
    return readableFrame != nullptr &&
        HasCompletedPickingReadback(m_renderer.GetDriver(), *readableFrame) &&
        NLS::Editor::Panels::ShouldResolveHitProxyPickingRequest(
            requestKind,
            true,
            readableFrame->serial,
            minimumReadablePickingFrameSerial,
            requestSignature,
            readableFrame->signature);
}

const std::optional<NLS::Render::Context::RenderPassCommandInput>& Editor::Rendering::PickingRenderPass::GetPreparedThreadedPassInput() const
{
    return m_preparedThreadedPassInput;
}

std::optional<NLS::Render::Context::RenderPassCommandInput> Editor::Rendering::PickingRenderPass::ConsumePreparedThreadedPassInput()
{
    auto passInput = std::move(m_preparedThreadedPassInput);
    m_preparedThreadedPassInput.reset();
    return passInput;
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
        HasCompletedPickingReadback(m_renderer.GetDriver(), *pendingFrame));
}

void Editor::Rendering::PickingRenderPass::ResetPickingFrameState()
{
    m_readbackLifecycle.ResetSubmittedFrame();
}

Editor::Rendering::PickingReadbackLifecycle<Engine::SceneSystem::Scene>::Frame
Editor::Rendering::PickingRenderPass::BuildSubmittedReadbackFrame(
    Engine::SceneSystem::Scene& scene,
    const uint64_t serial,
    const uint64_t readbackGeneration,
    const NLS::Editor::Panels::HitProxyPickingSignature& signature) const
{
    const auto& frameDescriptor = m_renderer.GetFrameDescriptor();
    return {
        &scene,
        frameDescriptor.renderWidth,
        frameDescriptor.renderHeight,
        serial,
        m_gameObjectPickingFramebuffer.GetExplicitTextureHandle(),
        readbackGeneration,
        m_submittedPickRegistry,
        signature
    };
}

NLS::Editor::Panels::HitProxyPickingSignature
Editor::Rendering::PickingRenderPass::BuildCurrentPickingSignature(
    Engine::SceneSystem::Scene& scene) const
{
    const auto& frameDescriptor = m_renderer.GetFrameDescriptor();
    NLS::Editor::Panels::HitProxyPickingSignature signature;
    signature.renderWidth = frameDescriptor.renderWidth;
    signature.renderHeight = frameDescriptor.renderHeight;
    signature.viewId = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&m_renderer));

    uint64_t cameraHash = 0xCAFE51CEBEEFu;
    if (frameDescriptor.camera != nullptr)
    {
        HashPointer(cameraHash, frameDescriptor.camera);
        HashMatrix(cameraHash, frameDescriptor.camera->GetViewMatrix());
        HashMatrix(cameraHash, frameDescriptor.camera->GetProjectionMatrix());
    }
    signature.cameraViewHash = cameraHash;

    uint64_t sceneRevision = 0x51CE0001u;
    HashPointer(sceneRevision, &scene);
    if (NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::EditorActions>())
    {
        const auto token = EDITOR_EXEC(CaptureSceneMutationToken());
        HashCombine(sceneRevision, token.mainSceneGeneration);
        HashCombine(sceneRevision, token.prefabStageGeneration);
    }
    signature.pickableSceneRevision = sceneRevision;

    uint64_t drawSourceHash = 0xD2A7500Du;
    if (const auto* sceneRenderer = dynamic_cast<Engine::Rendering::BaseSceneRenderer*>(&m_renderer);
        sceneRenderer != nullptr && sceneRenderer->HasLastVisiblePickablePrimitiveDrawSources())
    {
        const auto& sources = sceneRenderer->GetLastVisiblePickablePrimitiveDrawSources();
        HashCombine(drawSourceHash, static_cast<uint64_t>(sources.size()));
        for (const auto& source : sources)
            HashPickableSource(drawSourceHash, source);
    }
    else
    {
        const auto& fastAccess = scene.GetFastAccessComponents();
        HashCombine(drawSourceHash, static_cast<uint64_t>(fastAccess.modelRenderers.size()));
        for (auto* modelRenderer : fastAccess.modelRenderers)
        {
            if (modelRenderer == nullptr || modelRenderer->gameobject() == nullptr)
                continue;

            auto& actor = *modelRenderer->gameobject();
            HashPickableActor(drawSourceHash, actor);
            if (auto* meshFilter = actor.GetComponent<Engine::Components::MeshFilter>())
            {
                auto* mesh = meshFilter->ResolveMesh();
                HashPointer(drawSourceHash, mesh);
                HashCombine(drawSourceHash, mesh != nullptr ? mesh->GetContentRevision() : 0u);
            }
        }
    }
    const auto& fastAccess = scene.GetFastAccessComponents();
    HashCombine(drawSourceHash, static_cast<uint64_t>(fastAccess.cameras.size()));
    for (auto* cameraComponent : fastAccess.cameras)
    {
        if (cameraComponent == nullptr || cameraComponent->gameobject() == nullptr)
            continue;
        HashPickableActor(drawSourceHash, *cameraComponent->gameobject());
    }
    HashCombine(drawSourceHash, static_cast<uint64_t>(fastAccess.lights.size()));
    for (auto* lightComponent : fastAccess.lights)
    {
        if (lightComponent == nullptr || lightComponent->gameobject() == nullptr)
            continue;
        HashPickableActor(drawSourceHash, *lightComponent->gameobject());
        const auto* lightData = lightComponent->GetData();
        HashPointer(drawSourceHash, lightData);
        HashCombine(drawSourceHash, lightData != nullptr ? static_cast<uint64_t>(lightData->type) : 0u);
    }
    signature.pickableDrawSourceHash = drawSourceHash;
    return signature;
}

bool Editor::Rendering::PickingRenderPass::CanReuseReadablePickingFrameForSignature(
    const NLS::Editor::Panels::HitProxyPickingSignature& signature) const
{
    PromotePendingFrameIfReadbackAvailable();
    const auto* readableFrame = m_readbackLifecycle.GetReadableFrame();
    return readableFrame != nullptr &&
        HasCompletedPickingReadback(m_renderer.GetDriver(), *readableFrame) &&
        NLS::Editor::Panels::ShouldReuseHitProxyPickingFrame(
            true,
            signature,
            readableFrame->signature);
}

void Editor::Rendering::PickingRenderPass::Draw(NLS::Render::Data::PipelineState p_pso)
{
    NLS_PROFILE_NAMED_SCOPE("PickingRenderPass::Draw");
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

    uint64_t visiblePickableDrawCount = 0u;
    const auto* sceneRenderer = dynamic_cast<Engine::Rendering::BaseSceneRenderer*>(&m_renderer);
    if (sceneRenderer != nullptr && sceneRenderer->HasLastVisiblePickablePrimitiveDrawSources())
    {
        visiblePickableDrawCount =
            static_cast<uint64_t>(sceneRenderer->GetLastVisiblePickablePrimitiveDrawSources().size());
    }

    const auto recordPickingDiagnostics =
        [&](const uint64_t rebuiltFrames, const uint64_t reusedFrames, const uint64_t hoverBudgetSkips)
        {
            NLS::Render::Data::PickingDiagnostics diagnostics;
            diagnostics.rebuiltFrames = rebuiltFrames;
            diagnostics.reusedFrames = reusedFrames;
            diagnostics.hoverBudgetSkips = hoverBudgetSkips;
            diagnostics.pendingReadback = m_readbackLifecycle.GetPendingFrame() != nullptr;
            diagnostics.submittedSerial = m_submittedPickingFrameSerial;
            diagnostics.readableSerial = GetReadablePickingFrameSerial();
            diagnostics.clickMinimumSerial = debugSceneDescriptor.clickMinimumPickingFrameSerial;
            diagnostics.visiblePickableDrawCount = visiblePickableDrawCount;
            m_renderer.RecordPickingDiagnostics(diagnostics);
        };

    if (sceneRenderer != nullptr &&
        sceneRenderer->HasLastVisiblePickablePrimitiveDrawSources() &&
        Editor::Panels::ShouldSkipSceneHoverPickingForVisibleDrawBudget(
            debugSceneDescriptor.requestPickingFrameForClick,
            visiblePickableDrawCount,
            debugSceneDescriptor.hoverPickingVisibleDrawBudget))
    {
        NLS_PROFILE_NAMED_SCOPE("EditorPicking::SkipHoverBudget");
        recordPickingDiagnostics(0u, 0u, 1u);
        return;
    }

    const auto pickingSignature = BuildCurrentPickingSignature(sceneDescriptor.scene);

    if (!debugSceneDescriptor.requestPickingFrameForClick &&
        CanReuseReadablePickingFrameForSignature(pickingSignature))
    {
        NLS_PROFILE_NAMED_SCOPE("EditorPicking::Reuse");
        recordPickingDiagnostics(0u, 1u, 0u);
        return;
    }

    NLS_PROFILE_NAMED_SCOPE("EditorPicking::Rebuild");
    const uint64_t submittedSerial = ++m_submittedPickingFrameSerial;
    m_submittedPickRegistry.clear();
    m_submittedPickIds.clear();
	m_gameObjectPickingFramebuffer.Resize(frameDescriptor.renderWidth, frameDescriptor.renderHeight);
    const auto readbackTexture = m_gameObjectPickingFramebuffer.GetExplicitTextureHandle();
    NLS::Render::Context::DriverRendererAccess::InvalidateCompletedReadbackTexture(
        m_renderer.GetDriver(),
        readbackTexture);
    if (NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_renderer.GetDriver()))
    {
        m_preparedThreadedPassInput = BuildThreadedPassInput(p_pso, 0u);
        if (!m_preparedThreadedPassInput.has_value())
        {
            ResetPickingFrameState();
            return;
        }
        const uint64_t readbackGeneration =
            NLS::Render::Context::DriverRendererAccess::BeginReadbackTextureSubmission(
                m_renderer.GetDriver(),
                readbackTexture);
        m_preparedThreadedPassInput->readbackTextureGeneration = readbackGeneration;
        m_readbackLifecycle.QueueSubmittedFrame(BuildSubmittedReadbackFrame(
            sceneDescriptor.scene,
            submittedSerial,
            readbackGeneration,
            pickingSignature));
        recordPickingDiagnostics(1u, 0u, 0u);
    }
    else if (!RenderPickingScene(p_pso))
    {
        ResetPickingFrameState();
        return;
    }
    else
    {
        m_readbackLifecycle.MarkSubmittedFrameImmediatelyReadable(
            BuildSubmittedReadbackFrame(sceneDescriptor.scene, submittedSerial, 0u, pickingSignature));
        recordPickingDiagnostics(1u, 0u, 0u);
    }
}

bool Editor::Rendering::PickingRenderPass::RenderPickingScene(NLS::Render::Data::PipelineState p_pso)
{
    NLS_PROFILE_NAMED_SCOPE("PickingRenderPass::RenderPickingScene");
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
		&m_gameObjectPickingFramebuffer,
		frameDescriptor.renderWidth,
		frameDescriptor.renderHeight,
		true,
		true,
		true);

	if (!startedPickingPass)
		return false;

    if (const auto* sceneRenderer = dynamic_cast<Engine::Rendering::BaseSceneRenderer*>(&m_renderer);
        sceneRenderer != nullptr && sceneRenderer->HasLastVisiblePickablePrimitiveDrawSources())
    {
        DrawPickableModelSources(sceneRenderer->GetLastVisiblePickablePrimitiveDrawSources());
    }
    else
    {
	    DrawPickableModels(p_pso, scene);
    }
	DrawPickableCameras(p_pso, scene);
	DrawPickableLights(p_pso, scene);

	m_renderer.EndRecordedRenderPass();
	return true;
}

std::optional<NLS::Render::Context::RenderPassCommandInput> Editor::Rendering::PickingRenderPass::BuildThreadedPassInput(
    NLS::Render::Data::PipelineState p_pso,
    const uint64_t readbackGeneration)
{
    NLS_PROFILE_NAMED_SCOPE("PickingRenderPass::BuildThreadedPassInput");
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
    passInput.writesDepthStencilAttachment = true;
    passInput.readbackTextureGeneration = readbackGeneration;
    passInput.colorAttachmentViews = {
        m_gameObjectPickingFramebuffer.GetOrCreateExplicitColorView("EditorPickingColorView")
    };
    passInput.depthStencilAttachmentView =
        m_gameObjectPickingFramebuffer.GetOrCreateExplicitDepthStencilView("EditorPickingDepthView");

    if (const auto* sceneRenderer = dynamic_cast<Engine::Rendering::BaseSceneRenderer*>(&m_renderer);
        sceneRenderer != nullptr && sceneRenderer->HasLastVisiblePickablePrimitiveDrawSources())
    {
        const auto& sources = sceneRenderer->GetLastVisiblePickablePrimitiveDrawSources();
        passInput.recordedDrawCommands.reserve(
            sources.size() +
            scene.GetFastAccessComponents().cameras.size() +
            scene.GetFastAccessComponents().lights.size());
        CapturePickableModelSources(sources, passInput.recordedDrawCommands);
    }
    else
    {
        passInput.recordedDrawCommands.reserve(
            scene.GetFastAccessComponents().modelRenderers.size() +
            scene.GetFastAccessComponents().cameras.size() +
            scene.GetFastAccessComponents().lights.size());
        CapturePickableModels(p_pso, scene, passInput.recordedDrawCommands);
    }
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

uint32_t Editor::Rendering::PickingRenderPass::RegisterPickableGameObject(Engine::GameObject& actor)
{
    const auto found = m_submittedPickIds.find(&actor);
    if (found != m_submittedPickIds.end())
        return found->second;

    m_submittedPickRegistry.push_back(&actor);
    const auto pickId = static_cast<uint32_t>(m_submittedPickRegistry.size());
    m_submittedPickIds.emplace(&actor, pickId);
    return pickId;
}

void Editor::Rendering::PickingRenderPass::CapturePickableModels(
    NLS::Render::Data::PipelineState p_pso,
    Engine::SceneSystem::Scene& p_scene,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    NLS_PROFILE_NAMED_SCOPE("PickingRenderPass::CapturePickableModels");
    auto modelPickingPso = Editor::Rendering::CreateEditorUnculledPipelineState(p_pso);

    for (auto modelRenderer : p_scene.GetFastAccessComponents().modelRenderers)
    {
        auto& actor = *modelRenderer->gameobject();
        if (!actor.IsActive())
            continue;

        auto* meshFilter = actor.GetComponent<Engine::Components::MeshFilter>();
        auto* mesh = meshFilter != nullptr ? meshFilter->ResolveMesh() : nullptr;
        if (mesh == nullptr)
            continue;

        const auto* materials = &modelRenderer->ResolveMaterials();
        const auto& modelMatrix = actor.GetTransform()->GetWorldMatrix();
        PreparePickingMaterial(RegisterPickableGameObject(actor), m_gameObjectPickingMaterial);

        auto stateMask = m_gameObjectPickingMaterial.GenerateStateMask();
        if (materials && mesh->GetMaterialIndex() < materials->size())
        {
            if (auto material = materials->at(mesh->GetMaterialIndex()); material && material->IsValid())
                stateMask = material->GenerateStateMask();
        }

        NLS::Render::Entities::Drawable drawable;
        drawable.mesh = mesh;
        drawable.material = &m_gameObjectPickingMaterial;
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

void Editor::Rendering::PickingRenderPass::CapturePickableModelSources(
    const std::vector<Engine::Rendering::ScenePickablePrimitiveDrawSource>& sources,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    NLS_PROFILE_NAMED_SCOPE("PickingRenderPass::CapturePickableModelSources");

    for (const auto& source : sources)
    {
        if (source.owner == nullptr || source.mesh == nullptr || !source.owner->IsActive())
            continue;

        PreparePickingMaterial(RegisterPickableGameObject(*source.owner), m_gameObjectPickingMaterial);

        NLS::Render::Entities::Drawable drawable;
        drawable.mesh = source.mesh;
        drawable.material = &m_gameObjectPickingMaterial;
        drawable.stateMask = source.stateMask;
        drawable.AddDescriptor<Engine::Rendering::EngineDrawableDescriptor>({
            source.worldMatrix
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
            auto* meshFilter = actor.GetComponent<Engine::Components::MeshFilter>();
            if (auto* mesh = meshFilter != nullptr ? meshFilter->ResolveMesh() : nullptr)
			{
                const auto* materials = &modelRenderer->ResolveMaterials();
					const auto& modelMatrix = actor.GetTransform()->GetWorldMatrix();

					PreparePickingMaterial(RegisterPickableGameObject(actor), m_gameObjectPickingMaterial);

					auto stateMask = m_gameObjectPickingMaterial.GenerateStateMask();

					// Override the state mask to use the material state mask (if this one is valid)
					if (materials && mesh->GetMaterialIndex() < materials->size())
					{
                        if (auto material = materials->at(mesh->GetMaterialIndex()); material && material->IsValid())
						    stateMask = material->GenerateStateMask();
					}

					NLS::Render::Entities::Drawable drawable;
					drawable.mesh = mesh;
					drawable.material = &m_gameObjectPickingMaterial;
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

void Editor::Rendering::PickingRenderPass::DrawPickableModelSources(
    const std::vector<Engine::Rendering::ScenePickablePrimitiveDrawSource>& sources)
{
    for (const auto& source : sources)
    {
        if (source.owner == nullptr || source.mesh == nullptr || !source.owner->IsActive())
            continue;

        PreparePickingMaterial(RegisterPickableGameObject(*source.owner), m_gameObjectPickingMaterial);

        NLS::Render::Entities::Drawable drawable;
        drawable.mesh = source.mesh;
        drawable.material = &m_gameObjectPickingMaterial;
        drawable.stateMask = source.stateMask;
        drawable.AddDescriptor<Engine::Rendering::EngineDrawableDescriptor>({
            source.worldMatrix
        });

        NLS::Render::Resources::MaterialPipelineStateOverrides unculledOverrides;
        unculledOverrides.culling = false;
        m_renderer.DrawEntity(drawable, unculledOverrides);
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
			PreparePickingMaterial(RegisterPickableGameObject(actor), m_gameObjectPickingMaterial);
			auto& cameraMesh = *EDITOR_CONTEXT(editorResources)->GetMesh("Camera");
            auto translation = Maths::Matrix4::Translation(actor.GetTransform()->GetWorldPosition());
            auto rotation = Maths::Quaternion::ToMatrix4(actor.GetTransform()->GetWorldRotation());
			auto modelMatrix = translation * rotation;

			m_debugModelRenderer.DrawMeshWithSingleMaterial(cameraPickingPso, cameraMesh, m_gameObjectPickingMaterial, modelMatrix);
		}
	}
}

void Editor::Rendering::PickingRenderPass::CapturePickableCameras(
    NLS::Render::Data::PipelineState p_pso,
    Engine::SceneSystem::Scene& p_scene,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    NLS_PROFILE_NAMED_SCOPE("PickingRenderPass::CapturePickableCameras");
    auto cameraPickingPso = Editor::Rendering::CreateEditorUnculledPipelineState(p_pso);
    auto* cameraMesh = EDITOR_CONTEXT(editorResources)->GetMesh("Camera");
    if (cameraMesh == nullptr)
        return;

    for (auto camera : p_scene.GetFastAccessComponents().cameras)
    {
        auto& actor = *camera->gameobject();
        if (!actor.IsActive())
            continue;

        PreparePickingMaterial(RegisterPickableGameObject(actor), m_gameObjectPickingMaterial);
        auto translation = Maths::Matrix4::Translation(actor.GetTransform()->GetWorldPosition());
        auto rotation = Maths::Quaternion::ToMatrix4(actor.GetTransform()->GetWorldRotation());
        auto modelMatrix = translation * rotation;
        m_debugModelRenderer.CaptureMeshDrawCommandsWithSingleMaterial(
            cameraPickingPso,
            *cameraMesh,
            m_gameObjectPickingMaterial,
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
				PreparePickingMaterial(RegisterPickableGameObject(actor), m_lightMaterial);
				auto& lightMesh = *EDITOR_CONTEXT(editorResources)->GetMesh("Vertical_Plane");
				auto modelMatrix = Maths::Matrix4::Translation(actor.GetTransform()->GetWorldPosition());

				m_debugModelRenderer.DrawMeshWithSingleMaterial(lightPickingPso, lightMesh, m_lightMaterial, modelMatrix);
			}
		}
	}
}

void Editor::Rendering::PickingRenderPass::CapturePickableLights(
    NLS::Render::Data::PipelineState p_pso,
    Engine::SceneSystem::Scene& p_scene,
    std::vector<NLS::Render::Context::RecordedDrawCommandInput>& outDrawCommands)
{
    NLS_PROFILE_NAMED_SCOPE("PickingRenderPass::CapturePickableLights");
    const auto lightBillboardScale = Settings::EditorSettings::GetDebugDrawSettingsObject().lightBillboardScale;
    if (lightBillboardScale <= 0.001f)
        return;

    auto lightPickingPso = Editor::Rendering::CreateEditorOverlayPipelineState(p_pso);
    auto* lightMesh = EDITOR_CONTEXT(editorResources)->GetMesh("Vertical_Plane");
    if (lightMesh == nullptr)
        return;
    m_lightMaterial.Set<float>("u_Scale", lightBillboardScale * 0.1f);

    for (auto light : p_scene.GetFastAccessComponents().lights)
    {
        auto& actor = *light->gameobject();
        if (!actor.IsActive())
            continue;

        PreparePickingMaterial(RegisterPickableGameObject(actor), m_lightMaterial);
        auto modelMatrix = Maths::Matrix4::Translation(actor.GetTransform()->GetWorldPosition());
        m_debugModelRenderer.CaptureMeshDrawCommandsWithSingleMaterial(
            lightPickingPso,
            *lightMesh,
            m_lightMaterial,
            modelMatrix,
            outDrawCommands);
    }
}


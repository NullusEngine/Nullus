#include <Components/CameraComponent.h>
#include <Components/LightComponent.h>
#include <Components/TransformComponent.h>
#include <Rendering/Debug/DebugDrawPass.h>
#include <Rendering/Debug/DebugDrawGeometry.h>
#include <Rendering/Debug/DebugDrawService.h>
#include <Rendering/EngineDrawableDescriptor.h>
#include <Rendering/FrameGraph/ExternalResourceBridge.h>
#include <Rendering/FrameGraph/FrameGraphExecutionPlan.h>
#include <Rendering/FrameGraph/SceneRenderGraphBuilder.h>
#include <Profiling/Profiler.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <Rendering/Settings/GraphicsBackendUtils.h>

#include <Debug/Assertion.h>

#include "Rendering/DebugGameObjectSelectionCollector.h"
#include "Rendering/DebugModelRenderer.h"
#include "Rendering/DebugSceneRenderer.h"
#include "Rendering/EditorHelperLifecycle.h"
#include "Rendering/EditorDefaultResources.h"
#include "Rendering/EditorPipelineStatePresets.h"
#include "Rendering/GridRenderPass.h"
#include "Rendering/OutlineRenderer.h"
#include "Rendering/PickingRenderPass.h"
#include "Rendering/SelectionOutlineMaskRenderer.h"
#include "Core/EditorResources.h"
//#include "Panels/AView.h"
//#include "Panels/GameView.h"
#include "Settings/EditorSettings.h"

#include "Core/EditorActions.h"
using namespace NLS;
using namespace Maths;
using namespace NLS::Render::Resources;

const Maths::Vector3 kDebugBoundsColor		= { 1.0f, 0.0f, 0.0f };
const Maths::Vector3 kLightVolumeColor		= { 1.0f, 1.0f, 0.0f };
const Maths::Vector3 kColliderColor			= { 0.0f, 1.0f, 0.0f };
const Maths::Vector3 kFrustumColor			= { 1.0f, 1.0f, 1.0f };

const Maths::Vector4 kDefaultOutlineColor{ 1.0f, 0.7f, 0.0f, 1.0f };
const Maths::Vector4 kSelectedOutlineColor{ 1.0f, 1.0f, 0.0f, 1.0f };

constexpr float kDefaultOutlineWidth = 2.5f;
constexpr float kSelectedOutlineWidth = 5.0f;

namespace
{
    bool ShouldLogEditorHelperDiagnostics(const NLS::Render::Context::Driver& driver)
    {
        return NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(driver).logRenderDrawPath;
    }

    bool IsDeferredThreadedFramePublishSkipped(const NLS::Render::Core::CompositeRenderer& renderer)
    {
        const auto* deferredRenderer =
            dynamic_cast<const NLS::Engine::Rendering::DeferredSceneRenderer*>(&renderer);
        return deferredRenderer != nullptr &&
            deferredRenderer->IsThreadedFramePublishSkippedForCurrentFrame();
    }

    std::shared_ptr<NLS::Render::RHI::RHITextureView> ResolveDeferredSelectionOutlineDepthView(
        const NLS::Render::Core::CompositeRenderer& renderer)
    {
        const auto* deferredRenderer =
            dynamic_cast<const NLS::Engine::Rendering::DeferredSceneRenderer*>(&renderer);
        return deferredRenderer != nullptr
            ? deferredRenderer->GetDeferredPreparedSceneDepthViewForEditorHelpers()
            : nullptr;
    }

	std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata> BuildDebugDeferredThreadedPassMetadata(
		const uint64_t helperVisibleCount,
        const bool includeGridPass,
        const bool includeCameraPass,
        const bool includeLightPass,
        const std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata>& selectionOutlineMetadata,
        const bool includePickingPass)
	{
		std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata> metadata;
		metadata.reserve(
            (includeGridPass ? 1u : 0u) +
            (includeCameraPass ? 1u : 0u) +
            (includeLightPass ? 1u : 0u) +
            selectionOutlineMetadata.size() +
            1u +
            (includePickingPass ? 1u : 0u));

        if (includeGridPass)
        {
            NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata gridMetadata;
            gridMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
            gridMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
            gridMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
            gridMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
            gridMetadata.visibleDrawCountContribution = 1u;
            NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(
                gridMetadata,
                "EditorGridPass");
            metadata.push_back(std::move(gridMetadata));
        }

        if (includeCameraPass)
        {
            NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata cameraMetadata;
            cameraMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
            cameraMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
            cameraMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
            cameraMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
            cameraMetadata.visibleDrawCountContribution = 1u;
            NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(
                cameraMetadata,
                "EditorDebugCamerasPass");
            metadata.push_back(std::move(cameraMetadata));
        }

        if (includeLightPass)
        {
            NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata lightMetadata;
            lightMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
            lightMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
            lightMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
            lightMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
            lightMetadata.visibleDrawCountContribution = 1u;
            NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(
                lightMetadata,
                "EditorDebugLightsPass");
            metadata.push_back(std::move(lightMetadata));
        }

        metadata.insert(
            metadata.end(),
            selectionOutlineMetadata.begin(),
            selectionOutlineMetadata.end());

		NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata helperMetadata;
		helperMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
		helperMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Helper;
		helperMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
		helperMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
		helperMetadata.visibleDrawCountContribution = helperVisibleCount;
		NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(
			helperMetadata,
			"EditorHelperPass");
		metadata.push_back(std::move(helperMetadata));

        if (includePickingPass)
        {
            NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata pickingMetadata;
            pickingMetadata.commandKind = NLS::Render::Context::RenderPassCommandKind::Helper;
            pickingMetadata.role = NLS::Render::FrameGraph::ThreadedRenderScenePassRole::Auxiliary;
            pickingMetadata.executionMode = NLS::Render::FrameGraph::ThreadedRenderScenePassExecutionMode::Recorded;
            pickingMetadata.queueType = NLS::Render::RHI::QueueType::Graphics;
            pickingMetadata.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
            pickingMetadata.visibleDrawCountContribution = 0u;
            pickingMetadata.propagatesColorOutput = false;
            pickingMetadata.propagatesDepthOutput = false;
            NLS::Render::FrameGraph::SetThreadedRenderScenePassGraphPassName(
                pickingMetadata,
                "EditorPickingPass");
            metadata.push_back(std::move(pickingMetadata));
        }

		return metadata;
	}

	std::vector<NLS::Render::Context::RenderPassCommandInput> BuildDebugDeferredAppendedPassInputs(
        std::optional<NLS::Render::Context::RenderPassCommandInput> gridPassInput,
        std::optional<NLS::Render::Context::RenderPassCommandInput> cameraPassInput,
        std::optional<NLS::Render::Context::RenderPassCommandInput> lightPassInput,
        std::vector<NLS::Render::Context::RenderPassCommandInput> selectionOutlinePassInputs,
        std::optional<NLS::Render::Context::RenderPassCommandInput> pickingPassInput)
	{
		std::vector<NLS::Render::Context::RenderPassCommandInput> passInputs;
		passInputs.reserve(
            (gridPassInput.has_value() ? 1u : 0u) +
            (cameraPassInput.has_value() ? 1u : 0u) +
            (lightPassInput.has_value() ? 1u : 0u) +
            selectionOutlinePassInputs.size() +
            (pickingPassInput.has_value() ? 1u : 0u));

        if (gridPassInput.has_value())
            passInputs.push_back(std::move(*gridPassInput));

        if (cameraPassInput.has_value())
            passInputs.push_back(std::move(*cameraPassInput));

        if (lightPassInput.has_value())
            passInputs.push_back(std::move(*lightPassInput));

        for (auto& selectionPassInput : selectionOutlinePassInputs)
            passInputs.push_back(std::move(selectionPassInput));

        if (pickingPassInput.has_value())
            passInputs.push_back(std::move(*pickingPassInput));

		return passInputs;
	}

	bool IsEditorDebugPassEnabled(const char* /*name*/)
	{
		// Debug passes are controlled via EditorSettings, not command-line diagnostics
		// This function is kept for potential future use
		return true;
	}

	bool ShouldDisableEditorGridPass()
	{
		return NLS::Render::Settings::GetThreadDiagnosticsSettings().editorDisableGridPass;
	}

	bool ShouldDisableDebugCamerasPass()
	{
		return NLS::Render::Settings::GetThreadDiagnosticsSettings().editorDisableDebugCamerasPass;
	}

	bool ShouldDisableDebugLightsPass()
	{
		return NLS::Render::Settings::GetThreadDiagnosticsSettings().editorDisableDebugLightsPass;
	}

	bool ShouldDisableDebugGameObjectPass()
	{
		return NLS::Render::Settings::GetThreadDiagnosticsSettings().editorDisableDebugGameObjectPass;
	}

	bool ShouldDisableDebugDrawPass()
	{
		return NLS::Render::Settings::GetThreadDiagnosticsSettings().editorDisableDebugDrawPass;
	}

	bool ShouldDisablePickingPass()
	{
		return NLS::Render::Settings::GetThreadDiagnosticsSettings().editorDisablePickingPass;
	}

    constexpr const char* kLegacyEditorSelectionPassName = "EditorSelectionPass";
}

Maths::Matrix4 CalculateCameraModelMatrix(Engine::GameObject& p_actor)
{
	auto translation = Matrix4::Translation(p_actor.GetTransform()->GetWorldPosition());
    auto rotation = Quaternion::ToMatrix4(p_actor.GetTransform()->GetWorldRotation());
	return translation * rotation;
}

std::optional<std::string> GetLightTypeTextureName(Render::Settings::ELightType type)
{
	using namespace Render::Settings;

	switch (type)
	{
	case ELightType::POINT: return "Bill_Point_Light";
	case ELightType::SPOT: return "Bill_Spot_Light";
	case ELightType::DIRECTIONAL: return "Bill_Directional_Light";
	case ELightType::AMBIENT_BOX: return "Bill_Ambient_Box_Light";
	case ELightType::AMBIENT_SPHERE: return "Bill_Ambient_Sphere_Light";
	}

	return std::nullopt;
}

class DebugCamerasRenderPass : public Render::Core::ARenderPass
{
public:
	DebugCamerasRenderPass(Render::Core::CompositeRenderer& p_renderer)
        : Render::Core::ARenderPass(p_renderer)
        , m_debugModelRenderer(p_renderer)
	{
		m_cameraMaterial.SetShader(EDITOR_CONTEXT(editorResources)->GetShader("DebugLitColor"));
		m_cameraMaterial.Set("u_Diffuse", Vector4(0.0f, 0.3f, 0.7f, 1.0f));
	}

    const std::optional<NLS::Render::Context::RenderPassCommandInput>& GetPreparedThreadedPassInput() const
    {
        return m_preparedThreadedPassInput;
    }

    std::optional<NLS::Render::Context::RenderPassCommandInput> ConsumePreparedThreadedPassInput()
    {
        auto passInput = std::move(m_preparedThreadedPassInput);
        m_preparedThreadedPassInput.reset();
        return passInput;
    }

protected:
    void OnBeginFrame(const NLS::Render::Data::FrameDescriptor&) override
    {
        m_preparedThreadedPassInput.reset();
    }

	virtual void Draw(Render::Data::PipelineState p_pso) override
	{
        const auto& debugSettings = Editor::Settings::EditorSettings::GetDebugDrawSettingsObject();
        if (!debugSettings.debugDrawEnabled || !debugSettings.debugDrawCamera)
            return;

        p_pso = Editor::Rendering::CreateEditorOverlayPipelineState(p_pso);
		auto& sceneDescriptor = m_renderer.GetDescriptor<Engine::Rendering::BaseSceneRenderer::SceneDescriptor>();
        if (NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_renderer.GetDriver()))
        {
            m_preparedThreadedPassInput = BuildThreadedPassInput(sceneDescriptor.scene, p_pso);
            return;
        }

		for (auto camera : sceneDescriptor.scene.GetFastAccessComponents().cameras)
		{
            auto actor = camera->gameobject();

			if (actor->IsActive())
			{
				auto& mesh = *EDITOR_CONTEXT(editorResources)->GetMesh("Camera");
				auto modelMatrix = CalculateCameraModelMatrix(*actor);

				m_debugModelRenderer.DrawMeshWithSingleMaterial(p_pso, mesh, m_cameraMaterial, modelMatrix);
			}
		}
	}

private:
    std::optional<NLS::Render::Context::RenderPassCommandInput> BuildThreadedPassInput(
        Engine::SceneSystem::Scene& scene,
        Render::Data::PipelineState pso)
    {
        auto* cameraMesh = EDITOR_CONTEXT(editorResources)->GetMesh("Camera");
        if (cameraMesh == nullptr)
        {
            if (ShouldLogEditorHelperDiagnostics(m_renderer.GetDriver()))
                NLS_LOG_INFO("[EditorDebugCamerasPass] threaded input skipped: Camera mesh unavailable");
            return std::nullopt;
        }

        const auto& frameDescriptor = m_renderer.GetFrameDescriptor();
        NLS::Render::Context::RenderPassCommandInput passInput;
        passInput.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
        passInput.debugName = "EditorDebugCamerasPass";
        passInput.queueType = NLS::Render::RHI::QueueType::Graphics;
        passInput.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
        passInput.requiresFrameData = true;
        passInput.requiresObjectData = true;
        passInput.targetsSwapchain = NLS::Render::FrameGraph::FrameTargetsSwapchain(frameDescriptor);
        passInput.renderWidth = frameDescriptor.renderWidth;
        passInput.renderHeight = frameDescriptor.renderHeight;
        passInput.usesColorAttachment = true;
        passInput.usesDepthStencilAttachment = true;

        for (auto camera : scene.GetFastAccessComponents().cameras)
        {
            auto* actor = camera != nullptr ? camera->gameobject() : nullptr;
            if (actor == nullptr || !actor->IsActive())
                continue;

            auto modelMatrix = CalculateCameraModelMatrix(*actor);
            m_debugModelRenderer.CaptureMeshDrawCommandsWithSingleMaterial(
                pso,
                *cameraMesh,
                m_cameraMaterial,
                modelMatrix,
                passInput.recordedDrawCommands);
        }

        passInput.drawCount = static_cast<uint64_t>(passInput.recordedDrawCommands.size());
        if (passInput.drawCount == 0u)
        {
            if (ShouldLogEditorHelperDiagnostics(m_renderer.GetDriver()))
            {
                NLS_LOG_INFO(
                    "[EditorDebugCamerasPass] threaded input skipped: captured draw count is zero sceneCameraCount=" +
                    std::to_string(scene.GetFastAccessComponents().cameras.size()));
            }
            return std::nullopt;
        }

        if (ShouldLogEditorHelperDiagnostics(m_renderer.GetDriver()))
        {
            NLS_LOG_INFO(
                "[EditorDebugCamerasPass] threaded input captured drawCount=" +
                std::to_string(passInput.drawCount) +
                " sceneCameraCount=" +
                std::to_string(scene.GetFastAccessComponents().cameras.size()));
        }

        return passInput;
    }

    Editor::Rendering::DebugModelRenderer m_debugModelRenderer;
    NLS::Render::Resources::Material m_cameraMaterial;
    std::optional<NLS::Render::Context::RenderPassCommandInput> m_preparedThreadedPassInput;
};

class DebugLightsRenderPass : public Render::Core::ARenderPass
{
public:
	DebugLightsRenderPass(Render::Core::CompositeRenderer& p_renderer)
        : Render::Core::ARenderPass(p_renderer)
        , m_debugModelRenderer(p_renderer)
	{
		m_lightMaterial.SetShader(EDITOR_CONTEXT(editorResources)->GetShader("Billboard"));
		m_lightMaterial.Set("u_Diffuse", Vector4(1.f, 1.f, 0.5f, 0.5f));
		m_lightMaterial.Set("u_TextureTiling", Vector2(1.0f, 1.0f));
		m_lightMaterial.Set("u_TextureOffset", Vector2(0.0f, 0.0f));
		m_lightMaterial.SetBlendable(true);
	}

    const std::optional<NLS::Render::Context::RenderPassCommandInput>& GetPreparedThreadedPassInput() const
    {
        return m_preparedThreadedPassInput;
    }

    std::optional<NLS::Render::Context::RenderPassCommandInput> ConsumePreparedThreadedPassInput()
    {
        auto passInput = std::move(m_preparedThreadedPassInput);
        m_preparedThreadedPassInput.reset();
        return passInput;
    }

protected:
    void OnBeginFrame(const NLS::Render::Data::FrameDescriptor&) override
    {
        m_preparedThreadedPassInput.reset();
    }

	virtual void Draw(Render::Data::PipelineState p_pso) override
	{
        const auto& debugSettings = Editor::Settings::EditorSettings::GetDebugDrawSettingsObject();
        if (!debugSettings.debugDrawEnabled || !debugSettings.debugDrawLighting)
            return;

		auto& sceneDescriptor = m_renderer.GetDescriptor<Engine::Rendering::BaseSceneRenderer::SceneDescriptor>();
		p_pso = Editor::Rendering::CreateEditorTransparentOverlayPipelineState(p_pso);

		m_lightMaterial.Set<float>("u_Scale", debugSettings.lightBillboardScale * 0.1f);
        if (NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_renderer.GetDriver()))
        {
            m_preparedThreadedPassInput = BuildThreadedPassInput(sceneDescriptor.scene, p_pso);
            return;
        }

		for (auto light : sceneDescriptor.scene.GetFastAccessComponents().lights)
		{
			auto actor = light->gameobject();

			if (actor->IsActive())
			{
				auto& mesh = *EDITOR_CONTEXT(editorResources)->GetMesh("Vertical_Plane");
                auto modelMatrix = Maths::Matrix4::Translation(actor->GetTransform()->GetWorldPosition());

				auto lightTypeTextureName = GetLightTypeTextureName(light->GetData()->type);

				auto lightTexture =
					lightTypeTextureName ?
					EDITOR_CONTEXT(editorResources)->GetTexture(lightTypeTextureName.value()) :
					nullptr;

				const auto& lightColor = light->GetColor();
				m_lightMaterial.Set<Render::Resources::Texture2D*>("u_DiffuseMap", lightTexture);
				m_lightMaterial.Set<Maths::Vector4>("u_Diffuse", Maths::Vector4(lightColor.x, lightColor.y, lightColor.z, 0.75f));

				m_debugModelRenderer.DrawMeshWithSingleMaterial(p_pso, mesh, m_lightMaterial, modelMatrix);
			}
		}
	}

private:
    std::optional<NLS::Render::Context::RenderPassCommandInput> BuildThreadedPassInput(
        Engine::SceneSystem::Scene& scene,
        Render::Data::PipelineState pso)
    {
        auto* lightMesh = EDITOR_CONTEXT(editorResources)->GetMesh("Vertical_Plane");
        if (lightMesh == nullptr)
        {
            if (ShouldLogEditorHelperDiagnostics(m_renderer.GetDriver()))
                NLS_LOG_INFO("[EditorDebugLightsPass] threaded input skipped: Vertical_Plane mesh unavailable");
            return std::nullopt;
        }

        const auto& frameDescriptor = m_renderer.GetFrameDescriptor();
        NLS::Render::Context::RenderPassCommandInput passInput;
        passInput.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
        passInput.debugName = "EditorDebugLightsPass";
        passInput.queueType = NLS::Render::RHI::QueueType::Graphics;
        passInput.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
        passInput.requiresFrameData = true;
        passInput.requiresObjectData = true;
        passInput.targetsSwapchain = NLS::Render::FrameGraph::FrameTargetsSwapchain(frameDescriptor);
        passInput.renderWidth = frameDescriptor.renderWidth;
        passInput.renderHeight = frameDescriptor.renderHeight;
        passInput.usesColorAttachment = true;
        passInput.usesDepthStencilAttachment = true;

        for (auto light : scene.GetFastAccessComponents().lights)
        {
            auto* actor = light != nullptr ? light->gameobject() : nullptr;
            if (actor == nullptr || !actor->IsActive())
                continue;

            auto lightTypeTextureName = GetLightTypeTextureName(light->GetData()->type);
            auto lightTexture =
                lightTypeTextureName ?
                EDITOR_CONTEXT(editorResources)->GetTexture(lightTypeTextureName.value()) :
                nullptr;
            const auto& lightColor = light->GetColor();
            m_lightMaterial.Set<Render::Resources::Texture2D*>("u_DiffuseMap", lightTexture);
            m_lightMaterial.Set<Maths::Vector4>("u_Diffuse", Maths::Vector4(lightColor.x, lightColor.y, lightColor.z, 0.75f));

            auto modelMatrix = Maths::Matrix4::Translation(actor->GetTransform()->GetWorldPosition());
            m_debugModelRenderer.CaptureMeshDrawCommandsWithSingleMaterial(
                pso,
                *lightMesh,
                m_lightMaterial,
                modelMatrix,
                passInput.recordedDrawCommands);
        }

        passInput.drawCount = static_cast<uint64_t>(passInput.recordedDrawCommands.size());
        if (passInput.drawCount == 0u)
        {
            if (ShouldLogEditorHelperDiagnostics(m_renderer.GetDriver()))
            {
                NLS_LOG_INFO(
                    "[EditorDebugLightsPass] threaded input skipped: captured draw count is zero sceneLightCount=" +
                    std::to_string(scene.GetFastAccessComponents().lights.size()));
            }
            return std::nullopt;
        }

        if (ShouldLogEditorHelperDiagnostics(m_renderer.GetDriver()))
        {
            NLS_LOG_INFO(
                "[EditorDebugLightsPass] threaded input captured drawCount=" +
                std::to_string(passInput.drawCount) +
                " sceneLightCount=" +
                std::to_string(scene.GetFastAccessComponents().lights.size()));
        }

        return passInput;
    }

    Editor::Rendering::DebugModelRenderer m_debugModelRenderer;
    NLS::Render::Resources::Material m_lightMaterial;
    std::optional<NLS::Render::Context::RenderPassCommandInput> m_preparedThreadedPassInput;
};

class DebugGameObjectRenderPass : public Render::Core::ARenderPass
{
public:
	DebugGameObjectRenderPass(Render::Core::CompositeRenderer& p_renderer)
        : Render::Core::ARenderPass(p_renderer)
        , m_debugModelRenderer(p_renderer)
        , m_outlineRenderer(p_renderer, m_debugModelRenderer)
        , m_selectionOutlineMaskRenderer(p_renderer)
	{
		
	}

    const std::vector<NLS::Render::Context::RenderPassCommandInput>& GetPreparedThreadedPassInputs() const
    {
        return m_preparedThreadedPassInputs;
    }

    const std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata>& GetPreparedThreadedPassMetadata() const
    {
        return m_preparedThreadedPassMetadata;
    }

    std::vector<NLS::Render::Context::RenderPassCommandInput> ConsumePreparedThreadedPassInputs()
    {
        auto passInputs = std::move(m_preparedThreadedPassInputs);
        m_preparedThreadedPassInputs.clear();
        m_preparedThreadedPassMetadata.clear();
        return passInputs;
    }

    void CommitPendingSelectionOutlineMaskCache(uint64_t publishedFrameId)
    {
        m_selectionOutlineMaskRenderer.CommitPendingCachedMask(publishedFrameId);
    }

    void DiscardPendingSelectionOutlineMaskCache()
    {
        m_selectionOutlineMaskRenderer.DiscardPendingCachedMask();
    }

    bool ManagesOwnRenderPass() const override { return true; }

protected:
    void OnBeginFrame(const NLS::Render::Data::FrameDescriptor&) override
    {
        m_preparedThreadedPassInputs.clear();
        m_preparedThreadedPassMetadata.clear();
    }

	virtual void Draw(Render::Data::PipelineState p_pso) override
	{
		auto& debugSceneDescriptor = m_renderer.GetDescriptor<Editor::Rendering::DebugSceneRenderer::DebugSceneDescriptor>();
        auto* selectedGameObject = debugSceneDescriptor.selectedGameObject;

		if (selectedGameObject == nullptr)
            return;

        const bool usesThreadedRendering =
            NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_renderer.GetDriver());
        if (usesThreadedRendering && IsDeferredThreadedFramePublishSkipped(m_renderer))
        {
            if (ShouldLogEditorHelperDiagnostics(m_renderer.GetDriver()))
            {
                NLS_LOG_INFO(
                    "[DebugSceneRenderer] selected-object debug skipped: deferred threaded frame publish unavailable");
            }
            return;
        }

		if (Editor::Rendering::OutlineRenderer::ShouldIncludeInThreadedFrame(true, selectedGameObject))
		{
            const auto& debugSettings = Editor::Settings::EditorSettings::GetDebugDrawSettingsObject();
            const auto& debugDrawItems = PrepareDebugGameObjectDebugDrawItems(*selectedGameObject, debugSettings);
            ApplyDebugDrawSettings(debugSettings);
            if (ShouldSubmitDebugGameObjectElements(debugSettings))
            {
                NLS_PROFILE_NAMED_SCOPE("DebugGameObject::DrawDebugElements");
                DrawGameObjectDebugElements(debugDrawItems, debugSettings);
            }

            if (usesThreadedRendering)
            {
                auto selectionOutlineOutput = BuildThreadedPassInput(
                    *selectedGameObject,
                    debugSceneDescriptor,
                    p_pso,
                    debugDrawItems);
                m_preparedThreadedPassMetadata = std::move(selectionOutlineOutput.metadata);
                m_preparedThreadedPassInputs = std::move(selectionOutlineOutput.passInputs);
                return;
            }

            if (!m_outlineRenderer.PrepareOutlineDrawItems(debugDrawItems))
                return;

            const auto& frameDescriptor = m_renderer.GetFrameDescriptor();
            const bool startedRenderPass = m_renderer.BeginOutputRenderPass(
                frameDescriptor.renderWidth,
                frameDescriptor.renderHeight,
                false,
                false,
                false);
            if (!startedRenderPass)
                return;

            m_outlineRenderer.DrawPreparedOutline(kSelectedOutlineColor, kSelectedOutlineWidth);
            m_renderer.EndOutputRenderPass(startedRenderPass);
		}
	}

    static bool ShouldSubmitDebugGameObjectElements(
        const Editor::Settings::EditorDebugDrawSettingsObject& debugSettings)
    {
        return debugSettings.debugDrawEnabled &&
            (debugSettings.debugDrawBounds ||
                debugSettings.debugDrawCamera ||
                debugSettings.debugDrawLighting);
    }

	void DrawGameObjectDebugElements(
        const Editor::Rendering::DebugGameObjectDebugDrawItems& debugDrawItems,
        const Editor::Settings::EditorDebugDrawSettingsObject& debugSettings)
	{
        /* Render static mesh outline and bounding spheres */
        if (debugSettings.debugDrawBounds)
        {
            for (const auto& item : debugDrawItems.selectionMeshItems)
                DrawBoundingSpheres(item);
        }

        /* Render camera component outline */
        if (debugSettings.debugDrawCamera)
        {
            for (const auto& cameraItem : debugDrawItems.cameras)
            {
                auto* cameraComponent = cameraItem.cameraComponent;
                if (cameraComponent != nullptr)
                    DrawCameraFrustum(*cameraComponent);
            }
        }

// 			/* Render the actor collider */
// 			if (p_actor.GetComponent<Engine::Components::CPhysicalObject>())
// 			{
// 				DrawGameObjectCollider(p_actor);
// 			}

        /* Render the actor ambient light */
        if (debugSettings.debugDrawLighting)
        {
            for (auto* light : debugDrawItems.lights)
            {
                if (light != nullptr)
                    DrawLightVolume(*light);
            }
        }
	}

	void DrawFrustumLines(
		const Maths::Vector3& pos,
		const Maths::Vector3& forward,
		const float nearPlane,
		const float farPlane,
		const Maths::Vector3& a,
		const Maths::Vector3& b,
		const Maths::Vector3& c,
		const Maths::Vector3& d,
		const Maths::Vector3& e,
		const Maths::Vector3& f,
		const Maths::Vector3& g,
		const Maths::Vector3& h
	)
	{
		Render::Debug::DebugDrawSubmitOptions options;
		options.category = Render::Debug::DebugDrawCategory::Camera;
		options.style.color = kFrustumColor;
		options.style.lineWidth = 1.0f;
		options.style.depthMode = Render::Debug::DebugDrawDepthMode::AlwaysOnTop;

		const std::array<Vector3, 8u> corners = {
			pos + forward * nearPlane + a,
			pos + forward * nearPlane + b,
			pos + forward * nearPlane + c,
			pos + forward * nearPlane + d,
			pos + forward * farPlane + e,
			pos + forward * farPlane + f,
			pos + forward * farPlane + g,
			pos + forward * farPlane + h
		};

		Render::Debug::SubmitFrustum(GetDebugDrawService(), corners, options);
	}

	void DrawCameraPerspectiveFrustum(std::pair<uint16_t, uint16_t>& p_size, Engine::Components::CameraComponent& p_camera)
	{
		const auto& owner = *p_camera.gameobject();
		auto& camera = *p_camera.GetCamera();

		const auto& cameraPos = owner.GetTransform()->GetWorldPosition();
        const auto& cameraRotation = owner.GetTransform()->GetWorldRotation();
        const auto& cameraForward = owner.GetTransform()->GetWorldForward();

		camera.CacheMatrices(p_size.first, p_size.second); // TODO: We shouldn't cache matrices mid air, we could use another function to get the matrices/calculate
		const auto proj = Matrix4::Transpose(camera.GetProjectionMatrix());
		const auto nearPlane = camera.GetNear();
		const auto farPlane = camera.GetFar();

		const auto nLeft = nearPlane * (proj.data[2] - 1.0f) / proj.data[0];
		const auto nRight = nearPlane * (1.0f + proj.data[2]) / proj.data[0];
		const auto nTop = nearPlane * (1.0f + proj.data[6]) / proj.data[5];
		const auto nBottom = nearPlane * (proj.data[6] - 1.0f) / proj.data[5];

		const auto fLeft = farPlane * (proj.data[2] - 1.0f) / proj.data[0];
		const auto fRight = farPlane * (1.0f + proj.data[2]) / proj.data[0];
		const auto fTop = farPlane * (1.0f + proj.data[6]) / proj.data[5];
		const auto fBottom = farPlane * (proj.data[6] - 1.0f) / proj.data[5];

		auto a = cameraRotation * Vector3{ nLeft, nTop, 0 };
		auto b = cameraRotation * Vector3{ nRight, nTop, 0 };
		auto c = cameraRotation * Vector3{ nLeft, nBottom, 0 };
		auto d = cameraRotation * Vector3{ nRight, nBottom, 0 };
		auto e = cameraRotation * Vector3{ fLeft, fTop, 0 };
		auto f = cameraRotation * Vector3{ fRight, fTop, 0 };
		auto g = cameraRotation * Vector3{ fLeft, fBottom, 0 };
		auto h = cameraRotation * Vector3{ fRight, fBottom, 0 };

		DrawFrustumLines(cameraPos, cameraForward, nearPlane, farPlane, a, b, c, d, e, f, g, h);
	}

	void DrawCameraOrthographicFrustum(std::pair<uint16_t, uint16_t>& p_size, Engine::Components::CameraComponent& p_camera)
	{
		auto& owner = *p_camera.gameobject();
		auto& camera = *p_camera.GetCamera();
		const auto ratio = p_size.first / static_cast<float>(p_size.second);

		const auto& cameraPos = owner.GetTransform()->GetWorldPosition();
        const auto& cameraRotation = owner.GetTransform()->GetWorldRotation();
        const auto& cameraForward = owner.GetTransform()->GetWorldForward();

		const auto nearPlane = camera.GetNear();
		const auto farPlane = camera.GetFar();
		const auto size = p_camera.GetSize();

		const auto right = ratio * size;
		const auto left = -right;
		const auto top = size;
		const auto bottom = -top;

		const auto a = cameraRotation * Vector3{ left, top, 0 };
		const auto b = cameraRotation * Vector3{ right, top, 0 };
		const auto c = cameraRotation * Vector3{ left, bottom, 0 };
		const auto d = cameraRotation * Vector3{ right, bottom, 0 };

		DrawFrustumLines(cameraPos, cameraForward, nearPlane, farPlane, a, b, c, d, a, b, c, d);
	}

	void DrawCameraFrustum(Engine::Components::CameraComponent& p_camera)
	{
		const auto& frameDescriptor = m_renderer.GetFrameDescriptor();
		std::pair<uint16_t, uint16_t> viewSize = {
			frameDescriptor.renderWidth != 0 ? frameDescriptor.renderWidth : 16,
			frameDescriptor.renderHeight != 0 ? frameDescriptor.renderHeight : 9
		};

		switch (p_camera.GetProjectionMode())
		{
		case Render::Settings::EProjectionMode::ORTHOGRAPHIC:
			DrawCameraOrthographicFrustum(viewSize, p_camera);
			break;

		case Render::Settings::EProjectionMode::PERSPECTIVE:
			DrawCameraPerspectiveFrustum(viewSize, p_camera);
			break;
		}
	}

	void DrawGameObjectCollider(Engine::GameObject& p_actor)
	{
// 		using namespace Engine::Components;
// 		using namespace OvPhysics::Entities;
// 
// 		auto pso = m_renderer.CreatePipelineState();
// 		pso.depthTest = false;
// 
// 		/* Draw the box collider if any */
// 		if (auto boxColliderComponent = p_actor.GetComponent<Engine::Components::CPhysicalBox>(); boxColliderComponent)
// 		{
// 			SubmitBox(
// 				pso,
// 				p_actor.transform.GetWorldPosition(),
// 				p_actor.transform.GetWorldRotation(),
// 				boxColliderComponent->GetSize() * p_actor.transform.GetWorldScale(),
// 				Maths::Vector3{ 0.f, 1.f, 0.f },
// 				1.0f
// 			);
// 		}
// 
// 		/* Draw the sphere collider if any */
// 		if (auto sphereColliderComponent = p_actor.GetComponent<Engine::Components::CPhysicalSphere>(); sphereColliderComponent)
// 		{
// 			Vector3 actorScale = p_actor.transform.GetWorldScale();
// 			float radius = sphereColliderComponent->GetRadius() * std::max(std::max(std::max(actorScale.x, actorScale.y), actorScale.z), 0.0f);
// 
// 			SubmitSphere(
// 				pso,
// 				p_actor.transform.GetWorldPosition(),
// 				p_actor.transform.GetWorldRotation(),
// 				radius,
// 				Maths::Vector3{ 0.f, 1.f, 0.f },
// 				1.0f
// 			);
// 		}
// 
// 		/* Draw the capsule collider if any */
// 		if (auto capsuleColliderComponent = p_actor.GetComponent<Engine::Components::CPhysicalCapsule>(); capsuleColliderComponent)
// 		{
// 			Vector3 actorScale = p_actor.transform.GetWorldScale();
// 			float radius = abs(capsuleColliderComponent->GetRadius() * std::max(std::max(actorScale.x, actorScale.z), 0.f));
// 			float height = abs(capsuleColliderComponent->GetHeight() * actorScale.y);
// 
// 			SubmitCapsule(
// 				pso,
// 				p_actor.transform.GetWorldPosition(),
// 				p_actor.transform.GetWorldRotation(),
// 				radius,
// 				height,
// 				Maths::Vector3{ 0.f, 1.f, 0.f },
// 				1.0f
// 			);
// 		}
	}

	void DrawLightVolume(Engine::Components::LightComponent& p_light)
	{
		Render::Debug::DebugDrawSubmitOptions options;
		options.category = Render::Debug::DebugDrawCategory::Lighting;
		options.style.color = kLightVolumeColor;
		options.style.lineWidth = 1.0f;
		options.style.depthMode = Render::Debug::DebugDrawDepthMode::AlwaysOnTop;

		Render::Debug::SubmitLightVolume(GetDebugDrawService(), *p_light.GetData(), options);
	}

	void DrawBoundingSpheres(const Editor::Rendering::DebugGameObjectDebugDrawItems::SelectionMeshItem& item)
	{
        if (item.meshRenderer == nullptr || item.mesh == nullptr)
            return;

		const auto& modelBoundingsphere =
			item.meshRenderer->GetFrustumBehaviour() == Engine::Components::MeshRenderer::EFrustumBehaviour::CULL_CUSTOM ?
			item.meshRenderer->GetCustomBoundingSphere() :
			item.mesh->GetBoundingSphere();

		float radiusScale = std::max(std::max(std::max(item.worldScale.x, item.worldScale.y), item.worldScale.z), 0.0f);
		float scaledRadius = modelBoundingsphere.radius * radiusScale;
		auto sphereOffset = Maths::Quaternion::RotatePoint(modelBoundingsphere.position, item.worldRotation) * radiusScale;

		SubmitSphere(
			item.worldPosition + sphereOffset,
			item.worldRotation,
			scaledRadius,
			kDebugBoundsColor,
			1.0f,
			Render::Debug::DebugDrawCategory::Bounds
		);
	}

private:
    const Editor::Rendering::DebugGameObjectDebugDrawItems& PrepareDebugGameObjectDebugDrawItems(
        Engine::GameObject& selectedGameObject,
        const Editor::Settings::EditorDebugDrawSettingsObject& debugSettings)
    {
        NLS_PROFILE_NAMED_SCOPE("DebugGameObject::CollectSelectedItems");
        CollectSelectedDebugGameObjectDebugDrawItems(
            selectedGameObject,
            m_debugDrawScratchItems,
            true,
            debugSettings.debugDrawLighting,
            {
                NLS::Engine::LayerMask(0xFFFFFFFFu),
                m_renderer.GetFrameDescriptor().camera != nullptr
                    ? m_renderer.GetFrameDescriptor().camera->GetGeometryFrustum()
                    : nullptr,
                true
            });
        return m_debugDrawScratchItems;
    }

	void ApplyDebugDrawSettings(const Editor::Settings::EditorDebugDrawSettingsObject& debugSettings)
	{
		auto& debugDrawService = GetDebugDrawService();
		debugDrawService.SetEnabled(debugSettings.debugDrawEnabled);
		debugDrawService.SetCategoryEnabled(Render::Debug::DebugDrawCategory::Grid, debugSettings.debugDrawGrid);
		debugDrawService.SetCategoryEnabled(Render::Debug::DebugDrawCategory::Bounds, debugSettings.debugDrawBounds);
		debugDrawService.SetCategoryEnabled(Render::Debug::DebugDrawCategory::Camera, debugSettings.debugDrawCamera);
		debugDrawService.SetCategoryEnabled(Render::Debug::DebugDrawCategory::Lighting, debugSettings.debugDrawLighting);
	}

    Render::Debug::DebugDrawService& GetDebugDrawService()
    {
        auto* debugDrawService = m_renderer.GetDebugDrawService();
        NLS_ASSERT(debugDrawService != nullptr, "Cannot find DebugDrawService attached to this renderer");
        return *debugDrawService;
    }

    void SubmitLine(
        const Maths::Vector3& start,
        const Maths::Vector3& end,
        const Maths::Vector3& color,
        const float lineWidth = 1.0f,
		const Render::Debug::DebugDrawCategory category = Render::Debug::DebugDrawCategory::General)
    {
		Render::Debug::DebugDrawSubmitOptions options;
		options.category = category;
		options.style.color = color;
		options.style.lineWidth = lineWidth;
        GetDebugDrawService().SubmitLine(start, end, options);
    }

    void SubmitBox(
        const Maths::Vector3& position,
        const Maths::Quaternion& rotation,
        const Maths::Vector3& size,
        const Maths::Vector3& color,
        const float lineWidth = 1.0f,
		const Render::Debug::DebugDrawCategory category = Render::Debug::DebugDrawCategory::General)
    {
		Render::Debug::DebugDrawSubmitOptions options;
		options.category = category;
		options.style.color = color;
		options.style.lineWidth = lineWidth;
        Render::Debug::SubmitBox(GetDebugDrawService(), position, rotation, size, options);
    }

    void SubmitSphere(
        const Maths::Vector3& position,
        const Maths::Quaternion& rotation,
        const float radius,
        const Maths::Vector3& color,
        const float lineWidth = 1.0f,
		const Render::Debug::DebugDrawCategory category = Render::Debug::DebugDrawCategory::General)
    {
		Render::Debug::DebugDrawSubmitOptions options;
		options.category = category;
		options.style.color = color;
		options.style.lineWidth = lineWidth;
        Render::Debug::SubmitSphere(GetDebugDrawService(), position, rotation, radius, options);
    }

    void SubmitCapsule(
        const Maths::Vector3& position,
        const Maths::Quaternion& rotation,
        const float radius,
        const float height,
        const Maths::Vector3& color,
        const float lineWidth = 1.0f,
		const Render::Debug::DebugDrawCategory category = Render::Debug::DebugDrawCategory::General)
    {
		Render::Debug::DebugDrawSubmitOptions options;
		options.category = category;
		options.style.color = color;
		options.style.lineWidth = lineWidth;
        Render::Debug::SubmitCapsule(GetDebugDrawService(), position, rotation, radius, height, options);
    }

    Editor::Rendering::SelectionOutlinePreparedOutput BuildThreadedPassInput(
        Engine::GameObject& selectedGameObject,
        const Editor::Rendering::DebugSceneRenderer::DebugSceneDescriptor& debugSceneDescriptor,
        NLS::Render::Data::PipelineState p_pso,
        const Editor::Rendering::DebugGameObjectDebugDrawItems& debugDrawItems)
    {
        NLS_PROFILE_NAMED_SCOPE("DebugGameObject::BuildThreadedPassInput");

        if (IsDeferredThreadedFramePublishSkipped(m_renderer))
            return {};

        auto* frameObjectBindingProvider = m_renderer.GetFrameObjectBindingProvider();
        const bool hadPreparedFrameReservationBeforeSelection =
            frameObjectBindingProvider != nullptr &&
            frameObjectBindingProvider->HasReservedPreparedFrameResources();
        const auto releaseSelectionOwnedPreparedFrameReservation =
            [frameObjectBindingProvider, hadPreparedFrameReservationBeforeSelection]()
        {
            if (frameObjectBindingProvider != nullptr &&
                !hadPreparedFrameReservationBeforeSelection &&
                frameObjectBindingProvider->HasReservedPreparedFrameResources())
            {
                frameObjectBindingProvider->ReleaseReservedPreparedFrameResources();
            }
        };

        const auto selectionOutlineDepthView = ResolveDeferredSelectionOutlineDepthView(m_renderer);
        if (selectionOutlineDepthView == nullptr)
        {
            releaseSelectionOwnedPreparedFrameReservation();

            Editor::Rendering::SelectionOutlinePreparedOutput missingDepthOutput;
            missingDepthOutput.selectedItemCount =
                static_cast<uint64_t>(debugDrawItems.selectionMeshItems.size() + debugDrawItems.cameras.size());
            if (missingDepthOutput.selectedItemCount > 0u)
            {
                missingDepthOutput.fallbackDecision = {
                    Editor::Rendering::SelectionOutlineFallbackReason::MissingSceneDepth,
                    missingDepthOutput.selectedItemCount
                };
            }
            return missingDepthOutput;
        }

        auto maskOutput = m_selectionOutlineMaskRenderer.BuildPreparedOutput(
            debugDrawItems,
            kDefaultOutlineColor,
            kSelectedOutlineColor,
            p_pso,
            selectionOutlineDepthView);
        if (maskOutput.HasScreenSpacePasses())
            return maskOutput;

        if (!maskOutput.fallbackDecision.has_value())
        {
            releaseSelectionOwnedPreparedFrameReservation();
            return maskOutput;
        }

        if (ShouldLogEditorHelperDiagnostics(m_renderer.GetDriver()))
        {
            NLS_LOG_INFO(
                "[DebugSceneRenderer] Selection outline screen-space fallback reason=" +
                    std::string(Editor::Rendering::SelectionOutlineFallbackReasonToString(maskOutput.fallbackDecision->reason)) +
                " selectedItems=" +
                std::to_string(maskOutput.fallbackDecision->selectedItemCount));
        }
        if (Editor::Rendering::ResolveSelectionOutlineFallbackAction(maskOutput.fallbackDecision->reason) !=
            Editor::Rendering::SelectionOutlineFallbackAction::LegacyShell)
        {
            releaseSelectionOwnedPreparedFrameReservation();
            return maskOutput;
        }
        if (!Editor::Rendering::SelectionOutlineLegacyShellFallbackIsAttachmentCompatible(
                m_renderer.GetFrameDescriptor(),
                selectionOutlineDepthView))
        {
            if (ShouldLogEditorHelperDiagnostics(m_renderer.GetDriver()))
            {
                NLS_LOG_INFO(
                    "[DebugSceneRenderer] Selection outline legacy fallback skipped because current color/depth attachments are not extent/sample-count compatible.");
            }
            releaseSelectionOwnedPreparedFrameReservation();
            return maskOutput;
        }

        NLS::Render::Context::RenderPassCommandInput passInput;
        const auto& frameDescriptor = m_renderer.GetFrameDescriptor();
        passInput.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
        passInput.debugName = kLegacyEditorSelectionPassName;
        passInput.queueType = NLS::Render::RHI::QueueType::Graphics;
        passInput.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
        passInput.requiresFrameData = true;
        passInput.requiresObjectData = true;
        passInput.targetsSwapchain = NLS::Render::FrameGraph::FrameTargetsSwapchain(frameDescriptor);
        passInput.renderWidth = frameDescriptor.renderWidth;
        passInput.renderHeight = frameDescriptor.renderHeight;
        passInput.usesColorAttachment = true;
        passInput.usesDepthStencilAttachment = true;
        passInput.writesDepthStencilAttachment = true;

        m_outlineRenderer.CaptureOutlineDrawCommands(
            debugDrawItems,
            kSelectedOutlineColor,
            kSelectedOutlineWidth,
            passInput.recordedDrawCommands);

        passInput.drawCount = static_cast<uint64_t>(passInput.recordedDrawCommands.size());
        if (passInput.drawCount == 0u)
        {
            releaseSelectionOwnedPreparedFrameReservation();
            return maskOutput;
        }

        maskOutput.passInputs.push_back(std::move(passInput));
        maskOutput.metadata.clear();
        maskOutput.metadata.push_back(
            Editor::Rendering::BuildSelectionOutlineLegacyShellMetadata(maskOutput.passInputs.back()));
        return maskOutput;
    }

private:
    Editor::Rendering::DebugModelRenderer m_debugModelRenderer;
    Editor::Rendering::OutlineRenderer m_outlineRenderer;
    Editor::Rendering::SelectionOutlineMaskRenderer m_selectionOutlineMaskRenderer;
    Editor::Rendering::DebugGameObjectDebugDrawItems m_debugDrawScratchItems;
    std::vector<NLS::Render::Context::RenderPassCommandInput> m_preparedThreadedPassInputs;
    std::vector<NLS::Render::FrameGraph::ThreadedRenderScenePassMetadata> m_preparedThreadedPassMetadata;
};

Editor::Rendering::DebugSceneRenderer::DebugSceneRenderer(NLS::Render::Context::Driver& p_driver) :
	Engine::Rendering::DeferredSceneRenderer(p_driver)
{
    SetDebugDrawService(std::make_unique<NLS::Render::Debug::DebugDrawService>());

    auto& gridPass = AddPass<GridRenderPass>("Grid", NLS::Render::Settings::ERenderPassOrder::Transparent + 1);
    gridPass.SetEnabled(!ShouldDisableEditorGridPass());

    auto& debugCamerasPass = AddPass<DebugCamerasRenderPass>("Debug Cameras", NLS::Render::Settings::ERenderPassOrder::Transparent + 1);
    debugCamerasPass.SetEnabled(!ShouldDisableDebugCamerasPass());

    auto& debugLightsPass = AddPass<DebugLightsRenderPass>("Debug Lights", NLS::Render::Settings::ERenderPassOrder::Transparent + 2);
    debugLightsPass.SetEnabled(!ShouldDisableDebugLightsPass());

    auto& debugGameObjectPass = AddPass<DebugGameObjectRenderPass>("Debug GameObject", NLS::Render::Settings::ERenderPassOrder::Transparent + 3);
    debugGameObjectPass.SetEnabled(!ShouldDisableDebugGameObjectPass());
    auto& debugDrawPass = AddPass<NLS::Render::Debug::DebugDrawPass>("Debug Draw", NLS::Render::Settings::ERenderPassOrder::Transparent + 4);
    debugDrawPass.SetEnabled(!ShouldDisableDebugDrawPass());
    auto& pickingPass = AddPass<PickingRenderPass>("Picking", NLS::Render::Settings::ERenderPassOrder::PostProcessing + 1);
    pickingPass.SetEnabled(!ShouldDisablePickingPass());
}

void Editor::Rendering::DebugSceneRenderer::OnThreadedFramePublished(const uint64_t publishedFrameId)
{
    Engine::Rendering::DeferredSceneRenderer::OnThreadedFramePublished(publishedFrameId);
    GetPass<DebugGameObjectRenderPass>("Debug GameObject").CommitPendingSelectionOutlineMaskCache(publishedFrameId);
}

void Editor::Rendering::DebugSceneRenderer::OnThreadedFramePublishFailed()
{
    GetPass<DebugGameObjectRenderPass>("Debug GameObject").DiscardPendingSelectionOutlineMaskCache();
    Engine::Rendering::DeferredSceneRenderer::OnThreadedFramePublishFailed();
}

std::optional<NLS::Render::Context::FrameSnapshot> Editor::Rendering::DebugSceneRenderer::BuildFrameSnapshot(
    const NLS::Render::Data::FrameDescriptor& frameDescriptor) const
{
    auto snapshot = Engine::Rendering::DeferredSceneRenderer::BuildFrameSnapshot(frameDescriptor);
    if (!snapshot.has_value() || !HasDescriptor<Engine::Rendering::BaseSceneRenderer::SceneDescriptor>())
        return snapshot;

    const auto& scene = GetDescriptor<Engine::Rendering::BaseSceneRenderer::SceneDescriptor>().scene;
    const bool gridPassEnabled = !ShouldDisableEditorGridPass();
    const bool cameraPassEnabled = !ShouldDisableDebugCamerasPass();
    const bool lightPassEnabled = !ShouldDisableDebugLightsPass();
    const bool gameObjectPassEnabled = !ShouldDisableDebugGameObjectPass();
    const bool debugDrawPassEnabled = !ShouldDisableDebugDrawPass();

    ThreadedEditorHelperState helperState;
    helperState.gridPassEnabled = gridPassEnabled;
    helperState.cameraPassEnabled = cameraPassEnabled;
    helperState.lightPassEnabled = lightPassEnabled;
    helperState.gameObjectPassEnabled = gameObjectPassEnabled;
    helperState.debugDrawPassEnabled = debugDrawPassEnabled;
    const auto& debugSettings = Editor::Settings::EditorSettings::GetDebugDrawSettingsObject();
    helperState.debugDrawEnabled = debugSettings.debugDrawEnabled;
    helperState.debugDrawCamera = debugSettings.debugDrawCamera;
    helperState.debugDrawLighting = debugSettings.debugDrawLighting;
    helperState.gridEnabled = GridRenderPass::ShouldIncludeInThreadedFrame(
        gridPassEnabled,
        HasDescriptor<GridRenderPass::GridDescriptor>(),
        debugSettings.debugDrawEnabled,
        debugSettings.debugDrawGrid);
    helperState.sceneCameraCount = static_cast<uint64_t>(scene.GetFastAccessComponents().cameras.size());
    helperState.sceneLightCount = static_cast<uint64_t>(scene.GetFastAccessComponents().lights.size());
    if (HasDescriptor<DebugSceneDescriptor>())
    {
        const auto* selectedGameObject = GetDescriptor<DebugSceneDescriptor>().selectedGameObject;
        helperState.hasSelectedGameObject =
            !IsThreadedFramePublishSkippedForCurrentFrame() &&
            OutlineRenderer::ShouldIncludeInThreadedFrame(gameObjectPassEnabled, selectedGameObject);
    }

    if (const auto* debugDrawService = GetDebugDrawService(); debugDrawService != nullptr)
        helperState.hasVisibleDebugDrawPrimitives = !debugDrawService->CollectVisiblePrimitives().empty();

    snapshot->visibleHelperDrawCount = CountThreadedEditorHelperPasses(helperState);
    return snapshot;
}

NLS::Render::Context::PreparedRenderSceneBuilder Editor::Rendering::DebugSceneRenderer::BuildPreparedRenderSceneBuilder(
    const NLS::Render::Context::FrameSnapshot& snapshot) const
{
    const auto& gridPassInput = GetPass<GridRenderPass>("Grid").GetPreparedThreadedPassInput();
    const auto& cameraPassInput = GetPass<DebugCamerasRenderPass>("Debug Cameras").GetPreparedThreadedPassInput();
    const auto& lightPassInput = GetPass<DebugLightsRenderPass>("Debug Lights").GetPreparedThreadedPassInput();
    const auto& selectionOutlinePassInputs = GetPass<DebugGameObjectRenderPass>("Debug GameObject").GetPreparedThreadedPassInputs();
    const auto& selectionOutlineMetadata = GetPass<DebugGameObjectRenderPass>("Debug GameObject").GetPreparedThreadedPassMetadata();
    const auto& pickingPassInput = GetPass<PickingRenderPass>("Picking").GetPreparedThreadedPassInput();
    if (ShouldLogEditorHelperDiagnostics(m_driver))
    {
        uint64_t selectionOutlineDrawCount = 0u;
        for (const auto& input : selectionOutlinePassInputs)
            selectionOutlineDrawCount += input.drawCount;

        NLS_LOG_INFO(
            "[DebugSceneRenderer] prepared helper inputs grid=" +
            std::to_string(gridPassInput.has_value() ? gridPassInput->drawCount : 0u) +
            " cameras=" +
            std::to_string(cameraPassInput.has_value() ? cameraPassInput->drawCount : 0u) +
            " lights=" +
            std::to_string(lightPassInput.has_value() ? lightPassInput->drawCount : 0u) +
            " selection=" +
            std::to_string(selectionOutlineDrawCount) +
            " picking=" +
            std::to_string(pickingPassInput.has_value() ? pickingPassInput->drawCount : 0u) +
            " snapshotVisibleHelpers=" +
            std::to_string(snapshot.visibleHelperDrawCount));
    }
    const auto explicitHelperContribution =
        static_cast<uint64_t>(gridPassInput.has_value() ? 1u : 0u) +
        static_cast<uint64_t>(cameraPassInput.has_value() ? 1u : 0u) +
        static_cast<uint64_t>(lightPassInput.has_value() ? 1u : 0u) +
        static_cast<uint64_t>(selectionOutlineMetadata.size());
    const auto aggregateHelperVisibleCount =
        snapshot.visibleHelperDrawCount >= explicitHelperContribution
            ? snapshot.visibleHelperDrawCount - explicitHelperContribution
            : 0u;
    auto metadata = BuildDebugDeferredThreadedPassMetadata(
        aggregateHelperVisibleCount,
        gridPassInput.has_value(),
        cameraPassInput.has_value(),
        lightPassInput.has_value(),
        selectionOutlineMetadata,
        pickingPassInput.has_value());

    auto consumedGridPassInput = GetPass<GridRenderPass>("Grid").ConsumePreparedThreadedPassInput();
    auto consumedCameraPassInput = GetPass<DebugCamerasRenderPass>("Debug Cameras").ConsumePreparedThreadedPassInput();
    auto consumedLightPassInput = GetPass<DebugLightsRenderPass>("Debug Lights").ConsumePreparedThreadedPassInput();
    auto consumedSelectionOutlinePassInputs =
        GetPass<DebugGameObjectRenderPass>("Debug GameObject").ConsumePreparedThreadedPassInputs();
    auto consumedPickingPassInput = GetPass<PickingRenderPass>("Picking").ConsumePreparedThreadedPassInput();

    std::shared_ptr<NLS::Render::RHI::RHITexture> preferredReadbackTexture;
    uint64_t additionalRenderTargetUseCount = 0u;
    if (consumedPickingPassInput.has_value())
    {
        preferredReadbackTexture =
            !consumedPickingPassInput->colorAttachmentViews.empty()
                ? consumedPickingPassInput->colorAttachmentViews.front()->GetTexture()
                : nullptr;
        additionalRenderTargetUseCount = 2u;
    }
    auto appendedPassInputs = BuildDebugDeferredAppendedPassInputs(
        std::move(consumedGridPassInput),
        std::move(consumedCameraPassInput),
        std::move(consumedLightPassInput),
        std::move(consumedSelectionOutlinePassInputs),
        std::move(consumedPickingPassInput));

    const bool hasSkyboxTexture =
        HasDescriptor<DeferredSceneDescriptor>() &&
        GetDescriptor<DeferredSceneDescriptor>().hasSkyboxTexture;

    return BuildDeferredPreparedRenderSceneBuilder(
        snapshot,
        hasSkyboxTexture,
        std::move(appendedPassInputs),
        std::move(metadata),
        std::move(preferredReadbackTexture),
        additionalRenderTargetUseCount);
}

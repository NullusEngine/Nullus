#include <Rendering/Debug/DebugDrawService.h>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/FrameGraph/ExternalResourceBridge.h>
#include <Rendering/Settings/GraphicsBackendUtils.h>
#include <cstdlib>
#include <cstring>

#include <Debug/Assertion.h>

#include "Core/EditorResources.h"
#include "Core/EditorActions.h"
#include "Rendering/EditorHelperLifecycle.h"
#include "Rendering/EditorPipelineStatePresets.h"
#include "Rendering/GridRenderPass.h"
#include "Settings/EditorSettings.h"
using namespace NLS;

namespace
{
    constexpr uint64_t kEditorGridAxesPersistentGroupId = 0x4E4C534752494441ull;

	bool ShouldSkipEditorGridPlane()
	{
		return NLS::Render::Settings::GetThreadDiagnosticsSettings().editorGridSkipPlane;
	}

	bool ShouldSkipEditorGridAxes()
	{
		return NLS::Render::Settings::GetThreadDiagnosticsSettings().editorGridSkipAxes;
	}

    Maths::Matrix4 BuildGridPlaneModelMatrix(
        const NLS::Editor::Rendering::GridRenderPass::GridDescriptor& gridDescriptor,
        const float gridSize)
    {
        return Maths::Matrix4::Translation({ gridDescriptor.viewPosition.x, 0.0f, gridDescriptor.viewPosition.z }) *
            Maths::Matrix4::Scaling({ gridSize * 2.0f, 1.f, gridSize * 2.0f });
    }
}

void Editor::Rendering::GridRenderPass::UpdateGridAxes(
    NLS::Render::Debug::DebugDrawService& debugDrawService,
    const GridDescriptor& gridDescriptor,
    const float gridSize)
{
    if (ShouldSkipEditorGridAxes() || NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_renderer.GetDriver()))
    {
        if (m_hasCachedGridAxes)
            debugDrawService.RemovePersistentPrimitiveGroup(kEditorGridAxesPersistentGroupId);
        m_hasCachedGridAxes = false;
        return;
    }

    if (m_hasCachedGridAxes && m_cachedGridAxesPosition == gridDescriptor.viewPosition)
        return;

    const auto axes = BuildEditorOriginAxes(gridDescriptor.viewPosition,gridSize);
    std::vector<NLS::Render::Debug::DebugDrawPrimitive> primitives(axes.begin(),axes.end());

    if (debugDrawService.SetPersistentPrimitiveGroup(kEditorGridAxesPersistentGroupId, std::move(primitives)))
    {
        m_cachedGridAxesPosition = gridDescriptor.viewPosition;
        m_hasCachedGridAxes = true;
    }
}

Editor::Rendering::GridRenderPass::GridRenderPass(NLS::Render::Core::CompositeRenderer& p_renderer) :
	NLS::Render::Core::ARenderPass(p_renderer),
	m_debugModelRenderer(p_renderer)
{
	/* Grid Material */
	m_gridMaterial.SetShader(EDITOR_CONTEXT(editorResources)->GetShader("Grid"));
	m_gridMaterial.SetBlendable(true);
}

void Editor::Rendering::GridRenderPass::ApplyGridColor(const Maths::Vector3& gridColor)
{
	// Material parameter writes bump the global material mutation epoch, which is
	// part of the Scene View static frame cache key. Writing the same color every
	// frame would make that cache miss forever, so only apply real changes. Reading
	// the stored value (instead of caching a copy here) also stays correct when the
	// parameter block is reset by a shader reload.
	const auto* existing = m_gridMaterial.GetParameterBlock().TryGet("u_Color");
	if (existing != nullptr && existing->type() == typeid(Maths::Vector3))
	{
		if (std::any_cast<const Maths::Vector3&>(*existing) == gridColor)
			return;
	}

	m_gridMaterial.Set("u_Color", gridColor);
}

const std::optional<NLS::Render::Context::RenderPassCommandInput>& Editor::Rendering::GridRenderPass::GetPreparedThreadedPassInput() const
{
    return m_preparedThreadedPassInput;
}

std::optional<NLS::Render::Context::RenderPassCommandInput> Editor::Rendering::GridRenderPass::ConsumePreparedThreadedPassInput()
{
    auto passInput = std::move(m_preparedThreadedPassInput);
    m_preparedThreadedPassInput.reset();
    return passInput;
}

void Editor::Rendering::GridRenderPass::OnBeginFrame(const NLS::Render::Data::FrameDescriptor&)
{
    m_preparedThreadedPassInput.reset();
}

void Editor::Rendering::GridRenderPass::Draw(NLS::Render::Data::PipelineState p_pso)
{
	NLS_ASSERT(m_renderer.HasDescriptor<GridDescriptor>(), "Cannot find GridDescriptor attached to this renderer");
	NLS_ASSERT(m_renderer.HasDebugDrawService(), "Cannot find DebugDrawService attached to this renderer");

	auto& gridDescriptor = m_renderer.GetDescriptor<GridDescriptor>();
	auto& debugDrawService = *m_renderer.GetDebugDrawService();
    const auto& debugSettings = Editor::Settings::EditorSettings::GetDebugDrawSettingsObject();
	debugDrawService.SetEnabled(debugSettings.debugDrawEnabled);
	debugDrawService.SetCategoryEnabled(NLS::Render::Debug::DebugDrawCategory::Grid, debugSettings.debugDrawGrid);

	if (!ShouldIncludeInThreadedFrame(
        true,
        true,
        debugSettings.debugDrawEnabled,
        debugSettings.debugDrawGrid))
		return;

	auto pso = Editor::Rendering::CreateEditorGridPipelineState(p_pso);

	constexpr float gridSize = 5000.0f;

	Maths::Matrix4 model = BuildGridPlaneModelMatrix(gridDescriptor, gridSize);

	ApplyGridColor(gridDescriptor.gridColor);

	UpdateGridAxes(debugDrawService, gridDescriptor, gridSize);

    if (NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_renderer.GetDriver()))
    {
        m_preparedThreadedPassInput = BuildThreadedPassInput(pso);
        return;
    }

	if (!ShouldSkipEditorGridPlane())
	{
		if (auto* planeMesh = EDITOR_CONTEXT(editorResources)->GetMesh("Plane"))
			m_debugModelRenderer.DrawMeshWithSingleMaterial(pso, *planeMesh, m_gridMaterial, model);
	}
}

std::optional<NLS::Render::Context::RenderPassCommandInput> Editor::Rendering::GridRenderPass::BuildThreadedPassInput(
    NLS::Render::Data::PipelineState)
{
    if (ShouldSkipEditorGridPlane() && ShouldSkipEditorGridAxes()) return std::nullopt;
    const auto& frame = m_renderer.GetFrameDescriptor();
    const auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_renderer.GetDriver());
    const auto* shader = m_gridMaterial.GetShader();
    if (!device || !frame.camera || !shader) return std::nullopt;
    const auto backend = device->GetAdapter()->GetBackendType();
    if (!m_gridShaders || m_gridShaders->generation != shader->GetGeneration() || m_gridShaders->vertex.targetBackend != backend)
        m_gridShaders = CaptureEditorGridShaders(*shader,backend);
    const auto snapshot = NLS::Render::FrameGraph::CaptureExternalSceneOutputSnapshot(frame);
    NLS::Render::RHI::RHIRenderTargetLayoutDesc targets;
    targets.colorFormats = {snapshot.outputColorView ? snapshot.outputColorView->GetDesc().format : NLS::Render::RHI::TextureFormat::RGBA8};
    targets.colorSpaces = {snapshot.outputColorView ? snapshot.outputColorView->GetDesc().colorSpace : NLS::Render::RHI::TextureColorSpace::Linear};
    targets.hasDepth = true;
    targets.depthFormat = snapshot.outputDepthStencilView ? snapshot.outputDepthStencilView->GetDesc().format : NLS::Render::RHI::TextureFormat::Depth24Stencil8;
    if (snapshot.outputColorTexture) targets.sampleCount = snapshot.outputColorTexture->GetDesc().sampleCount;
    const auto& grid = m_renderer.GetDescriptor<GridDescriptor>();
    std::string error;
    std::vector<NLS::Render::Context::RecordedDrawCommandInput> draws;
    const auto viewProjection = frame.camera->GetProjectionMatrix()*frame.camera->GetViewMatrix();
    if (!ShouldSkipEditorGridPlane())
    {
        auto draw = m_gridRenderer.Prepare(device,m_gridShaders,targets,viewProjection,grid.viewPosition,grid.gridColor,error);
        if (!draw) { NLS_LOG_WARNING("Editor grid: "+error); return std::nullopt; }
        draws.push_back(std::move(*draw));
    }
    if (!ShouldSkipEditorGridAxes())
    {
        const auto* axesShader = EDITOR_CONTEXT(shaderManager)[":Shaders/DebugPrimitive.hlsl"];
        if (!axesShader) return std::nullopt;
        if (!m_axesShaders || m_axesShaders->generation != axesShader->GetGeneration() || m_axesShaders->vertex.targetBackend != backend)
            m_axesShaders = CaptureEditorGridShaders(*axesShader,backend);
        auto axes = m_gridRenderer.PrepareAxes(device,m_axesShaders,targets,viewProjection,grid.viewPosition,error);
        if (!axes) { NLS_LOG_WARNING("Editor axes: "+error); return std::nullopt; }
        for (auto& draw : *axes) draws.push_back(std::move(draw));
    }
    NLS::Render::Context::RenderPassCommandInput input;
    input.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    input.debugName = "EditorGridPass";
    input.queueType = NLS::Render::RHI::QueueType::Graphics;
    input.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    input.targetsSwapchain = NLS::Render::FrameGraph::FrameTargetsSwapchain(frame);
    input.renderWidth = frame.renderWidth; input.renderHeight = frame.renderHeight;
    input.usesColorAttachment = true; input.usesDepthStencilAttachment = true;
    input.recordedDrawCommands = std::move(draws); input.drawCount = input.recordedDrawCommands.size();
    return input;
}

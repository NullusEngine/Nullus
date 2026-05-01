#include <Rendering/Debug/DebugDrawService.h>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/FrameGraph/ExternalResourceBridge.h>
#include <Rendering/Settings/GraphicsBackendUtils.h>
#include <cstdlib>
#include <cstring>

#include <Debug/Assertion.h>

#include "Core/EditorResources.h"
#include "Core/EditorActions.h"
#include "Rendering/EditorPipelineStatePresets.h"
#include "Rendering/GridRenderPass.h"
#include "Settings/EditorSettings.h"
using namespace NLS;

namespace
{
	bool ShouldSkipEditorGridPlane()
	{
		return NLS::Render::Settings::GetThreadDiagnosticsSettings().editorGridSkipPlane;
	}

	bool ShouldSkipEditorGridAxes()
	{
		return NLS::Render::Settings::GetThreadDiagnosticsSettings().editorGridSkipAxes;
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

std::optional<NLS::Render::Context::RenderPassCommandInput> Editor::Rendering::GridRenderPass::GetPreparedThreadedPassInput() const
{
    return m_preparedThreadedPassInput;
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
	debugDrawService.SetEnabled(Editor::Settings::EditorSettings::DebugDrawEnabled);
	debugDrawService.SetCategoryEnabled(NLS::Render::Debug::DebugDrawCategory::Grid, Editor::Settings::EditorSettings::DebugDrawGrid);

	if (!ShouldIncludeInThreadedFrame(
        true,
        true,
        Editor::Settings::EditorSettings::DebugDrawEnabled,
        Editor::Settings::EditorSettings::DebugDrawGrid))
		return;

	auto pso = Editor::Rendering::CreateEditorGridPipelineState(p_pso);

    if (NLS::Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(m_renderer.GetDriver()))
    {
        m_preparedThreadedPassInput = BuildThreadedPassInput(pso);
        return;
    }

	constexpr float gridSize = 5000.0f;

	Maths::Matrix4 model =
		Maths::Matrix4::Translation({ gridDescriptor.viewPosition.x, 0.0f, gridDescriptor.viewPosition.z }) *
		Maths::Matrix4::Scaling({ gridSize * 2.0f, 1.f, gridSize * 2.0f });

	m_gridMaterial.Set("u_Color", gridDescriptor.gridColor);

	if (!ShouldSkipEditorGridPlane())
	{
		m_debugModelRenderer.DrawModelWithSingleMaterial(pso, *EDITOR_CONTEXT(editorResources)->GetModel("Plane"), m_gridMaterial, model);
	}

	if (!ShouldSkipEditorGridAxes())
	{
		NLS::Render::Debug::DebugDrawSubmitOptions gridOptions;
		gridOptions.category = NLS::Render::Debug::DebugDrawCategory::Grid;
		debugDrawService.SubmitLine(Maths::Vector3(-gridSize + gridDescriptor.viewPosition.x, 0.0f, 0.0f), Maths::Vector3(gridSize + gridDescriptor.viewPosition.x, 0.0f, 0.0f), Maths::Vector3(1.0f, 0.0f, 0.0f), 1.0f, gridOptions);
		debugDrawService.SubmitLine(Maths::Vector3(0.0f, -gridSize + gridDescriptor.viewPosition.y, 0.0f), Maths::Vector3(0.0f, gridSize + gridDescriptor.viewPosition.y, 0.0f), Maths::Vector3(0.0f, 1.0f, 0.0f), 1.0f, gridOptions);
		debugDrawService.SubmitLine(Maths::Vector3(0.0f, 0.0f, -gridSize + gridDescriptor.viewPosition.z), Maths::Vector3(0.0f, 0.0f, gridSize + gridDescriptor.viewPosition.z), Maths::Vector3(0.0f, 0.0f, 1.0f), 1.0f, gridOptions);
	}
}

std::optional<NLS::Render::Context::RenderPassCommandInput> Editor::Rendering::GridRenderPass::BuildThreadedPassInput(
    NLS::Render::Data::PipelineState p_pso)
{
    if (ShouldSkipEditorGridPlane())
        return std::nullopt;

    auto* planeModel = EDITOR_CONTEXT(editorResources)->GetModel("Plane");
    if (planeModel == nullptr)
        return std::nullopt;

    NLS_ASSERT(m_renderer.HasDescriptor<GridDescriptor>(), "Cannot find GridDescriptor attached to this renderer");
    auto& gridDescriptor = m_renderer.GetDescriptor<GridDescriptor>();
    const auto& frameDescriptor = m_renderer.GetFrameDescriptor();

    constexpr float gridSize = 5000.0f;
    Maths::Matrix4 model =
        Maths::Matrix4::Translation({ gridDescriptor.viewPosition.x, 0.0f, gridDescriptor.viewPosition.z }) *
        Maths::Matrix4::Scaling({ gridSize * 2.0f, 1.f, gridSize * 2.0f });

    m_gridMaterial.Set("u_Color", gridDescriptor.gridColor);

    NLS::Render::Context::RenderPassCommandInput passInput;
    passInput.kind = NLS::Render::Context::RenderPassCommandKind::Helper;
    passInput.debugName = "EditorGridPass";
    passInput.queueType = NLS::Render::RHI::QueueType::Graphics;
    passInput.queueDependencyPolicy = NLS::Render::Context::QueueDependencyPolicy::Previous;
    passInput.requiresFrameData = true;
    passInput.requiresObjectData = true;
    passInput.targetsSwapchain = NLS::Render::FrameGraph::FrameTargetsSwapchain(frameDescriptor);
    passInput.renderWidth = frameDescriptor.renderWidth;
    passInput.renderHeight = frameDescriptor.renderHeight;
    passInput.usesColorAttachment = true;
    passInput.usesDepthStencilAttachment = true;

    m_debugModelRenderer.CaptureModelDrawCommandsWithSingleMaterial(
        p_pso,
        *planeModel,
        m_gridMaterial,
        model,
        passInput.recordedDrawCommands);

    passInput.drawCount = static_cast<uint64_t>(passInput.recordedDrawCommands.size());
    if (passInput.drawCount == 0u)
        return std::nullopt;

    return passInput;
}

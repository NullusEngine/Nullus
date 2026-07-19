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
    constexpr uint64_t kEditorGridAxesPersistentGroupId = 0x4E4C534752494441ull;

    bool ShouldLogGridPassDiagnostics(const NLS::Render::Context::Driver& driver)
    {
        return NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(driver).logRenderDrawPath;
    }

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
    if (ShouldSkipEditorGridAxes())
    {
        if (m_hasCachedGridAxes)
            debugDrawService.RemovePersistentPrimitiveGroup(kEditorGridAxesPersistentGroupId);
        m_hasCachedGridAxes = false;
        return;
    }

    if (m_hasCachedGridAxes && m_cachedGridAxesPosition == gridDescriptor.viewPosition)
        return;

    NLS::Render::Debug::DebugDrawSubmitOptions gridOptions;
    gridOptions.category = NLS::Render::Debug::DebugDrawCategory::Grid;
    gridOptions.lifetime = NLS::Render::Debug::DebugDrawLifetime::Persistent();

    const auto makeLine = [&gridOptions](
        const Maths::Vector3& start,
        const Maths::Vector3& end,
        const Maths::Vector3& color)
    {
        auto options = gridOptions;
        options.style.color = color;
        options.style.lineWidth = 1.0f;
        NLS::Render::Debug::DebugDrawPrimitive primitive;
        primitive.type = NLS::Render::Debug::DebugDrawPrimitiveType::Line;
        primitive.points[0] = start;
        primitive.points[1] = end;
        primitive.options = options;
        return primitive;
    };

    std::vector<NLS::Render::Debug::DebugDrawPrimitive> axes;
    axes.reserve(3u);
    axes.push_back(makeLine(
        { -gridSize + gridDescriptor.viewPosition.x, 0.0f, 0.0f },
        { gridSize + gridDescriptor.viewPosition.x, 0.0f, 0.0f },
        { 1.0f, 0.0f, 0.0f }));
    axes.push_back(makeLine(
        { 0.0f, -gridSize + gridDescriptor.viewPosition.y, 0.0f },
        { 0.0f, gridSize + gridDescriptor.viewPosition.y, 0.0f },
        { 0.0f, 1.0f, 0.0f }));
    axes.push_back(makeLine(
        { 0.0f, 0.0f, -gridSize + gridDescriptor.viewPosition.z },
        { 0.0f, 0.0f, gridSize + gridDescriptor.viewPosition.z },
        { 0.0f, 0.0f, 1.0f }));

    if (debugDrawService.SetPersistentPrimitiveGroup(kEditorGridAxesPersistentGroupId, std::move(axes)))
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

	m_gridMaterial.Set("u_Color", gridDescriptor.gridColor);

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
    NLS::Render::Data::PipelineState p_pso)
{
    if (ShouldSkipEditorGridPlane())
    {
        if (ShouldLogGridPassDiagnostics(m_renderer.GetDriver()))
            NLS_LOG_INFO("[EditorGridPass] threaded input skipped: plane disabled");
        return std::nullopt;
    }

    auto* planeMesh = EDITOR_CONTEXT(editorResources)->GetMesh("Plane");
    if (planeMesh == nullptr)
    {
        if (ShouldLogGridPassDiagnostics(m_renderer.GetDriver()))
            NLS_LOG_INFO("[EditorGridPass] threaded input skipped: Plane mesh unavailable");
        return std::nullopt;
    }

    NLS_ASSERT(m_renderer.HasDescriptor<GridDescriptor>(), "Cannot find GridDescriptor attached to this renderer");
    auto& gridDescriptor = m_renderer.GetDescriptor<GridDescriptor>();
    const auto& frameDescriptor = m_renderer.GetFrameDescriptor();

    constexpr float gridSize = 5000.0f;
    Maths::Matrix4 model = BuildGridPlaneModelMatrix(gridDescriptor, gridSize);

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

    const auto explicitDevice =
        NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(m_renderer.GetDriver());
    const ThreadedCommandCacheKey cacheKey {
        planeMesh,
        m_gridMaterial.GetShader(),
        explicitDevice != nullptr ? explicitDevice->GetCacheIdentity() : 0u,
        p_pso.bits.to_ullong(),
        gridDescriptor.gridColor,
        gridDescriptor.viewPosition
    };
    bool canReuseCachedCommands =
        m_threadedCommandCacheKey.has_value() &&
        *m_threadedCommandCacheKey == cacheKey &&
        !m_cachedThreadedDrawCommands.empty();
    if (canReuseCachedCommands)
    {
        std::shared_ptr<NLS::Render::RHI::RHIBindingSet> currentFrameBindingSet;
        auto* bindingProvider = m_renderer.GetFrameObjectBindingProvider();
        canReuseCachedCommands =
            bindingProvider != nullptr &&
            bindingProvider->CaptureFrameBindingSet(currentFrameBindingSet);
        if (canReuseCachedCommands)
        {
            passInput.recordedDrawCommands = m_cachedThreadedDrawCommands;
            for (auto& drawCommand : passInput.recordedDrawCommands)
                drawCommand.frameBindingSet = currentFrameBindingSet;
        }
        else
        {
            m_threadedCommandCacheKey.reset();
            m_cachedThreadedDrawCommands.clear();
        }
    }

    if (!canReuseCachedCommands)
    {
        m_debugModelRenderer.CaptureMeshDrawCommandsWithSingleMaterial(
            p_pso,
            *planeMesh,
            m_gridMaterial,
            model,
            passInput.recordedDrawCommands);

        m_cachedThreadedDrawCommands = passInput.recordedDrawCommands;
        for (auto& drawCommand : m_cachedThreadedDrawCommands)
            drawCommand.frameBindingSet.reset();
        m_threadedCommandCacheKey = cacheKey;
    }

    passInput.drawCount = static_cast<uint64_t>(passInput.recordedDrawCommands.size());
    if (passInput.drawCount == 0u)
    {
        if (ShouldLogGridPassDiagnostics(m_renderer.GetDriver()))
            NLS_LOG_INFO("[EditorGridPass] threaded input skipped: captured draw count is zero");
        return std::nullopt;
    }

    if (ShouldLogGridPassDiagnostics(m_renderer.GetDriver()))
    {
        NLS_LOG_INFO(
            "[EditorGridPass] threaded input captured drawCount=" +
            std::to_string(passInput.drawCount));
    }

    return passInput;
}

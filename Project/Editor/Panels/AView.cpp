#include "Panels/AView.h"
#include "Panels/ViewFrameLifecycle.h"
#include "Core/EditorActions.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/FrameGraph/ExternalResourceBridge.h"
#include "ServiceLocator.h"
#include "UI/UIManager.h"
#include <algorithm>
#include <limits>

using namespace NLS;

Editor::Panels::AView::AView
(
	const std::string& p_title,
	bool p_opened,
	const UI::PanelWindowSettings& p_windowSettings
) : PanelWindow(p_title, p_opened, p_windowSettings)
{
	m_image = &CreateWidget<UI::Widgets::Image>(m_fbo.GetOrCreateExplicitColorView("Editor.AView.Output"), Maths::Vector2{ 0.f, 0.f });
	m_image->flipVertically = NLS_SERVICE(UI::UIManager).ShouldFlipPresentedRenderTargetVertically();
	panelSettings.scrollable = false;
}

void Editor::Panels::AView::Update(float p_deltaTime)
{
	(void)p_deltaTime;
}

void Editor::Panels::AView::_Draw_Impl()
{
    NLS_SERVICE(UI::UIManager).PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	UI::PanelWindow::_Draw_Impl();
    NLS_SERVICE(UI::UIManager).PopStyleVar();
}

void Editor::Panels::AView::OnBeforeDrawWidgets()
{
	SyncViewToCurrentContentRegion();
	Render(m_lastResolvedViewSize.first, m_lastResolvedViewSize.second);
}

void Editor::Panels::AView::InitFrame()
{
	m_renderer->AddDescriptor<Engine::Rendering::BaseSceneRenderer::SceneDescriptor>(
		CreateSceneDescriptor()
	);
}

void Editor::Panels::AView::Render()
{
	auto [winWidth, winHeight] = GetSafeSize();
	Render(winWidth, winHeight);
}

void Editor::Panels::AView::SyncViewToCurrentContentRegion()
{
	const ImVec2 availableContentRegion = ImGui::GetContentRegionAvail();
	const float clampedWidth = std::clamp(
		availableContentRegion.x,
		0.0f,
		static_cast<float>(std::numeric_limits<uint16_t>::max()));
	const float clampedHeight = std::clamp(
		availableContentRegion.y,
		0.0f,
		static_cast<float>(std::numeric_limits<uint16_t>::max()));

	const auto winWidth = static_cast<uint16_t>(clampedWidth);
	const auto winHeight = static_cast<uint16_t>(clampedHeight);

    std::pair<uint16_t, uint16_t> requestedSize { winWidth, winHeight };
    Render::Context::ThreadedFrameTelemetry telemetry {};
    auto* driver = Render::Context::TryGetLocatedDriver();
    const bool threadedRendering =
        driver != nullptr &&
        Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(*driver);
    if (threadedRendering)
    {
        telemetry = Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(*driver);
        if (Editor::Panels::ShouldDrainBeforeRetirementAwareViewResize(
            requestedSize,
            m_lastResolvedViewSize,
            RequiresRetiredFrameConsumption(),
            telemetry))
        {
            Render::Context::DriverRendererAccess::DrainThreadedRendering(*driver);
            telemetry = Render::Context::DriverRendererAccess::GetThreadedFrameTelemetry(*driver);
        }
    }

    if (Editor::Panels::ShouldDeferRetirementAwareViewResize(
        requestedSize,
        m_lastResolvedViewSize,
        RequiresRetiredFrameConsumption(),
        telemetry))
    {
        m_pendingResolvedViewSize = requestedSize;
        m_image->size = Maths::Vector2(
            static_cast<float>(m_lastResolvedViewSize.first),
            static_cast<float>(m_lastResolvedViewSize.second));
        return;
    }

    if (m_pendingResolvedViewSize.has_value())
        requestedSize = m_pendingResolvedViewSize.value();

    ApplyResolvedViewSize(requestedSize.first, requestedSize.second);
    m_image->size = Maths::Vector2(
        static_cast<float>(m_lastResolvedViewSize.first),
        static_cast<float>(m_lastResolvedViewSize.second));
}

void Editor::Panels::AView::Render(const uint16_t p_width, const uint16_t p_height)
{
	auto camera = GetCamera();
	auto scene = GetScene();

	if (p_width > 0 && p_height > 0 && camera && scene)
	{
		InitFrame();

		Render::Data::FrameDescriptor frameDescriptor;
		frameDescriptor.renderWidth = p_width;
		frameDescriptor.renderHeight = p_height;
		frameDescriptor.camera = camera;
		const auto& clearColor = camera->GetClearColor();
		m_fbo.SetOptimizedColorClearValue(clearColor.x, clearColor.y, clearColor.z, 1.0f);
		m_image->textureView = m_fbo.GetOrCreateExplicitColorView("Editor.AView.Output");
        NLS::Render::FrameGraph::SetExternalSceneOutputFramebuffer(frameDescriptor, &m_fbo);

		m_renderer->BeginFrame(frameDescriptor);
		DrawFrame();
		m_renderer->EndFrame();
        if (Editor::Panels::ShouldDrainAfterRetirementAwareViewRender(
            RequiresRetiredFrameConsumption(),
            RequiresImmediateRetiredFrameReadback()))
        {
            if (auto* driver = Render::Context::TryGetLocatedDriver();
                driver != nullptr && Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(*driver))
            {
                // Immediate readback consumers need the just-submitted offscreen frame retired.
                // Texture-only consumers can present the latest available texture without a CPU drain.
                Render::Context::DriverRendererAccess::DrainThreadedRendering(*driver);
            }
        }
		AfterRenderFrame();
	}
}

void Editor::Panels::AView::DrawFrame()
{
	m_renderer->DrawFrame();
}

void Editor::Panels::AView::AfterRenderFrame()
{
}

bool Editor::Panels::AView::RequiresRetiredFrameConsumption() const
{
    return m_requiresRetiredFrameConsumption;
}

void Editor::Panels::AView::SetRequiresRetiredFrameConsumption(const bool requiresRetiredFrameConsumption)
{
    m_requiresRetiredFrameConsumption = requiresRetiredFrameConsumption;
}

bool Editor::Panels::AView::RequiresImmediateRetiredFrameReadback() const
{
    return m_requiresImmediateRetiredFrameReadback;
}

void Editor::Panels::AView::SetRequiresImmediateRetiredFrameReadback(
    const bool requiresImmediateRetiredFrameReadback)
{
    m_requiresImmediateRetiredFrameReadback = requiresImmediateRetiredFrameReadback;
}

void Editor::Panels::AView::ApplyResolvedViewSize(const uint16_t p_width, const uint16_t p_height)
{
    m_lastResolvedViewSize = { p_width, p_height };
    m_fbo.Resize(p_width, p_height);
    m_image->textureView = m_fbo.GetOrCreateExplicitColorView("Editor.AView.Output");
    m_pendingResolvedViewSize.reset();
}

std::pair<uint16_t, uint16_t> Editor::Panels::AView::GetSafeSize() const
{
	if (m_lastResolvedViewSize.first > 0u || m_lastResolvedViewSize.second > 0u)
		return m_lastResolvedViewSize;

	constexpr float kTitleBarHeight = 25.0f; // <--- this takes into account the imgui window title bar
	const auto& size = GetSize();
	return {
		static_cast<uint16_t>(size.x),
		static_cast<uint16_t>(std::max(0.0f, size.y - kTitleBarHeight)) // <--- clamp to prevent the output size to be negative
	}; 
}

const Engine::Rendering::BaseSceneRenderer& Editor::Panels::AView::GetRenderer() const
{
	return *m_renderer.get();
}

bool Editor::Panels::AView::IsMouseWithinView(const Maths::Vector2& mousePosition) const
{
    if (m_image == nullptr || !m_image->HasLastDrawBounds())
        return false;

    const auto& min = m_image->GetLastDrawMin();
    const auto& max = m_image->GetLastDrawMax();
    return mousePosition.x >= min.x && mousePosition.x < max.x &&
        mousePosition.y >= min.y && mousePosition.y < max.y;
}

std::optional<Maths::Vector2> Editor::Panels::AView::GetLocalViewPosition(const Maths::Vector2& mousePosition) const
{
    if (m_image == nullptr || !m_image->HasLastDrawBounds() || !IsMouseWithinView(mousePosition))
        return std::nullopt;

    const auto& min = m_image->GetLastDrawMin();
    const auto& max = m_image->GetLastDrawMax();
    Maths::Vector2 localPosition {
        mousePosition.x - min.x,
        mousePosition.y - min.y
    };

    localPosition.x = std::clamp(localPosition.x, 0.0f, std::max(0.0f, max.x - min.x));
    localPosition.y = std::clamp(localPosition.y, 0.0f, std::max(0.0f, max.y - min.y));
    return localPosition;
}

Engine::Rendering::BaseSceneRenderer::SceneDescriptor Editor::Panels::AView::CreateSceneDescriptor()
{
	auto scene = GetScene();

	NLS_ASSERT(scene, "No scene assigned to this view!");

	return {
		*scene
	};
}

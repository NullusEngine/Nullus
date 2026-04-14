#include "Panels/AView.h"
#include "Core/EditorActions.h"
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
	m_image->flipVertically =
		NLS_SERVICE(UI::UIManager).GetGraphicsBackend() == NLS::Render::Settings::EGraphicsBackend::OPENGL;
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
	m_lastResolvedViewSize = { winWidth, winHeight };
	m_image->size = Maths::Vector2(static_cast<float>(winWidth), static_cast<float>(winHeight));
	m_fbo.Resize(winWidth, winHeight);
	m_image->textureView = m_fbo.GetOrCreateExplicitColorView("Editor.AView.Output");
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
		frameDescriptor.outputBuffer = &m_fbo;

		m_renderer->BeginFrame(frameDescriptor);
		DrawFrame();
		m_renderer->EndFrame();
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

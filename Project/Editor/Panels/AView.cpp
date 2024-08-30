#include "Panels/AView.h"
#include "Core/EditorActions.h"
#include "ServiceLocator.h"
#include "UI/UIManager.h"

using namespace NLS;
Editor::Panels::AView::AView
(
	const std::string& p_title,
	bool p_opened,
	const UI::PanelWindowSettings& p_windowSettings
) : PanelWindow(p_title, p_opened, p_windowSettings)
{
	m_image = &CreateWidget<UI::Widgets::Image>(m_fbo.GetTextureID(), Maths::Vector2{ 0.f, 0.f });
	panelSettings.scrollable = false;
}

void Editor::Panels::AView::Update(float p_deltaTime)
{
	auto[winWidth, winHeight] = GetSafeSize();
	m_image->size = Maths::Vector2(static_cast<float>(winWidth), static_cast<float>(winHeight));
	m_fbo.Resize(winWidth, winHeight);
}

void Editor::Panels::AView::_Draw_Impl()
{
    NLS_SERVICE(UI::UIManager).PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	UI::PanelWindow::_Draw_Impl();
    NLS_SERVICE(UI::UIManager).PopStyleVar();
}

void Editor::Panels::AView::InitFrame()
{
	m_renderer->AddDescriptor<Engine::Rendering::SceneRenderer::SceneDescriptor>(
		CreateSceneDescriptor()
	);
}

void Editor::Panels::AView::Render()
{
	auto [winWidth, winHeight] = GetSafeSize();
	auto camera = GetCamera();
	auto scene = GetScene();

	if (winWidth > 0 && winHeight > 0 && camera && scene)
	{
		InitFrame();

		Render::Data::FrameDescriptor frameDescriptor;
		frameDescriptor.renderWidth = winWidth;
		frameDescriptor.renderHeight = winHeight;
		frameDescriptor.camera = camera;
		frameDescriptor.outputBuffer = &m_fbo;

		m_renderer->BeginFrame(frameDescriptor);
		DrawFrame();
		m_renderer->EndFrame();
	}
}

void Editor::Panels::AView::DrawFrame()
{
	m_renderer->DrawFrame();
}

std::pair<uint16_t, uint16_t> Editor::Panels::AView::GetSafeSize() const
{
	constexpr float kTitleBarHeight = 25.0f; // <--- this takes into account the imgui window title bar
	const auto& size = GetSize();
	return {
		static_cast<uint16_t>(size.x),
		static_cast<uint16_t>(std::max(0.0f, size.y - kTitleBarHeight)) // <--- clamp to prevent the output size to be negative
	}; 
}

const Engine::Rendering::SceneRenderer& Editor::Panels::AView::GetRenderer() const
{
	return *m_renderer.get();
}

Engine::Rendering::SceneRenderer::SceneDescriptor Editor::Panels::AView::CreateSceneDescriptor()
{
	auto scene = GetScene();

	NLS_ASSERT(scene, "No scene assigned to this view!");

	return {
		*scene
	};
}

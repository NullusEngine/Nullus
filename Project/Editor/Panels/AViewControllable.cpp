
#include "Panels/AViewControllable.h"
#include "Rendering/DebugSceneRenderer.h"
#include "Rendering/GridRenderPass.h"
#include "Core/EditorActions.h"
using namespace NLS;
const Maths::Vector3 kDefaultGridColor{ 0.176f, 0.176f, 0.176f };
const Maths::Vector3 kDefaultClearColor{ 0.098f, 0.098f, 0.098f };
const Maths::Vector3 kDefaultCameraPosition{ -10.0f, 3.0f, 10.0f };
const Maths::Quaternion kDefaultCameraRotation({ 0.0f, 135.0f, 0.0f });

Editor::Panels::AViewControllable::AViewControllable(
	const std::string& p_title,
	bool p_opened,
	const UI::Settings::PanelWindowSettings& p_windowSettings
) :
	AView(p_title, p_opened, p_windowSettings),
	m_cameraController(*this, m_camera)
{
	ResetCameraTransform();
	ResetGridColor();
	ResetClearColor();
}

void Editor::Panels::AViewControllable::Update(float p_deltaTime)
{
	m_cameraController.HandleInputs(p_deltaTime);
	AView::Update(p_deltaTime);
}

void Editor::Panels::AViewControllable::InitFrame()
{
	AView::InitFrame();

	m_renderer->AddDescriptor<Rendering::GridRenderPass::GridDescriptor>({
		m_gridColor,
		m_camera.GetPosition()
	});
}

void Editor::Panels::AViewControllable::ResetCameraTransform()
{
	if (m_camera.transform)
	{
		m_camera.transform->SetWorldPosition(kDefaultCameraPosition);
		m_camera.transform->SetWorldRotation(kDefaultCameraRotation);
	}
}

Editor::Core::CameraController& Editor::Panels::AViewControllable::GetCameraController()
{
	return m_cameraController;
}

NLS::Render::Entities::Camera* Editor::Panels::AViewControllable::GetCamera()
{
	return &m_camera;
}

const Maths::Vector3& Editor::Panels::AViewControllable::GetGridColor() const
{
	return m_gridColor;
}

void Editor::Panels::AViewControllable::SetGridColor(const Maths::Vector3& p_color)
{
	m_gridColor = p_color;
}

void Editor::Panels::AViewControllable::ResetGridColor()
{
	m_gridColor = kDefaultGridColor;
}

void Editor::Panels::AViewControllable::ResetClearColor()
{
	m_camera.SetClearColor(kDefaultClearColor);
}

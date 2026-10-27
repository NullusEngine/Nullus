#include "VfxPreviewCamera.h"
#include "Core/Context.h"
#include "Settings/EditorSettings.h"
#include <ServiceLocator.h>
#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace NLS::Editor::VFXTools
{
VfxPreviewCamera::VfxPreviewCamera()
{
    m_camera.SetPosition({0,0,15});
    m_camera.SetRotation(Maths::Quaternion::LookAt({0,0,-1},Maths::Vector3::Up));
    m_camera.SetFov(45); m_camera.SetNear(0.05f); m_camera.SetFar(5000);
}
VfxPreviewCamera::~VfxPreviewCamera() = default;
void VfxPreviewCamera::HandleInputs(const Maths::Vector2& minimum,const Maths::Vector2& maximum,
    float deltaTime,bool inputAvailable)
{
    if (!NLS::Core::ServiceLocator::Contains<Core::Context>()) return;
    if (!m_controller)
    {
        const auto& context = NLS::Core::ServiceLocator::Get<Core::Context>();
        m_controller = std::make_unique<Core::CameraController>(m_camera,*context.inputManager,*context.window);
        m_controller->SetFocusState(&m_focus);
    }
    m_controller->SetViewportInputBounds(minimum,maximum,inputAvailable);
    m_controller->SetSpeed(Settings::EditorSettings::GetViewSettingsObject().sceneViewCameraSpeed);
    m_controller->SetInputBlocked(ImGui::GetIO().WantTextInput ||
        ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel));
    m_controller->HandleInputs(deltaTime);
}
void VfxPreviewCamera::SuspendInputs()
{ if (m_controller) m_controller->ClearViewportInputBounds(); }
bool VfxPreviewCamera::IsNavigating() const
{ return m_controller && m_controller->IsCameraControlActive(); }
std::array<float,3> VfxPreviewCamera::Position() const
{ const auto& p = m_camera.GetPosition(); return {p.x,p.y,p.z}; }
void VfxPreviewCamera::Focus(const std::array<float,3>& center,float radius)
{
    const Maths::Vector3 point{center[0],center[1],center[2]};
    const auto distance = std::clamp((std::max)(radius,0.001f)/std::tan(m_camera.GetFov()*0.5f*3.14159265358979323846f/180.0f)*1.4f,0.05f,100000.0f);
    m_focus = {point,distance,true};
    if (m_controller) m_controller->MoveToPoint(point,distance);
    else m_camera.SetPosition(point-m_camera.GetRotation()*Maths::Vector3::Forward*distance);
}
void VfxPreviewCamera::ResetFocus()
{
    m_focus = {{0,0,0},15,true};
    m_camera.SetPosition(m_focus.focusPoint-m_camera.GetRotation()*Maths::Vector3::Forward*m_focus.focusDistance);
}
std::array<float,16> VfxPreviewCamera::ViewProjection(uint32_t width,uint32_t height) const
{
    m_camera.CacheMatrices(static_cast<uint16_t>(std::clamp(width,1u,65535u)),static_cast<uint16_t>(std::clamp(height,1u,65535u)));
    // The camera owns projection math; particle shaders consume [0,w] depth.
    auto depth = Maths::Matrix4::Identity; depth(2,2) = 0.5f; depth(2,3) = 0.5f;
    const auto matrix = depth*m_camera.GetProjectionMatrix()*m_camera.GetViewMatrix();
    std::array<float,16> result{};
    std::copy(std::begin(matrix.data),std::end(matrix.data),result.begin()); return result;
}
VFX::SpriteView VfxPreviewCamera::MakeView(uint32_t width,uint32_t height) const
{
    VFX::SpriteView view; view.viewProjection = ViewProjection(width,height);
    const auto& matrix = m_camera.GetViewMatrix();
    view.cameraRight = {matrix.data[0],matrix.data[1],matrix.data[2],0};
    view.cameraUp = {matrix.data[4],matrix.data[5],matrix.data[6],0};
    view.cameraForward = {-matrix.data[8],-matrix.data[9],-matrix.data[10],0};
    view.frustumCulling = true; return view;
}
VFX::CameraContext VfxPreviewCamera::MakeContext(uint32_t width,uint32_t height) const
{
    const auto view = MakeView(width,height); const auto& position = m_camera.GetPosition();
    VFX::CameraContext result;
    result.position = {position.x,position.y,position.z,0};
    result.right = view.cameraRight; result.up = view.cameraUp; result.forward = view.cameraForward;
    result.fovDegrees = m_camera.GetFov(); result.viewportWidth = static_cast<float>((std::max)(1u,width));
    result.viewportHeight = static_cast<float>((std::max)(1u,height)); result.valid = true; return result;
}
}

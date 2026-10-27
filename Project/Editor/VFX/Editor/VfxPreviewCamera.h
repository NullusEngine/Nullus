#pragma once

#include <array>
#include <memory>
#include <Rendering/Entities/Camera.h>
#include <VFX/ParticleBounds.h>
#include "Core/CameraController.h"

namespace NLS::Editor::VFXTools
{
// Adapts the shared editor camera to particle render and simulation snapshots.
class VfxPreviewCamera final
{
public:
    VfxPreviewCamera();
    ~VfxPreviewCamera();
    Render::Entities::Camera& Camera() { return m_camera; }
    const Render::Entities::Camera& Camera() const { return m_camera; }
    const Core::SceneCameraFocusState& FocusState() const { return m_focus; }
    Core::CameraController* Controller() const { return m_controller.get(); }
    void HandleInputs(const Maths::Vector2& minimum,const Maths::Vector2& maximum,float deltaTime,bool inputAvailable);
    void SuspendInputs();
    bool IsNavigating() const;
    void Focus(const std::array<float,3>& center,float radius);
    void ResetFocus();
    std::array<float,3> Position() const;
    std::array<float,16> ViewProjection(uint32_t width,uint32_t height) const;
    VFX::SpriteView MakeView(uint32_t width,uint32_t height) const;
    VFX::CameraContext MakeContext(uint32_t width,uint32_t height) const;
private:
    Maths::Transform m_transform;
    mutable Render::Entities::Camera m_camera{&m_transform};
    Core::SceneCameraFocusState m_focus{{0,0,0},15,true};
    std::unique_ptr<Core::CameraController> m_controller;
};
}

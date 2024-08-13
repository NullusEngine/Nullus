#include <UI/Plugins/DDTarget.h>

#include "Rendering/DebugSceneRenderer.h"
#include "Rendering/PickingRenderPass.h"
#include "Core/EditorActions.h"
#include "Panels/SceneView.h"
#include "Panels/GameView.h"
#include "Settings/EditorSettings.h"
using namespace NLS;
Editor::Panels::SceneView::SceneView(
    const std::string& p_title,
    bool p_opened,
    const UI::Settings::PanelWindowSettings& p_windowSettings)
    : AViewControllable(p_title, p_opened, p_windowSettings), m_sceneManager(EDITOR_CONTEXT(sceneManager))
{
    m_renderer = std::make_unique<Editor::Rendering::DebugSceneRenderer>(*EDITOR_CONTEXT(driver));

    m_camera.SetFar(5000.0f);

    m_fallbackMaterial.SetShader(EDITOR_CONTEXT(shaderManager)[":Shaders\\Unlit.glsl"]);
    m_fallbackMaterial.Set<Maths::Vector4>("u_Diffuse", {1.f, 0.f, 1.f, 1.0f});
    m_fallbackMaterial.Set<Render::Resources::Texture2D*>("u_DiffuseMap", nullptr);

    m_image->AddPlugin<UI::Plugins::DDTarget<std::pair<std::string, UI::Widgets::Layout::Group*>>>("File").DataReceivedEvent += [this](auto p_data)
    {
        std::string path = p_data.first;

        switch (Utils::PathParser::GetFileType(path))
        {
            case Utils::PathParser::EFileType::SCENE:
                EDITOR_EXEC(LoadSceneFromDisk(path));
                break;
            case Utils::PathParser::EFileType::MODEL:
                EDITOR_EXEC(CreateActorWithModel(path, true));
                break;
        }
    };

    Engine::GameObject::DestroyedEvent += [this](const Engine::GameObject& actor)
    {
        if (m_highlightedActor && m_highlightedActor->GetWorldID() == actor.GetWorldID())
        {
            m_highlightedActor = nullptr;
        }
    };
}

void Editor::Panels::SceneView::Update(float p_deltaTime)
{
    AViewControllable::Update(p_deltaTime);

    using namespace Windowing::Inputs;

    if (IsFocused() && !m_cameraController.IsRightMousePressed())
    {
        if (EDITOR_CONTEXT(inputManager)->IsKeyPressed(EKey::KEY_W))
        {
            m_currentOperation = Editor::Core::EGizmoOperation::TRANSLATE;
        }

        if (EDITOR_CONTEXT(inputManager)->IsKeyPressed(EKey::KEY_E))
        {
            m_currentOperation = Editor::Core::EGizmoOperation::ROTATE;
        }

        if (EDITOR_CONTEXT(inputManager)->IsKeyPressed(EKey::KEY_R))
        {
            m_currentOperation = Editor::Core::EGizmoOperation::SCALE;
        }
    }
}

void Editor::Panels::SceneView::InitFrame()
{
    AViewControllable::InitFrame();

    Engine::GameObject* selectedActor = nullptr;

    if (EDITOR_EXEC(IsAnyActorSelected()))
    {
        selectedActor = EDITOR_EXEC(GetSelectedActor());
    }

    m_renderer->AddDescriptor<Rendering::DebugSceneRenderer::DebugSceneDescriptor>({m_currentOperation,
                                                                                    m_highlightedActor,
                                                                                    selectedActor,
                                                                                    m_highlightedGizmoDirection});
}

Engine::SceneSystem::Scene* Editor::Panels::SceneView::GetScene()
{
    return m_sceneManager.GetCurrentScene();
}

Engine::Rendering::SceneRenderer::SceneDescriptor Editor::Panels::SceneView::CreateSceneDescriptor()
{
    auto descriptor = AViewControllable::CreateSceneDescriptor();
    descriptor.fallbackMaterial = &m_fallbackMaterial;
    return descriptor;
}

void Editor::Panels::SceneView::DrawFrame()
{
    Editor::Panels::AViewControllable::DrawFrame();
    HandleActorPicking();
}

bool IsResizing()
{
    auto cursor = NLS_SERVICE(UI::UIManager).GetMouseCursor();

    return cursor == ImGuiMouseCursor_ResizeEW || cursor == ImGuiMouseCursor_ResizeNS || cursor == ImGuiMouseCursor_ResizeNWSE || cursor == ImGuiMouseCursor_ResizeNESW || cursor == ImGuiMouseCursor_ResizeAll;
}

void Editor::Panels::SceneView::HandleActorPicking()
{
    using namespace Windowing::Inputs;

    auto& inputManager = *EDITOR_CONTEXT(inputManager);

    if (inputManager.IsMouseButtonReleased(EMouseButton::MOUSE_BUTTON_LEFT))
    {
        m_gizmoOperations.StopPicking();
    }

    if (IsHovered() && !IsResizing())
    {
        auto mousePos = inputManager.GetMousePosition();
        mousePos -= m_position;
        mousePos.y = GetSafeSize().second - mousePos.y + 25;

        auto& scene = *GetScene();

        auto& actorPickingFeature = m_renderer->GetPass<Rendering::PickingRenderPass>("Picking");

        auto pickingResult = actorPickingFeature.ReadbackPickingResult(
            scene,
            static_cast<uint32_t>(mousePos.x),
            static_cast<uint32_t>(mousePos.y));

        m_highlightedActor = {};
        m_highlightedGizmoDirection = {};

        if (!m_cameraController.IsRightMousePressed() && pickingResult.has_value())
        {
            if (const auto pval = std::get_if<Engine::GameObject*>(&pickingResult.value()))
            {
                m_highlightedActor = *pval;
            }
            else if (const auto pval = std::get_if<Editor::Core::GizmoBehaviour::EDirection>(&pickingResult.value()))
            {
                m_highlightedGizmoDirection = *pval;
            }
        }
        else
        {
            m_highlightedActor = {};
            m_highlightedGizmoDirection = {};
        }

        if (inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT) && !m_cameraController.IsRightMousePressed())
        {
            if (m_highlightedGizmoDirection)
            {
                m_gizmoOperations.StartPicking(
                    *EDITOR_EXEC(GetSelectedActor()),
                    m_camera.GetPosition(),
                    m_currentOperation,
                    m_highlightedGizmoDirection.value());
            }
            else if (m_highlightedActor)
            {
                EDITOR_EXEC(SelectActor(*m_highlightedActor));
            }
            else
            {
                EDITOR_EXEC(UnselectActor());
            }
        }
    }
    else
    {
        m_highlightedActor = nullptr;
        m_highlightedGizmoDirection = std::nullopt;
    }

    if (m_gizmoOperations.IsPicking())
    {
        auto mousePosition = EDITOR_CONTEXT(inputManager)->GetMousePosition();

        auto [winWidth, winHeight] = GetSafeSize();

        m_gizmoOperations.SetCurrentMouse(mousePosition);
        m_gizmoOperations.ApplyOperation(m_camera.GetViewMatrix(), m_camera.GetProjectionMatrix(), {static_cast<float>(winWidth), static_cast<float>(winHeight)});
    }
}

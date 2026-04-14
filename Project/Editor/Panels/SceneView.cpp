#include <UI/Plugins/DDTarget.h>

#include "Rendering/DebugSceneRenderer.h"
#include "Rendering/PickingRenderPass.h"
#include "Rendering/EditorDefaultResources.h"
#include "Rendering/SceneRendererFactory.h"
#include "Rendering/Features/FrameInfoRenderFeature.h"
#include "Core/EditorActions.h"
#include "Panels/SceneView.h"
#include "Panels/GameView.h"
#include "Settings/EditorSettings.h"
#include <UI/UIManager.h>
#include <chrono>
#include <cmath>
using namespace NLS;
Editor::Panels::SceneView::SceneView(
    const std::string& p_title,
    bool p_opened,
    const UI::PanelWindowSettings& p_windowSettings)
    : AViewControllable(p_title, p_opened, p_windowSettings), m_sceneManager(EDITOR_CONTEXT(sceneManager))
{
    // Scene View should always render editor overlays (grid/gizmo/light billboards),
    // so use the editor renderer on every backend.
    m_renderer = std::make_unique<Editor::Rendering::DebugSceneRenderer>(*EDITOR_CONTEXT(driver));

    m_camera.SetFar(5000.0f);

    m_fallbackMaterial.SetShader(EDITOR_CONTEXT(shaderManager)[":Shaders\\Unlit.hlsl"]);
    m_fallbackMaterial.Set<Maths::Vector4>("u_Diffuse", {1.f, 0.f, 1.f, 1.0f});
    m_fallbackMaterial.Set<Render::Resources::Texture2D*>("u_DiffuseMap", Editor::Rendering::GetEditorDefaultWhiteTexture());

    m_image->AddPlugin<UI::DDTarget<std::pair<std::string, UI::Widgets::Group*>>>("File").DataReceivedEvent += [this](auto p_data)
    {
        std::string path = p_data.first;

        switch (Utils::PathParser::GetFileType(path))
        {
            case Utils::PathParser::EFileType::SCENE:
                EDITOR_EXEC(LoadSceneFromDisk(path));
                break;
            case Utils::PathParser::EFileType::MODEL:
                EDITOR_EXEC(CreateActor(path, true));
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

    auto* debugRenderer = dynamic_cast<Editor::Rendering::DebugSceneRenderer*>(m_renderer.get());
    if (debugRenderer == nullptr)
        return;

    Engine::GameObject* selectedActor = nullptr;

    if (EDITOR_EXEC(IsAnyActorSelected()))
    {
        selectedActor = EDITOR_EXEC(GetSelectedActor());
    }

    debugRenderer->AddDescriptor<Rendering::DebugSceneRenderer::DebugSceneDescriptor>({m_currentOperation,
                                                                                       m_highlightedActor,
                                                                                       selectedActor,
                                                                                       m_highlightedGizmoDirection});
}

Engine::SceneSystem::Scene* Editor::Panels::SceneView::GetScene()
{
    return m_sceneManager.GetCurrentScene();
}

Engine::Rendering::BaseSceneRenderer::SceneDescriptor Editor::Panels::SceneView::CreateSceneDescriptor()
{
    auto descriptor = AViewControllable::CreateSceneDescriptor();
    descriptor.fallbackMaterial = &m_fallbackMaterial;
    return descriptor;
}

void Editor::Panels::SceneView::DrawFrame()
{
    Editor::Panels::AViewControllable::DrawFrame();
}

void Editor::Panels::SceneView::AfterRenderFrame()
{
    HandleActorPicking();
}

bool IsResizing()
{
    auto cursor = NLS_SERVICE(UI::UIManager).GetMouseCursor();

    return cursor == ImGuiMouseCursor_ResizeEW || cursor == ImGuiMouseCursor_ResizeNS || cursor == ImGuiMouseCursor_ResizeNWSE || cursor == ImGuiMouseCursor_ResizeNESW || cursor == ImGuiMouseCursor_ResizeAll;
}

void Editor::Panels::SceneView::HandleActorPicking()
{
    auto* debugRenderer = dynamic_cast<Editor::Rendering::DebugSceneRenderer*>(m_renderer.get());
    if (debugRenderer == nullptr)
    {
        m_highlightedActor = nullptr;
        m_highlightedGizmoDirection = std::nullopt;
        m_hasPickingSample = false;
        return;
    }

    using namespace Windowing::Inputs;

    auto& inputManager = *EDITOR_CONTEXT(inputManager);
    const Maths::Vector2 screenMousePos = inputManager.GetMousePosition();
    const bool mouseOverView = IsMouseWithinView(screenMousePos);

    if (inputManager.IsMouseButtonReleased(EMouseButton::MOUSE_BUTTON_LEFT))
    {
        m_gizmoOperations.StopPicking();
    }

    if (mouseOverView && !IsResizing())
    {
        auto& actorPickingFeature = debugRenderer->GetPass<Rendering::PickingRenderPass>("Picking");
        if (!actorPickingFeature.SupportsPickingReadback())
        {
            m_highlightedActor = nullptr;
            m_highlightedGizmoDirection = std::nullopt;
            m_hasPickingSample = false;
            return;
        }

        const auto localMousePos = GetLocalViewPosition(screenMousePos);
        if (!localMousePos.has_value())
        {
            m_highlightedActor = nullptr;
            m_highlightedGizmoDirection = std::nullopt;
            m_hasPickingSample = false;
            return;
        }

        auto mousePos = localMousePos.value();
        const auto [safeWidth, safeHeight] = GetSafeSize();
        const float maxRenderX = std::max(0.0f, static_cast<float>(safeWidth) - 1.0f);
        const float maxRenderY = std::max(0.0f, static_cast<float>(safeHeight) - 1.0f);
        const bool bottomLeftOriginPicking =
            NLS_SERVICE(UI::UIManager).GetGraphicsBackend() == Render::Settings::EGraphicsBackend::OPENGL;

        mousePos.x = std::clamp(mousePos.x, 0.0f, maxRenderX);
        mousePos.y = bottomLeftOriginPicking
            ? std::clamp(static_cast<float>(safeHeight) - 1.0f - mousePos.y, 0.0f, maxRenderY)
            : std::clamp(mousePos.y, 0.0f, maxRenderY);

        const auto now = std::chrono::steady_clock::now();
        const bool mouseMoved =
            std::fabs(mousePos.x - m_lastPickingMousePos.x) > 0.5f ||
            std::fabs(mousePos.y - m_lastPickingMousePos.y) > 0.5f;
        constexpr int kHoverPickingIntervalMs = 120;
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastPickingSampleTime).count();
        const bool sampleExpired =
            !m_hasPickingSample ||
            elapsedMs >= kHoverPickingIntervalMs;
        const bool leftClicked = inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT);
        const bool rightMousePressed = m_cameraController.IsRightMousePressed();
        const bool shouldRepick =
            leftClicked ||
            (!rightMousePressed && sampleExpired && (mouseMoved || !m_hasPickingSample));

        if (shouldRepick)
        {
            auto pickingResult = actorPickingFeature.PickAtRenderCoordinate(
                static_cast<uint32_t>(mousePos.x),
                static_cast<uint32_t>(mousePos.y));

            m_highlightedActor = {};
            m_highlightedGizmoDirection = {};

            if (!rightMousePressed && pickingResult.has_value())
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

            m_lastPickingMousePos = mousePos;
            m_lastPickingSampleTime = now;
            m_hasPickingSample = true;
        }
        else if (rightMousePressed)
        {
            m_highlightedActor = {};
            m_highlightedGizmoDirection = {};
        }

        if (leftClicked && !rightMousePressed)
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
        m_hasPickingSample = false;
    }

    if (m_gizmoOperations.IsPicking())
    {
        auto mousePosition = EDITOR_CONTEXT(inputManager)->GetMousePosition();

        auto [winWidth, winHeight] = GetSafeSize();

        m_gizmoOperations.SetCurrentMouse(mousePosition);
        m_gizmoOperations.ApplyOperation(m_camera.GetViewMatrix(), m_camera.GetProjectionMatrix(), {static_cast<float>(winWidth), static_cast<float>(winHeight)});
    }
}

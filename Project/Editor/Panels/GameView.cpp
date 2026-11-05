#include <Components/CameraComponent.h>
#include <Rendering/Buffers/UniformBuffer.h>
#include <Rendering/SceneRendererFactory.h>
#include <Engine/Rendering/GameOutput.h>
#include <Engine/Components/TransformComponent.h>
#include <Engine/Components/MeshFilter.h>
#include <Rendering/Resources/Mesh.h>
#include "Panels/GameView.h"
#include "Core/EditorActions.h"
#include "Core/EditorInputFrame.h"
#include "Settings/EditorSettings.h"
#include "Settings/EditorSettingsPersistence.h"
#include "ImGui/imgui.h"
#include "ImGui/imgui_internal.h"
#include <Scripting/ScriptEngineApi.h>
#include <Rendering/RHI/Core/RHIResource.h>
#include <Rendering/UI/UIDrawSnapshotAdapter.h>
#include <Rendering/Context/DriverAccess.h>
#include <Engine/UI/UIDocumentSourceResolver.h>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <array>
#include <cstring>
#include <GLFW/glfw3.h>

using namespace NLS;
namespace
{
float GameViewFramebufferScale()
{
    // Monitor DPI scales fonts; framebuffer/window coordinates determine pixels.
    // On Windows these are usually equal even at 150% monitor DPI.
    const auto& io=ImGui::GetIO();
    // DX12 uses imgui_impl_win32: its PlatformHandle is an HWND, not a GLFWwindow.
    if(!io.BackendPlatformName || std::strcmp(io.BackendPlatformName,"imgui_impl_glfw")!=0)
        return io.DisplayFramebufferScale.x>0?io.DisplayFramebufferScale.x:1;
    const auto* viewport=ImGui::GetWindowViewport();
    auto* window=viewport?static_cast<GLFWwindow*>(viewport->PlatformHandle):nullptr;
    if(!window)return ImGui::GetIO().DisplayFramebufferScale.x;
    int width=0,height=0,pixelsWidth=0,pixelsHeight=0;
    glfwGetWindowSize(window,&width,&height);glfwGetFramebufferSize(window,&pixelsWidth,&pixelsHeight);
    return width>0 && pixelsWidth>0?float(pixelsWidth)/width:1;
}
}
#include "GameViewGizmos.inl"
void Editor::Panels::GameView::BeginPlayPreview()
{
    m_wasOpenedBeforePlay = IsOpened();
    m_maximized = Settings::EditorSettings::GetGameViewSettingsObject().maximizeOnPlay;
    m_outputNeedsClear=true;
    m_outputSession=0;m_outputFrame=0;
    Render::Context::DriverRendererAccess::SetPreviewVsyncOverride(*EDITOR_CONTEXT(driver),
        Settings::EditorSettings::GetGameViewSettingsObject().vsync?std::optional<bool>(true):std::nullopt);
    Open();
    Focus();
}
void Editor::Panels::GameView::EndPlayPreview()
{
    ReleaseGameInput();
    m_maximized = false;
    Render::Context::DriverRendererAccess::SetPreviewVsyncOverride(*EDITOR_CONTEXT(driver),std::nullopt);
    m_outputNeedsClear=true;
    m_outputSession=0;m_outputFrame=0;
    if (!m_wasOpenedBeforePlay) Close();
}

void Editor::Panels::GameView::ReleaseGameInput()
{
    if (m_input.IsFocused())
    {
        EDITOR_CONTEXT(window)->SetCursorMode(Cursor::ECursorMode::NORMAL);
        EDITOR_CONTEXT(window)->SetInfiniteCursorWrapEnabled(false);
    }
    m_input.Release();
}

NLS::Scripting::GameInputFrame Editor::Panels::GameView::CaptureGameInput()
{
    auto raw = Core::CaptureEditorInputFrame(*EDITOR_CONTEXT(inputManager));
    auto pointer = raw.position;
    bool ownsPointer = false;
    bool windowFocused=EDITOR_CONTEXT(window)->IsFocused();
    if (auto* context=ImGui::GetCurrentContext())
    {
        auto* window = ImGui::FindWindowByName((name + GetPanelID()).c_str());
        ImGuiWindow* hovered = nullptr;
        ImGui::FindHoveredWindowEx({pointer.x, pointer.y}, false, &hovered, nullptr);
        if (window && ImGui::IsWindowContentHoverable(window))
            for (auto* parent = hovered; parent; parent = parent->ParentWindow)
                if (parent == window) { ownsPointer = true; break; }
        if(window)
        {
            const auto& platform=ImGui::GetPlatformIO();
            if(platform.Platform_GetWindowFocus)windowFocused=platform.Platform_GetWindowFocus(window->Viewport);
            if(m_input.IsFocused() && context->NavWindow && context->NavWindow->RootWindow!=window->RootWindow)windowFocused=false;
        }
    }
    auto& controller = EDITOR_EXEC(GetPlayModeController());
    const bool active = controller.GetState() == Core::PlaySessionState::Playing || controller.GetState() == Core::PlaySessionState::Stepping;
    if (!IsOpened() || !active) ReleaseGameInput();
    const bool wasFocused = m_input.IsFocused();
    auto frame = m_input.Route(std::move(raw), m_presentation, pointer, ownsPointer,
        IsOpened() && windowFocused, active && m_presentation.sessionId==controller.GetSessionId() &&
        m_presentation.display==uint32_t(Settings::EditorSettings::GetGameViewSettingsObject().display), controller.GetSessionId());
    if (wasFocused && !m_input.IsFocused())
        EDITOR_CONTEXT(window)->SetCursorMode(Cursor::ECursorMode::NORMAL);
    EDITOR_EXEC(GetRuntimePlaySession()).NotifyFocus(frame.focused);
    return frame;
}

void Editor::Panels::GameView::_Draw_Impl()
{
    if (m_maximized)
    {
        const auto* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
        ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
    }
    AView::_Draw_Impl();
}

std::pair<uint16_t, uint16_t> Editor::Panels::GameView::ResolveRenderSize(Maths::Vector2 available) const
{
    const auto& settings = Settings::EditorSettings::GetGameViewSettingsObject();
    const auto view = Core::GameViewPresentation::Resolve({}, available,
        static_cast<Core::GameViewSizeMode>(std::clamp(settings.sizeMode, 0, 2)),
        std::clamp(settings.width, 1, 8192), std::clamp(settings.height, 1, 8192),
        GameViewFramebufferScale(), settings.lowResolution, true, 1, {});
    return {static_cast<uint16_t>(view.width), static_cast<uint16_t>(view.height)};
}

void Editor::Panels::GameView::DrawControls()
{
    auto& settings = Settings::EditorSettings::GetGameViewSettingsObject();
    bool changed = false;
    ImGui::SetNextItemWidth(100);
    changed |= ImGui::Combo("##Display", &settings.display, "Display 1\0Display 2\0Display 3\0Display 4\0Display 5\0Display 6\0Display 7\0Display 8\0");
    ImGui::SameLine();
    const char* label = settings.sizeMode == 0 ? "Free Aspect" : settings.sizeMode == 1 ? "Aspect Ratio" : "Fixed Resolution";
    ImGui::SetNextItemWidth(140);
    if (ImGui::BeginCombo("##Resolution", label))
    {
        struct Preset { const char* name; int mode, width, height; };
        const Preset presets[] = {{"Free Aspect",0,16,9},{"16:9",1,16,9},{"4:3",1,4,3},{"9:16",1,9,16},
            {"1280 x 720",2,1280,720},{"1920 x 1080",2,1920,1080},{"3840 x 2160",2,3840,2160}};
        for (const auto& preset : presets)
            if (ImGui::Selectable(preset.name))
            { settings.sizeMode=preset.mode; settings.width=preset.width; settings.height=preset.height; changed=true; m_pan={}; }
        ImGui::Separator();
        ImGui::SetNextItemWidth(120); changed |= ImGui::InputInt("Width", &settings.width);
        ImGui::SetNextItemWidth(120); changed |= ImGui::InputInt("Height", &settings.height);
        changed |= ImGui::RadioButton("Custom aspect", &settings.sizeMode, 1);
        changed |= ImGui::RadioButton("Custom pixels", &settings.sizeMode, 2);
        changed |= ImGui::Checkbox("Low Resolution Aspect Ratios", &settings.lowResolution);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Fit", &settings.fit);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    if (ImGui::SliderFloat("##Scale", &settings.scale, 0.1f, 8, "%.2fx")) { settings.fit=false; changed=true; }
    ImGui::SameLine();
    if (ImGui::Button("1:1")) { settings.fit=false; settings.scale=1; m_pan={}; changed=true; }
    ImGui::SameLine();
    if (ImGui::Button("Options")) ImGui::OpenPopup("GameViewOptions");
    if (ImGui::BeginPopup("GameViewOptions"))
    {
        changed |= ImGui::Checkbox("Maximize on Play", &settings.maximizeOnPlay);
        changed |= ImGui::Checkbox("Mute Audio", &settings.muteAudio);
        changed |= ImGui::Checkbox("Editor Shortcuts", &settings.editorShortcuts);
        changed |= ImGui::Checkbox("VSync (Game preview)", &settings.vsync);
        ImGui::TextWrapped("Preview VSync applies to the Editor window. Project VSync is restored when Play stops.");
        const auto effective=Render::Context::DriverRendererAccess::GetEffectivePresentVsync(*EDITOR_CONTEXT(driver));
        ImGui::Text("Effective Editor VSync: %s",effective?(*effective?"On":"Off"):"Unavailable");
        changed |= ImGui::Checkbox("Stats", &settings.stats);
        changed |= ImGui::Checkbox("Gizmos", &settings.gizmos);
        if (settings.gizmos)
        {
            changed |= ImGui::Checkbox("Cameras", &settings.cameraGizmos);
            changed |= ImGui::Checkbox("Lights", &settings.lightGizmos);
            changed |= ImGui::Checkbox("Colliders", &settings.colliderGizmos);
            changed |= ImGui::Checkbox("Bounds", &settings.boundsGizmos);
        }
        changed |= ImGui::Checkbox("Warn if No Cameras Rendering", &settings.warnNoCamera);
        changed |= ImGui::Checkbox("Clear Every Frame in Edit Mode", &settings.clearInEdit);
        ImGui::EndPopup();
    }
    settings.width=std::clamp(settings.width,1,8192);
    settings.height=std::clamp(settings.height,1,8192);
    settings.display=std::clamp(settings.display,0,7);
    if (changed)
    {
        if(EDITOR_EXEC(GetPlayModeController()).IsActive())
            Render::Context::DriverRendererAccess::SetPreviewVsyncOverride(*EDITOR_CONTEXT(driver),settings.vsync?std::optional<bool>(true):std::nullopt);
        Settings::EditorSettingsRegistry registry;
        Settings::EditorSettings::RegisterSettingObjects(registry);
        Settings::EditorSettingsPersistence::Save(std::filesystem::path(EDITOR_CONTEXT(projectPath))/"UserSettings"/"editor-settings.json", registry);
        InvalidateStaticFrameCache();
    }
}

void Editor::Panels::GameView::OnBeforeDrawWidgets()
{
    DrawControls();
    ImGui::BeginChild("GameCanvas", ImVec2(0,0), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    m_canvasStarted = true;
    const auto origin = ImGui::GetCursorScreenPos();
    const auto available = ImGui::GetContentRegionAvail();
    ImGui::GetWindowDrawList()->AddRectFilled(origin, {origin.x+available.x,origin.y+available.y}, IM_COL32(0,0,0,255));
    AView::OnBeforeDrawWidgets();
    auto size=GetSafeSize();
    if (m_image->textureView && m_image->textureView->GetTexture())
    {
        const auto& desc=m_image->textureView->GetTexture()->GetDesc();
        size={static_cast<uint16_t>(desc.extent.width),static_cast<uint16_t>(desc.extent.height)};
    }
    auto& settings=Settings::EditorSettings::GetGameViewSettingsObject();
    if (EDITOR_EXEC(GetCurrentEditorMode()) != Core::EditorActions::EEditorMode::PLAY && ImGui::IsWindowHovered())
    {
        if (ImGui::GetIO().MouseWheel != 0)
        { settings.fit=false; settings.scale=std::clamp(settings.scale*std::pow(1.15f,ImGui::GetIO().MouseWheel),0.1f,8.0f); }
        if (!settings.fit && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
            m_pan += Maths::Vector2{ImGui::GetIO().MouseDelta.x,ImGui::GetIO().MouseDelta.y};
    }
    m_presentation=Core::GameViewPresentation::Resolve({origin.x,origin.y},{available.x,available.y},
        Core::GameViewSizeMode::FixedResolution,size.first,size.second,GameViewFramebufferScale(),false,settings.fit,settings.scale,m_pan);
    m_presentation.generation=m_outputFrame;
    m_presentation.sessionId=m_outputSession;
    m_presentation.display=m_outputDisplay;
    m_image->size=m_presentation.imageMax-m_presentation.imageMin;
    ImGui::SetCursorScreenPos({m_presentation.imageMin.x,m_presentation.imageMin.y});
}

void Editor::Panels::GameView::OnAfterDrawWidgets()
{
    AView::OnAfterDrawWidgets();
    const auto& settings=Settings::EditorSettings::GetGameViewSettingsObject();
    if(settings.gizmos)DrawGizmos();
    const auto min=m_presentation.clipMin;
    if (!GetCamera() && settings.warnNoCamera)
        ImGui::GetWindowDrawList()->AddText({min.x+12,min.y+12},IM_COL32(240,200,100,255),"No cameras rendering");
    if (settings.stats)
    {
        char text[256];
        const auto& session=EDITOR_EXEC(GetRuntimePlaySession());
        const auto& frame=session.GetFrame();
        char draws[96];
        if(m_outputStats)std::snprintf(draws,sizeof(draws),"Draws: %llu | Triangles: %llu | Vertices: %llu",
            static_cast<unsigned long long>(m_outputStats->batchCount),static_cast<unsigned long long>(m_outputStats->polyCount),static_cast<unsigned long long>(m_outputStats->vertexCount));
        else std::snprintf(draws,sizeof(draws),"Draws: \xe2\x80\x94 | Triangles: \xe2\x80\x94 | Vertices: \xe2\x80\x94");
        std::snprintf(text,sizeof(text),"Game: %.1f FPS | %u x %u\nCPU: %.2f ms | GPU: \xe2\x80\x94 | Audio Voices: %zu\n%s",
            frame.unscaledDeltaTime>0 ? 1.0/frame.unscaledDeltaTime : 0.0,m_presentation.width,m_presentation.height,
            session.GetLastTickMilliseconds()+m_lastRenderMilliseconds,session.GetAudioVoiceCount(),draws);
        ImGui::GetWindowDrawList()->AddText({min.x+12,min.y+38},IM_COL32_WHITE,text);
    }
    if (m_canvasStarted) { ImGui::EndChild(); m_canvasStarted=false; }
}
Editor::Panels::GameView::GameView
(
	const std::string & p_title,
	bool p_opened,
	const UI::PanelWindowSettings & p_windowSettings
) :
	AView(p_title, p_opened, p_windowSettings),
	m_sceneManager(EDITOR_CONTEXT(sceneManager))
{
    SetRequiresRetiredFrameConsumption(true);
}

void Editor::Panels::GameView::EnsureRenderer()
{
    if (m_renderer != nullptr)
        return;
    if (!IsSceneRendererAvailable())
        return;

    NLS_LOG_INFO("[Startup] Creating Game View renderer");
    m_renderer = Engine::Rendering::CreateSceneRenderer(*EDITOR_CONTEXT(driver),Engine::Rendering::SceneRendererKind::Forward);
}

Render::Entities::Camera* Editor::Panels::GameView::GetCamera()
{
    if(m_renderingOutput)return &m_outputCamera;
    auto* scene=m_sceneManager.GetCurrentScene();
    if(!scene)return nullptr;
    const auto output=Engine::Rendering::GameOutput::Build(*scene,Settings::EditorSettings::GetGameViewSettingsObject().display,m_lastResolvedViewSize.first,m_lastResolvedViewSize.second);
    return output.cameras.empty()?nullptr:output.cameras.front()->GetRenderCamera();
}

Engine::SceneSystem::Scene* Editor::Panels::GameView::GetScene()
{
	return m_sceneManager.GetCurrentScene();
}

Engine::Rendering::BaseSceneRenderer::SceneDescriptor Editor::Panels::GameView::CreateSceneDescriptor()
{
	auto descriptor = AView::CreateSceneDescriptor();
	descriptor.trustSceneRenderContentRevision = true;
    descriptor.skipSceneDrawables=m_backgroundPass;
    descriptor.includeSkyboxes=m_includeSkybox;
	return descriptor;
}

void Editor::Panels::GameView::ConfigureFrameDescriptor(Render::Data::FrameDescriptor& descriptor)
{
    if(!m_renderingOutput)return;
    descriptor.outputViewport=m_outputViewport;
    descriptor.renderWidth=static_cast<uint16_t>(m_outputViewport.z);
    descriptor.renderHeight=static_cast<uint16_t>(m_outputViewport.w);
    auto& session=EDITOR_EXEC(GetRuntimePlaySession());
    if(session.IsActive())
    {
        descriptor.playSessionId=session.GetSessionId();
        descriptor.simulationFrame=session.GetFrame().frameIndex;
        descriptor.gameTime=session.GetFrame().time;
        if(m_lastCameraPass)
            descriptor.gameUiSnapshot=Render::UI::AdaptUIDrawSnapshot(session.GetUI().BuildScreenSnapshot(m_renderOutputWidth,m_renderOutputHeight,1,session.GetFrame().frameIndex,Settings::EditorSettings::GetGameViewSettingsObject().display));
    }
    else if(m_lastCameraPass)descriptor.gameUiSnapshot=m_previewSnapshot;
}

void Editor::Panels::GameView::Render(uint16_t width,uint16_t height)
{
    auto* scene=GetScene();
    if(!scene || !width || !height || !IsSceneRendererAvailable())return;
    const auto& settings=Settings::EditorSettings::GetGameViewSettingsObject();
    const auto output=Engine::Rendering::GameOutput::Build(*scene,settings.display,width,height);
    m_renderOutputWidth=width;m_renderOutputHeight=height;
    const auto renderStarted=std::chrono::steady_clock::now();
    const auto& session=EDITOR_EXEC(GetRuntimePlaySession());
    const auto sessionId=session.IsActive()?session.GetSessionId():0;
    const auto sceneEpoch=static_cast<uint64_t>(scene->GetInstanceID());
    if(m_outputSession!=sessionId || m_outputSceneEpoch!=sceneEpoch || m_outputDisplay!=output.display || m_outputWidth!=width || m_outputHeight!=height)
        m_outputNeedsClear=true;
    Render::Data::FrameInfo totalStats{};
    bool hasStats=true,allPublished=true;
    const size_t count=output.cameras.size()+1;
    if(m_cameraRenderers.size()+1<count)m_cameraRenderers.resize(count-1);
    const bool edit=!EDITOR_EXEC(GetPlayModeController()).IsActive();
    if(edit)
    {
        m_previewUI.SyncCanvases(scene->GetFastAccessComponents().uiCanvases);
        for(auto* canvas:m_previewUI.GetCanvases())
            if(canvas && !canvas->GetDocument() && !canvas->GetDocumentPath().empty())
            {
                const auto path=Engine::UI::ResolveProjectUIDocumentPath(EDITOR_CONTEXT(projectAssetsPath),canvas->GetDocumentPath());
                if(!path.empty())canvas->LoadDocumentResolvedPath(path,nullptr);
            }
        m_previewUI.SetInputDisplay(output.display);
        m_previewUI.ConfigureOutput(output.display,{float(width),float(height)},1);
        m_previewUI.Tick(0,{float(width),float(height)},1,0);
        m_previewSnapshot=Render::UI::AdaptUIDrawSnapshot(m_previewUI.GetLatestSnapshot());
    }
    const bool clear=m_outputNeedsClear || (edit && settings.clearInEdit) || output.cameras.empty();
    // Separate renderer capture state per camera; all packages retain the same immutable attachments.
    m_renderingOutput=true;
    for(size_t index=0;index<count;++index)
    {
        m_backgroundPass=index==0;
        m_lastCameraPass=index+1==count;
        m_includeSkybox=false;
        // A retained output does not need a background pass when cameras
        // follow it. Publishing an empty pass fails the offscreen submission.
        if (m_backgroundPass && !clear && !m_lastCameraPass)
            continue;
        if(m_backgroundPass)
        {
            m_outputCamera=Render::Entities::Camera{};
            m_outputCamera.SetClearColor({0,0,0});
            m_outputCamera.SetClearColorBuffer(clear);
            m_outputCamera.SetClearDepthBuffer(clear);
            m_outputCamera.SetClearStencilBuffer(clear);
            m_outputViewport={0,0,float(width),float(height)};
        }
        else
        {
            auto* camera=output.cameras[index-1];
            m_outputCamera=*camera->GetRenderCamera();
            const auto mode=camera->GetClearMode();
            m_includeSkybox=mode==Engine::Components::CameraComponent::ECameraClearMode::SKYBOX;
            m_outputCamera.SetClearColorBuffer(mode==Engine::Components::CameraComponent::ECameraClearMode::SKYBOX || mode==Engine::Components::CameraComponent::ECameraClearMode::SOLID_COLOR);
            m_outputCamera.SetClearDepthBuffer(mode!=Engine::Components::CameraComponent::ECameraClearMode::NOTHING);
            m_outputCamera.SetClearStencilBuffer(mode!=Engine::Components::CameraComponent::ECameraClearMode::NOTHING);
            const auto rect=camera->GetViewportRect();
            const int x=std::clamp(int(std::round(rect.x*width)),0,int(width)-1);
            const int y=std::clamp(int(std::round((1-rect.y-rect.w)*height)),0,int(height)-1);
            m_outputViewport={float(x),float(y),float(std::max(1,std::min(int(width)-x,int(std::round(rect.z*width))))),
                float(std::max(1,std::min(int(height)-y,int(std::round(rect.w*height)))))};
        }
        if(index)m_renderer.swap(m_cameraRenderers[index-1]);
        // AView records renderer-owned snapshots and preserves the existing retirement policy.
        AView::Render(width,height);
        allPublished &= WasLastRenderFramePublished();
        const auto& stats=GetLastRenderedFrameInfoSnapshot();
        if(stats){totalStats.batchCount+=stats->batchCount;totalStats.polyCount+=stats->polyCount;totalStats.vertexCount+=stats->vertexCount;}
        else hasStats=false;
        if(index)m_renderer.swap(m_cameraRenderers[index-1]);
    }
    m_backgroundPass=false;m_lastCameraPass=false;m_renderingOutput=false;
    m_outputNeedsClear=!allPublished;
    m_outputStats=hasStats?std::optional(totalStats):std::nullopt;
    m_lastRenderMilliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-renderStarted).count();
    if(allPublished){m_outputSession=sessionId;m_outputFrame=session.GetFrame().frameIndex;m_outputSceneEpoch=sceneEpoch;m_outputDisplay=output.display;m_outputWidth=width;m_outputHeight=height;}
}

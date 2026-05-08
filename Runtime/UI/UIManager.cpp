
#include "UI/UIManager.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "Core/ServiceLocator.h"
#include "Debug/Logger.h"
#include "Profiling/Profiler.h"
#include "Rendering/RHI/Core/RHIRenderSurfaceConvention.h"
#include "Rendering/RHI/Utils/RHIUIBridge.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "UI/Icons/FontAwesomeIconFont.h"
#include "Windowing/Window.h"
#include "ImGui/backends/imgui_impl_glfw.h"
#include "ImGui/imgui_internal.h"
#include <GLFW/glfw3.h>

namespace NLS::UI
{
namespace
{
NLS::Render::RHI::NativeBackendType ToNativeBackendType(const NLS::Render::Settings::EGraphicsBackend backend)
{
    switch (backend)
    {
    case NLS::Render::Settings::EGraphicsBackend::OPENGL:
        return NLS::Render::RHI::NativeBackendType::OpenGL;
    case NLS::Render::Settings::EGraphicsBackend::VULKAN:
        return NLS::Render::RHI::NativeBackendType::Vulkan;
    case NLS::Render::Settings::EGraphicsBackend::DX12:
        return NLS::Render::RHI::NativeBackendType::DX12;
    case NLS::Render::Settings::EGraphicsBackend::DX11:
        return NLS::Render::RHI::NativeBackendType::DX11;
    case NLS::Render::Settings::EGraphicsBackend::METAL:
        return NLS::Render::RHI::NativeBackendType::Metal;
    case NLS::Render::Settings::EGraphicsBackend::NONE:
    default:
        return NLS::Render::RHI::NativeBackendType::None;
    }
}

NLS::Render::RHI::BackendType ToTaggedBackendType(const NLS::Render::RHI::NativeBackendType backend)
{
    switch (backend)
    {
    case NLS::Render::RHI::NativeBackendType::DX12:
        return NLS::Render::RHI::BackendType::DX12;
    case NLS::Render::RHI::NativeBackendType::Vulkan:
        return NLS::Render::RHI::BackendType::Vulkan;
    case NLS::Render::RHI::NativeBackendType::OpenGL:
        return NLS::Render::RHI::BackendType::OpenGL;
    case NLS::Render::RHI::NativeBackendType::Metal:
        return NLS::Render::RHI::BackendType::Metal;
    case NLS::Render::RHI::NativeBackendType::DX11:
        return NLS::Render::RHI::BackendType::DX11;
    case NLS::Render::RHI::NativeBackendType::None:
    default:
        return NLS::Render::RHI::BackendType::Unknown;
    }
}
}

ImGuiGlfwInitBackend ResolveImGuiGlfwInitBackend(const NLS::Render::Settings::EGraphicsBackend backend)
{
    switch (backend)
    {
    case NLS::Render::Settings::EGraphicsBackend::OPENGL:
        return NLS::Render::Settings::SupportsImGuiRendererBackend(backend)
            ? ImGuiGlfwInitBackend::OpenGL
            : ImGuiGlfwInitBackend::Other;
    case NLS::Render::Settings::EGraphicsBackend::VULKAN:
        return ImGuiGlfwInitBackend::Vulkan;
    case NLS::Render::Settings::EGraphicsBackend::DX11:
    case NLS::Render::Settings::EGraphicsBackend::DX12:
    case NLS::Render::Settings::EGraphicsBackend::METAL:
    case NLS::Render::Settings::EGraphicsBackend::NONE:
    default:
        return ImGuiGlfwInitBackend::Other;
    }
}

UIManager::UIManager(
    GLFWwindow* p_glfwWindow,
    NLS::Render::Settings::EGraphicsBackend p_backend,
    EStyle p_style,
    const std::string& p_glslVersion,
    const NLS::Render::RHI::NativeRenderDeviceInfo* p_nativeDeviceInfo)
    : m_glfwWindow(p_glfwWindow),
      m_backend(p_backend)
{
    ImGui::CreateContext();
    m_uiScale = ResolveWindowContentScale();

    ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true; /* Disable moving windows by dragging another thing than the title bar */
    EnableDocking(false);

    ApplyStyle(p_style);

#ifdef _WIN32
    if (auto* window = NLS::Windowing::Window::FindInstance(p_glfwWindow))
    {
        if (window->GetNativeWindowHandle() == nullptr)
        {
            const std::string message = "UIManager startup failed: GLFW did not provide a Win32 native window handle.";
            NLS_LOG_ERROR(message);
            throw std::runtime_error(message);
        }

        if (!window->HasValidNativeWindowProc())
        {
            const std::string message = "UIManager startup failed: Win32 native window procedure is not available.";
            NLS_LOG_ERROR(message);
            throw std::runtime_error(message);
        }
    }
#endif

    switch (ResolveImGuiGlfwInitBackend(m_backend))
    {
    case ImGuiGlfwInitBackend::OpenGL:
        ImGui_ImplGlfw_InitForOpenGL(p_glfwWindow, true);
        break;
    case ImGuiGlfwInitBackend::Vulkan:
        ImGui_ImplGlfw_InitForVulkan(p_glfwWindow, true);
        break;
    case ImGuiGlfwInitBackend::Other:
        ImGui_ImplGlfw_InitForOther(p_glfwWindow, true);
        break;
    }

#ifdef _WIN32
    if (auto* window = NLS::Windowing::Window::FindInstance(p_glfwWindow))
    {
        window->InstallNativeWindowProc();
    }
#endif

    // GLFW init still depends on the requested UI backend, but the renderer bridge
    // is now selected from resolved native backend identity.
    m_uiBridge = NLS::Render::RHI::CreateRHIUIBridge(
        p_glfwWindow,
        p_glslVersion,
        p_nativeDeviceInfo);
    const bool hasRendererBackend = m_uiBridge != nullptr && m_uiBridge->HasRendererBackend();

    if (m_backend != NLS::Render::Settings::EGraphicsBackend::OPENGL)
    {
        if (NLS::Render::Settings::HasCompiledOfficialImGuiBackend(m_backend))
        {
            if (!hasRendererBackend)
            {
                NLS_LOG_WARNING(
                    "Official ImGui renderer backend for " +
                    std::string(NLS::Render::Settings::ToString(m_backend)) +
                    " is available, but runtime initialization did not complete.");
            }
        }
        else
        {
            NLS_LOG_WARNING(
                "No compiled ImGui renderer backend is available for " +
                std::string(NLS::Render::Settings::ToString(m_backend)) + ".");
        }
    }
}

UIManager::~UIManager()
{
#ifdef _WIN32
    if (auto* window = NLS::Windowing::Window::FindInstance(m_glfwWindow))
        window->RestoreNativeWindowProc();
#endif

    m_uiBridge.reset();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void UIManager::BeginFrame()
{
    RefreshScale();

    if (NLS::Core::ServiceLocator::Contains<NLS::Windowing::Window>())
    {
        auto& window = NLS_SERVICE(NLS::Windowing::Window);
        if (m_ownsInfiniteCursorWrap && !m_infiniteCursorWrapRequestedThisFrame)
        {
            if (!window.IsInfiniteCursorWrapEnabled() ||
                window.GetCursorShape() == NLS::Cursor::ECursorShape::SLIDE_ARROW)
            {
                window.SetInfiniteCursorWrapEnabled(false);
                window.SetCursorShape(NLS::Cursor::ECursorShape::ARROW);
            }
            m_ownsInfiniteCursorWrap = false;
        }
        if (!m_ownsInfiniteCursorWrap && m_forcedNoMouseCursorChange)
        {
            PopCustomCursorControl();
            m_forcedNoMouseCursorChange = false;
        }

        if (m_ownsInfiniteCursorWrap)
        {
            const auto wrapCompensation = window.PollInfiniteCursorWrap();
            m_pendingInfiniteCursorWrapCompensation = ImVec2(wrapCompensation.x, wrapCompensation.y);
        }
        else
        {
            m_pendingInfiniteCursorWrapCompensation = ImVec2(0.0f, 0.0f);
        }
    }

    if (m_uiBridge != nullptr && !m_uiBridge->HasRendererBackend())
    {
        NLS_LOG_INFO("UIManager::BeginFrame: UI bridge has no renderer backend");
        m_inFrame = false;
        return;
    }

    if (GImGui != nullptr)
    {
        auto& context = *GImGui;
        if (context.FrameCount > 0 && context.FrameCountEnded != context.FrameCount)
        {
            NLS_LOG_WARNING("UIManager detected an unfinished ImGui frame; forcing EndFrame() before starting a new one.");
            ImGui::EndFrame();
        }
    }

    ImGui_ImplGlfw_NewFrame();
    if (m_uiBridge != nullptr)
        m_uiBridge->BeginFrame();

    if (NLS::Core::ServiceLocator::Contains<NLS::Windowing::Window>())
    {
        auto& window = NLS_SERVICE(NLS::Windowing::Window);
        const auto windowSize = window.GetSize();
        const auto framebufferSize = window.GetFramebufferSize();
        if (windowSize.x > 0.0f && windowSize.y > 0.0f)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(windowSize.x, windowSize.y);
            io.DisplayFramebufferScale = ImVec2(
                framebufferSize.x / windowSize.x,
                framebufferSize.y / windowSize.y);
        }
    }

    ImGui::NewFrame();
    m_infiniteCursorWrapRequestedThisFrame = false;
    if (m_pendingInfiniteCursorWrapCompensation.x != 0.0f || m_pendingInfiniteCursorWrapCompensation.y != 0.0f)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.MouseDelta.x -= m_pendingInfiniteCursorWrapCompensation.x;
        io.MouseDelta.y -= m_pendingInfiniteCursorWrapCompensation.y;
        m_pendingInfiniteCursorWrapCompensation = ImVec2(0.0f, 0.0f);
    }
    m_inFrame = true;
}

void UIManager::ApplyStyle(EStyle p_style)
{
    m_currentStyle = p_style;
    ImGuiStyle* style = &ImGui::GetStyle();

    switch (p_style)
    {
        case EStyle::IM_CLASSIC_STYLE:
            ImGui::StyleColorsClassic();
            break;
        case EStyle::IM_DARK_STYLE:
            ImGui::StyleColorsDark();
            break;
        case EStyle::IM_LIGHT_STYLE:
            ImGui::StyleColorsLight();
            break;
    }

    if (p_style == EStyle::DUNE_DARK)
    {
        style->WindowPadding = ImVec2(15, 15);
        style->WindowRounding = 5.0f;
        style->FramePadding = ImVec2(5, 5);
        style->FrameRounding = 4.0f;
        style->ItemSpacing = ImVec2(12, 8);
        style->ItemInnerSpacing = ImVec2(8, 6);
        style->IndentSpacing = 25.0f;
        style->ScrollbarSize = 15.0f;
        style->ScrollbarRounding = 9.0f;
        style->GrabMinSize = 5.0f;
        style->GrabRounding = 3.0f;

        style->Colors[ImGuiCol_Text] = ImVec4(0.80f, 0.80f, 0.83f, 1.00f);
        style->Colors[ImGuiCol_TextDisabled] = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
        style->Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
        style->Colors[ImGuiCol_ChildBg] = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
        style->Colors[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
        style->Colors[ImGuiCol_Border] = ImVec4(0.2f, 0.2f, 0.2f, 0.88f);
        style->Colors[ImGuiCol_BorderShadow] = ImVec4(0.92f, 0.91f, 0.88f, 0.00f);
        style->Colors[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
        style->Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
        style->Colors[ImGuiCol_FrameBgActive] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
        style->Colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
        style->Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.3f, 0.3f, 0.3f, 0.75f);
        style->Colors[ImGuiCol_TitleBgActive] = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
        style->Colors[ImGuiCol_MenuBarBg] = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
        style->Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
        style->Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.80f, 0.80f, 0.83f, 0.31f);
        style->Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
        style->Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
        style->Colors[ImGuiCol_CheckMark] = ImVec4(0.80f, 0.80f, 0.83f, 0.31f);
        style->Colors[ImGuiCol_SliderGrab] = ImVec4(0.80f, 0.80f, 0.83f, 0.31f);
        style->Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
        style->Colors[ImGuiCol_Button] = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
        style->Colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
        style->Colors[ImGuiCol_ButtonActive] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
        style->Colors[ImGuiCol_Header] = ImVec4(0.10f, 0.09f, 0.12f, 1.00f);
        style->Colors[ImGuiCol_HeaderHovered] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
        style->Colors[ImGuiCol_HeaderActive] = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
        style->Colors[ImGuiCol_Separator] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
        style->Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.24f, 0.23f, 0.29f, 1.00f);
        style->Colors[ImGuiCol_SeparatorActive] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
        style->Colors[ImGuiCol_ResizeGrip] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        style->Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.56f, 0.56f, 0.58f, 1.00f);
        style->Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.06f, 0.05f, 0.07f, 1.00f);
        style->Colors[ImGuiCol_PlotLines] = ImVec4(0.40f, 0.39f, 0.38f, 0.63f);
        style->Colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.25f, 1.00f, 0.00f, 1.00f);
        style->Colors[ImGuiCol_PlotHistogram] = ImVec4(0.40f, 0.39f, 0.38f, 0.63f);
        style->Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.25f, 1.00f, 0.00f, 1.00f);
        style->Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.25f, 1.00f, 0.00f, 0.43f);
        style->Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(1.00f, 0.98f, 0.95f, 0.73f);

        style->Colors[ImGuiCol_Tab] = style->Colors[ImGuiCol_TabUnfocused];
    }
    else if (p_style == EStyle::ALTERNATIVE_DARK)
    {
        style->WindowPadding = ImVec2(10, 10);
        style->WindowRounding = 0.0f;
        style->FramePadding = ImVec2(6, 4);
        style->FrameRounding = 2.0f;
        style->ItemSpacing = ImVec2(8, 6);
        style->ItemInnerSpacing = ImVec2(6, 4);
        style->IndentSpacing = 18.0f;
        style->ScrollbarSize = 12.0f;
        style->ScrollbarRounding = 2.0f;
        style->GrabMinSize = 8.0f;
        style->GrabRounding = 2.0f;
        style->TabRounding = 2.0f;
        style->ChildRounding = 0.0f;
        style->PopupRounding = 2.0f;

        style->WindowBorderSize = 1.0f;
        style->FrameBorderSize = 0.0f;
        style->PopupBorderSize = 1.0f;
        style->TabBorderSize = 0.0f;
        style->WindowMenuButtonPosition = ImGuiDir_None;

        ImVec4* colors = ImGui::GetStyle().Colors;
        const ImVec4 unityBlue = ImVec4(0.23f, 0.49f, 0.82f, 1.00f);
        const ImVec4 unityBlueHover = ImVec4(0.29f, 0.58f, 0.93f, 1.00f);
        colors[ImGuiCol_Text] = ImVec4(0.84f, 0.86f, 0.90f, 1.00f);
        colors[ImGuiCol_TextDisabled] = ImVec4(0.46f, 0.49f, 0.55f, 1.00f);
        colors[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.12f, 0.13f, 1.00f);
        colors[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.12f, 0.13f, 1.00f);
        colors[ImGuiCol_PopupBg] = ImVec4(0.13f, 0.14f, 0.15f, 1.00f);
        colors[ImGuiCol_Border] = ImVec4(0.18f, 0.19f, 0.21f, 1.00f);
        colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.17f, 0.18f, 1.00f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.22f, 0.24f, 1.00f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.25f, 0.27f, 1.00f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.13f, 0.14f, 1.00f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.16f, 0.18f, 1.00f);
        colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.12f, 0.13f, 0.14f, 1.00f);
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.12f, 0.13f, 0.14f, 1.00f);
        colors[ImGuiCol_ScrollbarBg] = ImVec4(0.10f, 0.11f, 0.12f, 1.00f);
        colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.24f, 0.25f, 0.27f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.31f, 0.33f, 0.36f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.38f, 0.40f, 0.43f, 1.00f);
        colors[ImGuiCol_CheckMark] = unityBlue;
        colors[ImGuiCol_SliderGrab] = unityBlue;
        colors[ImGuiCol_SliderGrabActive] = unityBlueHover;
        colors[ImGuiCol_Button] = ImVec4(0.18f, 0.19f, 0.21f, 1.00f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.25f, 0.27f, 1.00f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.28f, 0.30f, 0.33f, 1.00f);
        colors[ImGuiCol_Header] = ImVec4(0.18f, 0.19f, 0.21f, 1.00f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.23f, 0.24f, 0.26f, 1.00f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.26f, 0.28f, 0.31f, 1.00f);
        colors[ImGuiCol_Separator] = ImVec4(1.00f, 1.00f, 1.00f, 0.08f);
        colors[ImGuiCol_SeparatorHovered] = unityBlue;
        colors[ImGuiCol_SeparatorActive] = unityBlueHover;
        colors[ImGuiCol_ResizeGrip] = ImVec4(0.26f, 0.59f, 0.98f, 0.00f);
        colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.26f, 0.59f, 0.98f, 0.00f);
        colors[ImGuiCol_ResizeGripActive] = ImVec4(0.26f, 0.59f, 0.98f, 0.00f);
        colors[ImGuiCol_Tab] = ImVec4(0.16f, 0.17f, 0.18f, 1.00f);
        colors[ImGuiCol_TabHovered] = ImVec4(0.22f, 0.23f, 0.25f, 1.00f);
        colors[ImGuiCol_TabActive] = ImVec4(0.24f, 0.26f, 0.29f, 1.00f);
        colors[ImGuiCol_TabUnfocused] = ImVec4(0.13f, 0.14f, 0.15f, 1.00f);
        colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.18f, 0.19f, 0.21f, 1.00f);
        colors[ImGuiCol_DockingPreview] = ImVec4(0.23f, 0.49f, 0.82f, 0.55f);
        colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.10f, 0.11f, 0.12f, 1.00f);
        colors[ImGuiCol_PlotLines] = ImVec4(0.96f, 0.96f, 0.99f, 1.00f);
        colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.12f, 1.00f, 0.12f, 1.00f);
        colors[ImGuiCol_PlotHistogram] = ImVec4(0.96f, 0.96f, 0.99f, 1.00f);
        colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.12f, 1.00f, 0.12f, 1.00f);
        colors[ImGuiCol_TextSelectedBg] = ImVec4(0.23f, 0.49f, 0.82f, 0.35f);
        colors[ImGuiCol_DragDropTarget] = unityBlueHover;
        colors[ImGuiCol_NavHighlight] = unityBlueHover;
        colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
        colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
        colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.35f);
    }

    style->ScaleAllSizes(m_uiScale);
}

bool UIManager::LoadFont(const std::string& p_id, const std::string& p_path, float p_fontSize)
{
    if (m_fonts.find(p_id) == m_fonts.end())
    {
        auto& io = ImGui::GetIO();
        ImFont* fontInstance = io.Fonts->AddFontFromFileTTF(p_path.c_str(), p_fontSize * m_uiScale);

        if (fontInstance)
        {
            Icons::EnsureFontAwesomeIconFontLoaded(p_fontSize * m_uiScale, fontInstance);
            m_fonts.emplace(p_id, FontEntry{ p_path, p_fontSize, fontInstance });
            return true;
        }
    }

    return false;
}

bool UIManager::UnloadFont(const std::string& p_id)
{
    if (m_fonts.find(p_id) != m_fonts.end())
    {
        if (m_currentFontId == p_id)
            m_currentFontId.clear();
        m_fonts.erase(p_id);
        return true;
    }

    return false;
}

bool UIManager::UseFont(const std::string& p_id)
{
    auto foundFont = m_fonts.find(p_id);

    if (foundFont != m_fonts.end())
    {
        m_currentFontId = p_id;
        ImGui::GetIO().FontDefault = foundFont->second.instance;
        return true;
    }

    return false;
}

void UIManager::UseDefaultFont()
{
    m_currentFontId.clear();
    ImGui::GetIO().FontDefault = nullptr;
}

void UIManager::EnableEditorLayoutSave(bool p_value)
{
    if (p_value)
        ImGui::GetIO().IniFilename = m_layoutSaveFilename.c_str();
    else
        ImGui::GetIO().IniFilename = nullptr;
}

bool UIManager::IsEditorLayoutSaveEnabled() const
{
    return ImGui::GetIO().IniFilename != nullptr;
}

void UIManager::SetEditorLayoutSaveFilename(const std::string& p_filename)
{
    m_layoutSaveFilename = p_filename;
    if (IsEditorLayoutSaveEnabled())
        ImGui::GetIO().IniFilename = m_layoutSaveFilename.c_str();
}

void UIManager::SetEditorLayoutAutosaveFrequency(float p_frequency)
{
    ImGui::GetIO().IniSavingRate = p_frequency;
}

float UIManager::GetEditorLayoutAutosaveFrequency(float p_frequeny)
{
    return ImGui::GetIO().IniSavingRate;
}

void UIManager::EnableDocking(bool p_value)
{
    m_dockingState = p_value;

    if (p_value)
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    else
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_DockingEnable;
}

void UIManager::ResetLayout(const std::string& p_config) const
{
    ImGui::LoadIniSettingsFromDisk(p_config.c_str());
}

bool UIManager::IsDockingEnabled() const
{
    return m_dockingState;
}

void UIManager::SetCanvas(Canvas& p_canvas)
{
    RemoveCanvas();

    m_currentCanvas = &p_canvas;
}

void UIManager::RemoveCanvas()
{
    m_currentCanvas = nullptr;
}

void UIManager::Render()
{
    NLS_PROFILE_SCOPE();
    if (m_currentCanvas == nullptr || m_isRenderingFrame)
        return;

    m_isRenderingFrame = true;
    {
        NLS_PROFILE_NAMED_SCOPE("UIManager::BeginFrame");
        BeginFrame();
    }
    if (!m_inFrame)
    {
        m_isRenderingFrame = false;
        return;
    }
    {
        NLS_PROFILE_NAMED_SCOPE("UIManager::DrawCanvas");
        m_currentCanvas->Draw();
    }

    // All paths: ImGui::Render() must be called before RenderDrawData to build font atlas if needed
    if (m_uiBridge != nullptr)
    {
        {
            NLS_PROFILE_NAMED_SCOPE("ImGui::Render");
            ImGui::Render();
        }
        {
            NLS_PROFILE_NAMED_SCOPE("UIBridge::RenderDrawData");
            m_uiBridge->RenderDrawData(ImGui::GetDrawData(), m_currentSwapchainImageIndex);
        }
    }
    m_isRenderingFrame = false;
}

NLS::Render::RHI::NativeHandle UIManager::ResolveTextureView(const std::shared_ptr<NLS::Render::RHI::RHITextureView>& textureView)
{
    return m_uiBridge != nullptr ? m_uiBridge->ResolveTextureView(textureView) : NLS::Render::RHI::NativeHandle{};
}

void UIManager::NotifySwapchainWillResize()
{
    if (m_uiBridge != nullptr)
        m_uiBridge->NotifySwapchainWillResize();
}

void UIManager::ReleaseTextureViewHandle(const std::shared_ptr<NLS::Render::RHI::RHITextureView>& textureView)
{
    if (m_uiBridge != nullptr)
        m_uiBridge->ReleaseTextureViewHandle(textureView);
}

void UIManager::SetWaitSemaphore(void* semaphore)
{
    waitSemaphore_ = semaphore;
    if (m_uiBridge != nullptr)
    {
        m_uiBridge->SetWaitSemaphore(semaphore);
    }
}

void UIManager::SetSignalSemaphore(void* semaphore)
{
    signalSemaphore_ = semaphore;
    if (m_uiBridge != nullptr)
    {
        m_uiBridge->SetSignalSemaphore(semaphore);
    }
}

void UIManager::SubmitUIRendering()
{
    NLS_PROFILE_SCOPE();
    if (m_uiBridge != nullptr)
    {
        m_uiBridge->SubmitCommandBuffer(m_currentSwapchainImageIndex);
    }
}

NLS::Render::RHI::NativeHandle UIManager::ResolveUISignalSemaphore()
{
    if (m_uiBridge != nullptr)
    {
        void* sem = m_uiBridge->GetUISignalSemaphore();
        if (sem != nullptr)
        {
            NLS::Render::RHI::NativeHandle handle;
            handle.backend = ToTaggedBackendType(m_uiBridge->GetNativeBackendType());
            handle.handle = sem;
            return handle;
        }
    }
    return NLS::Render::RHI::NativeHandle{};
}

uint64_t UIManager::ResolveUISignalValue() const
{
    return m_uiBridge != nullptr ? m_uiBridge->GetUISignalValue() : 0u;
}

bool UIManager::ShouldFlipPresentedRenderTargetVertically() const
{
    return NLS::Render::RHI::GetRenderSurfaceConvention(ToNativeBackendType(m_backend))
        .RequiresPresentedTextureVerticalFlip();
}

bool UIManager::UsesBottomLeftRenderTargetOrigin() const
{
    return NLS::Render::RHI::GetRenderSurfaceConvention(ToNativeBackendType(m_backend))
        .UsesBottomLeftRenderTargetOrigin();
}

float UIManager::GetScale() const
{
    return m_uiScale;
}

float UIManager::Scale(const float p_value) const
{
    return p_value * m_uiScale;
}

ImVec2 UIManager::Scale(const ImVec2& p_value) const
{
    return ImVec2(p_value.x * m_uiScale, p_value.y * m_uiScale);
}

float UIManager::ResolveWindowContentScale() const
{
    if (m_glfwWindow == nullptr)
        return 1.0f;

    float xScale = 1.0f;
    float yScale = 1.0f;
    glfwGetWindowContentScale(m_glfwWindow, &xScale, &yScale);

    const float scale = std::max(xScale, yScale);
    return std::clamp(scale, 0.75f, 3.0f);
}

void UIManager::RefreshScale()
{
    const float resolvedScale = ResolveWindowContentScale();
    if (std::fabs(resolvedScale - m_uiScale) < 0.01f)
        return;

    m_uiScale = resolvedScale;
    ApplyStyle(m_currentStyle);
    RebuildFonts();
}

void UIManager::RebuildFonts()
{
    auto& io = ImGui::GetIO();
    io.Fonts->Clear();

    for (auto& [id, font] : m_fonts)
    {
        font.instance = io.Fonts->AddFontFromFileTTF(font.path.c_str(), font.baseSize * m_uiScale);
        if (font.instance != nullptr)
            Icons::EnsureFontAwesomeIconFontLoaded(font.baseSize * m_uiScale, font.instance);
    }

    if (!m_currentFontId.empty())
    {
        auto foundFont = m_fonts.find(m_currentFontId);
        io.FontDefault = foundFont != m_fonts.end() ? foundFont->second.instance : nullptr;
    }
    else
    {
        io.FontDefault = nullptr;
    }

    io.Fonts->Build();
    if (m_uiBridge != nullptr)
        m_uiBridge->NotifyFontAtlasChanged();
}

void UIManager::PushCurrentFont()
{
}

void UIManager::PopCurrentFont()
{
}

bool UIManager::BeginDragDropTarget()
{
    return ImGui::BeginDragDropTarget();
}

void UIManager::EndDragDropTarget()
{
    ImGui::EndDragDropTarget();
}

const ImGuiPayload* UIManager::AcceptDragDropPayload(const char* type, ImGuiDragDropFlags flags)
{
    return ImGui::AcceptDragDropPayload(type, flags);
}

void UIManager::PushStyleVar(ImGuiStyleVar idx, const ImVec2& val)
{
    ImGui::PushStyleVar(idx, val);
}

void UIManager::PopStyleVar(int count)
{
    ImGui::PopStyleVar(count);
}

bool UIManager::BeginDragDropSource(ImGuiDragDropFlags flags /*= 0*/)
{
    return ImGui::BeginDragDropSource(flags);
}

void UIManager::EndDragDropSource()
{
    ImGui::EndDragDropSource();
}

bool UIManager::SetDragDropPayload(const char* type, const void* data, size_t sz, ImGuiCond cond /*= 0*/)
{
    return ImGui::SetDragDropPayload(type, data, sz, cond);
}

bool UIManager::IsAnyItemActive()
{
    return ImGui::IsAnyItemActive();
}

void UIManager::PushCustomCursorControl()
{
    if (m_customCursorControlDepth == 0)
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ++m_customCursorControlDepth;
}

void UIManager::PopCustomCursorControl()
{
    if (m_customCursorControlDepth == 0)
        return;

    --m_customCursorControlDepth;
    if (m_customCursorControlDepth == 0)
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
}

void UIManager::RequestInfiniteDragCursor(const NLS::Cursor::ECursorShape p_cursorShape)
{
    if (!NLS::Core::ServiceLocator::Contains<NLS::Windowing::Window>())
        return;

    if (!m_forcedNoMouseCursorChange)
    {
        PushCustomCursorControl();
        m_forcedNoMouseCursorChange = true;
    }

    auto& window = NLS_SERVICE(NLS::Windowing::Window);
    window.SetInfiniteCursorWrapEnabled(true);
    window.SetCursorShape(p_cursorShape);
    m_ownsInfiniteCursorWrap = true;
    m_infiniteCursorWrapRequestedThisFrame = true;
}

float UIManager::GetMouseWheel()
{
    return ImGui::GetIO().MouseWheel;
}

ImGuiMouseCursor UIManager::GetMouseCursor()
{
    return ImGui::GetMouseCursor();
}

bool UIManager::IsItemHovered(ImGuiHoveredFlags flags /*= 0*/)
{
    return ImGui::IsItemHovered(flags);
}

bool UIManager::BeginTooltip()
{
    return ImGui::BeginTooltip();
}

void UIManager::EndTooltip()
{
    return ImGui::EndTooltip();
}

} // namespace NLS::UI

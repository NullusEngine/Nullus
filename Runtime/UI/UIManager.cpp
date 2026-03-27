
#include "UI/UIManager.h"

#include <vector>

#include "Debug/Logger.h"
#include "Rendering/RHI/Utils/RHIUIBridge.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "ImGui/backends/imgui_impl_glfw.h"
#include "ImGui/imgui_internal.h"

namespace NLS::UI
{
UIManager::UIManager(
    GLFWwindow* p_glfwWindow,
    NLS::Render::Settings::EGraphicsBackend p_backend,
    const NLS::Render::RHI::NativeRenderDeviceInfo& p_nativeDeviceInfo,
    EStyle p_style,
    const std::string& p_glslVersion)
    : m_backend(p_backend)
{
    ImGui::CreateContext();

    ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true; /* Disable moving windows by dragging another thing than the title bar */
    EnableDocking(false);

    ApplyStyle(p_style);

    switch (m_backend)
    {
    case NLS::Render::Settings::EGraphicsBackend::OPENGL:
        ImGui_ImplGlfw_InitForOpenGL(p_glfwWindow, true);
        break;
    case NLS::Render::Settings::EGraphicsBackend::VULKAN:
        ImGui_ImplGlfw_InitForVulkan(p_glfwWindow, true);
        break;
    case NLS::Render::Settings::EGraphicsBackend::DX12:
        ImGui_ImplGlfw_InitForOther(p_glfwWindow, true);
        break;
    case NLS::Render::Settings::EGraphicsBackend::METAL:
    case NLS::Render::Settings::EGraphicsBackend::NONE:
    default:
        ImGui_ImplGlfw_InitForOther(p_glfwWindow, true);
        break;
    }

    m_uiBridge = NLS::Render::RHI::CreateRHIUIBridge(p_glfwWindow, m_backend, p_nativeDeviceInfo, p_glslVersion);
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
    m_uiBridge.reset();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void UIManager::BeginFrame()
{
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

    ImGui::NewFrame();
}

void UIManager::ApplyStyle(EStyle p_style)
{
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
}

bool UIManager::LoadFont(const std::string& p_id, const std::string& p_path, float p_fontSize)
{
    if (m_fonts.find(p_id) == m_fonts.end())
    {
        auto& io = ImGui::GetIO();
        ImFont* fontInstance = io.Fonts->AddFontFromFileTTF(p_path.c_str(), p_fontSize);

        if (fontInstance)
        {
            m_fonts[p_id] = fontInstance;
            return true;
        }
    }

    return false;
}

bool UIManager::UnloadFont(const std::string& p_id)
{
    if (m_fonts.find(p_id) != m_fonts.end())
    {
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
        ImGui::GetIO().FontDefault = foundFont->second;
        return true;
    }

    return false;
}

void UIManager::UseDefaultFont()
{
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
    if (m_currentCanvas == nullptr || m_isRenderingFrame)
        return;

    m_isRenderingFrame = true;
    BeginFrame();
    m_currentCanvas->Draw();
    ImGui::Render();
    if (m_uiBridge != nullptr)
        m_uiBridge->RenderDrawData(ImGui::GetDrawData());
    m_isRenderingFrame = false;
}

void* UIManager::ResolveTextureID(uint32_t textureId)
{
    return m_uiBridge != nullptr ? m_uiBridge->ResolveTextureID(textureId) : nullptr;
}

void UIManager::NotifySwapchainWillResize()
{
    if (m_uiBridge != nullptr)
        m_uiBridge->NotifySwapchainWillResize();
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

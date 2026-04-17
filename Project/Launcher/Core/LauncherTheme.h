#pragma once

#include <algorithm>

#include <UI/Internal/Converter.h>
#include <Math/Color.h>

#include "ImGui/imgui.h"

namespace NLS
{

// ─── Hub Theme Colors (Unity Hub dark palette) ───
namespace HubColors
{
    inline const Maths::Color Background      {0.118f, 0.118f, 0.118f, 1.0f};
    inline const Maths::Color Surface         {0.157f, 0.157f, 0.157f, 1.0f};
    inline const Maths::Color SurfaceHover    {0.196f, 0.196f, 0.196f, 1.0f};
    inline const Maths::Color SurfaceActive   {0.235f, 0.235f, 0.235f, 1.0f};
    inline const Maths::Color Border          {0.255f, 0.255f, 0.255f, 0.12f};
    inline const Maths::Color BorderStrong    {0.420f, 0.420f, 0.420f, 0.42f};
    inline const Maths::Color Accent          {0.129f, 0.588f, 0.953f, 1.0f};
    inline const Maths::Color AccentHover     {0.169f, 0.667f, 0.988f, 1.0f};
    inline const Maths::Color AccentActive    {0.102f, 0.510f, 0.859f, 1.0f};
    inline const Maths::Color TextPrimary     {0.88f,  0.88f,  0.88f,  1.0f};
    inline const Maths::Color TextSecondary   {0.50f,  0.50f,  0.50f,  1.0f};
    inline const Maths::Color TextMuted       {0.35f,  0.35f,  0.35f,  1.0f};
    inline const Maths::Color TextDisabled    {0.28f,  0.28f,  0.28f,  1.0f};
    inline const Maths::Color Danger          {0.906f, 0.298f, 0.235f, 1.0f};
    inline const Maths::Color RowOdd          {0.133f, 0.133f, 0.133f, 1.0f};
    inline const Maths::Color RowHover        {0.145f, 0.145f, 0.152f, 1.0f};
    inline const Maths::Color RowSelected     {0.110f, 0.165f, 0.220f, 1.0f};
    inline const Maths::Color InputBg         {0.12f,  0.12f,  0.12f,  1.0f};
    inline const Maths::Color InputBgHover    {0.14f,  0.14f,  0.14f,  1.0f};
    inline const Maths::Color InputBgActive   {0.16f,  0.16f,  0.16f,  1.0f};
    inline const Maths::Color SearchBorder    {0.460f, 0.460f, 0.460f, 0.55f};
    inline const Maths::Color SearchBorderActive {0.129f, 0.588f, 0.953f, 0.95f};
}

namespace HubLayout
{
    inline constexpr float kDefaultWindowWidth = 1280.0f;
    inline constexpr float kDefaultWindowHeight = 760.0f;
    inline constexpr float kMinimumWindowWidth = 1040.0f;
    inline constexpr float kMinimumWindowHeight = 640.0f;
    inline constexpr float kBrandRailWidth = 68.0f;
    inline constexpr float kNavigationWidth = 232.0f;
    inline constexpr float kActionBarHeight = 132.0f;
    inline constexpr float kTableHeaderHeight = 36.0f;
    inline constexpr float kProjectRowHeight = 64.0f;
    inline constexpr float kProjectTablePadding = 24.0f;
    inline constexpr float kWizardHeaderHeight = 72.0f;
    inline constexpr float kWizardFooterHeight = 64.0f;

    struct ProjectTableColumns
    {
        float name = 360.0f;
        float modified = 176.0f;
        float backend = 180.0f;
        float actions = 72.0f;
    };

    struct WizardColumns
    {
        float category = 180.0f;
        float templates = 680.0f;
        float preview = 360.0f;
    };

    inline ProjectTableColumns CalculateProjectTableColumns(float availableWidth)
    {
        ProjectTableColumns columns;
        const bool compact = availableWidth < 920.0f;
        columns.modified = compact ? 136.0f : 176.0f;
        columns.backend = compact ? 136.0f : 180.0f;
        columns.actions = 72.0f;

        const float reserved = kProjectTablePadding + columns.modified + columns.backend + columns.actions;
        columns.name = (std::max)(compact ? 280.0f : 360.0f, availableWidth - reserved);
        return columns;
    }

    inline WizardColumns CalculateWizardColumns(float availableWidth)
    {
        WizardColumns columns;
        columns.category = availableWidth >= 1160.0f ? 180.0f : 150.0f;
        columns.preview = availableWidth >= 1160.0f ? 360.0f : 300.0f;
        columns.templates = availableWidth - columns.category - columns.preview;

        if (columns.templates < 500.0f)
        {
            const float deficit = 500.0f - columns.templates;
            columns.preview = (std::max)(280.0f, columns.preview - deficit);
            columns.templates = availableWidth - columns.category - columns.preview;
        }

        return columns;
    }
}

// ─── Theme Utilities ───

inline ImU32 HubColorU32(const Maths::Color& color)
{
    return ImGui::GetColorU32(UI::Internal::Converter::ToImVec4(color));
}

inline bool DrawHubButton(const char* label, const ImVec2& size,
    const Maths::Color& bg, const Maths::Color& hover,
    const Maths::Color& active, const Maths::Color& textColor)
{
    ImGui::PushStyleColor(ImGuiCol_Button,        UI::Internal::Converter::ToImVec4(bg));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  UI::Internal::Converter::ToImVec4(hover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   UI::Internal::Converter::ToImVec4(active));
    ImGui::PushStyleColor(ImGuiCol_Text,           UI::Internal::Converter::ToImVec4(textColor));
    ImGui::PushStyleColor(ImGuiCol_Border,         UI::Internal::Converter::ToImVec4(Maths::Color(0,0,0,0)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
    return clicked;
}

inline void PushHubInputStyle()
{
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        UI::Internal::Converter::ToImVec4(HubColors::InputBg));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,  UI::Internal::Converter::ToImVec4(HubColors::InputBgHover));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,   UI::Internal::Converter::ToImVec4(HubColors::InputBgActive));
    ImGui::PushStyleColor(ImGuiCol_Text,            UI::Internal::Converter::ToImVec4(HubColors::TextPrimary));
    ImGui::PushStyleColor(ImGuiCol_Border,          UI::Internal::Converter::ToImVec4(HubColors::Border));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));
}

inline void PopHubInputStyle()
{
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
}

inline void PushHubText(const Maths::Color& color)
{
    ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4(color));
}

inline void PopHubText()
{
    ImGui::PopStyleColor();
}

} // namespace NLS

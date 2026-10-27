#include "Icons/IconPainter.h"

#include <algorithm>
#include <cmath>

#include "Core/ServiceLocator.h"
#include "Rendering/UI/UiDrawDataSnapshot.h"
#include "UI/UIManager.h"

namespace NLS::Editor::Icons
{
namespace
{
    ImTextureID ToImGuiTexture(const Render::UI::UiTextureId p_id)
    {
        if (!p_id.IsValid())
            return nullptr;
        return reinterpret_cast<ImTextureID>(
            static_cast<std::uintptr_t>(Render::UI::PackUiTextureIdForImGui(p_id)));
    }

    ImU32 ToImU32(const std::uint32_t p_color)
    {
        return static_cast<ImU32>(p_color);
    }

    ImVec2 IconDisplaySize(const IconToken p_token, const IconMetrics& p_metrics)
    {
        const float size = static_cast<float>(IconTokenToLogicalPixels(p_token)) *
            (p_metrics.uiScale > 0.0f ? p_metrics.uiScale : 1.0f);
        return ImVec2(size, size);
    }

    ImVec4 ToImVec4(const std::uint32_t p_color)
    {
        return ImGui::ColorConvertU32ToFloat4(ToImU32(p_color));
    }
}

IconMetrics CurrentIconMetrics()
{
    IconMetrics metrics;
    if (NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
    {
        metrics.uiScale = NLS::Core::ServiceLocator::Get<NLS::UI::UIManager>().GetScale();
    }

    const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
    metrics.framebufferScale = framebufferScale.x > 0.0f ? framebufferScale.x : 1.0f;
    return metrics;
}

std::uint32_t ComputeIconTargetPixels(const float p_logicalPixels, const IconMetrics& p_metrics)
{
    const float uiScale = p_metrics.uiScale > 0.0f ? p_metrics.uiScale : 1.0f;
    const float framebufferScale = p_metrics.framebufferScale > 0.0f ? p_metrics.framebufferScale : 1.0f;
    const long computed = std::lround(p_logicalPixels * uiScale * framebufferScale);
    return static_cast<std::uint32_t>((std::max)(computed, 1L));
}

IconPainter::IconPainter(IconService& p_service)
    : m_service(p_service)
{
}

void IconPainter::BeginFrame()
{
    const int frame = ImGui::GetFrameCount();
    if (frame == m_metricsFrame)
        return;

    m_metricsFrame = frame;
    m_metrics = CurrentIconMetrics();
    m_service.SetFrameId(static_cast<std::uint64_t>(frame));
    m_service.TickRetirement();
}

bool IconPainter::Button(
    const char* p_widgetId,
    const IconId p_icon,
    const IconToken p_token,
    const IconButtonOptions& p_options)
{
    BeginFrame();

    const ImVec2 iconSize = IconDisplaySize(p_token, m_metrics);
    const std::uint32_t targetPixels = ComputeIconTargetPixels(
        static_cast<float>(IconTokenToLogicalPixels(p_token)), m_metrics);
    const IconLoadResult resolved = m_service.Resolve(p_icon, targetPixels);
    const ImTextureID texture = ToImGuiTexture(resolved.icon.texture);

    std::uint32_t tintColor = IconStateColor(
        p_options.danger ? IconState::Danger
            : (p_options.selected ? IconState::Selected : IconState::Normal),
        resolved.icon.tintable);
    if (!p_options.enabled)
        tintColor = IconStateColor(IconState::Disabled, resolved.icon.tintable);

    if (!p_options.enabled)
        ImGui::BeginDisabled();

    if (p_options.selected)
    {
        const ImVec4 highlight = ToImVec4(GetEditorIconTheme().accent);
        ImGui::PushStyleColor(ImGuiCol_Button, highlight);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, highlight);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, highlight);
    }

    bool clicked = false;
    if (texture != nullptr)
    {
        clicked = ImGui::ImageButton(
            p_widgetId,
            texture,
            iconSize,
            ImVec2(resolved.icon.uvMin.x, resolved.icon.uvMin.y),
            ImVec2(resolved.icon.uvMax.x, resolved.icon.uvMax.y),
            ImVec4(0.0f, 0.0f, 0.0f, 0.0f),
            ToImVec4(tintColor));
    }
    else
    {
        const GlyphIcon glyph = m_service.ResolveGlyph(p_icon, iconSize.x);
        if (glyph.valid)
        {
            clicked = ImGui::Button(p_widgetId, ImVec2(iconSize.x + ImGui::GetStyle().FramePadding.x * 2.0f,
                iconSize.y + ImGui::GetStyle().FramePadding.y * 2.0f));
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            const ImVec2 textSize = ImGui::CalcTextSize(glyph.utf8);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(min.x + (max.x - min.x - textSize.x) * 0.5f,
                    min.y + (max.y - min.y - textSize.y) * 0.5f),
                ToImU32(tintColor),
                glyph.utf8);
        }
        else
        {
            clicked = ImGui::Button(p_widgetId, iconSize);
        }
    }

    if (p_options.selected)
        ImGui::PopStyleColor(3);

    if (p_options.tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", p_options.tooltip);

    if (!p_options.enabled)
    {
        ImGui::EndDisabled();
        return false;
    }

    return clicked;
}

bool IconPainter::Toggle(
    const char* p_widgetId,
    const IconId p_icon,
    const IconToken p_token,
    const bool p_value,
    const IconButtonOptions& p_options)
{
    IconButtonOptions options = p_options;
    options.selected = p_value;
    const bool clicked = Button(p_widgetId, p_icon, p_token, options);
    return clicked ? !p_value : p_value;
}

bool IconPainter::TextButton(
    const char* p_widgetId,
    const IconId p_icon,
    const char* p_label,
    const IconToken p_token,
    const IconButtonOptions& p_options)
{
    BeginFrame();

    const ImVec2 iconSize = IconDisplaySize(p_token, m_metrics);
    const ImVec2 labelSize = ImGui::CalcTextSize(p_label);
    const ImVec2 framePadding = ImGui::GetStyle().FramePadding;
    const ImVec2 buttonSize(
        iconSize.x + labelSize.x + framePadding.x * 3.0f,
        (std::max)(iconSize.y + framePadding.y * 2.0f, iconSize.y));

    if (!p_options.enabled)
        ImGui::BeginDisabled();

    if (p_options.selected)
    {
        const ImVec4 highlight = ToImVec4(GetEditorIconTheme().accent);
        ImGui::PushStyleColor(ImGuiCol_Button, highlight);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, highlight);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, highlight);
    }

    const bool clicked = ImGui::Button(p_widgetId, buttonSize);

    if (p_options.selected)
        ImGui::PopStyleColor(3);

    const ImVec2 buttonMin = ImGui::GetItemRectMin();
    const ImVec2 buttonMax = ImGui::GetItemRectMax();

    std::uint32_t tintColor = IconStateColor(
        p_options.danger ? IconState::Danger
            : (p_options.selected ? IconState::Selected : IconState::Normal),
        true);
    if (!p_options.enabled)
        tintColor = IconStateColor(IconState::Disabled, true);

    const std::uint32_t targetPixels = ComputeIconTargetPixels(
        static_cast<float>(IconTokenToLogicalPixels(p_token)), m_metrics);
    const IconLoadResult resolved = m_service.Resolve(p_icon, targetPixels);
    if (const ImTextureID texture = ToImGuiTexture(resolved.icon.texture); texture != nullptr)
    {
        const ImVec2 iconPos(
            buttonMin.x + framePadding.x,
            buttonMin.y + (buttonMax.y - buttonMin.y - iconSize.y) * 0.5f);
        ImGui::GetWindowDrawList()->AddImage(
            texture,
            iconPos,
            ImVec2(iconPos.x + iconSize.x, iconPos.y + iconSize.y),
            ImVec2(resolved.icon.uvMin.x, resolved.icon.uvMin.y),
            ImVec2(resolved.icon.uvMax.x, resolved.icon.uvMax.y),
            ToImU32(tintColor));
    }

    const ImVec2 textPos(
        buttonMin.x + framePadding.x + iconSize.x + framePadding.x,
        buttonMin.y + (buttonMax.y - buttonMin.y - labelSize.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddText(
        textPos,
        ImGui::GetColorU32(p_options.enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled),
        p_label);

    if (p_options.tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", p_options.tooltip);

    if (!p_options.enabled)
    {
        ImGui::EndDisabled();
        return false;
    }

    return clicked;
}

bool IconPainter::MenuItem(
    const char* p_widgetId,
    const IconId p_icon,
    const char* p_label,
    const char* p_shortcut,
    const bool p_selected,
    const bool p_enabled)
{
    BeginFrame();

    ImGui::PushID(p_widgetId);
    const float rowHeight = ImGui::GetFrameHeight();
    const float iconSize = static_cast<float>(IconTokenToLogicalPixels(IconToken::Small)) *
        (m_metrics.uiScale > 0.0f ? m_metrics.uiScale : 1.0f);
    const float padding = ImGui::GetStyle().FramePadding.x;

    if (!p_enabled)
        ImGui::BeginDisabled();

    const bool clicked = ImGui::Selectable(
        "##icon-menu-row",
        p_selected,
        ImGuiSelectableFlags_None,
        ImVec2(0.0f, rowHeight));

    if (!p_enabled)
        ImGui::EndDisabled();

    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    const std::uint32_t targetPixels = ComputeIconTargetPixels(
        static_cast<float>(IconTokenToLogicalPixels(IconToken::Small)), m_metrics);
    const IconLoadResult resolved = m_service.Resolve(p_icon, targetPixels);
    std::uint32_t tintColor = IconStateColor(
        p_enabled ? (p_selected ? IconState::Selected : IconState::Normal) : IconState::Disabled,
        resolved.icon.tintable);

    if (const ImTextureID texture = ToImGuiTexture(resolved.icon.texture); texture != nullptr)
    {
        const ImVec2 iconPos(
            itemMin.x + padding,
            itemMin.y + (itemMax.y - itemMin.y - iconSize) * 0.5f);
        drawList->AddImage(
            texture,
            iconPos,
            ImVec2(iconPos.x + iconSize, iconPos.y + iconSize),
            ImVec2(resolved.icon.uvMin.x, resolved.icon.uvMin.y),
            ImVec2(resolved.icon.uvMax.x, resolved.icon.uvMax.y),
            ToImU32(tintColor));
    }

    const ImVec2 labelSize = ImGui::CalcTextSize(p_label);
    const ImVec2 labelPos(
        itemMin.x + padding + iconSize + padding,
        itemMin.y + (itemMax.y - itemMin.y - labelSize.y) * 0.5f);
    drawList->AddText(
        labelPos,
        ImGui::GetColorU32(p_enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled),
        p_label);

    if (p_shortcut != nullptr && p_shortcut[0] != '\0')
    {
        const ImVec2 shortcutSize = ImGui::CalcTextSize(p_shortcut);
        drawList->AddText(
            ImVec2(itemMax.x - padding - shortcutSize.x,
                itemMin.y + (itemMax.y - itemMin.y - shortcutSize.y) * 0.5f),
            ImGui::GetColorU32(ImGuiCol_TextDisabled),
            p_shortcut);
    }

    ImGui::PopID();
    return clicked && p_enabled;
}

bool IconPainter::Draw(
    ImDrawList& p_drawList,
    const ImVec2& p_topLeft,
    const ImVec2& p_size,
    const IconId p_icon,
    const IconState p_state,
    const IconMetrics& p_metrics)
{
    const float logicalSize = (std::max)(p_size.x, p_size.y);
    if (logicalSize <= 0.0f)
        return false;

    const float framebufferScale = p_metrics.framebufferScale > 0.0f
        ? p_metrics.framebufferScale
        : 1.0f;
    const auto targetPixels = static_cast<std::uint32_t>((std::max)(
        1L, std::lround(logicalSize * framebufferScale)));

    const IconLoadResult resolved = m_service.Resolve(p_icon, targetPixels);
    const ImTextureID texture = ToImGuiTexture(resolved.icon.texture);
    if (texture == nullptr)
        return false;

    p_drawList.AddImage(
        texture,
        p_topLeft,
        ImVec2(p_topLeft.x + p_size.x, p_topLeft.y + p_size.y),
        ImVec2(resolved.icon.uvMin.x, resolved.icon.uvMin.y),
        ImVec2(resolved.icon.uvMax.x, resolved.icon.uvMax.y),
        ToImU32(IconStateColor(p_state, resolved.icon.tintable)));
    return true;
}

void IconPainter::Draw(
    ImDrawList& p_drawList,
    const ImVec2& p_topLeft,
    const IconId p_icon,
    const IconToken p_token,
    const IconState p_state,
    const IconMetrics& p_metrics)
{
    Draw(p_drawList, p_topLeft, IconDisplaySize(p_token, p_metrics), p_icon, p_state, p_metrics);
}
}

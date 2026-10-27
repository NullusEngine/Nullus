#include "Panels/IconBrowser.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "ImGui/imgui.h"

#include "Core/Editor.h"
#include "Core/EditorActions.h"
#include "Core/EditorResources.h"
#include "Icons/EditorIconCatalog.h"
#include "Icons/IconPainter.h"
#include "Icons/IconService.h"

namespace NLS::Editor::Panels
{
namespace
{
    constexpr const char* kTokenNames[] {
        "Tiny (12)", "Small (16)", "Default (20)", "Medium (24)", "Large (32)", "Huge (48)"
    };
    constexpr const char* kStateNames[] {
        "Normal", "Hovered", "Pressed", "Selected", "Disabled", "Danger"
    };

    const char* KindName(const Icons::IconKind p_kind)
    {
        switch (p_kind)
        {
        case Icons::IconKind::Monochrome: return "Monochrome";
        case Icons::IconKind::Color:      return "Color";
        case Icons::IconKind::Glyph:      return "Glyph";
        case Icons::IconKind::Generated:  return "Generated";
        case Icons::IconKind::Raster:     return "Raster";
        }
        return "Unknown";
    }

    std::string ToLower(std::string p_text)
    {
        std::transform(
            p_text.begin(),
            p_text.end(),
            p_text.begin(),
            [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
        return p_text;
    }

    const char* ShortName(const std::string& p_id)
    {
        const auto dot = p_id.rfind('.');
        return dot != std::string::npos ? p_id.c_str() + dot + 1 : p_id.c_str();
    }
}

IconBrowser::IconBrowser(
    const std::string& p_title,
    const bool p_opened,
    const UI::PanelWindowSettings& p_windowSettings)
    : PanelWindow(p_title, p_opened, p_windowSettings)
{
}

bool IconBrowser::MatchesFilter(const Icons::IconDefinition& p_definition)
{
    if (m_missingOnly)
    {
        auto& resources = *EDITOR_CONTEXT(editorResources);
        auto& painter = resources.GetIconPainter();
        const auto pixels = Icons::ComputeIconTargetPixels(
            static_cast<float>(Icons::IconTokenToLogicalPixels(Icons::IconToken::Small)),
            painter.GetMetrics());
        if (resources.GetIconService().Resolve(Icons::IconId{ p_definition.id }, pixels, false).status !=
            Icons::IconResolveStatus::Missing)
        {
            return false;
        }
    }

    if (m_search[0] == '\0' && m_usageFilter == 0)
        return true;

    if (m_usageFilter != 0)
    {
        auto& resources = *EDITOR_CONTEXT(editorResources);
        const auto usage = resources.GetIconService().GetUsageCount(p_definition.id);
        if (m_usageFilter == 1 && usage == 0)
            return false;
        if (m_usageFilter == 2 &&
            (usage > 0 || p_definition.kind == Icons::IconKind::Generated))
        {
            return false;
        }
    }

    if (m_search[0] == '\0')
        return true;

    const std::string query = ToLower(m_search);
    if (ToLower(p_definition.id).find(query) != std::string::npos)
        return true;
    for (const auto& tag : p_definition.tags)
    {
        if (ToLower(tag).find(query) != std::string::npos)
            return true;
    }
    for (const auto& alias : p_definition.aliases)
    {
        if (ToLower(alias).find(query) != std::string::npos)
            return true;
    }
    return false;
}

void IconBrowser::DrawToolbar()
{
    auto& resources = *EDITOR_CONTEXT(editorResources);
    auto& service = resources.GetIconService();
    auto& painter = resources.GetIconPainter();

    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##search", "Search id, tag or alias", m_search, sizeof(m_search));

    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::Combo("##token", &m_tokenIndex, kTokenNames, IM_ARRAYSIZE(kTokenNames));

    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("##state", &m_stateIndex, kStateNames, IM_ARRAYSIZE(kStateNames));

    ImGui::SameLine();
    int themeIndex = Icons::GetEditorIconThemeVariant() == Icons::EditorIconThemeVariant::Light ? 1 : 0;
    static const char* kThemeNames[] { "Dark icons", "Light icons" };
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::Combo("##icon-theme", &themeIndex, kThemeNames, IM_ARRAYSIZE(kThemeNames)))
    {
        Icons::SetEditorIconThemeVariant(
            themeIndex == 0 ? Icons::EditorIconThemeVariant::Dark : Icons::EditorIconThemeVariant::Light);
    }

    ImGui::SameLine();
    static const char* kUsageNames[] { "Usage: all", "Usage: used", "Usage: unused" };
    ImGui::SetNextItemWidth(130.0f);
    ImGui::Combo("##usage", &m_usageFilter, kUsageNames, IM_ARRAYSIZE(kUsageNames));

    ImGui::SameLine();
    ImGui::Checkbox("Missing only", &m_missingOnly);

    ImGui::SameLine();
    if (ImGui::Button("Reload editor icons"))
    {
        service.InvalidateModule("editor");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Retire cached editor icon textures; they reload from disk on next use.");

    ImGui::SameLine();
    if (ImGui::Button("Reset usage"))
        service.ResetUsageCounts();

    const auto diagnostics = service.GetDiagnostics();
    ImGui::SameLine();
    ImGui::TextDisabled(
        "textures=%zu (~%zu KB) unused=%zu resolutions=%zu fallbacks=%zu hits=%zu scale=%.2fx fb=%.2f",
        diagnostics.loadedTextureCount,
        diagnostics.estimatedTextureBytes / 1024u,
        diagnostics.unusedDefinitionCount,
        diagnostics.resolutionCount,
        diagnostics.fallbackCount,
        diagnostics.cacheHitCount,
        painter.GetMetrics().uiScale,
        painter.GetMetrics().framebufferScale);
}

void IconBrowser::DrawGrid()
{
    auto& resources = *EDITOR_CONTEXT(editorResources);
    auto& registry = resources.GetIconRegistry();
    auto& service = resources.GetIconService();
    auto& painter = resources.GetIconPainter();

    const auto metrics = painter.GetMetrics();
    const auto token = static_cast<Icons::IconToken>(m_tokenIndex);
    const auto state = static_cast<Icons::IconState>(m_stateIndex);
    const float logical = static_cast<float>(Icons::IconTokenToLogicalPixels(token)) *
        (metrics.uiScale > 0.0f ? metrics.uiScale : 1.0f);
    const auto targetPixels = Icons::ComputeIconTargetPixels(
        static_cast<float>(Icons::IconTokenToLogicalPixels(token)), metrics);

    const ImVec2 cellSize(logical + 30.0f, logical + 42.0f);
    const float available = ImGui::GetContentRegionAvail().x;
    const int columns = (std::max)(1, static_cast<int>(available / cellSize.x));

    if (ImGui::BeginTable("##icon-grid", columns, ImGuiTableFlags_SizingFixedFit))
    {
        for (const auto& definition : registry.GetDefinitions())
        {
            if (!MatchesFilter(definition))
                continue;

            ImGui::TableNextColumn();
            ImGui::PushID(definition.id.c_str());
            const ImVec2 cellMin = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton("##cell", cellSize);
            const bool hovered = ImGui::IsItemHovered();
            const bool selected = m_selectedId == definition.id;
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            const ImVec2 cellMax(cellMin.x + cellSize.x, cellMin.y + cellSize.y);
            drawList->AddRectFilled(
                cellMin,
                cellMax,
                hovered ? IM_COL32(58, 64, 72, 220) : IM_COL32(38, 42, 48, 180),
                5.0f);
            if (selected)
                drawList->AddRect(cellMin, cellMax, IM_COL32(150, 198, 255, 255), 5.0f, 0, 1.5f);

            const Icons::IconId icon { definition.id };
            const Icons::IconLoadResult resolved = service.Resolve(icon, targetPixels, false);
            const ImVec2 iconPos(
                cellMin.x + (cellSize.x - logical) * 0.5f,
                cellMin.y + 8.0f);

            if (definition.kind == Icons::IconKind::Glyph)
            {
                const auto glyph = service.ResolveGlyph(icon, logical, false);
                if (glyph.valid)
                {
                    const ImVec2 textSize = ImGui::CalcTextSize(glyph.utf8);
                    drawList->AddText(
                        ImVec2(iconPos.x + (logical - textSize.x) * 0.5f,
                            iconPos.y + (logical - textSize.y) * 0.5f),
                        IM_COL32(220, 220, 220, 255),
                        glyph.utf8);
                }
            }
            else
            {
                painter.Draw(*drawList, iconPos, icon, token, state, metrics);
            }

            if (resolved.status != Icons::IconResolveStatus::Resolved)
            {
                drawList->AddText(
                    ImVec2(cellMin.x + 4.0f, cellMax.y - 30.0f),
                    IM_COL32(235, 96, 96, 255),
                    resolved.status == Icons::IconResolveStatus::Missing ? "MISSING" : "FALLBACK");
            }

            const ImVec2 labelSize = ImGui::CalcTextSize(ShortName(definition.id));
            drawList->AddText(
                ImVec2(cellMin.x + (cellSize.x - labelSize.x) * 0.5f, cellMax.y - 18.0f),
                IM_COL32(200, 200, 200, 255),
                ShortName(definition.id));

            if (hovered)
                ImGui::SetTooltip("%s\n%s", definition.id.c_str(), KindName(definition.kind));

            if (clicked)
                m_selectedId = definition.id;

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void IconBrowser::DrawDetails()
{
    auto& resources = *EDITOR_CONTEXT(editorResources);
    auto& registry = resources.GetIconRegistry();
    auto& service = resources.GetIconService();
    auto& painter = resources.GetIconPainter();

    ImGui::SeparatorText("Details");
    if (m_selectedId.empty())
    {
        ImGui::TextDisabled("Select an icon to inspect it.");
        return;
    }

    const auto* definition = registry.Find(m_selectedId);
    if (definition == nullptr)
        return;

    ImGui::Text("ID: %s", definition->id.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy ID"))
        ImGui::SetClipboardText(definition->id.c_str());

    ImGui::Text("Kind: %s   Module: %s   Themeable: %s",
        KindName(definition->kind),
        definition->module.c_str(),
        definition->themeable ? "yes" : "no");

    if (!definition->tags.empty())
    {
        ImGui::TextUnformatted("Tags:");
        for (const auto& tag : definition->tags)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tag.c_str());
        }
    }
    if (!definition->aliases.empty())
    {
        ImGui::TextUnformatted("Aliases:");
        for (const auto& alias : definition->aliases)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", alias.c_str());
        }
    }
    if (!definition->tiers.empty())
    {
        ImGui::TextUnformatted("Tiers:");
        for (const auto tier : definition->tiers)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%upx", tier);
        }
    }

    const auto targetPixels = Icons::ComputeIconTargetPixels(
        static_cast<float>(Icons::IconTokenToLogicalPixels(Icons::IconToken::Huge)),
        painter.GetMetrics());
    const auto resolved = service.Resolve(Icons::IconId{ definition->id }, targetPixels, false);
    const char* status = resolved.status == Icons::IconResolveStatus::Resolved
        ? "Resolved"
        : (resolved.status == Icons::IconResolveStatus::Fallback ? "Fallback" : "Missing");
    ImGui::Text("Resolve: %s   resolvedId: %s   textureGeneration: %u   used: %u",
        status,
        resolved.resolvedId.empty() ? "-" : resolved.resolvedId.c_str(),
        resolved.icon.texture.generation,
        service.GetUsageCount(definition->id));
}

void IconBrowser::_Draw_Impl()
{
    if (!IsOpened())
        return;

    EDITOR_CONTEXT(editorResources)->GetIconPainter().BeginFrame();
    DrawToolbar();
    ImGui::Separator();
    ImGui::BeginChild("##grid", ImVec2(0.0f, -190.0f));
    DrawGrid();
    ImGui::EndChild();
    DrawDetails();
}
}

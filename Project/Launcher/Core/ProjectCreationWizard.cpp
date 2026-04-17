#include "ProjectCreationWizard.h"
#include "LauncherProjectMetadata.h"
#include "TemplateManager.h"
#include "LauncherTheme.h"
#include "LauncherLocalization.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <vector>

#include <Debug/Logger.h>
#include <UI/Internal/Converter.h>
#include <Utils/PathParser.h>
#include <Windowing/Dialogs/MessageBox.h>
#include <Windowing/Dialogs/SelectFolderDialog.h>

#include <Rendering/Settings/GraphicsBackendUtils.h>

#include "ImGui/imgui.h"
#include "ImGui/imgui_internal.h"

namespace NLS
{

namespace
{
const char* Tr(LauncherTextKey key)
{
    return GetLauncherLocalization().CStr(key);
}

void DrawWizardPageTitle(const ImVec2& position, const char* text, float size = 24.0f)
{
    auto* drawList = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const ImU32 color = HubColorU32(HubColors::TextPrimary);
    drawList->AddText(font, size, ImVec2(position.x + 0.4f, position.y), color, text);
    drawList->AddText(font, size, position, color, text);
}
} // namespace

ProjectCreationWizard::ProjectCreationWizard(
    const std::vector<ProjectTemplate>& templates,
    std::vector<LauncherInstallView> editorVersions,
    OnProjectCreatedCallback onCreated)
    : m_onCreated(std::move(onCreated))
    , m_templates(templates)
    , m_editorVersions(std::move(editorVersions))
{
    m_selectedCategory = std::string(Tr(LauncherTextKey::AllTemplates));
    m_categories.push_back(m_selectedCategory);
    for (const auto& tmpl : m_templates)
    {
        if (std::find(m_categories.begin(), m_categories.end(), tmpl.category) == m_categories.end())
            m_categories.push_back(tmpl.category);
    }

    for (int index = 0; index < static_cast<int>(m_editorVersions.size()); ++index)
    {
        if (m_editorVersions[index].isDefault && m_editorVersions[index].isValid)
        {
            m_selectedEditorVersionIndex = index;
            m_config.editorExecutablePath = m_editorVersions[index].executablePath;
            break;
        }
    }

    if (m_selectedEditorVersionIndex < 0)
    {
        for (int index = 0; index < static_cast<int>(m_editorVersions.size()); ++index)
        {
            if (m_editorVersions[index].isValid)
            {
                m_selectedEditorVersionIndex = index;
                m_config.editorExecutablePath = m_editorVersions[index].executablePath;
                break;
            }
        }
    }
}

void ProjectCreationWizard::DrawContent(float width, float height)
{
    const ImVec2 rootOrigin = ImGui::GetCursorScreenPos();
    DrawWizardHeader(rootOrigin.x, rootOrigin.y, width);

    const float footerHeight = HubLayout::kWizardFooterHeight;
    DrawWizardFooter(rootOrigin.x, rootOrigin.y + height - footerHeight, width);

    const float headerHeight = HubLayout::kWizardHeaderHeight;
    const float bodyY = rootOrigin.y + headerHeight;
    const float bodyHeight = height - headerHeight - footerHeight;
    const auto columns = HubLayout::CalculateWizardColumns(width);

    DrawCategorySidebar(rootOrigin.x, bodyY, columns.category, bodyHeight);
    DrawTemplateGrid(rootOrigin.x + columns.category, bodyY, columns.templates, bodyHeight);
    DrawTemplatePreview(rootOrigin.x + columns.category + columns.templates, bodyY, columns.preview, bodyHeight);
}

void ProjectCreationWizard::DrawWizardHeader(float x, float y, float width)
{
    const ImVec2 origin(x, y);
    constexpr float kHeaderHeight = HubLayout::kWizardHeaderHeight;
    const ImVec2 end(origin.x + width, origin.y + kHeaderHeight);
    auto* drawList = ImGui::GetWindowDrawList();

    drawList->AddRectFilled(origin, end, HubColorU32(HubColors::Surface));
    drawList->AddLine(ImVec2(origin.x, end.y), ImVec2(end.x, end.y), HubColorU32(HubColors::Border), 1.0f);

    // Title
    const char* title = Tr(LauncherTextKey::NewProjectTitle);
    const ImVec2 titleSize = ImGui::CalcTextSize(title);
    DrawWizardPageTitle(
        ImVec2(origin.x + width * 0.5f - titleSize.x * 0.5f, origin.y + 18.0f),
        title);
}

void ProjectCreationWizard::DrawWizardFooter(float x, float y, float width)
{
    const ImVec2 origin(x, y);
    constexpr float kFooterHeight = HubLayout::kWizardFooterHeight;
    const ImVec2 end(origin.x + width, origin.y + kFooterHeight);
    auto* drawList = ImGui::GetWindowDrawList();

    drawList->AddRectFilled(origin, end, HubColorU32(HubColors::Surface));
    drawList->AddLine(ImVec2(origin.x, origin.y), ImVec2(end.x, origin.y), HubColorU32(HubColors::Border), 1.0f);

    if (!m_validationError.empty())
    {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 24.0f, origin.y + 22.0f));
        PushHubText(HubColors::Danger);
        ImGui::PushTextWrapPos(origin.x + width - 340.0f);
        ImGui::TextWrapped("%s", m_validationError.c_str());
        ImGui::PopTextWrapPos();
        PopHubText();
    }

    float btnY = origin.y + 14.0f;
    float rightX = origin.x + width - 24.0f;

    const ImVec2 createBtnSize(140.0f, 34.0f);
    rightX -= createBtnSize.x;
    ImGui::SetCursorScreenPos(ImVec2(rightX, btnY));
    if (DrawHubButton(Tr(LauncherTextKey::CreateProject), createBtnSize, HubColors::Accent, HubColors::AccentHover, HubColors::AccentActive, Maths::Color(1,1,1,0.95f)))
    {
        if (ValidateConfig() && CreateProjectFiles())
        {
            m_completed = true;
            if (m_onCreated)
                m_onCreated(m_config);
        }
    }

    const ImVec2 cancelBtnSize(80.0f, 34.0f);
    rightX -= cancelBtnSize.x + 8.0f;
    ImGui::SetCursorScreenPos(ImVec2(rightX, btnY));
    if (DrawHubButton(Tr(LauncherTextKey::Cancel), cancelBtnSize, HubColors::Surface, HubColors::SurfaceHover, HubColors::SurfaceActive, HubColors::TextPrimary))
    {
        m_cancelled = true;
    }
}

void ProjectCreationWizard::DrawCategorySidebar(float x, float y, float width, float height)
{
    auto* drawList = ImGui::GetWindowDrawList();
    const ImVec2 end(x + width, y + height);

    drawList->AddRectFilled(ImVec2(x, y), end, HubColorU32(HubColors::Surface));
    drawList->AddLine(ImVec2(end.x, y), ImVec2(end.x, end.y), HubColorU32(HubColors::Border), 1.0f);

    float itemY = y + 16.0f;
    constexpr float kItemHeight = 36.0f;

    for (const auto& category : m_categories)
    {
        const bool isSelected = (m_selectedCategory == category);
        const ImVec2 itemOrigin(x, itemY);
        const ImVec2 itemEnd(x + width, itemY + kItemHeight);

        if (isSelected)
        {
            drawList->AddRectFilled(itemOrigin, itemEnd, HubColorU32(HubColors::RowSelected));
            drawList->AddRectFilled(itemOrigin, ImVec2(x + 3.0f, itemEnd.y), HubColorU32(HubColors::Accent));
        }

        ImGui::SetCursorScreenPos(ImVec2(x + 20.0f, itemY + 8.0f));
        ImGui::PushID(category.c_str());
        ImGui::InvisibleButton("##CategoryItem", ImVec2(width - 20.0f, kItemHeight));

        if (ImGui::IsItemHovered() && !isSelected)
            drawList->AddRectFilled(itemOrigin, itemEnd, HubColorU32(HubColors::RowHover));

        if (ImGui::IsItemClicked())
            m_selectedCategory = category;

        ImGui::PopID();

        ImGui::SetCursorScreenPos(ImVec2(x + 20.0f, itemY + 8.0f));
        PushHubText(isSelected ? HubColors::TextPrimary : HubColors::TextSecondary);
        ImGui::TextUnformatted(category.c_str());
        PopHubText();

        itemY += kItemHeight;
    }
}

void ProjectCreationWizard::DrawTemplateGrid(float x, float y, float width, float height)
{
    auto* drawList = ImGui::GetWindowDrawList();
    const ImVec2 end(x + width, y + height);

    drawList->AddRectFilled(ImVec2(x, y), end, HubColorU32(HubColors::Background));

    // Search bar
    const float searchHeight = 40.0f;
    ImGui::SetCursorScreenPos(ImVec2(x + 16.0f, y + 8.0f));
    ImGui::PushItemWidth(width - 32.0f);
    PushHubInputStyle();
    ImGui::InputTextWithHint("##TemplateSearch", Tr(LauncherTextKey::SearchTemplates), m_templateSearchBuffer, sizeof(m_templateSearchBuffer));
    PopHubInputStyle();
    ImGui::PopItemWidth();

    drawList->AddLine(
        ImVec2(x, y + searchHeight),
        ImVec2(x + width, y + searchHeight),
        HubColorU32(HubColors::Border), 1.0f);

    // Filter templates
    std::vector<int> filteredIndices;
    std::string searchStr(m_templateSearchBuffer);
    std::transform(searchStr.begin(), searchStr.end(), searchStr.begin(), ::tolower);

    for (int i = 0; i < static_cast<int>(m_templates.size()); ++i)
    {
        const auto& tmpl = m_templates[i];
        if (m_selectedCategory != Tr(LauncherTextKey::AllTemplates) && tmpl.category != m_selectedCategory)
            continue;
        if (!searchStr.empty())
        {
            std::string nameLower = tmpl.name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
            if (nameLower.find(searchStr) == std::string::npos)
                continue;
        }
        filteredIndices.push_back(i);
    }

    const float listY = y + searchHeight;
    const float listHeight = height - searchHeight;

    ImGui::SetCursorScreenPos(ImVec2(x, listY));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));
    ImGui::BeginChild("##TemplateList", ImVec2(width, listHeight), false, ImGuiWindowFlags_None);

    if (filteredIndices.empty())
    {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - 32.0f) * 0.3f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 20.0f);
        PushHubText(HubColors::TextMuted);
        ImGui::TextUnformatted(Tr(LauncherTextKey::NoMatchTemplate));
        PopHubText();
    }
    else
    {
        const float cardWidth = width - 32.0f;
        constexpr float kCardHeight = 72.0f;
        constexpr float kCardSpacing = 6.0f;

        for (int fi = 0; fi < static_cast<int>(filteredIndices.size()); ++fi)
        {
            int i = filteredIndices[fi];
            const auto& tmpl = m_templates[i];
            const bool isSelected = (m_selectedTemplateIndex == i);

            const ImVec2 cardOrigin = ImGui::GetCursorScreenPos();
            const ImVec2 cardEnd(cardOrigin.x + cardWidth, cardOrigin.y + kCardHeight);

            ImU32 cardBg = isSelected ? HubColorU32(HubColors::RowSelected) : HubColorU32(HubColors::Surface);
            drawList->AddRectFilled(cardOrigin, cardEnd, cardBg, 6.0f);
            drawList->AddRect(cardOrigin, cardEnd, HubColorU32(HubColors::Border), 6.0f, 0, 1.0f);

            if (isSelected)
            {
                drawList->AddRectFilled(cardOrigin, ImVec2(cardOrigin.x + 3.0f, cardEnd.y), HubColorU32(HubColors::Accent), 6.0f, ImDrawFlags_RoundCornersLeft);
                drawList->AddRect(cardOrigin, cardEnd, HubColorU32(HubColors::Accent), 6.0f, 0, 1.5f);
            }

            // Icon placeholder
            const ImVec2 iconMin(cardOrigin.x + 12.0f, cardOrigin.y + 12.0f);
            const ImVec2 iconMax(iconMin.x + 48.0f, iconMin.y + 48.0f);
            drawList->AddRectFilled(iconMin, iconMax, HubColorU32(HubColors::SurfaceHover), 6.0f);
            {
                ImU32 lineColor = HubColorU32(HubColors::TextMuted);
                for (int row = 0; row < 3; ++row)
                {
                    float ly = iconMin.y + 12.0f + row * 10.0f;
                    drawList->AddLine(
                        ImVec2(iconMin.x + 10.0f, ly),
                        ImVec2(iconMax.x - 10.0f, ly),
                        lineColor, 1.2f);
                }
            }

            // Template name
            ImGui::SetCursorScreenPos(ImVec2(cardOrigin.x + 72.0f, cardOrigin.y + 14.0f));
            PushHubText(HubColors::TextPrimary);
            ImGui::TextUnformatted(tmpl.name.c_str());
            PopHubText();

            // Category tag
            ImGui::SetCursorScreenPos(ImVec2(cardOrigin.x + 72.0f, cardOrigin.y + 38.0f));
            PushHubText(HubColors::TextMuted);
            ImGui::TextUnformatted(tmpl.category.c_str());
            PopHubText();

            // Clickable
            ImGui::SetCursorScreenPos(cardOrigin);
            ImGui::PushID(i);
            ImGui::InvisibleButton("##TemplateCard", ImVec2(cardWidth, kCardHeight));
            if (ImGui::IsItemHovered() && !isSelected)
                drawList->AddRectFilled(cardOrigin, cardEnd, HubColorU32(HubColors::RowHover), 6.0f);
            if (ImGui::IsItemClicked())
                m_selectedTemplateIndex = i;
            ImGui::PopID();

            ImGui::SetCursorScreenPos(ImVec2(cardOrigin.x, cardEnd.y + kCardSpacing));
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void ProjectCreationWizard::DrawTemplatePreview(float x, float y, float width, float height)
{
    auto* drawList = ImGui::GetWindowDrawList();
    const ImVec2 end(x + width, y + height);

    drawList->AddRectFilled(ImVec2(x, y), end, HubColorU32(HubColors::Surface));
    drawList->AddLine(ImVec2(x, y), ImVec2(x, end.y), HubColorU32(HubColors::Border), 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 16.0f));
    ImGui::BeginChild("##PreviewSettings", ImVec2(width, height), false, ImGuiWindowFlags_None);

    const float contentWidth = width - 40.0f;

    if (!m_templates.empty() && m_selectedTemplateIndex >= 0 && m_selectedTemplateIndex < static_cast<int>(m_templates.size()))
    {
        const auto& tmpl = m_templates[m_selectedTemplateIndex];

        // Preview image placeholder
        {
            const ImVec2 imgOrigin = ImGui::GetCursorScreenPos();
            const float imgHeight = 140.0f;
            const ImVec2 imgEnd(imgOrigin.x + contentWidth, imgOrigin.y + imgHeight);
            drawList->AddRectFilled(imgOrigin, imgEnd, HubColorU32(HubColors::SurfaceHover), 6.0f);

            ImGui::SetCursorScreenPos(ImVec2(imgOrigin.x + contentWidth * 0.5f - 30.0f, imgOrigin.y + imgHeight * 0.5f - 8.0f));
            PushHubText(HubColors::TextMuted);
            ImGui::TextUnformatted(Tr(LauncherTextKey::PreviewImage));
            PopHubText();

            ImGui::SetCursorScreenPos(ImVec2(imgOrigin.x, imgEnd.y + 12.0f));
        }

        // Template name
        PushHubText(HubColors::TextPrimary);
        ImGui::TextUnformatted(tmpl.name.c_str());
        PopHubText();
        ImGui::Spacing();

        // Description
        PushHubText(HubColors::TextSecondary);
        ImGui::PushTextWrapPos(x + width - 20.0f);
        ImGui::TextWrapped("%s", tmpl.description.c_str());
        ImGui::PopTextWrapPos();
        PopHubText();

        ImGui::Spacing();
        ImGui::Spacing();

        // Project Settings
        drawList->AddLine(
            ImGui::GetCursorScreenPos(),
            ImVec2(ImGui::GetCursorScreenPos().x + contentWidth, ImGui::GetCursorScreenPos().y),
            HubColorU32(HubColors::Border), 1.0f);
        ImGui::Spacing();

        PushHubText(HubColors::TextMuted);
        ImGui::TextUnformatted(Tr(LauncherTextKey::ProjectSettings));
        PopHubText();
        ImGui::Spacing();

        // Project Name
        PushHubText(HubColors::TextSecondary);
        ImGui::TextUnformatted(Tr(LauncherTextKey::ProjectName));
        PopHubText();
        ImGui::PushItemWidth(contentWidth);
        PushHubInputStyle();
        ImGui::InputText("##ProjectName", m_nameBuffer, kInputBufferSize);
        PopHubInputStyle();
        ImGui::PopItemWidth();
        ImGui::Spacing();

        // Location
        PushHubText(HubColors::TextSecondary);
        ImGui::TextUnformatted(Tr(LauncherTextKey::Location));
        PopHubText();

        float browseBtnWidth = 70.0f;
        ImGui::PushItemWidth(contentWidth - browseBtnWidth - 8.0f);
        PushHubInputStyle();
        ImGui::InputText("##ProjectLocation", m_locationBuffer, kInputBufferSize);
        PopHubInputStyle();
        ImGui::PopItemWidth();

        ImGui::SameLine(0.0f, 8.0f);
        if (DrawHubButton(Tr(LauncherTextKey::Browse), {browseBtnWidth, 0.0f}, HubColors::Surface, HubColors::SurfaceHover, HubColors::SurfaceActive, HubColors::TextPrimary))
        {
            Dialogs::SelectFolderDialog dialog(Tr(LauncherTextKey::SelectLocation));
            std::string result = dialog.Result();
            if (!result.empty())
            {
                m_config.projectLocation = result;
                strncpy(m_locationBuffer, result.c_str(), kInputBufferSize - 1);
                m_locationBuffer[kInputBufferSize - 1] = '\0';
            }
        }

        ImGui::Spacing();
        ImGui::Spacing();

        PushHubText(HubColors::TextSecondary);
        ImGui::TextUnformatted(Tr(LauncherTextKey::EditorVersion));
        PopHubText();

        ImGui::PushItemWidth(contentWidth);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, UI::Internal::Converter::ToImVec4(HubColors::InputBg));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, UI::Internal::Converter::ToImVec4(HubColors::Surface));
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4(HubColors::TextPrimary));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

        const char* currentEditorLabel = Tr(LauncherTextKey::NoInstalledVersions);
        if (m_selectedEditorVersionIndex >= 0 &&
            m_selectedEditorVersionIndex < static_cast<int>(m_editorVersions.size()) &&
            m_editorVersions[m_selectedEditorVersionIndex].isValid)
        {
            currentEditorLabel = m_editorVersions[m_selectedEditorVersionIndex].versionLabel.c_str();
        }

        if (ImGui::BeginCombo("##EditorVersion", currentEditorLabel))
        {
            for (int index = 0; index < static_cast<int>(m_editorVersions.size()); ++index)
            {
                const auto& version = m_editorVersions[index];
                if (!version.isValid)
                    continue;

                bool isSelected = (index == m_selectedEditorVersionIndex);
                if (ImGui::Selectable(version.versionLabel.c_str(), isSelected))
                {
                    m_selectedEditorVersionIndex = index;
                    m_config.editorExecutablePath = version.executablePath;
                }
                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        ImGui::PopItemWidth();

        if (m_selectedEditorVersionIndex >= 0 &&
            m_selectedEditorVersionIndex < static_cast<int>(m_editorVersions.size()))
        {
            PushHubText(HubColors::TextMuted);
            ImGui::PushTextWrapPos(x + width - 20.0f);
            ImGui::TextWrapped("%s", m_editorVersions[m_selectedEditorVersionIndex].executablePath.c_str());
            ImGui::PopTextWrapPos();
            PopHubText();
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // Advanced Settings (collapsible)
        PushHubText(HubColors::TextSecondary);
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0,0,0,0));
        if (ImGui::CollapsingHeader(Tr(LauncherTextKey::Advanced)))
        {
            ImGui::PopStyleColor(4);
            ImGui::Spacing();

            // Backend
            PushHubText(HubColors::TextSecondary);
            ImGui::TextUnformatted(Tr(LauncherTextKey::BackendLabel));
            PopHubText();

            ImGui::PushItemWidth(contentWidth);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, UI::Internal::Converter::ToImVec4(HubColors::InputBg));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, UI::Internal::Converter::ToImVec4(HubColors::Surface));
            ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4(HubColors::TextPrimary));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

            int currentBackendIdx = 0;
            for (int i = 0; i < s_backendCount; ++i)
            {
                if (s_backends[i].backend == m_config.selectedBackend)
                {
                    currentBackendIdx = i;
                    break;
                }
            }
            if (ImGui::BeginCombo("##Backend", s_backends[currentBackendIdx].label))
            {
                for (int i = 0; i < s_backendCount; ++i)
                {
                    bool isSel = (s_backends[i].backend == m_config.selectedBackend);
                    if (ImGui::Selectable(s_backends[i].label, isSel))
                        m_config.selectedBackend = s_backends[i].backend;
                    if (isSel)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            ImGui::PopItemWidth();
            ImGui::Spacing();

            // Resolution
            PushHubText(HubColors::TextSecondary);
            ImGui::TextUnformatted(Tr(LauncherTextKey::Resolution));
            PopHubText();

            ImGui::PushItemWidth((contentWidth - 16.0f) * 0.45f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, UI::Internal::Converter::ToImVec4(HubColors::InputBg));
            ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4(HubColors::TextPrimary));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            ImGui::InputInt("##Width", &m_config.windowWidth);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            ImGui::PopItemWidth();

            ImGui::SameLine(0, 6.0f);
            PushHubText(HubColors::TextMuted);
            ImGui::TextUnformatted("x");
            PopHubText();
            ImGui::SameLine(0, 6.0f);

            ImGui::PushItemWidth((contentWidth - 16.0f) * 0.45f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, UI::Internal::Converter::ToImVec4(HubColors::InputBg));
            ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4(HubColors::TextPrimary));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            ImGui::InputInt("##Height", &m_config.windowHeight);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            ImGui::PopItemWidth();
            ImGui::Spacing();

            // VSync & MSAA
            PushHubText(HubColors::TextPrimary);
            ImGui::Checkbox("VSync", &m_config.vsync);
            ImGui::SameLine(0, 20.0f);
            ImGui::Checkbox("MSAA", &m_config.multiSampling);
            PopHubText();

            if (m_config.multiSampling)
            {
                ImGui::SameLine(0, 12.0f);
                PushHubText(HubColors::TextSecondary);
                ImGui::TextUnformatted(Tr(LauncherTextKey::Samples));
                PopHubText();
                ImGui::SameLine(0, 6.0f);
                ImGui::PushItemWidth(60.0f);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, UI::Internal::Converter::ToImVec4(HubColors::InputBg));
                ImGui::PushStyleColor(ImGuiCol_PopupBg, UI::Internal::Converter::ToImVec4(HubColors::Surface));
                ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4(HubColors::TextPrimary));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

                if (ImGui::BeginCombo("##Samples", std::to_string(m_config.sampleCount).c_str()))
                {
                    for (int i = 0; i < s_sampleCountCount; ++i)
                    {
                        bool isSel = (s_sampleCounts[i] == m_config.sampleCount);
                        if (ImGui::Selectable(std::to_string(s_sampleCounts[i]).c_str(), isSel))
                            m_config.sampleCount = s_sampleCounts[i];
                        if (isSel)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(3);
                ImGui::PopItemWidth();
            }
        }
        else
        {
            ImGui::PopStyleColor(4);
        }
    }
    else
    {
        ImGui::SetCursorScreenPos(ImVec2(x + 20.0f, y + height * 0.5f - 10.0f));
        PushHubText(HubColors::TextMuted);
        ImGui::TextUnformatted(Tr(LauncherTextKey::SelectTemplate));
        PopHubText();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

bool ProjectCreationWizard::ValidateConfig()
{
    m_validationError.clear();
    m_config.projectName = m_nameBuffer;
    m_config.projectLocation = m_locationBuffer;
    m_config.editorExecutablePath.clear();
    if (m_selectedEditorVersionIndex >= 0 &&
        m_selectedEditorVersionIndex < static_cast<int>(m_editorVersions.size()) &&
        m_editorVersions[m_selectedEditorVersionIndex].isValid)
    {
        m_config.editorExecutablePath = m_editorVersions[m_selectedEditorVersionIndex].executablePath;
    }

    const auto validation = ValidateProjectCreationConfig(m_config);
    if (validation.ok)
        return true;

    if (validation.messageKey == LauncherTextKey::DirExistsPrefix)
    {
        m_validationError = std::string(Tr(LauncherTextKey::DirExistsPrefix)) + m_config.projectName + Tr(LauncherTextKey::DirExistsSuffix);
        return false;
    }

    if (validation.requiresModalPrompt)
    {
        Dialogs::MessageBox prompt(
            Tr(LauncherTextKey::WizardNoEditorVersionsTitle),
            Tr(LauncherTextKey::WizardNoEditorVersionsBody),
            Dialogs::MessageBox::EMessageType::WARNING,
            Dialogs::MessageBox::EButtonLayout::OK);
        return false;
    }

    m_validationError = Tr(validation.messageKey);
    return false;
}

bool ProjectCreationWizard::CreateProjectFiles()
{
    std::filesystem::path projectRoot = std::filesystem::path(m_config.projectLocation) / m_config.projectName;

    std::error_code ec;
    std::filesystem::create_directory(projectRoot, ec);
    if (ec)
    {
        m_validationError = std::string(Tr(LauncherTextKey::CreateDirFailed)) + " " + ec.message();
        return false;
    }

    std::filesystem::create_directory(projectRoot / "Assets", ec);
    std::filesystem::create_directory(projectRoot / "Logs", ec);
    std::filesystem::create_directory(projectRoot / "UserSettings", ec);
    std::filesystem::create_directory(projectRoot / "ProjectSettings", ec);

    if (!m_templates.empty() && m_selectedTemplateIndex >= 0 && m_selectedTemplateIndex < static_cast<int>(m_templates.size()))
    {
        const auto& selectedTemplate = m_templates[m_selectedTemplateIndex];
        m_config.templateId = selectedTemplate.id;
        auto contentPath = selectedTemplate.GetContentPath();
        if (selectedTemplate.HasContent() && std::filesystem::exists(contentPath))
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(contentPath, ec))
            {
                if (ec) break;
                auto relativePath = std::filesystem::relative(entry.path(), contentPath, ec);
                if (ec) break;
                auto targetPath = projectRoot / relativePath;
                if (entry.is_directory())
                {
                    std::filesystem::create_directories(targetPath, ec);
                }
                else if (entry.is_regular_file())
                {
                    std::filesystem::create_directories(targetPath.parent_path(), ec);
                    std::filesystem::copy_file(entry.path(), targetPath, std::filesystem::copy_options::overwrite_existing, ec);
                }
            }
        }
    }

    std::string nullusFilePath = (projectRoot / (m_config.projectName + ".nullus")).string();

    std::ofstream ofs(nullusFilePath);
    if (!ofs.is_open())
    {
        m_validationError = Tr(LauncherTextKey::CreateFileFailed);
        return false;
    }

    ofs << "executable_name=Game\n";
    ofs << "gravity=-9.810000\n";
    ofs << "x_resolution=" << m_config.windowWidth << "\n";
    ofs << "y_resolution=" << m_config.windowHeight << "\n";
    ofs << "start_scene=\n";
    ofs << "fullscreen=false\n";
    ofs << "multi_sampling=" << (m_config.multiSampling ? "true" : "false") << "\n";
    ofs << "vsync=" << (m_config.vsync ? "true" : "false") << "\n";
    ofs << "samples=" << m_config.sampleCount << "\n";
    ofs << "graphics_backend=" << Render::Settings::ToString(m_config.selectedBackend) << "\n";
    ofs << "opengl_major=4\n";
    ofs << "opengl_minor=3\n";
    ofs << "dev_build=true\n";
    ofs.close();

    if (!m_config.editorExecutablePath.empty())
        WriteProjectLastEditorExecutable(projectRoot, m_config.editorExecutablePath);

    NLS_LOG_INFO("Project created: " + nullusFilePath);
    return true;
}

} // namespace NLS

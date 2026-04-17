#include "Launcher.h"
#include "ProjectCreationWizard.h"
#include "LauncherProjectMetadata.h"
#include "LauncherTheme.h"
#include "LauncherLocalization.h"
#include "LauncherNavigation.h"
#include "LauncherSettings.h"
#define _CRT_SECURE_NO_WARNINGS

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <vector>

#include <Core/ServiceLocator.h>
#include <Debug/Logger.h>
#include <Filesystem/IniFile.h>
#include <Image.h>
#include <UI/Icons/VectorIcons.h>
#include <UI/UIManager.h>

#include <Rendering/Resources/Texture2D.h>
#include <Rendering/Resources/Loaders/TextureLoader.h>
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/Settings/GraphicsBackendUtils.h>
#include <Rendering/Tooling/RenderDocEnvironment.h>
#include <Utils/PathParser.h>

#include <Windowing/Dialogs/SelectFolderDialog.h>
#include <Windowing/Dialogs/OpenFileDialog.h>
#include <Windowing/Dialogs/MessageBox.h>
#include <Platform/Utils/SystemCalls.h>

#include "ImGui/imgui_internal.h"

#define PROJECTS_FILE "projects.ini"

namespace NLS
{
namespace
{

const char* Tr(LauncherTextKey key)
{
    return GetLauncherLocalization().CStr(key);
}

void DrawHubPageTitle(const ImVec2& position, const char* text, float size = 28.0f)
{
    auto* drawList = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const ImU32 color = HubColorU32(HubColors::TextPrimary);
    drawList->AddText(font, size, ImVec2(position.x + 0.45f, position.y), color, text);
    drawList->AddText(font, size, position, color, text);
}

void DrawSectionTab(const ImVec2& position, const char* text)
{
    auto* drawList = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(position);
    PushHubText(HubColors::TextPrimary);
    ImGui::TextUnformatted(text);
    PopHubText();
    drawList->AddLine(
        ImVec2(position.x, position.y + 34.0f),
        ImVec2(position.x + 40.0f, position.y + 34.0f),
        HubColorU32(HubColors::Accent),
        2.0f);
}

std::string ReadEnvironmentValue(const char* name)
{
    if (const char* value = std::getenv(name); value != nullptr)
        return value;

    return {};
}

std::string ResolveLauncherLocale()
{
    for (const char* envName : {"NLS_LAUNCHER_LOCALE", "LC_ALL", "LANG"})
    {
        if (const auto normalized = NormalizeLocaleName(ReadEnvironmentValue(envName)); !normalized.empty())
            return normalized;
    }

    try
    {
        if (const auto normalized = NormalizeLocaleName(std::locale("").name()); !normalized.empty())
            return normalized;
    }
    catch (const std::runtime_error&)
    {
    }

    return "en-US";
}

void DrawTexture(
    const ImVec2& min,
    const ImVec2& max,
    const std::shared_ptr<Render::RHI::RHITextureView>& textureView,
    const ImVec4& tint = ImVec4(1.f, 1.f, 1.f, 1.f))
{
    if (textureView == nullptr)
        return;

    void* resolvedTextureId = nullptr;
    if (Core::ServiceLocator::Contains<UI::UIManager>())
    {
        auto nativeHandle = NLS_SERVICE(UI::UIManager).ResolveTextureView(textureView);
        if (nativeHandle.IsValid())
            resolvedTextureId = nativeHandle.handle;
    }

    if (resolvedTextureId == nullptr)
        return;

    ImGui::GetWindowDrawList()->AddImage(
        resolvedTextureId,
        min, max,
        ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f),
        ImGui::GetColorU32(tint));
}

std::string GetRelativeTimeString(const std::string& projectPath)
{
    std::error_code ec;
    const auto writeTime = std::filesystem::last_write_time(projectPath, ec);
    if (ec)
        return std::string(Tr(LauncherTextKey::TimeUnknown));

    using namespace std::chrono;
    const auto now = std::filesystem::file_time_type::clock::now();
    const auto diff = now - writeTime;

    auto totalSeconds = duration_cast<seconds>(diff).count();
    if (totalSeconds < 0) totalSeconds = 0;

    const auto minutes = totalSeconds / 60;
    const auto hours = minutes / 60;
    const auto days = hours / 24;
    const auto years = days / 365;

    if (totalSeconds < 60)
        return std::to_string(totalSeconds) + " " + Tr(LauncherTextKey::TimeSecondsAgo);
    if (minutes < 60)
        return std::to_string(minutes) + " " + Tr(LauncherTextKey::TimeMinutesAgo);
    if (hours < 24)
        return std::to_string(hours) + " " + Tr(LauncherTextKey::TimeHoursAgo);
    if (days < 365)
        return std::to_string(days) + " " + Tr(LauncherTextKey::TimeDaysAgo);
    return std::to_string(years) + " " + Tr(LauncherTextKey::TimeYearsAgo);
}

std::string DescribeProjectEditorVersionDisplay(const std::string& projectPath)
{
    const auto version = DescribeProjectLastEditorVersion(projectPath);
    if (!version.empty())
        return version;

    return std::string(Tr(LauncherTextKey::TimeUnknown));
}

} // namespace

// ─── View enum ───
enum class LauncherView { Projects, Installs, NewProject };

// ─── Sort helpers ───
enum class SortColumn { Name, Modified, EditorVersion };

class LauncherPanel : public UI::PanelWindow
{
public:
    LauncherPanel(
        Windowing::Window &p_window,
        bool &p_readyToGo,
        std::string &p_path,
        std::string &p_projectName,
        std::string &p_editorExecutablePath,
        std::shared_ptr<Render::RHI::RHITextureView> p_brandTextureView,
        TemplateManager& p_templateManager)
        : PanelWindow("Nullus - Launcher", true)
        , m_window(p_window)
        , m_readyToGo(p_readyToGo)
        , m_path(p_path)
        , m_projectName(p_projectName)
        , m_editorExecutablePath(p_editorExecutablePath)
        , m_brandTextureView(p_brandTextureView)
        , m_templateManager(p_templateManager)
    {
        panelSettings.resizable = false;
        panelSettings.movable = false;
        panelSettings.titleBar = false;
        panelSettings.closable = false;

        SetSize({HubLayout::kDefaultWindowWidth, HubLayout::kDefaultWindowHeight});
        SetPosition({0.f, 0.f});

        std::string line;
        std::ifstream myfile(PROJECTS_FILE);
        if (myfile.is_open())
        {
            while (getline(myfile, line))
            {
                if (std::filesystem::exists(line))
                    m_registeredProjects.push_back(line);
            }
            myfile.close();
        }

        if (!m_registeredProjects.empty())
            m_selectedProject = m_registeredProjects.front();

        m_settings.Load();
    }

    void Draw() override
    {
        if (!IsOpened())
            return;

        // Check wizard state
        if (m_currentView == LauncherView::NewProject && m_wizard)
        {
            if (m_wizard->WasCancelled())
            {
                m_currentView = LauncherView::Projects;
                m_wizard.reset();
            }
            else if (m_wizard->WasCompleted())
            {
                m_currentView = LauncherView::Projects;
                m_wizard.reset();
                return;
            }
        }

        const int windowFlags = ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_NoTitleBar |
                                ImGuiWindowFlags_NoBringToFrontOnFocus;
        const auto windowSize = m_window.GetSize();
        SetSize({windowSize.x, windowSize.y});
        ImGui::SetNextWindowSize(UI::Internal::Converter::ToImVec2(GetSize()), ImGuiCond_Always);
        ImGui::SetNextWindowPos(UI::Internal::Converter::ToImVec2(GetPosition()), ImGuiCond_Always);

        if (!ImGui::Begin(name.c_str(), nullptr, windowFlags))
        {
            ImGui::End();
            return;
        }

        if (m_currentView == LauncherView::NewProject && m_wizard)
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            auto avail = ImGui::GetContentRegionAvail();
            m_wizard->DrawContent(avail.x, avail.y);
            ImGui::PopStyleVar();
        }
        else
        {
            DrawLauncher();
        }

        ImGui::End();
    }

    void RegisterProject(const std::string &p_path)
    {
        if (p_path.empty())
            return;

        if (std::find(m_registeredProjects.begin(), m_registeredProjects.end(), p_path) == m_registeredProjects.end())
            m_registeredProjects.push_back(p_path);

        if (m_selectedProject.empty())
            m_selectedProject = p_path;

        FlushProjectsFile();
    }

    void OpenProject(const std::string &p_path)
    {
        m_readyToGo = std::filesystem::exists(p_path);
        if (!m_readyToGo)
        {
            using namespace Dialogs;
            MessageBox errorMessage(Tr(LauncherTextKey::ProjectNotFound), Tr(LauncherTextKey::ProjectNotExist), MessageBox::EMessageType::ERROR, MessageBox::EButtonLayout::OK);
            return;
        }

        m_path = p_path;
        m_projectName = Utils::PathParser::GetElementName(m_path);
        const auto boundEditorExecutablePath = ReadProjectLastEditorExecutable(p_path);
        if (LauncherSettings::IsValidEngineExecutablePath(boundEditorExecutablePath))
        {
            m_editorExecutablePath = boundEditorExecutablePath;
        }
        else if (m_settings.HasValidDefaultEngineExecutablePath())
        {
            m_editorExecutablePath = m_settings.GetDefaultEngineExecutablePath();
        }
        else
        {
            m_editorExecutablePath.clear();
        }
        m_selectedProject = p_path;
        Close();
    }

private:
    void FlushProjectsFile()
    {
        std::ofstream projectsFile(PROJECTS_FILE, std::ios::trunc);
        for (const auto &project : m_registeredProjects)
        {
            if (std::filesystem::exists(project))
                projectsFile << project << std::endl;
        }
    }

    void ShowWizard()
    {
        auto onCreated = [this](const ProjectCreationConfig& config)
        {
            std::string projectPath = (std::filesystem::path(config.projectLocation) / config.projectName).string();
            RegisterProject(projectPath);

            m_readyToGo = true;
            m_path = projectPath;
            m_projectName = config.projectName;
            m_editorExecutablePath = config.editorExecutablePath;
            Close();
        };

        auto editorVersions = m_settings.GetEngineInstallationViews();
        m_wizard = std::make_unique<ProjectCreationWizard>(
            m_templateManager.GetTemplates(),
            std::move(editorVersions),
            onCreated);
        m_currentView = LauncherView::NewProject;
    }

    void RemoveProject(const std::string &p_path)
    {
        m_registeredProjects.erase(
            std::remove(m_registeredProjects.begin(), m_registeredProjects.end(), p_path),
            m_registeredProjects.end());

        if (m_selectedProject == p_path)
            m_selectedProject = m_registeredProjects.empty() ? std::string{} : m_registeredProjects.front();

        FlushProjectsFile();
    }

    void DrawBrandRail(float x, float y, float width, float height)
    {
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height), HubColorU32(HubColors::Surface));
        drawList->AddLine(ImVec2(x + width, y), ImVec2(x + width, y + height), HubColorU32(HubColors::BorderStrong), 1.0f);

        const ImVec2 logoMin(x + 18.0f, y + 18.0f);
        const ImVec2 logoMax(x + width - 18.0f, y + 18.0f + (width - 36.0f));
        if (m_brandTextureView)
        {
            DrawTexture(logoMin, logoMax, m_brandTextureView, ImVec4(1, 1, 1, 0.92f));
        }
        else
        {
            drawList->AddRect(logoMin, logoMax, HubColorU32(HubColors::TextPrimary), 4.0f, 0, 1.5f);
            drawList->AddLine(logoMin, logoMax, HubColorU32(HubColors::TextMuted), 1.0f);
            drawList->AddLine(ImVec2(logoMax.x, logoMin.y), ImVec2(logoMin.x, logoMax.y), HubColorU32(HubColors::TextMuted), 1.0f);
        }

    }

    bool DrawNavigationItem(
        const char* label,
        UI::Icons::IconId iconId,
        float x,
        float y,
        float width,
        bool selected)
    {
        constexpr float kItemHeight = 40.0f;
        auto* drawList = ImGui::GetWindowDrawList();
        const ImVec2 itemMin(x, y);
        const ImVec2 itemMax(x + width, y + kItemHeight);

        if (selected)
        {
            drawList->AddRectFilled(itemMin, itemMax, HubColorU32(HubColors::SurfaceActive), 6.0f);
            drawList->AddRectFilled(ImVec2(itemMin.x, itemMin.y + 5.0f), ImVec2(itemMin.x + 3.0f, itemMax.y - 5.0f), HubColorU32(HubColors::Accent), 2.0f);
        }

        ImGui::SetCursorScreenPos(itemMin);
        ImGui::PushID(label);
        ImGui::InvisibleButton("##NavItem", ImVec2(width, kItemHeight));
        const bool clicked = ImGui::IsItemClicked();
        if (ImGui::IsItemHovered() && !selected)
            drawList->AddRectFilled(itemMin, itemMax, HubColorU32(HubColors::SurfaceHover), 6.0f);
        ImGui::PopID();

        const float iconX = x + 18.0f;
        const float iconY = y + 20.0f;
        const auto iconColor = selected ? HubColors::TextPrimary : HubColors::TextSecondary;
        UI::Icons::DrawIcon(
            drawList,
            iconId,
            ImVec2(iconX, iconY),
            14.0f,
            { HubColorU32(iconColor), 1.4f });

        ImGui::SetCursorScreenPos(ImVec2(x + 42.0f, y + 10.0f));
        PushHubText(selected ? HubColors::TextPrimary : HubColors::TextSecondary);
        ImGui::TextUnformatted(label);
        PopHubText();

        return clicked;
    }

    void DrawNavigationRail(float x, float y, float width, float height)
    {
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height), HubColorU32(HubColors::Surface));
        drawList->AddLine(ImVec2(x, y), ImVec2(x, y + height), HubColorU32(HubColors::BorderStrong), 1.0f);
        drawList->AddLine(ImVec2(x + width, y), ImVec2(x + width, y + height), HubColorU32(HubColors::BorderStrong), 1.0f);

        ImGui::SetCursorScreenPos(ImVec2(x + 28.0f, y + 28.0f));
        PushHubText(HubColors::TextPrimary);
        ImGui::TextUnformatted(Tr(LauncherTextKey::LauncherTitle));
        PopHubText();

        const float separatorY = y + 76.0f;
        drawList->AddLine(
            ImVec2(x + 16.0f, separatorY),
            ImVec2(x + width - 12.0f, separatorY),
            HubColorU32(HubColors::BorderStrong),
            1.0f);

        float itemY = y + 96.0f;
        if (DrawNavigationItem(Tr(LauncherTextKey::ProjectsTitle), UI::Icons::IconId::Project, x + 16.0f, itemY, width - 32.0f, m_currentView == LauncherView::Projects))
            m_currentView = LauncherView::Projects;
        itemY += 48.0f;
        if (DrawNavigationItem(Tr(LauncherTextKey::Installs), UI::Icons::IconId::Install, x + 16.0f, itemY, width - 32.0f, m_currentView == LauncherView::Installs))
            m_currentView = LauncherView::Installs;
    }

    // ─── Action Bar (header area below native title bar) ───
    void DrawActionBar(float x, float y, float fullWidth)
    {
        const ImVec2 origin(x, y);
        constexpr float kBarHeight = HubLayout::kActionBarHeight;
        const ImVec2 end(origin.x + fullWidth, origin.y + kBarHeight);
        auto* drawList = ImGui::GetWindowDrawList();

        drawList->AddRectFilled(origin, end, HubColorU32(HubColors::Background));
        drawList->AddLine(ImVec2(origin.x, end.y), ImVec2(end.x, end.y), HubColorU32(HubColors::Border), 1.0f);

        DrawHubPageTitle(ImVec2(origin.x + 32.0f, origin.y + 18.0f), Tr(LauncherTextKey::ProjectsTitle));

        // Right side controls
        float rightX = end.x - 32.0f;

        // Primary project creation action.
        const ImVec2 newBtnSize(130.0f, 32.0f);
        rightX -= newBtnSize.x;
        ImGui::SetCursorScreenPos(ImVec2(rightX, origin.y + 28.0f));
        if (DrawHubButton(Tr(LauncherTextKey::NewProject), newBtnSize, HubColors::Accent, HubColors::AccentHover, HubColors::AccentActive, Maths::Color(1,1,1,0.95f)))
        {
            ShowWizard();
        }

        // Existing project open action.
        const ImVec2 openBtnSize(80.0f, 32.0f);
        rightX -= openBtnSize.x + 8.0f;
        ImGui::SetCursorScreenPos(ImVec2(rightX, origin.y + 28.0f));
        if (DrawHubButton(Tr(LauncherTextKey::OpenProject), openBtnSize, HubColors::Surface, HubColors::SurfaceHover, HubColors::SurfaceActive, HubColors::TextPrimary))
        {
            Dialogs::OpenFileDialog dialog(Tr(LauncherTextKey::OpenProjectFile), "", {"Nullus Project", "*.nullus"});
            if (!dialog.Result().empty())
            {
                const std::string projectFile = dialog.Result()[0];
                const std::string rootFolderPath = std::filesystem::path(projectFile).parent_path().string();
                RegisterProject(rootFolderPath);
                OpenProject(rootFolderPath);
            }
        }

        DrawSectionTab(ImVec2(origin.x + 32.0f, origin.y + 92.0f), Tr(LauncherTextKey::ProjectsTitle));

        const float searchWidth = (std::min)(330.0f, (std::max)(220.0f, fullWidth * 0.28f));
        const ImVec2 searchOrigin(end.x - searchWidth - 32.0f, origin.y + 78.0f);
        ImGui::SetCursorScreenPos(searchOrigin);
        ImGui::PushItemWidth(searchWidth);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, UI::Internal::Converter::ToImVec4(HubColors::InputBg));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, UI::Internal::Converter::ToImVec4(HubColors::InputBgHover));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, UI::Internal::Converter::ToImVec4(HubColors::InputBgActive));
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4(HubColors::TextPrimary));
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, UI::Internal::Converter::ToImVec4(HubColors::TextSecondary));
        ImGui::PushStyleColor(ImGuiCol_Border, UI::Internal::Converter::ToImVec4(HubColors::SearchBorder));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(32.0f, 8.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::InputTextWithHint("##ProjectSearch", Tr(LauncherTextKey::ProjectsSearchHint), m_searchBuffer, sizeof(m_searchBuffer));
        const ImVec2 searchMin = ImGui::GetItemRectMin();
        const ImVec2 searchMax = ImGui::GetItemRectMax();
        const bool searchHovered = ImGui::IsItemHovered();
        const bool searchActive = ImGui::IsItemActive();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(6);
        ImGui::PopItemWidth();

        const auto searchBorderColor = searchActive ? HubColors::SearchBorderActive : HubColors::SearchBorder;
        drawList->AddRect(searchMin, searchMax, HubColorU32(searchBorderColor), 4.0f, 0, 1.2f);
        UI::Icons::DrawIcon(
            drawList,
            UI::Icons::IconId::Search,
            ImVec2(searchMin.x + 16.0f, (searchMin.y + searchMax.y) * 0.5f),
            14.0f,
            { HubColorU32(searchHovered || searchActive ? HubColors::TextSecondary : HubColors::TextMuted), 1.5f });
    }

    // ─── Table Header ───
    void DrawTableHeader(float x, float y, float width, const HubLayout::ProjectTableColumns& columns)
    {
        constexpr float kHeaderHeight = HubLayout::kTableHeaderHeight;
        const ImVec2 origin(x, y);
        const ImVec2 end(x + width, y + kHeaderHeight);
        auto* drawList = ImGui::GetWindowDrawList();

        drawList->AddRectFilled(origin, end, HubColorU32(HubColors::Surface));
        drawList->AddLine(ImVec2(origin.x, end.y), ImVec2(end.x, end.y), HubColorU32(HubColors::Border), 1.0f);

        const float padding = HubLayout::kProjectTablePadding;
        float colX = origin.x + padding;

        const float nameDividerX = colX + columns.name;
        const float modifiedDividerX = nameDividerX + columns.modified;
        const float editorDividerX = modifiedDividerX + columns.backend;

        drawList->AddLine(ImVec2(nameDividerX, origin.y), ImVec2(nameDividerX, end.y), HubColorU32(HubColors::BorderStrong), 1.0f);
        drawList->AddLine(ImVec2(modifiedDividerX, origin.y), ImVec2(modifiedDividerX, end.y), HubColorU32(HubColors::BorderStrong), 1.0f);
        drawList->AddLine(ImVec2(editorDividerX, origin.y), ImVec2(editorDividerX, end.y), HubColorU32(HubColors::BorderStrong), 1.0f);

        ImGui::SetCursorScreenPos(ImVec2(colX, y + 8.0f));
        DrawColumnHeader(Tr(LauncherTextKey::Name), SortColumn::Name);
        colX += columns.name;

        ImGui::SetCursorScreenPos(ImVec2(colX, y + 8.0f));
        DrawColumnHeader(Tr(LauncherTextKey::Modified), SortColumn::Modified);
        colX += columns.modified;

        ImGui::SetCursorScreenPos(ImVec2(colX, y + 8.0f));
        DrawColumnHeader(Tr(LauncherTextKey::EditorVersion), SortColumn::EditorVersion);
    }

    void DrawColumnHeader(const char* label, SortColumn col)
    {
        PushHubText(HubColors::TextSecondary);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0,0,0,0));

        char buf[128];
        ImFormatString(buf, sizeof(buf), "%s##ColHeader_%s", label, label);

        if (ImGui::SmallButton(buf))
        {
            if (m_sortColumn == col)
                m_sortAscending = !m_sortAscending;
            else
            {
                m_sortColumn = col;
                m_sortAscending = true;
            }
        }

        ImGui::SameLine(0.0f, 4.0f);
        if (m_sortColumn == col)
        {
            auto* drawList = ImGui::GetWindowDrawList();
            ImVec2 pos = ImGui::GetCursorScreenPos();
            float arrowY = pos.y + 4.0f;
            if (m_sortAscending)
            {
                drawList->AddTriangleFilled(
                    ImVec2(pos.x, arrowY + 8.0f),
                    ImVec2(pos.x + 8.0f, arrowY + 8.0f),
                    ImVec2(pos.x + 4.0f, arrowY),
                    HubColorU32(HubColors::Accent));
            }
            else
            {
                drawList->AddTriangleFilled(
                    ImVec2(pos.x, arrowY),
                    ImVec2(pos.x + 8.0f, arrowY),
                    ImVec2(pos.x + 4.0f, arrowY + 8.0f),
                    HubColorU32(HubColors::Accent));
            }
        }

        ImGui::PopStyleColor(4);
    }

    // ─── Project Row ───
    void DrawProjectRow(
        const std::string &projectPath,
        int index,
        float x,
        float y,
        float width,
        const HubLayout::ProjectTableColumns& columns)
    {
        constexpr float kRowHeight = HubLayout::kProjectRowHeight;
        constexpr float kActionButtonWidth = 34.0f;
        constexpr float kActionButtonHeight = 28.0f;
        const ImVec2 origin(x, y);
        const ImVec2 end(x + width, y + kRowHeight);
        auto* drawList = ImGui::GetWindowDrawList();

        const std::string projectName = Utils::PathParser::GetElementName(projectPath);
        const std::string editorVersion = DescribeProjectEditorVersionDisplay(projectPath);
        const bool isSelected = m_selectedProject == projectPath;
        ImGui::PushID(projectPath.c_str());

        const float mainAreaWidth = width - columns.actions;
        ImGui::SetCursorScreenPos(origin);
        ImGui::InvisibleButton("##RowClick", ImVec2(mainAreaWidth, kRowHeight));
        const bool rowHovered = ImGui::IsItemHovered();

        if (rowHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            OpenProject(projectPath);
            ImGui::PopID();
            return;
        }

        if (ImGui::IsItemClicked())
            m_selectedProject = projectPath;

        const ImVec2 actionButtonOrigin(
            origin.x + width - columns.actions + (columns.actions - kActionButtonWidth) * 0.5f,
            origin.y + (kRowHeight - kActionButtonHeight) * 0.5f);
        const ImVec2 actionButtonEnd(
            actionButtonOrigin.x + kActionButtonWidth,
            actionButtonOrigin.y + kActionButtonHeight);

        ImGui::SetCursorScreenPos(actionButtonOrigin);
        ImGui::InvisibleButton("##ProjectActionsButton", ImVec2(kActionButtonWidth, kActionButtonHeight));
        const bool actionHovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked())
        {
            m_selectedProject = projectPath;
            ImGui::OpenPopup("##ProjectActionsMenu");
        }

        const bool menuOpen = ImGui::IsPopupOpen("##ProjectActionsMenu");

        Maths::Color rowBg = (index % 2 == 0) ? HubColors::Background : HubColors::RowOdd;
        drawList->AddRectFilled(origin, end, HubColorU32(rowBg));
        if (isSelected)
        {
            drawList->AddRectFilled(origin, end, HubColorU32(HubColors::RowSelected));
            drawList->AddRectFilled(origin, ImVec2(origin.x + 3.0f, end.y), HubColorU32(HubColors::Accent));
        }
        else if (rowHovered || actionHovered || menuOpen)
        {
            drawList->AddRectFilled(origin, end, HubColorU32(HubColors::RowHover));
        }

        const float padding = HubLayout::kProjectTablePadding;
        float colX = origin.x + padding;

        // Name column
        {
            ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 10.0f));
            PushHubText(HubColors::TextPrimary);
            ImGui::TextUnformatted(projectName.c_str());
            PopHubText();

            ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 34.0f));
            PushHubText(HubColors::TextSecondary);
            ImGui::PushTextWrapPos(colX + columns.name - 18.0f);
            ImGui::TextUnformatted(projectPath.c_str());
            ImGui::PopTextWrapPos();
            PopHubText();
        }
        colX += columns.name;

        // Modified column
        {
            ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 22.0f));
            PushHubText(HubColors::TextSecondary);
            ImGui::TextUnformatted(GetRelativeTimeString(projectPath).c_str());
            PopHubText();
        }
        colX += columns.modified;

        // Editor version column
        {
            ImGui::SetCursorScreenPos(ImVec2(colX, origin.y + 22.0f));
            PushHubText(HubColors::TextSecondary);
            ImGui::TextUnformatted(editorVersion.c_str());
            PopHubText();
        }

        if (actionHovered || menuOpen)
            drawList->AddRectFilled(actionButtonOrigin, actionButtonEnd, HubColorU32(HubColors::SurfaceHover), 4.0f);

        UI::Icons::DrawIcon(
            drawList,
            UI::Icons::IconId::MoreHorizontal,
            ImVec2((actionButtonOrigin.x + actionButtonEnd.x) * 0.5f, (actionButtonOrigin.y + actionButtonEnd.y) * 0.5f),
            14.0f,
            { HubColorU32(HubColors::TextMuted), 1.5f });

        ImGui::SetNextWindowPos(ImVec2(actionButtonEnd.x - 240.0f, actionButtonEnd.y + 4.0f), ImGuiCond_Appearing);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, UI::Internal::Converter::ToImVec4(HubColors::Surface));
        ImGui::PushStyleColor(ImGuiCol_Border, UI::Internal::Converter::ToImVec4(HubColors::Border));
        ImGui::PushStyleColor(ImGuiCol_Header, UI::Internal::Converter::ToImVec4(HubColors::SurfaceHover));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, UI::Internal::Converter::ToImVec4(HubColors::SurfaceHover));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, UI::Internal::Converter::ToImVec4(HubColors::SurfaceActive));
        if (ImGui::BeginPopup("##ProjectActionsMenu"))
        {
            if (ImGui::MenuItem(Tr(LauncherTextKey::OpenInExplorer)))
                Platform::SystemCalls::ShowInExplorer(projectPath);

            ImGui::BeginDisabled();
            ImGui::MenuItem(Tr(LauncherTextKey::AddCommandLineArgs), nullptr, false, false);
            ImGui::EndDisabled();

            ImGui::Separator();
            if (ImGui::MenuItem(Tr(LauncherTextKey::RemoveFromList)))
                RemoveProject(projectPath);
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(2);

        ImGui::PopID();

        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(width, kRowHeight));
    }

    // ─── Empty State ───
    void DrawEmptyState(float x, float y, float width, float height)
    {
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height), HubColorU32(HubColors::Background));

        float centerX = x + width * 0.5f;
        float centerY = y + height * 0.5f;

        ImGui::SetCursorScreenPos(ImVec2(centerX - 60.0f, centerY - 30.0f));
        PushHubText(HubColors::TextSecondary);
        ImGui::TextUnformatted(Tr(LauncherTextKey::NoProjects));
        PopHubText();

        ImGui::SetCursorScreenPos(ImVec2(centerX - 65.0f, centerY + 4.0f));
        if (DrawHubButton(Tr(LauncherTextKey::NewProject), {130.0f, 32.0f}, HubColors::Accent, HubColors::AccentHover, HubColors::AccentActive, Maths::Color(1,1,1,0.95f)))
        {
            ShowWizard();
        }
    }

    void DrawInstallsContent(float x, float y, float width, float height)
    {
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + height), HubColorU32(HubColors::Background));

        auto addInstallVersion = [this]()
        {
            Dialogs::OpenFileDialog dialog(Tr(LauncherTextKey::SelectEngineExecutable), "", {"Executable", "*.exe"});
            if (dialog.Result().empty())
                return;

            if (m_settings.AddEngineExecutablePath(dialog.Result()[0]))
            {
                m_settings.SetDefaultEngineExecutablePath(dialog.Result()[0]);
                m_settings.Save();
                m_installError.clear();
            }
            else
            {
                m_installError = Tr(LauncherTextKey::InvalidEngineExecutable);
            }
        };

        const ImVec2 headerOrigin(x, y);
        const ImVec2 headerEnd(x + width, y + HubLayout::kActionBarHeight);
        drawList->AddRectFilled(headerOrigin, headerEnd, HubColorU32(HubColors::Background));
        drawList->AddLine(ImVec2(headerOrigin.x, headerEnd.y), ImVec2(headerEnd.x, headerEnd.y), HubColorU32(HubColors::Border), 1.0f);

        DrawHubPageTitle(ImVec2(headerOrigin.x + 32.0f, headerOrigin.y + 18.0f), Tr(LauncherTextKey::Installs));

        const ImVec2 addBtnSize(130.0f, 32.0f);
        ImGui::SetCursorScreenPos(ImVec2(headerEnd.x - addBtnSize.x - 32.0f, headerOrigin.y + 28.0f));
        if (DrawHubButton(Tr(LauncherTextKey::AddEngineExecutable), addBtnSize, HubColors::Accent, HubColors::AccentHover, HubColors::AccentActive, Maths::Color(1,1,1,0.95f)))
            addInstallVersion();

        DrawSectionTab(ImVec2(headerOrigin.x + 32.0f, headerOrigin.y + 92.0f), Tr(LauncherTextKey::Installs));

        constexpr float kHeaderHeight = HubLayout::kTableHeaderHeight;
        constexpr float kRowHeight = 64.0f;
        const float tableTop = y + HubLayout::kActionBarHeight;
        const float listTop = tableTop + kHeaderHeight;
        const float listHeight = (std::max)(0.0f, y + height - listTop);
        const float padding = 24.0f;
        const float versionWidth = width < 920.0f ? 180.0f : 220.0f;
        const float actionsWidth = width < 920.0f ? 188.0f : 220.0f;
        const float pathWidth = (std::max)(240.0f, width - padding - versionWidth - actionsWidth - padding);

        const ImVec2 tableHeaderMin(x, tableTop);
        const ImVec2 tableHeaderMax(x + width, tableTop + kHeaderHeight);
        drawList->AddRectFilled(tableHeaderMin, tableHeaderMax, HubColorU32(HubColors::Surface));
        drawList->AddLine(ImVec2(tableHeaderMin.x, tableHeaderMax.y), ImVec2(tableHeaderMax.x, tableHeaderMax.y), HubColorU32(HubColors::Border), 1.0f);

        const float versionDividerX = x + padding + versionWidth;
        const float pathDividerX = versionDividerX + pathWidth;
        drawList->AddLine(ImVec2(versionDividerX, tableHeaderMin.y), ImVec2(versionDividerX, tableHeaderMax.y), HubColorU32(HubColors::BorderStrong), 1.0f);
        drawList->AddLine(ImVec2(pathDividerX, tableHeaderMin.y), ImVec2(pathDividerX, tableHeaderMax.y), HubColorU32(HubColors::BorderStrong), 1.0f);

        ImGui::SetCursorScreenPos(ImVec2(x + padding, tableTop + 8.0f));
        PushHubText(HubColors::TextSecondary);
        ImGui::TextUnformatted(Tr(LauncherTextKey::EditorVersion));
        PopHubText();

        ImGui::SetCursorScreenPos(ImVec2(versionDividerX + 16.0f, tableTop + 8.0f));
        PushHubText(HubColors::TextSecondary);
        ImGui::TextUnformatted(Tr(LauncherTextKey::EngineExecutable));
        PopHubText();

        const auto installViews = m_settings.GetEngineInstallationViews();

        ImGui::SetCursorScreenPos(ImVec2(x, listTop));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("##InstallsList", ImVec2(width, listHeight), false, ImGuiWindowFlags_None);

        if (installViews.empty())
        {
            ImGui::SetCursorScreenPos(ImVec2(x + 32.0f, listTop + 20.0f));
            PushHubText(HubColors::TextMuted);
            ImGui::TextUnformatted(Tr(LauncherTextKey::NoInstalledVersions));
            PopHubText();
        }
        else
        {
            for (size_t index = 0; index < installViews.size(); ++index)
            {
                const auto& install = installViews[index];
                const ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
                const float rowWidth = width;
                const ImVec2 rowEnd(rowOrigin.x + rowWidth, rowOrigin.y + kRowHeight);
                const Maths::Color rowBg = (index % 2 == 0) ? HubColors::Background : HubColors::RowOdd;

                drawList->AddRectFilled(rowOrigin, rowEnd, HubColorU32(rowBg));
                drawList->AddLine(ImVec2(versionDividerX, rowOrigin.y), ImVec2(versionDividerX, rowEnd.y), HubColorU32(HubColors::Border), 1.0f);
                drawList->AddLine(ImVec2(pathDividerX, rowOrigin.y), ImVec2(pathDividerX, rowEnd.y), HubColorU32(HubColors::Border), 1.0f);

                ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x + padding, rowOrigin.y + 10.0f));
                PushHubText(HubColors::TextPrimary);
                const std::string versionText = install.versionLabel.empty()
                    ? std::string(Tr(LauncherTextKey::InvalidInstalledVersion))
                    : install.versionLabel;
                ImGui::TextUnformatted(versionText.c_str());
                PopHubText();

                if (install.isDefault)
                {
                    ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x + padding, rowOrigin.y + 34.0f));
                    PushHubText(HubColors::Accent);
                    ImGui::TextUnformatted(Tr(LauncherTextKey::DefaultEngineExecutable));
                    PopHubText();
                }
                else if (!install.isValid)
                {
                    ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x + padding, rowOrigin.y + 34.0f));
                    PushHubText(HubColors::Danger);
                    ImGui::TextUnformatted(Tr(LauncherTextKey::InvalidInstalledVersion));
                    PopHubText();
                }

                ImGui::SetCursorScreenPos(ImVec2(versionDividerX + 16.0f, rowOrigin.y + 20.0f));
                PushHubText(install.isValid ? HubColors::TextSecondary : HubColors::Danger);
                ImGui::PushTextWrapPos(pathDividerX - 20.0f);
                ImGui::TextWrapped("%s", install.executablePath.c_str());
                ImGui::PopTextWrapPos();
                PopHubText();

                if (!install.isDefault)
                {
                    ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x + rowWidth - 176.0f, rowOrigin.y + 18.0f));
                    if (DrawHubButton(Tr(LauncherTextKey::DefaultEngineExecutable), {84.0f, 28.0f}, HubColors::Surface, HubColors::SurfaceHover, HubColors::SurfaceActive, HubColors::TextPrimary))
                    {
                        if (m_settings.SetDefaultEngineExecutablePath(install.executablePath))
                        {
                            m_settings.Save();
                            m_installError.clear();
                        }
                    }
                }

                ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x + rowWidth - 86.0f, rowOrigin.y + 18.0f));
                if (DrawHubButton(Tr(LauncherTextKey::RemoveEngineExecutable), {78.0f, 28.0f}, HubColors::Surface, HubColors::SurfaceHover, HubColors::SurfaceActive, HubColors::TextPrimary))
                {
                    m_settings.RemoveEngineExecutablePath(install.executablePath);
                    m_settings.Save();
                    break;
                }

                ImGui::Dummy(ImVec2(rowWidth, kRowHeight));
            }
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();

        if (!m_installError.empty())
        {
            ImGui::SetCursorScreenPos(ImVec2(x + 32.0f, y + height - 28.0f));
            PushHubText(HubColors::Danger);
            ImGui::TextUnformatted(m_installError.c_str());
            PopHubText();
        }
    }

    // ─── Main Launcher Draw ───
    void DrawLauncher()
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("LauncherRoot", ImGui::GetContentRegionAvail(), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();

        const ImVec2 rootOrigin = ImGui::GetCursorScreenPos();
        const float fullWidth = ImGui::GetContentRegionAvail().x;
        const float fullHeight = ImGui::GetContentRegionAvail().y;
        const float brandWidth = HubLayout::kBrandRailWidth;
        const float navWidth = HubLayout::kNavigationWidth;
        const float contentX = rootOrigin.x + brandWidth + navWidth;
        const float contentWidth = (std::max)(0.0f, fullWidth - brandWidth - navWidth);
        const float tableTop = rootOrigin.y + HubLayout::kActionBarHeight;
        const float headerHeight = HubLayout::kTableHeaderHeight;
        const float listTop = tableTop + headerHeight;
        const float listHeight = (std::max)(0.0f, rootOrigin.y + fullHeight - listTop);
        const auto columns = HubLayout::CalculateProjectTableColumns(contentWidth);

        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(rootOrigin, ImVec2(rootOrigin.x + fullWidth, rootOrigin.y + fullHeight), HubColorU32(HubColors::Background));
        drawList->AddRectFilled(
            rootOrigin,
            ImVec2(rootOrigin.x + brandWidth + navWidth, rootOrigin.y + fullHeight),
            HubColorU32(HubColors::Surface));

        DrawBrandRail(rootOrigin.x, rootOrigin.y, brandWidth, fullHeight);
        DrawNavigationRail(rootOrigin.x + brandWidth, rootOrigin.y, navWidth, fullHeight);

        if (m_currentView == LauncherView::Installs)
        {
            DrawInstallsContent(contentX, rootOrigin.y, contentWidth, fullHeight);
            ImGui::EndChild();
            return;
        }

        DrawActionBar(contentX, rootOrigin.y, contentWidth);

        // Table header
        DrawTableHeader(contentX, tableTop, contentWidth, columns);

        // Filter projects by search
        std::vector<std::pair<int, std::string>> filteredProjects;
        std::string searchStr(m_searchBuffer);
        std::transform(searchStr.begin(), searchStr.end(), searchStr.begin(), ::tolower);

        for (int i = 0; i < static_cast<int>(m_registeredProjects.size()); ++i)
        {
            const auto& path = m_registeredProjects[i];
            if (!searchStr.empty())
            {
                std::string name = Utils::PathParser::GetElementName(path);
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                std::string pathLower = path;
                std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), ::tolower);
                if (name.find(searchStr) == std::string::npos && pathLower.find(searchStr) == std::string::npos)
                    continue;
            }
            filteredProjects.push_back({i, path});
        }

        // Sort filtered projects
        {
            const bool asc = m_sortAscending;
            std::sort(filteredProjects.begin(), filteredProjects.end(),
                [this, asc](const std::pair<int, std::string>& a, const std::pair<int, std::string>& b)
                {
                    int cmp = 0;
                    switch (m_sortColumn)
                    {
                    case SortColumn::Name:
                    {
                        std::string nameA = Utils::PathParser::GetElementName(a.second);
                        std::string nameB = Utils::PathParser::GetElementName(b.second);
                        std::transform(nameA.begin(), nameA.end(), nameA.begin(), ::tolower);
                        std::transform(nameB.begin(), nameB.end(), nameB.begin(), ::tolower);
                        cmp = nameA.compare(nameB);
                        break;
                    }
                    case SortColumn::Modified:
                    {
                        std::error_code ec;
                        auto timeA = std::filesystem::last_write_time(a.second, ec);
                        auto timeB = std::filesystem::last_write_time(b.second, ec);
                        cmp = (timeA < timeB) ? -1 : (timeA > timeB) ? 1 : 0;
                        break;
                    }
                    case SortColumn::EditorVersion:
                    {
                        const std::string versionA = DescribeProjectEditorVersionDisplay(a.second);
                        const std::string versionB = DescribeProjectEditorVersionDisplay(b.second);
                        cmp = versionA.compare(versionB);
                        break;
                    }
                    }
                    return asc ? cmp < 0 : cmp > 0;
                });
        }

        // Project list
        if (filteredProjects.empty())
        {
            DrawEmptyState(contentX, listTop, contentWidth, listHeight);
        }
        else
        {
            ImGui::SetCursorScreenPos(ImVec2(contentX, listTop));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::BeginChild("ProjectList", ImVec2(contentWidth, listHeight), false, ImGuiWindowFlags_None);

            const ImVec2 listOrigin = ImGui::GetCursorScreenPos();
            float scrollY = 0.0f;
            for (int i = 0; i < static_cast<int>(filteredProjects.size()); ++i)
            {
                const float rowY = listOrigin.y + scrollY;
                ImGui::SetCursorScreenPos(ImVec2(listOrigin.x, rowY));
                DrawProjectRow(filteredProjects[i].second, i, listOrigin.x, rowY, contentWidth, columns);
                scrollY += HubLayout::kProjectRowHeight;
            }

            ImGui::Dummy(ImVec2(contentWidth, scrollY));

            ImGui::EndChild();
            ImGui::PopStyleVar();
        }

        ImGui::EndChild();
    }

private:
    Windowing::Window &m_window;
    bool &m_readyToGo;
    std::string &m_path;
    std::string &m_projectName;
    std::string &m_editorExecutablePath;
    std::shared_ptr<Render::RHI::RHITextureView> m_brandTextureView;
    std::vector<std::string> m_registeredProjects;
    std::string m_selectedProject;

    // View state
    LauncherView m_currentView = LauncherView::Projects;
    std::unique_ptr<ProjectCreationWizard> m_wizard;
    TemplateManager& m_templateManager;
    LauncherSettings m_settings{"launcher.ini"};
    std::string m_installError;

    // Search & sort
    char m_searchBuffer[128] = {};
    SortColumn m_sortColumn = SortColumn::Modified;
    bool m_sortAscending = false;
};

Render::Settings::EGraphicsBackend ResolveLauncherGraphicsBackend()
{
    const auto resolvedBackend = Render::Settings::GetPlatformDefaultGraphicsBackend();

    if (Render::Settings::HasCompiledOfficialImGuiBackend(resolvedBackend))
        return resolvedBackend;

    NLS_LOG_WARNING(
        "Launcher UI backend is not compiled for " +
        std::string(Render::Settings::ToString(resolvedBackend)) +
        " to OpenGL.");
    return Render::Settings::EGraphicsBackend::OPENGL;
}

Launcher::Launcher(
    std::optional<Render::Settings::EGraphicsBackend> backendOverride,
    const Render::Settings::RenderDocSettings& renderDocSettings)
    : m_backendOverride(backendOverride)
    , m_renderDocSettings(renderDocSettings)
{
    SetupContext();
    m_mainPanel = std::make_unique<LauncherPanel>(*m_window, m_readyToGo, m_projectPath, m_projectName, m_editorExecutablePath, m_brandTextureView, m_templateManager);

    m_uiManager->SetCanvas(m_canvas);
    m_canvas.AddPanel(*m_mainPanel);
}

Launcher::~Launcher()
{
    m_brandTextureView.reset();
    NLS::Render::Resources::Loaders::TextureLoader::Destroy(m_brandTextureResource);
}

LauncherRunResult Launcher::Run()
{
    while (!m_window->ShouldClose())
    {
        m_device->PollEvents();

        if (Render::Context::DriverRendererAccess::HasExplicitRHI(*m_driver))
        {
            Render::Context::DriverRendererAccess::BeginExplicitFrame(*m_driver, true);
        }

        m_uiManager->Render();
        m_uiManager->SubmitUIRendering();

        if (Render::Context::DriverRendererAccess::HasExplicitRHI(*m_driver))
        {
            Render::Context::DriverRendererAccess::EndExplicitFrame(*m_driver, true);
        }
        else
        {
            Render::Context::DriverUIAccess::PresentSwapchain(*m_driver);
        }

        if (!m_mainPanel->IsOpened())
            m_window->SetShouldClose(true);
    }

    return {m_readyToGo, m_projectPath, m_projectName, m_editorExecutablePath};
}

void Launcher::SetupContext()
{
    GetLauncherLocalization().Load(std::filesystem::path("../Assets/Localization/Launcher"), ResolveLauncherLocale());

    Windowing::Settings::DeviceSettings deviceSettings;
    Windowing::Settings::WindowSettings windowSettings;
    windowSettings.title = Tr(LauncherTextKey::LauncherTitle);
    windowSettings.width = static_cast<uint16_t>(HubLayout::kDefaultWindowWidth);
    windowSettings.height = static_cast<uint16_t>(HubLayout::kDefaultWindowHeight);
    windowSettings.minimumWidth = static_cast<int16_t>(HubLayout::kMinimumWindowWidth);
    windowSettings.minimumHeight = static_cast<int16_t>(HubLayout::kMinimumWindowHeight);
    windowSettings.maximized = false;
    windowSettings.resizable = true;
    windowSettings.decorated = true; // Use native OS title bar
    if (m_backendOverride.has_value())
    {
        m_graphicsBackend = m_backendOverride.value();
        NLS_LOG_INFO("Launcher using command-line backend override: " + std::string(Render::Settings::ToString(m_graphicsBackend)));
    }
    else
    {
        m_graphicsBackend = ResolveLauncherGraphicsBackend();
    }
    windowSettings.clientAPI = m_graphicsBackend == Render::Settings::EGraphicsBackend::OPENGL
        ? Windowing::Settings::WindowClientAPI::OpenGL
        : Windowing::Settings::WindowClientAPI::NoAPI;

    Render::Settings::DriverSettings driverSettings;
    driverSettings.graphicsBackend = m_graphicsBackend;
    driverSettings.debugMode = false;
    if (m_renderDocSettings.enabled || m_renderDocSettings.startupCaptureAfterFrames > 0)
    {
        driverSettings.renderDoc = m_renderDocSettings;
        NLS_LOG_INFO("Launcher RenderDoc: applied command-line settings (enabled=" +
            std::string(m_renderDocSettings.enabled ? "true" : "false") +
            ", captureAfterFrames=" + std::to_string(m_renderDocSettings.startupCaptureAfterFrames) + ")");
    }
    Render::Tooling::ApplyRenderDocEnvironmentOverrides(
        driverSettings.renderDoc,
        (std::filesystem::current_path() / "Logs" / "RenderDoc" / "Launcher").string(),
        "Launcher");
    Render::Tooling::PreloadRenderDocIfAvailable(driverSettings.renderDoc);

    m_device = std::make_unique<Context::Device>(deviceSettings);
    m_window = std::make_unique<Windowing::Window>(*m_device, windowSettings);
    if (m_graphicsBackend == Render::Settings::EGraphicsBackend::OPENGL)
        m_window->MakeCurrentContext();
    const std::string brandIconPath = std::filesystem::canonical(std::filesystem::path("../Assets/Engine/Brand/NullusLogoMark.png")).string();
    m_window->SetIcon(brandIconPath);

    auto monSize = m_device->GetMonitorSize();
    auto winSize = m_window->GetSize();
    m_window->SetPosition(monSize.x / 2 - winSize.x / 2, monSize.y / 2 - winSize.y / 2);

    m_driver = std::make_unique<Render::Context::Driver>(driverSettings);
    NLS::Core::ServiceLocator::Provide<Render::Context::Driver>(*m_driver);

    m_driver->CreatePlatformSwapchain(
        m_window->GetGlfwWindow(),
        m_window->GetNativeWindowHandle(),
        static_cast<uint32_t>(windowSettings.width),
        static_cast<uint32_t>(windowSettings.height),
        true);

    auto nativeDeviceInfo = m_driver->GetNativeDeviceInfo();
    m_uiManager = std::make_unique<UI::UIManager>(
        m_window->GetGlfwWindow(),
        driverSettings.graphicsBackend,
        UI::EStyle::ALTERNATIVE_DARK,
        "#version 150",
        &nativeDeviceInfo);
    m_driver->SetSwapchainWillResizeCallback([this]()
    {
        if (m_uiManager != nullptr)
            m_uiManager->NotifySwapchainWillResize();
    });
    NLS::Core::ServiceLocator::Provide<UI::UIManager>(*m_uiManager);

    // Load fonts with CJK support
    {
        auto& io = ImGui::GetIO();
        // Load CJK font (Microsoft YaHei on Windows, fall back to Ruda on other platforms)
#ifdef _WIN32
        // Windows: use system font msyh.ttc (Microsoft YaHei) with CJK glyph range
        const char* cjkFontPath = "C:\\Windows\\Fonts\\msyh.ttc";
        ImFont* cjkFont = io.Fonts->AddFontFromFileTTF(cjkFontPath, 18.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
        if (cjkFont)
        {
            m_uiManager->LoadFont("Ruda_Title", "../Assets/Editor/Fonts/Ruda-Bold.ttf", 30);
            // Use the CJK font as the primary font
            io.FontDefault = cjkFont;
        }
        else
#endif
        {
            // Fallback: load Ruda without CJK support
            m_uiManager->LoadFont("Ruda_Medium", "../Assets/Editor/Fonts/Ruda-Bold.ttf", 18);
            m_uiManager->LoadFont("Ruda_Title", "../Assets/Editor/Fonts/Ruda-Bold.ttf", 30);
            m_uiManager->UseFont("Ruda_Medium");
        }
    }

    auto resizeSwapchain = [this](uint16_t width, uint16_t height)
    {
        if (m_driver != nullptr && width > 0u && height > 0u)
        {
            m_driver->ResizePlatformSwapchain(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        }
    };
    m_window->ResizeEvent.AddListener(resizeSwapchain);
    m_window->FramebufferResizeEvent.AddListener(resizeSwapchain);
    m_uiManager->EnableEditorLayoutSave(false);
    m_uiManager->EnableDocking(false);

    m_brandTextureResource = NLS::Render::Resources::Loaders::TextureLoader::Create(
        brandIconPath,
        NLS::Render::Settings::ETextureFilteringMode::LINEAR,
        NLS::Render::Settings::ETextureFilteringMode::LINEAR,
        false);
    m_brandTextureView = m_brandTextureResource != nullptr
        ? m_brandTextureResource->GetOrCreateExplicitTextureView("Launcher.BrandTexture")
        : nullptr;

    // Load project templates
    {
        auto templatePath = std::filesystem::path("../Assets/Templates");
        std::error_code ec;
        auto resolvedPath = std::filesystem::canonical(templatePath, ec);
        if (!ec && std::filesystem::exists(resolvedPath))
        {
            m_templateManager.LoadTemplates(resolvedPath);
        }
        else
        {
            NLS_LOG_WARNING("TemplateManager: Templates directory not found at " + templatePath.string() + ", skipping template loading");
        }
    }
}

void Launcher::RegisterProject(const std::string &p_path)
{
    static_cast<LauncherPanel *>(m_mainPanel.get())->RegisterProject(p_path);
}
} // namespace NLS

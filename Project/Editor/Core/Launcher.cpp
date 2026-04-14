#include "Launcher.h"
#define _CRT_SECURE_NO_WARNINGS

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#include <Core/ServiceLocator.h>
#include <Debug/Logger.h>
#include <Filesystem/IniFile.h>
#include <Image.h>
#include <UI/Internal/Converter.h>
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

#include "ImGui/imgui_internal.h"

#define PROJECTS_FILE "projects.ini"

namespace NLS
{
namespace
{
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

ImU32 ToU32(const Maths::Color &color)
{
    return ImGui::GetColorU32(UI::Internal::Converter::ToImVec4(color));
}

bool DrawActionButton(const char *label, const ImVec2 &size, const Maths::Color &idle, const Maths::Color &hovered, const Maths::Color &active, const Maths::Color &textColor)
{
    ImGui::PushStyleColor(ImGuiCol_Button, UI::Internal::Converter::ToImVec4(idle));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UI::Internal::Converter::ToImVec4(hovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, UI::Internal::Converter::ToImVec4(active));
    ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4(textColor));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 9.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    return clicked;
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
        min,
        max,
        ImVec2(0.0f, 1.0f),
        ImVec2(1.0f, 0.0f),
        ImGui::GetColorU32(tint));
}

enum class TitleBarGlyph
{
    Minimize,
    Close
};

bool DrawTitleBarButton(const char *id, TitleBarGlyph glyph, const ImVec2 &size, const Maths::Color &hovered, const Maths::Color &active, const Maths::Color &strokeColor)
{
    ImGui::PushID(id);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##titlebar_button", size);

    auto *drawList = ImGui::GetWindowDrawList();
    const bool hoveredState = ImGui::IsItemHovered();
    const bool activeState = ImGui::IsItemActive();
    const ImVec2 buttonMax{pos.x + size.x, pos.y + size.y};

    if (hoveredState || activeState)
    {
        const Maths::Color background = activeState ? active : hovered;
        drawList->AddRectFilled(pos, buttonMax, ToU32(background), 8.0f);
    }
    
    const ImU32 stroke = ToU32(strokeColor);
    if (glyph == TitleBarGlyph::Minimize)
    {
        const float y = pos.y + size.y * 0.62f;
        drawList->AddLine(
            ImVec2(pos.x + 10.0f, y),
            ImVec2(pos.x + size.x - 10.0f, y),
            stroke,
            1.6f);
    }
    else
    {
        const float inset = 10.0f;
        drawList->AddLine(
            ImVec2(pos.x + inset, pos.y + inset),
            ImVec2(pos.x + size.x - inset, pos.y + size.y - inset),
            stroke,
            1.6f);
        drawList->AddLine(
            ImVec2(pos.x + size.x - inset, pos.y + inset),
            ImVec2(pos.x + inset, pos.y + size.y - inset),
            stroke,
            1.6f);
    }

    ImGui::PopID();
    return clicked;
}

std::string FormatTimestamp(std::filesystem::file_time_type value)
{
    using namespace std::chrono;
    const auto systemTime = time_point_cast<system_clock::duration>(
        value - std::filesystem::file_time_type::clock::now() + system_clock::now());
    const std::time_t time = system_clock::to_time_t(systemTime);

    std::tm localTm {};
#ifdef _WIN32
    localtime_s(&localTm, &time);
#else
    localTm = *std::localtime(&time);
#endif

    std::ostringstream oss;
    oss << std::put_time(&localTm, "%Y-%m-%d %H:%M");
    return oss.str();
}

std::string GetProjectUpdatedTimestamp(const std::string& projectPath)
{
    std::error_code errorCode;
    const auto writeTime = std::filesystem::last_write_time(projectPath, errorCode);
    if (errorCode)
        return "Unknown";
    return FormatTimestamp(writeTime);
}

std::string DetectStartScene(const std::string &projectPath)
{
    const std::filesystem::path assetsPath = std::filesystem::path(projectPath) / "Assets";
    if (!std::filesystem::exists(assetsPath))
        return "No scene found";

    for (const auto &entry : std::filesystem::directory_iterator(assetsPath))
    {
        if (entry.is_regular_file())
        {
            const auto ext = entry.path().extension().string();
            if (ext == ".ovscene" || ext == ".scene")
                return entry.path().filename().string();
        }
    }

    return "No scene found";
}

struct ProjectBackendResolution
{
    Render::Settings::EGraphicsBackend backend = Render::Settings::GetPlatformDefaultGraphicsBackend();
    const char* source = "Platform default";
};

ProjectBackendResolution ResolveProjectRuntimeBackend(const std::string& projectPath)
{
    const std::filesystem::path projectRoot(projectPath);
    const auto projectFilePath = projectRoot / (projectRoot.filename().string() + ".nullus");
    if (std::filesystem::exists(projectFilePath))
    {
        NLS::Filesystem::IniFile projectSettings(projectFilePath.string());
        if (projectSettings.IsKeyExisting("graphics_backend"))
        {
            const auto configuredBackend = projectSettings.Get<std::string>("graphics_backend");
            if (const auto parsedBackend = Render::Settings::TryParseGraphicsBackend(configuredBackend); parsedBackend.has_value())
                return { parsedBackend.value(), "Project setting" };

            return {
                Render::Settings::GetPlatformDefaultGraphicsBackend(),
                "Invalid project setting, using platform default"
            };
        }
    }

    return {};
}

std::string DescribeProjectRuntimeBackend(const std::string& projectPath)
{
    const auto resolution = ResolveProjectRuntimeBackend(projectPath);
    return std::string(Render::Settings::ToString(resolution.backend)) + " (" + resolution.source + ")";
}
} // namespace

class LauncherPanel : public UI::PanelWindow
{
public:
    LauncherPanel(
        Windowing::Window &p_window,
        bool &p_readyToGo,
        std::string &p_path,
        std::string &p_projectName,
        std::shared_ptr<Render::RHI::RHITextureView> p_brandTextureView)
        : PanelWindow("Nullus - Launcher", true)
        , m_window(p_window)
        , m_readyToGo(p_readyToGo)
        , m_path(p_path)
        , m_projectName(p_projectName)
        , m_brandTextureView(p_brandTextureView)
    {
        panelSettings.resizable = false;
        panelSettings.movable = false;
        panelSettings.titleBar = false;
        panelSettings.closable = false;

        SetSize({1000, 580});
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
    }

    void Draw() override
    {
        if (!IsOpened())
            return;

        const int windowFlags = ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_NoTitleBar |
                                ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::SetNextWindowSize(UI::Internal::Converter::ToImVec2(GetSize()), ImGuiCond_Always);
        ImGui::SetNextWindowPos(UI::Internal::Converter::ToImVec2(GetPosition()), ImGuiCond_Always);

        if (!ImGui::Begin(name.c_str(), nullptr, windowFlags))
        {
            ImGui::End();
            return;
        }

        DrawLauncher();
        ImGui::End();
    }

    void CreateProject(const std::string &p_path)
    {
        if (!std::filesystem::exists(p_path))
            std::filesystem::create_directory(p_path);

        if (!std::filesystem::exists(p_path + Utils::PathParser::Separator() + "Assets"))
            std::filesystem::create_directory(p_path + Utils::PathParser::Separator() + "Assets");

        if (!std::filesystem::exists(p_path + Utils::PathParser::Separator() + "Logs"))
            std::filesystem::create_directory(p_path + Utils::PathParser::Separator() + "Logs");

        if (!std::filesystem::exists(p_path + Utils::PathParser::Separator() + "UserSetting"))
            std::filesystem::create_directory(p_path + Utils::PathParser::Separator() + "UserSettings");

        if (!std::filesystem::exists(p_path + Utils::PathParser::Separator() + "ProjectSettings"))
            std::filesystem::create_directory(p_path + Utils::PathParser::Separator() + "ProjectSettings");

        std::ofstream projectFile(p_path + Utils::PathParser::Separator() + Utils::PathParser::GetElementName(p_path) + ".nullus");
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
            MessageBox errorMessage("Project not found", "The selected project does not exists", MessageBox::EMessageType::ERROR, MessageBox::EButtonLayout::OK);
            return;
        }

        m_path = p_path;
        m_projectName = Utils::PathParser::GetElementName(m_path);
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

    void RemoveProject(const std::string &p_path)
    {
        m_registeredProjects.erase(
            std::remove(m_registeredProjects.begin(), m_registeredProjects.end(), p_path),
            m_registeredProjects.end());

        if (m_selectedProject == p_path)
            m_selectedProject = m_registeredProjects.empty() ? std::string{} : m_registeredProjects.front();

        FlushProjectsFile();
    }

    void DrawHeaderCard(float availableWidth)
    {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float cardHeight = 140.0f;
        const ImVec2 end(start.x + availableWidth, start.y + cardHeight);

        auto *drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(start, end, ToU32({0.125f, 0.145f, 0.17f, 1.0f}), 16.0f);
        drawList->AddRect(start, end, ToU32({0.20f, 0.23f, 0.27f, 1.0f}), 16.0f, 0, 1.0f);

        DrawTexture(ImVec2(start.x + 28.0f, start.y + 28.0f), ImVec2(start.x + 88.0f, start.y + 88.0f), m_brandTextureView);

        ImGui::SetCursorScreenPos(ImVec2(start.x + 104.0f, start.y + 26.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4({0.93f, 0.96f, 0.99f, 1.0f}));
        if (ImGui::GetIO().Fonts->Fonts.Size > 1)
            ImGui::PushFont(ImGui::GetIO().Fonts->Fonts.back());
        ImGui::TextUnformatted("Nullus Launcher");
        if (ImGui::GetIO().Fonts->Fonts.Size > 1)
            ImGui::PopFont();
        ImGui::PopStyleColor();

        ImGui::SetCursorScreenPos(ImVec2(start.x + 104.0f, start.y + 78.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4({0.56f, 0.62f, 0.71f, 1.0f}));
        ImGui::PushTextWrapPos(start.x + availableWidth - 320.0f);
        ImGui::TextWrapped("Continue where you left off, open an existing project, or create a clean new workspace.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::SetCursorScreenPos(ImVec2(start.x + availableWidth - 280.0f, start.y + 38.0f));
        if (DrawActionButton("New Project", {124.0f, 42.0f}, {0.24f, 0.48f, 0.84f, 1.0f}, {0.30f, 0.57f, 0.94f, 1.0f}, {0.19f, 0.41f, 0.74f, 1.0f}, {0.96f, 0.97f, 0.99f, 1.0f}))
        {
            Dialogs::SelectFolderDialog dialog("New Project Location");
            std::string result = dialog.Result();
            if (!result.empty())
            {
                CreateProject(result);
                RegisterProject(result);
            }
        }

        ImGui::SameLine(0.0f, 12.0f);
        if (DrawActionButton("Open Project", {124.0f, 42.0f}, {0.18f, 0.19f, 0.22f, 1.0f}, {0.22f, 0.24f, 0.28f, 1.0f}, {0.25f, 0.28f, 0.33f, 1.0f}, {0.89f, 0.92f, 0.96f, 1.0f}))
        {
            Dialogs::OpenFileDialog dialog("Open project", "", {"Nullus Project", "*.nullus"});
            if (!dialog.Result().empty())
            {
                const std::string projectFile = dialog.Result()[0];
                const std::string rootFolderPath = std::filesystem::path(projectFile).parent_path().string();
                RegisterProject(rootFolderPath);
                OpenProject(rootFolderPath);
            }
        }

        ImGui::Dummy(ImVec2(availableWidth, cardHeight));
    }

    void DrawTitleBar(float width)
    {
        constexpr float kTitleBarHeight = 42.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 end(origin.x + width, origin.y + kTitleBarHeight);
        auto *drawList = ImGui::GetWindowDrawList();

        drawList->AddRectFilled(origin, end, ToU32({0.11f, 0.12f, 0.15f, 1.0f}));
        drawList->AddLine(ImVec2(origin.x, end.y), ImVec2(end.x, end.y), ToU32({0.20f, 0.23f, 0.27f, 1.0f}), 1.0f);

        DrawTexture(ImVec2(origin.x + 12.0f, origin.y + 7.0f), ImVec2(origin.x + 34.0f, origin.y + 29.0f), m_brandTextureView);

        ImGui::SetCursorScreenPos(ImVec2(origin.x + 42.0f, origin.y + 10.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4({0.88f, 0.92f, 0.97f, 1.0f}));
        ImGui::TextUnformatted("Nullus Launcher");
        ImGui::PopStyleColor();

        const float buttonWidth = 38.0f;
        const float buttonSpacing = 6.0f;
        const float buttonsWidth = buttonWidth * 2.0f + buttonSpacing;

#ifndef _WIN32
        const ImVec2 dragAreaSize(width - buttonsWidth - 12.0f, kTitleBarHeight);
        ImGui::SetCursorScreenPos(origin);
        ImGui::InvisibleButton("##LauncherTitleDrag", dragAreaSize);
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            const ImVec2 mouseDelta = ImGui::GetIO().MouseDelta;
            const auto position = m_window.GetPosition();
            m_window.SetPosition(
                static_cast<int16_t>(position.x + mouseDelta.x),
                static_cast<int16_t>(position.y + mouseDelta.y));
        }
#endif

        ImGui::SetCursorScreenPos(ImVec2(origin.x + width - buttonsWidth - 10.0f, origin.y + 4.0f));
        if (DrawTitleBarButton("Minimize", TitleBarGlyph::Minimize, {buttonWidth, 34.0f}, {0.18f, 0.20f, 0.24f, 1.0f}, {0.23f, 0.25f, 0.30f, 1.0f}, {0.77f, 0.82f, 0.89f, 1.0f}))
            m_window.Minimize();
        ImGui::SameLine(0.0f, buttonSpacing);
        if (DrawTitleBarButton("Close", TitleBarGlyph::Close, {buttonWidth, 34.0f}, {0.47f, 0.18f, 0.20f, 1.0f}, {0.39f, 0.13f, 0.15f, 1.0f}, {0.95f, 0.95f, 0.96f, 1.0f}))
        {
            m_window.SetShouldClose(true);
            Close();
        }

        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(width, kTitleBarHeight));
    }

    void DrawProjectDetailsCard(const ImVec2 &origin, float width)
    {
        const float height = 280.0f;
        const ImVec2 end(origin.x + width, origin.y + height);
        auto *drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(origin, end, ToU32({0.125f, 0.145f, 0.17f, 1.0f}), 16.0f);
        drawList->AddRect(origin, end, ToU32({0.20f, 0.23f, 0.27f, 1.0f}), 16.0f, 0, 1.0f);

        ImGui::SetCursorScreenPos(ImVec2(origin.x + 24.0f, origin.y + 24.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4({0.69f, 0.73f, 0.79f, 1.0f}));
        ImGui::TextUnformatted("PROJECT DETAILS");
        ImGui::PopStyleColor();

        if (m_selectedProject.empty() || !std::filesystem::exists(m_selectedProject))
        {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + 24.0f, origin.y + 60.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4({0.56f, 0.62f, 0.71f, 1.0f}));
            ImGui::PushTextWrapPos(origin.x + width - 24.0f);
            ImGui::TextWrapped("Select a recent project on the left to inspect its details.");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::SetCursorScreenPos(origin);
            ImGui::Dummy(ImVec2(width, height));
            return;
        }

        if (m_cachedProjectDetailsPath != m_selectedProject)
        {
            m_cachedProjectDetailsPath = m_selectedProject;
            m_cachedProjectRuntimeBackend = DescribeProjectRuntimeBackend(m_selectedProject);
            m_cachedProjectStartScene = DetectStartScene(m_selectedProject);
            m_cachedProjectUpdatedTime = GetProjectUpdatedTimestamp(m_selectedProject);
        }

        const std::array<std::pair<const char *, std::string>, 4> details =
        {{
            {"Project", Utils::PathParser::GetElementName(m_selectedProject)},
            {"Scene", m_cachedProjectStartScene},
            {"Runtime Backend", m_cachedProjectRuntimeBackend},
            {"Updated", m_cachedProjectUpdatedTime}
        }};

        float y = origin.y + 62.0f;
        for (const auto &[label, value] : details)
        {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + 24.0f, y));
            ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4({0.55f, 0.61f, 0.69f, 1.0f}));
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();

            ImGui::SetCursorScreenPos(ImVec2(origin.x + 24.0f, y + 20.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4({0.93f, 0.96f, 0.99f, 1.0f}));
            ImGui::PushTextWrapPos(origin.x + width - 24.0f);
            ImGui::TextWrapped("%s", value.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();

            y += 52.0f;
        }

        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(width, height));
    }

    void DrawProjectCard(const std::string &projectPath, const ImVec2 &origin, float width)
    {
        const float height = 128.0f;
        const ImVec2 end(origin.x + width, origin.y + height);
        auto *drawList = ImGui::GetWindowDrawList();

        drawList->AddRectFilled(origin, end, ToU32({0.125f, 0.145f, 0.17f, 1.0f}), 16.0f);
        drawList->AddRect(origin, end, ToU32({0.20f, 0.23f, 0.27f, 1.0f}), 16.0f, 0, 1.0f);

        const std::string projectName = Utils::PathParser::GetElementName(projectPath);
        const bool isSelected = m_selectedProject == projectPath;

        if (isSelected)
            drawList->AddRect(origin, end, ToU32({0.24f, 0.48f, 0.84f, 1.0f}), 16.0f, 0, 1.5f);

        const ImVec2 iconMin(origin.x + 18.0f, origin.y + 20.0f);
        const ImVec2 iconMax(origin.x + 90.0f, origin.y + 92.0f);
        drawList->AddRectFilled(iconMin, iconMax, ToU32({0.16f, 0.20f, 0.25f, 1.0f}), 14.0f);
        drawList->AddRect(iconMin, iconMax, ToU32({0.24f, 0.29f, 0.36f, 1.0f}), 14.0f, 0, 1.0f);
        DrawTexture(ImVec2(iconMin.x + 6.0f, iconMin.y + 6.0f), ImVec2(iconMax.x - 6.0f, iconMax.y - 6.0f), m_brandTextureView);

        ImGui::SetCursorScreenPos(ImVec2(origin.x + 100.0f, origin.y + 28.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4({0.94f, 0.96f, 0.99f, 1.0f}));
        ImGui::TextUnformatted(projectName.c_str());
        ImGui::PopStyleColor();

        ImGui::SetCursorScreenPos(ImVec2(origin.x + 100.0f, origin.y + 56.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4({0.57f, 0.63f, 0.72f, 1.0f}));
        ImGui::TextUnformatted(projectPath.c_str());
        ImGui::PopStyleColor();

        ImGui::SetCursorScreenPos(ImVec2(origin.x + 100.0f, origin.y + 84.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4({0.46f, 0.52f, 0.60f, 1.0f}));
        ImGui::TextUnformatted("Recent project");
        ImGui::PopStyleColor();

        ImGui::SetCursorScreenPos(ImVec2(origin.x + width - 194.0f, origin.y + 42.0f));
        ImGui::PushID(projectPath.c_str());
        if (DrawActionButton("Open", {84.0f, 40.0f}, {0.24f, 0.48f, 0.84f, 1.0f}, {0.30f, 0.57f, 0.94f, 1.0f}, {0.19f, 0.41f, 0.74f, 1.0f}, {0.96f, 0.97f, 0.99f, 1.0f}))
            OpenProject(projectPath);
        ImGui::SameLine(0.0f, 10.0f);
        if (DrawActionButton("Remove", {92.0f, 40.0f}, {0.23f, 0.13f, 0.15f, 1.0f}, {0.29f, 0.16f, 0.18f, 1.0f}, {0.19f, 0.11f, 0.12f, 1.0f}, {0.95f, 0.90f, 0.91f, 1.0f}))
            RemoveProject(projectPath);
        ImGui::PopID();

        ImGui::SetCursorScreenPos(origin);
        ImGui::InvisibleButton(("##ProjectCard" + projectPath).c_str(), ImVec2(width - 214.0f, height));
        if (ImGui::IsItemClicked())
            m_selectedProject = projectPath;
        if (!isSelected && ImGui::IsItemHovered())
            drawList->AddRect(origin, end, ToU32({0.30f, 0.36f, 0.46f, 0.8f}), 16.0f, 0, 1.0f);

        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(width, height));
    }

    void DrawEmptyState(const ImVec2 &origin, float width)
    {
        const float height = 180.0f;
        const ImVec2 end(origin.x + width, origin.y + height);
        auto *drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(origin, end, ToU32({0.11f, 0.13f, 0.16f, 1.0f}), 16.0f);
        drawList->AddRect(origin, end, ToU32({0.19f, 0.22f, 0.27f, 1.0f}), 16.0f, 0, 1.0f);

        ImGui::SetCursorScreenPos(ImVec2(origin.x + 24.0f, origin.y + 34.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4({0.92f, 0.95f, 0.98f, 1.0f}));
        ImGui::TextUnformatted("No recent projects yet");
        ImGui::PopStyleColor();

        ImGui::SetCursorScreenPos(ImVec2(origin.x + 24.0f, origin.y + 66.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4({0.55f, 0.61f, 0.69f, 1.0f}));
        ImGui::PushTextWrapPos(origin.x + width - 24.0f);
        ImGui::TextWrapped("Create a new workspace or open an existing .nullus project to get started.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();

        ImGui::SetCursorScreenPos(ImVec2(origin.x + 24.0f, origin.y + 118.0f));
        if (DrawActionButton("Create Workspace", {154.0f, 42.0f}, {0.24f, 0.48f, 0.84f, 1.0f}, {0.30f, 0.57f, 0.94f, 1.0f}, {0.19f, 0.41f, 0.74f, 1.0f}, {0.96f, 0.97f, 0.99f, 1.0f}))
        {
            Dialogs::SelectFolderDialog dialog("New Project Location");
            std::string result = dialog.Result();
            if (!result.empty())
            {
                CreateProject(result);
                RegisterProject(result);
            }
        }

        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(width, height));
    }

    void DrawLauncher()
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("LauncherRoot", ImGui::GetContentRegionAvail(), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();

        const float padding = 28.0f;
        const float fullWidth = ImGui::GetContentRegionAvail().x;
        const float contentTop = ImGui::GetCursorScreenPos().y;

        ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos());
        DrawTitleBar(fullWidth);

        const float contentWidth = fullWidth - padding * 2.0f;
        const float bodyTop = contentTop + 42.0f + padding;

        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + padding, bodyTop));
        DrawHeaderCard(contentWidth);

        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + padding, bodyTop + 164.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4({0.68f, 0.73f, 0.80f, 1.0f}));
        ImGui::TextUnformatted("RECENT PROJECTS");
        ImGui::PopStyleColor();

        const float leftWidth = contentWidth * 0.70f;
        const float rightWidth = contentWidth - leftWidth - 18.0f;
        const ImVec2 contentOrigin(ImGui::GetCursorScreenPos().x + padding, bodyTop + 194.0f);

        if (m_registeredProjects.empty())
        {
            ImGui::SetCursorScreenPos(contentOrigin);
            DrawEmptyState(contentOrigin, leftWidth);
        }
        else
        {
            float y = contentOrigin.y;
            for (const auto &project : m_registeredProjects)
            {
                ImGui::SetCursorScreenPos(ImVec2(contentOrigin.x, y));
                DrawProjectCard(project, ImVec2(contentOrigin.x, y), leftWidth);
                y += 142.0f;
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(contentOrigin.x + leftWidth + 18.0f, contentOrigin.y));
        DrawProjectDetailsCard(ImVec2(contentOrigin.x + leftWidth + 18.0f, contentOrigin.y), rightWidth);

        ImGui::EndChild();
    }

private:
    Windowing::Window &m_window;
    bool &m_readyToGo;
    std::string &m_path;
    std::string &m_projectName;
    std::shared_ptr<Render::RHI::RHITextureView> m_brandTextureView;
    std::vector<std::string> m_registeredProjects;
    std::string m_selectedProject;
    std::string m_cachedProjectDetailsPath;
    std::string m_cachedProjectRuntimeBackend;
    std::string m_cachedProjectStartScene;
    std::string m_cachedProjectUpdatedTime;
};

Launcher::Launcher(
    std::optional<Render::Settings::EGraphicsBackend> backendOverride,
    const Render::Settings::RenderDocSettings& renderDocSettings)
    : m_backendOverride(backendOverride)
    , m_renderDocSettings(renderDocSettings)
{
    SetupContext();
    m_mainPanel = std::make_unique<LauncherPanel>(*m_window, m_readyToGo, m_projectPath, m_projectName, m_brandTextureView);

    m_uiManager->SetCanvas(m_canvas);
    m_canvas.AddPanel(*m_mainPanel);
}

Launcher::~Launcher()
{
    m_brandTextureView.reset();
    NLS::Render::Resources::Loaders::TextureLoader::Destroy(m_brandTextureResource);
}

std::tuple<bool, std::string, std::string> Launcher::Run()
{
    while (!m_window->ShouldClose())
    {
        m_device->PollEvents();

        // Use explicit RHI frame management for proper swapchain synchronization
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

    return {m_readyToGo, m_projectPath, m_projectName};
}

void Launcher::SetupContext()
{
    Windowing::Settings::DeviceSettings deviceSettings;
    Windowing::Settings::WindowSettings windowSettings;
    windowSettings.title = "Nullus - Launcher";
    windowSettings.width = 1000;
    windowSettings.height = 580;
    windowSettings.maximized = false;
    windowSettings.resizable = false;
    windowSettings.decorated = false;
    // Use backend override if provided, otherwise resolve from platform default
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
    m_window->SetNativeTitleBarDragRegion(42, 100);

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
    NLS::Core::ServiceLocator::Provide<UI::UIManager>(*m_uiManager);
    m_uiManager->LoadFont("Ruda_Medium", "../Assets/Editor/Fonts/Ruda-Bold.ttf", 18);
    m_uiManager->LoadFont("Ruda_Title", "../Assets/Editor/Fonts/Ruda-Bold.ttf", 30);
    m_uiManager->UseFont("Ruda_Medium");
    m_window->FramebufferResizeEvent.AddListener([this](uint16_t width, uint16_t height)
    {
        if (m_driver != nullptr && width > 0u && height > 0u)
        {
            m_driver->ResizePlatformSwapchain(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
        }
    });
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
}

void Launcher::RegisterProject(const std::string &p_path)
{
    static_cast<LauncherPanel *>(m_mainPanel.get())->RegisterProject(p_path);
}
} // namespace NLS

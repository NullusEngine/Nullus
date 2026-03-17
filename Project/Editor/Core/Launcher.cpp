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

#include <UI/Internal/Converter.h>

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
} // namespace

class LauncherPanel : public UI::PanelWindow
{
public:
    LauncherPanel(bool &p_readyToGo, std::string &p_path, std::string &p_projectName)
        : PanelWindow("Nullus - Launcher", true), m_readyToGo(p_readyToGo), m_path(p_path), m_projectName(p_projectName)
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

        const int windowFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
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

        ImGui::SetCursorScreenPos(ImVec2(start.x + 28.0f, start.y + 26.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4({0.93f, 0.96f, 0.99f, 1.0f}));
        if (ImGui::GetIO().Fonts->Fonts.Size > 1)
            ImGui::PushFont(ImGui::GetIO().Fonts->Fonts.back());
        ImGui::TextUnformatted("Nullus Launcher");
        if (ImGui::GetIO().Fonts->Fonts.Size > 1)
            ImGui::PopFont();
        ImGui::PopStyleColor();

        ImGui::SetCursorScreenPos(ImVec2(start.x + 28.0f, start.y + 78.0f));
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

        const std::array<std::pair<const char *, std::string>, 4> details =
        {{
            {"Project", Utils::PathParser::GetElementName(m_selectedProject)},
            {"Scene", DetectStartScene(m_selectedProject)},
            {"Renderer", "OpenGL / RDG"},
            {"Updated", FormatTimestamp(std::filesystem::last_write_time(m_selectedProject))}
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

        const ImVec2 iconMin(origin.x + 22.0f, origin.y + 24.0f);
        const ImVec2 iconMax(origin.x + 82.0f, origin.y + 84.0f);
        drawList->AddRectFilled(iconMin, iconMax, ToU32({0.16f, 0.20f, 0.25f, 1.0f}), 14.0f);
        drawList->AddRect(iconMin, iconMax, ToU32({0.24f, 0.29f, 0.36f, 1.0f}), 14.0f, 0, 1.0f);

        const ImVec2 sheetBackMin(iconMin.x + 21.0f, iconMin.y + 18.0f);
        const ImVec2 sheetBackMax(iconMin.x + 43.0f, iconMin.y + 47.0f);
        const ImVec2 sheetFrontMin(iconMin.x + 15.0f, iconMin.y + 12.0f);
        const ImVec2 sheetFrontMax(iconMin.x + 37.0f, iconMin.y + 41.0f);
        drawList->AddRectFilled(sheetBackMin, sheetBackMax, IM_COL32(144, 162, 186, 120), 4.0f);
        drawList->AddRectFilled(sheetFrontMin, sheetFrontMax, IM_COL32(231, 237, 245, 255), 4.0f);
        drawList->AddLine(ImVec2(sheetFrontMin.x + 4.0f, sheetFrontMin.y + 9.0f), ImVec2(sheetFrontMax.x - 4.0f, sheetFrontMin.y + 9.0f), IM_COL32(135, 150, 173, 220), 1.0f);
        drawList->AddLine(ImVec2(sheetFrontMin.x + 4.0f, sheetFrontMin.y + 15.0f), ImVec2(sheetFrontMax.x - 8.0f, sheetFrontMin.y + 15.0f), IM_COL32(135, 150, 173, 220), 1.0f);
        drawList->AddLine(ImVec2(sheetFrontMin.x + 4.0f, sheetFrontMin.y + 21.0f), ImVec2(sheetFrontMax.x - 10.0f, sheetFrontMin.y + 21.0f), IM_COL32(135, 150, 173, 220), 1.0f);

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
        const float fullWidth = ImGui::GetContentRegionAvail().x - padding * 2.0f;
        const float contentTop = ImGui::GetCursorScreenPos().y + padding;

        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + padding, contentTop));
        DrawHeaderCard(fullWidth);

        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + padding, contentTop + 164.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, UI::Internal::Converter::ToImVec4({0.68f, 0.73f, 0.80f, 1.0f}));
        ImGui::TextUnformatted("RECENT PROJECTS");
        ImGui::PopStyleColor();

        const float leftWidth = fullWidth * 0.70f;
        const float rightWidth = fullWidth - leftWidth - 18.0f;
        const ImVec2 contentOrigin(ImGui::GetCursorScreenPos().x + padding, contentTop + 194.0f);

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
    bool &m_readyToGo;
    std::string &m_path;
    std::string &m_projectName;
    std::vector<std::string> m_registeredProjects;
    std::string m_selectedProject;
};

Launcher::Launcher()
{
    SetupContext();
    m_mainPanel = std::make_unique<LauncherPanel>(m_readyToGo, m_projectPath, m_projectName);

    m_uiManager->SetCanvas(m_canvas);
    m_canvas.AddPanel(*m_mainPanel);
}

std::tuple<bool, std::string, std::string> Launcher::Run()
{
    while (!m_window->ShouldClose())
    {
        m_device->PollEvents();
        m_uiManager->Render();
        m_window->SwapBuffers();

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
    windowSettings.decorated = true;

    m_device = std::make_unique<Context::Device>(deviceSettings);
    m_window = std::make_unique<Windowing::Window>(*m_device, windowSettings);
    m_window->MakeCurrentContext();

    auto monSize = m_device->GetMonitorSize();
    auto winSize = m_window->GetSize();
    m_window->SetPosition(monSize.x / 2 - winSize.x / 2, monSize.y / 2 - winSize.y / 2);

    m_driver = std::make_unique<Render::Context::Driver>(Render::Settings::DriverSettings{false});

    m_uiManager = std::make_unique<UI::UIManager>(m_window->GetGlfwWindow(), UI::EStyle::ALTERNATIVE_DARK);
    m_uiManager->LoadFont("Ruda_Medium", "../Assets/Editor/Fonts/Ruda-Bold.ttf", 18);
    m_uiManager->LoadFont("Ruda_Title", "../Assets/Editor/Fonts/Ruda-Bold.ttf", 30);
    m_uiManager->UseFont("Ruda_Medium");
    m_uiManager->EnableEditorLayoutSave(false);
    m_uiManager->EnableDocking(false);
}

void Launcher::RegisterProject(const std::string &p_path)
{
    static_cast<LauncherPanel *>(m_mainPanel.get())->RegisterProject(p_path);
}
} // namespace NLS

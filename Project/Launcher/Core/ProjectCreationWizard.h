#pragma once

#include "LauncherLocalization.h"
#include "LauncherSettings.h"

#include <Rendering/Settings/EGraphicsBackend.h>
#include <Rendering/Settings/GraphicsBackendUtils.h>

#include <filesystem>
#include <functional>
#include <regex>
#include <string>
#include <vector>

namespace NLS
{

struct ProjectTemplate;

struct ProjectCreationConfig
{
    std::string projectName;
    std::string projectLocation;
    std::string templateId; // Template directory name (empty = no template content)
    std::string editorExecutablePath;
    Render::Settings::EGraphicsBackend selectedBackend = Render::Settings::GetPlatformDefaultGraphicsBackend();
    int windowWidth = 1280;
    int windowHeight = 720;
    bool vsync = true;
    bool multiSampling = true;
    int sampleCount = 4;
};

struct ProjectCreationValidationResult
{
    bool ok = false;
    LauncherTextKey messageKey = LauncherTextKey::MissingText;
    bool requiresModalPrompt = false;
};

inline ProjectCreationValidationResult ValidateProjectCreationConfig(const ProjectCreationConfig& config)
{
    if (config.editorExecutablePath.empty())
        return { false, LauncherTextKey::WizardEditorVersionRequired, true };

    if (config.projectName.empty())
        return { false, LauncherTextKey::NameEmpty, false };
    if (config.projectName.size() > 128)
        return { false, LauncherTextKey::NameTooLong, false };
    if (!std::regex_match(config.projectName, std::regex("^[a-zA-Z0-9_-]+$")))
        return { false, LauncherTextKey::NameInvalid, false };
    if (config.projectLocation.empty())
        return { false, LauncherTextKey::LocationEmpty, false };
    if (!std::filesystem::exists(config.projectLocation))
        return { false, LauncherTextKey::LocationNotExist, false };

    const std::filesystem::path fullProjectPath = std::filesystem::path(config.projectLocation) / config.projectName;
    if (std::filesystem::exists(fullProjectPath))
        return { false, LauncherTextKey::DirExistsPrefix, false };

    if (config.windowWidth < 640 || config.windowWidth > 7680 ||
        config.windowHeight < 480 || config.windowHeight > 4320)
    {
        return { false, LauncherTextKey::ResolutionInvalid, false };
    }

    return { true, LauncherTextKey::MissingText, false };
}

class ProjectCreationWizard
{
public:
    using OnProjectCreatedCallback = std::function<void(const ProjectCreationConfig& config)>;

    ProjectCreationWizard(
        const std::vector<ProjectTemplate>& templates,
        std::vector<LauncherInstallView> editorVersions,
        OnProjectCreatedCallback onCreated);

    /**
     * Draw wizard content into the current ImGui window.
     * @param width Available width
     * @param height Available height
     */
    void DrawContent(float width, float height);

    bool WasCancelled() const { return m_cancelled; }
    bool WasCompleted() const { return m_completed; }

private:
    bool ValidateConfig();
    bool CreateProjectFiles();

    // Layout helpers
    void DrawWizardHeader(float x, float y, float width);
    void DrawWizardFooter(float x, float y, float width);
    void DrawCategorySidebar(float x, float y, float width, float height);
    void DrawTemplateGrid(float x, float y, float width, float height);
    void DrawTemplatePreview(float x, float y, float width, float height);

    ProjectCreationConfig m_config;
    OnProjectCreatedCallback m_onCreated;

    // Template list and selection
    const std::vector<ProjectTemplate>& m_templates;
    int m_selectedTemplateIndex = 0;
    std::vector<LauncherInstallView> m_editorVersions;
    int m_selectedEditorVersionIndex = -1;

    // State
    std::string m_validationError;
    bool m_cancelled = false;
    bool m_completed = false;

    // Input buffers for ImGui
    static constexpr size_t kInputBufferSize = 256;
    char m_nameBuffer[kInputBufferSize] = {};
    char m_locationBuffer[kInputBufferSize] = {};

    // Category filtering
    std::string m_selectedCategory;
    std::vector<std::string> m_categories;
    char m_templateSearchBuffer[128] = {};

    // Advanced settings visibility
    bool m_showAdvanced = false;

    // Available backends for the dropdown
    struct BackendOption
    {
        Render::Settings::EGraphicsBackend backend;
        const char* label;
    };
    static inline const BackendOption s_backends[] = {
        {Render::Settings::EGraphicsBackend::DX12, "DirectX 12"},
        {Render::Settings::EGraphicsBackend::VULKAN, "Vulkan"},
        {Render::Settings::EGraphicsBackend::OPENGL, "OpenGL"},
        {Render::Settings::EGraphicsBackend::DX11, "DirectX 11"},
    };
    static inline const int s_backendCount = 4;
    static inline const int s_sampleCounts[] = {1, 2, 4, 8};
    static inline const int s_sampleCountCount = 4;
};

} // namespace NLS

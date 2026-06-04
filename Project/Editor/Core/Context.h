#pragma once

#include <Windowing/Context/Device.h>
#include <Windowing/Inputs/InputManager.h>
#include <Windowing/Window.h>
#include "UI/UIManager.h"
#include "Context/Driver.h"
#include <memory>
#include <functional>
#include <mutex>
#include <optional>
#include "SceneSystem/SceneManager.h"
#include "Filesystem/IniFile.h"
#include "ResourceManagement/MeshManager.h"
#include "ResourceManagement/TextureManager.h"
#include "ResourceManagement/ShaderManager.h"
#include "ResourceManagement/MaterialManager.h"
#include "ResourceManagement/ResourceLifetimeRegistry.h"
#include "EditorResources.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Settings/EGraphicsBackend.h"
#include "Rendering/Settings/DriverSettings.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Settings/EditorSettings.h"
#include "Assets/ImportProgressTracker.h"
#include "Assets/PrefabEditorWorkflow.h"
namespace NLS
{
namespace Editor::Core
{
class NativeProgressDialog;

/**
 * The Context handle the engine features setup
 */
class Context
{
	public:
    /**
     * Constructor
     * @param p_projectPath
     * @param p_projectName
     * @param p_backendOverride optional backend override from command line, if not provided uses project settings
     * @param p_renderDocOverride optional one-shot RenderDoc override from command line
     */
    Context(const std::string& p_projectPath, const std::string& p_projectName,
            std::optional<Render::Settings::EGraphicsBackend> p_backendOverride = std::nullopt,
            std::optional<Render::Settings::RenderDocSettings> p_renderDocOverride = std::nullopt,
            std::optional<Render::Settings::EngineDiagnosticsSettings> p_diagnosticsOverride = std::nullopt);

    /**
     * Destructor
     */
    ~Context();

    void ShutdownThreadedRendering();

    /**
     * Reset project settings ini file
     */
    void ResetProjectSettings();

    /**
     * Verify that project settings are complete (No missing key).
     * Returns true if the integrity is verified
     */
    bool IsProjectSettingsIntegrityVerified();

    /**
     * Apply project settings to the ini file
     */
    void ApplyProjectSettings();
    void PresentStartupProgressFrame(const std::string& label, float normalizedProgress);
    void CompleteStartupProgress();
    bool IsNativeStartupProgressAvailable() const;
    void PresentTaskProgress(
        uint64_t taskKey,
        const std::string& label,
        float normalizedProgress,
        std::function<void()> cancelHandler = {});
    void CompleteTaskProgress(uint64_t taskKey, const std::string& label);
    void CompleteTaskProgress(const std::string& label = "Task complete");
    void ApplyEditorSettings()
    {
        m_diagnosticsSettings = Settings::EditorSettings::BuildDiagnosticsSettings();
        ApplyDiagnosticsOverride(m_diagnosticsSettings);
        Render::Settings::SetThreadDiagnosticsSettings(m_diagnosticsSettings);

        if (driver != nullptr)
        {
            Render::Context::DriverRendererAccess::SetDiagnosticsSettings(*driver, m_diagnosticsSettings);

            const auto renderDocSettings = Settings::EditorSettings::BuildRenderDocSettings();
            Render::Context::DriverUIAccess::SetRenderDocEnabled(*driver, renderDocSettings.enabled);
            Render::Context::DriverUIAccess::SetRenderDocAutoOpenEnabled(*driver, renderDocSettings.autoOpenReplayUI);
        }
    }

    const Render::Settings::EngineDiagnosticsSettings& GetDiagnosticsSettings() const;

	public:
    const std::string projectPath;
    const std::string projectName;
    const std::string projectFilePath;
    const std::string engineAssetsPath;
    const std::string projectAssetsPath;
    const std::string editorAssetsPath;

    std::unique_ptr<NLS::Context::Device> device;
    std::unique_ptr<NLS::Windowing::Window> window;
    std::unique_ptr<NLS::Windowing::Inputs::InputManager> inputManager;
    std::unique_ptr<NLS::Render::Context::Driver> driver;
    std::unique_ptr<NLS::UI::UIManager> uiManager;
    std::unique_ptr<Editor::Core::EditorResources> editorResources;
    NLS::Engine::SceneSystem::SceneManager sceneManager;
    NLS::Editor::Assets::PrefabInstanceRegistry prefabInstanceRegistry;
    NLS::Editor::Assets::ImportProgressTracker importProgressTracker;
    std::optional<NLS::Editor::Assets::PrefabStageState> activePrefabStage;

    NLS::Core::ResourceManagement::MeshManager meshManager;
    NLS::Core::ResourceManagement::TextureManager textureManager;
    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ResourceManagement::ResourceLifetimeRegistry resourceLifetimeRegistry;

    NLS::Windowing::Settings::WindowSettings windowSettings;

    NLS::Filesystem::IniFile projectSettings;

private:
    void ApplyDiagnosticsOverride(Render::Settings::EngineDiagnosticsSettings& settings) const
    {
        if (!m_diagnosticsOverride.has_value())
            return;

        if (!m_diagnosticsOverride->editorValidationSceneCamera.empty())
            settings.editorValidationSceneCamera = m_diagnosticsOverride->editorValidationSceneCamera;
        if (!m_diagnosticsOverride->editorValidationFocusView.empty())
            settings.editorValidationFocusView = m_diagnosticsOverride->editorValidationFocusView;
        if (!m_diagnosticsOverride->editorValidationExclusiveView.empty())
            settings.editorValidationExclusiveView = m_diagnosticsOverride->editorValidationExclusiveView;
        if (m_diagnosticsOverride->editorValidationOpenFrameInfo)
            settings.editorValidationOpenFrameInfo = true;
        if (!m_diagnosticsOverride->editorValidationSelectGameObject.empty())
            settings.editorValidationSelectGameObject = m_diagnosticsOverride->editorValidationSelectGameObject;
        if (!m_diagnosticsOverride->editorValidationCreateAsset.empty())
            settings.editorValidationCreateAsset = m_diagnosticsOverride->editorValidationCreateAsset;
        if (!m_diagnosticsOverride->editorValidationSceneReadbackOutput.empty())
            settings.editorValidationSceneReadbackOutput = m_diagnosticsOverride->editorValidationSceneReadbackOutput;
        if (!m_diagnosticsOverride->editorValidationSceneReadbackSummary.empty())
            settings.editorValidationSceneReadbackSummary = m_diagnosticsOverride->editorValidationSceneReadbackSummary;
        if (m_diagnosticsOverride->logRenderDrawPath)
            settings.logRenderDrawPath = true;
        if (m_diagnosticsOverride->dx12LogFrameFlow)
            settings.dx12LogFrameFlow = true;
        if (m_diagnosticsOverride->editorLogSceneCameraInput)
            settings.editorLogSceneCameraInput = true;
    }

    std::optional<Render::Settings::EGraphicsBackend> m_backendOverride;
    std::optional<Render::Settings::RenderDocSettings> m_renderDocOverride;
    std::optional<Render::Settings::EngineDiagnosticsSettings> m_diagnosticsOverride;
    Render::Settings::EngineDiagnosticsSettings m_diagnosticsSettings;
    std::unique_ptr<NativeProgressDialog> m_nativeProgressDialog;
    mutable std::mutex m_progressDialogMutex;
    float m_lastStartupProgress = 0.0f;
    uint64_t m_activeTaskProgressKey = 0u;
    float m_lastTaskProgress = 0.0f;
    bool m_taskProgressVisible = false;
};
} // namespace Editor::Core
} // namespace NLS

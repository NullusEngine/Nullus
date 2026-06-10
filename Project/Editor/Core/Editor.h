#pragma once
#include "Core/Context.h"
#include "Core/EditorActions.h"
#include "Core/AssetFileWatcher.h"
#include "Core/PanelsManager.h"
#include "Assets/EditorStartupAssetPreimport.h"
#include "Shortcuts/EditorShortcutService.h"
#include <UI/Modules/Canvas.h>
namespace NLS
{
namespace Editor::Core
{
/**
 * Handle the editor logic
 */
class Editor
{
public:
    /**
     * Constructor of the editor
     * @param p_context
     */
    Editor(Context& p_context);

    /**
     * Destructor of the editor
     */
    ~Editor();

    /**
     * Handle panels creation and canvas binding
     */
    void SetupUI();

    /**
     * Prepare the frame (Inputs update, screen clearing)
     */
    void PreUpdate();
    void BeginProfilerFrame();

    /**
     * Editor main loop.
     * Render the scene, update panels...
     * @param p_deltaTime
     */
    void Update(float p_deltaTime);

    /**
     * Handle editor global shortcuts
     */
    void HandleGlobalShortcuts();
    void RegisterShortcutContexts();
    void RegisterDefaultShortcuts();
    void ApplyStartupValidationDirectives();
    void RestoreStartupScene();
    void RefreshProjectAssetBrowser();
    void PrepareProjectAssetWatchersForStartup();
    void AdoptStartupAssetWatchers(
        AssetFileWatcher engineAssetsWatcher,
        AssetFileWatcher projectAssetsWatcher);
    bool RunStartupWatcherPreimport(
        const NLS::Editor::Assets::StartupAssetPreimportProgressSink& progressSink = {});
    bool CompleteStartupWatcherPreimportGate(
        const NLS::Editor::Assets::StartupAssetPreimportProgressSink& progressSink = {});
    void RememberLastOpenedScene(const std::string& p_scenePath);
    void RefreshProfilerRecordingState();
    bool IsProfilerRecordingEnabled();

    /**
     * Update the current editor mode
     * @param p_deltaTime
     */
    void UpdateCurrentEditorMode(float p_deltaTime);

    /**
     * Apply the play mode logic
     * @param p_deltaTime
     */
    void UpdatePlayMode(float p_deltaTime);

    /**
     * Apply the edit mode logic
     * @param p_deltaTime
     */
    void UpdateEditMode(float p_deltaTime);

    /**
     * Update editor panels
     * @param p_deltaTime
     */
    void UpdateEditorPanels(float p_deltaTime);

    /**
     * Update every view (Scene View, Game View, Asset View) before UI drawing.
     * Rendering happens during panel draw once the current ImGui layout is known.
     * @param p_deltaTime
     */
    void UpdateViews(float p_deltaTime);

    /**
     * Render the editor UI using ImGui
     * @param p_deltaTime
     */
    void RenderEditorUI(float p_deltaTime);

    float GetCurrentFrameRate() const;
    float GetCurrentDeltaTime() const;
    void OpenConsole();

    /**
     * Actually render the scene (Buffer swapping)
     * and clear input events
     */
    void PostUpdate();

private:
    uint64_t m_elapsedFrames = 0;
    uint64_t m_sceneSourcePathChangedListener = 0;
    uint32_t m_frameRateSampleCount = 0;
    float m_currentDeltaTime = 0.0f;
    float m_currentFrameRate = 0.0f;
    float m_frameRateAccumulatedTime = 0.0f;
    UI::Canvas			m_canvas;
    Context& m_context;
    PanelsManager	m_panelsManager;
    struct JobSystemLifetime
    {
        JobSystemLifetime();
        ~JobSystemLifetime();

        bool ownsJobSystem = false;
    };
    JobSystemLifetime m_jobSystemLifetime;
    EditorActions	m_editorActions;
    Shortcuts::EditorShortcutService m_shortcutService;
};
} // namespace Editor::Core
} // namespace NLS

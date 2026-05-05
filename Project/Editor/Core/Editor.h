#pragma once
#include "Core/Context.h"
#include "Core/EditorActions.h"
#include "Core/PanelsManager.h"
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

    /**
     * Actually render the scene (Buffer swapping)
     * and clear input events
     */
    void PostUpdate();

private:
    uint64_t m_elapsedFrames = 0;
    uint32_t m_frameRateSampleCount = 0;
    float m_currentDeltaTime = 0.0f;
    float m_currentFrameRate = 0.0f;
    float m_frameRateAccumulatedTime = 0.0f;
    UI::Canvas			m_canvas;
    Context& m_context;
    PanelsManager	m_panelsManager;
    EditorActions	m_editorActions;
    Shortcuts::EditorShortcutService m_shortcutService;
};
} // namespace Editor::Core
} // namespace NLS

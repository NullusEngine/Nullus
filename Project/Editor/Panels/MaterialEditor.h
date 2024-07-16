#pragma once

#include <UI/Widgets/Texts/Text.h>
#include <UI/Panels/PanelWindow.h>
#include <UI/Widgets/Layout/Group.h>
#include <UI/Widgets/Layout/Columns.h>
#include <Rendering/Resources/Shader.h>

namespace NLS::Render::Resources
{
class Material;
}

namespace NLS::Editor::Panels
{
class MaterialEditor : public NLS::UI::Panels::PanelWindow
{
public:
    /**
     * Constructor
     * @param p_title
     * @param p_opened
     * @param p_windowSettings
     */
    MaterialEditor(
        const std::string& p_title,
        bool p_opened,
        const NLS::UI::Settings::PanelWindowSettings& p_windowSettings);

    /**
     * Refresh the material editor
     */
    void Refresh();

    /**
     * Defines the target material of the material editor
     * @param p_newTarget
     */
    void SetTarget(Render::Resources::Material& p_newTarget);

    /**
     * Returns the target of the material editor
     */
    Render::Resources::Material* GetTarget() const;

    /**
     * Remove the target of the material editor (Clear the material editor)
     */
    void RemoveTarget();

    /**
     * Launch the preview of the currently targeted material
     */
    void Preview();

    /**
     * Reset material
     */
    void Reset();

private:
    void OnMaterialDropped();
    void OnShaderDropped();

    void CreateHeaderButtons();
    void CreateMaterialSelector();
    void CreateShaderSelector();
    void CreateMaterialSettings();
    void CreateShaderSettings();

    void GenerateShaderSettingsContent();
    void GenerateMaterialSettingsContent();

private:
    Render::Resources::Material* m_target = nullptr;
    Render::Resources::Shader* m_shader = nullptr;

    NLS::UI::Widgets::Texts::Text* m_targetMaterialText = nullptr;
    NLS::UI::Widgets::Texts::Text* m_shaderText = nullptr;

    Event<> m_materialDroppedEvent;
    Event<> m_shaderDroppedEvent;

    NLS::UI::Widgets::Layout::Group* m_settings = nullptr;
    NLS::UI::Widgets::Layout::Group* m_materialSettings = nullptr;
    NLS::UI::Widgets::Layout::Group* m_shaderSettings = nullptr;
    NLS::UI::Widgets::Layout::Columns* m_shaderSettingsColumns = nullptr;
    NLS::UI::Widgets::Layout::Columns* m_materialSettingsColumns = nullptr;
};
} // namespace NLS::Editor::Panels
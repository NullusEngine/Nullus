#pragma once

#include <Rendering/Resources/Loaders/TextureLoader.h>

#include <UI/Panels/PanelWindow.h>
#include <UI/Widgets/Layout/GroupCollapsable.h>
#include <UI/Widgets/InputFields/InputText.h>
#include <UI/Widgets/Visual/Separator.h>
#include <UI/Widgets/Selection/CheckBox.h>
#include <UI/Widgets/Buttons/Button.h>
#include <UI/Widgets/Selection/ComboBox.h>

#include "Panels/Hierarchy.h"
#include "Panels/AssetBrowser.h"
#include "UDRefl/Object.hpp"

namespace NLS::Editor::Panels
{
class Inspector : public NLS::UI::PanelWindow
{
public:
    /**
     * Constructor
     * @param p_title
     * @param p_opened
     * @param p_windowSettings
     */
    Inspector(
        const std::string& p_title,
        bool p_opened,
        const NLS::UI::PanelWindowSettings& p_windowSettings);

    /**
     * Destructor
     */
    ~Inspector();

    /**
     * Focus the given actor
     * @param p_target
     */
    void FocusActor(Engine::GameObject& p_target);

    /**
     * Unfocus the currently targeted actor
     */
    void UnFocus();

    /**
     * Unfocus the currently targeted actor without removing listeners attached to this actor
     */
    void SoftUnFocus();

    /**
     * Returns the currently selected actor
     */
    Engine::GameObject* GetTargetActor() const;

    /**
     * Create the actor inspector for the given actor
     */
    void CreateActorInspector(Engine::GameObject& p_target);

    /**
     * Draw the given component in inspector
     */
    void DrawComponent(UDRefl::SharedObject p_component);

    /**
     * Refresh the inspector
     */
    void Refresh();

private:
    Engine::GameObject* m_targetActor = nullptr;
    UI::Widgets::Group* m_actorInfo;
    UI::Widgets::Group* m_inspectorHeader;
    UI::Widgets::ComboBox* m_componentSelectorWidget;

    uint64_t m_componentAddedListener = 0;
    uint64_t m_componentRemovedListener = 0;
    uint64_t m_behaviourAddedListener = 0;
    uint64_t m_behaviourRemovedListener = 0;
    uint64_t m_destroyedListener = 0;
};
} // namespace NLS::Editor::Panels
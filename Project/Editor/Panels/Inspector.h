#pragma once

#include <UI/Panels/PanelWindow.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include "Panels/Hierarchy.h"
#include "Panels/AssetBrowser.h"

namespace NLS::Engine
{
class GameObject;
}

namespace NLS::Engine::Components
{
class Component;
class UICanvasComponent;
}

namespace NLS::UI::Widgets
{
class Group;
}

namespace NLS::Editor::Panels
{
class ComponentSearchPanel;

#if defined(NLS_ENABLE_TEST_HOOKS)
namespace TestHooks
{
std::map<int, std::string> BuildUICanvasDocumentChoicesForTesting(
    const std::filesystem::path& projectAssetsPath,
    const std::string& selectedDocumentPath = {});
int FindUICanvasDocumentChoiceForTesting(
    const std::map<int, std::string>& choices,
    const std::filesystem::path& projectAssetsPath,
    const std::string& documentPath);
bool AssignUICanvasDocumentReferenceForTesting(
    Engine::Components::UICanvasComponent& canvas,
    const std::filesystem::path& projectAssetsPath,
    const std::string& documentPath);
bool AssignUICanvasDocumentChoiceForTesting(
    Engine::Components::UICanvasComponent& canvas,
    const std::filesystem::path& projectAssetsPath,
    const std::map<int, std::string>& choices,
    int value);
}
#endif

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
    void FocusGameObject(Engine::GameObject& p_target);

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
    Engine::GameObject* GetTargetGameObject() const;

    /**
     * Create the actor inspector for the given actor
     */
    void CreateGameObjectInspector(Engine::GameObject& p_target);

    /**
     * Draw the given component in inspector
     */
    void DrawComponent(Engine::Components::Component* p_component);

    /**
     * Refresh the inspector
     */
    void Refresh();

private:
    void DrawPrefabState(Engine::GameObject& p_target);
    void SyncComponentPicker();
    void MarkTargetSceneDirty() const;

    Engine::GameObject* m_targetGameObject = nullptr;
    UI::Widgets::Group* m_gameObjectInfo;
    UI::Widgets::Group* m_inspectorHeader;
    ComponentSearchPanel* m_componentPicker = nullptr;
    std::shared_ptr<bool> m_lifetimeToken;

    uint64_t m_componentAddedListener = 0;
    uint64_t m_componentRemovedListener = 0;
    uint64_t m_behaviourAddedListener = 0;
    uint64_t m_behaviourRemovedListener = 0;
    uint64_t m_destroyedListener = 0;
};
} // namespace NLS::Editor::Panels

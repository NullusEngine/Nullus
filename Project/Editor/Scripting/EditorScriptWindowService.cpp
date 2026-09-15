#include "Scripting/EditorScriptWindowService.h"

#include <memory>
#include <utility>
#include <vector>

#include <Core/ServiceLocator.h>
#include <UI/Settings/PanelWindowSettings.h>

#include "Core/EditorActions.h"
#include "Core/PanelsManager.h"
#include "Panels/EditorTopBar.h"
#include "Panels/ScriptedEditorWindow.h"

namespace NLS::Editor::Scripting
{
namespace
{
constexpr const char* kPanelIdPrefix = "Script Window:";
constexpr const char* kWindowMenuOwner = "Editor Top Bar";

std::string TitleFor(std::string_view editorTypeName)
{
    // Prefer the menu path's leaf when the script registered a Window menu
    // entry, so the panel title matches what the user clicked.
    const auto& items = EditorScriptHost::Get().GetMenuItems();
    for (const auto& item : items)
    {
        if (item.editorTypeName != editorTypeName)
            continue;
        const auto separator = item.menuPath.find_last_of('/');
        const auto leaf = separator == std::string::npos ? item.menuPath : item.menuPath.substr(separator + 1u);
        if (!leaf.empty())
            return leaf;
    }

    const auto separator = editorTypeName.find_last_of('.');
    const auto simple = separator == std::string_view::npos ? editorTypeName : editorTypeName.substr(separator + 1u);
    return simple.empty() ? std::string(editorTypeName) : std::string(simple);
}

Core::PanelsManager* TryGetPanelsManager()
{
    if (!NLS::Core::ServiceLocator::Contains<Core::EditorActions>())
        return nullptr;
    return &NLS::Core::ServiceLocator::Get<Core::EditorActions>().GetPanelsManager();
}
} // namespace

EditorScriptWindowService& EditorScriptWindowService::Get()
{
    static EditorScriptWindowService service;
    return service;
}

bool EditorScriptWindowService::HasScriptWindow(std::string_view editorTypeName) const
{
    return m_windows.find(std::string(editorTypeName)) != m_windows.end();
}

Panels::ScriptedEditorWindow* EditorScriptWindowService::FindScriptWindow(std::string_view editorTypeName) const
{
    const auto iterator = m_windows.find(std::string(editorTypeName));
    return iterator != m_windows.end() ? iterator->second : nullptr;
}

void EditorScriptWindowService::OpenWindow(std::string_view editorTypeName)
{
    if (editorTypeName.empty())
        return;

    const std::string key(editorTypeName);
    if (const auto iterator = m_windows.find(key); iterator != m_windows.end())
    {
        iterator->second->SetOpened(true);
        iterator->second->Focus();
        return;
    }

    auto* manager = TryGetPanelsManager();
    if (manager == nullptr)
        return;

    const auto title = TitleFor(editorTypeName);
    UI::PanelWindowSettings settings;
    settings.closable = true;
    settings.collapsable = true;
    settings.dockable = true;

    auto window = std::make_unique<Panels::ScriptedEditorWindow>(title, key, settings);
    auto* raw = window.get();
    manager->AddPanel(std::string(kPanelIdPrefix) + key, std::move(window));
    m_windows.emplace(key, raw);

    // A scripted Window menu entry already opens this panel, so adding a second
    // toggleable entry would duplicate the menu path.  Only windows without a
    // scripted entry need the generic Window menu registration.
    const auto& menuItems = EditorScriptHost::Get().GetMenuItems();
    const auto hasScriptedEntry = std::any_of(
        menuItems.begin(),
        menuItems.end(),
        [&](const auto& item) { return item.editorTypeName == key; });

    if (!hasScriptedEntry && manager->HasPanel(kWindowMenuOwner))
        manager->GetPanelAs<Panels::EditorTopBar>(kWindowMenuOwner).RegisterWindowPanel(title, *raw);
}

void EditorScriptWindowService::CloseWindow(std::string_view editorTypeName)
{
    const auto iterator = m_windows.find(std::string(editorTypeName));
    if (iterator == m_windows.end())
        return;
    iterator->second->SetOpened(false);
}

void EditorScriptWindowService::ReleaseManagedInstances()
{
    for (auto& [typeName, window] : m_windows)
    {
        (void)typeName;
        window->ResetManagedInstance();
    }
}

void EditorScriptWindowService::CloseAllWindows()
{
    for (auto& [typeName, window] : m_windows)
    {
        (void)typeName;
        window->SetOpened(false);
    }
}
} // namespace NLS::Editor::Scripting

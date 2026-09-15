#pragma once

#include <UI/Panels/PanelWindow.h>

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "Settings/EditorSettingsRegistry.h"

namespace NLS::Editor::Inspector
{
class InspectorView;
}

namespace NLS::Editor::Undo
{
struct UndoHistoryEvent;
}

namespace NLS::Editor::Panels
{
class ProjectSettings : public UI::PanelWindow
{
public:
    ProjectSettings(
        const std::string& p_title,
        bool p_opened,
        const UI::PanelWindowSettings& p_windowSettings);
    ~ProjectSettings() override;

    void Open();
    void Close();
    void DrawModal();
    bool IsModalOpen() const;

    static std::string ChooseSelectionAfterSearch(
        const Settings::EditorSettingsRegistry& p_registry,
        const std::string& p_currentSelection,
        const std::string& p_searchText);

protected:
    void _Draw_Impl() override;

private:
    void EnsureInitialized();
    void DrawSettingsModal();
    void DrawSearchRow();
    void DrawCategoryList(const std::vector<const Settings::EditorSettingObject*>& p_visibleObjects);
    void DrawSelectedSettings();
    void ApplyLiveSettings();
    void HandleUndoHistoryEvent(const Undo::UndoHistoryEvent& p_event);
    void SynchronizeDirtySettingsFromUndo();
    [[nodiscard]] bool HasUnsavedSettings() const;
    void SaveIfDirty();
    std::filesystem::path GetSettingsPath() const;

private:
    Settings::EditorSettingsRegistry m_registry;
    bool m_initialized = false;
    bool m_opened = false;
    std::string m_searchText;
    std::string m_selectedSettingId;
    std::set<std::string> m_dirtySettings;
    std::set<std::string> m_restartRequiredFields;
    NLS::Editor::Inspector::InspectorView* m_inspectorView = nullptr;
    uint64_t m_propertyChangedListener = 0u;
    uint64_t m_undoHistoryListener = 0u;
};
}

#pragma once
#include <UI/Panels/PanelWindow.h>
#include <Input/InputAction.h>
#include <filesystem>

namespace NLS::Editor::Panels
{
class InputAssetEditor final : public UI::PanelWindow
{
public:
    InputAssetEditor(const std::string& title, bool opened, const UI::PanelWindowSettings& settings);
    bool OpenDocument(const std::filesystem::path& path);
    bool Save();
protected:
    void OnBeforeDrawWidgets() override;
private:
    void DrawDefinition();
    void DrawDebugger();
    Input::InputDefinition m_definition;
    std::filesystem::path m_path;
    std::vector<std::string> m_diagnostics;
    bool m_dirty = false;
};
}

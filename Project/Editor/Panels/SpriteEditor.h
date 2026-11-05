#pragma once

#include <UI/Panels/PanelWindow.h>
#include <filesystem>
#include <string_view>

namespace NLS::Editor::Panels
{
class SpriteEditor final : public UI::PanelWindow
{
public:
    SpriteEditor(const std::string& title,bool opened,const UI::PanelWindowSettings& settings,
        std::filesystem::path projectRoot);
    ~SpriteEditor() override;
    bool OpenDocument(const std::filesystem::path& path);
    bool OwnsShortcuts() const;
    bool ExecuteCommand(std::string_view command);
protected:
    void _Draw_Impl() override;
    void OnBeforeDrawWidgets() override;
private:
    struct State;
    std::unique_ptr<State> m;
};
}

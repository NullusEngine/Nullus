#pragma once
#include <UI/Panels/PanelWindow.h>
#include <VFX/Authoring/VfxLibrary.h>
#include <memory>

namespace NLS::Editor::Panels
{
class VfxLibraryEditor final : public NLS::UI::PanelWindow
{
public:
    VfxLibraryEditor(const std::string& title,bool opened,const NLS::UI::PanelWindowSettings& settings);
    ~VfxLibraryEditor() override;
    bool OpenDocument(const std::filesystem::path& path);
    bool OwnsShortcuts() const;
protected:
    void OnBeforeDrawWidgets() override;
private:
    struct State;
    std::unique_ptr<State> m_state;
    void Save();
    void Publish();
    void DuplicateToProject();
    void ApplyPending();
    void ScriptProperties();
    void DrawGraph();
    void EmitterProperties();
};
}

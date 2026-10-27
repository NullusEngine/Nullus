#pragma once
#include <UI/Panels/PanelWindow.h>
#include <VFX/Authoring/VfxAuthoring.h>
#include <filesystem>
#include <memory>
#include <vector>

namespace NLS::Editor::Panels
{
class VfxEditor final : public NLS::UI::PanelWindow
{
public:
    VfxEditor(const std::string& title, bool opened, const NLS::UI::PanelWindowSettings& settings, std::filesystem::path cacheRoot = {});
    ~VfxEditor() override;
    bool OpenDocument(const std::filesystem::path& path);
    bool OwnsShortcuts() const;
    size_t DocumentCount() const;
    // Editor commands are the single entry point for the toolbar, the Window
    // menu and the keyboard shortcuts, mirroring NiagaraEditorCommands. They
    // are public so tests and editor scripting can drive the same path.
    bool ExecuteCommand(const std::string& id);
    bool IsCommandEnabled(const std::string& id) const;
    bool IsCommandChecked(const std::string& id) const;
    // Number of emitters in the active document (tests and editor scripting).
    size_t ActiveEmitterCount() const;
    // Workspace panel visibility per active document ('Preview', 'Details', ...).
    bool SetWorkspacePanel(const std::string& label, bool visible);
    bool IsWorkspacePanelVisible(const std::string& label) const;
protected:
    void OnBeforeDrawWidgets() override;
private:
    struct Document;
    struct State;
    std::unique_ptr<State> m_state;
    bool ExecuteCommand(Document* document, const std::string& id);
    bool IsCommandEnabled(const Document* document, const std::string& id) const;
    bool IsCommandChecked(const Document* document, const std::string& id) const;
    Document* ActiveDocument();
    const Document* ActiveDocument() const;
    void NewDocument();
    void AddEmitter(Document& document);
    void CollapseStackToHeaders(Document& document);
    bool FocusPreview(Document& document,bool selectedOnly);
    bool OpenModuleSource(Document& document,const VFX::Authoring::Module& module);
    void Toolbar();
    void Workspace(Document& document);
    void Overview(Document& document);
    void Details(Document& document);
    void Parameters(Document& document, bool overrides);
    void Preview(Document& document);
    void Timeline(Document& document);
    void Curves(Document& document);
    void Script(Document& document);
    void Diagnostics(Document& document);
    void Save(Document& document);
    void Publish(Document& document);
    void Dialogs();
    void Shortcuts(Document& document);
};
}

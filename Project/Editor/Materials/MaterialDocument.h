#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include "Rendering/Assets/MaterialAsset.h"
#include "Undo/EditorUndoService.h"

namespace NLS::Editor::Materials
{
namespace Detail
{
struct DocumentState
{
    Render::Assets::MaterialAsset asset;
    std::filesystem::path path;
    std::string source;
    std::string savedSource;
    std::string savedFileHash;
    std::string appliedSource;
    std::string appliedSnapshot;
    Undo::UndoScopeId scope;
    uint64_t revision = 1;
};
struct EditState;
}

class MaterialDocument final
{
public:
    explicit MaterialDocument(Undo::EditorUndoService* undo = nullptr);
    ~MaterialDocument();
    MaterialDocument(const MaterialDocument&) = delete;
    MaterialDocument& operator=(const MaterialDocument&) = delete;
    void New(Render::Assets::MaterialAsset asset);
    bool Load(const std::filesystem::path& path);
    bool Save(const std::filesystem::path& path = {});
    bool Apply(Render::Assets::MaterialAsset candidate, std::string label);
    bool BeginEdit(std::string label);
    bool PreviewEdit(Render::Assets::MaterialAsset candidate);
    bool EndEdit(bool commit = true);
    bool DiscardWorking();
    bool SetWorkingFromExternal(Render::Assets::MaterialAsset asset);
    void SetUndoScope(Undo::UndoScopeId scope);
    void MarkApplied(std::string snapshotId, std::string source);
    const Render::Assets::MaterialAsset& Asset() const { return m_state->asset; }
    const std::filesystem::path& Path() const { return m_state->path; }
    const std::string& Error() const { return m_error; }
    const std::string& Source() const { return m_state->source; }
    const std::string& SavedSource() const { return m_state->savedSource; }
    const std::string& AppliedSource() const { return m_state->appliedSource; }
    const std::string& AppliedSnapshot() const { return m_state->appliedSnapshot; }
    bool IsDirty() const { return m_state->source != m_state->savedSource; }
    bool IsApplied() const { return m_state->source == m_state->appliedSource; }
    bool IsEditing() const { return m_edit != nullptr; }
    std::weak_ptr<const void> Lifetime() const { return m_state; }
    uint64_t Revision() const { return m_state->revision; }
    const Undo::UndoScopeId& Scope() const { return m_state->scope; }

private:
    void Replace(Render::Assets::MaterialAsset asset,std::filesystem::path path);
    Undo::EditorUndoService* m_undo = nullptr;
    std::shared_ptr<Detail::DocumentState> m_state;
    std::shared_ptr<Detail::EditState> m_edit;
    std::optional<Undo::UndoEditSession> m_session;
    std::string m_error;
};
}

#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include "Undo/EditorUndoService.h"

namespace NLS::Editor::Properties
{
namespace Detail
{
struct DocumentState
{
    std::string asset, id;
    std::filesystem::path path;
    std::string source, savedSource, savedFileHash;
    Undo::UndoScopeId scope;
    uint64_t revision = 1;
};
struct EditState;
} // namespace Detail
class SourcePropertyDocument final
{
public:
    explicit SourcePropertyDocument(Undo::EditorUndoService* undo = nullptr);
    ~SourcePropertyDocument();
    void New(std::string source);
    bool Load(const std::filesystem::path& path);
    bool Save(const std::filesystem::path& path = {});
    bool Apply(std::string source, std::string label);
    void MarkSaved();
    bool BeginEdit(std::string label);
    bool PreviewEdit(std::string source);
    bool EndEdit(bool commit = true);
    const std::string& Asset() const
    {
        return m_state->source;
    }
    const std::string& SavedSource() const
    {
        return m_state->savedSource;
    }
    const std::filesystem::path& Path() const
    {
        return m_state->path;
    }
    const std::string& Error() const
    {
        return m_error;
    }
    bool IsDirty() const
    {
        return m_state->source != m_state->savedSource;
    }
    bool IsEditing() const
    {
        return m_edit != nullptr;
    }
    std::weak_ptr<const void> Lifetime() const
    {
        return m_state;
    }
    uint64_t Revision() const
    {
        return m_state->revision;
    }

private:
    void Replace(std::string source, std::filesystem::path path);
    Undo::EditorUndoService* m_undo = nullptr;
    std::shared_ptr<Detail::DocumentState> m_state;
    std::shared_ptr<Detail::EditState> m_edit;
    std::optional<Undo::UndoEditSession> m_session;
    std::string m_error;
};
} // namespace NLS::Editor::Properties

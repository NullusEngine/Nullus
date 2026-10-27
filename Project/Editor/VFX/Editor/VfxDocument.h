#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <VFX/Authoring/VfxAuthoring.h>
#include "Undo/EditorUndoService.h"

namespace NLS::Editor::VFXTools
{
namespace NextDetail
{
struct DocumentState
{
    NLS::VFX::Authoring::System asset;
    std::filesystem::path path;
    std::string source, savedSource;
    Undo::UndoScopeId scope;
    uint64_t revision = 1;
};
struct EditState;
}
class VfxDocument final
{
public:
    explicit VfxDocument(Undo::EditorUndoService* undo = nullptr);
    ~VfxDocument();
    VfxDocument(const VfxDocument&) = delete;
    VfxDocument& operator=(const VfxDocument&) = delete;
    void New(std::string name = "Particle System");
    bool Load(const std::filesystem::path& path);
    bool Save(const std::filesystem::path& path = {});
    bool Apply(NLS::VFX::Authoring::System candidate, std::string label);
    bool BeginEdit(std::string label);
    bool PreviewEdit(NLS::VFX::Authoring::System candidate);
    bool EndEdit(bool commit = true);
    const NLS::VFX::Authoring::System& Asset() const { return m_state->asset; }
    const std::filesystem::path& Path() const { return m_state->path; }
    const std::string& Error() const { return m_error; }
    bool IsDirty() const { return m_state->source != m_state->savedSource; }
    bool IsEditing() const { return m_edit != nullptr; }
    uint64_t Revision() const { return m_state->revision; }
    const Undo::UndoScopeId& Scope() const { return m_state->scope; }
    std::weak_ptr<const void> Lifetime() const { return m_state; }
private:
    void Replace(NLS::VFX::Authoring::System asset, std::filesystem::path path);
    Undo::EditorUndoService* m_undo = nullptr;
    std::shared_ptr<NextDetail::DocumentState> m_state;
    std::shared_ptr<NextDetail::EditState> m_edit;
    std::optional<Undo::UndoEditSession> m_session;
    std::string m_error;
};
}

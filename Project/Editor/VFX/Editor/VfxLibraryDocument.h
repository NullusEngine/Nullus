#pragma once
#include <VFX/Authoring/VfxLibrary.h>
#include "Undo/EditorUndoService.h"

namespace NLS::Editor::VFXTools
{
struct LibraryAsset
{
    NLS::VFX::Authoring::AssetKind kind = NLS::VFX::Authoring::AssetKind::Module;
    NLS::VFX::Authoring::ScriptAsset script;
    NLS::VFX::Authoring::EmitterSource emitter;
    const std::string& Id() const { return kind == NLS::VFX::Authoring::AssetKind::Emitter ? emitter.id : script.id; }
    const std::string& Name() const { return kind == NLS::VFX::Authoring::AssetKind::Emitter ? emitter.name : script.name; }
};
LibraryAsset CreateLibraryAsset(NLS::VFX::Authoring::AssetKind kind,std::string name);
std::string SerializeLibraryAsset(const LibraryAsset& asset);
bool DeserializeLibraryAsset(std::string_view source,LibraryAsset& asset,std::string& error);
bool LoadLibraryAsset(const std::filesystem::path& path,LibraryAsset& asset,std::string& error);
bool SaveLibraryAsset(const std::filesystem::path& path,const LibraryAsset& asset,std::string& error);
namespace LibraryDetail
{
struct DocumentState
{
    LibraryAsset asset;
    std::filesystem::path path;
    std::string source,savedSource;
    Undo::UndoScopeId scope;
    uint64_t revision = 1;
    bool readOnly = false;
};
struct EditState;
}
class VfxLibraryDocument final
{
public:
    explicit VfxLibraryDocument(Undo::EditorUndoService* undo = nullptr);
    ~VfxLibraryDocument();
    VfxLibraryDocument(const VfxLibraryDocument&) = delete;
    VfxLibraryDocument& operator=(const VfxLibraryDocument&) = delete;
    void New(NLS::VFX::Authoring::AssetKind kind,std::string name = "VFX Script");
    bool Load(const std::filesystem::path& path,bool readOnly = false);
    bool Save(const std::filesystem::path& path = {});
    bool Apply(LibraryAsset asset,std::string label);
    bool BeginEdit(std::string label);
    bool PreviewEdit(LibraryAsset asset);
    bool EndEdit(bool commit = true);
    const LibraryAsset& Asset() const { return m_state->asset; }
    const std::filesystem::path& Path() const { return m_state->path; }
    bool IsReadOnly() const { return m_state->readOnly; }
    const std::string& Error() const { return m_error; }
    bool IsDirty() const { return m_state->source != m_state->savedSource; }
    bool IsEditing() const { return m_edit != nullptr; }
    uint64_t Revision() const { return m_state->revision; }
    std::weak_ptr<const void> Lifetime() const { return m_state; }
private:
    void Replace(LibraryAsset asset,std::filesystem::path path);
    Undo::EditorUndoService* m_undo = nullptr;
    std::shared_ptr<LibraryDetail::DocumentState> m_state;
    std::shared_ptr<LibraryDetail::EditState> m_edit;
    std::optional<Undo::UndoEditSession> m_session;
    std::string m_error;
};
}

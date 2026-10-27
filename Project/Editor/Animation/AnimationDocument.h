#pragma once
#include <AnimationCompiler/AnimationCompiler.h>
#include "Undo/EditorUndoService.h"
#include <variant>

namespace NLS::Editor::AnimationTools
{
using Asset=std::variant<Animation::GraphAsset,Animation::ClipAsset,Animation::BlendSpaceAsset,Animation::BoneMaskAsset>;
struct DocumentState
{
    Asset asset;
    std::filesystem::path path;
    std::string source,saved,applied,snapshot;
    Undo::UndoScopeId scope;
    uint64_t revision=1;
};
class AnimationDocument
{
public:
    explicit AnimationDocument(Undo::EditorUndoService* undo=nullptr);
    ~AnimationDocument();
    void New(Asset asset,std::filesystem::path path={});
    bool Load(const std::filesystem::path& path);
    bool Save(const std::filesystem::path& path={});
    bool Edit(Asset asset,std::string label);
    bool Restore(std::string source);
    void MarkApplied(std::string snapshot);
    const Asset& Value()const{return m_state->asset;}
    const DocumentState& State()const{return *m_state;}
    const std::string& Error()const{return m_error;}
private:
    Undo::EditorUndoService* m_undo;
    std::shared_ptr<DocumentState> m_state;
    std::string m_error;
};
std::string Serialize(const Asset& asset);
bool Deserialize(const std::filesystem::path& path,std::string_view source,Asset& asset,std::string& error);
}

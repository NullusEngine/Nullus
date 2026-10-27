#include "AnimationDocument.h"
#include <Assets/AssetMeta.h>
#include <atomic>
#include <fstream>

namespace NLS::Editor::AnimationTools
{
std::string Serialize(const Asset& asset){return std::visit([](const auto& value){return AnimationCompiler::Serialize(value);},asset);}
bool Deserialize(const std::filesystem::path& path,std::string_view text,Asset& asset,std::string& error)
{
    const auto extension=path.extension().string();
    if(extension==".animgraph"){Animation::GraphAsset v;if(!AnimationCompiler::Deserialize(text,v,error))return false;asset=std::move(v);}
    else if(extension==".animclip"){Animation::ClipAsset v;if(!AnimationCompiler::Deserialize(text,v,error))return false;asset=std::move(v);}
    else if(extension==".blendspace"){Animation::BlendSpaceAsset v;if(!AnimationCompiler::Deserialize(text,v,error))return false;asset=std::move(v);}
    else if(extension==".bonemask"){Animation::BoneMaskAsset v;if(!AnimationCompiler::Deserialize(text,v,error))return false;asset=std::move(v);}
    else {error="Unsupported animation document type";return false;}return true;
}
namespace
{
Undo::UndoStartResult RestoreState(DocumentState& state,const std::string& source)
{
    Asset asset;std::string error;if(!Deserialize(state.path,source,asset,error))return Undo::UndoStartResult::Failed(error);
    state.asset=std::move(asset);state.source=source;++state.revision;return Undo::UndoStartResult::Completed();
}
class Command final:public Undo::IEditorUndoCommand
{
public:
    std::weak_ptr<DocumentState> state;Undo::UndoCommandInfo info;std::string before,after;
    Undo::UndoCommandInfo GetInfo()const override{return info;}
    Undo::UndoValidationResult Validate(Undo::UndoDirection direction,const Undo::UndoExecutionContext&)const override
    {
        auto target=state.lock();if(!target||target->scope!=info.scopes[0])return Undo::UndoValidationResult::Stale("Animation document was closed or reloaded");
        if(target->source!=(direction==Undo::UndoDirection::Undo?after:before))return Undo::UndoValidationResult::Conflict("Animation document changed outside this command");return Undo::UndoValidationResult::Ready();
    }
    Undo::UndoStartResult Apply(Undo::UndoDirection direction,const Undo::UndoExecutionContext&,Undo::UndoCompletion)override
    {auto target=state.lock();return target?RestoreState(*target,direction==Undo::UndoDirection::Undo?before:after):Undo::UndoStartResult::Failed("Animation document is closed");}
    bool TryMergeFrom(const Undo::IEditorUndoCommand& other)override
    {const auto* command=dynamic_cast<const Command*>(&other);if(!command||command->info.scopes!=info.scopes||after!=command->before)return false;after=command->after;info.estimatedBytes=before.size()+after.size();return true;}
};
}
AnimationDocument::AnimationDocument(Undo::EditorUndoService* undo):m_undo(undo){New(AnimationCompiler::CreateGraph(),"Untitled.animgraph");}
AnimationDocument::~AnimationDocument()=default;
void AnimationDocument::New(Asset asset,std::filesystem::path path)
{
    static std::atomic<uint64_t> generation{1};m_state=std::make_shared<DocumentState>();m_state->asset=std::move(asset);m_state->path=std::move(path);
    m_state->source=Serialize(m_state->asset);m_state->scope={Undo::UndoScopeKind::Asset,"animation:"+std::visit([](const auto& v){return v.id;},m_state->asset),generation.fetch_add(1)};m_error.clear();
}
bool AnimationDocument::Load(const std::filesystem::path& path)
{
    std::ifstream file(path);if(!file){m_error="Cannot read animation source";return false;}const std::string source{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    Asset asset;if(!Deserialize(path,source,asset,m_error))return false;New(std::move(asset),path);m_state->saved=m_state->source;if(m_undo)m_undo->MarkSaved(m_state->scope,m_state->source);return true;
}
bool AnimationDocument::Save(const std::filesystem::path& path)
{
    const auto destination=path.empty()?m_state->path:path;if(destination.empty()){m_error="Select an animation destination";return false;}
    std::ofstream file(destination,std::ios::binary|std::ios::trunc);if(!file||!file.write(m_state->source.data(),m_state->source.size())){m_error="Cannot save animation source";return false;}file.close();
    if(!std::filesystem::exists(Core::Assets::GetAssetMetaPath(destination))&&!Core::Assets::AssetMeta::CreateForAsset(destination).Save(Core::Assets::GetAssetMetaPath(destination))){m_error="Cannot create animation metadata";return false;}
    m_state->path=destination;m_state->saved=m_state->source;if(m_undo)m_undo->MarkSaved(m_state->scope,m_state->source);return true;
}
bool AnimationDocument::Edit(Asset asset,std::string label)
{
    const auto source=Serialize(asset);if(source==m_state->source)return true;
    if(!m_undo){m_state->asset=std::move(asset);m_state->source=source;++m_state->revision;return true;}
    auto command=std::make_unique<Command>();command->state=m_state;command->before=m_state->source;command->after=source;
    command->info.label=std::move(label);command->info.sourceModule="Animation";command->info.scopes={m_state->scope};command->info.estimatedBytes=command->before.size()+source.size();
    command->info.allowMerge=true;command->info.mergeKey="animation:"+m_state->scope.stableKey;
    const auto result=m_undo->Execute(std::move(command));m_error=result.message;return result.state==Undo::UndoRequestState::Completed;
}
bool AnimationDocument::Restore(std::string source){const auto result=RestoreState(*m_state,source);m_error=result.message;return result.state==Undo::UndoStartState::Completed;}
void AnimationDocument::MarkApplied(std::string snapshot){m_state->snapshot=std::move(snapshot);m_state->applied=m_state->source;}
}

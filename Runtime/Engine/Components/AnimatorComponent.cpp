#include "AnimatorComponent.h"
#include "GameObject.h"
#include "Serialize/ObjectReferenceResolver.h"
#include <cmath>

namespace NLS::Engine::Components
{
namespace
{
struct AppliedProgram {uint64_t generation=0;std::shared_ptr<const Animation::AnimationProgram> program;};
std::map<std::string,AppliedProgram>& AppliedPrograms(){static std::map<std::string,AppliedProgram> programs;return programs;}
}
void AnimatorComponent::PublishAppliedProgram(std::string guid,std::shared_ptr<const Animation::AnimationProgram> program)
{auto& entry=AppliedPrograms()[guid];++entry.generation;entry.program=std::move(program);}
AnimatorComponent::AnimatorComponent():m_instance(std::make_unique<Animation::AnimationInstance>()){}
AnimatorComponent::~AnimatorComponent()=default;
Serialize::PPtr<Assets::AnimationAsset> AnimatorComponent::GetAnimationReference()const{return animation;}
void AnimatorComponent::SetAnimationReference(Serialize::PPtr<Assets::AnimationAsset> value){animation=value;m_asset.reset();m_failedHash.clear();m_appliedGeneration=0;Reload();}
bool AnimatorComponent::SetCompiledProgram(std::shared_ptr<const Animation::AnimationProgram> program,std::string& error)
{return m_instance->SetProgram(std::move(program),error);}
bool AnimatorComponent::SetModelClip(std::shared_ptr<const Animation::Model> model,size_t clip,std::string& error)
{
    if(!model){error="Animation model is missing";return false;}
    if(clip<model->clips.size())return m_instance->PlayClip(std::make_shared<Animation::Skeleton>(model->skeleton),std::make_shared<Animation::Clip>(model->clips[clip]),error);
    auto program=std::make_shared<Animation::AnimationProgram>();program->skeleton=std::make_shared<Animation::Skeleton>(model->skeleton);Animation::CompiledNode node;node.source.id=1;program->nodes.push_back(node);return m_instance->SetProgram(std::move(program),error);
}
bool AnimatorComponent::Reload()
{
    if(animation.GetInstanceID()==NLS::InstanceID_None)return false;
    Serialize::ObjectIdentifier id;if(!Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(animation.GetInstanceID(),id))return false;
    auto asset=Assets::LoadAnimationAsset(id,m_error);if(!asset)return false;
    if(asset==m_asset&&asset->ContentHash()==m_loadedHash&&m_instance->Program())return true;
    const bool loaded=asset->Program()?SetCompiledProgram(asset->Program(),m_error):SetModelClip(asset->Model(),0,m_error);
    if(loaded){m_loadedHash=asset->ContentHash();m_asset=std::move(asset);m_error.clear();}return loaded;
}
void AnimatorComponent::OnCreate(){Reload();}
void AnimatorComponent::OnStart(){Reload();if(playOnAwake)Play();else Pause();}
void AnimatorComponent::OnDestroy(){m_instance->Stop();if(m_instance->Program())m_instance->Tick(0,m_error);Finalize();m_asset.reset();}
void AnimatorComponent::Evaluate(float delta)
{
    if(animation.GetInstanceID()!=NLS::InstanceID_None)
    {
        Serialize::ObjectIdentifier id;if(Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(animation.GetInstanceID(),id))
        {
            const auto* entry=Serialize::ResolveRuntimeAssetReferenceEntry(id);
            // Existing instances retain their Applied program. Importing a Saved
            // source creates the version used by new instances; Apply or an
            // explicit Reload switches an already running instance.
            if(entry&&!m_instance->Program()&&m_failedHash!=entry->contentHash)
                if(!Reload())m_failedHash=entry->contentHash;
        }
    }
    Serialize::ObjectIdentifier bound;
    if(Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(animation.GetInstanceID(),bound))
    {
        auto found=AppliedPrograms().find(bound.guid.ToString());
        if(found!=AppliedPrograms().end()&&found->second.generation!=m_appliedGeneration)
        {if(SetCompiledProgram(found->second.program,m_error))m_appliedGeneration=found->second.generation;}
    }
    if(m_instance->Program())m_instance->Tick(delta,m_error);
}
void AnimatorComponent::Finalize(){auto frame=m_instance->Capture();if(!frame||frame->revision==m_dispatched)return;m_dispatched=frame->revision;for(const auto& event:frame->notifies)NotifyEvent.Invoke(event);}
void AnimatorComponent::Play(){m_instance->Play();}
void AnimatorComponent::Pause(bool paused){m_instance->Pause(paused);}
void AnimatorComponent::Stop(){m_instance->Stop();}
void AnimatorComponent::Seek(float seconds){m_instance->Seek(seconds);}
void AnimatorComponent::SetLooping(bool value){looping=value;m_instance->SetLooping(value);}
void AnimatorComponent::SetPlaybackSpeed(float value){if(std::isfinite(value)){playbackSpeed=value;m_instance->SetSpeed(value);}}
float AnimatorComponent::GetPlaybackSpeed()const{return playbackSpeed;}
bool AnimatorComponent::GetLooping()const{return looping;}
bool AnimatorComponent::SetFloat(const std::string& name,float value){return m_instance->SetFloat(name,value);}
bool AnimatorComponent::SetBool(const std::string& name,bool value){return m_instance->SetBool(name,value);}
bool AnimatorComponent::SetVector(const std::string& name,const Maths::Vector3& value){return m_instance->SetVector(name,{value.x,value.y,value.z,0});}
bool AnimatorComponent::SetTrigger(const std::string& name){return m_instance->SetTrigger(name);}
float AnimatorComponent::GetTime()const{return float(m_instance->Age());}
std::shared_ptr<const Animation::AnimationFrameResult> AnimatorComponent::CaptureFrame()const{return m_instance->Capture();}
}

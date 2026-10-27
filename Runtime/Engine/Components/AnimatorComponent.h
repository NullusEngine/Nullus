#pragma once
#include "Components/Component.h"
#include "Assets/AnimationAsset.h"
#include <Animation/Graph.h>
#include <Eventing/Event.h>
#include "Components/AnimatorComponent.generated.h"

namespace NLS::Engine::Components
{
CLASS(NLS_ENGINE_API AnimatorComponent,Scriptable,ComponentMenu("Animation/Animator"),DisallowMultipleComponent):public Component
{
public:
    GENERATED_BODY()
    AnimatorComponent();
    ~AnimatorComponent()override;
    void OnCreate()override;
    void OnStart()override;
    void OnDestroy()override;
    FUNCTION()
    Serialize::PPtr<Assets::AnimationAsset> GetAnimationReference()const;
    FUNCTION()
    void SetAnimationReference(Serialize::PPtr<Assets::AnimationAsset> reference);
    FUNCTION(Scriptable)
    void Play();
    FUNCTION(Scriptable)
    void Pause(bool paused=true);
    FUNCTION(Scriptable)
    void Stop();
    FUNCTION(Scriptable)
    void Seek(float seconds);
    FUNCTION(Scriptable)
    void SetLooping(bool loop);
    FUNCTION(Scriptable)
    void SetPlaybackSpeed(float speed);
    FUNCTION(Scriptable)
    float GetPlaybackSpeed()const;
    FUNCTION(Scriptable)
    bool GetLooping()const;
    FUNCTION(Scriptable)
    bool SetFloat(const std::string& name,float value);
    FUNCTION(Scriptable)
    bool SetBool(const std::string& name,bool value);
    FUNCTION(Scriptable)
    bool SetVector(const std::string& name,const Maths::Vector3& value);
    FUNCTION(Scriptable)
    bool SetTrigger(const std::string& name);
    FUNCTION(Scriptable)
    float GetTime()const;
    bool SetCompiledProgram(std::shared_ptr<const Animation::AnimationProgram> program,std::string& error);
    bool SetModelClip(std::shared_ptr<const Animation::Model> model,size_t clip,std::string& error);
    static void PublishAppliedProgram(std::string sourceGuid,std::shared_ptr<const Animation::AnimationProgram> program);
    bool Reload();
    void Evaluate(float delta);
    void Finalize();
    std::shared_ptr<const Animation::AnimationFrameResult> CaptureFrame()const;
    const std::string& LastError()const{return m_error;}
    NLS::Event<const Animation::NotifyEvent&> NotifyEvent;
private:
    PROPERTY(DisplayName("Animation"),Getter(GetAnimationReference),Setter(SetAnimationReference))
    Serialize::PPtr<Assets::AnimationAsset> animation;
    PROPERTY(DisplayName("Play On Awake"))
    bool playOnAwake=true;
    PROPERTY(DisplayName("Playback Speed"),Getter(GetPlaybackSpeed),Setter(SetPlaybackSpeed))
    float playbackSpeed=1;
    PROPERTY(DisplayName("Looping"),Getter(GetLooping),Setter(SetLooping))
    bool looping=true;
    std::unique_ptr<Animation::AnimationInstance> m_instance;
    std::shared_ptr<Assets::AnimationAsset> m_asset;
    std::string m_error,m_failedHash,m_loadedHash;
    uint64_t m_dispatched=0;
    uint64_t m_appliedGeneration=0;
};
}

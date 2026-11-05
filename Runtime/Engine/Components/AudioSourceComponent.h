#pragma once
#include "Components/Component.h"
#include "Assets/AudioClip.h"
#include "Components/AudioSourceComponent.generated.h"
#include <memory>
namespace NLS::Engine::Components
{
CLASS(NLS_ENGINE_API AudioSourceComponent, Scriptable, ComponentMenu("Audio/Audio Source"), DisallowMultipleComponent) : public Component
{
public:
    GENERATED_BODY()
    ~AudioSourceComponent() override;
    void OnStart() override;
    void OnEnable() override;
    void OnDisable() override;
    void OnDestroy() override;
    FUNCTION() Serialize::PPtr<Assets::AudioClip> GetClip() const { return clip; }
    FUNCTION() void SetClip(Serialize::PPtr<Assets::AudioClip> value);
    FUNCTION(Scriptable) bool GetPlayOnAwake() const {return playOnAwake;}
    FUNCTION(Scriptable) void SetPlayOnAwake(bool value) {playOnAwake=value;}
    FUNCTION(Scriptable) bool GetLoop() const {return loop;}
    FUNCTION(Scriptable) void SetLoop(bool value);
    FUNCTION(Scriptable) float GetVolume() const {return volume;}
    FUNCTION(Scriptable) void SetVolume(float value);
    FUNCTION(Scriptable) bool IsPlaying() const;
    FUNCTION(Scriptable) void Play();
    FUNCTION(Scriptable) void Pause();
    FUNCTION(Scriptable) void Stop();
    const std::string& LastError() const {return m_error;}
private:
    PROPERTY(Getter(GetClip), Setter(SetClip)) Serialize::PPtr<Assets::AudioClip> clip;
    PROPERTY(Getter(GetPlayOnAwake), Setter(SetPlayOnAwake)) bool playOnAwake=true;
    PROPERTY(Getter(GetLoop), Setter(SetLoop)) bool loop=false;
    PROPERTY(Getter(GetVolume), Setter(SetVolume), Range(0.0f, 1.0f)) float volume=1;
    std::shared_ptr<Assets::AudioClip> m_clip;
    std::weak_ptr<Audio::AudioDevice> m_device;
    uint64_t m_voice=0;
    bool m_resumeOnEnable=false;
    std::string m_error;
};
}

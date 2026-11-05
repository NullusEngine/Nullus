#include "AudioSourceComponent.h"
#include "GameObject.h"
#include "SceneSystem/Scene.h"
#include <Debug/Logger.h>
#include <algorithm>
#include <cmath>
namespace NLS::Engine::Components
{
AudioSourceComponent::~AudioSourceComponent() {Stop();}
void AudioSourceComponent::OnStart() {if(playOnAwake)Play();}
void AudioSourceComponent::OnEnable() {if(m_resumeOnEnable){if(auto device=m_device.lock())device->ResumeVoice(m_voice);m_resumeOnEnable=false;}}
void AudioSourceComponent::OnDisable() {m_resumeOnEnable=IsPlaying();Pause();}
void AudioSourceComponent::OnDestroy() {Stop();m_clip.reset();}
void AudioSourceComponent::SetClip(Serialize::PPtr<Assets::AudioClip> value) {Stop();clip=value;m_clip.reset();}
void AudioSourceComponent::SetLoop(bool value) {loop=value;if(auto device=m_device.lock())device->SetVoiceLooping(m_voice,loop);}
void AudioSourceComponent::SetVolume(float value)
{ if(!std::isfinite(value))return;volume=std::clamp(value,0.0f,1.0f);if(auto device=m_device.lock())device->SetVoiceVolume(m_voice,volume); }
bool AudioSourceComponent::IsPlaying() const {auto device=m_device.lock();return device&&device->IsVoicePlaying(m_voice);}
void AudioSourceComponent::Play()
{
    if(!gameobject()||!gameobject()->GetScene()||!gameobject()->GetScene()->IsPlaying())return;
    if(auto device=m_device.lock();device&&m_voice){device->ResumeVoice(m_voice);return;}
    auto* active=Audio::AudioDevice::GetActive();if(!active)return;
    Serialize::ObjectIdentifier identifier;
    if(!Serialize::PersistentManager::Instance().InstanceIDToObjectIdentifier(clip.GetInstanceID(),identifier))return;
    m_clip=Assets::LoadAudioClip(identifier,m_error);
    if(!m_clip){NLS_LOG_WARNING("Audio Source: "+m_error);return;}
    m_device=active->weak_from_this();
    m_voice=active->Play(active->GetActiveSessionId(),m_clip->GetPcm(),loop,volume);
}
void AudioSourceComponent::Pause() {if(auto device=m_device.lock())device->PauseVoice(m_voice);}
void AudioSourceComponent::Stop() {if(auto device=m_device.lock())device->StopVoice(m_voice);m_voice=0;m_resumeOnEnable=false;}
}

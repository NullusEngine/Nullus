#include "AudioDevice.h"
#include <miniaudio.h>
#include <Debug/Logger.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace NLS::Engine::Audio
{
namespace { AudioDevice* activeDevice=nullptr; }
std::shared_ptr<const AudioPcm> DecodeAudio(std::span<const uint8_t> bytes, std::string& error)
{
    if (bytes.empty() || bytes.size()>256u*1024u*1024u) { error="Audio data is empty or exceeds the encoded size limit."; return {}; }
    auto config=ma_decoder_config_init(ma_format_f32,0,0);
    ma_decoder decoder;
    if (ma_decoder_init_memory(bytes.data(),bytes.size(),&config,&decoder)!=MA_SUCCESS)
    { error="Unsupported or invalid WAV, MP3 or OGG audio."; return {}; }
    struct Close { ma_decoder* decoder; ~Close(){ma_decoder_uninit(decoder);} } close{&decoder};
    ma_uint64 frames=0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder,&frames)!=MA_SUCCESS || !frames || decoder.outputChannels==0 || decoder.outputChannels>2 ||
        frames>128u*1024u*1024u/decoder.outputChannels)
    { error="Audio must contain mono/stereo samples and fit the decoded size limit."; return {}; }
    auto pcm=std::make_shared<AudioPcm>();
    pcm->channels=decoder.outputChannels; pcm->sampleRate=decoder.outputSampleRate;
    pcm->samples.resize(static_cast<size_t>(frames)*pcm->channels);
    ma_uint64 read=0;
    const auto result=ma_decoder_read_pcm_frames(&decoder,pcm->samples.data(),frames,&read);
    if ((result!=MA_SUCCESS && result!=MA_AT_END) || !read) { error="Audio decode failed."; return {}; }
    pcm->samples.resize(static_cast<size_t>(read)*pcm->channels);
    error.clear(); return pcm;
}

struct AudioDevice::State
{
    struct Bus { bool paused=false,muted=false; };
    struct Voice { uint64_t session=0; std::shared_ptr<const AudioPcm> pcm; double position=0; float volume=1; bool playing=true,looping=false; };
    mutable std::mutex mutex;
    std::unordered_map<uint64_t,Bus> buses;
    std::unordered_map<uint64_t,Voice> voices;
    uint64_t nextVoice=1;
    uint64_t activeSession=0;
    ma_device device {};
    bool deviceReady=false;
    void Mix(float* output,size_t frames)
    {
        if (output) std::fill_n(output,frames*2,0.0f);
        std::lock_guard lock(mutex);
        for (auto& [id,voice] : voices)
        {
            const auto bus=buses.find(voice.session);
            if (!voice.playing || bus==buses.end() || bus->second.paused || !voice.pcm || !voice.pcm->FrameCount()) continue;
            const auto& pcm=*voice.pcm;
            const auto count=pcm.FrameCount();
            const double advance=double(pcm.sampleRate)/48000.0;
            for (size_t frame=0;frame<frames;++frame)
            {
                if (voice.position>=count)
                { if (voice.looping) voice.position=std::fmod(voice.position,double(count)); else {voice.playing=false;break;} }
                const auto index=static_cast<size_t>(voice.position);
                if (output && !bus->second.muted)
                {
                    output[frame*2]+=pcm.samples[index*pcm.channels]*voice.volume;
                    output[frame*2+1]+=pcm.samples[index*pcm.channels+(pcm.channels==2 ? 1 : 0)]*voice.volume;
                }
                voice.position+=advance;
            }
        }
        if (output) for (size_t i=0;i<frames*2;++i) output[i]=std::clamp(output[i],-1.0f,1.0f);
    }
};
AudioDevice::AudioDevice(bool openDevice) : m_state(std::make_unique<State>())
{
    if (!openDevice) return;
    auto config=ma_device_config_init(ma_device_type_playback);
    config.playback.format=ma_format_f32; config.playback.channels=2; config.sampleRate=48000;
    config.pUserData=m_state.get();
    config.dataCallback=[](ma_device* device,void* output,const void*,ma_uint32 frames)
    { static_cast<State*>(device->pUserData)->Mix(static_cast<float*>(output),frames); };
    if (ma_device_init(nullptr,&config,&m_state->device)==MA_SUCCESS)
    {
        if (ma_device_start(&m_state->device)==MA_SUCCESS) m_state->deviceReady=true;
        else ma_device_uninit(&m_state->device);
    }
    if (!m_state->deviceReady) NLS_LOG_WARNING("Audio output device unavailable; game audio will run silently.");
}
AudioDevice::~AudioDevice()
{
    if (m_state->deviceReady) ma_device_uninit(&m_state->device);
    if (activeDevice==this) activeDevice=nullptr;
}
std::shared_ptr<AudioDevice> AudioDevice::AcquireHost()
{
    static std::mutex mutex; static std::weak_ptr<AudioDevice> host;
    std::lock_guard lock(mutex);
    auto device=host.lock();
    if (!device) { device=std::shared_ptr<AudioDevice>(new AudioDevice(true)); host=device; }
    return device;
}
std::shared_ptr<AudioDevice> AudioDevice::CreateOffline() { return std::shared_ptr<AudioDevice>(new AudioDevice(false)); }
AudioDevice* AudioDevice::GetActive() { return activeDevice; }
uint64_t AudioDevice::GetActiveSessionId() const { std::lock_guard lock(m_state->mutex); return m_state->activeSession; }
void AudioDevice::BeginSession(uint64_t id)
{ std::lock_guard lock(m_state->mutex); m_state->buses[id]={}; m_state->activeSession=id; activeDevice=this; }
void AudioDevice::EndSession(uint64_t id)
{
    std::lock_guard lock(m_state->mutex);
    std::erase_if(m_state->voices,[id](const auto& pair){return pair.second.session==id;});
    m_state->buses.erase(id);
    if (m_state->activeSession==id) {m_state->activeSession=0;if(activeDevice==this)activeDevice=nullptr;}
}
void AudioDevice::SetPaused(uint64_t id,bool paused) { std::lock_guard lock(m_state->mutex); if(auto it=m_state->buses.find(id);it!=m_state->buses.end())it->second.paused=paused; }
void AudioDevice::SetMuted(uint64_t id,bool muted) { std::lock_guard lock(m_state->mutex); if(auto it=m_state->buses.find(id);it!=m_state->buses.end())it->second.muted=muted; }
uint64_t AudioDevice::Play(uint64_t id,std::shared_ptr<const AudioPcm> pcm,bool loop,float volume)
{
    if (!pcm || !pcm->FrameCount() || !pcm->sampleRate || pcm->channels>2) return 0;
    std::lock_guard lock(m_state->mutex);
    if (!m_state->buses.contains(id)) return 0;
    const auto voice=m_state->nextVoice++;
    m_state->voices.emplace(voice,State::Voice{id,std::move(pcm),0,std::isfinite(volume)?std::clamp(volume,0.0f,1.0f):0,true,loop});
    return voice;
}
void AudioDevice::ResumeVoice(uint64_t id) { std::lock_guard lock(m_state->mutex); if(auto it=m_state->voices.find(id);it!=m_state->voices.end())
    { if(it->second.position>=it->second.pcm->FrameCount())it->second.position=0;it->second.playing=true; } }
void AudioDevice::PauseVoice(uint64_t id) { std::lock_guard lock(m_state->mutex); if(auto it=m_state->voices.find(id);it!=m_state->voices.end())it->second.playing=false; }
void AudioDevice::StopVoice(uint64_t id) { std::lock_guard lock(m_state->mutex); m_state->voices.erase(id); }
void AudioDevice::SetVoiceVolume(uint64_t id,float volume) { std::lock_guard lock(m_state->mutex); if(auto it=m_state->voices.find(id);it!=m_state->voices.end())it->second.volume=std::isfinite(volume)?std::clamp(volume,0.0f,1.0f):0; }
void AudioDevice::SetVoiceLooping(uint64_t id,bool looping) { std::lock_guard lock(m_state->mutex); if(auto it=m_state->voices.find(id);it!=m_state->voices.end())it->second.looping=looping; }
bool AudioDevice::IsVoicePlaying(uint64_t id) const
{ std::lock_guard lock(m_state->mutex); const auto it=m_state->voices.find(id);return it!=m_state->voices.end()&&it->second.playing; }
double AudioDevice::GetVoicePosition(uint64_t id) const
{ std::lock_guard lock(m_state->mutex);const auto it=m_state->voices.find(id);return it==m_state->voices.end()?0:it->second.position/it->second.pcm->sampleRate; }
size_t AudioDevice::GetVoiceCount(uint64_t id) const
{ std::lock_guard lock(m_state->mutex);return std::count_if(m_state->voices.begin(),m_state->voices.end(),[id](const auto& pair){return pair.second.session==id&&pair.second.playing;}); }
bool AudioDevice::IsDeviceAvailable() const {return m_state->deviceReady;}
void AudioDevice::Render(std::span<float> samples) {m_state->Mix(samples.data(),samples.size()/2);}
void AudioDevice::TickSilent(float delta)
{ if (!m_state->deviceReady&&std::isfinite(delta)&&delta>0)m_state->Mix(nullptr,static_cast<size_t>(std::min(delta,10.0f)*48000)); }
}

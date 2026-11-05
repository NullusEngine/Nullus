#pragma once
#include "EngineDef.h"
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace NLS::Engine::Audio
{
struct AudioPcm
{
    std::vector<float> samples;
    uint32_t channels = 0, sampleRate = 0;
    uint64_t FrameCount() const { return channels ? samples.size()/channels : 0; }
};
NLS_ENGINE_API std::shared_ptr<const AudioPcm> DecodeAudio(std::span<const uint8_t> bytes, std::string& error);
class NLS_ENGINE_API AudioDevice final : public std::enable_shared_from_this<AudioDevice>
{
public:
    static std::shared_ptr<AudioDevice> AcquireHost();
    static std::shared_ptr<AudioDevice> CreateOffline();
    static AudioDevice* GetActive();
    uint64_t GetActiveSessionId() const;
    ~AudioDevice();
    void BeginSession(uint64_t sessionId);
    void EndSession(uint64_t sessionId);
    void SetPaused(uint64_t sessionId, bool paused);
    void SetMuted(uint64_t sessionId, bool muted);
    uint64_t Play(uint64_t sessionId, std::shared_ptr<const AudioPcm> pcm, bool looping, float volume);
    void ResumeVoice(uint64_t voice);
    void PauseVoice(uint64_t voice);
    void StopVoice(uint64_t voice);
    void SetVoiceVolume(uint64_t voice,float volume);
    void SetVoiceLooping(uint64_t voice,bool looping);
    bool IsVoicePlaying(uint64_t voice) const;
    double GetVoicePosition(uint64_t voice) const;
    size_t GetVoiceCount(uint64_t sessionId) const;
    bool IsDeviceAvailable() const;
    void TickSilent(float unscaledDeltaTime);
    // The offline path executes exactly the same mixer as the device callback.
    void Render(std::span<float> stereoFrames);
private:
    explicit AudioDevice(bool openDevice);
    struct State;
    std::unique_ptr<State> m_state;
};
}

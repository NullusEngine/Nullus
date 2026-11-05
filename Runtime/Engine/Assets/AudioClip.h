#pragma once
#include "EngineDef.h"
#include "Reflection/Object.h"
#include "Reflection/Macros.h"
#include "Serialize/PPtr.h"
#include "Audio/AudioDevice.h"
#include "Assets/AudioClip.generated.h"
#include <filesystem>
#include <memory>

namespace NLS::Engine::Assets
{
CLASS(NLS_ENGINE_API AudioClip) : public NLS::Object
{
public:
    GENERATED_BODY()
    const std::shared_ptr<const Audio::AudioPcm>& GetPcm() const { return m_pcm; }
    const std::string& ContentHash() const { return m_hash; }
    bool Reload(const std::filesystem::path& path,std::string hash,std::string& error);
private:
    std::shared_ptr<const Audio::AudioPcm> m_pcm;
    std::string m_hash;
};
NLS_ENGINE_API std::shared_ptr<AudioClip> LoadAudioClip(const Serialize::ObjectIdentifier& identifier,std::string& error);
}

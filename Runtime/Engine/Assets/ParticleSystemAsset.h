#pragma once

#include <memory>
#include <string>
#include <VFX/CompiledSystem.h>
#include "EngineDef.h"
#include "Reflection/Object.h"
#include "Reflection/Macros.h"
#include "Serialize/PPtr.h"
#include "Assets/ParticleSystemAsset.generated.h"

namespace NLS::Engine::Assets
{
CLASS(NLS_ENGINE_API ParticleSystemAsset) : public NLS::Object
{
public:
    GENERATED_BODY()
    ParticleSystemAsset() = default;
    [[nodiscard]] const std::shared_ptr<const VFX::CompiledSystem>& Program() const { return m_program; }
    [[nodiscard]] const std::string& ContentHash() const { return m_hash; }
    bool Reload(const std::filesystem::path& path, std::string hash, std::string& error);
private:
    std::shared_ptr<const VFX::CompiledSystem> m_program;
    std::string m_hash;
};

// The manifest resolves stable asset identity; the path hint never causes source compilation.
NLS_ENGINE_API std::shared_ptr<ParticleSystemAsset> LoadParticleSystemAsset(
    const Serialize::ObjectIdentifier& identifier, std::string& error);
}

#pragma once

#include <Assets/SpriteArtifact.h>
#include "EngineDef.h"
#include "Reflection/Object.h"
#include "Reflection/Macros.h"
#include "Serialize/PPtr.h"
#include "Assets/SpriteAsset.generated.h"

namespace NLS::Engine::Assets
{
CLASS(NLS_ENGINE_API SpriteAsset) : public NLS::Object
{
public:
    GENERATED_BODY()
    SpriteAsset() = default;
    bool Reload(const std::filesystem::path& path, std::string hash, std::string& error);
    const std::shared_ptr<const Core::Assets::SpriteArtifact>& Capture() const { return m_data; }
    const std::string& ContentHash() const { return m_hash; }
private:
    std::shared_ptr<const Core::Assets::SpriteArtifact> m_data;
    std::string m_hash;
};
NLS_ENGINE_API std::shared_ptr<SpriteAsset> LoadSpriteAsset(
    const Serialize::ObjectIdentifier& identifier, std::string& error);
}

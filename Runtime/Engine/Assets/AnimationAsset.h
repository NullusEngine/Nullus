#pragma once
#include <Animation/Graph.h>
#include <Assets/ArtifactManifest.h>
#include "EngineDef.h"
#include "Reflection/Object.h"
#include "Reflection/Macros.h"
#include "Serialize/PPtr.h"
#include "Assets/AnimationAsset.generated.h"

namespace NLS::Engine::Assets
{
CLASS(NLS_ENGINE_API AnimationAsset):public NLS::Object
{
public:
    GENERATED_BODY()
    AnimationAsset()=default;
    bool Reload(const std::filesystem::path& path,Core::Assets::ArtifactType type,std::string hash,std::string& error);
    const std::shared_ptr<const Animation::Model>& Model()const{return m_model;}
    const std::shared_ptr<const Animation::AnimationProgram>& Program()const{return m_program;}
    const std::string& ContentHash()const{return m_hash;}
private:
    std::shared_ptr<const Animation::Model> m_model;
    std::shared_ptr<const Animation::AnimationProgram> m_program;
    std::string m_hash;
};
NLS_ENGINE_API std::shared_ptr<AnimationAsset> LoadAnimationAsset(const Serialize::ObjectIdentifier& identifier,std::string& error);
}

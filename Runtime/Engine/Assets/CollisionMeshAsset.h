#pragma once

#include "EngineDef.h"
#include <Reflection/Object.h>
#include <Reflection/Macros.h>
#include <Serialize/PPtr.h>
#include <Physics/CollisionMesh.h>
#include <filesystem>
#include "Assets/CollisionMeshAsset.generated.h"
#include <memory>

namespace NLS::Engine::Assets
{
CLASS(NLS_ENGINE_API CollisionMeshAsset) : public NLS::Object
{
public:
    GENERATED_BODY()
    std::shared_ptr<const Physics::CookedCollisionMesh> Capture() const { return m_mesh; }
    const std::string& ContentHash() const { return m_hash; }
    bool Reload(const std::filesystem::path& path,std::string hash,std::string& error,
        const Serialize::ObjectIdentifier* expected=nullptr);
private:
    std::shared_ptr<const Physics::CookedCollisionMesh> m_mesh;
    std::string m_hash;
};
NLS_ENGINE_API std::shared_ptr<CollisionMeshAsset> LoadCollisionMeshAsset(const Serialize::ObjectIdentifier& identifier,std::string& error);
}

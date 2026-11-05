#pragma once

#include "EngineDef.h"
#include <Reflection/Object.h>
#include <Reflection/Macros.h>
#include <Serialize/PPtr.h>
#include <Physics/PhysicsCommandBuffer.h>
#include <filesystem>
#include "Assets/PhysicsShapeAsset.generated.h"
#include <memory>

namespace NLS::Engine::Assets
{
struct PhysicsShapePart
{
    uint32_t id=1;
    Physics::CollisionGeometry3D geometry;
    Maths::Vector3 position,scale{1,1,1};
    Maths::Quaternion rotation=Maths::Quaternion::Identity;
    Serialize::ObjectIdentifier collisionMesh,material;
};
struct PhysicsShapeData
{
    std::vector<PhysicsShapePart> parts;
};
CLASS(NLS_ENGINE_API PhysicsShapeAsset) : public NLS::Object
{
public:
    GENERATED_BODY()
    std::shared_ptr<const PhysicsShapeData> Capture() const { return m_data; }
    const std::string& ContentHash() const { return m_hash; }
    bool Reload(const std::filesystem::path& path,std::string hash,std::string& error,
        const Serialize::ObjectIdentifier* expected=nullptr);
private:
    std::shared_ptr<const PhysicsShapeData> m_data;
    std::string m_hash;
};
NLS_ENGINE_API std::shared_ptr<PhysicsShapeAsset> LoadPhysicsShapeAsset(const Serialize::ObjectIdentifier& identifier,std::string& error);
}

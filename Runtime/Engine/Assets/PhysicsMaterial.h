#pragma once

#include "EngineDef.h"
#include <Reflection/Object.h>
#include <Reflection/Macros.h>
#include <Serialize/PPtr.h>
#include <Physics/PhysicsWorld.h>
#include <filesystem>
#include "Assets/PhysicsMaterial.generated.h"
#include <memory>

namespace NLS::Engine::Assets
{
struct PhysicsMaterialData
{
    float staticFriction=0.5f, dynamicFriction=0.5f, restitution=0.5f;
    float friction2D=0.5f, restitution2D=0;
    Physics::MaterialCombineMode frictionCombine=Physics::MaterialCombineMode::Average;
    Physics::MaterialCombineMode restitutionCombine=Physics::MaterialCombineMode::Average;
};

CLASS(NLS_ENGINE_API PhysicsMaterial) : public NLS::Object
{
public:
    GENERATED_BODY()
    std::shared_ptr<const PhysicsMaterialData> Capture() const { return m_data; }
    const std::string& ContentHash() const { return m_hash; }
    bool Reload(const std::filesystem::path& path,std::string hash,std::string& error,
        const Serialize::ObjectIdentifier* expected=nullptr);
private:
    std::shared_ptr<const PhysicsMaterialData> m_data;
    std::string m_hash;
};
NLS_ENGINE_API std::shared_ptr<PhysicsMaterial> LoadPhysicsMaterial(const Serialize::ObjectIdentifier& identifier,std::string& error);
}

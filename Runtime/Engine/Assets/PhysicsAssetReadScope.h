#pragma once
#include "EngineDef.h"
namespace NLS::Engine::Assets
{
// One lazy ArtifactDB snapshot per scene synchronization, shared by nested reads.
// Leaving the outermost scope makes subsequent imports visible on the next sync.
class NLS_ENGINE_API PhysicsAssetReadScope final
{
public:
    PhysicsAssetReadScope();
    ~PhysicsAssetReadScope();
    PhysicsAssetReadScope(const PhysicsAssetReadScope&)=delete;
    PhysicsAssetReadScope& operator=(const PhysicsAssetReadScope&)=delete;
};
}

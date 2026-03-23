#pragma once

#include "EngineDef.h"

#include "Resource/Actor/Actor.h"
#include "ResourceManagement/AResourceManager.h"

namespace NLS::Engine
{
class NLS_ENGINE_API ActorManager : public NLS::Core::ResourceManagement::AResourceManager<Actor>
{
public:
	virtual Actor* CreateResource(const std::string& path) override;
	virtual void DestroyResource(Actor* resource) override;
	virtual void ReloadResource(Actor* p_resource, const std::string& p_path) override;
};
} // namespace NLS::Engine

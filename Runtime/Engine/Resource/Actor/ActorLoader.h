#pragma once

#include "EngineDef.h"
#include "Resource/Actor/Actor.h"

namespace NLS::Engine
{
class NLS_ENGINE_API ActorLoader
{
public:
    static Actor* LoadActor(const std::string& path, const std::string& absPath);
};
} // namespace NLS::Engine

#pragma once
#include "RuntimePlaySession.h"
#include <Core/Filesystem/IniFile.h>
#include <exception>

namespace NLS::Scripting
{
// Project fields override the existing session clock. Missing fields retain
// explicit configuration, or the runtime's 1/60s default for a new session.
inline bool ConfigureProjectPhysicsTiming(RuntimePlaySession& session,Filesystem::IniFile& project,std::string& error)
{
    if(!project.IsKeyExisting("fixed_delta_time")&&!project.IsKeyExisting("maximum_fixed_steps")&&
        !project.IsKeyExisting("maximum_physics_accumulated_time")){error.clear();return true;}
    try
    {
        auto settings=session.GetPhysicsClock().GetSettings();
        settings.fixedDeltaTime=project.GetOrDefault<double>("fixed_delta_time",settings.fixedDeltaTime);
        const auto maximum=project.GetOrDefault<int>("maximum_fixed_steps",static_cast<int>(settings.maxSubsteps));
        if(maximum<1||maximum>1024){error="maximum_fixed_steps must be between 1 and 1024";return false;}
        settings.maxSubsteps=static_cast<uint32_t>(maximum);
        settings.maximumAccumulatedTime=project.GetOrDefault<double>("maximum_physics_accumulated_time",settings.maximumAccumulatedTime);
        return session.ConfigurePhysicsTiming(settings,error);
    }
    catch(const std::exception& exception){error=std::string("Invalid project physics timing: ")+exception.what();return false;}
}
}

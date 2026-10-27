#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "VfxProgram.h"
#include "VfxDataInterface.h"
#include "VfxShaders.h"

namespace NLS::VFX
{
struct SimulationStageProgram
{
    Program program;
    std::string hlsl;
};

struct EmitterPrograms
{
    Program spawn;
    Program update;
    // CPU-owned emitter-stage scripts, executed by both simulation targets.
    Program emitterSpawn;
    Program emitterUpdate;
    std::vector<Program> eventHandlers;
    std::vector<Program> eventParticles;
    Program emitterCollision;
    std::string spawnHlsl;
    std::string updateHlsl;
    std::string compactionHlsl;
    std::vector<SimulationStageProgram> stages;
};

struct CompiledSystem
{
    SystemAsset asset;
    Program systemSpawn;
    Program systemUpdate;
    std::vector<EmitterPrograms> emitters;
    std::vector<PlatformShaderPrograms> platforms;
    [[nodiscard]] NLS_VFX_API const PlatformShaderPrograms* FindPlatform(Render::RHI::NativeBackendType backend) const;
};

template<class Visitor> void VisitCompiledPrograms(const CompiledSystem& system,Visitor&& visit)
{
    visit(system.systemSpawn); visit(system.systemUpdate);
    for(const auto& emitter:system.emitters)
    {
        visit(emitter.spawn); visit(emitter.update); visit(emitter.emitterSpawn); visit(emitter.emitterUpdate); visit(emitter.emitterCollision);
        for(const auto& program:emitter.eventHandlers)visit(program);
        for(const auto& program:emitter.eventParticles)visit(program);
        for(const auto& stage:emitter.stages)visit(stage.program);
    }
}
inline bool CollectCompiledDataInterfaces(const CompiledSystem& system,std::vector<DataInterfaceDefinition>& bindings,std::string& error)
{
    Graph merged; bool valid=true;
    VisitCompiledPrograms(system,[&](const Program& program)
    {
        if(!valid)return;
        Typed::TypeRegistry types;
        if (!Typed::ValidateBytecodeProgram(program, types, error))
        {
            valid = false;
            return;
        }
        Graph input;input.dataInterfaces=program.dataInterfaces;
        valid=MergeGraphDeclarations(merged,input,error);
    });
    if(valid)bindings=std::move(merged.dataInterfaces);
    return valid;
}

NLS_VFX_API std::string SerializeCompiledSystem(const CompiledSystem& system, bool includeShaderSources = true);
NLS_VFX_API bool DeserializeCompiledSystem(std::string_view source, CompiledSystem& system, std::string& error);
NLS_VFX_API bool ValidatePlatformPrograms(const CompiledSystem& system, Render::RHI::NativeBackendType backend, std::string& error);
NLS_VFX_API bool LoadCompiledSystemArtifact(const std::filesystem::path& path, CompiledSystem& system, std::string& error);
}

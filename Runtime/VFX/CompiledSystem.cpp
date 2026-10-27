#include "VfxGraphSchemaSerialization.h"
#include "CompiledSystem.h"
#include "VfxValueSerialization.h"
#include "VfxTypeRules.h"

#include <algorithm>
#include <cmath>
#include <Json/json.hpp>
#include <Core/Assets/NativeArtifactContainer.h>
#include <Core/Assets/ArtifactManifest.h>
#include <array>
#include <limits>
#include <type_traits>

namespace NLS::VFX
{
namespace
{
using Json = nlohmann::json;
namespace rhi = Render::RHI;
constexpr size_t kMaximumShaderBytes = 16u*1024u*1024u;

template<class T> auto Kernels(T& shaders)
{
    using Descriptor = std::conditional_t<std::is_const_v<T>,const rhi::RHIShaderModuleDesc,rhi::RHIShaderModuleDesc>;
    std::vector<Descriptor*> result{&shaders.simulation.beginSpawn,&shaders.simulation.spawn,
        &shaders.simulation.update,&shaders.simulation.advanceSpawn,&shaders.simulation.compact,
        &shaders.simulation.finish,&shaders.simulation.reset,&shaders.spriteVertex,&shaders.spriteFragment,
        &shaders.sort.initialize,&shaders.sort.step,&shaders.simulation.bounds.reduce,&shaders.simulation.bounds.finish,
        &shaders.visibility.cull,&shaders.visibility.scan,&shaders.visibility.scatter,
        &shaders.simulation.resetEvents,&shaders.simulation.receiveEvents};
    for (auto& stage : shaders.simulation.stages) result.push_back(&stage);
    return result;
}
constexpr std::array<const char*,18> kKernelNames{"BeginSpawn","SpawnParticles","UpdateParticles","AdvanceSpawn","Compact","Finish","Reset","SpriteVS","SpritePS","SortInitialize","SortStep",
    "BoundsReduce","BoundsFinish","CullSprites","ScanVisibility","ScatterVisibility","ResetEvents","ReceiveEvents"};
rhi::ShaderStage KernelStage(size_t index) { return index == 7 ? rhi::ShaderStage::Vertex : index == 8 ? rhi::ShaderStage::Fragment : rhi::ShaderStage::Compute; }
// The vertex kernel entry point follows the emitter's renderer. The package
// validator uses the same mapping, so a mismatched renderer is rejected.
const char* KernelName(size_t index, const RendererType renderer)
{
    if (index == 7)
    {
        if (renderer == RendererType::Mesh) return "MeshVS";
        if (renderer == RendererType::Ribbon) return "RibbonVS";
    }
    return index < kKernelNames.size() ? kKernelNames[index] : "Main";
}

std::string EncodeBytes(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() > kMaximumShaderBytes) throw std::runtime_error("VFX shader exceeds 16 MiB");
    constexpr char digits[] = "0123456789abcdef";
    std::string text(bytes.size()*2,'0');
    for (size_t index = 0; index < bytes.size(); ++index) { text[index*2] = digits[bytes[index]>>4]; text[index*2+1] = digits[bytes[index]&15]; }
    return text;
}

std::vector<uint8_t> DecodeBytes(const std::string& text)
{
    if (text.size()%2 || text.size()/2 > kMaximumShaderBytes) throw std::runtime_error("Invalid VFX shader bytecode size");
    const auto digit = [](char value) -> uint8_t
    {
        if (value >= '0' && value <= '9') return static_cast<uint8_t>(value-'0');
        if (value >= 'a' && value <= 'f') return static_cast<uint8_t>(value-'a'+10);
        throw std::runtime_error("Invalid VFX shader bytecode encoding");
    };
    std::vector<uint8_t> result(text.size()/2);
    for (size_t index = 0; index < result.size(); ++index) result[index] = static_cast<uint8_t>(digit(text[index*2])*16+digit(text[index*2+1]));
    return result;
}

Json EncodeShaders(const EmitterShaderPrograms& shaders)
{
    Json result = Json::array();
    for (const auto* kernel : Kernels(shaders))
        result.push_back({{"entry",kernel->entryPoint},{"stage",static_cast<uint32_t>(kernel->stage)},{"code",EncodeBytes(kernel->bytecode)}});
    return result;
}

EmitterShaderPrograms DecodeShaders(const Json& source, rhi::NativeBackendType backend, bool gpu, size_t stages, RendererType renderer)
{
    if (stages > 64 || (!gpu && stages) || !source.is_array() || source.size() != kKernelNames.size()+stages) throw std::runtime_error("VFX shader kernel count mismatch");
    EmitterShaderPrograms result;
    result.simulation.stages.resize(stages);
    const auto kernels = Kernels(result);
    for (size_t index = 0; index < kernels.size(); ++index)
    {
        auto& kernel = *kernels[index];
        kernel.bytecode = DecodeBytes(source[index].at("code").get<std::string>());
        if ((index < 7 || index == 16 || index == 17) && !gpu && kernel.bytecode.empty()) continue;
        if (source[index].at("entry") != KernelName(index,renderer) || source[index].at("stage").get<uint32_t>() != static_cast<uint32_t>(KernelStage(index)) || kernel.bytecode.empty())
            throw std::runtime_error("VFX shader kernel stage, entry point or bytecode mismatch");
        kernel.entryPoint = KernelName(index,renderer); kernel.stage = KernelStage(index); kernel.targetBackend = backend;
    }
    return result;
}
Json Encode(const Program& program)
{
    Json result{{"bytecodeVersion", 1}, {"stage", static_cast<uint32_t>(program.stage)}, {"registerCount", program.registerCount}, {"parameters", Json::array()}, {"attributes", Json::array()}, {"instructions", Json::array()}};
    result["parameterSize"] = program.parameters.size;
    result["parameterAlignment"] = program.parameters.alignment;
    for (const auto& parameter : program.parameters.entries)
        result["parameters"].push_back({{"name", parameter.name}, {"type", parameter.type}, {"offset", parameter.offset}, {"default", Detail::EncodeValue(parameter.defaultValue.scalar, Typed::ToValueType(parameter.type))}});
    for (const auto& attribute : program.attributes)
        result["attributes"].push_back({{"name", attribute.name}, {"type", attribute.type}, {"read", attribute.read}, {"write", attribute.write}});
    if(!program.dataInterfaces.empty())result["dataInterfaces"]=Schema::EncodeDataInterfaces(program.dataInterfaces);
    for (const auto& instruction : program.instructions)
        result["instructions"].push_back({{"operation", static_cast<uint32_t>(instruction.operation)}, {"type", instruction.type}, {"inputs", instruction.inputs}, {"inputTypes", instruction.inputTypes}, {"fieldIndices", instruction.fieldIndices}, {"destination", instruction.destination}, {"attribute", instruction.attribute}, {"parameter", instruction.parameter}, {"randomStream", instruction.randomStream}, {"name", instruction.name}, {"literal", Detail::EncodeValue(instruction.literal.scalar, Typed::ToValueType(instruction.type))}});
    return result;
}

Program Decode(const Json& source, ScriptStage expected, const std::vector<Attribute>& attributes)
{
    if (source.at("bytecodeVersion").get<uint32_t>() != 1 || source.at("stage").get<uint32_t>() != static_cast<uint32_t>(expected) || !source.at("instructions").is_array() || source.at("instructions").size() > 65536 || !source.at("parameters").is_array() || source.at("parameters").size() > 16384 || !source.at("attributes").is_array() || source.at("attributes").size() > 1024)
        throw std::runtime_error("Invalid typed bytecode format, stage or size");
    Program program;
    program.stage = expected;
    program.registerCount = source.at("registerCount").get<uint32_t>();
    program.parameters.size = source.at("parameterSize").get<uint32_t>();
    program.parameters.alignment = source.at("parameterAlignment").get<uint32_t>();
    const auto type = [](const Json& input)
    {
        const auto id = input.get<uint32_t>();
        if (id < Typed::kBool || id > Typed::kFloat4)
            throw std::runtime_error("Unknown runtime bytecode value type");
        return id;
    };
    for (const auto& entry : source.at("parameters"))
    {
        Typed::ParameterMapEntry parameter;
        parameter.name = entry.at("name").get<std::string>();
        parameter.type = type(entry.at("type"));
        parameter.offset = entry.at("offset").get<uint32_t>();
        parameter.defaultValue.type = parameter.type;
        parameter.defaultValue.scalar = Detail::DecodeValue(entry.at("default"), Typed::ToValueType(parameter.type));
        program.parameters.entries.push_back(std::move(parameter));
    }
    for (const auto& entry : source.at("attributes"))
        program.attributes.push_back({entry.at("name").get<std::string>(), type(entry.at("type")), entry.at("read").get<bool>(), entry.at("write").get<bool>()});
    program.dataInterfaces=Schema::DecodeDataInterfaces(source.value("dataInterfaces",Json::array()));
    for (const auto& entry : source.at("instructions"))
    {
        Instruction instruction;
        const auto operation = entry.at("operation").get<uint32_t>();
        if (operation > static_cast<uint32_t>(Operation::DataInterfaceCall) || operation == static_cast<uint32_t>(Operation::StructMake) || operation == static_cast<uint32_t>(Operation::StructGet))
            throw std::runtime_error("Unlowered or unknown runtime bytecode operation");
        instruction.operation = static_cast<Operation>(operation);
        instruction.type = type(entry.at("type"));
        if (!entry.at("inputs").is_array() || entry.at("inputs").size() > 64 || !entry.at("inputTypes").is_array() || entry.at("inputTypes").size() > 64)
            throw std::runtime_error("Invalid typed bytecode inputs");
        instruction.inputs = entry.at("inputs").get<std::vector<uint32_t>>();
        for (const auto& input : entry.at("inputTypes"))
            instruction.inputTypes.push_back(type(input));
        instruction.inputCount = static_cast<uint32_t>(instruction.inputs.size());
        instruction.destination = entry.at("destination").get<uint32_t>();
        instruction.attribute = entry.at("attribute").get<uint32_t>();
        instruction.parameter = entry.at("parameter").get<uint32_t>();
        instruction.randomStream = entry.at("randomStream").get<uint32_t>();
        instruction.name = entry.at("name").get<std::string>();
        instruction.fieldIndices = entry.at("fieldIndices").get<std::vector<uint32_t>>();
        if (!instruction.fieldIndices.empty())
            throw std::runtime_error("Unexpected aggregate runtime field metadata");
        instruction.literal.type = instruction.type;
        instruction.literal.scalar = Detail::DecodeValue(entry.at("literal"), Typed::ToValueType(instruction.type));
        program.instructions.push_back(std::move(instruction));
    }
    Typed::TypeRegistry types;
    std::string error;
    std::vector<int32_t> bindings;
    if (!Typed::ValidateBytecodeProgram(program, types, error))
        throw std::runtime_error(error);
    ParticleDataSet schema;
    schema.Initialize(attributes, 1);
    if (!Typed::BindBytecodeAttributes(program, schema, bindings, error))
        throw std::runtime_error(error);
    return program;
}
}

const PlatformShaderPrograms* CompiledSystem::FindPlatform(Render::RHI::NativeBackendType backend) const
{
    const auto found = std::find_if(platforms.begin(),platforms.end(),[&](const auto& platform) { return platform.backend == backend; });
    return found == platforms.end() ? nullptr : &*found;
}

bool ValidatePlatformPrograms(const CompiledSystem& system, Render::RHI::NativeBackendType backend, std::string& error)
{
    const auto* platform = system.FindPlatform(backend);
    if (!platform || platform->emitters.size() != system.asset.emitters.size() || system.emitters.size() != system.asset.emitters.size())
    { error = "Particle system has no complete shader package for the selected backend"; return false; }
    for (size_t emitter = 0; emitter < platform->emitters.size(); ++emitter)
    {
        if (platform->emitters[emitter].simulation.stages.size() != system.asset.emitters[emitter].simulationStages.size() ||
            system.emitters[emitter].stages.size() != system.asset.emitters[emitter].simulationStages.size())
        { error = "Particle Simulation Stage package count mismatch"; return false; }
        const auto kernels = Kernels(platform->emitters[emitter]);
        for (size_t index = system.asset.emitters[emitter].target == SimulationTarget::GPU ? 0 : 7; index < kernels.size(); ++index)
            if (system.asset.emitters[emitter].target == SimulationTarget::CPU && (index == 16 || index == 17)) continue;
            else if (kernels[index]->targetBackend != backend || kernels[index]->stage != KernelStage(index) ||
                kernels[index]->entryPoint != KernelName(index,system.asset.emitters[emitter].renderer) || kernels[index]->bytecode.empty() || kernels[index]->bytecode.size() > kMaximumShaderBytes)
            { error = "Particle shader package contains an invalid kernel"; return false; }
    }
    error.clear(); return true;
}

std::string SerializeCompiledSystem(const CompiledSystem& system, bool includeShaderSources)
{
    auto settings = system.asset;
    for (auto& emitter : settings.emitters)
    {
        emitter.spawn.nodes.clear(); emitter.update.nodes.clear();
        // Emitter-stage graphs are compile-time inputs; the compiled artifact
        // only carries the resulting schedule.
        emitter.emitterSpawn.nodes.clear(); emitter.emitterUpdate.nodes.clear();
        for (auto& stage : emitter.simulationStages) stage.graph.nodes.clear();
    }
    settings.systemSpawn.nodes.clear(); settings.systemUpdate.nodes.clear();
    Json result = {{"format", "NullusVFXProgram"}, {"version", 7}, {"shaderLayoutVersion", kVfxShaderLayoutVersion}, {"settings", Json::parse(SerializeSystem(settings, false))}, {"emitters", Json::array()}, {"platforms", Json::array()}};
    if (!system.systemSpawn.instructions.empty()) result["systemSpawn"] = Encode(system.systemSpawn);
    if (!system.systemUpdate.instructions.empty()) result["systemUpdate"] = Encode(system.systemUpdate);
    for (const auto& emitter : system.emitters)
    {
        Json entry = {{"spawn",Encode(emitter.spawn)},{"update",Encode(emitter.update)},
            {"spawnHlsl",includeShaderSources ? emitter.spawnHlsl : std::string{}},
            {"updateHlsl",includeShaderSources ? emitter.updateHlsl : std::string{}},
            {"compactionHlsl",includeShaderSources ? emitter.compactionHlsl : std::string{}}};
        if (!emitter.emitterSpawn.instructions.empty()) entry["emitterSpawn"] = Encode(emitter.emitterSpawn);
        if (!emitter.emitterUpdate.instructions.empty()) entry["emitterUpdate"] = Encode(emitter.emitterUpdate);
        entry["eventHandlers"] = Json::array();
        for (const auto& event : emitter.eventHandlers)
            entry["eventHandlers"].push_back(event.instructions.empty() ? Json(nullptr) : Encode(event));
        entry["eventParticles"] = Json::array();
        for (const auto& event : emitter.eventParticles)
            entry["eventParticles"].push_back(event.instructions.empty() ? Json(nullptr) : Encode(event));
        if (!emitter.emitterCollision.instructions.empty()) entry["emitterCollision"] = Encode(emitter.emitterCollision);
        result["emitters"].push_back(std::move(entry));
        auto& stages = result["emitters"].back()["stages"] = Json::array();
        for (const auto& stage : emitter.stages)
            stages.push_back({{"program",Encode(stage.program)},{"hlsl",includeShaderSources ? stage.hlsl : std::string{}}});
    }
    for (const auto& platform : system.platforms)
    {
        Json shaders = {{"backend",static_cast<uint32_t>(platform.backend)},{"emitters",Json::array()}};
        for (const auto& emitter : platform.emitters) shaders["emitters"].push_back(EncodeShaders(emitter));
        result["platforms"].push_back(std::move(shaders));
    }
    return result.dump();
}

bool DeserializeCompiledSystem(std::string_view source, CompiledSystem& system, std::string& error)
{
    if (source.size() > 128u*1024u*1024u) { error = "VFX program exceeds 128 MiB"; return false; }
    try
    {
        const auto data = Json::parse(source);
        const auto version = data.at("version").get<uint32_t>();
        if (data.at("format") != "NullusVFXProgram" || version != 7)
            throw std::runtime_error("Unsupported VFX program format");
        if (data.at("shaderLayoutVersion").get<uint32_t>() != kVfxShaderLayoutVersion)
            throw std::runtime_error("Unsupported VFX shader resource layout");
        CompiledSystem candidate;
        if (!DeserializeSystem(data.at("settings").dump(),candidate.asset,error)) return false;
        if (data.contains("systemSpawn"))
            candidate.systemSpawn = Decode(data.at("systemSpawn"),ScriptStage::SystemSpawn,candidate.asset.systemAttributes);
        if (data.contains("systemUpdate"))
            candidate.systemUpdate = Decode(data.at("systemUpdate"),ScriptStage::SystemUpdate,candidate.asset.systemAttributes);
        const auto& emitters = data.at("emitters");
        if (!emitters.is_array() || emitters.size() != candidate.asset.emitters.size()) throw std::runtime_error("VFX emitter program count mismatch");
        for (size_t index = 0; index < emitters.size(); ++index)
        {
            const auto& entry = emitters[index];
            const auto& attributes = candidate.asset.emitters[index].attributes;
            EmitterPrograms programs;
            programs.spawn = Decode(entry.at("spawn"),ScriptStage::ParticleSpawn,attributes);
            programs.update = Decode(entry.at("update"),ScriptStage::ParticleUpdate,attributes);
            if (entry.contains("emitterSpawn"))
                programs.emitterSpawn = Decode(entry.at("emitterSpawn"),ScriptStage::EmitterSpawn,candidate.asset.emitters[index].emitterAttributes);
            if (entry.contains("emitterUpdate"))
                programs.emitterUpdate = Decode(entry.at("emitterUpdate"),ScriptStage::EmitterUpdate,candidate.asset.emitters[index].emitterAttributes);
            if (entry.contains("eventHandlers"))
            {
                const auto& events = entry.at("eventHandlers");
                if (!events.is_array() || events.size() != candidate.asset.emitters[index].eventHandlers.size())
                    throw std::runtime_error("VFX event handler program count mismatch");
                for (const auto& event : events)
                    programs.eventHandlers.push_back(event.is_null() ? Program{} :
                        Decode(event,ScriptStage::Event,candidate.asset.emitters[index].emitterAttributes));
            }
            else if (!candidate.asset.emitters[index].eventHandlers.empty())
                throw std::runtime_error("VFX event handler program list is missing");
            if (entry.contains("eventParticles"))
            {
                const auto& events = entry.at("eventParticles");
                const auto& handlers = candidate.asset.emitters[index].eventHandlers;
                const bool cpu = candidate.asset.emitters[index].target == SimulationTarget::CPU;
                if (!events.is_array() || (cpu && events.size() != handlers.size()) || (!cpu && !events.empty()))
                    throw std::runtime_error("VFX event particle program count mismatch");
                for (const auto& event : events)
                    programs.eventParticles.push_back(event.is_null() ? Program{} :
                        Decode(event,ScriptStage::Event,candidate.asset.emitters[index].attributes));
            }
            else if (candidate.asset.emitters[index].target == SimulationTarget::CPU &&
                !candidate.asset.emitters[index].eventHandlers.empty())
                throw std::runtime_error("VFX event particle program list is missing");
            if (entry.contains("emitterCollision"))
                programs.emitterCollision = Decode(entry.at("emitterCollision"),ScriptStage::ParticleUpdate,candidate.asset.emitters[index].emitterAttributes);
            programs.spawnHlsl = entry.at("spawnHlsl").get<std::string>();
            programs.updateHlsl = entry.at("updateHlsl").get<std::string>();
            programs.compactionHlsl = entry.value("compactionHlsl",std::string{});
            const auto stages = entry.value("stages",Json::array());
            if (!stages.is_array() || stages.size() != candidate.asset.emitters[index].simulationStages.size() ||
                (!stages.empty() && candidate.asset.emitters[index].target != SimulationTarget::GPU))
                throw std::runtime_error("Simulation Stage bytecode count or target mismatch");
            for (const auto& stage : stages)
                programs.stages.push_back({Decode(stage.at("program"),ScriptStage::SimulationStage,attributes),stage.at("hlsl").get<std::string>()});
            candidate.emitters.push_back(std::move(programs));
        }
        if (version >= 2)
        {
            const auto& platforms = data.at("platforms");
            if (!platforms.is_array() || platforms.size() > 3) throw std::runtime_error("Invalid VFX platform shader packages");
            for (const auto& sourcePlatform : platforms)
            {
                const auto rawBackend = sourcePlatform.at("backend").get<uint32_t>();
                if (rawBackend > (std::numeric_limits<uint8_t>::max)()) throw std::runtime_error("Invalid VFX shader backend");
                const auto backend = static_cast<rhi::NativeBackendType>(rawBackend);
                if ((backend != rhi::NativeBackendType::DX12 && backend != rhi::NativeBackendType::Vulkan && backend != rhi::NativeBackendType::Metal) ||
                    candidate.FindPlatform(backend) || !sourcePlatform.at("emitters").is_array() || sourcePlatform.at("emitters").size() != candidate.emitters.size())
                    throw std::runtime_error("Invalid or duplicate VFX platform shader package");
                PlatformShaderPrograms platform; platform.backend = backend;
                for (size_t index = 0; index < candidate.emitters.size(); ++index)
                    platform.emitters.push_back(DecodeShaders(sourcePlatform.at("emitters")[index],backend,candidate.asset.emitters[index].target == SimulationTarget::GPU,
                        candidate.asset.emitters[index].simulationStages.size(),candidate.asset.emitters[index].renderer));
                candidate.platforms.push_back(std::move(platform));
            }
        }
        if (version < 4 && Json::parse(SerializeCompiledSystem(candidate))["version"] == 4) throw std::runtime_error("Typed data requires VFX program format 4");
        std::vector<DataInterfaceDefinition> bindings;
        if(!CollectCompiledDataInterfaces(candidate,bindings,error))return false;
        system = std::move(candidate);
        error.clear();
        return true;
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}

bool LoadCompiledSystemArtifact(const std::filesystem::path& path, CompiledSystem& system, std::string& error)
{
    const auto portable = Core::Assets::TryMakePortableContentArtifactPath(path.generic_string());
    if (portable.empty() || !Core::Assets::IsRuntimeArtifactPathAuthorized(portable))
    { error = "Particle systems must load from an authorized native artifact"; return false; }
    std::error_code fileError;
    if (const auto size = std::filesystem::file_size(path,fileError); fileError || size > 128u*1024u*1024u)
    { error = "Particle artifact is missing or exceeds 128 MiB"; return false; }
    const auto payload = Core::Assets::ReadNativeArtifactPayloadTextFromFile(path,Core::Assets::ArtifactType::VfxSystem,1);
    if (!payload) { error = "Invalid particle native artifact"; return false; }
    return DeserializeCompiledSystem(payload->payload,system,error);
}
}

#include "VfxShaderCompiler.h"
#include <VFX/VfxCompileCache.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <Core/Assets/NativeArtifactContainer.h>
#include <Core/Assets/ArtifactManifest.h>
#include <Rendering/ShaderCompiler/ShaderCompiler.h>

namespace NLS::VFX
{
bool CompilePlatformPrograms(CompiledSystem& system, std::span<const Render::RHI::NativeBackendType> backends,
    const std::filesystem::path& cacheRoot, std::vector<Diagnostic>& diagnostics, const std::atomic_bool* cancelled,PlatformCompileCacheStats* cacheStats)
{
    namespace rhi = Render::RHI;
    namespace shader = Render::ShaderCompiler;
    diagnostics.clear();
    if (cacheStats) *cacheStats = {};
    if (backends.empty() || system.asset.emitters.size() != system.emitters.size())
    { diagnostics.push_back({"Shader compilation requires target backends and complete emitter programs",{},0}); return false; }
    std::vector<PlatformShaderPrograms> platforms;
    try
    {
        shader::ShaderCompiler compiler;
        const auto checkCancelled = [&]()
        { if (cancelled && cancelled->load(std::memory_order_relaxed)) throw std::runtime_error("Particle shader compilation cancelled"); };
        checkCancelled();
        // Include every generated shader source as well as executable CPU
        // programs/settings and the real toolchain identity. A renderer helper
        // or DXC version change must invalidate the platform package too.
        auto cacheSource = system;
        cacheSource.platforms.clear();
        std::string fingerprint = SerializeCompiledSystem(cacheSource);
        for (const auto& emitter : system.asset.emitters)
        {
            std::string source,error;
            const auto* mesh = emitter.renderer == RendererType::Mesh ? &emitter.mesh : nullptr;
            if (!BuildSpriteShader(emitter.attributes,source,error,emitter.sprite,mesh)) throw std::runtime_error(error);
            fingerprint += source;
            if (!BuildGpuSortShader(emitter.attributes,source,error)) throw std::runtime_error(error); fingerprint += source;
            if (!BuildGpuBoundsShader(emitter.attributes,source,error,mesh)) throw std::runtime_error(error); fingerprint += source;
            if (!BuildGpuVisibilityShader(emitter.attributes,source,error,mesh)) throw std::runtime_error(error); fingerprint += source;
        }
        fingerprint += shader::BuildShaderCompilerToolchainDependencyFingerprint();
        CompileCacheKey packageKey;
        packageKey.sourceHash = HashBytes(fingerprint);
        // Profiles/optimization/target-format changes are explicit key inputs.
        packageKey.dependencyHash = HashBytes("VfxPlatformPackage2:cs_6_0:vs_6_0:ps_6_0:DXIL-SPIRV");
        CompileCache packages(cacheRoot.empty() ? std::filesystem::path{} : cacheRoot/"ShaderMaps");
        const auto compile = [&](const std::string& source, const char* entry, shader::ShaderStage stage, rhi::NativeBackendType backend)
        {
            checkCancelled();
            if (source.empty()) throw std::runtime_error("Particle shader source is missing; recompile the source asset before cooking");
            const std::vector<uint8_t> bytes(source.begin(),source.end());
            const auto directory = cacheRoot/Core::Assets::BuildArtifactStorageFileName(bytes.data(),bytes.size());
            std::filesystem::create_directories(directory);
            const auto path = directory/(std::string(entry)+".hlsl");
            std::string error;
            if (!shader::WriteShaderArtifactTextAtomic(path.string(),source,&error)) throw std::runtime_error(error);
            shader::ShaderCompilationInput input;
            input.assetPath = path.string(); input.stage = stage;
            input.options.targetPlatform = backend == rhi::NativeBackendType::DX12 ? shader::ShaderTargetPlatform::DXIL : shader::ShaderTargetPlatform::SPIRV;
            input.options.entryPoint = entry; input.options.targetProfile = stage == shader::ShaderStage::Vertex ? "vs_6_0" : stage == shader::ShaderStage::Pixel ? "ps_6_0" : "cs_6_0";
            input.options.artifactDirectory = (directory/"Artifacts").string();
            if (cacheStats) ++cacheStats->shaderCompilations;
            const auto result = compiler.Compile(input);
            checkCancelled();
            if (result.status != shader::ShaderCompilationStatus::Succeeded) throw std::runtime_error(result.diagnostics);
            rhi::RHIShaderModuleDesc module;
            module.bytecode = result.bytecode; module.targetBackend = backend; module.entryPoint = entry;
            module.stage = stage == shader::ShaderStage::Vertex ? rhi::ShaderStage::Vertex : stage == shader::ShaderStage::Pixel ? rhi::ShaderStage::Fragment : rhi::ShaderStage::Compute;
            return module;
        };
        for (const auto backend : backends)
        {
            if (backend != rhi::NativeBackendType::DX12 && backend != rhi::NativeBackendType::Vulkan && backend != rhi::NativeBackendType::Metal)
                throw std::runtime_error("Unsupported particle shader backend");
            if (std::any_of(platforms.begin(),platforms.end(),[&](const auto& platform) { return platform.backend == backend; })) continue;
            checkCancelled();
            auto key = packageKey; key.backendMask = 1u << static_cast<uint32_t>(backend);
            const auto cached = packages.Get(key);
            std::string cacheError;
            if (cached && ValidatePlatformPrograms(*cached,backend,cacheError))
            {
                platforms.push_back(*cached->FindPlatform(backend));
                if (cacheStats) ++cacheStats->hits;
                continue;
            }
            if (cacheStats) ++cacheStats->misses;
            PlatformShaderPrograms platform; platform.backend = backend;
            for (size_t index = 0; index < system.emitters.size(); ++index)
            {
                const auto& emitter = system.asset.emitters[index]; const auto& program = system.emitters[index];
                EmitterShaderPrograms shaders;
                if (emitter.target == SimulationTarget::GPU)
                {
                    auto& simulation = shaders.simulation;
                    simulation.beginSpawn = compile(program.spawnHlsl,"BeginSpawn",shader::ShaderStage::Compute,backend);
                    simulation.spawn = compile(program.spawnHlsl,"SpawnParticles",shader::ShaderStage::Compute,backend);
                    simulation.resetEvents = compile(program.spawnHlsl,"ResetEvents",shader::ShaderStage::Compute,backend);
                    simulation.receiveEvents = compile(program.spawnHlsl,"ReceiveEvents",shader::ShaderStage::Compute,backend);
                    simulation.update = compile(program.updateHlsl,"UpdateParticles",shader::ShaderStage::Compute,backend);
                    simulation.advanceSpawn = compile(program.updateHlsl,"AdvanceSpawn",shader::ShaderStage::Compute,backend);
                    simulation.compact = compile(program.compactionHlsl,"Compact",shader::ShaderStage::Compute,backend);
                    simulation.finish = compile(program.compactionHlsl,"Finish",shader::ShaderStage::Compute,backend);
                    simulation.reset = compile(program.compactionHlsl,"Reset",shader::ShaderStage::Compute,backend);
                    if (program.stages.size() != emitter.simulationStages.size()) throw std::runtime_error("Simulation Stage program count mismatch");
                    for (const auto& stage : program.stages) simulation.stages.push_back(compile(stage.hlsl,"Main",shader::ShaderStage::Compute,backend));
                }
                std::string source,error;
                const auto* mesh = emitter.renderer == RendererType::Mesh ? &emitter.mesh : nullptr;
                const bool ribbon = emitter.renderer == RendererType::Ribbon;
                if (!BuildSpriteShader(emitter.attributes,source,error,emitter.sprite,mesh)) throw std::runtime_error(emitter.name+": "+error);
                shaders.spriteVertex = compile(source,mesh ? "MeshVS" : "SpriteVS",shader::ShaderStage::Vertex,backend);
                shaders.spriteFragment = compile(source,"SpritePS",shader::ShaderStage::Pixel,backend);
                if (!BuildGpuSortShader(emitter.attributes,source,error)) throw std::runtime_error(emitter.name+": "+error);
                shaders.sort.initialize = compile(source,"SortInitialize",shader::ShaderStage::Compute,backend);
                shaders.sort.step = compile(source,"SortStep",shader::ShaderStage::Compute,backend);
                if (!BuildGpuBoundsShader(emitter.attributes,source,error,mesh)) throw std::runtime_error(emitter.name+": "+error);
                shaders.simulation.bounds.reduce = compile(source,"BoundsReduce",shader::ShaderStage::Compute,backend);
                shaders.simulation.bounds.finish = compile(source,"BoundsFinish",shader::ShaderStage::Compute,backend);
                if (!BuildGpuVisibilityShader(emitter.attributes,source,error,mesh)) throw std::runtime_error(emitter.name+": "+error);
                shaders.visibility.cull = compile(source,"CullSprites",shader::ShaderStage::Compute,backend);
                shaders.visibility.scan = compile(source,"ScanVisibility",shader::ShaderStage::Compute,backend);
                shaders.visibility.scatter = compile(source,"ScatterVisibility",shader::ShaderStage::Compute,backend);
                platform.emitters.push_back(std::move(shaders));
            }
            auto cachedSystem = std::make_shared<CompiledSystem>(cacheSource);
            cachedSystem->platforms.push_back(platform);
            if (!ValidatePlatformPrograms(*cachedSystem,backend,cacheError)) throw std::runtime_error(cacheError);
            checkCancelled(); packages.Put(key,cachedSystem);
            platforms.push_back(std::move(platform));
        }
        system.platforms = std::move(platforms); return true;
    }
    catch (const std::exception& exception)
    { diagnostics.push_back({exception.what(),{},0}); return false; }
}
}

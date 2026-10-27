#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <Guid.h>
#include <VFXCompiler/VfxCompiler.h>
#include <VFXCompiler/VfxShaderCompiler.h>
#include <Json/json.hpp>
#include <Rendering/ShaderCompiler/ShaderCompiler.h>
#include "VfxStageTestAsset.h"

TEST(VfxShaderTests, GeneratedProgramsCompileToDxilAndSpirv)
{
    namespace shaders = NLS::Render::ShaderCompiler;
    const auto asset = NLS::VFX::CreateDefaultSystem();
    const auto root = std::filesystem::temp_directory_path()/("NullusVfxShader_"+NLS::Guid::New().ToString());
    std::filesystem::create_directories(root);
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path,error); }
    } cleanup{root};
    shaders::ShaderCompiler compiler;
    for (const auto* graph : {&asset.emitters[0].spawn,&asset.emitters[0].update})
    {
        const auto program = NLS::VFX::CompileGraph(*graph,asset.emitters[0].attributes);
        ASSERT_TRUE(program.Succeeded());
        const auto path = root/(graph->id+".hlsl");
        { std::ofstream file(path); file << program.compiled.hlsl; }
        const std::vector<const char*> entries = graph->stage == NLS::VFX::ScriptStage::ParticleSpawn
            ? std::vector<const char*>{"Main","BeginSpawn","SpawnParticles"}
            : std::vector<const char*>{"Main","UpdateParticles","AdvanceSpawn"};
        for (const auto* entry : entries)
        for (const auto target : {shaders::ShaderTargetPlatform::DXIL,shaders::ShaderTargetPlatform::SPIRV})
        {
            shaders::ShaderCompilationInput input;
            input.assetPath = path.string();
            input.stage = shaders::ShaderStage::Compute;
            input.options.targetPlatform = target;
            input.options.targetProfile = "cs_6_0";
            input.options.entryPoint = entry;
            input.options.artifactDirectory = (root/"Artifacts").string();
            const auto output = compiler.Compile(input);
            if (output.diagnostics.find("Unable to locate") != std::string::npos)
                GTEST_SKIP() << output.diagnostics;
            ASSERT_EQ(output.status,shaders::ShaderCompilationStatus::Succeeded) << output.diagnostics << "\n" << program.compiled.hlsl;
            EXPECT_FALSE(output.bytecode.empty());
        }
    }
    std::string compactSource,error;
    ASSERT_TRUE(NLS::VFX::BuildGpuCompactionShader(asset.emitters[0].attributes,compactSource,error)) << error;
    const auto compactPath = root/"compaction.hlsl";
    { std::ofstream file(compactPath); file << compactSource; }
    for (const auto* entry : {"Compact","Finish","Reset"})
    {
        for (const auto target : {shaders::ShaderTargetPlatform::DXIL,shaders::ShaderTargetPlatform::SPIRV})
        {
            shaders::ShaderCompilationInput input;
            input.assetPath = compactPath.string(); input.stage = shaders::ShaderStage::Compute;
            input.options.targetPlatform = target; input.options.targetProfile = "cs_6_0"; input.options.entryPoint = entry;
            input.options.artifactDirectory = (root/"Artifacts").string();
            const auto output = compiler.Compile(input);
            ASSERT_EQ(output.status,shaders::ShaderCompilationStatus::Succeeded) << output.diagnostics;
        }
    }
    std::string spriteSource;
    ASSERT_TRUE(NLS::VFX::BuildSpriteShader(asset.emitters[0].attributes,spriteSource,error)) << error;
    const auto spritePath = root/"sprite.hlsl";
    { std::ofstream file(spritePath); file << spriteSource; }
    for (const auto target : {shaders::ShaderTargetPlatform::DXIL,shaders::ShaderTargetPlatform::SPIRV})
        for (const auto stage : {shaders::ShaderStage::Vertex,shaders::ShaderStage::Pixel})
        {
            shaders::ShaderCompilationInput input;
            input.assetPath = spritePath.string(); input.stage = stage; input.options.targetPlatform = target;
            input.options.targetProfile = stage == shaders::ShaderStage::Vertex ? "vs_6_0" : "ps_6_0";
            input.options.entryPoint = stage == shaders::ShaderStage::Vertex ? "SpriteVS" : "SpritePS";
            input.options.artifactDirectory = (root/"Artifacts").string();
            const auto output = compiler.Compile(input);
            ASSERT_EQ(output.status,shaders::ShaderCompilationStatus::Succeeded) << output.diagnostics;
        }
}

TEST(VfxShaderTests, CookedPackagesRunWithoutSourcesAndRejectIncompatibleKernelsTransactionally)
{
    using namespace NLS::VFX;
    namespace rhi = NLS::Render::RHI;
    auto asset = CreateDefaultSystem();
    auto gpu = NLS::Tests::CreateStagedParticleTestAsset().emitters[0];
    asset.emitters[0].sortByViewDepth = true;
    asset.emitters[0].frustumCulling = false;
    asset.emitters[0].sprite.texture = {NLS::Guid::New().ToString(),13};
    asset.emitters[0].sprite.blend = SpriteBlendMode::Additive;
    asset.emitters[0].sprite.alignment = SpriteAlignment::Velocity;
    asset.emitters[0].sprite.subImagesX = 4; asset.emitters[0].sprite.blendSubImages = true;
    asset.emitters[0].sprite.dynamicParameters[0] = {"Particles.CustomMaterial","User.Dynamic",{0.2f,0.4f,0.6f,0.8f}};
    asset.emitters[0].sprite.material = {NLS::Guid::New().ToString(),29}; asset.emitters[0].sprite.useMaterialShader = true;
    asset.emitters[0].sprite.materialParameters = {{"_Tint","User.Tint",ValueType::Float4,{1,1,1,1}}};
    gpu.sortByViewDepth = true;
    asset.emitters.push_back(std::move(gpu));
    CompiledSystem compiled;
    std::vector<Diagnostic> diagnostics;
    ASSERT_TRUE(CompileSystem(asset,compiled,diagnostics));
    const auto root = std::filesystem::temp_directory_path()/("NullusVfxCook_"+NLS::Guid::New().ToString());
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path,error); } } cleanup{root};
    const std::array targets{rhi::NativeBackendType::DX12,rhi::NativeBackendType::Vulkan,rhi::NativeBackendType::Metal};
    ASSERT_TRUE(CompilePlatformPrograms(compiled,targets,root,diagnostics)) << (diagnostics.empty() ? "" : diagnostics[0].message);
    const auto text = SerializeCompiledSystem(compiled,false);
    CompiledSystem restored;
    std::string error;
    ASSERT_TRUE(DeserializeCompiledSystem(text,restored,error)) << error;
    EXPECT_EQ(restored.asset.emitters[0].sprite.texture.guid,asset.emitters[0].sprite.texture.guid);
    EXPECT_EQ(restored.asset.emitters[0].sprite.texture.localId,13);
    EXPECT_EQ(restored.asset.emitters[0].sprite.blend,SpriteBlendMode::Additive);
    EXPECT_EQ(restored.asset.emitters[0].sprite.alignment,SpriteAlignment::Velocity);
    EXPECT_EQ(restored.asset.emitters[0].sprite.subImagesX,4u);
    EXPECT_TRUE(restored.asset.emitters[0].sprite.blendSubImages);
    EXPECT_TRUE(restored.asset.emitters[0].sprite.useMaterialShader);
    EXPECT_EQ(restored.asset.emitters[0].sprite.dynamicParameters[0].attribute,"Particles.CustomMaterial");
    EXPECT_EQ(restored.asset.emitters[0].sprite.dynamicParameters[0].fallbackParameter,"User.Dynamic");
    ASSERT_EQ(restored.asset.emitters[0].sprite.materialParameters.size(),1u);
    EXPECT_EQ(restored.asset.emitters[0].sprite.materialParameters[0].systemParameter,"User.Tint");
    ASSERT_EQ(restored.platforms.size(),3u);
    EXPECT_TRUE(restored.asset.emitters[0].sortByViewDepth);
    EXPECT_TRUE(restored.asset.emitters[1].sortByViewDepth);
    EXPECT_FALSE(restored.asset.emitters[0].frustumCulling);
    EXPECT_TRUE(restored.asset.emitters[1].frustumCulling);
    for (const auto target : targets) EXPECT_TRUE(ValidatePlatformPrograms(restored,target,error)) << error;
    for (const auto& emitter : restored.emitters)
    {
        EXPECT_TRUE(emitter.spawnHlsl.empty()); EXPECT_TRUE(emitter.updateHlsl.empty()); EXPECT_TRUE(emitter.compactionHlsl.empty());
    }
    EXPECT_TRUE(restored.asset.emitters[1].spawn.nodes.empty());
    ASSERT_EQ(restored.emitters[1].stages.size(),5u);
    EXPECT_TRUE(restored.emitters[1].stages[0].hlsl.empty()); EXPECT_TRUE(restored.asset.emitters[1].simulationStages[0].graph.nodes.empty());
    EXPECT_EQ(SerializeCompiledSystem(restored,false),text);
    const auto original = nlohmann::json::parse(text);
    const auto reject = [&](auto mutate)
    {
        auto malformed = original; mutate(malformed);
        EXPECT_FALSE(DeserializeCompiledSystem(malformed.dump(),restored,error));
        EXPECT_FALSE(error.empty()); EXPECT_EQ(SerializeCompiledSystem(restored,false),text);
    };
    reject([](auto& value) { value["shaderLayoutVersion"] = 999; });
    reject([](auto& value) { value["platforms"][0]["backend"] = 255; });
    reject([](auto& value) { value["platforms"].push_back(value["platforms"][0]); });
    reject([](auto& value) { value["platforms"][0]["emitters"][1][0]["entry"] = "Wrong"; });
    reject([](auto& value) { value["platforms"][0]["emitters"][1][0]["stage"] = 999; });
    reject([](auto& value) { value["platforms"][0]["emitters"][1][0]["code"] = "zz"; });
    reject([](auto& value) { value["platforms"][0]["emitters"][1][0]["code"] = "0"; });
    reject([](auto& value) { value["platforms"][0]["emitters"][1][0]["code"] = ""; });
    reject([](auto& value) { value["platforms"][0]["emitters"][1].erase(0); });
    reject([](auto& value) { value["platforms"][0]["emitters"][0][9]["entry"] = "WrongSortKernel"; });
    reject([](auto& value) { value["platforms"][0]["emitters"][1][10]["code"] = ""; });
    reject([](auto& value) { value["platforms"][0]["emitters"][1][11]["code"] = ""; });
    reject([](auto& value) { value["platforms"][0]["emitters"][0][13]["entry"] = "WrongCullingKernel"; });
    reject([](auto& value) { value["platforms"][0]["emitters"][1][16]["code"] = ""; });
    reject([](auto& value) { value["emitters"][1]["stages"].erase(0); });
    reject([](auto& value) { value["emitters"][1]["stages"][0]["program"]["stage"] = 0; });

    const auto previous = SerializeCompiledSystem(compiled,false);
    std::atomic_bool cancelled = true;
    EXPECT_FALSE(CompilePlatformPrograms(compiled,targets,root,diagnostics,&cancelled));
    EXPECT_EQ(SerializeCompiledSystem(compiled,false),previous);
    compiled.emitters[1].spawnHlsl = "invalid hlsl";
    EXPECT_FALSE(CompilePlatformPrograms(compiled,targets,root,diagnostics));
    EXPECT_EQ(SerializeCompiledSystem(compiled,false),previous);
}

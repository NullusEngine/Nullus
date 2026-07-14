#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/ShaderManager.h"
#include "Core/ServiceLocator.h"
#include "Guid.h"
#include "Rendering/Resources/Material.h"
#include "Rendering/Resources/Shader.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/ShaderLab/ShaderLabAsset.h"
#include "Rendering/ShaderLab/ShaderLabHotReload.h"
#include "Rendering/ShaderLab/ShaderLabPipelineKey.h"

namespace
{
    NLS::Render::ShaderLab::ShaderLabAssetDesc MakePassShaderDesc()
    {
        using namespace NLS::Render::ShaderLab;

        ShaderLabAssetDesc desc;
        desc.shaderName = "Nullus/PassTest";
        ShaderLabSubShaderDesc subShader;

        ShaderLabPassDesc forward;
        forward.name = "Forward";
        forward.tags.values["LightMode"] = "Forward";
        forward.vertexEntry = "VSForward";
        forward.fragmentEntry = "PSForward";
        forward.state.cullMode = ShaderLabCullMode::Back;
        forward.state.depthWrite = true;
        forward.state.depthCompare = NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL;

        ShaderLabPassDesc depth;
        depth.name = "DepthOnly";
        depth.tags.values["LightMode"] = "DepthOnly";
        depth.vertexEntry = "VSDepth";
        depth.fragmentEntry = "PSDepth";
        depth.state.cullMode = ShaderLabCullMode::Off;
        depth.state.depthWrite = true;
        depth.state.depthCompare = NLS::Render::Settings::EComparaisonAlgorithm::LESS;

        subShader.passes.push_back(std::move(forward));
        subShader.passes.push_back(std::move(depth));
        desc.subShaders.push_back(std::move(subShader));
        return desc;
    }

    class AlwaysValidReloadValidator final : public NLS::Render::ShaderLab::IShaderLabReloadValidator
    {
    public:
        NLS::Render::ShaderLab::ShaderLabReloadValidationResult Validate(
            const NLS::Render::ShaderLab::ShaderLabAssetDesc&,
            const std::vector<NLS::Render::ShaderLab::ShaderLabVariantKey>&) override
        {
            return { true, {} };
        }
    };

    class ScopedTrustedBuiltInShaderAssetsRoot final
    {
    public:
        explicit ScopedTrustedBuiltInShaderAssetsRoot(const std::string& engineAssetsPath)
        {
            NLS::Render::Resources::Loaders::ShaderLoader::SetTrustedBuiltInShaderEngineAssetsPath(
                engineAssetsPath);
        }

        ~ScopedTrustedBuiltInShaderAssetsRoot()
        {
            NLS::Render::Resources::Loaders::ShaderLoader::SetTrustedBuiltInShaderEngineAssetsPath({});
        }
    };

    void ReloadShaderForPassTest(
        const std::shared_ptr<NLS::Render::ShaderLab::ShaderLabAsset>& shader,
        NLS::Render::ShaderLab::ShaderLabAssetDesc desc)
    {
        AlwaysValidReloadValidator validator;
        NLS::Render::ShaderLab::ShaderLabHotReloadService reloader(validator);
        NLS::Render::ShaderLab::ShaderLabReloadRequest request;
        request.shader = shader;
        request.candidate = std::move(desc);
        ASSERT_TRUE(reloader.Reload(request).succeeded);
    }
}

TEST(ShaderLabPassAndPipelineTests, FindsPassByLightModeAndFailsClosedWhenMissing)
{
    const NLS::Render::ShaderLab::ShaderLabAsset shader(
        NLS::Guid::NewDeterministic("pass-shader"),
        MakePassShaderDesc());

    const auto forward = shader.FindPass(0, "Forward");
    ASSERT_TRUE(forward);
    const auto forwardPass = forward.GetPass();
    ASSERT_NE(forwardPass, nullptr);
    EXPECT_EQ(forwardPass->name, "Forward");
    EXPECT_EQ(forwardPass->passIndex, 0u);
    EXPECT_EQ(forward.GetSubShaderIndex(), 0u);
    EXPECT_EQ(forward.GetPassIndex(), 0u);

    const auto depth = shader.FindPass(0, "DepthOnly");
    ASSERT_TRUE(depth);
    const auto depthPass = depth.GetPass();
    ASSERT_NE(depthPass, nullptr);
    EXPECT_EQ(depthPass->name, "DepthOnly");
    EXPECT_EQ(depthPass->passIndex, 1u);

    EXPECT_FALSE(shader.FindPass(0, "ShadowCaster"));
    EXPECT_FALSE(shader.FindPass(1, "Forward"));
}

TEST(ShaderLabPassAndPipelineTests, PassHandlesRemainGenerationSafeAfterReload)
{
    using namespace NLS::Render::ShaderLab;

    auto shader = std::make_shared<ShaderLabAsset>(
        NLS::Guid::NewDeterministic("pass-handle-shader"),
        MakePassShaderDesc());
    const auto oldHandle = shader->GetPassHandle(0, "Forward");
    ASSERT_TRUE(oldHandle.IsValid());
    const auto oldPass = shader->ResolvePass(oldHandle);
    ASSERT_TRUE(oldPass);
    ASSERT_NE(oldPass.GetPass(), nullptr);
    EXPECT_EQ(oldPass.GetPass()->name, "Forward");

    auto newDesc = MakePassShaderDesc();
    newDesc.subShaders[0].passes.erase(newDesc.subShaders[0].passes.begin());
    ReloadShaderForPassTest(shader, std::move(newDesc));

    EXPECT_FALSE(shader->ResolvePass(oldHandle));
    const auto newHandle = shader->GetPassHandle(0, "DepthOnly");
    EXPECT_TRUE(newHandle.IsValid());
    const auto newPass = shader->ResolvePass(newHandle).GetPass();
    ASSERT_NE(newPass, nullptr);
    EXPECT_EQ(newPass->name, "DepthOnly");
}

TEST(ShaderLabPassAndPipelineTests, SnapshotHandleKeepsOldPassAliveWithoutAssetRetainingAllRetiredSnapshots)
{
    using namespace NLS::Render::ShaderLab;

    auto shader = std::make_shared<ShaderLabAsset>(
        NLS::Guid::NewDeterministic("snapshot-handle-shader"),
        MakePassShaderDesc());
    const auto oldForward = shader->FindPass(0, "Forward");
    ASSERT_TRUE(oldForward);

    auto newDesc = MakePassShaderDesc();
    newDesc.subShaders[0].passes.erase(newDesc.subShaders[0].passes.begin());
    ReloadShaderForPassTest(shader, std::move(newDesc));

    EXPECT_EQ(shader->GetRetainedSnapshotCountForTests(), 1u);
    ASSERT_NE(oldForward.GetPass(), nullptr);
    EXPECT_EQ(oldForward.GetPass()->name, "Forward");
    EXPECT_NE(oldForward.GetGeneration(), shader->GetGeneration());
}

TEST(ShaderLabPassAndPipelineTests, RenderStateChangesGraphicsPipelineKeyButNotArtifactKey)
{
    using namespace NLS::Render::ShaderLab;

    const auto shaderGuid = NLS::Guid::NewDeterministic("pipeline-shader");
    ShaderLabGraphicsPipelineKey forward;
    forward.program.shaderGuid = shaderGuid;
    forward.program.subShaderIndex = 0;
    forward.program.passIndex = 0;
    forward.program.keywordHash = HashShaderLabString("keywords");
    forward.program.vertexArtifactHash = HashShaderLabString("vs-artifact");
    forward.program.fragmentArtifactHash = HashShaderLabString("ps-artifact");
    forward.renderStateHash = HashShaderLabString("CullBackDepthLEqual");
    forward.vertexLayoutHash = HashShaderLabString("P3N3UV2");
    forward.renderTargetLayoutHash = HashShaderLabString("RGBA8+D24");
    forward.topology = NLS::Render::RHI::PrimitiveTopology::TriangleList;
    forward.sampleCount = 1;

    auto depth = forward;
    depth.program.passIndex = 1;
    depth.renderStateHash = HashShaderLabString("CullOffDepthLess");
    EXPECT_NE(forward.Hash(), depth.Hash());

    auto recompiledForward = forward;
    recompiledForward.program.fragmentArtifactHash = HashShaderLabString("ps-artifact-after-include-change");
    EXPECT_NE(forward.Hash(), recompiledForward.Hash())
        << "PSO program identity must change when compiled shader artifacts change.";

    ShaderLabArtifactKey artifact;
    artifact.shaderGuid = shaderGuid;
    artifact.subShaderIndex = 0;
    artifact.passIndex = 0;
    artifact.stage = NLS::Render::ShaderCompiler::ShaderStage::Pixel;
    artifact.entryPoint = "PSMain";
    artifact.hlslSourceHash = HashShaderLabString("source");
    artifact.includeDependencyHash = HashShaderLabString("include");
    artifact.keywordHash = 0;
    artifact.backend = NLS::Render::RHI::NativeBackendType::DX12;
    artifact.target = NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
    artifact.shaderModel = ShaderLabShaderModel::SM6_6;
    artifact.compilerFingerprint = "dxc";
    artifact.compileArgumentsHash = HashShaderLabString("-O0");

    const auto artifactHash = artifact.Hash();
    const auto psoHashBeforeRtChange = forward.Hash();
    forward.renderTargetLayoutHash = HashShaderLabString("RGBA16F+D32");
    EXPECT_EQ(artifact.Hash(), artifactHash)
        << "Render target layout belongs to PSO identity, not shader artifact identity.";
    EXPECT_NE(forward.Hash(), psoHashBeforeRtChange)
        << "Render target layout must remain part of graphics PSO identity.";
}

TEST(ShaderLabPassAndPipelineTests, ShaderLabBlendFlagsEnterMaterialPipelineOverrides)
{
    using namespace NLS::Render::Resources;
    using namespace NLS::Render::ShaderLab;

    ShaderLabPassState passState;
    passState.blend.alphaToCoverageEnable = true;
    passState.blend.independentBlendEnable = false;

    const auto overrides = BuildMaterialPipelineStateOverrides(passState);

    ASSERT_TRUE(overrides.alphaToCoverage.has_value());
    EXPECT_TRUE(*overrides.alphaToCoverage);
    ASSERT_TRUE(overrides.independentBlend.has_value());
    EXPECT_FALSE(*overrides.independentBlend);

    auto changed = overrides;
    changed.independentBlend = true;
    EXPECT_NE(overrides, changed);
    EXPECT_NE(overrides.GetHash(), changed.GetHash());
}

TEST(ShaderLabPassAndPipelineTests, CullOffDisablesRhiCulling)
{
    using namespace NLS::Render::ShaderLab;

    const auto off = ToRhiRasterState(ShaderLabCullMode::Off);
    EXPECT_FALSE(off.cullEnabled);
    EXPECT_EQ(off.cullFace, NLS::Render::Settings::ECullFace::BACK);

    const auto front = ToRhiRasterState(ShaderLabCullMode::Front);
    EXPECT_TRUE(front.cullEnabled);
    EXPECT_EQ(front.cullFace, NLS::Render::Settings::ECullFace::FRONT);
}

TEST(ShaderLabPassAndPipelineTests, PassStateBuildsMaterialPipelineOverrides)
{
    using namespace NLS::Render::ShaderLab;

    ShaderLabPassState state;
    state.cullMode = ShaderLabCullMode::Front;
    state.depthWrite = false;
    state.depthCompare = NLS::Render::Settings::EComparaisonAlgorithm::GREATER;
    state.blend.enabled = true;
    NLS::Render::RHI::RHIRenderTargetBlendStateDesc target;
    target.blendEnable = true;
    target.colorWriteMask = NLS::Render::RHI::RHIColorWriteMask::All;
    state.blend.renderTargets.push_back(target);

    const auto overrides = NLS::Render::Resources::BuildMaterialPipelineStateOverrides(state);
    ASSERT_TRUE(overrides.culling.has_value());
    EXPECT_TRUE(*overrides.culling);
    ASSERT_TRUE(overrides.cullFace.has_value());
    EXPECT_EQ(*overrides.cullFace, NLS::Render::Settings::ECullFace::FRONT);
    ASSERT_TRUE(overrides.depthWrite.has_value());
    EXPECT_FALSE(*overrides.depthWrite);
    ASSERT_TRUE(overrides.depthCompare.has_value());
    EXPECT_EQ(*overrides.depthCompare, NLS::Render::Settings::EComparaisonAlgorithm::GREATER);
    ASSERT_TRUE(overrides.blending.has_value());
    EXPECT_TRUE(*overrides.blending);
    ASSERT_EQ(overrides.GetRenderTargetBlendStates().size(), 1u);
    EXPECT_TRUE(overrides.GetRenderTargetBlendStates()[0].blendEnable);

    auto changed = overrides;
    changed.depthCompare = NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL;
    EXPECT_NE(overrides.GetHash(), changed.GetHash());
}

TEST(ShaderLabPassAndPipelineTests, LegacyShaderCarriesSelectedShaderLabPassStateForMaterialPso)
{
    auto* shader = NLS::Render::Resources::Shader::CreateForTesting("Assets/Shaders/Forward.shader");
    NLS::Render::ShaderLab::ShaderLabPassState passState;
    passState.cullMode = NLS::Render::ShaderLab::ShaderLabCullMode::Off;
    passState.depthWrite = false;
    passState.depthCompare = NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL;
    passState.blend.enabled = true;

    ASSERT_NE(shader, nullptr);
    shader->SetShaderLabPassStateForTesting(passState);
    ASSERT_TRUE(shader->GetShaderLabPassState().has_value());

    const auto overrides = NLS::Render::Resources::BuildMaterialPipelineStateOverrides(
        *shader->GetShaderLabPassState());
    ASSERT_TRUE(overrides.culling.has_value());
    EXPECT_FALSE(*overrides.culling);
    ASSERT_TRUE(overrides.depthWrite.has_value());
    EXPECT_FALSE(*overrides.depthWrite);
    ASSERT_TRUE(overrides.depthCompare.has_value());
    EXPECT_EQ(*overrides.depthCompare, NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL);
    ASSERT_TRUE(overrides.blending.has_value());
    EXPECT_TRUE(*overrides.blending);

    NLS::Render::Resources::Shader::DestroyForTesting(shader);
}

TEST(ShaderLabPassAndPipelineTests, MaterialResolvesShaderLabPassArtifactsByLightMode)
{
    using NLS::Render::Resources::Material;
    using NLS::Render::Resources::Shader;
    using NLS::Render::ShaderLab::ShaderLabPassState;

    auto* forward = Shader::CreateForTesting("Library/Artifacts/12/forwardhash");
    auto* depth = Shader::CreateForTesting("Library/Artifacts/34/depthhash");
    ASSERT_NE(forward, nullptr);
    ASSERT_NE(depth, nullptr);

    ShaderLabPassState forwardState;
    forwardState.depthWrite = true;
    forward->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi",
        "Forward",
        forwardState);

    ShaderLabPassState depthState;
    depthState.cullMode = NLS::Render::ShaderLab::ShaderLabCullMode::Off;
    depthState.depthWrite = true;
    depthState.depthCompare = NLS::Render::Settings::EComparaisonAlgorithm::LESS_EQUAL;
    depth->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/DepthOnly#1",
        "DepthOnly",
        depthState);

    Material material(forward);
    material.SetShaderLabSourcePath("Assets/Shaders/Multi.shader");
    material.RegisterShaderLabPassShader(forward);
    material.RegisterShaderLabPassShader(depth);

    EXPECT_EQ(material.ResolveShaderForLightMode("Forward"), forward);
    EXPECT_EQ(material.ResolveShaderForLightMode("DepthOnly"), depth);
    EXPECT_EQ(material.ResolveShaderForLightMode("ShadowCaster"), nullptr)
        << "Renderer must fail closed when the ShaderLab asset has no matching LightMode pass.";

    Shader::DestroyForTesting(forward);
    Shader::DestroyForTesting(depth);
}

TEST(ShaderLabPassAndPipelineTests, DeferredDecalLightModeResolvesDedicatedBuiltInAndShaderLabPasses)
{
    using NLS::Render::Resources::Loaders::ShaderLoader;
    using NLS::Render::Resources::Material;
    using NLS::Render::Resources::Shader;

    const auto builtInPath =
        std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/DeferredDecal.hlsl";
    auto* builtIn = ShaderLoader::CreateBuiltInHlsl(builtInPath.string());
    ASSERT_NE(builtIn, nullptr);
    Material builtInMaterial(builtIn);
    EXPECT_EQ(builtInMaterial.ResolveShaderForLightMode("DeferredDecal"), builtIn);
    EXPECT_EQ(builtInMaterial.ResolveShaderForLightMode("GBuffer"), nullptr);
    EXPECT_TRUE(ShaderLoader::Destroy(builtIn));

    auto* forward = Shader::CreateForTesting("Library/Artifacts/12/forwardhash");
    auto* gbuffer = Shader::CreateForTesting("Library/Artifacts/34/gbufferhash");
    auto* decal = Shader::CreateForTesting("Library/Artifacts/56/decalhash");
    ASSERT_NE(forward, nullptr);
    ASSERT_NE(gbuffer, nullptr);
    ASSERT_NE(decal, nullptr);

    forward->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/StandardPBR.shader",
        "shader:StandardPBR/Forward#0",
        "Forward",
        {});
    gbuffer->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/StandardPBR.shader",
        "shader:StandardPBR/GBuffer#1",
        "GBuffer",
        {});
    decal->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/StandardPBR.shader",
        "shader:StandardPBR/DeferredDecal#2",
        "DeferredDecal",
        {});

    Material shaderLabMaterial(forward);
    shaderLabMaterial.SetShaderLabSourcePath("Assets/Shaders/StandardPBR.shader");
    shaderLabMaterial.RegisterShaderLabPassShader(forward);
    shaderLabMaterial.RegisterShaderLabPassShader(gbuffer);
    shaderLabMaterial.RegisterShaderLabPassShader(decal);

    EXPECT_EQ(shaderLabMaterial.ResolveShaderForLightMode("Forward"), forward);
    EXPECT_EQ(shaderLabMaterial.ResolveShaderForLightMode("GBuffer"), gbuffer);
    EXPECT_EQ(shaderLabMaterial.ResolveShaderForLightMode("DeferredDecal"), decal);
    EXPECT_NE(
        shaderLabMaterial.ResolveShaderForLightMode("DeferredDecal"),
        shaderLabMaterial.ResolveShaderForLightMode("GBuffer"));

    Shader::DestroyForTesting(forward);
    Shader::DestroyForTesting(gbuffer);
    Shader::DestroyForTesting(decal);
}

TEST(ShaderLabPassAndPipelineTests, BuiltInLightModeInferenceRejectsAdjacentAssetsDirectoryName)
{
    using NLS::Render::Resources::Loaders::ShaderLoader;
    using NLS::Render::Resources::Material;

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_deferred_decal_adjacent_assets_" + NLS::Guid::New().ToString());
    const auto engineAssets = root / "MyAssets" / "Engine";
    const auto shaderPath = engineAssets / "Shaders" / "DeferredDecal.hlsl";
    const auto projectAssets = root / "Project" / "Assets";
    std::filesystem::create_directories(shaderPath.parent_path());
    std::filesystem::create_directories(projectAssets);
    std::ofstream shaderFile(shaderPath, std::ios::binary);
    ASSERT_TRUE(shaderFile.is_open());
    shaderFile
        << "struct VSOutput { float4 position : SV_Position; };\n"
        << "VSOutput VSMain(uint vertexId : SV_VertexID) {\n"
        << "    VSOutput output;\n"
        << "    output.position = float4(0.0f, 0.0f, 0.0f, 1.0f);\n"
        << "    return output;\n"
        << "}\n"
        << "float4 PSMain(VSOutput input) : SV_Target0 {\n"
        << "    return float4(1.0f, 1.0f, 1.0f, 1.0f);\n"
        << "}\n";
    shaderFile.close();

    const ScopedTrustedBuiltInShaderAssetsRoot trustedRoot(engineAssets.string());
    auto* shader = ShaderLoader::CreateBuiltInHlsl(shaderPath.string(), projectAssets.string());
    ASSERT_NE(shader, nullptr);
    EXPECT_TRUE(shader->GetShaderLabLightMode().empty())
        << "MyAssets must not match the Assets directory segment used by built-in shaders.";
    Material material(shader);
    EXPECT_EQ(material.ResolveShaderForLightMode("DeferredDecal"), nullptr);
    EXPECT_EQ(material.ResolveShaderForLightMode("Forward"), shader);
    EXPECT_TRUE(ShaderLoader::Destroy(shader));

    std::filesystem::remove_all(root);
}

TEST(ShaderLabPassAndPipelineTests, LegacyMaterialFallsBackOnlyForForwardLightMode)
{
    using NLS::Render::Resources::Material;
    using NLS::Render::Resources::Shader;

    auto* shader = Shader::CreateForTesting("Library/Artifacts/12/legacyhash");
    ASSERT_NE(shader, nullptr);

    Material material(shader);

    EXPECT_EQ(material.ResolveShaderForLightMode("Forward"), shader)
        << "Legacy non-ShaderLab materials must still render through the default forward path.";
    EXPECT_EQ(material.ResolveShaderForLightMode("GBuffer"), nullptr)
        << "Explicit non-forward LightMode queries must fail closed instead of reusing a forward shader.";

    Shader::DestroyForTesting(shader);
}

TEST(ShaderLabPassAndPipelineTests, DuplicateLightModeRegistrationKeepsFirstPass)
{
    using NLS::Render::Resources::Material;
    using NLS::Render::Resources::Shader;

    auto* first = Shader::CreateForTesting("Library/Artifacts/12/firstforward");
    auto* second = Shader::CreateForTesting("Library/Artifacts/34/secondforward");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    first->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Duplicate.shader",
        "shader:Duplicate/Forward#0",
        "Forward",
        {});
    second->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Duplicate.shader",
        "shader:Duplicate/Forward#1",
        "Forward",
        {});

    Material material(first);
    material.SetShaderLabSourcePath("Assets/Shaders/Duplicate.shader");
    material.RegisterShaderLabPassShader(first);
    material.RegisterShaderLabPassShader(second);

    EXPECT_EQ(material.ResolveShaderForLightMode("Forward"), first)
        << "Duplicate LightMode passes must not depend on artifact registration order.";

    Shader::DestroyForTesting(first);
    Shader::DestroyForTesting(second);
}

TEST(ShaderLabPassAndPipelineTests, MaterialRejectsStaleLightModeMapEntriesAfterShaderReload)
{
    using NLS::Render::Resources::Material;
    using NLS::Render::Resources::Shader;

    auto* shader = Shader::CreateForTesting("Library/Artifacts/12/forwardhash");
    ASSERT_NE(shader, nullptr);

    shader->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/Forward#0",
        "Forward",
        {});

    Material material(shader);
    material.SetShaderLabSourcePath("Assets/Shaders/Multi.shader");
    material.RegisterShaderLabPassShader(shader);
    ASSERT_EQ(material.ResolveShaderForLightMode("Forward"), shader);

    shader->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/DepthOnly#0",
        "DepthOnly",
        {});

    EXPECT_EQ(material.ResolveShaderForLightMode("Forward"), nullptr)
        << "A hot-reloaded pass whose LightMode changed must not remain reachable under the old key.";
    EXPECT_EQ(material.ResolveShaderForLightMode("DepthOnly"), shader);

    Shader::DestroyForTesting(shader);
}

TEST(ShaderLabPassAndPipelineTests, EffectivePassBindingRebuildDoesNotAdvanceMaterialBindingRevision)
{
    using NLS::Render::Resources::Material;
    using NLS::Render::Resources::Shader;

    auto* forward = Shader::CreateForTesting("Library/Artifacts/12/forwardhash");
    auto* depth = Shader::CreateForTesting("Library/Artifacts/34/depthhash");
    ASSERT_NE(forward, nullptr);
    ASSERT_NE(depth, nullptr);

    forward->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/Forward#0",
        "Forward",
        {});
    depth->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/DepthOnly#1",
        "DepthOnly",
        {});

    Material material(forward);
    material.SetShaderLabSourcePath("Assets/Shaders/Multi.shader");
    material.RegisterShaderLabPassShader(forward);
    material.RegisterShaderLabPassShader(depth);

    const auto revisionBeforePassBindingQueries = material.GetBindingRevision();
    EXPECT_EQ(material.GetRecordedBindingSet(nullptr, forward), nullptr);
    EXPECT_EQ(material.GetRecordedBindingSet(nullptr, depth), nullptr);
    EXPECT_EQ(material.GetRecordedBindingSet(nullptr, forward), nullptr);

    EXPECT_EQ(material.GetBindingRevision(), revisionBeforePassBindingQueries)
        << "Pass-specific binding rebuilds must not invalidate material-wide PSO cache keys.";

    Shader::DestroyForTesting(forward);
    Shader::DestroyForTesting(depth);
}

TEST(ShaderLabPassAndPipelineTests, ShaderManagerUnregistrationClearsMaterialShaderLabPassReferences)
{
    using NLS::Render::Resources::Material;
    using NLS::Render::Resources::Shader;

    NLS::Core::ResourceManagement::ShaderManager shaderManager;
    NLS::Core::ResourceManagement::MaterialManager materialManager;
    NLS::Core::ServiceLocator::Provide<NLS::Core::ResourceManagement::MaterialManager>(materialManager);

    auto* forward = Shader::CreateForTesting("Library/Artifacts/12/forwardhash");
    auto* depth = Shader::CreateForTesting("Library/Artifacts/34/depthhash");
    ASSERT_NE(forward, nullptr);
    ASSERT_NE(depth, nullptr);

    forward->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/Forward#0",
        "Forward",
        {});
    depth->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/DepthOnly#1",
        "DepthOnly",
        {});

    auto* material = new Material(forward);
    material->SetShaderLabSourcePath("Assets/Shaders/Multi.shader");
    material->RegisterShaderLabPassShader(forward);
    material->RegisterShaderLabPassShader(depth);
    materialManager.RegisterResource("Library/Artifacts/ab/materialhash", material);
    shaderManager.RegisterResource("Library/Artifacts/12/forwardhash", forward);
    shaderManager.RegisterResource("Library/Artifacts/34/depthhash", depth);
    ASSERT_EQ(material->ResolveShaderForLightMode("DepthOnly"), depth);

    shaderManager.UnloadResource("Library/Artifacts/34/depthhash");

    EXPECT_EQ(material->ResolveShaderForLightMode("DepthOnly"), nullptr);
    EXPECT_EQ(material->ResolveShaderForLightMode("Forward"), forward);

    materialManager.UnloadResources();
    shaderManager.UnloadResources();
    NLS::Core::ServiceLocator::Remove<NLS::Core::ResourceManagement::MaterialManager>();
}

TEST(ShaderLabPassAndPipelineTests, ShaderManagerUnregistrationClearsUnmanagedLiveMaterials)
{
    using NLS::Render::Resources::Material;
    using NLS::Render::Resources::Shader;

    NLS::Core::ResourceManagement::ShaderManager shaderManager;

    auto* forward = Shader::CreateForTesting("Library/Artifacts/12/forwardhash");
    auto* depth = Shader::CreateForTesting("Library/Artifacts/34/depthhash");
    ASSERT_NE(forward, nullptr);
    ASSERT_NE(depth, nullptr);

    forward->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/Forward#0",
        "Forward",
        {});
    depth->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/DepthOnly#1",
        "DepthOnly",
        {});

    Material unmanaged(forward);
    unmanaged.SetShaderLabSourcePath("Assets/Shaders/Multi.shader");
    unmanaged.RegisterShaderLabPassShader(forward);
    unmanaged.RegisterShaderLabPassShader(depth);
    shaderManager.RegisterResource("Library/Artifacts/12/forwardhash", forward);
    shaderManager.RegisterResource("Library/Artifacts/34/depthhash", depth);
    ASSERT_EQ(unmanaged.ResolveShaderForLightMode("DepthOnly"), depth);

    shaderManager.UnloadResource("Library/Artifacts/34/depthhash");

    EXPECT_EQ(unmanaged.ResolveShaderForLightMode("DepthOnly"), nullptr);
    EXPECT_EQ(unmanaged.ResolveShaderForLightMode("Forward"), forward);

    shaderManager.UnloadResources();
}

TEST(ShaderLabPassAndPipelineTests, ShaderLoaderDestroyClearsUnmanagedLiveMaterials)
{
    using NLS::Render::Resources::Loaders::ShaderLoader;
    using NLS::Render::Resources::Material;
    using NLS::Render::Resources::Shader;

    auto* forward = Shader::CreateForTesting("Library/Artifacts/12/forwardhash");
    auto* depth = Shader::CreateForTesting("Library/Artifacts/34/depthhash");
    ASSERT_NE(forward, nullptr);
    ASSERT_NE(depth, nullptr);

    forward->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/Forward#0",
        "Forward",
        {});
    depth->SetImportedShaderLabPassForTesting(
        "Assets/Shaders/Multi.shader",
        "shader:Multi/DepthOnly#1",
        "DepthOnly",
        {});

    Material unmanaged(forward);
    unmanaged.SetShaderLabSourcePath("Assets/Shaders/Multi.shader");
    unmanaged.RegisterShaderLabPassShader(forward);
    unmanaged.RegisterShaderLabPassShader(depth);
    ASSERT_EQ(unmanaged.ResolveShaderForLightMode("DepthOnly"), depth);

    EXPECT_TRUE(ShaderLoader::Destroy(depth));

    EXPECT_EQ(unmanaged.ResolveShaderForLightMode("DepthOnly"), nullptr);
    EXPECT_EQ(unmanaged.ResolveShaderForLightMode("Forward"), forward);

    EXPECT_TRUE(ShaderLoader::Destroy(forward));
}

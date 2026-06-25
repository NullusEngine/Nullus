#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "Guid.h"
#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/Resources/Loaders/ShaderLoader.h"
#include "Rendering/ShaderLab/ShaderLabAsset.h"
#include "Rendering/ShaderLab/ShaderLabParser.h"
#include "Rendering/ShaderLab/ShaderLabVariant.h"

namespace
{
void WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
}

TEST(ShaderLabVariantTests, KeywordHashIsStableAndOrderIndependent)
{
    NLS::Render::ShaderLab::ShaderLabKeywordSet first;
    first.Enable("_NORMALMAP");
    first.Enable("MAIN_LIGHT_SHADOWS");

    NLS::Render::ShaderLab::ShaderLabKeywordSet second;
    second.Enable("MAIN_LIGHT_SHADOWS");
    second.Enable("_NORMALMAP");

    EXPECT_EQ(first.ToVector(), std::vector<std::string>({"MAIN_LIGHT_SHADOWS", "_NORMALMAP"}));
    EXPECT_EQ(first.Hash(), second.Hash());
}

TEST(ShaderLabVariantTests, VariantKeyIncludesBackendStagePassAndShaderModel)
{
    using namespace NLS::Render::ShaderLab;

    ShaderLabKeywordSet keywords;
    keywords.Enable("_ALPHATEST_ON");

    ShaderLabVariantKey dx12;
    dx12.shaderGuid = NLS::Guid::NewDeterministic("shader");
    dx12.subShaderIndex = 0;
    dx12.passIndex = 1;
    dx12.stage = NLS::Render::ShaderCompiler::ShaderStage::Pixel;
    dx12.keywordHash = keywords.Hash();
    dx12.backend = NLS::Render::RHI::NativeBackendType::DX12;
    dx12.shaderModel = ShaderLabShaderModel::SM6_6;

    auto vulkan = dx12;
    vulkan.backend = NLS::Render::RHI::NativeBackendType::Vulkan;

    EXPECT_NE(dx12.Hash(), vulkan.Hash());
    vulkan.backend = dx12.backend;
    vulkan.passIndex = 0;
    EXPECT_NE(dx12.Hash(), vulkan.Hash());
}

TEST(ShaderLabVariantTests, VariantAndArtifactKeysDoNotTruncateLargePassIndices)
{
    using namespace NLS::Render::ShaderLab;

    const uint32_t largeIndex = 70000u;
    const uint32_t truncatedIndex = largeIndex & 0xffffu;

    ShaderLabVariantKey low;
    low.shaderGuid = NLS::Guid::NewDeterministic("large-index-shader");
    low.subShaderIndex = truncatedIndex;
    low.passIndex = truncatedIndex;

    auto high = low;
    high.subShaderIndex = largeIndex;
    high.passIndex = largeIndex;
    EXPECT_NE(low.Hash(), high.Hash());

    ShaderLabArtifactKey lowArtifact;
    lowArtifact.shaderGuid = low.shaderGuid;
    lowArtifact.subShaderIndex = truncatedIndex;
    lowArtifact.passIndex = truncatedIndex;
    lowArtifact.entryPoint = "PSMain";

    auto highArtifact = lowArtifact;
    highArtifact.subShaderIndex = largeIndex;
    highArtifact.passIndex = largeIndex;
    EXPECT_NE(lowArtifact.Hash(), highArtifact.Hash());
}

TEST(ShaderLabVariantTests, ArtifactKeyInvalidatesForIncludeCompilerBackendAndArguments)
{
    using namespace NLS::Render::ShaderLab;

    ShaderLabArtifactKey base;
    base.shaderGuid = NLS::Guid::NewDeterministic("artifact-shader");
    base.subShaderIndex = 0;
    base.passIndex = 0;
    base.stage = NLS::Render::ShaderCompiler::ShaderStage::Vertex;
    base.entryPoint = "VSMain";
    base.hlslSourceHash = HashShaderLabString("source");
    base.includeDependencyHash = HashShaderLabString("include-a");
    base.keywordHash = HashShaderLabString("_NORMALMAP");
    base.backend = NLS::Render::RHI::NativeBackendType::DX12;
    base.target = NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
    base.shaderModel = ShaderLabShaderModel::SM6_6;
    base.compilerFingerprint = "dxc-1";
    base.compileArgumentsHash = HashShaderLabString("-O0");

    auto changedInclude = base;
    changedInclude.includeDependencyHash = HashShaderLabString("include-b");
    EXPECT_NE(base.Hash(), changedInclude.Hash());

    auto changedCompiler = base;
    changedCompiler.compilerFingerprint = "dxc-2";
    EXPECT_NE(base.Hash(), changedCompiler.Hash());

    auto changedBackend = base;
    changedBackend.backend = NLS::Render::RHI::NativeBackendType::Vulkan;
    changedBackend.target = NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV;
    EXPECT_NE(base.Hash(), changedBackend.Hash());

    auto changedArgs = base;
    changedArgs.compileArgumentsHash = HashShaderLabString("-Zi");
    EXPECT_NE(base.Hash(), changedArgs.Hash());
}

TEST(ShaderLabVariantTests, ArtifactKeyBuilderCopiesCompilerInputAndFingerprints)
{
    using namespace NLS::Render::ShaderLab;

    ShaderLabArtifactKeyBuildInput buildInput;
    buildInput.shaderGuid = NLS::Guid::NewDeterministic("artifact-builder-shader");
    buildInput.subShaderIndex = 1;
    buildInput.passIndex = 2;
    buildInput.backend = NLS::Render::RHI::NativeBackendType::DX12;
    buildInput.shaderModel = ShaderLabShaderModel::SM6_6;
    buildInput.hlslSourceHash = HashShaderLabString("shaderlab-hlsl");
    buildInput.includeDependencyHash = HashShaderLabString("include-graph-v1");
    buildInput.keywordHash = HashShaderLabString("_ALPHATEST_ON");
    buildInput.compilerFingerprint = "dxc.exe|1.8.2407|schema2";
    buildInput.compileArgumentsHash = HashShaderLabString("-O0 -Zi");
    buildInput.compileInput.assetPath = "Assets/Shaders/Test.shader";
    buildInput.compileInput.stage = NLS::Render::ShaderCompiler::ShaderStage::Pixel;
    buildInput.compileInput.options.entryPoint = "PSMain";
    buildInput.compileInput.options.targetPlatform = NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
    buildInput.compileInput.options.targetProfile = "ps_6_6";

    const auto base = BuildShaderLabArtifactKey(buildInput);
    EXPECT_EQ(base.stage, NLS::Render::ShaderCompiler::ShaderStage::Pixel);
    EXPECT_EQ(base.entryPoint, "PSMain");
    EXPECT_EQ(base.target, NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL);
    EXPECT_NE(base.compileArgumentsHash, 0u);
    EXPECT_NE(base.compileArgumentsHash, HashShaderLabString("-O0 -Zi"));
    EXPECT_EQ(base.compileArgumentsHash, BuildShaderLabArtifactKey(buildInput).compileArgumentsHash);

    auto changedEntry = buildInput;
    changedEntry.compileInput.options.entryPoint = "PSDepth";
    EXPECT_NE(base.Hash(), BuildShaderLabArtifactKey(changedEntry).Hash());

    auto changedTarget = buildInput;
    changedTarget.compileInput.options.targetPlatform = NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV;
    changedTarget.backend = NLS::Render::RHI::NativeBackendType::Vulkan;
    EXPECT_NE(base.Hash(), BuildShaderLabArtifactKey(changedTarget).Hash());

    auto changedShaderModel = buildInput;
    changedShaderModel.shaderModel = ShaderLabShaderModel::SM6_0;
    EXPECT_NE(base.Hash(), BuildShaderLabArtifactKey(changedShaderModel).Hash());

    auto changedCompiler = buildInput;
    changedCompiler.compilerFingerprint = "dxc.exe|1.9.0|schema2";
    EXPECT_NE(base.Hash(), BuildShaderLabArtifactKey(changedCompiler).Hash());

    auto changedDependency = buildInput;
    changedDependency.includeDependencyHash = HashShaderLabString("include-graph-v2");
    EXPECT_NE(base.Hash(), BuildShaderLabArtifactKey(changedDependency).Hash());

    auto changedArguments = buildInput;
    changedArguments.compileArgumentsHash = HashShaderLabString("-O3");
    EXPECT_NE(base.Hash(), BuildShaderLabArtifactKey(changedArguments).Hash());

    auto changedProfile = buildInput;
    changedProfile.compileInput.options.targetProfile = "ps_6_0";
    EXPECT_NE(base.Hash(), BuildShaderLabArtifactKey(changedProfile).Hash());

    auto changedDebug = buildInput;
    changedDebug.compileInput.options.enableDebugInfo = true;
    EXPECT_NE(base.Hash(), BuildShaderLabArtifactKey(changedDebug).Hash());

    auto changedMacro = buildInput;
    changedMacro.compileInput.options.macros.push_back({ "MAIN_LIGHT_SHADOWS", "1" });
    EXPECT_NE(base.Hash(), BuildShaderLabArtifactKey(changedMacro).Hash());
}

TEST(ShaderLabVariantTests, CompileInputBuilderCopiesPassStateEntryPointKeywordsAndIncludes)
{
    using namespace NLS::Render::ShaderLab;

    ShaderLabPassRuntime pass;
    pass.vertexEntry = "VSMain";
    pass.fragmentEntry = "PSMain";
    pass.computeEntry = "CSMain";
    pass.hlslLocation = { "Assets/Shaders/Test.shader", 18, 3, 512 };
    pass.shaderFeatures.push_back({ { "_ALPHATEST_ON" }, { "Assets/Shaders/Test.shader", 3, 5, 48 } });
    pass.multiCompiles.push_back({ { "_", "MAIN_LIGHT_SHADOWS" }, { "Assets/Shaders/Test.shader", 4, 5, 61 } });

    const ShaderLabKeywordSet keywords = []()
    {
        ShaderLabKeywordSet set;
        set.Enable("_ALPHATEST_ON");
        set.Enable("MAIN_LIGHT_SHADOWS");
        return set;
    }();

    const auto input = BuildShaderLabCompileInput(
        pass,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL,
        ShaderLabShaderModel::SM6_6,
        "Library/ShaderLab/Test.pass0.ps.hlsl",
        { "Assets/Shaders", "App/Assets/Engine/Shaders" },
        keywords);

    EXPECT_EQ(input.assetPath, "Library/ShaderLab/Test.pass0.ps.hlsl");
    EXPECT_EQ(input.stage, NLS::Render::ShaderCompiler::ShaderStage::Pixel);
    EXPECT_EQ(input.options.entryPoint, "PSMain");
    EXPECT_EQ(input.options.targetPlatform, NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL);
    EXPECT_EQ(input.options.targetProfile, "ps_6_6");
    EXPECT_EQ(input.options.includeDirectories.size(), 2u);
    EXPECT_EQ(input.options.includeDirectories[0], "Assets/Shaders");
    EXPECT_EQ(input.options.includeDirectories[1], "App/Assets/Engine/Shaders");
    ASSERT_EQ(input.options.macros.size(), 2u);
    EXPECT_EQ(input.options.macros[0].name, "MAIN_LIGHT_SHADOWS");
    EXPECT_EQ(input.options.macros[0].value, "1");
    EXPECT_EQ(input.options.macros[1].name, "_ALPHATEST_ON");
    EXPECT_EQ(input.options.macros[1].value, "1");
}

TEST(ShaderLabVariantTests, CompileRequestBuilderKeepsVariantInputAndArtifactKeyInSync)
{
    using namespace NLS::Render::ShaderLab;

    auto pass = std::make_shared<ShaderLabPassRuntime>();
    pass->subShaderIndex = 3;
    pass->passIndex = 4;
    pass->vertexEntry = "VSForward";
    pass->fragmentEntry = "PSForward";
    pass->hlslSource = "float4 PSForward() : SV_Target0 { return 1; }";
    pass->hlslLocation = { "Assets/Shaders/Forward.shader", 42, 1, 2048 };

    ShaderLabKeywordSet keywords;
    keywords.Enable("_NORMALMAP");
    keywords.Enable("MAIN_LIGHT_SHADOWS");

    ShaderLabCompileRequestBuildInput requestInput;
    requestInput.shaderGuid = NLS::Guid::NewDeterministic("compile-request-shader");
    requestInput.pass = pass;
    requestInput.stage = NLS::Render::ShaderCompiler::ShaderStage::Pixel;
    requestInput.backend = NLS::Render::RHI::NativeBackendType::DX12;
    requestInput.target = NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL;
    requestInput.shaderModel = ShaderLabShaderModel::SM6_6;
    requestInput.compileSourcePath = "Library/ShaderLab/Forward.pass4.ps.hlsl";
    requestInput.includeDirectories = { "Assets/Shaders", "App/Assets/Engine/Shaders" };
    requestInput.keywords = keywords;
    requestInput.hlslSourceHash = HashShaderLabString(pass->hlslSource);
    requestInput.includeDependencyHash = HashShaderLabString("include-graph");
    requestInput.compilerFingerprint = "dxc|fingerprint";
    requestInput.compileArgumentsHash = HashShaderLabString("-O3");

    const auto request = BuildShaderLabCompileRequest(requestInput);

    EXPECT_EQ(request.variantKey.shaderGuid, requestInput.shaderGuid);
    EXPECT_EQ(request.variantKey.subShaderIndex, 3u);
    EXPECT_EQ(request.variantKey.passIndex, 4u);
    EXPECT_EQ(request.variantKey.stage, NLS::Render::ShaderCompiler::ShaderStage::Pixel);
    EXPECT_EQ(request.variantKey.keywordHash, keywords.Hash());
    EXPECT_EQ(request.variantKey.backend, NLS::Render::RHI::NativeBackendType::DX12);
    EXPECT_EQ(request.compileInput.assetPath, "Library/ShaderLab/Forward.pass4.ps.hlsl");
    EXPECT_EQ(request.compileInput.options.entryPoint, "PSForward");
    EXPECT_EQ(request.sourceText, BuildShaderLabHlslForCompile(*pass));
    EXPECT_NE(request.sourceText.find("#line 42 \"Assets/Shaders/Forward.shader\""), std::string::npos);
    EXPECT_NE(request.sourceText.find(pass->hlslSource), std::string::npos);
    EXPECT_EQ(request.artifactKey.Hash(), BuildShaderLabCompileRequest(requestInput).artifactKey.Hash());

    auto changedKeywords = requestInput;
    changedKeywords.keywords.Enable("_ALPHATEST_ON");
    EXPECT_NE(request.artifactKey.Hash(), BuildShaderLabCompileRequest(changedKeywords).artifactKey.Hash());

    auto changedShaderModel = requestInput;
    changedShaderModel.shaderModel = ShaderLabShaderModel::SM6_0;
    EXPECT_NE(request.artifactKey.Hash(), BuildShaderLabCompileRequest(changedShaderModel).artifactKey.Hash());
}

TEST(ShaderLabVariantTests, KeywordsConvertToCompilerMacrosIgnoringUnderscorePlaceholder)
{
    NLS::Render::ShaderLab::ShaderLabKeywordSet keywords;
    keywords.Enable("_");
    keywords.Enable("_ALPHATEST_ON");
    keywords.Enable("MAIN_LIGHT_SHADOWS");

    const auto macros = NLS::Render::ShaderLab::BuildShaderLabKeywordMacros(keywords);

    ASSERT_EQ(macros.size(), 2u);
    EXPECT_EQ(macros[0].name, "MAIN_LIGHT_SHADOWS");
    EXPECT_EQ(macros[0].value, "1");
    EXPECT_EQ(macros[1].name, "_ALPHATEST_ON");
    EXPECT_EQ(macros[1].value, "1");
}

TEST(ShaderLabVariantTests, UnderscorePlaceholderDoesNotAffectKeywordHash)
{
    NLS::Render::ShaderLab::ShaderLabKeywordSet empty;
    NLS::Render::ShaderLab::ShaderLabKeywordSet placeholderOnly;
    placeholderOnly.Enable("_");

    EXPECT_EQ(empty.Hash(), placeholderOnly.Hash());

    empty.Enable("_ALPHATEST_ON");
    placeholderOnly.Enable("_ALPHATEST_ON");
    EXPECT_EQ(empty.Hash(), placeholderOnly.Hash())
        << "Keyword hash must describe the effective compiler macro set.";
}

TEST(ShaderLabVariantTests, ImportedShaderArtifactStoresAndSelectsMaterialKeywordVariants)
{
    using namespace NLS::Render;

    ShaderLab::ShaderLabKeywordSet alphaTest;
    alphaTest.Enable("_ALPHATEST_ON");

    Assets::ShaderArtifact artifact;
    artifact.sourcePath = "Assets/Shaders/Alpha.shader";
    artifact.subAssetKey = "shader:Alpha";
    artifact.stages.push_back({
        ShaderCompiler::ShaderStage::Pixel,
        ShaderCompiler::ShaderTargetPlatform::DXIL,
        "PSMain",
        "ps_6_0",
        {ShaderCompiler::ShaderCompilationStatus::Succeeded, {0x01u}, {}, {}, {}},
        0u
    });
    artifact.stages.push_back({
        ShaderCompiler::ShaderStage::Pixel,
        ShaderCompiler::ShaderTargetPlatform::DXIL,
        "PSMain",
        "ps_6_0",
        {ShaderCompiler::ShaderCompilationStatus::Succeeded, {0x02u}, {}, {}, {}},
        alphaTest.Hash()
    });

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_keyword_artifact_" + NLS::Guid::New().ToString());
    const auto artifactPath = root / "Library" / "Artifacts" / "Alpha" /
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    WriteBytes(artifactPath, Assets::SerializeShaderArtifact(artifact));

    auto* shader = Resources::Loaders::ShaderLoader::Create(artifactPath.string());
    ASSERT_NE(shader, nullptr);

    const auto* defaultStage = shader->FindCompiledArtifact(
        ShaderCompiler::ShaderStage::Pixel,
        ShaderCompiler::ShaderTargetPlatform::DXIL,
        0u);
    const auto* keywordStage = shader->FindCompiledArtifact(
        ShaderCompiler::ShaderStage::Pixel,
        ShaderCompiler::ShaderTargetPlatform::DXIL,
        alphaTest.Hash());
    ASSERT_NE(defaultStage, nullptr);
    ASSERT_NE(keywordStage, nullptr);
    ASSERT_EQ(defaultStage->output.bytecode.size(), 1u);
    ASSERT_EQ(keywordStage->output.bytecode.size(), 1u);
    EXPECT_EQ(defaultStage->output.bytecode[0], 0x01u);
    EXPECT_EQ(keywordStage->output.bytecode[0], 0x02u);

    EXPECT_TRUE(Resources::Loaders::ShaderLoader::Destroy(shader));
    std::filesystem::remove_all(root);
}

TEST(ShaderLabVariantTests, MissingKeywordCombinationDoesNotFallbackToDefaultArtifact)
{
    using namespace NLS::Render;

    ShaderLab::ShaderLabKeywordSet alphaTest;
    alphaTest.Enable("_ALPHATEST_ON");

    ShaderLab::ShaderLabKeywordSet alphaAndNormal;
    alphaAndNormal.Enable("_ALPHATEST_ON");
    alphaAndNormal.Enable("_NORMALMAP");

    Assets::ShaderArtifact artifact;
    artifact.sourcePath = "Assets/Shaders/Alpha.shader";
    artifact.subAssetKey = "shader:Alpha";
    artifact.stages.push_back({
        ShaderCompiler::ShaderStage::Pixel,
        ShaderCompiler::ShaderTargetPlatform::DXIL,
        "PSMain",
        "ps_6_0",
        {ShaderCompiler::ShaderCompilationStatus::Succeeded, {0x01u}, {}, {}, {}},
        0u
    });
    artifact.stages.push_back({
        ShaderCompiler::ShaderStage::Pixel,
        ShaderCompiler::ShaderTargetPlatform::DXIL,
        "PSMain",
        "ps_6_0",
        {ShaderCompiler::ShaderCompilationStatus::Succeeded, {0x02u}, {}, {}, {}},
        alphaTest.Hash()
    });

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_missing_keyword_combo_" + NLS::Guid::New().ToString());
    const auto artifactPath = root / "Library" / "Artifacts" / "Alpha" /
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    WriteBytes(artifactPath, Assets::SerializeShaderArtifact(artifact));

    auto* shader = Resources::Loaders::ShaderLoader::Create(artifactPath.string());
    ASSERT_NE(shader, nullptr);

    EXPECT_EQ(
        shader->FindCompiledArtifact(
            ShaderCompiler::ShaderStage::Pixel,
            ShaderCompiler::ShaderTargetPlatform::DXIL,
            alphaAndNormal.Hash()),
        nullptr);

    EXPECT_TRUE(Resources::Loaders::ShaderLoader::Destroy(shader));
    std::filesystem::remove_all(root);
}

TEST(ShaderLabVariantTests, ShaderHotReloadPreservesAllKeywordVariants)
{
    using namespace NLS::Render;

    ShaderLab::ShaderLabKeywordSet alphaTest;
    alphaTest.Enable("_ALPHATEST_ON");

    Assets::ShaderArtifact artifact;
    artifact.sourcePath = "Assets/Shaders/Alpha.shader";
    artifact.subAssetKey = "shader:Alpha";
    artifact.stages.push_back({
        ShaderCompiler::ShaderStage::Pixel,
        ShaderCompiler::ShaderTargetPlatform::DXIL,
        "PSMain",
        "ps_6_0",
        {ShaderCompiler::ShaderCompilationStatus::Succeeded, {0x01u}, {}, {}, {}},
        0u
    });
    artifact.stages.push_back({
        ShaderCompiler::ShaderStage::Pixel,
        ShaderCompiler::ShaderTargetPlatform::DXIL,
        "PSMain",
        "ps_6_0",
        {ShaderCompiler::ShaderCompilationStatus::Succeeded, {0x02u}, {}, {}, {}},
        alphaTest.Hash()
    });

    const auto root = std::filesystem::temp_directory_path() /
        ("nullus_shaderlab_copy_keyword_variants_" + NLS::Guid::New().ToString());
    const auto artifactPath = root / "Library" / "Artifacts" / "Alpha" /
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    Assets::ShaderArtifact defaultOnlyArtifact = artifact;
    defaultOnlyArtifact.stages.resize(1u);
    WriteBytes(artifactPath, Assets::SerializeShaderArtifact(defaultOnlyArtifact));

    auto* shader = Resources::Loaders::ShaderLoader::Create(artifactPath.string());
    ASSERT_NE(shader, nullptr);
    EXPECT_EQ(
        shader->FindCompiledArtifact(
            ShaderCompiler::ShaderStage::Pixel,
            ShaderCompiler::ShaderTargetPlatform::DXIL,
            alphaTest.Hash()),
        nullptr);

    WriteBytes(artifactPath, Assets::SerializeShaderArtifact(artifact));
    Resources::Loaders::ShaderLoader::Recompile(*shader, artifactPath.string());

    const auto* copiedKeywordStage = shader->FindCompiledArtifact(
        ShaderCompiler::ShaderStage::Pixel,
        ShaderCompiler::ShaderTargetPlatform::DXIL,
        alphaTest.Hash());
    ASSERT_NE(copiedKeywordStage, nullptr);
    ASSERT_EQ(copiedKeywordStage->output.bytecode.size(), 1u);
    EXPECT_EQ(copiedKeywordStage->output.bytecode[0], 0x02u);

    EXPECT_TRUE(Resources::Loaders::ShaderLoader::Destroy(shader));
    std::filesystem::remove_all(root);
}

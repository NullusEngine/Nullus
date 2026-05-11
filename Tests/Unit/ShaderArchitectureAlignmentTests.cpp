#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

#include "Engine/Rendering/Shaders/DeferredLightingShaders.h"
#include "Engine/Rendering/Shaders/LightGridShaders.h"
#include "Engine/Rendering/Shaders/MaterialShaders.h"
#include "Rendering/Resources/ShaderMap.h"
#include "Rendering/Resources/ShaderParameterMetadata.h"
#include "Rendering/Resources/ShaderType.h"

namespace
{
    struct ExpectedEngineShaderType
    {
        std::string_view typeName;
        std::string_view sourcePath;
        std::string_view entryPoint;
        NLS::Render::ShaderCompiler::ShaderStage stage;
        NLS::Render::Resources::ShaderTypeKind kind;
    };
}

TEST(ShaderArchitectureAlignmentTests, EngineGraphicsShadersResolveThroughShaderTypeRegistry)
{
    const std::array<ExpectedEngineShaderType, 4u> expectedTypes = {{
        { "StandardPS", "App/Assets/Engine/Shaders/Standard.hlsl", "PSMain", NLS::Render::ShaderCompiler::ShaderStage::Pixel, NLS::Render::Resources::ShaderTypeKind::Material },
        { "LambertPS", "App/Assets/Engine/Shaders/Lambert.hlsl", "PSMain", NLS::Render::ShaderCompiler::ShaderStage::Pixel, NLS::Render::Resources::ShaderTypeKind::Material },
        { "StandardPBRPS", "App/Assets/Engine/Shaders/StandardPBR.hlsl", "PSMain", NLS::Render::ShaderCompiler::ShaderStage::Pixel, NLS::Render::Resources::ShaderTypeKind::Material },
        { "DeferredLightingPS", "App/Assets/Engine/Shaders/DeferredLighting.hlsl", "PSMain", NLS::Render::ShaderCompiler::ShaderStage::Pixel, NLS::Render::Resources::ShaderTypeKind::Global }
    }};

    const auto& registry = NLS::Render::Resources::GetShaderTypeRegistry();
    for (const auto& expected : expectedTypes)
    {
        const auto* shaderType = registry.FindByName(expected.typeName);
        ASSERT_NE(shaderType, nullptr) << expected.typeName;
        EXPECT_EQ(shaderType->GetName(), expected.typeName);
        EXPECT_EQ(shaderType->GetSourcePath(), expected.sourcePath);
        EXPECT_EQ(shaderType->GetEntryPoint(), expected.entryPoint);
        EXPECT_EQ(shaderType->GetStage(), expected.stage);
        EXPECT_EQ(shaderType->GetKind(), expected.kind);
        EXPECT_FALSE(shaderType->GetRootParameterStructs().empty());
        EXPECT_TRUE(shaderType->ShouldCompilePermutation({}));
    }
}

TEST(ShaderArchitectureAlignmentTests, EngineShaderTypesCanBeResolvedBySourcePath)
{
    const auto& registry = NLS::Render::Resources::GetShaderTypeRegistry();

    const auto standardTypes = registry.FindBySourcePath("App\\Assets\\Engine\\Shaders\\Standard.hlsl");

    ASSERT_FALSE(standardTypes.empty());
    EXPECT_NE(
        std::find_if(
            standardTypes.begin(),
            standardTypes.end(),
            [](const NLS::Render::Resources::ShaderType* shaderType)
            {
                return shaderType != nullptr &&
                    shaderType->GetName() == "StandardPS" &&
                    shaderType->GetStage() == NLS::Render::ShaderCompiler::ShaderStage::Pixel;
            }),
        standardTypes.end());
}

TEST(ShaderArchitectureAlignmentTests, CustomShaderPathDoesNotResolveAsEngineShaderType)
{
    const auto& registry = NLS::Render::Resources::GetShaderTypeRegistry();

    EXPECT_TRUE(registry.FindBySourcePath("App/Assets/Project/Shaders/CustomUserShader.hlsl").empty());
    EXPECT_EQ(registry.FindByName("CustomUserShaderPS"), nullptr);
}

TEST(ShaderArchitectureAlignmentTests, MigratedShaderTypesExposeRootParameterMetadata)
{
    const auto& registry = NLS::Render::Resources::GetShaderTypeRegistry();
    const auto* standard = registry.FindByName("StandardPS");

    ASSERT_NE(standard, nullptr);
    const auto* metadata = standard->GetRootParameterMetadata();

    ASSERT_NE(metadata, nullptr);
    EXPECT_EQ(metadata->debugName, "StandardPSRootParameters");
    EXPECT_FALSE(metadata->groups.empty());
    EXPECT_EQ(metadata->ToParameterStructs().size(), standard->GetRootParameterStructs().size());
}

TEST(ShaderArchitectureAlignmentTests, GlobalShaderClassesRegisterLightGridAndDeferredLightingTypes)
{
    using namespace NLS::Render::Engine::Shaders;

    EXPECT_EQ(LightGridResetCS::GetStaticShaderType().GetName(), "LightGridResetCS");
    EXPECT_EQ(LightGridInjectionCS::GetStaticShaderType().GetName(), "LightGridInjectionCS");
    EXPECT_EQ(LightGridCompactCS::GetStaticShaderType().GetName(), "LightGridCompactCS");
    EXPECT_EQ(DeferredLightingPS::GetStaticShaderType().GetName(), "DeferredLightingPS");

    const auto& registry = NLS::Render::Resources::GetShaderTypeRegistry();
    EXPECT_EQ(registry.FindByName("LightGridResetCS"), &LightGridResetCS::GetStaticShaderType());
    EXPECT_EQ(registry.FindByName("LightGridInjectionCS"), &LightGridInjectionCS::GetStaticShaderType());
    EXPECT_EQ(registry.FindByName("LightGridCompactCS"), &LightGridCompactCS::GetStaticShaderType());
    EXPECT_EQ(registry.FindByName("DeferredLightingPS"), &DeferredLightingPS::GetStaticShaderType());
}

TEST(ShaderArchitectureAlignmentTests, MaterialShaderClassesRegisterStandardLambertAndDeferredGBufferTypes)
{
    using namespace NLS::Render::Engine::Shaders;

    EXPECT_EQ(StandardVS::GetStaticShaderType().GetName(), "StandardVS");
    EXPECT_EQ(StandardPS::GetStaticShaderType().GetName(), "StandardPS");
    EXPECT_EQ(LambertVS::GetStaticShaderType().GetName(), "LambertVS");
    EXPECT_EQ(LambertPS::GetStaticShaderType().GetName(), "LambertPS");
    EXPECT_EQ(StandardPBRVS::GetStaticShaderType().GetName(), "StandardPBRVS");
    EXPECT_EQ(StandardPBRPS::GetStaticShaderType().GetName(), "StandardPBRPS");
    EXPECT_EQ(DeferredGBufferVS::GetStaticShaderType().GetName(), "DeferredGBufferVS");
    EXPECT_EQ(DeferredGBufferPS::GetStaticShaderType().GetName(), "DeferredGBufferPS");
}

TEST(ShaderArchitectureAlignmentTests, ShaderMapsResolveEngineShaderRefsByTypeAndPermutation)
{
    NLS::Render::Resources::ShaderMap shaderMap;

    shaderMap.RegisterCompiledShader(
        &NLS::Render::Engine::Shaders::StandardPS::GetStaticShaderType(),
        NLS::Render::Resources::ShaderPermutationId{},
        nullptr);

    NLS::Render::Resources::ShaderMapRef<NLS::Render::Engine::Shaders::StandardPS> shaderRef(shaderMap);

    EXPECT_TRUE(shaderRef.IsValid());
    ASSERT_NE(shaderRef.GetShaderType(), nullptr);
    EXPECT_EQ(shaderRef.GetShaderType()->GetName(), "StandardPS");
}

TEST(ShaderArchitectureAlignmentTests, ShaderLoaderDoesNotOwnEngineShaderFilenameSwitches)
{
    const std::filesystem::path loaderPath =
        std::filesystem::path(NLS_ROOT_DIR) / "Runtime/Rendering/Resources/Loaders/ShaderLoader.cpp";
    std::ifstream stream(loaderPath, std::ios::binary);
    ASSERT_TRUE(stream) << loaderPath.string();

    std::ostringstream sourceBuffer;
    sourceBuffer << stream.rdbuf();
    const std::string loaderSourceText = sourceBuffer.str();

    EXPECT_TRUE((loaderSourceText.find("BuildEngineGraphicsShaderParameterStructs") == std::string::npos));
    EXPECT_TRUE((loaderSourceText.find("Standard.hlsl") == std::string::npos));
    EXPECT_TRUE((loaderSourceText.find("Lambert.hlsl") == std::string::npos));
    EXPECT_TRUE((loaderSourceText.find("StandardPBR.hlsl") == std::string::npos));
    EXPECT_TRUE((loaderSourceText.find("DeferredLighting.hlsl") == std::string::npos));
    EXPECT_TRUE((loaderSourceText.find("DeferredGBuffer.hlsl") == std::string::npos));
}

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "Engine/Rendering/Shaders/DeferredLightingShaders.h"
#include "Engine/Rendering/Shaders/HZBShaders.h"
#include "Rendering/SceneOcclusion.h"
#include "Engine/Rendering/Shaders/LightGridShaders.h"
#include "Engine/Rendering/Shaders/MaterialShaders.h"
#include "Rendering/Data/SceneOcclusionPacketLayout.h"
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
    EXPECT_EQ(HZBBuildCS::GetStaticShaderType().GetName(), "HZBBuildCS");
    EXPECT_EQ(HZBOcclusionCS::GetStaticShaderType().GetName(), "HZBOcclusionCS");
    EXPECT_EQ(DeferredLightingPS::GetStaticShaderType().GetName(), "DeferredLightingPS");

    const auto& registry = NLS::Render::Resources::GetShaderTypeRegistry();
    EXPECT_EQ(registry.FindByName("LightGridResetCS"), &LightGridResetCS::GetStaticShaderType());
    EXPECT_EQ(registry.FindByName("LightGridInjectionCS"), &LightGridInjectionCS::GetStaticShaderType());
    EXPECT_EQ(registry.FindByName("LightGridCompactCS"), &LightGridCompactCS::GetStaticShaderType());
    EXPECT_EQ(registry.FindByName("HZBBuildCS"), &HZBBuildCS::GetStaticShaderType());
	EXPECT_EQ(registry.FindByName("HZBOcclusionCS"), &HZBOcclusionCS::GetStaticShaderType());
	EXPECT_EQ(registry.FindByName("DeferredLightingPS"), &DeferredLightingPS::GetStaticShaderType());
}

TEST(ShaderArchitectureAlignmentTests, HZBShaderTypeLookupReportsMissingRegistryEntries)
{
	NLS::Render::Resources::ShaderTypeRegistry emptyRegistry;

	try
	{
		(void)NLS::Render::Engine::Shaders::ResolveRequiredHZBShaderType(emptyRegistry, "HZBBuildCS");
		FAIL() << "missing HZB shader type should throw";
	}
	catch (const std::runtime_error& error)
	{
		const std::string message = error.what();
		EXPECT_NE(message.find("Required HZB shader type is not registered"), std::string::npos);
		EXPECT_NE(message.find("HZBBuildCS"), std::string::npos);
	}
}

TEST(ShaderArchitectureAlignmentTests, HZBOcclusionShaderConsumesPrimitiveInputsAndWritesPrimitiveResults)
{
	const std::filesystem::path shaderPath =
		std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/HZBOcclusion.hlsl";
	std::ifstream stream(shaderPath, std::ios::binary);
	ASSERT_TRUE(stream) << shaderPath.string();

	std::ostringstream sourceBuffer;
	sourceBuffer << stream.rdbuf();
	const std::string shaderSourceText = sourceBuffer.str();

	EXPECT_NE(shaderSourceText.find("struct OcclusionPrimitiveInput"), std::string::npos);
	EXPECT_NE(shaderSourceText.find("StructuredBuffer<OcclusionPrimitiveInput> u_OcclusionPrimitiveInputs"), std::string::npos);
	EXPECT_NE(shaderSourceText.find("RWStructuredBuffer<uint> u_OcclusionPrimitiveResults"), std::string::npos);
	EXPECT_EQ(shaderSourceText.find("u_OcclusionOutput"), std::string::npos);
	EXPECT_NE(shaderSourceText.find("u_OcclusionPrimitiveResults[primitiveIndex]"), std::string::npos);
	EXPECT_NE(shaderSourceText.find("kHZBOcclusionDepthBias"), std::string::npos);
	EXPECT_NE(shaderSourceText.find("static const float kHZBOcclusionDepthBias = 0.00001f"), std::string::npos);
	EXPECT_NE(shaderSourceText.find("IsConservativelyOccludedByHZBCoverage"), std::string::npos);
	EXPECT_NE(shaderSourceText.find("kHZBOcclusionCoverageGridDimension"), std::string::npos);
	EXPECT_EQ(shaderSourceText.find("kHZBOcclusionGridDimension"), std::string::npos);
	EXPECT_EQ(shaderSourceText.find("float2(0.5f, 0.5f)"), std::string::npos);
	EXPECT_EQ(shaderSourceText.find("allCornersOcclude"), std::string::npos);
	EXPECT_NE(shaderSourceText.find("nearestOccluderDepth = primitive.nearestDepth - kHZBOcclusionDepthBias"), std::string::npos);
	EXPECT_EQ(shaderSourceText.find("hzbDepth <= primitive.nearestDepth"), std::string::npos);
	EXPECT_EQ(shaderSourceText.find("hzbDepth + kHZBOcclusionDepthBias < primitive.nearestDepth"), std::string::npos);
}

TEST(ShaderArchitectureAlignmentTests, HZBOcclusionPrimitiveInputMemberOrderMatchesCppPacketLayout)
{
	const std::filesystem::path shaderPath =
		std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/HZBOcclusion.hlsl";
	std::ifstream stream(shaderPath, std::ios::binary);
	ASSERT_TRUE(stream) << shaderPath.string();

	std::ostringstream sourceBuffer;
	sourceBuffer << stream.rdbuf();
	const std::string shaderSourceText = sourceBuffer.str();

	const auto structStart = shaderSourceText.find("struct OcclusionPrimitiveInput");
	ASSERT_NE(structStart, std::string::npos);
	const auto bodyStart = shaderSourceText.find('{', structStart);
	const auto bodyEnd = shaderSourceText.find("};", bodyStart);
	ASSERT_NE(bodyStart, std::string::npos);
	ASSERT_NE(bodyEnd, std::string::npos);
	const auto structBody = shaderSourceText.substr(bodyStart, bodyEnd - bodyStart);

	const std::array<std::string_view, 4u> expectedMembers = {
		"float2 screenMin;",
		"float2 screenMax;",
		"float nearestDepth;",
		"uint flags;"
	};
	size_t previousOffset = 0u;
	for (const auto expectedMember : expectedMembers)
	{
		const auto offset = structBody.find(expectedMember);
		ASSERT_NE(offset, std::string::npos) << expectedMember;
		EXPECT_GE(offset, previousOffset) << expectedMember;
		previousOffset = offset;
	}
}

TEST(ShaderArchitectureAlignmentTests, HZBOcclusionShaderSamplesLargeFootprintsInsteadOfForcingVisibleFallback)
{
	const std::filesystem::path hzbBuildPath =
		std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/HZBBuild.hlsl";
	std::ifstream hzbBuildStream(hzbBuildPath, std::ios::binary);
	ASSERT_TRUE(hzbBuildStream) << hzbBuildPath.string();

	std::ostringstream hzbBuildBuffer;
	hzbBuildBuffer << hzbBuildStream.rdbuf();
	const std::string hzbBuildSource = hzbBuildBuffer.str();
	ASSERT_NE(hzbBuildSource.find("Texture2D<float> u_HZBPreviousMip"), std::string::npos);
	ASSERT_NE(hzbBuildSource.find("RWTexture2D<float> u_HZBOutputMip"), std::string::npos);
	ASSERT_NE(hzbBuildSource.find("u_HZBPreviousMip.GetDimensions"), std::string::npos);
	ASSERT_NE(hzbBuildSource.find("copyPreviousMip = previousWidth == outputWidth && previousHeight == outputHeight"), std::string::npos);
	ASSERT_NE(hzbBuildSource.find("copyPreviousMip ? dispatchThreadId.xy : dispatchThreadId.xy * 2u"), std::string::npos);
	ASSERT_NE(hzbBuildSource.find("max(max(depth00, depth10), max(depth01, depth11))"), std::string::npos);
	ASSERT_EQ(hzbBuildSource.find("min(min(depth00, depth10), min(depth01, depth11))"), std::string::npos);
	ASSERT_EQ(hzbBuildSource.find("RWTexture2D<float> u_HZBOutput :"), std::string::npos);
	ASSERT_EQ(hzbBuildSource.find("RWTexture2DArray"), std::string::npos);

	const std::filesystem::path occlusionPath =
		std::filesystem::path(NLS_ROOT_DIR) / "App/Assets/Engine/Shaders/HZBOcclusion.hlsl";
	std::ifstream occlusionStream(occlusionPath, std::ios::binary);
	ASSERT_TRUE(occlusionStream) << occlusionPath.string();

	std::ostringstream occlusionBuffer;
	occlusionBuffer << occlusionStream.rdbuf();
	const std::string occlusionSource = occlusionBuffer.str();

	EXPECT_EQ(occlusionSource.find("kHZBOcclusionMaxMip0ScanPixels"), std::string::npos);
	EXPECT_NE(occlusionSource.find("kHZBOcclusionCoverageGridDimension"), std::string::npos);
	EXPECT_NE(occlusionSource.find("IsConservativelyOccludedByHZBCoverage"), std::string::npos);
	EXPECT_NE(occlusionSource.find("SelectHZBMipLevel"), std::string::npos);
	EXPECT_NE(occlusionSource.find("u_HZB.GetDimensions(0u, width, height, mipCount)"), std::string::npos);
	EXPECT_NE(occlusionSource.find("u_HZB.Load(int3(pixel, mipLevel))"), std::string::npos);
	EXPECT_NE(occlusionSource.find("const uint stepX = max(1u, mipFootprintWidth / kHZBOcclusionCoverageGridDimension)"), std::string::npos);
	EXPECT_NE(occlusionSource.find("const uint stepY = max(1u, mipFootprintHeight / kHZBOcclusionCoverageGridDimension)"), std::string::npos);
	EXPECT_NE(occlusionSource.find("for (uint pixelY = mipMinPixelY; pixelY <= mipMaxPixelY; pixelY += stepY)"), std::string::npos);
	EXPECT_NE(occlusionSource.find("for (uint pixelX = mipMinPixelX; pixelX <= mipMaxPixelX; pixelX += stepX)"), std::string::npos);
	EXPECT_EQ(occlusionSource.find("IsConservativelyOccludedByCoarseHZBCoverage"), std::string::npos);
	EXPECT_EQ(occlusionSource.find(": false"), std::string::npos);
	EXPECT_NE(occlusionSource.find("u_OcclusionPrimitiveResults[primitiveIndex] = occluded ? 1u : 0u"), std::string::npos);
	EXPECT_EQ(occlusionSource.find("kHZBOcclusionGridDimension"), std::string::npos);
	EXPECT_NE(occlusionSource.find("[numthreads(8, 1, 1)]"), std::string::npos);
	EXPECT_EQ(occlusionSource.find("dispatchThreadId.y"), std::string::npos);
}

TEST(ShaderArchitectureAlignmentTests, HZBOcclusionPrimitivePacketLayoutOffsetsAreSingleSourceOfTruth)
{
	using NLS::Engine::Rendering::SceneOcclusionPrimitivePacket;
	using namespace NLS::Render::Data;

	EXPECT_EQ(kSceneOcclusionPrimitivePacketScreenMinXOffset, offsetof(SceneOcclusionPrimitivePacket, screenMinX));
	EXPECT_EQ(kSceneOcclusionPrimitivePacketScreenMinYOffset, offsetof(SceneOcclusionPrimitivePacket, screenMinY));
	EXPECT_EQ(kSceneOcclusionPrimitivePacketScreenMaxXOffset, offsetof(SceneOcclusionPrimitivePacket, screenMaxX));
	EXPECT_EQ(kSceneOcclusionPrimitivePacketScreenMaxYOffset, offsetof(SceneOcclusionPrimitivePacket, screenMaxY));
	EXPECT_EQ(kSceneOcclusionPrimitivePacketNearestDepthOffset, offsetof(SceneOcclusionPrimitivePacket, nearestDepth));
	EXPECT_EQ(kSceneOcclusionPrimitivePacketFlagsOffset, offsetof(SceneOcclusionPrimitivePacket, flags));
	EXPECT_EQ(kSceneOcclusionPrimitivePacketStride, sizeof(SceneOcclusionPrimitivePacket));
	EXPECT_LT(kSceneOcclusionPrimitivePacketScreenMinXOffset, kSceneOcclusionPrimitivePacketScreenMinYOffset);
	EXPECT_LT(kSceneOcclusionPrimitivePacketScreenMinYOffset, kSceneOcclusionPrimitivePacketScreenMaxXOffset);
	EXPECT_LT(kSceneOcclusionPrimitivePacketScreenMaxXOffset, kSceneOcclusionPrimitivePacketScreenMaxYOffset);
	EXPECT_LT(kSceneOcclusionPrimitivePacketScreenMaxYOffset, kSceneOcclusionPrimitivePacketNearestDepthOffset);
	EXPECT_LT(kSceneOcclusionPrimitivePacketNearestDepthOffset, kSceneOcclusionPrimitivePacketFlagsOffset);
}

TEST(ShaderArchitectureAlignmentTests, HZBOcclusionMetadataMatchesPrimitiveBufferShaderContract)
{
	using namespace NLS::Render::Engine::Shaders;

	const auto& parameters = HZBOcclusionCS::GetStaticShaderType().GetRootParameterStructs().front();
	auto findMember = [&parameters](const std::string_view name)
	{
		return std::find_if(
			parameters.members.begin(),
			parameters.members.end(),
			[name](const NLS::Render::Resources::ShaderParameterMember& member)
			{
				return member.name == name;
			});
	};

	const auto input = findMember("u_OcclusionPrimitiveInputs");
	ASSERT_NE(input, parameters.members.end());
	EXPECT_EQ(input->type, NLS::Render::RHI::BindingType::StructuredBuffer);
	EXPECT_EQ(input->binding, 2u);
	EXPECT_EQ(input->stageMask, NLS::Render::RHI::ShaderStageMask::Compute);
	EXPECT_EQ(input->elementStride, NLS::Render::Data::kSceneOcclusionPrimitivePacketStride);

	const auto staleTextureOutput = findMember("u_OcclusionOutput");
	EXPECT_EQ(staleTextureOutput, parameters.members.end());

	const auto result = findMember("u_OcclusionPrimitiveResults");
	ASSERT_NE(result, parameters.members.end());
	EXPECT_EQ(result->type, NLS::Render::RHI::BindingType::StorageBuffer);
	EXPECT_EQ(result->binding, 3u);
	EXPECT_EQ(result->stageMask, NLS::Render::RHI::ShaderStageMask::Compute);
	EXPECT_EQ(result->elementStride, sizeof(uint32_t));
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

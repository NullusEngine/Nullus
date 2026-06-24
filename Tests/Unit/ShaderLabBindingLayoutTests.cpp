#include <gtest/gtest.h>

#include "Rendering/ShaderLab/ShaderLabBindingLayout.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "Rendering/ShaderLab/ShaderLabParser.h"

namespace
{
std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}
}

TEST(ShaderLabBindingLayoutTests, BuildsMaterialLayoutFromReflectionOffsetsWithoutGuessing)
{
    NLS::Render::Resources::ShaderReflection reflection;

    NLS::Render::Resources::ShaderConstantBufferDesc materialCBuffer;
    materialCBuffer.name = "MaterialProperties";
    materialCBuffer.stage = NLS::Render::ShaderCompiler::ShaderStage::Vertex;
    materialCBuffer.stageMask = NLS::Render::RHI::ShaderStageMask::Vertex;
    materialCBuffer.bindingSpace = 2;
    materialCBuffer.bindingIndex = 3;
    materialCBuffer.byteSize = 96;
    materialCBuffer.members.push_back({
        "_BaseColor",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
        32,
        16,
        1
    });
    materialCBuffer.members.push_back({
        "_Matrices",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT_MAT4,
        48,
        64,
        1
    });
    reflection.constantBuffers.push_back(std::move(materialCBuffer));

    NLS::Render::Resources::ShaderConstantBufferDesc pixelMaterialCBuffer;
    pixelMaterialCBuffer.name = "MaterialProperties";
    pixelMaterialCBuffer.stage = NLS::Render::ShaderCompiler::ShaderStage::Pixel;
    pixelMaterialCBuffer.stageMask = NLS::Render::RHI::ShaderStageMask::Fragment;
    pixelMaterialCBuffer.bindingSpace = 2;
    pixelMaterialCBuffer.bindingIndex = 3;
    pixelMaterialCBuffer.byteSize = 96;
    pixelMaterialCBuffer.members.push_back({
        "_BaseColor",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
        32,
        16,
        1
    });
    reflection.constantBuffers.push_back(std::move(pixelMaterialCBuffer));

    reflection.properties.push_back({
        "_BaseMap",
        NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
        NLS::Render::Resources::ShaderResourceKind::SampledTexture,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        2,
        5,
        -1,
        1,
        0u,
        0u,
        {},
        NLS::Render::RHI::ShaderStageMask::Fragment
    });
    reflection.properties.push_back({
        "_BaseMap",
        NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
        NLS::Render::Resources::ShaderResourceKind::SampledTexture,
        NLS::Render::ShaderCompiler::ShaderStage::Vertex,
        2,
        5,
        -1,
        1,
        0u,
        0u,
        {},
        NLS::Render::RHI::ShaderStageMask::Vertex
    });
    reflection.properties.push_back({
        "sampler_BaseMap",
        NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
        NLS::Render::Resources::ShaderResourceKind::Sampler,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        2,
        6,
        -1,
        1,
        0u,
        0u,
        {},
        NLS::Render::RHI::ShaderStageMask::Fragment
    });

    const auto layout = NLS::Render::ShaderLab::BuildShaderLabMaterialBindingLayout(
        reflection,
        NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace);

    EXPECT_EQ(layout.constantBufferSize, 96u);
    ASSERT_EQ(layout.constantBuffers.size(), 1u);
    EXPECT_EQ(layout.constantBuffers[0].bindingSpace, 2u);
    EXPECT_EQ(layout.constantBuffers[0].bindingIndex, 3u);
    EXPECT_TRUE(NLS::Render::RHI::HasShaderStage(
        layout.constantBuffers[0].stageMask,
        NLS::Render::RHI::ShaderStageMask::Vertex));
    EXPECT_TRUE(NLS::Render::RHI::HasShaderStage(
        layout.constantBuffers[0].stageMask,
        NLS::Render::RHI::ShaderStageMask::Fragment));
    ASSERT_EQ(layout.properties.size(), 2u);
    EXPECT_EQ(layout.properties[0].name, "_BaseColor");
    EXPECT_EQ(layout.properties[0].byteOffset, 32u)
        << "ShaderLab must trust compiler reflection offsets instead of deriving cbuffer packing.";
    EXPECT_EQ(layout.properties[1].byteSize, 64u);

    ASSERT_EQ(layout.textures.size(), 1u);
    EXPECT_EQ(layout.textures[0].name, "_BaseMap");
    EXPECT_EQ(layout.textures[0].bindingSpace, 2u);
    EXPECT_EQ(layout.textures[0].bindingIndex, 5u);
    EXPECT_TRUE(NLS::Render::RHI::HasShaderStage(
        layout.textures[0].stageMask,
        NLS::Render::RHI::ShaderStageMask::Vertex));
    EXPECT_TRUE(NLS::Render::RHI::HasShaderStage(
        layout.textures[0].stageMask,
        NLS::Render::RHI::ShaderStageMask::Fragment));

    ASSERT_EQ(layout.samplers.size(), 1u);
    EXPECT_EQ(layout.samplers[0].name, "sampler_BaseMap");
    EXPECT_EQ(layout.samplers[0].bindingIndex, 6u);
    EXPECT_EQ(layout.samplers[0].stageMask, NLS::Render::RHI::ShaderStageMask::Fragment);
}

TEST(ShaderLabBindingLayoutTests, DefaultMaterialBindingSpaceMatchesRhiBindingPointMap)
{
    NLS::Render::Resources::ShaderReflection reflection;
    NLS::Render::Resources::ShaderConstantBufferDesc materialCBuffer;
    materialCBuffer.name = "MaterialProperties";
    materialCBuffer.bindingSpace = NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace;
    materialCBuffer.byteSize = 16;
    materialCBuffer.members.push_back({
        "_Tint",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
        0,
        16,
        1
    });
    reflection.constantBuffers.push_back(std::move(materialCBuffer));

    const auto layout = NLS::Render::ShaderLab::BuildShaderLabMaterialBindingLayout(reflection);

    ASSERT_EQ(layout.properties.size(), 1u);
    EXPECT_EQ(layout.properties[0].name, "_Tint");
}

TEST(ShaderLabBindingLayoutTests, KeepsMaterialConstantBuffersSeparateWhenBindingsDiffer)
{
    NLS::Render::Resources::ShaderReflection reflection;

    NLS::Render::Resources::ShaderConstantBufferDesc vertexBuffer;
    vertexBuffer.name = "MaterialProperties";
    vertexBuffer.stage = NLS::Render::ShaderCompiler::ShaderStage::Vertex;
    vertexBuffer.stageMask = NLS::Render::RHI::ShaderStageMask::Vertex;
    vertexBuffer.bindingSpace = NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace;
    vertexBuffer.bindingIndex = 2;
    vertexBuffer.byteSize = 16;
    vertexBuffer.members.push_back({
        "_ObjectTint",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
        0,
        16,
        1
    });
    reflection.constantBuffers.push_back(std::move(vertexBuffer));

    NLS::Render::Resources::ShaderConstantBufferDesc pixelBuffer;
    pixelBuffer.name = "MaterialProperties";
    pixelBuffer.stage = NLS::Render::ShaderCompiler::ShaderStage::Pixel;
    pixelBuffer.stageMask = NLS::Render::RHI::ShaderStageMask::Fragment;
    pixelBuffer.bindingSpace = NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace;
    pixelBuffer.bindingIndex = 5;
    pixelBuffer.byteSize = 16;
    pixelBuffer.members.push_back({
        "_ObjectTint",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
        0,
        16,
        1
    });
    reflection.constantBuffers.push_back(std::move(pixelBuffer));

    const auto layout = NLS::Render::ShaderLab::BuildShaderLabMaterialBindingLayout(reflection);

    ASSERT_EQ(layout.constantBuffers.size(), 2u);
    EXPECT_EQ(layout.constantBuffers[0].bindingIndex, 2u);
    EXPECT_EQ(layout.constantBuffers[0].stageMask, NLS::Render::RHI::ShaderStageMask::Vertex);
    EXPECT_EQ(layout.constantBuffers[1].bindingIndex, 5u);
    EXPECT_EQ(layout.constantBuffers[1].stageMask, NLS::Render::RHI::ShaderStageMask::Fragment);
}

TEST(ShaderLabBindingLayoutTests, BuiltInStandardPbrDeclaresMaterialResourcesInMaterialSpace)
{
    const auto source = ReadTextFile(
        std::filesystem::path(NLS_ROOT_DIR) /
        "App" / "Assets" / "Engine" / "Shaders" / "ShaderLab" / "StandardPBR.shader");

    ASSERT_FALSE(source.empty());
    const std::string textureNames[] = {
        "_BaseMap",
        "_MetallicMap",
        "_RoughnessMap",
        "_OcclusionMap",
        "_NormalMap",
        "_OpacityMap",
        "_EmissiveMap",
        "_SpecularMap"
    };
    for (uint32_t index = 0u; index < std::size(textureNames); ++index)
    {
        EXPECT_NE(
            source.find("Texture2D " + textureNames[index] + " : register(t" + std::to_string(index) + ", space2);"),
            std::string::npos)
            << textureNames[index];
        EXPECT_NE(
            source.find("SamplerState sampler" + textureNames[index] + " : register(s" + std::to_string(index) + ", space2);"),
            std::string::npos)
            << textureNames[index];
    }
    EXPECT_NE(source.find("cbuffer MaterialProperties : register(b0, space2)"), std::string::npos);

    const auto parsed = NLS::Render::ShaderLab::ParseShaderLabSource(source, "StandardPBR.shader");
    ASSERT_TRUE(parsed.Succeeded()) << parsed.DiagnosticsToString();
    ASSERT_FALSE(parsed.asset.subShaders.empty());
    ASSERT_FALSE(parsed.asset.subShaders.front().passes.empty());
    const auto& forward = parsed.asset.subShaders.front().passes.front();
    const auto compileSource = NLS::Render::ShaderLab::BuildShaderLabHlslForCompile(forward);
    for (const auto& textureName : textureNames)
        EXPECT_NE(compileSource.find(textureName), std::string::npos) << textureName;
}

#include <gtest/gtest.h>

#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/Resources/ShaderReflectionMerge.h"

TEST(ShaderReflectionMergeTests, MergesConstantBufferMembersAcrossStagesWithoutDroppingOffsets)
{
    NLS::Render::Resources::ShaderReflection destination;
    NLS::Render::Resources::ShaderConstantBufferDesc vertexBuffer;
    vertexBuffer.name = "MaterialProperties";
    vertexBuffer.stage = NLS::Render::ShaderCompiler::ShaderStage::Vertex;
    vertexBuffer.stageMask = NLS::Render::RHI::ShaderStageMask::Vertex;
    vertexBuffer.bindingSpace = 2u;
    vertexBuffer.bindingIndex = 0u;
    vertexBuffer.byteSize = 16u;
    vertexBuffer.members.push_back({
        "_ObjectTint",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
        0u,
        16u,
        1u
    });
    destination.constantBuffers.push_back(std::move(vertexBuffer));

    NLS::Render::Resources::ShaderReflection source;
    NLS::Render::Resources::ShaderConstantBufferDesc pixelBuffer;
    pixelBuffer.name = "MaterialProperties";
    pixelBuffer.stage = NLS::Render::ShaderCompiler::ShaderStage::Pixel;
    pixelBuffer.stageMask = NLS::Render::RHI::ShaderStageMask::Fragment;
    pixelBuffer.bindingSpace = 2u;
    pixelBuffer.bindingIndex = 0u;
    pixelBuffer.byteSize = 32u;
    pixelBuffer.members.push_back({
        "_BaseColor",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
        16u,
        16u,
        1u
    });
    source.constantBuffers.push_back(std::move(pixelBuffer));

    NLS::Render::Resources::MergeShaderReflection(destination, source);

    ASSERT_EQ(destination.constantBuffers.size(), 1u);
    EXPECT_EQ(destination.constantBuffers[0].byteSize, 32u);
    EXPECT_TRUE(NLS::Render::RHI::HasShaderStage(
        destination.constantBuffers[0].stageMask,
        NLS::Render::RHI::ShaderStageMask::Vertex));
    EXPECT_TRUE(NLS::Render::RHI::HasShaderStage(
        destination.constantBuffers[0].stageMask,
        NLS::Render::RHI::ShaderStageMask::Fragment));
    ASSERT_EQ(destination.constantBuffers[0].members.size(), 2u);
    EXPECT_EQ(destination.constantBuffers[0].members[0].name, "_ObjectTint");
    EXPECT_EQ(destination.constantBuffers[0].members[0].byteOffset, 0u);
    EXPECT_EQ(destination.constantBuffers[0].members[1].name, "_BaseColor");
    EXPECT_EQ(destination.constantBuffers[0].members[1].byteOffset, 16u);
}

TEST(ShaderReflectionMergeTests, RejectsSameConstantBufferMemberWithDifferentLayout)
{
    NLS::Render::Resources::ShaderReflection destination;
    NLS::Render::Resources::ShaderConstantBufferDesc vertexBuffer;
    vertexBuffer.name = "MaterialProperties";
    vertexBuffer.stage = NLS::Render::ShaderCompiler::ShaderStage::Vertex;
    vertexBuffer.bindingSpace = 2u;
    vertexBuffer.bindingIndex = 0u;
    vertexBuffer.byteSize = 16u;
    vertexBuffer.members.push_back({
        "_BaseColor",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
        0u,
        16u,
        1u
    });
    destination.constantBuffers.push_back(std::move(vertexBuffer));

    NLS::Render::Resources::ShaderReflection source;
    NLS::Render::Resources::ShaderConstantBufferDesc pixelBuffer;
    pixelBuffer.name = "MaterialProperties";
    pixelBuffer.stage = NLS::Render::ShaderCompiler::ShaderStage::Pixel;
    pixelBuffer.bindingSpace = 2u;
    pixelBuffer.bindingIndex = 0u;
    pixelBuffer.byteSize = 32u;
    pixelBuffer.members.push_back({
        "_BaseColor",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
        16u,
        16u,
        1u
    });
    source.constantBuffers.push_back(std::move(pixelBuffer));

    std::string diagnostic;
    EXPECT_FALSE(NLS::Render::Resources::TryMergeShaderReflection(destination, source, &diagnostic));
    EXPECT_NE(diagnostic.find("_BaseColor"), std::string::npos);
    ASSERT_EQ(destination.constantBuffers.size(), 1u);
    ASSERT_EQ(destination.constantBuffers[0].members.size(), 1u);
    EXPECT_EQ(destination.constantBuffers[0].members[0].byteOffset, 0u);
}

TEST(ShaderReflectionMergeTests, RejectsConstantBufferMembersOutsideDeclaredSize)
{
    NLS::Render::Resources::ShaderReflection destination;

    NLS::Render::Resources::ShaderReflection source;
    NLS::Render::Resources::ShaderConstantBufferDesc buffer;
    buffer.name = "MaterialProperties";
    buffer.bindingSpace = 2u;
    buffer.bindingIndex = 0u;
    buffer.byteSize = 16u;
    buffer.members.push_back({
        "_TooLarge",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
        8u,
        16u,
        1u
    });
    source.constantBuffers.push_back(std::move(buffer));

    std::string diagnostic;
    EXPECT_FALSE(NLS::Render::Resources::TryMergeShaderReflection(destination, source, &diagnostic));
    EXPECT_NE(diagnostic.find("_TooLarge"), std::string::npos);
    EXPECT_TRUE(destination.constantBuffers.empty());
}

TEST(ShaderReflectionMergeTests, KeepsPreferredReflectionWhenItHasResourcesButNoConstantBuffers)
{
    NLS::Render::Resources::ShaderReflection preferred;
    preferred.properties.push_back({
        "_PreferredTexture",
        NLS::Render::Resources::UniformType::UNIFORM_SAMPLER_2D,
        NLS::Render::Resources::ShaderResourceKind::SampledTexture,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        2u,
        4u,
        -1,
        1,
        0u,
        0u,
        {}
    });

    NLS::Render::Resources::ShaderReflection fallback;
    fallback.constantBuffers.push_back({
        "MaterialProperties",
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        2u,
        0u,
        16u,
        {
            {"_BaseColor", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, 0u, 16u, 1u}
        }
    });
    fallback.properties.push_back({
        "_BaseColor",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
        NLS::Render::Resources::ShaderResourceKind::Value,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        2u,
        0u,
        -1,
        1,
        0u,
        16u,
        "MaterialProperties"
    });

    NLS::Render::Resources::ShaderReflection result;
    std::string diagnostic;
    const std::vector<NLS::Render::Resources::ShaderReflection> preferredStages{preferred};
    const std::vector<NLS::Render::Resources::ShaderReflection> fallbackStages{fallback};
    ASSERT_TRUE(NLS::Render::Resources::TryMergePreferredShaderReflectionOrFallback(
        preferredStages,
        fallbackStages,
        result,
        &diagnostic));

    EXPECT_TRUE(result.constantBuffers.empty());
    ASSERT_EQ(result.properties.size(), 1u);
    EXPECT_EQ(result.properties[0].name, "_PreferredTexture");
}

TEST(ShaderReflectionMergeTests, UsesFallbackReflectionFromCleanStateWhenPreferredIsEmpty)
{
    const std::vector<NLS::Render::Resources::ShaderReflection> preferredStages{
        NLS::Render::Resources::ShaderReflection{}
    };

    NLS::Render::Resources::ShaderReflection fallback;
    fallback.constantBuffers.push_back({
        "MaterialProperties",
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        2u,
        0u,
        16u,
        {
            {"_BaseColor", NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4, 0u, 16u, 1u}
        }
    });
    fallback.properties.push_back({
        "_BaseColor",
        NLS::Render::Resources::UniformType::UNIFORM_FLOAT_VEC4,
        NLS::Render::Resources::ShaderResourceKind::Value,
        NLS::Render::ShaderCompiler::ShaderStage::Pixel,
        2u,
        0u,
        -1,
        1,
        0u,
        16u,
        "MaterialProperties"
    });

    NLS::Render::Resources::ShaderReflection result;
    std::string diagnostic;
    const std::vector<NLS::Render::Resources::ShaderReflection> fallbackStages{fallback};
    ASSERT_TRUE(NLS::Render::Resources::TryMergePreferredShaderReflectionOrFallback(
        preferredStages,
        fallbackStages,
        result,
        &diagnostic));

    ASSERT_EQ(result.constantBuffers.size(), 1u);
    EXPECT_EQ(result.constantBuffers[0].name, "MaterialProperties");
    ASSERT_EQ(result.properties.size(), 1u);
    EXPECT_EQ(result.properties[0].name, "_BaseColor");
}

#include <gtest/gtest.h>

#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/Resources/ShaderBindingLayoutUtils.h"
#include "Rendering/Resources/ShaderReflection.h"

namespace
{
    using NLS::Render::Resources::ShaderConstantBufferDesc;
    using NLS::Render::Resources::ShaderPropertyDesc;
    using NLS::Render::Resources::ShaderReflection;
    using NLS::Render::Resources::ShaderResourceKind;
    using NLS::Render::Resources::UniformType;
    using NLS::Render::ShaderCompiler::ShaderStage;

    ShaderReflection BuildGridLikeReflection()
    {
        ShaderReflection reflection;
        reflection.constantBuffers = {
            { "FrameConstants", ShaderStage::Vertex, NLS::Render::RHI::BindingPointMap::kFrameBindingSpace, 0u, 64u, {} },
            { "ObjectConstants", ShaderStage::Vertex, NLS::Render::RHI::BindingPointMap::kObjectBindingSpace, 0u, 64u, {} },
            { "MaterialConstants", ShaderStage::Pixel, NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace, 0u, 16u, {} }
        };
        reflection.properties = {
            { "u_Color", UniformType::UNIFORM_FLOAT_VEC3, ShaderResourceKind::Value, ShaderStage::Pixel, NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace, 0u, -1, 1, 0u, 12u, "MaterialConstants" }
        };
        return reflection;
    }

    ShaderReflection BuildPassAndMaterialReflection()
    {
        ShaderReflection reflection;
        reflection.constantBuffers = {
            { "PassConstants", ShaderStage::Pixel, NLS::Render::RHI::BindingPointMap::kPassBindingSpace, 0u, 64u, {} }
        };
        reflection.properties = {
            { "u_GBufferAlbedo", UniformType::UNIFORM_SAMPLER_2D, ShaderResourceKind::SampledTexture, ShaderStage::Pixel, NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace, 0u, -1, 1, 0u, 0u, {} },
            { "u_LinearWrapSampler", UniformType::UNIFORM_SAMPLER_2D, ShaderResourceKind::Sampler, ShaderStage::Pixel, NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace, 0u, -1, 1, 0u, 0u, {} }
        };
        return reflection;
    }
}

TEST(ShaderBindingLayoutUtilsTests, BuildsDenseDescriptorSetLayoutsForFrameMaterialAndObjectBindings)
{
    const auto layouts = NLS::Render::Resources::BuildExplicitBindingLayoutDescsBySet(
        BuildGridLikeReflection(),
        "Grid");

    ASSERT_EQ(layouts.size(), 3u);
    EXPECT_EQ(layouts[0].entries.size(), 1u);
    EXPECT_EQ(layouts[0].entries[0].name, "FrameConstants");
    EXPECT_EQ(layouts[1].entries.size(), 1u);
    EXPECT_EQ(layouts[1].entries[0].name, "MaterialConstants");
    EXPECT_EQ(layouts[1].entries[0].set, NLS::Render::RHI::BindingPointMap::kMaterialDescriptorSet);
    EXPECT_EQ(layouts[1].entries[0].registerSpace, NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace);
    EXPECT_EQ(layouts[2].entries.size(), 1u);
    EXPECT_EQ(layouts[2].entries[0].name, "ObjectConstants");
    EXPECT_EQ(layouts[2].entries[0].set, NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet);
    EXPECT_EQ(layouts[2].entries[0].registerSpace, NLS::Render::RHI::BindingPointMap::kObjectBindingSpace);
}

TEST(ShaderBindingLayoutUtilsTests, PreservesEmptyDescriptorSetSlotsNeededByHigherSetIndices)
{
    const auto layouts = NLS::Render::Resources::BuildExplicitBindingLayoutDescsBySet(
        BuildPassAndMaterialReflection(),
        "DeferredLighting");

    ASSERT_EQ(layouts.size(), 4u);
    EXPECT_TRUE(layouts[0].entries.empty());
    ASSERT_EQ(layouts[1].entries.size(), 2u);
    EXPECT_EQ(layouts[1].entries[0].name, "u_GBufferAlbedo");
    EXPECT_EQ(layouts[1].entries[0].set, NLS::Render::RHI::BindingPointMap::kMaterialDescriptorSet);
    EXPECT_EQ(layouts[1].entries[0].registerSpace, NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace);
    EXPECT_EQ(layouts[1].entries[1].name, "u_LinearWrapSampler");
    EXPECT_EQ(layouts[1].entries[1].set, NLS::Render::RHI::BindingPointMap::kMaterialDescriptorSet);
    EXPECT_EQ(layouts[1].entries[1].registerSpace, NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace);
    EXPECT_TRUE(layouts[2].entries.empty());
    ASSERT_EQ(layouts[3].entries.size(), 1u);
    EXPECT_EQ(layouts[3].entries[0].name, "PassConstants");
    EXPECT_EQ(layouts[3].entries[0].set, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet);
    EXPECT_EQ(layouts[3].entries[0].registerSpace, NLS::Render::RHI::BindingPointMap::kPassBindingSpace);
}

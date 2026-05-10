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

TEST(ShaderBindingLayoutUtilsTests, ValidatesConflictingReflectionBindingsBeforeLayoutCreation)
{
    ShaderReflection reflection;
    reflection.properties = {
        { "u_Texture", UniformType::UNIFORM_SAMPLER_2D, ShaderResourceKind::SampledTexture, ShaderStage::Pixel, NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace, 0u, -1, 1, 0u, 0u, {} },
        { "u_OtherTexture", UniformType::UNIFORM_SAMPLER_2D, ShaderResourceKind::SampledTexture, ShaderStage::Pixel, NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace, 0u, -1, 1, 0u, 0u, {} },
        { "u_InvalidArray", UniformType::UNIFORM_SAMPLER_2D, ShaderResourceKind::SampledTexture, ShaderStage::Pixel, NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace, 1u, -1, 0, 0u, 0u, {} }
    };

    const auto validation = NLS::Render::Resources::ValidateShaderBindingReflection(reflection);

    EXPECT_TRUE(validation.HasErrors());
    ASSERT_EQ(validation.diagnostics.size(), 2u);
    EXPECT_EQ(validation.diagnostics[0].severity, NLS::Render::Resources::ShaderBindingValidationSeverity::Error);
    EXPECT_NE(validation.diagnostics[0].message.find("conflict"), std::string::npos);
    EXPECT_NE(validation.diagnostics[0].message.find("space2"), std::string::npos);
    EXPECT_NE(validation.diagnostics[1].message.find("arraySize"), std::string::npos);
}

TEST(ShaderBindingLayoutUtilsTests, UE427ShaderParameterGroupsPreserveFrameMaterialObjectPassOrder)
{
    ShaderReflection reflection;
    reflection.constantBuffers = {
        { "PassConstants", ShaderStage::Pixel, NLS::Render::RHI::BindingPointMap::kPassBindingSpace, 0u, 64u, {} },
        { "ObjectConstants", ShaderStage::Vertex, NLS::Render::RHI::BindingPointMap::kObjectBindingSpace, 0u, 64u, {} },
        { "FrameConstants", ShaderStage::Vertex, NLS::Render::RHI::BindingPointMap::kFrameBindingSpace, 0u, 64u, {} }
    };
    reflection.properties = {
        { "u_BaseColor", UniformType::UNIFORM_SAMPLER_2D, ShaderResourceKind::SampledTexture, ShaderStage::Pixel, NLS::Render::RHI::BindingPointMap::kMaterialBindingSpace, 0u, -1, 1, 0u, 0u, {} }
    };

    const auto groups = NLS::Render::Resources::BuildShaderParameterGroupContracts(
        reflection,
        "LitMesh");

    ASSERT_EQ(groups.size(), 4u);
    EXPECT_EQ(groups[0].groupKind, NLS::Render::Resources::ShaderParameterGroupKind::Frame);
    EXPECT_EQ(groups[0].descriptorSet, NLS::Render::RHI::BindingPointMap::kFrameDescriptorSet);
    ASSERT_EQ(groups[0].parameters.size(), 1u);
    EXPECT_EQ(groups[0].parameters[0].name, "FrameConstants");

    EXPECT_EQ(groups[1].groupKind, NLS::Render::Resources::ShaderParameterGroupKind::Material);
    EXPECT_EQ(groups[1].descriptorSet, NLS::Render::RHI::BindingPointMap::kMaterialDescriptorSet);
    ASSERT_EQ(groups[1].parameters.size(), 1u);
    EXPECT_EQ(groups[1].parameters[0].name, "u_BaseColor");

    EXPECT_EQ(groups[2].groupKind, NLS::Render::Resources::ShaderParameterGroupKind::Object);
    EXPECT_EQ(groups[2].descriptorSet, NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet);
    ASSERT_EQ(groups[2].parameters.size(), 1u);
    EXPECT_EQ(groups[2].parameters[0].name, "ObjectConstants");

    EXPECT_EQ(groups[3].groupKind, NLS::Render::Resources::ShaderParameterGroupKind::Pass);
    EXPECT_EQ(groups[3].descriptorSet, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet);
    ASSERT_EQ(groups[3].parameters.size(), 1u);
    EXPECT_EQ(groups[3].parameters[0].name, "PassConstants");
}

TEST(ShaderBindingLayoutUtilsTests, UE427ShaderParameterGroupValidationReportsMissingAndStalePassBindings)
{
    ShaderReflection reflection;
    reflection.constantBuffers = {
        { "PassConstants", ShaderStage::Pixel, NLS::Render::RHI::BindingPointMap::kPassBindingSpace, 0u, 64u, {} }
    };
    reflection.properties = {
        { "u_PassTexture", UniformType::UNIFORM_SAMPLER_2D, ShaderResourceKind::SampledTexture, ShaderStage::Pixel, NLS::Render::RHI::BindingPointMap::kPassBindingSpace, 1u, -1, 1, 0u, 0u, {} }
    };

    const auto groups = NLS::Render::Resources::BuildShaderParameterGroupContracts(
        reflection,
        "DeferredLighting");

    const std::vector<NLS::Render::Resources::ShaderParameterBindingResourceState> resources = {
        {
            "PassConstants",
            NLS::Render::Resources::ShaderParameterGroupKind::Pass,
            NLS::Render::RHI::BindingPointMap::kPassDescriptorSet,
            NLS::Render::RHI::BindingType::UniformBuffer,
            0u,
            false,
            7u,
            7u
        },
        {
            "u_PassTexture",
            NLS::Render::Resources::ShaderParameterGroupKind::Pass,
            NLS::Render::RHI::BindingPointMap::kPassDescriptorSet,
            NLS::Render::RHI::BindingType::Texture,
            1u,
            true,
            4u,
            7u
        }
    };

    const auto validation = NLS::Render::Resources::ValidateShaderParameterGroupResources(groups, resources);

    EXPECT_TRUE(validation.HasErrors());
    ASSERT_EQ(validation.diagnostics.size(), 2u);
    EXPECT_NE(validation.diagnostics[0].message.find("missing"), std::string::npos);
    EXPECT_NE(validation.diagnostics[0].message.find("PassConstants"), std::string::npos);
    EXPECT_NE(validation.diagnostics[1].message.find("stale"), std::string::npos);
    EXPECT_NE(validation.diagnostics[1].message.find("u_PassTexture"), std::string::npos);
}

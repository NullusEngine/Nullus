#include <gtest/gtest.h>

#include <algorithm>

#include "Rendering/RHI/BindingPointMap.h"
#include "Math/Matrix4.h"
#include "Rendering/Resources/ComputeShaderUtils.h"
#include "Rendering/Resources/ShaderBindingLayoutUtils.h"
#include "Rendering/Resources/ShaderParameterStruct.h"
#include "Rendering/Resources/ShaderReflection.h"

namespace
{
    using NLS::Render::Resources::ShaderConstantBufferDesc;
    using NLS::Render::Resources::ShaderPropertyDesc;
    using NLS::Render::Resources::ShaderReflection;
    using NLS::Render::Resources::ShaderResourceKind;
    using NLS::Render::Resources::UniformType;
    using NLS::Render::ShaderCompiler::ShaderStage;

    class ContractBuffer final : public NLS::Render::RHI::RHIBuffer
    {
    public:
        explicit ContractBuffer(NLS::Render::RHI::RHIBufferDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override
        {
            return m_desc.debugName.empty() ? std::string_view("ContractBuffer") : std::string_view(m_desc.debugName);
        }

        const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override
        {
            return m_desc;
        }

        NLS::Render::RHI::ResourceState GetState() const override
        {
            return NLS::Render::RHI::ResourceState::Unknown;
        }

        uint64_t GetGPUAddress() const override
        {
            return 0u;
        }

    private:
        NLS::Render::RHI::RHIBufferDesc m_desc;
    };

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

TEST(ShaderBindingLayoutUtilsTests, RendererOwnedObjectIndexConstantsAreSkippedForExplicitDescriptorLayouts)
{
    ShaderReflection reflection;
    reflection.constantBuffers = {
        { "FrameConstants", ShaderStage::Vertex, NLS::Render::RHI::BindingPointMap::kFrameBindingSpace, 0u, 64u, {} },
        { "ObjectIndexConstants", ShaderStage::Vertex, NLS::Render::RHI::BindingPointMap::kObjectBindingSpace, 1u, 16u, {} }
    };
    reflection.properties = {
        { "ObjectData", UniformType::UNIFORM_FLOAT_MAT4, ShaderResourceKind::StructuredBuffer, ShaderStage::Vertex, NLS::Render::RHI::BindingPointMap::kObjectBindingSpace, 0u, -1, 1, 0u, sizeof(NLS::Maths::Matrix4), {} }
    };

    const auto layouts = NLS::Render::Resources::BuildExplicitBindingLayoutDescsBySet(
        reflection,
        "IndexedShader");

    ASSERT_EQ(layouts.size(), 3u);
    ASSERT_EQ(layouts[0].entries.size(), 1u);
    EXPECT_EQ(layouts[0].entries[0].name, "FrameConstants");
    ASSERT_EQ(layouts[2].entries.size(), 1u);
    EXPECT_EQ(layouts[2].entries[0].name, "ObjectData");
    EXPECT_EQ(layouts[2].entries[0].binding, 0u);
    EXPECT_NE(layouts[2].entries[0].name, "ObjectIndexConstants");
}

TEST(ShaderBindingLayoutUtilsTests, ReflectionStructuredBuffersCarryElementStrideFromByteSize)
{
    ShaderReflection reflection;
    reflection.properties = {
        { "BoneMatrices", UniformType::UNIFORM_FLOAT_MAT4, ShaderResourceKind::StructuredBuffer, ShaderStage::Vertex, NLS::Render::RHI::BindingPointMap::kObjectBindingSpace, 4u, -1, 1, 0u, sizeof(NLS::Maths::Matrix4), {} }
    };

    const auto layouts = NLS::Render::Resources::BuildExplicitBindingLayoutDescsBySet(
        reflection,
        "SkinnedShader");

    ASSERT_EQ(layouts.size(), 3u);
    ASSERT_EQ(layouts[2].entries.size(), 1u);
    EXPECT_EQ(layouts[2].entries[0].name, "BoneMatrices");
    EXPECT_EQ(layouts[2].entries[0].type, NLS::Render::RHI::BindingType::StructuredBuffer);
    EXPECT_EQ(layouts[2].entries[0].elementStride, sizeof(NLS::Maths::Matrix4));
}

TEST(ShaderBindingLayoutUtilsTests, RendererOwnedObjectIndexConstantsAreSkippedForParameterGroupContracts)
{
    ShaderReflection reflection;
    reflection.constantBuffers = {
        { "ObjectIndexConstants", ShaderStage::Vertex, NLS::Render::RHI::BindingPointMap::kObjectBindingSpace, 1u, sizeof(uint32_t), {} }
    };
    reflection.properties = {
        { "ObjectData", UniformType::UNIFORM_FLOAT_MAT4, ShaderResourceKind::StructuredBuffer, ShaderStage::Vertex, NLS::Render::RHI::BindingPointMap::kObjectBindingSpace, 0u, -1, 1, 0u, sizeof(NLS::Maths::Matrix4), {} }
    };

    const auto groups = NLS::Render::Resources::BuildShaderParameterGroupContracts(
        reflection,
        "IndexedShader");

    ASSERT_EQ(groups.size(), 4u);
    const auto objectGroup = std::find_if(
        groups.begin(),
        groups.end(),
        [](const NLS::Render::Resources::ShaderParameterGroupContract& group)
        {
            return group.groupKind == NLS::Render::Resources::ShaderParameterGroupKind::Object;
        });
    ASSERT_NE(objectGroup, groups.end());
    ASSERT_EQ(objectGroup->parameters.size(), 1u);
    EXPECT_EQ(objectGroup->parameters[0].name, "ObjectData");
    EXPECT_EQ(objectGroup->parameters[0].binding, 0u);
}

TEST(ShaderBindingLayoutUtilsTests, RendererOwnedObjectIndexConstantsAreSkippedForShaderParameterStructLayouts)
{
    const std::vector<NLS::Render::Resources::ShaderParameterStruct> parameterStructs = {
        NLS::Render::Resources::ShaderParameterStructBuilder("IndexedObjectParameters")
            .SetGroup(NLS::Render::Resources::ShaderParameterGroupKind::Object)
            .AddStructuredBuffer(
                "ObjectData",
                0u,
                NLS::Render::RHI::ShaderStageMask::Vertex,
                sizeof(NLS::Maths::Matrix4))
            .AddUniformBuffer(
                "ObjectIndexConstants",
                1u,
                sizeof(uint32_t),
                NLS::Render::RHI::ShaderStageMask::Vertex)
            .Build()
    };

    const auto layouts = NLS::Render::Resources::BuildBindingLayoutDescsFromShaderParameters(
        parameterStructs,
        "SelectionOutlineMask");

    ASSERT_EQ(layouts.size(), 3u);
    ASSERT_EQ(layouts[2].entries.size(), 1u);
    EXPECT_EQ(layouts[2].entries[0].name, "ObjectData");
    EXPECT_EQ(layouts[2].entries[0].binding, 0u);
    EXPECT_EQ(layouts[2].entries[0].type, NLS::Render::RHI::BindingType::StructuredBuffer);
}

TEST(ShaderBindingLayoutUtilsTests, ReflectionParameterGroupContractsCarryStructuredBufferElementStride)
{
    ShaderReflection reflection;
    reflection.properties = {
        { "BoneMatrices", UniformType::UNIFORM_FLOAT_MAT4, ShaderResourceKind::StructuredBuffer, ShaderStage::Vertex, NLS::Render::RHI::BindingPointMap::kObjectBindingSpace, 4u, -1, 1, 0u, sizeof(NLS::Maths::Matrix4), {} }
    };

    const auto groups = NLS::Render::Resources::BuildShaderParameterGroupContracts(
        reflection,
        "SkinnedShader");

    const auto objectGroup = std::find_if(
        groups.begin(),
        groups.end(),
        [](const NLS::Render::Resources::ShaderParameterGroupContract& group)
        {
            return group.groupKind == NLS::Render::Resources::ShaderParameterGroupKind::Object;
        });
    ASSERT_NE(objectGroup, groups.end());
    ASSERT_EQ(objectGroup->parameters.size(), 1u);
    EXPECT_EQ(objectGroup->parameters[0].name, "BoneMatrices");
    EXPECT_EQ(objectGroup->parameters[0].type, NLS::Render::RHI::BindingType::StructuredBuffer);
    EXPECT_EQ(objectGroup->parameters[0].elementStride, sizeof(NLS::Maths::Matrix4));
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

TEST(ShaderBindingLayoutUtilsTests, ShaderParameterStructBuildsPassLayoutAndBindingSetLikeGlobalShaderParameters)
{
    using namespace NLS::Render::Resources;

    const auto parameters = ShaderParameterStructBuilder("LightGridInjectionParameters")
        .SetGroup(ShaderParameterGroupKind::Pass)
        .AddUniformBuffer("Forward", 0u, sizeof(uint32_t) * 16u, NLS::Render::RHI::ShaderStageMask::Compute)
        .AddStructuredBuffer("ForwardLocalLightBuffer", 0u, NLS::Render::RHI::ShaderStageMask::Compute)
        .AddStorageBuffer("RWStartOffsetGrid", 1u, NLS::Render::RHI::ShaderStageMask::Compute)
        .AddStorageBuffer("RWCulledLightLinks", 2u, NLS::Render::RHI::ShaderStageMask::Compute)
        .Build();

    const auto layoutDesc = BuildBindingLayoutDescFromShaderParameters(parameters);

    ASSERT_EQ(layoutDesc.debugName, "LightGridInjectionParametersBindingLayout");
    ASSERT_EQ(layoutDesc.entries.size(), 4u);
    EXPECT_EQ(layoutDesc.entries[0].name, "Forward");
    EXPECT_EQ(layoutDesc.entries[0].type, NLS::Render::RHI::BindingType::UniformBuffer);
    EXPECT_EQ(layoutDesc.entries[0].set, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet);
    EXPECT_EQ(layoutDesc.entries[0].registerSpace, NLS::Render::RHI::BindingPointMap::kPassBindingSpace);
    EXPECT_EQ(layoutDesc.entries[1].name, "ForwardLocalLightBuffer");
    EXPECT_EQ(layoutDesc.entries[1].binding, 0u);
    EXPECT_EQ(layoutDesc.entries[2].name, "RWStartOffsetGrid");
    EXPECT_EQ(layoutDesc.entries[2].binding, 1u);
    EXPECT_EQ(layoutDesc.entries[3].name, "RWCulledLightLinks");
    EXPECT_EQ(layoutDesc.entries[3].binding, 2u);

    auto constants = std::make_shared<ContractBuffer>(NLS::Render::RHI::RHIBufferDesc{});
    auto lightBuffer = std::make_shared<ContractBuffer>(NLS::Render::RHI::RHIBufferDesc{});
    auto startOffsetGrid = std::make_shared<ContractBuffer>(NLS::Render::RHI::RHIBufferDesc{});
    auto culledLightLinks = std::make_shared<ContractBuffer>(NLS::Render::RHI::RHIBufferDesc{});

    const auto bindingSetDesc = BuildBindingSetDescFromShaderParameters(
        parameters,
        nullptr,
        {
            ShaderParameterBindingValue::UniformBuffer("Forward", constants, sizeof(uint32_t) * 16u),
            ShaderParameterBindingValue::StructuredBuffer("ForwardLocalLightBuffer", lightBuffer, 64u),
            ShaderParameterBindingValue::StorageBuffer("RWStartOffsetGrid", startOffsetGrid, 128u),
            ShaderParameterBindingValue::StorageBuffer("RWCulledLightLinks", culledLightLinks, 256u)
        },
        "LightGridInjectionBindingSet");

    EXPECT_EQ(bindingSetDesc.debugName, "LightGridInjectionBindingSet");
    ASSERT_EQ(bindingSetDesc.entries.size(), 4u);
    EXPECT_EQ(bindingSetDesc.entries[0].binding, 0u);
    EXPECT_EQ(bindingSetDesc.entries[0].type, NLS::Render::RHI::BindingType::UniformBuffer);
    EXPECT_EQ(bindingSetDesc.entries[1].binding, 0u);
    EXPECT_EQ(bindingSetDesc.entries[1].type, NLS::Render::RHI::BindingType::StructuredBuffer);
    EXPECT_EQ(bindingSetDesc.entries[2].binding, 1u);
    EXPECT_EQ(bindingSetDesc.entries[2].type, NLS::Render::RHI::BindingType::StorageBuffer);
}

TEST(ShaderBindingLayoutUtilsTests, ShaderParameterStructCarriesStructuredBufferElementStride)
{
    using namespace NLS::Render::Resources;

    const auto parameters = ShaderParameterStructBuilder("ObjectParameters")
        .SetGroup(ShaderParameterGroupKind::Object)
        .AddStructuredBuffer("ObjectData", 0u, NLS::Render::RHI::ShaderStageMask::Vertex, sizeof(NLS::Maths::Matrix4))
        .Build();

    const auto layoutDesc = BuildBindingLayoutDescFromShaderParameters(parameters);

    ASSERT_EQ(layoutDesc.entries.size(), 1u);
    EXPECT_EQ(layoutDesc.entries[0].type, NLS::Render::RHI::BindingType::StructuredBuffer);
    EXPECT_EQ(layoutDesc.entries[0].elementStride, sizeof(NLS::Maths::Matrix4));

    auto objectBuffer = std::make_shared<ContractBuffer>(NLS::Render::RHI::RHIBufferDesc{});
    const auto bindingSetDesc = BuildBindingSetDescFromShaderParameters(
        parameters,
        nullptr,
        {
            ShaderParameterBindingValue::StructuredBuffer(
                "ObjectData",
                objectBuffer,
                sizeof(NLS::Maths::Matrix4) * 4u,
                0u,
                sizeof(NLS::Maths::Matrix4))
        },
        "ObjectBindingSet");

    ASSERT_EQ(bindingSetDesc.entries.size(), 1u);
    EXPECT_EQ(bindingSetDesc.entries[0].elementStride, sizeof(NLS::Maths::Matrix4));
}

TEST(ShaderBindingLayoutUtilsTests, ShaderParameterStructCarriesStorageBufferElementStride)
{
    using namespace NLS::Render::Resources;

    const auto parameters = ShaderParameterStructBuilder("StorageParameters")
        .SetGroup(ShaderParameterGroupKind::Pass)
        .AddStorageBuffer("RWFloat4Data", 2u, NLS::Render::RHI::ShaderStageMask::Compute, sizeof(float) * 4u)
        .Build();

    const auto layoutDesc = BuildBindingLayoutDescFromShaderParameters(parameters);

    ASSERT_EQ(layoutDesc.entries.size(), 1u);
    EXPECT_EQ(layoutDesc.entries[0].type, NLS::Render::RHI::BindingType::StorageBuffer);
    EXPECT_EQ(layoutDesc.entries[0].elementStride, sizeof(float) * 4u);

    auto storageBuffer = std::make_shared<ContractBuffer>(NLS::Render::RHI::RHIBufferDesc{});
    const auto bindingSetDesc = BuildBindingSetDescFromShaderParameters(
        parameters,
        nullptr,
        {
            ShaderParameterBindingValue::StorageBuffer(
                "RWFloat4Data",
                storageBuffer,
                sizeof(float) * 4u * 8u,
                0u,
                sizeof(float) * 4u)
        },
        "StorageBindingSet");

    ASSERT_EQ(bindingSetDesc.entries.size(), 1u);
    EXPECT_EQ(bindingSetDesc.entries[0].type, NLS::Render::RHI::BindingType::StorageBuffer);
    EXPECT_EQ(bindingSetDesc.entries[0].elementStride, sizeof(float) * 4u);
}

TEST(ShaderBindingLayoutUtilsTests, ShaderParameterStructsBuildDenseGraphicsPipelineLayouts)
{
    using namespace NLS::Render::Resources;

    const std::vector<ShaderParameterStruct> parameters = {
        ShaderParameterStructBuilder("StandardFrameParameters")
            .SetGroup(ShaderParameterGroupKind::Frame)
            .AddUniformBuffer("FrameConstants", 0u, 144u, NLS::Render::RHI::ShaderStageMask::AllGraphics)
            .Build(),
        ShaderParameterStructBuilder("StandardMaterialParameters")
            .SetGroup(ShaderParameterGroupKind::Material)
            .AddUniformBuffer("MaterialConstants", 0u, 64u, NLS::Render::RHI::ShaderStageMask::Fragment)
            .AddTexture("u_DiffuseMap", 0u, NLS::Render::RHI::ShaderStageMask::Fragment)
            .AddSampler("u_LinearWrapSampler", 0u, NLS::Render::RHI::ShaderStageMask::Fragment)
            .Build(),
        ShaderParameterStructBuilder("StandardObjectParameters")
            .SetGroup(ShaderParameterGroupKind::Object)
            .AddStructuredBuffer("ObjectData", 0u, NLS::Render::RHI::ShaderStageMask::Vertex, sizeof(NLS::Maths::Matrix4))
            .Build(),
        ShaderParameterStructBuilder("StandardPassParameters")
            .SetGroup(ShaderParameterGroupKind::Pass)
            .AddUniformBuffer("ForwardLightData", 0u, 352u, NLS::Render::RHI::ShaderStageMask::Fragment)
            .AddStructuredBuffer("u_ForwardLocalLightBuffer", 0u, NLS::Render::RHI::ShaderStageMask::Fragment)
            .AddStructuredBuffer("u_NumCulledLightsGrid", 1u, NLS::Render::RHI::ShaderStageMask::Fragment)
            .AddStructuredBuffer("u_CulledLightDataGrid", 2u, NLS::Render::RHI::ShaderStageMask::Fragment)
            .Build()
    };

    const auto layouts = BuildBindingLayoutDescsFromShaderParameters(parameters, "Standard");

    ASSERT_EQ(layouts.size(), 4u);
    ASSERT_EQ(layouts[0].entries.size(), 1u);
    EXPECT_EQ(layouts[0].entries[0].name, "FrameConstants");
    EXPECT_EQ(layouts[0].entries[0].set, NLS::Render::RHI::BindingPointMap::kFrameDescriptorSet);
    EXPECT_EQ(layouts[0].entries[0].registerSpace, NLS::Render::RHI::BindingPointMap::kFrameBindingSpace);

    ASSERT_EQ(layouts[1].entries.size(), 3u);
    EXPECT_EQ(layouts[1].entries[0].name, "MaterialConstants");
    EXPECT_EQ(layouts[1].entries[1].name, "u_DiffuseMap");
    EXPECT_EQ(layouts[1].entries[1].type, NLS::Render::RHI::BindingType::Texture);
    EXPECT_EQ(layouts[1].entries[2].name, "u_LinearWrapSampler");
    EXPECT_EQ(layouts[1].entries[2].type, NLS::Render::RHI::BindingType::Sampler);

    ASSERT_EQ(layouts[2].entries.size(), 1u);
    EXPECT_EQ(layouts[2].entries[0].name, "ObjectData");
    EXPECT_EQ(layouts[2].entries[0].type, NLS::Render::RHI::BindingType::StructuredBuffer);
    EXPECT_EQ(layouts[2].entries[0].elementStride, sizeof(NLS::Maths::Matrix4));
    EXPECT_EQ(layouts[2].entries[0].set, NLS::Render::RHI::BindingPointMap::kObjectDescriptorSet);

    ASSERT_EQ(layouts[3].entries.size(), 4u);
    EXPECT_EQ(layouts[3].entries[0].name, "ForwardLightData");
    EXPECT_EQ(layouts[3].entries[0].type, NLS::Render::RHI::BindingType::UniformBuffer);
    EXPECT_EQ(layouts[3].entries[0].binding, 0u);
    EXPECT_EQ(layouts[3].entries[1].name, "u_ForwardLocalLightBuffer");
    EXPECT_EQ(layouts[3].entries[2].name, "u_NumCulledLightsGrid");
    EXPECT_EQ(layouts[3].entries[3].name, "u_CulledLightDataGrid");
    EXPECT_EQ(layouts[3].entries[3].set, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet);
}

TEST(ShaderBindingLayoutUtilsTests, ComputeShaderUtilsBuildsRecordedDispatchFromGlobalShaderParameterStruct)
{
    using namespace NLS::Render::Resources;

    GlobalShader lightGridInjectionShader;
    lightGridInjectionShader.debugName = "LightGridInjectionCS";
    lightGridInjectionShader.stage = NLS::Render::ShaderCompiler::ShaderStage::Compute;
    lightGridInjectionShader.parameters = ShaderParameterStructBuilder("LightGridInjectionParameters")
        .SetGroup(ShaderParameterGroupKind::Pass)
        .AddUniformBuffer("Forward", 0u, sizeof(uint32_t) * 16u, NLS::Render::RHI::ShaderStageMask::Compute)
        .AddStorageBuffer("RWStartOffsetGrid", 1u, NLS::Render::RHI::ShaderStageMask::Compute)
        .Build();

    const auto dispatchInput = ComputeShaderUtils::BuildRecordedDispatch(
        lightGridInjectionShader,
        nullptr,
        nullptr,
        { 4u, 5u, 6u },
        "LightGridInjection");

    EXPECT_EQ(dispatchInput.debugName, "LightGridInjection");
    ASSERT_EQ(dispatchInput.bindingSets.size(), 1u);
    EXPECT_EQ(dispatchInput.bindingSets[0].setIndex, NLS::Render::RHI::BindingPointMap::kPassDescriptorSet);
    EXPECT_EQ(dispatchInput.groupCountX, 4u);
    EXPECT_EQ(dispatchInput.groupCountY, 5u);
    EXPECT_EQ(dispatchInput.groupCountZ, 6u);
}

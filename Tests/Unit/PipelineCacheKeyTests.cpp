#include <gtest/gtest.h>

#include <memory>
#include <string_view>
#include <vector>

#include "Rendering/RHI/Utils/PipelineCache/PipelineCache.h"
#include "Rendering/Resources/Shader.h"

namespace
{
    class TestBindingLayout final : public NLS::Render::RHI::RHIBindingLayout
    {
    public:
        explicit TestBindingLayout(NLS::Render::RHI::RHIBindingLayoutDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIBindingLayoutDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIBindingLayoutDesc m_desc;
    };

    class TestPipelineLayout final : public NLS::Render::RHI::RHIPipelineLayout
    {
    public:
        explicit TestPipelineLayout(NLS::Render::RHI::RHIPipelineLayoutDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIPipelineLayoutDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIPipelineLayoutDesc m_desc;
    };

    class TestShaderModule final : public NLS::Render::RHI::RHIShaderModule
    {
    public:
        explicit TestShaderModule(NLS::Render::RHI::RHIShaderModuleDesc desc)
            : m_desc(std::move(desc))
        {
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIShaderModuleDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIShaderModuleDesc m_desc;
    };

    std::shared_ptr<NLS::Render::RHI::RHIShaderModule> MakeShader(
        NLS::Render::RHI::ShaderStage stage,
        std::vector<uint8_t> bytecode,
        std::string toolchainFingerprint)
    {
        NLS::Render::RHI::RHIShaderModuleDesc desc;
        desc.stage = stage;
        desc.targetBackend = NLS::Render::RHI::NativeBackendType::DX12;
        desc.entryPoint = "main";
        desc.bytecode = std::move(bytecode);
        desc.shaderToolchainFingerprint = std::move(toolchainFingerprint);
        desc.debugName = stage == NLS::Render::RHI::ShaderStage::Vertex
            ? "TestVS"
            : "TestFS";
        return std::make_shared<TestShaderModule>(std::move(desc));
    }

    std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> MakePipelineLayout(uint32_t binding)
    {
        NLS::Render::RHI::RHIBindingLayoutEntry entry;
        entry.name = "FrameData";
        entry.type = NLS::Render::RHI::BindingType::UniformBuffer;
        entry.set = 0u;
        entry.binding = binding;
        entry.count = 1u;
        entry.stageMask = NLS::Render::RHI::ShaderStageMask::AllGraphics;

        NLS::Render::RHI::RHIBindingLayoutDesc bindingLayoutDesc;
        bindingLayoutDesc.entries.push_back(entry);
        bindingLayoutDesc.debugName = "FrameLayout";

        NLS::Render::RHI::RHIPipelineLayoutDesc pipelineLayoutDesc;
        pipelineLayoutDesc.bindingLayouts.push_back(std::make_shared<TestBindingLayout>(std::move(bindingLayoutDesc)));
        pipelineLayoutDesc.debugName = "PipelineLayout";
        return std::make_shared<TestPipelineLayout>(std::move(pipelineLayoutDesc));
    }

    NLS::Render::RHI::RHIGraphicsPipelineDesc MakeGraphicsDesc()
    {
        NLS::Render::RHI::RHIGraphicsPipelineDesc desc;
        desc.pipelineLayout = MakePipelineLayout(0u);
        desc.vertexShader = MakeShader(
            NLS::Render::RHI::ShaderStage::Vertex,
            { 0x01u, 0x02u, 0x03u, 0x04u },
            "dxc-1.8|vs_6_6|-O3");
        desc.fragmentShader = MakeShader(
            NLS::Render::RHI::ShaderStage::Fragment,
            { 0x10u, 0x20u, 0x30u, 0x40u },
            "dxc-1.8|ps_6_6|-O3");
        desc.renderTargetLayout.colorFormats = { NLS::Render::RHI::TextureFormat::RGBA8 };
        desc.renderTargetLayout.depthFormat = NLS::Render::RHI::TextureFormat::Depth24Stencil8;
        desc.renderTargetLayout.hasDepth = true;
        desc.debugName = "PipelineKeyTest";
        return desc;
    }

    NLS::Render::RHI::RHIComputePipelineDesc MakeComputeDesc()
    {
        NLS::Render::RHI::RHIComputePipelineDesc desc;
        desc.pipelineLayout = MakePipelineLayout(0u);
        desc.computeShader = MakeShader(
            NLS::Render::RHI::ShaderStage::Compute,
            { 0xA0u, 0xB1u, 0xC2u, 0xD3u },
            "dxc-1.8|cs_6_6|-O3");
        desc.debugName = "LightGridInjectionPipeline";
        return desc;
    }
}

TEST(PipelineCacheKeyTests, GraphicsKeyChangesWhenShaderBytecodeChanges)
{
    auto baseDesc = MakeGraphicsDesc();
    auto changedDesc = MakeGraphicsDesc();
    changedDesc.fragmentShader = MakeShader(
        NLS::Render::RHI::ShaderStage::Fragment,
        { 0x10u, 0x20u, 0x30u, 0x41u },
        "dxc-1.8|ps_6_6|-O3");

    const auto baseKey = NLS::Render::RHI::BuildGraphicsPipelineCacheKey(baseDesc);
    const auto changedKey = NLS::Render::RHI::BuildGraphicsPipelineCacheKey(changedDesc);

    EXPECT_TRUE(baseKey.IsValid());
    EXPECT_NE(baseKey.hash, changedKey.hash);
}

TEST(PipelineCacheKeyTests, GraphicsKeyChangesWhenShaderToolchainFingerprintChanges)
{
    auto baseDesc = MakeGraphicsDesc();
    auto changedDesc = MakeGraphicsDesc();
    changedDesc.vertexShader = MakeShader(
        NLS::Render::RHI::ShaderStage::Vertex,
        { 0x01u, 0x02u, 0x03u, 0x04u },
        "dxc-1.9|vs_6_8|-O3");

    const auto baseKey = NLS::Render::RHI::BuildGraphicsPipelineCacheKey(baseDesc);
    const auto changedKey = NLS::Render::RHI::BuildGraphicsPipelineCacheKey(changedDesc);

    EXPECT_NE(baseKey.hash, changedKey.hash);
}

TEST(PipelineCacheKeyTests, GraphicsKeyChangesWhenBindingLayoutChanges)
{
    auto baseDesc = MakeGraphicsDesc();
    auto changedDesc = MakeGraphicsDesc();
    changedDesc.pipelineLayout = MakePipelineLayout(1u);

    const auto baseKey = NLS::Render::RHI::BuildGraphicsPipelineCacheKey(baseDesc);
    const auto changedKey = NLS::Render::RHI::BuildGraphicsPipelineCacheKey(changedDesc);

    EXPECT_NE(baseKey.hash, changedKey.hash);
}

TEST(PipelineCacheKeyTests, GraphicsKeyChangesWhenRenderTargetLayoutChanges)
{
    auto baseDesc = MakeGraphicsDesc();
    auto changedDesc = MakeGraphicsDesc();
    changedDesc.renderTargetLayout.colorFormats = { NLS::Render::RHI::TextureFormat::RGBA16F };

    const auto baseKey = NLS::Render::RHI::BuildGraphicsPipelineCacheKey(baseDesc);
    const auto changedKey = NLS::Render::RHI::BuildGraphicsPipelineCacheKey(changedDesc);

    EXPECT_NE(baseKey.hash, changedKey.hash);
}

TEST(PipelineCacheKeyTests, ShaderArtifactToolchainFingerprintIncludesProfileCacheKeyAndArtifactPath)
{
    NLS::Render::ShaderCompiler::ShaderCompilationOutput output;
    output.cacheKey = "shader-cache-key";
    output.artifactPath = "Cache/Shaders/Test.vs.dxil";

    const auto fingerprint = NLS::Render::Resources::BuildShaderArtifactToolchainFingerprint(
        NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL,
        "vs_6_6",
        "VSMain",
        output);

    EXPECT_EQ(fingerprint, "DXIL|vs_6_6|VSMain|shader-cache-key|Cache/Shaders/Test.vs.dxil");
}

TEST(PipelineCacheKeyTests, GraphicsKeyChangesWhenExpandedBlendStateChanges)
{
    auto baseDesc = MakeGraphicsDesc();
    auto changedDesc = MakeGraphicsDesc();
    changedDesc.blendState.independentBlendEnable = true;
    changedDesc.blendState.renderTargets.resize(2u);
    changedDesc.blendState.renderTargets[0].blendEnable = true;
    changedDesc.blendState.renderTargets[0].colorWriteMask = NLS::Render::RHI::RHIColorWriteMask::All;
    changedDesc.blendState.renderTargets[1].blendEnable = false;
    changedDesc.blendState.renderTargets[1].colorWriteMask = NLS::Render::RHI::RHIColorWriteMask::Red;

    const auto baseKey = NLS::Render::RHI::BuildGraphicsPipelineCacheKey(baseDesc);
    const auto changedKey = NLS::Render::RHI::BuildGraphicsPipelineCacheKey(changedDesc);

    EXPECT_NE(baseKey.hash, changedKey.hash);
}

TEST(PipelineCacheKeyTests, GraphicsKeyChangesWhenRasterMsaaFlagChanges)
{
    auto baseDesc = MakeGraphicsDesc();
    auto changedDesc = MakeGraphicsDesc();
    changedDesc.rasterState.multisampleEnable = true;
    changedDesc.renderTargetLayout.sampleCount = 4u;

    const auto baseKey = NLS::Render::RHI::BuildGraphicsPipelineCacheKey(baseDesc);
    const auto changedKey = NLS::Render::RHI::BuildGraphicsPipelineCacheKey(changedDesc);

    EXPECT_NE(baseKey.hash, changedKey.hash);
}

TEST(PipelineCacheKeyTests, ComputeKeyChangesWhenShaderBytecodeAndToolchainChange)
{
    auto baseDesc = MakeComputeDesc();
    auto changedBytecodeDesc = MakeComputeDesc();
    changedBytecodeDesc.computeShader = MakeShader(
        NLS::Render::RHI::ShaderStage::Compute,
        { 0xA0u, 0xB1u, 0xC2u, 0xD4u },
        "dxc-1.8|cs_6_6|-O3");

    auto changedToolchainDesc = MakeComputeDesc();
    changedToolchainDesc.computeShader = MakeShader(
        NLS::Render::RHI::ShaderStage::Compute,
        { 0xA0u, 0xB1u, 0xC2u, 0xD3u },
        "dxc-1.9|cs_6_8|-O3");

    const auto baseKey = NLS::Render::RHI::BuildComputePipelineCacheKey(baseDesc);
    const auto changedBytecodeKey = NLS::Render::RHI::BuildComputePipelineCacheKey(changedBytecodeDesc);
    const auto changedToolchainKey = NLS::Render::RHI::BuildComputePipelineCacheKey(changedToolchainDesc);

    EXPECT_TRUE(baseKey.IsValid());
    EXPECT_NE(baseKey.hash, changedBytecodeKey.hash);
    EXPECT_NE(baseKey.hash, changedToolchainKey.hash);
}

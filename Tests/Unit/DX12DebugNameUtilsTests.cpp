#include <gtest/gtest.h>

#if defined(_WIN32)
#include "Rendering/RHI/Backends/DX12/DX12DebugNameUtils.h"

namespace
{
    class TestShaderModule final : public NLS::Render::RHI::RHIShaderModule
    {
    public:
        TestShaderModule(NLS::Render::RHI::ShaderStage stage, std::string debugName)
        {
            m_desc.stage = stage;
            m_desc.debugName = std::move(debugName);
        }

        std::string_view GetDebugName() const override { return m_desc.debugName; }
        const NLS::Render::RHI::RHIShaderModuleDesc& GetDesc() const override { return m_desc; }

    private:
        NLS::Render::RHI::RHIShaderModuleDesc m_desc;
    };
}

TEST(DX12DebugNameUtilsTests, BuildsSpecificTextureAndBufferLabelsForCaptureTools)
{
    NLS::Render::RHI::RHITextureDesc textureDesc;
    textureDesc.debugName = "GBufferAlbedo";
    textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
    textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
    textureDesc.extent = { 1920u, 1080u, 1u };
    textureDesc.mipLevels = 1u;
    textureDesc.arrayLayers = 1u;
    textureDesc.sampleCount = 4u;

    const auto textureLabel = NLS::Render::RHI::DX12::BuildDX12TextureDebugLabel(textureDesc);

    EXPECT_NE(textureLabel.find("DX12"), std::string::npos);
    EXPECT_NE(textureLabel.find("Texture2D"), std::string::npos);
    EXPECT_NE(textureLabel.find("GBufferAlbedo"), std::string::npos);
    EXPECT_NE(textureLabel.find("1920x1080x1"), std::string::npos);
    EXPECT_NE(textureLabel.find("RGBA8"), std::string::npos);
    EXPECT_NE(textureLabel.find("MSAAx4"), std::string::npos);

    NLS::Render::RHI::RHIBufferDesc bufferDesc;
    bufferDesc.debugName = "FrameObjectData";
    bufferDesc.size = 4096u;
    bufferDesc.usage = NLS::Render::RHI::BufferUsageFlags::Uniform;

    const auto bufferLabel = NLS::Render::RHI::DX12::BuildDX12BufferDebugLabel(bufferDesc);

    EXPECT_NE(bufferLabel.find("DX12"), std::string::npos);
    EXPECT_NE(bufferLabel.find("Buffer"), std::string::npos);
    EXPECT_NE(bufferLabel.find("FrameObjectData"), std::string::npos);
    EXPECT_NE(bufferLabel.find("4096B"), std::string::npos);
    EXPECT_NE(bufferLabel.find("Uniform"), std::string::npos);
}

TEST(DX12DebugNameUtilsTests, BuildsPipelineLabelsWithStableKeyAndShaderNames)
{
    auto vertexShader = std::make_shared<TestShaderModule>(
        NLS::Render::RHI::ShaderStage::Vertex,
        "SceneVS");
    auto pixelShader = std::make_shared<TestShaderModule>(
        NLS::Render::RHI::ShaderStage::Fragment,
        "LightingPS");

    NLS::Render::RHI::RHIGraphicsPipelineDesc graphicsDesc;
    graphicsDesc.debugName = "DeferredLighting";
    graphicsDesc.vertexShader = vertexShader;
    graphicsDesc.fragmentShader = pixelShader;
    graphicsDesc.renderTargetLayout.colorFormats = { NLS::Render::RHI::TextureFormat::RGBA16F };
    graphicsDesc.renderTargetLayout.hasDepth = true;
    graphicsDesc.renderTargetLayout.depthFormat = NLS::Render::RHI::TextureFormat::Depth24Stencil8;

    const auto graphicsLabel = NLS::Render::RHI::DX12::BuildDX12GraphicsPipelineDebugLabel(
        graphicsDesc,
        "abc123");

    EXPECT_NE(graphicsLabel.find("DX12"), std::string::npos);
    EXPECT_NE(graphicsLabel.find("GraphicsPSO"), std::string::npos);
    EXPECT_NE(graphicsLabel.find("DeferredLighting"), std::string::npos);
    EXPECT_NE(graphicsLabel.find("SceneVS"), std::string::npos);
    EXPECT_NE(graphicsLabel.find("LightingPS"), std::string::npos);
    EXPECT_NE(graphicsLabel.find("key=abc123"), std::string::npos);

    auto computeShader = std::make_shared<TestShaderModule>(
        NLS::Render::RHI::ShaderStage::Compute,
        "LightGridCS");
    NLS::Render::RHI::RHIComputePipelineDesc computeDesc;
    computeDesc.debugName = "LightGridInjection";
    computeDesc.computeShader = computeShader;

    const auto computeLabel = NLS::Render::RHI::DX12::BuildDX12ComputePipelineDebugLabel(
        computeDesc,
        "def456");

    EXPECT_NE(computeLabel.find("DX12"), std::string::npos);
    EXPECT_NE(computeLabel.find("ComputePSO"), std::string::npos);
    EXPECT_NE(computeLabel.find("LightGridInjection"), std::string::npos);
    EXPECT_NE(computeLabel.find("LightGridCS"), std::string::npos);
    EXPECT_NE(computeLabel.find("key=def456"), std::string::npos);
}
#endif

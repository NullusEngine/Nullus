#pragma once

#include "Rendering/RHI/Core/RHIBinding.h"

namespace NLS::Render::RHI
{
    struct NLS_RENDER_API RHIPushConstantRange
    {
        ShaderStageMask stageMask = ShaderStageMask::AllGraphics;
        uint32_t offset = 0;
        uint32_t size = 0;
    };

    struct NLS_RENDER_API RHIPipelineLayoutDesc
    {
        std::vector<std::shared_ptr<class RHIBindingLayout>> bindingLayouts;
        std::vector<RHIPushConstantRange> pushConstants;
        std::string debugName;
    };

    struct NLS_RENDER_API RHIShaderModuleDesc
    {
        ShaderStage stage = ShaderStage::Vertex;
        NativeBackendType targetBackend = NativeBackendType::None;
        std::string entryPoint = "main";
        std::vector<uint8_t> bytecode;
        std::string debugName;
    };

    struct NLS_RENDER_API RHIVertexAttributeDesc
    {
        uint32_t location = 0;
        uint32_t binding = 0;
        uint32_t offset = 0;
        uint32_t elementSize = 0;
    };

    struct NLS_RENDER_API RHIVertexBufferLayoutDesc
    {
        uint32_t binding = 0;
        uint32_t stride = 0;
        bool perInstance = false;
    };

    struct NLS_RENDER_API RHIRasterStateDesc
    {
        bool cullEnabled = true;
        NLS::Render::Settings::ECullFace cullFace = NLS::Render::Settings::ECullFace::BACK;
        bool wireframe = false;
    };

    struct NLS_RENDER_API RHIBlendStateDesc
    {
        bool enabled = false;
        bool colorWrite = true;
    };

    struct NLS_RENDER_API RHIDepthStencilStateDesc
    {
        bool depthTest = true;
        bool depthWrite = true;
        NLS::Render::Settings::EComparaisonAlgorithm depthCompare = NLS::Render::Settings::EComparaisonAlgorithm::LESS;
    };

    struct NLS_RENDER_API RHIRenderTargetLayoutDesc
    {
        std::vector<TextureFormat> colorFormats;
        TextureFormat depthFormat = TextureFormat::Depth24Stencil8;
        bool hasDepth = false;
        uint32_t sampleCount = 1;
    };

    struct NLS_RENDER_API RHIGraphicsPipelineDesc
    {
        std::shared_ptr<class RHIPipelineLayout> pipelineLayout;
        std::shared_ptr<class RHIShaderModule> vertexShader;
        std::shared_ptr<class RHIShaderModule> fragmentShader;
        const NLS::Render::Resources::ShaderReflection* reflection = nullptr;
        std::vector<RHIVertexBufferLayoutDesc> vertexBuffers;
        std::vector<RHIVertexAttributeDesc> vertexAttributes;
        RHIRasterStateDesc rasterState{};
        RHIBlendStateDesc blendState{};
        RHIDepthStencilStateDesc depthStencilState{};
        PrimitiveTopology primitiveTopology = PrimitiveTopology::TriangleList;
        RHIRenderTargetLayoutDesc renderTargetLayout{};
        std::string debugName;
    };

    struct NLS_RENDER_API RHIComputePipelineDesc
    {
        std::shared_ptr<class RHIPipelineLayout> pipelineLayout;
        std::shared_ptr<class RHIShaderModule> computeShader;
        std::string debugName;
    };

    class NLS_RENDER_API RHIPipelineLayout : public RHIObject
    {
    public:
        virtual const RHIPipelineLayoutDesc& GetDesc() const = 0;
    };

    class NLS_RENDER_API RHIShaderModule : public RHIObject
    {
    public:
        virtual const RHIShaderModuleDesc& GetDesc() const = 0;
    };

    class NLS_RENDER_API RHIGraphicsPipeline : public RHIObject
    {
    public:
        virtual const RHIGraphicsPipelineDesc& GetDesc() const = 0;
    };

    class NLS_RENDER_API RHIComputePipeline : public RHIObject
    {
    public:
        virtual const RHIComputePipelineDesc& GetDesc() const = 0;
    };
}

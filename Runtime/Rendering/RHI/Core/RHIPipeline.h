#pragma once

#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/Settings/EComparaisonAlgorithm.h"
#include "Rendering/Settings/EOperation.h"

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
        std::string shaderToolchainFingerprint;
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
        bool multisampleEnable = false;
    };

    enum class NLS_RENDER_API RHIBlendFactor : uint8_t
    {
        Zero,
        One,
        SrcColor,
        InvSrcColor,
        SrcAlpha,
        InvSrcAlpha,
        DstAlpha,
        InvDstAlpha,
        DstColor,
        InvDstColor
    };

    enum class NLS_RENDER_API RHIBlendOp : uint8_t
    {
        Add,
        Subtract,
        ReverseSubtract,
        Min,
        Max
    };

    enum class NLS_RENDER_API RHIColorWriteMask : uint8_t
    {
        None = 0,
        Red = 1u << 0,
        Green = 1u << 1,
        Blue = 1u << 2,
        Alpha = 1u << 3,
        All = Red | Green | Blue | Alpha
    };

    inline constexpr RHIColorWriteMask operator|(RHIColorWriteMask lhs, RHIColorWriteMask rhs)
    {
        return static_cast<RHIColorWriteMask>(static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
    }

    inline constexpr bool HasColorWriteMask(RHIColorWriteMask mask, RHIColorWriteMask flag)
    {
        return (static_cast<uint8_t>(mask) & static_cast<uint8_t>(flag)) != 0u;
    }

    struct NLS_RENDER_API RHIRenderTargetBlendStateDesc
    {
        bool blendEnable = false;
        RHIBlendFactor srcColor = RHIBlendFactor::SrcAlpha;
        RHIBlendFactor dstColor = RHIBlendFactor::InvSrcAlpha;
        RHIBlendOp colorOp = RHIBlendOp::Add;
        RHIBlendFactor srcAlpha = RHIBlendFactor::One;
        RHIBlendFactor dstAlpha = RHIBlendFactor::InvSrcAlpha;
        RHIBlendOp alphaOp = RHIBlendOp::Add;
        RHIColorWriteMask colorWriteMask = RHIColorWriteMask::All;
    };

    struct NLS_RENDER_API RHIBlendStateDesc
    {
        bool enabled = false;
        bool colorWrite = true;
        bool alphaToCoverageEnable = false;
        bool independentBlendEnable = false;
        std::vector<RHIRenderTargetBlendStateDesc> renderTargets;
    };

    struct NLS_RENDER_API RHIDepthStencilStateDesc
    {
        bool depthTest = true;
        bool depthWrite = true;
        NLS::Render::Settings::EComparaisonAlgorithm depthCompare = NLS::Render::Settings::EComparaisonAlgorithm::LESS;
        bool stencilTest = false;
        uint32_t stencilReadMask = 0xFFu;
        uint32_t stencilWriteMask = 0xFFu;
        uint32_t stencilReference = 0u;
        NLS::Render::Settings::EComparaisonAlgorithm stencilCompare = NLS::Render::Settings::EComparaisonAlgorithm::ALWAYS;
        NLS::Render::Settings::EOperation stencilFailOp = NLS::Render::Settings::EOperation::KEEP;
        NLS::Render::Settings::EOperation stencilDepthFailOp = NLS::Render::Settings::EOperation::KEEP;
        NLS::Render::Settings::EOperation stencilPassOp = NLS::Render::Settings::EOperation::KEEP;
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
        virtual uint64_t GetPipelineHandle() const { return 0; }
    };

    class NLS_RENDER_API RHIComputePipeline : public RHIObject
    {
    public:
        virtual const RHIComputePipelineDesc& GetDesc() const = 0;
    };
}

#include "Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <spirv_msl.hpp>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#include <array>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "Debug/Logger.h"
#include "Rendering/RHI/BindingPointMap.h"
#include "Rendering/RHI/Core/RHIBinding.h"
#include "Rendering/RHI/Core/RHICommand.h"
#include "Rendering/RHI/Core/RHIDevice.h"
#include "Rendering/RHI/RHITypes.h"

namespace NLS::Render::Backend
{
    namespace
    {
        constexpr NLS::Render::RHI::BackendType kMetalBackendType = NLS::Render::RHI::BackendType::Metal;
        constexpr uint32_t kMetalPushConstantBufferIndex = 28u;

        std::optional<uint32_t> ToMetalBufferBindingIndex(const uint32_t bindingSpace, const uint32_t binding)
        {
            const uint32_t index = NLS::Render::RHI::BindingPointMap::GetUniformBufferBindingPoint(bindingSpace, binding);
            return index < kMetalPushConstantBufferIndex ? std::optional<uint32_t>(index) : std::nullopt;
        }

        std::optional<uint32_t> ToMetalTextureBindingIndex(const uint32_t bindingSpace, const uint32_t binding)
        {
            const uint32_t index = NLS::Render::RHI::BindingPointMap::GetTextureBindingPoint(bindingSpace, binding);
            return index < 128u ? std::optional<uint32_t>(index) : std::nullopt;
        }

        std::optional<uint32_t> ToMetalSamplerBindingIndex(const uint32_t bindingSpace, const uint32_t binding)
        {
            const uint32_t index = bindingSpace * NLS::Render::RHI::BindingPointMap::kUniformBufferSpaceStride + binding;
            return index < 16u ? std::optional<uint32_t>(index) : std::nullopt;
        }

        MTLLoadAction ToMetalLoadAction(const NLS::Render::RHI::LoadOp loadOp)
        {
            using namespace NLS::Render::RHI;
            switch (loadOp)
            {
            case LoadOp::Load: return MTLLoadActionLoad;
            case LoadOp::DontCare: return MTLLoadActionDontCare;
            case LoadOp::Clear:
            default: return MTLLoadActionClear;
            }
        }

        MTLStoreAction ToMetalStoreAction(const NLS::Render::RHI::StoreOp storeOp)
        {
            return storeOp == NLS::Render::RHI::StoreOp::Store
                ? MTLStoreActionStore
                : MTLStoreActionDontCare;
        }

        MTLPrimitiveType ToMetalPrimitiveType(const NLS::Render::RHI::PrimitiveTopology topology)
        {
            using namespace NLS::Render::RHI;
            switch (topology)
            {
            case PrimitiveTopology::PointList: return MTLPrimitiveTypePoint;
            case PrimitiveTopology::LineList: return MTLPrimitiveTypeLine;
            case PrimitiveTopology::TriangleList:
            default: return MTLPrimitiveTypeTriangle;
            }
        }

        MTLVertexFormat ToMetalVertexFormat(const uint32_t elementSize)
        {
            switch (elementSize)
            {
            case 4u: return MTLVertexFormatFloat;
            case 8u: return MTLVertexFormatFloat2;
            case 12u: return MTLVertexFormatFloat3;
            case 16u:
            default: return MTLVertexFormatFloat4;
            }
        }

        MTLBlendFactor ToMetalBlendFactor(const NLS::Render::RHI::RHIBlendFactor factor)
        {
            using namespace NLS::Render::RHI;
            switch (factor)
            {
            case RHIBlendFactor::Zero: return MTLBlendFactorZero;
            case RHIBlendFactor::One: return MTLBlendFactorOne;
            case RHIBlendFactor::SrcColor: return MTLBlendFactorSourceColor;
            case RHIBlendFactor::InvSrcColor: return MTLBlendFactorOneMinusSourceColor;
            case RHIBlendFactor::SrcAlpha: return MTLBlendFactorSourceAlpha;
            case RHIBlendFactor::InvSrcAlpha: return MTLBlendFactorOneMinusSourceAlpha;
            case RHIBlendFactor::DstAlpha: return MTLBlendFactorDestinationAlpha;
            case RHIBlendFactor::InvDstAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
            case RHIBlendFactor::DstColor: return MTLBlendFactorDestinationColor;
            case RHIBlendFactor::InvDstColor: return MTLBlendFactorOneMinusDestinationColor;
            default: return MTLBlendFactorOne;
            }
        }

        MTLBlendOperation ToMetalBlendOperation(const NLS::Render::RHI::RHIBlendOp operation)
        {
            using namespace NLS::Render::RHI;
            switch (operation)
            {
            case RHIBlendOp::Subtract: return MTLBlendOperationSubtract;
            case RHIBlendOp::ReverseSubtract: return MTLBlendOperationReverseSubtract;
            case RHIBlendOp::Min: return MTLBlendOperationMin;
            case RHIBlendOp::Max: return MTLBlendOperationMax;
            case RHIBlendOp::Add:
            default: return MTLBlendOperationAdd;
            }
        }

        MTLColorWriteMask ToMetalColorWriteMask(const NLS::Render::RHI::RHIColorWriteMask mask)
        {
            using namespace NLS::Render::RHI;
            MTLColorWriteMask result = MTLColorWriteMaskNone;
            if (HasColorWriteMask(mask, RHIColorWriteMask::Red)) result |= MTLColorWriteMaskRed;
            if (HasColorWriteMask(mask, RHIColorWriteMask::Green)) result |= MTLColorWriteMaskGreen;
            if (HasColorWriteMask(mask, RHIColorWriteMask::Blue)) result |= MTLColorWriteMaskBlue;
            if (HasColorWriteMask(mask, RHIColorWriteMask::Alpha)) result |= MTLColorWriteMaskAlpha;
            return result;
        }

        MTLCompareFunction ToMetalCompareFunction(const NLS::Render::Settings::EComparaisonAlgorithm algorithm)
        {
            using NLS::Render::Settings::EComparaisonAlgorithm;
            switch (algorithm)
            {
            case EComparaisonAlgorithm::NEVER: return MTLCompareFunctionNever;
            case EComparaisonAlgorithm::LESS: return MTLCompareFunctionLess;
            case EComparaisonAlgorithm::EQUAL: return MTLCompareFunctionEqual;
            case EComparaisonAlgorithm::LESS_EQUAL: return MTLCompareFunctionLessEqual;
            case EComparaisonAlgorithm::GREATER: return MTLCompareFunctionGreater;
            case EComparaisonAlgorithm::NOTEQUAL: return MTLCompareFunctionNotEqual;
            case EComparaisonAlgorithm::GREATER_EQUAL: return MTLCompareFunctionGreaterEqual;
            case EComparaisonAlgorithm::ALWAYS:
            default: return MTLCompareFunctionAlways;
            }
        }

        MTLStencilOperation ToMetalStencilOperation(const NLS::Render::Settings::EOperation operation)
        {
            using NLS::Render::Settings::EOperation;
            switch (operation)
            {
            case EOperation::ZERO: return MTLStencilOperationZero;
            case EOperation::KEEP: return MTLStencilOperationKeep;
            case EOperation::REPLACE: return MTLStencilOperationReplace;
            case EOperation::INCREMENT: return MTLStencilOperationIncrementClamp;
            case EOperation::INCREMENT_WRAP: return MTLStencilOperationIncrementWrap;
            case EOperation::DECREMENT: return MTLStencilOperationDecrementClamp;
            case EOperation::DECREMENT_WRAP: return MTLStencilOperationDecrementWrap;
            case EOperation::INVERT: return MTLStencilOperationInvert;
            default: return MTLStencilOperationKeep;
            }
        }

        MTLSamplerMinMagFilter ToMetalSamplerFilter(const NLS::Render::RHI::TextureFilter filter)
        {
            return filter == NLS::Render::RHI::TextureFilter::Nearest
                ? MTLSamplerMinMagFilterNearest
                : MTLSamplerMinMagFilterLinear;
        }

        MTLSamplerMipFilter ToMetalSamplerMipFilter(const NLS::Render::RHI::TextureMipFilter filter)
        {
            return filter == NLS::Render::RHI::TextureMipFilter::Nearest
                ? MTLSamplerMipFilterNearest
                : MTLSamplerMipFilterLinear;
        }

        MTLSamplerAddressMode ToMetalSamplerAddressMode(const NLS::Render::RHI::TextureWrap wrap)
        {
            using NLS::Render::RHI::TextureWrap;
            switch (wrap)
            {
            case TextureWrap::ClampToEdge: return MTLSamplerAddressModeClampToEdge;
            case TextureWrap::MirrorRepeat: return MTLSamplerAddressModeMirrorRepeat;
            case TextureWrap::ClampToBorder: return MTLSamplerAddressModeClampToBorderColor;
            case TextureWrap::Repeat:
            default: return MTLSamplerAddressModeRepeat;
            }
        }

        MTLPixelFormat ToMetalPixelFormat(
            const NLS::Render::RHI::TextureFormat format,
            const NLS::Render::RHI::TextureColorSpace colorSpace)
        {
            using namespace NLS::Render::RHI;
            switch (format)
            {
            case TextureFormat::R8: return MTLPixelFormatR8Unorm;
            case TextureFormat::RG8: return MTLPixelFormatRG8Unorm;
            case TextureFormat::RGBA8:
                return colorSpace == TextureColorSpace::SRGB
                    ? MTLPixelFormatRGBA8Unorm_sRGB
                    : MTLPixelFormatRGBA8Unorm;
            case TextureFormat::R16F: return MTLPixelFormatR16Float;
            case TextureFormat::RG16F: return MTLPixelFormatRG16Float;
            case TextureFormat::RGBA16F: return MTLPixelFormatRGBA16Float;
            case TextureFormat::R32F: return MTLPixelFormatR32Float;
            case TextureFormat::RG32F: return MTLPixelFormatRG32Float;
            case TextureFormat::RGBA32F: return MTLPixelFormatRGBA32Float;
            case TextureFormat::Depth32F: return MTLPixelFormatDepth32Float;
            case TextureFormat::Depth24Stencil8: return MTLPixelFormatDepth24Unorm_Stencil8;
            case TextureFormat::RGB8:
            case TextureFormat::BC1:
            case TextureFormat::BC3:
            case TextureFormat::BC5:
            case TextureFormat::BC7:
            case TextureFormat::BC6H:
            case TextureFormat::ASTC4x4:
            case TextureFormat::ETC2RGBA8:
            case TextureFormat::Count:
            default:
                return MTLPixelFormatInvalid;
            }
        }

        MTLTextureUsage ToMetalTextureUsage(const NLS::Render::RHI::TextureUsageFlags usage)
        {
            using namespace NLS::Render::RHI;
            MTLTextureUsage result = MTLTextureUsageShaderRead;
            if (HasTextureUsage(usage, TextureUsageFlags::Storage))
                result |= MTLTextureUsageShaderWrite;
            if (HasTextureUsage(usage, TextureUsageFlags::ColorAttachment) ||
                HasTextureUsage(usage, TextureUsageFlags::DepthStencilAttachment) ||
                HasTextureUsage(usage, TextureUsageFlags::Present))
            {
                result |= MTLTextureUsageRenderTarget;
            }
            return result;
        }

        class MetalAdapter final : public NLS::Render::RHI::RHIAdapter
        {
        public:
            explicit MetalAdapter(std::string hardware)
                : m_hardware(std::move(hardware))
            {
            }

            std::string_view GetDebugName() const override { return "MetalAdapter"; }
            NLS::Render::RHI::NativeBackendType GetBackendType() const override
            {
                return NLS::Render::RHI::NativeBackendType::Metal;
            }
            std::string_view GetVendor() const override { return "Apple"; }
            std::string_view GetHardware() const override { return m_hardware; }

        private:
            std::string m_hardware;
        };

        class MetalBuffer final : public NLS::Render::RHI::RHIBuffer
        {
        public:
            MetalBuffer(id<MTLBuffer> buffer, NLS::Render::RHI::RHIBufferDesc desc)
                : m_buffer([buffer retain])
                , m_desc(std::move(desc))
            {
            }

            ~MetalBuffer() override
            {
                [m_buffer release];
            }

            std::string_view GetDebugName() const override { return m_desc.debugName; }
            const NLS::Render::RHI::RHIBufferDesc& GetDesc() const override { return m_desc; }
            NLS::Render::RHI::ResourceState GetState() const override
            {
                return NLS::Render::RHI::ResourceState::GenericRead;
            }
            uint64_t GetGPUAddress() const override { return 0u; }
            NLS::Render::RHI::NativeHandle GetNativeBufferHandle() override
            {
                return { kMetalBackendType, (__bridge void*)m_buffer };
            }
            NLS::Render::RHI::RHIUpdateResult UpdateData(
                const NLS::Render::RHI::RHIBufferUploadDesc& uploadDesc) override
            {
                if (m_buffer == nil || !uploadDesc.HasData() ||
                    uploadDesc.destinationOffset + uploadDesc.dataSize > m_desc.size)
                {
                    return { NLS::Render::RHI::RHIUpdateStatusCode::InvalidArgument, "Invalid Metal buffer upload" };
                }

                std::memcpy(
                    static_cast<uint8_t*>([m_buffer contents]) + uploadDesc.destinationOffset,
                    uploadDesc.data,
                    uploadDesc.dataSize);
                return { NLS::Render::RHI::RHIUpdateStatusCode::Success, {} };
            }

            id<MTLBuffer> GetBuffer() const { return m_buffer; }

        private:
            id<MTLBuffer> m_buffer = nil;
            NLS::Render::RHI::RHIBufferDesc m_desc{};
        };

        class MetalTexture final : public NLS::Render::RHI::RHITexture
        {
        public:
            MetalTexture(id<MTLTexture> texture, NLS::Render::RHI::RHITextureDesc desc)
                : m_texture([texture retain])
                , m_desc(std::move(desc))
            {
            }

            ~MetalTexture() override
            {
                [m_texture release];
            }

            std::string_view GetDebugName() const override { return m_desc.debugName; }
            const NLS::Render::RHI::RHITextureDesc& GetDesc() const override { return m_desc; }
            NLS::Render::RHI::ResourceState GetState() const override
            {
                return NLS::Render::RHI::ResourceState::ShaderRead;
            }
            NLS::Render::RHI::NativeHandle GetNativeImageHandle() override
            {
                return { kMetalBackendType, (__bridge void*)m_texture };
            }

            id<MTLTexture> GetTexture() const { return m_texture; }

        private:
            id<MTLTexture> m_texture = nil;
            NLS::Render::RHI::RHITextureDesc m_desc{};
        };

        class MetalTextureView final : public NLS::Render::RHI::RHITextureView
        {
        public:
            MetalTextureView(
                std::shared_ptr<NLS::Render::RHI::RHITexture> texture,
                NLS::Render::RHI::RHITextureViewDesc desc,
                id<MTLTexture> textureView)
                : m_texture(std::move(texture))
                , m_desc(std::move(desc))
                , m_textureView([textureView retain])
            {
            }

            ~MetalTextureView() override
            {
                [m_textureView release];
            }

            std::string_view GetDebugName() const override { return m_desc.debugName; }
            const NLS::Render::RHI::RHITextureViewDesc& GetDesc() const override { return m_desc; }
            const std::shared_ptr<NLS::Render::RHI::RHITexture>& GetTexture() const override { return m_texture; }
            NLS::Render::RHI::NativeHandle GetNativeShaderResourceView() override
            {
                return { kMetalBackendType, (__bridge void*)m_textureView };
            }
            NLS::Render::RHI::NativeHandle GetNativeRenderTargetView() override
            {
                return { kMetalBackendType, (__bridge void*)m_textureView };
            }
            NLS::Render::RHI::NativeHandle GetNativeDepthStencilView() override
            {
                return { kMetalBackendType, (__bridge void*)m_textureView };
            }

            id<MTLTexture> GetMetalTextureView() const { return m_textureView; }

        private:
            std::shared_ptr<NLS::Render::RHI::RHITexture> m_texture;
            NLS::Render::RHI::RHITextureViewDesc m_desc{};
            id<MTLTexture> m_textureView = nil;
        };

        class MetalFence final : public NLS::Render::RHI::RHIFence
        {
        public:
            explicit MetalFence(std::string debugName) : m_debugName(std::move(debugName)) {}

            std::string_view GetDebugName() const override { return m_debugName; }
            bool IsSignaled() const override { return m_signaled.load(std::memory_order_acquire); }
            void Reset() override { m_signaled.store(false, std::memory_order_release); }
            bool Wait(uint64_t) override { return IsSignaled(); }
            void Signal() { m_signaled.store(true, std::memory_order_release); }

        private:
            std::string m_debugName;
            std::atomic_bool m_signaled { true };
        };

        class MetalSemaphore final : public NLS::Render::RHI::RHISemaphore
        {
        public:
            explicit MetalSemaphore(std::string debugName) : m_debugName(std::move(debugName)) {}

            std::string_view GetDebugName() const override { return m_debugName; }
            bool IsSignaled() const override { return m_signaled.load(std::memory_order_acquire); }
            void Reset() override { m_signaled.store(false, std::memory_order_release); }
            void Signal() { m_signaled.store(true, std::memory_order_release); }

        private:
            std::string m_debugName;
            std::atomic_bool m_signaled { false };
        };

        class MetalSampler final : public NLS::Render::RHI::RHISampler
        {
        public:
            MetalSampler(id<MTLSamplerState> sampler, NLS::Render::RHI::SamplerDesc desc, std::string debugName)
                : m_sampler([sampler retain])
                , m_desc(std::move(desc))
                , m_debugName(std::move(debugName))
            {
            }

            ~MetalSampler() override
            {
                [m_sampler release];
            }

            std::string_view GetDebugName() const override { return m_debugName; }
            const NLS::Render::RHI::SamplerDesc& GetDesc() const override { return m_desc; }
            NLS::Render::RHI::NativeHandle GetNativeSamplerHandle() override
            {
                return { kMetalBackendType, (__bridge void*)m_sampler };
            }

            id<MTLSamplerState> GetSampler() const { return m_sampler; }

        private:
            id<MTLSamplerState> m_sampler = nil;
            NLS::Render::RHI::SamplerDesc m_desc{};
            std::string m_debugName;
        };

        class MetalBindingLayout final : public NLS::Render::RHI::RHIBindingLayout
        {
        public:
            explicit MetalBindingLayout(NLS::Render::RHI::RHIBindingLayoutDesc desc)
                : m_desc(std::move(desc))
            {
            }

            std::string_view GetDebugName() const override { return m_desc.debugName; }
            const NLS::Render::RHI::RHIBindingLayoutDesc& GetDesc() const override { return m_desc; }

        private:
            NLS::Render::RHI::RHIBindingLayoutDesc m_desc{};
        };

        class MetalBindingSet final : public NLS::Render::RHI::RHIBindingSet
        {
        public:
            explicit MetalBindingSet(NLS::Render::RHI::RHIBindingSetDesc desc)
                : m_desc(std::move(desc))
            {
            }

            std::string_view GetDebugName() const override { return m_desc.debugName; }
            const NLS::Render::RHI::RHIBindingSetDesc& GetDesc() const override { return m_desc; }

        private:
            NLS::Render::RHI::RHIBindingSetDesc m_desc{};
        };

        class MetalPipelineLayout final : public NLS::Render::RHI::RHIPipelineLayout
        {
        public:
            explicit MetalPipelineLayout(NLS::Render::RHI::RHIPipelineLayoutDesc desc)
                : m_desc(std::move(desc))
            {
            }

            std::string_view GetDebugName() const override { return m_desc.debugName; }
            const NLS::Render::RHI::RHIPipelineLayoutDesc& GetDesc() const override { return m_desc; }

        private:
            NLS::Render::RHI::RHIPipelineLayoutDesc m_desc{};
        };

        class MetalShaderModule final : public NLS::Render::RHI::RHIShaderModule
        {
        public:
            MetalShaderModule(
                id<MTLLibrary> library,
                id<MTLFunction> function,
                NLS::Render::RHI::RHIShaderModuleDesc desc)
                : m_library([library retain])
                , m_function([function retain])
                , m_desc(std::move(desc))
            {
            }

            ~MetalShaderModule() override
            {
                [m_function release];
                [m_library release];
            }

            std::string_view GetDebugName() const override { return m_desc.debugName; }
            const NLS::Render::RHI::RHIShaderModuleDesc& GetDesc() const override { return m_desc; }
            id<MTLFunction> GetFunction() const { return m_function; }

        private:
            id<MTLLibrary> m_library = nil;
            id<MTLFunction> m_function = nil;
            NLS::Render::RHI::RHIShaderModuleDesc m_desc{};
        };

        class MetalGraphicsPipeline final : public NLS::Render::RHI::RHIGraphicsPipeline
        {
        public:
            MetalGraphicsPipeline(
                id<MTLRenderPipelineState> pipelineState,
                id<MTLDepthStencilState> depthStencilState,
                NLS::Render::RHI::RHIGraphicsPipelineDesc desc)
                : m_pipelineState([pipelineState retain])
                , m_depthStencilState([depthStencilState retain])
                , m_desc(std::move(desc))
            {
            }

            ~MetalGraphicsPipeline() override
            {
                [m_depthStencilState release];
                [m_pipelineState release];
            }

            std::string_view GetDebugName() const override { return m_desc.debugName; }
            const NLS::Render::RHI::RHIGraphicsPipelineDesc& GetDesc() const override { return m_desc; }
            uint64_t GetPipelineHandle() const override
            {
                return reinterpret_cast<uint64_t>((__bridge void*)m_pipelineState);
            }
            id<MTLRenderPipelineState> GetPipelineState() const { return m_pipelineState; }
            id<MTLDepthStencilState> GetDepthStencilState() const { return m_depthStencilState; }

        private:
            id<MTLRenderPipelineState> m_pipelineState = nil;
            id<MTLDepthStencilState> m_depthStencilState = nil;
            NLS::Render::RHI::RHIGraphicsPipelineDesc m_desc{};
        };

        class MetalCommandBuffer final : public NLS::Render::RHI::RHICommandBuffer
        {
        public:
            MetalCommandBuffer(id<MTLCommandQueue> queue, std::string debugName)
                : m_queue([queue retain])
                , m_debugName(std::move(debugName))
            {
            }

            ~MetalCommandBuffer() override
            {
                Reset();
                [m_queue release];
            }

            std::string_view GetDebugName() const override { return m_debugName; }

            void Begin() override
            {
                Reset();
                if (m_queue == nil)
                    return;

                m_commandBuffer = [[m_queue commandBuffer] retain];
                if (m_commandBuffer == nil)
                    return;
                if (!m_debugName.empty())
                    m_commandBuffer.label = [NSString stringWithUTF8String:m_debugName.c_str()];
                m_recording = true;
                m_closed = false;
            }

            void End() override
            {
                if (!m_recording)
                    return;
                EndActiveEncoders();
                m_recording = false;
                m_closed = m_commandBuffer != nil;
            }

            void Reset() override
            {
                EndActiveEncoders();
                [m_commandBuffer release];
                m_commandBuffer = nil;
                m_currentGraphicsPipeline.reset();
                m_indexBuffer = nil;
                m_indexBufferOffset = 0u;
                m_recording = false;
                m_closed = false;
                m_submitted = false;
            }

            bool IsRecording() const override { return m_recording; }
            NLS::Render::RHI::NativeHandle GetNativeCommandBuffer() const override
            {
                return { kMetalBackendType, (__bridge void*)m_commandBuffer };
            }

            void BeginRenderPass(const NLS::Render::RHI::RHIRenderPassDesc& desc) override
            {
                if (!m_recording || m_commandBuffer == nil)
                    return;
                EndActiveEncoders();
                if (desc.colorAttachments.empty() && !desc.depthStencilAttachment.has_value())
                    return;

                MTLRenderPassDescriptor* renderPass = [MTLRenderPassDescriptor renderPassDescriptor];
                const size_t colorAttachmentCount = (std::min)(desc.colorAttachments.size(), static_cast<size_t>(8u));
                for (size_t index = 0u; index < colorAttachmentCount; ++index)
                {
                    const auto& source = desc.colorAttachments[index];
                    const auto view = std::dynamic_pointer_cast<MetalTextureView>(source.view);
                    if (view == nullptr || view->GetMetalTextureView() == nil)
                    {
                        NLS_LOG_ERROR("MetalCommandBuffer: render pass received a non-Metal color attachment.");
                        return;
                    }

                    MTLRenderPassColorAttachmentDescriptor* attachment = renderPass.colorAttachments[index];
                    attachment.texture = view->GetMetalTextureView();
                    attachment.loadAction = ToMetalLoadAction(source.loadOp);
                    attachment.storeAction = ToMetalStoreAction(source.storeOp);
                    attachment.clearColor = MTLClearColorMake(
                        source.clearValue.r,
                        source.clearValue.g,
                        source.clearValue.b,
                        source.clearValue.a);
                }

                if (desc.depthStencilAttachment.has_value())
                {
                    const auto& source = *desc.depthStencilAttachment;
                    const auto view = std::dynamic_pointer_cast<MetalTextureView>(source.view);
                    if (view == nullptr || view->GetMetalTextureView() == nil)
                    {
                        NLS_LOG_ERROR("MetalCommandBuffer: render pass received a non-Metal depth attachment.");
                        return;
                    }

                    renderPass.depthAttachment.texture = view->GetMetalTextureView();
                    renderPass.depthAttachment.loadAction = ToMetalLoadAction(source.depthLoadOp);
                    renderPass.depthAttachment.storeAction = ToMetalStoreAction(source.depthStoreOp);
                    renderPass.depthAttachment.clearDepth = source.clearValue.depth;
                    if (view->GetMetalTextureView().pixelFormat == MTLPixelFormatDepth24Unorm_Stencil8)
                    {
                        renderPass.stencilAttachment.texture = view->GetMetalTextureView();
                        renderPass.stencilAttachment.loadAction = ToMetalLoadAction(source.stencilLoadOp);
                        renderPass.stencilAttachment.storeAction = ToMetalStoreAction(source.stencilStoreOp);
                        renderPass.stencilAttachment.clearStencil = source.clearValue.stencil;
                    }
                }

                m_renderEncoder = [[m_commandBuffer renderCommandEncoderWithDescriptor:renderPass] retain];
                if (m_renderEncoder != nil && !desc.debugName.empty())
                    m_renderEncoder.label = [NSString stringWithUTF8String:desc.debugName.c_str()];
            }

            void EndRenderPass() override
            {
                EndRenderEncoder();
            }

            void SetViewport(const NLS::Render::RHI::RHIViewport& viewport) override
            {
                if (m_renderEncoder == nil)
                    return;
                [m_renderEncoder setViewport:MTLViewport {
                    viewport.x,
                    viewport.y,
                    viewport.width,
                    viewport.height,
                    viewport.minDepth,
                    viewport.maxDepth
                }];
            }

            void SetScissor(const NLS::Render::RHI::RHIRect2D& rect) override
            {
                if (m_renderEncoder == nil || rect.x < 0 || rect.y < 0)
                    return;
                [m_renderEncoder setScissorRect:MTLScissorRect {
                    static_cast<NSUInteger>(rect.x),
                    static_cast<NSUInteger>(rect.y),
                    static_cast<NSUInteger>(rect.width),
                    static_cast<NSUInteger>(rect.height)
                }];
            }

            void BindGraphicsPipeline(
                const std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline>& pipeline) override
            {
                m_currentGraphicsPipeline = std::dynamic_pointer_cast<MetalGraphicsPipeline>(pipeline);
                if (m_renderEncoder == nil || m_currentGraphicsPipeline == nullptr)
                    return;

                [m_renderEncoder setRenderPipelineState:m_currentGraphicsPipeline->GetPipelineState()];
                if (m_currentGraphicsPipeline->GetDepthStencilState() != nil)
                    [m_renderEncoder setDepthStencilState:m_currentGraphicsPipeline->GetDepthStencilState()];
                const auto& rasterState = m_currentGraphicsPipeline->GetDesc().rasterState;
                if (!rasterState.cullEnabled)
                {
                    [m_renderEncoder setCullMode:MTLCullModeNone];
                }
                else
                {
                    [m_renderEncoder setCullMode:rasterState.cullFace == NLS::Render::Settings::ECullFace::FRONT
                        ? MTLCullModeFront
                        : MTLCullModeBack];
                }
                [m_renderEncoder setTriangleFillMode:rasterState.wireframe
                    ? MTLTriangleFillModeLines
                    : MTLTriangleFillModeFill];
                [m_renderEncoder setFrontFacingWinding:MTLWindingCounterClockwise];
            }

            void BindComputePipeline(const std::shared_ptr<NLS::Render::RHI::RHIComputePipeline>&) override {}

            void BindBindingSet(
                const uint32_t setIndex,
                const std::shared_ptr<NLS::Render::RHI::RHIBindingSet>& bindingSet) override
            {
                if (m_renderEncoder == nil)
                    return;
                const auto metalSet = std::dynamic_pointer_cast<MetalBindingSet>(bindingSet);
                if (metalSet == nullptr)
                    return;

                const auto& desc = metalSet->GetDesc();
                for (const auto& entry : desc.entries)
                {
                    const auto layoutEntry = desc.layout != nullptr
                        ? std::find_if(
                            desc.layout->GetDesc().entries.begin(),
                            desc.layout->GetDesc().entries.end(),
                            [&entry](const NLS::Render::RHI::RHIBindingLayoutEntry& candidate)
                            {
                                return candidate.binding == entry.binding && candidate.type == entry.type;
                            })
                        : std::vector<NLS::Render::RHI::RHIBindingLayoutEntry>::const_iterator{};
                    const bool hasLayoutEntry = desc.layout != nullptr &&
                        layoutEntry != desc.layout->GetDesc().entries.end();
                    const uint32_t bindingSpace = hasLayoutEntry ? layoutEntry->registerSpace : setIndex;
                    const auto stageMask = hasLayoutEntry
                        ? layoutEntry->stageMask
                        : NLS::Render::RHI::ShaderStageMask::AllGraphics;

                    const bool bindVertex = NLS::Render::RHI::HasShaderStage(
                        stageMask, NLS::Render::RHI::ShaderStageMask::Vertex);
                    const bool bindFragment = NLS::Render::RHI::HasShaderStage(
                        stageMask, NLS::Render::RHI::ShaderStageMask::Fragment);

                    switch (entry.type)
                    {
                    case NLS::Render::RHI::BindingType::UniformBuffer:
                    case NLS::Render::RHI::BindingType::StructuredBuffer:
                    case NLS::Render::RHI::BindingType::StorageBuffer:
                    {
                        const auto buffer = std::dynamic_pointer_cast<MetalBuffer>(entry.buffer);
                        const auto index = ToMetalBufferBindingIndex(bindingSpace, entry.binding);
                        if (buffer == nullptr || !index.has_value())
                            break;
                        if (bindVertex)
                            [m_renderEncoder setVertexBuffer:buffer->GetBuffer()
                                                   offset:entry.bufferOffset
                                                  atIndex:*index];
                        if (bindFragment)
                            [m_renderEncoder setFragmentBuffer:buffer->GetBuffer()
                                                     offset:entry.bufferOffset
                                                    atIndex:*index];
                        break;
                    }
                    case NLS::Render::RHI::BindingType::Texture:
                    case NLS::Render::RHI::BindingType::RWTexture:
                    {
                        const auto textureView = std::dynamic_pointer_cast<MetalTextureView>(entry.textureView);
                        const auto index = ToMetalTextureBindingIndex(bindingSpace, entry.binding);
                        if (textureView == nullptr || !index.has_value())
                            break;
                        if (bindVertex)
                            [m_renderEncoder setVertexTexture:textureView->GetMetalTextureView() atIndex:*index];
                        if (bindFragment)
                            [m_renderEncoder setFragmentTexture:textureView->GetMetalTextureView() atIndex:*index];
                        break;
                    }
                    case NLS::Render::RHI::BindingType::Sampler:
                    {
                        const auto sampler = std::dynamic_pointer_cast<MetalSampler>(entry.sampler);
                        const auto index = ToMetalSamplerBindingIndex(bindingSpace, entry.binding);
                        if (sampler == nullptr || !index.has_value())
                            break;
                        if (bindVertex)
                            [m_renderEncoder setVertexSamplerState:sampler->GetSampler() atIndex:*index];
                        if (bindFragment)
                            [m_renderEncoder setFragmentSamplerState:sampler->GetSampler() atIndex:*index];
                        break;
                    }
                    }
                }
            }

            void PushConstants(
                const NLS::Render::RHI::ShaderStageMask stageMask,
                uint32_t,
                const uint32_t size,
                const void* data) override
            {
                if (m_renderEncoder == nil || data == nullptr || size == 0u)
                    return;
                if (NLS::Render::RHI::HasShaderStage(stageMask, NLS::Render::RHI::ShaderStageMask::Vertex))
                    [m_renderEncoder setVertexBytes:data length:size atIndex:kMetalPushConstantBufferIndex];
                if (NLS::Render::RHI::HasShaderStage(stageMask, NLS::Render::RHI::ShaderStageMask::Fragment))
                    [m_renderEncoder setFragmentBytes:data length:size atIndex:kMetalPushConstantBufferIndex];
            }

            void BindVertexBuffer(const uint32_t slot, const NLS::Render::RHI::RHIVertexBufferView& view) override
            {
                if (m_renderEncoder == nil || slot >= 8u)
                    return;
                const auto buffer = std::dynamic_pointer_cast<MetalBuffer>(view.buffer);
                if (buffer != nullptr)
                    [m_renderEncoder setVertexBuffer:buffer->GetBuffer() offset:view.offset atIndex:slot];
            }

            void BindIndexBuffer(const NLS::Render::RHI::RHIIndexBufferView& view) override
            {
                const auto buffer = std::dynamic_pointer_cast<MetalBuffer>(view.buffer);
                m_indexBuffer = buffer != nullptr ? buffer->GetBuffer() : nil;
                m_indexBufferOffset = view.offset;
                m_indexType = view.indexType;
            }

            void Draw(
                const uint32_t vertexCount,
                const uint32_t instanceCount,
                const uint32_t firstVertex,
                const uint32_t firstInstance) override
            {
                if (!CanDraw())
                    return;
                [m_renderEncoder drawPrimitives:ToMetalPrimitiveType(m_currentGraphicsPipeline->GetDesc().primitiveTopology)
                                     vertexStart:firstVertex
                                     vertexCount:vertexCount
                                   instanceCount:instanceCount
                                    baseInstance:firstInstance];
            }

            void DrawIndexed(
                const uint32_t indexCount,
                const uint32_t instanceCount,
                const uint32_t firstIndex,
                const int32_t vertexOffset,
                const uint32_t firstInstance) override
            {
                if (!CanDraw() || m_indexBuffer == nil)
                    return;
                const NSUInteger indexSize = m_indexType == NLS::Render::RHI::IndexType::UInt16 ? 2u : 4u;
                [m_renderEncoder drawIndexedPrimitives:ToMetalPrimitiveType(m_currentGraphicsPipeline->GetDesc().primitiveTopology)
                                         indexCount:indexCount
                                          indexType:m_indexType == NLS::Render::RHI::IndexType::UInt16
                                              ? MTLIndexTypeUInt16
                                              : MTLIndexTypeUInt32
                                        indexBuffer:m_indexBuffer
                                  indexBufferOffset:m_indexBufferOffset + firstIndex * indexSize
                                      instanceCount:instanceCount
                                         baseVertex:vertexOffset
                                       baseInstance:firstInstance];
            }

            NLS::Render::RHI::RHICommandRecordingResult DrawChecked(
                const uint32_t vertexCount,
                const uint32_t instanceCount,
                const uint32_t firstVertex,
                const uint32_t firstInstance) override
            {
                if (!CanDraw())
                {
                    return {
                        NLS::Render::RHI::RHICommandRecordingStatusCode::InvalidArgument,
                        "Metal draw requires an active render pass and graphics pipeline."
                    };
                }
                Draw(vertexCount, instanceCount, firstVertex, firstInstance);
                return {};
            }

            NLS::Render::RHI::RHICommandRecordingResult DrawIndexedChecked(
                const uint32_t indexCount,
                const uint32_t instanceCount,
                const uint32_t firstIndex,
                const int32_t vertexOffset,
                const uint32_t firstInstance) override
            {
                if (!CanDraw() || m_indexBuffer == nil)
                {
                    return {
                        NLS::Render::RHI::RHICommandRecordingStatusCode::InvalidArgument,
                        "Metal indexed draw requires an active render pass, graphics pipeline, and index buffer."
                    };
                }
                DrawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
                return {};
            }

            void Dispatch(uint32_t, uint32_t, uint32_t) override {}

            void CopyBuffer(
                const std::shared_ptr<NLS::Render::RHI::RHIBuffer>& source,
                const std::shared_ptr<NLS::Render::RHI::RHIBuffer>& destination,
                const NLS::Render::RHI::RHIBufferCopyRegion& region) override
            {
                const auto sourceBuffer = std::dynamic_pointer_cast<MetalBuffer>(source);
                const auto destinationBuffer = std::dynamic_pointer_cast<MetalBuffer>(destination);
                id<MTLBlitCommandEncoder> encoder = BeginBlitEncoder();
                if (encoder == nil || sourceBuffer == nullptr || destinationBuffer == nullptr || region.size == 0u)
                    return;
                [encoder copyFromBuffer:sourceBuffer->GetBuffer()
                           sourceOffset:region.srcOffset
                               toBuffer:destinationBuffer->GetBuffer()
                      destinationOffset:region.dstOffset
                                    size:region.size];
            }

            void CopyBufferToTexture(const NLS::Render::RHI::RHIBufferToTextureCopyDesc& desc) override
            {
                const auto source = std::dynamic_pointer_cast<MetalBuffer>(desc.source);
                const auto destination = std::dynamic_pointer_cast<MetalTexture>(desc.destination);
                id<MTLBlitCommandEncoder> encoder = BeginBlitEncoder();
                if (encoder == nil || source == nullptr || destination == nullptr ||
                    desc.extent.width == 0u || desc.extent.height == 0u)
                {
                    return;
                }
                const uint32_t rowPitch = desc.rowPitch != 0u
                    ? desc.rowPitch
                    : NLS::Render::RHI::CalculateTextureRowPitch(destination->GetDesc().format, desc.extent.width);
                const uint32_t slicePitch = desc.slicePitch != 0u
                    ? desc.slicePitch
                    : rowPitch * desc.extent.height;
                [encoder copyFromBuffer:source->GetBuffer()
                           sourceOffset:desc.bufferOffset
                      sourceBytesPerRow:rowPitch
                    sourceBytesPerImage:slicePitch
                             sourceSize:MTLSizeMake(desc.extent.width, desc.extent.height, desc.extent.depth)
                              toTexture:destination->GetTexture()
                       destinationSlice:desc.arrayLayer
                       destinationLevel:desc.mipLevel
                      destinationOrigin:MTLOriginMake(desc.textureOffset.x, desc.textureOffset.y, desc.textureOffset.z)];
            }

            void CopyTexture(const NLS::Render::RHI::RHITextureCopyDesc& desc) override
            {
                const auto source = std::dynamic_pointer_cast<MetalTexture>(desc.source);
                const auto destination = std::dynamic_pointer_cast<MetalTexture>(desc.destination);
                id<MTLBlitCommandEncoder> encoder = BeginBlitEncoder();
                if (encoder == nil || source == nullptr || destination == nullptr ||
                    desc.extent.width == 0u || desc.extent.height == 0u)
                {
                    return;
                }
                [encoder copyFromTexture:source->GetTexture()
                             sourceSlice:desc.sourceRange.baseArrayLayer
                             sourceLevel:desc.sourceRange.baseMipLevel
                            sourceOrigin:MTLOriginMake(desc.sourceOffset.x, desc.sourceOffset.y, desc.sourceOffset.z)
                              sourceSize:MTLSizeMake(desc.extent.width, desc.extent.height, desc.extent.depth)
                               toTexture:destination->GetTexture()
                        destinationSlice:desc.destinationRange.baseArrayLayer
                        destinationLevel:desc.destinationRange.baseMipLevel
                       destinationOrigin:MTLOriginMake(desc.destinationOffset.x, desc.destinationOffset.y, desc.destinationOffset.z)];
            }

            void Barrier(const NLS::Render::RHI::RHIBarrierDesc&) override {}
            bool IsClosedForSubmission() const override { return m_closed && !m_submitted; }
            id<MTLCommandBuffer> GetMetalCommandBuffer() const { return m_commandBuffer; }
            void MarkSubmitted() { m_submitted = true; }

        private:
            bool CanDraw() const
            {
                return m_renderEncoder != nil && m_currentGraphicsPipeline != nullptr &&
                    m_currentGraphicsPipeline->GetPipelineState() != nil;
            }

            id<MTLBlitCommandEncoder> BeginBlitEncoder()
            {
                if (!m_recording || m_commandBuffer == nil || m_renderEncoder != nil)
                    return nil;
                if (m_blitEncoder == nil)
                    m_blitEncoder = [[m_commandBuffer blitCommandEncoder] retain];
                return m_blitEncoder;
            }

            void EndRenderEncoder()
            {
                if (m_renderEncoder != nil)
                {
                    [m_renderEncoder endEncoding];
                    [m_renderEncoder release];
                    m_renderEncoder = nil;
                }
            }

            void EndBlitEncoder()
            {
                if (m_blitEncoder != nil)
                {
                    [m_blitEncoder endEncoding];
                    [m_blitEncoder release];
                    m_blitEncoder = nil;
                }
            }

            void EndActiveEncoders()
            {
                EndRenderEncoder();
                EndBlitEncoder();
            }

            id<MTLCommandQueue> m_queue = nil;
            id<MTLCommandBuffer> m_commandBuffer = nil;
            id<MTLRenderCommandEncoder> m_renderEncoder = nil;
            id<MTLBlitCommandEncoder> m_blitEncoder = nil;
            std::shared_ptr<MetalGraphicsPipeline> m_currentGraphicsPipeline;
            id<MTLBuffer> m_indexBuffer = nil;
            uint64_t m_indexBufferOffset = 0u;
            NLS::Render::RHI::IndexType m_indexType = NLS::Render::RHI::IndexType::UInt32;
            std::string m_debugName;
            bool m_recording = false;
            bool m_closed = false;
            bool m_submitted = false;
        };

        class MetalCommandPool final : public NLS::Render::RHI::RHICommandPool
        {
        public:
            MetalCommandPool(id<MTLCommandQueue> queue, NLS::Render::RHI::QueueType queueType, std::string debugName)
                : m_queue([queue retain])
                , m_queueType(queueType)
                , m_debugName(std::move(debugName))
            {
            }

            ~MetalCommandPool() override
            {
                [m_queue release];
            }

            std::string_view GetDebugName() const override { return m_debugName; }
            NLS::Render::RHI::QueueType GetQueueType() const override { return m_queueType; }
            std::shared_ptr<NLS::Render::RHI::RHICommandBuffer> CreateCommandBuffer(std::string debugName) override
            {
                if (debugName.empty())
                    debugName = m_debugName + ".CommandBuffer";
                return std::make_shared<MetalCommandBuffer>(m_queue, std::move(debugName));
            }
            void Reset() override {}

        private:
            id<MTLCommandQueue> m_queue = nil;
            NLS::Render::RHI::QueueType m_queueType;
            std::string m_debugName;
        };

        class MetalSwapchain final : public NLS::Render::RHI::RHISwapchain
        {
        public:
            MetalSwapchain(id<MTLDevice> device, NLS::Render::RHI::SwapchainDesc desc)
                : m_device([device retain])
                , m_desc(std::move(desc))
            {
                auto* glfwWindow = static_cast<GLFWwindow*>(m_desc.platformWindow);
                NSWindow* window = glfwWindow != nullptr ? glfwGetCocoaWindow(glfwWindow) : nil;
                if (window == nil || window.contentView == nil)
                    return;

                m_nativeWindowHandle = (__bridge void*)window;
                m_layer = [[CAMetalLayer alloc] init];
                m_layer.device = m_device;
                m_layer.pixelFormat = MTLPixelFormatRGBA8Unorm;
                m_layer.framebufferOnly = YES;
                m_layer.displaySyncEnabled = m_desc.vsync;

                NSView* view = window.contentView;
                view.wantsLayer = YES;
                view.layer = m_layer;
                UpdateDrawableSize(m_desc.width, m_desc.height);
            }

            ~MetalSwapchain() override
            {
                ReleaseCurrentDrawable();
                [m_layer release];
                [m_device release];
            }

            std::string_view GetDebugName() const override { return "MetalSwapchain"; }
            const NLS::Render::RHI::SwapchainDesc& GetDesc() const override { return m_desc; }
            uint32_t GetImageCount() const override { return 1u; }
            std::optional<NLS::Render::RHI::RHIAcquiredImage> AcquireNextImage(
                const std::shared_ptr<NLS::Render::RHI::RHISemaphore>& signalSemaphore,
                const std::shared_ptr<NLS::Render::RHI::RHIFence>& signalFence) override
            {
                if (m_layer == nil)
                    return std::nullopt;

                ReleaseCurrentDrawable();
                id<CAMetalDrawable> drawable = [m_layer nextDrawable];
                if (drawable == nil)
                    return std::nullopt;

                m_drawable = [drawable retain];
                NLS::Render::RHI::RHITextureDesc textureDesc{};
                textureDesc.extent = {
                    static_cast<uint32_t>(m_drawable.texture.width),
                    static_cast<uint32_t>(m_drawable.texture.height),
                    1u
                };
                textureDesc.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
                textureDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
                textureDesc.usage = NLS::Render::RHI::TextureUsageFlags::ColorAttachment |
                    NLS::Render::RHI::TextureUsageFlags::Present;
                textureDesc.debugName = "MetalSwapchainDrawable";
                m_backbufferTexture = std::make_shared<MetalTexture>(m_drawable.texture, std::move(textureDesc));

                NLS::Render::RHI::RHITextureViewDesc viewDesc{};
                viewDesc.viewType = NLS::Render::RHI::TextureViewType::Texture2D;
                viewDesc.format = NLS::Render::RHI::TextureFormat::RGBA8;
                viewDesc.debugName = "MetalSwapchainDrawableView";
                m_backbufferView = std::make_shared<MetalTextureView>(
                    m_backbufferTexture,
                    std::move(viewDesc),
                    m_drawable.texture);
                if (const auto semaphore = std::dynamic_pointer_cast<MetalSemaphore>(signalSemaphore); semaphore != nullptr)
                    semaphore->Signal();
                if (const auto fence = std::dynamic_pointer_cast<MetalFence>(signalFence); fence != nullptr)
                    fence->Signal();
                return NLS::Render::RHI::RHIAcquiredImage { 0u, m_backbufferView, false };
            }
            std::shared_ptr<NLS::Render::RHI::RHITextureView> GetBackbufferView(uint32_t index) override
            {
                return index == 0u ? m_backbufferView : nullptr;
            }
            bool Resize(uint32_t width, uint32_t height) override
            {
                if (width == 0u || height == 0u || m_layer == nil)
                    return false;
                m_desc.width = width;
                m_desc.height = height;
                UpdateDrawableSize(width, height);
                return true;
            }
            NLS::Render::RHI::NativeHandle GetNativeSwapchainHandle() override
            {
                return { kMetalBackendType, (__bridge void*)m_layer };
            }
            void* GetNativeWindowHandle() const { return m_nativeWindowHandle; }

            bool Present(id<MTLCommandQueue> queue, const std::shared_ptr<MetalFence>& signalFence)
            {
                if (queue == nil || m_drawable == nil)
                    return false;

                id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
                [commandBuffer presentDrawable:m_drawable];
                if (signalFence != nullptr)
                {
                    signalFence->Reset();
                    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>)
                    {
                        signalFence->Signal();
                    }];
                }
                [commandBuffer commit];
                ReleaseCurrentDrawable();
                return true;
            }

        private:
            void UpdateDrawableSize(uint32_t fallbackWidth, uint32_t fallbackHeight)
            {
                if (m_layer == nil)
                    return;

                uint32_t width = fallbackWidth;
                uint32_t height = fallbackHeight;
                if (auto* glfwWindow = static_cast<GLFWwindow*>(m_desc.platformWindow); glfwWindow != nullptr)
                {
                    int framebufferWidth = 0;
                    int framebufferHeight = 0;
                    glfwGetFramebufferSize(glfwWindow, &framebufferWidth, &framebufferHeight);
                    if (framebufferWidth > 0 && framebufferHeight > 0)
                    {
                        width = static_cast<uint32_t>(framebufferWidth);
                        height = static_cast<uint32_t>(framebufferHeight);
                    }
                }

                if (width > 0u && height > 0u)
                    m_layer.drawableSize = CGSizeMake(width, height);
            }

            void ReleaseCurrentDrawable()
            {
                m_backbufferView.reset();
                m_backbufferTexture.reset();
                [m_drawable release];
                m_drawable = nil;
            }

            id<MTLDevice> m_device = nil;
            CAMetalLayer* m_layer = nil;
            id<CAMetalDrawable> m_drawable = nil;
            NLS::Render::RHI::SwapchainDesc m_desc{};
            void* m_nativeWindowHandle = nullptr;
            std::shared_ptr<NLS::Render::RHI::RHITexture> m_backbufferTexture;
            std::shared_ptr<NLS::Render::RHI::RHITextureView> m_backbufferView;
        };

        class MetalQueue final : public NLS::Render::RHI::RHIQueue
        {
        public:
            MetalQueue(id<MTLCommandQueue> queue, NLS::Render::RHI::QueueType queueType, std::string debugName)
                : m_queue([queue retain])
                , m_queueType(queueType)
                , m_debugName(std::move(debugName))
            {
            }

            ~MetalQueue() override
            {
                [m_queue release];
            }

            std::string_view GetDebugName() const override { return m_debugName; }
            NLS::Render::RHI::QueueType GetType() const override { return m_queueType; }
            void Submit(const NLS::Render::RHI::RHISubmitDesc& submitDesc) override
            {
                (void)SubmitChecked(submitDesc);
            }
            void Present(const NLS::Render::RHI::RHIPresentDesc& presentDesc) override
            {
                (void)PresentChecked(presentDesc);
            }
            NLS::Render::RHI::RHIQueueOperationResult SubmitChecked(
                const NLS::Render::RHI::RHISubmitDesc& submitDesc) override
            {
                for (const auto& waitSemaphore : submitDesc.waitSemaphores)
                {
                    const auto semaphore = std::dynamic_pointer_cast<MetalSemaphore>(waitSemaphore);
                    if (semaphore == nullptr)
                    {
                        return {
                            NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument,
                            "Metal queue received a semaphore from another backend."
                        };
                    }
                    if (!semaphore->IsSignaled())
                    {
                        return {
                            NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
                            "Metal queue cannot submit before its CPU-visible wait semaphore is signaled."
                        };
                    }
                }

                std::vector<std::shared_ptr<MetalCommandBuffer>> commandBuffers;
                commandBuffers.reserve(submitDesc.commandBuffers.size());
                for (const auto& commandBuffer : submitDesc.commandBuffers)
                {
                    const auto metalCommandBuffer = std::dynamic_pointer_cast<MetalCommandBuffer>(commandBuffer);
                    if (metalCommandBuffer == nullptr)
                    {
                        return {
                            NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument,
                            "Metal queue received a command buffer from another backend."
                        };
                    }
                    if (!metalCommandBuffer->IsClosedForSubmission() || metalCommandBuffer->GetMetalCommandBuffer() == nil)
                    {
                        return {
                            NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument,
                            "Metal command buffer must be ended before queue submission."
                        };
                    }
                    commandBuffers.push_back(metalCommandBuffer);
                }

                const auto signalFence = std::dynamic_pointer_cast<MetalFence>(submitDesc.signalFence);
                if (submitDesc.signalFence != nullptr && signalFence == nullptr)
                {
                    return {
                        NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument,
                        "Metal queue received a fence from another backend."
                    };
                }

                std::vector<std::shared_ptr<MetalSemaphore>> signalSemaphores;
                signalSemaphores.reserve(submitDesc.signalSemaphores.size());
                for (const auto& signalSemaphore : submitDesc.signalSemaphores)
                {
                    const auto semaphore = std::dynamic_pointer_cast<MetalSemaphore>(signalSemaphore);
                    if (semaphore == nullptr)
                    {
                        return {
                            NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument,
                            "Metal queue received a signal semaphore from another backend."
                        };
                    }
                    semaphore->Reset();
                    signalSemaphores.push_back(semaphore);
                }

                if (signalFence != nullptr)
                    signalFence->Reset();

                if (commandBuffers.empty())
                {
                    for (const auto& semaphore : signalSemaphores)
                        semaphore->Signal();
                    if (signalFence != nullptr)
                        signalFence->Signal();
                    NLS::Render::RHI::RHIQueueOperationResult result{};
                    result.frameFenceSignalQueued = signalFence != nullptr;
                    return result;
                }

                const auto completionFence = signalFence;
                const auto completionSemaphores = signalSemaphores;
                id<MTLCommandBuffer> finalCommandBuffer = commandBuffers.back()->GetMetalCommandBuffer();
                [finalCommandBuffer addCompletedHandler:^(id<MTLCommandBuffer>)
                {
                    for (const auto& semaphore : completionSemaphores)
                        semaphore->Signal();
                    if (completionFence != nullptr)
                        completionFence->Signal();
                }];

                for (const auto& commandBuffer : commandBuffers)
                {
                    [commandBuffer->GetMetalCommandBuffer() commit];
                    commandBuffer->MarkSubmitted();
                }

                NLS::Render::RHI::RHIQueueOperationResult result{};
                result.mayHaveQueuedGpuWork = true;
                result.frameFenceSignalQueued = signalFence != nullptr;
                return result;
            }
            NLS::Render::RHI::RHIQueueOperationResult PresentChecked(
                const NLS::Render::RHI::RHIPresentDesc& presentDesc) override
            {
                const auto swapchain = std::dynamic_pointer_cast<MetalSwapchain>(presentDesc.swapchain);
                if (swapchain == nullptr)
                {
                    return {
                        NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument,
                        "Metal queue received a swapchain from another backend."
                    };
                }
                const auto signalFence = std::dynamic_pointer_cast<MetalFence>(presentDesc.signalFence);
                if (presentDesc.signalFence != nullptr && signalFence == nullptr)
                {
                    return {
                        NLS::Render::RHI::RHIQueueOperationStatusCode::InvalidArgument,
                        "Metal queue received a present fence from another backend."
                    };
                }
                if (!swapchain->Present(m_queue, signalFence))
                {
                    return {
                        NLS::Render::RHI::RHIQueueOperationStatusCode::BackendFailure,
                        "Metal swapchain does not own an acquired drawable."
                    };
                }
                NLS::Render::RHI::RHIQueueOperationResult result{};
                result.mayHaveQueuedGpuWork = true;
                result.frameFenceSignalQueued = signalFence != nullptr;
                return result;
            }

        private:
            id<MTLCommandQueue> m_queue = nil;
            NLS::Render::RHI::QueueType m_queueType;
            std::string m_debugName;
        };

        class MetalDevice final : public NLS::Render::RHI::RHIDevice
        {
        public:
            explicit MetalDevice(id<MTLDevice> device)
                : m_device([device retain])
                , m_graphicsQueue([m_device newCommandQueue])
                , m_adapter(std::make_shared<MetalAdapter>(
                    m_device.name != nil ? std::string(m_device.name.UTF8String) : std::string("Unknown Metal device")))
                , m_capabilities(CreateCapabilities())
            {
            }

            ~MetalDevice() override
            {
                [m_graphicsQueue release];
                [m_device release];
            }

            std::string_view GetDebugName() const override { return "MetalDevice"; }
            const std::shared_ptr<NLS::Render::RHI::RHIAdapter>& GetAdapter() const override { return m_adapter; }
            const NLS::Render::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_capabilities; }
            NLS::Render::RHI::NativeRenderDeviceInfo GetNativeDeviceInfo() const override
            {
                NLS::Render::RHI::NativeRenderDeviceInfo info{};
                info.backend = NLS::Render::RHI::NativeBackendType::Metal;
                info.device = (__bridge void*)m_device;
                info.graphicsQueue = (__bridge void*)m_graphicsQueue;
                info.swapchain = m_swapchainLayer;
                info.platformWindow = m_platformWindow;
                info.nativeWindowHandle = m_nativeWindowHandle;
                info.swapchainImageCount = m_swapchainLayer != nullptr ? 1u : 0u;
                return info;
            }
            bool IsBackendReady() const override { return m_device != nil && m_graphicsQueue != nil; }
            std::shared_ptr<NLS::Render::RHI::RHIQueue> GetQueue(NLS::Render::RHI::QueueType queueType) override
            {
                const size_t queueIndex = static_cast<size_t>(queueType);
                if (queueIndex >= m_queues.size())
                    return nullptr;
                if (m_queues[queueIndex] == nullptr)
                {
                    m_queues[queueIndex] = std::make_shared<MetalQueue>(
                        m_graphicsQueue,
                        queueType,
                        queueType == NLS::Render::RHI::QueueType::Graphics ? "MetalGraphicsQueue" : "MetalSharedQueue");
                }
                return m_queues[queueIndex];
            }
            std::shared_ptr<NLS::Render::RHI::RHISwapchain> CreateSwapchain(
                const NLS::Render::RHI::SwapchainDesc& desc) override
            {
                auto swapchain = std::make_shared<MetalSwapchain>(m_device, desc);
                const auto handle = swapchain->GetNativeSwapchainHandle();
                if (!handle.IsValid())
                {
                    NLS_LOG_ERROR("CreateMetalSwapchain: GLFW did not provide a Cocoa window or CAMetalLayer creation failed.");
                    return nullptr;
                }
                m_swapchainLayer = handle.handle;
                m_platformWindow = desc.platformWindow;
                m_nativeWindowHandle = swapchain->GetNativeWindowHandle();
                return swapchain;
            }
            std::shared_ptr<NLS::Render::RHI::RHIBuffer> CreateBuffer(
                const NLS::Render::RHI::RHIBufferDesc& desc,
                const NLS::Render::RHI::RHIBufferUploadDesc& uploadDesc) override
            {
                if (m_device == nil || desc.size == 0u)
                    return nullptr;

                id<MTLBuffer> buffer = [m_device newBufferWithLength:desc.size options:MTLResourceStorageModeShared];
                if (buffer == nil)
                    return nullptr;
                auto result = std::make_shared<MetalBuffer>(buffer, desc);
                [buffer release];
                if (uploadDesc.HasData() && !result->UpdateData(uploadDesc).Succeeded())
                    return nullptr;
                return result;
            }
            std::shared_ptr<NLS::Render::RHI::RHITexture> CreateTexture(
                const NLS::Render::RHI::RHITextureDesc& desc,
                const NLS::Render::RHI::RHITextureUploadDesc& uploadDesc) override
            {
                if (m_device == nil || desc.dimension != NLS::Render::RHI::TextureDimension::Texture2D ||
                    desc.extent.width == 0u || desc.extent.height == 0u)
                {
                    return nullptr;
                }

                const MTLPixelFormat pixelFormat = ToMetalPixelFormat(desc.format, desc.colorSpace);
                if (pixelFormat == MTLPixelFormatInvalid)
                {
                    NLS_LOG_WARNING("CreateMetalTexture: unsupported texture format " +
                        std::string(NLS::Render::RHI::GetTextureFormatName(desc.format)));
                    return nullptr;
                }

                MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixelFormat
                    width:desc.extent.width
                    height:desc.extent.height
                    mipmapped:desc.mipLevels > 1u];
                descriptor.usage = ToMetalTextureUsage(desc.usage);
                descriptor.storageMode = MTLStorageModeShared;
                id<MTLTexture> texture = [m_device newTextureWithDescriptor:descriptor];
                if (texture == nil)
                    return nullptr;

                const auto* formatInfo = NLS::Render::RHI::GetTextureFormatDescriptor(desc.format);
                if (uploadDesc.HasData())
                {
                    const auto uploadSubresource = [&](const void* data, const uint32_t mipLevel)
                    {
                        if (data == nullptr || formatInfo == nullptr || formatInfo->isCompressed)
                            return false;
                        const uint32_t mipWidth = (std::max)(1u, desc.extent.width >> mipLevel);
                        const uint32_t mipHeight = (std::max)(1u, desc.extent.height >> mipLevel);
                        const uint32_t rowPitch = NLS::Render::RHI::CalculateTextureRowPitch(desc.format, mipWidth);
                        [texture replaceRegion:MTLRegionMake2D(0, 0, mipWidth, mipHeight)
                                  mipmapLevel:mipLevel
                                    withBytes:data
                                  bytesPerRow:rowPitch];
                        return true;
                    };

                    if (!uploadDesc.subresources.empty())
                    {
                        const uint32_t count = (std::min)(
                            static_cast<uint32_t>(uploadDesc.subresources.size()),
                            (std::max)(1u, desc.mipLevels));
                        for (uint32_t mipLevel = 0u; mipLevel < count; ++mipLevel)
                        {
                            if (!uploadSubresource(uploadDesc.subresources[mipLevel].data, mipLevel))
                            {
                                [texture release];
                                return nullptr;
                            }
                        }
                    }
                    else if (!uploadSubresource(uploadDesc.data, uploadDesc.mipLevel))
                    {
                        [texture release];
                        return nullptr;
                    }
                }

                auto result = std::make_shared<MetalTexture>(texture, desc);
                [texture release];
                return result;
            }
            std::shared_ptr<NLS::Render::RHI::RHITextureView> CreateTextureView(
                const std::shared_ptr<NLS::Render::RHI::RHITexture>& texture,
                const NLS::Render::RHI::RHITextureViewDesc& desc) override
            {
                const auto metalTexture = std::dynamic_pointer_cast<MetalTexture>(texture);
                if (metalTexture == nullptr)
                    return nullptr;

                const MTLPixelFormat viewFormat = ToMetalPixelFormat(desc.format, desc.colorSpace);
                if (viewFormat == MTLPixelFormatInvalid)
                    return nullptr;
                const auto& textureDesc = metalTexture->GetDesc();
                if (desc.subresourceRange.baseMipLevel >= textureDesc.mipLevels ||
                    desc.subresourceRange.baseArrayLayer >= textureDesc.arrayLayers)
                {
                    return nullptr;
                }

                const uint32_t mipCount = (std::min)(
                    (std::max)(1u, desc.subresourceRange.mipLevelCount),
                    textureDesc.mipLevels - desc.subresourceRange.baseMipLevel);
                const uint32_t layerCount = (std::min)(
                    (std::max)(1u, desc.subresourceRange.arrayLayerCount),
                    textureDesc.arrayLayers - desc.subresourceRange.baseArrayLayer);
                id<MTLTexture> textureView = [metalTexture->GetTexture()
                    newTextureViewWithPixelFormat:viewFormat
                    textureType:MTLTextureType2D
                    levels:NSMakeRange(desc.subresourceRange.baseMipLevel, mipCount)
                    slices:NSMakeRange(desc.subresourceRange.baseArrayLayer, layerCount)];
                if (textureView == nil)
                    return nullptr;
                auto result = std::make_shared<MetalTextureView>(texture, desc, textureView);
                [textureView release];
                return result;
            }
            std::shared_ptr<NLS::Render::RHI::RHISampler> CreateSampler(
                const NLS::Render::RHI::SamplerDesc& desc, std::string debugName) override
            {
                if (m_device == nil)
                    return nullptr;
                MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
                descriptor.minFilter = ToMetalSamplerFilter(desc.minFilter);
                descriptor.magFilter = ToMetalSamplerFilter(desc.magFilter);
                descriptor.mipFilter = ToMetalSamplerMipFilter(desc.mipFilter);
                descriptor.sAddressMode = ToMetalSamplerAddressMode(desc.wrapU);
                descriptor.tAddressMode = ToMetalSamplerAddressMode(desc.wrapV);
                descriptor.rAddressMode = ToMetalSamplerAddressMode(desc.wrapW);
                descriptor.maxAnisotropy = (std::max)(1u, desc.maxAnisotropy);
                descriptor.lodMinClamp = desc.minLod;
                descriptor.lodMaxClamp = desc.maxLod;
                (void)desc.mipLodBias;
                descriptor.compareFunction = desc.compareEnabled
                    ? ToMetalCompareFunction(desc.compareFunc)
                    : MTLCompareFunctionNever;
                id<MTLSamplerState> sampler = [m_device newSamplerStateWithDescriptor:descriptor];
                [descriptor release];
                if (sampler == nil)
                    return nullptr;
                if (debugName.empty())
                    debugName = "MetalSampler";
                auto result = std::make_shared<MetalSampler>(sampler, desc, std::move(debugName));
                [sampler release];
                return result;
            }
            std::shared_ptr<NLS::Render::RHI::RHIBindingLayout> CreateBindingLayout(
                const NLS::Render::RHI::RHIBindingLayoutDesc& desc) override
            {
                for (const auto& entry : desc.entries)
                {
                    const auto index = entry.type == NLS::Render::RHI::BindingType::UniformBuffer ||
                            entry.type == NLS::Render::RHI::BindingType::StructuredBuffer ||
                            entry.type == NLS::Render::RHI::BindingType::StorageBuffer
                        ? ToMetalBufferBindingIndex(entry.registerSpace, entry.binding)
                        : entry.type == NLS::Render::RHI::BindingType::Sampler
                            ? ToMetalSamplerBindingIndex(entry.registerSpace, entry.binding)
                            : ToMetalTextureBindingIndex(entry.registerSpace, entry.binding);
                    if (!index.has_value())
                    {
                        NLS_LOG_ERROR("CreateMetalBindingLayout: binding exceeds Metal's supported binding range.");
                        return nullptr;
                    }
                }
                return std::make_shared<MetalBindingLayout>(desc);
            }
            std::shared_ptr<NLS::Render::RHI::RHIBindingSet> CreateBindingSet(
                const NLS::Render::RHI::RHIBindingSetDesc& desc) override
            {
                if (desc.layout == nullptr || std::dynamic_pointer_cast<MetalBindingLayout>(desc.layout) == nullptr)
                    return nullptr;
                return std::make_shared<MetalBindingSet>(desc);
            }
            std::shared_ptr<NLS::Render::RHI::RHIPipelineLayout> CreatePipelineLayout(
                const NLS::Render::RHI::RHIPipelineLayoutDesc& desc) override
            {
                for (const auto& layout : desc.bindingLayouts)
                {
                    if (layout == nullptr || std::dynamic_pointer_cast<MetalBindingLayout>(layout) == nullptr)
                        return nullptr;
                }
                return std::make_shared<MetalPipelineLayout>(desc);
            }
            std::shared_ptr<NLS::Render::RHI::RHIShaderModule> CreateShaderModule(
                const NLS::Render::RHI::RHIShaderModuleDesc& desc) override
            {
                if (m_device == nil || desc.bytecode.empty())
                    return nullptr;

                try
                {
                    std::string source;
                    std::string metalEntryPoint = desc.entryPoint;
                    uint32_t magic = 0u;
                    if (desc.bytecode.size() >= sizeof(magic))
                        std::memcpy(&magic, desc.bytecode.data(), sizeof(magic));
                    const bool isSpirv = desc.bytecode.size() % sizeof(uint32_t) == 0u && magic == 0x07230203u;
                    if (isSpirv)
                    {
                        std::vector<uint32_t> spirv(desc.bytecode.size() / sizeof(uint32_t));
                        std::memcpy(spirv.data(), desc.bytecode.data(), desc.bytecode.size());
                        spirv_cross::CompilerMSL compiler(std::move(spirv));
                        auto options = compiler.get_msl_options();
                        options.platform = spirv_cross::CompilerMSL::Options::macOS;
                        options.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2u, 4u);
                        compiler.set_msl_options(options);

                        const auto executionModel = compiler.get_execution_model();
                        const auto addBufferBindings = [&](const auto& resources)
                        {
                            for (const auto& resource : resources)
                            {
                                const uint32_t bindingSpace = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
                                const uint32_t binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
                                const auto index = ToMetalBufferBindingIndex(bindingSpace, binding);
                                if (!index.has_value())
                                    throw std::runtime_error("Metal buffer binding exceeds the supported binding map.");
                                spirv_cross::MSLResourceBinding remap{};
                                remap.stage = executionModel;
                                remap.desc_set = bindingSpace;
                                remap.binding = binding;
                                remap.msl_buffer = *index;
                                compiler.add_msl_resource_binding(remap);
                            }
                        };
                        const auto addTextureBindings = [&](const auto& resources, const bool includeSampler)
                        {
                            for (const auto& resource : resources)
                            {
                                const uint32_t bindingSpace = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
                                const uint32_t binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
                                const auto index = ToMetalTextureBindingIndex(bindingSpace, binding);
                                if (!index.has_value())
                                    throw std::runtime_error("Metal texture binding exceeds the supported binding map.");
                                const auto samplerIndex = includeSampler
                                    ? ToMetalSamplerBindingIndex(bindingSpace, binding)
                                    : std::optional<uint32_t>{};
                                if (includeSampler && !samplerIndex.has_value())
                                    throw std::runtime_error("Metal sampler binding exceeds the supported binding map.");
                                spirv_cross::MSLResourceBinding remap{};
                                remap.stage = executionModel;
                                remap.desc_set = bindingSpace;
                                remap.binding = binding;
                                remap.msl_texture = *index;
                                remap.msl_sampler = samplerIndex.value_or(0u);
                                compiler.add_msl_resource_binding(remap);
                            }
                        };
                        const auto addSamplerBindings = [&](const auto& resources)
                        {
                            for (const auto& resource : resources)
                            {
                                const uint32_t bindingSpace = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
                                const uint32_t binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
                                const auto index = ToMetalSamplerBindingIndex(bindingSpace, binding);
                                if (!index.has_value())
                                    throw std::runtime_error("Metal sampler binding exceeds the supported binding map.");
                                spirv_cross::MSLResourceBinding remap{};
                                remap.stage = executionModel;
                                remap.desc_set = bindingSpace;
                                remap.binding = binding;
                                remap.msl_sampler = *index;
                                compiler.add_msl_resource_binding(remap);
                            }
                        };

                        const auto resources = compiler.get_shader_resources();
                        addBufferBindings(resources.uniform_buffers);
                        addBufferBindings(resources.storage_buffers);
                        addTextureBindings(resources.sampled_images, true);
                        addTextureBindings(resources.separate_images, false);
                        addTextureBindings(resources.storage_images, false);
                        addSamplerBindings(resources.separate_samplers);
                        source = compiler.compile();
                        metalEntryPoint = compiler.get_cleansed_entry_point_name(desc.entryPoint, executionModel);
                    }
                    else
                    {
                        source.assign(reinterpret_cast<const char*>(desc.bytecode.data()), desc.bytecode.size());
                    }

                    NSString* sourceString = [[NSString alloc] initWithBytes:source.data()
                        length:source.size()
                        encoding:NSUTF8StringEncoding];
                    NSError* error = nil;
                    id<MTLLibrary> library = [m_device newLibraryWithSource:sourceString options:nil error:&error];
                    [sourceString release];
                    if (library == nil)
                    {
                        const char* message = error != nil ? error.localizedDescription.UTF8String : "Unknown Metal shader compiler error.";
                        NLS_LOG_ERROR("CreateMetalShaderModule: " + std::string(message != nullptr ? message : "Unknown error."));
                        return nullptr;
                    }
                    NSString* entryPoint = [NSString stringWithUTF8String:metalEntryPoint.c_str()];
                    id<MTLFunction> function = [library newFunctionWithName:entryPoint];
                    if (function == nil)
                    {
                        NLS_LOG_ERROR("CreateMetalShaderModule: MSL entry point not found: " + metalEntryPoint);
                        [library release];
                        return nullptr;
                    }
                    auto result = std::make_shared<MetalShaderModule>(library, function, desc);
                    [function release];
                    [library release];
                    return result;
                }
                catch (const std::exception& exception)
                {
                    NLS_LOG_ERROR("CreateMetalShaderModule: SPIR-V to MSL translation failed: " + std::string(exception.what()));
                    return nullptr;
                }
            }
            std::shared_ptr<NLS::Render::RHI::RHIGraphicsPipeline> CreateGraphicsPipeline(
                const NLS::Render::RHI::RHIGraphicsPipelineDesc& desc) override
            {
                const auto vertexShader = std::dynamic_pointer_cast<MetalShaderModule>(desc.vertexShader);
                const auto fragmentShader = std::dynamic_pointer_cast<MetalShaderModule>(desc.fragmentShader);
                if (m_device == nil || vertexShader == nullptr || fragmentShader == nullptr)
                    return nullptr;

                MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
                descriptor.label = [NSString stringWithUTF8String:desc.debugName.c_str()];
                descriptor.vertexFunction = vertexShader->GetFunction();
                descriptor.fragmentFunction = fragmentShader->GetFunction();
                descriptor.rasterSampleCount = (std::max)(1u, desc.renderTargetLayout.sampleCount);

                const size_t colorAttachmentCount = (std::max)(size_t{1u}, desc.renderTargetLayout.colorFormats.size());
                if (colorAttachmentCount > 8u)
                {
                    [descriptor release];
                    return nullptr;
                }
                for (size_t index = 0u; index < colorAttachmentCount; ++index)
                {
                    const auto format = index < desc.renderTargetLayout.colorFormats.size()
                        ? desc.renderTargetLayout.colorFormats[index]
                        : NLS::Render::RHI::TextureFormat::RGBA8;
                    const MTLPixelFormat pixelFormat = ToMetalPixelFormat(
                        format,
                        desc.renderTargetLayout.GetColorSpace(index));
                    if (pixelFormat == MTLPixelFormatInvalid)
                    {
                        [descriptor release];
                        return nullptr;
                    }
                    const auto& sourceBlend = index < desc.blendState.renderTargets.size()
                        ? desc.blendState.renderTargets[index]
                        : NLS::Render::RHI::RHIRenderTargetBlendStateDesc {
                            desc.blendState.enabled,
                            NLS::Render::RHI::RHIBlendFactor::SrcAlpha,
                            NLS::Render::RHI::RHIBlendFactor::InvSrcAlpha,
                            NLS::Render::RHI::RHIBlendOp::Add,
                            NLS::Render::RHI::RHIBlendFactor::One,
                            NLS::Render::RHI::RHIBlendFactor::InvSrcAlpha,
                            NLS::Render::RHI::RHIBlendOp::Add,
                            desc.blendState.colorWrite
                                ? NLS::Render::RHI::RHIColorWriteMask::All
                                : NLS::Render::RHI::RHIColorWriteMask::None
                        };
                    MTLRenderPipelineColorAttachmentDescriptor* color = descriptor.colorAttachments[index];
                    color.pixelFormat = pixelFormat;
                    color.blendingEnabled = sourceBlend.blendEnable;
                    color.sourceRGBBlendFactor = ToMetalBlendFactor(sourceBlend.srcColor);
                    color.destinationRGBBlendFactor = ToMetalBlendFactor(sourceBlend.dstColor);
                    color.rgbBlendOperation = ToMetalBlendOperation(sourceBlend.colorOp);
                    color.sourceAlphaBlendFactor = ToMetalBlendFactor(sourceBlend.srcAlpha);
                    color.destinationAlphaBlendFactor = ToMetalBlendFactor(sourceBlend.dstAlpha);
                    color.alphaBlendOperation = ToMetalBlendOperation(sourceBlend.alphaOp);
                    color.writeMask = ToMetalColorWriteMask(sourceBlend.colorWriteMask);
                }

                if (desc.renderTargetLayout.hasDepth)
                {
                    const MTLPixelFormat depthFormat = ToMetalPixelFormat(
                        desc.renderTargetLayout.depthFormat,
                        NLS::Render::RHI::TextureColorSpace::Linear);
                    if (depthFormat == MTLPixelFormatInvalid)
                    {
                        [descriptor release];
                        return nullptr;
                    }
                    descriptor.depthAttachmentPixelFormat = depthFormat;
                    if (depthFormat == MTLPixelFormatDepth24Unorm_Stencil8)
                        descriptor.stencilAttachmentPixelFormat = depthFormat;
                }

                MTLVertexDescriptor* vertexDescriptor = [[MTLVertexDescriptor alloc] init];
                for (const auto& buffer : desc.vertexBuffers)
                {
                    if (buffer.binding >= 8u)
                    {
                        [vertexDescriptor release];
                        [descriptor release];
                        return nullptr;
                    }
                    MTLVertexBufferLayoutDescriptor* layout = vertexDescriptor.layouts[buffer.binding];
                    layout.stride = buffer.stride;
                    layout.stepFunction = buffer.perInstance
                        ? MTLVertexStepFunctionPerInstance
                        : MTLVertexStepFunctionPerVertex;
                    layout.stepRate = 1u;
                }
                for (const auto& attribute : desc.vertexAttributes)
                {
                    if (attribute.location >= 31u || attribute.binding >= 8u)
                    {
                        [vertexDescriptor release];
                        [descriptor release];
                        return nullptr;
                    }
                    MTLVertexAttributeDescriptor* target = vertexDescriptor.attributes[attribute.location];
                    target.format = ToMetalVertexFormat(attribute.elementSize);
                    target.offset = attribute.offset;
                    target.bufferIndex = attribute.binding;
                }
                descriptor.vertexDescriptor = vertexDescriptor;
                [vertexDescriptor release];

                NSError* error = nil;
                id<MTLRenderPipelineState> pipelineState = [m_device newRenderPipelineStateWithDescriptor:descriptor error:&error];
                [descriptor release];
                if (pipelineState == nil)
                {
                    const char* message = error != nil ? error.localizedDescription.UTF8String : "Unknown Metal pipeline error.";
                    NLS_LOG_ERROR("CreateMetalGraphicsPipeline: " + std::string(message != nullptr ? message : "Unknown error."));
                    return nullptr;
                }

                id<MTLDepthStencilState> depthStencilState = nil;
                if (desc.renderTargetLayout.hasDepth)
                {
                    MTLDepthStencilDescriptor* depthDescriptor = [[MTLDepthStencilDescriptor alloc] init];
                    depthDescriptor.depthCompareFunction = desc.depthStencilState.depthTest
                        ? ToMetalCompareFunction(desc.depthStencilState.depthCompare)
                        : MTLCompareFunctionAlways;
                    depthDescriptor.depthWriteEnabled = desc.depthStencilState.depthWrite;
                    if (desc.depthStencilState.stencilTest)
                    {
                        MTLStencilDescriptor* stencil = [[MTLStencilDescriptor alloc] init];
                        stencil.stencilCompareFunction = ToMetalCompareFunction(desc.depthStencilState.stencilCompare);
                        stencil.stencilFailureOperation = ToMetalStencilOperation(desc.depthStencilState.stencilFailOp);
                        stencil.depthFailureOperation = ToMetalStencilOperation(desc.depthStencilState.stencilDepthFailOp);
                        stencil.depthStencilPassOperation = ToMetalStencilOperation(desc.depthStencilState.stencilPassOp);
                        stencil.readMask = desc.depthStencilState.stencilReadMask;
                        stencil.writeMask = desc.depthStencilState.stencilWriteMask;
                        depthDescriptor.frontFaceStencil = stencil;
                        depthDescriptor.backFaceStencil = stencil;
                        [stencil release];
                    }
                    depthStencilState = [m_device newDepthStencilStateWithDescriptor:depthDescriptor];
                    [depthDescriptor release];
                }
                auto result = std::make_shared<MetalGraphicsPipeline>(pipelineState, depthStencilState, desc);
                [depthStencilState release];
                [pipelineState release];
                return result;
            }
            std::shared_ptr<NLS::Render::RHI::RHIComputePipeline> CreateComputePipeline(
                const NLS::Render::RHI::RHIComputePipelineDesc&) override { return nullptr; }
            std::shared_ptr<NLS::Render::RHI::RHICommandPool> CreateCommandPool(
                NLS::Render::RHI::QueueType queueType, std::string debugName) override
            {
                return std::make_shared<MetalCommandPool>(m_graphicsQueue, queueType,
                    debugName.empty() ? "MetalCommandPool" : std::move(debugName));
            }
            std::shared_ptr<NLS::Render::RHI::RHIFence> CreateFence(std::string debugName) override
            {
                return std::make_shared<MetalFence>(debugName.empty() ? "MetalFence" : std::move(debugName));
            }
            std::shared_ptr<NLS::Render::RHI::RHISemaphore> CreateSemaphore(std::string debugName) override
            {
                return std::make_shared<MetalSemaphore>(debugName.empty() ? "MetalSemaphore" : std::move(debugName));
            }
            void ReadPixels(
                const std::shared_ptr<NLS::Render::RHI::RHITexture>&,
                uint32_t,
                uint32_t,
                uint32_t,
                uint32_t,
                NLS::Render::Settings::EPixelDataFormat,
                NLS::Render::Settings::EPixelDataType,
                void*) override
            {
            }
            NLS::Render::RHI::RHIReadbackResult ReadPixelsChecked(
                const std::shared_ptr<NLS::Render::RHI::RHITexture>&,
                uint32_t,
                uint32_t,
                uint32_t,
                uint32_t,
                NLS::Render::Settings::EPixelDataFormat,
                NLS::Render::Settings::EPixelDataType,
                void*) override
            {
                return {
                    NLS::Render::RHI::RHIReadbackStatusCode::UnsupportedFormat,
                    "Metal pixel readback is not implemented in the Launcher UI milestone."
                };
            }

        private:
            static NLS::Render::RHI::RHIDeviceCapabilities CreateCapabilities()
            {
                NLS::Render::RHI::RHIDeviceCapabilities capabilities{};
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::BackendReady, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::Graphics, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::Compute, false,
                    "Metal compute command encoding is not implemented yet");
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::Swapchain, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::UITextureHandles, true);
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::CurrentSceneRenderer, false,
                    "Metal graphics command encoding is available, but the deferred scene renderer still requires compute, readback, and full texture topology support");
                capabilities.SetFeature(NLS::Render::RHI::RHIDeviceFeature::UIOverlayFrameGraph, false,
                    "Metal Launcher UI uses the direct ImGui bridge while FrameGraph UI encoding is unfinished");
                capabilities.maxTextureDimension2D = 16384u;
                capabilities.maxColorAttachments = 8u;

                for (const auto format : {
                    NLS::Render::RHI::TextureFormat::R8,
                    NLS::Render::RHI::TextureFormat::RG8,
                    NLS::Render::RHI::TextureFormat::RGBA8,
                    NLS::Render::RHI::TextureFormat::R16F,
                    NLS::Render::RHI::TextureFormat::RG16F,
                    NLS::Render::RHI::TextureFormat::RGBA16F,
                    NLS::Render::RHI::TextureFormat::R32F,
                    NLS::Render::RHI::TextureFormat::RG32F,
                    NLS::Render::RHI::TextureFormat::RGBA32F,
                    NLS::Render::RHI::TextureFormat::Depth32F,
                    NLS::Render::RHI::TextureFormat::Depth24Stencil8 })
                {
                    NLS::Render::RHI::TextureFormatCapability formatCapability{};
                    formatCapability.sampled = true;
                    formatCapability.upload = true;
                    formatCapability.colorAttachment =
                        format != NLS::Render::RHI::TextureFormat::Depth32F &&
                        format != NLS::Render::RHI::TextureFormat::Depth24Stencil8;
                    capabilities.SetTextureFormatCapability(format, formatCapability);
                }
                return capabilities;
            }

            id<MTLDevice> m_device = nil;
            id<MTLCommandQueue> m_graphicsQueue = nil;
            std::shared_ptr<NLS::Render::RHI::RHIAdapter> m_adapter;
            NLS::Render::RHI::RHIDeviceCapabilities m_capabilities{};
            std::array<std::shared_ptr<NLS::Render::RHI::RHIQueue>, 3u> m_queues{};
            void* m_swapchainLayer = nullptr;
            void* m_platformWindow = nullptr;
            void* m_nativeWindowHandle = nullptr;
        };
    }

    std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateMetalRhiDevice(const bool debugMode)
    {
        (void)debugMode;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil)
        {
            NLS_LOG_ERROR("CreateMetalRhiDevice: MTLCreateSystemDefaultDevice returned null.");
            return nullptr;
        }

        auto result = std::make_shared<MetalDevice>(device);
        if (!result->IsBackendReady())
        {
            NLS_LOG_ERROR("CreateMetalRhiDevice: failed to create a Metal command queue.");
            return nullptr;
        }
        NLS_LOG_INFO("CreateMetalRhiDevice: created Metal device " + std::string(device.name.UTF8String));
        return result;
    }
}

#pragma once

#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/RHI/Core/RHISync.h"

namespace NLS::Render::RHI
{
    enum class RHIUpdateStatusCode : uint8_t
    {
        Success,
        InvalidArgument,
        Unsupported,
        BackendFailure
    };

    struct NLS_RENDER_API RHIUpdateResult
    {
        RHIUpdateStatusCode code = RHIUpdateStatusCode::Unsupported;
        std::string message;
        std::shared_ptr<RHICompletionToken> completion;

        bool Succeeded() const { return code == RHIUpdateStatusCode::Success; }
    };

    struct NLS_RENDER_API RHIBufferDesc
    {
        size_t size = 0;
        BufferUsageFlags usage = BufferUsageFlags::None;
        MemoryUsage memoryUsage = MemoryUsage::GPUOnly;
        std::string debugName;
    };

    struct NLS_RENDER_API RHIBufferUploadDesc
    {
        const void* data = nullptr;
        size_t dataSize = 0u;
        uint64_t destinationOffset = 0u;
        std::string debugName;

        bool HasData() const { return data != nullptr && dataSize != 0u; }
    };

    struct NLS_RENDER_API RHITextureDesc
    {
        struct OptimizedClearValue
        {
            bool enabled = false;
            float color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            float depth = 1.0f;
            uint32_t stencil = 0u;
        };

        RHIExtent3D extent{};
        TextureDimension dimension = TextureDimension::Texture2D;
        TextureFormat format = TextureFormat::RGBA8;
        uint32_t mipLevels = 1;
        uint32_t arrayLayers = 1;
        uint32_t sampleCount = 1;
        TextureUsageFlags usage = TextureUsageFlags::Sampled;
        MemoryUsage memoryUsage = MemoryUsage::GPUOnly;
        OptimizedClearValue optimizedClearValue{};
        std::string debugName;
    };

    struct NLS_RENDER_API RHITextureUploadDesc
    {
        const void* data = nullptr;
        size_t dataSize = 0u;
        uint32_t mipLevel = 0u;
        uint32_t arrayLayer = 0u;
        RHIExtent3D extent{};
        uint32_t rowPitch = 0u;
        uint32_t slicePitch = 0u;
        std::string debugName;

        bool HasData() const { return data != nullptr && dataSize != 0u; }
    };

    struct NLS_RENDER_API RHITextureUpdateDesc
    {
        std::shared_ptr<class RHITexture> texture;
        const void* data = nullptr;
        size_t dataSize = 0u;
        uint32_t mipLevel = 0u;
        uint32_t arrayLayer = 0u;
        uint32_t x = 0u;
        uint32_t y = 0u;
        uint32_t z = 0u;
        RHIExtent3D extent{};
        uint32_t rowPitch = 0u;
        uint32_t slicePitch = 0u;
        std::string debugName;
    };

    struct NLS_RENDER_API RHITextureViewDesc
    {
        TextureViewType viewType = TextureViewType::Auto;
        TextureFormat format = TextureFormat::RGBA8;
        RHISubresourceRange subresourceRange{};
        std::string debugName;
    };

    struct NLS_RENDER_API RHIVertexBufferView
    {
        std::shared_ptr<class RHIBuffer> buffer;
        uint64_t offset = 0;
        uint32_t stride = 0;
    };

    struct NLS_RENDER_API RHIIndexBufferView
    {
        std::shared_ptr<class RHIBuffer> buffer;
        uint64_t offset = 0;
        IndexType indexType = IndexType::UInt32;
    };

    class NLS_RENDER_API RHIBuffer : public RHIObject
    {
    public:
        virtual const RHIBufferDesc& GetDesc() const = 0;
        virtual ResourceState GetState() const = 0;
        virtual uint64_t GetGPUAddress() const = 0;
        virtual RHIUpdateResult UpdateData(const RHIBufferUploadDesc& uploadDesc)
        {
            (void)uploadDesc;
            return { RHIUpdateStatusCode::Unsupported, "RHI buffer does not support in-place data updates" };
        }
        virtual NativeHandle GetNativeBufferHandle() { return {}; } // Type-safe native handle
    };

    class NLS_RENDER_API RHITexture : public RHIObject
    {
    public:
        virtual const RHITextureDesc& GetDesc() const = 0;
        virtual ResourceState GetState() const = 0;
        virtual NativeHandle GetNativeImageHandle() { return {}; } // Type-safe native handle
    };

    class NLS_RENDER_API RHITextureView : public RHIObject
    {
    public:
        virtual const RHITextureViewDesc& GetDesc() const = 0;
        virtual const std::shared_ptr<RHITexture>& GetTexture() const = 0;
        virtual NativeHandle GetNativeRenderTargetView() { return {}; } // Type-safe native handle
        virtual NativeHandle GetNativeDepthStencilView() { return {}; } // Type-safe native handle
        virtual NativeHandle GetNativeShaderResourceView() { return {}; } // Type-safe native handle
    };

    class NLS_RENDER_API RHISampler : public RHIObject
    {
    public:
        virtual const SamplerDesc& GetDesc() const = 0;
        virtual NativeHandle GetNativeSamplerHandle() { return {}; } // Type-safe native handle
    };
}

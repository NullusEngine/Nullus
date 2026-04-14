#pragma once

#include "Rendering/RHI/Core/RHIEnums.h"

namespace NLS::Render::RHI
{
    struct NLS_RENDER_API RHIBufferDesc
    {
        size_t size = 0;
        BufferUsageFlags usage = BufferUsageFlags::None;
        MemoryUsage memoryUsage = MemoryUsage::GPUOnly;
        std::string debugName;
    };

    struct NLS_RENDER_API RHITextureDesc
    {
        RHIExtent3D extent{};
        TextureDimension dimension = TextureDimension::Texture2D;
        TextureFormat format = TextureFormat::RGBA8;
        uint32_t mipLevels = 1;
        uint32_t arrayLayers = 1;
        uint32_t sampleCount = 1;
        TextureUsageFlags usage = TextureUsageFlags::Sampled;
        MemoryUsage memoryUsage = MemoryUsage::GPUOnly;
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

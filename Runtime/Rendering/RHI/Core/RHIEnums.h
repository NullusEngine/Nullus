#pragma once

#include "Rendering/RHI/Core/RHICommon.h"
#include "Rendering/Settings/EComparaisonAlgorithm.h"
#include "Rendering/Settings/ECullFace.h"

namespace NLS::Render::RHI
{
    enum class NLS_RENDER_API QueueType : uint8_t
    {
        Graphics,
        Compute,
        Copy
    };

    enum class NLS_RENDER_API MemoryUsage : uint8_t
    {
        GPUOnly,
        CPUToGPU,
        GPUToCPU
    };

    enum class NLS_RENDER_API ShaderStageMask : uint32_t
    {
        None = 0,
        Vertex = 1u << 0,
        Fragment = 1u << 1,
        Compute = 1u << 2,
        AllGraphics = Vertex | Fragment,
        All = AllGraphics | Compute
    };

    inline constexpr ShaderStageMask operator|(ShaderStageMask lhs, ShaderStageMask rhs)
    {
        return static_cast<ShaderStageMask>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    inline constexpr ShaderStageMask operator&(ShaderStageMask lhs, ShaderStageMask rhs)
    {
        return static_cast<ShaderStageMask>(static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
    }

    inline constexpr bool HasShaderStage(ShaderStageMask mask, ShaderStageMask flag)
    {
        return (static_cast<uint32_t>(mask) & static_cast<uint32_t>(flag)) != 0u;
    }

    enum class NLS_RENDER_API BufferUsageFlags : uint32_t
    {
        None = 0,
        CopySrc = 1u << 0,
        CopyDst = 1u << 1,
        Vertex = 1u << 2,
        Index = 1u << 3,
        Uniform = 1u << 4,
        Storage = 1u << 5,
        Indirect = 1u << 6
    };

    inline constexpr BufferUsageFlags operator|(BufferUsageFlags lhs, BufferUsageFlags rhs)
    {
        return static_cast<BufferUsageFlags>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    inline constexpr bool HasBufferUsage(BufferUsageFlags usage, BufferUsageFlags flag)
    {
        return (static_cast<uint32_t>(usage) & static_cast<uint32_t>(flag)) != 0u;
    }

    enum class NLS_RENDER_API TextureUsageFlags : uint32_t
    {
        None = 0,
        CopySrc = 1u << 0,
        CopyDst = 1u << 1,
        Sampled = 1u << 2,
        Storage = 1u << 3,
        ColorAttachment = 1u << 4,
        DepthStencilAttachment = 1u << 5,
        Present = 1u << 6
    };

    inline constexpr TextureUsageFlags operator|(TextureUsageFlags lhs, TextureUsageFlags rhs)
    {
        return static_cast<TextureUsageFlags>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    inline constexpr bool HasTextureUsage(TextureUsageFlags usage, TextureUsageFlags flag)
    {
        return (static_cast<uint32_t>(usage) & static_cast<uint32_t>(flag)) != 0u;
    }

    enum class NLS_RENDER_API PipelineStageMask : uint32_t
    {
        None = 0,
        Copy = 1u << 0,
        VertexInput = 1u << 1,
        VertexShader = 1u << 2,
        FragmentShader = 1u << 3,
        ComputeShader = 1u << 4,
        RenderTarget = 1u << 5,
        DepthStencil = 1u << 6,
        Present = 1u << 7,
        Host = 1u << 8,
        AllGraphics = VertexInput | VertexShader | FragmentShader | RenderTarget | DepthStencil,
        AllCommands = 0xFFFFFFFFu
    };

    inline constexpr PipelineStageMask operator|(PipelineStageMask lhs, PipelineStageMask rhs)
    {
        return static_cast<PipelineStageMask>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    enum class NLS_RENDER_API AccessMask : uint32_t
    {
        None = 0,
        CopyRead = 1u << 0,
        CopyWrite = 1u << 1,
        VertexRead = 1u << 2,
        IndexRead = 1u << 3,
        UniformRead = 1u << 4,
        ShaderRead = 1u << 5,
        ShaderWrite = 1u << 6,
        ColorAttachmentRead = 1u << 7,
        ColorAttachmentWrite = 1u << 8,
        DepthStencilRead = 1u << 9,
        DepthStencilWrite = 1u << 10,
        HostRead = 1u << 11,
        HostWrite = 1u << 12,
        Present = 1u << 13,
        MemoryRead = 1u << 14,
        MemoryWrite = 1u << 15
    };

    inline constexpr AccessMask operator|(AccessMask lhs, AccessMask rhs)
    {
        return static_cast<AccessMask>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    enum class NLS_RENDER_API ResourceState : uint32_t
    {
        Unknown = 0,
        CopySrc = 1u << 0,
        CopyDst = 1u << 1,
        VertexBuffer = 1u << 2,
        IndexBuffer = 1u << 3,
        UniformBuffer = 1u << 4,
        ShaderRead = 1u << 5,
        ShaderWrite = 1u << 6,
        RenderTarget = 1u << 7,
        DepthRead = 1u << 8,
        DepthWrite = 1u << 9,
        Present = 1u << 10
    };

    enum class NLS_RENDER_API TextureViewType : uint8_t
    {
        Auto,
        Texture2D,
        Texture2DArray,
        Cube,
        CubeArray
    };

    enum class NLS_RENDER_API BindingType : uint8_t
    {
        UniformBuffer,
        StorageBuffer,
        Texture,
        RWTexture,
        Sampler
    };

    enum class NLS_RENDER_API PrimitiveTopology : uint8_t
    {
        TriangleList,
        LineList,
        PointList
    };

    enum class NLS_RENDER_API IndexType : uint8_t
    {
        UInt16,
        UInt32
    };

    enum class NLS_RENDER_API LoadOp : uint8_t
    {
        Load,
        Clear,
        DontCare
    };

    enum class NLS_RENDER_API StoreOp : uint8_t
    {
        Store,
        DontCare
    };

    struct NLS_RENDER_API RHIExtent3D
    {
        uint32_t width = 1;
        uint32_t height = 1;
        uint32_t depth = 1;
    };

    struct NLS_RENDER_API RHIOffset3D
    {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
    };

    struct NLS_RENDER_API RHISubresourceRange
    {
        uint32_t baseMipLevel = 0;
        uint32_t mipLevelCount = 1;
        uint32_t baseArrayLayer = 0;
        uint32_t arrayLayerCount = 1;
    };
}

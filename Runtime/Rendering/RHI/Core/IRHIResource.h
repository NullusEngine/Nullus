#pragma once

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHICommon.h"
#include "Rendering/RHI/RHITypes.h"
#include "Rendering/RHI/Core/RHIEnums.h"
#include "Rendering/RHI/Core/RHIResource.h"

#include <cstdint>

namespace NLS::Render::RHI
{
    /**
     * Unified resource type enum for all RHI resources.
     */
    enum class NLS_RENDER_API EResourceType : uint8_t
    {
        Buffer = 0,
        Texture1D,
        Texture2D,
        Texture3D,
        TextureCube,
        Texture2DArray,
        TextureCubeArray
    };

    /**
     * Pixel format enum - mirrors TextureFormat for type-safe graphics formats.
     * Note: This is an alias for TextureFormat to maintain naming consistency with the unified interface.
     */
    using EPixelFormat = TextureFormat;

    /**
     * Resource creation and tracking flags.
     */
    enum class NLS_RENDER_API EResourceFlags : uint32_t
    {
        None = 0,
        Shared = 1u << 0,      // Resource is shared across backends or processes
        Sparse = 1u << 1,     // Resource uses sparse binding
        Transient = 1u << 2   // Resource is short-lived and optimally placed in fast memory
    };

    inline constexpr EResourceFlags operator|(EResourceFlags lhs, EResourceFlags rhs)
    {
        return static_cast<EResourceFlags>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    inline constexpr bool HasResourceFlags(EResourceFlags flags, EResourceFlags flag)
    {
        return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0u;
    }

    /**
     * Unified resource descriptor covering all resource types.
     * This provides a single descriptor structure that can describe buffers and textures
     * in a backend-agnostic way.
     */
    struct NLS_RENDER_API ResourceDesc
    {
        // Resource identification
        EResourceType type = EResourceType::Buffer;
        std::string debugName;

        // Buffer-specific fields (valid when type == EResourceType::Buffer)
        uint64_t size = 0;

        // Texture-specific fields (valid when type != EResourceType::Buffer)
        uint32_t width = 0;
        uint32_t height = 1;
        uint32_t depth = 1;
        uint32_t arraySize = 1;
        uint32_t mipLevels = 1;
        EPixelFormat format = EPixelFormat::RGBA8;

        // Resource flags
        EResourceFlags flags = EResourceFlags::None;

        // Initial resource state
        ResourceState initialState = ResourceState::Unknown;

        // Usage hints (backing enum values from existing BufferUsageFlags/TextureUsageFlags)
        uint32_t usageFlags = 0;
    };

    /**
     * Base interface for all RHI resources.
     * Provides common functionality for state management and native handle access.
     */
    class NLS_RENDER_API IRHIResource : public RHIObject
    {
    public:
        virtual ~IRHIResource() = default;

        /**
         * Get the full descriptor for this resource.
         */
        virtual ResourceDesc GetDesc() const = 0;

        /**
         * Get the current state of this resource.
         */
        virtual ResourceState GetState() const = 0;

        /**
         * Set the resource to a new state.
         */
        virtual void SetState(ResourceState state) = 0;

        /**
         * Get the native handle for this resource.
         * Returns an invalid handle if the resource is not currently bound to a native object.
         */
        virtual NativeHandle GetNativeHandle() = 0;

        /**
         * Check if this resource is valid and usable.
         */
        virtual bool IsValid() const = 0;
    };

    /**
     * Interface for buffer resources.
     * Provides buffer-specific operations like sizing, GPU addressing, and CPU mapping.
     */
    class NLS_RENDER_API IRHIBuffer : public IRHIResource
    {
    public:
        ~IRHIBuffer() override = default;

        /**
         * Get the size of this buffer in bytes.
         */
        virtual uint64_t GetSize() const = 0;

        /**
         * Get the GPU virtual address of this buffer.
         * Returns 0 if the buffer does not support address queries.
         */
        virtual uint64_t GetGPUAddress() const = 0;

        /**
         * Map the buffer into CPU accessible memory.
         * Returns nullptr if mapping is not supported.
         */
        virtual void* Map() = 0;

        /**
         * Unmap the buffer from CPU accessible memory.
         */
        virtual void Unmap() = 0;
    };

    /**
     * Interface for texture resources.
     * Provides texture-specific operations like dimension queries and format access.
     */
    class NLS_RENDER_API IRHITexture : public IRHIResource
    {
    public:
        ~IRHITexture() override = default;

        /**
         * Get the width of this texture.
         */
        virtual uint32_t GetWidth() const = 0;

        /**
         * Get the height of this texture.
         */
        virtual uint32_t GetHeight() const = 0;

        /**
         * Get the depth of this texture (1 for 2D textures).
         */
        virtual uint32_t GetDepth() const = 0;

        /**
         * Get the number of mip levels in this texture.
         */
        virtual uint32_t GetMipLevels() const = 0;

        /**
         * Get the pixel format of this texture.
         */
        virtual EPixelFormat GetFormat() const = 0;
    };

    // ========================================================================
    // Helper functions for safe downcasting
    // ========================================================================

    /**
     * Safely cast an RHIBuffer to IRHIResource.
     * Returns nullptr if the buffer does not implement IRHIResource.
     */
    inline IRHIResource* AsIRHIResource(RHIBuffer* buffer)
    {
        return dynamic_cast<IRHIResource*>(buffer);
    }

    /**
     * Safely cast an RHITexture to IRHIResource.
     * Returns nullptr if the texture does not implement IRHIResource.
     */
    inline IRHIResource* AsIRHIResource(RHITexture* texture)
    {
        return dynamic_cast<IRHIResource*>(texture);
    }

    /**
     * Const versions of the helper cast functions.
     */
    inline const IRHIResource* AsIRHIResource(const RHIBuffer* buffer)
    {
        return dynamic_cast<const IRHIResource*>(buffer);
    }

    inline const IRHIResource* AsIRHIResource(const RHITexture* texture)
    {
        return dynamic_cast<const IRHIResource*>(texture);
    }

    /**
     * Helper to check if a resource is a buffer type.
     */
    inline bool IsBuffer(const IRHIResource* resource)
    {
        return dynamic_cast<const IRHIBuffer*>(resource) != nullptr;
    }

    /**
     * Helper to check if a resource is a texture type.
     */
    inline bool IsTexture(const IRHIResource* resource)
    {
        return dynamic_cast<const IRHITexture*>(resource) != nullptr;
    }
} // namespace NLS::Render::RHI
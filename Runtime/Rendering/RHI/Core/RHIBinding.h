#pragma once

#include "Rendering/RHI/Core/RHIEnums.h"

namespace NLS::Render::RHI
{
    class RHIBuffer;
    class RHISampler;
    class RHITextureView;

    struct NLS_RENDER_API RHIBindingLayoutEntry
    {
        std::string name;
        BindingType type = BindingType::UniformBuffer;
        uint32_t set = 0;
        uint32_t binding = 0;
        uint32_t count = 1;
        ShaderStageMask stageMask = ShaderStageMask::AllGraphics;
        uint32_t registerSpace = 0;
    };

    struct NLS_RENDER_API RHIBindingLayoutDesc
    {
        std::vector<RHIBindingLayoutEntry> entries;
        std::string debugName;
    };

    struct NLS_RENDER_API RHIBindingSetEntry
    {
        uint32_t binding = 0;
        BindingType type = BindingType::UniformBuffer;
        std::shared_ptr<class RHIBuffer> buffer;
        uint64_t bufferOffset = 0;
        uint64_t bufferRange = 0;
        std::shared_ptr<class RHITextureView> textureView;
        std::shared_ptr<class RHISampler> sampler;
    };

    struct NLS_RENDER_API RHIBindingSetDesc
    {
        std::shared_ptr<class RHIBindingLayout> layout;
        std::vector<RHIBindingSetEntry> entries;
        std::string debugName;
    };

    class NLS_RENDER_API RHIBindingLayout : public RHIObject
    {
    public:
        virtual const RHIBindingLayoutDesc& GetDesc() const = 0;
    };

    class NLS_RENDER_API RHIBindingSet : public RHIObject
    {
    public:
        virtual const RHIBindingSetDesc& GetDesc() const = 0;
        virtual NativeHandle GetNativeBindingSetHandle() const { return {}; }
        virtual std::shared_ptr<RHIBindingSet> GetWrappedBindingSetShared() { return nullptr; }
        virtual std::shared_ptr<const RHIBindingSet> GetWrappedBindingSetShared() const { return nullptr; }
    };
}

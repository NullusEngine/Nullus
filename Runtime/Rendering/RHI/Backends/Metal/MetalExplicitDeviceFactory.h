// Runtime/Rendering/RHI/Backends/Metal/MetalExplicitDeviceFactory.h
#pragma once

#include <memory>
#include <string>

#include "RenderDef.h"
#include "Rendering/RHI/RHITypes.h"

#if defined(__APPLE__)
#import <Metal/Metal.h>
#endif

namespace NLS::Render::RHI
{
    class RHIDevice;
}

namespace NLS::Render::Backend
{
#if defined(__APPLE__)
    // Typed NativeHandle variants for Metal backend
    struct MetalBufferHandle
    {
        id<MTLBuffer> buffer = nil;
    };

    struct MetalTextureHandle
    {
        id<MTLTexture> texture = nil;
    };

    struct MetalSamplerHandle
    {
        id<MTLSamplerState> sampler = nil;
    };
#endif

    // Creates a Metal Tier A device (stub implementation - Metal only available on macOS/iOS)
    NLS_RENDER_API std::shared_ptr<NLS::Render::RHI::RHIDevice> CreateMetalRhiDevice(void* platformWindow);
}

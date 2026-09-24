#pragma once

#include "RenderDef.h"
#include "Rendering/RHI/Core/RHIResource.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Render::RHI
{
class RHITexture;
}

namespace NLS::Render::Context
{
struct NLS_RENDER_API TextureRuntimeUploadMip
{
    uint32_t level = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t rowPitch = 0u;
    uint32_t slicePitch = 0u;
    std::vector<uint8_t> pixels;
};

struct NLS_RENDER_API TextureRuntimeUploadRequest
{
    RHI::RHITextureDesc desc;
    std::vector<TextureRuntimeUploadMip> mips;
    std::string debugName;

    size_t ByteSize() const
    {
        size_t size = 0u;
        for (const auto& mip : mips)
            size += mip.pixels.size();
        return size;
    }
};

struct NLS_RENDER_API TextureRuntimeUploadResult
{
    bool ready = false;
    bool success = false;
    std::shared_ptr<RHI::RHITexture> texture;
    std::string diagnostic;
};
}

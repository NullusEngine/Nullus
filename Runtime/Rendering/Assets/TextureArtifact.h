#pragma once

#include "RenderDef.h"
#include "Rendering/RHI/RHITypes.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace NLS::Render::Assets
{
enum class TextureArtifactColorSpace : uint8_t
{
    Linear = 0,
    Srgb = 1
};

struct TextureArtifactMip
{
    uint32_t level = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t rowPitch = 0u;
    uint32_t slicePitch = 0u;
    std::vector<uint8_t> pixels;
};

struct TextureArtifactData
{
    uint32_t width = 0u;
    uint32_t height = 0u;
    RHI::TextureFormat format = RHI::TextureFormat::RGBA8;
    TextureArtifactColorSpace colorSpace = TextureArtifactColorSpace::Linear;
    std::vector<TextureArtifactMip> mips;
};

NLS_RENDER_API std::vector<uint8_t> SerializeTextureArtifact(const TextureArtifactData& texture);
NLS_RENDER_API std::optional<TextureArtifactData> DeserializeTextureArtifact(const std::vector<uint8_t>& bytes);
NLS_RENDER_API std::optional<TextureArtifactData> LoadTextureArtifact(const std::filesystem::path& path);
NLS_RENDER_API bool IsNativeTextureArtifact(const std::vector<uint8_t>& bytes);
NLS_RENDER_API std::optional<TextureArtifactData> DecodeTextureArtifactFromEncodedImage(
    const uint8_t* encodedData,
    size_t encodedDataSize,
    TextureArtifactColorSpace colorSpace,
    bool flipVertically);
}

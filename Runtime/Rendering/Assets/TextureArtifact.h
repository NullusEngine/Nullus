#pragma once

#include "RenderDef.h"
#include "Rendering/RHI/RHITypes.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Render::Assets
{
struct TextureMipGeneratorSettings;

enum class TextureArtifactColorSpace : uint8_t
{
    Linear = 0,
    Srgb = 1
};

enum class TextureArtifactCubeFace : uint8_t
{
    None = 0,
    PositiveX,
    NegativeX,
    PositiveY,
    NegativeY,
    PositiveZ,
    NegativeZ
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

struct TextureArtifactSubresource
{
    uint32_t level = 0u;
    uint32_t arrayLayer = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t depth = 1u;
    TextureArtifactCubeFace face = TextureArtifactCubeFace::None;
    uint32_t rowPitch = 0u;
    uint32_t slicePitch = 0u;
    uint64_t dataOffset = 0u;
    uint64_t dataSize = 0u;
};

struct TextureArtifactData
{
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t depth = 1u;
    RHI::TextureDimension dimension = RHI::TextureDimension::Texture2D;
    uint32_t arrayLayers = 1u;
    RHI::TextureFormat format = RHI::TextureFormat::RGBA8;
    TextureArtifactColorSpace colorSpace = TextureArtifactColorSpace::Linear;
    std::string targetPlatform;
    std::string buildIdentity;
    std::string encoderId;
    uint32_t encoderVersion = 0u;
    std::vector<TextureArtifactMip> mips;
    std::vector<TextureArtifactSubresource> subresources;
};

NLS_RENDER_API std::vector<uint8_t> SerializeTextureArtifact(const TextureArtifactData& texture);
NLS_RENDER_API std::optional<TextureArtifactData> DeserializeTextureArtifact(const std::vector<uint8_t>& bytes);
NLS_RENDER_API std::optional<TextureArtifactData> LoadTextureArtifact(const std::filesystem::path& path);
NLS_RENDER_API std::optional<TextureArtifactData> LoadTextureArtifact(
    const std::filesystem::path& path,
    const std::atomic_bool* cancellationFlag);
NLS_RENDER_API bool IsNativeTextureArtifact(const std::vector<uint8_t>& bytes);
NLS_RENDER_API std::optional<TextureArtifactData> DecodeTextureArtifactFromEncodedImage(
    const uint8_t* encodedData,
    size_t encodedDataSize,
    TextureArtifactColorSpace colorSpace,
    bool flipVertically);
NLS_RENDER_API std::optional<TextureArtifactData> DecodeTextureArtifactFromEncodedImage(
    const uint8_t* encodedData,
    size_t encodedDataSize,
    const TextureMipGeneratorSettings& settings,
    bool flipVertically);
}

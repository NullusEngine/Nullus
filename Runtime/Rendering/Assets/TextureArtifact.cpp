#include "Rendering/Assets/TextureArtifact.h"
#include "Assets/NativeArtifactContainer.h"

#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define stbi__tga_read_rgb16 nls_texture_artifact_stbi__tga_read_rgb16
#include <stb/stb_image.h>
#undef stbi__tga_read_rgb16

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

namespace NLS::Render::Assets
{
namespace
{
constexpr uint32_t kTextureArtifactMagic = 0x5845544Eu; // "NTEX" little-endian.
constexpr uint32_t kTextureArtifactVersion = 2u;
constexpr uint32_t kTextureArtifactContainerSchemaVersion = 3u;
constexpr uint64_t kTextureArtifactHeaderBytes = 32u;
constexpr uint64_t kTextureArtifactMipRecordBytes = 36u;

struct TextureArtifactHeader
{
    uint32_t magic = kTextureArtifactMagic;
    uint32_t version = kTextureArtifactVersion;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t format = static_cast<uint32_t>(RHI::TextureFormat::RGBA8);
    uint32_t colorSpace = static_cast<uint32_t>(TextureArtifactColorSpace::Linear);
    uint32_t mipCount = 0u;
    uint32_t reserved = 0u;
};

struct TextureArtifactMipRecord
{
    uint32_t level = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t rowPitch = 0u;
    uint32_t slicePitch = 0u;
    uint64_t dataOffset = 0u;
    uint64_t dataSize = 0u;
};

static_assert(sizeof(TextureArtifactHeader) == kTextureArtifactHeaderBytes);

bool IsSupportedTextureFormat(const RHI::TextureFormat format)
{
    switch (format)
    {
    case RHI::TextureFormat::R8:
    case RHI::TextureFormat::RG8:
    case RHI::TextureFormat::RGB8:
    case RHI::TextureFormat::RGBA8:
    case RHI::TextureFormat::R16F:
    case RHI::TextureFormat::RG16F:
    case RHI::TextureFormat::RGBA16F:
    case RHI::TextureFormat::R32F:
    case RHI::TextureFormat::RG32F:
    case RHI::TextureFormat::RGBA32F:
        return true;
    case RHI::TextureFormat::Depth32F:
    case RHI::TextureFormat::Depth24Stencil8:
    default:
        return false;
    }
}

bool IsSupportedColorSpace(const TextureArtifactColorSpace colorSpace)
{
    return colorSpace == TextureArtifactColorSpace::Linear ||
        colorSpace == TextureArtifactColorSpace::Srgb;
}

void AppendUInt32(std::vector<uint8_t>& bytes, const uint32_t value)
{
    bytes.push_back(static_cast<uint8_t>(value & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 16u) & 0xFFu));
    bytes.push_back(static_cast<uint8_t>((value >> 24u) & 0xFFu));
}

void AppendUInt64(std::vector<uint8_t>& bytes, const uint64_t value)
{
    for (uint32_t byteIndex = 0u; byteIndex < 8u; ++byteIndex)
        bytes.push_back(static_cast<uint8_t>((value >> (byteIndex * 8u)) & 0xFFu));
}

void AppendHeader(std::vector<uint8_t>& bytes, const TextureArtifactHeader& header)
{
    AppendUInt32(bytes, header.magic);
    AppendUInt32(bytes, header.version);
    AppendUInt32(bytes, header.width);
    AppendUInt32(bytes, header.height);
    AppendUInt32(bytes, header.format);
    AppendUInt32(bytes, header.colorSpace);
    AppendUInt32(bytes, header.mipCount);
    AppendUInt32(bytes, header.reserved);
}

void AppendMipRecord(std::vector<uint8_t>& bytes, const TextureArtifactMipRecord& record)
{
    AppendUInt32(bytes, record.level);
    AppendUInt32(bytes, record.width);
    AppendUInt32(bytes, record.height);
    AppendUInt32(bytes, record.rowPitch);
    AppendUInt32(bytes, record.slicePitch);
    AppendUInt64(bytes, record.dataOffset);
    AppendUInt64(bytes, record.dataSize);
}

bool ReadUInt32(const std::vector<uint8_t>& bytes, size_t& offset, uint32_t& value)
{
    if (offset + sizeof(uint32_t) > bytes.size())
        return false;

    value =
        static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
        (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
        (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
    offset += sizeof(uint32_t);
    return true;
}

bool ReadUInt64(const std::vector<uint8_t>& bytes, size_t& offset, uint64_t& value)
{
    if (offset + sizeof(uint64_t) > bytes.size())
        return false;

    value = 0u;
    for (uint32_t byteIndex = 0u; byteIndex < 8u; ++byteIndex)
        value |= static_cast<uint64_t>(bytes[offset + byteIndex]) << (byteIndex * 8u);
    offset += sizeof(uint64_t);
    return true;
}

bool ReadHeader(const std::vector<uint8_t>& bytes, TextureArtifactHeader& header)
{
    if (bytes.size() < kTextureArtifactHeaderBytes)
        return false;

    size_t offset = 0u;
    if (!ReadUInt32(bytes, offset, header.magic) ||
        !ReadUInt32(bytes, offset, header.version) ||
        !ReadUInt32(bytes, offset, header.width) ||
        !ReadUInt32(bytes, offset, header.height) ||
        !ReadUInt32(bytes, offset, header.format) ||
        !ReadUInt32(bytes, offset, header.colorSpace) ||
        !ReadUInt32(bytes, offset, header.mipCount) ||
        !ReadUInt32(bytes, offset, header.reserved))
    {
        return false;
    }

    if (header.magic != kTextureArtifactMagic ||
        header.version != kTextureArtifactVersion ||
        header.width == 0u ||
        header.height == 0u ||
        header.mipCount == 0u)
    {
        return false;
    }

    const auto format = static_cast<RHI::TextureFormat>(header.format);
    const auto colorSpace = static_cast<TextureArtifactColorSpace>(header.colorSpace);
    return IsSupportedTextureFormat(format) && IsSupportedColorSpace(colorSpace);
}

bool ReadMipRecord(const std::vector<uint8_t>& bytes, size_t& offset, TextureArtifactMipRecord& record)
{
    return ReadUInt32(bytes, offset, record.level) &&
        ReadUInt32(bytes, offset, record.width) &&
        ReadUInt32(bytes, offset, record.height) &&
        ReadUInt32(bytes, offset, record.rowPitch) &&
        ReadUInt32(bytes, offset, record.slicePitch) &&
        ReadUInt64(bytes, offset, record.dataOffset) &&
        ReadUInt64(bytes, offset, record.dataSize);
}

bool ValidateMip(
    const TextureArtifactMip& mip,
    const RHI::TextureFormat format,
    const uint32_t expectedLevel,
    const uint32_t expectedWidth,
    const uint32_t expectedHeight)
{
    if (mip.level != expectedLevel ||
        mip.width != expectedWidth ||
        mip.height != expectedHeight)
    {
        return false;
    }

    const uint32_t bytesPerPixel = RHI::GetTextureFormatBytesPerPixel(format);
    const uint64_t minimumRowPitch = static_cast<uint64_t>(mip.width) * bytesPerPixel;
    const uint64_t minimumSlicePitch = minimumRowPitch * mip.height;
    return mip.rowPitch >= minimumRowPitch &&
        mip.slicePitch >= minimumSlicePitch &&
        mip.pixels.size() == mip.slicePitch;
}

uint32_t NextMipDimension(const uint32_t value)
{
    return value > 1u ? value / 2u : 1u;
}

TextureArtifactMip MakeMip(
    const uint32_t level,
    const uint32_t width,
    const uint32_t height,
    std::vector<uint8_t> pixels)
{
    const uint32_t rowPitch = width * RHI::GetTextureFormatBytesPerPixel(RHI::TextureFormat::RGBA8);
    return {
        level,
        width,
        height,
        rowPitch,
        rowPitch * height,
        std::move(pixels)
    };
}

std::vector<uint8_t> GenerateNextRgba8Mip(
    const std::vector<uint8_t>& sourcePixels,
    const uint32_t sourceWidth,
    const uint32_t sourceHeight,
    const uint32_t nextWidth,
    const uint32_t nextHeight)
{
    std::vector<uint8_t> nextPixels(static_cast<size_t>(nextWidth) * nextHeight * 4u, 0u);
    for (uint32_t y = 0u; y < nextHeight; ++y)
    {
        for (uint32_t x = 0u; x < nextWidth; ++x)
        {
            uint32_t sums[4] = {};
            uint32_t samples = 0u;
            for (uint32_t offsetY = 0u; offsetY < 2u; ++offsetY)
            {
                const uint32_t sourceY = (std::min)(y * 2u + offsetY, sourceHeight - 1u);
                for (uint32_t offsetX = 0u; offsetX < 2u; ++offsetX)
                {
                    const uint32_t sourceX = (std::min)(x * 2u + offsetX, sourceWidth - 1u);
                    const size_t sourceIndex = (static_cast<size_t>(sourceY) * sourceWidth + sourceX) * 4u;
                    for (uint32_t channel = 0u; channel < 4u; ++channel)
                        sums[channel] += sourcePixels[sourceIndex + channel];
                    ++samples;
                }
            }

            const size_t destinationIndex = (static_cast<size_t>(y) * nextWidth + x) * 4u;
            for (uint32_t channel = 0u; channel < 4u; ++channel)
                nextPixels[destinationIndex + channel] = static_cast<uint8_t>(sums[channel] / samples);
        }
    }
    return nextPixels;
}
}

bool IsNativeTextureArtifact(const std::vector<uint8_t>& bytes)
{
    return NLS::Core::Assets::IsNativeArtifactContainer(bytes);
}

std::vector<uint8_t> SerializeTextureArtifactPayload(const TextureArtifactData& texture)
{
    if (texture.width == 0u ||
        texture.height == 0u ||
        texture.mips.empty() ||
        texture.mips.size() > std::numeric_limits<uint32_t>::max() ||
        !IsSupportedTextureFormat(texture.format) ||
        !IsSupportedColorSpace(texture.colorSpace))
    {
        return {};
    }

    uint32_t expectedWidth = texture.width;
    uint32_t expectedHeight = texture.height;
    for (uint32_t mipIndex = 0u; mipIndex < texture.mips.size(); ++mipIndex)
    {
        if (!ValidateMip(texture.mips[mipIndex], texture.format, mipIndex, expectedWidth, expectedHeight))
            return {};
        expectedWidth = NextMipDimension(expectedWidth);
        expectedHeight = NextMipDimension(expectedHeight);
    }

    const uint64_t tableBytes = kTextureArtifactHeaderBytes +
        static_cast<uint64_t>(texture.mips.size()) * kTextureArtifactMipRecordBytes;
    if (tableBytes > std::numeric_limits<size_t>::max())
        return {};

    std::vector<TextureArtifactMipRecord> records;
    records.reserve(texture.mips.size());
    uint64_t dataOffset = tableBytes;
    for (const auto& mip : texture.mips)
    {
        records.push_back({
            mip.level,
            mip.width,
            mip.height,
            mip.rowPitch,
            mip.slicePitch,
            dataOffset,
            static_cast<uint64_t>(mip.pixels.size())
        });
        dataOffset += mip.pixels.size();
    }
    if (dataOffset > std::numeric_limits<size_t>::max())
        return {};

    TextureArtifactHeader header;
    header.width = texture.width;
    header.height = texture.height;
    header.format = static_cast<uint32_t>(texture.format);
    header.colorSpace = static_cast<uint32_t>(texture.colorSpace);
    header.mipCount = static_cast<uint32_t>(texture.mips.size());

    std::vector<uint8_t> bytes;
    bytes.reserve(static_cast<size_t>(dataOffset));
    AppendHeader(bytes, header);
    for (const auto& record : records)
        AppendMipRecord(bytes, record);
    for (const auto& mip : texture.mips)
        bytes.insert(bytes.end(), mip.pixels.begin(), mip.pixels.end());
    return bytes;
}

std::vector<uint8_t> SerializeTextureArtifact(const TextureArtifactData& texture)
{
    auto payload = SerializeTextureArtifactPayload(texture);
    if (payload.empty())
        return {};

    NLS::Core::Assets::NativeArtifactMetadata metadata;
    metadata.artifactType = NLS::Core::Assets::ArtifactType::Texture;
    metadata.schemaName = "texture";
    metadata.schemaVersion = kTextureArtifactContainerSchemaVersion;
    return NLS::Core::Assets::WriteNativeArtifactContainer(std::move(metadata), payload);
}

std::optional<TextureArtifactData> DeserializeTextureArtifact(const std::vector<uint8_t>& bytes)
{
    auto container = NLS::Core::Assets::ReadNativeArtifactContainer(
        bytes,
        NLS::Core::Assets::ArtifactType::Texture,
        kTextureArtifactContainerSchemaVersion);
    if (!container.has_value())
        return std::nullopt;

    const auto& payload = container->payload;
    TextureArtifactHeader header;
    if (!ReadHeader(payload, header))
        return std::nullopt;

    const uint64_t tableBytes = kTextureArtifactHeaderBytes +
        static_cast<uint64_t>(header.mipCount) * kTextureArtifactMipRecordBytes;
    if (tableBytes > payload.size() || tableBytes > std::numeric_limits<size_t>::max())
        return std::nullopt;

    std::vector<TextureArtifactMipRecord> records;
    records.resize(header.mipCount);
    size_t offset = static_cast<size_t>(kTextureArtifactHeaderBytes);
    for (auto& record : records)
    {
        if (!ReadMipRecord(payload, offset, record))
            return std::nullopt;
    }

    TextureArtifactData texture;
    texture.width = header.width;
    texture.height = header.height;
    texture.format = static_cast<RHI::TextureFormat>(header.format);
    texture.colorSpace = static_cast<TextureArtifactColorSpace>(header.colorSpace);
    texture.mips.reserve(header.mipCount);

    uint32_t expectedWidth = texture.width;
    uint32_t expectedHeight = texture.height;
    for (uint32_t mipIndex = 0u; mipIndex < records.size(); ++mipIndex)
    {
        const auto& record = records[mipIndex];
        if (record.dataSize > std::numeric_limits<size_t>::max() ||
            record.dataOffset > std::numeric_limits<size_t>::max() ||
            record.dataOffset + record.dataSize > payload.size())
        {
            return std::nullopt;
        }

        TextureArtifactMip mip;
        mip.level = record.level;
        mip.width = record.width;
        mip.height = record.height;
        mip.rowPitch = record.rowPitch;
        mip.slicePitch = record.slicePitch;
        const auto begin = payload.begin() + static_cast<std::ptrdiff_t>(record.dataOffset);
        mip.pixels.assign(begin, begin + static_cast<std::ptrdiff_t>(record.dataSize));
        if (!ValidateMip(mip, texture.format, mipIndex, expectedWidth, expectedHeight))
            return std::nullopt;

        texture.mips.push_back(std::move(mip));
        expectedWidth = NextMipDimension(expectedWidth);
        expectedHeight = NextMipDimension(expectedHeight);
    }

    return texture;
}

std::optional<TextureArtifactData> LoadTextureArtifact(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;

    std::vector<uint8_t> bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
    return DeserializeTextureArtifact(bytes);
}

std::optional<TextureArtifactData> DecodeTextureArtifactFromEncodedImage(
    const uint8_t* encodedData,
    const size_t encodedDataSize,
    const TextureArtifactColorSpace colorSpace,
    const bool flipVertically)
{
    if (encodedData == nullptr ||
        encodedDataSize == 0u ||
        encodedDataSize > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        !IsSupportedColorSpace(colorSpace))
    {
        return std::nullopt;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_set_flip_vertically_on_load(flipVertically);
    unsigned char* decoded = stbi_load_from_memory(
        encodedData,
        static_cast<int>(encodedDataSize),
        &width,
        &height,
        &channels,
        4);
    if (!decoded || width <= 0 || height <= 0)
    {
        if (decoded)
            stbi_image_free(decoded);
        return std::nullopt;
    }

    TextureArtifactData artifact;
    artifact.width = static_cast<uint32_t>(width);
    artifact.height = static_cast<uint32_t>(height);
    artifact.format = RHI::TextureFormat::RGBA8;
    artifact.colorSpace = colorSpace;

    auto currentWidth = artifact.width;
    auto currentHeight = artifact.height;
    std::vector<uint8_t> currentPixels(
        decoded,
        decoded + static_cast<size_t>(artifact.width) * static_cast<size_t>(artifact.height) * 4u);
    stbi_image_free(decoded);

    uint32_t mipLevel = 0u;
    while (!currentPixels.empty())
    {
        artifact.mips.push_back(MakeMip(mipLevel, currentWidth, currentHeight, currentPixels));
        if (currentWidth == 1u && currentHeight == 1u)
            break;

        const uint32_t nextWidth = NextMipDimension(currentWidth);
        const uint32_t nextHeight = NextMipDimension(currentHeight);
        currentPixels = GenerateNextRgba8Mip(currentPixels, currentWidth, currentHeight, nextWidth, nextHeight);
        currentWidth = nextWidth;
        currentHeight = nextHeight;
        ++mipLevel;
    }

    if (artifact.mips.empty())
        return std::nullopt;
    return artifact;
}
}

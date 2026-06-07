#include "Rendering/Assets/TextureArtifact.h"
#include "Rendering/Assets/TextureMipGenerator.h"
#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/NativeArtifactContainer.h"

#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define stbi__tga_read_rgb16 nls_texture_artifact_stbi__tga_read_rgb16
#include <stb/stb_image.h>
#undef stbi__tga_read_rgb16

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

namespace NLS::Render::Assets
{
namespace
{
constexpr uint32_t kTextureArtifactMagic = 0x5845544Eu; // "NTEX" little-endian.
constexpr uint32_t kTextureArtifactVersion = 3u;
constexpr uint32_t kLegacyTextureArtifactVersion = 2u;
constexpr uint32_t kTextureArtifactContainerSchemaVersion = 4u;
constexpr uint32_t kLegacyTextureArtifactContainerSchemaVersion = 3u;
constexpr uint64_t kTextureArtifactHeaderBytes = 64u;
constexpr uint64_t kLegacyTextureArtifactHeaderBytes = 32u;
constexpr uint64_t kTextureArtifactMipRecordBytes = 36u;
constexpr uint64_t kTextureArtifactSubresourceRecordBytes = 48u;

struct TextureArtifactHeader
{
    uint32_t magic = kTextureArtifactMagic;
    uint32_t version = kTextureArtifactVersion;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t depth = 1u;
    uint32_t dimension = static_cast<uint32_t>(RHI::TextureDimension::Texture2D);
    uint32_t arrayLayers = 1u;
    uint32_t format = static_cast<uint32_t>(RHI::TextureFormat::RGBA8);
    uint32_t colorSpace = static_cast<uint32_t>(TextureArtifactColorSpace::Linear);
    uint32_t mipCount = 0u;
    uint32_t subresourceCount = 0u;
    uint64_t metadataStringTableOffset = 0u;
    uint64_t metadataStringTableSize = 0u;
    uint32_t reserved1 = 0u;
};

struct LegacyTextureArtifactHeader
{
    uint32_t magic = kTextureArtifactMagic;
    uint32_t version = kLegacyTextureArtifactVersion;
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

struct TextureArtifactSubresourceRecord
{
    uint32_t level = 0u;
    uint32_t arrayLayer = 0u;
    uint32_t face = static_cast<uint32_t>(TextureArtifactCubeFace::None);
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t depth = 1u;
    uint32_t rowPitch = 0u;
    uint32_t slicePitch = 0u;
    uint64_t dataOffset = 0u;
    uint64_t dataSize = 0u;
};

static_assert(sizeof(LegacyTextureArtifactHeader) == kLegacyTextureArtifactHeaderBytes);

struct ByteView
{
    const uint8_t* data = nullptr;
    size_t size = 0u;
};

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
    case RHI::TextureFormat::BC1:
    case RHI::TextureFormat::BC3:
    case RHI::TextureFormat::BC5:
    case RHI::TextureFormat::BC7:
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
    AppendUInt32(bytes, header.depth);
    AppendUInt32(bytes, header.dimension);
    AppendUInt32(bytes, header.arrayLayers);
    AppendUInt32(bytes, header.format);
    AppendUInt32(bytes, header.colorSpace);
    AppendUInt32(bytes, header.mipCount);
    AppendUInt32(bytes, header.subresourceCount);
    AppendUInt64(bytes, header.metadataStringTableOffset);
    AppendUInt64(bytes, header.metadataStringTableSize);
    AppendUInt32(bytes, header.reserved1);
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

void AppendSubresourceRecord(std::vector<uint8_t>& bytes, const TextureArtifactSubresourceRecord& record)
{
    AppendUInt32(bytes, record.level);
    AppendUInt32(bytes, record.arrayLayer);
    AppendUInt32(bytes, record.face);
    AppendUInt32(bytes, record.width);
    AppendUInt32(bytes, record.height);
    AppendUInt32(bytes, record.depth);
    AppendUInt32(bytes, record.rowPitch);
    AppendUInt32(bytes, record.slicePitch);
    AppendUInt64(bytes, record.dataOffset);
    AppendUInt64(bytes, record.dataSize);
}

bool ReadUInt32(const ByteView bytes, size_t& offset, uint32_t& value)
{
    if (offset + sizeof(uint32_t) > bytes.size)
        return false;

    value =
        static_cast<uint32_t>(bytes.data[offset]) |
        (static_cast<uint32_t>(bytes.data[offset + 1u]) << 8u) |
        (static_cast<uint32_t>(bytes.data[offset + 2u]) << 16u) |
        (static_cast<uint32_t>(bytes.data[offset + 3u]) << 24u);
    offset += sizeof(uint32_t);
    return true;
}

bool ReadUInt64(const ByteView bytes, size_t& offset, uint64_t& value)
{
    if (offset + sizeof(uint64_t) > bytes.size)
        return false;

    value = 0u;
    for (uint32_t byteIndex = 0u; byteIndex < 8u; ++byteIndex)
        value |= static_cast<uint64_t>(bytes.data[offset + byteIndex]) << (byteIndex * 8u);
    offset += sizeof(uint64_t);
    return true;
}

bool ReadHeader(const ByteView bytes, TextureArtifactHeader& header)
{
    if (bytes.size < 8u)
        return false;

    size_t offset = 0u;
    uint32_t magic = 0u;
    uint32_t version = 0u;
    if (!ReadUInt32(bytes, offset, magic) || !ReadUInt32(bytes, offset, version))
        return false;

    offset = 0u;
    if (magic != kTextureArtifactMagic)
        return false;

    if (version == kLegacyTextureArtifactVersion)
    {
        if (bytes.size < kLegacyTextureArtifactHeaderBytes)
            return false;

        LegacyTextureArtifactHeader legacyHeader;
        if (!ReadUInt32(bytes, offset, legacyHeader.magic) ||
            !ReadUInt32(bytes, offset, legacyHeader.version) ||
            !ReadUInt32(bytes, offset, legacyHeader.width) ||
            !ReadUInt32(bytes, offset, legacyHeader.height) ||
            !ReadUInt32(bytes, offset, legacyHeader.format) ||
            !ReadUInt32(bytes, offset, legacyHeader.colorSpace) ||
            !ReadUInt32(bytes, offset, legacyHeader.mipCount) ||
            !ReadUInt32(bytes, offset, legacyHeader.reserved))
        {
            return false;
        }

        header.magic = legacyHeader.magic;
        header.version = legacyHeader.version;
        header.width = legacyHeader.width;
        header.height = legacyHeader.height;
        header.depth = 1u;
        header.dimension = static_cast<uint32_t>(RHI::TextureDimension::Texture2D);
        header.arrayLayers = 1u;
        header.format = legacyHeader.format;
        header.colorSpace = legacyHeader.colorSpace;
        header.mipCount = legacyHeader.mipCount;
        header.subresourceCount = legacyHeader.mipCount;
        header.metadataStringTableOffset = 0u;
        header.metadataStringTableSize = 0u;
        header.reserved1 = 0u;
    }
    else if (version == kTextureArtifactVersion)
    {
        if (bytes.size < kTextureArtifactHeaderBytes)
            return false;

        if (!ReadUInt32(bytes, offset, header.magic) ||
            !ReadUInt32(bytes, offset, header.version) ||
            !ReadUInt32(bytes, offset, header.width) ||
            !ReadUInt32(bytes, offset, header.height) ||
            !ReadUInt32(bytes, offset, header.depth) ||
            !ReadUInt32(bytes, offset, header.dimension) ||
            !ReadUInt32(bytes, offset, header.arrayLayers) ||
            !ReadUInt32(bytes, offset, header.format) ||
            !ReadUInt32(bytes, offset, header.colorSpace) ||
            !ReadUInt32(bytes, offset, header.mipCount) ||
            !ReadUInt32(bytes, offset, header.subresourceCount) ||
            !ReadUInt64(bytes, offset, header.metadataStringTableOffset) ||
            !ReadUInt64(bytes, offset, header.metadataStringTableSize) ||
            !ReadUInt32(bytes, offset, header.reserved1))
        {
            return false;
        }
    }
    else
    {
        return false;
    }

    if (header.width == 0u ||
        header.height == 0u ||
        header.depth == 0u ||
        header.arrayLayers == 0u ||
        header.mipCount == 0u ||
        header.subresourceCount == 0u)
    {
        return false;
    }

    const auto format = static_cast<RHI::TextureFormat>(header.format);
    const auto colorSpace = static_cast<TextureArtifactColorSpace>(header.colorSpace);
    return IsSupportedTextureFormat(format) && IsSupportedColorSpace(colorSpace);
}

bool ReadMipRecord(const ByteView bytes, size_t& offset, TextureArtifactMipRecord& record)
{
    return ReadUInt32(bytes, offset, record.level) &&
        ReadUInt32(bytes, offset, record.width) &&
        ReadUInt32(bytes, offset, record.height) &&
        ReadUInt32(bytes, offset, record.rowPitch) &&
        ReadUInt32(bytes, offset, record.slicePitch) &&
        ReadUInt64(bytes, offset, record.dataOffset) &&
        ReadUInt64(bytes, offset, record.dataSize);
}

bool ReadSubresourceRecord(const ByteView bytes, size_t& offset, TextureArtifactSubresourceRecord& record)
{
    return ReadUInt32(bytes, offset, record.level) &&
        ReadUInt32(bytes, offset, record.arrayLayer) &&
        ReadUInt32(bytes, offset, record.face) &&
        ReadUInt32(bytes, offset, record.width) &&
        ReadUInt32(bytes, offset, record.height) &&
        ReadUInt32(bytes, offset, record.depth) &&
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

    const uint64_t minimumRowPitch = RHI::CalculateTextureRowPitch(format, mip.width);
    const uint64_t minimumSlicePitch = RHI::CalculateTextureSlicePitch(format, mip.width, mip.height, 1u);
    return mip.rowPitch == minimumRowPitch &&
        mip.slicePitch == minimumSlicePitch &&
        mip.pixels.size() == mip.slicePitch;
}

bool IsSupportedFace(const TextureArtifactCubeFace face)
{
    switch (face)
    {
    case TextureArtifactCubeFace::None:
    case TextureArtifactCubeFace::PositiveX:
    case TextureArtifactCubeFace::NegativeX:
    case TextureArtifactCubeFace::PositiveY:
    case TextureArtifactCubeFace::NegativeY:
    case TextureArtifactCubeFace::PositiveZ:
    case TextureArtifactCubeFace::NegativeZ:
        return true;
    default:
        return false;
    }
}

uint32_t NextMipDimension(const uint32_t value)
{
    return value > 1u ? value / 2u : 1u;
}

uint32_t MipDimensionAtLevel(uint32_t value, const uint32_t level)
{
    for (uint32_t index = 0u; index < level; ++index)
        value = NextMipDimension(value);
    return value;
}

uint32_t CountMipLevels(const std::vector<TextureArtifactMip>& mips)
{
    uint32_t mipCount = 0u;
    uint32_t lastLevel = std::numeric_limits<uint32_t>::max();
    for (const auto& mip : mips)
    {
        if (mip.level != lastLevel)
        {
            ++mipCount;
            lastLevel = mip.level;
        }
    }
    return mipCount;
}

bool HasContiguousMipLevels(const std::vector<TextureArtifactMip>& mips)
{
    if (mips.empty())
        return false;

    uint32_t expectedLevel = 0u;
    uint32_t lastLevel = std::numeric_limits<uint32_t>::max();
    for (const auto& mip : mips)
    {
        if (mip.level == lastLevel)
            continue;
        if (mip.level != expectedLevel)
            return false;
        lastLevel = mip.level;
        ++expectedLevel;
    }
    return true;
}

TextureArtifactCubeFace ExpectedCubeFaceForLayer(const uint32_t arrayLayer)
{
    switch (arrayLayer)
    {
    case 0u: return TextureArtifactCubeFace::PositiveX;
    case 1u: return TextureArtifactCubeFace::NegativeX;
    case 2u: return TextureArtifactCubeFace::PositiveY;
    case 3u: return TextureArtifactCubeFace::NegativeY;
    case 4u: return TextureArtifactCubeFace::PositiveZ;
    case 5u: return TextureArtifactCubeFace::NegativeZ;
    default: return TextureArtifactCubeFace::None;
    }
}

bool HasValidSubresourceLayout(const TextureArtifactData& texture)
{
    if (texture.subresources.empty())
        return texture.dimension == RHI::TextureDimension::Texture2D;

    if (texture.dimension == RHI::TextureDimension::Texture2D)
    {
        return std::all_of(
            texture.subresources.begin(),
            texture.subresources.end(),
            [](const TextureArtifactSubresource& subresource)
            {
                return subresource.arrayLayer == 0u &&
                    subresource.face == TextureArtifactCubeFace::None;
            });
    }

    if (texture.dimension != RHI::TextureDimension::TextureCube ||
        texture.arrayLayers != 6u ||
        texture.subresources.size() != static_cast<size_t>(CountMipLevels(texture.mips)) * 6u)
    {
        return false;
    }

    uint32_t currentLevel = std::numeric_limits<uint32_t>::max();
    uint32_t faceIndex = 0u;
    for (const auto& subresource : texture.subresources)
    {
        if (subresource.level != currentLevel)
        {
            currentLevel = subresource.level;
            faceIndex = 0u;
        }
        if (faceIndex >= 6u ||
            subresource.arrayLayer != faceIndex ||
            subresource.face != ExpectedCubeFaceForLayer(faceIndex))
        {
            return false;
        }
        ++faceIndex;
    }
    return true;
}

uint64_t HeaderSizeForVersion(const uint32_t version)
{
    return version == kLegacyTextureArtifactVersion
        ? kLegacyTextureArtifactHeaderBytes
        : kTextureArtifactHeaderBytes;
}

void AppendLengthPrefixedString(std::vector<uint8_t>& bytes, const std::string& value)
{
    AppendUInt32(bytes, static_cast<uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

std::vector<uint8_t> BuildMetadataStringTable(const TextureArtifactData& texture)
{
    std::vector<uint8_t> bytes;
    AppendLengthPrefixedString(bytes, texture.targetPlatform);
    AppendLengthPrefixedString(bytes, texture.buildIdentity);
    AppendLengthPrefixedString(bytes, texture.encoderId);
    AppendUInt32(bytes, texture.encoderVersion);
    return bytes;
}

bool ReadLengthPrefixedString(
    const ByteView bytes,
    size_t& offset,
    const size_t endOffset,
    std::string& value)
{
    uint32_t length = 0u;
    if (!ReadUInt32(bytes, offset, length))
        return false;
    if (offset > endOffset || length > endOffset - offset)
        return false;

    value.assign(
        reinterpret_cast<const char*>(bytes.data + offset),
        reinterpret_cast<const char*>(bytes.data + offset + length));
    offset += length;
    return true;
}

bool ReadMetadataStringTable(const ByteView payload, const TextureArtifactHeader& header, TextureArtifactData& texture)
{
    if (header.metadataStringTableOffset == 0u && header.metadataStringTableSize == 0u)
        return true;
    if (header.metadataStringTableOffset > std::numeric_limits<size_t>::max() ||
        header.metadataStringTableSize > std::numeric_limits<size_t>::max())
    {
        return false;
    }

    const size_t begin = static_cast<size_t>(header.metadataStringTableOffset);
    const size_t size = static_cast<size_t>(header.metadataStringTableSize);
    if (begin > payload.size || size > payload.size - begin)
        return false;

    size_t offset = begin;
    const size_t endOffset = begin + size;
    return ReadLengthPrefixedString(payload, offset, endOffset, texture.targetPlatform) &&
        ReadLengthPrefixedString(payload, offset, endOffset, texture.buildIdentity) &&
        ReadLengthPrefixedString(payload, offset, endOffset, texture.encoderId) &&
        ReadUInt32(payload, offset, texture.encoderVersion) &&
        offset == endOffset;
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
        texture.depth != 1u ||
        (texture.dimension != RHI::TextureDimension::Texture2D &&
            texture.dimension != RHI::TextureDimension::TextureCube) ||
        texture.arrayLayers == 0u ||
        texture.mips.empty() ||
        texture.mips.size() > std::numeric_limits<uint32_t>::max() ||
        (!texture.subresources.empty() && texture.subresources.size() != texture.mips.size()) ||
        !IsSupportedTextureFormat(texture.format) ||
        !IsSupportedColorSpace(texture.colorSpace) ||
        !HasContiguousMipLevels(texture.mips) ||
        !HasValidSubresourceLayout(texture))
    {
        return {};
    }

    for (uint32_t mipIndex = 0u; mipIndex < texture.mips.size(); ++mipIndex)
    {
        const auto& mip = texture.mips[mipIndex];
        if (!ValidateMip(
                mip,
                texture.format,
                mip.level,
                MipDimensionAtLevel(texture.width, mip.level),
                MipDimensionAtLevel(texture.height, mip.level)))
        {
            return {};
        }
        if (!texture.subresources.empty())
        {
            const auto& subresource = texture.subresources[mipIndex];
            if (!IsSupportedFace(subresource.face) ||
                subresource.level != mip.level ||
                subresource.arrayLayer >= texture.arrayLayers ||
                subresource.width != mip.width ||
                subresource.height != mip.height ||
                subresource.depth != 1u ||
                subresource.rowPitch != mip.rowPitch ||
                subresource.slicePitch != mip.slicePitch ||
                (subresource.dataSize != 0u && subresource.dataSize != mip.pixels.size()))
            {
                return {};
            }
        }
    }

    const uint64_t tableBytes = kTextureArtifactHeaderBytes +
        static_cast<uint64_t>(texture.mips.size()) * kTextureArtifactMipRecordBytes +
        static_cast<uint64_t>(texture.mips.size()) * kTextureArtifactSubresourceRecordBytes;
    if (tableBytes > std::numeric_limits<size_t>::max())
        return {};

    const auto metadataBytes = BuildMetadataStringTable(texture);
    if (metadataBytes.size() > std::numeric_limits<uint64_t>::max() - tableBytes)
        return {};

    std::vector<TextureArtifactMipRecord> records;
    records.reserve(texture.mips.size());
    std::vector<TextureArtifactSubresourceRecord> subresourceRecords;
    subresourceRecords.reserve(texture.mips.size());
    uint64_t dataOffset = tableBytes + metadataBytes.size();
    for (size_t mipIndex = 0u; mipIndex < texture.mips.size(); ++mipIndex)
    {
        const auto& mip = texture.mips[mipIndex];
        records.push_back({
            mip.level,
            mip.width,
            mip.height,
            mip.rowPitch,
            mip.slicePitch,
            dataOffset,
            static_cast<uint64_t>(mip.pixels.size())
        });
        const TextureArtifactSubresource defaultSubresource{
            mip.level,
            0u,
            mip.width,
            mip.height,
            1u,
            TextureArtifactCubeFace::None,
            mip.rowPitch,
            mip.slicePitch,
            dataOffset,
            static_cast<uint64_t>(mip.pixels.size())
        };
        const auto& subresource = texture.subresources.empty()
            ? defaultSubresource
            : texture.subresources[mipIndex];
        subresourceRecords.push_back({
            mip.level,
            subresource.arrayLayer,
            static_cast<uint32_t>(subresource.face),
            mip.width,
            mip.height,
            subresource.depth,
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
    header.depth = texture.depth;
    header.dimension = static_cast<uint32_t>(texture.dimension);
    header.arrayLayers = texture.arrayLayers;
    header.format = static_cast<uint32_t>(texture.format);
    header.colorSpace = static_cast<uint32_t>(texture.colorSpace);
    header.mipCount = CountMipLevels(texture.mips);
    header.subresourceCount = static_cast<uint32_t>(texture.mips.size());
    header.metadataStringTableOffset = tableBytes;
    header.metadataStringTableSize = metadataBytes.size();

    std::vector<uint8_t> bytes;
    bytes.reserve(static_cast<size_t>(dataOffset));
    AppendHeader(bytes, header);
    for (const auto& record : records)
        AppendMipRecord(bytes, record);
    for (const auto& record : subresourceRecords)
        AppendSubresourceRecord(bytes, record);
    bytes.insert(bytes.end(), metadataBytes.begin(), metadataBytes.end());
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
    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        NLS::Core::Assets::ArtifactLoadTelemetryStage::CpuDeserialize});

    auto container = NLS::Core::Assets::ReadNativeArtifactContainerView(
        bytes,
        NLS::Core::Assets::ArtifactType::Texture,
        kTextureArtifactContainerSchemaVersion);
    if (!container.has_value())
    {
        container = NLS::Core::Assets::ReadNativeArtifactContainerView(
            bytes,
            NLS::Core::Assets::ArtifactType::Texture,
            kLegacyTextureArtifactContainerSchemaVersion);
    }
    if (!container.has_value())
        return std::nullopt;

    const ByteView payload {container->payloadData, container->payloadSize};
    TextureArtifactHeader header;
    if (!ReadHeader(payload, header))
        return std::nullopt;

    const uint32_t storedSubresourceCount = header.version == kLegacyTextureArtifactVersion
        ? header.mipCount
        : header.subresourceCount;
    const uint64_t tableBytes = HeaderSizeForVersion(header.version) +
        static_cast<uint64_t>(storedSubresourceCount) * kTextureArtifactMipRecordBytes +
        (header.version == kLegacyTextureArtifactVersion
            ? 0u
            : static_cast<uint64_t>(storedSubresourceCount) * kTextureArtifactSubresourceRecordBytes);
    if (tableBytes > payload.size || tableBytes > std::numeric_limits<size_t>::max())
        return std::nullopt;

    std::vector<TextureArtifactMipRecord> records;
    records.resize(storedSubresourceCount);
    size_t offset = static_cast<size_t>(HeaderSizeForVersion(header.version));
    for (auto& record : records)
    {
        if (!ReadMipRecord(payload, offset, record))
            return std::nullopt;
    }
    std::vector<TextureArtifactSubresourceRecord> subresourceRecords;
    if (header.version != kLegacyTextureArtifactVersion)
    {
        if (header.subresourceCount == 0u)
            return std::nullopt;
        subresourceRecords.resize(header.subresourceCount);
        for (auto& record : subresourceRecords)
        {
            if (!ReadSubresourceRecord(payload, offset, record))
                return std::nullopt;
        }
    }

    TextureArtifactData texture;
    texture.width = header.width;
    texture.height = header.height;
    texture.depth = header.depth;
    texture.dimension = static_cast<RHI::TextureDimension>(header.dimension);
    texture.arrayLayers = header.arrayLayers;
    texture.format = static_cast<RHI::TextureFormat>(header.format);
    texture.colorSpace = static_cast<TextureArtifactColorSpace>(header.colorSpace);
    texture.mips.reserve(storedSubresourceCount);
    texture.subresources.reserve(storedSubresourceCount);
    if (!ReadMetadataStringTable(payload, header, texture))
        return std::nullopt;

    for (uint32_t mipIndex = 0u; mipIndex < records.size(); ++mipIndex)
    {
        const auto& record = records[mipIndex];
        const TextureArtifactSubresourceRecord subresourceRecord = header.version == kLegacyTextureArtifactVersion
            ? TextureArtifactSubresourceRecord{
                record.level,
                0u,
                static_cast<uint32_t>(TextureArtifactCubeFace::None),
                record.width,
                record.height,
                1u,
                record.rowPitch,
                record.slicePitch,
                record.dataOffset,
                record.dataSize
            }
            : subresourceRecords[mipIndex];
        if (record.dataSize > std::numeric_limits<size_t>::max() ||
            record.dataOffset > std::numeric_limits<size_t>::max() ||
            record.dataOffset > payload.size ||
            record.dataSize > payload.size - record.dataOffset)
        {
            return std::nullopt;
        }

        TextureArtifactMip mip;
        mip.level = record.level;
        mip.width = record.width;
        mip.height = record.height;
        mip.rowPitch = record.rowPitch;
        mip.slicePitch = record.slicePitch;
        const auto begin = payload.data + static_cast<size_t>(record.dataOffset);
        mip.pixels.assign(begin, begin + static_cast<size_t>(record.dataSize));
        if (!ValidateMip(
                mip,
                texture.format,
                record.level,
                MipDimensionAtLevel(texture.width, record.level),
                MipDimensionAtLevel(texture.height, record.level)))
        {
            return std::nullopt;
        }
        const auto face = static_cast<TextureArtifactCubeFace>(subresourceRecord.face);
        if (!IsSupportedFace(face) ||
            subresourceRecord.level != record.level ||
            subresourceRecord.arrayLayer >= texture.arrayLayers ||
            subresourceRecord.width != record.width ||
            subresourceRecord.height != record.height ||
            subresourceRecord.depth != 1u ||
            subresourceRecord.rowPitch != record.rowPitch ||
            subresourceRecord.slicePitch != record.slicePitch ||
            subresourceRecord.dataOffset != record.dataOffset ||
            subresourceRecord.dataSize != record.dataSize)
        {
            return std::nullopt;
        }

        texture.mips.push_back(std::move(mip));
        texture.subresources.push_back({
            record.level,
            subresourceRecord.arrayLayer,
            record.width,
            record.height,
            subresourceRecord.depth,
            face,
            record.rowPitch,
            record.slicePitch,
            record.dataOffset,
            record.dataSize
        });
    }

    if (CountMipLevels(texture.mips) != header.mipCount ||
        !HasContiguousMipLevels(texture.mips) ||
        !HasValidSubresourceLayout(texture))
    {
        return std::nullopt;
    }

    return texture;
}

std::optional<TextureArtifactData> LoadTextureArtifact(const std::filesystem::path& path)
{
    return LoadTextureArtifact(path, nullptr);
}

std::optional<TextureArtifactData> LoadTextureArtifact(
    const std::filesystem::path& path,
    const std::atomic_bool* cancellationFlag)
{
    NLS::Core::Assets::ArtifactLoadTelemetryRecord telemetry;
    telemetry.stage = NLS::Core::Assets::ArtifactLoadTelemetryStage::NativeArtifactFileRead;
    telemetry.path = path.generic_string();
    NLS::Core::Assets::RecordArtifactLoadTelemetry(telemetry);

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;

    auto isCancelled = [cancellationFlag]
    {
        return cancellationFlag != nullptr && cancellationFlag->load(std::memory_order_acquire);
    };
    if (isCancelled())
        return std::nullopt;

    std::vector<uint8_t> bytes;
    std::array<char, 64u * 1024u> buffer {};
    while (input)
    {
        if (isCancelled())
            return std::nullopt;

        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto readCount = input.gcount();
        if (readCount <= 0)
            break;

        const auto* begin = reinterpret_cast<const uint8_t*>(buffer.data());
        bytes.insert(bytes.end(), begin, begin + static_cast<size_t>(readCount));
    }
    if (isCancelled())
        return std::nullopt;

    return DeserializeTextureArtifact(bytes);
}

std::optional<TextureArtifactData> DecodeTextureArtifactFromEncodedImage(
    const uint8_t* encodedData,
    const size_t encodedDataSize,
    const TextureArtifactColorSpace colorSpace,
    const bool flipVertically)
{
    TextureMipGeneratorSettings settings;
    settings.intent = TextureMipIntent::Color;
    settings.colorSpace = colorSpace;
    settings.format = RHI::TextureFormat::RGBA8;
    settings.mipmapEnabled = true;
    return DecodeTextureArtifactFromEncodedImage(encodedData, encodedDataSize, settings, flipVertically);
}

std::optional<TextureArtifactData> DecodeTextureArtifactFromEncodedImage(
    const uint8_t* encodedData,
    const size_t encodedDataSize,
    const TextureMipGeneratorSettings& settings,
    const bool flipVertically)
{
    if (encodedData == nullptr ||
        encodedDataSize == 0u ||
        encodedDataSize > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        !IsSupportedColorSpace(settings.colorSpace))
    {
        return std::nullopt;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_set_flip_vertically_on_load_thread(flipVertically);
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

    std::vector<uint8_t> basePixels(
        decoded,
        decoded + static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    stbi_image_free(decoded);

    return GenerateTextureMipChain(
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        std::move(basePixels),
        settings);
}
}

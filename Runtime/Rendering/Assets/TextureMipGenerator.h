#pragma once

#include "RenderDef.h"
#include "Rendering/Assets/TextureArtifact.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <optional>
#include <vector>

namespace NLS::Render::Assets
{
enum class TextureMipIntent : uint8_t
{
    Color = 0,
    Normal,
    Mask,
    UI,
    HDR
};

struct NLS_RENDER_API TextureMipGeneratorSettings
{
    TextureMipIntent intent = TextureMipIntent::Color;
    TextureArtifactColorSpace colorSpace = TextureArtifactColorSpace::Linear;
    NLS::Render::RHI::TextureFormat format = NLS::Render::RHI::TextureFormat::RGBA8;
    bool mipmapEnabled = true;
};

namespace Detail
{
    inline uint32_t NextTextureMipDimension(const uint32_t value)
    {
        return value > 1u ? value / 2u : 1u;
    }

    inline TextureArtifactMip MakeTextureMip(
        const NLS::Render::RHI::TextureFormat format,
        const uint32_t level,
        const uint32_t width,
        const uint32_t height,
        std::vector<uint8_t> pixels)
    {
        return {
            level,
            width,
            height,
            NLS::Render::RHI::CalculateTextureRowPitch(format, width),
            NLS::Render::RHI::CalculateTextureSlicePitch(format, width, height, 1u),
            std::move(pixels)
        };
    }

    inline uint16_t FloatToHalfBits(const float value)
    {
        uint32_t bits = 0u;
        std::memcpy(&bits, &value, sizeof(float));

        const uint32_t sign = (bits >> 16u) & 0x8000u;
        int32_t exponent = static_cast<int32_t>((bits >> 23u) & 0xFFu) - 127 + 15;
        uint32_t mantissa = bits & 0x7FFFFFu;

        if (exponent <= 0)
        {
            if (exponent < -10)
                return static_cast<uint16_t>(sign);
            mantissa = (mantissa | 0x800000u) >> static_cast<uint32_t>(1 - exponent);
            return static_cast<uint16_t>(sign | ((mantissa + 0x1000u) >> 13u));
        }

        if (exponent >= 31)
            return static_cast<uint16_t>(sign | 0x7C00u);

        return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10u) | ((mantissa + 0x1000u) >> 13u));
    }

    inline float HalfBitsToFloat(const uint16_t half)
    {
        const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16u;
        int32_t exponent = static_cast<int32_t>((half >> 10u) & 0x1Fu);
        uint32_t mantissa = half & 0x03FFu;

        uint32_t bits = 0u;
        if (exponent == 0)
        {
            if (mantissa == 0u)
            {
                bits = sign;
            }
            else
            {
                exponent = 1u;
                while ((mantissa & 0x0400u) == 0u)
                {
                    mantissa <<= 1u;
                    --exponent;
                }
                mantissa &= 0x03FFu;
                bits = sign | (static_cast<uint32_t>(exponent + (127 - 15)) << 23u) | (mantissa << 13u);
            }
        }
        else if (exponent == 31)
        {
            bits = sign | 0x7F800000u | (mantissa << 13u);
        }
        else
        {
            bits = sign | (static_cast<uint32_t>(exponent + (127 - 15)) << 23u) | (mantissa << 13u);
        }

        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(float));
        return value;
    }

    inline float ReadHalfComponent(const std::vector<uint8_t>& pixels, const size_t texelIndex, const uint32_t channel)
    {
        const size_t byteIndex = texelIndex * 8u + static_cast<size_t>(channel) * 2u;
        const uint16_t half = static_cast<uint16_t>(pixels[byteIndex]) |
            (static_cast<uint16_t>(pixels[byteIndex + 1u]) << 8u);
        return HalfBitsToFloat(half);
    }

    inline void WriteHalfComponent(
        std::vector<uint8_t>& pixels,
        const size_t texelIndex,
        const uint32_t channel,
        const float value)
    {
        const uint16_t half = FloatToHalfBits(value);
        const size_t byteIndex = texelIndex * 8u + static_cast<size_t>(channel) * 2u;
        pixels[byteIndex] = static_cast<uint8_t>(half & 0xFFu);
        pixels[byteIndex + 1u] = static_cast<uint8_t>((half >> 8u) & 0xFFu);
    }

    inline std::vector<uint8_t> GenerateBoxFilteredTextureMip(
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

    inline float DecodeTextureNormalChannel(const uint8_t value)
    {
        return (static_cast<float>(value) / 255.0f) * 2.0f - 1.0f;
    }

    inline uint8_t EncodeTextureNormalChannel(const float value)
    {
        const float normalized = std::clamp(value * 0.5f + 0.5f, 0.0f, 1.0f);
        return static_cast<uint8_t>(std::lround(normalized * 255.0f));
    }

    inline float SrgbToLinear(const uint8_t value)
    {
        const float srgb = static_cast<float>(value) / 255.0f;
        return srgb <= 0.04045f
            ? srgb / 12.92f
            : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
    }

    inline uint8_t LinearToSrgb(const float value)
    {
        const float linear = std::clamp(value, 0.0f, 1.0f);
        const float srgb = linear <= 0.0031308f
            ? linear * 12.92f
            : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        return static_cast<uint8_t>(std::lround(std::clamp(srgb, 0.0f, 1.0f) * 255.0f));
    }

    inline std::vector<uint8_t> GenerateSrgbColorFilteredTextureMip(
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
                float colorSums[3] = {};
                uint32_t alphaSum = 0u;
                uint32_t samples = 0u;
                for (uint32_t offsetY = 0u; offsetY < 2u; ++offsetY)
                {
                    const uint32_t sourceY = (std::min)(y * 2u + offsetY, sourceHeight - 1u);
                    for (uint32_t offsetX = 0u; offsetX < 2u; ++offsetX)
                    {
                        const uint32_t sourceX = (std::min)(x * 2u + offsetX, sourceWidth - 1u);
                        const size_t sourceIndex = (static_cast<size_t>(sourceY) * sourceWidth + sourceX) * 4u;
                        for (uint32_t channel = 0u; channel < 3u; ++channel)
                            colorSums[channel] += SrgbToLinear(sourcePixels[sourceIndex + channel]);
                        alphaSum += sourcePixels[sourceIndex + 3u];
                        ++samples;
                    }
                }

                const size_t destinationIndex = (static_cast<size_t>(y) * nextWidth + x) * 4u;
                for (uint32_t channel = 0u; channel < 3u; ++channel)
                    nextPixels[destinationIndex + channel] = LinearToSrgb(colorSums[channel] / static_cast<float>(samples));
                nextPixels[destinationIndex + 3u] = static_cast<uint8_t>(alphaSum / samples);
            }
        }

        return nextPixels;
    }

    inline std::vector<uint8_t> GenerateNormalFilteredTextureMip(
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
                float sumX = 0.0f;
                float sumY = 0.0f;
                float sumZ = 0.0f;
                uint32_t alphaSum = 0u;
                uint32_t samples = 0u;
                for (uint32_t offsetY = 0u; offsetY < 2u; ++offsetY)
                {
                    const uint32_t sourceY = (std::min)(y * 2u + offsetY, sourceHeight - 1u);
                    for (uint32_t offsetX = 0u; offsetX < 2u; ++offsetX)
                    {
                        const uint32_t sourceX = (std::min)(x * 2u + offsetX, sourceWidth - 1u);
                        const size_t sourceIndex = (static_cast<size_t>(sourceY) * sourceWidth + sourceX) * 4u;
                        sumX += DecodeTextureNormalChannel(sourcePixels[sourceIndex + 0u]);
                        sumY += DecodeTextureNormalChannel(sourcePixels[sourceIndex + 1u]);
                        sumZ += DecodeTextureNormalChannel(sourcePixels[sourceIndex + 2u]);
                        alphaSum += sourcePixels[sourceIndex + 3u];
                        ++samples;
                    }
                }

                if (samples == 0u)
                    continue;

                float normalX = sumX / static_cast<float>(samples);
                float normalY = sumY / static_cast<float>(samples);
                float normalZ = sumZ / static_cast<float>(samples);
                const float length = std::sqrt(normalX * normalX + normalY * normalY + normalZ * normalZ);
                if (length > 0.0001f)
                {
                    normalX /= length;
                    normalY /= length;
                    normalZ /= length;
                }
                else
                {
                    normalX = 0.0f;
                    normalY = 0.0f;
                    normalZ = 1.0f;
                }

                const size_t destinationIndex = (static_cast<size_t>(y) * nextWidth + x) * 4u;
                nextPixels[destinationIndex + 0u] = EncodeTextureNormalChannel(normalX);
                nextPixels[destinationIndex + 1u] = EncodeTextureNormalChannel(normalY);
                nextPixels[destinationIndex + 2u] = EncodeTextureNormalChannel(normalZ);
                nextPixels[destinationIndex + 3u] = static_cast<uint8_t>(alphaSum / samples);
            }
        }

        return nextPixels;
    }

    inline std::vector<uint8_t> GenerateHalfFloatFilteredTextureMip(
        const std::vector<uint8_t>& sourcePixels,
        const uint32_t sourceWidth,
        const uint32_t sourceHeight,
        const uint32_t nextWidth,
        const uint32_t nextHeight)
    {
        std::vector<uint8_t> nextPixels(static_cast<size_t>(nextWidth) * nextHeight * 8u, 0u);
        for (uint32_t y = 0u; y < nextHeight; ++y)
        {
            for (uint32_t x = 0u; x < nextWidth; ++x)
            {
                float sums[4] = {};
                uint32_t samples = 0u;
                for (uint32_t offsetY = 0u; offsetY < 2u; ++offsetY)
                {
                    const uint32_t sourceY = (std::min)(y * 2u + offsetY, sourceHeight - 1u);
                    for (uint32_t offsetX = 0u; offsetX < 2u; ++offsetX)
                    {
                        const uint32_t sourceX = (std::min)(x * 2u + offsetX, sourceWidth - 1u);
                        const size_t sourceTexelIndex = static_cast<size_t>(sourceY) * sourceWidth + sourceX;
                        for (uint32_t channel = 0u; channel < 4u; ++channel)
                            sums[channel] += ReadHalfComponent(sourcePixels, sourceTexelIndex, channel);
                        ++samples;
                    }
                }

                const size_t destinationTexelIndex = static_cast<size_t>(y) * nextWidth + x;
                for (uint32_t channel = 0u; channel < 4u; ++channel)
                    WriteHalfComponent(nextPixels, destinationTexelIndex, channel, sums[channel] / static_cast<float>(samples));
            }
        }

        return nextPixels;
    }

    inline std::optional<std::vector<uint8_t>> GenerateNextTextureMip(
        const NLS::Render::RHI::TextureFormat format,
        const TextureMipIntent intent,
        const TextureArtifactColorSpace colorSpace,
        const std::vector<uint8_t>& currentPixels,
        const uint32_t currentWidth,
        const uint32_t currentHeight,
        const uint32_t nextWidth,
        const uint32_t nextHeight)
    {
        if (format == NLS::Render::RHI::TextureFormat::RGBA16F)
            return GenerateHalfFloatFilteredTextureMip(currentPixels, currentWidth, currentHeight, nextWidth, nextHeight);

        if (format != NLS::Render::RHI::TextureFormat::RGBA8)
            return std::nullopt;

        if (intent == TextureMipIntent::Normal)
            return GenerateNormalFilteredTextureMip(currentPixels, currentWidth, currentHeight, nextWidth, nextHeight);
        if (intent == TextureMipIntent::Color && colorSpace == TextureArtifactColorSpace::Srgb)
            return GenerateSrgbColorFilteredTextureMip(currentPixels, currentWidth, currentHeight, nextWidth, nextHeight);
        return GenerateBoxFilteredTextureMip(currentPixels, currentWidth, currentHeight, nextWidth, nextHeight);
    }
}

inline std::optional<TextureArtifactData> GenerateTextureMipChain(
    const uint32_t width,
    const uint32_t height,
    std::vector<uint8_t> basePixels,
    const TextureMipGeneratorSettings& settings)
{
    if (width == 0u || height == 0u)
        return std::nullopt;

    const auto* descriptor = NLS::Render::RHI::GetTextureFormatDescriptor(settings.format);
    if (descriptor == nullptr ||
        descriptor->isCompressed ||
        (settings.format != NLS::Render::RHI::TextureFormat::RGBA8 &&
            settings.format != NLS::Render::RHI::TextureFormat::RGBA16F))
    {
        return std::nullopt;
    }

    const size_t expectedBytes = static_cast<size_t>(
        NLS::Render::RHI::CalculateTextureSlicePitch(settings.format, width, height, 1u));
    if (basePixels.size() != expectedBytes)
        return std::nullopt;

    TextureArtifactData artifact;
    artifact.width = width;
    artifact.height = height;
    artifact.depth = 1u;
    artifact.dimension = NLS::Render::RHI::TextureDimension::Texture2D;
    artifact.arrayLayers = 1u;
    artifact.format = settings.format;
    artifact.colorSpace = settings.colorSpace;

    uint32_t currentWidth = width;
    uint32_t currentHeight = height;
    std::vector<uint8_t> currentPixels = std::move(basePixels);
    uint32_t mipLevel = 0u;
    uint64_t dataOffset = 0u;

    while (true)
    {
        artifact.mips.push_back(Detail::MakeTextureMip(settings.format, mipLevel, currentWidth, currentHeight, currentPixels));
        const uint64_t dataSize = currentPixels.size();
        artifact.subresources.push_back({
            mipLevel,
            0u,
            currentWidth,
            currentHeight,
            1u,
            TextureArtifactCubeFace::None,
            NLS::Render::RHI::CalculateTextureRowPitch(settings.format, currentWidth),
            NLS::Render::RHI::CalculateTextureSlicePitch(
                settings.format,
                currentWidth,
                currentHeight,
                1u),
            dataOffset,
            dataSize
        });
        dataOffset += dataSize;
        if (!settings.mipmapEnabled ||
            settings.intent == TextureMipIntent::UI ||
            (currentWidth == 1u && currentHeight == 1u))
        {
            break;
        }

        const uint32_t nextWidth = Detail::NextTextureMipDimension(currentWidth);
        const uint32_t nextHeight = Detail::NextTextureMipDimension(currentHeight);
        auto nextPixels = Detail::GenerateNextTextureMip(
            settings.format,
            settings.intent,
            settings.colorSpace,
            currentPixels,
            currentWidth,
            currentHeight,
            nextWidth,
            nextHeight);
        if (!nextPixels.has_value())
            return std::nullopt;
        currentPixels = std::move(*nextPixels);
        currentWidth = nextWidth;
        currentHeight = nextHeight;
        ++mipLevel;
    }

    return artifact;
}
}

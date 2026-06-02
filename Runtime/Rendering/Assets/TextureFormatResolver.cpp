#include "Rendering/Assets/TextureFormatResolver.h"

#include <algorithm>
#include <utility>

namespace NLS::Render::Assets
{
namespace
{
bool IsFormatAvailable(
    const TextureBackendCapabilities& capabilities,
    const RHI::TextureFormat format,
    const RHI::TextureColorSpace colorSpace)
{
    const auto found = capabilities.supportedFormats.find(format);
    if (found == capabilities.supportedFormats.end())
        return false;

    const auto& support = found->second;
    if (!support.sampled || !support.upload)
        return false;

    const auto* descriptor = RHI::GetTextureFormatDescriptor(format);
    if (colorSpace == RHI::TextureColorSpace::SRGB &&
        descriptor != nullptr &&
        descriptor->supportsSrgbView &&
        !support.supportsSrgbView)
    {
        return false;
    }

    return true;
}

bool IsEncoderCompatibleWithFormat(const std::string& encoderId, const RHI::TextureFormat format)
{
    if (encoderId == "rgba8-passthrough")
    {
        return format == RHI::TextureFormat::RGBA8 ||
            format == RHI::TextureFormat::RGBA16F;
    }

    if (encoderId == "directxtex-bc")
    {
        return format == RHI::TextureFormat::BC1 ||
            format == RHI::TextureFormat::BC3 ||
            format == RHI::TextureFormat::BC5 ||
            format == RHI::TextureFormat::BC7;
    }

    return false;
}

void ResolveCompatibleEncoder(TextureBuildSettings& resolved)
{
    if (resolved.resolvedFormat == RHI::TextureFormat::RGBA8 ||
        resolved.resolvedFormat == RHI::TextureFormat::RGBA16F)
    {
        resolved.encoderId = "rgba8-passthrough";
        resolved.encoderVersion = 1u;
    }
}

bool RequiresTinyFallback(
    const TextureBackendCapabilities& capabilities,
    const RHI::TextureFormat format,
    const TextureSourceDescriptor& source)
{
    if (!RHI::IsTextureFormatCompressed(format))
        return false;

    if (source.width >= 4u && source.height >= 4u)
        return false;

    const auto found = capabilities.supportedFormats.find(format);
    if (found == capabilities.supportedFormats.end())
        return true;

    return !found->second.supportsUnalignedBlockTextures;
}

RHI::TextureFormat ResolveExplicitFormat(const std::string& value)
{
    return RHI::ParseTextureFormatName(value);
}

std::string EffectiveTextureIntent(const TextureImportSettingsSnapshot& settings)
{
    const std::string intent = settings.textureType.empty() ? "default" : settings.textureType;
    if (intent == "normal" || intent == "normal-map" || intent == "normalmap")
        return "normal";
    if (intent == "mask" || intent == "mask-map" || intent == "maskmap")
        return "mask";
    if (intent == "ui" || intent == "sprite")
        return "ui";
    if (intent == "hdr" || intent == "hdr-color")
        return "hdr";
    return intent;
}

void AddDiagnostic(
    TextureBuildResolveResult& result,
    const TextureSourceDescriptor& source,
    const TextureBackendCapabilities& capabilities,
    const RHI::TextureFormat requestedFormat,
    const RHI::TextureFormat resolvedFormat,
    std::string reason)
{
    TextureBuildDiagnostic diagnostic;
    diagnostic.assetPath = source.assetPath;
    diagnostic.targetPlatform = capabilities.targetPlatform;
    diagnostic.requestedFormat = requestedFormat;
    diagnostic.resolvedFormat = resolvedFormat;
    diagnostic.reason = std::move(reason);
    result.diagnostics.push_back(std::move(diagnostic));
}

bool ResolveFallbackFormat(
    TextureBuildResolveResult& result,
    TextureBuildSettings& resolved,
    const TextureSourceDescriptor& source,
    const TextureBackendCapabilities& capabilities,
    const RHI::TextureFormat preferredFormat,
    const RHI::TextureColorSpace preferredColorSpace,
    const char* intentName)
{
    if (IsFormatAvailable(capabilities, preferredFormat, preferredColorSpace) &&
        !RequiresTinyFallback(capabilities, preferredFormat, source))
    {
        resolved.resolvedFormat = preferredFormat;
        return true;
    }

    if (!IsFormatAvailable(capabilities, RHI::TextureFormat::RGBA8, resolved.colorSpace))
    {
        AddDiagnostic(
            result,
            source,
            capabilities,
            preferredFormat,
            RHI::TextureFormat::Count,
            std::string(intentName) + " preferred format unavailable and RGBA8 fallback unavailable");
        return false;
    }

    resolved.resolvedFormat = RHI::TextureFormat::RGBA8;
    AddDiagnostic(
        result,
        source,
        capabilities,
        preferredFormat,
        resolved.resolvedFormat,
        IsFormatAvailable(capabilities, preferredFormat, preferredColorSpace)
            ? std::string(intentName) + " preferred format rejected by top-level block alignment policy"
            : std::string(intentName) + " preferred format unavailable; falling back to RGBA8");
    return true;
}

void ForceLinearColorSpaceWhenFormatRequiresIt(
    TextureBuildResolveResult& result,
    TextureBuildSettings& resolved,
    const TextureSourceDescriptor& source,
    const TextureBackendCapabilities& capabilities)
{
    if (resolved.colorSpace != RHI::TextureColorSpace::SRGB)
        return;

    const auto* descriptor = RHI::GetTextureFormatDescriptor(resolved.resolvedFormat);
    if (descriptor == nullptr || descriptor->supportsSrgbView)
        return;

    resolved.colorSpace = RHI::TextureColorSpace::Linear;
    AddDiagnostic(
        result,
        source,
        capabilities,
        resolved.resolvedFormat,
        resolved.resolvedFormat,
        "resolved texture format does not support sRGB views; forcing linear color space");
}

bool CompleteResolve(
    TextureBuildResolveResult& result,
    TextureBuildSettings& resolved,
    const TextureSourceDescriptor& source,
    const TextureBackendCapabilities& capabilities,
    const bool hasUnknownExplicitFormat,
    const std::string& explicitFormatText)
{
    if (hasUnknownExplicitFormat)
    {
        AddDiagnostic(
            result,
            source,
            capabilities,
            RHI::TextureFormat::Count,
            resolved.resolvedFormat,
            "unknown explicit texture format \"" + explicitFormatText + "\"; resolved by texture intent");
    }

    ForceLinearColorSpaceWhenFormatRequiresIt(result, resolved, source, capabilities);
    ResolveCompatibleEncoder(resolved);
    if (!IsEncoderCompatibleWithFormat(resolved.encoderId, resolved.resolvedFormat))
    {
        AddDiagnostic(
            result,
            source,
            capabilities,
            resolved.resolvedFormat,
            RHI::TextureFormat::Count,
            "texture encoder \"" + resolved.encoderId + "\" does not support resolved texture format");
        return false;
    }

    result.settings = std::move(resolved);
    return true;
}
}

TextureBuildResolveResult ResolveTextureBuildSettingsWithDiagnostics(
    const TextureImportSettingsSnapshot& settings,
    const std::optional<TexturePlatformOverrideSettings>& platformOverride,
    const TextureSourceDescriptor& source,
    const TextureBackendCapabilities& capabilities,
    std::string encoderId,
    const uint32_t encoderVersion)
{
    TextureBuildResolveResult result;
    if (source.width == 0u || source.height == 0u || capabilities.targetPlatform.empty())
    {
        AddDiagnostic(
            result,
            source,
            capabilities,
            RHI::TextureFormat::Count,
            RHI::TextureFormat::Count,
            "invalid texture source dimensions or empty target platform");
        return result;
    }
    if (encoderId.empty())
    {
        AddDiagnostic(
            result,
            source,
            capabilities,
            RHI::TextureFormat::Count,
            RHI::TextureFormat::Count,
            "texture encoder is unavailable for target platform");
        return result;
    }

    TextureBuildSettings resolved;
    resolved.sourceAssetPath = source.assetPath;
    resolved.targetPlatform = capabilities.targetPlatform;
    resolved.textureIntent = EffectiveTextureIntent(settings);
    resolved.mipmapEnabled = platformOverride.has_value() && platformOverride->mipmapEnabled.has_value()
        ? *platformOverride->mipmapEnabled
        : settings.mipmapEnabled;
    resolved.maxTextureSize = platformOverride.has_value() && platformOverride->maxTextureSize != 0u
        ? platformOverride->maxTextureSize
        : settings.maxTextureSize;
    resolved.resizePolicy = platformOverride.has_value() && !platformOverride->resizePolicy.empty()
        ? platformOverride->resizePolicy
        : settings.resizePolicy;
    resolved.colorSpace = settings.srgbTexture
        ? RHI::TextureColorSpace::SRGB
        : RHI::TextureColorSpace::Linear;
    resolved.encoderId = std::move(encoderId);
    resolved.encoderVersion = encoderVersion;
    resolved.artifactSchemaVersion = 4u;

    const std::string explicitFormatText = platformOverride.has_value() && !platformOverride->format.empty()
        ? platformOverride->format
        : settings.explicitFormat;
    const RHI::TextureFormat explicitFormat = ResolveExplicitFormat(explicitFormatText);
    const bool hasUnknownExplicitFormat = !explicitFormatText.empty() && explicitFormat == RHI::TextureFormat::Count;
    if (explicitFormat != RHI::TextureFormat::Count)
    {
        const auto* descriptor = RHI::GetTextureFormatDescriptor(explicitFormat);
        const RHI::TextureColorSpace explicitColorSpace =
            resolved.colorSpace == RHI::TextureColorSpace::SRGB &&
            descriptor != nullptr &&
            !descriptor->supportsSrgbView
                ? RHI::TextureColorSpace::Linear
                : resolved.colorSpace;
        if (!IsFormatAvailable(capabilities, explicitFormat, explicitColorSpace))
        {
            AddDiagnostic(
                result,
                source,
                capabilities,
                explicitFormat,
                RHI::TextureFormat::Count,
                "explicit texture format is unavailable for target platform");
            return result;
        }
        resolved.resolvedFormat = explicitFormat;
        if (RequiresTinyFallback(capabilities, resolved.resolvedFormat, source))
        {
            if (!IsFormatAvailable(capabilities, RHI::TextureFormat::RGBA8, resolved.colorSpace))
            {
                AddDiagnostic(
                    result,
                    source,
                    capabilities,
                    explicitFormat,
                    RHI::TextureFormat::Count,
                    "explicit texture format rejected by top-level block alignment policy and RGBA8 fallback unavailable");
                return result;
            }
            AddDiagnostic(
                result,
                source,
                capabilities,
                explicitFormat,
                RHI::TextureFormat::RGBA8,
                "explicit texture format rejected by top-level block alignment policy; falling back to RGBA8");
            resolved.resolvedFormat = RHI::TextureFormat::RGBA8;
        }
        CompleteResolve(result, resolved, source, capabilities, false, explicitFormatText);
        return result;
    }

    if (source.isHDR || resolved.textureIntent == "hdr")
    {
        resolved.colorSpace = RHI::TextureColorSpace::Linear;
        if (!IsFormatAvailable(capabilities, RHI::TextureFormat::RGBA16F, resolved.colorSpace))
        {
            AddDiagnostic(
                result,
                source,
                capabilities,
                RHI::TextureFormat::RGBA16F,
                RHI::TextureFormat::Count,
                "HDR texture requires RGBA16F fallback, but RGBA16F is unavailable");
            return result;
        }
        resolved.resolvedFormat = RHI::TextureFormat::RGBA16F;
        CompleteResolve(result, resolved, source, capabilities, hasUnknownExplicitFormat, explicitFormatText);
        return result;
    }

    if (settings.compressionIntent == "uncompressed")
    {
        if (!IsFormatAvailable(capabilities, RHI::TextureFormat::RGBA8, resolved.colorSpace))
        {
            AddDiagnostic(
                result,
                source,
                capabilities,
                RHI::TextureFormat::RGBA8,
                RHI::TextureFormat::Count,
                "uncompressed RGBA8 texture format is unavailable for target platform");
            return result;
        }
        resolved.resolvedFormat = RHI::TextureFormat::RGBA8;
        CompleteResolve(result, resolved, source, capabilities, hasUnknownExplicitFormat, explicitFormatText);
        return result;
    }

    if (resolved.textureIntent == "normal")
    {
        resolved.colorSpace = RHI::TextureColorSpace::Linear;
        if (!ResolveFallbackFormat(
                result,
                resolved,
                source,
                capabilities,
                RHI::TextureFormat::BC5,
                RHI::TextureColorSpace::Linear,
                "normal"))
        {
            return result;
        }
        CompleteResolve(result, resolved, source, capabilities, hasUnknownExplicitFormat, explicitFormatText);
        return result;
    }

    if (resolved.textureIntent == "mask")
    {
        resolved.colorSpace = RHI::TextureColorSpace::Linear;
        if (!ResolveFallbackFormat(
                result,
                resolved,
                source,
                capabilities,
                RHI::TextureFormat::BC1,
                RHI::TextureColorSpace::Linear,
                "mask"))
        {
            return result;
        }
        CompleteResolve(result, resolved, source, capabilities, hasUnknownExplicitFormat, explicitFormatText);
        return result;
    }

    if (resolved.textureIntent == "ui")
    {
        if (!IsFormatAvailable(capabilities, RHI::TextureFormat::RGBA8, resolved.colorSpace))
        {
            AddDiagnostic(
                result,
                source,
                capabilities,
                RHI::TextureFormat::RGBA8,
                RHI::TextureFormat::Count,
                "UI texture requires RGBA8, but RGBA8 is unavailable for target platform");
            return result;
        }
        resolved.resolvedFormat = RHI::TextureFormat::RGBA8;
        resolved.mipmapEnabled = false;
        CompleteResolve(result, resolved, source, capabilities, hasUnknownExplicitFormat, explicitFormatText);
        return result;
    }

    const bool hasAlpha = source.hasAlpha || settings.alphaIsTransparency;
    if (hasAlpha)
    {
        if (!ResolveFallbackFormat(
                result,
                resolved,
                source,
                capabilities,
                RHI::TextureFormat::BC3,
                resolved.colorSpace,
                "alpha color"))
        {
            return result;
        }
        CompleteResolve(result, resolved, source, capabilities, hasUnknownExplicitFormat, explicitFormatText);
        return result;
    }

    if (!ResolveFallbackFormat(
            result,
            resolved,
            source,
            capabilities,
            RHI::TextureFormat::BC1,
            resolved.colorSpace,
            "color"))
    {
        return result;
    }
    CompleteResolve(result, resolved, source, capabilities, hasUnknownExplicitFormat, explicitFormatText);
    return result;
}

std::optional<TextureBuildSettings> ResolveTextureBuildSettings(
    const TextureImportSettingsSnapshot& settings,
    const std::optional<TexturePlatformOverrideSettings>& platformOverride,
    const TextureSourceDescriptor& source,
    const TextureBackendCapabilities& capabilities,
    std::string encoderId,
    const uint32_t encoderVersion)
{
    return ResolveTextureBuildSettingsWithDiagnostics(
        settings,
        platformOverride,
        source,
        capabilities,
        std::move(encoderId),
        encoderVersion).settings;
}
}

#pragma once

#include "RenderDef.h"
#include "Rendering/RHI/RHITypes.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Render::Assets
{
struct TexturePlatformOverrideSettings
{
    std::string platform;
    uint32_t maxTextureSize = 0u;
    std::string format;
    std::string compressionQuality;
    std::string resizePolicy;
    std::optional<bool> mipmapEnabled;
};

struct TextureImportSettingsSnapshot
{
    std::string textureType = "default";
    bool srgbTexture = true;
    bool alphaIsTransparency = false;
    bool mipmapEnabled = true;
    uint32_t maxTextureSize = 2048u;
    std::string resizePolicy = "keep";
    std::string compressionIntent = "default";
    std::string explicitFormat;
    std::vector<TexturePlatformOverrideSettings> platformOverrides;
};

struct TextureSourceDescriptor
{
    std::string assetPath;
    uint32_t width = 0u;
    uint32_t height = 0u;
    bool hasAlpha = false;
    bool isHDR = false;
};

struct TextureBackendCapabilities
{
    std::string targetPlatform;
    std::map<RHI::TextureFormat, RHI::TextureFormatCapability> supportedFormats;
};

struct TextureBuildSettings
{
    std::string sourceAssetPath;
    std::string sourceAssetIdentity;
    std::string sourceContentHash;
    std::string normalizedSettingsHash;
    std::string platformOverrideHash;
    uint32_t importerVersion = 0u;
    uint32_t postprocessorVersion = 0u;
    std::string dependencyHash;
    std::string targetPlatform;
    std::string textureIntent;
    RHI::TextureFormat resolvedFormat = RHI::TextureFormat::RGBA8;
    bool mipmapEnabled = true;
    uint32_t maxTextureSize = 0u;
    std::string resizePolicy;
    RHI::TextureColorSpace colorSpace = RHI::TextureColorSpace::Linear;
    std::string encoderId;
    uint32_t encoderVersion = 0u;
    std::string encoderOptionsHash;
    std::string toolVersion;
    uint32_t artifactSchemaVersion = 0u;
};

struct TextureBuildDiagnostic
{
    std::string assetPath;
    std::string targetPlatform;
    RHI::TextureFormat requestedFormat = RHI::TextureFormat::Count;
    RHI::TextureFormat resolvedFormat = RHI::TextureFormat::Count;
    std::string reason;
};

struct TextureBuildResolveResult
{
    std::optional<TextureBuildSettings> settings;
    std::vector<TextureBuildDiagnostic> diagnostics;
};

NLS_RENDER_API std::string BuildTextureBuildIdentity(const TextureBuildSettings& settings);
}

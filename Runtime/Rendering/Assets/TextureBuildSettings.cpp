#include "Rendering/Assets/TextureBuildSettings.h"

#include <sstream>

namespace NLS::Render::Assets
{
std::string BuildTextureBuildIdentity(const TextureBuildSettings& settings)
{
    std::ostringstream stream;
    stream
        << "sourcePath=" << settings.sourceAssetPath
        << "|sourceId=" << settings.sourceAssetIdentity
        << "|sourceHash=" << settings.sourceContentHash
        << "|settings=" << settings.normalizedSettingsHash
        << "|override=" << settings.platformOverrideHash
        << "|importer=" << settings.importerVersion
        << "|post=" << settings.postprocessorVersion
        << "|deps=" << settings.dependencyHash
        << "|target=" << settings.targetPlatform
        << "|intent=" << settings.textureIntent
        << "|format=" << static_cast<uint32_t>(settings.resolvedFormat)
        << "|mips=" << (settings.mipmapEnabled ? 1u : 0u)
        << "|maxSize=" << settings.maxTextureSize
        << "|resize=" << settings.resizePolicy
        << "|colorSpace=" << static_cast<uint32_t>(settings.colorSpace)
        << "|encoder=" << settings.encoderId
        << "|encoderVersion=" << settings.encoderVersion
        << "|encoderOptions=" << settings.encoderOptionsHash
        << "|toolVersion=" << settings.toolVersion
        << "|schema=" << settings.artifactSchemaVersion;
    return stream.str();
}
}

#pragma once

#include "RenderDef.h"
#include "Rendering/Assets/TextureBuildSettings.h"

#include <optional>
#include <string>

namespace NLS::Render::Assets
{
NLS_RENDER_API TextureBuildResolveResult ResolveTextureBuildSettingsWithDiagnostics(
    const TextureImportSettingsSnapshot& settings,
    const std::optional<TexturePlatformOverrideSettings>& platformOverride,
    const TextureSourceDescriptor& source,
    const TextureBackendCapabilities& capabilities,
    std::string encoderId,
    uint32_t encoderVersion);

NLS_RENDER_API std::optional<TextureBuildSettings> ResolveTextureBuildSettings(
    const TextureImportSettingsSnapshot& settings,
    const std::optional<TexturePlatformOverrideSettings>& platformOverride,
    const TextureSourceDescriptor& source,
    const TextureBackendCapabilities& capabilities,
    std::string encoderId,
    uint32_t encoderVersion);
}

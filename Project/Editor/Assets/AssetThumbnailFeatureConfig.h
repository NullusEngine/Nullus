#pragma once

#include <filesystem>

namespace NLS::Editor::Assets
{
/// Runtime switches for the editor thumbnail pipeline.
///
/// The defaults preserve the current pipeline where a feature already exists;
/// experimental paths remain opt-in until their renderer/RHI lifetime rules
/// are complete.
struct AssetThumbnailFeatureConfig
{
    bool residentPrefabPreview = true;
    // Keep the pool available behind an explicit flag until matched editor
    // trials show a consistent first-fill gain. It is a lifetime optimization
    // and must not trade cold-preview throughput for reuse on every device.
    bool previewProxyPool = false;
    bool atlas = false;
    // Keep the service-level default compatible with callers that rely on
    // Encoding after a completed readback. Asset Browser startup settings can
    // still opt out until the three-slot ring passes its default-enable gate.
    bool readbackRing = true;
    bool adaptiveBudget = true;
    bool explicitLanes = true;
    std::filesystem::path cacheRoot;

    [[nodiscard]] static AssetThumbnailFeatureConfig Legacy()
    {
        AssetThumbnailFeatureConfig config;
        config.residentPrefabPreview = false;
        config.previewProxyPool = false;
        config.atlas = false;
        config.readbackRing = false;
        config.adaptiveBudget = false;
        config.explicitLanes = false;
        return config;
    }
};
}

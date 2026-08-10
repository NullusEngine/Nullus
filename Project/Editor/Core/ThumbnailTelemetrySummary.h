#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "Assets/ArtifactLoadTelemetry.h"

namespace NLS::Editor::Panels
{
struct AssetBrowserThumbnailDrawOutcomeTelemetrySnapshot;
}

namespace NLS::Editor::Core
{
struct ResidentPrefabPreviewTelemetrySnapshot
{
    struct ThumbnailRequestIdentityCount
    {
        std::string identity;
        size_t count = 0u;
    };

    size_t entryCount = 0u;
    size_t activeLeaseCount = 0u;
    size_t residentBytes = 0u;
    size_t hitCount = 0u;
    size_t missCount = 0u;
    size_t staleCount = 0u;
    size_t zeroArtifactReadHitCount = 0u;
    size_t evictionCount = 0u;
    size_t identityMissCount = 0u;
    size_t freshnessMismatchCount = 0u;
    size_t thumbnailRequestCount = 0u;
    size_t thumbnailRequestOtherIdentityCount = 0u;
    std::vector<ThumbnailRequestIdentityCount> thumbnailRequestIdentityCounts;
    std::string lastRegisteredIdentity;
    std::string lastRegisteredLookupIdentity;
    std::string lastRegisteredFreshness;
    std::string lastRequestedIdentity;
    std::string lastRequestedFreshness;
    std::string lastKnownIdentity;
    std::string lastKnownFreshness;
    std::string lastMismatchIdentity;
    std::string lastMismatchRequestedFreshness;
    std::string lastMismatchKnownFreshness;
};

// Formats immutable telemetry inputs without reading editor-global state.
std::string BuildThumbnailTelemetrySummaryReport(
    const std::vector<NLS::Core::Assets::ArtifactLoadTelemetryRecord>& records,
    const NLS::Editor::Panels::AssetBrowserThumbnailDrawOutcomeTelemetrySnapshot& drawOutcomes,
    bool telemetryEnabled,
    std::string_view runConfiguration = {},
    const ResidentPrefabPreviewTelemetrySnapshot& resident = {});
}

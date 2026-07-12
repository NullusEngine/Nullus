#pragma once

#include <string>
#include <vector>

#include "Assets/ArtifactLoadTelemetry.h"

namespace NLS::Editor::Panels
{
struct AssetBrowserThumbnailDrawOutcomeTelemetrySnapshot;
}

namespace NLS::Editor::Core
{
// Formats immutable telemetry inputs without reading editor-global state.
std::string BuildThumbnailTelemetrySummaryReport(
    const std::vector<NLS::Core::Assets::ArtifactLoadTelemetryRecord>& records,
    const NLS::Editor::Panels::AssetBrowserThumbnailDrawOutcomeTelemetrySnapshot& drawOutcomes,
    bool telemetryEnabled);
}

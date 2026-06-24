#pragma once

#include "Assets/ModelTextureReferenceResolver.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace NLS::Editor::Assets
{
struct ModelTextureResolutionReport
{
    uint32_t reportVersion = 1u;
    std::string modelAssetId;
    std::string targetPlatform;
    uint32_t importerVersion = 0u;
    std::string settingsFingerprint;
    std::vector<ResolvedModelTextureReference> entries;
};

struct ModelTextureReportContext
{
    std::string modelAssetId;
    std::string targetPlatform;
    uint32_t importerVersion = 0u;
    std::string settingsFingerprint;
};

std::string SerializeModelTextureResolutionReport(const ModelTextureResolutionReport& report);
std::optional<ModelTextureResolutionReport> ParseModelTextureResolutionReport(const std::string& text);
bool IsModelTextureResolutionReportCurrent(
    const ModelTextureResolutionReport& report,
    const ModelTextureReportContext& context);
std::filesystem::path ModelTextureResolutionReportPath(const std::filesystem::path& committedArtifactRoot);
}

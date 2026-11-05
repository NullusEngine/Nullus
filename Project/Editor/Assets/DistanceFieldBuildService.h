#pragma once

#include <atomic>
#include <mutex>
#include "Assets/AssetMeta.h"
#include "Rendering/Assets/DistanceFieldBuildService.h"

namespace NLS::Editor::Assets
{
struct DistanceFieldBuildRequest
{
    std::filesystem::path projectRoot;
    std::string assetPath;
    bool forceRebuild = false;
    uint32_t mipMask = 7;
    bool includeFallbacks = true;
    std::string sourceIdentityHint;
    // The Inspector records this before writing a DF-only metadata change.
    std::string previousMetaStamp;
};
struct DistanceFieldBuildResult
{
    bool success = false, cancelled = false;
    uint64_t legacySourceReads = 0, publications = 0;
    double firstCoarseMs = 0, totalMs = 0;
    double publicationMs = 0;
    Render::DistanceField::DistanceFieldModelBuildResult model;
    std::string error;
};
struct DistanceFieldBuildJob
{
    std::atomic_bool cancelled{false}, running{true};
    std::mutex mutex;
    Render::DistanceField::DistanceFieldModelBuildProgress progress;
    DistanceFieldBuildResult result;
    std::string signature;
};

class DistanceFieldBuildService
{
public:
    static bool CanSchedule();
    static DistanceFieldBuildResult Build(const DistanceFieldBuildRequest& request,
        const std::atomic_bool* cancellation = nullptr,
        std::function<void(const Render::DistanceField::DistanceFieldModelBuildProgress&)> progress = {});
    static std::shared_ptr<DistanceFieldBuildJob> Queue(DistanceFieldBuildRequest request,
        std::function<void(bool, std::string)> completion = {});
    static bool IsBuilding(const std::filesystem::path& projectRoot, const std::string& assetPath);
    static std::shared_ptr<DistanceFieldBuildJob> FindBuildJob(const std::filesystem::path& projectRoot, const std::string& assetPath);
    static void CancelAll(const std::filesystem::path& projectRoot);
    static std::string MetadataStamp(const std::filesystem::path& path);
};
}

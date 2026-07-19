
#include <filesystem>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <Debug/Logger.h>
#include <Assets/ArtifactLoadTelemetry.h>
#include <Jobs/BackgroundJobQueue.h>
#include <Jobs/JobSystem.h>
#include <Profiling/PerformanceStageStats.h>
#include <Profiling/Profiler.h>
#include <Profiling/TracyProfiler.h>
#include <Reflection/ReflectionDiagnostics.h>
#include <Rendering/Debug/DebugDrawService.h>
#include <ServiceLocator.h>
#include <imgui.h>

#include "Core/Editor.h"
#include "Core/EditorJobSystemPolicy.h"
#include "Core/ThumbnailTelemetrySummary.h"
#include "UI/Settings/PanelWindowSettings.h"
#include "Assembly.h"
#include "Core/AssemblyCore.h"
#include "AssemblyMath.h"
#include "AssemblyEngine.h"
#include "AssemblyPlatform.h"
#include "AssemblyRender.h"

#include "Panels/EditorTopBar.h"
#include "Panels/EditorStatusBar.h"
#include "Panels/AssetBrowser.h"
#include "Panels/FrameInfo.h"
#include "Panels/Console.h"
#include "Panels/Inspector.h"
#include "Panels/Hierarchy.h"
#include "Panels/SceneView.h"
#include "Panels/GameView.h"
#include "Panels/AssetView.h"
#include "Panels/ViewFrameLifecycle.h"
#include "Panels/MaterialEditor.h"
#include "Panels/ProjectSettings.h"
#include "Panels/AssetProperties.h"
#include "Panels/ProfilerPanel.h"
#include "Engine/PrimitiveType.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Shortcuts/EditorShortcutBinding.h"
#include "Shortcuts/EditorShortcutContext.h"
#include <Windowing/Inputs/EMouseButton.h>
#include <Windowing/Inputs/EMouseButtonState.h>
using namespace NLS::Core::ResourceManagement;
using namespace NLS::Editor::Panels;
namespace NLS
{
namespace
{
NLS::Base::Profiling::TracyProfiler g_tracyProfiler;
std::size_t g_publishedReflectionDiagnosticCount = 0;
constexpr uint32_t kEditorMainThreadContinuationDrainBudget = 64u;
constexpr float kEditorValidationCameraForwardStep = 0.1f;
constexpr auto kThumbnailTelemetrySummaryWriteInterval = std::chrono::seconds(2);

enum class ValidationFocusTarget
{
    None,
    SceneView,
    GameView
};

std::string NormalizeValidationToken(std::string_view value)
{
    std::string normalized(value);
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](unsigned char ch)
        {
            return static_cast<char>(std::tolower(ch));
        });
    return normalized;
}

ValidationFocusTarget ResolveValidationFocusTarget(std::string_view value)
{
    const std::string normalized = NormalizeValidationToken(value);
    if (normalized.empty())
        return ValidationFocusTarget::None;
    if (normalized == "scene" || normalized == "sceneview" || normalized == "scene-view")
        return ValidationFocusTarget::SceneView;
    if (normalized == "game" || normalized == "gameview" || normalized == "game-view")
        return ValidationFocusTarget::GameView;
    return ValidationFocusTarget::None;
}

struct ValidationSceneCameraTransform
{
    Maths::Vector3 position;
    Maths::Vector3 rotationEulerDegrees;
};

std::optional<ValidationSceneCameraTransform> ParseValidationSceneCamera(std::string_view value)
{
    std::string text(value);
    ValidationSceneCameraTransform transform;
    char trailing = 0;
    const int parsedCount = std::sscanf(
        text.c_str(),
        " %f , %f , %f ; %f , %f , %f %c",
        &transform.position.x,
        &transform.position.y,
        &transform.position.z,
        &transform.rotationEulerDegrees.x,
        &transform.rotationEulerDegrees.y,
        &transform.rotationEulerDegrees.z,
        &trailing);

    if (parsedCount != 6)
        return std::nullopt;

    return transform;
}

void CreateValidationOcclusionStack(
    Editor::Core::EditorActions& editorActions,
    const NLS::Render::Entities::Camera& camera,
    const uint32_t count)
{
    if (count == 0u)
        return;

    constexpr float kNearOccluderDistance = 6.0f;
    constexpr float kTargetStartDistance = 10.0f;
    constexpr float kTargetSpacing = 1.25f;
    constexpr float kOccluderScale = 4.0f;
    constexpr float kTargetScale = 0.75f;
    const auto cameraPosition = camera.GetPosition();
    const auto cameraForward = Maths::Vector3::Normalize(camera.transform->GetWorldForward());
    const auto cameraRight = Maths::Vector3::Normalize(camera.GetRotation() * Maths::Vector3::Right);
    const auto cameraUp = Maths::Vector3::Normalize(camera.GetRotation() * Maths::Vector3::Up);
    uint32_t createdCount = 0u;
    for (uint32_t index = 0u; index < count; ++index)
    {
        auto* cube = editorActions.CreatePrimitive(NLS::Engine::PrimitiveType::Cube, false);
        if (cube == nullptr || cube->GetTransform() == nullptr)
            continue;

        cube->SetName("Validation Occlusion Cube " + std::to_string(index));
        const bool isOccluder = index == 0u;
        const float distance = isOccluder
            ? kNearOccluderDistance
            : kTargetStartDistance + static_cast<float>(index - 1u) * kTargetSpacing;
        const float lane = isOccluder ? 0.0f : (static_cast<float>((index - 1u) % 3u) - 1.0f) * 0.1f;
        const float row = isOccluder ? 0.0f : static_cast<float>((index - 1u) / 3u) * 0.1f;
        cube->GetTransform()->SetWorldPosition(
            cameraPosition +
            cameraForward * distance +
            cameraRight * lane +
            cameraUp * row);
        const float scale = isOccluder ? kOccluderScale : kTargetScale;
        cube->GetTransform()->SetWorldScale({ scale, scale, scale });
        ++createdCount;
    }

    NLS_LOG_INFO(
        "Editor validation created occlusion stack: requested=" +
        std::to_string(count) +
        " created=" +
        std::to_string(createdCount));
}

void PublishReflectionDiagnosticsToLog()
{
    const auto diagnostics = NLS::meta::ReflectionDiagnostics::Snapshot();
    if (g_publishedReflectionDiagnosticCount > diagnostics.size())
        g_publishedReflectionDiagnosticCount = 0;

    for (std::size_t index = g_publishedReflectionDiagnosticCount; index < diagnostics.size(); ++index)
    {
        const auto& diagnostic = diagnostics[index];
        const std::string message = "Reflection: " + NLS::meta::ReflectionDiagnostics::Format(diagnostic);
        if (diagnostic.severity == NLS::meta::ReflectionDiagnosticSeverity::Error)
            NLS_LOG_ERROR(message);
        else
            NLS_LOG_WARNING(message);
    }

    g_publishedReflectionDiagnosticCount = diagnostics.size();
}

bool IsThumbnailLatencyStage(const NLS::Core::Assets::ArtifactLoadTelemetryStage stage)
{
    return std::string_view(NLS::Core::Assets::ArtifactLoadTelemetryStageName(stage))
        .starts_with("Thumbnail");
}

std::string FormatTelemetryDurationMs(const std::chrono::microseconds elapsed)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3)
        << (static_cast<double>(elapsed.count()) / 1000.0);
    return stream.str();
}

struct ArtifactTelemetryReportSummaries
{
    std::vector<NLS::Core::Assets::ArtifactLoadTelemetryStageSummary> pathSummaries;
    std::vector<NLS::Core::Assets::ArtifactLoadTelemetryStageSummary> stageTotals;
};

ArtifactTelemetryReportSummaries BuildArtifactTelemetryReportSummaries(
    const std::vector<NLS::Core::Assets::ArtifactLoadTelemetryRecord>& records)
{
    using NLS::Core::Assets::ArtifactLoadTelemetryStageSummary;

    ArtifactTelemetryReportSummaries result;
    result.pathSummaries.reserve(records.size());
    std::unordered_map<uint8_t, std::unordered_map<std::string, size_t>> pathSummaryIndices;
    std::unordered_map<uint8_t, size_t> stageTotalIndices;
    const auto addRecord = [](ArtifactLoadTelemetryStageSummary& summary, const auto& record)
    {
        ++summary.recordCount;
        summary.totalElapsed += record.elapsed;
        summary.totalBytes += record.byteCount;
    };
    for (const auto& record : records)
    {
        const auto stageKey = static_cast<uint8_t>(record.stage);
        auto& indicesForStage = pathSummaryIndices[stageKey];
        const auto [pathIndex, insertedPath] = indicesForStage.emplace(
            record.path,
            result.pathSummaries.size());
        if (insertedPath)
        {
            result.pathSummaries.push_back({ record.stage, record.path });
        }
        addRecord(result.pathSummaries[pathIndex->second], record);

        const auto [stageIndex, insertedStage] = stageTotalIndices.emplace(
            stageKey,
            result.stageTotals.size());
        if (insertedStage)
        {
            result.stageTotals.push_back({ record.stage, {} });
        }
        addRecord(result.stageTotals[stageIndex->second], record);
    }
    return result;
}

std::string FormatThumbnailTelemetrySummaryReport(
    const std::vector<NLS::Core::Assets::ArtifactLoadTelemetryRecord>& records,
    const NLS::Editor::Panels::AssetBrowserThumbnailDrawOutcomeTelemetrySnapshot& thumbnailDrawOutcomes,
    const bool telemetryEnabled)
{
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;
    using NLS::Core::Assets::ArtifactLoadTelemetryStageName;

    struct StageAggregate
    {
        ArtifactLoadTelemetryStage stage = ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRender;
        size_t recordCount = 0u;
        size_t totalBytes = 0u;
        std::chrono::microseconds totalElapsed {};
        std::chrono::microseconds maxElapsed {};
        std::string slowestPath;
        bool hasSlowestPath = false;
    };

    auto reportSummaries = BuildArtifactTelemetryReportSummaries(records);
    auto& summaries = reportSummaries.pathSummaries;
    auto& allStageTotals = reportSummaries.stageTotals;
    std::sort(
        allStageTotals.begin(),
        allStageTotals.end(),
        [](const auto& left, const auto& right)
        {
            if (left.totalElapsed != right.totalElapsed)
                return left.totalElapsed > right.totalElapsed;
            return std::string_view(ArtifactLoadTelemetryStageName(left.stage)) <
                std::string_view(ArtifactLoadTelemetryStageName(right.stage));
        });

    std::unordered_map<uint8_t, StageAggregate> stageAggregates;
    for (const auto& record : records)
    {
        if (!IsThumbnailLatencyStage(record.stage))
            continue;

        auto& aggregate = stageAggregates[static_cast<uint8_t>(record.stage)];
        aggregate.stage = record.stage;
        ++aggregate.recordCount;
        aggregate.totalBytes += record.byteCount;
        aggregate.totalElapsed += record.elapsed;
        if (!aggregate.hasSlowestPath ||
            record.elapsed > aggregate.maxElapsed ||
            (record.elapsed == aggregate.maxElapsed && record.path < aggregate.slowestPath))
        {
            aggregate.maxElapsed = record.elapsed;
            aggregate.slowestPath = record.path;
            aggregate.hasSlowestPath = true;
        }
    }

    std::vector<StageAggregate> stageTotals;
    stageTotals.reserve(stageAggregates.size());
    for (const auto& [_, aggregate] : stageAggregates)
        stageTotals.push_back(aggregate);
    std::sort(
        stageTotals.begin(),
        stageTotals.end(),
        [](const StageAggregate& left, const StageAggregate& right)
        {
            if (left.totalElapsed != right.totalElapsed)
                return left.totalElapsed > right.totalElapsed;
            return std::string_view(ArtifactLoadTelemetryStageName(left.stage)) <
                std::string_view(ArtifactLoadTelemetryStageName(right.stage));
        });

    std::vector<NLS::Core::Assets::ArtifactLoadTelemetryStageSummary> thumbnailBuckets;
    thumbnailBuckets.reserve(summaries.size());
    for (const auto& summary : summaries)
    {
        if (IsThumbnailLatencyStage(summary.stage))
            thumbnailBuckets.push_back(summary);
    }
    std::sort(
        thumbnailBuckets.begin(),
        thumbnailBuckets.end(),
        [](const auto& left, const auto& right)
        {
            if (left.totalElapsed != right.totalElapsed)
                return left.totalElapsed > right.totalElapsed;
            const auto leftStage = std::string_view(ArtifactLoadTelemetryStageName(left.stage));
            const auto rightStage = std::string_view(ArtifactLoadTelemetryStageName(right.stage));
            if (leftStage != rightStage)
                return leftStage < rightStage;
            return left.path < right.path;
        });

    auto thumbnailDrawOutcomePathTotals = thumbnailDrawOutcomes.pathTotals;
    std::sort(
        thumbnailDrawOutcomePathTotals.begin(),
        thumbnailDrawOutcomePathTotals.end(),
        [](const NLS::Editor::Panels::AssetBrowserThumbnailDrawOutcomePathTotal& left,
           const NLS::Editor::Panels::AssetBrowserThumbnailDrawOutcomePathTotal& right)
        {
            if (left.count != right.count)
                return left.count > right.count;
            return left.path < right.path;
        });

    std::ostringstream report;
    report << "Thumbnail telemetry summary\n";
    report << "telemetryEnabled=" << (telemetryEnabled ? "true" : "false") << '\n';
    report << "thumbnailRecordCount=";
    size_t thumbnailRecordCount = 0u;
    for (const auto& aggregate : stageTotals)
        thumbnailRecordCount += aggregate.recordCount;
    report << thumbnailRecordCount << '\n';
    report << "stageBucketCount=" << thumbnailBuckets.size() << "\n\n";

    report << "Thumbnail draw outcomes\n";
    report << "- |draw=thumbnail records=" << thumbnailDrawOutcomes.thumbnailDrawCount << '\n';
    report << "- |draw=fallback records=" << thumbnailDrawOutcomes.fallbackDrawCount << '\n';
    report << "- |draw=type-fallback records=" << thumbnailDrawOutcomes.typeFallbackDrawCount << '\n';
    report << "- droppedPathRecords=" << thumbnailDrawOutcomes.droppedPathCount << "\n\n";

    report << "Thumbnail draw outcome paths\n";
    const size_t maxDrawOutcomePaths = std::min<size_t>(thumbnailDrawOutcomePathTotals.size(), 32u);
    for (size_t index = 0u; index < maxDrawOutcomePaths; ++index)
    {
        const auto& total = thumbnailDrawOutcomePathTotals[index];
        report << "- " << total.path << " records=" << total.count << '\n';
    }
    report << '\n';

    report << "Artifact stage totals (sorted by total elapsed)\n";
    for (const auto& total : allStageTotals)
    {
        report
            << "- " << ArtifactLoadTelemetryStageName(total.stage)
            << " records=" << total.recordCount
            << " totalMs=" << FormatTelemetryDurationMs(total.totalElapsed)
            << " totalBytes=" << total.totalBytes
            << '\n';
    }
    report << '\n';

    if (stageTotals.empty())
    {
        report << "No thumbnail telemetry records were captured.\n";
        report << "Browse Asset Browser thumbnails before closing the editor.\n";
        return report.str();
    }

    report << "Stage totals (sorted by total elapsed)\n";
    for (const auto& aggregate : stageTotals)
    {
        report
            << "- " << ArtifactLoadTelemetryStageName(aggregate.stage)
            << " records=" << aggregate.recordCount
            << " totalMs=" << FormatTelemetryDurationMs(aggregate.totalElapsed)
            << " avgMs=" << FormatTelemetryDurationMs(
                aggregate.recordCount == 0u
                    ? std::chrono::microseconds {}
                    : std::chrono::microseconds(
                        aggregate.totalElapsed.count() / static_cast<long long>(aggregate.recordCount)))
            << " maxMs=" << FormatTelemetryDurationMs(aggregate.maxElapsed)
            << " totalBytes=" << aggregate.totalBytes
            << " slowestPath=" << aggregate.slowestPath
            << '\n';
    }

    std::unordered_map<std::string, size_t> gpuPreviewQueueDecisionPathCounts;
    for (const auto& record : records)
    {
        if (record.stage != ArtifactLoadTelemetryStage::ThumbnailServiceGpuPreviewQueueDecision)
            continue;
        ++gpuPreviewQueueDecisionPathCounts[record.path];
    }
    std::vector<std::pair<std::string, size_t>> gpuPreviewQueueDecisionPaths;
    gpuPreviewQueueDecisionPaths.reserve(gpuPreviewQueueDecisionPathCounts.size());
    for (const auto& [path, count] : gpuPreviewQueueDecisionPathCounts)
        gpuPreviewQueueDecisionPaths.emplace_back(path, count);
    std::sort(
        gpuPreviewQueueDecisionPaths.begin(),
        gpuPreviewQueueDecisionPaths.end(),
        [](const auto& left, const auto& right)
        {
            if (left.second != right.second)
                return left.second > right.second;
            return left.first < right.first;
        });

    report << "\nGPU preview queue decisions\n";
    const size_t maxGpuPreviewQueueDecisionPaths =
        std::min<size_t>(gpuPreviewQueueDecisionPaths.size(), 32u);
    for (size_t index = 0u; index < maxGpuPreviewQueueDecisionPaths; ++index)
    {
        const auto& [path, count] = gpuPreviewQueueDecisionPaths[index];
        report << "- " << path << " records=" << count << '\n';
    }

    report << "\nTop buckets (stage + path)\n";
    const size_t maxBuckets = std::min<size_t>(thumbnailBuckets.size(), 16u);
    for (size_t index = 0u; index < maxBuckets; ++index)
    {
        const auto& bucket = thumbnailBuckets[index];
        report
            << "- " << ArtifactLoadTelemetryStageName(bucket.stage)
            << " path=" << bucket.path
            << " records=" << bucket.recordCount
            << " totalMs=" << FormatTelemetryDurationMs(bucket.totalElapsed)
            << " avgMs=" << FormatTelemetryDurationMs(
                bucket.recordCount == 0u
                    ? std::chrono::microseconds {}
                    : std::chrono::microseconds(
                        bucket.totalElapsed.count() / static_cast<long long>(bucket.recordCount)))
            << " totalBytes=" << bucket.totalBytes
            << '\n';
    }
    return report.str();
}

std::string BuildThumbnailTelemetrySummaryReport()
{
    const auto records = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    const auto drawOutcomes = NLS::Editor::Panels::SnapshotAssetBrowserThumbnailDrawOutcomeTelemetry();
    return NLS::Editor::Core::BuildThumbnailTelemetrySummaryReport(
        records,
        drawOutcomes,
        NLS::Core::Assets::IsArtifactLoadTelemetryEnabled());
}

void WriteThumbnailTelemetrySummaryIfRequested(
    const NLS::Editor::Core::Context& context,
    const bool logSuccess = true)
{
    const auto& outputSetting = context.GetDiagnosticsSettings().editorThumbnailTelemetrySummaryOutput;
    if (outputSetting.empty())
        return;

    auto outputPath = std::filesystem::path(outputSetting);
    if (outputPath.is_relative())
        outputPath = std::filesystem::path(context.projectPath) / outputPath;

    std::error_code error;
    if (!outputPath.parent_path().empty())
        std::filesystem::create_directories(outputPath.parent_path(), error);

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        NLS_LOG_ERROR("Failed to open thumbnail telemetry summary output: " + outputPath.generic_string());
        return;
    }

    output << BuildThumbnailTelemetrySummaryReport();
    output.close();
    if (logSuccess)
        NLS_LOG_INFO("Wrote thumbnail telemetry summary: " + outputPath.generic_string());
}

std::string FormatValidationVector3(const NLS::Maths::Vector3& value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3)
        << value.x << ','
        << value.y << ','
        << value.z;
    return stream.str();
}

std::string BuildPrefabDragProxyValidationSummaryReport(NLS::Editor::Panels::SceneView* sceneView = nullptr)
{
    const std::array<NLS::Maths::Vector3, 3u> placements = {
        NLS::Maths::Vector3 { 0.0f, 0.0f, 0.0f },
        NLS::Maths::Vector3 { 1.5f, 0.25f, -2.0f },
        NLS::Maths::Vector3 { -0.75f, 1.0f, 3.25f }
    };

    NLS::Render::Debug::DebugDrawService debugDrawService;
    debugDrawService.SetEnabled(false);
    debugDrawService.SetCategoryEnabled(NLS::Render::Debug::DebugDrawCategory::General, false);

    bool proxyVisibleBeforeRoot = false;
    bool debugDrawForcedVisible = false;
    bool followedPlacement = true;
    size_t visibleLineCount = 0u;
    uint32_t submittedPrimitiveGroups = 0u;
    std::chrono::microseconds totalSubmitTime {};
    std::vector<NLS::Maths::Vector3> reportedPlacements;
    reportedPlacements.reserve(placements.size());
#if defined(NLS_ENABLE_TEST_HOOKS)
    NLS::Editor::Panels::SceneView::PrefabDragProxySceneViewLoopValidation sceneViewLoopValidation;
    if (sceneView != nullptr)
    {
        const NLS::Editor::Assets::EditorAssetDragPayload validationPayload {};
        sceneViewLoopValidation = sceneView->ValidatePrefabDragProxySceneViewLoopForTesting(
            validationPayload,
            std::vector<NLS::Maths::Vector3>(placements.begin(), placements.end()));
    }
#endif

    for (const auto& placement : placements)
    {
        const auto begin = std::chrono::steady_clock::now();
        const auto descriptor = NLS::Editor::Panels::BuildSceneViewPrefabDragProxyDescriptor(
            std::optional<NLS::Maths::Vector3> { placement },
            true,
            nullptr);
        totalSubmitTime += std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin);

        if (!descriptor.has_value())
        {
            followedPlacement = false;
            continue;
        }

        proxyVisibleBeforeRoot = true;
        followedPlacement = followedPlacement &&
            descriptor->position.x == placement.x &&
            descriptor->position.y == placement.y &&
            descriptor->position.z == placement.z;
        reportedPlacements.push_back(descriptor->position);

        const auto submitBegin = std::chrono::steady_clock::now();
        submittedPrimitiveGroups += NLS::Editor::Rendering::SubmitPrefabDragProxyDebugPrimitives(
            debugDrawService,
            *descriptor);
        debugDrawForcedVisible = debugDrawForcedVisible ||
            (debugDrawService.IsEnabled() &&
                debugDrawService.IsCategoryEnabled(NLS::Render::Debug::DebugDrawCategory::General));
        totalSubmitTime += std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - submitBegin);

        visibleLineCount += debugDrawService.CollectVisibleLines().size();
        debugDrawService.Clear();
        debugDrawService.SetEnabled(false);
        debugDrawService.SetCategoryEnabled(NLS::Render::Debug::DebugDrawCategory::General, false);
    }

    std::ostringstream report;
    report << "Prefab drag proxy validation summary\n";
    report << (proxyVisibleBeforeRoot ? "proxyVisibleBeforeRoot=true\n" : "proxyVisibleBeforeRoot=false\n");
    report << (followedPlacement ? "followedPlacement=true\n" : "followedPlacement=false\n");
    report << "samplePlacementCount=" << placements.size() << '\n';
    report << "reportedPlacementCount=" << reportedPlacements.size() << '\n';
    report << "visibleLineCount=" << visibleLineCount << '\n';
    report << "submittedPrimitiveGroups=" << submittedPrimitiveGroups << '\n';
    report << "debugDrawForcedVisible=" << (debugDrawForcedVisible ? "true" : "false") << '\n';
    report << "sceneRootCreatedByProxy=false\n";
    report << "prefabArtifactLoadRequested=false\n";
#if defined(NLS_ENABLE_TEST_HOOKS)
    report << (sceneViewLoopValidation.dragLoopExercised
        ? "sceneViewDragLoopExercised=true\n"
        : "sceneViewDragLoopExercised=false\n");
    report << (sceneViewLoopValidation.payloadAcceptedBeforeDelivery
        ? "sceneViewDragLoopPayloadAcceptedBeforeDelivery=true\n"
        : "sceneViewDragLoopPayloadAcceptedBeforeDelivery=false\n");
    report << (sceneViewLoopValidation.followedPlacement
        ? "sceneViewDragLoopFollowedPlacement=true\n"
        : "sceneViewDragLoopFollowedPlacement=false\n");
    report << (sceneViewLoopValidation.sceneRootCreatedByProxy
        ? "sceneViewDragLoopSceneRootCreated=true\n"
        : "sceneViewDragLoopSceneRootCreated=false\n");
    report << "sceneViewDragLoopProxyDescriptorCount="
        << sceneViewLoopValidation.descriptorPlacements.size() << '\n';
#else
    report << "sceneViewDragLoopExercised=false\n";
    report << "sceneViewDragLoopPayloadAcceptedBeforeDelivery=false\n";
    report << "sceneViewDragLoopFollowedPlacement=false\n";
    report << "sceneViewDragLoopSceneRootCreated=false\n";
    report << "sceneViewDragLoopProxyDescriptorCount=0\n";
#endif
    report << "totalProxySubmitMicros=" << totalSubmitTime.count() << '\n';
    for (size_t index = 0u; index < reportedPlacements.size(); ++index)
    {
        report << "placement" << index << '=' << FormatValidationVector3(reportedPlacements[index]) << '\n';
    }
    return report.str();
}

void WritePrefabDragProxyValidationSummaryIfRequested(
    const NLS::Editor::Core::Context& context,
    NLS::Editor::Panels::SceneView* sceneView,
    const bool logSuccess = true)
{
    const auto& outputSetting = context.GetDiagnosticsSettings().editorValidationPrefabDragProxySummaryOutput;
    if (outputSetting.empty())
        return;

    auto outputPath = std::filesystem::path(outputSetting);
    if (outputPath.is_relative())
        outputPath = std::filesystem::path(context.projectPath) / outputPath;

    std::error_code error;
    if (!outputPath.parent_path().empty())
        std::filesystem::create_directories(outputPath.parent_path(), error);

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        NLS_LOG_ERROR("Failed to open prefab drag proxy validation summary output: " + outputPath.generic_string());
        return;
    }

    output << BuildPrefabDragProxyValidationSummaryReport(sceneView);
    output.close();
    if (logSuccess)
        NLS_LOG_INFO("Wrote prefab drag proxy validation summary: " + outputPath.generic_string());
}
}

namespace Editor::Core
{
std::string BuildThumbnailTelemetrySummaryReport(
    const std::vector<NLS::Core::Assets::ArtifactLoadTelemetryRecord>& records,
    const NLS::Editor::Panels::AssetBrowserThumbnailDrawOutcomeTelemetrySnapshot& drawOutcomes,
    const bool telemetryEnabled)
{
    return FormatThumbnailTelemetrySummaryReport(records, drawOutcomes, telemetryEnabled);
}
}

Editor::Core::EditorJobWorkerBudget Editor::Core::ResolveEditorJobWorkerBudget(
    const uint32_t hardwareConcurrency,
    const std::optional<uint32_t> backgroundWorkerOverride)
{
    constexpr uint32_t kFallbackHardwareConcurrency = 4u;
    constexpr uint32_t kMaximumBackgroundWorkerCount = 16u;
    const uint32_t availableConcurrency = hardwareConcurrency == 0u
        ? kFallbackHardwareConcurrency
        : hardwareConcurrency;
    constexpr uint32_t kReservedEditorExecutionLanes = 3u;
    const uint32_t totalWorkerBudget = std::min(
        std::max(
            2u,
            availableConcurrency > kReservedEditorExecutionLanes
                ? availableConcurrency - kReservedEditorExecutionLanes
                : 2u),
        31u);
    const uint32_t automaticBackgroundWorkerCount = totalWorkerBudget >= 15u
        ? std::min(kMaximumBackgroundWorkerCount, totalWorkerBudget - 1u)
        : std::max(1u, totalWorkerBudget * 3u / 4u);
    const uint32_t backgroundWorkerCount = backgroundWorkerOverride.has_value()
        ? std::clamp(*backgroundWorkerOverride, 1u, std::min(kMaximumBackgroundWorkerCount, totalWorkerBudget - 1u))
        : automaticBackgroundWorkerCount;
    return {
        totalWorkerBudget - backgroundWorkerCount,
        backgroundWorkerCount};
}

Editor::Core::Editor::JobSystemLifetime::JobSystemLifetime()
{
    std::optional<uint32_t> backgroundWorkerOverride;
    if (const char* value = std::getenv("NLS_EDITOR_BACKGROUND_WORKERS"))
    {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end != value && *end == '\0' && parsed <= std::numeric_limits<uint32_t>::max())
            backgroundWorkerOverride = static_cast<uint32_t>(parsed);
    }
    const auto workerBudget = ResolveEditorJobWorkerBudget(
        std::thread::hardware_concurrency(),
        backgroundWorkerOverride);
    NLS::Base::Jobs::JobSystemConfig config;
    config.workerCount = workerBudget.foregroundWorkerCount;
    config.backgroundWorkerCount = workerBudget.backgroundWorkerCount;
    ownsJobSystem = NLS::Base::Jobs::TryInitializeJobSystem(config);
    NLS_LOG_INFO(
        "Editor job system foregroundWorkers=" +
        std::to_string(NLS::Base::Jobs::GetJobWorkerCount()) +
        " backgroundWorkers=" +
        std::to_string(NLS::Base::Jobs::GetBackgroundJobWorkerCount()) +
        " requestedForegroundWorkers=" +
        std::to_string(config.workerCount) +
        " requestedBackgroundWorkers=" +
        std::to_string(config.backgroundWorkerCount) +
        " hardwareConcurrency=" +
        std::to_string(std::thread::hardware_concurrency()) +
        " ownsJobSystem=" +
        (ownsJobSystem ? "true" : "false"));
}

Editor::Core::Editor::JobSystemLifetime::~JobSystemLifetime()
{
    Shutdown();
}

void Editor::Core::Editor::JobSystemLifetime::Shutdown()
{
    if (!ownsJobSystem)
        return;

    ownsJobSystem = false;
    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::DrainAcceptedWork);
}

Editor::Core::Editor::Editor(Context& p_context)
    : m_context(p_context), m_panelsManager(m_canvas),
    m_jobSystemLifetime(),
    m_editorActions(m_context, m_panelsManager)
{
    NLS::Base::Profiling::Profiler::SetEnabled(false);
    NLS::Base::Profiling::Profiler::RegisterDestination(g_tracyProfiler);

    NLS::Core::ServiceLocator::Provide<NLS::Editor::Core::Context>(m_context);
    NLS::Core::ServiceLocator::Provide<NLS::Editor::Core::Editor>(*this);
    NLS::Core::ServiceLocator::Provide<NLS::Editor::Shortcuts::EditorShortcutService>(m_shortcutService);
    Assembly::Instance().Instance().Load<AssemblyMath>().Load<AssemblyCore>().Load<AssemblyPlatform>().Load<AssemblyRender>().Load<Engine::AssemblyEngine>();

    if (!m_context.GetDiagnosticsSettings().editorThumbnailTelemetrySummaryOutput.empty())
    {
        NLS::Core::Assets::SetArtifactLoadTelemetryEnabled(true);
        NLS::Core::Assets::ClearArtifactLoadTelemetry();
        NLS_LOG_INFO(
            "Thumbnail telemetry summary export enabled: " +
            m_context.GetDiagnosticsSettings().editorThumbnailTelemetrySummaryOutput);
    }

    m_context.PresentStartupProgressFrame("Preparing editor panels", 0.55f);
    NLS_LOG_INFO("[Startup] SetupUI begin");
    SetupUI();
    NLS_LOG_INFO("[Startup] SetupUI end");
    m_context.PresentStartupProgressFrame("Preparing editor shortcuts", 0.62f);
    PublishReflectionDiagnosticsToLog();
    RegisterShortcutContexts();
    RegisterDefaultShortcuts();
    m_sceneSourcePathChangedListener = m_context.sceneManager.CurrentSceneSourcePathChangedEvent += [this](const std::string& p_scenePath)
    {
        RememberLastOpenedScene(p_scenePath);
    };

    m_context.PresentStartupProgressFrame("Loading startup scene models and shaders", 0.65f);

    try
    {
        NLS::Core::ResourceManagement::ResourceLoadProgressScope resourceLoadProgressScope(
            [this](const NLS::Core::ResourceManagement::ResourceLoadProgress& progress)
            {
                const float startupProgress = progress.completed ? 0.86f : 0.82f;
                m_context.PresentStartupProgressFrame(progress.message, startupProgress);
            });
        NLS_LOG_INFO("[Startup] RestoreStartupScene begin");
        RestoreStartupScene();
        NLS_LOG_INFO("[Startup] RestoreStartupScene end");

        m_context.PresentStartupProgressFrame("Applying startup editor state", 0.90f);
        NLS_LOG_INFO("[Startup] ApplyStartupValidationDirectives begin");
        ApplyStartupValidationDirectives();
        NLS_LOG_INFO("[Startup] ApplyStartupValidationDirectives end");
        m_context.PresentStartupProgressFrame("Preparing first editor frame", 0.94f);
    }
    catch (const std::exception& exception)
    {
        NLS_LOG_ERROR(std::string("Startup scene load failed: ") + exception.what());
        throw;
    }
    catch (...)
    {
        NLS_LOG_ERROR("Startup scene load failed with an unknown exception.");
        throw;
    }
}

Editor::Core::Editor::~Editor()
{
    m_shortcutService.SaveProfile(std::filesystem::path(m_context.projectPath) / "UserSettings" / "shortcuts.json");
    m_editorActions.PromptSaveCurrentSceneIfDirty();
    WriteThumbnailTelemetrySummaryIfRequested(m_context);
    m_jobSystemLifetime.Shutdown();
    NLS::Base::Profiling::Profiler::UnregisterDestination(
        m_panelsManager.GetPanelAs<Panels::ProfilerPanel>("Profiler").GetTimelineSink());
    m_panelsManager.DestroyPanels();
    m_context.sceneManager.CurrentSceneSourcePathChangedEvent -= m_sceneSourcePathChangedListener;
    m_context.sceneManager.UnloadCurrentScene();
}

void Editor::Core::Editor::SetupUI()
{
    const auto setupUiBegin = std::chrono::steady_clock::now();
    auto logSetupStep =
        [last = setupUiBegin](const char* step) mutable
        {
            const auto now = std::chrono::steady_clock::now();
            NLS_LOG_INFO(
                std::string("[Startup] SetupUI step ") +
                step +
                " elapsedMs=" +
                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count()));
            last = now;
        };

    NLS::UI::PanelWindowSettings settings;
    settings.closable = true;
    settings.collapsable = true;
    settings.dockable = true;
    auto frameInfoSettings = settings;
    frameInfoSettings.autoSize = true;

    m_panelsManager.CreatePanel<Panels::EditorTopBar>("Editor Top Bar");
    logSetupStep("EditorTopBar");
    m_panelsManager.CreatePanel<Panels::EditorStatusBar>("Editor Status Bar");
    logSetupStep("EditorStatusBar");
    m_panelsManager.CreatePanel<Panels::AssetBrowser>("Asset Browser", true, settings, m_context.engineAssetsPath, m_context.projectAssetsPath);
    logSetupStep("AssetBrowser");
    m_panelsManager.CreatePanel<Panels::ProfilerPanel>("Profiler", false, settings);
    logSetupStep("Profiler");
    auto& profilerPanel = m_panelsManager.GetPanelAs<Panels::ProfilerPanel>("Profiler");
    NLS::Base::Profiling::Profiler::RegisterDestination(profilerPanel.GetTimelineSink());
    logSetupStep("RegisterProfiler");
    m_panelsManager.CreatePanel<Panels::Console>("Console", false, settings);
    logSetupStep("Console");
    m_panelsManager.CreatePanel<Panels::AssetView>("Asset View", false, settings);
    logSetupStep("AssetView");
    m_panelsManager.CreatePanel<Panels::Hierarchy>("Hierarchy", true, settings);
    logSetupStep("Hierarchy");
    m_panelsManager.CreatePanel<Panels::Inspector>("Inspector", true, settings);
    logSetupStep("Inspector");
    m_panelsManager.CreatePanel<Panels::SceneView>("Scene View", true, settings);
    logSetupStep("SceneView");
    m_panelsManager.CreatePanel<Panels::GameView>("Game View", false, settings);
    logSetupStep("GameView");
    m_panelsManager.CreatePanel<Panels::FrameInfo>("Frame Info", false, frameInfoSettings);
    logSetupStep("FrameInfo");
    m_panelsManager.CreatePanel<Panels::MaterialEditor>("Material Editor", false, settings);
    logSetupStep("MaterialEditor");
    m_panelsManager.CreatePanel<Panels::ProjectSettings>("Project Settings", false, settings);
    logSetupStep("ProjectSettings");
    m_panelsManager.GetPanelAs<Panels::ProjectSettings>("Project Settings").enabled = false;
    m_panelsManager.CreatePanel<Panels::AssetProperties>("Asset Properties", false, settings);
    logSetupStep("AssetProperties");
    auto& topBar = m_panelsManager.GetPanelAs<Panels::EditorTopBar>("Editor Top Bar");
    topBar.RegisterProjectSettingsPanel(m_panelsManager.GetPanelAs<Panels::ProjectSettings>("Project Settings"));
    topBar.RegisterWindowPanel("Asset Browser", m_panelsManager.GetPanelAs<Panels::AssetBrowser>("Asset Browser"));
    topBar.RegisterWindowPanel("Frame Info", m_panelsManager.GetPanelAs<Panels::FrameInfo>("Frame Info"));
    topBar.RegisterWindowPanel("Profiler", profilerPanel);
    topBar.RegisterWindowPanel("Console", m_panelsManager.GetPanelAs<Panels::Console>("Console"));
    topBar.RegisterWindowPanel("Asset View", m_panelsManager.GetPanelAs<Panels::AssetView>("Asset View"));
    topBar.RegisterWindowPanel("Hierarchy", m_panelsManager.GetPanelAs<Panels::Hierarchy>("Hierarchy"));
    topBar.RegisterWindowPanel("Inspector", m_panelsManager.GetPanelAs<Panels::Inspector>("Inspector"));
    topBar.RegisterWindowPanel("Scene View", m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View"));
    topBar.RegisterWindowPanel("Game View", m_panelsManager.GetPanelAs<Panels::GameView>("Game View"));
    topBar.RegisterWindowPanel("Material Editor", m_panelsManager.GetPanelAs<Panels::MaterialEditor>("Material Editor"));
    topBar.RegisterWindowPanel("Asset Properties", m_panelsManager.GetPanelAs<Panels::AssetProperties>("Asset Properties"));
    logSetupStep("RegisterWindows");
    // Needs to be called after all panels got created, because some settings in this menu depend on other panels
    topBar.InitializeSettingsMenu();
    logSetupStep("InitializeSettingsMenu");
    m_canvas.MakeDockspace(true);
    logSetupStep("MakeDockspace");
    m_context.uiManager->SetCanvas(m_canvas);
    logSetupStep("SetCanvas");
    m_context.uiManager->ResetLayout(m_context.projectPath + "/UserSettings/layout.ini");
    logSetupStep("ResetLayout");
    m_panelsManager.GetPanelAs<Panels::GameView>("Game View").SetOpened(false);
    logSetupStep("CloseGameViewForStartup");
}

void Editor::Core::Editor::PreUpdate()
{
    RefreshProfilerRecordingState();
    NLS_PROFILE_SCOPE();
    {
        NLS_PROFILE_NAMED_SCOPE("Editor::PollEvents");
        m_context.device->PollEvents();
    }
}

void Editor::Core::Editor::BeginProfilerFrame()
{
    m_validationCameraMotionPendingForFrame = true;
    RefreshProfilerRecordingState();
    m_panelsManager.GetPanelAs<Panels::ProfilerPanel>("Profiler").BeginProfilerFrame();
    UpdateValidationTimelineTraceExport();
}

void Editor::Core::Editor::UpdateValidationTimelineTraceExport()
{
    const auto& diagnostics = m_context.GetDiagnosticsSettings();
    const uint32_t requestedFrames = diagnostics.editorValidationTimelineTraceFrames;
    if (requestedFrames == 0u || m_validationTraceExportFinished)
        return;

    auto& profilerPanel = m_panelsManager.GetPanelAs<Panels::ProfilerPanel>("Profiler");
    auto& timelineSink = profilerPanel.GetTimelineSink();

    if (!m_validationTraceExportStarted)
    {
        if (!profilerPanel.IsRecordingEnabled())
            return;

        const std::filesystem::path logFilePath(NLS::Debug::FileHandler::GetLogFilePath());
        const auto logDirectory = logFilePath.parent_path();
        m_validationTracePath = !logDirectory.empty()
            ? logDirectory / "trace.json"
            : std::filesystem::path(m_context.projectPath) / "Logs" / "trace.json";
        if (!timelineSink.BeginTraceExport(m_validationTracePath))
        {
            NLS_LOG_WARNING("Editor validation TimelineProfiler trace failed to open: " + m_validationTracePath.string());
            m_validationTraceExportFinished = true;
            return;
        }

        m_validationTraceExportStarted = true;
        NLS_LOG_INFO(
            "Editor validation TimelineProfiler trace started: " +
            m_validationTracePath.string() +
            " frames=" +
            std::to_string(requestedFrames));

        // Keep recording active through the sink, but remove the profiler panel's
        // timeline rendering from the measured editor frame.
        profilerPanel.Close();
        NLS_LOG_INFO("Editor validation TimelineProfiler panel hidden during export.");
    }

    const uint32_t exportedFrameCount = timelineSink.GetTraceExportedFrameCount();
    const uint32_t remainingFrames = requestedFrames > exportedFrameCount
        ? requestedFrames - exportedFrameCount
        : 0u;
    timelineSink.UpdateTraceExport(std::min(8u, remainingFrames));

    if (timelineSink.GetTraceExportedFrameCount() >= requestedFrames)
    {
        timelineSink.EndTraceExport();
        m_validationTraceExportFinished = true;
        NLS_LOG_INFO(
            "Editor validation TimelineProfiler trace finished: " +
            m_validationTracePath.string() +
            " exportedFrames=" +
            std::to_string(timelineSink.GetTraceExportedFrameCount()));
        WriteThumbnailTelemetrySummaryIfRequested(m_context, false);
        if (m_context.window != nullptr)
            m_context.window->SetShouldClose(true);
    }
}

void Editor::Core::Editor::UpdateThumbnailTelemetrySummaryExport()
{
    if (m_context.GetDiagnosticsSettings().editorThumbnailTelemetrySummaryOutput.empty())
        return;

    const auto now = std::chrono::steady_clock::now();
    if (m_lastThumbnailTelemetrySummaryWriteTime.time_since_epoch().count() != 0 &&
        now - m_lastThumbnailTelemetrySummaryWriteTime < kThumbnailTelemetrySummaryWriteInterval)
    {
        return;
    }

    if (!m_thumbnailTelemetrySummaryWriteAttemptLogged)
    {
        NLS_LOG_INFO("Thumbnail telemetry summary export tick reached.");
        m_thumbnailTelemetrySummaryWriteAttemptLogged = true;
    }

    WriteThumbnailTelemetrySummaryIfRequested(m_context, false);
    m_lastThumbnailTelemetrySummaryWriteTime = now;
}

void Editor::Core::Editor::Update(float p_deltaTime)
{
    NLS_PROFILE_SCOPE();

    const bool logUpdateStages = m_logNextUpdateStages;
    m_logNextUpdateStages = false;
    auto updateStageBegin = std::chrono::steady_clock::now();
    auto logUpdateStage =
        [&updateStageBegin, logUpdateStages](const char* stage)
        {
            if (!logUpdateStages)
                return;

            const auto now = std::chrono::steady_clock::now();
            NLS_LOG_INFO(
                std::string("[Startup] FirstEditorUpdate stage ") +
                stage +
                " elapsedMs=" +
                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now - updateStageBegin).count()));
            updateStageBegin = now;
        };

    m_currentDeltaTime = p_deltaTime;
    if (p_deltaTime > std::numeric_limits<float>::epsilon())
    {
        m_frameRateAccumulatedTime += p_deltaTime;
        ++m_frameRateSampleCount;

        if (m_frameRateAccumulatedTime >= 1.0f)
        {
            m_currentFrameRate = static_cast<float>(m_frameRateSampleCount) / m_frameRateAccumulatedTime;
            if (m_context.GetDiagnosticsSettings().logEditorFps)
                NLS_LOG_INFO("Editor FPS: " + std::to_string(m_currentFrameRate));

            m_frameRateAccumulatedTime = 0.0f;
            m_frameRateSampleCount = 0;
        }
    }
    logUpdateStage("FrameRate");

    UpdateThumbnailTelemetrySummaryExport();
    logUpdateStage("UpdateThumbnailTelemetrySummaryExport");

    {
        NLS_PROFILE_NAMED_SCOPE("Editor::HandleGlobalShortcuts");
        HandleGlobalShortcuts();
    }
    logUpdateStage("HandleGlobalShortcuts");
    {
        NLS_PROFILE_NAMED_SCOPE("Editor::UpdateCurrentEditorMode");
        UpdateCurrentEditorMode(p_deltaTime);
    }
    logUpdateStage("UpdateCurrentEditorMode");
    {
        NLS_PROFILE_NAMED_SCOPE("Editor::UpdateViews");
        UpdateViews(p_deltaTime);
    }
    logUpdateStage("UpdateViews");
    UpdateValidationSceneCameraMotion();
    logUpdateStage("UpdateValidationSceneCameraMotion");
    {
        NLS_PROFILE_NAMED_SCOPE("Editor::UpdateEditorPanels");
        UpdateEditorPanels(p_deltaTime);
    }
    logUpdateStage("UpdateEditorPanels");
    RenderEditorUI(p_deltaTime);
    logUpdateStage("RenderEditorUI");
    {
        NLS_PROFILE_NAMED_SCOPE("EditorActions::ExecuteDelayedActions");
        m_editorActions.ExecuteDelayedActions();
    }
    logUpdateStage("ExecuteDelayedActions");
    {
        NLS_PROFILE_NAMED_SCOPE("JobSystem::DrainMainThreadContinuations");
        NLS::Base::Jobs::DrainMainThreadContinuations(kEditorMainThreadContinuationDrainBudget);
    }
    logUpdateStage("DrainMainThreadContinuations");
    {
        NLS_PROFILE_NAMED_SCOPE("Editor::PublishReflectionDiagnosticsToLog");
        PublishReflectionDiagnosticsToLog();
    }
    logUpdateStage("PublishReflectionDiagnosticsToLog");
}

void Editor::Core::Editor::LogNextUpdateStages()
{
    m_logNextUpdateStages = true;
}

void Editor::Core::Editor::RefreshProfilerRecordingState()
{
    auto& profilerPanel = m_panelsManager.GetPanelAs<Panels::ProfilerPanel>("Profiler");
    const bool recording =
        profilerPanel.IsRecordingEnabled() ||
        profilerPanel.GetTimelineSink().IsTraceExportOpen();
    profilerPanel.GetTimelineSink().SetRecordingEnabled(recording);

    const bool tracyConnected = NLS::Base::Profiling::TracyProfiler::IsConnected();
    NLS::Base::Profiling::Profiler::SetEnabled(tracyConnected || recording);
}

bool Editor::Core::Editor::IsProfilerRecordingEnabled()
{
    return m_panelsManager.GetPanelAs<Panels::ProfilerPanel>("Profiler").IsRecordingEnabled();
}

void Editor::Core::Editor::DeferStartupSceneViewRenderForNextFrame()
{
    auto& sceneView = m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View");
    sceneView.RequestSkipNextRenderFrame();
}

void Editor::Core::Editor::UpdateValidationSceneCameraMotion()
{
    const auto& diagnostics = m_context.GetDiagnosticsSettings();
    const uint32_t requestedFrames = diagnostics.editorValidationCameraForwardFrames;
    if (!m_validationCameraMotionPendingForFrame || requestedFrames == 0u)
    {
        return;
    }

    if (diagnostics.editorValidationTimelineTraceFrames != 0u &&
        (!m_validationTraceExportStarted || m_validationTraceExportFinished))
    {
        return;
    }

    auto& sceneView = m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View");
    if (m_validationCameraForwardCompletedFrames >= requestedFrames)
    {
        sceneView.SetValidationCameraMotionActive(false);
        m_validationCameraMotionPendingForFrame = false;
        return;
    }

    NLS_PROFILE_NAMED_SCOPE("EditorValidation::MoveSceneCameraForward");
    if (m_validationCameraForwardCompletedFrames == 0u)
    {
        NLS_LOG_INFO(
            "Editor validation Scene View camera forward motion started: frames=" +
            std::to_string(requestedFrames) +
            " fixedStep=" +
            std::to_string(kEditorValidationCameraForwardStep));
    }

    sceneView.ApplyValidationCameraForwardStep(kEditorValidationCameraForwardStep);

    m_validationCameraMotionPendingForFrame = false;
    ++m_validationCameraForwardCompletedFrames;
    if (m_validationCameraForwardCompletedFrames == requestedFrames)
    {
        NLS_LOG_INFO(
            "Editor validation Scene View camera forward motion finished: completedFrames=" +
            std::to_string(m_validationCameraForwardCompletedFrames));
    }
}

void Editor::Core::Editor::HandleGlobalShortcuts()
{
    m_shortcutService.ExecutePressedShortcut(
        [this](Windowing::Inputs::EKey p_key)
        {
            return m_context.inputManager->IsKeyPressed(p_key);
        },
        [this](Windowing::Inputs::EKey p_key)
        {
            return m_context.inputManager->GetKeyState(p_key);
        });
}

void Editor::Core::Editor::RegisterShortcutContexts()
{
    using namespace NLS::Editor::Shortcuts;
    using namespace Windowing::Inputs;

    const auto isSceneViewFocused = [this]
    {
        return m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View").IsFocused();
    };

    m_shortcutService.RegisterContext({
        ShortcutContexts::SceneView,
        "Scene View",
        20,
        "focused-panel",
        isSceneViewFocused });

    m_shortcutService.RegisterContext({
        ShortcutContexts::SceneViewFlyMode,
        "Scene View/Fly Mode",
        40,
        "scene-navigation-mode",
        [this, isSceneViewFocused]
        {
            return isSceneViewFocused() &&
                m_context.inputManager->GetMouseButtonState(EMouseButton::MOUSE_BUTTON_RIGHT) == EMouseButtonState::MOUSE_DOWN;
        }});

    m_shortcutService.RegisterContext({
        ShortcutContexts::Hierarchy,
        "Hierarchy",
        20,
        "focused-panel",
        [this]
        {
            return m_panelsManager.GetPanelAs<Panels::Hierarchy>("Hierarchy").IsFocused();
        }});

    m_shortcutService.RegisterContext({
        ShortcutContexts::TextInput,
        "Text Input",
        100,
        "",
        []
        {
            return ImGui::GetIO().WantTextInput;
        }});
}

void Editor::Core::Editor::RegisterDefaultShortcuts()
{
    using namespace NLS::Editor::Shortcuts;
    using Windowing::Inputs::EKey;

    const auto registerCommand = [this](ShortcutCommand p_command)
    {
        m_shortcutService.RegisterCommand(std::move(p_command));
    };

    auto makeCommand = [](std::string p_id,
                          std::string p_displayName,
                          std::string p_category,
                          ShortcutContextId p_context,
                          ShortcutBinding p_binding,
                          std::function<void()> p_execute)
    {
        ShortcutCommand command;
        command.id = std::move(p_id);
        command.displayName = std::move(p_displayName);
        command.category = std::move(p_category);
        command.context = std::move(p_context);
        command.defaultBinding = p_binding;
        command.execute = std::move(p_execute);
        return command;
    };

    registerCommand(makeCommand(
        "file.new-scene",
        "New Scene",
        "File",
        ShortcutContexts::Global,
        ShortcutBinding::FromKey(EKey::KEY_N, EShortcutModifier::Ctrl),
        [this] { m_editorActions.LoadEmptyScene(); }));

    registerCommand(makeCommand(
        "file.save-scene",
        "Save Scene",
        "File",
        ShortcutContexts::Global,
        ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl),
        [this] { m_editorActions.SaveSceneChanges(); }));

    registerCommand(makeCommand(
        "file.save-scene-as",
        "Save Scene As",
        "File",
        ShortcutContexts::Global,
        ShortcutBinding::FromKey(EKey::KEY_S, EShortcutModifier::Ctrl | EShortcutModifier::Shift),
        [this] { m_editorActions.SaveAs(); }));

    registerCommand(makeCommand(
        "editor.play",
        "Play",
        "Editor",
        ShortcutContexts::Global,
        ShortcutBinding::FromKey(EKey::KEY_F5),
        [this] { m_editorActions.StartPlaying(); }));

    auto stopCommand = makeCommand(
        "editor.stop",
        "Stop",
        "Editor",
        ShortcutContexts::Global,
        ShortcutBinding::FromKey(EKey::KEY_ESCAPE),
        [this] { m_editorActions.StopPlaying(); });
    stopCommand.availability = [this]
    {
        return m_editorActions.GetCurrentEditorMode() != EditorActions::EEditorMode::EDIT;
    };
    registerCommand(std::move(stopCommand));

    auto captureCommand = makeCommand(
        "debug.renderdoc.capture-next-frame",
        "Capture Next Frame",
        "Debugging",
        ShortcutContexts::Global,
        ShortcutBinding::FromKey(EKey::KEY_F11),
        [this]
        {
            if (Render::Context::DriverUIAccess::IsRenderDocAvailable(*m_context.driver))
                Render::Context::DriverUIAccess::QueueRenderDocCapture(*m_context.driver, "Editor");
        });
    captureCommand.allowDuringTextInput = true;
    registerCommand(std::move(captureCommand));

    auto openCaptureCommand = makeCommand(
        "debug.renderdoc.open-latest-capture",
        "Open Latest RenderDoc Capture",
        "Debugging",
        ShortcutContexts::Global,
        ShortcutBinding::FromKey(EKey::KEY_F11, EShortcutModifier::Ctrl),
        [this]
        {
            Render::Context::DriverUIAccess::OpenLatestRenderDocCapture(*m_context.driver);
        });
    openCaptureCommand.allowDuringTextInput = true;
    registerCommand(std::move(openCaptureCommand));

    const auto registerSceneToolCommand = [&](std::string p_id,
                                              std::string p_displayName,
                                              const EKey p_key,
                                              std::function<void(Panels::SceneView&)> p_execute)
    {
        registerCommand(makeCommand(
            std::move(p_id),
            std::move(p_displayName),
            "Scene View",
            ShortcutContexts::SceneView,
            ShortcutBinding::FromKey(p_key),
            [this, execute = std::move(p_execute)]
            {
                execute(m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View"));
            }));
    };

    registerCommand(makeCommand(
        "scene-view.fly-forward",
        "Fly Mode Forward",
        "Scene View/Fly Mode",
        ShortcutContexts::SceneViewFlyMode,
        ShortcutBinding::FromKey(EKey::KEY_W),
        [] {}));
    registerCommand(makeCommand(
        "scene-view.fly-backward",
        "Fly Mode Backward",
        "Scene View/Fly Mode",
        ShortcutContexts::SceneViewFlyMode,
        ShortcutBinding::FromKey(EKey::KEY_S),
        [] {}));
    registerCommand(makeCommand(
        "scene-view.fly-left",
        "Fly Mode Left",
        "Scene View/Fly Mode",
        ShortcutContexts::SceneViewFlyMode,
        ShortcutBinding::FromKey(EKey::KEY_A),
        [] {}));
    registerCommand(makeCommand(
        "scene-view.fly-right",
        "Fly Mode Right",
        "Scene View/Fly Mode",
        ShortcutContexts::SceneViewFlyMode,
        ShortcutBinding::FromKey(EKey::KEY_D),
        [] {}));
    registerCommand(makeCommand(
        "scene-view.fly-up",
        "Fly Mode Up",
        "Scene View/Fly Mode",
        ShortcutContexts::SceneViewFlyMode,
        ShortcutBinding::FromKey(EKey::KEY_E),
        [] {}));
    registerCommand(makeCommand(
        "scene-view.fly-down",
        "Fly Mode Down",
        "Scene View/Fly Mode",
        ShortcutContexts::SceneViewFlyMode,
        ShortcutBinding::FromKey(EKey::KEY_Q),
        [] {}));

    registerSceneToolCommand(
        "scene-view.view-tool",
        "View",
        EKey::KEY_Q,
        [](Panels::SceneView&) {});
    registerSceneToolCommand(
        "scene-view.move-tool",
        "Move",
        EKey::KEY_W,
        [](Panels::SceneView& p_sceneView) { p_sceneView.SetCurrentGizmoOperation(EGizmoOperation::TRANSLATE); });
    registerSceneToolCommand(
        "scene-view.rotate-tool",
        "Rotate",
        EKey::KEY_E,
        [](Panels::SceneView& p_sceneView) { p_sceneView.SetCurrentGizmoOperation(EGizmoOperation::ROTATE); });
    registerSceneToolCommand(
        "scene-view.scale-tool",
        "Scale",
        EKey::KEY_R,
        [](Panels::SceneView& p_sceneView) { p_sceneView.SetCurrentGizmoOperation(EGizmoOperation::SCALE); });
    registerSceneToolCommand(
        "scene-view.rect-tool",
        "Rect",
        EKey::KEY_T,
        [](Panels::SceneView& p_sceneView) { p_sceneView.SetCurrentGizmoOperation(EGizmoOperation::TRANSLATE); });
    registerSceneToolCommand(
        "scene-view.transform-tool",
        "Transform",
        EKey::KEY_Y,
        [](Panels::SceneView& p_sceneView) { p_sceneView.SetCurrentGizmoOperation(EGizmoOperation::TRANSLATE); });
    registerSceneToolCommand(
        "scene-view.toggle-pivot-position",
        "Toggle Pivot Position",
        EKey::KEY_Z,
        [](Panels::SceneView& p_sceneView) { p_sceneView.ToggleCurrentGizmoPivot(); });
    registerSceneToolCommand(
        "scene-view.toggle-pivot-orientation",
        "Toggle Pivot Orientation",
        EKey::KEY_X,
        [](Panels::SceneView& p_sceneView) { p_sceneView.ToggleCurrentGizmoSpace(); });

    registerCommand(makeCommand(
        "edit.delete-selected-gameobject",
        "Delete Selected GameObject",
        "Edit",
        ShortcutContexts::SceneView,
        ShortcutBinding::FromKey(EKey::KEY_DELETE),
        [this]
        {
            if (m_editorActions.IsAnyGameObjectSelected())
                m_editorActions.DestroyGameObject(*m_editorActions.GetSelectedGameObject());
        }));

    registerCommand(makeCommand(
        "edit.delete-selected-gameobject-hierarchy",
        "Delete Selected GameObject",
        "Edit",
        ShortcutContexts::Hierarchy,
        ShortcutBinding::FromKey(EKey::KEY_DELETE),
        [this]
        {
            if (m_editorActions.IsAnyGameObjectSelected())
                m_editorActions.DestroyGameObject(*m_editorActions.GetSelectedGameObject());
        }));

    m_shortcutService.LoadProfile(std::filesystem::path(m_context.projectPath) / "UserSettings" / "shortcuts.json");
}

void Editor::Core::Editor::RestoreStartupScene()
{
    const auto restoreStartupSceneBegin = std::chrono::steady_clock::now();
    const auto restoreLoadedScenePrefabs = [this]()
    {
        const auto prefabRestoreBegin = std::chrono::steady_clock::now();
        NLS_LOG_INFO("[Startup] RestoreStartupScene prefab restore begin");
        m_context.PresentStartupProgressFrame("Restoring startup scene prefab instances", 0.87f);
        const bool prefabRestoreSucceeded = m_editorActions.RestorePrefabInstancesForCurrentSceneFromDisk();
        NLS_LOG_INFO(
            "[Startup] RestoreStartupScene prefab restore end elapsedMs=" +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - prefabRestoreBegin).count()));
        m_context.PresentStartupProgressFrame(
            prefabRestoreSucceeded ? "Startup scene loaded" : "Startup scene loaded with prefab restore warnings",
            0.89f);
    };
    const auto loadSceneWithStartupTelemetry = [this](const std::filesystem::path& resolvedPath) -> bool
    {
        const auto loadSceneBegin = std::chrono::steady_clock::now();
        NLS_LOG_INFO("[Startup] RestoreStartupScene LoadScene begin: " + resolvedPath.string());

        NLS::Base::Profiling::PerformanceStageStats sceneLoadStageStats;
        bool sceneLoaded = false;
        {
            NLS::Base::Profiling::PerformanceStageStatsCapture capture(sceneLoadStageStats);
            sceneLoaded = m_context.sceneManager.LoadScene(
                resolvedPath.string(),
                true,
                [this](const Engine::SceneSystem::SceneLoadProgress& progress)
                {
                    const float startupProgress = 0.65f + progress.normalizedProgress * 0.22f;
                    m_context.PresentStartupProgressFrame(progress.message, startupProgress);
                });
        }

        for (const auto& stage : sceneLoadStageStats.TopBottlenecks(
            NLS::Base::Profiling::PerformanceStageDomain::Unknown,
            12u))
        {
            if (stage.stageName.rfind("SceneLoad.", 0u) != 0u)
                continue;

            NLS_LOG_INFO(
                "[Startup] RestoreStartupScene scene load stage " +
                stage.stageName +
                " totalMs=" +
                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(stage.totalDuration).count()) +
                " calls=" +
                std::to_string(stage.callCount));
        }

        if (!sceneLoaded)
            return false;

        NLS_LOG_INFO(
            "[Startup] RestoreStartupScene LoadScene end elapsedMs=" +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - loadSceneBegin).count()));
        return true;
    };

    const auto loadRememberedScene =
        [this, &loadSceneWithStartupTelemetry, &restoreLoadedScenePrefabs, &restoreStartupSceneBegin](const std::string& scenePath) -> bool
    {
        if (scenePath.empty() || scenePath == "NULL")
            return false;

        const std::filesystem::path configuredPath(scenePath);
        const auto resolvedPath = configuredPath.is_absolute()
            ? configuredPath
            : std::filesystem::path(m_context.projectAssetsPath) / configuredPath;

        if (!std::filesystem::exists(resolvedPath))
            return false;

        if (!loadSceneWithStartupTelemetry(resolvedPath))
            return false;

        restoreLoadedScenePrefabs();
        NLS_LOG_INFO(
            "[Startup] RestoreStartupScene total elapsedMs=" +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - restoreStartupSceneBegin).count()));
        return true;
    };

    const auto lastOpenedScene = m_context.projectSettings.GetOrDefault<std::string>("last_opened_scene", "");
    if (loadRememberedScene(lastOpenedScene))
        return;

    const auto startScene = m_context.projectSettings.Get<std::string>("start_scene");
    const auto startScenePath = m_context.projectAssetsPath + startScene;
    if (!startScene.empty() && startScene != "NULL" && std::filesystem::exists(startScenePath))
    {
        if (loadSceneWithStartupTelemetry(startScenePath))
        {
            restoreLoadedScenePrefabs();
            NLS_LOG_INFO(
                "[Startup] RestoreStartupScene total elapsedMs=" +
                std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - restoreStartupSceneBegin).count()));
            return;
        }
    }

    m_context.PresentStartupProgressFrame("Creating default startup scene", 0.78f);
    m_context.sceneManager.LoadEmptyLightedScene();
    m_context.sceneManager.MarkCurrentSceneDirty();
}

void Editor::Core::Editor::RefreshProjectAssetBrowser()
{
    m_panelsManager.GetPanelAs<Panels::AssetBrowser>("Asset Browser").Refresh();
}

void Editor::Core::Editor::PrepareProjectAssetWatchersForStartup()
{
    m_panelsManager.GetPanelAs<Panels::AssetBrowser>("Asset Browser").PrepareStartupWatchers();
}

void Editor::Core::Editor::AdoptStartupAssetWatchers(
    AssetFileWatcher engineAssetsWatcher,
    AssetFileWatcher projectAssetsWatcher)
{
    m_panelsManager.GetPanelAs<Panels::AssetBrowser>("Asset Browser").AdoptStartupWatchers(
        std::move(engineAssetsWatcher),
        std::move(projectAssetsWatcher));
}

NLS::Editor::Assets::StartupWatcherPreimportResult Editor::Core::Editor::RunStartupWatcherPreimport(
    const NLS::Editor::Assets::StartupAssetPreimportProgressSink& progressSink)
{
    return m_panelsManager.GetPanelAs<Panels::AssetBrowser>("Asset Browser").RunStartupWatcherPreimport(progressSink);
}

NLS::Editor::Assets::StartupWatcherPreimportResult Editor::Core::Editor::CompleteStartupWatcherPreimportGate(
    const NLS::Editor::Assets::StartupAssetPreimportProgressSink& progressSink)
{
    return m_panelsManager.GetPanelAs<Panels::AssetBrowser>("Asset Browser").CompleteStartupWatcherPreimportGate(progressSink);
}

void Editor::Core::Editor::RememberLastOpenedScene(const std::string& p_scenePath)
{
    std::string storedScenePath = p_scenePath;
    if (!storedScenePath.empty())
    {
        std::error_code scenePathError;
        std::error_code assetsPathError;
        const auto absoluteScenePath = std::filesystem::absolute(storedScenePath, scenePathError);
        const auto absoluteAssetsPath = std::filesystem::absolute(m_context.projectAssetsPath, assetsPathError);
        if (!scenePathError && !assetsPathError)
        {
            const auto relativeScenePath = absoluteScenePath.lexically_relative(absoluteAssetsPath);
            const auto relativeScenePathText = relativeScenePath.generic_string();
            if (!relativeScenePathText.empty() && relativeScenePathText != ".." && relativeScenePathText.rfind("../", 0) != 0)
                storedScenePath = relativeScenePathText;
        }
    }

    if (!m_context.projectSettings.IsKeyExisting("last_opened_scene"))
        m_context.projectSettings.Add<std::string>("last_opened_scene", storedScenePath);
    else
        m_context.projectSettings.Set<std::string>("last_opened_scene", storedScenePath);

    m_context.projectSettings.Rewrite();
}

void Editor::Core::Editor::ApplyStartupValidationDirectives()
{
    const auto& diagnostics = m_context.GetDiagnosticsSettings();
    auto& sceneView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::SceneView>("Scene View");
	    auto& gameView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::GameView>("Game View");
	    auto& frameInfo = m_panelsManager.GetPanelAs<NLS::Editor::Panels::FrameInfo>("Frame Info");
	    auto& profilerPanel = m_panelsManager.GetPanelAs<NLS::Editor::Panels::ProfilerPanel>("Profiler");
	    auto& assetBrowser = m_panelsManager.GetPanelAs<NLS::Editor::Panels::AssetBrowser>("Asset Browser");

    switch (ResolveValidationFocusTarget(diagnostics.editorValidationExclusiveView))
    {
    case ValidationFocusTarget::SceneView:
        sceneView.Open();
        gameView.Close();
        NLS_LOG_INFO("Editor validation isolated Scene View.");
        break;
    case ValidationFocusTarget::GameView:
        gameView.Open();
        sceneView.Close();
        NLS_LOG_INFO("Editor validation isolated Game View.");
        break;
    case ValidationFocusTarget::None:
    default:
        break;
    }

    switch (ResolveValidationFocusTarget(diagnostics.editorValidationFocusView))
    {
    case ValidationFocusTarget::SceneView:
        sceneView.Focus();
        NLS_LOG_INFO("Editor validation pre-focused Scene View.");
        break;
    case ValidationFocusTarget::GameView:
        gameView.Focus();
        NLS_LOG_INFO("Editor validation pre-focused Game View.");
        break;
    case ValidationFocusTarget::None:
    default:
        break;
    }

    if (diagnostics.editorValidationOpenFrameInfo)
    {
        frameInfo.Open();
        NLS_LOG_INFO("Editor validation opened Frame Info.");
    }

    if (diagnostics.editorValidationOpenProfiler)
    {
        profilerPanel.Open();
        NLS_LOG_INFO("Editor validation opened Profiler.");
    }

	    if (diagnostics.editorValidationDisableHZBOcclusion)
	        NLS_LOG_INFO("Editor validation requested HZB occlusion disable override.");

	    if (!diagnostics.editorValidationAssetBrowserFolder.empty())
	    {
	        assetBrowser.SelectProjectFolderForValidation(diagnostics.editorValidationAssetBrowserFolder);
	        NLS_LOG_INFO("Editor validation selected Asset Browser folder: " + diagnostics.editorValidationAssetBrowserFolder);
	    }

	    if (diagnostics.editorValidationOcclusionStackCount != 0u)
    {
        if (auto* camera = sceneView.GetCamera(); camera != nullptr)
            CreateValidationOcclusionStack(m_editorActions, *camera, diagnostics.editorValidationOcclusionStackCount);
    }

    WritePrefabDragProxyValidationSummaryIfRequested(m_context, &sceneView);

    if (!diagnostics.editorValidationCreateAsset.empty())
    {
        const auto assetPath = diagnostics.editorValidationCreateAsset;
        m_editorActions.DelayAction(
            [this, assetPath]
            {
                if (auto* created = m_editorActions.CreateGameObjectFromAsset(assetPath, true);
                    created != nullptr)
                {
                    NLS_LOG_INFO(
                        "Editor validation created asset instance: " +
                        assetPath +
                        " root=" +
                        created->GetName());
                }
                else
                {
                    NLS_LOG_WARNING(
                        "Editor validation failed to create asset instance: " +
                        assetPath);
                }
            },
            1);
        NLS_LOG_INFO("Editor validation queued asset instance creation: " + assetPath);
    }

	    if (!diagnostics.editorValidationSelectGameObject.empty())
	    {
	        const auto gameObjectName = diagnostics.editorValidationSelectGameObject;
	        m_editorActions.DelayAction(
	            [this, gameObjectName]
	            {
	                auto* currentScene = m_context.sceneManager.GetCurrentScene();
	                auto* actor = currentScene != nullptr
	                    ? currentScene->FindGameObjectByName(gameObjectName)
	                    : nullptr;
	                if (actor != nullptr)
	                {
	                    m_editorActions.SelectGameObject(*actor);
	                    NLS_LOG_INFO("Editor validation pre-selected GameObject: " + gameObjectName);
	                }
	                else
	                {
	                    NLS_LOG_WARNING(
	                        "Editor validation could not find GameObject after startup: " + gameObjectName);
	                }
	            },
	            1);
	        NLS_LOG_INFO("Editor validation queued GameObject selection: " + gameObjectName);
	    }

    if (!diagnostics.editorValidationSceneCamera.empty())
    {
        const auto cameraTransform = ParseValidationSceneCamera(diagnostics.editorValidationSceneCamera);
        if (!cameraTransform.has_value())
        {
            NLS_LOG_WARNING(
                "Editor validation scene camera directive has invalid format: " +
                diagnostics.editorValidationSceneCamera);
            return;
        }

        auto* camera = sceneView.GetCamera();
        if (camera == nullptr)
            return;

        camera->SetPosition(cameraTransform->position);
        camera->SetRotation(Maths::Quaternion(cameraTransform->rotationEulerDegrees));
        camera->CacheViewMatrix();
        sceneView.GetCameraController().ResetMouseInteractionState();
        NLS_LOG_INFO("Editor validation applied Scene View camera: " + diagnostics.editorValidationSceneCamera);
    }
}

void Editor::Core::Editor::UpdateCurrentEditorMode(float p_deltaTime)
{
    {
        NLS_PROFILE_NAMED_SCOPE("Editor::UpdateModeState");
        if (auto editorMode = m_editorActions.GetCurrentEditorMode(); editorMode == EditorActions::EEditorMode::PLAY || editorMode == EditorActions::EEditorMode::FRAME_BY_FRAME)
            UpdatePlayMode(p_deltaTime);
        else
            UpdateEditMode(p_deltaTime);
    }

    {
        NLS_PROFILE_NAMED_SCOPE("SceneManager::CollectGarbagesAndUpdate");
        m_context.sceneManager.GetCurrentScene()->CollectGarbages();
        m_context.sceneManager.Update();
    }
}

void Editor::Core::Editor::UpdatePlayMode(float p_deltaTime)
{
    NLS_PROFILE_SCOPE();
    auto currentScene = m_context.sceneManager.GetCurrentScene();

    {
        NLS_PROFILE_NAMED_SCOPE("Scene::Update");
        currentScene->Update(p_deltaTime);
    }

    {
        NLS_PROFILE_NAMED_SCOPE("Scene::LateUpdate");
        currentScene->LateUpdate(p_deltaTime);
    }


    if (m_editorActions.GetCurrentEditorMode() == EditorActions::EEditorMode::FRAME_BY_FRAME)
        m_editorActions.PauseGame();
}

void Editor::Core::Editor::UpdateEditMode(float p_deltaTime)
{
    NLS_PROFILE_SCOPE();
    (void)p_deltaTime;
}

void Editor::Core::Editor::UpdateEditorPanels(float p_deltaTime)
{
    NLS_PROFILE_SCOPE();
    auto& topBar = m_panelsManager.GetPanelAs<NLS::Editor::Panels::EditorTopBar>("Editor Top Bar");
    auto& frameInfo = m_panelsManager.GetPanelAs<NLS::Editor::Panels::FrameInfo>("Frame Info");
    auto& sceneView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::SceneView>("Scene View");
    auto& gameView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::GameView>("Game View");
    auto& assetView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::AssetView>("Asset View");

    {
        NLS_PROFILE_NAMED_SCOPE("EditorTopBar::HandleShortcuts");
        topBar.HandleShortcuts(p_deltaTime);
    }

    const bool keepDefaultSceneFocus =
        ResolveValidationFocusTarget(m_context.GetDiagnosticsSettings().editorValidationFocusView) ==
        ValidationFocusTarget::None;
    constexpr uint64_t kStartupValidationFocusWarmupFrames = 8u;
    if (Panels::ShouldKeepStartupValidationFocusActive(
            m_context.GetDiagnosticsSettings().editorValidationFocusView,
            m_elapsedFrames,
            kStartupValidationFocusWarmupFrames))
    {
        switch (ResolveValidationFocusTarget(m_context.GetDiagnosticsSettings().editorValidationFocusView))
        {
        case ValidationFocusTarget::SceneView:
            sceneView.Focus();
            break;
        case ValidationFocusTarget::GameView:
            gameView.Focus();
            break;
        case ValidationFocusTarget::None:
        default:
            break;
        }
    }
    if (m_elapsedFrames == 1 && keepDefaultSceneFocus) // Let the first frame happen and then make the scene view the first seen view
        sceneView.Focus();

    if (frameInfo.IsOpened())
    {
        frameInfo.SetCandidateViews({
            &sceneView,
            &gameView,
            &assetView
        });
    }
}

void Editor::Core::Editor::UpdateViews(float p_deltaTime)
{
    NLS_PROFILE_SCOPE();
    auto& assetView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::AssetView>("Asset View");
    auto& sceneView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::SceneView>("Scene View");
    auto& gameView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::GameView>("Game View");

    {
        NLS_PROFILE_NAMED_SCOPE("AssetView::Update");
        assetView.Update(p_deltaTime);
    }
    {
        NLS_PROFILE_NAMED_SCOPE("GameView::Update");
        gameView.Update(p_deltaTime);
    }
    {
        NLS_PROFILE_NAMED_SCOPE("SceneView::Update");
        sceneView.Update(p_deltaTime);
    }
}

void Editor::Core::Editor::RenderEditorUI(float p_deltaTime)
{
    NLS_PROFILE_SCOPE();

    if (Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow)
        NLS_LOG_INFO("Editor::RenderEditorUI: begin");

    {
        NLS_PROFILE_NAMED_SCOPE("Editor::UIManagerRender");
        EDITOR_CONTEXT(uiManager)->Render();
    }
    if (Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow)
        NLS_LOG_INFO("Editor::RenderEditorUI: UIManager::Render returned");
}

void Editor::Core::Editor::PostUpdate()
{
    NLS_PROFILE_SCOPE();
    if (Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow)
        NLS_LOG_INFO("Editor::PostUpdate: begin");

    {
        NLS_PROFILE_NAMED_SCOPE("DriverUIAccess::PresentSwapchain");
        Render::Context::DriverUIAccess::PresentSwapchain(*m_context.driver);
    }
    if (Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow)
        NLS_LOG_INFO("Editor::PostUpdate: PresentSwapchain returned");

    {
        NLS_PROFILE_NAMED_SCOPE("InputManager::ClearEvents");
        m_context.inputManager->ClearEvents();
    }
    ++m_elapsedFrames;
    if (Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow)
        NLS_LOG_INFO("Editor::PostUpdate: end");
}

float Editor::Core::Editor::GetCurrentFrameRate() const
{
    return m_currentFrameRate;
}

float Editor::Core::Editor::GetCurrentDeltaTime() const
{
    return m_currentDeltaTime;
}

void Editor::Core::Editor::OpenConsole()
{
    auto& console = m_panelsManager.GetPanelAs<Panels::Console>("Console");
    console.Open();
    console.Focus();
    console.ScrollToBottom();
}
} // namespace NLS

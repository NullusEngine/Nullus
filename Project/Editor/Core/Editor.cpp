
#include <filesystem>
#include "Core/Modules/ModuleServices.h"
#include "Plugins/PluginManagerPanel.h"
#include "Panels/ECSBakingPanel.h"
#include "ECS/BakingAssetImport.h"
#include <ImGui/imgui_internal.h>
#include "Core/EditorGuiInput.h"
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
#include <Rendering/Context/DriverAccess.h>
#include <Rendering/Context/RenderingThreadSystem.h>
#include <UI/UIManager.h>
#include <Assets/ArtifactLoadTelemetry.h>
#include "Scripting/EditorScriptHost.h"
#include <Jobs/BackgroundJobQueue.h>
#include <Jobs/JobSystem.h>
#include <TaskGraph/TaskGraphSystem.h>
#include <Profiling/PerformanceStageStats.h>
#include <Profiling/Profiler.h>
#include <Profiling/TracyProfiler.h>
#include <Reflection/ReflectionDiagnostics.h>
#include <Rendering/Debug/DebugDrawService.h>
#include <Scripting/ScriptEngineApi.h>
#include <ServiceLocator.h>
#include <Threading/ExecutionThreadBudget.h>
#include <Utils/SystemCalls.h>
#include <imgui.h>

#include "Core/Editor.h"
#include "Core/EditorInputFrame.h"
#include "Scripting/ManagedScriptDebugSession.h"
#include "Debug/EditorDebugEndpoint.h"
#include "Debug/ExternalCodeEditor.h"
#include "Core/EditorCameraPerformanceBenchmark.h"
#include "Core/EditorJobSystemPolicy.h"
#include "Core/ThumbnailTelemetrySummary.h"
#include "Settings/EditorSettings.h"
#include "Settings/EditorSettingsPersistence.h"
#include "Settings/EditorSettingsRegistry.h"
#include "UI/Settings/PanelWindowSettings.h"
#include "Assembly.h"
#include "Core/AssemblyCore.h"
#include "AssemblyMath.h"
#include "AssemblyEngine.h"
#include "AssemblyPlatform.h"
#include "AssemblyRender.h"

#include "Panels/EditorTopBar.h"
#include "Panels/EditorStatusBar.h"
#include "Panels/BackgroundTasks.h"
#include "Panels/AssetBrowser.h"
#include "Panels/FrameInfo.h"
#include "Panels/IconBrowser.h"
#include "Panels/Console.h"
#include "Panels/ConsoleSourceLocation.h"
#include "Panels/Inspector.h"
#include "Panels/Hierarchy.h"
#include "Panels/HierarchyUndo.h"
#include "Panels/SceneView.h"
#include "Panels/GameView.h"
#include "Panels/AssetView.h"
#include "Panels/ViewFrameLifecycle.h"
#include "Panels/UIDocumentDesigner.h"
#include "Panels/InputAssetEditor.h"
#include "Panels/VfxEditor.h"
#include "Panels/VfxLibraryEditor.h"
#include "Panels/MaterialEditor.h"
#include "Panels/AnimationWorkbench.h"
#include "Panels/SpriteEditor.h"
#include "Animation/AnimationPreview.h"
#include "Panels/ProjectSettings.h"
#include "Panels/BuildProfilesPanel.h"
#include "Panels/ProfilerPanel.h"
#include "Engine/PrimitiveType.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Shortcuts/EditorShortcutBinding.h"
#include "Shortcuts/EditorShortcutContext.h"
#include <Windowing/Inputs/EKey.h>
#include <Windowing/Inputs/EMouseButton.h>
#include <Windowing/Inputs/EMouseButtonState.h>
using namespace NLS::Core::ResourceManagement;
using namespace NLS::Editor::Panels;
namespace NLS
{
namespace
{
// Automated editor UI smoke tests can run while the optional VFX WIP panels
// are being repaired. Normal editor launches keep the panels enabled.
bool SkipOptionalVfxPanels()
{
    const char* value = std::getenv("NLS_EDITOR_SKIP_VFX_PANELS");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

NLS::Base::Profiling::TracyProfiler g_tracyProfiler;
std::size_t g_publishedReflectionDiagnosticCount = 0;
constexpr uint32_t kEditorMainThreadContinuationDrainBudget = 64u;
constexpr float kEditorValidationCameraForwardStep = 0.1f;
constexpr auto kThumbnailTelemetrySummaryWriteInterval = std::chrono::seconds(2);

std::string ManagedPreparationStateName(
    NLS::Editor::Scripting::ManagedDebugPreparationState state)
{
    using State = NLS::Editor::Scripting::ManagedDebugPreparationState;
    switch (state)
    {
    case State::Building: return "Building";
    case State::LoadingRuntime: return "LoadingRuntime";
    case State::Ready: return "Ready";
    case State::Failed: return "Failed";
    default: return "Idle";
    }
}

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

std::string FormatTelemetryPercentileMs(
    std::vector<std::chrono::microseconds> values,
    const double percentile)
{
    if (values.empty())
        return "n/a";

    std::sort(values.begin(), values.end());
    const auto rank = static_cast<size_t>(std::ceil(percentile * static_cast<double>(values.size()))) - 1u;
    const auto index = (std::min)(rank, values.size() - 1u);
    return FormatTelemetryDurationMs(values[index]);
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
    const bool telemetryEnabled,
    const std::string_view runConfiguration,
    const NLS::Editor::Core::ResidentPrefabPreviewTelemetrySnapshot& resident)
{
    using NLS::Core::Assets::ArtifactLoadTelemetryStage;
    using NLS::Core::Assets::ArtifactLoadTelemetryStageName;

    struct StageAggregate
    {
        NLS::Core::Assets::ArtifactLoadTelemetryStage stage =
            NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewRender;
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
    if (!runConfiguration.empty())
        report << runConfiguration << '\n';
    report << "Resident prefab preview\n";
    report << "- residentEntryCount=" << resident.entryCount << '\n';
    report << "- residentActiveLeaseCount=" << resident.activeLeaseCount << '\n';
    report << "- residentBytes=" << resident.residentBytes << '\n';
    report << "- residentHitCount=" << resident.hitCount << '\n';
    report << "- residentMissCount=" << resident.missCount << '\n';
    report << "- residentStaleCount=" << resident.staleCount << '\n';
    report << "- residentZeroArtifactReadHitCount=" << resident.zeroArtifactReadHitCount << '\n';
    report << "- residentEvictionCount=" << resident.evictionCount << '\n';
    report << "- residentIdentityMissCount=" << resident.identityMissCount << '\n';
    report << "- residentFreshnessMismatchCount=" << resident.freshnessMismatchCount << '\n';
    report << "- residentThumbnailRequestCount=" << resident.thumbnailRequestCount << '\n';
    report << "- residentThumbnailRequestOtherIdentityCount="
        << resident.thumbnailRequestOtherIdentityCount << '\n';
    for (const auto& identityCount : resident.thumbnailRequestIdentityCounts)
    {
        report << "- residentThumbnailRequestIdentity=" << identityCount.identity
            << " count=" << identityCount.count << '\n';
    }
    report << "- residentLastRegisteredIdentity=" << resident.lastRegisteredIdentity << '\n';
    report << "- residentLastRegisteredLookupIdentity=" << resident.lastRegisteredLookupIdentity << '\n';
    report << "- residentLastRegisteredFreshness=" << resident.lastRegisteredFreshness << '\n';
    report << "- residentLastRequestedIdentity=" << resident.lastRequestedIdentity << '\n';
    report << "- residentLastRequestedFreshness=" << resident.lastRequestedFreshness << '\n';
    report << "- residentLastKnownIdentity=" << resident.lastKnownIdentity << '\n';
    report << "- residentLastKnownFreshness=" << resident.lastKnownFreshness << "\n\n";
    report << "- residentLastMismatchIdentity=" << resident.lastMismatchIdentity << '\n';
    report << "- residentLastMismatchRequestedFreshness="
        << resident.lastMismatchRequestedFreshness << '\n';
    report << "- residentLastMismatchKnownFreshness="
        << resident.lastMismatchKnownFreshness << "\n\n";
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
    report << "- requestBuildFailurePaths=" << thumbnailDrawOutcomes.requestBuildFailurePathTotals.size() << '\n';
    for (const auto& pathTotal : thumbnailDrawOutcomes.requestBuildFailurePathTotals)
        report << "- request-build-failure " << pathTotal.path << " records=" << pathTotal.count << '\n';
    report << '\n';

    report << "Canonical thumbnail timing\n";
    report << "- initialVisibleThumbnailCount=" << thumbnailDrawOutcomes.initialVisibleThumbnailCount << '\n';
    report << "- initialVisibleCanonicalEligibleCount="
        << thumbnailDrawOutcomes.initialVisibleCanonicalEligibleCount << '\n';
    report << "- canonicalVisibleThumbnailCount=" << thumbnailDrawOutcomes.canonicalVisibleThumbnailCount << '\n';
    report << "- initialVisibleLoadingCount=" << thumbnailDrawOutcomes.initialVisibleLoadingCount << '\n';
    report << "- initialVisibleReadyCount=" << thumbnailDrawOutcomes.initialVisibleReadyCount << '\n';
    report << "- initialVisibleFailedCount=" << thumbnailDrawOutcomes.initialVisibleFailedCount << '\n';
    report << "- initialVisibleFallbackCount=" << thumbnailDrawOutcomes.initialVisibleFallbackCount << '\n';
    report << "- initialVisiblePendingAfter30SecondsCount="
        << thumbnailDrawOutcomes.initialVisiblePendingAfter30SecondsCount << '\n';
    report << "- initialVisibleAllTerminal="
        << (thumbnailDrawOutcomes.initialVisibleAllTerminal ? "true" : "false") << '\n';
    report << "- initialVisibleTimedOut="
        << (thumbnailDrawOutcomes.initialVisibleTimedOut ? "true" : "false") << '\n';
    report << "- initialVisibleSetFingerprint="
        << thumbnailDrawOutcomes.initialVisibleSetFingerprint << '\n';
    report << "- firstCanonicalDrawMs=";
    if (thumbnailDrawOutcomes.firstCanonicalDrawMs.has_value())
        report << std::fixed << std::setprecision(3) << *thumbnailDrawOutcomes.firstCanonicalDrawMs;
    else
        report << "n/a";
    report << '\n';
    report << "- canonical90PercentFillMs=";
    if (thumbnailDrawOutcomes.canonical90PercentFillMs.has_value())
        report << std::fixed << std::setprecision(3) << *thumbnailDrawOutcomes.canonical90PercentFillMs;
    else
        report << "n/a";
    report << "\n\n";

    report << "Initial visible thumbnail presentation details\n";
    for (const auto& detail : thumbnailDrawOutcomes.initialVisiblePresentationDetails)
        report << "- " << detail << '\n';
    report << '\n';

    std::vector<std::chrono::microseconds> editorFrameDurations;
    std::vector<std::chrono::microseconds> thumbnailUiDurations;
    for (const auto& record : records)
    {
        if (record.stage == ArtifactLoadTelemetryStage::ThumbnailEditorFrame)
            editorFrameDurations.push_back(record.elapsed);
        else if (record.stage == ArtifactLoadTelemetryStage::ThumbnailUiDraw)
            thumbnailUiDurations.push_back(record.elapsed);
    }
    report << "Frame timing\n";
    report << "- editorFrame records=" << editorFrameDurations.size()
        << " p50Ms=" << FormatTelemetryPercentileMs(editorFrameDurations, 0.50)
        << " p95Ms=" << FormatTelemetryPercentileMs(editorFrameDurations, 0.95)
        << " p99Ms=" << FormatTelemetryPercentileMs(editorFrameDurations, 0.99) << '\n';
    report << "- thumbnailUiDraw records=" << thumbnailUiDurations.size()
        << " p50Ms=" << FormatTelemetryPercentileMs(thumbnailUiDurations, 0.50)
        << " p95Ms=" << FormatTelemetryPercentileMs(thumbnailUiDurations, 0.95)
        << " p99Ms=" << FormatTelemetryPercentileMs(thumbnailUiDurations, 0.99) << "\n\n";

    report << "Thumbnail draw outcome paths\n";
    const size_t maxDrawOutcomePaths = std::min<size_t>(thumbnailDrawOutcomePathTotals.size(), 32u);
    for (size_t index = 0u; index < maxDrawOutcomePaths; ++index)
    {
        const auto& total = thumbnailDrawOutcomePathTotals[index];
        report << "- " << total.path << " records=" << total.count << '\n';
    }
    report << '\n';

    std::vector<NLS::Core::Assets::ArtifactLoadTelemetryStageSummary> thumbnailRequestResultPaths;
    for (const auto& summary : summaries)
    {
        if (summary.stage == ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeRequestPreview &&
            summary.path.rfind("thumbnail-result|", 0u) == 0u)
        {
            thumbnailRequestResultPaths.push_back(summary);
        }
    }
    std::sort(
        thumbnailRequestResultPaths.begin(),
        thumbnailRequestResultPaths.end(),
        [](const auto& left, const auto& right)
        {
            if (left.recordCount != right.recordCount)
                return left.recordCount > right.recordCount;
            return left.path < right.path;
        });
    report << "Thumbnail request result paths\n";
    const size_t maxThumbnailRequestResultPaths =
        std::min<size_t>(thumbnailRequestResultPaths.size(), 64u);
    for (size_t index = 0u; index < maxThumbnailRequestResultPaths; ++index)
    {
        const auto& summary = thumbnailRequestResultPaths[index];
        report << "- " << summary.path << " records=" << summary.recordCount << '\n';
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

std::string BuildThumbnailTelemetryRunConfiguration(const NLS::Editor::Core::Context& context)
{
    const auto config = context.GetAssetThumbnailFeatureConfig();
    const auto backend = context.driver != nullptr
        ? NLS::Render::Settings::ToString(context.driver->GetActiveGraphicsBackend())
        : "Unknown";
#if defined(NDEBUG)
    constexpr const char* buildConfiguration = "Release";
#else
    constexpr const char* buildConfiguration = "Debug";
#endif

    std::ostringstream output;
    output << "buildConfiguration=" << buildConfiguration << '\n';
    output << "backend=" << backend << '\n';
    output << "thumbnailFeature.resident=" << (config.residentPrefabPreview ? 1 : 0) << '\n';
    output << "thumbnailFeature.proxyPool=" << (config.previewProxyPool ? 1 : 0) << '\n';
    output << "thumbnailFeature.atlas=" << (config.atlas ? 1 : 0) << '\n';
    output << "thumbnailFeature.readbackRing=" << (config.readbackRing ? 1 : 0) << '\n';
    output << "thumbnailFeature.adaptiveBudget=" << (config.adaptiveBudget ? 1 : 0) << '\n';
    output << "thumbnailFeature.explicitLanes=" << (config.explicitLanes ? 1 : 0) << '\n';
    output << "thumbnailCacheMode=" << (config.cacheRoot.empty() ? "project-local" : "custom") << '\n';
    if (!config.cacheRoot.empty())
        output << "thumbnailCacheRoot=" << config.cacheRoot.generic_string() << '\n';
    return output.str();
}

std::string BuildThumbnailTelemetrySummaryReport(const NLS::Editor::Core::Context& context)
{
    const auto records = NLS::Core::Assets::SnapshotArtifactLoadTelemetry();
    const auto drawOutcomes = NLS::Editor::Panels::SnapshotAssetBrowserThumbnailDrawOutcomeTelemetry();
    NLS::Editor::Core::ResidentPrefabPreviewTelemetrySnapshot resident;
    if (context.residentPrefabPreviewRegistry != nullptr)
    {
        const auto stats = context.residentPrefabPreviewRegistry->GetStats();
        resident.entryCount = stats.entryCount;
        resident.activeLeaseCount = stats.activeLeaseCount;
        resident.residentBytes = stats.residentBytes;
        resident.hitCount = stats.thumbnailHitCount;
        resident.missCount = stats.thumbnailMissCount;
        resident.staleCount = stats.thumbnailStaleCount;
        resident.zeroArtifactReadHitCount = stats.thumbnailZeroArtifactReadHitCount;
        resident.evictionCount = stats.evictionCount;
        resident.identityMissCount = stats.thumbnailIdentityMissCount;
        resident.freshnessMismatchCount = stats.thumbnailFreshnessMismatchCount;
        resident.thumbnailRequestCount = stats.thumbnailRequestCount;
        resident.thumbnailRequestOtherIdentityCount = stats.thumbnailRequestOtherIdentityCount;
        resident.thumbnailRequestIdentityCounts.reserve(stats.thumbnailRequestIdentityCounts.size());
        for (const auto& identityCount : stats.thumbnailRequestIdentityCounts)
        {
            resident.thumbnailRequestIdentityCounts.push_back({
                identityCount.identity,
                identityCount.count
            });
        }
        resident.lastRegisteredIdentity = stats.lastRegisteredIdentity;
        resident.lastRegisteredLookupIdentity = stats.lastRegisteredLookupIdentity;
        resident.lastRegisteredFreshness = stats.lastRegisteredFreshness;
        resident.lastRequestedIdentity = stats.lastThumbnailRequestedIdentity;
        resident.lastRequestedFreshness = stats.lastThumbnailRequestedFreshness;
        resident.lastKnownIdentity = stats.lastThumbnailKnownIdentity;
        resident.lastKnownFreshness = stats.lastThumbnailKnownFreshness;
        resident.lastMismatchIdentity = stats.lastThumbnailMismatchIdentity;
        resident.lastMismatchRequestedFreshness = stats.lastThumbnailMismatchRequestedFreshness;
        resident.lastMismatchKnownFreshness = stats.lastThumbnailMismatchKnownFreshness;
    }
    return NLS::Editor::Core::BuildThumbnailTelemetrySummaryReport(
        records,
        drawOutcomes,
        NLS::Core::Assets::IsArtifactLoadTelemetryEnabled(),
        BuildThumbnailTelemetryRunConfiguration(context),
        resident);
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

    const auto temporaryPath = outputPath.string() + ".tmp";
    std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        NLS_LOG_ERROR("Failed to open thumbnail telemetry summary output: " + outputPath.generic_string());
        return;
    }

    output << BuildThumbnailTelemetrySummaryReport(context);
    output.close();
    error.clear();
    std::filesystem::remove(outputPath, error);
    if (error)
    {
        NLS_LOG_ERROR(
            "Failed to remove previous thumbnail telemetry summary output: " +
            outputPath.generic_string() + " (" + error.message() + ")");
        return;
    }
    error.clear();
    std::filesystem::rename(temporaryPath, outputPath, error);
    if (error)
    {
        NLS_LOG_ERROR(
            "Failed to atomically replace thumbnail telemetry summary output: " +
            outputPath.generic_string() + " (" + error.message() + ")");
        return;
    }
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
    const bool telemetryEnabled,
    const std::string_view runConfiguration,
    const ResidentPrefabPreviewTelemetrySnapshot& resident)
{
    return FormatThumbnailTelemetrySummaryReport(
        records,
        drawOutcomes,
        telemetryEnabled,
        runConfiguration,
        resident);
}
}

Editor::Core::EditorJobWorkerBudget Editor::Core::ResolveEditorJobWorkerBudget(
    const uint32_t hardwareConcurrency,
    const std::optional<uint32_t> backgroundWorkerOverride)
{
    // TaskGraph uses the foreground Jobs workers. Budget that compute pool once
    // alongside the separate pool for blocking background work.
    const auto poolBudget = NLS::Base::Threading::ResolveExecutionPoolBudget(
        hardwareConcurrency,
        backgroundWorkerOverride.value_or(0u));
    return {
        poolBudget.jobsForegroundWorkerCount,
        poolBudget.jobsBackgroundWorkerCount};
}

Editor::Core::Context::JobSystemLifetime::JobSystemLifetime()
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

Editor::Core::Context::JobSystemLifetime::~JobSystemLifetime()
{
    Shutdown();
}

void Editor::Core::Context::JobSystemLifetime::Shutdown()
{
    if (!ownsJobSystem)
        return;

    // Also covers partial Context construction after Driver created the graph.
    if (!NLS::Render::Context::RenderingThreadSystem::HasRenderingThreadOwners())
        NLS::Base::TaskGraph::ShutdownGlobalTaskGraph();
    ownsJobSystem = false;
    NLS::Base::Jobs::ShutdownJobSystem(NLS::Base::Jobs::JobSystemShutdownMode::DrainAcceptedWork);
}

Editor::Core::Editor::Editor(Context& p_context)
    : m_context(p_context), m_undoService(), m_bakingService(m_undoService), m_panelsManager(m_canvas),
    m_editorActions(m_context, m_panelsManager)
{
    NLS::Base::Profiling::Profiler::SetEnabled(false);
    NLS::Base::Profiling::Profiler::RegisterDestination(g_tracyProfiler);

    const auto undoBlobStore = m_undoService.ConfigureBlobStore(
        // ConfigureBlobStore owns the private per-Editor session segment. Keep
        // this project-local base short enough for Windows blob staging paths.
        std::filesystem::path(m_context.projectPath) / "Library" / "Editor" / "UndoBlobs");
    if (!undoBlobStore.Succeeded())
    {
        NLS_LOG_WARNING(
            "Editor Undo: large snapshot storage is unavailable; large edits will remain in memory. " +
            undoBlobStore.message);
    }

    NLS::Core::ServiceLocator::Provide<NLS::Editor::Core::Context>(m_context);
    NLS::Core::ServiceLocator::Provide<NLS::Editor::Core::Editor>(*this);
	NLS::Core::ServiceLocator::Provide<NLS::Editor::Undo::EditorUndoService>(m_undoService);
    NLS::Core::ServiceLocator::Provide<NLS::Editor::Shortcuts::EditorShortcutService>(m_shortcutService);
    if (m_context.uiManager)
    {
        m_context.uiManager->SetPostCanvasDrawCallback([this]
        {
            if (m_inputUpdatePending)
            {
                m_inputUpdatePending = false;
                m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View").RefreshCameraInputAfterUi();
                HandleGlobalShortcuts();
            }
        });
    }
    m_context.SetDebugControlCallbacks(
        [this] { m_editorActions.StartPlaying(); },
        [this] { m_editorActions.PauseGame(); },
        [this] { m_editorActions.StartPlaying(); },
        [this] { m_editorActions.NextFrame(); },
        [this] { m_editorActions.StopPlaying(); },
        [this]
        {
            switch (m_editorActions.GetCurrentEditorMode())
            {
            case EditorActions::EEditorMode::PLAY: return std::string("Playing");
            case EditorActions::EEditorMode::PAUSE: return std::string("Paused");
            case EditorActions::EEditorMode::FRAME_BY_FRAME: return std::string("FrameByFrame");
            default: return std::string("Edit");
            }
        });
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
    RegisterEditorCommands();
    RegisterPanelCommands();
    m_graphInput = std::make_unique<Shortcuts::EditorGraphInput>(m_shortcutService);
    m_shortcutService.InitializeInput(m_context.inputManager->GetInputSystem());
    std::vector<std::string> inputDiagnostics;
    if (!m_shortcutService.LoadDefaults(std::filesystem::path(m_context.editorAssetsPath) / "Input" / "EditorShortcuts.inputactions", inputDiagnostics))
        NLS_LOG_WARNING(inputDiagnostics.empty() ? "Editor input defaults could not be loaded." : inputDiagnostics.front());
    if (!m_shortcutService.LoadProfile(std::filesystem::path(m_context.projectPath) / "UserSettings" / "editor.inputprofile",
        std::filesystem::path(m_context.projectPath) / "UserSettings" / "shortcuts.json"))
        NLS_LOG_WARNING("Could not load editor input profile; retaining the current input configuration.");
    for (const auto& diagnostic : m_shortcutService.GetProfileDiagnostics()) NLS_LOG_WARNING(diagnostic);
    m_sceneSourcePathChangedListener = m_context.sceneManager.CurrentSceneSourcePathChangedEvent += [this](const std::string& p_scenePath)
    {
        RememberLastOpenedScene(p_scenePath);
    };
	m_sceneLoadListener = m_context.sceneManager.SceneLoadSucceededEvent += [this]
	{
		m_undoService.ClearAll(Undo::UndoClearReason::SceneLoaded);
	};
    m_editorModeChangedListener = m_editorActions.EditorModeChangedEvent += [this](const EditorActions::EEditorMode)
    {
        // EditorActions owns the transition freeze. The active Play domain
        // remains editable without exposing the suspended Edit timeline.
    };

    m_context.PresentStartupProgressFrame("Loading startup scene models and shaders", 0.65f);
    NLS::Core::Modules::AdvanceHostPhase(NLS::Core::Modules::LoadingPhase::PostEditor);
    if (NLS::Core::ServiceLocator::Contains<NLS::Plugins::PluginHost>() &&
        NLS::Core::ServiceLocator::Contains<NLS::Plugins::PluginManager>())
    {
        m_pluginExtensions = std::make_unique<NLS::Editor::PluginSupport::EditorExtensionAdapter>(m_panelsManager,
            NLS::Core::ServiceLocator::Get<NLS::Plugins::PluginManager>(), NLS::Core::ServiceLocator::Get<NLS::Plugins::PluginHost>());
        auto& managerPanel = static_cast<NLS::Editor::PluginSupport::PluginManagerPanel&>(m_panelsManager.AddPanel("Plugin Manager",
            std::make_unique<NLS::Editor::PluginSupport::PluginManagerPanel>(NLS::Core::ServiceLocator::Get<NLS::Plugins::PluginManager>(),NLS::Core::ServiceLocator::Get<NLS::Plugins::PluginHost>())));
        m_panelsManager.GetPanelAs<Panels::EditorTopBar>("Editor Top Bar").RegisterWindowPanel("Plugin Manager",managerPanel);
    }

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

bool Editor::Core::Editor::PrepareForClose()
{
    if(m_closePrepared)return true;
    m_editorActions.StopPlaying();m_editorActions.PumpPlayRequests();
    if(m_editorActions.GetPlayModeController().IsActive())
    { NLS_LOG_ERROR("Editor close deferred: " + m_editorActions.GetPlayModeController().GetError());return false; }
    if(!m_editorActions.PromptSaveCurrentSceneIfDirty())return false;
    m_closePrepared=true;return true;
}

Editor::Core::Editor::~Editor()
{
    if (m_context.managedScriptDebugSession)
    {
        m_context.managedScriptDebugSession->SetBuildBoundary({});
        m_context.managedScriptDebugSession->SetBuildCallbacks({}, {});
    }
    m_editorActions.StopPlaying();
    m_editorActions.PumpPlayRequests();
    if (m_context.uiManager)
        m_context.uiManager->SetPostCanvasDrawCallback({});

	if (m_sceneLoadListener != 0u)
		m_context.sceneManager.SceneLoadSucceededEvent -= m_sceneLoadListener;
	if (m_editorModeChangedListener != 0u)
		m_editorActions.EditorModeChangedEvent -= m_editorModeChangedListener;
	m_undoService.ClearAll(Undo::UndoClearReason::EditorShutdown);

    m_shortcutService.SaveProfile({});
    if (m_uiOverlaySupplementInstalled &&
        NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
    {
        // The presenter outlives the overlay snapshot only within this object, so
        // drop the hook before teardown to avoid a dangling capture.
        NLS::Core::ServiceLocator::Get<NLS::UI::UIManager>().SetOverlaySnapshotSupplement({});
        m_uiOverlaySupplementInstalled = false;
    }
    if(!m_closePrepared)m_editorActions.PromptSaveCurrentSceneIfDirty();
    WriteThumbnailTelemetrySummaryIfRequested(m_context);
    if (m_panelsManager.HasPanel("Plugin Manager")) m_panelsManager.GetPanelAs<NLS::Editor::PluginSupport::PluginManagerPanel>("Plugin Manager").CancelPending();
    m_editorActions.StopBackgroundTasks();
    m_panelsManager.GetPanelAs<Panels::AssetBrowser>("Asset Browser").StopBackgroundWork();
    m_context.DrainJobSystem();
    NLS::Base::Profiling::Profiler::UnregisterDestination(
        m_panelsManager.GetPanelAs<Panels::ProfilerPanel>("Profiler").GetTimelineSink());
    m_editorActions.PlayEvent -= m_consolePlayListener;
    m_consolePlayListener = 0;
    m_editorActions.BuildEvent -= m_consoleBuildListener;
    m_consoleBuildListener = 0;
    m_pluginExtensions.reset();
    m_panelsManager.DestroyPanels();
    m_graphInput.reset();
	m_context.sceneManager.CurrentSceneSourcePathChangedEvent -= m_sceneSourcePathChangedListener;
	NLS::Core::ServiceLocator::Remove<NLS::Editor::Undo::EditorUndoService>();
	m_context.sceneManager.UnloadCurrentScene();
	NLS::Core::ServiceLocator::Remove<NLS::Editor::Shortcuts::EditorShortcutService>();
	NLS::Core::ServiceLocator::Remove<NLS::Editor::Core::Editor>();
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
    m_panelsManager.CreatePanel<Panels::AssetBrowser>(
        "Asset Browser",
        true,
        settings,
        m_context.engineAssetsPath,
        m_context.projectAssetsPath,
        m_context.editorAssetsPath,
        m_context.GetAssetThumbnailFeatureConfig());
    logSetupStep("AssetBrowser");
    m_panelsManager.CreatePanel<Panels::ECSBakingPanel>("ECS Baking", false, settings,
        m_bakingService, std::filesystem::path(m_context.projectAssetsPath),
        [this](const std::filesystem::path& path)
        {
            NLS::Editor::Assets::AssetDatabaseFacade database(std::vector<std::filesystem::path>{
                m_context.projectPath, m_context.engineAssetsPath});
            std::string error;
            if (!ECSAuthoring::ImportBakedSceneAsset(database, path, error))
                throw std::runtime_error(error);
            m_panelsManager.GetPanelAs<Panels::AssetBrowser>("Asset Browser").Refresh();
            m_context.RefreshRuntimeAssetDatabaseFromArtifactDB();
            return true;
        });
    m_panelsManager.CreatePanel<Panels::ProfilerPanel>("Profiler", false, settings);
    logSetupStep("Profiler");
    auto& profilerPanel = m_panelsManager.GetPanelAs<Panels::ProfilerPanel>("Profiler");
    NLS::Base::Profiling::Profiler::RegisterDestination(profilerPanel.GetTimelineSink());
    logSetupStep("RegisterProfiler");
    m_panelsManager.CreatePanel<Panels::Console>("Console", false, settings);
    m_panelsManager.CreatePanel<Panels::BackgroundTasks>("Background Tasks", false, settings, m_context.taskProgress);
    auto& statusConsole = m_panelsManager.GetPanelAs<Panels::Console>("Console");
    SyncConsoleSettings();
    {
        statusConsole.SetOptionsChanged([this](const Panels::ConsoleOptions& value)
        {
            auto& target = NLS::Editor::Settings::EditorSettings::GetConsoleSettingsObject();
            target.clearOnPlay = value.clearOnPlay;
            target.clearOnBuild = value.clearOnBuild;
            target.clearOnRecompile = value.clearOnRecompile;
            target.collapse = value.collapse;
            target.errorPause = value.errorPause;
            target.showTimestamp = value.showTimestamp;
            target.messages = value.messages;
            target.warnings = value.warnings;
            target.errors = value.errors;
            target.csharp = value.csharp;
            target.lua = value.lua;
            target.build = value.build;
            target.lineCount = value.lineCount;
            target.detailRatio = value.detailRatio;
            target.messageStack = value.messageStack;
            target.warningStack = value.warningStack;
            target.errorStack = value.errorStack;
            NLS::Editor::Settings::EditorSettingsRegistry registry;
            NLS::Editor::Settings::EditorSettings::RegisterSettingObjects(registry);
            const auto path = std::filesystem::path(m_context.projectPath) / "UserSettings" / "editor-settings.json";
            if (!NLS::Editor::Settings::EditorSettingsPersistence::Save(path, registry))
                NLS_LOG_WARNING("Could not save Console preferences: " + path.string());
        });
    }
    statusConsole.SetOpenLogFile([]
    {
        NLS::Debug::Logger::Flush();
        NLS::Platform::SystemCalls::OpenFile(NLS::Debug::FileHandler::GetLogFilePath());
    });
    statusConsole.SetSourceAvailability([this](const std::string& source)
    {
        try
        {
            const auto path = Panels::ResolveConsoleSourcePath(
                std::filesystem::u8path(source), std::filesystem::path(m_context.projectPath));
            std::error_code error;
            return std::filesystem::is_regular_file(path, error);
        }
        catch (const std::exception&) { return false; }
    });
    statusConsole.SetScriptConsole(
        m_context.scriptDebugService ? &m_context.scriptDebugService->GetConsole() : nullptr,
        [this](const NLS::Scripting::ScriptError& error)
        {
            if (error.sourcePath.empty()) return;
            const auto configuredPath = m_context.projectSettings.GetOrDefault<std::string>("vscode_path", "");
            const std::filesystem::path vscodePath = configuredPath.empty() ? std::filesystem::path{} : std::filesystem::path(configuredPath);
            std::filesystem::path sourcePath;
            try { sourcePath = std::filesystem::u8path(error.sourcePath); }
            catch (const std::exception&) { sourcePath = std::filesystem::path(error.sourcePath); }
            sourcePath = Panels::ResolveConsoleSourcePath(sourcePath, std::filesystem::path(m_context.projectPath));
            std::error_code pathError;
            if (!std::filesystem::exists(sourcePath, pathError))
            {
                const std::filesystem::path nativePath(error.sourcePath);
                pathError.clear();
                if (std::filesystem::exists(nativePath, pathError)) sourcePath = nativePath;
            }
            const auto result = NLS::Editor::Debug::ExternalCodeEditor::OpenAtLine(
                sourcePath, error.line, error.column,
                vscodePath.is_absolute() ? vscodePath : std::filesystem::path{});
            if (!result.success && !result.errorMessage.empty()) NLS_LOG_WARNING("Unable to open diagnostic: " + result.errorMessage);
        });
    if (m_context.managedScriptDebugSession)
    {
        m_context.managedScriptDebugSession->SetBuildCallbacks(
            [console = &statusConsole] { console->ClearOnRecompile(); },
            [console = &statusConsole](const NLS::Editor::Scripting::ManagedScriptBuildResult& result)
            {
                std::vector<Panels::ConsoleLogEntry> entries;
                for (const auto& diagnostic : NLS::Editor::Scripting::ManagedScriptBuildService::ReadDiagnostics(result))
                {
                    Panels::ConsoleLogEntry entry;
                    entry.received = std::chrono::steady_clock::now();
                    entry.source = Panels::ConsoleEntrySource::Build;
                    entry.level = diagnostic.severity == NLS::Editor::Scripting::ManagedBuildDiagnostic::Severity::Error
                        ? NLS::Debug::ELogLevel::LOG_ERROR
                        : diagnostic.severity == NLS::Editor::Scripting::ManagedBuildDiagnostic::Severity::Warning
                            ? NLS::Debug::ELogLevel::LOG_WARNING : NLS::Debug::ELogLevel::LOG_INFO;
                    entry.message = diagnostic.message;
                    entry.sourcePath = diagnostic.sourcePath;
                    entry.line = diagnostic.line;
                    entry.column = diagnostic.column;
                    entries.push_back(std::move(entry));
                }
                console->ReplaceCompilerDiagnostics(std::move(entries));
            });
    }
    m_consolePlayListener = m_editorActions.PlayEvent += [console = &statusConsole] { console->ClearOnPlay(); };
    m_consoleBuildListener = m_editorActions.BuildEvent += [console = &statusConsole] { console->ClearOnBuild(); };
    m_panelsManager.GetPanelAs<Panels::EditorStatusBar>("Editor Status Bar").SetSources(
        m_panelsManager.GetPanelAs<Panels::Console>("Console"), m_context.taskProgress,
        [this] { OpenConsole(); },
        [this]
        {
            auto& tasks = m_panelsManager.GetPanelAs<Panels::BackgroundTasks>("Background Tasks");
            tasks.Open(); tasks.Focus();
        },
        [this] { return GetCurrentFrameRate(); });
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
    m_panelsManager.CreatePanel<Panels::IconBrowser>("Icon Browser", false, settings);
    logSetupStep("IconBrowser");
    m_panelsManager.CreatePanel<Panels::UIDocumentDesigner>("UI Designer", false, settings);
    m_panelsManager.CreatePanel<Panels::InputAssetEditor>("Input Editor", false, settings);
    logSetupStep("UIDocumentDesigner");
    if (!SkipOptionalVfxPanels())
    {
        m_panelsManager.CreatePanel<Panels::VfxEditor>("Particle Editor", false, settings,
            std::filesystem::path(m_context.projectPath) / "Library" / "Editor" / "VFX");
        logSetupStep("ParticleEditor");
        m_panelsManager.CreatePanel<Panels::VfxLibraryEditor>("Particle Library Editor",false,settings);
        logSetupStep("ParticleLibraryEditor");
    }
    m_panelsManager.CreatePanel<Panels::MaterialEditor>("Material Editor",false,settings,
        std::filesystem::path(m_context.projectPath),std::filesystem::path(m_context.engineAssetsPath));
    logSetupStep("MaterialEditor");
    m_panelsManager.CreatePanel<AnimationTools::AnimationPreview>("Animation Preview",false,settings);
    auto& animationPreview=m_panelsManager.GetPanelAs<AnimationTools::AnimationPreview>("Animation Preview");
    m_panelsManager.CreatePanel<Panels::AnimationWorkbench>("Animation Workbench",false,settings,std::filesystem::path(m_context.projectPath),animationPreview);
    m_panelsManager.CreatePanel<Panels::SpriteEditor>("Sprite Editor",false,settings,std::filesystem::path(m_context.projectPath));
    m_panelsManager.CreatePanel<Panels::ProjectSettings>("Project Settings", false, settings);
    logSetupStep("ProjectSettings");
    m_panelsManager.CreatePanel<Panels::BuildProfilesPanel>("Build Profiles", false, settings);
    logSetupStep("BuildProfiles");
    m_panelsManager.GetPanelAs<Panels::ProjectSettings>("Project Settings").enabled = false;
    m_panelsManager.GetPanelAs<Panels::BuildProfilesPanel>("Build Profiles").enabled = false;
    auto& topBar = m_panelsManager.GetPanelAs<Panels::EditorTopBar>("Editor Top Bar");
    topBar.RegisterProjectSettingsPanel(m_panelsManager.GetPanelAs<Panels::ProjectSettings>("Project Settings"));
    topBar.RegisterWindowPanel("Asset Browser", m_panelsManager.GetPanelAs<Panels::AssetBrowser>("Asset Browser"));
    topBar.RegisterWindowPanel("Frame Info", m_panelsManager.GetPanelAs<Panels::FrameInfo>("Frame Info"));
    topBar.RegisterWindowPanel("Icon Browser", m_panelsManager.GetPanelAs<Panels::IconBrowser>("Icon Browser"));
    topBar.RegisterWindowPanel("Profiler", profilerPanel);
    topBar.RegisterWindowPanel("ECS Baking", m_panelsManager.GetPanelAs<Panels::ECSBakingPanel>("ECS Baking"));
    topBar.RegisterWindowPanel("Console", m_panelsManager.GetPanelAs<Panels::Console>("Console"));
    topBar.RegisterWindowPanel("Background Tasks", m_panelsManager.GetPanelAs<Panels::BackgroundTasks>("Background Tasks"));
    topBar.RegisterWindowPanel("Asset View", m_panelsManager.GetPanelAs<Panels::AssetView>("Asset View"));
    topBar.RegisterWindowPanel("Hierarchy", m_panelsManager.GetPanelAs<Panels::Hierarchy>("Hierarchy"));
    topBar.RegisterWindowPanel("Inspector", m_panelsManager.GetPanelAs<Panels::Inspector>("Inspector"));
    topBar.RegisterWindowPanel("Scene View", m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View"));
    topBar.RegisterWindowPanel("Game View", m_panelsManager.GetPanelAs<Panels::GameView>("Game View"));
    topBar.RegisterWindowPanel("UI Designer", m_panelsManager.GetPanelAs<Panels::UIDocumentDesigner>("UI Designer"));
    topBar.RegisterWindowPanel("Input Editor", m_panelsManager.GetPanelAs<Panels::InputAssetEditor>("Input Editor"));
    if (!SkipOptionalVfxPanels())
    {
        topBar.RegisterWindowPanel("Particle Editor", m_panelsManager.GetPanelAs<Panels::VfxEditor>("Particle Editor"));
        topBar.RegisterWindowPanel("Particle Library Editor",m_panelsManager.GetPanelAs<Panels::VfxLibraryEditor>("Particle Library Editor"));
    }
    topBar.RegisterWindowPanel("Material Editor",m_panelsManager.GetPanelAs<Panels::MaterialEditor>("Material Editor"));
    topBar.RegisterWindowPanel("Animation Workbench",m_panelsManager.GetPanelAs<Panels::AnimationWorkbench>("Animation Workbench"));
    topBar.RegisterWindowPanel("Sprite Editor",m_panelsManager.GetPanelAs<Panels::SpriteEditor>("Sprite Editor"));
    topBar.RegisterWindowPanel("Animation Preview",m_panelsManager.GetPanelAs<AnimationTools::AnimationPreview>("Animation Preview"));
    topBar.RegisterWindowPanel("Build Profiles", m_panelsManager.GetPanelAs<Panels::BuildProfilesPanel>("Build Profiles"));
    logSetupStep("RegisterWindows");
    // Scripted editor windows register their menu entries through the script
    // ABI; publish them once the top bar exists.
    // Scripted editor menu entries are an extension point, never a startup
    // dependency: a malformed descriptor must not abort editor construction.
    try
    {
        NLS::Editor::Scripting::InstallScriptMenuEntries();
    }
    catch (const std::exception& exception)
    {
        NLS_LOG_ERROR("Scripted editor menu registration threw: " + std::string(exception.what()));
    }
    catch (...)
    {
        NLS_LOG_ERROR("Scripted editor menu registration threw an unknown exception.");
    }
    logSetupStep("InstallScriptMenuEntries");
    m_canvas.MakeDockspace(true);
    logSetupStep("MakeDockspace");
    m_context.uiManager->SetCanvas(m_canvas);
    logSetupStep("SetCanvas");
    m_context.uiManager->ResetLayout(m_context.projectPath + "/UserSettings/layout.ini");
    logSetupStep("ResetLayout");
    m_panelsManager.GetPanelAs<Panels::GameView>("Game View").SetOpened(false);
    logSetupStep("CloseGameViewForStartup");
}

void Editor::Core::Editor::SyncConsoleSettings()
{
    if (!m_panelsManager.HasPanel("Console")) return;
    const auto& saved = NLS::Editor::Settings::EditorSettings::GetConsoleSettingsObject();
    Panels::ConsoleOptions options;
    options.clearOnPlay = saved.clearOnPlay;
    options.clearOnBuild = saved.clearOnBuild;
    options.clearOnRecompile = saved.clearOnRecompile;
    options.collapse = saved.collapse;
    options.errorPause = saved.errorPause;
    options.showTimestamp = saved.showTimestamp;
    options.messages = saved.messages;
    options.warnings = saved.warnings;
    options.errors = saved.errors;
    options.csharp = saved.csharp;
    options.lua = saved.lua;
    options.build = saved.build;
    options.lineCount = saved.lineCount;
    options.detailRatio = saved.detailRatio;
    options.messageStack = saved.messageStack;
    options.warningStack = saved.warningStack;
    options.errorStack = saved.errorStack;
    m_panelsManager.GetPanelAs<Panels::Console>("Console").SetOptions(options);
}

void Editor::Core::Editor::PreUpdate()
{
    m_inputUpdatePending = false;
    if (NLS::Core::ServiceLocator::Contains<NLS::Editor::Materials::MaterialEditingService>())
        NLS::Core::ServiceLocator::Get<NLS::Editor::Materials::MaterialEditingService>().Pump();
    (void)NLS::Render::Context::RenderingThreadSystem::PumpGameThreadTasks(64u);
    static_cast<void>(m_undoService.DrainPendingCompletions());
    RefreshProfilerRecordingState();
    NLS_PROFILE_SCOPE();
    {
        NLS_PROFILE_NAMED_SCOPE("Editor::PollEvents");
        m_context.device->PollEvents();
    }
    m_inputUpdatePending = true;
}

void Editor::Core::Editor::BeginProfilerFrame()
{
    m_validationCameraMotionPendingForFrame = true;
    RefreshProfilerRecordingState();
    NLS::Base::Profiling::TracyProfiler::MarkFrame();
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
    if (m_context.uiManager && m_context.uiManager->PrepareFrame()) m_shortcutService.PrepareInput(GetInputRoute());
    const auto editorFrameBegin = std::chrono::steady_clock::now();

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
        // Keep the engine font atlas resident as a UI texture and hook the
        // retained game UI into the overlay snapshot the editor publishes.
        if (auto* driver = NLS::Render::Context::TryGetLocatedDriver())
            m_uiOverlayPresenter.PumpFontAtlas(*driver);

        if (!m_uiOverlaySupplementInstalled &&
            NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
        {
            NLS::Core::ServiceLocator::Get<NLS::UI::UIManager>().SetOverlaySnapshotSupplement(
                [this](NLS::Render::UI::UiDrawDataSnapshot& snapshot)
                {
                    m_uiOverlayPresenter.AppendToOverlaySnapshot(snapshot);
                });
            m_uiOverlaySupplementInstalled = true;
        }
    }
    logUpdateStage("UiOverlayPresenter");

    {
        NLS_PROFILE_NAMED_SCOPE("Editor::PumpAssetFileWatchers");
        m_panelsManager.GetPanelAs<Panels::AssetBrowser>("Asset Browser").PumpFileWatchers();
    }
    if (m_context.editorDebugEndpoint)
    {
        NLS_PROFILE_NAMED_SCOPE("Editor::PollDebugEndpoint");
        m_context.editorDebugEndpoint->Poll();
    }
    if (m_context.managedScriptDebugSession)
    {
        NLS_PROFILE_NAMED_SCOPE("Editor::PollManagedScriptDebugBuild");
        m_context.managedScriptDebugSession->SetBuildBoundary([this]
        {
            if (!m_editorActions.GetPlayModeController().IsActive()) return true;
            m_editorActions.StopPlaying();
            return false;
        });
        m_context.managedScriptDebugSession->Poll();

        if (m_context.editorDebugEndpoint)
        {
            const auto& preparation = m_context.managedScriptDebugSession->GetPreparation();
            const auto state = ManagedPreparationStateName(preparation.state);
            if (state != m_context.debugLastManagedState ||
                preparation.assembly != m_context.debugLastManagedAssembly ||
                preparation.status.message != m_context.debugLastManagedMessage)
            {
                m_context.editorDebugEndpoint->PublishEvent({
                    {"type", "Build"},
                    {"state", state},
                    {"succeeded", preparation.state == NLS::Editor::Scripting::ManagedDebugPreparationState::Ready},
                    {"assembly", preparation.assembly.string()},
                    {"symbols", preparation.symbols.string()},
                    {"message", preparation.status.message}});
                m_context.debugLastManagedState = state;
                m_context.debugLastManagedAssembly = preparation.assembly;
                m_context.debugLastManagedMessage = preparation.status.message;
            }
        }
    }
    {
        NLS_PROFILE_NAMED_SCOPE("Editor::UpdateCurrentEditorMode");
        auto& console=m_panelsManager.GetPanelAs<Panels::Console>("Console");
        console.UpdateLogState();
        if (console.ConsumeErrorPauseRequest() && m_editorActions.GetPlayModeController().IsActive())
            m_editorActions.PauseGame();
        UpdateCurrentEditorMode(p_deltaTime);
    }
    if (m_context.editorDebugEndpoint && m_context.debugStateProvider)
    {
        const auto state = m_context.debugStateProvider();
        if (state != m_context.debugLastPlayState)
        {
            m_context.editorDebugEndpoint->PublishEvent({
                {"type", "PlayState"},
                {"state", state}});
            m_context.debugLastPlayState = state;
        }
    }
    if (m_context.editorDebugEndpoint && m_context.scriptDebugService)
    {
        const auto& diagnostics = m_context.scriptDebugService->GetConsole().GetErrors();
        // The console is bounded. If its front item was evicted, restart the
        // cursor and publish the currently retained diagnostics once.
        if (diagnostics.size() < m_context.debugLastDiagnosticCount)
            m_context.debugLastDiagnosticCount = 0u;
        while (m_context.debugLastDiagnosticCount < diagnostics.size())
        {
            const auto& error = diagnostics[m_context.debugLastDiagnosticCount++];
            m_context.editorDebugEndpoint->PublishEvent({
                {"type", "RuntimeDiagnostic"},
                {"language", NLS::Scripting::ToString(error.language)},
                {"asset", error.scriptAsset.ToString()},
                {"sourcePath", error.sourcePath},
                {"line", error.line},
                {"column", error.column},
                {"message", error.message},
                {"stackTrace", error.stackTrace}});
        }
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
        const EditorCameraPerformanceStageScope stageScope(EditorCameraPerformanceStage::EditorPanels);
        UpdateEditorPanels(p_deltaTime);
    }
    logUpdateStage("UpdateEditorPanels");
    {
        const EditorCameraPerformanceStageScope stageScope(EditorCameraPerformanceStage::RenderUi);
        RenderEditorUI(p_deltaTime);
    }
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

    if (m_context.IsEditorWindowShown() && NLS::Core::Assets::IsArtifactLoadTelemetryEnabled())
    {
        // Keep frame timing representative while leaving telemetry capacity for
        // the thumbnail stages that explain stalled GPU/resource requests.
        const auto telemetryFrameSerial = m_thumbnailTelemetryFrameSerial++;
        if ((telemetryFrameSerial % 8u) == 0u)
        {
            NLS::Core::Assets::RecordArtifactLoadTelemetry({
                NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailEditorFrame,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - editorFrameBegin),
                0u,
                "editor-frame"
            });
        }
    }
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
    if (tracyConnected && !m_tracyWasConnected)
        NLS::Base::Profiling::Profiler::ReplayGpuContextIfAvailable(g_tracyProfiler);
    m_tracyWasConnected = tracyConnected;
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
    if (!diagnostics.editorCameraPerformanceOutput.empty() &&
        !m_cameraPerformanceMeasurementStarted)
    {
        return;
    }
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

bool Editor::Core::Editor::IsCameraPerformanceWarmupReady() const
{
    if (!m_context.IsEditorWindowShown())
        return false;

    const auto& assetBrowser = m_panelsManager.GetPanelAs<Panels::AssetBrowser>("Asset Browser");
    if (!assetBrowser.IsThumbnailPreviewRenderWarmupComplete())
        return false;

    return WasCameraPerformanceViewThreadedFramePublished();
}

void Editor::Core::Editor::BeginCameraPerformanceMeasurement()
{
    if (m_cameraPerformanceMeasurementStarted)
        return;

    m_cameraPerformanceMeasurementStarted = true;
    NLS_LOG_INFO("Editor camera performance measurement warmup complete; starting fixed measurement window.");
}

uint32_t Editor::Core::Editor::GetValidationCameraForwardCompletedFrames() const
{
    return m_validationCameraForwardCompletedFrames;
}

bool Editor::Core::Editor::WasLastSceneViewThreadedFramePublished() const
{
    return m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View").WasLastRenderFramePublished();
}

bool Editor::Core::Editor::WasCameraPerformanceViewThreadedFramePublished() const
{
    if (ResolveValidationFocusTarget(m_context.GetDiagnosticsSettings().editorValidationFocusView) ==
        ValidationFocusTarget::GameView)
    {
        return m_panelsManager.GetPanelAs<Panels::GameView>("Game View").WasLastRenderFramePublished();
    }

    return WasLastSceneViewThreadedFramePublished();
}

std::pair<uint16_t, uint16_t> Editor::Core::Editor::GetSceneViewSafeSize() const
{
    return m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View").GetSafeSize();
}

std::pair<uint16_t, uint16_t> Editor::Core::Editor::GetCameraPerformanceViewSafeSize() const
{
    if (ResolveValidationFocusTarget(m_context.GetDiagnosticsSettings().editorValidationFocusView) ==
        ValidationFocusTarget::GameView)
    {
        return m_panelsManager.GetPanelAs<Panels::GameView>("Game View").GetSafeSize();
    }

    return GetSceneViewSafeSize();
}

Render::Data::FrameInfo Editor::Core::Editor::GetSceneViewCumulativeFrameInfo() const
{
    return m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View").GetRenderer().GetCumulativeFrameInfo();
}

Render::Data::FrameInfo Editor::Core::Editor::GetCameraPerformanceViewCumulativeFrameInfo() const
{
    if (ResolveValidationFocusTarget(m_context.GetDiagnosticsSettings().editorValidationFocusView) ==
        ValidationFocusTarget::GameView)
    {
        return m_panelsManager.GetPanelAs<Panels::GameView>("Game View")
            .GetRenderer()
            .GetCumulativeFrameInfo();
    }

    return GetSceneViewCumulativeFrameInfo();
}

void Editor::Core::Editor::HandleGlobalShortcuts()
{
    m_shortcutService.DispatchInput(GetInputRoute());
}

Editor::Shortcuts::EditorInputRoute Editor::Core::Editor::GetInputRoute()
{
    auto& gameView = m_panelsManager.GetPanelAs<Panels::GameView>("Game View");
    Shortcuts::EditorInputRoute route;
    const auto windows = m_context.inputManager->GetInputSystem().GetWindows();
    route.windowFocused = std::any_of(windows.begin(), windows.end(), [](const auto& window) { return window.registered && window.focused; });
    const auto& gui = *ImGui::GetCurrentContext();
    route.textEditing = IsEditorTextInputActive();
    route.widgetOwnsPointer = gui.HoveredId != 0 || (gui.ActiveId != 0 && gui.ActiveIdSource == ImGuiInputSource_Mouse);
    route.widgetKeyboardActivation = gui.NavActivateId != 0;
    route.modal = ImGui::GetTopMostPopupModal() != nullptr;
    route.menuActive = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    route.gameOwnsInput = gameView.OwnsGameInput();
    route.allowEditorInGame = Settings::EditorSettings::GetGameViewSettingsObject().editorShortcuts;
    route.workbenchOwnsInput = m_panelsManager.GetPanelAs<Panels::AnimationWorkbench>("Animation Workbench").OwnsShortcuts() ||
        ((!SkipOptionalVfxPanels() &&
            (m_panelsManager.GetPanelAs<Panels::VfxEditor>("Particle Editor").OwnsShortcuts() ||
             m_panelsManager.GetPanelAs<Panels::VfxLibraryEditor>("Particle Library Editor").OwnsShortcuts())) ||
        m_panelsManager.GetPanelAs<Panels::UIDocumentDesigner>("UI Designer").OwnsShortcuts());
    route.gestureOwnsEscape = m_undoService.HasActiveEditSession();
    if (m_panelsManager.GetPanelAs<Panels::UIDocumentDesigner>("UI Designer").HasCurrentKeyboardFocus()) route.ownedScope = "ui-designer";
    if (!SkipOptionalVfxPanels())
    {
        if (m_panelsManager.GetPanelAs<Panels::VfxEditor>("Particle Editor").OwnsShortcuts()) route.ownedScope = "vfx-editor";
        if (m_panelsManager.GetPanelAs<Panels::VfxLibraryEditor>("Particle Library Editor").HasCurrentKeyboardFocus()) route.ownedScope = "vfx-library";
    }
    if (m_panelsManager.GetPanelAs<Panels::AnimationWorkbench>("Animation Workbench").HasCurrentKeyboardFocus()) route.ownedScope = "animation-workbench";
    if (m_panelsManager.GetPanelAs<Panels::InputAssetEditor>("Input Editor").HasCurrentKeyboardFocus()) route.ownedScope = "input-editor";
    if (m_panelsManager.GetPanelAs<Panels::SpriteEditor>("Sprite Editor").OwnsShortcuts()) route.ownedScope = "sprite-editor";
    route.graphOwnsInput = m_graphInput && m_graphInput->HasFocus();
    route.cameraOwnsInput = Core::CameraController::GetKeyboardInputTarget() || Core::CameraController::NavigationOwnsInput();
    route.workbenchOwnsInput |= route.graphOwnsInput;
    route.workbenchOwnsInput = route.workbenchOwnsInput || !route.ownedScope.empty();
    return route;
}

void Editor::Core::Editor::RegisterShortcutContexts()
{
    using namespace NLS::Editor::Shortcuts;
    using namespace Windowing::Inputs;

    const auto isSceneViewFocused = [this]
    {
        const auto& scene = m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View");
        return scene.HasCurrentKeyboardFocus() && !scene.IsToolbarInteractionActive();
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
        [this]
        {
            auto& scene = m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View");
            return Core::CameraController::NavigationOwnsInput() || scene.GetCameraController().IsCameraControlActive() ||
                (m_shortcutService.GetInputState(EditorInputAction::CameraFly).pressed &&
                 m_shortcutService.GetInputState(EditorInputAction::CameraFly).actuated &&
                 scene.IsMouseHoveringView(m_context.inputManager->GetMousePosition()));
        }});

    m_shortcutService.RegisterContext({
        ShortcutContexts::Hierarchy,
        "Hierarchy",
        20,
        "focused-panel",
        [this]
        {
            return m_panelsManager.GetPanelAs<Panels::Hierarchy>("Hierarchy").HasCurrentKeyboardFocus();
        }});

    m_shortcutService.RegisterContext({
        ShortcutContexts::TextInput,
        "Text Input",
        100,
        "",
        []
        {
            return IsEditorTextInputActive();
        }});
    m_shortcutService.RegisterContext({"edit-gesture", "Active Edit Gesture", 150, "", [this]
    { return m_undoService.HasActiveEditSession() && !IsEditorTextInputActive(); }});
}

void Editor::Core::Editor::RegisterEditorCommands()
{
    using namespace NLS::Editor::Shortcuts;

    const auto registerCommand = [this](ShortcutCommand p_command)
    {
        m_shortcutService.RegisterCommand(std::move(p_command));
    };

    auto makeCommand = [](std::string p_id,
                          std::string p_displayName,
                          std::string p_category,
                          ShortcutContextId p_context,
                          std::function<void()> p_execute)
    {
        ShortcutCommand command;
        command.id = std::move(p_id);
        command.displayName = std::move(p_displayName);
        command.category = std::move(p_category);
        command.context = std::move(p_context);
        command.execute = std::move(p_execute);
        return command;
    };

    registerCommand(makeCommand(
        "file.new-scene",
        "New Scene",
        "File",
        ShortcutContexts::Global,
        [this] { m_editorActions.LoadEmptyScene(); }));

    registerCommand(makeCommand(
        "file.save-scene",
        "Save Scene",
        "File",
        ShortcutContexts::Global,
        [this] { m_editorActions.SaveSceneChanges(); }));

    registerCommand(makeCommand(
        "file.save-scene-as",
        "Save Scene As",
        "File",
        ShortcutContexts::Global,
        [this] { m_editorActions.SaveAs(); }));

	// These are global timeline operations. They deliberately do not use the
	// focused panel as a history domain: the top completed transaction always
	// wins, regardless of which editor surface started it.
	auto undoCommand = makeCommand(
		"edit.undo",
		"Undo",
		"Edit",
		ShortcutContexts::Global,
		[this] { static_cast<void>(m_undoService.RequestUndo()); });
	undoCommand.availability = [this] { return m_undoService.CanUndo(); };
	registerCommand(std::move(undoCommand));

	auto redoCommand = makeCommand(
		"edit.redo",
		"Redo",
		"Edit",
		ShortcutContexts::Global,
		[this] { static_cast<void>(m_undoService.RequestRedo()); });
	redoCommand.availability = [this] { return m_undoService.CanRedo(); };
	registerCommand(std::move(redoCommand));

    auto playCommand = makeCommand("editor.play", "Play / Stop", "Editor", ShortcutContexts::Global,
        [this] { m_editorActions.TogglePlaying(); });
    registerCommand(std::move(playCommand));
    auto stopCommand = makeCommand("editor.stop", "Stop", "Editor", ShortcutContexts::Global,
        [this] { m_editorActions.StopPlaying(); });
    stopCommand.availability = [this] { return m_editorActions.GetPlayModeController().IsActive(); };
    registerCommand(std::move(stopCommand));
    auto pauseCommand = makeCommand("editor.pause", "Pause / Resume", "Editor", ShortcutContexts::Global,
        [this]
        {
            if (m_editorActions.GetCurrentEditorMode() == EditorActions::EEditorMode::PAUSE) m_editorActions.StartPlaying();
            else m_editorActions.PauseGame();
        });
    pauseCommand.availability = [this] { return m_editorActions.GetPlayModeController().IsActive(); };
    registerCommand(std::move(pauseCommand));
    auto stepCommand = makeCommand("editor.step", "Step", "Editor", ShortcutContexts::Global,
        [this] { m_editorActions.NextFrame(); });
    stepCommand.availability = [this] { return m_editorActions.GetPlayModeController().IsActive(); };
    registerCommand(std::move(stepCommand));

    auto releaseGame = makeCommand("editor.release-game-input", "Release Game Input", "Editor", ShortcutContexts::Global,
        [this] { m_panelsManager.GetPanelAs<Panels::GameView>("Game View").ReleaseGameInput(); });
    releaseGame.availability = [this] { return m_panelsManager.GetPanelAs<Panels::GameView>("Game View").OwnsGameInput(); };
    registerCommand(std::move(releaseGame));
    auto cancelEdit = makeCommand("edit.cancel-active-edit", "Cancel Active Edit", "Edit", "edit-gesture",
        [this] { static_cast<void>(m_undoService.RequestUndo()); });
    cancelEdit.availability = [this] { return m_undoService.HasActiveEditSession(); };
    registerCommand(std::move(cancelEdit));

    auto captureCommand = makeCommand(
        "debug.renderdoc.capture-next-frame",
        "Capture Next Frame",
        "Debugging",
        ShortcutContexts::Global,
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
        [this]
        {
            Render::Context::DriverUIAccess::OpenLatestRenderDocCapture(*m_context.driver);
        });
    openCaptureCommand.allowDuringTextInput = true;
    registerCommand(std::move(openCaptureCommand));

    const auto registerSceneToolCommand = [&](std::string p_id,
                                              std::string p_displayName,
                                              std::function<void(Panels::SceneView&)> p_execute)
    {
        registerCommand(makeCommand(
            std::move(p_id),
            std::move(p_displayName),
            "Scene View",
            ShortcutContexts::SceneView,
            [this, execute = std::move(p_execute)]
            {
                execute(m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View"));
            }));
    };

    registerSceneToolCommand(
        "scene-view.view-tool",
        "View",
        [](Panels::SceneView&) {});
    registerSceneToolCommand(
        "scene-view.move-tool",
        "Move",
        [](Panels::SceneView& p_sceneView) { p_sceneView.SetCurrentGizmoOperation(EGizmoOperation::TRANSLATE); });
    registerSceneToolCommand(
        "scene-view.rotate-tool",
        "Rotate",
        [](Panels::SceneView& p_sceneView) { p_sceneView.SetCurrentGizmoOperation(EGizmoOperation::ROTATE); });
    registerSceneToolCommand(
        "scene-view.scale-tool",
        "Scale",
        [](Panels::SceneView& p_sceneView) { p_sceneView.SetCurrentGizmoOperation(EGizmoOperation::SCALE); });
    registerSceneToolCommand(
        "scene-view.rect-tool",
        "Rect",
        [](Panels::SceneView& p_sceneView) { p_sceneView.SetCurrentGizmoOperation(EGizmoOperation::RECT); });
    registerSceneToolCommand(
        "scene-view.transform-tool",
        "Transform",
        [](Panels::SceneView& p_sceneView) { p_sceneView.SetCurrentGizmoOperation(EGizmoOperation::TRANSLATE); });
    registerSceneToolCommand(
        "scene-view.toggle-pivot-position",
        "Toggle Pivot Position",
        [](Panels::SceneView& p_sceneView) { p_sceneView.ToggleCurrentGizmoPivot(); });
    registerSceneToolCommand(
        "scene-view.toggle-pivot-orientation",
        "Toggle Pivot Orientation",
        [](Panels::SceneView& p_sceneView) { p_sceneView.ToggleCurrentGizmoSpace(); });

    registerSceneToolCommand(
        "scene-view.toggle-selected-visibility", "Toggle Selected Scene Visibility",
        [](Panels::SceneView& sceneView)
        {
            if (auto* active = EDITOR_EXEC(GetActiveGameObject()))
            {
                const bool visible = sceneView.IsObjectHidden(*active);
                for (auto* object : EDITOR_EXEC(GetSelectedGameObjects())) sceneView.SetObjectVisibility(*object, visible);
            }
        });

    const auto deleteSelectedGameObjectThroughUndo = [this](const InvocationContext& invocation)
    {
        auto selected = m_editorActions.GetSelectedGameObjects();
        if (invocation.targetInstance != 0)
        {
            auto* target = Object::IDToPointer<Engine::GameObject>(invocation.targetInstance);
            if (!target) return CommandResult::TargetExpired;
            if (std::find(selected.begin(), selected.end(), target) == selected.end()) selected = {target};
        }
        if (selected.empty() || selected.front()->GetScene() == nullptr)
            return CommandResult::TargetExpired;

        const auto scope = m_editorActions.GetUndoScopeForScene(*selected.front()->GetScene());
        if (!scope.IsValid())
            return CommandResult::Disabled;

        const auto result = Panels::HierarchyUndo::DeleteGameObjects(m_undoService, selected, scope);
        if (!result.Succeeded())
        {
            NLS_LOG_WARNING(
                "Delete Selected GameObject was rejected by the global Undo service: " +
                (result.message.empty() ? std::string("Unknown error.") : result.message));
        }
        return result.Succeeded() ? CommandResult::Completed : CommandResult::Failed;
    };

    registerCommand(makeCommand(
        "edit.delete-selected-gameobject",
        "Delete Selected GameObject",
        "Edit",
        ShortcutContexts::SceneView,
        [deleteSelectedGameObjectThroughUndo]
        {
            deleteSelectedGameObjectThroughUndo({});
        }));

    registerCommand(makeCommand(
        "edit.delete-selected-gameobject-hierarchy",
        "Delete Selected GameObject",
        "Edit",
        ShortcutContexts::Hierarchy,
        [deleteSelectedGameObjectThroughUndo]
        {
            deleteSelectedGameObjectThroughUndo({});
        }));

    m_shortcutService.GetRegistry().FindCommand("edit.delete-selected-gameobject")->invoke = deleteSelectedGameObjectThroughUndo;
    m_shortcutService.GetRegistry().FindCommand("edit.delete-selected-gameobject-hierarchy")->invoke = deleteSelectedGameObjectThroughUndo;
    for (const auto& context : {ShortcutContexts::SceneView, ShortcutContexts::Hierarchy})
    {
        const std::string suffix = context == ShortcutContexts::SceneView ? "scene" : "hierarchy";
        registerCommand(makeCommand("edit.duplicate-selection-" + suffix, "Duplicate Selected GameObjects", "Edit", context,
            [this]
            {
                const auto objects = m_editorActions.GetSelectedGameObjects();
                if (objects.empty()) return;
                const auto result = Panels::HierarchyUndo::DuplicateGameObjects(m_undoService, objects,
                    m_editorActions.GetUndoScopeForScene(*objects.front()->GetScene()));
                if (!result.Succeeded()) NLS_LOG_WARNING("Duplicate selection failed: " + result.message);
            }));
        registerCommand(makeCommand("edit.select-all-" + suffix, "Select All GameObjects", "Edit", context,
            [this, context]
            {
                if (context == ShortcutContexts::Hierarchy) m_panelsManager.GetPanelAs<Panels::Hierarchy>("Hierarchy").SelectAll();
                else m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View").SelectAll();
            }));
        registerCommand(makeCommand("edit.frame-selection-" + suffix, "Frame Selected GameObjects", "Edit", context,
            [this] { m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View").FocusSelection(); }));
        m_shortcutService.GetRegistry().FindCommand("edit.duplicate-selection-" + suffix)->invoke = [this](const InvocationContext& invocation)
        {
            auto objects = m_editorActions.GetSelectedGameObjects();
            if (invocation.targetInstance)
            {
                auto* target = Object::IDToPointer<Engine::GameObject>(invocation.targetInstance);
                if (!target) return CommandResult::TargetExpired;
                if (std::find(objects.begin(), objects.end(), target) == objects.end()) objects = {target};
            }
            if (objects.empty() || !objects.front()->GetScene()) return CommandResult::TargetExpired;
            const auto result = Panels::HierarchyUndo::DuplicateGameObjects(m_undoService, objects, m_editorActions.GetUndoScopeForScene(*objects.front()->GetScene()));
            return result.Succeeded() ? CommandResult::Completed : CommandResult::Failed;
        };
        m_shortcutService.GetRegistry().FindCommand("edit.frame-selection-" + suffix)->invoke = [this](const InvocationContext& invocation)
        {
            if (invocation.targetInstance)
            {
                auto* target = Object::IDToPointer<Engine::GameObject>(invocation.targetInstance);
                if (!target) return CommandResult::TargetExpired;
                m_editorActions.MoveToTarget(*target);
            }
            else m_panelsManager.GetPanelAs<Panels::SceneView>("Scene View").FocusSelection();
            return CommandResult::Completed;
        };
    }
}

void Editor::Core::Editor::RestoreStartupScene()
{
    struct ResidentStartupSceneRestoreScope
    {
        std::shared_ptr<NLS::Editor::Assets::ResidentPrefabPreviewRegistry> registry;

        explicit ResidentStartupSceneRestoreScope(
            std::shared_ptr<NLS::Editor::Assets::ResidentPrefabPreviewRegistry> value)
            : registry(std::move(value))
        {
            if (registry != nullptr)
                registry->SetSceneRestoreInProgress(true);
        }

        ~ResidentStartupSceneRestoreScope()
        {
            if (registry != nullptr)
                registry->SetSceneRestoreInProgress(false);
        }
    } residentStartupSceneRestoreScope(m_context.residentPrefabPreviewRegistry);

    const auto restoreStartupSceneBegin = std::chrono::steady_clock::now();
    const auto restoreLoadedScenePrefabs = [this]()
    {
        const auto prefabRestoreBegin = std::chrono::steady_clock::now();
        NLS_LOG_INFO("[Startup] RestoreStartupScene prefab restore begin");
        m_context.PresentStartupProgressFrame("Restoring startup scene prefab instances", 0.87f);
        const bool prefabRestoreSucceeded = m_editorActions.RestorePrefabInstancesForCurrentSceneFromDisk(true);
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

#if defined(NLS_ENABLE_TEST_HOOKS)
    if (const auto* source = std::getenv("NLS_SPRITE_EDITOR_VALIDATION_SOURCE"); source && *source)
    {
        auto& sprites = m_panelsManager.GetPanelAs<Panels::SpriteEditor>("Sprite Editor");
        const bool opened = sprites.OpenDocument(source);
        NLS_LOG_INFO(std::string("Sprite Editor validation open: ") + (opened ? "ready " : "failed ") + source);
    }
#endif
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
    m_editorActions.PumpPlayRequests();
    auto& game = m_panelsManager.GetPanelAs<Panels::GameView>("Game View");
    auto input = game.CaptureGameInput();
    if (m_editorActions.GetPlayModeController().IsActive())
        NLS::Scripting::ScriptEngineApi::SetGameInputFrame(std::move(input));
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
    auto& runtime = m_editorActions.GetRuntimePlaySession();
    const auto& presentation=m_panelsManager.GetPanelAs<Panels::GameView>("Game View").GetPresentation();
    runtime.ConfigureUI(m_context.projectAssetsPath,std::max(1u,presentation.width),std::max(1u,presentation.height),1,
        Settings::EditorSettings::GetGameViewSettingsObject().display);
    runtime.SetMuted(Settings::EditorSettings::GetGameViewSettingsObject().muteAudio);
    try
    {
        if (m_editorActions.GetPlayModeController().GetState() == PlaySessionState::Stepping)
        {
            runtime.Step();
            m_editorActions.GetPlayModeController().CompleteStep();
        }
        else runtime.Tick(p_deltaTime);
    }
    catch(const std::exception& error)
    {
        NLS_LOG_ERROR(std::string("Play session failed: ")+error.what());
        m_editorActions.StopPlaying();m_editorActions.PumpPlayRequests();
    }
    catch(...)
    {
        NLS_LOG_ERROR("Play session failed with an unknown host error.");
        m_editorActions.StopPlaying();m_editorActions.PumpPlayRequests();
    }
}

void Editor::Core::Editor::UpdateEditMode(float p_deltaTime)
{
    NLS_PROFILE_SCOPE();
    (void)p_deltaTime;
}

void Editor::Core::Editor::UpdateEditorPanels(float p_deltaTime)
{
    NLS_PROFILE_SCOPE();
    m_bakingService.Tick(m_context.sceneManager.GetCurrentScene(),
        m_editorActions.GetCurrentEditorMode() == EditorActions::EEditorMode::EDIT &&
        !m_context.sceneManager.IsPlaySandboxActive());
    m_panelsManager.GetPanelAs<Panels::Console>("Console").UpdateLogState();
    (void)p_deltaTime;
    auto& frameInfo = m_panelsManager.GetPanelAs<NLS::Editor::Panels::FrameInfo>("Frame Info");
    auto& sceneView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::SceneView>("Scene View");
    auto& gameView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::GameView>("Game View");
    auto& assetView = m_panelsManager.GetPanelAs<NLS::Editor::Panels::AssetView>("Asset View");

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
        const EditorCameraPerformanceStageScope stageScope(EditorCameraPerformanceStage::SceneViewUpdate);
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
        const EditorCameraPerformanceStageScope stageScope(EditorCameraPerformanceStage::PresentSwapchain);
        Render::Context::DriverUIAccess::PresentSwapchain(*m_context.driver);
    }
    if (Render::Settings::GetThreadDiagnosticsSettings().dx12LogFrameFlow)
        NLS_LOG_INFO("Editor::PostUpdate: PresentSwapchain returned");

    {
        NLS_PROFILE_NAMED_SCOPE("InputManager::ClearEvents");
        m_context.inputManager->ClearEvents();
    }
    ++m_elapsedFrames;
    if (m_pluginExtensions) m_pluginExtensions->Sync();
    if (NLS::Core::ServiceLocator::Contains<NLS::Plugins::PluginManager>())
        NLS::Core::ServiceLocator::Get<NLS::Plugins::PluginManager>().Pump();
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

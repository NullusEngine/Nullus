#include <array>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <cstring>
#include <exception>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <ShellAPI.h>
#endif

#include "ImGui/imgui.h"
#include "ImGui/imgui_internal.h"
#include <Json/json.hpp>

#include "Assets/ArtifactLoadTelemetry.h"
#include "Assets/ScriptAssetUtility.h"
#include "Debug/ExternalCodeEditor.h"
#include "Debug/VSCodeConfigurationGenerator.h"
#include "Debug/VisualStudioConfigurationGenerator.h"
#include "Debug/EditorDebugEndpoint.h"
#include "Scripting/ManagedScriptDebugSession.h"
#include "Scripting/ManagedScriptProjectPaths.h"

#include <UI/Widgets/Texts/TextClickable.h>
#include <UI/Widgets/Visual/Image.h>
#include <UI/Widgets/Visual/Separator.h>
#include <UI/Widgets/Buttons/Button.h>
#include <UI/Widgets/Layout/Group.h>
#include <UI/Widgets/Layout/Spacing.h>
#include <UI/Plugins/DDSource.h>
#include <UI/Plugins/DDTarget.h>
#include <UI/Plugins/DragDrop.h>
#include <UI/Plugins/ContextualMenu.h>

#include <Windowing/Dialogs/MessageBox.h>
#include <Windowing/Dialogs/SaveFileDialog.h>
#include <Windowing/Dialogs/OpenFileDialog.h>
#include <Windowing/Window.h>
#include <Platform/Process/Process.h>
#include <Utils/SystemCalls.h>
#include <Utils/PathParser.h>
#include <Utils/String.h>
#include <Jobs/BackgroundJobQueue.h>
#include <Jobs/JobSystem.h>

#include <ServiceLocator.h>
#include <ResourceManagement/MeshManager.h>
#include <ResourceManagement/TextureManager.h>
#include <ResourceManagement/ShaderManager.h>

#include <Debug/Logger.h>
#include <Image.h>
#include <Profiling/Profiler.h>

#include "Panels/MaterialEditor.h"
#include "Panels/AssetBrowser.h"
#include "Panels/AssetView.h"
#include "Panels/AssetProperties.h"
#include "Panels/Hierarchy.h"
#include "Panels/SceneView.h"
#include "Assets/AssetBrowserPresentation.h"
#include "Assets/AssetThumbnailPool.h"
#include "Assets/AssetDatabaseRetirementScheduler.h"
#include "Assets/AssetDatabaseFacade.h"
#include "Assets/ArtifactDatabaseManifestUtils.h"
#include "Assets/EditorThumbnailPreviewRenderer.h"
#include "Assets/ThumbnailRendererRegistry.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Assets/EditorAssetPath.h"
#include "Assets/ArtifactManifest.h"
#include "Assets/AssetMeta.h"
#include "Assets/AssetImporterFacade.h"
#include "Assets/EditorAssetDatabase.h"
#include "Core/EditorActions.h"
#include "Core/Context.h"
#include "Assets/AssetDragDropWorkflow.h"
#include "Assets/ShaderLabMaterialDefaults.h"
#include "Assets/PrefabUtilityFacade.h"
#include "Core/EditorResources.h"
#include "Core/RecentBackgroundWorkGate.h"
#include "GameObject.h"
#include "Rendering/Context/DriverAccess.h"
#include "SceneSystem/SceneManager.h"
#include "UI/Widgets/InputFields/InputText.h"
#include "UI/UIManager.h"
#include "Rendering/Context/Driver.h"
#include "Rendering/Resources/Loaders/MaterialLoader.h"
#include "Rendering/Resources/Texture2D.h"

using namespace NLS;
using namespace NLS::UI;
using namespace NLS::UI::Widgets;

#define FILENAMES_CHARS Editor::Panels::AssetBrowser::__FILENAMES_CHARS

const std::string FILENAMES_CHARS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-_=+ 0123456789()[]";

namespace
{
constexpr size_t kMaxAssetBrowserThumbnailDrawOutcomePaths = 64u;
constexpr int kAssetBrowserThumbnailDrawOutcomeSamplePeriodFrames = 15;
constexpr size_t kAssetBrowserThumbnailVisibleSetStableFrames = 3u;
constexpr auto kAssetBrowserThumbnailInitialVisibleTimeout = std::chrono::seconds(30);
constexpr double kAssetBrowserSceneLoadThumbnailEscapeSeconds = 2.0;
constexpr double kAssetBrowserVisiblePendingPresentationRecoveryIntervalSeconds = 0.25;
constexpr size_t kMaxAssetBrowserVisiblePendingPresentationProbesPerPump = 2u;

std::optional<std::string> LoadScriptTemplate(
	const std::filesystem::path& editorAssetFolder,
	const std::string_view templateFileName,
	const std::string_view scriptName)
{
	const auto templatePath = editorAssetFolder / "ScriptTemplates" / templateFileName;
	std::ifstream input(templatePath, std::ios::binary);
	if (!input)
		return std::nullopt;

	std::string contents {
		std::istreambuf_iterator<char>(input),
		std::istreambuf_iterator<char>() };
	constexpr std::string_view scriptNameToken = "#SCRIPTNAME#";
	for (size_t position = contents.find(scriptNameToken);
		position != std::string::npos;
		position = contents.find(scriptNameToken, position + scriptName.size()))
	{
		contents.replace(position, scriptNameToken.size(), scriptName);
	}
	return contents;
}

NLS::Render::RHI::TextureColorSpace CachedThumbnailTextureColorSpace(
	const NLS::Editor::Assets::AssetThumbnailKind kind)
{
	return kind == NLS::Editor::Assets::AssetThumbnailKind::Texture
		? NLS::Render::RHI::TextureColorSpace::Linear
		: NLS::Render::RHI::TextureColorSpace::SRGB;
}

NLS::Render::RHI::TextureColorSpace CachedThumbnailTextureColorSpace(
	const NLS::Editor::Assets::AssetThumbnailServiceResult& result)
{
	return CachedThumbnailTextureColorSpace(
		static_cast<NLS::Editor::Assets::AssetThumbnailKind>(result.requestKind));
}

struct AssetBrowserThumbnailDrawOutcomeTelemetryState
{
    std::array<size_t, 3u> counts {};
    size_t droppedPathCount = 0u;
    std::unordered_map<std::string, size_t> pathCounts;
    std::unordered_map<std::string, size_t> requestBuildFailurePathCounts;
    bool initialVisibleSetStarted = false;
    bool initialVisibleMeasurementClosed = false;
    size_t initialVisibleThumbnailCount = 0u;
    size_t initialVisiblePendingAfter30SecondsCount = 0u;
    bool initialVisibleTimedOut = false;
    std::chrono::steady_clock::time_point initialVisibleSetStartedAt {};
    uint64_t initialVisibleSetFingerprint = 0u;
    uint64_t candidateVisibleSetFingerprint = 0u;
    size_t candidateVisibleSetStableFrameCount = 0u;
    std::vector<std::string> candidateVisibleItemKeys;
    std::unordered_set<std::string> initialVisibleItemKeys;
    std::unordered_set<std::string> explicitTypeFallbackItemKeys;
    std::unordered_set<std::string> canonicalVisiblePaths;
    std::unordered_map<
        std::string,
        NLS::Editor::Assets::ThumbnailPresentationState> initialVisiblePresentationStates;
    std::unordered_map<std::string, std::string> initialVisiblePresentationDetails;
    std::optional<double> firstCanonicalDrawMs;
    std::optional<double> canonical90PercentFillMs;
    std::mutex mutex;
};

AssetBrowserThumbnailDrawOutcomeTelemetryState& ThumbnailDrawOutcomeTelemetryState()
{
    static AssetBrowserThumbnailDrawOutcomeTelemetryState state;
    return state;
}

size_t CountInitialVisibleLoadingItems(
    const AssetBrowserThumbnailDrawOutcomeTelemetryState& state)
{
    using NLS::Editor::Assets::ThumbnailPresentationState;
    size_t loadingCount = 0u;
    for (const auto& [_, presentationState] : state.initialVisiblePresentationStates)
    {
        if (presentationState == ThumbnailPresentationState::Loading ||
            presentationState == ThumbnailPresentationState::StaleRefreshing)
        {
            ++loadingCount;
        }
    }

    if (state.initialVisiblePresentationStates.empty() &&
        state.initialVisibleThumbnailCount > state.canonicalVisiblePaths.size())
    {
        loadingCount = state.initialVisibleThumbnailCount - state.canonicalVisiblePaths.size();
    }
    return loadingCount;
}

void CloseInitialVisibleMeasurementIfExpired(
    AssetBrowserThumbnailDrawOutcomeTelemetryState& state,
    const std::chrono::steady_clock::time_point now)
{
    if (!state.initialVisibleSetStarted || state.initialVisibleMeasurementClosed ||
        now - state.initialVisibleSetStartedAt < kAssetBrowserThumbnailInitialVisibleTimeout)
    {
        return;
    }

    state.initialVisibleMeasurementClosed = true;
    state.initialVisiblePendingAfter30SecondsCount = CountInitialVisibleLoadingItems(state);
    state.initialVisibleTimedOut = state.initialVisiblePendingAfter30SecondsCount != 0u;
}

size_t ThumbnailDrawOutcomeIndex(const NLS::Editor::Panels::AssetBrowserThumbnailDrawOutcome outcome)
{
    return static_cast<size_t>(outcome);
}

}

void NLS::Editor::Panels::RecordAssetBrowserThumbnailDrawOutcomeTelemetry(
    const std::string_view assetPath,
    const AssetBrowserThumbnailDrawOutcome outcome)
{
    if (!NLS::Core::Assets::IsArtifactLoadTelemetryEnabled())
        return;

    const char* suffix = "|draw=thumbnail";
    switch (outcome)
    {
    case AssetBrowserThumbnailDrawOutcome::Thumbnail:
        suffix = "|draw=thumbnail";
        break;
    case AssetBrowserThumbnailDrawOutcome::Fallback:
        suffix = "|draw=fallback";
        break;
    case AssetBrowserThumbnailDrawOutcome::TypeFallback:
        suffix = "|draw=type-fallback";
        break;
    }

    auto& state = ThumbnailDrawOutcomeTelemetryState();
    std::lock_guard lock(state.mutex);
    ++state.counts[ThumbnailDrawOutcomeIndex(outcome)];

    std::string path;
    path.reserve(assetPath.size() + std::char_traits<char>::length(suffix));
    path.append(assetPath.data(), assetPath.size());
    path += suffix;
    if (auto found = state.pathCounts.find(path); found != state.pathCounts.end())
    {
        ++found->second;
        return;
    }
    if (state.pathCounts.size() >= kMaxAssetBrowserThumbnailDrawOutcomePaths)
    {
        ++state.droppedPathCount;
        return;
    }
    state.pathCounts.emplace(std::move(path), 1u);
}

NLS::Editor::Panels::AssetBrowserThumbnailDrawOutcomeTelemetrySnapshot
NLS::Editor::Panels::SnapshotAssetBrowserThumbnailDrawOutcomeTelemetry()
{
    auto& state = ThumbnailDrawOutcomeTelemetryState();
    std::lock_guard lock(state.mutex);
    CloseInitialVisibleMeasurementIfExpired(state, std::chrono::steady_clock::now());

    AssetBrowserThumbnailDrawOutcomeTelemetrySnapshot snapshot;
    snapshot.thumbnailDrawCount = state.counts[ThumbnailDrawOutcomeIndex(AssetBrowserThumbnailDrawOutcome::Thumbnail)];
    snapshot.fallbackDrawCount = state.counts[ThumbnailDrawOutcomeIndex(AssetBrowserThumbnailDrawOutcome::Fallback)];
    snapshot.typeFallbackDrawCount = state.counts[ThumbnailDrawOutcomeIndex(AssetBrowserThumbnailDrawOutcome::TypeFallback)];
    snapshot.droppedPathCount = state.droppedPathCount;
    snapshot.requestBuildFailurePathTotals.reserve(state.requestBuildFailurePathCounts.size());
    for (const auto& [path, count] : state.requestBuildFailurePathCounts)
        snapshot.requestBuildFailurePathTotals.push_back({ path, count });
    snapshot.initialVisibleThumbnailCount = state.initialVisibleThumbnailCount;
    snapshot.canonicalVisibleThumbnailCount = state.canonicalVisiblePaths.size();
    snapshot.initialVisibleSetFingerprint = state.initialVisibleSetFingerprint;
    for (const auto& [_, presentationState] : state.initialVisiblePresentationStates)
    {
        using NLS::Editor::Assets::ThumbnailPresentationState;
        switch (presentationState)
        {
        case ThumbnailPresentationState::Loading:
        case ThumbnailPresentationState::StaleRefreshing:
            ++snapshot.initialVisibleLoadingCount;
            break;
        case ThumbnailPresentationState::Ready:
            ++snapshot.initialVisibleReadyCount;
            break;
        case ThumbnailPresentationState::FailedRetained:
            ++snapshot.initialVisibleFailedCount;
            break;
        case ThumbnailPresentationState::Fallback:
            ++snapshot.initialVisibleFallbackCount;
            break;
        }
    }
    snapshot.initialVisibleAllTerminal =
        !state.initialVisiblePresentationStates.empty() &&
        snapshot.initialVisibleLoadingCount == 0u;
    snapshot.initialVisibleTimedOut = state.initialVisibleTimedOut;
    snapshot.initialVisiblePendingAfter30SecondsCount =
        state.initialVisiblePendingAfter30SecondsCount;
    snapshot.firstCanonicalDrawMs = state.firstCanonicalDrawMs;
    snapshot.canonical90PercentFillMs = state.canonical90PercentFillMs;
    snapshot.initialVisiblePresentationDetails.reserve(
        state.initialVisiblePresentationDetails.size());
    for (const auto& [_, detail] : state.initialVisiblePresentationDetails)
        snapshot.initialVisiblePresentationDetails.push_back(detail);
    std::sort(
        snapshot.initialVisiblePresentationDetails.begin(),
        snapshot.initialVisiblePresentationDetails.end());
    snapshot.pathTotals.reserve(state.pathCounts.size());
    snapshot.initialVisibleCanonicalEligibleCount = 0u;
    for (const auto& entry : state.initialVisiblePresentationStates)
    {
        const auto& itemKey = entry.first;
        if (state.explicitTypeFallbackItemKeys.find(itemKey) ==
            state.explicitTypeFallbackItemKeys.end())
            ++snapshot.initialVisibleCanonicalEligibleCount;
    }
    if (state.initialVisiblePresentationStates.empty())
        snapshot.initialVisibleCanonicalEligibleCount = state.initialVisibleThumbnailCount;
    for (const auto& [path, count] : state.pathCounts)
        snapshot.pathTotals.push_back({ path, count });
    return snapshot;
}

void NLS::Editor::Panels::RecordAssetBrowserThumbnailRequestBuildFailureTelemetry(
    const std::string_view itemIdentity)
{
    if (!NLS::Core::Assets::IsArtifactLoadTelemetryEnabled() || itemIdentity.empty())
        return;

    auto& state = ThumbnailDrawOutcomeTelemetryState();
    std::lock_guard lock(state.mutex);
    if (!state.initialVisibleSetStarted ||
        state.initialVisibleItemKeys.find(std::string(itemIdentity)) == state.initialVisibleItemKeys.end())
    {
        return;
    }
    auto found = state.requestBuildFailurePathCounts.find(std::string(itemIdentity));
    if (found != state.requestBuildFailurePathCounts.end())
    {
        ++found->second;
        return;
    }
    if (state.requestBuildFailurePathCounts.size() >= kMaxAssetBrowserThumbnailDrawOutcomePaths)
        return;
    state.requestBuildFailurePathCounts.emplace(std::string(itemIdentity), 1u);
}

void NLS::Editor::Panels::BeginAssetBrowserThumbnailVisibleSetTelemetry(const size_t thumbnailCount)
{
    BeginAssetBrowserThumbnailVisibleSetTelemetry(
        thumbnailCount,
        {});
}

void NLS::Editor::Panels::BeginAssetBrowserThumbnailVisibleSetTelemetry(
    const size_t thumbnailCount,
    const std::vector<std::string>& itemKeys)
{
    if (!NLS::Core::Assets::IsArtifactLoadTelemetryEnabled() || thumbnailCount == 0u)
        return;

    auto& state = ThumbnailDrawOutcomeTelemetryState();
    std::lock_guard lock(state.mutex);
    if (state.initialVisibleSetStarted)
        return;

    state.initialVisibleSetStarted = true;
    state.initialVisibleMeasurementClosed = false;
    state.initialVisibleThumbnailCount = thumbnailCount;
    state.initialVisiblePendingAfter30SecondsCount = 0u;
    state.initialVisibleTimedOut = false;
    state.initialVisibleItemKeys.clear();
    state.initialVisibleItemKeys.insert(itemKeys.begin(), itemKeys.end());
    state.initialVisiblePresentationStates.clear();
    state.initialVisiblePresentationDetails.clear();
    for (const auto& itemKey : itemKeys)
    {
        if (!itemKey.empty())
        {
            state.initialVisiblePresentationStates.emplace(
                itemKey,
                NLS::Editor::Assets::ThumbnailPresentationState::Loading);
        }
    }
    state.initialVisibleSetStartedAt = std::chrono::steady_clock::now();
}

void NLS::Editor::Panels::BeginAssetBrowserThumbnailVisibleSetTelemetry(
    const size_t thumbnailCount,
    const std::vector<std::string>& itemKeys,
    const uint64_t visibleSetFingerprint)
{
    if (!NLS::Core::Assets::IsArtifactLoadTelemetryEnabled() || thumbnailCount == 0u)
        return;

    auto& state = ThumbnailDrawOutcomeTelemetryState();
    std::lock_guard lock(state.mutex);
    if (state.initialVisibleSetStarted)
        return;

    // Folder enumeration and generated-subasset disclosure arrive asynchronously.
    // Freeze only after the same visible collection has survived several UI frames,
    // otherwise matched editor trials can measure different initial sets.
    if (state.candidateVisibleSetFingerprint == visibleSetFingerprint &&
        state.candidateVisibleItemKeys == itemKeys)
    {
        ++state.candidateVisibleSetStableFrameCount;
    }
    else
    {
        state.candidateVisibleSetFingerprint = visibleSetFingerprint;
        state.candidateVisibleItemKeys = itemKeys;
        state.candidateVisibleSetStableFrameCount = 1u;
    }
    if (state.candidateVisibleSetStableFrameCount < kAssetBrowserThumbnailVisibleSetStableFrames)
        return;

    state.initialVisibleSetStarted = true;
    state.initialVisibleMeasurementClosed = false;
    state.initialVisibleThumbnailCount = thumbnailCount;
    state.initialVisibleSetFingerprint = visibleSetFingerprint;
    state.initialVisiblePendingAfter30SecondsCount = 0u;
    state.initialVisibleTimedOut = false;
    state.initialVisibleItemKeys.clear();
    state.initialVisibleItemKeys.insert(itemKeys.begin(), itemKeys.end());
    state.initialVisiblePresentationStates.clear();
    state.initialVisiblePresentationDetails.clear();
    for (const auto& itemKey : itemKeys)
    {
        if (!itemKey.empty())
        {
            state.initialVisiblePresentationStates.emplace(
                itemKey,
                NLS::Editor::Assets::ThumbnailPresentationState::Loading);
        }
    }
    state.initialVisibleSetStartedAt = std::chrono::steady_clock::now();
}

void NLS::Editor::Panels::RecordAssetBrowserThumbnailCanonicalDrawTelemetry(const std::string_view itemIdentity)
{
    if (!NLS::Core::Assets::IsArtifactLoadTelemetryEnabled())
        return;

    auto& state = ThumbnailDrawOutcomeTelemetryState();
    std::lock_guard lock(state.mutex);
    CloseInitialVisibleMeasurementIfExpired(state, std::chrono::steady_clock::now());
    if (!state.initialVisibleSetStarted)
        return;

    std::string path(itemIdentity);
    if (path.empty())
        path = "<anonymous-thumbnail>";
    if (!state.initialVisibleItemKeys.empty() &&
        state.initialVisibleItemKeys.find(path) == state.initialVisibleItemKeys.end())
    {
        return;
    }
    if (!state.canonicalVisiblePaths.insert(std::move(path)).second)
        return;

    if (state.initialVisibleMeasurementClosed)
        return;

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - state.initialVisibleSetStartedAt);
    const double elapsedMs = static_cast<double>(elapsed.count()) / 1000.0;
    if (!state.firstCanonicalDrawMs.has_value())
        state.firstCanonicalDrawMs = elapsedMs;

    size_t canonicalEligibleCount = 0u;
    for (const auto& [itemKey, _] : state.initialVisiblePresentationStates)
    {
        if (state.explicitTypeFallbackItemKeys.find(itemKey) ==
            state.explicitTypeFallbackItemKeys.end())
            ++canonicalEligibleCount;
    }
    if (canonicalEligibleCount == 0u)
        canonicalEligibleCount = state.initialVisibleThumbnailCount;
    const size_t targetCount = (std::max<size_t>)(
        1u,
        static_cast<size_t>(std::ceil(
            static_cast<double>(canonicalEligibleCount) * 0.9)));
    if (!state.canonical90PercentFillMs.has_value() &&
        state.canonicalVisiblePaths.size() >= targetCount)
    {
        state.canonical90PercentFillMs = elapsedMs;
    }
}

void NLS::Editor::Panels::RecordAssetBrowserThumbnailPresentationStateTelemetry(
    const std::string_view itemIdentity,
    const NLS::Editor::Assets::ThumbnailPresentationState presentationState,
    const NLS::Editor::Assets::AssetThumbnailServiceResult* result)
{
    if (!NLS::Core::Assets::IsArtifactLoadTelemetryEnabled() || itemIdentity.empty())
        return;

    auto& state = ThumbnailDrawOutcomeTelemetryState();
    std::lock_guard lock(state.mutex);
    CloseInitialVisibleMeasurementIfExpired(state, std::chrono::steady_clock::now());
    if (!state.initialVisibleSetStarted ||
        state.initialVisibleItemKeys.find(std::string(itemIdentity)) == state.initialVisibleItemKeys.end())
    {
        return;
    }
    const std::string itemKey(itemIdentity);
    state.initialVisiblePresentationStates[itemKey] = presentationState;
    if (result != nullptr)
    {
        std::string detail = itemKey;
        detail += "|status=" + std::to_string(static_cast<unsigned int>(result->status));
        detail += "|presentation=" +
            std::to_string(static_cast<unsigned int>(result->presentationState));
        detail += "|quality=" +
            std::to_string(static_cast<unsigned int>(result->previewQuality));
        detail += "|presentationKey=" + result->presentationKey;
        detail += "|revision=" + std::to_string(result->requestRevision);
        detail += "|refresh=" + std::to_string(result->refreshPending ? 1u : 0u);
        detail += "|image=" + std::to_string(result->imagePath.empty() ? 0u : 1u);
        detail += "|retained=" + std::to_string(result->retainedImage.has_value() ? 1u : 0u);
        detail += "|gpu=" + std::to_string(result->gpuTexture.IsValid() ? 1u : 0u);
        detail += "|cacheKey=";
        if (result->cacheEntry.has_value())
            detail += result->cacheEntry->cacheKey;
        if (!result->diagnostic.empty())
            detail += "|diagnostic=" + result->diagnostic;
        state.initialVisiblePresentationDetails.insert_or_assign(itemKey, std::move(detail));
    }
    if (state.initialVisibleMeasurementClosed)
        return;

    if (!state.canonical90PercentFillMs.has_value())
    {
        size_t canonicalEligibleCount = 0u;
        for (const auto& [itemKey, _] : state.initialVisiblePresentationStates)
        {
            if (state.explicitTypeFallbackItemKeys.find(itemKey) ==
                state.explicitTypeFallbackItemKeys.end())
                ++canonicalEligibleCount;
        }
        if (canonicalEligibleCount == 0u)
            canonicalEligibleCount = state.initialVisibleThumbnailCount;
        const size_t targetCount = (std::max<size_t>)(
            1u,
            static_cast<size_t>(std::ceil(
                static_cast<double>(canonicalEligibleCount) * 0.9)));
        if (state.canonicalVisiblePaths.size() >= targetCount)
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - state.initialVisibleSetStartedAt);
            state.canonical90PercentFillMs =
                static_cast<double>(elapsed.count()) / 1000.0;
        }
    }
}

void NLS::Editor::Panels::RecordAssetBrowserThumbnailTypeFallbackTelemetry(
    const std::string_view itemIdentity)
{
    if (!NLS::Core::Assets::IsArtifactLoadTelemetryEnabled() || itemIdentity.empty())
        return;

    auto& state = ThumbnailDrawOutcomeTelemetryState();
    std::lock_guard lock(state.mutex);
    if (!state.initialVisibleSetStarted ||
        state.initialVisibleItemKeys.find(std::string(itemIdentity)) == state.initialVisibleItemKeys.end())
    {
        return;
    }
    state.explicitTypeFallbackItemKeys.emplace(itemIdentity);
}

#if defined(NLS_ENABLE_TEST_HOOKS)
void NLS::Editor::Panels::ClearAssetBrowserThumbnailDrawOutcomeTelemetryForTesting()
{
    auto& state = ThumbnailDrawOutcomeTelemetryState();
    std::lock_guard lock(state.mutex);
    state.counts = {};
    state.droppedPathCount = 0u;
    state.pathCounts.clear();
    state.requestBuildFailurePathCounts.clear();
    state.initialVisibleSetStarted = false;
    state.initialVisibleMeasurementClosed = false;
    state.initialVisibleThumbnailCount = 0u;
    state.initialVisiblePendingAfter30SecondsCount = 0u;
    state.initialVisibleTimedOut = false;
    state.initialVisibleSetStartedAt = {};
    state.initialVisibleSetFingerprint = 0u;
    state.candidateVisibleSetFingerprint = 0u;
    state.candidateVisibleSetStableFrameCount = 0u;
    state.candidateVisibleItemKeys.clear();
    state.initialVisibleItemKeys.clear();
    state.explicitTypeFallbackItemKeys.clear();
    state.canonicalVisiblePaths.clear();
    state.initialVisiblePresentationStates.clear();
    state.initialVisiblePresentationDetails.clear();
    state.firstCanonicalDrawMs.reset();
    state.canonical90PercentFillMs.reset();
}

void NLS::Editor::Panels::ExpireAssetBrowserThumbnailVisibleSetTelemetryForTesting()
{
    auto& state = ThumbnailDrawOutcomeTelemetryState();
    std::lock_guard lock(state.mutex);
    if (state.initialVisibleSetStarted)
    {
        state.initialVisibleSetStartedAt =
            std::chrono::steady_clock::now() - kAssetBrowserThumbnailInitialVisibleTimeout;
    }
}
#endif

template <typename Function, typename CancelFunction>
auto ScheduleAssetBrowserJobFuture(
	const char* debugName,
	Function&& function,
	CancelFunction&& cancelFunction)
{
	using Result = std::invoke_result_t<std::decay_t<Function>&>;

	struct JobState
	{
		std::promise<Result> promise;
		std::decay_t<Function> function;
		std::decay_t<CancelFunction> cancelFunction;
	};

	auto state = std::make_unique<JobState>(JobState {
		std::promise<Result> {},
		std::forward<Function>(function),
		std::forward<CancelFunction>(cancelFunction),
	});
	auto future = state->promise.get_future();
	auto* statePtr = state.release();

	NLS::Base::Jobs::BackgroundJobDesc desc {};
	desc.userData = statePtr;
	desc.debugName = debugName;
	desc.function = [](void* userData)
	{
		std::unique_ptr<JobState> ownedState(static_cast<JobState*>(userData));
		try
		{
			if constexpr (std::is_void_v<Result>)
			{
				ownedState->function();
				ownedState->promise.set_value();
			}
			else
			{
				ownedState->promise.set_value(ownedState->function());
			}
		}
		catch (...)
		{
			ownedState->promise.set_exception(std::current_exception());
		}
	};
	desc.cancelUserData = statePtr;
	desc.cancelFunction = [](void* userData)
	{
		std::unique_ptr<JobState> ownedState(static_cast<JobState*>(userData));
		auto failure = std::make_exception_ptr(
			std::runtime_error("asset browser background job cancelled before execution"));
		try { ownedState->cancelFunction(); }
		catch (...) { failure = std::current_exception(); }
		ownedState->promise.set_exception(std::move(failure));
	};

	const auto handle = NLS::Base::Jobs::ScheduleBackgroundJob(desc);
	if (handle.id == 0u)
	{
		std::unique_ptr<JobState> ownedState(statePtr);
		throw std::runtime_error(NLS::Base::Jobs::IsJobSystemInitialized()
			? "asset browser background job scheduling rejected"
			: "asset browser background job scheduling requires initialized JobSystem");
	}

	return future;
}

template <typename Function>
auto ScheduleAssetBrowserJobFuture(const char* debugName, Function&& function)
{
	return ScheduleAssetBrowserJobFuture(
		debugName,
		std::forward<Function>(function),
		[] {});
}

std::string GetAssociatedMetaFile(const std::string& p_assetPath)
{
	return p_assetPath + ".meta";
}

std::filesystem::path ProjectRootFromAssetsFolder(const std::string& projectAssetsFolder)
{
	auto assetsPath = std::filesystem::path(projectAssetsFolder).lexically_normal();
	while (!assetsPath.empty() && !assetsPath.has_filename())
		assetsPath = assetsPath.parent_path();
	return assetsPath.parent_path();
}

template <typename T>
void AbandonAssetBrowserFuture(std::future<T>& future)
{
	if (!future.valid())
		return;
	if (future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		try
		{
			(void)future.get();
		}
		catch (...)
		{
		}
		return;
	}

	try
	{
		(void)future.get();
	}
	catch (...)
	{
	}
}

std::filesystem::path EditorAssetFolderFromAbsolutePath(
	const std::string& projectAssetsFolder,
	const std::string& absoluteFolderPath)
{
	const auto projectRoot = ProjectRootFromAssetsFolder(projectAssetsFolder);
	auto relative = std::filesystem::path(absoluteFolderPath).lexically_normal().lexically_relative(projectRoot);
	if (relative.empty() || relative.is_absolute())
		return {};

	for (const auto& part : relative)
	{
		if (part == "..")
			return {};
	}

	return relative;
}

std::filesystem::path EditorAssetPathFromAbsolutePath(
	const std::string& projectAssetsFolder,
	const std::string& absolutePath)
{
	return EditorAssetFolderFromAbsolutePath(projectAssetsFolder, absolutePath);
}

std::string NormalizeProjectBrowserPath(std::string path)
{
	return NLS::Editor::Assets::NormalizeAssetBrowserProjectRelativePath(std::move(path));
}

std::string NormalizeProjectBrowserPath(const std::filesystem::path& path)
{
	const auto text = path.lexically_normal().generic_u8string();
	return NLS::Editor::Assets::NormalizeAssetBrowserProjectRelativePath(
		{ reinterpret_cast<const char*>(text.data()), text.size() });
}

bool IsProjectBrowserAncestorOf(
	const std::string& ancestor,
	const std::string& descendant)
{
	const auto normalizedAncestor = NormalizeProjectBrowserPath(ancestor);
	const auto normalizedDescendant = NormalizeProjectBrowserPath(descendant);
	return normalizedDescendant == normalizedAncestor ||
		(normalizedDescendant.size() > normalizedAncestor.size() &&
		 normalizedDescendant.compare(0u, normalizedAncestor.size(), normalizedAncestor) == 0 &&
		 normalizedDescendant[normalizedAncestor.size()] == '/');
}

void AddProjectBrowserAncestorFolders(
	std::unordered_set<std::string>& expandedFolders,
	const std::string& projectRelativePath)
{
	const auto normalized = NormalizeProjectBrowserPath(projectRelativePath);
	std::string current;
	size_t offset = 0u;
	while (offset <= normalized.size())
	{
		const auto separator = normalized.find('/', offset);
		const auto end = separator == std::string::npos ? normalized.size() : separator;
		const auto length = end - offset;
		if (length > 0u)
		{
			if (!current.empty())
				current += '/';
			current.append(normalized, offset, length);
			const auto currentText = NormalizeProjectBrowserPath(current);
			if (currentText != normalized)
				expandedFolders.insert(currentText);
		}

		if (separator == std::string::npos)
			break;
		offset = separator + 1u;
	}
}

ImU32 AssetBrowserItemColor(const NLS::Editor::Assets::AssetBrowserItemType type)
{
	const auto color = NLS::Editor::Assets::AssetBrowserItemTypeDisplayColor(type);
	return IM_COL32(color.red, color.green, color.blue, color.alpha);
}

size_t AssetBrowserUtf8CodepointLength(const unsigned char leadByte)
{
	if ((leadByte & 0x80u) == 0u)
		return 1u;
	if ((leadByte & 0xE0u) == 0xC0u)
		return 2u;
	if ((leadByte & 0xF0u) == 0xE0u)
		return 3u;
	if ((leadByte & 0xF8u) == 0xF0u)
		return 4u;
	return 1u;
}

void DrawAssetBrowserDisclosureButton(
	ImDrawList* drawList,
	const ImVec2& center,
	float radius,
	bool expanded,
	bool hovered,
	bool horizontalToggle = false);

void DrawAssetBrowserFilmstripPanel(
	ImDrawList* drawList,
	const ImVec2& min,
	const ImVec2& max,
	bool hovered,
	bool continuesLeft = false,
	bool continuesRight = false);

void DrawAssetBrowserSegmentPanel(
	ImDrawList* drawList,
	const ImVec2& min,
	const ImVec2& max,
	bool hovered,
	ImDrawFlags cornerFlags);

std::string EllipsizeAssetBrowserLabel(
	const std::string& text,
	const float maxWidth)
{
	NLS_PROFILE_NAMED_SCOPE("AssetBrowser::EllipsizeAssetBrowserLabel");
	if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
		return text;

	constexpr const char* ellipsis = "...";
	const float ellipsisWidth = ImGui::CalcTextSize(ellipsis).x;
	if (ellipsisWidth >= maxWidth)
		return ellipsis;

	std::vector<size_t> prefixEnds;
	prefixEnds.reserve(text.size() + 1u);
	prefixEnds.push_back(0u);
	for (size_t offset = 0u; offset < text.size();)
	{
		size_t length = AssetBrowserUtf8CodepointLength(static_cast<unsigned char>(text[offset]));
		if (offset + length > text.size())
			length = 1u;
		prefixEnds.push_back(offset + length);
		offset += length;
	}

	size_t low = 0u;
	size_t high = prefixEnds.size() - 1u;
	size_t best = 0u;
	while (low <= high)
	{
		const size_t mid = low + (high - low) / 2u;
		const char* begin = text.data();
		const char* end = begin + prefixEnds[mid];
		const float candidateWidth = ImGui::CalcTextSize(begin, end).x + ellipsisWidth;
		if (candidateWidth <= maxWidth)
		{
			best = mid;
			low = mid + 1u;
		}
		else
		{
			if (mid == 0u)
				break;
			high = mid - 1u;
		}
	}

	return text.substr(0u, prefixEnds[best]) + ellipsis;
}

constexpr size_t kMaxResidentAssetBrowserThumbnailTextures = 256u;
constexpr size_t kMaxAssetBrowserThumbnailTextureLoadsPerFrame = 4u;
constexpr size_t kMaxAssetBrowserThumbnailTextureDecodesInFlight = 4u;
constexpr size_t kMaxAssetBrowserThumbnailTextureUploadsPerFrame = 4u;
constexpr size_t kMaxAssetBrowserInteractiveThumbnailTextureUploadsPerFrame = 2u;
constexpr size_t kMaxAssetBrowserThumbnailTextureUploadPollsPerFrame = 128u;
constexpr size_t kMaxAssetBrowserInteractiveThumbnailTextureUploadPollsPerFrame = 32u;
constexpr uint64_t kAssetBrowserThumbnailTextureRetryDelayFrames = 30u;
constexpr uint64_t kMaxAssetBrowserThumbnailTextureUploadMicrosecondsPerFrame = 2000u;
constexpr size_t kMaxAssetBrowserThumbnailRequestsPerFrame = 8u;
constexpr size_t kMaxAssetBrowserInteractiveThumbnailRequestsPerFrame = 4u;
constexpr size_t kMaxAssetBrowserInteractiveThumbnailStartsPerFrame = 2u;
constexpr size_t kMaxAssetBrowserInteractiveCachedThumbnailTexturePumpsPerFrame = 4u;
constexpr uint64_t kMaxAssetBrowserThumbnailRequestMicrosecondsPerFrame = 2000u;
constexpr size_t kMaxAssetBrowserCompletedThumbnailConsumesPerPump = 8u;
constexpr uint32_t kMaxAssetBrowserCachedThumbnailTextureDimension = 512u;
constexpr uint32_t kAssetBrowserCompactGeneratedThumbnailCacheSize = 72u;
constexpr uint32_t kAssetBrowserDefaultGeneratedThumbnailCacheSize = 96u;
constexpr uint32_t kAssetBrowserLargeGeneratedThumbnailCacheSize = 128u;
constexpr uint32_t kAssetBrowserGeneratedThumbnailCacheSize = 160u;
constexpr double kAssetBrowserGpuThumbnailIntervalSeconds = 0.08;
constexpr double kAssetBrowserHeavyGpuThumbnailIntervalSeconds = 0.25;
// Resource readiness checks can synchronously inspect renderer dependencies.
// Resource continuations are already limited to one scheduler slot per frame;
// keep their retry cadence short so a large prefab can advance through many
// small dependency stages without waiting behind the normal heavy-preview gate.
constexpr double kAssetBrowserHeavyGpuThumbnailResourcePendingIntervalSeconds = 0.05;
constexpr double kAssetBrowserHeavyGpuThumbnailIdleDelaySeconds = 0.25;
constexpr double kAssetBrowserScrollIdleDelaySeconds = 0.75;
constexpr double kAssetBrowserRefreshDebounceSeconds = 0.20;
constexpr double kAssetBrowserInlineRenamePendingMaxSeconds = 1.0;
constexpr size_t kAssetBrowserPrefabHotCachePreloadGateCapacity = 256u;
constexpr auto kAssetBrowserPrefabHotCachePreloadGateTtl = std::chrono::seconds(3);
constexpr double kAssetBrowserHoveredPrefabHotCachePreloadRepeatDelaySeconds =
	static_cast<double>(kAssetBrowserPrefabHotCachePreloadGateTtl.count());
constexpr auto kAssetBrowserThumbnailUiDrawTelemetryMinimum = std::chrono::microseconds(1000);
constexpr ImVec2 kAssetBrowserImageUv0(0.0f, 1.0f);
constexpr ImVec2 kAssetBrowserImageUv1(1.0f, 0.0f);
constexpr ImVec2 kAssetBrowserThumbnailUv0 = kAssetBrowserImageUv0;
constexpr ImVec2 kAssetBrowserThumbnailUv1 = kAssetBrowserImageUv1;

struct AssetBrowserArtifactTelemetryScope
{
	NLS::Core::Assets::ArtifactLoadTelemetryStage stage;
	std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
	const std::string* path = nullptr;
	const size_t* byteCount = nullptr;
	std::chrono::microseconds minimumElapsed {};

	~AssetBrowserArtifactTelemetryScope()
	{
		const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - begin);
		if (elapsed < minimumElapsed)
			return;

		NLS::Core::Assets::RecordArtifactLoadTelemetry({
			stage,
			elapsed,
			byteCount != nullptr ? *byteCount : 0u,
			path != nullptr ? *path : std::string {}
		});
	}
};

struct AssetBrowserArtifactTelemetryAccumulatorScope
{
	std::chrono::microseconds& elapsed;
	std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

	~AssetBrowserArtifactTelemetryAccumulatorScope()
	{
		elapsed += std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - begin);
	}
};

void RecordAssetBrowserArtifactTelemetryStage(
	const NLS::Core::Assets::ArtifactLoadTelemetryStage stage,
	const std::chrono::microseconds elapsed,
	const std::string& path,
	const size_t itemCount,
	const std::chrono::microseconds minimumElapsed)
{
	if (elapsed < minimumElapsed)
		return;

	NLS::Core::Assets::RecordArtifactLoadTelemetry({
		stage,
		elapsed,
		itemCount,
		path
	});
}

void RecordAssetBrowserThumbnailResultTelemetry(
    const NLS::Editor::Assets::AssetThumbnailServiceResult& result,
    const NLS::Core::Assets::ArtifactLoadTelemetryStage stage =
        NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpConsumeCompleted)
{
    if (!NLS::Core::Assets::IsArtifactLoadTelemetryEnabled())
        return;

    std::string path = "thumbnail-result|source=" + result.sourceAssetPath;
    path += "|sub=" + result.subAssetKey;
    path += "|artifact=" + result.artifactPath;
    path += "|kind=" + std::to_string(static_cast<unsigned int>(result.requestKind));
    path += "|status=" + std::to_string(static_cast<unsigned int>(result.status));
    path += "|presentation=" +
        std::to_string(static_cast<unsigned int>(result.presentationState));
    path += "|quality=" +
        std::to_string(static_cast<unsigned int>(result.previewQuality));
    path += "|refresh=" + std::to_string(result.refreshPending ? 1u : 0u);
    path += "|retained=" + std::to_string(result.retainedImage.has_value() ? 1u : 0u);
    path += "|gpu=" + std::to_string(result.gpuTexture.IsValid() ? 1u : 0u);
    if (!result.diagnostic.empty())
        path += "|diagnostic=" + result.diagnostic;

    NLS::Core::Assets::RecordArtifactLoadTelemetry({
        stage,
        {},
        result.imagePath.empty() ? 0u : 1u,
        std::move(path)
    });
}

NLS::Editor::Assets::AssetThumbnailRequestBuildContext MakeAssetBrowserThumbnailRequestBuildContext(
	const NLS::Editor::Assets::AssetThumbnailFeatureConfig& featureConfig = {})
{
	NLS::Editor::Assets::AssetThumbnailRequestBuildContext context;
	context.deferManifestLookups = true;
	context.featureConfig = featureConfig;
	return context;
}

bool ShouldBypassAssetBrowserThumbnailService(
	const NLS::Editor::Assets::AssetThumbnailKind kind)
{
	(void)kind;
	return false;
}

std::mutex& AssetBrowserPrefabHotCachePreloadMutex()
{
	static std::mutex mutex;
	return mutex;
}

NLS::Editor::Core::RecentBackgroundWorkGate& AssetBrowserPrefabHotCachePreloadGate()
{
	static NLS::Editor::Core::RecentBackgroundWorkGate gate(
		kAssetBrowserPrefabHotCachePreloadGateCapacity,
		kAssetBrowserPrefabHotCachePreloadGateTtl);
	return gate;
}

uint32_t AssetBrowserThumbnailRequestSize(const float thumbnailSize)
{
	if (thumbnailSize <= 84.0f)
		return kAssetBrowserCompactGeneratedThumbnailCacheSize;
	if (thumbnailSize <= 112.0f)
		return kAssetBrowserDefaultGeneratedThumbnailCacheSize;
	if (thumbnailSize <= 144.0f)
		return kAssetBrowserLargeGeneratedThumbnailCacheSize;
	return kAssetBrowserGeneratedThumbnailCacheSize;
}

uint64_t HashAssetBrowserItemFingerprint(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	uint64_t hash = 1469598103934665603ull;
	auto mixByte = [&hash](unsigned char value)
	{
		hash ^= static_cast<uint64_t>(value);
		hash *= 1099511628211ull;
	};
	auto mixString = [&mixByte](const std::string& text)
	{
		for (const unsigned char ch : text)
			mixByte(ch);
		mixByte(0u);
	};
	auto mixUInt64 = [&mixByte](uint64_t value)
	{
		for (size_t index = 0u; index < sizeof(uint64_t); ++index)
			mixByte(static_cast<unsigned char>((value >> (index * 8u)) & 0xffu));
	};

	mixString(item.projectRelativePath);
	mixString(item.sourceAssetPath);
	mixString(item.subAssetKey);
	mixString(item.dragResourcePath);
	mixString(item.selectionResourcePath);
	mixUInt64(static_cast<uint64_t>(item.kind));
	mixUInt64(static_cast<uint64_t>(item.type));
	mixUInt64(static_cast<uint64_t>(item.artifactType));
	for (const auto byte : item.assetId.GetGuid().GetBytes())
		mixByte(byte);
	mixUInt64(item.generatedReadOnly ? 1ull : 0ull);
	mixUInt64(item.previewableInAssetView ? 1ull : 0ull);
	mixUInt64(item.hasGeneratedSubAssets ? 1ull : 0ull);
	return hash;
}

uint64_t HashVisibleThumbnailItems(
	const std::vector<NLS::Editor::Assets::AssetBrowserItem>& items,
	const uint32_t requestedSize,
	const std::string& selectedFolder)
{
	uint64_t hash = 1469598103934665603ull;
	auto mixByte = [&hash](unsigned char value)
	{
		hash ^= static_cast<uint64_t>(value);
		hash *= 1099511628211ull;
	};
	auto mixUInt64 = [&mixByte](uint64_t value)
	{
		for (size_t index = 0u; index < sizeof(uint64_t); ++index)
			mixByte(static_cast<unsigned char>((value >> (index * 8u)) & 0xffu));
	};
	auto mixString = [&mixByte](const std::string& text)
	{
		for (const unsigned char ch : text)
			mixByte(ch);
		mixByte(0u);
	};

	mixString(NormalizeProjectBrowserPath(selectedFolder));
	mixUInt64(requestedSize);
	mixUInt64(static_cast<uint64_t>(items.size()));
	for (const auto& item : items)
		mixUInt64(HashAssetBrowserItemFingerprint(item));
	return hash;
}

std::string LowerAscii(std::string text)
{
	std::transform(
		text.begin(),
		text.end(),
		text.begin(),
		[](const unsigned char ch)
		{
			return static_cast<char>(std::tolower(ch));
		});
	return text;
}

std::string TrimAscii(const std::string& text)
{
	size_t begin = 0u;
	while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0)
		++begin;
	size_t end = text.size();
	while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1u])) != 0)
		--end;
	return text.substr(begin, end - begin);
}

bool MatchesProjectAssetDisplayFilter(
	const NLS::Editor::Assets::AssetBrowserItem& item,
	const NLS::Editor::Assets::AssetBrowserItemType typeFilter,
	const std::string& normalizedQuery)
{
	if (typeFilter != NLS::Editor::Assets::AssetBrowserItemType::All &&
		item.type != typeFilter)
	{
		return false;
	}
	if (normalizedQuery.empty())
		return true;
	return LowerAscii(item.displayName).find(normalizedQuery) != std::string::npos;
}

std::string EnsureTrailingPathSeparator(std::filesystem::path path)
{
	auto text = path.lexically_normal().string();
	if (!text.empty() && text.back() != '\\' && text.back() != '/')
		text += Utils::PathParser::Separator();
	return text;
}

std::string ProjectBrowserLegacyResourcePath(std::string projectRelativePath)
{
	projectRelativePath = NormalizeProjectBrowserPath(std::move(projectRelativePath));
	const std::string assetsPrefix = "Assets/";
	if (projectRelativePath == "Assets")
		return {};
	if (projectRelativePath.compare(0u, assetsPrefix.size(), assetsPrefix) == 0)
		return projectRelativePath.substr(assetsPrefix.size());
	return projectRelativePath;
}

std::string ProjectBrowserResourcePathForItem(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	if (!item.dragResourcePath.empty())
		return ProjectBrowserLegacyResourcePath(item.dragResourcePath);
	const auto& sourcePath = item.sourceAssetPath.empty()
		? item.projectRelativePath
		: item.sourceAssetPath;
	return ProjectBrowserLegacyResourcePath(sourcePath);
}

std::string ProjectBrowserSelectionPathForItem(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	const auto& sourcePath = item.sourceAssetPath.empty()
		? item.projectRelativePath
		: item.sourceAssetPath;
	auto legacyPath = ProjectBrowserLegacyResourcePath(sourcePath);
	if (item.kind == NLS::Editor::Assets::AssetBrowserItemKind::GeneratedSubAsset &&
		!item.subAssetKey.empty())
	{
		legacyPath += "#";
		legacyPath += item.subAssetKey;
	}
	return legacyPath;
}

std::filesystem::path ProjectBrowserAbsolutePathForResourcePath(
	const std::string& projectAssetsFolder,
	std::string resourcePath)
{
	if (resourcePath.empty())
		return {};
	if (resourcePath.front() == ':')
		return std::filesystem::path(EDITOR_EXEC(GetRealPath(resourcePath))).lexically_normal();
	if (std::filesystem::path(resourcePath).is_absolute())
		return std::filesystem::path(resourcePath).lexically_normal();

	resourcePath = NormalizeProjectBrowserPath(std::move(resourcePath));
	const auto projectRoot = ProjectRootFromAssetsFolder(projectAssetsFolder);
	if (resourcePath == "Assets" || resourcePath.compare(0u, 7u, "Assets/") == 0)
		return (projectRoot / resourcePath).lexically_normal();
	return std::filesystem::path(EDITOR_EXEC(GetRealPath(resourcePath))).lexically_normal();
}

std::string SanitizeAssetBrowserName(std::string value)
{
	value.erase(std::remove_if(value.begin(), value.end(), [](const auto& c)
	{
		return std::find(FILENAMES_CHARS.begin(), FILENAMES_CHARS.end(), c) == FILENAMES_CHARS.end();
	}), value.end());
	return value;
}

std::filesystem::path BuildUniqueAssetPath(
	const std::filesystem::path& folder,
	const std::string& requestedName,
	const std::string& extension)
{
	size_t suffix = 0u;
	for (;;)
	{
		const auto name = suffix == 0u
			? requestedName
			: requestedName + " (" + std::to_string(suffix) + ")";
		auto candidate = folder / (name + extension);
		if (!std::filesystem::exists(candidate))
			return candidate.lexically_normal();
		++suffix;
	}
}

bool CreateNativeMaterialAssetAtPath(
	const std::string& projectAssetsFolder,
	const std::filesystem::path& absolutePath,
	const std::string& payload)
{
	const auto normalizedPath = absolutePath.lexically_normal();
	const auto projectRoot = ProjectRootFromAssetsFolder(projectAssetsFolder);
	const auto projectRelativePath = EditorAssetPathFromAbsolutePath(projectAssetsFolder, normalizedPath.string());
	if (projectRoot.empty() || projectRelativePath.empty())
		return false;

	NLS::Editor::Assets::AssetDatabaseFacade database(
		NLS::Editor::Assets::MakeProjectEditorAssetRoots(projectRoot));
	if (!database.Refresh())
		return false;

	NLS::Editor::Assets::AssetObjectRecord material;
	material.name = normalizedPath.stem().generic_string();
	material.artifactType = NLS::Core::Assets::ArtifactType::Material;
	material.loaderId = "material";
	material.serializedPayload = payload;
	return database.CreateAsset(material, projectRelativePath.generic_string());
}

std::string BuildDefaultAssetBrowserMaterialPayload()
{
	return NLS::Editor::Assets::BuildDefaultShaderLabMaterialPayload();
}

bool IsPathInsideOrEqual(
	const std::filesystem::path& candidate,
	const std::filesystem::path& root)
{
	const auto normalizedCandidate = candidate.lexically_normal();
	const auto normalizedRoot = root.lexically_normal();
	if (normalizedCandidate == normalizedRoot)
		return true;
	const auto relative = normalizedCandidate.lexically_relative(normalizedRoot);
	if (relative.empty() || relative.is_absolute())
		return false;
	for (const auto& part : relative)
	{
		if (part == "..")
			return false;
	}
	return true;
}

std::vector<std::filesystem::path> ParseClipboardPathText(const std::string& text)
{
	std::vector<std::filesystem::path> paths;
	std::istringstream stream(text);
	std::string line;
	while (std::getline(stream, line))
	{
		line = TrimAscii(line);
		if (line.empty())
			continue;
		if ((line.front() == '"' && line.back() == '"') || (line.front() == '\'' && line.back() == '\''))
			line = line.substr(1u, line.size() - 2u);
		paths.emplace_back(line);
	}
	return paths;
}

#ifdef _WIN32
std::vector<std::filesystem::path> ReadWindowsClipboardFilePaths()
{
	std::vector<std::filesystem::path> paths;
	if (!OpenClipboard(nullptr))
		return paths;

	const HANDLE fileDropHandle = GetClipboardData(CF_HDROP);
	if (fileDropHandle != nullptr)
	{
		const auto dropHandle = static_cast<HDROP>(fileDropHandle);
		if (dropHandle != nullptr)
		{
			const UINT count = DragQueryFileW(dropHandle, 0xFFFFFFFF, nullptr, 0);
			paths.reserve(count);
			for (UINT index = 0; index < count; ++index)
			{
				const UINT length = DragQueryFileW(dropHandle, index, nullptr, 0);
				if (length == 0)
					continue;
				std::wstring buffer(length + 1u, L'\0');
				DragQueryFileW(dropHandle, index, buffer.data(), static_cast<UINT>(buffer.size()));
				buffer.resize(length);
				paths.emplace_back(buffer);
			}
		}
	}

	CloseClipboard();
	return paths;
}
#endif

bool CopyAssetFileWithMeta(
	const std::filesystem::path& source,
	const std::filesystem::path& destination)
{
	std::error_code error;
	std::filesystem::create_directories(destination.parent_path(), error);
	if (error)
		return false;

	std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, error);
	if (error)
		return false;

	const auto sourceMeta = source.string() + ".meta";
	if (std::filesystem::exists(sourceMeta))
	{
		const auto destinationMeta = destination.string() + ".meta";
		auto meta = NLS::Core::Assets::AssetMeta::Load(sourceMeta)
			.value_or(NLS::Core::Assets::AssetMeta::CreateForAsset(destination));
		meta.id = NLS::Core::Assets::AssetId::New();
		meta.assetType = NLS::Core::Assets::InferAssetType(destination);
		meta.importerId = NLS::Core::Assets::InferImporterId(meta.assetType);
		if (!meta.Save(destinationMeta))
			return false;
	}
	return true;
}

bool CopyAssetFolderRecursively(
	const std::filesystem::path& source,
	const std::filesystem::path& destination)
{
	std::error_code error;
	std::filesystem::create_directories(destination, error);
	if (error)
		return false;

	for (const auto& entry : std::filesystem::recursive_directory_iterator(
			 source,
			 std::filesystem::directory_options::skip_permission_denied,
			 error))
	{
		if (error)
			return false;

		const auto relative = entry.path().lexically_relative(source);
		if (relative.empty())
			continue;

		if (entry.is_directory())
		{
			std::filesystem::create_directories(destination / relative, error);
			if (error)
				return false;
			continue;
		}

		if (entry.is_regular_file())
		{
			if (entry.path().extension() == ".meta")
				continue;
			if (!CopyAssetFileWithMeta(entry.path(), destination / relative))
				return false;
		}
	}
	return true;
}

bool CopyAssetFolderRecursivelyWithoutMeta(
	const std::filesystem::path& source,
	const std::filesystem::path& destination)
{
	std::error_code error;
	std::filesystem::create_directories(destination, error);
	if (error)
		return false;

	for (const auto& entry : std::filesystem::recursive_directory_iterator(
			 source,
			 std::filesystem::directory_options::skip_permission_denied,
			 error))
	{
		if (error)
			return false;

		const auto relative = entry.path().lexically_relative(source);
		if (relative.empty())
			continue;

		if (entry.is_directory())
		{
			std::filesystem::create_directories(destination / relative, error);
			if (error)
				return false;
			continue;
		}

		if (entry.is_regular_file())
		{
			if (entry.path().extension() == ".meta")
				continue;

			const auto target = destination / relative;
			std::filesystem::create_directories(target.parent_path(), error);
			if (error)
				return false;
			std::filesystem::copy_file(
				entry.path(),
				target,
				std::filesystem::copy_options::overwrite_existing,
				error);
			if (error)
				return false;
		}
	}
	return true;
}

std::string AssetBrowserFileStamp(const std::filesystem::path& path)
{
	std::error_code error;
	const auto size = std::filesystem::file_size(path, error);
	if (error)
		return {};

	error.clear();
	const auto writeTime = std::filesystem::last_write_time(path, error);
	if (error)
		return {};

	const auto writeTimeTicks = static_cast<std::intmax_t>(writeTime.time_since_epoch().count());
	return std::to_string(size) + ":" + std::to_string(writeTimeTicks);
}

bool ManifestDependencyStampsAreCurrent(
	const NLS::Core::Assets::ArtifactManifest& manifest,
	const std::string& projectAssetsFolder,
	const std::string& absolutePath)
{
	const auto meta = NLS::Core::Assets::AssetMeta::Load(
		NLS::Core::Assets::GetAssetMetaPath(absolutePath));
	if (!meta.has_value() ||
		manifest.importerId != meta->importerId ||
		manifest.importerVersion != meta->importerVersion ||
		manifest.targetPlatform != "editor")
	{
		return false;
	}

	if (manifest.dependencies.empty())
		return false;

	const auto assetPath = NLS::Editor::Assets::NormalizeEditorAssetPath(
		EditorAssetPathFromAbsolutePath(projectAssetsFolder, absolutePath));
	const auto metaAbsolutePath = NLS::Core::Assets::GetAssetMetaPath(absolutePath);
	const auto metaPath = NLS::Editor::Assets::NormalizeEditorAssetPath(
		EditorAssetPathFromAbsolutePath(projectAssetsFolder, metaAbsolutePath.string()));
	const auto projectRoot = ProjectRootFromAssetsFolder(projectAssetsFolder);

	bool checkedAsset = false;
	bool checkedMeta = false;
	for (const auto& dependency : manifest.dependencies)
	{
		const auto value = NLS::Editor::Assets::NormalizeEditorAssetPath(dependency.value);
		if (dependency.kind == NLS::Core::Assets::AssetDependencyKind::SourceFileHash)
		{
			if (value == assetPath)
				checkedAsset = true;

			const auto dependencyPath = NLS::Editor::Assets::ResolveEditorManifestDependencyPath(projectRoot, value);
			if (!dependencyPath.has_value() || dependency.hashOrVersion != AssetBrowserFileStamp(*dependencyPath))
				return false;
			continue;
		}
		if (dependency.kind == NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping)
		{
			if (value == metaPath)
				checkedMeta = true;

			const auto dependencyPath = NLS::Editor::Assets::ResolveEditorManifestDependencyPath(projectRoot, value);
			if (!dependencyPath.has_value() || dependency.hashOrVersion != AssetBrowserFileStamp(*dependencyPath))
				return false;
			continue;
		}
	}

	return checkedAsset && checkedMeta;
}

std::filesystem::path ResolveArtifactPathForManifest(
	const std::filesystem::path& projectRoot,
	const NLS::Core::Assets::ImportedArtifact& subAsset)
{
	if (subAsset.artifactPath.empty())
		return {};
	if (!NLS::Core::Assets::IsContentStorageArtifactPath(subAsset.artifactPath))
		return {};

	const auto artifactPath = std::filesystem::path(subAsset.artifactPath);
	std::vector<std::filesystem::path> candidates;
	if (artifactPath.is_absolute())
	{
		candidates.push_back(artifactPath.lexically_normal());

		const auto artifactsRoot = projectRoot / "Library" / "Artifacts";
		std::vector<std::filesystem::path> parts;
		for (const auto& part : artifactPath.lexically_normal())
			parts.push_back(part);
		for (size_t index = 0u; index + 2u < parts.size(); ++index)
		{
			if (parts[index].generic_string() != "Artifacts")
				continue;

			std::filesystem::path remapped = artifactsRoot / parts[index + 1u];
			for (size_t relativeIndex = index + 2u; relativeIndex < parts.size(); ++relativeIndex)
				remapped /= parts[relativeIndex];
			remapped = remapped.lexically_normal();
			if (std::find(candidates.begin(), candidates.end(), remapped) == candidates.end())
				candidates.push_back(remapped);
		}
	}
	else
	{
		candidates.push_back((projectRoot / artifactPath).lexically_normal());
	}

	for (const auto& resolvedPath : candidates)
	{
		const auto relative = resolvedPath.lexically_relative(projectRoot.lexically_normal());
		if (relative.empty() || relative.is_absolute())
			continue;

		bool escapesProject = false;
		for (const auto& part : relative)
		{
			if (part == "..")
			{
				escapesProject = true;
				break;
			}
		}
		if (!escapesProject)
			return resolvedPath;
	}

	return {};
}

std::optional<std::string> SelectManifestPrefabSubAssetKeyForDragPayload(
	const std::filesystem::path& projectRoot,
	const NLS::Core::Assets::ArtifactManifest& manifest)
{
	if (manifest.subAssets.empty())
		return std::nullopt;

	auto isUsablePrefabSubAsset = [&](const NLS::Core::Assets::ImportedArtifact& subAsset, const std::string* expectedKey)
	{
		if (subAsset.subAssetKey.empty())
			return false;
		if (expectedKey != nullptr && subAsset.subAssetKey != *expectedKey)
			return false;

		if (subAsset.artifactType != NLS::Core::Assets::ArtifactType::Prefab)
			return false;

		const auto resolvedArtifactPath = ResolveArtifactPathForManifest(projectRoot, subAsset);
		return !resolvedArtifactPath.empty() && std::filesystem::is_regular_file(resolvedArtifactPath);
	};

	if (!manifest.primarySubAssetKey.empty())
	{
		for (const auto& subAsset : manifest.subAssets)
		{
			if (isUsablePrefabSubAsset(subAsset, &manifest.primarySubAssetKey))
				return manifest.primarySubAssetKey;
		}
	}

	for (const auto& subAsset : manifest.subAssets)
	{
		if (!isUsablePrefabSubAsset(subAsset, nullptr))
			continue;

		if (!subAsset.subAssetKey.empty())
			return subAsset.subAssetKey;
	}

	return std::nullopt;
}

void ReimportProjectAssetAsync(const std::string& projectAssetsFolder, const std::string& absolutePath)
{
	const auto projectRoot = ProjectRootFromAssetsFolder(projectAssetsFolder);
	const auto assetPath = EditorAssetPathFromAbsolutePath(projectAssetsFolder, absolutePath);
	if (projectRoot.empty() || assetPath.empty())
	{
		NLS_LOG_ERROR("Failed to resolve project asset path for reimport: " + absolutePath);
		return;
	}

	auto& tracker = EDITOR_CONTEXT(importProgressTracker);
	auto residentPrefabPreviewRegistry = EDITOR_CONTEXT(residentPrefabPreviewRegistry);
	const auto queued = EDITOR_EXEC(TrackBackgroundTask([
		projectRoot,
		assetPath = assetPath.generic_string(),
		residentPrefabPreviewRegistry = std::move(residentPrefabPreviewRegistry),
		&tracker]
	{
		NLS::Editor::Assets::AssetImporterFacade importer(
			NLS::Editor::Assets::MakeProjectEditorAssetRoots(projectRoot));
		importer.SetResidentPrefabPreviewRegistry(residentPrefabPreviewRegistry);
		const auto imported = importer.SaveAndReimport(assetPath, tracker);
		EDITOR_EXEC(DelayAction([assetPath, imported]
		{
			EDITOR_PANEL(NLS::Editor::Panels::AssetBrowser, "Asset Browser").Refresh();
			EDITOR_PANEL(NLS::Editor::Panels::AssetProperties, "Asset Properties").Refresh();
			if (imported)
				NLS_LOG_INFO("Reimported asset: " + assetPath);
			else
				NLS_LOG_ERROR("Failed to reimport asset: " + assetPath);
		}));
	}));
	if (!queued)
		NLS_LOG_ERROR("Failed to queue asset reimport because the editor background task queue is at capacity: " + assetPath.generic_string());
}

const char* AssetPreimportReasonLabel(const NLS::Editor::Assets::AssetPreimportReason reason)
{
	using NLS::Editor::Assets::AssetPreimportReason;
	switch (reason)
	{
	case AssetPreimportReason::EditorStartup:
		return "editor startup";
	case AssetPreimportReason::FileWatcherChanged:
		return "file watcher change";
	case AssetPreimportReason::AssetCopiedOrMoved:
		return "asset copy or move";
	default:
		return "asset preimport";
	}
}

const char* AssetDiagnosticSeverityLabel(const NLS::Core::Assets::AssetDiagnosticSeverity severity)
{
	using NLS::Core::Assets::AssetDiagnosticSeverity;
	switch (severity)
	{
	case AssetDiagnosticSeverity::Error:
		return "error";
	case AssetDiagnosticSeverity::Warning:
		return "warning";
	case AssetDiagnosticSeverity::Info:
	default:
		return "info";
	}
}

const char* ImportJobTerminalStatusLabel(const NLS::Editor::Assets::ImportJobTerminalStatus status)
{
	using NLS::Editor::Assets::ImportJobTerminalStatus;
	switch (status)
	{
	case ImportJobTerminalStatus::Succeeded:
		return "succeeded";
	case ImportJobTerminalStatus::Failed:
		return "failed";
	case ImportJobTerminalStatus::Cancelled:
		return "cancelled";
	case ImportJobTerminalStatus::None:
	default:
		return "running";
	}
}

void AppendUniqueDiagnostics(
	NLS::Core::Assets::AssetDiagnostics& diagnostics,
	const NLS::Core::Assets::AssetDiagnostics& incoming)
{
	for (const auto& diagnostic : incoming)
	{
		const auto duplicate = std::find_if(
			diagnostics.begin(),
			diagnostics.end(),
			[&diagnostic](const NLS::Core::Assets::AssetDiagnostic& existing)
			{
				return existing.severity == diagnostic.severity &&
					existing.code == diagnostic.code &&
					existing.path == diagnostic.path &&
					existing.message == diagnostic.message;
			});
		if (duplicate == diagnostics.end())
			diagnostics.push_back(diagnostic);
	}
}

void LogAssetPreimportFailureDetails(
	const NLS::Editor::Assets::AssetPreimportReason reason,
	const std::vector<std::filesystem::path>& changedPaths,
	const std::vector<NLS::Editor::Assets::ImportProgressEvent>& events,
	const NLS::Core::Assets::AssetDiagnostics& diagnostics)
{
	NLS_LOG_ERROR(std::string("Asset preimport failed after ") + AssetPreimportReasonLabel(reason));
	for (const auto& changedPath : changedPaths)
		NLS_LOG_ERROR("  changed path: " + changedPath.generic_string());

	for (const auto& event : events)
	{
		if (event.terminalStatus == NLS::Editor::Assets::ImportJobTerminalStatus::None)
			continue;

		NLS_LOG_ERROR(
			"  job " +
			std::to_string(event.jobId.value) +
			" " +
			ImportJobTerminalStatusLabel(event.terminalStatus) +
			": " +
			event.sourcePath +
			" - " +
			event.message);
	}

	for (const auto& diagnostic : diagnostics)
	{
		NLS_LOG_ERROR(
			std::string("  diagnostic [") +
			AssetDiagnosticSeverityLabel(diagnostic.severity) +
			"] " +
			diagnostic.code +
			" path=" +
			diagnostic.path.generic_string() +
			" message=" +
			diagnostic.message);
	}
}

NLS::Editor::Assets::AssetPreimportReason MergeAssetPreimportReasons(
	const NLS::Editor::Assets::AssetPreimportReason current,
	const NLS::Editor::Assets::AssetPreimportReason incoming)
{
	using NLS::Editor::Assets::AssetPreimportReason;
	if (current == AssetPreimportReason::FileWatcherChanged ||
		incoming == AssetPreimportReason::FileWatcherChanged)
	{
		return AssetPreimportReason::FileWatcherChanged;
	}
	if (current == AssetPreimportReason::AssetCopiedOrMoved ||
		incoming == AssetPreimportReason::AssetCopiedOrMoved)
	{
		return AssetPreimportReason::AssetCopiedOrMoved;
	}
	return AssetPreimportReason::EditorStartup;
}

NLS::Editor::Assets::AssetPreimportRequest MergeAssetPreimportRequests(
	NLS::Editor::Assets::AssetPreimportRequest current,
	const NLS::Editor::Assets::AssetPreimportRequest& incoming)
{
	current.reason = MergeAssetPreimportReasons(current.reason, incoming.reason);
	current.changedPaths.insert(
		current.changedPaths.end(),
		incoming.changedPaths.begin(),
		incoming.changedPaths.end());
	for (auto& path : current.changedPaths)
		path = path.lexically_normal();
	std::sort(current.changedPaths.begin(), current.changedPaths.end());
	current.changedPaths.erase(
		std::unique(current.changedPaths.begin(), current.changedPaths.end()),
		current.changedPaths.end());
	return current;
}

std::optional<NLS::Editor::Assets::EditorAssetDragPayload> BuildEditorAssetDragPayloadForFile(
	const std::string& projectAssetsFolder,
	const std::string& absolutePath,
	const std::string& resourceFormatPath,
	Utils::PathParser::EFileType fileType)
{
	using namespace NLS::Editor::Assets;

	if (fileType != Utils::PathParser::EFileType::MODEL &&
		fileType != Utils::PathParser::EFileType::PREFAB &&
		fileType != Utils::PathParser::EFileType::MATERIAL &&
		fileType != Utils::PathParser::EFileType::TEXTURE &&
		fileType != Utils::PathParser::EFileType::SHADER &&
		fileType != Utils::PathParser::EFileType::SCRIPT)
	{
		return std::nullopt;
	}
	if (fileType == Utils::PathParser::EFileType::SCRIPT &&
		!IsScriptAssetPath(absolutePath))
	{
		return std::nullopt;
	}

	const auto meta = NLS::Core::Assets::AssetMeta::Load(
		NLS::Core::Assets::GetAssetMetaPath(absolutePath));
	if (!meta.has_value() || !meta->id.IsValid())
		return std::nullopt;

	NLS::Core::Assets::ArtifactType artifactType = NLS::Core::Assets::ArtifactType::Unknown;
	std::string subAssetKey;
	bool imported = false;

	if (fileType == Utils::PathParser::EFileType::MODEL &&
		subAssetKey.empty())
	{
		subAssetKey = "prefab:" + std::filesystem::path(resourceFormatPath).stem().generic_string();
		artifactType = NLS::Core::Assets::ArtifactType::Prefab;
	}

	if (fileType == Utils::PathParser::EFileType::PREFAB &&
		subAssetKey.empty())
	{
		subAssetKey = "prefab:" + std::filesystem::path(resourceFormatPath).stem().generic_string();
		artifactType = NLS::Core::Assets::ArtifactType::Prefab;
	}

	if (fileType == Utils::PathParser::EFileType::MATERIAL &&
		subAssetKey.empty())
	{
		subAssetKey = "material:" + std::filesystem::path(resourceFormatPath).stem().generic_string();
		artifactType = NLS::Core::Assets::ArtifactType::Material;
	}

	if (fileType == Utils::PathParser::EFileType::TEXTURE &&
		subAssetKey.empty())
	{
		subAssetKey = "texture:" + std::filesystem::path(resourceFormatPath).stem().generic_string();
		artifactType = NLS::Core::Assets::ArtifactType::Texture;
	}

	if (fileType == Utils::PathParser::EFileType::SHADER &&
		subAssetKey.empty())
	{
		subAssetKey = "shader:" + std::filesystem::path(resourceFormatPath).stem().generic_string();
		artifactType = NLS::Core::Assets::ArtifactType::Shader;
	}

	if (fileType == Utils::PathParser::EFileType::SCRIPT && subAssetKey.empty())
		subAssetKey = "script:" + std::filesystem::path(resourceFormatPath).stem().generic_string();

	const auto projectRoot = ProjectRootFromAssetsFolder(projectAssetsFolder);
	const auto manifest = NLS::Editor::Assets::LoadArtifactManifestFromProjectArtifactDB(projectRoot, meta->id);
	if (manifest.has_value())
	{
		const auto currentManifest = ManifestDependencyStampsAreCurrent(
			*manifest,
			projectAssetsFolder,
			absolutePath);
		if (fileType == Utils::PathParser::EFileType::PREFAB ||
			fileType == Utils::PathParser::EFileType::MATERIAL ||
			fileType == Utils::PathParser::EFileType::TEXTURE ||
			fileType == Utils::PathParser::EFileType::SHADER)
		{
			if (!manifest->primarySubAssetKey.empty())
				subAssetKey = manifest->primarySubAssetKey;
		}
		else if (fileType == Utils::PathParser::EFileType::MODEL)
		{
			if (auto manifestPrefabKey = SelectManifestPrefabSubAssetKeyForDragPayload(projectRoot, *manifest);
				manifestPrefabKey.has_value())
			{
				subAssetKey = std::move(*manifestPrefabKey);
			}
		}

		for (const auto& subAsset : manifest->subAssets)
		{
			if (subAsset.subAssetKey != subAssetKey)
				continue;

			const auto resolvedArtifactPath = ResolveArtifactPathForManifest(projectRoot, subAsset);
			if (resolvedArtifactPath.empty() || !std::filesystem::is_regular_file(resolvedArtifactPath))
				continue;

			artifactType = subAsset.artifactType;
			imported = currentManifest && artifactType != NLS::Core::Assets::ArtifactType::Unknown;
			break;
		}
		}

	if (!CanStoreEditorAssetDragPayload(resourceFormatPath, meta->id, subAssetKey))
		return std::nullopt;

	const bool generatedModelPrefab =
		fileType == Utils::PathParser::EFileType::MODEL &&
		artifactType == NLS::Core::Assets::ArtifactType::Prefab;
	return MakeEditorAssetDragPayload(
		resourceFormatPath,
		meta->id,
		subAssetKey,
			artifactType,
			generatedModelPrefab,
			imported);
}

void RenameAsset(const std::string& p_prev, const std::string& p_new)
{
	std::filesystem::rename(p_prev, p_new);

	if (const std::string previousMetaPath = GetAssociatedMetaFile(p_prev); std::filesystem::exists(previousMetaPath))
	{
		if (const std::string newMetaPath = GetAssociatedMetaFile(p_new); !std::filesystem::exists(newMetaPath))
		{
			std::filesystem::rename(previousMetaPath, newMetaPath);
		}
		else
		{
			NLS_LOG_ERROR(newMetaPath + " is already existing, .meta creation failed");
		}
	}
}

void RemoveAsset(const std::string& p_toDelete)
{
	std::filesystem::remove(p_toDelete);

	if (const std::string metaPath = GetAssociatedMetaFile(p_toDelete); std::filesystem::exists(metaPath))
	{
		std::filesystem::remove(metaPath);
	}
}

Editor::Panels::AssetBrowser::AssetBrowser
(
	const std::string& p_title,
	bool p_opened,
	const UI::PanelWindowSettings& p_windowSettings,
	const std::string& p_engineAssetFolder,
	const std::string& p_projectAssetFolder,
	const std::string& p_editorAssetFolder,
	NLS::Editor::Assets::AssetThumbnailFeatureConfig thumbnailFeatureConfig
) :
	PanelWindow(p_title, p_opened, p_windowSettings),
	m_engineAssetFolder(p_engineAssetFolder),
	m_projectAssetFolder(p_projectAssetFolder),
	m_editorAssetFolder(p_editorAssetFolder),
	m_thumbnailFeatureConfig(std::move(thumbnailFeatureConfig)),
	m_thumbnailService(m_thumbnailFeatureConfig)
{
	m_thumbnailRenderScheduler.SetAdaptiveBudgetEnabled(
		m_thumbnailFeatureConfig.adaptiveBudget);
	if (NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::EditorActions>() &&
		EDITOR_CONTEXT(residentPrefabPreviewRegistry) != nullptr)
	{
		m_residentPrefabPreviewRegistry = EDITOR_CONTEXT(residentPrefabPreviewRegistry);
	}
	else
	{
		m_residentPrefabPreviewRegistry =
			NLS::Editor::Assets::ResidentPrefabPreviewRegistry::Create();
	}
	m_lastResidentPrefabThumbnailWakeRevision =
		m_residentPrefabPreviewRegistry->GetThumbnailWakeRevision();
	NLS::Editor::Assets::SetObjectReferencePickerAssetRoots(
		NLS::Editor::Assets::MakeProjectEditorAssetRoots(ProjectRootFromAssetsFolder(m_projectAssetFolder)));
	NLS::Editor::Assets::SetObjectReferencePickerEntriesProvider([this]()
	{
		return m_projectAssetDatabaseReady && m_projectAssetDatabaseSnapshot
			? NLS::Editor::Assets::BuildObjectReferencePickerEntries(*m_projectAssetDatabaseSnapshot)
			: std::vector<NLS::Editor::Assets::ObjectReferencePickerEntry> {};
	});

	if (!std::filesystem::exists(m_projectAssetFolder))
	{
		std::filesystem::create_directories(m_projectAssetFolder);

		NLS::Dialogs::MessageBox message
		(
			"Assets folder not found",
			"The \"Assets/\" folders hasn't been found in your project directory.\nIt has been automatically generated",
            NLS::Dialogs::MessageBox::EMessageType::WARNING,
            NLS::Dialogs::MessageBox::EButtonLayout::OK
		);
	}

	m_assetThumbnailPool = std::make_shared<NLS::Editor::Assets::AssetThumbnailPool>();
	m_assetThumbnailPool->SetTextureCallbacks(
		[](const std::shared_ptr<NLS::Render::RHI::RHITextureView>& textureView) -> void*
		{
			return NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>()
				? NLS_SERVICE(NLS::UI::UIManager).ResolveTextureId(textureView)
				: nullptr;
		},
		[](const std::shared_ptr<NLS::Render::RHI::RHITextureView>& textureView, const bool immediate)
		{
			if (!NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
				return;
			if (immediate)
				NLS_SERVICE(NLS::UI::UIManager).ReleaseTextureViewHandle(textureView);
			else
				NLS_SERVICE(NLS::UI::UIManager).RetireTextureViewHandle(textureView);
		});

	if (EDITOR_CONTEXT(window) != nullptr)
	{
		m_windowDroppedFilesListener = EDITOR_CONTEXT(window)->DroppedFilesEvent.AddListener(
			[this](std::vector<std::string> paths)
			{
				NLS::Editor::Assets::EnqueueAssetBrowserExternalDroppedFiles(
					m_pendingExternalDroppedFiles,
					std::move(paths));
			});
	}

	Fill();
}

Editor::Panels::AssetBrowser::~AssetBrowser()
{
	ShutdownThumbnailPipeline();
	NLS::Editor::Assets::CloseAssetBrowserAsyncRefresh(m_projectAssetDatabaseRefreshState);
	NLS::Editor::Assets::CloseAssetBrowserAsyncRefresh(m_currentFolderItemsRefreshState);
	NLS::Editor::Assets::CloseAssetBrowserAsyncRefresh(m_objectReferencePickerRefreshState);

	if (EDITOR_CONTEXT(window) != nullptr && m_windowDroppedFilesListener != 0u)
		EDITOR_CONTEXT(window)->DroppedFilesEvent.RemoveListener(m_windowDroppedFilesListener);

	if (m_projectAssetDatabaseRefresh.has_value())
		AbandonProjectAssetDatabaseRefreshFuture(m_projectAssetDatabaseRefresh->future);
	for (auto& refresh : m_retiredProjectAssetDatabaseRefreshes)
		AbandonProjectAssetDatabaseRefreshFuture(refresh.future);
	m_projectAssetDatabaseRefresh.reset();
	m_retiredProjectAssetDatabaseRefreshes.clear();
	RetireCurrentProjectAssetDatabase();

	if (m_currentFolderItemsRefresh.has_value())
		AbandonAssetBrowserFuture(m_currentFolderItemsRefresh->future);
	m_currentFolderItemsRefresh.reset();
	m_pendingCurrentFolderItemsRefresh.reset();
	NLS::Editor::Assets::CloseAssetBrowserLatestRequestCoordinator(m_currentFolderItemsRefreshCoordinator);

	if (m_projectFolderTreeRefresh.has_value())
		AbandonAssetBrowserFuture(m_projectFolderTreeRefresh->future);
	for (auto& refresh : m_retiredProjectFolderTreeRefreshes)
		AbandonAssetBrowserFuture(refresh.future);
	m_projectFolderTreeRefresh.reset();
	m_retiredProjectFolderTreeRefreshes.clear();

	if (m_objectReferencePickerRefresh.has_value())
		AbandonAssetBrowserFuture(m_objectReferencePickerRefresh->future);
	m_objectReferencePickerRefresh.reset();
	m_pendingObjectReferencePickerRefresh.reset();
	NLS::Editor::Assets::CloseAssetBrowserLatestRequestCoordinator(m_objectReferencePickerRefreshCoordinator);

	AbandonAssetBrowserFuture(m_watcherStartup);
	AbandonAssetBrowserFuture(m_standardPbrShaderPassPrewarm);

	NLS::Editor::Assets::SetObjectReferencePickerEntriesProvider({});
	NLS::Editor::Assets::SetObjectReferencePickerEntries({});
}

void Editor::Panels::AssetBrowser::Fill()
{
	RebuildProjectAssetPresentation(NLS::Editor::Assets::BuildAssetBrowserRefreshPlan(
		NLS::Editor::Assets::AssetBrowserRefreshReason::InitialBuild));
}

void Editor::Panels::AssetBrowser::Clear()
{
	ReleaseAssetBrowserTextureHandleCache(false);
	DestroyCachedThumbnailTextures(false);
	m_thumbnailResultsByItemKey.clear();
	m_thumbnailItemKeyByCacheKey.clear();
	m_thumbnailItemKeyByPresentationKey.clear();
	m_assetThumbnailsByCacheKey.clear();
	m_lastThumbnailRequestSize = 0u;
	m_lastThumbnailGenerationScopeKey.clear();
	m_lastThumbnailGenerationScopeInteractive = false;
	m_thumbnailGenerationScopeDirty = true;
	m_pendingThumbnailScopeItems.clear();
	m_pendingThumbnailScopeOffset = 0u;
	m_pendingThumbnailRequestContext = MakeAssetBrowserThumbnailRequestBuildContext(m_thumbnailFeatureConfig);
	m_pendingThumbnailRequestContext.residentPrefabPreviewRegistry = m_residentPrefabPreviewRegistry;
	m_thumbnailScopeBuildInProgress = false;
	m_thumbnailService.ClearQueuedRequests();
}

void Editor::Panels::AssetBrowser::Refresh()
{
	RefreshPreservingExpandedFolders();
}

void Editor::Panels::AssetBrowser::OnBeforeDrawWidgets()
{
	NLS_PROFILE_NAMED_SCOPE("AssetBrowser::OnBeforeDrawWidgets");
	EnsureThumbnailTextureDeviceIdentity();
	if (m_residentPrefabPreviewRegistry != nullptr)
	{
		const auto wakeRevision =
			m_residentPrefabPreviewRegistry->GetThumbnailWakeRevision();
		if (wakeRevision != m_lastResidentPrefabThumbnailWakeRevision)
		{
			m_lastResidentPrefabThumbnailWakeRevision = wakeRevision;
			// A completed import or live scene attachment is a new admission
			// opportunity for every outstanding continuation. The service
			// coalesces active work, while terminal/fallback attempts must be
			// allowed to acquire the newly resident snapshot.
			m_importedPrefabThumbnailContinuationSubmittedRevisions.clear();
			m_thumbnailGenerationScopeDirty = true;
		}
	}
	const double now = ImGui::GetTime();
	const bool deleteActionInputsReleased =
		!ImGui::IsKeyDown(ImGuiKey_Delete) &&
		!ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
		!ImGui::IsMouseDown(ImGuiMouseButton_Right);
	if (m_projectDeleteActionAwaitingRelease &&
		deleteActionInputsReleased &&
		now >= m_projectDeleteActionSuppressedUntil)
	{
		m_projectDeleteActionAwaitingRelease = false;
	}
	if (m_projectBrowserInlineRename.pending &&
		now - m_projectBrowserInlineRename.pendingSince > kAssetBrowserInlineRenamePendingMaxSeconds)
	{
		m_projectBrowserInlineRename.pending = false;
	}
	const bool pointerInputPending =
		ImGui::IsWindowHovered(
			ImGuiHoveredFlags_RootAndChildWindows |
			ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
		(ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
			ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
			ImGui::IsMouseDown(ImGuiMouseButton_Middle));
	if (pointerInputPending)
		PrioritizeAssetBrowserUiFeedback();
	const bool uiFeedbackPriorityActive = IsAssetBrowserUiFeedbackPriorityActive();
	const bool interactive = IsAssetBrowserInteractive();
	const bool editorWindowClosing = IsEditorWindowClosing();
	const bool sceneReadbackValidationActive = IsEditorSceneReadbackValidationActive();
	if (editorWindowClosing)
	{
		ShutdownThumbnailPipeline();
		return;
	}
	PumpStandardPbrShaderPassPrewarm();
	if (!editorWindowClosing &&
		!sceneReadbackValidationActive &&
		!interactive &&
		m_thumbnailPreviewRenderer == nullptr)
	{
		(void)EnsureThumbnailPreviewRenderer();
	}
	const auto texturePumpDecision = NLS::Editor::Assets::PlanAssetBrowserCachedThumbnailTexturePump({
		interactive,
		m_thumbnailTextureLoadQueue.size(),
		m_thumbnailTextureDecodes.size(),
		m_pendingThumbnailTextureUploadsByPath.size(),
		0u,
		kMaxAssetBrowserInteractiveCachedThumbnailTexturePumpsPerFrame
	});
	if (texturePumpDecision.shouldPump)
	{
		const std::string texturePumpTelemetryPath = m_selectedProjectFolder + "|texture-pump";
		const size_t texturePumpTelemetryItemCount =
			m_thumbnailTextureLoadQueue.size() +
			m_thumbnailTextureDecodes.size() +
			m_pendingThumbnailTextureUploadsByPath.size();
		AssetBrowserArtifactTelemetryScope texturePumpTelemetry {
			NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTexturePump,
			std::chrono::steady_clock::now(),
			&texturePumpTelemetryPath,
			&texturePumpTelemetryItemCount,
			kAssetBrowserThumbnailUiDrawTelemetryMinimum
		};
		PumpQueuedCachedThumbnailTextureLoads(
			interactive
				? kMaxAssetBrowserInteractiveCachedThumbnailTexturePumpsPerFrame
				: kMaxAssetBrowserThumbnailTextureLoadsPerFrame);
	}
	PumpRetiredProjectAssetDatabaseRefreshes();
	PumpProjectFolderTreeRefresh();
	PumpCurrentFolderItemsRefresh();
	if (m_projectAssetDatabaseRefresh.has_value())
	{
		auto& refresh = *m_projectAssetDatabaseRefresh;
		if (!refresh.future.valid())
		{
			const auto diagnostic = std::string("Asset Browser database refresh future became invalid for project root \"") +
				refresh.root.generic_string() + "\".";
			NLS::Editor::Assets::FailAssetBrowserAsyncRefresh(m_projectAssetDatabaseRefreshState, diagnostic);
			NLS_LOG_ERROR(diagnostic);
			m_projectAssetDatabaseRefresh.reset();
		}
		else if (refresh.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			AssetDatabaseRefreshResult result;
			try
			{
				result = refresh.future.get();
			}
			catch (const std::exception& exception)
			{
				result.diagnostic = std::string("Asset Browser database refresh worker failed for project root \"") +
					refresh.root.generic_string() + "\": " + exception.what();
			}
			catch (...)
			{
				result.diagnostic = std::string("Asset Browser database refresh worker failed for project root \"") +
					refresh.root.generic_string() + "\": unknown error.";
			}

			const auto refreshRoot = refresh.root.lexically_normal();
			m_projectAssetDatabaseRefresh.reset();
			const bool currentRefresh =
				!refreshRoot.empty() &&
				refreshRoot == m_projectAssetDatabaseRoot.lexically_normal();
			if (currentRefresh &&
				result.database &&
				result.snapshot)
			{
				InvalidateObjectReferencePickerEntriesRefresh();
				RetireCurrentProjectAssetDatabase();
				m_projectAssetDatabase = std::move(result.database);
				m_projectAssetDatabaseSnapshot = std::move(result.snapshot);
				m_projectAssetDatabaseReady = true;
				// Thumbnail requests may have been built from the filesystem
				// fallback while the database refresh was in flight.  A database
				// snapshot changes the request's artifact identity and freshness,
				// so force the current scope to be rebuilt instead of allowing the
				// old no-artifact requests to remain queued under the same scope key.
				m_pendingThumbnailRequestContext.assetDatabaseSnapshot = m_projectAssetDatabaseSnapshot;
				m_importedPrefabThumbnailContinuationSubmittedRevisions.clear();
				m_lastImportedPrefabThumbnailContinuationPumpRevision = 0u;
				m_thumbnailGenerationScopeDirty = true;
				m_lastThumbnailGenerationScopeKey.clear();
				NLS::Editor::Assets::CompleteAssetBrowserAsyncRefresh(m_projectAssetDatabaseRefreshState);
				RefreshProjectAssetSubAssetSnapshotCache();
				RequestObjectReferencePickerEntriesRefresh();
				RebuildProjectAssetPresentation(NLS::Editor::Assets::BuildAssetBrowserRefreshPlan(
					NLS::Editor::Assets::AssetBrowserRefreshReason::AssetDatabaseReady));
			}
			else if (currentRefresh)
			{
				m_projectAssetDatabaseReady = false;
				if (result.diagnostic.empty())
				{
					result.diagnostic = std::string("Asset Browser database refresh produced no usable snapshot for project root \"") +
						refreshRoot.generic_string() + "\".";
				}
				NLS::Editor::Assets::FailAssetBrowserAsyncRefresh(
					m_projectAssetDatabaseRefreshState,
					result.diagnostic);
				NLS_LOG_ERROR(result.diagnostic);
			}
			RetireProjectAssetDatabaseResult(std::move(result));
			if (m_projectAssetDatabaseRefreshQueuedAfterInFlight)
			{
				m_projectAssetDatabaseRefreshQueuedAfterInFlight = false;
				RebuildProjectAssetPresentation(NLS::Editor::Assets::BuildAssetBrowserRefreshPlan(
					NLS::Editor::Assets::AssetBrowserRefreshReason::AssetDatabaseMutation));
			}
		}
	}
	PumpObjectReferencePickerEntriesRefresh();
	if (!uiFeedbackPriorityActive &&
		!editorWindowClosing &&
		!sceneReadbackValidationActive)
	{
		const bool sceneViewCameraNavigationActive =
			EDITOR_PANEL(NLS::Editor::Panels::SceneView, "Scene View").IsCameraNavigationActive();
		const auto thumbnailPumpPermissions =
			NLS::Editor::Assets::PlanAssetBrowserPostDrawThumbnailPump({
				interactive,
				sceneViewCameraNavigationActive,
				now,
				m_lightGpuThumbnailGenerationDeferredUntil,
				m_heavyGpuThumbnailGenerationDeferredUntil
			});
		PumpThumbnailGeneration(
			thumbnailPumpPermissions.allowGpuPreviewStart,
			thumbnailPumpPermissions.allowHeavyGpuPreview,
			true,
			sceneViewCameraNavigationActive);
	}

	const bool canApplyRequestedRefresh =
		!interactive &&
		m_refreshRequested &&
		ImGui::GetTime() >= m_refreshRequestedAfter &&
		!m_projectBrowserInlineRename.active &&
		!m_projectBrowserInlineRename.pending;
	if (canApplyRequestedRefresh)
	{
		m_refreshRequested = false;
		m_refreshRequestedAfter = 0.0;
		m_projectFolderTreeRefreshRequested = false;
		RefreshPreservingExpandedFolders();
	}
	else if (!interactive && m_projectFolderTreeRefreshRequested)
	{
		m_projectFolderTreeRefreshRequested = false;
		RebuildProjectFolderTreePresentation();
	}
}

void Editor::Panels::AssetBrowser::OnAfterDrawWidgets()
{
	DrawProjectAssetBrowser();
}

void Editor::Panels::AssetBrowser::PumpFileWatchers()
{
	if (!m_watchersStartupQueued)
		StartWatchersAsync();

	CompleteWatcherStartupIfReady();
	if (m_startupWatcherPreimportGateOpen)
		ConsumeWatcherChangesAndSchedulePreimport();
}

void Editor::Panels::AssetBrowser::PrepareStartupWatchers()
{
	if (!m_watchersStartupQueued)
		StartWatchersSynchronously();

	if (m_watcherStartup.valid() &&
		m_watcherStartup.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
	{
		m_watcherStartup.wait();
	}

	CompleteWatcherStartupIfReady();
	ConsumeWatcherChangesAndSchedulePreimport();
	RequestRefresh();
}

void Editor::Panels::AssetBrowser::AdoptStartupWatchers(
	Core::AssetFileWatcher engineAssetsWatcher,
	Core::AssetFileWatcher projectAssetsWatcher)
{
	CompleteWatcherStartupIfReady();
	if (m_watcherStartup.valid())
		AbandonAssetBrowserFuture(m_watcherStartup);

	m_engineAssetsWatcher = std::move(engineAssetsWatcher);
	m_projectAssetsWatcher = std::move(projectAssetsWatcher);
	m_watchersStartupQueued = true;
	m_watchersReadyRefreshQueued = true;
	m_startupWatcherPreimportGateOpen = false;
	RequestRefresh();
}

NLS::Editor::Assets::StartupWatcherPreimportResult Editor::Panels::AssetBrowser::RunStartupWatcherPreimport(
	const NLS::Editor::Assets::StartupAssetPreimportProgressSink& progressSink)
{
	using namespace NLS::Editor::Assets;

	CompleteWatcherStartupIfReady();
	StartupWatcherPreimportResult result;
	result.succeeded = true;
	for (;;)
	{
		const auto projectAssetChanges = m_projectAssetsWatcher.ConsumeChangedPaths();
		const auto engineAssetChanges = m_engineAssetsWatcher.ConsumeChangedPaths();
		if (!engineAssetChanges.empty())
		{
			NLS_LOG_INFO(
				"[Startup] Asset watcher engine changes during startup count=" +
				std::to_string(engineAssetChanges.size()) +
				" first=" +
				engineAssetChanges.front().string());
			RequestRefresh();
		}
		if (projectAssetChanges.empty())
			return result;
		NLS_LOG_INFO(
			"[Startup] Asset watcher project changes during startup count=" +
			std::to_string(projectAssetChanges.size()) +
			" first=" +
			projectAssetChanges.front().string());
		result.requiresRuntimeAssetRefresh = true;

		std::vector<std::filesystem::path> relativeChanges;
		relativeChanges.reserve(projectAssetChanges.size());
		const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
		for (const auto& changedPath : projectAssetChanges)
		{
			auto relative = changedPath.lexically_relative(projectRoot);
			if (relative.empty() || relative.is_absolute())
				relative = changedPath;
			relativeChanges.push_back(relative.lexically_normal());
		}

		AssetDatabaseFacade database(MakeProjectEditorAssetRoots(projectRoot));
		database.SetResidentPrefabPreviewRegistry(m_residentPrefabPreviewRegistry);
		ImportProgressTracker tracker;
		if (progressSink)
			tracker.Subscribe(progressSink);
		AssetPreimportScheduler preimportScheduler;
		const auto imported = preimportScheduler.Run(
			database,
			tracker,
			{AssetPreimportReason::FileWatcherChanged, std::move(relativeChanges)});
		result.succeeded = result.succeeded && imported;
	}
}

NLS::Editor::Assets::StartupWatcherPreimportResult Editor::Panels::AssetBrowser::CompleteStartupWatcherPreimportGate(
	const NLS::Editor::Assets::StartupAssetPreimportProgressSink& progressSink)
{
	const auto imported = RunStartupWatcherPreimport(progressSink);
	if (!imported.succeeded)
		return imported;

	m_startupWatcherPreimportGateOpen = true;
	RequestRefresh();
	return imported;
}

void Editor::Panels::AssetBrowser::CompleteWatcherStartupIfReady()
{
	if (m_watcherStartup.valid() &&
		m_watcherStartup.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		auto startup = m_watcherStartup.get();
		m_engineAssetsWatcher = std::move(startup.engineAssetsWatcher);
		m_projectAssetsWatcher = std::move(startup.projectAssetsWatcher);
		for (const auto& diagnostic : startup.diagnostics)
			NLS_LOG_WARNING(diagnostic.message);
		if (!m_watchersReadyRefreshQueued)
		{
			m_watchersReadyRefreshQueued = true;
			RequestRefresh();
		}
	}
}

void Editor::Panels::AssetBrowser::ConsumeWatcherChangesAndSchedulePreimport()
{
	if (!m_startupWatcherPreimportGateOpen)
		return;

	const auto engineAssetChanges = m_engineAssetsWatcher.ConsumeChangedPaths();
	const auto projectAssetChanges = m_projectAssetsWatcher.ConsumeChangedPaths();
	const bool engineAssetsChanged = !engineAssetChanges.empty();
	const bool projectAssetsChanged = !projectAssetChanges.empty();
	const bool csharpSourceChanged = std::any_of(
		projectAssetChanges.begin(),
		projectAssetChanges.end(),
		[](const std::filesystem::path& path)
		{
			auto extension = path.extension().string();
			std::transform(
				extension.begin(),
				extension.end(),
				extension.begin(),
				[](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
			return extension == ".cs";
		});
	const bool scriptSourceChanged = std::any_of(
		projectAssetChanges.begin(),
		projectAssetChanges.end(),
		[](const std::filesystem::path& path)
		{
			auto extension = path.extension().string();
			std::transform(
				extension.begin(),
				extension.end(),
				extension.begin(),
				[](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
			return extension == ".cs" || extension == ".lua";
		});
	if (projectAssetsChanged)
	{
		std::vector<std::filesystem::path> relativeChanges;
		relativeChanges.reserve(projectAssetChanges.size());
		const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
		for (const auto& changedPath : projectAssetChanges)
		{
			auto relative = changedPath.lexically_relative(projectRoot);
			if (relative.empty() || relative.is_absolute())
				relative = changedPath;
			relativeChanges.push_back(relative.lexically_normal());
		}
		ScheduleProjectAssetPreimport({
			NLS::Editor::Assets::AssetPreimportReason::FileWatcherChanged,
			std::move(relativeChanges)
		});
	}
	if (csharpSourceChanged && EDITOR_CONTEXT(managedScriptDebugSession) != nullptr)
	{
		if (!EDITOR_CONTEXT(managedScriptDebugSession)->RequestBuild())
			NLS_LOG_WARNING("C# source changed, but the managed script build session is disabled or unavailable.");
	}
	if (scriptSourceChanged)
	{
		// WorkspaceRevision is content-derived. Re-run the generator after a
		// script add/remove/move so CPS and VS Code see the same project view;
		// WriteTextIfChanged keeps idle frames free of filesystem writes.
		const auto editorExecutable = NLS::Platform::Process::GetCurrentExecutablePath();
		const auto brokerExecutable = editorExecutable.parent_path() /
#ifdef _WIN32
			"NullusDebugBroker.exe";
#else
			"NullusDebugBroker";
#endif
		NLS::Scripting::ScriptDebugSettings debugSettings;
		if (EDITOR_CONTEXT(scriptDebugService) != nullptr)
			debugSettings = EDITOR_CONTEXT(scriptDebugService)->GetSettings();
		const auto workspace = NLS::Editor::Debug::GenerateProjectDebugWorkspace(
			ProjectRootFromAssetsFolder(m_projectAssetFolder),
			editorExecutable,
			brokerExecutable,
			debugSettings.luaPandaPort,
			debugSettings.stopOnEntry);
		if (!workspace.success)
			NLS_LOG_WARNING("Script workspace synchronization failed: " + workspace.errorMessage);
		else if (EDITOR_CONTEXT(editorDebugEndpoint) != nullptr)
			EDITOR_CONTEXT(editorDebugEndpoint)->PublishEvent({
				{"type", "WorkspaceChanged"},
				{"workspaceRevision", workspace.manifest.workspaceRevision},
				{"visualStudioSolution", workspace.manifest.visualStudioSolution.string()},
				{"visualStudioProject", workspace.manifest.visualStudioProject.string()}});
	}
	if (engineAssetsChanged && !projectAssetsChanged)
		RequestRefresh();
}

void Editor::Panels::AssetBrowser::RequestRefresh()
{
	m_refreshRequested = true;
	const double now = ImGui::GetCurrentContext() != nullptr ? ImGui::GetTime() : 0.0;
	m_refreshRequestedAfter = (std::max)(
		m_refreshRequestedAfter,
		now + kAssetBrowserRefreshDebounceSeconds);
}

void Editor::Panels::AssetBrowser::ScheduleProjectAssetPreimport(
	NLS::Editor::Assets::AssetPreimportRequest request)
{
	using namespace NLS::Editor::Assets;

	AssetPreimportScheduler scheduler;
	if (!scheduler.ShouldRunForReason(request.reason))
		return;

	const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
	if (projectRoot.empty())
	{
		NLS_LOG_ERROR("Failed to resolve project root for asset preimport.");
		return;
	}

	if (m_projectAssetPreimportRunning)
	{
		m_pendingProjectAssetPreimportRequest = m_pendingProjectAssetPreimportRequest.has_value()
			? MergeAssetPreimportRequests(std::move(*m_pendingProjectAssetPreimportRequest), request)
			: std::move(request);
		return;
	}
	m_projectAssetPreimportRunning = true;

	auto& tracker = EDITOR_CONTEXT(importProgressTracker);
	const auto queued = EDITOR_EXEC(TrackBackgroundTask([
		projectRoot,
		request = std::move(request),
		residentPrefabPreviewRegistry = m_residentPrefabPreviewRegistry,
		&tracker]
	{
		AssetDatabaseFacade database(MakeProjectEditorAssetRoots(projectRoot));
		database.SetResidentPrefabPreviewRegistry(residentPrefabPreviewRegistry);
		NLS::Core::Assets::AssetDiagnostics diagnostics;
		AssetPreimportScheduler preimportScheduler;
		const auto imported = preimportScheduler.Run(database, tracker, request);
		AppendUniqueDiagnostics(diagnostics, database.GetDiagnostics());
		EDITOR_EXEC(DelayAction([
			reason = request.reason,
			changedPaths = request.changedPaths,
			imported,
			diagnostics = std::move(diagnostics)]
		{
			auto& assetBrowser = EDITOR_PANEL(NLS::Editor::Panels::AssetBrowser, "Asset Browser");
			assetBrowser.m_projectAssetPreimportRunning = false;
			if (assetBrowser.m_pendingProjectAssetPreimportRequest.has_value())
			{
				auto pendingRequest = std::move(*assetBrowser.m_pendingProjectAssetPreimportRequest);
				assetBrowser.m_pendingProjectAssetPreimportRequest.reset();
				assetBrowser.ScheduleProjectAssetPreimport(std::move(pendingRequest));
			}
			else
			{
				assetBrowser.RequestRefresh();
			}
			if (imported)
			{
				NLS_LOG_INFO(std::string("Asset preimport completed after ") + AssetPreimportReasonLabel(reason));
			}
			else
			{
				LogAssetPreimportFailureDetails(reason, changedPaths, {}, diagnostics);
			}
		}));
	}));
	if (!queued)
	{
		m_projectAssetPreimportRunning = false;
		NLS_LOG_ERROR("Failed to queue project asset preimport because the editor background task queue is at capacity.");
		RequestRefresh();
	}
}

void Editor::Panels::AssetBrowser::RefreshPreservingExpandedFolders()
{
	RebuildProjectAssetPresentation(NLS::Editor::Assets::BuildAssetBrowserRefreshPlan(
		NLS::Editor::Assets::AssetBrowserRefreshReason::AssetDatabaseMutation));
}

void Editor::Panels::AssetBrowser::RebuildProjectFolderTreePresentation()
{
	NLS_PROFILE_NAMED_SCOPE("AssetBrowser::RebuildProjectFolderTreePresentation");
	const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
	AddProjectBrowserAncestorFolders(m_expandedProjectFolders, m_selectedProjectFolder);
	NLS::Editor::Assets::AssetBrowserFolderTreeBuildOptions treeOptions;
	treeOptions.expandedFolders = m_expandedProjectFolders;
	treeOptions.selectedFolder = m_selectedProjectFolder;
	StartProjectFolderTreeRefresh(projectRoot, std::move(treeOptions));
	if (m_projectFolderTree.projectRelativePath.empty())
	{
		m_projectFolderTree.displayName = "Assets";
		m_projectFolderTree.projectRelativePath = "Assets";
		m_projectFolderTree.absolutePath = projectRoot / "Assets";
	}
}

void Editor::Panels::AssetBrowser::RebuildProjectAssetPresentation(
	const NLS::Editor::Assets::AssetBrowserRefreshPlan refreshPlan)
{
	NLS_PROFILE_NAMED_SCOPE("AssetBrowser::RebuildProjectAssetPresentation");
	++m_projectAssetPresentationGeneration;
	const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
	const auto resolved = NLS::Editor::Assets::ResolveAssetBrowserFolderSelection(
		projectRoot,
		m_selectedProjectFolder);
	m_selectedProjectFolder = resolved.projectRelativePath;
	m_currentBreadcrumb = NLS::Editor::Assets::BuildAssetBrowserBreadcrumb(m_selectedProjectFolder);

	if (refreshPlan.rebuildFolderTree || m_projectFolderTree.projectRelativePath.empty())
	{
		RebuildProjectFolderTreePresentation();
	}

	if (projectRoot.empty())
	{
		InvalidateObjectReferencePickerEntriesRefresh();
		RetireCurrentProjectAssetDatabase();
		m_projectAssetDatabaseRoot.clear();
		m_projectAssetDatabaseReady = false;
		m_projectAssetSubAssetSnapshotIndex.reset();
		DiscardProjectAssetDatabaseRefresh();
		m_projectAssetDatabaseRefreshQueuedAfterInFlight = false;
		NLS::Editor::Assets::SetObjectReferencePickerEntries({});
		m_objectReferencePickerRefreshRequested = false;
		NLS::Editor::Assets::ResetAssetBrowserAsyncRefresh(m_projectAssetDatabaseRefreshState);
		NLS::Editor::Assets::ResetAssetBrowserAsyncRefresh(m_currentFolderItemsRefreshState);
		NLS::Editor::Assets::ResetAssetBrowserAsyncRefresh(m_objectReferencePickerRefreshState);
	}
	else if (m_projectAssetDatabaseRoot.lexically_normal() != projectRoot.lexically_normal())
	{
		InvalidateObjectReferencePickerEntriesRefresh();
		m_projectAssetDatabaseRoot = projectRoot.lexically_normal();
		RetireCurrentProjectAssetDatabase();
		m_projectAssetDatabaseReady = false;
		m_projectAssetSubAssetSnapshotIndex.reset();
		DiscardProjectAssetDatabaseRefresh();
		m_projectAssetDatabaseRefreshQueuedAfterInFlight = false;
		NLS::Editor::Assets::SetObjectReferencePickerEntries({});
		m_objectReferencePickerRefreshRequested = false;
	}

	const auto refreshScheduling = NLS::Editor::Assets::PlanAssetDatabaseRefreshScheduling(
		projectRoot.empty(),
		refreshPlan.refreshAssetDatabase,
		m_projectAssetDatabaseReady,
		m_projectAssetDatabaseRefresh.has_value() &&
			m_projectAssetDatabaseRefresh->root.lexically_normal() == projectRoot.lexically_normal());
	if (refreshScheduling.queueRefreshAfterInFlight)
		m_projectAssetDatabaseRefreshQueuedAfterInFlight = true;
		if (refreshScheduling.startRefresh)
		{
			InvalidateObjectReferencePickerEntriesRefresh();
			NLS::Editor::Assets::SetObjectReferencePickerEntries({});
			m_objectReferencePickerRefreshRequested = false;
			NLS::Editor::Assets::ResetAssetBrowserAsyncRefresh(m_objectReferencePickerRefreshState);
			m_projectAssetDatabaseReady = false;
			m_projectAssetSubAssetSnapshotIndex.reset();
			m_selectedProjectItem.reset();
			m_unfilteredCurrentFolderItems.clear();
			m_currentFolderItems.clear();
			m_projectDisplayItems.clear();
			RetireCurrentProjectAssetDatabase();
			MarkProjectAssetDisplayItemsDirty();
			m_visibleThumbnailItems.clear();
			m_visibleThumbnailItemsKnown = false;
			m_visiblePrefabHotCachePreloadPending = false;
			m_thumbnailResultsByItemKey.clear();
			m_thumbnailItemKeyByCacheKey.clear();
			m_thumbnailItemKeyByPresentationKey.clear();
			m_assetThumbnailsByCacheKey.clear();
			m_thumbnailService.ClearQueuedRequests();
			NLS::Editor::Assets::BeginAssetBrowserAsyncRefresh(m_projectAssetDatabaseRefreshState);
			try
			{
				m_projectAssetDatabaseRefresh = AssetDatabaseRefresh {
					projectRoot.lexically_normal(),
					ScheduleAssetBrowserJobFuture(
						"AssetBrowser.ProjectAssetDatabaseRefresh",
						[projectRoot = projectRoot.lexically_normal(),
						 residentPrefabPreviewRegistry = m_residentPrefabPreviewRegistry]() -> AssetDatabaseRefreshResult
						{
							AssetDatabaseRefreshResult result;
							const auto context = std::string("Asset Browser database refresh failed for project root \"") +
								projectRoot.generic_string() + "\"";
							try
							{
								result.database = std::make_unique<NLS::Editor::Assets::AssetDatabaseFacade>(
									NLS::Editor::Assets::MakeProjectEditorAssetRoots(projectRoot));
								result.database->SetResidentPrefabPreviewRegistry(
									residentPrefabPreviewRegistry);
								if (!result.database->Refresh())
								{
									result.database.reset();
									result.diagnostic = context + ": refresh returned false.";
									return result;
								}
								result.snapshot = NLS::Editor::Assets::AssetDatabaseFacade::CreateReadOnlySnapshot(*result.database);
							}
							catch (const std::exception& exception)
							{
								result.database.reset();
								result.snapshot.reset();
								result.diagnostic = context + ": " + exception.what();
							}
							catch (...)
							{
								result.database.reset();
								result.snapshot.reset();
								result.diagnostic = context + ": unknown error.";
							}
						return result;
					})
			};
		}
		catch (const std::exception& exception)
		{
			DiscardProjectAssetDatabaseRefresh();
			const auto diagnostic = std::string("Asset Browser database refresh failed to start for project root \"") +
				projectRoot.generic_string() + "\": " + exception.what();
			NLS::Editor::Assets::FailAssetBrowserAsyncRefresh(m_projectAssetDatabaseRefreshState, diagnostic);
			NLS_LOG_ERROR(diagnostic);
		}
		catch (...)
		{
			DiscardProjectAssetDatabaseRefresh();
			const auto diagnostic = std::string("Asset Browser database refresh failed to start for project root \"") +
				projectRoot.generic_string() + "\": unknown error.";
			NLS::Editor::Assets::FailAssetBrowserAsyncRefresh(m_projectAssetDatabaseRefreshState, diagnostic);
			NLS_LOG_ERROR(diagnostic);
		}
	}
	if (refreshPlan.rebuildCurrentFolderItems)
		{
			NLS::Editor::Assets::AssetBrowserBuildOptions presentationBuildOptions;
			presentationBuildOptions.includeGeneratedSubAssets = true;
			presentationBuildOptions.verifyGeneratedSubAssetManifests = false;
			presentationBuildOptions.loadSourceAssetMetadataWithoutDatabase = !m_projectAssetDatabaseSnapshot;
			presentationBuildOptions.expandedSourceAssets = m_expandedProjectAssetItems;
			presentationBuildOptions.searchQuery = m_projectSearchQuery;
			presentationBuildOptions.typeFilter = m_projectTypeFilter;
			if (refreshPlan.clearCurrentFolderItemsBeforeAsyncRefresh)
			{
				NLS::Editor::Assets::AssetBrowserBuildOptions fallbackBuildOptions;
				fallbackBuildOptions.includeGeneratedSubAssets = false;
				fallbackBuildOptions.verifyGeneratedSubAssetManifests = false;
				fallbackBuildOptions.loadSourceAssetMetadataWithoutDatabase = true;
				m_unfilteredCurrentFolderItems = NLS::Editor::Assets::BuildCurrentFolderAssetItems(
					projectRoot,
					m_selectedProjectFolder,
					nullptr,
					fallbackBuildOptions);
				NLS::Editor::Assets::AssetBrowserBuildOptions fallbackFilterOptions;
				fallbackFilterOptions.includeGeneratedSubAssets = false;
				fallbackFilterOptions.verifyGeneratedSubAssetManifests = false;
				fallbackFilterOptions.searchQuery = m_projectSearchQuery;
				fallbackFilterOptions.typeFilter = m_projectTypeFilter;
				m_currentFolderItems = NLS::Editor::Assets::FilterAssetBrowserItems(
					m_unfilteredCurrentFolderItems,
					fallbackFilterOptions);
				m_projectDisplayItems = NLS::Editor::Assets::BuildAssetBrowserDisplayItems(
					m_currentFolderItems,
					{});
				m_thumbnailGenerationScopeDirty = true;
			}
				StartCurrentFolderItemsRefresh(
					projectRoot,
					m_selectedProjectFolder,
					presentationBuildOptions);
				return;
			}
	MarkProjectAssetDisplayItemsDirty();
}

void Editor::Panels::AssetBrowser::SelectProjectFolder(const std::string& projectRelativePath)
{
	const auto requested = NormalizeProjectBrowserPath(projectRelativePath);
	if (m_selectedProjectFolder == requested)
		return;

	m_selectedProjectFolder = requested;
	m_selectedProjectItem.reset();
	CancelInlineRenameProjectItem();
	const double now = ImGui::GetTime();
	m_assetBrowserInteractiveUntil = now + kAssetBrowserScrollIdleDelaySeconds;
	m_lightGpuThumbnailGenerationDeferredUntil = now + kAssetBrowserScrollIdleDelaySeconds;
	m_heavyGpuThumbnailGenerationDeferredUntil = now + kAssetBrowserHeavyGpuThumbnailIdleDelaySeconds;
	m_thumbnailRenderScheduler.DeferHeavyGpuPreviewUntil(
		m_heavyGpuThumbnailGenerationDeferredUntil);
	AddProjectBrowserAncestorFolders(m_expandedProjectFolders, m_selectedProjectFolder);
	RebuildProjectAssetPresentation(NLS::Editor::Assets::BuildAssetBrowserRefreshPlan(
		NLS::Editor::Assets::AssetBrowserRefreshReason::FolderSelection));
}

void Editor::Panels::AssetBrowser::SelectProjectFolderForValidation(const std::string& projectRelativePath)
{
	SelectProjectFolder(projectRelativePath);
}

bool Editor::Panels::AssetBrowser::IsAssetBrowserInteractive() const
{
	return IsAssetBrowserUiFeedbackPriorityActive() ||
		ImGui::GetTime() < m_assetBrowserInteractiveUntil;
}

void Editor::Panels::AssetBrowser::PrioritizeAssetBrowserUiFeedback()
{
	const int priorityThroughFrame = ImGui::GetFrameCount() + 1;
	if (!m_assetBrowserUiFeedbackPriorityThroughFrame.has_value() ||
		*m_assetBrowserUiFeedbackPriorityThroughFrame < priorityThroughFrame)
	{
		m_assetBrowserUiFeedbackPriorityThroughFrame = priorityThroughFrame;
	}
}

bool Editor::Panels::AssetBrowser::IsAssetBrowserUiFeedbackPriorityActive() const
{
	return m_assetBrowserUiFeedbackPriorityThroughFrame.has_value() &&
		ImGui::GetFrameCount() <= *m_assetBrowserUiFeedbackPriorityThroughFrame;
}

void Editor::Panels::AssetBrowser::MarkProjectAssetDisplayItemsDirty()
{
	m_projectDisplayItemsDirty = true;
	m_projectDisplayItems.clear();
	m_thumbnailGenerationScopeDirty = true;
	m_pendingThumbnailScopeItems.clear();
	m_pendingThumbnailScopeOffset = 0u;
	m_pendingThumbnailRequestContext = MakeAssetBrowserThumbnailRequestBuildContext(m_thumbnailFeatureConfig);
	m_pendingThumbnailRequestContext.residentPrefabPreviewRegistry = m_residentPrefabPreviewRegistry;
	m_thumbnailScopeBuildInProgress = false;
	m_heavyGpuThumbnailGenerationDeferredUntil = (std::max)(
		m_heavyGpuThumbnailGenerationDeferredUntil,
		ImGui::GetTime() + kAssetBrowserHeavyGpuThumbnailIdleDelaySeconds);
}

bool Editor::Panels::AssetBrowser::ApplyProjectAssetDisclosureImmediately(
	const NLS::Editor::Assets::AssetBrowserItem& sourceItem)
{
	const auto sourcePath = sourceItem.sourceAssetPath.empty()
		? sourceItem.projectRelativePath
		: sourceItem.sourceAssetPath;
	if (sourcePath.empty())
		return false;
	PrioritizeAssetBrowserUiFeedback();

	const auto sourceIdentity = NLS::Editor::Assets::BuildAssetBrowserActionIdentity(sourceItem);
	const auto source = std::find_if(
		m_projectDisplayItems.begin(),
		m_projectDisplayItems.end(),
		[&](const NLS::Editor::Assets::AssetBrowserDisplayItem& candidate)
		{
			if (candidate.subAsset || candidate.loadingPlaceholder)
				return false;
			if (sourceIdentity.has_value())
				return NLS::Editor::Assets::AssetBrowserItemMatchesActionIdentity(candidate.item, *sourceIdentity);
			const auto candidatePath = candidate.item.sourceAssetPath.empty()
				? candidate.item.projectRelativePath
				: candidate.item.sourceAssetPath;
			return candidatePath == sourcePath;
		});
	if (source == m_projectDisplayItems.end())
		return false;

	NLS::Editor::Assets::AssetBrowserBuildOptions options;
	options.includeGeneratedSubAssets = true;
	options.verifyGeneratedSubAssetManifests = false;
	options.loadSourceAssetMetadataWithoutDatabase = !m_projectAssetDatabaseSnapshot;
	options.expandedSourceAssets = m_expandedProjectAssetItems;
	options.searchQuery = m_projectSearchQuery;
	options.typeFilter = m_projectTypeFilter;
	const bool expanded = m_expandedProjectAssetItems.contains(sourcePath);
	auto group = NLS::Editor::Assets::BuildAssetBrowserImmediateDisclosureFeedback(
		*source,
		expanded);

	const auto sourceIndex = static_cast<size_t>(std::distance(m_projectDisplayItems.begin(), source));
	size_t groupEnd = sourceIndex + 1u;
	while (groupEnd < m_projectDisplayItems.size() && m_projectDisplayItems[groupEnd].subAsset)
		++groupEnd;
	auto insertion = m_projectDisplayItems.erase(
		m_projectDisplayItems.begin() + static_cast<std::ptrdiff_t>(sourceIndex),
		m_projectDisplayItems.begin() + static_cast<std::ptrdiff_t>(groupEnd));
	m_projectDisplayItems.insert(
		insertion,
		std::make_move_iterator(group.begin()),
		std::make_move_iterator(group.end()));

	m_currentFolderItems.clear();
	m_currentFolderItems.reserve(m_projectDisplayItems.size());
	for (const auto& displayItem : m_projectDisplayItems)
	{
		if (!displayItem.loadingPlaceholder)
			m_currentFolderItems.push_back(displayItem.item);
	}
	++m_projectAssetPresentationGeneration;
	StartCurrentFolderItemsRefresh(
		ProjectRootFromAssetsFolder(m_projectAssetFolder),
		m_selectedProjectFolder,
		std::move(options));
	return true;
}

void Editor::Panels::AssetBrowser::RebuildProjectAssetDisplayItemsIfNeeded()
{
	if (!m_projectDisplayItemsDirty)
		return;
	NLS_PROFILE_NAMED_SCOPE("AssetBrowser::RebuildProjectAssetDisplayItemsIfNeeded");
	if (m_projectAssetFolder.empty())
		return;

	NLS::Editor::Assets::AssetBrowserBuildOptions buildOptions;
	buildOptions.includeGeneratedSubAssets = true;
	buildOptions.verifyGeneratedSubAssetManifests = false;
	buildOptions.loadSourceAssetMetadataWithoutDatabase = !m_projectAssetDatabaseSnapshot;
	buildOptions.expandedSourceAssets = m_expandedProjectAssetItems;
	buildOptions.searchQuery = m_projectSearchQuery;
	buildOptions.typeFilter = m_projectTypeFilter;
	StartCurrentFolderItemsRefresh(
		ProjectRootFromAssetsFolder(m_projectAssetFolder),
		m_selectedProjectFolder,
		std::move(buildOptions));
}

void Editor::Panels::AssetBrowser::SetVisibleThumbnailItems(
	std::vector<NLS::Editor::Assets::AssetBrowserItem> visibleItems)
{
	const size_t visibleThumbnailCount = static_cast<size_t>(std::count_if(
		visibleItems.begin(),
		visibleItems.end(),
		[](const NLS::Editor::Assets::AssetBrowserItem& item)
		{
			return NLS::Editor::Assets::CanGenerateAssetBrowserThumbnail(item);
		}));
	const std::string visibleSetTelemetryPath = m_selectedProjectFolder + "|visible-set";
	const size_t visibleSetTelemetryItemCount = visibleItems.size();
	std::chrono::microseconds hashElapsed {};
	std::chrono::microseconds applyElapsed {};
	const auto nextRequestSize = AssetBrowserThumbnailRequestSize(m_thumbnailSize);
	uint64_t nextFingerprint = 0u;
	{
		AssetBrowserArtifactTelemetryAccumulatorScope hashTelemetry {
			hashElapsed
		};
		nextFingerprint = HashVisibleThumbnailItems(
			visibleItems,
			nextRequestSize,
			m_selectedProjectFolder);
	}
	if (NLS::Core::Assets::IsArtifactLoadTelemetryEnabled())
	{
		std::vector<std::string> initialVisibleThumbnailItemKeys;
		initialVisibleThumbnailItemKeys.reserve(visibleThumbnailCount);
		for (const auto& item : visibleItems)
		{
			if (NLS::Editor::Assets::CanGenerateAssetBrowserThumbnail(item))
			{
				initialVisibleThumbnailItemKeys.push_back(
					NLS::Editor::Assets::BuildAssetBrowserThumbnailItemKey(item, nextRequestSize));
			}
		}
		BeginAssetBrowserThumbnailVisibleSetTelemetry(
			visibleThumbnailCount,
			initialVisibleThumbnailItemKeys,
			nextFingerprint);
	}
	if (m_visibleThumbnailItemsKnown &&
		nextFingerprint == m_visibleThumbnailFingerprint &&
		visibleItems.size() == m_visibleThumbnailCount &&
		nextRequestSize == m_visibleThumbnailRequestSize)
	{
		FlushPendingVisiblePrefabHotCachePreload();
		RecordAssetBrowserArtifactTelemetryStage(
			NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSetHash,
			hashElapsed,
			visibleSetTelemetryPath,
			visibleSetTelemetryItemCount,
			kAssetBrowserThumbnailUiDrawTelemetryMinimum);
		return;
	}

	{
		AssetBrowserArtifactTelemetryAccumulatorScope applyTelemetry {
			applyElapsed
		};
		m_visibleThumbnailItems = std::move(visibleItems);
		m_visibleThumbnailItemsKnown = true;
		m_visibleThumbnailFingerprint = nextFingerprint;
		m_visibleThumbnailCount = m_visibleThumbnailItems.size();
		m_visibleThumbnailRequestSize = nextRequestSize;
		m_visiblePrefabHotCachePreloadPending = true;
		m_visiblePendingPresentationRecoveryAfter = 0.0;
		m_visiblePendingPresentationRecoveryOffset = 0u;
		m_thumbnailGenerationScopeDirty = true;
	}
	RecordAssetBrowserArtifactTelemetryStage(
		NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSetHash,
		hashElapsed,
		visibleSetTelemetryPath,
		visibleSetTelemetryItemCount,
		kAssetBrowserThumbnailUiDrawTelemetryMinimum);
	RecordAssetBrowserArtifactTelemetryStage(
		NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSetApply,
		applyElapsed,
		visibleSetTelemetryPath,
		visibleSetTelemetryItemCount,
		kAssetBrowserThumbnailUiDrawTelemetryMinimum);
}

void Editor::Panels::AssetBrowser::DrawProjectAssetBrowser()
{
	++m_thumbnailTextureFrameSerial;
	if (m_assetThumbnailPool != nullptr)
		m_assetThumbnailPool->Prune(m_thumbnailTextureFrameSerial);
	const auto thumbnailTextureFramePlan = NLS::Editor::Assets::BeginAssetBrowserThumbnailTextureFrame(
		std::move(m_thumbnailTexturesUsedThisFrame),
		std::move(m_thumbnailTexturesPendingRelease));
	m_thumbnailTexturesUsedThisFrame = std::move(thumbnailTextureFramePlan.usedThisFrame);
	m_thumbnailTexturesPendingRelease = std::move(thumbnailTextureFramePlan.pendingRelease);
	for (const auto& key : thumbnailTextureFramePlan.releaseNow)
		ReleaseCachedThumbnailTexture(key);

	const float availableHeight = ImGui::GetContentRegionAvail().y;
	if (availableHeight <= 0.0f)
		return;
	const auto& io = ImGui::GetIO();
	const bool wheelActive = io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f;
	const auto markInteractive = [this]()
	{
		const double now = ImGui::GetTime();
		m_assetBrowserInteractiveUntil = now + kAssetBrowserScrollIdleDelaySeconds;
		m_lightGpuThumbnailGenerationDeferredUntil =
			now + kAssetBrowserScrollIdleDelaySeconds;
	};

	const float treeWidth = (std::max)(180.0f, (std::min)(320.0f, ImGui::GetContentRegionAvail().x * 0.30f));
	ImGui::BeginChild("##AssetBrowserFolderTree", ImVec2(treeWidth, availableHeight), true);
	const bool folderTreeHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
	if (folderTreeHovered &&
		(wheelActive ||
			ImGui::IsMouseDragging(ImGuiMouseButton_Left) ||
			ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
			ImGui::IsMouseDragging(ImGuiMouseButton_Right)))
	{
		markInteractive();
	}
	if (m_projectFolderTree.projectRelativePath.empty())
		RebuildProjectAssetPresentation(NLS::Editor::Assets::BuildAssetBrowserRefreshPlan(
			NLS::Editor::Assets::AssetBrowserRefreshReason::InitialBuild));
		(void)DrawProjectFolderTree(m_projectFolderTree);
	ImGui::EndChild();

	ImGui::SameLine();

	ImGui::BeginChild("##AssetBrowserContent", ImVec2(0.0f, availableHeight), true);
	const bool contentWindowHovered =
		ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
	const bool contentWindowFocused =
		ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	if ((contentWindowHovered || contentWindowFocused) && io.WantTextInput)
	{
		// Text entry is local Asset Browser interaction even when the mouse is
		// outside the filter control; keep heavy preview starts deferred.
		markInteractive();
	}
	DrawProjectBreadcrumb();
	DrawProjectFilterBar();
	ImGui::Separator();
	const float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
	ImGui::BeginChild("##AssetBrowserCurrentFolder", ImVec2(0.0f, -footerHeight), false);
	const bool currentFolderHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
	if (auto droppedFiles = NLS::Editor::Assets::ConsumeAssetBrowserExternalDroppedFiles(
			m_pendingExternalDroppedFiles,
			currentFolderHovered);
		droppedFiles.has_value())
	{
		HandleProjectAssetBrowserDroppedFiles(*droppedFiles);
	}
	HandleProjectAssetBrowserShortcuts();
	const bool currentFolderActive =
		currentFolderHovered &&
		(wheelActive ||
			ImGui::IsMouseDragging(ImGuiMouseButton_Left) ||
			ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
			ImGui::IsMouseDragging(ImGuiMouseButton_Right));
	if (currentFolderActive)
		markInteractive();
	DrawCurrentFolderGrid();
	if (ImGui::BeginPopupContextWindow(
			"##AssetBrowserCurrentFolderContext",
			ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
	{
		const auto currentFolder = NormalizeProjectBrowserPath(m_selectedProjectFolder);
		DrawProjectCurrentFolderContextMenu(
			"##currentFolderContent",
			currentFolder,
			ProjectBrowserAbsolutePathForResourcePath(m_projectAssetFolder, currentFolder));
		ImGui::EndPopup();
	}
	const ImVec2 remainingDropSpace = ImGui::GetContentRegionAvail();
	if (remainingDropSpace.x > 1.0f && remainingDropSpace.y > ImGui::GetFrameHeight())
	{
		ImGui::InvisibleButton("##AssetBrowserCurrentFolderDropTarget", remainingDropSpace);
		DrawProjectFolderDropTarget(
			NormalizeProjectBrowserPath(m_selectedProjectFolder),
			ProjectBrowserAbsolutePathForResourcePath(m_projectAssetFolder, m_selectedProjectFolder));
	}
	ImGui::EndChild();
	DrawProjectBrowserTextDialog();
	ImGui::Separator();
	DrawAssetBrowserFooter();
	RebuildProjectAssetDisplayItemsIfNeeded();
	UpdateThumbnailGenerationScope();
	PruneCachedThumbnailTextures();
	ImGui::EndChild();
}

bool Editor::Panels::AssetBrowser::DrawProjectFolderTree(
	const NLS::Editor::Assets::AssetBrowserFolderNode& node)
{
	if (node.projectRelativePath.empty())
		return true;

	const bool selected = m_selectedProjectFolder == node.projectRelativePath;
	const bool openedBySelection = IsProjectBrowserAncestorOf(node.projectRelativePath, m_selectedProjectFolder);
	const bool hasExpandableChildren = node.hasChildren || !node.children.empty();
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
	if (!hasExpandableChildren)
		flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	if (selected)
		flags |= ImGuiTreeNodeFlags_Selected;
	if (openedBySelection)
		ImGui::SetNextItemOpen(true, ImGuiCond_Once);

	const bool opened = ImGui::TreeNodeEx(
		node.projectRelativePath.c_str(),
		flags,
		"%s",
		node.displayName.c_str());
	if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
	{
		const auto selectedProjectFolder = node.projectRelativePath;
		const bool selectionWillChange =
			NLS::Editor::Assets::ShouldStopDrawingAssetBrowserFolderNodeAfterSelection(
				m_selectedProjectFolder,
				selectedProjectFolder);
		SelectProjectFolder(selectedProjectFolder);
		if (selectionWillChange)
		{
			if (opened && hasExpandableChildren)
				ImGui::TreePop();
			return false;
		}
	}
	if (opened && hasExpandableChildren && !node.childrenEnumerated)
	{
		m_expandedProjectFolders.insert(node.projectRelativePath);
		m_projectFolderTreeRefreshRequested = true;
	}
	const auto presentationGeneration = m_projectAssetPresentationGeneration;
	DrawProjectFolderContextMenu(
		"##folderTreeContext",
		node.projectRelativePath,
		node.absolutePath);
	if (presentationGeneration != m_projectAssetPresentationGeneration)
	{
		if (opened && hasExpandableChildren)
			ImGui::TreePop();
		return false;
	}
	DrawProjectFolderDropTarget(node.projectRelativePath, node.absolutePath);
	if (presentationGeneration != m_projectAssetPresentationGeneration)
	{
		if (opened && hasExpandableChildren)
			ImGui::TreePop();
		return false;
	}

	if (hasExpandableChildren && opened)
	{
		for (const auto& child : node.children)
		{
			if (!DrawProjectFolderTree(child))
			{
				ImGui::TreePop();
				return false;
			}
		}
		ImGui::TreePop();
	}
	return true;
}

void Editor::Panels::AssetBrowser::DrawProjectFilterBar()
{
	bool filtersChanged = false;
	std::array<char, 256u> searchBuffer {};
	const auto copyCount = (std::min)(m_projectSearchQuery.size(), searchBuffer.size() - 1u);
	std::copy_n(m_projectSearchQuery.data(), copyCount, searchBuffer.data());

	ImGui::SetNextItemWidth((std::max)(160.0f, ImGui::GetContentRegionAvail().x - 160.0f));
	if (ImGui::InputTextWithHint(
			"##AssetBrowserSearch",
			"Search",
			searchBuffer.data(),
			searchBuffer.size()))
	{
		m_projectSearchQuery = searchBuffer.data();
		filtersChanged = true;
	}

	ImGui::SameLine();
	ImGui::SetNextItemWidth(140.0f);
	if (ImGui::BeginCombo("##AssetBrowserTypeFilter", NLS::Editor::Assets::AssetBrowserItemTypeDisplayLabel(m_projectTypeFilter)))
	{
		for (const auto type : NLS::Editor::Assets::AssetBrowserItemTypeFilterOptions())
		{
			const bool selected = m_projectTypeFilter == type;
			if (ImGui::Selectable(NLS::Editor::Assets::AssetBrowserItemTypeDisplayLabel(type), selected))
			{
				if (m_projectTypeFilter != type)
				{
					m_projectTypeFilter = type;
					filtersChanged = true;
				}
			}
			if (selected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	if (filtersChanged)
	{
		m_selectedProjectItem.reset();
		MarkProjectAssetDisplayItemsDirty();
	}
}

void Editor::Panels::AssetBrowser::DrawAssetBrowserFooter()
{
	const float iconSize = 14.0f;
	const float sliderWidth = 120.0f;
	const float controlWidth = iconSize + ImGui::GetStyle().ItemSpacing.x +
		sliderWidth + ImGui::GetStyle().ItemSpacing.x + iconSize;
	const float x = (std::max)(0.0f, ImGui::GetContentRegionAvail().x - controlWidth);
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + x);
	const ImVec2 cursor = ImGui::GetCursorScreenPos();
	auto* drawList = ImGui::GetWindowDrawList();
	const auto iconColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
	drawList->AddRect(
		ImVec2(cursor.x + 2.0f, cursor.y + 4.0f),
		ImVec2(cursor.x + 11.0f, cursor.y + 13.0f),
		iconColor,
		1.0f);
	for (int row = 0; row < 3; ++row)
	{
		drawList->AddLine(
			ImVec2(cursor.x + 3.0f, cursor.y + 5.0f + row * 3.0f),
			ImVec2(cursor.x + 10.0f, cursor.y + 5.0f + row * 3.0f),
			iconColor);
	}
	ImGui::Dummy(ImVec2(iconSize, ImGui::GetFrameHeight()));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(sliderWidth);
	ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 6.0f);
	ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetColorU32(ImGuiCol_FrameBg));
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImGui::GetColorU32(ImGuiCol_FrameBgHovered));
	ImGui::PushStyleColor(ImGuiCol_SliderGrab, IM_COL32(73, 140, 224, 255));
	ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(98, 164, 244, 255));
	if (ImGui::SliderFloat("##AssetBrowserThumbnailSize", &m_thumbnailSize, 64.0f, 160.0f, ""))
	{
		m_visibleThumbnailItemsKnown = false;
		m_visibleThumbnailScopeKey.clear();
		m_thumbnailGenerationScopeDirty = true;
	}
	ImGui::PopStyleColor(4);
	ImGui::PopStyleVar();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Thumbnail Size");
	ImGui::SameLine();
	const ImVec2 gridCursor = ImGui::GetCursorScreenPos();
	const float cell = 5.0f;
	for (int y = 0; y < 2; ++y)
	{
		for (int xCell = 0; xCell < 2; ++xCell)
		{
			const ImVec2 min(gridCursor.x + xCell * (cell + 2.0f), gridCursor.y + 4.0f + y * (cell + 2.0f));
			drawList->AddRect(min, ImVec2(min.x + cell, min.y + cell), iconColor, 1.0f);
		}
	}
	ImGui::Dummy(ImVec2(iconSize, ImGui::GetFrameHeight()));
}

void Editor::Panels::AssetBrowser::DrawProjectBreadcrumb()
{
	for (size_t index = 0u; index < m_currentBreadcrumb.size(); ++index)
	{
		if (index > 0u)
		{
			ImGui::SameLine();
			ImGui::TextUnformatted(">");
			ImGui::SameLine();
		}

		const auto& segment = m_currentBreadcrumb[index];
		if (ImGui::SmallButton((segment.displayName + "##breadcrumb_" + segment.projectRelativePath).c_str()))
			SelectProjectFolder(segment.projectRelativePath);
	}
}

void Editor::Panels::AssetBrowser::RequestProjectBrowserTextDialog(
	const ProjectBrowserTextDialogKind kind,
	std::string title,
	const std::filesystem::path& targetAbsoluteFolder,
	std::string targetProjectRelativeFolder,
	const std::filesystem::path& sourceAbsolutePath,
	std::string defaultName)
{
	m_projectBrowserTextDialog = {};
	m_projectBrowserTextDialog.kind = kind;
	m_projectBrowserTextDialog.title = std::move(title);
	m_projectBrowserTextDialog.targetAbsoluteFolder = targetAbsoluteFolder.lexically_normal();
	m_projectBrowserTextDialog.targetProjectRelativeFolder = NormalizeProjectBrowserPath(std::move(targetProjectRelativeFolder));
	m_projectBrowserTextDialog.sourceAbsolutePath = sourceAbsolutePath.lexically_normal();
	m_projectBrowserTextDialog.requestOpen = true;
	defaultName = SanitizeAssetBrowserName(std::move(defaultName));
	const auto copyCount = (std::min)(
		defaultName.size(),
		m_projectBrowserTextDialog.buffer.size() - 1u);
	std::copy_n(defaultName.data(), copyCount, m_projectBrowserTextDialog.buffer.data());
}

void Editor::Panels::AssetBrowser::DrawProjectBrowserTextDialog()
{
	if (m_projectBrowserTextDialog.kind == ProjectBrowserTextDialogKind::None)
		return;

	if (m_projectBrowserTextDialog.requestOpen)
	{
		ImGui::OpenPopup(m_projectBrowserTextDialog.title.c_str());
		m_projectBrowserTextDialog.requestOpen = false;
	}

	bool close = false;
	if (ImGui::BeginPopupModal(
			m_projectBrowserTextDialog.title.c_str(),
			nullptr,
			ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::SetNextItemWidth(280.0f);
		const bool enterPressed = ImGui::InputText(
			"Name",
			m_projectBrowserTextDialog.buffer.data(),
			m_projectBrowserTextDialog.buffer.size(),
			ImGuiInputTextFlags_EnterReturnsTrue);
		if (enterPressed || ImGui::Button("OK"))
		{
			close = CommitProjectBrowserTextDialog();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
			close = true;

		if (close)
		{
			m_projectBrowserTextDialog = {};
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

bool Editor::Panels::AssetBrowser::CommitProjectBrowserTextDialog()
{
	const auto kind = m_projectBrowserTextDialog.kind;
	auto name = SanitizeAssetBrowserName(m_projectBrowserTextDialog.buffer.data());
	if (name.empty())
		return false;

	auto createdOrChangedProjectPath = std::filesystem::path {};
	auto targetFolder = m_projectBrowserTextDialog.targetAbsoluteFolder.lexically_normal();
	if (targetFolder.empty())
		targetFolder = (ProjectRootFromAssetsFolder(m_projectAssetFolder) / m_selectedProjectFolder).lexically_normal();

	auto makeScriptIdentifier = [](std::string value)
	{
		value.erase(
			std::remove_if(
				value.begin(),
				value.end(),
				[](const unsigned char character)
				{
					return !((character >= 'A' && character <= 'Z') ||
						(character >= 'a' && character <= 'z') ||
						(character >= '0' && character <= '9') ||
						character == '_');
				}),
			value.end());
		if (value.empty())
			value = "NewScript";
		if (value.front() >= '0' && value.front() <= '9')
			value.insert(value.begin(), '_');
		return value;
	};
	auto buildUniqueScriptPath = [&](const std::string& extension)
	{
		const auto scriptName = makeScriptIdentifier(name);
		size_t suffix = 0u;
		for (;; ++suffix)
		{
			const auto candidateName = suffix == 0u
				? scriptName
				: scriptName + "_" + std::to_string(suffix);
			const auto candidate = (targetFolder / (candidateName + extension)).lexically_normal();
			if (!std::filesystem::exists(candidate))
				return candidate;
		}
	};
	auto createScriptAsset = [&](const std::filesystem::path& finalPath, const std::string& contents)
	{
		std::error_code error;
		std::filesystem::create_directories(finalPath.parent_path(), error);
		if (error)
			return false;

		std::ofstream output(finalPath, std::ios::binary | std::ios::trunc);
		if (!output)
			return false;
		output << contents;
		output.flush();
		if (!output)
		{
			std::filesystem::remove(finalPath, error);
			return false;
		}
		output.close();

		auto meta = NLS::Core::Assets::AssetMeta::CreateForAsset(finalPath);
		if (!meta.Save(NLS::Core::Assets::GetAssetMetaPath(finalPath)))
		{
			std::filesystem::remove(finalPath, error);
			return false;
		}
		createdOrChangedProjectPath = EditorAssetPathFromAbsolutePath(m_projectAssetFolder, finalPath.string());
		return !createdOrChangedProjectPath.empty();
	};
	auto createMaterialAsset = [&](const std::string& contents)
	{
		const auto finalPath = BuildUniqueAssetPath(targetFolder, name, ".mat");
		const auto projectRelativePath = EditorAssetPathFromAbsolutePath(m_projectAssetFolder, finalPath.string());
		if (projectRelativePath.empty())
			return false;

		if (!CreateNativeMaterialAssetAtPath(m_projectAssetFolder, finalPath, contents))
			return false;

		createdOrChangedProjectPath = projectRelativePath;
		return true;
	};
	bool shouldPreimportCreatedAsset = true;

	switch (kind)
	{
	case ProjectBrowserTextDialogKind::RenameFolder:
	{
		const auto source = m_projectBrowserTextDialog.sourceAbsolutePath.lexically_normal();
		const auto destination = (source.parent_path() / name).lexically_normal();
		if (source == destination || std::filesystem::exists(destination))
			return false;
		RenameAsset(source.string(), EnsureTrailingPathSeparator(destination));
		EDITOR_EXEC(PropagateFolderRename(source.string(), EnsureTrailingPathSeparator(destination)));
		createdOrChangedProjectPath = EditorAssetFolderFromAbsolutePath(m_projectAssetFolder, destination.string());
		if (m_selectedProjectFolder == m_projectBrowserTextDialog.targetProjectRelativeFolder)
			m_selectedProjectFolder = NormalizeProjectBrowserPath(createdOrChangedProjectPath);
		break;
	}
	case ProjectBrowserTextDialogKind::RenameFile:
	{
		const auto source = m_projectBrowserTextDialog.sourceAbsolutePath.lexically_normal();
		const auto destination = (source.parent_path() / (name + source.extension().string())).lexically_normal();
		if (source == destination || std::filesystem::exists(destination))
			return false;
		RenameAsset(source.string(), destination.string());
		EDITOR_EXEC(PropagateFileRename(source.string(), destination.string()));
		if (EDITOR_CONTEXT(sceneManager).GetCurrentSceneSourcePath() == source.string())
			EDITOR_CONTEXT(sceneManager).StoreCurrentSceneSourcePath(destination.string());
		createdOrChangedProjectPath = EditorAssetPathFromAbsolutePath(m_projectAssetFolder, destination.string());
		break;
	}
	case ProjectBrowserTextDialogKind::CreateFolder:
	{
		const auto finalPath = BuildUniqueAssetPath(targetFolder, name, "");
		std::filesystem::create_directories(finalPath);
		createdOrChangedProjectPath = EditorAssetFolderFromAbsolutePath(m_projectAssetFolder, finalPath.string());
		break;
	}
	case ProjectBrowserTextDialogKind::CreateScene:
	{
		const auto finalPath = BuildUniqueAssetPath(targetFolder, name, ".scene");
		Engine::SceneSystem::Scene scene;
		if (!Engine::SceneSystem::SceneManager::SaveSceneToPath(scene, finalPath.string()))
		{
			NLS_LOG_ERROR("Failed to create scene asset: " + finalPath.string());
			return false;
		}
		createdOrChangedProjectPath = EditorAssetPathFromAbsolutePath(m_projectAssetFolder, finalPath.string());
		break;
	}
	case ProjectBrowserTextDialogKind::CreateCSharpScript:
	{
		const auto finalPath = buildUniqueScriptPath(".cs");
		const auto scriptName = makeScriptIdentifier(finalPath.stem().string());
		const auto contents = LoadScriptTemplate(
			m_editorAssetFolder,
			"CSharpBehaviour.cs.txt",
			scriptName);
		if (!contents.has_value())
		{
			NLS_LOG_ERROR("Failed to load C# script template from engine assets.");
			return false;
		}
		if (!createScriptAsset(finalPath, *contents))
			return false;
		break;
	}
	case ProjectBrowserTextDialogKind::CreateLuaScript:
	{
		const auto finalPath = buildUniqueScriptPath(".lua");
		const auto contents = LoadScriptTemplate(
			m_editorAssetFolder,
			"LuaBehaviour.lua.txt",
			finalPath.stem().string());
		if (!contents.has_value())
		{
			NLS_LOG_ERROR("Failed to load Lua script template from engine assets.");
			return false;
		}
		if (!createScriptAsset(finalPath, *contents))
			return false;
		break;
	}
	case ProjectBrowserTextDialogKind::CreateStandardShader:
	{
		const auto finalPath = BuildUniqueAssetPath(targetFolder, name, ".shader");
		std::filesystem::copy_file(
			EDITOR_CONTEXT(engineAssetsPath) + "Shaders\\ShaderLab\\StandardPBR.shader",
			finalPath,
			std::filesystem::copy_options::overwrite_existing);
		createdOrChangedProjectPath = EditorAssetPathFromAbsolutePath(m_projectAssetFolder, finalPath.string());
		break;
	}
	case ProjectBrowserTextDialogKind::CreateStandardPBRShader:
	{
		const auto finalPath = BuildUniqueAssetPath(targetFolder, name, ".shader");
		std::filesystem::copy_file(
			EDITOR_CONTEXT(engineAssetsPath) + "Shaders\\ShaderLab\\StandardPBR.shader",
			finalPath,
			std::filesystem::copy_options::overwrite_existing);
		createdOrChangedProjectPath = EditorAssetPathFromAbsolutePath(m_projectAssetFolder, finalPath.string());
		break;
	}
	case ProjectBrowserTextDialogKind::CreateUnlitShader:
	{
		const auto finalPath = BuildUniqueAssetPath(targetFolder, name, ".shader");
		std::filesystem::copy_file(
			EDITOR_CONTEXT(engineAssetsPath) + "Shaders\\ShaderLab\\UnlitColor.shader",
			finalPath,
			std::filesystem::copy_options::overwrite_existing);
		createdOrChangedProjectPath = EditorAssetPathFromAbsolutePath(m_projectAssetFolder, finalPath.string());
		break;
	}
	case ProjectBrowserTextDialogKind::CreateUnlitTextureShader:
	{
		const auto finalPath = BuildUniqueAssetPath(targetFolder, name, ".shader");
		std::filesystem::copy_file(
			EDITOR_CONTEXT(engineAssetsPath) + "Shaders\\ShaderLab\\UnlitTexture.shader",
			finalPath,
			std::filesystem::copy_options::overwrite_existing);
		createdOrChangedProjectPath = EditorAssetPathFromAbsolutePath(m_projectAssetFolder, finalPath.string());
		break;
	}
	case ProjectBrowserTextDialogKind::CreateEmptyMaterial:
		if (!createMaterialAsset(BuildDefaultAssetBrowserMaterialPayload()))
			return false;
		shouldPreimportCreatedAsset = false;
		break;
	case ProjectBrowserTextDialogKind::CreateStandardMaterial:
	case ProjectBrowserTextDialogKind::CreateStandardPBRMaterial:
		if (!createMaterialAsset(BuildDefaultAssetBrowserMaterialPayload()))
			return false;
		shouldPreimportCreatedAsset = false;
		break;
	case ProjectBrowserTextDialogKind::CreateUnlitMaterial:
		if (!createMaterialAsset(BuildDefaultAssetBrowserMaterialPayload()))
			return false;
		shouldPreimportCreatedAsset = false;
		break;
	case ProjectBrowserTextDialogKind::CreateDefaultSurfaceMaterial:
		if (!createMaterialAsset(BuildDefaultAssetBrowserMaterialPayload()))
			return false;
		shouldPreimportCreatedAsset = false;
		break;
	case ProjectBrowserTextDialogKind::None:
	default:
		return false;
	}

	if (!createdOrChangedProjectPath.empty() && shouldPreimportCreatedAsset)
		ScheduleProjectAssetPreimportForPath(createdOrChangedProjectPath);
	RebuildProjectAssetPresentationAfterWorkflow();
	return true;
}

void Editor::Panels::AssetBrowser::BeginInlineRenameProjectItem(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	const auto capabilities = NLS::Editor::Assets::BuildAssetBrowserWorkflowCapabilities(item);
	if (!capabilities.canRename || item.kind == NLS::Editor::Assets::AssetBrowserItemKind::GeneratedSubAsset)
		return;

	m_projectBrowserInlineRename = {};
	m_projectBrowserInlineRename.active = true;
	m_projectBrowserInlineRename.focusRequested = true;
	m_projectBrowserInlineRename.kind = item.kind;
	m_projectBrowserInlineRename.sourceProjectRelativePath = item.projectRelativePath;
	m_projectBrowserInlineRename.sourceAbsolutePath = item.absolutePath.lexically_normal();
	m_projectBrowserInlineRename.targetAbsoluteFolder = item.absolutePath.parent_path().lexically_normal();
	m_projectBrowserInlineRename.targetProjectRelativeFolder = NormalizeProjectBrowserPath(m_selectedProjectFolder);
	auto defaultName = item.kind == NLS::Editor::Assets::AssetBrowserItemKind::Folder
		? item.absolutePath.filename().generic_string()
		: item.absolutePath.stem().generic_string();
	defaultName = SanitizeAssetBrowserName(std::move(defaultName));
	const auto copyCount = (std::min)(
		defaultName.size(),
		m_projectBrowserInlineRename.buffer.size() - 1u);
	std::copy_n(defaultName.data(), copyCount, m_projectBrowserInlineRename.buffer.data());
}

void Editor::Panels::AssetBrowser::CancelInlineRenameProjectItem()
{
	m_projectBrowserInlineRename = {};
}

bool Editor::Panels::AssetBrowser::CommitInlineRenameProjectItem()
{
	if (!m_projectBrowserInlineRename.active)
		return false;

	NLS::Editor::Assets::AssetBrowserItem item;
	item.projectRelativePath = m_projectBrowserInlineRename.sourceProjectRelativePath;
	item.absolutePath = m_projectBrowserInlineRename.sourceAbsolutePath;
	item.kind = m_projectBrowserInlineRename.kind;
	const bool renamed = RenameProjectItem(item, m_projectBrowserInlineRename.buffer.data());
	if (renamed)
		m_projectBrowserInlineRename = {};
	return renamed;
}

void Editor::Panels::AssetBrowser::DrawProjectGridItemInlineRename(
	const NLS::Editor::Assets::AssetBrowserItem& item,
	const ImVec2& labelMin,
	const ImVec2& labelMax)
{
	if (!m_projectBrowserInlineRename.active ||
		m_projectBrowserInlineRename.sourceProjectRelativePath != item.projectRelativePath)
	{
		return;
	}

	constexpr float padding = 2.0f;
	ImGui::SetCursorScreenPos(ImVec2(labelMin.x + padding, labelMin.y + padding));
	ImGui::SetNextItemWidth((std::max)(1.0f, labelMax.x - labelMin.x - padding * 2.0f));
	if (m_projectBrowserInlineRename.focusRequested)
	{
		ImGui::SetKeyboardFocusHere();
		m_projectBrowserInlineRename.focusRequested = false;
	}

	const bool enterPressed = ImGui::InputText(
		"##ProjectAssetInlineRename",
		m_projectBrowserInlineRename.buffer.data(),
		m_projectBrowserInlineRename.buffer.size(),
		ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
	const bool escapePressed = ImGui::IsKeyPressed(ImGuiKey_Escape);
	const bool deactivated = ImGui::IsItemDeactivatedAfterEdit();

	if (escapePressed)
	{
		CancelInlineRenameProjectItem();
		return;
	}
	if (enterPressed || deactivated)
		(void)CommitInlineRenameProjectItem();
}

bool Editor::Panels::AssetBrowser::RenameProjectItem(
	const NLS::Editor::Assets::AssetBrowserItem& item,
	const std::string& newName)
{
	auto name = SanitizeAssetBrowserName(newName);
	if (name.empty() || item.absolutePath.empty())
		return false;

	const auto source = item.absolutePath.lexically_normal();
	if (item.kind == NLS::Editor::Assets::AssetBrowserItemKind::Folder)
	{
		const auto destination = (source.parent_path() / name).lexically_normal();
		if (source == destination)
			return false;
		if (std::filesystem::exists(destination))
		{
			NLS::Dialogs::MessageBox message(
				"Folder already exists",
				"A folder with that name already exists in this location.",
				NLS::Dialogs::MessageBox::EMessageType::ERROR,
				NLS::Dialogs::MessageBox::EButtonLayout::OK);
			return false;
		}
		RenameAsset(source.string(), EnsureTrailingPathSeparator(destination));
		EDITOR_EXEC(PropagateFolderRename(source.string(), EnsureTrailingPathSeparator(destination)));
		const auto changedProjectPath = EditorAssetFolderFromAbsolutePath(m_projectAssetFolder, destination.string());
		if (m_selectedProjectFolder == item.projectRelativePath)
			m_selectedProjectFolder = NormalizeProjectBrowserPath(changedProjectPath);
		auto renamedItem = item;
		renamedItem.projectRelativePath = NormalizeProjectBrowserPath(changedProjectPath);
		m_selectedProjectItem = NLS::Editor::Assets::BuildAssetBrowserActionIdentity(renamedItem);
		ScheduleProjectAssetPreimportForPath(changedProjectPath);
		RebuildProjectAssetPresentationAfterWorkflow();
		return true;
	}

	const auto destination = (source.parent_path() / (name + source.extension().string())).lexically_normal();
	if (source == destination)
		return false;
	if (std::filesystem::exists(destination))
	{
		NLS::Dialogs::MessageBox message(
			"File already exists",
			"A file with that name already exists in this location.",
			NLS::Dialogs::MessageBox::EMessageType::ERROR,
			NLS::Dialogs::MessageBox::EButtonLayout::OK);
		return false;
	}
	RenameAsset(source.string(), destination.string());
	EDITOR_EXEC(PropagateFileRename(source.string(), destination.string()));
	if (EDITOR_CONTEXT(sceneManager).GetCurrentSceneSourcePath() == source.string())
		EDITOR_CONTEXT(sceneManager).StoreCurrentSceneSourcePath(destination.string());
	const auto changedProjectPath = EditorAssetPathFromAbsolutePath(m_projectAssetFolder, destination.string());
	auto renamedItem = item;
	renamedItem.projectRelativePath = NormalizeProjectBrowserPath(changedProjectPath);
	renamedItem.sourceAssetPath = renamedItem.projectRelativePath;
	m_selectedProjectItem = NLS::Editor::Assets::BuildAssetBrowserActionIdentity(renamedItem);
	ScheduleProjectAssetPreimportForPath(changedProjectPath);
	RebuildProjectAssetPresentationAfterWorkflow();
	return true;
}

void Editor::Panels::AssetBrowser::ClearProjectAssetClipboard()
{
	m_projectAssetClipboardPaths.clear();
	m_projectAssetClipboardCut = false;
}

void Editor::Panels::AssetBrowser::CopySelectedProjectItemToClipboard()
{
	ClearProjectAssetClipboard();
	if (!m_selectedProjectItem.has_value())
		return;

	const auto selected = std::find_if(
		m_currentFolderItems.begin(),
		m_currentFolderItems.end(),
		[this](const auto& item)
		{
			return m_selectedProjectItem.has_value() &&
				NLS::Editor::Assets::AssetBrowserItemMatchesActionIdentity(item, *m_selectedProjectItem);
		});
	if (selected == m_currentFolderItems.end() ||
		selected->kind == NLS::Editor::Assets::AssetBrowserItemKind::GeneratedSubAsset)
	{
		return;
	}

	const auto capabilities = NLS::Editor::Assets::BuildAssetBrowserWorkflowCapabilities(*selected);
	if (!capabilities.canDuplicate)
		return;

	const auto source = selected->absolutePath.lexically_normal();
	if (!std::filesystem::exists(source))
		return;

	m_projectAssetClipboardPaths.push_back(source);
	m_projectAssetClipboardCut = false;
	ImGui::SetClipboardText(source.string().c_str());
}

bool Editor::Panels::AssetBrowser::DuplicateProjectItem(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	const auto capabilities = NLS::Editor::Assets::BuildAssetBrowserWorkflowCapabilities(item);
	if (!capabilities.canDuplicate || item.absolutePath.empty())
		return false;

	const auto source = item.absolutePath.lexically_normal();
	if (!std::filesystem::exists(source))
		return false;

	const auto destination = BuildUniqueAssetPath(
		source.parent_path(),
		std::filesystem::is_directory(source) ? source.filename().string() : source.stem().string(),
		std::filesystem::is_directory(source) ? "" : source.extension().string());
	bool copied = false;
	if (std::filesystem::is_directory(source))
		copied = CopyAssetFolderRecursively(source, destination);
	else if (std::filesystem::is_regular_file(source))
		copied = CopyAssetFileWithMeta(source, destination);
	if (!copied)
		return false;

	ScheduleProjectAssetPreimportForPath(EditorAssetPathFromAbsolutePath(m_projectAssetFolder, destination.string()));
	RebuildProjectAssetPresentationAfterWorkflow();
	return true;
}

bool Editor::Panels::AssetBrowser::ImportExternalFilesIntoCurrentFolder(
	const std::vector<std::filesystem::path>& sourcePaths)
{
	if (sourcePaths.empty())
		return false;

	const auto destinationFolder = ProjectBrowserAbsolutePathForResourcePath(
		m_projectAssetFolder,
		m_selectedProjectFolder);
	if (destinationFolder.empty())
		return false;

	std::error_code error;
	std::filesystem::create_directories(destinationFolder, error);
	if (error)
		return false;

	const auto projectAssetsRoot = std::filesystem::path(m_projectAssetFolder).lexically_normal();
	bool changed = false;
	for (const auto& rawSource : sourcePaths)
	{
		auto source = rawSource.lexically_normal();
		if (source.empty() || !std::filesystem::exists(source))
			continue;

		const bool isDirectory = std::filesystem::is_directory(source);
		const bool isFile = std::filesystem::is_regular_file(source);
		if (!isDirectory && !isFile)
			continue;

		const bool sourceInsideProjectAssets = IsPathInsideOrEqual(source, projectAssetsRoot);
		if (isDirectory && IsPathInsideOrEqual(destinationFolder, source))
			continue;
		const auto destination = BuildUniqueAssetPath(
			destinationFolder,
			isDirectory ? source.filename().string() : source.stem().string(),
			isDirectory ? "" : source.extension().string());

		bool copied = false;
		if (isDirectory)
			copied = sourceInsideProjectAssets
				? CopyAssetFolderRecursively(source, destination)
				: CopyAssetFolderRecursivelyWithoutMeta(source, destination);
		else if (sourceInsideProjectAssets)
			copied = CopyAssetFileWithMeta(source, destination);
		else
		{
			std::filesystem::copy_file(
				source,
				destination,
				std::filesystem::copy_options::overwrite_existing,
				error);
			copied = !error;
			error.clear();
		}

		if (!copied)
			continue;

		ScheduleProjectAssetPreimportForPath(EditorAssetPathFromAbsolutePath(m_projectAssetFolder, destination.string()));
		changed = true;
	}

	if (changed)
		RebuildProjectAssetPresentationAfterWorkflow();
	return changed;
}

bool Editor::Panels::AssetBrowser::PasteClipboardIntoCurrentFolder()
{
	std::vector<std::filesystem::path> paths = m_projectAssetClipboardPaths;

#ifdef _WIN32
	if (paths.empty())
		paths = ReadWindowsClipboardFilePaths();
#endif

	if (paths.empty())
	{
		if (const char* clipboard = ImGui::GetClipboardText(); clipboard != nullptr)
			paths = ParseClipboardPathText(clipboard);
	}

	if (paths.empty())
		return false;
	return ImportExternalFilesIntoCurrentFolder(paths);
}

bool Editor::Panels::AssetBrowser::DeleteProjectItem(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	const double now = ImGui::GetCurrentContext() != nullptr ? ImGui::GetTime() : 0.0;
	if (m_projectDeleteActionAwaitingRelease || now < m_projectDeleteActionSuppressedUntil)
		return false;

	const auto capabilities = NLS::Editor::Assets::BuildAssetBrowserWorkflowCapabilities(item);
	if (!capabilities.canDelete || item.kind == NLS::Editor::Assets::AssetBrowserItemKind::GeneratedSubAsset)
		return false;

	m_projectDeleteActionAwaitingRelease = true;
	m_projectDeleteActionSuppressedUntil = now + 0.20;

	using namespace NLS::Dialogs;
	const bool isFolder = item.kind == NLS::Editor::Assets::AssetBrowserItemKind::Folder;
	MessageBox message(
		isFolder ? "Delete folder" : "Delete file",
		"Deleting this asset is irreversible, are you sure that you want to delete \"" + item.absolutePath.string() + "\"?",
		MessageBox::EMessageType::WARNING,
		MessageBox::EButtonLayout::YES_NO);
	if (message.GetUserAction() != MessageBox::EUserAction::YES)
		return false;

	const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
	std::vector<std::filesystem::path> thumbnailPathsToRelease;
	std::optional<NLS::Editor::Assets::AssetThumbnailRequest> deletedThumbnailRequest;
	if (!isFolder && item.assetId.IsValid())
	{
		auto requestContext = MakeAssetBrowserThumbnailRequestBuildContext(m_thumbnailFeatureConfig);
		requestContext.assetDatabaseSnapshot = m_projectAssetDatabaseSnapshot;
		requestContext.residentPrefabPreviewRegistry = m_residentPrefabPreviewRegistry;
		deletedThumbnailRequest = NLS::Editor::Assets::BuildAssetThumbnailRequestForItem(
			projectRoot,
			item,
			AssetBrowserThumbnailRequestSize(m_thumbnailSize),
			requestContext);
		if (deletedThumbnailRequest.has_value())
		{
			if (const auto presentation = NLS::Editor::Assets::ReadAssetThumbnailPresentationIndex(
					*deletedThumbnailRequest);
				presentation.has_value())
			{
				if (presentation->current.has_value())
					thumbnailPathsToRelease.push_back(presentation->current->imagePath);
				if (presentation->previous.has_value())
					thumbnailPathsToRelease.push_back(presentation->previous->imagePath);
			}
			if (const auto currentEntry = NLS::Editor::Assets::ResolveAssetThumbnailCacheEntry(
					*deletedThumbnailRequest);
				currentEntry.has_value())
			{
				thumbnailPathsToRelease.push_back(currentEntry->imagePath);
			}
		}
	}

	if (isFolder)
	{
		EDITOR_EXEC(PropagateFolderDestruction(item.absolutePath.string()));
		std::filesystem::remove_all(item.absolutePath);
		m_thumbnailService.ClearQueuedRequests();
		(void)NLS::Editor::Assets::RemoveAssetThumbnailCachesForSourcePath(
			projectRoot,
			m_thumbnailFeatureConfig.cacheRoot,
			item.projectRelativePath,
			true);
		if (IsProjectBrowserAncestorOf(item.projectRelativePath, m_selectedProjectFolder))
			m_selectedProjectFolder = "Assets";
	}
	else
	{
		RemoveAsset(item.absolutePath.string());
		EDITOR_EXEC(PropagateFileRename(item.absolutePath.string(), "?"));
		if (deletedThumbnailRequest.has_value())
		{
			m_thumbnailService.InvalidateThumbnail(*deletedThumbnailRequest);
			(void)NLS::Editor::Assets::RemoveAssetThumbnailCachesForSourcePath(
				projectRoot,
				m_thumbnailFeatureConfig.cacheRoot,
				item.sourceAssetPath.empty() ? item.projectRelativePath : item.sourceAssetPath,
				false);
		}
		if (EDITOR_CONTEXT(sceneManager).GetCurrentSceneSourcePath() == item.absolutePath.string())
			EDITOR_CONTEXT(sceneManager).ForgetCurrentSceneSourcePath();
	}
	for (const auto& path : thumbnailPathsToRelease)
	{
		if (!path.empty())
			ReleaseCachedThumbnailTexture(path.lexically_normal().generic_string());
	}

	if (m_selectedProjectItem.has_value() &&
		NLS::Editor::Assets::AssetBrowserItemMatchesActionIdentity(item, *m_selectedProjectItem))
	{
		m_selectedProjectItem.reset();
	}
	if (m_projectBrowserInlineRename.sourceProjectRelativePath == item.projectRelativePath)
		CancelInlineRenameProjectItem();
	ScheduleProjectAssetPreimportForPath(item.projectRelativePath);
	RebuildProjectAssetPresentationAfterWorkflow();
	return true;
}

void Editor::Panels::AssetBrowser::DrawProjectFolderContextMenu(
	const std::string& popupId,
	const std::string& projectRelativeFolder,
	const std::filesystem::path& absoluteFolder)
{
	const auto scopedPopupId = popupId + "_" + NormalizeProjectBrowserPath(projectRelativeFolder);
	if (!ImGui::BeginPopupContextItem(scopedPopupId.c_str()))
		return;

	DrawProjectCurrentFolderContextMenu("##folderContent", projectRelativeFolder, absoluteFolder);

	const bool isAssetsRoot = NormalizeProjectBrowserPath(projectRelativeFolder) == "Assets";
	if (ImGui::MenuItem("Rename...", nullptr, false, !isAssetsRoot))
	{
		NLS::Editor::Assets::AssetBrowserItem item;
		item.projectRelativePath = NormalizeProjectBrowserPath(projectRelativeFolder);
		item.absolutePath = absoluteFolder;
		item.kind = NLS::Editor::Assets::AssetBrowserItemKind::Folder;
		BeginInlineRenameProjectItem(item);
	}

	if (ImGui::MenuItem("Delete", nullptr, false, !isAssetsRoot))
	{
		NLS::Editor::Assets::AssetBrowserItem item;
		item.projectRelativePath = NormalizeProjectBrowserPath(projectRelativeFolder);
		item.absolutePath = absoluteFolder;
		item.kind = NLS::Editor::Assets::AssetBrowserItemKind::Folder;
		(void)DeleteProjectItem(item);
	}

	ImGui::EndPopup();
}

void Editor::Panels::AssetBrowser::DrawProjectCurrentFolderContextMenu(
	const std::string& popupId,
	const std::string& projectRelativeFolder,
	const std::filesystem::path& absoluteFolder)
{
	(void)popupId;

	if (ImGui::MenuItem("Show in explorer"))
		Platform::SystemCalls::ShowInExplorer(EnsureTrailingPathSeparator(absoluteFolder));

	if (ImGui::MenuItem("Import Here..."))
	{
		if (EDITOR_EXEC(ImportAssetAtLocation(EnsureTrailingPathSeparator(absoluteFolder))))
			RebuildProjectAssetPresentationAfterWorkflow();
	}

	if (ImGui::BeginMenu("Create"))
	{
		if (ImGui::MenuItem("Folder"))
		{
			RequestProjectBrowserTextDialog(
				ProjectBrowserTextDialogKind::CreateFolder,
				"Create Folder",
				absoluteFolder,
				projectRelativeFolder,
				{},
				"New Folder");
		}
		if (ImGui::MenuItem("Scene"))
		{
			RequestProjectBrowserTextDialog(
				ProjectBrowserTextDialogKind::CreateScene,
				"Create Scene",
				absoluteFolder,
				projectRelativeFolder,
				{},
				"New Scene");
		}
		if (ImGui::MenuItem("C# Script"))
		{
			RequestProjectBrowserTextDialog(
				ProjectBrowserTextDialogKind::CreateCSharpScript,
				"Create C# Script",
				absoluteFolder,
				projectRelativeFolder,
				{},
				"NewScript");
		}
		if (ImGui::MenuItem("Lua Script"))
		{
			RequestProjectBrowserTextDialog(
				ProjectBrowserTextDialogKind::CreateLuaScript,
				"Create Lua Script",
				absoluteFolder,
				projectRelativeFolder,
				{},
				"NewScript");
		}
		if (ImGui::BeginMenu("Shader"))
		{
			if (ImGui::MenuItem("Standard template"))
				RequestProjectBrowserTextDialog(ProjectBrowserTextDialogKind::CreateStandardShader, "Create Standard Shader", absoluteFolder, projectRelativeFolder, {}, "New Shader");
			if (ImGui::MenuItem("Standard PBR template"))
				RequestProjectBrowserTextDialog(ProjectBrowserTextDialogKind::CreateStandardPBRShader, "Create Standard PBR Shader", absoluteFolder, projectRelativeFolder, {}, "New Shader");
			if (ImGui::MenuItem("Unlit template"))
				RequestProjectBrowserTextDialog(ProjectBrowserTextDialogKind::CreateUnlitShader, "Create Unlit Shader", absoluteFolder, projectRelativeFolder, {}, "New Shader");
			if (ImGui::MenuItem("Unlit Texture template"))
				RequestProjectBrowserTextDialog(ProjectBrowserTextDialogKind::CreateUnlitTextureShader, "Create Unlit Texture Shader", absoluteFolder, projectRelativeFolder, {}, "New Shader");
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("Material"))
		{
			if (ImGui::MenuItem("Empty"))
				RequestProjectBrowserTextDialog(ProjectBrowserTextDialogKind::CreateEmptyMaterial, "Create Empty Material", absoluteFolder, projectRelativeFolder, {}, "New Material");
			if (ImGui::MenuItem("Standard"))
				RequestProjectBrowserTextDialog(ProjectBrowserTextDialogKind::CreateStandardMaterial, "Create Standard Material", absoluteFolder, projectRelativeFolder, {}, "New Material");
			if (ImGui::MenuItem("Standard PBR"))
				RequestProjectBrowserTextDialog(ProjectBrowserTextDialogKind::CreateStandardPBRMaterial, "Create Standard PBR Material", absoluteFolder, projectRelativeFolder, {}, "New Material");
			if (ImGui::MenuItem("Unlit"))
				RequestProjectBrowserTextDialog(ProjectBrowserTextDialogKind::CreateUnlitMaterial, "Create Unlit Material", absoluteFolder, projectRelativeFolder, {}, "New Material");
			if (ImGui::MenuItem("Default Surface"))
				RequestProjectBrowserTextDialog(ProjectBrowserTextDialogKind::CreateDefaultSurfaceMaterial, "Create Default Surface Material", absoluteFolder, projectRelativeFolder, {}, "New Material");
			ImGui::EndMenu();
		}
		ImGui::EndMenu();
	}
}

void Editor::Panels::AssetBrowser::DrawProjectGridItemContextMenu(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	NLS_PROFILE_NAMED_SCOPE("AssetBrowser::DrawProjectGridItemContextMenu");
	const auto capabilities = NLS::Editor::Assets::BuildAssetBrowserWorkflowCapabilities(item);
	if (item.kind == NLS::Editor::Assets::AssetBrowserItemKind::Folder)
	{
		DrawProjectFolderContextMenu(
			"##gridFolderContext",
			item.projectRelativePath,
			item.absolutePath);
		return;
	}

	const auto scopedPopupId = std::string("##gridItemContext_") + item.projectRelativePath;
	if (!ImGui::BeginPopupContextItem(scopedPopupId.c_str()))
		return;

	if (capabilities.canOpenExternal && ImGui::MenuItem("Open"))
		Platform::SystemCalls::OpenFile(item.absolutePath.string());
	if (capabilities.canEdit && ImGui::MenuItem("Edit"))
		OpenProjectGridItem(item);
	if (capabilities.canPreview && ImGui::MenuItem("Preview"))
		PreviewProjectGridItem(item);
	if (capabilities.canReimport && ImGui::MenuItem("Reimport"))
		ReimportProjectAssetAsync(m_projectAssetFolder, item.absolutePath.string());
	if (capabilities.canReload && ImGui::MenuItem("Reload"))
	{
		const auto resourcePath = ProjectBrowserResourcePathForItem(item);
		if (item.type == NLS::Editor::Assets::AssetBrowserItemType::Texture)
		{
			auto& textureManager = NLS_SERVICE(NLS::Core::ResourceManagement::TextureManager);
			if (textureManager.IsResourceRegistered(resourcePath))
			{
				textureManager.AResourceManager::ReloadResource(resourcePath);
				EDITOR_PANEL(Editor::Panels::MaterialEditor, "Material Editor").Refresh();
			}
		}
		else if (item.type == NLS::Editor::Assets::AssetBrowserItemType::Material)
		{
			auto& materialManager = NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager);
			if (materialManager[resourcePath] != nullptr)
			{
				materialManager.AResourceManager::ReloadResource(resourcePath);
				EDITOR_PANEL(Editor::Panels::MaterialEditor, "Material Editor").Refresh();
			}
		}
	}
	if (capabilities.canCompile && ImGui::MenuItem("Compile"))
	{
		const auto resourcePath = ProjectBrowserResourcePathForItem(item);
		auto& shaderManager = NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager);
		if (shaderManager.IsResourceRegistered(resourcePath))
		{
			Render::Resources::Loaders::ShaderLoader::Recompile(
				*shaderManager[resourcePath],
				item.absolutePath.string(),
				NLS::Core::ResourceManagement::ShaderManager::ProjectAssetsRoot());
		}
		else if (NLS_SERVICE(NLS::Core::ResourceManagement::ShaderManager)[resourcePath] != nullptr)
		{
			NLS_LOG_INFO("[COMPILE] \"" + item.absolutePath.string() + "\": Success!");
		}
	}
	if (capabilities.canDuplicate && ImGui::MenuItem("Duplicate"))
		(void)DuplicateProjectItem(item);
	if (capabilities.canRename && ImGui::MenuItem("Rename..."))
		BeginInlineRenameProjectItem(item);
	if (capabilities.canDelete && ImGui::MenuItem("Delete"))
		(void)DeleteProjectItem(item);
	if (capabilities.canOpenProperties && ImGui::MenuItem("Properties"))
		OpenProjectGridItemProperties(item);

	ImGui::EndPopup();
}

void Editor::Panels::AssetBrowser::DrawCurrentFolderGrid()
{
	NLS_PROFILE_NAMED_SCOPE("AssetBrowser::DrawCurrentFolderGrid");
	RebuildProjectAssetDisplayItemsIfNeeded();
	const auto& displayItems = m_projectDisplayItems;
	if (NLS::Editor::Assets::ResolveAssetBrowserContentViewMode(m_thumbnailSize) ==
		NLS::Editor::Assets::AssetBrowserContentViewMode::List)
	{
		DrawCurrentFolderList(displayItems);
		return;
	}
	const std::string drawTelemetryPath = m_selectedProjectFolder + "|grid";
	size_t drawTelemetryItemCount = displayItems.size();
	std::vector<NLS::Editor::Assets::AssetBrowserItem> visibleThumbnailItems;
	std::chrono::microseconds visibleRowsElapsed {};
	std::chrono::microseconds interactionsElapsed {};
	std::chrono::microseconds thumbnailElapsed {};
	std::chrono::microseconds labelElapsed {};
	std::chrono::microseconds visibleSetElapsed {};
	std::chrono::microseconds generationScopeElapsed {};
	{
		AssetBrowserArtifactTelemetryScope drawTelemetry {
			NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDraw,
			std::chrono::steady_clock::now(),
			&drawTelemetryPath,
			&drawTelemetryItemCount,
			kAssetBrowserThumbnailUiDrawTelemetryMinimum
		};

		const float cellWidth = (std::max)(96.0f, m_thumbnailSize + 28.0f);
		const float availableWidth = ImGui::GetContentRegionAvail().x;
	const int columns = (std::max)(1, static_cast<int>(std::floor(availableWidth / cellWidth)));
	const float thumbnailSize = (std::max)(64.0f, m_thumbnailSize);
	const float labelHeight = 24.0f;
	const float labelGap = 4.0f;
	const ImVec2 cardSize(cellWidth - 8.0f, thumbnailSize + labelGap + labelHeight + 8.0f);

	if (displayItems.empty())
	{
		SetVisibleThumbnailItems({});
		UpdateThumbnailGenerationScope();
		if (m_currentFolderItemsRefresh.has_value())
		{
			auto* drawList = ImGui::GetWindowDrawList();
			const auto placeholderRows = (std::max)(2, static_cast<int>(std::ceil(ImGui::GetContentRegionAvail().y / (cardSize.y + ImGui::GetStyle().ItemSpacing.y))));
			const auto placeholderCount = (std::min)(columns * placeholderRows, columns * 5);
			ImGui::Columns(columns, "##AssetBrowserFolderLoadingGrid", false);
			for (int index = 0; index < placeholderCount; ++index)
			{
				ImGui::PushID(index);
				const ImVec2 cursor = ImGui::GetCursorScreenPos();
				ImGui::InvisibleButton("##assetFolderLoadingPlaceholder", cardSize);
				const float placeholderSize = thumbnailSize;
				const ImVec2 iconMin(
					cursor.x + (cardSize.x - placeholderSize) * 0.5f,
					cursor.y + 4.0f);
				const ImVec2 iconMax(iconMin.x + placeholderSize, iconMin.y + placeholderSize);
				drawList->AddRectFilled(iconMin, iconMax, IM_COL32(42, 46, 50, 92), 3.0f);
				drawList->AddRectFilled(
					ImVec2(cursor.x + 10.0f, iconMax.y + labelGap + 7.0f),
					ImVec2(cursor.x + cardSize.x - 10.0f, iconMax.y + labelGap + 17.0f),
					IM_COL32(42, 46, 50, 76),
					2.0f);
				ImGui::NextColumn();
				ImGui::PopID();
			}
			ImGui::Columns(1);
		}
		else
		{
			ImGui::TextDisabled("This folder is empty");
		}
		return;
		}

		const auto itemCount = static_cast<int>(displayItems.size());
		const auto rowCount = (itemCount + columns - 1) / columns;
		visibleThumbnailItems.reserve(static_cast<size_t>(columns) * 8u);
		struct DeferredDisclosureButton
		{
		ImDrawList* drawList = nullptr;
		ImVec2 center;
		float radius = 0.0f;
		bool expanded = false;
		bool hovered = false;
	};
	std::vector<DeferredDisclosureButton> deferredDisclosureButtons;
	deferredDisclosureButtons.reserve(static_cast<size_t>(columns) * 8u);
	ImGuiListClipper clipper;
	clipper.Begin(rowCount, cardSize.y + ImGui::GetStyle().ItemSpacing.y);
	ImGui::Columns(columns, "##AssetBrowserGrid", false);
	while (clipper.Step())
	{
		AssetBrowserArtifactTelemetryAccumulatorScope visibleRowsTelemetry {
			visibleRowsElapsed
		};
		NLS_PROFILE_NAMED_SCOPE("AssetBrowser::DrawCurrentFolderGrid.VisibleRows");
		for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
		{
			const auto rowPresentationGeneration = m_projectAssetPresentationGeneration;
			const auto rowBegin = static_cast<size_t>(row) * static_cast<size_t>(columns);
			const ImVec2 rowCursor = ImGui::GetCursorScreenPos();
			auto* rowDrawList = ImGui::GetWindowDrawList();
			const auto rowSegments = NLS::Editor::Assets::ResolveAssetBrowserGridRowGroupSegments(
				displayItems,
				rowBegin,
				static_cast<size_t>(columns));
			const ImRect parentClipRect = ImGui::GetCurrentWindow()->DC.CurrentColumns->HostInitialClipRect;
			ImGui::PushColumnsBackground();
			rowDrawList->PushClipRect(parentClipRect.Min, parentClipRect.Max, true);
			for (const auto& segment : rowSegments)
			{
				const int firstColumn = static_cast<int>(segment.range.begin - rowBegin);
				const int lastColumnExclusive = firstColumn + static_cast<int>(segment.range.count);
				const float segmentMinX =
					ImGui::GetWindowPos().x + ImGui::GetColumnOffset(firstColumn) + 4.0f;
				const float segmentMaxX =
					ImGui::GetWindowPos().x + ImGui::GetColumnOffset(lastColumnExclusive) - 4.0f;
				const float generatedThumbnailSize = (std::max)(48.0f, thumbnailSize - 18.0f);
				ImDrawFlags cornerFlags = ImDrawFlags_RoundCornersNone;
				if (segment.trueSegmentStart)
					cornerFlags |= ImDrawFlags_RoundCornersLeft;
				if (segment.trueSegmentEnd)
					cornerFlags |= ImDrawFlags_RoundCornersRight;
				DrawAssetBrowserSegmentPanel(
					rowDrawList,
					ImVec2(segmentMinX, rowCursor.y + 3.0f),
					ImVec2(
						segmentMaxX,
						rowCursor.y + 8.0f + (thumbnailSize + generatedThumbnailSize) * 0.5f),
					false,
					cornerFlags);
			}
			rowDrawList->PopClipRect();
			ImGui::PopColumnsBackground();
			for (int column = 0; column < columns; ++column)
			{
				if (rowPresentationGeneration != m_projectAssetPresentationGeneration)
				{
					ImGui::Columns(1);
					return;
				}
				const auto presentationGeneration = m_projectAssetPresentationGeneration;
				const auto itemIndex = row * columns + column;
				if (itemIndex >= itemCount)
				{
					ImGui::NextColumn();
					continue;
				}

				const auto& displayItem = displayItems[static_cast<size_t>(itemIndex)];
				const auto& item = displayItem.item;
				if (displayItem.loadingPlaceholder)
					ImGui::PushID(itemIndex);
				else
					ImGui::PushID(item.projectRelativePath.c_str());

				const bool selected = m_selectedProjectItem.has_value() &&
					NLS::Editor::Assets::AssetBrowserItemMatchesActionIdentity(item, *m_selectedProjectItem);
				const ImVec2 cursor = ImGui::GetCursorScreenPos();
				auto* drawList = ImGui::GetWindowDrawList();
				if (displayItem.loadingPlaceholder)
				{
					ImGui::InvisibleButton("##assetPlaceholder", cardSize);
					const float placeholderSize = displayItem.subAsset
						? (std::max)(48.0f, thumbnailSize - 18.0f)
						: thumbnailSize;
					const ImVec2 placeholderMin(
						cursor.x + (cardSize.x - placeholderSize) * 0.5f,
						cursor.y + 4.0f + (thumbnailSize - placeholderSize) * 0.5f);
					const ImVec2 placeholderMax(placeholderMin.x + placeholderSize, placeholderMin.y + placeholderSize);
					drawList->AddRectFilled(
						placeholderMin,
						placeholderMax,
						IM_COL32(42, 46, 50, 120),
						3.0f);
					ImGui::NextColumn();
					ImGui::PopID();
					continue;
				}
				visibleThumbnailItems.push_back(item);
				const float childIndent = displayItem.subAsset ? 14.0f : 0.0f;
				const float visibleThumbnailSize = displayItem.subAsset
					? (std::max)(48.0f, thumbnailSize - 18.0f)
					: thumbnailSize;
				const ImVec2 iconMin(
					cursor.x + childIndent + (cardSize.x - childIndent - visibleThumbnailSize) * 0.5f,
					cursor.y + 4.0f + (thumbnailSize - visibleThumbnailSize) * 0.5f);
				const ImVec2 iconMax(iconMin.x + visibleThumbnailSize, iconMin.y + visibleThumbnailSize);
				ImGui::InvisibleButton("##assetCard", cardSize);
					const bool hovered = ImGui::IsItemHovered();
					SchedulePrefabHotCachePreloadForHoveredItem(item, hovered);
					const bool hasDisclosure =
						NLS::Editor::Assets::ShouldShowAssetBrowserSubAssetDisclosure(displayItem);
				const auto disclosureSourcePath = item.sourceAssetPath.empty()
					? item.projectRelativePath
					: item.sourceAssetPath;
				const ImVec2 disclosureCenter(iconMax.x + 2.0f, iconMin.y + visibleThumbnailSize * 0.5f);
				const float disclosureRadius = 12.0f;
				const bool disclosureHovered =
					hasDisclosure &&
					ImGui::IsMouseHoveringRect(
						ImVec2(disclosureCenter.x - disclosureRadius, disclosureCenter.y - disclosureRadius),
						ImVec2(disclosureCenter.x + disclosureRadius, disclosureCenter.y + disclosureRadius));
				const bool disclosureClicked =
					disclosureHovered &&
					ImGui::IsMouseClicked(ImGuiMouseButton_Left);
				if (disclosureClicked)
				{
					const auto sourceItem = item;
					if (displayItem.expanded)
					{
						m_expandedProjectAssetItems.erase(disclosureSourcePath);
					}
					else
					{
						m_expandedProjectAssetItems.insert(disclosureSourcePath);
					}
					if (!ApplyProjectAssetDisclosureImmediately(sourceItem))
						MarkProjectAssetDisplayItemsDirty();
					ImGui::PopID();
					ImGui::Columns(1);
					return;
				}
				if (hovered && !selected)
				{
					drawList->AddRectFilled(
						ImVec2(iconMin.x - 3.0f, iconMin.y - 3.0f),
						ImVec2(iconMax.x + 3.0f, iconMax.y + 3.0f),
						ImGui::GetColorU32(ImGuiCol_HeaderHovered),
						2.0f);
				}
				if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
				{
					if (!disclosureHovered)
						SelectProjectGridItem(item);
				}
				{
					AssetBrowserArtifactTelemetryAccumulatorScope interactionsTelemetry {
						interactionsElapsed
					};
					DrawProjectGridItemDragSource(item);
					DrawProjectGridItemContextMenu(item);
				}
				if (presentationGeneration != m_projectAssetPresentationGeneration)
				{
					ImGui::PopID();
					ImGui::Columns(1);
					return;
				}
				if (item.kind == NLS::Editor::Assets::AssetBrowserItemKind::Folder)
				{
					AssetBrowserArtifactTelemetryAccumulatorScope interactionsTelemetry {
						interactionsElapsed
					};
					DrawProjectFolderDropTarget(item.projectRelativePath, item.absolutePath);
				}
				if (presentationGeneration != m_projectAssetPresentationGeneration)
				{
					ImGui::PopID();
					ImGui::Columns(1);
					return;
				}

				if (selected)
				{
					drawList->AddRectFilled(
						ImVec2(iconMin.x - 3.0f, iconMin.y - 3.0f),
						ImVec2(iconMax.x + 3.0f, iconMax.y + 3.0f),
						ImGui::GetColorU32(ImGuiCol_Header),
						2.0f);
				}
				{
					AssetBrowserArtifactTelemetryAccumulatorScope thumbnailTelemetry {
						thumbnailElapsed
					};
					DrawProjectGridItemThumbnail(item, iconMin, iconMax, visibleThumbnailSize, hovered);
				}
				if (hasDisclosure)
				{
					const bool expanded = m_expandedProjectAssetItems.find(disclosureSourcePath) != m_expandedProjectAssetItems.end();
					deferredDisclosureButtons.push_back({
						drawList,
						disclosureCenter,
						disclosureRadius,
						expanded,
						disclosureHovered
					});
				}

				const ImVec2 selectionMin(iconMin.x - 3.0f, iconMin.y - 3.0f);
				const ImVec2 selectionMax(iconMax.x + 3.0f, iconMax.y + 3.0f);
				const ImVec2 labelMin(selectionMin.x, iconMax.y + labelGap);
				const ImVec2 labelMax(selectionMax.x, labelMin.y + labelHeight);
				const bool labelHovered = ImGui::IsMouseHoveringRect(labelMin, labelMax);
				{
					AssetBrowserArtifactTelemetryAccumulatorScope interactionsTelemetry {
						interactionsElapsed
					};
					if (!disclosureHovered &&
						!labelHovered &&
						ImGui::IsItemHovered() &&
						ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					{
						m_projectBrowserInlineRename.pending = false;
						const bool openingWillInvalidateGrid =
							NLS::Editor::Assets::ShouldStopDrawingAssetBrowserGridAfterOpeningItem(
								m_selectedProjectFolder,
								item);
						OpenProjectGridItem(item);
						if (openingWillInvalidateGrid ||
							presentationGeneration != m_projectAssetPresentationGeneration)
						{
							ImGui::PopID();
							ImGui::Columns(1);
							return;
						}
					}
				}
				if (m_projectBrowserInlineRename.pending &&
					m_projectBrowserInlineRename.sourceProjectRelativePath == item.projectRelativePath &&
					ImGui::GetTime() - m_projectBrowserInlineRename.pendingSince > 0.28)
				{
					m_projectBrowserInlineRename.pending = false;
					BeginInlineRenameProjectItem(item);
				}
				{
					AssetBrowserArtifactTelemetryAccumulatorScope labelTelemetry {
						labelElapsed
					};
					const float labelPadding = 4.0f;
					const float labelAvailableWidth = (std::max)(1.0f, labelMax.x - labelMin.x - labelPadding * 2.0f);
					const auto label = EllipsizeAssetBrowserLabel(item.displayName, labelAvailableWidth);
					const ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
					const float labelX = labelMin.x + labelPadding + (std::max)(0.0f, (labelAvailableWidth - labelSize.x) * 0.5f);
					if (selected)
					{
						drawList->AddRectFilled(
							labelMin,
							labelMax,
							ImGui::GetColorU32(ImGuiCol_Header),
							2.0f);
					}
					if (!m_projectBrowserInlineRename.active &&
						selected &&
						!displayItem.subAsset &&
						NLS::Editor::Assets::BuildAssetBrowserWorkflowCapabilities(item).canRename &&
						labelHovered &&
						ImGui::IsMouseClicked(ImGuiMouseButton_Left))
					{
						m_projectBrowserInlineRename.pending = true;
						m_projectBrowserInlineRename.pendingSince = ImGui::GetTime();
						m_projectBrowserInlineRename.sourceProjectRelativePath = item.projectRelativePath;
					}
					if (m_projectBrowserInlineRename.active &&
						m_projectBrowserInlineRename.sourceProjectRelativePath == item.projectRelativePath)
					{
						DrawProjectGridItemInlineRename(item, labelMin, labelMax);
					}
					else
					{
						drawList->AddText(
							ImVec2(labelX, labelMin.y + 4.0f),
							ImGui::GetColorU32(ImGuiCol_Text),
							label.c_str());
					}
				}

				if (hovered)
					ImGui::SetTooltip("%s", item.projectRelativePath.c_str());

				ImGui::NextColumn();
				ImGui::PopID();
			}
		}
	}
	ImGui::Columns(1);
	for (const auto& button : deferredDisclosureButtons)
	{
		if (button.drawList == nullptr)
			continue;
		DrawAssetBrowserDisclosureButton(
			button.drawList,
			button.center,
			button.radius,
			button.expanded,
			button.hovered,
				true);
		}
	}
	{
		AssetBrowserArtifactTelemetryAccumulatorScope visibleSetTelemetry {
			visibleSetElapsed
		};
		SetVisibleThumbnailItems(std::move(visibleThumbnailItems));
	}
	{
		AssetBrowserArtifactTelemetryAccumulatorScope generationScopeTelemetry {
			generationScopeElapsed
		};
		UpdateThumbnailGenerationScope();
	}
	RecordAssetBrowserArtifactTelemetryStage(
		NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGridVisibleRows,
		visibleRowsElapsed,
		drawTelemetryPath,
		drawTelemetryItemCount,
		kAssetBrowserThumbnailUiDrawTelemetryMinimum);
	RecordAssetBrowserArtifactTelemetryStage(
		NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemInteractions,
		interactionsElapsed,
		drawTelemetryPath,
		drawTelemetryItemCount,
		kAssetBrowserThumbnailUiDrawTelemetryMinimum);
	RecordAssetBrowserArtifactTelemetryStage(
		NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemThumbnail,
		thumbnailElapsed,
		drawTelemetryPath,
		drawTelemetryItemCount,
		kAssetBrowserThumbnailUiDrawTelemetryMinimum);
	RecordAssetBrowserArtifactTelemetryStage(
		NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGridItemLabel,
		labelElapsed,
		drawTelemetryPath,
		drawTelemetryItemCount,
		kAssetBrowserThumbnailUiDrawTelemetryMinimum);
	RecordAssetBrowserArtifactTelemetryStage(
		NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawVisibleSet,
		visibleSetElapsed,
		drawTelemetryPath,
		drawTelemetryItemCount,
		kAssetBrowserThumbnailUiDrawTelemetryMinimum);
	RecordAssetBrowserArtifactTelemetryStage(
		NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScope,
		generationScopeElapsed,
		drawTelemetryPath,
		drawTelemetryItemCount,
		kAssetBrowserThumbnailUiDrawTelemetryMinimum);
}

void Editor::Panels::AssetBrowser::HandleProjectAssetBrowserDroppedFiles(const std::vector<std::string>& paths)
{
	if (paths.empty())
		return;

	std::vector<std::filesystem::path> sourcePaths;
	sourcePaths.reserve(paths.size());
	for (const auto& path : paths)
	{
		if (!path.empty())
			sourcePaths.emplace_back(path);
	}

	if (!sourcePaths.empty())
		(void)ImportExternalFilesIntoCurrentFolder(sourcePaths);
}

void Editor::Panels::AssetBrowser::HandleProjectAssetBrowserShortcuts()
{
	if (ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive())
		return;

	const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
	const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	if (!hovered && !focused)
		return;

	if (ImGui::IsKeyPressed(ImGuiKey_F2))
	{
		if (const auto selected = std::find_if(
				m_currentFolderItems.begin(),
				m_currentFolderItems.end(),
				[this](const auto& item)
				{
					return m_selectedProjectItem.has_value() &&
						NLS::Editor::Assets::AssetBrowserItemMatchesActionIdentity(item, *m_selectedProjectItem);
				});
			selected != m_currentFolderItems.end())
		{
			BeginInlineRenameProjectItem(*selected);
		}
	}
	if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C))
		CopySelectedProjectItemToClipboard();
	if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V))
		(void)PasteClipboardIntoCurrentFolder();
	if (!ImGui::IsKeyDown(ImGuiKey_Delete))
		m_projectDeleteShortcutAwaitingRelease = false;
	if (!m_projectDeleteActionAwaitingRelease &&
		!m_projectDeleteShortcutAwaitingRelease &&
		ImGui::IsKeyPressed(ImGuiKey_Delete, false))
	{
		m_projectDeleteShortcutAwaitingRelease = true;
		if (const auto selected = std::find_if(
				m_currentFolderItems.begin(),
				m_currentFolderItems.end(),
				[this](const auto& item)
				{
					return m_selectedProjectItem.has_value() &&
						NLS::Editor::Assets::AssetBrowserItemMatchesActionIdentity(item, *m_selectedProjectItem);
				});
			selected != m_currentFolderItems.end())
		{
			(void)DeleteProjectItem(*selected);
		}
	}
	if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D))
	{
		if (const auto selected = std::find_if(
				m_currentFolderItems.begin(),
				m_currentFolderItems.end(),
				[this](const auto& item)
				{
					return m_selectedProjectItem.has_value() &&
						NLS::Editor::Assets::AssetBrowserItemMatchesActionIdentity(item, *m_selectedProjectItem);
				});
			selected != m_currentFolderItems.end())
		{
			(void)DuplicateProjectItem(*selected);
		}
	}
}

void Editor::Panels::AssetBrowser::DrawCurrentFolderList(
	const std::vector<NLS::Editor::Assets::AssetBrowserDisplayItem>& displayItems)
{
	const std::string drawTelemetryPath = m_selectedProjectFolder + "|list";
	size_t drawTelemetryItemCount = displayItems.size();
	std::vector<NLS::Editor::Assets::AssetBrowserItem> visibleThumbnailItems;
	{
		AssetBrowserArtifactTelemetryScope drawTelemetry {
			NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDraw,
			std::chrono::steady_clock::now(),
			&drawTelemetryPath,
			&drawTelemetryItemCount,
			kAssetBrowserThumbnailUiDrawTelemetryMinimum
		};

		if (displayItems.empty())
		{
		SetVisibleThumbnailItems({});
		UpdateThumbnailGenerationScope();
		if (m_currentFolderItemsRefresh.has_value())
		{
			auto* drawList = ImGui::GetWindowDrawList();
			const float rowHeight = 26.0f;
			const int placeholderCount = (std::max)(8, static_cast<int>(std::ceil(ImGui::GetContentRegionAvail().y / rowHeight)));
			for (int index = 0; index < placeholderCount; ++index)
			{
				ImGui::PushID(index);
				const ImVec2 cursor = ImGui::GetCursorScreenPos();
				const ImVec2 rowSize(ImGui::GetContentRegionAvail().x, rowHeight);
				ImGui::InvisibleButton("##assetFolderLoadingListPlaceholder", rowSize);
				drawList->AddRectFilled(
					ImVec2(cursor.x + 18.0f, cursor.y + 5.0f),
					ImVec2(cursor.x + 36.0f, cursor.y + 23.0f),
					IM_COL32(42, 46, 50, 92),
					2.0f);
				drawList->AddRectFilled(
					ImVec2(cursor.x + 46.0f, cursor.y + 8.0f),
					ImVec2(cursor.x + (std::min)(rowSize.x - 12.0f, 220.0f), cursor.y + 18.0f),
					IM_COL32(42, 46, 50, 76),
					2.0f);
				ImGui::PopID();
			}
		}
		else
		{
			ImGui::TextDisabled("This folder is empty");
		}
			return;
		}

		visibleThumbnailItems.reserve(64u);
		const float rowHeight = 26.0f;
		ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(displayItems.size()), rowHeight);
	while (clipper.Step())
	{
		const ImVec2 visibleSliceCursor = ImGui::GetCursorScreenPos();
		const float visibleSliceWidth = ImGui::GetContentRegionAvail().x;
		for (const auto& segment : NLS::Editor::Assets::ResolveAssetBrowserVisibleListGroupSegments(
			displayItems,
			static_cast<size_t>(clipper.DisplayStart),
			static_cast<size_t>(clipper.DisplayEnd)))
		{
			const float firstRow = static_cast<float>(segment.range.begin - static_cast<size_t>(clipper.DisplayStart));
			const float rowCount = static_cast<float>(segment.range.count);
			ImDrawFlags cornerFlags = ImDrawFlags_RoundCornersNone;
			if (segment.trueSegmentStart)
				cornerFlags |= ImDrawFlags_RoundCornersTop;
			if (segment.trueSegmentEnd)
				cornerFlags |= ImDrawFlags_RoundCornersBottom;
			DrawAssetBrowserSegmentPanel(
				ImGui::GetWindowDrawList(),
				ImVec2(
					visibleSliceCursor.x + 18.0f,
					visibleSliceCursor.y + firstRow * rowHeight + (segment.trueSegmentStart ? 2.0f : 0.0f)),
				ImVec2(
					visibleSliceCursor.x + visibleSliceWidth - 8.0f,
					visibleSliceCursor.y + (firstRow + rowCount) * rowHeight -
						(segment.trueSegmentEnd ? 2.0f : 0.0f)),
				false,
				cornerFlags);
		}
		for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
		{
			const auto& displayItem = displayItems[static_cast<size_t>(index)];
			const auto& item = displayItem.item;
			const auto presentationGeneration = m_projectAssetPresentationGeneration;

			if (displayItem.loadingPlaceholder)
				ImGui::PushID(index);
			else
				ImGui::PushID(item.projectRelativePath.c_str());
			const ImVec2 cursor = ImGui::GetCursorScreenPos();
			const float indent = displayItem.subAsset ? 20.0f : 0.0f;
			const ImVec2 rowSize(ImGui::GetContentRegionAvail().x, rowHeight);
			if (displayItem.loadingPlaceholder)
			{
				ImGui::InvisibleButton("##assetListPlaceholder", rowSize);
				ImGui::GetWindowDrawList()->AddRectFilled(
					ImVec2(cursor.x + 38.0f, cursor.y + 7.0f),
					ImVec2(cursor.x + (std::min)(rowSize.x - 12.0f, 180.0f), cursor.y + 17.0f),
					IM_COL32(42, 46, 50, 120),
					2.0f);
				ImGui::PopID();
				continue;
			}
			visibleThumbnailItems.push_back(item);
			const bool selected = m_selectedProjectItem.has_value() &&
				NLS::Editor::Assets::AssetBrowserItemMatchesActionIdentity(item, *m_selectedProjectItem);
			auto* drawList = ImGui::GetWindowDrawList();
			bool disclosureHovered = false;
			ImGui::InvisibleButton("##assetListRow", rowSize);
			const bool hovered = ImGui::IsItemHovered();
			if (selected || hovered)
			{
				drawList->AddRectFilled(
					cursor,
					ImVec2(cursor.x + rowSize.x, cursor.y + rowSize.y),
					ImGui::GetColorU32(selected ? ImGuiCol_Header : ImGuiCol_HeaderHovered),
					2.0f);
			}
				if (NLS::Editor::Assets::ShouldShowAssetBrowserSubAssetDisclosure(displayItem))
			{
				const auto sourcePath = item.sourceAssetPath.empty()
					? item.projectRelativePath
					: item.sourceAssetPath;
				const ImVec2 disclosureCenter(cursor.x + 9.0f, cursor.y + rowHeight * 0.5f);
				const float disclosureRadius = 8.0f;
				disclosureHovered =
					ImGui::IsMouseHoveringRect(
						ImVec2(disclosureCenter.x - disclosureRadius, disclosureCenter.y - disclosureRadius),
						ImVec2(disclosureCenter.x + disclosureRadius, disclosureCenter.y + disclosureRadius));
				if (disclosureHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
				{
					const auto sourceItem = item;
					if (displayItem.expanded)
					{
						m_expandedProjectAssetItems.erase(sourcePath);
					}
					else
					{
						m_expandedProjectAssetItems.insert(sourcePath);
					}
					if (!ApplyProjectAssetDisclosureImmediately(sourceItem))
						MarkProjectAssetDisplayItemsDirty();
					ImGui::PopID();
					return;
				}
				DrawAssetBrowserDisclosureButton(
					drawList,
					disclosureCenter,
					disclosureRadius,
					m_expandedProjectAssetItems.find(sourcePath) != m_expandedProjectAssetItems.end(),
					disclosureHovered);
			}
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
			{
				if (!disclosureHovered)
					SelectProjectGridItem(item);
			}

			const ImVec2 iconMin(cursor.x + 18.0f + indent, cursor.y + 4.0f);
			const ImVec2 iconMax(iconMin.x + 18.0f, iconMin.y + 18.0f);
			DrawProjectGridItemThumbnail(item, iconMin, iconMax, 18.0f, hovered, true);

			const float textX = iconMax.x + 8.0f;
			const char* typeLabel = NLS::Editor::Assets::AssetBrowserItemTypeDisplayLabel(item.type);
			const ImVec2 typeSize = ImGui::CalcTextSize(typeLabel);
			const float typeLabelX = cursor.x + rowSize.x - typeSize.x - 12.0f;
			const float textWidth = (std::max)(1.0f, typeLabelX - textX - 12.0f);
			const ImVec2 labelMin(textX, cursor.y + 1.0f);
			const ImVec2 labelMax(textX + textWidth, cursor.y + rowHeight - 1.0f);
			const bool labelHovered = ImGui::IsMouseHoveringRect(labelMin, labelMax);
			if (m_projectBrowserInlineRename.pending &&
				m_projectBrowserInlineRename.sourceProjectRelativePath == item.projectRelativePath &&
				ImGui::GetTime() - m_projectBrowserInlineRename.pendingSince > 0.28)
			{
				m_projectBrowserInlineRename.pending = false;
				BeginInlineRenameProjectItem(item);
			}
			if (!m_projectBrowserInlineRename.active &&
				selected &&
				!displayItem.subAsset &&
				NLS::Editor::Assets::BuildAssetBrowserWorkflowCapabilities(item).canRename &&
				labelHovered &&
				ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			{
				m_projectBrowserInlineRename.pending = true;
				m_projectBrowserInlineRename.pendingSince = ImGui::GetTime();
				m_projectBrowserInlineRename.sourceProjectRelativePath = item.projectRelativePath;
			}
			if (m_projectBrowserInlineRename.active &&
				m_projectBrowserInlineRename.sourceProjectRelativePath == item.projectRelativePath)
			{
				ImGui::SetCursorScreenPos(ImVec2(textX, cursor.y + 1.0f));
				ImGui::SetNextItemWidth(textWidth);
				if (m_projectBrowserInlineRename.focusRequested)
				{
					ImGui::SetKeyboardFocusHere();
					m_projectBrowserInlineRename.focusRequested = false;
				}
				const bool enterPressed = ImGui::InputText(
					"##ProjectAssetInlineRenameList",
					m_projectBrowserInlineRename.buffer.data(),
					m_projectBrowserInlineRename.buffer.size(),
					ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
				if (ImGui::IsKeyPressed(ImGuiKey_Escape))
				{
					CancelInlineRenameProjectItem();
				}
				else if (enterPressed || ImGui::IsItemDeactivatedAfterEdit())
				{
					(void)CommitInlineRenameProjectItem();
				}
			}
			else
			{
				drawList->AddText(
					ImVec2(textX, cursor.y + 5.0f),
					ImGui::GetColorU32(ImGuiCol_Text),
					item.displayName.c_str());
			}
			drawList->AddText(
				ImVec2(typeLabelX, cursor.y + 5.0f),
				ImGui::GetColorU32(ImGuiCol_TextDisabled),
				typeLabel);

			DrawProjectGridItemDragSource(item);
			DrawProjectGridItemContextMenu(item);
			if (presentationGeneration != m_projectAssetPresentationGeneration)
			{
				ImGui::PopID();
				return;
			}
			if (item.kind == NLS::Editor::Assets::AssetBrowserItemKind::Folder)
				DrawProjectFolderDropTarget(item.projectRelativePath, item.absolutePath);
			if (presentationGeneration != m_projectAssetPresentationGeneration)
			{
				ImGui::PopID();
				return;
			}
			if (!disclosureHovered && !labelHovered && hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				m_projectBrowserInlineRename.pending = false;
				OpenProjectGridItem(item);
				if (presentationGeneration != m_projectAssetPresentationGeneration)
				{
					ImGui::PopID();
					return;
				}
			}
			if (hovered)
				ImGui::SetTooltip("%s", item.projectRelativePath.c_str());
			ImGui::PopID();
		}
	}
	}

	SetVisibleThumbnailItems(std::move(visibleThumbnailItems));
	UpdateThumbnailGenerationScope();
}

Editor::Panels::AssetBrowser::ThumbnailTextureHandle Editor::Panels::AssetBrowser::ResolveCachedThumbnailTextureHandle(
	const std::filesystem::path& imagePath,
	const bool queueIfMissing,
	const NLS::Render::RHI::TextureColorSpace colorSpace)
{
	if (imagePath.empty())
		return {};

	const auto normalizedPath = imagePath.lexically_normal().generic_string();
	if (const auto found = m_thumbnailTexturesByPath.find(normalizedPath); found != m_thumbnailTexturesByPath.end())
	{
		m_thumbnailTexturesUsedThisFrame.insert(normalizedPath);
		found->second.lastUsedFrame = m_thumbnailTextureFrameSerial;
		if (found->second.atlas)
		{
			m_thumbnailAtlas.Touch(normalizedPath, m_thumbnailTextureFrameSerial);
            if (const auto page = m_thumbnailAtlasPagesByKey.find(found->second.atlasPageKey);
                page != m_thumbnailAtlasPagesByKey.end() &&
                page->second.textureId != nullptr &&
                page->second.pageGeneration == found->second.atlasPageGeneration)
            {
                ThumbnailTextureHandle handle;
                handle.textureHandle = page->second.textureId;
                handle.width = found->second.width;
                handle.height = found->second.height;
                handle.source = ThumbnailTextureSource::Atlas;
                handle.uv = found->second.uv;
                handle.pageGeneration = page->second.pageGeneration;
                return handle;
            }
			ReleaseCachedThumbnailTexture(normalizedPath);
			return {};
		}
			if (found->second.textureView != nullptr &&
				found->second.textureId != nullptr)
				{
                    ThumbnailTextureHandle handle;
                    handle.textureHandle = found->second.textureId;
                    handle.width = found->second.width;
                    handle.height = found->second.height;
                    handle.source = ThumbnailTextureSource::Standalone;
                    return handle;
				}
		ReleaseCachedThumbnailTexture(normalizedPath);
		return {};
	}

	if (queueIfMissing)
		QueueCachedThumbnailTextureLoad(imagePath, colorSpace);
	return {};
}

void Editor::Panels::AssetBrowser::EnsureThumbnailTextureDeviceIdentity()
{
	auto* driver = NLS::Render::Context::TryGetLocatedDriver();
	uint64_t deviceIdentity = 0u;
	if (driver != nullptr)
	{
		const auto device = NLS::Render::Context::DriverRendererAccess::GetExplicitDevice(*driver);
		if (device != nullptr)
			deviceIdentity = device->GetCacheIdentity();
	}

	if (deviceIdentity == m_thumbnailTextureDeviceIdentity)
		return;

	if (m_thumbnailTextureDeviceIdentity != 0u || deviceIdentity != 0u)
		ReleaseAssetBrowserTextureHandleCache(true);
	m_thumbnailTextureDeviceIdentity = deviceIdentity;
}

void Editor::Panels::AssetBrowser::ApplyThumbnailServiceResult(
	const NLS::Editor::Assets::AssetThumbnailServiceResult& generated)
{
	auto recovered = generated;
	if (recovered.status != NLS::Editor::Assets::AssetThumbnailServiceStatus::Fresh &&
		!recovered.presentationKey.empty() &&
		!recovered.sourceAssetPath.empty())
	{
		auto requestContext = MakeAssetBrowserThumbnailRequestBuildContext(m_thumbnailFeatureConfig);
		requestContext.assetDatabaseSnapshot = m_projectAssetDatabaseSnapshot;
		requestContext.residentPrefabPreviewRegistry = m_residentPrefabPreviewRegistry;
		const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
		for (const auto& item : m_currentFolderItems)
		{
			if (!NLS::Editor::Assets::AssetBrowserThumbnailItemMatchesResultIdentity(
					item,
					recovered.sourceAssetPath,
					recovered.subAssetKey,
					static_cast<NLS::Editor::Assets::AssetThumbnailKind>(recovered.requestKind)))
			{
				continue;
			}
			const auto request = NLS::Editor::Assets::BuildAssetThumbnailRequestForItem(
				projectRoot,
				item,
				AssetBrowserThumbnailRequestSize(m_thumbnailSize),
				requestContext);
			if (request.has_value() &&
				NLS::Editor::Assets::BuildAssetThumbnailPresentationKey(*request) ==
					recovered.presentationKey &&
				NLS::Editor::Assets::PromoteAssetThumbnailResultFromPresentationIndex(
					*request,
					recovered))
			{
				break;
			}
		}
	}
	const auto& effective = recovered;
	RecordAssetBrowserThumbnailResultTelemetry(effective);
	if (effective.revokeGpuTexture && effective.cacheEntry.has_value() &&
		m_assetThumbnailPool != nullptr)
	{
		const auto& cacheKey = effective.cacheEntry->cacheKey;
		const bool matchesReadyPoolGeneration =
			m_assetThumbnailPool->GetStatus(cacheKey, effective.gpuTextureGeneration) ==
			NLS::Editor::Assets::ThumbnailRenderStatus::Ready;
		const auto hasRetainedPresentation = [&]()
		{
			const auto hasDisplayImage = [this](const std::vector<std::string>& itemKeys)
			{
				return std::any_of(
					itemKeys.begin(),
					itemKeys.end(),
					[this](const std::string& itemKey)
					{
						const auto found = m_thumbnailResultsByItemKey.find(itemKey);
						return found != m_thumbnailResultsByItemKey.end() &&
							NLS::Editor::Assets::AssetBrowserThumbnailResultHasDisplayImage(found->second);
					});
			};
			if (const auto found = m_thumbnailItemKeyByCacheKey.find(cacheKey);
				found != m_thumbnailItemKeyByCacheKey.end() && hasDisplayImage(found->second))
			{
				return true;
			}
			if (!effective.presentationKey.empty())
			{
				if (const auto found = m_thumbnailItemKeyByPresentationKey.find(effective.presentationKey);
					found != m_thumbnailItemKeyByPresentationKey.end() && hasDisplayImage(found->second))
				{
					return true;
				}
			}
			return false;
		};
		// A late failure must not revoke a newer generation, nor the image that
		// the presentation map deliberately retained while refreshing.
		if (matchesReadyPoolGeneration && !hasRetainedPresentation())
		{
			m_assetThumbnailPool->Remove(cacheKey);
			m_assetThumbnailsByCacheKey.erase(cacheKey);
		}
	}
	if (effective.cacheEntry.has_value() && effective.gpuTexture.IsValid() &&
		m_assetThumbnailPool != nullptr)
	{
		if (m_assetThumbnailPool->Publish(
			effective.cacheEntry->cacheKey,
			effective.gpuTextureGeneration,
			effective.gpuTexture))
		{
			m_assetThumbnailsByCacheKey.insert_or_assign(
				effective.cacheEntry->cacheKey,
				m_assetThumbnailPool->MakeThumbnail(
					effective.cacheEntry->cacheKey,
					effective.gpuTextureGeneration));
		}
	}
	if (effective.status == NLS::Editor::Assets::AssetThumbnailServiceStatus::Fresh &&
		!effective.imagePath.empty())
	{
		const auto key = effective.imagePath.lexically_normal().generic_string();
		m_thumbnailTexturesFailedToLoad.erase(key);
		if (m_thumbnailTexturesDecoding.find(key) != m_thumbnailTexturesDecoding.end())
			m_thumbnailTexturesRetryAfterDecode.insert(key);
		QueueCachedThumbnailTextureLoad(
			effective.imagePath,
			CachedThumbnailTextureColorSpace(effective));
	}
	if (effective.cacheEntry.has_value())
	{
		NLS::Editor::Assets::ApplyAssetBrowserThumbnailCacheKeyResult(
			m_thumbnailItemKeyByCacheKey,
			m_thumbnailResultsByItemKey,
			effective.cacheEntry->cacheKey,
			effective);
	}

	// Deferred prefab resolution can complete after the cache-key binding was
	// registered.  The presentation key is stable across that transition, so
	// use it as a second commit route instead of leaving the result stranded.
	if (!effective.presentationKey.empty())
	{
		NLS::Editor::Assets::ApplyAssetBrowserThumbnailPresentationKeyResult(
			m_thumbnailItemKeyByPresentationKey,
			m_thumbnailResultsByItemKey,
			effective.presentationKey,
			effective);
	}

	// The cache-key and presentation-key bindings are populated while the
	// current scope is being built. A resolved manifest can nevertheless finish
	// after that binding window. Rebind the result to the live folder item by
	// stable source/sub-asset identity so a valid PNG or GPU result is not left
	// stranded on a fallback tile.
	if (!effective.sourceAssetPath.empty())
	{
		for (const auto& item : m_currentFolderItems)
		{
			if (!NLS::Editor::Assets::AssetBrowserThumbnailItemMatchesResultIdentity(
				item,
				effective.sourceAssetPath,
				effective.subAssetKey,
				static_cast<NLS::Editor::Assets::AssetThumbnailKind>(effective.requestKind)))
			{
				continue;
			}

			const auto itemKey = NLS::Editor::Assets::BuildAssetBrowserThumbnailItemKey(
				item,
				AssetBrowserThumbnailRequestSize(m_thumbnailSize));
			NLS::Editor::Assets::ApplyAssetBrowserThumbnailResultToItemKeys(
				std::vector<std::string> {itemKey},
				m_thumbnailResultsByItemKey,
				effective);
			if (effective.cacheEntry.has_value())
			{
				NLS::Editor::Assets::RegisterAssetBrowserThumbnailCacheKeyBinding(
					m_thumbnailItemKeyByCacheKey,
					effective.cacheEntry->cacheKey,
					itemKey);
			}
		}
	}
}

size_t Editor::Panels::AssetBrowser::RecoverVisiblePendingThumbnailPresentations(
	const double nowSeconds)
{
	if (!m_visibleThumbnailItemsKnown || m_visibleThumbnailItems.empty() ||
		nowSeconds < m_visiblePendingPresentationRecoveryAfter)
	{
		return 0u;
	}

	m_visiblePendingPresentationRecoveryAfter =
		nowSeconds + kAssetBrowserVisiblePendingPresentationRecoveryIntervalSeconds;
	const auto requestSize = AssetBrowserThumbnailRequestSize(m_thumbnailSize);
	auto requestContext = MakeAssetBrowserThumbnailRequestBuildContext(m_thumbnailFeatureConfig);
	requestContext.assetDatabaseSnapshot = m_projectAssetDatabaseSnapshot;
	requestContext.residentPrefabPreviewRegistry = m_residentPrefabPreviewRegistry;
	const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
	const size_t itemCount = m_visibleThumbnailItems.size();
	size_t visited = 0u;
	size_t probed = 0u;
	size_t promoted = 0u;

	while (visited < itemCount &&
		probed < kMaxAssetBrowserVisiblePendingPresentationProbesPerPump)
	{
		const size_t itemIndex = m_visiblePendingPresentationRecoveryOffset % itemCount;
		m_visiblePendingPresentationRecoveryOffset = (itemIndex + 1u) % itemCount;
		++visited;
		const auto& item = m_visibleThumbnailItems[itemIndex];
		if (!NLS::Editor::Assets::CanGenerateAssetBrowserThumbnail(item))
			continue;

		const auto itemKey = NLS::Editor::Assets::BuildAssetBrowserThumbnailItemKey(
			item,
			requestSize);
		const auto found = m_thumbnailResultsByItemKey.find(itemKey);
		if (found == m_thumbnailResultsByItemKey.end() ||
			!NLS::Editor::Assets::AssetBrowserThumbnailResultNeedsCanonicalRecovery(found->second) ||
			found->second.presentationKey.empty() ||
			found->second.sourceAssetPath.empty())
		{
			continue;
		}

		auto request = NLS::Editor::Assets::BuildAssetThumbnailRequestForItem(
			projectRoot,
			item,
			requestSize,
			requestContext);
		if (!request.has_value())
			continue;
		request->priority = NLS::Editor::Assets::ThumbnailRequestPriority::Visible;
		const auto rebuiltPresentationKey =
			NLS::Editor::Assets::BuildAssetThumbnailPresentationKey(*request);
		if (rebuiltPresentationKey != found->second.presentationKey)
			continue;

		++probed;
		// A Pending tile can outlive every service owner after an asynchronous
		// database/scope rebuild. Presentation-index probing alone can only find
		// an already-written PNG; it cannot restart work that was cancelled before
		// persistence. Re-submit this bounded rotating set as Visible so the service
		// either coalesces with a live owner or recreates the missing queue entry.
		auto recovered = m_thumbnailService.RequestAssetPreview(*request);
		const bool promotedFromPresentation =
			NLS::Editor::Assets::PromoteAssetThumbnailResultFromPresentationIndex(
				*request,
				recovered);
		ApplyThumbnailServiceResult(recovered);
		if (promotedFromPresentation)
		{
			++promoted;
		}
	}

	return promoted;
}

bool Editor::Panels::AssetBrowser::EnsureThumbnailPreviewRenderer()
{
	if (m_thumbnailPipelineShutdown)
		return false;
	if (m_thumbnailPreviewRenderer != nullptr)
		return true;
	if (IsEditorWindowClosing())
		return false;
	if (IsEditorSceneReadbackValidationActive())
		return false;

	auto* driver = NLS::Render::Context::TryGetLocatedDriver();
	if (driver == nullptr)
		return false;

	const std::string telemetryPath = m_selectedProjectFolder + "|preview-renderer-create";
	const size_t telemetryItemCount = m_thumbnailService.GetQueuedRequestCount();
	AssetBrowserArtifactTelemetryScope createTelemetry {
		NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpCreatePreviewRenderer,
		std::chrono::steady_clock::now(),
		&telemetryPath,
		&telemetryItemCount,
		kAssetBrowserThumbnailUiDrawTelemetryMinimum
	};
	m_thumbnailPreviewRenderer =
		std::make_shared<NLS::Editor::Assets::EditorThumbnailPreviewRenderer>(*driver);
	if (m_thumbnailPreviewRenderer == nullptr)
		return false;

	m_thumbnailRendererRegistry =
		std::make_shared<NLS::Editor::Assets::ThumbnailRendererRegistry>();
	m_thumbnailRendererRegistry->Register(
		NLS::Editor::Assets::AssetThumbnailKind::MaterialSphere,
		m_thumbnailPreviewRenderer);
	m_thumbnailRendererRegistry->Register(
		NLS::Editor::Assets::AssetThumbnailKind::ModelPreview,
		m_thumbnailPreviewRenderer);
	m_thumbnailRendererRegistry->Register(
		NLS::Editor::Assets::AssetThumbnailKind::PrefabPreview,
		m_thumbnailPreviewRenderer);
	return true;
}

void Editor::Panels::AssetBrowser::ShutdownThumbnailPipeline()
{
	if (m_thumbnailPipelineShutdown)
		return;
	m_thumbnailPipelineShutdown = true;

	m_thumbnailService.Shutdown();
	if (m_thumbnailRendererRegistry != nullptr)
		m_thumbnailRendererRegistry->Clear();
	m_thumbnailRendererRegistry.reset();
	m_thumbnailPreviewRenderer.reset();

	if (m_assetThumbnailPool != nullptr)
		m_assetThumbnailPool->Clear(true);
	ReleaseAssetBrowserTextureHandleCache(true);
	DestroyCachedThumbnailTextures(true);
}

bool Editor::Panels::AssetBrowser::IsEditorWindowClosing() const
{
	return EDITOR_CONTEXT(window) != nullptr && EDITOR_CONTEXT(window)->ShouldClose();
}

bool Editor::Panels::AssetBrowser::IsEditorSceneReadbackValidationActive() const
{
	auto* driver = NLS::Render::Context::TryGetLocatedDriver();
	if (driver == nullptr)
		return false;

	const auto& diagnostics = NLS::Render::Context::DriverRendererAccess::GetDiagnosticsSettings(*driver);
	return !diagnostics.editorValidationSceneReadbackOutput.empty() ||
		!diagnostics.editorValidationSceneReadbackSummary.empty();
}

bool Editor::Panels::AssetBrowser::IsStandardPbrShaderPassPrewarmPending() const
{
	return m_standardPbrShaderPassPrewarmQueued &&
		!m_standardPbrShaderPassPrewarmCompleted &&
		m_standardPbrShaderPassPrewarm.valid();
}

void Editor::Panels::AssetBrowser::PumpStandardPbrShaderPassPrewarm()
{
	if (m_standardPbrShaderPassPrewarmCompleted)
		return;

	if (m_standardPbrShaderPassPrewarmQueued)
	{
		if (!m_standardPbrShaderPassPrewarm.valid())
		{
			m_standardPbrShaderPassPrewarmQueued = false;
			m_standardPbrShaderPassPrewarmCompleted = true;
			return;
		}
		if (m_standardPbrShaderPassPrewarm.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			return;

		try
		{
			const size_t loadedPasses = m_standardPbrShaderPassPrewarm.get();
			NLS_LOG_INFO(
				"Standard PBR thumbnail shader pass prewarm completed: loadedPasses=" +
				std::to_string(loadedPasses));
		}
		catch (const std::exception& exception)
		{
			NLS_LOG_WARNING(std::string("Standard PBR thumbnail shader pass prewarm failed: ") + exception.what());
		}
		catch (...)
		{
			NLS_LOG_WARNING("Standard PBR thumbnail shader pass prewarm failed with an unknown exception.");
		}
		m_standardPbrShaderPassPrewarmQueued = false;
		m_standardPbrShaderPassPrewarmCompleted = true;
		return;
	}

	if (IsAssetBrowserInteractive() || !m_projectAssetDatabaseReady)
		return;

	const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
	const auto artifactDatabasePath = NLS::Editor::Assets::GetProjectArtifactDatabasePath(projectRoot);
	if (projectRoot.empty() || !std::filesystem::exists(artifactDatabasePath / "data.mdb"))
		return;

	try
	{
			m_standardPbrShaderPassPrewarm = ScheduleAssetBrowserJobFuture(
				"AssetBrowser.StandardPbrShaderPassPrewarm",
				[artifactDatabasePath]
				{
					NLS::Render::Resources::Loaders::MaterialLoader::LoadOptions options;
					options.loadMissingTextures = false;
					options.loadMissingShaders = true;
					options.artifactDatabasePath = artifactDatabasePath;
					options.targetPlatform = "editor";
					options.shaderPassLoadTelemetryStage =
						NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailGpuPreviewBackgroundMaterialShaderPassLoad;
					return NLS::Render::Resources::Loaders::MaterialLoader::PreloadShaderLabPassArtifacts(
						{},
						NLS::Editor::Assets::kDefaultShaderLabMaterialShaderPath,
						options);
				});
		m_standardPbrShaderPassPrewarmQueued = true;
	}
	catch (const std::exception& exception)
	{
		m_standardPbrShaderPassPrewarmCompleted = true;
		NLS_LOG_WARNING(std::string("Standard PBR thumbnail shader pass prewarm could not be queued: ") + exception.what());
	}
	catch (...)
	{
		m_standardPbrShaderPassPrewarmCompleted = true;
		NLS_LOG_WARNING("Standard PBR thumbnail shader pass prewarm could not be queued.");
	}
}

void Editor::Panels::AssetBrowser::PumpThumbnailPreviewRenderWarmup()
{
	if (m_thumbnailPreviewRenderWarmupCompleted ||
		IsEditorWindowClosing() ||
		IsEditorSceneReadbackValidationActive() ||
		IsAssetBrowserInteractive() ||
		!m_standardPbrShaderPassPrewarmCompleted ||
		IsStandardPbrShaderPassPrewarmPending())
	{
		return;
	}

	if (!EnsureThumbnailPreviewRenderer())
		return;

	constexpr uint32_t kThumbnailPreviewRenderWarmupSize = 96u;
	if (m_thumbnailPreviewRenderer->PrewarmMaterialPreviewRenderPath(kThumbnailPreviewRenderWarmupSize))
	{
		m_thumbnailPreviewRenderWarmupCompleted = true;
		NLS_LOG_INFO("Thumbnail material preview render path warmup completed.");
	}
}

void Editor::Panels::AssetBrowser::PumpImportedPrefabThumbnailContinuations()
{
	if (m_residentPrefabPreviewRegistry == nullptr)
		return;

	const auto wakeRevision =
		m_residentPrefabPreviewRegistry->GetThumbnailWakeRevision();
	if (wakeRevision == m_lastImportedPrefabThumbnailContinuationPumpRevision)
		return;

	const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
	const auto continuations =
		m_residentPrefabPreviewRegistry->GetImportedPrefabThumbnailContinuations(projectRoot);
	if (continuations.empty())
	{
		m_importedPrefabThumbnailContinuationOffset = 0u;
		m_importedPrefabThumbnailContinuationSubmittedRevisions.clear();
		m_lastImportedPrefabThumbnailContinuationPumpRevision = wakeRevision;
		return;
	}

	const NLS::Editor::Assets::ImportedPrefabThumbnailContinuation* pending = nullptr;
	for (size_t visited = 0u; visited < continuations.size(); ++visited)
	{
		const auto& candidate = continuations[
			m_importedPrefabThumbnailContinuationOffset % continuations.size()];
		++m_importedPrefabThumbnailContinuationOffset;
		const auto submitted =
			m_importedPrefabThumbnailContinuationSubmittedRevisions.find(
				candidate.assetId.ToString());
		if (submitted == m_importedPrefabThumbnailContinuationSubmittedRevisions.end() ||
			submitted->second != candidate.registrationRevision)
		{
			pending = &candidate;
			break;
		}
	}
	if (pending == nullptr)
	{
		m_lastImportedPrefabThumbnailContinuationPumpRevision = wakeRevision;
		return;
	}

	const auto& continuation = *pending;
	const auto continuationAssetId = continuation.assetId.ToString();
	m_importedPrefabThumbnailContinuationSubmittedRevisions.insert_or_assign(
		continuationAssetId,
		continuation.registrationRevision);
	const auto sourceAbsolutePath =
		(projectRoot / continuation.sourceAssetPath).lexically_normal();
	const auto artifactAbsolutePath =
		(projectRoot / continuation.artifactPath).lexically_normal();
	std::error_code sourceError;
	std::error_code artifactError;
	const auto sourceMeta = NLS::Core::Assets::AssetMeta::Load(
		NLS::Core::Assets::GetAssetMetaPath(sourceAbsolutePath));
	if (!std::filesystem::is_regular_file(sourceAbsolutePath, sourceError) || sourceError ||
		!std::filesystem::is_regular_file(artifactAbsolutePath, artifactError) || artifactError ||
		!sourceMeta.has_value() || sourceMeta->id != continuation.assetId)
	{
		m_residentPrefabPreviewRegistry->CompleteImportedPrefabThumbnailContinuation(
			projectRoot,
			continuation.assetId);
		return;
	}

	NLS::Editor::Assets::AssetBrowserItem item;
	item.displayName = std::filesystem::path(continuation.sourceAssetPath)
		.filename()
		.generic_string();
	item.projectRelativePath = continuation.sourceAssetPath;
	item.sourceAssetPath = continuation.sourceAssetPath;
	item.absolutePath = sourceAbsolutePath;
	item.artifactPath = continuation.artifactPath;
	item.kind = NLS::Editor::Assets::AssetBrowserItemKind::SourceAsset;
	item.type = NLS::Editor::Assets::AssetBrowserItemType::Model;
	item.assetId = continuation.assetId;
	item.subAssetKey = continuation.prefabSubAssetKey;
	item.artifactType = NLS::Core::Assets::ArtifactType::Prefab;
	item.hasGeneratedSubAssets = true;

	auto context = MakeAssetBrowserThumbnailRequestBuildContext(m_thumbnailFeatureConfig);
	context.assetDatabaseSnapshot = m_projectAssetDatabaseSnapshot;
	context.residentPrefabPreviewRegistry = m_residentPrefabPreviewRegistry;
	auto request = NLS::Editor::Assets::BuildAssetThumbnailRequestForItem(
		projectRoot,
		item,
		AssetBrowserThumbnailRequestSize(m_thumbnailSize),
		context);
	if (!request.has_value() || !request->importedPrefabThumbnailContinuation)
		return;

	request->priority = NLS::Editor::Assets::ThumbnailRequestPriority::Background;
	const auto result = m_thumbnailService.RequestAssetPreview(*request);
	if (NLS::Editor::Assets::ShouldRetryAssetBrowserThumbnailRequestAfterBackpressure(
			result.diagnostic))
	{
		m_importedPrefabThumbnailContinuationSubmittedRevisions.erase(
			continuationAssetId);
	}
	if (result.status == NLS::Editor::Assets::AssetThumbnailServiceStatus::Fresh)
		ApplyThumbnailServiceResult(result);
}

void Editor::Panels::AssetBrowser::PumpThumbnailGeneration(
		const bool allowGpuPreviewStart,
		const bool allowHeavyGpuPreview,
		const bool allowPreviewRenderWarmup,
		const bool sceneViewCameraNavigationActive)
{
	NLS_PROFILE_NAMED_SCOPE("AssetBrowser::PumpThumbnailGeneration");
	const bool thumbnailInteractive = IsAssetBrowserInteractive();
	constexpr uint64_t kAssetBrowserTargetFrameMicroseconds = 16667u;
	const auto previousFrameElapsedMicroseconds = ImGui::GetIO().DeltaTime > 0.0f
		? static_cast<uint64_t>(ImGui::GetIO().DeltaTime * 1000000.0f)
		: 0u;
	const auto previousFrameHeadroomMicroseconds = previousFrameElapsedMicroseconds > 0u &&
		previousFrameElapsedMicroseconds < kAssetBrowserTargetFrameMicroseconds
		? kAssetBrowserTargetFrameMicroseconds - previousFrameElapsedMicroseconds
		: 0u;
	m_thumbnailRenderScheduler.BeginFrame(
		static_cast<uint64_t>(ImGui::GetFrameCount()),
		thumbnailInteractive,
		previousFrameHeadroomMicroseconds,
		previousFrameElapsedMicroseconds > kAssetBrowserTargetFrameMicroseconds);
	const auto thumbnailFrameBudget = m_thumbnailRenderScheduler.GetFrameStats().budgetMicroseconds;
	m_thumbnailService.SetThumbnailPreviewResourcePumpBudgetMicroseconds(
		m_thumbnailFeatureConfig.adaptiveBudget
			? static_cast<uint32_t>(thumbnailFrameBudget)
			: 1000u);
	PumpImportedPrefabThumbnailContinuations();
	// Deadline and cancellation maintenance is lifecycle work, not a render
	// submission. Run it before scheduler admission so a rejected frame cannot
	// strand a visible thumbnail in a terminally pending state.
	m_thumbnailService.MaintainPendingThumbnailRequests();
	const std::string pumpTelemetryPath = m_selectedProjectFolder + "|post-draw-pump";
	const size_t pumpTelemetryItemCount = m_thumbnailService.GetQueuedRequestCount();
	AssetBrowserArtifactTelemetryScope pumpTelemetry {
		NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPump,
		std::chrono::steady_clock::now(),
		&pumpTelemetryPath,
		&pumpTelemetryItemCount,
		kAssetBrowserThumbnailUiDrawTelemetryMinimum
	};
	{
		NLS_PROFILE_NAMED_SCOPE("AssetBrowser::PumpThumbnailGeneration.ConsumeCompleted");
		AssetBrowserArtifactTelemetryScope consumeCompletedTelemetry {
			NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpConsumeCompleted,
			std::chrono::steady_clock::now(),
			&pumpTelemetryPath,
			&pumpTelemetryItemCount,
			kAssetBrowserThumbnailUiDrawTelemetryMinimum
		};
		size_t completedThumbnailsConsumedThisPump = 0u;
		bool completedThumbnailQueuedTextureLoad = false;
		while (completedThumbnailsConsumedThisPump < kMaxAssetBrowserCompletedThumbnailConsumesPerPump &&
			m_thumbnailRenderScheduler.TryBeginCompletedResult())
		{
			const auto consumeBegin = std::chrono::steady_clock::now();
			// Global request maintenance already ran once above. Completion
			// consumption must not repeat the full queue/deadline scan for every
			// result admitted in this frame.
			const auto generated = m_thumbnailService.ConsumeCompletedThumbnail(false);
			if (generated.has_value())
			{
				completedThumbnailQueuedTextureLoad =
					completedThumbnailQueuedTextureLoad ||
					(generated->status == NLS::Editor::Assets::AssetThumbnailServiceStatus::Fresh &&
						!generated->imagePath.empty());
				ApplyThumbnailServiceResult(*generated);
				++completedThumbnailsConsumedThisPump;
			}
			m_thumbnailRenderScheduler.FinishWork(
				NLS::Editor::Assets::AssetThumbnailRenderWorkKind::ConsumeCompleted,
				static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - consumeBegin).count()),
				generated.has_value());
			if (!generated.has_value())
				break;
		}
		if (completedThumbnailQueuedTextureLoad)
		{
			StartQueuedCachedThumbnailTextureDecodes(
				thumbnailInteractive
					? kMaxAssetBrowserInteractiveCachedThumbnailTexturePumpsPerFrame
					: kMaxAssetBrowserThumbnailTextureLoadsPerFrame);
		}
	}
	if (IsEditorWindowClosing())
		return;
	if (IsEditorSceneReadbackValidationActive())
		return;

	const double now = ImGui::GetTime();
	if (RecoverVisiblePendingThumbnailPresentations(now) > 0u)
	{
		StartQueuedCachedThumbnailTextureDecodes(
			thumbnailInteractive
				? kMaxAssetBrowserInteractiveCachedThumbnailTexturePumpsPerFrame
				: kMaxAssetBrowserThumbnailTextureLoadsPerFrame);
	}
	const bool hasPreviewRenderer =
		m_thumbnailPreviewRenderer != nullptr ||
		NLS::Render::Context::TryGetLocatedDriver() != nullptr;

	// Visible source textures do not need the GPU preview path, but they do
	// need a worker start before a heavy prefab continuation can consume the
	// frame budget. Admit a bounded texture burst first; the actual decode and
	// PNG write remain on the thumbnail worker queue.
	if (m_thumbnailService.HasQueuedVisibleTextureThumbnailWork())
	{
		size_t visibleTextureStartsThisFrame = 0u;
		const size_t maxVisibleTextureStartsThisFrame = thumbnailInteractive
			? kMaxAssetBrowserInteractiveThumbnailStartsPerFrame
			: kMaxAssetBrowserThumbnailTextureUploadsPerFrame;
		while (visibleTextureStartsThisFrame < maxVisibleTextureStartsThisFrame &&
			m_thumbnailService.HasQueuedVisibleTextureThumbnailWork())
		{
			const NLS::Editor::Assets::AssetBrowserThumbnailPumpInput pumpInput {
				thumbnailInteractive,
				true,
				m_thumbnailService.HasInFlightRequest(),
				visibleTextureStartsThisFrame,
				kMaxAssetBrowserInteractiveThumbnailStartsPerFrame
			};
			if (!m_thumbnailRenderScheduler.TryBeginBackgroundGeneration(pumpInput))
				break;

			const auto workBegin = std::chrono::steady_clock::now();
			const bool started = m_thumbnailService.StartNextThumbnailGeneration();
			m_thumbnailRenderScheduler.FinishWork(
				NLS::Editor::Assets::AssetThumbnailRenderWorkKind::BackgroundGenerationStart,
				static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - workBegin).count()),
				started);
			if (!started)
				break;
			++visibleTextureStartsThisFrame;
		}
	}

	if (allowPreviewRenderWarmup &&
		allowGpuPreviewStart &&
		!thumbnailInteractive &&
		m_standardPbrShaderPassPrewarmCompleted &&
		!IsStandardPbrShaderPassPrewarmPending() &&
		!m_thumbnailPreviewRenderWarmupCompleted &&
		m_thumbnailRenderScheduler.TryBeginPreviewWarmup(true))
	{
		const auto warmupBegin = std::chrono::steady_clock::now();
		const bool warmedPreviewRenderPath = !m_thumbnailPreviewRenderWarmupCompleted;
		PumpThumbnailPreviewRenderWarmup();
		m_thumbnailRenderScheduler.FinishWork(
			NLS::Editor::Assets::AssetThumbnailRenderWorkKind::PreviewWarmup,
			static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - warmupBegin).count()),
			warmedPreviewRenderPath && m_thumbnailPreviewRenderWarmupCompleted);
		if (warmedPreviewRenderPath && m_thumbnailPreviewRenderWarmupCompleted)
			return;
	}

	const bool hasReadyResidentThumbnail =
		m_thumbnailService.HasQueuedReadyResidentThumbnail();
	const bool hasSceneAssemblyContinuation =
		m_thumbnailService.HasQueuedGpuPreviewSceneAssemblyContinuation();
	if (m_thumbnailService.GetQueuedRequestCount() == 0u &&
		!m_thumbnailService.HasInFlightRequest() &&
		!hasReadyResidentThumbnail &&
		!m_thumbnailService.HasQueuedGpuPreviewResourceContinuation())
	{
		return;
	}

	NLS::Editor::Assets::AssetBrowserLightGpuThumbnailPumpInput lightGpuPumpInput;
	lightGpuPumpInput.allowGpuPreviewStart = allowGpuPreviewStart;
	lightGpuPumpInput.interactive = thumbnailInteractive;
	lightGpuPumpInput.hasQueuedWork = m_thumbnailService.GetQueuedRequestCount() > 0u;
	lightGpuPumpInput.hasInFlightWork = m_thumbnailService.HasInFlightRequest();
	// A complete resident prefab may need several assembly slices while keeping
	// its proxy objects alive. Another GPU preview would reuse the same preview
	// scene and reset that cursor, so let the resident continuation finish first.
	lightGpuPumpInput.hasExclusivePreviewContinuation =
		hasReadyResidentThumbnail || hasSceneAssemblyContinuation;
	lightGpuPumpInput.hasPreviewRenderer = hasPreviewRenderer;
	lightGpuPumpInput.standardPbrShaderPassPrewarmPending = IsStandardPbrShaderPassPrewarmPending();
	lightGpuPumpInput.nowSeconds = now;
	lightGpuPumpInput.deferredUntilSeconds = m_lightGpuThumbnailGenerationDeferredUntil;
	if (m_thumbnailRenderScheduler.TryBeginLightGpuPreview(lightGpuPumpInput))
	{
		const auto workBegin = std::chrono::steady_clock::now();
		std::optional<NLS::Editor::Assets::AssetThumbnailServiceResult> generated;
		if (EnsureThumbnailPreviewRenderer())
		{
			NLS_PROFILE_NAMED_SCOPE("AssetBrowser::PumpThumbnailGeneration.StartLightGpuPreview");
			AssetBrowserArtifactTelemetryScope startLightGpuTelemetry {
				NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpStartLightGpu,
				std::chrono::steady_clock::now(),
				&pumpTelemetryPath,
				&pumpTelemetryItemCount,
				kAssetBrowserThumbnailUiDrawTelemetryMinimum
			};
			generated = m_thumbnailService.GenerateNextThumbnail(
				*m_thumbnailRendererRegistry,
				false);
		if (generated.has_value())
			ApplyThumbnailServiceResult(*generated);
		}
		m_thumbnailRenderScheduler.FinishWork(
			NLS::Editor::Assets::AssetThumbnailRenderWorkKind::LightGpuPreview,
			static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - workBegin).count()),
			generated.has_value());
		m_thumbnailRenderScheduler.RecordLightGpuPreviewResult(
			generated.has_value(),
			now,
			kAssetBrowserGpuThumbnailIntervalSeconds);
		if (generated.has_value() &&
			!m_thumbnailService.HasQueuedNonGpuThumbnailWork() &&
			!m_thumbnailService.HasQueuedReadyResidentThumbnail() &&
			!m_thumbnailService.HasQueuedGpuPreviewResourceContinuation())
			return;
	}

	NLS::Editor::Assets::AssetBrowserHeavyGpuThumbnailPumpInput heavyGpuPumpInput;
	heavyGpuPumpInput.allowHeavyGpuPreview = allowHeavyGpuPreview;
	heavyGpuPumpInput.interactive = thumbnailInteractive;
	heavyGpuPumpInput.sceneViewCameraNavigationActive = sceneViewCameraNavigationActive;
	heavyGpuPumpInput.hasQueuedWork = m_thumbnailService.GetQueuedRequestCount() > 0u;
	heavyGpuPumpInput.hasQueuedNonGpuWork = m_thumbnailService.HasQueuedNonGpuThumbnailWork();
	heavyGpuPumpInput.hasInFlightWork = m_thumbnailService.HasInFlightRequest();
	heavyGpuPumpInput.hasQueuedReadback = m_thumbnailService.HasQueuedGpuPreviewReadback();
	heavyGpuPumpInput.hasQueuedResourceContinuation =
		m_thumbnailService.HasQueuedGpuPreviewResourceContinuation();
	heavyGpuPumpInput.hasPreviewRenderer = hasPreviewRenderer;
	const bool sceneLoadRendererResourcesPending =
		NLS::Editor::Core::HasBlockingSceneLoadRendererResourceResolution();
	if (sceneLoadRendererResourcesPending)
	{
		if (m_sceneLoadThumbnailGateStartedAt == 0.0)
			m_sceneLoadThumbnailGateStartedAt = now;
	}
	else
	{
		m_sceneLoadThumbnailGateStartedAt = 0.0;
	}
    const bool sceneLoadThumbnailEscapeHatchActive =
        sceneLoadRendererResourcesPending &&
        m_sceneLoadThumbnailGateStartedAt != 0.0 &&
		now - m_sceneLoadThumbnailGateStartedAt >=
			kAssetBrowserSceneLoadThumbnailEscapeSeconds;
	// Scene loading gets the first opportunity to use the renderer budget, but
	// it must not hold visible thumbnails in a non-terminal state indefinitely.
    // Once the gate has exceeded its grace period, the scheduler's normal budget
    // and previous-frame headroom rules control the bounded thumbnail progress.
    heavyGpuPumpInput.sceneLoadRendererResourcesPending =
        sceneLoadRendererResourcesPending;
    heavyGpuPumpInput.sceneLoadThumbnailEscapeHatchActive =
        sceneLoadThumbnailEscapeHatchActive;
	heavyGpuPumpInput.hasQueuedVisibleResidentPreview =
		m_thumbnailService.HasQueuedVisibleResidentThumbnail();
	const bool readyResidentInput = m_thumbnailService.HasQueuedReadyResidentThumbnail();
	heavyGpuPumpInput.hasQueuedReadyResidentPreview = readyResidentInput;
	heavyGpuPumpInput.nowSeconds = now;
	heavyGpuPumpInput.deferredUntilSeconds = m_heavyGpuThumbnailGenerationDeferredUntil;
	if (m_thumbnailRenderScheduler.TryBeginHeavyGpuPreview(heavyGpuPumpInput))
	{
		const auto workBegin = std::chrono::steady_clock::now();
		std::optional<NLS::Editor::Assets::AssetThumbnailServiceResult> generated;
		if (EnsureThumbnailPreviewRenderer())
		{
			NLS_PROFILE_NAMED_SCOPE("AssetBrowser::PumpThumbnailGeneration.StartHeavyGpuPreview");
			AssetBrowserArtifactTelemetryScope startHeavyGpuTelemetry {
				NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpStartHeavyGpu,
				std::chrono::steady_clock::now(),
				&pumpTelemetryPath,
				&pumpTelemetryItemCount,
				kAssetBrowserThumbnailUiDrawTelemetryMinimum
			};
			generated = m_thumbnailService.GenerateNextThumbnail(
				*m_thumbnailRendererRegistry,
				true);
			if (generated.has_value())
				ApplyThumbnailServiceResult(*generated);
		}
		m_thumbnailRenderScheduler.FinishActiveWork(
			static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - workBegin).count()),
			generated.has_value());
		m_thumbnailRenderScheduler.RecordHeavyGpuPreviewResult(
			generated.has_value(),
			generated.has_value() &&
				generated->status == NLS::Editor::Assets::AssetThumbnailServiceStatus::Pending,
			generated.has_value()
				? std::string_view(generated->diagnostic)
				: std::string_view {},
			now,
			kAssetBrowserHeavyGpuThumbnailResourcePendingIntervalSeconds,
			kAssetBrowserHeavyGpuThumbnailIntervalSeconds);
		if (generated.has_value())
		{
			const bool hasQueuedNonGpuWork = m_thumbnailService.HasQueuedNonGpuThumbnailWork();
			// A GPU result, including a readback-pending result, is not a reason to
			// stop the CPU/texture lane. Continue to the bounded background turn
			// whenever other work is queued; otherwise a large prefab can starve
			// visible textures behind its readback cadence. Resource continuations
			// remain represented in their own lane for the next pump.
			if (!hasQueuedNonGpuWork)
				return;
		}
	}

	size_t thumbnailStartsThisFrame = 0u;
	while (true)
	{
		const bool hasQueuedNonGpuWork = m_thumbnailService.HasQueuedNonGpuThumbnailWork();
		if (!hasQueuedNonGpuWork)
			break;
		const NLS::Editor::Assets::AssetBrowserThumbnailPumpInput pumpInput {
			thumbnailInteractive,
			hasQueuedNonGpuWork,
			m_thumbnailService.HasInFlightRequest(),
			thumbnailStartsThisFrame,
			kMaxAssetBrowserInteractiveThumbnailStartsPerFrame
		};
		if (!m_thumbnailRenderScheduler.TryBeginBackgroundGeneration(pumpInput))
		{
			break;
		}

		const auto workBegin = std::chrono::steady_clock::now();
		NLS_PROFILE_NAMED_SCOPE("AssetBrowser::PumpThumbnailGeneration.StartBackground");
		AssetBrowserArtifactTelemetryScope startBackgroundTelemetry {
			NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiPostDrawPumpStartBackground,
			std::chrono::steady_clock::now(),
			&pumpTelemetryPath,
			&pumpTelemetryItemCount,
			kAssetBrowserThumbnailUiDrawTelemetryMinimum
		};
		const bool started = m_thumbnailService.StartNextThumbnailGeneration();
		m_thumbnailRenderScheduler.FinishWork(
			NLS::Editor::Assets::AssetThumbnailRenderWorkKind::BackgroundGenerationStart,
			static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - workBegin).count()),
			started);
		if (!started)
			break;
		++thumbnailStartsThisFrame;
	}
}

void Editor::Panels::AssetBrowser::SchedulePrefabHotCachePreloadForDragPayload(
	const NLS::Editor::Assets::EditorAssetDragPayload& payload)
{
	const auto begin = std::chrono::steady_clock::now();
	const auto path = NLS::Editor::Assets::GetEditorAssetDragPayloadPath(payload);
	if (path.empty())
		return;
	const auto key = path + "|" +
		NLS::Editor::Assets::GetEditorAssetDragPayloadGuid(payload) + "|" +
		NLS::Editor::Assets::GetEditorAssetDragPayloadSubAssetKey(payload);
	{
		std::lock_guard lock(AssetBrowserPrefabHotCachePreloadMutex());
			if (!AssetBrowserPrefabHotCachePreloadGate().TryBegin(
					key,
					NLS::Editor::Core::RecentBackgroundWorkGate::Clock::now()))
			{
				NLS::Core::Assets::RecordArtifactLoadTelemetry({
					NLS::Core::Assets::ArtifactLoadTelemetryStage::PrefabVisiblePrewarmSchedule,
					std::chrono::duration_cast<std::chrono::microseconds>(
						std::chrono::steady_clock::now() - begin),
					0u,
					path});
				return;
			}
	}

	const auto projectAssetsPath = std::filesystem::path(m_projectAssetFolder);
	const bool scheduled = EDITOR_EXEC(TrackOpportunisticBackgroundTask(
		[payload, projectAssetsPath, key, path]
		{
			const auto preloadBegin = std::chrono::steady_clock::now();
			auto completion = AssetBrowserPrefabHotCachePreloadGate().CompleteOnScopeExit(key);
			try
			{
				NLS_LOG_INFO("Prefab hot-cache prewarm started: " + path);
				NLS::Editor::Assets::EditorAssetDragDropBridge bridge(projectAssetsPath);
				const bool ready = bridge.PreloadImportedAssetHandlePrefabHotCache(payload);
				const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - preloadBegin);
				NLS_LOG_INFO(
					std::string("Prefab hot-cache prewarm ") +
					(ready ? "ready: " : "not ready: ") +
					path +
					" elapsedMs=" +
					std::to_string(elapsedMs.count()));
			}
			catch (const std::exception& exception)
			{
				NLS_LOG_WARNING(std::string("Imported prefab hot-cache preload failed: ") + exception.what());
			}
			catch (...)
			{
				NLS_LOG_WARNING("Imported prefab hot-cache preload failed with an unknown exception.");
			}
		}));
	if (!scheduled)
	{
		AssetBrowserPrefabHotCachePreloadGate().End(key);
		NLS_LOG_WARNING("Prefab hot-cache prewarm failed to schedule: " + path);
	}
	else
	{
		NLS_LOG_INFO("Prefab hot-cache prewarm queued: " + path);
	}
	NLS::Core::Assets::RecordArtifactLoadTelemetry({
		NLS::Core::Assets::ArtifactLoadTelemetryStage::PrefabVisiblePrewarmSchedule,
		std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - begin),
		scheduled ? 1u : 0u,
		path});
}

void Editor::Panels::AssetBrowser::SchedulePrefabHotCachePreloadForHoveredItem(
    const NLS::Editor::Assets::AssetBrowserItem& item,
    const bool hovered)
{
    if (!hovered ||
        (item.type != NLS::Editor::Assets::AssetBrowserItemType::Model &&
            item.type != NLS::Editor::Assets::AssetBrowserItemType::Prefab))
    {
        return;
    }

    if (item.kind != NLS::Editor::Assets::AssetBrowserItemKind::SourceAsset &&
        item.kind != NLS::Editor::Assets::AssetBrowserItemKind::GeneratedSubAsset)
    {
        return;
    }

    const double now = ImGui::GetTime();
    if (m_lastHoveredPrefabHotCachePreloadIdentity.Matches(item) &&
        now - m_lastHoveredPrefabHotCachePreloadTime < kAssetBrowserHoveredPrefabHotCachePreloadRepeatDelaySeconds)
    {
        return;
    }
    m_lastHoveredPrefabHotCachePreloadIdentity.Store(item);
    m_lastHoveredPrefabHotCachePreloadTime = now;

    auto payload = NLS::Editor::Assets::MakeAssetBrowserItemDragPayload(item, nullptr);
    if (payload.has_value())
        SchedulePrefabHotCachePreloadForDragPayload(*payload);
}

void Editor::Panels::AssetBrowser::SchedulePrefabHotCachePreloadForVisibleItems(
    const std::vector<NLS::Editor::Assets::AssetBrowserItem>& visibleItems)
{
    for (const auto& item : visibleItems)
    {
        if (item.type != NLS::Editor::Assets::AssetBrowserItemType::Model &&
            item.type != NLS::Editor::Assets::AssetBrowserItemType::Prefab)
        {
            continue;
        }

        if (item.kind != NLS::Editor::Assets::AssetBrowserItemKind::SourceAsset &&
            item.kind != NLS::Editor::Assets::AssetBrowserItemKind::GeneratedSubAsset)
        {
            continue;
        }

        auto payload = NLS::Editor::Assets::MakeAssetBrowserItemDragPayload(item, nullptr);
        if (payload.has_value())
            SchedulePrefabHotCachePreloadForDragPayload(*payload);
    }
}

void Editor::Panels::AssetBrowser::FlushPendingVisiblePrefabHotCachePreload()
{
    if (!m_visiblePrefabHotCachePreloadPending || IsAssetBrowserInteractive())
        return;

    m_visiblePrefabHotCachePreloadPending = false;
    SchedulePrefabHotCachePreloadForVisibleItems(m_visibleThumbnailItems);
}

bool Editor::Panels::AssetBrowser::IsResidentPrefabPreviewAvailableForItem(
    const NLS::Editor::Assets::AssetBrowserItem& item) const
{
    if (!m_thumbnailFeatureConfig.residentPrefabPreview ||
        m_residentPrefabPreviewRegistry == nullptr ||
        !item.assetId.IsValid() ||
        (item.type != NLS::Editor::Assets::AssetBrowserItemType::Model &&
            item.type != NLS::Editor::Assets::AssetBrowserItemType::Prefab))
    {
        return false;
    }

    // Reuse the same manifest, canonical sub-asset and freshness construction
    // as the real thumbnail request. A path-only or identity-only probe could
    // incorrectly suppress the fallback after an importer change.
    auto context = MakeAssetBrowserThumbnailRequestBuildContext(m_thumbnailFeatureConfig);
    context.assetDatabaseSnapshot = m_projectAssetDatabaseSnapshot;
    context.residentPrefabPreviewRegistry = m_residentPrefabPreviewRegistry;
    const auto request = NLS::Editor::Assets::BuildAssetThumbnailRequestForItem(
        ProjectRootFromAssetsFolder(m_projectAssetFolder),
        item,
        AssetBrowserThumbnailRequestSize(m_thumbnailSize),
        context);
    if (!request ||
        request->kind != NLS::Editor::Assets::AssetThumbnailKind::PrefabPreview ||
        !request->residentPrefabPreviewSource.has_value())
    {
        const auto canonicalSubAssetKey =
            NLS::Editor::Assets::BuildCanonicalPrefabPreviewSubAssetKey(
                item.sourceAssetPath,
                item.subAssetKey);
        if (canonicalSubAssetKey.empty())
            return false;
        return m_residentPrefabPreviewRegistry->HasSnapshotForRuntimeCacheIdentity(
            NLS::Editor::Assets::BuildResidentPrefabRuntimeCacheIdentity(
                item.assetId.ToString(),
                canonicalSubAssetKey));
    }

    if (!request->residentPrefabPreviewSource->snapshot.expired())
        return true;

    // The first visible set can be built before the immutable database snapshot
    // is published. Identity-only suppression is safe here because this helper
    // only avoids redundant hot-cache work; generation still requires the exact
    // freshness fingerprint and falls back on mismatch.
    return m_residentPrefabPreviewRegistry->HasSnapshotForRuntimeCacheIdentity(
        request->residentPrefabPreviewSource->runtimeCacheIdentity);
}

bool Editor::Panels::AssetBrowser::ShouldHoldResidentPrefabThumbnailFallback(
    const NLS::Editor::Assets::AssetBrowserItem& item,
    const NLS::Editor::Assets::AssetThumbnailServiceResult* result) const
{
    if (!m_thumbnailFeatureConfig.residentPrefabPreview ||
        m_residentPrefabPreviewRegistry == nullptr ||
        (item.type != NLS::Editor::Assets::AssetBrowserItemType::Model &&
            item.type != NLS::Editor::Assets::AssetBrowserItemType::Prefab))
    {
        return false;
    }

    if (result != nullptr)
    {
        // A resident request is still provisional until it has a display image
        // or reaches an explicit terminal failure/fallback state.
        if (result->status != NLS::Editor::Assets::AssetThumbnailServiceStatus::Pending ||
            NLS::Editor::Assets::AssetBrowserThumbnailResultHasDisplayImage(*result))
        {
            return false;
        }
        return result->residentPreviewRequest ||
            m_residentPrefabPreviewRegistry->IsSceneRestoreInProgress() ||
            IsResidentPrefabPreviewAvailableForItem(item);
    }

    // The first grid draw happens before UpdateThumbnailGenerationScope queues
    // the request. Keep a loaded scene object in a stable loading state during
    // that one-frame gap instead of exposing the type icon.
    if (m_residentPrefabPreviewRegistry->IsSceneRestoreInProgress())
        return true;

    return IsResidentPrefabPreviewAvailableForItem(item);
}

bool Editor::Panels::AssetBrowser::LoadCachedThumbnailTexture(
			const std::string& normalizedPath)
{
	const auto foundColorSpace = m_thumbnailTextureColorSpacesByPath.find(normalizedPath);
	const auto colorSpace = foundColorSpace != m_thumbnailTextureColorSpacesByPath.end()
		? foundColorSpace->second
		: NLS::Render::RHI::TextureColorSpace::Linear;
	return LoadDecodedCachedThumbnailTexture(
		DecodeCachedThumbnailTexture(normalizedPath, colorSpace));
}

void Editor::Panels::AssetBrowser::MarkCachedThumbnailTextureUploadRetryableFailure(
	const std::string& normalizedPath)
{
	if (normalizedPath.empty())
		return;
	m_thumbnailTextureRetryAfterFrameByPath[normalizedPath] =
		m_thumbnailTextureFrameSerial + kAssetBrowserThumbnailTextureRetryDelayFrames;
}

bool Editor::Panels::AssetBrowser::LoadDecodedCachedThumbnailTexture(
	ThumbnailTextureDecodeResult result)
{
	const auto normalizedPath = std::move(result.normalizedPath);
		if (normalizedPath.empty() ||
			result.rgbaPixels.empty() ||
			result.width == 0u ||
			result.height == 0u ||
			!NLS::Editor::Assets::IsAssetBrowserCachedThumbnailTextureSizeAllowed(
				result.width,
				result.height,
				kMaxAssetBrowserCachedThumbnailTextureDimension) ||
			m_thumbnailTexturesByPath.find(normalizedPath) != m_thumbnailTexturesByPath.end() ||
			m_pendingThumbnailTextureUploadsByPath.find(normalizedPath) != m_pendingThumbnailTextureUploadsByPath.end() ||
			m_thumbnailTexturesFailedToLoad.find(normalizedPath) != m_thumbnailTexturesFailedToLoad.end())
	{
		if (!normalizedPath.empty() && result.rgbaPixels.empty())
		{
			m_thumbnailTexturesFailedToLoad.insert(normalizedPath);
		}
		return false;
	}

	auto* driver = NLS::Render::Context::TryGetLocatedDriver();
	std::optional<NLS::Editor::Assets::AssetThumbnailAtlas::Allocation> atlasAllocation;
	const auto atlasSizeClass =
		NLS::Editor::Assets::AssetThumbnailAtlas::SizeClassForDimensions(
			result.width,
			result.height);
	if (m_thumbnailFeatureConfig.atlas &&
		driver != nullptr &&
		NLS::Render::Context::DriverUIAccess::SupportsUiRgba8TextureAtlasRegionUploads(*driver) &&
		atlasSizeClass.has_value())
	{
		const auto bucketKey = std::string(
			result.colorSpace == NLS::Render::RHI::TextureColorSpace::SRGB
				? "srgb-rgba8-"
				: "linear-rgba8-") +
			std::to_string(*atlasSizeClass);
		auto allocationResult = m_thumbnailAtlas.Allocate(
			normalizedPath,
			result.width,
			result.height,
			bucketKey,
			m_thumbnailTextureFrameSerial);
		for (const auto& evictedKey : allocationResult.evictedKeys)
		{
			if (evictedKey != normalizedPath)
				ReleaseCachedThumbnailTexture(evictedKey);
		}
		atlasAllocation = std::move(allocationResult.allocation);
	}

		const size_t uploadByteCount = result.rgbaPixels.size();
		AssetBrowserArtifactTelemetryScope uploadTelemetry {
			NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTextureUploadEnqueue,
			std::chrono::steady_clock::now(),
				&normalizedPath,
				&uploadByteCount
		};
		if (driver == nullptr)
		{
			if (atlasAllocation.has_value())
				m_thumbnailAtlas.Release(normalizedPath);
			MarkCachedThumbnailTextureUploadRetryableFailure(normalizedPath);
			return false;
		}

		NLS::Render::Context::DriverUIAccess::Rgba8TextureUploadRequest uploadRequest;
		uploadRequest.width = result.width;
		uploadRequest.height = result.height;
		uploadRequest.rgbaPixels = std::move(result.rgbaPixels);
		uploadRequest.debugName = normalizedPath;
		uploadRequest.colorSpace = result.colorSpace;
		if (atlasAllocation.has_value())
		{
			const uint32_t paddedWidth = result.width + NLS::Editor::Assets::AssetThumbnailAtlas::kGutter * 2u;
			const uint32_t paddedHeight = result.height + NLS::Editor::Assets::AssetThumbnailAtlas::kGutter * 2u;
			std::vector<uint8_t> paddedPixels(
				static_cast<size_t>(paddedWidth) * static_cast<size_t>(paddedHeight) * 4u,
				0u);
			for (uint32_t y = 0u; y < paddedHeight; ++y)
			{
				const uint32_t sourceY = (std::min)(
					result.height - 1u,
					y < NLS::Editor::Assets::AssetThumbnailAtlas::kGutter
						? 0u
						: y - NLS::Editor::Assets::AssetThumbnailAtlas::kGutter);
				for (uint32_t x = 0u; x < paddedWidth; ++x)
				{
					const uint32_t sourceX = (std::min)(
						result.width - 1u,
						x < NLS::Editor::Assets::AssetThumbnailAtlas::kGutter
							? 0u
							: x - NLS::Editor::Assets::AssetThumbnailAtlas::kGutter);
					const auto sourceIndex =
						(static_cast<size_t>(sourceY) * result.width + sourceX) * 4u;
					const auto targetIndex =
						(static_cast<size_t>(y) * paddedWidth + x) * 4u;
					std::copy_n(uploadRequest.rgbaPixels.data() + sourceIndex, 4u, paddedPixels.data() + targetIndex);
				}
			}
			uploadRequest.width = paddedWidth;
			uploadRequest.height = paddedHeight;
			uploadRequest.rgbaPixels = std::move(paddedPixels);
			uploadRequest.atlasPageKey = atlasAllocation->pageKey;
			uploadRequest.atlasPageSize = NLS::Editor::Assets::AssetThumbnailAtlas::kPageSize;
			uploadRequest.atlasX = atlasAllocation->x - NLS::Editor::Assets::AssetThumbnailAtlas::kGutter;
			uploadRequest.atlasY = atlasAllocation->y - NLS::Editor::Assets::AssetThumbnailAtlas::kGutter;
		}
		const uint64_t requestId = NLS::Render::Context::DriverUIAccess::RequestUiRgba8TextureUpload(
			*driver,
			std::move(uploadRequest));
		if (requestId == 0u)
		{
			if (atlasAllocation.has_value())
				m_thumbnailAtlas.Release(normalizedPath);
			MarkCachedThumbnailTextureUploadRetryableFailure(normalizedPath);
			return false;
		}
		m_thumbnailTextureRetryAfterFrameByPath.erase(normalizedPath);

		m_pendingThumbnailTextureUploadsByPath[normalizedPath] = {
			requestId,
			result.width,
			result.height,
			std::move(atlasAllocation)
		};
		return true;
	}

Editor::Panels::AssetBrowser::ThumbnailTextureDecodeResult
Editor::Panels::AssetBrowser::DecodeCachedThumbnailTexture(
	std::string normalizedPath,
	const NLS::Render::RHI::TextureColorSpace colorSpace)
{
	ThumbnailTextureDecodeResult result;
	result.normalizedPath = std::move(normalizedPath);
	result.colorSpace = colorSpace;
	size_t decodeByteCount = 0u;
	AssetBrowserArtifactTelemetryScope decodeTelemetry {
		NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTextureDecode,
		std::chrono::steady_clock::now(),
		&result.normalizedPath,
		&decodeByteCount
	};
	if (result.normalizedPath.empty())
		return result;

	try
	{
		// The async standalone and Atlas upload paths both use top-left UVs.
		// Preserve PNG row order instead of applying the legacy TextureLoader flip.
		NLS::Image image(result.normalizedPath, false);
		const auto* source = image.GetData();
		const auto width = image.GetWidth();
		const auto height = image.GetHeight();
		const auto channels = image.GetChannels();
		if (source == nullptr || width <= 0 || height <= 0 || channels <= 0 || channels > 4)
			return result;

		result.width = static_cast<uint32_t>(width);
		result.height = static_cast<uint32_t>(height);
		if (!NLS::Editor::Assets::IsAssetBrowserCachedThumbnailTextureSizeAllowed(
				result.width,
				result.height,
				kMaxAssetBrowserCachedThumbnailTextureDimension))
		{
			result.width = 0u;
			result.height = 0u;
			return result;
		}

		const auto pixelCount = static_cast<size_t>(result.width) * static_cast<size_t>(result.height);
		result.rgbaPixels.resize(pixelCount * 4u, 255u);
		for (size_t pixel = 0u; pixel < pixelCount; ++pixel)
		{
			const auto sourceIndex = pixel * static_cast<size_t>(channels);
			const auto targetIndex = pixel * 4u;
			switch (channels)
			{
			case 1:
				result.rgbaPixels[targetIndex + 0u] = source[sourceIndex + 0u];
				result.rgbaPixels[targetIndex + 1u] = source[sourceIndex + 0u];
				result.rgbaPixels[targetIndex + 2u] = source[sourceIndex + 0u];
				break;
			case 2:
				result.rgbaPixels[targetIndex + 0u] = source[sourceIndex + 0u];
				result.rgbaPixels[targetIndex + 1u] = source[sourceIndex + 0u];
				result.rgbaPixels[targetIndex + 2u] = source[sourceIndex + 0u];
				result.rgbaPixels[targetIndex + 3u] = source[sourceIndex + 1u];
				break;
			case 3:
				result.rgbaPixels[targetIndex + 0u] = source[sourceIndex + 0u];
				result.rgbaPixels[targetIndex + 1u] = source[sourceIndex + 1u];
				result.rgbaPixels[targetIndex + 2u] = source[sourceIndex + 2u];
				break;
			case 4:
				result.rgbaPixels[targetIndex + 0u] = source[sourceIndex + 0u];
				result.rgbaPixels[targetIndex + 1u] = source[sourceIndex + 1u];
				result.rgbaPixels[targetIndex + 2u] = source[sourceIndex + 2u];
				result.rgbaPixels[targetIndex + 3u] = source[sourceIndex + 3u];
				break;
			default:
				result.rgbaPixels.clear();
				break;
			}
		}
	}
	catch (const std::bad_alloc&)
	{
		result.rgbaPixels.clear();
		result.width = 0u;
		result.height = 0u;
	}
	catch (...)
	{
		result.rgbaPixels.clear();
		result.width = 0u;
		result.height = 0u;
	}
	decodeByteCount = result.rgbaPixels.size();
	return result;
}

void Editor::Panels::AssetBrowser::QueueCachedThumbnailTextureLoad(
	const std::filesystem::path& imagePath,
	const NLS::Render::RHI::TextureColorSpace colorSpace)
{
	if (imagePath.empty())
		return;

		const auto normalizedPath = imagePath.lexically_normal().generic_string();
		if (!normalizedPath.empty())
			m_thumbnailTextureColorSpacesByPath.insert_or_assign(normalizedPath, colorSpace);
			if (normalizedPath.empty() ||
				m_thumbnailTexturesByPath.find(normalizedPath) != m_thumbnailTexturesByPath.end() ||
				m_pendingThumbnailTextureUploadsByPath.find(normalizedPath) != m_pendingThumbnailTextureUploadsByPath.end() ||
				m_thumbnailTexturesQueuedForLoad.find(normalizedPath) != m_thumbnailTexturesQueuedForLoad.end() ||
				m_thumbnailTexturesFailedToLoad.find(normalizedPath) != m_thumbnailTexturesFailedToLoad.end())
			{
			return;
		}
		if (const auto retry = m_thumbnailTextureRetryAfterFrameByPath.find(normalizedPath);
			retry != m_thumbnailTextureRetryAfterFrameByPath.end())
		{
			if (m_thumbnailTextureFrameSerial < retry->second)
				return;
			m_thumbnailTextureRetryAfterFrameByPath.erase(retry);
		}

		m_thumbnailTextureLoadQueue.push_back(normalizedPath);
		m_thumbnailTexturesQueuedForLoad.insert(normalizedPath);
}

void Editor::Panels::AssetBrowser::PumpQueuedCachedThumbnailTextureLoads(const size_t maxDecodeStartsPerFrame)
{
	ConsumeCompletedCachedThumbnailTextureDecodes();
	StartQueuedCachedThumbnailTextureDecodes(maxDecodeStartsPerFrame);
}

void Editor::Panels::AssetBrowser::StartQueuedCachedThumbnailTextureDecodes(const size_t maxDecodeStartsPerFrame)
{
	if (m_thumbnailTextureLoadQueue.empty())
		return;

	const std::string telemetryPath = m_selectedProjectFolder + "|texture-pump-start-decodes";
	const size_t telemetryItemCount = m_thumbnailTextureLoadQueue.size() + m_thumbnailTextureDecodes.size();
	AssetBrowserArtifactTelemetryScope startTelemetry {
		NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTexturePumpStartDecodes,
		std::chrono::steady_clock::now(),
		&telemetryPath,
		&telemetryItemCount,
		kAssetBrowserThumbnailUiDrawTelemetryMinimum
	};
	const auto startBudget = NLS::Editor::Assets::AssetBrowserThumbnailTextureDecodeStartBudget(
		m_thumbnailTextureDecodes.size(),
		kMaxAssetBrowserThumbnailTextureDecodesInFlight);
	if (startBudget == 0u)
		return;

	std::vector<std::string> candidates;
	{
		const std::string selectTelemetryPath = m_selectedProjectFolder + "|texture-pump-select-decodes";
		const size_t selectTelemetryItemCount = m_thumbnailTextureLoadQueue.size();
		AssetBrowserArtifactTelemetryScope selectTelemetry {
			NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTexturePumpSelectDecodeCandidates,
			std::chrono::steady_clock::now(),
			&selectTelemetryPath,
			&selectTelemetryItemCount,
			kAssetBrowserThumbnailUiDrawTelemetryMinimum
		};
		candidates = NLS::Editor::Assets::SelectAssetBrowserThumbnailTextureDecodeCandidates(
				m_thumbnailTextureLoadQueue,
				[this](const std::string& key)
				{
					return m_thumbnailTexturesByPath.find(key) != m_thumbnailTexturesByPath.end();
				},
				m_thumbnailTexturesDecoding,
				std::min({ kMaxAssetBrowserThumbnailTextureLoadsPerFrame, maxDecodeStartsPerFrame, startBudget }));
	}
	{
		const std::string scheduleTelemetryPath = m_selectedProjectFolder + "|texture-pump-schedule-decodes";
		const size_t scheduleTelemetryItemCount = candidates.size();
		AssetBrowserArtifactTelemetryScope scheduleTelemetry {
			NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTexturePumpScheduleDecodeJobs,
			std::chrono::steady_clock::now(),
			&scheduleTelemetryPath,
			&scheduleTelemetryItemCount,
			kAssetBrowserThumbnailUiDrawTelemetryMinimum
		};
		for (const auto& key : candidates)
		{
			m_thumbnailTexturesDecoding.insert(key);
			const auto foundColorSpace = m_thumbnailTextureColorSpacesByPath.find(key);
			const auto colorSpace = foundColorSpace != m_thumbnailTextureColorSpacesByPath.end()
				? foundColorSpace->second
				: NLS::Render::RHI::TextureColorSpace::Linear;
			try
			{
				m_thumbnailTextureDecodes.push_back({
					key,
					colorSpace,
					ScheduleAssetBrowserJobFuture(
						"AssetBrowser.DecodeCachedThumbnailTexture",
						[key, colorSpace]
						{
							return DecodeCachedThumbnailTexture(key, colorSpace);
						})
				});
			}
			catch (...)
			{
				m_thumbnailTexturesDecoding.erase(key);
				m_thumbnailTexturesQueuedForLoad.erase(key);
			}
		}
	}

	m_thumbnailTextureLoadQueue.erase(
		std::remove_if(
			m_thumbnailTextureLoadQueue.begin(),
			m_thumbnailTextureLoadQueue.end(),
			[this](const std::string& key)
			{
				return m_thumbnailTexturesQueuedForLoad.find(key) == m_thumbnailTexturesQueuedForLoad.end() ||
					m_thumbnailTexturesByPath.find(key) != m_thumbnailTexturesByPath.end() ||
					m_thumbnailTexturesFailedToLoad.find(key) != m_thumbnailTexturesFailedToLoad.end();
			}),
		m_thumbnailTextureLoadQueue.end());
}

void Editor::Panels::AssetBrowser::ConsumeCompletedCachedThumbnailTextureDecodes()
{
	const auto pumpBegin = std::chrono::steady_clock::now();
	const std::string consumeTelemetryPath = m_selectedProjectFolder + "|texture-pump-consume";
	const size_t consumeTelemetryItemCount =
		m_pendingThumbnailTextureUploadsByPath.size() + m_thumbnailTextureDecodes.size();
	AssetBrowserArtifactTelemetryScope consumeTelemetry {
		NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTexturePumpConsumeCompleted,
		pumpBegin,
		&consumeTelemetryPath,
		&consumeTelemetryItemCount,
		kAssetBrowserThumbnailUiDrawTelemetryMinimum
	};
		size_t completedUploadsPublishedThisFrame = 0u;
		size_t queuedUploadsThisFrame = 0u;
		size_t deferredReadyUploadsThisFrame = 0u;
		std::string deferredReadyUploadTelemetryPath;
		const size_t uploadBudget = IsAssetBrowserInteractive()
			? kMaxAssetBrowserInteractiveThumbnailTextureUploadsPerFrame
			: kMaxAssetBrowserThumbnailTextureUploadsPerFrame;
		const size_t completedPublishBudget = IsAssetBrowserInteractive()
			? kMaxAssetBrowserInteractiveCachedThumbnailTexturePumpsPerFrame
			: kMaxAssetBrowserThumbnailTextureLoadsPerFrame;
		size_t pendingUploadsPolledThisFrame = 0u;
		const size_t pendingUploadPollBudget = IsAssetBrowserInteractive()
			? kMaxAssetBrowserInteractiveThumbnailTextureUploadPollsPerFrame
			: kMaxAssetBrowserThumbnailTextureUploadPollsPerFrame;
		if (!m_pendingThumbnailTextureUploadsByPath.empty())
		{
			auto* driver = NLS::Render::Context::TryGetLocatedDriver();
		{
			const std::string pollTelemetryPath = m_selectedProjectFolder + "|texture-pump-pending-uploads";
			const size_t pollTelemetryItemCount = m_pendingThumbnailTextureUploadsByPath.size();
			AssetBrowserArtifactTelemetryScope pollTelemetry {
				NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadPoll,
				std::chrono::steady_clock::now(),
				&pollTelemetryPath,
				&pollTelemetryItemCount,
				kAssetBrowserThumbnailUiDrawTelemetryMinimum
			};
				auto iterator = m_nextPendingThumbnailTextureUploadPollPath.empty()
					? m_pendingThumbnailTextureUploadsByPath.begin()
					: m_pendingThumbnailTextureUploadsByPath.find(m_nextPendingThumbnailTextureUploadPollPath);
				if (iterator == m_pendingThumbnailTextureUploadsByPath.end())
					iterator = m_pendingThumbnailTextureUploadsByPath.begin();
				for (; driver != nullptr && iterator != m_pendingThumbnailTextureUploadsByPath.end();)
			{
				const auto elapsedMicroseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - pumpBegin).count());
					if (!NLS::Editor::Assets::ShouldContinueAssetBrowserThumbnailTextureUploads({
							completedUploadsPublishedThisFrame,
							completedPublishBudget,
							pendingUploadsPolledThisFrame,
							pendingUploadPollBudget,
							elapsedMicroseconds,
							kMaxAssetBrowserThumbnailTextureUploadMicrosecondsPerFrame }))
					{
						break;
					}

					const std::string normalizedPath = iterator->first;
					const auto pendingUpload = iterator->second;
					const bool atlasUpload = pendingUpload.atlasAllocation.has_value();
					++pendingUploadsPolledThisFrame;
					decltype(NLS::Render::Context::DriverUIAccess::ConsumeUiRgba8TextureUploadResult(
						*driver,
						pendingUpload.requestId)) result;
					{
						const size_t pendingUploadCount = m_pendingThumbnailTextureUploadsByPath.size();
						AssetBrowserArtifactTelemetryScope consumeResultTelemetry {
							NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadConsumeResult,
							std::chrono::steady_clock::now(),
							&normalizedPath,
							&pendingUploadCount
						};
						result = NLS::Render::Context::DriverUIAccess::ConsumeUiRgba8TextureUploadResult(
							*driver,
							pendingUpload.requestId);
					}
					if (!result.ready)
					{
						++iterator;
						continue;
					}

				if (!result.success || result.texture == nullptr || result.textureView == nullptr ||
				!NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
				{
					if (atlasUpload)
						m_thumbnailAtlas.Release(normalizedPath);
					MarkCachedThumbnailTextureUploadRetryableFailure(normalizedPath);
					iterator = m_pendingThumbnailTextureUploadsByPath.erase(iterator);
					continue;
				}

				const size_t uploadByteCount =
					static_cast<size_t>(pendingUpload.width) *
					static_cast<size_t>(pendingUpload.height) *
					4u;
				void* textureId = nullptr;
					{
						AssetBrowserArtifactTelemetryScope resolveUploadTelemetry {
							NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTextureUploadResolveUiId,
							std::chrono::steady_clock::now(),
							&normalizedPath,
							&uploadByteCount
						};
						AssetBrowserArtifactTelemetryScope resolveUiIdTelemetry {
							NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadResolveUiId,
							std::chrono::steady_clock::now(),
							&normalizedPath,
							&uploadByteCount
						};
						if (atlasUpload)
						{
							const auto& allocation = *pendingUpload.atlasAllocation;
							auto& page = m_thumbnailAtlasPagesByKey[allocation.pageKey];
							if (page.textureView == nullptr || page.textureView != result.textureView)
							{
								if (page.textureView != nullptr &&
									NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
								{
									NLS_SERVICE(NLS::UI::UIManager).ReleaseTextureViewHandle(page.textureView);
								}
								page.texture = result.texture;
								page.textureView = result.textureView;
								page.textureId = nullptr;
                            page.pageSize = NLS::Editor::Assets::AssetThumbnailAtlas::kPageSize;
                            page.pageGeneration = allocation.pageGeneration;
							}
							if (page.textureView != nullptr && page.textureId == nullptr)
								page.textureId = NLS_SERVICE(NLS::UI::UIManager).ResolveTextureId(page.textureView);
							textureId = page.textureId;
						}
						else
						{
							textureId = NLS_SERVICE(NLS::UI::UIManager).ResolveTextureId(result.textureView);
						}
					}
					if (textureId == nullptr)
				{
					if (atlasUpload)
						m_thumbnailAtlas.Release(normalizedPath);
					NLS_SERVICE(NLS::UI::UIManager).ReleaseTextureViewHandle(result.textureView);
					MarkCachedThumbnailTextureUploadRetryableFailure(normalizedPath);
					iterator = m_pendingThumbnailTextureUploadsByPath.erase(iterator);
					continue;
				}

					std::unique_ptr<NLS::Render::Resources::Texture2D> texture;
					if (!atlasUpload)
					{
						AssetBrowserArtifactTelemetryScope wrapTextureTelemetry {
							NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadWrapTexture,
							std::chrono::steady_clock::now(),
							&normalizedPath,
							&uploadByteCount
						};
						texture = NLS::Render::Resources::Texture2D::WrapExternal(
							result.texture,
							result.width,
						result.height);
					}
				if (!atlasUpload && texture == nullptr)
				{
					m_thumbnailAtlas.Release(normalizedPath);
					NLS_SERVICE(NLS::UI::UIManager).ReleaseTextureViewHandle(result.textureView);
					MarkCachedThumbnailTextureUploadRetryableFailure(normalizedPath);
					iterator = m_pendingThumbnailTextureUploadsByPath.erase(iterator);
					continue;
				}

					if (!atlasUpload)
					{
						texture->firstFilter = NLS::Render::Settings::ETextureFilteringMode::LINEAR;
						texture->secondFilter = NLS::Render::Settings::ETextureFilteringMode::LINEAR;
						texture->bitsPerPixel = 4u;
					}
					{
						AssetBrowserArtifactTelemetryScope cachePublishTelemetry {
							NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTexturePumpPendingUploadCachePublish,
							std::chrono::steady_clock::now(),
							&normalizedPath,
							&uploadByteCount
						};
						ThumbnailTextureCacheEntry cacheEntry;
						cacheEntry.texture = texture.release();
						cacheEntry.textureView = atlasUpload ? nullptr : std::move(result.textureView);
						cacheEntry.textureId = textureId;
						cacheEntry.width = pendingUpload.width;
						cacheEntry.height = pendingUpload.height;
						cacheEntry.lastUsedFrame = m_thumbnailTextureFrameSerial;
						cacheEntry.atlas = atlasUpload;
                        if (atlasUpload)
                        {
                            cacheEntry.atlasPageKey = pendingUpload.atlasAllocation->pageKey;
                            cacheEntry.atlasPageGeneration = pendingUpload.atlasAllocation->pageGeneration;
                            cacheEntry.uv = pendingUpload.atlasAllocation->uv;
                        }
						m_thumbnailTexturesByPath.emplace(normalizedPath, std::move(cacheEntry));
							m_thumbnailTextureLru.push_back(normalizedPath);
							m_thumbnailTextureRetryAfterFrameByPath.erase(normalizedPath);
							iterator = m_pendingThumbnailTextureUploadsByPath.erase(iterator);
						}
					++completedUploadsPublishedThisFrame;
				}
				m_nextPendingThumbnailTextureUploadPollPath =
					iterator != m_pendingThumbnailTextureUploadsByPath.end()
						? iterator->first
						: std::string {};
			}
		}
		else
		{
			m_nextPendingThumbnailTextureUploadPollPath.clear();
		}
	{
		const std::string readyTelemetryPath = m_selectedProjectFolder + "|texture-pump-ready-decodes";
		const size_t readyTelemetryItemCount = m_thumbnailTextureDecodes.size();
		AssetBrowserArtifactTelemetryScope readyTelemetry {
			NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTexturePumpReadyDecodePoll,
			std::chrono::steady_clock::now(),
			&readyTelemetryPath,
			&readyTelemetryItemCount,
			kAssetBrowserThumbnailUiDrawTelemetryMinimum
		};
		for (auto iterator = m_thumbnailTextureDecodes.begin(); iterator != m_thumbnailTextureDecodes.end();)
		{
			if (!iterator->future.valid())
			{
				m_thumbnailTexturesDecoding.erase(iterator->normalizedPath);
				iterator = m_thumbnailTextureDecodes.erase(iterator);
				continue;
			}

			if (iterator->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			{
				++iterator;
				continue;
			}

				const auto elapsedMicroseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - pumpBegin).count());
				if (!NLS::Editor::Assets::ShouldContinueAssetBrowserThumbnailTextureUploads({
						queuedUploadsThisFrame,
						uploadBudget,
						0u,
						0u,
						elapsedMicroseconds,
						kMaxAssetBrowserThumbnailTextureUploadMicrosecondsPerFrame }))
				{
				++deferredReadyUploadsThisFrame;
				if (deferredReadyUploadTelemetryPath.empty())
					deferredReadyUploadTelemetryPath = iterator->normalizedPath;
				++iterator;
				continue;
			}

			ThumbnailTextureDecodeResult result;
			try
			{
				result = iterator->future.get();
			}
			catch (const std::bad_alloc&)
			{
				result.normalizedPath = iterator->normalizedPath;
				result.colorSpace = iterator->colorSpace;
			}
			catch (...)
			{
				result.normalizedPath = iterator->normalizedPath;
				result.colorSpace = iterator->colorSpace;
			}
			const auto normalizedPath = result.normalizedPath;
			const auto colorSpace = result.colorSpace;
			bool retryCommittedFreshImage = false;
			{
				const size_t loadByteCount = result.rgbaPixels.size();
					AssetBrowserArtifactTelemetryScope loadTelemetry {
						NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTexturePumpReadyDecodeLoad,
						std::chrono::steady_clock::now(),
						&normalizedPath,
						&loadByteCount,
						kAssetBrowserThumbnailUiDrawTelemetryMinimum
					};
					const bool loaded = LoadDecodedCachedThumbnailTexture(std::move(result));
					if (loaded)
						++queuedUploadsThisFrame;
					if (loaded)
					{
						m_thumbnailTexturesRetryAfterDecode.erase(normalizedPath);
					}
					else if (m_thumbnailTexturesRetryAfterDecode.erase(normalizedPath) > 0u)
					{
						retryCommittedFreshImage = true;
					}
				}
				m_thumbnailTexturesQueuedForLoad.erase(normalizedPath);
				m_thumbnailTexturesDecoding.erase(normalizedPath);
				if (retryCommittedFreshImage)
				{
					// A Fresh completion can race a decode that started before the
					// atomic PNG commit. Retry that path once against the committed file.
					m_thumbnailTexturesFailedToLoad.erase(normalizedPath);
					m_thumbnailTextureLoadQueue.erase(
						std::remove(
							m_thumbnailTextureLoadQueue.begin(),
							m_thumbnailTextureLoadQueue.end(),
							normalizedPath),
						m_thumbnailTextureLoadQueue.end());
					QueueCachedThumbnailTextureLoad(normalizedPath, colorSpace);
				}
			iterator = m_thumbnailTextureDecodes.erase(iterator);
		}
	}
	if (deferredReadyUploadsThisFrame > 0u)
	{
		NLS::Core::Assets::RecordArtifactLoadTelemetry({
			NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailTextureUploadDeferred,
			std::chrono::microseconds(0),
			deferredReadyUploadsThisFrame,
			deferredReadyUploadTelemetryPath
		});
	}
}

void Editor::Panels::AssetBrowser::StartCurrentFolderItemsRefresh(
	const std::filesystem::path& projectRoot,
	std::string selectedFolder,
	NLS::Editor::Assets::AssetBrowserBuildOptions buildOptions)
{
	NLS_PROFILE_NAMED_SCOPE("AssetBrowser::StartCurrentFolderItemsRefresh");
	CurrentFolderItemsRefreshRequest request;
	request.databaseSnapshot = m_projectAssetDatabaseSnapshot;
	request.snapshotIndex = m_projectAssetSubAssetSnapshotIndex;
	request.key.projectRoot = projectRoot.lexically_normal();
	request.key.selectedFolder = std::move(selectedFolder);
	request.key.databaseSnapshotIdentity = request.databaseSnapshot.get();
	request.key.snapshotIndexIdentity = request.snapshotIndex.get();
	request.key.sourceStateIdentity = std::make_shared<const uint8_t>(0u);
	request.key.buildOptions = std::move(buildOptions);
	m_projectDisplayItemsDirty = false;
	const auto disposition = NLS::Editor::Assets::QueueAssetBrowserLatestRequest(
		m_currentFolderItemsRefreshCoordinator,
		request.key);
	if (disposition == NLS::Editor::Assets::AssetBrowserLatestRequestDisposition::ActiveUnchanged)
	{
		m_pendingCurrentFolderItemsRefresh.reset();
		return;
	}
	m_pendingCurrentFolderItemsRefresh = std::move(request);
	if (disposition == NLS::Editor::Assets::AssetBrowserLatestRequestDisposition::StartNow)
		StartNextCurrentFolderItemsRefresh();
}

void Editor::Panels::AssetBrowser::StartNextCurrentFolderItemsRefresh()
{
	if (m_currentFolderItemsRefresh.has_value() || !m_pendingCurrentFolderItemsRefresh.has_value())
		return;
	const auto activatedKey = NLS::Editor::Assets::ActivateAssetBrowserLatestRequest(
		m_currentFolderItemsRefreshCoordinator);
	if (!activatedKey.has_value() || !(m_pendingCurrentFolderItemsRefresh->key == *activatedKey))
		return;
	auto request = std::move(*m_pendingCurrentFolderItemsRefresh);
	m_pendingCurrentFolderItemsRefresh.reset();
	NLS::Editor::Assets::BeginAssetBrowserAsyncRefresh(m_currentFolderItemsRefreshState);

	try
	{
		m_currentFolderItemsRefresh = CurrentFolderItemsRefresh {
				request.key,
				ScheduleAssetBrowserJobFuture(
					"AssetBrowser.CurrentFolderItemsRefresh",
					[request = std::move(request)]() mutable
			{
					CurrentFolderItemsRefresh::Result result;
					const auto context = std::string("Asset Browser folder refresh failed for folder \"") +
						request.key.selectedFolder + "\".";
					try
					{
						auto rootBuildOptions = request.key.buildOptions;
						rootBuildOptions.includeGeneratedSubAssets = false;
						rootBuildOptions.expandedSourceAssets.clear();
						rootBuildOptions.searchQuery.clear();
						rootBuildOptions.typeFilter = NLS::Editor::Assets::AssetBrowserItemType::All;
						auto rootItems = NLS::Editor::Assets::BuildCurrentFolderAssetItems(
							request.key.projectRoot,
							request.key.selectedFolder,
							request.databaseSnapshot.get(),
							rootBuildOptions);
						result.bundle = NLS::Editor::Assets::BuildAssetBrowserPresentationBundle(
							std::move(rootItems),
							request.snapshotIndex.get(),
							request.key.buildOptions);
					}
					catch (const std::exception& exception)
					{
						result.diagnostic = context + " " + exception.what();
					}
					catch (...)
					{
						result.diagnostic = context + " Unknown error.";
					}
					return result;
				})
		};
	}
	catch (const std::exception& exception)
	{
		const auto diagnostic = std::string("Asset Browser folder items refresh failed to start for folder \"") +
			activatedKey->selectedFolder + "\": " + exception.what();
		(void)NLS::Editor::Assets::CompleteAssetBrowserLatestRequest(
			m_currentFolderItemsRefreshCoordinator,
			*activatedKey);
		NLS::Editor::Assets::FailAssetBrowserAsyncRefresh(m_currentFolderItemsRefreshState, diagnostic);
		NLS_LOG_ERROR(diagnostic);
		StartNextCurrentFolderItemsRefresh();
	}
	catch (...)
	{
		const auto diagnostic = std::string("Asset Browser folder items refresh failed to start for folder \"") +
			activatedKey->selectedFolder + "\": unknown error.";
		(void)NLS::Editor::Assets::CompleteAssetBrowserLatestRequest(
			m_currentFolderItemsRefreshCoordinator,
			*activatedKey);
		NLS::Editor::Assets::FailAssetBrowserAsyncRefresh(m_currentFolderItemsRefreshState, diagnostic);
		NLS_LOG_ERROR(diagnostic);
		StartNextCurrentFolderItemsRefresh();
	}
}

void Editor::Panels::AssetBrowser::PumpCurrentFolderItemsRefresh()
{
	if (!m_currentFolderItemsRefresh.has_value())
		return;
	NLS_PROFILE_NAMED_SCOPE("AssetBrowser::PumpCurrentFolderItemsRefresh");

	auto& refresh = *m_currentFolderItemsRefresh;
	const auto completedKey = refresh.key;
	CurrentFolderItemsRefresh::Result result;
	if (!refresh.future.valid())
	{
		result.diagnostic = std::string("Asset Browser folder refresh future became invalid for folder \"") +
			completedKey.selectedFolder + "\".";
	}
	else if (refresh.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return;
	else try
	{
		result = refresh.future.get();
	}
	catch (const std::exception& exception)
	{
		result.diagnostic = std::string("Asset Browser folder refresh worker failed for folder \"") +
			completedKey.selectedFolder + "\": " + exception.what();
	}
	catch (...)
	{
		result.diagnostic = std::string("Asset Browser folder refresh worker failed for folder \"") +
			completedKey.selectedFolder + "\": unknown error.";
	}
	m_currentFolderItemsRefresh.reset();
	const auto completion = NLS::Editor::Assets::CompleteAssetBrowserLatestRequest(
		m_currentFolderItemsRefreshCoordinator,
		completedKey);
	if (completion.publish && !result.diagnostic.empty())
	{
		NLS::Editor::Assets::FailAssetBrowserAsyncRefresh(
			m_currentFolderItemsRefreshState,
			result.diagnostic);
		NLS_LOG_ERROR(result.diagnostic);
	}
	else if (completion.publish)
	{
		NLS::Editor::Assets::CompleteAssetBrowserAsyncRefresh(m_currentFolderItemsRefreshState);
		++m_projectAssetPresentationGeneration;
		m_unfilteredCurrentFolderItems = std::move(result.bundle.rootItems);
		m_currentFolderItems = std::move(result.bundle.visibleItems);
		m_projectDisplayItems = std::move(result.bundle.displayItems);
		m_projectDisplayItemsDirty = false;
		m_thumbnailGenerationScopeDirty = true;
		m_pendingThumbnailScopeItems.clear();
		m_pendingThumbnailScopeOffset = 0u;
		m_pendingThumbnailRequestContext = MakeAssetBrowserThumbnailRequestBuildContext(m_thumbnailFeatureConfig);
		m_pendingThumbnailRequestContext.residentPrefabPreviewRegistry = m_residentPrefabPreviewRegistry;
		m_thumbnailScopeBuildInProgress = false;
	}
	StartNextCurrentFolderItemsRefresh();
}

void Editor::Panels::AssetBrowser::StartProjectFolderTreeRefresh(
	const std::filesystem::path& projectRoot,
	NLS::Editor::Assets::AssetBrowserFolderTreeBuildOptions treeOptions)
{
	NLS_PROFILE_NAMED_SCOPE("AssetBrowser::StartProjectFolderTreeRefresh");
	DiscardProjectFolderTreeRefresh();
	const auto generation = ++m_projectFolderTreeRefreshGeneration;
	const auto normalizedRoot = projectRoot.lexically_normal();
	const auto selectedFolder = treeOptions.selectedFolder;

	try
	{
		m_projectFolderTreeRefresh = ProjectFolderTreeRefresh {
				generation,
				normalizedRoot,
				selectedFolder,
				ScheduleAssetBrowserJobFuture(
					"AssetBrowser.ProjectFolderTreeRefresh",
					[normalizedRoot, treeOptions = std::move(treeOptions)]() mutable
					{
						return NLS::Editor::Assets::BuildProjectAssetFolderTree(
							normalizedRoot,
						treeOptions);
				})
		};
	}
	catch (const std::exception& exception)
	{
		DiscardProjectFolderTreeRefresh();
		NLS_LOG_ERROR(std::string("Asset Browser folder tree refresh failed to start: ") + exception.what());
	}
	catch (...)
	{
		DiscardProjectFolderTreeRefresh();
		NLS_LOG_ERROR("Asset Browser folder tree refresh failed to start.");
	}
}

void Editor::Panels::AssetBrowser::PumpProjectFolderTreeRefresh()
{
	if (!m_projectFolderTreeRefresh.has_value())
		return;
	NLS_PROFILE_NAMED_SCOPE("AssetBrowser::PumpProjectFolderTreeRefresh");

	auto& refresh = *m_projectFolderTreeRefresh;
	if (!refresh.future.valid())
	{
		m_projectFolderTreeRefresh.reset();
		return;
	}
	if (refresh.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return;

	NLS::Editor::Assets::AssetBrowserFolderNode tree;
	try
	{
		tree = refresh.future.get();
	}
	catch (...)
	{
		tree = {};
	}

	const bool stillCurrent =
		refresh.generation == m_projectFolderTreeRefreshGeneration &&
		refresh.projectRoot.lexically_normal() == ProjectRootFromAssetsFolder(m_projectAssetFolder).lexically_normal() &&
		refresh.selectedFolder == m_selectedProjectFolder;
	m_projectFolderTreeRefresh.reset();
	if (!stillCurrent || tree.projectRelativePath.empty())
		return;

	m_projectFolderTree = std::move(tree);
}

void Editor::Panels::AssetBrowser::RefreshProjectAssetSubAssetSnapshotCache()
{
	NLS_PROFILE_NAMED_SCOPE("AssetBrowser::RefreshProjectAssetSubAssetSnapshotCache");
	if (!m_projectAssetDatabaseReady || !m_projectAssetDatabase)
	{
		m_projectAssetSubAssetSnapshotIndex.reset();
		MarkProjectAssetDisplayItemsDirty();
		return;
	}

	m_projectAssetSubAssetSnapshotIndex.reset();

	const auto publishedState = m_projectAssetDatabase->GetPublishedState();
	if (!publishedState || !publishedState->snapshotIndex ||
		publishedState->snapshotIndex->status != NLS::Editor::Assets::EditorAssetSnapshotStatus::Valid)
	{
		MarkProjectAssetDisplayItemsDirty();
		return;
	}

	m_projectAssetSubAssetSnapshotIndex = publishedState->snapshotIndex;
	MarkProjectAssetDisplayItemsDirty();
}

void Editor::Panels::AssetBrowser::DiscardProjectFolderTreeRefresh()
{
	if (m_projectFolderTreeRefresh.has_value() &&
		m_projectFolderTreeRefresh->future.valid() &&
		m_projectFolderTreeRefresh->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
	{
		m_retiredProjectFolderTreeRefreshes.push_back(std::move(*m_projectFolderTreeRefresh));
	}
	m_projectFolderTreeRefresh.reset();
	m_retiredProjectFolderTreeRefreshes.erase(
		std::remove_if(
			m_retiredProjectFolderTreeRefreshes.begin(),
			m_retiredProjectFolderTreeRefreshes.end(),
			[](ProjectFolderTreeRefresh& refresh)
			{
				return !refresh.future.valid() ||
					refresh.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
			}),
		m_retiredProjectFolderTreeRefreshes.end());
}

void Editor::Panels::AssetBrowser::RequestObjectReferencePickerEntriesRefresh()
{
	NLS::Editor::Assets::MarkObjectReferencePickerEntriesDirty();
	const auto publishedState = m_projectAssetDatabase ? m_projectAssetDatabase->GetPublishedState() : nullptr;
	auto snapshotIndex = publishedState ? publishedState->snapshotIndex : nullptr;
	if (!snapshotIndex ||
		snapshotIndex->status != NLS::Editor::Assets::EditorAssetSnapshotStatus::Valid)
	{
		m_objectReferencePickerRefreshRequested = false;
		NLS::Editor::Assets::FailAssetBrowserAsyncRefresh(
			m_objectReferencePickerRefreshState,
			"Asset Browser object-reference picker refresh has no valid published snapshot index.");
		return;
	}
	ObjectReferencePickerRefreshRequest request;
	request.snapshotIndex = std::move(snapshotIndex);
	request.key.snapshotIndexIdentity = request.snapshotIndex.get();
	request.key.lifetimeIdentity = m_objectReferencePickerLifetimeIdentity.get();
	const auto disposition = NLS::Editor::Assets::QueueAssetBrowserLatestRequest(
		m_objectReferencePickerRefreshCoordinator,
		request.key);
	if (disposition == NLS::Editor::Assets::AssetBrowserLatestRequestDisposition::ActiveUnchanged)
	{
		m_pendingObjectReferencePickerRefresh.reset();
		m_objectReferencePickerRefreshRequested = false;
		return;
	}
	m_pendingObjectReferencePickerRefresh = std::move(request);
	if (disposition == NLS::Editor::Assets::AssetBrowserLatestRequestDisposition::StartNow)
		NLS::Editor::Assets::ResetAssetBrowserAsyncRefresh(m_objectReferencePickerRefreshState);
	m_objectReferencePickerRefreshRequested = true;
}

std::shared_ptr<Editor::Panels::AssetBrowser::AssetDatabaseRetirementState>
Editor::Panels::AssetBrowser::SharedProjectAssetDatabaseRetirementState()
{
	static const auto retirementState = std::make_shared<AssetDatabaseRetirementState>();
	return retirementState;
}

void Editor::Panels::AssetBrowser::AbandonProjectAssetDatabaseRefreshFuture(
	std::future<AssetDatabaseRefreshResult>& future)
{
	if (!future.valid())
		return;
	if (future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		AssetDatabaseRefreshResult result;
		try
		{
			result = future.get();
		}
		catch (...)
		{
		}
		RetireProjectAssetDatabaseResult(std::move(result));
		return;
	}

	{
		std::lock_guard lock(m_projectAssetDatabaseRetirementState->mutex);
		m_projectAssetDatabaseRetirementState->pendingFutures.push_back(std::move(future));
	}
	ScheduleProjectAssetDatabaseRetirementWorker();
}

void Editor::Panels::AssetBrowser::RetireProjectAssetDatabaseResult(
	AssetDatabaseRefreshResult result)
{
	if (!result.database && !result.snapshot)
		return;

	{
		std::lock_guard lock(m_projectAssetDatabaseRetirementState->mutex);
		m_projectAssetDatabaseRetirementState->pending.push_back(std::move(result));
	}
	ScheduleProjectAssetDatabaseRetirementWorker();
}

void Editor::Panels::AssetBrowser::ScheduleProjectAssetDatabaseRetirementWorker()
{
	(void)NLS::Editor::Assets::ScheduleAssetDatabaseRetirementWorker(
		m_projectAssetDatabaseRetirementState->mutex,
		m_projectAssetDatabaseRetirementState->workerRunning,
		[](std::function<void()> worker, std::function<void()> cancel)
		{
			(void)ScheduleAssetBrowserJobFuture(
				"AssetBrowser.RetireProjectAssetDatabase",
				std::move(worker),
				std::move(cancel));
			return true;
		},
		[retirementState = m_projectAssetDatabaseRetirementState]
		{
			std::vector<AssetDatabaseRefreshResult> retired;
			std::vector<std::future<AssetDatabaseRefreshResult>> retiredFutures;
			{
				std::lock_guard lock(retirementState->mutex);
				retirementState->pending.swap(retired);
				retirementState->pendingFutures.swap(retiredFutures);
			}
			for (auto& future : retiredFutures)
			{
				AssetDatabaseRefreshResult result;
				try
				{
					result = future.get();
				}
				catch (...)
				{
				}
				retired.push_back(std::move(result));
			}
			retired.clear();
		},
		[retirementState = m_projectAssetDatabaseRetirementState]
		{
			return !retirementState->pending.empty() ||
				!retirementState->pendingFutures.empty();
		});
}

void Editor::Panels::AssetBrowser::RetireCurrentProjectAssetDatabase()
{
	AssetDatabaseRefreshResult result;
	result.database = std::move(m_projectAssetDatabase);
	result.snapshot = std::move(m_projectAssetDatabaseSnapshot);
	RetireProjectAssetDatabaseResult(std::move(result));
}

void Editor::Panels::AssetBrowser::PumpRetiredProjectAssetDatabaseRefreshes()
{
	for (auto iterator = m_retiredProjectAssetDatabaseRefreshes.begin();
		iterator != m_retiredProjectAssetDatabaseRefreshes.end();)
	{
		if (!iterator->future.valid())
		{
			iterator = m_retiredProjectAssetDatabaseRefreshes.erase(iterator);
			continue;
		}
		if (iterator->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			++iterator;
			continue;
		}
		AssetDatabaseRefreshResult result;
		try
		{
			result = iterator->future.get();
		}
		catch (...)
		{
		}
		RetireProjectAssetDatabaseResult(std::move(result));
		iterator = m_retiredProjectAssetDatabaseRefreshes.erase(iterator);
	}
}

void Editor::Panels::AssetBrowser::DiscardProjectAssetDatabaseRefresh()
{
	if (m_projectAssetDatabaseRefresh.has_value() &&
		m_projectAssetDatabaseRefresh->future.valid())
	{
		const bool ready =
			m_projectAssetDatabaseRefresh->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
		if (ready)
		{
			AssetDatabaseRefreshResult result;
			try
			{
				result = m_projectAssetDatabaseRefresh->future.get();
			}
			catch (...)
			{
			}
			RetireProjectAssetDatabaseResult(std::move(result));
			m_projectAssetDatabaseRefresh.reset();
			return;
		}
		if (NLS::Editor::Assets::PlanAssetDatabaseRefreshDiscardAction(true, ready) ==
			NLS::Editor::Assets::AssetDatabaseRefreshDiscardAction::Retire)
		{
			m_retiredProjectAssetDatabaseRefreshes.push_back(std::move(*m_projectAssetDatabaseRefresh));
			m_projectAssetDatabaseRefresh.reset();
			return;
		}
	}
	m_projectAssetDatabaseRefresh.reset();
}

void Editor::Panels::AssetBrowser::InvalidateObjectReferencePickerEntriesRefresh()
{
	m_objectReferencePickerRefreshCoordinator.desiredKey.reset();
	m_objectReferencePickerRefreshCoordinator.pendingKey.reset();
	m_pendingObjectReferencePickerRefresh.reset();
	m_objectReferencePickerRefreshRequested = false;
}

void Editor::Panels::AssetBrowser::PumpObjectReferencePickerEntriesRefresh()
{
	const bool interactive = IsAssetBrowserInteractive();
	if (m_objectReferencePickerRefresh.has_value())
	{
		auto& refresh = *m_objectReferencePickerRefresh;
		const auto completedKey = refresh.key;
		ObjectReferencePickerRefresh::Result result;
		bool completed = false;
		if (!refresh.future.valid())
		{
			result.diagnostic = "Asset Browser object-reference picker refresh future became invalid.";
			completed = true;
		}
		else if (!interactive &&
			refresh.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			try
			{
				result = refresh.future.get();
			}
			catch (const std::exception& exception)
			{
				result.diagnostic = std::string("Asset Browser object-reference picker refresh worker failed: ") +
					exception.what();
			}
			catch (...)
			{
				result.diagnostic = "Asset Browser object-reference picker refresh worker failed: unknown error.";
			}
			completed = true;
		}
		if (completed)
		{
			m_objectReferencePickerRefresh.reset();
			const auto completion = NLS::Editor::Assets::CompleteAssetBrowserLatestRequest(
				m_objectReferencePickerRefreshCoordinator,
				completedKey);
			if (completion.publish && result.diagnostic.empty())
			{
				NLS::Editor::Assets::SetObjectReferencePickerEntries(std::move(result.entries));
				m_objectReferencePickerRefreshRequested = false;
				NLS::Editor::Assets::CompleteAssetBrowserAsyncRefresh(m_objectReferencePickerRefreshState);
			}
			else if (completion.publish)
			{
				m_objectReferencePickerRefreshRequested = false;
				NLS::Editor::Assets::FailAssetBrowserAsyncRefresh(
					m_objectReferencePickerRefreshState,
					result.diagnostic.empty()
						? "Asset Browser object-reference picker refresh failed."
						: result.diagnostic);
				NLS_LOG_ERROR(m_objectReferencePickerRefreshState.diagnostic);
			}
			StartNextObjectReferencePickerEntriesRefresh();
		}
	}

	if (m_objectReferencePickerRefresh.has_value() ||
			!m_objectReferencePickerRefreshRequested ||
			interactive ||
			m_projectAssetDatabaseRefresh.has_value() ||
			m_projectAssetDatabaseRefreshQueuedAfterInFlight ||
			!m_projectAssetDatabaseReady ||
			!m_projectAssetDatabase ||
			!NLS::Editor::Assets::ShouldDeferObjectReferencePickerEntriesRefresh())
	{
		return;
	}
	StartNextObjectReferencePickerEntriesRefresh();
}

void Editor::Panels::AssetBrowser::StartNextObjectReferencePickerEntriesRefresh()
{
	if (m_objectReferencePickerRefresh.has_value() || !m_pendingObjectReferencePickerRefresh.has_value())
		return;
	const auto activatedKey = NLS::Editor::Assets::ActivateAssetBrowserLatestRequest(
		m_objectReferencePickerRefreshCoordinator);
	if (!activatedKey.has_value() || !(m_pendingObjectReferencePickerRefresh->key == *activatedKey))
		return;
	auto request = std::move(*m_pendingObjectReferencePickerRefresh);
	m_pendingObjectReferencePickerRefresh.reset();
	m_objectReferencePickerRefreshRequested = false;
	NLS::Editor::Assets::BeginAssetBrowserAsyncRefresh(m_objectReferencePickerRefreshState);
	try
	{
		m_objectReferencePickerRefresh = ObjectReferencePickerRefresh {
			request.key,
			ScheduleAssetBrowserJobFuture(
				"AssetBrowser.ObjectReferencePickerRefresh",
				[snapshotIndex = std::move(request.snapshotIndex)]() mutable
				{
					ObjectReferencePickerRefresh::Result result;
					try
					{
						result.entries = NLS::Editor::Assets::BuildObjectReferencePickerEntriesFromSnapshots(
							snapshotIndex->assets);
					}
					catch (const std::exception& exception)
					{
						result.diagnostic = std::string("Asset Browser object-reference picker refresh failed: ") +
							exception.what();
					}
					catch (...)
					{
						result.diagnostic = "Asset Browser object-reference picker refresh failed: unknown error.";
					}
					return result;
				})
		};
	}
	catch (const std::exception& exception)
	{
		const auto diagnostic = std::string("Asset Browser object-reference picker refresh failed to start: ") +
			exception.what();
		(void)NLS::Editor::Assets::CompleteAssetBrowserLatestRequest(
			m_objectReferencePickerRefreshCoordinator,
			*activatedKey);
		NLS::Editor::Assets::FailAssetBrowserAsyncRefresh(m_objectReferencePickerRefreshState, diagnostic);
		NLS_LOG_ERROR(diagnostic);
	}
	catch (...)
	{
		const std::string diagnostic = "Asset Browser object-reference picker refresh failed to start: unknown error.";
		(void)NLS::Editor::Assets::CompleteAssetBrowserLatestRequest(
			m_objectReferencePickerRefreshCoordinator,
			*activatedKey);
		NLS::Editor::Assets::FailAssetBrowserAsyncRefresh(m_objectReferencePickerRefreshState, diagnostic);
		NLS_LOG_ERROR(diagnostic);
	}
}

void Editor::Panels::AssetBrowser::DestroyCachedThumbnailTextures(const bool force)
{
	for (auto iterator = m_thumbnailTextureDecodes.begin(); iterator != m_thumbnailTextureDecodes.end();)
	{
		if (!iterator->future.valid())
		{
			m_thumbnailTexturesDecoding.erase(iterator->normalizedPath);
			iterator = m_thumbnailTextureDecodes.erase(iterator);
			continue;
		}
		if (!force && iterator->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			++iterator;
			continue;
		}
		if (force && iterator->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			AbandonAssetBrowserFuture(iterator->future);
			m_thumbnailTexturesDecoding.erase(iterator->normalizedPath);
			iterator = m_thumbnailTextureDecodes.erase(iterator);
			continue;
		}
		try
		{
			(void)iterator->future.get();
		}
		catch (...)
		{
		}
		m_thumbnailTexturesDecoding.erase(iterator->normalizedPath);
		iterator = m_thumbnailTextureDecodes.erase(iterator);
	}

	std::vector<std::string> textureKeys;
	textureKeys.reserve(m_thumbnailTexturesByPath.size());
	for (const auto& [key, _] : m_thumbnailTexturesByPath)
		textureKeys.push_back(key);
	if (!force)
	{
		const auto clearPlan = NLS::Editor::Assets::PlanAssetBrowserThumbnailTextureFullClear(
			textureKeys,
			m_thumbnailTexturesUsedThisFrame,
			m_thumbnailTexturesPendingRelease);
		m_thumbnailTexturesUsedThisFrame = clearPlan.usedThisFrame;
		m_thumbnailTexturesPendingRelease = clearPlan.pendingRelease;
		textureKeys = clearPlan.releaseNow;
	}
	for (const auto& key : textureKeys)
	{
		if (!force)
		{
			const auto found = m_thumbnailTexturesByPath.find(key);
			if (found == m_thumbnailTexturesByPath.end())
				continue;
			if (found->second.atlas)
			{
				m_thumbnailAtlas.Release(key);
				m_thumbnailTexturesByPath.erase(found);
				continue;
			}
			if (found->second.textureView != nullptr &&
				NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
			{
				NLS_SERVICE(NLS::UI::UIManager).RetireTextureViewHandle(found->second.textureView);
			}
			NLS::Render::Resources::Loaders::TextureLoader::Destroy(found->second.texture);
			m_thumbnailTexturesByPath.erase(found);
			continue;
		}

		const auto found = m_thumbnailTexturesByPath.find(key);
		if (found == m_thumbnailTexturesByPath.end())
			continue;
		if (found->second.atlas)
		{
			m_thumbnailAtlas.Release(key);
			m_thumbnailTexturesByPath.erase(found);
			continue;
		}
		if (found->second.textureView != nullptr &&
			NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
		{
			NLS_SERVICE(NLS::UI::UIManager).ReleaseTextureViewHandle(found->second.textureView);
		}
		NLS::Render::Resources::Loaders::TextureLoader::Destroy(found->second.texture);
		m_thumbnailTexturesByPath.erase(found);
	}
	m_thumbnailTextureLru.clear();
	if (force)
	{
		m_thumbnailTexturesUsedThisFrame.clear();
		m_thumbnailTexturesPendingRelease.clear();
	}
		m_thumbnailTextureLoadQueue.clear();
		m_thumbnailTexturesQueuedForLoad.clear();
		m_thumbnailTextureColorSpacesByPath.clear();
		if (!m_pendingThumbnailTextureUploadsByPath.empty())
		{
			if (auto* driver = NLS::Render::Context::TryGetLocatedDriver())
			{
				for (const auto& [path, pendingUpload] : m_pendingThumbnailTextureUploadsByPath)
				{
					if (pendingUpload.atlasAllocation.has_value())
						m_thumbnailAtlas.Release(path);
					NLS::Render::Context::DriverUIAccess::CancelUiRgba8TextureUpload(
						*driver,
						pendingUpload.requestId);
				}
			}
		}
		m_pendingThumbnailTextureUploadsByPath.clear();
		if (force)
		{
			for (auto& [_, page] : m_thumbnailAtlasPagesByKey)
			{
				if (page.textureView != nullptr &&
					NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
				{
					NLS_SERVICE(NLS::UI::UIManager).ReleaseTextureViewHandle(page.textureView);
				}
			}
			m_thumbnailAtlasPagesByKey.clear();
			m_thumbnailAtlas.Reset();
		}
		m_thumbnailTexturesFailedToLoad.clear();
		m_thumbnailTexturesRetryAfterDecode.clear();
		if (force)
			m_thumbnailTexturesDecoding.clear();
	}

void Editor::Panels::AssetBrowser::ReleaseAssetBrowserTextureHandleCache(const bool force)
{
	if (NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
	{
		std::unordered_set<NLS::Render::RHI::RHITextureView*> releasedTextureViews;
		for (const auto& [_, entry] : m_assetBrowserTextureHandleCache)
		{
			if (entry.textureView == nullptr)
				continue;
			if (!releasedTextureViews.insert(entry.textureView.get()).second)
				continue;
			if (force)
				NLS_SERVICE(NLS::UI::UIManager).ReleaseTextureViewHandle(entry.textureView);
			else
				NLS_SERVICE(NLS::UI::UIManager).RetireTextureViewHandle(entry.textureView);
		}
	}
	m_assetBrowserTextureHandleCache.clear();
}

void* Editor::Panels::AssetBrowser::ResolveAssetBrowserTextureHandle(
	NLS::Render::Resources::Texture2D* texture,
	const std::string& debugName)
{
	if (texture == nullptr || !NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
		return nullptr;

	const auto resolvedTextureView = texture->GetOrCreateExplicitTextureView(debugName);
	if (resolvedTextureView == nullptr)
		return nullptr;

	if (const auto found = m_assetBrowserTextureHandleCache.find(texture);
		found != m_assetBrowserTextureHandleCache.end())
	{
		if (found->second.textureView == resolvedTextureView && found->second.textureId != nullptr)
			return found->second.textureId;

		if (found->second.textureView != nullptr)
			NLS_SERVICE(NLS::UI::UIManager).RetireTextureViewHandle(found->second.textureView);
		m_assetBrowserTextureHandleCache.erase(found);
	}

	void* textureId = NLS_SERVICE(NLS::UI::UIManager).ResolveTextureId(resolvedTextureView);
	if (textureId == nullptr)
		return nullptr;

	m_assetBrowserTextureHandleCache.emplace(texture, AssetBrowserTextureHandleCacheEntry {
		resolvedTextureView,
		textureId
	});
	return textureId;
}

void Editor::Panels::AssetBrowser::ReleaseCachedThumbnailTexture(
	const std::string& normalizedPath)
{
	if (const auto pending = m_pendingThumbnailTextureUploadsByPath.find(normalizedPath);
		pending != m_pendingThumbnailTextureUploadsByPath.end())
	{
		if (auto* driver = NLS::Render::Context::TryGetLocatedDriver())
		{
			NLS::Render::Context::DriverUIAccess::CancelUiRgba8TextureUpload(
				*driver,
				pending->second.requestId);
		}
		if (pending->second.atlasAllocation.has_value())
			m_thumbnailAtlas.Release(normalizedPath);
		m_pendingThumbnailTextureUploadsByPath.erase(pending);
	}
	const auto found = m_thumbnailTexturesByPath.find(normalizedPath);
	if (found == m_thumbnailTexturesByPath.end())
		return;
	if (m_thumbnailTexturesUsedThisFrame.find(normalizedPath) != m_thumbnailTexturesUsedThisFrame.end())
	{
		m_thumbnailTexturesPendingRelease.insert(normalizedPath);
		return;
	}
	if (found->second.atlas)
	{
		m_thumbnailAtlas.Release(normalizedPath);
		m_thumbnailTexturesByPath.erase(found);
		m_thumbnailTextureLru.erase(
			std::remove(m_thumbnailTextureLru.begin(), m_thumbnailTextureLru.end(), normalizedPath),
			m_thumbnailTextureLru.end());
		return;
	}

	if (found->second.textureView != nullptr &&
		NLS::Core::ServiceLocator::Contains<NLS::UI::UIManager>())
	{
		NLS_SERVICE(NLS::UI::UIManager).RetireTextureViewHandle(found->second.textureView);
	}
	NLS::Render::Resources::Loaders::TextureLoader::Destroy(found->second.texture);
	m_thumbnailTexturesByPath.erase(found);
	m_thumbnailTextureLru.erase(
		std::remove(m_thumbnailTextureLru.begin(), m_thumbnailTextureLru.end(), normalizedPath),
		m_thumbnailTextureLru.end());
}

void Editor::Panels::AssetBrowser::PruneCachedThumbnailTextures()
{
	if (IsAssetBrowserInteractive())
		return;
	size_t standaloneCount = 0u;
	for (const auto& [_, entry] : m_thumbnailTexturesByPath)
	{
		if (!entry.atlas)
			++standaloneCount;
	}
	if (standaloneCount <= kMaxResidentAssetBrowserThumbnailTextures)
		return;

	std::vector<std::pair<std::string, uint64_t>> candidates;
	candidates.reserve(m_thumbnailTexturesByPath.size());
	for (const auto& [key, entry] : m_thumbnailTexturesByPath)
	{
		if (entry.atlas)
			continue;
		if (m_thumbnailTexturesUsedThisFrame.find(key) == m_thumbnailTexturesUsedThisFrame.end())
			candidates.emplace_back(key, entry.lastUsedFrame);
	}
	std::sort(
		candidates.begin(),
		candidates.end(),
		[](const auto& left, const auto& right)
		{
			return left.second < right.second;
		});

	std::vector<std::string> evictions;
	auto residentAfterEviction = m_thumbnailTexturesByPath.size();
	for (const auto& [key, _] : candidates)
	{
		if (residentAfterEviction <= kMaxResidentAssetBrowserThumbnailTextures)
			break;
		evictions.push_back(key);
		--residentAfterEviction;
	}
	for (const auto& key : evictions)
		ReleaseCachedThumbnailTexture(key);
}

void Editor::Panels::AssetBrowser::UpdateThumbnailGenerationScope()
{
	const bool interactive = IsAssetBrowserInteractive();
	if (!m_thumbnailGenerationScopeDirty && !m_thumbnailScopeBuildInProgress)
		return;
	NLS_PROFILE_NAMED_SCOPE("AssetBrowser::UpdateThumbnailGenerationScope");
	const std::string scopeTelemetryPath = m_selectedProjectFolder + "|generation-scope";
	const size_t scopeTelemetryItemCount = m_visibleThumbnailItemsKnown
		? m_visibleThumbnailItems.size()
		: m_currentFolderItems.size();
		std::chrono::microseconds selectItemsElapsed {};
		std::chrono::microseconds buildKeyElapsed {};
		std::chrono::microseconds itemKeyElapsed {};
		std::chrono::microseconds resultLookupElapsed {};
		std::chrono::microseconds buildRequestElapsed {};
		std::chrono::microseconds requestPreviewElapsed {};

	const auto nextSize = AssetBrowserThumbnailRequestSize(m_thumbnailSize);
	if (m_thumbnailGenerationScopeDirty && m_thumbnailScopeBuildInProgress)
	{
		m_pendingThumbnailScopeItems.clear();
		m_pendingThumbnailScopeOffset = 0u;
		m_pendingThumbnailRequestContext = MakeAssetBrowserThumbnailRequestBuildContext(m_thumbnailFeatureConfig);
		m_pendingThumbnailRequestContext.residentPrefabPreviewRegistry = m_residentPrefabPreviewRegistry;
		m_thumbnailScopeBuildInProgress = false;
	}
	if (!m_thumbnailScopeBuildInProgress)
	{
		std::vector<NLS::Editor::Assets::AssetBrowserItem> scopedThumbnailItems;
		{
			AssetBrowserArtifactTelemetryAccumulatorScope selectItemsTelemetry {
				selectItemsElapsed
			};
			scopedThumbnailItems = NLS::Editor::Assets::SelectAssetBrowserThumbnailGenerationItems(
				m_currentFolderItems,
				m_visibleThumbnailItems,
				m_visibleThumbnailItemsKnown);
		}
		const auto nextFolder = NormalizeProjectBrowserPath(m_selectedProjectFolder);
		std::string nextScopeKey;
		{
			AssetBrowserArtifactTelemetryAccumulatorScope buildKeyTelemetry {
				buildKeyElapsed
			};
			// The generation scope describes the stable contents of the current
			// folder. The visible set is intentionally excluded here: scrolling
			// must only reprioritize requests and allow the service's off-screen
			// grace period to expire, rather than cancelling the whole generation.
			nextScopeKey = NLS::Editor::Assets::BuildAssetBrowserThumbnailGenerationScopeKey(
				nextFolder,
				nextSize,
				m_currentFolderItems);
		}
		const auto decision = NLS::Editor::Assets::EvaluateAssetBrowserThumbnailGenerationScope(
			m_lastThumbnailGenerationScopeKey,
			m_lastThumbnailRequestSize,
			m_thumbnailGenerationScopeDirty,
			nextScopeKey,
			nextSize);
		if (decision.canSkip)
		{
			m_thumbnailGenerationScopeDirty = false;
			return;
			}

			m_lastThumbnailRequestSize = nextSize;
			m_lastThumbnailGenerationScopeKey = nextScopeKey;
			m_lastThumbnailGenerationScopeInteractive = interactive;
		m_thumbnailGenerationScopeDirty = false;
		m_pendingThumbnailScopeItems = std::move(scopedThumbnailItems);
		m_pendingThumbnailScopeOffset = 0u;
		m_pendingThumbnailRequestContext = MakeAssetBrowserThumbnailRequestBuildContext(m_thumbnailFeatureConfig);
		m_pendingThumbnailRequestContext.assetDatabaseSnapshot = m_projectAssetDatabaseSnapshot;
		m_pendingThumbnailRequestContext.residentPrefabPreviewRegistry = m_residentPrefabPreviewRegistry;
		m_thumbnailScopeBuildInProgress = true;
		if (decision.scopeChanged)
		{
			// Cache keys may change when deferred artifact resolution completes,
			// while presentationKey remains stable. Keep only bindings for the
			// current folder generation so a late result cannot update an old tile.
			m_thumbnailItemKeyByPresentationKey.clear();
			m_thumbnailService.SupersedeQueuedRequestsForGeneration(nextScopeKey);
		}
	}

	const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
	std::unordered_set<std::string> visibleThumbnailKeys;
	visibleThumbnailKeys.reserve(m_visibleThumbnailItems.size());
	for (const auto& visibleItem : m_visibleThumbnailItems)
	{
		visibleThumbnailKeys.insert(
			NLS::Editor::Assets::BuildAssetBrowserThumbnailItemKey(
				visibleItem,
				nextSize));
	}
		const size_t requestBudget = IsAssetBrowserInteractive()
			? kMaxAssetBrowserInteractiveThumbnailRequestsPerFrame
			: kMaxAssetBrowserThumbnailRequestsPerFrame;
		const auto requestBudgetStart = std::chrono::steady_clock::now();
		size_t processedThisFrame = 0u;
		while (m_pendingThumbnailScopeOffset < m_pendingThumbnailScopeItems.size() &&
			NLS::Editor::Assets::ShouldContinueAssetBrowserThumbnailRequests({
			processedThisFrame,
			requestBudget,
			static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - requestBudgetStart).count()),
			kMaxAssetBrowserThumbnailRequestMicrosecondsPerFrame }))
	{
			const auto& item = m_pendingThumbnailScopeItems[m_pendingThumbnailScopeOffset++];
			++processedThisFrame;
			std::string itemThumbnailKey;
			{
				AssetBrowserArtifactTelemetryAccumulatorScope itemKeyTelemetry {
					itemKeyElapsed
				};
				itemThumbnailKey =
					NLS::Editor::Assets::BuildAssetBrowserThumbnailItemKey(item, nextSize);
			}
			decltype(m_thumbnailResultsByItemKey)::iterator foundThumbnail;
			{
				AssetBrowserArtifactTelemetryAccumulatorScope resultLookupTelemetry {
					resultLookupElapsed
				};
				foundThumbnail = m_thumbnailResultsByItemKey.find(itemThumbnailKey);
			}
			const auto ThumbnailResultMatchesRequestCacheKey = [](
				const NLS::Editor::Assets::AssetThumbnailServiceResult& result,
				const std::string& currentRequestCacheKey)
			{
				return result.cacheEntry.has_value() &&
					result.cacheEntry->cacheKey == currentRequestCacheKey;
			};
			if (foundThumbnail != m_thumbnailResultsByItemKey.end() &&
				foundThumbnail->second.status == NLS::Editor::Assets::AssetThumbnailServiceStatus::Fresh &&
				NLS::Editor::Assets::AssetBrowserThumbnailResultHasDisplayImage(foundThumbnail->second))
			{
				const auto key = foundThumbnail->second.imagePath.lexically_normal().generic_string();
				m_thumbnailTexturesFailedToLoad.erase(key);
				QueueCachedThumbnailTextureLoad(
					foundThumbnail->second.imagePath,
					CachedThumbnailTextureColorSpace(foundThumbnail->second));
			}
			std::optional<NLS::Editor::Assets::AssetThumbnailRequest> request;
			{
				AssetBrowserArtifactTelemetryAccumulatorScope buildRequestTelemetry {
				buildRequestElapsed
			};
			request = NLS::Editor::Assets::BuildAssetThumbnailRequestForItem(
					projectRoot,
					item,
					nextSize,
					m_pendingThumbnailRequestContext);
			}
				if (!request)
				{
					RecordAssetBrowserThumbnailRequestBuildFailureTelemetry(itemThumbnailKey);
					continue;
				}
				const auto currentRequestCacheKey = NLS::Editor::Assets::BuildAssetThumbnailCacheKey(*request);
				const auto currentPresentationKey = NLS::Editor::Assets::BuildAssetThumbnailPresentationKey(*request);
				if (!currentPresentationKey.empty())
				{
					auto& itemKeys = m_thumbnailItemKeyByPresentationKey[currentPresentationKey];
					if (std::find(itemKeys.begin(), itemKeys.end(), itemThumbnailKey) == itemKeys.end())
						itemKeys.push_back(itemThumbnailKey);
				}
				const auto PromoteFreshThumbnailCache = [&](const NLS::Editor::Assets::AssetThumbnailCacheEvaluation& evaluation)
				{
					if (evaluation.status == NLS::Editor::Assets::AssetThumbnailCacheStatus::Fresh &&
						evaluation.entry.has_value())
					{
						NLS::Editor::Assets::AssetThumbnailServiceResult cached;
						cached.status = NLS::Editor::Assets::AssetThumbnailServiceStatus::Fresh;
						cached.presentationState = NLS::Editor::Assets::ThumbnailPresentationState::Ready;
						cached.previewQuality = NLS::Editor::Assets::ThumbnailPreviewQuality::Canonical;
						cached.presentationKey = NLS::Editor::Assets::BuildAssetThumbnailPresentationKey(*request);
						cached.requestRevision = foundThumbnail != m_thumbnailResultsByItemKey.end()
							? foundThumbnail->second.requestRevision
							: request->requestRevision;
						cached.refreshPending = false;
						cached.failureRetained = false;
						cached.requestKind = static_cast<uint8_t>(request->kind);
						cached.cacheEntry = evaluation.entry;
						cached.imagePath = evaluation.entry->imagePath;
						if (foundThumbnail != m_thumbnailResultsByItemKey.end())
						{
							cached = NLS::Editor::Assets::PreserveAssetBrowserThumbnailRetainedImage(
								foundThumbnail->second,
								cached);
							foundThumbnail->second = cached;
						}
						else
						{
							m_thumbnailResultsByItemKey.emplace(itemThumbnailKey, cached);
						}
						if (!cached.imagePath.empty())
						{
							const auto key = cached.imagePath.lexically_normal().generic_string();
							m_thumbnailTexturesFailedToLoad.erase(key);
							QueueCachedThumbnailTextureLoad(
								cached.imagePath,
								CachedThumbnailTextureColorSpace(request->kind));
						}
						return true;
					}
					return false;
				};
			if (foundThumbnail != m_thumbnailResultsByItemKey.end())
			{
				if (foundThumbnail->second.status == NLS::Editor::Assets::AssetThumbnailServiceStatus::Fresh &&
					NLS::Editor::Assets::AssetBrowserThumbnailResultHasDisplayImage(foundThumbnail->second) &&
					ThumbnailResultMatchesRequestCacheKey(foundThumbnail->second, currentRequestCacheKey))
				{
					continue;
				}
				if (NLS::Editor::Assets::AssetBrowserThumbnailResultNeedsCanonicalRecovery(
						foundThumbnail->second) &&
					// A completed writer can finish after deferred artifact
					// resolution or a freshness revision changed the cache key.
					// Re-evaluate the current request instead of requiring the old
					// Pending result to carry the exact same key.
					!currentRequestCacheKey.empty())
				{
					const auto evaluation = NLS::Editor::Assets::EvaluateAssetThumbnailCache(
							*request,
							NLS::Editor::Assets::AssetThumbnailCacheIntegrityMode::Fast);
						if (PromoteFreshThumbnailCache(evaluation))
							continue;
					}
				}
				{
					auto prioritizedRequest = *request;
			prioritizedRequest.priority = visibleThumbnailKeys.find(itemThumbnailKey) != visibleThumbnailKeys.end()
				? NLS::Editor::Assets::ThumbnailRequestPriority::Visible
				: NLS::Editor::Assets::ThumbnailRequestPriority::Prefetch;
					if (ShouldBypassAssetBrowserThumbnailService(prioritizedRequest.kind))
					{
						const auto evaluation = NLS::Editor::Assets::EvaluateAssetThumbnailCache(
							prioritizedRequest,
							NLS::Editor::Assets::AssetThumbnailCacheIntegrityMode::Fast);
						(void)PromoteFreshThumbnailCache(evaluation);
						continue;
					}
			NLS::Editor::Assets::AssetThumbnailServiceResult thumbnail;
			{
				AssetBrowserArtifactTelemetryAccumulatorScope requestPreviewTelemetry {
					requestPreviewElapsed
				};
				thumbnail = m_thumbnailService.RequestAssetPreview(prioritizedRequest);
				(void)NLS::Editor::Assets::PromoteAssetThumbnailResultFromPresentationIndex(
					prioritizedRequest,
					thumbnail);
			}
				RecordAssetBrowserThumbnailResultTelemetry(
					thumbnail,
					NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeRequestPreview);
				if (NLS::Editor::Assets::ShouldRetryAssetBrowserThumbnailRequestAfterBackpressure(
					thumbnail.diagnostic))
				{
					// Queue capacity is transient. Keep this item at the head of the
					// remaining scope so a later frame retries it after the service pump.
					--m_pendingThumbnailScopeOffset;
					break;
				}
				foundThumbnail = m_thumbnailResultsByItemKey.find(itemThumbnailKey);
				bool replaceThumbnailResult = foundThumbnail == m_thumbnailResultsByItemKey.end();
			if (!replaceThumbnailResult)
			{
				const auto& existing = foundThumbnail->second;
				const bool existingHasDisplay =
					NLS::Editor::Assets::AssetBrowserThumbnailResultHasDisplayImage(existing);
				const bool incomingHasDisplay =
					NLS::Editor::Assets::AssetBrowserThumbnailResultHasDisplayImage(thumbnail);
				const bool existingCanonical =
					existing.presentationState == NLS::Editor::Assets::ThumbnailPresentationState::Ready &&
						existing.previewQuality == NLS::Editor::Assets::ThumbnailPreviewQuality::Canonical;
				const bool incomingCanonical =
					thumbnail.presentationState == NLS::Editor::Assets::ThumbnailPresentationState::Ready &&
					thumbnail.previewQuality == NLS::Editor::Assets::ThumbnailPreviewQuality::Canonical;
				const bool completesSamePresentation =
					NLS::Editor::Assets::AssetBrowserThumbnailResultCompletesSamePresentation(
						existing,
						thumbnail);
				replaceThumbnailResult =
					!NLS::Editor::Assets::AssetBrowserThumbnailResultRegressesSamePresentation(
						existing,
						thumbnail) &&
					(!NLS::Editor::Assets::AssetBrowserThumbnailResultIsOlder(existing, thumbnail) ||
						completesSamePresentation) &&
					(!existingHasDisplay || incomingHasDisplay || completesSamePresentation) &&
					(!existingCanonical || incomingCanonical);
			}
			if (replaceThumbnailResult)
			{
				if (foundThumbnail != m_thumbnailResultsByItemKey.end())
				{
					foundThumbnail->second = NLS::Editor::Assets::PreserveAssetBrowserThumbnailRetainedImage(
						foundThumbnail->second,
						thumbnail);
				}
				else
				{
					m_thumbnailResultsByItemKey.emplace(itemThumbnailKey, thumbnail);
				}
			}
			if (thumbnail.status == NLS::Editor::Assets::AssetThumbnailServiceStatus::Fresh &&
				!thumbnail.imagePath.empty())
			{
				const auto key = thumbnail.imagePath.lexically_normal().generic_string();
				m_thumbnailTexturesFailedToLoad.erase(key);
				QueueCachedThumbnailTextureLoad(
					thumbnail.imagePath,
					CachedThumbnailTextureColorSpace(thumbnail));
			}
			if (thumbnail.cacheEntry.has_value())
			{
				NLS::Editor::Assets::RegisterAssetBrowserThumbnailCacheKeyBinding(
					m_thumbnailItemKeyByCacheKey,
					thumbnail.cacheEntry->cacheKey,
					itemThumbnailKey);
			}
		}
	}
	if (m_pendingThumbnailScopeOffset >= m_pendingThumbnailScopeItems.size())
	{
		m_pendingThumbnailScopeItems.clear();
		m_pendingThumbnailScopeOffset = 0u;
		m_pendingThumbnailRequestContext = MakeAssetBrowserThumbnailRequestBuildContext(m_thumbnailFeatureConfig);
		m_pendingThumbnailRequestContext.residentPrefabPreviewRegistry = m_residentPrefabPreviewRegistry;
		m_thumbnailScopeBuildInProgress = false;
	}
	RecordAssetBrowserArtifactTelemetryStage(
		NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeSelectItems,
		selectItemsElapsed,
		scopeTelemetryPath,
		scopeTelemetryItemCount,
		kAssetBrowserThumbnailUiDrawTelemetryMinimum);
		RecordAssetBrowserArtifactTelemetryStage(
			NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildKey,
			buildKeyElapsed,
			scopeTelemetryPath,
			scopeTelemetryItemCount,
			kAssetBrowserThumbnailUiDrawTelemetryMinimum);
		RecordAssetBrowserArtifactTelemetryStage(
			NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeItemKey,
			itemKeyElapsed,
			scopeTelemetryPath,
			scopeTelemetryItemCount,
			kAssetBrowserThumbnailUiDrawTelemetryMinimum);
		RecordAssetBrowserArtifactTelemetryStage(
			NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeResultLookup,
			resultLookupElapsed,
			scopeTelemetryPath,
			scopeTelemetryItemCount,
			kAssetBrowserThumbnailUiDrawTelemetryMinimum);
		RecordAssetBrowserArtifactTelemetryStage(
			NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeBuildRequest,
		buildRequestElapsed,
		scopeTelemetryPath,
		scopeTelemetryItemCount,
		kAssetBrowserThumbnailUiDrawTelemetryMinimum);
	RecordAssetBrowserArtifactTelemetryStage(
		NLS::Core::Assets::ArtifactLoadTelemetryStage::ThumbnailUiDrawGenerationScopeRequestPreview,
		requestPreviewElapsed,
		scopeTelemetryPath,
		scopeTelemetryItemCount,
		kAssetBrowserThumbnailUiDrawTelemetryMinimum);
}

NLS::Editor::Assets::AssetBrowserRect MakeAssetBrowserRect(
    const ImVec2& min,
    const ImVec2& max)
{
    return {
        { min.x, min.y },
        { max.x, max.y }
    };
}

ImVec2 ToImVec2(const NLS::Editor::Assets::AssetBrowserPoint& point)
{
    return { point.x, point.y };
}

void DrawAssetBrowserDisclosureButton(
	ImDrawList* drawList,
	const ImVec2& center,
	const float radius,
	const bool expanded,
	const bool hovered,
	const bool horizontalToggle)
{
	const ImU32 background = hovered ? IM_COL32(216, 220, 224, 235) : IM_COL32(188, 193, 198, 220);
	const ImU32 outline = IM_COL32(74, 78, 84, 210);
	const ImU32 arrow = IM_COL32(56, 60, 66, 255);
	drawList->AddCircleFilled(center, radius, background, 20);
	drawList->AddCircle(center, radius, outline, 20, 1.0f);

	const float arrowSize = radius * 0.48f;
	if (horizontalToggle && expanded)
	{
		drawList->AddTriangleFilled(
			ImVec2(center.x + arrowSize * 0.35f, center.y - arrowSize),
			ImVec2(center.x + arrowSize * 0.35f, center.y + arrowSize),
			ImVec2(center.x - arrowSize * 0.75f, center.y),
			arrow);
	}
	else if (horizontalToggle)
	{
		drawList->AddTriangleFilled(
			ImVec2(center.x - arrowSize * 0.35f, center.y - arrowSize),
			ImVec2(center.x - arrowSize * 0.35f, center.y + arrowSize),
			ImVec2(center.x + arrowSize * 0.75f, center.y),
			arrow);
	}
	else if (expanded)
	{
		drawList->AddTriangleFilled(
			ImVec2(center.x - arrowSize, center.y - arrowSize * 0.45f),
			ImVec2(center.x + arrowSize, center.y - arrowSize * 0.45f),
			ImVec2(center.x, center.y + arrowSize * 0.65f),
			arrow);
	}
	else
	{
		drawList->AddTriangleFilled(
			ImVec2(center.x - arrowSize * 0.35f, center.y - arrowSize),
			ImVec2(center.x - arrowSize * 0.35f, center.y + arrowSize),
			ImVec2(center.x + arrowSize * 0.75f, center.y),
			arrow);
	}
}

void DrawAssetBrowserFilmstripPanel(
	ImDrawList* drawList,
	const ImVec2& min,
	const ImVec2& max,
	const bool hovered,
	const bool continuesLeft,
	const bool continuesRight)
{
	ImDrawFlags cornerFlags = ImDrawFlags_RoundCornersAll;
	if (continuesLeft && continuesRight)
		cornerFlags = ImDrawFlags_RoundCornersNone;
	else if (continuesLeft)
		cornerFlags = ImDrawFlags_RoundCornersRight;
	else if (continuesRight)
		cornerFlags = ImDrawFlags_RoundCornersLeft;
	DrawAssetBrowserSegmentPanel(drawList, min, max, hovered, cornerFlags);
}

void DrawAssetBrowserSegmentPanel(
	ImDrawList* drawList,
	const ImVec2& min,
	const ImVec2& max,
	const bool hovered,
	const ImDrawFlags cornerFlags)
{
	const ImU32 fillColor = hovered ? IM_COL32(88, 88, 88, 255) : IM_COL32(74, 74, 74, 255);
	drawList->AddRectFilled(
		min,
		max,
		fillColor,
		10.0f,
		cornerFlags);
}

void Editor::Panels::AssetBrowser::DrawProjectGridItemThumbnail(
	const NLS::Editor::Assets::AssetBrowserItem& item,
	const ImVec2& iconMin,
	const ImVec2& iconMax,
	const float thumbnailSize,
	const bool hovered,
	const bool compact)
{
	NLS_PROFILE_NAMED_SCOPE("AssetBrowser::DrawProjectGridItemThumbnail");
	auto* drawList = ImGui::GetWindowDrawList();
	auto recordDrawPath = [&, itemPath = item.projectRelativePath](
		const char* drawKind,
		const AssetBrowserThumbnailDrawOutcome outcome,
		const bool canonical = false,
		const std::string_view canonicalIdentity = {})
	{
		if (!NLS::Core::Assets::IsArtifactLoadTelemetryEnabled())
			return;

		const std::string telemetryPath = itemPath.empty()
			? item.absolutePath.lexically_normal().generic_string()
			: itemPath;
		if (outcome == AssetBrowserThumbnailDrawOutcome::Thumbnail && canonical)
			RecordAssetBrowserThumbnailCanonicalDrawTelemetry(
				canonicalIdentity.empty() ? telemetryPath : canonicalIdentity);

		const int frameCount = ImGui::GetFrameCount();
		if (frameCount > 0 && frameCount % kAssetBrowserThumbnailDrawOutcomeSamplePeriodFrames != 0)
			return;

		(void)drawKind;
		RecordAssetBrowserThumbnailDrawOutcomeTelemetry(telemetryPath, outcome);
	};
	auto drawFallbackBlock = [&]()
	{
		if (!NLS::Editor::Assets::CanGenerateAssetBrowserThumbnail(item))
		{
			NLS::Editor::Panels::RecordAssetBrowserThumbnailTypeFallbackTelemetry(
				NLS::Editor::Assets::BuildAssetBrowserThumbnailItemKey(
					item,
					AssetBrowserThumbnailRequestSize(m_thumbnailSize)));
		}
		const char* iconId = item.type == NLS::Editor::Assets::AssetBrowserItemType::Script
			? NLS::Editor::Assets::AssetBrowserScriptIconId(item.absolutePath.string())
			: NLS::Editor::Assets::AssetBrowserFallbackIconId(item.type);
				if (void* textureHandle = ResolveAssetBrowserTextureHandle(
						EDITOR_CONTEXT(editorResources)->GetTexture(iconId),
					"AssetBrowser.TypeIcon"))
				{
					drawList->PushClipRect(iconMin, iconMax, true);
					drawList->AddImage(textureHandle, iconMin, iconMax, kAssetBrowserImageUv0, kAssetBrowserImageUv1);
					drawList->PopClipRect();
					recordDrawPath("|draw=type-fallback", AssetBrowserThumbnailDrawOutcome::TypeFallback);
					return;
				}

		const auto color = AssetBrowserItemColor(item.type);
		drawList->AddRectFilled(iconMin, iconMax, color, compact ? 2.0f : 6.0f);
		recordDrawPath("|draw=type-fallback", AssetBrowserThumbnailDrawOutcome::TypeFallback);
	};
	auto drawResidentPendingBlock = [&]()
	{
		// Keep the tile visually stable while scene-owned resources become
		// drawable. This deliberately emits no fallback telemetry.
		drawList->AddRectFilled(
			iconMin,
			iconMax,
			IM_COL32(42, 46, 50, compact ? 92 : 120),
			compact ? 2.0f : 6.0f);
	};

	if (item.kind == NLS::Editor::Assets::AssetBrowserItemKind::Folder)
	{
			if (void* textureHandle = ResolveAssetBrowserTextureHandle(
	                    EDITOR_CONTEXT(editorResources)->GetTexture("editor.icon.asset.folder"),
					"AssetBrowser.Folder"))
			{
				drawList->PushClipRect(iconMin, iconMax, true);
				drawList->AddImage(textureHandle, iconMin, iconMax, kAssetBrowserImageUv0, kAssetBrowserImageUv1);
				drawList->PopClipRect();
				return;
			}
		drawFallbackBlock();
		return;
	}

	const auto itemThumbnailKey =
		NLS::Editor::Assets::BuildAssetBrowserThumbnailItemKey(
			item,
			AssetBrowserThumbnailRequestSize(m_thumbnailSize));
	if (NLS::Editor::Assets::CanGenerateAssetBrowserThumbnail(item))
	{
		const auto thumbnailIterator = m_thumbnailResultsByItemKey.find(itemThumbnailKey);
		RecordAssetBrowserThumbnailPresentationStateTelemetry(
			itemThumbnailKey,
			thumbnailIterator == m_thumbnailResultsByItemKey.end()
				? NLS::Editor::Assets::ThumbnailPresentationState::Loading
				: thumbnailIterator->second.presentationState,
			thumbnailIterator == m_thumbnailResultsByItemKey.end()
				? nullptr
				: &thumbnailIterator->second);
	}
	if (const auto thumbnailIterator = m_thumbnailResultsByItemKey.find(itemThumbnailKey);
		thumbnailIterator != m_thumbnailResultsByItemKey.end())
	{
		const auto& thumbnail = thumbnailIterator->second;
		const auto drawPooledThumbnail = [&](
			const std::string& cacheKey,
			const uint64_t textureGeneration,
			const bool canonical,
			const char* drawKind)
		{
			if (m_assetThumbnailPool == nullptr || cacheKey.empty())
				return false;
			auto [pooledThumbnail, inserted] = m_assetThumbnailsByCacheKey.try_emplace(cacheKey);
			if (inserted)
			{
				pooledThumbnail->second = m_assetThumbnailPool->MakeThumbnail(
					cacheKey,
					textureGeneration);
			}
			const auto pooledTexture = pooledThumbnail->second.Resolve(
				m_thumbnailTextureFrameSerial);
			if (!pooledTexture.IsReady())
				return false;

			const auto thumbnailRect = NLS::Editor::Assets::ComputeAssetBrowserThumbnailRect(
				MakeAssetBrowserRect(iconMin, iconMax),
				pooledTexture.width,
				pooledTexture.height);
			drawList->PushClipRect(iconMin, iconMax, true);
			const bool flipGpuThumbnailVertically =
				NLS::Core::ServiceLocator::Contains<UI::UIManager>() &&
				NLS_SERVICE(UI::UIManager).ShouldFlipPresentedRenderTargetVertically();
			const ImVec2 gpuThumbnailUv0 = flipGpuThumbnailVertically
				? kAssetBrowserThumbnailUv0
				: ImVec2(0.0f, 0.0f);
			const ImVec2 gpuThumbnailUv1 = flipGpuThumbnailVertically
				? kAssetBrowserThumbnailUv1
				: ImVec2(1.0f, 1.0f);
			drawList->AddImage(
				pooledTexture.textureId,
				ToImVec2(thumbnailRect.min),
				ToImVec2(thumbnailRect.max),
				gpuThumbnailUv0,
				gpuThumbnailUv1);
			drawList->PopClipRect();
			recordDrawPath(
				drawKind,
				AssetBrowserThumbnailDrawOutcome::Thumbnail,
				canonical,
				itemThumbnailKey);
			return true;
		};

		if (thumbnail.cacheEntry.has_value() &&
			drawPooledThumbnail(
				thumbnail.cacheEntry->cacheKey,
				thumbnail.gpuTextureGeneration,
				thumbnail.presentationState == NLS::Editor::Assets::ThumbnailPresentationState::Ready &&
					thumbnail.previewQuality == NLS::Editor::Assets::ThumbnailPreviewQuality::Canonical,
				"|draw=gpu-pool"))
		{
			return;
		}
		// A newly published PNG can spend several frames in decode/upload. Try the
		// previous visible image before allowing the tile to fall back to its icon.
		const auto cachedImageCandidates = NLS::Editor::Assets::BuildAssetBrowserThumbnailImageCandidates(
			thumbnail.status == NLS::Editor::Assets::AssetThumbnailServiceStatus::Fresh
				? thumbnail.imagePath
				: std::filesystem::path {},
			thumbnail.retainedImage.has_value()
				? thumbnail.retainedImage->imagePath
				: std::filesystem::path {},
			thumbnail.status == NLS::Editor::Assets::AssetThumbnailServiceStatus::Pending &&
				thumbnail.cacheEntry.has_value()
				? thumbnail.cacheEntry->imagePath
				: std::filesystem::path {});
		for (const auto& cachedImagePath : cachedImageCandidates)
		{
			const auto textureInfo = ResolveCachedThumbnailTextureHandle(
				cachedImagePath,
				true,
				CachedThumbnailTextureColorSpace(thumbnail));
			if (textureInfo.textureHandle == nullptr)
				continue;

			const bool isCanonicalCachedThumbnail =
				thumbnail.status == NLS::Editor::Assets::AssetThumbnailServiceStatus::Fresh &&
				cachedImagePath == thumbnail.imagePath;
			const auto thumbnailRect = NLS::Editor::Assets::ComputeAssetBrowserThumbnailRect(
				MakeAssetBrowserRect(iconMin, iconMax),
				textureInfo.width,
				textureInfo.height);
			drawList->PushClipRect(iconMin, iconMax, true);
			drawList->AddImage(
				textureInfo.textureHandle,
				ToImVec2(thumbnailRect.min),
				ToImVec2(thumbnailRect.max),
				ImVec2(textureInfo.uv.u0, textureInfo.uv.v0),
				ImVec2(textureInfo.uv.u1, textureInfo.uv.v1));
			drawList->PopClipRect();
			recordDrawPath(
				isCanonicalCachedThumbnail
					? "|draw=thumbnail"
					: "|draw=thumbnail-stale",
				AssetBrowserThumbnailDrawOutcome::Thumbnail,
				isCanonicalCachedThumbnail,
				itemThumbnailKey);
			return;
		}
		if (thumbnail.retainedGpuPresentation.has_value() &&
			thumbnail.retainedGpuPresentation->IsValid() &&
			drawPooledThumbnail(
				thumbnail.retainedGpuPresentation->cacheKey,
				thumbnail.retainedGpuPresentation->textureGeneration,
				false,
				"|draw=gpu-pool-retained"))
		{
			return;
		}
		if (ShouldHoldResidentPrefabThumbnailFallback(item, &thumbnail))
		{
			drawResidentPendingBlock();
			return;
		}

		const auto fallbackIconId =
			NLS::Editor::Assets::ResolveAssetBrowserDisplayFallbackIconId(
				item.type,
				thumbnail.fallbackIcon,
				item.absolutePath.string());
		if (!fallbackIconId.empty())
		{
			const std::string fallbackIconKey(fallbackIconId);
			if (void* textureHandle = ResolveAssetBrowserTextureHandle(
					EDITOR_CONTEXT(editorResources)->GetTexture(fallbackIconKey),
					"AssetBrowser.Fallback"))
				{
					drawList->PushClipRect(iconMin, iconMax, true);
					drawList->AddImage(
						textureHandle,
						iconMin,
					iconMax,
						kAssetBrowserImageUv0,
						kAssetBrowserImageUv1);
					drawList->PopClipRect();
					recordDrawPath("|draw=fallback", AssetBrowserThumbnailDrawOutcome::Fallback);
					return;
				}
			}
	}
	else if (ShouldHoldResidentPrefabThumbnailFallback(item, nullptr))
	{
		drawResidentPendingBlock();
		return;
	}

	drawFallbackBlock();
}

void Editor::Panels::AssetBrowser::DrawProjectGridItemDragSource(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	NLS_PROFILE_NAMED_SCOPE("AssetBrowser::DrawProjectGridItemDragSource");
	if (item.kind == NLS::Editor::Assets::AssetBrowserItemKind::Folder)
	{
		if (item.dragResourcePath.empty() ||
			!UI::BeginDragDropSource(
				UI::DragDropSourceFlags::NoDisableHover |
				UI::DragDropSourceFlags::NoHoldToOpenOthers))
		{
			return;
		}

		UI::DrawDragDropTooltipText(item.projectRelativePath.c_str());
		m_projectGridDragPairPayload = { item.projectRelativePath, nullptr };
		UI::SetDragDropPayload(
			"Folder",
			&m_projectGridDragPairPayload,
			sizeof(m_projectGridDragPairPayload));
		UI::EndDragDropSource();
		return;
	}

	const bool sourceEditorPayload =
		item.kind == NLS::Editor::Assets::AssetBrowserItemKind::SourceAsset &&
		(item.type == NLS::Editor::Assets::AssetBrowserItemType::Model ||
		 item.type == NLS::Editor::Assets::AssetBrowserItemType::Prefab ||
		 item.type == NLS::Editor::Assets::AssetBrowserItemType::Material ||
		 item.type == NLS::Editor::Assets::AssetBrowserItemType::Texture ||
		 item.type == NLS::Editor::Assets::AssetBrowserItemType::Shader ||
         (item.type == NLS::Editor::Assets::AssetBrowserItemType::Script &&
          NLS::Editor::Assets::IsScriptAssetPath(item.dragResourcePath)));
	const bool generatedEditorPayload =
		item.kind == NLS::Editor::Assets::AssetBrowserItemKind::GeneratedSubAsset &&
		!item.dragResourcePath.empty() &&
		item.assetId.IsValid() &&
		NLS::Editor::Assets::CanStoreEditorAssetDragPayload(
			item.dragResourcePath,
			item.assetId,
			item.subAssetKey);
	const bool filePayload =
		item.kind != NLS::Editor::Assets::AssetBrowserItemKind::GeneratedSubAsset &&
		!item.dragResourcePath.empty();
	if (!sourceEditorPayload &&
		!generatedEditorPayload &&
		!filePayload)
	{
		return;
	}
	if (!UI::BeginDragDropSource(
			UI::DragDropSourceFlags::NoDisableHover |
			UI::DragDropSourceFlags::NoHoldToOpenOthers))
	{
		return;
	}

	auto editorAssetPayload = (generatedEditorPayload || sourceEditorPayload)
		? NLS::Editor::Assets::MakeAssetBrowserItemDragPayload(
				item,
				m_projectAssetDatabaseReady && m_projectAssetDatabaseSnapshot
					? m_projectAssetDatabaseSnapshot.get()
					: nullptr)
		: std::optional<NLS::Editor::Assets::EditorAssetDragPayload> {};
	UI::DrawDragDropTooltipText(item.displayName.c_str());
	if (editorAssetPayload.has_value())
	{
		if (item.type == NLS::Editor::Assets::AssetBrowserItemType::Model ||
			item.type == NLS::Editor::Assets::AssetBrowserItemType::Prefab)
		{
			SchedulePrefabHotCachePreloadForDragPayload(*editorAssetPayload);
		}
		UI::SetDragDropPayload(
			NLS::Editor::Assets::kEditorAssetDragPayloadType,
			&*editorAssetPayload,
			sizeof(*editorAssetPayload));
	}
	else if (filePayload)
	{
		m_projectGridDragPairPayload = { item.dragResourcePath, nullptr };
		UI::SetDragDropPayload(
			"File",
			&m_projectGridDragPairPayload,
			sizeof(m_projectGridDragPairPayload));
	}
	UI::EndDragDropSource();
}

void Editor::Panels::AssetBrowser::DrawProjectFolderDropTarget(
	const std::string& projectRelativeFolder,
	const std::filesystem::path& absoluteFolder)
{
	NLS_PROFILE_NAMED_SCOPE("AssetBrowser::DrawProjectFolderDropTarget");
	if (!UI::BeginDragDropTarget())
		return;

	if (const auto payload = UI::AcceptDragDropPayload(
			"Folder",
			UI::DragDropTargetFlags::None);
		payload.delivered &&
		payload.data != nullptr &&
		payload.dataSize == sizeof(std::pair<std::string, UI::Widgets::Group*>))
	{
		const auto* folderPayload = static_cast<const std::pair<std::string, UI::Widgets::Group*>*>(payload.data);
		(void)MoveOrCopyProjectBrowserFolderIntoFolder(
			folderPayload->first,
			absoluteFolder);
	}

	if (const auto payload = UI::AcceptDragDropPayload(
			"File",
			UI::DragDropTargetFlags::None);
		payload.delivered &&
		payload.data != nullptr &&
		payload.dataSize == sizeof(std::pair<std::string, UI::Widgets::Group*>))
	{
		const auto* filePayload = static_cast<const std::pair<std::string, UI::Widgets::Group*>*>(payload.data);
		(void)MoveOrCopyProjectBrowserFileIntoFolder(
			filePayload->first,
			absoluteFolder);
	}

	if (const auto payload = UI::AcceptDragDropPayload(
			NLS::Editor::Assets::kEditorAssetDragPayloadType,
			UI::DragDropTargetFlags::None);
		payload.delivered &&
		payload.data != nullptr &&
		payload.dataSize == sizeof(NLS::Editor::Assets::EditorAssetDragPayload))
	{
		const auto* assetPayload = static_cast<const NLS::Editor::Assets::EditorAssetDragPayload*>(payload.data);
		if (NLS::Editor::Assets::CanMoveEditorAssetDragPayloadAsPhysicalProjectFile(*assetPayload))
		{
			(void)MoveOrCopyProjectBrowserFileIntoFolder(
				NLS::Editor::Assets::GetEditorAssetDragPayloadPath(*assetPayload),
				absoluteFolder);
		}
	}

	if (const auto payload = UI::AcceptDragDropPayload(
			"GameObject",
			UI::DragDropTargetFlags::None);
		payload.delivered &&
		payload.data != nullptr &&
		payload.dataSize == sizeof(std::pair<Engine::GameObject*, UI::Widgets::TreeNode*>))
	{
		const auto* objectPayload = static_cast<const std::pair<Engine::GameObject*, UI::Widgets::TreeNode*>*>(payload.data);
		(void)SaveHierarchyObjectAsPrefabIntoFolder(
			objectPayload->first,
			projectRelativeFolder,
			absoluteFolder);
	}

	UI::EndDragDropTarget();
}

void Editor::Panels::AssetBrowser::OpenProjectGridItemProperties(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	if (!NLS::Editor::Assets::BuildAssetBrowserWorkflowCapabilities(item).canOpenProperties)
		return;

	auto& assetProperties = EDITOR_PANEL(Editor::Panels::AssetProperties, "Asset Properties");
	assetProperties.SetTarget(ProjectBrowserSelectionPathForItem(item));
	assetProperties.Open();
	assetProperties.Focus();
}

void Editor::Panels::AssetBrowser::PreviewProjectGridItem(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	OpenProjectGridItemProperties(item);
	auto& assetProperties = EDITOR_PANEL(Editor::Panels::AssetProperties, "Asset Properties");
	auto& assetView = EDITOR_PANEL(Editor::Panels::AssetView, "Asset View");
	assetProperties.Preview();
	assetView.Open();
	assetView.Focus();
}

void Editor::Panels::AssetBrowser::RebuildProjectAssetPresentationAfterWorkflow()
{
	m_refreshRequested = false;
	RefreshPreservingExpandedFolders();
}

void Editor::Panels::AssetBrowser::ScheduleProjectAssetPreimportForPath(
	std::string projectRelativePath)
{
	projectRelativePath = NormalizeProjectBrowserPath(std::move(projectRelativePath));
	if (projectRelativePath.empty())
		return;

	ScheduleProjectAssetPreimport({
		NLS::Editor::Assets::AssetPreimportReason::AssetCopiedOrMoved,
		{ std::move(projectRelativePath) }
	});
}

void Editor::Panels::AssetBrowser::ScheduleProjectAssetPreimportForPath(
	const std::filesystem::path& projectRelativePath)
{
	if (projectRelativePath.empty())
		return;

	ScheduleProjectAssetPreimportForPath(NormalizeProjectBrowserPath(projectRelativePath));
}

bool Editor::Panels::AssetBrowser::MoveOrCopyProjectBrowserFolderIntoFolder(
	const std::string& receivedProjectRelativeFolder,
	const std::filesystem::path& targetAbsoluteFolder)
{
	if (receivedProjectRelativeFolder.empty() || targetAbsoluteFolder.empty())
		return false;

	const auto target = targetAbsoluteFolder.lexically_normal();
	if (!NLS::Editor::Assets::CanMoveProjectBrowserResourcePathIntoFolder(
			m_projectAssetFolder,
			receivedProjectRelativeFolder,
			target,
			true))
	{
		return false;
	}

	const auto source = ProjectBrowserAbsolutePathForResourcePath(m_projectAssetFolder, receivedProjectRelativeFolder);
	if (source.empty() || !std::filesystem::is_directory(source))
		return false;

	const auto destination = (target / source.filename()).lexically_normal();
	if (source == destination)
		return false;

	if (IsPathInsideOrEqual(target, source))
	{
		using namespace NLS::Dialogs;
		MessageBox errorMessage(
			"Invalid folder move",
			"You can't move a folder into itself.",
			MessageBox::EMessageType::ERROR,
			MessageBox::EButtonLayout::OK);
		return false;
	}

	if (std::filesystem::exists(destination))
	{
		using namespace NLS::Dialogs;
		MessageBox errorMessage(
			"Folder already exists",
			"You can't move this folder to this location because the name is already taken.",
			MessageBox::EMessageType::ERROR,
			MessageBox::EButtonLayout::OK);
		return false;
	}

	RenameAsset(source.string(), EnsureTrailingPathSeparator(destination));
	EDITOR_EXEC(PropagateFolderRename(source.string(), EnsureTrailingPathSeparator(destination)));
	ScheduleProjectAssetPreimportForPath(EditorAssetFolderFromAbsolutePath(m_projectAssetFolder, destination.string()));
	RebuildProjectAssetPresentationAfterWorkflow();
	return true;
}

bool Editor::Panels::AssetBrowser::MoveOrCopyProjectBrowserFileIntoFolder(
	const std::string& receivedResourcePath,
	const std::filesystem::path& targetAbsoluteFolder)
{
	if (receivedResourcePath.empty() || targetAbsoluteFolder.empty())
		return false;

	const auto target = targetAbsoluteFolder.lexically_normal();
	if (!NLS::Editor::Assets::CanMoveProjectBrowserResourcePathIntoFolder(
			m_projectAssetFolder,
			receivedResourcePath,
			target,
			false))
	{
		return false;
	}

	const auto source = ProjectBrowserAbsolutePathForResourcePath(m_projectAssetFolder, receivedResourcePath);
	if (source.empty() || !std::filesystem::is_regular_file(source))
		return false;

	const auto destination = (target / source.filename()).lexically_normal();
	if (source == destination)
		return false;
	if (std::filesystem::exists(destination))
	{
		using namespace NLS::Dialogs;
		MessageBox errorMessage(
			"File already exists",
			"You can't move this file to this location because the name is already taken.",
			MessageBox::EMessageType::ERROR,
			MessageBox::EButtonLayout::OK);
		return false;
	}

	RenameAsset(source.string(), destination.string());
	EDITOR_EXEC(PropagateFileRename(source.string(), destination.string()));

	ScheduleProjectAssetPreimportForPath(EditorAssetPathFromAbsolutePath(m_projectAssetFolder, destination.string()));
	RebuildProjectAssetPresentationAfterWorkflow();
	return true;
}

bool Editor::Panels::AssetBrowser::SaveHierarchyObjectAsPrefabIntoFolder(
	Engine::GameObject* gameObject,
	const std::string& targetProjectRelativeFolder,
	const std::filesystem::path& targetAbsoluteFolder)
{
	if (gameObject == nullptr || targetProjectRelativeFolder.empty() || targetAbsoluteFolder.empty())
		return false;

	const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
	if (projectRoot.empty())
	{
		NLS_LOG_ERROR("Failed to resolve prefab destination project root for hierarchy drop.");
		return false;
	}

	if (!m_projectAssetDatabaseReady ||
		m_projectAssetDatabase == nullptr ||
		m_projectAssetDatabaseRoot.lexically_normal() != projectRoot.lexically_normal())
	{
		NLS_LOG_ERROR("Asset database is still refreshing; prefab drop will be available when the asset browser finishes indexing.");
		return false;
	}

	NLS::Core::Assets::AssetId sceneAssetId;
	const auto currentSceneSourcePath = EDITOR_CONTEXT(sceneManager).GetCurrentSceneSourcePath();
	if (!currentSceneSourcePath.empty())
	{
		const auto sceneMeta = NLS::Core::Assets::AssetMeta::Load(
			NLS::Core::Assets::GetAssetMetaPath(std::filesystem::path(currentSceneSourcePath).lexically_normal()));
		if (sceneMeta.has_value())
			sceneAssetId = sceneMeta->id;
	}

	const auto result = NLS::Editor::Assets::AssetDragDropWorkflow().Execute({
		{NLS::Editor::Assets::DragPayloadKind::HierarchyObject, {}, {}, nullptr, gameObject},
		{NLS::Editor::Assets::DropTargetKind::AssetBrowserFolder, nullptr, nullptr, 0u, false, targetProjectRelativeFolder},
		sceneAssetId,
		NLS::Editor::Assets::DragDropOperationKind::SaveAsPrefab,
		m_projectAssetDatabase.get(),
		&EDITOR_CONTEXT(prefabInstanceRegistry)
	});

	if (result.status != NLS::Editor::Assets::DragDropOperationStatus::Committed)
	{
		for (const auto& diagnostic : result.diagnostics)
			NLS_LOG_ERROR(diagnostic.code + ": " + diagnostic.message);
		return false;
	}

	EDITOR_CONTEXT(sceneManager).MarkCurrentSceneDirty();
	if (result.instance.has_value() && result.instance->instanceRoot != nullptr)
	{
		EDITOR_PANEL(NLS::Editor::Panels::Hierarchy, "Hierarchy")
			.RefreshPrefabPresentation(*result.instance->instanceRoot);
	}
	for (const auto& createdPath : result.createdAssetPaths)
		ScheduleProjectAssetPreimportForPath(createdPath);
	RebuildProjectAssetPresentationAfterWorkflow();
	return true;
}

void Editor::Panels::AssetBrowser::SelectProjectGridItem(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	m_selectedProjectItem = NLS::Editor::Assets::BuildAssetBrowserActionIdentity(item);
}

void Editor::Panels::AssetBrowser::PrepareCSharpScriptDebugProject(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);

	std::filesystem::path vscodePath;
	const auto configuredVscodePath =
		EDITOR_CONTEXT(projectSettings).GetOrDefault<std::string>("vscode_path", "");
	if (!configuredVscodePath.empty())
	{
		const auto candidate = std::filesystem::path(configuredVscodePath);
		if (candidate.is_absolute() && std::filesystem::exists(candidate))
			vscodePath = candidate;
		else
			NLS_LOG_WARNING(
				"Configured VS Code path is not an existing absolute path: " +
				configuredVscodePath);
	}

	const auto editorExecutable = NLS::Platform::Process::GetCurrentExecutablePath();
	const auto brokerExecutable = editorExecutable.parent_path() /
#ifdef _WIN32
		"NullusDebugBroker.exe";
#else
		"NullusDebugBroker";
#endif
	const auto workspace = NLS::Editor::Debug::GenerateProjectDebugWorkspace(
		projectRoot,
		editorExecutable,
		brokerExecutable);
	if (!workspace.success)
	{
		NLS_LOG_ERROR("Could not generate the project C# debug workspace: " + workspace.errorMessage);
		return;
	}

	const auto projectOpen = NLS::Editor::Debug::ExternalCodeEditor::OpenDebugWorkspace(
		workspace.manifest,
		item.absolutePath,
		vscodePath);
	if (!projectOpen.success)
	{
		NLS_LOG_ERROR("Could not open generated C# project: " + projectOpen.errorMessage);
		return;
	}

	NLS_LOG_INFO(
		"C# project debug workspace opened for " + item.absolutePath.string() +
		". Press F5 in the IDE to reuse or start the matching Editor and attach C#.");
}

void Editor::Panels::AssetBrowser::PrepareLuaScriptDebugWorkspace(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
	if (projectRoot.empty())
	{
		NLS_LOG_ERROR("Cannot prepare Lua debugging: project root is empty.");
		return;
	}

	std::filesystem::path vscodePath;
	const auto configuredVscodePath =
		EDITOR_CONTEXT(projectSettings).GetOrDefault<std::string>("vscode_path", "");
	if (!configuredVscodePath.empty())
	{
		const auto candidate = std::filesystem::path(configuredVscodePath);
		if (candidate.is_absolute() && std::filesystem::exists(candidate))
			vscodePath = candidate;
		else
			NLS_LOG_WARNING(
				"Configured VS Code path is not an existing absolute path: " +
				configuredVscodePath);
	}

	NLS::Scripting::ScriptDebugSettings debugSettings;
	if (EDITOR_CONTEXT(scriptDebugService) != nullptr)
		debugSettings = EDITOR_CONTEXT(scriptDebugService)->GetSettings();
	debugSettings.enableLuaPanda = true;
	if (EDITOR_CONTEXT(scriptDebugService) != nullptr)
	{
		const auto status = EDITOR_CONTEXT(scriptDebugService)->SetSettings(debugSettings);
		if (!status.Succeeded())
			NLS_LOG_WARNING("LuaPanda debugging could not be enabled: " + status.message);
	}

	const auto editorExecutable = NLS::Platform::Process::GetCurrentExecutablePath();
	const auto brokerExecutable = editorExecutable.parent_path() /
#ifdef _WIN32
		"NullusDebugBroker.exe";
#else
		"NullusDebugBroker";
#endif
	const auto workspace = NLS::Editor::Debug::GenerateProjectDebugWorkspace(
		projectRoot,
		editorExecutable,
		brokerExecutable,
		debugSettings.luaPandaPort,
		debugSettings.stopOnEntry);
	if (!workspace.success)
	{
		NLS_LOG_WARNING("Could not generate Lua project debug workspace: " + workspace.errorMessage);
		return;
	}

	if (vscodePath.empty())
	{
		NLS_LOG_WARNING(
			"LuaPanda configuration is ready, but no absolute VS Code path is configured; "
			"set project setting 'vscode_path' to attach LuaPanda automatically.");
	}

	const auto workspaceOpen = vscodePath.empty()
		? NLS::Editor::Debug::ExternalCodeEditor::OpenWorkspace(projectRoot, vscodePath)
		: NLS::Editor::Debug::ExternalCodeEditor::OpenDebugWorkspace(
			workspace.manifest,
			item.absolutePath,
			vscodePath);
	if (!workspaceOpen.success)
	{
		NLS_LOG_ERROR("Could not open Lua project workspace: " + workspaceOpen.errorMessage);
		return;
	}

	if (vscodePath.empty())
	{
		const auto sourceOpen = NLS::Editor::Debug::ExternalCodeEditor::Open(
			item.absolutePath,
			1,
			1,
			vscodePath);
		if (!sourceOpen.success)
			NLS_LOG_WARNING("Could not open Lua script source: " + sourceOpen.errorMessage);
	}

	NLS_LOG_INFO(
		"LuaPanda debugging prepared for " + item.absolutePath.string() +
		" (workspace opened, port=" + std::to_string(debugSettings.luaPandaPort) + ").");
}

void Editor::Panels::AssetBrowser::OpenProjectGridItem(
	const NLS::Editor::Assets::AssetBrowserItem& item)
{
	using NLS::Editor::Assets::AssetBrowserItemKind;
	using NLS::Editor::Assets::AssetBrowserItemType;

	if (item.kind == AssetBrowserItemKind::Folder)
	{
		SelectProjectFolder(item.projectRelativePath);
		return;
	}

	if (item.dragResourcePath.empty())
		return;

	auto scriptExtension = item.absolutePath.extension().string();
	std::transform(
		scriptExtension.begin(),
		scriptExtension.end(),
		scriptExtension.begin(),
		[](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
	if (item.kind == AssetBrowserItemKind::SourceAsset &&
		item.type == AssetBrowserItemType::Script &&
		scriptExtension == ".cs")
	{
		SelectProjectGridItem(item);
		PrepareCSharpScriptDebugProject(item);
		return;
	}
	if (item.kind == AssetBrowserItemKind::SourceAsset &&
		item.type == AssetBrowserItemType::Script &&
		scriptExtension == ".lua")
	{
		SelectProjectGridItem(item);
		PrepareLuaScriptDebugWorkspace(item);
		return;
	}

	if (item.type == AssetBrowserItemType::Scene)
	{
		EDITOR_EXEC(LoadSceneFromDisk(item.absolutePath.string(), true));
		return;
	}

	if (item.type == AssetBrowserItemType::Prefab)
	{
		const auto projectRoot = ProjectRootFromAssetsFolder(m_projectAssetFolder);
		NLS::Editor::Assets::AssetDatabaseFacade database(
			NLS::Editor::Assets::MakeProjectEditorAssetRoots(projectRoot));
		if (!database.Refresh())
		{
			NLS_LOG_ERROR("Failed to refresh asset database before opening prefab: " + item.dragResourcePath);
			return;
		}
		if (!database.IsArtifactManifestCurrentForAssetPath(item.dragResourcePath))
		{
			NLS_LOG_ERROR("Skipped opening stale prefab artifact: " + item.dragResourcePath);
			return;
		}

		const auto prefabSubAssetKey = item.subAssetKey.empty()
			? "prefab:" + std::filesystem::path(item.dragResourcePath).stem().generic_string()
			: item.subAssetKey;
		auto prefab = database.LoadPrefabArtifactAtPath(item.dragResourcePath, prefabSubAssetKey);
		if (!prefab.has_value())
		{
			NLS_LOG_ERROR("Failed to load prefab artifact for prefab stage: " + item.dragResourcePath);
			return;
		}

		auto stage = NLS::Editor::Assets::PrefabUtilityFacade().LoadPrefabContents({
			&*prefab,
			prefab->assetId,
			prefabSubAssetKey,
			prefab->generatedModelPrefab || item.generatedReadOnly,
			item.dragResourcePath
		});
		if (stage.status != NLS::Editor::Assets::PrefabOperationStatus::Committed)
		{
			for (const auto& diagnostic : stage.diagnostics)
				NLS_LOG_ERROR(diagnostic.code + ": " + diagnostic.message);
			return;
		}

		EDITOR_EXEC(GetContext()).activePrefabStage = std::move(stage.stage);
		EDITOR_EXEC(NotifyPrefabStageOpened());
		EDITOR_PANEL(NLS::Editor::Panels::Hierarchy, "Hierarchy").RebuildFromCurrentScene();
		EDITOR_PANEL(NLS::Editor::Panels::SceneView, "Scene View").Focus();
		NLS_LOG_INFO("Opened prefab stage: " + item.dragResourcePath);
		return;
	}

	if (item.kind == AssetBrowserItemKind::SourceAsset &&
		item.type == AssetBrowserItemType::Material)
	{
		SelectProjectGridItem(item);
		const auto resourcePath = ProjectBrowserResourcePathForItem(item);
		NLS::Render::Resources::Material* material =
			NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager)[resourcePath];
		if (material != nullptr)
		{
			auto& materialEditor = EDITOR_PANEL(Editor::Panels::MaterialEditor, "Material Editor");
			materialEditor.SetTarget(*material);
			materialEditor.Open();
			materialEditor.Focus();

			auto& assetView = EDITOR_PANEL(Editor::Panels::AssetView, "Asset View");
			assetView.SetResource(material);
			assetView.Open();
			assetView.Focus();
		}
		return;
	}

	SelectProjectGridItem(item);

	if (item.previewableInAssetView)
	{
		auto& assetProperties = EDITOR_PANEL(Editor::Panels::AssetProperties, "Asset Properties");
		auto& assetView = EDITOR_PANEL(Editor::Panels::AssetView, "Asset View");
		assetProperties.Preview();
		assetView.Open();
		assetView.Focus();
	}
}

void Editor::Panels::AssetBrowser::StartWatchersAsync()
{
	const auto engineAssetFolder = m_engineAssetFolder;
	const auto projectAssetFolder = m_projectAssetFolder;
	try
	{
		m_watcherStartup = ScheduleAssetBrowserJobFuture(
			"AssetBrowser.WatcherStartup",
			[engineAssetFolder, projectAssetFolder]
			{
				WatcherStartupResult result;
				const auto engineWatcherStarted = result.engineAssetsWatcher.Start(engineAssetFolder);
				const auto projectWatcherStarted = result.projectAssetsWatcher.Start(projectAssetFolder);
				auto report = NLS::Editor::Assets::BuildAssetWatcherStartupReport(
					engineAssetFolder,
					engineWatcherStarted,
					projectAssetFolder,
					projectWatcherStarted);
				result.diagnostics = std::move(report.diagnostics);
				return result;
			});
		m_watchersStartupQueued = true;
	}
	catch (const std::exception& exception)
	{
		m_watchersStartupQueued = false;
		NLS_LOG_WARNING(std::string("Asset Browser watcher startup failed to schedule: ") + exception.what());
	}
	catch (...)
	{
		m_watchersStartupQueued = false;
		NLS_LOG_WARNING("Asset Browser watcher startup failed to schedule.");
	}
}

void Editor::Panels::AssetBrowser::StartWatchersSynchronously()
{
	m_watchersStartupQueued = true;
	const auto engineWatcherStarted = m_engineAssetsWatcher.Start(m_engineAssetFolder);
	const auto projectWatcherStarted = m_projectAssetsWatcher.Start(m_projectAssetFolder);
	auto report = NLS::Editor::Assets::BuildAssetWatcherStartupReport(
		m_engineAssetFolder,
		engineWatcherStarted,
		m_projectAssetFolder,
		projectWatcherStarted);
	for (const auto& diagnostic : report.diagnostics)
		NLS_LOG_WARNING(diagnostic.message);
}

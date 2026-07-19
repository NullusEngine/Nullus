#include "ImGui/imgui.h"

#include "Rendering/DebugSceneRenderer.h"
#include "Rendering/PickingRenderPass.h"
#include "Rendering/SceneRendererFactory.h"
#include "Core/EditorActions.h"
#include "Core/PrefabInstanceResourceLifetime.h"
#include "Core/RecentBackgroundWorkGate.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Assets/EditorAssetPathUtils.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Panels/SceneView.h"
#include "Panels/GameView.h"
#include "Panels/SceneViewPickingPolicy.h"
#include "Panels/ViewFrameLifecycle.h"
#include "Settings/EditorSettings.h"
#include "Profiling/Profiler.h"
#include "Core/EditorInteractionBlocker.h"
#include "Core/RendererResourceStreamingBudget.h"
#include "Core/SceneCameraFocus.h"
#include "ImGuizmo.h"
#include "Debug/Logger.h"
#include "Rendering/Context/DriverAccess.h"
#include "Rendering/Assets/MeshArtifact.h"
#include "Rendering/Settings/EPixelDataFormat.h"
#include "Rendering/Settings/EPixelDataType.h"
#include "Rendering/Tooling/MaterialVisualEvidence.h"
#include "Rendering/Settings/GraphicsBackendUtils.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ResourceManagement/ResourceLifetimeRegistry.h"
#include "Core/ResourceManagement/TextureManager.h"
#include "Rendering/Resources/Material.h"
#include "Jobs/JobSystem.h"
#include <ServiceLocator.h>
#include <UI/Plugins/DragDrop.h>
#include <UI/Plugins/IPlugin.h>
#include <UI/UIManager.h>
#include <UI/Widgets/Layout/Group.h>
#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
using namespace NLS;

namespace
{
constexpr float kSceneViewGizmoCameraLength = 8.0f;
constexpr float kSceneViewDefaultFocusDistance = 15.0f;
constexpr float kSceneViewPrefabDragFallbackDistance = 12.0f;
constexpr uint64_t kSceneViewHoverPickingVisibleDrawBudget = 1024u;
constexpr size_t kSceneViewImportedPrefabPreloadGateCapacity = 256u;
constexpr size_t kSceneViewValidationMinimumVisibleSceneLoadObjects = 32u;
constexpr auto kSceneViewImportedPrefabPreloadGateTtl = std::chrono::seconds(3);

std::mutex& SceneViewImportedPrefabPreloadMutex()
{
    static std::mutex mutex;
    return mutex;
}

NLS::Editor::Core::RecentBackgroundWorkGate& SceneViewImportedPrefabPreloadGate()
{
    static NLS::Editor::Core::RecentBackgroundWorkGate gate(
        kSceneViewImportedPrefabPreloadGateCapacity,
        kSceneViewImportedPrefabPreloadGateTtl);
    return gate;
}

std::string BuildSceneViewImportedPrefabPayloadKey(
    const NLS::Editor::Assets::EditorAssetDragPayload& payload)
{
    return NLS::Editor::Assets::GetEditorAssetDragPayloadPath(payload) + "|" +
        NLS::Editor::Assets::GetEditorAssetDragPayloadGuid(payload) + "|" +
        NLS::Editor::Assets::GetEditorAssetDragPayloadSubAssetKey(payload);
}

bool TryCacheSceneViewImportedPrefabPayload(
    const NLS::Editor::Assets::EditorAssetDragPayload& payload,
    std::string& assetPath,
    std::string& subAssetKey,
    NLS::Core::Assets::AssetId& assetId,
    NLS::Core::Assets::AssetType& assetType)
{
    assetPath = NLS::Editor::Assets::GetEditorAssetDragPayloadPath(payload);
    if (assetPath.empty())
        return false;

    assetId = NLS::Editor::Assets::GetEditorAssetDragPayloadAssetId(payload);
    if (!assetId.IsValid())
        return false;

    assetType = payload.generatedModelPrefab != 0u
        ? NLS::Core::Assets::AssetType::ModelScene
        : NLS::Core::Assets::InferAssetType(EDITOR_CONTEXT(projectAssetsPath) / std::filesystem::path(assetPath));
    if (assetType != NLS::Core::Assets::AssetType::ModelScene &&
        assetType != NLS::Core::Assets::AssetType::Prefab)
    {
        return false;
    }

    subAssetKey = NLS::Editor::Assets::GetEditorAssetDragPayloadSubAssetKey(payload);
    subAssetKey = NLS::Editor::Assets::NormalizeGeneratedPrefabSubAssetKeyForAssetPath(
        assetPath,
        std::move(subAssetKey),
        assetType);
    return true;
}

bool ScheduleSceneViewImportedPrefabPreloadOnce(
    const NLS::Editor::Assets::EditorAssetDragPayload& payload)
{
    const auto key = BuildSceneViewImportedPrefabPayloadKey(payload);
    if (key.empty())
        return false;
    {
        std::lock_guard lock(SceneViewImportedPrefabPreloadMutex());
        if (!SceneViewImportedPrefabPreloadGate().TryBegin(
                key,
                NLS::Editor::Core::RecentBackgroundWorkGate::Clock::now()))
            return false;
    }

    struct SceneViewPrefabDragPreloadJob final : NLS::Base::Jobs::IJob
    {
        SceneViewPrefabDragPreloadJob(
            NLS::Editor::Assets::EditorAssetDragPayload payload,
            std::filesystem::path projectAssetsPath,
            std::string key)
            : payload(std::move(payload))
            , projectAssetsPath(std::move(projectAssetsPath))
            , key(std::move(key))
        {
        }

        void Execute()
        {
            auto completion = SceneViewImportedPrefabPreloadGate().CompleteOnScopeExit(key);
            try
            {
                NLS::Editor::Assets::EditorAssetDragDropBridge bridge(projectAssetsPath);
                const auto path = NLS::Editor::Assets::GetEditorAssetDragPayloadPath(payload);
                const auto begin = std::chrono::steady_clock::now();
                NLS_LOG_INFO("Scene View prefab drag hot-cache preload started: " + path);
                const bool ready = bridge.PreloadImportedAssetHandlePrefabHotCache(payload);
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - begin).count();
                NLS_LOG_INFO(
                    std::string("Scene View prefab drag hot-cache preload ") +
                    (ready ? "ready: " : "not ready: ") +
                    path +
                    " elapsedMs=" +
                    std::to_string(elapsedMs));
            }
            catch (const std::exception& exception)
            {
                NLS_LOG_WARNING(std::string("Scene View prefab drag hot-cache preload failed: ") + exception.what());
            }
            catch (...)
            {
                NLS_LOG_WARNING("Scene View prefab drag hot-cache preload failed with an unknown exception.");
            }
        }

        NLS::Editor::Assets::EditorAssetDragPayload payload;
        std::filesystem::path projectAssetsPath;
        std::string key;
    };

    const auto projectAssetsPath = std::filesystem::path(EDITOR_CONTEXT(projectAssetsPath));
    const auto handle = NLS::Base::Jobs::Schedule(
        SceneViewPrefabDragPreloadJob(payload, projectAssetsPath, key),
        {
            {},
            NLS::Base::Jobs::JobPriority::High,
            NLS::Base::Jobs::JobSafetyPolicy::MaySyncWait,
            "SceneView::PrefabDragPreload"
        });
    const bool scheduled = handle.id != 0u;
    if (!scheduled)
    {
        std::lock_guard lock(SceneViewImportedPrefabPreloadMutex());
        SceneViewImportedPrefabPreloadGate().End(key);
    }
    else
    {
        NLS_LOG_INFO("Scene View prefab drag hot-cache preload queued: " + NLS::Editor::Assets::GetEditorAssetDragPayloadPath(payload));
    }
    return scheduled;
}

void HashSceneViewCacheValue(uint64_t& seed, const uint64_t value)
{
    seed ^= value + 0x9E3779B97F4A7C15ull + (seed << 6u) + (seed >> 2u);
}

void HashSceneViewCachePointer(uint64_t& seed, const void* value)
{
    HashSceneViewCacheValue(seed, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value)));
}

void HashSceneViewCacheFloat(uint64_t& seed, const float value)
{
    uint32_t bits = 0u;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(value));
    HashSceneViewCacheValue(seed, static_cast<uint64_t>(bits));
}

void HashSceneViewCacheVector2(uint64_t& seed, const NLS::Maths::Vector2& value)
{
    HashSceneViewCacheFloat(seed, value.x);
    HashSceneViewCacheFloat(seed, value.y);
}

void HashSceneViewCacheVector3(uint64_t& seed, const NLS::Maths::Vector3& value)
{
    HashSceneViewCacheFloat(seed, value.x);
    HashSceneViewCacheFloat(seed, value.y);
    HashSceneViewCacheFloat(seed, value.z);
}

uint64_t BeginSceneViewCacheSegment(const uint64_t salt)
{
    uint64_t seed = 0x51CE71E55EED0001ull;
    HashSceneViewCacheValue(seed, salt);
    return seed;
}

void TraceSceneViewCacheSegmentChange(
    const char* scopeName,
    const uint64_t previousValue,
    const uint64_t currentValue)
{
    if (previousValue == currentValue)
        return;

    NLS_PROFILE_NAMED_SCOPE(scopeName);
}

ImGuizmo::OPERATION ToNativeImGuizmoOperation(Editor::Core::EGizmoOperation operation)
{
    switch (Editor::Core::ToImGuizmoOperation(operation))
    {
    case Editor::Core::SceneViewGizmoOperation::Rotate:
        return ImGuizmo::ROTATE;
    case Editor::Core::SceneViewGizmoOperation::Scale:
        return ImGuizmo::SCALE;
    case Editor::Core::SceneViewGizmoOperation::Translate:
    default:
        return ImGuizmo::TRANSLATE;
    }
}

ImGuizmo::MODE ToNativeImGuizmoMode(const Editor::Core::SceneViewGizmoSpace space)
{
    return space == Editor::Core::SceneViewGizmoSpace::Local
        ? ImGuizmo::LOCAL
        : ImGuizmo::WORLD;
}

Engine::GameObject* PickGameObjectAtRenderCoordinate(
    Editor::Rendering::PickingRenderPass& pickingPass,
    const float x,
    const float y)
{
    auto pickingResult = pickingPass.PickAtRenderCoordinate(
        static_cast<uint32_t>(x),
        static_cast<uint32_t>(y));

    if (pickingResult.has_value())
    {
        if (const auto pickedGameObject = std::get_if<Engine::GameObject*>(&pickingResult.value()))
            return *pickedGameObject;
    }

    return nullptr;
}

Engine::GameObject* PickGameObjectNearRenderCoordinate(
    Editor::Rendering::PickingRenderPass& pickingPass,
    const Maths::Vector2& mousePos,
    const float maxRenderX,
    const float maxRenderY)
{
    constexpr float kClickPickRadius = 2.0f;
    const std::array<Maths::Vector2, 9> offsets {{
        {0.0f, 0.0f},
        {-kClickPickRadius, 0.0f},
        {kClickPickRadius, 0.0f},
        {0.0f, -kClickPickRadius},
        {0.0f, kClickPickRadius},
        {-kClickPickRadius, -kClickPickRadius},
        {kClickPickRadius, -kClickPickRadius},
        {-kClickPickRadius, kClickPickRadius},
        {kClickPickRadius, kClickPickRadius},
    }};

    for (const auto& offset : offsets)
    {
        const float sampleX = std::clamp(mousePos.x + offset.x, 0.0f, maxRenderX);
        const float sampleY = std::clamp(mousePos.y + offset.y, 0.0f, maxRenderY);
        if (auto* pickedGameObject = PickGameObjectAtRenderCoordinate(pickingPass, sampleX, sampleY))
            return pickedGameObject;
    }

    return nullptr;
}

bool ShouldLogScenePickingDiagnostics()
{
    const auto& diagnostics = NLS::Render::Settings::GetThreadDiagnosticsSettings();
    return diagnostics.dx12LogFrameFlow || diagnostics.editorLogScenePicking;
}

bool ShouldLogSceneCameraInputDiagnostics()
{
    const auto& diagnostics = NLS::Render::Settings::GetThreadDiagnosticsSettings();
    if (diagnostics.dx12LogFrameFlow || diagnostics.editorLogSceneCameraInput)
        return true;

    if (NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::EditorActions>())
    {
        const auto& editorDiagnostics = EDITOR_EXEC(GetContext()).GetDiagnosticsSettings();
        return editorDiagnostics.dx12LogFrameFlow || editorDiagnostics.editorLogSceneCameraInput;
    }

    return false;
}

void LogScenePickingDiagnostics(const std::string& message)
{
    if (ShouldLogScenePickingDiagnostics())
        NLS_LOG_INFO("[SceneViewPicking] " + message);
}

void LogSceneCameraInputDiagnostics(const std::string& message)
{
    if (ShouldLogSceneCameraInputDiagnostics())
        NLS_LOG_INFO("[SceneViewCamera] " + message);
}

}

std::optional<Editor::Rendering::DebugSceneRenderer::PrefabDragProxyDescriptor>
Editor::Panels::BuildSceneViewPrefabDragProxyDescriptor(
    const std::optional<Maths::Vector3>& placement,
    const bool hasActivePayload,
    const Engine::GameObject* activeRoot,
    const bool activeRootVisible)
{
    if (!hasActivePayload || (activeRoot != nullptr && activeRootVisible) || !placement.has_value())
        return std::nullopt;

    Editor::Rendering::DebugSceneRenderer::PrefabDragProxyDescriptor descriptor;
    descriptor.position = *placement;
    return descriptor;
}

bool Editor::Panels::ShouldTrustSceneViewRenderContentRevision(
    const bool hasActivePrefabDragPayload,
    const bool activePrefabDragCommitPending)
{
    return !hasActivePrefabDragPayload && !activePrefabDragCommitPending;
}

bool Editor::Panels::ShouldDeferSceneViewRenderForPendingSceneLoadResources(const size_t pendingTaskCount)
{
    (void)pendingTaskCount;
    return false;
}

bool Editor::Panels::ShouldSkipSceneViewSceneDrawablesForPendingSceneLoadResources(
    const size_t pendingTaskCount,
    const bool placeholderAlreadyRendered,
    const size_t visibleObjectCount)
{
    if (pendingTaskCount == 0u)
        return false;

    if (placeholderAlreadyRendered && visibleObjectCount > 0u)
        return false;

    return true;
}

bool Editor::Panels::ShouldSuppressSceneViewLightGridComputeForPendingSceneLoadResources(
    const size_t pendingTaskCount,
    const bool skipSceneDrawables)
{
    return pendingTaskCount > 0u && skipSceneDrawables;
}

bool Editor::Panels::ShouldForceSceneViewRenderForPendingSceneLoadResources(
    const size_t pendingTaskCount,
    const bool validationReadbackRequested)
{
    (void)pendingTaskCount;
    (void)validationReadbackRequested;
    return false;
}

bool Editor::Panels::ShouldWaitForSceneViewValidationReadbackSceneLoadResources(
    const bool activeResolution,
    const size_t pendingTaskCount,
    const size_t visibleObjectCount)
{
    (void)visibleObjectCount;
    if (pendingTaskCount == 0u)
        return activeResolution;
    return true;
}

bool Editor::Panels::ShouldWaitForSceneViewValidationReadbackAfterSceneLoadResources(
    const bool observedSceneLoadResources,
    const uint32_t stableFramesAfterResourcesReady,
    const uint32_t requiredStableFrames)
{
    return observedSceneLoadResources && stableFramesAfterResourcesReady < requiredStableFrames;
}

std::string Editor::Panels::BuildSceneViewValidationReadbackStatus(
    const uint64_t nonBlackPixels,
    const uint32_t maxChannel,
    const size_t pendingSceneLoadTextureLoads)
{
    if (pendingSceneLoadTextureLoads > 0u)
        return "pending-texture-tail";
    if (nonBlackPixels == 0u || maxChannel == 0u)
        return "failed-empty-frame";
    return "success";
}

uint64_t Editor::Panels::BuildSceneViewSceneLoadResourceCacheVersion(
    const size_t pendingTaskCount,
    const bool placeholderAlreadyRendered,
    const size_t visibleObjectCount)
{
    if (pendingTaskCount == 0u)
        return 0u;

    uint64_t version = BeginSceneViewCacheSegment(6u);
    HashSceneViewCacheValue(version, static_cast<uint64_t>(pendingTaskCount));
    HashSceneViewCacheValue(version, placeholderAlreadyRendered ? 1u : 0u);
    HashSceneViewCacheValue(version, static_cast<uint64_t>(visibleObjectCount));
    return version;
}

class Editor::Panels::SceneView::ViewportDragDropTarget final : public UI::IPlugin
{
public:
    explicit ViewportDragDropTarget(SceneView& owner)
        : m_owner(owner)
    {
    }

    void Execute() override
    {
        m_owner.HandleViewportAssetDragDrop();
    }

private:
    SceneView& m_owner;
};

Editor::Panels::SceneView::SceneView(
    const std::string& p_title,
    bool p_opened,
    const UI::PanelWindowSettings& p_windowSettings)
    : AViewControllable(p_title, p_opened, p_windowSettings), m_sceneManager(EDITOR_CONTEXT(sceneManager))
{
    // Scene View always renders editor overlays (grid/gizmo/light billboards),
    // so create the editor renderer during startup while the native startup
    // progress window is still visible.
    NLS_LOG_INFO("[Startup] Creating Scene View renderer");
    m_renderer = std::make_unique<Editor::Rendering::DebugSceneRenderer>(*EDITOR_CONTEXT(driver));
    SetRequiresRetiredFrameConsumption(true);
    SetRequiresImmediateRetiredFrameReadback(ShouldSceneViewRequestImmediatePickingReadback());

    m_camera.SetFar(5000.0f);
    m_image->AddPlugin<ViewportDragDropTarget>(*this);

	    m_destroyedListener = Engine::GameObject::DestroyedEvent += [this](const Engine::GameObject& actor)
	    {
	        if (m_highlightedGameObject == &actor)
	        {
	            m_highlightedGameObject = nullptr;
	        }
	        if (m_activeDraggedPrefabRoot == &actor)
	        {
	            ClearActivePrefabDragState();
	        }
	    };
}

Editor::Panels::SceneView::~SceneView()
{
    CancelActivePrefabDragInstance();
    Engine::GameObject::DestroyedEvent -= m_destroyedListener;
}

void Editor::Panels::SceneView::HandleViewportAssetDragDrop()
{
    if (!UI::BeginDragDropTarget())
    {
        if (m_activeDraggedPrefabPayload.has_value() && !m_activeDraggedPrefabCommitPending)
            CancelActivePrefabDragInstance();
        return;
    }

    if (const UI::DragDropPayloadView payloadView = UI::AcceptDragDropPayload(
            NLS::Editor::Assets::kEditorAssetDragPayloadType,
            UI::DragDropTargetFlags::AcceptBeforeDelivery);
        payloadView.data != nullptr)
    {
        const auto payload = *static_cast<const NLS::Editor::Assets::EditorAssetDragPayload*>(payloadView.data);
        UpdateActivePrefabDragInstance(payload);
        if (payloadView.delivered)
        {
            if (m_activeDraggedPrefabRootSceneToken.has_value() &&
                !IsActivePrefabDragSceneTokenCurrent(*m_activeDraggedPrefabRootSceneToken))
            {
                CancelActivePrefabDragInstance();
            }
            else if (m_activeDraggedPrefabRoot != nullptr &&
                m_activeDraggedPrefabRoot->IsAlive() &&
                m_activeDraggedPrefabRoot->GetTransform() != nullptr)
            {
                (void)CommitActivePrefabDragInstance();
            }
            else
            {
                m_activeDraggedPrefabDropSceneToken = CaptureActivePrefabDragSceneToken();
                m_activeDraggedPrefabDropPlacement = ResolveActivePrefabDragPlacement(
                    EDITOR_CONTEXT(inputManager)->GetMousePosition());
                m_activeDraggedPrefabCommitPending = true;
            }
        }
        UI::EndDragDropTarget();
        return;
    }

    if (m_activeDraggedPrefabPayload.has_value() && !m_activeDraggedPrefabCommitPending)
        CancelActivePrefabDragInstance();

    if (const UI::DragDropPayloadView payloadView = UI::AcceptDragDropPayload("File", UI::DragDropTargetFlags::None);
        payloadView.data != nullptr)
    {
        const auto payload = *static_cast<const std::pair<std::string, UI::Widgets::Group*>*>(payloadView.data);
        const std::string path = payload.first;

        switch (Utils::PathParser::GetFileType(path))
        {
        case Utils::PathParser::EFileType::SCENE:
            EDITOR_EXEC(LoadSceneFromDisk(path));
            break;
        default:
            if (!NLS::Editor::Assets::IsBuiltInResourcePath(path))
                break;
            EDITOR_EXEC(CreateGameObjectFromAsset(path, true));
            break;
        }
    }

    UI::EndDragDropTarget();
}

void Editor::Panels::SceneView::UpdateActivePrefabDragInstance(
    const NLS::Editor::Assets::EditorAssetDragPayload& payload)
{
    const auto key = BuildSceneViewImportedPrefabPayloadKey(payload);
    if (key.empty())
        return;

    if (m_activeDraggedPrefabPayloadKey != key)
    {
        CancelActivePrefabDragInstance();
        m_activeDraggedPrefabPayload = payload;
        m_activeDraggedPrefabPayloadKey = key;
        m_activeDraggedPrefabAssetPath.clear();
        m_activeDraggedPrefabSubAssetKey.clear();
        m_activeDraggedPrefabAssetId = {};
        m_activeDraggedPrefabAssetType = NLS::Core::Assets::AssetType::Unknown;
        m_activeDraggedPrefabHotCacheKey.reset();
        m_activeDraggedPrefabHotCacheKeyBuildAttempted = false;
        m_activeDraggedPrefabRootAwaitingRendererResources = false;
        m_activeDraggedPrefabCommitPending = false;
        (void)TryCacheSceneViewImportedPrefabPayload(
            payload,
            m_activeDraggedPrefabAssetPath,
            m_activeDraggedPrefabSubAssetKey,
            m_activeDraggedPrefabAssetId,
            m_activeDraggedPrefabAssetType);
        TryRefreshActivePrefabDragHotCacheKey();
    }

    const auto mousePosition = EDITOR_CONTEXT(inputManager)->GetMousePosition();
    const auto placement = ResolveActivePrefabDragPlacement(mousePosition);
    if (placement.has_value())
        m_activeDraggedPrefabProxyPlacement = placement;
#if defined(NLS_ENABLE_TEST_HOOKS)
    if (!m_disableActivePrefabDragPreloadForTesting)
    {
#endif
        (void)ScheduleSceneViewImportedPrefabPreloadOnce(payload);
#if defined(NLS_ENABLE_TEST_HOOKS)
    }
#endif
    TryRefreshActivePrefabDragHotCacheKey();

    if (m_activeDraggedPrefabRoot == nullptr)
    {
        if (m_activeDraggedPrefabAssetPath.empty() ||
            m_activeDraggedPrefabSubAssetKey.empty() ||
            !m_activeDraggedPrefabAssetId.IsValid() ||
            m_activeDraggedPrefabAssetType == NLS::Core::Assets::AssetType::Unknown ||
            !m_activeDraggedPrefabHotCacheKey.has_value())
        {
            return;
        }

        if (m_activeDraggedPrefabCommitPending &&
            m_activeDraggedPrefabDropSceneToken.has_value() &&
            !IsActivePrefabDragSceneTokenCurrent(*m_activeDraggedPrefabDropSceneToken))
        {
            CancelActivePrefabDragInstance();
            return;
        }

        auto* scene = GetScene();
        if (scene == nullptr)
            return;

        NLS::Editor::Assets::EditorAssetDragDropBridge bridge(EDITOR_CONTEXT(projectAssetsPath));
        auto result = bridge.TryDropImportedAssetHandleFromHotCacheIntoHierarchy(
            m_activeDraggedPrefabAssetPath,
            m_activeDraggedPrefabSubAssetKey,
            m_activeDraggedPrefabAssetId,
            m_activeDraggedPrefabAssetType,
            *m_activeDraggedPrefabHotCacheKey,
            *scene,
            {},
            &EDITOR_CONTEXT(prefabInstanceRegistry),
            nullptr,
            nullptr,
            true);
        if (!result.handled ||
            result.dragDrop.status != NLS::Editor::Assets::DragDropOperationStatus::Committed ||
            !result.dragDrop.instance.has_value() ||
            result.dragDrop.instance->instanceRoot == nullptr)
        {
            return;
        }

        m_activeDraggedPrefabRoot = result.dragDrop.instance->instanceRoot;
        m_activeDraggedPrefabRootSceneToken = CaptureActivePrefabDragSceneToken();
        m_activeDraggedPrefabRoot->SetEditorTransient(true);
        m_activeDraggedPrefabRootAwaitingRendererResources = false;
        if (result.dragDrop.deferredAssetReferenceResolutionRequested)
        {
            NLS::Editor::Core::PrefabInstanceAssetResolutionOptions resolutionOptions;
            resolutionOptions.hideRootUntilRendererResourcesReady = true;
            resolutionOptions.keepRootRenderingSuppressedOnFailure = true;
            resolutionOptions.rendererDependencyTemplates = result.dragDrop.rendererDependencyTemplates;
            m_activeDraggedPrefabRootAwaitingRendererResources = true;
            EDITOR_EXEC(QueuePrefabInstanceAssetResolution(
                result.dragDrop.instance ? &*result.dragDrop.instance : nullptr,
                result.dragDrop.sharedArtifact
                    ? result.dragDrop.sharedArtifact.get()
                    : (result.dragDrop.artifact ? &*result.dragDrop.artifact : nullptr),
                NLS::Editor::Assets::GetEditorAssetDragPayloadPath(payload),
                {},
                resolutionOptions));
        }
    }

    if (m_activeDraggedPrefabRootSceneToken.has_value() &&
        !IsActivePrefabDragSceneTokenCurrent(*m_activeDraggedPrefabRootSceneToken))
    {
        CancelActivePrefabDragInstance();
        return;
    }
    if (m_activeDraggedPrefabRootAwaitingRendererResources &&
        m_activeDraggedPrefabRoot != nullptr &&
        m_activeDraggedPrefabRoot->IsAlive())
    {
        const auto presentation = EDITOR_CONTEXT(prefabInstanceRegistry).GetPresentation(
            *m_activeDraggedPrefabRoot);
        m_activeDraggedPrefabRootAwaitingRendererResources =
            presentation.pendingResources;
        if (!m_activeDraggedPrefabRootAwaitingRendererResources)
            m_activeDraggedPrefabProxyPlacement.reset();
    }

    const auto targetPlacement = m_activeDraggedPrefabCommitPending && m_activeDraggedPrefabDropPlacement.has_value()
        ? m_activeDraggedPrefabDropPlacement
        : placement;
    if (targetPlacement.has_value() &&
        m_activeDraggedPrefabRoot != nullptr &&
        m_activeDraggedPrefabRoot->IsAlive() &&
        m_activeDraggedPrefabRoot->GetTransform() != nullptr)
    {
        if (!m_activeDraggedPrefabRootAwaitingRendererResources)
            m_activeDraggedPrefabProxyPlacement.reset();
        m_activeDraggedPrefabRoot->GetTransform()->SetWorldPosition(*targetPlacement);
    }
}

void Editor::Panels::SceneView::TryRefreshActivePrefabDragHotCacheKey()
{
    if (m_activeDraggedPrefabHotCacheKey.has_value() ||
        m_activeDraggedPrefabAssetPath.empty() ||
        m_activeDraggedPrefabSubAssetKey.empty() ||
        !m_activeDraggedPrefabAssetId.IsValid() ||
        m_activeDraggedPrefabAssetType == NLS::Core::Assets::AssetType::Unknown)
    {
        return;
    }

    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(EDITOR_CONTEXT(projectAssetsPath));
    m_activeDraggedPrefabHotCacheKey = bridge.TryFindImportedPrefabHotCacheKey(
        m_activeDraggedPrefabAssetPath,
        m_activeDraggedPrefabSubAssetKey,
        m_activeDraggedPrefabAssetId,
        m_activeDraggedPrefabAssetType,
        NLS::Editor::Assets::UnifiedPrefabReadiness::MeshMaterialTextureReady);
    if (m_activeDraggedPrefabHotCacheKey.has_value())
    {
        m_activeDraggedPrefabHotCacheKeyBuildAttempted = true;
        return;
    }

    m_activeDraggedPrefabHotCacheKey = bridge.TryFindImportedPrefabHotCacheKey(
        m_activeDraggedPrefabAssetPath,
        m_activeDraggedPrefabSubAssetKey,
        m_activeDraggedPrefabAssetId,
        m_activeDraggedPrefabAssetType,
        NLS::Editor::Assets::UnifiedPrefabReadiness::PrefabGraphOnly);
    m_activeDraggedPrefabHotCacheKeyBuildAttempted = true;
}

void Editor::Panels::SceneView::ClearActivePrefabDragState()
{
    m_activeDraggedPrefabPayload.reset();
    m_activeDraggedPrefabPayloadKey.clear();
    m_activeDraggedPrefabAssetPath.clear();
    m_activeDraggedPrefabSubAssetKey.clear();
    m_activeDraggedPrefabAssetId = {};
    m_activeDraggedPrefabAssetType = NLS::Core::Assets::AssetType::Unknown;
    m_activeDraggedPrefabHotCacheKey.reset();
    m_activeDraggedPrefabHotCacheKeyBuildAttempted = false;
    m_activeDraggedPrefabRoot = nullptr;
    m_activeDraggedPrefabDropSceneToken.reset();
    m_activeDraggedPrefabRootSceneToken.reset();
    m_activeDraggedPrefabDropPlacement.reset();
    m_activeDraggedPrefabProxyPlacement.reset();
    m_activeDraggedPrefabRootAwaitingRendererResources = false;
    m_activeDraggedPrefabCommitPending = false;
}

void Editor::Panels::SceneView::CancelActivePrefabDragInstance()
{
    auto* root = m_activeDraggedPrefabRoot;
    const auto rootSceneToken = m_activeDraggedPrefabRootSceneToken;
    ClearActivePrefabDragState();
    if (root == nullptr)
        return;
    NLS::Editor::Core::CleanupPrefabInstanceMarkedDestroy(
        EDITOR_CONTEXT(prefabInstanceRegistry),
        EDITOR_CONTEXT(resourceLifetimeRegistry),
        *root);
    if (rootSceneToken.has_value() && !IsActivePrefabDragSceneTokenCurrent(*rootSceneToken))
    {
        root->MarkAsDestroy();
        return;
    }
    if ((!rootSceneToken.has_value() || IsActivePrefabDragSceneTokenCurrent(*rootSceneToken)) &&
        GetScene() != nullptr)
    {
        auto* scene = GetScene();
        scene->DestroyGameObject(*root);
        scene->CollectGarbages();
    }
    else
    {
        root->MarkAsDestroy();
    }
}
bool Editor::Panels::SceneView::CommitActivePrefabDragInstance()
{
    auto* root = m_activeDraggedPrefabRoot;
    if (m_activeDraggedPrefabRootSceneToken.has_value() &&
        !IsActivePrefabDragSceneTokenCurrent(*m_activeDraggedPrefabRootSceneToken))
    {
        ClearActivePrefabDragState();
        return false;
    }
    if (root == nullptr || !root->IsAlive())
    {
        ClearActivePrefabDragState();
        return false;
    }
    bool replaceGraphOnlyPreviewWithRendererReadyInstance = false;
    if (m_activeDraggedPrefabHotCacheKey.has_value())
    {
        NLS::Editor::Assets::EditorAssetDragDropBridge bridge(EDITOR_CONTEXT(projectAssetsPath));
        if (!m_activeDraggedPrefabHotCacheKey->rendererArtifactReadinessRequired)
        {
            replaceGraphOnlyPreviewWithRendererReadyInstance = true;
            m_activeDraggedPrefabHotCacheKey = bridge.TryFindImportedPrefabHotCacheKey(
                m_activeDraggedPrefabAssetPath,
                m_activeDraggedPrefabSubAssetKey,
                m_activeDraggedPrefabAssetId,
                m_activeDraggedPrefabAssetType,
                NLS::Editor::Assets::UnifiedPrefabReadiness::MeshMaterialTextureReady);
            if (!m_activeDraggedPrefabHotCacheKey.has_value())
            {
                CancelActivePrefabDragInstance();
                return false;
            }
        }
        if (!bridge.IsImportedPrefabHotCacheKeyCurrent(
                m_activeDraggedPrefabAssetPath,
                m_activeDraggedPrefabSubAssetKey,
                m_activeDraggedPrefabAssetId,
                m_activeDraggedPrefabAssetType,
                *m_activeDraggedPrefabHotCacheKey))
        {
            CancelActivePrefabDragInstance();
            return false;
        }
    }

    const auto placement = m_activeDraggedPrefabDropPlacement.has_value()
        ? m_activeDraggedPrefabDropPlacement
        : ResolveActivePrefabDragPlacement(EDITOR_CONTEXT(inputManager)->GetMousePosition());
    if (replaceGraphOnlyPreviewWithRendererReadyInstance)
    {
        const auto payload = m_activeDraggedPrefabPayload;
        const auto assetPath = m_activeDraggedPrefabAssetPath;
        const auto subAssetKey = m_activeDraggedPrefabSubAssetKey;
        const auto assetId = m_activeDraggedPrefabAssetId;
        const auto assetType = m_activeDraggedPrefabAssetType;
        const auto rendererReadyHotCacheKey = m_activeDraggedPrefabHotCacheKey;
        CancelActivePrefabDragInstance();
        if (!rendererReadyHotCacheKey.has_value())
            return false;

        auto* scene = GetScene();
        if (scene == nullptr)
            return false;

        NLS::Editor::Assets::EditorAssetDragDropBridge bridge(EDITOR_CONTEXT(projectAssetsPath));
        auto result = bridge.TryDropImportedAssetHandleFromHotCacheIntoHierarchy(
            assetPath,
            subAssetKey,
            assetId,
            assetType,
            *rendererReadyHotCacheKey,
            *scene,
            {},
            &EDITOR_CONTEXT(prefabInstanceRegistry),
            nullptr,
            nullptr);
        if (!result.handled ||
            result.dragDrop.status != NLS::Editor::Assets::DragDropOperationStatus::Committed ||
            !result.dragDrop.instance.has_value() ||
            result.dragDrop.instance->instanceRoot == nullptr)
        {
            return false;
        }

        root = result.dragDrop.instance->instanceRoot;
        if (placement.has_value() && root->GetTransform() != nullptr)
            root->GetTransform()->SetWorldPosition(*placement);
        if (result.dragDrop.deferredAssetReferenceResolutionRequested)
        {
            NLS::Editor::Core::PrefabInstanceAssetResolutionOptions resolutionOptions;
            resolutionOptions.hideRootUntilRendererResourcesReady = true;
            resolutionOptions.keepRootRenderingSuppressedOnFailure = true;
            resolutionOptions.rendererDependencyTemplates = result.dragDrop.rendererDependencyTemplates;
            EDITOR_EXEC(QueuePrefabInstanceAssetResolution(
                result.dragDrop.instance ? &*result.dragDrop.instance : nullptr,
                result.dragDrop.sharedArtifact
                    ? result.dragDrop.sharedArtifact.get()
                    : (result.dragDrop.artifact ? &*result.dragDrop.artifact : nullptr),
                payload.has_value()
                    ? NLS::Editor::Assets::GetEditorAssetDragPayloadPath(*payload)
                    : assetPath,
                {},
                resolutionOptions));
        }
        EDITOR_EXEC(SelectGameObject(*root));
        EDITOR_EXEC(MarkOwningSceneDirty(*root));
        EDITOR_PANEL(Editor::Panels::Hierarchy, "Hierarchy").RebuildFromCurrentScene();
        ClearActivePrefabDragState();
        return true;
    }

    if (placement.has_value() && root->GetTransform() != nullptr)
    {
        root->GetTransform()->SetWorldPosition(*placement);
    }
    root->SetEditorTransient(false);
    EDITOR_EXEC(SelectGameObject(*root));
    EDITOR_EXEC(MarkOwningSceneDirty(*root));
    EDITOR_PANEL(Editor::Panels::Hierarchy, "Hierarchy").RebuildFromCurrentScene();
    ClearActivePrefabDragState();
    return true;
}

Editor::Core::EditorActions::SceneMutationToken Editor::Panels::SceneView::CaptureActivePrefabDragSceneToken() const
{
    return EDITOR_EXEC(CaptureSceneMutationToken());
}

bool Editor::Panels::SceneView::IsActivePrefabDragSceneTokenCurrent(
    const Editor::Core::EditorActions::SceneMutationToken& token)
{
    const auto current = CaptureActivePrefabDragSceneToken();
    return current.mainSceneGeneration == token.mainSceneGeneration &&
        current.prefabStageGeneration == token.prefabStageGeneration &&
        GetScene() != nullptr;
}

std::optional<Maths::Vector3> Editor::Panels::SceneView::ResolveActivePrefabDragPlacement(
    const Maths::Vector2& mousePosition) const
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    if (m_activePrefabDragPlacementOverrideForTesting.has_value())
        return m_activePrefabDragPlacementOverrideForTesting;
#endif
    if (m_camera.transform == nullptr)
        return std::nullopt;

    const auto localPosition = GetLocalViewPosition(mousePosition);
    const auto [safeWidth, safeHeight] = GetSafeSize();
    if (!localPosition.has_value() || safeWidth == 0u || safeHeight == 0u)
        return m_camera.GetPosition() + m_camera.transform->GetWorldForward() * kSceneViewPrefabDragFallbackDistance;

    const float width = std::max(1.0f, static_cast<float>(safeWidth));
    const float height = std::max(1.0f, static_cast<float>(safeHeight));
    const float ndcX = (localPosition->x / width) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (localPosition->y / height) * 2.0f;
    const float aspect = width / height;
    const float tanHalfFov = std::tan(Maths::DegreesToRadians(m_camera.GetFov()) * 0.5f);
    const float screenRightNdcX = -ndcX;

    auto rayDirection =
        m_camera.transform->GetWorldForward() +
        m_camera.transform->GetWorldRight() * (screenRightNdcX * tanHalfFov * aspect) +
        m_camera.transform->GetWorldUp() * (ndcY * tanHalfFov);
    rayDirection.Normalise();

    const auto cameraPosition = m_camera.GetPosition();
    if (std::fabs(rayDirection.y) > Maths::SMALL_NUMBER)
    {
        const float distanceToGround = -cameraPosition.y / rayDirection.y;
        if (distanceToGround > 0.0f && distanceToGround < m_camera.GetFar())
            return cameraPosition + rayDirection * distanceToGround;
    }

    const float fallbackDistance = m_cameraFocus.hasFocus
        ? std::max(1.0f, m_cameraFocus.focusDistance)
        : kSceneViewPrefabDragFallbackDistance;
    return cameraPosition + rayDirection * fallbackDistance;
}

#if defined(NLS_ENABLE_TEST_HOOKS)
Editor::Panels::SceneView::PrefabDragProxySceneViewLoopValidation
Editor::Panels::SceneView::ValidatePrefabDragProxySceneViewLoopForTesting(
    const NLS::Editor::Assets::EditorAssetDragPayload& payload,
    const std::vector<Maths::Vector3>& placements)
{
    PrefabDragProxySceneViewLoopValidation validation;
    const auto previousPlacementOverride = m_activePrefabDragPlacementOverrideForTesting;
    const bool previousPreloadDisabled = m_disableActivePrefabDragPreloadForTesting;

    CancelActivePrefabDragInstance();
    m_disableActivePrefabDragPreloadForTesting = true;

    for (const auto& placement : placements)
    {
        UI::DragDropTargetPayloadForTesting targetPayload;
        targetPayload.type = NLS::Editor::Assets::kEditorAssetDragPayloadType;
        targetPayload.bytes.resize(sizeof(payload));
        std::memcpy(targetPayload.bytes.data(), &payload, sizeof(payload));
        targetPayload.targetActive = true;
        targetPayload.delivered = false;
        UI::SetDragDropTargetPayloadForTesting(targetPayload);

        m_activePrefabDragPlacementOverrideForTesting = placement;
        HandleViewportAssetDragDrop();

        validation.dragLoopExercised = validation.dragLoopExercised ||
            m_activeDraggedPrefabPayload.has_value();
        validation.payloadAcceptedBeforeDelivery = validation.payloadAcceptedBeforeDelivery ||
            (m_activeDraggedPrefabPayload.has_value() && !m_activeDraggedPrefabCommitPending);

        const auto descriptor = BuildSceneViewPrefabDragProxyDescriptor(
            m_activeDraggedPrefabProxyPlacement,
            m_activeDraggedPrefabPayload.has_value(),
            m_activeDraggedPrefabRoot,
            !m_activeDraggedPrefabRootAwaitingRendererResources);
        if (!descriptor.has_value())
        {
            validation.followedPlacement = false;
            continue;
        }

        validation.followedPlacement = validation.followedPlacement &&
            descriptor->position.x == placement.x &&
            descriptor->position.y == placement.y &&
            descriptor->position.z == placement.z;
        validation.descriptorPlacements.push_back(descriptor->position);
    }

    validation.sceneRootCreatedByProxy = m_activeDraggedPrefabRoot != nullptr;
    UI::ClearDragDropTargetPayloadForTesting();
    CancelActivePrefabDragInstance();
    m_activePrefabDragPlacementOverrideForTesting = previousPlacementOverride;
    m_disableActivePrefabDragPreloadForTesting = previousPreloadDisabled;
    return validation;
}
#endif

void Editor::Panels::SceneView::EnsureRenderer()
{
    if (m_renderer != nullptr)
        return;

    NLS_LOG_INFO("[Startup] Creating Scene View renderer");
    m_renderer = std::make_unique<Editor::Rendering::DebugSceneRenderer>(*EDITOR_CONTEXT(driver));
}

void Editor::Panels::SceneView::Update(float p_deltaTime)
{
    using namespace Windowing::Inputs;
    if (NLS::Editor::Core::GetPendingSceneLoadRendererResourceResolutionTaskCount() == 0u)
        m_hasRenderedSceneLoadResourcePlaceholder = false;

    const auto previousCameraPosition = m_camera.GetPosition();
    const auto previousCameraRotation = m_camera.GetRotation();
    const Maths::Vector2 mousePosition = EDITOR_CONTEXT(inputManager)->GetMousePosition();
    const bool shortcutsWindowOpen = Editor::Core::DoesShortcutSettingsWindowBlockSceneInput();
    const bool viewportInputAvailable = HasViewportImageInputBounds();
    const bool mouseOverSceneView = viewportInputAvailable &&
        (IsMouseWithinView(mousePosition) ||
            (m_image != nullptr && m_image->WasHoveredLastDraw()));
    const bool sceneViewActive = !shortcutsWindowOpen && (IsFocused() || IsHovered() || mouseOverSceneView);
    const bool isAnyItemActive = NLS_SERVICE(UI::UIManager).IsAnyItemActive();
    const bool blockCameraInput = ShouldSceneViewBlockCameraInput(
        shortcutsWindowOpen,
        isAnyItemActive,
        mouseOverSceneView,
        ImGui::GetIO().WantTextInput);
    const bool sceneViewInputContextActive = sceneViewActive && !(isAnyItemActive && !mouseOverSceneView);
    m_cameraController.SetInputBlocked(blockCameraInput);
    if (shortcutsWindowOpen)
    {
        m_cameraController.ResetMouseInteractionState();
        m_gizmoInteraction = {};
        m_pendingClickPickRenderPos.reset();
        m_pendingClickPickingSignature.reset();
        m_pendingClickMinReadablePickingFrameSerial = 0u;
        m_highlightedGameObject = nullptr;
        m_hasPickingSample = false;
    }
    EnsureCameraFocus();
    m_cameraController.SetFocusState(&m_cameraFocus);
    m_cameraController.SetInputActive(sceneViewInputContextActive);
    if (HasViewportImageBounds())
    {
        const auto imageMin = GetViewportImageMin();
        const auto imageMax = GetViewportImageMax();
        m_cameraController.SetViewportHeight(std::max(1.0f, imageMax.y - imageMin.y));
    }

    AViewControllable::Update(p_deltaTime);
    if (m_activeDraggedPrefabPayload.has_value())
    {
        UpdateActivePrefabDragInstance(*m_activeDraggedPrefabPayload);
        if (m_activeDraggedPrefabCommitPending &&
            m_activeDraggedPrefabRoot != nullptr &&
            m_activeDraggedPrefabRoot->IsAlive() &&
            m_activeDraggedPrefabRoot->GetTransform() != nullptr)
        {
            (void)CommitActivePrefabDragInstance();
        }
    }
    m_cameraMovedForPresentation = HasSceneViewCameraMotionForPresentation(
        previousCameraPosition,
        previousCameraRotation,
        m_camera.GetPosition(),
        m_camera.GetRotation());
    if (ShouldLogSceneCameraInputDiagnostics())
    {
        std::ostringstream stream;
        stream << "dt=" << p_deltaTime
            << " focused=" << IsFocused()
            << " hovered=" << IsHovered()
            << " mouseOver=" << mouseOverSceneView
            << " hasBounds=" << HasViewportImageBounds()
            << " active=" << sceneViewActive
            << " blockCamera=" << blockCameraInput
            << " shortcutsBlocked=" << shortcutsWindowOpen
            << " cameraControl=" << IsCameraControlActive()
            << " moved=" << m_cameraMovedForPresentation
            << " mouse=(" << mousePosition.x << "," << mousePosition.y << ")"
            << " pos=(" << m_camera.GetPosition().x << "," << m_camera.GetPosition().y << "," << m_camera.GetPosition().z << ")"
            << " rot=(" << m_camera.GetRotation().x << "," << m_camera.GetRotation().y << "," << m_camera.GetRotation().z << "," << m_camera.GetRotation().w << ")";
        LogSceneCameraInputDiagnostics(stream.str());
    }
}

Editor::Core::EGizmoOperation Editor::Panels::SceneView::GetCurrentGizmoOperation() const
{
    return m_currentOperation;
}

void Editor::Panels::SceneView::SetCurrentGizmoOperation(const Editor::Core::EGizmoOperation p_operation)
{
    m_currentOperation = p_operation;
}

Editor::Core::SceneViewGizmoPivot Editor::Panels::SceneView::GetCurrentGizmoPivot() const
{
    return m_currentPivot;
}

void Editor::Panels::SceneView::SetCurrentGizmoPivot(const Editor::Core::SceneViewGizmoPivot p_pivot)
{
    m_currentPivot = p_pivot;
}

void Editor::Panels::SceneView::ToggleCurrentGizmoPivot()
{
    m_currentPivot = Core::ToggleGizmoPivot(m_currentPivot);
}

Editor::Core::SceneViewGizmoSpace Editor::Panels::SceneView::GetCurrentGizmoSpace() const
{
    return m_currentSpace;
}

void Editor::Panels::SceneView::SetCurrentGizmoSpace(const Editor::Core::SceneViewGizmoSpace p_space)
{
    m_currentSpace = p_space;
}

void Editor::Panels::SceneView::ToggleCurrentGizmoSpace()
{
    m_currentSpace = Core::ToggleGizmoSpace(m_currentSpace);
}

void Editor::Panels::SceneView::InitFrame()
{
    AViewControllable::InitFrame();

    auto* debugRenderer = dynamic_cast<Editor::Rendering::DebugSceneRenderer*>(m_renderer.get());
    if (debugRenderer == nullptr)
        return;

    Engine::GameObject* selectedGameObject = nullptr;

    if (EDITOR_EXEC(IsAnyGameObjectSelected()))
    {
        selectedGameObject = EDITOR_EXEC(GetSelectedGameObject());
    }

    m_requestPickingFrameForClick = ShouldRequestHitProxyPickingFrameForClick(
        EDITOR_CONTEXT(inputManager)->IsMouseButtonPressed(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_LEFT),
        m_pendingClickPickRenderPos.has_value(),
        m_pendingClickPickingSignature.has_value());
    m_requestPickingFrame = ShouldRequestPickingFrame();
    if (!m_requestPickingFrame)
        m_requestPickingFrameForClick = false;
    debugRenderer->AddDescriptor<Rendering::DebugSceneRenderer::DebugSceneDescriptor>({
        m_highlightedGameObject,
        selectedGameObject,
        m_requestPickingFrame,
        nullptr,
        m_requestPickingFrameForClick,
        kSceneViewHoverPickingVisibleDrawBudget,
        m_pendingClickMinReadablePickingFrameSerial,
        BuildSceneViewPrefabDragProxyDescriptor(
            m_activeDraggedPrefabProxyPlacement,
            m_activeDraggedPrefabPayload.has_value(),
            m_activeDraggedPrefabRoot,
            !m_activeDraggedPrefabRootAwaitingRendererResources)});
}

Engine::SceneSystem::Scene* Editor::Panels::SceneView::GetScene()
{
    if (EDITOR_CONTEXT(activePrefabStage).has_value() && EDITOR_CONTEXT(activePrefabStage)->stageScene)
        return EDITOR_CONTEXT(activePrefabStage)->stageScene.get();

    return m_sceneManager.GetCurrentScene();
}

Engine::Rendering::BaseSceneRenderer::SceneDescriptor Editor::Panels::SceneView::CreateSceneDescriptor()
{
    auto descriptor = AViewControllable::CreateSceneDescriptor();
    const size_t pendingSceneLoadResourceTasks =
        NLS::Editor::Core::GetPendingSceneLoadRendererResourceResolutionTaskCount();
    const size_t visibleSceneLoadResourceObjects =
        NLS::Editor::Core::GetVisibleSceneLoadRendererResourceResolutionObjectCount();
    if (pendingSceneLoadResourceTasks == 0u)
        m_hasRenderedSceneLoadResourcePlaceholder = false;

    descriptor.skipSceneDrawables =
        ShouldSkipSceneViewSceneDrawablesForPendingSceneLoadResources(
            pendingSceneLoadResourceTasks,
            m_hasRenderedSceneLoadResourcePlaceholder,
            visibleSceneLoadResourceObjects);
    descriptor.suppressVisibleMaterialTextureRequests =
        pendingSceneLoadResourceTasks > 0u;
    descriptor.suppressHZBOcclusion =
        pendingSceneLoadResourceTasks > 0u;
    descriptor.suppressLightGridCompute =
        ShouldSuppressSceneViewLightGridComputeForPendingSceneLoadResources(
            pendingSceneLoadResourceTasks,
            descriptor.skipSceneDrawables);
    descriptor.allowDefaultMaterialForUnresolvedExplicitMaterials =
        pendingSceneLoadResourceTasks > 0u;
    descriptor.trustSceneRenderContentRevision = ShouldTrustSceneViewRenderContentRevision(
        m_activeDraggedPrefabPayload.has_value(),
        m_activeDraggedPrefabCommitPending);
    if (descriptor.skipSceneDrawables)
        m_hasRenderedSceneLoadResourcePlaceholder = true;
    return descriptor;
}

bool Editor::Panels::SceneView::ShouldUseStaticFrameCache() const
{
    return true;
}

uint64_t Editor::Panels::SceneView::BuildStaticFrameCacheKey(
    const Render::Entities::Camera& camera,
    const Engine::SceneSystem::Scene& scene,
    const uint16_t width,
    const uint16_t height) const
{
    const uint64_t baseKey = AViewControllable::BuildStaticFrameCacheKey(camera, scene, width, height);

    uint64_t highlightKey = BeginSceneViewCacheSegment(1u);
    HashSceneViewCachePointer(highlightKey, m_highlightedGameObject);

    uint64_t gizmoKey = BeginSceneViewCacheSegment(2u);
    HashSceneViewCacheValue(gizmoKey, static_cast<uint64_t>(m_currentOperation));
    HashSceneViewCacheValue(gizmoKey, static_cast<uint64_t>(m_currentPivot));
    HashSceneViewCacheValue(gizmoKey, static_cast<uint64_t>(m_currentSpace));
    HashSceneViewCacheValue(gizmoKey, m_gizmoInteraction.isHovered ? 1u : 0u);
    HashSceneViewCacheValue(gizmoKey, m_gizmoInteraction.isUsing ? 1u : 0u);
    HashSceneViewCacheValue(gizmoKey, m_gizmoInteraction.isViewHovered ? 1u : 0u);
    HashSceneViewCacheValue(gizmoKey, m_gizmoInteraction.isViewUsing ? 1u : 0u);

    uint64_t focusKey = BeginSceneViewCacheSegment(3u);
    HashSceneViewCacheValue(focusKey, m_cameraFocus.hasFocus ? 1u : 0u);
    HashSceneViewCacheFloat(focusKey, m_cameraFocus.focusDistance);
    HashSceneViewCacheVector3(focusKey, m_cameraFocus.focusPoint);

    uint64_t selectionKey = BeginSceneViewCacheSegment(4u);
    if (NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::EditorActions>())
    {
        HashSceneViewCacheValue(selectionKey, EDITOR_EXEC(IsAnyGameObjectSelected()) ? 1u : 0u);
        HashSceneViewCachePointer(selectionKey, EDITOR_EXEC(GetSelectedGameObject()));
        HashSceneViewCacheValue(
            selectionKey,
            EDITOR_EXEC(GetContext()).activePrefabStage.has_value() ? 1u : 0u);
    }

    const size_t pendingSceneLoadResourceTasks =
        NLS::Editor::Core::GetPendingSceneLoadRendererResourceResolutionTaskCount();
    const size_t visibleSceneLoadResourceObjects =
        NLS::Editor::Core::GetVisibleSceneLoadRendererResourceResolutionObjectCount();
    const bool skipSceneDrawables =
        ShouldSkipSceneViewSceneDrawablesForPendingSceneLoadResources(
            pendingSceneLoadResourceTasks,
            m_hasRenderedSceneLoadResourcePlaceholder,
            visibleSceneLoadResourceObjects);
    const uint64_t sceneLoadResourceCacheVersion =
        BuildSceneViewSceneLoadResourceCacheVersion(
            pendingSceneLoadResourceTasks,
            m_hasRenderedSceneLoadResourcePlaceholder,
            visibleSceneLoadResourceObjects);
    uint64_t sceneDrawableKey = BeginSceneViewCacheSegment(5u);
    HashSceneViewCacheValue(sceneDrawableKey, skipSceneDrawables ? 1u : 0u);
    HashSceneViewCacheValue(sceneDrawableKey, sceneLoadResourceCacheVersion);

    m_lastComputedStaticCacheBaseKey = baseKey;
    m_lastComputedStaticCacheHighlightKey = highlightKey;
    m_lastComputedStaticCacheGizmoKey = gizmoKey;
    m_lastComputedStaticCacheFocusKey = focusKey;
    m_lastComputedStaticCacheSelectionKey = selectionKey;
    m_lastComputedStaticCacheSceneLoadResourcesKey = sceneDrawableKey;

    uint64_t seed = 0x51CE71E55EEDCACEull;
    HashSceneViewCacheValue(seed, baseKey);
    HashSceneViewCacheValue(seed, highlightKey);
    HashSceneViewCacheValue(seed, gizmoKey);
    HashSceneViewCacheValue(seed, focusKey);
    HashSceneViewCacheValue(seed, selectionKey);
    HashSceneViewCacheValue(seed, sceneDrawableKey);
    return seed;
}

void Editor::Panels::SceneView::TraceStaticFrameCacheKeyChanged(
    const uint64_t previousKey,
    const uint64_t currentKey) const
{
    (void)previousKey;
    (void)currentKey;

    TraceSceneViewCacheSegmentChange(
        "SceneView::StaticCacheKeyChanged::BaseCameraSceneViewport",
        m_committedStaticCacheBaseKey,
        m_lastComputedStaticCacheBaseKey);
    TraceSceneViewCacheSegmentChange(
        "SceneView::StaticCacheKeyChanged::Highlight",
        m_committedStaticCacheHighlightKey,
        m_lastComputedStaticCacheHighlightKey);
    TraceSceneViewCacheSegmentChange(
        "SceneView::StaticCacheKeyChanged::Gizmo",
        m_committedStaticCacheGizmoKey,
        m_lastComputedStaticCacheGizmoKey);
    TraceSceneViewCacheSegmentChange(
        "SceneView::StaticCacheKeyChanged::Focus",
        m_committedStaticCacheFocusKey,
        m_lastComputedStaticCacheFocusKey);
    TraceSceneViewCacheSegmentChange(
        "SceneView::StaticCacheKeyChanged::Selection",
        m_committedStaticCacheSelectionKey,
        m_lastComputedStaticCacheSelectionKey);
    TraceSceneViewCacheSegmentChange(
        "SceneView::StaticCacheKeyChanged::SceneLoadResources",
        m_committedStaticCacheSceneLoadResourcesKey,
        m_lastComputedStaticCacheSceneLoadResourcesKey);
}

void Editor::Panels::SceneView::CommitStaticFrameCacheKey(const uint64_t staticFrameCacheKey)
{
    AViewControllable::CommitStaticFrameCacheKey(staticFrameCacheKey);
    m_committedStaticCacheBaseKey = m_lastComputedStaticCacheBaseKey;
    m_committedStaticCacheHighlightKey = m_lastComputedStaticCacheHighlightKey;
    m_committedStaticCacheGizmoKey = m_lastComputedStaticCacheGizmoKey;
    m_committedStaticCacheFocusKey = m_lastComputedStaticCacheFocusKey;
    m_committedStaticCacheSelectionKey = m_lastComputedStaticCacheSelectionKey;
    m_committedStaticCacheSceneLoadResourcesKey = m_lastComputedStaticCacheSceneLoadResourcesKey;
}

bool Editor::Panels::SceneView::ShouldForceStaticFrameRender() const
{
    if (m_cameraMovedForPresentation || IsCameraControlActive())
        return true;
    if (m_activeDraggedPrefabPayload.has_value())
        return true;
    if (m_activeDraggedPrefabCommitPending)
        return true;
    if (ShouldForceSceneViewStaticFrameRenderForPendingClick(m_pendingClickPickRenderPos.has_value()))
        return true;
    if (ShouldForceSceneViewStaticFrameRenderForPicking(
        ShouldRequestPickingFrame(),
        m_hasPickingSample,
        EDITOR_CONTEXT(inputManager)->IsMouseButtonPressed(Windowing::Inputs::EMouseButton::MOUSE_BUTTON_LEFT) ||
            m_pendingClickPickRenderPos.has_value()))
    {
        return true;
    }
    if (NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::EditorActions>())
    {
        const auto& diagnostics = EDITOR_EXEC(GetContext()).GetDiagnosticsSettings();
        const bool validationReadbackRequested =
            !diagnostics.editorValidationSceneReadbackOutput.empty() ||
            !diagnostics.editorValidationSceneReadbackSummary.empty();
        if (ShouldForceSceneViewRenderForPendingSceneLoadResources(
                NLS::Editor::Core::GetPendingSceneLoadRendererResourceResolutionTaskCount(),
                validationReadbackRequested))
        {
            return true;
        }

        if (!m_validationReadbackWritten &&
            validationReadbackRequested &&
            NLS::Editor::Core::GetPendingSceneLoadRendererResourceResolutionTaskCount() == 0u)
        {
            return true;
        }
    }
    return false;
}

bool Editor::Panels::SceneView::ShouldDeferRenderFrame() const
{
    const size_t pendingSceneLoadResourceTasks =
        NLS::Editor::Core::GetPendingSceneLoadRendererResourceResolutionTaskCount();
    if (!ShouldDeferSceneViewRenderForPendingSceneLoadResources(pendingSceneLoadResourceTasks))
        return false;

    if (m_lastLoggedDeferredSceneLoadResourceTasks != pendingSceneLoadResourceTasks)
    {
        m_lastLoggedDeferredSceneLoadResourceTasks = pendingSceneLoadResourceTasks;
        NLS_LOG_INFO(
            "[Startup] SceneView deferred render while scene-load renderer resources are pending tasks=" +
            std::to_string(pendingSceneLoadResourceTasks));
    }
    return true;
}

bool Editor::Panels::SceneView::RequiresSynchronizedRetiredFramePresentation() const
{
    return ShouldSceneViewSynchronizeRetiredFramePresentation();
}

void Editor::Panels::SceneView::DrawPreRenderViewportOverlay()
{
    if (ShouldApplySceneMutationFromViewportOverlay(ViewportOverlayLifecyclePhase::BeforeViewRender))
        DrawViewportOverlay();
}

void Editor::Panels::SceneView::OnAfterDrawWidgets()
{
    if (ShouldResolveViewportPicking(ViewportOverlayLifecyclePhase::AfterWidgetDraw))
        HandleGameObjectPicking();
    EndViewportOverlayDrawListChannels();
    MarkViewportImageInputBoundsForLastDraw();
}

void Editor::Panels::SceneView::AfterRenderFrame()
{
    AViewControllable::AfterRenderFrame();
    TryWriteValidationReadback();
}

void Editor::Panels::SceneView::TryWriteValidationReadback()
{
    if (m_validationReadbackWritten)
        return;

    const auto& diagnostics = EDITOR_EXEC(GetContext()).GetDiagnosticsSettings();
    if (diagnostics.editorValidationSceneReadbackOutput.empty() &&
        diagnostics.editorValidationSceneReadbackSummary.empty())
    {
        return;
    }

    constexpr uint32_t kValidationReadbackWarmupFrames = 4u;
    if (m_validationReadbackWarmupFrames < kValidationReadbackWarmupFrames)
    {
        ++m_validationReadbackWarmupFrames;
        return;
    }

    const size_t pendingSceneLoadResourceTasks =
        NLS::Editor::Core::GetPendingSceneLoadRendererResourceResolutionTaskCount();
    const size_t visibleSceneLoadResourceObjects =
        NLS::Editor::Core::GetVisibleSceneLoadRendererResourceResolutionObjectCount();
    const bool activeSceneLoadResourceResolution =
        NLS::Editor::Core::HasActiveSceneLoadRendererResourceResolution();
    if (activeSceneLoadResourceResolution ||
        pendingSceneLoadResourceTasks > 0u ||
        visibleSceneLoadResourceObjects > 0u)
    {
        m_validationReadbackObservedSceneLoadResources = true;
    }
    if (ShouldWaitForSceneViewValidationReadbackSceneLoadResources(
            activeSceneLoadResourceResolution,
            pendingSceneLoadResourceTasks,
            visibleSceneLoadResourceObjects))
    {
        m_validationReadbackReadyFrames = 0u;
        m_validationReadbackSceneLoadReadyFrames = 0u;
        return;
    }
    if (ShouldWaitForSceneViewValidationReadbackAfterSceneLoadResources(
            m_validationReadbackObservedSceneLoadResources,
            m_validationReadbackSceneLoadReadyFrames))
    {
        ++m_validationReadbackSceneLoadReadyFrames;
        m_validationReadbackReadyFrames = 0u;
        return;
    }
    if (!diagnostics.editorValidationCreateAsset.empty())
    {
        auto* selectedGameObject = EDITOR_EXEC(GetSelectedGameObject());
        if (selectedGameObject == nullptr)
        {
            m_validationReadbackReadyFrames = 0u;
            return;
        }

        if (EDITOR_EXEC(GetContext()).importProgressTracker.HasRunningJobs())
        {
            m_validationReadbackReadyFrames = 0u;
            return;
        }

        constexpr uint32_t kStableFramesAfterAssetCreation = 4u;
        if (m_validationReadbackReadyFrames < kStableFramesAfterAssetCreation)
        {
            ++m_validationReadbackReadyFrames;
            return;
        }
    }

    const auto width = static_cast<uint32_t>(m_lastResolvedViewSize.first);
    const auto height = static_cast<uint32_t>(m_lastResolvedViewSize.second);
    if (width == 0u || height == 0u)
        return;

    auto* driver = Render::Context::TryGetLocatedDriver();
    if (driver == nullptr)
        return;

    auto texture = m_fbo.GetExplicitTextureHandle();
    if (texture == nullptr ||
        !Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(*driver, texture))
    {
        m_validationReadbackReadyFrames = 0u;
        return;
    }

    if (Render::Context::DriverRendererAccess::IsThreadedRenderingEnabled(*driver) &&
        !Render::Context::DriverRendererAccess::TryDrainThreadedRendering(*driver))
    {
        return;
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0u);
    const auto readback = Render::Context::DriverRendererAccess::ReadPixelsChecked(
        *driver,
        texture,
        0u,
        0u,
        width,
        height,
        Render::Settings::EPixelDataFormat::RGBA,
        Render::Settings::EPixelDataType::UNSIGNED_BYTE,
        pixels.data());

    if (!readback.Succeeded())
    {
        NLS_LOG_ERROR("Scene View validation readback failed: " + readback.message);
        m_validationReadbackWritten = true;
        NLS::Editor::Core::CancelSceneLoadRendererResourceResolution();
        EDITOR_CONTEXT(window)->SetShouldClose(true);
        return;
    }

    uint64_t nonBlackPixels = 0u;
    uint64_t nonZeroAlphaPixels = 0u;
    uint64_t rgbSum = 0u;
    uint8_t maxChannel = 0u;
    for (size_t index = 0u; index + 3u < pixels.size(); index += 4u)
    {
        const auto r = pixels[index + 0u];
        const auto g = pixels[index + 1u];
        const auto b = pixels[index + 2u];
        const auto a = pixels[index + 3u];
        if (r != 0u || g != 0u || b != 0u)
            ++nonBlackPixels;
        if (a != 0u)
            ++nonZeroAlphaPixels;
        rgbSum += static_cast<uint64_t>(r) + static_cast<uint64_t>(g) + static_cast<uint64_t>(b);
        maxChannel = std::max(maxChannel, std::max(r, std::max(g, b)));
    }

    const auto pixelCount = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    const double averageRgb = pixelCount > 0u
        ? static_cast<double>(rgbSum) / static_cast<double>(pixelCount * 3u)
        : 0.0;
    const size_t pendingSceneLoadTextureLoads =
        NLS::Editor::Core::GetPendingSceneLoadRendererResourceResolutionTextureLoadCount();
    const auto readbackStatus = BuildSceneViewValidationReadbackStatus(
        nonBlackPixels,
        static_cast<uint32_t>(maxChannel),
        pendingSceneLoadTextureLoads);

    std::string pngError;
    if (!diagnostics.editorValidationSceneReadbackOutput.empty())
    {
        std::error_code error;
        std::filesystem::create_directories(
            std::filesystem::path(diagnostics.editorValidationSceneReadbackOutput).parent_path(),
            error);
        if (!Render::Tooling::WriteMaterialVisualEvidencePng(
            diagnostics.editorValidationSceneReadbackOutput,
            pixels.data(),
            width,
            height,
            4u,
            width * 4u,
            &pngError))
        {
            NLS_LOG_ERROR("Scene View validation PNG write failed: " + pngError);
        }
    }

    std::ostringstream summary;
    summary << "width=" << width << "\n";
    summary << "height=" << height << "\n";
    summary << "pixels=" << pixelCount << "\n";
    summary << "nonBlackPixels=" << nonBlackPixels << "\n";
    summary << "nonZeroAlphaPixels=" << nonZeroAlphaPixels << "\n";
    summary << "averageRgb=" << averageRgb << "\n";
    summary << "maxChannel=" << static_cast<uint32_t>(maxChannel) << "\n";
    summary << "pendingSceneLoadTextureLoads=" << pendingSceneLoadTextureLoads << "\n";
    summary << "readbackStatus=" << readbackStatus << "\n";

    if (readbackStatus == "success")
        NLS_LOG_INFO("Scene View validation readback: " + summary.str());
    else
        NLS_LOG_ERROR("Scene View validation readback failed visual gate: " + summary.str());
    if (!diagnostics.editorValidationSceneReadbackSummary.empty())
    {
        std::error_code error;
        std::filesystem::create_directories(
            std::filesystem::path(diagnostics.editorValidationSceneReadbackSummary).parent_path(),
            error);
        std::ofstream output(diagnostics.editorValidationSceneReadbackSummary, std::ios::trunc);
        if (output)
            output << summary.str();
    }

    m_validationReadbackWritten = true;
    NLS::Editor::Core::CancelSceneLoadRendererResourceResolution();
    EDITOR_CONTEXT(window)->SetShouldClose(true);
}

void Editor::Panels::SceneView::DrawViewportOverlay()
{
    m_gizmoInteraction = {};
    if (Editor::Core::DoesShortcutSettingsWindowBlockSceneInput())
        return;

    const auto imageMin = HasViewportImageBounds()
        ? GetViewportImageMin()
        : GetCurrentViewportImageMin();
    const auto imageMax = HasViewportImageBounds()
        ? GetViewportImageMax()
        : GetCurrentViewportImageMax();
    const auto imageWidth = imageMax.x - imageMin.x;
    const auto imageHeight = imageMax.y - imageMin.y;
    if (imageWidth <= 0.0f || imageHeight <= 0.0f)
        return;

    const auto overlayMatrices = GetViewportOverlayCameraMatrices();
    auto viewMatrix = Core::ToImGuizmoMatrix(overlayMatrices.view);
    auto projectionMatrix = Core::ToImGuizmoMatrix(overlayMatrices.projection);
    const bool cameraControlActive = IsCameraControlActive();
    EnsureCameraFocus();

    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(imageMin.x, imageMin.y, imageWidth, imageHeight);

    const auto viewGizmoRect = Core::GetSceneViewViewGizmoRect(imageMin, imageMax);
    auto viewGizmoModelMatrix = Core::ToImGuizmoMatrix(Maths::Matrix4::Identity);
    ImGuizmo::ViewManipulate(
        viewMatrix.data(),
        projectionMatrix.data(),
        ToNativeImGuizmoOperation(m_currentOperation),
        ToNativeImGuizmoMode(m_currentSpace),
        viewGizmoModelMatrix.data(),
        kSceneViewGizmoCameraLength,
        ImVec2(viewGizmoRect.position.x, viewGizmoRect.position.y),
        ImVec2(viewGizmoRect.size.x, viewGizmoRect.size.y),
        IM_COL32(0, 0, 0, 0));

    m_gizmoInteraction.isViewHovered = ImGuizmo::IsViewManipulateHovered();
    m_gizmoInteraction.isViewUsing = ImGuizmo::IsUsingViewManipulate();
    if (Core::ShouldCancelViewGizmoCameraTransform(cameraControlActive, m_gizmoInteraction.isViewUsing))
    {
        ImGuizmo::CancelViewManipulate();
        m_gizmoInteraction.isViewUsing = false;
    }

    const ImGuiIO& io = ImGui::GetIO();
    const Maths::Vector2 mouseDelta {io.MouseDelta.x, io.MouseDelta.y};
    if (!cameraControlActive && Core::ShouldApplyViewGizmoCameraTransform(
        m_gizmoInteraction.isViewUsing,
        io.MouseDown[0],
        mouseDelta))
    {
        const auto cameraTransform = Core::GetCameraTransformFromViewMatrix(viewMatrix);
        float viewGizmoTargetDirection[3] {};
        const bool hasViewGizmoTargetDirection =
            ImGuizmo::GetViewManipulateTargetDirection(viewGizmoTargetDirection);
        const Maths::Vector3 targetForward {
            viewGizmoTargetDirection[0],
            viewGizmoTargetDirection[1],
            viewGizmoTargetDirection[2]
        };
        const Maths::Vector3 targetCameraForward =
            Core::GetCameraForwardFromImGuizmoViewTargetDirection(targetForward);
        const auto stableCameraTransform = Core::StabilizeViewGizmoCameraTransform(
            {m_camera.GetPosition(), m_camera.GetRotation()},
            cameraTransform,
            m_cameraFocus.focusDistance,
            m_cameraFocus.focusPoint,
            hasViewGizmoTargetDirection ? &targetCameraForward : nullptr);
        m_camera.SetPosition(stableCameraTransform.position);
        m_camera.SetRotation(stableCameraTransform.rotation);
        m_cameraFocus.focusDistance = Maths::Vector3::Distance(m_camera.GetPosition(), m_cameraFocus.focusPoint);
        m_cameraFocus.hasFocus = true;
        m_camera.CacheViewMatrix();
        m_cameraMovedForPresentation = true;
    }

    if (!EDITOR_EXEC(IsAnyGameObjectSelected()))
        return;

    auto* selectedGameObject = EDITOR_EXEC(GetSelectedGameObject());
    if (selectedGameObject == nullptr || selectedGameObject->GetTransform() == nullptr)
        return;

    auto modelMatrix = Core::GetGameObjectWorldGizmoMatrix(*selectedGameObject, m_currentPivot);

    using namespace Windowing::Inputs;
    const bool snapEnabled = Core::IsSnapModifierActive(
        EDITOR_CONTEXT(inputManager)->GetKeyState(EKey::KEY_LEFT_CONTROL),
        EDITOR_CONTEXT(inputManager)->GetKeyState(EKey::KEY_RIGHT_CONTROL));
    const float snapValue = Core::GetSnapValue(m_currentOperation);
    float snap[3] { snapValue, snapValue, snapValue };

    const bool manipulated = ImGuizmo::Manipulate(
        viewMatrix.data(),
        projectionMatrix.data(),
        ToNativeImGuizmoOperation(m_currentOperation),
        ToNativeImGuizmoMode(m_currentSpace),
        modelMatrix.data(),
        nullptr,
        snapEnabled ? snap : nullptr);

    m_gizmoInteraction.isHovered = !cameraControlActive && ImGuizmo::IsOver();
    m_gizmoInteraction.isUsing = !cameraControlActive && ImGuizmo::IsUsing();

    if (!cameraControlActive && manipulated)
    {
        Core::ApplyGameObjectWorldGizmoMatrix(*selectedGameObject, modelMatrix, m_currentOperation, m_currentPivot);
        EDITOR_EXEC(MarkOwningSceneDirty(*selectedGameObject));
    }
}

void Editor::Panels::SceneView::ApplyValidationCameraForwardStep(const float step)
{
    if (m_camera.transform == nullptr)
        return;

    m_validationCameraMotionActive = true;
    m_camera.SetPosition(
        m_camera.GetPosition() +
        m_camera.transform->GetWorldForward() * step);
    m_camera.CacheViewMatrix();
    m_cameraMovedForPresentation = true;
}

void Editor::Panels::SceneView::SetValidationCameraMotionActive(const bool active)
{
    m_validationCameraMotionActive = active;
}

bool Editor::Panels::SceneView::IsCameraControlActive() const
{
    return m_validationCameraMotionActive || m_cameraController.IsCameraControlActive();
}

void Editor::Panels::SceneView::EnsureCameraFocus()
{
    m_cameraFocus = Core::EnsureSceneCameraFocus(
        m_cameraFocus,
        m_camera.GetPosition(),
        m_camera.GetRotation() * Maths::Vector3::Forward,
        kSceneViewDefaultFocusDistance);
}

bool IsResizing()
{
    auto cursor = NLS_SERVICE(UI::UIManager).GetMouseCursor();

    return cursor == ImGuiMouseCursor_ResizeEW || cursor == ImGuiMouseCursor_ResizeNS || cursor == ImGuiMouseCursor_ResizeNWSE || cursor == ImGuiMouseCursor_ResizeNESW || cursor == ImGuiMouseCursor_ResizeAll;
}

bool Editor::Panels::SceneView::ShouldRequestPickingFrame() const
{
    using namespace Windowing::Inputs;
    if (Editor::Core::DoesShortcutSettingsWindowBlockSceneInput())
        return false;

    const bool pickingSuppressedByGizmo = Core::ShouldSuppressScenePicking(m_gizmoInteraction);
    if (pickingSuppressedByGizmo)
    {
        if (EDITOR_CONTEXT(inputManager)->IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT) ||
            m_pendingClickPickRenderPos.has_value())
        {
            std::ostringstream stream;
            stream << "request blocked by gizmo"
                << " leftPressed=" << EDITOR_CONTEXT(inputManager)->IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT)
                << " pendingClick=" << m_pendingClickPickRenderPos.has_value()
                << " hover=" << m_gizmoInteraction.isHovered
                << " using=" << m_gizmoInteraction.isUsing
                << " viewHover=" << m_gizmoInteraction.isViewHovered
                << " viewUsing=" << m_gizmoInteraction.isViewUsing;
            LogScenePickingDiagnostics(stream.str());
        }
        return false;
    }

    auto& inputManager = *EDITOR_CONTEXT(inputManager);
    const Maths::Vector2 screenMousePos = inputManager.GetMousePosition();
    const bool mouseOverView = IsMouseWithinView(screenMousePos);
    if (!mouseOverView || IsResizing())
    {
        if (inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT) ||
            m_pendingClickPickRenderPos.has_value())
        {
            std::ostringstream stream;
            stream << "request blocked by bounds"
                << " mouseOverView=" << mouseOverView
                << " resizing=" << IsResizing()
                << " mouse=(" << screenMousePos.x << "," << screenMousePos.y << ")"
                << " hasBounds=" << HasViewportImageBounds();
            LogScenePickingDiagnostics(stream.str());
        }
        return false;
    }

    const auto localMousePos = GetLocalViewPosition(screenMousePos);
    if (!localMousePos.has_value())
        return false;

    auto mousePos = localMousePos.value();
    const auto [safeWidth, safeHeight] = GetSafeSize();
    const float maxRenderX = std::max(0.0f, static_cast<float>(safeWidth) - 1.0f);
    const float maxRenderY = std::max(0.0f, static_cast<float>(safeHeight) - 1.0f);

    mousePos.x = std::clamp(mousePos.x, 0.0f, maxRenderX);
    mousePos.y = NLS_SERVICE(UI::UIManager).UsesBottomLeftRenderTargetOrigin()
        ? std::clamp(static_cast<float>(safeHeight) - 1.0f - mousePos.y, 0.0f, maxRenderY)
        : std::clamp(mousePos.y, 0.0f, maxRenderY);

    const auto now = std::chrono::steady_clock::now();
    const bool mouseMoved =
        std::fabs(mousePos.x - m_lastPickingMousePos.x) > 0.5f ||
        std::fabs(mousePos.y - m_lastPickingMousePos.y) > 0.5f;
    constexpr int kHoverPickingIntervalMs = 120;
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastPickingSampleTime).count();
    const bool sampleExpired =
        !m_hasPickingSample ||
        elapsedMs >= kHoverPickingIntervalMs;
    const bool leftClicked = inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT);
    const bool cameraControlActive = IsCameraControlActive();
    if (!ShouldRequestHitProxyPickingFrameWhileClickReadbackPending(
        leftClicked,
        m_pendingClickPickRenderPos.has_value(),
        m_pendingClickPickingSignature.has_value()))
    {
        return false;
    }
    if (ShouldCancelScenePickingWhileCameraControlIsActive(
        cameraControlActive,
        m_pendingClickPickRenderPos.has_value()))
    {
        LogScenePickingDiagnostics("request cancelled pending click while camera control is active");
        return false;
    }

    const bool request = ShouldRenderScenePickingFrame(
        mouseOverView,
        false,
        false,
        m_pendingClickPickRenderPos.has_value() && !m_pendingClickPickingSignature.has_value(),
        leftClicked,
        cameraControlActive,
        m_cameraMovedForPresentation,
        sampleExpired,
        mouseMoved,
        m_hasPickingSample);
    const bool clickPickingFrame = ShouldRequestHitProxyPickingFrameForClick(
        leftClicked,
        m_pendingClickPickRenderPos.has_value(),
        m_pendingClickPickingSignature.has_value());
    if (request && !clickPickingFrame)
    {
        const auto* sceneRenderer = dynamic_cast<const Engine::Rendering::BaseSceneRenderer*>(m_renderer.get());
        if (sceneRenderer != nullptr &&
            sceneRenderer->HasLastVisiblePickablePrimitiveDrawSources() &&
            ShouldSkipSceneHoverPickingForVisibleDrawBudget(
                false,
                static_cast<uint64_t>(sceneRenderer->GetLastVisiblePickablePrimitiveDrawSources().size()),
                kSceneViewHoverPickingVisibleDrawBudget))
        {
            NLS_PROFILE_NAMED_SCOPE("SceneView::PickingSkipped::HoverVisibleDrawBudget");
            return false;
        }
    }
    if (leftClicked || m_pendingClickPickRenderPos.has_value())
    {
        std::ostringstream stream;
        stream << "request"
            << " result=" << request
            << " leftClicked=" << leftClicked
            << " pendingClick=" << m_pendingClickPickRenderPos.has_value()
            << " cameraControl=" << cameraControlActive
            << " sampleExpired=" << sampleExpired
            << " mouseMoved=" << mouseMoved
            << " hasSample=" << m_hasPickingSample
            << " local=(" << mousePos.x << "," << mousePos.y << ")";
        LogScenePickingDiagnostics(stream.str());
    }
    return request;
}

void Editor::Panels::SceneView::HandleGameObjectPicking()
{
    if (Editor::Core::DoesShortcutSettingsWindowBlockSceneInput())
    {
        m_highlightedGameObject = nullptr;
        m_hasPickingSample = false;
        return;
    }

    auto* debugRenderer = dynamic_cast<Editor::Rendering::DebugSceneRenderer*>(m_renderer.get());
    if (debugRenderer == nullptr)
    {
        m_highlightedGameObject = nullptr;
        m_hasPickingSample = false;
        return;
    }

    using namespace Windowing::Inputs;

    auto& inputManager = *EDITOR_CONTEXT(inputManager);
    const Maths::Vector2 screenMousePos = inputManager.GetMousePosition();
    const bool mouseOverView = IsMouseWithinView(screenMousePos);

    if (inputManager.IsMouseButtonReleased(EMouseButton::MOUSE_BUTTON_LEFT))
    {
        m_gizmoInteraction.isUsing = false;
    }

    if (Core::ShouldSuppressScenePicking(m_gizmoInteraction))
    {
        if (inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT) ||
            m_pendingClickPickRenderPos.has_value())
        {
            std::ostringstream stream;
            stream << "handle blocked by gizmo"
                << " leftPressed=" << inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT)
                << " leftReleased=" << inputManager.IsMouseButtonReleased(EMouseButton::MOUSE_BUTTON_LEFT)
                << " pendingClick=" << m_pendingClickPickRenderPos.has_value()
                << " hover=" << m_gizmoInteraction.isHovered
                << " using=" << m_gizmoInteraction.isUsing
                << " viewHover=" << m_gizmoInteraction.isViewHovered
                << " viewUsing=" << m_gizmoInteraction.isViewUsing;
            LogScenePickingDiagnostics(stream.str());
        }
        return;
    }

    if (mouseOverView && !IsResizing())
    {
        auto& gameObjectPickingFeature = debugRenderer->GetPass<Rendering::PickingRenderPass>("Picking");
        if (!gameObjectPickingFeature.SupportsPickingReadback())
        {
            m_highlightedGameObject = nullptr;
            m_hasPickingSample = false;
            return;
        }

        const auto localMousePos = GetLocalViewPosition(screenMousePos);
        if (!localMousePos.has_value())
        {
            m_highlightedGameObject = nullptr;
            m_hasPickingSample = false;
            return;
        }

        auto mousePos = localMousePos.value();
        const auto [safeWidth, safeHeight] = GetSafeSize();
        const float maxRenderX = std::max(0.0f, static_cast<float>(safeWidth) - 1.0f);
        const float maxRenderY = std::max(0.0f, static_cast<float>(safeHeight) - 1.0f);

        mousePos.x = std::clamp(mousePos.x, 0.0f, maxRenderX);
        mousePos.y = NLS_SERVICE(UI::UIManager).UsesBottomLeftRenderTargetOrigin()
            ? std::clamp(static_cast<float>(safeHeight) - 1.0f - mousePos.y, 0.0f, maxRenderY)
            : std::clamp(mousePos.y, 0.0f, maxRenderY);

        const auto now = std::chrono::steady_clock::now();
        const bool mouseMoved =
            std::fabs(mousePos.x - m_lastPickingMousePos.x) > 0.5f ||
            std::fabs(mousePos.y - m_lastPickingMousePos.y) > 0.5f;
        constexpr int kHoverPickingIntervalMs = 120;
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastPickingSampleTime).count();
        const bool sampleExpired =
            !m_hasPickingSample ||
            elapsedMs >= kHoverPickingIntervalMs;
        const bool leftClicked = inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT);
        const bool cameraControlActive = IsCameraControlActive();
        if (ShouldCancelScenePickingWhileCameraControlIsActive(
            cameraControlActive,
            m_pendingClickPickRenderPos.has_value()))
        {
            LogScenePickingDiagnostics("handle cancelled pending click while camera control is active");
            m_pendingClickPickRenderPos.reset();
            m_pendingClickPickingSignature.reset();
            m_pendingClickMinReadablePickingFrameSerial = 0u;
        }
        const bool queuedClickPickThisFrame = leftClicked && !cameraControlActive;
        if (queuedClickPickThisFrame)
        {
            m_pendingClickPickRenderPos = mousePos;
            m_pendingClickMinReadablePickingFrameSerial = ComputePendingClickMinimumReadablePickingFrameSerial(
                gameObjectPickingFeature.GetSubmittedPickingFrameSerial(),
                m_requestPickingFrameForClick);
            m_pendingClickPickingSignature = m_requestPickingFrameForClick
                ? gameObjectPickingFeature.GetSubmittedPickingFrameSignature()
                : std::nullopt;
            std::ostringstream stream;
            stream << "queued click"
                << " local=(" << mousePos.x << "," << mousePos.y << ")"
                << " minReadableSerial=" << m_pendingClickMinReadablePickingFrameSerial
                << " currentReadableSerial=" << gameObjectPickingFeature.GetReadablePickingFrameSerial()
                << " requestThisFrame=" << m_requestPickingFrame;
            LogScenePickingDiagnostics(stream.str());
        }
        if (m_pendingClickPickRenderPos.has_value() &&
            !m_pendingClickPickingSignature.has_value() &&
            m_pendingClickMinReadablePickingFrameSerial > 0u &&
            gameObjectPickingFeature.GetSubmittedPickingFrameSerial() >= m_pendingClickMinReadablePickingFrameSerial)
        {
            m_pendingClickPickingSignature = gameObjectPickingFeature.GetSubmittedPickingFrameSignature();
        }

        const bool shouldRepick = ShouldRenderScenePickingFrame(
            mouseOverView,
            false,
            false,
            m_pendingClickPickRenderPos.has_value(),
            leftClicked,
            cameraControlActive,
            m_cameraMovedForPresentation,
            sampleExpired,
            mouseMoved,
            m_hasPickingSample);
        if (shouldRepick)
        {
            m_highlightedGameObject = nullptr;

            const bool pickingFrameReadable = gameObjectPickingFeature.HasReadablePickingFrame();
            const uint64_t readablePickingFrameSerial = gameObjectPickingFeature.GetReadablePickingFrameSerial();
            const bool resolvePendingClickPick =
                m_pendingClickPickRenderPos.has_value() &&
                !cameraControlActive &&
                m_pendingClickPickingSignature.has_value() &&
                gameObjectPickingFeature.CanResolvePickingRequest(
                    HitProxyPickingRequestKind::Click,
                    m_pendingClickMinReadablePickingFrameSerial,
                    m_pendingClickPickingSignature.value());
            if (queuedClickPickThisFrame || m_pendingClickPickRenderPos.has_value())
            {
                std::ostringstream stream;
                stream << "repick"
                    << " shouldRepick=" << shouldRepick
                    << " readable=" << pickingFrameReadable
                    << " readableSerial=" << readablePickingFrameSerial
                    << " minReadableSerial=" << m_pendingClickMinReadablePickingFrameSerial
                    << " hasSignature=" << m_pendingClickPickingSignature.has_value()
                    << " resolvePending=" << resolvePendingClickPick
                    << " requestThisFrame=" << m_requestPickingFrame;
                LogScenePickingDiagnostics(stream.str());
            }
            if (!cameraControlActive && pickingFrameReadable &&
                (!queuedClickPickThisFrame || resolvePendingClickPick))
            {
                const bool resolveClickPick = resolvePendingClickPick;
                if (resolveClickPick)
                {
                    NLS_PROFILE_NAMED_SCOPE("EditorPicking::ResolveClick");
                    const auto samplePos = m_pendingClickPickRenderPos.value();
                    m_highlightedGameObject =
                        PickGameObjectNearRenderCoordinate(gameObjectPickingFeature, samplePos, maxRenderX, maxRenderY);
                }
                else
                {
                    m_highlightedGameObject =
                        PickGameObjectAtRenderCoordinate(gameObjectPickingFeature, mousePos.x, mousePos.y);
                }
                if (m_highlightedGameObject != nullptr &&
                    m_activeDraggedPrefabRoot != nullptr &&
                    (m_highlightedGameObject == m_activeDraggedPrefabRoot ||
                        m_highlightedGameObject->IsDescendantOf(m_activeDraggedPrefabRoot) ||
                        m_highlightedGameObject->IsEditorTransient()))
                {
                    m_highlightedGameObject = nullptr;
                }
                if (resolveClickPick)
                {
                    std::ostringstream stream;
                    stream << "resolved click"
                        << " hit=" << (m_highlightedGameObject != nullptr)
                        << " actor=" << (m_highlightedGameObject != nullptr ? m_highlightedGameObject->GetName() : std::string("<none>"));
                    LogScenePickingDiagnostics(stream.str());
                }
            }
            else if (m_pendingClickPickRenderPos.has_value() && !pickingFrameReadable)
            {
                NLS_PROFILE_NAMED_SCOPE("EditorPicking::WaitReadback");
            }

            if (pickingFrameReadable)
            {
                m_lastPickingMousePos = mousePos;
                m_lastPickingSampleTime = now;
                m_hasPickingSample = true;
            }
        }
        else if (cameraControlActive)
        {
            m_highlightedGameObject = {};
        }

        const uint64_t readablePickingFrameSerial = gameObjectPickingFeature.GetReadablePickingFrameSerial();
        const bool resolvePendingClickPick =
            m_pendingClickPickRenderPos.has_value() &&
            !cameraControlActive &&
            m_pendingClickPickingSignature.has_value() &&
            gameObjectPickingFeature.CanResolvePickingRequest(
                HitProxyPickingRequestKind::Click,
                m_pendingClickMinReadablePickingFrameSerial,
                m_pendingClickPickingSignature.value());
        if (resolvePendingClickPick)
        {
            if (m_highlightedGameObject)
            {
                LogScenePickingDiagnostics("select GameObject=" + m_highlightedGameObject->GetName());
                EDITOR_EXEC(SelectGameObject(*m_highlightedGameObject));
                m_pendingClickPickRenderPos.reset();
                m_pendingClickPickingSignature.reset();
            }
            else if (gameObjectPickingFeature.HasReadablePickingFrame())
            {
                LogScenePickingDiagnostics("unselect GameObject from click");
                EDITOR_EXEC(UnselectGameObject());
                m_pendingClickPickRenderPos.reset();
                m_pendingClickPickingSignature.reset();
            }
            m_pendingClickMinReadablePickingFrameSerial = 0u;
        }
        else if (m_pendingClickPickRenderPos.has_value() &&
            m_pendingClickPickingSignature.has_value() &&
            m_pendingClickMinReadablePickingFrameSerial > 0u &&
            readablePickingFrameSerial >= m_pendingClickMinReadablePickingFrameSerial)
        {
            LogScenePickingDiagnostics("cancel pending click because readable picking signature changed");
            m_pendingClickPickRenderPos.reset();
            m_pendingClickPickingSignature.reset();
            m_pendingClickMinReadablePickingFrameSerial = 0u;
        }
    }
    else
    {
        if (inputManager.IsMouseButtonPressed(EMouseButton::MOUSE_BUTTON_LEFT) ||
            m_pendingClickPickRenderPos.has_value())
        {
            std::ostringstream stream;
            stream << "handle blocked by bounds"
                << " mouseOverView=" << mouseOverView
                << " resizing=" << IsResizing()
                << " mouse=(" << screenMousePos.x << "," << screenMousePos.y << ")"
                << " hasBounds=" << HasViewportImageBounds();
            LogScenePickingDiagnostics(stream.str());
        }
        m_highlightedGameObject = nullptr;
        m_hasPickingSample = false;
        m_pendingClickPickRenderPos.reset();
        m_pendingClickPickingSignature.reset();
        m_pendingClickMinReadablePickingFrameSerial = 0u;
    }
}

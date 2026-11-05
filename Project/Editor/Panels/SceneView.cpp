#include "ImGui/imgui.h"
#include "ImGui/imgui_internal.h"

#include "Rendering/DebugSceneRenderer.h"
#include "Rendering/GridRenderPass.h"
#include "Rendering/Assets/DistanceFieldArtifact.h"
#include "Rendering/PickingRenderPass.h"
#include "Rendering/SceneRendererFactory.h"
#include "Core/EditorActions.h"
#include "Core/PrefabInstanceResourceLifetime.h"
#include "Core/RecentBackgroundWorkGate.h"
#include "Assets/EditorAssetDragDropBridge.h"
#include "Assets/EditorAssetDragPayload.h"
#include "Assets/EditorAssetPathUtils.h"
#include "Assets/AssetImporterFacade.h"
#include "Project/Editor/Assets/DistanceFieldBuildService.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Engine/Assets/PrefabAsset.h"
#include "Panels/SceneView.h"
#include "Panels/GameView.h"
#include "Panels/HierarchyUndo.h"
#include "Panels/SceneViewPickingPolicy.h"
#include "Panels/ViewFrameLifecycle.h"
#include "Settings/EditorSettings.h"
#include "Profiling/Profiler.h"
#include "Core/EditorInteractionBlocker.h"
#include "Core/RendererResourceStreamingBudget.h"
#include "Core/SceneCameraFocus.h"
#include "Core/EditorGuiInput.h"
#include "Shortcuts/EditorInputActions.h"
#include "Inspector/EditorSelectionService.h"
#include "Core/SceneSelection.h"
#include <Components/CameraComponent.h>
#include <Components/LightComponent.h>
#include "Undo/EditorUndoMutationContext.h"
#include "Undo/EditorUndoService.h"
#include "Undo/SceneObjectUndoTarget.h"
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
#include <cstdlib>
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

namespace NLS::Editor::Panels
{
struct SceneViewGizmoEditState
{
    std::optional<Core::SceneViewGizmoMatrix> pendingModelMatrix;
    std::optional<Core::RectToolTransform> pendingLocalTransform;
    Core::EGizmoOperation operation = Core::EGizmoOperation::TRANSLATE;
    Core::SceneViewGizmoPivot pivot = Core::SceneViewGizmoPivot::Pivot;
    Core::SceneViewGizmoSpace space = Core::SceneViewGizmoSpace::Global;
    Core::SceneViewGizmoMatrix initialModelMatrix{};
    uint64_t selectionRevision = 0;
    bool multi = false;
};
}

namespace
{
constexpr float kSceneViewGizmoCameraLength = 8.0f;
constexpr float kSceneViewDefaultFocusDistance = 15.0f;
constexpr float kSceneViewPrefabDragFallbackDistance = 12.0f;
constexpr uint64_t kSceneViewHoverPickingVisibleDrawBudget = 1024u;
constexpr size_t kSceneViewImportedPrefabPreloadGateCapacity = 256u;
constexpr size_t kSceneViewValidationMinimumVisibleSceneLoadObjects = 32u;
constexpr auto kSceneViewImportedPrefabPreloadGateTtl = std::chrono::seconds(3);

using SceneViewTransformSnapshot = Editor::Core::RectToolTransform;

Editor::Undo::UndoScopeId BuildSceneViewUndoScope(const Engine::GameObject& gameObject);

struct SceneViewGizmoTarget
{
    Editor::Undo::SceneObjectUndoTarget object;
};

std::optional<SceneViewGizmoTarget> CaptureSceneViewGizmoTarget(
    Engine::GameObject& gameObject,
    const Editor::Undo::UndoScopeId& scope)
{
    auto object = Editor::Undo::SceneObjectUndoTarget::Capture(gameObject, scope);
    if (!object.has_value())
        return std::nullopt;

    return SceneViewGizmoTarget {std::move(*object)};
}

Engine::GameObject* ResolveSceneViewGizmoTarget(const SceneViewGizmoTarget& target)
{
    if (!NLS::Core::ServiceLocator::Contains<Editor::Core::EditorActions>())
        return nullptr;

    auto& actions = NLS::Core::ServiceLocator::Get<Editor::Core::EditorActions>();
    auto* gameObject = target.object.Resolve();
    if (gameObject == nullptr || gameObject->GetScene() == nullptr)
        return nullptr;

    const auto& context = actions.GetContext();
    const bool belongsToActivePrefabStage = context.activePrefabStage.has_value() &&
        context.activePrefabStage->stageScene.get() == gameObject->GetScene();
    const bool belongsToCurrentScene = context.sceneManager.GetCurrentScene() == gameObject->GetScene();
    if (!belongsToActivePrefabStage && !belongsToCurrentScene)
        return nullptr;

    return BuildSceneViewUndoScope(*gameObject) == target.object.GetScope()
        ? gameObject
        : nullptr;
}

std::optional<SceneViewTransformSnapshot> ReadSceneViewTransform(const Engine::GameObject& gameObject)
{
    if (gameObject.GetTransform() == nullptr)
        return std::nullopt;

    const auto* transform = gameObject.GetTransform();
    return SceneViewTransformSnapshot {
        transform->GetLocalPosition(),
        transform->GetLocalRotation(),
        transform->GetLocalScale()};
}

std::optional<SceneViewTransformSnapshot> ReadSceneViewTransform(const SceneViewGizmoTarget& target)
{
    const auto* gameObject = ResolveSceneViewGizmoTarget(target);
    return gameObject != nullptr ? ReadSceneViewTransform(*gameObject) : std::nullopt;
}

bool ApplySceneViewTransform(const SceneViewGizmoTarget& target, const SceneViewTransformSnapshot& snapshot)
{
    NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT("SceneView::ApplySceneViewTransform");
    auto* gameObject = ResolveSceneViewGizmoTarget(target);
    if (gameObject == nullptr || gameObject->GetTransform() == nullptr)
        return false;

    gameObject->GetTransform()->SetLocalTransform(
        snapshot.localPosition,
        snapshot.localRotation,
        snapshot.localScale);
    if (NLS::Core::ServiceLocator::Contains<Editor::Core::EditorActions>())
        EDITOR_EXEC(MarkOwningSceneDirty(*gameObject));
    return true;
}

class SceneViewTransformUndoCommand final : public Editor::Undo::IEditorUndoCommand
{
public:
    SceneViewTransformUndoCommand(
        Editor::Undo::UndoCommandInfo info,
        SceneViewGizmoTarget target,
        const SceneViewTransformSnapshot before,
        const SceneViewTransformSnapshot after)
        : m_info(std::move(info))
        , m_target(std::move(target))
        , m_before(before)
        , m_after(after)
    {
    }

    [[nodiscard]] Editor::Undo::UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    [[nodiscard]] Editor::Undo::UndoValidationResult Validate(
        const Editor::Undo::UndoDirection,
        const Editor::Undo::UndoExecutionContext&) const override
    {
        return ReadSceneViewTransform(m_target).has_value()
            ? Editor::Undo::UndoValidationResult::Ready()
            : Editor::Undo::UndoValidationResult::Stale("The transformed GameObject is no longer valid in this Scene.");
    }

    Editor::Undo::UndoStartResult Apply(
        const Editor::Undo::UndoDirection direction,
        const Editor::Undo::UndoExecutionContext&,
        Editor::Undo::UndoCompletion) override
    {
        return ApplySceneViewTransform(m_target, direction == Editor::Undo::UndoDirection::Undo ? m_before : m_after)
            ? Editor::Undo::UndoStartResult::Completed()
            : Editor::Undo::UndoStartResult::Failed("The transformed GameObject is no longer valid in this Scene.");
    }

    bool TryMergeFrom(const Editor::Undo::IEditorUndoCommand&) override
    {
        return false;
    }

private:
    Editor::Undo::UndoCommandInfo m_info;
    SceneViewGizmoTarget m_target;
    SceneViewTransformSnapshot m_before;
    SceneViewTransformSnapshot m_after;
};

class SceneViewTransformEdit final : public Editor::Undo::IEditorUndoEdit
{
public:
    SceneViewTransformEdit(
        Editor::Undo::UndoCommandInfo info,
        SceneViewGizmoTarget target,
        const SceneViewTransformSnapshot before,
        std::shared_ptr<Editor::Panels::SceneViewGizmoEditState> state)
        : m_info(std::move(info))
        , m_target(std::move(target))
        , m_before(before)
        , m_state(std::move(state))
    {
    }

    [[nodiscard]] Editor::Undo::UndoValidationResult Validate(
        const Editor::Undo::UndoExecutionContext&) const override
    {
        return ReadSceneViewTransform(m_target).has_value()
            ? Editor::Undo::UndoValidationResult::Ready()
            : Editor::Undo::UndoValidationResult::Stale("The transformed GameObject is no longer valid in this Scene.");
    }

    Editor::Undo::UndoStartResult Preview(
        const Editor::Undo::UndoExecutionContext&,
        Editor::Undo::UndoCompletion) override
    {
        auto* gameObject = ResolveSceneViewGizmoTarget(m_target);
        if (gameObject == nullptr || gameObject->GetTransform() == nullptr)
            return Editor::Undo::UndoStartResult::Failed("The transformed GameObject is no longer valid in this Scene.");
        if (!m_state || (!m_state->pendingModelMatrix.has_value() && !m_state->pendingLocalTransform.has_value()))
            return Editor::Undo::UndoStartResult::Failed("The Scene View gizmo has no pending transform.");
        if (m_state->pendingModelMatrix && !std::all_of(m_state->pendingModelMatrix->begin(), m_state->pendingModelMatrix->end(),
            [](const float value) { return std::isfinite(value); }))
            return Editor::Undo::UndoStartResult::Failed("The Scene View gizmo produced an invalid transform.");

        // PreviewEditSession establishes the mutation context before this runs.
        // Both gizmos only calculate a candidate; the edit owns the actual write.
        if (m_state->pendingLocalTransform)
        {
            if (!ApplySceneViewTransform(m_target, *m_state->pendingLocalTransform))
                return Editor::Undo::UndoStartResult::Failed("The Rect tool target is no longer valid.");
        }
        else
            NLS::Editor::Core::ApplyGameObjectWorldGizmoMatrix(
                *gameObject,
                *m_state->pendingModelMatrix,
                m_state->operation,
                m_state->pivot);
        const auto current = ReadSceneViewTransform(*gameObject);
        if (!current.has_value())
            return Editor::Undo::UndoStartResult::Failed("The transformed GameObject is no longer valid in this Scene.");
        m_after = *current;
        return Editor::Undo::UndoStartResult::Completed();
    }

    Editor::Undo::UndoStartResult Cancel(
        const Editor::Undo::UndoExecutionContext&,
        Editor::Undo::UndoCompletion) override
    {
        return ApplySceneViewTransform(m_target, m_before)
            ? Editor::Undo::UndoStartResult::Completed()
            : Editor::Undo::UndoStartResult::Failed("The transformed GameObject is no longer valid in this Scene.");
    }

    [[nodiscard]] bool HasChanges() const override
    {
        return m_after.has_value() && *m_after != m_before;
    }

    [[nodiscard]] Editor::Undo::UndoCommandPtr BuildCommand() override
    {
        if (!HasChanges())
            return {};
        return std::make_unique<SceneViewTransformUndoCommand>(m_info, m_target, m_before, *m_after);
    }

private:
    Editor::Undo::UndoCommandInfo m_info;
    SceneViewGizmoTarget m_target;
    SceneViewTransformSnapshot m_before;
    std::optional<SceneViewTransformSnapshot> m_after;
    std::shared_ptr<Editor::Panels::SceneViewGizmoEditState> m_state;
};

struct SceneViewGroupTransformEntry
{
    SceneViewGizmoTarget target;
    SceneViewTransformSnapshot before, after;
    Editor::Core::SceneSelectionTransform worldBefore;
};

Editor::Undo::UndoValidationResult ValidateGroupTransforms(const std::vector<SceneViewGroupTransformEntry>& entries)
{
    for (const auto& entry : entries)
        if (!ReadSceneViewTransform(entry.target))
            return Editor::Undo::UndoValidationResult::Stale("A transformed object no longer belongs to the working Scene.");
    return Editor::Undo::UndoValidationResult::Ready();
}

class SceneViewGroupTransformCommand final : public Editor::Undo::IEditorUndoCommand
{
public:
    SceneViewGroupTransformCommand(Editor::Undo::UndoCommandInfo info, std::vector<SceneViewGroupTransformEntry> entries)
        : m_info(std::move(info)), m_entries(std::move(entries)) {}
    Editor::Undo::UndoCommandInfo GetInfo() const override { return m_info; }
    Editor::Undo::UndoValidationResult Validate(Editor::Undo::UndoDirection, const Editor::Undo::UndoExecutionContext&) const override
    { return ValidateGroupTransforms(m_entries); }
    Editor::Undo::UndoStartResult Apply(Editor::Undo::UndoDirection direction, const Editor::Undo::UndoExecutionContext&, Editor::Undo::UndoCompletion) override
    {
        if (ValidateGroupTransforms(m_entries).state != Editor::Undo::UndoValidationState::Ready)
            return Editor::Undo::UndoStartResult::Failed("A transformed object no longer exists.");
        size_t applied = 0;
        for (; applied < m_entries.size(); ++applied)
        {
            const auto& entry = m_entries[applied];
            if (!ApplySceneViewTransform(entry.target, direction == Editor::Undo::UndoDirection::Undo ? entry.before : entry.after))
            {
                bool rollbackFailed = false;
                while (applied > 0)
                {
                    const auto& previous = m_entries[--applied];
                    rollbackFailed |= !ApplySceneViewTransform(previous.target,
                        direction == Editor::Undo::UndoDirection::Undo ? previous.after : previous.before);
                }
                return Editor::Undo::UndoStartResult::Failed("The group transform could not be applied.", rollbackFailed);
            }
        }
        return Editor::Undo::UndoStartResult::Completed();
    }
    bool TryMergeFrom(const Editor::Undo::IEditorUndoCommand&) override { return false; }
private:
    Editor::Undo::UndoCommandInfo m_info;
    std::vector<SceneViewGroupTransformEntry> m_entries;
};

class SceneViewGroupTransformEdit final : public Editor::Undo::IEditorUndoEdit
{
public:
    SceneViewGroupTransformEdit(Editor::Undo::UndoCommandInfo info, std::vector<SceneViewGroupTransformEntry> entries,
        std::shared_ptr<Editor::Panels::SceneViewGizmoEditState> state)
        : m_info(std::move(info)), m_entries(std::move(entries)), m_state(std::move(state)) {}
    Editor::Undo::UndoValidationResult Validate(const Editor::Undo::UndoExecutionContext&) const override
    { return ValidateGroupTransforms(m_entries); }
    Editor::Undo::UndoStartResult Preview(const Editor::Undo::UndoExecutionContext&, Editor::Undo::UndoCompletion) override
    {
        if (!m_state || !m_state->pendingModelMatrix || ValidateGroupTransforms(m_entries).state != Editor::Undo::UndoValidationState::Ready)
            return Editor::Undo::UndoStartResult::Failed("The group transform is no longer available.");
        std::vector<Editor::Core::SceneSelectionTransform> candidates;
        for (const auto& entry : m_entries)
        {
            auto candidate = Editor::Core::TransformSceneSelection(entry.worldBefore,
                Editor::Core::FromImGuizmoMatrix(m_state->initialModelMatrix),
                Editor::Core::FromImGuizmoMatrix(*m_state->pendingModelMatrix),
                m_state->operation, m_state->pivot, m_state->space);
            for (const float value : {candidate.worldPosition.x, candidate.worldPosition.y, candidate.worldPosition.z,
                candidate.localScale.x, candidate.localScale.y, candidate.localScale.z,
                candidate.worldRotation.x, candidate.worldRotation.y, candidate.worldRotation.z, candidate.worldRotation.w})
                if (!std::isfinite(value)) return Editor::Undo::UndoStartResult::Failed("The group transform is not finite.");
            candidates.push_back(candidate);
        }
        for (size_t index = 0; index < m_entries.size(); ++index)
        {
            auto& entry = m_entries[index];
            auto* object = ResolveSceneViewGizmoTarget(entry.target);
            if (!object) return Editor::Undo::UndoStartResult::Failed("A transformed object no longer exists.");
            auto& transform = *object->GetTransform();
            transform.SetWorldPosition(candidates[index].worldPosition);
            transform.SetWorldRotation(candidates[index].worldRotation);
            transform.SetLocalScale(candidates[index].localScale);
            entry.after = *ReadSceneViewTransform(*object);
            EDITOR_EXEC(MarkOwningSceneDirty(*object));
        }
        return Editor::Undo::UndoStartResult::Completed();
    }
    Editor::Undo::UndoStartResult Cancel(const Editor::Undo::UndoExecutionContext&, Editor::Undo::UndoCompletion) override
    {
        for (const auto& entry : m_entries)
            if (ReadSceneViewTransform(entry.target)) static_cast<void>(ApplySceneViewTransform(entry.target, entry.before));
        return Editor::Undo::UndoStartResult::Completed();
    }
    bool HasChanges() const override
    { return std::any_of(m_entries.begin(), m_entries.end(), [](const auto& entry) { return entry.before != entry.after; }); }
    Editor::Undo::UndoCommandPtr BuildCommand() override
    {
        return HasChanges() ? std::make_unique<SceneViewGroupTransformCommand>(m_info, m_entries) : nullptr;
    }
private:
    Editor::Undo::UndoCommandInfo m_info;
    std::vector<SceneViewGroupTransformEntry> m_entries;
    std::shared_ptr<Editor::Panels::SceneViewGizmoEditState> m_state;
};

Editor::Undo::UndoScopeId BuildSceneViewUndoScope(const Engine::SceneSystem::Scene& scene)
{
    if (!NLS::Core::ServiceLocator::Contains<Editor::Core::EditorActions>())
        return {};
    return NLS::Core::ServiceLocator::Get<Editor::Core::EditorActions>().GetUndoScopeForScene(scene);
}

Editor::Undo::UndoScopeId BuildSceneViewUndoScope(const Engine::GameObject& gameObject)
{
    const auto* scene = gameObject.GetScene();
    return scene != nullptr ? BuildSceneViewUndoScope(*scene) : Editor::Undo::UndoScopeId {};
}

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
    if (IsSceneRendererAvailable())
    {
        // Create the overlay renderer during startup while the native progress window is visible.
        NLS_LOG_INFO("[Startup] Creating Scene View renderer");
        const auto rendererConstructionBegin = std::chrono::steady_clock::now();
        m_renderer = std::make_unique<Editor::Rendering::DebugSceneRenderer>(*EDITOR_CONTEXT(driver));
        NLS_LOG_INFO(
            "[Startup] Scene View renderer construction elapsedMs=" +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - rendererConstructionBegin).count()));
        SetRequiresRetiredFrameConsumption(true);
        SetRequiresImmediateRetiredFrameReadback(ShouldSceneViewRequestImmediatePickingReadback());
    }
    else
    {
        NLS_LOG_INFO(
            "[Startup] Generic Scene View renderer skipped; native Metal viewport preview will be used.");
    }

    m_camera.SetFar(5000.0f);
    if (EDITOR_CONTEXT(projectSettings).GetOrDefault<bool>("scene_view_2d", false)) Set2D(true);
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
    CancelSceneSelectionGesture(false);
    if (m_toolbarSettingsDirty && NLS::Core::ServiceLocator::Contains<Editor::Core::EditorActions>())
        SaveSceneToolbarSettings();
    CancelGizmoEditSession();
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
        if (const auto spritePayload = NLS::Editor::Assets::ResolveSpriteDragPayload(payload))
        {
            CancelActivePrefabDragInstance();
            if (payloadView.delivered && GetScene() && NLS::Core::ServiceLocator::Contains<Editor::Undo::EditorUndoService>())
            {
                const auto result = HierarchyUndo::CreateGameObjectFromAsset(
                    NLS::Core::ServiceLocator::Get<Editor::Undo::EditorUndoService>(), *spritePayload, *GetScene(), nullptr,
                    BuildSceneViewUndoScope(*GetScene()), ResolveActivePrefabDragPlacement(EDITOR_CONTEXT(inputManager)->GetMousePosition()));
                if (!result.Succeeded()) NLS_LOG_WARNING("Sprite drop: " + result.message);
            }
            UI::EndDragDropTarget();
            return;
        }
        UpdateActivePrefabDragInstance(payload);
        if (payloadView.delivered)
        {
            m_activeDraggedPrefabDropSceneToken = CaptureActivePrefabDragSceneToken();
            m_activeDraggedPrefabDropPlacement = ResolveActivePrefabDragPlacement(
                EDITOR_CONTEXT(inputManager)->GetMousePosition());
            m_activeDraggedPrefabCommitPending = true;
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
            auto* scene = GetScene();
            if (scene == nullptr)
            {
                NLS_LOG_WARNING("Skipped built-in asset drop because there is no active Scene.");
                break;
            }
            if (!NLS::Core::ServiceLocator::Contains<Editor::Undo::EditorUndoService>())
            {
                NLS_LOG_WARNING("Skipped built-in asset drop because the global Undo service is unavailable.");
                break;
            }

            const auto result = HierarchyUndo::CreateGameObjectFromAsset(
                NLS::Core::ServiceLocator::Get<Editor::Undo::EditorUndoService>(),
                path,
                *scene,
                nullptr,
                BuildSceneViewUndoScope(*scene));
            if (!result.Succeeded())
            {
                NLS_LOG_WARNING(
                    "Scene View built-in asset drop was not applied through Undo: " +
                    (result.message.empty() ? std::string("Undo service rejected the command.") : result.message));
            }
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
        NLS::Editor::Assets::EditorAssetDragDropBridgeResult result;
        {
            // The drag proxy is live scene state, but it is intentionally not
            // a durable history entry. Keep its creation inside an explicit
            // transient mutation scope so debug builds still detect any
            // editor-side write that bypasses the mutation gateway.
            const NLS::Editor::Undo::ScopedEditorUndoTransientMutationContext transientMutationContext(
                BuildSceneViewUndoScope(*scene));
            result = bridge.TryDropImportedAssetHandleFromHotCacheIntoHierarchy(
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
        }
        if (!result.handled ||
            result.dragDrop.status != NLS::Editor::Assets::DragDropOperationStatus::Committed ||
            !result.dragDrop.instance.has_value() ||
            result.dragDrop.instance->instanceRoot == nullptr)
        {
            return;
        }

        m_activeDraggedPrefabRoot = result.dragDrop.instance->instanceRoot;
        m_activeDraggedPrefabRootSceneToken = CaptureActivePrefabDragSceneToken();
        {
            const NLS::Editor::Undo::ScopedEditorUndoTransientMutationContext transientMutationContext(
                BuildSceneViewUndoScope(*m_activeDraggedPrefabRoot));
            m_activeDraggedPrefabRoot->SetEditorTransient(true);
        }
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
        const NLS::Editor::Undo::ScopedEditorUndoTransientMutationContext transientMutationContext(
            BuildSceneViewUndoScope(*m_activeDraggedPrefabRoot));
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
#if defined(NLS_ENABLE_TEST_HOOKS)
    if (!m_forceGraphOnlyPrefabDragForTesting)
#endif
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
    const auto transientScope = root != nullptr
        ? BuildSceneViewUndoScope(*root)
        : Editor::Undo::UndoScopeId {};
    ClearActivePrefabDragState();
    if (root == nullptr)
        return;

    const NLS::Editor::Undo::ScopedEditorUndoTransientMutationContext transientMutationContext(transientScope);
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
    if (m_activeDraggedPrefabRootAwaitingRendererResources)
        return false;
    if (m_activeDraggedPrefabHotCacheKey.has_value())
    {
        NLS::Editor::Assets::EditorAssetDragDropBridge bridge(EDITOR_CONTEXT(projectAssetsPath));
        if (!m_activeDraggedPrefabHotCacheKey->rendererArtifactReadinessRequired)
        {
            const auto rendererReadyKey =
#if defined(NLS_ENABLE_TEST_HOOKS)
                m_forceGraphOnlyPrefabDragForTesting ? std::optional<NLS::Editor::Assets::UnifiedPrefabLoadKey> {} :
#endif
                bridge.TryFindImportedPrefabHotCacheKey(
                m_activeDraggedPrefabAssetPath,
                m_activeDraggedPrefabSubAssetKey,
                m_activeDraggedPrefabAssetId,
                m_activeDraggedPrefabAssetType,
                NLS::Editor::Assets::UnifiedPrefabReadiness::MeshMaterialTextureReady);
            if (!rendererReadyKey.has_value())
            {
                // The visible graph preview can already use resources owned by
                // another scene instance while the stricter disk cache is
                // still being prepared. Delivery must keep that preview alive.
                return false;
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
            m_activeDraggedPrefabHotCacheKey = rendererReadyKey;
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
    const auto payload = m_activeDraggedPrefabPayload;
    const auto scope = BuildSceneViewUndoScope(*root);
    auto* scene = root->GetScene();
    CancelActivePrefabDragInstance();

    if (!payload.has_value() || scene == nullptr)
        return false;
    if (!NLS::Core::ServiceLocator::Contains<Editor::Undo::EditorUndoService>())
    {
        NLS_LOG_WARNING("Scene View prefab drop was cancelled because the global Undo service is unavailable.");
        return false;
    }

    auto& undoService = NLS::Core::ServiceLocator::Get<Editor::Undo::EditorUndoService>();
    const auto result = HierarchyUndo::CreateGameObjectFromAsset(
        undoService,
        *payload,
        *scene,
        nullptr,
        scope,
        placement);
    if (!result.Succeeded())
    {
        NLS_LOG_WARNING(
            "Scene View prefab drop was not applied through Undo: " +
            (result.message.empty() ? std::string("Undo service rejected the command.") : result.message));
        return false;
    }

    EDITOR_PANEL(Editor::Panels::Hierarchy, "Hierarchy").RebuildFromCurrentScene();
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

    const auto ray = Core::BuildSceneViewPlacementRay(m_camera, *localPosition,
        {static_cast<float>(safeWidth), static_cast<float>(safeHeight)});
    const auto normal = m_is2D ? Maths::Vector3::Forward : Maths::Vector3::Up;
    const float denominator = Maths::Vector3::Dot(ray.direction, normal);
    if (std::fabs(denominator) > Maths::SMALL_NUMBER)
    {
        const float distanceToGround = -Maths::Vector3::Dot(ray.origin, normal) / denominator;
        if (distanceToGround > 0.0f && distanceToGround < m_camera.GetFar())
            return ray.origin + ray.direction * distanceToGround;
    }

    const float fallbackDistance = m_cameraFocus.hasFocus
        ? std::max(1.0f, m_cameraFocus.focusDistance)
        : kSceneViewPrefabDragFallbackDistance;
    return ray.origin + ray.direction * fallbackDistance;
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
Editor::Panels::SceneView::PrefabDragDeliveryValidation
Editor::Panels::SceneView::ValidatePrefabDragDeliveryForTesting(
    const NLS::Editor::Assets::EditorAssetDragPayload& payload,
    const Maths::Vector3& placement)
{
    PrefabDragDeliveryValidation validation;
    CancelActivePrefabDragInstance();
    const auto previousPlacement = m_activePrefabDragPlacementOverrideForTesting;
    m_activePrefabDragPlacementOverrideForTesting = placement;
    m_disableActivePrefabDragPreloadForTesting = true;
    m_forceGraphOnlyPrefabDragForTesting = true;
    NLS::Editor::Assets::EditorAssetDragDropBridge bridge(EDITOR_CONTEXT(projectAssetsPath));
    auto request = NLS::Editor::Assets::MakeSceneRestoreUnifiedPrefabLoadRequest(
        NLS::Editor::Assets::NormalizePrefabSourceIdentity(
            EDITOR_CONTEXT(projectPath),
            NLS::Editor::Assets::GetEditorAssetDragPayloadPath(payload),
            NLS::Editor::Assets::GetEditorAssetDragPayloadSubAssetKey(payload),
            NLS::Editor::Assets::GetEditorAssetDragPayloadAssetId(payload)),
        "prefab-drag-delivery-validation");
    request.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::PrefabGraphOnly;
    (void)bridge.PreloadPreparedPrefabHotCache(request);

    UI::DragDropTargetPayloadForTesting targetPayload;
    targetPayload.type = NLS::Editor::Assets::kEditorAssetDragPayloadType;
    targetPayload.bytes.resize(sizeof(payload));
    std::memcpy(targetPayload.bytes.data(), &payload, sizeof(payload));
    targetPayload.targetActive = true;
    UI::SetDragDropTargetPayloadForTesting(targetPayload);
    HandleViewportAssetDragDrop();
    auto* previewRoot = m_activeDraggedPrefabRoot;
    targetPayload.delivered = true;
    UI::SetDragDropTargetPayloadForTesting(targetPayload);
    HandleViewportAssetDragDrop();
    validation.previewRetainedAfterDelivery = previewRoot != nullptr &&
        m_activeDraggedPrefabRoot == previewRoot && m_activeDraggedPrefabCommitPending;
    targetPayload.targetActive = false;
    UI::SetDragDropTargetPayloadForTesting(targetPayload);
    HandleViewportAssetDragDrop();
    validation.previewRetainedOutsideTarget = validation.previewRetainedAfterDelivery &&
        m_activeDraggedPrefabRoot == previewRoot && m_activeDraggedPrefabCommitPending;

    m_forceGraphOnlyPrefabDragForTesting = false;
    request.requiredReadiness = NLS::Editor::Assets::UnifiedPrefabReadiness::MeshMaterialTextureReady;
    (void)bridge.PreloadPreparedPrefabHotCache(request);
    if (m_activeDraggedPrefabPayload.has_value())
    {
        UpdateActivePrefabDragInstance(payload);
        validation.committed = CommitActivePrefabDragInstance();
    }
    UI::ClearDragDropTargetPayloadForTesting();
    CancelActivePrefabDragInstance();
    m_disableActivePrefabDragPreloadForTesting = false;
    m_activePrefabDragPlacementOverrideForTesting = previousPlacement;
    return validation;
}
#endif

void Editor::Panels::SceneView::EnsureRenderer()
{
    if (m_renderer != nullptr)
        return;
    if (!IsSceneRendererAvailable())
        return;

    NLS_LOG_INFO("[Startup] Creating Scene View renderer");
    m_renderer = std::make_unique<Editor::Rendering::DebugSceneRenderer>(*EDITOR_CONTEXT(driver));
}

void Editor::Panels::SceneView::ApplyViewSettings()
{
    const auto& viewSettings = Settings::EditorSettings::GetViewSettingsObject();
    const auto& sceneSettings = Settings::EditorSettings::GetSceneViewSettingsObject();
    m_cameraController.SetNavigationOptions(sceneSettings.cameraEasing, sceneSettings.cameraAcceleration,
        sceneSettings.cameraMinSpeed, sceneSettings.cameraMaxSpeed);
    m_cameraController.SetSpeed(viewSettings.sceneViewCameraSpeed);
    m_camera.SetFov(std::clamp(sceneSettings.cameraFov, 1.0f, 179.0f));
    if (sceneSettings.cameraDynamicClipping)
    {
        const float extent = std::max({1.0f, m_cameraFocus.focusDistance,
            m_camera.GetProjectionMode() == Render::Settings::EProjectionMode::ORTHOGRAPHIC ? m_camera.GetSize() : 0.0f});
        m_camera.SetFar(std::max(1000.0f, extent * 100.0f));
        m_camera.SetNear(std::max(0.001f, m_camera.GetFar() / 100000.0f));
    }
    else
    {
        m_camera.SetNear(std::max(0.001f, sceneSettings.cameraNear));
        m_camera.SetFar(std::max(m_camera.GetNear() + 0.01f, sceneSettings.cameraFar));
    }
    m_gridColor = Maths::Vector3(
        viewSettings.sceneViewGrid.r,
        viewSettings.sceneViewGrid.g,
        viewSettings.sceneViewGrid.b);
    m_camera.SetClearColor(Maths::Vector3(
        viewSettings.sceneViewBackground.r,
        viewSettings.sceneViewBackground.g,
        viewSettings.sceneViewBackground.b));
}

void Editor::Panels::SceneView::UpdateCameraInputContext()
{
    using namespace Windowing::Inputs;
    const Maths::Vector2 mousePosition = EDITOR_CONTEXT(inputManager)->GetMousePosition();
    const bool shortcutsWindowOpen = Editor::Core::DoesShortcutSettingsWindowBlockSceneInput() || IsToolbarInteractionActive();
    const bool viewportInputAvailable = HasViewportImageInputBounds();
    auto& input = *EDITOR_CONTEXT(inputManager);
    const auto& gui = *ImGui::GetCurrentContext();
    const bool navigationPress = Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::CameraFly).pressed ||
        Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::CameraPan).pressed ||
        Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::CameraOrbit).pressed;
    const bool pointerOwnerHeld = gui.ActiveId != 0 &&
        (input.GetMouseButtonState(EMouseButton::MOUSE_BUTTON_LEFT) == EMouseButtonState::MOUSE_DOWN ||
         (gui.ActiveIdSource == ImGuiInputSource_Mouse && gui.ActiveIdMouseButton >= 0 && gui.ActiveIdMouseButton < 8 &&
          input.GetMouseButtonState(static_cast<EMouseButton>(gui.ActiveIdMouseButton)) == EMouseButtonState::MOUSE_DOWN));
    if (!shortcutsWindowOpen && viewportInputAvailable && IsMouseWithinView(mousePosition))
        Core::TryFocusViewportForNavigation(ImGui::FindWindowByName((name + GetPanelID()).c_str()),
            {mousePosition.x, mousePosition.y}, navigationPress, pointerOwnerHeld);
    const bool mouseOverSceneView = viewportInputAvailable && IsMouseHoveringView(mousePosition);
    const bool sceneViewActive = !shortcutsWindowOpen && (IsFocused() || IsHovered() || mouseOverSceneView);
    const bool isAnyItemActive = NLS_SERVICE(UI::UIManager).IsAnyItemActive();
    if (m_rectDrag && (ImGui::GetIO().AppFocusLost || !IsOpened())) CancelGizmoEditSession();
    if (m_selectionGesture && (ImGui::GetIO().AppFocusLost || !IsOpened())) CancelSceneSelectionGesture(true);
    const bool blockCameraInput = m_rectDrag.has_value() || ShouldSceneViewBlockCameraInput(
        shortcutsWindowOpen,
        isAnyItemActive,
        mouseOverSceneView,
        Core::IsEditorTextInputActive());
    const bool sceneViewInputContextActive = sceneViewActive && !(isAnyItemActive && !mouseOverSceneView);
    m_cameraController.SetInputBlocked(!enabled || !IsOpened() || blockCameraInput ||
        (m_selectionGesture && !m_selectionGesture->waitingForPick));
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
}

void Editor::Panels::SceneView::RefreshCameraInputAfterUi()
{
    UpdateCameraInputContext();
    m_cameraController.RefreshPointerCaptureAfterUi();
}

void Editor::Panels::SceneView::Update(float p_deltaTime)
{
    if (NLS::Editor::Core::GetPendingSceneLoadRendererResourceResolutionTaskCount() == 0u)
        m_hasRenderedSceneLoadResourcePlaceholder = false;
    const auto previousCameraPosition = m_camera.GetPosition();
    const auto previousCameraRotation = m_camera.GetRotation();
    const float previousCameraSize = m_camera.GetSize();
    UpdateCameraInputContext();

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
    m_cameraMovedForPresentation |= previousCameraSize != m_camera.GetSize();
    if (ShouldLogSceneCameraInputDiagnostics())
    {
        const auto mousePosition = EDITOR_CONTEXT(inputManager)->GetMousePosition();
        const bool shortcutsWindowOpen = Editor::Core::DoesShortcutSettingsWindowBlockSceneInput() || IsToolbarInteractionActive();
        const bool mouseOverSceneView = HasViewportImageInputBounds() && IsMouseHoveringView(mousePosition);
        const bool sceneViewActive = !shortcutsWindowOpen && (IsFocused() || IsHovered() || mouseOverSceneView);
        const bool blockCameraInput = m_cameraController.IsInputBlocked();
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
    if (m_currentOperation == p_operation) return;
    CancelGizmoEditSession();
    if (m_savedRectSpace)
    {
        m_currentSpace = *m_savedRectSpace;
        m_savedRectSpace.reset();
    }
    if (p_operation == Core::EGizmoOperation::RECT)
    {
        m_savedRectSpace = m_currentSpace;
        m_currentSpace = Core::SceneViewGizmoSpace::Local;
    }
    m_currentOperation = p_operation;
    InvalidateStaticFrameCache();
}

Editor::Core::SceneViewGizmoPivot Editor::Panels::SceneView::GetCurrentGizmoPivot() const
{
    return m_currentPivot;
}

void Editor::Panels::SceneView::SetCurrentGizmoPivot(const Editor::Core::SceneViewGizmoPivot p_pivot)
{
    if (m_gizmoEditSession && m_currentPivot != p_pivot) CancelGizmoEditSession();
    m_currentPivot = p_pivot;
}

void Editor::Panels::SceneView::ToggleCurrentGizmoPivot()
{
    SetCurrentGizmoPivot(Core::ToggleGizmoPivot(m_currentPivot));
}

Editor::Core::SceneViewGizmoSpace Editor::Panels::SceneView::GetCurrentGizmoSpace() const
{
    return m_currentSpace;
}

void Editor::Panels::SceneView::SetCurrentGizmoSpace(const Editor::Core::SceneViewGizmoSpace p_space)
{
    if (m_currentOperation == Core::EGizmoOperation::RECT) return;
    if (m_gizmoEditSession && m_currentSpace != p_space) CancelGizmoEditSession();
    m_currentSpace = p_space;
}

void Editor::Panels::SceneView::ToggleCurrentGizmoSpace()
{
    SetCurrentGizmoSpace(Core::ToggleGizmoSpace(m_currentSpace));
}

void Editor::Panels::SceneView::InitFrame()
{
    AViewControllable::InitFrame();

    auto grid = m_renderer->GetDescriptor<Rendering::GridRenderPass::GridDescriptor>();
    const auto& sceneSettings = Settings::EditorSettings::GetSceneViewSettingsObject();
    grid.enabled = sceneSettings.showGrid;
    grid.planarXY = m_is2D;
    m_renderer->RemoveDescriptor<Rendering::GridRenderPass::GridDescriptor>();
    m_renderer->AddDescriptor<Rendering::GridRenderPass::GridDescriptor>(std::move(grid));

    auto* debugRenderer = dynamic_cast<Editor::Rendering::DebugSceneRenderer*>(m_renderer.get());
    if (debugRenderer == nullptr)
        return;

    Engine::GameObject* selectedGameObject = nullptr;

    if (EDITOR_EXEC(IsAnyGameObjectSelected()))
    {
        selectedGameObject = EDITOR_EXEC(GetActiveGameObject());
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
            !m_activeDraggedPrefabRootAwaitingRendererResources),
        BuildSceneViewDebugDrawSettings(sceneSettings),
        sceneSettings.showSelectionOutline,
        EDITOR_EXEC(GetSelectedGameObjects())});
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
    const auto& viewSettings = Settings::EditorSettings::GetSceneViewSettingsObject();
    // Skybox visibility is its own Scene View setting.  Keep it independent of
    // the optional effects toggle so disabling particles/post effects does not
    // silently turn the shaded viewport background black.
    descriptor.includeSkyboxes = viewSettings.showSkybox;
    m_sceneVisibility.BindScene(GetScene(), GetScene() ? GetScene()->GetInstanceID() : 0);
    std::vector<int32_t> objectIds;
    if (const auto* scene = GetScene())
        for (auto* object : scene->GetGameObjects()) if (object) objectIds.push_back(object->GetInstanceID());
    descriptor.viewOptions.hiddenObjectIds = m_sceneVisibility.CaptureHiddenIds(objectIds, viewSettings.sceneVisibility);
    if (viewSettings.showSelectionWire)
        for (auto* selected : Core::GetSceneSelectionRoots(EDITOR_EXEC(GetSelectedGameObjects())))
        {
            const auto collect = [&](const auto& self, const Engine::GameObject& object) -> void
            {
                descriptor.viewOptions.selectionWireIds.push_back(object.GetInstanceID());
                for (const auto* child : object.GetChildren()) if (child) self(self, *child);
            };
            collect(collect, *selected);
            std::sort(descriptor.viewOptions.selectionWireIds.begin(), descriptor.viewOptions.selectionWireIds.end());
            descriptor.viewOptions.selectionWireIds.erase(std::unique(descriptor.viewOptions.selectionWireIds.begin(), descriptor.viewOptions.selectionWireIds.end()), descriptor.viewOptions.selectionWireIds.end());
        }
    descriptor.viewOptions.postProcessing = viewSettings.effectsEnabled && viewSettings.postProcessing;
    Core::ApplySceneViewDrawMode(Core::DecodeSceneViewDrawMode(viewSettings.drawMode), descriptor.viewOptions);
    descriptor.viewOptions.debugParameters = m_drawModeParameters;
    descriptor.viewOptions.previewLighting = !viewSettings.sceneLighting;
    descriptor.viewOptions.includeParticles = viewSettings.effectsEnabled && viewSettings.showParticles;
    const auto& viewColors = Settings::EditorSettings::GetViewSettingsObject();
    descriptor.viewOptions.backgroundColor = {viewColors.sceneViewBackground.r, viewColors.sceneViewBackground.g,
        viewColors.sceneViewBackground.b, 1.0f};
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
        pendingSceneLoadResourceTasks > 0u || viewSettings.drawMode == 1 ||
        descriptor.viewOptions.debugMode == Engine::Rendering::SceneDisplayDebugMode::Overdraw || !viewSettings.cameraOcclusionCulling;
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
    // Scene render revisions are part of the cache key and are raised by
    // render-affecting component changes, including runtime script updates.
    if (Render::DistanceField::DistanceFieldSystem::HasPendingWork()) return false;
    if (m_distanceFieldDebug.visualization != Render::DistanceField::DistanceFieldVisualization::Off)
    {
        const auto* renderer = dynamic_cast<Engine::Rendering::BaseSceneRenderer*>(m_renderer.get());
        return renderer && renderer->GetDistanceFieldStats().CanReuseVisualization();
    }
    return (Settings::EditorSettings::GetRenderingSettingsObject().enableSceneViewStaticFrameCache ||
            !Settings::EditorSettings::GetSceneViewSettingsObject().effectsEnabled ||
            !Settings::EditorSettings::GetSceneViewSettingsObject().alwaysRefresh);
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
        HashSceneViewCacheValue(selectionKey, NLS::Editor::Inspector::EditorSelectionService::Get().GetSelection().revision);
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
    HashSceneViewCacheValue(seed, Render::DistanceField::DistanceFieldSystem::RuntimeRevision());
    HashSceneViewCacheValue(seed, Render::DistanceField::GetDistanceFieldSourceRevision());
    HashSceneViewCacheValue(seed, m_distanceFieldDebugRevision);
    const auto& toolbar = Settings::EditorSettings::GetSceneViewSettingsObject();
    HashSceneViewCacheValue(seed, static_cast<uint64_t>(toolbar.drawMode));
    HashSceneViewCacheFloat(seed, m_drawModeParameters.overdrawMax);
    for (const auto& range : {m_drawModeParameters.albedoRange, m_drawModeParameters.specularRange})
        for (float value : {range.x, range.y, range.z, range.w}) HashSceneViewCacheFloat(seed, value);
    HashSceneViewCacheValue(seed, toolbar.effectsEnabled);
    HashSceneViewCacheValue(seed, toolbar.postProcessing);
    HashSceneViewCacheValue(seed, toolbar.alwaysRefresh);
    HashSceneViewCacheValue(seed, toolbar.sceneVisibility);
    HashSceneViewCacheValue(seed, m_sceneVisibility.Revision());
    HashSceneViewCacheValue(seed, toolbar.cameraOcclusionCulling);
    HashSceneViewCacheValue(seed, toolbar.showGrid);
    HashSceneViewCacheValue(seed, toolbar.sceneLighting);
    HashSceneViewCacheValue(seed, toolbar.showSkybox);
    HashSceneViewCacheValue(seed, toolbar.showParticles);
    HashSceneViewCacheValue(seed, toolbar.showGizmos);
    HashSceneViewCacheValue(seed, toolbar.showCameras);
    HashSceneViewCacheValue(seed, toolbar.showLights);
    HashSceneViewCacheValue(seed, toolbar.showBounds);
    HashSceneViewCacheValue(seed, toolbar.showCameraIcons);
    HashSceneViewCacheValue(seed, toolbar.showLightIcons);
    HashSceneViewCacheValue(seed, toolbar.icons3D);
    HashSceneViewCacheValue(seed, toolbar.fadeGizmos);
    HashSceneViewCacheValue(seed, toolbar.showSelectionWire);
    HashSceneViewCacheValue(seed, toolbar.showSelectionOutline);
    HashSceneViewCacheFloat(seed, toolbar.lightBillboardScale);
    HashSceneViewCacheValue(seed, m_is2D);
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

void Editor::Panels::SceneView::DrawDistanceFieldControls()
{
    namespace df = Render::DistanceField;
    auto* renderer = dynamic_cast<Engine::Rendering::BaseSceneRenderer*>(m_renderer.get());
    if (!renderer) return;
    bool changed = false;
    {
        int channel = static_cast<int>(m_distanceFieldDebug.channel);
        if (ImGui::Combo("Display", &channel, "Surface\0Trace Steps\0Signed Slice\0Normals\0Clipmaps\0Pages\0Residency\0"))
        { m_distanceFieldDebug.channel = static_cast<df::DistanceFieldDebugChannel>(channel); changed = true; }
        if (m_distanceFieldDebug.channel == df::DistanceFieldDebugChannel::Steps)
        {
            int maximum = m_distanceFieldDebug.traceStepsMax;
            if (ImGui::SliderInt("Red at steps", &maximum, 8, 512))
            { m_distanceFieldDebug.traceStepsMax = static_cast<uint16_t>(maximum); changed = true; }
            ImGui::TextWrapped("Blue to red: fewer to more steps. Surface lighting preserves shape; missed rays stay dark.");
        }
        if (m_distanceFieldDebug.channel == df::DistanceFieldDebugChannel::SignedSlice ||
            (m_distanceFieldDebug.channel == df::DistanceFieldDebugChannel::Residency &&
             m_distanceFieldDebug.visualization == df::DistanceFieldVisualization::Global))
            changed = ImGui::DragFloat("Slice Height", &m_distanceFieldDebug.slicePosition, .05f) || changed;
        changed = ImGui::Checkbox("Mostly Static Cache", &m_distanceFieldDebug.mostlyStaticOnly) || changed;
        if (ImGui::Button("Rebuild Global Pages"))
        { m_distanceFieldDebug.forceRebuild = true; changed = true; }
        DrawDistanceFieldBuildControls();
        const auto stats = renderer->GetDistanceFieldStats();
        ImGui::Separator();
        ImGui::Text("Objects: %llu  Bricks: %llu", static_cast<unsigned long long>(stats.objects), static_cast<unsigned long long>(stats.residentBricks));
        ImGui::Text("Pages: %llu  Dirty: %llu", static_cast<unsigned long long>(stats.residentPages), static_cast<unsigned long long>(stats.dirtyPages));
        ImGui::Text("Mesh / Pages: %.1f / %.1f MiB", stats.meshBytes / 1048576.0, stats.pageBytes / 1048576.0);
        ImGui::Text("Upload: %.2f MiB  Metadata: %.2f MiB", stats.uploadedBytes / 1048576.0, stats.metadataBytes / 1048576.0);
        ImGui::Text("Pending: %llu  Failed Loads: %llu  Allocation Failures: %llu", static_cast<unsigned long long>(stats.pendingLoads),
            static_cast<unsigned long long>(stats.failedLoads), static_cast<unsigned long long>(stats.allocationFailures));
        ImGui::Text("Allocation Retries: %llu", static_cast<unsigned long long>(stats.allocationRetries));
        ImGui::Text("Prepare: %.2f ms", stats.updateCpuMs);
        if (stats.gpuUpdateMs) ImGui::Text("GPU (last update): %.3f ms", *stats.gpuUpdateMs);
        else ImGui::TextDisabled("GPU (last update): --");
        ImGui::TextDisabled("View image: %s", GetLastStaticFrameCacheDecisionReason() == StaticFrameCacheDecisionReason::Hit
            ? "cached" : "updating");
        if (!stats.diagnostic.empty()) ImGui::TextWrapped("%s", stats.diagnostic.c_str());
    }
    if (changed) ++m_distanceFieldDebugRevision;
    renderer->SetDistanceFieldDebugSettings(m_distanceFieldDebug);
}

void Editor::Panels::SceneView::OnAfterDrawWidgets()
{
    DrawSceneSelectionGesture();
    if (ShouldResolveViewportPicking(ViewportOverlayLifecyclePhase::AfterWidgetDraw))
        HandleGameObjectPicking();
    EndViewportOverlayDrawListChannels();
    DrawDistanceFieldStatusOverlay();
    MarkViewportImageInputBoundsForLastDraw();
}

void Editor::Panels::SceneView::CancelGizmoEditSession()
{
    if (m_gizmoEditSession) { ImGuizmo::Enable(false); ImGuizmo::Enable(true); }
    m_rectDrag.reset();
    if (m_rectActiveId != 0 && ImGui::GetCurrentContext())
    {
        if (ImGui::GetKeyOwner(ImGuiKey_MouseLeft) == m_rectActiveId)
            ImGui::SetKeyOwner(ImGuiKey_MouseLeft, ImGuiKeyOwner_NoOwner);
        if (ImGui::GetActiveID() == m_rectActiveId) ImGui::ClearActiveID();
    }
    m_rectActiveId = 0;
    if (!m_gizmoEditSession.has_value())
    {
        m_gizmoEditTarget.reset();
        m_gizmoEditState.reset();
        return;
    }

    if (NLS::Core::ServiceLocator::Contains<Editor::Undo::EditorUndoService>())
    {
        auto& undoService = NLS::Core::ServiceLocator::Get<Editor::Undo::EditorUndoService>();
        static_cast<void>(undoService.CancelEditSession(*m_gizmoEditSession));
    }

    m_gizmoEditSession.reset();
    m_gizmoEditTarget.reset();
    m_gizmoEditState.reset();
}

bool Editor::Panels::SceneView::BeginGizmoEditSession(Engine::GameObject& object)
{
    if (m_gizmoEditSession) return true;
    if (!NLS::Core::ServiceLocator::Contains<Editor::Undo::EditorUndoService>()) return false;
    const auto selected = EDITOR_EXEC(GetSelectedGameObjects());
    if (selected.size() > 1 && m_currentOperation != Core::EGizmoOperation::RECT)
    {
        Editor::Undo::UndoCommandInfo info;
        info.label = "Transform GameObjects"; info.sourceModule = "Scene View";
        info.scopes = {BuildSceneViewUndoScope(object)};
        std::vector<SceneViewGroupTransformEntry> entries;
        for (auto* root : Core::GetSceneSelectionRoots(selected))
        {
            auto target = CaptureSceneViewGizmoTarget(*root, info.scopes.front());
            auto before = ReadSceneViewTransform(*root);
            if (!target || !before) return false;
            const auto* transform = root->GetTransform();
            entries.push_back({*target, *before, *before,
                {transform->GetWorldPosition(), transform->GetWorldRotation(), transform->GetLocalScale()}});
            info.affected.push_back(target->object.GetAffectedKey());
        }
        auto activeTarget = CaptureSceneViewGizmoTarget(object, info.scopes.front());
        if (!activeTarget || entries.empty()) return false;
        info.estimatedBytes = entries.size() * sizeof(SceneViewGroupTransformEntry);
        auto state = std::make_shared<SceneViewGizmoEditState>();
        state->multi = true; state->space = m_currentSpace; state->pivot = m_currentPivot; state->operation = m_currentOperation;
        state->selectionRevision = NLS::Editor::Inspector::EditorSelectionService::Get().GetSelection().revision;
        state->initialModelMatrix = Core::ToImGuizmoMatrix(Core::BuildSceneSelectionGizmoMatrix(selected, object, m_currentPivot, m_currentSpace));
        auto begin = NLS::Core::ServiceLocator::Get<Editor::Undo::EditorUndoService>().BeginEditSession({info,
            std::make_unique<SceneViewGroupTransformEdit>(info, std::move(entries), state)});
        if (begin.result.state != Editor::Undo::UndoRequestState::Completed || !begin.session.IsValid()) return false;
        m_gizmoEditSession.emplace(std::move(begin.session)); m_gizmoEditTarget = activeTarget->object; m_gizmoEditState = std::move(state);
        return true;
    }
    const auto before = ReadSceneViewTransform(object);
    auto target = CaptureSceneViewGizmoTarget(object, BuildSceneViewUndoScope(object));
    if (!before || !target) return false;
    Editor::Undo::UndoCommandInfo info;
    info.label = m_currentOperation == Core::EGizmoOperation::RECT ? "Rect Transform" : "Transform";
    info.sourceModule = "Scene View";
    info.scopes = {target->object.GetScope()};
    info.affected = {target->object.GetAffectedKey()};
    info.estimatedBytes = sizeof(SceneViewTransformSnapshot) * 2u;
    const auto targetForPanel = target->object;
    auto state = std::make_shared<SceneViewGizmoEditState>();
    state->selectionRevision = NLS::Editor::Inspector::EditorSelectionService::Get().GetSelection().revision;
    auto begin = NLS::Core::ServiceLocator::Get<Editor::Undo::EditorUndoService>().BeginEditSession({
        info, std::make_unique<SceneViewTransformEdit>(info, std::move(*target), *before, state)});
    if (begin.result.state != Editor::Undo::UndoRequestState::Completed || !begin.session.IsValid()) return false;
    m_gizmoEditSession.emplace(std::move(begin.session));
    m_gizmoEditTarget = targetForPanel;
    m_gizmoEditState = std::move(state);
    return true;
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

    if (m_distanceFieldDebug.visualization != Render::DistanceField::DistanceFieldVisualization::Off &&
        (Render::DistanceField::DistanceFieldSystem::HasPendingWork() ||
            EDITOR_CONTEXT(importProgressTracker).HasRunningJobs() ||
            std::any_of(m_distanceFieldBuildAssets.begin(), m_distanceFieldBuildAssets.end(),
                [&](const auto& path) { return Editor::Assets::DistanceFieldBuildService::IsBuilding(EDITOR_CONTEXT(projectPath),path); })))
        return;
#if defined(NLS_ENABLE_TEST_HOOKS)
    if (const auto* action = std::getenv("NLS_EDITOR_TOOLBAR_VALIDATION_ACTION");
        action && std::string_view(action) == "build-distance-fields" && m_distanceFieldBuildAssets.empty())
        return;
#endif

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
        if (selectedGameObject == nullptr || (Settings::EditorSettings::GetSceneViewSettingsObject().sceneVisibility && IsObjectHidden(*selectedGameObject)))
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

    std::shared_ptr<Render::RHI::RHITexture> texture;
    bool usingMetalViewportPreview = false;
#if defined(__APPLE__)
    if (!IsSceneRendererAvailable() && m_image != nullptr && m_image->textureView != nullptr)
    {
        texture = m_image->textureView->GetTexture();
        usingMetalViewportPreview = texture != nullptr;
    }
#endif
    if (texture == nullptr)
        texture = m_fbo.GetExplicitTextureHandle();
    if (texture == nullptr ||
        (!usingMetalViewportPreview &&
            !Render::Context::DriverRendererAccess::HasCompletedReadbackTexture(*driver, texture)))
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
    auto readbackStatus = BuildSceneViewValidationReadbackStatus(
        nonBlackPixels,
        static_cast<uint32_t>(maxChannel),
        pendingSceneLoadTextureLoads);
    if (m_distanceFieldDebug.visualization != Render::DistanceField::DistanceFieldVisualization::Off &&
        m_distanceFieldDebug.channel == Render::DistanceField::DistanceFieldDebugChannel::Surface && maxChannel <= 32u)
        readbackStatus = "empty-distance-field-visualization";

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
    if (m_distanceFieldDebug.visualization != Render::DistanceField::DistanceFieldVisualization::Off)
        if (const auto* renderer = dynamic_cast<Engine::Rendering::BaseSceneRenderer*>(m_renderer.get()))
        {
            const auto stats = renderer->GetDistanceFieldStats();
            summary << "distanceFieldObjects=" << stats.objects << "\n";
            summary << "distanceFieldBricks=" << stats.residentBricks << "\n";
            summary << "distanceFieldPages=" << stats.residentPages << "\n";
            summary << "distanceFieldFailedLoads=" << stats.failedLoads << "\n";
            summary << "distanceFieldDiagnostic=" << stats.diagnostic << "\n";
        }

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

void Editor::Panels::SceneView::DrawRectTool(Engine::GameObject& object, const Core::RectToolView& view,
    const bool snapEnabled)
{
    using Core::RectToolHandle;
    auto& io = ImGui::GetIO();
    const Maths::Vector2 mouse {io.MousePos.x, io.MousePos.y};
    const float uiScale = NLS_SERVICE(UI::UIManager).GetScale();
    std::optional<Core::RectToolFrame> frame;
    RectToolHandle hovered = RectToolHandle::None;

    if (m_rectDrag)
    {
        const auto& initial = m_rectDrag->frame;
        bool environmentChanged = view.min != initial.view.min || view.max != initial.view.max;
        const auto* parent = object.GetParent();
        const auto parentWorld = parent && parent->GetTransform()
            ? parent->GetTransform()->GetWorldMatrix() : Maths::Matrix4::Identity;
        for (int i = 0; i < 16; ++i)
            environmentChanged |= parentWorld.data[i] != initial.target.parentWorld.data[i] ||
                view.viewProjection.data[i] != initial.view.viewProjection.data[i];
        if (environmentChanged || io.AppFocusLost || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
            ImGui::GetActiveID() != m_rectActiveId)
        {
            CancelGizmoEditSession();
            return;
        }
        // The scoped Command cancels after UI routing. External interruption
        // above still cancels immediately, even when Escape arrives with it.
        if (Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::SceneCancel).pressed) return;
        ImGui::KeepAliveID(m_rectActiveId);
        m_gizmoInteraction.isUsing = true;
        auto& undoService = NLS::Core::ServiceLocator::Get<Editor::Undo::EditorUndoService>();
        if (io.MouseDown[0])
        {
            const auto& settings = Settings::EditorSettings::GetSceneToolSettingsObject();
            const Core::RectToolSnap snap {snapEnabled, settings.translationSnapUnit,
                settings.rotationSnapUnit, settings.scalingSnapUnit};
            const auto candidate = Core::UpdateRectToolDrag(*m_rectDrag, mouse, snap);
            if (!candidate || !m_gizmoEditState)
            {
                CancelGizmoEditSession();
                return;
            }
            const auto previous = m_gizmoEditState->pendingLocalTransform.value_or(initial.target.transform);
            if (*candidate != previous)
            {
                m_gizmoEditState->pendingLocalTransform = *candidate;
                const auto preview = undoService.PreviewEditSession(*m_gizmoEditSession);
                if (preview.state != Editor::Undo::UndoRequestState::Completed)
                {
                    CancelGizmoEditSession();
                    return;
                }
            }
            auto visualTarget = initial.target;
            visualTarget.transform = *candidate;
            frame = Core::BuildRectToolFrame(visualTarget, view, initial.pivot, initial.plane);
        }
        else
        {
            const auto commit = undoService.CommitEditSession(*m_gizmoEditSession);
            if (commit.state == Editor::Undo::UndoRequestState::Completed || !undoService.HasActiveEditSession())
            {
                m_gizmoEditSession.reset();
                CancelGizmoEditSession();
            }
        }
    }

    if (!m_rectDrag)
    {
        frame = Core::BuildRectToolFrame(Core::CaptureRectToolTarget(object), view, m_currentPivot);
        const bool inputAvailable = !io.AppFocusLost && !Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::SceneSuspendSelection).actuated &&
            !Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::CameraFly).actuated &&
            !Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::CameraPan).actuated &&
            !Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::CameraOrbit).actuated &&
            !m_gizmoInteraction.isViewHovered && !m_gizmoInteraction.isViewUsing &&
            !ImGui::GetCurrentContext()->DragDropActive && ImGui::GetActiveID() == 0 && IsMouseHoveringView(mouse);
        if (frame && inputAvailable)
        {
            hovered = Core::HitTestRectTool(*frame, mouse, uiScale);
            m_gizmoInteraction.isHovered = hovered != RectToolHandle::None;
            if (m_gizmoInteraction.isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                auto drag = Core::BeginRectToolDrag(*frame, hovered, mouse);
                if (drag && BeginGizmoEditSession(object))
                {
                    m_rectDrag = std::move(drag);
                    m_rectActiveId = ImGui::GetID("##SceneRectTool");
                    auto* window = ImGui::GetCurrentWindow();
                    ImGui::FocusWindow(window);
                    ImGui::SetActiveID(m_rectActiveId, window);
                    ImGui::SetKeyOwner(ImGuiKey_MouseLeft, m_rectActiveId, ImGuiInputFlags_LockUntilRelease);
                    m_gizmoInteraction.isUsing = true;
                    m_pendingClickPickRenderPos.reset();
                    m_pendingClickPickingSignature.reset();
                }
            }
        }
    }
    if (frame) Core::DrawRectToolOverlay(*ImGui::GetWindowDrawList(), *frame, hovered,
        m_rectDrag ? m_rectDrag->handle : RectToolHandle::None, uiScale);
}

void Editor::Panels::SceneView::DrawViewportOverlay()
{
    m_gizmoInteraction = {};
	if (m_gizmoEditSession.has_value() &&
		(!NLS::Core::ServiceLocator::Contains<Editor::Undo::EditorUndoService>() ||
		 !NLS::Core::ServiceLocator::Get<Editor::Undo::EditorUndoService>().HasActiveEditSession()))
	{
		// A global Undo (or Escape) may have cancelled this continuous edit.
		CancelGizmoEditSession();
	}
    if (Editor::Core::DoesShortcutSettingsWindowBlockSceneInput() || IsToolbarInteractionActive())
    {
        CancelGizmoEditSession();
        return;
    }

    const auto imageMin = HasViewportImageBounds()
        ? GetViewportImageMin()
        : GetCurrentViewportImageMin();
    const auto imageMax = HasViewportImageBounds()
        ? GetViewportImageMax()
        : GetCurrentViewportImageMax();
    const auto imageWidth = imageMax.x - imageMin.x;
    const auto imageHeight = imageMax.y - imageMin.y;
    if (imageWidth <= 0.0f || imageHeight <= 0.0f)
    {
        CancelGizmoEditSession();
        return;
    }

    const auto overlayMatrices = GetViewportOverlayCameraMatrices();
    auto viewMatrix = Core::ToImGuizmoMatrix(overlayMatrices.view);
    auto projectionMatrix = Core::ToImGuizmoMatrix(overlayMatrices.projection);
    const bool cameraControlActive = IsCameraControlActive();
    EnsureCameraFocus();

    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(imageMin.x, imageMin.y, imageWidth, imageHeight);
    ImGuizmo::SetOrthographic(m_camera.GetProjectionMode() == Render::Settings::EProjectionMode::ORTHOGRAPHIC);

    const auto viewGizmoRect = Core::GetSceneViewViewGizmoRect(imageMin, imageMax);
    auto viewGizmoModelMatrix = Core::ToImGuizmoMatrix(Maths::Matrix4::Identity);
    if (!m_is2D) ImGuizmo::ViewManipulate(
        viewMatrix.data(),
        projectionMatrix.data(),
        ToNativeImGuizmoOperation(m_currentOperation == Core::EGizmoOperation::RECT
            ? Core::EGizmoOperation::TRANSLATE : m_currentOperation),
        ToNativeImGuizmoMode(m_currentSpace),
        viewGizmoModelMatrix.data(),
        kSceneViewGizmoCameraLength,
        ImVec2(viewGizmoRect.position.x, viewGizmoRect.position.y),
        ImVec2(viewGizmoRect.size.x, viewGizmoRect.size.y),
        IM_COL32(0, 0, 0, 0));

    m_gizmoInteraction.isViewHovered = !m_is2D && ImGuizmo::IsViewManipulateHovered();
    m_gizmoInteraction.isViewUsing = !m_is2D && ImGuizmo::IsUsingViewManipulate();
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

    if (m_selectionGesture && !m_selectionGesture->waitingForPick)
    {
        // Keep the view cube visible while selection owns the mouse.
        CancelGizmoEditSession();
        return;
    }

    auto* undoService = NLS::Core::ServiceLocator::Contains<Editor::Undo::EditorUndoService>()
        ? &NLS::Core::ServiceLocator::Get<Editor::Undo::EditorUndoService>()
        : nullptr;

    if (!EDITOR_EXEC(IsAnyGameObjectSelected()))
    {
        CancelGizmoEditSession();
        return;
    }

    auto* selectedGameObject = EDITOR_EXEC(GetActiveGameObject());
    if (selectedGameObject == nullptr || selectedGameObject->GetTransform() == nullptr)
    {
        CancelGizmoEditSession();
        return;
    }

    if ((m_gizmoEditTarget.has_value() && !m_gizmoEditTarget->Matches(*selectedGameObject)) ||
        (m_gizmoEditState && m_gizmoEditState->selectionRevision != NLS::Editor::Inspector::EditorSelectionService::Get().GetSelection().revision))
    {
        CancelGizmoEditSession();
    }

    const auto selectedObjects = EDITOR_EXEC(GetSelectedGameObjects());
    auto modelMatrix = selectedObjects.size() > 1
        ? (m_gizmoEditState && m_gizmoEditState->multi && m_gizmoEditState->pendingModelMatrix
            ? *m_gizmoEditState->pendingModelMatrix
            : Core::ToImGuizmoMatrix(Core::BuildSceneSelectionGizmoMatrix(selectedObjects, *selectedGameObject, m_currentPivot, m_currentSpace)))
        : Core::GetGameObjectWorldGizmoMatrix(*selectedGameObject, m_currentPivot);

    using namespace Windowing::Inputs;
    const bool snapEnabled = Settings::EditorSettings::GetSceneViewSettingsObject().snapEnabled ||
        Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::SceneSnap).actuated;
    if (m_currentOperation == Core::EGizmoOperation::RECT)
    {
        if (cameraControlActive || selectedObjects.size() > 1)
        {
            CancelGizmoEditSession();
            return;
        }
        Core::RectToolView view;
        view.viewProjection = overlayMatrices.projection * overlayMatrices.view;
        view.cameraPosition = m_camera.GetPosition();
        view.cameraForward = m_camera.GetRotation() * Maths::Vector3::Forward;
        view.min = imageMin;
        view.max = imageMax;
        view.orthographic = m_camera.GetProjectionMode() == Render::Settings::EProjectionMode::ORTHOGRAPHIC;
        view.is2D = m_is2D;
        DrawRectTool(*selectedGameObject, view, snapEnabled);
        return;
    }
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


    if (!cameraControlActive && m_gizmoInteraction.isUsing && !BeginGizmoEditSession(*selectedGameObject)) return;

    if (!cameraControlActive && manipulated && undoService != nullptr && m_gizmoEditSession.has_value())
    {
		if (!m_gizmoEditState)
		{
			CancelGizmoEditSession();
			return;
		}
		m_gizmoEditState->pendingModelMatrix = modelMatrix;
		m_gizmoEditState->operation = m_currentOperation;
		m_gizmoEditState->pivot = m_currentPivot;
		const auto preview = undoService->PreviewEditSession(*m_gizmoEditSession);
		if (preview.state != Editor::Undo::UndoRequestState::Completed)
		{
			CancelGizmoEditSession();
		}
	}

	if (undoService != nullptr && m_gizmoEditSession.has_value() && !m_gizmoInteraction.isUsing)
	{
		const auto commit = undoService->CommitEditSession(*m_gizmoEditSession);
		if (commit.state == Editor::Undo::UndoRequestState::Completed || !undoService->HasActiveEditSession())
		{
			m_gizmoEditSession.reset();
			m_gizmoEditTarget.reset();
			m_gizmoEditState.reset();
		}
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

bool Editor::Panels::SceneView::IsCameraNavigationActive() const
{
    return IsCameraControlActive();
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

std::vector<Engine::GameObject*> Editor::Panels::SceneView::QueryMarqueeObjects(
    const std::optional<Core::SceneSelectionRect>& rectangle)
{
    std::vector<Engine::GameObject*> result;
    auto* scene = GetScene();
    if (!scene) return result;
    const auto matrices = GetViewportOverlayCameraMatrices();
    const auto viewProjection = matrices.projection * matrices.view;
    const Core::SceneSelectionRect viewport{GetViewportImageMin(), GetViewportImageMax()};
    const auto& settings = Settings::EditorSettings::GetSceneViewSettingsObject();
    const auto collect = [&](auto&& self, Engine::GameObject& object) -> void
    {
        if (object.IsEditorTransient()) return;
        if (object.IsActive() && m_camera.IsLayerVisible(object.GetLayer()) && object.GetTag() != "EditorOnly" &&
            (!settings.sceneVisibility || !IsObjectHidden(object)))
        {
            bool hit = false;
            std::array objects{&object};
            if (auto bounds = Core::GetSceneSelectionBounds(objects, false))
            {
                if (!rectangle) hit = true;
                else if (auto projected = Core::ProjectSceneSelectionBounds(*bounds, viewProjection, viewport))
                    hit = projected->Intersects(*rectangle);
            }
            const bool hasIcon = settings.showGizmos &&
                ((settings.showCameraIcons && settings.showCameras && object.GetComponent<Engine::Components::CameraComponent>()) ||
                 (settings.showLightIcons && settings.showLights && object.GetComponent<Engine::Components::LightComponent>()));
            if (!hit && hasIcon && object.GetTransform())
            {
                if (!rectangle) hit = true;
                else
                {
                    const auto position = object.GetTransform()->GetWorldPosition();
                    if (auto projected = Core::ProjectSceneSelectionBounds({position, position}, viewProjection, viewport))
                    {
                        const float radius = ImGui::GetFrameHeight() * .75f * settings.lightBillboardScale;
                        projected->min.x -= radius; projected->min.y -= radius;
                        projected->max.x += radius; projected->max.y += radius;
                        hit = projected->Intersects(*rectangle);
                    }
                }
            }
            if (hit) result.push_back(&object);
        }
        for (auto* child : object.GetChildren()) if (child) self(self, *child);
    };
    for (auto* object : scene->GetGameObjects()) if (object && !object->HasParent()) collect(collect, *object);
    return result;
}

void Editor::Panels::SceneView::SelectAll()
{
    CancelSceneSelectionGesture(false);
    EDITOR_EXEC(SelectGameObjects(QueryMarqueeObjects(std::nullopt)));
}

void Editor::Panels::SceneView::FocusSelection()
{
    const auto objects = EDITOR_EXEC(GetSelectedGameObjects());
    if (objects.empty()) return;
    auto bounds = Core::GetSceneSelectionBounds(objects);
    if (!bounds)
    {
        for (const auto* object : objects)
            if (object->GetTransform())
            {
                const auto position = object->GetTransform()->GetWorldPosition();
                if (!bounds) bounds = Core::SceneSelectionBounds{position, position};
                else for (int axis = 0; axis < 3; ++axis)
                { bounds->min[axis] = (std::min)(bounds->min[axis], position[axis]); bounds->max[axis] = (std::max)(bounds->max[axis], position[axis]); }
            }
    }
    if (!bounds) return;
    const auto center = (bounds->min + bounds->max) * .5f;
    const auto size = bounds->max - bounds->min;
    const float radius = (std::max)(1.f, std::sqrt(size.x * size.x + size.y * size.y + size.z * size.z) * .5f);
    const float distance = radius / (std::max)(.1f, std::sin(m_camera.GetFov() * .00872664626f));
    if (m_camera.GetProjectionMode() == Render::Settings::EProjectionMode::ORTHOGRAPHIC) m_camera.SetSize(radius * 1.2f);
    m_cameraController.MoveToPoint(center, distance * 1.2f);
}

bool Editor::Panels::SceneViewSelectionCapture::CanBegin() const
{
    return !m_id && ImGui::GetCurrentContext() && ImGui::GetActiveID() == 0 &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left, ImGuiInputFlags_None, ImGuiKeyOwner_NoOwner);
}

void Editor::Panels::SceneViewSelectionCapture::Begin()
{
    m_id = ImGui::GetID("##SceneSelectionMarquee");
    auto* window = ImGui::GetCurrentWindow();
    ImGui::FocusWindow(window);
    ImGui::SetActiveID(m_id, window);
    ImGui::SetKeyOwner(ImGuiKey_MouseLeft, m_id, ImGuiInputFlags_LockUntilRelease);
    ImGui::KeepAliveID(m_id);
}

Editor::Panels::SceneViewSelectionCaptureState Editor::Panels::SceneViewSelectionCapture::Poll() const
{
    if (!m_id || !ImGui::GetCurrentContext() ||
        !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
        ImGui::GetActiveID() != m_id || ImGui::GetKeyOwner(ImGuiKey_MouseLeft) != m_id)
        return SceneViewSelectionCaptureState::Lost;
    ImGui::KeepAliveID(m_id);
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left, m_id))
        return SceneViewSelectionCaptureState::Released;
    return ImGui::GetIO().MouseDown[ImGuiMouseButton_Left]
        ? SceneViewSelectionCaptureState::Held : SceneViewSelectionCaptureState::Lost;
}

void Editor::Panels::SceneViewSelectionCapture::Reset()
{
    if (m_id && ImGui::GetCurrentContext())
    {
        if (ImGui::GetActiveID() == m_id) ImGui::ClearActiveID();
        if (ImGui::GetKeyOwner(ImGuiKey_MouseLeft) == m_id)
            ImGui::SetKeyOwner(ImGuiKey_MouseLeft, ImGuiKeyOwner_NoOwner);
    }
    m_id = 0;
}

void Editor::Panels::SceneView::CancelSceneSelectionGesture(const bool restore)
{
    if (restore && m_selectionGesture && GetScene() &&
        m_selectionGesture->scope == BuildSceneViewUndoScope(*GetScene()) &&
        NLS::Editor::Inspector::EditorSelectionService::Get().GetSelection().revision == m_selectionGesture->expectedRevision)
        NLS::Editor::Inspector::EditorSelectionService::Get().SetSelection(m_selectionGesture->before);
    m_selectionCapture.Reset();
    m_selectionGesture.reset();
    m_pendingClickPickRenderPos.reset(); m_pendingClickPickingSignature.reset(); m_pendingClickMinReadablePickingFrameSerial = 0;
}

bool Editor::Panels::SceneView::HasActiveInputGesture() const
{
    return m_rectDrag.has_value() || m_gizmoEditSession.has_value() || m_selectionGesture.has_value();
}

void Editor::Panels::SceneView::CancelInputGesture()
{
    CancelGizmoEditSession();
    CancelSceneSelectionGesture(true);
}

void Editor::Panels::SceneView::CompleteSceneClickSelection(Engine::GameObject* object)
{
    if (!m_selectionGesture || !m_selectionGesture->waitingForPick) return;
    auto& service = NLS::Editor::Inspector::EditorSelectionService::Get();
    const auto gesture = *m_selectionGesture;
    if (!GetScene() || gesture.scope != BuildSceneViewUndoScope(*GetScene()) || service.GetSelection().revision != gesture.expectedRevision)
    { CancelSceneSelectionGesture(false); return; }
    NLS::Editor::Inspector::InspectorTargetSet picked;
    if (object && object->GetScene() == GetScene() && !object->IsEditorTransient())
        picked.targets.push_back(NLS::Editor::Inspector::InspectorTargetHandle::FromObject(*object, NLS::Editor::Inspector::InspectorTargetKind::GameObject));
    auto operation = NLS::Editor::Inspector::SelectionOperation::Replace;
    if (gesture.action) operation = NLS::Editor::Inspector::SelectionOperation::Toggle;
    else if (gesture.shift)
    {
        operation = NLS::Editor::Inspector::SelectionOperation::Add;
        if (!picked.Empty() && gesture.before.Active() && *gesture.before.Active() == picked.targets.front())
            operation = NLS::Editor::Inspector::SelectionOperation::Remove;
    }
    const auto before = Core::FilterSceneSelection(gesture.before, *GetScene());
    service.SetSelection(NLS::Editor::Inspector::ComposeSelection(before, std::move(picked), operation));
    CancelSceneSelectionGesture(false);
}

void Editor::Panels::SceneView::DrawSceneSelectionGesture()
{
    auto& service = NLS::Editor::Inspector::EditorSelectionService::Get();
    const auto& io = ImGui::GetIO();
    auto* scene = GetScene();
    if (m_selectionGesture && (!scene || io.AppFocusLost || !IsOpened() ||
        m_selectionGesture->scope != BuildSceneViewUndoScope(*scene) ||
        service.GetSelection().revision != m_selectionGesture->expectedRevision ||
        Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::SceneSuspendSelection).actuated ||
        Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::CameraFly).actuated ||
        Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::CameraPan).actuated ||
        Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::CameraOrbit).actuated || IsCameraControlActive() || IsToolbarInteractionActive()))
    { CancelSceneSelectionGesture(true); return; }
    if (m_selectionGesture && Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::SceneCancel).pressed) return;
    const Maths::Vector2 mouse{io.MousePos.x, io.MousePos.y};
    if (m_selectionCapture.CanBegin() && scene && HasViewportImageBounds() && IsMouseHoveringView(mouse) &&
        !Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::SceneSuspendSelection).actuated && !IsCameraControlActive() && !IsToolbarInteractionActive() &&
        !Core::ShouldSuppressScenePicking(m_gizmoInteraction) && !ImGui::IsDragDropActive())
    {
        CancelSceneSelectionGesture(false);
        SelectionGesture gesture;
        gesture.drag.start = gesture.drag.current = mouse;
        gesture.before = service.GetSelection(); gesture.expectedRevision = gesture.before.revision;
        gesture.scope = BuildSceneViewUndoScope(*scene);
        m_selectionGesture = std::move(gesture);
        m_selectionCapture.Begin();
    }
    if (!m_selectionGesture) return;
    auto& gesture = *m_selectionGesture;
    if (gesture.waitingForPick)
    {
        if (!m_pendingClickPickRenderPos) CancelSceneSelectionGesture(false);
        return;
    }
    const auto captureState = m_selectionCapture.Poll();
    if (captureState == SceneViewSelectionCaptureState::Lost)
    { CancelSceneSelectionGesture(true); return; }
    gesture.drag.Update(mouse, io.MouseDragThreshold);
    gesture.action = Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::SceneToggleSelection).actuated;
    gesture.shift = Shortcuts::GetEditorInputState(Shortcuts::EditorInputAction::SceneAddSelection).actuated;
    if (gesture.drag.marquee)
    {
        const auto min = GetViewportImageMin(), max = GetViewportImageMax();
        const Maths::Vector2 clipped{std::clamp(mouse.x, min.x, max.x), std::clamp(mouse.y, min.y, max.y)};
        const auto rectangle = Core::MakeSceneSelectionRect(gesture.drag.start, clipped);
        const auto objects = QueryMarqueeObjects(rectangle);
        NLS::Editor::Inspector::InspectorTargetSet candidates;
        for (auto* object : objects) candidates.targets.push_back(NLS::Editor::Inspector::InspectorTargetHandle::FromObject(*object, NLS::Editor::Inspector::InspectorTargetKind::GameObject));
        const auto operation = gesture.action ? NLS::Editor::Inspector::SelectionOperation::Remove : gesture.shift ? NLS::Editor::Inspector::SelectionOperation::Add : NLS::Editor::Inspector::SelectionOperation::Replace;
        service.SetSelection(NLS::Editor::Inspector::ComposeSelection(Core::FilterSceneSelection(gesture.before, *scene), std::move(candidates), operation, true));
        gesture.expectedRevision = service.GetSelection().revision;
        auto* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect({min.x,min.y}, {max.x,max.y}, true);
        draw->AddRectFilled({rectangle.min.x,rectangle.min.y}, {rectangle.max.x,rectangle.max.y}, IM_COL32(70,145,235,35));
        draw->AddRect({rectangle.min.x,rectangle.min.y}, {rectangle.max.x,rectangle.max.y}, IM_COL32(90,170,255,230));
        draw->PopClipRect();
    }
    if (captureState != SceneViewSelectionCaptureState::Released) return;
    m_selectionCapture.Reset();
    if (gesture.drag.marquee) { m_selectionGesture.reset(); return; }
    const auto local = GetLocalViewPosition(gesture.drag.start);
    auto* renderer = dynamic_cast<Rendering::DebugSceneRenderer*>(m_renderer.get());
    if (!local || !renderer) { CancelSceneSelectionGesture(false); return; }
    auto& picking = renderer->GetPass<Rendering::PickingRenderPass>("Picking");
    if (!picking.SupportsPickingReadback()) { CancelSceneSelectionGesture(false); return; }
    const auto [width,height] = GetSafeSize();
    m_pendingClickPickRenderPos = Maths::Vector2{std::clamp(local->x, 0.f, (std::max)(0.f, float(width) - 1)),
        NLS_SERVICE(UI::UIManager).UsesBottomLeftRenderTargetOrigin()
            ? std::clamp(float(height) - 1 - local->y, 0.f, (std::max)(0.f, float(height) - 1))
            : std::clamp(local->y, 0.f, (std::max)(0.f, float(height) - 1))};
    m_pendingClickPickingSignature.reset(); m_pendingClickMinReadablePickingFrameSerial = picking.GetSubmittedPickingFrameSerial() + 1;
    m_hasPickingSample = false; gesture.waitingForPick = true;
}

bool Editor::Panels::SceneView::ShouldRequestPickingFrame() const
{
    using namespace Windowing::Inputs;
    if (Editor::Core::DoesShortcutSettingsWindowBlockSceneInput())
        return false;

    const bool pickingSuppressedByGizmo = Core::ShouldSuppressScenePicking(m_gizmoInteraction);
    if (pickingSuppressedByGizmo && !m_pendingClickPickRenderPos.has_value())
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
    const bool mouseOverView = IsMouseHoveringView(screenMousePos);
    if ((!mouseOverView && !m_pendingClickPickRenderPos.has_value()) || IsResizing())
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
    if (!localMousePos.has_value() && !m_pendingClickPickRenderPos.has_value())
        return false;

    auto mousePos = m_pendingClickPickRenderPos.has_value() ? *m_pendingClickPickRenderPos : *localMousePos;
    const auto [safeWidth, safeHeight] = GetSafeSize();
    const float maxRenderX = std::max(0.0f, static_cast<float>(safeWidth) - 1.0f);
    const float maxRenderY = std::max(0.0f, static_cast<float>(safeHeight) - 1.0f);

    if (!m_pendingClickPickRenderPos.has_value())
    {
        mousePos.x = std::clamp(mousePos.x, 0.0f, maxRenderX);
        mousePos.y = NLS_SERVICE(UI::UIManager).UsesBottomLeftRenderTargetOrigin()
            ? std::clamp(static_cast<float>(safeHeight) - 1.0f - mousePos.y, 0.0f, maxRenderY)
            : std::clamp(mousePos.y, 0.0f, maxRenderY);
    }

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
        mouseOverView || m_pendingClickPickRenderPos.has_value(),
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
        const auto visiblePickableSources =
            sceneRenderer != nullptr
                ? sceneRenderer->GetLastVisiblePickablePrimitiveDrawSources()
                : nullptr;
        if (sceneRenderer != nullptr &&
            sceneRenderer->HasLastVisiblePickablePrimitiveDrawSources() &&
            visiblePickableSources != nullptr &&
            ShouldSkipSceneHoverPickingForVisibleDrawBudget(
                false,
                static_cast<uint64_t>(visiblePickableSources->size()),
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
    if (Editor::Core::DoesShortcutSettingsWindowBlockSceneInput() || IsToolbarInteractionActive())
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
    const bool mouseOverView = IsMouseHoveringView(screenMousePos);

    if (inputManager.IsMouseButtonReleased(EMouseButton::MOUSE_BUTTON_LEFT))
    {
        m_gizmoInteraction.isUsing = false;
    }

    if (Core::ShouldSuppressScenePicking(m_gizmoInteraction) && !m_pendingClickPickRenderPos.has_value())
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

    if ((mouseOverView || m_pendingClickPickRenderPos.has_value()) && !IsResizing())
    {
        auto& gameObjectPickingFeature = debugRenderer->GetPass<Rendering::PickingRenderPass>("Picking");
        if (!gameObjectPickingFeature.SupportsPickingReadback())
        {
            m_highlightedGameObject = nullptr;
            m_hasPickingSample = false;
            return;
        }

        if (m_pendingClickPickRenderPos.has_value() && m_pendingClickPickingSignature.has_value() &&
            gameObjectPickingFeature.GetSubmittedPickingFrameSerial() < m_pendingClickMinReadablePickingFrameSerial)
        {
            LogScenePickingDiagnostics("retry pending click after picking frame publication failed");
            m_pendingClickPickingSignature.reset();
        }

        const auto localMousePos = GetLocalViewPosition(screenMousePos);
        if (!localMousePos.has_value() && !m_pendingClickPickRenderPos.has_value())
        {
            m_highlightedGameObject = nullptr;
            m_hasPickingSample = false;
            return;
        }

        auto mousePos = m_pendingClickPickRenderPos.has_value() ? *m_pendingClickPickRenderPos : *localMousePos;
        const auto [safeWidth, safeHeight] = GetSafeSize();
        const float maxRenderX = std::max(0.0f, static_cast<float>(safeWidth) - 1.0f);
        const float maxRenderY = std::max(0.0f, static_cast<float>(safeHeight) - 1.0f);

        if (!m_pendingClickPickRenderPos.has_value())
        {
            mousePos.x = std::clamp(mousePos.x, 0.0f, maxRenderX);
            mousePos.y = NLS_SERVICE(UI::UIManager).UsesBottomLeftRenderTargetOrigin()
                ? std::clamp(static_cast<float>(safeHeight) - 1.0f - mousePos.y, 0.0f, maxRenderY)
                : std::clamp(mousePos.y, 0.0f, maxRenderY);
        }

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
        const bool queuedClickPickThisFrame = false; // Clicks are queued on release after drag classification.
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
            mouseOverView || m_pendingClickPickRenderPos.has_value(),
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
            CompleteSceneClickSelection(m_highlightedGameObject);
            m_pendingClickPickRenderPos.reset();
            m_pendingClickPickingSignature.reset();
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

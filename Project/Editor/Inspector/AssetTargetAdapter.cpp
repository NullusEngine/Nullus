#include "Inspector/AssetTargetAdapter.h"

#include <algorithm>
#include <any>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <Assets/AssetImporterFacade.h>
#include <Assets/AssetImporterSettings.h>
#include <Assets/AssetMeta.h>
#include <Components/Component.h>
#include <Math/Vector2.h>
#include <Math/Vector3.h>
#include <Math/Vector4.h>
#include <Rendering/Resources/Loaders/MaterialLoader.h>
#include <Rendering/Resources/Material.h>
#include <Rendering/Resources/Shader.h>
#include <Rendering/Resources/Texture2D.h>
#include <ServiceLocator.h>
#include "Debug/Logger.h"

#include "Core/Context.h"
#include "Core/EditorActions.h"
#include "Inspector/InspectorUndo.h"
#include "Inspector/PropertyTypeTraits.h"
#include "Inspector/ModelImporterSupport.h"
#include "Profiling/Profiler.h"
#include "Serialization/ImporterDraftSerializedBackend.h"
#include "Panels/AssetView.h"
#include "Undo/EditorUndoMutationContext.h"
#include "Undo/EditorUndoService.h"
#include "Materials/MaterialEditingService.h"

namespace NLS::Editor::Inspector
{
namespace
{
enum class DraftValueKind : uint8_t
{
    Boolean,
    SignedInteger,
    FloatingPoint,
    String
};

struct ImporterDraft
{
    std::map<std::string, std::string> settings;
    bool initialized = false;
    bool dirty = false;
    bool available = false;
    bool reimportQueued = false;
    uint64_t revision = 0u;
    std::string lastError;
};

using ReimportCompletion = std::function<void(bool, std::string)>;

std::mutex g_importerDraftMutex;
std::unordered_map<std::string, ImporterDraft> g_importerDrafts;
#if defined(NLS_ENABLE_TEST_HOOKS)
// The regular liveness check deliberately requires a real project asset. Unit
// tests can opt into an explicit in-memory target without manufacturing an
// Editor::Core::Context (which would start graphics and filesystem services).
// Tests execute this fixture only on the test thread; production never reads
// this set.
std::unordered_set<std::string> g_importerDraftTestTargets;
#endif

std::string NormalizeAssetRelativePath(std::string path)
{
    const auto separator = path.find('#');
    if (separator != std::string::npos)
        path.erase(separator);
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    if (path == "Assets")
        return {};
    constexpr std::string_view prefix = "Assets/";
    if (path.rfind(prefix, 0u) == 0u)
        path.erase(0u, prefix.size());
    return path;
}

std::filesystem::path ResolveAssetSourcePath(const InspectorTargetHandle& handle)
{
    if (!NLS::Core::ServiceLocator::Contains<Core::Context>())
        return {};

    const auto& context = NLS::Core::ServiceLocator::Get<Core::Context>();
    const auto relativePath = NormalizeAssetRelativePath(handle.resourcePath);
    if (relativePath.empty())
        return {};
    return (std::filesystem::path(context.projectAssetsPath) / relativePath).lexically_normal();
}

std::string ToEditorAssetPath(const InspectorTargetHandle& handle)
{
    const auto relativePath = NormalizeAssetRelativePath(handle.resourcePath);
    return relativePath.empty() ? std::string {} : "Assets/" + relativePath;
}

std::string DraftKey(const InspectorTargetHandle& handle)
{
    if (!handle.assetId.empty())
        return handle.assetId;
    return NormalizeAssetRelativePath(handle.resourcePath);
}

[[nodiscard]] InspectorTargetHandle MakeDurableAssetUndoTarget(InspectorTargetHandle handle)
{
    // Asset undo resolves from its GUID/path. An Inspector handle's instance ID
    // is only a frame-local cache and must not cross an asynchronous operation.
    handle.instanceId = InstanceID_None;
    return handle;
}

std::optional<NLS::Core::Assets::AssetMeta> LoadImporterMeta(const InspectorTargetHandle& handle)
{
    const auto sourcePath = ResolveAssetSourcePath(handle);
    if (sourcePath.empty())
        return std::nullopt;
    return NLS::Core::Assets::AssetMeta::Load(NLS::Core::Assets::GetAssetMetaPath(sourcePath));
}

bool IsModelImporterMeta(const NLS::Core::Assets::AssetMeta& meta)
{
    return meta.assetType == NLS::Core::Assets::AssetType::ModelScene;
}

void EnsureModelImporterDraftDefaults(
    ImporterDraft& draft,
    const NLS::Core::Assets::AssetMeta& meta)
{
    if (!IsModelImporterMeta(meta))
        return;

    static const std::map<std::string, std::string> modelDefaults {
        {"CALC_TANGENT_SPACE", "true"},
        {"JOIN_IDENTICAL_VERTICES", "true"},
        {"MAKE_LEFT_HANDED", "false"},
        {"TRIANGULATE", "true"},
        {"REMOVE_COMPONENT", "false"},
        {"GEN_NORMALS", "false"},
        {"GEN_SMOOTH_NORMALS", "true"},
        {"SPLIT_LARGE_MESHES", "false"},
        {"PRE_TRANSFORM_VERTICES", "true"},
        {"LIMIT_BONE_WEIGHTS", "false"},
        {"VALIDATE_DATA_STRUCTURE", "false"},
        {"IMPROVE_CACHE_LOCALITY", "true"},
        {"REMOVE_REDUNDANT_MATERIALS", "false"},
        {"FIX_INFACING_NORMALS", "false"},
        {"SORT_BY_PTYPE", "false"},
        {"FIND_DEGENERATES", "false"},
        {"FIND_INVALID_DATA", "true"},
        {"GEN_UV_COORDS", "true"},
        {"TRANSFORM_UV_COORDS", "false"},
        {"FIND_INSTANCES", "true"},
        {"OPTIMIZE_MESHES", "true"},
        {"OPTIMIZE_GRAPH", "true"},
        {"FLIP_UVS", "false"},
        {"FLIP_WINDING_ORDER", "false"},
        {"SPLIT_BY_BONE_COUNT", "false"},
        {"DEBONE", "true"},
        {"GLOBAL_SCALE", "true"},
        {"EMBED_TEXTURES", "false"},
        {"FORCE_GEN_NORMALS", "false"},
        {"DROP_NORMALS", "false"},
        {"GEN_BOUNDING_BOXES", "false"},
        {"DISTANCE_FIELD_GENERATION", "inherit"},
        {"DISTANCE_FIELD_RESOLUTION_SCALE", "1.0"},
        {"DISTANCE_FIELD_TWO_SIDED", "false"}
    };
    for (const auto& [key, value] : modelDefaults)
        draft.settings.try_emplace(key, value);

    auto normalizedMeta = meta;
    normalizedMeta.settings = draft.settings;
    const auto lodView = BuildModelLODInspectorView(normalizedMeta.settings);
    StoreModelLODInspectorSettings(normalizedMeta.settings, lodView);
    const auto textureSettings = Assets::LoadModelTextureResolutionSettings(normalizedMeta);
    Assets::StoreModelTextureResolutionSettings(normalizedMeta, textureSettings);
    for (const auto& [key, value] : normalizedMeta.settings)
        draft.settings.try_emplace(key, value);
}

ImporterDraft& EnsureImporterDraftLocked(const InspectorTargetHandle& handle)
{
    NLS_PROFILE_NAMED_SCOPE("Inspector::AssetTargetAdapter::EnsureImporterDraft");
    auto& draft = g_importerDrafts[DraftKey(handle)];
    if (draft.initialized)
        return draft;

    draft.initialized = true;
    if (const auto meta = LoadImporterMeta(handle); meta.has_value())
    {
        draft.settings = meta->settings;
        EnsureModelImporterDraftDefaults(draft, *meta);
        draft.available = true;
    }
    return draft;
}

bool IsImporterTargetAlive(const InspectorTargetHandle& handle)
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    if (g_importerDraftTestTargets.contains(DraftKey(handle)))
        return true;
#endif
    std::error_code error;
    return std::filesystem::is_regular_file(ResolveAssetSourcePath(handle), error) && !error;
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

DraftValueKind ClassifyDraftValue(const std::string& value)
{
    const auto lowercase = LowerAscii(value);
    if (lowercase == "true" || lowercase == "false")
        return DraftValueKind::Boolean;

    int64_t integer = 0;
    const auto [integerEnd, integerError] = std::from_chars(value.data(), value.data() + value.size(), integer);
    if (integerError == std::errc {} && integerEnd == value.data() + value.size())
        return DraftValueKind::SignedInteger;

    if (!value.empty())
    {
        char* end = nullptr;
        (void)std::strtod(value.c_str(), &end);
        if (end != value.c_str() && *end == '\0')
            return DraftValueKind::FloatingPoint;
    }
    return DraftValueKind::String;
}

NLS::Render::Resources::Material* ResolveMaterial(const InspectorTargetHandle& handle)
{
    if (!NLS::Core::ServiceLocator::Contains<Core::Context>())
        return nullptr;

    auto& context = NLS::Core::ServiceLocator::Get<Core::Context>();
    return context.materialManager[NormalizeAssetRelativePath(handle.resourcePath)];
}

NLS::Render::Resources::Material* FindRegisteredMaterial(const InspectorTargetHandle& handle)
{
    // Per-frame Inspector queries must never start a material load. The
    // serialized backend owns the one-time load; actions only operate on a
    // material that is already registered.
    if (!NLS::Core::ServiceLocator::Contains<Core::Context>())
        return nullptr;

    auto& context = NLS::Core::ServiceLocator::Get<Core::Context>();
    return context.materialManager.FindRegisteredMaterialByEquivalentArtifactPath(
        NormalizeAssetRelativePath(handle.resourcePath));
}

bool IsMaterialTargetAlive(const InspectorTargetHandle& handle)
{
    return FindRegisteredMaterial(handle) != nullptr;
}

void MarkMaterialUndoScopeSaved(const InspectorTargetHandle& handle)
{
    if (!NLS::Core::ServiceLocator::Contains<NLS::Editor::Undo::EditorUndoService>())
        return;

    NLS::Core::ServiceLocator::Get<NLS::Editor::Undo::EditorUndoService>()
        .MarkSaved(MakeInspectorUndoScope(handle));
}

void ResetMaterialUndoScopeAfterReload(const InspectorTargetHandle& handle)
{
    if (!NLS::Core::ServiceLocator::Contains<NLS::Editor::Undo::EditorUndoService>())
        return;

    auto& undo = NLS::Core::ServiceLocator::Get<NLS::Editor::Undo::EditorUndoService>();
    const auto scope = MakeInspectorUndoScope(handle);
    undo.InvalidateScope(scope, NLS::Editor::Undo::UndoInvalidationReason::Reloaded);
    undo.MarkSaved(scope);
}

void ResetImporterUndoScopeAfterReimport(const InspectorTargetHandle& handle)
{
    if (!NLS::Core::ServiceLocator::Contains<NLS::Editor::Undo::EditorUndoService>())
        return;

    auto& undo = NLS::Core::ServiceLocator::Get<NLS::Editor::Undo::EditorUndoService>();
    const auto scope = MakeInspectorUndoScope(handle);
    if (!scope.IsValid())
        return;

    // A manual reimport replaces generated data outside a command's before/
    // after snapshot. It must invalidate this durable asset scope, not the
    // currently selected Inspector target.
    undo.InvalidateScope(scope, NLS::Editor::Undo::UndoInvalidationReason::AssetReimported);
    undo.MarkSaved(scope);
}

std::filesystem::path ResolveMaterialFilePath(const InspectorTargetHandle& handle)
{
    return ResolveAssetSourcePath(handle);
}

uint64_t HashText(const std::string& value)
{
    return static_cast<uint64_t>(std::hash<std::string> {}(value));
}

bool QueueReimportAsset(
    const InspectorTargetHandle& handle,
    ReimportCompletion completion,
    std::string& error)
{
    const auto sourcePath = ResolveAssetSourcePath(handle);
    const auto editorAssetPath = ToEditorAssetPath(handle);
    if (sourcePath.empty() || editorAssetPath.empty())
    {
        error = "The asset path is no longer valid.";
        return false;
    }

    const auto importSynchronously = [&]()
    {
        if (!NLS::Core::ServiceLocator::Contains<Core::Context>())
            return false;
        const auto projectRoot = std::filesystem::path(
            NLS::Core::ServiceLocator::Get<Core::Context>().projectPath);
        NLS::Editor::Assets::AssetImporterFacade importer(
            NLS::Editor::Assets::MakeProjectEditorAssetRoots(projectRoot));
        return importer.SaveAndReimport(editorAssetPath);
    };

    if (!NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::EditorActions>())
    {
        bool imported = false;
        std::string completionError;
        try
        {
            imported = importSynchronously();
        }
        catch (const std::exception& exception)
        {
            completionError = std::string("Reimport failed: ") + exception.what();
        }
        catch (...)
        {
            completionError = "Reimport failed with an unknown error.";
        }
        if (!imported && completionError.empty())
            completionError = "Reimport failed. The importer settings draft is still available.";
        if (completion)
            completion(imported, completionError);
        // A synchronous importer failure was still dispatched to its
        // completion. Returning success here makes the caller use the same
        // Pending/completion state machine as the background path instead of
        // attempting a second rollback after the completion has run.
        return true;
    }

    // Context already owns the project root. projectAssetsPath ends in a
    // separator, so parent_path() on it still names Assets rather than its parent.
    const auto projectRoot = std::filesystem::path(EDITOR_EXEC(GetContext()).projectPath);
    if (projectRoot.empty())
    {
        error = "The project root is no longer available.";
        return false;
    }

    auto residentPrefabPreviewRegistry = EDITOR_EXEC(GetContext()).residentPrefabPreviewRegistry;
    const auto queued = EDITOR_EXEC(TrackBackgroundTask([
        projectRoot,
        editorAssetPath,
        residentPrefabPreviewRegistry = std::move(residentPrefabPreviewRegistry),
        completion]() mutable
    {
        bool imported = false;
        std::string completionError;
        try
        {
            NLS::Editor::Assets::AssetImporterFacade importer(
                NLS::Editor::Assets::MakeProjectEditorAssetRoots(projectRoot));
            importer.SetResidentPrefabPreviewRegistry(residentPrefabPreviewRegistry);
            // The task must not retain Context-owned objects (in particular
            // ImportProgressTracker) across frames. Inspector state already
            // exposes the pending operation; the durable command completion
            // is the authoritative progress boundary.
            imported = importer.SaveAndReimport(editorAssetPath);
            if (!imported)
                for (const auto& diagnostic : importer.GetDiagnostics())
                    if (diagnostic.severity == NLS::Core::Assets::AssetDiagnosticSeverity::Error)
                        completionError += diagnostic.code + ": " + diagnostic.message + "\n";
        }
        catch (const std::exception& exception)
        {
            completionError = std::string("Reimport failed: ") + exception.what();
        }
        catch (...)
        {
            completionError = "Reimport failed with an unknown error.";
        }
        if (!imported && completionError.empty())
            completionError = "Reimport failed. The importer settings draft is still available.";
        // EditorActions owns and drains tracked background work before it is
        // destroyed. Resolve it only at dispatch time so this task captures no
        // raw EditorActions/Context pointer.
        if (NLS::Core::ServiceLocator::Contains<NLS::Editor::Core::EditorActions>())
        {
            NLS::Core::ServiceLocator::Get<NLS::Editor::Core::EditorActions>().DelayAction([
                completion = std::move(completion),
                imported,
                completionError = std::move(completionError)]() mutable
            {
                if (completion)
                    completion(imported, std::move(completionError));
            });
        }
    }));
    if (!queued)
        error = "The editor background task queue is full; the importer draft was kept.";
    return queued;
}

void CompleteImporterReimport(
    const InspectorTargetHandle& handle,
    const uint64_t operationRevision,
    const std::optional<NLS::Core::Assets::AssetMeta>& rollbackMeta,
    const std::filesystem::path& rollbackMetaPath,
    const std::optional<std::map<std::string, std::string>>& appliedSettings,
    const bool imported,
    std::string error)
{
    // A delayed completion can arrive after a target was invalidated or after a
    // rejected queue attempt already cleared this operation. Do not let it
    // overwrite a newer draft.
    {
        std::lock_guard lock(g_importerDraftMutex);
        const auto found = g_importerDrafts.find(DraftKey(handle));
        if (found == g_importerDrafts.end() ||
            !found->second.reimportQueued ||
            found->second.revision != operationRevision)
        {
            return;
        }
    }

    bool rollbackSucceeded = true;
    if (!imported && rollbackMeta.has_value())
        rollbackSucceeded = rollbackMeta->Save(rollbackMetaPath);

    bool reimportSucceeded = false;
    {
        std::lock_guard lock(g_importerDraftMutex);
        const auto found = g_importerDrafts.find(DraftKey(handle));
        if (found == g_importerDrafts.end() ||
            !found->second.reimportQueued ||
            found->second.revision != operationRevision)
        {
            return;
        }

        auto& draft = found->second;
        draft.reimportQueued = false;
        ++draft.revision;
        if (appliedSettings.has_value())
        {
            draft.settings = *appliedSettings;
            draft.dirty = !imported;
            draft.available = true;
        }

        if (imported)
        {
            draft.lastError.clear();
            reimportSucceeded = true;
        }
        else
        {
            if (error.empty())
                error = "Reimport failed. The importer settings draft is still available.";
            if (!rollbackSucceeded)
                error += " The original importer metadata could not be restored.";
            draft.lastError = std::move(error);
        }
    }

    if (reimportSucceeded)
        ResetImporterUndoScopeAfterReimport(handle);
}

struct ImporterApplyUndoState
{
    InspectorTargetHandle handle;
    NLS::Core::Assets::AssetMeta before;
    NLS::Core::Assets::AssetMeta after;
    // This is updated from disk after every successful import because an
    // importer may normalize its metadata while writing the artifact.
    NLS::Core::Assets::AssetMeta expectedCurrent;
    struct SourceFingerprint
    {
        uintmax_t size = 0u;
        std::filesystem::file_time_type writeTime {};

        [[nodiscard]] bool operator==(const SourceFingerprint& other) const noexcept
        {
            return size == other.size && writeTime == other.writeTime;
        }
    };
    std::optional<SourceFingerprint> sourceFingerprint;
    uint64_t operationGeneration = 0u;
    uint64_t activeOperationGeneration = 0u;
    NLS::Editor::Undo::UndoPendingToken activePendingToken = 0u;
    uint64_t activeDraftRevision = 0u;
    bool operationActive = false;
};

std::optional<ImporterApplyUndoState::SourceFingerprint> ReadImporterSourceFingerprint(
    const InspectorTargetHandle& handle)
{
    std::error_code error;
    const auto sourcePath = ResolveAssetSourcePath(handle);
    if (sourcePath.empty() || !std::filesystem::is_regular_file(sourcePath, error) || error)
        return std::nullopt;

    const auto size = std::filesystem::file_size(sourcePath, error);
    if (error)
        return std::nullopt;
    const auto writeTime = std::filesystem::last_write_time(sourcePath, error);
    if (error)
        return std::nullopt;
    return ImporterApplyUndoState::SourceFingerprint {size, writeTime};
}

void ClearImporterApplyOperation(
    const std::shared_ptr<ImporterApplyUndoState>& state,
    const uint64_t operationGeneration,
    const NLS::Editor::Undo::UndoPendingToken pendingToken,
    const uint64_t draftRevision)
{
    if (state == nullptr || state->activeOperationGeneration != operationGeneration ||
        state->activePendingToken != pendingToken || state->activeDraftRevision != draftRevision)
    {
        return;
    }
    state->operationActive = false;
    state->activePendingToken = 0u;
    state->activeDraftRevision = 0u;
}

bool IsImporterApplyOperationCurrent(
    const std::shared_ptr<ImporterApplyUndoState>& state,
    const uint64_t operationGeneration,
    const NLS::Editor::Undo::UndoPendingToken pendingToken,
    const uint64_t draftRevision)
{
    if (state == nullptr || !state->operationActive ||
        state->activeOperationGeneration != operationGeneration ||
        state->activePendingToken != pendingToken ||
        state->activeDraftRevision != draftRevision)
    {
        return false;
    }

    std::lock_guard lock(g_importerDraftMutex);
    const auto found = g_importerDrafts.find(DraftKey(state->handle));
    return found != g_importerDrafts.end() &&
        found->second.reimportQueued &&
        found->second.revision == draftRevision;
}

void AbandonInvalidatedImporterApplyOperation(
    const std::shared_ptr<ImporterApplyUndoState>& state,
    const uint64_t operationGeneration,
    const NLS::Editor::Undo::UndoPendingToken pendingToken,
    const uint64_t draftRevision)
{
    if (state == nullptr || state->activeOperationGeneration != operationGeneration ||
        state->activePendingToken != pendingToken || state->activeDraftRevision != draftRevision)
    {
        return;
    }

    // Scope invalidation wins over an old asynchronous import. Do not restore
    // a snapshot here: the asset may already have been reloaded or replaced by
    // a newer operation. Only release the transient draft lock.
    {
        std::lock_guard lock(g_importerDraftMutex);
        const auto found = g_importerDrafts.find(DraftKey(state->handle));
        if (found != g_importerDrafts.end() && found->second.reimportQueued &&
            found->second.revision == draftRevision)
        {
            auto& draft = found->second;
            draft.reimportQueued = false;
            ++draft.revision;
            draft.available = true;
            draft.dirty = false;
            draft.lastError = "The importer operation was invalidated before completion.";
        }
    }

    ClearImporterApplyOperation(state, operationGeneration, pendingToken, draftRevision);
}

void FinishImporterApplyDraftFailure(
    const std::shared_ptr<ImporterApplyUndoState>& state,
    const uint64_t operationGeneration,
    const NLS::Editor::Undo::UndoPendingToken pendingToken,
    const uint64_t draftRevision,
    const NLS::Editor::Undo::UndoDirection direction,
    const NLS::Core::Assets::AssetMeta& attempted,
    const NLS::Core::Assets::AssetMeta& restored,
    std::string error)
{
    if (state == nullptr || state->activeOperationGeneration != operationGeneration ||
        state->activePendingToken != pendingToken || state->activeDraftRevision != draftRevision)
    {
        return;
    }

    {
        std::lock_guard lock(g_importerDraftMutex);
        const auto found = g_importerDrafts.find(DraftKey(state->handle));
        if (found != g_importerDrafts.end() && found->second.reimportQueued &&
            found->second.revision == draftRevision)
        {
            auto& draft = found->second;
            draft.reimportQueued = false;
            ++draft.revision;
            draft.available = true;

            if (direction == NLS::Editor::Undo::UndoDirection::Redo)
            {
                // The command was not recorded (or Redo did not advance), so
                // keep the requested after-state as a retryable Inspector
                // draft.
                draft.settings = attempted.settings;
                EnsureModelImporterDraftDefaults(draft, attempted);
                draft.dirty = true;
            }
            else
            {
                // Undo failed and the history cursor remains on the
                // after-state. Do not present a second conflicting edit; leave
                // the restored committed state visible so the same Undo
                // operation can retry.
                draft.settings = restored.settings;
                EnsureModelImporterDraftDefaults(draft, restored);
                draft.dirty = false;
            }

            if (error.empty())
                error = "Reimport failed. The importer settings draft is still available.";
            draft.lastError = std::move(error);
        }
    }

    ClearImporterApplyOperation(state, operationGeneration, pendingToken, draftRevision);
}

void CompleteImporterApplyUndo(
    const std::shared_ptr<ImporterApplyUndoState>& state,
    const NLS::Editor::Undo::UndoExecutionContext& context,
    const uint64_t operationGeneration,
    const NLS::Editor::Undo::UndoPendingToken pendingToken,
    const uint64_t draftRevision,
    const NLS::Editor::Undo::UndoDirection direction,
    const NLS::Core::Assets::AssetMeta& attempted,
    const NLS::Core::Assets::AssetMeta& restored,
    const std::filesystem::path& metaPath,
    const bool imported,
    std::string error,
    NLS::Editor::Undo::UndoCompletion completion)
{
    // Reimport completes after the command's initial Apply scope has returned.
    // Establish the original mutation context and retain the service-owned
    // validity lease for every delayed metadata/draft mutation. Scope
    // invalidation closes the lease gate before it returns, so a stale worker
    // completion cannot overwrite a newly reloaded asset.
    const NLS::Editor::Undo::ScopedEditorUndoMutationContext mutationContext(context);
    auto mutationLease = context.TryAcquireOperationMutation();
    if (!mutationLease)
    {
        NLS_LOG_ERROR("Importer Apply completion invalidated: " + metaPath.generic_string());
        AbandonInvalidatedImporterApplyOperation(state, operationGeneration, pendingToken, draftRevision);
        if (completion)
        {
            completion(NLS::Editor::Undo::UndoCompletionResult::Failed(
                "The importer reimport completion was invalidated."));
        }
        return;
    }
    if (!IsImporterApplyOperationCurrent(state, operationGeneration, pendingToken, draftRevision))
    {
        NLS_LOG_ERROR("Importer Apply completion stale: " + metaPath.generic_string());
        ClearImporterApplyOperation(state, operationGeneration, pendingToken, draftRevision);
        mutationLease.Release();
        if (completion)
        {
            completion(NLS::Editor::Undo::UndoCompletionResult::Failed(
                "The importer reimport completion is stale."));
        }
        return;
    }

    bool completed = imported;
    bool rollbackSucceeded = true;
    std::optional<NLS::Core::Assets::AssetMeta> persisted;
    if (completed)
    {
        persisted = NLS::Core::Assets::AssetMeta::Load(metaPath);
        if (!persisted.has_value() || persisted->id != restored.id)
        {
            completed = false;
            error = "The reimport completed without valid importer metadata.";
        }
    }

    if (!completed)
    {
        // AssetImporterFacade restores the metadata it read immediately before
        // import. Restore the opposite command snapshot explicitly so a failed
        // initial Apply/Redo never leaves a half-committed meta file behind.
        if (!context.IsOperationCurrent())
        {
            mutationLease.Release();
            AbandonInvalidatedImporterApplyOperation(state, operationGeneration, pendingToken, draftRevision);
            if (completion)
            {
                completion(NLS::Editor::Undo::UndoCompletionResult::Failed(
                    "The importer reimport completion was invalidated."));
            }
            return;
        }
        rollbackSucceeded = restored.Save(metaPath);
        if (error.empty())
            error = "Reimport failed. The importer settings draft is still available.";
        NLS_LOG_ERROR("Importer Apply failed: " + metaPath.generic_string() + ": " + error);
        if (!rollbackSucceeded)
            error += " The previous importer metadata could not be restored.";

        FinishImporterApplyDraftFailure(
            state,
            operationGeneration,
            pendingToken,
            draftRevision,
            direction,
            attempted,
            restored,
            error);
        mutationLease.Release();
        if (completion)
            completion(NLS::Editor::Undo::UndoCompletionResult::Failed(std::move(error), !rollbackSucceeded));
        return;
    }

    bool superseded = false;
    {
        std::lock_guard lock(g_importerDraftMutex);
        const auto found = g_importerDrafts.find(DraftKey(state->handle));
        if (found == g_importerDrafts.end() || !found->second.reimportQueued ||
            found->second.revision != draftRevision || !state->operationActive ||
            state->activeOperationGeneration != operationGeneration ||
            state->activePendingToken != pendingToken)
        {
            superseded = true;
        }
        else
        {
            auto& draft = found->second;
            draft.reimportQueued = false;
            ++draft.revision;
            draft.available = true;
            draft.settings = persisted->settings;
            EnsureModelImporterDraftDefaults(draft, *persisted);
            draft.dirty = false;
            draft.lastError.clear();

            if (direction == NLS::Editor::Undo::UndoDirection::Redo)
                state->after = *persisted;
            else
                state->before = *persisted;
            state->expectedCurrent = *persisted;
            ClearImporterApplyOperation(state, operationGeneration, pendingToken, draftRevision);
        }
    }

    if (superseded)
    {
        ClearImporterApplyOperation(state, operationGeneration, pendingToken, draftRevision);
        mutationLease.Release();
        if (completion)
        {
            completion(NLS::Editor::Undo::UndoCompletionResult::Failed(
                "The importer reimport completion was superseded."));
        }
        return;
    }

    mutationLease.Release();
    if (completion)
        completion(NLS::Editor::Undo::UndoCompletionResult::Succeeded());
}

class ImporterApplyUndoCommand final : public NLS::Editor::Undo::IEditorUndoCommand
{
public:
    ImporterApplyUndoCommand(
        NLS::Editor::Undo::UndoCommandInfo info,
        InspectorTargetHandle handle,
        NLS::Core::Assets::AssetMeta before,
        NLS::Core::Assets::AssetMeta after)
        : m_info(std::move(info))
        , m_state(std::make_shared<ImporterApplyUndoState>())
    {
        m_state->handle = MakeDurableAssetUndoTarget(std::move(handle));
        m_state->before = std::move(before);
        m_state->after = std::move(after);
        // Establish the durable initial pre-Apply snapshot here. Successful
        // import completions replace it with the importer-normalized version.
        m_state->expectedCurrent = m_state->before;
        m_state->sourceFingerprint = ReadImporterSourceFingerprint(m_state->handle);
    }

    [[nodiscard]] NLS::Editor::Undo::UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    [[nodiscard]] bool MayCompleteAsynchronously() const override
    {
        return true;
    }

    [[nodiscard]] NLS::Editor::Undo::UndoValidationResult Validate(
        NLS::Editor::Undo::UndoDirection,
        const NLS::Editor::Undo::UndoExecutionContext&) const override
    {
        if (m_state == nullptr || m_state->operationActive)
        {
            return NLS::Editor::Undo::UndoValidationResult::Conflict(
                "The importer command is already reimporting this asset.");
        }
        if (!IsImporterTargetAlive(m_state->handle))
        {
            return NLS::Editor::Undo::UndoValidationResult::Stale(
                "The importer source asset no longer exists.");
        }

        const auto sourceFingerprint = ReadImporterSourceFingerprint(m_state->handle);
        if (!m_state->sourceFingerprint.has_value() || !sourceFingerprint.has_value() ||
            *sourceFingerprint != *m_state->sourceFingerprint)
        {
            return NLS::Editor::Undo::UndoValidationResult::Conflict(
                "The source asset changed outside the Undo timeline.");
        }

        const auto current = LoadImporterMeta(m_state->handle);
        if (!current.has_value() || current->id != m_state->before.id)
        {
            return NLS::Editor::Undo::UndoValidationResult::Stale(
                "The importer metadata now belongs to a different asset.");
        }
        if (*current != m_state->expectedCurrent)
        {
            return NLS::Editor::Undo::UndoValidationResult::Conflict(
                "The importer metadata changed outside the Undo timeline.");
        }

        std::lock_guard lock(g_importerDraftMutex);
        const auto& draft = EnsureImporterDraftLocked(m_state->handle);
        const auto editorAssetPath = ToEditorAssetPath(m_state->handle);
        if (draft.reimportQueued ||
            (!editorAssetPath.empty() && NLS::Editor::Assets::AssetImporterFacade::IsReimportInProgress(editorAssetPath)))
        {
            return NLS::Editor::Undo::UndoValidationResult::Conflict(
                "The importer is already reimporting this asset.");
        }
        return NLS::Editor::Undo::UndoValidationResult::Ready();
    }

    NLS::Editor::Undo::UndoStartResult Apply(
        const NLS::Editor::Undo::UndoDirection direction,
        const NLS::Editor::Undo::UndoExecutionContext& context,
        NLS::Editor::Undo::UndoCompletion completion) override
    {
        const auto validation = Validate(direction, context);
        if (validation.state != NLS::Editor::Undo::UndoValidationState::Ready)
            return NLS::Editor::Undo::UndoStartResult::Failed(validation.message);

        const auto attempted = direction == NLS::Editor::Undo::UndoDirection::Redo
            ? m_state->after
            : m_state->before;
        const auto restored = m_state->expectedCurrent;
        const auto sourcePath = ResolveAssetSourcePath(m_state->handle);
        const auto metaPath = NLS::Core::Assets::GetAssetMetaPath(sourcePath);
        const auto editorAssetPath = ToEditorAssetPath(m_state->handle);
        const auto distanceFieldResolution = attempted.settings.find("DISTANCE_FIELD_RESOLUTION_SCALE");
        NLS_LOG_INFO("Importer Apply: " + editorAssetPath + " distanceFieldResolution=" +
            (distanceFieldResolution == attempted.settings.end() ? std::string("inherit") : distanceFieldResolution->second));
        // Saving the draft wakes the file watcher before the background task
        // starts. Reserve the import through its completion so that watcher
        // work cannot import the intermediate metadata or invalidate this Undo.
        auto reimportGuard = std::make_shared<NLS::Editor::Assets::AssetImporterFacade::ScopedReimportInProgress>(editorAssetPath);
        if (sourcePath.empty() || !attempted.Save(metaPath))
        {
            constexpr std::string_view message = "The importer metadata could not be saved for reimport.";
            {
                std::lock_guard lock(g_importerDraftMutex);
                auto& draft = EnsureImporterDraftLocked(m_state->handle);
                draft.lastError = message;
            }
            return NLS::Editor::Undo::UndoStartResult::Failed(std::string(message));
        }

        uint64_t draftRevision = 0u;
        {
            std::lock_guard lock(g_importerDraftMutex);
            auto& draft = EnsureImporterDraftLocked(m_state->handle);
            if (draft.reimportQueued)
            {
                const bool rollbackSucceeded = restored.Save(metaPath);
                return NLS::Editor::Undo::UndoStartResult::Failed(
                    "The importer began reimporting before the Undo command could start.",
                    !rollbackSucceeded);
            }
            draft.reimportQueued = true;
            draft.lastError.clear();
            draftRevision = ++draft.revision;
        }

        const uint64_t operationGeneration = ++m_state->operationGeneration;
        m_state->activeOperationGeneration = operationGeneration;
        m_state->activePendingToken = context.pendingToken;
        m_state->activeDraftRevision = draftRevision;
        m_state->operationActive = true;

        std::string error;
        const auto queued = QueueReimportAsset(
            m_state->handle,
            [
                state = m_state,
                context,
                operationGeneration,
                pendingToken = context.pendingToken,
                draftRevision,
                direction,
                attempted,
                restored,
                metaPath,
                reimportGuard,
                completion = std::move(completion)
            ](const bool imported, std::string completionError) mutable
            {
                CompleteImporterApplyUndo(
                    state,
                    context,
                    operationGeneration,
                    pendingToken,
                    draftRevision,
                    direction,
                    attempted,
                    restored,
                    metaPath,
                    imported,
                    std::move(completionError),
                    std::move(completion));
            },
            error);
        if (queued)
            return NLS::Editor::Undo::UndoStartResult::Pending();

        const bool rollbackSucceeded = restored.Save(metaPath);
        FinishImporterApplyDraftFailure(
            m_state,
            operationGeneration,
            context.pendingToken,
            draftRevision,
            direction,
            attempted,
            restored,
            error);
        return NLS::Editor::Undo::UndoStartResult::Failed(
            error.empty() ? "The importer reimport could not be queued." : std::move(error),
            !rollbackSucceeded);
    }

    bool TryMergeFrom(const NLS::Editor::Undo::IEditorUndoCommand&) override
    {
        // Importing writes an artifact and is intentionally never time-merged.
        return false;
    }

private:
    NLS::Editor::Undo::UndoCommandInfo m_info;
    std::shared_ptr<ImporterApplyUndoState> m_state;
};
} // namespace

namespace ImporterDraftStreamDetail
{
struct Snapshot
{
    std::map<std::string, std::string> settings;
    bool available = false;
    bool reimporting = false;
    uint64_t revision = 0u;
};

Snapshot Read(const InspectorTargetHandle& handle)
{
    std::lock_guard lock(g_importerDraftMutex);
    const auto& draft = EnsureImporterDraftLocked(handle);
    return {draft.settings,
        draft.available && IsImporterTargetAlive(handle),
        draft.reimportQueued ||
            NLS::Editor::Assets::AssetImporterFacade::IsReimportInProgress(ToEditorAssetPath(handle)),
        draft.revision};
}

bool Write(const InspectorTargetHandle& handle, std::map<std::string, std::string> settings)
{
    if (!IsImporterTargetAlive(handle))
        return false;

    std::map<std::string, std::string> persisted;
    bool hasPersistedSettings = false;
    if (const auto meta = LoadImporterMeta(handle); meta.has_value())
    {
        ImporterDraft baseline;
        baseline.settings = meta->settings;
        EnsureModelImporterDraftDefaults(baseline, *meta);
        persisted = std::move(baseline.settings);
        hasPersistedSettings = true;
    }

    std::lock_guard lock(g_importerDraftMutex);
    auto& draft = EnsureImporterDraftLocked(handle);
    if (!draft.available || draft.reimportQueued ||
        NLS::Editor::Assets::AssetImporterFacade::IsReimportInProgress(ToEditorAssetPath(handle)))
    {
        return false;
    }
    if (draft.settings == settings)
        return true;

    draft.settings = std::move(settings);
    draft.dirty = !hasPersistedSettings || draft.settings != persisted;
    draft.lastError.clear();
    ++draft.revision;
    return true;
}

uint64_t SchemaSignature(const Snapshot& snapshot)
{
    constexpr uint64_t offset = 1469598103934665603ULL;
    constexpr uint64_t prime = 1099511628211ULL;
    uint64_t hash = offset;
    const auto append = [&hash](const std::string_view text)
    {
        for (const auto character : text)
        {
            hash ^= static_cast<uint8_t>(character);
            hash *= prime;
        }
    };
    for (const auto& [key, value] : snapshot.settings)
    {
        append(key);
        hash ^= static_cast<uint64_t>(ClassifyDraftValue(value));
        hash *= prime;
    }
    return hash == 0u ? 1u : hash;
}

bool IsCompatible(const Snapshot& left, const Snapshot& right)
{
    if (!left.available || !right.available || left.settings.size() != right.settings.size())
        return false;
    auto leftIt = left.settings.begin();
    auto rightIt = right.settings.begin();
    for (; leftIt != left.settings.end(); ++leftIt, ++rightIt)
    {
        if (leftIt->first != rightIt->first ||
            ClassifyDraftValue(leftIt->second) != ClassifyDraftValue(rightIt->second))
        {
            return false;
        }
    }
    return true;
}
} // namespace ImporterDraftStreamDetail

bool AssetTargetAdapter::CanBuild(const InspectorTargetSet& targets) const
{
    const auto* target = targets.Primary();
    return target != nullptr &&
        (target->kind == InspectorTargetKind::AssetImporter || target->kind == InspectorTargetKind::Material);
}

uint64_t AssetTargetAdapter::GetStructureVersion(const InspectorTargetSet& targets) const
{
    NLS_PROFILE_NAMED_SCOPE("Inspector::AssetTargetAdapter::GetStructureVersion");
    const auto* target = targets.Primary();
    if (target == nullptr)
        return 0u;

    uint64_t version = HashText(target->resourcePath) ^ (static_cast<uint64_t>(target->kind) << 56u);
    if (target->kind == InspectorTargetKind::Material)
    {
        if (auto* material = FindRegisteredMaterial(*target))
        {
            version ^= reinterpret_cast<uintptr_t>(material);
            for (const auto& [name, value] : material->GetUniformsData())
            {
                version ^= HashText(name);
                version ^= static_cast<uint64_t>(value.type().hash_code());
            }
        }
        return version;
    }

    std::error_code error;
    const auto sourcePath = ResolveAssetSourcePath(*target);
    const auto metaPath = NLS::Core::Assets::GetAssetMetaPath(sourcePath);
    if (std::filesystem::exists(metaPath, error) && !error)
    {
        version ^= static_cast<uint64_t>(std::filesystem::file_size(metaPath, error));
        error.clear();
        version ^= static_cast<uint64_t>(std::filesystem::last_write_time(metaPath, error).time_since_epoch().count());
    }
    {
        std::lock_guard lock(g_importerDraftMutex);
        version ^= EnsureImporterDraftLocked(*target).revision;
    }
    return version;
}

InspectorPresentation AssetTargetAdapter::BuildPresentation(
    const InspectorTargetSet& targets,
    const InspectorBuildContext& context) const
{
    NLS_PROFILE_NAMED_SCOPE("Inspector::AssetTargetAdapter::BuildPresentation");
    InspectorPresentation presentation;
    auto& tree = presentation;
    tree.targets = targets;
    tree.structureVersion = GetStructureVersion(targets);
    const auto* target = targets.Primary();
    if (target == nullptr)
        return tree;

    // Asset presentation is UI-only. All material and importer
    // values are read and committed through their byte-stream backends.

    PropertyNode root;
    root.path = PropertyPath(target->kind == InspectorTargetKind::Material ? "Material" : "Importer");
    root.displayName = std::filesystem::path(NormalizeAssetRelativePath(target->resourcePath)).filename().string();
    root.kind = InspectorPropertyKind::Struct;
    root.target = *target;
    root.structuralVersion = tree.structureVersion;
    SetPropertyNodeTargets(root, tree.targets.targets);

    // All fields now come from InspectorProjectionBuilder and the canonical
    // byte-stream SerializedObject. Target adapters retain only chrome and
    // target actions; the old field construction below is retired and kept
    // unreachable temporarily until its asset-specific helper cleanup lands.
    if (root.displayName.empty())
        root.displayName = target->kind == InspectorTargetKind::Material ? "Material" : "Importer";
    tree.roots.push_back(std::move(root));
    return tree;

}

std::vector<InspectorTargetAction> AssetTargetAdapter::GetActions(const InspectorTargetSet& targets) const
{
    NLS_PROFILE_NAMED_SCOPE("Inspector::AssetTargetAdapter::GetActions");
    const auto* target = targets.Primary();
    if (target == nullptr)
        return {};

    if (target->kind == InspectorTargetKind::Material)
    {
        const bool alive = IsMaterialTargetAlive(*target);
        return {
            {InspectorTargetActionKind::Save, "Save", "Write the current material parameters to disk.", alive, true},
            {InspectorTargetActionKind::Apply, "Apply to Scene", "Publish the compiled material version to the scene.", alive, false},
            {InspectorTargetActionKind::Reload, "Reload", "Discard unsaved material changes and reload the file.", alive, false},
            {InspectorTargetActionKind::Preview, "Preview", "Open the live material preview.", alive, false}
        };
    }

    std::lock_guard lock(g_importerDraftMutex);
    const auto& draft = EnsureImporterDraftLocked(*target);
    const bool alive = draft.available && IsImporterTargetAlive(*target);
    const auto editorPath = ToEditorAssetPath(*target);
    const bool reimporting = draft.reimportQueued ||
        (!editorPath.empty() && NLS::Editor::Assets::AssetImporterFacade::IsReimportInProgress(editorPath));
    if (reimporting)
    {
        return {
            {InspectorTargetActionKind::Apply, "Apply", "Reimport is already in progress.", false, true},
            {InspectorTargetActionKind::Revert, "Revert", "Reimport is already in progress.", false, false},
            {InspectorTargetActionKind::Reimport, "Reimporting...", "The importer is reimporting this asset.", false, false}
        };
    }
    return {
        {InspectorTargetActionKind::Apply, "Apply", "Save the importer draft and reimport the asset.", alive && draft.dirty && !reimporting, true},
        {InspectorTargetActionKind::Revert, "Revert", "Discard unsaved importer changes.", alive && draft.dirty && !reimporting, false},
        {InspectorTargetActionKind::Reimport, "Reimport", "Reimport the current saved importer settings.", alive && !draft.dirty && !reimporting, false}
    };
}

bool AssetTargetAdapter::ExecuteAction(
    const InspectorTargetSet& targets,
    const InspectorTargetActionKind action,
    std::string& error) const
{
    const auto* target = targets.Primary();
    if (target == nullptr)
    {
        error = "There is no active Inspector target.";
        return false;
    }

    if (target->kind == InspectorTargetKind::Material)
    {
        if (NLS::Core::ServiceLocator::Contains<Materials::MaterialEditingService>())
        {
            auto& service=NLS::Core::ServiceLocator::Get<Materials::MaterialEditingService>();
            if (const auto session=service.OpenTarget(target->assetId,target->resourcePath,error))
            {
                switch(action)
                {
                case InspectorTargetActionKind::Save:
                    return service.Save(*session);
                case InspectorTargetActionKind::Apply:
                    return service.ApplyToScene(*session);
                case InspectorTargetActionKind::Reload:
                    if (!session->document.DiscardWorking()) {error=session->document.Error();return false;}
                    session->failedSource.clear();service.RequestPreview(*session);return true;
                case InspectorTargetActionKind::Preview:
                    if (session->preview)
                    {auto& view=EDITOR_PANEL(NLS::Editor::Panels::AssetView,"Asset View");view.SetOwnedMaterial(session->preview);view.Open();return true;}
                    error="The material preview is still compiling";return false;
                default: break;
                }
            }
        }
        auto* material = ResolveMaterial(*target);
        const auto filePath = ResolveMaterialFilePath(*target);
        if (material == nullptr || filePath.empty())
        {
            error = "The selected material is no longer available.";
            return false;
        }

        switch (action)
        {
        case InspectorTargetActionKind::Save:
            if (!NLS::Render::Resources::Loaders::MaterialLoader::TrySave(*material, filePath.string()))
            {
                error = "The material could not be saved.";
                return false;
            }
            MarkMaterialUndoScopeSaved(*target);
            return true;
        case InspectorTargetActionKind::Reload:
            if (!NLS::Render::Resources::Loaders::MaterialLoader::TryReload(*material, filePath.string()))
            {
                error = "The material could not be reloaded.";
                return false;
            }
            ResetMaterialUndoScopeAfterReload(*target);
            return true;
        case InspectorTargetActionKind::Preview:
        {
            auto& assetView = EDITOR_PANEL(NLS::Editor::Panels::AssetView, "Asset View");
            assetView.SetResource(material);
            assetView.Open();
            assetView.Focus();
            return true;
        }
        default:
            error = "That operation is not valid for a material.";
            return false;
        }
    }

    if (target->kind != InspectorTargetKind::AssetImporter)
    {
        error = "The selected target is not an asset importer.";
        return false;
    }

    if (action == InspectorTargetActionKind::Revert)
    {
        std::lock_guard lock(g_importerDraftMutex);
        auto& draft = EnsureImporterDraftLocked(*target);
        const auto editorAssetPath = ToEditorAssetPath(*target);
        if (draft.reimportQueued ||
            (!editorAssetPath.empty() && NLS::Editor::Assets::AssetImporterFacade::IsReimportInProgress(editorAssetPath)))
        {
            error = "Importer settings cannot be reverted while reimport is in progress.";
            return false;
        }
        const auto meta = LoadImporterMeta(*target);
        if (!meta.has_value())
        {
            error = "The importer metadata could not be reloaded.";
            return false;
        }
        draft.settings = meta->settings;
        EnsureModelImporterDraftDefaults(draft, *meta);
        draft.available = true;
        draft.dirty = false;
        draft.lastError.clear();
        ++draft.revision;
        return true;
    }

    if (action == InspectorTargetActionKind::Reimport)
    {
        const auto handle = MakeDurableAssetUndoTarget(*target);
        const auto editorAssetPath = ToEditorAssetPath(handle);
        uint64_t operationRevision = 0u;
        {
            std::lock_guard lock(g_importerDraftMutex);
            auto& draft = EnsureImporterDraftLocked(handle);
            if (!draft.available || !IsImporterTargetAlive(handle))
            {
                error = "The selected asset importer is no longer available.";
                return false;
            }
            if (draft.dirty)
            {
                error = "Apply or revert the importer draft before reimporting.";
                return false;
            }
            if (draft.reimportQueued ||
                (!editorAssetPath.empty() && NLS::Editor::Assets::AssetImporterFacade::IsReimportInProgress(editorAssetPath)))
            {
                error = "Reimport is already in progress.";
                return false;
            }
            draft.reimportQueued = true;
            draft.lastError.clear();
            operationRevision = ++draft.revision;
        }

        const ReimportCompletion completion = [handle, operationRevision](
            const bool imported,
            std::string completionError)
        {
            CompleteImporterReimport(
                handle,
                operationRevision,
                std::nullopt,
                {},
                std::nullopt,
                imported,
                std::move(completionError));
        };
        if (!QueueReimportAsset(handle, completion, error))
        {
            completion(false, error);
            return false;
        }
        return true;
    }

    if (action != InspectorTargetActionKind::Apply)
    {
        error = "That operation is not valid for an importer.";
        return false;
    }

    const auto handle = MakeDurableAssetUndoTarget(*target);
    NLS::Core::Assets::AssetMeta original;
    NLS_LOG_INFO("Importer Apply requested: " + handle.resourcePath);
    std::map<std::string, std::string> draftSettings;
    {
        std::lock_guard lock(g_importerDraftMutex);
        auto& draft = EnsureImporterDraftLocked(handle);
        const auto meta = LoadImporterMeta(handle);
        const auto editorAssetPath = ToEditorAssetPath(handle);
        if (!draft.available || !draft.dirty || !meta.has_value())
        {
            error = "There are no importer changes to apply.";
            NLS_LOG_ERROR("Importer Apply rejected: " + handle.resourcePath + ": " + error);
            return false;
        }
        if (draft.reimportQueued ||
            (!editorAssetPath.empty() && NLS::Editor::Assets::AssetImporterFacade::IsReimportInProgress(editorAssetPath)))
        {
            error = "Importer settings cannot be applied while reimport is in progress.";
            NLS_LOG_ERROR("Importer Apply rejected: " + handle.resourcePath + ": " + error);
            return false;
        }
        original = *meta;
        draftSettings = draft.settings;
    }

    auto updated = original;
    updated.settings = draftSettings;
    if (!NLS::Core::ServiceLocator::Contains<NLS::Editor::Undo::EditorUndoService>())
    {
        error = "The global Undo service is unavailable; importer settings were not applied.";
        return false;
    }

    auto info = BuildInspectorUndoCommandInfo(
        "Apply Importer Settings",
        {handle},
        PropertyPath("Importer.Settings"));
    info.affected.push_back(NLS::Core::Assets::GetAssetMetaPath(ResolveAssetSourcePath(handle)).generic_string());
    const auto scope = MakeInspectorUndoScope(handle);
    if (scope.IsValid())
        info.writeThroughScopes.push_back(scope);
    info.estimatedBytes = sizeof(ImporterApplyUndoCommand) +
        sizeof(NLS::Core::Assets::AssetMeta) * 2u;
    for (const auto& [key, value] : original.settings)
        info.estimatedBytes += key.size() + value.size();
    for (const auto& [key, value] : updated.settings)
        info.estimatedBytes += key.size() + value.size();

    auto command = std::make_unique<ImporterApplyUndoCommand>(
        std::move(info), handle, std::move(original), std::move(updated));
    const auto result = NLS::Core::ServiceLocator::Get<NLS::Editor::Undo::EditorUndoService>()
        .Execute(std::move(command));
    if (!result.Succeeded())
    {
        error = result.message.empty()
            ? "The importer settings could not be applied. The draft was kept."
            : result.message;
        NLS_LOG_ERROR("Importer Apply rejected: " + handle.resourcePath + ": " + error);
        return false;
    }
    return true;
}

#if defined(NLS_ENABLE_TEST_HOOKS)
void TestHooks::NotifyAssetImporterReimportedForTesting(const InspectorTargetHandle& target)
{
    ResetImporterUndoScopeAfterReimport(target);
}

TestHooks::ImporterCompletionStateForTesting TestHooks::CompleteAssetImporterReimportForTesting(
    const InspectorTargetHandle& target,
    const uint64_t activeRevision,
    const uint64_t completionRevision,
    std::map<std::string, std::string> initialSettings,
    std::optional<std::map<std::string, std::string>> appliedSettings,
    const bool imported,
    std::string error)
{
    const auto durableTarget = MakeDurableAssetUndoTarget(target);
    {
        std::lock_guard lock(g_importerDraftMutex);
        auto& draft = EnsureImporterDraftLocked(durableTarget);
        draft.available = true;
        draft.dirty = false;
        draft.reimportQueued = true;
        draft.revision = activeRevision;
        draft.settings = std::move(initialSettings);
        draft.lastError.clear();
    }

    CompleteImporterReimport(
        durableTarget,
        completionRevision,
        std::nullopt,
        {},
        std::move(appliedSettings),
        imported,
        std::move(error));

    TestHooks::ImporterCompletionStateForTesting result;
    {
        std::lock_guard lock(g_importerDraftMutex);
        const auto found = g_importerDrafts.find(DraftKey(durableTarget));
        if (found != g_importerDrafts.end())
        {
            const auto& draft = found->second;
            result.available = draft.available;
            result.dirty = draft.dirty;
            result.reimportQueued = draft.reimportQueued;
            result.revision = draft.revision;
            result.settings = draft.settings;
            result.lastError = draft.lastError;
            g_importerDrafts.erase(found);
        }
    }
    return result;
}

void TestHooks::SeedImporterDraftForTesting(
    const InspectorTargetHandle& target,
    std::map<std::string, std::string> settings)
{
    const auto durableTarget = MakeDurableAssetUndoTarget(target);
    const auto key = DraftKey(durableTarget);
    g_importerDraftTestTargets.insert(key);
    std::lock_guard lock(g_importerDraftMutex);
    auto& draft = g_importerDrafts[key];
    draft.settings = std::move(settings);
    draft.initialized = true;
    draft.dirty = false;
    draft.available = true;
    draft.reimportQueued = false;
    draft.revision = 1u;
    draft.lastError.clear();
}

void TestHooks::ClearImporterDraftForTesting(const InspectorTargetHandle& target)
{
    const auto key = DraftKey(MakeDurableAssetUndoTarget(target));
    {
        std::lock_guard lock(g_importerDraftMutex);
        g_importerDrafts.erase(key);
    }
    g_importerDraftTestTargets.erase(key);
}
#endif
} // namespace NLS::Editor::Inspector

namespace NLS::Editor::Serialization
{
namespace
{
using Engine::Serialize::NativeSerializedStreamCodec;
using Engine::Serialize::SerializedByteStream;
using Engine::Serialize::SerializedNodeFlags;
using Engine::Serialize::SerializedNodeKind;
using Engine::Serialize::SerializedNumericKind;
using Engine::Serialize::SerializedReferenceTable;
using Engine::Serialize::SerializedTypeTree;
using Engine::Serialize::SerializedTypeTreeNode;

enum class ImporterStreamValueKind : uint8_t
{
    Boolean,
    SignedInteger,
    FloatingPoint,
    String
};

ImporterStreamValueKind ClassifyImporterStreamValue(const std::string& value)
{
    std::string lowercase = value;
    std::transform(lowercase.begin(), lowercase.end(), lowercase.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    if (lowercase == "true" || lowercase == "false")
        return ImporterStreamValueKind::Boolean;

    int64_t integer = 0;
    const auto [integerEnd, integerError] = std::from_chars(value.data(), value.data() + value.size(), integer);
    if (integerError == std::errc {} && integerEnd == value.data() + value.size())
        return ImporterStreamValueKind::SignedInteger;

    if (!value.empty())
    {
        char* end = nullptr;
        static_cast<void>(std::strtod(value.c_str(), &end));
        if (end != value.c_str() && *end == '\0')
            return ImporterStreamValueKind::FloatingPoint;
    }
    return ImporterStreamValueKind::String;
}

bool IsLegalImporterFieldName(const std::string_view name)
{
    return !name.empty() && name.find_first_of(".[]") == std::string_view::npos && name != "Array";
}

uint64_t StableImporterFieldId(const std::string_view name)
{
    constexpr uint64_t offset = 1469598103934665603ULL;
    constexpr uint64_t prime = 1099511628211ULL;
    uint64_t hash = offset;
    constexpr std::string_view prefix = "Importer.Settings.";
    for (const auto character : prefix)
    {
        hash ^= static_cast<uint8_t>(character);
        hash *= prime;
    }
    for (const auto character : name)
    {
        hash ^= static_cast<uint8_t>(character);
        hash *= prime;
    }
    return hash == 0u ? 1u : hash;
}

void ConfigureImporterLeaf(SerializedTypeTreeNode& node, const ImporterStreamValueKind kind)
{
    switch (kind)
    {
    case ImporterStreamValueKind::Boolean:
        node.kind = SerializedNodeKind::Boolean;
        node.numericKind = SerializedNumericKind::UInt8;
        node.fixedByteSize = 1;
        node.typeName = NLS_TYPEOF(bool).GetName();
        node.typeId = NLS_TYPEOF(bool).GetID();
        break;
    case ImporterStreamValueKind::SignedInteger:
        node.kind = SerializedNodeKind::Integer;
        node.numericKind = SerializedNumericKind::Int64;
        node.fixedByteSize = static_cast<int32_t>(sizeof(int64_t));
        node.typeName = NLS_TYPEOF(int64_t).GetName();
        node.typeId = NLS_TYPEOF(int64_t).GetID();
        break;
    case ImporterStreamValueKind::FloatingPoint:
        node.kind = SerializedNodeKind::Float;
        node.numericKind = SerializedNumericKind::Float64;
        node.fixedByteSize = static_cast<int32_t>(sizeof(double));
        node.typeName = NLS_TYPEOF(double).GetName();
        node.typeId = NLS_TYPEOF(double).GetID();
        break;
    case ImporterStreamValueKind::String:
        node.kind = SerializedNodeKind::String;
        node.fixedByteSize = -1;
        node.flags = node.flags | SerializedNodeFlags::Align4After;
        node.typeName = NLS_TYPEOF(std::string).GetName();
        node.typeId = NLS_TYPEOF(std::string).GetID();
        break;
    }
}

template <typename TValue>
void AppendImporterScalar(std::vector<uint8_t>& bytes, const TValue value)
{
    const auto start = bytes.size();
    bytes.resize(start + sizeof(TValue));
    std::memcpy(bytes.data() + start, &value, sizeof(TValue));
}

bool AppendImporterValue(
    std::vector<uint8_t>& bytes,
    const std::string& source,
    const ImporterStreamValueKind kind,
    std::string& error)
{
    switch (kind)
    {
    case ImporterStreamValueKind::Boolean:
    {
        std::string lower = source;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
        if (lower != "true" && lower != "false")
        {
            error = "Importer boolean setting is invalid.";
            return false;
        }
        AppendImporterScalar(bytes, static_cast<uint8_t>(lower == "true"));
        return true;
    }
    case ImporterStreamValueKind::SignedInteger:
    {
        int64_t value = 0;
        const auto [end, parseError] = std::from_chars(source.data(), source.data() + source.size(), value);
        if (parseError != std::errc {} || end != source.data() + source.size())
        {
            error = "Importer integer setting is invalid.";
            return false;
        }
        AppendImporterScalar(bytes, value);
        return true;
    }
    case ImporterStreamValueKind::FloatingPoint:
    {
        char* end = nullptr;
        const double value = std::strtod(source.c_str(), &end);
        if (end == source.c_str() || *end != '\0')
        {
            error = "Importer floating-point setting is invalid.";
            return false;
        }
        AppendImporterScalar(bytes, value);
        return true;
    }
    case ImporterStreamValueKind::String:
    {
        if (source.size() > (std::numeric_limits<uint32_t>::max)())
        {
            error = "Importer string setting exceeds the native stream limit.";
            return false;
        }
        AppendImporterScalar(bytes, static_cast<uint32_t>(source.size()));
        bytes.insert(bytes.end(), source.begin(), source.end());
        bytes.resize((bytes.size() + 3u) & ~size_t {3u}, 0u);
        return true;
    }
    }
    error = "Importer setting has an unsupported stream type.";
    return false;
}

bool DecodeImporterSettings(
    const SerializedByteStream& source,
    std::map<std::string, std::string>& settings,
    std::string& error)
{
    const auto schema = source.GetSchema();
    if (schema == nullptr)
    {
        error = "Importer stream has no TypeTree.";
        return false;
    }
    const auto* settingsNode = schema->FindPath("Importer.Settings");
    if (settingsNode == nullptr)
    {
        error = "Importer stream has no settings root.";
        return false;
    }
    const auto stream = std::make_shared<SerializedByteStream>(source);
    for (auto child = schema->GetFirstChild(settingsNode->id); child != nullptr;
         child = schema->GetNextSibling(child->id))
    {
        const auto cursor = NativeSerializedStreamCodec::ResolveCursor(
            stream, "Importer.Settings." + child->serializedName);
        if (!cursor.has_value())
        {
            error = "Importer stream property is truncated: " + child->serializedName;
            return false;
        }
        switch (child->kind)
        {
        case SerializedNodeKind::Boolean:
        {
            uint8_t value = 0u;
            if (!cursor->Read(&value, sizeof(value)))
            {
                error = "Importer boolean setting is truncated.";
                return false;
            }
            settings[child->serializedName] = value != 0u ? "true" : "false";
            break;
        }
        case SerializedNodeKind::Integer:
        {
            int64_t value = 0;
            if (!cursor->Read(&value, sizeof(value)))
            {
                error = "Importer integer setting is truncated.";
                return false;
            }
            if (child->serializedName == "MIN_LOD")
                value = (std::max)(int64_t {0}, value);
            settings[child->serializedName] = std::to_string(value);
            break;
        }
        case SerializedNodeKind::Float:
        {
            double value = 0.0;
            if (!cursor->Read(&value, sizeof(value)))
            {
                error = "Importer floating-point setting is truncated.";
                return false;
            }
            std::ostringstream text;
            text.precision(12);
            text << value;
            settings[child->serializedName] = text.str();
            break;
        }
        case SerializedNodeKind::String:
        {
            uint32_t length = 0u;
            if (!cursor->Read(&length, sizeof(length)) ||
                length > cursor->GetByteSize() - sizeof(length))
            {
                error = "Importer string setting is truncated.";
                return false;
            }
            std::string value(length, '\0');
            if (length != 0u && !cursor->Read(value.data(), length, sizeof(length)))
            {
                error = "Importer string setting is truncated.";
                return false;
            }
            settings[child->serializedName] = std::move(value);
            break;
        }
        default:
            error = "Importer stream has an unsupported property type.";
            return false;
        }
    }
    return true;
}
} // namespace

std::shared_ptr<ImporterDraftSerializedBackend> ImporterDraftSerializedBackend::Create()
{
    return std::make_shared<ImporterDraftSerializedBackend>();
}

bool ImporterDraftSerializedBackend::Supports(const SerializedTargetSet& targets)
{
    if (targets.Empty())
        return false;
    const auto first = Inspector::ImporterDraftStreamDetail::Read(targets.targets.front());
    if (!first.available || first.reimporting)
        return false;
    return std::all_of(targets.targets.begin(), targets.targets.end(), [&first](const SerializedTargetHandle& target)
    {
        return target.kind == Inspector::InspectorTargetKind::AssetImporter &&
            Inspector::ImporterDraftStreamDetail::IsCompatible(
                first, Inspector::ImporterDraftStreamDetail::Read(target));
    });
}

Engine::Serialize::TypeTreeResult ImporterDraftSerializedBackend::BuildTypeTree(
    const SerializedTargetSet& targets,
    const SerializedInspectorMode) const
{
    if (!Supports(targets))
        return {nullptr, "Importer drafts are unavailable, busy, or have incompatible schemas."};
    const auto snapshot = Inspector::ImporterDraftStreamDetail::Read(targets.targets.front());
    auto tree = std::make_shared<SerializedTypeTree>();
    SerializedTypeTreeNode root;
    root.serializedName = "Importer";
    root.typeName = "ImporterDraft";
    root.kind = SerializedNodeKind::Generic;
    root.fixedByteSize = -1;
    const auto rootId = tree->AddNode(std::move(root));

    SerializedTypeTreeNode settings;
    settings.parent = rootId;
    settings.serializedName = "Settings";
    settings.typeName = "ImporterSettings";
    settings.kind = SerializedNodeKind::Generic;
    settings.fixedByteSize = -1;
    const auto settingsId = tree->AddNode(std::move(settings));
    tree->GetNodes()[rootId - 1u].firstChild = settingsId;

    Engine::Serialize::SerializedNodeId previous = 0u;
    for (const auto& [name, value] : snapshot.settings)
    {
        if (!IsLegalImporterFieldName(name))
            return {nullptr, "Importer setting name cannot be represented by a serialized property path: " + name};
        SerializedTypeTreeNode leaf;
        leaf.parent = settingsId;
        leaf.fieldId = StableImporterFieldId(name);
        leaf.serializedName = name;
        ConfigureImporterLeaf(leaf, ClassifyImporterStreamValue(value));
        const auto id = tree->AddNode(std::move(leaf));
        if (previous == 0u)
            tree->GetNodes()[settingsId - 1u].firstChild = id;
        else
            tree->GetNodes()[previous - 1u].nextSibling = id;
        previous = id;
    }
    tree->RebuildIndexes();
    return {std::move(tree), {}};
}

Engine::Serialize::StreamResult ImporterDraftSerializedBackend::SerializeTarget(
    const SerializedTargetHandle& target,
    std::shared_ptr<const SerializedTypeTree> schema,
    SerializedReferenceTable& references) const
{
    const auto snapshot = Inspector::ImporterDraftStreamDetail::Read(target);
    if (!snapshot.available || snapshot.reimporting || schema == nullptr)
        return {nullptr, "Importer draft is no longer available."};

    const auto* settings = schema->FindPath("Importer.Settings");
    if (settings == nullptr)
        return {nullptr, "Importer TypeTree has no settings root."};
    std::vector<uint8_t> bytes;
    std::string error;
    for (auto child = schema->GetFirstChild(settings->id); child != nullptr;
         child = schema->GetNextSibling(child->id))
    {
        const auto found = snapshot.settings.find(child->serializedName);
        if (found == snapshot.settings.end() || !AppendImporterValue(
                bytes, found->second, ClassifyImporterStreamValue(found->second), error))
        {
            return {nullptr, error.empty()
                ? "Importer draft changed while its byte stream was being built."
                : std::move(error)};
        }
    }
    auto stream = std::make_shared<SerializedByteStream>(std::move(schema));
    stream->SetBytes(std::move(bytes));
    stream->SetReferences(std::make_shared<SerializedReferenceTable>(references));
    return {std::move(stream), {}};
}

ValidationResult ImporterDraftSerializedBackend::ValidateAndNormalize(
    const SerializedTargetHandle& target,
    const std::span<const SerializedPropertyPath> modifiedPaths,
    SerializedByteStream& stream) const
{
    const auto snapshot = Inspector::ImporterDraftStreamDetail::Read(target);
    if (!snapshot.available || snapshot.reimporting)
        return ValidationResult::Failure("Importer draft is no longer editable.");
    for (const auto& path : modifiedPaths)
    {
        const auto text = path.ToString();
        if (text.rfind("Importer.Settings.", 0u) != 0u ||
            !NativeSerializedStreamCodec::ResolveCursor(
                std::make_shared<SerializedByteStream>(stream), text).has_value())
        {
            return ValidationResult::Failure("Importer property is no longer writable: " + text);
        }
    }
    std::map<std::string, std::string> decoded;
    std::string error;
    return DecodeImporterSettings(stream, decoded, error)
        ? ValidationResult::Success()
        : ValidationResult::Failure(error.empty() ? "Importer draft stream is invalid." : std::move(error));
}

ApplyResult ImporterDraftSerializedBackend::DeserializeTarget(
    const SerializedTargetHandle& target,
    const SerializedByteStream& stream,
    const Undo::UndoExecutionContext&) const
{
    std::map<std::string, std::string> settings;
    std::string error;
    if (!DecodeImporterSettings(stream, settings, error))
        return ApplyResult::Failure(error.empty() ? "Importer draft stream is invalid." : std::move(error));
    return Inspector::ImporterDraftStreamDetail::Write(target, std::move(settings))
        ? ApplyResult::Success()
        : ApplyResult::Failure("Importer draft could not be updated while reimport is in progress.");
}

SerializedVersionStamp ImporterDraftSerializedBackend::GetVersion(const SerializedTargetHandle& target) const
{
    const auto snapshot = Inspector::ImporterDraftStreamDetail::Read(target);
    if (!snapshot.available)
        return {};
    const auto schema = Inspector::ImporterDraftStreamDetail::SchemaSignature(snapshot);
    return {snapshot.revision, schema, schema};
}
} // namespace NLS::Editor::Serialization

#include "Assets/EditorAssetDatabase.h"

#include "Rendering/Assets/ShaderArtifact.h"
#include "Rendering/SceneRendererMaterialBinding.h"

#include <algorithm>
#include <filesystem>
#include <utility>

namespace NLS::Editor::Assets
{
namespace
{
void AddCommand(
    std::vector<EditorAssetCommandDescriptor>& commands,
    std::string commandId,
    std::string displayName,
    bool enabled)
{
    commands.push_back({std::move(commandId), std::move(displayName), enabled});
}

bool CanOpenPrefab(const PrefabCommandSurfaceRequest& request)
{
    return request.assetExists && request.prefabAssetId.IsValid();
}

bool CanCreateVariant(const PrefabCommandSurfaceRequest& request)
{
    return request.assetExists &&
        request.prefabAssetId.IsValid() &&
        (request.subject == PrefabCommandSubject::SourcePrefabAsset ||
            request.subject == PrefabCommandSubject::GeneratedModelPrefabAsset ||
            request.subject == PrefabCommandSubject::PrefabInstance);
}

bool CanUnpackPrefab(const PrefabCommandSurfaceRequest& request)
{
    return request.subject == PrefabCommandSubject::PrefabInstance ||
        request.subject == PrefabCommandSubject::MissingPrefabInstance ||
        request.subject == PrefabCommandSubject::GeneratedModelPrefabAsset;
}

bool CanApplyOverrides(const PrefabCommandSurfaceRequest& request)
{
    return request.subject == PrefabCommandSubject::PrefabInstance &&
        request.connectedInstance &&
        request.assetExists &&
        request.overrideCount > 0u;
}

bool CanRevertOverrides(const PrefabCommandSurfaceRequest& request)
{
    return (request.subject == PrefabCommandSubject::PrefabInstance ||
        request.subject == PrefabCommandSubject::MissingPrefabInstance) &&
        request.overrideCount > 0u;
}

bool CanDropToMutableHierarchy(const AssetDragDropCommandSurfaceRequest& request)
{
    return request.targetExists && !request.targetReadOnly;
}

bool CanDropToEditableAssetFolder(const AssetDragDropCommandSurfaceRequest& request)
{
    return request.targetExists && !request.targetReadOnly && request.editableAssetFolder;
}
}

bool GeneratedPrefabState::CanEditInPlace() const
{
    return editPolicy == GeneratedPrefabEditPolicy::EditableSource && !generatedReadOnly;
}

bool GeneratedPrefabState::CanCreateEditableVariant() const
{
    return sourceAssetId.IsValid();
}

bool GeneratedPrefabState::CanUnpackToSceneObjects() const
{
    return sourceAssetId.IsValid();
}

void EditorAssetDatabase::RegisterGeneratedPrefab(GeneratedPrefabState state)
{
    state.generatedReadOnly = state.editPolicy == GeneratedPrefabEditPolicy::ReadOnlyGenerated;
    for (auto& existing : m_generatedPrefabs)
    {
        if (existing.sourceAssetId == state.sourceAssetId && existing.subAssetKey == state.subAssetKey)
        {
            existing = std::move(state);
            return;
        }
    }

    m_generatedPrefabs.push_back(std::move(state));
}

const GeneratedPrefabState* EditorAssetDatabase::FindGeneratedPrefabState(
    NLS::Core::Assets::AssetId sourceAssetId,
    const std::string& subAssetKey) const
{
    for (const auto& state : m_generatedPrefabs)
    {
        if (state.sourceAssetId == sourceAssetId && state.subAssetKey == subAssetKey)
            return &state;
    }
    return nullptr;
}

std::vector<EditorAssetCommandDescriptor> EditorAssetDatabase::GetGeneratedPrefabCommands(
    NLS::Core::Assets::AssetId sourceAssetId,
    const std::string& subAssetKey) const
{
    std::vector<EditorAssetCommandDescriptor> commands;
    const auto* state = FindGeneratedPrefabState(sourceAssetId, subAssetKey);
    if (!state)
        return commands;

    commands.push_back({
        "prefab.create-variant",
        "Create Variant",
        state->CanCreateEditableVariant()
    });
    commands.push_back({
        "prefab.unpack",
        "Unpack",
        state->CanUnpackToSceneObjects()
    });
    return commands;
}

std::vector<EditorAssetCommandDescriptor> EditorAssetDatabase::GetPrefabCommandSurface(
    const PrefabCommandSurfaceRequest& request) const
{
    std::vector<EditorAssetCommandDescriptor> commands;
    AddCommand(
        commands,
        "prefab.create-from-selection",
        "Create Prefab",
        request.surface == PrefabCommandSurface::AssetBrowser &&
            request.subject != PrefabCommandSubject::GeneratedModelPrefabAsset);
    AddCommand(commands, "prefab.open", "Open Prefab", CanOpenPrefab(request));
    AddCommand(commands, "prefab.create-variant", "Create Variant", CanCreateVariant(request));
    AddCommand(commands, "prefab.apply-overrides", "Apply Overrides", CanApplyOverrides(request));
    AddCommand(commands, "prefab.revert-overrides", "Revert Overrides", CanRevertOverrides(request));
    AddCommand(commands, "prefab.unpack", "Unpack", CanUnpackPrefab(request));
    return commands;
}

std::vector<EditorAssetCommandDescriptor> EditorAssetDatabase::GetAssetDragDropCommandSurface(
    const AssetDragDropCommandSurfaceRequest& request) const
{
    std::vector<EditorAssetCommandDescriptor> commands;
    switch (request.subject)
    {
    case AssetDragDropCommandSubject::PrefabAssetToHierarchy:
    case AssetDragDropCommandSubject::GeneratedModelPrefabAssetToHierarchy:
        AddCommand(
            commands,
            "dragdrop.instantiate-prefab",
            "Instantiate Prefab",
            CanDropToMutableHierarchy(request));
        break;
    case AssetDragDropCommandSubject::MaterialAssetToRenderer:
        AddCommand(
            commands,
            "dragdrop.assign-material",
            "Assign Material",
            CanDropToMutableHierarchy(request) && request.rendererTarget);
        break;
    case AssetDragDropCommandSubject::TextureAssetToRenderer:
        AddCommand(
            commands,
            "dragdrop.create-material-and-assign",
            "Create Material And Assign",
            CanDropToMutableHierarchy(request) && request.rendererTarget);
        break;
    case AssetDragDropCommandSubject::HierarchyObjectToAssetFolder:
        AddCommand(
            commands,
            "dragdrop.save-as-prefab",
            "Save As Prefab",
            CanDropToEditableAssetFolder(request));
        AddCommand(commands, "dragdrop.create-variant", "Create Variant", false);
        AddCommand(commands, "dragdrop.create-unpacked-copy", "Create Unpacked Copy", false);
        break;
    case AssetDragDropCommandSubject::PrefabInstanceToAssetFolder:
        AddCommand(
            commands,
            "dragdrop.save-as-prefab",
            "Save As Prefab",
            CanDropToEditableAssetFolder(request) && !request.generatedReadOnly);
        AddCommand(
            commands,
            "dragdrop.create-variant",
            "Create Variant",
            CanDropToEditableAssetFolder(request));
        AddCommand(
            commands,
            "dragdrop.create-unpacked-copy",
            "Create Unpacked Copy",
            CanDropToEditableAssetFolder(request));
        break;
    case AssetDragDropCommandSubject::GeneratedModelPrefabInstanceToAssetFolder:
        AddCommand(commands, "dragdrop.save-as-prefab", "Save As Prefab", false);
        AddCommand(
            commands,
            "dragdrop.create-variant",
            "Create Variant",
            CanDropToEditableAssetFolder(request));
        AddCommand(
            commands,
            "dragdrop.create-unpacked-copy",
            "Create Unpacked Copy",
            CanDropToEditableAssetFolder(request));
        break;
    case AssetDragDropCommandSubject::Unknown:
    default:
        break;
    }
    return commands;
}

std::vector<EditorAssetCommandDescriptor> EditorAssetDatabase::GetImportProgressCommandSurface(
    const ImportBatchProgress& progress) const
{
    std::vector<EditorAssetCommandDescriptor> commands;
    const auto hasRunningImport = progress.activeJob.has_value();
    const auto hasVisibleStatus =
        hasRunningImport ||
        progress.completedAssets > 0u ||
        progress.failedAssets > 0u ||
        progress.cancelledAssets > 0u;

    AddCommand(commands, "assetimport.show-progress", "Show Import Progress", hasVisibleStatus);
    AddCommand(commands, "assetimport.cancel", "Cancel Import", hasRunningImport);
    return commands;
}

EditorImportProgressStatus EditorAssetDatabase::GetImportProgressStatus(
    const std::optional<ImportProgressEvent>& activeEvent) const
{
    EditorImportProgressStatus status;
    if (!activeEvent.has_value())
        return status;

    status.visible = true;
    status.normalizedProgress = static_cast<float>(activeEvent->normalizedProgress);
    status.cancellable =
        activeEvent->sourcePath != "Startup Scene" &&
        activeEvent->sourcePath != "Asset Browser";
    status.label = activeEvent->message.empty()
        ? "Processing assets"
        : activeEvent->message;
    if (!activeEvent->sourcePath.empty())
        status.label += " - " + activeEvent->sourcePath;
    return status;
}

std::vector<EditorMaterialViewportBinding> EditorAssetDatabase::GetMaterialPreviewBindings(
    const NLS::Engine::Assets::PrefabArtifact& prefab,
    const NLS::Engine::Assets::RuntimeAssetDatabase& runtimeAssets) const
{
    std::vector<EditorMaterialViewportBinding> bindings;
    const auto sceneBindings = NLS::Engine::Rendering::ResolveSceneRendererMaterialBindings(
        prefab,
        runtimeAssets);
    bindings.reserve(sceneBindings.size());
    for (const auto& binding : sceneBindings)
    {
        bindings.push_back({
            binding.rendererDebugName,
            binding.slotIndex,
            binding.reference,
            binding.artifactPath,
            binding.resolved
        });
    }
    return bindings;
}

size_t EditorAssetDatabase::GetGeneratedPrefabStateCount() const
{
    return m_generatedPrefabs.size();
}

void AssetRefreshScheduler::RequestRefresh(std::filesystem::path path, AssetRefreshReason reason)
{
    m_requests.push_back({std::move(path), reason});
}

void AssetRefreshScheduler::PollWatcher(
    NLS::Editor::Core::AssetFileWatcher& watcher,
    const std::filesystem::path& root)
{
    if (!watcher.ConsumeChangedPaths().empty())
        RequestRefresh(root, AssetRefreshReason::FileWatcherChanged);
}

bool AssetRefreshScheduler::HasPendingRefresh() const
{
    return !m_requests.empty();
}

std::vector<AssetRefreshRequest> AssetRefreshScheduler::ConsumeRefreshRequests()
{
    auto requests = std::move(m_requests);
    m_requests.clear();
    return requests;
}

namespace
{
std::string DefaultGeneratedPrefabSubAssetKey(const std::string& assetPath)
{
    return "prefab:" + std::filesystem::path(assetPath).stem().generic_string();
}

bool IsPreimportableAsset(const std::string& assetPath)
{
    const auto assetType = NLS::Core::Assets::InferAssetType(assetPath);
    return assetType == NLS::Core::Assets::AssetType::ModelScene ||
        assetType == NLS::Core::Assets::AssetType::Prefab ||
        assetType == NLS::Core::Assets::AssetType::Shader;
}

std::vector<std::string> CollectPreimportableAssets(AssetDatabaseFacade& database)
{
    std::vector<std::string> assets = database.FindAssets("type:model-scene", {});
    auto prefabs = database.FindAssets("type:prefab", {});
    assets.insert(assets.end(), prefabs.begin(), prefabs.end());
    auto shaders = database.FindAssets("type:shader", {});
    assets.insert(assets.end(), shaders.begin(), shaders.end());
    std::sort(assets.begin(), assets.end());
    assets.erase(std::unique(assets.begin(), assets.end()), assets.end());
    return assets;
}

bool IsWarmPreimportableAsset(
    AssetDatabaseFacade& database,
    const std::string& assetPath)
{
    if (!database.IsArtifactManifestCurrentForAssetPath(assetPath))
        return false;

    const auto assetType = NLS::Core::Assets::InferAssetType(assetPath);
    if (assetType == NLS::Core::Assets::AssetType::Prefab ||
        assetType == NLS::Core::Assets::AssetType::ModelScene)
    {
        return database.LoadPrefabArtifactAtPath(
            assetPath,
            DefaultGeneratedPrefabSubAssetKey(assetPath)).has_value();
    }

    if (assetType != NLS::Core::Assets::AssetType::Shader)
        return false;

    const auto mainAsset = database.LoadMainAssetAtPath(assetPath);
    if (!mainAsset.has_value() || mainAsset->artifactPath.empty())
        return false;

    const auto artifact = NLS::Render::Assets::LoadShaderArtifact(mainAsset->artifactPath);
    return artifact.has_value() &&
        NLS::Render::Assets::HasUsableShaderArtifactStage(
            *artifact,
            NLS::Render::ShaderCompiler::ShaderTargetPlatform::DXIL) &&
        NLS::Render::Assets::HasUsableShaderArtifactStage(
            *artifact,
            NLS::Render::ShaderCompiler::ShaderTargetPlatform::SPIRV) &&
        NLS::Render::Assets::HasUsableShaderArtifactStage(
            *artifact,
            NLS::Render::ShaderCompiler::ShaderTargetPlatform::GLSL);
}

bool ShouldForcePreimport(const AssetPreimportReason reason)
{
    return reason == AssetPreimportReason::FileWatcherChanged ||
        reason == AssetPreimportReason::AssetCopiedOrMoved;
}

std::filesystem::path NormalizePreimportPath(std::filesystem::path path)
{
    return path.lexically_normal();
}

bool PathIsOrContainsPath(const std::filesystem::path& changedPath, const std::filesystem::path& assetPath)
{
    const auto changed = NormalizePreimportPath(changedPath);
    const auto asset = NormalizePreimportPath(assetPath);
    if (changed.empty())
        return true;
    if (changed == asset)
        return true;
    const auto relative = asset.lexically_relative(changed);
    if (relative.empty() || relative.is_absolute())
        return false;

    for (const auto& part : relative)
    {
        if (part == "..")
            return false;
    }
    return true;
}

std::vector<std::filesystem::path> BuildChangedPathCandidates(
    AssetDatabaseFacade& database,
    const std::filesystem::path& changedPath,
    const std::string& ownerAssetPath = {})
{
    std::vector<std::filesystem::path> candidates;
    candidates.push_back(NormalizePreimportPath(changedPath));

    auto editorAssetPath = database.TryGetEditorAssetPath(changedPath);
    if (editorAssetPath.has_value())
    {
        const auto editorRelative = NormalizePreimportPath(*editorAssetPath);
        if (std::find(candidates.begin(), candidates.end(), editorRelative) == candidates.end())
            candidates.push_back(editorRelative);
    }

    if (!ownerAssetPath.empty())
    {
        auto rootRelativePath = database.TryGetRootRelativeAssetPath(ownerAssetPath, changedPath);
        if (rootRelativePath.has_value())
        {
            const auto rootRelative = NormalizePreimportPath(*rootRelativePath);
            if (std::find(candidates.begin(), candidates.end(), rootRelative) == candidates.end())
                candidates.push_back(rootRelative);
        }
    }

    return candidates;
}

bool AssetMatchesChangedPaths(
    AssetDatabaseFacade& database,
    const std::string& assetPath,
    const std::vector<std::filesystem::path>& changedPaths)
{
    if (changedPaths.empty())
        return true;

    const auto normalizedAssetPath = NormalizePreimportPath(assetPath);
    const auto metaPath = NormalizePreimportPath(assetPath + ".meta");
    for (const auto& changedPath : changedPaths)
    {
        for (const auto& candidate : BuildChangedPathCandidates(database, changedPath))
        {
            if (PathIsOrContainsPath(candidate, normalizedAssetPath) ||
                PathIsOrContainsPath(candidate, metaPath))
            {
                return true;
            }
        }
    }
    return false;
}

bool DependencyMatchesChangedPaths(
    AssetDatabaseFacade& database,
    const std::string& ownerAssetPath,
    const NLS::Core::Assets::AssetDependencyRecord& dependency,
    const std::vector<std::filesystem::path>& changedPaths)
{
    if (changedPaths.empty())
        return true;
    if (dependency.kind != NLS::Core::Assets::AssetDependencyKind::SourceFileHash &&
        dependency.kind != NLS::Core::Assets::AssetDependencyKind::PathToGuidMapping)
    {
        return false;
    }

    const auto dependencyPath = NormalizePreimportPath(dependency.value);
    for (const auto& changedPath : changedPaths)
    {
        for (const auto& candidate : BuildChangedPathCandidates(database, changedPath, ownerAssetPath))
        {
            if (PathIsOrContainsPath(candidate, dependencyPath))
                return true;
        }
    }
    return false;
}

bool ManifestMatchesChangedPaths(
    AssetDatabaseFacade& database,
    const std::string& ownerAssetPath,
    const NLS::Core::Assets::ArtifactManifest& manifest,
    const std::vector<std::filesystem::path>& changedPaths)
{
    return std::any_of(
        manifest.dependencies.begin(),
        manifest.dependencies.end(),
        [&database, &ownerAssetPath, &changedPaths](const NLS::Core::Assets::AssetDependencyRecord& dependency)
        {
            return DependencyMatchesChangedPaths(database, ownerAssetPath, dependency, changedPaths);
        });
}
}

AssetPreimportPlan AssetPreimportScheduler::BuildPlan(AssetDatabaseFacade& database) const
{
    return BuildPlan(database, AssetPreimportReason::EditorStartup);
}

AssetPreimportPlan AssetPreimportScheduler::BuildPlan(
    AssetDatabaseFacade& database,
    const AssetPreimportReason reason) const
{
    return BuildPlan(database, {reason, {}});
}

AssetPreimportPlan AssetPreimportScheduler::BuildPlan(
    AssetDatabaseFacade& database,
    const AssetPreimportRequest& request) const
{
    AssetPreimportPlan plan;
    for (const auto& assetPath : CollectPreimportableAssets(database))
    {
        if (!IsPreimportableAsset(assetPath))
            continue;

        auto manifest = database.GetArtifactManifestForAssetPath(assetPath);
        if (!AssetMatchesChangedPaths(database, assetPath, request.changedPaths) &&
            (!manifest.has_value() || !ManifestMatchesChangedPaths(database, assetPath, *manifest, request.changedPaths)))
        {
            continue;
        }

        if (IsWarmPreimportableAsset(database, assetPath))
        {
            continue;
        }

        plan.assetPaths.push_back(assetPath);
    }
    return plan;
}

bool AssetPreimportScheduler::Run(
    AssetDatabaseFacade& database,
    ImportProgressTracker& progressTracker)
{
    return Run(database, progressTracker, AssetPreimportReason::EditorStartup);
}

bool AssetPreimportScheduler::Run(
    AssetDatabaseFacade& database,
    ImportProgressTracker& progressTracker,
    const AssetPreimportReason reason)
{
    return Run(database, progressTracker, {reason, {}});
}

bool AssetPreimportScheduler::Run(
    AssetDatabaseFacade& database,
    ImportProgressTracker& progressTracker,
    const AssetPreimportRequest& request)
{
    if (!database.Refresh())
        return false;

    const auto plan = BuildPlan(database, request);
    return RunAlreadyPlanned(database, progressTracker, request, plan);
}

bool AssetPreimportScheduler::RunAlreadyPlanned(
    AssetDatabaseFacade& database,
    ImportProgressTracker& progressTracker,
    const AssetPreimportRequest& request,
    const AssetPreimportPlan& plan)
{
    const bool forcePreimport = ShouldForcePreimport(request.reason);
    if (forcePreimport && !database.Refresh())
        return false;

    std::vector<std::string> pendingAssetPaths;
    pendingAssetPaths.reserve(plan.assetPaths.size());
    for (const auto& assetPath : plan.assetPaths)
    {
        if (forcePreimport && IsWarmPreimportableAsset(database, assetPath))
            continue;
        pendingAssetPaths.push_back(assetPath);
    }

    const auto batchTotalAssets = std::max<size_t>(pendingAssetPaths.size(), 1u);
    bool allSucceeded = true;
    for (const auto& assetPath : pendingAssetPaths)
    {
        const auto imported = forcePreimport
            ? database.ReimportAssetFromCurrentDatabase(assetPath, progressTracker, batchTotalAssets)
            : database.ImportAssetFromCurrentDatabase(assetPath, progressTracker, batchTotalAssets);
        if (!imported)
            allSucceeded = false;
    }
    return allSucceeded;
}

bool AssetPreimportScheduler::ShouldRunForReason(const AssetPreimportReason reason) const
{
    switch (reason)
    {
    case AssetPreimportReason::EditorStartup:
    case AssetPreimportReason::FileWatcherChanged:
    case AssetPreimportReason::AssetCopiedOrMoved:
        return true;
    default:
        return false;
    }
}
}

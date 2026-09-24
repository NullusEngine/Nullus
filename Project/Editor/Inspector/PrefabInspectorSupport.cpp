#include "Inspector/PrefabInspectorSupport.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <Assets/AssetDatabaseFacade.h>
#include <Assets/PrefabUtilityFacade.h>
#include <Components/Component.h>
#include <GameObject.h>
#include <Serialize/ObjectGraphDocument.h>
#include <Serialize/ObjectGraphWriter.h>
#include <ServiceLocator.h>

#include "Core/Context.h"
#include "Core/EditorActions.h"
#include "Inspector/NativeReflectionAdapter.h"
#include "Undo/EditorUndoMutationContext.h"
#include "Undo/EditorUndoService.h"

namespace NLS::Editor::Inspector
{
namespace
{
using NLS::Engine::Assets::PrefabArtifact;
using NLS::Engine::Components::Component;
using NLS::Engine::GameObject;
using NLS::Engine::Serialize::ObjectGraphDocument;
using NLS::Engine::Serialize::ObjectId;
using NLS::Engine::Serialize::ObjectRecord;
using NLS::Engine::Serialize::PatchOperation;
using NLS::Engine::Serialize::PatchOperationType;
using NLS::Core::Assets::AssetId;

#if defined(NLS_ENABLE_TEST_HOOKS)
Assets::PrefabInstanceRegistry* g_testInstanceRegistry = nullptr;
std::filesystem::path g_testProjectAssetsPath;
uint64_t g_markerDiscoveryCount = 0u;
#endif

struct PrefabInspectorServices
{
    Assets::PrefabInstanceRegistry* instanceRegistry = nullptr;
    std::filesystem::path projectAssetsPath;
};

std::optional<PrefabInspectorServices> ResolvePrefabInspectorServices()
{
#if defined(NLS_ENABLE_TEST_HOOKS)
    if (g_testInstanceRegistry != nullptr)
        return PrefabInspectorServices {g_testInstanceRegistry, g_testProjectAssetsPath};
#endif

    if (!NLS::Core::ServiceLocator::Contains<Core::Context>())
        return std::nullopt;

    auto& context = NLS::Core::ServiceLocator::Get<Core::Context>();
    return PrefabInspectorServices {&context.prefabInstanceRegistry, context.projectAssetsPath};
}

struct PrefabOverrideSnapshot
{
    Assets::PrefabInstanceRecord* instance = nullptr;
    std::shared_ptr<const PrefabArtifact> source;
    std::vector<Assets::PrefabOverrideDescriptor> overrides;
};

using PrefabSnapshotCache = std::unordered_map<Assets::PrefabInstanceRecord*, PrefabOverrideSnapshot>;

// An Inspector selection handle is only valid for the frame in which it is
// resolved. Prefab history instead identifies an instance by the ObjectGraph
// object that represents its root. Loaded scenes provide
// preservedInstanceRootObject; a live, unsaved instance has the equivalent
// sourceToInstance root mapping. Neither identity retains an engine pointer.
struct PrefabInstanceUndoIdentity
{
    ObjectId rootObject;
    int64_t rootFileId = 0;
};

bool IsSamePrefabInstanceUndoIdentity(
    const PrefabInstanceUndoIdentity& left,
    const PrefabInstanceUndoIdentity& right)
{
    return left.rootObject == right.rootObject && left.rootFileId == right.rootFileId;
}

struct PrefabInstanceUndoSnapshot
{
    PrefabInstanceUndoIdentity identity;
    std::vector<PatchOperation> localPatches;
    std::vector<Assets::PrefabOverrideDescriptor> overrides;
};

struct PrefabInspectorUndoState
{
    PrefabInstanceUndoIdentity primaryInstance;
    AssetId prefabAssetId;
    std::string prefabSubAssetKey;
    std::string sourceAssetPath;
    Undo::UndoScopeId ownerScope;
    bool sourceChanged = false;
    std::string sourceBefore;
    std::string sourceAfter;
    std::vector<PrefabInstanceUndoSnapshot> instancesBefore;
    std::vector<PrefabInstanceUndoSnapshot> instancesAfter;
    // PrefabOverrideDescriptor contains only ObjectGraph identities and
    // serialized patch payloads. It is safe to retain for the command's first
    // Redo, unlike the live inspector snapshot that produced it.
    std::vector<Assets::PrefabOverrideDescriptor> requestedOverrides;
};

GameObject* ResolveGameObject(const InspectorTargetHandle& handle)
{
    auto* object = NativeReflectionAdapter::ResolveObject(handle);
    if (auto* gameObject = dynamic_cast<GameObject*>(object))
        return gameObject;
    if (auto* component = dynamic_cast<Component*>(object))
        return component->gameobject();
    return nullptr;
}

const ObjectRecord* FindObjectRecord(const ObjectGraphDocument& graph, const ObjectId& id)
{
    const auto found = std::find_if(graph.objects.begin(), graph.objects.end(), [&id](const ObjectRecord& record)
    {
        return record.id == id;
    });
    return found != graph.objects.end() ? &*found : nullptr;
}

std::vector<ObjectId> GetOwnedReferences(const ObjectRecord& record, const std::string_view propertyName)
{
    std::vector<ObjectId> result;
    const auto property = std::find_if(record.properties.begin(), record.properties.end(), [propertyName](const auto& candidate)
    {
        return candidate.name == propertyName;
    });
    if (property == record.properties.end() ||
        property->value.GetKind() != NLS::Engine::Serialize::PropertyValue::Kind::Array)
    {
        return result;
    }

    for (const auto& value : property->value.GetArray())
    {
        if (value.GetKind() == NLS::Engine::Serialize::PropertyValue::Kind::OwnedReference)
            result.push_back(value.GetObjectId());
    }
    return result;
}

std::optional<ObjectId> FindSourceObject(
    const Assets::PrefabInstanceRecord& instance,
    const GameObject& gameObject)
{
    const auto found = instance.sourceByInstanceObject.find(&gameObject);
    if (found == instance.sourceByInstanceObject.end())
        return std::nullopt;
    return found->second;
}

std::optional<ObjectId> FindSourceComponent(
    const Assets::PrefabInstanceRecord& instance,
    const PrefabArtifact& source,
    const Component& component)
{
    auto* owner = component.gameobject();
    if (owner == nullptr)
        return std::nullopt;

    const auto sourceOwnerId = FindSourceObject(instance, *owner);
    if (!sourceOwnerId.has_value())
        return std::nullopt;
    const auto* sourceOwner = FindObjectRecord(source.graph, *sourceOwnerId);
    if (sourceOwner == nullptr)
        return std::nullopt;

    const auto sourceComponents = GetOwnedReferences(*sourceOwner, "components");
    std::vector<bool> matched(sourceComponents.size(), false);
    const auto typeName = component.GetType().GetName();
    for (const auto& candidate : owner->GetComponents())
    {
        if (candidate == nullptr)
            continue;

        const auto candidateTypeName = candidate->GetType().GetName();
        for (size_t index = 0u; index < sourceComponents.size(); ++index)
        {
            if (matched[index])
                continue;
            const auto* sourceRecord = FindObjectRecord(source.graph, sourceComponents[index]);
            if (sourceRecord == nullptr || sourceRecord->typeName != candidateTypeName)
                continue;

            matched[index] = true;
            if (candidate.get() == &component)
                return sourceComponents[index];
            break;
        }
    }
    return std::nullopt;
}

std::shared_ptr<const PrefabArtifact> BuildSourceArtifact(const Assets::PrefabInstanceRecord& instance)
{
    if (instance.sharedSourcePrefab != nullptr)
        return instance.sharedSourcePrefab;

    auto source = std::make_shared<PrefabArtifact>();
    source->assetId = instance.prefabAssetId;
    source->graph = instance.SourceGraph();
    source->generatedModelPrefab = instance.generatedReadOnly;
    return source;
}

PrefabOverrideSnapshot* ResolveSnapshot(
    const InspectorTargetHandle& handle,
    PrefabSnapshotCache& cache)
{
    const auto services = ResolvePrefabInspectorServices();
    if (!services.has_value() || services->instanceRegistry == nullptr)
        return nullptr;

    auto* owner = ResolveGameObject(handle);
    if (owner == nullptr)
        return nullptr;

    auto* instance = services->instanceRegistry->FindInstance(*owner);
    if (instance == nullptr || instance->unpacked || !instance->prefabAssetId.IsValid())
        return nullptr;

    const auto existing = cache.find(instance);
    if (existing != cache.end())
        return &existing->second;

    PrefabOverrideSnapshot snapshot;
    snapshot.instance = instance;
    snapshot.source = BuildSourceArtifact(*instance);
    if (snapshot.source == nullptr || snapshot.source->graph.objects.empty())
        return nullptr;

    snapshot.overrides = Assets::PrefabUtilityFacade().GetPrefabOverridesForObject(
        *snapshot.source,
        *instance,
        *owner,
        true);
    return &cache.emplace(instance, std::move(snapshot)).first->second;
}

bool IsStructuralOverride(const Assets::PrefabOverrideDescriptor& descriptor)
{
    return descriptor.kind != Assets::PrefabOverrideKind::Property &&
        descriptor.kind != Assets::PrefabOverrideKind::DefaultOverride &&
        descriptor.patch.type != PatchOperationType::ReplaceProperty;
}

bool IsSamePatchIdentity(const PatchOperation& left, const PatchOperation& right)
{
    return left.type == right.type &&
        left.target == right.target &&
        left.property == right.property &&
        left.object == right.object &&
        left.hasIndex == right.hasIndex &&
        (!left.hasIndex || left.index == right.index);
}

std::vector<Assets::PrefabOverrideDescriptor> CollectOverridesForNode(
    const PropertyNode& node,
    PrefabOverrideSnapshot& snapshot)
{
    if (GetPropertyNodeTargets(node).size() > 1u)
        return {};

    auto* targetObject = NativeReflectionAdapter::ResolveObject(node.target);
    auto* component = dynamic_cast<Component*>(targetObject);
    auto* owner = component != nullptr
        ? component->gameobject()
        : dynamic_cast<GameObject*>(targetObject);
    if (owner == nullptr || snapshot.instance == nullptr)
        return {};

    const auto sourceOwner = FindSourceObject(*snapshot.instance, *owner);
    if (!sourceOwner.has_value())
        return {};

    const auto sourceComponent = component != nullptr
        ? FindSourceComponent(*snapshot.instance, *snapshot.source, *component)
        : std::optional<ObjectId> {};
    std::vector<Assets::PrefabOverrideDescriptor> result;
    for (const auto& descriptor : snapshot.overrides)
    {
        if (node.isComponent)
        {
            if (!IsStructuralOverride(descriptor))
                continue;
            if (sourceComponent.has_value() &&
                (descriptor.sourceObject == *sourceComponent ||
                    descriptor.patch.target == *sourceComponent ||
                    descriptor.patch.object == *sourceComponent))
            {
                result.push_back(descriptor);
                continue;
            }
            if (descriptor.propertyPath == "components" &&
                (descriptor.sourceObject == *sourceOwner || descriptor.patch.target == *sourceOwner))
            {
                result.push_back(descriptor);
            }
            continue;
        }

        if (node.fieldName.empty() || !sourceComponent.has_value() && component != nullptr)
            continue;

        const auto sourceObject = sourceComponent.has_value() ? *sourceComponent : *sourceOwner;
        if ((descriptor.kind == Assets::PrefabOverrideKind::Property ||
                descriptor.kind == Assets::PrefabOverrideKind::DefaultOverride) &&
            descriptor.patch.type == PatchOperationType::ReplaceProperty &&
            descriptor.sourceObject == sourceObject &&
            descriptor.propertyPath == node.fieldName)
        {
            result.push_back(descriptor);
        }
    }
    return result;
}

void RefreshNodeOverrides(PropertyNode& node, PrefabOverrideQuery& query)
{
    node.prefabOverride = query.GetState(node);

    for (auto& child : node.children)
        RefreshNodeOverrides(child, query);
}

std::string DescribeFailure(const Assets::PrefabOperationResult& result)
{
    if (!result.diagnostics.empty())
    {
        const auto& diagnostic = result.diagnostics.front();
        return diagnostic.message.empty() ? diagnostic.code : diagnostic.message;
    }
    return "The prefab override operation could not be completed.";
}

bool MatchesUndoState(
    const Assets::PrefabInstanceRecord& instance,
    const PrefabInspectorUndoState& state)
{
    return !instance.unpacked &&
        instance.prefabAssetId == state.prefabAssetId &&
        instance.prefabSubAssetKey == state.prefabSubAssetKey;
}

std::optional<PrefabInstanceUndoIdentity> MakePrefabInstanceUndoIdentity(
    const Assets::PrefabInstanceRecord& instance)
{
    if (instance.instanceRoot == nullptr)
        return std::nullopt;

    ObjectId rootObject = instance.preservedInstanceRootObject;
    if (!rootObject.IsValid())
    {
        const auto& sourceGraph = instance.SourceGraph();
        const auto sourceRoot = instance.sourceToInstance.find(sourceGraph.root);
        if (sourceRoot != instance.sourceToInstance.end())
            rootObject = sourceRoot->second;
    }

    const int64_t rootFileId = NLS::Engine::Serialize::MakeLocalIdentifierInFile(rootObject);
    if (!rootObject.IsValid() || rootFileId == 0)
        return std::nullopt;

    return PrefabInstanceUndoIdentity {rootObject, rootFileId};
}

bool MatchesPrefabInstanceUndoIdentity(
    const Assets::PrefabInstanceRecord& instance,
    const PrefabInstanceUndoIdentity& identity)
{
    const auto current = MakePrefabInstanceUndoIdentity(instance);
    return current.has_value() && IsSamePrefabInstanceUndoIdentity(*current, identity);
}

std::optional<PrefabInstanceUndoSnapshot> CaptureInstanceUndoSnapshot(
    const Assets::PrefabInstanceRecord& instance)
{
    const auto identity = MakePrefabInstanceUndoIdentity(instance);
    if (!identity.has_value())
        return std::nullopt;

    PrefabInstanceUndoSnapshot snapshot;
    snapshot.identity = *identity;
    snapshot.localPatches = instance.localPatches;
    const auto source = BuildSourceArtifact(instance);
    if (source != nullptr)
        snapshot.overrides = Assets::PrefabUtilityFacade().GetPrefabOverrides(*source, instance, true);
    return snapshot;
}

std::vector<PrefabInstanceUndoSnapshot> CaptureInstanceUndoSnapshots(
    Assets::PrefabInstanceRegistry& instanceRegistry,
    const AssetId prefabAssetId,
    const std::string& prefabSubAssetKey)
{
    std::vector<PrefabInstanceUndoSnapshot> snapshots;
    for (auto* instance : instanceRegistry.FindInstancesForPrefab(prefabAssetId, prefabSubAssetKey))
    {
        if (instance == nullptr || instance->instanceRoot == nullptr || instance->unpacked)
            continue;
        if (auto snapshot = CaptureInstanceUndoSnapshot(*instance); snapshot.has_value())
            snapshots.push_back(std::move(*snapshot));
    }
    return snapshots;
}

Assets::PrefabInstanceRecord* ResolveUndoInstance(
    Assets::PrefabInstanceRegistry& instanceRegistry,
    const PrefabInstanceUndoSnapshot& snapshot,
    const PrefabInspectorUndoState& state)
{
    for (auto* instance : instanceRegistry.FindInstancesForPrefab(
             state.prefabAssetId,
             state.prefabSubAssetKey))
    {
        if (instance != nullptr && MatchesUndoState(*instance, state) &&
            MatchesPrefabInstanceUndoIdentity(*instance, snapshot.identity))
        {
            return instance;
        }
    }
    return nullptr;
}

bool IsPrefabUndoStateValid(const PrefabInspectorUndoState& state, const bool requireAfterSnapshots = true)
{
    const auto services = ResolvePrefabInspectorServices();
    if (!services.has_value() || services->instanceRegistry == nullptr)
        return false;

    const auto findPrimary = [&state](const std::vector<PrefabInstanceUndoSnapshot>& snapshots)
    {
        return std::find_if(
            snapshots.begin(),
            snapshots.end(),
            [&state](const PrefabInstanceUndoSnapshot& snapshot)
            {
                return IsSamePrefabInstanceUndoIdentity(snapshot.identity, state.primaryInstance);
            });
    };
    const auto primaryBefore = findPrimary(state.instancesBefore);
    const auto primaryAfter = findPrimary(state.instancesAfter);
    return primaryBefore != state.instancesBefore.end() &&
        (!requireAfterSnapshots || primaryAfter != state.instancesAfter.end()) &&
        ResolveUndoInstance(*services->instanceRegistry, *primaryBefore, state) != nullptr;
}

void SynchronizeLocalPatches(
    Assets::PrefabInstanceRecord& instance,
    const std::vector<Assets::PrefabOverrideDescriptor>& overrides)
{
    NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT("synchronizing prefab override patches");
    for (const auto& descriptor : overrides)
    {
        const auto existing = std::find_if(instance.localPatches.begin(), instance.localPatches.end(), [&descriptor](const PatchOperation& patch)
        {
            return IsSamePatchIdentity(patch, descriptor.patch);
        });
        if (existing != instance.localPatches.end())
            *existing = descriptor.patch;
        else
            instance.localPatches.push_back(descriptor.patch);
    }
}

void RemoveAppliedPatches(
    Assets::PrefabInstanceRecord& instance,
    const std::vector<Assets::PrefabOverrideDescriptor>& overrides)
{
    NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT("removing applied prefab override patches");
    instance.localPatches.erase(
        std::remove_if(instance.localPatches.begin(), instance.localPatches.end(), [&overrides](const PatchOperation& patch)
        {
            return std::any_of(overrides.begin(), overrides.end(), [&patch](const auto& descriptor)
            {
                return IsSamePatchIdentity(patch, descriptor.patch);
            });
        }),
        instance.localPatches.end());
}

bool IsSafeProjectRelativeAssetPath(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute())
        return false;
    for (const auto& part : path)
    {
        if (part == "..")
            return false;
    }
    const auto iterator = path.begin();
    return iterator != path.end() && *iterator == "Assets";
}

std::string NormalizeProjectRelativeAssetPath(const std::string& path)
{
    const auto normalized = std::filesystem::path(path).lexically_normal().generic_string();
    return normalized == "." ? std::string {} : normalized;
}

std::optional<std::string> ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool WriteTextFileAtomically(const std::filesystem::path& path, const std::string& contents)
{
    NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT("writing a prefab source file");
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;

    auto temporary = path;
    temporary += ".inspector.tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            return false;
        output << contents;
        if (!output.good())
        {
            output.close();
            std::filesystem::remove(temporary, error);
            return false;
        }
    }

    std::filesystem::rename(temporary, path, error);
    if (!error)
        return true;

    // Windows does not always permit replacement rename. Preserve the old
    // file until a fully written temporary exists, then use overwrite copy as
    // the portable fallback.
    error.clear();
    const bool copied = std::filesystem::copy_file(
        temporary,
        path,
        std::filesystem::copy_options::overwrite_existing,
        error);
    std::filesystem::remove(temporary, error);
    return copied && !error;
}

void MarkOwningSceneDirty(GameObject* owner)
{
    if (owner != nullptr && NLS::Core::ServiceLocator::Contains<Core::EditorActions>())
        NLS::Core::ServiceLocator::Get<Core::EditorActions>().MarkOwningSceneDirty(*owner);
}

Undo::UndoScopeId CapturePrefabOwnerUndoScope(
    const GameObject& owner,
    const PrefabInstanceUndoIdentity& primaryInstance)
{
    if (owner.GetScene() != nullptr && NLS::Core::ServiceLocator::Contains<Core::EditorActions>())
    {
        const auto scope = NLS::Core::ServiceLocator::Get<Core::EditorActions>().GetUndoScopeForScene(
            *owner.GetScene());
        if (scope.IsValid())
            return scope;
    }

    if (NLS::Core::ServiceLocator::Contains<Core::Context>())
    {
        const auto& context = NLS::Core::ServiceLocator::Get<Core::Context>();
        if (context.activePrefabStage.has_value() &&
            context.activePrefabStage->stageScene.get() == owner.GetScene() &&
            !context.activePrefabStage->prefabAssetPath.empty())
        {
            return {Undo::UndoScopeKind::PrefabStage, context.activePrefabStage->prefabAssetPath, 0u};
        }

        if (context.sceneManager.GetCurrentScene() == owner.GetScene())
        {
            const auto scenePath = context.sceneManager.GetCurrentSceneSourcePath();
            return {
                Undo::UndoScopeKind::Scene,
                scenePath.empty() ? std::string("untitled-scene") : scenePath,
                0u};
        }
    }

    // Tests and detached preview scenes do not have an Editor Context. The
    // ObjectGraph root is still durable for the lifetime of this history.
    return {
        Undo::UndoScopeKind::Scene,
        "prefab-instance:" + primaryInstance.rootObject.GetGuid().ToString(),
        0u};
}

bool BuildPrefabUndoState(
    Assets::PrefabInstanceRegistry& instanceRegistry,
    const Assets::PrefabInstanceRecord& instance,
    const bool sourceChanged,
    PrefabInspectorUndoState& state,
    std::string& error)
{
    if (instance.instanceRoot == nullptr || !instance.prefabAssetId.IsValid())
    {
        error = "The prefab instance is no longer available.";
        return false;
    }

    const auto primaryIdentity = MakePrefabInstanceUndoIdentity(instance);
    if (!primaryIdentity.has_value())
    {
        error = "The prefab instance has no stable ObjectGraph identity.";
        return false;
    }

    state.primaryInstance = *primaryIdentity;
    state.prefabAssetId = instance.prefabAssetId;
    state.prefabSubAssetKey = instance.prefabSubAssetKey;
    state.ownerScope = CapturePrefabOwnerUndoScope(*instance.instanceRoot, state.primaryInstance);
    state.sourceChanged = sourceChanged;
    state.instancesBefore = CaptureInstanceUndoSnapshots(
        instanceRegistry,
        state.prefabAssetId,
        state.prefabSubAssetKey);
    const bool containsPrimary = std::any_of(
        state.instancesBefore.begin(),
        state.instancesBefore.end(),
        [&state](const PrefabInstanceUndoSnapshot& snapshot)
        {
            return IsSamePrefabInstanceUndoIdentity(snapshot.identity, state.primaryInstance);
        });
    if (!containsPrimary)
    {
        error = "The prefab instance is no longer connected.";
        return false;
    }
    return true;
}

bool RestorePrefabInstanceSnapshots(
    Assets::PrefabInstanceRegistry& instanceRegistry,
    const PrefabInspectorUndoState& state,
    const std::vector<PrefabInstanceUndoSnapshot>& snapshots,
    const std::shared_ptr<const PrefabArtifact>& source,
    std::string& error)
{
    NLS_EDITOR_UNDO_REQUIRE_MUTATION_CONTEXT("restoring prefab Inspector live state");
    if (!source)
    {
        error = "The prefab source artifact is unavailable.";
        return false;
    }

    struct ResolvedSnapshot
    {
        Assets::PrefabInstanceRecord* instance = nullptr;
        const PrefabInstanceUndoSnapshot* snapshot = nullptr;
    };
    std::vector<ResolvedSnapshot> resolved;
    resolved.reserve(snapshots.size());
    for (const auto& snapshot : snapshots)
    {
        auto* instance = ResolveUndoInstance(instanceRegistry, snapshot, state);
        if (instance == nullptr)
        {
            if (IsSamePrefabInstanceUndoIdentity(snapshot.identity, state.primaryInstance))
            {
                error = "The prefab instance was destroyed before Undo could restore it.";
                return false;
            }
            continue;
        }
        resolved.push_back({instance, &snapshot});
    }

    if (resolved.empty())
    {
        error = "No connected prefab instances are available to restore.";
        return false;
    }

    Assets::PrefabUtilityFacade facade;
    for (const auto& entry : resolved)
    {
        auto overlay = *source;
        if (!entry.snapshot->overrides.empty())
        {
            const auto materialized = facade.MaterializeOverrideView(overlay, entry.snapshot->overrides);
            if (materialized.status != Assets::PrefabOperationStatus::Committed)
            {
                error = DescribeFailure(materialized);
                return false;
            }
        }

        entry.instance->localPatches = entry.snapshot->localPatches;
        const auto refreshed = facade.RefreshConnectedInstance(*entry.instance, overlay);
        if (refreshed.status != Assets::PrefabOperationStatus::Committed)
        {
            error = DescribeFailure(refreshed);
            return false;
        }

        // The overlay is only for rebuilding live state. Keep the registered
        // source graph at the imported base so future override discovery does
        // not treat an Undo reconstruction as an applied source edit.
        entry.instance->UseSharedSourcePrefab(source);
        entry.instance->localPatches = entry.snapshot->localPatches;
    }

    const auto primary = std::find_if(
        resolved.begin(),
        resolved.end(),
        [&state](const ResolvedSnapshot& entry)
        {
            return entry.snapshot != nullptr &&
                IsSamePrefabInstanceUndoIdentity(entry.snapshot->identity, state.primaryInstance);
        });
    if (primary != resolved.end())
        MarkOwningSceneDirty(primary->instance->instanceRoot);
    return true;
}

bool RestorePrefabUndoState(
    const PrefabInspectorUndoState& state,
    const bool restoreAfter,
    std::string& error)
{
    const auto services = ResolvePrefabInspectorServices();
    if (!services.has_value() || services->instanceRegistry == nullptr)
    {
        error = "The prefab instance registry is no longer available.";
        return false;
    }

    auto& instanceRegistry = *services->instanceRegistry;
    const auto& desiredSnapshots = restoreAfter ? state.instancesAfter : state.instancesBefore;
    const auto primary = std::find_if(
        desiredSnapshots.begin(),
        desiredSnapshots.end(),
        [&state](const PrefabInstanceUndoSnapshot& snapshot)
        {
            return IsSamePrefabInstanceUndoIdentity(snapshot.identity, state.primaryInstance);
        });
    if (primary == desiredSnapshots.end())
    {
        error = "The prefab Undo record is incomplete.";
        return false;
    }

    auto* currentPrimary = ResolveUndoInstance(instanceRegistry, *primary, state);
    if (currentPrimary == nullptr)
    {
        error = "The prefab instance was destroyed before Undo could restore it.";
        return false;
    }

    if (!state.sourceChanged)
    {
        const auto rollbackSnapshots = CaptureInstanceUndoSnapshots(
            instanceRegistry,
            state.prefabAssetId,
            state.prefabSubAssetKey);
        auto source = BuildSourceArtifact(*currentPrimary);
        if (RestorePrefabInstanceSnapshots(instanceRegistry, state, desiredSnapshots, source, error))
            return true;

        std::string rollbackError;
        (void)RestorePrefabInstanceSnapshots(instanceRegistry, state, rollbackSnapshots, source, rollbackError);
        return false;
    }

    const auto relativePath = std::filesystem::path(state.sourceAssetPath).lexically_normal();
    if (!IsSafeProjectRelativeAssetPath(relativePath))
    {
        error = "The prefab Undo record contains an unsafe source path.";
        return false;
    }

    const auto projectRoot = services->projectAssetsPath.parent_path();
    if (projectRoot.empty())
    {
        error = "The project asset root is no longer available.";
        return false;
    }
    const auto absolutePath = (projectRoot / relativePath).lexically_normal();
    const auto expectedSource = restoreAfter ? state.sourceBefore : state.sourceAfter;
    const auto desiredSource = restoreAfter ? state.sourceAfter : state.sourceBefore;
    const auto currentSource = ReadTextFile(absolutePath);
    if (!currentSource.has_value())
    {
        error = "The prefab source file could not be read for Undo.";
        return false;
    }
    if (*currentSource != expectedSource)
    {
        error = "The prefab source changed outside Inspector; Undo was not applied.";
        return false;
    }

    Assets::AssetDatabaseFacade database(Assets::MakeProjectEditorAssetRoots(projectRoot));
    if (!database.Refresh())
    {
        error = "The asset database could not be refreshed for prefab Undo.";
        return false;
    }

    const auto rollbackSnapshots = CaptureInstanceUndoSnapshots(
        instanceRegistry,
        state.prefabAssetId,
        state.prefabSubAssetKey);
    if (!WriteTextFileAtomically(absolutePath, desiredSource) ||
        !database.ReimportAsset(state.sourceAssetPath))
    {
        (void)WriteTextFileAtomically(absolutePath, *currentSource);
        (void)database.ReimportAsset(state.sourceAssetPath);
        error = "The prefab source was restored because Undo reimport failed.";
        return false;
    }

    auto importedSource = database.LoadPrefabArtifactByAssetId(
        state.prefabAssetId,
        state.prefabSubAssetKey);
    if (!importedSource.has_value())
    {
        (void)WriteTextFileAtomically(absolutePath, *currentSource);
        (void)database.ReimportAsset(state.sourceAssetPath);
        error = "The prefab source was restored because Undo could not load the reimported artifact.";
        return false;
    }

    auto sharedSource = std::make_shared<const PrefabArtifact>(std::move(*importedSource));
    if (RestorePrefabInstanceSnapshots(instanceRegistry, state, desiredSnapshots, sharedSource, error))
        return true;

    // A failed live-state refresh must not leave a new source file behind.
    // Best-effort reconstruction of the state that preceded the Undo attempt
    // keeps failure behavior transactional even for structural overrides.
    (void)WriteTextFileAtomically(absolutePath, *currentSource);
    if (database.ReimportAsset(state.sourceAssetPath))
    {
        if (auto rollbackSource = database.LoadPrefabArtifactByAssetId(
                state.prefabAssetId,
                state.prefabSubAssetKey);
            rollbackSource.has_value())
        {
            std::string rollbackError;
            (void)RestorePrefabInstanceSnapshots(
                instanceRegistry,
                state,
                rollbackSnapshots,
                std::make_shared<const PrefabArtifact>(std::move(*rollbackSource)),
                rollbackError);
        }
    }
    return false;
}

Assets::PrefabInstanceRecord* ResolvePrimaryUndoInstance(
    Assets::PrefabInstanceRegistry& instanceRegistry,
    const PrefabInspectorUndoState& state)
{
    const auto primary = std::find_if(
        state.instancesBefore.begin(),
        state.instancesBefore.end(),
        [&state](const PrefabInstanceUndoSnapshot& snapshot)
        {
            return IsSamePrefabInstanceUndoIdentity(snapshot.identity, state.primaryInstance);
        });
    return primary == state.instancesBefore.end()
        ? nullptr
        : ResolveUndoInstance(instanceRegistry, *primary, state);
}

bool RestoreInitialPrefabUndoState(
    PrefabInspectorUndoState& state,
    std::string& error,
    bool& rollbackFailed)
{
    std::string rollbackError;
    if (RestorePrefabUndoState(state, false, rollbackError))
        return true;

    rollbackFailed = true;
    if (!rollbackError.empty())
    {
        if (!error.empty())
            error += " ";
        error += "The previous prefab state could not be restored: " + rollbackError;
    }
    return false;
}

bool ApplyInitialPrefabRevert(
    PrefabInspectorUndoState& state,
    std::string& error,
    bool& rollbackFailed)
{
    const auto services = ResolvePrefabInspectorServices();
    if (!services.has_value() || services->instanceRegistry == nullptr)
    {
        error = "The prefab instance registry is no longer available.";
        return false;
    }

    auto& instanceRegistry = *services->instanceRegistry;
    auto* primary = ResolvePrimaryUndoInstance(instanceRegistry, state);
    if (primary == nullptr)
    {
        error = "The prefab instance is no longer available.";
        return false;
    }
    if (state.requestedOverrides.empty())
    {
        error = "The prefab override action no longer has a stable patch payload.";
        return false;
    }

    const auto source = BuildSourceArtifact(*primary);
    SynchronizeLocalPatches(*primary, state.requestedOverrides);
    const auto result = Assets::PrefabUtilityFacade().RevertOverrideGroup(*primary, state.requestedOverrides);
    if (result.status != Assets::PrefabOperationStatus::Committed)
    {
        error = DescribeFailure(result);
        (void)RestoreInitialPrefabUndoState(state, error, rollbackFailed);
        return false;
    }

    state.instancesAfter = CaptureInstanceUndoSnapshots(
        instanceRegistry,
        state.prefabAssetId,
        state.prefabSubAssetKey);
    if (!IsPrefabUndoStateValid(state))
    {
        error = "The prefab instance could not be resolved after reverting its override.";
        (void)RestoreInitialPrefabUndoState(state, error, rollbackFailed);
        return false;
    }

    MarkOwningSceneDirty(primary->instanceRoot);
    return true;
}

bool ApplyInitialPrefabSourceChange(
    PrefabInspectorUndoState& state,
    std::string& error,
    bool& rollbackFailed)
{
    const auto services = ResolvePrefabInspectorServices();
    if (!services.has_value() || services->instanceRegistry == nullptr)
    {
        error = "The prefab instance registry is no longer available.";
        return false;
    }

    const auto projectRoot = services->projectAssetsPath.parent_path();
    if (projectRoot.empty())
    {
        error = "The project asset root is no longer available.";
        return false;
    }
    const auto relativePath = std::filesystem::path(state.sourceAssetPath).lexically_normal();
    if (!IsSafeProjectRelativeAssetPath(relativePath))
    {
        error = "The prefab source path is not a safe project asset path.";
        return false;
    }
    if (state.requestedOverrides.empty())
    {
        error = "The prefab override action no longer has a stable patch payload.";
        return false;
    }

    auto& instanceRegistry = *services->instanceRegistry;
    auto* primary = ResolvePrimaryUndoInstance(instanceRegistry, state);
    if (primary == nullptr)
    {
        error = "The prefab instance is no longer available.";
        return false;
    }

    const auto absolutePath = (projectRoot / relativePath).lexically_normal();
    const auto currentSource = ReadTextFile(absolutePath);
    if (!currentSource.has_value())
    {
        error = "The prefab source file could not be read before applying changes.";
        return false;
    }
    if (*currentSource != state.sourceBefore)
    {
        error = "The prefab source changed outside Inspector; the override was not applied.";
        return false;
    }

    Assets::AssetDatabaseFacade database(Assets::MakeProjectEditorAssetRoots(projectRoot));
    if (!database.Refresh())
    {
        error = "The asset database could not be refreshed before applying prefab overrides.";
        return false;
    }
    auto mutablePrefab = database.LoadPrefabArtifactByAssetId(
        state.prefabAssetId,
        state.prefabSubAssetKey);
    if (!mutablePrefab.has_value())
    {
        error = "The editable prefab source artifact could not be loaded.";
        return false;
    }

    const auto result = Assets::PrefabUtilityFacade().ApplyOverrideGroup(
        *mutablePrefab,
        state.requestedOverrides,
        primary->generatedReadOnly || mutablePrefab->generatedModelPrefab);
    if (result.status != Assets::PrefabOperationStatus::Committed)
    {
        error = DescribeFailure(result);
        return false;
    }

    const auto updatedSource = NLS::Engine::Serialize::ObjectGraphWriter::Write(mutablePrefab->graph);
    if (!WriteTextFileAtomically(absolutePath, updatedSource))
    {
        error = "The prefab source file could not be written; no override was applied.";
        return false;
    }
    state.sourceAfter = updatedSource;

    if (!database.ReimportAsset(state.sourceAssetPath))
    {
        error = "The prefab source reimport failed.";
        (void)RestoreInitialPrefabUndoState(state, error, rollbackFailed);
        return false;
    }

    auto importedPrefab = database.LoadPrefabArtifactByAssetId(
        state.prefabAssetId,
        state.prefabSubAssetKey);
    if (!importedPrefab.has_value())
    {
        error = "The reimported prefab source artifact could not be loaded.";
        (void)RestoreInitialPrefabUndoState(state, error, rollbackFailed);
        return false;
    }

    const auto sharedSource = std::make_shared<const PrefabArtifact>(std::move(*importedPrefab));
    RemoveAppliedPatches(*primary, state.requestedOverrides);
    for (auto* instance : instanceRegistry.FindInstancesForPrefab(state.prefabAssetId, state.prefabSubAssetKey))
    {
        if (instance != nullptr)
            instance->UseSharedSourcePrefab(sharedSource);
    }

    state.instancesAfter = CaptureInstanceUndoSnapshots(
        instanceRegistry,
        state.prefabAssetId,
        state.prefabSubAssetKey);
    if (!IsPrefabUndoStateValid(state) ||
        !RestorePrefabInstanceSnapshots(instanceRegistry, state, state.instancesAfter, sharedSource, error))
    {
        if (error.empty())
            error = "The connected prefab instances could not be refreshed.";
        (void)RestoreInitialPrefabUndoState(state, error, rollbackFailed);
        return false;
    }
    return true;
}

class PrefabInspectorUndoCommand final : public Undo::IEditorUndoCommand
{
public:
    PrefabInspectorUndoCommand(Undo::UndoCommandInfo info, PrefabInspectorUndoState state)
        : m_info(std::move(info))
        , m_state(std::move(state))
    {
    }

    [[nodiscard]] Undo::UndoCommandInfo GetInfo() const override
    {
        return m_info;
    }

    [[nodiscard]] Undo::UndoValidationResult Validate(
        const Undo::UndoDirection direction,
        const Undo::UndoExecutionContext&) const override
    {
        const bool isInitialRedo = direction == Undo::UndoDirection::Redo && m_state.instancesAfter.empty();
        if (!IsPrefabUndoStateValid(m_state, !isInitialRedo))
        {
            return Undo::UndoValidationResult::Stale(
                "The prefab or its recorded connected instance no longer exists.");
        }

        if (!m_state.sourceChanged)
            return isInitialRedo && m_state.requestedOverrides.empty()
                ? Undo::UndoValidationResult::Failed("The prefab override action no longer has a stable patch payload.")
                : Undo::UndoValidationResult::Ready();

        const auto relativePath = std::filesystem::path(m_state.sourceAssetPath).lexically_normal();
        if (!IsSafeProjectRelativeAssetPath(relativePath))
            return Undo::UndoValidationResult::Failed("The prefab Undo record contains an unsafe source path.");

        const auto services = ResolvePrefabInspectorServices();
        if (!services.has_value() || services->projectAssetsPath.empty())
        {
            return Undo::UndoValidationResult::Stale(
                "The prefab source cannot be resolved in the current editor context.");
        }

        const auto projectRoot = services->projectAssetsPath.parent_path();
        if (projectRoot.empty())
        {
            return Undo::UndoValidationResult::Stale(
                "The prefab source project root is no longer available.");
        }

        const auto currentSource = ReadTextFile((projectRoot / relativePath).lexically_normal());
        if (!currentSource.has_value())
            return Undo::UndoValidationResult::Stale("The prefab source file no longer exists.");

        const auto& expectedSource = direction == Undo::UndoDirection::Redo
            ? m_state.sourceBefore
            : m_state.sourceAfter;
        return *currentSource == expectedSource
            ? Undo::UndoValidationResult::Ready()
            : Undo::UndoValidationResult::Conflict(
                "The prefab source changed outside Inspector; Undo was not applied.");
    }

    Undo::UndoStartResult Apply(
        const Undo::UndoDirection direction,
        const Undo::UndoExecutionContext&,
        Undo::UndoCompletion) override
    {
        std::string error;
        bool rollbackFailed = false;
        const bool isInitialRedo = direction == Undo::UndoDirection::Redo && m_state.instancesAfter.empty();
        const bool applied = isInitialRedo
            ? (m_state.sourceChanged
                ? ApplyInitialPrefabSourceChange(m_state, error, rollbackFailed)
                : ApplyInitialPrefabRevert(m_state, error, rollbackFailed))
            : RestorePrefabUndoState(m_state, direction == Undo::UndoDirection::Redo, error);
        return applied
            ? Undo::UndoStartResult::Completed()
            : Undo::UndoStartResult::Failed(error.empty()
                ? "The prefab override state could not be restored."
                : std::move(error), rollbackFailed);
    }

    bool TryMergeFrom(const Undo::IEditorUndoCommand&) override
    {
        // Applying/reverting prefab overrides changes structure and may touch a
        // source asset. It is never a time-mergeable inspector edit.
        return false;
    }

private:
    Undo::UndoCommandInfo m_info;
    PrefabInspectorUndoState m_state;
};

Undo::UndoCommandInfo BuildPrefabUndoCommandInfo(const PrefabInspectorUndoState& state, std::string label)
{
    Undo::UndoCommandInfo info;
    info.label = std::move(label);
    info.sourceModule = "Prefab Inspector";
    if (state.ownerScope.IsValid())
        info.scopes.push_back(state.ownerScope);

    const auto sourceKey = state.prefabAssetId.IsValid()
        ? state.prefabAssetId.ToString()
        : state.sourceAssetPath;
    if (!sourceKey.empty())
    {
        const Undo::UndoScopeId assetScope {Undo::UndoScopeKind::Asset, sourceKey, 0u};
        info.scopes.push_back(assetScope);
        if (state.sourceChanged)
            info.writeThroughScopes.push_back(assetScope);
    }
    info.affected.push_back("prefab:" + sourceKey);
    info.affected.push_back("prefab-instance:" + state.primaryInstance.rootObject.GetGuid().ToString());
    info.estimatedBytes = sizeof(PrefabInspectorUndoState) +
        state.sourceBefore.size() + state.sourceAfter.size() +
        (state.instancesBefore.size() * 2u + state.instancesAfter.size()) * sizeof(PrefabInstanceUndoSnapshot) +
        state.requestedOverrides.size() * sizeof(Assets::PrefabOverrideDescriptor);
    return info;
}

bool ExecutePrefabUndo(
    PrefabInspectorUndoState state,
    std::string label,
    std::string& error)
{
    if (!NLS::Core::ServiceLocator::Contains<Undo::EditorUndoService>())
    {
        error = "The global Undo service is unavailable; the prefab change was not applied.";
        return false;
    }

    auto result = NLS::Core::ServiceLocator::Get<Undo::EditorUndoService>().Execute(
        std::make_unique<PrefabInspectorUndoCommand>(
            BuildPrefabUndoCommandInfo(state, std::move(label)),
            std::move(state)));
    if (result.Succeeded())
        return true;

    error = result.message.empty()
        ? "The global Undo service rejected the prefab change."
        : "The global Undo service rejected the prefab change: " + result.message;
    return false;
}

bool ApplyOverridesToSource(
    PrefabOverrideSnapshot& snapshot,
    const std::vector<Assets::PrefabOverrideDescriptor>& overrides,
    std::string& error)
{
    const auto services = ResolvePrefabInspectorServices();
    if (!services.has_value() || services->instanceRegistry == nullptr || snapshot.instance == nullptr)
    {
        error = "The prefab instance is no longer available.";
        return false;
    }
    if (!NLS::Core::ServiceLocator::Contains<Undo::EditorUndoService>())
    {
        error = "The global Undo service is unavailable; the prefab change was not applied.";
        return false;
    }

    const auto projectRoot = services->projectAssetsPath.parent_path();
    if (projectRoot.empty())
    {
        error = "The project asset root is no longer available.";
        return false;
    }

    auto& instanceRegistry = *services->instanceRegistry;
    PrefabInspectorUndoState undoState;
    if (!BuildPrefabUndoState(instanceRegistry, *snapshot.instance, true, undoState, error))
        return false;
    undoState.requestedOverrides = overrides;

    Assets::AssetDatabaseFacade database(Assets::MakeProjectEditorAssetRoots(projectRoot));
    if (!database.Refresh())
    {
        error = "The asset database could not be refreshed before applying prefab overrides.";
        return false;
    }

    const auto assetPath = database.GUIDToAssetPath(undoState.prefabAssetId.ToString());
    const auto relativePath = std::filesystem::path(assetPath).lexically_normal();
    if (!IsSafeProjectRelativeAssetPath(relativePath))
    {
        error = "The prefab source path is not a safe project asset path.";
        return false;
    }
    const auto sourceBefore = ReadTextFile((projectRoot / relativePath).lexically_normal());
    if (!sourceBefore.has_value())
    {
        error = "The prefab source file could not be read before applying changes.";
        return false;
    }

    undoState.sourceAssetPath = NormalizeProjectRelativeAssetPath(assetPath);
    undoState.sourceBefore = *sourceBefore;
    return ExecutePrefabUndo(
        std::move(undoState),
        overrides.size() == 1u ? "Apply Prefab Override" : "Apply Prefab Overrides",
        error);
}

bool RevertOverridesOnInstance(
    PrefabOverrideSnapshot& snapshot,
    const std::vector<Assets::PrefabOverrideDescriptor>& overrides,
    std::string& error)
{
    if (snapshot.instance == nullptr)
    {
        error = "The prefab instance is no longer available.";
        return false;
    }
    const auto services = ResolvePrefabInspectorServices();
    if (!services.has_value() || services->instanceRegistry == nullptr)
    {
        error = "The prefab instance registry is no longer available.";
        return false;
    }
    if (!NLS::Core::ServiceLocator::Contains<Undo::EditorUndoService>())
    {
        error = "The global Undo service is unavailable; the prefab change was not applied.";
        return false;
    }

    PrefabInspectorUndoState undoState;
    if (!BuildPrefabUndoState(*services->instanceRegistry, *snapshot.instance, false, undoState, error))
        return false;
    undoState.requestedOverrides = overrides;
    return ExecutePrefabUndo(
        std::move(undoState),
        overrides.size() == 1u ? "Revert Prefab Override" : "Revert Prefab Overrides",
        error);
}

std::vector<PrefabInspectorAction> BuildActions(
    const std::vector<Assets::PrefabOverrideDescriptor>& overrides,
    const bool isComponent,
    const bool isTarget,
    const PrefabOverrideSnapshot* snapshot)
{
    if (overrides.empty() || snapshot == nullptr || snapshot->instance == nullptr)
        return {};

    const bool canApply = std::any_of(overrides.begin(), overrides.end(), [](const auto& descriptor)
    {
        return descriptor.canApply;
    }) && !snapshot->instance->generatedReadOnly && [&]
    {
        const auto services = ResolvePrefabInspectorServices();
        return services.has_value() && !services->projectAssetsPath.empty();
    }();
    const bool canRevert = std::any_of(overrides.begin(), overrides.end(), [](const auto& descriptor)
    {
        return descriptor.canRevert;
    });
    const std::string suffix = isTarget ? " All Overrides" : isComponent ? " Component Override" : " Override";
    return {
        {PrefabInspectorActionKind::Apply, std::string("Apply") + suffix,
            canApply ? "Apply the selected prefab override to the source asset." : "The source prefab is read-only or unavailable.", canApply},
        {PrefabInspectorActionKind::Revert, std::string("Revert") + suffix,
            canRevert ? "Restore this value from the source prefab." : "This override cannot be reverted.", canRevert}
    };
}

uint64_t MixGuidFingerprint(uint64_t hash, const NLS::Guid& guid)
{
    for (const auto byte : guid.GetBytes())
    {
        hash ^= static_cast<uint64_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

// Local patches are the only prefab-override state that survives selection
// changes. Hashing their identities keeps the cached marker index honest
// without serializing any live object.
uint64_t ComputeLocalPatchFingerprint(const Assets::PrefabInstanceRecord& instance)
{
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](const uint64_t value)
    {
        hash ^= value;
        hash *= 1099511628211ull;
    };

    mix(instance.localPatches.size());
    for (const auto& patch : instance.localPatches)
    {
        mix(static_cast<uint64_t>(patch.type));
        mix(patch.hasIndex ? 1u : 0u);
        mix(static_cast<uint64_t>(patch.index));
        hash = MixGuidFingerprint(hash, patch.target.GetGuid());
        hash = MixGuidFingerprint(hash, patch.object.GetGuid());
        for (const auto character : patch.property)
            mix(static_cast<uint64_t>(static_cast<unsigned char>(character)) + 1u);
        mix(0u);
    }
    return hash;
}

struct SourceObjectOverrideStates
{
    std::unordered_set<std::string> properties;
};

// Per (prefab instance, selected live object) marker index. Building it is the
// expensive discovery pass; every Inspector node afterwards is answered from
// these tables instead of rescanning serialized properties or override lists.
struct OwnerOverrideIndex
{
    const Assets::PrefabInstanceRecord* instance = nullptr;
    const void* registryIdentity = nullptr;
    uint64_t registryRevision = 0u;
    uint64_t patchFingerprint = 0u;
    const void* sourceIdentity = nullptr;
    std::shared_ptr<const PrefabArtifact> source;
    bool hasInstance = false;
    std::unordered_map<ObjectId, SourceObjectOverrideStates> propertiesBySource;
    std::unordered_set<ObjectId> structuralSources;
    std::unordered_set<ObjectId> structuralOwnerSources;
    std::unordered_map<const Component*, ObjectId> sourceByComponent;
    bool componentSourcesBuilt = false;
};

void BuildOwnerOverrideIndex(
    OwnerOverrideIndex& index,
    Assets::PrefabInstanceRegistry& registry,
    Assets::PrefabInstanceRecord& instance,
    GameObject& owner)
{
    index = OwnerOverrideIndex {};
    index.instance = &instance;
    index.registryIdentity = &registry;
    index.registryRevision = registry.GetRevision();
    index.patchFingerprint = ComputeLocalPatchFingerprint(instance);

    index.source = BuildSourceArtifact(instance);
    index.sourceIdentity = instance.sharedSourcePrefab != nullptr
        ? static_cast<const void*>(instance.sharedSourcePrefab.get())
        : static_cast<const void*>(&instance.sourceGraph);
    if (index.source == nullptr || index.source->graph.objects.empty())
        return;

    index.hasInstance = true;
#if defined(NLS_ENABLE_TEST_HOOKS)
    ++g_markerDiscoveryCount;
#endif
    const auto overrides = Assets::PrefabUtilityFacade().GetPrefabOverrideMarkersForObject(
        *index.source,
        instance,
        owner,
        true);
    for (const auto& descriptor : overrides)
    {
        if (IsStructuralOverride(descriptor))
        {
            index.structuralSources.insert(descriptor.sourceObject);
            index.structuralSources.insert(descriptor.patch.target);
            index.structuralSources.insert(descriptor.patch.object);
            if (descriptor.propertyPath == "components")
            {
                index.structuralOwnerSources.insert(descriptor.sourceObject);
                index.structuralOwnerSources.insert(descriptor.patch.target);
            }
            continue;
        }

        if ((descriptor.kind == Assets::PrefabOverrideKind::Property ||
                descriptor.kind == Assets::PrefabOverrideKind::DefaultOverride) &&
            descriptor.patch.type == PatchOperationType::ReplaceProperty)
        {
            index.propertiesBySource[descriptor.sourceObject].properties.insert(descriptor.propertyPath);
        }
    }
}

// Mirrors PrefabEditorWorkflow's component-to-source matching (nth component of
// a type maps to the nth source component of that type) but resolves the whole
// owner once instead of rescanning every component per Inspector row.
void BuildComponentSourceMap(
    OwnerOverrideIndex& index,
    const GameObject& owner,
    const ObjectId& sourceOwner)
{
    index.componentSourcesBuilt = true;
    index.sourceByComponent.clear();
    if (index.source == nullptr)
        return;

    const auto* sourceOwnerRecord = FindObjectRecord(index.source->graph, sourceOwner);
    if (sourceOwnerRecord == nullptr)
        return;

    const auto sourceComponents = GetOwnedReferences(*sourceOwnerRecord, "components");
    std::unordered_map<std::string, std::vector<ObjectId>> sourceIdsByType;
    sourceIdsByType.reserve(sourceComponents.size());
    for (const auto& sourceComponent : sourceComponents)
    {
        const auto* record = FindObjectRecord(index.source->graph, sourceComponent);
        if (record == nullptr)
            continue;
        sourceIdsByType[record->typeName].push_back(sourceComponent);
    }

    std::unordered_map<std::string, size_t> nextSourceIndex;
    nextSourceIndex.reserve(sourceIdsByType.size());
    for (const auto& candidate : owner.GetComponents())
    {
        if (candidate == nullptr)
            continue;

        const std::string typeName = candidate->GetType().GetName();
        const auto ids = sourceIdsByType.find(typeName);
        if (ids == sourceIdsByType.end())
            continue;

        auto& next = nextSourceIndex[typeName];
        if (next >= ids->second.size())
            continue;

        index.sourceByComponent.emplace(candidate.get(), ids->second[next]);
        ++next;
    }
}

std::optional<ObjectId> ResolveSourceComponent(
    OwnerOverrideIndex& index,
    const GameObject& owner,
    const ObjectId& sourceOwner,
    const Component& component)
{
    if (!index.componentSourcesBuilt)
        BuildComponentSourceMap(index, owner, sourceOwner);

    const auto found = index.sourceByComponent.find(&component);
    return found != index.sourceByComponent.end() ? std::optional<ObjectId> {found->second} : std::nullopt;
}

PrefabOverrideState ComputeNodeOverrideState(OwnerOverrideIndex& index, const PropertyNode& node)
{
    if (!index.hasInstance || index.instance == nullptr)
        return PrefabOverrideState::None;

    auto* owner = ResolveGameObject(node.target);
    if (owner == nullptr)
        return PrefabOverrideState::None;

    const auto sourceOwner = FindSourceObject(*index.instance, *owner);
    if (!sourceOwner.has_value())
        return PrefabOverrideState::None;

    ObjectId sourceObject = *sourceOwner;
    if (const auto* component = dynamic_cast<const Component*>(
            NativeReflectionAdapter::ResolveObject(node.target)); component != nullptr)
    {
        const auto sourceComponent = ResolveSourceComponent(index, *owner, *sourceOwner, *component);
        if (!sourceComponent.has_value())
            return PrefabOverrideState::None;
        sourceObject = *sourceComponent;
    }

    if (node.isComponent)
    {
        const bool structural =
            index.structuralSources.find(sourceObject) != index.structuralSources.end() ||
            index.structuralOwnerSources.find(*sourceOwner) != index.structuralOwnerSources.end();
        return structural ? PrefabOverrideState::StructuralOverride : PrefabOverrideState::None;
    }

    if (node.fieldName.empty())
        return PrefabOverrideState::None;

    const auto found = index.propertiesBySource.find(sourceObject);
    if (found == index.propertiesBySource.end())
        return PrefabOverrideState::None;

    return found->second.properties.find(node.fieldName) != found->second.properties.end()
        ? PrefabOverrideState::PropertyOverride
        : PrefabOverrideState::None;
}

bool ResolveNodeSnapshot(
    const PropertyNode& node,
    PrefabSnapshotCache& cache,
    PrefabOverrideSnapshot*& snapshot,
    GameObject*& owner)
{
    if (GetPropertyNodeTargets(node).size() > 1u)
        return false;
    owner = ResolveGameObject(node.target);
    snapshot = ResolveSnapshot(node.target, cache);
    return owner != nullptr && snapshot != nullptr;
}
} // namespace

struct PrefabOverrideQuery::Impl
{
    std::unordered_map<const GameObject*, OwnerOverrideIndex> indexesByOwner;
    std::unordered_map<std::string, PrefabOverrideState> states;
    std::unordered_map<const GameObject*, OwnerOverrideIndex*> resolvedThisRefresh;
    std::unordered_map<const GameObject*, const Assets::PrefabInstanceRecord*> instanceByOwner;
    const void* registryIdentity = nullptr;
    uint64_t registryRevision = 0u;
    uint64_t refreshSerial = 0u;
};

PrefabOverrideQuery::PrefabOverrideQuery()
    : m_impl(std::make_unique<Impl>())
{
}

PrefabOverrideQuery::~PrefabOverrideQuery() = default;

void PrefabOverrideQuery::BeginRefresh()
{
    if (const auto services = ResolvePrefabInspectorServices();
        services.has_value() && services->instanceRegistry != nullptr)
    {
        auto& registry = *services->instanceRegistry;
        const uint64_t revision = registry.GetRevision();
        if (m_impl->registryIdentity != &registry || m_impl->registryRevision != revision)
        {
            m_impl->registryIdentity = &registry;
            m_impl->registryRevision = revision;
            m_impl->indexesByOwner.clear();
            m_impl->instanceByOwner.clear();
        }
        else if (m_impl->indexesByOwner.size() >= 128u)
        {
            // Bounded selection cache: a long editing session must not retain
            // every prefab instance the user has inspected.
            m_impl->indexesByOwner.clear();
        }
    }

    ++m_impl->refreshSerial;
    if (m_impl->refreshSerial == 0u)
        m_impl->refreshSerial = 1u;
    m_impl->states.clear();
    m_impl->resolvedThisRefresh.clear();
}

void PrefabOverrideQuery::InvalidateAll()
{
    m_impl->indexesByOwner.clear();
    m_impl->instanceByOwner.clear();
    m_impl->states.clear();
    m_impl->resolvedThisRefresh.clear();
}

PrefabOverrideState PrefabOverrideQuery::GetState(const PropertyNode& node)
{
    if (GetPropertyNodeTargets(node).size() > 1u)
        return PrefabOverrideState::None;

    std::string key = node.path.ToString();
    key += "|";
    key += node.fieldName;
    key += "|";
    key += node.isComponent ? "component" : "property";
    key += "|";
    key += std::to_string(node.target.instanceId);
    key += "|";
    key += node.target.objectIdentifier.guid.ToString();
    key += ":";
    key += std::to_string(node.target.objectIdentifier.localIdentifierInFile);
    if (const auto found = m_impl->states.find(key); found != m_impl->states.end())
        return found->second;

    PrefabOverrideState state = PrefabOverrideState::None;
    if (auto* owner = ResolveGameObject(node.target))
    {
        OwnerOverrideIndex* index = nullptr;
        if (const auto resolved = m_impl->resolvedThisRefresh.find(owner);
            resolved != m_impl->resolvedThisRefresh.end())
        {
            index = resolved->second;
        }
        else
        {
            const auto services = ResolvePrefabInspectorServices();
            if (services.has_value() && services->instanceRegistry != nullptr)
            {
                auto& registry = *services->instanceRegistry;
                if (auto* instance = registry.FindInstance(*owner);
                    instance != nullptr && !instance->unpacked && instance->prefabAssetId.IsValid())
                {
                    const auto patchFingerprint = ComputeLocalPatchFingerprint(*instance);
                    const void* sourceIdentity = instance->sharedSourcePrefab != nullptr
                        ? static_cast<const void*>(instance->sharedSourcePrefab.get())
                        : static_cast<const void*>(&instance->sourceGraph);

                    auto& candidate = m_impl->indexesByOwner[owner];
                    const bool matches =
                        candidate.instance == instance &&
                        candidate.registryIdentity == &registry &&
                        candidate.registryRevision == registry.GetRevision() &&
                        candidate.patchFingerprint == patchFingerprint &&
                        candidate.sourceIdentity == sourceIdentity;
                    if (!matches)
                        BuildOwnerOverrideIndex(candidate, registry, *instance, *owner);
                    index = &candidate;
                }
            }

            m_impl->resolvedThisRefresh.emplace(owner, index);
        }

        if (index != nullptr)
            state = ComputeNodeOverrideState(*index, node);
    }
    m_impl->states.emplace(std::move(key), state);
    return state;
}

bool PrefabOverrideQuery::HasInstance(const InspectorTargetHandle& target) const
{
    const auto services = ResolvePrefabInspectorServices();
    if (!services.has_value() || services->instanceRegistry == nullptr)
        return false;

    auto* owner = ResolveGameObject(target);
    if (owner == nullptr)
        return false;

    auto& registry = *services->instanceRegistry;
    const uint64_t revision = registry.GetRevision();
    if (m_impl->registryIdentity != &registry || m_impl->registryRevision != revision)
    {
        m_impl->registryIdentity = &registry;
        m_impl->registryRevision = revision;
        m_impl->indexesByOwner.clear();
        m_impl->instanceByOwner.clear();
    }

    const Assets::PrefabInstanceRecord* instance = nullptr;
    if (const auto found = m_impl->instanceByOwner.find(owner);
        found != m_impl->instanceByOwner.end())
    {
        instance = found->second;
    }
    else
    {
        instance = registry.FindInstance(*owner);
        m_impl->instanceByOwner.emplace(owner, instance);
    }

    return instance != nullptr && !instance->unpacked && instance->prefabAssetId.IsValid();
}

void RefreshPrefabOverrideStates(
    InspectorPresentation& tree,
    PrefabOverrideQuery& query)
{
    query.BeginRefresh();
    for (auto& root : tree.roots)
        RefreshNodeOverrides(root, query);
    for (auto& node : tree.auxiliary)
        RefreshNodeOverrides(node, query);
}

void RefreshPrefabOverrideStates(InspectorPresentation& tree)
{
    PrefabOverrideQuery query;
    RefreshPrefabOverrideStates(tree, query);
}

#if defined(NLS_ENABLE_TEST_HOOKS)
uint64_t TestHooks::GetPrefabOverrideMarkerDiscoveryCountForTesting()
{
    return g_markerDiscoveryCount;
}

void TestHooks::ResetPrefabOverrideMarkerDiscoveryCountForTesting()
{
    g_markerDiscoveryCount = 0u;
}

TestHooks::ScopedPrefabInspectorServicesForTesting::ScopedPrefabInspectorServicesForTesting(
    Assets::PrefabInstanceRegistry& instanceRegistry,
    std::filesystem::path projectAssetsPath)
    : m_previousInstanceRegistry(g_testInstanceRegistry),
      m_previousProjectAssetsPath(std::move(g_testProjectAssetsPath))
{
    g_testInstanceRegistry = &instanceRegistry;
    g_testProjectAssetsPath = std::move(projectAssetsPath);
}

TestHooks::ScopedPrefabInspectorServicesForTesting::~ScopedPrefabInspectorServicesForTesting()
{
    g_testInstanceRegistry = m_previousInstanceRegistry;
    g_testProjectAssetsPath = std::move(m_previousProjectAssetsPath);
}
#endif

PrefabOverrideState GetPrefabOverrideState(const PropertyNode& node)
{
    PrefabSnapshotCache cache;
    PrefabOverrideSnapshot* snapshot = nullptr;
    GameObject* owner = nullptr;
    if (!ResolveNodeSnapshot(node, cache, snapshot, owner))
        return PrefabOverrideState::None;

    return CollectOverridesForNode(node, *snapshot).empty()
        ? PrefabOverrideState::None
        : (node.isComponent
            ? PrefabOverrideState::StructuralOverride
            : PrefabOverrideState::PropertyOverride);
}

std::vector<PrefabInspectorAction> GetPrefabInspectorActions(const PropertyNode& node)
{
    PrefabSnapshotCache cache;
    PrefabOverrideSnapshot* snapshot = nullptr;
    GameObject* owner = nullptr;
    if (!ResolveNodeSnapshot(node, cache, snapshot, owner))
        return {};
    return BuildActions(
        CollectOverridesForNode(node, *snapshot),
        node.isComponent,
        false,
        snapshot);
}

std::vector<PrefabInspectorAction> GetPrefabInspectorActions(const InspectorTargetSet& targets)
{
    const auto* target = targets.Primary();
    if (target == nullptr)
        return {};

    PrefabSnapshotCache cache;
    auto* snapshot = ResolveSnapshot(*target, cache);
    if (snapshot == nullptr || snapshot->source == nullptr || snapshot->instance == nullptr)
        return {};

    auto fullSnapshot = *snapshot;
    fullSnapshot.overrides = Assets::PrefabUtilityFacade().GetPrefabOverrides(
        *fullSnapshot.source,
        *fullSnapshot.instance,
        true);
    return BuildActions(
        fullSnapshot.overrides,
        false,
        true,
        &fullSnapshot);
}

bool ExecutePrefabInspectorAction(
    const PropertyNode& node,
    const PrefabInspectorActionKind action,
    std::string& error)
{
    PrefabSnapshotCache cache;
    PrefabOverrideSnapshot* snapshot = nullptr;
    GameObject* owner = nullptr;
    if (!ResolveNodeSnapshot(node, cache, snapshot, owner))
    {
        error = "This property is not part of a live prefab instance.";
        return false;
    }

    const auto overrides = CollectOverridesForNode(node, *snapshot);
    if (overrides.empty())
    {
        error = "This property no longer has a prefab override.";
        return false;
    }
    return action == PrefabInspectorActionKind::Apply
        ? ApplyOverridesToSource(*snapshot, overrides, error)
        : RevertOverridesOnInstance(*snapshot, overrides, error);
}

bool ExecutePrefabInspectorAction(
    const InspectorTargetSet& targets,
    const PrefabInspectorActionKind action,
    std::string& error)
{
    const auto* target = targets.Primary();
    if (target == nullptr)
    {
        error = "There is no selected prefab instance.";
        return false;
    }

    PrefabSnapshotCache cache;
    auto* snapshot = ResolveSnapshot(*target, cache);
    auto* owner = ResolveGameObject(*target);
    if (snapshot == nullptr || snapshot->source == nullptr || snapshot->instance == nullptr || owner == nullptr)
    {
        error = "The selected target has no prefab overrides.";
        return false;
    }

    auto fullSnapshot = *snapshot;
    fullSnapshot.overrides = Assets::PrefabUtilityFacade().GetPrefabOverrides(
        *fullSnapshot.source,
        *fullSnapshot.instance,
        true);
    if (fullSnapshot.overrides.empty())
    {
        error = "The selected target has no prefab overrides.";
        return false;
    }
    return action == PrefabInspectorActionKind::Apply
        ? ApplyOverridesToSource(fullSnapshot, fullSnapshot.overrides, error)
        : RevertOverridesOnInstance(fullSnapshot, fullSnapshot.overrides, error);
}
} // namespace NLS::Editor::Inspector

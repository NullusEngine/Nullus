#pragma once

#include <memory>
#include <optional>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Components/Component.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Components/TransformComponent.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ServiceLocator.h"
#include "GameObject.h"
#include "Color.h"
#include "Profiling/PerformanceStageStats.h"
#include "Rect.h"
#include "Reflection/Field.h"
#include "Rendering/ExternalReflection.h"
#include "Rendering/Resources/Material.h"
#include "Reflection/TypeCreator.h"
#include "SceneSystem/Scene.h"
#include "Serialize/ObjectGraphDocument.h"
#include "Serialize/ObjectGraphPPtr.h"
#include "Serialize/ObjectGraphWriter.h"
#include "Serialize/ObjectReferenceResolver.h"
#include "Serialize/PrefabDocument.h"
#include "Vector2.h"
#include "Vector4.h"

namespace NLS::Engine::Serialize
{
    enum class UnknownTypePolicy
    {
        Preserve,
        Fail
    };

    enum class MissingAssetPolicy
    {
        Preserve,
        Fail
    };

    enum class InvalidReferencePolicy
    {
        Preserve,
        Fail
    };

    struct LoadPolicy
    {
        UnknownTypePolicy unknownTypePolicy = UnknownTypePolicy::Fail;
        MissingAssetPolicy missingAssetPolicy = MissingAssetPolicy::Preserve;
        InvalidReferencePolicy invalidReferencePolicy = InvalidReferencePolicy::Fail;
        bool deferAssetReferenceResolution = false;
        bool synchronousAssetReferencePrewarm = false;
        bool suppressGameObjectCreatedEvents = false;
        bool deferActivation = false;
        bool rebuildRuntimeCachesAfterLoad = true;
        bool skipDeferredAssetReferenceCacheLookup = false;
    };

    struct DocumentAnalysisResult
    {
        SerializationDiagnosticList diagnostics;
    };

    struct SceneInstantiationResult
    {
        std::unique_ptr<SceneSystem::Scene> scene;
        SerializationDiagnosticList diagnostics;
    };

    struct InstantiationProgress
    {
        float normalizedProgress = 0.0f;
        std::string message;
    };

    using InstantiationProgressCallback = std::function<void(const InstantiationProgress&)>;

    class ObjectGraphInstantiator
    {
    public:
        struct PrefabGameObjectStatePlan
        {
            std::optional<size_t> namePropertyIndex;
            std::optional<size_t> tagPropertyIndex;
            std::optional<size_t> activePropertyIndex;
            std::optional<size_t> layerPropertyIndex;
            std::optional<size_t> sourceObjectKeyPropertyIndex;
            std::optional<size_t> largeSceneHLODPropertyIndex;
            bool canApplyDirectState = true;
        };

        struct PrefabComponentInstantiatePlan
        {
            ObjectId sourceObject;
            size_t recordIndex = 0u;
            size_t componentIndex = 0u;
            bool hasExternalAssetReferenceBindingProperty = false;
        };

        struct PrefabGameObjectInstantiatePlan
        {
            ObjectId sourceObject;
            size_t recordIndex = 0u;
            std::optional<ObjectId> parentObject;
            PrefabGameObjectStatePlan state;
            std::vector<PrefabComponentInstantiatePlan> components;
        };

        struct PrefabInstantiatePlan
        {
            size_t objectRecordCount = 0u;
            size_t componentRecordCount = 0u;
            size_t assetReferenceBindingCandidateCount = 0u;
            std::unordered_map<ObjectId, size_t> objectRecordIndicesById;
            std::unordered_map<ObjectId, size_t> gameObjectPlanIndicesById;
            std::vector<PrefabGameObjectInstantiatePlan> gameObjects;
        };

        static PrefabInstantiatePlan BuildPrefabInstantiatePlan(const ObjectGraphDocument& graph)
        {
            PrefabInstantiatePlan plan;
            plan.objectRecordCount = graph.objects.size();
            plan.objectRecordIndicesById.reserve(graph.objects.size());
            plan.gameObjectPlanIndicesById.reserve(graph.objects.size());
            for (size_t index = 0u; index < graph.objects.size(); ++index)
                plan.objectRecordIndicesById.emplace(graph.objects[index].id, index);

            for (size_t index = 0u; index < graph.objects.size(); ++index)
            {
                const auto& object = graph.objects[index];
                if (IsInstantiableRecordState(object.state))
                {
                    const auto type = NLS::meta::Type::GetFromName(object.typeName);
                    if (type.IsValid() && type.DerivesFrom(NLS_TYPEOF(Components::Component)))
                        ++plan.componentRecordCount;
                }

                if (!IsInstantiableRecordState(object.state) || !RecordTypeMatches<GameObject>(object))
                    continue;

                PrefabGameObjectInstantiatePlan gameObjectPlan;
                gameObjectPlan.sourceObject = object.id;
                gameObjectPlan.recordIndex = index;
                gameObjectPlan.state = BuildGameObjectStatePlan(object);

                if (const auto* parentProperty = FindProperty(object, "parent");
                    parentProperty != nullptr &&
                    parentProperty->value.GetKind() == PropertyValue::Kind::ObjectReference)
                {
                    gameObjectPlan.parentObject = graph.ResolveObjectReference(
                        parentProperty->value.GetObjectReference());
                }

                const auto* components = FindProperty(object, "components");
                if (components != nullptr && components->value.GetKind() == PropertyValue::Kind::Array)
                {
                    const auto& componentReferences = components->value.GetArray();
                    gameObjectPlan.components.reserve(componentReferences.size());
                    for (size_t componentIndex = 0u; componentIndex < componentReferences.size(); ++componentIndex)
                    {
                        const auto componentId = ResolveObjectId(graph, componentReferences[componentIndex]);
                        if (!componentId.has_value())
                            continue;

                        const auto foundRecordIndex = plan.objectRecordIndicesById.find(*componentId);
                        if (foundRecordIndex == plan.objectRecordIndicesById.end())
                            continue;

                        const auto& componentRecord = graph.objects[foundRecordIndex->second];
                        const bool hasExternalBinding = HasExternalAssetReferenceBindingProperty(componentRecord);
                        if (hasExternalBinding)
                            ++plan.assetReferenceBindingCandidateCount;

                        gameObjectPlan.components.push_back({
                            *componentId,
                            foundRecordIndex->second,
                            componentIndex,
                            hasExternalBinding
                        });
                    }
                }

                plan.gameObjectPlanIndicesById.emplace(
                    gameObjectPlan.sourceObject,
                    plan.gameObjects.size());
                plan.gameObjects.push_back(std::move(gameObjectPlan));
            }
            return plan;
        }

        static DocumentAnalysisResult AnalyzeDocument(const ObjectGraphDocument& document, const LoadPolicy& policy)
        {
            DocumentAnalysisResult result;
            result.diagnostics = document.Validate();

            AnalyzeObjectTypes(document, policy, result.diagnostics);
            AnalyzeReflectedObjectReferenceShapes(document, policy, result.diagnostics);
            AnalyzeAssetReferences(document, policy, result.diagnostics);
            return result;
        }

        static std::unique_ptr<SceneSystem::Scene> InstantiateScene(const ObjectGraphDocument& document)
        {
            return InstantiateSceneStrict(document);
        }

        static SceneInstantiationResult InstantiateScene(const ObjectGraphDocument& document, const LoadPolicy& policy)
        {
            return InstantiateScene(document, policy, {});
        }

        static SceneInstantiationResult InstantiateScene(
            const ObjectGraphDocument& document,
            const LoadPolicy& policy,
            const InstantiationProgressCallback& progressCallback)
        {
            SceneInstantiationResult result;
            result.diagnostics = AnalyzeDocument(document, policy).diagnostics;
            if (result.diagnostics.HasErrors())
                return result;

            result.scene = InstantiateSceneStrict(document, progressCallback, policy);
            if (!result.scene)
            {
                result.diagnostics.Add({
                    SerializationDiagnosticCode::MissingObject,
                    SerializationDiagnosticSeverity::Error,
                    "Object graph scene could not be instantiated."
                });
            }
            return result;
        }

        static std::unique_ptr<SceneSystem::Scene> InstantiateSceneStrict(const ObjectGraphDocument& document)
        {
            return InstantiateSceneStrict(document, {});
        }

        static std::unique_ptr<SceneSystem::Scene> InstantiateSceneStrict(
            const ObjectGraphDocument& document,
            const InstantiationProgressCallback& progressCallback)
        {
            return InstantiateSceneStrict(document, progressCallback, {});
        }

        static std::unique_ptr<SceneSystem::Scene> InstantiateSceneStrict(
            const ObjectGraphDocument& document,
            const InstantiationProgressCallback& progressCallback,
            const LoadPolicy& policy)
        {
            if (document.Validate().HasErrors())
                return nullptr;

            const auto* sceneRecord = FindRecord(document, document.root);
            if (!sceneRecord || !RecordTypeMatches<NLS::Engine::SceneSystem::Scene>(*sceneRecord))
                return nullptr;

            auto scene = std::make_unique<SceneSystem::Scene>();
            InstanceContext context;
            context.document = &document;
            BuildObjectRecordIndex(context, document);

            const auto* gameObjects = FindProperty(*sceneRecord, "gameObjects");
            if (!gameObjects || gameObjects->value.GetKind() != PropertyValue::Kind::Array)
                return scene;

            const auto& gameObjectReferences = gameObjects->value.GetArray();
            const auto gameObjectCount = std::max<size_t>(gameObjectReferences.size(), 1u);
            size_t createdGameObjectCount = 0;
            for (const auto& reference : gameObjectReferences)
            {
                const auto objectId = ResolveObjectId(document, reference);
                if (!objectId.has_value())
                    continue;

                const auto* record = FindRecord(context, *objectId);
                if (!record ||
                    !IsInstantiableRecordState(record->state) ||
                    !RecordTypeMatches<GameObject>(*record))
                    continue;

                auto* gameObject = CreateGameObject(*record, policy);
                if (!gameObject)
                    continue;

                context.gameObjects.emplace(record->id, gameObject);
                scene->AddGameObject(gameObject);
                ++createdGameObjectCount;
                ReportProgress(
                    progressCallback,
                    0.25f + 0.20f * (static_cast<float>(createdGameObjectCount) / static_cast<float>(gameObjectCount)),
                    "Creating GameObject: " + gameObject->GetName());
            }

            const auto objectCount = std::max<size_t>(document.objects.size(), 1u);
            size_t appliedObjectCount = 0;
            for (const auto& object : document.objects)
            {
                ++appliedObjectCount;
                if (!IsInstantiableRecordState(object.state))
                    continue;

                if (!RecordTypeMatches<GameObject>(object))
                    continue;

                auto* gameObject = FindGameObject(context, object.id);
                if (!gameObject)
                    continue;

                ReportProgress(
                    progressCallback,
                    0.45f + 0.30f * (static_cast<float>(appliedObjectCount) / static_cast<float>(objectCount)),
                    "Restoring components: " + gameObject->GetName());
                ApplyGameObjectState(*gameObject, object);
                InstantiateComponents(document, object, *gameObject, context, policy);
            }

            size_t resolvedObjectCount = 0;
            for (const auto& object : document.objects)
            {
                ++resolvedObjectCount;
                if (!IsInstantiableRecordState(object.state))
                    continue;

                if (RecordTypeMatches<GameObject>(object))
                {
                    ReportProgress(
                        progressCallback,
                        0.75f + 0.15f * (static_cast<float>(resolvedObjectCount) / static_cast<float>(objectCount)),
                        "Restoring hierarchy");
                    ResolveParent(context, object);
                }
            }

            ReportProgress(progressCallback, 0.92f, "Rebuilding scene runtime caches");
            scene->RebuildRuntimeCachesAfterLoad();
            return scene;
        }

        static PrefabInstantiationResult InstantiatePrefab(const PrefabDocument& prefab, SceneSystem::Scene& scene)
        {
            return InstantiatePrefab(prefab, scene, {});
        }

        static PrefabInstantiationResult InstantiatePrefab(
            const PrefabDocument& prefab,
            SceneSystem::Scene& scene,
            const LoadPolicy& policy)
        {
            if (!prefab.graph.overrides.empty())
            {
                auto graph = prefab.graph;
                ApplyOverrides(graph);
                return InstantiatePrefabGraph(graph, scene, policy);
            }

            return InstantiatePrefabGraph(prefab.graph, scene, policy);
        }

        static PrefabInstantiationResult InstantiatePrefabGraph(
            const ObjectGraphDocument& graph,
            SceneSystem::Scene& scene,
            const LoadPolicy& policy)
        {
            return InstantiatePrefabGraph(graph, scene, policy, nullptr);
        }

        static PrefabInstantiationResult InstantiatePrefabGraph(
            const ObjectGraphDocument& graph,
            SceneSystem::Scene& scene,
            const LoadPolicy& policy,
            const PrefabInstantiatePlan* instantiatePlan)
        {
            namespace Profiling = NLS::Base::Profiling;

            PrefabInstantiationResult result;
            const auto* compiledPlan = GetCompatiblePrefabInstantiatePlan(graph, instantiatePlan);
            {
                Profiling::PerformanceStageScope resolveScope(
                    Profiling::PerformanceStageDomain::Prefab,
                    "ResolveDependencies",
                    Profiling::PerformanceStageThread::Main);
                AnalyzeObjectTypes(graph, policy, result.diagnostics);
                AnalyzeReflectedObjectReferenceShapes(graph, policy, result.diagnostics);
                AnalyzeAssetReferences(graph, policy, result.diagnostics);
                resolveScope.AddCounter("objectCount", graph.objects.size());
                if (compiledPlan != nullptr)
                {
                    resolveScope.AddCounter("compiledInstantiatePlanUsedCount", 1u);
                    resolveScope.AddCounter("compiledInstantiatePlanGameObjectCount", compiledPlan->gameObjects.size());
                    resolveScope.AddCounter("compiledInstantiatePlanComponentCount", compiledPlan->componentRecordCount);
                }
            }
            if (result.diagnostics.HasErrors())
                return result;

            InstanceContext context;
            context.document = &graph;
            context.instantiatePlan = compiledPlan;
            if (compiledPlan == nullptr)
                BuildObjectRecordIndex(context, graph);
            const auto instantiableObjectCount = compiledPlan != nullptr
                ? compiledPlan->gameObjects.size()
                : CountInstantiableGameObjectRecords(graph);
            const auto componentRecordCount = compiledPlan != nullptr
                ? compiledPlan->componentRecordCount
                : CountComponentRecords(graph);
            const auto instanceSeed = NLS::Guid::New().ToString();
            auto makeInstanceObjectId = [&instanceSeed](const ObjectId& sourceObject)
            {
                return ObjectId(NLS::Guid::NewDeterministic(
                    "Prefab.Instance:" + instanceSeed + ":" + sourceObject.GetGuid().ToString()));
            };

            {
                Profiling::PerformanceStageScope allocateScope(
                    Profiling::PerformanceStageDomain::Prefab,
                    "AllocateInstanceObjects",
                    Profiling::PerformanceStageThread::Main);
                context.gameObjects.reserve(instantiableObjectCount);
                result.sourceToInstance.reserve(instantiableObjectCount + componentRecordCount);
                result.sourceByInstanceObject.reserve(instantiableObjectCount);
                if (compiledPlan != nullptr)
                {
                    for (const auto& gameObjectPlan : compiledPlan->gameObjects)
                    {
                        const auto* object = GetPlannedRecord(graph, gameObjectPlan.recordIndex);
                        if (object == nullptr)
                            continue;

                        auto* gameObject = CreateGameObject(*object, policy, &gameObjectPlan.state);
                        if (!gameObject)
                            continue;

                        result.sourceToInstance.emplace(object->id, makeInstanceObjectId(object->id));
                        result.sourceByInstanceObject.emplace(gameObject, object->id);
                        context.gameObjects.emplace(object->id, gameObject);
                    }
                }
                else
                {
                    for (const auto& object : graph.objects)
                    {
                        if (!IsInstantiableRecordState(object.state))
                            continue;

                        if (!RecordTypeMatches<GameObject>(object))
                            continue;

                        auto* gameObject = CreateGameObject(object, policy);
                        if (!gameObject)
                            continue;

                        result.sourceToInstance.emplace(object.id, makeInstanceObjectId(object.id));
                        result.sourceByInstanceObject.emplace(gameObject, object.id);
                        context.gameObjects.emplace(object.id, gameObject);
                    }
                }
                allocateScope.AddCounter("objectCount", context.gameObjects.size());
                allocateScope.AddCounter("reservedObjectCount", instantiableObjectCount);
            }

            {
                Profiling::PerformanceStageScope restoreGameObjectsScope(
                    Profiling::PerformanceStageDomain::Prefab,
                    "RestoreGameObjectState",
                Profiling::PerformanceStageThread::Main);
                size_t restoredGameObjectCount = 0u;
                size_t directGameObjectPropertyCount = 0u;
                size_t compiledGameObjectStatePlanUsedCount = 0u;
                size_t gameObjectStatePropertyLookupCount = 0u;
                if (compiledPlan != nullptr)
                {
                    for (const auto& gameObjectPlan : compiledPlan->gameObjects)
                    {
                        const auto* object = GetPlannedRecord(graph, gameObjectPlan.recordIndex);
                        if (object == nullptr)
                            continue;

                        auto* gameObject = FindGameObject(context, object->id);
                        if (!gameObject)
                            continue;

                        const auto stateResult = ApplyGameObjectState(*gameObject, *object, &gameObjectPlan.state);
                        directGameObjectPropertyCount += stateResult.directPropertyCount;
                        gameObjectStatePropertyLookupCount += stateResult.propertyLookupCount;
                        if (stateResult.usedCompiledStatePlan)
                            ++compiledGameObjectStatePlanUsedCount;
                        ++restoredGameObjectCount;
                    }
                }
                else
                {
                    for (const auto& object : graph.objects)
                    {
                        if (!IsInstantiableRecordState(object.state))
                            continue;

                        if (!RecordTypeMatches<GameObject>(object))
                            continue;

                        auto* gameObject = FindGameObject(context, object.id);
                        if (!gameObject)
                            continue;

                        const auto stateResult = ApplyGameObjectState(*gameObject, object, nullptr);
                        directGameObjectPropertyCount += stateResult.directPropertyCount;
                        gameObjectStatePropertyLookupCount += stateResult.propertyLookupCount;
                        ++restoredGameObjectCount;
                    }
                }
                restoreGameObjectsScope.AddCounter("restoredGameObjectCount", restoredGameObjectCount);
                restoreGameObjectsScope.AddCounter("directGameObjectPropertyCount", directGameObjectPropertyCount);
                restoreGameObjectsScope.AddCounter(
                    "compiledGameObjectStatePlanUsedCount",
                    compiledGameObjectStatePlanUsedCount);
                restoreGameObjectsScope.AddCounter(
                    "gameObjectStatePropertyLookupCount",
                    gameObjectStatePropertyLookupCount);
            }

            {
                Profiling::PerformanceStageScope createComponentsScope(
                    Profiling::PerformanceStageDomain::Prefab,
                    "CreateComponents",
                    Profiling::PerformanceStageThread::Main);
                context.components.reserve(componentRecordCount);
                context.componentBindingSpansByGameObject.reserve(instantiableObjectCount);
                context.componentBindings.reserve(componentRecordCount);
                if (compiledPlan != nullptr)
                    context.assetReferenceBindingIndices.reserve(compiledPlan->assetReferenceBindingCandidateCount);
                const auto indexedLookupStart = context.indexedRecordLookupCount;
                const auto linearLookupStart = context.linearRecordLookupCount;
                size_t createdComponentCount = 0u;
                if (compiledPlan != nullptr)
                {
                    for (const auto& gameObjectPlan : compiledPlan->gameObjects)
                    {
                        auto* gameObject = FindGameObject(context, gameObjectPlan.sourceObject);
                        if (!gameObject)
                            continue;

                        const auto bindingStart = context.componentBindings.size();
                        createdComponentCount += CreateComponents(gameObjectPlan, *gameObject, context);
                        context.componentBindingSpansByGameObject.emplace(
                            gameObjectPlan.sourceObject,
                            std::make_pair(bindingStart, context.componentBindings.size()));
                    }
                }
                else
                {
                    for (const auto& object : graph.objects)
                    {
                        if (!IsInstantiableRecordState(object.state))
                            continue;

                        if (!RecordTypeMatches<GameObject>(object))
                            continue;

                        auto* gameObject = FindGameObject(context, object.id);
                        if (!gameObject)
                            continue;

                        const auto bindingStart = context.componentBindings.size();
                        createdComponentCount += CreateComponents(graph, object, *gameObject, context);
                        context.componentBindingSpansByGameObject.emplace(
                            object.id,
                            std::make_pair(bindingStart, context.componentBindings.size()));
                    }
                }
                createComponentsScope.AddCounter("createdComponentCount", createdComponentCount);
                createComponentsScope.AddCounter("componentRecordCount", componentRecordCount);
                createComponentsScope.AddCounter(
                    "indexedRecordLookupCount",
                    context.indexedRecordLookupCount - indexedLookupStart);
                createComponentsScope.AddCounter(
                    "linearRecordLookupCount",
                    context.linearRecordLookupCount - linearLookupStart);
            }

            {
                Profiling::PerformanceStageScope deserializeScope(
                    Profiling::PerformanceStageDomain::Prefab,
                    "DeserializeComponents",
                    Profiling::PerformanceStageThread::Main);
                const auto indexedLookupStart = context.indexedRecordLookupCount;
                const auto linearLookupStart = context.linearRecordLookupCount;
                size_t restoredGameObjectCount = 0u;
                size_t componentCount = 0u;
                size_t directComponentBindingPopulateCount = 0u;
                if (compiledPlan != nullptr)
                {
                    for (const auto& gameObjectPlan : compiledPlan->gameObjects)
                    {
                        const auto span = context.componentBindingSpansByGameObject.find(gameObjectPlan.sourceObject);
                        if (span != context.componentBindingSpansByGameObject.end())
                        {
                            const auto restored = PopulateComponents(
                                context,
                                policy,
                                span->second.first,
                                span->second.second);
                            componentCount += restored;
                            directComponentBindingPopulateCount += restored;
                        }
                        else
                        {
                            componentCount += PopulateComponents(gameObjectPlan, context, policy);
                        }
                        RegisterComponentMappings(gameObjectPlan, result, context, makeInstanceObjectId);
                        ++restoredGameObjectCount;
                    }
                }
                else
                {
                    for (const auto& object : graph.objects)
                    {
                        if (!IsInstantiableRecordState(object.state))
                            continue;

                        if (!RecordTypeMatches<GameObject>(object))
                            continue;
                        const auto span = context.componentBindingSpansByGameObject.find(object.id);
                        if (span != context.componentBindingSpansByGameObject.end())
                        {
                            const auto restored = PopulateComponents(
                                context,
                                policy,
                                span->second.first,
                                span->second.second);
                            componentCount += restored;
                            directComponentBindingPopulateCount += restored;
                        }
                        else
                        {
                            componentCount += PopulateComponents(object, context, policy);
                        }
                        RegisterComponentMappings(object, result, context, makeInstanceObjectId);
                        ++restoredGameObjectCount;
                    }
                }
                deserializeScope.AddCounter("componentCount", componentCount);
                deserializeScope.AddCounter("restoredGameObjectCount", restoredGameObjectCount);
                deserializeScope.AddCounter("restoredComponentCount", componentCount);
                deserializeScope.AddCounter(
                    "directComponentBindingPopulateCount",
                    directComponentBindingPopulateCount);
                deserializeScope.AddCounter(
                    "indexedRecordLookupCount",
                    context.indexedRecordLookupCount - indexedLookupStart);
                deserializeScope.AddCounter(
                    "linearRecordLookupCount",
                    context.linearRecordLookupCount - linearLookupStart);
            }

            {
                Profiling::PerformanceStageScope bindScope(
                    Profiling::PerformanceStageDomain::Prefab,
                    "BindExternalAssetReferences",
                    Profiling::PerformanceStageThread::Main);
                const auto bindingCounts = BindExternalAssetReferences(context, policy);
                bindScope.AddCounter("componentCount", context.componentBindings.size());
                bindScope.AddCounter("candidateComponentCount", bindingCounts.candidateComponentCount);
                bindScope.AddCounter("scannedComponentCount", bindingCounts.scannedComponentCount);
                bindScope.AddCounter("assetReferenceBindingCount", bindingCounts.propertyCount);
                bindScope.AddCounter("assetReferenceElementBindingCount", bindingCounts.elementCount);
                bindScope.AddCounter("meshReferenceCacheHitCount", context.meshReferenceCacheHitCount);
                bindScope.AddCounter("meshReferenceCacheMissCount", context.meshReferenceCacheMissCount);
                bindScope.AddCounter("materialReferenceCacheHitCount", context.materialReferenceCacheHitCount);
                bindScope.AddCounter("materialReferenceCacheMissCount", context.materialReferenceCacheMissCount);
                bindScope.AddCounter("meshResourceLookupCount", context.meshResourceLookupCount);
                bindScope.AddCounter("materialResourceLookupCount", context.materialResourceLookupCount);
            }

            {
                Profiling::PerformanceStageScope fixupScope(
                    Profiling::PerformanceStageDomain::Prefab,
                    "FixupInternalReferences",
                    Profiling::PerformanceStageThread::Main);
                size_t parentFixupCount = 0u;
                if (compiledPlan != nullptr)
                {
                    for (const auto& gameObjectPlan : compiledPlan->gameObjects)
                    {
                        if (ResolveParent(context, gameObjectPlan))
                            ++parentFixupCount;
                    }
                }
                else
                {
                    for (const auto& object : graph.objects)
                    {
                        if (!IsInstantiableRecordState(object.state))
                            continue;

                        if (RecordTypeMatches<GameObject>(object))
                        {
                            if (ResolveParent(context, object))
                                ++parentFixupCount;
                        }
                    }
                }
                fixupScope.AddCounter("parentFixupCount", parentFixupCount);
            }

            result.root = FindGameObject(context, graph.root);
            if (result.root)
            {
                Profiling::PerformanceStageScope registerScope(
                    Profiling::PerformanceStageDomain::Prefab,
                    "RegisterRenderers",
                    Profiling::PerformanceStageThread::Main);
                size_t rendererCount = 0u;
                for (const auto& [objectId, gameObject] : context.gameObjects)
                {
                    (void)objectId;
                    if (gameObject != nullptr && gameObject->GetComponent<Components::MeshRenderer>() != nullptr)
                        ++rendererCount;
                }
                registerScope.AddCounter("rendererCount", rendererCount);
                registerScope.AddCounter("objectCount", context.gameObjects.size());
                registerScope.AddCounter("componentCount", context.componentBindings.size());
                scene.AddGameObject(
                    result.root,
                    policy.deferActivation
                        ? SceneSystem::Scene::AddGameObjectActivation::Deferred
                        : SceneSystem::Scene::AddGameObjectActivation::Immediate);
            }

            {
                Profiling::PerformanceStageScope registerPhysicsScope(
                    Profiling::PerformanceStageDomain::Prefab,
                    "RegisterPhysics",
                    Profiling::PerformanceStageThread::Main);
            }

            {
                Profiling::PerformanceStageScope registerScriptsScope(
                    Profiling::PerformanceStageDomain::Prefab,
                    "RegisterScripts",
                    Profiling::PerformanceStageThread::Main);
            }

            if (policy.rebuildRuntimeCachesAfterLoad)
                scene.RebuildRuntimeCachesAfterLoad();
            {
                Profiling::PerformanceStageScope lifecycleScope(
                    Profiling::PerformanceStageDomain::Prefab,
                    "InvokeLifecycle",
                    Profiling::PerformanceStageThread::Main);
                size_t activatedObjectCount = 0u;
                if (policy.deferActivation && scene.IsPlaying() && result.root != nullptr)
                    activatedObjectCount = scene.ActivateGameObjectForPlay(result.root);
                lifecycleScope.AddCounter("activatedObjectCount", activatedObjectCount);
            }
            return result;
        }

        static SerializationDiagnosticList ValidatePrefab(const PrefabDocument& prefab)
        {
            return ValidatePrefabGraph(prefab.graph);
        }

        static SerializationDiagnosticList ValidatePrefabGraph(const ObjectGraphDocument& graph)
        {
            SerializationDiagnosticList diagnostics = graph.Validate();
            for (const auto& object : graph.objects)
            {
                if (object.state != ObjectRecordState::Stripped)
                    continue;

                diagnostics.Add({
                    SerializationDiagnosticCode::InvalidPrefabOverride,
                    SerializationDiagnosticSeverity::Error,
                    "Prefab source graphs cannot contain scene-only stripped object records."
                });
            }

            for (const auto& operation : graph.overrides)
            {
                if (!FindRecord(graph, operation.target))
                {
                    diagnostics.Add({
                        SerializationDiagnosticCode::InvalidPrefabOverride,
                        SerializationDiagnosticSeverity::Error,
                        "Prefab override targets a missing object."
                    });
                    continue;
                }

                if ((operation.type == PatchOperationType::InsertOwned ||
                     operation.type == PatchOperationType::RemoveOwned ||
                     operation.type == PatchOperationType::MoveOwned) &&
                    !FindRecord(graph, operation.object))
                {
                    diagnostics.Add({
                        SerializationDiagnosticCode::InvalidPrefabOverride,
                        SerializationDiagnosticSeverity::Error,
                        "Prefab override references a missing owned object."
                    });
                }
            }
            return diagnostics;
        }

    private:
        struct InstanceContext
        {
            const ObjectGraphDocument* document = nullptr;
            const PrefabInstantiatePlan* instantiatePlan = nullptr;
            std::unordered_map<ObjectId, const ObjectRecord*> objectRecordsById;
            std::unordered_map<ObjectId, GameObject*> gameObjects;
            std::unordered_map<ObjectId, Components::Component*> components;
            std::unordered_map<ObjectId, std::pair<size_t, size_t>> componentBindingSpansByGameObject;
            std::vector<std::pair<Components::Component*, const ObjectRecord*>> componentBindings;
            std::vector<size_t> assetReferenceBindingIndices;
            std::unordered_map<std::string, Core::ResourceManagement::MeshManager::Mesh*> meshResourcesByNormalizedPath;
            std::unordered_map<std::string, Core::ResourceManagement::MaterialManager::Material*> materialResourcesByNormalizedPath;
            std::unordered_map<std::string, Core::ResourceManagement::MeshManager::Mesh*> meshResourcesByResolvedReferencePath;
            std::unordered_map<std::string, Core::ResourceManagement::MaterialManager::Material*> materialResourcesByResolvedReferencePath;
            size_t indexedRecordLookupCount = 0u;
            size_t linearRecordLookupCount = 0u;
            size_t meshReferenceCacheHitCount = 0u;
            size_t meshReferenceCacheMissCount = 0u;
            size_t materialReferenceCacheHitCount = 0u;
            size_t materialReferenceCacheMissCount = 0u;
            size_t meshResourceLookupCount = 0u;
            size_t materialResourceLookupCount = 0u;
            bool meshResourcePathIndexBuilt = false;
            bool materialResourcePathIndexBuilt = false;
        };

        struct GameObjectStateApplyResult
        {
            size_t directPropertyCount = 0u;
            size_t propertyLookupCount = 0u;
            bool usedCompiledStatePlan = false;
        };

        static PrefabGameObjectStatePlan BuildGameObjectStatePlan(const ObjectRecord& record)
        {
            PrefabGameObjectStatePlan plan;
            for (size_t propertyIndex = 0u; propertyIndex < record.properties.size(); ++propertyIndex)
            {
                const auto& property = record.properties[propertyIndex];
                if (property.name == "name")
                {
                    if (property.value.GetKind() == PropertyValue::Kind::String)
                    {
                        if (!plan.namePropertyIndex.has_value())
                            plan.namePropertyIndex = propertyIndex;
                    }
                    else
                    {
                        plan.canApplyDirectState = false;
                    }
                    continue;
                }

                if (property.name == "tag")
                {
                    if (property.value.GetKind() == PropertyValue::Kind::String)
                    {
                        if (!plan.tagPropertyIndex.has_value())
                            plan.tagPropertyIndex = propertyIndex;
                    }
                    else
                    {
                        plan.canApplyDirectState = false;
                    }
                    continue;
                }

                if (property.name == "active")
                {
                    if (property.value.GetKind() == PropertyValue::Kind::Bool)
                    {
                        if (!plan.activePropertyIndex.has_value())
                            plan.activePropertyIndex = propertyIndex;
                    }
                    else
                    {
                        plan.canApplyDirectState = false;
                    }
                    continue;
                }

                if (property.name == "layer")
                {
                    if (property.value.GetKind() == PropertyValue::Kind::Integer)
                    {
                        const auto value = property.value.GetInteger();
                        if (value >= std::numeric_limits<int>::min() &&
                            value <= std::numeric_limits<int>::max())
                        {
                            if (!plan.layerPropertyIndex.has_value())
                                plan.layerPropertyIndex = propertyIndex;
                            continue;
                        }
                    }

                    plan.canApplyDirectState = false;
                    continue;
                }

                if (property.name == "sourceObjectKey")
                {
                    if (property.value.GetKind() == PropertyValue::Kind::String &&
                        !plan.sourceObjectKeyPropertyIndex.has_value())
                    {
                        plan.sourceObjectKeyPropertyIndex = propertyIndex;
                    }
                    continue;
                }

                if (property.name == "largeSceneHLOD")
                {
                    if (!plan.largeSceneHLODPropertyIndex.has_value())
                        plan.largeSceneHLODPropertyIndex = propertyIndex;
                    continue;
                }
            }
            return plan;
        }

        static bool GameObjectStatePlansMatch(
            const PrefabGameObjectStatePlan& current,
            const PrefabGameObjectStatePlan& planned)
        {
            return current.namePropertyIndex == planned.namePropertyIndex &&
                current.tagPropertyIndex == planned.tagPropertyIndex &&
                current.activePropertyIndex == planned.activePropertyIndex &&
                current.layerPropertyIndex == planned.layerPropertyIndex &&
                current.sourceObjectKeyPropertyIndex == planned.sourceObjectKeyPropertyIndex &&
                current.largeSceneHLODPropertyIndex == planned.largeSceneHLODPropertyIndex &&
                current.canApplyDirectState == planned.canApplyDirectState;
        }

        static const PrefabInstantiatePlan* GetCompatiblePrefabInstantiatePlan(
            const ObjectGraphDocument& graph,
            const PrefabInstantiatePlan* plan)
        {
            if (plan == nullptr || plan->objectRecordCount != graph.objects.size())
                return nullptr;

            size_t currentGameObjectCount = 0u;
            for (size_t index = 0u; index < graph.objects.size(); ++index)
            {
                const auto& object = graph.objects[index];
                if (!IsInstantiableRecordState(object.state) || !RecordTypeMatches<GameObject>(object))
                    continue;

                ++currentGameObjectCount;
                const auto foundPlan = plan->gameObjectPlanIndicesById.find(object.id);
                if (foundPlan == plan->gameObjectPlanIndicesById.end())
                    return nullptr;

                if (foundPlan->second >= plan->gameObjects.size() ||
                    plan->gameObjects[foundPlan->second].recordIndex != index)
                {
                    return nullptr;
                }
            }

            if (currentGameObjectCount != plan->gameObjects.size())
                return nullptr;

            for (const auto& gameObjectPlan : plan->gameObjects)
            {
                const auto* record = GetPlannedRecord(graph, gameObjectPlan.recordIndex);
                if (record == nullptr || record->id != gameObjectPlan.sourceObject)
                    return nullptr;

                if (!IsInstantiableRecordState(record->state) || !RecordTypeMatches<GameObject>(*record))
                    return nullptr;

                if (!GameObjectStatePlansMatch(BuildGameObjectStatePlan(*record), gameObjectPlan.state))
                    return nullptr;

                const auto currentParent = ResolvePlannedParentObject(graph, *record);
                if (currentParent != gameObjectPlan.parentObject)
                    return nullptr;

                if (!PlannedComponentsMatchGraph(graph, gameObjectPlan))
                    return nullptr;
            }
            return plan;
        }

        static const ObjectRecord* GetPlannedRecord(
            const ObjectGraphDocument& graph,
            const size_t recordIndex)
        {
            return recordIndex < graph.objects.size() ? &graph.objects[recordIndex] : nullptr;
        }

        static std::optional<ObjectId> ResolvePlannedParentObject(
            const ObjectGraphDocument& graph,
            const ObjectRecord& record)
        {
            const auto* parentProperty = FindProperty(record, "parent");
            if (parentProperty == nullptr ||
                parentProperty->value.GetKind() != PropertyValue::Kind::ObjectReference)
            {
                return std::nullopt;
            }

            return graph.ResolveObjectReference(parentProperty->value.GetObjectReference());
        }

        static bool PlannedComponentsMatchGraph(
            const ObjectGraphDocument& graph,
            const PrefabGameObjectInstantiatePlan& gameObjectPlan)
        {
            const auto* record = GetPlannedRecord(graph, gameObjectPlan.recordIndex);
            if (record == nullptr)
                return false;

            const auto* components = FindProperty(*record, "components");
            if (components == nullptr || components->value.GetKind() != PropertyValue::Kind::Array)
                return gameObjectPlan.components.empty();

            size_t plannedComponentIndex = 0u;
            const auto& componentReferences = components->value.GetArray();
            for (size_t componentIndex = 0u; componentIndex < componentReferences.size(); ++componentIndex)
            {
                const auto componentId = ResolveObjectId(graph, componentReferences[componentIndex]);
                if (!componentId.has_value())
                    continue;

                if (plannedComponentIndex >= gameObjectPlan.components.size())
                    return false;

                const auto& componentPlan = gameObjectPlan.components[plannedComponentIndex];
                if (componentPlan.sourceObject != *componentId ||
                    componentPlan.componentIndex != componentIndex)
                {
                    return false;
                }

                const auto* componentRecord = GetPlannedRecord(graph, componentPlan.recordIndex);
                if (componentRecord == nullptr || componentRecord->id != componentPlan.sourceObject)
                    return false;

                if (componentPlan.hasExternalAssetReferenceBindingProperty !=
                    HasExternalAssetReferenceBindingProperty(*componentRecord))
                {
                    return false;
                }

                ++plannedComponentIndex;
            }

            return plannedComponentIndex == gameObjectPlan.components.size();
        }

        static void BuildObjectRecordIndex(InstanceContext& context, const ObjectGraphDocument& document)
        {
            context.objectRecordsById.clear();
            context.objectRecordsById.reserve(document.objects.size());
            for (const auto& object : document.objects)
                context.objectRecordsById.emplace(object.id, &object);
        }

        static size_t CountInstantiableGameObjectRecords(const ObjectGraphDocument& document)
        {
            size_t count = 0u;
            for (const auto& object : document.objects)
            {
                if (IsInstantiableRecordState(object.state) && RecordTypeMatches<GameObject>(object))
                    ++count;
            }
            return count;
        }

        static size_t CountComponentRecords(const ObjectGraphDocument& document)
        {
            size_t count = 0u;
            for (const auto& object : document.objects)
            {
                if (!IsInstantiableRecordState(object.state))
                    continue;

                const auto type = NLS::meta::Type::GetFromName(object.typeName);
                if (type.IsValid() && type.DerivesFrom(NLS_TYPEOF(Components::Component)))
                    ++count;
            }
            return count;
        }

        static const ObjectRecord* FindRecord(const ObjectGraphDocument& document, const ObjectId& id)
        {
            for (const auto& object : document.objects)
            {
                if (object.id == id)
                    return &object;
            }
            return nullptr;
        }

        static const ObjectRecord* FindRecord(InstanceContext& context, const ObjectId& id)
        {
            if (context.instantiatePlan != nullptr && context.document != nullptr)
            {
                const auto foundIndex = context.instantiatePlan->objectRecordIndicesById.find(id);
                if (foundIndex != context.instantiatePlan->objectRecordIndicesById.end())
                {
                    ++context.indexedRecordLookupCount;
                    return GetPlannedRecord(*context.document, foundIndex->second);
                }
            }

            const auto found = context.objectRecordsById.find(id);
            if (found != context.objectRecordsById.end())
            {
                ++context.indexedRecordLookupCount;
                return found->second;
            }

            ++context.linearRecordLookupCount;
            return context.document != nullptr ? FindRecord(*context.document, id) : nullptr;
        }

        static const ObjectRecord* FindRecord(
            const ObjectGraphDocument& document,
            const ObjectIdentifier& reference)
        {
            const auto id = document.ResolveObjectReference(reference);
            return id.has_value() ? FindRecord(document, *id) : nullptr;
        }

        static std::optional<ObjectId> ResolveObjectId(
            const ObjectGraphDocument& document,
            const PropertyValue& value)
        {
            if (value.GetKind() == PropertyValue::Kind::OwnedReference)
                return value.GetObjectId();
            if (value.GetKind() == PropertyValue::Kind::ObjectReference)
                return document.ResolveObjectReference(value.GetObjectReference());
            return std::nullopt;
        }

        static const PropertyRecord* FindProperty(const ObjectRecord& record, const char* name)
        {
            for (const auto& property : record.properties)
            {
                if (property.name == name)
                    return &property;
            }
            return nullptr;
        }

        static PropertyRecord* FindMutableProperty(ObjectRecord& record, const char* name)
        {
            for (auto& property : record.properties)
            {
                if (property.name == name)
                    return &property;
            }
            return nullptr;
        }

        static ObjectRecord* FindMutableRecord(ObjectGraphDocument& document, const ObjectId& id)
        {
            for (auto& object : document.objects)
            {
                if (object.id == id)
                    return &object;
            }
            return nullptr;
        }

        static void ReportProgress(
            const InstantiationProgressCallback& callback,
            const float normalizedProgress,
            const std::string& message)
        {
            if (callback)
                callback({std::clamp(normalizedProgress, 0.0f, 1.0f), message});
        }

        static std::optional<std::string> ReadString(const ObjectRecord& record, const char* name)
        {
            const auto* property = FindProperty(record, name);
            if (!property || property->value.GetKind() != PropertyValue::Kind::String)
                return std::nullopt;
            return property->value.GetString();
        }

        static std::optional<bool> ReadBool(const ObjectRecord& record, const char* name)
        {
            const auto* property = FindProperty(record, name);
            if (!property || property->value.GetKind() != PropertyValue::Kind::Bool)
                return std::nullopt;
            return property->value.GetBool();
        }

        static std::optional<int> ReadInt(const ObjectRecord& record, const char* name)
        {
            const auto* property = FindProperty(record, name);
            if (!property || property->value.GetKind() != PropertyValue::Kind::Integer)
                return std::nullopt;
            const auto value = property->value.GetInteger();
            if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
                return std::nullopt;
            return static_cast<int>(value);
        }

        static std::optional<std::string> ReadString(
            const ObjectRecord& record,
            const char* name,
            size_t* propertyLookupCount)
        {
            if (propertyLookupCount != nullptr)
                ++(*propertyLookupCount);
            return ReadString(record, name);
        }

        static std::optional<bool> ReadBool(
            const ObjectRecord& record,
            const char* name,
            size_t* propertyLookupCount)
        {
            if (propertyLookupCount != nullptr)
                ++(*propertyLookupCount);
            return ReadBool(record, name);
        }

        static std::optional<int> ReadInt(
            const ObjectRecord& record,
            const char* name,
            size_t* propertyLookupCount)
        {
            if (propertyLookupCount != nullptr)
                ++(*propertyLookupCount);
            return ReadInt(record, name);
        }

        static const PropertyRecord* GetPlannedProperty(
            const ObjectRecord& record,
            const std::optional<size_t> propertyIndex,
            const char* expectedName)
        {
            if (!propertyIndex.has_value() || *propertyIndex >= record.properties.size())
                return nullptr;

            const auto& property = record.properties[*propertyIndex];
            return property.name == expectedName ? &property : nullptr;
        }

        static std::optional<std::string> ReadPlannedString(
            const ObjectRecord& record,
            const std::optional<size_t> propertyIndex,
            const char* expectedName)
        {
            const auto* property = GetPlannedProperty(record, propertyIndex, expectedName);
            if (property == nullptr || property->value.GetKind() != PropertyValue::Kind::String)
                return std::nullopt;
            return property->value.GetString();
        }

        static std::optional<bool> ReadPlannedBool(
            const ObjectRecord& record,
            const std::optional<size_t> propertyIndex,
            const char* expectedName)
        {
            const auto* property = GetPlannedProperty(record, propertyIndex, expectedName);
            if (property == nullptr || property->value.GetKind() != PropertyValue::Kind::Bool)
                return std::nullopt;
            return property->value.GetBool();
        }

        static std::optional<int> ReadPlannedInt(
            const ObjectRecord& record,
            const std::optional<size_t> propertyIndex,
            const char* expectedName)
        {
            const auto* property = GetPlannedProperty(record, propertyIndex, expectedName);
            if (property == nullptr || property->value.GetKind() != PropertyValue::Kind::Integer)
                return std::nullopt;

            const auto value = property->value.GetInteger();
            if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
                return std::nullopt;
            return static_cast<int>(value);
        }

        static bool IsGameObjectGraphProperty(const std::string& name)
        {
            return name == "parent" ||
                name == "components" ||
                name == "children" ||
                name == "sourceObjectKey" ||
                name == "largeSceneHLOD";
        }

        static bool IsGameObjectDirectOrGraphProperty(const std::string& name)
        {
            return IsGameObjectGraphProperty(name) ||
                name == "name" ||
                name == "tag" ||
                name == "active" ||
                name == "layer";
        }

        static bool IsNoGraphProperty(const std::string&)
        {
            return false;
        }

        static bool IsBatchedTransformProperty(const std::string& name)
        {
            return name == "localPosition" ||
                name == "localRotation" ||
                name == "localScale";
        }

        template<typename ComponentType>
        static bool RecordTypeMatchesComponent(const ObjectRecord& record)
        {
            return RecordTypeMatches<ComponentType>(record);
        }

        template<typename ObjectType>
        static bool RecordTypeMatches(const ObjectRecord& record)
        {
            static const std::string typeName = NLS_TYPEOF(ObjectType).GetName();
            return record.typeName == typeName;
        }

        static bool IsDeferredAssetProperty(const ObjectRecord& record, const std::string& name)
        {
            if (RecordTypeMatchesComponent<Components::MeshFilter>(record))
                return name == "mesh";
            if (RecordTypeMatchesComponent<Components::MeshRenderer>(record))
                return name == "materials";
            return false;
        }

        static bool IsExternalAssetReferenceBindingProperty(const ObjectRecord& record, const PropertyRecord& property)
        {
            return IsDeferredAssetProperty(record, property.name);
        }

        static bool HasExternalAssetReferenceBindingProperty(const ObjectRecord& record)
        {
            for (const auto& property : record.properties)
            {
                if (IsExternalAssetReferenceBindingProperty(record, property))
                    return true;
            }
            return false;
        }

        static GameObject* CreateGameObject(const ObjectRecord& record, const LoadPolicy& policy)
        {
            return CreateGameObject(record, policy, nullptr);
        }

        static GameObject* CreateGameObject(
            const ObjectRecord& record,
            const LoadPolicy& policy,
            const PrefabGameObjectStatePlan* statePlan)
        {
            const auto name = statePlan != nullptr
                ? ReadPlannedString(record, statePlan->namePropertyIndex, "name").value_or(record.debugName)
                : ReadString(record, "name").value_or(record.debugName);
            const auto tag = statePlan != nullptr
                ? ReadPlannedString(record, statePlan->tagPropertyIndex, "tag").value_or(std::string {})
                : ReadString(record, "tag").value_or(std::string {});
            if (policy.suppressGameObjectCreatedEvents)
                return new GameObject(GameObject::SilentCreationTag {}, name, tag);
            return new GameObject(name, tag);
        }

        static GameObject* FindGameObject(const InstanceContext& context, const ObjectId& id)
        {
            const auto found = context.gameObjects.find(id);
            return found != context.gameObjects.end() ? found->second : nullptr;
        }

        static size_t ApplyGameObjectState(GameObject& gameObject, const ObjectRecord& record)
        {
            return ApplyGameObjectState(gameObject, record, nullptr).directPropertyCount;
        }

        static GameObjectStateApplyResult ApplyGameObjectState(
            GameObject& gameObject,
            const ObjectRecord& record,
            const PrefabGameObjectStatePlan* statePlan)
        {
            GameObjectStateApplyResult result;
            if (statePlan != nullptr)
            {
                result.usedCompiledStatePlan = true;
                if (statePlan->canApplyDirectState)
                {
                    ApplyPlannedDirectGameObjectState(gameObject, record, *statePlan, result.directPropertyCount);
                    ApplyReflectedFields(gameObject, record, IsGameObjectDirectOrGraphProperty);
                }
                else
                {
                    ApplyReflectedFields(gameObject, record, IsGameObjectGraphProperty);
                }

                ApplyPlannedGameObjectRuntimeMetadata(gameObject, record, *statePlan, result.directPropertyCount);
                return result;
            }

            if (CanApplyDirectGameObjectState(record, &result.propertyLookupCount))
            {
                ApplyDirectGameObjectState(
                    gameObject,
                    record,
                    result.directPropertyCount,
                    &result.propertyLookupCount);
                ApplyReflectedFields(gameObject, record, IsGameObjectDirectOrGraphProperty);
            }
            else
            {
                ApplyReflectedFields(gameObject, record, IsGameObjectGraphProperty);
            }
            if (const auto sourceObjectKey = ReadString(
                record,
                "sourceObjectKey",
                &result.propertyLookupCount); sourceObjectKey.has_value())
            {
                gameObject.SetSourceObjectKey(*sourceObjectKey);
                ++result.directPropertyCount;
            }
            else
            {
                gameObject.SetSourceObjectKey({});
            }

            ++result.propertyLookupCount;
            if (const auto* hlodMetadata = FindProperty(record, "largeSceneHLOD"))
            {
                gameObject.SetLargeSceneHLODMetadata(ObjectGraphWriter::WriteValueForRuntimeMetadata(hlodMetadata->value).dump());
                ++result.directPropertyCount;
            }
            else
            {
                gameObject.SetLargeSceneHLODMetadata({});
            }
            return result;
        }

        static bool CanApplyDirectGameObjectState(
            const ObjectRecord& record,
            size_t* propertyLookupCount = nullptr)
        {
            for (const auto& property : record.properties)
            {
                if ((property.name == "name" || property.name == "tag") &&
                    property.value.GetKind() != PropertyValue::Kind::String)
                {
                    return false;
                }

                if (property.name == "active" && property.value.GetKind() != PropertyValue::Kind::Bool)
                    return false;

                if (property.name == "layer")
                {
                    const auto value = ReadInt(record, "layer", propertyLookupCount);
                    if (!value.has_value())
                        return false;
                }
            }
            return true;
        }

        static void ApplyDirectGameObjectState(
            GameObject& gameObject,
            const ObjectRecord& record,
            size_t& directPropertyCount,
            size_t* propertyLookupCount = nullptr)
        {
            if (const auto name = ReadString(record, "name", propertyLookupCount); name.has_value())
            {
                gameObject.SetName(*name);
                ++directPropertyCount;
            }

            if (const auto tag = ReadString(record, "tag", propertyLookupCount); tag.has_value())
            {
                gameObject.SetTag(*tag);
                ++directPropertyCount;
            }

            if (const auto active = ReadBool(record, "active", propertyLookupCount); active.has_value())
            {
                gameObject.SetActive(*active);
                ++directPropertyCount;
            }

            if (const auto layer = ReadInt(record, "layer", propertyLookupCount); layer.has_value())
            {
                gameObject.SetLayer(*layer);
                ++directPropertyCount;
            }
        }

        static void ApplyPlannedDirectGameObjectState(
            GameObject& gameObject,
            const ObjectRecord& record,
            const PrefabGameObjectStatePlan& statePlan,
            size_t& directPropertyCount)
        {
            if (const auto name = ReadPlannedString(record, statePlan.namePropertyIndex, "name"); name.has_value())
            {
                gameObject.SetName(*name);
                ++directPropertyCount;
            }

            if (const auto tag = ReadPlannedString(record, statePlan.tagPropertyIndex, "tag"); tag.has_value())
            {
                gameObject.SetTag(*tag);
                ++directPropertyCount;
            }

            if (const auto active = ReadPlannedBool(record, statePlan.activePropertyIndex, "active"); active.has_value())
            {
                gameObject.SetActive(*active);
                ++directPropertyCount;
            }

            if (const auto layer = ReadPlannedInt(record, statePlan.layerPropertyIndex, "layer"); layer.has_value())
            {
                gameObject.SetLayer(*layer);
                ++directPropertyCount;
            }
        }

        static void ApplyPlannedGameObjectRuntimeMetadata(
            GameObject& gameObject,
            const ObjectRecord& record,
            const PrefabGameObjectStatePlan& statePlan,
            size_t& directPropertyCount)
        {
            if (const auto sourceObjectKey = ReadPlannedString(
                record,
                statePlan.sourceObjectKeyPropertyIndex,
                "sourceObjectKey"); sourceObjectKey.has_value())
            {
                gameObject.SetSourceObjectKey(*sourceObjectKey);
                ++directPropertyCount;
            }
            else
            {
                gameObject.SetSourceObjectKey({});
            }

            if (const auto* hlodMetadata = GetPlannedProperty(
                record,
                statePlan.largeSceneHLODPropertyIndex,
                "largeSceneHLOD"))
            {
                gameObject.SetLargeSceneHLODMetadata(
                    ObjectGraphWriter::WriteValueForRuntimeMetadata(hlodMetadata->value).dump());
                ++directPropertyCount;
            }
            else
            {
                gameObject.SetLargeSceneHLODMetadata({});
            }
        }

        static void InstantiateComponents(
            const ObjectGraphDocument& document,
            const ObjectRecord& gameObjectRecord,
            GameObject& gameObject,
            InstanceContext& context,
            const LoadPolicy& policy = {})
        {
            const auto bindingStart = context.componentBindings.size();
            CreateComponents(document, gameObjectRecord, gameObject, context);
            PopulateComponents(gameObjectRecord, context, policy);
            BindExternalAssetReferences(context, policy, bindingStart, context.componentBindings.size());
        }

        static size_t CreateComponents(
            const ObjectGraphDocument& document,
            const ObjectRecord& gameObjectRecord,
            GameObject& gameObject,
            InstanceContext& context)
        {
            const auto* components = FindProperty(gameObjectRecord, "components");
            if (!components || components->value.GetKind() != PropertyValue::Kind::Array)
                return 0u;

            size_t createdComponentCount = 0u;
            for (size_t index = 0; index < components->value.GetArray().size(); ++index)
            {
                const auto& reference = components->value.GetArray()[index];
                const auto componentId = ResolveObjectId(document, reference);
                if (!componentId.has_value())
                    continue;

                const auto* componentRecord = FindRecord(context, *componentId);
                if (!componentRecord)
                    continue;

                auto* component = EnsureComponent(gameObject, *componentRecord, index);
                if (!component)
                    continue;

                context.components.emplace(componentRecord->id, component);
                if (HasExternalAssetReferenceBindingProperty(*componentRecord))
                    context.assetReferenceBindingIndices.push_back(context.componentBindings.size());
                context.componentBindings.emplace_back(component, componentRecord);
                gameObject.MoveComponent(component, index);
                ++createdComponentCount;
            }
            return createdComponentCount;
        }

        static size_t CreateComponents(
            const PrefabGameObjectInstantiatePlan& gameObjectPlan,
            GameObject& gameObject,
            InstanceContext& context)
        {
            if (context.document == nullptr)
                return 0u;

            size_t createdComponentCount = 0u;
            for (const auto& componentPlan : gameObjectPlan.components)
            {
                const auto* componentRecord = GetPlannedRecord(*context.document, componentPlan.recordIndex);
                if (componentRecord == nullptr)
                    continue;

                auto* component = EnsureComponent(gameObject, *componentRecord, componentPlan.componentIndex);
                if (!component)
                    continue;

                context.components.emplace(componentPlan.sourceObject, component);
                if (componentPlan.hasExternalAssetReferenceBindingProperty)
                    context.assetReferenceBindingIndices.push_back(context.componentBindings.size());
                context.componentBindings.emplace_back(component, componentRecord);
                gameObject.MoveComponent(component, componentPlan.componentIndex);
                ++createdComponentCount;
            }
            return createdComponentCount;
        }

        static size_t PopulateComponents(
            const ObjectRecord& gameObjectRecord,
            InstanceContext& context,
            const LoadPolicy& policy = {})
        {
            const auto* components = FindProperty(gameObjectRecord, "components");
            if (!components || components->value.GetKind() != PropertyValue::Kind::Array)
                return 0u;

            size_t restoredComponentCount = 0u;
            for (const auto& reference : components->value.GetArray())
            {
                if (context.document == nullptr)
                    continue;

                const auto componentId = ResolveObjectId(*context.document, reference);
                if (!componentId.has_value())
                    continue;

                const auto component = context.components.find(*componentId);
                if (component == context.components.end() || component->second == nullptr)
                    continue;

                const auto* componentRecord = FindRecord(context, *componentId);
                if (!componentRecord)
                    continue;

                ApplyComponentState(*component->second, *componentRecord, policy);
                ++restoredComponentCount;
            }
            return restoredComponentCount;
        }

        static size_t PopulateComponents(
            InstanceContext& context,
            const LoadPolicy& policy,
            const size_t firstBindingIndex,
            const size_t lastBindingIndex)
        {
            size_t restoredComponentCount = 0u;
            const auto end = std::min(lastBindingIndex, context.componentBindings.size());
            for (size_t bindingIndex = firstBindingIndex; bindingIndex < end; ++bindingIndex)
            {
                const auto& [component, componentRecord] = context.componentBindings[bindingIndex];
                if (component == nullptr || componentRecord == nullptr)
                    continue;

                ApplyComponentState(*component, *componentRecord, policy);
                ++restoredComponentCount;
            }
            return restoredComponentCount;
        }

        static size_t PopulateComponents(
            const PrefabGameObjectInstantiatePlan& gameObjectPlan,
            InstanceContext& context,
            const LoadPolicy& policy = {})
        {
            if (context.document == nullptr)
                return 0u;

            size_t restoredComponentCount = 0u;
            for (const auto& componentPlan : gameObjectPlan.components)
            {
                const auto component = context.components.find(componentPlan.sourceObject);
                if (component == context.components.end() || component->second == nullptr)
                    continue;

                const auto* componentRecord = GetPlannedRecord(*context.document, componentPlan.recordIndex);
                if (!componentRecord)
                    continue;

                ApplyComponentState(*component->second, *componentRecord, policy);
                ++restoredComponentCount;
            }
            return restoredComponentCount;
        }

        static Components::Component* EnsureComponent(GameObject& gameObject, const ObjectRecord& record, size_t index)
        {
            if (RecordTypeMatchesComponent<Components::TransformComponent>(record))
            {
                auto* transform = gameObject.GetTransform();
                if (transform)
                    transform->CreateBy(&gameObject);
                return transform;
            }

            const auto type = NLS::meta::Type::GetFromName(record.typeName);
            if (!type.IsValid() || !type.DerivesFrom(NLS_TYPEOF(Components::Component)))
                return nullptr;

            if (index < gameObject.GetComponents().size())
            {
                auto* existing = gameObject.GetComponents()[index].get();
                if (existing && existing->GetType() == type)
                    return existing;
            }

            return gameObject.AddComponent(type);
        }

        static void ApplyComponentState(
            Components::Component& component,
            const ObjectRecord& record,
            const LoadPolicy& policy = {})
        {
            if (RecordTypeMatchesComponent<Components::TransformComponent>(record) &&
                TryApplyBatchedTransformState(component, record))
            {
                ApplyReflectedFields(component, record, IsBatchedTransformProperty, &policy, nullptr);
                return;
            }

            ApplyReflectedFields(component, record, IsNoGraphProperty, &policy, nullptr);
        }

        static bool TryApplyBatchedTransformState(
            Components::Component& component,
            const ObjectRecord& record)
        {
            const auto* positionProperty = FindProperty(record, "localPosition");
            const auto* rotationProperty = FindProperty(record, "localRotation");
            const auto* scaleProperty = FindProperty(record, "localScale");
            if (positionProperty == nullptr || rotationProperty == nullptr || scaleProperty == nullptr)
                return false;

            const auto position = TryReadVector3(positionProperty->value);
            const auto rotation = TryReadQuaternion(rotationProperty->value);
            const auto scale = TryReadVector3(scaleProperty->value);
            if (!position.has_value() || !rotation.has_value() || !scale.has_value())
                return false;

            static_cast<Components::TransformComponent&>(component).SetLocalTransform(
                *position,
                *rotation,
                *scale);
            return true;
        }

        struct ExternalAssetBindingCounts
        {
            size_t propertyCount = 0u;
            size_t elementCount = 0u;
            size_t candidateComponentCount = 0u;
            size_t scannedComponentCount = 0u;
        };

        static ExternalAssetBindingCounts BindExternalAssetReferences(InstanceContext& context, const LoadPolicy& policy)
        {
            return BindExternalAssetReferences(context, policy, 0u, context.componentBindings.size());
        }

        static ExternalAssetBindingCounts BindExternalAssetReferences(
            InstanceContext& context,
            const LoadPolicy& policy,
            size_t firstBindingIndex,
            size_t lastBindingIndex)
        {
            ExternalAssetBindingCounts bindingCounts;
            lastBindingIndex = std::min(lastBindingIndex, context.componentBindings.size());
            for (const auto bindingIndex : context.assetReferenceBindingIndices)
            {
                if (bindingIndex < firstBindingIndex || bindingIndex >= lastBindingIndex)
                    continue;

                ++bindingCounts.candidateComponentCount;
                const auto& [component, record] = context.componentBindings[bindingIndex];
                if (component == nullptr || record == nullptr)
                    continue;

                ++bindingCounts.scannedComponentCount;
                if (policy.deferAssetReferenceResolution)
                    ApplyDeferredAssetReferenceHints(*component, *record, context, policy);

                for (const auto& property : record->properties)
                {
                    if (!IsExternalAssetReferenceBindingProperty(*record, property))
                        continue;

                    ++bindingCounts.propertyCount;
                    bindingCounts.elementCount += CountExternalAssetReferenceElements(*record, property);
                    if (!policy.deferAssetReferenceResolution)
                        ResolveRuntimeAssetReference(*component, *record, property, context);
                }
            }
            return bindingCounts;
        }

        static size_t CountExternalAssetReferenceElements(
            const ObjectRecord& record,
            const PropertyRecord& property)
        {
            if (RecordTypeMatchesComponent<Components::MeshRenderer>(record) &&
                property.name == "materials" &&
                property.value.GetKind() == PropertyValue::Kind::Array)
            {
                const auto& values = property.value.GetArray();
                const auto materialCount = std::min(
                    values.size(),
                    static_cast<size_t>(Components::MeshRenderer::kMaxMaterialCount));
                size_t count = 0u;
                for (size_t index = 0u; index < materialCount; ++index)
                    count += CountExternalAssetReferenceElements(values[index]);
                return count;
            }

            return CountExternalAssetReferenceElements(property.value);
        }

        static size_t CountExternalAssetReferenceElements(const PropertyValue& value)
        {
            switch (value.GetKind())
            {
            case PropertyValue::Kind::ObjectReference:
                return value.GetObjectReference().guid.IsValid() ? 1u : 0u;
            case PropertyValue::Kind::Array:
            {
                size_t count = 0u;
                for (const auto& item : value.GetArray())
                    count += CountExternalAssetReferenceElements(item);
                return count;
            }
            case PropertyValue::Kind::Object:
            {
                size_t count = 0u;
                for (const auto& property : value.GetObject())
                    count += CountExternalAssetReferenceElements(property.second);
                return count;
            }
            default:
                return 0u;
            }
        }

        static void ApplyReflectedFields(
            NLS::Object& object,
            const ObjectRecord& record,
            bool (*isGraphProperty)(const std::string&),
            const LoadPolicy* policy = nullptr,
            InstanceContext* context = nullptr)
        {
            auto instance = NLS::meta::Variant(&object, NLS::meta::variant_policy::WrapObject {});
            const auto type = object.GetType();
            if (!type.IsValid())
                return;

            ReflectionApplyTelemetry telemetry;
            const auto collectTelemetry = NLS::Base::Profiling::PerformanceStageScope::GetActiveStats() != nullptr;

            for (const auto& property : record.properties)
            {
                if (isGraphProperty && isGraphProperty(property.name))
                    continue;

                const auto fieldLookupStart = collectTelemetry ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
                const auto field = type.GetField(property.name);
                if (collectTelemetry)
                    telemetry.fieldLookupDuration += std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - fieldLookupStart);
                ++telemetry.fieldLookupCount;
                if (!field.IsValid() || field.IsReadOnly())
                    continue;

                if (policy &&
                    policy->deferAssetReferenceResolution &&
                    (ContainsAssetReference(property.value) || IsDeferredAssetProperty(record, property.name)))
                {
                    continue;
                }

                const auto objectReferenceResult = TrySetUnityObjectReferenceField(instance, field, property.value);
                if (objectReferenceResult == UnityObjectReferenceApplyResult::Applied)
                {
                    if (context != nullptr)
                        ResolveRuntimeAssetReference(object, record, property, *context);
                    continue;
                }

                if (objectReferenceResult == UnityObjectReferenceApplyResult::ShapeMismatch)
                {
                    if (policy != nullptr &&
                        policy->invalidReferencePolicy == InvalidReferencePolicy::Preserve)
                    {
                        continue;
                    }

                    throw std::invalid_argument(
                        "Object graph property \"" + record.typeName + "." + property.name +
                        "\" must use a Unity-style object reference shape.");
                }

                const auto directStart = collectTelemetry ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
                const auto directApplied = TryApplyDirectPropertyValue(instance, field, property.value);
                if (collectTelemetry)
                    telemetry.directApplyDuration += std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - directStart);
                if (directApplied)
                {
                    ++telemetry.directApplyCount;
                    continue;
                }

                const auto convertStart = collectTelemetry ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
                const auto json = ConvertPropertyValue(property.value);
                if (collectTelemetry)
                    telemetry.convertDuration += std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - convertStart);
                ++telemetry.convertCount;

                const auto deserializeStart = collectTelemetry ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
                auto value = field.GetType().DeserializeJson(NLS::Json(json));
                if (collectTelemetry)
                    telemetry.deserializeDuration += std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - deserializeStart);
                ++telemetry.deserializeCount;

                const auto setStart = collectTelemetry ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
                field.SetValue(instance, value);
                if (collectTelemetry)
                    telemetry.setDuration += std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - setStart);
                ++telemetry.setCount;
            }

            if (collectTelemetry)
                RecordReflectionApplyTelemetry(telemetry);
        }

        struct ReflectionApplyTelemetry
        {
            std::chrono::microseconds fieldLookupDuration{0};
            std::chrono::microseconds directApplyDuration{0};
            std::chrono::microseconds convertDuration{0};
            std::chrono::microseconds deserializeDuration{0};
            std::chrono::microseconds setDuration{0};
            uint64_t fieldLookupCount = 0u;
            uint64_t directApplyCount = 0u;
            uint64_t convertCount = 0u;
            uint64_t deserializeCount = 0u;
            uint64_t setCount = 0u;
        };

        static void RecordReflectionApplyTelemetry(const ReflectionApplyTelemetry& telemetry)
        {
            auto* stats = NLS::Base::Profiling::PerformanceStageScope::GetActiveStats();
            if (stats == nullptr)
                return;

            RecordReflectionSubstage(*stats, "ReflectFieldLookup", telemetry.fieldLookupDuration, telemetry.fieldLookupCount);
            RecordReflectionSubstage(*stats, "ApplyDirectPropertyValue", telemetry.directApplyDuration, telemetry.directApplyCount);
            RecordReflectionSubstage(*stats, "ConvertPropertyValue", telemetry.convertDuration, telemetry.convertCount);
            RecordReflectionSubstage(*stats, "DeserializePropertyValue", telemetry.deserializeDuration, telemetry.deserializeCount);
            RecordReflectionSubstage(*stats, "SetReflectedField", telemetry.setDuration, telemetry.setCount);
        }

        static bool TryApplyDirectPropertyValue(
            NLS::meta::Variant& instance,
            const NLS::meta::Field& field,
            const PropertyValue& value)
        {
            const auto fieldType = field.GetType();
            if (fieldType == NLS_TYPEOF(bool) && value.GetKind() == PropertyValue::Kind::Bool)
            {
                const auto directValue = value.GetBool();
                return field.SetValue(instance, NLS::meta::Variant(directValue));
            }

            if (fieldType == NLS_TYPEOF(int) && value.GetKind() == PropertyValue::Kind::Integer)
            {
                const auto directValue = static_cast<int>(value.GetInteger());
                return field.SetValue(instance, NLS::meta::Variant(directValue));
            }

            if (fieldType == NLS_TYPEOF(float) &&
                (value.GetKind() == PropertyValue::Kind::Number || value.GetKind() == PropertyValue::Kind::Integer))
            {
                const auto directValue = value.GetKind() == PropertyValue::Kind::Number
                    ? static_cast<float>(value.GetNumber())
                    : static_cast<float>(value.GetInteger());
                return field.SetValue(instance, NLS::meta::Variant(directValue));
            }

            if (fieldType == NLS_TYPEOF(double) &&
                (value.GetKind() == PropertyValue::Kind::Number || value.GetKind() == PropertyValue::Kind::Integer))
            {
                const auto directValue = value.GetKind() == PropertyValue::Kind::Number
                    ? value.GetNumber()
                    : static_cast<double>(value.GetInteger());
                return field.SetValue(instance, NLS::meta::Variant(directValue));
            }

            if (fieldType == NLS_TYPEOF(std::string) && value.GetKind() == PropertyValue::Kind::String)
            {
                const auto directValue = value.GetString();
                return field.SetValue(instance, NLS::meta::Variant(directValue));
            }

            if (fieldType == NLS_TYPEOF(Maths::Vector2))
            {
                if (auto directValue = TryReadVector2(value); directValue.has_value())
                    return field.SetValue(instance, NLS::meta::Variant(*directValue));
                return false;
            }

            if (fieldType == NLS_TYPEOF(Maths::Vector3))
            {
                if (auto directValue = TryReadVector3(value); directValue.has_value())
                    return field.SetValue(instance, NLS::meta::Variant(*directValue));
                return false;
            }

            if (fieldType == NLS_TYPEOF(Maths::Vector4))
            {
                if (auto directValue = TryReadVector4(value); directValue.has_value())
                    return field.SetValue(instance, NLS::meta::Variant(*directValue));
                return false;
            }

            if (fieldType == NLS_TYPEOF(Maths::Quaternion))
            {
                if (auto directValue = TryReadQuaternion(value); directValue.has_value())
                    return field.SetValue(instance, NLS::meta::Variant(*directValue));
                return false;
            }

            if (fieldType == NLS_TYPEOF(Maths::Color))
            {
                if (auto directValue = TryReadColor(value); directValue.has_value())
                    return field.SetValue(instance, NLS::meta::Variant(*directValue));
                return false;
            }

            if (fieldType == NLS_TYPEOF(Maths::Rect))
            {
                if (auto directValue = TryReadRect(value); directValue.has_value())
                    return field.SetValue(instance, NLS::meta::Variant(*directValue));
                return false;
            }

            return false;
        }

        static std::optional<float> TryReadFloatProperty(
            const PropertyValue::ObjectValue& object,
            const std::string_view name)
        {
            const auto found = std::find_if(
                object.begin(),
                object.end(),
                [name](const auto& property)
                {
                    return property.first == name;
                });
            if (found == object.end())
                return std::nullopt;

            const auto& value = found->second;
            if (value.GetKind() == PropertyValue::Kind::Number)
                return static_cast<float>(value.GetNumber());
            if (value.GetKind() == PropertyValue::Kind::Integer)
                return static_cast<float>(value.GetInteger());
            return std::nullopt;
        }

        static std::optional<Maths::Vector2> TryReadVector2(const PropertyValue& value)
        {
            if (value.GetKind() != PropertyValue::Kind::Object)
                return std::nullopt;

            const auto& object = value.GetObject();
            const auto x = TryReadFloatProperty(object, "x");
            const auto y = TryReadFloatProperty(object, "y");
            if (!x.has_value() || !y.has_value())
                return std::nullopt;
            return Maths::Vector2(*x, *y);
        }

        static std::optional<Maths::Vector3> TryReadVector3(const PropertyValue& value)
        {
            if (value.GetKind() != PropertyValue::Kind::Object)
                return std::nullopt;

            const auto& object = value.GetObject();
            const auto x = TryReadFloatProperty(object, "x");
            const auto y = TryReadFloatProperty(object, "y");
            const auto z = TryReadFloatProperty(object, "z");
            if (!x.has_value() || !y.has_value() || !z.has_value())
                return std::nullopt;
            return Maths::Vector3(*x, *y, *z);
        }

        static std::optional<Maths::Vector4> TryReadVector4(const PropertyValue& value)
        {
            if (value.GetKind() != PropertyValue::Kind::Object)
                return std::nullopt;

            const auto& object = value.GetObject();
            const auto x = TryReadFloatProperty(object, "x");
            const auto y = TryReadFloatProperty(object, "y");
            const auto z = TryReadFloatProperty(object, "z");
            const auto w = TryReadFloatProperty(object, "w");
            if (!x.has_value() || !y.has_value() || !z.has_value() || !w.has_value())
                return std::nullopt;
            return Maths::Vector4(*x, *y, *z, *w);
        }

        static std::optional<Maths::Quaternion> TryReadQuaternion(const PropertyValue& value)
        {
            if (value.GetKind() != PropertyValue::Kind::Object)
                return std::nullopt;

            const auto& object = value.GetObject();
            const auto x = TryReadFloatProperty(object, "x");
            const auto y = TryReadFloatProperty(object, "y");
            const auto z = TryReadFloatProperty(object, "z");
            const auto w = TryReadFloatProperty(object, "w");
            if (!x.has_value() || !y.has_value() || !z.has_value() || !w.has_value())
                return std::nullopt;
            return Maths::Quaternion(*x, *y, *z, *w);
        }

        static std::optional<Maths::Color> TryReadColor(const PropertyValue& value)
        {
            if (value.GetKind() != PropertyValue::Kind::Object)
                return std::nullopt;

            const auto& object = value.GetObject();
            const auto r = TryReadFloatProperty(object, "r");
            const auto g = TryReadFloatProperty(object, "g");
            const auto b = TryReadFloatProperty(object, "b");
            const auto a = TryReadFloatProperty(object, "a");
            if (!r.has_value() || !g.has_value() || !b.has_value() || !a.has_value())
                return std::nullopt;
            return Maths::Color(*r, *g, *b, *a);
        }

        static std::optional<Maths::Rect> TryReadRect(const PropertyValue& value)
        {
            if (value.GetKind() != PropertyValue::Kind::Object)
                return std::nullopt;

            const auto& object = value.GetObject();
            const auto x = TryReadFloatProperty(object, "x");
            const auto y = TryReadFloatProperty(object, "y");
            const auto width = TryReadFloatProperty(object, "width");
            const auto height = TryReadFloatProperty(object, "height");
            if (!x.has_value() || !y.has_value() || !width.has_value() || !height.has_value())
                return std::nullopt;
            return Maths::Rect(*x, *y, *width, *height);
        }

        static void RecordReflectionSubstage(
            NLS::Base::Profiling::PerformanceStageStats& stats,
            const char* stageName,
            std::chrono::microseconds duration,
            uint64_t propertyCount)
        {
            if (propertyCount == 0u)
                return;

            NLS::Base::Profiling::PerformanceStageSample sample;
            sample.domain = NLS::Base::Profiling::PerformanceStageDomain::Prefab;
            sample.stageName = stageName;
            sample.thread = NLS::Base::Profiling::PerformanceStageThread::Main;
            sample.duration = duration;
            sample.counters.emplace("propertyCount", propertyCount);
            stats.Record(sample);
        }

        enum class UnityObjectReferenceApplyResult
        {
            NotObjectReferenceField,
            Applied,
            ShapeMismatch
        };

        static UnityObjectReferenceApplyResult TrySetUnityObjectReferenceField(
            NLS::meta::Variant& instance,
            const NLS::meta::Field& field,
            const PropertyValue& value)
        {
            const auto fieldType = field.GetType();
            if (fieldType.IsArray() && Internal::IsPPtrTypeName(fieldType.GetArrayType()))
            {
                switch (Internal::ApplyPPtrArrayOrThrow(instance, field, value))
                {
                case Internal::PPtrApplyResult::Applied:
                    return UnityObjectReferenceApplyResult::Applied;
                case Internal::PPtrApplyResult::ShapeMismatch:
                    return UnityObjectReferenceApplyResult::ShapeMismatch;
                }
            }

            if (Internal::IsPPtrTypeName(fieldType))
            {
                switch (Internal::ApplyPPtrValueOrThrow(instance, field, value))
                {
                case Internal::PPtrApplyResult::Applied:
                    return UnityObjectReferenceApplyResult::Applied;
                case Internal::PPtrApplyResult::ShapeMismatch:
                    return UnityObjectReferenceApplyResult::ShapeMismatch;
                }
            }

            return UnityObjectReferenceApplyResult::NotObjectReferenceField;
        }

        static void ResolveRuntimeAssetReference(
            NLS::Object& object,
            const ObjectRecord& record,
            const PropertyRecord& property,
            InstanceContext& context)
        {
            if (RecordTypeMatchesComponent<Components::MeshFilter>(record) && property.name == "mesh")
            {
                if (object.GetType() != NLS_TYPEOF(Components::MeshFilter) ||
                    property.value.GetKind() != PropertyValue::Kind::ObjectReference ||
                    !property.value.GetObjectReference().guid.IsValid())
                {
                    return;
                }
                auto* meshFilter = static_cast<Components::MeshFilter*>(&object);

                const auto path = ResolveAssetReferencePath(property.value.GetObjectReference());
                if (path.empty() ||
                    !Core::ServiceLocator::Contains<Core::ResourceManagement::MeshManager>())
                {
                    return;
                }

                auto& meshManager = NLS_SERVICE(Core::ResourceManagement::MeshManager);
                if (auto* mesh = FindCachedMeshResource(meshManager, path, context))
                    meshFilter->SetResolvedMeshFromReference(mesh);
                return;
            }

            if (RecordTypeMatchesComponent<Components::MeshRenderer>(record) && property.name == "materials")
            {
                if (object.GetType() != NLS_TYPEOF(Components::MeshRenderer) ||
                    property.value.GetKind() != PropertyValue::Kind::Array ||
                    !Core::ServiceLocator::Contains<Core::ResourceManagement::MaterialManager>())
                {
                    return;
                }
                auto* meshRenderer = static_cast<Components::MeshRenderer*>(&object);

                auto& materialManager = NLS_SERVICE(Core::ResourceManagement::MaterialManager);
                const auto& values = property.value.GetArray();
                const auto materialCount = std::min(
                    values.size(),
                    static_cast<size_t>(Components::MeshRenderer::kMaxMaterialCount));
                for (size_t index = 0; index < materialCount; ++index)
                {
                    const auto& value = values[index];
                    if (value.GetKind() != PropertyValue::Kind::ObjectReference ||
                        !value.GetObjectReference().guid.IsValid())
                    {
                        continue;
                    }

                    const auto path = ResolveAssetReferencePath(value.GetObjectReference());
                    if (path.empty())
                        continue;

                    if (auto* material = FindCachedMaterialResource(materialManager, path, context))
                        meshRenderer->SetResolvedMaterialFromReference(static_cast<uint32_t>(index), *material);
                }
            }
        }

        static bool ResolveParent(const InstanceContext& context, const ObjectRecord& record)
        {
            auto* child = FindGameObject(context, record.id);
            if (!child)
                return false;

            const auto* parentProperty = FindProperty(record, "parent");
            if (!parentProperty || parentProperty->value.GetKind() != PropertyValue::Kind::ObjectReference)
                return false;

            const auto parentId = context.document != nullptr
                ? context.document->ResolveObjectReference(parentProperty->value.GetObjectReference())
                : std::optional<ObjectId> {};
            auto* parent = parentId.has_value() ? FindGameObject(context, *parentId) : nullptr;
            if (parent)
            {
                child->SetParent(*parent);
                return true;
            }
            return false;
        }

        static bool ResolveParent(
            const InstanceContext& context,
            const PrefabGameObjectInstantiatePlan& gameObjectPlan)
        {
            if (!gameObjectPlan.parentObject.has_value())
                return false;

            auto* child = FindGameObject(context, gameObjectPlan.sourceObject);
            if (!child)
                return false;

            auto* parent = FindGameObject(context, *gameObjectPlan.parentObject);
            if (!parent)
                return false;

            child->SetParent(*parent);
            return true;
        }

        template<typename MakeInstanceObjectId>
        static void RegisterComponentMappings(
            const ObjectRecord& gameObjectRecord,
            PrefabInstantiationResult& result,
            InstanceContext& context,
            const MakeInstanceObjectId& makeInstanceObjectId)
        {
            const auto* components = FindProperty(gameObjectRecord, "components");
            if (!components || components->value.GetKind() != PropertyValue::Kind::Array)
                return;

            for (const auto& reference : components->value.GetArray())
            {
                if (context.document == nullptr)
                    continue;

                const auto componentId = ResolveObjectId(*context.document, reference);
                if (!componentId.has_value())
                    continue;

                if (!context.components.contains(*componentId))
                    continue;

                result.sourceToInstance.emplace(*componentId, makeInstanceObjectId(*componentId));
            }
        }

        template<typename MakeInstanceObjectId>
        static void RegisterComponentMappings(
            const PrefabGameObjectInstantiatePlan& gameObjectPlan,
            PrefabInstantiationResult& result,
            InstanceContext& context,
            const MakeInstanceObjectId& makeInstanceObjectId)
        {
            for (const auto& componentPlan : gameObjectPlan.components)
            {
                if (!context.components.contains(componentPlan.sourceObject))
                    continue;

                result.sourceToInstance.emplace(
                    componentPlan.sourceObject,
                    makeInstanceObjectId(componentPlan.sourceObject));
            }
        }

        static void ApplyOverrides(ObjectGraphDocument& graph)
        {
            for (const auto& operation : graph.overrides)
            {
                switch (operation.type)
                {
                case PatchOperationType::ReplaceProperty:
                    ApplyReplaceProperty(graph, operation);
                    break;
                case PatchOperationType::InsertOwned:
                    ApplyInsertOwned(graph, operation);
                    break;
                case PatchOperationType::RemoveOwned:
                    ApplyRemoveOwned(graph, operation);
                    break;
                case PatchOperationType::MoveOwned:
                    ApplyMoveOwned(graph, operation);
                    break;
                case PatchOperationType::RemoveObject:
                    ApplyRemoveObject(graph, operation);
                    break;
                default:
                    break;
                }
            }
        }

        static void ApplyReplaceProperty(ObjectGraphDocument& graph, const PatchOperation& operation)
        {
            auto* record = FindMutableRecord(graph, operation.target);
            if (!record)
                return;

            if (auto* property = FindMutableProperty(*record, operation.property.c_str()))
                property->value = operation.value;
            else
                record->properties.push_back({operation.property, operation.value});
        }

        static void ApplyInsertOwned(ObjectGraphDocument& graph, const PatchOperation& operation)
        {
            auto* record = FindMutableRecord(graph, operation.target);
            if (!record)
                return;

            auto* property = FindMutableProperty(*record, operation.property.c_str());
            if (!property || property->value.GetKind() != PropertyValue::Kind::Array)
                return;

            auto values = property->value.GetArray();
            const auto insertIndex = operation.hasIndex && operation.index < values.size()
                ? operation.index
                : values.size();
            values.insert(values.begin() + static_cast<std::ptrdiff_t>(insertIndex), PropertyValue::OwnedReference(operation.object));
            property->value = PropertyValue::Array(std::move(values));
        }

        static void ApplyRemoveOwned(ObjectGraphDocument& graph, const PatchOperation& operation)
        {
            auto* record = FindMutableRecord(graph, operation.target);
            if (!record)
                return;

            auto* property = FindMutableProperty(*record, operation.property.c_str());
            if (!property || property->value.GetKind() != PropertyValue::Kind::Array)
                return;

            auto values = property->value.GetArray();
            values.erase(std::remove_if(values.begin(), values.end(), [&operation](const PropertyValue& value)
            {
                return value.GetKind() == PropertyValue::Kind::OwnedReference && value.GetObjectId() == operation.object;
            }), values.end());
            property->value = PropertyValue::Array(std::move(values));
        }

        static void ApplyMoveOwned(ObjectGraphDocument& graph, const PatchOperation& operation)
        {
            auto* record = FindMutableRecord(graph, operation.target);
            if (!record)
                return;

            auto* property = FindMutableProperty(*record, operation.property.c_str());
            if (!property || property->value.GetKind() != PropertyValue::Kind::Array)
                return;

            auto values = property->value.GetArray();
            auto found = std::find_if(values.begin(), values.end(), [&operation](const PropertyValue& value)
            {
                return value.GetKind() == PropertyValue::Kind::OwnedReference && value.GetObjectId() == operation.object;
            });

            if (found == values.end())
                return;

            auto moved = *found;
            values.erase(found);
            const auto insertIndex = operation.hasIndex && operation.index < values.size()
                ? operation.index
                : values.size();
            values.insert(values.begin() + static_cast<std::ptrdiff_t>(insertIndex), std::move(moved));
            property->value = PropertyValue::Array(std::move(values));
        }

        static void ApplyRemoveObject(ObjectGraphDocument& graph, const PatchOperation& operation)
        {
            auto* record = FindMutableRecord(graph, operation.target);
            if (!record)
                return;

            std::vector<ObjectId> ownedChildren;
            for (const auto& property : record->properties)
            {
                if (property.value.GetKind() != PropertyValue::Kind::Array)
                    continue;

                for (const auto& value : property.value.GetArray())
                {
                    if (value.GetKind() == PropertyValue::Kind::OwnedReference)
                        ownedChildren.push_back(value.GetObjectId());
                }
            }

            record->state = ObjectRecordState::Removed;
            for (const auto& child : ownedChildren)
            {
                PatchOperation childOperation;
                childOperation.target = child;
                ApplyRemoveObject(graph, childOperation);
            }
        }

        static nlohmann::json ConvertPropertyValue(const PropertyValue& value)
        {
            switch (value.GetKind())
            {
            case PropertyValue::Kind::Null:
                return nullptr;
            case PropertyValue::Kind::Bool:
                return value.GetBool();
            case PropertyValue::Kind::Integer:
                return value.GetInteger();
            case PropertyValue::Kind::Number:
                return value.GetNumber();
            case PropertyValue::Kind::String:
                return value.GetString();
            case PropertyValue::Kind::Guid:
                return value.GetGuid().ToString();
            case PropertyValue::Kind::ObjectReference:
            {
                const auto& reference = value.GetObjectReference();
                return reference.guid.IsValid() ? nlohmann::json(reference.filePath) : nlohmann::json(nullptr);
            }
            case PropertyValue::Kind::Array:
            {
                nlohmann::json output = nlohmann::json::array();
                for (const auto& item : value.GetArray())
                    output.push_back(ConvertPropertyValue(item));
                return output;
            }
            case PropertyValue::Kind::Object:
            {
                nlohmann::json output = nlohmann::json::object();
                for (const auto& property : value.GetObject())
                    output[property.first] = ConvertPropertyValue(property.second);
                return output;
            }
            default:
                return nullptr;
            }
        }

        static bool ContainsAssetReference(const PropertyValue& value)
        {
            switch (value.GetKind())
            {
            case PropertyValue::Kind::ObjectReference:
                return value.GetObjectReference().guid.IsValid();
            case PropertyValue::Kind::Array:
                return std::any_of(
                    value.GetArray().begin(),
                    value.GetArray().end(),
                    [](const PropertyValue& item)
                    {
                        return ContainsAssetReference(item);
                    });
            case PropertyValue::Kind::Object:
                return std::any_of(
                    value.GetObject().begin(),
                    value.GetObject().end(),
                    [](const auto& property)
                    {
                        return ContainsAssetReference(property.second);
                    });
            default:
                return false;
            }
        }

        static void ApplyDeferredAssetReferenceHints(
            Components::Component& component,
            const ObjectRecord& record,
            InstanceContext& context,
            const LoadPolicy& policy)
        {
            if (policy.skipDeferredAssetReferenceCacheLookup)
            {
                if (RecordTypeMatchesComponent<Components::MeshFilter>(record) &&
                    component.GetType() == NLS_TYPEOF(Components::MeshFilter))
                {
                    auto* meshFilter = static_cast<Components::MeshFilter*>(&component);
                    const auto* mesh = FindProperty(record, "mesh");
                    if (mesh && mesh->value.GetKind() == PropertyValue::Kind::ObjectReference &&
                        mesh->value.GetObjectReference().guid.IsValid())
                    {
                        meshFilter->SetDeferredMeshObjectIdentifierHint(mesh->value.GetObjectReference());
                    }
                    return;
                }

                if (RecordTypeMatchesComponent<Components::MeshRenderer>(record) &&
                    component.GetType() == NLS_TYPEOF(Components::MeshRenderer))
                {
                    auto* meshRenderer = static_cast<Components::MeshRenderer*>(&component);
                    const auto* materials = FindProperty(record, "materials");
                    if (!materials || materials->value.GetKind() != PropertyValue::Kind::Array)
                        return;

                    NLS::Array<NLS::Engine::Serialize::ObjectIdentifier> references;
                    NLS::Array<std::string> paths;
                    const auto& values = materials->value.GetArray();
                    const auto materialCount = std::min(
                        values.size(),
                        static_cast<size_t>(Components::MeshRenderer::kMaxMaterialCount));
                    references.reserve(materialCount);
                    paths.reserve(materialCount);
                    for (size_t index = 0u; index < materialCount; ++index)
                    {
                        const auto& value = values[index];
                        if (value.GetKind() == PropertyValue::Kind::ObjectReference &&
                            value.GetObjectReference().guid.IsValid())
                        {
                            references.push_back(value.GetObjectReference());
                            paths.push_back(ResolveAssetReferencePath(value.GetObjectReference()));
                        }
                        else
                        {
                            references.push_back({});
                            paths.push_back({});
                        }
                    }
                    meshRenderer->SetMaterialObjectIdentifiers(references);
                    meshRenderer->SetMaterialPathHints(paths);
                    return;
                }

                return;
            }

            NLS::Base::Profiling::PerformanceStageScope hintScope(
                NLS::Base::Profiling::PerformanceStageDomain::Prefab,
                "ApplyDeferredAssetReferenceHints",
                NLS::Base::Profiling::PerformanceStageThread::Main);
            if (RecordTypeMatchesComponent<Components::MeshFilter>(record) &&
                component.GetType() == NLS_TYPEOF(Components::MeshFilter))
            {
                NLS::Base::Profiling::PerformanceStageScope meshHintScope(
                    NLS::Base::Profiling::PerformanceStageDomain::Prefab,
                    "ApplyDeferredMeshReferenceHint",
                    NLS::Base::Profiling::PerformanceStageThread::Main);
                auto* meshFilter = static_cast<Components::MeshFilter*>(&component);
                const auto* mesh = FindProperty(record, "mesh");
                if (mesh && mesh->value.GetKind() == PropertyValue::Kind::ObjectReference &&
                    mesh->value.GetObjectReference().guid.IsValid())
                {
                    const auto& reference = mesh->value.GetObjectReference();
                    const auto path = ResolveAssetReferencePath(reference);
                    meshFilter->SetDeferredMeshObjectIdentifierHint(reference);
                    if (policy.skipDeferredAssetReferenceCacheLookup)
                        return;
                    if (Core::ServiceLocator::Contains<Core::ResourceManagement::MeshManager>())
                    {
                        auto& meshManager = NLS_SERVICE(Core::ResourceManagement::MeshManager);
                        NLS::Base::Profiling::PerformanceStageScope meshLookupScope(
                            NLS::Base::Profiling::PerformanceStageDomain::Prefab,
                            "FindCachedMeshResource",
                            NLS::Base::Profiling::PerformanceStageThread::Main);
                        if (auto* cached = FindCachedMeshResource(meshManager, path, context))
                        {
                            meshFilter->SetResolvedMeshFromReference(cached);
                            return;
                        }
                    }
                }
                return;
            }

            if (RecordTypeMatchesComponent<Components::MeshRenderer>(record) &&
                component.GetType() == NLS_TYPEOF(Components::MeshRenderer))
            {
                NLS::Base::Profiling::PerformanceStageScope materialHintScope(
                    NLS::Base::Profiling::PerformanceStageDomain::Prefab,
                    "ApplyDeferredMaterialReferenceHints",
                    NLS::Base::Profiling::PerformanceStageThread::Main);
                auto* meshRenderer = static_cast<Components::MeshRenderer*>(&component);
                const auto* materials = FindProperty(record, "materials");
                if (!materials || materials->value.GetKind() != PropertyValue::Kind::Array)
                    return;

                NLS::Array<NLS::Engine::Serialize::ObjectIdentifier> references;
                NLS::Array<std::string> paths;
                const auto& values = materials->value.GetArray();
                const auto materialCount = std::min(
                    values.size(),
                    static_cast<size_t>(Components::MeshRenderer::kMaxMaterialCount));
                references.reserve(materialCount);
                paths.reserve(materialCount);
                for (size_t index = 0u; index < materialCount; ++index)
                {
                    const auto& value = values[index];
                    if (value.GetKind() == PropertyValue::Kind::ObjectReference && value.GetObjectReference().guid.IsValid())
                    {
                        references.push_back(value.GetObjectReference());
                        paths.push_back(ResolveAssetReferencePath(value.GetObjectReference()));
                    }
                    else
                    {
                        references.push_back({});
                        paths.push_back({});
                    }
                }
                meshRenderer->SetMaterialObjectIdentifiers(references);
                meshRenderer->SetMaterialPathHints(paths);
                if (policy.skipDeferredAssetReferenceCacheLookup)
                    return;
                if (Core::ServiceLocator::Contains<Core::ResourceManagement::MaterialManager>())
                {
                    for (size_t index = 0; index < paths.size(); ++index)
                    {
                        if (paths[index].empty())
                            continue;

                        NLS::Base::Profiling::PerformanceStageScope materialLookupScope(
                            NLS::Base::Profiling::PerformanceStageDomain::Prefab,
                            "FindCachedMaterialResource",
                            NLS::Base::Profiling::PerformanceStageThread::Main);
                        if (auto* cached = FindCachedMaterialResource(
                                NLS_SERVICE(Core::ResourceManagement::MaterialManager),
                                paths[index],
                                context))
                        {
                            meshRenderer->SetResolvedMaterialFromReference(static_cast<uint32_t>(index), *cached);
                        }
                    }
                }
            }
        }

        static Core::ResourceManagement::MeshManager::Mesh* FindCachedMeshResource(
            Core::ResourceManagement::MeshManager& meshManager,
            const std::string& path,
            InstanceContext& context)
        {
            const auto cacheKey = NormalizeResourceCacheLookupKey(path);
            if (!cacheKey.empty())
            {
                const auto cachedReference = context.meshResourcesByResolvedReferencePath.find(cacheKey);
                if (cachedReference != context.meshResourcesByResolvedReferencePath.end())
                {
                    ++context.meshReferenceCacheHitCount;
                    return cachedReference->second;
                }
            }

            ++context.meshReferenceCacheMissCount;
            ++context.meshResourceLookupCount;
            const auto candidates = BuildEquivalentResourcePathCandidates(
                path,
                Core::ResourceManagement::MeshManager::ResolveResourcePath(path),
                Core::ResourceManagement::MeshManager::ProjectAssetsRoot());
            auto* resource = FindCachedResourceByEquivalentPath(
                meshManager,
                candidates,
                context.meshResourcesByNormalizedPath,
                context.meshResourcePathIndexBuilt);
            if (!cacheKey.empty())
                context.meshResourcesByResolvedReferencePath.emplace(cacheKey, resource);
            return resource;
        }

        static Core::ResourceManagement::MaterialManager::Material* FindCachedMaterialResource(
            Core::ResourceManagement::MaterialManager& materialManager,
            const std::string& path,
            InstanceContext& context)
        {
            const auto cacheKey = NormalizeResourceCacheLookupKey(path);
            if (!cacheKey.empty())
            {
                const auto cachedReference = context.materialResourcesByResolvedReferencePath.find(cacheKey);
                if (cachedReference != context.materialResourcesByResolvedReferencePath.end())
                {
                    ++context.materialReferenceCacheHitCount;
                    return cachedReference->second;
                }
            }

            ++context.materialReferenceCacheMissCount;
            ++context.materialResourceLookupCount;
            const auto candidates = BuildEquivalentResourcePathCandidates(
                path,
                Core::ResourceManagement::MaterialManager::ResolveResourcePath(path),
                Core::ResourceManagement::MeshManager::ProjectAssetsRoot());
            auto* resource = FindCachedResourceByEquivalentPath(
                materialManager,
                candidates,
                context.materialResourcesByNormalizedPath,
                context.materialResourcePathIndexBuilt);
            if (!cacheKey.empty())
                context.materialResourcesByResolvedReferencePath.emplace(cacheKey, resource);
            return resource;
        }

        static std::string ToProjectRelativeResourcePath(
            const std::string& path,
            const std::string& projectAssetsRoot)
        {
            if (path.empty() || projectAssetsRoot.empty())
                return {};

            const auto absolutePath = std::filesystem::path(path).lexically_normal();
            if (!absolutePath.is_absolute())
                return {};

            auto assetsRoot = std::filesystem::path(projectAssetsRoot).lexically_normal();
            while (!assetsRoot.empty() && !assetsRoot.has_filename())
                assetsRoot = assetsRoot.parent_path();

            const auto projectRoot = assetsRoot.parent_path();
            if (projectRoot.empty())
                return {};

            const auto relative = absolutePath.lexically_relative(projectRoot.lexically_normal());
            if (relative.empty() || relative.is_absolute())
                return {};

            for (const auto& part : relative)
            {
                if (part == "..")
                    return {};
            }

            return relative.generic_string();
        }

        static std::vector<std::string> BuildEquivalentResourcePathCandidates(
            const std::string& path,
            const std::string& resolvedPath,
            const std::string& projectAssetsRoot)
        {
            std::vector<std::string> candidates;
            auto addCandidate = [&candidates](const std::string& candidate)
            {
                if (candidate.empty() ||
                    std::find(candidates.begin(), candidates.end(), candidate) != candidates.end())
                {
                    return;
                }
                candidates.push_back(candidate);
            };

            auto addPathVariants = [&addCandidate](const std::string& candidate)
            {
                if (candidate.empty())
                    return;

                addCandidate(candidate);
                const auto normalized = std::filesystem::path(candidate).lexically_normal();
                addCandidate(normalized.string());
                addCandidate(normalized.generic_string());
            };

            addPathVariants(path);
            addPathVariants(resolvedPath);
            addPathVariants(ToProjectRelativeResourcePath(
                resolvedPath.empty() ? path : resolvedPath,
                projectAssetsRoot));
            return candidates;
        }

        static std::string NormalizeResourceCacheLookupKey(std::string path)
        {
            if (path.empty())
                return {};

            std::replace(path.begin(), path.end(), '\\', '/');
            path = std::filesystem::path(path).lexically_normal().generic_string();
            return path;
        }

        template <typename ResourceManagerType>
        using ResourcePointerForManager = decltype(std::declval<ResourceManagerType&>().GetResource(std::declval<std::string>(), false));

        template <typename ResourceManagerType>
        static void BuildNormalizedResourceCacheIndex(
            ResourceManagerType& resourceManager,
            std::unordered_map<std::string, ResourcePointerForManager<ResourceManagerType>>& normalizedResourceCache)
        {
            normalizedResourceCache.clear();
            const auto resources = resourceManager.GetResources();
            normalizedResourceCache.reserve(resources.size());
            for (const auto& [resourcePath, resource] : resources)
            {
                if (resource == nullptr)
                    continue;

                const auto normalizedResourcePath = NormalizeResourceCacheLookupKey(resourcePath);
                if (!normalizedResourcePath.empty())
                    normalizedResourceCache.try_emplace(normalizedResourcePath, resource);
            }
        }

        template <typename ResourceManagerType>
        static ResourcePointerForManager<ResourceManagerType> FindCachedResourceByEquivalentPath(
            ResourceManagerType& resourceManager,
            const std::vector<std::string>& candidates,
            std::unordered_map<std::string, ResourcePointerForManager<ResourceManagerType>>& normalizedResourceCache,
            bool& normalizedResourceCacheBuilt)
        {
            for (const auto& candidate : candidates)
            {
                if (auto* cached = resourceManager.GetResource(candidate, false))
                    return cached;
            }

            std::vector<std::string> normalizedCandidates;
            normalizedCandidates.reserve(candidates.size());
            for (const auto& candidate : candidates)
            {
                auto normalized = NormalizeResourceCacheLookupKey(candidate);
                if (!normalized.empty() &&
                    std::find(normalizedCandidates.begin(), normalizedCandidates.end(), normalized) ==
                        normalizedCandidates.end())
                {
                    normalizedCandidates.push_back(std::move(normalized));
                }
            }

            if (!normalizedResourceCacheBuilt)
            {
                BuildNormalizedResourceCacheIndex(resourceManager, normalizedResourceCache);
                normalizedResourceCacheBuilt = true;
            }

            for (const auto& normalizedCandidate : normalizedCandidates)
            {
                const auto cached = normalizedResourceCache.find(normalizedCandidate);
                if (cached != normalizedResourceCache.end())
                    return cached->second;
            }
            return ResourcePointerForManager<ResourceManagerType> {};
        }

        static void AnalyzeObjectTypes(
            const ObjectGraphDocument& document,
            const LoadPolicy& policy,
            SerializationDiagnosticList& diagnostics)
        {
            for (const auto& object : document.objects)
            {
                if (!IsInstantiableRecordState(object.state))
                    continue;

                const auto type = NLS::meta::Type::GetFromName(object.typeName);
                if (type.IsValid())
                    continue;

                diagnostics.Add({
                    SerializationDiagnosticCode::UnknownType,
                    policy.unknownTypePolicy == UnknownTypePolicy::Fail
                        ? SerializationDiagnosticSeverity::Error
                        : SerializationDiagnosticSeverity::Warning,
                    "Object graph contains an unknown object type: " + object.typeName
                });
            }
        }

        static void AnalyzeReflectedObjectReferenceShapes(
            const ObjectGraphDocument& document,
            const LoadPolicy& policy,
            SerializationDiagnosticList& diagnostics)
        {
            for (const auto& object : document.objects)
            {
                if (!IsInstantiableRecordState(object.state))
                    continue;

                const auto type = NLS::meta::Type::GetFromName(object.typeName);
                if (!type.IsValid())
                    continue;

                for (const auto& property : object.properties)
                {
                    const auto field = type.GetField(property.name);
                    if (!field.IsValid())
                        continue;

                    if (policy.deferAssetReferenceResolution &&
                        IsDeferredAssetProperty(object, property.name))
                    {
                        continue;
                    }

                    const auto fieldType = field.GetType();
                    if (fieldType.IsArray() && Internal::IsPPtrTypeName(fieldType.GetArrayType()))
                    {
                        if (!IsPPtrArrayPropertyValue(property.value))
                            AddInvalidObjectReferenceDiagnostic(policy, diagnostics, object, property);
                        continue;
                    }

                    if (Internal::IsPPtrTypeName(fieldType) && !IsPPtrPropertyValue(property.value))
                        AddInvalidObjectReferenceDiagnostic(policy, diagnostics, object, property);
                }
            }
        }

        static bool IsPPtrPropertyValue(const PropertyValue& value)
        {
            return value.GetKind() == PropertyValue::Kind::ObjectReference;
        }

        static bool IsPPtrArrayPropertyValue(const PropertyValue& value)
        {
            if (value.GetKind() != PropertyValue::Kind::Array)
                return false;

            for (const auto& item : value.GetArray())
            {
                if (!IsPPtrPropertyValue(item))
                    return false;
            }
            return true;
        }

        static void AddInvalidObjectReferenceDiagnostic(
            const LoadPolicy& policy,
            SerializationDiagnosticList& diagnostics,
            const ObjectRecord& object,
            const PropertyRecord& property)
        {
            diagnostics.Add({
                SerializationDiagnosticCode::InvalidPropertyType,
                policy.invalidReferencePolicy == InvalidReferencePolicy::Fail
                    ? SerializationDiagnosticSeverity::Error
                    : SerializationDiagnosticSeverity::Warning,
                "Object graph property \"" + object.typeName + "." + property.name +
                    "\" must use a Unity-style object reference shape."
            });
        }

        static void AnalyzeAssetReferences(
            const ObjectGraphDocument& document,
            const LoadPolicy& policy,
            SerializationDiagnosticList& diagnostics)
        {
            if (document.basePrefab.has_value())
                AnalyzeObjectIdentifier(PropertyValue::ObjectReference(*document.basePrefab), policy, diagnostics);

            for (const auto& object : document.objects)
            {
                if (!IsInstantiableRecordState(object.state))
                    continue;

                for (const auto& property : object.properties)
                    AnalyzeObjectIdentifier(property.value, policy, diagnostics);
            }
        }

        static void AnalyzeObjectIdentifier(
            const PropertyValue& value,
            const LoadPolicy& policy,
            SerializationDiagnosticList& diagnostics)
        {
            switch (value.GetKind())
            {
            case PropertyValue::Kind::ObjectReference:
            {
                const auto& reference = value.GetObjectReference();
                if (reference.IsAsset() &&
                    Core::ServiceLocator::Contains<Assets::RuntimeAssetDatabase>() &&
                    ResolveRuntimeAssetReferenceEntry(reference) == nullptr)
                {
                    AddMissingAssetDiagnostic(policy, diagnostics);
                    break;
                }

                if (!reference.guid.IsValid() &&
                    (reference.fileType != FileType::NonAssetType || !reference.filePath.empty()))
                {
                    AddMissingAssetDiagnostic(policy, diagnostics);
                }
                break;
            }
            case PropertyValue::Kind::Array:
                for (const auto& item : value.GetArray())
                    AnalyzeObjectIdentifier(item, policy, diagnostics);
                break;
            case PropertyValue::Kind::Object:
                for (const auto& property : value.GetObject())
                    AnalyzeObjectIdentifier(property.second, policy, diagnostics);
                break;
            default:
                break;
            }
        }

        static void AddMissingAssetDiagnostic(
            const LoadPolicy& policy,
            SerializationDiagnosticList& diagnostics)
        {
            diagnostics.Add({
                SerializationDiagnosticCode::MissingAsset,
                policy.missingAssetPolicy == MissingAssetPolicy::Fail
                    ? SerializationDiagnosticSeverity::Error
                    : SerializationDiagnosticSeverity::Warning,
                "Object graph contains a missing or invalid asset reference."
            });
        }
    };
}

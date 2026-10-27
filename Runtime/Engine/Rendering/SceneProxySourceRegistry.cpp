#include "Rendering/SceneProxySourceRegistry.h"

#include "Components/LightComponent.h"
#include "Components/MeshFilter.h"
#include "Components/MeshRenderer.h"
#include "Components/SkyBoxComponent.h"
#include "Core/ResourceManagement/MaterialManager.h"
#include "Core/ResourceManagement/MeshManager.h"
#include "Core/ResourceManagement/ResourceLifetimeRegistry.h"
#include "Core/ServiceLocator.h"
#include "GameObject.h"
#include "SceneSystem/Scene.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace NLS::Engine::Rendering
{
    namespace
    {
        template <typename T>
        [[nodiscard]] bool ValueEquals(const T& lhs, const T& rhs) noexcept
        {
            return std::memcmp(&lhs, &rhs, sizeof(T)) == 0;
        }

        struct TransformStamp
        {
            Maths::Matrix4 worldMatrix = Maths::Matrix4::Identity;
            Maths::Vector3 worldPosition {};
            Maths::Quaternion worldRotation = Maths::Quaternion::Identity;
            Maths::Vector3 worldScale {1.0f, 1.0f, 1.0f};
            uint64_t transformRevision = 0u;

            [[nodiscard]] bool HasSameWorldTransform(const TransformStamp& other) const noexcept
            {
                return ValueEquals(worldMatrix, other.worldMatrix) &&
                    ValueEquals(worldPosition, other.worldPosition) &&
                    ValueEquals(worldRotation, other.worldRotation) &&
                    ValueEquals(worldScale, other.worldScale);
            }

            [[nodiscard]] bool operator==(const TransformStamp& other) const noexcept
            {
                return transformRevision == other.transformRevision &&
                    HasSameWorldTransform(other);
            }
        };

        struct PrimitiveStamp
        {
            uint64_t ownerInstanceId = 0u;
            uint64_t parentInstanceId = 0u;
            uint64_t ownerRenderRevision = 0u;
            uint64_t meshFilterRevision = 0u;
            uint64_t meshRendererRevision = 0u;
            uint64_t meshContentRevision = 0u;
            uint64_t materialInstanceId = 0u;
            uint64_t requestedMaterialInstanceId = 0u;
            uint64_t materialParameterRevision = 0u;
            uint64_t materialRenderStateRevision = 0u;
            NLS::Render::Resources::Mesh* mesh = nullptr;
            NLS::Render::Resources::Material* material = nullptr;
            bool hasExplicitMaterialPath = false;
            bool materialResolved = false;
            std::string requestedMaterialPath;
            std::string sourceObjectKey;
            uint64_t hlodScopeRootInstanceId = 0u;
            std::string hlodScopeMetadata;
            uint32_t layer = 0u;
            uint8_t frustumBehaviour = 0u;
            bool ownerAlive = false;
            bool ownerActive = false;
            bool transientRenderingSuppressed = false;

            [[nodiscard]] bool PayloadEquals(const PrimitiveStamp& other) const noexcept
            {
                return ownerInstanceId == other.ownerInstanceId &&
                    parentInstanceId == other.parentInstanceId &&
                    meshFilterRevision == other.meshFilterRevision &&
                    meshRendererRevision == other.meshRendererRevision &&
                    meshContentRevision == other.meshContentRevision &&
                    materialInstanceId == other.materialInstanceId &&
                    requestedMaterialInstanceId == other.requestedMaterialInstanceId &&
                    materialParameterRevision == other.materialParameterRevision &&
                    materialRenderStateRevision == other.materialRenderStateRevision &&
                    mesh == other.mesh && material == other.material &&
                    hasExplicitMaterialPath == other.hasExplicitMaterialPath &&
                    materialResolved == other.materialResolved &&
                    requestedMaterialPath == other.requestedMaterialPath &&
                    sourceObjectKey == other.sourceObjectKey &&
                    hlodScopeRootInstanceId == other.hlodScopeRootInstanceId &&
                    hlodScopeMetadata == other.hlodScopeMetadata &&
                    layer == other.layer &&
                    frustumBehaviour == other.frustumBehaviour &&
                    ownerAlive == other.ownerAlive &&
                    ownerActive == other.ownerActive &&
                    transientRenderingSuppressed == other.transientRenderingSuppressed;
            }
        };

        struct LightStamp
        {
            uint64_t ownerInstanceId = 0u;
            uint64_t parentInstanceId = 0u;
            NLS::Render::Settings::ELightType type = NLS::Render::Settings::ELightType::POINT;
            Maths::Vector3 color {};
            float intensity = 0.0f;
            float range = 0.0f;
            float cutoff = 0.0f;
            float outerCutoff = 0.0f;
            Maths::Vector3 size {1.0f, 1.0f, 1.0f};
            bool ownerAlive = false;
            bool ownerActive = false;

            [[nodiscard]] bool PayloadEquals(const LightStamp& other) const noexcept
            {
                return ownerInstanceId == other.ownerInstanceId &&
                    parentInstanceId == other.parentInstanceId &&
                    type == other.type && ValueEquals(color, other.color) && intensity == other.intensity &&
                    range == other.range && cutoff == other.cutoff &&
                    outerCutoff == other.outerCutoff && ValueEquals(size, other.size) &&
                    ownerAlive == other.ownerAlive &&
                    ownerActive == other.ownerActive;
            }
        };

        struct SkyBoxStamp
        {
            uint64_t ownerInstanceId = 0u;
            const void* mesh = nullptr;
            const void* material = nullptr;
            bool ownerAlive = false;
            bool ownerActive = false;

            [[nodiscard]] bool PayloadEquals(const SkyBoxStamp& other) const noexcept
            {
                return ownerInstanceId == other.ownerInstanceId &&
                    mesh == other.mesh && material == other.material &&
                    ownerAlive == other.ownerAlive && ownerActive == other.ownerActive;
            }
        };

        [[nodiscard]] uint64_t AdvanceTransformRevision(const uint64_t revision) noexcept
        {
            const auto nextRevision = revision + 1u;
            return nextRevision != 0u ? nextRevision : 1u;
        }

        TransformStamp BuildTransformStamp(
            const GameObject& owner,
            const TransformStamp* previous = nullptr)
        {
            TransformStamp stamp;
            if (const auto* transform = owner.GetTransform())
            {
                stamp.worldMatrix = transform->GetWorldMatrix();
                stamp.worldPosition = transform->GetWorldPosition();
                stamp.worldRotation = transform->GetWorldRotation();
                stamp.worldScale = transform->GetWorldScale();
                stamp.transformRevision = transform->GetRenderRevision();
                if (previous != nullptr)
                {
                    stamp.transformRevision = stamp.HasSameWorldTransform(*previous)
                        ? previous->transformRevision
                        : AdvanceTransformRevision(previous->transformRevision);
                }
                else if (stamp.transformRevision == 0u)
                {
                    stamp.transformRevision = 1u;
                }
            }
            return stamp;
        }

        SceneProxyTransformState ToProxyTransform(const TransformStamp& stamp)
        {
            SceneProxyTransformState transform;
            transform.worldMatrix = stamp.worldMatrix;
            transform.worldPosition = stamp.worldPosition;
            transform.worldRotation = stamp.worldRotation;
            transform.worldScale = stamp.worldScale;
            transform.transformRevision = stamp.transformRevision;
            return transform;
        }

        std::shared_ptr<SceneProxyResourceBinding::MeshHandle> AcquireMeshHandle(
            const std::string& ownerToken,
            const std::string& path)
        {
            if (path.empty() ||
                !NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MeshManager>() ||
                !NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::ResourceLifetimeRegistry>())
            {
                return {};
            }

            auto handle = std::make_shared<SceneProxyResourceBinding::MeshHandle>(
                NLS_SERVICE(NLS::Core::ResourceManagement::MeshManager).AcquireRegisteredMeshHandle(
                    NLS_SERVICE(NLS::Core::ResourceManagement::ResourceLifetimeRegistry),
                    ownerToken,
                    path));
            return handle->IsValid() ? handle : nullptr;
        }

        std::shared_ptr<SceneProxyResourceBinding::MaterialHandle> AcquireMaterialHandle(
            const std::string& ownerToken,
            const std::string& path)
        {
            if (path.empty() ||
                !NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::MaterialManager>() ||
                !NLS::Core::ServiceLocator::Contains<NLS::Core::ResourceManagement::ResourceLifetimeRegistry>())
            {
                return {};
            }

            auto handle = std::make_shared<SceneProxyResourceBinding::MaterialHandle>(
                NLS_SERVICE(NLS::Core::ResourceManagement::MaterialManager).AcquireRegisteredMaterialHandle(
                    NLS_SERVICE(NLS::Core::ResourceManagement::ResourceLifetimeRegistry),
                    ownerToken,
                    path));
            return handle->IsValid() ? handle : nullptr;
        }

        SceneProxyResourceBinding BuildMeshResourceBinding(
            const std::string& ownerToken,
            NLS::Render::Resources::Mesh* mesh,
            NLS::Render::Resources::Material* material,
            const std::string& meshPath)
        {
            SceneProxyResourceBinding binding;
            binding.mesh = mesh;
            binding.material = material;
            binding.meshPath = meshPath;
            binding.meshHandle = AcquireMeshHandle(ownerToken, meshPath);
            binding.materialPath = material != nullptr ? material->path : std::string {};
            binding.materialHandle = AcquireMaterialHandle(ownerToken, binding.materialPath);
            binding.meshContentRevision = mesh != nullptr ? mesh->GetContentRevision() : 0u;
            binding.materialInstanceId = material != nullptr ? material->GetInstanceId() : 0u;
            binding.materialParameterRevision = material != nullptr ? material->GetParameterRevision() : 0u;
            binding.materialRenderStateRevision = material != nullptr ? material->GetRenderStateRevision() : 0u;
            binding.valid = mesh != nullptr && material != nullptr &&
                material->IsValid();
            return binding;
        }

        SceneProxyResourceBinding BuildSkyBoxResourceBinding(
            const std::string& ownerToken,
            NLS::Render::Resources::Mesh* mesh,
            NLS::Render::Resources::Material* material)
        {
            SceneProxyResourceBinding binding;
            binding.mesh = mesh;
            binding.material = material;
            binding.materialPath = material != nullptr ? material->path : std::string {};
            binding.materialHandle = AcquireMaterialHandle(ownerToken, binding.materialPath);
            binding.meshContentRevision = mesh != nullptr ? mesh->GetContentRevision() : 0u;
            binding.materialInstanceId = material != nullptr ? material->GetInstanceId() : 0u;
            binding.materialParameterRevision = material != nullptr ? material->GetParameterRevision() : 0u;
            binding.materialRenderStateRevision = material != nullptr ? material->GetRenderStateRevision() : 0u;
            binding.valid = mesh != nullptr && material != nullptr && material->IsValid();
            return binding;
        }

        struct HLODScopeSource
        {
            uint64_t rootInstanceId = 0u;
            std::string metadata;
        };

        HLODScopeSource ResolveHLODScope(const GameObject& owner)
        {
            for (const GameObject* candidate = &owner;
                 candidate != nullptr;
                 candidate = candidate->GetParent())
            {
                if (!candidate->GetLargeSceneHLODMetadata().empty())
                {
                    return {
                        static_cast<uint64_t>(candidate->GetInstanceID()),
                        candidate->GetLargeSceneHLODMetadata()
                    };
                }
            }
            return {};
        }

        void PopulatePrimitivePayload(
            SceneProxyPayload& payload,
            const PrimitiveStamp& stamp,
            const Components::MeshRenderer& meshRenderer,
            const Components::MeshFilter* meshFilter,
            const std::string& ownerToken)
        {
            payload.kind = SceneProxyKind::Primitive;
            payload.ownerInstanceId = stamp.ownerInstanceId;
            payload.parentInstanceId = stamp.parentInstanceId;
            payload.sourceObjectKey = stamp.sourceObjectKey;
            payload.hlodScopeRootInstanceId = stamp.hlodScopeRootInstanceId;
            payload.hlodScopeMetadata = stamp.hlodScopeMetadata;
            payload.resources = BuildMeshResourceBinding(
                ownerToken,
                stamp.mesh,
                stamp.material,
                meshFilter != nullptr ? meshFilter->GetModelPath() : std::string {});
            payload.resources.hasExplicitMaterialPath = stamp.hasExplicitMaterialPath;
            if(meshFilter)payload.resources.transientMeshOwner=meshFilter->GetResolvedTransientMesh();
            payload.resources.materialResolved = stamp.materialResolved;
            payload.resources.requestedMaterialPath = stamp.requestedMaterialPath;
            payload.resources.requestedMaterialInstanceId = stamp.requestedMaterialInstanceId;
            payload.modelBoundingSphere = stamp.mesh != nullptr
                ? stamp.mesh->GetBoundingSphere()
                : NLS::Render::Geometry::BoundingSphere {};
            payload.customBoundingSphere = meshRenderer.GetCustomBoundingSphere();
            payload.modelBounds = stamp.mesh != nullptr
                ? stamp.mesh->GetBounds()
                : NLS::Render::Geometry::Bounds {};
            payload.userMatrix = meshRenderer.GetUserMatrix();
            payload.affectDistanceField = meshRenderer.GetAffectDistanceField();
            payload.distanceFieldMostlyStatic = meshRenderer.GetDistanceFieldMostlyStatic();
            payload.layer = stamp.layer;
            payload.ownerRenderRevision = stamp.ownerRenderRevision;
            payload.meshRendererRenderRevision = stamp.meshRendererRevision;
            payload.ownerAlive = stamp.ownerAlive;
            payload.ownerActive = stamp.ownerActive;
            payload.transientRenderingSuppressed = stamp.transientRenderingSuppressed;
            payload.frustumBehaviour = stamp.frustumBehaviour;
        }
    }

    struct SceneProxySourceRegistry::Impl final
    {
        struct CollectionStamp final
        {
            uint64_t sceneId = 0u;
            uint64_t sceneRevision = 0u;
            uint64_t fastAccessRevision = 0u;
            uint64_t meshMutationEpoch = 0u;
            uint64_t materialMutationEpoch = 0u;
            const NLS::Render::Resources::Material* defaultMaterial = nullptr;
            const NLS::Render::Resources::Material* overrideMaterial = nullptr;
            const LargeSceneSettings* largeSceneSettings = nullptr;
            bool requireExplicitMaterialTextures = false;
            bool allowDefaultMaterialForUnresolvedExplicitMaterials = false;

            [[nodiscard]] bool operator==(const CollectionStamp& other) const noexcept = default;
        };

        struct PrimitiveEntry
        {
            SceneProxyHandle handle;
            TransformStamp transform;
            PrimitiveStamp stamp;
        };

        struct LightEntry
        {
            SceneProxyHandle handle;
            TransformStamp transform;
            LightStamp stamp;
        };

        struct SkyBoxEntry
        {
            SceneProxyHandle handle;
            TransformStamp transform;
            SkyBoxStamp stamp;
        };

        uint64_t sceneId = 0u;
        uint32_t nextProxyIndex = 0u;
        std::unordered_map<const void*, PrimitiveEntry> primitives;
        std::unordered_map<const void*, LightEntry> lights;
        std::unordered_map<const void*, SkyBoxEntry> skyboxes;
        CollectionStamp lastCollectionStamp;
        uint64_t collectionScanCount = 0u;
        bool hasLastCollectionStamp = false;

        SceneProxyHandle AllocateHandle()
        {
            SceneProxyHandle handle;
            handle.sceneId = sceneId;
            handle.index = nextProxyIndex++;
            handle.generation = 1u;
            return handle;
        }

        void ResetForScene(const uint64_t newSceneId)
        {
            if (sceneId != newSceneId)
            {
                primitives.clear();
                lights.clear();
                skyboxes.clear();
                nextProxyIndex = 0u;
                sceneId = newSceneId;
                hasLastCollectionStamp = false;
            }
        }
    };

    SceneProxySourceRegistry::SceneProxySourceRegistry()
        : m_impl(std::make_unique<Impl>())
    {
    }

    SceneProxySourceRegistry::~SceneProxySourceRegistry() = default;

    void SceneProxySourceRegistry::Reset()
    {
        m_impl->primitives.clear();
        m_impl->lights.clear();
        m_impl->skyboxes.clear();
        m_impl->nextProxyIndex = 0u;
        m_impl->sceneId = 0u;
        m_impl->lastCollectionStamp = {};
        m_impl->collectionScanCount = 0u;
        m_impl->hasLastCollectionStamp = false;
    }

    SceneProxyUpdateBatch SceneProxySourceRegistry::Collect(
        SceneSystem::Scene& scene,
        const SceneProxyExtractionOptions& options,
        const uint64_t frameId)
    {
        m_impl->ResetForScene(scene.GetInstanceID());

        SceneProxyUpdateBatch batch;
        batch.frameId = frameId;
        batch.sceneId = m_impl->sceneId;
        batch.sceneRevision = scene.GetRenderContentRevision();

        Impl::CollectionStamp collectionStamp;
        collectionStamp.sceneId = batch.sceneId;
        collectionStamp.sceneRevision = batch.sceneRevision;
        collectionStamp.fastAccessRevision = scene.GetFastAccessComponentsRevision();
        collectionStamp.meshMutationEpoch = NLS::Render::Resources::Mesh::GetGlobalMutationEpoch();
        collectionStamp.materialMutationEpoch = NLS::Render::Resources::Material::GetGlobalMutationEpoch();
        collectionStamp.defaultMaterial = options.defaultMaterial;
        collectionStamp.overrideMaterial = options.overrideMaterial;
        collectionStamp.largeSceneSettings = options.largeSceneSettings;
        collectionStamp.requireExplicitMaterialTextures = options.requireExplicitMaterialTextures;
        collectionStamp.allowDefaultMaterialForUnresolvedExplicitMaterials =
            options.allowDefaultMaterialForUnresolvedExplicitMaterials;
        if (m_impl->hasLastCollectionStamp &&
            m_impl->lastCollectionStamp == collectionStamp)
        {
            return batch;
        }
        ++m_impl->collectionScanCount;

        const auto& fastAccess = scene.GetFastAccessComponents();
        std::unordered_set<const void*> livePrimitives;
        std::unordered_set<const void*> liveLights;
        std::unordered_set<const void*> liveSkyBoxes;
        livePrimitives.reserve(fastAccess.modelRenderers.size());
        liveLights.reserve(fastAccess.lights.size());
        liveSkyBoxes.reserve(fastAccess.skyboxs.size());

        const auto makeOwnerToken = [this](const SceneProxyHandle handle)
        {
            return std::string("SceneProxy:") + std::to_string(handle.sceneId) + ":" +
                std::to_string(handle.index);
        };

        for (auto* meshRenderer : fastAccess.modelRenderers)
        {
            if (meshRenderer == nullptr)
                continue;

            livePrimitives.insert(meshRenderer);
            auto* owner = meshRenderer->gameobject();
            if (owner == nullptr || !owner->IsAlive() || meshRenderer->IsTransientRenderingSuppressed())
            {
                const auto found = m_impl->primitives.find(meshRenderer);
                if (found != m_impl->primitives.end())
                {
                    batch.commands.push_back({ SceneProxyCommandKind::Remove, found->second.handle });
                    m_impl->primitives.erase(found);
                }
                continue;
            }

            auto* meshFilter = owner->GetComponent<Components::MeshFilter>();
            auto* mesh = meshFilter != nullptr ? meshFilter->ResolveMesh() : nullptr;
            const auto materialPaths = meshRenderer->GetMaterialPaths();
            const size_t materialIndex = mesh != nullptr ? mesh->GetMaterialIndex() : 0u;
            const bool hasExplicitMaterialPath =
                materialIndex < materialPaths.size() &&
                !materialPaths[materialIndex].empty();
            const std::string requestedMaterialPath = hasExplicitMaterialPath
                ? materialPaths[materialIndex]
                : std::string {};
            auto* material = options.overrideMaterial;
            if (material == nullptr && mesh != nullptr)
                material = meshRenderer->ResolveMaterialAtIndex(mesh->GetMaterialIndex());
            const uint64_t requestedMaterialInstanceId =
                material != nullptr ? material->GetInstanceId() : 0u;
            const bool materialResolved = material != nullptr && material->IsValid();
            if (!materialResolved && hasExplicitMaterialPath &&
                !options.allowDefaultMaterialForUnresolvedExplicitMaterials)
            {
                material = nullptr;
            }
            else if (!materialResolved)
                material = options.defaultMaterial;

            PrimitiveStamp stamp;
            stamp.ownerInstanceId = static_cast<uint64_t>(owner->GetInstanceID());
            stamp.parentInstanceId = owner->GetParent() != nullptr
                ? static_cast<uint64_t>(owner->GetParent()->GetInstanceID())
                : 0u;
            stamp.ownerRenderRevision = owner->GetRenderStateRevision();
            stamp.meshFilterRevision = meshFilter != nullptr ? meshFilter->GetRenderRevision() : 0u;
            stamp.meshRendererRevision = meshRenderer->GetRenderRevision();
            stamp.meshContentRevision = mesh != nullptr ? mesh->GetContentRevision() : 0u;
            stamp.materialInstanceId = material != nullptr ? material->GetInstanceId() : 0u;
            stamp.materialParameterRevision = material != nullptr ? material->GetParameterRevision() : 0u;
            stamp.materialRenderStateRevision = material != nullptr ? material->GetRenderStateRevision() : 0u;
            stamp.mesh = mesh;
            stamp.material = material;
            stamp.hasExplicitMaterialPath = hasExplicitMaterialPath;
            stamp.materialResolved = materialResolved;
            stamp.requestedMaterialPath = requestedMaterialPath;
            stamp.requestedMaterialInstanceId = requestedMaterialInstanceId;
            stamp.sourceObjectKey = owner->GetSourceObjectKey();
            const auto hlodScope = ResolveHLODScope(*owner);
            stamp.hlodScopeRootInstanceId = hlodScope.rootInstanceId;
            stamp.hlodScopeMetadata = hlodScope.metadata;
            stamp.layer = static_cast<uint32_t>(std::clamp(owner->GetLayer(), 0, 31));
            stamp.frustumBehaviour = static_cast<uint8_t>(meshRenderer->GetFrustumBehaviour());
            stamp.ownerAlive = owner->IsAlive();
            stamp.ownerActive = owner->IsActive();
            stamp.transientRenderingSuppressed = meshRenderer->IsTransientRenderingSuppressed();

            const auto found = m_impl->primitives.find(meshRenderer);
            if (found == m_impl->primitives.end())
            {
                const auto handle = m_impl->AllocateHandle();
                const auto ownerToken = makeOwnerToken(handle);
                SceneProxyPayload payload;
                PopulatePrimitivePayload(
                    payload,
                    stamp,
                    *meshRenderer,
                    meshFilter,
                    ownerToken);

                const auto transform = BuildTransformStamp(*owner);
                batch.commands.push_back({ SceneProxyCommandKind::Add, handle, std::move(payload), ToProxyTransform(transform) });
                m_impl->primitives.emplace(meshRenderer, Impl::PrimitiveEntry { handle, transform, stamp });
                continue;
            }

            const auto transform = BuildTransformStamp(*owner, &found->second.transform);
            if (found->second.stamp.PayloadEquals(stamp))
            {
                if (!(found->second.transform == transform))
                {
                    batch.commands.push_back({ SceneProxyCommandKind::UpdateTransform, found->second.handle, {}, ToProxyTransform(transform) });
                    found->second.transform = transform;
                }
                continue;
            }

            const auto ownerToken = makeOwnerToken(found->second.handle);
            SceneProxyPayload payload;
            PopulatePrimitivePayload(
                payload,
                stamp,
                *meshRenderer,
                meshFilter,
                ownerToken);
            batch.commands.push_back({ SceneProxyCommandKind::UpdatePayload, found->second.handle, std::move(payload), ToProxyTransform(transform) });
            found->second.stamp = stamp;
            found->second.transform = transform;
        }

        for (auto* light : fastAccess.lights)
        {
            if (light == nullptr)
                continue;

            liveLights.insert(light);
            auto* owner = light->gameobject();
            if (owner == nullptr || !owner->IsAlive())
            {
                const auto found = m_impl->lights.find(light);
                if (found != m_impl->lights.end())
                {
                    batch.commands.push_back({ SceneProxyCommandKind::Remove, found->second.handle });
                    m_impl->lights.erase(found);
                }
                continue;
            }

            LightStamp stamp;
            stamp.ownerInstanceId = static_cast<uint64_t>(owner->GetInstanceID());
            stamp.parentInstanceId = owner->GetParent() != nullptr
                ? static_cast<uint64_t>(owner->GetParent()->GetInstanceID())
                : 0u;
            stamp.type = light->GetLightType();
            stamp.color = light->GetColor();
            stamp.intensity = light->GetIntensity();
            stamp.range = light->GetRange();
            stamp.cutoff = light->GetCutoff();
            stamp.outerCutoff = light->GetOuterCutoff();
            stamp.size = light->GetSize();
            stamp.ownerAlive = owner->IsAlive();
            stamp.ownerActive = owner->IsActive();

            const auto found = m_impl->lights.find(light);
            if (found == m_impl->lights.end())
            {
                const auto handle = m_impl->AllocateHandle();
                SceneProxyPayload payload;
                payload.kind = SceneProxyKind::Light;
                payload.ownerInstanceId = stamp.ownerInstanceId;
                payload.parentInstanceId = stamp.parentInstanceId;
                payload.ownerAlive = stamp.ownerAlive;
                payload.ownerActive = stamp.ownerActive;
                payload.light = SceneProxyLightData {
                    stamp.type,
                    stamp.color,
                    stamp.intensity,
                    stamp.range,
                    stamp.cutoff,
                    stamp.outerCutoff,
                    stamp.size };
                const auto transform = BuildTransformStamp(*owner);
                batch.commands.push_back({ SceneProxyCommandKind::Add, handle, std::move(payload), ToProxyTransform(transform) });
                m_impl->lights.emplace(light, Impl::LightEntry { handle, transform, stamp });
                continue;
            }

            const auto transform = BuildTransformStamp(*owner, &found->second.transform);
            if (found->second.stamp.PayloadEquals(stamp))
            {
                if (!(found->second.transform == transform))
                {
                    batch.commands.push_back({ SceneProxyCommandKind::UpdateTransform, found->second.handle, {}, ToProxyTransform(transform) });
                    found->second.transform = transform;
                }
                continue;
            }

            SceneProxyPayload payload;
            payload.kind = SceneProxyKind::Light;
            payload.ownerInstanceId = stamp.ownerInstanceId;
            payload.parentInstanceId = stamp.parentInstanceId;
            payload.ownerAlive = stamp.ownerAlive;
            payload.ownerActive = stamp.ownerActive;
            payload.light = SceneProxyLightData {
                stamp.type,
                stamp.color,
                stamp.intensity,
                stamp.range,
                stamp.cutoff,
                stamp.outerCutoff,
                stamp.size };
            batch.commands.push_back({ SceneProxyCommandKind::UpdatePayload, found->second.handle, std::move(payload), ToProxyTransform(transform) });
            found->second.stamp = stamp;
            found->second.transform = transform;
        }

        for (auto* skybox : fastAccess.skyboxs)
        {
            if (skybox == nullptr)
                continue;

            liveSkyBoxes.insert(skybox);
            auto* owner = skybox->gameobject();
            if (owner == nullptr || !owner->IsAlive())
            {
                const auto found = m_impl->skyboxes.find(skybox);
                if (found != m_impl->skyboxes.end())
                {
                    batch.commands.push_back({ SceneProxyCommandKind::Remove, found->second.handle });
                    m_impl->skyboxes.erase(found);
                }
                continue;
            }

            SkyBoxStamp stamp;
            stamp.ownerInstanceId = static_cast<uint64_t>(owner->GetInstanceID());
            stamp.mesh = skybox->GetMesh();
            stamp.material = skybox->GetMaterial();
            stamp.ownerAlive = owner->IsAlive();
            stamp.ownerActive = owner->IsActive();

            const auto found = m_impl->skyboxes.find(skybox);
            if (found == m_impl->skyboxes.end())
            {
                const auto handle = m_impl->AllocateHandle();
                const auto ownerToken = makeOwnerToken(handle);
                SceneProxyPayload payload;
                payload.kind = SceneProxyKind::SkyBox;
                payload.ownerInstanceId = stamp.ownerInstanceId;
                payload.ownerAlive = stamp.ownerAlive;
                payload.ownerActive = stamp.ownerActive;
                payload.resources = BuildSkyBoxResourceBinding(ownerToken, skybox->GetMesh(), skybox->GetMaterial());
                const auto transform = BuildTransformStamp(*owner);
                batch.commands.push_back({ SceneProxyCommandKind::Add, handle, std::move(payload), ToProxyTransform(transform) });
                m_impl->skyboxes.emplace(skybox, Impl::SkyBoxEntry { handle, transform, stamp });
                continue;
            }

            const auto transform = BuildTransformStamp(*owner, &found->second.transform);
            if (found->second.stamp.PayloadEquals(stamp))
            {
                if (!(found->second.transform == transform))
                {
                    batch.commands.push_back({ SceneProxyCommandKind::UpdateTransform, found->second.handle, {}, ToProxyTransform(transform) });
                    found->second.transform = transform;
                }
                continue;
            }

            const auto ownerToken = makeOwnerToken(found->second.handle);
            SceneProxyPayload payload;
            payload.kind = SceneProxyKind::SkyBox;
            payload.ownerInstanceId = stamp.ownerInstanceId;
            payload.ownerAlive = stamp.ownerAlive;
            payload.ownerActive = stamp.ownerActive;
            payload.resources = BuildSkyBoxResourceBinding(ownerToken, skybox->GetMesh(), skybox->GetMaterial());
            batch.commands.push_back({ SceneProxyCommandKind::UpdatePayload, found->second.handle, std::move(payload), ToProxyTransform(transform) });
            found->second.stamp = stamp;
            found->second.transform = transform;
        }

        for (auto iterator = m_impl->primitives.begin(); iterator != m_impl->primitives.end();)
        {
            if (livePrimitives.find(iterator->first) == livePrimitives.end())
            {
                batch.commands.push_back({ SceneProxyCommandKind::Remove, iterator->second.handle });
                iterator = m_impl->primitives.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
        for (auto iterator = m_impl->lights.begin(); iterator != m_impl->lights.end();)
        {
            if (liveLights.find(iterator->first) == liveLights.end())
            {
                batch.commands.push_back({ SceneProxyCommandKind::Remove, iterator->second.handle });
                iterator = m_impl->lights.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
        for (auto iterator = m_impl->skyboxes.begin(); iterator != m_impl->skyboxes.end();)
        {
            if (liveSkyBoxes.find(iterator->first) == liveSkyBoxes.end())
            {
                batch.commands.push_back({ SceneProxyCommandKind::Remove, iterator->second.handle });
                iterator = m_impl->skyboxes.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }

        m_impl->lastCollectionStamp = collectionStamp;
        m_impl->hasLastCollectionStamp = true;
        return batch;
    }

    size_t SceneProxySourceRegistry::GetTrackedPrimitiveProxyCount() const
    {
        return m_impl->primitives.size();
    }

    size_t SceneProxySourceRegistry::GetTrackedLightProxyCount() const
    {
        return m_impl->lights.size();
    }

    size_t SceneProxySourceRegistry::GetTrackedSkyBoxProxyCount() const
    {
        return m_impl->skyboxes.size();
    }

    uint64_t SceneProxySourceRegistry::GetCollectionScanCountForTesting() const
    {
        return m_impl->collectionScanCount;
    }
}

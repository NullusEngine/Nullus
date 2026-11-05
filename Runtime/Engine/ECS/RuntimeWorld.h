#pragma once
#include <ECS/Serialization/SceneStreaming.h>
#include "EngineDef.h"
#include <Base/Object/Object.h>
#include <ECS/Systems/SystemScheduler.h>
#include <array>

namespace NLS::Engine
{
class GameObject;
namespace Assets { class RuntimeAssetDatabase; struct RuntimeAssetRef; }
namespace SceneSystem { class Scene; }
namespace ECSIntegration
{
class ECSAssetLoader;
// World-space pose for the hybrid adapter. Its plain value representation is
// independent of the ABI and throwing copy constructors of legacy Math classes.
struct WorldPose
{
    std::array<float, 3> position{0, 0, 0};
    std::array<float, 4> rotation{0, 0, 0, 1};
    std::array<float, 3> scale{1, 1, 1};
    bool operator==(const WorldPose&) const = default;
};
enum class TransformAuthority : uint8_t { None, GameObject, Entity };
struct HybridLinkOptions
{
    TransformAuthority authority = TransformAuthority::None;
    bool destroyEntityWithObject = true;
    // Opt in only for a presentation object owned by this entity. Traditional
    // authoring/player/camera objects retain their Scene ownership by default.
    bool destroyObjectWithEntity = false;
    std::string authoringId;
};
struct HybridLink
{
    ECS::Entity entity;
    InstanceID object = InstanceID_None;
    uint64_t scene = 0;
    HybridLinkOptions options;
};
class NLS_ENGINE_API RuntimeWorld final
{
public:
    explicit RuntimeWorld(ECS::WorldConfig config = {});
    ~RuntimeWorld();
    RuntimeWorld(const RuntimeWorld&) = delete;
    RuntimeWorld& operator=(const RuntimeWorld&) = delete;
    ECS::World& GetWorld() { return m_world; }
    const ECS::World& GetWorld() const { return m_world; }
    ECS::SceneStreaming& Streaming() { return m_streaming; }
    void ConfigureAssets(std::shared_ptr<const Assets::RuntimeAssetDatabase> database, std::filesystem::path contentRoot);
    void ConfigureAssets(std::shared_ptr<const ECSAssetLoader> loader);
    ECS::SceneLoadHandle RequestSceneAsset(uint64_t chunk, const Assets::RuntimeAssetRef& asset,
        ECS::ArchiveLoadOptions options = {});
    ECS::ArchiveInstance InstantiatePrefabAsset(const Assets::RuntimeAssetRef& asset,
        std::span<const ECS::PrefabOverride> overrides = {}, uint64_t sceneOwner = 0, ECS::ArchiveLimits limits = {});
    void AttachScene(SceneSystem::Scene& scene);
    void Link(ECS::Entity entity, GameObject& object, HybridLinkOptions options = {});
    bool Unlink(ECS::Entity entity);
    GameObject* ResolveGameObject(ECS::Entity entity) const;
    ECS::Entity FindEntity(InstanceID object) const;
    ECS::Entity FindAuthoring(std::string_view persistentId) const;
    std::vector<HybridLink> GetLinks() const;
    void Prune();
    void SyncFromGameObjects();
    void SyncToGameObjects();
    void RunPhase(ECS::SystemPhase phase, const ECS::SystemTime& time);
    void Stop();
    static WorldPose Capture(const GameObject& object);
private:
    void CheckThread() const;
    static GameObject* Resolve(InstanceID object);
    ECS::World m_world;
    ECS::SceneStreaming m_streaming;
    std::shared_ptr<const ECSAssetLoader> m_assets;
    std::thread::id m_owner;
    uint64_t m_activeScene = 0;
    uint64_t m_destroyListener = 0;
    std::unordered_map<ECS::Entity, HybridLink, ECS::EntityHash> m_links;
    std::unordered_map<InstanceID, ECS::Entity> m_objects;
};
}
}

#include "RuntimeWorld.h"
#include "ECSAssets.h"
#include <Engine/GameObject.h>
#include <Engine/SceneSystem/Scene.h>
#include <Engine/Components/TransformComponent.h>
#include <Engine/Components/RigidbodyComponent.h>
#include <Engine/Components/Rigidbody2DComponent.h>
#include <cmath>

namespace NLS::Engine::ECSIntegration
{
namespace
{
uint64_t SceneKey(const SceneSystem::Scene* scene)
{ return scene ? static_cast<uint32_t>(scene->GetInstanceID()) : 0; }
void RequireEntityAuthority(const GameObject& object)
{
    if (const auto* body = object.GetComponent<Components::RigidbodyComponent>(); body && !body->IsKinematic())
        throw std::invalid_argument("Dynamic Rigidbody owns its pose; select GameObject transform authority for the ECS link");
    if (const auto* body = object.GetComponent<Components::Rigidbody2DComponent>();
        body && body->GetMotionType() == Components::Rigidbody2DComponent::EMotionType::DYNAMIC)
        throw std::invalid_argument("Dynamic Rigidbody2D owns its pose; select GameObject transform authority for the ECS link");
}
void ValidatePose(const WorldPose& pose)
{
    for (const auto value : pose.position) if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite ECS bridge position");
    for (const auto value : pose.scale) if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite ECS bridge scale");
    double length = 0;
    for (const auto value : pose.rotation)
    { if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite ECS bridge rotation"); length += double(value) * value; }
    if (length < 1e-12) throw std::invalid_argument("Zero ECS bridge quaternion");
}
}
RuntimeWorld::RuntimeWorld(ECS::WorldConfig config) : m_world(std::move(config)), m_streaming(m_world), m_owner(std::this_thread::get_id())
{
    m_world.RegisterComponent<WorldPose>("Nullus.ECS.WorldPose");
    m_destroyListener = GameObject::DestroyedEvent.AddListener([this](GameObject& object)
    {
        // Only mark the link here: object teardown can occur during script
        // callbacks. Structural ECS cleanup waits for the next safe point.
        const auto found = m_objects.find(object.GetInstanceID());
        if (found == m_objects.end()) return;
        if (auto link = m_links.find(found->second); link != m_links.end()) link->second.object = InstanceID_None;
        m_objects.erase(found);
    });
}
RuntimeWorld::~RuntimeWorld()
{
    Stop();
    GameObject::DestroyedEvent.RemoveListener(m_destroyListener);
}
void RuntimeWorld::CheckThread() const
{ if (std::this_thread::get_id() != m_owner) throw std::logic_error("GameObject/ECS bridges run on their owning main thread"); }
GameObject* RuntimeWorld::Resolve(InstanceID object)
{
    auto* value = dynamic_cast<GameObject*>(Object::IDToPointerNoThreadCheck(object));
    return value && value->IsAlive() ? value : nullptr;
}
WorldPose RuntimeWorld::Capture(const GameObject& object)
{
    const auto* transform = object.GetTransform();
    const auto& position = transform->GetWorldPosition(); const auto& rotation = transform->GetWorldRotation();
    const auto& scale = transform->GetWorldScale();
    return {{position.x, position.y, position.z}, {rotation.x, rotation.y, rotation.z, rotation.w}, {scale.x, scale.y, scale.z}};
}
void RuntimeWorld::AttachScene(SceneSystem::Scene& scene)
{
    CheckThread(); const auto key = SceneKey(&scene);
    if (key == m_activeScene) return;
    if (m_activeScene) { m_streaming.UnloadAll(); m_world.UnloadScene(m_activeScene); }
    m_activeScene = key; Prune();
}
void RuntimeWorld::ConfigureAssets(std::shared_ptr<const Assets::RuntimeAssetDatabase> database, std::filesystem::path contentRoot)
{ ConfigureAssets(std::make_shared<ECSAssetLoader>(std::move(database), std::move(contentRoot))); }
void RuntimeWorld::ConfigureAssets(std::shared_ptr<const ECSAssetLoader> loader)
{ CheckThread(); (void)m_world.GetChangeToken(); m_assets = std::move(loader); }
ECS::SceneLoadHandle RuntimeWorld::RequestSceneAsset(uint64_t chunk, const Assets::RuntimeAssetRef& asset, ECS::ArchiveLoadOptions options)
{
    CheckThread();
    if (!m_assets) throw std::logic_error("Configure the runtime asset database before loading ECS asset scenes");
    if (!options.resolveResource) options.resolveResource = m_assets->MakeLoadOptions().resolveResource;
    return m_streaming.RequestLoadSource(chunk, m_assets->CreateSource(asset), std::move(options));
}
ECS::ArchiveInstance RuntimeWorld::InstantiatePrefabAsset(const Assets::RuntimeAssetRef& asset,
    std::span<const ECS::PrefabOverride> overrides, uint64_t sceneOwner, ECS::ArchiveLimits limits)
{
    CheckThread(); (void)m_world.GetChangeToken();
    if (!m_assets) throw std::logic_error("Configure the runtime asset database before loading ECS asset prefabs");
    return m_assets->InstantiatePrefab(m_world, asset, overrides, sceneOwner, limits);
}
void RuntimeWorld::Link(ECS::Entity entity, GameObject& object, HybridLinkOptions options)
{
    CheckThread();
    if (!m_world.IsAlive(entity) || !object.IsAlive() || !object.GetScene()) throw std::invalid_argument("ECS link requires a live entity and scene-owned GameObject");
    if (options.authority == TransformAuthority::Entity) RequireEntityAuthority(object);
    const auto objectId = object.GetInstanceID();
    const auto existing = m_objects.find(objectId);
    if (existing != m_objects.end() && existing->second != entity) throw std::invalid_argument("GameObject already has an ECS transform link");
    if (!options.authoringId.empty())
        for (const auto& [key, link] : m_links)
            if (key != entity && link.options.authoringId == options.authoringId) throw std::invalid_argument("Duplicate ECS authoring identity");
    if (options.authority != TransformAuthority::None)
    {
        if (!m_world.HasComponent<WorldPose>(entity)) m_world.AddComponent<WorldPose>(entity, Capture(object));
        else if (options.authority == TransformAuthority::GameObject) m_world.SetComponent<WorldPose>(entity, Capture(object));
    }
    const auto scene = SceneKey(object.GetScene()); m_world.SetScene(entity, scene);
    // Reserve bucket storage before replacing existing bindings.
    m_objects.reserve(m_objects.size() + 1); m_links.reserve(m_links.size() + 1);
    if (auto old = m_links.find(entity); old != m_links.end()) m_objects.erase(old->second.object);
    m_links.insert_or_assign(entity, HybridLink{entity, objectId, scene, std::move(options)});
    m_objects.insert_or_assign(objectId, entity);
}
bool RuntimeWorld::Unlink(ECS::Entity entity)
{
    CheckThread(); const auto found = m_links.find(entity);
    if (found == m_links.end()) return false;
    m_objects.erase(found->second.object); m_links.erase(found); return true;
}
GameObject* RuntimeWorld::ResolveGameObject(ECS::Entity entity) const
{
    CheckThread(); if (!m_world.IsAlive(entity)) return nullptr;
    const auto found = m_links.find(entity); return found == m_links.end() ? nullptr : Resolve(found->second.object);
}
ECS::Entity RuntimeWorld::FindEntity(InstanceID object) const
{
    CheckThread(); const auto found = m_objects.find(object);
    return found != m_objects.end() && m_world.IsAlive(found->second) && Resolve(object) ? found->second : ECS::Entity{};
}
ECS::Entity RuntimeWorld::FindAuthoring(std::string_view persistentId) const
{
    CheckThread();
    if (!persistentId.empty()) for (const auto& [entity, link] : m_links)
        if (link.options.authoringId == persistentId && m_world.IsAlive(entity)) return entity;
    return {};
}
std::vector<HybridLink> RuntimeWorld::GetLinks() const
{
    CheckThread(); std::vector<HybridLink> result; result.reserve(m_links.size());
    for (const auto& [entity, link] : m_links) result.push_back(link);
    return result;
}
void RuntimeWorld::Prune()
{
    CheckThread();
    for (auto iterator = m_links.begin(); iterator != m_links.end();)
    {
        auto& link = iterator->second;
        const bool alive = m_world.IsAlive(link.entity);
        auto* object = Resolve(link.object);
        if (object && SceneKey(object->GetScene()) != link.scene) object = nullptr;
        if (alive && object) { ++iterator; continue; }
        if (alive && link.options.destroyEntityWithObject) m_world.DestroyEntity(link.entity);
        if (!alive && object && link.options.destroyObjectWithEntity) object->GetScene()->DestroyGameObject(*object);
        m_objects.erase(link.object); iterator = m_links.erase(iterator);
    }
}
void RuntimeWorld::SyncFromGameObjects()
{
    CheckThread(); Prune();
    for (const auto& [entity, link] : m_links)
        if (link.options.authority == TransformAuthority::GameObject)
            if (auto* object = Resolve(link.object))
            {
                const auto pose = Capture(*object);
                if (m_world.ReadComponent<WorldPose>(entity) != pose) m_world.SetComponent<WorldPose>(entity, pose);
            }
}
void RuntimeWorld::SyncToGameObjects()
{
    CheckThread(); Prune();
    for (const auto& [entity, link] : m_links)
        if (link.options.authority == TransformAuthority::Entity)
            if (auto* object = Resolve(link.object))
            {
                RequireEntityAuthority(*object);
                const auto& pose = m_world.ReadComponent<WorldPose>(entity); ValidatePose(pose);
                if (Capture(*object) == pose) continue;
                auto* transform = object->GetTransform();
                transform->SetWorldScale({pose.scale[0], pose.scale[1], pose.scale[2]});
                transform->SetWorldRotation({pose.rotation[0], pose.rotation[1], pose.rotation[2], pose.rotation[3]});
                transform->SetWorldPosition({pose.position[0], pose.position[1], pose.position[2]});
            }
}
void RuntimeWorld::RunPhase(ECS::SystemPhase phase, const ECS::SystemTime& time)
{
    CheckThread(); Prune();
    if (phase == ECS::SystemPhase::Initialization || phase == ECS::SystemPhase::PreFixedUpdate || phase == ECS::SystemPhase::Update)
        m_streaming.Pump();
    if (phase == ECS::SystemPhase::Initialization || phase == ECS::SystemPhase::PreFixedUpdate ||
        phase == ECS::SystemPhase::FixedSimulation || phase == ECS::SystemPhase::PostPhysics || phase == ECS::SystemPhase::Update)
        SyncFromGameObjects();
    m_world.Systems().RunPhase(phase, time);
    Prune();
    if (phase == ECS::SystemPhase::Initialization || phase == ECS::SystemPhase::FixedSimulation ||
        phase == ECS::SystemPhase::LateUpdate || phase == ECS::SystemPhase::Presentation) SyncToGameObjects();
}
void RuntimeWorld::Stop()
{
    CheckThread(); if (m_world.IsClosed()) return;
    m_world.Systems().Stop();
    m_streaming.UnloadAll();
    for (const auto& [entity, link] : m_links)
        if (link.options.destroyObjectWithEntity && m_world.IsAlive(entity)) m_world.DestroyEntity(entity);
    Prune(); m_world.Close(); m_links.clear(); m_objects.clear();
}
}

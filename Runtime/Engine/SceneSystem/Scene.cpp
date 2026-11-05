#include <Profiling/Profiler.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <type_traits>
#include <functional>
#include <string>
#include <stdexcept>
#include <unordered_set>

#include <Debug/Logger.h>
#include <Physics/PhysicsWorld.h>
#include <Physics/PhysicsCommandBuffer.h>
#include "Assets/PhysicsAssetReadScope.h"
#include "SceneSystem/Scene.h"
#include "Rendering/SceneRenderStoreRegistry.h"
#include "GameObject.h"
#include "Profiling/PerformanceStageStats.h"
#include "Serialize/PPtr.h"
using namespace NLS;
using namespace NLS::Engine::SceneSystem;

namespace
{
    using NLS::Engine::GameObject;
    namespace Components = NLS::Engine::Components;
    void ReleaseComponentPhysics(Components::Component& component,Scene& scene)
    {
        // Query geometry can be recreated after RuntimeDestroy (Stop/editing).
        // Always route final cleanup through this scene before losing ownership,
        // independently of whether gameplay OnDestroy was already delivered.
        if(auto* body=dynamic_cast<Components::RigidbodyComponent*>(&component)) body->ReleaseToScene(scene);
        else if(auto* body=dynamic_cast<Components::Rigidbody2DComponent*>(&component)) body->ReleaseToScene(scene);
        else if(auto* collider=dynamic_cast<Components::PhysicsColliderComponent*>(&component)) collider->ReleaseToScene(scene);
        else if(auto* collider=dynamic_cast<Components::PhysicsCollider2DComponent*>(&component)) collider->ReleaseToScene(scene);
    }
    void ReleaseObjectPhysics(GameObject& object,Scene& scene)
    {
        // A normal live component still cleans up in its original callback order.
        // Only resources recreated after an earlier OnDestroy need this bridge.
        for(const auto& component:object.GetComponents())
            if(component && component->HasRuntimeDestroyed()) ReleaseComponentPhysics(*component,scene);
    }
    template<class World>
    Physics::PhysicsCommandBuffer& GetPhysicsCommands(std::vector<std::unique_ptr<Physics::PhysicsCommandBuffer>>& buffers, const std::shared_ptr<World>& world)
    {
        auto found=std::find_if(buffers.begin(),buffers.end(),[&](const auto& buffer) { return buffer->Matches(world); });
        if (found==buffers.end())
        {
            buffers.push_back(std::make_unique<Physics::PhysicsCommandBuffer>(world));
            found=std::prev(buffers.end());
        }
        return **found;
    }
    template<class World, class Handle>
    bool EnqueuePhysicsRemoval(std::vector<std::unique_ptr<Physics::PhysicsCommandBuffer>>& buffers, const std::shared_ptr<World>& world, Handle handle)
    {
        if (!world || !handle.IsValid()) return false;
        auto& commands=GetPhysicsCommands(buffers,world);
        if constexpr (std::is_same_v<Handle,Physics::BodyHandle> || std::is_same_v<Handle,Physics::BodyHandle2D>) commands.RemoveBody(handle);
        else commands.RemoveShape(handle);
        return true;
    }
	template<class Visitor>
	void VisitLiveActors(Scene& scene, Visitor&& visitor)
	{
		std::vector<NLS::InstanceID> snapshot;
		for (auto* actor : scene.GetGameObjects())
			if (actor) snapshot.push_back(actor->GetInstanceID());
		for (const auto id : snapshot)
			if (auto* actor = dynamic_cast<NLS::Engine::GameObject*>(NLS::Object::IDToPointerNoThreadCheck(id));
				actor && actor->GetScene() == &scene && actor->IsAlive()) visitor(*actor);
	}
	template<typename ComponentType>
	void CollectExactComponents(
		NLS::Engine::GameObject& gameObject,
		std::vector<ComponentType*>& output)
	{
		const auto requestedType = NLS_TYPEOF(ComponentType);
		for (const auto& component : gameObject.GetComponents())
		{
			if (!component || component->GetType() != requestedType)
				continue;

			output.push_back(static_cast<ComponentType*>(component.get()));
		}
		}

    template<class Visitor>
    void VisitPhysicsControllers(Scene& scene, Visitor&& visitor)
    {
        struct Controller
        {
            Components::RigidbodyComponent* body3D = nullptr;
            Components::Rigidbody2DComponent* body2D = nullptr;
            size_t depth = 0;
        };
        const auto& components = scene.GetFastAccessComponents();
        std::vector<Controller> controllers;
        controllers.reserve(components.rigidbodies.size() + components.rigidbodies2D.size());
        const auto depthOf = [](const GameObject* owner)
        {
            size_t depth = 0;
            for (; owner; owner = owner->GetParent()) ++depth;
            return depth;
        };
        for (auto* body : components.rigidbodies) if (body) controllers.push_back({body, nullptr, depthOf(body->gameobject())});
        for (auto* body : components.rigidbodies2D) if (body) controllers.push_back({nullptr, body, depthOf(body->gameobject())});
        std::stable_sort(controllers.begin(), controllers.end(), [](const auto& a, const auto& b) { return a.depth < b.depth; });
        // Parent-first world-pose writeback also covers bodies created before
        // their parents and hierarchies mixing the two physics dimensions.
        for (const auto& controller : controllers)
        {
            if (controller.body3D) visitor(*controller.body3D);
            else visitor(*controller.body2D);
        }
    }

}

class Scene::ScopedFastAccessRebuildDeferral
{
public:
	explicit ScopedFastAccessRebuildDeferral(Scene& scene)
		: m_scene(scene)
	{
		m_scene.BeginFastAccessRebuildDeferral();
	}

	~ScopedFastAccessRebuildDeferral()
	{
		m_scene.EndFastAccessRebuildDeferral();
	}

private:
	Scene& m_scene;
};

Scene::Scene()
{

}

Scene::~Scene()
{
    NLS::Engine::Serialize::PersistentManager::Instance().ClearObjectGraphIds(*this);
    m_isStopping = true;
    std::unordered_set<GameObject*> notifiedActors;
    for (auto* actor : m_gameobject)
    {
        if (actor)
            NotifyGameObjectDestroyed(*actor, notifiedActors);
    }

    for (auto* actor : m_gameobject)
    {
        if (!actor || !actor->HasParent())
            continue;

        actor->DetachFromParent();
    }

	std::for_each(m_gameobject.begin(), m_gameobject.end(), [this](GameObject* element)
    {
        if(element) ReleaseObjectPhysics(*element,*this);
        delete element;
    });

	m_gameobject.clear();
    CommitPhysicsCommands();
    m_fastAccessComponents = {};
    m_fastAccessComponentsValid = false;
    NLS::Engine::Rendering::SceneRenderStoreRegistry::Release(this);
}

void Scene::Play()
{
	if (m_isPlaying)
		return;
	m_isPlaying = true;
	m_physicsInterpolationAlpha = 1;
	MarkRenderContentChanged();

	/* Wake up actors to allow them to react to OnEnable, OnDisable and OnDestroy, */
	VisitLiveActors(*this, [](GameObject& actor) { actor.SetSleeping(false); });
	VisitLiveActors(*this, [](GameObject& actor) { if (actor.IsActive()) actor.OnAwake(); });
	VisitLiveActors(*this, [](GameObject& actor) { if (actor.IsActive()) actor.OnEnable(); });
	// Start is delivered immediately before the first simulation update.
}

void Scene::Stop()
{
	if (!m_isPlaying)
		return;
    if (m_isStopping) return;
    if (m_physicsMutationDeferralDepth || m_synchronizingPhysics)
    { m_physicsStopPending=true; return; }
    m_physicsStopPending=false;
	m_isStopping = true;
    {
        const PhysicsMutationDeferral deferral(*this);
        VisitLiveActors(*this, [](GameObject& actor) { actor.OnDisable(); });
        VisitLiveActors(*this, [](GameObject& actor) { actor.OnDestroy(); });
        VisitLiveActors(*this, [](GameObject& actor) { actor.SetSleeping(true); });
    }
    const auto& physics=GetFastAccessComponents();
    for (auto* body:physics.rigidbodies) if (body) body->ResetSimulation();
    for (auto* body:physics.rigidbodies2D) if (body) body->ResetSimulation();
    for (auto* collider:physics.physicsColliders) if (collider) collider->Release();
    for (auto* collider:physics.physicsColliders2D) if (collider) collider->Release();
    CommitPhysicsCommands();
	m_isPlaying = false;
	m_physicsInterpolationAlpha = 1;
	m_isStopping = false;
    m_physicsWorld2D.reset();
    m_physicsWorld2DUnavailable = false;
    m_physicsWorld.reset();
    m_physicsWorldUnavailable = false;
	MarkRenderContentChanged();
}

bool Scene::IsPlaying() const
{
	return m_isPlaying;
}

void Scene::Update(float p_deltaTime)
{
    UpdateGameplay(p_deltaTime);
    UpdateAnimations(p_deltaTime);
}

void Scene::UpdateGameplay(float p_deltaTime)
{
    NLS_PROFILE_NAMED_SCOPE("Scene::UpdateGameplay");
    const bool wasPlaying=m_isPlaying;
	SynchronizePhysics();
    if (wasPlaying && !m_isPlaying) return;
	VisitLiveActors(*this, [p_deltaTime](GameObject& actor) { actor.OnUpdate(p_deltaTime); });
}

void Scene::UpdateAnimations(float p_deltaTime)
{
    NLS_PROFILE_NAMED_SCOPE("Scene::UpdateAnimations");
    const auto& animation=GetFastAccessComponents();
    for(auto* animator:animation.animators)if(animator&&animator->IsSelfEnabled()&&animator->gameobject()->IsActive())animator->Evaluate(p_deltaTime);
    for(auto* mesh:animation.skinnedMeshes)if(mesh&&mesh->IsSelfEnabled()&&mesh->gameobject()->IsActive())mesh->PublishPose();
    for(auto* animator:animation.animators)if(animator&&animator->IsSelfEnabled()&&animator->gameobject()->IsActive())animator->Finalize();
}

void Scene::FixedUpdate(float p_deltaTime)
{
    NLS_PROFILE_NAMED_SCOPE("Scene::FixedUpdate");
    if (m_physicsMutationDeferralDepth) return;
    const bool wasPlaying=m_isPlaying;
	SynchronizePhysics();
    if (wasPlaying && !m_isPlaying) return;
    for (auto* body : GetFastAccessComponents().rigidbodies) if (body) body->ApplyPendingCommands();
    for (auto* body : GetFastAccessComponents().rigidbodies2D) if (body) body->ApplyPendingCommands();
	VisitLiveActors(*this, [p_deltaTime](GameObject& actor) { actor.OnFixedUpdate(p_deltaTime); });
}

void Scene::LateUpdate(float p_deltaTime)
{
    NLS_PROFILE_NAMED_SCOPE("Scene::LateUpdate");
	VisitLiveActors(*this, [p_deltaTime](GameObject& actor) { actor.OnLateUpdate(p_deltaTime); });
}

std::vector<NLS::Physics::ContactEvent> Scene::SimulatePhysics(float delta)
{
    NLS_PROFILE_NAMED_SCOPE("Scene::SimulatePhysics");
    if (!m_isPlaying || m_physicsMutationDeferralDepth || m_synchronizingPhysics) return {};
    SynchronizePhysics();
    if (!m_isPlaying) return {};
    std::vector<NLS::Physics::ContactEvent> events;
    if (const auto world = PeekPhysicsWorld())
    {
        const auto bodies = GetFastAccessComponents().rigidbodies;
        for (auto* body : bodies) if (body) body->ApplyPendingCommands();
        world->Step(delta);
        events = world->TakeContactEvents();
    }
    if (const auto world = PeekPhysicsWorld2D())
    {
        const auto bodies = GetFastAccessComponents().rigidbodies2D;
        for (auto* body : bodies) if (body) body->ApplyPendingCommands();
        world->Step(delta);
    }
    VisitPhysicsControllers(*this, [](auto& body) { body.ReadSimulationPose(); });
    {
        const PhysicsMutationDeferral deferral(*this);
        for (const auto& event : events) PhysicsContactEvent.Invoke(event);
    }
    return events;
}

Engine::GameObject& Scene::CreateGameObject()
{
	return CreateGameObject("New GameObject");
}

Engine::GameObject& Scene::CreateGameObject(const std::string& p_name, const std::string& p_tag)
{
	if (m_isStopping) throw std::logic_error("Cannot create a GameObject while its Play scene is stopping.");
	GameObject* newGameObject = new Engine::GameObject(p_name, p_tag);

	AddGameObject(newGameObject);

	return *newGameObject;
}

Engine::GameObject& Scene::CreateEditorTransientGameObject(const std::string& p_name, const std::string& p_tag)
{
	GameObject* newGameObject = new Engine::GameObject(Engine::GameObject::SilentCreationTag {}, p_name, p_tag);
	newGameObject->SetEditorTransient(true);

	AddGameObject(newGameObject);

	return *newGameObject;
}

bool Scene::AddGameObject(GameObject* gameObject, AddGameObjectActivation activation)
{
	if (!gameObject || m_isStopping)
		return false;

	GameObject& instance = *gameObject;
    size_t sceneObjectCount = 0u;
    size_t sceneComponentCount = 0u;
    size_t hashTrackedLookupCount = 0u;
    ScopedFastAccessRebuildDeferral rebuildDeferral(*this);

	auto AddComponents = [this, &sceneComponentCount](GameObject* go)
		{
			if (!go)
				return;

            NLS::Base::Profiling::PerformanceStageScope componentScope(
                NLS::Base::Profiling::PerformanceStageDomain::Prefab,
                "SceneRegisterComponents",
                NLS::Base::Profiling::PerformanceStageThread::Main);
			for (auto&& component : go->GetComponents())
			{
				OnComponentAdded(component.get());
                ++sceneComponentCount;
			}
            componentScope.AddCounter("componentCount", go->GetComponents().size());
			go->ComponentAddedEvent += std::bind(&Scene::OnComponentAdded, this, std::placeholders::_1);
			go->ComponentRemovedEvent += std::bind(&Scene::OnComponentRemoved, this, std::placeholders::_1);
		};

    {
        NLS::Base::Profiling::PerformanceStageScope addScope(
            NLS::Base::Profiling::PerformanceStageDomain::Prefab,
            "SceneAddGameObjects",
            NLS::Base::Profiling::PerformanceStageThread::Main);
        std::unordered_set<GameObject*> trackedObjects;
        trackedObjects.reserve(m_gameobject.size() + 16u);
        for (auto* tracked : m_gameobject)
            trackedObjects.insert(tracked);

	    std::function<void(GameObject*)> AddGameObjectRecursively =
            [this,
             &AddComponents,
             &AddGameObjectRecursively,
             &trackedObjects,
             &sceneObjectCount,
             &hashTrackedLookupCount](GameObject* go)
		    {
			    if (!go)
				    return;

                go->SetScene(this);

                ++hashTrackedLookupCount;
			    if (trackedObjects.insert(go).second)
			    {
				    m_gameobject.push_back(go);
                    ++sceneObjectCount;
				    AddComponents(go);
			    }
			    for (auto&& child : go->GetChildren())
			    {
				    AddGameObjectRecursively(child);
			    }
		    };
        AddGameObjectRecursively(&instance);
        addScope.AddCounter("objectCount", sceneObjectCount);
        addScope.AddCounter("hashTrackedLookupCount", hashTrackedLookupCount);
        addScope.AddCounter("linearTrackedLookupCount", 0u);
    }

    if (activation == AddGameObjectActivation::Immediate)
        ActivateGameObjectForPlay(&instance);
	return true;
}

bool Scene::AddGameObjects(
    const std::vector<GameObject*>& gameObjects,
    AddGameObjectActivation activation)
{
    ScopedFastAccessRebuildDeferral rebuildDeferral(*this);
    bool added = true;
    for (auto* gameObject : gameObjects)
        added = AddGameObject(gameObject, activation) && added;
    return added;
}

size_t Scene::ActivateGameObjectForPlay(GameObject* gameObject)
{
    if (!gameObject)
        return 0u;

    if (!m_isPlaying)
        return 0u;

    size_t activatedObjectCount = 0u;
    const auto wasAwaked = gameObject->HasAwaked();
    const auto wasStarted = gameObject->HasStarted();
    if (!wasAwaked || !wasStarted)
    {
        gameObject->SetSleeping(false);
        if (gameObject->IsActive())
        {
            if (!wasAwaked)
                gameObject->OnAwake();
            gameObject->OnEnable();
            if (!wasStarted)
                // Start is deferred until the first simulation update.
            ++activatedObjectCount;
        }
    }

    for (auto* child : gameObject->GetChildren())
        activatedObjectCount += ActivateGameObjectForPlay(child);

    return activatedObjectCount;
}

bool Scene::DestroyGameObject(GameObject& p_target)
{
	const auto found = std::find_if(m_gameobject.begin(), m_gameobject.end(), [&p_target](Engine::GameObject* element)
	{
		return element == &p_target;
	});

	if (found == m_gameobject.end())
		return false;

    DestroyGameObjectSubtree(p_target);
	return true;
}

void Scene::CollectGarbages()
{
    NLS_PROFILE_NAMED_SCOPE("Scene::CollectGarbages");
    auto garbage = std::vector<GameObject*> {};
    auto collected = std::unordered_set<GameObject*> {};
    for (auto* element : m_gameobject)
    {
        if (element && !element->IsAlive())
            CollectGameObjectSubtree(*element, garbage, collected);
    }

    if (garbage.empty())
        return;

    RemoveGameObjectsFromSceneList(collected);
    DestroyCollectedGameObjects(garbage);
}

Engine::GameObject* Scene::FindGameObjectByName(const std::string& p_name) const
{
	auto result = std::find_if(m_gameobject.begin(), m_gameobject.end(), [p_name](Engine::GameObject* element)
	{ 
		return element->GetName() == p_name;
	});

	if (result != m_gameobject.end())
		return *result;
	else
		return nullptr;
}

Engine::GameObject* Scene::FindGameObjectByTag(const std::string & p_tag) const
{
	auto result = std::find_if(m_gameobject.begin(), m_gameobject.end(), [p_tag](Engine::GameObject* element)
	{
		return element->GetTag() == p_tag;
	});

	if (result != m_gameobject.end())
		return *result;
	else
		return nullptr;
}

std::vector<std::reference_wrapper<Engine::GameObject>> Scene::FindGameObjectsByName(const std::string & p_name) const
{
	std::vector<std::reference_wrapper<Engine::GameObject>> actors;

	for (auto actor : m_gameobject)
	{
		if (actor->GetName() == p_name)
			actors.push_back(std::ref(*actor));
	}

	return actors;
}

std::vector<std::reference_wrapper<Engine::GameObject>> Scene::FindGameObjectsByTag(const std::string & p_tag) const
{
	std::vector<std::reference_wrapper<Engine::GameObject>> actors;

	for (auto actor : m_gameobject)
	{
		if (actor->GetTag() == p_tag)
			actors.push_back(std::ref(*actor));
	}

	return actors;
}

Engine::Components::CameraComponent* Scene::FindMainCamera() const
{
	for (auto* camera : m_fastAccessComponents.cameras)
	{
		if (!camera)
			continue;

		auto* owner = camera->gameobject();
		if (owner && owner->IsActive())
		{
			return camera;
		}
	}

	for (auto* actor : m_gameobject)
	{
		if (actor && actor->IsActive())
		{
			if (auto* camera = actor->GetComponent<Engine::Components::CameraComponent>())
			{
				return camera;
			}
		}
	}

	return nullptr;
}

void Scene::OnComponentAdded(Components::Component* p_compononent)
{
    RequestFastAccessComponentsRebuild();
    MarkRenderContentChanged();
    ComponentAddedEvent.Invoke(p_compononent);
}

void Scene::OnComponentRemoved(Components::Component* p_compononent)
{
    if(p_compononent) ReleaseComponentPhysics(*p_compononent,*this);
    RequestFastAccessComponentsRebuild();
    MarkRenderContentChanged();
}

std::vector<Engine::GameObject*>& Scene::GetGameObjects()
{
	return m_gameobject;
}

const std::vector<Engine::GameObject*>& Scene::GetGameObjects() const
{
	return m_gameobject;
}

const Scene::FastAccessComponents& Scene::GetFastAccessComponents() const
{
	if (!m_fastAccessComponentsValid)
		const_cast<Scene*>(this)->RebuildFastAccessComponents();
	return m_fastAccessComponents;
}

uint64_t Scene::GetFastAccessComponentsRevision() const
{
	(void)GetFastAccessComponents();
	return m_fastAccessComponentsRevision;
}

uint64_t Scene::GetRenderContentRevision() const
{
	return m_renderContentRevision;
}

void Scene::MarkRenderContentChanged()
{
	++m_renderContentRevision;
}

const std::shared_ptr<NLS::Physics::PhysicsWorld>& Scene::GetPhysicsWorld()
{
	if (m_physicsWorld == nullptr && !m_physicsWorldUnavailable && !m_isStopping)
	{
		std::string error;
            NLS::Physics::WorldSettings settings;
            settings.gravity = m_physicsSettings.gravity3D;
            settings.workerCount = m_physicsSettings.workerCount3D;
			m_physicsWorld = NLS::Physics::PhysicsWorld::Create(settings, error);
            if (m_physicsWorld) m_physicsWorkerCount3D = settings.workerCount;
		if (m_physicsWorld == nullptr)
		{
			m_physicsWorldUnavailable = true;
			NLS_LOG_ERROR("Scene physics query world creation failed: " + error);
		}
	}
	return m_physicsWorld;
}

void Scene::NotifyPhysicsChanged()
{
	++m_physicsRevision;
}

void Scene::QueuePhysicsRemoval(const std::shared_ptr<Physics::PhysicsWorld>& world, Physics::BodyHandle body)
{
    if (!EnqueuePhysicsRemoval(m_physicsStructuralCommands,world,body)) return;
    NotifyPhysicsChanged();
    if (!m_isPlaying && !m_isStopping && !m_synchronizingPhysics && !m_physicsMutationDeferralDepth) CommitPhysicsCommands();
}
void Scene::QueuePhysicsRemoval(const std::shared_ptr<Physics::PhysicsWorld>& world, Physics::ShapeHandle shape)
{
    if (!EnqueuePhysicsRemoval(m_physicsStructuralCommands,world,shape)) return;
    NotifyPhysicsChanged();
    if (!m_isPlaying && !m_isStopping && !m_synchronizingPhysics && !m_physicsMutationDeferralDepth) CommitPhysicsCommands();
}
void Scene::QueuePhysicsRemoval(const std::shared_ptr<Physics::PhysicsWorld2D>& world, Physics::BodyHandle2D body)
{
    if (!EnqueuePhysicsRemoval(m_physicsStructuralCommands,world,body)) return;
    NotifyPhysicsChanged();
    if (!m_isPlaying && !m_isStopping && !m_synchronizingPhysics && !m_physicsMutationDeferralDepth) CommitPhysicsCommands();
}
void Scene::QueuePhysicsRemoval(const std::shared_ptr<Physics::PhysicsWorld2D>& world, Physics::ShapeHandle2D shape)
{
    if (!EnqueuePhysicsRemoval(m_physicsStructuralCommands,world,shape)) return;
    NotifyPhysicsChanged();
    if (!m_isPlaying && !m_isStopping && !m_synchronizingPhysics && !m_physicsMutationDeferralDepth) CommitPhysicsCommands();
}
size_t Scene::GetPendingPhysicsCommandCount() const
{
    size_t count=0;
    for (const auto& buffer:m_physicsStructuralCommands) count+=buffer->GetPendingCount();
    return count;
}
Scene::PhysicsMutationDeferral::PhysicsMutationDeferral(Scene& scene) : m_scene(scene) { ++m_scene.m_physicsMutationDeferralDepth; }
Scene::PhysicsMutationDeferral::~PhysicsMutationDeferral() { --m_scene.m_physicsMutationDeferralDepth; }
bool Scene::TrySyncTransforms()
{
    if (m_physicsMutationDeferralDepth || m_synchronizingPhysics) return false;
    SynchronizePhysics(); return true;
}
void Scene::CommitPhysicsCommands()
{
    for (const auto& buffer:m_physicsStructuralCommands)
    {
        const auto report=buffer->Commit();
        if (report.failed) NLS_LOG_ERROR("Scene physics structural command failed: " + std::to_string(report.failed));
        if (report.applied) NotifyPhysicsChanged();
    }
    m_physicsStructuralCommands.clear();
}

bool Scene::SetPhysicsSettings(const ScenePhysicsSettings& settings, std::string& error)
{
    if (!settings.Validate(error)) return false;
    if (m_isStopping) { error = "Cannot change physics settings while the scene is stopping."; return false; }
    if (settings == m_physicsSettings) { error.clear(); return true; }
    if (m_physicsWorld && settings.gravity3D!=m_physicsSettings.gravity3D)
        GetPhysicsCommands(m_physicsStructuralCommands,m_physicsWorld).SetGravity(settings.gravity3D);
    if (m_physicsWorld2D)
    {
        auto& commands=GetPhysicsCommands(m_physicsStructuralCommands,m_physicsWorld2D);
        if (settings.gravity2D!=m_physicsSettings.gravity2D) commands.SetGravity(settings.gravity2D);
        if (settings.solverSubsteps2D!=m_physicsSettings.solverSubsteps2D) commands.SetSolverSubsteps(settings.solverSubsteps2D);
    }
    const bool interpolationChanged = settings.interpolate != m_physicsSettings.interpolate;
    m_physicsSettings = settings;
    if (interpolationChanged) ResetPhysicsInterpolation();
    if (!m_physicsWorld) m_physicsWorldUnavailable = false;
    if (!m_physicsWorld2D) m_physicsWorld2DUnavailable = false;
    if (!m_physicsMutationDeferralDepth && !m_synchronizingPhysics) CommitPhysicsCommands();
    NotifyPhysicsChanged();
    error.clear(); return true;
}

bool Scene::HasPendingPhysicsWorkerSettings() const
{
    return (m_physicsWorld && m_physicsWorkerCount3D != m_physicsSettings.workerCount3D) ||
        (m_physicsWorld2D && m_physicsWorkerCount2D != m_physicsSettings.workerCount2D);
}

void Scene::SetPhysicsInterpolationAlpha(double alpha)
{
    alpha = std::isfinite(alpha) ? std::clamp(alpha, 0.0, 1.0) : 1.0;
    if (m_physicsInterpolationAlpha == alpha) return;
    m_physicsInterpolationAlpha = alpha;
    if (m_isPlaying && m_physicsSettings.interpolate) MarkRenderContentChanged();
}

void Scene::ResetPhysicsInterpolation()
{
    m_physicsInterpolationAlpha = 1;
    const auto& components = GetFastAccessComponents();
    for (auto* body : components.rigidbodies) if (body) body->ResetInterpolation();
    for (auto* body : components.rigidbodies2D) if (body) body->ResetInterpolation();
    MarkRenderContentChanged();
}

PhysicsPresentationTransform Scene::CapturePhysicsPresentationTransform(const GameObject& object) const
{
    NLS_PROFILE_NAMED_SCOPE("Scene::CapturePhysicsPresentationTransform");
    PhysicsPresentationTransform captured;
    const auto* transform = object.GetTransform();
    if (!transform) return captured;
    captured.worldMatrix = transform->GetWorldMatrix();
    captured.worldPosition = transform->GetWorldPosition();
    captured.worldRotation = transform->GetWorldRotation();
    captured.worldScale = transform->GetWorldScale();
    if (!m_isPlaying || !m_physicsSettings.interpolate || object.GetScene() != this) return captured;
    for (const auto* ancestor = &object; ancestor && ancestor->GetScene() == this; ancestor = ancestor->GetParent())
    {
        Physics::PhysicsPose authoring, presented;
        bool hasController = false, hasPose = false;
        for (const auto& component : ancestor->GetComponents())
        {
            if (auto* body = dynamic_cast<Components::RigidbodyComponent*>(component.get()); body && body->IsActiveAndEnabled())
            {
                hasController = true;
                hasPose = body->TryGetPresentationPose(m_physicsInterpolationAlpha, authoring, presented);
                break;
            }
            if (auto* body = dynamic_cast<Components::Rigidbody2DComponent*>(component.get()); body && body->IsActiveAndEnabled())
            {
                hasController = true;
                hasPose = body->TryGetPresentationPose(m_physicsInterpolationAlpha, authoring, presented);
                break;
            }
        }
        if (!hasController) continue;
        if (!hasPose) return captured;
        // A rigid world-space delta preserves the object's complete affine
        // transform, including descendant scale, without changing authoring data.
        const auto rotation = Maths::Quaternion::Normalize(presented.rotation) *
            Maths::Quaternion::Inverse(Maths::Quaternion::Normalize(authoring.rotation));
        const auto delta = Maths::Matrix4::Translation(presented.position) * Maths::Matrix4::Rotation(rotation) *
            Maths::Matrix4::Translation(-authoring.position);
        captured.worldMatrix = delta * captured.worldMatrix;
        const auto position = delta * Maths::Vector4(captured.worldPosition, 1);
        captured.worldPosition = {position.x, position.y, position.z};
        captured.worldRotation = Maths::Quaternion::Normalize(rotation * captured.worldRotation);
        captured.usesPhysicsPose = true;
        return captured;
    }
    return captured;
}

const std::shared_ptr<NLS::Physics::PhysicsWorld2D>& Scene::GetPhysicsWorld2D()
{
    if (!m_physicsWorld2D && !m_physicsWorld2DUnavailable && !m_isStopping)
    {
        std::string error;
        NLS::Physics::WorldSettings2D settings;
        settings.gravity = m_physicsSettings.gravity2D;
        settings.solverSubsteps = m_physicsSettings.solverSubsteps2D;
        settings.workerCount = m_physicsSettings.workerCount2D;
        m_physicsWorld2D = NLS::Physics::PhysicsWorld2D::Create(settings, error);
        if (m_physicsWorld2D) m_physicsWorkerCount2D = settings.workerCount;
        if (!m_physicsWorld2D)
        {
            m_physicsWorld2DUnavailable = true;
            NLS_LOG_ERROR("Scene 2D physics world creation failed: " + error);
        }
    }
    return m_physicsWorld2D;
}

// Runs before component updates so particle collision queries observe the
// current frame's collider poses. Only dirty or moved colliders touch PhysX.
void Scene::SynchronizePhysics()
{
    NLS_PROFILE_NAMED_SCOPE("Scene::SynchronizePhysics");
    const Assets::PhysicsAssetReadScope physicsAssetScope;
    if (m_synchronizingPhysics || m_physicsMutationDeferralDepth) return;
    if (m_physicsStopPending) { Stop(); return; }
    struct SynchronizationScope
    {
        bool& flag;
        explicit SynchronizationScope(bool& value) : flag(value) { flag=true; }
        ~SynchronizationScope() { flag=false; }
    } scope(m_synchronizingPhysics);
    CommitPhysicsCommands();
    const auto& components = GetFastAccessComponents();
    const auto world2D = components.rigidbodies2D.empty() && components.physicsColliders2D.empty() ? PeekPhysicsWorld2D() : GetPhysicsWorld2D();
    const auto batch2D = world2D ? world2D->BeginMutationBatch() : nullptr;
    const auto& colliders = components.physicsColliders;
    const auto& world = colliders.empty() && components.rigidbodies.empty() ? PeekPhysicsWorld() : GetPhysicsWorld();
    const auto batch = world ? world->BeginMutationBatch() : nullptr;
    VisitPhysicsControllers(*this, [](auto& body) { body.Synchronize(); });
    for (auto* collider : components.physicsColliders2D) if (collider) collider->Synchronize();
	for (auto* collider : colliders)
		if (collider != nullptr)
			collider->Synchronize();
    CommitPhysicsCommands();
    if (m_physicsSettings.interpolate)
    {
        for (auto* body : components.rigidbodies) if (body) body->RefreshInterpolation();
        for (auto* body : components.rigidbodies2D) if (body) body->RefreshInterpolation();
    }
}

void Scene::NotifyGameObjectDestroyed(GameObject& p_actor)
{
    std::unordered_set<GameObject*> notifiedActors;
    NotifyGameObjectDestroyed(p_actor, notifiedActors);
}

void Scene::NotifyGameObjectDestroyed(GameObject& p_actor, std::unordered_set<GameObject*>& p_notifiedActors)
{
    if (!p_notifiedActors.insert(&p_actor).second)
        return;

    const auto children = p_actor.GetChildren();
    for (auto* child : children)
    {
        if (child)
            NotifyGameObjectDestroyed(*child, p_notifiedActors);
    }

    GameObject::DestroyedEvent.Invoke(p_actor);
}

void Scene::CollectGameObjectSubtree(
    GameObject& p_actor,
    std::vector<GameObject*>& p_outGameObjects,
    std::unordered_set<GameObject*>& p_visitedGameObjects)
{
    if (!p_visitedGameObjects.insert(&p_actor).second)
        return;

    p_outGameObjects.push_back(&p_actor);
    const auto children = p_actor.GetChildren();
    for (auto* child : children)
    {
        if (child)
            CollectGameObjectSubtree(*child, p_outGameObjects, p_visitedGameObjects);
    }
}

void Scene::RemoveGameObjectsFromSceneList(const std::unordered_set<GameObject*>& p_gameObjects)
{
    m_gameobject.erase(std::remove_if(
        m_gameobject.begin(),
        m_gameobject.end(),
        [&p_gameObjects](GameObject* element)
        {
            return p_gameObjects.contains(element);
        }),
        m_gameobject.end());
}

void Scene::DestroyGameObjectSubtree(GameObject& p_actor)
{
    auto gameObjects = std::vector<GameObject*> {};
    auto collected = std::unordered_set<GameObject*> {};
    CollectGameObjectSubtree(p_actor, gameObjects, collected);
    RemoveGameObjectsFromSceneList(collected);
    DestroyCollectedGameObjects(gameObjects);
}

void Scene::DestroyCollectedGameObjects(std::vector<GameObject*>& p_gameObjects)
{
    std::unordered_set<GameObject*> notifiedActors;
    for (auto* gameObject : p_gameObjects)
    {
        if (gameObject)
            NotifyGameObjectDestroyed(*gameObject, notifiedActors);
    }

    for (auto it = p_gameObjects.rbegin(); it != p_gameObjects.rend(); ++it)
    {
        auto* gameObject = *it;
        if (!gameObject)
            continue;

        if (gameObject->HasParent())
            gameObject->DetachFromParent();

        const auto children = gameObject->GetChildren();
        for (auto* child : children)
        {
            if (child)
                child->DetachFromParent();
        }

        ReleaseObjectPhysics(*gameObject,*this);
        delete gameObject;
    }

    RebuildFastAccessComponents();
}

void Scene::RebuildFastAccessComponents()
{
    NLS::Base::Profiling::PerformanceStageScope rebuildScope(
        NLS::Base::Profiling::PerformanceStageDomain::Prefab,
        "SceneRebuildFastAccess",
        NLS::Base::Profiling::PerformanceStageThread::Main);
    rebuildScope.AddCounter("rebuildCount");
    rebuildScope.AddCounter("trackedObjectCount", m_gameobject.size());

	m_fastAccessComponents.modelRenderers.clear();
	m_fastAccessComponents.cameras.clear();
	m_fastAccessComponents.lights.clear();
	m_fastAccessComponents.skyboxs.clear();
	m_fastAccessComponents.uiCanvases.clear();
        m_fastAccessComponents.particleSystems.clear();
        m_fastAccessComponents.animators.clear();
        m_fastAccessComponents.skinnedMeshes.clear();
        m_fastAccessComponents.sprites.clear();
		m_fastAccessComponents.physicsColliders.clear();
    m_fastAccessComponents.rigidbodies.clear();
    m_fastAccessComponents.rigidbodies2D.clear();
    m_fastAccessComponents.physicsColliders2D.clear();

	std::unordered_set<GameObject*> visited;
	std::function<void(GameObject*)> collectComponents = [this, &visited, &collectComponents](GameObject* go)
	{
		if (!go)
			return;
		if (!visited.insert(go).second)
			return;

		CollectExactComponents(*go, m_fastAccessComponents.modelRenderers);
		CollectExactComponents(*go, m_fastAccessComponents.cameras);
		CollectExactComponents(*go, m_fastAccessComponents.lights);
		CollectExactComponents(*go, m_fastAccessComponents.skyboxs);
		CollectExactComponents(*go, m_fastAccessComponents.uiCanvases);
            CollectExactComponents(*go, m_fastAccessComponents.particleSystems);
            CollectExactComponents(*go, m_fastAccessComponents.animators);
            CollectExactComponents(*go, m_fastAccessComponents.skinnedMeshes);
            CollectExactComponents(*go, m_fastAccessComponents.sprites);
            for (const auto& component : go->GetComponents())
            {
                if (auto* collider = dynamic_cast<Components::PhysicsColliderComponent*>(component.get()))
                    m_fastAccessComponents.physicsColliders.push_back(collider);
                if (auto* body = dynamic_cast<Components::RigidbodyComponent*>(component.get()))
                    m_fastAccessComponents.rigidbodies.push_back(body);
                if (auto* body = dynamic_cast<Components::Rigidbody2DComponent*>(component.get()))
                    m_fastAccessComponents.rigidbodies2D.push_back(body);
                if (auto* collider = dynamic_cast<Components::PhysicsCollider2DComponent*>(component.get()))
                    m_fastAccessComponents.physicsColliders2D.push_back(collider);
            }

		for (auto* child : go->GetChildren())
			collectComponents(child);
	};

	for (auto* go : m_gameobject)
	{
		collectComponents(go);
	}
	m_fastAccessComponentsValid = true;
    const auto depth = [](const auto* body)
    {
        size_t count = 0;
        for (auto* owner = body->gameobject(); owner && owner->GetParent(); owner = owner->GetParent()) ++count;
        return count;
    };
    std::stable_sort(m_fastAccessComponents.rigidbodies2D.begin(), m_fastAccessComponents.rigidbodies2D.end(),
        [&](const auto* a, const auto* b) { return depth(a) < depth(b); });
    std::stable_sort(m_fastAccessComponents.rigidbodies.begin(), m_fastAccessComponents.rigidbodies.end(),
        [&](const auto* a, const auto* b) { return depth(a) < depth(b); });
	++m_fastAccessComponentsRevision;
	MarkRenderContentChanged();
}

void Scene::RequestFastAccessComponentsRebuild()
{
    m_fastAccessComponentsValid = false;
    MarkRenderContentChanged();

    if (m_fastAccessRebuildDeferralDepth > 0u)
    {
        m_fastAccessRebuildPending = true;
        return;
    }

    RebuildFastAccessComponents();
}

void Scene::BeginFastAccessRebuildDeferral()
{
    ++m_fastAccessRebuildDeferralDepth;
}

void Scene::EndFastAccessRebuildDeferral()
{
    if (m_fastAccessRebuildDeferralDepth == 0u)
        return;

    --m_fastAccessRebuildDeferralDepth;
    if (m_fastAccessRebuildDeferralDepth == 0u && m_fastAccessRebuildPending)
    {
        m_fastAccessRebuildPending = false;
        RebuildFastAccessComponents();
    }
}


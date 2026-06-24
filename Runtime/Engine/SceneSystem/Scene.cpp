
#include <algorithm>
#include <functional>
#include <string>
#include <unordered_set>

#include <Debug/Logger.h>
#include "SceneSystem/Scene.h"
#include "GameObject.h"
#include "Profiling/PerformanceStageStats.h"
using namespace NLS;
using namespace NLS::Engine::SceneSystem;

namespace
{
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

    std::for_each(m_gameobject.begin(), m_gameobject.end(), [](GameObject* element)
    {
        delete element;
    });

	m_gameobject.clear();
    m_fastAccessComponents = {};
    m_fastAccessComponentsValid = false;
}

void Scene::Play()
{
	m_isPlaying = true;

	/* Wake up actors to allow them to react to OnEnable, OnDisable and OnDestroy, */
	std::for_each(m_gameobject.begin(), m_gameobject.end(), [](GameObject * p_element) { p_element->SetSleeping(false); });

	std::for_each(m_gameobject.begin(), m_gameobject.end(), [](GameObject * p_element) { if (p_element->IsActive()) p_element->OnAwake(); });
	std::for_each(m_gameobject.begin(), m_gameobject.end(), [](GameObject * p_element) { if (p_element->IsActive()) p_element->OnEnable(); });
	std::for_each(m_gameobject.begin(), m_gameobject.end(), [](GameObject * p_element) { if (p_element->IsActive()) p_element->OnStart(); });
}

bool Scene::IsPlaying() const
{
	return m_isPlaying;
}

void Scene::Update(float p_deltaTime)
{
	auto actors = m_gameobject;
	std::for_each(actors.begin(), actors.end(), std::bind(std::mem_fn(&GameObject::OnUpdate), std::placeholders::_1, p_deltaTime));
}

void Scene::FixedUpdate(float p_deltaTime)
{
	auto actors = m_gameobject;
	std::for_each(actors.begin(), actors.end(), std::bind(std::mem_fn(&GameObject::OnFixedUpdate), std::placeholders::_1, p_deltaTime));
}

void Scene::LateUpdate(float p_deltaTime)
{
	auto actors = m_gameobject;
	std::for_each(actors.begin(), actors.end(), std::bind(std::mem_fn(&GameObject::OnLateUpdate), std::placeholders::_1, p_deltaTime));
}

Engine::GameObject& Scene::CreateGameObject()
{
	return CreateGameObject("New GameObject");
}

Engine::GameObject& Scene::CreateGameObject(const std::string& p_name, const std::string& p_tag)
{
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
	if (!gameObject)
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
                gameObject->OnStart();
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
    (void)p_compononent;
    RequestFastAccessComponentsRebuild();
}

void Scene::OnComponentRemoved(Components::Component* p_compononent)
{
    (void)p_compononent;
    RequestFastAccessComponentsRebuild();
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

		for (auto* child : go->GetChildren())
			collectComponents(child);
	};

	for (auto* go : m_gameobject)
	{
		collectComponents(go);
	}
	m_fastAccessComponentsValid = true;
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



#include <algorithm>
#include <string>

#include "SceneSystem/Scene.h"
#include "GameObject.h"
using namespace  NLS::Engine::SceneSystem;
Scene::Scene()
{

}

Scene::~Scene()
{
	std::for_each(m_actors.begin(), m_actors.end(), [](GameObject* element)
	{ 
		delete element;
	});

	m_actors.clear();
}

void Scene::Play()
{
	m_isPlaying = true;

	/* Wake up actors to allow them to react to OnEnable, OnDisable and OnDestroy, */
	std::for_each(m_actors.begin(), m_actors.end(), [](GameObject * p_element) { p_element->SetSleeping(false); });

	std::for_each(m_actors.begin(), m_actors.end(), [](GameObject * p_element) { if (p_element->IsActive()) p_element->OnAwake(); });
	std::for_each(m_actors.begin(), m_actors.end(), [](GameObject * p_element) { if (p_element->IsActive()) p_element->OnEnable(); });
	std::for_each(m_actors.begin(), m_actors.end(), [](GameObject * p_element) { if (p_element->IsActive()) p_element->OnStart(); });
}

bool Scene::IsPlaying() const
{
	return m_isPlaying;
}

void Scene::Update(float p_deltaTime)
{
	auto actors = m_actors;
	std::for_each(actors.begin(), actors.end(), std::bind(std::mem_fn(&GameObject::OnUpdate), std::placeholders::_1, p_deltaTime));
}

void Scene::FixedUpdate(float p_deltaTime)
{
	auto actors = m_actors;
	std::for_each(actors.begin(), actors.end(), std::bind(std::mem_fn(&GameObject::OnFixedUpdate), std::placeholders::_1, p_deltaTime));
}

void Scene::LateUpdate(float p_deltaTime)
{
	auto actors = m_actors;
	std::for_each(actors.begin(), actors.end(), std::bind(std::mem_fn(&GameObject::OnLateUpdate), std::placeholders::_1, p_deltaTime));
}

Engine::GameObject& Scene::CreateGameObject()
{
	return CreateGameObject("New GameObject");
}

Engine::GameObject& Scene::CreateGameObject(const std::string& p_name, const std::string& p_tag)
{
	m_actors.push_back(new Engine::GameObject(m_availableID++, p_name, p_tag, m_isPlaying));
	GameObject& instance = *m_actors.back();
	instance.ComponentAddedEvent	+= std::bind(&Scene::OnComponentAdded, this, std::placeholders::_1);
	instance.ComponentRemovedEvent	+= std::bind(&Scene::OnComponentRemoved, this, std::placeholders::_1);
	if (m_isPlaying)
	{
		instance.SetSleeping(false);
		if (instance.IsActive())
		{
			instance.OnAwake();
			instance.OnEnable();
			instance.OnStart();
		}
	}
	return instance;
}

bool Scene::DestroyActor(GameObject& p_target)
{
	auto found = std::find_if(m_actors.begin(), m_actors.end(), [&p_target](Engine::GameObject* element)
	{
		return element == &p_target;
	});

	if (found != m_actors.end())
	{
		delete *found;
		m_actors.erase(found);
		return true;
	}
	else
	{
		return false;
	}
}

void Scene::CollectGarbages()
{
	m_actors.erase(std::remove_if(m_actors.begin(), m_actors.end(), [this](GameObject* element)
	{ 
		bool isGarbage = !element->IsAlive();
		if (isGarbage)
		{
			delete element;
		}
		return isGarbage;
	}), m_actors.end());
}

Engine::GameObject* Scene::FindActorByName(const std::string& p_name) const
{
	auto result = std::find_if(m_actors.begin(), m_actors.end(), [p_name](Engine::GameObject* element)
	{ 
		return element->GetName() == p_name;
	});

	if (result != m_actors.end())
		return *result;
	else
		return nullptr;
}

Engine::GameObject* Scene::FindActorByTag(const std::string & p_tag) const
{
	auto result = std::find_if(m_actors.begin(), m_actors.end(), [p_tag](Engine::GameObject* element)
	{
		return element->GetTag() == p_tag;
	});

	if (result != m_actors.end())
		return *result;
	else
		return nullptr;
}

Engine::GameObject* Scene::FindActorByID(int64_t p_id) const
{
	auto result = std::find_if(m_actors.begin(), m_actors.end(), [p_id](Engine::GameObject* element)
	{
		return element->GetWorldID() == p_id;
	});

	if (result != m_actors.end())
		return *result;
	else
		return nullptr;
}

std::vector<std::reference_wrapper<Engine::GameObject>> Scene::FindActorsByName(const std::string & p_name) const
{
	std::vector<std::reference_wrapper<Engine::GameObject>> actors;

	for (auto actor : m_actors)
	{
		if (actor->GetName() == p_name)
			actors.push_back(std::ref(*actor));
	}

	return actors;
}

std::vector<std::reference_wrapper<Engine::GameObject>> Scene::FindActorsByTag(const std::string & p_tag) const
{
	std::vector<std::reference_wrapper<Engine::GameObject>> actors;

	for (auto actor : m_actors)
	{
		if (actor->GetTag() == p_tag)
			actors.push_back(std::ref(*actor));
	}

	return actors;
}

Engine::Components::CameraComponent* Scene::FindMainCamera() const
{
	for (Engine::Components::CameraComponent* camera : m_fastAccessComponents.cameras)
	{
		if (camera->gameobject()->IsActive())
		{
			return camera;
		}
	}

	return nullptr;
}

void Scene::OnComponentAdded(Engine::Components::Component& p_compononent)
{
	if (auto result = dynamic_cast<Engine::Components::MeshRenderer*>(&p_compononent))
		m_fastAccessComponents.modelRenderers.push_back(result);

	if (auto result = dynamic_cast<Engine::Components::CameraComponent*>(&p_compononent))
		m_fastAccessComponents.cameras.push_back(result);

	if (auto result = dynamic_cast<Engine::Components::LightComponent*>(&p_compononent))
		m_fastAccessComponents.lights.push_back(result);
}

void Scene::OnComponentRemoved(Engine::Components::Component& p_compononent)
{
	if (auto result = dynamic_cast<Engine::Components::MeshRenderer*>(&p_compononent))
		m_fastAccessComponents.modelRenderers.erase(std::remove(m_fastAccessComponents.modelRenderers.begin(), m_fastAccessComponents.modelRenderers.end(), result), m_fastAccessComponents.modelRenderers.end());

	if (auto result = dynamic_cast<Engine::Components::CameraComponent*>(&p_compononent))
		m_fastAccessComponents.cameras.erase(std::remove(m_fastAccessComponents.cameras.begin(), m_fastAccessComponents.cameras.end(), result), m_fastAccessComponents.cameras.end());

	if (auto result = dynamic_cast<Engine::Components::LightComponent*>(&p_compononent))
		m_fastAccessComponents.lights.erase(std::remove(m_fastAccessComponents.lights.begin(), m_fastAccessComponents.lights.end(), result), m_fastAccessComponents.lights.end());
}

std::vector<Engine::GameObject*>& Scene::GetActors()
{
	return m_actors;
}

const Scene::FastAccessComponents& Scene::GetFastAccessComponents() const
{
	return m_fastAccessComponents;
}


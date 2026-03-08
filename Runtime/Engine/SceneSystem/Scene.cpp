
#include <algorithm>
#include <string>

#include <Debug/Logger.h>
#include "SceneSystem/Scene.h"
#include "GameObject.h"
using namespace NLS;
using namespace UDRefl;
using namespace NLS::Engine::SceneSystem;
Scene::Scene()
{

}

Scene::~Scene()
{
	std::for_each(m_gameobject.begin(), m_gameobject.end(), [](GameObject* element)
	{ 
		delete element;
	});

	m_gameobject.clear();
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
	GameObject* newGameObject = new Engine::GameObject(m_availableID++, p_name, p_tag, m_isPlaying);

	AddGameObject(newGameObject);

	return *newGameObject;
}

bool Scene::AddGameObject(GameObject* gameObject)
{
	// check added...

	m_gameobject.push_back(gameObject);
	GameObject& instance = *m_gameobject.back();

	auto AddComponents = [this](GameObject* go)
		{
			for (auto&& component : go->GetComponents())
			{
				OnComponentAdded(component);
			}
			go->ComponentAddedEvent += std::bind(&Scene::OnComponentAdded, this, std::placeholders::_1);
			go->ComponentRemovedEvent += std::bind(&Scene::OnComponentRemoved, this, std::placeholders::_1);
		};

	std::function<void(GameObject*)> AddGameObjectRecursively = [this, &AddComponents, &AddGameObjectRecursively](GameObject* go)
		{
			AddComponents(go);
			for (auto&& child : go->GetChildren())
			{
				AddGameObjectRecursively(child);
			}
		};
	AddGameObjectRecursively(&instance);

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
	return true;
}

bool Scene::AddActor(Actor* actor)
{
	return AddGameObject(actor->GetGameObject());
}

bool Scene::DestroyActor(GameObject& p_target)
{
	auto found = std::find_if(m_gameobject.begin(), m_gameobject.end(), [&p_target](Engine::GameObject* element)
	{
		return element == &p_target;
	});

	if (found != m_gameobject.end())
	{
		delete *found;
		m_gameobject.erase(found);
		return true;
	}
	else
	{
		return false;
	}
}

void Scene::CollectGarbages()
{
	m_gameobject.erase(std::remove_if(m_gameobject.begin(), m_gameobject.end(), [this](GameObject* element)
	{ 
		bool isGarbage = !element->IsAlive();
		if (isGarbage)
		{
			delete element;
		}
		return isGarbage;
	}), m_gameobject.end());
}

Engine::GameObject* Scene::FindActorByName(const std::string& p_name) const
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

Engine::GameObject* Scene::FindActorByTag(const std::string & p_tag) const
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

Engine::GameObject* Scene::FindActorByID(int64_t p_id) const
{
	auto result = std::find_if(m_gameobject.begin(), m_gameobject.end(), [p_id](Engine::GameObject* element)
	{
		return element->GetWorldID() == p_id;
	});

	if (result != m_gameobject.end())
		return *result;
	else
		return nullptr;
}

std::vector<std::reference_wrapper<Engine::GameObject>> Scene::FindActorsByName(const std::string & p_name) const
{
	std::vector<std::reference_wrapper<Engine::GameObject>> actors;

	for (auto actor : m_gameobject)
	{
		if (actor->GetName() == p_name)
			actors.push_back(std::ref(*actor));
	}

	return actors;
}

std::vector<std::reference_wrapper<Engine::GameObject>> Scene::FindActorsByTag(const std::string & p_tag) const
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

void Scene::OnComponentAdded(SharedObject p_compononent)
{
	(void)p_compononent;
	RebuildFastAccessComponents();
}

void Scene::OnComponentRemoved(SharedObject p_compononent)
{
	(void)p_compononent;
	RebuildFastAccessComponents();
}

std::vector<Engine::GameObject*>& Scene::GetActors()
{
	return m_gameobject;
}

const Scene::FastAccessComponents& Scene::GetFastAccessComponents() const
{
	if (m_fastAccessComponents.modelRenderers.empty() &&
		m_fastAccessComponents.cameras.empty() &&
		m_fastAccessComponents.lights.empty() &&
		m_fastAccessComponents.skyboxs.empty())
	{
		const_cast<Scene*>(this)->RebuildFastAccessComponents();
	}
	return m_fastAccessComponents;
}

void Scene::RebuildFastAccessComponents()
{
	m_fastAccessComponents.modelRenderers.clear();
	m_fastAccessComponents.cameras.clear();
	m_fastAccessComponents.lights.clear();
	m_fastAccessComponents.skyboxs.clear();

	for (auto* go : m_gameobject)
	{
		if (!go)
			continue;

		for (auto& component : go->GetComponents())
		{
			auto* baseComponent = component.AsPtr<Engine::Components::Component>();
			if (!baseComponent)
				continue;

			const auto typeName = component.GetType().GetName();
			if (typeName.find("MeshRenderer") != std::string::npos)
			{
				if (auto* ptr = dynamic_cast<Engine::Components::MeshRenderer*>(baseComponent))
					m_fastAccessComponents.modelRenderers.push_back(ptr);
			}
			else if (typeName.find("CameraComponent") != std::string::npos)
			{
				if (auto* ptr = dynamic_cast<Engine::Components::CameraComponent*>(baseComponent))
					m_fastAccessComponents.cameras.push_back(ptr);
			}
			else if (typeName.find("LightComponent") != std::string::npos)
			{
				if (auto* ptr = dynamic_cast<Engine::Components::LightComponent*>(baseComponent))
					m_fastAccessComponents.lights.push_back(ptr);
			}
			else if (typeName.find("SkyBoxComponent") != std::string::npos)
			{
				if (auto* ptr = dynamic_cast<Engine::Components::SkyBoxComponent*>(baseComponent))
					m_fastAccessComponents.skyboxs.push_back(ptr);
			}
		}
	}
}
#include "Reflection/Compat/ReflMngr.hpp"
using namespace NLS::UDRefl;
void Scene::Bind()
{
    Mngr.RegisterType<Scene>();
    Mngr.AddField<&Scene::m_gameobject>("m_gameobject");
}


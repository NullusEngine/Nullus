#pragma once


#include "GameObject.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkyBoxComponent.h"
#include "Components/MeshRenderer.h"
#include "Resource/Actor/Actor.h"

#include "EngineDef.h"
#include "Reflection/Macros.h"
#include "SceneSystem/Scene.generated.h"

namespace NLS::Engine::SceneSystem
{
	/**
	* The scene is a set of actors
	*/
	CLASS() class NLS_ENGINE_API Scene
	{
    public:
		GENERATED_BODY()
		/**
		* Contains a set of vectors of components that are sorted. It allows fast
		* manipulation of components without parsing the whole scene
		*/
		struct FastAccessComponents
		{
			std::vector<Components::MeshRenderer*>		modelRenderers;
			std::vector<Components::CameraComponent*>			cameras;
			std::vector<Components::LightComponent*>			lights;
			std::vector<Components::SkyBoxComponent*>			skyboxs;
		};

		/**
		* Constructor of the scene
		*/
		Scene();

		/**
		* Handle the memory de-allocation of every actors
		*/
		~Scene();

		/**
		* Play the scene
		*/
        FUNCTION()
		void Play();

		/**
		* Returns true if the scene is playing
		*/
		bool IsPlaying() const;

		/**
		* Update every actors
		* @param p_deltaTime
		*/
		void Update(float p_deltaTime);

		/**
		* Update every actors 60 frames per seconds
		* @param p_deltaTime
		*/
		void FixedUpdate(float p_deltaTime);

		/**
		* Update every actors lately
		* @param p_deltaTime
		*/
		void LateUpdate(float p_deltaTime);

		/**
		* Create an actor with a default name and return a reference to it.
		*/
		GameObject& CreateGameObject();

		/**
		* Create an actor with the given name and return a reference to it.
		* @param p_name
		* @param p_tag
		*/
		GameObject& CreateGameObject(const std::string& p_name, const std::string& p_tag = "");

		bool AddGameObject(GameObject* gameObject);

		bool AddActor(Actor* actor);

		/**
		* Destroy and actor and return true on success
		* @param p_target (The actor to remove from the scene)
		*/
		bool DestroyActor(GameObject& p_target);

		/**
		* Collect garbages by removing Destroyed-marked actors
		*/
		void CollectGarbages();

		/**
		* Return the first actor identified by the given name, or nullptr on fail
		* @param p_name
		*/
		GameObject* FindActorByName(const std::string& p_name) const;

		/**
		* Return the first actor identified by the given tag, or nullptr on fail
		* @param p_tag
		*/
		GameObject* FindActorByTag(const std::string& p_tag) const;

		/**
		* Return the actor identified by the given ID (Returns nullptr on fail)
		* @param p_id
		*/
		GameObject* FindActorByID(int64_t p_id) const;

		/**
		* Return every actors identified by the given name
		* @param p_name
		*/
		std::vector<std::reference_wrapper<GameObject>> FindActorsByName(const std::string& p_name) const;

		/**
		* Return every actors identified by the given tag
		* @param p_tag
		*/
		std::vector<std::reference_wrapper<GameObject>> FindActorsByTag(const std::string& p_tag) const;

		/**
		* Parse the scene to find the main camera
		*/
		Components::CameraComponent* FindMainCamera() const;

		/**
		* Callback method called everytime a component is added on an actor of the scene
		* @param p_component
		*/
        void OnComponentAdded(Components::Component* p_compononent);

		/**
		* Callback method called everytime a component is removed on an actor of the scene
		* @param p_component
		*/
        void OnComponentRemoved(Components::Component* p_compononent);

		/**
		* Return a reference on the actor map
		*/
		std::vector<GameObject*>& GetActors();
		const std::vector<GameObject*>& GetActors() const;

		/**
		* Return the fast access components data structure
		*/
		const FastAccessComponents& GetFastAccessComponents() const;

		void SetAvailableID(int64_t p_nextID);

	private:
		void RebuildFastAccessComponents();
		int64_t m_availableID = 1;
		bool m_isPlaying = false;
		std::vector<GameObject*> m_gameobject;

		FastAccessComponents m_fastAccessComponents;
	};
}

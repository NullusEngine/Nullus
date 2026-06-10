#pragma once


#include <unordered_set>

#include "GameObject.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkyBoxComponent.h"
#include "Components/MeshRenderer.h"

#include "EngineDef.h"
#include "Reflection/Macros.h"
#include "Reflection/Object.h"
#include "SceneSystem/Scene.generated.h"

namespace NLS::Engine::SceneSystem
{
	/**
	* The scene is a set of GameObjects
	*/
	CLASS(NLS_ENGINE_API Scene) : public NLS::Object
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
		* Handle the memory de-allocation of every GameObjects
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
		* Update every GameObjects
		* @param p_deltaTime
		*/
		void Update(float p_deltaTime);

		/**
		* Update every GameObjects 60 frames per seconds
		* @param p_deltaTime
		*/
		void FixedUpdate(float p_deltaTime);

		/**
		* Update every GameObjects lately
		* @param p_deltaTime
		*/
		void LateUpdate(float p_deltaTime);

		/**
		* Create an GameObject with a default name and return a reference to it.
		*/
		GameObject& CreateGameObject();

		/**
		* Create an GameObject with the given name and return a reference to it.
		* @param p_name
		* @param p_tag
		*/
		GameObject& CreateGameObject(const std::string& p_name, const std::string& p_tag = "");

		bool AddGameObject(GameObject* gameObject);

		/**
		* Destroy and GameObject and return true on success
		* @param p_target (The GameObject to remove from the scene)
		*/
		bool DestroyGameObject(GameObject& p_target);

		/**
		* Collect garbages by removing Destroyed-marked GameObjects
		*/
		void CollectGarbages();

		/**
		* Return the first GameObject identified by the given name, or nullptr on fail
		* @param p_name
		*/
		GameObject* FindGameObjectByName(const std::string& p_name) const;

		/**
		* Return the first GameObject identified by the given tag, or nullptr on fail
		* @param p_tag
		*/
		GameObject* FindGameObjectByTag(const std::string& p_tag) const;

		/**
		* Return every GameObjects identified by the given name
		* @param p_name
		*/
		std::vector<std::reference_wrapper<GameObject>> FindGameObjectsByName(const std::string& p_name) const;

		/**
		* Return every GameObjects identified by the given tag
		* @param p_tag
		*/
		std::vector<std::reference_wrapper<GameObject>> FindGameObjectsByTag(const std::string& p_tag) const;

		/**
		* Parse the scene to find the main camera
		*/
		Components::CameraComponent* FindMainCamera() const;

		/**
		* Callback method called everytime a component is added on an GameObject of the scene
		* @param p_component
		*/
        void OnComponentAdded(Components::Component* p_compononent);

		/**
		* Callback method called everytime a component is removed on an GameObject of the scene
		* @param p_component
		*/
        void OnComponentRemoved(Components::Component* p_compononent);

		/**
		* Return a reference on the GameObject map
		*/
		std::vector<GameObject*>& GetGameObjects();
        FUNCTION()
		const std::vector<GameObject*>& GetGameObjects() const;

		/**
		* Return the fast access components data structure
		*/
		const FastAccessComponents& GetFastAccessComponents() const;
		uint64_t GetFastAccessComponentsRevision() const;
		uint64_t GetRenderContentRevision() const;
		void MarkRenderContentChanged();

		void RebuildRuntimeCachesAfterLoad()
		{
			RebuildFastAccessComponents();
		}

	private:
        void CollectGameObjectSubtree(GameObject& p_GameObject, std::vector<GameObject*>& p_outGameObjects, std::unordered_set<GameObject*>& p_visitedGameObjects);
        void RemoveGameObjectsFromSceneList(const std::unordered_set<GameObject*>& p_GameObjects);
        void NotifyGameObjectDestroyed(GameObject& p_GameObject);
        void NotifyGameObjectDestroyed(GameObject& p_GameObject, std::unordered_set<GameObject*>& p_notifiedGameObjects);
        void DestroyGameObjectSubtree(GameObject& p_GameObject);
        void DestroyCollectedGameObjects(std::vector<GameObject*>& p_GameObjects);
		void RebuildFastAccessComponents();
		bool m_isPlaying = false;
		std::vector<GameObject*> m_gameobject;

		FastAccessComponents m_fastAccessComponents;
		bool m_fastAccessComponentsValid = false;
		uint64_t m_fastAccessComponentsRevision = 0u;
		uint64_t m_renderContentRevision = 0u;
	};
}

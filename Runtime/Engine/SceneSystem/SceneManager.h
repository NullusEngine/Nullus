#pragma once


#include <string>
#include "SceneSystem/Scene.h"
#include "EngineDef.h"
namespace NLS::Engine::SceneSystem
{
	/**
	* The scene manager of the current scene
	*/
	class NLS_ENGINE_API SceneManager
	{
	public:

		/** 
		* Default constructor
		* @param p_sceneRootFolder (Optional)
		*/
		SceneManager(const std::string& p_sceneRootFolder = "");

		/**
		* Default destructor
		*/
		~SceneManager();

		/**
		* Update
		*/
		void Update();

		/**
		* Load an play a scene with a delay
		* @param p_path
		* @param p_absolute
		*/
		void LoadAndPlayDelayed(const std::string& p_path, bool p_absolute = false);

		/**
		* Load an empty scene in memory
		*/
		void LoadEmptyScene();

		/**
		* Load an empty lighted scene in memory
		*/
		void LoadEmptyLightedScene();

		/**
		* Load specific scene in memory
		* @param p_scenePath
		* @param p_absolute (If this setting is set to true, the scene loader will ignore the "SceneRootFolder" given on SceneManager construction)
		*/
		bool LoadScene(const std::string& p_path, bool p_absolute = false);

		/**
		* Load specific scene in memory
		* @param p_scenePath
		*/
		//bool LoadSceneFromMemory(tinyxml2::XMLDocument& p_doc);

		/**
		* Save the current scene to the given path
		* @param p_path
		*/
		bool SaveCurrentScene(const std::string& p_path);

		/**
		* Destroy current scene from memory
		*/
		void UnloadCurrentScene();

		/**
		* Return true if a scene is currently loaded
		*/
		bool HasCurrentScene() const;

		/*
		* Return current loaded scene
		*/
		Scene* GetCurrentScene() const;

		/**
		* Return the current scene source path
		*/
		std::string GetCurrentSceneSourcePath() const;

		/**
		* Return true if the currently loaded scene has been loaded from a file
		*/
		bool IsCurrentSceneLoadedFromDisk() const;

		/**
		* Store the given path as the current scene source path
		* @param p_path
		*/
		void StoreCurrentSceneSourcePath(const std::string& p_path);

		/**
		* Reset the current scene source path to an empty string
		*/
		void ForgetCurrentSceneSourcePath();

		/**
		* Mark the current scene as changed since the last save/load.
		*/
		void MarkCurrentSceneDirty();

		/**
		* Mark the current scene as matching its last saved/loaded state.
		*/
		void MarkCurrentSceneClean();

		/**
		* Return true if the current scene has unsaved edits.
		*/
		bool HasUnsavedSceneChanges() const;

	public:
		NLS::Event<> SceneLoadEvent;
		NLS::Event<> SceneUnloadEvent;
		NLS::Event<const std::string&> CurrentSceneSourcePathChangedEvent;
		NLS::Event<bool> CurrentSceneDirtyStateChangedEvent;

	private:
		const std::string m_sceneRootFolder;
		Scene* m_currentScene = nullptr;

		bool m_currentSceneLoadedFromPath = false;
		std::string m_currentSceneSourcePath = "";
		bool m_currentSceneDirty = false;

		std::function<void()> m_delayedLoadCall;
	};
}

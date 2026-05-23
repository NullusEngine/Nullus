#pragma once

#include <cstdint>
#include <cstddef>

#include <Rendering/Resources/Loaders/TextureLoader.h>
#include <Rendering/Entities/Camera.h>

#include <SceneSystem/SceneManager.h>

#include <Assets/PrefabEditorWorkflow.h>
#include <UI/Panels/PanelWindow.h>
#include <UI/Widgets/Layout/Group.h>
#include <UI/Widgets/Layout/TreeNode.h>

namespace NLS::Editor::Panels
{
	class Hierarchy : public UI::PanelWindow
	{
	public:
		/**
		* Constructor
		* @param p_title
		* @param p_opened
		* @param p_windowSettings
		*/
		Hierarchy
		(
			const std::string& p_title,
			bool p_opened,
			const UI::PanelWindowSettings& p_windowSettings
		);
		~Hierarchy();
		
		/**
		* Clear hierarchy nodes
		*/
		void Clear();

		/**
		* Unselect every widgets
		*/
		void UnselectGameObjectsWidgets();

		/**
		* Select the widget corresponding to the given GameObject
		* @param p_GameObject
		*/
		void SelectGameObjectByInstance(Engine::GameObject& p_GameObject);

		/**
		* Select the widget
		* @param p_GameObject
		*/
		void SelectGameObjectByWidget(UI::Widgets::TreeNode& p_widget);

		/**
		* Attach the given GameObject linked widget to its parent widget
		* @param p_GameObject
		*/
		void AttachGameObjectToParent(Engine::GameObject& p_GameObject);

		/**
		* Detach the given GameObject linked widget from its parent widget
		* @param p_GameObject
		*/
		void DetachFromParent(Engine::GameObject& p_GameObject);

		/**
		* Delete the widget referencing the given GameObject
		* @param p_GameObject
		*/
		void DeleteGameObjectByInstance(Engine::GameObject& p_GameObject);

		/**
		* Add a widget referencing the given GameObject
		* @param p_GameObject
		*/
		void AddGameObjectByInstance(Engine::GameObject& p_GameObject);

		void RebuildFromCurrentScene();

		void RefreshSceneRootName();
        void RefreshPrefabPresentation(Engine::GameObject& p_GameObject);
		size_t GetHierarchyNodeCount() const { return m_widgetGameObjectLink.size(); }

	public:
		Event<Engine::GameObject&> GameObjectSelectedEvent;
		Event<Engine::GameObject&> GameObjectUnselectedEvent;

	private:
		UI::Widgets::TreeNode* m_sceneRoot;
        NLS::Editor::Assets::PrefabInstanceRegistry* m_prefabInstanceRegistry = nullptr;

		std::unordered_map<Engine::GameObject*, UI::Widgets::TreeNode*> m_widgetGameObjectLink;
		uint64_t m_gameObjectUnselectedListener = 0;
		uint64_t m_sceneUnloadListener = 0;
		uint64_t m_gameObjectCreatedListener = 0;
		uint64_t m_gameObjectDestroyedListener = 0;
		uint64_t m_gameObjectSelectedListener = 0;
		uint64_t m_gameObjectAttachedListener = 0;
		uint64_t m_gameObjectDetachedListener = 0;
		uint64_t m_sceneLoadListener = 0;
		uint64_t m_sceneSourcePathChangedListener = 0;
		uint64_t m_sceneDirtyStateChangedListener = 0;
	};
}

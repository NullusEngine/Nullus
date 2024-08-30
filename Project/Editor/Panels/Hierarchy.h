#pragma once

#include <Rendering/Resources/Loaders/TextureLoader.h>
#include <Rendering/Entities/Camera.h>

#include <SceneSystem/SceneManager.h>

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
		
		/**
		* Clear hierarchy nodes
		*/
		void Clear();

		/**
		* Unselect every widgets
		*/
		void UnselectActorsWidgets();

		/**
		* Select the widget corresponding to the given actor
		* @param p_actor
		*/
		void SelectActorByInstance(Engine::GameObject& p_actor);

		/**
		* Select the widget
		* @param p_actor
		*/
		void SelectActorByWidget(UI::Widgets::TreeNode& p_widget);

		/**
		* Attach the given actor linked widget to its parent widget
		* @param p_actor
		*/
		void AttachActorToParent(Engine::GameObject& p_actor);

		/**
		* Detach the given actor linked widget from its parent widget
		* @param p_actor
		*/
		void DetachFromParent(Engine::GameObject& p_actor);

		/**
		* Delete the widget referencing the given actor
		* @param p_actor
		*/
		void DeleteActorByInstance(Engine::GameObject& p_actor);

		/**
		* Add a widget referencing the given actor
		* @param p_actor
		*/
		void AddActorByInstance(Engine::GameObject& p_actor);

	public:
		Event<Engine::GameObject&> ActorSelectedEvent;
		Event<Engine::GameObject&> ActorUnselectedEvent;

	private:
		UI::Widgets::TreeNode* m_sceneRoot;

		std::unordered_map<Engine::GameObject*, UI::Widgets::TreeNode*> m_widgetActorLink;
	};
}
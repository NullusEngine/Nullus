#pragma once

#include <unordered_map>

//#include "Panels/MenuBar.h"
#include "UI/Panels/APanel.h"
#include "UI/Panels/PanelWindow.h"
#include "UI/Modules/Canvas.h"

namespace NLS::Editor::Core
{
	/**
	* Handle the creation and storage of UI panels
	*/
	class PanelsManager
	{
	public:
		/**
		* Constructor
		* @param p_canvas
		*/
		PanelsManager(UI::Canvas& p_canvas);

		/**
		* Create a panel
		* @param p_id
		* @param p_args
		*/
		template<typename T, typename... Args>
		void CreatePanel(const std::string& p_id, Args&&... p_args)
		{
			if constexpr (std::is_base_of<UI::PanelWindow, T>::value)
			{
				m_panels.emplace(p_id, std::make_unique<T>(p_id, std::forward<Args>(p_args)...));
				T& instance = *static_cast<T*>(m_panels.at(p_id).get());
				//GetPanelAs<Editor::Panels::MenuBar>("Menu Bar").RegisterPanel(instance.name, instance);
			}
			else
			{
				m_panels.emplace(p_id, std::make_unique<T>(std::forward<Args>(p_args)...));
			}

			m_canvas.AddPanel(*m_panels.at(p_id));
		}

		/**
		* Returns the panel identified by the given id casted in the given type
		* @param p_id
		*/
		template<typename T>
		T& GetPanelAs(const std::string& p_id)
		{
			return *static_cast<T*>(m_panels[p_id].get());
		}

		template<typename T>
		const T& GetPanelAs(const std::string& p_id) const
		{
			return *static_cast<const T*>(m_panels.at(p_id).get());
		}

		/// <summary>
		/// Adds a panel that was created outside this manager (for example a
		/// scripted editor window built at runtime from a managed descriptor).
		/// The manager takes ownership and keeps it for the panel lifetime so
		/// the canvas and the window menu always reference a live object.
		/// </summary>
		UI::APanel& AddPanel(const std::string& p_id, std::unique_ptr<UI::APanel> p_panel)
		{
			auto& stored = *m_panels.emplace(p_id, std::move(p_panel)).first->second;
			m_canvas.AddPanel(stored);
			return stored;
		}

		/// <summary>
		/// Returns true when a panel with the given id is already stored.
		/// </summary>
		bool HasPanel(const std::string& p_id) const { return m_panels.find(p_id) != m_panels.end(); }

		void DestroyPanels()
		{
			m_canvas.RemoveAllPanels();
			m_panels.clear();
		}

	private:
		std::unordered_map<std::string, std::unique_ptr<UI::APanel>> m_panels;
		UI::Canvas& m_canvas;
	};
}

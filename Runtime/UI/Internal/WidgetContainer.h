#pragma once

#include <optional>
#include <vector>

#include "UI/Widgets/AWidget.h"
#include "UI/Internal/EMemoryMode.h"

namespace NLS::UI::Internal
{
	/**
	* The base class for any widget container
	*/
	class NLS_UI_API WidgetContainer
	{
	public:
		/**
		* Constructor
		*/
		WidgetContainer() = default;

		/**
		* Destructor. Handle the memory de-allocation of every widgets that are internally managed
		*/
		virtual ~WidgetContainer();

		/**
		* Remove a widget from the container
		* @param p_widget
		*/
		void RemoveWidget(Widgets::AWidget& p_widget);

		/**
		* Remove all widgets from the container
		*/
		void RemoveAllWidgets();

		/**
		* Consider a widget
		* @param p_manageMemory
		*/
		void ConsiderWidget(Widgets::AWidget& p_widget, bool p_manageMemory = true);

		/**
		* Unconsider a widget
		* @param p_widget
		*/
		void UnconsiderWidget(Widgets::AWidget& p_widget);

		/**
		* Collect garbages by removing widgets marked as "Destroyed"
		*/
		void CollectGarbages();

		void MarkGarbageCollectionDirty();

		/**
		* Draw every widgets
		*/
		void DrawWidgets();

        /**
        * Allow the user to reverse the draw order of this widget container
        */
        void ReverseDrawOrder(bool reversed = true);

		/**
		* Create a widget
		* @param p_args
		*/
		template <typename T, typename ... Args>
		T& CreateWidget(Args&&... p_args)
		{
			m_widgets.emplace_back(new T(p_args...), Internal::EMemoryMode::INTERNAL_MANAGMENT);
			T& instance = *reinterpret_cast<T*>(m_widgets.back().first);
			instance.SetParent(this);
			if (instance.IsDestroyed())
				MarkGarbageCollectionDirty();
			return instance;
		}

		/**
		* Returns the widgets and their memory management mode
		*/
		const std::vector<std::pair<UI::Widgets::AWidget*, Internal::EMemoryMode>>& GetWidgets() const;

	protected:
		std::optional<Internal::EMemoryMode> ExtractWidget(Widgets::AWidget& p_widget);

	protected:
		std::vector<std::pair<UI::Widgets::AWidget*, Internal::EMemoryMode>> m_widgets;
        bool m_reversedDrawOrder = false;
		bool m_garbageCollectionDirty = false;
	};
}

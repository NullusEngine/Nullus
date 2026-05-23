#pragma once

#include <vector>

#include <Eventing/Event.h>
#include <Math/Color.h>

#include "UI/Internal/WidgetContainer.h"
#include "UI/Widgets/DataWidget.h"

namespace NLS::UI::Widgets
{
	/**
	* Widget that allow columnification
	*/
	class NLS_UI_API TreeNode : public DataWidget<std::string>, public Internal::WidgetContainer
	{
	public:
		/**
		* Constructor
		* @param p_name
		* @param p_arrowClickToOpen
		*/
		TreeNode(const std::string& p_name = "", bool p_arrowClickToOpen = false);

		/**
		* Open the tree node
		*/
		void Open();

		/**
		* Close the tree node
		*/
		void Close();

		/**
		* Returns true if the TreeNode is currently opened
		*/
		bool IsOpened() const;

	protected:
		virtual void _Draw_Impl() override;

	private:
		const char* GetImGuiLabel();

	public:
		std::string name;
		bool selected = false;
        bool leaf = false;
        bool useTextColor = false;
        Maths::Color textColor = Maths::Color(1.0f, 1.0f, 1.0f, 1.0f);

		NLS::Event<> ClickedEvent;
		NLS::Event<> DoubleClickedEvent;
		NLS::Event<> OpenedEvent;
		NLS::Event<> ClosedEvent;

	private:
		bool m_arrowClickToOpen = false;
		bool m_shouldOpen = false;
		bool m_shouldClose = false;
		bool m_opened = false;
		std::string m_imguiLabel;
		std::string m_cachedName;
		std::string m_cachedWidgetID;
	};
}

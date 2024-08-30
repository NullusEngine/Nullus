#pragma once

#include <vector>

#include <Eventing/Event.h>

#include "UI/Widgets/Layout/Group.h"

namespace NLS::UI::Widgets
{
	/**
	* Widget that behave like a group with a menu display
	*/
	class NLS_UI_API MenuList : public Group
	{
	public:
		/**
		* Constructor
		* @param p_name
		* @param p_locked
		*/
		MenuList(const std::string& p_name, bool p_locked = false);

	protected:
		virtual void _Draw_Impl() override;

	public:
		std::string name;
		bool locked;
		NLS::Event<> ClickedEvent;

	private:
		bool m_opened;
	};
}
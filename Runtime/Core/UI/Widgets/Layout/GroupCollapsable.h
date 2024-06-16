#pragma once

#include <vector>

#include <Eventing/Event.h>

#include "UI/Widgets/Layout/Group.h"

namespace NLS::UI::Widgets::Layout
{
	/**
	* Widget that can contains other widgets and is collapsable
	*/
	class NLS_CORE_API GroupCollapsable : public Group
	{
	public:
		/**
		* Constructor
		* @param p_name
		*/
		GroupCollapsable(const std::string& p_name = "");

	protected:
		virtual void _Draw_Impl() override;

	public:
		std::string name;
		bool closable = false;
		bool opened = true;
		NLS::Event<> CloseEvent;
		NLS::Event<> OpenEvent;
	};
}
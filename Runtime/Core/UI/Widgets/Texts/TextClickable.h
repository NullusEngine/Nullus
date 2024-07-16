#pragma once

#include <Eventing/Event.h>

#include "UI/Widgets/Texts/Text.h"

namespace NLS::UI::Widgets::Texts
{
	/**
	* Widget to display text on a panel that is also clickable
	*/
	class NLS_CORE_API TextClickable : public Text
	{
	public:
		/**
		* Constructor
		* @param p_content
		*/
		TextClickable(const std::string& p_content = "");

	protected:
		virtual void _Draw_Impl() override;

	public:
		NLS::Event<> ClickedEvent;
		NLS::Event<> DoubleClickedEvent;
	};
}
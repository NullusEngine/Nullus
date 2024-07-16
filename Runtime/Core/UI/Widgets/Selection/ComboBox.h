#pragma once

#include <map>

#include <Eventing/Event.h>

#include "UI/Widgets/DataWidget.h"

namespace NLS::UI::Widgets::Selection
{
	/**
	* Widget that can display a list of values that the user can select
	*/
	class NLS_CORE_API ComboBox : public DataWidget<int>
	{
	public:
		/**
		* Constructor
		* @param p_currentChoice
		*/
		ComboBox(int p_currentChoice = 0);

	protected:
		void _Draw_Impl() override;

	public:
		std::map<int, std::string> choices;
		int currentChoice;

	public:
		NLS::Event<int> ValueChangedEvent;
	};
}
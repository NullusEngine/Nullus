#pragma once

#include <Eventing/Event.h>

#include "UI/Widgets/DataWidget.h"

namespace NLS::UI::Widgets::InputFields
{
	/**
	* Input widget of type string
	*/
	class NLS_CORE_API InputText : public DataWidget<std::string>
	{
	public:
		/**
		* Constructor
		* @param p_content
		* @param p_label
		*/
		InputText(const std::string& p_content = "", const std::string& p_label = "");

	protected:
		void _Draw_Impl() override;

	public:
		std::string content;
		std::string label;
		bool selectAllOnClick = false;
		NLS::Event<std::string> ContentChangedEvent;
		NLS::Event<std::string> EnterPressedEvent;
	};
}
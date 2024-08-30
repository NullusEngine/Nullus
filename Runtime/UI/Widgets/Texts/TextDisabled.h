#pragma once

#include "UI/Widgets/Texts/Text.h"

namespace NLS::UI::Widgets
{
	/**
	* Widget to display a disabled text on a panel
	*/
	class TextDisabled : public Text
	{
	public:
		/**
		* Constructor
		* @param p_content
		*/
		TextDisabled(const std::string& p_content = "");

	protected:
		virtual void _Draw_Impl() override;
	};
}
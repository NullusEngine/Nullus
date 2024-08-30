#pragma once

#include "UI/Widgets/Texts/Text.h"
#include "Color.h"

namespace NLS::UI::Widgets
{
	/**
	* Widget to display text on a panel that can be colored
	*/
	class NLS_UI_API TextColored : public Text
	{
	public:
		/**
		* Constructor
		* @param p_content
		* @param p_color
		*/
		TextColored(const std::string& p_content = "", const Maths::Color& p_color = Maths::Color(1.0f, 1.0f, 1.0f, 1.0f));

	public:
		Maths::Color color;

	protected:
		virtual void _Draw_Impl() override;
	};
}
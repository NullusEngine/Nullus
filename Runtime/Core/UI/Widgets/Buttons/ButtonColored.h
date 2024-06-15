#pragma once

#include <string>

#include <Vector2.h>

#include "UI/Types/Color.h"
#include "UI/Widgets/Buttons/AButton.h"

namespace NLS::UI::Widgets::Buttons
{
	/**
	* Button widget of a single color (Color palette element)
	*/
	class ButtonColored : public AButton
	{
	public:
		/**
		* Constructor
		* @param p_label
		* @param p_color
		* @param p_size
		* @param p_enableAlpha
		*/
		ButtonColored(const std::string& p_label = "", const Types::Color& p_color = {}, const Maths::Vector2& p_size =Maths::Vector2(0.f, 0.f), bool p_enableAlpha = true);

	protected:
		void _Draw_Impl() override;

	public:
		std::string label;
		Types::Color color;
		Maths::Vector2 size;
		bool enableAlpha;
	};
}
#pragma once

#include <string>

#include <Vector2.h>

#include "Color.h"
#include "UI/Widgets/Buttons/AButton.h"

namespace NLS::UI::Widgets
{
	/**
	* Button widget of a single color (Color palette element)
	*/
	class NLS_UI_API ButtonColored : public AButton
	{
	public:
		/**
		* Constructor
		* @param p_label
		* @param p_color
		* @param p_size
		* @param p_enableAlpha
		*/
		ButtonColored(const std::string& p_label = "", const Maths::Color& p_color = {}, const Maths::Vector2& p_size =Maths::Vector2(0.f, 0.f), bool p_enableAlpha = true);

	protected:
		void _Draw_Impl() override;

	public:
		std::string label;
		Maths::Color color;
		Maths::Vector2 size;
		bool enableAlpha;
	};
}
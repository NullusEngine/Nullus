#pragma once

#include <string>

#include <Vector2.h>

#include "UI/Widgets/Buttons/AButton.h"
#include "Color.h"

namespace NLS::UI::Widgets
{
	/**
	* Simple button widget
	*/
	class NLS_UI_API Button : public AButton
	{
	public:
		/**
		* Constructor
		* @param p_label
		* @param p_size
		* @param p_disabled
		*/
		Button(const std::string& p_label = "", const Maths::Vector2& p_size = Maths::Vector2(0.f, 0.f), bool p_disabled = false);

	protected:
		void _Draw_Impl() override;

	public:
		std::string label;
		Maths::Vector2 size;
		bool disabled = false;

		Maths::Color idleBackgroundColor;
		Maths::Color hoveredBackgroundColor;
		Maths::Color clickedBackgroundColor;

		Maths::Color textColor;
	};
}
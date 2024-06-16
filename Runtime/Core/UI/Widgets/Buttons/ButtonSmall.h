#pragma once

#include <string>

#include "UI/Widgets/Buttons/AButton.h"
#include "Color.h"

namespace NLS::UI::Widgets::Buttons
{
	/**
	* Small button widget
	*/
	class NLS_CORE_API ButtonSmall : public AButton
	{
	public:
		/**
		* Constructor
		* @param p_label
		*/
		ButtonSmall(const std::string& p_label = "");

	protected:
		void _Draw_Impl() override;

	public:
		std::string label;

		Maths::Color idleBackgroundColor;
		Maths::Color hoveredBackgroundColor;
		Maths::Color clickedBackgroundColor;

		Maths::Color textColor;
	};
}
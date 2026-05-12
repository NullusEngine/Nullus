#pragma once

#include <string>

#include "UI/Widgets/Buttons/AButton.h"

enum ImGuiDir : int;

namespace NLS::UI::Widgets
{
	/**
	* Button widget with an arrow image
	*/
	class NLS_UI_API ButtonArrow : public AButton
	{
	public:
		/**
		* Create the button
		* @param p_direction
		*/
		ButtonArrow();
		ButtonArrow(ImGuiDir p_direction);

	protected:
		void _Draw_Impl() override;

	public:
		ImGuiDir direction;
	};
}

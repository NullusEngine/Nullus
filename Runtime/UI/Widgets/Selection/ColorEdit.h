#pragma once

#include <Eventing/Event.h>

#include "UI/Widgets/DataWidget.h"
#include "Color.h"

namespace NLS::UI::Widgets
{
	/**
	* Widget that can open a color picker on click
	*/
	class NLS_UI_API ColorEdit : public DataWidget<Maths::Color>
	{
	public:
		/**
		* Constructor
		* @param p_enableAlpha
		* @param p_defaultColor
		*/
		ColorEdit(bool p_enableAlpha = false, const Maths::Color& p_defaultColor = {});

	protected:
		void _Draw_Impl() override;

	public:
		bool enableAlpha;
		Maths::Color color;
		NLS::Event<Maths::Color&> ColorChangedEvent;
	};
}
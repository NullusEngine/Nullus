#pragma once

#include <Eventing/Event.h>

#include "UI/Widgets/DataWidget.h"
#include "UI/Types/Color.h"

namespace NLS::UI::Widgets::Selection
{
	/**
	* Widget that can open a color picker on click
	*/
	class ColorEdit : public DataWidget<Types::Color>
	{
	public:
		/**
		* Constructor
		* @param p_enableAlpha
		* @param p_defaultColor
		*/
		ColorEdit(bool p_enableAlpha = false, const Types::Color& p_defaultColor = {});

	protected:
		void _Draw_Impl() override;

	public:
		bool enableAlpha;
		Types::Color color;
		NLS::Event<Types::Color&> ColorChangedEvent;
	};
}
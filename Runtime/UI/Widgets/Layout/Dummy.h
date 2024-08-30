#pragma once

#include <Math/Vector2.h>

#include "UI/Widgets/AWidget.h"

namespace NLS::UI::Widgets
{
	/**
	* Dummy widget that takes the given size as space in the panel
	*/
	class NLS_UI_API Dummy : public AWidget
	{
	public:
		/**
		* Constructor
		* @param p_size
		*/
		Dummy(const Maths::Vector2& p_size = { 0.0f, 0.0f });

	protected:
		void _Draw_Impl() override;

	public:
		Maths::Vector2 size;
	};
}
#pragma once

#include <Vector2.h>

#include "UI/Widgets/AWidget.h"

namespace NLS::UI::Widgets
{
	/**
	* Simple widget that display a progress bar
	*/
	class ProgressBar : public AWidget
	{
	public:
		/**
		* Constructor
		* @param p_fraction
		* @param p_size
		* @param p_overlay
		*/
		ProgressBar(float p_fraction = 0.0f, const Maths::Vector2& p_size = { 0.0f, 0.0f }, const std::string& p_overlay = "");

	protected:
		void _Draw_Impl() override;

	public:
		float fraction;
		Maths::Vector2 size;
		std::string overlay;
	};
}
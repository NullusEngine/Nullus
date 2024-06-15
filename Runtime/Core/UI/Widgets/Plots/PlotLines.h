#pragma once

#include "UI/Widgets/Plots/APlot.h"

namespace NLS::UI::Widgets::Plots
{
	/**
	* Plot displayed as lines
	*/
	class PlotLines : public APlot
	{
	public:
		/**
		* Constructor
		* @param p_data
		* @param p_minScale
		* @param p_maxScale
		* @param p_size
		* @param p_overlay
		* @param p_label
		* @param p_forceHover
		*/
		PlotLines
		(
			const std::vector<float>& p_data = std::vector<float>(),
			float p_minScale = std::numeric_limits<float>::min(),
			float p_maxScale = std::numeric_limits<float>::max(),
			const Maths::Vector2& p_size = { 0.0f, 0.0f },
			const std::string& p_overlay = "",
			const std::string& p_label = ""
		);

	protected:
		void _Draw_Impl() override;
	};
}
#pragma once

#include "UI/Widgets/Drags/DragSingleScalar.h"

namespace NLS::UI::Widgets::Drags
{
	/**
	* Drag widget of type int
	*/
	class NLS_CORE_API DragInt : public DragSingleScalar<int>
	{
	public:
		/**
		* Constructor
		* @param p_min
		* @param p_max
		* @param p_value
		* @param p_speed
		* @param p_label
		* @param p_format
		*/
		DragInt
		(
			int p_min = 0,
			int p_max = 100,
			int p_value = 50,
			float p_speed = 1.0f,
			const std::string& p_label = "",
			const std::string& p_format = "%d"
		);
	};
}
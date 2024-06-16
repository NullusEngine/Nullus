#pragma once

#include "UI/Widgets/InputFields/InputMultipleScalars.h"

namespace NLS::UI::Widgets::InputFields
{
	/**
	* Input widget of multiple type int
	*/
	template <size_t _Size>
	class NLS_CORE_API InputMultipleInts : public InputMultipleScalars<int, _Size>
	{
	public:
		/**
		* Constructor
		* @param p_defaultValue
		* @param p_step
		* @param p_fastStep
		* @param p_label
		* @param p_format
		* @param p_selectAllOnClick
		*/
		InputMultipleInts
		(
			int p_defaultValue = 0,
			int p_step = 1,
			int p_fastStep = 0,
			const std::string& p_label = "",
			const std::string& p_format = "%d",
			bool p_selectAllOnClick = true
		) : InputMultipleScalars<int, _Size>(ImGuiDataType_::ImGuiDataType_S32, p_defaultValue, p_step, p_fastStep, p_label, p_format, p_selectAllOnClick) {}
	};
}
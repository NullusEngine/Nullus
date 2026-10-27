#pragma once

#include <Eventing/Event.h>

#include "UI/Widgets/DataWidget.h"
#include "ImGui/imgui.h"
#include "UI/Controls/ScalarControl.h"

namespace NLS::UI::Widgets
{
	/**
	* Input widget of generic type
	*/
	template <typename T>
	class NLS_UI_API InputSingleScalar : public DataWidget<T>
	{
		static_assert(std::is_scalar<T>::value, "Invalid InputSingleScalar T (Scalar expected)");

	public:
		/**
		* Constructor
		* @param p_dataType
		* @param p_defaultValue
		* @param p_step
		* @param p_fastStep
		* @param p_label
		* @param p_format
		* @param p_selectAllOnClick
		*/
		InputSingleScalar
		(
			ImGuiDataType p_dataType,
			T p_defaultValue,
			T p_step,
			T p_fastStep,
			const std::string& p_label,
			const std::string& p_format,
			bool p_selectAllOnClick
		) : DataWidget<T>(value), m_dataType(p_dataType), value(p_defaultValue), step(p_step), fastStep(p_fastStep), label(p_label), format(p_format), selectAllOnClick(p_selectAllOnClick) {}

	protected:
		void _Draw_Impl() override
		{
			T previousValue = value;

			ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
			if (selectAllOnClick)
				flags |= ImGuiInputTextFlags_AutoSelectAll;

			Controls::ScalarOptions options;options.step=step!=0?&step:nullptr;options.fastStep=fastStep!=0?&fastStep:nullptr;options.format=format.c_str();options.textFlags=flags;
            const auto result=Controls::Scalar((label+this->m_widgetID).c_str(),m_dataType,&value,options);
            const bool enterPressed=result.completed && ImGui::IsKeyPressed(ImGuiKey_Enter);

			if (previousValue != value)
			{
				ContentChangedEvent.Invoke(value);
				this->NotifyChange();
			}

			if (enterPressed)
				EnterPressedEvent.Invoke(value);
		}

	public:
		T value;
		T step;
		T fastStep;
		std::string label;
		std::string format;
		bool selectAllOnClick;
		NLS::Event<T> ContentChangedEvent;
		NLS::Event<T> EnterPressedEvent;

	private:
		ImGuiDataType m_dataType;
	};
}

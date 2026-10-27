#pragma once

#include <Vector2.h>
#include <Eventing/Event.h>

#include "UI/Widgets/DataWidget.h"
#include "UI/Widgets/Sliders/ESliderOrientation.h"
#include "UI/Internal/Converter.h"
#include "ImGui/imgui.h"
#include "UI/Controls/ScalarControl.h"

namespace NLS::UI::Widgets
{
	/**
	* Slider widget of generic type
	*/
	template <typename T>
	class SliderSingleScalar : public DataWidget<T>
	{
		static_assert(std::is_scalar<T>::value, "Invalid SliderSingleScalar T (Scalar expected)");

	public:
		/**
		* Constructor
		* @param p_dataType
		* @param p_min
		* @param p_max
		* @param p_value
		* @param p_orientation
		* @param p_label
		* @param p_format
		*/
		SliderSingleScalar
		(
			ImGuiDataType p_dataType,
			T p_min,
			T p_max,
			T p_value,
			ESliderOrientation p_orientation,
			const std::string& p_label,
			const std::string& p_format
		) : DataWidget<T>(value), m_dataType(p_dataType), min(p_min), max(p_max), value(p_value), orientation(p_orientation), label(p_label), format(p_format) {}

	protected:
		void _Draw_Impl() override
		{
			if (max < min)
				max = min;

			if (value < min)
				value = min;
			else if (value > max)
				value = max;

			bool valueChanged = false;

            Controls::ScalarOptions options;options.slider=true;options.minimum=&min;options.maximum=&max;options.format=format.c_str();
            options.vertical=orientation==ESliderOrientation::VERTICAL;options.size=Internal::Converter::ToImVec2(verticalModeSize);
            valueChanged=Controls::Scalar((label+this->m_widgetID).c_str(),m_dataType,&value,options).changed;

			if (valueChanged)
			{
				ValueChangedEvent.Invoke(value);
				this->NotifyChange();
			}
		}

	public:
		T min;
		T max;
		T value;
		ESliderOrientation orientation;
		Maths::Vector2 verticalModeSize; /* Only applied with ESliderOrientation::VERTICAL */
		std::string label;
		std::string format;
		Event<T> ValueChangedEvent;

	private:
		ImGuiDataType m_dataType;
	};
}

#pragma once

#include <array>
#include <algorithm>

#include <Eventing/Event.h>

#include "UI/Internal/Converter.h"
#include "UI/Widgets/DataWidget.h"

namespace NLS::UI::Widgets
{
	/**
	* Drag widget of multiple generic type
	*/
	template <typename T, size_t _Size>
	class NLS_UI_API DragMultipleScalars : public DataWidget<std::array<T, _Size>>
	{
		static_assert(_Size > 1, "Invalid DragMultipleScalars _Size (2 or more requiered)");

	public:
		/**
		* Constructor
		* @param p_dataType
		* @param p_min
		* @param p_max
		* @param p_value
		* @param p_speed
		* @param p_label
		* @param p_format
		*/
		DragMultipleScalars
		(
			ImGuiDataType_ p_dataType,
			T p_min,
			T p_max,
			T p_value,
			float p_speed,
			const std::string& p_label,
			const std::string& p_format
		) : DataWidget<std::array<T, _Size>>(values), m_dataType(p_dataType), min(p_min), max(p_max), speed(p_speed), label(p_label), format(p_format)
		{
			values.fill(p_value);
			componentLabels.fill("");
			componentColors.fill(Maths::Color::White);
		}

	protected:
		void _Draw_Impl() override
		{
			if (max < min)
				max = min;

			for (size_t i = 0; i < _Size; ++i)
			{
				if (values[i] < min)
					values[i] = min;
				else if (values[i] > max)
					values[i] = max;
			}

			bool valueChanged = false;

			if (axisStyle)
			{
				const float fullWidth = ImGui::GetContentRegionAvail().x;
				const float totalSpacing = (_Size > 0 ? static_cast<float>(_Size - 1) * 4.0f : 0.0f);
				const float labelWidth = 14.0f;
				const float perItemWidth = std::max(24.0f, (fullWidth - totalSpacing - static_cast<float>(_Size) * labelWidth) / static_cast<float>(_Size));
				const float frameHeight = ImGui::GetFrameHeight();

				for (size_t i = 0; i < _Size; ++i)
				{
					ImGui::PushID(static_cast<int>(i));

					const ImVec4 axisColor = UI::Internal::Converter::ToImVec4(componentColors[i]);
					ImVec2 barMin = ImGui::GetCursorScreenPos();
					ImVec2 barMax = ImVec2(barMin.x + labelWidth, barMin.y + frameHeight);
					ImGui::GetWindowDrawList()->AddRectFilled(barMin, barMax, ImGui::GetColorU32(axisColor), 3.0f);
					ImGui::GetWindowDrawList()->AddText(
						ImVec2(barMin.x + 3.0f, barMin.y + 2.0f),
						IM_COL32(243, 246, 250, 255),
						componentLabels[i].c_str());
					ImGui::Dummy(ImVec2(labelWidth, frameHeight));

					ImGui::SameLine(0.0f, 0.0f);
					ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));
					ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
					ImGui::PushItemWidth(perItemWidth);

					if (ImGui::DragScalar(("##Axis" + this->m_widgetID).c_str(), m_dataType, &values[i], speed, &min, &max, format.c_str()))
						valueChanged = true;

					ImGui::PopItemWidth();
					ImGui::PopStyleVar(2);

					if (i + 1 < _Size)
						ImGui::SameLine(0.0f, 4.0f);

					ImGui::PopID();
				}
			}
			else if (ImGui::DragScalarN((label + this->m_widgetID).c_str(), m_dataType, values.data(), _Size, speed, &min, &max, format.c_str()))
			{
				valueChanged = true;
			}

			if (valueChanged)
			{
				ValueChangedEvent.Invoke(values);
				this->NotifyChange();
			}
		}

	public:
		T min;
		T max;
		float speed;
		std::array<T, _Size> values;
		std::array<std::string, _Size> componentLabels;
		std::array<Maths::Color, _Size> componentColors;
		std::string label;
		std::string format;
		bool axisStyle = false;
		NLS::Event<std::array<T, _Size>&> ValueChangedEvent;

	protected:
		ImGuiDataType_ m_dataType;
	};
}

#pragma once

#include <Eventing/Event.h>

#include "Core/ServiceLocator.h"
#include "UI/UIManager.h"
#include "UI/Widgets/DataWidget.h"
#include "UI/Widgets/Drags/DragScalarInteraction.h"
#include "ImGui/imgui.h"

namespace NLS::UI::Widgets
{
	/**
	* Drag widget of generic type
	*/
	template <typename T>
	class NLS_UI_API DragSingleScalar : public DataWidget<T>
	{
		static_assert(std::is_scalar<T>::value, "Invalid DragSingleScalar T (Scalar expected)");

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
		DragSingleScalar
		(
			ImGuiDataType p_dataType,
			T p_min,
			T p_max,
			T p_value,
			float p_speed,
			const std::string& p_label,
			const std::string& p_format
		) : DataWidget<T>(value), min(p_min), max(p_max), value(p_value), speed(p_speed), label(p_label), format(p_format), enableClickToInput(true), m_dataType(p_dataType) {}

	protected:
		void _Draw_Impl() override
		{
			if (max < min)
				max = min;

			if (value < min)
				value = min;
			else if (value > max)
				value = max;

			const float uiScale = NLS::Core::ServiceLocator::Contains<UIManager>()
				? NLS_SERVICE(UIManager).GetScale()
				: 1.0f;
            const ImGuiSliderFlags dragFlags = enableClickToInput ? static_cast<ImGuiSliderFlags>(0) : ImGuiSliderFlags_NoInput;

			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * uiScale, 4.0f * uiScale));
			ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * uiScale);

			if (ImGui::DragScalar((label + this->m_widgetID).c_str(), m_dataType, &value, speed, &min, &max, format.c_str(), dragFlags))
			{
				ValueChangedEvent.Invoke(value);
				this->NotifyChange();
            }
            const ImGuiID itemId = ImGui::GetItemID();
            if (ImGui::IsItemActive() &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
                !DragScalarInternal::IsDragScalarTextInputActive(itemId) &&
                NLS::Core::ServiceLocator::Contains<UIManager>())
                NLS_SERVICE(UIManager).RequestInfiniteDragCursor(NLS::Cursor::ECursorShape::SLIDE_ARROW);

			ImGui::PopStyleVar(2);
		}

	public:
		T min;
		T max;
		T value;
		float speed;
		std::string label;
		std::string format;
        bool enableClickToInput;
		NLS::Event<T> ValueChangedEvent;

	private:
		ImGuiDataType m_dataType;
	};
}

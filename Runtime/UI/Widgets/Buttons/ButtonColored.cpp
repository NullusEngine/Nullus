#include "UI/Widgets/Buttons/ButtonColored.h"
#include "UI/Internal/Converter.h"

namespace NLS::UI::Widgets
{
	ButtonColored::ButtonColored(const std::string& p_label, const Maths::Color& p_color, const Maths::Vector2& p_size, bool p_enableAlpha) :
		label(p_label), color(p_color), size(p_size), enableAlpha(p_enableAlpha)
	{
	}

	void ButtonColored::_Draw_Impl()
	{
		ImVec4 imColor = Internal::Converter::ToImVec4(color);

		if (ImGui::ColorButton((label + m_widgetID).c_str(), imColor, !enableAlpha ? ImGuiColorEditFlags_NoAlpha : 0, Internal::Converter::ToImVec2(size)))
			ClickedEvent.Invoke();

		color = Internal::Converter::ToColor(imColor);
	}
}


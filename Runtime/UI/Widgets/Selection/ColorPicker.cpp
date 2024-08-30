#include "UI/Widgets/Selection/ColorPicker.h"

namespace NLS::UI::Widgets
{
ColorPicker::ColorPicker(bool p_enableAlpha, const Maths::Color& p_defaultColor)
    : DataWidget<Maths::Color>(color), enableAlpha(p_enableAlpha), color(p_defaultColor)
{
}

void ColorPicker::_Draw_Impl()
{
    int flags = !enableAlpha ? ImGuiColorEditFlags_NoAlpha : 0;
    bool valueChanged = false;

    if (enableAlpha)
        valueChanged = ImGui::ColorPicker4(m_widgetID.c_str(), &color.r, flags);
    else
        valueChanged = ImGui::ColorPicker3(m_widgetID.c_str(), &color.r, flags);

    if (valueChanged)
    {
        ColorChangedEvent.Invoke(color);
        this->NotifyChange();
    }
}
} // namespace NLS::UI::Widgets

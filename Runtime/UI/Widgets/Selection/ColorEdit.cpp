
#include "UI/Widgets/Selection/ColorEdit.h"

namespace NLS::UI::Widgets
{
ColorEdit::ColorEdit(bool p_enableAlpha, const Maths::Color& p_defaultColor)
    : DataWidget<Maths::Color>(color), enableAlpha(p_enableAlpha), color(p_defaultColor)
{
}

void ColorEdit::_Draw_Impl()
{
    int flags = !enableAlpha ? ImGuiColorEditFlags_NoAlpha : 0;
    bool valueChanged = false;

    if (enableAlpha)
        valueChanged = ImGui::ColorEdit4(m_widgetID.c_str(), &color.r, flags);
    else
        valueChanged = ImGui::ColorEdit3(m_widgetID.c_str(), &color.r, flags);

    if (valueChanged)
    {
        ColorChangedEvent.Invoke(color);
        this->NotifyChange();
    }
}
} // namespace NLS::UI::Widgets
